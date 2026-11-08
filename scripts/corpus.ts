/**
 * Fetches and verifies the held-out image corpus named by `scripts/corpus-manifest.json`.
 *
 * ```sh
 * bun scripts/corpus.ts fetch    # download every entry into corpus/, derive the rest, write LICENSES.md
 * bun scripts/corpus.ts check    # verify bytes, sha256, header facts and split offline
 * ```
 *
 * The manifest is the record. A downloaded or derived file whose sha256 differs from the manifest is
 * refused, so a re-uploaded source cannot change the test set without the manifest changing with it.
 * Header facts are read from the bytes, never from an API response.
 */

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CORPUS = join(ROOT, 'corpus');
const MANIFEST = join(ROOT, 'scripts', 'corpus-manifest.json');
const USER_AGENT = 'tinyimg-corpus/1.0 (https://github.com/gmitch215/tinyimg)';

export const FORMATS = ['jpeg', 'png', 'webp', 'gif', 'tiff', 'bmp', 'avif'] as const;
export type Format = (typeof FORMATS)[number];

export const BANDS = ['under 0.25', '0.25 to 1', '1 to 4', '4 to 12', 'over 12'] as const;
export type Band = (typeof BANDS)[number];

export type Split = 'cal' | 'test';

/** One manifest row. Optional fields are present only where the source or the derivation defines them. */
export type Entry = {
	id: string;
	url: string | null;
	source?: 'local';
	path?: string;
	page?: string;
	license: string;
	licenseUrl: string | null;
	licenseNote?: string;
	author?: string;
	derivedFrom?: string;
	command?: string[];
	tool?: string;
	sha256: string;
	bytes: number;
	width: number;
	height: number;
	format: Format;
	progressive?: boolean;
	subsampling?: string;
	restartInterval?: number;
	split: Split;
};

export type Header = {
	format: Format;
	width: number;
	height: number;
	progressive?: boolean;
	subsampling?: string;
	restartInterval?: number;
};

/** The calibration/test split, fixed by content. Mirrored in tests/node/corpus.spec.ts. */
export function splitOf(sha256: string): Split {
	return parseInt(sha256.slice(0, 8), 16) % 5 < 3 ? 'cal' : 'test';
}

/** Megapixel bands use [0.25, 1, 4, 12) on width times height over one million. */
export function bandOf(width: number, height: number): Band {
	const mp = (width * height) / 1_000_000;
	if (mp < 0.25) return 'under 0.25';
	if (mp < 1) return '0.25 to 1';
	if (mp < 4) return '1 to 4';
	if (mp < 12) return '4 to 12';
	return 'over 12';
}

/** Lowercase hex sha256 of a byte array. */
export function sha256Of(bytes: Uint8Array): string {
	return createHash('sha256').update(bytes).digest('hex');
}

function byteAt(b: Uint8Array, i: number): number {
	const x = b[i];
	if (x === undefined) throw new RangeError(`read past end of buffer at ${i}`);
	return x;
}

function u16be(b: Uint8Array, i: number): number {
	return (byteAt(b, i) << 8) | byteAt(b, i + 1);
}

function u16le(b: Uint8Array, i: number): number {
	return byteAt(b, i) | (byteAt(b, i + 1) << 8);
}

function u24le(b: Uint8Array, i: number): number {
	return u16le(b, i) | (byteAt(b, i + 2) << 16);
}

function u32le(b: Uint8Array, i: number): number {
	return (u16le(b, i) | (u16le(b, i + 2) << 16)) >>> 0;
}

function u32be(b: Uint8Array, i: number): number {
	return ((u16be(b, i) << 16) | u16be(b, i + 2)) >>> 0;
}

function ascii(b: Uint8Array, i: number, n: number): string {
	return String.fromCharCode(...b.subarray(i, i + n));
}

const SOF_PROGRESSIVE = [0xc2, 0xc6, 0xca, 0xce];
const SOF_ANY = [0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf];

function subsamplingOf(comps: Array<{ h: number; v: number }>): string {
	if (comps.length === 1) return 'gray';
	if (comps.length === 4) return 'cmyk';
	const y = comps[0];
	const c = comps[1];
	if (!y || !c) return 'other';
	if (c.h === y.h && c.v === y.v) return '4:4:4';
	if (c.h * 2 === y.h && c.v === y.v) return '4:2:2';
	if (c.h * 2 === y.h && c.v * 2 === y.v) return '4:2:0';
	if (c.h === y.h && c.v * 2 === y.v) return '4:4:0';
	if (c.h * 4 === y.h && c.v === y.v) return '4:1:1';
	return 'other';
}

/** Walks marker segments up to the first SOS, so a header-only prefix of a file is enough. */
function parseJpeg(b: Uint8Array): Header {
	let width = 0;
	let height = 0;
	let progressive = false;
	let restartInterval = 0;
	let sof = false;
	let comps: Array<{ h: number; v: number }> = [];
	let p = 2;

	for (;;) {
		if (byteAt(b, p) !== 0xff) throw new Error(`jpeg: no marker at ${p}`);
		let m = byteAt(b, p + 1);
		while (m === 0xff) m = byteAt(b, ++p + 1);
		p += 2;
		if (m === 0xd8 || m === 0x01 || (m >= 0xd0 && m <= 0xd7)) continue;
		if (m === 0xda) break;

		const len = u16be(b, p);
		const seg = p + 2;
		if (m === 0xdd) restartInterval = u16be(b, seg);
		if (!sof && SOF_ANY.includes(m)) {
			sof = true;
			progressive = SOF_PROGRESSIVE.includes(m);
			height = u16be(b, seg + 1);
			width = u16be(b, seg + 3);
			const n = byteAt(b, seg + 5);
			comps = [];
			for (let i = 0; i < n; i++) {
				const hv = byteAt(b, seg + 6 + i * 3 + 1);
				comps.push({ h: hv >> 4, v: hv & 0x0f });
			}
		}
		p += len;
	}

	if (!sof) throw new Error('jpeg: no frame header before the first scan');
	const out: Header = { format: 'jpeg', width, height, progressive };
	out.subsampling = subsamplingOf(comps);
	out.restartInterval = restartInterval;
	return out;
}

function parseWebp(b: Uint8Array): Header {
	const chunk = ascii(b, 12, 4);
	if (chunk === 'VP8 ') {
		const width = u16le(b, 26) & 0x3fff;
		const height = u16le(b, 28) & 0x3fff;
		return { format: 'webp', width, height };
	}
	if (chunk === 'VP8L') {
		const bits = u32le(b, 21);
		return { format: 'webp', width: (bits & 0x3fff) + 1, height: ((bits >>> 14) & 0x3fff) + 1 };
	}
	if (chunk === 'VP8X') {
		return { format: 'webp', width: u24le(b, 24) + 1, height: u24le(b, 27) + 1 };
	}
	throw new Error(`webp: unknown first chunk ${chunk}`);
}

function parseTiff(b: Uint8Array): Header {
	const le = ascii(b, 0, 2) === 'II';
	const u16 = (i: number) => (le ? u16le(b, i) : u16be(b, i));
	const u32 = (i: number) => (le ? u32le(b, i) : u32be(b, i));
	const ifd = u32(4);
	const count = u16(ifd);
	let width = 0;
	let height = 0;
	for (let i = 0; i < count; i++) {
		const e = ifd + 2 + i * 12;
		const tag = u16(e);
		const type = u16(e + 2);
		const value = type === 3 ? u16(e + 8) : u32(e + 8);
		if (tag === 256) width = value;
		if (tag === 257) height = value;
	}
	if (!width || !height) throw new Error('tiff: no ImageWidth or ImageLength tag');
	return { format: 'tiff', width, height };
}

/** The major brand is often mif1, with avif only in the compatible brands. */
function isAvifBrand(b: Uint8Array): boolean {
	const end = Math.min(u32be(b, 0), b.length);
	for (let i = 8; i + 4 <= end; i += 4) {
		const brand = ascii(b, i, 4);
		if (brand === 'avif' || brand === 'avis') return true;
	}
	return false;
}

/** Reads the magic bytes and the header fields the manifest records. Throws on anything unknown. */
export function parseHeader(b: Uint8Array): Header {
	if (byteAt(b, 0) === 0xff && byteAt(b, 1) === 0xd8) return parseJpeg(b);
	if (ascii(b, 1, 3) === 'PNG') {
		return { format: 'png', width: u32be(b, 16), height: u32be(b, 20) };
	}
	if (ascii(b, 0, 3) === 'GIF') {
		return { format: 'gif', width: u16le(b, 6), height: u16le(b, 8) };
	}
	if (ascii(b, 0, 2) === 'BM') {
		const width = u32le(b, 18) | 0;
		const height = Math.abs(u32le(b, 22) | 0);
		return { format: 'bmp', width, height };
	}
	if (ascii(b, 0, 4) === 'RIFF' && ascii(b, 8, 4) === 'WEBP') return parseWebp(b);
	if (ascii(b, 0, 4) === 'II*\0' || ascii(b, 0, 4) === 'MM\0*') return parseTiff(b);
	if (ascii(b, 4, 4) === 'ftyp' && isAvifBrand(b)) {
		const ispe = Buffer.from(b.buffer, b.byteOffset, b.byteLength).indexOf('ispe');
		if (ispe < 0) throw new Error('avif: no ispe box');
		return { format: 'avif', width: u32be(b, ispe + 8), height: u32be(b, ispe + 12) };
	}
	throw new Error('unrecognized image format');
}

function loadEntries(): Entry[] {
	const parsed = JSON.parse(readFileSync(MANIFEST, 'utf8')) as { entries: Entry[] };
	return parsed.entries;
}

/** Local entries stay where they are in the repository; everything else lives in corpus/. */
function fileOf(e: Entry): string {
	if (e.source === 'local') return join(ROOT, e.path ?? '');
	const ext = e.format === 'jpeg' ? 'jpg' : e.format === 'tiff' ? 'tif' : e.format;
	return join(CORPUS, `${e.id}.${ext}`);
}

async function download(e: Entry, file: string): Promise<void> {
	if (!e.url) throw new Error(`${e.id}: no url`);
	const res = await fetch(e.url, {
		headers: { 'User-Agent': USER_AGENT },
		signal: AbortSignal.timeout(20_000)
	});
	if (!res.ok) throw new Error(`${e.id}: HTTP ${res.status} from ${e.url}`);
	const bytes = new Uint8Array(await res.arrayBuffer());
	verify(e, bytes, e.url);
	writeAtomic(file, bytes);
}

function derive(e: Entry, file: string, all: Entry[]): void {
	const source = all.find((s) => s.id === e.derivedFrom);
	if (!source) throw new Error(`${e.id}: derivedFrom ${e.derivedFrom} is not in the manifest`);
	const input = fileOf(source);
	if (!existsSync(input)) throw new Error(`${e.id}: input ${input} is missing; fetch it first`);
	if (!e.command) throw new Error(`${e.id}: no command recorded`);

	const part = `${file}.part.${file.split('.').pop()}`;
	rmSync(part, { force: true });
	const [tool, ...args] = e.command.map((a) =>
		a.replaceAll('{in}', input).replaceAll('{out}', part)
	);
	if (!tool) throw new Error(`${e.id}: empty command`);
	execFileSync(tool, args, { stdio: 'pipe' });

	const bytes = new Uint8Array(readFileSync(part));
	try {
		verify(e, bytes, `derived with ${e.tool ?? tool}`);
	} catch (err) {
		rmSync(part, { force: true });
		throw err;
	}
	renameSync(part, file);
}

function verify(e: Entry, bytes: Uint8Array, source: string): void {
	const hash = sha256Of(bytes);
	if (hash !== e.sha256 || bytes.length !== e.bytes) {
		throw new Error(
			`${e.id}: refused, ${source} gave ${bytes.length} bytes sha256 ${hash}; ` +
				`manifest says ${e.bytes} bytes sha256 ${e.sha256}`
		);
	}
}

function writeAtomic(file: string, bytes: Uint8Array): void {
	const part = `${file}.part`;
	writeFileSync(part, bytes);
	renameSync(part, file);
}

function licensesMarkdown(list: Entry[]): string {
	const byLicense = new Map<string, Entry[]>();
	for (const e of list) {
		const group = byLicense.get(e.license) ?? [];
		group.push(e);
		byLicense.set(e.license, group);
	}
	const out = [
		'# Corpus licenses',
		'',
		'Generated by `bun scripts/corpus.ts fetch` from the manifest.',
		''
	];
	for (const [license, group] of [...byLicense].sort(([a], [b]) => a.localeCompare(b))) {
		const first = group[0];
		if (!first) continue;
		out.push(`## ${license}`, '', `License: ${first.licenseUrl ?? 'none stated by the owner'}`);
		if (first.licenseNote) out.push('', first.licenseNote);
		out.push('');
		for (const e of group) {
			const source = e.derivedFrom
				? `derived from ${e.derivedFrom}`
				: (e.page ?? e.url ?? '');
			const by = e.author ? `${e.author}, ` : '';
			out.push(`- \`${basename(fileOf(e))}\`: ${by}${source}`);
		}
		out.push('');
	}
	return out.join('\n');
}

async function runFetch(): Promise<number> {
	const entries = loadEntries();
	mkdirSync(CORPUS, { recursive: true });
	const remote = entries.filter((e) => e.source !== 'local');
	const ordered = [...remote.filter((e) => e.url), ...remote.filter((e) => !e.url)];
	const failures: string[] = [];
	let kept = 0;
	let fetched = 0;

	for (const e of ordered) {
		const file = fileOf(e);
		if (existsSync(file) && sha256Of(new Uint8Array(readFileSync(file))) === e.sha256) {
			kept++;
			continue;
		}
		try {
			if (e.url) await download(e, file);
			else derive(e, file, entries);
			fetched++;
		} catch (err) {
			failures.push(err instanceof Error ? err.message : String(err));
		}
	}

	writeFileSync(join(CORPUS, 'LICENSES.md'), licensesMarkdown(entries));
	console.log(`fetch: ${entries.length} entries, ${kept} already matched, ${fetched} written`);
	for (const f of failures) console.error(f);
	return failures.length === 0 ? 0 : 1;
}

function runCheck(): number {
	const entries = loadEntries();
	const problems: string[] = [];
	const perFormat = new Map<string, number>();
	const perSplit = new Map<string, number>();
	const perBand = new Map<string, number>();
	let totalBytes = 0;
	let overCap = 0;

	for (const e of entries) {
		const file = fileOf(e);
		if (!existsSync(file)) {
			problems.push(`${e.id}: missing ${file}`);
			continue;
		}
		const bytes = new Uint8Array(readFileSync(file));
		if (bytes.length !== e.bytes)
			problems.push(`${e.id}: ${bytes.length} bytes, manifest ${e.bytes}`);
		if (sha256Of(bytes) !== e.sha256) problems.push(`${e.id}: sha256 mismatch`);

		try {
			const h = parseHeader(bytes);
			const facts: Array<[string, unknown, unknown]> = [
				['format', h.format, e.format],
				['width', h.width, e.width],
				['height', h.height, e.height]
			];
			if (e.format === 'jpeg') {
				facts.push(
					['progressive', h.progressive, e.progressive],
					['subsampling', h.subsampling, e.subsampling],
					['restartInterval', h.restartInterval, e.restartInterval]
				);
			}
			for (const [name, got, want] of facts) {
				if (got !== want)
					problems.push(`${e.id}: ${name} is ${got} in bytes, manifest ${want}`);
			}
		} catch (err) {
			problems.push(`${e.id}: ${err instanceof Error ? err.message : String(err)}`);
		}

		if (splitOf(e.sha256) !== e.split)
			problems.push(`${e.id}: split ${e.split} does not follow the rule`);

		totalBytes += bytes.length;
		const band = bandOf(e.width, e.height);
		if (band === 'over 12') overCap++;
		perFormat.set(e.format, (perFormat.get(e.format) ?? 0) + 1);
		perSplit.set(e.split, (perSplit.get(e.split) ?? 0) + 1);
		perBand.set(band, (perBand.get(band) ?? 0) + 1);
	}

	console.log(`check: ${entries.length} entries, ${totalBytes} bytes on disk`);
	console.log(`formats: ${FORMATS.map((f) => `${f} ${perFormat.get(f) ?? 0}`).join(', ')}`);
	console.log(`splits: cal ${perSplit.get('cal') ?? 0}, test ${perSplit.get('test') ?? 0}`);
	console.log(`bands: ${BANDS.map((b) => `${b} ${perBand.get(b) ?? 0}`).join(', ')}`);
	console.log(`over 12 Mpx: ${overCap} entries`);
	for (const p of problems) console.error(p);
	console.log(`mismatches: ${problems.length}`);
	return problems.length === 0 ? 0 : 1;
}

async function main(): Promise<number> {
	const cmd = process.argv[2];
	if (cmd === 'fetch') return runFetch();
	if (cmd === 'check') return runCheck();
	console.error('usage: bun scripts/corpus.ts fetch | check');
	return 2;
}

if (import.meta.main) {
	process.exitCode = await main();
}
