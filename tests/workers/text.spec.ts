import { env } from 'cloudflare:test';
import { beforeAll, describe, expect, it } from 'vitest';
import cascade from '../fixtures/derived/cascades/lbp-frontalface.bin?bin';
import profile from '../fixtures/derived/cascades/lbp-profileface.bin?bin';
import font from '../fixtures/derived/fonts/dejavu-latin.ttf?bin';
import psf from '../fixtures/derived/fonts/tiny.psf?bin';
import mountains from '../fixtures/mountains.jpg?bin';
import photo from '../fixtures/sf-24.jpg?bin';
import portrait from '../fixtures/smile.jpg?bin';

interface Report {
	result: number;
	parses?: number;
	width?: number;
	height?: number;
	ascent?: number;
	lines?: number;
	glyphs?: number;
	missing?: number;
	ink?: number;
	digest?: string;
	sourceWidth?: number;
	sourceHeight?: number;
	faces?: { x: number; y: number; width: number; height: number; neighbors: number }[];
}

async function call(query: string, body?: Uint8Array): Promise<Report> {
	const response = await env.CODEC.fetch(`https://tinyimg.test/?${query}`, {
		method: 'POST',
		body: body ?? new Uint8Array(0)
	});

	expect(response.status).toBe(200);
	return (await response.json()) as Report;
}

/** TinyBlobKind. */
const FONT = 0;
const CASCADE = 2;

/**
 * Text and face detection where they ship.
 *
 * The node lane already drives both against a module compiled from bytes, which workerd forbids.
 * What this adds is that they run inside the real runtime, through a module the embedder compiled,
 * and that a blob reaches the module the way a Worker would deliver one: as bytes the host fetched
 * and handed over, with the module taking ownership. Nothing here is linked in.
 */
describe('text inside workerd', () => {
	beforeAll(async () => {
		await call('unload');

		expect((await call(`blob=${FONT}:latin`, font)).result).toBe(0);
		expect((await call(`blob=${FONT}:psf`, psf)).result).toBe(0);
	});

	it('draws a line through a face the host delivered', async () => {
		const report = await call('text=Hamburgefons&size=32&width=300&height=60');

		expect(report.result).toBe(0);
		expect(report.lines).toBe(1);
		expect(report.glyphs).toBe(12);
		expect(report.missing).toBe(0);
		expect(report.ink).toBeGreaterThan(0);

		// 1901 font units of ascent over 2048 per em, at 32 pixels
		expect(report.ascent).toBeCloseTo((1901 * 32) / 2048, 3);
	});

	it('reports a face it has not been given', async () => {
		const report = await call('text=x&font=missing&size=20&width=40&height=40');

		// TINYIMG_ERR_BLOB_MISSING
		expect(report.result).toBe(-11);
	});

	it('produces the same pixels for the same request', async () => {
		const first = await call('text=Repeat&size=28&width=200&height=50');
		const second = await call('text=Repeat&size=28&width=200&height=50');

		expect(first.result).toBe(0);
		expect(first.digest).toBe(second.digest);
	});

	it('wraps inside a width and aligns inside it', async () => {
		const sentence = encodeURIComponent('the quick brown fox jumps over the lazy dog');

		const loose = await call(`text=${sentence}&size=16&width=400&height=200`);
		const tight = await call(`text=${sentence}&size=16&width=400&height=200&wrap=160`);

		expect(loose.result).toBe(0);
		expect(tight.result).toBe(0);
		expect(loose.lines).toBe(1);
		expect(tight.lines!).toBeGreaterThan(1);
		expect(tight.width!).toBeLessThanOrEqual(160);

		// the three alignments draw the same glyphs and are not the same image
		const digests = new Set<string>();

		for (const align of [0, 1, 2]) {
			const drawn = await call(
				`text=short&size=20&width=220&height=40&wrap=200&align=${align}`
			);

			expect(drawn.result).toBe(0);
			digests.add(drawn.digest!);
		}

		expect(digests.size).toBe(3);
	});

	it('draws a bitmap face at its own size, whatever size is asked for', async () => {
		const small = await call('text=AAA&font=psf&size=8&width=40&height=20');
		const large = await call('text=AAA&font=psf&size=96&width=40&height=20');

		expect(small.result).toBe(0);
		expect(large.result).toBe(0);
		expect(small.digest).toBe(large.digest);
		expect(small.width).toBe(24);
	});

	it('draws a codepoint the face has no glyph for rather than failing', async () => {
		// U+4E2D, outside the latin subset
		const report = await call(`text=${encodeURIComponent('a中b')}&size=24&width=120&height=40`);

		expect(report.result).toBe(0);
		expect(report.glyphs).toBe(3);
		expect(report.missing).toBe(1);
	});

	it('grows memory for a large render inside the runtime', async () => {
		const report = await call('text=wide&size=400&width=1400&height=700');

		expect(report.result).toBe(0);
		expect(report.ink).toBeGreaterThan(0);
	});
});

describe('face detection inside workerd', () => {
	it('reports a missing cascade rather than finding nothing', async () => {
		await call('unload');

		const report = await call('faces=1', portrait);
		expect(report.result).toBe(-11);
	});

	it('refuses a cascade that does not parse, at load', async () => {
		await call('unload');

		// the font, handed over as a cascade: right length, wrong contents
		const report = await call(`blob=${CASCADE}:wrong`, font);

		expect(report.result).toBe(0);
		// TINYIMG_ERR_CORRUPT
		expect(report.parses).toBe(-8);

		await call('unload');
	});

	it('finds the face in the portrait and nothing in the photograph', async () => {
		await call('unload');
		expect((await call(`blob=${CASCADE}:frontal`, cascade)).parses).toBe(0);
		expect((await call(`blob=${CASCADE}:profile`, profile)).parses).toBe(0);

		const found = await call('faces=1', portrait);

		expect(found.result).toBe(0);
		expect(found.faces!.length).toBeGreaterThan(0);

		const face = found.faces![0]!;
		expect(face.width).toBeGreaterThan(0);
		expect(face.x + face.width).toBeLessThanOrEqual(found.sourceWidth!);
		expect(face.y + face.height).toBeLessThanOrEqual(found.sourceHeight!);
		expect(face.neighbors).toBeGreaterThanOrEqual(3);

		const nothing = await call('faces=1', photo);
		expect(nothing.result).toBe(0);
		expect(nothing.faces).toHaveLength(0);

		const alsoNothing = await call('faces=1', mountains);
		expect(alsoNothing.result).toBe(0);
		expect(alsoNothing.faces).toHaveLength(0);
	});

	it('takes the options the host wrote into the structure the module sized', async () => {
		await call('unload');
		await call(`blob=${CASCADE}:frontal`, cascade);

		// no scale left to search
		const none = await call('faces=1&minSize=9000', portrait);
		expect(none.result).toBe(0);
		expect(none.faces).toHaveLength(0);

		// a threshold nothing clears
		const strict = await call('faces=1&minSize=200&minNeighbors=500', portrait);
		expect(strict.result).toBe(0);
		expect(strict.faces).toHaveLength(0);

		await call('unload');
	});

	it('crops to a computed gravity, and falls back with no cascade', async () => {
		await call('unload');

		// fit 300x300 cover, gravity 10 is face and 9 is auto
		const fallback = await call('plan=fit%3A300%2C300%2C2%2C10', portrait);
		const auto = await call('plan=fit%3A300%2C300%2C2%2C9', portrait);

		expect(fallback.result).toBe(0);
		expect(auto.result).toBe(0);

		// with no cascade the face request is the auto request, exactly
		expect(fallback.digest).toBe(auto.digest);

		await call(`blob=${CASCADE}:frontal`, cascade);
		await call(`blob=${CASCADE}:profile`, profile);

		const detected = await call('plan=fit%3A300%2C300%2C2%2C10', portrait);

		expect(detected.result).toBe(0);

		// and with one, it is not: the preflight that answers the gravity before the plan resolves
		// ran inside the runtime
		expect(detected.digest).not.toBe(auto.digest);

		await call('unload');
	});
});
