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
 * A PNG chunk walker written from the format's own documentation rather than from the C.
 */
function chunks(data: Uint8Array): { type: string; length: number; at: number }[] {
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
	const out: { type: string; length: number; at: number }[] = [];

	let at = 8;
	while (at + 12 <= data.byteLength) {
		const length = view.getUint32(at);
		const type = String.fromCharCode(...data.subarray(at + 4, at + 8));

		out.push({ type, length, at });
		if (type === 'IEND') break;

		at += 12 + length;
	}

	return out;
}

describe('the png codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reads a header without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base.png'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.png,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});
	});

	it('reports Adam7 as progressive, since a region cannot be streamed out of it', () => {
		expect(abi.probe(fixture('derived/base-interlaced.png')).info.progressive).toBe(true);
		expect(abi.probe(fixture('derived/base.png')).info.progressive).toBe(false);
	});

	it('reports the file bit depth rather than the decoded one', () => {
		expect(abi.probe(fixture('derived/base-rgb16.png')).info.bitDepth).toBe(16);
		expect(abi.probe(fixture('webassembly.png')).info.bitDepth).toBe(4);
	});

	it('decodes the same pixels a completely different codec produces', async () => {
		// base.png and base.bmp hold one picture in two containers. one stores rows top-down in RGB
		// and the other bottom-up in BGR, so a single shared digest rules out a row order or channel
		// order mistake in either
		const png = abi.decode(fixture('derived/base.png')).image!;
		const bmp = abi.decode(fixture('derived/base.bmp')).image!;

		expect(await sha256(png.pixels)).toBe(golden.reference);
		expect(await sha256(bmp.pixels)).toBe(golden.reference);
	});

	it('decodes 16 bit and interlaced copies to those same pixels', async () => {
		for (const name of [
			'derived/base-rgb8.png',
			'derived/base-rgb16.png',
			'derived/base-interlaced.png'
		]) {
			const image = abi.decode(fixture(name)).image!;

			expect([image.width, image.height, image.channels], name).toEqual([320, 180, 3]);
			expect(await sha256(image.pixels), name).toBe(golden.reference);
		}
	});

	it('applies palette transparency from a four bit indexed file', async () => {
		const image = abi.decode(fixture('webassembly.png')).image!;

		expect([image.width, image.height, image.channels]).toEqual([512, 512, 4]);
		expect(await sha256(image.pixels)).toBe(golden.pngPalette4Bit);

		let clear = 0;
		for (let i = 3; i < image.pixels.length; i += 4) if (image.pixels[i] === 0) clear++;

		// ignoring tRNS would leave nothing transparent, and mapping it to the wrong entry would
		// leave nearly everything transparent
		expect(clear).toBe(37847);
	});

	it('joins a stream split across many IDAT chunks', async () => {
		const source = fixture('forest.png');

		// a decoder that inflated each chunk separately would read this as corrupt
		expect(chunks(source).filter((c) => c.type === 'IDAT').length).toBeGreaterThan(100);

		const image = abi.decode(source).image!;
		expect([image.width, image.height, image.channels]).toEqual([2000, 831, 4]);
		expect(await sha256(image.pixels)).toBe(golden.pngMultiIdat);
	});

	it('box averages a scaled decode to the same pixels the bmp path gives', async () => {
		const image = abi.decode(fixture('derived/base.png'), (i, b, n) =>
			abi.exports.tiny_image_load_scaled(i, b, n, 40, 20)
		).image!;

		expect([image.width, image.height]).toEqual([40, 23]);
		expect(await sha256(image.pixels)).toBe(golden.referenceEighth);
	});

	it('decodes a region equal to the same rectangle of a full decode', () => {
		const source = fixture('derived/base.png');
		const full = abi.decode(source).image!;

		const region = abi.decode(source, (i, b, n) =>
			abi.exports.tiny_image_load_region(i, b, n, 37, 21, 64, 48)
		).image!;

		expect([region.width, region.height]).toEqual([64, 48]);

		for (let y = 0; y < 48; y++) {
			const wanted = full.pixels.subarray(
				((y + 21) * 320 + 37) * 3,
				((y + 21) * 320 + 37 + 64) * 3
			);
			expect(region.pixels.subarray(y * 64 * 3, (y + 1) * 64 * 3), `row ${y}`).toEqual(
				wanted
			);
		}
	});

	it('round trips every channel count without losing a pixel', () => {
		for (const channels of [1, 2, 3, 4]) {
			const original = abi.decode(fixture('derived/base.png'), (i, b, n) => {
				const r = abi.exports.tiny_image_load(i, b, n);
				return r === Err.ok ? abi.exports.tiny_image_convert_channels(i, channels) : r;
			}).image!;

			const { result, bytes: encoded } = abi.transcode(
				fixture('derived/base.png'),
				Format.png,
				channels
			);
			expect(result, `${channels} channels`).toBe(Err.ok);

			const back = abi.decode(encoded!).image!;
			expect(back.channels, `${channels} channels`).toBe(channels);
			expect(back.pixels, `${channels} channels`).toEqual(original.pixels);
		}
	});

	it('writes a well formed chunk sequence', () => {
		const { bytes: encoded } = abi.transcode(fixture('derived/base.png'), Format.png);
		const walked = chunks(encoded!);

		expect(walked[0]!.type).toBe('IHDR');
		expect(walked[walked.length - 1]!.type).toBe('IEND');
		expect(walked.some((c) => c.type === 'IDAT')).toBe(true);

		// every chunk's CRC has to check out, computed here rather than trusted
		const view = new DataView(encoded!.buffer, encoded!.byteOffset);
		for (const { type, length, at } of walked) {
			const stored = view.getUint32(at + 8 + length);
			expect(crc32(encoded!.subarray(at + 4, at + 8 + length)), type).toBe(stored);
		}
	});

	it('names the reason a file cannot be decoded', () => {
		expect(abi.decode(fixture('derived/malformed/truncated.png')).result).toBe(Err.corrupt);
		expect(abi.decode(fixture('derived/malformed/signature-only.png')).result).toBe(
			Err.corrupt
		);

		// the absurd dimensions fixture has a deliberately broken IHDR checksum, so corrupt is the
		// honest answer: the size field it carries has already failed its own check
		expect(abi.decode(fixture('derived/malformed/absurd-dimensions.png')).result).toBe(
			Err.corrupt
		);
	});

	it('catches a single flipped byte anywhere in the file', () => {
		const source = fixture('derived/base.png');

		for (const at of [20, 100, Math.floor(source.length / 2), source.length - 20]) {
			const tampered = new Uint8Array(source);
			tampered[at]! ^= 0xff;

			expect(abi.decode(tampered).result, `byte ${at}`).toBe(Err.corrupt);
		}
	});
});

/** CRC-32 with the reflected polynomial PNG uses, written here so the check is independent. */
function crc32(data: Uint8Array): number {
	let crc = 0xffffffff;

	for (const byte of data) {
		crc ^= byte;
		for (let bit = 0; bit < 8; bit++) {
			crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
		}
	}

	return (crc ^ 0xffffffff) >>> 0;
}

describe.skipIf(!hasMagick())('the png encoder against imagemagick', () => {
	let abi: TinyAbi;
	let directory: string;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
		directory = mkdtempSync(join(tmpdir(), 'tinyimg-png-'));
	});

	/** Writes our output and asks magick for its raw samples, which is what a third party sees. */
	function samples(name: string, encoded: Uint8Array, format: string): Uint8Array {
		const path = join(directory, name);
		writeFileSync(path, encoded);

		return new Uint8Array(
			execFileSync('magick', [path, '-depth', '8', `${format}:-`], { maxBuffer: 1 << 26 })
		);
	}

	it.each([
		['rgb', 3],
		['gray', 1],
		['graya', 2],
		['rgba', 4]
	])('produces a %s file magick decodes to our own pixels', (format, channels) => {
		const ours = abi.transcode(fixture('derived/base.png'), Format.png, channels);
		expect(ours.result).toBe(Err.ok);

		const decoded = abi.decode(ours.bytes!).image!;
		expect(samples(`out-${format}.png`, ours.bytes!, format)).toEqual(decoded.pixels);
	});

	it('produces flat artwork no larger than magick does', () => {
		// the unfiltered candidate exists for this case: scoring a row by how close its bytes are to
		// zero says nothing about whether LZ77 could match it against its neighbours
		const ours = abi.transcode(fixture('derived/logo.png'), Format.png, 4);
		const source = fixture('derived/logo.png');

		expect(ours.bytes!.byteLength).toBeLessThan(source.byteLength);
	});

	it('reads back a png magick wrote from our own output', () => {
		const ours = abi.transcode(fixture('derived/base.png'), Format.png);
		const path = join(directory, 'reencode.png');
		writeFileSync(path, ours.bytes!);

		const round = join(directory, 'magick.png');
		execFileSync('magick', [path, round]);

		const { result, image } = abi.decode(new Uint8Array(readFileSync(round)));
		expect(result).toBe(Err.ok);
		expect(image!.pixels).toEqual(abi.decode(ours.bytes!).image!.pixels);
	});
});
