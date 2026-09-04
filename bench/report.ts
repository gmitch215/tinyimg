/**
 * Renders a run into markdown and JSON.
 *
 * The markdown goes into a job summary and a pull request comment; the JSON is what a later run
 * diffs against. Neither gates anything, which is why the numbers can be reported plainly rather
 * than compared to a threshold nobody chose on evidence.
 */

import { perMegapixel, type Timing } from './harness.js';

/** One section of the report. */
export interface Section {
	/** The heading. */
	title: string;
	/** Why the section exists, in one or two sentences. */
	blurb: string;
	/** The measurements, in the order they should appear. */
	timings: Timing[];
}

/** A whole run. */
export interface Report {
	/** When it ran, so a stale artifact is obvious. */
	at: string;
	/** What it ran on. */
	platform: string;
	/** The library version the module reported. */
	version: string;
	/** Whether the module was built with SIMD. */
	simd: boolean;
	/** The sections. */
	sections: Section[];
}

function ms(value: number): string {
	if (value >= 100) return value.toFixed(0);
	if (value >= 10) return value.toFixed(1);
	return value.toFixed(2);
}

function bytes(value: number | undefined): string {
	if (value === undefined) return '';
	return value.toLocaleString();
}

/**
 * Groups a section's timings by label so the arms sit side by side.
 *
 * A table with one row per measurement and one column per arm is the only shape a reader can
 * compare in: two separate tables mean counting rows to find the pair.
 */
function compare(timings: Timing[]): { arms: string[]; rows: Map<string, Map<string, Timing>> } {
	const arms: string[] = [];
	const rows = new Map<string, Map<string, Timing>>();

	for (const timing of timings) {
		if (!arms.includes(timing.arm)) arms.push(timing.arm);

		const row = rows.get(timing.label) ?? new Map<string, Timing>();
		row.set(timing.arm, timing);
		rows.set(timing.label, row);
	}

	return { arms, rows };
}

function renderSection(section: Section): string {
	const { arms, rows } = compare(section.timings);
	const lines: string[] = [`### ${section.title}`, '', section.blurb, ''];

	// a single arm needs no comparison columns, and the per-megapixel figure is the useful one
	if (arms.length === 1) {
		lines.push('| Measurement | Median | p95 | ms/Mpx | Bytes out |');
		lines.push('| --- | ---: | ---: | ---: | ---: |');

		for (const [label, row] of rows) {
			const timing = row.get(arms[0]!)!;

			if (timing.skipped) {
				lines.push(`| ${label} | | | | ${timing.skipped} |`);
				continue;
			}

			const rate = perMegapixel(timing);
			lines.push(
				`| ${label} | ${ms(timing.median)} | ${ms(timing.p95)} | ` +
					`${rate === undefined ? '' : ms(rate)} | ${bytes(timing.bytesOut)} |`
			);
		}

		lines.push('');
		return lines.join('\n');
	}

	// the byte count sits beside the time on purpose. a lossless encoder trades one against the
	// other, so a ratio without it is not a verdict: our PNG encode reads 21x slower than
	// @jsquash's and produces a file 47% smaller, and only one of those two facts is visible in a
	// column of milliseconds
	const header = [
		'Measurement',
		...arms.flatMap((arm) => [`${arm} (ms)`, `${arm} bytes`]),
		'Ratio'
	];
	lines.push(`| ${header.join(' | ')} |`);
	lines.push(`| --- | ${arms.flatMap(() => ['---:', '---:']).join(' | ')} | ---: |`);

	for (const [label, row] of rows) {
		const cells = arms.flatMap((arm) => {
			const timing = row.get(arm);
			if (!timing) return ['', ''];
			if (timing.skipped) return [timing.skipped, ''];

			return [ms(timing.median), bytes(timing.bytesOut)];
		});

		// the ratio is the first arm against the second, which is the direction the section's
		// blurb reads in
		const first = row.get(arms[0]!);
		const second = row.get(arms[1]!);
		const ratio =
			first && second && !first.skipped && !second.skipped && first.median > 0
				? `${(second.median / first.median).toFixed(2)}x`
				: '';

		lines.push(`| ${label} | ${cells.join(' | ')} | ${ratio} |`);
	}

	lines.push('');
	return lines.join('\n');
}

/**
 * The whole report as markdown.
 *
 * @param report The run.
 * @return Markdown, without a trailing newline.
 */
export function renderMarkdown(report: Report): string {
	const lines: string[] = [
		'## Benchmarks',
		'',
		`\`${report.version}\` on ${report.platform}, SIMD ${report.simd ? 'on' : 'off'}, ` +
			`${report.at}.`,
		'',
		'Median of the measured runs. Reported, not gated: a threshold nobody picked on evidence',
		'would either never fire or fire on a busy runner.',
		''
	];

	for (const section of report.sections) lines.push(renderSection(section));

	return lines.join('\n').trimEnd();
}
