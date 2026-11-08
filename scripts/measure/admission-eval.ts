/**
 * A4: scores the admission bound on the held-out cells against the clock, under a gate written
 * before the run.
 *
 * ```sh
 * bun scripts/measure/admission-eval.ts run [--wasm f] [--out dir] [--expect-sha prefix]
 * bun scripts/measure/admission-eval.ts run --standin        # predictions only, v1.1.1 clocks
 * bun scripts/measure/admission-eval.ts analyze [--out dir]
 * bun scripts/measure/admission-eval.ts summarize [--out dir]
 * ```
 *
 * The cells are the `test` split of `bench/baselines/v1.1.1/cells.json` that timed, with no plan
 * budget set, so the budget B is the only constraint. Each is re-timed here on the module under
 * test: one whole request (open, resize, encode at quality 80). A budget B in 5, 10, 20, 50 and
 * 100 ms is then crossed with every cell. Three arms predict whether the request fits:
 *
 * - NEW is `Image.admit(B, { mode: 'strict', format })` on this module, the calibrated upper bound.
 * - FIXED is the v1.1.1 arm's `decide()` estimate plus `tiny_encode_cost`, admitted when at or under B.
 * - FIXED A is the v1.1.1 `decide()` estimate alone, for information.
 *
 * An overrun is a request an arm admitted whose measured time T exceeds B. A false reject is one
 * it rejected whose T is at or under B. The gate (pre-registered in RUN-PLAN-V12.md) is pooled over
 * every (cell, B) pair: NEW overrun rate at most 5 percent and FIXED overruns at least 2x NEW's.
 *
 * Refuses to time under load, takes `/tmp/tinyimg-timing.lock` around every timed block, and marks
 * a cell recorded above the load limit so the gate never reads it. A run with a subset, or on
 * stand-in clocks, is not the evaluation and its summary carries no verdict.
 */

import { createHash } from 'node:crypto';
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { join, resolve } from 'node:path';
import type { ImageFormat } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';
import { totalEstimateUs, type Dims } from './baseline-total.js';
import {
	arg,
	errorText,
	fileOf,
	median,
	ROOT,
	RUNS,
	waitForQuiet,
	WARMUPS,
	withLock,
	type Entry
} from './baseline.js';

export const BUDGETS_MS = [5, 10, 20, 50, 100] as const;
export const QUALITY = 80;
export const MAX_NEW_OVERRUN_RATE = 0.05;
export const MIN_OVERRUN_RATIO = 2;
export const CONTROL = { file: 'tests/fixtures/sf-24.jpg', width: 200, output: 'webp' } as const;

const FROZEN = join(ROOT, 'bench', 'arms', 'tinyimg-v1.1.1.wasm');
const BASELINE = join(ROOT, 'bench', 'baselines', 'v1.1.1', 'cells.json');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const DEFAULT_OUT = join(ROOT, 'bench', 'baselines', 'v1.2');

export type Status = 'ok' | 'failed' | 'skipped';
export type Decision = 'admit' | 'degraded' | 'reject';
export type ArmName = 'new' | 'fixed' | 'fixedA' | 'newQ80';

export type AdmCell = {
	id: string;
	split: 'cal' | 'test';
	format: string;
	source_mpx: number;
	width: number;
	output: string;
	status: Status;
	reason: string | null;
	alpha: boolean;
	measured_source: 'timed' | 'standin-v1.1.1';
	measured_us: number | null;
	samples_us: number[];
	load_before: number | null;
	load_after: number | null;
	load_excluded: boolean;
	fixed_a_us: number | null;
	fixed_b_us: number | null;
	new_cost_us: number | null;
	new_upper_us: number | null;
	new_decision: Record<string, Decision>;
	new_q80_upper_us: number | null;
	new_q80_decision: Record<string, Decision>;
};

export type Control = {
	label: string;
	measured_us: number;
	samples_us: number[];
	load_before: number;
	load_after: number;
};

export type AdmMeta = {
	machine: string;
	cores: number;
	load_limit: number;
	wasm: string;
	wasm_sha256: string;
	frozen_sha256: string;
	node: string;
	mode: 'timed' | 'standin';
	evaluation: boolean;
	subset: string | null;
	budgets_ms: number[];
	quality: number;
	runs: number;
	warmups: number;
	load_start: number;
	load_end: number | null;
};

export type AdmFile = { meta: AdmMeta; cells: AdmCell[]; controls: Control[] };

// #region gate

/** Whether a cell counts toward the gate. */
export function eligible(c: AdmCell): boolean {
	return (
		c.status === 'ok' &&
		c.split === 'test' &&
		!c.alpha &&
		!c.load_excluded &&
		c.measured_us !== null &&
		c.measured_us > 0 &&
		c.fixed_a_us !== null &&
		c.fixed_b_us !== null &&
		Object.keys(c.new_decision).length > 0
	);
}

/** Whether an arm admits a cell at a budget, in milliseconds. */
export function isAdmitted(c: AdmCell, arm: ArmName, budgetMs: number): boolean {
	switch (arm) {
		case 'new':
			return c.new_decision[String(budgetMs)] === 'admit';
		case 'newQ80':
			return c.new_q80_decision[String(budgetMs)] === 'admit';
		case 'fixed':
			return c.fixed_b_us! <= budgetMs * 1000;
		case 'fixedA':
			return c.fixed_a_us! <= budgetMs * 1000;
	}
}

export type Counts = {
	pairs: number;
	admitted: number;
	overruns: number;
	rejected: number;
	falseRejects: number;
	/** pairs whose measured time is at or under the budget */
	fit: number;
};

/** Counts the pairs of the eligible cells and the budgets for one arm. */
export function tally(
	cells: AdmCell[],
	arm: ArmName,
	budgets: readonly number[] = BUDGETS_MS
): Counts {
	const n: Counts = { pairs: 0, admitted: 0, overruns: 0, rejected: 0, falseRejects: 0, fit: 0 };

	for (const c of cells) {
		if (!eligible(c)) continue;
		for (const b of budgets) {
			const fits = c.measured_us! <= b * 1000;
			const admit = isAdmitted(c, arm, b);

			n.pairs++;
			if (fits) n.fit++;
			if (admit) {
				n.admitted++;
				if (!fits) n.overruns++;
			} else {
				n.rejected++;
				if (fits) n.falseRejects++;
			}
		}
	}

	return n;
}

/** `part / whole`, or null for an empty whole. */
export function rate(part: number, whole: number): number | null {
	return whole === 0 ? null : part / whole;
}

export const overrunRate = (n: Counts) => rate(n.overruns, n.admitted);
export const falseRejectRate = (n: Counts) => rate(n.falseRejects, n.fit);

export type Gate = {
	pairs: number;
	new: Counts;
	fixed: Counts;
	fixedA: Counts;
	newQ80: Counts;
	/** FIXED overruns over NEW overruns; Infinity when NEW has none, null when neither does */
	ratio: number | null;
	newRatePass: boolean;
	ratioPass: boolean;
	pass: boolean;
	reason: string;
};

/**
 * The pre-registered gate: NEW overrun rate at most 5 percent and FIXED overruns at least 2x NEW's,
 * pooled over every (cell, budget) pair of the eligible cells.
 *
 * Two edge cases the registration does not name. With no admitted request the rate is 0 of 0, and
 * that fails the gate because it is no evidence. With no overruns on either arm the ratio is 0 of
 * 0, and that fails because no request showed a difference. When NEW has none and FIXED has some
 * the ratio is infinite and passes.
 */
export function gate(cells: AdmCell[], budgets: readonly number[] = BUDGETS_MS): Gate {
	const fresh = tally(cells, 'new', budgets);
	const fixed = tally(cells, 'fixed', budgets);
	const fixedA = tally(cells, 'fixedA', budgets);
	const newQ80 = tally(cells, 'newQ80', budgets);
	const r = overrunRate(fresh);
	const ratio =
		fresh.overruns === 0
			? fixed.overruns === 0
				? null
				: Number.POSITIVE_INFINITY
			: fixed.overruns / fresh.overruns;
	const newRatePass = r !== null && r <= MAX_NEW_OVERRUN_RATE;
	const ratioPass = ratio !== null && ratio >= MIN_OVERRUN_RATIO;
	const reasons: string[] = [];

	if (fresh.pairs === 0) reasons.push('no eligible pairs');
	if (r === null) reasons.push('NEW admitted no request, so the overrun rate is undefined');
	else if (!newRatePass) reasons.push(`NEW overrun rate ${(100 * r).toFixed(2)}% is over 5%`);
	if (ratio === null) reasons.push('neither arm overran, so the ratio is undefined');
	else if (!ratioPass)
		reasons.push(`FIXED has ${ratio.toFixed(2)}x NEW's overruns, under the required 2x`);

	return {
		pairs: fresh.pairs,
		new: fresh,
		fixed,
		fixedA,
		newQ80,
		ratio,
		newRatePass,
		ratioPass,
		pass: fresh.pairs > 0 && newRatePass && ratioPass,
		reason: reasons.join('; ')
	};
}

/** The same counts per group, for the rows that are reported and not gated. */
export function breakdown(
	cells: AdmCell[],
	key: (c: AdmCell) => string
): Map<string, { new: Counts; fixed: Counts; fixedA: Counts }> {
	const groups = new Map<string, AdmCell[]>();
	for (const c of cells) {
		if (!eligible(c)) continue;
		const k = key(c);
		if (!groups.has(k)) groups.set(k, []);
		groups.get(k)!.push(c);
	}

	return new Map(
		[...groups]
			.sort(([a], [b]) => a.localeCompare(b, 'en', { numeric: true }))
			.map(([k, g]) => [
				k,
				{ new: tally(g, 'new'), fixed: tally(g, 'fixed'), fixedA: tally(g, 'fixedA') }
			])
	);
}

/** The resolved output container of a cell. */
export const outputFormat = (c: Pick<AdmCell, 'format' | 'output'>) =>
	c.output === 'source' ? c.format : c.output;

/** Largest control over the smallest, and last over first; null with fewer than two. */
export function controlSpread(controls: Control[]) {
	if (controls.length < 2) return null;
	const us = controls.map((c) => c.measured_us);
	return {
		spread: Math.max(...us) / Math.min(...us),
		firstToLast: us[us.length - 1]! / us[0]!
	};
}

// #endregion

// #region summary

const pct = (v: number | null) => (v === null ? 'n/a' : `${(100 * v).toFixed(2)}%`);
const ratioText = (v: number | null) =>
	v === null ? 'n/a' : Number.isFinite(v) ? `${v.toFixed(2)}x` : 'infinite';

function armRow(label: string, n: Counts): string {
	return (
		`| ${label} | ${n.pairs} | ${n.admitted} | ${n.overruns} | ${pct(overrunRate(n))} | ` +
		`${n.falseRejects} / ${n.fit} | ${pct(falseRejectRate(n))} |`
	);
}

const ARM_HEADER =
	'| arm | pairs | admitted | overruns | overrun rate | false rejects / fit | false-reject rate |\n' +
	'| --- | ---: | ---: | ---: | ---: | ---: | ---: |';

function groupTable(title: string, groups: ReturnType<typeof breakdown>): string[] {
	const out = [`### ${title}`, '', ARM_HEADER];
	for (const [k, g] of groups) {
		out.push(armRow(`${k}, NEW`, g.new), armRow(`${k}, FIXED`, g.fixed));
	}
	out.push('');
	return out;
}

/** Builds the summary from the cells alone, so a spec can recompute it. */
export function summaryMarkdown(file: AdmFile): string {
	const { meta, cells, controls } = file;
	const g = gate(cells, meta.budgets_ms);
	const used = cells.filter(eligible);
	const out: string[] = [];

	out.push('# A4: admission against the clock', '');
	if (!meta.evaluation) {
		out.push(
			meta.mode === 'standin'
				? '**Stand-in data, not the evaluation.** The predictions are computed live on the module below; ' +
						'the measured times are the v1.1.1 baseline clocks, which include the old encoder. ' +
						'This exercises the pipeline and says nothing about the gate. No verdict is printed.'
				: `**Subset run${meta.subset ? ` (${meta.subset})` : ''}, not the evaluation.** ` +
						'A dry run of the pipeline on a few cells. No verdict is printed.',
			''
		);
	}

	out.push(
		`Machine ${meta.machine} (${meta.cores} cores), node ${meta.node}. Module \`${meta.wasm}\` ` +
			`sha256 ${meta.wasm_sha256}. FIXED arm: v1.1.1 module sha256 ${meta.frozen_sha256}. ` +
			`Mode ${meta.mode}, load average ${meta.load_start.toFixed(2)} at start` +
			(meta.load_end === null ? '' : ` and ${meta.load_end.toFixed(2)} at end`) +
			`, load limit ${meta.load_limit.toFixed(2)}, ${meta.warmups} warmups and ${meta.runs} timed ` +
			`runs per cell, median is T, encode quality ${meta.quality}. Budgets ${meta.budgets_ms.join(', ')} ms, ` +
			'platformScale 1.',
		''
	);

	const by = (s: Status) => cells.filter((c) => c.status === s).length;
	out.push('## Cells', '');
	out.push(
		`${cells.length} cells: ${by('ok')} timed, ${by('failed')} failed, ${by('skipped')} skipped ` +
			`(alpha sources are skipped). ${cells.filter((c) => c.load_excluded).length} timed cells were ` +
			`recorded above the load limit and are excluded from the gate. ${used.length} cells are ` +
			`eligible, ${g.pairs} (cell, budget) pairs.`,
		''
	);

	out.push('## Gate', '');
	out.push(ARM_HEADER);
	out.push(armRow('NEW, strict admit', g.new));
	out.push(armRow('FIXED, v1.1.1 decide plus encoder', g.fixed));
	out.push(armRow('FIXED A, v1.1.1 decide alone (information)', g.fixedA));
	out.push(armRow(`NEW at quality ${meta.quality} (sensitivity)`, g.newQ80), '');
	out.push(
		`Overrun ratio FIXED over NEW: ${ratioText(g.ratio)}. NEW overrun rate ${pct(overrunRate(g.new))} ` +
			`against the 5% limit; ratio against the 2x minimum.`,
		''
	);

	if (meta.evaluation) {
		out.push(
			`**Gate: ${g.pass ? 'PASS' : 'FAIL'}.**${g.reason ? ` ${g.reason}.` : ''}`,
			'',
			'A failure closes the two-term upper-bound mechanism and goes under refuted approaches; the objective stays.',
			''
		);
	}

	out.push('## Rows reported, not gated', '');
	out.push(
		...groupTable(
			'by budget',
			new Map(
				meta.budgets_ms.map((b) => [
					`${b} ms`,
					{
						new: tally(cells, 'new', [b]),
						fixed: tally(cells, 'fixed', [b]),
						fixedA: tally(cells, 'fixedA', [b])
					}
				])
			)
		)
	);
	out.push(
		...groupTable(
			'by source format',
			breakdown(cells, (c) => c.format)
		)
	);
	out.push(...groupTable('by output format', breakdown(cells, outputFormat)));

	out.push(
		'Not covered by the test: alpha sources (excluded), lossy WebP with alpha and TIFF encode by ' +
			'compression, the two coverage misses A2c named (0.820 and 0.891).',
		''
	);

	out.push('## Control', '');
	if (controls.length === 0) {
		out.push('No timed control: this run used stand-in clocks.', '');
	} else {
		out.push(
			`\`${CONTROL.file}\` width ${CONTROL.width}, ${CONTROL.output} output, same request as a cell.`,
			'',
			'| label | ms | load before | load after |',
			'| --- | ---: | ---: | ---: |'
		);
		for (const c of controls)
			out.push(
				`| ${c.label} | ${(c.measured_us / 1000).toFixed(3)} | ${c.load_before.toFixed(2)} | ${c.load_after.toFixed(2)} |`
			);
		const s = controlSpread(controls);
		out.push(
			'',
			s
				? `Spread (max over min) ${s.spread.toFixed(3)}, last over first ${s.firstToLast.toFixed(3)}.`
				: 'Fewer than two controls.',
			''
		);
	}

	out.push('## How to reproduce', '');
	out.push(
		'```sh\nbun scripts/measure/admission-eval.ts run --wasm <module> --expect-sha <prefix>\nbun scripts/measure/admission-eval.ts summarize\n```',
		''
	);
	return out.join('\n');
}

// #endregion

// #region sweep

type Prior = {
	id: string;
	split: 'cal' | 'test';
	format: string;
	source_mpx: number;
	width: number;
	budget: number;
	output: string;
	status: string;
	alpha: boolean;
	estimate_us: number | null;
	measured_us: number | null;
	samples_us: number[];
};

const cellKey = (c: Pick<AdmCell, 'id' | 'width' | 'output'>) => `${c.id}|${c.width}|${c.output}`;

function emptyCell(p: Prior): AdmCell {
	return {
		id: p.id,
		split: p.split,
		format: p.format,
		source_mpx: p.source_mpx,
		width: p.width,
		output: p.output,
		status: 'ok',
		reason: null,
		alpha: false,
		measured_source: 'timed',
		measured_us: null,
		samples_us: [],
		load_before: null,
		load_after: null,
		load_excluded: false,
		fixed_a_us: null,
		fixed_b_us: null,
		new_cost_us: null,
		new_upper_us: null,
		new_decision: {},
		new_q80_upper_us: null,
		new_q80_decision: {}
	};
}

const round1 = (v: number) => Math.round(v * 10) / 10;

async function sweep() {
	const wasmPath = resolve(ROOT, arg('--wasm') ?? 'bin/tinyimg.wasm');
	const outDir = resolve(ROOT, arg('--out') ?? DEFAULT_OUT);
	const standin = process.argv.includes('--standin');
	const only = arg('--only')?.split(',');
	const widths = arg('--widths')?.split(',').map(Number);
	const outputs = arg('--outputs')?.split(',');
	const waitS = Number(arg('--wait-min') ?? 60) * 60;
	const wasmBytes = readFileSync(wasmPath);
	const wasmSha = createHash('sha256').update(wasmBytes).digest('hex');
	const expect = arg('--expect-sha');

	if (expect && !wasmSha.startsWith(expect)) {
		console.error(`module sha256 ${wasmSha} does not start with ${expect}`);
		process.exit(2);
	}

	const compiled = await WebAssembly.compile(wasmBytes);
	let tinyimg = await TinyImgModule.load(compiled);
	const frozenBytes = readFileSync(FROZEN);
	const frozen = await TinyImgModule.load(await WebAssembly.compile(frozenBytes));
	const frozenSha = createHash('sha256').update(frozenBytes).digest('hex');

	const manifest = JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] };
	const entries = new Map(manifest.entries.map((e) => [e.id, e]));
	const dims: Dims = new Map(manifest.entries.map((e) => [e.id, e]));
	const priors = (JSON.parse(readFileSync(BASELINE, 'utf8')) as { cells: Prior[] }).cells.filter(
		(c) =>
			c.split === 'test' &&
			c.status === 'ok' &&
			c.budget === 0 &&
			!c.alpha &&
			(!only || only.some((o) => c.id === o)) &&
			(!widths || widths.includes(c.width)) &&
			(!outputs || outputs.includes(c.output))
	);

	mkdirSync(outDir, { recursive: true });
	const jsonl = join(outDir, 'admission-cells.jsonl');
	const done = new Map<string, AdmCell>();
	const controls = new Map<string, Control>();
	if (existsSync(jsonl))
		for (const line of readFileSync(jsonl, 'utf8').split('\n')) {
			if (!line.trim()) continue;
			const row = JSON.parse(line) as AdmCell & { kind?: string };
			if (row.kind === 'control')
				controls.set((row as unknown as Control).label, row as never);
			else done.set(cellKey(row), row);
		}

	const loadStart = standin ? loadavg()[0]! : await waitForQuiet(waitS);
	const limit = cpus().length / 4;
	const machine = `${platform()}-${arch()}, ${cpus()[0]?.model ?? 'unknown cpu'}`;
	console.log(`${machine}, ${cpus().length} cores, node ${process.version}`);
	console.log(`wasm ${wasmPath} sha256 ${wasmSha}`);
	console.log(
		`${priors.length} cells, ${standin ? 'stand-in clocks' : 'timed'}, load ${loadStart.toFixed(2)}, resuming ${done.size}`
	);

	const sources = new Map<string, Uint8Array>();
	const sourceOf = (id: string) => {
		if (!sources.has(id))
			sources.set(id, new Uint8Array(readFileSync(fileOf(entries.get(id)!))));
		return sources.get(id)!;
	};

	const request = async (src: Uint8Array, width: number, format: ImageFormat) => {
		using image = await Image.open(tinyimg, src);
		image.resize(width, 0);
		await image.bytes(format, { quality: QUALITY });
	};

	async function timeIt(src: Uint8Array, width: number, format: ImageFormat) {
		await waitForQuiet(waitS);
		const before = loadavg()[0]!;
		const samples = await withLock(async () => {
			for (let i = 0; i < WARMUPS; i++) await request(src, width, format);
			const out: number[] = [];
			for (let i = 0; i < RUNS; i++) {
				const start = performance.now();
				await request(src, width, format);
				out.push((performance.now() - start) * 1000);
			}
			return out;
		});
		return { samples, before, after: loadavg()[0]! };
	}

	async function control(label: string) {
		if (standin || controls.has(label)) return;
		const src = new Uint8Array(readFileSync(join(ROOT, CONTROL.file)));
		const t = await timeIt(src, CONTROL.width, CONTROL.output);
		const row: Control & { kind: string } = {
			kind: 'control',
			label,
			measured_us: round1(median(t.samples)),
			samples_us: t.samples.map(round1),
			load_before: t.before,
			load_after: t.after
		};
		controls.set(label, row);
		appendFileSync(jsonl, JSON.stringify(row) + '\n');
		console.log(
			`control ${label}: ${(row.measured_us / 1000).toFixed(3)} ms, load ${t.after.toFixed(2)}`
		);
	}

	async function runCell(p: Prior): Promise<AdmCell> {
		const cell = emptyCell(p);
		const format = (p.output === 'source' ? p.format : p.output) as ImageFormat;

		try {
			const src = sourceOf(p.id);
			cell.fixed_a_us = p.estimate_us;
			cell.fixed_b_us = totalEstimateUs(frozen, dims, p);

			{
				using image = await Image.open(tinyimg, src);
				image.resize(p.width, 0);
				const channels = image.decide().output.channels;
				cell.alpha = channels === 2 || channels === 4;
				if (cell.alpha) return { ...cell, status: 'skipped', reason: 'alpha source' };

				for (const b of BUDGETS_MS) {
					const a = image.admit(b, { mode: 'strict', format });
					const q = image.admit(b, { mode: 'strict', format, quality: QUALITY });
					cell.new_decision[String(b)] = a.decision;
					cell.new_q80_decision[String(b)] = q.decision;
					cell.new_cost_us = Math.round(a.costMs * 1000);
					cell.new_upper_us = Math.round(a.costUpperMs * 1000);
					cell.new_q80_upper_us = Math.round(q.costUpperMs * 1000);
				}
			}

			if (standin) {
				cell.measured_source = 'standin-v1.1.1';
				cell.measured_us = p.measured_us;
				cell.samples_us = p.samples_us;
				return cell;
			}

			const t = await timeIt(src, p.width, format);
			cell.samples_us = t.samples.map(round1);
			cell.measured_us = round1(median(t.samples));
			cell.load_before = t.before;
			cell.load_after = t.after;
			cell.load_excluded = Math.max(t.before, t.after) > limit;
			return cell;
		} catch (error) {
			tinyimg = await TinyImgModule.load(compiled);
			return { ...cell, status: 'failed', reason: errorText(error), measured_us: null };
		}
	}

	const cells: AdmCell[] = [];
	const middle = Math.floor(priors.length / 2);
	let n = 0;

	await control('start');
	for (const [i, p] of priors.entries()) {
		if (i === middle && i > 0) await control('middle');

		const key = cellKey(p);
		const prior = done.get(key);
		if (prior) {
			cells.push(prior);
			continue;
		}

		const cell = await runCell(p);
		cells.push(cell);
		appendFileSync(jsonl, JSON.stringify(cell) + '\n');
		if (++n % 20 === 0) console.log(`${n} new cells, at ${p.id}`);
	}
	await control('end');

	const meta: AdmMeta = {
		machine,
		cores: cpus().length,
		load_limit: limit,
		wasm: wasmPath.startsWith(ROOT) ? wasmPath.slice(ROOT.length + 1) : wasmPath,
		wasm_sha256: wasmSha,
		frozen_sha256: frozenSha,
		node: process.version,
		mode: standin ? 'standin' : 'timed',
		evaluation: !standin && !only && !widths && !outputs,
		subset:
			only || widths || outputs
				? `${only ? `${only.length} images` : 'all images'}, widths ${widths?.join(',') ?? 'all'}, outputs ${outputs?.join(',') ?? 'all'}`
				: null,
		budgets_ms: [...BUDGETS_MS],
		quality: QUALITY,
		runs: RUNS,
		warmups: WARMUPS,
		load_start: loadStart,
		load_end: loadavg()[0]!
	};
	const order = ['start', 'middle', 'end'];
	const file: AdmFile = {
		meta,
		cells,
		controls: [...controls.values()].sort(
			(a, b) => order.indexOf(a.label) - order.indexOf(b.label)
		)
	};
	writeFileSync(join(outDir, 'admission-cells.json'), JSON.stringify(file) + '\n');
	writeFileSync(join(outDir, 'admission-summary.md'), summaryMarkdown(file));
	console.log(`wrote ${cells.length} cells, load ${meta.load_end!.toFixed(2)}`);
}

// #endregion

if (import.meta.main) {
	const command = process.argv[2];
	const outDir = resolve(ROOT, arg('--out') ?? DEFAULT_OUT);

	if (command === 'run') {
		await sweep();
	} else if (command === 'analyze' || command === 'summarize') {
		const file = JSON.parse(
			readFileSync(join(outDir, 'admission-cells.json'), 'utf8')
		) as AdmFile;
		if (command === 'summarize') {
			writeFileSync(join(outDir, 'admission-summary.md'), summaryMarkdown(file));
		} else {
			const g = gate(file.cells, file.meta.budgets_ms);
			const { reason, pass, ...counts } = g;
			console.log(
				JSON.stringify(file.meta.evaluation ? { pass, reason, ...counts } : counts, null, 1)
			);
		}
	} else {
		console.error('usage: admission-eval.ts run | analyze | summarize');
		process.exit(2);
	}
}
