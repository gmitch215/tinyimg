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
 * Counts the frames a file holds, from the format's own block structure.
 *
 * Written from the specification rather than from the C, so that the frame count `probe` reports is
 * checked against an independent reading of the same file.
 */
function frames(data: Uint8Array): number {
	const packed = data[10]!;
	let at = 13 + ((packed & 0x80) !== 0 ? (2 << (packed & 7)) * 3 : 0);
	let count = 0;

	const skipBlocks = (from: number): number => {
		let cursor = from;

		while (cursor < data.byteLength) {
			const length = data[cursor]!;

			if (length === 0) return cursor + 1;
			cursor += 1 + length;
		}

		return data.byteLength;
	};

	while (at < data.byteLength) {
		const block = data[at]!;

		if (block === 0x3b) break;

		if (block === 0x21) {
			at = skipBlocks(at + 2);
			continue;
		}

		if (block !== 0x2c) break;

		const descriptor = data[at + 9]!;
		let after = at + 10;

		if ((descriptor & 0x80) !== 0) after += (2 << (descriptor & 7)) * 3;

		count++;
		at = skipBlocks(after + 1);
	}

	return count;
}

describe('the gif codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reads a header without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base.gif'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.gif,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});
	});

	it('counts the frames of an animation it will only decode one of', async () => {
		const source = fixture('derived/base-animation.gif');
		const { info } = abi.probe(source);

		expect(info.frames).toBeGreaterThan(1);
		expect(info.frames).toBe(frames(source));

		const image = abi.decode(source).image!;
		expect([image.width, image.height]).toEqual([160, 90]);

		// and it is the first frame rather than the last or a composite of all three, which the
		// extent alone cannot tell apart. each frame of this file is the one before it rolled
		// sideways, so the three digests are genuinely different
		expect(await sha256(image.pixels)).toBe(golden.gifAnimationFirstFrame);
	});

	it('decodes to the pixels the bmp codec reaches through a different container', async () => {
		// the same picture through the same quantizer, in a palette BMP and a GIF: three
		// unrelated readers of one set of indices have to agree
		const gif = abi.decode(fixture('derived/base.gif')).image!;
		const bmp = abi.decode(fixture('derived/base-rle8.bmp')).image!;

		expect(gif.pixels).toEqual(bmp.pixels);
		expect(await sha256(gif.pixels)).toBe(golden.bmpRle8);
	});

	it('reconstructs an interlaced file to those same pixels', async () => {
		const image = abi.decode(fixture('derived/base-interlaced.gif')).image!;

		expect(abi.probe(fixture('derived/base-interlaced.gif')).info.progressive).toBe(true);
		expect(await sha256(image.pixels)).toBe(golden.bmpRle8);
	});

	it('turns a transparent index into a transparent pixel', async () => {
		const source = fixture('derived/base-transparent.gif');
		const { info } = abi.probe(source);

		expect([info.channels, info.hasAlpha]).toEqual([4, true]);

		const image = abi.decode(source).image!;
		expect(await sha256(image.pixels)).toBe(golden.gifTransparent);

		let clear = 0;
		for (let i = 3; i < image.pixels.length; i += 4) if (image.pixels[i] === 0) clear++;

		expect(clear).toBeGreaterThan(0);
		expect(clear).toBeLessThan(320 * 180);
	});

	it('decodes the logical screen rather than the frame', async () => {
		const source = fixture('derived/base-offset.gif');
		const { info } = abi.probe(source);

		// the frame is 120x90 at an offset inside a 160x120 screen, and the screen is the image
		expect([info.width, info.height]).toEqual([160, 120]);

		const image = abi.decode(source).image!;
		expect([image.width, image.height]).toEqual([160, 120]);
		expect(await sha256(image.pixels)).toBe(golden.gifOffsetFrame);
	});

	it('reads the smallest palette the format allows', async () => {
		const image = abi.decode(fixture('derived/base-mono.gif')).image!;

		expect(await sha256(image.pixels)).toBe(golden.gifMono);

		const seen = new Set<number>();
		for (let i = 0; i < image.pixels.length; i += 3) {
			seen.add((image.pixels[i]! << 16) | (image.pixels[i + 1]! << 8) | image.pixels[i + 2]!);
		}

		expect(seen.size).toBe(2);
	});

	it('keeps a palette that already fits, so the round trip is lossless', () => {
		for (const name of [
			'derived/base.gif',
			'derived/base-mono.gif',
			'derived/base-transparent.gif'
		]) {
			const original = abi.decode(fixture(name)).image!;
			const { result, bytes: encoded } = abi.transcode(fixture(name), Format.gif);

			expect(result, name).toBe(Err.ok);

			const back = abi.decode(encoded!).image!;

			expect([back.width, back.height, back.channels], name).toEqual([
				original.width,
				original.height,
				original.channels
			]);
			expect(back.pixels, name).toEqual(original.pixels);
		}
	});

	it('quantizes what does not fit, and stays close', () => {
		const source = fixture('sf-24.jpg');
		const original = abi.decode(source).image!;

		const { result, bytes: encoded } = abi.transcode(source, Format.gif);
		expect(result).toBe(Err.ok);

		const back = abi.decode(encoded!).image!;
		expect([back.width, back.height]).toEqual([original.width, original.height]);

		let sum = 0;
		for (let i = 0; i < back.pixels.length; i++) {
			const diff = back.pixels[i]! - original.pixels[i]!;
			sum += diff * diff;
		}

		const psnr = 10 * Math.log10((255 * 255 * back.pixels.length) / sum);
		expect(psnr).toBeGreaterThan(34);

		// and the stream compresses, or the LZW is doing nothing
		expect(encoded!.byteLength).toBeLessThan(back.pixels.length);
	});

	it('spends one palette entry on transparency and keeps every clear pixel clear', () => {
		const source = fixture('forest.png');
		const original = abi.decode(source).image!;

		expect(original.channels).toBe(4);

		const { result, bytes: encoded } = abi.transcode(source, Format.gif);
		expect(result).toBe(Err.ok);

		const back = abi.decode(encoded!).image!;
		expect(back.channels).toBe(4);

		let clear = 0;
		let kept = 0;

		for (let i = 3; i < original.pixels.length; i += 4) {
			if (original.pixels[i]! < 128) {
				clear++;
				if (back.pixels[i] === 0) kept++;
			}
		}

		expect(clear).toBeGreaterThan(0);
		expect(kept).toBe(clear);
	});

	it('rejects what it cannot read and decodes what arrived of what it can', () => {
		expect(abi.decode(fixture('derived/malformed/not-an-image.bin')).result).toBe(
			Err.unknownFormat
		);

		const source = fixture('derived/base.gif');

		expect(abi.decode(source.subarray(0, 6)).result).toBe(Err.corrupt);
		expect(abi.decode(source.subarray(0, 13)).result).toBe(Err.corrupt);

		// a stream cut off mid frame gives back the rows that arrived, which is what every other
		// reader does and what the format's block structure is for
		const partial = abi.decode(source.subarray(0, source.byteLength >> 1)).image!;
		expect([partial.width, partial.height]).toEqual([320, 180]);
	});
});
