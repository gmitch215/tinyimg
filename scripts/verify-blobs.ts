/**
 * Checks the published blobs against their manifest.
 *
 * The manifest and the payload are uploaded separately, so a partial upload leaves a manifest that
 * describes files nobody can fetch. Nothing in the build depends on the blobs, which is exactly why
 * that state can sit unnoticed: the first sign would be text rendering or face detection failing at
 * runtime, a long way from the cause.
 *
 * Fetching without checking the digest is not enough either. A CDN answers a missing path with an
 * HTML error page and a 200 is not guaranteed, so a naive check hashes the error page and reports a
 * mismatch it cannot explain, or worse passes on a redirect.
 */

/**
 * The manifests to check, one per origin.
 *
 * Each entry's `path` resolves against the manifest's own URL, the way a sourcemap or an import map
 * resolves its members. That is what lets a caller hold one URL instead of two, and it is why
 * `blobs.json` belongs beside the files it lists rather than a level above them.
 */
const MANIFESTS = [
	'https://cdn.gmitch215.dev/tinyimg/blobs.json',
	'https://cdn.gmitch215.xyz/tinyimg/blobs.json',
	'https://cdn.gmitch215.blog/tinyimg/blobs.json'
];

interface Entry {
	path: string;
	bytes: number;
	sha256: string;
	type: string;
}

async function sha256(bytes: ArrayBuffer): Promise<string> {
	const digest = await crypto.subtle.digest('SHA-256', bytes);

	return Array.from(new Uint8Array(digest))
		.map((byte) => byte.toString(16).padStart(2, '0'))
		.join('');
}

interface Row {
	path: string;
	status: number;
	expected: number;
	served: number;
	digest: 'match' | 'mismatch' | 'unchecked';
}

async function verify(manifestUrl: string): Promise<{ rows: Row[]; wrong: number }> {
	const manifestResponse = await fetch(manifestUrl);
	if (!manifestResponse.ok) {
		throw new Error(`${manifestUrl} returned ${manifestResponse.status}`);
	}

	const manifest = (await manifestResponse.json()) as { blobs: Record<string, Entry> };
	const entries = Object.values(manifest.blobs).sort((a, b) => a.path.localeCompare(b.path));

	const rows: Row[] = [];
	let wrong = 0;

	for (const entry of entries) {
		const response = await fetch(new URL(entry.path, manifestUrl));

		if (!response.ok) {
			rows.push({
				path: entry.path,
				status: response.status,
				expected: entry.bytes,
				served: 0,
				digest: 'unchecked'
			});
			wrong++;
			continue;
		}

		const body = await response.arrayBuffer();
		const matches = (await sha256(body)) === entry.sha256;

		rows.push({
			path: entry.path,
			status: response.status,
			expected: entry.bytes,
			served: body.byteLength,
			digest: matches ? 'match' : 'mismatch'
		});
		if (!matches) wrong++;
	}

	return { rows, wrong };
}

function report(origin: string, rows: Row[], wrong: number) {
	console.log(origin);
	console.log(
		`  ${'path'.padEnd(32)} ${'http'.padStart(5)} ${'expected'.padStart(9)} ${'served'.padStart(9)}  digest`
	);

	for (const row of rows) {
		console.log(
			`  ${row.path.padEnd(32)} ${String(row.status).padStart(5)} ${String(row.expected).padStart(9)} ${String(row.served).padStart(9)}  ${row.digest}`
		);
	}

	if (wrong > 0) {
		console.error(
			`  ${wrong} of ${rows.length} blobs are missing or do not match the manifest`
		);
	} else {
		console.log(`  ${rows.length} blobs match the manifest`);
	}
}

if (import.meta.main) {
	const args = process.argv.slice(2);

	// an explicit manifest URL, for checking a layout other than the published one
	const named = args.indexOf('--manifest');
	const origins =
		named >= 0 && args[named + 1]
			? [args[named + 1]!]
			: args.includes('--mirrors')
				? MANIFESTS
				: [MANIFESTS[0]!];

	let failed = 0;

	for (const origin of origins) {
		try {
			const { rows, wrong } = await verify(origin);
			report(origin, rows, wrong);
			failed += wrong;
		} catch (error) {
			console.error(`${origin}: ${error instanceof Error ? error.message : error}`);
			failed++;
		}
	}

	if (failed > 0) process.exitCode = 1;
}
