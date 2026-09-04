import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Err, Format, TinyAbi } from '../support/abi.js';
import { golden, sha256 } from '../support/golden.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

/**
 * Walks the RIFF chunks, from the container's own structure.
 *
 * Written from the specification rather than from the C, so what `probe` reports is checked against
 * an independent reading of the same bytes rather than against itself.
 */
function chunks(data: Uint8Array): { tag: string; at: number; length: number }[] {
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
	const text = new TextDecoder('latin1');
	const found: { tag: string; at: number; length: number }[] = [];

	const limit = Math.min(view.getUint32(4, true) + 8, data.byteLength);
	let at = 12;

	while (at + 8 <= limit) {
		const tag = text.decode(data.subarray(at, at + 4));
		const length = view.getUint32(at + 4, true);

		found.push({ tag, at: at + 8, length });

		// a frame is a container of its own, so the walk steps inside it
		if (tag === 'ANMF') {
			at += 8 + 16;
			continue;
		}

		at += 8 + length + (length & 1);
	}

	return found;
}

describe('the webp codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reads a lossy header without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base-lossy.webp'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.webp,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,

			// neither bitstream can be read out of order, so nothing here is progressive in the
			// sense the field means
			progressive: false
		});
	});

	it('takes a lossless stream at its own word about alpha', () => {
		// a simple file has no extended header to have declared alpha, so the five byte lossless
		// header is the only place it can come from
		const source = fixture('derived/base-alpha.webp');

		expect(chunks(source).map((chunk) => chunk.tag)).toEqual(['VP8L']);
		expect(abi.probe(source).info).toMatchObject({ channels: 4, hasAlpha: true });
	});

	it('takes a lossy frame at the container word about alpha', () => {
		// a lossy frame keeps alpha in a chunk of its own, and the flag saying so is in the
		// extended header rather than in the frame
		const source = fixture('derived/base-lossy-alpha.webp');
		const tags = chunks(source).map((chunk) => chunk.tag);

		expect(tags).toContain('VP8X');
		expect(tags).toContain('ALPH');
		expect(abi.probe(source).info).toMatchObject({ channels: 4, hasAlpha: true });
	});

	it('counts the frames of an animation it will only decode one of', async () => {
		const source = fixture('derived/base-animation.webp');
		const { info } = abi.probe(source);

		expect(info.frames).toBe(chunks(source).filter((chunk) => chunk.tag === 'ANMF').length);
		expect(info.frames).toBe(2);

		// the canvas the file declares, not the frame's own extents: the second frame of this
		// fixture is a row shorter, so a decoder reporting the frame would be wrong by a row
		expect([info.width, info.height]).toEqual([320, 180]);

		const image = abi.decode(source).image!;

		expect([image.width, image.height]).toEqual([320, 180]);
		expect(await sha256(image.pixels)).toBe(golden.webpAnimationFirstFrame);
	});

	it('decodes a lossless stream to the pixels three other codecs reach', async () => {
		const image = abi.decode(fixture('derived/base-lossless.webp')).image!;

		// the same picture as a BMP, three PNGs and seven TIFFs. WebP lossless gets there by
		// subtracting green from red and blue and then predicting every pixel from its neighbors,
		// which is nothing like what the others do
		expect(await sha256(image.pixels)).toBe(golden.reference);
		expect(image.pixels).toEqual(abi.decode(fixture('derived/base-rgb8.png')).image!.pixels);
	});

	it('decodes a lossless stream with alpha to the pixels tiff reaches', async () => {
		const image = abi.decode(fixture('derived/base-alpha.webp')).image!;

		expect(image.channels).toBe(4);
		expect(await sha256(image.pixels)).toBe(golden.tiffAlpha);
	});

	it('decodes a lossy frame to what dwebp produces', async () => {
		const image = abi.decode(fixture('derived/base-lossy.webp')).image!;

		expect(await sha256(image.pixels)).toBe(golden.webpLossy);
	});

	it('reads an alpha plane the same whether it was compressed or stored raw', async () => {
		const compressed = abi.decode(fixture('derived/base-lossy-alpha.webp')).image!;
		const raw = abi.decode(fixture('derived/base-raw-alpha.webp')).image!;

		expect(compressed.pixels).toEqual(raw.pixels);
		expect(await sha256(compressed.pixels)).toBe(golden.webpLossyAlpha);

		// the ramp runs down the rows, opaque at the top and clear at the bottom, which a plane
		// read upside down or left unfiltered gets wrong
		const stride = compressed.width * 4;
		const top = compressed.pixels[3]!;
		const bottom = compressed.pixels[(compressed.height - 1) * stride + 3]!;

		expect(top).toBeGreaterThan(200);
		expect(bottom).toBeLessThan(60);
	});

	it('undoes the filter an alpha plane was written with', () => {
		// the encoder only ever settles on the horizontal filter, so that is the one a fixture can
		// reach; the other two are below
		const source = fixture('derived/base-filtered-alpha.webp');
		const alph = chunks(source).find((chunk) => chunk.tag === 'ALPH')!;
		const header = source[alph.at]!;

		expect(header & 3).toBe(1);
		expect((header >> 2) & 3).toBe(1);

		const image = abi.decode(source).image!;

		expect(image.channels).toBe(4);
		expect(image.pixels[3]).toBeGreaterThan(200);
		expect(image.pixels[(image.height - 1) * image.width * 4 + 3]).toBeLessThan(60);
	});

	it.each([
		[
			'vertical',
			2,
			(plane: Uint8Array, width: number, height: number) => {
				// each row minus the one above it, the first falling back to the horizontal filter
				const out = Uint8Array.from(plane);

				for (let y = height - 1; y > 0; y--) {
					for (let x = 0; x < width; x++) {
						out[y * width + x] =
							(plane[y * width + x]! - plane[(y - 1) * width + x]!) & 0xff;
					}
				}

				let left = 0;
				for (let x = 0; x < width; x++) {
					out[x] = (plane[x]! - left) & 0xff;
					left = plane[x]!;
				}

				return out;
			}
		],
		[
			'gradient',
			3,
			(plane: Uint8Array, width: number, height: number) => {
				const clamp = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : v);
				const out = Uint8Array.from(plane);

				for (let y = height - 1; y > 0; y--) {
					let corner = plane[(y - 1) * width]!;
					let previous = corner;

					for (let x = 0; x < width; x++) {
						const top = plane[(y - 1) * width + x]!;
						const guess = clamp(previous + top - corner);

						out[y * width + x] = (plane[y * width + x]! - guess) & 0xff;
						corner = top;
						previous = plane[y * width + x]!;
					}
				}

				let left = 0;
				for (let x = 0; x < width; x++) {
					out[x] = (plane[x]! - left) & 0xff;
					left = plane[x]!;
				}

				return out;
			}
		]
	])('undoes the %s alpha filter, which no encoder here writes', (_name, method, filter) => {
		/*
		 * The encoder available to the fixtures only ever writes the horizontal filter, so
		 * these two branches of the decoder would otherwise never run. Rather than assert that
		 * a hand-built file merely decodes, the forward filter is computed here from the
		 * specification and the decoder has to invert it back to the plane it started from,
		 * which is an answer known independently of the code under test.
		 */
		const source = Uint8Array.from(fixture('derived/base-raw-alpha.webp'));
		const alph = chunks(source).find((chunk) => chunk.tag === 'ALPH')!;

		// stored raw, so the payload past its header byte is the plane itself
		expect(source[alph.at]! & 3).toBe(0);

		const width = 320;
		const height = 180;
		const plane = source.subarray(alph.at + 1, alph.at + 1 + width * height);
		const original = Uint8Array.from(plane);

		plane.set(filter(original, width, height));
		source[alph.at] = (source[alph.at]! & ~0x0c) | (method << 2);

		const image = abi.decode(source).image!;
		const back = new Uint8Array(width * height);

		for (let i = 0; i < back.length; i++) back[i] = image.pixels[i * 4 + 3]!;

		expect(back).toEqual(original);
	});

	it.each([
		['a strong filter', 'derived/base-strong.webp', golden.webpStrong],
		['a simple filter', 'derived/base-simple.webp', golden.webpSimple],
		['a reduced interior limit', 'derived/base-sharp.webp', golden.webpSharp],
		['no segment map', 'derived/base-onesegment.webp', golden.webpOneSegment]
	])('decodes a frame written with %s', async (_what, name, digest) => {
		const image = abi.decode(fixture(name)).image!;

		expect(await sha256(image.pixels)).toBe(digest);
	});

	it.each([
		['lossy', 'derived/tiny-odd-lossy.webp', golden.webpOddLossy],
		['lossless', 'derived/tiny-odd-lossless.webp', golden.webpOddLossless]
	])('decodes odd extents through the %s path', async (_mode, name, digest) => {
		const image = abi.decode(fixture(name)).image!;

		expect([image.width, image.height]).toEqual([65, 33]);
		expect(await sha256(image.pixels)).toBe(digest);
	});

	it('takes a region after decoding, since neither bitstream can start in the middle', () => {
		const source = fixture('derived/base-lossless.webp');
		const whole = abi.decode(source).image!;

		const part = abi.decode(source, (image, buffer, size) =>
			abi.exports.tiny_image_load_region(image, buffer, size, 41, 17, 100, 50)
		).image!;

		expect([part.width, part.height]).toEqual([100, 50]);

		for (let y = 0; y < part.height; y++) {
			const got = part.pixels.subarray(y * 300, (y + 1) * 300);
			const from = ((y + 17) * whole.width + 41) * 3;

			expect(got, `row ${y}`).toEqual(whole.pixels.subarray(from, from + 300));
		}
	});

	it('box averages a scaled decode the way every other codec does', () => {
		const scale = (name: string) =>
			abi.decode(fixture(name), (image, buffer, size) =>
				abi.exports.tiny_image_load_scaled(image, buffer, size, 80, 45)
			).image!;

		const scaled = scale('derived/base-lossless.webp');

		expect([scaled.width, scaled.height]).toEqual([80, 45]);
		expect(scaled.pixels).toEqual(scale('derived/base-rgb8.png').pixels);
	});

	it('encodes losslessly, which means the pixels come back', () => {
		const source = fixture('derived/base-rgb8.png');
		const original = abi.decode(source).image!;

		const { result, bytes: encoded } = abi.transcode(source, Format.webp, undefined, {
			lossless: true
		});

		expect(result).toBe(Err.ok);
		expect(chunks(encoded!).map((chunk) => chunk.tag)).toEqual(['VP8L']);

		const back = abi.decode(encoded!).image!;

		expect(back.pixels).toEqual(original.pixels);
		expect(encoded!.byteLength).toBeLessThan(320 * 180 * 3);
	});

	it('keeps an image inside 256 colors exactly, through a palette', () => {
		const source = fixture('derived/logo.png');
		const original = abi.decode(source).image!;

		const { bytes: encoded } = abi.transcode(source, Format.webp, undefined, {
			lossless: true
		});

		expect(abi.decode(encoded!).image!.pixels).toEqual(original.pixels);

		// a logo is what the format is best at, and a photograph's pipeline would handle it badly
		expect(encoded!.byteLength).toBeLessThan(2048);
	});

	it('keeps one color exactly, which is the smallest palette there is', () => {
		const source = fixture('derived/flat.png');
		const original = abi.decode(source).image!;

		const { bytes: encoded } = abi.transcode(source, Format.webp, undefined, {
			lossless: true
		});

		expect(abi.decode(encoded!).image!.pixels).toEqual(original.pixels);
		expect(encoded!.byteLength).toBeLessThan(100);
	});

	it('encodes lossily, with a quality number that means something monotone', () => {
		const source = fixture('derived/base-rgb8.png');
		const original = abi.decode(source).image!;

		const sizes: number[] = [];
		const errors: number[] = [];

		for (const quality of [40, 80, 95]) {
			const { result, bytes: encoded } = abi.transcode(source, Format.webp, undefined, {
				quality
			});

			expect(result).toBe(Err.ok);
			expect(chunks(encoded!).map((chunk) => chunk.tag)).toEqual(['VP8 ']);

			const back = abi.decode(encoded!).image!;

			expect([back.width, back.height]).toEqual([320, 180]);

			let total = 0;
			for (let i = 0; i < back.pixels.length; i++) {
				const diff = back.pixels[i]! - original.pixels[i]!;
				total += diff * diff;
			}

			sizes.push(encoded!.byteLength);
			errors.push(total);
		}

		// more quality costs more bytes and loses less, or the number is not usable
		expect(sizes[0]).toBeLessThan(sizes[1]!);
		expect(sizes[1]).toBeLessThan(sizes[2]!);
		expect(errors[0]).toBeGreaterThan(errors[1]!);
		expect(errors[1]).toBeGreaterThan(errors[2]!);

		// 32 dB at quality 80 is what the format delivers; a wrong forward transform or a wrong
		// mode decision lands far below it
		const psnr = 10 * Math.log10((255 * 255 * 320 * 180 * 3) / errors[1]!);
		expect(psnr).toBeGreaterThan(32);
	});

	it('refuses a frame it cannot use, distinctly from one it cannot read', () => {
		const good = fixture('derived/base-lossy.webp');

		// the low bit of the frame tag clear means a keyframe; set means an interframe, which
		// needs a reference frame a still image never has
		const inter = Uint8Array.from(good);
		inter[20] = good[20]! | 1;

		expect(abi.decode(inter).result).toBe(Err.unsupportedVariant);

		const broken = Uint8Array.from(good);
		broken[23] = 0;

		expect(abi.decode(broken).result).toBe(Err.corrupt);
	});

	it('reports a truncated container rather than reading past it', () => {
		const source = fixture('derived/base-lossless.webp');

		expect(abi.decode(source.subarray(0, 40)).result).toBe(Err.corrupt);
		expect(abi.probe(new Uint8Array(12)).result).toBe(Err.unknownFormat);
	});
});
