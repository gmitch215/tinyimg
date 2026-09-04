import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeEach, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import { Image, TinyImgBlobError, TinyImgModule } from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(fixtures, name));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

/**
 * The wrapper's runtime-data and raw-pixel surface.
 *
 * Separate from `wrapper.spec.ts` because these need blobs loaded and freed, and a module whose
 * blob table one test filled is not the module the next test wants. A fresh instance per test is
 * cheap and removes the ordering entirely.
 */
describe('blobs through the wrapper', () => {
	let tinyimg: TinyImgModule;

	beforeEach(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('takes a blob of every kind, and reports what it took', async () => {
		await tinyimg.loadBlob('font', 'latin', fixture('derived/fonts/dejavu-latin.ttf'));
		await tinyimg.loadBlob('icc', 'p3', fixture('derived/icc/display-p3.icc'));
		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);

		// a cascade is parsed at load, so getting here means it parsed
		const faces = await tinyimg.detectFaces(fixture('smile.jpg'));
		expect(faces.length).toBeGreaterThan(0);
	});

	it('refuses a cascade that does not parse, at load rather than at search', async () => {
		await expect(
			tinyimg.loadBlob('cascade', 'broken', fixture('derived/cascades/malformed.bin'))
		).rejects.toThrow(/corrupt/);
	});

	it('takes a blob from any source shape', async () => {
		const bytes = fixture('derived/cascades/lbp-frontalface.bin');

		await tinyimg.loadBlob('cascade', 'a', bytes);
		await tinyimg.loadBlob('cascade', 'b', new Response(bytes));
		await tinyimg.loadBlob('cascade', 'c', new Blob([bytes]));

		// three resident, so the same face is found by all three and grouped into one box
		const faces = await tinyimg.detectFaces(fixture('smile.jpg'));
		expect(faces.length).toBeGreaterThan(0);
	});

	it('releases one blob and all of them', async () => {
		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);

		expect(tinyimg.freeBlob('cascade', 'frontal')).toBe(true);
		expect(tinyimg.freeBlob('cascade', 'frontal')).toBe(false);
		expect(tinyimg.freeBlob('font')).toBe(false);

		await expect(tinyimg.detectFaces(fixture('smile.jpg'))).rejects.toThrow(TinyImgBlobError);

		// and the id-less form takes the first of a kind
		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);
		expect(tinyimg.freeBlob('cascade')).toBe(true);

		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);
		tinyimg.freeBlobs();
		await expect(tinyimg.detectFaces(fixture('smile.jpg'))).rejects.toThrow(TinyImgBlobError);
	});

	it('refuses a ninth blob rather than dropping one', async () => {
		const bytes = fixture('derived/icc/srgb.icc');

		for (let i = 0; i < 8; i++) {
			await tinyimg.loadBlob('icc', `slot${i}`, bytes);
		}

		await expect(tinyimg.loadBlob('icc', 'slot8', bytes)).rejects.toThrow(/memory/);
	});

	it('reads faces back at the stride the module reported', async () => {
		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);

		const source = fixture('smile.jpg');
		const info = await tinyimg.probe(source);
		const faces = await tinyimg.detectFaces(source);

		expect(faces.length).toBeGreaterThan(0);

		for (const face of faces) {
			expect(face.width).toBeGreaterThan(0);
			expect(face.height).toBeGreaterThan(0);
			expect(face.x + face.width).toBeLessThanOrEqual(info.width);
			expect(face.y + face.height).toBeLessThanOrEqual(info.height);
			expect(face.neighbors).toBeGreaterThanOrEqual(3);
		}

		// ordered by confidence
		for (let i = 1; i < faces.length; i++) {
			expect(faces[i - 1]!.neighbors).toBeGreaterThanOrEqual(faces[i]!.neighbors);
		}

		// a limit smaller than the count keeps the strongest
		const one = await tinyimg.detectFaces(source, 1);
		expect(one.length).toBeLessThanOrEqual(1);

		if (one.length === 1) expect(one[0]).toEqual(faces[0]);
	});

	it('finds nothing in a photograph with no face, without failing', async () => {
		await tinyimg.loadBlob(
			'cascade',
			'frontal',
			fixture('derived/cascades/lbp-frontalface.bin')
		);

		expect(await tinyimg.detectFaces(fixture('sf-24.jpg'))).toEqual([]);
	});
});

describe('raw pixels through the wrapper', () => {
	let tinyimg: TinyImgModule;

	beforeEach(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('decodes to samples', async () => {
		const raw = await tinyimg.decode(fixture('derived/base.png'));

		expect(raw.width).toBe(320);
		expect(raw.height).toBe(180);
		expect(raw.channels).toBe(3);
		expect(raw.pixels.byteLength).toBe(320 * 180 * 3);

		// a real picture, so not every sample is the same
		expect(new Set(raw.pixels.subarray(0, 3000)).size).toBeGreaterThan(1);
	});

	it('decodes an image with alpha to four channels', async () => {
		const raw = await tinyimg.decode(fixture('derived/base-alpha.png'));

		expect(raw.channels).toBe(4);
		expect(raw.pixels.byteLength).toBe(raw.width * raw.height * 4);
	});

	it('refuses bytes that are not an image', async () => {
		await expect(tinyimg.decode(fixture('derived/malformed/not-an-image.bin'))).rejects.toThrow(
			/unknown format/
		);
	});

	it('runs a plan to samples without an encoder', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0).grayscale();

		const raw = await image.pixels();

		// 1032 * 200 / 1835 is 112.46, and the aspect ratio is kept by rounding
		expect(raw.width).toBe(200);
		expect(raw.height).toBe(112);

		// grayscale drops the color channels rather than writing three equal ones
		expect(raw.channels).toBe(1);
		expect(raw.pixels.byteLength).toBe(200 * 112);
	});

	it('gives the same samples fused and unfused', async () => {
		const source = fixture('derived/base.png');

		using fused = await Image.open(tinyimg, source);
		using eager = await Image.open(tinyimg, source);

		fused.resize(80, 45).brightness(1.2).fusion(true);
		eager.resize(80, 45).brightness(1.2).fusion(false);

		const a = await fused.pixels();
		const b = await eager.pixels();

		// the planner-off arm is what the benchmark measures against, so it has to produce the same
		// image rather than merely a similar one
		expect(a.width).toBe(b.width);
		expect(Array.from(a.pixels)).toEqual(Array.from(b.pixels));
	});

	it('reports a format it cannot name', async () => {
		using image = await Image.open(tinyimg, fixture('derived/base.png'));

		// 'heif' is a real container this build refuses to encode
		await expect(image.bytes('heif')).rejects.toThrow();

		// and a name that is not a container at all is a caller error
		await expect(image.bytes('jpg' as never)).rejects.toThrow(TypeError);
	});
});
