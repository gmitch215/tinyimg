import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import {
	Image,
	mimeFor,
	readColor,
	readSource,
	TinyImgArgumentError,
	TinyImgBlobError,
	TinyImgFormatError,
	TinyImgLoadError,
	TinyImgModule,
	transform,
	TRANSFORM_ORDER,
	type Source
} from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

/**
 * A fixture's bytes.
 *
 * Typed as backing a real `ArrayBuffer`, which the `Uint8Array` constructor makes true when handed
 * a typed array: it copies rather than aliasing. Without that a caller cannot pass the result to a
 * `Blob`, because the DOM's `BlobPart` will not take an `ArrayBufferLike`.
 */
function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(fixtures, name));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

/**
 * The shipped wrapper, driven the way a caller will.
 *
 * `tests/support/abi.ts` drives the raw exports and this drives the package, which are different
 * jobs: a failure there points at the C and a failure here points at the wrapper. Both lanes run
 * against the same linked module.
 */
describe('the wrapper', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('reports the version, abi and features the module was built with', () => {
		expect(tinyimg.versionText).toBe('1.0.0');
		expect(tinyimg.version).toEqual([1, 0, 0]);
		expect(tinyimg.abi).toBe(1);

		expect(tinyimg.has('png')).toBe(true);
		expect(tinyimg.has('jpeg')).toBe(true);
		expect(tinyimg.has('text')).toBe(true);
		expect(tinyimg.has('detect')).toBe(true);
		expect(tinyimg.features).toContain('webp');

		expect(tinyimg.errorName(-11)).toBe('blob not loaded');
		expect(tinyimg.errorName(0)).toBe('ok');
	});

	it('refuses a module that is not tinyimg', async () => {
		// the smallest valid wasm module: a header and nothing else
		const empty = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);

		await expect(TinyImgModule.loadBytes(empty)).rejects.toThrow(TinyImgLoadError);
	});

	it('probes a header without decoding anything', async () => {
		const info = await tinyimg.probe(fixture('sf-24.jpg'));

		expect(info).toEqual({
			width: 1835,
			height: 1032,
			frames: 1,
			format: 'jpeg',
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});

		// the one progressive fixture, which is the case that cannot stream a region
		expect((await tinyimg.probe(fixture('road.jpg'))).progressive).toBe(true);

		// a container it recognizes and cannot decode still answers fully
		const avif = await tinyimg.probe(fixture('derived/base.avif'));
		expect(avif.format).toBe('avif');
		expect(avif.width).toBeGreaterThan(0);
	});

	it('takes every shape a source can arrive in', async () => {
		const bytes = fixture('derived/base.png');
		const expected = await tinyimg.probe(bytes);

		const shapes: Source[] = [
			bytes,
			bytes.buffer,
			new Blob([bytes]),
			new Response(bytes),
			new Response(bytes).body as ReadableStream<Uint8Array>
		];

		for (const shape of shapes) {
			expect(await tinyimg.probe(shape)).toEqual(expected);
		}

		await expect(readSource(42 as never)).rejects.toThrow(TypeError);
	});

	it('maps every error code onto the class a caller would branch on', async () => {
		await expect(tinyimg.probe(fixture('derived/malformed/not-an-image.bin'))).rejects.toThrow(
			TinyImgFormatError
		);

		// a blob nobody loaded is its own class, because the remedy is to load one
		tinyimg.freeBlobs();
		await expect(tinyimg.detectFaces(fixture('smile.jpg'))).rejects.toThrow(TinyImgBlobError);

		using image = await Image.open(tinyimg, fixture('derived/base.png'));
		expect(() => image.gamma(-1)).toThrow(TinyImgArgumentError);

		// the code and its name travel with the error, so a caller need not parse a message
		try {
			image.gamma(-1);
			expect.unreachable();
		} catch (error) {
			expect(error).toBeInstanceOf(TinyImgArgumentError);
			expect((error as TinyImgArgumentError).code).toBe(-2);
			expect((error as TinyImgArgumentError).codeName).toBe('argument out of range');
		}
	});
});

describe('the chainable image', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('appends without running, and runs once at the end', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));

		expect(image.operations).toBe(0);
		expect(image.sourceFormat).toBe('jpeg');

		image.crop(400, 200, 900, 600).resize(300, 200).brightness(1.2).contrast(1.1);
		expect(image.operations).toBe(4);

		const decided = image.decide();

		// the whole point: a 300x200 output of a 900x600 region asks the decoder for that region at
		// a reduced scale, not for 1.9 megapixels
		expect(decided.region.width).toBeLessThanOrEqual(900);
		expect(decided.scale).toBeGreaterThan(1);
		expect(decided.decoded.width).toBeLessThan(1835);
		expect(decided.output).toEqual({ width: 300, height: 200, channels: 3 });
		expect(decided.kernels).toContain('region');
		expect(decided.kernels).toContain('scaled');

		// two color operations became one stage
		expect(decided.colorStages).toBe(1);

		const png = await image.bytes('png');
		expect(png.byteLength).toBeGreaterThan(0);
		expect(await tinyimg.probe(png)).toMatchObject({ width: 300, height: 200, format: 'png' });
	});

	it('reports what the rewrites removed', async () => {
		using image = await Image.open(tinyimg, fixture('derived/base.png'));

		// each of these is an identity and none of them survives
		image.brightness(1).contrast(1).gamma(1).rotate(0).blur(0);
		expect(image.operations).toBe(5);

		const decided = image.decide();
		expect(decided.eliminated).toBe(5);
		expect(decided.operations).toBe(0);
	});

	it('encodes twice from one plan', async () => {
		using image = await Image.open(tinyimg, fixture('derived/base.png'));
		image.resize(80, 45);

		const png = await image.bytes('png');
		const webp = await image.bytes('webp', { quality: 70 });

		expect((await tinyimg.probe(png)).format).toBe('png');
		expect((await tinyimg.probe(webp)).format).toBe('webp');
		expect((await tinyimg.probe(webp)).width).toBe(80);

		// and a third time, so nothing was consumed
		expect((await image.bytes('bmp')).byteLength).toBeGreaterThan(0);
	});

	it('applies the operations that cannot be plan operations after the plan', async () => {
		// trim's extent is a function of the pixels, and the planner decides the decode before a
		// pixel is read, so it cannot be a plan operation; it still has to work
		using image = await Image.open(tinyimg, fixture('derived/trim.png'));

		const before = await tinyimg.probe(fixture('derived/trim.png'));
		const trimmed = await image.trim(8).bytes('png');
		const after = await tinyimg.probe(trimmed);

		expect(after.width).toBeLessThan(before.width);
		expect(after.height).toBeLessThan(before.height);
	});

	it('turns a quarter turn into the free operation and anything else into a pass', async () => {
		using quarter = await Image.open(tinyimg, fixture('derived/base.png'));
		quarter.rotateFree(90);

		// folded into the output addressing, so it is a plan operation
		expect(quarter.operations).toBe(1);
		expect((await tinyimg.probe(await quarter.bytes('png'))).width).toBe(180);

		using free = await Image.open(tinyimg, fixture('derived/base.png'));
		free.rotateFree(30).background('#ff0000');

		// not a plan operation, so nothing was appended
		expect(free.operations).toBe(0);

		const rotated = await tinyimg.probe(await free.bytes('png'));
		expect(rotated.width).toBeGreaterThan(320);
	});

	it('hands back the conveniences a Worker wants', async () => {
		using image = await Image.open(tinyimg, fixture('derived/base.png'));
		image.resize(40, 0);

		const blob = await image.blob('png');
		expect(blob.type).toBe('image/png');
		expect(blob.size).toBeGreaterThan(0);

		const response = await image.response('webp', {
			quality: 60,
			headers: { 'cache-control': 'public' }
		});
		expect(response.headers.get('content-type')).toBe('image/webp');
		expect(response.headers.get('cache-control')).toBe('public');
		expect(Number(response.headers.get('content-length'))).toBeGreaterThan(0);

		const url = await image.dataUrl('png');
		expect(url.startsWith('data:image/png;base64,')).toBe(true);
	});

	it('releases the handle, and refuses every later call', async () => {
		const image = await Image.open(tinyimg, fixture('derived/base.png'));

		image.dispose();
		// twice is harmless
		image.dispose();

		expect(() => image.resize(10, 10)).toThrow(TypeError);
		expect(() => image.decide()).toThrow(TypeError);
		await expect(image.bytes('png')).rejects.toThrow(TypeError);
	});

	it('gives the memory back, so a long-lived module does not grow without bound', async () => {
		const bytes = fixture('sf-24.jpg');

		// one pass to let the heap reach its working size
		{
			using warm = await Image.open(tinyimg, bytes);
			await warm.resize(200, 0).bytes('png');
		}

		const before = tinyimg.pages;

		for (let i = 0; i < 8; i++) {
			using image = await Image.open(tinyimg, bytes);
			await image.resize(200, 0).brightness(1.1).bytes('png');
		}

		expect(tinyimg.pages).toBe(before);
	});
});

describe('the one-shot transform', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	it('does nothing but decode and re-encode for an empty option object', async () => {
		const result = await transform(tinyimg, fixture('derived/base.png'));

		expect(result.format).toBe('png');
		expect(result.contentType).toBe('image/png');
		expect(result.width).toBe(320);
		expect(result.height).toBe(180);
		expect(result.bytes().byteLength).toBeGreaterThan(0);
	});

	it('reads the Cloudflare Images option names', async () => {
		const result = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 400,
			height: 300,
			fit: 'cover',
			gravity: 'north',
			format: 'webp',
			quality: 80,
			sharpen: 1,
			brightness: 1.1,
			saturation: 0.9
		});

		expect(result.format).toBe('webp');
		expect(result.width).toBe(400);
		expect(result.height).toBe(300);

		const info = await tinyimg.probe(result.bytes());
		expect(info).toMatchObject({ width: 400, height: 300, format: 'webp' });
	});

	it('multiplies the extent by the device pixel ratio', async () => {
		const one = await transform(tinyimg, fixture('derived/base.png'), {
			width: 100,
			height: 100
		});
		const two = await transform(tinyimg, fixture('derived/base.png'), {
			width: 100,
			height: 100,
			dpr: 2
		});

		expect(one.width).toBe(100);
		expect(two.width).toBe(200);
		expect(two.height).toBe(200);
	});

	it('resizes on one axis and fits on two', async () => {
		// one extent has no aspect mismatch to absorb, so it is a resize and the other axis follows
		const wide = await transform(tinyimg, fixture('sf-24.jpg'), { width: 400 });
		expect(wide.width).toBe(400);
		expect(wide.height).toBe(225);

		// two extents have to be reconciled, and `fit` is what says how
		const box = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 400,
			height: 400,
			fit: 'pad',
			background: '#123456'
		});
		expect(box.width).toBe(400);
		expect(box.height).toBe(400);
	});

	it('applies the options in a fixed order, whatever order the keys are in', async () => {
		const a = await transform(tinyimg, fixture('sf-24.jpg'), {
			width: 200,
			height: 200,
			brightness: 1.3,
			blur: 2
		});

		const b = await transform(tinyimg, fixture('sf-24.jpg'), {
			blur: 2,
			brightness: 1.3,
			height: 200,
			width: 200
		});

		expect(Array.from(a.bytes())).toEqual(Array.from(b.bytes()));
		expect(TRANSFORM_ORDER.indexOf('resize or fit')).toBeLessThan(
			TRANSFORM_ORDER.indexOf('blur')
		);
	});

	it('refuses to guess a format for a source it could not identify', async () => {
		await expect(
			transform(tinyimg, fixture('derived/malformed/not-an-image.bin'))
		).rejects.toThrow(TinyImgFormatError);
	});

	it('serves a Response, a Blob and a data URL', async () => {
		const result = await transform(tinyimg, fixture('derived/base.png'), { width: 32 });

		const response = result.response({ 'x-test': 'yes' });
		expect(response.headers.get('content-type')).toBe('image/png');
		expect(response.headers.get('x-test')).toBe('yes');
		expect(new Uint8Array(await response.arrayBuffer())).toEqual(result.bytes());

		expect(result.blob().type).toBe('image/png');
		expect(result.dataUrl().startsWith('data:image/png;base64,')).toBe(true);
	});
});

describe('the helpers', () => {
	it('reads a color from hex or channels', () => {
		expect(Array.from(readColor('#ff8000', 3))).toEqual([255, 128, 0]);
		expect(Array.from(readColor('ff8000', 3))).toEqual([255, 128, 0]);
		expect(Array.from(readColor('#f80', 3))).toEqual([255, 136, 0]);
		expect(Array.from(readColor('#ff800080', 4))).toEqual([255, 128, 0, 128]);

		// an image with alpha and a color without one is opaque, not invisible
		expect(Array.from(readColor('#ff8000', 4))).toEqual([255, 128, 0, 255]);
		expect(Array.from(readColor([255, 128, 0], 4))).toEqual([255, 128, 0, 255]);

		// a gray image takes the color's luminance
		expect(Array.from(readColor('#ffffff', 1))).toEqual([255]);
		expect(Array.from(readColor('#000000', 1))).toEqual([0]);
		expect(readColor('#ff0000', 1)[0]).toBeGreaterThan(0);

		expect(Array.from(readColor([64], 3))).toEqual([64, 64, 64]);
		expect(() => readColor('not a color', 3)).toThrow(TypeError);
		expect(() => readColor([], 3)).toThrow(TypeError);
	});

	it('names a content type for every container', () => {
		expect(mimeFor('png')).toBe('image/png');
		expect(mimeFor('jpeg')).toBe('image/jpeg');
		expect(mimeFor('webp')).toBe('image/webp');
		expect(mimeFor('unknown')).toBe('application/octet-stream');
	});
});
