import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import { Image, TinyImgModule, artifactKey, transform } from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(fixtures, name));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

function median(values: number[]): number {
	return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]!;
}

async function timed(body: () => Promise<unknown>): Promise<number> {
	// a few runs so a scheduling hiccup does not decide the assertion
	const samples: number[] = [];

	for (let i = 0; i < 9; i++) {
		const start = performance.now();
		await body();
		samples.push(performance.now() - start);
	}

	return median(samples);
}

/**
 * The cost estimate, and the choice it makes possible.
 *
 * The estimate exists to answer whether a request will fit a CPU limit before spending any of it,
 * so what has to be tested is that it tracks reality. An estimate nothing checks against a clock is
 * arithmetic dressed as a measurement.
 *
 * The tolerance is wide on purpose. These run on whatever machine CI gives them, and the rates were
 * calibrated on one developer machine, so the assertion is about the estimate being the right shape
 * and the right order of magnitude rather than about this runner's speed.
 */
describe('the cost estimate', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('rises with the output extent', async () => {
		const source = fixture('sf-24.jpg');
		const estimates: number[] = [];

		for (const width of [200, 400, 800]) {
			using image = await Image.open(tinyimg, source);
			image.resize(width, 0);

			estimates.push(image.decide().estimateMs);
		}

		expect(estimates[0]).toBeGreaterThan(0);
		expect(estimates[1]).toBeGreaterThan(estimates[0]!);
		expect(estimates[2]).toBeGreaterThan(estimates[1]!);
	});

	it('prices the expensive filter above the cheap one', async () => {
		const source = fixture('sf-24.jpg');

		using box = await Image.open(tinyimg, source);
		box.resize(600, 0, 'box');

		using cubic = await Image.open(tinyimg, source);
		cubic.resize(600, 0, 'catmull-rom');

		// measured at 5.3x per sample, so the estimate has to put them in that order
		expect(cubic.decide().estimateMs).toBeGreaterThan(box.decide().estimateMs);
	});

	it('charges for a neighborhood operation and almost nothing for a color one', async () => {
		const source = fixture('sf-24.jpg');

		using plain = await Image.open(tinyimg, source);
		plain.resize(400, 0);

		using colored = await Image.open(tinyimg, source);
		colored.resize(400, 0).brightness(1.2).contrast(1.1).saturation(0.9);

		using blurred = await Image.open(tinyimg, source);
		blurred.resize(400, 0).blur(4);

		const bare = plain.decide().estimateMs;
		const color = colored.decide().estimateMs;
		const blur = blurred.decide().estimateMs;

		// color fuses into the sampler, so three of them are close to free
		expect(color - bare).toBeLessThan(bare * 0.2);

		// a neighborhood operation cannot fuse, and the estimate has to say so
		expect(blur).toBeGreaterThan(color);
	});

	it('tracks a real transform within an order of magnitude', async () => {
		const source = fixture('sf-24.jpg');

		for (const width of [200, 400, 800]) {
			using image = await Image.open(tinyimg, source);
			image.resize(width, 0);

			const estimated = image.decide().estimateMs;
			const actual = await timed(async () => {
				using run = await Image.open(tinyimg, source);
				run.resize(width, 0);
				await run.pixels();
			});

			// the estimate is documented as accurate to about 20% on the machine it was calibrated
			// on; this asserts a factor of three either way, which is what survives an unknown
			// runner while still failing if the model is wrong about the shape of the work
			expect(estimated, `${width} wide`).toBeGreaterThan(actual / 3);
			expect(estimated, `${width} wide`).toBeLessThan(actual * 3);
		}
	});

	it('prices the encoders apart, which is what makes a budget actionable', () => {
		const wide = 800;
		const high = 450;

		const cost = (format: number) =>
			tinyimg.exports.tiny_encode_cost(format, wide, high) / 1000;

		const jpeg = cost(2);
		const webp = cost(6);
		const png = cost(1);

		expect(jpeg).toBeGreaterThan(0);

		// measured: webp is about 4x jpeg per sample and png about 28x
		expect(webp / jpeg).toBeGreaterThan(2);
		expect(png / jpeg).toBeGreaterThan(10);

		// a format this build cannot write has no price
		expect(tinyimg.exports.tiny_encode_cost(7, wide, high)).toBe(0);
	});
});

describe('a budget choosing the format', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('takes webp when nothing constrains it', async () => {
		const out = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 400,
			format: 'auto'
		});

		expect(out.format).toBe('webp');
	});

	it('falls back to jpeg when webp will not fit the budget', async () => {
		const source = fixture('sf-24.jpg');

		using probe = await Image.open(tinyimg, source);
		probe.resize(800, 0);

		const plan = probe.decide().estimateMs;
		const jpeg = tinyimg.exports.tiny_encode_cost(2, 800, 450) / 1000;
		const webp = tinyimg.exports.tiny_encode_cost(6, 800, 450) / 1000;

		// a budget between the two, so the choice is forced rather than incidental
		const budget = plan + (jpeg + webp) / 2;

		expect(plan + jpeg).toBeLessThan(budget);
		expect(plan + webp).toBeGreaterThan(budget);

		const out = await transform(tinyimg, source, {
			width: 800,
			format: 'auto',
			budgetMs: budget
		});

		expect(out.format).toBe('jpeg');
	});

	it('never substitutes a format the caller named, however far over budget', async () => {
		const out = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 800,
			format: 'webp',
			budgetMs: 0.001
		});

		// naming a format is a decision, and a budget does not overrule a decision
		expect(out.format).toBe('webp');
	});

	it('takes the cheapest candidate when nothing fits at all', async () => {
		const out = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 800,
			format: 'auto',
			budgetMs: 0.001
		});

		expect(out.format).toBe('jpeg');
	});
});

describe('the pass-through', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('hands back the exact source bytes when nothing is asked for', async () => {
		const source = fixture('sf-24.jpg');
		const out = await transform(tinyimg, source, {});

		// the same bytes, not merely an equivalent image
		expect(out.bytes()).toBe(source);
		expect([out.width, out.height]).toEqual([1835, 1032]);
		expect(out.format).toBe('jpeg');
	});

	it('passes through a bound the source is already inside', async () => {
		const source = fixture('derived/base-420.jpg');

		// 320x180, so a 400 wide bound asks for nothing
		expect((await transform(tinyimg, source, { width: 400 })).bytes()).toBe(source);
		expect((await transform(tinyimg, source, { height: 400 })).bytes()).toBe(source);
		expect((await transform(tinyimg, source, { width: 320 })).bytes()).toBe(source);
		expect((await transform(tinyimg, source, { format: 'jpeg' })).bytes()).toBe(source);
	});

	it('decodes rather than passing through whenever the request changes anything', async () => {
		const source = fixture('derived/base-420.jpg');

		const cases: [string, Parameters<typeof transform>[2]][] = [
			['a smaller bound', { width: 200 }],
			['both extents, which fit has to reach exactly', { width: 400, height: 400 }],
			['another container', { format: 'webp' }],
			['a quality, which means re-encoding', { quality: 60 }],
			['a crop', { crop: { x: 0, y: 0, width: 100, height: 100 } }],
			['a rotation', { rotate: 180 }],
			['a flip', { flip: 'horizontal' }],
			['a color operation', { brightness: 1.2 }],
			['grayscale, which is falsy-safe only if checked properly', { grayscale: true }],
			['a blur', { blur: 2 }],
			['a trim', { trim: true }],
			['stripping metadata', { metadata: 'none' }],
			['a device pixel ratio', { dpr: 2 }]
		];

		for (const [label, options] of cases) {
			const out = await transform(tinyimg, source, options);
			expect(out.bytes(), label).not.toBe(source);
		}
	});

	it('passes an animation through whole rather than decoding one frame of it', async () => {
		// the case that makes the pass-through worth more than the time it saves: a decode yields
		// frame one, so a request with nothing to do that went down that path would spend real work
		// turning 57 frames into a still. both container's animations, since only one has an encoder
		for (const name of ['ball_kick.gif', 'derived/base-animation.webp']) {
			const source = fixture(name);
			const before = await tinyimg.probe(source);

			expect(before.frames, name).toBeGreaterThan(1);

			const out = await transform(tinyimg, source, {});

			expect(out.bytes(), name).toBe(source);
			expect(out.flattened, name).toBe(false);
			expect((await tinyimg.probe(out.bytes())).frames, name).toBe(before.frames);
		}
	});

	it('says so when a request it cannot avoid flattens an animation', async () => {
		const source = fixture('ball_kick.gif');

		// a real resize has to decode, and there is no animated encoder, so this one is lossy in a
		// way a caller may want to route around rather than discover
		const out = await transform(tinyimg, source, { width: 200 });

		expect(out.flattened).toBe(true);
		expect((await tinyimg.probe(out.bytes())).frames).toBe(1);

		// and a still never claims it
		expect((await transform(tinyimg, fixture('sf-24.jpg'), { width: 200 })).flattened).toBe(
			false
		);
	});

	it('leaves a broken source to the decoder to report', async () => {
		// a pass-through that swallowed this would answer 200 with garbage
		await expect(
			transform(tinyimg, fixture('derived/malformed/not-an-image.bin'), {})
		).rejects.toThrow();
	});

	it('costs a header read rather than a decode', async () => {
		const source = fixture('sf-24.jpg');

		const { work } = await tinyimg.measure(() => transform(tinyimg, source, {}));

		// the counters are the proof: a pass-through decodes no pixels at all
		expect(work.decodedSamples).toBe(0);
		expect(work.transforms).toBe(0);
		expect(work.encoded).toBe(0);
	});
});

describe('the artifact key', () => {
	it('is the same for two requests that mean the same thing', async () => {
		const source = fixture('sf-24.jpg');

		// written in a different order, and one spells out what the other leaves absent
		const a = await artifactKey(source, { width: 400, format: 'webp', quality: 80 });
		const b = await artifactKey(source, { quality: 80, format: 'webp', width: 400 });

		expect(a).toBe(b);
	});

	it('differs whenever anything about the request differs', async () => {
		const source = fixture('sf-24.jpg');
		const base = await artifactKey(source, { width: 400, format: 'webp' });

		const others = await Promise.all([
			artifactKey(source, { width: 401, format: 'webp' }),
			artifactKey(source, { width: 400, format: 'jpeg' }),
			artifactKey(source, { width: 400, format: 'webp', quality: 80 }),
			artifactKey(source, { width: 400, format: 'webp', effort: 'fast' }),
			artifactKey(source, { width: 400 })
		]);

		for (const other of others) expect(other).not.toBe(base);
		expect(new Set(others).size).toBe(others.length);
	});

	it('identifies the source by its bytes, not by where it came from', async () => {
		const one = await artifactKey(fixture('sf-24.jpg'), { width: 400 });
		const same = await artifactKey(fixture('sf-24.jpg'), { width: 400 });
		const other = await artifactKey(fixture('mountains.jpg'), { width: 400 });

		expect(same).toBe(one);
		expect(other).not.toBe(one);
	});

	it('orders a nested option so two spellings of one crop agree', async () => {
		const source = fixture('sf-24.jpg');

		const a = await artifactKey(source, { crop: { x: 1, y: 2, width: 3, height: 4 } });
		const b = await artifactKey(source, { crop: { height: 4, width: 3, y: 2, x: 1 } });

		expect(a).toBe(b);
	});

	it('is a usable URL, which is what a cache takes as a key', async () => {
		const key = await artifactKey(fixture('sf-24.jpg'), { width: 400, format: 'webp' });

		expect(() => new URL(key)).not.toThrow();
		expect(new URL(key).searchParams.get('width')).toBe('400');
	});
});
