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
 * A marker walker written from the format's own definition rather than from the C.
 *
 * Skipping an entropy coded segment means finding the next marker that is neither a stuffed zero
 * nor a restart, which is the same rule the decoder applies and the reason it is worth restating
 * here: two independent implementations of the rule agreeing is what makes the scan counts below
 * evidence rather than assertion.
 */
function markers(data: Uint8Array): { marker: number; at: number }[] {
	const out: { marker: number; at: number }[] = [];
	let at = 2;

	while (at + 1 < data.byteLength) {
		if (data[at] !== 0xff) {
			at++;
			continue;
		}

		const marker = data[at + 1]!;

		if (marker === 0xff || marker === 0x00) {
			at++;
			continue;
		}

		out.push({ marker, at });
		if (marker === 0xd9) break;

		const length = (data[at + 2]! << 8) | data[at + 3]!;

		if (marker === 0xda) {
			at += 2 + length;

			while (at + 1 < data.byteLength) {
				const next = data[at + 1]!;
				const boundary =
					data[at] === 0xff &&
					next !== 0x00 &&
					next !== 0xff &&
					!(next >= 0xd0 && next <= 0xd7);

				if (boundary) break;
				at++;
			}
			continue;
		}

		at += 2 + length;
	}

	return out;
}

describe('the jpeg codec inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('reads a header without decoding it', () => {
		const { result, info } = abi.probe(fixture('derived/base-444.jpg'));

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,
			frames: 1,
			format: Format.jpeg,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});
	});

	it('reports a progressive stream as one', () => {
		expect(abi.probe(fixture('derived/base-progressive.jpg')).info.progressive).toBe(true);
		expect(abi.probe(fixture('road.jpg')).info.progressive).toBe(true);
		expect(abi.probe(fixture('sf-24.jpg')).info.progressive).toBe(false);
	});

	it('matches what libjpeg produces for every subsampling', async () => {
		const cases: [string, keyof typeof golden, number][] = [
			['derived/base-444.jpg', 'jpeg444', 3],
			['derived/base-422.jpg', 'jpeg422', 3],
			['derived/base-420.jpg', 'jpeg420', 3],
			['derived/base-411.jpg', 'jpeg411', 3],
			['derived/base-cmyk.jpg', 'jpegCmyk', 3],
			['derived/base-gray.jpg', 'jpegGray', 1]
		];

		for (const [name, key, channels] of cases) {
			const image = abi.decode(fixture(name)).image!;

			expect([image.width, image.height, image.channels], name).toEqual([320, 180, channels]);
			expect(await sha256(image.pixels), name).toBe(golden[key]);
		}
	});

	it('decodes a progressive stream to the same pixels as the sequential one', async () => {
		const sequential = abi.decode(fixture('derived/base-444.jpg')).image!;
		const progressive = abi.decode(fixture('derived/base-progressive.jpg')).image!;

		// the two fixtures differ only in how the coefficients are split across scans, so any
		// error in spectral selection, successive approximation or the EOB run breaks this
		expect(progressive.pixels).toEqual(sequential.pixels);
		expect(await sha256(progressive.pixels)).toBe(golden.jpeg444);

		// and the progressive file really is one, with ten scans against the other's one
		expect(
			markers(fixture('derived/base-progressive.jpg')).filter((m) => m.marker === 0xda)
		).toHaveLength(10);
		expect(
			markers(fixture('derived/base-444.jpg')).filter((m) => m.marker === 0xda)
		).toHaveLength(1);
	});

	it('resynchronizes on restart markers', async () => {
		const source = fixture('derived/base-restart.jpg');

		// the fixture carries a restart interval, so the decoder has to step over the markers
		expect(markers(source).some((m) => m.marker === 0xdd)).toBe(true);

		const image = abi.decode(source).image!;

		// and land on exactly the same pixels as the same image without them
		expect(await sha256(image.pixels)).toBe(golden.jpeg420);
	});

	it('decodes the reference photograph and the progressive one', async () => {
		const photo = abi.decode(fixture('sf-24.jpg')).image!;
		expect([photo.width, photo.height]).toEqual([1835, 1032]);
		expect(await sha256(photo.pixels)).toBe(golden.jpegPhoto);

		const road = abi.decode(fixture('road.jpg')).image!;
		expect([road.width, road.height]).toEqual([1281, 1920]);
		expect(await sha256(road.pixels)).toBe(golden.jpegProgressivePhoto);
	});

	it('scales in the dct domain down to an eighth', async () => {
		const image = abi.decode(fixture('sf-24.jpg'), (i, b, n) =>
			abi.exports.tiny_image_load_scaled(i, b, n, 1, 1)
		).image!;

		expect([image.width, image.height]).toEqual([230, 129]);
		expect(await sha256(image.pixels)).toBe(golden.jpegPhotoEighth);
	});

	it('decodes a region equal to the same rectangle of a full decode', () => {
		for (const name of ['derived/base-420.jpg', 'derived/base-progressive.jpg']) {
			const source = fixture(name);
			const full = abi.decode(source).image!;

			// an odd offset on purpose: the chroma filter reaches a sample either side, so a
			// region starting mid chroma sample only comes out right if the plane window carries
			// the row and column outside it
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

	it('reads an oversized header and decodes it scaled', () => {
		const source = fixture('derived/oversized.jpg');
		const { info } = abi.probe(source);

		expect([info.width, info.height]).toEqual([5000, 4000]);

		// past the pixel budget a full decode says so, rather than running out of memory
		expect(abi.decode(source).result).toBe(Err.tooLarge);

		// the loader picks the smallest scale that still covers the box, so a 400 square box
		// lands on an eighth and a 700 square one cannot, since an eighth is only 500 tall
		const eighth = abi.decode(source, (i, b, n) =>
			abi.exports.tiny_image_load_scaled(i, b, n, 400, 400)
		).image!;
		expect([eighth.width, eighth.height]).toEqual([625, 500]);

		const quarter = abi.decode(source, (i, b, n) =>
			abi.exports.tiny_image_load_scaled(i, b, n, 700, 700)
		).image!;
		expect([quarter.width, quarter.height]).toEqual([1250, 1000]);
	});

	it('rejects what it cannot read and survives what it can partly read', () => {
		expect(abi.decode(fixture('derived/malformed/not-an-image.bin')).result).toBe(
			Err.unknownFormat
		);
		expect(abi.decode(fixture('derived/malformed/signature-only.jpg')).result).toBe(
			Err.unknownFormat
		);
		expect(abi.decode(fixture('derived/malformed/header-only.jpg')).result).toBe(Err.corrupt);

		// a truncated scan decodes: the format is built so a reader can stop early, and the
		// entropy decoder reads the missing bits as zeros
		const truncated = abi.decode(fixture('derived/malformed/truncated.jpg')).image!;
		expect([truncated.width, truncated.height]).toEqual([320, 180]);
	});

	it('encodes what its own decoder and libjpeg both read back', async () => {
		const source = fixture('derived/base.png');
		const original = abi.decode(source).image!;

		const { result, bytes: encoded } = abi.transcode(source, Format.jpeg, 3, {
			quality: 85
		});

		expect(result).toBe(Err.ok);
		expect(encoded!.byteLength).toBeGreaterThan(0);

		const { info } = abi.probe(encoded!);
		expect([info.width, info.height, info.channels]).toEqual([320, 180, 3]);
		expect(info.progressive).toBe(false);

		const back = abi.decode(encoded!).image!;
		expect([back.width, back.height, back.channels]).toEqual([320, 180, 3]);

		// lossy, so the check is a floor rather than a digest; quality 85 on a photograph is a
		// well known place to be
		let sum = 0;
		for (let i = 0; i < back.pixels.length; i++) {
			const diff = back.pixels[i]! - original.pixels[i]!;
			sum += diff * diff;
		}

		const psnr = 10 * Math.log10((255 * 255 * back.pixels.length) / sum);
		expect(psnr).toBeGreaterThan(32);
	});

	it('spends quality on size in both directions', () => {
		const source = fixture('derived/base.png');
		let previous = 0;

		for (const quality of [40, 60, 85, 92]) {
			const { result, bytes: encoded } = abi.transcode(source, Format.jpeg, 3, {
				quality
			});

			expect(result, `quality ${quality}`).toBe(Err.ok);
			expect(encoded!.byteLength, `quality ${quality}`).toBeGreaterThan(previous);
			previous = encoded!.byteLength;
		}
	});

	it('writes a progressive stream that decodes to the baseline pixels', () => {
		for (const channels of [1, 3]) {
			const source = fixture('derived/base.png');

			const flat = abi.transcode(source, Format.jpeg, channels, { quality: 85 });
			const staged = abi.transcode(source, Format.jpeg, channels, {
				quality: 85,
				progressive: true
			});

			expect(flat.result).toBe(Err.ok);
			expect(staged.result).toBe(Err.ok);

			expect(abi.probe(staged.bytes!).info.progressive, `${channels} channels`).toBe(true);
			expect(abi.probe(flat.bytes!).info.progressive, `${channels} channels`).toBe(false);

			// the same coefficients through different entropy coding, so the pixels cannot
			// differ; this is what caught a gray scan script that never finished DC
			// successive approximation and quietly lost the DC band's lowest bit
			const a = abi.decode(flat.bytes!).image!;
			const b = abi.decode(staged.bytes!).image!;

			expect(b.pixels, `${channels} channels`).toEqual(a.pixels);
		}
	});

	it('drops the alpha it cannot carry and keeps a single channel single', () => {
		const source = fixture('derived/base-alpha.png');

		for (const [channels, expected] of [
			[1, 1],
			[2, 1],
			[3, 3],
			[4, 3]
		]) {
			const { result, bytes: encoded } = abi.transcode(source, Format.jpeg, channels);

			expect(result, `${channels} channels`).toBe(Err.ok);
			expect(abi.probe(encoded!).info.channels, `${channels} channels`).toBe(expected);
		}
	});

	it('encodes dimensions that are not whole blocks', () => {
		for (const name of ['derived/single-pixel.png', 'derived/tiny-odd.png']) {
			const source = fixture(name);
			const original = abi.decode(source).image!;

			for (const progressive of [false, true]) {
				const { result, bytes: encoded } = abi.transcode(source, Format.jpeg, 3, {
					quality: 90,
					progressive
				});

				expect(result, name).toBe(Err.ok);

				const back = abi.decode(encoded!).image!;
				expect([back.width, back.height], name).toEqual([original.width, original.height]);
			}
		}
	});

	it('reads the exif block and reports the orientation without applying it', () => {
		const rotated = abi.decode(fixture('derived/base-exif-rotated.jpg')).image!;

		expect(rotated.meta.hasExif).toBe(true);
		expect(rotated.meta.orientation).toBe('6');
		expect(rotated.meta.count).toBe(1);

		// the file says rotate 90 clockwise and the pixels are still as stored; the rotation
		// belongs in the plan, where a region can be walked back through it
		expect([rotated.width, rotated.height]).toEqual([320, 180]);

		// the payload starts at its own TIFF header, which is what every other library means by
		// the EXIF block
		const header = String.fromCharCode(rotated.meta.exif![0]!, rotated.meta.exif![1]!);
		expect(['MM', 'II']).toContain(header);

		const upright = abi.decode(fixture('derived/base-exif.jpg')).image!;
		expect(upright.meta.orientation).toBe('1');

		// a file with no APP1 carries no metadata at all
		const bare = abi.decode(fixture('derived/base-444.jpg')).image!;
		expect(bare.meta.hasExif).toBe(false);
		expect(bare.meta.count).toBe(0);
	});

	it('carries the exif block through a re-encode, or drops it when asked', () => {
		const source = fixture('derived/base-exif-rotated.jpg');
		const original = abi.decode(source).image!;

		const kept = abi.transcode(source, Format.jpeg, 3, { quality: 85 });
		const dropped = abi.transcode(source, Format.jpeg, 3, {
			quality: 85,
			stripMetadata: true
		});

		expect(kept.result).toBe(Err.ok);
		expect(dropped.result).toBe(Err.ok);

		// the difference between the two files is the segment and nothing else
		expect(kept.bytes!.byteLength - dropped.bytes!.byteLength).toBe(
			original.meta.exif!.byteLength + 10
		);

		const back = abi.decode(kept.bytes!).image!;
		expect(back.meta.hasExif).toBe(true);
		expect(back.meta.exif).toEqual(original.meta.exif);
		expect(back.meta.orientation).toBe('6');

		const without = abi.decode(dropped.bytes!).image!;
		expect(without.meta.hasExif).toBe(false);

		// and dropping it leaves the pixels alone
		expect(without.pixels).toEqual(back.pixels);
	});

	it('gives back what a large decode borrowed', () => {
		// road.jpg is progressive, so it holds a whole coefficient plane as well as the samples,
		// which is the largest thing this codec ever asks for
		const image = abi.decode(fixture('road.jpg')).image!;
		expect(image.width).toBe(1281);

		const after = abi.pages;

		// a second decode of the same file must not need another page, or the arena is leaking.
		// Asserting growth instead would only measure which test ran first
		abi.decode(fixture('road.jpg'));
		expect(abi.pages).toBe(after);

		// and a region of it costs no more than the whole, which is the point of the plane window
		abi.decode(fixture('road.jpg'), (i, b, n) =>
			abi.exports.tiny_image_load_region(i, b, n, 300, 400, 128, 128)
		);
		expect(abi.pages).toBe(after);
	});
});
