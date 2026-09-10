/**
 * Measures the rates the planner's cost estimate is built from.
 *
 * ```sh
 * bun scripts/measure/calibrate.ts
 * ```
 *
 * The estimate answers "will this request fit a CPU budget", so its constants have to come from
 * the module that ships, timed in a JavaScript runtime rather than from the native build. Each
 * rate is measured in isolation instead of fitted across a mixed workload, because a fit spreads
 * one stage's error over every other stage's constant.
 *
 * Prints the constants for `src/plan.c`. They are a rate per unit of counted work, so a machine
 * twice as fast wants every one of them halved and nothing else changes.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { ImageFormat } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';

const ROOT = join(import.meta.dirname, '..', '..');
const RUNS = 15;

const tinyimg = await TinyImgModule.load(
	await WebAssembly.compile(readFileSync(join(ROOT, 'bin', 'tinyimg.wasm')))
);

function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(ROOT, 'tests', 'fixtures', name));
	const bytes = new Uint8Array(buffer.byteLength);

	bytes.set(buffer);
	return bytes;
}

function median(values: number[]): number {
	return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]!;
}

async function time(body: () => unknown | Promise<unknown>): Promise<number> {
	const samples: number[] = [];

	for (let i = 0; i < RUNS; i++) {
		const start = performance.now();
		await body();
		samples.push(performance.now() - start);
	}

	return median(samples);
}

const source = fixture('sf-24.jpg');

// #region decode

/*
 * Two scales give two equations. An eighth writes one sample per block, so it is almost all
 * entropy; a full decode writes 64, so the difference is the transform. Solving the pair separates
 * a rate that a scale cannot lower from one it can.
 */
/** A pure decode at a chosen scale, with no plan and no sampler in the way. */
function decodeScaled(bytes: Uint8Array, den: number): void {
	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(tinyimg.exports.tiny_image_sizeof());

	try {
		const code = tinyimg.exports.tiny_image_load_scaled(
			image,
			buffer,
			bytes.byteLength,
			Math.ceil(1835 / den),
			Math.ceil(1032 / den)
		);

		if (code !== 0) throw new Error(`decode at 1/${den} failed with ${code}`);
		tinyimg.exports.tiny_image_destroy(image);
	} finally {
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

const decode = async (den: number) => {
	const ms = await time(() => decodeScaled(source, den));
	const { work } = await tinyimg.measure(() => decodeScaled(source, den));

	return { ms, work };
};

const eighth = await decode(8);
const full = await decode(1);

/*
 * In samples rather than blocks, because that is what the planner has before it decodes: it knows
 * the source extent and the extent it is about to ask for, and not how a codec divides either one.
 *
 * The stream rate covers everything a scale cannot lower, which for JPEG is the entropy decode of
 * every coefficient; the sample rate covers what it can.
 */
const streamRate =
	((eighth.ms * full.work.decodedSamples - full.ms * eighth.work.decodedSamples) * 1000) /
	(full.work.decodedSamples - eighth.work.decodedSamples) /
	full.work.sourceSamples;

const sampleRate =
	((full.ms - eighth.ms) * 1000) / (full.work.decodedSamples - eighth.work.decodedSamples);

console.log('decode');
for (const arm of [eighth, full]) {
	console.log(
		`  ${arm.ms.toFixed(2).padStart(6)} ms  source ${arm.work.sourceSamples}  ` +
			`decoded ${arm.work.decodedSamples}  blocks ${arm.work.blocks}`
	);
}

console.log(`  stream ${streamRate.toFixed(6)} us per source sample`);
console.log(`  sample ${sampleRate.toFixed(6)} us per decoded sample`);

/*
 * A two term model in source and decoded samples under-predicts the middle scales by about a
 * quarter, because the reduced transform's cost follows the number of one dimensional passes a
 * block takes rather than the samples they write: a quarter scale block still runs eight column
 * passes to write four samples per row. Rather than invent a third term, the scale is an
 * empirical factor against the full decode, which is exact at all four points and needs nothing
 * the planner does not already know.
 */
console.log('\n  scale factors against a full decode');
const factors: Record<number, number> = { 1: 1 };

for (const den of [2, 4, 8]) {
	const arm = den === 8 ? eighth : await decode(den);

	factors[den] = arm.ms / full.ms;
	console.log(
		`    1/${den}  ${arm.ms.toFixed(2)} ms  factor ${factors[den]!.toFixed(4)}  ` +
			`(a two term model would have said ${(
				((arm.work.sourceSamples * streamRate + arm.work.decodedSamples * sampleRate) /
					1000 /
					full.ms) as number
			).toFixed(4)})`
	);
}

// #endregion

// #region decode by format

/*
 * Every format at its own full-scale rate, since a JPEG-derived constant would misprice the others
 * by more than the scale factors do.
 */
console.log('\ndecode by format, us per source sample at full scale');

/*
 * Re-encoded from the reference photograph rather than taken from the 320x180 fixtures, because at
 * that extent a per-call overhead of a fraction of a millisecond doubles the apparent per-sample
 * rate and PNG comes out looking dearer to decode than JPEG.
 */
const samples = 1835 * 1032;

for (const format of ['jpeg', 'png', 'webp', 'gif', 'tiff', 'bmp', 'avif'] as ImageFormat[]) {
	try {
		using original = await Image.open(tinyimg, source);
		const encoded = await original.bytes(format, { quality: 80 });

		const ms = await time(() => tinyimg.decode(encoded));

		console.log(
			`  ${format.padEnd(5)} ${ms.toFixed(2).padStart(6)} ms  ` +
				`${((ms * 1000) / samples).toFixed(6)}`
		);
	} catch (error) {
		console.log(`  ${format.padEnd(5)} unavailable: ${(error as Error).message}`);
	}
}

// #endregion

// #region scale factors by format

/*
 * The scale factors above are JPEG's, measured on the reference photograph, and the planner applied
 * them to every format. Only a codec whose decode actually shrinks earns them: JPEG works in the
 * DCT domain and BMP skips rows, while the rest decode the whole frame and reduce afterwards.
 *
 * Each format is re-encoded from the same photograph so the extent and the content are constant and
 * the only variable is the codec. A factor near 1.0 means a scaled request buys that format
 * nothing.
 */
console.log('\nscale factors by format, against that format at full scale');

const scaleWidth = 1835;
const scaleHeight = 1032;

function loadScaled(bytes: Uint8Array, den: number): void {
	const buffer = tinyimg.copyIn(bytes);
	const image = tinyimg.alloc(tinyimg.exports.tiny_image_sizeof());

	try {
		const code = tinyimg.exports.tiny_image_load_scaled(
			image,
			buffer,
			bytes.byteLength,
			Math.ceil(scaleWidth / den),
			Math.ceil(scaleHeight / den)
		);

		if (code !== 0) throw new Error(`decode at 1/${den} failed with ${code}`);
		tinyimg.exports.tiny_image_destroy(image);
	} finally {
		tinyimg.free(image);
		tinyimg.free(buffer);
	}
}

/*
 * Five interleaved repeats rather than one pass per format, and the spread is printed beside the
 * median. A single pass swung these by up to 1.6x between runs, which is wide enough to invert the
 * ordering of two formats, so a constant taken from one pass would be noise wearing a decimal
 * point. Interleaving spreads any drift over every cell instead of over whichever format ran late.
 */
const REPEATS = 5;
const scaleFormats = ['jpeg', 'bmp', 'png', 'webp', 'gif', 'tiff', 'avif'] as ImageFormat[];
const encodedAt = new Map<ImageFormat, Uint8Array>();

for (const format of scaleFormats) {
	try {
		using original = await Image.open(tinyimg, source);
		encodedAt.set(format, await original.bytes(format, { quality: 80 }));
	} catch (error) {
		console.log(`  ${format.padEnd(5)} unavailable: ${(error as Error).message}`);
	}
}

const observed = new Map<string, number[]>();

for (let repeat = 0; repeat < REPEATS; repeat++) {
	for (const [format, encoded] of encodedAt) {
		const full = await time(() => loadScaled(encoded, 1));

		for (const den of [2, 4, 8]) {
			const key = `${format}/${den}`;
			const factor = (await time(() => loadScaled(encoded, den))) / full;

			observed.set(key, [...(observed.get(key) ?? []), factor]);
		}
	}
}

for (const format of scaleFormats) {
	if (!encodedAt.has(format)) continue;

	const cells = [2, 4, 8].map((den) => {
		const samples = observed.get(`${format}/${den}`)!;
		const low = Math.min(...samples);
		const high = Math.max(...samples);

		return `1/${den} ${median(samples).toFixed(3)} [${low.toFixed(2)}-${high.toFixed(2)}]`;
	});

	console.log(`  ${format.padEnd(5)} ${cells.join('  ')}`);
}

// #endregion

// #region effort by denominator

/*
 * What FAST takes off a decode, per denominator.
 *
 * The planner multiplies one effort fraction by one scale fraction, which assumes the two levers
 * are independent. For WebP they are not: FAST reduces in the plane domain, so its saving grows
 * with the denominator, and a single fraction taken at full scale over-charges a scaled FAST
 * request by more than the band allows.
 */
console.log('\neffort fraction by denominator, fast against fancy at the same scale');

/*
 * Driven through the planner rather than a raw decode, because that is the path the model predicts
 * and no options-taking decode is exported to the wrapper. The widths are the largest the strict
 * denominator rule maps to each `den`, so asking for 458 pixels of an 1835 pixel source is what
 * selects a quarter scale decode.
 */
const widthFor: Record<number, number> = { 1: 1835, 2: 917, 4: 458, 8: 229 };

const atEffort = async (bytes: Uint8Array, width: number, effort: 'fancy' | 'fast') =>
	await time(async () => {
		using image = await Image.open(tinyimg, bytes);
		image.resize(width, 0).effort(effort);
		await image.pixels();
	});

for (const format of ['jpeg', 'webp'] as ImageFormat[]) {
	const bytes = encodedAt.get(format);
	if (!bytes) continue;

	const cells: string[] = [];

	for (const den of [1, 2, 4, 8]) {
		const width = widthFor[den]!;
		const fancy = await atEffort(bytes, width, 'fancy');
		const fast = await atEffort(bytes, width, 'fast');

		cells.push(`1/${den} ${(fast / fancy).toFixed(3)}`);
	}

	console.log(`  ${format.padEnd(5)} ${cells.join('  ')}`);
}

/*
 * JPEG's row is uninformative on this source and that is a property of the file, not the lever.
 * The reference photograph is 4:4:4, so there is no chroma to replicate and FAST has nothing to
 * drop; the ratios wander either side of 1.0 by up to 1.5x, which is the noise floor rather than a
 * measurement. JPEG's constant comes from a subsampled fixture.
 */

// #endregion

// #region compressed bytes

/*
 * Whether a per-byte term is needed on top of a per-sample one.
 *
 * A sequential inflate is proportional to the compressed stream and nothing about the output
 * touches it, so two PNGs of one extent and very different compressed sizes should cost differently
 * if the term matters. Three contents at a fixed extent hold the sample count constant and vary
 * only the bytes.
 */
console.log('\ncompressed bytes at a fixed extent, png');

const extent = '1200x800';

function pngOf(recipe: string[]): Uint8Array<ArrayBuffer> {
	const out = join(tmpdir(), `calibrate-${recipe.join('-').replace(/[^a-z0-9]/gi, '')}.png`);

	execFileSync('magick', [...recipe, '-depth', '8', `PNG24:${out}`]);

	const buffer = readFileSync(out);
	const bytes = new Uint8Array(buffer.byteLength);

	bytes.set(buffer);
	return bytes;
}

const contents = {
	flat: pngOf(['-size', extent, 'xc:rgb(128,96,64)']),
	gradient: pngOf(['-size', extent, 'gradient:red-blue']),
	photo: pngOf([join(ROOT, 'tests', 'fixtures', 'sf-24.jpg'), '-resize', `${extent}!`]),
	noise: pngOf(['-size', extent, 'xc:gray', '+noise', 'Random'])
};

for (const [name, encoded] of Object.entries(contents)) {
	const ms = await time(() => tinyimg.decode(encoded));
	const perSample = (ms * 1000) / (1200 * 800);
	const perByte = (ms * 1000) / encoded.byteLength;

	console.log(
		`  ${name.padEnd(9)} ${encoded.byteLength.toString().padStart(8)} bytes  ` +
			`${ms.toFixed(2).padStart(6)} ms  ${perSample.toFixed(4)} us/sample  ` +
			`${perByte.toFixed(4)} us/byte`
	);
}

// #endregion

// #region resample

/*
 * Against a decode of the same extent, so the difference is the sampler rather than the decode it
 * had to do first. Catmull-Rom is the expensive filter and box the cheap one; both are reported,
 * because the choice is one of the few a budget can actually make.
 */
const resample = async (filter: 'box' | 'bilinear' | 'nearest' | 'catmull-rom') => {
	const ms = await time(async () => {
		using image = await Image.open(tinyimg, source);
		image.resize(400, 0, filter);
		await image.pixels();
	});

	const { work } = await tinyimg.measure(async () => {
		using image = await Image.open(tinyimg, source);
		image.resize(400, 0, filter);
		await image.pixels();
	});

	return { ms, work };
};

const arms = {
	nearest: await resample('nearest'),
	box: await resample('box'),
	bilinear: await resample('bilinear'),
	'catmull-rom': await resample('catmull-rom')
};

const decodeCost = (work: { sourceSamples: number; decodedSamples: number }) =>
	(work.sourceSamples * streamRate + work.decodedSamples * sampleRate) / 1000;

console.log('\nresample, us per output sample');

for (const [name, arm] of Object.entries(arms)) {
	const rate = ((arm.ms - decodeCost(arm.work)) * 1000) / arm.work.resampled;
	console.log(`  ${name.padEnd(12)} ${arm.ms.toFixed(2).padStart(6)} ms  ${rate.toFixed(4)}`);
}

// #endregion

// #region operations

/*
 * A color operation fuses into the sampler and a neighborhood one cannot, so they are priced
 * separately. Against the same plan with no operation on it.
 */
console.log('\noperations at 400 wide, us per output sample');

const plain = await time(async () => {
	using image = await Image.open(tinyimg, source);
	image.resize(400, 0);
	await image.pixels();
});

const outputSamples = 400 * 225;

for (const [name, build] of [
	['one color', (i: Image) => i.brightness(1.2)],
	['four color', (i: Image) => i.brightness(1.2).contrast(1.1).saturation(0.9).gamma(1.1)],
	['blur 4', (i: Image) => i.blur(4)],
	['sharpen', (i: Image) => i.sharpen(1)]
] as [string, (i: Image) => Image][]) {
	const ms = await time(async () => {
		using image = await Image.open(tinyimg, source);
		build(image.resize(400, 0));
		await image.pixels();
	});

	console.log(
		`  ${name.padEnd(12)} ${ms.toFixed(2).padStart(6)} ms  ` +
			`${(((ms - plain) * 1000) / outputSamples).toFixed(4)}`
	);
}

// #endregion

// #region encode

/*
 * Timed against the same plan with no encoder, so the difference is the encoder alone. Measured at
 * two extents to check the rate is per sample rather than per call.
 */
console.log('\nencode');

const bare = await time(async () => {
	using image = await Image.open(tinyimg, source);
	image.resize(400, 0);
	await image.pixels();
});

for (const format of ['jpeg', 'webp', 'png', 'bmp', 'gif', 'tiff'] as ImageFormat[]) {
	const rates: number[] = [];

	for (const width of [200, 400]) {
		const withEncoder = await time(async () => {
			using image = await Image.open(tinyimg, source);
			image.resize(width, 0);
			await image.bytes(format, { quality: 80 });
		});

		const withoutEncoder = await time(async () => {
			using image = await Image.open(tinyimg, source);
			image.resize(width, 0);
			await image.pixels();
		});

		const samples = width * Math.round((width * 1032) / 1835);
		rates.push(((withEncoder - withoutEncoder) * 1000) / samples);
	}

	console.log(
		`  ${format.padEnd(5)} ${rates[0]!.toFixed(4)} us/sample at 200 wide, ` +
			`${rates[1]!.toFixed(4)} at 400 wide`
	);
}

console.log(`\n(a plan to 400 wide with no encoder is ${bare.toFixed(2)} ms)`);

// #endregion
