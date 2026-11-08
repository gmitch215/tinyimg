/**
 * Fits the planner's cost model from `bench/baselines/fit/cells.json` and writes `src/cost-model.h`.
 *
 * ```sh
 * bun scripts/cost-model.ts            # regenerate src/cost-model.h
 * bun scripts/cost-model.ts --check    # verify the committed header matches the committed cells
 * bun scripts/cost-model.ts --report   # print the fit, the verdicts and the conf-half error tables
 * ```
 *
 * The cells are the record, written by `bun scripts/measure/fit-cost.ts`. Everything here is a pure
 * function of them, so `--check` needs neither a build nor a clock. Cells come from `cal` entries
 * only, split by content: an entry is `fit` when `parseInt(sha256.slice(8, 10), 16)` is even and
 * `conf` otherwise. Every constant, structural choice and the fallback group is decided from the
 * `fit` half; the `conf` half is only used for the conformal quantile and for reporting, so
 * the coverage it shows has not been fitted to.
 *
 * Beside decode and the default encode rate, the cells carry paired measurements that set the effort
 * fractions (decode and encode FAST over FANCY), the quality and lossless factors, and the alpha
 * factor. Each is a median ratio over `fit` cells; a FAST fraction whose median lies within the
 * median half range of its own cells' round-by-round ratios of 1 is exactly 1000. Cells of a derived
 * GIF or TIFF belong to the half of their source, and leave-one-out leaves out every size of a
 * source together.
 *
 * Decode is `alpha[format][den] * S + beta[format] * B` per request, fitted by non-negative least
 * squares on relative error, where S is the samples the decoder traverses and B the entropy bytes it
 * consumes. The table of consumed fractions comes from the `ENTROPY_BYTES` counter.
 */

import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = join(import.meta.dirname, '..');
const cellsFlag = process.argv.indexOf('--cells');
const CELLS =
	cellsFlag >= 0
		? process.argv[cellsFlag + 1]!
		: join(ROOT, 'bench', 'baselines', 'fit', 'cells.json');
const HEADER = join(ROOT, 'src', 'cost-model.h');

/** Names by `TinyImageFormat` value; index 0 and 8 are not formats the model prices. */
export const FORMATS = ['unknown', 'png', 'jpeg', 'bmp', 'gif', 'tiff', 'webp', 'avif', 'heif'];
const PRICED = [1, 2, 3, 4, 5, 6, 7];
const DENS = [1, 2, 4, 8];
const MISCOVERAGE = 0.05;
const STORED_FLOOR = 900;

type Counters = { entropy: number; source: number; decoded: number; probes: number };

export type Cell = {
	key: string;
	kind: 'decode' | 'encode' | 'effort' | 'whole' | 'control' | 'effortdec' | 'tier';
	id: string;
	fit: boolean;
	derived?: boolean;
	format: string;
	group?: string;
	variant?: string;
	ratioMin?: number;
	ratioMax?: number;
	lossless?: boolean | null;
	ms: number;
	error?: number;
	tag?: string;
	den?: number;
	region?: { x: number; y: number; w: number; h: number } | null;
	fileBytes?: number;
	width?: number;
	height?: number;
	progressive?: boolean;
	srcChannels?: number;
	hasAlpha?: boolean;
	outFormat?: string;
	compression?: string;
	quality?: number;
	channels?: number;
	opaque?: boolean;
	counters?: Counters;
	fastMs?: number;
	ratio?: number;
	decodeMs?: number;
	encodeMs?: number;
	sumOverWhole?: number;
};

export type DecodeRow = {
	id: string;
	/** the entry, or for a derived entry the corpus file it came from */
	src: string;
	derived: boolean;
	fit: boolean;
	f: number;
	T: number;
	den: number;
	tag: string;
	W: number;
	H: number;
	x: number;
	y: number;
	w: number;
	h: number;
	bytes: number;
	prog: number;
	channels: number;
	entropy: number;
};

export type EncodeRow = {
	id: string;
	fit: boolean;
	f: number;
	T: number;
	N: number;
	alpha: number;
	opaque: boolean;
	tag: string;
	width: number;
	compression: string;
};

const fmt = (name: string): number => FORMATS.indexOf(name);

/** A derived entry's corpus source, so leave-one-out leaves out every size of a source together. */
export function sourceOf(id: string): string {
	return id.replace(/^derived-(gif|tiff)-/, '').replace(/-w\d+$/, '');
}

// #region statistics

function median(values: number[]): number {
	if (values.length === 0) return NaN;
	const s = [...values].sort((a, b) => a - b);
	const m = s.length >> 1;
	return s.length % 2 ? s[m]! : (s[m - 1]! + s[m]!) / 2;
}

/** Empirical quantile by the `ceil` rank, as split conformal prediction defines it. */
function rankQuantile(values: number[], rank: number): number {
	const s = [...values].sort((a, b) => a - b);
	return s[Math.min(s.length, Math.max(1, rank)) - 1]!;
}

function interpolated(values: number[], p: number): number {
	const s = [...values].sort((a, b) => a - b);
	if (s.length === 0) return NaN;
	const at = p * (s.length - 1);
	const lo = Math.floor(at);
	const hi = Math.ceil(at);
	return s[lo]! + (s[hi]! - s[lo]!) * (at - lo);
}

/** Solves the normal equations of a small least squares problem by Gaussian elimination. */
function solve(a: number[][], b: number[]): number[] {
	const n = b.length;
	const m = a.map((row, i) => [...row, b[i]!]);

	for (let c = 0; c < n; c++) {
		let p = c;
		for (let r = c + 1; r < n; r++) if (Math.abs(m[r]![c]!) > Math.abs(m[p]![c]!)) p = r;
		[m[c], m[p]] = [m[p]!, m[c]!];
		const pivot = m[c]![c]!;
		if (Math.abs(pivot) < 1e-300) continue;
		for (let r = 0; r < n; r++) {
			if (r === c) continue;
			const k = m[r]![c]! / pivot;
			for (let j = c; j <= n; j++) m[r]![j]! -= k * m[c]![j]!;
		}
	}

	return m.map((row, i) => (Math.abs(row[i]!) < 1e-300 ? 0 : row[n]! / row[i]!));
}

/** Lawson and Hanson's active set method for `min |Ax - b|` with `x >= 0`; columns are normalized first. */
export function nnls(rows: number[][], b: number[]): number[] {
	const n = rows[0]?.length ?? 0;
	const scale = Array.from({ length: n }, (_, j) =>
		Math.sqrt(rows.reduce((s, r) => s + r[j]! * r[j]!, 0))
	);
	const a = rows.map((r) => r.map((v, j) => (scale[j]! > 0 ? v / scale[j]! : 0)));

	const x = new Array<number>(n).fill(0);
	const passive = new Array<boolean>(n).fill(false);
	const gradient = () =>
		Array.from({ length: n }, (_, j) =>
			a.reduce((s, r, i) => {
				const fitted = r.reduce((t, v, k) => t + v * x[k]!, 0);
				return s + r[j]! * (b[i]! - fitted);
			}, 0)
		);

	const lsOnPassive = (): number[] => {
		const idx = passive.flatMap((p, j) => (p ? [j] : []));
		const ata = idx.map((p) => idx.map((q) => a.reduce((s, r) => s + r[p]! * r[q]!, 0)));
		const atb = idx.map((p) => a.reduce((s, r, i) => s + r[p]! * b[i]!, 0));
		const sol = solve(ata, atb);
		const z = new Array<number>(n).fill(0);
		idx.forEach((j, k) => (z[j] = sol[k]!));
		return z;
	};

	for (let iter = 0; iter < 50 * n + 50; iter++) {
		const w = gradient();
		let best = -1;
		for (let j = 0; j < n; j++) {
			if (!passive[j] && w[j]! > 1e-12 && (best < 0 || w[j]! > w[best]!)) best = j;
		}
		if (best < 0) break;
		passive[best] = true;

		for (;;) {
			const z = lsOnPassive();
			if (passive.every((p, j) => !p || z[j]! > 0)) {
				for (let j = 0; j < n; j++) x[j] = z[j]!;
				break;
			}
			let alpha = Infinity;
			for (let j = 0; j < n; j++) {
				if (passive[j] && z[j]! <= 0) alpha = Math.min(alpha, x[j]! / (x[j]! - z[j]!));
			}
			for (let j = 0; j < n; j++) x[j] = x[j]! + alpha * (z[j]! - x[j]!);
			for (let j = 0; j < n; j++)
				if (passive[j] && x[j]! < 1e-12) ((passive[j] = false), (x[j] = 0));
		}
	}

	return x.map((v, j) => (scale[j]! > 0 ? v / scale[j]! : 0));
}

// #endregion

// #region rows

type Loaded = { meta: Record<string, unknown>; cells: Cell[] };

export function loadCells(): Loaded {
	return JSON.parse(readFileSync(CELLS, 'utf8')) as Loaded;
}

export function decodeRows(cells: Cell[]): DecodeRow[] {
	return cells
		.filter((c) => c.kind === 'decode' && !c.error && c.ms > 0)
		.map((c) => {
			const r = c.region;
			const W = c.width!;
			const H = c.height!;
			return {
				id: c.id,
				src: c.derived ? sourceOf(c.id) : c.id,
				derived: !!c.derived,
				fit: c.fit,
				f: fmt(c.format),
				T: c.ms * 1000,
				den: c.den!,
				tag: c.tag!,
				W,
				H,
				x: r ? r.x : 0,
				y: r ? r.y : 0,
				w: r ? r.w : W,
				h: r ? r.h : H,
				bytes: c.fileBytes!,
				prog: c.progressive ? 1 : 0,
				channels: c.srcChannels ?? 3,
				entropy: c.counters!.entropy
			};
		});
}

export function encodeRows(cells: Cell[]): EncodeRow[] {
	return cells
		.filter((c) => c.kind === 'encode' && !c.error && c.ms > 0)
		.map((c) => ({
			id: c.id,
			fit: c.fit,
			f: fmt(c.outFormat!),
			T: c.ms * 1000,
			N: c.width! * c.height!,
			alpha: c.channels === 2 || c.channels === 4 ? 1 : 0,
			opaque: c.opaque ?? true,
			tag: c.tag!,
			width: c.width!,
			compression: c.compression ?? 'auto'
		}));
}

// #endregion

// #region decode model

/** What the generated header carries for one format's decode. */
export type DecodeSpec = {
	alpha: number[];
	/** microseconds per million entropy bytes, sequential then progressive */
	beta: number[];
	consumed: number[][];
	rows: number;
	stored: number;
};

const denIndex = (den: number): number => DENS.indexOf(den);

/**
 * Samples the decoder traverses. Rule 0 is the region, for a decoder that can skip; rule 1 is the
 * rows through the region's bottom edge, for a stream read top down; rule 2 is the whole frame,
 * for a decoder a region does not shorten. A progressive source is always the whole frame, because
 * its scans cover the image and cannot be read partially (`TinyImageInfo.progressive`).
 */
export function traversed(r: DecodeRow, rows: number): number {
	if (rows === 2 || r.prog) return r.W * r.H;
	return rows ? r.W * Math.min(r.H, r.y + r.h) : r.w * r.h;
}

/** Entropy bytes the decoder consumes, from the consumed table and the region rule. */
export function entropyBytes(
	r: DecodeRow,
	spec: Pick<DecodeSpec, 'consumed' | 'rows' | 'stored'>
): number {
	if (spec.stored && r.bytes * 1000 >= r.W * r.H * r.channels * spec.stored) return 0;

	let b = r.bytes * (spec.consumed[r.prog]![denIndex(r.den)]! / 1000);
	if (spec.rows === 1 && !r.prog) b *= Math.min(r.H, r.y + r.h) / r.H;
	return b;
}

export function decodeMicros(r: DecodeRow, spec: DecodeSpec): number {
	return (
		(spec.alpha[r.prog * 4 + denIndex(r.den)]! * traversed(r, spec.rows)) / 1e6 +
		(spec.beta[r.prog]! * entropyBytes(r, spec)) / 1e6
	);
}

const relLog = (measured: number, predicted: number): number =>
	Math.log(measured / Math.max(predicted, 1e-9));

/** Fits alpha (per den) and beta on `rows`, given everything but the two coefficient sets. */
export function fitCoefficients(
	rows: DecodeRow[],
	base: Pick<DecodeSpec, 'consumed' | 'rows' | 'stored'>,
	withBeta: boolean
): { alpha: number[]; beta: number[] } {
	const a = rows.map((r) => {
		const row = new Array<number>(10).fill(0);
		row[r.prog * 4 + denIndex(r.den)] = traversed(r, base.rows) / r.T;
		row[8 + r.prog] = withBeta ? entropyBytes(r, base) / r.T : 0;
		return row;
	});
	const x = nnls(
		a,
		rows.map(() => 1)
	);
	const alpha = x.slice(0, 8).map((v) => v * 1e6);
	const beta = [x[8]! * 1e6, x[9]! * 1e6];

	// a format with no progressive cells prices a progressive flag like a sequential one
	if (!rows.some((r) => r.prog)) {
		for (let i = 0; i < 4; i++) alpha[4 + i] = alpha[i]!;
		beta[1] = beta[0]!;
	}

	return { alpha, beta };
}

/** Median of the consumed share per progressive flag and denominator, from full-frame cells. */
function consumedTable(rows: DecodeRow[]): number[][] {
	const table = [
		[1000, 1000, 1000, 1000],
		[1000, 1000, 1000, 1000]
	];

	for (const prog of [0, 1]) {
		for (const den of DENS) {
			const shares = rows
				.filter(
					(r) =>
						r.prog === prog &&
						r.den === den &&
						r.tag.startsWith('den') &&
						r.entropy > 0 &&
						r.bytes > 0
				)
				.map((r) => r.entropy / r.bytes);
			if (shares.length) table[prog]![denIndex(den)] = Math.round(median(shares) * 1000);
		}

		// a flag with no cells of its own reads as the other one rather than as a guess
		if (!rows.some((r) => r.prog === prog && r.entropy > 0))
			table[prog] = [...table[1 - prog]!];
	}

	return table;
}

type Choice = { spec: DecodeSpec; score: number; variants: Record<string, number> };

/** Mean absolute log error of `fit` sources predicted by a model fitted without them. */
function leaveOneOut(
	rows: DecodeRow[],
	base: Pick<DecodeSpec, 'consumed' | 'rows' | 'stored'>,
	withBeta: boolean
): number {
	const ids = [...new Set(rows.map((r) => r.src))];
	if (ids.length < 2) return NaN;
	const errors: number[] = [];

	for (const id of ids) {
		const train = rows.filter((r) => r.src !== id);
		const { alpha, beta } = fitCoefficients(train, base, withBeta);
		const spec = { ...base, alpha, beta };
		for (const r of rows.filter((q) => q.src === id)) {
			errors.push(Math.abs(relLog(r.T, decodeMicros(r, spec))));
		}
	}

	return errors.reduce((s, e) => s + e, 0) / errors.length;
}

/**
 * Chooses the region rule and whether the byte term stays, per format, from the fit half only.
 *
 * Both choices are made by leave-one-source-out error on the fit entries, so the conf half stays out
 * of the selection. A byte term has to lower that error by more than 2 percent of itself to stay.
 */
export function chooseDecode(fitRows: DecodeRow[], f: number): Choice {
	const rows = fitRows.filter((r) => r.f === f);
	const stored = rows.some((r) => r.entropy === 0 && r.bytes > 0) ? STORED_FLOOR : 0;
	const consumed = consumedTable(rows.filter((r) => !(stored && r.entropy === 0)));

	const variants: Record<string, number> = {};
	let best: Choice | null = null;

	for (const rule of [0, 1, 2]) {
		const base = { consumed, rows: rule, stored };
		const without = leaveOneOut(rows, base, false);
		const withB = leaveOneOut(rows, base, true);
		variants[`rows${rule}/no-beta`] = without;
		variants[`rows${rule}/beta`] = withB;

		const useBeta = Number.isFinite(withB) && withB < without * 0.98;
		const score = useBeta ? withB : without;
		if (!best || score < best.score) {
			const { alpha, beta } = fitCoefficients(rows, base, useBeta);
			best = { spec: { ...base, alpha, beta: useBeta ? beta : [0, 0] }, score, variants };
		}
	}

	// the v1.1.1 constants are a candidate too, scored by their own error on the fit entries, so a
	// refit that cannot beat them on held-out fit entries does not replace them
	const old = oldSpec(f);
	if (old && rows.length) {
		const score =
			rows.reduce((s, r) => s + Math.abs(relLog(r.T, decodeMicros(r, old))), 0) / rows.length;
		variants['v1.1.1 constants'] = score;
		if (best && score < best.score) best = { spec: old, score, variants };
	}

	return best!;
}

/** The v1.1.1 decode model written as a spec: one rate per den, whole frame, no byte term. */
function oldSpec(f: number): DecodeSpec | null {
	const rate = OLD_DECODE[f];
	const fractions = OLD_FRACTION[f];
	if (!rate || !fractions) return null;

	return {
		...emptySpec(),
		alpha: [...fractions, ...fractions].map((x) => (rate * x) / 1000),
		rows: 2
	};
}

// #endregion

// #region encode model

export type EncodeSpec = {
	/** microseconds per million pixels, opaque then alpha */
	rate: number[];
	compression: { none: number; fast: number; best: number };
};

/** Median microseconds per million pixels, so the point estimate is median-unbiased on its cells. */
function rate(rows: EncodeRow[]): number {
	return median(rows.map((r) => (r.T / r.N) * 1e6));
}

/** Rate over the default-tier cells: `auto` compression, default quality, no lossless flag. */
export function fitEncode(rows: EncodeRow[], f: number): number[] {
	const base = rows.filter((r) => r.f === f && r.tag === FORMATS[f]);
	const opaque = base.filter((r) => !r.alpha);
	const alpha = base.filter((r) => r.alpha);
	const r0 = opaque.length ? rate(opaque) : rate(base);
	const alphaEntries = new Set(alpha.map((r) => r.id)).size;

	// a separate rate needs enough entries of its own to be a measurement; fewer reads the opaque one
	// only WebP has an alpha mechanism (P0d's ALPH chunk); another format's alpha cells are
	// graphics whose content is cheap to code, which is not a property of the channel
	return [
		Math.round(r0),
		f === 6 && alphaEntries >= 4 ? Math.round(rate(alpha)) : Math.round(r0)
	];
}

/** Tier time over default time on the same input, per deflate format, medians over the fit entries. */
export function compressionTiers(rows: EncodeRow[]): Record<string, number> {
	const out: Record<string, number> = {};

	for (const level of ['none', 'fast', 'best']) {
		const ratios: number[] = [];

		for (const name of ['png', 'tiff']) {
			const f = fmt(name);
			const defaults = new Map(
				rows
					.filter((r) => r.f === f && r.tag === name && r.fit)
					.map((r) => [`${r.id}|${r.width}`, r])
			);
			for (const r of rows.filter(
				(q) => q.f === f && q.tag === `${name}-${level}` && q.fit
			)) {
				const d = defaults.get(`${r.id}|${r.width}`);
				if (d) ratios.push(r.T / d.T);
			}
		}

		out[level] = Math.round(median(ratios) * 1000);
	}

	return out;
}

// #endregion

// #region tiers

/** Quality points the tier table is measured at; a request takes the nearest, ties to the dearer. */
export const QUALITY_POINTS = [50, 75, 85, 90, 95];
/** Format rows with a quality tier: JPEG, WebP and AVIF. */
const QUALITY_FORMATS = [2, 6, 7];
/** Pixel counts that start the second and third lossless class; the ratio grows with the extent. */
export const LOSSLESS_CLASSES = [100_000, 1_000_000];
export const losslessClass = (pixels: number): number =>
	LOSSLESS_CLASSES.filter((b) => pixels >= b).length;
/** Row index of lossless WebP in the effort tables, which key on the coding rather than the format. */
export const LOSSLESS_WEBP = 9;
const DEFAULT_QUALITY: Record<number, number> = { 2: 85, 6: 75, 7: 75 };

export type TierRow = {
	id: string;
	fit: boolean;
	f: number;
	tag: string;
	group: string;
	variant: string;
	ratio: number;
	half: number;
	T: number;
	N: number;
};

export type EffortRow = {
	id: string;
	fit: boolean;
	f: number;
	den: number;
	ratio: number;
	half: number;
	ms: number;
	fastMs: number;
};

export function tierRows(cells: Cell[]): TierRow[] {
	return cells
		.filter((c) => c.kind === 'tier' && !c.error && c.ms > 0)
		.map((c) => ({
			id: c.id,
			fit: c.fit,
			f: fmt(c.outFormat!),
			tag: c.tag!,
			group: c.group!,
			variant: c.variant!,
			ratio: c.ratio!,
			half: (c.ratioMax! - c.ratioMin!) / 2,
			T: c.ms * 1000,
			N: c.width! * c.height!
		}));
}

export function effortRows(cells: Cell[]): EffortRow[] {
	return cells
		.filter((c) => c.kind === 'effortdec' && !c.error && c.ms > 0)
		.map((c) => ({
			id: c.id,
			fit: c.fit,
			f: c.format === 'webp' && c.lossless ? LOSSLESS_WEBP : fmt(c.format),
			den: c.den!,
			ratio: c.ratio!,
			half: (c.ratioMax! - c.ratioMin!) / 2,
			ms: c.ms,
			fastMs: c.fastMs!
		}));
}

export type Factor = { value: number; n: number; median: number; half: number; snapped: boolean };

/**
 * A FAST over FANCY factor in thousandths from the fit cells' ratios.
 *
 * The rule: when the median ratio is within the median half range of a cell's own round-by-round
 * ratios of 1, the saving is inside what one cell can resolve and the factor is exactly 1000.
 * Otherwise it is the median ratio. No cells reads as 1000 with n = 0.
 */
export function effortFactor(ratios: number[], halves: number[]): Factor {
	if (ratios.length === 0) return { value: 1000, n: 0, median: NaN, half: NaN, snapped: false };
	const m = median(ratios);
	const h = median(halves);
	const snapped = Math.abs(m - 1) <= h;
	return {
		value: snapped ? 1000 : Math.round(m * 1000),
		n: ratios.length,
		median: m,
		half: h,
		snapped
	};
}

export type Tiers = {
	/** decode FAST fraction by format row (0..9) and denominator */
	effortDecode: Factor[][];
	/** encode FAST fraction by format row (0..9) */
	effortEncode: Factor[];
	/** lossy quality factor by format row and QUALITY_POINTS index */
	quality: number[][];
	/** lossless over default, by format row and extent class; 0 where no lossless mode was measured */
	lossless: number[][];
	/** RGBA over RGB with every alpha 255, then with a ramp, by format row */
	alphaOpaque: number[];
	alphaRamp: number[];
};

const thousandths = (v: number): number => Math.round(v * 1000);

export function fitTiers(cells: Cell[]): Tiers {
	const effort = effortRows(cells).filter((r) => r.fit);
	const tiers = tierRows(cells).filter((r) => r.fit);
	const rows = FORMATS.length + 1;

	const effortDecode = Array.from({ length: rows }, (_, f) =>
		DENS.map((den) => {
			const g = effort.filter((r) => r.f === f && r.den === den);
			return effortFactor(
				g.map((r) => r.ratio),
				g.map((r) => r.half)
			);
		})
	);

	const effortEncode = Array.from({ length: rows }, (_, f) => {
		const g = tiers.filter(
			(r) =>
				r.group === 'effort' &&
				r.variant === 'fast' &&
				(f === LOSSLESS_WEBP
					? r.tag === 'webp-lossless'
					: r.f === f && r.tag === FORMATS[f])
		);
		return effortFactor(
			g.map((r) => r.ratio),
			g.map((r) => r.half)
		);
	});

	const med = (g: TierRow[]): number =>
		g.length ? thousandths(median(g.map((r) => r.ratio))) : 1000;

	const quality = FORMATS.map((_, f) =>
		QUALITY_POINTS.map((q) => {
			if (!QUALITY_FORMATS.includes(f)) return 1000;
			// the default's own point is the same code path as quality 0, so it is the base by definition
			if (q === DEFAULT_QUALITY[f]) return 1000;
			return med(
				tiers.filter(
					(r) =>
						r.group === 'quality' &&
						r.f === f &&
						r.tag === FORMATS[f] &&
						r.variant === `q${q}`
				)
			);
		})
	);

	const losslessCells = tiers.filter(
		(r) =>
			r.f === 6 &&
			r.variant === 'lossless' &&
			((r.group === 'quality' && r.tag === 'webp') || r.group === 'size')
	);
	const lossless = FORMATS.map((_, f) => {
		if (f !== 6) return [0, 0, 0];
		const byClass = [0, 1, 2].map((c) => losslessCells.filter((r) => losslessClass(r.N) === c));
		// a class with no cells reads as the nearest smaller class that has some
		return byClass.map((_g, c) => {
			for (let k = c; k >= 0; k--) if (byClass[k]!.length) return med(byClass[k]!);
			return med(losslessCells);
		});
	});

	const alpha = (variant: string) =>
		FORMATS.map((_, f) =>
			med(
				tiers.filter(
					(r) =>
						r.group === 'alpha' &&
						r.f === f &&
						r.tag === FORMATS[f] &&
						r.variant === variant
				)
			)
		);

	return {
		effortDecode,
		effortEncode,
		quality,
		lossless,
		alphaOpaque: alpha('rgba-opaque'),
		alphaRamp: alpha('rgba-ramp')
	};
}

/** What the encode price needs about a request beyond the extent. */
export type PriceOpts = {
	quality?: number | undefined;
	lossless?: boolean | undefined;
	compression?: string | undefined;
	channels?: number | undefined;
	effort?: number | undefined;
};

/** Index of the quality point nearest `quality`, the dearer one on a tie. */
export function qualityPoint(quality: number): number {
	let best = 0;
	for (let i = 1; i < QUALITY_POINTS.length; i++) {
		if (Math.abs(quality - QUALITY_POINTS[i]!) <= Math.abs(quality - QUALITY_POINTS[best]!))
			best = i;
	}
	return best;
}

/** The encode price in microseconds, mirroring `encode_micros` in src/plan.c. */
export function encodePrice(
	model: Model,
	f: number,
	N: number,
	o: PriceOpts,
	upper: boolean
): number {
	const rate = model.encode[f]![0]!;
	if (!rate) return 0;

	let cost = (N * rate) / 1e6;
	const lossless = !!o.lossless && model.lossless[f]![0]! > 0;

	if (lossless) cost = (cost * model.lossless[f]![losslessClass(N)]!) / 1000;
	else if (QUALITY_FORMATS.includes(f) && o.quality) {
		cost = (cost * model.quality[f]![qualityPoint(o.quality)]!) / 1000;
	}

	if (o.channels === 2 || o.channels === 4) cost = (cost * model.alphaFactor[f]!) / 1000;

	if (f === 1 || f === 5) {
		const c = o.compression ?? 'auto';
		const fraction =
			c === 'none'
				? model.compression.none!
				: c === 'fast'
					? model.compression.fast!
					: c === 'best'
						? model.compression.best!
						: 1000;
		cost = (cost * fraction) / 1000;
	}

	if (o.effort) {
		const row = lossless && f === 6 ? LOSSLESS_WEBP : f;
		cost = (cost * model.effortEncode[row]!) / 1000;
	}

	return upper ? (cost * model.qEncode[f]!) / 1000 : cost;
}

export type EncodeP = {
	id: string;
	fit: boolean;
	f: number;
	T: number;
	N: number;
	/** 'default', 'quality', 'lossless', 'compression', 'alpha' or 'effort' */
	group: string;
	opts: PriceOpts;
};

/** Every encode cell the price function prices, from the A2 cells and the tier cells. */
export function pricedEncodes(cells: Cell[]): EncodeP[] {
	const out: EncodeP[] = [];

	for (const c of cells) {
		if (c.error || c.ms <= 0) continue;

		if (c.kind === 'encode') {
			const f = fmt(c.outFormat!);
			const tag = c.tag!;
			const group =
				tag === FORMATS[f]
					? 'default'
					: tag === 'webp-lossless'
						? 'lossless'
						: /-q\d+$/.test(tag)
							? 'quality'
							: 'compression';
			out.push({
				id: c.id,
				fit: c.fit,
				f,
				T: c.ms * 1000,
				N: c.width! * c.height!,
				group,
				opts: {
					quality: c.quality,
					lossless: tag === 'webp-lossless',
					compression: c.compression,
					channels: c.channels
				}
			});
		} else if (c.kind === 'tier') {
			const v = c.variant!;
			if (c.group === 'effort' && v === 'fancy') continue;
			if (c.group === 'alpha' && v === 'rgb') continue;
			const f = fmt(c.outFormat!);
			out.push({
				id: c.id,
				fit: c.fit,
				f,
				T: c.ms * 1000,
				N: c.width! * c.height!,
				group: c.group!,
				opts: {
					quality: c.quality,
					lossless: !!c.lossless || c.tag === 'webp-lossless',
					channels: c.group === 'alpha' && v !== 'rgb' ? 4 : 3,
					effort: c.group === 'effort' && v === 'fast' ? 1 : 0
				}
			});
		}
	}

	return out;
}

// #endregion

// #region the whole model

export type Model = {
	decode: DecodeSpec[];
	encode: number[][];
	compression: Record<string, number>;
	qDecode: number[];
	qEncode: number[];
	nDecode: number[];
	nEncode: number[];
	noteDecode: string[];
	noteEncode: string[];
	choices: Choice[];
	/** quality factor by format row and QUALITY_POINTS index, thousandths */
	quality: number[][];
	/** lossless over default by extent class, thousandths; zero where there is no lossless mode */
	lossless: number[][];
	/** price of a source with an alpha channel over one without, thousandths */
	alphaFactor: number[];
	alphaChoice: string[];
	/** FAST fraction of a decode by format row 0..9 and denominator, thousandths */
	effortDecode: number[][];
	/** FAST fraction of an encode by format row 0..9, thousandths */
	effortEncode: number[];
	tiers: Tiers;
	legacy: boolean;
};

const emptySpec = (): DecodeSpec => ({
	alpha: [0, 0, 0, 0, 0, 0, 0, 0],
	beta: [0, 0],
	consumed: [
		[1000, 1000, 1000, 1000],
		[1000, 1000, 1000, 1000]
	],
	rows: 0,
	stored: 0
});

/** Multiplier in thousandths from the conformal rank, or null when the group has no conf cells. */
function conformal(residuals: number[]): { q: number; n: number; short: boolean } | null {
	const n = residuals.length;
	if (n === 0) return null;
	const rank = Math.ceil((n + 1) * (1 - MISCOVERAGE));
	return { q: rankQuantile(residuals, rank), n, short: rank > n };
}

/** The decode cells a FAST request is priced on: the same entry and denominator, timed at FAST. */
export function fastDecodeRows(
	cells: Cell[],
	dec: DecodeRow[]
): Array<{ row: DecodeRow; T: number; kind: number }> {
	const byKey = new Map(
		dec.filter((r) => r.tag.startsWith('den')).map((r) => [`${r.id}|${r.den}`, r])
	);
	const out: Array<{ row: DecodeRow; T: number; kind: number }> = [];

	for (const e of effortRows(cells)) {
		const row = byKey.get(`${e.id}|${e.den}`);
		if (row) out.push({ row, T: e.fastMs * 1000, kind: e.f });
	}

	return out;
}

const ALPHA_CANDIDATES = ['opaque', 'ramp', 'geometric mean'];

/**
 * Picks the factor for a source with an alpha channel per format, from the fit half only.
 *
 * Three candidates: the controlled ratio for RGBA with every alpha 255, the one for a ramp, and
 * their geometric mean. Each prices the fit half's real alpha-bearing default cells and the lowest
 * mean absolute log error wins. A format with fewer than 3 such entries takes the ramp ratio, the
 * dearer measured case, because a price that is too low is the unsafe direction for a budget.
 */
function chooseAlpha(model: Model, encodes: EncodeP[]): void {
	for (const f of PRICED) {
		const t = model.tiers;
		const candidates = [
			t.alphaOpaque[f]!,
			t.alphaRamp[f]!,
			Math.round(Math.sqrt(t.alphaOpaque[f]! * t.alphaRamp[f]!))
		];
		const real = encodes.filter(
			(r) =>
				r.fit &&
				r.f === f &&
				r.group === 'default' &&
				(r.opts.channels === 2 || r.opts.channels === 4)
		);

		if (new Set(real.map((r) => r.id)).size < 3) {
			model.alphaFactor[f] = candidates[1]!;
			model.alphaChoice[f] =
				`ramp (${new Set(real.map((r) => r.id)).size} real alpha entries)`;
			continue;
		}

		let best = 0;
		let bestError = Infinity;
		candidates.forEach((c, i) => {
			model.alphaFactor[f] = c;
			const error =
				real.reduce(
					(sum, r) =>
						sum + Math.abs(relLog(r.T, encodePrice(model, f, r.N, r.opts, false))),
					0
				) / real.length;
			if (error < bestError) {
				bestError = error;
				best = i;
			}
		});

		model.alphaFactor[f] = candidates[best]!;
		model.alphaChoice[f] =
			`${ALPHA_CANDIDATES[best]} (error ${bestError.toFixed(3)} on ${real.length} real alpha cells)`;
	}
}

export function fitModel(cells: Cell[], legacy = false): Model {
	const dec = decodeRows(cells);
	const enc = encodeRows(cells);
	const fitDec = dec.filter((r) => r.fit);
	const fitEnc = enc.filter((r) => r.fit);
	const tiers = fitTiers(cells);

	const model: Model = {
		decode: FORMATS.map(emptySpec),
		encode: FORMATS.map(() => [0, 0]),
		compression: compressionTiers(enc),
		qDecode: FORMATS.map(() => 1000),
		qEncode: FORMATS.map(() => 1000),
		nDecode: FORMATS.map(() => 0),
		nEncode: FORMATS.map(() => 0),
		noteDecode: FORMATS.map(() => ''),
		noteEncode: FORMATS.map(() => ''),
		choices: [],
		quality: tiers.quality,
		lossless: tiers.lossless,
		alphaFactor: FORMATS.map(() => 1000),
		alphaChoice: FORMATS.map(() => ''),
		effortDecode: tiers.effortDecode.map((row) => row.map((f) => f.value)),
		effortEncode: tiers.effortEncode.map((f) => f.value),
		tiers,
		legacy
	};

	const pooledDecode: number[] = [];
	const pooledEncode: number[] = [];
	const own = new Map<string, number[]>();

	for (const f of PRICED) {
		if (fitDec.some((r) => r.f === f)) {
			const choice = chooseDecode(fitDec, f);
			model.decode[f] = choice.spec;
			model.choices[f] = choice;
		}
		model.encode[f] = fitEncode(fitEnc, f);
	}

	const priced = pricedEncodes(cells);
	if (!legacy) chooseAlpha(model, priced);
	const fast = fastDecodeRows(cells, dec).filter((x) => !x.row.fit);

	for (const f of PRICED) {
		const d = dec
			.filter((r) => !r.fit && r.f === f)
			.map((r) => relLog(r.T, decodeMicros(r, model.decode[f]!)));
		if (!legacy) {
			for (const x of fast.filter((q) => q.row.f === f)) {
				const fraction = model.effortDecode[x.kind]![denIndex(x.row.den)]!;
				d.push(relLog(x.T, (decodeMicros(x.row, model.decode[f]!) * fraction) / 1000));
			}
		}
		const e = legacy
			? enc
					.filter((r) => !r.fit && r.f === f && r.tag === FORMATS[f])
					.map((r) => relLog(r.T, (model.encode[f]![r.alpha]! * r.N) / 1e6))
			: priced
					.filter((r) => !r.fit && r.f === f)
					.map((r) => relLog(r.T, encodePrice(model, f, r.N, r.opts, false)));
		own.set(`d${f}`, d);
		own.set(`e${f}`, e);
		pooledDecode.push(...d);
		pooledEncode.push(...e);
	}

	const pooledD = conformal(pooledDecode);
	const pooledE = conformal(pooledEncode);

	for (const f of PRICED) {
		for (const stage of ['d', 'e'] as const) {
			const residuals = own.get(`${stage}${f}`)!;
			const own_ = conformal(residuals);
			const pooled = stage === 'd' ? pooledD : pooledE;
			const use = own_ ?? pooled;
			const q = use ? Math.round(Math.exp(Math.max(use.q, 0)) * 1000) : 1000;
			const note = own_
				? `n=${own_.n}${own_.short ? ' (rank over n, maximum used)' : ''}`
				: `n=0, pooled over every format (n=${pooled?.n ?? 0})`;

			if (stage === 'd') {
				model.qDecode[f] = q;
				model.nDecode[f] = own_?.n ?? 0;
				model.noteDecode[f] = note;
			} else {
				model.qEncode[f] = q;
				model.nEncode[f] = own_?.n ?? 0;
				model.noteEncode[f] = note;
			}
		}
	}

	return model;
}

// #endregion

// #region header

const u = (v: number): string => `${Math.round(v)}u`;
const names = PRICED.map((f) => FORMATS[f]!.toUpperCase());

function table(label: string, rows: (f: number) => string): string {
	return `${label} = {\n    {0},\n${PRICED.map((f) => `    /* ${FORMATS[f]} */\n    ${rows(f)},`).join('\n')}\n    {0},\n};`;
}

export function header(model: Model): string {
	const alpha = table('static const uint32_t cost_alpha[9][2][4]', (f) => {
		const a = model.decode[f]!.alpha.map((v) => u(v));
		return `{{${a.slice(0, 4).join(', ')}}, {${a.slice(4).join(', ')}}}`;
	});
	const beta = `static const uint32_t cost_beta[9][2] = {\n    {0u, 0u},\n${PRICED.map(
		(f) =>
			`    /* ${FORMATS[f]} */\n    {${u(model.decode[f]!.beta[0]!)}, ${u(model.decode[f]!.beta[1]!)}},`
	).join('\n')}\n    {0u, 0u},\n};`;
	const rows = `static const uint8_t cost_rows[9] = {0, ${PRICED.map((f) => model.decode[f]!.rows).join(', ')}, 0};`;
	const stored = `static const uint16_t cost_stored[9] = {0, ${PRICED.map((f) => model.decode[f]!.stored).join(', ')}, 0};`;
	const consumed = `static const uint16_t cost_consumed[9][2][4] = {\n    {{0}},\n${PRICED.map(
		(f) =>
			`    /* ${FORMATS[f]} */\n    {{${model.decode[f]!.consumed[0]!.join(', ')}}, {${model.decode[f]!.consumed[1]!.join(', ')}}},`
	).join('\n')}\n    {{0}},\n};`;
	const encode = `static const uint32_t cost_encode[9] = {0u, ${PRICED.map((f) => u(model.encode[f]![0]!)).join(', ')}, 0u};`;
	const row = (list: number[]) => `${list.map((v) => `${Math.round(v)}u`).join(', ')}`;
	const effortDecode = `static const uint16_t cost_effort_decode[10][4] = {\n    {1000u, 1000u, 1000u, 1000u},\n${Array.from(
		{ length: 9 },
		(_, i) => i + 1
	)
		.map(
			(f) =>
				`    /* ${f === LOSSLESS_WEBP ? 'webp lossless' : FORMATS[f]} */\n    {${row(model.effortDecode[f]!)}},`
		)
		.join('\n')}\n};`;
	const effortEncode = `static const uint16_t cost_effort_encode[10] = {${model.effortEncode.map((v) => `${v}u`).join(', ')}};`;
	const qualityPoints = `static const uint8_t cost_quality_points[${QUALITY_POINTS.length}] = {${QUALITY_POINTS.join(', ')}};`;
	const quality = `static const uint16_t cost_quality[9][${QUALITY_POINTS.length}] = {\n    {1000u, 1000u, 1000u, 1000u, 1000u},\n${PRICED.map(
		(f) => `    /* ${FORMATS[f]} */\n    {${row(model.quality[f]!)}},`
	).join('\n')}\n    {1000u, 1000u, 1000u, 1000u, 1000u},\n};`;
	const lossless = `#define COST_LOSSLESS_MEDIUM ${LOSSLESS_CLASSES[0]}u\n#define COST_LOSSLESS_LARGE ${LOSSLESS_CLASSES[1]}u\n\nstatic const uint16_t cost_lossless[9][3] = {\n${model.lossless.map((r) => `    {${r.map((v) => `${v}u`).join(', ')}},`).join('\n')}\n};`;
	const alphaFactor = `static const uint16_t cost_alpha_factor[9] = {1000u, ${PRICED.map((f) => `${model.alphaFactor[f]}u`).join(', ')}, 1000u};`;
	const qd = `static const uint16_t cost_q_decode[9] = {1000, ${PRICED.map((f) => model.qDecode[f]).join(', ')}, 1000};`;
	const qe = `static const uint16_t cost_q_encode[9] = {1000, ${PRICED.map((f) => model.qEncode[f]).join(', ')}, 1000};`;
	const nd = `static const uint16_t cost_n_decode[9] = {0, ${PRICED.map((f) => model.nDecode[f]).join(', ')}, 0};`;
	const ne = `static const uint16_t cost_n_encode[9] = {0, ${PRICED.map((f) => model.nEncode[f]).join(', ')}, 0};`;

	const text = `/*
 * Generated from bench/baselines/fit/cells.json by scripts/cost-model.ts. Do
 * not hand edit. Rates are microseconds per million samples on the machine the
 * cells were timed on; a multiplier or fraction is in thousandths. Effort rows
 * are the format's value, with row 9 for lossless WebP.
 */

#define COST_COMPRESSION_NONE ${u(model.compression.none!)}
#define COST_COMPRESSION_FAST ${u(model.compression.fast!)}
#define COST_COMPRESSION_BEST ${u(model.compression.best!)}

${alpha}

${beta}

${rows}

${stored}

${consumed}

${encode}

${effortDecode}

${effortEncode}

${qualityPoints}

${quality}

${lossless}

${alphaFactor}

${qd}

${qe}

${nd}

${ne}
`;

	return clangFormat(text);
}

function clangFormat(text: string): string {
	return execFileSync('clang-format', [`--assume-filename=${HEADER}`], {
		input: text,
		encoding: 'utf8'
	});
}

// #endregion

// #region report

type Stats = {
	n: number;
	median: number;
	p05: number;
	p95: number;
	under: number;
	absMedian: number;
};

function signed(values: number[]): Stats {
	return {
		n: values.length,
		median: median(values),
		p05: interpolated(values, 0.05),
		p95: interpolated(values, 0.95),
		under: Math.min(...values),
		absMedian: median(values.map(Math.abs))
	};
}

/** The v1.1.1 model's constants, for the old-against-new comparison. */
const OLD_DECODE: Record<number, number> = {
	3: 5668,
	4: 6687,
	2: 8004,
	1: 12977,
	6: 13673,
	5: 14328,
	7: 26461
};
const OLD_FRACTION: Record<number, number[]> = {
	2: [1000, 350, 290, 203],
	3: [1000, 527, 384, 358],
	1: [1000, 1619, 1559, 1547],
	6: [1000, 1024, 993, 975],
	4: [1000, 953, 882, 872],
	5: [1000, 847, 811, 804],
	7: [1000, 919, 879, 865]
};
const OLD_ENCODE: Record<number, number> = {
	3: 7900,
	2: 17000,
	4: 50000,
	6: 39000,
	5: 240000,
	1: 480000
};

export function oldDecode(r: DecodeRow): number {
	return (((r.W * r.H * OLD_DECODE[r.f]!) / 1e6) * OLD_FRACTION[r.f]![denIndex(r.den)]!) / 1000;
}

/** The A2 encode price: the default rate, alpha rate for an alpha source, and the compression tier. */
function legacyEncode(model: Model, r: EncodeP): number {
	const alpha = r.opts.channels === 2 || r.opts.channels === 4 ? 1 : 0;
	let cost = (model.encode[r.f]![alpha]! * r.N) / 1e6;

	if (r.f === 1 || r.f === 5) {
		const c = r.opts.compression ?? 'auto';
		const k = c === 'none' ? 'none' : c === 'fast' ? 'fast' : c === 'best' ? 'best' : '';
		if (k) cost = (cost * model.compression[k]!) / 1000;
	}

	return cost;
}

export type Models = { a2: Model; next: Model };

export function report(cells: Cell[], models: Models): string {
	const { a2, next } = models;
	const dec = decodeRows(cells).filter((r) => !r.fit);
	const priced = pricedEncodes(cells).filter((r) => !r.fit);
	const out: string[] = [];
	const pct = (v: number) => `${(v * 100).toFixed(0)}%`;
	const line = (name: string, s: Stats) =>
		`| ${name} | ${s.n} | ${pct(s.median)} | ${pct(s.p05)} | ${pct(s.p95)} | ${pct(s.under)} | ${pct(s.absMedian)} |`;
	const head = (cols: string) => {
		out.push(cols);
		out.push(
			`| ${cols
				.split('|')
				.slice(1, -1)
				.map(() => '---')
				.join(' | ')} |`
		);
	};
	const cover = (values: number[], q: number) =>
		values.length ? values.filter((v) => v <= q).length / values.length : NaN;

	out.push(
		'Decode, signed error of the point estimate on conf cells, predicted / measured - 1:\n'
	);
	head('| group | n | median | p05 | p95 | worst under | median abs |');

	for (const f of PRICED) {
		const groups: Array<[string, DecodeRow[]]> = [
			['', dec.filter((r) => r.f === f)],
			['derived only', dec.filter((r) => r.f === f && r.derived)]
		];
		for (const [label, d] of groups) {
			if (!d.length || (label && f !== 4 && f !== 5)) continue;
			const name = `${FORMATS[f]} decode${label ? ` (${label})` : ''}`;
			out.push(line(`${name} old`, signed(d.map((r) => oldDecode(r) / r.T - 1))));
			out.push(
				line(`${name} A2`, signed(d.map((r) => decodeMicros(r, a2.decode[f]!) / r.T - 1)))
			);
			out.push(
				line(
					`${name} A2b`,
					signed(d.map((r) => decodeMicros(r, next.decode[f]!) / r.T - 1))
				)
			);
		}
	}

	out.push(
		'\nEncode, signed error of the point estimate on conf cells, predicted / measured - 1:\n'
	);
	head('| group | n | median | p05 | p95 | worst under | median abs |');

	for (const f of PRICED) {
		const sets: Array<[string, EncodeP[]]> = [
			['default', priced.filter((r) => r.f === f && r.group === 'default')],
			['non-default', priced.filter((r) => r.f === f && r.group !== 'default')]
		];
		for (const [label, e] of sets) {
			if (!e.length) continue;
			const name = `${FORMATS[f]} encode ${label}`;
			if (OLD_ENCODE[f] && label === 'default') {
				out.push(
					line(
						`${name} old`,
						signed(e.map((r) => (OLD_ENCODE[f]! * r.N) / 1e6 / r.T - 1))
					)
				);
			}
			out.push(line(`${name} A2`, signed(e.map((r) => legacyEncode(a2, r) / r.T - 1))));
			out.push(
				line(
					`${name} A2b`,
					signed(e.map((r) => encodePrice(next, f, r.N, r.opts, false) / r.T - 1))
				)
			);
		}
	}

	out.push('\nCoverage of the bound on conf cells (measured <= upper), A2 against A2b:\n');
	head('| group | n | A2 q (n) | A2 coverage | A2b q (n) | A2b coverage |');

	for (const f of PRICED) {
		const sets: Array<[string, DecodeRow[]]> = [
			['', dec.filter((r) => r.f === f)],
			['derived only', dec.filter((r) => r.f === f && r.derived)]
		];
		for (const [label, d] of sets) {
			if (!d.length || (label && f !== 4 && f !== 5)) continue;
			const ra = d.map((r) => r.T / decodeMicros(r, a2.decode[f]!));
			const rn = d.map((r) => r.T / decodeMicros(r, next.decode[f]!));
			out.push(
				`| ${FORMATS[f]} decode${label ? ` (${label})` : ''} | ${d.length} | ${(a2.qDecode[f]! / 1000).toFixed(3)} (${a2.nDecode[f]}) | ${cover(ra, a2.qDecode[f]! / 1000).toFixed(3)} | ${(next.qDecode[f]! / 1000).toFixed(3)} (${next.nDecode[f]}) | ${cover(rn, next.qDecode[f]! / 1000).toFixed(3)} |`
			);
		}

		const encSets: Array<[string, EncodeP[]]> = [
			['all', priced.filter((r) => r.f === f)],
			['default', priced.filter((r) => r.f === f && r.group === 'default')],
			['non-default', priced.filter((r) => r.f === f && r.group !== 'default')]
		];
		for (const [label, e] of encSets) {
			if (!e.length) continue;
			const ra = e.map((r) => r.T / legacyEncode(a2, r));
			const rn = e.map((r) => r.T / encodePrice(next, f, r.N, r.opts, false));
			out.push(
				`| ${FORMATS[f]} encode ${label} | ${e.length} | ${(a2.qEncode[f]! / 1000).toFixed(3)} (${a2.nEncode[f]}) | ${cover(ra, a2.qEncode[f]! / 1000).toFixed(3)} | ${(next.qEncode[f]! / 1000).toFixed(3)} (${next.nEncode[f]}) | ${cover(rn, next.qEncode[f]! / 1000).toFixed(3)} |`
			);
		}
	}

	out.push('\nEncode coverage of A2b by group, per format (non-default groups only):\n');
	head('| format | group | n | coverage |');
	for (const f of PRICED) {
		for (const group of ['quality', 'lossless', 'compression', 'alpha', 'effort']) {
			const e = priced.filter((r) => r.f === f && r.group === group);
			if (!e.length) continue;
			const rn = e.map((r) => r.T / encodePrice(next, f, r.N, r.opts, false));
			out.push(
				`| ${FORMATS[f]} | ${group} | ${e.length} | ${cover(rn, next.qEncode[f]! / 1000).toFixed(3)} |`
			);
		}
	}

	out.push(
		'\nDecode FAST over FANCY from the fit half (n cells, median ratio, median half range, factor):\n'
	);
	head('| format | den | n | median | half range | factor | rule |');
	next.tiers.effortDecode.forEach((row, f) => {
		row.forEach((x, i) => {
			if (!x.n) return;
			out.push(
				`| ${f === LOSSLESS_WEBP ? 'webp lossless' : FORMATS[f]} | ${DENS[i]} | ${x.n} | ${x.median.toFixed(3)} | ${x.half.toFixed(3)} | ${x.value} | ${x.snapped ? 'within half range, 1.0' : 'median'} |`
			);
		});
	});

	out.push('\nEncode FAST over FANCY from the fit half:\n');
	head('| format | n | median | half range | factor | rule |');
	next.tiers.effortEncode.forEach((x, f) => {
		if (!x.n) return;
		out.push(
			`| ${f === LOSSLESS_WEBP ? 'webp lossless' : FORMATS[f]} | ${x.n} | ${x.median.toFixed(3)} | ${x.half.toFixed(3)} | ${x.value} | ${x.snapped ? 'within half range, 1.0' : 'median'} |`
		);
	});

	out.push(`\nQuality points ${QUALITY_POINTS.join(' ')} (factor over default, thousandths):\n`);
	head('| format | factors | lossless |');
	for (const f of QUALITY_FORMATS) {
		out.push(
			`| ${FORMATS[f]} | ${next.quality[f]!.join(' ')} | ${next.lossless[f]![0] ? next.lossless[f]!.join(' / ') : '-'} |`
		);
	}

	out.push('\nAlpha factor (RGBA over RGB), controlled ratios and the choice:\n');
	head('| format | opaque | ramp | chosen | choice |');
	for (const f of PRICED) {
		out.push(
			`| ${FORMATS[f]} | ${next.tiers.alphaOpaque[f]} | ${next.tiers.alphaRamp[f]} | ${next.alphaFactor[f]} | ${next.alphaChoice[f]} |`
		);
	}

	out.push(
		'\nStructure choices from the fit half, leave-one-source-out mean absolute log error:\n'
	);
	head('| format | variant | error |');
	for (const f of PRICED) {
		const c = next.choices[f];
		if (!c) continue;
		for (const [k, v] of Object.entries(c.variants))
			out.push(`| ${FORMATS[f]} | ${k} | ${v.toFixed(4)} |`);
		out.push(
			`| ${FORMATS[f]} | chosen | rows=${c.spec.rows} beta=${c.spec.beta.map((b) => b.toFixed(0)).join('/')} score ${c.score.toFixed(4)} |`
		);
	}

	return out.join('\n');
}

// #endregion

if (import.meta.main) {
	const { cells } = loadCells();
	const model = fitModel(cells);
	const text = header(model);

	if (process.argv.includes('--report')) {
		const a2 = fitModel(
			cells.filter((c) => !c.derived && c.kind !== 'tier' && c.kind !== 'effortdec'),
			true
		);
		console.log(report(cells, { a2, next: model }));
		console.log(`\nnames: ${names.join(' ')}`);
	} else if (process.argv.includes('--check')) {
		const committed = readFileSync(HEADER, 'utf8');
		if (committed !== text) {
			console.error(
				'src/cost-model.h does not match bench/baselines/fit/cells.json; run bun scripts/cost-model.ts'
			);
			process.exit(1);
		}
		console.log('cost-model.h matches its cells');
	} else {
		writeFileSync(HEADER, text);
		console.log(`wrote ${HEADER}`);
	}
}
