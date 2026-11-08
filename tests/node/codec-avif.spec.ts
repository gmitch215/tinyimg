import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import {
	TinyImgDataError,
	TinyImgFormatError,
	TinyImgModule,
	transform
} from '../../src/ts/index.js';
import { Err, Format, TinyAbi } from '../support/abi.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

/**
 * Walks the boxes of one container level, from the format's own structure.
 *
 * Written from the specification rather than from the C, so what `probe` reports is checked against
 * an independent reading of the same bytes. Every level of the format is a length and a four
 * character type, so one walker serves the whole file.
 */
function boxes(
	data: Uint8Array,
	from = 0,
	to = data.byteLength
): { type: string; at: number; size: number }[] {
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
	const text = new TextDecoder('latin1');
	const found: { type: string; at: number; size: number }[] = [];
	let at = from;

	while (at + 8 <= to) {
		let length = view.getUint32(at, false);
		let header = 8;

		if (length === 1) {
			length = view.getUint32(at + 12, false);
			header = 16;
		} else if (length === 0) {
			length = to - at;
		}

		if (length < header || at + length > to) break;

		found.push({
			type: text.decode(data.subarray(at + 4, at + 8)),
			at: at + header,
			size: length - header
		});

		at += length;
	}

	return found;
}

describe('the avif container reader inside the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('describes the primary item without decoding it', () => {
		const source = fixture('derived/base.avif');
		const { result, info } = abi.probe(source);

		expect(result).toBe(Err.ok);
		expect(info).toEqual({
			width: 320,
			height: 180,

			// a still image is one frame; an image sequence keeps its count in a movie track this
			// build does not read
			frames: 1,
			format: Format.avif,
			channels: 3,
			bitDepth: 8,
			hasAlpha: false,
			progressive: false
		});

		// the container really is nested the way the reader assumes
		const top = boxes(source);

		expect(top.map((box) => box.type)).toContain('ftyp');
		expect(top.map((box) => box.type)).toContain('meta');
	});

	it('follows the property associations rather than taking the first of each kind', () => {
		/*
		 * A file with alpha carries a second item, and so two of every property. Taking the first
		 * spatial extents would describe whichever item happens to come first, which is not
		 * necessarily the one the file says to show.
		 */
		const source = fixture('derived/base-alpha.avif');
		const meta = boxes(source).find((box) => box.type === 'meta')!;

		// the metadata box is a full box, so its version and flags come before its children
		const inside = boxes(source, meta.at + 4, meta.at + meta.size);
		const properties = inside.find((box) => box.type === 'iprp')!;
		const container = boxes(source, properties.at, properties.at + properties.size).find(
			(box) => box.type === 'ipco'
		)!;

		const children = boxes(source, container.at, container.at + container.size);
		const kinds = children.map((box) => box.type);

		expect(kinds).toContain('auxC');

		/*
		 * Two channel-count properties, and they disagree: the first describes the picture and the
		 * second the alpha plane, which is one channel. So a reader taking the last property of
		 * each kind reports one channel here, and only following the associations from the primary
		 * item gives the right answer. The extents are shared between the two items in this file,
		 * which is why they cannot be what discriminates.
		 */
		const counts = children
			.filter((box) => box.type === 'pixi')
			.map((box) => source[box.at + 4]!);

		expect(counts).toEqual([3, 1]);

		expect(abi.probe(source).info).toMatchObject({
			width: 320,
			height: 180,
			channels: 4,
			hasAlpha: true
		});
	});

	it('reports the coded extents of a rotated file, as avifdec does', () => {
		/*
		 * A rotation is a display transform, and applying one needs a decode. This library treats a
		 * JPEG's orientation the same way: parsed and reported, applied by a geometry operation
		 * rather than by a decoder.
		 */
		const { info } = abi.probe(fixture('derived/base-rotated.avif'));

		expect([info.width, info.height]).toEqual([320, 180]);
	});

	it('decodes the pixels through the same module the container came from', () => {
		const source = fixture('derived/base.avif');

		expect(abi.probe(source).result).toBe(Err.ok);

		const { result, image } = abi.decode(source);

		expect(result).toBe(Err.ok);
		expect([image?.width, image?.height]).toEqual([320, 180]);
		expect(image?.format).toBe(Format.avif);
	});

	it('leaves heif recognized and unclaimed', () => {
		// the two families share the container and are separated by the brand. Nothing registers
		// HEIF, so a probe of one has to say so specifically rather than call it unreadable
		const heic = new Uint8Array([
			0, 0, 0, 0x18, 0x66, 0x74, 0x79, 0x70, 0x68, 0x65, 0x69, 0x63
		]);

		const { result, info } = abi.probe(heic);

		expect(result).toBe(Err.unsupportedCodec);
		expect(info.format).toBe(Format.heif);
	});

	it('reports a container it cannot make sense of', () => {
		// the brand alone, with no metadata box to describe anything
		const bare = new Uint8Array([
			0, 0, 0, 0x0c, 0x66, 0x74, 0x79, 0x70, 0x61, 0x76, 0x69, 0x66
		]);

		expect(abi.probe(bare).result).toBe(Err.corrupt);
		expect(abi.probe(fixture('derived/base.avif').subarray(0, 16)).result).toBe(Err.corrupt);
	});
});

/**
 * What an AVIF this decoder would show wrongly does through the shipped wrapper, and how a rotated
 * one reaches the planner. The C lane holds the reference comparison; these check that the codes
 * and the orientation survive the module boundary.
 */
describe('the avif refusals and orientation through the wrapper', () => {
	let tinyimg: TinyImgModule;

	beforeAll(async () => {
		tinyimg = await TinyImgModule.loadBytes(bytes);
	});

	it.each([
		'avif-grain',
		'avif-superres',
		'avif-grid',
		'avif-pq',
		'avif-hlg',
		'avif-alpha-grain',
		'avif-clap',
		'avif-clap-odd'
	])('refuses %s with the variant error and still probes it', async (name) => {
		const source = fixture(`derived/${name}.avif`);

		expect((await tinyimg.probe(source)).format).toBe('avif');

		const error = await transform(tinyimg, source, { format: 'png' }).catch((e: unknown) => e);

		expect(error).toBeInstanceOf(TinyImgFormatError);
		expect((error as TinyImgFormatError).code).toBe(Err.unsupportedVariant);
	});

	it('decodes a clean aperture the size of the frame', async () => {
		const image = await tinyimg.decode(fixture('derived/avif-clap-full.avif'));

		expect([image.width, image.height]).toEqual([64, 48]);
	});

	it('reports no alpha when no auxl reference ties the plane to the picture', async () => {
		const source = fixture('derived/base-alpha.avif').slice();
		const at = Buffer.from(source).indexOf('auxl');

		expect((await tinyimg.probe(source)).hasAlpha).toBe(true);
		expect(at).toBeGreaterThan(0);

		source.set(Buffer.from('xxxx'), at);

		expect((await tinyimg.probe(source)).hasAlpha).toBe(false);
	});

	it('fails with the corrupt error when the alpha item is not in the file', async () => {
		const source = fixture('derived/base-alpha.avif').slice();
		const at = Buffer.from(source).indexOf('auxl');

		source[at + 4] = 0;
		source[at + 5] = 99;

		const error = await transform(tinyimg, source, { format: 'png' }).catch((e: unknown) => e);

		expect(error).toBeInstanceOf(TinyImgDataError);
		expect((error as TinyImgDataError).code).toBe(Err.corrupt);
	});

	it('reports the orientation a rotated or mirrored file equals', async () => {
		const expected = [
			[1, 4, 2],
			[8, 5, 7],
			[3, 2, 4],
			[6, 7, 5]
		];
		const axes = ['n', '0', '1'];

		for (let irot = 0; irot < 4; irot++) {
			for (let imir = 0; imir < 3; imir++) {
				const info = await tinyimg.probe(
					fixture(`derived/avif-orient-${irot}-${axes[imir]}.avif`)
				);

				expect(info.orientation, `irot ${irot} imir ${axes[imir]}`).toBe(
					expected[irot]![imir]
				);
			}
		}

		expect((await tinyimg.probe(fixture('derived/base.avif'))).orientation).toBe(1);
	});

	it('turns a quarter-turned file upright, so the output swaps width and height', async () => {
		const upright = await tinyimg.decode(fixture('derived/avif-orient-0-n.avif'));

		for (const name of ['avif-orient-1-n', 'avif-orient-3-n', 'avif-orient-1-0']) {
			const out = await transform(tinyimg, fixture(`derived/${name}.avif`), {
				format: 'png'
			});
			const shown = await tinyimg.decode(out.bytes());

			expect([shown.width, shown.height], name).toEqual([upright.height, upright.width]);
		}

		const half = await transform(tinyimg, fixture('derived/avif-orient-2-n.avif'), {
			format: 'png'
		});
		const shown = await tinyimg.decode(half.bytes());

		expect([shown.width, shown.height]).toEqual([upright.width, upright.height]);
	});
});
