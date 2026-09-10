import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import { Font, og, TinyImgModule } from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

/**
 * The card renderer, which is the one surface that needs nothing loaded.
 *
 * Everything here runs against a module that has had no blob handed to it, because that is the
 * claim: the face is compiled in, so a Worker that wants an Open Graph card imports the module and
 * calls one function.
 */
describe('the og card helper', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
		tinyimg.freeBlobs();
	});

	it('renders a card with nothing loaded', async () => {
		const card = await og(tinyimg, {
			title: 'How a planner made the decode cheap',
			subtitle: 'Deciding what to decode before decoding it',
			footer: 'tinyimg.gmitch215.dev',
			background: '#0b1020'
		});

		expect(card.width).toBe(1200);
		expect(card.height).toBe(630);
		expect(card.format).toBe('png');
		expect(card.contentType).toBe('image/png');
		expect(card.titleTruncated).toBe(false);
		expect(card.missingGlyphs).toBe(0);

		// a real PNG, and one this library reads back at the extent it claims
		expect(Array.from(card.data.subarray(0, 8))).toEqual([137, 80, 78, 71, 13, 10, 26, 10]);
		expect(await tinyimg.probe(card.data)).toMatchObject({
			width: 1200,
			height: 630,
			format: 'png'
		});
	});

	it('puts ink on the card rather than only a background', async () => {
		// a small card, because the property is about ink and not about the extent, and this lane
		// runs its files in parallel
		const small = { width: 400, height: 210, background: '#101010' };

		const blank = await og(tinyimg, small);
		const written = await og(tinyimg, { ...small, title: 'Hello' });

		// the background alone compresses to almost nothing; the text is what makes it larger
		expect(written.data.byteLength).toBeGreaterThan(blank.data.byteLength);
	});

	it('reports a headline it had to truncate', async () => {
		const long =
			'a headline far longer than three lines of a card can hold, which keeps going and ' +
			'going and going and going past anything a reader would want to read on a preview ' +
			'card, and then goes on further still so the box certainly runs out of room';

		const card = await og(tinyimg, {
			title: long,
			background: '#000000',
			width: 600,
			height: 315
		});

		expect(card.titleTruncated).toBe(true);
	});

	it('reports the glyphs the face did not have', async () => {
		// the compiled-in face is a Latin subset, so a CJK headline is all missing glyphs
		const card = await og(tinyimg, {
			title: 'a title with 中文 in it',
			width: 600,
			height: 315
		});

		expect(card.missingGlyphs).toBeGreaterThan(0);
	});

	it('covers the card with an image when given one', async () => {
		const card = await og(tinyimg, {
			width: 600,
			height: 315,
			image: fixture('sf-24.jpg'),
			title: 'Over a photograph',
			format: 'jpeg',
			encode: { quality: 78 }
		});

		expect(card.format).toBe('jpeg');
		expect(await tinyimg.probe(card.data)).toMatchObject({ width: 600, height: 315 });

		// a photograph does not compress to a few hundred bytes, so this is the image and not the
		// solid background path
		expect(card.data.byteLength).toBeGreaterThan(10_000);
	});

	it('hands back a Response ready to serve', async () => {
		const card = await og(tinyimg, {
			title: 'Ready',
			background: '#222222',
			width: 400,
			height: 210
		});
		const response = card.response({ 'cache-control': 'public, max-age=86400' });

		expect(response.headers.get('content-type')).toBe('image/png');
		expect(response.headers.get('content-length')).toBe(String(card.data.byteLength));
		expect(response.headers.get('cache-control')).toBe('public, max-age=86400');
		expect(new Uint8Array(await response.arrayBuffer())).toEqual(card.data);
	});

	it('takes a custom extent and format', async () => {
		const card = await og(tinyimg, {
			width: 600,
			height: 600,
			title: 'Square',
			background: '#334455',
			format: 'webp'
		});

		expect(card.width).toBe(600);
		expect(card.format).toBe('webp');
		expect(await tinyimg.probe(card.data)).toMatchObject({ width: 600, height: 600 });
	});
});

describe('a font through the wrapper', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
		tinyimg.freeBlobs();
	});

	it('loads the face the module carries', () => {
		using font = Font.load(tinyimg);

		expect(font.has('A'.codePointAt(0)!)).toBe(true);
		expect(font.has(0xe9)).toBe(true);
		expect(font.has(0x4e2d)).toBe(false);
	});

	it('loads it by name too, and refuses a name nothing answers to', () => {
		using named = Font.load(tinyimg, 'sans');
		expect(named.has('Z'.codePointAt(0)!)).toBe(true);

		expect(() => Font.load(tinyimg, 'nothing-by-that-name')).toThrow();
	});

	it('refuses to be used after it is disposed', () => {
		const font = Font.load(tinyimg);
		font.dispose();

		expect(() => font.pointer).toThrow(TypeError);

		// and disposing twice is harmless, which is what makes `using` safe beside an explicit
		// dispose
		font.dispose();
	});
});
