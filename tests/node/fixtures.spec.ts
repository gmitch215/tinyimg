import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const FIXTURES = join(import.meta.dirname, '..', 'fixtures');
const DERIVED = join(FIXTURES, 'derived');

/** Mirrors TINYIMG_MAX_PIXELS, which bounds decode time. */
const MAX_PIXELS = 16_000_000;

/** Mirrors TINYIMG_MAX_IMAGE_BYTES, which is the cap a whole-image decode actually runs into. */
const MAX_IMAGE_BYTES = 33_554_432;

/**
 * Reads dimensions straight out of a JPEG or PNG header.
 *
 * Test-only and deliberately independent of `tiny_image_probe`: a reference set validated by the code
 * it is meant to be a reference for proves nothing.
 */
function dimensions(bytes: Buffer): { width: number; height: number } {
	if (bytes.readUInt32BE(0) === 0x89504e47) {
		return { width: bytes.readUInt32BE(16), height: bytes.readUInt32BE(20) };
	}

	if (bytes.readUInt16BE(0) === 0xffd8) {
		let at = 2;
		while (at + 9 < bytes.length) {
			if (bytes[at] !== 0xff) {
				at++;
				continue;
			}
			const marker = bytes[at + 1]!;
			// every SOFn except DHT, JPG and DAC, which share the 0xc0-0xcf range
			if (marker >= 0xc0 && marker <= 0xcf && ![0xc4, 0xc8, 0xcc].includes(marker)) {
				return { width: bytes.readUInt16BE(at + 7), height: bytes.readUInt16BE(at + 5) };
			}
			at += 2 + bytes.readUInt16BE(at + 2);
		}
	}

	throw new Error('not a jpeg or png');
}

function hasMarker(bytes: Buffer, marker: number): boolean {
	let at = 2;
	while (at + 4 < bytes.length) {
		if (bytes[at] !== 0xff) {
			at++;
			continue;
		}
		if (bytes[at + 1] === marker) return true;
		const length = bytes.readUInt16BE(at + 2);
		if (length < 2) return false;
		at += 2 + length;
	}
	return false;
}

/**
 * Lists a PNG's chunk types in order.
 *
 * Independent of the C, like `dimensions` above.
 */
function chunks(bytes: Buffer): string[] {
	const names: string[] = [];

	let at = 8;
	while (at + 12 <= bytes.length) {
		const length = bytes.readUInt32BE(at);
		const type = bytes.subarray(at + 4, at + 8).toString('ascii');

		names.push(type);
		if (type === 'IEND') break;

		at += 12 + length;
	}

	return names;
}

/** The logical screen, which is the extent a viewer shows whatever the first frame's size is. */
function gifExtent(bytes: Buffer): { width: number; height: number } {
	return { width: bytes.readUInt16LE(6), height: bytes.readUInt16LE(8) };
}

/** Frames a GIF holds, counted from its block structure rather than from our own probe. */
function gifFrames(bytes: Buffer): number {
	let at = 13;

	if ((bytes[10]! & 0x80) !== 0) at += 3 * (1 << ((bytes[10]! & 0x07) + 1));

	let count = 0;

	while (at < bytes.length) {
		const block = bytes[at]!;

		if (block === 0x3b) break;

		if (block === 0x21) {
			at += 2;
			while (at < bytes.length && bytes[at] !== 0) at += bytes[at]! + 1;
			at++;
			continue;
		}

		if (block !== 0x2c) break;

		count++;
		const packed = bytes[at + 9]!;
		at += 10;
		if ((packed & 0x80) !== 0) at += 3 * (1 << ((packed & 0x07) + 1));
		at++;
		while (at < bytes.length && bytes[at] !== 0) at += bytes[at]! + 1;
		at++;
	}

	return count;
}

/** Tags 256 and 257 out of the first IFD, both byte orders. */
function tiffExtent(bytes: Buffer): { width: number; height: number } {
	const little = bytes.subarray(0, 2).toString('ascii') === 'II';
	const u16 = (at: number) => (little ? bytes.readUInt16LE(at) : bytes.readUInt16BE(at));
	const u32 = (at: number) => (little ? bytes.readUInt32LE(at) : bytes.readUInt32BE(at));

	const ifd = u32(4);
	const entries = u16(ifd);
	let width = 0;
	let height = 0;

	for (let i = 0; i < entries; i++) {
		const at = ifd + 2 + i * 12;
		const tag = u16(at);
		// a short fits in the value field, which is why the type decides the read width
		const value = u16(at + 2) === 3 ? u16(at + 8) : u32(at + 8);

		if (tag === 256) width = value;
		if (tag === 257) height = value;
	}

	return { width, height };
}

/** The canvas extent, across all three chunk layouts the container allows. */
function webpExtent(bytes: Buffer): { width: number; height: number } {
	const chunk = bytes.subarray(12, 16).toString('ascii');

	if (chunk === 'VP8X') {
		return {
			width: (bytes.readUIntLE(24, 3) & 0xffffff) + 1,
			height: (bytes.readUIntLE(27, 3) & 0xffffff) + 1
		};
	}

	if (chunk === 'VP8L') {
		const bits = bytes.readUInt32LE(21);
		return { width: (bits & 0x3fff) + 1, height: ((bits >> 14) & 0x3fff) + 1 };
	}

	// lossy: past the 3-byte frame tag and the 0x9d012a start code
	return { width: bytes.readUInt16LE(26) & 0x3fff, height: bytes.readUInt16LE(28) & 0x3fff };
}

/** Any source fixture's extent, dispatched on its magic bytes. */
function extentOf(name: string, bytes: Buffer): { width: number; height: number } {
	if (/\.gif$/i.test(name)) return gifExtent(bytes);
	if (/\.tiff?$/i.test(name)) return tiffExtent(bytes);
	if (/\.webp$/i.test(name)) return webpExtent(bytes);

	return dimensions(bytes);
}

function sources(): string[] {
	return readdirSync(FIXTURES)
		.filter((name) => /\.(jpg|jpeg|png|webp|gif|tiff)$/i.test(name))
		.sort();
}

describe('the source fixtures', () => {
	it('has the set the tests name', () => {
		expect(sources()).toEqual([
			'ball_kick.gif',
			'budapest.jpg',
			'dartmouth.jpg',
			'dartmouth.tiff',
			'digicam.jpg',
			'dog.jpg',
			'face_art.jpg',
			'family.jpg',
			'flower.jpg',
			'forest.png',
			'man.jpg',
			'moped.jpg',
			'mountains.jpg',
			'mushroom.jpg',
			'road.jpg',
			'sf-24.jpg',
			'smile.jpg',
			'toyota_racing.webp',
			'webassembly.png',
			'winter_cabin.jpg',
			'winter_forest.png',
			'woman.jpg'
		]);
	});

	it('is all within both budgets, so scripts/fixtures.ts has run', () => {
		for (const name of sources()) {
			const { width, height } = extentOf(name, readFileSync(join(FIXTURES, name)));
			const where = `${name} is ${width}x${height}`;

			expect(width * height, where).toBeLessThanOrEqual(MAX_PIXELS);
			// the cap that actually binds, and it binds on bytes: three channels is the least any
			// of these decodes to, so a fixture failing here cannot be whole-image decoded at all
			expect(width * height * 3, where).toBeLessThanOrEqual(MAX_IMAGE_BYTES);
		}
	});

	it('keeps digicam.jpg at the extent it was measured at', () => {
		// it exists to be the largest source the library supports, so a shrink would remove the only
		// thing it tests. 9.72 Mpx is 29.2 MB of RGB against the 32 MiB cap, and the next step up
		// reaches 98.9% of it
		const { width, height } = extentOf(
			'digicam.jpg',
			readFileSync(join(FIXTURES, 'digicam.jpg'))
		);

		expect({ width, height }).toEqual({ width: 3600, height: 2700 });
		expect(width * height * 3).toBeLessThanOrEqual(MAX_IMAGE_BYTES);
		expect((width * height * 3) / MAX_IMAGE_BYTES).toBeGreaterThan(0.8);
	});

	it('has an animation, which is what a gif is in production', () => {
		// the still gifs are all generated; this is the case a caller actually passes, and the one
		// that decides whether a no-op request keeps 57 frames or flattens them to 1
		const bytes = readFileSync(join(FIXTURES, 'ball_kick.gif'));

		expect(bytes.subarray(0, 6).toString('ascii')).toBe('GIF89a');
		expect(gifFrames(bytes)).toBe(57);
		expect(gifExtent(bytes)).toEqual({ width: 800, height: 600 });
	});

	it('has a lossy webp nobody in this repo encoded', () => {
		// every other webp fixture comes out of our encoder or cwebp driven by our script, so all of
		// them share whatever assumptions those make. this one arrived from the wild
		const bytes = readFileSync(join(FIXTURES, 'toyota_racing.webp'));

		expect(bytes.subarray(0, 4).toString('ascii')).toBe('RIFF');
		expect(bytes.subarray(8, 12).toString('ascii')).toBe('WEBP');
		expect(bytes.subarray(12, 16).toString('ascii'), 'a bare VP8 chunk, so lossy').toBe('VP8 ');
		expect(webpExtent(bytes)).toEqual({ width: 1000, height: 666 });
	});

	it('has the same picture as an uncompressed tiff and the jpeg it came from', () => {
		// two containers over one image, so a decode difference between them is the codec rather
		// than the content. the tiff is uncompressed, which is the strip layout with no filter in it
		const tiff = readFileSync(join(FIXTURES, 'dartmouth.tiff'));
		const jpeg = readFileSync(join(FIXTURES, 'dartmouth.jpg'));

		expect(tiff.subarray(0, 4)).toEqual(Buffer.from([0x49, 0x49, 0x2a, 0x00]));
		expect(tiffExtent(tiff)).toEqual({ width: 250, height: 187 });
		expect(dimensions(jpeg)).toEqual({ width: 250, height: 187 });

		// uncompressed, so the pixel data alone accounts for almost the whole file
		expect(tiff.length).toBeGreaterThan(250 * 187 * 3);
	});

	it('keeps road.jpg the only progressive fixture', () => {
		// if a second one appeared, the bounded-memory limit would stop being isolated to one file
		const progressive = sources()
			.filter((name) => name.endsWith('.jpg'))
			.filter((name) => hasMarker(readFileSync(join(FIXTURES, name)), 0xc2));

		expect(progressive).toEqual(['road.jpg']);
	});

	it('has two sources that were png to begin with', () => {
		// every derived png comes from a jpeg, so it carries photographic noise in every pixel and
		// exercises only the truecolor path. these two were written as pngs, and between them they
		// reach three things the derived set cannot: a sub-byte bit depth, palette transparency, and
		// a stream split across many chunks
		for (const name of ['webassembly.png', 'forest.png', 'winter_forest.png']) {
			const bytes = readFileSync(join(FIXTURES, name));

			expect(bytes.readUInt32BE(0), name).toBe(0x89504e47);
			expect(bytes.subarray(12, 16).toString('ascii'), name).toBe('IHDR');
		}

		const logo = readFileSync(join(FIXTURES, 'webassembly.png'));
		expect(logo.readUInt8(24), 'bit depth').toBe(4);
		expect(logo.readUInt8(25), 'color type 3 is indexed').toBe(3);

		const logoChunks = chunks(logo);
		expect(logoChunks).toContain('PLTE');
		expect(logoChunks, 'palette transparency, which no derived fixture has').toContain('tRNS');

		const forest = readFileSync(join(FIXTURES, 'forest.png'));
		expect(forest.readUInt8(24), 'bit depth').toBe(8);
		expect(forest.readUInt8(25), 'color type 6 is truecolor with alpha').toBe(6);

		// a decoder that inflates each IDAT separately instead of concatenating them first reads
		// this file as corrupt, and nothing else in the set would show it
		const idats = chunks(forest).filter((name) => name === 'IDAT');
		expect(idats.length).toBeGreaterThan(1);

		// its illustrated counterpart carries the same color type in a single chunk, so the two
		// together cover both shapes of the same stream
		const illustrated = readFileSync(join(FIXTURES, 'winter_forest.png'));
		expect(illustrated.readUInt8(25), 'color type 6 is truecolor with alpha').toBe(6);
		expect(chunks(illustrated).filter((name) => name === 'IDAT')).toHaveLength(1);
	});

	it('has the face fixtures the detector is measured on', () => {
		// frontal, profile, stylized, two cartoons, and a group whose faces are small enough to sit
		// near the cascade's minimum window
		for (const name of [
			'smile.jpg',
			'man.jpg',
			'face_art.jpg',
			'winter_cabin.jpg',
			'moped.jpg',
			'family.jpg'
		]) {
			expect(existsSync(join(FIXTURES, name)), name).toBe(true);
		}
	});

	it('has an illustration of a person carrying no face at all', () => {
		// the false positive case that matters most: a detector keyed on body shape rather than on
		// facial texture would fire here, and nothing else in the set would catch it
		expect(existsSync(join(FIXTURES, 'woman.jpg'))).toBe(true);
	});
});

describe('the derived reference set', () => {
	const required = [
		'base.png',
		'base-alpha.png',
		'base-gray.png',
		'base-rgb8.png',
		'base-rgba8.png',
		'base-gray8.png',
		'base-gray-alpha8.png',
		'base-rgb16.png',
		'base-palette.png',
		'base-interlaced.png',
		'base.bmp',
		'base-rle8.bmp',
		'base.gif',
		'base-interlaced.gif',
		'base-uncompressed.tif',
		'base-packbits.tif',
		'base-lzw.tif',
		'base-deflate.tif',
		'base-444.jpg',
		'base-422.jpg',
		'base-420.jpg',
		'base-progressive.jpg',
		'base-gray.jpg',
		'base-cmyk.jpg',
		'base-lossy.webp',
		'base-lossless.webp',
		'base-alpha.webp',
		'base.avif',
		'tiny-odd.png',
		'tiny-odd.jpg',
		'oversized.jpg',
		'flat.png',
		'single-pixel.png',
		'trim.png',
		'logo.png',
		'subject.png'
	];

	it('exists, so the differential tests have something to compare against', () => {
		expect(existsSync(DERIVED)).toBe(true);
	});

	it.each(required)('has %s, non-empty', (name) => {
		const path = join(DERIVED, name);
		expect(existsSync(path), path).toBe(true);
		expect(statSync(path).size).toBeGreaterThan(0);
	});

	it('has the reference at the size the codec tests assume', () => {
		expect(dimensions(readFileSync(join(DERIVED, 'base.png')))).toEqual({
			width: 320,
			height: 180
		});
	});

	it('has an oversized fixture that really is over the budget', () => {
		const { width, height } = dimensions(readFileSync(join(DERIVED, 'oversized.jpg')));
		expect(width * height).toBeGreaterThan(MAX_PIXELS);
	});

	it('has odd dimensions on both axes, where chroma subsampling breaks', () => {
		const { width, height } = dimensions(readFileSync(join(DERIVED, 'tiny-odd.png')));
		expect(width % 2).toBe(1);
		expect(height % 2).toBe(1);
	});

	it('marks base-progressive.jpg progressive and base-420.jpg baseline', () => {
		expect(hasMarker(readFileSync(join(DERIVED, 'base-progressive.jpg')), 0xc2)).toBe(true);
		expect(hasMarker(readFileSync(join(DERIVED, 'base-420.jpg')), 0xc0)).toBe(true);
	});

	it('has a real avif container for probe to read', () => {
		const bytes = readFileSync(join(DERIVED, 'base.avif'));
		expect(bytes.subarray(4, 8).toString('ascii')).toBe('ftyp');
		expect(bytes.subarray(8, 12).toString('ascii')).toBe('avif');
	});
});

describe('the crop manifest', () => {
	const manifest = JSON.parse(readFileSync(join(DERIVED, 'ref', 'crops.json'), 'utf8')) as {
		side: number;
		scaledLongSide: number;
		crops: Record<
			string,
			{
				x: number;
				y: number;
				width: number;
				height: number;
				sourceWidth: number;
				sourceHeight: number;
			}
		>;
	};

	it('covers every source fixture', () => {
		expect(Object.keys(manifest.crops).sort()).toEqual(sources());
	});

	it('records the dimensions the fixtures actually have', () => {
		for (const [name, crop] of Object.entries(manifest.crops)) {
			const { width, height } = extentOf(name, readFileSync(join(FIXTURES, name)));
			expect({ name, width: crop.sourceWidth, height: crop.sourceHeight }).toEqual({
				name,
				width,
				height
			});
		}
	});

	it('keeps every crop inside its source', () => {
		for (const [name, crop] of Object.entries(manifest.crops)) {
			expect(crop.x + crop.width, name).toBeLessThanOrEqual(crop.sourceWidth);
			expect(crop.y + crop.height, name).toBeLessThanOrEqual(crop.sourceHeight);
		}
	});

	it('has a reference image per fixture, at the recorded size', () => {
		for (const name of sources()) {
			const stem = name.replace(/\.[^.]+$/, '');
			const crop = join(DERIVED, 'ref', `${stem}.crop.png`);
			const scaled = join(DERIVED, 'ref', `${stem}.${manifest.scaledLongSide}.png`);

			expect(existsSync(crop), crop).toBe(true);
			expect(existsSync(scaled), scaled).toBe(true);

			expect(dimensions(readFileSync(crop))).toEqual({
				width: manifest.side,
				height: manifest.side
			});

			// the reference resize only ever shrinks, so a source already inside the target keeps its
			// own long side; dartmouth.jpg at 250 is the one fixture that reaches this
			const crops = manifest.crops[name]!;
			const source = Math.max(crops.sourceWidth, crops.sourceHeight);
			const size = dimensions(readFileSync(scaled));

			expect(Math.max(size.width, size.height), name).toBe(
				Math.min(manifest.scaledLongSide, source)
			);
		}
	});
});

describe('the malformed set', () => {
	const dir = join(DERIVED, 'malformed');

	it('is actually malformed, not merely present', () => {
		const whole = statSync(join(DERIVED, 'base-420.jpg')).size;

		expect(statSync(join(dir, 'empty.jpg')).size).toBe(0);
		expect(statSync(join(dir, 'header-only.jpg')).size).toBe(4);
		expect(statSync(join(dir, 'truncated.jpg')).size).toBeLessThan(whole);
	});

	it('has a valid signature over garbage, which a sniff-only guard would accept', () => {
		const jpeg = readFileSync(join(dir, 'signature-only.jpg'));
		expect(jpeg.readUInt16BE(0)).toBe(0xffd8);
		expect(jpeg.subarray(2).every((byte) => byte === 0x5a)).toBe(true);

		const png = readFileSync(join(dir, 'signature-only.png'));
		expect(png.readUInt32BE(0)).toBe(0x89504e47);
	});

	it('has a png claiming a size no data could fill', () => {
		expect(readFileSync(join(dir, 'absurd-dimensions.png')).readUInt32BE(16)).toBe(0x7fffffff);
	});
});

describe('the generated color profiles', () => {
	const ids = ['srgb', 'display-p3', 'adobe-rgb-1998', 'rec2020'];

	it.each(ids)('%s is a well-formed icc profile', (id) => {
		const bytes = readFileSync(join(DERIVED, 'icc', `${id}.icc`));

		// the header's own size field must match the file, and 'acsp' must sit at offset 36
		expect(bytes.readUInt32BE(0)).toBe(bytes.length);
		expect(bytes.subarray(36, 40).toString('ascii')).toBe('acsp');
		expect(bytes.subarray(16, 20).toString('ascii')).toBe('RGB ');
		expect(bytes.subarray(20, 24).toString('ascii')).toBe('XYZ ');

		// every tag offset and size has to land inside the file
		const tags = bytes.readUInt32BE(128);
		expect(tags).toBeGreaterThan(0);
		for (let i = 0; i < tags; i++) {
			const base = 132 + i * 12;
			expect(bytes.readUInt32BE(base + 4) + bytes.readUInt32BE(base + 8)).toBeLessThanOrEqual(
				bytes.length
			);
		}
	});

	it('declares the tags the matrix and trc path needs', () => {
		const bytes = readFileSync(join(DERIVED, 'icc', 'display-p3.icc'));
		const tags = bytes.readUInt32BE(128);
		const signatures = new Set<string>();
		for (let i = 0; i < tags; i++) {
			signatures.add(bytes.subarray(132 + i * 12, 136 + i * 12).toString('ascii'));
		}

		for (const tag of ['rXYZ', 'gXYZ', 'bXYZ', 'rTRC', 'gTRC', 'bTRC', 'wtpt', 'chad']) {
			expect(signatures.has(tag), tag).toBe(true);
		}
	});

	it('adapts the primaries to d50 rather than leaving them at d65', () => {
		// sRGB's red primary adapted to D50 is near (0.4360, 0.2225, 0.0139); unadapted D65 red is
		// near (0.4124, 0.2126, 0.0193), so the x component alone separates the two
		const bytes = readFileSync(join(DERIVED, 'icc', 'srgb.icc'));
		const tags = bytes.readUInt32BE(128);

		let redOffset = 0;
		for (let i = 0; i < tags; i++) {
			const base = 132 + i * 12;
			if (bytes.subarray(base, base + 4).toString('ascii') === 'rXYZ') {
				redOffset = bytes.readUInt32BE(base + 4);
			}
		}
		expect(redOffset).toBeGreaterThan(0);

		const x = bytes.readInt32BE(redOffset + 8) / 65536;
		expect(x).toBeCloseTo(0.436, 2);
	});
});
