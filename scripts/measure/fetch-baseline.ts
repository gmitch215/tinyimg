/**
 * Downloads the size document from the last passing master run of the metrics workflow.
 *
 * ```sh
 * bun scripts/measure/fetch-baseline.ts --out=baseline/size.json
 * ```
 *
 * The baseline is another run's artifact rather than a committed file, so nothing has to be refreshed
 * by hand and a stale number cannot sit in the repository pretending to be current.
 *
 * Prints `found=true` to `GITHUB_OUTPUT` when it retrieved one. A missing baseline is not a failure:
 * the first run on a new branch has nothing to compare against, and the report says so rather than
 * inventing a delta.
 */

import { execFileSync } from 'node:child_process';
import { appendFileSync, existsSync, mkdirSync, readdirSync, renameSync } from 'node:fs';
import { dirname, join } from 'node:path';

const WORKFLOW = 'metrics.yml';
const ARTIFACT = 'metrics';

function flag(name: string, fallback: string): string {
	const found = process.argv.find((argument) => argument.startsWith(`--${name}=`));
	return found ? found.slice(name.length + 3) : fallback;
}

function gh(args: string[]): string {
	return execFileSync('gh', args, { encoding: 'utf8' });
}

function report(found: boolean) {
	const output = process.env.GITHUB_OUTPUT;
	if (output) appendFileSync(output, `found=${found}\n`);
	console.log(`baseline found: ${found}`);
}

const out = flag('out', 'baseline/size.json');
const repository = process.env.GITHUB_REPOSITORY;

if (!repository) {
	console.error('GITHUB_REPOSITORY is unset; this only runs inside Actions');
	report(false);
	process.exit(0);
}

try {
	const runs = JSON.parse(
		gh([
			'run',
			'list',
			'--repo',
			repository,
			'--workflow',
			WORKFLOW,
			'--branch',
			'master',
			'--status',
			'success',
			'--limit',
			'1',
			'--json',
			'databaseId'
		])
	) as { databaseId: number }[];

	const run = runs[0]?.databaseId;
	if (!run) {
		console.error('no passing master run of the metrics workflow yet');
		report(false);
		process.exit(0);
	}

	const into = dirname(out);
	mkdirSync(into, { recursive: true });
	gh(['run', 'download', String(run), '--repo', repository, '--name', ARTIFACT, '--dir', into]);

	// the artifact carries the file under its own name, so it is moved to where the gate expects it
	if (!existsSync(out)) {
		const downloaded = readdirSync(into).find((name) => name.endsWith('.json'));
		if (!downloaded) {
			console.error(`the ${ARTIFACT} artifact from run ${run} holds no json`);
			report(false);
			process.exit(0);
		}
		renameSync(join(into, downloaded), out);
	}

	console.log(`baseline from run ${run} at ${out}`);
	report(true);
} catch (error) {
	console.error(error instanceof Error ? error.message : error);
	report(false);
}
