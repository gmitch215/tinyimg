import { Image, mimeFor, type EncodeOptions } from './image.js';
import { readColor, readSource, type Color, type ImageFormat, type Source } from './types.js';
import type { TinyImgModule } from './wasm.js';

/** How a run of text is set. Mirrors `TinyTextStyle`. */
export interface TextStyle {
	/** Em size in pixels. */
	size: number;
	/** Extra space between glyphs. Negative tightens. */
	tracking?: number;
	/** Multiple of the face's own line height. */
	lineHeight?: number;
	/** Apply the face's kern pairs. Defaults to true. */
	kerning?: boolean;
	/** Outline width in pixels. */
	stroke?: number;
	/** The outline's color. */
	strokeColor?: Color;
	/** A shadow under the run, or omitted for none. */
	shadow?: TextShadow;
}

/** A shadow under a run of text. Any non-zero field asks for one. */
export interface TextShadow {
	/** Horizontal offset in pixels. Negative is left. */
	x?: number;
	/** Vertical offset in pixels. Negative is up. */
	y?: number;
	/** Softness in pixels, or 0 for a hard offset copy. */
	blur?: number;
	/** The shadow's color. Defaults to opaque black. */
	color?: Color;
}

/** Where a run sits inside the rectangle it was given. Mirrors `TinyTextBox`. */
export interface TextBox {
	/** Width to wrap inside, or 0 for no wrapping. */
	width?: number;
	/** Height to fill, or 0 for no limit. */
	height?: number;
	/** Where each line sits inside the width. */
	align?: 'left' | 'center' | 'right' | 'justify';
	/** Where the run sits inside the height. */
	valign?: 'top' | 'middle' | 'bottom';
	/** What happens to what does not fit. */
	overflow?: 'clip' | 'ellipsis';
}

/** One line of a measured run. Mirrors `TinyTextLine`. */
export interface TextLine {
	/** Byte offset of the line's first character. */
	at: number;
	/** Bytes the line holds. */
	length: number;
	/** Advance width in pixels. */
	width: number;
	/** Left edge once the alignment is applied. */
	x: number;
	/** Top edge of the line box. */
	y: number;
	/** Baseline, measured down from the top of the run. */
	baseline: number;
	/** Codepoints the line holds. */
	glyphs: number;
	/** Codepoints no face in the chain had a glyph for. */
	missing: number;
	/** The box's height stopped this line being drawn. */
	clipped: boolean;
	/** This line was cut short and ends in an ellipsis. */
	ellipsized: boolean;
}

const ALIGN = { left: 0, center: 1, right: 2, justify: 3 } as const;
const VALIGN = { top: 0, middle: 1, bottom: 2 } as const;
const OVERFLOW = { clip: 0, ellipsis: 1 } as const;

/** Writes a `TinyTextStyle` into module memory. */
function writeStyle(module: TinyImgModule, style: TextStyle, channels: number): number {
	const at = module.alloc(module.exports.tiny_text_style_sizeof());
	const view = module.view();

	module.exports.tiny_text_style(at, style.size);

	view.setFloat32(at + 4, style.tracking ?? 0, true);
	view.setFloat32(at + 8, style.lineHeight ?? 1, true);
	view.setFloat32(at + 12, style.stroke ?? 0, true);
	view.setFloat32(at + 16, style.shadow?.x ?? 0, true);
	view.setFloat32(at + 20, style.shadow?.y ?? 0, true);
	view.setFloat32(at + 24, style.shadow?.blur ?? 0, true);
	view.setUint8(at + 28, style.kerning === false ? 0 : 1);

	const bytes = new Uint8Array(module.exports.memory.buffer);

	bytes.set(readColor(style.strokeColor ?? [0, 0, 0, 255], 4), at + 29);
	bytes.set(readColor(style.shadow?.color ?? [0, 0, 0, 255], 4), at + 33);

	// the channel count only matters to the caller's own color, which the draw takes separately
	void channels;

	return at;
}

/** Writes a `TinyTextBox` into module memory. */
function writeBox(module: TinyImgModule, box: TextBox | undefined): number {
	const at = module.alloc(module.exports.tiny_text_box_sizeof());
	const view = module.view();

	view.setUint32(at, Math.max(0, Math.round(box?.width ?? 0)), true);
	view.setUint32(at + 4, Math.max(0, Math.round(box?.height ?? 0)), true);
	view.setUint32(at + 8, ALIGN[box?.align ?? 'left'], true);
	view.setUint32(at + 12, VALIGN[box?.valign ?? 'top'], true);
	view.setUint32(at + 16, OVERFLOW[box?.overflow ?? 'clip'], true);

	return at;
}

/** Reads a `TinyTextLine` back out of module memory. */
function readLine(module: TinyImgModule, at: number): TextLine {
	const view = module.view();

	return {
		at: view.getUint32(at, true),
		length: view.getUint32(at + 4, true),
		width: view.getFloat32(at + 8, true),
		x: view.getFloat32(at + 12, true),
		y: view.getFloat32(at + 16, true),
		baseline: view.getFloat32(at + 20, true),
		glyphs: view.getUint32(at + 24, true),
		missing: view.getUint32(at + 28, true),
		clipped: view.getUint8(at + 32) !== 0,
		ellipsized: view.getUint8(at + 33) !== 0
	};
}

/** A loaded face, held open for as long as the caller needs it. */
export class Font implements Disposable {
	readonly #module: TinyImgModule;
	readonly #font: number;
	#alive = true;

	private constructor(module: TinyImgModule, font: number) {
		this.#module = module;
		this.#font = font;
	}

	/**
	 * Loads a face from a blob, or from the one the module carries.
	 *
	 * With no id at all this is the compiled-in `sans`, which covers ASCII and eight accented
	 * letters and needs no host wiring. A wider glyph set or a different face is
	 * {@link TinyImgModule.loadBlob} followed by its id here.
	 *
	 * @param module The loaded module.
	 * @param id The blob id, or omitted for the first resident font and then the builtin.
	 */
	static load(module: TinyImgModule, id?: string): Font {
		const font = module.alloc(module.exports.tiny_font_sizeof());
		const name = id === undefined ? 0 : module.writeString(id);

		try {
			module.check(module.exports.tiny_font_load(font, name), `load the font '${id ?? ''}'`);
		} catch (error) {
			module.free(font);
			throw error;
		} finally {
			if (name !== 0) module.free(name);
		}

		return new Font(module, font);
	}

	/** @internal The module pointer, for the drawing entry points. */
	get pointer(): number {
		if (!this.#alive) throw new TypeError('the font has been disposed');
		return this.#font;
	}

	/** Whether the face has a glyph for a codepoint. */
	has(codepoint: number): boolean {
		return this.#module.exports.tiny_font_has_glyph(this.pointer, codepoint) === 1;
	}

	/** Releases the face. The bytes it borrowed are the blob's and are not freed. */
	dispose(): void {
		if (!this.#alive) return;

		this.#module.exports.tiny_font_free(this.#font);
		this.#module.free(this.#font);
		this.#alive = false;
	}

	/** Releases the face, so `using` works on it. */
	[Symbol.dispose](): void {
		this.dispose();
	}
}

/** What {@link og} takes. */
export interface OgOptions {
	/** Card width. Defaults to 1200, which is what the OG spec asks for. */
	width?: number;
	/** Card height. Defaults to 630. */
	height?: number;
	/** Solid background color, drawn under everything. */
	background?: Color;
	/** An image to cover the card with, under the text. */
	image?: Source;
	/** How much of the card the text keeps clear of the edges. Defaults to 64. */
	padding?: number;
	/** The headline. */
	title?: string;
	/** The line under it. */
	subtitle?: string;
	/** A small line at the bottom, for a site name or a date. */
	footer?: string;
	/** Text color. Defaults to white. */
	color?: Color;
	/** Subtitle and footer color. Defaults to {@link OgOptions.color} at 70% alpha. */
	mutedColor?: Color;
	/** Blob id of the face to set it in, or omitted for the one the module carries. */
	font?: string;
	/** Title size in pixels. Defaults to a tenth of the height. */
	titleSize?: number;
	/** Subtitle size. Defaults to two fifths of the title. */
	subtitleSize?: number;
	/** Footer size. Defaults to a quarter of the title. */
	footerSize?: number;
	/** A shadow under the text, which is what keeps a headline readable over a photograph. */
	shadow?: boolean;
	/** Container to encode as. Defaults to PNG. */
	format?: ImageFormat;
	/** Encoder settings. */
	encode?: EncodeOptions;
}

/** What {@link og} produces. */
export interface OgResult {
	/** The encoded card. */
	data: Uint8Array<ArrayBuffer>;
	/** What it was encoded as. */
	format: ImageFormat;
	/** Its content type. */
	contentType: string;
	/** Card width in pixels. */
	width: number;
	/** Card height in pixels. */
	height: number;
	/** Non-zero when the title did not fit and was truncated. */
	titleTruncated: boolean;
	/** Codepoints no face in the chain had a glyph for. */
	missingGlyphs: number;
	/** The bytes as a `Response`, typed and length-set. */
	response(headers?: HeadersInit): Response;
}

/**
 * Renders an Open Graph card.
 *
 * The whole of what a social preview needs in one call: a background, a headline that wraps and
 * truncates instead of running off the card, a subtitle, a footer, and a shadow so the text stays
 * readable over a photograph. Nothing has to be loaded first, because the face is compiled in.
 *
 * ```ts
 * const card = await og(tinyimg, {
 *   title: 'How a planner made the decode cheap',
 *   subtitle: 'Deciding what to decode before decoding it',
 *   footer: 'tinyimg.gmitch215.dev',
 *   background: '#0b1020'
 * });
 *
 * return card.response({ 'cache-control': 'public, max-age=86400' });
 * ```
 *
 * The report is the part worth reading before serving it: `titleTruncated` says the headline was
 * cut, and `missingGlyphs` says the face had nothing for some of the characters, which is what a
 * card with empty boxes in the headline looks like from the inside.
 *
 * @param module The loaded module.
 * @param options What to draw.
 */
export async function og(module: TinyImgModule, options: OgOptions = {}): Promise<OgResult> {
	const width = Math.max(1, Math.round(options.width ?? 1200));
	const height = Math.max(1, Math.round(options.height ?? 630));
	const padding = Math.max(0, Math.round(options.padding ?? 64));

	const titleSize = options.titleSize ?? Math.round(height / 10);
	const subtitleSize = options.subtitleSize ?? Math.round(titleSize * 0.4);
	const footerSize = options.footerSize ?? Math.round(titleSize * 0.25);

	const color = readColor(options.color ?? '#ffffff', 4);
	const muted = options.mutedColor === undefined ? color : readColor(options.mutedColor, 4);

	const image = module.alloc(module.exports.tiny_image_sizeof());
	const writer = module.alloc(module.exports.tiny_writer_sizeof());

	let truncated = false;
	let missing = 0;

	try {
		if (options.image) {
			// the background photograph is fitted by the planner, so a 4000 px source is never
			// decoded at 4000 px to end up 1200 wide
			using base = await Image.open(module, await readSource(options.image));
			base.fit(width, height, { fit: 'cover' });

			const raw = await base.pixels();

			module.check(
				module.exports.tiny_image_create(image, width, height, 4),
				'create the card'
			);

			// the card is always four channels, so a three channel background is widened here
			// rather than every later step having to ask how many the source had
			const target = new Uint8Array(module.exports.memory.buffer);
			const at = module.exports.tiny_image_getdata(image);

			if (raw.channels === 4) {
				target.set(raw.pixels, at);
			} else {
				for (let i = 0; i < raw.width * raw.height; i++) {
					const from = i * raw.channels;

					target[at + i * 4] = raw.pixels[from]!;
					target[at + i * 4 + 1] = raw.pixels[from + (raw.channels > 2 ? 1 : 0)]!;
					target[at + i * 4 + 2] = raw.pixels[from + (raw.channels > 2 ? 2 : 0)]!;
					target[at + i * 4 + 3] = 255;
				}
			}
		} else {
			module.check(
				module.exports.tiny_image_create(image, width, height, 4),
				'create the card'
			);

			if (options.background !== undefined) {
				const fill = module.alloc(4);
				new Uint8Array(module.exports.memory.buffer).set(
					readColor(options.background, 4),
					fill
				);

				try {
					module.check(
						module.exports.tiny_image_fill_rectangle(image, 0, 0, width, height, fill),
						'fill the background'
					);
				} finally {
					module.free(fill);
				}
			}
		}

		using font = Font.load(module, options.font);

		const inner = width - 2 * padding;
		const shadow = options.shadow ?? true;

		const styleOf = (size: number): TextStyle => ({
			size,
			lineHeight: 1.15,
			...(shadow
				? { shadow: { x: 0, y: Math.max(1, Math.round(size / 16)), blur: size / 12 } }
				: {})
		});

		let cursor = padding;

		if (options.title) {
			cursor = drawBlock(
				module,
				image,
				font,
				options.title,
				padding,
				cursor,
				inner,
				Math.round(titleSize * 1.15 * 3),
				styleOf(titleSize),
				color,
				(line) => {
					missing += line.missing;
					if (line.ellipsized) truncated = true;
				}
			);

			cursor = Math.round(cursor + titleSize * 0.1);
		}

		if (options.subtitle) {
			cursor = drawBlock(
				module,
				image,
				font,
				options.subtitle,
				padding,
				cursor,
				inner,
				Math.round(subtitleSize * 1.15 * 3),
				styleOf(subtitleSize),
				muted
			);
		}

		if (options.footer) {
			drawBlock(
				module,
				image,
				font,
				options.footer,
				padding,
				height - padding - Math.round(footerSize * 1.4),
				inner,
				Math.round(footerSize * 1.4),
				styleOf(footerSize),
				muted
			);
		}

		const format = options.format ?? 'png';
		const encode = options.encode ?? {};

		module.check(module.exports.tiny_writer_init(writer, 0), 'prepare the encoder');

		const opts = module.alloc(module.exports.tiny_encode_opts_sizeof());
		const view = module.view();

		view.setUint8(opts, encode.quality ?? 0);
		view.setUint8(opts + 1, encode.lossless ? 1 : 0);
		view.setUint8(opts + 2, encode.progressive ? 1 : 0);
		view.setUint8(opts + 3, encode.stripMetadata ? 1 : 0);
		view.setUint8(opts + 4, encode.effort === 'fast' ? 1 : 0);
		view.setUint8(opts + 5, 0);

		try {
			module.check(
				module.exports.tiny_image_encode(image, module.formatId(format), opts, writer),
				'encode the card'
			);

			const data = module.copyOut(
				module.exports.tiny_writer_data(writer),
				module.exports.tiny_writer_size(writer)
			);
			const contentType = mimeFor(format);

			return {
				data,
				format,
				contentType,
				width,
				height,
				titleTruncated: truncated,
				missingGlyphs: missing,
				response: (headers?: HeadersInit) => {
					const merged = new Headers(headers);
					merged.set('content-type', contentType);
					merged.set('content-length', String(data.byteLength));

					return new Response(data, { headers: merged });
				}
			};
		} finally {
			module.free(opts);
			module.exports.tiny_writer_free(writer);
		}
	} finally {
		module.exports.tiny_image_destroy(image);
		module.free(image);
		module.free(writer);
	}
}

/**
 * Draws one block of text and reports where the next one starts.
 *
 * The measure runs after the draw rather than before it, because the two share every line of
 * layout: measuring first and drawing second would be the same answer and one more walk.
 */
function drawBlock(
	module: TinyImgModule,
	image: number,
	font: Font,
	content: string,
	x: number,
	y: number,
	width: number,
	height: number,
	style: TextStyle,
	color: Uint8Array,
	each?: (line: TextLine) => void
): number {
	const at = writeStyle(module, style, 4);
	const box = writeBox(module, { width, height, overflow: 'ellipsis' });
	const text = module.writeString(content);
	const fill = module.alloc(4);
	const lines = module.alloc(module.exports.tiny_text_line_sizeof() * 4);
	const count = module.alloc(4);

	try {
		new Uint8Array(module.exports.memory.buffer).set(color, fill);

		module.check(
			module.exports.tiny_image_draw_text_in(image, font.pointer, text, x, y, box, at, fill),
			'draw the text'
		);
		module.check(
			module.exports.tiny_text_lines(font.pointer, text, box, at, lines, 4, count),
			'measure the text'
		);

		const total = Math.min(module.view().getUint32(count, true), 4);
		let bottom = y;

		for (let i = 0; i < total; i++) {
			const line = readLine(module, lines + i * module.exports.tiny_text_line_sizeof());

			each?.(line);
			if (!line.clipped) bottom = y + line.y + style.size * (style.lineHeight ?? 1);
		}

		return Math.round(bottom + style.size * 0.4);
	} finally {
		module.free(count);
		module.free(lines);
		module.free(fill);
		module.free(text);
		module.free(box);
		module.free(at);
	}
}
