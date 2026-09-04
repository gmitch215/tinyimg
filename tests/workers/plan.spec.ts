import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import photo from '../fixtures/sf-24.jpg?bin';
import { golden } from '../support/golden.js';

interface Report {
	result: number;
	errorName?: string;
	ops?: number;
	width?: number;
	height?: number;
	channels?: number;
	format?: number;
	size?: number;
	digest?: string;
	decode?: {
		x: number;
		y: number;
		width: number;
		height: number;
		scale: number;
		channels: number;
	};
	decodeWidth?: number;
	decodeHeight?: number;
}

/**
 * The planner where it ships.
 *
 * The ctest suite proves the arithmetic and the node lane proves it survives compilation to wasm.
 * What only this lane can show is that a plan runs under workerd, whose instantiation path and
 * memory ceiling are its own, and that it lands on the same pixels: the digests here are the ones
 * tests/node/plan.spec.ts checks, so a runtime specific difference in the sample map or the color
 * collapse cannot pass both.
 */
describe('the planner inside workerd', () => {
	async function run(chain: string[], eager = false): Promise<Report> {
		const query = chain.map((step) => `plan=${encodeURIComponent(step)}`).join('&');
		const response = await env.CODEC.fetch(
			`https://tinyimg.test/?${query}${eager ? '&eager' : ''}`,
			{ method: 'POST', body: photo as unknown as BodyInit }
		);

		expect(response.status).toBe(200);
		return response.json();
	}

	it('runs the worked chain to the digest the node lane recorded', async () => {
		const report = await run([
			'crop:400,200,900,600',
			'resize:300,200',
			'brightness:1.2',
			'contrast:1.1',
			'saturation:0.8',
			'gamma:2.2'
		]);

		expect(report.result).toBe(0);
		expect(report.ops).toBe(6);
		expect(report.width).toBe(300);
		expect(report.height).toBe(200);
		expect(report.digest).toBe(golden.planWorkedChain);
	});

	it('turns and mirrors to the same digest', async () => {
		const report = await run(['rotate:90', 'fliph']);

		expect(report.result).toBe(0);
		expect(report.width).toBe(1032);
		expect(report.height).toBe(1835);
		expect(report.digest).toBe(golden.planTurned);
	});

	it('decodes a rectangle at a scale rather than the whole image', async () => {
		const report = await run(['crop:600,300,600,400', 'resize:150,100']);

		expect(report.result).toBe(0);
		expect(report.decode).toEqual({
			x: 600,
			y: 300,
			width: 600,
			height: 400,
			scale: 4,
			channels: 0
		});
		expect(report.decodeWidth).toBe(150);
		expect(report.decodeHeight).toBe(100);

		// the decode landed exactly on the output extent, so nothing resamples after it
		expect(report.width).toBe(150);
		expect(report.height).toBe(100);
	});

	it('asks the decoder for the luminance when that is all the chain wants', async () => {
		const report = await run(['grayscale']);

		expect(report.result).toBe(0);
		expect(report.channels).toBe(1);
		expect(report.decode!.channels).toBe(1);
	});

	it('reports a plan whose crop falls outside the image', async () => {
		const report = await run(['crop:9000,9000,10,10']);

		expect(report.result).toBe(-2);
		expect(report.errorName).toBe('argument out of range');
	});

	it('produces the same extent fused or not', async () => {
		const chain = ['crop:200,100,800,600', 'resize:200,150', 'brightness:0.9'];

		const fused = await run(chain);
		const eager = await run(chain, true);

		expect(fused.result).toBe(0);
		expect(eager.result).toBe(0);
		expect(fused.width).toBe(eager.width);
		expect(fused.height).toBe(eager.height);
		expect(fused.size).toBe(eager.size);
	});

	it('runs a fit with a blur, which is two passes', async () => {
		const report = await run(['blur:8', 'fit:400,400,2,0']);

		expect(report.result).toBe(0);
		expect(report.width).toBe(400);
		expect(report.height).toBe(400);
		expect(report.channels).toBe(3);
	});

	it('keeps the source format on the output', async () => {
		const report = await run(['resize:100,60']);

		expect(report.result).toBe(0);
		expect(report.format).toBe(2);
	});
});
