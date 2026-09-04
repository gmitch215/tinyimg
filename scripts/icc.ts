/**
 * Builds matrix and TRC ICC profiles from published primaries and transfer functions.
 *
 * These are generated rather than copied so the bytes are ours to redistribute, and because the
 * generator is the inverse of the parser `color.c` gets in Phase 5 - a profile written here and read
 * back there is a round trip either half can fail.
 *
 * Only the matrix and TRC class is produced, which is what web images carry. A LUT-based profile
 * (`A2B0`, mostly CMYK printer profiles) is a different structure and tinyimg rejects those.
 */

/**
 * A tone response curve, sampled into a 1024-entry table.
 *
 * Device value in, linear light out. That direction is what an ICC `rTRC` means: in a matrix and
 * TRC profile the transform runs device -> curve -> linear -> matrix -> PCS, so the curve is the
 * EOTF and not the OETF a color space is usually quoted by.
 *
 * Writing the OETF instead produces a profile that parses, converts, and round-trips against
 * itself, so neither an identity check nor a plausible-looking conversion percentage can see it.
 * The number that can is the curve read back at a known input and compared against a profile
 * somebody else wrote: a real sRGB profile's `rTRC` at 128/255 is 0.2159, and the OETF gives
 * 0.7353.
 */
export type Transfer = (value: number) => number;

export interface ColorSpace {
	/** Four-character profile description, used for the file name and the `desc` tag. */
	id: string;
	name: string;
	/** Red, green and blue primaries as CIE xy chromaticities. */
	primaries: [[number, number], [number, number], [number, number]];
	/** White point as a CIE xy chromaticity. */
	white: [number, number];
	/** Encoding function, linear light in, signal out. */
	transfer: Transfer;
}

/** IEC 61966-2-1 piecewise curve, used by sRGB and Display P3. */
const srgbTransfer: Transfer = (v) =>
	v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);

/** Adobe RGB (1998) uses a pure 563/256 gamma. */
const adobeTransfer: Transfer = (v) => Math.pow(v, 563 / 256);

/** Rec.2020's EOTF, the inverse of its 10-bit OETF. */
const rec2020Transfer: Transfer = (v) =>
	v < 4.5 * 0.018053968510807
		? v / 4.5
		: Math.pow((v + 0.09929682680944) / 1.09929682680944, 1 / 0.45);

const D65: [number, number] = [0.3127, 0.329];

export const SPACES: ColorSpace[] = [
	{
		id: 'srgb',
		name: 'tinyimg sRGB',
		primaries: [
			[0.64, 0.33],
			[0.3, 0.6],
			[0.15, 0.06]
		],
		white: D65,
		transfer: srgbTransfer
	},
	{
		id: 'display-p3',
		name: 'tinyimg Display P3',
		primaries: [
			[0.68, 0.32],
			[0.265, 0.69],
			[0.15, 0.06]
		],
		white: D65,
		transfer: srgbTransfer
	},
	{
		id: 'adobe-rgb-1998',
		name: 'tinyimg Adobe RGB (1998) compatible',
		primaries: [
			[0.64, 0.33],
			[0.21, 0.71],
			[0.15, 0.06]
		],
		white: D65,
		transfer: adobeTransfer
	},
	{
		id: 'rec2020',
		name: 'tinyimg Rec.2020',
		primaries: [
			[0.708, 0.292],
			[0.17, 0.797],
			[0.131, 0.046]
		],
		white: D65,
		transfer: rec2020Transfer
	}
];

// #region color math

type Matrix = [number, number, number, number, number, number, number, number, number];
type Vector = [number, number, number];

function multiply(a: Matrix, b: Matrix): Matrix {
	const out = new Array(9).fill(0) as number[];
	for (let row = 0; row < 3; row++) {
		for (let col = 0; col < 3; col++) {
			let sum = 0;
			for (let k = 0; k < 3; k++) sum += a[row * 3 + k]! * b[k * 3 + col]!;
			out[row * 3 + col] = sum;
		}
	}
	return out as Matrix;
}

function apply(m: Matrix, v: Vector): Vector {
	return [
		m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
		m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
		m[6] * v[0] + m[7] * v[1] + m[8] * v[2]
	];
}

function invert(m: Matrix): Matrix {
	const [a, b, c, d, e, f, g, h, i] = m;
	const det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (Math.abs(det) < 1e-12) throw new Error('singular matrix');

	return [
		(e * i - f * h) / det,
		(c * h - b * i) / det,
		(b * f - c * e) / det,
		(f * g - d * i) / det,
		(a * i - c * g) / det,
		(c * d - a * f) / det,
		(d * h - e * g) / det,
		(b * g - a * h) / det,
		(a * e - b * d) / det
	];
}

/** xy chromaticity to an XYZ vector normalized to Y = 1. */
function whiteXYZ(xy: [number, number]): Vector {
	const [x, y] = xy;
	return [x / y, 1, (1 - x - y) / y];
}

/** The RGB to XYZ matrix for a set of primaries under their own white point. */
function rgbToXYZ(space: ColorSpace): Matrix {
	const [r, g, b] = space.primaries;
	const columns: Matrix = [
		r[0] / r[1],
		g[0] / g[1],
		b[0] / b[1],
		1,
		1,
		1,
		(1 - r[0] - r[1]) / r[1],
		(1 - g[0] - g[1]) / g[1],
		(1 - b[0] - b[1]) / b[1]
	];

	const scale = apply(invert(columns), whiteXYZ(space.white));
	return [
		columns[0] * scale[0],
		columns[1] * scale[1],
		columns[2] * scale[2],
		columns[3] * scale[0],
		columns[4] * scale[1],
		columns[5] * scale[2],
		columns[6] * scale[0],
		columns[7] * scale[1],
		columns[8] * scale[2]
	];
}

const BRADFORD: Matrix = [
	0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296
];

/** D50, the illuminant every ICC profile connection space is defined against. */
const D50: Vector = [0.9642, 1.0, 0.8249];

/**
 * The Bradford adaptation matrix taking `from` to D50.
 *
 * ICC stores primaries already adapted to D50, so a profile for a D65 space cannot just write its
 * own matrix; skipping this shifts every converted image warm.
 */
function chromaticAdaptation(from: Vector): Matrix {
	const source = apply(BRADFORD, from);
	const destination = apply(BRADFORD, D50);

	const scale: Matrix = [
		destination[0] / source[0],
		0,
		0,
		0,
		destination[1] / source[1],
		0,
		0,
		0,
		destination[2] / source[2]
	];

	return multiply(invert(BRADFORD), multiply(scale, BRADFORD));
}

// #endregion

// #region icc serialization

function s15Fixed16(value: number): number {
	return Math.round(value * 65536);
}

function tag(signature: string, body: Buffer): { signature: string; body: Buffer } {
	return { signature, body };
}

/** XYZType, one adapted primary column. */
function xyzTag(v: Vector): Buffer {
	const body = Buffer.alloc(20);
	body.write('XYZ ', 0, 'ascii');
	body.writeInt32BE(s15Fixed16(v[0]), 8);
	body.writeInt32BE(s15Fixed16(v[1]), 12);
	body.writeInt32BE(s15Fixed16(v[2]), 16);
	return body;
}

/**
 * curveType, sampled to 1024 points.
 *
 * A table rather than a single gamma because sRGB and Rec.2020 are piecewise; a lone gamma value
 * would be wrong in the near-black region where the linear segment lives.
 */
function curveTag(transfer: Transfer, points = 1024): Buffer {
	const body = Buffer.alloc(12 + points * 2);
	body.write('curv', 0, 'ascii');
	body.writeUInt32BE(points, 8);

	for (let i = 0; i < points; i++) {
		const linear = transfer(i / (points - 1));
		const clamped = Math.min(1, Math.max(0, linear));
		body.writeUInt16BE(Math.round(clamped * 65535), 12 + i * 2);
	}
	return body;
}

/** textDescriptionType, the ICC v2 description tag. */
function descriptionTag(text: string): Buffer {
	const ascii = `${text}\0`;
	const body = Buffer.alloc(12 + ascii.length + 12 + 67);
	body.write('desc', 0, 'ascii');
	body.writeUInt32BE(ascii.length, 8);
	body.write(ascii, 12, 'ascii');
	return body;
}

/** textType, used for the copyright tag. */
function textTag(text: string): Buffer {
	const ascii = `${text}\0`;
	const body = Buffer.alloc(8 + ascii.length);
	body.write('text', 0, 'ascii');
	body.write(ascii, 8, 'ascii');
	return body;
}

/** s15Fixed16ArrayType, the chromatic adaptation matrix. */
function chadTag(m: Matrix): Buffer {
	const body = Buffer.alloc(8 + 36);
	body.write('sf32', 0, 'ascii');
	for (let i = 0; i < 9; i++) body.writeInt32BE(s15Fixed16(m[i]!), 8 + i * 4);
	return body;
}

/**
 * Assembles a complete matrix and TRC ICC profile.
 *
 * @param space The color space to describe.
 * @return Buffer The profile bytes, ready to write or embed.
 */
export function buildProfile(space: ColorSpace): Buffer {
	const adaptation = chromaticAdaptation(whiteXYZ(space.white));
	const adapted = multiply(adaptation, rgbToXYZ(space));

	const red: Vector = [adapted[0], adapted[3], adapted[6]];
	const green: Vector = [adapted[1], adapted[4], adapted[7]];
	const blue: Vector = [adapted[2], adapted[5], adapted[8]];

	const curve = curveTag(space.transfer);
	const tags = [
		tag('desc', descriptionTag(space.name)),
		tag('wtpt', xyzTag(D50)),
		tag('rXYZ', xyzTag(red)),
		tag('gXYZ', xyzTag(green)),
		tag('bXYZ', xyzTag(blue)),
		tag('rTRC', curve),
		tag('gTRC', curve),
		tag('bTRC', curve),
		tag('chad', chadTag(adaptation)),
		tag('cprt', textTag('Generated by tinyimg. Public domain.'))
	];

	// the three TRC tags are byte-identical, so they share one block; readers follow the offsets
	const blocks = new Map<Buffer, number>();
	const table = Buffer.alloc(4 + tags.length * 12);
	table.writeUInt32BE(tags.length, 0);

	const bodies: Buffer[] = [];
	let offset = 128 + table.length;

	tags.forEach((entry, index) => {
		let at = blocks.get(entry.body);
		if (at === undefined) {
			at = offset;
			blocks.set(entry.body, at);
			bodies.push(entry.body);
			// every tag element starts on a 4-byte boundary
			const padded = (entry.body.length + 3) & ~3;
			if (padded > entry.body.length) bodies.push(Buffer.alloc(padded - entry.body.length));
			offset += padded;
		}

		const base = 4 + index * 12;
		table.write(entry.signature, base, 'ascii');
		table.writeUInt32BE(at, base + 4);
		table.writeUInt32BE(entry.body.length, base + 8);
	});

	const header = Buffer.alloc(128);
	header.writeUInt32BE(offset, 0);
	header.write('tiny', 4, 'ascii');
	header.writeUInt32BE(0x02400000, 8);
	header.write('mntr', 12, 'ascii');
	header.write('RGB ', 16, 'ascii');
	header.write('XYZ ', 20, 'ascii');
	header.writeUInt16BE(2026, 24);
	header.writeUInt16BE(1, 26);
	header.writeUInt16BE(1, 28);
	header.write('acsp', 36, 'ascii');
	header.write('APPL', 40, 'ascii');
	header.writeUInt32BE(0, 64);
	header.writeInt32BE(s15Fixed16(D50[0]), 68);
	header.writeInt32BE(s15Fixed16(D50[1]), 72);
	header.writeInt32BE(s15Fixed16(D50[2]), 76);
	header.write('tiny', 80, 'ascii');

	return Buffer.concat([header, table, ...bodies]);
}

// #endregion
