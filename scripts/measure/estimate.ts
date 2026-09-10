/**
 * Scores the planner's cost estimate against the clock, per format and per budget.
 *
 * ```sh
 * bun scripts/measure/estimate.ts
 * ```
 *
 * A periodic eval rather than a gate test. The estimate is a static model of one machine's rates,
 * so an assertion comparing it to a live clock fails on any machine that is not the one it was
 * calibrated on, and `tests/node/budget.spec.ts` deliberately keeps its own band loose for that
 * reason. This script is where the band is tight, and it names the machine it passed on.
 *
 * Exits non-zero when a cell falls outside the band, so it can be run before a release.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { join } from 'node:path';
import type { ImageFormat } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';

const ROOT = join(import.meta.dirname, '..', '..');
const RUNS = 11;

/*
 * The band the model is expected to hold. Wide because it is one set of per-sample rates standing
 * in for six codecs at four denominators, and asymmetric because the two directions are not equally
 * bad: over-charging makes the planner degrade a request further than it needed to, while
 * under-charging makes it report that something fits when it does not.
 */
const OVER = 2.0;
const UNDER = 0.6;

const tinyimg = await TinyImgModule.load(
	await WebAssembly.compile(readFileSync(join(ROOT, 'bin', 'tinyimg.wasm')))
);

function median(values: number[]): number {
	return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]!;
}

async function time(body: () => unknown | Promise<unknown>): Promise<number> {
	const samples: number[] = [];

	for (let i = 0; i < RUNS; i++) {
		const start = performance.now();
		await body();
		samples.push(performance.now() - start);
	}

	return median(samples);
}

const source = readFileSync(join(ROOT, 'tests', 'fixtures', 'sf-24.jpg'));
const encoded = new Map<ImageFormat, Uint8Array>();

for (const format of ['jpeg', 'png', 'webp', 'gif', 'tiff', 'bmp', 'avif'] as ImageFormat[]) {
	const bytes = new Uint8Array(source.byteLength);
	bytes.set(source);

	using original = await Image.open(tinyimg, bytes);
	encoded.set(format, await original.bytes(format, { quality: 80 }));
}

/*
 * Refuse to report on a busy machine rather than print ratios that describe the load.
 *
 * A run at a load average of 24 doubled every clock and left every estimate identical, which reads
 * as the model under-predicting by half across the board. The estimates being byte-identical is
 * what gave it away, and an instrument that can produce that silently is worse than one that stops.
 */
const load = loadavg()[0]!;
const cores = cpus().length;

if (load > cores / 4) {
	console.error(
		`load average ${load.toFixed(2)} on ${cores} cores is too high to time against.\n` +
			`Every clock here would measure the load. Wait for it to fall below ${(
				cores / 4
			).toFixed(2)} and re-run.`
	);
	process.exit(2);
}

console.log(
	`${platform()}-${arch()}, ${cpus()[0]?.model ?? 'unknown cpu'}, load ${load.toFixed(2)}`
);
console.log(`band: estimate within ${UNDER}x to ${OVER}x of the clock\n`);
console.log('format  width  budget    estimate     clock   ratio');

let failures = 0;

for (const [format, bytes] of encoded) {
	for (const width of [200, 400, 800]) {
		for (const budget of [0, 10]) {
			using image = await Image.open(tinyimg, bytes);
			image.resize(width, 0);
			if (budget) image.budget(budget);

			const decided = image.decide();
			const estimate = decided.estimateMs;

			const clock = await time(async () => {
				using run = await Image.open(tinyimg, bytes);
				run.resize(width, 0);
				if (budget) run.budget(budget);
				await run.pixels();
			});

			const ratio = estimate / clock;
			const ok = ratio >= UNDER && ratio <= OVER;

			if (!ok) failures++;

			console.log(
				`${format.padEnd(6)} ${String(width).padStart(5)} ${String(budget).padStart(6)}  ` +
					`${estimate.toFixed(2).padStart(8)}  ${clock.toFixed(2).padStart(8)}  ` +
					`${ratio.toFixed(2).padStart(6)}${ok ? '' : '  OUT OF BAND'}`
			);
		}
	}
}

const revision = (() => {
	try {
		return execFileSync('git', ['rev-parse', '--short', 'HEAD'], { encoding: 'utf8' }).trim();
	} catch {
		return 'unknown';
	}
})();

console.log(
	`\n${failures === 0 ? 'every cell in band' : `${failures} cells out of band`} at ${revision}`
);

if (failures > 0) process.exit(1);
