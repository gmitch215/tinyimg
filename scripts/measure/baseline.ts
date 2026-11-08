/**
 * Scores the planner's cost estimate against the clock on the held-out corpus, signed.
 *
 * ```sh
 * bun scripts/measure/baseline.ts                              # sweep, write bench/baselines/v1.1.1/
 * bun scripts/measure/baseline.ts --wasm bin/tinyimg.wasm --out bench/baselines/v1.2
 * bun scripts/measure/baseline.ts summarize [--out dir]        # rebuild summary.md from cells.json
 * ```
 *
 * Each cell is one whole request (open, plan, run, encode) on one corpus entry: a width, a budget
 * and an output format. The estimate is what `Image.decide()` reports, which covers decode and
 * resample and no encoder; the clock covers the encode too, so a cell with an expensive encoder reads
 * as an under-estimate. The error is the signed `log2(estimate / measured)`.
 *
 * Refuses to time under load and takes `/tmp/tinyimg-timing.lock` around every timed cell so two
 * timing lanes never overlap. Resumable: a rerun skips the cells already in `<out>/cells.jsonl`.
 */

import { createHash } from 'node:crypto';
import {
	appendFileSync,
	existsSync,
	mkdirSync,
	readFileSync,
	rmdirSync,
	writeFileSync
} from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { join, resolve } from 'node:path';
import type { ImageFormat } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';

export const ROOT = join(import.meta.dirname, '..', '..');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const CORPUS = join(ROOT, 'corpus');
export const LOCK = '/tmp/tinyimg-timing.lock';

export const WIDTHS = [200, 400, 800] as const;
export const BUDGETS = [0, 10] as const;
export const WARMUPS = 2;
export const RUNS = 9;
export const OVER = 2.0;
export const UNDER = 0.6;

export type Status = 'ok' | 'failed' | 'skipped';

export type Cell = {
	id: string;
	split: 'cal' | 'test';
	format: string;
	local: boolean;
	source_bytes: number;
	source_mpx: number;
	width: number;
	budget: number;
	output: string;
	status: Status;
	reason: string | null;
	scale: number | null;
	degraded: string[];
	alpha: boolean;
	estimate_us: number | null;
	measured_us: number | null;
	samples_us: number[];
};

export type Meta = {
	machine: string;
	cores: number;
	wasm: string;
	wasm_sha256: string;
	node: string;
	load_before: number;
	load_after: number | null;
	runs: number;
	warmups: number;
	cuts: string[];
};

export type CellsFile = { meta: Meta; cells: Cell[] };

export type Entry = {
	id: string;
	source?: 'local';
	path?: string;
	bytes: number;
	width: number;
	height: number;
	format: string;
	split: 'cal' | 'test';
};

// #region statistics

export function quantile(sorted: number[], p: number): number {
	return sorted[Math.round(p * (sorted.length - 1))]!;
}

export type Stats = {
	count: number;
	median: number;
	p10: number;
	p90: number;
	worstOver: number;
	worstUnder: number;
	inBand: number;
};

/** The signed log2 error of one cell, or null when the cell has no usable pair. */
export function signedError(cell: Cell): number | null {
	if (cell.status !== 'ok') return null;
	if (!cell.estimate_us || !cell.measured_us) return null;
	return Math.log2(cell.estimate_us / cell.measured_us);
}

export function statsOf(cells: Cell[]): Stats | null {
	const errors = cells.map(signedError).filter((e): e is number => e !== null);
	if (errors.length === 0) return null;

	const sorted = [...errors].sort((a, b) => a - b);
	const lo = Math.log2(UNDER);
	const hi = Math.log2(OVER);

	return {
		count: sorted.length,
		median: quantile(sorted, 0.5),
		p10: quantile(sorted, 0.1),
		p90: quantile(sorted, 0.9),
		worstOver: sorted[sorted.length - 1]!,
		worstUnder: sorted[0]!,
		inBand: sorted.filter((e) => e >= lo && e <= hi).length / sorted.length
	};
}

// #endregion

// #region summary

const ratio = (log2: number) => 2 ** log2;
const fmtLog = (v: number) => v.toFixed(2);
const fmtRatio = (v: number) => (v >= 1 ? `${v.toFixed(2)}x over` : `${(1 / v).toFixed(2)}x under`);

function statsRow(label: string, cells: Cell[]): string {
	const s = statsOf(cells);
	if (!s) return `| ${label} | 0 | | | | | | |`;
	return (
		`| ${label} | ${s.count} | ${fmtLog(s.median)} | ${fmtLog(s.p10)} | ${fmtLog(s.p90)} | ` +
		`${fmtLog(s.worstOver)} (${fmtRatio(ratio(s.worstOver))}) | ` +
		`${fmtLog(s.worstUnder)} (${fmtRatio(ratio(s.worstUnder))}) | ${(s.inBand * 100).toFixed(1)}% |`
	);
}

const HEADER =
	'| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |\n' +
	'| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |';

function table(title: string, cells: Cell[], key: (c: Cell) => string, order?: string[]): string {
	const groups = new Map<string, Cell[]>();
	for (const c of cells) {
		const k = key(c);
		if (!groups.has(k)) groups.set(k, []);
		groups.get(k)!.push(c);
	}
	const keys = order ? order.filter((k) => groups.has(k)) : [...groups.keys()].sort();
	return [
		`### ${title}`,
		'',
		HEADER,
		statsRow('all', cells),
		...keys.map((k) => statsRow(k, groups.get(k)!)),
		''
	].join('\n');
}

const FORMAT_ORDER = ['jpeg', 'png', 'webp', 'gif', 'tiff', 'bmp', 'avif'];

function worstCell(cells: Cell[]): { cell: Cell; err: number; ratio: number; dir: string } | null {
	let best: { cell: Cell; err: number } | null = null;
	for (const cell of cells) {
		const err = signedError(cell);
		if (err === null) continue;
		if (!best || Math.abs(err) > Math.abs(best.err)) best = { cell, err };
	}
	if (!best) return null;
	const r = ratio(Math.abs(best.err));
	return { ...best, ratio: r, dir: best.err > 0 ? 'over-estimate' : 'under-estimate' };
}

function describeWorst(label: string, cells: Cell[]): string {
	const w = worstCell(cells);
	if (!w) return `- ${label}: no cells.`;
	const c = w.cell;
	return (
		`- ${label}: ${w.ratio.toFixed(2)}x ${w.dir} (log2 ${fmtLog(w.err)}), cell ` +
		`\`${c.id}\` ${c.format} ${c.source_mpx.toFixed(2)} Mpx, width ${c.width}, budget ${c.budget}, ` +
		`output ${c.output}, scale 1/${c.scale}, estimate ${(c.estimate_us! / 1000).toFixed(2)} ms, ` +
		`measured ${(c.measured_us! / 1000).toFixed(2)} ms.`
	);
}

function worstDirections(label: string, cells: Cell[]): string {
	const s = statsOf(cells);
	if (!s) return `- ${label}: no cells.`;
	return (
		`- ${label}: worst over-estimate ${ratio(s.worstOver).toFixed(2)}x, worst under-estimate ` +
		`${(1 / ratio(s.worstUnder)).toFixed(2)}x.`
	);
}

/** Builds summary.md from cells.json alone, so a spec can recompute it. */
export function summaryMarkdown(file: CellsFile): string {
	const { meta, cells } = file;
	const ok = cells.filter((c) => c.status === 'ok');
	const failed = cells.filter((c) => c.status === 'failed');
	const skipped = cells.filter((c) => c.status === 'skipped');
	const zero = ok.filter((c) => !c.estimate_us);
	const alpha = ok.filter((c) => c.alpha);
	const usable = ok.filter((c) => !c.alpha);
	const test = usable.filter((c) => c.split === 'test');
	const cal = usable.filter((c) => c.split === 'cal');
	const fresh = ok.filter((c) => c.local);
	const freshHeadline = fresh.filter((c) => !c.alpha);

	const out: string[] = [];
	out.push(`# Estimate against the clock, ${meta.wasm}`, '');
	out.push(
		`Machine ${meta.machine} (${meta.cores} cores), node ${meta.node}, wasm sha256 ${meta.wasm_sha256}, ` +
			`load average ${meta.load_before.toFixed(2)} before` +
			(meta.load_after === null ? '' : ` and ${meta.load_after.toFixed(2)} after`) +
			`, ${meta.warmups} warmups and ${meta.runs} timed runs per cell, median reported.`,
		''
	);
	out.push(
		'Error is `log2(estimate / measured)`: 0 is exact, +1 is a 2x over-estimate, -1 is a 2x ' +
			'under-estimate. The band estimate.ts asserts, 0.6x to 2.0x, is -0.74 to +1.00. The estimate is ' +
			'`Image.decide().estimateMs` (decode and resample, no encoder); the clock is the whole request ' +
			'including the encode. The `worst over` and `worst under` columns give the log2 value and the ratio.',
		''
	);
	out.push('## Cells', '');
	out.push(
		`${cells.length} cells: ${ok.length} timed, ${failed.length} failed, ${skipped.length} skipped. ` +
			`${zero.length} timed cells have a zero estimate (excluded from every table). ` +
			`${alpha.length} timed cells are flagged \`alpha\` (webp output from a 4-channel plan; the ` +
			'model predates the ALPH encode) and are listed apart from the headline.',
		''
	);
	if (meta.cuts.length) out.push(`Cuts: ${meta.cuts.join('; ')}.`, '');

	if (failed.length) {
		const byReason = new Map<string, number>();
		for (const c of failed)
			byReason.set(c.reason ?? '', (byReason.get(c.reason ?? '') ?? 0) + 1);
		out.push(`Failed cells by error (${new Set(failed.map((c) => c.id)).size} images):`, '');
		for (const [r, n] of [...byReason].sort()) out.push(`- ${n} cells: ${r}`);
		out.push('');
		out.push('Failed images:', '');
		const seen = new Map<string, Cell>();
		for (const c of failed) if (!seen.has(c.id)) seen.set(c.id, c);
		for (const c of seen.values())
			out.push(`- \`${c.id}\` ${c.format} ${c.source_mpx.toFixed(2)} Mpx: ${c.reason}`);
		out.push('');
	}
	if (skipped.length) {
		const byReason = new Map<string, number>();
		for (const c of skipped)
			byReason.set(c.reason ?? '', (byReason.get(c.reason ?? '') ?? 0) + 1);
		out.push('Skipped cells:', '');
		for (const [r, n] of [...byReason].sort()) out.push(`- ${n} cells: ${r}`);
		out.push('');
	}

	out.push('## The 12.8x question', '');
	out.push(describeWorst('test (headline, alpha excluded)', test));
	out.push(describeWorst('cal (alpha excluded)', cal));
	out.push(describeWorst('alpha cells', alpha));
	out.push(describeWorst('the five local images (alpha excluded)', freshHeadline));
	out.push(
		'- the 12 JPEGs README.md:169 describes: the corpus manifest carries no `legacy` marker, so they cannot be identified and are not reported apart.'
	);
	out.push('');
	out.push(worstDirections('test', test));
	out.push(worstDirections('cal', cal), '');

	out.push('## Headline: test split, alpha cells excluded', '');
	out.push(table('by source format', test, (c) => c.format, FORMAT_ORDER));
	out.push(
		table('by scale denominator', test, (c) => `1/${c.scale}`, ['1/1', '1/2', '1/4', '1/8'])
	);
	out.push(
		table('by budget', test, (c) => (c.budget ? `${c.budget} ms` : 'none'), ['none', '10 ms'])
	);
	out.push(table('by output format', test, (c) => c.output, ['source', 'jpeg', 'webp']));
	out.push(table('by width', test, (c) => String(c.width), ['200', '400', '800']));

	out.push('## Calibration split, alpha cells excluded', '');
	out.push(table('by source format', cal, (c) => c.format, FORMAT_ORDER));
	out.push(
		table('by scale denominator', cal, (c) => `1/${c.scale}`, ['1/1', '1/2', '1/4', '1/8'])
	);
	out.push(
		table('by budget', cal, (c) => (c.budget ? `${c.budget} ms` : 'none'), ['none', '10 ms'])
	);

	out.push('## The five local images', '');
	out.push(
		"Gregory's images, never part of the old calibration; held-out cells. Alpha cells excluded here and listed below.",
		''
	);
	out.push(table('per image', freshHeadline, (c) => c.id));

	out.push('## Alpha cells', '');
	out.push(
		'Webp output where the plan carries 4 channels. The old model prices no ALPH encode, so these are expected to under-estimate; kept out of the headline.',
		''
	);
	out.push(table('by source format', alpha, (c) => c.format, FORMAT_ORDER));

	out.push('## How to reproduce', '');
	out.push(
		'```sh\nbun scripts/corpus.ts check\nbun scripts/measure/baseline.ts --wasm bench/arms/tinyimg-v1.1.1.wasm --out bench/baselines/v1.1.1\nbun scripts/measure/baseline.ts summarize --out bench/baselines/v1.1.1\n```',
		'',
		'A sweep takes the timing lock, waits for the load average to fall below a quarter of the core count and appends each cell to `cells.jsonl` in the output directory (a working file, not committed), so an interrupted run resumes. The sweep writes `cells.json` and `summary.md` when it finishes, and `summarize` rewrites `summary.md` from `cells.json`. Compare another build by pointing `--wasm` at it and `--out` at a new directory.',
		''
	);
	return out.join('\n');
}

// #endregion

// #region sweep

export function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function waitForQuiet(maxWaitS = 3600): Promise<number> {
	const limit = cpus().length / 4;
	for (let waited = 0; ; waited += 5) {
		const load = loadavg()[0]!;
		if (load <= limit) return load;
		if (waited >= maxWaitS) {
			console.error(
				`load average ${load.toFixed(2)} stayed above ${limit.toFixed(2)} for ${Math.round(maxWaitS / 60)} minutes`
			);
			process.exit(2);
		}
		await sleep(5000);
	}
}

export async function withLock<T>(body: () => Promise<T>): Promise<T> {
	for (let waited = 0; ; waited += 2) {
		try {
			mkdirSync(LOCK);
			break;
		} catch (error) {
			if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
			if (waited >= 1800) {
				console.error('timing lock held for 30 minutes');
				process.exit(2);
			}
			await sleep(2000);
		}
	}
	try {
		return await body();
	} finally {
		rmdirSync(LOCK);
	}
}

export function median(values: number[]): number {
	return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]!;
}

export function fileOf(e: Entry): string {
	if (e.source === 'local') return join(ROOT, e.path ?? '');
	const ext = e.format === 'jpeg' ? 'jpg' : e.format === 'tiff' ? 'tif' : e.format;
	return join(CORPUS, `${e.id}.${ext}`);
}

const cellKey = (c: Pick<Cell, 'id' | 'width' | 'budget' | 'output'>) =>
	`${c.id}|${c.width}|${c.budget}|${c.output}`;

export function errorText(error: unknown): string {
	const e = error as Error;
	return `${e.name}: ${e.message}`.replace(/\s+/g, ' ');
}

export function arg(name: string): string | undefined {
	const i = process.argv.indexOf(name);
	return i >= 0 ? process.argv[i + 1] : undefined;
}

function writeOutputs(dir: string, meta: Meta, cells: Cell[]) {
	const file: CellsFile = { meta, cells };
	writeFileSync(join(dir, 'cells.json'), JSON.stringify(file) + '\n');
	writeFileSync(join(dir, 'summary.md'), summaryMarkdown(file));
}

async function sweep() {
	const wasmPath = resolve(ROOT, arg('--wasm') ?? 'bench/arms/tinyimg-v1.1.1.wasm');
	const outDir = resolve(ROOT, arg('--out') ?? 'bench/baselines/v1.1.1');
	const only = arg('--only');
	const maxMpx800 = arg('--cut-w800-above-mpx');
	const maxMpxWebp = arg('--cut-webp-above-mpx');
	const wasmBytes = readFileSync(wasmPath);
	const wasmSha = createHash('sha256').update(wasmBytes).digest('hex');
	const compiled = await WebAssembly.compile(wasmBytes);
	let tinyimg = await TinyImgModule.load(compiled);

	mkdirSync(outDir, { recursive: true });
	const jsonl = join(outDir, 'cells.jsonl');
	const done = new Map<string, Cell>();
	if (existsSync(jsonl))
		for (const line of readFileSync(jsonl, 'utf8').split('\n'))
			if (line.trim()) {
				const c = JSON.parse(line) as Cell;
				done.set(cellKey(c), c);
			}

	const load0 = await waitForQuiet();
	const machine = `${platform()}-${arch()}, ${cpus()[0]?.model ?? 'unknown cpu'}`;
	console.log(`${machine}, ${cpus().length} cores, node ${process.version}`);
	console.log(`wasm ${wasmPath} sha256 ${wasmSha}`);
	console.log(`load ${load0.toFixed(2)}, resuming ${done.size} cells`);

	const cuts: string[] = [];
	if (maxMpx800) cuts.push(`width 800 skipped above ${maxMpx800} Mpx`);
	if (maxMpxWebp) cuts.push(`webp output skipped above ${maxMpxWebp} Mpx`);

	const entries = (
		JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] }
	).entries.filter((e) => !only || e.id.includes(only));
	const cells: Cell[] = [];
	let n = 0;

	for (const entry of entries) {
		const bytes = new Uint8Array(readFileSync(fileOf(entry)));
		sources.clear();
		sources.set(entry.id, bytes);
		const mpx = (entry.width * entry.height) / 1e6;
		const outputs = ['source', ...['jpeg', 'webp'].filter((f) => f !== entry.format)];

		for (const width of WIDTHS) {
			for (const budget of BUDGETS) {
				for (const output of outputs) {
					const base: Cell = {
						id: entry.id,
						split: entry.split,
						format: entry.format,
						local: entry.source === 'local',
						source_bytes: entry.bytes,
						source_mpx: mpx,
						width,
						budget,
						output,
						status: 'ok',
						reason: null,
						scale: null,
						degraded: [],
						alpha: false,
						estimate_us: null,
						measured_us: null,
						samples_us: []
					};
					const key = cellKey(base);
					const prior = done.get(key);
					if (prior) {
						cells.push(prior);
						continue;
					}

					const cell = await runCell(base);
					cells.push(cell);
					appendFileSync(jsonl, JSON.stringify(cell) + '\n');
					n++;
					if (n % 20 === 0) console.log(`${n} new cells, at ${entry.id}`);
				}
			}
		}
	}

	async function runCell(cell: Cell): Promise<Cell> {
		if (cell.width > entry_width(cell.id)) {
			return {
				...cell,
				status: 'skipped',
				reason: 'width exceeds source width (enlargement)'
			};
		}
		if (maxMpx800 && cell.width === 800 && cell.source_mpx > Number(maxMpx800))
			return { ...cell, status: 'skipped', reason: 'cut: width 800 above the Mpx limit' };
		if (maxMpxWebp && cell.output === 'webp' && cell.source_mpx > Number(maxMpxWebp))
			return { ...cell, status: 'skipped', reason: 'cut: webp output above the Mpx limit' };

		const format = (cell.output === 'source' ? cell.format : cell.output) as ImageFormat;
		const src = sources.get(cell.id)!;
		const request = async (decideOnly: boolean) => {
			using image = await Image.open(tinyimg, src);
			image.resize(cell.width, 0);
			if (cell.budget) image.budget(cell.budget);
			if (decideOnly) return image.decide();
			await image.bytes(format, { quality: 80 });
			return null;
		};

		try {
			const decided = (await request(true))!;
			cell.scale = decided.scale;
			cell.degraded = decided.degraded;
			cell.estimate_us = Math.round(decided.estimateMs * 1000);
			cell.alpha = format === 'webp' && decided.output.channels === 4;

			await waitForQuiet();
			const samples = await withLock(async () => {
				for (let i = 0; i < WARMUPS; i++) await request(false);
				const out: number[] = [];
				for (let i = 0; i < RUNS; i++) {
					const start = performance.now();
					await request(false);
					out.push((performance.now() - start) * 1000);
				}
				return out;
			});
			cell.samples_us = samples.map((s) => Math.round(s * 10) / 10);
			cell.measured_us = Math.round(median(samples) * 10) / 10;
			return cell;
		} catch (error) {
			tinyimg = await TinyImgModule.load(compiled);
			return {
				...cell,
				status: 'failed',
				reason: errorText(error),
				scale: null,
				estimate_us: null,
				measured_us: null,
				samples_us: []
			};
		}
	}

	function entry_width(id: string): number {
		return entries.find((e) => e.id === id)!.width;
	}

	const meta: Meta = {
		machine,
		cores: cpus().length,
		wasm: wasmPath.startsWith(ROOT) ? wasmPath.slice(ROOT.length + 1) : wasmPath,
		wasm_sha256: wasmSha,
		node: process.version,
		load_before: load0,
		load_after: loadavg()[0]!,
		runs: RUNS,
		warmups: WARMUPS,
		cuts
	};
	writeOutputs(outDir, meta, cells);
	console.log(`wrote ${cells.length} cells, load after ${meta.load_after!.toFixed(2)}`);
}

const sources = new Map<string, Uint8Array>();

// #endregion

if (import.meta.main) {
	if (process.argv[2] === 'summarize') {
		const outDir = resolve(ROOT, arg('--out') ?? 'bench/baselines/v1.1.1');
		const prior = JSON.parse(readFileSync(join(outDir, 'cells.json'), 'utf8')) as CellsFile;
		writeFileSync(join(outDir, 'summary.md'), summaryMarkdown(prior));
	} else {
		await sweep();
	}
}
