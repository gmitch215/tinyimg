/**
 * B5: does choosing the decode lever, the output format and its quality jointly serve more
 * constrained requests than the planner's own ladder?
 *
 * ```sh
 * bun scripts/measure/selector-ablation.ts run [--wasm f] [--out dir] [--expect-sha prefix]
 * bun scripts/measure/selector-ablation.ts run --agreement-only   # no clock, no execution
 * bun scripts/measure/selector-ablation.ts analyze [--out dir]
 * bun scripts/measure/selector-ablation.ts summarize [--out dir]
 * ```
 *
 * The units are the (image, width) pairs of the `test` split cells that A4 uses. Each is crossed with
 * a budget of 10, 20 and 50 ms (machine factor 1) and a quality floor of none and 35 dB. Three arms
 * answer every (unit, constraint): the joint search, the ladder (the source's format at its default
 * quality, then the planner's budget ladder) and the independent policy (format and quality by
 * bytes first, then the ladder). The ladder and independent functions are native-only, so
 * `select-probe.c` runs all three against the native build; the joint arm is also run on the pinned
 * module through `Image.select`, and the two must agree on every unit.
 *
 * Each feasible choice is then executed on the pinned module with `Image.bytesFor` (a request that
 * several arms and constraints agree on is run once), timed as open, resize and encode with the
 * same lock and load gate as A4, decoded by an external decoder and scored by PSNR against
 * candidate 0 of the same plan encoded as PNG, which is the plan's own undegraded output.
 * Outcome: served is feasible, measured T at or under B, and PSNR at or above the floor when there
 * is one. The primary figure is the served-rate difference, joint minus each other arm, with a
 * paired bootstrap interval over units. Nothing here is gated.
 */

import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
	appendFileSync,
	existsSync,
	mkdirSync,
	readFileSync,
	rmSync,
	writeFileSync
} from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { basename, join, resolve } from 'node:path';
import type { ImageFormat, Selection } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';
import { CONTROL, controlSpread, type Control } from './admission-eval.js';
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

export const BUDGETS_MS = [10, 20, 50] as const;
export const MIN_DBS = [0, 35] as const;
export const ARMS = ['joint', 'ladder', 'independent'] as const;
export const SEED = 20261108;
export const RESAMPLES = 10000;

const BASELINE = join(ROOT, 'bench', 'baselines', 'v1.1.1', 'cells.json');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const DEFAULT_OUT = join(ROOT, 'bench', 'baselines', 'v1.2');
const REASONS = [
	'none',
	'format',
	'alpha',
	'uncharacterized',
	'peak',
	'quality',
	'budget'
] as const;

export type Arm = (typeof ARMS)[number];

/** One JSON line of `select-probe`. */
export type ProbeLine = {
	image: string;
	width: number;
	budget_us: number;
	min_db: number;
	arm: Arm;
	error?: number;
	feasible?: number;
	reason?: number;
	candidate?: number;
	format?: string;
	quality?: number;
	lossless?: number;
	cost_upper_us?: number;
	peak?: number;
	db?: number;
	db_known?: number;
	db_median?: number;
	bytes_per_pixel?: number;
	bytes_known?: number;
	predicted_bytes?: number;
	out_width?: number;
	out_height?: number;
	alpha?: number;
	lattice?: number;
	considered?: number;
};

// #region selection

/** Builds the `Selection` `Image.bytesFor` runs from a probe line, which must be feasible. */
export function selectionFromProbe(line: ProbeLine): Selection {
	if (!line.feasible) throw new Error('the probe line is not feasible');

	return {
		feasible: true,
		reason: 'none',
		choice: {
			format: line.format as ImageFormat,
			quality: line.quality!,
			lossless: line.lossless !== 0,
			reason: 'none',
			costUpperMs: line.cost_upper_us! / 1000,
			peakBytes: line.peak!,
			db: line.db_known ? line.db! : undefined,
			bytes: line.bytes_known ? line.predicted_bytes! : undefined,
			candidate: line.candidate!,
			dbMedian: line.db_known ? line.db_median! : undefined
		},
		width: line.out_width!,
		height: line.out_height!,
		alpha: line.alpha !== 0,
		coverage: { lever: 0, encode: 0 },
		considered: []
	};
}

/** Candidate 0 as a lossless PNG: the plan's own undegraded output, the PSNR reference. */
export function referenceSelection(width: number, height: number): Selection {
	return {
		feasible: true,
		reason: 'none',
		choice: {
			format: 'png',
			quality: 0,
			lossless: true,
			reason: 'none',
			costUpperMs: 0,
			peakBytes: 0,
			db: undefined,
			bytes: undefined,
			candidate: 0,
			dbMedian: undefined
		},
		width,
		height,
		alpha: false,
		coverage: { lever: 0, encode: 0 },
		considered: []
	};
}

/**
 * The fields where the native probe's joint choice and the module's `Image.select` disagree, by
 * name; empty when they agree. An infeasible search compares its reason, a feasible one the
 * lattice index, format, quality, lossless flag, time bound, dB and bytes.
 */
export function agrees(line: ProbeLine, wasm: Selection): string[] {
	const diff: string[] = [];
	const native = line.feasible === 1;

	if (native !== wasm.feasible) {
		diff.push('feasible');
		return diff;
	}
	if (!native) {
		if (REASONS[line.reason ?? 0] !== wasm.reason) diff.push('reason');
		return diff;
	}

	const c = wasm.choice!;
	if (line.candidate !== c.candidate) diff.push('candidate');
	if (line.format !== c.format) diff.push('format');
	if (line.quality !== c.quality) diff.push('quality');
	if ((line.lossless !== 0) !== c.lossless) diff.push('lossless');
	if (Math.abs(line.cost_upper_us! - c.costUpperMs * 1000) > 1) diff.push('cost_upper');
	if (line.out_width !== wasm.width || line.out_height !== wasm.height) diff.push('extent');
	if ((line.db_known === 1) !== (c.db !== undefined)) diff.push('db_known');
	else if (c.db !== undefined && Math.abs(line.db! - c.db) > 0.0006) diff.push('db');
	if ((line.bytes_known === 1) !== (c.bytes !== undefined)) diff.push('bytes_known');
	else if (c.bytes !== undefined && line.predicted_bytes !== c.bytes) diff.push('bytes');

	return diff;
}

// #endregion

// #region analysis

export type Outcome = {
	unit: string;
	id: string;
	format: string;
	budgetMs: number;
	minDb: number;
	arm: Arm;
	feasible: boolean;
	served: boolean;
	bytes: number | null;
	excluded: boolean;
};

/** Served: feasible, measured T at or under B, and the floor met when there is one. */
export function isServed(
	feasible: boolean,
	timeUs: number | null,
	budgetMs: number,
	minDb: number,
	psnr: number | null
): boolean {
	if (!feasible || timeUs === null || timeUs > budgetMs * 1000) return false;
	return minDb <= 0 || (psnr !== null && psnr >= minDb);
}

export type Pair = {
	unit: string;
	id: string;
	format: string;
	budgetMs: number;
	minDb: number;
	arms: Record<Arm, Outcome>;
};

/**
 * Groups outcomes into (unit, constraint) triples. A triple with an arm missing, or with any arm
 * timed under load, is dropped whole so the three arms stay paired.
 */
export function pairUp(outcomes: Outcome[]): {
	pairs: Pair[];
	excluded: number;
	incomplete: number;
} {
	const groups = new Map<string, Outcome[]>();
	for (const o of outcomes) {
		const k = `${o.unit}|${o.budgetMs}|${o.minDb}`;
		if (!groups.has(k)) groups.set(k, []);
		groups.get(k)!.push(o);
	}

	const pairs: Pair[] = [];
	let excluded = 0;
	let incomplete = 0;
	for (const g of groups.values()) {
		const arms = Object.fromEntries(g.map((o) => [o.arm, o])) as Record<Arm, Outcome>;
		if (ARMS.some((a) => !arms[a])) {
			incomplete++;
		} else if (ARMS.some((a) => arms[a].excluded)) {
			excluded++;
		} else {
			const o = g[0]!;
			pairs.push({
				unit: o.unit,
				id: o.id,
				format: o.format,
				budgetMs: o.budgetMs,
				minDb: o.minDb,
				arms
			});
		}
	}

	return { pairs, excluded, incomplete };
}

/** mulberry32, so the bootstrap is the same on every machine. */
export function rng(seed: number): () => number {
	let a = seed >>> 0;
	return () => {
		a = (a + 0x6d2b79f5) >>> 0;
		let t = a;
		t = Math.imul(t ^ (t >>> 15), t | 1);
		t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}

export type Interval = { estimate: number; lo: number; hi: number; resamples: number };

/**
 * Paired bootstrap of the pooled served-rate difference `a - b`, resampling clusters of pairs with
 * replacement. The rate of a resample is the sum of served over the sum of pairs of the clusters
 * drawn. The interval is the 2.5th and 97.5th percentile of the sorted differences, indices
 * `floor(0.025 N)` and `ceil(0.975 N) - 1`.
 */
export function bootstrapDifference(
	pairs: Pair[],
	a: Arm,
	b: Arm,
	cluster: (p: Pair) => string,
	seed = SEED,
	resamples = RESAMPLES
): Interval {
	const groups = new Map<string, { n: number; a: number; b: number }>();
	for (const p of pairs) {
		const k = cluster(p);
		const g = groups.get(k) ?? { n: 0, a: 0, b: 0 };
		g.n++;
		g.a += p.arms[a].served ? 1 : 0;
		g.b += p.arms[b].served ? 1 : 0;
		groups.set(k, g);
	}

	const list = [...groups.values()];
	const total = list.reduce((s, g) => s + g.n, 0);
	const estimate =
		total === 0
			? 0
			: (list.reduce((s, g) => s + g.a, 0) - list.reduce((s, g) => s + g.b, 0)) / total;
	if (list.length === 0) return { estimate, lo: 0, hi: 0, resamples };

	const next = rng(seed);
	const diffs: number[] = [];
	for (let r = 0; r < resamples; r++) {
		let n = 0;
		let sa = 0;
		let sb = 0;
		for (let i = 0; i < list.length; i++) {
			const g = list[Math.floor(next() * list.length)]!;
			n += g.n;
			sa += g.a;
			sb += g.b;
		}
		diffs.push((sa - sb) / n);
	}

	const [lo, hi] = interval95(diffs);
	return { estimate, lo, hi, resamples };
}

/** The 2.5th and 97.5th percentile of resampled values: sorted indices `floor(0.025 N)` and `ceil(0.975 N) - 1`. */
export function interval95(values: number[]): [number, number] {
	const sorted = [...values].sort((x, y) => x - y);
	const n = sorted.length;

	return [sorted[Math.floor(0.025 * n)]!, sorted[Math.ceil(0.975 * n) - 1]!];
}

export type Ratio = { n: number; median: number; p10: number; p90: number; total: number } | null;

/** Joint bytes over the other arm's, among the pairs both served; null when none. */
export function bytesRatio(pairs: Pair[], other: Arm): Ratio {
	const both = pairs.filter(
		(p) =>
			p.arms.joint.served &&
			p.arms[other].served &&
			p.arms.joint.bytes !== null &&
			p.arms[other].bytes !== null &&
			p.arms[other].bytes! > 0
	);
	if (both.length === 0) return null;

	const r = both.map((p) => p.arms.joint.bytes! / p.arms[other].bytes!).sort((x, y) => x - y);
	const q = (f: number) => r[Math.round(f * (r.length - 1))]!;
	const sum = (arm: Arm) => both.reduce((s, p) => s + p.arms[arm].bytes!, 0);

	return {
		n: both.length,
		median: q(0.5),
		p10: q(0.1),
		p90: q(0.9),
		total: sum('joint') / sum(other)
	};
}

export type Rates = Record<Arm, number> & { pairs: number };

/** Served counts per arm over some pairs. */
export function servedCounts(pairs: Pair[]): Rates {
	const out = { pairs: pairs.length, joint: 0, ladder: 0, independent: 0 };
	for (const p of pairs) for (const a of ARMS) if (p.arms[a].served) out[a]++;
	return out;
}

export type Analysis = {
	pairs: number;
	units: number;
	excluded: number;
	incomplete: number;
	served: Rates;
	jointMinusLadder: Interval;
	jointMinusIndependent: Interval;
	jointMinusLadderByImage: Interval;
	jointMinusIndependentByImage: Interval;
	rescues: { ladder: number; independent: number };
	regressions: { ladder: number; independent: number };
	bytes: { ladder: Ratio; independent: Ratio };
	byConstraint: [string, Rates][];
	byFormat: [string, Rates][];
};

/** Everything the pre-registration asks for, from the outcomes alone. */
export function analyze(outcomes: Outcome[], resamples = RESAMPLES): Analysis {
	const { pairs, excluded, incomplete } = pairUp(outcomes);
	const unit = (p: Pair) => p.unit;
	const image = (p: Pair) => p.id;
	const count = (a: Arm, b: Arm) =>
		pairs.filter((p) => p.arms[a].served && !p.arms[b].served).length;
	const group = (key: (p: Pair) => string): [string, Rates][] => {
		const m = new Map<string, Pair[]>();
		for (const p of pairs) {
			const k = key(p);
			if (!m.has(k)) m.set(k, []);
			m.get(k)!.push(p);
		}
		return [...m]
			.sort(([x], [y]) => x.localeCompare(y, 'en', { numeric: true }))
			.map(([k, v]) => [k, servedCounts(v)]);
	};

	return {
		pairs: pairs.length,
		units: new Set(pairs.map(unit)).size,
		excluded,
		incomplete,
		served: servedCounts(pairs),
		jointMinusLadder: bootstrapDifference(pairs, 'joint', 'ladder', unit, SEED, resamples),
		jointMinusIndependent: bootstrapDifference(
			pairs,
			'joint',
			'independent',
			unit,
			SEED,
			resamples
		),
		jointMinusLadderByImage: bootstrapDifference(
			pairs,
			'joint',
			'ladder',
			image,
			SEED,
			resamples
		),
		jointMinusIndependentByImage: bootstrapDifference(
			pairs,
			'joint',
			'independent',
			image,
			SEED,
			resamples
		),
		rescues: { ladder: count('joint', 'ladder'), independent: count('joint', 'independent') },
		regressions: {
			ladder: count('ladder', 'joint'),
			independent: count('independent', 'joint')
		},
		bytes: {
			ladder: bytesRatio(pairs, 'ladder'),
			independent: bytesRatio(pairs, 'independent')
		},
		byConstraint: group(
			(p) => `${p.budgetMs} ms, ${p.minDb > 0 ? `${p.minDb} dB floor` : 'no floor'}`
		),
		byFormat: group((p) => p.format)
	};
}

// #endregion

// #region summary

export type Run = {
	key: string;
	time_us: number | null;
	samples_us: number[];
	bytes: number | null;
	psnr: number | null;
	load_before: number | null;
	load_after: number | null;
	load_excluded: boolean;
	error: string | null;
};

export type Agreement = {
	compared: number;
	disagreements: { unit: string; budget_us: number; min_db: number; fields: string[] }[];
};

export type SelMeta = {
	machine: string;
	cores: number;
	load_limit: number;
	wasm: string;
	wasm_sha256: string;
	node: string;
	probe_source_sha256: string;
	library: string;
	library_sha256: string;
	mode: 'timed' | 'agreement-only';
	evaluation: boolean;
	subset: string | null;
	budgets_ms: number[];
	min_dbs: number[];
	seed: number;
	resamples: number;
	runs: number;
	warmups: number;
	units: number;
	alpha_units: string[];
	load_start: number;
	load_end: number | null;
};

export type SelFile = {
	meta: SelMeta;
	agreement: Agreement;
	outcomes: Outcome[];
	runs: Run[];
	controls: Control[];
	errors: string[];
};

const pct = (v: number) => `${(100 * v).toFixed(2)}%`;
const signed = (v: number) => `${v >= 0 ? '+' : ''}${(100 * v).toFixed(2)}`;
const ci = (i: Interval) => `${signed(i.estimate)} points, 95% [${signed(i.lo)}, ${signed(i.hi)}]`;
const where = (i: Interval) =>
	i.lo > 0 ? 'above zero' : i.hi < 0 ? 'below zero' : 'includes zero';
const ratio = (r: Ratio) =>
	r === null
		? 'no co-served cells'
		: `${r.n} co-served cells, median ${r.median.toFixed(3)}, p10 ${r.p10.toFixed(3)}, p90 ${r.p90.toFixed(3)}, total ${r.total.toFixed(3)}`;

function ratesRows(rows: [string, Rates][]): string[] {
	return [
		'| group | pairs | joint | ladder | independent | joint minus ladder |',
		'| --- | ---: | ---: | ---: | ---: | ---: |',
		...rows.map(
			([k, r]) =>
				`| ${k} | ${r.pairs} | ${r.joint} (${pct(r.joint / r.pairs)}) | ${r.ladder} (${pct(r.ladder / r.pairs)}) | ` +
				`${r.independent} (${pct(r.independent / r.pairs)}) | ${signed((r.joint - r.ladder) / r.pairs)} |`
		),
		''
	];
}

/** Builds the summary from the file alone, so a spec can recompute it. */
export function summaryMarkdown(file: SelFile): string {
	const { meta, agreement, outcomes, controls } = file;
	const out: string[] = ['# B5: the selector ablation', ''];

	if (!meta.evaluation) {
		out.push(
			meta.mode === 'agreement-only'
				? '**Agreement check only, not the evaluation.** No choice was executed and nothing was timed, so there are no served rates.'
				: `**Subset run${meta.subset ? ` (${meta.subset})` : ''}, not the evaluation.** A dry run of the pipeline on a few units. The figures below show that the code runs and say nothing about the arms.`,
			''
		);
	}

	out.push(
		`Machine ${meta.machine} (${meta.cores} cores), node ${meta.node}. Module \`${meta.wasm}\` sha256 ${meta.wasm_sha256}. ` +
			`Native probe: \`${meta.library}\`, library sha256 ${meta.library_sha256}, probe source sha256 ${meta.probe_source_sha256}. Mode ${meta.mode}, load ` +
			`${meta.load_start.toFixed(2)} at start` +
			(meta.load_end === null ? '' : ` and ${meta.load_end.toFixed(2)} at end`) +
			`, limit ${meta.load_limit.toFixed(2)}. ${meta.units} units; budgets ${meta.budgets_ms.join(', ')} ms at machine factor 1, ` +
			`floors ${meta.min_dbs.map((d) => (d > 0 ? `${d} dB` : 'none')).join(' and ')}; objective fewest bytes, uncharacterized candidates refused. ` +
			`Reference for PSNR: candidate 0 of the same plan, encoded as PNG, decoded by magick. Bootstrap seed ${meta.seed}, ${meta.resamples} resamples.`,
		''
	);
	if (meta.alpha_units.length)
		out.push(
			`Alpha sources left out, as in A4 (${meta.alpha_units.length} units): ${meta.alpha_units.join(', ')}.`,
			''
		);

	out.push('## Joint arm, native probe against the module', '');
	out.push(
		`${agreement.compared} (unit, constraint) joint choices compared, ${agreement.disagreements.length} disagreements.`,
		''
	);
	for (const d of agreement.disagreements.slice(0, 20))
		out.push(`- ${d.unit}, ${d.budget_us} us, ${d.min_db} dB: ${d.fields.join(', ')}`);
	if (agreement.disagreements.length) out.push('');

	if (outcomes.length === 0) {
		out.push('No outcomes: nothing was executed.', '');
		return out.join('\n');
	}

	const a = analyze(outcomes, meta.resamples);
	out.push('## Served', '');
	out.push(
		`${a.pairs} (unit, constraint) pairs over ${a.units} units; ${a.excluded} pairs left out because a run was timed above the load limit, ${a.incomplete} incomplete.`,
		'',
		'Served is feasible, measured T at or under B, and the PSNR floor met when there is one.',
		''
	);
	out.push(...ratesRows([['all', a.served]]));
	out.push(
		`Joint minus ladder (cells are units): ${ci(a.jointMinusLadder)}; the interval ${where(a.jointMinusLadder)}.`,
		'',
		`Joint minus independent (cells are units): ${ci(a.jointMinusIndependent)}; the interval ${where(a.jointMinusIndependent)}.`,
		'',
		`Sensitivity, clusters are images: joint minus ladder ${ci(a.jointMinusLadderByImage)}; joint minus independent ${ci(a.jointMinusIndependentByImage)}.`,
		'',
		`Cells joint serves and ladder does not (lattice rescues): ${a.rescues.ladder}. The reverse: ${a.regressions.ladder}. ` +
			`Against independent: ${a.rescues.independent} and ${a.regressions.independent}.`,
		'',
		`Bytes, joint over ladder: ${ratio(a.bytes.ladder)}. Joint over independent: ${ratio(a.bytes.independent)}.`,
		''
	);
	out.push('### By constraint', '', ...ratesRows(a.byConstraint));
	out.push('### By source format', '', ...ratesRows(a.byFormat));

	out.push('## Control', '');
	if (controls.length === 0) {
		out.push('No timed control.', '');
	} else {
		out.push(
			`\`${CONTROL.file}\` width ${CONTROL.width}, ${CONTROL.output} output.`,
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

	if (file.errors.length) {
		out.push('## Errors', '', ...file.errors.slice(0, 30).map((e) => `- ${e}`), '');
	}

	out.push(
		'## How to reproduce',
		'',
		'```sh\ncmake -S . -B build-a4 -DCMAKE_C_FLAGS=-O2 && cmake --build build-a4 -j3 --target tinyimg\nbun scripts/measure/selector-ablation.ts run --wasm <module> --expect-sha <prefix>\nbun scripts/measure/selector-ablation.ts summarize\n```',
		''
	);
	return out.join('\n');
}

// #endregion

// #region sweep

type Prior = {
	id: string;
	split: string;
	status: string;
	budget: number;
	alpha: boolean;
	width: number;
};

type Unit = { id: string; width: number; path: string; format: string; key: string };

export function readRaw(file: string, format: string): Buffer {
	const out = (input: string) =>
		execFileSync('magick', [input, '-depth', '8', 'rgb:-'], { maxBuffer: 1 << 28 });
	const scratch = `${file}.dec`;

	if (format === 'jpeg') {
		execFileSync('djpeg', ['-outfile', `${scratch}.pnm`, file]);
		return out(`${scratch}.pnm`);
	}
	if (format === 'webp') {
		execFileSync('dwebp', ['-quiet', '-pam', file, '-o', `${scratch}.pam`]);
		return out(`${scratch}.pam`);
	}
	if (format === 'avif') {
		execFileSync('avifdec', [file, `${scratch}.png`], { stdio: 'ignore' });
		return out(`${scratch}.png`);
	}
	return out(file);
}

/** PSNR over every compared sample; identical buffers are 99 dB, as in the distortion table. */
export function psnr(a: Uint8Array, b: Uint8Array): number {
	let sq = 0;
	for (let i = 0; i < a.length; i++) {
		const d = a[i]! - b[i]!;
		sq += d * d;
	}
	return sq === 0 ? 99 : Math.min(99, 10 * Math.log10((255 * 255 * a.length) / sq));
}

const EXT: Record<string, string> = { jpeg: 'jpg', tiff: 'tif' };

async function sweep() {
	const wasmPath = resolve(ROOT, arg('--wasm') ?? 'bin/tinyimg.wasm');
	const outDir = resolve(ROOT, arg('--out') ?? DEFAULT_OUT);
	const buildDir = resolve(ROOT, arg('--build-dir') ?? 'build-a4');
	const agreementOnly = process.argv.includes('--agreement-only');
	const only = arg('--only')?.split(',');
	const widths = arg('--widths')?.split(',').map(Number);
	const extra = arg('--extra')?.split(',');
	const waitS = Number(arg('--wait-min') ?? 60) * 60;
	const wasmBytes = readFileSync(wasmPath);
	const wasmSha = createHash('sha256').update(wasmBytes).digest('hex');
	const expect = arg('--expect-sha');

	if (expect && !wasmSha.startsWith(expect)) {
		console.error(`module sha256 ${wasmSha} does not start with ${expect}`);
		process.exit(2);
	}

	const probeBin = join(buildDir, 'select-probe');
	const probeSource = join(ROOT, 'scripts', 'measure', 'select-probe.c');
	const library = join(buildDir, 'lib', 'libtinyimg.a');
	if (!existsSync(library)) {
		console.error(`${library} is missing; build it first (see the header of this file)`);
		process.exit(2);
	}
	execFileSync(
		'clang',
		[
			'-O2',
			'-I',
			join(ROOT, 'include'),
			'-I',
			join(ROOT, 'src'),
			'-o',
			probeBin,
			probeSource,
			library,
			'-lm'
		],
		{ stdio: 'inherit' }
	);

	const compiled = await WebAssembly.compile(wasmBytes);
	let tinyimg = await TinyImgModule.load(compiled);
	const manifest = JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] };
	const entries = new Map(manifest.entries.map((e) => [e.id, e]));
	const priors = (JSON.parse(readFileSync(BASELINE, 'utf8')) as { cells: Prior[] }).cells.filter(
		(c) => c.split === 'test' && c.status === 'ok' && c.budget === 0 && !c.alpha
	);

	const units: Unit[] = [];
	const seen = new Set<string>();
	for (const p of priors) {
		const key = `${p.id}@${p.width}`;
		if (seen.has(key)) continue;
		if (only && !only.includes(p.id)) continue;
		if (widths && !widths.includes(p.width)) continue;
		seen.add(key);
		const e = entries.get(p.id)!;
		units.push({ id: p.id, width: p.width, path: fileOf(e), format: e.format, key });
	}
	for (const f of extra ?? []) {
		const path = resolve(ROOT, f);
		const ext = path.slice(path.lastIndexOf('.') + 1).toLowerCase();
		const format = ({ jpg: 'jpeg', tif: 'tiff' } as Record<string, string>)[ext] ?? ext;
		for (const w of widths ?? [200]) {
			units.push({
				id: basename(path),
				width: w,
				path,
				format,
				key: `${basename(path)}@${w}`
			});
		}
	}

	mkdirSync(outDir, { recursive: true });
	const work = join(buildDir, 'selector-work');
	mkdirSync(work, { recursive: true });
	const jsonl = join(outDir, 'selector-runs.jsonl');
	const doneRuns = new Map<string, Run>();
	const controls = new Map<string, Control>();
	if (existsSync(jsonl))
		for (const line of readFileSync(jsonl, 'utf8').split('\n')) {
			if (!line.trim()) continue;
			const row = JSON.parse(line) as Run & { kind?: string; label?: string };
			if (row.kind === 'control') controls.set(row.label!, row as unknown as Control);
			else doneRuns.set(row.key, row);
		}

	const limit = cpus().length / 4;
	const loadStart = agreementOnly ? loadavg()[0]! : await waitForQuiet(waitS);
	const machine = `${platform()}-${arch()}, ${cpus()[0]?.model ?? 'unknown cpu'}`;
	console.log(`${machine}; wasm ${wasmPath} sha256 ${wasmSha}`);
	console.log(
		`${units.length} units, load ${loadStart.toFixed(2)}, resuming ${doneRuns.size} runs`
	);

	// #region probe
	const budgetsUs = BUDGETS_MS.map((b) => b * 1000).join(',');
	const floors = MIN_DBS.join(',');
	const lines = new Map<string, ProbeLine>();
	const lineKey = (image: string, width: number, budgetUs: number, minDb: number, arm: string) =>
		`${image}|${width}|${budgetUs}|${minDb}|${arm}`;
	const byWidth = new Map<number, Unit[]>();
	for (const u of units) byWidth.set(u.width, [...(byWidth.get(u.width) ?? []), u]);
	for (const [width, group] of byWidth) {
		for (let i = 0; i < group.length; i += 40) {
			const run = spawnSync(
				probeBin,
				[String(width), budgetsUs, floors, ...group.slice(i, i + 40).map((u) => u.path)],
				{ encoding: 'utf8', maxBuffer: 1 << 28 }
			);
			for (const l of run.stdout.split('\n')) {
				if (!l.startsWith('{')) continue;
				const line = JSON.parse(l) as ProbeLine;
				lines.set(
					lineKey(line.image, line.width, line.budget_us, line.min_db, line.arm ?? ''),
					line
				);
			}
		}
	}
	const probed = (u: Unit, budgetMs: number, minDb: number, arm: Arm) =>
		lines.get(lineKey(basename(u.path), u.width, budgetMs * 1000, minDb, arm));
	// #endregion

	const errors: string[] = [];
	const agreement: Agreement = { compared: 0, disagreements: [] };
	const alphaUnits: string[] = [];
	const live: Unit[] = [];

	// #region agreement
	for (const u of units) {
		const src = new Uint8Array(readFileSync(u.path));
		const first = probed(u, BUDGETS_MS[0], MIN_DBS[0], 'joint');
		if (!first || first.error !== undefined) {
			errors.push(`${u.key}: the probe failed (${first?.error ?? 'no output'})`);
			continue;
		}
		if (first.alpha) {
			alphaUnits.push(u.key);
			continue;
		}
		live.push(u);

		try {
			using image = await Image.open(tinyimg, src);
			image.resize(u.width, 0);
			for (const b of BUDGETS_MS)
				for (const d of MIN_DBS) {
					const line = probed(u, b, d, 'joint')!;
					const formats = [...new Set([u.format, 'jpeg', 'webp'])] as ImageFormat[];
					const sel = image.select({
						budgetMs: b,
						...(d > 0 ? { minDb: d } : {}),
						formats,
						objective: 'bytes',
						uncharacterized: 'refuse'
					});
					const fields = agrees(line, sel);
					agreement.compared++;
					if (fields.length)
						agreement.disagreements.push({
							unit: u.key,
							budget_us: b * 1000,
							min_db: d,
							fields
						});
				}
		} catch (error) {
			tinyimg = await TinyImgModule.load(compiled);
			errors.push(`${u.key}: select failed: ${errorText(error)}`);
		}
	}
	console.log(
		`agreement: ${agreement.compared} compared, ${agreement.disagreements.length} disagreements`
	);
	// #endregion

	const outcomes: Outcome[] = [];
	const runs = new Map<string, Run>();

	async function request(src: Uint8Array, u: Unit, sel: Selection) {
		using image = await Image.open(tinyimg, src);
		image.resize(u.width, 0);
		return image.bytesFor(sel);
	}

	async function control(label: string) {
		if (controls.has(label)) return;
		const src = new Uint8Array(readFileSync(join(ROOT, CONTROL.file)));
		await waitForQuiet(waitS);
		const before = loadavg()[0]!;
		const samples = await withLock(async () => {
			const one = async () => {
				using image = await Image.open(tinyimg, src);
				image.resize(CONTROL.width, 0);
				await image.bytes(CONTROL.output, { quality: 80 });
			};
			for (let i = 0; i < WARMUPS; i++) await one();
			const o: number[] = [];
			for (let i = 0; i < RUNS; i++) {
				const t = performance.now();
				await one();
				o.push((performance.now() - t) * 1000);
			}
			return o;
		});
		const row = {
			kind: 'control',
			label,
			measured_us: Math.round(median(samples) * 10) / 10,
			samples_us: samples.map((s) => Math.round(s * 10) / 10),
			load_before: before,
			load_after: loadavg()[0]!
		};
		controls.set(label, row as unknown as Control);
		appendFileSync(jsonl, JSON.stringify(row) + '\n');
		console.log(
			`control ${label}: ${(row.measured_us / 1000).toFixed(3)} ms, load ${row.load_after.toFixed(2)}`
		);
	}

	async function execute(
		u: Unit,
		src: Uint8Array,
		line: ProbeLine,
		ref: { raw: Buffer | null }
	): Promise<Run> {
		const key = `${u.key}|${line.candidate}|${line.format}|${line.quality}|${line.lossless}`;
		const done = doneRuns.get(key) ?? runs.get(key);
		if (done) return done;

		const sel = selectionFromProbe(line);
		const run: Run = {
			key,
			time_us: null,
			samples_us: [],
			bytes: null,
			psnr: null,
			load_before: null,
			load_after: null,
			load_excluded: false,
			error: null
		};
		try {
			await waitForQuiet(waitS);
			run.load_before = loadavg()[0]!;
			let last: Uint8Array | null = null;
			const samples = await withLock(async () => {
				for (let i = 0; i < WARMUPS; i++) await request(src, u, sel);
				const o: number[] = [];
				for (let i = 0; i < RUNS; i++) {
					const t = performance.now();
					last = await request(src, u, sel);
					o.push((performance.now() - t) * 1000);
				}
				return o;
			});
			run.load_after = loadavg()[0]!;
			run.load_excluded = Math.max(run.load_before, run.load_after) > limit;
			run.samples_us = samples.map((s) => Math.round(s * 10) / 10);
			run.time_us = Math.round(median(samples) * 10) / 10;
			run.bytes = last!.byteLength;

			if (!ref.raw) {
				const refBytes = await request(
					src,
					u,
					referenceSelection(line.out_width!, line.out_height!)
				);
				const refFile = join(
					work,
					`${createHash('sha1').update(u.key).digest('hex')}.ref.png`
				);
				writeFileSync(refFile, refBytes);
				ref.raw = readRaw(refFile, 'png');
				rmSync(refFile, { force: true });
			}
			const file = join(
				work,
				`${createHash('sha1').update(key).digest('hex')}.${EXT[line.format!] ?? line.format}`
			);
			writeFileSync(file, last!);
			try {
				const got = readRaw(file, line.format!);
				if (got.length === ref.raw.length)
					run.psnr = Math.round(psnr(ref.raw, got) * 10000) / 10000;
				else run.error = `decoded ${got.length} bytes, reference ${ref.raw.length}`;
			} finally {
				for (const ext of ['pnm', 'pam', 'png'])
					rmSync(`${file}.dec.${ext}`, { force: true });
				rmSync(file, { force: true });
			}
		} catch (error) {
			tinyimg = await TinyImgModule.load(compiled);
			run.error = errorText(error);
		}
		runs.set(key, run);
		appendFileSync(jsonl, JSON.stringify(run) + '\n');
		return run;
	}

	if (!agreementOnly) {
		await control('start');
		const middle = Math.floor(live.length / 2);
		for (const [i, u] of live.entries()) {
			if (i === middle && i > 0) await control('middle');
			const src = new Uint8Array(readFileSync(u.path));
			const ref: { raw: Buffer | null } = { raw: null };

			for (const b of BUDGETS_MS)
				for (const d of MIN_DBS)
					for (const arm of ARMS) {
						const line = probed(u, b, d, arm)!;
						const feasible = line.feasible === 1;
						const run = feasible ? await execute(u, src, line, ref) : null;
						if (run?.error)
							errors.push(`${u.key} ${arm} ${b} ms ${d} dB: ${run.error}`);
						outcomes.push({
							unit: u.key,
							id: u.id,
							format: u.format,
							budgetMs: b,
							minDb: d,
							arm,
							feasible,
							served: run ? isServed(feasible, run.time_us, b, d, run.psnr) : false,
							bytes: run?.bytes ?? null,
							excluded: run?.load_excluded ?? false
						});
					}
			if ((i + 1) % 10 === 0) console.log(`${i + 1}/${live.length} units, at ${u.key}`);
		}
		await control('end');
	}

	const order = ['start', 'middle', 'end'];
	const meta: SelMeta = {
		machine,
		cores: cpus().length,
		load_limit: limit,
		wasm: wasmPath.startsWith(ROOT) ? wasmPath.slice(ROOT.length + 1) : wasmPath,
		wasm_sha256: wasmSha,
		node: process.version,
		probe_source_sha256: createHash('sha256').update(readFileSync(probeSource)).digest('hex'),
		library: library.startsWith(ROOT) ? library.slice(ROOT.length + 1) : library,
		library_sha256: createHash('sha256').update(readFileSync(library)).digest('hex'),
		mode: agreementOnly ? 'agreement-only' : 'timed',
		evaluation: !agreementOnly && !only && !widths && !extra,
		subset:
			only || widths || extra
				? `${only ? `${only.length} images` : 'all images'}, widths ${widths?.join(',') ?? 'all'}, extra ${extra?.join(',') ?? 'none'}`
				: null,
		budgets_ms: [...BUDGETS_MS],
		min_dbs: [...MIN_DBS],
		seed: SEED,
		resamples: RESAMPLES,
		runs: RUNS,
		warmups: WARMUPS,
		units: live.length,
		alpha_units: alphaUnits,
		load_start: loadStart,
		load_end: loadavg()[0]!
	};
	const file: SelFile = {
		meta,
		agreement,
		outcomes,
		runs: [...doneRuns.values(), ...runs.values()],
		controls: [...controls.values()].sort(
			(a, b) => order.indexOf(a.label) - order.indexOf(b.label)
		),
		errors
	};
	writeFileSync(join(outDir, 'selector-cells.json'), JSON.stringify(file) + '\n');
	writeFileSync(join(outDir, 'selector-summary.md'), summaryMarkdown(file));
	console.log(`wrote ${outcomes.length} outcomes, ${file.runs.length} runs`);
}

// #endregion

if (import.meta.main) {
	const command = process.argv[2];
	const outDir = resolve(ROOT, arg('--out') ?? DEFAULT_OUT);

	if (command === 'run') {
		await sweep();
	} else if (command === 'analyze' || command === 'summarize') {
		const file = JSON.parse(
			readFileSync(join(outDir, 'selector-cells.json'), 'utf8')
		) as SelFile;
		if (command === 'summarize')
			writeFileSync(join(outDir, 'selector-summary.md'), summaryMarkdown(file));
		else console.log(JSON.stringify(analyze(file.outcomes, file.meta.resamples), null, 1));
	} else {
		console.error('usage: selector-ablation.ts run | analyze | summarize');
		process.exit(2);
	}
}
