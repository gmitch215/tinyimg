/**
 * Times the decode and encode phases of every `cal` corpus entry, on the shipping module, for
 * `scripts/cost-model.ts` to fit.
 *
 * ```sh
 * bun scripts/measure/fit-cost.ts [--wasm bin/tinyimg.wasm] [--partial cells.partial.jsonl]
 *                                 [--only decode,encode,effort,whole,effortdec,tier]
 *                                 [--set original|derived|both] [--ids a,b] [--limit N]
 * bun scripts/measure/fit-cost.ts --finish [--partial ...] [--base cells.json] [--out file]
 * ```
 *
 * `effortdec` times `tiny_image_decode` at FANCY and FAST in alternating rounds, whole frame at
 * each denominator. `tier` times groups of encodes of one input in alternating rounds (quality
 * points, WebP lossless, RGB against RGBA with and without alpha, FANCY against FAST), so a ratio
 * inside a group cancels drift. `--set derived` reads `scripts/corpus-derived.json`; a derived
 * entry takes its source's fit flag. `--finish --base` merges the partial rows over an earlier
 * cells file.
 *
 * A periodic eval, not a gate test. Reads `cal` entries only; the `test` split is lane A4's and is
 * never opened here. Each timed cell is the median of 9 runs after 2 warmups, taken under the
 * `/tmp/tinyimg-timing.lock` directory lock. The script waits for the machine to fall below a
 * quarter of its cores in load average before it starts, the check `estimate.ts` makes, and it
 * records the load beside every cell. The 1-minute average includes this process's own core, so a
 * control cell (a fixed decode) is re-timed every few entries and the sweep waits and retries when
 * the control drifts, which names interference directly instead of through the load.
 *
 * Rows go to a JSONL partial file as they finish, so a killed run resumes by key. `--finish`
 * writes the sorted result to `bench/baselines/fit/cells.json`.
 *
 * Phase isolation: decode is `tiny_image_load_scaled` and `tiny_image_load_region` called
 * directly, with the input copied in and the pixels left in the module, so no plan, resample or
 * copy-out is in the clock. Encode is `tiny_image_encode` over an in-memory image. A `whole` cell
 * times `Image.open(...).bytes(format)` with no operation, which is decode plus encode and nothing
 * else, and compares it with the two isolated phases.
 */

import { createHash } from 'node:crypto';
import {
	appendFileSync,
	existsSync,
	mkdirSync,
	readFileSync,
	rmdirSync,
	writeFileSync
} from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { join, resolve } from 'node:path';
import type { ImageFormat } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';

const ROOT = join(import.meta.dirname, '..', '..');
const LOCK = '/tmp/tinyimg-timing.lock';
const WARMUPS = 2;
const RUNS = 9;
const DENS = [1, 2, 4, 8] as const;
const CONTROL_EVERY = 8;
const CONTROL_DRIFT = 1.15;
const COMPRESSION = ['auto', 'none', 'fast', 'default', 'best'] as const;

type Entry = {
	id: string;
	format: ImageFormat;
	split: 'cal' | 'test';
	sha256: string;
	bytes: number;
	width: number;
	height: number;
	progressive?: boolean;
	subsampling?: string;
	restartInterval?: number;
	source?: 'local';
	path?: string;
	derived?: boolean;
	fitSource?: boolean;
};

/** One timed cell. `kind` selects which of the optional fields are present. */
export type Cell = {
	key: string;
	kind: 'decode' | 'encode' | 'effort' | 'whole' | 'control' | 'effortdec' | 'tier';
	id: string;
	sha8: string;
	fit: boolean;
	derived?: boolean;
	format: ImageFormat;
	load: number;
	ms: number;
	min: number;
	max: number;
	[field: string]: unknown;
};

const args = process.argv.slice(2);

function flag(name: string, fallback: string): string {
	const i = args.indexOf(name);
	return i >= 0 && args[i + 1] ? args[i + 1]! : fallback;
}

const wasmPath = resolve(ROOT, flag('--wasm', 'bin/tinyimg.wasm'));
const partialPath = flag(
	'--partial',
	join(ROOT, 'bench', 'baselines', 'fit', 'cells.partial.jsonl')
);
const only = new Set(flag('--only', 'decode,encode,effort,whole').split(','));
const entrySet = flag('--set', 'original');
const baseCells = flag('--base', '');
const limit = Number(flag('--limit', '0'));
const cores = cpus().length;
const loadLimit = cores / 4;

function sleep(ms: number): Promise<void> {
	return new Promise((done) => setTimeout(done, ms));
}

/** Waits for the 1-minute load average to fall under the limit; never lowers the limit. */
async function waitForLoad(): Promise<void> {
	let announced = false;

	while (!args.includes('--debug-ignore-load') && loadavg()[0]! > loadLimit) {
		if (!announced) {
			console.error(`load ${loadavg()[0]!.toFixed(2)} is over ${loadLimit}; waiting`);
			announced = true;
		}
		await sleep(15000);
	}
}

async function acquire(): Promise<void> {
	for (;;) {
		try {
			mkdirSync(LOCK);
			return;
		} catch (error) {
			if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
			await sleep(2000);
		}
	}
}

function release(): void {
	try {
		rmdirSync(LOCK);
	} catch {
		// already gone
	}
}

function median(values: number[]): number {
	return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]!;
}

type Timing = { ms: number; min: number; max: number };

/** Median of RUNS after WARMUPS, holding the lock for the whole cell. */
async function timed(body: () => void): Promise<Timing> {
	await acquire();

	try {
		for (let i = 0; i < WARMUPS; i++) body();

		const samples: number[] = [];
		for (let i = 0; i < RUNS; i++) {
			const start = performance.now();
			body();
			samples.push(performance.now() - start);
		}

		return { ms: median(samples), min: Math.min(...samples), max: Math.max(...samples) };
	} finally {
		release();
	}
}

const wasmBytes = readFileSync(wasmPath);
const wasmSha = createHash('sha256').update(wasmBytes).digest('hex');
const tinyimg = await TinyImgModule.load(await WebAssembly.compile(wasmBytes));

// a pinned module can predate an export the wrapper's probe calls; orientation is not timed
const missing = new Set(['tiny_avif_orientation', 'tiny_work_clock_us']);
const rawExports = tinyimg.exports as unknown as Record<string | symbol, unknown>;
(tinyimg as unknown as { exports: unknown }).exports = new Proxy(rawExports, {
	get: (target, name) =>
		typeof name === 'string' && missing.has(name) && !(name in target) ? () => 0 : target[name]
});
const exp = tinyimg.exports;

function readManifest(): Entry[] {
	const manifest = JSON.parse(
		readFileSync(join(ROOT, 'scripts', 'corpus-manifest.json'), 'utf8')
	) as { entries: Entry[] };

	const original =
		entrySet === 'derived' ? [] : manifest.entries.filter((e) => e.split === 'cal');
	const derived =
		entrySet === 'original'
			? []
			: (
					JSON.parse(
						readFileSync(join(ROOT, 'scripts', 'corpus-derived.json'), 'utf8')
					) as { entries: Entry[] }
				).entries
					.filter((e) => e.split === 'cal')
					.map((e) => ({ ...e, derived: true }));

	return [...original, ...derived].sort((a, b) => (a.id < b.id ? -1 : 1));
}

function entryPath(e: Entry): string {
	if (e.derived)
		return join(ROOT, 'corpus', 'derived', `${e.id}.${e.format === 'gif' ? 'gif' : 'tif'}`);
	if (e.source === 'local') return join(ROOT, e.path ?? '');
	const ext = e.format === 'jpeg' ? 'jpg' : e.format === 'tiff' ? 'tif' : e.format;
	return join(ROOT, 'corpus', `${e.id}.${ext}`);
}

function load(e: Entry): Uint8Array<ArrayBuffer> {
	const file = readFileSync(entryPath(e));
	const bytes = new Uint8Array(file.byteLength);
	bytes.set(file);

	if (createHash('sha256').update(bytes).digest('hex') !== e.sha256) {
		throw new Error(`${e.id}: sha256 differs from the manifest`);
	}

	return bytes;
}

const done = new Set<string>();
if (existsSync(partialPath)) {
	for (const line of readFileSync(partialPath, 'utf8').split('\n')) {
		if (line.startsWith('{')) done.add((JSON.parse(line) as Cell).key);
	}
}
mkdirSync(join(partialPath, '..'), { recursive: true });

function emit(cell: Cell): void {
	appendFileSync(partialPath, `${JSON.stringify(cell)}\n`);
	done.add(cell.key);
}

function fitOf(sha256: string): boolean {
	return parseInt(sha256.slice(8, 10), 16) % 2 === 0;
}

/** A derived entry takes the fit flag of its source, so no source straddles the two halves. */
function fitOfEntry(e: Entry): boolean {
	return e.derived ? (e.fitSource ?? false) : fitOf(e.sha256);
}

type Base = Pick<Cell, 'id' | 'sha8' | 'fit' | 'format'>;

let info = { channels: 0, hasAlpha: false };

function baseOf(e: Entry): Base & { srcChannels: number; hasAlpha: boolean; derived?: boolean } {
	return {
		id: e.id,
		sha8: e.sha256.slice(0, 8),
		fit: fitOfEntry(e),
		...(e.derived ? { derived: true } : {}),
		format: e.format,
		srcChannels: info.channels,
		hasAlpha: info.hasAlpha
	};
}

function counters(work: Awaited<ReturnType<typeof tinyimg.measure>>['work']) {
	return {
		source: work.sourceSamples,
		decoded: work.decodedSamples,
		entropy: work.entropyBytes,
		blocks: work.blocks,
		macroblocks: work.macroblocks,
		scansSkipped: work.scansSkipped,
		probes: work.encodeProbes,
		encoded: work.encoded
	};
}

// #region decode

type Region = { x: number; y: number; w: number; h: number };

/** One decode of the whole frame at a denominator, or of a rectangle at full scale. */
function decodeOnce(
	bytes: Uint8Array,
	buffer: number,
	image: number,
	e: Entry,
	den: number,
	region: Region | null
): number {
	const code = region
		? exp.tiny_image_load_region(
				image,
				buffer,
				bytes.byteLength,
				region.x,
				region.y,
				region.w,
				region.h
			)
		: exp.tiny_image_load_scaled(
				image,
				buffer,
				bytes.byteLength,
				Math.ceil(e.width / den),
				Math.ceil(e.height / den)
			);

	if (code === 0) exp.tiny_image_destroy(image);
	return code;
}

async function decodeCell(
	e: Entry,
	bytes: Uint8Array<ArrayBuffer>,
	den: number,
	region: Region | null,
	tag: string
): Promise<void> {
	const key = `decode|${e.id}|${tag}`;
	if (done.has(key)) return;

	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(exp.tiny_image_sizeof());

	try {
		const code = decodeOnce(bytes, buffer, image, e, den, region);
		if (code !== 0) {
			emit({
				...baseOf(e),
				key,
				kind: 'decode',
				load: loadavg()[0]!,
				ms: 0,
				min: 0,
				max: 0,
				error: code,
				den,
				tag
			});
			return;
		}

		const timing = await timed(() => decodeOnce(bytes, buffer, image, e, den, region));
		const { work } = await tinyimg.measure(() =>
			decodeOnce(bytes, buffer, image, e, den, region)
		);

		emit({
			...baseOf(e),
			...timing,
			key,
			kind: 'decode',
			load: loadavg()[0]!,
			tag,
			den,
			region,
			fileBytes: bytes.byteLength,
			width: e.width,
			height: e.height,
			progressive: e.progressive ?? false,
			subsampling: e.subsampling ?? null,
			restartInterval: e.restartInterval ?? 0,
			counters: counters(work)
		});
	} finally {
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

async function decodeCells(e: Entry, bytes: Uint8Array<ArrayBuffer>): Promise<void> {
	const w = e.width;
	const h = e.height;

	for (const den of DENS) await decodeCell(e, bytes, den, null, `den${den}`);

	const top = { x: 0, y: 0, w, h: Math.ceil(h / 4) };
	const centre = { x: w >> 2, y: h >> 2, w: w >> 1, h: h >> 1 };
	const bottom = { x: 0, y: h >> 1, w, h: h - (h >> 1) };

	await decodeCell(e, bytes, 1, top, 'top');
	await decodeCell(e, bytes, 1, centre, 'centre');
	await decodeCell(e, bytes, 1, bottom, 'bottom');
}

// #endregion

// #region encode

const OUTPUTS: ImageFormat[] = ['jpeg', 'webp', 'png', 'gif', 'tiff', 'bmp', 'avif'];

type RawInput = { width: number; height: number; channels: number; pixels: Uint8Array };

function nonOpaque(raw: RawInput): boolean {
	if (raw.channels !== 4 && raw.channels !== 2) return false;
	const step = raw.channels;

	for (let i = step - 1; i < raw.pixels.length; i += step) {
		if (raw.pixels[i]! < 255) return true;
	}

	return false;
}

type EncodeVariant = {
	tag: string;
	format: ImageFormat;
	quality: number;
	lossless: boolean;
	compression: (typeof COMPRESSION)[number];
};

async function encodeCells(e: Entry, bytes: Uint8Array<ArrayBuffer>): Promise<void> {
	const widths = [256, 800]
		.map((w) => Math.min(w, e.width))
		.filter((w, i, all) => all.indexOf(w) === i);

	for (const width of widths) {
		const small = width === widths[0];
		const variants: EncodeVariant[] = [];

		for (const format of OUTPUTS) {
			variants.push({
				tag: `${format}`,
				format,
				quality: 0,
				lossless: false,
				compression: 'auto'
			});
		}

		if (small) {
			for (const compression of ['none', 'fast', 'best'] as const) {
				for (const format of ['png', 'tiff'] as ImageFormat[]) {
					variants.push({
						tag: `${format}-${compression}`,
						format,
						quality: 0,
						lossless: false,
						compression
					});
				}
			}

			variants.push({
				tag: 'webp-lossless',
				format: 'webp',
				quality: 0,
				lossless: true,
				compression: 'auto'
			});

			for (const quality of [50, 95]) {
				for (const format of ['jpeg', 'webp', 'avif'] as ImageFormat[]) {
					variants.push({
						tag: `${format}-q${quality}`,
						format,
						quality,
						lossless: false,
						compression: 'auto'
					});
				}
			}
		}

		const pending = variants.filter((v) => !done.has(`encode|${e.id}|w${width}|${v.tag}`));
		if (pending.length === 0) continue;

		let raw: RawInput;
		try {
			using plan = await Image.open(tinyimg, bytes);
			if (width < e.width) plan.resize(width, 0);
			raw = await plan.pixels();
		} catch (error) {
			console.error(
				`${e.id}: cannot derive the ${width} px input: ${(error as Error).message}`
			);
			continue;
		}

		const opaque = !nonOpaque(raw);
		const image = tinyimg.alloc(exp.tiny_image_sizeof());
		const writer = tinyimg.alloc(exp.tiny_writer_sizeof());
		const opts = tinyimg.alloc(exp.tiny_encode_opts_sizeof());

		try {
			if (exp.tiny_image_create(image, raw.width, raw.height, raw.channels) !== 0) {
				throw new Error('create failed');
			}
			tinyimg.bytes().set(raw.pixels, exp.tiny_image_getdata(image));

			for (const v of pending) {
				const key = `encode|${e.id}|w${width}|${v.tag}`;
				const view = tinyimg.view();
				view.setUint8(opts, v.quality);
				view.setUint8(opts + 1, v.lossless ? 1 : 0);
				view.setUint8(opts + 2, 0);
				view.setUint8(opts + 3, 0);
				view.setUint8(opts + 4, 0);
				view.setUint8(opts + 5, COMPRESSION.indexOf(v.compression));
				const id = tinyimg.formatId(v.format);

				const once = (): number => {
					exp.tiny_writer_init(writer, 0);
					const code = exp.tiny_image_encode(image, id, opts, writer);
					exp.tiny_writer_free(writer);
					return code;
				};

				const code = once();
				if (code !== 0) {
					emit({
						...baseOf(e),
						key,
						kind: 'encode',
						load: loadavg()[0]!,
						ms: 0,
						min: 0,
						max: 0,
						error: code,
						tag: v.tag
					});
					continue;
				}

				const timing = await timed(() => void once());
				let outBytes = 0;
				const { work } = await tinyimg.measure(() => {
					exp.tiny_writer_init(writer, 0);
					exp.tiny_image_encode(image, id, opts, writer);
					outBytes = exp.tiny_writer_size(writer);
					exp.tiny_writer_free(writer);
				});

				emit({
					...baseOf(e),
					...timing,
					key,
					kind: 'encode',
					load: loadavg()[0]!,
					tag: v.tag,
					outFormat: v.format,
					quality: v.quality,
					lossless: v.lossless,
					compression: v.compression,
					width: raw.width,
					height: raw.height,
					channels: raw.channels,
					opaque,
					outBytes,
					counters: counters(work)
				});
			}
		} finally {
			exp.tiny_image_destroy(image);
			tinyimg.free(opts);
			tinyimg.free(writer);
			tinyimg.free(image);
		}
	}
}

// #endregion

// #region effort

/** FAST against FANCY through the plan, as `calibrate.ts` does, at the width that selects each den. */
async function effortCells(e: Entry, bytes: Uint8Array<ArrayBuffer>): Promise<void> {
	if (e.format !== 'jpeg' && e.format !== 'webp') return;
	if (e.width * e.height > 4_000_000) return;

	for (const den of DENS) {
		const key = `effort|${e.id}|den${den}`;
		if (done.has(key)) continue;

		const width = Math.max(1, Math.floor(e.width / den));
		const run = (effort: 'fancy' | 'fast') => async () => {
			using image = await Image.open(tinyimg, bytes);
			image.resize(width, 0).effort(effort);
			await image.pixels();
		};

		try {
			await run('fancy')();
		} catch {
			continue;
		}

		const fancy = await timedAsync(run('fancy'));
		const fast = await timedAsync(run('fast'));

		emit({
			...baseOf(e),
			key,
			kind: 'effort',
			load: loadavg()[0]!,
			ms: fancy.ms,
			min: fancy.min,
			max: fancy.max,
			den,
			fastMs: fast.ms,
			ratio: fast.ms / fancy.ms,
			progressive: e.progressive ?? false,
			subsampling: e.subsampling ?? null
		});
	}
}

/** The async plan path cannot run inside the synchronous `timed`, so it takes its own lock. */
async function timedAsync(body: () => Promise<void>): Promise<Timing> {
	await acquire();

	try {
		for (let i = 0; i < WARMUPS; i++) await body();

		const samples: number[] = [];
		for (let i = 0; i < RUNS; i++) {
			const start = performance.now();
			await body();
			samples.push(performance.now() - start);
		}

		return { ms: median(samples), min: Math.min(...samples), max: Math.max(...samples) };
	} finally {
		release();
	}
}

// #endregion

// #region whole

const WHOLE_OUTPUTS: ImageFormat[] = ['jpeg', 'webp', 'png', 'gif', 'bmp'];

/** Whole request against the two isolated phases, for the phase-sum check. */
async function wholeCell(e: Entry, bytes: Uint8Array<ArrayBuffer>, index: number): Promise<void> {
	const out = WHOLE_OUTPUTS[index % WHOLE_OUTPUTS.length]!;
	const key = `whole|${e.id}|${out}`;
	if (done.has(key)) return;

	const whole = await timedAsync(async () => {
		using image = await Image.open(tinyimg, bytes);
		await image.bytes(out);
	});

	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(exp.tiny_image_sizeof());
	const writer = tinyimg.alloc(exp.tiny_writer_sizeof());
	const opts = tinyimg.alloc(exp.tiny_encode_opts_sizeof());

	try {
		tinyimg.bytes().fill(0, opts, opts + exp.tiny_encode_opts_sizeof());
		const id = tinyimg.formatId(out);

		if (exp.tiny_image_load(image, buffer, bytes.byteLength) !== 0) return;
		exp.tiny_image_destroy(image);

		const decode = await timed(() => {
			exp.tiny_image_load(image, buffer, bytes.byteLength);
			exp.tiny_image_destroy(image);
		});

		exp.tiny_image_load(image, buffer, bytes.byteLength);

		const encode = await timed(() => {
			exp.tiny_writer_init(writer, 0);
			exp.tiny_image_encode(image, id, opts, writer);
			exp.tiny_writer_free(writer);
		});

		exp.tiny_image_destroy(image);

		emit({
			...baseOf(e),
			key,
			kind: 'whole',
			load: loadavg()[0]!,
			ms: whole.ms,
			min: whole.min,
			max: whole.max,
			outFormat: out,
			decodeMs: decode.ms,
			encodeMs: encode.ms,
			sumOverWhole: (decode.ms + encode.ms) / whole.ms
		});
	} finally {
		tinyimg.free(opts);
		tinyimg.free(writer);
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

// #endregion

// #region groups

type Samples = number[][];

/** Times several bodies round-robin, so a ratio between two of them cancels drift. */
async function timedGroup(bodies: Array<() => void>): Promise<Samples> {
	await acquire();

	try {
		for (let i = 0; i < WARMUPS; i++) for (const body of bodies) body();

		const samples: Samples = bodies.map(() => []);
		for (let r = 0; r < RUNS; r++) {
			for (let k = 0; k < bodies.length; k++) {
				const i = (k + r) % bodies.length;
				const start = performance.now();
				bodies[i]!();
				samples[i]!.push(performance.now() - start);
			}
		}

		return samples;
	} finally {
		release();
	}
}

/** Median over the rounds of body `i` over body 0, taken within each round. */
function ratioOf(samples: Samples, i: number): { ratio: number; min: number; max: number } {
	const rounds = samples[0]!.map((base, r) => samples[i]![r]! / base);
	return { ratio: median(rounds), min: Math.min(...rounds), max: Math.max(...rounds) };
}

// #endregion

// #region effort decode

const decodeExports = exp as unknown as {
	tiny_image_decode(image: number, buffer: number, size: number, opts: number): number;
};

function isLosslessWebp(bytes: Uint8Array): boolean {
	return Buffer.from(bytes.subarray(0, 64)).indexOf('VP8L') >= 0;
}

/** FAST over FANCY through `tiny_image_decode` itself, whole frame at each denominator. */
async function effortDecodeCells(e: Entry, bytes: Uint8Array<ArrayBuffer>): Promise<void> {
	if (e.width * e.height > 4_000_000) return;

	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(exp.tiny_image_sizeof());
	const opts = tinyimg.alloc(24);

	const run = (den: number, effort: number) => (): number => {
		const view = tinyimg.view();
		for (let i = 0; i < 20; i += 4) view.setUint32(opts + i, 0, true);
		view.setUint8(opts + 16, den);
		view.setUint8(opts + 18, effort);
		const code = decodeExports.tiny_image_decode(image, buffer, bytes.byteLength, opts);
		if (code === 0) exp.tiny_image_destroy(image);
		return code;
	};

	try {
		for (const den of DENS) {
			const key = `effortdec|${e.id}|den${den}`;
			if (done.has(key)) continue;

			if (run(den, 0)() !== 0 || run(den, 1)() !== 0) {
				emit({
					...baseOf(e),
					key,
					kind: 'effortdec',
					load: loadavg()[0]!,
					ms: 0,
					min: 0,
					max: 0,
					error: 1,
					den
				});
				continue;
			}

			const samples = await timedGroup([run(den, 0), run(den, 1)]);
			const r = ratioOf(samples, 1);

			emit({
				...baseOf(e),
				key,
				kind: 'effortdec',
				load: loadavg()[0]!,
				ms: median(samples[0]!),
				min: Math.min(...samples[0]!),
				max: Math.max(...samples[0]!),
				fastMs: median(samples[1]!),
				ratio: r.ratio,
				ratioMin: r.min,
				ratioMax: r.max,
				den,
				width: e.width,
				height: e.height,
				progressive: e.progressive ?? false,
				subsampling: e.subsampling ?? null,
				lossless: e.format === 'webp' ? isLosslessWebp(bytes) : null
			});
		}
	} finally {
		tinyimg.free(opts);
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

// #endregion

// #region tiers

type Spec = { variant: string; quality: number; lossless: boolean; effort: number };

const QUALITIES = [50, 75, 85, 90, 95];
/** Width of the one group measured above 800 px: WebP lossless over lossy, where the ratio grows with size. */
const LARGE_WIDTH = 1600;

/** The raw input as packed RGB, gray and gray-alpha replicated, alpha dropped. */
function rgbOf(raw: RawInput): Uint8Array {
	const n = raw.width * raw.height;
	const out = new Uint8Array(n * 3);

	for (let i = 0; i < n; i++) {
		const c = raw.channels;
		const p = i * c;
		const r = raw.pixels[p]!;
		out[i * 3] = r;
		out[i * 3 + 1] = c >= 3 ? raw.pixels[p + 1]! : r;
		out[i * 3 + 2] = c >= 3 ? raw.pixels[p + 2]! : r;
	}

	return out;
}

function rgbaOf(rgb: Uint8Array, width: number, ramp: boolean): Uint8Array {
	const n = rgb.length / 3;
	const out = new Uint8Array(n * 4);

	for (let i = 0; i < n; i++) {
		out[i * 4] = rgb[i * 3]!;
		out[i * 4 + 1] = rgb[i * 3 + 1]!;
		out[i * 4 + 2] = rgb[i * 3 + 2]!;
		out[i * 4 + 3] = ramp ? 64 + Math.round(((i % width) * 191) / Math.max(1, width - 1)) : 255;
	}

	return out;
}

/** Interleaved groups on one input: quality points, alpha and effort; a group's first body is its base. */
async function tierCells(e: Entry, bytes: Uint8Array<ArrayBuffer>): Promise<void> {
	const widths = [256, 800]
		.map((w) => Math.min(w, e.width))
		.filter((w, i, all) => all.indexOf(w) === i);
	if (e.width >= LARGE_WIDTH) widths.push(LARGE_WIDTH);

	for (const width of widths) {
		const prefix = `tier|${e.id}|w${width}|`;
		const keys = (group: string, format: string, variants: string[]) =>
			variants.map((v) => `${prefix}${group}|${format}|${v}`);

		const quality = (format: ImageFormat): Spec[] => [
			{ variant: 'default', quality: 0, lossless: false, effort: 0 },
			...QUALITIES.map((q) => ({
				variant: `q${q}`,
				quality: q,
				lossless: false,
				effort: 0
			})),
			...(format === 'webp'
				? [{ variant: 'lossless', quality: 0, lossless: true, effort: 0 }]
				: [])
		];
		const alpha = (lossless: boolean): Spec[] =>
			['rgb', 'rgba-opaque', 'rgba-ramp'].map((variant) => ({
				variant,
				quality: 0,
				lossless,
				effort: 0
			}));
		const effort = (lossless: boolean): Spec[] =>
			['fancy', 'fast'].map((variant, i) => ({ variant, quality: 0, lossless, effort: i }));

		type Plan = {
			group: string;
			format: ImageFormat;
			specs: Spec[];
			input: 'rgb' | 'alpha';
			tag: string;
		};
		const plans: Plan[] = [];

		for (const format of ['jpeg', 'webp', 'avif'] as ImageFormat[]) {
			plans.push({
				group: 'quality',
				format,
				specs: quality(format),
				input: 'rgb',
				tag: format
			});
		}
		for (const format of OUTPUTS) {
			plans.push({
				group: 'alpha',
				format,
				specs: alpha(false),
				input: 'alpha',
				tag: format
			});
			plans.push({
				group: 'effort',
				format,
				specs: effort(false),
				input: 'rgb',
				tag: format
			});
		}
		plans.push({
			group: 'alpha',
			format: 'webp',
			specs: alpha(true),
			input: 'alpha',
			tag: 'webp-lossless'
		});
		plans.push({
			group: 'effort',
			format: 'webp',
			specs: effort(true),
			input: 'rgb',
			tag: 'webp-lossless'
		});

		const sizePlan: Plan = {
			group: 'size',
			format: 'webp',
			specs: [
				{ variant: 'default', quality: 0, lossless: false, effort: 0 },
				{ variant: 'lossless', quality: 0, lossless: true, effort: 0 }
			],
			input: 'rgb',
			tag: 'webp'
		};

		const pending = (width === LARGE_WIDTH ? [sizePlan] : plans).filter(
			(p) =>
				!keys(
					p.group,
					p.tag,
					p.specs.map((s) => s.variant)
				).every((k) => done.has(k))
		);
		if (pending.length === 0) continue;

		let raw: RawInput;
		try {
			using plan = await Image.open(tinyimg, bytes);
			if (width < e.width) plan.resize(width, 0);
			raw = await plan.pixels();
		} catch (error) {
			console.error(
				`${e.id}: cannot derive the ${width} px input: ${(error as Error).message}`
			);
			continue;
		}

		const rgb = rgbOf(raw);
		const inputs: Record<string, { channels: number; pixels: Uint8Array }> = {
			rgb: { channels: 3, pixels: rgb },
			'rgba-opaque': { channels: 4, pixels: rgbaOf(rgb, raw.width, false) },
			'rgba-ramp': { channels: 4, pixels: rgbaOf(rgb, raw.width, true) }
		};
		const images = new Map<string, number>();
		const writer = tinyimg.alloc(exp.tiny_writer_sizeof());
		const opts = tinyimg.alloc(exp.tiny_encode_opts_sizeof());

		try {
			for (const [name, input] of Object.entries(inputs)) {
				const image = tinyimg.alloc(exp.tiny_image_sizeof());
				if (exp.tiny_image_create(image, raw.width, raw.height, input.channels) !== 0) {
					throw new Error('create failed');
				}
				tinyimg.bytes().set(input.pixels, exp.tiny_image_getdata(image));
				images.set(name, image);
			}

			for (const p of pending) {
				const id = tinyimg.formatId(p.format);
				const imageOf = (s: Spec): number =>
					images.get(p.input === 'rgb' ? 'rgb' : s.variant)!;

				const once =
					(s: Spec, size?: number[]): (() => number) =>
					() => {
						const view = tinyimg.view();
						view.setUint8(opts, s.quality);
						view.setUint8(opts + 1, s.lossless ? 1 : 0);
						view.setUint8(opts + 2, 0);
						view.setUint8(opts + 3, 0);
						view.setUint8(opts + 4, s.effort);
						view.setUint8(opts + 5, 0);
						exp.tiny_writer_init(writer, 0);
						const code = exp.tiny_image_encode(imageOf(s), id, opts, writer);
						size?.push(code === 0 ? exp.tiny_writer_size(writer) : 0);
						exp.tiny_writer_free(writer);
						return code;
					};

				const outBytes: number[] = [];
				let failed = 0;
				for (const s of p.specs) failed = once(s, outBytes)() || failed;

				if (failed) {
					emit({
						...baseOf(e),
						key: `${prefix}${p.group}|${p.tag}|${p.specs[0]!.variant}`,
						kind: 'tier',
						load: loadavg()[0]!,
						ms: 0,
						min: 0,
						max: 0,
						error: failed,
						group: p.group,
						outFormat: p.tag
					});
					continue;
				}

				const samples = await timedGroup(p.specs.map((s) => () => void once(s)()));

				p.specs.forEach((s, i) => {
					const r = i === 0 ? { ratio: 1, min: 1, max: 1 } : ratioOf(samples, i);

					emit({
						...baseOf(e),
						key: `${prefix}${p.group}|${p.tag}|${s.variant}`,
						kind: 'tier',
						load: loadavg()[0]!,
						ms: median(samples[i]!),
						min: Math.min(...samples[i]!),
						max: Math.max(...samples[i]!),
						group: p.group,
						outFormat: p.format,
						tag: p.tag,
						variant: s.variant,
						base: p.specs[0]!.variant,
						quality: s.quality,
						lossless: s.lossless,
						width: raw.width,
						height: raw.height,
						channels: imageChannels(p, s),
						ratio: r.ratio,
						ratioMin: r.min,
						ratioMax: r.max,
						outBytes: outBytes[i]!
					});
				});
			}
		} finally {
			for (const image of images.values()) {
				exp.tiny_image_destroy(image);
				tinyimg.free(image);
			}
			tinyimg.free(opts);
			tinyimg.free(writer);
		}
	}
}

function imageChannels(p: { input: string }, s: Spec): number {
	return p.input === 'rgb' || s.variant === 'rgb' ? 3 : 4;
}

// #endregion

// #region control

let controlFloor = 0;
const runStamp = new Date().toISOString().slice(0, 19);

/** A fixed decode, re-timed during the sweep; waits and retries when it drifts. */
async function control(bytes: Uint8Array<ArrayBuffer>, label: string): Promise<void> {
	const e = { id: 'control', format: 'jpeg', width: 1835, height: 1032 } as Entry;
	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(exp.tiny_image_sizeof());

	try {
		for (let attempt = 0; attempt < 6; attempt++) {
			const t = await timed(() => void decodeOnce(bytes, buffer, image, e, 1, null));

			if (controlFloor === 0 || t.ms < controlFloor) controlFloor = t.ms;
			const drift = t.ms / controlFloor;

			emit({
				key: `control|${runStamp}|${label}|${attempt}`,
				kind: 'control',
				id: 'sf-24.jpg',
				sha8: '',
				fit: false,
				format: 'jpeg',
				load: loadavg()[0]!,
				...t,
				drift
			});

			if (drift < CONTROL_DRIFT) return;
			console.error(`control drifted ${drift.toFixed(2)}x at ${label}; waiting`);
			await sleep(60000);
		}
	} finally {
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

// #endregion

// #region main

function finish(): void {
	const cells = readFileSync(partialPath, 'utf8')
		.split('\n')
		.filter((l) => l.startsWith('{'))
		.map((l) => JSON.parse(l) as Cell);

	const byKey = new Map<string, Cell>();
	let base: { meta: Record<string, unknown>; runs?: unknown[]; cells: Cell[] } | null = null;

	if (baseCells) {
		base = JSON.parse(readFileSync(resolve(ROOT, baseCells), 'utf8'));
		for (const c of base!.cells) byKey.set(c.key, c);
	}
	for (const c of cells) byKey.set(c.key, c);

	const rows = [...byKey.values()].sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
	const out = resolve(ROOT, flag('--out', join('bench', 'baselines', 'fit', 'cells.json')));
	const metas = readFileSync(`${partialPath}.meta`, 'utf8')
		.split('\n')
		.filter((l) => l.startsWith('{'))
		.map((l) => JSON.parse(l) as Record<string, unknown>);

	const meta = base ? base.meta : metas[0]!;
	const runs = [...(base?.runs ?? []), ...(base ? metas : metas.slice(1))];

	writeFileSync(out, `${JSON.stringify({ meta, runs, cells: rows }, null, '\t')}\n`);
	console.log(`wrote ${rows.length} cells to ${out}`);
}

if (args.includes('--finish')) {
	finish();
	process.exit(0);
}

await waitForLoad();

const meta = {
	wasmSha256: wasmSha,
	machine: `${platform()}-${arch()}, ${cpus()[0]?.model ?? 'unknown cpu'}, ${cores} cores`,
	loadAtStart: loadavg()[0]!,
	loadLimit,
	startedAt: new Date().toISOString(),
	runs: RUNS,
	warmups: WARMUPS,
	stages: [...only].join(','),
	entries: entrySet
};
appendFileSync(`${partialPath}.meta`, `${JSON.stringify(meta)}\n`);
console.log(`${meta.machine}, load ${meta.loadAtStart.toFixed(2)}, wasm ${wasmSha.slice(0, 12)}`);

const controlBytes = (() => {
	const file = readFileSync(join(ROOT, 'tests', 'fixtures', 'sf-24.jpg'));
	const bytes = new Uint8Array(file.byteLength);
	bytes.set(file);
	return bytes;
})();

const idFilter = flag('--ids', '').split(',').filter(Boolean);
const entries = readManifest()
	.filter((e) => idFilter.length === 0 || idFilter.some((id) => e.id.includes(id)))
	.slice(0, limit > 0 ? limit : undefined);
let index = 0;

for (const e of entries) {
	if (index % CONTROL_EVERY === 0) await control(controlBytes, `entry${index}`);

	const bytes = load(e);
	const probed = await tinyimg.probe(bytes);
	info = { channels: probed.channels, hasAlpha: probed.hasAlpha };
	const t0 = performance.now();

	try {
		if (only.has('decode')) await decodeCells(e, bytes);
		if (only.has('encode') && !e.derived) await encodeCells(e, bytes);
		if (only.has('effort') && !e.derived) await effortCells(e, bytes);
		if (only.has('effortdec')) await effortDecodeCells(e, bytes);
		if (only.has('tier') && !e.derived) await tierCells(e, bytes);
		if (only.has('whole') && !e.derived && fitOf(e.sha256) && e.width * e.height <= 1_200_000) {
			await wholeCell(e, bytes, index);
		}
	} catch (error) {
		console.error(`${e.id}: ${(error as Error).message}`);
	}

	console.log(
		`${String(index + 1).padStart(3)}/${entries.length} ${e.id} ${e.format} ` +
			`${((performance.now() - t0) / 1000).toFixed(1)} s load ${loadavg()[0]!.toFixed(2)}`
	);
	index++;
}

await control(controlBytes, 'end');
console.log('SWEEP_DONE');

// #endregion
