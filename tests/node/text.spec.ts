import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { beforeAll, describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Align, Blob, Err, Gravity, TinyAbi } from '../support/abi.js';

const fixtures = join(import.meta.dirname, '../fixtures');

function fixture(name: string): Uint8Array {
	return new Uint8Array(readFileSync(join(fixtures, name)));
}

/**
 * The text and detection surfaces through the module's own exports.
 *
 * What this adds over the ctest lane is the boundary: a face and a cascade both arrive as bytes a
 * host copied into linear memory and handed to the blob table, a style and a metrics structure are
 * both allocated at a size the module reported, and a box array is read back at the stride the
 * module reported. Every one of those is a place a host can get the layout wrong, and the C tests
 * cannot reach any of them because they share the compiler's own idea of the structures.
 */
describe('text through the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));

		expect(abi.loadBlob(Blob.font, 'latin', fixture('derived/fonts/dejavu-latin.ttf'))).toBe(
			Err.ok
		);
		expect(abi.loadBlob(Blob.font, 'psf', fixture('derived/fonts/tiny.psf'))).toBe(Err.ok);
	});

	it('reports the structure sizes a host allocates with', () => {
		/*
		 * TinyTextStyle is seven floats, a byte and two four-byte colors, which is 40 with the
		 * padding after the flag; TinyTextMetrics five floats and three words.
		 *
		 * The numbers are spelled out rather than derived because a host writes the fields by
		 * offset, so the layout is the contract and a field inserted in the middle of it has to
		 * fail here rather than in whoever wrote the old offsets.
		 */
		expect(abi.exports.tiny_text_style_sizeof()).toBe(40);
		expect(abi.exports.tiny_text_metrics_sizeof()).toBe(32);
		expect(abi.exports.tiny_text_box_sizeof()).toBe(20);
		expect(abi.exports.tiny_text_line_sizeof()).toBe(36);
		expect(abi.exports.tiny_face_box_sizeof()).toBe(20);
		expect(abi.exports.tiny_detect_opts_sizeof()).toBe(16);
		expect(abi.exports.tiny_font_sizeof()).toBeGreaterThan(0);
	});

	it('draws a line and reports what it measured', () => {
		const drawn = abi.drawText({
			font: 'latin',
			text: 'Hamburgefons',
			size: 32,
			width: 300,
			height: 60
		});

		expect(drawn.result).toBe(Err.ok);
		expect(drawn.image?.width).toBe(300);
		expect(drawn.metrics?.lines).toBe(1);
		expect(drawn.metrics?.glyphs).toBe(12);
		expect(drawn.metrics?.missing).toBe(0);

		// the measurement crossed the boundary as floats and has to come back usable
		expect(drawn.metrics!.width).toBeGreaterThan(100);
		expect(drawn.metrics!.width).toBeLessThan(300);
		expect(drawn.metrics!.ascent).toBeCloseTo((1901 * 32) / 2048, 3);

		const ink = drawn.image!.pixels.reduce((sum, value) => sum + value, 0);
		expect(ink).toBeGreaterThan(0);
	});

	it('refuses a face that is not resident', () => {
		const drawn = abi.drawText({
			font: 'nothing',
			text: 'x',
			size: 20,
			width: 40,
			height: 40
		});

		expect(drawn.result).toBe(Err.blobMissing);
		expect(drawn.image).toBeUndefined();
	});

	it('wraps and aligns inside a box', () => {
		const sentence = 'the quick brown fox jumps over the lazy dog';

		const wrapped = abi.drawText({
			font: 'latin',
			text: sentence,
			size: 16,
			width: 200,
			height: 200,
			box: { width: 160, height: 0, align: Align.left }
		});

		expect(wrapped.result).toBe(Err.ok);
		expect(wrapped.metrics!.lines).toBeGreaterThan(1);
		expect(wrapped.metrics!.width).toBeLessThanOrEqual(160);

		// the three alignments put the same glyphs in different columns
		const columns = [Align.left, Align.center, Align.right].map((align) => {
			const drawn = abi.drawText({
				font: 'latin',
				text: 'short',
				size: 20,
				width: 220,
				height: 40,
				box: { width: 200, height: 0, align }
			});

			expect(drawn.result).toBe(Err.ok);

			const pixels = drawn.image!.pixels;
			for (let x = 0; x < 220; x++) {
				for (let y = 0; y < 40; y++) {
					if (pixels[y * 220 + x] !== 0) return x;
				}
			}

			return 220;
		});

		expect(columns[0]!).toBeLessThan(columns[1]!);
		expect(columns[1]!).toBeLessThan(columns[2]!);
	});

	it('draws a bitmap face at its own size, whatever size is asked for', () => {
		const small = abi.drawText({ font: 'psf', text: 'AAA', size: 8, width: 40, height: 20 });
		const large = abi.drawText({ font: 'psf', text: 'AAA', size: 96, width: 40, height: 20 });

		expect(small.result).toBe(Err.ok);
		expect(large.result).toBe(Err.ok);

		// a fixed cell means the two are the same pixels
		expect(Array.from(large.image!.pixels)).toEqual(Array.from(small.image!.pixels));
		expect(small.metrics!.width).toBe(24);
	});

	it('grows memory for a large render rather than failing', () => {
		const before = abi.pages;

		const drawn = abi.drawText({
			font: 'latin',
			text: 'wide',
			size: 400,
			width: 1400,
			height: 700
		});

		expect(drawn.result).toBe(Err.ok);
		expect(abi.pages).toBeGreaterThanOrEqual(before);

		const ink = drawn.image!.pixels.reduce((sum, value) => sum + value, 0);
		expect(ink).toBeGreaterThan(0);
	});
});

describe('detection through the wasm module', () => {
	let abi: TinyAbi;

	beforeAll(() => {
		abi = new TinyAbi(new WebAssembly.Instance(new WebAssembly.Module(bytes), {}));
	});

	it('refuses a bitmap face whose header sizes a cell or a glyph table past 32 bits', () => {
		// magic, version, header size, flags, glyph count, bytes per glyph, height, width
		const psf2 = (
			glyphs: number,
			bytes: number,
			height: number,
			width: number,
			data: number
		) => {
			const out = new Uint8Array(32 + data).fill(0xff);
			const view = new DataView(out.buffer);

			out.set([0x72, 0xb5, 0x4a, 0x86], 0);
			view.setUint32(8, 32, true);
			view.setUint32(16, glyphs, true);
			view.setUint32(20, bytes, true);
			view.setUint32(24, height, true);
			view.setUint32(28, width, true);

			return out;
		};
		const bdf = (box: string) =>
			new TextEncoder().encode(
				`STARTFONT 2.1\nFONTBOUNDINGBOX ${box}\nCHARS 1\nSTARTCHAR a\nENCODING 97\nBITMAP\n00\nENDCHAR\nENDFONT\n`
			);

		const faces: [string, Uint8Array][] = [
			// two bytes a row times 0x80000001 rows wraps to two, so the size check passes
			['rows', psf2(256, 2, 0x80000001, 16, 512)],
			['wide', psf2(256, 8, 8, 0xffffffff, 2048)],
			// a glyph count and size whose product wraps to what the file holds
			['table', psf2(0x10001, 0x10000, 8, 8, 0x10000)],
			['bdf', bdf('4000 8 0 0')],
			['negative', bdf('-1 8 0 0')]
		];

		try {
			for (const [id, face] of faces) {
				expect(abi.loadBlob(Blob.font, id, face), id).toBe(Err.ok);
				expect(
					abi.drawText({ font: id, text: 'A', size: 8, width: 40, height: 20 }).result,
					id
				).toBe(Err.corrupt);
			}
		} finally {
			for (const [id] of faces) {
				const name = abi.writeString(id);

				abi.exports.tiny_blob_free(Blob.font, name);
				abi.exports.tiny_free(name);
			}
		}
	});

	it('finds faces with nothing loaded, because the cascades ship', () => {
		const found = abi.detectFaces(fixture('smile.jpg'));

		expect(found.result).toBe(Err.ok);
		expect(found.boxes.length).toBeGreaterThan(0);
	});

	it('checks a cascade at load rather than during a search', () => {
		const id = abi.writeString('bad');

		try {
			expect(
				abi.loadBlob(Blob.cascade, 'bad', fixture('derived/cascades/malformed.bin'))
			).toBe(Err.ok);
			expect(abi.exports.tiny_cascade_check(id)).toBe(Err.corrupt);
		} finally {
			abi.exports.tiny_free(id);
			abi.exports.tiny_blob_free_all();
		}
	});

	it('finds the face in the reference portrait and nothing in the reference photograph', () => {
		expect(
			abi.loadBlob(Blob.cascade, 'frontal', fixture('derived/cascades/lbp-frontalface.bin'))
		).toBe(Err.ok);
		expect(
			abi.loadBlob(Blob.cascade, 'profile', fixture('derived/cascades/lbp-profileface.bin'))
		).toBe(Err.ok);

		const portrait = abi.detectFaces(fixture('smile.jpg'));

		expect(portrait.result).toBe(Err.ok);
		expect(portrait.boxes.length).toBeGreaterThan(0);

		// the box came back at the stride the module reported, so its fields have to be sane
		const face = portrait.boxes[0]!;
		expect(face.width).toBeGreaterThan(0);
		expect(face.height).toBeGreaterThan(0);
		expect(face.x + face.width).toBeLessThanOrEqual(portrait.source.width);
		expect(face.y + face.height).toBeLessThanOrEqual(portrait.source.height);
		expect(face.neighbors).toBeGreaterThanOrEqual(3);

		const nothing = abi.detectFaces(fixture('sf-24.jpg'));
		expect(nothing.result).toBe(Err.ok);
		expect(nothing.boxes).toHaveLength(0);
	});

	it('takes the options a host writes into the structure it sized', () => {
		// a min_size larger than the image leaves no scale to search
		const none = abi.detectFaces(fixture('smile.jpg'), { minSize: 9000 });
		expect(none.result).toBe(Err.ok);
		expect(none.boxes).toHaveLength(0);

		// and a threshold nothing clears finds nothing, which says the field landed where the
		// module reads it from
		const strict = abi.detectFaces(fixture('smile.jpg'), {
			minSize: 200,
			minNeighbors: 500
		});
		expect(strict.result).toBe(Err.ok);
		expect(strict.boxes).toHaveLength(0);
	});

	it('answers the computed gravities, and falls back when the detector finds nothing', () => {
		const image = abi.exports.tiny_alloc(abi.exports.tiny_image_sizeof());
		const buffer = abi.copyIn(fixture('sf-24.jpg'));
		const out = abi.exports.tiny_alloc(8);

		try {
			expect(
				abi.exports.tiny_image_load_scaled(
					image,
					buffer,
					fixture('sf-24.jpg').byteLength,
					400,
					400
				)
			).toBe(Err.ok);

			const view = () => new DataView(abi.exports.memory.buffer);

			expect(abi.exports.tiny_image_focus(image, Gravity.auto, out, out + 4)).toBe(Err.ok);
			const autoX = view().getFloat32(out, true);
			const autoY = view().getFloat32(out + 4, true);

			expect(abi.exports.tiny_image_focus(image, Gravity.face, out, out + 4)).toBe(Err.ok);
			const faceX = view().getFloat32(out, true);
			const faceY = view().getFloat32(out + 4, true);

			// no face in this photograph, so the face answer is the auto answer exactly
			expect(faceX).toBe(autoX);
			expect(faceY).toBe(autoY);
			expect(autoX).toBeGreaterThan(0);
			expect(autoX).toBeLessThan(1);

			// a fixed gravity is arithmetic and reads no pixels
			expect(abi.exports.tiny_image_focus(image, Gravity.northWest, out, out + 4)).toBe(
				Err.ok
			);
			expect(view().getFloat32(out, true)).toBe(0);
			expect(view().getFloat32(out + 4, true)).toBe(0);

			abi.exports.tiny_image_destroy(image);
		} finally {
			abi.exports.tiny_free(out);
			abi.exports.tiny_free(image);
			abi.exports.tiny_free(buffer);
		}
	});

	it('leaves an image alone when the anonymizer finds no face', () => {
		abi.exports.tiny_blob_free_all();

		const source = fixture('mountains.jpg');
		const image = abi.exports.tiny_alloc(abi.exports.tiny_image_sizeof());
		const buffer = abi.copyIn(source);

		try {
			expect(
				abi.exports.tiny_image_load_scaled(image, buffer, source.byteLength, 400, 400)
			).toBe(Err.ok);

			const before = abi.copyOut(
				abi.exports.tiny_image_getdata(image),
				abi.exports.tiny_image_getsize(image)
			);

			expect(abi.exports.tiny_image_blur_faces(image, 0)).toBe(Err.ok);

			const after = abi.copyOut(
				abi.exports.tiny_image_getdata(image),
				abi.exports.tiny_image_getsize(image)
			);

			expect(Array.from(after)).toEqual(Array.from(before));

			abi.exports.tiny_image_destroy(image);
		} finally {
			abi.exports.tiny_free(image);
			abi.exports.tiny_free(buffer);
		}
	});
});
