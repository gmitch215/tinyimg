/**
 * Derives extra GIF and TIFF calibration entries from `cal` corpus JPEG and PNG files.
 *
 * ```sh
 * bun scripts/corpus-derive.ts derive    # write corpus/derived/ and scripts/corpus-derived.json
 * bun scripts/corpus-derive.ts check     # verify the files against scripts/corpus-derived.json
 * ```
 *
 * The cost model has too few real GIF and TIFF files to fit or to bound, so this fills them in
 * with `magick` and `gifsicle` from sources that are already `cal`. The record is a separate file
 * from `scripts/corpus-manifest.json`, which other items hash. A derived entry inherits its
 * source's split, and the fit half of the model inherits the source's `fit` flag, so no source has
 * one derivative on each side of the fit and conformal halves. Nothing here opens a `test` entry.
 */

import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import type { Entry } from './corpus.ts';
import { parseHeader, sha256Of } from './corpus.ts';

const ROOT = join(import.meta.dirname, '..');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const DERIVED = join(ROOT, 'scripts', 'corpus-derived.json');
const OUT = join(ROOT, 'corpus', 'derived');
const WIDTHS = [256, 800, 1600];
const SOURCES_PER_HALF = 6;

export type DerivedEntry = {
	id: string;
	derivedFrom: string;
	variant: string;
	commands: string[][];
	tools: Record<string, string>;
	sha256: string;
	bytes: number;
	width: number;
	height: number;
	format: 'gif' | 'tiff';
	split: 'cal';
	fitSource: boolean;
};

const fitOf = (sha256: string): boolean => parseInt(sha256.slice(8, 10), 16) % 2 === 0;

/** GIF variant `v` in 0..11: palette 256/64/16, dither on or off, interlaced or not. */
function gifVariant(v: number) {
	const colors = [256, 64, 16][v % 3]!;
	const dither = Math.floor(v / 3) % 2 === 0;
	const interlace = v >= 6;
	return {
		name: `colors${colors}-${dither ? 'dither' : 'nodither'}-${interlace ? 'interlaced' : 'sequential'}`,
		colors,
		dither,
		interlace
	};
}

/** The TIFF codings the decoder reads: none, PackBits, LZW and Deflate, with and without the predictor. */
const TIFF_CODING = [
	{ name: 'none', args: ['-compress', 'None'] },
	{ name: 'packbits', args: ['-compress', 'RLE'] },
	{ name: 'lzw', args: ['-compress', 'LZW'] },
	{ name: 'lzw-predictor', args: ['-compress', 'LZW', '-define', 'tiff:predictor=2'] },
	{ name: 'deflate', args: ['-compress', 'Zip'] },
	{ name: 'deflate-predictor', args: ['-compress', 'Zip', '-define', 'tiff:predictor=2'] }
];
const TIFF_STRIPS = ['default', 'one-row', 'whole'] as const;
const TIFF_COLOR = [
	{ name: 'rgb', args: [] as string[] },
	{ name: 'gray', args: ['-colorspace', 'Gray', '-type', 'Grayscale'] },
	{ name: 'rgba-opaque', args: ['-alpha', 'set'] }
];

function tool(name: string, flag: string): string {
	return execFileSync(name, [flag], { encoding: 'utf8' }).split('\n')[0]!;
}

function loadManifest(): Entry[] {
	return (JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] }).entries;
}

/** The first six fit and six conf `cal` JPEG or PNG files at least 1600 wide, in sha256 order. */
export function pickSources(entries: Entry[]): Entry[] {
	const pool = entries
		.filter(
			(e) =>
				e.split === 'cal' &&
				(e.format === 'jpeg' || e.format === 'png') &&
				e.width >= WIDTHS[2]! &&
				!e.derivedFrom
		)
		.sort((a, b) => (a.sha256 < b.sha256 ? -1 : 1));
	const fit = pool.filter((e) => fitOf(e.sha256)).slice(0, SOURCES_PER_HALF);
	const conf = pool.filter((e) => !fitOf(e.sha256)).slice(0, SOURCES_PER_HALF);

	return [...fit, ...conf].sort((a, b) => (a.sha256 < b.sha256 ? -1 : 1));
}

function sourceFile(e: Entry): string {
	const ext = e.format === 'jpeg' ? 'jpg' : e.format;
	return e.source === 'local' ? join(ROOT, e.path ?? '') : join(ROOT, 'corpus', `${e.id}.${ext}`);
}

const flatten = ['-background', 'white', '-alpha', 'remove', '-alpha', 'off'];

function plan(format: 'gif' | 'tiff', source: Entry, si: number, zi: number) {
	const width = WIDTHS[zi]!;
	const ext = format === 'gif' ? 'gif' : 'tif';
	const base = ['magick', '{in}', ...flatten, '-resize', `${width}x`];

	if (format === 'gif') {
		const v = gifVariant((si + 4 * zi) % 12);
		const commands = [
			[
				...base,
				'-dither',
				v.dither ? 'FloydSteinberg' : 'None',
				'-colors',
				String(v.colors),
				'GIF:{tmp}'
			],
			['gifsicle', v.interlace ? '--interlace' : '--no-interlace', '{tmp}', '-o', '{out}']
		];
		return { variant: v.name, commands, ext, width };
	}

	const coding = TIFF_CODING[(si + 5 * zi) % TIFF_CODING.length]!;
	const strips = TIFF_STRIPS[(si + zi) % TIFF_STRIPS.length]!;
	const color = TIFF_COLOR[(si + 2 * zi) % TIFF_COLOR.length]!;
	const stripArgs =
		strips === 'default'
			? []
			: ['-define', `tiff:rows-per-strip=${strips === 'one-row' ? 1 : 100000}`];

	return {
		variant: `${coding.name}-${strips}-${color.name}`,
		commands: [[...base, ...color.args, ...coding.args, ...stripArgs, 'TIFF:{out}']],
		ext,
		width
	};
}

function run(commands: string[][], input: string, out: string): void {
	const tmp = `${out}.tmp.gif`;

	for (const command of commands) {
		const [exe, ...args] = command.map((a) =>
			a.replaceAll('{in}', input).replaceAll('{tmp}', tmp).replaceAll('{out}', out)
		);
		execFileSync(exe!, args, { stdio: 'pipe' });
	}

	rmSync(tmp, { force: true });
}

function derive(): number {
	const entries = loadManifest();
	const sources = pickSources(entries);
	const tools = { magick: tool('magick', '-version'), gifsicle: tool('gifsicle', '--version') };
	mkdirSync(OUT, { recursive: true });
	const out: DerivedEntry[] = [];

	for (const format of ['gif', 'tiff'] as const) {
		sources.forEach((source, si) => {
			WIDTHS.forEach((_, zi) => {
				const p = plan(format, source, si, zi);
				const id = `derived-${format}-${source.id}-w${p.width}`;
				const file = join(OUT, `${id}.${p.ext}`);
				const part = `${file}.part.${p.ext}`;
				rmSync(part, { force: true });
				run(p.commands, sourceFile(source), part);
				const bytes = new Uint8Array(readFileSync(part));
				const h = parseHeader(bytes);
				renameSync(part, file);

				out.push({
					id,
					derivedFrom: source.id,
					variant: p.variant,
					commands: p.commands,
					tools,
					sha256: sha256Of(bytes),
					bytes: bytes.length,
					width: h.width,
					height: h.height,
					format,
					split: 'cal',
					fitSource: fitOf(source.sha256)
				});
			});
		});
	}

	writeFileSync(DERIVED, `${JSON.stringify({ entries: out }, null, '\t')}\n`);
	console.log(`derive: ${out.length} entries written to ${DERIVED}`);
	return 0;
}

function check(): number {
	const { entries } = JSON.parse(readFileSync(DERIVED, 'utf8')) as { entries: DerivedEntry[] };
	const manifest = loadManifest();
	const problems: string[] = [];

	for (const e of entries) {
		const source = manifest.find((m) => m.id === e.derivedFrom);
		const ext = e.format === 'gif' ? 'gif' : 'tif';
		const file = join(OUT, `${e.id}.${ext}`);

		if (!source || source.split !== 'cal') problems.push(`${e.id}: source is not a cal entry`);
		else if (fitOf(source.sha256) !== e.fitSource) problems.push(`${e.id}: fitSource differs`);

		if (!existsSync(file)) {
			problems.push(`${e.id}: missing ${file}`);
			continue;
		}

		const bytes = new Uint8Array(readFileSync(file));
		if (sha256Of(bytes) !== e.sha256 || bytes.length !== e.bytes) {
			problems.push(`${e.id}: bytes or sha256 differ from the record`);
		}

		const h = parseHeader(bytes);
		if (h.format !== e.format || h.width !== e.width || h.height !== e.height) {
			problems.push(`${e.id}: header is ${h.format} ${h.width}x${h.height}`);
		}
	}

	for (const p of problems) console.error(p);
	console.log(`check: ${entries.length} derived entries, mismatches: ${problems.length}`);
	return problems.length === 0 ? 0 : 1;
}

if (import.meta.main) {
	const cmd = process.argv[2];
	if (cmd === 'derive') process.exitCode = derive();
	else if (cmd === 'check') process.exitCode = check();
	else {
		console.error('usage: bun scripts/corpus-derive.ts derive | check');
		process.exitCode = 2;
	}
}
