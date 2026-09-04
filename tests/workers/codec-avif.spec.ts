import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import alpha from '../fixtures/derived/base-alpha.avif?bin';
import base from '../fixtures/derived/base.avif?bin';

interface Report {
	result: number;
	errorName?: string;
	probeResult?: number;
	width?: number;
	height?: number;
	frames?: number;
	format?: number;
	channels?: number;
	bitDepth?: number;
	hasAlpha?: boolean;
}

/**
 * The AVIF container reader running where it ships.
 *
 * Only `probe` exists for this format, so this lane checks the one thing it can: that a Worker can
 * describe an AVIF it cannot decode, which is what lets a caller report the format and reach for
 * something else rather than treating the file as unreadable.
 */
describe('the avif container reader inside workerd', () => {
	async function probe(bytes: Uint8Array): Promise<Report> {
		const response = await env.CODEC.fetch('https://tinyimg.test/', {
			method: 'POST',
			body: bytes as unknown as BodyInit
		});

		expect(response.status).toBe(200);
		return (await response.json()) as Report;
	}

	it('describes the primary item of a file it will not decode', async () => {
		const report = await probe(base);

		// the two answers are different on purpose: the pixels are refused specifically, and the
		// header still comes back
		expect(report.result).toBe(-7);
		expect(report.errorName).toBe('unsupported codec');

		expect(report.probeResult).toBe(0);
		expect([report.width, report.height]).toEqual([320, 180]);
		expect(report.channels).toBe(3);
		expect(report.hasAlpha).toBe(false);
		expect(report.frames).toBe(1);
		expect(report.bitDepth).toBe(8);
	});

	it('finds the alpha of a file whose properties describe two items', async () => {
		const report = await probe(alpha);

		expect([report.width, report.height]).toEqual([320, 180]);

		// the alpha item's own channel count is one, so reporting four means the association list
		// was followed rather than the last property of each kind taken
		expect(report.channels).toBe(4);
		expect(report.hasAlpha).toBe(true);
	});
});
