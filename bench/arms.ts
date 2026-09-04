/**
 * The comparator arms.
 *
 * Four questions, each of which needs its own arm because none of them is answerable from a single
 * set of timings:
 *
 * - **Codecs.** How does our decode and encode compare with a shipped libwebp/libjpeg/libpng built
 *   the usual way? `@jsquash` is emscripten-built libraries in the same runtime, which is the
 *   closest available like-for-like.
 * - **Operations.** What does each transformation cost per megapixel?
 * - **The planner.** What does fusion and the backward ROI walk actually save? Fusion off runs the
 *   same chain one operation per pass, which is the arm that says whether the planner earned its
 *   bytes.
 * - **SIMD.** What does `-msimd128` buy? Needs a second module, so it runs only when one was built.
 */

import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Image, TinyImgModule, transform } from '../src/ts/index.js';
import { measure, skip, type Budget, type Timing } from './harness.js';

const ROOT = join(import.meta.dirname, '..');
const FIXTURES = join(ROOT, 'tests', 'fixtures');

/** The reference fixture every throughput number is quoted on. */
export const REFERENCE = { name: 'sf-24.jpg', width: 1835, height: 1032 };

function fixture(...parts: string[]): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(FIXTURES, ...parts));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

/** Megapixels an extent covers. */
function megapixels(width: number, height: number): number {
	return (width * height) / 1e6;
}

/** Loads the module from a path, or reports why it could not. */
export async function loadModule(path: string): Promise<TinyImgModule | undefined> {
	if (!existsSync(path)) return undefined;
	return TinyImgModule.loadBytes(new Uint8Array(readFileSync(path)));
}

// #region codecs

/** What `@jsquash` exposes, loaded lazily so a missing arm does not stop the run. */
interface Squash {
	decode(bytes: ArrayBuffer): Promise<{ width: number; height: number; data: Uint8ClampedArray }>;
	encode(data: { data: Uint8ClampedArray; width: number; height: number }): Promise<ArrayBuffer>;
}

async function squash(format: 'jpeg' | 'png' | 'webp'): Promise<Squash | undefined> {
	try {
		const decode = (await import(`@jsquash/${format}/decode.js`)) as {
			default: Squash['decode'];
		};
		const encode = (await import(`@jsquash/${format}/encode.js`)) as {
			default: Squash['encode'];
		};

		return { decode: decode.default, encode: encode.default };
	} catch {
		return undefined;
	}
}

/**
 * Decode and encode, ours against a conventionally built library.
 *
 * The formats `@jsquash` covers, which is the three that matter. Anything it does not have is
 * reported as ours alone rather than dropped, because the per-megapixel number is worth having even
 * with nothing to compare it against.
 */
export async function codecArm(tinyimg: TinyImgModule, budget: Partial<Budget>): Promise<Timing[]> {
	const out: Timing[] = [];
	const source = fixture(REFERENCE.name);
	const pixels = megapixels(REFERENCE.width, REFERENCE.height);

	// one PNG of the same picture, so the PNG arms measure the same pixels
	const png = await transform(tinyimg, source, { format: 'png' });

	const cases = [
		{ format: 'jpeg' as const, bytes: source },
		{ format: 'png' as const, bytes: png.bytes() },
		{
			format: 'webp' as const,
			bytes: (await transform(tinyimg, source, { format: 'webp', quality: 80 })).bytes()
		}
	];

	for (const { format, bytes } of cases) {
		out.push(
			await measure(
				`decode ${format}`,
				'tinyimg',
				async () => (await tinyimg.decode(bytes)).pixels.byteLength,
				{ ...budget, bytesIn: bytes.byteLength, megapixels: pixels }
			)
		);

		// PNG is lossless, so a quality is not a thing it has and labeling one would invite a
		// comparison at an operating point neither encoder is at
		const label = format === 'png' ? 'encode png' : `encode ${format} q80`;

		out.push(
			await measure(
				label,
				'tinyimg',
				async () => (await transform(tinyimg, bytes, { format, quality: 80 })).bytes(),
				{ ...budget, bytesIn: bytes.byteLength, megapixels: pixels }
			)
		);

		const arm = await squash(format);

		if (!arm) {
			out.push(skip(`decode ${format}`, '@jsquash', 'not installed'));
			out.push(skip(label, '@jsquash', 'not installed'));
			continue;
		}

		const buffer = bytes.buffer.slice(0) as ArrayBuffer;
		const decoded = await arm.decode(buffer);

		out.push(
			await measure(
				`decode ${format}`,
				'@jsquash',
				() => arm.decode(bytes.buffer.slice(0) as ArrayBuffer),
				{
					...budget,
					bytesIn: bytes.byteLength,
					megapixels: pixels
				}
			)
		);

		out.push(
			await measure(label, '@jsquash', async () => (await arm.encode(decoded)).byteLength, {
				...budget,
				bytesIn: bytes.byteLength,
				megapixels: pixels
			})
		);
	}

	return out;
}

// #endregion

// #region operations

/** Every transformation, over the reference fixture, one at a time. */
export async function operationArm(
	tinyimg: TinyImgModule,
	budget: Partial<Budget>
): Promise<Timing[]> {
	const source = fixture(REFERENCE.name);
	const pixels = megapixels(REFERENCE.width, REFERENCE.height);

	const cases: [string, (image: Image) => void][] = [
		['decode only, no operations', () => {}],
		['resize bilinear to 400', (image) => image.resize(400, 0, 'bilinear')],
		['resize box to 400', (image) => image.resize(400, 0, 'box')],
		['resize catmull-rom to 400', (image) => image.resize(400, 0, 'catmull-rom')],
		['crop 500x500', (image) => image.crop(400, 200, 500, 500)],
		['rotate 180', (image) => image.rotate(180)],
		['flip horizontal', (image) => image.flip('horizontal')],
		['brightness', (image) => image.brightness(1.2)],
		['grayscale', (image) => image.grayscale()],
		['gamma', (image) => image.gamma(2.2)],
		['saturation', (image) => image.saturation(0.8)],
		['hue', (image) => image.hue(30)],
		[
			'four color operations',
			(image) => image.brightness(1.2).contrast(1.1).saturation(0.8).gamma(2.2)
		],
		['gaussian blur sigma 4', (image) => image.blur(4)],
		['sharpen', (image) => image.sharpen(1)]
	];

	const out: Timing[] = [];

	for (const [label, build] of cases) {
		out.push(
			await measure(
				label,
				'tinyimg',
				async () => {
					using image = await Image.open(tinyimg, source);
					build(image);

					// the run, not an encoder: encoding the result to BMP wrote 5.7 MB and cost
					// more than every operation here put together, so the column measured the
					// writer rather than the operation
					return (await image.pixels()).pixels.byteLength;
				},
				{ ...budget, bytesIn: source.byteLength, megapixels: pixels }
			)
		);
	}

	return out;
}

// #endregion

// #region planner

/**
 * The chain the planner exists for, with fusion on and off.
 *
 * The worked example from the plan: a 500x500 crop of a 1.9 megapixel photograph down to 100x100
 * with four color operations after it. Fusion off runs each operation as its own pass over its own
 * buffer, which is what the library would cost without the planner.
 */
export async function plannerArm(
	tinyimg: TinyImgModule,
	budget: Partial<Budget>
): Promise<Timing[]> {
	const source = fixture(REFERENCE.name);
	const pixels = megapixels(REFERENCE.width, REFERENCE.height);
	const out: Timing[] = [];

	const chain = (image: Image) =>
		image
			.crop(400, 200, 500, 500)
			.resize(100, 100)
			.brightness(1.2)
			.contrast(1.1)
			.saturation(0.8)
			.gamma(2.2);

	for (const fusion of [true, false]) {
		out.push(
			await measure(
				'crop, resize and four color operations',
				fusion ? 'planner on' : 'planner off',
				async () => {
					using image = await Image.open(tinyimg, source);
					chain(image);
					image.fusion(fusion);

					return (await image.pixels()).pixels.byteLength;
				},
				{ ...budget, bytesIn: source.byteLength, megapixels: pixels }
			)
		);
	}

	// and what the planner decided, which is the number the saving is actually visible in
	using image = await Image.open(tinyimg, source);
	chain(image);

	const decided = image.decide();

	out.push({
		label: `decode ${decided.region.width}x${decided.region.height} at 1/${decided.scale}, ${decided.passes} pass(es)`,
		arm: 'planner on',
		median: 0,
		p95: 0,
		best: 0,
		runs: 0,
		skipped: `${decided.eliminated} eliminated, ${decided.collapsed} collapsed, ${decided.colorStages} color stage(s)`
	});

	return out;
}

// #endregion

// #region simd

/** The same operations through a module built without `-msimd128`. */
export async function simdArm(simdless: TinyImgModule, budget: Partial<Budget>): Promise<Timing[]> {
	const source = fixture(REFERENCE.name);
	const pixels = megapixels(REFERENCE.width, REFERENCE.height);

	const cases: [string, (image: Image) => void][] = [
		['resize box to 400', (image) => image.resize(400, 0, 'box')],
		[
			'four color operations',
			(image) => image.brightness(1.2).contrast(1.1).saturation(0.8).gamma(2.2)
		],
		['gaussian blur sigma 4', (image) => image.blur(4)]
	];

	const out: Timing[] = [];

	for (const [label, build] of cases) {
		out.push(
			await measure(
				label,
				'simd off',
				async () => {
					using image = await Image.open(simdless, source);
					build(image);

					return (await image.pixels()).pixels.byteLength;
				},
				{ ...budget, bytesIn: source.byteLength, megapixels: pixels }
			)
		);
	}

	return out;
}

// #endregion
