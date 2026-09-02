/**
 * Measures the linked wasm module against its budget and reports where the bytes are.
 *
 * ```sh
 * bun scripts/size-report.ts
 * bun scripts/size-report.ts --json=dist/size.json --markdown=dist/size.md
 * bun scripts/size-report.ts --ablate
 * ```
 *
 * Three views, in decreasing order of how much they can be trusted:
 *
 * 1. **Totals.** The linked module, raw and under both compressors. Cloudflare applies its own gzip
 *    before measuring against the plan ceiling, so the gzip column is the one that binds.
 * 2. **Sections.** Read out of the module itself with `wasm-objdump -h`, so code, data and the type
 *    and export tables are separated exactly.
 * 3. **Objects.** Per translation unit, read from the build directory. These are pre-link sizes, so
 *    they over-count: `--gc-sections` and LTO have not run yet. Useful as a relative signal for which
 *    file is growing, never as a figure to publish.
 *
 * `--ablate` relinks the module once per entry in {@link ABLATIONS} and reports the difference, which
 * is the only one of the four that yields a true marginal cost. It is slow, so it is opt-in.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { brotliCompressSync, gzipSync } from 'node:zlib';

const ROOT = join(import.meta.dirname, '..');
const WASM = join(ROOT, 'bin', 'tinyimg.wasm');
const BUILD = join(ROOT, 'build');

/** The plan's target and the ceiling it must never cross, both after gzip. */
export const TARGET_BYTES = 150 * 1024;
export const LIMIT_BYTES = 200 * 1024;

/**
 * Relink arms, each one cmake flags that remove a feature.
 *
 * Filled in as codecs land; an empty table means `--ablate` has nothing to measure and says so
 * rather than reporting an empty table as a clean result.
 */
/**
 * These are marginal, not additive. Removing one arm leaves the code it shared with the others
 * behind, so the numbers do not sum to the module: PNG's arm covers the DEFLATE unit only because
 * nothing else references it yet, and BMP's arm shrank when PNG arrived to share the pixel
 * conversion and region resolution with it.
 */
export const ABLATIONS: { name: string; flags: string[] }[] = [
	{ name: 'png', flags: ['-DCMAKE_C_FLAGS=-DTINYIMG_NO_PNG'] },
	{ name: 'bmp', flags: ['-DCMAKE_C_FLAGS=-DTINYIMG_NO_BMP'] },
	{ name: 'simd', flags: ['-DTINYIMG_SIMD=OFF'] }
];

export interface SizeReport {
	raw: number;
	gzip: number;
	brotli: number;
	target: number;
	limit: number;
	sections: { name: string; bytes: number }[];
	objects: { file: string; bytes: number }[];
	ablations: { name: string; rawDelta: number; gzipDelta: number }[];
	/** Absent when no passing master run was found to compare against. */
	baseline?: { raw: number; gzip: number; brotli: number };
}

function kib(bytes: number): string {
	return `${(bytes / 1024).toFixed(1)} KiB`;
}

function signedKib(bytes: number): string {
	const sign = bytes >= 0 ? '+' : '-';
	return `${sign}${(Math.abs(bytes) / 1024).toFixed(1)} KiB`;
}

/** Reads the section table out of the module, which is the one breakdown that cannot drift. */
function sections(wasm: string): { name: string; bytes: number }[] {
	let out: string;
	try {
		out = execFileSync('wasm-objdump', ['-h', wasm], { encoding: 'utf8' });
	} catch {
		return [];
	}

	const rows: { name: string; bytes: number }[] = [];
	for (const line of out.split('\n')) {
		// e.g. "     Code start=0x0000012f end=0x000004a1 (size=0x00000372) count: 12"
		const match =
			/^\s*(\w+)\s+start=0x[0-9a-f]+\s+end=0x[0-9a-f]+\s+\(size=0x([0-9a-f]+)\)/.exec(line);
		if (match?.[1] && match[2]) rows.push({ name: match[1], bytes: parseInt(match[2], 16) });
	}
	return rows.sort((a, b) => b.bytes - a.bytes);
}

/** Walks the build tree for compiled objects; pre-link, so over-counted on purpose. */
function objects(dir: string): { file: string; bytes: number }[] {
	const rows: { file: string; bytes: number }[] = [];
	if (!existsSync(dir)) return rows;

	const walk = (path: string) => {
		for (const entry of readdirSync(path, { withFileTypes: true })) {
			const full = join(path, entry.name);
			if (entry.isDirectory()) walk(full);
			else if (entry.name.endsWith('.obj') || entry.name.endsWith('.o')) {
				const path = relative(dir, full);
				// cmake's own compiler-identification probe, not part of the module
				if (path.includes('CompilerId')) continue;
				rows.push({
					file: path.replace(/^CMakeFiles\/tinyimg\.dir\//, ''),
					bytes: statSync(full).size
				});
			}
		}
	};

	walk(dir);
	return rows.sort((a, b) => b.bytes - a.bytes);
}

function ablate(baseline: { raw: number; gzip: number }) {
	const rows: { name: string; rawDelta: number; gzipDelta: number }[] = [];

	for (const arm of ABLATIONS) {
		const armDir = join(ROOT, `build-ablate-${arm.name}`);
		execFileSync(
			'cmake',
			[
				'-S',
				ROOT,
				'-B',
				armDir,
				`-DCMAKE_TOOLCHAIN_FILE=${join(ROOT, 'cmake', 'wasm32.cmake')}`,
				'-DDOCS_TINYIMG=OFF',
				// its own output directory, or the arm's module lands in bin/ and everything
				// downstream silently ships a module with a codec missing
				`-DTINYIMG_BIN_DIR=${join(armDir, 'bin')}`,
				...arm.flags
			],
			{ stdio: 'ignore' }
		);
		execFileSync('cmake', ['--build', armDir], { stdio: 'ignore' });

		const armWasm = readFileSync(join(armDir, 'bin', 'tinyimg.wasm'));
		rows.push({
			name: arm.name,
			rawDelta: baseline.raw - armWasm.length,
			gzipDelta: baseline.gzip - gzipSync(armWasm, { level: 9 }).length
		});
	}

	return rows;
}

/** The previous run's document, if one was downloaded. Absent is normal on a new branch. */
function baseline(path: string | undefined): SizeReport['baseline'] {
	if (!path || !existsSync(path)) return undefined;
	try {
		const document = JSON.parse(readFileSync(path, 'utf8')) as SizeReport;
		if (typeof document.gzip !== 'number') return undefined;
		return { raw: document.raw, gzip: document.gzip, brotli: document.brotli };
	} catch {
		return undefined;
	}
}

export function collect(options: { ablate?: boolean; baseline?: string } = {}): SizeReport {
	if (!existsSync(WASM)) {
		throw new Error(`no module at ${WASM}; run "bun run build:wasm" first`);
	}

	const bytes = readFileSync(WASM);
	const raw = bytes.length;
	const gzip = gzipSync(bytes, { level: 9 }).length;
	const brotli = brotliCompressSync(bytes).length;

	return {
		raw,
		gzip,
		brotli,
		target: TARGET_BYTES,
		limit: LIMIT_BYTES,
		sections: sections(WASM),
		objects: objects(BUILD),
		ablations: options.ablate ? ablate({ raw, gzip }) : [],
		...(baseline(options.baseline) ? { baseline: baseline(options.baseline)! } : {})
	};
}

export function render(report: SizeReport): string {
	const lines: string[] = [];
	const pct = ((report.gzip / report.target) * 100).toFixed(1);

	const delta = (now: number, before: number | undefined) => {
		if (before === undefined) return 'no baseline';
		if (now === before) return 'unchanged';
		return `${signedKib(now - before)} (${(((now - before) / before) * 100).toFixed(1)}%)`;
	};

	lines.push('## Size');
	lines.push('');
	lines.push('| Measure | Bytes | Against the target | Since master |');
	lines.push('| --- | ---: | ---: | ---: |');
	lines.push(
		`| raw | ${report.raw.toLocaleString()} | | ${delta(report.raw, report.baseline?.raw)} |`
	);
	lines.push(
		`| **gzip** | **${report.gzip.toLocaleString()}** | ${pct}% of ${kib(report.target)} | ` +
			`${delta(report.gzip, report.baseline?.gzip)} |`
	);
	lines.push(
		`| brotli | ${report.brotli.toLocaleString()} | | ${delta(report.brotli, report.baseline?.brotli)} |`
	);
	lines.push('');

	if (report.gzip > report.limit) {
		lines.push(`**Over the hard limit.** ${kib(report.gzip)} against ${kib(report.limit)}.`);
	} else if (report.gzip > report.target) {
		lines.push(`**Over the target.** ${kib(report.gzip)} against ${kib(report.target)}.`);
	} else {
		lines.push(`Within the ${kib(report.target)} target.`);
	}
	lines.push('');

	if (report.sections.length > 0) {
		lines.push('### Sections');
		lines.push('');
		lines.push('| Section | Bytes |');
		lines.push('| --- | ---: |');
		for (const section of report.sections) {
			lines.push(`| ${section.name} | ${section.bytes.toLocaleString()} |`);
		}
		lines.push('');
	}

	if (report.objects.length > 0) {
		lines.push('### Objects, pre-link');
		lines.push('');
		lines.push('Before `--gc-sections` and LTO, so these over-count. A relative signal only.');
		lines.push('');
		lines.push('| Object | Bytes |');
		lines.push('| --- | ---: |');
		for (const object of report.objects) {
			lines.push(`| \`${object.file}\` | ${object.bytes.toLocaleString()} |`);
		}
		lines.push('');
	}

	if (report.ablations.length > 0) {
		lines.push('### Marginal cost by feature');
		lines.push('');
		lines.push('| Feature | raw | gzip |');
		lines.push('| --- | ---: | ---: |');
		for (const arm of report.ablations) {
			lines.push(
				`| ${arm.name} | ${signedKib(arm.rawDelta)} | ${signedKib(arm.gzipDelta)} |`
			);
		}
		lines.push('');
	}

	return lines.join('\n');
}

function flag(name: string): string | undefined {
	const found = process.argv.find((argument) => argument.startsWith(`--${name}=`));
	return found?.slice(name.length + 3);
}

if (import.meta.main) {
	const wantsAblation = process.argv.includes('--ablate');
	if (wantsAblation && ABLATIONS.length === 0) {
		console.error('no ablation arms are defined yet; nothing to measure');
	}

	const report = collect({
		ablate: wantsAblation && ABLATIONS.length > 0,
		...(flag('baseline') ? { baseline: flag('baseline')! } : {})
	});
	const markdown = render(report);

	console.log(markdown);

	const jsonPath = flag('json');
	if (jsonPath) {
		mkdirSync(dirname(jsonPath), { recursive: true });
		writeFileSync(jsonPath, `${JSON.stringify(report, null, 2)}\n`);
	}

	const markdownPath = flag('markdown');
	if (markdownPath) {
		mkdirSync(dirname(markdownPath), { recursive: true });
		writeFileSync(markdownPath, `${markdown}\n`);
	}

	// a module over the hard limit cannot be shipped, and the workflow reports rather than gates,
	// so this only ever surfaces locally
	if (report.gzip > report.limit) process.exitCode = 1;
}
