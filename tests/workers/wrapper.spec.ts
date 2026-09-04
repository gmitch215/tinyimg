import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';
import base from '../fixtures/derived/base.png?bin';
import broken from '../fixtures/derived/malformed/not-an-image.bin?bin';
import photo from '../fixtures/sf-24.jpg?bin';

async function call(query: string, body?: Uint8Array): Promise<Response> {
	return env.WRAPPER.fetch(`https://tinyimg.test/?${query}`, {
		method: 'POST',
		body: body ?? new Uint8Array(0)
	});
}

async function json<T>(query: string, body?: Uint8Array): Promise<T> {
	const response = await call(query, body);
	expect(response.status).toBe(200);

	return (await response.json()) as T;
}

/**
 * The shipped package, inside the runtime it is for.
 *
 * This worker imports `dist/`, so what runs here is the compiled output a caller installs, loaded
 * the only way workerd allows: an embedder-compiled module instantiated at worker startup. The
 * node lane drives the same wrapper over a module compiled from bytes, which workerd forbids, so
 * between them they cover both load paths.
 */
describe('the shipped wrapper inside workerd', () => {
	it('loads a module the embedder compiled', async () => {
		const info = await json<{ version: string; abi: number; features: string[] }>('version');

		expect(info.version).toBe('1.0.0');
		expect(info.abi).toBe(1);
		expect(info.features).toContain('jpeg');
		expect(info.features).toContain('webp');
	});

	it('serves a transformed image end to end', async () => {
		const response = await call('width=400&height=300&fit=cover&format=webp&quality=80', photo);

		expect(response.status).toBe(200);
		expect(response.headers.get('content-type')).toBe('image/webp');
		expect(response.headers.get('x-tinyimg-format')).toBe('webp');

		const bytes = new Uint8Array(await response.arrayBuffer());
		expect(bytes.byteLength).toBeGreaterThan(0);
		expect(Number(response.headers.get('content-length'))).toBe(bytes.byteLength);

		// a RIFF container with a WEBP fourcc, so the bytes really are what the header claims
		expect(String.fromCharCode(...bytes.subarray(0, 4))).toBe('RIFF');
		expect(String.fromCharCode(...bytes.subarray(8, 12))).toBe('WEBP');

		const probed = await json<{ width: number; height: number; format: string }>(
			'probe',
			bytes
		);
		expect(probed).toMatchObject({ width: 400, height: 300, format: 'webp' });
	});

	it('encodes every container the build carries', async () => {
		for (const format of ['png', 'jpeg', 'bmp', 'gif', 'tiff', 'webp']) {
			const response = await call(`width=64&format=${format}`, photo);

			expect(response.status).toBe(200);
			expect(response.headers.get('x-tinyimg-format')).toBe(format);

			const bytes = new Uint8Array(await response.arrayBuffer());
			const probed = await json<{ format: string; width: number }>('probe', bytes);

			expect(probed.format).toBe(format);
			expect(probed.width).toBe(64);
		}
	});

	it('decides the decode before touching a pixel', async () => {
		const decided = await json<{
			region: { width: number; height: number };
			scale: number;
			decoded: { width: number };
			output: { width: number; height: number };
			kernels: string[];
		}>('decide&crop=400,200,900,600&width=300&height=200', photo);

		expect(decided.region.width).toBeLessThanOrEqual(900);
		expect(decided.scale).toBeGreaterThan(1);
		expect(decided.decoded.width).toBeLessThan(1835);
		expect(decided.output).toMatchObject({ width: 300, height: 200 });
		expect(decided.kernels).toContain('region');
		expect(decided.kernels).toContain('scaled');
	});

	it('encodes twice from one chain', async () => {
		const report = await json<{ png: number; webp: number; operations: number }>(
			'chain',
			photo
		);

		expect(report.png).toBeGreaterThan(0);
		expect(report.webp).toBeGreaterThan(0);
		expect(report.operations).toBe(3);
	});

	it('reads the Cloudflare Images option names', async () => {
		const response = await call(
			'width=200&height=200&fit=pad&gravity=north&background=%23112233&format=png',
			photo
		);

		expect(response.status).toBe(200);

		const bytes = new Uint8Array(await response.arrayBuffer());
		const probed = await json<{ width: number; height: number }>('probe', bytes);

		expect(probed).toMatchObject({ width: 200, height: 200 });
	});

	it('turns a module failure into a typed error rather than a 500', async () => {
		const response = await call('width=64&format=png', broken);

		// 422 is what the worker maps a TinyImgError onto, so this says the class survived the
		// bundling rather than collapsing to a generic Error
		expect(response.status).toBe(422);

		const body = (await response.json()) as { name: string; code: number; error: string };
		expect(body.name).toBe('TinyImgFormatError');
		expect(body.code).toBe(-6);
		expect(body.error).toBe('unknown format');
	});

	it('gives memory back across a run of requests', async () => {
		// one pass first, so the heap has reached its working size before the count is taken
		await call('width=200&format=png', photo);

		const before = await json<{ pages: number }>('pages');

		for (let i = 0; i < 6; i++) {
			const response = await call('width=200&brightness=1.1&format=png', photo);
			expect(response.status).toBe(200);
		}

		const after = await json<{ pages: number }>('pages');

		// the instance outlives every request it serves, so a wrapper that leaked would show up
		// here and nowhere else
		expect(after.pages).toBe(before.pages);
	});

	it('re-encodes with no options at all', async () => {
		const response = await call('', base);

		expect(response.status).toBe(200);
		expect(response.headers.get('content-type')).toBe('image/png');

		const probed = await json<{ width: number; height: number }>(
			'probe',
			new Uint8Array(await response.arrayBuffer())
		);
		expect(probed).toMatchObject({ width: 320, height: 180 });
	});
});
