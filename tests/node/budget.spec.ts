import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { crc32, deflateSync } from 'node:zlib';
import { beforeAll, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import {
	Err,
	Image,
	TinyImgBudgetError,
	TinyImgModule,
	artifactKey,
	transform
} from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(fixtures, name));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

/**
 * The cost estimate, and the choice it makes possible.
 *
 * The estimate exists to answer whether a request will fit a CPU limit before spending any of it,
 * so what has to be tested is that it tracks reality. An estimate nothing checks against a clock is
 * arithmetic dressed as a measurement.
 *
 * **Every assertion here is machine independent, and the clock is deliberately absent.** These run
 * on whatever machine CI gives them while the rates were calibrated on one developer machine, so an
 * estimate compared against a live clock fails on a runner of a different speed rather than when
 * the model is wrong: CI timed a 200 px transform at 12.33 ms against this machine's 4.89, and the
 * estimate that is right here read 3.2x under there. Widening the band far enough to survive that
 * left it passing a mutation that flattened the resample term, which is a check that covers nothing.
 *
 * What is gated instead is the model's shape, which does not depend on speed: which stage costs more
 * than which, and which rung the ladder picks. Those catch model breakage; flattening the scale
 * fraction fails three of them.
 *
 * The absolute comparison is a release step, `bun run eval:estimate`, which holds a 0.6x to 2.0x
 * band, names the machine it passed on and refuses to run under load.
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

	it('prices the encoders apart, which is what makes a budget actionable', () => {
		const wide = 800;
		const high = 450;

		const cost = (format: number) =>
			tinyimg.exports.tiny_encode_cost(format, wide, high) / 1000;

		const jpeg = cost(2);
		const webp = cost(6);
		const png = cost(1);

		expect(jpeg).toBeGreaterThan(0);

		// measured on the corpus: webp is about 2.2x jpeg per sample and png about 18x
		expect(webp / jpeg).toBeGreaterThan(2);
		expect(png / jpeg).toBeGreaterThan(10);

		// AVIF is priced since the corpus fit; HEIF is the format this build cannot write
		expect(tinyimg.exports.tiny_encode_cost(7, wide, high)).toBeGreaterThan(0);
		expect(tinyimg.exports.tiny_encode_cost(8, wide, high)).toBe(0);
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

describe('a budget degrading the request', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('changes nothing when the request already fits', async () => {
		const out = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 400,
			format: 'jpeg',
			budgetMs: 1000
		});

		expect(out.degraded).toEqual([]);
	});

	it('reports nothing on a pass-through, which needs no lever', async () => {
		const source = fixture('derived/base.png');
		const out = await transform(tinyimg, source, { width: 10000, budgetMs: 0.001 });

		expect(out.degraded).toEqual([]);
		expect(out.data).toEqual(source);
	});

	it('gives up the effort tier first, and says so', async () => {
		using probe = await Image.open(tinyimg, fixture('toyota_racing.webp'));
		probe.resize(500, 0);

		const undegraded = probe.decide().estimateMs;
		const encodeMs = tinyimg.exports.tiny_encode_cost(2, 500, 333) / 1000;

		const out = await transform(tinyimg, fixture('toyota_racing.webp'), {
			width: 500,
			format: 'jpeg',
			budgetMs: undegraded + encodeMs - 0.001
		});

		expect(out.degraded).toEqual(['effort']);
		expect(out.width).toBe(500);
	});

	it('does not report the effort tier alone where JPEG decode gains nothing from it', async () => {
		using probe = await Image.open(tinyimg, fixture('mountains.jpg'));
		probe.resize(600, 0);

		const undegraded = probe.decide().estimateMs;
		const encodeMs = tinyimg.exports.tiny_encode_cost(2, 600, 400) / 1000;

		const out = await transform(tinyimg, fixture('mountains.jpg'), {
			width: 600,
			format: 'jpeg',
			budgetMs: undegraded + encodeMs - 0.001
		});

		expect(out.degraded).not.toEqual(['effort']);
		expect(out.degraded).toContain('filter');
		expect(out.width).toBe(600);
	});

	it('walks the whole ladder for a budget nothing can meet', async () => {
		const out = await transform(tinyimg, fixture('mountains.jpg'), {
			width: 600,
			format: 'jpeg',
			budgetMs: 0.001
		});

		// the extent is never what gets given up: a budget changes how the pixels are made
		expect(out.degraded).toEqual(['effort', 'filter', 'scale']);
		expect(out.width).toBe(600);
		expect(out.height).toBe(400);
	});

	it('degrades the plan through the image surface too', async () => {
		using image = await Image.open(tinyimg, fixture('mountains.jpg'));
		image.resize(600, 0);

		const before = image.decide();
		expect(before.degraded).toEqual([]);

		image.budget(0.001);
		const after = image.decide();

		expect(after.degraded).toEqual(['effort', 'filter', 'scale']);
		expect(after.estimateMs).toBeLessThan(before.estimateMs);
		expect(after.output).toEqual(before.output);

		// and removing it puts the estimate back exactly, which is what makes the budget a
		// property of the plan rather than a mutation of it
		image.budget(0);
		expect(image.decide()).toEqual(before);
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
		expect(work.entropyBytes).toBe(0);
	});

	it('reports the entropy bytes a decode consumed', async () => {
		const source = fixture('sf-24.jpg');

		const run = () => tinyimg.measure(() => transform(tinyimg, source, { width: 200 }));
		const first = (await run()).work;
		const second = (await run()).work;

		// a Huffman scan is most of a baseline file, and none of it can exceed the file
		expect(first.entropyBytes).toBeGreaterThan(source.byteLength * 0.8);
		expect(first.entropyBytes).toBeLessThanOrEqual(source.byteLength);
		expect(second).toEqual(first);
	});

	it('reports the match search a PNG encode did', async () => {
		const source = fixture('sf-24.jpg');

		const png = await tinyimg.measure(() =>
			transform(tinyimg, source, { width: 200, format: 'png' })
		);
		const jpeg = await tinyimg.measure(() => transform(tinyimg, source, { width: 200 }));

		expect(png.work.encodeProbes).toBeGreaterThan(0);
		expect(jpeg.work.encodeProbes).toBe(0);
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

/**
 * Compares two encoded outputs by digest rather than by bytes.
 *
 * `toEqual` on a typed array builds a structural diff element by element when it fails, which on a
 * 124 kB PNG took six minutes to report a mismatch that took milliseconds to find. A digest fails
 * in constant time and says the same thing.
 */
function same(a: Uint8Array, b: Uint8Array): boolean {
	if (a.length !== b.length) return false;

	for (let i = 0; i < a.length; i++) {
		if (a[i] !== b[i]) return false;
	}

	return true;
}

/**
 * The effort option, on the side of it a caller can see from here.
 *
 * The per-stage measurements live in `tests/c/core/effort.c`, which can hold a decode against
 * another decode. What belongs here is that one option reaches every stage and that the formats
 * with nothing to trade are left alone, since that is the contract the option is documented with.
 */
describe('effort', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.load(new WebAssembly.Module(wasm));
	});

	it('reaches the decode, which is a stage the encode option cannot', async () => {
		// a lossless re-encode, so the encoder cannot be the thing that differs: any difference in
		// the output pixels has to have come from the decode
		const source = fixture('toyota_racing.webp');

		const fancy = await transform(tinyimg, source, {
			width: 500,
			format: 'png',
			effort: 'fancy'
		});
		const fast = await transform(tinyimg, source, {
			width: 500,
			format: 'png',
			effort: 'fast'
		});

		expect(same(fast.bytes(), fancy.bytes())).toBe(false);
	});

	it('decodes a lossless source identically at either effort, having nothing to drop', async () => {
		// every step of a lossless decode is required to produce the defined pixels, so there is no
		// approximation available and both arms have to agree exactly.
		//
		// this compares the PIXELS rather than an encoded file, and the distinction is the point.
		// An earlier version of this test compared the bytes of a PNG re-encode, which conflated
		// two different questions: lossless DECODE has no effort lever, but lossless ENCODE has a
		// large one, because how hard the compressor searches is not constrained by the input. The
		// encoder's own behaviour is asserted separately below.
		//
		// 100 px is a reduction of all three, which is what isolates the decode: an enlargement
		// would also move the resample filter, and dartmouth.tiff at 250 wide is small enough that
		// a larger request would have measured that instead
		for (const name of ['forest.png', 'ball_kick.gif', 'dartmouth.tiff']) {
			const source = fixture(name);

			const fancy = await (await Image.open(tinyimg, source)).resize(100).pixels();
			const fast = await (
				await Image.open(tinyimg, source)
			)
				.resize(100)
				.effort('fast')
				.pixels();

			expect(same(fast.pixels, fancy.pixels), name).toBe(true);
		}
	});

	it('spends less on a lossless encode at fast, which is a separate lever', async () => {
		// the encoder compresses two candidate streams and keeps the smaller; fast keeps only the
		// adaptive one. On a photograph adaptive wins anyway, so the bytes are identical and the
		// saving is free; on flat artwork unfiltered was the winner and fast pays for it in size.
		// Measured 1.59x-1.74x at +0.0% on photographs, 1.28x-1.55x at +7.0% to +63.1% on artwork
		const photo = await (await Image.open(tinyimg, fixture('sf-24.jpg'))).pixels();
		expect(photo.width).toBe(1835);

		const artwork = fixture('derived/base-mono.gif');

		const fancy = await transform(tinyimg, artwork, { format: 'png', effort: 'fancy' });
		const fast = await transform(tinyimg, artwork, { format: 'png', effort: 'fast' });

		// a pass-through would make both arms the source, so this only means anything if the plan
		// actually re-encoded
		expect(fancy.bytes().byteLength).toBeGreaterThan(0);
		expect(same(fast.bytes(), fancy.bytes())).toBe(false);
	});

	it('does not change a pass-through, which decodes nothing to approximate', async () => {
		const source = fixture('sf-24.jpg');

		for (const effort of ['fancy', 'fast'] as const) {
			const out = await transform(tinyimg, source, { effort });
			const { work } = await tinyimg.measure(() => transform(tinyimg, source, { effort }));

			expect(out.bytes(), effort).toBe(source);
			expect(work.decodedSamples, effort).toBe(0);
		}
	});

	it('keeps the filter a caller named', async () => {
		const source = fixture('derived/base.png');

		/*
		 * Both extents, because `width` alone is an upper bound: 640 against a 320 wide source is
		 * already satisfied and passes through, so the one-extent form cannot reach an enlargement
		 * at all. `fit` with both is what asks for the extent exactly.
		 */
		const enlarge = { width: 640, height: 360, fit: 'cover' } as const;

		// effort decides what was left open; a named filter was not left open
		const named = await Promise.all(
			(['fancy', 'fast'] as const).map((effort) =>
				transform(tinyimg, source, {
					...enlarge,
					filter: 'catmull-rom',
					format: 'png',
					effort
				})
			)
		);

		expect(same(named[1]!.bytes(), named[0]!.bytes())).toBe(true);

		// and with the filter left to the library, enlarging is where the two diverge
		const auto = await Promise.all(
			(['fancy', 'fast'] as const).map((effort) =>
				transform(tinyimg, source, { ...enlarge, format: 'png', effort })
			)
		);

		expect(same(auto[1]!.bytes(), auto[0]!.bytes())).toBe(false);

		// a reduction already picks the cheap filter, so it cannot diverge
		const down = await Promise.all(
			(['fancy', 'fast'] as const).map((effort) =>
				transform(tinyimg, source, {
					width: 160,
					height: 90,
					fit: 'cover',
					format: 'png',
					effort
				})
			)
		);

		expect(same(down[1]!.bytes(), down[0]!.bytes())).toBe(true);
	});

	/*
	 * The compressor is the first thing a budget reaches on a lossless output, and it used to be the
	 * one thing it could not: the plan was handed `budgetMs - encodeMs`, which for PNG is already
	 * negative, so it floored at a microsecond and the planner then softened the picture to pay for
	 * a compressor it had no lever on.
	 */
	it('turns the compressor down before it softens the picture', async () => {
		const source = fixture('sf-24.jpg');
		const request = { width: 400, format: 'png' } as const;

		const free = await transform(tinyimg, source, request);

		expect(free.degraded).not.toContain('compression');

		// a budget the encoder alone cannot meet at its default level
		const tight = await transform(tinyimg, source, { ...request, budgetMs: 20 });

		expect(tight.degraded).toContain('compression');

		// naming a level pins it, and the planner degrades instead
		const pinned = await transform(tinyimg, source, {
			...request,
			budgetMs: 20,
			compression: 'best'
		});

		expect(pinned.degraded).not.toContain('compression');

		// a lossy output has no deflate stream, so the lever does not exist there
		const lossy = await transform(tinyimg, source, {
			width: 400,
			format: 'jpeg',
			budgetMs: 1
		});

		expect(lossy.degraded).not.toContain('compression');
	});

	/*
	 * A scaled decode is priced per format now, and PNG is the format that motivated it: the shared
	 * factors said a reduced PNG decode was 33% cheaper where it measures 62% dearer, so a budget
	 * took the rung expecting a saving that does not exist.
	 */
	it('does not treat a reduced png decode as a saving', async () => {
		using full = await Image.open(tinyimg, fixture('forest.png'));
		const whole = full.decide();

		using reduced = await Image.open(tinyimg, fixture('forest.png'));
		reduced.resize(160, 0);
		const thumbnail = reduced.decide();

		expect(thumbnail.scale).toBeGreaterThan(1);

		// the estimate for the reduced decode is not below the full one, which is the sign the
		// shared factors had backwards. The resample it saves is real but smaller than the
		// averaging pass a denominator above one adds
		expect(thumbnail.estimateMs).toBeGreaterThan(whole.estimateMs * 0.9);

		// and a JPEG of the same picture goes the other way, because its decode really does shrink
		using jpegFull = await Image.open(tinyimg, fixture('sf-24.jpg'));
		using jpegThumb = await Image.open(tinyimg, fixture('sf-24.jpg'));
		jpegThumb.resize(160, 0);

		expect(jpegThumb.decide().estimateMs).toBeLessThan(jpegFull.decide().estimateMs);
	});
});

/** An RGBA PNG whose alpha at each pixel comes from `alpha`, built with zlib and nothing else. */
function rgbaPng(
	width: number,
	height: number,
	alpha: (x: number, y: number) => number
): Uint8Array<ArrayBuffer> {
	const raw = Buffer.alloc((width * 4 + 1) * height);

	for (let y = 0; y < height; y++) {
		for (let x = 0; x < width; x++) {
			const at = y * (width * 4 + 1) + 1 + x * 4;

			raw[at] = (x * 255) / width;
			raw[at + 1] = (y * 255) / height;
			raw[at + 2] = 90;
			raw[at + 3] = alpha(x, y);
		}
	}

	const chunk = (tag: string, data: Buffer) => {
		const out = Buffer.alloc(12 + data.length);

		out.writeUInt32BE(data.length, 0);
		out.write(tag, 4, 'latin1');
		data.copy(out, 8);
		out.writeUInt32BE(
			crc32(Buffer.concat([Buffer.from(tag, 'latin1'), data])),
			8 + data.length
		);
		return out;
	};

	const header = Buffer.alloc(13);

	header.writeUInt32BE(width, 0);
	header.writeUInt32BE(height, 4);
	header.set([8, 6, 0, 0, 0], 8);

	const png = Buffer.concat([
		Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
		chunk('IHDR', header),
		chunk('IDAT', deflateSync(raw)),
		chunk('IEND', Buffer.alloc(0))
	]);

	const out = new Uint8Array(png.byteLength);

	out.set(png);
	return out;
}

/**
 * Transparency an output format cannot keep.
 *
 * The planner knows the channel count and not the values, so `degraded` names `'alpha'` only when
 * the pixels handed to the encoder carry some and the format writes less than all of it. WebP
 * writes all of it, so it never reports.
 */
describe('alpha the output format cannot carry', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.load(await WebAssembly.compile(wasm));
	});

	const ramp = () => rgbaPng(64, 32, (x) => Math.round((x * 255) / 63));
	const mask = () => rgbaPng(64, 32, (x, y) => ((x - 32) ** 2 + (y - 16) ** 2 < 150 ? 255 : 0));
	const opaque = () => rgbaPng(64, 32, () => 255);

	it.each(['jpeg', 'avif'] as const)(
		'reports it for partial alpha and a mask to %s',
		async (format) => {
			for (const source of [ramp(), mask()]) {
				const out = await transform(tinyimg, source, { format, quality: 75 });

				expect(out.degraded).toEqual(['alpha']);
			}
		}
	);

	it('reports partial alpha to gif, which keeps one bit of it, and not a mask', async () => {
		expect((await transform(tinyimg, ramp(), { format: 'gif' })).degraded).toEqual(['alpha']);
		expect((await transform(tinyimg, mask(), { format: 'gif' })).degraded).toEqual([]);
	});

	it('reports nothing for a format that carries alpha in full', async () => {
		for (const options of [
			{ format: 'webp', quality: 75 },
			{ format: 'webp', lossless: true },
			{ format: 'png' },
			{ format: 'tiff' },
			{ format: 'bmp' }
		] as const) {
			const out = await transform(tinyimg, ramp(), { width: 32, ...options });

			expect(out.degraded).toEqual([]);
		}
	});

	it('keeps the alpha of a lossy webp, in a frame that declares it', async () => {
		const out = await transform(tinyimg, ramp(), { format: 'webp', quality: 75 });
		const tag = (at: number) => new TextDecoder('latin1').decode(out.data.subarray(at, at + 4));

		expect(tag(12)).toBe('VP8X');
		expect(tag(30)).toBe('ALPH');

		using decoded = await Image.open(tinyimg, out.data);
		const raw = await decoded.pixels();

		expect(raw.channels).toBe(4);
		expect(raw.pixels[3]).toBe(0);
		expect(raw.pixels[63 * 4 + 3]).toBe(255);
	});

	it('reports nothing for an opaque RGBA source, and writes the plain frame', async () => {
		for (const format of ['jpeg', 'avif', 'gif', 'webp', 'png'] as const) {
			const out = await transform(tinyimg, opaque(), { format, quality: 75 });

			expect(out.degraded).toEqual([]);
		}

		const webp = await transform(tinyimg, opaque(), { format: 'webp', quality: 75 });

		expect(new TextDecoder('latin1').decode(webp.data.subarray(12, 16))).toBe('VP8 ');
	});

	it('looks at the output, so a crop onto the opaque part reports nothing', async () => {
		const source = rgbaPng(64, 32, (x) => (x < 32 ? x * 8 : 255));

		const whole = await transform(tinyimg, source, { format: 'jpeg' });
		const part = await transform(tinyimg, source, {
			format: 'jpeg',
			crop: { x: 40, y: 0, width: 20, height: 20 }
		});

		expect(whole.degraded).toEqual(['alpha']);
		expect(part.degraded).toEqual([]);
	});

	it('reports it from the encoder, with the plan run once', async () => {
		for (const format of ['jpeg', 'avif', 'gif'] as const) {
			const { value, work } = await tinyimg.measure(() =>
				transform(tinyimg, ramp(), { format, quality: 75 })
			);

			expect(value.degraded).toEqual(['alpha']);
			expect(work.passes).toBe(1);
			expect(work.alphaDropped).toBeGreaterThan(0);
		}

		const opaqueRun = await tinyimg.measure(() =>
			transform(tinyimg, opaque(), { format: 'jpeg' })
		);

		expect(opaqueRun.work.alphaDropped).toBe(0);
		expect(opaqueRun.work.passes).toBe(1);
	});

	it('does not report alpha that another request on the same module dropped', async () => {
		for (const lossyFirst of [true, false]) {
			const jobs = [
				transform(tinyimg, opaque(), { format: 'jpeg' }),
				transform(tinyimg, ramp(), { format: 'jpeg' })
			];
			if (lossyFirst) jobs.reverse();

			const done = await Promise.all(jobs);
			const clean = lossyFirst ? done[1]! : done[0]!;
			const lossy = lossyFirst ? done[0]! : done[1]!;

			expect(clean.degraded, `lossy first: ${lossyFirst}`).toEqual([]);
			expect(lossy.degraded, `lossy first: ${lossyFirst}`).toEqual(['alpha']);
		}
	});

	it('does not look when the source has no alpha channel', async () => {
		const out = await transform(tinyimg, fixture('sf-24.jpg'), { width: 64, format: 'avif' });

		expect(out.degraded).toEqual([]);
	});
});

/**
 * Admission on the calibrated bound, and the work guard behind it.
 *
 * Nothing here compares a timing: the bound and the work clock are both functions of the request,
 * so the assertions are about decisions, about bytes, and about which error comes out.
 */
describe('admission', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('admits a request whose bound fits, with no margin to spare at the bound itself', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const whole = image.admit(1000, { mode: 'strict', format: 'webp' });
		expect(whole.decision).toBe('admit');
		expect(whole.costUpperMs).toBeGreaterThan(0);
		expect(whole.costUpperMs).toBeGreaterThanOrEqual(whole.costMs);
		expect(whole.encodeUpperMs).toBeGreaterThan(0);
		expect(whole.degraded).toEqual([]);

		const exact = image.admit(whole.costUpperMs, { mode: 'strict', format: 'webp' });
		expect(exact.decision).toBe('admit');
		expect(exact.marginMs).toBe(0);

		const under = image.admit(whole.costUpperMs - 0.001, { mode: 'strict', format: 'webp' });
		expect(under.decision).toBe('reject');
		expect(under.marginMs).toBeCloseTo(-0.001, 6);
		expect(under.degraded).toEqual([]);
	});

	it('multiplies the bound by the platform factor', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const one = image.admit(1000, { mode: 'strict', format: 'webp' });
		const two = image.admit(1000, { mode: 'strict', format: 'webp', platformScale: 2 });

		expect(two.costUpperMs).toBeCloseTo(one.costUpperMs * 2, 3);
		expect(image.admit(one.costUpperMs, { mode: 'strict', format: 'webp' }).decision).toBe(
			'admit'
		);
		expect(
			image.admit(one.costUpperMs, { mode: 'strict', format: 'webp', platformScale: 2 })
				.decision
		).toBe('reject');
	});

	it('degrades a request to a rung whose bound fits, and rejects when none does', async () => {
		using image = await Image.open(tinyimg, fixture('mountains.jpg'));
		image.resize(600, 0);

		const whole = image.admit(1000, { format: 'webp' });
		let degraded = 0;
		let rejected = 0;

		for (let budget = whole.costUpperMs; budget > 0.05; budget *= 0.9) {
			const got = image.admit(budget, { format: 'webp' });

			if (got.decision === 'degraded') {
				degraded++;
				expect(got.costUpperMs).toBeLessThanOrEqual(budget);
				expect(got.degraded.length).toBeGreaterThan(0);
			} else if (got.decision === 'reject') {
				rejected++;
				expect(got.costUpperMs).toBeGreaterThan(budget);
			}
		}

		expect(degraded).toBeGreaterThan(0);
		expect(rejected).toBeGreaterThan(0);
	});

	it('throws from strict with the numbers, and gives nothing up', async () => {
		const error = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			format: 'webp',
			budgetMs: 0.01,
			admission: 'strict'
		}).catch((caught: unknown) => caught);

		expect(error).toBeInstanceOf(TinyImgBudgetError);

		const budget = error as TinyImgBudgetError;

		expect(budget.reason).toBe('bound');
		expect(budget.code).toBe(Err.budget);
		expect(budget.admission?.decision).toBe('reject');
		expect(budget.admission?.degraded).toEqual([]);
		expect(budget.admission!.costUpperMs).toBeGreaterThan(0.01);
		expect(budget.admission!.marginMs).toBeLessThan(0);
		expect(budget.message).toContain('budget');
	});

	it('returns the source bytes unchanged from passthrough when nothing fits', async () => {
		const source = fixture('sf-24.jpg');
		const out = await transform(tinyimg, source, {
			width: 200,
			budgetMs: 0.01,
			admission: 'passthrough'
		});

		expect(out.degraded).toEqual(['passthrough']);
		expect(out.format).toBe('jpeg');
		expect(Buffer.compare(Buffer.from(out.data), Buffer.from(source))).toBe(0);
	});

	it('refuses a passthrough that would change the container, and says why', async () => {
		const error = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			format: 'webp',
			budgetMs: 0.01,
			admission: 'passthrough'
		}).catch((caught: unknown) => caught);

		expect(error).toBeInstanceOf(TinyImgBudgetError);
		expect((error as Error).message).toContain('cannot pass the source through');
	});

	it('serves an affordable request the same bytes with or without a guard', async () => {
		const request = { width: 200, format: 'webp' } as const;
		const plain = await transform(tinyimg, fixture('sf-24.jpg'), request);

		for (const admission of ['strict', 'degrade', 'passthrough'] as const) {
			const guarded = await transform(tinyimg, fixture('sf-24.jpg'), {
				...request,
				budgetMs: 1000,
				admission
			});

			expect(guarded.degraded, admission).toEqual([]);
			expect(Buffer.compare(Buffer.from(guarded.data), Buffer.from(plain.data))).toBe(0);
		}
	});

	it('leaves a budget with no admission mode exactly as it was', async () => {
		const request = { width: 200, format: 'webp', budgetMs: 0.01 } as const;
		const { value, work } = await tinyimg.measure(() =>
			transform(tinyimg, fixture('sf-24.jpg'), request)
		);

		// over budget and still served, which is what a budget has always meant
		expect(value.data.byteLength).toBeGreaterThan(0);
		expect(value.degraded.length).toBeGreaterThan(0);
		expect(work.encoded).toBeGreaterThan(0);
	});

	it('stops an admitted-by-nothing request part way, and says it was the work counters', async () => {
		const error = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			format: 'webp',
			budgetMs: 0.01,
			admission: 'degrade'
		}).catch((caught: unknown) => caught);

		expect(error).toBeInstanceOf(TinyImgBudgetError);
		expect((error as TinyImgBudgetError).code).toBe(Err.budget);
		expect((error as TinyImgBudgetError).reason).toBe('work');
	});

	it('stops a decode and an encode through the module, whatever the format', async () => {
		for (const name of ['sf-24.jpg', 'mountains.jpg']) {
			using image = await Image.open(tinyimg, fixture(name));
			image.resize(200, 0);

			tinyimg.exports.tiny_work_set_limit(1);

			try {
				await expect(image.bytes('png')).rejects.toBeInstanceOf(TinyImgBudgetError);
			} finally {
				tinyimg.exports.tiny_work_set_limit(0);
			}

			// the limit is gone, and the same image runs to the end
			expect((await image.bytes('png')).byteLength).toBeGreaterThan(0);
		}
	});

	it('reads the same work clock for the same request', async () => {
		const run = () =>
			tinyimg.measure(() =>
				transform(tinyimg, fixture('sf-24.jpg'), { width: 200, format: 'webp' })
			);

		const first = await run();
		const second = await run();

		expect(first.work.clockMicros).toBeGreaterThan(0);
		expect(first.work.clockMicros).toBe(second.work.clockMicros);
		expect(first.work.decodeMicros).toBeGreaterThan(0);
		expect(first.work.clockMicros).toBeGreaterThan(first.work.decodeMicros);
	});

	it('keeps one request out of another on the same module', async () => {
		const aborted = transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			format: 'webp',
			budgetMs: 0.01,
			admission: 'degrade'
		});
		const fine = transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			format: 'webp',
			budgetMs: 1000,
			admission: 'degrade'
		});
		const plain = transform(tinyimg, fixture('sf-24.jpg'), { width: 200, format: 'webp' });

		const [stopped, kept, free] = await Promise.allSettled([aborted, fine, plain]);

		expect(stopped.status).toBe('rejected');
		expect(kept.status).toBe('fulfilled');
		expect(free.status).toBe('fulfilled');

		if (kept.status === 'fulfilled' && free.status === 'fulfilled') {
			expect(Buffer.compare(Buffer.from(kept.value.data), Buffer.from(free.value.data))).toBe(
				0
			);
		}
	});

	it('never trips the work guard on a request admitted at exactly its bound', async () => {
		for (const name of ['sf-24.jpg', 'toyota_racing.webp', 'drupflare.png', 'ball_kick.gif']) {
			for (const format of ['webp', 'png'] as const) {
				using image = await Image.open(tinyimg, fixture(name));
				image.autoOrient().resize(200, 0);

				const bound = image.admit(1000, { mode: 'strict', format }).costUpperMs;
				const result = await transform(tinyimg, fixture(name), {
					width: 200,
					format,
					budgetMs: bound,
					admission: 'passthrough'
				});

				expect(result.degraded, `${name} to ${format}`).not.toContain('passthrough');
			}
		}
	});
});

describe('selection', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('picks a feasible choice that is one of the candidates it looked at', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const picked = image.select();

		expect(picked.feasible).toBe(true);
		expect(picked.reason).toBe('none');
		expect(picked.considered.length).toBeGreaterThan(0);
		expect(picked.considered.length).toBeLessThanOrEqual(288);
		expect(picked.width).toBe(200);

		const choice = picked.choice!;

		expect(['jpeg', 'webp']).toContain(choice.format);
		expect(
			picked.considered.some(
				(c) =>
					c.reason === 'none' &&
					c.format === choice.format &&
					c.quality === choice.quality &&
					c.lossless === choice.lossless
			)
		).toBe(true);
	});

	it('answers the same twice, and reads no pixels', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const first = await tinyimg.measure(() => image.select());
		const again = image.select();

		expect(again).toEqual(first.value);
		expect(first.work.decodedSamples).toBe(0);
		expect(first.work.encoded).toBe(0);
	});

	it('names the check the nearest miss failed when nothing fits', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const none = image.select({ budgetMs: 0.001 });

		expect(none.feasible).toBe(false);
		expect(none.reason).toBe('budget');
		expect(none.choice).toBeUndefined();
		await expect(image.bytesFor(none)).rejects.toThrow(TypeError);
	});

	it('limits the output to the formats it is given', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const picked = image.select({ formats: ['png'] });

		expect(picked.choice?.format).toBe('png');
		expect(picked.considered.filter((c) => c.reason !== 'format')).toSatisfy((rows: any[]) =>
			rows.every((c) => c.format === 'png')
		);
	});

	it('holds a quality floor on the predicted figure', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const picked = image.select({ minDb: 40 });

		if (picked.feasible) expect(picked.choice!.db!).toBeGreaterThanOrEqual(40);
		else expect(picked.reason).toBe('quality');

		for (const c of picked.considered) {
			if (c.reason === 'none') expect(c.db!).toBeGreaterThanOrEqual(40);
		}
	});

	it('writes what it chose, at the extent it reported', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const picked = image.select({ formats: ['jpeg', 'webp'], objective: 'bytes' });
		const data = await image.bytesFor(picked);
		const info = await tinyimg.probe(data);

		expect(info.format).toBe(picked.choice!.format);
		expect(info.width).toBe(picked.width);
		expect(info.height).toBe(picked.height);
	});

	it('keeps operations added after the plan when it writes', async () => {
		using plain = await Image.open(tinyimg, fixture('sf-24.jpg'));
		using turned = await Image.open(tinyimg, fixture('sf-24.jpg'));

		plain.resize(200, 0);
		turned.resize(200, 0).rotate(90);

		const a = await tinyimg.probe(await plain.bytesFor(plain.select({ formats: ['jpeg'] })));
		const b = await tinyimg.probe(await turned.bytesFor(turned.select({ formats: ['jpeg'] })));

		expect(b.width).toBe(a.height);
		expect(b.height).toBe(a.width);
	});

	it('refuses an output that cannot carry alpha unless told the pixels are opaque', async () => {
		using image = await Image.open(tinyimg, fixture('drupflare.png'));
		image.resize(128, 0);

		const refused = image.select({ formats: ['jpeg'] });

		expect(refused.alpha).toBe(true);
		expect(refused.feasible).toBe(false);
		expect(refused.reason).toBe('alpha');

		const allowed = image.select({ formats: ['jpeg'], assumeOpaque: true });

		expect(allowed.feasible).toBe(true);
	});

	it('rejects an objective it does not know', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));

		expect(() => image.select({ objective: 'speed' as never })).toThrow(TypeError);
	});
});
