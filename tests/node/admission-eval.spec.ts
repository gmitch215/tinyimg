import { describe, expect, it } from 'vitest';
import {
	type AdmCell,
	type AdmFile,
	breakdown,
	controlSpread,
	eligible,
	falseRejectRate,
	gate,
	outputFormat,
	overrunRate,
	summaryMarkdown,
	tally
} from '../../scripts/measure/admission-eval.js';

type Spec = {
	t: number;
	fixedB: number;
	fixedA?: number;
	newAdmit: number[];
	budgets: number[];
} & Partial<AdmCell>;

/** One cell: T and the FIXED estimate in microseconds, the budgets NEW admits at in milliseconds. */
function cell(s: Spec): AdmCell {
	const decision = Object.fromEntries(
		s.budgets.map((b) => [String(b), s.newAdmit.includes(b) ? 'admit' : 'reject'])
	) as AdmCell['new_decision'];
	const { t, fixedB, fixedA, newAdmit, budgets, ...rest } = s;
	void newAdmit;
	void budgets;

	return {
		id: 'x',
		split: 'test',
		format: 'jpeg',
		source_mpx: 1,
		width: 400,
		output: 'source',
		status: 'ok',
		reason: null,
		alpha: false,
		measured_source: 'timed',
		measured_us: t,
		samples_us: [t],
		load_before: 1,
		load_after: 1,
		load_excluded: false,
		fixed_a_us: fixedA ?? fixedB - 1000,
		fixed_b_us: fixedB,
		new_cost_us: 0,
		new_upper_us: 0,
		new_decision: decision,
		new_q80_upper_us: 0,
		new_q80_decision: decision,
		...rest
	};
}

const B = [10, 20];

/** Four cells whose counts below were worked out by hand. */
const four = [
	cell({ t: 8000, fixedB: 6000, newAdmit: [10, 20], budgets: B }),
	cell({ t: 15000, fixedB: 9000, newAdmit: [20], budgets: B }),
	cell({ t: 25000, fixedB: 12000, newAdmit: [], budgets: B }),
	cell({ t: 9000, fixedB: 11000, newAdmit: [20], budgets: B })
];

describe('tally', () => {
	it('counts NEW by hand on four cells at 10 and 20 ms', () => {
		expect(tally(four, 'new', B)).toEqual({
			pairs: 8,
			admitted: 4,
			overruns: 0,
			rejected: 4,
			falseRejects: 1,
			fit: 5
		});
	});

	it('counts FIXED by hand on the same cells', () => {
		expect(tally(four, 'fixed', B)).toEqual({
			pairs: 8,
			admitted: 6,
			overruns: 2,
			rejected: 2,
			falseRejects: 1,
			fit: 5
		});
	});

	it('counts FIXED A from its own estimate', () => {
		expect(tally(four, 'fixedA', B)).toEqual({
			pairs: 8,
			admitted: 7,
			overruns: 2,
			rejected: 1,
			falseRejects: 0,
			fit: 5
		});
	});

	it('takes a request exactly at the budget as fitting and a FIXED estimate at it as admitted', () => {
		const at = cell({ t: 10000, fixedB: 10000, newAdmit: [10], budgets: [10] });

		expect(tally([at], 'new', [10])).toMatchObject({ admitted: 1, overruns: 0, fit: 1 });
		expect(tally([at], 'fixed', [10])).toMatchObject({ admitted: 1, overruns: 0, fit: 1 });

		const over = cell({ t: 10000.1, fixedB: 10001, newAdmit: [10], budgets: [10] });
		expect(tally([over], 'new', [10])).toMatchObject({ admitted: 1, overruns: 1, fit: 0 });
		expect(tally([over], 'fixed', [10])).toMatchObject({ admitted: 0, falseRejects: 0 });
	});

	it('reads the rates from the counts', () => {
		const n = tally(four, 'fixed', B);

		expect(overrunRate(n)).toBeCloseTo(2 / 6, 12);
		expect(falseRejectRate(n)).toBeCloseTo(1 / 5, 12);
		expect(overrunRate(tally([], 'new'))).toBeNull();
		expect(falseRejectRate(tally([], 'new'))).toBeNull();
	});
});

describe('eligible', () => {
	it('drops a cell recorded under load, an alpha source, a failure, a cal cell and a missing arm', () => {
		const base = { t: 5000, fixedB: 5000, newAdmit: [10], budgets: [10] };

		expect(eligible(cell(base))).toBe(true);
		expect(eligible(cell({ ...base, load_excluded: true }))).toBe(false);
		expect(eligible(cell({ ...base, alpha: true }))).toBe(false);
		expect(eligible(cell({ ...base, status: 'failed' }))).toBe(false);
		expect(eligible(cell({ ...base, split: 'cal' }))).toBe(false);
		expect(eligible(cell({ ...base, fixed_b_us: null }))).toBe(false);
		expect(eligible(cell({ ...base, measured_us: null }))).toBe(false);
		expect(eligible(cell({ ...base, new_decision: {} }))).toBe(false);
	});

	it('leaves a cell marked as measured under load out of every count', () => {
		const loaded = cell({
			t: 50000,
			fixedB: 1000,
			newAdmit: [10, 20],
			budgets: B,
			load_excluded: true
		});
		const g = gate([...four, loaded], B);

		expect(g.pairs).toBe(8);
		expect(g.new.overruns).toBe(0);
	});
});

describe('gate', () => {
	it('passes on the four cells: NEW 0 of 4, FIXED 2 overruns, ratio infinite', () => {
		const g = gate(four, B);

		expect(g.new.overruns).toBe(0);
		expect(g.fixed.overruns).toBe(2);
		expect(g.ratio).toBe(Number.POSITIVE_INFINITY);
		expect(g.newRatePass).toBe(true);
		expect(g.ratioPass).toBe(true);
		expect(g.pass).toBe(true);
		expect(g.reason).toBe('');
	});

	/**
	 * n cells NEW admits at 10 ms, `slow` of them over it, FIXED admitting them all, plus `extra` cells
	 * only FIXED admits that run over. NEW overruns `slow` times and FIXED `slow + extra`.
	 */
	function pooled(n: number, slow: number, extra: number): AdmCell[] {
		const taken = Array.from({ length: n }, (_, i) =>
			cell({ t: i < slow ? 12000 : 8000, fixedB: 5000, newAdmit: [10], budgets: [10] })
		);
		const only = Array.from({ length: extra }, () =>
			cell({ t: 12000, fixedB: 5000, newAdmit: [], budgets: [10] })
		);
		return [...taken, ...only];
	}

	it('passes at exactly 5 percent and exactly 2x', () => {
		const g = gate(pooled(20, 1, 1), [10]);

		expect(overrunRate(g.new)).toBe(0.05);
		expect(g.fixed.overruns).toBe(2);
		expect(g.ratio).toBe(2);
		expect(g.pass).toBe(true);
	});

	it('fails one overrun in 19, which is over 5 percent', () => {
		const g = gate(pooled(19, 1, 1), [10]);

		expect(g.newRatePass).toBe(false);
		expect(g.ratioPass).toBe(true);
		expect(g.pass).toBe(false);
		expect(g.reason).toContain('over 5%');
	});

	it('fails a ratio of 1.5 with the rate inside the limit', () => {
		const g = gate(pooled(40, 2, 1), [10]);

		expect(g.newRatePass).toBe(true);
		expect(g.ratio).toBe(1.5);
		expect(g.pass).toBe(false);
		expect(g.reason).toContain('under the required 2x');
	});

	it('fails when NEW admits nothing, because 0 of 0 is no evidence', () => {
		const rejected = [
			cell({ t: 5000, fixedB: 4000, newAdmit: [], budgets: B }),
			cell({ t: 30000, fixedB: 4000, newAdmit: [], budgets: B })
		];
		const g = gate(rejected, B);

		expect(g.new.admitted).toBe(0);
		expect(overrunRate(g.new)).toBeNull();
		expect(g.newRatePass).toBe(false);
		expect(g.pass).toBe(false);
		expect(g.reason).toContain('admitted no request');
		expect(g.new.falseRejects).toBe(2);
	});

	it('fails with no cells at all', () => {
		const g = gate([], B);

		expect(g.pairs).toBe(0);
		expect(g.pass).toBe(false);
		expect(g.reason).toContain('no eligible pairs');
	});

	it('fails when neither arm overran, since the ratio is 0 of 0', () => {
		const calm = [cell({ t: 5000, fixedB: 4000, newAdmit: [10], budgets: [10] })];
		const g = gate(calm, [10]);

		expect(g.new.overruns).toBe(0);
		expect(g.fixed.overruns).toBe(0);
		expect(g.ratio).toBeNull();
		expect(g.pass).toBe(false);
		expect(g.reason).toContain('ratio is undefined');
	});

	it('fails when every request is rejected by both arms', () => {
		const all = [cell({ t: 5000, fixedB: 50000, newAdmit: [], budgets: B })];
		const g = gate(all, B);

		expect(g.fixed.admitted).toBe(0);
		expect(g.pass).toBe(false);
	});
});

describe('breakdown', () => {
	it('groups by key in numeric order and skips ineligible cells', () => {
		const cells = [
			cell({ t: 5000, fixedB: 4000, newAdmit: [10], budgets: [10], width: 800 }),
			cell({ t: 5000, fixedB: 4000, newAdmit: [10], budgets: [10], width: 200 }),
			cell({
				t: 5000,
				fixedB: 4000,
				newAdmit: [10],
				budgets: [10],
				width: 400,
				load_excluded: true
			})
		];
		const groups = breakdown(cells, (c) => String(c.width));

		expect([...groups.keys()]).toEqual(['200', '800']);
		expect(groups.get('200')!.new.admitted).toBe(1);
	});

	it('resolves a source output to the source format', () => {
		expect(outputFormat({ format: 'png', output: 'source' })).toBe('png');
		expect(outputFormat({ format: 'png', output: 'webp' })).toBe('webp');
	});
});

describe('controlSpread', () => {
	const control = (label: string, ms: number) => ({
		label,
		measured_us: ms * 1000,
		samples_us: [ms * 1000],
		load_before: 1,
		load_after: 1
	});

	it('is the largest over the smallest and the last over the first', () => {
		const s = controlSpread([control('start', 4), control('middle', 5), control('end', 4.2)]);

		expect(s!.spread).toBeCloseTo(1.25, 12);
		expect(s!.firstToLast).toBeCloseTo(1.05, 12);
	});

	it('is null with fewer than two controls', () => {
		expect(controlSpread([control('start', 4)])).toBeNull();
		expect(controlSpread([])).toBeNull();
	});
});

describe('summary', () => {
	const meta: AdmFile['meta'] = {
		machine: 'test',
		cores: 12,
		load_limit: 3,
		wasm: 'a.wasm',
		wasm_sha256: 'aa',
		frozen_sha256: 'bb',
		node: 'v0',
		mode: 'timed',
		evaluation: true,
		subset: null,
		budgets_ms: B,
		quality: 80,
		runs: 9,
		warmups: 2,
		load_start: 1,
		load_end: 1
	};

	it('prints the verdict for the evaluation and none for a subset or stand-in run', () => {
		const full = summaryMarkdown({ meta, cells: four, controls: [] });
		const subset = summaryMarkdown({
			meta: { ...meta, evaluation: false, subset: '--only x' },
			cells: four,
			controls: []
		});
		const standin = summaryMarkdown({
			meta: { ...meta, evaluation: false, mode: 'standin' },
			cells: four,
			controls: []
		});

		expect(full).toContain('**Gate: PASS.**');
		expect(subset).not.toMatch(/Gate: (PASS|FAIL)/);
		expect(subset).toContain('not the evaluation');
		expect(standin).not.toMatch(/Gate: (PASS|FAIL)/);
		expect(standin).toContain('Stand-in data, not the evaluation');
	});

	it('states FAIL with the reason when the gate fails', () => {
		const text = summaryMarkdown({ meta, cells: [], controls: [] });

		expect(text).toContain('**Gate: FAIL.**');
		expect(text).toContain('no eligible pairs');
	});

	it('is a pure function of the file', () => {
		const file = { meta, cells: four, controls: [] };

		expect(summaryMarkdown(file)).toBe(summaryMarkdown(structuredClone(file)));
	});
});
