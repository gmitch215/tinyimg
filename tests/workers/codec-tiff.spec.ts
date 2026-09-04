import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import alpha from '../fixtures/derived/base-alpha.tif?bin';
import lzw from '../fixtures/derived/base-lzw.tif?bin';
import msb from '../fixtures/derived/base-msb.tif?bin';
import packbits from '../fixtures/derived/base-packbits.tif?bin';
import palette from '../fixtures/derived/base-palette.tif?bin';
import predictor from '../fixtures/derived/base-predictor.tif?bin';
import strips from '../fixtures/derived/base-strips.tif?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	width?: number;
	height?: number;
	channels?: number;
	digest?: string;
	encodeResult?: number;
	roundTripResult?: number;
	roundTripDigest?: string;
}

/**
 * The TIFF codec running where it ships.
 *
 * The node lane covers the same files in more detail. What only this lane shows is that the
 * pre-compiled module reaches identical pixels under workerd.
 */
describe('the tiff codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes every compression and layout to one set of pixels', async () => {
		for (const [name, bytes] of [
			['lzw', lzw],
			['packbits', packbits],
			['msb', msb],
			['predictor', predictor],
			['strips', strips]
		] as const) {
			const report = await decode(bytes);

			expect(report.result, name).toBe(0);
			expect([report.width, report.height, report.channels], name).toEqual([320, 180, 3]);
			expect(report.digest, name).toBe(golden.reference);
		}
	});

	it('reads a colour map and an alpha channel', async () => {
		const indexed = await decode(palette);
		expect(indexed.channels).toBe(3);
		expect(indexed.digest).toBe(golden.bmpRle8);

		const transparent = await decode(alpha);
		expect(transparent.channels).toBe(4);
		expect(transparent.digest).toBe(golden.tiffAlpha);
	});

	it('encodes losslessly, which is the only thing this format promises', async () => {
		// format 5 is TIFF, and the format is lossless, so the round trip is exact
		const report = await decode(lzw, '?reencode=5');

		expect(report.result).toBe(0);
		expect(report.encodeResult).toBe(0);
		expect(report.roundTripResult).toBe(0);
		expect(report.roundTripDigest).toBe(golden.reference);
	});
});
