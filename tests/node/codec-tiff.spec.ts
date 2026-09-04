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
 * Reads one tag out of the first directory, from the format's own definition.
 *
 * Written independently of the C so the assertions below about what a fixture contains are a
 * reading of the file rather than a restatement of the decoder.
 */
function tag(data: Uint8Array, wanted: number): number | undefined {
	const big = data[0] === 0x4d;
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

	const read16 = (at: number) => view.getUint16(at, !big);
	const read32 = (at: number) => view.getUint32(at, !big);

	const directory = read32(4);
	const count = read16(directory);

	for (let i = 0; i < count; i++) {
		const entry = directory + 2 + i * 12;

		if (read16(entry) !== wanted) continue;

		const type = read16(entry + 2);
		return type === 3 ? read16(entry + 8) : read32(entry + 8);
	}

	return undefined;
}

describe('the tiff codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reads a header without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base-uncompressed.tif'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.tiff,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});
	});

	it('decodes every compression and layout to the pixels png and bmp reach', async () => {
		// four compressions, both byte orders, the horizontal predictor and a seven row strip
		// height, and none of them is allowed to change a pixel
		const variants = [
			'derived/base-uncompressed.tif',
			'derived/base-packbits.tif',
			'derived/base-lzw.tif',
			'derived/base-deflate.tif',
			'derived/base-msb.tif',
			'derived/base-predictor.tif',
			'derived/base-strips.tif'
		];

		for (const name of variants) {
			const image = abi.decode(fixture(name)).image!;

			expect([image.width, image.height, image.channels], name).toEqual([320, 180, 3]);
			expect(await sha256(image.pixels), name).toBe(golden.reference);
		}

		// and the fixtures really are what they claim: the byte order, the predictor tag and the
		// strip height are all read straight out of the files
		expect(fixture('derived/base-msb.tif')[0]).toBe(0x4d);
		expect(fixture('derived/base-uncompressed.tif')[0]).toBe(0x49);
		expect(tag(fixture('derived/base-predictor.tif'), 317)).toBe(2);
		expect(tag(fixture('derived/base-strips.tif'), 278)).toBe(7);
		expect(tag(fixture('derived/base-lzw.tif'), 259)).toBe(5);
		expect(tag(fixture('derived/base-packbits.tif'), 259)).toBe(32773);
	});

	it('reads a color map to the pixels the other palette formats reach', async () => {
		const image = abi.decode(fixture('derived/base-palette.tif')).image!;

		expect(tag(fixture('derived/base-palette.tif'), 262)).toBe(3);
		expect([image.width, image.height, image.channels]).toEqual([320, 180, 3]);
		expect(await sha256(image.pixels)).toBe(golden.bmpRle8);
	});

	it('reads grayscale and an alpha channel', async () => {
		const gray = abi.decode(fixture('derived/base-gray.tif')).image!;

		expect(gray.channels).toBe(1);
		expect(await sha256(gray.pixels)).toBe(golden.tiffGray);

		const alpha = abi.decode(fixture('derived/base-alpha.tif')).image!;

		expect(alpha.channels).toBe(4);
		expect(await sha256(alpha.pixels)).toBe(golden.tiffAlpha);

		let partial = 0;
		for (let i = 3; i < alpha.pixels.length; i += 4) {
			if (alpha.pixels[i] !== 255) partial++;
		}
		expect(partial).toBeGreaterThan(0);
	});

	it('decodes a region out of the strips it touches', () => {
		// the seven row file means a region crosses several strips and the full height one means
		// it crosses a single strip, and both have to give the same answer
		for (const name of ['derived/base-strips.tif', 'derived/base-deflate.tif']) {
			const source = fixture(name);
			const full = abi.decode(source).image!;

			const region = abi.decode(source, (i, b, n) =>
				abi.exports.tiny_image_load_region(i, b, n, 37, 21, 64, 48)
			).image!;

			expect([region.width, region.height], name).toEqual([64, 48]);

			for (let y = 0; y < 48; y++) {
				const wanted = full.pixels.subarray(
					((y + 21) * 320 + 37) * 3,
					((y + 21) * 320 + 37 + 64) * 3
				);
				expect(
					region.pixels.subarray(y * 64 * 3, (y + 1) * 64 * 3),
					`${name} row ${y}`
				).toEqual(wanted);
			}
		}
	});

	it('round trips losslessly at every channel count', () => {
		for (const channels of [1, 2, 3, 4]) {
			const source = abi.decode(fixture('derived/base-alpha.png'), (i, b, n) => {
				const result = abi.exports.tiny_image_load(i, b, n);
				return result === Err.ok
					? abi.exports.tiny_image_convert_channels(i, channels)
					: result;
			}).image!;

			const { result, bytes: encoded } = abi.transcode(
				fixture('derived/base-alpha.png'),
				Format.tiff,
				channels
			);

			expect(result, `${channels} channels`).toBe(Err.ok);

			const back = abi.decode(encoded!).image!;

			expect(back.channels, `${channels} channels`).toBe(channels);
			expect(back.pixels, `${channels} channels`).toEqual(source.pixels);
		}
	});

	it('writes several strips, and reads a region back out of them', () => {
		const source = fixture('derived/base.png');
		const original = abi.decode(source).image!;

		const { result, bytes: encoded } = abi.transcode(source, Format.tiff, 3);
		expect(result).toBe(Err.ok);

		// several strips, since the whole point of writing them is that a region decode reads only
		// the ones it needs. At 320 pixels of RGB a 64 KiB strip is 68 rows, so 180 rows is three
		expect(tag(encoded!, 278)).toBe(68);

		const back = abi.decode(encoded!).image!;
		expect(back.pixels).toEqual(original.pixels);

		// a region that starts in the second strip and ends in the third, so the reader has to
		// load more than one and index into each correctly
		const region = abi.decode(encoded!, (i, b, n) =>
			abi.exports.tiny_image_load_region(i, b, n, 37, 60, 128, 96)
		).image!;

		expect([region.width, region.height]).toEqual([128, 96]);

		const wanted = new Uint8Array(128 * 96 * 3);
		for (let y = 0; y < 96; y++) {
			wanted.set(
				original.pixels.subarray(
					((y + 60) * 320 + 37) * 3,
					((y + 60) * 320 + 37 + 128) * 3
				),
				y * 128 * 3
			);
		}

		expect(region.pixels).toEqual(wanted);
	});

	it('reports what it cannot read as a variant rather than as corrupt', () => {
		const source = fixture('derived/base-lzw.tif');

		expect(abi.decode(fixture('derived/malformed/not-an-image.bin')).result).toBe(
			Err.unknownFormat
		);
		expect(abi.decode(source.subarray(0, 8)).result).toBe(Err.corrupt);

		// a directory offset pointing outside the file
		const broken = new Uint8Array(source);
		broken[4] = 0xff;
		broken[5] = 0xff;
		broken[6] = 0xff;
		broken[7] = 0x7f;

		expect(abi.decode(broken).result).toBe(Err.corrupt);
	});
});
