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

/**
 * A third worker running the shipped wrapper rather than the raw exports.
 *
 * It imports `dist/`, not `src/ts/`, and that is the point: the other two prove the C works inside
 * workerd, and this proves the package a caller installs does. Miniflare takes an explicit module
 * list and does not transpile, so the compiled output is also the only thing it could load.
 *
 * `bun run test:workers` builds `dist/` first for this reason.
 */
const wrapper = {
	name: 'tinyimg-wrapper',
	compatibilityDate: '2026-08-22',
	modulesRoot: root,
	modules: [
		{
			type: 'ESModule' as const,
			path: join(root, 'tests/workers/fixtures/wrapper-worker.mjs')
		},
		...['index', 'image', 'transform', 'types', 'wasm'].map((name) => ({
			type: 'ESModule' as const,
			path: join(root, `dist/${name}.js`)
		})),
		{ type: 'CompiledWasm' as const, path: join(root, 'bin/tinyimg.wasm') }
	]
};

export default defineConfig({
	test: {
		coverage: {
			/*
			 * The node lane is the only one coverage can measure, for two reasons that stack.
			 *
			 * This provider cannot collect anything under workerd: it reads counters through
			 * `node:inspector`, which workerd does not implement, so the workers lane reports 0%
			 * even for a function the test imports and calls directly. Measured, not assumed.
			 *
			 * Switching that lane to `istanbul`, which instruments at transform time and needs no
			 * inspector, lifts it to 1.87% of 481 statements and no further. Those tests reach the
			 * library through `env.PROBE`, `env.CODEC` and `env.WRAPPER`: service bindings to
			 * separate workers that miniflare instantiates from file paths, in their own isolates,
			 * with no transform applied. Nothing under test is in the instrumented module graph.
			 *
			 * So the workers lane is a behavior check on the package a caller installs, and it
			 * emits JUnit results and no coverage; see `test:workers:junit`. A 2% report on a lane
			 * that exercises the whole package end to end is worse than none.
			 */
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
							serviceBindings: {
								PROBE: 'tinyimg-probe',
								CODEC: 'tinyimg-codec',
								WRAPPER: 'tinyimg-wrapper'
							},
							workers: [probe, codec, wrapper]
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
