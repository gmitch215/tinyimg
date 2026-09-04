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

import { readFileSync } from 'node:fs';
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

for (const format of ['jpeg', 'png', 'webp', 'gif', 'tiff', 'bmp'] as ImageFormat[]) {
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
