import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import photo from '../fixtures/sf-24.jpg?bin';

interface Report {
	result: number;
	ops?: number;
	width?: number;
	height?: number;
	channels?: number;
	digest?: string;
	decodeWidth?: number;
	decodeHeight?: number;
}

/**
 * The Phase 5 operations where they ship.
 *
 * The node lane already drives them against a module built from bytes, which workerd forbids; what
 * this adds is that the same operations run inside the real runtime, through a module the embedder
 * compiled. A kernel that reaches for something workerd does not allow fails here and nowhere else.
 */
async function plan(chain: string[], fusion = 1): Promise<Report> {
	const query = chain.map((step) => `plan=${encodeURIComponent(step)}`).join('&');
	const response = await env.CODEC.fetch(`https://tinyimg.test/?${query}&fusion=${fusion}`, {
		method: 'POST',
		body: photo
	});

	expect(response.status).toBe(200);
	return (await response.json()) as Report;
}

describe('the phase 5 operations inside workerd', () => {
	it('carries a caller-supplied matrix through the plan', async () => {
		// sepia, as twelve numbers rather than a named operation
		const report = await plan([
			'matrix:0.393,0.769,0.189,0,0.349,0.686,0.168,0,0.272,0.534,0.131,0',
			'resize:200,0'
		]);

		expect(report.result).toBe(0);
		expect(report.width).toBe(200);
		expect(report.channels).toBe(3);
	});

	it('carries a named curve, and its parameters reach the table', async () => {
		// levels 5 is TINYIMG_CURVE_LEVELS, then in black, in white, gamma, out
		// black and out white
		const report = await plan(['curve:5,20,235,1.1,0,255', 'resize:160,0']);

		expect(report.result).toBe(0);
		expect(report.width).toBe(160);
	});

	it('rejects a curve whose parameters it cannot build', async () => {
		// an inverted input range has no inverse
		const report = await plan(['curve:5,235,20,1,0,255']);

		expect(report.result).toBe(-2);
	});

	it('runs a neighborhood effect and still resolves a scaled decode', async () => {
		// 0 is TINYIMG_FX_UNSHARP: sigma, amount, threshold
		const report = await plan(['resize:400,0', 'effect:0,1.5,1,0']);

		expect(report.result).toBe(0);
		expect(report.width).toBe(400);

		// the resize is what the decoder is asked for, and the effect runs after
		// it on the smaller image; that is the whole point of the ordering
		expect(report.decodeWidth).toBeLessThan(1835);
	});

	it('accepts an identity matrix and a unit curve in a chain', async () => {
		const report = await plan([
			'brightness:1.2',
			'matrix:1,0,0,0,0,1,0,0,0,0,1,0',
			'curve:0,1',
			'contrast:1.1'
		]);

		expect(report.result).toBe(0);

		// four appended, which is what tiny_plan_count reports: it is the count
		// before any rewrite, so this says the two generic operations were
		// accepted rather than that they were eliminated. the elimination itself
		// is asserted on the resolution, in tests/c/image/effects.c
		expect(report.ops).toBe(4);
		expect(report.width).toBe(1835);
	});

	it('produces the same pixels fused and unfused for a single effect', async () => {
		const fused = await plan(['resize:120,0', 'effect:2'], 1);
		const eager = await plan(['resize:120,0', 'effect:2'], 0);

		expect(fused.result).toBe(0);
		expect(eager.result).toBe(0);

		// a sobel takes no parameters and runs on a materialized image either
		// way, so the two paths have to agree exactly
		expect(fused.digest).toBe(eager.digest);
	});

	it('reports an unknown operation rather than running something else', async () => {
		const report = await plan(['nonsense:1']);

		expect(report.result).toBe(-2);
	});
});
