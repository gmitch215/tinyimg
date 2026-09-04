import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Err, Format, TinyAbi } from '../support/abi.js';
import { golden, sha256 } from '../support/golden.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

function hasMagick(): boolean {
	try {
		execFileSync('magick', ['-version'], { stdio: 'ignore' });
		return true;
	} catch {
		return false;
	}
}

/**
 * A BMP header parser written from the format's own documentation rather than from the C, so a
 * mistake shared between encoder and decoder cannot hide behind a round trip.
 */
function parseBmpHeader(data: Uint8Array) {
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

	return {
		signature: String.fromCharCode(data[0]!, data[1]!),
		fileSize: view.getUint32(2, true),
		pixelOffset: view.getUint32(10, true),
		headerSize: view.getUint32(14, true),
		width: view.getInt32(18, true),
		height: view.getInt32(22, true),
		planes: view.getUint16(26, true),
		bpp: view.getUint16(28, true),
		compression: view.getUint32(30, true),
		paletteCount: view.getUint32(46, true)
	};
}

/**
 * The wasm module driven directly, which is what makes this lane worth having: the ctest suite
 * exercises the native build, and the heap's growth path only exists on wasm.
 */
describe('the bmp codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		const module = new WebAssembly.Module(bytes);
		abi = new TinyAbi(new WebAssembly.Instance(module, {}));
	});

	it('probes a bitmap without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base.bmp'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.bmp,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});
	});

	it('decodes the whole bitmap', async () => {
		const { result, image } = abi.decode(fixture('derived/base.bmp'));

		expect(result).toBe(Err.ok);
		expect(image).toBeDefined();
		expect(image!.width).toBe(320);
		expect(image!.height).toBe(180);
		expect(image!.channels).toBe(3);
		expect(image!.format).toBe(Format.bmp);
		expect(image!.pixels).toHaveLength(320 * 180 * 3);

		// the same digest the workers lane checks, so a runtime specific difference in the decode
		// cannot pass both lanes
		expect(await sha256(image!.pixels)).toBe(golden.reference);
	});

	it('decodes the run length encoded and scaled cases to their recorded pixels', async () => {
		const rle = abi.decode(fixture('derived/base-rle8.bmp')).image!;
		expect(await sha256(rle.pixels)).toBe(golden.bmpRle8);

		const eighth = abi.decode(fixture('derived/base.bmp'), (image, buffer, size) =>
			abi.exports.tiny_image_load_scaled(image, buffer, size, 40, 20)
		).image!;
		expect(await sha256(eighth.pixels)).toBe(golden.referenceEighth);
	});

	it('decodes a region equal to the same rectangle of a full decode', () => {
		const source = fixture('derived/base.bmp');
		const full = abi.decode(source).image!;

		const region = abi.decode(source, (image, buffer, size) =>
			abi.exports.tiny_image_load_region(image, buffer, size, 37, 21, 64, 48)
		).image!;

		expect(region.width).toBe(64);
		expect(region.height).toBe(48);

		for (let y = 0; y < 48; y++) {
			const wanted = full.pixels.subarray(
				((y + 21) * 320 + 37) * 3,
				((y + 21) * 320 + 37 + 64) * 3
			);
			const got = region.pixels.subarray(y * 64 * 3, (y + 1) * 64 * 3);

			expect(got, `row ${y}`).toEqual(wanted);
		}
	});

	it('picks the cheapest scale that still covers the box', () => {
		const source = fixture('derived/base.bmp');

		const half = abi.decode(source, (image, buffer, size) =>
			abi.exports.tiny_image_load_scaled(image, buffer, size, 100, 50)
		).image!;
		expect([half.width, half.height]).toEqual([160, 90]);

		const eighth = abi.decode(source, (image, buffer, size) =>
			abi.exports.tiny_image_load_scaled(image, buffer, size, 40, 20)
		).image!;
		expect([eighth.width, eighth.height]).toEqual([40, 23]);
	});

	it('round trips 24 bit pixels losslessly', () => {
		const source = fixture('derived/base.bmp');
		const original = abi.decode(source).image!;

		const { result, bytes: encoded } = abi.transcode(source, Format.bmp);
		expect(result).toBe(Err.ok);

		const header = parseBmpHeader(encoded!);
		expect(header.signature).toBe('BM');
		expect(header.headerSize).toBe(40);
		expect(header.width).toBe(320);
		expect(header.height).toBe(180);
		expect(header.planes).toBe(1);
		expect(header.bpp).toBe(24);
		expect(header.compression).toBe(0);
		expect(header.pixelOffset).toBe(54);
		expect(header.fileSize).toBe(encoded!.byteLength);
		expect(header.fileSize).toBe(54 + 320 * 180 * 3);

		expect(abi.decode(encoded!).image!.pixels).toEqual(original.pixels);
	});

	it('writes a v4 header when the image carries alpha', () => {
		const { result, bytes: encoded } = abi.transcode(
			fixture('derived/base.bmp'),
			Format.bmp,
			4
		);
		expect(result).toBe(Err.ok);

		const header = parseBmpHeader(encoded!);
		expect(header.headerSize).toBe(108);
		expect(header.bpp).toBe(32);
		expect(header.compression).toBe(3);
		expect(header.pixelOffset).toBe(122);

		const view = new DataView(encoded!.buffer, encoded!.byteOffset);
		expect(view.getUint32(54, true)).toBe(0x00ff0000);
		expect(view.getUint32(58, true)).toBe(0x0000ff00);
		expect(view.getUint32(62, true)).toBe(0x000000ff);
		expect(view.getUint32(66, true)).toBe(0xff000000);

		expect(abi.decode(encoded!).image!.channels).toBe(4);
	});

	it('writes a greyscale palette for a one channel image', () => {
		const { result, bytes: encoded } = abi.transcode(
			fixture('derived/base.bmp'),
			Format.bmp,
			1
		);
		expect(result).toBe(Err.ok);

		const header = parseBmpHeader(encoded!);
		expect(header.bpp).toBe(8);
		expect(header.paletteCount).toBe(256);
		expect(header.pixelOffset).toBe(1078);

		// entry n has to be n in all three channels, or a reader shows a false colour image
		for (const index of [0, 1, 128, 254, 255]) {
			const entry = 54 + index * 4;
			expect([encoded![entry], encoded![entry + 1], encoded![entry + 2]]).toEqual([
				index,
				index,
				index
			]);
		}
	});

	it('agrees with the run length encoded copy of the same image', () => {
		const plain = abi.decode(fixture('derived/base.bmp')).image!;
		const rle = abi.decode(fixture('derived/base-rle8.bmp')).image!;

		expect([rle.width, rle.height, rle.channels]).toEqual([320, 180, 3]);

		let squared = 0;
		for (let i = 0; i < plain.pixels.length; i++) {
			const difference = plain.pixels[i]! - rle.pixels[i]!;
			squared += difference * difference;
		}

		const psnr = 10 * Math.log10((255 * 255) / (squared / plain.pixels.length));
		expect(psnr).toBeGreaterThan(28);
	});

	it('names the reason a file cannot be decoded', () => {
		expect(abi.decode(fixture('derived/malformed/not-an-image.bin')).result).toBe(
			Err.unknownFormat
		);
		expect(abi.decode(new Uint8Array([0x42, 0x4d])).result).toBe(Err.corrupt);

		// a format this build recognises but cannot decode, which is a different answer from not
		// recognising it at all. WebP served here until it gained a codec; AVIF is what is left,
		// since its own answers probe and neither direction of pixels
		expect(abi.decode(fixture('derived/base.avif')).result).toBe(Err.unsupportedCodec);

		expect(abi.errorName(Err.unsupportedCodec)).toBe('unsupported codec');
		expect(abi.errorName(Err.corrupt)).toBe('corrupt data');
	});
});

/**
 * The heap's growth path is compiled only for wasm, so the native ctest suite cannot reach it at
 * all. A fresh instance per assertion keeps the page counts predictable.
 */
describe('the heap growing inside the wasm module', () => {
	function fresh(): TinyAbi {
		return new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	}

	it('starts with the pages the link settings ask for', () => {
		expect(fresh().pages).toBe(16);
	});

	it('grows to serve an allocation the initial pages cannot hold', () => {
		const abi = fresh();
		const before = abi.pages;

		// four megabytes against roughly three quarters of a megabyte of initial heap
		const pointer = abi.exports.tiny_alloc(4 * 1024 * 1024);
		expect(pointer).toBeGreaterThan(0);
		expect(abi.pages).toBeGreaterThan(before);

		// the grown region is usable, not merely reserved
		const view = new Uint8Array(abi.exports.memory.buffer);
		view[pointer] = 0x11;
		view[pointer + 4 * 1024 * 1024 - 1] = 0x22;
		expect(view[pointer]).toBe(0x11);
		expect(view[pointer + 4 * 1024 * 1024 - 1]).toBe(0x22);
	});

	it('reuses the grown region rather than growing again', () => {
		const abi = fresh();

		const pointer = abi.exports.tiny_alloc(4 * 1024 * 1024);
		abi.exports.tiny_free(pointer);
		const grown = abi.pages;

		const again = abi.exports.tiny_alloc(4 * 1024 * 1024);
		expect(again).toBe(pointer);
		expect(abi.pages).toBe(grown);
	});

	it('refuses an allocation past the module memory ceiling without trapping', () => {
		const abi = fresh();

		// --max-memory is 64 MiB, so this cannot be satisfied and has to come back as a null
		expect(abi.exports.tiny_alloc(200 * 1024 * 1024)).toBe(0);

		// and the heap is still usable afterwards
		expect(abi.decode(fixture('derived/base.bmp')).result).toBe(Err.ok);
	});

	it('decodes every bitmap fixture after the heap has grown', () => {
		const abi = fresh();
		abi.exports.tiny_free(abi.exports.tiny_alloc(4 * 1024 * 1024));

		// a stale region end after memory.grow would only surface here
		for (const name of ['derived/base.bmp', 'derived/base-rle8.bmp']) {
			const { result, image } = abi.decode(fixture(name));

			expect(result, name).toBe(Err.ok);
			expect([image!.width, image!.height], name).toEqual([320, 180]);
		}
	});
});

describe.skipIf(!hasMagick())('the bmp encoder against imagemagick', () => {
	let abi: TinyAbi;
	let directory: string;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
		directory = mkdtempSync(join(tmpdir(), 'tinyimg-bmp-'));
	});

	/**
	 * Writes our encoder's output and asks magick what it sees, which is the only check here that
	 * a third party reads the file the way we meant it.
	 */
	function identify(name: string, encoded: Uint8Array) {
		const path = join(directory, name);
		writeFileSync(path, encoded);

		const output = execFileSync('magick', ['identify', '-format', '%w %h %[channels]', path], {
			encoding: 'utf8'
		});

		return { path, output: output.trim() };
	}

	it('produces a 24 bit file magick reads at the right size', () => {
		const { bytes: encoded } = abi.transcode(fixture('derived/base.bmp'), Format.bmp);
		const { output } = identify('rgb.bmp', encoded!);

		expect(output).toMatch(/^320 180 srgb/);
	});

	it('produces pixels magick decodes identically', () => {
		const source = fixture('derived/base.bmp');
		const original = abi.decode(source).image!;

		const { bytes: encoded } = abi.transcode(source, Format.bmp);
		const { path } = identify('compare.bmp', encoded!);

		// straight to raw rgb, so the comparison is our pixels against magick's without a second
		// codec in between
		const raw = execFileSync('magick', [path, '-depth', '8', 'rgb:-'], {
			maxBuffer: 1 << 26
		});

		expect(new Uint8Array(raw)).toEqual(original.pixels);
	});

	it('produces a 32 bit file magick reads as having alpha', () => {
		const { bytes: encoded } = abi.transcode(fixture('derived/base.bmp'), Format.bmp, 4);
		const { output } = identify('rgba.bmp', encoded!);

		expect(output).toMatch(/^320 180 srgba/);
	});

	it('produces a greyscale file magick reads as one channel', () => {
		const { bytes: encoded } = abi.transcode(fixture('derived/base.bmp'), Format.bmp, 1);
		const { path, output } = identify('grey.bmp', encoded!);

		expect(output).toMatch(/^320 180 /);

		const raw = execFileSync('magick', [path, '-depth', '8', 'gray:-'], { maxBuffer: 1 << 26 });
		const ours = abi.transcode(fixture('derived/base.bmp'), Format.bmp, 1);
		const decoded = abi.decode(ours.bytes!).image!;

		// our decode widens the palette back to rgb, so compare magick's grey against one channel
		const grey = new Uint8Array(320 * 180);
		for (let i = 0; i < grey.length; i++) grey[i] = decoded.pixels[i * 3]!;

		expect(new Uint8Array(raw)).toEqual(grey);
	});

	it('reads back a bitmap magick wrote from our own output', () => {
		const { bytes: encoded } = abi.transcode(fixture('derived/base.bmp'), Format.bmp);
		const { path } = identify('reencode.bmp', encoded!);

		const roundTripped = join(directory, 'magick.bmp');
		execFileSync('magick', [path, 'BMP3:' + roundTripped]);

		const { result, image } = abi.decode(new Uint8Array(readFileSync(roundTripped)));
		expect(result).toBe(Err.ok);
		expect(image!.pixels).toEqual(abi.decode(encoded!).image!.pixels);
	});
});
