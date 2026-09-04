import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import alpha from '../fixtures/derived/base-alpha.webp?bin';
import animation from '../fixtures/derived/base-animation.webp?bin';
import lossless from '../fixtures/derived/base-lossless.webp?bin';
import lossyAlpha from '../fixtures/derived/base-lossy-alpha.webp?bin';
import lossy from '../fixtures/derived/base-lossy.webp?bin';
import rawAlpha from '../fixtures/derived/base-raw-alpha.webp?bin';
import simple from '../fixtures/derived/base-simple.webp?bin';
import oddLossy from '../fixtures/derived/tiny-odd-lossy.webp?bin';
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
 * The WebP codec running where it ships.
 *
 * The node lane covers the same files in more detail. What only this lane shows is that the
 * pre-compiled module reaches identical pixels under workerd, which for this codec is worth more
 * than for the others: it is the one whose decode leans on `clz`, on saturating arithmetic and on a
 * 32 bit multiply overflowing deliberately, and those are exactly the operations a different engine
 * could get differently.
 */
describe('the webp codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes a lossless stream to the pixels three other codecs reach', async () => {
		const report = await decode(lossless);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.digest).toBe(golden.reference);
	});

	it('decodes a lossy frame to what dwebp produces', async () => {
		const report = await decode(lossy);

		expect(report.result).toBe(0);
		expect(report.digest).toBe(golden.webpLossy);
	});

	it('decodes the simple loop filter, which is a different algorithm', async () => {
		expect((await decode(simple)).digest).toBe(golden.webpSimple);
	});

	it('reads both ways of storing an alpha plane', async () => {
		const lossless4 = await decode(alpha);

		expect(lossless4.channels).toBe(4);
		expect(lossless4.digest).toBe(golden.tiffAlpha);

		for (const bytes of [lossyAlpha, rawAlpha]) {
			const report = await decode(bytes);

			expect(report.channels).toBe(4);
			expect(report.digest).toBe(golden.webpLossyAlpha);
		}
	});

	it('decodes the first frame of an animation onto the declared canvas', async () => {
		const report = await decode(animation);

		expect([report.width, report.height]).toEqual([320, 180]);
		expect(report.digest).toBe(golden.webpAnimationFirstFrame);
	});

	it('handles extents that are odd in both axes', async () => {
		const report = await decode(oddLossy);

		expect([report.width, report.height]).toEqual([65, 33]);
		expect(report.digest).toBe(golden.webpOddLossy);
	});

	it('encodes losslessly, which means the pixels come back', async () => {
		// format 6 is WebP, and the lossless flag is what separates its two modes
		const report = await decode(lossless, '?reencode=6&lossless=1');

		expect(report.result).toBe(0);
		expect(report.encodeResult).toBe(0);
		expect(report.roundTripResult).toBe(0);
		expect(report.roundTripDigest).toBe(golden.reference);
	});

	it('encodes lossily inside the runtime it ships in', async () => {
		const report = await decode(lossless, '?reencode=6&quality=80');

		expect(report.encodeResult).toBe(0);
		expect(report.roundTripResult).toBe(0);

		// a lossy round trip cannot match a digest, so what is checked is that it produced
		// something smaller than the source and readable
		expect(report.encodedSize).toBeLessThan(320 * 180 * 3);
		expect(report.roundTripDigest).not.toBe(golden.reference);
	});
});
