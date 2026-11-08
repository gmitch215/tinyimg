import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import {
	BASELINES,
	buildTable,
	fitCells,
	type LeverCell,
	type Table
} from '../../scripts/distortion-table.js';

const HEADER = join(import.meta.dirname, '..', '..', 'src', 'distortion-table.h');
const table = JSON.parse(readFileSync(join(BASELINES, 'table.json'), 'utf8')) as Table;
const header = readFileSync(HEADER, 'utf8');
const { lever, encode } = fitCells();

function macro(name: string): number {
	const m = new RegExp(`#define ${name} (\\d+)`).exec(header);
	if (!m) throw new Error(`${name} is not in the header`);
	return Number(m[1]);
}

describe('distortion table', () => {
	it('has as many header rows as the JSON has entries', () => {
		expect(macro('TINYIMG_DISTORTION_LEVER_ROWS')).toBe(table.lever.length);
		expect(macro('TINYIMG_DISTORTION_ENCODE_ROWS')).toBe(table.encode.length);
		expect(table.lever.length).toBeGreaterThan(0);
		expect(table.encode.length).toBeGreaterThan(0);
	});

	it('regenerates from the committed cells', () => {
		expect(buildTable()).toEqual(table);
	});

	it('orders every percentile and marks EXACT rows at 99 dB', () => {
		for (const r of [...table.lever, ...table.encode]) {
			expect(r.p05).toBeLessThanOrEqual(r.median);
			expect(r.n).toBeGreaterThanOrEqual(table.min_samples);
			if (r.class === 'EXACT') {
				expect(r.p05).toBe(99);
				expect(r.median).toBe(99);
			}
		}
	});

	it('records the pinned fixture inputs with their hashes', () => {
		const inputs = JSON.parse(
			readFileSync(join(BASELINES, 'inputs.fixtures.json'), 'utf8')
		) as {
			files: { id: string; sha256: string }[];
		};
		expect(inputs.files).toHaveLength(27);
		for (const f of inputs.files) expect(f.sha256).toMatch(/^[0-9a-f]{64}$/);
	});

	it('never fits from an image the manifest holds out', () => {
		const fitted = new Set([...lever, ...encode].map((c) => c.image));
		for (const id of table.excluded_test_images) expect(fitted.has(id)).toBe(false);
	});
});

describe('distortion cells', () => {
	it('never lets a deeper reduction come closer under a filter the plan chose', () => {
		const byRequest = new Map<string, LeverCell[]>();
		for (const c of lever) {
			if (c.error || c.extent_changed) continue;
			const key = `${c.image}|${c.width}`;
			(byRequest.get(key) ?? byRequest.set(key, []).get(key)!).push(c);
		}

		for (const cells of byRequest.values()) {
			for (const a of cells) {
				for (const b of cells) {
					const deeper =
						a.effort_delta === b.effort_delta &&
						a.filter_delta === b.filter_delta &&
						b.scale_steps === a.scale_steps + 1;
					if (deeper && a.filter_delta === 0) expect(b.db).toBeLessThanOrEqual(a.db);
				}
			}
		}
	});

	it('keeps the reference against itself at 99 dB', () => {
		const reference = lever.filter((c) => c.k === 0);
		expect(reference.length).toBeGreaterThan(0);
		for (const c of reference) {
			expect(c.db).toBe(99);
			expect(c.identical).toBe(true);
		}
	});

	it('has one record per encode setting and no failed encodes', () => {
		expect(encode.filter((c) => c.error !== undefined)).toEqual([]);
		expect(encode.filter((c) => c.db === null)).toEqual([]);
	});
});
