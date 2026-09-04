import { env } from 'cloudflare:test';
import { describe, expect, it } from 'vitest';

interface Probe {
	version: number;
	abi: number;
	features: number;
	imports: number;
	exportsMemory: boolean;
	pages: number;
	pagesAfterGrow: number;
}

/**
 * Proves the module instantiates inside the Workers runtime, which is the integration the whole
 * project rests on and the one most likely to fail for a reason no unit test can see.
 *
 * The module is reached through an auxiliary worker rather than compiled here: workerd refuses
 * `WebAssembly.Module(bytes)` with "Wasm code generation disallowed by embedder", so wasm has to
 * arrive as a module in a worker's own bundle. That refusal is the reason the TypeScript wrapper takes
 * a `WebAssembly.Module` and not an `ArrayBuffer`.
 */
describe('the wasm module inside workerd', () => {
	async function probe(grow = 0): Promise<Probe> {
		const response = await env.PROBE.fetch(`https://tinyimg.test/?grow=${grow}`);
		expect(response.status).toBe(200);
		return (await response.json()) as Probe;
	}

	it('instantiates and reports the version the header declares', async () => {
		const result = await probe();
		expect(result.version).toBe(1 << 16);
		expect(result.abi).toBe(1);
	});

	it('imports nothing, so it needs no host functions to run', async () => {
		expect((await probe()).imports).toBe(0);
	});

	it('exports its own memory rather than importing one', async () => {
		expect((await probe()).exportsMemory).toBe(true);
	});

	it('starts at the initial memory the linker was given', async () => {
		// -Wl,--initial-memory=1048576, and the worker only grows when asked
		expect((await probe()).pages).toBe(16);
	});

	it('grows its memory on demand', async () => {
		const result = await probe(4);
		expect(result.pagesAfterGrow).toBe(result.pages + 4);
	});

	it('reports simd as compiled in', async () => {
		const SIMD = 1 << 0;
		expect((await probe()).features & SIMD).toBe(SIMD);
	});
});
