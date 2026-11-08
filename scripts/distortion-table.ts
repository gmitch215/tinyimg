/**
 * Aggregates the distortion measurements and writes them as C.
 *
 * ```sh
 * bun scripts/distortion-table.ts           # regenerate src/distortion-table.h and table.json
 * bun scripts/distortion-table.ts --check   # verify both match the committed measurements
 * bun scripts/distortion-table.ts --markdown # print the two tables for TECHNICAL_REPORT.md
 * ```
 *
 * The cells under `bench/baselines/distortion/` are the record: one JSON line per measured
 * (image, width, candidate) for the lever table and one per (image, format, quality) for the encode
 * table, written by `bun scripts/measure/distortion.ts`. Everything downstream is a pure function
 * of them, so `--check` regenerates the aggregate and the header and fails on a difference. It
 * needs no probe build and no image tooling.
 *
 * The fit reads the cal cells when they exist and the fixture cells, minus every image whose sha256
 * is a `test` entry in `scripts/corpus-manifest.json`. The held-out split is never fitted from.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = join(import.meta.dirname, '..');
export const BASELINES = join(ROOT, 'bench', 'baselines', 'distortion');
const HEADER = join(ROOT, 'src', 'distortion-table.h');
const TABLE = join(BASELINES, 'table.json');

/** A cell group smaller than this is left out of the table and reads as uncharacterized. */
export const MIN_SAMPLES = 8;

/** dB recorded for two identical outputs. */
export const IDENTICAL_DB = 99;

export const FORMAT_ENUM: Record<string, string> = {
	png: 'TINYIMG_FORMAT_PNG',
	jpeg: 'TINYIMG_FORMAT_JPEG',
	bmp: 'TINYIMG_FORMAT_BMP',
	gif: 'TINYIMG_FORMAT_GIF',
	tiff: 'TINYIMG_FORMAT_TIFF',
	webp: 'TINYIMG_FORMAT_WEBP',
	avif: 'TINYIMG_FORMAT_AVIF'
};

export type DistortionClass = 'EXACT' | 'BOUNDED' | 'EMPIRICAL';

export type LeverCell = {
	image: string;
	src_format: string;
	width: number;
	k: number;
	effort_delta: number;
	filter_delta: number;
	scale_steps: number;
	db: number;
	max_abs: number;
	identical: boolean;
	error?: boolean;
	extent_changed?: boolean;
};

export type EncodeCell = {
	image: string;
	format: string;
	quality: number;
	channels: number;
	bytes: number;
	bpp: number;
	db: number | null;
	db_pm?: number | null;
	identical?: boolean;
	time_us: number | null;
	error?: number | string;
};

export type LeverRow = {
	format: string;
	effort_delta: number;
	filter_delta: number;
	scale_steps: number;
	class: DistortionClass;
	p05: number;
	median: number;
	n: number;
	max_abs: number;
};

export type EncodeRow = {
	format: string;
	quality: number;
	class: DistortionClass;
	bpp_median: number;
	bpp_p95: number;
	p05: number;
	median: number;
	n: number;
};

export type Table = {
	source: string;
	min_samples: number;
	excluded_test_images: string[];
	images: { lever: number; encode: number };
	lever: LeverRow[];
	encode: EncodeRow[];
};

/** Linear interpolation between order statistics, `p` in [0, 1]. */
export function percentile(values: number[], p: number): number {
	const sorted = [...values].sort((a, b) => a - b);
	const pos = (sorted.length - 1) * p;
	const lo = Math.floor(pos);
	const hi = Math.ceil(pos);
	return sorted[lo]! + (sorted[hi]! - sorted[lo]!) * (pos - lo);
}

export function readJsonl<T>(file: string): T[] {
	return readFileSync(file, 'utf8')
		.split('\n')
		.filter((line) => line.trim() !== '')
		.map((line) => JSON.parse(line) as T);
}

function round(value: number, places: number): number {
	const scale = 10 ** places;
	return Math.round(value * scale) / scale;
}

export function leverRows(cells: LeverCell[]): LeverRow[] {
	const groups = new Map<string, LeverCell[]>();

	for (const c of cells) {
		if (c.error || c.extent_changed) continue;
		const key = [c.src_format, c.effort_delta, c.filter_delta, c.scale_steps].join('|');
		(groups.get(key) ?? groups.set(key, []).get(key)!).push(c);
	}

	const rows: LeverRow[] = [];

	for (const group of groups.values()) {
		if (group.length < MIN_SAMPLES) continue;

		const first = group[0]!;
		const dbs = group.map((c) => c.db);
		const exact = group.every((c) => c.identical);

		rows.push({
			format: first.src_format,
			effort_delta: first.effort_delta,
			filter_delta: first.filter_delta,
			scale_steps: first.scale_steps,
			class: exact ? 'EXACT' : 'EMPIRICAL',
			p05: exact ? IDENTICAL_DB : round(percentile(dbs, 0.05), 2),
			median: exact ? IDENTICAL_DB : round(percentile(dbs, 0.5), 2),
			n: group.length,
			max_abs: Math.max(...group.map((c) => c.max_abs))
		});
	}

	return rows.sort(
		(a, b) =>
			a.format.localeCompare(b.format) ||
			a.effort_delta - b.effort_delta ||
			a.filter_delta - b.filter_delta ||
			a.scale_steps - b.scale_steps
	);
}

export function encodeRows(cells: EncodeCell[]): EncodeRow[] {
	const groups = new Map<string, EncodeCell[]>();

	for (const c of cells) {
		if (c.error !== undefined || c.db === null) continue;
		// the lossy WebP and AVIF encoders write no alpha, so an alpha reference measures that defect
		if (c.channels === 2 || c.channels === 4) continue;
		const key = `${c.format}|${c.quality}`;
		(groups.get(key) ?? groups.set(key, []).get(key)!).push(c);
	}

	const rows: EncodeRow[] = [];

	for (const group of groups.values()) {
		if (group.length < MIN_SAMPLES) continue;

		const first = group[0]!;
		const dbs = group.map((c) => c.db!);
		const exact = group.every((c) => c.db === IDENTICAL_DB);

		rows.push({
			format: first.format,
			quality: first.quality,
			class: exact ? 'EXACT' : 'EMPIRICAL',
			bpp_median: round(
				percentile(
					group.map((c) => c.bpp),
					0.5
				),
				4
			),
			bpp_p95: round(
				percentile(
					group.map((c) => c.bpp),
					0.95
				),
				4
			),
			p05: exact ? IDENTICAL_DB : round(percentile(dbs, 0.05), 2),
			median: exact ? IDENTICAL_DB : round(percentile(dbs, 0.5), 2),
			n: group.length
		});
	}

	return rows.sort((a, b) => a.format.localeCompare(b.format) || a.quality - b.quality);
}

/** sha256 of every manifest entry in the held-out split, which no cell may be fitted from. */
function testShas(): Set<string> {
	const manifest = join(ROOT, 'scripts', 'corpus-manifest.json');
	if (!existsSync(manifest)) return new Set();

	const { entries } = JSON.parse(readFileSync(manifest, 'utf8')) as {
		entries: { sha256: string; split: string }[];
	};
	return new Set(entries.filter((e) => e.split === 'test').map((e) => e.sha256));
}

/**
 * The cells the table is fitted from.
 *
 * The cal split when its cells exist, plus every fixture that is neither in the held-out split nor
 * already a cal entry (steam.png is both a fixture and a cal entry, and counting it twice would
 * weight it double). Four local fixtures are `test` entries in the manifest, so the fixture cells
 * carry them but the fit never reads them.
 */
export function fitCells(): {
	source: string;
	lever: LeverCell[];
	encode: EncodeCell[];
	excluded: string[];
} {
	const read = <T>(name: string): T[] => {
		const file = join(BASELINES, name);
		return existsSync(file) ? readJsonl<T>(file) : [];
	};
	const inputs = JSON.parse(readFileSync(join(BASELINES, 'inputs.fixtures.json'), 'utf8')) as {
		files: { id: string; sha256: string }[];
	};
	const held = testShas();
	const excluded = inputs.files.filter((f) => held.has(f.sha256)).map((f) => f.id);
	const hasCal =
		existsSync(join(BASELINES, 'lever-cells.cal.jsonl')) &&
		existsSync(join(BASELINES, 'encode-cells.cal.jsonl'));

	const calLever = hasCal ? read<LeverCell>('lever-cells.cal.jsonl') : [];
	const calEncode = hasCal ? read<EncodeCell>('encode-cells.cal.jsonl') : [];
	const taken = new Set([...calLever, ...calEncode].map((c) => c.image));
	const usable = (c: { image: string }) => !excluded.includes(c.image) && !taken.has(c.image);

	return {
		source: hasCal ? 'cal+fixtures' : 'fixtures',
		lever: [...calLever, ...read<LeverCell>('lever-cells.fixtures.jsonl').filter(usable)],
		encode: [...calEncode, ...read<EncodeCell>('encode-cells.fixtures.jsonl').filter(usable)],
		excluded
	};
}

export function buildTable(): Table {
	const { source, lever, encode, excluded } = fitCells();

	return {
		source,
		min_samples: MIN_SAMPLES,
		excluded_test_images: excluded,
		images: {
			lever: new Set(lever.map((c) => c.image)).size,
			encode: new Set(encode.map((c) => c.image)).size
		},
		lever: leverRows(lever),
		encode: encodeRows(encode)
	};
}

function db(value: number): string {
	return `${value.toFixed(2)}f`;
}

function generate(table: Table): string {
	const lever = table.lever.map(
		(r) =>
			`{${FORMAT_ENUM[r.format]}, ${r.effort_delta}, ${r.filter_delta}, ${r.scale_steps}, ` +
			`TINYIMG_DISTORTION_${r.class}, ${r.max_abs}, ${r.n}, ${db(r.p05)}, ${db(r.median)}}`
	);
	const encode = table.encode.map(
		(r) =>
			`{${FORMAT_ENUM[r.format]}, ${r.quality}, TINYIMG_DISTORTION_${r.class}, ${r.n}, ` +
			`${r.bpp_median.toFixed(4)}f, ${r.bpp_p95.toFixed(4)}f, ${db(r.p05)}, ${db(r.median)}}`
	);

	return `/*
 * Generated from bench/baselines/distortion by scripts/distortion-table.ts. Do not hand edit.
 * Fitted from ${table.source} (${table.images.lever} images lever, ${table.images.encode} encode), groups under ${table.min_samples} samples left out.
 */

#define TINYIMG_DISTORTION_LEVER_ROWS ${lever.length}
#define TINYIMG_DISTORTION_ENCODE_ROWS ${encode.length}

static const LeverRow lever_rows[TINYIMG_DISTORTION_LEVER_ROWS] = {
${lever.join(',\n')}
};

static const EncodeRow encode_rows[TINYIMG_DISTORTION_ENCODE_ROWS] = {
${encode.join(',\n')}
};
`;
}

function formatted(text: string): string {
	return execFileSync('clang-format', [`--assume-filename=${HEADER}`], {
		input: text,
		encoding: 'utf8'
	});
}

/** Compared as parsed values, so a prettier pass over table.json cannot fail the check. */
function sameJson(file: string, table: Table): boolean {
	return (
		existsSync(file) &&
		JSON.stringify(JSON.parse(readFileSync(file, 'utf8'))) === JSON.stringify(table)
	);
}

function markdown(table: Table): string {
	const lever = table.lever.map(
		(r) =>
			`| ${r.format} | ${r.effort_delta} | ${r.filter_delta} | ${r.scale_steps} | ${r.class} | ` +
			`${r.p05.toFixed(2)} | ${r.median.toFixed(2)} | ${r.n} | ${r.max_abs} |`
	);
	const encode = table.encode.map(
		(r) =>
			`| ${r.format} | ${r.quality} | ${r.class} | ${r.bpp_median.toFixed(4)} | ` +
			`${r.bpp_p95.toFixed(4)} | ${r.p05.toFixed(2)} | ${r.median.toFixed(2)} | ${r.n} |`
	);

	return [
		'| Source | Effort | Filter | Scale steps | Class | p05 dB | Median dB | n | Max abs |',
		'| ------ | ------ | ------ | ----------- | ----- | ------ | --------- | - | ------- |',
		...lever,
		'',
		'| Format | Quality | Class | Bytes per pixel | p95 bytes per pixel | p05 dB | Median dB | n |',
		'| ------ | ------- | ----- | --------------- | ------------------- | ------ | --------- | - |',
		...encode,
		''
	].join('\n');
}

function main(): number {
	const table = buildTable();

	if (process.argv.includes('--markdown')) {
		console.log(markdown(table));
		return 0;
	}

	const header = formatted(generate(table));

	if (process.argv.includes('--check')) {
		const problems: string[] = [];

		if (!existsSync(HEADER) || readFileSync(HEADER, 'utf8') !== header) {
			problems.push(`${HEADER} does not match the measurements`);
		}
		if (!sameJson(TABLE, table)) {
			problems.push(`${TABLE} does not match the measurements`);
		}

		for (const p of problems) console.error(p);
		if (problems.length > 0) return 1;

		console.log(
			`distortion-table.h matches the ${table.source} cells (` +
				`${table.lever.length} lever rows, ${table.encode.length} encode rows)`
		);
		return 0;
	}

	writeFileSync(HEADER, header);
	writeFileSync(TABLE, JSON.stringify(table, null, '\t') + '\n');
	console.log(
		`wrote ${HEADER} and ${TABLE}: ${table.lever.length} lever rows, ${table.encode.length} encode rows`
	);
	return 0;
}

if (import.meta.main) {
	process.exitCode = main();
}
