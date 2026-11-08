import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

type Entry = {
	id: string;
	url: string | null;
	source?: 'local';
	path?: string;
	license: string;
	licenseUrl: string | null;
	author?: string;
	derivedFrom?: string;
	sha256: string;
	bytes: number;
	width: number;
	height: number;
	format: string;
	progressive?: boolean;
	subsampling?: string;
	restartInterval?: number;
	split: 'cal' | 'test';
};

const MANIFEST = join(import.meta.dirname, '..', '..', 'scripts', 'corpus-manifest.json');
const { entries } = JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] };

/** Mirrors splitOf in scripts/corpus.ts; written out again so a change to one must change the other. */
function splitOf(sha256: string): 'cal' | 'test' {
	return parseInt(sha256.slice(0, 8), 16) % 5 < 3 ? 'cal' : 'test';
}

function bandOf(e: Entry): string {
	const mp = (e.width * e.height) / 1_000_000;
	if (mp < 0.25) return 'under 0.25';
	if (mp < 1) return '0.25 to 1';
	if (mp < 4) return '1 to 4';
	if (mp < 12) return '4 to 12';
	return 'over 12';
}

const count = (pred: (e: Entry) => boolean) => entries.filter(pred).length;
const jpeg = (e: Entry) => e.format === 'jpeg';

describe('corpus manifest', () => {
	it('parses to at least 200 entries', () => {
		expect(entries.length).toBeGreaterThanOrEqual(200);
	});

	it('gives every entry a unique id and a unique lowercase sha256', () => {
		expect(new Set(entries.map((e) => e.id)).size).toBe(entries.length);
		expect(new Set(entries.map((e) => e.sha256)).size).toBe(entries.length);
		for (const e of entries) expect(e.sha256).toMatch(/^[0-9a-f]{64}$/);
	});

	it('gives every entry a license; a licenseUrl unless the entry is an owner-provided local file', () => {
		for (const e of entries) {
			expect(e.license, e.id).not.toBe('');
			if (e.source === 'local') {
				expect(e.licenseUrl, e.id).toBeNull();
				expect(e.url, e.id).toBeNull();
				expect(e.path, e.id).toMatch(/^tests\/fixtures\//);
			} else {
				expect(e.licenseUrl, e.id).toBeTruthy();
				if (!e.derivedFrom) expect(e.url, e.id).toBeTruthy();
			}
		}
	});

	it('names an author for every entry whose license requires attribution', () => {
		for (const e of entries) {
			if (e.license.startsWith('CC BY')) expect(e.author, e.id).toBeTruthy();
		}
	});

	it('reproduces the recorded split for every entry', () => {
		for (const e of entries) expect(e.split, e.id).toBe(splitOf(e.sha256));
	});

	it('keeps every derived entry in the split of its source', () => {
		const byId = new Map(entries.map((e) => [e.id, e]));
		for (const e of entries) {
			if (!e.derivedFrom) continue;
			const source = byId.get(e.derivedFrom);
			expect(source, e.id).toBeDefined();
			expect(e.split, e.id).toBe(source?.split);
			expect(e.url, e.id).toBeNull();
		}
	});

	it('meets the format floors', () => {
		const floors: Array<[string, number]> = [
			['jpeg', 120],
			['png', 25],
			['webp', 20],
			['gif', 10],
			['tiff', 5],
			['bmp', 5],
			['avif', 15]
		];
		for (const [format, min] of floors) {
			expect(
				count((e) => e.format === format),
				format
			).toBeGreaterThanOrEqual(min);
		}
	});

	it('meets the JPEG floors for progressive, subsampling, restart interval and CMYK or grayscale', () => {
		expect(count((e) => jpeg(e) && e.progressive === true)).toBeGreaterThanOrEqual(20);
		expect(
			count((e) => jpeg(e) && (e.subsampling === '4:4:4' || e.subsampling === '4:2:2'))
		).toBeGreaterThanOrEqual(15);
		expect(count((e) => jpeg(e) && (e.restartInterval ?? 0) > 0)).toBeGreaterThanOrEqual(5);
		expect(
			count((e) => jpeg(e) && (e.subsampling === 'gray' || e.subsampling === 'cmyk'))
		).toBeGreaterThanOrEqual(5);
	});

	it('fits the 600 MB budget', () => {
		const total = entries.reduce((sum, e) => sum + e.bytes, 0);
		expect(total).toBeLessThanOrEqual(600_000_000);
	});

	it('puts at least one entry in every megapixel band', () => {
		const bands = new Set(entries.map(bandOf));
		for (const band of ['under 0.25', '0.25 to 1', '1 to 4', '4 to 12', 'over 12']) {
			expect(bands.has(band), band).toBe(true);
		}
	});
});
