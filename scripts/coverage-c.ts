/**
 * Builds the instrumented C library, runs ctest over it, and writes the two reports Codecov reads.
 *
 * ```sh
 * bun scripts/coverage-c.ts
 * bun scripts/coverage-c.ts --build build-native --skip-build   # a build that already ran ctest
 * bun scripts/coverage-c.ts --check                             # verify coverage/c.xml only
 * ```
 *
 * Cobertura, not lcov, and the choice is measured rather than preferred: `gcovr --lcov` writes
 * absolute `SF:` paths, and Codecov matches coverage onto the repo tree by repo-relative path, so an
 * absolute one matches zero files and the whole report is rejected as unusable. The cobertura writer
 * emits `filename="src/codec/png.c"` under `--root .`, which is what the tree looks like.
 *
 * The verification at the end exists because that rejection is invisible from CI: the upload step
 * succeeds, the job is green, and the number on the dashboard is simply the previous one carried
 * forward.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = join(import.meta.dirname, '..');
const COVERAGE = join(ROOT, 'coverage');
const REPORT = join(COVERAGE, 'c.xml');

function run(command: string, args: string[]) {
	execFileSync(command, args, { cwd: ROOT, stdio: 'inherit' });
}

/**
 * A separate build directory from `build-native` by default.
 *
 * `--coverage` changes the codegen of the library every test links against, so sharing one directory
 * would mean the plain `bun run test:c` either rebuilds the world or silently runs the instrumented
 * objects. CI already builds an instrumented tree, so it passes that one in with `--skip-build`.
 */
/**
 * Deletes the accumulated execution counts, so a report describes only this run.
 *
 * Counters accumulate across runs by design, and after a source file changes the older ones no
 * longer describe the same lines. gcov merges them anyway rather than refusing: that reported
 * `src/image.c` at 44% with 400 tested lines listed as missing, and dropped the total by four
 * points, on a run where every test passed.
 */
function clearCounters(dir: string) {
	const walk = (at: string) => {
		for (const entry of readdirSync(at)) {
			const path = join(at, entry);

			if (statSync(path).isDirectory()) walk(path);
			else if (entry.endsWith('.gcda')) rmSync(path);
		}
	};

	if (existsSync(dir)) walk(dir);
}

function build(dir: string) {
	run('cmake', ['-S', '.', '-B', dir, '-DDOCS_TINYIMG=OFF', '-DTINYIMG_COVERAGE=ON']);
	run('cmake', ['--build', dir, '--parallel']);

	clearCounters(dir);
	mkdirSync(COVERAGE, { recursive: true });
	run('ctest', [
		'--test-dir',
		dir,
		'--output-on-failure',
		'--parallel',
		'4',
		'--output-junit',
		join(COVERAGE, 'ctest.junit.xml')
	]);
}

/** The build directory is the search path, so a stale one elsewhere in the tree cannot be merged in. */
function report(dir: string) {
	mkdirSync(COVERAGE, { recursive: true });
	run('gcovr', [
		'--root',
		'.',
		'--filter',
		'src/',
		'--cobertura',
		REPORT,
		'--cobertura-pretty',
		'--txt',
		join(COVERAGE, 'c.txt'),
		dir
	]);
}

export interface CoverageCheck {
	files: number;
	absolute: string[];
	lineRate: number;
}

/** Reads back what gcovr wrote, since an unusable report is indistinguishable from a good one in CI. */
export function check(xml: string): CoverageCheck {
	const filenames = [...xml.matchAll(/<class\b[^>]*\bfilename="([^"]+)"/g)].map((match) =>
		String(match[1])
	);
	const rate = /<coverage\b[^>]*\bline-rate="([0-9.]+)"/.exec(xml);

	return {
		files: filenames.length,
		absolute: filenames.filter((name) => name.startsWith('/') || /^[A-Za-z]:[\\/]/.test(name)),
		lineRate: rate ? Number(rate[1]) : 0
	};
}

if (import.meta.main) {
	const args = process.argv.slice(2);
	const named = args.indexOf('--build');
	const dir = named >= 0 && args[named + 1] ? args[named + 1]! : 'build-cov';

	if (!args.includes('--check')) {
		if (!args.includes('--skip-build')) build(dir);
		report(dir);
	}

	if (!existsSync(REPORT)) {
		console.error(`coverage-c: ${REPORT} was not written`);
		process.exit(1);
	}

	const result = check(readFileSync(REPORT, 'utf8'));
	const percent = (result.lineRate * 100).toFixed(2);

	if (result.files === 0) {
		console.error('coverage-c: the report names no source files, so it covers nothing');
		process.exit(1);
	}

	if (result.absolute.length > 0) {
		console.error(
			`coverage-c: ${result.absolute.length} absolute paths, which Codecov cannot map onto the tree:`
		);
		for (const name of result.absolute) console.error(`  ${name}`);
		process.exit(1);
	}

	console.log(`coverage-c: ${result.files} files, ${percent}% of lines, all paths repo-relative`);
}
