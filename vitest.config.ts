import { cloudflareTest } from '@cloudflare/vitest-pool-workers';
import { join } from 'node:path';
import { defineConfig } from 'vitest/config';
import { wasmBytes } from './scripts/vite-wasm-bytes.ts';

const root = import.meta.dirname;

/**
 * The worker that holds the module, as a miniflare service.
 *
 * Not the pool's `main`: workerd refuses `WebAssembly.Module(bytes)` outright, so the module has to
 * arrive as a `CompiledWasm` module in a worker's own bundle and be instantiated during that worker's
 * startup. An auxiliary worker has its own startup, and is the shape a deployed tinyimg Worker has.
 */
const probe = {
	name: 'tinyimg-probe',
	compatibilityDate: '2026-08-22',
	modulesRoot: root,
	modules: [
		{ type: 'ESModule' as const, path: join(root, 'tests/workers/fixtures/probe-worker.mjs') },
		{ type: 'CompiledWasm' as const, path: join(root, 'bin/tinyimg.wasm') }
	]
};

/**
 * A second worker holding a second instance, for the tests that decode.
 *
 * Separate from the probe worker because an instance outlives every request it serves: a decode
 * grows linear memory, and the probe worker's assertions about its initial page count would then
 * depend on which test ran first.
 */
const codec = {
	name: 'tinyimg-codec',
	compatibilityDate: '2026-08-22',
	modulesRoot: root,
	modules: [
		{ type: 'ESModule' as const, path: join(root, 'tests/workers/fixtures/codec-worker.mjs') },
		{ type: 'CompiledWasm' as const, path: join(root, 'bin/tinyimg.wasm') }
	]
};

export default defineConfig({
	test: {
		coverage: {
			// v8 for the node lane. The workers lane passes --coverage.provider=istanbul, because
			// workerd does not implement node:inspector and the v8 provider needs a Session
			provider: 'v8',
			reporter: ['text', 'lcov'],
			reportsDirectory: './coverage/node',
			include: ['src/ts/**/*.ts'],
			exclude: ['**/*.d.ts'],
			reportOnFailure: true
		},
		projects: [
			{
				plugins: [wasmBytes()],
				test: {
					name: 'node',
					include: ['tests/node/*.spec.ts'],
					environment: 'node'
				}
			},
			{
				plugins: [
					wasmBytes(),
					cloudflareTest({
						miniflare: {
							compatibilityDate: '2026-08-22',
							serviceBindings: { PROBE: 'tinyimg-probe', CODEC: 'tinyimg-codec' },
							workers: [probe, codec]
						}
					})
				],
				test: {
					name: 'workers',
					include: ['tests/workers/*.spec.ts']
				}
			}
		]
	}
});
