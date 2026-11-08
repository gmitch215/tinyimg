import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

const OUT = join(import.meta.dirname, '..', '..', 'bench', 'baselines', 'free-counts');

type Thumb = { bytes: number; width: number; height: number; shownWidth: number; stale: boolean };
type Jpeg = { id: string; progressive: boolean; thumbnail: Thumb | null };
type Cells = { jpeg: Jpeg[] };

const cells = JSON.parse(readFileSync(join(OUT, 'cells.json'), 'utf8')) as Cells;
// prettier pads table cells, so collapse the spaces before comparing
const summary = readFileSync(join(OUT, 'summary.md'), 'utf8').replace(/ +/g, ' ');

function thumbsOf(jpeg: Jpeg[]): Thumb[] {
	return jpeg.flatMap((x) => (x.thumbnail ? [x.thumbnail] : []));
}

describe('free-counts', () => {
	it('records a size and dimensions for every JPEG thumbnail', () => {
		const withThumb = cells.jpeg.filter((x) => x.thumbnail);
		expect(withThumb.length).toBeGreaterThan(0);
		for (const x of withThumb) {
			const t = x.thumbnail;
			expect(t?.bytes, x.id).toBeGreaterThan(0);
			expect(t?.width, x.id).toBeGreaterThan(0);
			expect(t?.height, x.id).toBeGreaterThan(0);
		}
	});

	it('states the headline counts that cells.json gives', () => {
		const thumbs = thumbsOf(cells.jpeg);
		const at160 = thumbs.filter((t) => t.shownWidth >= 160);
		const fresh160 = at160.filter((t) => !t.stale);
		const progressive = cells.jpeg.filter((x) => x.progressive).length;
		const row = `| all | all | ${cells.jpeg.length} | ${progressive} | ${thumbs.length} | ${at160.length} | ${fresh160.length} |`;

		expect(summary).toContain(row);
		expect(summary).toContain(
			`JPEGs with an EXIF thumbnail: ${thumbs.length} of ${cells.jpeg.length}.`
		);
	});

	it('answers both mechanisms in the decision section', () => {
		expect(summary).toContain('## What this decides');
		expect(summary).toContain('### Zero-decode thumbnails');
		expect(summary).toContain('### Progressive JPEG at 1/8');
	});
});
