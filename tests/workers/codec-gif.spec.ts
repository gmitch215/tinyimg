import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import interlaced from '../fixtures/derived/base-interlaced.gif?bin';
import offset from '../fixtures/derived/base-offset.gif?bin';
import transparent from '../fixtures/derived/base-transparent.gif?bin';
import base from '../fixtures/derived/base.gif?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	width?: number;
	height?: number;
	channels?: number;
	digest?: string;
	encodeResult?: number;
	encodedSize?: number;
	roundTripResult?: number;
	roundTripDigest?: string;
}

/**
 * The GIF codec running where it ships.
 *
 * The node lane covers the same files in more detail. What only this lane shows is that the
 * pre-compiled module reaches identical pixels under workerd.
 */
describe('the gif codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes to the pixels the bmp codec reaches through a different container', async () => {
		const report = await decode(base);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.digest).toBe(golden.bmpRle8);
	});

	it('reconstructs an interlaced file to those same pixels', async () => {
		const report = await decode(interlaced);

		expect(report.result).toBe(0);
		expect(report.digest).toBe(golden.bmpRle8);
	});

	it('turns a transparent index into a transparent pixel', async () => {
		const report = await decode(transparent);

		expect(report.channels).toBe(4);
		expect(report.digest).toBe(golden.gifTransparent);
	});

	it('decodes the logical screen rather than the frame', async () => {
		const report = await decode(offset);

		expect([report.width, report.height]).toEqual([160, 120]);
		expect(report.digest).toBe(golden.gifOffsetFrame);
	});

	it('encodes a palette that already fits without losing a pixel', async () => {
		// format 4 is GIF, and this file's colours all fit, so the round trip has to be exact
		const report = await decode(base, '?reencode=4');

		expect(report.result).toBe(0);
		expect(report.encodeResult).toBe(0);
		expect(report.roundTripResult).toBe(0);
		expect(report.roundTripDigest).toBe(golden.bmpRle8);
	});
});
