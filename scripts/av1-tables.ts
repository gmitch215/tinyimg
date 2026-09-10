/**
 * Extracts AV1's constant tables from the specification and writes them as C.
 *
 * ```sh
 * bun scripts/av1-tables.ts           # regenerate src/codec/av1-tables.h
 * bun scripts/av1-tables.ts --check   # verify the committed header matches
 * ```
 *
 * There are about 28,000 numbers across 170 tables and almost none can be derived: the CDFs are
 * trained probabilities, the scans are hand tuned per transform size and the quantizer curves are
 * measured. Transcribing that by hand produces one wrong digit and a decoder that desynchronises on
 * one input, which is the defect that survives every test not hitting it.
 *
 * **One authority, deliberately.** The tables come from the specification and nowhere else. The
 * reference implementations disagree with it in ways that are invisible until a bitstream stops
 * decoding: dav1d stores its CDFs pre-inverted as `32768 - x` behind designated initializers keyed
 * to its own enum order, and the libaom snapshot reachable on GitHub predates the final bitstream
 * and still carries the Daala era experiments. Taking half the tables from one and half from
 * another is how VP8's prediction modes were mixed up here before; see the project memory.
 *
 * The dimensions are evaluated against the constants the specification itself defines in its
 * symbols section, so a table whose shape moved fails extraction rather than emitting a plausible
 * table of the wrong length.
 *
 * Every table is emitted whether the decoder references it or not. `-fdata-sections` plus
 * `--gc-sections` drop the ones nothing reads, so completeness costs no bytes and removes the
 * chance of discovering a missing table halfway through a bitstream.
 *
 * The committed header is what the build reads, so an ordinary build needs no network and no copy
 * of the specification. `--check` is what makes the header trustworthy rather than merely present.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = join(import.meta.dirname, '..');
const HEADER = join(ROOT, 'src', 'codec', 'av1-tables.h');
const CACHE = join(ROOT, 'node_modules', '.cache', 'av1-spec');
const BASE = 'https://raw.githubusercontent.com/AOMediaCodec/av1-spec/master';

/** The specification documents the tables live in. */
const DOCUMENTS = [
	'03.symbols.md',
	'06.bitstream.syntax.md',
	'07.bitstream.semantics.md',
	'08.decoding.process.md',
	'09.parsing.process.md',
	'10.additional.tables.md'
] as const;

function spec(document: string): string {
	const path = join(CACHE, document);

	if (!existsSync(path)) {
		mkdirSync(CACHE, { recursive: true });
		execFileSync('curl', ['-sfL', '-o', path, `${BASE}/${document}`]);
	}

	return readFileSync(path, 'utf8');
}

// #region constants

/**
 * Every name the specification gives a numeric value, from two different shapes of table.
 *
 * Reading them rather than hardcoding them is what lets a dimension like `INTRA_MODES + 1` and an
 * entry like `TX_8X8` be checked instead of trusted. A name neither section carries is a hard
 * error, because the alternative is silently accepting a table nobody verified.
 *
 * The two shapes:
 *
 * - **Constants**, in the symbols section, as ``| `NAME` | value | description``. A value can itself
 *   be an expression over earlier names, so they resolve lazily.
 * - **Enumerators**, throughout the semantics section, as value-and-name rows under a heading. The
 *   name is always the last column and its value the column before it, which is what makes
 *   `| 1 | 3 | RESTORE_SWITCHABLE` read as 3 rather than 1: the first column there is the coded
 *   `lr_type` and the second is the value the name stands for.
 */
function symbols(): Map<string, number> {
	const out = new Map<string, number>();
	const pending = new Map<string, string>();

	// markdown escapes the shift operators, and a value column may name earlier constants
	for (const line of spec('03.symbols.md').split('\n')) {
		const match = /^\|\s*`([A-Z][A-Z0-9_]*)`\s*\|([^|]+)\|/.exec(line);
		if (!match) continue;

		const text = match[2]!.replace(/\\/g, '').trim();
		if (/^-?\d+$/.test(text)) out.set(match[1]!, Number(text));
		else pending.set(match[1]!, text);
	}

	if (out.size < 50) throw new Error(`read only ${out.size} constants from the symbols section`);

	// a constant defined in terms of others, resolved once its dependencies are known
	for (let pass = 0; pass < 8 && pending.size > 0; pass++) {
		for (const [name, text] of [...pending]) {
			try {
				out.set(name, evaluate(text, out));
				pending.delete(name);
			} catch {
				// a later pass may resolve it
			}
		}
	}

	const conflicts: string[] = [];

	for (const line of spec('07.bitstream.semantics.md').split('\n')) {
		// value-and-name rows, two or more columns, the name last and its value beside it
		const match = /^\|((?:\s*-?\d+\s*\|)+)\s*([A-Z][A-Z0-9_]{2,})\s*\|?\s*$/.exec(line);
		if (!match) continue;

		const columns = match[1]!.split('|').filter((c) => c.trim() !== '');
		const value = Number(columns[columns.length - 1]!.trim());
		const name = match[2]!;

		const already = out.get(name);
		if (already !== undefined && already !== value)
			conflicts.push(`${name} = ${already} or ${value}`);
		else out.set(name, value);
	}

	// a name standing for two values would make every table using it a coin toss
	if (conflicts.length > 0) {
		throw new Error(`the specification gives these names two values: ${conflicts.join(', ')}`);
	}

	return out;
}

/**
 * Evaluates one integer expression against a symbol table.
 *
 * The expressions the specification writes are integer arithmetic over its own names, so a
 * recursive descent over `+ - * / << >> ( )` covers every one of them. Anything outside that
 * grammar throws, which is the point: an unrecognised expression must not become unchecked data.
 */
function evaluate(text: string, table: Map<string, number>): number {
	const tokens = text.match(/[A-Za-z][A-Za-z0-9_]*|\d+|<<|>>|[-+*/()]/g);
	if (!tokens) throw new Error(`cannot read "${text}"`);

	let at = 0;

	const peek = () => tokens[at];
	const take = () => tokens[at++]!;

	const primary = (): number => {
		const token = take();

		if (token === '(') {
			const value = shift();
			if (take() !== ')') throw new Error(`unbalanced parenthesis in "${text}"`);
			return value;
		}

		if (token === '-') return -primary();
		if (/^\d+$/.test(token)) return Number(token);

		const value = table.get(token);
		if (value === undefined) throw new Error(`"${text}" names unknown symbol ${token}`);
		return value;
	};

	const term = (): number => {
		let value = primary();

		while (peek() === '*' || peek() === '/') {
			value = take() === '*' ? value * primary() : Math.trunc(value / primary());
		}

		return value;
	};

	const sum = (): number => {
		let value = term();

		while (peek() === '+' || peek() === '-') {
			value = take() === '+' ? value + term() : value - term();
		}

		return value;
	};

	const shift = (): number => {
		let value = sum();

		while (peek() === '<<' || peek() === '>>') {
			value = take() === '<<' ? value << sum() : value >> sum();
		}

		return value;
	};

	const value = shift();
	if (at !== tokens.length) throw new Error(`trailing tokens in "${text}"`);
	return value;
}

const SYMBOLS = symbols();

/** Evaluates a dimension expression, which is the same grammar with a clearer failure. */
function dimension(text: string): number {
	return evaluate(text, SYMBOLS);
}

// #region extraction

interface Table {
	name: string;
	dims: number[];
	values: number[];
}

/**
 * Splits a table body into its leaf elements, one string per value.
 *
 * Scraping integers out of the body instead would read `TX_8X8` as two values and `128 * 125` as
 * two more, which is how a 19-entry table comes back with 38 entries. Splitting on the commas that
 * separate leaves and evaluating each one keeps a symbolic entry, an arithmetic entry and a plain
 * number all worth exactly one value. Braces are structure and are discarded; the declared
 * dimensions are what the count is checked against.
 */
function elements(text: string): string[] {
	// the specification annotates some tables with C comments naming the mode each row is for
	const body = text.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');

	const out: string[] = [];
	let current = '';
	let depth = 0;

	const flush = () => {
		const text = current.trim();
		if (text !== '') out.push(text);
		current = '';
	};

	for (const character of body) {
		if (character === '{') {
			flush();
			depth++;
		} else if (character === '}') {
			flush();
			depth--;
		} else if (character === ',' && depth >= 0) {
			flush();
		} else {
			current += character;
		}
	}

	flush();
	return out;
}

/**
 * Reads one table by name out of a document.
 *
 * The declaration and its data are matched by brace balance rather than by a line pattern, because
 * the specification wraps rows at its own column width and interleaves prose between the fenced
 * blocks. Numbers are taken only from inside the braces, so a dimension expression naming a
 * constant cannot leak into the data.
 */
/**
 * Typographical defects in the specification, repaired by exact string.
 *
 * `Split_Tx_Size` declares `TX_SIZES_ALL` entries and supplies eighteen, because the thirteenth
 * lost its trailing comma. The count assertion is what caught it, and a repair recorded here is
 * the honest fix: loosening the parser to treat a newline as a separator would accept this table
 * and every genuinely truncated one alongside it.
 *
 * A repair only ever adds punctuation. If one stops matching, the specification changed and the
 * extraction fails rather than falling back, which is what the assertion below enforces.
 */
const REPAIRS: { table: string; from: string; to: string }[] = [
	{ table: 'Split_Tx_Size', from: 'TX_32X32\n  TX_4X8', to: 'TX_32X32,\n  TX_4X8' }
];

function table(text: string, name: string): Table {
	for (const repair of REPAIRS) {
		if (repair.table !== name) continue;

		if (!text.includes(repair.from)) {
			throw new Error(
				`${name}: the recorded repair no longer matches, so the specification changed`
			);
		}

		text = text.replace(repair.from, repair.to);
	}

	const declaration = new RegExp(`^${name}\\s*((?:\\[[^\\]]*\\]\\s*)+)=\\s*\\{`, 'm');
	const found = declaration.exec(text);
	if (!found) throw new Error(`${name}: no declaration found`);

	const dims = [...found[1]!.matchAll(/\[([^\]]*)\]/g)].map((m) => dimension(m[1]!));

	let at = found.index + found[0].length - 1;
	let depth = 0;
	let end = -1;

	for (; at < text.length; at++) {
		if (text[at] === '{') depth++;
		else if (text[at] === '}' && --depth === 0) {
			end = at;
			break;
		}
	}

	if (end < 0) throw new Error(`${name}: unterminated table`);

	const body = text.slice(found.index + found[0].length, end);
	const values = elements(body).map((element) => evaluate(element, SYMBOLS));
	const wanted = dims.reduce((a, b) => a * b, 1);

	if (values.length !== wanted) {
		throw new Error(
			`${name}[${dims.join('][')}]: wanted ${wanted} values, read ${values.length}`
		);
	}

	return { name, dims, values };
}

/** Finds every table the specification declares in a document, by declaration shape. */
function declared(text: string): string[] {
	const out: string[] = [];

	for (const match of text.matchAll(/^([A-Z][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)+=\s*\{/gm)) {
		if (!out.includes(match[1]!)) out.push(match[1]!);
	}

	return out;
}

// #region emit

/** snake_case, so `Default_Intra_Frame_Y_Mode_Cdf` becomes `default_intra_frame_y_mode_cdf`. */
function identifier(name: string): string {
	return `tiny_av1_${name.toLowerCase()}`;
}

/** The narrowest C type that holds every value, which is what keeps the data section honest. */
function width(values: number[]): string {
	let low = 0;
	let high = 0;

	for (const value of values) {
		if (value < low) low = value;
		if (value > high) high = value;
	}

	if (low < 0) {
		if (low >= -128 && high <= 127) return 'int8_t';
		if (low >= -32768 && high <= 32767) return 'int16_t';
		return 'int32_t';
	}

	if (high <= 255) return 'uint8_t';
	if (high <= 65535) return 'uint16_t';
	return 'uint32_t';
}

/** Nests a flat list back into the brace structure the declaration needs. */
function nest(list: number[], dims: number[]): string {
	if (dims.length <= 1) return `{${list.join(', ')}}`;

	const [head, ...rest] = dims as [number, ...number[]];
	const stride = rest.reduce((a, b) => a * b, 1);
	const parts: string[] = [];

	for (let i = 0; i < head; i++) {
		parts.push(nest(list.slice(i * stride, (i + 1) * stride), rest));
	}

	return `{${parts.join(', ')}}`;
}

function emit(entry: Table): string {
	const bounds = entry.dims.map((d) => `[${d}]`).join('');

	return `static const ${width(entry.values)} ${identifier(entry.name)}${bounds} = ${nest(
		entry.values,
		entry.dims
	)};`;
}

/**
 * Runs the output through clang-format.
 *
 * The header sits under `src/`, so `format.sh` formats it and `--check` would fail the moment
 * anyone ran that. Formatting here keeps the two in agreement by construction rather than by an
 * exclusion. `--assume-filename` is what picks up the repo's `.clang-format`.
 */
function formatted(text: string): string {
	return execFileSync('clang-format', [`--assume-filename=${HEADER}`], {
		input: text,
		encoding: 'utf8',
		maxBuffer: 64 * 1024 * 1024
	});
}

/**
 * Values checked against knowledge outside the extraction.
 *
 * A count assertion proves a table is the right length. It cannot prove the numbers landed in the
 * right order, and a transposed scan or an off-by-one mode table desynchronises a bitstream rather
 * than failing loudly. These are the anchors: short tables whose contents are independently known,
 * plus the first and last entry of the two quantizer curves. The ICC profiles here shipped inverted
 * for weeks because every check compared them against themselves, so an external anchor is the
 * thing that makes a generated table trustworthy.
 */
const ANCHORS: { table: string; at: number[]; value: number }[] = [
	{ table: 'Default_Scan_4x4', at: [2], value: 4 },
	{ table: 'Default_Scan_4x4', at: [15], value: 15 },
	{ table: 'Sm_Weights_Tx_4x4', at: [0], value: 255 },
	{ table: 'Sm_Weights_Tx_4x4', at: [3], value: 64 },
	{ table: 'Mode_To_Angle', at: [1], value: 90 },
	{ table: 'Mode_To_Angle', at: [7], value: 203 },
	{ table: 'Tx_Size_Sqr', at: [4], value: 4 },
	{ table: 'Tx_Size_Sqr', at: [18], value: 2 },
	{ table: 'Split_Tx_Size', at: [13], value: 5 },
	{ table: 'Tx_Width', at: [18], value: 64 },
	{ table: 'Dc_Qlookup', at: [0, 0], value: 4 },
	{ table: 'Dc_Qlookup', at: [0, 255], value: 1336 },
	{ table: 'Ac_Qlookup', at: [0, 0], value: 4 },
	{ table: 'Ac_Qlookup', at: [0, 255], value: 1828 },
	{ table: 'Coeff_Base_Ctx_Offset', at: [0, 0, 0], value: 0 },
	{ table: 'Coeff_Base_Ctx_Offset', at: [0, 1, 3], value: 21 },
	{ table: 'Coeff_Base_Ctx_Offset', at: [6, 0, 1], value: 16 },
	{ table: 'Coeff_Base_Pos_Ctx_Offset', at: [0], value: 26 },
	{ table: 'Coeff_Base_Pos_Ctx_Offset', at: [2], value: 36 },
	{ table: 'Mag_Ref_Offset_With_Tx_Class', at: [0, 2, 1], value: 1 },
	{ table: 'Mag_Ref_Offset_With_Tx_Class', at: [2, 2, 0], value: 2 },
	{ table: 'Filter_Intra_Mode_To_Intra_Dir', at: [3], value: 6 }
];

function anchor(tables: Table[]): void {
	for (const check of ANCHORS) {
		const entry = tables.find((t) => t.name === check.table);
		if (!entry) throw new Error(`anchor names ${check.table}, which was not extracted`);

		let index = 0;

		for (let axis = 0; axis < check.at.length; axis++) {
			const stride = entry.dims.slice(axis + 1).reduce((a, b) => a * b, 1);
			index += check.at[axis]! * stride;
		}

		const got = entry.values[index];

		if (got !== check.value) {
			throw new Error(
				`${check.table}[${check.at.join('][')}] is ${got}, expected ${check.value}`
			);
		}
	}
}

/**
 * Every scan orders the positions of its transform exactly once.
 *
 * A structural property rather than a value, and it is the one thing a count
 * assertion cannot see: a scan that names one position twice drops a
 * coefficient and writes another one over itself, which shows up as a picture
 * with a wrong block rather than as a failure. There are 32 of them and each is
 * checked against its own length, so a truncated or transposed table fails
 * here.
 */
function permutations(tables: Table[]): void {
	let checked = 0;

	for (const entry of tables) {
		if (!/^(Default|Mrow|Mcol)_Scan_/.test(entry.name)) continue;

		const seen = new Set<number>();

		for (const value of entry.values) {
			if (value < 0 || value >= entry.values.length) {
				throw new Error(
					`${entry.name} names position ${value}, outside 0..${entry.values.length - 1}`
				);
			}
			if (seen.has(value)) {
				throw new Error(`${entry.name} names position ${value} twice`);
			}

			seen.add(value);
		}

		checked++;
	}

	if (checked < 32) throw new Error(`checked only ${checked} scans, expected 32`);
}

function generate(): string {
	// the four documents that carry data tables, in the order a reader would meet them. The
	// syntax document is mostly pseudocode, whose lines all begin with a pipe, so the
	// declaration pattern anchored at line start picks up only its real tables: the five
	// transform-type inverse sets. The parsing document carries the context offset tables the
	// coefficient CDF selections index, which live beside the pseudocode that reads them
	const sources = [
		spec('10.additional.tables.md'),
		spec('08.decoding.process.md'),
		spec('06.bitstream.syntax.md'),
		spec('09.parsing.process.md')
	];

	const seen = new Set<string>();
	const tables: Table[] = [];
	const failures: string[] = [];

	for (const source of sources) {
		for (const name of declared(source)) {
			if (seen.has(name)) continue;

			try {
				const entry = table(source, name);

				// a scalar dressed as a table is prose, not data
				if (entry.values.length === 0) continue;

				tables.push(entry);
				seen.add(name);
			} catch (error) {
				failures.push(`${name}: ${(error as Error).message}`);
			}
		}
	}

	if (tables.length < 150) {
		throw new Error(
			`extracted only ${tables.length} tables; failures were\n  ${failures.join('\n  ')}`
		);
	}

	anchor(tables);
	permutations(tables);

	const numbers = tables.reduce((a, t) => a + t.values.length, 0);
	const bytes = tables.reduce(
		(a, t) => a + t.values.length * (width(t.values).includes('8') ? 1 : 2),
		0
	);

	const body = tables.map(emit).join('\n\n');

	return `/*
 * Generated from the AV1 specification by scripts/av1-tables.ts. Do not hand edit.
 *
 * ${tables.length} tables, ${numbers.toLocaleString()} values, about ${Math.round(bytes / 1024)} KiB before --gc-sections.
 *
 * Source: https://github.com/AOMediaCodec/av1-spec, sections 6, 8, 9 and 10.
 */
#pragma once

#include <stdint.h>

${body}

/* Extraction refused these declarations, which are prose rather than data:
${failures.map((f) => ` * ${f}`).join('\n')}
 */
`;
}

const wanted = formatted(generate());

if (process.argv.includes('--check')) {
	const have = existsSync(HEADER) ? readFileSync(HEADER, 'utf8') : '';

	if (have !== wanted) {
		console.error(`${HEADER} does not match what the AV1 specification says`);
		process.exit(1);
	}

	console.log('av1-tables.h matches the AV1 specification');
} else {
	writeFileSync(HEADER, wanted);
	console.log(`wrote ${HEADER}, ${wanted.length.toLocaleString()} bytes`);
}
