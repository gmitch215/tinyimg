import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import rle8 from '../fixtures/derived/base-rle8.bmp?bin';
import unsupported from '../fixtures/derived/base.avif?bin';
import bmp from '../fixtures/derived/base.bmp?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	errorName?: string;
	width?: number;
	height?: number;
	channels?: number;
	format?: number;
	size?: number;
	digest?: string;
	pagesBefore?: number;
	pagesAfter?: number;
	encodeResult?: number;
	encodedSize?: number;
	roundTripResult?: number;
	roundTripDigest?: string;
}

/**
 * The codec running where it ships.
 *
 * The node lane covers the same decodes in more detail; what only this lane can show is that the
 * pre-compiled module produces the same pixels under workerd, with its own memory ceiling and its own
 * instantiation path. The digests are the check that actually catches a difference, since dimensions
 * and channel counts would match even if the pixels did not.
 */
describe('the bmp codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes a 24 bit bitmap to the pixels the node lane produces', async () => {
		const report = await decode(bmp);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.size).toBe(320 * 180 * 3);
		expect(report.digest).toBe(golden.reference);
	});

	it('decodes a run length encoded bitmap to the same pixels as node', async () => {
		const report = await decode(rle8);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.digest).toBe(golden.bmpRle8);
	});

	it('box averages a scaled decode to the same pixels as node', async () => {
		const report = await decode(bmp, '?scale=40,20');

		expect(report.result).toBe(0);
		expect([report.width, report.height]).toEqual([40, 23]);
		expect(report.digest).toBe(golden.referenceEighth);
	});

	it('serves a decode without growing its memory every time', async () => {
		// 767 KiB of the initial megabyte is heap, and one 320x180 decode holds the source, the
		// image and an arena chunk in about 410 KiB of it. a Worker that grew on every request
		// would walk into its memory ceiling under load rather than at a known size
		const first = await decode(bmp);
		const second = await decode(bmp);

		expect(second.pagesBefore).toBe(first.pagesAfter);
		expect(second.pagesAfter).toBe(second.pagesBefore);
	});

	it('grows its memory when a request needs more than is free', async () => {
		// asking for the whole of linear memory cannot fit inside linear memory's own free space,
		// so this forces growth regardless of what earlier tests left behind
		const response = await env.CODEC.fetch('https://tinyimg.test/?grow=1');
		const report = (await response.json()) as {
			pointer: number;
			pagesBefore: number;
			pagesAfter: number;
			usable: boolean;
			pagesAfterFree: number;
		};

		expect(report.pointer).toBeGreaterThan(0);
		expect(report.pagesAfter).toBeGreaterThan(report.pagesBefore);
		expect(report.usable).toBe(true);

		// freeing does not hand pages back, which is what makes the peak the figure that matters
		expect(report.pagesAfterFree).toBe(report.pagesAfter);
	});

	it('refuses an allocation past the module ceiling rather than trapping', async () => {
		const response = await env.CODEC.fetch('https://tinyimg.test/?ceiling=1');
		expect(((await response.json()) as { pointer: number }).pointer).toBe(0);

		// and the instance is still serving afterwards
		expect((await decode(bmp)).digest).toBe(golden.reference);
	});

	it('encodes and reads its own output back unchanged', async () => {
		const report = await decode(bmp, '?reencode=3');

		expect(report.encodeResult).toBe(0);
		expect(report.encodedSize).toBe(54 + 320 * 180 * 3);
		expect(report.roundTripResult).toBe(0);
		expect(report.roundTripDigest).toBe(golden.reference);
	});

	it('reports a recognised format it cannot decode by name', async () => {
		// WebP served here until it gained a codec; AVIF is what is left, since its own answers
		// probe and neither direction of pixels
		const report = await decode(unsupported);

		expect(report.result).toBe(-7);
		expect(report.errorName).toBe('unsupported codec');
	});

	it('reports an unrecognised buffer by name', async () => {
		const report = await decode(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]));

		expect(report.result).toBe(-6);
		expect(report.errorName).toBe('unknown format');
	});

	it('survives a truncated file rather than trapping', async () => {
		const report = await decode(bmp.subarray(0, 100));

		expect(report.result).toBe(-8);
		expect(report.errorName).toBe('corrupt data');

		// and the instance is still good afterwards, which a trap would have ended
		expect((await decode(bmp)).digest).toBe(golden.reference);
	});
});
