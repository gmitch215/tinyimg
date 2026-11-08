import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import {
	type Cell,
	type CellsFile,
	quantile,
	signedError,
	statsOf,
	summaryMarkdown
} from '../../scripts/measure/baseline.js';

const DIR = join(import.meta.dirname, '..', '..', 'bench', 'baselines', 'v1.1.1');
const file = JSON.parse(readFileSync(join(DIR, 'cells.json'), 'utf8')) as CellsFile;

const FIELDS: Array<keyof Cell> = [
	'id',
	'split',
	'format',
	'local',
	'source_bytes',
	'source_mpx',
	'width',
	'budget',
	'output',
	'status',
	'reason',
	'scale',
	'degraded',
	'alpha',
	'estimate_us',
	'measured_us',
	'samples_us'
];

describe('baseline cells', () => {
	it('carries every field on every cell', () => {
		expect(file.cells.length).toBeGreaterThan(0);
		for (const cell of file.cells) {
			for (const field of FIELDS) expect(cell, `${cell.id} ${field}`).toHaveProperty(field);
		}
	});

	it('uses only the cal and test splits', () => {
		for (const cell of file.cells) expect(['cal', 'test']).toContain(cell.split);
	});

	it('records a reason on every cell that did not time', () => {
		for (const cell of file.cells) {
			if (cell.status === 'ok') {
				expect(cell.samples_us).toHaveLength(file.meta.runs);
				expect(cell.measured_us).toBeGreaterThan(0);
			} else {
				expect(cell.reason).toBeTruthy();
			}
		}
	});

	it('names the arm it measured', () => {
		expect(file.meta.wasm_sha256).toBe(
			'8851e695962e8796caae504858b86c70823dd96e3cd443ee0365705c2e81b441'
		);
	});
});

describe('baseline summary', () => {
	it('recomputes from cells.json', () => {
		const written = readFileSync(join(DIR, 'summary.md'), 'utf8');
		expect(summaryMarkdown(file)).toBe(written);
	});

	it('signs the error as log2 of estimate over measured', () => {
		const cell = {
			status: 'ok',
			estimate_us: 4000,
			measured_us: 1000
		} as Cell;
		expect(signedError(cell)).toBe(2);
		expect(signedError({ ...cell, estimate_us: 250 })).toBe(-2);
		expect(signedError({ ...cell, status: 'failed' })).toBeNull();
		expect(signedError({ ...cell, estimate_us: 0 })).toBeNull();
	});

	it('counts the band as 0.6x to 2.0x', () => {
		const make = (est: number) =>
			({ status: 'ok', estimate_us: est, measured_us: 1000 }) as Cell;
		const stats = statsOf([make(500), make(600), make(2000), make(2100), make(100)])!;
		expect(stats.count).toBe(5);
		expect(stats.inBand).toBeCloseTo(2 / 5);
		expect(stats.worstOver).toBeCloseTo(Math.log2(2.1));
		expect(stats.worstUnder).toBeCloseTo(Math.log2(0.1));
		expect(quantile([1, 2, 3], 0.5)).toBe(2);
	});
});
