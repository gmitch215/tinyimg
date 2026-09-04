import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Err, Filter, Fit, Gravity, TinyAbi } from '../support/abi.js';
import { golden, sha256 } from '../support/golden.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

/**
 * The planner driven through the raw exports.
 *
 * The ctest suite covers the arithmetic in far more detail than this does; what this lane adds is
 * that the same arithmetic runs in wasm, that a TinyPlan and a TinyPlanResolution can be allocated
 * and read across the boundary, and that the pixels match a fixed digest the workers lane checks
 * too. A planner that decided differently under one runtime would pass every structural assertion
 * and fail here.
 */
describe('the planner inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reserves the same bytes for a plan as the compiler chose', () => {
		expect(abi.exports.tiny_plan_sizeof()).toBeGreaterThan(0);
		expect(abi.exports.tiny_plan_resolution_sizeof()).toBeGreaterThan(0);

		// both are meant to sit on a host's stack or in one small allocation
		expect(abi.exports.tiny_plan_sizeof()).toBeLessThan(4096);
		expect(abi.exports.tiny_plan_resolution_sizeof()).toBeLessThan(4096);
	});

	it('asks the decoder for a rectangle at a scale, not for the whole image', () => {
		const { result, image, resolution } = abi.plan(fixture('dog.jpg'), (plan) => {
			abi.exports.tiny_plan_crop(plan, 2000, 1500, 500, 500);
			abi.exports.tiny_plan_resize(plan, 100, 100);
		});

		expect(result).toBe(Err.ok);
		expect(resolution).toBeDefined();

		// the worked example from the plan: a 500 pixel square at a quarter, so 15,625 pixels are
		// decoded out of six million
		expect(resolution!.decode).toEqual({
			x: 2000,
			y: 1500,
			width: 500,
			height: 500,
			scale: 4,
			channels: 0
		});
		expect(resolution!.decodeWidth).toBe(125);
		expect(resolution!.decodeHeight).toBe(125);

		expect(image!.width).toBe(100);
		expect(image!.height).toBe(100);
		expect(image!.channels).toBe(3);
	});

	it('does not scale the decode when the output is larger than the source', () => {
		const { result, resolution } = abi.plan(fixture('sf-24.jpg'), (plan) => {
			abi.exports.tiny_plan_resize(plan, 4000, 2250);
		});

		expect(result).toBe(Err.ok);
		expect(resolution!.decode.scale).toBe(1);
		expect(resolution!.decodeWidth).toBe(1835);
	});

	it('runs the worked chain to a fixed digest', async () => {
		const { result, image } = abi.plan(fixture('sf-24.jpg'), (plan) => {
			abi.exports.tiny_plan_crop(plan, 400, 200, 900, 600);
			abi.exports.tiny_plan_resize(plan, 300, 200);
			abi.exports.tiny_plan_brightness(plan, 1.2);
			abi.exports.tiny_plan_contrast(plan, 1.1);
			abi.exports.tiny_plan_saturation(plan, 0.8);
			abi.exports.tiny_plan_gamma(plan, 2.2);
		});

		expect(result).toBe(Err.ok);
		expect(image!.width).toBe(300);
		expect(image!.height).toBe(200);
		expect(await sha256(image!.pixels)).toBe(golden.planWorkedChain);
	});

	it('turns and mirrors without resampling', async () => {
		const { result, image } = abi.plan(fixture('sf-24.jpg'), (plan) => {
			abi.exports.tiny_plan_rotate(plan, 90);
			abi.exports.tiny_plan_flip_horizontal(plan);
		});

		expect(result).toBe(Err.ok);
		expect(image!.width).toBe(1032);
		expect(image!.height).toBe(1835);
		expect(await sha256(image!.pixels)).toBe(golden.planTurned);
	});

	it('eliminates an operation that changes nothing', async () => {
		const identity = abi.plan(fixture('sf-24.jpg'), (plan) => {
			abi.exports.tiny_plan_brightness(plan, 1.0);
			abi.exports.tiny_plan_rotate(plan, 360);
			abi.exports.tiny_plan_gamma(plan, 1.0);
			abi.exports.tiny_plan_flip_horizontal(plan);
			abi.exports.tiny_plan_flip_horizontal(plan);
		});

		const plain = abi.decode(fixture('sf-24.jpg'));

		expect(identity.result).toBe(Err.ok);
		expect(await sha256(identity.image!.pixels)).toBe(await sha256(plain.image!.pixels));
	});

	it('agrees with itself whether it fuses or not', () => {
		// the factors all reduce, so no intermediate can clamp and the only difference left is
		// rounding once against rounding after every operation
		const build = (plan: number) => {
			abi.exports.tiny_plan_crop(plan, 200, 100, 800, 600);
			abi.exports.tiny_plan_resize(plan, 200, 150);
			abi.exports.tiny_plan_brightness(plan, 0.9);
			abi.exports.tiny_plan_saturation(plan, 0.8);
		};

		const fused = abi.plan(fixture('sf-24.jpg'), build, 1);
		const eager = abi.plan(fixture('sf-24.jpg'), build, 0);

		expect(fused.result).toBe(Err.ok);
		expect(eager.result).toBe(Err.ok);
		expect(fused.image!.width).toBe(eager.image!.width);
		expect(fused.image!.height).toBe(eager.image!.height);

		let sum = 0;
		for (let i = 0; i < fused.image!.pixels.length; i++) {
			const d = fused.image!.pixels[i]! - eager.image!.pixels[i]!;
			sum += d * d;
		}

		const psnr = 10 * Math.log10((255 * 255) / (sum / fused.image!.pixels.length));
		expect(psnr).toBeGreaterThan(45);
	});

	it('produces the requested extent for every fit mode', () => {
		const expected: Record<number, [number, number]> = {
			[Fit.scaleDown]: [400, 225],
			[Fit.contain]: [400, 225],
			[Fit.cover]: [400, 400],
			[Fit.crop]: [400, 400],
			[Fit.pad]: [400, 400],
			[Fit.stretch]: [400, 400],
			[Fit.aspectCrop]: [400, 400]
		};

		for (const [mode, [width, height]] of Object.entries(expected)) {
			const { result, image } = abi.plan(fixture('sf-24.jpg'), (plan) => {
				abi.exports.tiny_plan_fit(plan, 400, 400, Number(mode), Gravity.center);
			});

			expect(result, `fit mode ${mode}`).toBe(Err.ok);
			expect([image!.width, image!.height], `fit mode ${mode}`).toEqual([width, height]);
		}
	});

	it('fills padding with the background it was given', () => {
		const color = abi.exports.tiny_alloc(4);
		new Uint8Array(abi.exports.memory.buffer).set([12, 34, 56, 255], color);

		const { result, image } = abi.plan(fixture('sf-24.jpg'), (plan) => {
			abi.exports.tiny_plan_background(plan, color);
			abi.exports.tiny_plan_fit(plan, 400, 400, Fit.pad, Gravity.center);
		});

		abi.exports.tiny_free(color);

		expect(result).toBe(Err.ok);
		expect(image!.width).toBe(400);
		expect(image!.height).toBe(400);

		// the top row is above the image, so it is all background
		expect([image!.pixels[0], image!.pixels[1], image!.pixels[2]]).toEqual([12, 34, 56]);
	});

	it('honors every named filter', () => {
		for (const filter of [Filter.nearest, Filter.bilinear, Filter.box, Filter.catmullRom]) {
			const { result, image } = abi.plan(fixture('sf-24.jpg'), (plan) => {
				abi.exports.tiny_plan_resize_with(plan, 200, 120, filter);
			});

			expect(result, `filter ${filter}`).toBe(Err.ok);
			expect([image!.width, image!.height]).toEqual([200, 120]);
		}
	});

	it('refuses a plan longer than its capacity', () => {
		const plan = abi.exports.tiny_alloc(abi.exports.tiny_plan_sizeof());
		const buffer = abi.copyIn(fixture('sf-24.jpg'));

		try {
			expect(abi.exports.tiny_plan_init(plan, buffer, fixture('sf-24.jpg').byteLength)).toBe(
				Err.ok
			);

			let last: number = Err.ok;
			for (let i = 0; i < 64; i++) last = abi.exports.tiny_plan_invert(plan);

			expect(last).toBe(Err.plan);
			expect(abi.exports.tiny_plan_count(plan)).toBe(32);
		} finally {
			abi.exports.tiny_free(buffer);
			abi.exports.tiny_free(plan);
		}
	});

	it('gives back everything a plan over a large source borrowed', () => {
		const run = () =>
			abi.plan(fixture('dog.jpg'), (plan) => {
				abi.exports.tiny_plan_resize(plan, 1500, 1000);
			});

		expect(run().result).toBe(Err.ok);

		// six megapixels in and one and a half out, so the pass needs real memory. running it again
		// has to reuse what the first run released rather than growing, which is the assertion that
		// the sample map, the weights and the intermediate all come back
		const settled = abi.pages;

		for (let i = 0; i < 3; i++) expect(run().result).toBe(Err.ok);

		expect(abi.pages).toBe(settled);
	});
});
