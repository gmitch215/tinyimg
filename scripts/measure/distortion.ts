/**
 * Measures what each degrade lever costs in quality and what each output format costs in bytes.
 *
 * ```sh
 * bun scripts/measure/distortion.ts lever  [--corpus fixtures|cal] [--build-dir build-native]
 * bun scripts/measure/distortion.ts encode [--corpus fixtures|cal] [--build-dir build-native] [--time]
 * bun scripts/measure/distortion.ts bytes-check [--build-dir build-native]
 * bun scripts/measure/distortion.ts --check-test [--build-dir build-native]
 * bun scripts/distortion-table.ts            # then turn the cells into the table
 * ```
 *
 * A periodic eval, not a gate test. `lever` runs every candidate of `tiny_plan_enumerate` at widths
 * 200, 400 and 800 and records the PSNR of each against candidate 0, which is the plan's own effort
 * and filter at the scale the planner picks undegraded. `encode` takes candidate 0 at 400 px, encodes
 * it with tinyimg at eight qualities and decodes the result with an external decoder (`djpeg`,
 * `dwebp`, `avifdec` or `magick`), never with tinyimg's own, so the PSNR is against the raw pixels
 * and an encoder cannot agree with its own decoder by luck.
 *
 * The probe is `distortion-probe.c` compiled against the host static library. Output bytes do not
 * depend on the optimization level (the library builds with `-ffp-contract=off`), so the cells hold
 * for the shipped module; `bytes-check` is what shows it. Encode times are the native `-O2` build
 * and are not the cost model's input.
 *
 * The `test` split is read only by `--check-test`, which scores the committed table's p05 against it.
 */

import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, rmdirSync, rmSync, writeFileSync } from 'node:fs';
import { cpus, loadavg } from 'node:os';
import { basename, join } from 'node:path';
import {
	BASELINES,
	IDENTICAL_DB,
	percentile,
	type EncodeCell,
	type LeverCell,
	type Table
} from '../distortion-table.js';

const ROOT = join(import.meta.dirname, '..', '..');
const WIDTHS = [200, 400, 800];
const ENCODE_WIDTH = 400;
const REPEAT = 5;
const LOCK = '/tmp/tinyimg-timing.lock';
const FIXTURES = [
	'ball_kick.gif',
	'budapest.jpg',
	'dartmouth.jpg',
	'dartmouth.tiff',
	'digicam.jpg',
	'dog.jpg',
	'drupflare.png',
	'face_art.jpg',
	'family.jpg',
	'flower.jpg',
	'forest.png',
	'lake_sunapee.jpeg',
	'lil_bit.png',
	'long_walk.jpg',
	'man.jpg',
	'moped.jpg',
	'mountains.jpg',
	'mushroom.jpg',
	'road.jpg',
	'sf-24.jpg',
	'smile.jpg',
	'steam.png',
	'toyota_racing.webp',
	'webassembly.png',
	'winter_cabin.jpg',
	'winter_forest.png',
	'woman.jpg'
];

type Source = { id: string; path: string };

type Entry = {
	id: string;
	format: string;
	split: 'cal' | 'test';
	source?: string;
	path?: string;
};

const args = process.argv.slice(2);

function flag(name: string, fallback: string): string {
	const i = args.indexOf(name);
	return i >= 0 && args[i + 1] ? args[i + 1]! : fallback;
}

const buildDir = join(ROOT, flag('--build-dir', 'build-native'));
const probe = join(buildDir, 'distortion-probe');

function buildProbe(): void {
	const lib = join(buildDir, 'lib', 'libtinyimg.a');
	if (!existsSync(lib)) throw new Error(`${lib} is missing; build the static library first`);

	execFileSync(
		'clang',
		[
			'-O2',
			'-I',
			join(ROOT, 'include'),
			join(ROOT, 'scripts', 'measure', 'distortion-probe.c'),
			lib,
			'-lm',
			'-o',
			probe
		],
		{ stdio: 'inherit' }
	);
}

function fixtureSources(): Source[] {
	const dir = join(ROOT, 'tests', 'fixtures');

	return FIXTURES.map((f) => {
		const path = join(dir, f);
		if (!existsSync(path)) throw new Error(`pinned fixture ${f} is missing`);
		return { id: f, path };
	});
}

function manifestSources(split: 'cal' | 'test'): Source[] {
	const manifest = join(ROOT, 'scripts', 'corpus-manifest.json');
	if (!existsSync(manifest)) return [];

	const check = spawnSync(process.execPath, [join(ROOT, 'scripts', 'corpus.ts'), 'check'], {
		cwd: ROOT,
		encoding: 'utf8'
	});
	if (check.status !== 0) {
		console.error('corpus check failed, so the corpus is not used:');
		console.error((check.stdout + check.stderr).split('\n').slice(-6).join('\n'));
		return [];
	}

	const { entries } = JSON.parse(readFileSync(manifest, 'utf8')) as { entries: Entry[] };

	return entries
		.filter((e) => e.split === split)
		.map((e) => {
			const ext = e.format === 'jpeg' ? 'jpg' : e.format === 'tiff' ? 'tif' : e.format;
			const path =
				e.source === 'local'
					? join(ROOT, e.path ?? '')
					: join(ROOT, 'corpus', `${e.id}.${ext}`);
			return { id: basename(path), path };
		});
}

function sources(corpus: string): Source[] {
	if (corpus === 'fixtures') return fixtureSources();
	if (corpus === 'cal' || corpus === 'test') return manifestSources(corpus);
	throw new Error(`unknown corpus ${corpus}`);
}

function lines<T>(output: string): T[] {
	return output
		.split('\n')
		.filter((l) => l.startsWith('{'))
		.map((l) => JSON.parse(l) as T);
}

function leverCells(files: Source[]): LeverCell[] {
	const cells: LeverCell[] = [];

	for (const width of WIDTHS) {
		for (const f of files) {
			const run = spawnSync(probe, ['lever', String(width), f.path], {
				encoding: 'utf8',
				maxBuffer: 1 << 26
			});
			if (run.status !== 0) {
				console.error(`lever: ${f.id} at ${width} failed: ${run.stderr.trim()}`);
				continue;
			}
			cells.push(...lines<LeverCell>(run.stdout));
		}
	}

	return cells;
}

function readRaw(file: string, format: string, channels: number): Buffer {
	const layout = ['gray', 'graya', 'rgb', 'rgba'][channels - 1]!;
	const out = (input: string) =>
		execFileSync('magick', [input, '-depth', '8', `${layout}:-`], {
			maxBuffer: 1 << 28
		});
	const scratch = `${file}.dec`;

	if (format === 'jpeg') {
		execFileSync('djpeg', ['-outfile', `${scratch}.pnm`, file]);
		return out(`${scratch}.pnm`);
	}
	if (format === 'webp') {
		execFileSync('dwebp', ['-quiet', '-pam', file, '-o', `${scratch}.pam`]);
		return out(`${scratch}.pam`);
	}
	if (format === 'avif') {
		execFileSync('avifdec', [file, `${scratch}.png`], { stdio: 'ignore' });
		return out(`${scratch}.png`);
	}
	return out(file);
}

/** PSNR over every compared sample; identical buffers are 99 dB. */
function psnr(a: Buffer, b: Buffer): number {
	let sq = 0;
	for (let i = 0; i < a.length; i++) {
		const d = a[i]! - b[i]!;
		sq += d * d;
	}
	if (sq === 0) return IDENTICAL_DB;
	return Math.min(IDENTICAL_DB, 10 * Math.log10((255 * 255 * a.length) / sq));
}

/** PSNR with color premultiplied by the reference alpha, so RGB under alpha 0 counts for nothing. */
function psnrPremultiplied(
	ref: Buffer,
	got: Buffer,
	refChannels: number,
	gotChannels: number
): number {
	const pixels = ref.length / refChannels;
	const gotAlpha = gotChannels === refChannels;
	let sq = 0;

	for (let p = 0; p < pixels; p++) {
		const a = ref[p * refChannels + refChannels - 1]! / 255;
		for (let c = 0; c < refChannels; c++) {
			const last = c === refChannels - 1;
			const r = ref[p * refChannels + c]! * (last ? 1 : a);
			const g =
				c < gotChannels
					? got[p * gotChannels + c]! * (last && gotAlpha ? 1 : last ? 255 : a)
					: 0;
			const d = last && !gotAlpha ? 0 : r - g;
			sq += d * d;
		}
	}
	if (sq === 0) return IDENTICAL_DB;
	return Math.min(IDENTICAL_DB, 10 * Math.log10((255 * 255 * ref.length) / sq));
}

type EncodeLine = {
	image: string;
	width: number;
	channels: number;
	format: string;
	quality: number;
	bytes: number;
	bpp: number;
	file: string;
	time_us: number | null;
	error?: number;
};

function encodeCells(files: Source[], timed: boolean): EncodeCell[] {
	const work = join(buildDir, 'distortion-work');
	mkdirSync(work, { recursive: true });
	const cells: EncodeCell[] = [];

	for (const f of files) {
		const run = spawnSync(
			probe,
			['encode', String(ENCODE_WIDTH), String(timed ? REPEAT : 1), work, f.path],
			{ encoding: 'utf8', maxBuffer: 1 << 26 }
		);
		if (run.status !== 0) {
			console.error(`encode: ${f.id} failed: ${run.stderr.trim()}`);
			continue;
		}

		const ref = readFileSync(join(work, `${f.id}.ref`));
		for (const line of lines<EncodeLine>(run.stdout)) {
			if (line.error !== undefined) {
				cells.push({
					image: f.id,
					format: line.format,
					quality: line.quality,
					channels: line.channels ?? 0,
					bytes: 0,
					bpp: 0,
					db: null,
					time_us: null,
					error: line.error
				});
				continue;
			}

			// jpeg has no alpha, so it is compared on the color channels alone
			const dropAlpha =
				line.format === 'jpeg' && (line.channels === 2 || line.channels === 4);
			const channels = dropAlpha ? line.channels - 1 : line.channels;
			let expected: Buffer = ref;
			if (dropAlpha) {
				const keep: number[] = [];
				for (let i = 0; i < ref.length; i++)
					if (i % line.channels !== line.channels - 1) keep.push(ref[i]!);
				expected = Buffer.from(keep);
			}

			let db: number | null = null;
			let dbPm: number | null = null;
			try {
				const got = readRaw(line.file, line.format, channels);
				db = got.length === expected.length ? psnr(expected, got) : null;
				if (db !== null) {
					dbPm =
						line.channels === 2 || line.channels === 4
							? psnrPremultiplied(ref, got, line.channels, channels)
							: db;
				}
				if (db === null)
					console.error(`${line.file}: decoded ${got.length}, wanted ${expected.length}`);
			} catch (e) {
				console.error(
					`${line.file}: external decode failed: ${(e as Error).message.split('\n')[0]}`
				);
			}

			cells.push({
				image: f.id,
				format: line.format,
				quality: line.quality,
				channels: line.channels,
				bytes: line.bytes,
				bpp: line.bpp,
				db: db === null ? null : Math.round(db * 10000) / 10000,
				db_pm: dbPm === null ? null : Math.round(dbPm * 10000) / 10000,
				time_us: line.time_us
			});
			for (const ext of ['pnm', 'pam', 'png'])
				rmSync(`${line.file}.dec.${ext}`, { force: true });
			rmSync(line.file, { force: true });
		}
		rmSync(join(work, `${f.id}.ref`), { force: true });
	}

	return cells;
}

function writeInputs(corpus: string, files: Source[]): void {
	const inputs = files.map((f) => ({
		id: f.id,
		sha256: createHash('sha256').update(readFileSync(f.path)).digest('hex')
	}));
	const manifest = join(ROOT, 'scripts', 'corpus-manifest.json');
	const manifestSha =
		corpus === 'fixtures'
			? undefined
			: createHash('sha256').update(readFileSync(manifest)).digest('hex');
	writeFileSync(
		join(BASELINES, `inputs.${corpus}.json`),
		JSON.stringify(
			{
				corpus,
				manifest_sha256: manifestSha,
				widths: WIDTHS,
				encode_width: ENCODE_WIDTH,
				files: inputs
			},
			null,
			'\t'
		) + '\n'
	);
}

function writeCells(name: string, cells: unknown[]): string {
	mkdirSync(BASELINES, { recursive: true });
	const file = join(BASELINES, name);
	writeFileSync(file, cells.map((c) => JSON.stringify(c)).join('\n') + '\n');
	return file;
}

function summarizeLever(cells: LeverCell[]): void {
	const groups = new Map<string, LeverCell[]>();
	for (const c of cells) {
		const key = [c.src_format, c.effort_delta, c.filter_delta, c.scale_steps].join(' ');
		(groups.get(key) ?? groups.set(key, []).get(key)!).push(c);
	}
	console.log('format effort filter steps     n   p05 median   exact');
	for (const [key, g] of [...groups].sort()) {
		const dbs = g.map((c) => c.db);
		console.log(
			`${key.split(' ')[0]!.padEnd(6)} ${key
				.split(' ')
				.slice(1)
				.map((v) => v.padStart(6))
				.join(' ')} ` +
				`${String(g.length).padStart(5)} ${percentile(dbs, 0.05).toFixed(2).padStart(5)} ` +
				`${percentile(dbs, 0.5).toFixed(2).padStart(6)} ${String(g.filter((c) => c.identical).length).padStart(6)}`
		);
	}
}

function takeTimingLock(): boolean {
	const limit = cpus().length / 4;
	if (loadavg()[0]! > limit) {
		console.error(
			`timing refused: load average ${loadavg()[0]!.toFixed(2)} is above ${limit} (a quarter of ${cpus().length} cores)`
		);
		return false;
	}
	try {
		mkdirSync(LOCK);
		return true;
	} catch {
		console.error(`timing refused: ${LOCK} is held`);
		return false;
	}
}

function dropTimingLock(): void {
	try {
		rmdirSync(LOCK);
	} catch {
		// already gone
	}
}

function runBytesCheck(): number {
	const names = ['sf-24.jpg', 'toyota_racing.webp', 'winter_forest.png'];
	const cases = [
		{ width: 300, budget: 0 },
		{ width: 300, budget: 4000 },
		{ width: 200, budget: 1 }
	];
	const script = `
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createRequire } from 'node:module';
const root = ${JSON.stringify(ROOT)};
const { Image, TinyImgModule } = await import(join(root, 'src/ts/index.ts'));
const mod = await TinyImgModule.load(await WebAssembly.compile(readFileSync(join(root, 'bin/tinyimg.wasm'))));
function fnv(d) { let h = 1469598103934665603n; for (const b of d) { h ^= BigInt(b); h = (h * 1099511628211n) & 0xffffffffffffffffn; } return h.toString(16).padStart(16, '0'); }
for (const name of ${JSON.stringify(names)}) {
	for (const c of ${JSON.stringify(cases)}) {
		using image = await Image.open(mod, readFileSync(join(root, 'tests/fixtures', name)));
		image.resize(c.width, 0);
		if (c.budget) image.budget(c.budget / 1000);
		const raw = await image.pixels();
		console.log(JSON.stringify({ image: name, width: c.width, budget: c.budget, w: raw.width, h: raw.height, channels: raw.channels, fnv1a: fnv(raw.pixels) }));
	}
}
`;
	const scratch = join(buildDir, 'bytes-check.mjs');
	writeFileSync(scratch, script);
	const wasm = spawnSync(process.execPath, [scratch], { encoding: 'utf8', cwd: ROOT });
	rmSync(scratch, { force: true });
	if (wasm.status !== 0) {
		console.error(wasm.stderr);
		return 1;
	}

	type Hash = {
		image: string;
		width: number;
		budget: number;
		fnv1a: string;
		w: number;
		h: number;
	};
	const want = lines<Hash>(wasm.stdout);
	let mismatches = 0;

	for (const w of want) {
		const run = spawnSync(
			probe,
			['hash', String(w.width), String(w.budget), join(ROOT, 'tests', 'fixtures', w.image)],
			{ encoding: 'utf8' }
		);
		const got = lines<Hash>(run.stdout)[0];
		const same = got !== undefined && got.fnv1a === w.fnv1a && got.w === w.w && got.h === w.h;
		if (!same) mismatches++;
		console.log(
			`${w.image} width ${w.width} budget ${w.budget}us: wasm ${w.fnv1a} native ${got?.fnv1a} ${same ? 'same' : 'DIFFERENT'}`
		);
	}

	return mismatches === 0 ? 0 : 1;
}

function runCheckTest(): number {
	const tablePath = join(BASELINES, 'table.json');
	const test = sources('test');

	if (test.length === 0 || !existsSync(tablePath)) {
		console.log(
			'check-test: not run (no corpus manifest passes `bun scripts/corpus.ts check`)'
		);
		return 0;
	}

	const table = JSON.parse(readFileSync(tablePath, 'utf8')) as Table;
	const lever = leverCells(test);
	const encode = encodeCells(test, false);
	const tally = new Map<string, { n: number; hit: number }>();
	const add = (key: string, ok: boolean) => {
		const t = tally.get(key) ?? tally.set(key, { n: 0, hit: 0 }).get(key)!;
		t.n++;
		if (ok) t.hit++;
	};

	for (const row of table.lever) {
		const cells = lever.filter(
			(c) =>
				c.src_format === row.format &&
				c.effort_delta === row.effort_delta &&
				c.filter_delta === row.filter_delta &&
				c.scale_steps === row.scale_steps
		);
		for (const c of cells) add(`lever ${row.format}`, c.db >= row.p05);
		for (const c of cells) add(`lever all`, c.db >= row.p05);
	}
	for (const row of table.encode) {
		for (const c of encode.filter(
			(e) => e.format === row.format && e.quality === row.quality && e.db !== null
		)) {
			const alpha = c.channels === 2 || c.channels === 4;
			add(alpha ? `encode alpha ${row.format}` : `encode ${row.format}`, c.db! >= row.p05);
			if (!alpha) add('encode all', c.db! >= row.p05);
		}
	}

	console.log('held-out coverage of p05 (target 95 percent)');
	for (const [key, t] of [...tally].sort()) {
		console.log(
			`${key.padEnd(14)} ${((100 * t.hit) / t.n).toFixed(1).padStart(5)}%  (${t.hit}/${t.n})`
		);
	}
	return 0;
}

async function main(): Promise<number> {
	const cmd = args[0];
	buildProbe();

	if (args.includes('--check-test')) return runCheckTest();
	if (cmd === 'bytes-check') return runBytesCheck();

	const corpus = flag('--corpus', 'fixtures');
	const files = sources(corpus);
	if (files.length === 0) {
		console.error(`corpus ${corpus} has no usable entries`);
		return 1;
	}

	if (cmd === 'lever') {
		const cells = leverCells(files);
		mkdirSync(BASELINES, { recursive: true });
		writeInputs(corpus, files);
		const file = writeCells(`lever-cells.${corpus}.jsonl`, cells);
		console.log(`${files.length} images, ${cells.length} cells -> ${file}`);
		summarizeLever(cells);
		return 0;
	}

	if (cmd === 'encode') {
		const timed = args.includes('--time') && takeTimingLock();
		try {
			const cells = encodeCells(files, timed);
			mkdirSync(BASELINES, { recursive: true });
			writeInputs(corpus, files);
			const file = writeCells(`encode-cells.${corpus}.jsonl`, cells);
			console.log(
				`${files.length} images, ${cells.length} cells -> ${file} (timing ${timed ? 'on' : 'off'})`
			);
		} finally {
			if (timed) dropTimingLock();
		}
		return 0;
	}

	console.error('usage: lever | encode | bytes-check | --check-test');
	return 2;
}

process.exitCode = await main();
