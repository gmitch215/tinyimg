/**
 * Runs every benchmark arm and writes the report.
 *
 * ```sh
 * bun bench/run.ts
 * bun bench/run.ts --runs=21 --warmup=5
 * bun bench/run.ts --markdown=dist/bench.md --json=dist/bench.json
 * bun bench/run.ts --only=planner
 * ```
 *
 * Measures `bin/tinyimg.wasm`, which is the LTO build that ships. Measuring the ctest library
 * instead overstated PNG decode 3x and text drawing 4.6x, because the bit reader and the math shims
 * live in different translation units from their callers and only LTO inlines them; the two
 * mistakes cost real time, so this reads the linked module and nothing else.
 *
 * The SIMD arm needs a second module and skips when there is none:
 *
 * ```sh
 * cmake -S . -B build-nosimd -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32.cmake -DTINYIMG_SIMD=OFF \
 *   -DDOCS_TINYIMG=OFF -DTINYIMG_BIN_DIR=bench/arms && cmake --build build-nosimd
 * ```
 */

import { existsSync, mkdirSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { codecArm, loadModule, operationArm, plannerArm, simdArm } from './arms.js';
import { DEFAULT_BUDGET, type Budget, type Timing } from './harness.js';
import { renderMarkdown, type Report, type Section } from './report.js';

const ROOT = join(import.meta.dirname, '..');
const MODULE = join(ROOT, 'bin', 'tinyimg.wasm');
const NO_SIMD = join(ROOT, 'bench', 'arms', 'tinyimg.wasm');
const RESULTS = join(ROOT, 'bench', 'results', 'latest.md');

function flag(name: string): string | undefined {
	const found = process.argv.find((argument) => argument.startsWith(`--${name}=`));
	return found?.slice(name.length + 3);
}

/** The most recent mtime across the C sources both arms are built from. */
function newestSource(): number {
	let newest = 0;

	for (const directory of [join(ROOT, 'src'), join(ROOT, 'include')]) {
		for (const entry of readdirSync(directory, { recursive: true, withFileTypes: true })) {
			if (!entry.isFile()) continue;

			const at = statSync(join(entry.parentPath, entry.name)).mtimeMs;
			if (at > newest) newest = at;
		}
	}

	return newest;
}

function write(path: string, contents: string): void {
	mkdirSync(dirname(path), { recursive: true });
	writeFileSync(path, contents.endsWith('\n') ? contents : `${contents}\n`);
}

async function main(): Promise<void> {
	if (!existsSync(MODULE)) {
		console.error(`no module at ${MODULE}; run "bun run build:wasm" first`);
		process.exitCode = 1;
		return;
	}

	const budget: Partial<Budget> = {
		warmup: Number(flag('warmup') ?? DEFAULT_BUDGET.warmup),
		runs: Number(flag('runs') ?? DEFAULT_BUDGET.runs)
	};

	const only = flag('only');
	const wanted = (name: string) => only === undefined || only === name;

	const tinyimg = await loadModule(MODULE);
	if (!tinyimg) {
		console.error(`could not load ${MODULE}`);
		process.exitCode = 1;
		return;
	}

	const sections: Section[] = [];

	if (wanted('codecs')) {
		console.error('bench: codecs');
		sections.push({
			title: 'Codecs',
			blurb:
				'Against `@jsquash`, which is libjpeg, libpng and libwebp built with emscripten and ' +
				'running in the same runtime. The ratio is `@jsquash` over tinyimg, so above 1 means ' +
				'tinyimg is faster.\n\n' +
				'**Read the byte counts with the times.** A lossless encoder trades one against the ' +
				'other, and these two are not at the same operating point: tinyimg is slower at PNG ' +
				'and its output is 47% smaller, so neither number alone says which is better.',
			timings: await codecArm(tinyimg, budget)
		});
	}

	if (wanted('operations')) {
		console.error('bench: operations');
		sections.push({
			title: 'Operations',
			blurb:
				'Each transformation alone over `sf-24.jpg` at 1835x1032, timed as a decode and a ' +
				'plan run with no encoder in the way. The first row is the decode by itself, so ' +
				"every other row less that one is the operation's own cost. `ms/Mpx` is against " +
				'the source extent.',
			timings: await operationArm(tinyimg, budget)
		});
	}

	if (wanted('planner')) {
		console.error('bench: planner');
		sections.push({
			title: 'The planner',
			blurb:
				'A 500x500 crop of a 1.9 megapixel photograph down to 100x100 with four color ' +
				'operations after it. `planner off` runs the same chain one operation per pass, ' +
				'which is what the library would cost without it. The ratio is off over on.',
			timings: await plannerArm(tinyimg, budget)
		});
	}

	if (wanted('simd')) {
		// the arm is gitignored, so it outlives the sources it was built from, and a stale one
		// reports every unrelated codec change as a SIMD win
		if (existsSync(NO_SIMD) && statSync(NO_SIMD).mtimeMs < newestSource()) {
			console.error(
				`bench: simd skipped, ${NO_SIMD} predates the current sources; rebuild it`
			);
			process.exitCode = 1;
			return;
		}

		const simdless = await loadModule(NO_SIMD);

		if (simdless) {
			console.error('bench: simd');

			const on = await simdArm(tinyimg, budget);
			const off = await simdArm(simdless, budget);

			// the SIMD arm measures the same operations through two modules, so the on side has to
			// be relabeled to sit beside the off side in one table
			const paired: Timing[] = [
				...on.map((timing) => ({ ...timing, arm: 'simd on' })),
				...off
			];

			sections.push({
				title: 'SIMD',
				blurb:
					'The same operations through a module built without `-msimd128`. The ratio is ' +
					'off over on, so above 1 means SIMD helped.',
				timings: paired
			});
		} else {
			console.error(`bench: simd skipped, no module at ${NO_SIMD}`);
		}
	}

	const report: Report = {
		at: new Date().toISOString(),
		platform: `${process.platform}-${process.arch}`,
		version: tinyimg.versionText,
		simd: tinyimg.has('simd'),
		sections
	};

	const markdown = renderMarkdown(report);
	console.log(markdown);

	write(RESULTS, markdown);

	const markdownPath = flag('markdown');
	if (markdownPath) write(markdownPath, markdown);

	const jsonPath = flag('json');
	if (jsonPath) write(jsonPath, JSON.stringify(report, null, 2));
}

await main();
