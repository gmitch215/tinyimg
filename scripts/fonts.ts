/**
 * Builds the font fixtures the text tests read.
 *
 * The blobs are gitignored, so a test that only read `blobs/fonts` would not gate in CI. These are
 * committed instead: a real face cut down to something small enough to commit, and three synthesized
 * ones whose contents are known exactly.
 */

/** What a build produced, for the caller to report. */
export interface FontSummary {
	glyphs: number;
	codepoints: number;
	bytes: number;
}

function tag(name: string): number {
	return (
		((name.charCodeAt(0) << 24) |
			(name.charCodeAt(1) << 16) |
			(name.charCodeAt(2) << 8) |
			name.charCodeAt(3)) >>>
		0
	);
}

interface Table {
	offset: number;
	length: number;
}

function directory(font: Buffer): Map<string, Table> {
	const out = new Map<string, Table>();
	const count = font.readUInt16BE(4);

	for (let i = 0; i < count; i++) {
		const at = 12 + 16 * i;
		out.set(font.toString('latin1', at, at + 4), {
			offset: font.readUInt32BE(at + 8),
			length: font.readUInt32BE(at + 12)
		});
	}

	return out;
}

/** Every codepoint-to-glyph pair in a format 4 subtable. */
function readCmap4(font: Buffer, at: number): Map<number, number> {
	const out = new Map<number, number>();
	const segments = font.readUInt16BE(at + 6) / 2;
	const ends = at + 14;
	const starts = ends + 2 * segments + 2;
	const deltas = starts + 2 * segments;
	const ranges = deltas + 2 * segments;

	for (let i = 0; i < segments; i++) {
		const end = font.readUInt16BE(ends + 2 * i);
		const start = font.readUInt16BE(starts + 2 * i);
		const delta = font.readUInt16BE(deltas + 2 * i);
		const range = font.readUInt16BE(ranges + 2 * i);

		if (start === 0xffff) continue;

		for (let code = start; code <= end && code !== 0x10000; code++) {
			let glyph: number;

			if (range === 0) glyph = (code + delta) & 0xffff;
			else {
				const read = font.readUInt16BE(ranges + 2 * i + range + 2 * (code - start));
				if (read === 0) continue;
				glyph = (read + delta) & 0xffff;
			}

			if (glyph !== 0) out.set(code, glyph);
		}
	}

	return out;
}

function locaAt(font: Buffer, loca: Table, long: boolean, index: number): number {
	return long
		? font.readUInt32BE(loca.offset + 4 * index)
		: 2 * font.readUInt16BE(loca.offset + 2 * index);
}

/**
 * Walks a composite glyph's components, optionally renumbering them in place.
 *
 * One walk for both jobs, because a reader and a writer that disagree about how long a component
 * record is would find different components and the second one would corrupt the glyph.
 *
 * @param glyph The glyph's own bytes.
 * @param renumber Called with each component id; its result replaces the id when given.
 * @returns The component ids as they were read.
 */
function walkComponents(glyph: Buffer, renumber?: (id: number) => number): number[] {
	if (glyph.length < 10) return [];
	if (glyph.readInt16BE(0) !== -1) return [];

	const out: number[] = [];
	let at = 10;

	for (;;) {
		if (at + 4 > glyph.length) break;

		const flags = glyph.readUInt16BE(at);
		const id = glyph.readUInt16BE(at + 2);

		out.push(id);
		if (renumber) glyph.writeUInt16BE(renumber(id), at + 2);
		at += 4;

		at += flags & 1 ? 4 : 2;
		if (flags & 8) at += 2;
		else if (flags & 0x40) at += 4;
		else if (flags & 0x80) at += 8;

		if ((flags & 0x20) === 0) break;
	}

	return out;
}

function be16(value: number): Buffer {
	const out = Buffer.alloc(2);
	out.writeUInt16BE(value & 0xffff, 0);
	return out;
}

function be32(value: number): Buffer {
	const out = Buffer.alloc(4);
	out.writeUInt32BE(value >>> 0, 0);
	return out;
}

/**
 * A format 4 subtable over a codepoint map, one segment per contiguous run.
 *
 * A segment whose glyph ids run contiguously with its codepoints is written in the delta form and
 * the rest in the indexed form, which is what a real font does and which a reader has to handle
 * both of. Writing everything indexed would leave the delta path in the reader untested.
 */
function buildCmap4(map: Map<number, number>): Buffer {
	const codes = [...map.keys()].filter((code) => code <= 0xffff).sort((a, b) => a - b);
	const segments: { start: number; end: number; glyphs: number[] }[] = [];

	for (const code of codes) {
		const last = segments[segments.length - 1];
		if (last && code === last.end + 1) {
			last.end = code;
			last.glyphs.push(map.get(code)!);
			continue;
		}
		segments.push({ start: code, end: code, glyphs: [map.get(code)!] });
	}

	// the terminator segment every format 4 table ends with
	segments.push({ start: 0xffff, end: 0xffff, glyphs: [0] });

	const count = segments.length;
	const glyphArray: number[] = [];
	const ranges: number[] = [];
	const deltas: number[] = [];

	segments.forEach((segment, index) => {
		if (segment.start === 0xffff) {
			ranges.push(0);
			deltas.push(1);
			return;
		}

		const contiguous = segment.glyphs.every((glyph, at) => glyph === segment.glyphs[0]! + at);

		if (contiguous) {
			ranges.push(0);
			deltas.push((segment.glyphs[0]! - segment.start) & 0xffff);
			return;
		}

		// the offset is measured from the range entry's own address, so it counts the entries
		// still to come plus everything already in the glyph array
		ranges.push(2 * (count - index) + 2 * glyphArray.length);
		deltas.push(0);
		glyphArray.push(...segment.glyphs);
	});

	const parts: Buffer[] = [];
	parts.push(be16(4));
	parts.push(be16(16 + 8 * count + 2 * glyphArray.length));
	parts.push(be16(0));
	parts.push(be16(2 * count));
	const selector = Math.floor(Math.log2(count));
	parts.push(be16(2 * 2 ** selector), be16(selector), be16(2 * count - 2 * 2 ** selector));

	for (const segment of segments) parts.push(be16(segment.end));
	parts.push(be16(0));
	for (const segment of segments) parts.push(be16(segment.start));
	for (const delta of deltas) parts.push(be16(delta));
	for (const range of ranges) parts.push(be16(range));
	for (const glyph of glyphArray) parts.push(be16(glyph));

	return Buffer.concat(parts);
}

/**
 * A format 6 subtable, which is one dense run of codepoints and nothing else.
 *
 * The oldest of the three and still what a Macintosh subtable uses. It cannot express a gap, so
 * anything outside the longest run in the map is dropped, which is the point: the variant that
 * carries it has a smaller coverage than the others and a test can see the difference.
 */
function buildCmap6(map: Map<number, number>): Buffer {
	const codes = [...map.keys()].filter((code) => code <= 0xffff).sort((a, b) => a - b);

	let best = { start: codes[0] ?? 0, length: 0 };
	let from = 0;

	for (let at = 1; at <= codes.length; at++) {
		if (at < codes.length && codes[at] === codes[at - 1]! + 1) continue;

		if (at - from > best.length) best = { start: codes[from]!, length: at - from };
		from = at;
	}

	const parts: Buffer[] = [be16(6), be16(10 + 2 * best.length), be16(0)];
	parts.push(be16(best.start), be16(best.length));

	for (let i = 0; i < best.length; i++) parts.push(be16(map.get(best.start + i)!));

	return Buffer.concat(parts);
}

/** A format 12 subtable, one group per contiguous run of both code and glyph. */
function buildCmap12(map: Map<number, number>): Buffer {
	const codes = [...map.keys()].sort((a, b) => a - b);
	const groups: { start: number; end: number; glyph: number }[] = [];

	for (const code of codes) {
		const glyph = map.get(code)!;
		const last = groups[groups.length - 1];

		if (last && code === last.end + 1 && glyph === last.glyph + (code - last.start)) {
			last.end = code;
			continue;
		}

		groups.push({ start: code, end: code, glyph });
	}

	const parts: Buffer[] = [be16(12), be16(0), be32(16 + 12 * groups.length), be32(0)];
	parts.push(be32(groups.length));

	for (const group of groups) {
		parts.push(be32(group.start), be32(group.end), be32(group.glyph));
	}

	return Buffer.concat(parts);
}

function pad4(bytes: Buffer): Buffer {
	const over = bytes.length % 4;
	return over === 0 ? bytes : Buffer.concat([bytes, Buffer.alloc(4 - over)]);
}

/** The sfnt checksum: the sum of the file as big-endian 32 bit words. */
function checksum(bytes: Buffer): number {
	let sum = 0;
	for (let at = 0; at + 4 <= bytes.length; at += 4) sum = (sum + bytes.readUInt32BE(at)) >>> 0;
	return sum;
}

/** Assembles a directory and its tables into a font file. */
function assemble(tables: Map<string, Buffer>): Buffer {
	const names = [...tables.keys()].sort();
	const count = names.length;
	const selector = Math.floor(Math.log2(count));

	const header = Buffer.concat([
		be32(0x00010000),
		be16(count),
		be16(16 * 2 ** selector),
		be16(selector),
		be16(16 * count - 16 * 2 ** selector)
	]);

	let offset = header.length + 16 * count;
	const entries: Buffer[] = [];
	const bodies: Buffer[] = [];

	for (const name of names) {
		const body = tables.get(name)!;
		const padded = pad4(body);

		entries.push(
			Buffer.concat([
				be32(tag(name)),
				be32(checksum(padded)),
				be32(offset),
				be32(body.length)
			])
		);
		bodies.push(padded);
		offset += padded.length;
	}

	const font = Buffer.concat([header, ...entries, ...bodies]);

	// checkSumAdjustment: 0xB1B0AFBA less the whole file's checksum, with the field itself zero
	const head = names.indexOf('head');
	if (head >= 0) {
		const at = font.readUInt32BE(header.length + 16 * head + 8);
		font.writeUInt32BE(0, at + 8);
		font.writeUInt32BE((0xb1b0afba - checksum(font)) >>> 0, at + 8);
	}

	return font;
}

/**
 * Remaps a format 0 `kern` table onto new glyph ids.
 *
 * Pairs that lost either glyph are dropped, and the survivors are re-sorted because the table is
 * binary searched on the packed pair and renumbering reorders it. Anything but format 0 is dropped
 * whole rather than passed through with stale ids.
 */
function subsetKern(table: Buffer, remap: Map<number, number>): Buffer | undefined {
	if (table.length < 18) return undefined;
	if (table.readUInt16BE(0) !== 0) return undefined;
	if (table.readUInt16BE(2) < 1) return undefined;

	const coverage = table.readUInt16BE(8);
	if (coverage >> 8 !== 0) return undefined;

	const pairs = table.readUInt16BE(10);
	const kept: { key: number; value: number }[] = [];

	for (let i = 0; i < pairs; i++) {
		const at = 18 + 6 * i;
		if (at + 6 > table.length) break;

		const left = remap.get(table.readUInt16BE(at));
		const right = remap.get(table.readUInt16BE(at + 2));

		if (left === undefined || right === undefined) continue;
		kept.push({ key: (left << 16) | right, value: table.readInt16BE(at + 4) });
	}

	if (kept.length === 0) return undefined;
	kept.sort((a, b) => a.key - b.key);

	const body = Buffer.alloc(6 * kept.length);
	kept.forEach((pair, i) => {
		body.writeUInt16BE(pair.key >>> 16, 6 * i);
		body.writeUInt16BE(pair.key & 0xffff, 6 * i + 2);
		body.writeInt16BE(pair.value, 6 * i + 4);
	});

	const selector = Math.floor(Math.log2(kept.length));
	const header = Buffer.concat([
		be16(0),
		be16(1),
		be16(0),
		be16(14 + body.length),
		be16(1),
		be16(kept.length),
		be16(6 * 2 ** selector),
		be16(selector),
		be16(6 * kept.length - 6 * 2 ** selector)
	]);

	return Buffer.concat([header, body]);
}

/**
 * Cuts a TrueType face down to the glyphs a set of codepoints needs.
 *
 * The glyph set is closed under composite components first, or an accented glyph loses its accent,
 * and then renumbered. Renumbering is what makes the result small: DejaVu keeps its combining
 * accents at the very top of the glyph order, so `Å` reaches glyph 5925 out of 6253 and truncating
 * at the highest id kept would carry 596 KB to cover 110 glyphs. Renumbering means rewriting each
 * composite glyph's component ids and re-sorting `kern`, which is most of the length below.
 *
 * @param font The face to cut down.
 * @param wanted Codepoints to keep.
 * @param format Which cmap subtable to write.
 * @param options `aliases` maps extra codepoints onto the glyph another codepoint already uses,
 * which is the way to cover a codepoint past the BMP without a font that has one: the mapping is
 * ours to write, and what the format 12 reader does with a five digit codepoint is the same either
 * way. `shortLoca` writes the halved offset form, which a reader has to handle as well as the long
 * one. `dropHhea` leaves out the horizontal header, which is out of specification and which a
 * reader should recover from rather than refuse.
 */
export function subsetFont(
	font: Buffer,
	wanted: number[],
	format: 4 | 6 | 12,
	options: {
		aliases?: Record<number, number>;
		shortLoca?: boolean;
		dropHhea?: boolean;
	} = {}
): { bytes: Buffer; summary: FontSummary } {
	const tables = directory(font);
	const head = tables.get('head');
	const maxp = tables.get('maxp');
	const hhea = tables.get('hhea');
	const hmtx = tables.get('hmtx');
	const glyf = tables.get('glyf');
	const loca = tables.get('loca');
	const cmap = tables.get('cmap');

	if (!head || !maxp || !hhea || !hmtx || !glyf || !loca || !cmap) {
		throw new Error('font is missing a table the subsetter needs');
	}

	const long = font.readInt16BE(head.offset + 50) !== 0;
	const glyphs = font.readUInt16BE(maxp.offset + 4);
	const aliases = options.aliases ?? {};

	// the best format 4 subtable, which is where the whole mapping is read from
	let best = 0;
	const subtables = font.readUInt16BE(cmap.offset + 2);
	for (let i = 0; i < subtables; i++) {
		const at = cmap.offset + 4 + 8 * i;
		const platform = font.readUInt16BE(at);
		const offset = cmap.offset + font.readUInt32BE(at + 4);
		if (font.readUInt16BE(offset) !== 4) continue;
		if (best === 0 || platform === 3) best = offset;
	}

	if (best === 0) throw new Error('font has no format 4 cmap');

	const full = readCmap4(font, best);
	const map = new Map<number, number>();

	for (const code of wanted) {
		const glyph = full.get(code);
		if (glyph !== undefined) map.set(code, glyph);
	}

	if (map.size === 0) throw new Error('none of the wanted codepoints are in the font');

	for (const [code, onto] of Object.entries(aliases)) {
		const glyph = map.get(onto);
		if (glyph === undefined) throw new Error(`alias target U+${onto.toString(16)} is not kept`);
		map.set(Number(code), glyph);
	}

	const glyphBytes = (id: number): Buffer => {
		const from = locaAt(font, loca, long, id);
		const to = locaAt(font, loca, long, id + 1);
		if (to <= from) return Buffer.alloc(0);
		return Buffer.from(font.subarray(glyf.offset + from, glyf.offset + to));
	};

	const reached = new Set<number>([0, ...map.values()]);
	const queue = [...reached];

	while (queue.length > 0) {
		const glyph = queue.pop()!;
		if (glyph >= glyphs) continue;

		for (const part of walkComponents(glyphBytes(glyph))) {
			if (reached.has(part)) continue;
			reached.add(part);
			queue.push(part);
		}
	}

	const order = [...reached].sort((a, b) => a - b);
	const remap = new Map<number, number>();
	order.forEach((id, index) => remap.set(id, index));

	const keep = order.length;
	const bodies: Buffer[] = [];
	const offsets: number[] = [];
	let at = 0;

	for (const id of order) {
		offsets.push(at);

		const body = glyphBytes(id);
		walkComponents(body, (part) => remap.get(part) ?? 0);

		// loca entries are halved in the short format, so a glyph has to start on an even byte
		const padded = body.length % 2 === 0 ? body : Buffer.concat([body, Buffer.alloc(1)]);

		bodies.push(padded);
		at += padded.length;
	}

	offsets.push(at);

	// the short form halves every offset, so it only works while the glyf table fits in 128 KiB
	const short = options.shortLoca === true;
	if (short && at > 0x1fffe) {
		throw new Error(`glyf is ${at} bytes, which the short loca format cannot address`);
	}

	const newGlyf = Buffer.concat(bodies);
	const newLoca = Buffer.alloc(short ? 2 * (keep + 1) : 4 * (keep + 1));

	offsets.forEach((value, i) => {
		if (short) newLoca.writeUInt16BE(value / 2, 2 * i);
		else newLoca.writeUInt32BE(value, 4 * i);
	});

	// a full hmtx, one entry per kept glyph: the compressed tail form only works when the trailing
	// glyphs share an advance, which renumbering does not preserve
	const oldMetrics = font.readUInt16BE(hhea.offset + 34);
	const newHmtx = Buffer.alloc(4 * keep);

	order.forEach((id, index) => {
		const entry = Math.min(id, oldMetrics - 1);
		newHmtx.writeUInt16BE(font.readUInt16BE(hmtx.offset + 4 * entry), 4 * index);
		newHmtx.writeInt16BE(
			id < oldMetrics
				? font.readInt16BE(hmtx.offset + 4 * id + 2)
				: font.readInt16BE(hmtx.offset + 4 * oldMetrics + 2 * (id - oldMetrics)),
			4 * index + 2
		);
	});

	const newHead = Buffer.from(font.subarray(head.offset, head.offset + head.length));
	newHead.writeInt16BE(short ? 0 : 1, 50);

	const newHhea = Buffer.from(font.subarray(hhea.offset, hhea.offset + hhea.length));
	newHhea.writeUInt16BE(keep, 34);

	const newMaxp = Buffer.from(font.subarray(maxp.offset, maxp.offset + maxp.length));
	newMaxp.writeUInt16BE(keep, 4);

	const mapped = new Map<number, number>();
	for (const [code, glyph] of map) mapped.set(code, remap.get(glyph) ?? 0);

	const subtable =
		format === 4 ? buildCmap4(mapped) : format === 6 ? buildCmap6(mapped) : buildCmap12(mapped);

	// format 6 goes under the Macintosh platform, which is where a reader finds one in the wild
	const newCmap = Buffer.concat([
		be16(0),
		be16(1),
		be16(format === 6 ? 1 : 3),
		be16(format === 4 ? 1 : format === 6 ? 0 : 10),
		be32(12),
		subtable
	]);

	const out = new Map<string, Buffer>([
		['head', newHead],
		['maxp', newMaxp],
		['hmtx', newHmtx],
		['loca', newLoca],
		['glyf', newGlyf],
		['cmap', newCmap]
	]);

	if (options.dropHhea !== true) out.set('hhea', newHhea);

	const kern = tables.get('kern');
	if (kern) {
		const remapped = subsetKern(
			Buffer.from(font.subarray(kern.offset, kern.offset + kern.length)),
			remap
		);
		if (remapped) out.set('kern', remapped);
	}

	const bytes = assemble(out);
	const reachable = [...mapped.keys()].filter((code) => {
		if (format === 12) return true;
		if (format === 4) return code <= 0xffff;
		return true;
	});

	return {
		bytes,
		summary: {
			glyphs: keep,
			codepoints: format === 6 ? countCmap6(mapped) : reachable.length,
			bytes: bytes.length
		}
	};
}

/** An `OTTO` file, which is an OpenType face whose outlines this library does not read. */
export function cffStub(): Buffer {
	const table = Buffer.alloc(64);
	return assembleWithSignature(tag('OTTO'), new Map([['CFF ', table]]));
}

function assembleWithSignature(signature: number, tables: Map<string, Buffer>): Buffer {
	const font = assemble(tables);
	font.writeUInt32BE(signature, 0);
	return font;
}

/**
 * A PSF2 face with a known bitmap per glyph.
 *
 * Synthesized rather than downloaded so the expected pixels are exact: glyph `n` has its left
 * column set and row `n % height` filled, which makes both axes and the codepoint indexing
 * checkable from one bitmap.
 */
export function buildPsf2(width: number, height: number, glyphs: number): Buffer {
	const stride = Math.ceil(width / 8);
	const header = Buffer.alloc(32);

	header.writeUInt32LE(0x864ab572, 0);
	header.writeUInt32LE(0, 4);
	header.writeUInt32LE(32, 8);
	header.writeUInt32LE(0, 12);
	header.writeUInt32LE(glyphs, 16);
	header.writeUInt32LE(stride * height, 20);
	header.writeUInt32LE(height, 24);
	header.writeUInt32LE(width, 28);

	const body = Buffer.alloc(stride * height * glyphs);

	for (let glyph = 0; glyph < glyphs; glyph++) {
		const base = glyph * stride * height;

		for (let row = 0; row < height; row++) {
			// the left column always, so every glyph has ink and the origin is checkable
			body[base + row * stride] = body[base + row * stride]! | 0x80;
		}

		const filled = glyph % height;
		for (let column = 0; column < width; column++) {
			const at = base + filled * stride + (column >> 3);
			body[at] = body[at]! | (0x80 >> (column & 7));
		}
	}

	return Buffer.concat([header, body]);
}

/** How many codepoints a format 6 subtable over this map can actually reach. */
function countCmap6(map: Map<number, number>): number {
	const codes = [...map.keys()].filter((code) => code <= 0xffff).sort((a, b) => a - b);

	let best = 0;
	let run = 0;

	for (let at = 0; at < codes.length; at++) {
		run = at > 0 && codes[at] === codes[at - 1]! + 1 ? run + 1 : 1;
		if (run > best) best = run;
	}

	return best;
}

/** A PSF1 face, which has a four byte header and a fixed width of eight. */
export function buildPsf1(height: number): Buffer {
	const header = Buffer.from([0x36, 0x04, 0x00, height]);
	const body = Buffer.alloc(height * 256);

	for (let glyph = 0; glyph < 256; glyph++) {
		body[glyph * height] = 0x80;
		body[glyph * height + (glyph % height)] = 0xff;
	}

	return Buffer.concat([header, body]);
}

/**
 * A BDF face with a known bitmap per glyph.
 *
 * The same shape as the PSF one so a test can assert the same pixels through both readers, which is
 * what makes the hex nibble parsing checkable against the bit parsing.
 */
export function buildBdf(width: number, height: number, codepoints: number[]): string {
	const stride = Math.ceil(width / 8);
	const lines: string[] = [
		'STARTFONT 2.1',
		'FONT -tinyimg-fixture-medium-r-normal--8-80-75-75-c-80-iso10646-1',
		'SIZE 8 75 75',
		`FONTBOUNDINGBOX ${width} ${height} 0 0`,
		'STARTPROPERTIES 2',
		`FONT_ASCENT ${height}`,
		'FONT_DESCENT 0',
		'ENDPROPERTIES',
		`CHARS ${codepoints.length}`
	];

	codepoints.forEach((code, index) => {
		lines.push(`STARTCHAR U+${code.toString(16).toUpperCase().padStart(4, '0')}`);
		lines.push(`ENCODING ${code}`);
		lines.push('SWIDTH 500 0');
		lines.push(`DWIDTH ${width} 0`);
		lines.push(`BBX ${width} ${height} 0 0`);
		lines.push('BITMAP');

		const filled = index % height;

		for (let row = 0; row < height; row++) {
			const bytes = Buffer.alloc(stride);
			bytes[0] = bytes[0]! | 0x80;

			if (row === filled) {
				for (let column = 0; column < width; column++) {
					const at = column >> 3;
					bytes[at] = bytes[at]! | (0x80 >> (column & 7));
				}
			}

			lines.push(bytes.toString('hex').toUpperCase());
		}

		lines.push('ENDCHAR');
	});

	lines.push('ENDFONT');
	return `${lines.join('\n')}\n`;
}
