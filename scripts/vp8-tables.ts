/**
 * Extracts VP8's constant tables from RFC 6386 and writes them as C.
 *
 * ```sh
 * bun scripts/vp8-tables.ts           # regenerate src/codec/vp8-tables.h
 * bun scripts/vp8-tables.ts --check   # verify the committed header matches
 * ```
 *
 * There are about 3,300 numbers across the seven tables, and none of them can be derived: they are
 * trained probabilities and quantizer curves. Transcribing that by hand produces one wrong digit
 * and a decoder that is subtly wrong on one input, which is the kind of defect that survives every
 * test that does not happen to hit it. The specification ships them as C already, so the only work
 * is stripping the page furniture the text format interleaves and re-emitting them in the repo's
 * brace style.
 *
 * The committed header is what the build reads, so this needs no network and no RFC copy for an
 * ordinary build. `--check` is what makes the header trustworthy rather than merely present, and it
 * runs where the fixture check runs: locally, on demand.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = join(import.meta.dirname, '..');
const HEADER = join(ROOT, 'src', 'codec', 'vp8-tables.h');
const CACHE = join(ROOT, 'node_modules', '.cache', 'rfc6386.txt');
const SOURCE = 'https://www.rfc-editor.org/rfc/rfc6386.txt';

/**
 * The declaration line of each table, and how many values follow it.
 *
 * Line numbers rather than a search for the identifier, because the specification quotes several of
 * these names in prose before defining them. The counts are the assertion: a shifted line or a
 * reformatted RFC changes them and the extraction fails rather than emitting a plausible table.
 */
const TABLES = {
	zigzag: { line: 13065, count: 16 },
	bands: { line: 3561, count: 16 },
	dcQuant: { line: 4277, count: 128 },
	acQuant: { line: 4291, count: 128 },
	coeffProbs: { line: 4045, count: 4 * 8 * 3 * 11 },
	coeffUpdate: { line: 3819, count: 4 * 8 * 3 * 11 },
	bmodeProbs: { line: 2607, count: 10 * 10 * 9 }
} as const;

function rfc(): string[] {
	if (!existsSync(CACHE)) {
		execFileSync('mkdir', ['-p', join(CACHE, '..')]);
		execFileSync('curl', ['-sfL', '-o', CACHE, SOURCE]);
	}

	return readFileSync(CACHE, 'utf8').split('\n');
}

/**
 * Reads a run of numbers from a table's body.
 *
 * @param lines The specification, split into lines.
 * @param declaration The 1-based line the declaration sits on; extraction starts after it.
 * @param count How many values the table holds.
 */
function values(lines: string[], declaration: number, count: number): number[] {
	const out: number[] = [];

	for (const line of lines.slice(declaration)) {
		// page headers, footers and form feeds interrupt every table
		if (/Bankoski|RFC 6386|\f/.test(line)) continue;
		if (/---- End code block/.test(line)) break;

		// a declaration can wrap onto a second line and carry array bounds with it, so only lines
		// that are data take part: those start with a brace or a number
		if (!/^\s*[{}\d]/.test(line)) continue;

		for (const match of line.matchAll(/-?\d+/g)) out.push(Number(match[0]));
		if (out.length >= count) break;
	}

	if (out.length !== count) {
		throw new Error(`line ${declaration}: wanted ${count} values, read ${out.length}`);
	}

	return out;
}

function rows(list: number[], perRow: number): string {
	const out: string[] = [];

	for (let i = 0; i < list.length; i += perRow) {
		out.push('    ' + list.slice(i, i + perRow).join(', '));
	}

	return out.join(',\n');
}

/** Nests a flat list back into the brace structure the C declaration needs. */
function nest(list: number[], dims: number[], indent = '    '): string {
	if (dims.length === 1) return `{${list.join(', ')}}`;

	const [head, ...rest] = dims as [number, ...number[]];
	const stride = rest.reduce((a, b) => a * b, 1);
	const parts: string[] = [];

	for (let i = 0; i < head; i++) {
		parts.push(
			indent + '    ' + nest(list.slice(i * stride, (i + 1) * stride), rest, indent + '    ')
		);
	}

	return `{\n${parts.join(',\n')}\n${indent}}`;
}

function generate(): string {
	const lines = rfc();
	const read = (key: keyof typeof TABLES) => values(lines, TABLES[key].line, TABLES[key].count);

	return `/*
 * Generated from RFC 6386's reference decoder by scripts/vp8-tables.ts. Do not hand edit.
 */

static const uint8_t vp8_zigzag[16] = {${read('zigzag').join(', ')}};

static const uint8_t vp8_bands[16] = {${read('bands').join(', ')}};

static const uint16_t vp8_dc_quant[128] = {
${rows(read('dcQuant'), 13)}
};

static const uint16_t vp8_ac_quant[128] = {
${rows(read('acQuant'), 13)}
};

static const uint8_t vp8_coeff_probs[4][8][3][11] = ${nest(read('coeffProbs'), [4, 8, 3, 11])};

static const uint8_t vp8_coeff_update[4][8][3][11] = ${nest(read('coeffUpdate'), [4, 8, 3, 11])};

static const uint8_t vp8_bmode_probs[10][10][9] = ${nest(read('bmodeProbs'), [10, 10, 9])};
`;
}

/**
 * Runs the output through clang-format.
 *
 * The generated header sits under `src/` and so is formatted along with everything else, which
 * would otherwise make `--check` fail the moment `format.sh` ran. Formatting here rather than
 * excluding the file keeps it looking like its neighbors and keeps the two in agreement by
 * construction. `--assume-filename` is what picks up the repo's `.clang-format`.
 */
function formatted(text: string): string {
	return execFileSync('clang-format', [`--assume-filename=${HEADER}`], {
		input: text,
		encoding: 'utf8'
	});
}

const wanted = formatted(generate());

if (process.argv.includes('--check')) {
	const have = readFileSync(HEADER, 'utf8');

	if (have !== wanted) {
		console.error(`${HEADER} does not match what RFC 6386 says`);
		process.exit(1);
	}

	console.log('vp8-tables.h matches RFC 6386');
} else {
	writeFileSync(HEADER, wanted);
	console.log(`wrote ${HEADER}`);
}
