import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Blend, Curve, Err, Fx, Preset, TinyAbi } from '../support/abi.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

describe('the phase 5 operations inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('draws onto an image it made rather than one it decoded', () => {
		const { image } = abi.canvas(32, 32, 4, (handle, api) => {
			const red = api.bytesIn(new Uint8Array([255, 0, 0, 255]));

			try {
				expect(api.exports.tiny_image_fill_rectangle(handle, 4, 4, 8, 8, red)).toBe(Err.ok);
				expect(api.exports.tiny_image_fill_circle(handle, 24, 24, 5, red)).toBe(Err.ok);
			} finally {
				api.exports.tiny_free(red);
			}
		});

		const at = (x: number, y: number) => image.pixels[(y * 32 + x) * 4]!;

		expect(at(5, 5)).toBe(255);
		expect(at(24, 24)).toBe(255);
		expect(at(16, 16)).toBe(0);
	});

	it('renders a display list and reports what it eliminated', () => {
		const counts = abi.canvas(40, 40, 4, (handle, api) => {
			const list = api.exports.tiny_alloc(api.exports.tiny_display_sizeof());
			const green = api.bytesIn(new Uint8Array([0, 255, 0, 255]));
			const red = api.bytesIn(new Uint8Array([255, 0, 0, 255]));

			try {
				api.exports.tiny_display_init(list);

				// one shape off the canvas, one under an opaque cover, one that draws
				api.exports.tiny_display_rect(list, 500, 500, 10, 10, green);
				api.exports.tiny_display_rect(list, 5, 5, 6, 6, green);
				api.exports.tiny_display_rect(list, 0, 0, 40, 40, red);

				expect(api.exports.tiny_display_render(list, handle)).toBe(Err.ok);

				return [
					api.exports.tiny_display_culled(list),
					api.exports.tiny_display_covered(list)
				];
			} finally {
				api.exports.tiny_free(red);
				api.exports.tiny_free(green);
				api.exports.tiny_free(list);
			}
		});

		expect(counts.value).toEqual([1, 1]);
		expect(counts.image.pixels[0]).toBe(255);
		expect(counts.image.pixels[1]).toBe(0);
	});

	it('composites two layers through a blend mode', () => {
		const { value } = abi.canvas(4, 4, 4, (dest, api) => {
			const src = api.exports.tiny_alloc(api.exports.tiny_image_sizeof());

			try {
				expect(api.exports.tiny_image_create(src, 4, 4, 4)).toBe(Err.ok);

				const data = api.exports.tiny_image_getdata(src);
				const view = new Uint8Array(api.exports.memory.buffer);

				for (let i = 0; i < 16; i++) {
					view[data + i * 4] = 128;
					view[data + i * 4 + 1] = 128;
					view[data + i * 4 + 2] = 128;
					view[data + i * 4 + 3] = 255;
				}

				const under = api.exports.tiny_image_getdata(dest);
				for (let i = 0; i < 16; i++) {
					view[under + i * 4] = 200;
					view[under + i * 4 + 1] = 200;
					view[under + i * 4 + 2] = 200;
					view[under + i * 4 + 3] = 255;
				}

				expect(api.exports.tiny_image_composite(dest, src, Blend.multiply)).toBe(Err.ok);

				return new Uint8Array(api.exports.memory.buffer)[under]!;
			} finally {
				api.exports.tiny_image_destroy(src);
				api.exports.tiny_free(src);
			}
		});

		// 200 * 128 / 255, rounded
		expect(value).toBe(100);
	});

	it('carries a matrix and a curve through the plan', () => {
		const { resolution, image } = abi.plan(fixture('derived/base.png'), (plan) => {
			const matrix = abi.floatsIn([
				0.393, 0.769, 0.189, 0, 0.349, 0.686, 0.168, 0, 0.272, 0.534, 0.131, 0
			]);
			const levels = abi.floatsIn([10, 245, 1.1, 0, 255]);

			try {
				expect(abi.exports.tiny_plan_matrix(plan, matrix)).toBe(Err.ok);
				expect(abi.exports.tiny_plan_curve(plan, Curve.levels, levels, 0)).toBe(Err.ok);
				expect(abi.exports.tiny_plan_saturation(plan, 1.2)).toBe(Err.ok);
			} finally {
				abi.exports.tiny_free(levels);
				abi.exports.tiny_free(matrix);
			}
		});

		expect(resolution).toBeDefined();
		expect(image).toBeDefined();
		expect(image!.width).toBe(320);
	});

	it('runs a neighborhood effect as its own pass', () => {
		const { image } = abi.plan(fixture('derived/base.png'), (plan) => {
			const params = abi.floatsIn([1.5, 1.0, 0, 0]);

			try {
				expect(abi.exports.tiny_plan_effect(plan, Fx.unsharp, params)).toBe(Err.ok);
			} finally {
				abi.exports.tiny_free(params);
			}
		});

		expect(image).toBeDefined();
		expect(image!.width).toBe(320);
	});

	it('confines a region effect to its rectangle', () => {
		const plain = abi.plan(fixture('derived/base.png'), () => {});
		const { image } = abi.plan(fixture('derived/base.png'), (plan) => {
			const params = abi.floatsIn([6, 0, 0, 0]);

			try {
				expect(
					abi.exports.tiny_plan_effect_rect(plan, Fx.pixelateRegion, params, 0, 0, 40, 40)
				).toBe(Err.ok);
			} finally {
				abi.exports.tiny_free(params);
			}
		});

		expect(image).toBeDefined();

		const inside = (pixels: Uint8Array, x: number, y: number) => pixels[(y * 320 + x) * 3]!;

		// inside the rectangle the pixels are flattened into blocks, outside it they
		// are exactly what a plain decode gives
		expect(inside(image!.pixels, 1, 1)).toBe(inside(image!.pixels, 2, 2));
		expect(inside(image!.pixels, 200, 100)).toBe(inside(plain.image!.pixels, 200, 100));
	});

	it('applies a named preset', () => {
		const source = abi.decode(fixture('derived/base.png')).image!;

		const { image } = abi.canvas(
			source.width,
			source.height,
			source.channels,
			(handle, api) => {
				const data = api.exports.tiny_image_getdata(handle);
				new Uint8Array(api.exports.memory.buffer).set(source.pixels, data);

				expect(api.exports.tiny_image_preset(handle, Preset.mono)).toBe(Err.ok);
			}
		);

		// mono leaves no chroma at all
		for (let i = 0; i < 300; i += 3) {
			expect(image.pixels[i]).toBe(image.pixels[i + 1]);
			expect(image.pixels[i + 1]).toBe(image.pixels[i + 2]);
		}
	});

	it('reads a histogram and the colors an image is made of', () => {
		const source = abi.decode(fixture('derived/base.png')).image!;

		const { value } = abi.canvas(
			source.width,
			source.height,
			source.channels,
			(handle, api) => {
				const data = api.exports.tiny_image_getdata(handle);
				new Uint8Array(api.exports.memory.buffer).set(source.pixels, data);

				const bins = api.exports.tiny_alloc(256 * 4);
				const color = api.exports.tiny_alloc(4);
				const palette = api.exports.tiny_alloc(4 * 8);

				try {
					expect(api.exports.tiny_image_histogram(handle, 255, bins)).toBe(Err.ok);
					expect(api.exports.tiny_image_average_color(handle, color)).toBe(Err.ok);
					expect(api.exports.tiny_image_palette(handle, 8, palette)).toBe(Err.ok);

					const counts = api.u32sOut(bins, 256);
					const view = new Uint8Array(api.exports.memory.buffer);

					return {
						total: counts.reduce((sum, n) => sum + n, 0),
						mean: [view[color]!, view[color + 1]!, view[color + 2]!],
						first: [view[palette]!, view[palette + 1]!, view[palette + 2]!]
					};
				} finally {
					api.exports.tiny_free(palette);
					api.exports.tiny_free(color);
					api.exports.tiny_free(bins);
				}
			}
		);

		// every pixel counted once
		expect(value.total).toBe(source.width * source.height);
		expect(value.mean.every((n) => n > 0 && n < 255)).toBe(true);
		expect(value.first.some((n) => n > 0)).toBe(true);
	});

	it('hashes an image and separates it from another', () => {
		const hashOf = (name: string): bigint => {
			const source = abi.decode(fixture(name)).image!;

			return abi.canvas(source.width, source.height, source.channels, (handle, api) => {
				const data = api.exports.tiny_image_getdata(handle);
				new Uint8Array(api.exports.memory.buffer).set(source.pixels, data);

				const slot = api.exports.tiny_alloc(8);

				try {
					expect(api.exports.tiny_image_phash(handle, slot)).toBe(Err.ok);
					return new DataView(api.exports.memory.buffer).getBigUint64(slot, true);
				} finally {
					api.exports.tiny_free(slot);
				}
			}).value;
		};

		const base = hashOf('derived/base.png');
		const again = hashOf('derived/base.bmp');
		const other = hashOf('derived/base-mono.gif');

		// the same picture through two containers hashes the same
		expect(abi.exports.tiny_phash_distance(base, again)).toBe(0);
		expect(abi.exports.tiny_phash_distance(base, other)).toBeGreaterThan(0);
	});

	it('converts a tagged image to sRGB through its profile', () => {
		const source = abi.decode(fixture('derived/base-display-p3.png')).image!;
		const profileBytes = fixture('derived/icc/display-p3.icc');

		const { value } = abi.canvas(
			source.width,
			source.height,
			source.channels,
			(handle, api) => {
				const data = api.exports.tiny_image_getdata(handle);
				new Uint8Array(api.exports.memory.buffer).set(source.pixels, data);

				const bytesPointer = api.bytesIn(profileBytes);
				// the structure is a 3x3, a white point, three 256-entry curves and a flag
				const profile = api.exports.tiny_alloc(9 * 4 + 3 * 4 + 3 * 256 * 4 + 4);

				try {
					expect(
						api.exports.tiny_icc_parse(profile, bytesPointer, profileBytes.byteLength)
					).toBe(Err.ok);
					expect(api.exports.tiny_icc_convert_image(handle, profile)).toBe(Err.ok);

					return true;
				} finally {
					api.exports.tiny_free(profile);
					api.exports.tiny_free(bytesPointer);
				}
			}
		);

		expect(value).toBe(true);
	});

	it('rejects out of range parameters at the boundary rather than inside', () => {
		const { value } = abi.canvas(8, 8, 3, (handle, api) => {
			const params = abi.floatsIn([1, 0, 0, 0]);

			try {
				return {
					posterize: api.exports.tiny_image_posterize(handle, 1),
					pixelate: api.exports.tiny_image_pixelate(handle, 1),
					sobel: api.exports.tiny_image_sobel(handle)
				};
			} finally {
				api.exports.tiny_free(params);
			}
		});

		expect(value.posterize).toBe(Err.range);
		// a block size below two is a request with nothing in it, not an error
		expect(value.pixelate).toBe(Err.ok);
		expect(value.sobel).toBe(Err.ok);
	});
});
