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
 * The AV1 decoder is the largest thing in the module and workerd is where it has to run, so this
 * lane's job is to prove it does: the same bitstream the ctest lane decodes natively, decoded
 * inside the runtime, with the alpha item resolved through the container's own property list.
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

	it('decodes the primary item where it ships', async () => {
		const report = await probe(base);

		expect(report.result).toBe(0);
		expect([report.width, report.height]).toEqual([320, 180]);
		expect(report.channels).toBe(3);
	});

	it('finds the alpha of a file whose properties describe two items', async () => {
		const report = await probe(alpha);

		expect([report.width, report.height]).toEqual([320, 180]);

		/*
		 * Four channels, which says three things at once: the association list was followed rather
		 * than the last property of each kind taken, a caller who named no channel count got the
		 * file's own, and the second AV1 item was decoded and its plane became the alpha.
		 */
		expect(report.channels).toBe(4);
	});
});
