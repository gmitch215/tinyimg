import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import interlaced from '../fixtures/derived/base-interlaced.png?bin';
import base from '../fixtures/derived/base.png?bin';
import logo from '../fixtures/webassembly.png?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	errorName?: string;
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
 * The PNG codec running where it ships.
 *
 * The node lane covers the same files in more detail. What only this lane shows is that the
 * pre-compiled module produces identical pixels under workerd, whose memory ceiling and
 * instantiation path are its own. The digests are what catch a difference; dimensions would match
 * either way.
 */
describe('the png codec inside workerd', () => {
	async function decode(bytes: Uint8Array, query = ''): Promise<Report> {
		const response = await env.CODEC.fetch(`https://tinyimg.test/${query}`, {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('decodes to the pixels the node lane and the bmp codec both produce', async () => {
		const report = await decode(base);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([320, 180, 3]);
		expect(report.digest).toBe(golden.reference);
	});

	it('reconstructs an Adam7 image to those same pixels', async () => {
		const report = await decode(interlaced);

		expect(report.result).toBe(0);
		expect(report.digest).toBe(golden.reference);
	});

	it('applies palette transparency from a four bit indexed file', async () => {
		const report = await decode(logo);

		expect(report.result).toBe(0);
		expect([report.width, report.height, report.channels]).toEqual([512, 512, 4]);
		expect(report.digest).toBe(golden.pngPalette4Bit);
	});

	it('box averages a scaled decode', async () => {
		const report = await decode(base, '?scale=40,20');

		expect([report.width, report.height]).toEqual([40, 23]);
		expect(report.digest).toBe(golden.referenceEighth);
	});

	it('encodes and reads its own output back unchanged', async () => {
		const report = await decode(base, '?reencode=1');

		expect(report.encodeResult).toBe(0);
		expect(report.roundTripResult).toBe(0);

		// PNG is lossless, so the pixels have to survive a full pass through the encoder
		expect(report.roundTripDigest).toBe(golden.reference);
	});

	it('survives a corrupt file rather than trapping', async () => {
		const tampered = new Uint8Array(base);
		const at = Math.floor(tampered.length / 2);
		tampered[at] = (tampered[at] ?? 0) ^ 0xff;

		const report = await decode(tampered);
		expect(report.result).toBe(-8);
		expect(report.errorName).toBe('corrupt data');

		// and the instance is still serving afterwards
		expect((await decode(base)).digest).toBe(golden.reference);
	});
});
