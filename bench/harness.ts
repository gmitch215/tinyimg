/**
 * Timing, reduced to one honest number per measurement.
 *
 * Nothing here knows what an image is. It exists so every arm reports the same statistics the same
 * way, and so a change to how a measurement is taken changes every measurement at once.
 */

/** What one measured operation produced. */
export interface Timing {
	/** What was measured. */
	label: string;
	/** Which arm produced it, for the comparison columns. */
	arm: string;
	/** Milliseconds, middle of the measured runs. */
	median: number;
	/** Milliseconds, 95th percentile of them. */
	p95: number;
	/** Milliseconds, fastest run, which is the closest to a warm-cache best case. */
	best: number;
	/** How many measured runs went into it. */
	runs: number;
	/** Input bytes, where the measurement has an input. */
	bytesIn?: number;
	/** Output bytes, where it produces one. */
	bytesOut?: number;
	/** Megapixels the operation covered, for the per-megapixel column. */
	megapixels?: number;
	/** Set when the arm could not run this measurement, with the reason. */
	skipped?: string;
}

/** How many times to run before and during a measurement. */
export interface Budget {
	/** Runs whose timings are discarded. */
	warmup: number;
	/** Runs whose timings are kept. */
	runs: number;
}

/** The default budget: enough runs for a median to mean something, few enough to finish. */
export const DEFAULT_BUDGET: Budget = { warmup: 3, runs: 11 };

function percentile(sorted: number[], fraction: number): number {
	if (sorted.length === 0) return 0;

	const at = Math.min(sorted.length - 1, Math.floor(fraction * sorted.length));
	return sorted[at]!;
}

/**
 * Runs an operation and reduces its timings.
 *
 * The median rather than the mean, because one scheduling hiccup moves a mean and does not move a
 * median, and a benchmark that a background process can move is a benchmark nobody can act on.
 * `p95` is reported beside it so a genuinely bimodal operation is visible rather than smoothed.
 *
 * @param label What is being measured.
 * @param arm Which arm this is.
 * @param operation The work. Its return value is discarded except for the byte count.
 * @param options Budget and the sizes to report.
 * @return The reduced timing.
 */
export async function measure(
	label: string,
	arm: string,
	operation: () => unknown | Promise<unknown>,
	options: Partial<Budget> & {
		bytesIn?: number;
		megapixels?: number;
	} = {}
): Promise<Timing> {
	const budget = { ...DEFAULT_BUDGET, ...options };

	let bytesOut: number | undefined;

	for (let i = 0; i < budget.warmup; i++) await operation();

	const timings: number[] = [];

	for (let i = 0; i < budget.runs; i++) {
		const started = performance.now();
		const produced = await operation();

		timings.push(performance.now() - started);

		if (produced instanceof Uint8Array) bytesOut = produced.byteLength;
		else if (typeof produced === 'number') bytesOut = produced;
	}

	timings.sort((a, b) => a - b);

	return {
		label,
		arm,
		median: percentile(timings, 0.5),
		p95: percentile(timings, 0.95),
		best: timings[0]!,
		runs: budget.runs,
		...(options.bytesIn === undefined ? {} : { bytesIn: options.bytesIn }),
		...(bytesOut === undefined ? {} : { bytesOut }),
		...(options.megapixels === undefined ? {} : { megapixels: options.megapixels })
	};
}

/** Records that an arm could not run a measurement, which is not the same as it being slow. */
export function skip(label: string, arm: string, reason: string): Timing {
	return { label, arm, median: 0, p95: 0, best: 0, runs: 0, skipped: reason };
}

/** Milliseconds per megapixel, or undefined when the measurement has no pixel count. */
export function perMegapixel(timing: Timing): number | undefined {
	if (!timing.megapixels || timing.skipped) return undefined;
	return timing.median / timing.megapixels;
}
