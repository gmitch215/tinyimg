/**
 * Adds the encoder to the v1.1.1 baseline estimate, from the committed cells and the frozen arm.
 *
 * ```sh
 * bun scripts/measure/baseline-total.ts
 * ```
 *
 * `baseline.ts` scores `Image.decide().estimateMs`, which prices decode and resample and no encoder,
 * against a clock that includes the encode. `transform()` budgets against that estimate plus
 * `tiny_encode_cost`, so this is the total a caller of `transform({ budgetMs })` is held to. The
 * encode term is a pure function of the output extent and format, so it is added here without
 * timing anything: the output extent is the width asked for at the source's aspect, and `source`
 * output is the source's own format. Writes `bench/baselines/v1.1.1/total-summary.md`.
 */

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { TinyImgModule } from '../../src/ts/index.js';
import { OVER, quantile, UNDER } from './baseline.js';

const ROOT = join(import.meta.dirname, '..', '..');
const DIR = join(ROOT, 'bench', 'baselines', 'v1.1.1');
export const FORMAT_ID: Record<string, number> = {
	png: 1,
	jpeg: 2,
	bmp: 3,
	gif: 4,
	tiff: 5,
	webp: 6,
	avif: 7
};

type Cell = {
	id: string;
	split: string;
	format: string;
	width: number;
	output: string;
	status: string;
	alpha: boolean;
	estimate_us: number | null;
	measured_us: number | null;
};

export type Dims = Map<string, { width: number; height: number }>;

/** The v1.1.1 arm's `tiny_encode_cost` for a cell's output extent and format, or null. */
export function encodeCostUs(
	tinyimg: TinyImgModule,
	dims: Dims,
	cell: Pick<Cell, 'id' | 'format' | 'output' | 'width'>
): number | null {
	const source = dims.get(cell.id);
	if (!source) return null;

	const id = FORMAT_ID[cell.output === 'source' ? cell.format : cell.output];
	if (id === undefined) return null;

	const height = Math.max(1, Math.round((cell.width * source.height) / source.width));
	return tinyimg.exports.tiny_encode_cost(id, cell.width, height);
}

/** Arm B of the baseline: the cell's decode estimate plus the encoder, or null when unusable. */
export function totalEstimateUs(tinyimg: TinyImgModule, dims: Dims, cell: Cell): number | null {
	if (cell.estimate_us === null || cell.estimate_us <= 0) return null;

	const encode = encodeCostUs(tinyimg, dims, cell);
	return encode === null ? null : cell.estimate_us + encode;
}

function row(label: string, errors: number[]): string {
	const sorted = [...errors].sort((a, b) => a - b);
	const inBand = errors.filter((e) => e >= Math.log2(UNDER) && e <= Math.log2(OVER)).length;
	const ratio = (e: number) => `${Math.pow(2, Math.abs(e)).toFixed(2)}x`;
	const lo = sorted[0]!;
	const hi = sorted[sorted.length - 1]!;

	return (
		`| ${label} | ${errors.length} | ${quantile(sorted, 0.5).toFixed(2)} | ` +
		`${quantile(sorted, 0.1).toFixed(2)} | ${quantile(sorted, 0.9).toFixed(2)} | ` +
		`${hi.toFixed(2)} (${ratio(hi)} ${hi >= 0 ? 'over' : 'under'}) | ` +
		`${lo.toFixed(2)} (${ratio(lo)} ${lo >= 0 ? 'over' : 'under'}) | ` +
		`${((100 * inBand) / errors.length).toFixed(1)}% |`
	);
}

const header = [
	'| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |',
	'| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |'
];

async function main() {
	const arm = join(ROOT, 'bench', 'arms', 'tinyimg-v1.1.1.wasm');
	const tinyimg = await TinyImgModule.load(await WebAssembly.compile(readFileSync(arm)));
	const manifest = JSON.parse(
		readFileSync(join(ROOT, 'scripts', 'corpus-manifest.json'), 'utf8')
	) as { entries: { id: string; width: number; height: number }[] };
	const dims: Dims = new Map(manifest.entries.map((e) => [e.id, e]));
	const file = JSON.parse(readFileSync(join(DIR, 'cells.json'), 'utf8')) as { cells: Cell[] };
	const total = (cell: Cell) => totalEstimateUs(tinyimg, dims, cell);

	const timed = file.cells.filter(
		(c) => c.status === 'ok' && c.measured_us !== null && !c.alpha && c.split === 'test'
	);
	const errors = (cells: Cell[], pick: (c: Cell) => number | null) =>
		cells.flatMap((c) => {
			const estimate = pick(c);
			return estimate === null ? [] : [Math.log2(estimate / c.measured_us!)];
		});

	const out: string[] = [
		'# The shipped total, estimate plus encode, against the clock',
		'',
		'Same cells as `summary.md` (test split, alpha cells excluded). Arm A is `Image.decide().estimateMs`, decode and ' +
			'resample only. Arm B adds `tiny_encode_cost(format, width, height)`, which is what `transform({ budgetMs })` ' +
			'compares against. Error is `log2(estimate / measured)`: -1 is a 2x under-estimate. Produced by ' +
			'`scripts/measure/baseline-total.ts` from `cells.json`, the manifest dimensions and the frozen arm.',
		'',
		'## Arm A, decode and resample only',
		'',
		...header,
		row(
			'all',
			errors(timed, (c) => c.estimate_us)
		),
		'',
		'## Arm B, plus the encoder',
		'',
		...header,
		row('all', errors(timed, total))
	];

	for (const [title, key] of [
		['by source format', (c: Cell) => c.format],
		['by output format', (c: Cell) => (c.output === 'source' ? c.format : c.output)]
	] as const) {
		out.push('', `### Arm B ${title}`, '', ...header);
		for (const group of [...new Set(timed.map(key))].sort()) {
			const cells = timed.filter((c) => key(c) === group);
			const e = errors(cells, total);
			if (e.length > 0) out.push(row(group, e));
		}
	}

	writeFileSync(join(DIR, 'total-summary.md'), out.join('\n') + '\n');
	console.log(out.slice(0, 12).join('\n'));
}

if (import.meta.main) await main();
