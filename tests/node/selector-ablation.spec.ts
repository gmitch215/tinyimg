import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import wasm from '../../bin/tinyimg.wasm?bin';
import {
	agrees,
	analyze,
	ARMS,
	bootstrapDifference,
	bytesRatio,
	interval95,
	isServed,
	type Outcome,
	pairUp,
	type ProbeLine,
	psnr,
	referenceSelection,
	rng,
	selectionFromProbe,
	type SelFile,
	servedCounts,
	summaryMarkdown
} from '../../scripts/measure/selector-ablation.js';
import type { Selection } from '../../src/ts/index.js';
import { Image, TinyImgModule } from '../../src/ts/index.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array<ArrayBuffer> {
	const buffer = readFileSync(join(fixtures, name));
	const out = new Uint8Array(buffer.byteLength);

	out.set(buffer);
	return out;
}

/** One unit at one constraint, with each arm's served flag and bytes. */
function triple(
	unit: string,
	served: [boolean, boolean, boolean],
	bytes: [number | null, number | null, number | null] = [null, null, null],
	over: Partial<Outcome> = {}
): Outcome[] {
	return ARMS.map((arm, i) => ({
		unit,
		id: unit.split('@')[0]!,
		format: 'jpeg',
		budgetMs: 10,
		minDb: 0,
		arm,
		feasible: served[i]!,
		served: served[i]!,
		bytes: bytes[i]!,
		excluded: false,
		...over
	}));
}

describe('isServed', () => {
	it('needs feasible, T at or under B, and the floor met when there is one', () => {
		expect(isServed(true, 10000, 10, 0, null)).toBe(true);
		expect(isServed(true, 10000.1, 10, 0, null)).toBe(false);
		expect(isServed(false, 1, 10, 0, null)).toBe(false);
		expect(isServed(true, null, 10, 0, null)).toBe(false);
		expect(isServed(true, 5000, 10, 35, 35)).toBe(true);
		expect(isServed(true, 5000, 10, 35, 34.99)).toBe(false);
		expect(isServed(true, 5000, 10, 35, null)).toBe(false);
	});
});

describe('pairUp', () => {
	it('keeps complete triples and drops one with an arm missing or timed under load', () => {
		const ok = triple('a@200', [true, true, true]);
		const loaded = triple('b@200', [true, true, true]).map((o, i) => ({
			...o,
			excluded: i === 1
		}));
		const partial = triple('c@200', [true, true, true]).slice(0, 2);
		const { pairs, excluded, incomplete } = pairUp([...ok, ...loaded, ...partial]);

		expect(pairs.map((p) => p.unit)).toEqual(['a@200']);
		expect(excluded).toBe(1);
		expect(incomplete).toBe(1);
	});

	it('keeps the same unit at two constraints as two pairs', () => {
		const both = [
			...triple('a@200', [true, false, false]),
			...triple('a@200', [false, false, false], [null, null, null], { minDb: 35 })
		];

		expect(pairUp(both).pairs).toHaveLength(2);
	});
});

describe('analyze', () => {
	// served by joint, ladder, independent
	const four = [
		...triple('u1@200', [true, true, false], [100, 200, null]),
		...triple('u2@200', [true, false, false]),
		...triple('u3@200', [false, false, false]),
		...triple('u4@200', [true, false, true], [300, null, 600])
	];

	it('counts served, rescues and regressions by hand on four units', () => {
		const a = analyze(four, 200);

		expect(a.pairs).toBe(4);
		expect(a.units).toBe(4);
		expect(a.served).toEqual({ pairs: 4, joint: 3, ladder: 1, independent: 1 });
		expect(a.rescues).toEqual({ ladder: 2, independent: 2 });
		expect(a.regressions).toEqual({ ladder: 0, independent: 0 });
		expect(a.jointMinusLadder.estimate).toBe(0.5);
		expect(a.jointMinusIndependent.estimate).toBe(0.5);
	});

	it('takes the bytes ratio over the cells both arms served', () => {
		const a = analyze(four, 200);

		expect(a.bytes.ladder).toEqual({ n: 1, median: 0.5, p10: 0.5, p90: 0.5, total: 0.5 });
		expect(a.bytes.independent).toEqual({ n: 1, median: 0.5, p10: 0.5, p90: 0.5, total: 0.5 });
		expect(bytesRatio(pairUp(triple('x@1', [true, false, false])).pairs, 'ladder')).toBeNull();
	});

	it('pools served over pairs rather than averaging unit rates', () => {
		// unit a has two pairs, joint serves both and ladder neither; unit b has one, the reverse
		const pooled = [
			...triple('a@200', [true, false, false]),
			...triple('a@200', [true, false, false], [null, null, null], { minDb: 35 }),
			...triple('b@200', [false, true, false])
		];
		const { pairs } = pairUp(pooled);

		expect(servedCounts(pairs)).toEqual({ pairs: 3, joint: 2, ladder: 1, independent: 0 });
		expect(
			bootstrapDifference(pairs, 'joint', 'ladder', (p) => p.unit, 1, 10).estimate
		).toBeCloseTo(1 / 3, 12);
	});

	it('gives a degenerate interval when every unit differs the same way', () => {
		const same = [
			...triple('a@200', [true, false, false]),
			...triple('b@200', [true, false, false])
		];
		const a = analyze(same, 500);

		expect(a.jointMinusLadder).toMatchObject({ estimate: 1, lo: 1, hi: 1 });
	});

	it('gives [0, 0] on a tie in every unit', () => {
		const tie = [
			...triple('a@200', [true, true, true]),
			...triple('b@200', [false, false, false])
		];
		const a = analyze(tie, 500);

		expect(a.jointMinusLadder).toMatchObject({ estimate: 0, lo: 0, hi: 0 });
		expect(a.rescues).toEqual({ ladder: 0, independent: 0 });
	});

	it('bootstraps two units with differences 1 and 0 to the interval [0, 1]', () => {
		// resamples of two units give 0, 0.5 or 1 with probability 1/4, 1/2 and 1/4
		const two = [
			...triple('a@200', [true, false, false]),
			...triple('b@200', [false, false, false])
		];
		const { pairs } = pairUp(two);
		const i = bootstrapDifference(pairs, 'joint', 'ladder', (p) => p.unit);

		expect(i.estimate).toBe(0.5);
		expect(i.lo).toBe(0);
		expect(i.hi).toBe(1);
		expect(i.resamples).toBe(10000);
	});

	it('is deterministic for the fixed seed and differs for another', () => {
		const { pairs } = pairUp(four);
		const run = (seed: number) =>
			bootstrapDifference(pairs, 'joint', 'ladder', (p) => p.unit, seed, 300);

		expect(run(7)).toEqual(run(7));
		expect(rng(1)()).toBe(rng(1)());
		expect(rng(1)()).not.toBe(rng(2)());
	});

	it('returns an empty answer for no pairs without throwing', () => {
		const a = analyze([], 100);

		expect(a.pairs).toBe(0);
		expect(a.jointMinusLadder).toEqual({ estimate: 0, lo: 0, hi: 0, resamples: 100 });
		expect(a.bytes.ladder).toBeNull();
	});

	it('groups by constraint and source format', () => {
		const rows = [
			...triple('a@200', [true, false, false], [null, null, null], { budgetMs: 20 }),
			...triple('b@200', [true, true, false], [null, null, null], { format: 'png' }),
			...triple('c@200', [false, false, false], [null, null, null], { minDb: 35 })
		];
		const a = analyze(rows, 50);

		expect(a.byConstraint.map(([k]) => k)).toEqual([
			'10 ms, 35 dB floor',
			'10 ms, no floor',
			'20 ms, no floor'
		]);
		expect(a.byFormat.map(([k]) => k)).toEqual(['jpeg', 'png']);
	});
});

describe('interval95', () => {
	it('takes sorted indices 250 and 9749 of 10000 values', () => {
		const values = Array.from({ length: 10000 }, (_, i) => 9999 - i);

		expect(interval95(values)).toEqual([250, 9749]);
	});

	it('takes indices 5 and 194 of 200 values', () => {
		expect(interval95(Array.from({ length: 200 }, (_, i) => i))).toEqual([5, 194]);
	});
});

describe('psnr', () => {
	it('is 99 for identical buffers and 3.01 dB when half the squared range is lost', () => {
		expect(psnr(new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 3]))).toBe(99);
		expect(psnr(new Uint8Array([0, 0]), new Uint8Array([0, 255]))).toBeCloseTo(3.0103, 4);
	});
});

describe('probe lines', () => {
	const line: ProbeLine = {
		image: 'x.jpg',
		width: 400,
		budget_us: 10000,
		min_db: 0,
		arm: 'joint',
		feasible: 1,
		reason: 0,
		candidate: 3,
		format: 'jpeg',
		quality: 50,
		lossless: 0,
		cost_upper_us: 8453,
		peak: 444848,
		db: 21.2716,
		db_known: 1,
		db_median: 27.2186,
		bytes_per_pixel: 0.1091,
		bytes_known: 1,
		predicted_bytes: 9819,
		out_width: 400,
		out_height: 225,
		alpha: 0,
		lattice: 8,
		considered: 144
	};

	const wasmChoice = (over: Partial<Selection['choice'] & object> = {}): Selection => ({
		feasible: true,
		reason: 'none',
		choice: {
			format: 'jpeg',
			quality: 50,
			lossless: false,
			reason: 'none',
			costUpperMs: 8.453,
			peakBytes: 444848,
			db: 21.272,
			bytes: 9819,
			candidate: 3,
			dbMedian: 27.219,
			...over
		},
		width: 400,
		height: 225,
		alpha: false,
		coverage: { lever: 0, encode: 0 },
		considered: []
	});

	it('builds a selection from a feasible line and refuses an infeasible one', () => {
		const s = selectionFromProbe(line);

		expect(s.feasible).toBe(true);
		expect(s.choice).toMatchObject({
			format: 'jpeg',
			quality: 50,
			lossless: false,
			candidate: 3,
			db: 21.2716,
			bytes: 9819
		});
		expect(s.width).toBe(400);
		expect(() => selectionFromProbe({ ...line, feasible: 0 })).toThrow('not feasible');
	});

	it('leaves dB and bytes undefined when the probe did not know them', () => {
		const s = selectionFromProbe({ ...line, db_known: 0, bytes_known: 0 });

		expect(s.choice!.db).toBeUndefined();
		expect(s.choice!.bytes).toBeUndefined();
	});

	it('agrees on cell 1, where the module reports the same choice', () => {
		expect(agrees(line, wasmChoice())).toEqual([]);
	});

	it('names every field that differs on cell 2', () => {
		const other = wasmChoice({ candidate: 4, format: 'webp', quality: 75, lossless: true });

		expect(agrees(line, other).sort()).toEqual(['candidate', 'format', 'lossless', 'quality']);
		expect(agrees(line, wasmChoice({ costUpperMs: 8.46 }))).toEqual(['cost_upper']);
		expect(agrees(line, wasmChoice({ db: 22 }))).toEqual(['db']);
		expect(agrees(line, wasmChoice({ bytes: 9820 }))).toEqual(['bytes']);
	});

	it('compares an infeasible search by its reason and a feasibility mismatch by that alone', () => {
		const none: ProbeLine = { ...line, feasible: 0, reason: 6 };
		const infeasible = (reason: Selection['reason']): Selection => ({
			feasible: false,
			reason,
			choice: undefined,
			width: 400,
			height: 225,
			alpha: false,
			coverage: { lever: 0, encode: 0 },
			considered: []
		});

		expect(agrees(none, infeasible('budget'))).toEqual([]);
		expect(agrees(none, infeasible('quality'))).toEqual(['reason']);
		expect(agrees(none, wasmChoice())).toEqual(['feasible']);
	});

	it('names the reference as candidate 0 written losslessly as PNG', () => {
		const s = referenceSelection(400, 225);

		expect(s.choice).toMatchObject({ format: 'png', lossless: true, candidate: 0 });
	});
});

describe('the module and the probe line round trip', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(wasm);
	});

	/** What the native probe would print for the module's own choice. */
	function lineOf(sel: Selection, budgetMs: number): ProbeLine {
		const c = sel.choice!;

		return {
			image: 'sf-24.jpg',
			width: sel.width,
			budget_us: budgetMs * 1000,
			min_db: 0,
			arm: 'joint',
			feasible: 1,
			reason: 0,
			candidate: c.candidate,
			format: c.format,
			quality: c.quality,
			lossless: c.lossless ? 1 : 0,
			cost_upper_us: Math.round(c.costUpperMs * 1000),
			peak: c.peakBytes,
			db: c.db ?? 0,
			db_known: c.db === undefined ? 0 : 1,
			db_median: c.dbMedian ?? 0,
			bytes_per_pixel: 0,
			bytes_known: c.bytes === undefined ? 0 : 1,
			predicted_bytes: c.bytes ?? 0,
			out_width: sel.width,
			out_height: sel.height,
			alpha: sel.alpha ? 1 : 0,
			lattice: 0,
			considered: sel.considered.length
		};
	}

	it.each([
		['sf-24.jpg', 200, 50],
		['sf-24.jpg', 400, 100]
	])(
		'runs %s at %d px, %d ms the same from a built selection as from the original',
		async (name, width, budget) => {
			using image = await Image.open(tinyimg, fixture(name));
			image.resize(width, 0);

			const sel = image.select({ budgetMs: budget, formats: ['jpeg', 'webp'] });
			expect(sel.feasible).toBe(true);

			const line = lineOf(sel, budget);
			const built = selectionFromProbe(line);

			expect(agrees(line, sel)).toEqual([]);
			expect(built.choice!.candidate).toBe(sel.choice!.candidate);
			expect(built.choice!.format).toBe(sel.choice!.format);

			const direct = await image.bytesFor(sel);
			const fromLine = await image.bytesFor(built);

			expect(Buffer.compare(fromLine, direct)).toBe(0);
			expect(direct.byteLength).toBeGreaterThan(0);
		}
	);

	it('writes the reference at candidate 0 as a PNG', async () => {
		using image = await Image.open(tinyimg, fixture('sf-24.jpg'));
		image.resize(200, 0);

		const sel = image.select({ budgetMs: 50, formats: ['jpeg', 'webp'] });
		const bytes = await image.bytesFor(referenceSelection(sel.width, sel.height));

		expect([...bytes.slice(0, 4)]).toEqual([0x89, 0x50, 0x4e, 0x47]);
	});
});

describe('summary', () => {
	const file = (evaluation: boolean, outcomes: Outcome[]): SelFile => ({
		meta: {
			machine: 'test',
			cores: 12,
			load_limit: 3,
			wasm: 'a.wasm',
			wasm_sha256: 'aa',
			node: 'v0',
			probe_source_sha256: 'bb',
			library: 'build-a4/lib/libtinyimg.a',
			library_sha256: 'cc',
			mode: 'timed',
			evaluation,
			subset: evaluation ? null : '--only x',
			budgets_ms: [10],
			min_dbs: [0],
			seed: 1,
			resamples: 50,
			runs: 9,
			warmups: 2,
			units: 2,
			alpha_units: [],
			load_start: 1,
			load_end: 1
		},
		agreement: { compared: 2, disagreements: [] },
		outcomes,
		runs: [],
		controls: [],
		errors: []
	});
	const rows = [
		...triple('a@200', [true, false, false]),
		...triple('b@200', [false, false, false])
	];

	it('labels a subset run and reports the agreement count', () => {
		const text = summaryMarkdown(file(false, rows));

		expect(text).toContain('not the evaluation');
		expect(text).toContain('2 (unit, constraint) joint choices compared, 0 disagreements');
		expect(text).toContain('Joint minus ladder');
	});

	it('does not label the evaluation as a subset run', () => {
		expect(summaryMarkdown(file(true, rows))).not.toContain('not the evaluation');
	});

	it('says nothing was executed for an agreement-only file', () => {
		const text = summaryMarkdown({
			...file(false, []),
			meta: { ...file(false, []).meta, mode: 'agreement-only' }
		});

		expect(text).toContain('Agreement check only');
		expect(text).toContain('No outcomes');
	});

	it('is a pure function of the file', () => {
		const f = file(true, rows);

		expect(summaryMarkdown(f)).toBe(summaryMarkdown(structuredClone(f)));
	});
});
