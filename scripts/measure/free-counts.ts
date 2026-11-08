/**
 * Counts the two Free-plan mechanisms over the corpus before either is built.
 *
 * ```sh
 * bun scripts/measure/free-counts.ts                                # writes bench/baselines/free-counts/
 * bun scripts/measure/free-counts.ts --wasm <copy.wasm> --out <dir>
 * ```
 *
 * Counts only: no clock is read and nothing is fitted. A JPEG thumbnail is stale when its PSNR against
 * the box-filtered main image is under 20 dB or its raw aspect differs from the main image's by more
 * than 1 percent. `decision.md` beside the output, when present, is appended to `summary.md`.
 */
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { Image, TinyImgModule } from '../../src/ts/index.js';

const ROOT = join(import.meta.dirname, '..', '..');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const CORPUS = join(ROOT, 'corpus');
const WORK = join(CORPUS, 'free-counts');
const FIXTURES = join(ROOT, 'tests', 'fixtures');

const WIDTHS = [64, 96, 128, 160, 200] as const;
const PSNR_FLOOR_DB = 20;
const ASPECT_TOLERANCE_PCT = 1;
const ARMS = ['p160', 'p1200', 'p8'] as const;
type ArmName = (typeof ARMS)[number];
const ARM_TEXT: Record<ArmName, string> = {
	p160: 'output 160 px wide',
	p1200: 'output 1200 px wide',
	p8: 'the widest output width Image.decide() gives denominator 8'
};
const SOF = new Set([0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf]);

type Entry = {
	id: string;
	url: string | null;
	source?: 'local';
	path?: string;
	sha256: string;
	bytes: number;
	width: number;
	height: number;
	format: string;
	progressive?: boolean;
	split: 'cal' | 'test';
	derivedFrom?: string;
};

type Sof = { width: number; height: number };
type Scan = { ss: number; se: number; bytes: number; comps: number[] };
type Segment = { marker: number; start: number; payload: number; end: number };

type Work = {
	sourceSamples: number;
	decodedSamples: number;
	transformSamples: number;
	blocks: number;
	entropyBytes: number;
	scansSkipped: number;
};
type Arm = ({ ok: true; scale: number; outWidth: number } & Work) | { ok: false; reason: string };

type Thumb = {
	bytes: number;
	width: number;
	height: number;
	shownWidth: number;
	identifyMatches: boolean;
	psnrDb: number;
	aspectMismatchPct: number;
	orientation: 'agree' | 'disagree' | 'absent';
	stale: boolean;
};

type Jpeg = {
	id: string;
	split: 'cal' | 'test';
	origin: string;
	bytes: number;
	width: number;
	height: number;
	shownWidth: number;
	orientation: number;
	sizeMatchesManifest: boolean;
	progressive: boolean;
	scans: number;
	scanBytes: number;
	dcScans: number;
	dcScanBytes: number;
	acLumaBytes: number;
	acChromaBytes: number;
	sampling: string;
	thumbnail: Thumb | null;
	thumbnailNote: string | null;
	arms: Record<ArmName, Arm>;
};

type Avif = {
	id: string;
	split: string;
	origin: string;
	bytes: number;
	thmbBoxes: number;
	thmbReferences: number;
};

type Cells = {
	meta: {
		wasm: string;
		wasmSha256: string;
		wasmVersion: string;
		manifestSha256: string;
		tools: Record<string, string>;
		thresholds: { psnrFloorDb: number; aspectTolerancePct: number };
		arms: Record<ArmName, string>;
		widths: readonly number[];
	};
	jpeg: Jpeg[];
	avif: Avif[];
};

// #region helpers

function byte(b: Uint8Array, i: number): number {
	const x = b[i];
	if (x === undefined) throw new RangeError(`read past end of buffer at ${i}`);
	return x;
}

function u16(b: Uint8Array, i: number): number {
	return (byte(b, i) << 8) | byte(b, i + 1);
}

function sha256(bytes: Uint8Array): string {
	return createHash('sha256').update(bytes).digest('hex');
}

function errorText(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

function arg(name: string): string | undefined {
	const i = process.argv.indexOf(name);
	return i >= 0 ? process.argv[i + 1] : undefined;
}

function run(
	cmd: string,
	args: string[],
	okStatus: number[] = [0]
): { stdout: string; stderr: string } {
	const r = spawnSync(cmd, args, { encoding: 'utf8', maxBuffer: 1 << 26 });
	if (r.error) throw r.error;
	if (!okStatus.includes(r.status ?? -1)) {
		throw new Error(`${cmd} exited ${r.status}: ${r.stderr.trim()}`);
	}
	return { stdout: r.stdout, stderr: r.stderr };
}

function median(xs: number[]): number | null {
	if (xs.length === 0) return null;
	const s = [...xs].sort((a, b) => a - b);
	const m = s.length >> 1;
	return s.length % 2 ? (s[m] as number) : ((s[m - 1] as number) + (s[m] as number)) / 2;
}

/** "median (min to max)" over the values, or "n/a" when there are none. */
function spread(xs: number[], digits = 3): string {
	const m = median(xs);
	if (m === null) return 'n/a';
	const lo = Math.min(...xs);
	const hi = Math.max(...xs);
	return `${m.toFixed(digits)} (${lo.toFixed(digits)} to ${hi.toFixed(digits)})`;
}

function table(headers: string[], rows: string[][]): string {
	return [
		`| ${headers.join(' | ')} |`,
		`| ${headers.map(() => '---').join(' | ')} |`,
		...rows.map((r) => `| ${r.join(' | ')} |`)
	].join('\n');
}

// #endregion

// #region jpeg structure

function* segments(b: Uint8Array): Generator<Segment> {
	let i = 2;
	while (i + 1 < b.length) {
		if (byte(b, i) !== 0xff) throw new Error(`expected a marker at offset ${i}`);
		const m = byte(b, i + 1);
		if (m === 0xff) {
			i += 1;
			continue;
		}
		if (m === 0xd9) return;
		if (m === 0xd8 || m === 0x01 || (m >= 0xd0 && m <= 0xd7)) {
			i += 2;
			continue;
		}
		let end = i + 2 + u16(b, i + 2);
		if (m === 0xda) end = scanEnd(b, end);
		yield { marker: m, start: i, payload: i + 4, end };
		i = end;
	}
}

/** The first byte after the entropy-coded data: a marker other than a stuffed byte or an RSTn. */
function scanEnd(b: Uint8Array, from: number): number {
	let j = from;
	while (j + 1 < b.length) {
		if (byte(b, j) !== 0xff) {
			j++;
			continue;
		}
		const n = byte(b, j + 1);
		if (n === 0x00 || (n >= 0xd0 && n <= 0xd7)) {
			j += 2;
			continue;
		}
		if (n === 0xff) {
			j++;
			continue;
		}
		return j;
	}
	return b.length;
}

/**
 * The frame size, the id of its first component (luma for a YCbCr file), and each component's
 * sampling factors as `HxV`, comma separated, in frame order.
 */
function sofOf(b: Uint8Array): Sof & { luma: number; sampling: string } {
	for (const seg of segments(b)) {
		if (SOF.has(seg.marker)) {
			const nf = byte(b, seg.payload + 5);
			const factors: string[] = [];
			for (let k = 0; k < nf; k++) {
				const hv = byte(b, seg.payload + 7 + 3 * k);
				factors.push(`${hv >> 4}x${hv & 15}`);
			}
			return {
				height: u16(b, seg.payload + 1),
				width: u16(b, seg.payload + 3),
				luma: byte(b, seg.payload + 6),
				sampling: factors.join(',')
			};
		}
	}
	throw new Error('no frame header');
}

/** Every scan from its SOS marker to the next marker, header and entropy data together. */
function scansOf(b: Uint8Array): Scan[] {
	const out: Scan[] = [];
	for (const seg of segments(b)) {
		if (seg.marker !== 0xda) continue;
		const ns = byte(b, seg.payload);
		const comps: number[] = [];
		for (let k = 0; k < ns; k++) comps.push(byte(b, seg.payload + 1 + 2 * k));
		out.push({
			ss: byte(b, seg.payload + 1 + 2 * ns),
			se: byte(b, seg.payload + 2 + 2 * ns),
			bytes: seg.end - seg.start,
			comps
		});
	}
	return out;
}

function displayed(s: Sof, orientation: number): Sof {
	return orientation >= 5 ? { width: s.height, height: s.width } : s;
}

// #endregion

// #region isobmff

type Box = { type: string; start: number; end: number };

function boxesIn(b: Uint8Array, start: number, end: number): Box[] {
	const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
	const out: Box[] = [];
	let i = start;
	while (i + 8 <= end) {
		let size = view.getUint32(i);
		const type = String.fromCharCode(...b.subarray(i + 4, i + 8));
		let header = 8;
		if (size === 1) {
			size = Number(view.getBigUint64(i + 8));
			header = 16;
		} else if (size === 0) {
			size = end - i;
		}
		if (size < header || i + size > end) throw new Error(`bad box ${type} at ${i}`);
		out.push({ type, start: i + header, end: i + size });
		i += size;
	}
	return out;
}

/** Counts `thmb` references under the top-level `meta` box's `iref`. */
function thumbnailRefs(b: Uint8Array): { thmbBoxes: number; thmbReferences: number } {
	const view = new DataView(b.buffer, b.byteOffset, b.byteLength);
	const meta = boxesIn(b, 0, b.length).find((x) => x.type === 'meta');
	const iref = meta && boxesIn(b, meta.start + 4, meta.end).find((x) => x.type === 'iref');
	let thmbBoxes = 0;
	let thmbReferences = 0;
	if (iref) {
		const idSize = byte(b, iref.start) === 0 ? 2 : 4;
		for (const ref of boxesIn(b, iref.start + 4, iref.end)) {
			if (ref.type !== 'thmb') continue;
			thmbBoxes++;
			thmbReferences += view.getUint16(ref.start + idSize);
		}
	}
	return { thmbBoxes, thmbReferences };
}

// #endregion

// #region measurement

async function decode(module: TinyImgModule, bytes: Uint8Array, width: number): Promise<Arm> {
	try {
		const { value, work } = await module.measure(async () => {
			const image = await Image.open(module, bytes);
			try {
				image.autoOrient();
				image.resize(width);
				const decision = image.decide();
				await image.bytes('jpeg');
				return { scale: decision.scale, outWidth: decision.output.width };
			} finally {
				image.dispose();
			}
		});
		return {
			ok: true,
			scale: value.scale,
			outWidth: value.outWidth,
			sourceSamples: work.sourceSamples,
			decodedSamples: work.decodedSamples,
			transformSamples: work.transformSamples,
			blocks: work.blocks,
			entropyBytes: work.entropyBytes,
			scansSkipped: work.scansSkipped
		};
	} catch (error) {
		return { ok: false, reason: errorText(error) };
	}
}

/** The widest width at or below an eighth of `width` for which `Image.decide()` picks denominator 8. */
async function widestAt8(module: TinyImgModule, bytes: Uint8Array, width: number): Promise<number> {
	for (let w = Math.floor(width / 8); w >= 1; w--) {
		const image = await Image.open(module, bytes);
		try {
			image.autoOrient();
			image.resize(w);
			if (image.decide().scale === 8) return w;
		} finally {
			image.dispose();
		}
	}
	return 0;
}

function compareAll(a: string, b: string): number {
	const r = spawnSync('magick', ['compare', '-metric', 'PSNR', a, b, 'null:'], {
		encoding: 'utf8'
	});
	if (r.error) throw r.error;
	if (r.status !== 0 && r.status !== 1) throw new Error(`magick compare exited ${r.status}`);
	const value = Number.parseFloat(r.stderr);
	if (!Number.isFinite(value)) throw new Error(`no PSNR in ${JSON.stringify(r.stderr)}`);
	return value;
}

// #endregion

// #region corpus

type ExifRow = {
	thumbLength: number | null;
	orientation: number | null;
	thumbOrientation: number | null;
};

function exifRows(files: string[]): Map<string, ExifRow> {
	const { stdout } = run('exiftool', [
		'-j',
		'-n',
		'-G1',
		'-ThumbnailLength',
		'-ThumbnailOffset',
		'-Orientation',
		...files
	]);
	const num = (v: unknown) => (typeof v === 'number' ? v : null);
	const out = new Map<string, ExifRow>();
	for (const row of JSON.parse(stdout) as Record<string, unknown>[]) {
		out.set(String(row['SourceFile']), {
			thumbLength: num(row['IFD1:ThumbnailLength']),
			orientation: num(row['IFD0:Orientation']),
			thumbOrientation: num(row['IFD1:Orientation'])
		});
	}
	return out;
}

function fileOf(e: Entry): string {
	if (e.source === 'local') return join(ROOT, e.path ?? '');
	const ext = e.format === 'jpeg' ? 'jpg' : e.format;
	return join(CORPUS, `${e.id}.${ext}`);
}

function readVerified(file: string, e: Entry): Uint8Array {
	const bytes = new Uint8Array(readFileSync(file));
	if (bytes.byteLength !== e.bytes)
		throw new Error(`${e.id}: ${bytes.byteLength} bytes, manifest says ${e.bytes}`);
	const digest = sha256(bytes);
	if (digest !== e.sha256)
		throw new Error(`${e.id}: sha256 ${digest} is not the manifest's ${e.sha256}`);
	return bytes;
}

function originOf(e: Entry): string {
	if (e.source === 'local') return 'local';
	if (e.derivedFrom) {
		if (e.derivedFrom.startsWith('kodak-')) return 'kodak-derived';
		if (e.derivedFrom.startsWith('commons-')) return 'commons-derived';
		return 'derived';
	}
	if (e.id.startsWith('kodak-')) return 'kodak';
	if (e.id.startsWith('commons-')) return 'commons';
	return 'other';
}

// #endregion

// #region thumbnails

function thumbnailOf(
	file: string,
	id: string,
	row: ExifRow,
	main: Sof,
	orientation: number
): { thumbnail: Thumb | null; note: string | null } {
	if (row.thumbLength === null) return { thumbnail: null, note: null };

	const extracted = spawnSync('exiftool', ['-b', '-IFD1:ThumbnailImage', file], {
		maxBuffer: 1 << 26
	});
	if (extracted.error) throw extracted.error;
	const data = new Uint8Array(extracted.stdout);
	if (data.byteLength !== row.thumbLength) {
		throw new Error(`${id}: extracted ${data.byteLength} bytes, IFD1 says ${row.thumbLength}`);
	}
	if (byte(data, 0) !== 0xff || byte(data, 1) !== 0xd8) {
		return { thumbnail: null, note: 'IFD1 thumbnail is not a JPEG' };
	}

	const thumbFile = join(WORK, `${id}.thumb.jpg`);
	writeFileSync(thumbFile, data);
	const thumb = sofOf(data);

	const [w, h] = run('identify', ['-format', '%w %h', thumbFile]).stdout.trim().split(/\s+/);
	const identifyMatches = Number(w) === thumb.width && Number(h) === thumb.height;

	const refFile = join(WORK, `${id}.ref.png`);
	const thumbPng = join(WORK, `${id}.thumb.png`);
	run('magick', [
		file,
		'-filter',
		'box',
		'-resize',
		`${thumb.width}x${thumb.height}!`,
		'-alpha',
		'off',
		'-type',
		'TrueColor',
		`PNG24:${refFile}`
	]);
	run('magick', [thumbFile, '-alpha', 'off', '-type', 'TrueColor', `PNG24:${thumbPng}`]);
	const psnrDb = compareAll(thumbPng, refFile);

	const aspectMismatchPct =
		Math.abs(thumb.width / thumb.height / (main.width / main.height) - 1) * 100;
	const thumbOrientation = row.thumbOrientation;
	const orientationState: Thumb['orientation'] =
		thumbOrientation === null
			? 'absent'
			: thumbOrientation === orientation
				? 'agree'
				: 'disagree';
	const stale = psnrDb < PSNR_FLOOR_DB || aspectMismatchPct > ASPECT_TOLERANCE_PCT;

	return {
		thumbnail: {
			bytes: data.byteLength,
			width: thumb.width,
			height: thumb.height,
			shownWidth: displayed(thumb, orientation).width,
			identifyMatches,
			psnrDb,
			aspectMismatchPct,
			orientation: orientationState,
			stale
		},
		note: null
	};
}

// #endregion

async function jpegCell(module: TinyImgModule, e: Entry, row: ExifRow | undefined): Promise<Jpeg> {
	if (!row) throw new Error(`${e.id}: exiftool returned no row`);
	const file = fileOf(e);
	const bytes = readVerified(file, e);
	const main = sofOf(bytes);
	const orientation = row.orientation ?? 1;
	const shown = displayed(main, orientation);
	const scans = scansOf(bytes);
	const dc = scans.filter((s) => s.ss === 0 && s.se === 0);
	const sumBytes = (xs: Scan[]) => xs.reduce((sum, s) => sum + s.bytes, 0);
	const ac = scans.filter((s) => s.ss > 0);
	const acLuma = ac.filter((s) => s.comps.includes(main.luma));
	const acChroma = ac.filter((s) => !s.comps.includes(main.luma));
	const { thumbnail, note } = thumbnailOf(file, e.id, row, main, orientation);

	const w8 = await widestAt8(module, bytes, shown.width);
	const arms: Record<ArmName, Arm> = {
		p160: await decode(module, bytes, 160),
		p1200: await decode(module, bytes, 1200),
		p8:
			w8 > 0
				? await decode(module, bytes, w8)
				: { ok: false, reason: 'no output width gets denominator 8' }
	};

	return {
		id: e.id,
		split: e.split,
		origin: originOf(e),
		bytes: bytes.byteLength,
		width: main.width,
		height: main.height,
		shownWidth: shown.width,
		orientation,
		sizeMatchesManifest: main.width === e.width && main.height === e.height,
		progressive: e.progressive === true,
		scans: scans.length,
		scanBytes: sumBytes(scans),
		dcScans: dc.length,
		dcScanBytes: sumBytes(dc),
		acLumaBytes: sumBytes(acLuma),
		acChromaBytes: sumBytes(acChroma),
		sampling: main.sampling,
		thumbnail,
		thumbnailNote: note,
		arms
	};
}

function avifCell(id: string, split: string, origin: string, file: string): Avif {
	const bytes = new Uint8Array(readFileSync(file));
	return { id, split, origin, bytes: bytes.byteLength, ...thumbnailRefs(bytes) };
}

// #region summary

function okOf(a: Arm) {
	return a.ok ? a : null;
}

function jpegCounts(label: string[], group: Jpeg[]): string[] {
	const thumbs = group.flatMap((x) => (x.thumbnail ? [x.thumbnail] : []));
	return [
		...label,
		String(group.length),
		String(group.filter((x) => x.progressive).length),
		String(thumbs.length),
		String(thumbs.filter((t) => t.shownWidth >= 160).length),
		String(thumbs.filter((t) => t.shownWidth >= 160 && !t.stale).length)
	];
}

function faithfulnessRow(label: string, group: Jpeg[]): string[] {
	const thumbs = group.flatMap((x) => (x.thumbnail ? [x.thumbnail] : []));
	const count = (f: (t: Thumb) => boolean) => String(thumbs.filter(f).length);
	return [
		label,
		String(thumbs.length),
		count((t) => t.psnrDb < PSNR_FLOOR_DB),
		count((t) => t.aspectMismatchPct > ASPECT_TOLERANCE_PCT),
		count((t) => t.stale),
		count((t) => t.orientation === 'agree'),
		count((t) => t.orientation === 'disagree'),
		count((t) => t.orientation === 'absent'),
		count((t) => t.identifyMatches),
		spread(
			thumbs.map((t) => t.psnrDb),
			2
		)
	];
}

function applicabilityRow(width: number, cal: Jpeg[], test: Jpeg[]): string[] {
	const all = [...cal, ...test];
	const at = (group: Jpeg[]) =>
		group.filter((x) => x.thumbnail && x.thumbnail.shownWidth >= width);
	const fresh = (group: Jpeg[]) => at(group).filter((x) => x.thumbnail && !x.thumbnail.stale);
	return [
		String(width),
		String(at(cal).length),
		String(at(test).length),
		String(at(all).length),
		String(fresh(all).length)
	];
}

/** The files whose p8 arm the planner actually decoded at denominator 8. */
function at8(group: Jpeg[]): { x: Jpeg; a: Extract<Arm, { ok: true }> }[] {
	return group.flatMap((x) => {
		const a = okOf(x.arms.p8);
		return a && a.scale === 8 ? [{ x, a }] : [];
	});
}

function progressiveRow(label: string, group: Jpeg[]): string[] {
	const prog = group.filter((x) => x.progressive);
	const den8 = at8(prog);
	const den8At160 = prog.filter((x) => okOf(x.arms.p160)?.scale === 8).length;
	const wide = prog.filter((x) => x.shownWidth >= 1280).length;
	return [
		label,
		String(prog.length),
		String(den8At160),
		String(wide),
		String(den8.length),
		spread(den8.map(({ x }) => x.dcScanBytes / x.bytes)),
		spread(den8.map(({ x }) => x.acLumaBytes / x.bytes)),
		spread(den8.map(({ x }) => x.acChromaBytes / x.bytes)),
		spread(den8.map(({ x, a }) => a.entropyBytes / x.bytes)),
		spread(den8.map(({ x, a }) => a.entropyBytes / (x.dcScanBytes + x.acChromaBytes))),
		spread(
			prog.flatMap((x) => {
				const a = okOf(x.arms.p160);
				return a ? [a.entropyBytes / x.bytes] : [];
			})
		)
	];
}

function samplingRows(group: Jpeg[]): string[][] {
	const den8 = at8(group.filter((x) => x.progressive));
	const keys = [...new Set(den8.map(({ x }) => x.sampling))].sort();
	return keys.map((key) => {
		const rows = den8.filter(({ x }) => x.sampling === key);
		return [
			key,
			String(rows.length),
			spread(rows.map(({ x, a }) => a.entropyBytes / x.dcScanBytes)),
			spread(rows.map(({ x, a }) => a.entropyBytes / (x.dcScanBytes + x.acChromaBytes))),
			spread(rows.map(({ x }) => x.acChromaBytes / x.bytes))
		];
	});
}

function baselineRow(label: string, group: Jpeg[]): string[] {
	const base = group.filter((x) => !x.progressive);
	const den8 = at8(base);
	return [
		label,
		String(base.length),
		String(den8.length),
		spread(den8.map(({ x, a }) => a.entropyBytes / x.scanBytes))
	];
}

function denominatorRows(group: Jpeg[]): string[][] {
	return [1, 2, 4, 8].map((d) => [
		`1/${d}`,
		String(group.filter((x) => okOf(x.arms.p160)?.scale === d).length),
		String(group.filter((x) => okOf(x.arms.p1200)?.scale === d).length),
		String(group.filter((x) => okOf(x.arms.p8)?.scale === d).length)
	]);
}

function zeroDecodeRow(label: string, group: Jpeg[]): string[] {
	const eligible = group.filter(
		(x) => x.thumbnail && !x.thumbnail.stale && x.thumbnail.shownWidth >= 160
	);
	const at = (xs: Jpeg[], f: (a: Extract<Arm, { ok: true }>) => number) =>
		xs.flatMap((x) => {
			const a = okOf(x.arms.p160);
			return a ? [f(a)] : [];
		});
	return [
		label,
		`${eligible.length} of ${group.length}`,
		spread(
			at(eligible, (a) => a.entropyBytes),
			0
		),
		spread(
			at(eligible, (a) => a.decodedSamples),
			0
		),
		spread(
			at(group, (a) => a.entropyBytes),
			0
		)
	];
}

function summaryMarkdown(cells: Cells, decision: string | null): string {
	const j = cells.jpeg;
	const cal = j.filter((x) => x.split === 'cal');
	const test = j.filter((x) => x.split === 'test');
	const origins = [...new Set(j.map((x) => x.origin))].sort();
	const avifRows = (split: string) =>
		cells.avif.filter((a) => split === 'all' || a.split === split);
	const avifLine = (label: string, group: Avif[]) => [
		label,
		String(group.length),
		String(group.filter((a) => a.thmbBoxes > 0).length),
		String(group.reduce((sum, a) => sum + a.thmbReferences, 0))
	];

	const sections: string[] = [];
	sections.push(
		'# Free-plan counts',
		'',
		`Generated by \`scripts/measure/free-counts.ts\`. Wasm \`${cells.meta.wasmSha256}\` (${cells.meta.wasmVersion}). Manifest \`${cells.meta.manifestSha256}\`. Stale rule: PSNR under ${PSNR_FLOOR_DB} dB or aspect off by more than ${ASPECT_TOLERANCE_PCT} percent. Splits are labelled everywhere; nothing is fitted.`,
		'',
		'Request arms: ' +
			ARMS.map((n) => `\`${n}\` ${ARM_TEXT[n]}`).join('; ') +
			'. Widths are on the displayed (EXIF-oriented) image.',
		'',
		'## JPEG corpus by origin and split',
		'',
		table(
			[
				'origin',
				'split',
				'JPEGs',
				'progressive',
				'EXIF thumbnail',
				'thumbnail at least 160 px',
				'of those, not stale'
			],
			[
				...origins.flatMap((o) =>
					(['cal', 'test'] as const).map((s) =>
						jpegCounts(
							[o, s],
							j.filter((x) => x.origin === o && x.split === s)
						)
					)
				),
				jpegCounts(['all', 'cal'], cal),
				jpegCounts(['all', 'test'], test),
				jpegCounts(['all', 'all'], j)
			]
		),
		'',
		`JPEGs with an EXIF thumbnail: ${j.filter((x) => x.thumbnail).length} of ${j.length}.`,
		`Non-JPEG IFD1 thumbnails: ${j.filter((x) => x.thumbnailNote).length}.`,
		`Manifest width and height disagree with the SOF marker: ${j.filter((x) => !x.sizeMatchesManifest).length}.`,
		'',
		'## EXIF thumbnail faithfulness',
		'',
		table(
			[
				'split',
				'thumbnails',
				'PSNR under 20 dB',
				'aspect off over 1%',
				'stale (either)',
				'orientation agrees',
				'orientation disagrees',
				'orientation absent',
				'identify agrees with SOF',
				'PSNR median (min to max)'
			],
			[faithfulnessRow('cal', cal), faithfulnessRow('test', test), faithfulnessRow('all', j)]
		),
		'',
		'## Applicability by requested width (thumbnail displayed width at least the request)',
		'',
		table(
			['width', 'cal', 'test', 'all', 'all, not stale'],
			WIDTHS.map((w) => applicabilityRow(w, cal, test))
		),
		'',
		'## Zero-decode at 160 px (eligible = not stale and displayed width at least 160)',
		'',
		table(
			[
				'split',
				'eligible',
				'entropy bytes read today at 160 px, eligible files',
				'decoded samples at 160 px, eligible files',
				'entropy bytes read today at 160 px, all JPEGs'
			],
			[zeroDecodeRow('cal', cal), zeroDecodeRow('test', test), zeroDecodeRow('all', j)]
		),
		'',
		'## AVIF `thmb` references',
		'',
		table(
			['split', 'AVIF files', 'with a thmb reference', 'thmb references'],
			[
				avifLine('cal', avifRows('cal')),
				avifLine('test', avifRows('test')),
				avifLine('fixture', avifRows('fixture')),
				avifLine('all', avifRows('all'))
			]
		),
		'',
		'`avifenc` does not write a `thmb` item, so the corpus count is zero by construction and says nothing about camera output. `avifdec --info` prints no thumbnail line, so the count rests on the box walk alone.',
		'',
		'## Progressive JPEG at 1/8',
		'',
		'Columns: progressive files; files where the 160 px request decodes at denominator 8; files whose displayed width is at least 1280 (necessary for a 160 px request at denominator 8, not sufficient, because the rounded height has to fit too); files the p8 arm decoded at denominator 8. Over those, as shares of file bytes: DC scans, luma AC scans, chroma AC scans, and the entropy bytes the decoder read. The last column divides the entropy bytes by DC plus chroma AC bytes, the reads expected if the decoder skips luma AC and nothing else.',
		'',
		table(
			[
				'split',
				'progressive',
				'160 px at 1/8',
				'displayed width at least 1280',
				'p8 at 1/8',
				'DC scans',
				'luma AC scans',
				'chroma AC scans',
				'entropy read',
				'entropy read / (DC + chroma AC)',
				'160 px: entropy read, all progressive'
			],
			[progressiveRow('cal', cal), progressiveRow('test', test), progressiveRow('all', j)]
		),
		'',
		'Progressive files at 1/8 by sampling factors (`HxV` per component, frame order). A ratio near 1 against DC scans means the decoder read no chroma AC; a ratio near 1 against DC plus chroma AC means it read all of it:',
		'',
		table(
			[
				'sampling',
				'files at 1/8',
				'entropy read / DC scans',
				'entropy read / (DC + chroma AC)',
				'chroma AC / file bytes'
			],
			samplingRows(j)
		),
		'',
		'Baseline JPEGs, for contrast (no DC-only path applies to these), over the files the p8 arm decoded at denominator 8. The denominator is all scan bytes, so a complete read is 1:',
		'',
		table(
			['split', 'baseline JPEGs', 'p8 at 1/8', 'entropy read / scan bytes'],
			[baselineRow('cal', cal), baselineRow('test', test), baselineRow('all', j)]
		),
		'',
		'Denominator the planner picked, by arm, all JPEGs:',
		'',
		table(['denominator', 'p160', 'p1200', 'p8'], denominatorRows(j)),
		'',
		`Failed arms: ${ARMS.map((n) => `${n} ${j.filter((x) => !x.arms[n].ok).length}`).join(', ')}. Reasons are in \`cells.json\`.`,
		''
	);

	sections.push('## What this decides', '');
	sections.push(
		decision ? decision.trim() : '(decision.md not present; no decision written)',
		''
	);
	return sections.join('\n');
}

// #endregion

async function main(): Promise<void> {
	const wasmPath = resolve(ROOT, arg('--wasm') ?? 'bin/tinyimg.wasm');
	const outDir = resolve(ROOT, arg('--out') ?? 'bench/baselines/free-counts');
	mkdirSync(WORK, { recursive: true });
	mkdirSync(outDir, { recursive: true });

	const wasm = new Uint8Array(readFileSync(wasmPath));
	const module = await TinyImgModule.loadBytes(wasm);
	const manifestBytes = readFileSync(MANIFEST);
	const entries = (JSON.parse(manifestBytes.toString('utf8')) as { entries: Entry[] }).entries;

	const jpegEntries = entries
		.filter((e) => e.format === 'jpeg')
		.sort((a, b) => (a.id < b.id ? -1 : 1));
	const avifEntries = entries
		.filter((e) => e.format === 'avif')
		.sort((a, b) => (a.id < b.id ? -1 : 1));
	const jpegFiles = jpegEntries.map(fileOf);
	const rows = exifRows(jpegFiles);

	const jpeg: Jpeg[] = [];
	for (const e of jpegEntries) {
		jpeg.push(await jpegCell(module, e, rows.get(fileOf(e))));
	}

	const avif: Avif[] = [
		...avifEntries.map((e) => avifCell(e.id, e.split, originOf(e), fileOf(e))),
		...['fox', 'dartmouth'].map((name) =>
			avifCell(`fixture-${name}`, 'fixture', 'fixture', join(FIXTURES, `${name}.avif`))
		)
	];

	const tool = (cmd: string, args: string[]) =>
		run(cmd, args).stdout.split('\n')[0]?.trim() ?? '';
	const cells: Cells = {
		meta: {
			wasm: basename(wasmPath),
			wasmSha256: sha256(wasm),
			wasmVersion: module.version.join('.'),
			manifestSha256: sha256(manifestBytes),
			tools: {
				exiftool: tool('exiftool', ['-ver']),
				magick: tool('magick', ['-version']),
				avifdec: run('avifdec', ['--version'], [0, 1]).stdout.split('\n')[0]?.trim() ?? ''
			},
			thresholds: { psnrFloorDb: PSNR_FLOOR_DB, aspectTolerancePct: ASPECT_TOLERANCE_PCT },
			arms: ARM_TEXT,
			widths: WIDTHS
		},
		jpeg,
		avif
	};

	const decisionPath = join(outDir, 'decision.md');
	const decision = existsSync(decisionPath) ? readFileSync(decisionPath, 'utf8') : null;
	writeFileSync(join(outDir, 'cells.json'), `${JSON.stringify(cells, null, '\t')}\n`);
	writeFileSync(join(outDir, 'summary.md'), summaryMarkdown(cells, decision));
	console.log(`wrote ${jpeg.length} JPEG and ${avif.length} AVIF cells to ${outDir}`);
}

await main();
