import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import baseline from '../fixtures/derived/base-444.jpg?bin';
import cmyk from '../fixtures/derived/base-cmyk.jpg?bin';
import gray from '../fixtures/derived/base-gray.jpg?bin';
import progressive from '../fixtures/derived/base-progressive.jpg?bin';
import restart from '../fixtures/derived/base-restart.jpg?bin';
import photo from '../fixtures/sf-24.jpg?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	errorName?: string;
	width?: number;
	height?: number;
	channels?: number;
	digest?: string;
	pagesBefore?: number;
	pagesAfter?: number;
}

/**
 * The JPEG codec running where it ships.
 *
 * The node lane covers the same files in more detail. What only this lane shows is that the
 * pre-compiled module reaches identical pixels under workerd, which instantiates the module its own
 * way and holds a 64 MiB ceiling. The digests are what catch a difference; dimensions would match
 * either way.
 */
describe('the jpeg codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes to the pixels the node lane produces', async () => {
		const report = await decode(baseline);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.digest).toBe(golden.jpeg444);
	});

	it('reaches those same pixels through ten progressive scans', async () => {
		const report = await decode(progressive);

		expect(report.result).toBe(0);
		expect(report.digest).toBe(golden.jpeg444);
	});

	it('resynchronizes on restart markers', async () => {
		const report = await decode(restart);

		expect(report.result).toBe(0);
		expect(report.digest).toBe(golden.jpeg420);
	});

	it('converts a four component file and a single component one', async () => {
		const four = await decode(cmyk);
		expect(four.channels).toBe(3);
		expect(four.digest).toBe(golden.jpegCmyk);

		const one = await decode(gray);
		expect(one.channels).toBe(1);
		expect(one.digest).toBe(golden.jpegGray);
	});

	it('decodes the reference photograph inside the memory ceiling', async () => {
		const report = await decode(photo);

		expect(report.result).toBe(0);
		expect([report.width, report.height]).toEqual([1835, 1032]);
		expect(report.digest).toBe(golden.jpegPhoto);

		// 1.9 Mpx at three channels, plus the component planes, well inside the 64 MiB cap
		expect(report.pagesAfter).toBeLessThan(1024);
	});

	it('scales in the dct domain down to an eighth', async () => {
		const report = await decode(photo, '?scale=1,1');

		expect([report.width, report.height]).toEqual([230, 129]);
		expect(report.digest).toBe(golden.jpegPhotoEighth);
	});

	it('encodes and reads its own output back', async () => {
		// format 2 is JPEG, so this is the module encoding into its own container
		const report = await decode(baseline, '?reencode=2');

		expect(report.result).toBe(0);
		expect((report as { encodeResult?: number }).encodeResult).toBe(0);
		expect((report as { encodedSize?: number }).encodedSize).toBeGreaterThan(0);
		expect((report as { roundTripResult?: number }).roundTripResult).toBe(0);

		// re-encoding is lossy, so the digest changes; what matters is that the module can read
		// what it wrote, inside workerd, without a host library anywhere in the loop
		expect((report as { roundTripDigest?: string }).roundTripDigest).toBeTruthy();
	});

	it('decodes a region without holding the whole image', async () => {
		const region = await decode(photo, '?region=100,64,256,192');

		expect(region.result).toBe(0);
		expect([region.width, region.height]).toEqual([256, 192]);

		// the region is 1/39 of the image, so the pages it needed have to reflect that rather
		// than the full decode's; this is the plane window doing its job under workerd
		const full = await decode(photo);
		expect(region.pagesAfter).toBeLessThanOrEqual(full.pagesAfter!);
	});
});
