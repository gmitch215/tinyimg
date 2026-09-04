/**
 * A hand-written driver for the wasm module's raw exports.
 *
 * This is not the shipped wrapper; it exists so both test lanes can exercise the compiled module
 * before the TypeScript tier is written, and so a failure points at the C rather than at the
 * wrapper. `tests/node` uses it against a module compiled from bytes, which only node allows.
 */

/**
 * The subset of the module's exports the tests drive.
 */
export interface TinyExports {
	memory: WebAssembly.Memory;
	tiny_init(): void;
	tiny_version(): number;
	tiny_abi_version(): number;
	tiny_features(): number;
	tiny_error_name(code: number): number;
	tiny_alloc(size: number): number;
	tiny_realloc(pointer: number, size: number): number;
	tiny_free(pointer: number): void;
	tiny_arena_reset(): void;
	tiny_image_sizeof(): number;
	tiny_image_info_sizeof(): number;
	tiny_writer_sizeof(): number;
	tiny_writer_init(writer: number, initial: number): number;
	tiny_writer_free(writer: number): void;
	tiny_writer_data(writer: number): number;
	tiny_writer_size(writer: number): number;
	tiny_image_create(image: number, width: number, height: number, channels: number): number;
	tiny_image_destroy(image: number): number;
	tiny_image_probe(buffer: number, size: number, info: number): number;
	tiny_image_load(image: number, buffer: number, size: number): number;
	tiny_image_load_scaled(
		image: number,
		buffer: number,
		size: number,
		maxWidth: number,
		maxHeight: number
	): number;
	tiny_image_load_region(
		image: number,
		buffer: number,
		size: number,
		x: number,
		y: number,
		width: number,
		height: number
	): number;
	tiny_image_encode(image: number, format: number, opts: number, writer: number): number;
	tiny_image_convert_channels(image: number, channels: number): number;
	tiny_image_getwidth(image: number): number;
	tiny_image_getheight(image: number): number;
	tiny_image_getchannels(image: number): number;
	tiny_image_getdata(image: number): number;
	tiny_image_getsize(image: number): number;
	tiny_image_getformat(image: number): number;
	tiny_image_has_exif(image: number): number;
	tiny_image_get_exif(image: number, data: number, size: number): number;
	tiny_image_strip_exif(image: number): number;
	tiny_image_get_metadata(image: number, key: number, value: number): number;
	tiny_image_get_metadata_count(image: number, count: number): number;
	tiny_plan_sizeof(): number;
	tiny_plan_resolution_sizeof(): number;
	tiny_plan_init(plan: number, buffer: number, size: number): number;
	tiny_plan_init_image(plan: number, image: number): number;
	tiny_plan_set_fusion(plan: number, enabled: number): number;
	tiny_plan_background(plan: number, color: number): number;
	tiny_plan_crop(plan: number, x: number, y: number, width: number, height: number): number;
	tiny_plan_resize(plan: number, width: number, height: number): number;
	tiny_plan_resize_with(plan: number, width: number, height: number, filter: number): number;
	tiny_plan_fit(
		plan: number,
		width: number,
		height: number,
		mode: number,
		gravity: number
	): number;
	tiny_plan_flip_horizontal(plan: number): number;
	tiny_plan_flip_vertical(plan: number): number;
	tiny_plan_rotate(plan: number, degrees: number): number;
	tiny_plan_brightness(plan: number, factor: number): number;
	tiny_plan_contrast(plan: number, factor: number): number;
	tiny_plan_saturation(plan: number, factor: number): number;
	tiny_plan_hue(plan: number, degrees: number): number;
	tiny_plan_grayscale(plan: number): number;
	tiny_plan_invert(plan: number): number;
	tiny_plan_gamma(plan: number, gamma: number): number;
	tiny_plan_blur(plan: number, radius: number): number;
	tiny_plan_gaussian_blur(plan: number, sigma: number): number;
	tiny_plan_matrix(plan: number, matrix: number): number;
	tiny_plan_curve(plan: number, kind: number, params: number, channels: number): number;
	tiny_plan_effect(plan: number, kind: number, params: number): number;
	tiny_plan_effect_rect(
		plan: number,
		kind: number,
		params: number,
		x: number,
		y: number,
		width: number,
		height: number
	): number;
	tiny_plan_count(plan: number): number;
	tiny_plan_resolve(plan: number, resolution: number): number;
	tiny_plan_run(plan: number, out: number): number;
	tiny_plan_encode(plan: number, format: number, opts: number, writer: number): number;

	tiny_display_sizeof(): number;
	tiny_display_init(list: number): number;
	tiny_display_save(list: number): number;
	tiny_display_restore(list: number): number;
	tiny_display_translate(list: number, x: number, y: number): number;
	tiny_display_scale(list: number, x: number, y: number): number;
	tiny_display_rotate(list: number, degrees: number): number;
	tiny_display_blend(list: number, blend: number): number;
	tiny_display_rect(
		list: number,
		x: number,
		y: number,
		width: number,
		height: number,
		color: number
	): number;
	tiny_display_ellipse(
		list: number,
		cx: number,
		cy: number,
		rx: number,
		ry: number,
		color: number
	): number;
	tiny_display_render(list: number, image: number): number;
	tiny_display_culled(list: number): number;
	tiny_display_covered(list: number): number;
	tiny_display_bounds(list: number, x: number, y: number, width: number, height: number): number;

	tiny_image_create(image: number, width: number, height: number, channels: number): number;
	tiny_image_fill_rectangle(
		image: number,
		x: number,
		y: number,
		width: number,
		height: number,
		pixel: number
	): number;
	tiny_image_fill_circle(
		image: number,
		cx: number,
		cy: number,
		radius: number,
		pixel: number
	): number;
	tiny_image_composite(dest: number, src: number, blend: number): number;
	tiny_image_premultiply(image: number): number;
	tiny_image_unpremultiply(image: number): number;
	tiny_image_preset(image: number, preset: number): number;
	tiny_image_posterize(image: number, levels: number): number;
	tiny_image_sobel(image: number): number;
	tiny_image_pixelate(image: number, size: number): number;
	tiny_image_rotate(image: number, degrees: number, background: number): number;
	tiny_image_trim(image: number, tolerance: number): number;
	tiny_image_remove_background(image: number, tolerance: number): number;
	tiny_image_histogram(image: number, channel: number, bins: number): number;
	tiny_image_average_color(image: number, color: number): number;
	tiny_image_dominant_color(image: number, color: number): number;
	tiny_image_palette(image: number, count: number, palette: number): number;
	tiny_image_phash(image: number, hash: number): number;
	tiny_phash_distance(first: bigint, second: bigint): number;

	tiny_rgb_to_hsv(r: number, g: number, b: number, h: number, s: number, v: number): number;
	tiny_hsv_to_rgb(h: number, s: number, v: number, r: number, g: number, b: number): number;
	tiny_gradient_rgb(
		r1: number,
		g1: number,
		b1: number,
		r2: number,
		g2: number,
		b2: number,
		steps: number
	): number;
	tiny_icc_parse(profile: number, data: number, size: number): number;
	tiny_icc_srgb(profile: number): number;
	tiny_icc_convert_image(image: number, profile: number): number;

	tiny_blob_load(kind: number, id: number, data: number, size: number): number;
	tiny_blob_free(kind: number, id: number): number;
	tiny_blob_free_all(): void;

	tiny_font_sizeof(): number;
	tiny_font_metrics_sizeof(): number;
	tiny_font_load(font: number, id: number): number;
	tiny_font_free(font: number): void;
	tiny_font_metrics(font: number, size: number, out: number): number;
	tiny_font_has_glyph(font: number, codepoint: number): number;
	tiny_text_style_sizeof(): number;
	tiny_text_metrics_sizeof(): number;
	tiny_text_style(style: number, size: number): void;
	tiny_text_measure(font: number, text: number, style: number, out: number): number;
	tiny_text_measure_wrapped(
		font: number,
		text: number,
		width: number,
		style: number,
		out: number
	): number;
	tiny_image_draw_text(
		image: number,
		font: number,
		text: number,
		x: number,
		y: number,
		style: number,
		color: number
	): number;
	tiny_image_draw_text_box(
		image: number,
		font: number,
		text: number,
		x: number,
		y: number,
		width: number,
		height: number,
		style: number,
		align: number,
		color: number
	): number;

	tiny_face_box_sizeof(): number;
	tiny_detect_opts_sizeof(): number;
	tiny_detect_opts(opts: number): void;
	tiny_cascade_check(id: number): number;
	tiny_image_detect_faces(image: number, boxes: number, capacity: number, count: number): number;
	tiny_image_detect_faces_ex(
		image: number,
		opts: number,
		boxes: number,
		capacity: number,
		count: number
	): number;
	tiny_image_focus(image: number, gravity: number, x: number, y: number): number;
	tiny_image_blur_faces(image: number, sigma: number): number;
	tiny_image_pixelate_faces(image: number, size: number): number;
}

/**
 * The part of a TinyPlanResolution the tests read back, which is what the planner decided to ask
 * the decoder for.
 */
export interface PlanResolution {
	decode: {
		x: number;
		y: number;
		width: number;
		height: number;
		scale: number;
		channels: number;
	};
	decodeWidth: number;
	decodeHeight: number;
}

/**
 * Resample filters, mirroring TinyResampleFilter.
 */
export const Filter = {
	auto: 0,
	nearest: 1,
	bilinear: 2,
	box: 3,
	catmullRom: 4
} as const;

/**
 * Fit modes, mirroring TinyImageFit.
 */
export const Fit = {
	scaleDown: 0,
	contain: 1,
	cover: 2,
	crop: 3,
	aspectCrop: 4,
	aspectContain: 5,
	aspectCover: 6,
	pad: 7,
	stretch: 8,
	fill: 9,
	scaleUp: 10
} as const;

/**
 * Gravities, mirroring TinyImageGravity.
 */
export const Gravity = {
	center: 0,
	north: 1,
	south: 2,
	west: 3,
	east: 4,
	northWest: 5,
	northEast: 6,
	southWest: 7,
	southEast: 8,
	auto: 9,
	face: 10
} as const;

/**
 * Blob kinds, mirroring TinyBlobKind.
 */
export const Blob = {
	font: 0,
	icc: 1,
	cascade: 2
} as const;

/**
 * Text alignments, mirroring TinyTextAlign.
 */
export const Align = {
	left: 0,
	center: 1,
	right: 2
} as const;

/**
 * Blend modes, mirroring TinyBlendMode.
 */
export const Blend = {
	normal: 0,
	replace: 1,
	multiply: 2,
	screen: 3,
	overlay: 4,
	darken: 5,
	lighten: 6,
	difference: 7,
	exclusion: 8,
	hardLight: 9,
	softLight: 10,
	add: 11,
	subtract: 12
} as const;

/**
 * Tone curves, mirroring TinyCurveKind.
 */
export const Curve = {
	gamma: 0,
	posterize: 1,
	threshold: 2,
	solarize: 3,
	exposure: 4,
	levels: 5,
	fillLight: 6,
	gain: 7,
	sigmoid: 8,
	negate: 9,
	srgb: 10,
	balance: 11
} as const;

/**
 * Neighborhood effects, mirroring TinyEffectKind.
 */
export const Fx = {
	unsharp: 0,
	clarity: 1,
	sobel: 2,
	emboss: 3,
	pixelate: 4,
	median: 5,
	dilate: 6,
	erode: 7,
	outline: 8,
	motionBlur: 9,
	radialBlur: 10,
	zoomBlur: 11,
	tiltShift: 12,
	blurRegion: 13,
	pixelateRegion: 14,
	chromatic: 15,
	dither: 16,
	halftone: 17,
	scanlines: 18
} as const;

/**
 * Named looks, mirroring TinyImagePreset.
 */
export const Preset = {
	noir: 0,
	chrome: 1,
	mono: 2,
	fade: 3,
	vivid: 4,
	warm: 5,
	cool: 6,
	instant: 7,
	tonal: 8
} as const;

/**
 * Format ids, mirroring TinyImageFormat.
 */
export const Format = {
	unknown: 0,
	png: 1,
	jpeg: 2,
	bmp: 3,
	gif: 4,
	tiff: 5,
	webp: 6,
	avif: 7,
	heif: 8
} as const;

/**
 * Error codes, mirroring TinyImageError.
 */
export const Err = {
	ok: 0,
	null: -1,
	range: -2,
	bounds: -3,
	memory: -4,
	tooLarge: -5,
	unknownFormat: -6,
	unsupportedCodec: -7,
	corrupt: -8,
	unsupportedVariant: -9,
	bufferTooSmall: -10,
	blobMissing: -11,
	noChannel: -12,
	notFound: -13,
	plan: -14
} as const;

/**
 * What tiny_image_probe writes, read at the offsets image.h documents as ABI stable.
 */
export interface ProbeResult {
	width: number;
	height: number;
	frames: number;
	format: number;
	channels: number;
	bitDepth: number;
	hasAlpha: boolean;
	progressive: boolean;
}

/**
 * A decoded image still living in the module's memory.
 */
export interface DecodedImage {
	width: number;
	height: number;
	channels: number;
	format: number;
	pixels: Uint8Array;
	meta: ImageMetadata;
}

/**
 * What an image carries besides its pixels.
 */
export interface ImageMetadata {
	hasExif: boolean;
	exif: Uint8Array | undefined;
	count: number;
	orientation: string | undefined;
}

/**
 * What an encoder honors, mirroring TinyEncodeOpts.
 *
 * Every field is a byte in the C struct, in this order.
 */
export interface EncodeOptions {
	quality?: number;
	lossless?: boolean;
	progressive?: boolean;
	stripMetadata?: boolean;
}

/** A TinyTextMetrics, read back out of the module. */
export interface TextMetrics {
	width: number;
	height: number;
	ascent: number;
	descent: number;
	lineHeight: number;
	lines: number;
	glyphs: number;
	missing: number;
}

/** A TinyFaceBox, read back out of the module. */
export interface FaceBox {
	x: number;
	y: number;
	width: number;
	height: number;
	neighbors: number;
}

export class TinyAbi {
	readonly exports: TinyExports;

	constructor(instance: WebAssembly.Instance) {
		this.exports = instance.exports as unknown as TinyExports;
		this.exports.tiny_init();
	}

	/**
	 * Pages of linear memory the module currently holds.
	 */
	get pages(): number {
		return this.exports.memory.buffer.byteLength / 65536;
	}

	/**
	 * A view over the module's memory, taken fresh because growing detaches the old buffer.
	 */
	private view(): Uint8Array {
		return new Uint8Array(this.exports.memory.buffer);
	}

	/**
	 * Copies bytes into the module and returns the pointer, which the caller frees.
	 */
	copyIn(bytes: Uint8Array): number {
		const pointer = this.exports.tiny_alloc(bytes.byteLength);
		if (pointer === 0) throw new Error(`tiny_alloc refused ${bytes.byteLength} bytes`);

		this.view().set(bytes, pointer);
		return pointer;
	}

	/**
	 * Copies bytes back out of the module.
	 */
	copyOut(pointer: number, size: number): Uint8Array {
		return this.view().slice(pointer, pointer + size);
	}

	/**
	 * Reads a NUL terminated string out of the module.
	 */
	readString(pointer: number): string {
		const memory = this.view();

		let end = pointer;
		while (end < memory.byteLength && memory[end] !== 0) end++;

		return new TextDecoder().decode(memory.subarray(pointer, end));
	}

	/**
	 * Writes a NUL terminated string into the module and returns the pointer, which the caller
	 * frees.
	 */
	writeString(value: string): number {
		const bytes = new TextEncoder().encode(value);
		const pointer = this.exports.tiny_alloc(bytes.byteLength + 1);

		if (pointer === 0) throw new Error(`tiny_alloc refused ${bytes.byteLength + 1} bytes`);

		this.view().set(bytes, pointer);
		this.view()[pointer + bytes.byteLength] = 0;

		return pointer;
	}

	/**
	 * Reads the four numbers a pointer-sized field pair holds.
	 *
	 * wasm32 pointers and size_t are both four bytes, so an out parameter of either kind reads the
	 * same way; a 64 bit build would need this to change with it.
	 */
	readWord(pointer: number): number {
		const memory = this.view();

		return (
			memory[pointer]! |
			(memory[pointer + 1]! << 8) |
			(memory[pointer + 2]! << 16) |
			(memory[pointer + 3]! << 24)
		);
	}

	/**
	 * Everything an image carries besides its pixels, read back out of the module.
	 */
	metadata(image: number): ImageMetadata {
		const hasExif = this.exports.tiny_image_has_exif(image) === 1;
		const out = this.exports.tiny_alloc(8);

		try {
			let exif: Uint8Array | undefined;

			if (hasExif) {
				const result = this.exports.tiny_image_get_exif(image, out, out + 4);

				if (result === Err.ok) {
					const pointer = this.readWord(out);
					const size = this.readWord(out + 4);

					exif = this.copyOut(pointer, size);
					this.exports.tiny_free(pointer);
				}
			}

			this.exports.tiny_image_get_metadata_count(image, out);
			const count = this.readWord(out);

			const key = this.writeString('exif:Orientation');
			let orientation: string | undefined;

			if (this.exports.tiny_image_get_metadata(image, key, out) === Err.ok) {
				const pointer = this.readWord(out);

				orientation = this.readString(pointer);
				this.exports.tiny_free(pointer);
			}

			this.exports.tiny_free(key);

			return { hasExif, exif, count, orientation };
		} finally {
			this.exports.tiny_free(out);
		}
	}

	errorName(code: number): string {
		return this.readString(this.exports.tiny_error_name(code));
	}

	probe(bytes: Uint8Array): { result: number; info: ProbeResult } {
		const buffer = this.copyIn(bytes);
		const info = this.exports.tiny_alloc(this.exports.tiny_image_info_sizeof());

		try {
			const result = this.exports.tiny_image_probe(buffer, bytes.byteLength, info);
			const fields = new DataView(this.exports.memory.buffer, info);

			return {
				result,
				info: {
					width: fields.getUint32(0, true),
					height: fields.getUint32(4, true),
					frames: fields.getUint32(8, true),
					format: fields.getUint32(12, true),
					channels: fields.getUint8(16),
					bitDepth: fields.getUint8(17),
					hasAlpha: fields.getUint8(18) !== 0,
					progressive: fields.getUint8(19) !== 0
				}
			};
		} finally {
			this.exports.tiny_free(info);
			this.exports.tiny_free(buffer);
		}
	}

	/**
	 * Runs one of the loaders and copies the pixels out, releasing everything it allocated.
	 *
	 * @param bytes The encoded image.
	 * @param call Which loader to use and with what arguments.
	 */
	decode(
		bytes: Uint8Array,
		call: (image: number, buffer: number, size: number) => number = (image, buffer, size) =>
			this.exports.tiny_image_load(image, buffer, size)
	): { result: number; image: DecodedImage | undefined } {
		const buffer = this.copyIn(bytes);
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());

		try {
			const result = call(image, buffer, bytes.byteLength);
			if (result !== Err.ok) return { result, image: undefined };

			const decoded: DecodedImage = {
				width: this.exports.tiny_image_getwidth(image),
				height: this.exports.tiny_image_getheight(image),
				channels: this.exports.tiny_image_getchannels(image),
				format: this.exports.tiny_image_getformat(image),
				pixels: this.copyOut(
					this.exports.tiny_image_getdata(image),
					this.exports.tiny_image_getsize(image)
				),
				meta: this.metadata(image)
			};

			this.exports.tiny_image_destroy(image);
			return { result, image: decoded };
		} finally {
			this.exports.tiny_free(image);
			this.exports.tiny_free(buffer);
		}
	}

	/**
	 * Builds a plan over encoded bytes, runs it, and copies the pixels out.
	 *
	 * The plan and the resolution both live in the module's memory, so this is also what proves the
	 * two structures can be allocated and read across the boundary the way a host will have to.
	 *
	 * @param bytes The encoded image.
	 * @param build Appends operations; the plan pointer is the module's.
	 * @param fusion Zero to run one operation per pass, which is the planner-off arm.
	 */
	plan(
		bytes: Uint8Array,
		build: (plan: number) => void,
		fusion = 1
	): { result: number; image: DecodedImage | undefined; resolution: PlanResolution | undefined } {
		const buffer = this.copyIn(bytes);
		const plan = this.exports.tiny_alloc(this.exports.tiny_plan_sizeof());
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());
		const resolution = this.exports.tiny_alloc(this.exports.tiny_plan_resolution_sizeof());

		try {
			let result = this.exports.tiny_plan_init(plan, buffer, bytes.byteLength);
			if (result !== Err.ok) return { result, image: undefined, resolution: undefined };

			build(plan);
			this.exports.tiny_plan_set_fusion(plan, fusion);

			result = this.exports.tiny_plan_resolve(plan, resolution);
			if (result !== Err.ok) return { result, image: undefined, resolution: undefined };

			const decided = this.readResolution(resolution);

			result = this.exports.tiny_plan_run(plan, image);
			if (result !== Err.ok) {
				return { result, image: undefined, resolution: decided };
			}

			const produced: DecodedImage = {
				width: this.exports.tiny_image_getwidth(image),
				height: this.exports.tiny_image_getheight(image),
				channels: this.exports.tiny_image_getchannels(image),
				format: this.exports.tiny_image_getformat(image),
				pixels: this.copyOut(
					this.exports.tiny_image_getdata(image),
					this.exports.tiny_image_getsize(image)
				),
				meta: this.metadata(image)
			};

			this.exports.tiny_image_destroy(image);
			return { result, image: produced, resolution: decided };
		} finally {
			this.exports.tiny_free(resolution);
			this.exports.tiny_free(image);
			this.exports.tiny_free(plan);
			this.exports.tiny_free(buffer);
		}
	}

	/**
	 * Hands a blob to the module, which takes ownership of the copy.
	 *
	 * @param kind One of {@link Blob}.
	 * @param id Name the module will find it by.
	 * @param bytes The blob's contents.
	 */
	loadBlob(kind: number, id: string, bytes: Uint8Array): number {
		const data = this.copyIn(bytes);
		const name = this.writeString(id);

		try {
			// the module frees `data` itself, so it is not freed here even on failure: a refused
			// load leaks it, which is the caller's problem and not something to double free
			return this.exports.tiny_blob_load(kind, name, data, bytes.byteLength);
		} finally {
			this.exports.tiny_free(name);
		}
	}

	/**
	 * Draws text onto a fresh image and hands the pixels back.
	 *
	 * Everything the text surface needs across the boundary in one call: a face out of the blob
	 * table, a style structure the module sized, and a color.
	 */
	drawText(options: {
		font: string;
		text: string;
		size: number;
		width: number;
		height: number;
		channels?: number;
		color?: number[];
		box?: { width: number; height: number; align: number };
	}): { result: number; image: DecodedImage | undefined; metrics: TextMetrics | undefined } {
		const channels = options.channels ?? 1;
		const samples = options.color ?? [255, 255, 255, 255];

		const font = this.exports.tiny_alloc(this.exports.tiny_font_sizeof());
		const style = this.exports.tiny_alloc(this.exports.tiny_text_style_sizeof());
		const metrics = this.exports.tiny_alloc(this.exports.tiny_text_metrics_sizeof());
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());
		const id = this.writeString(options.font);
		const text = this.writeString(options.text);
		const color = this.copyIn(new Uint8Array(samples.slice(0, channels)));

		try {
			let result = this.exports.tiny_font_load(font, id);
			if (result !== Err.ok) return { result, image: undefined, metrics: undefined };

			this.exports.tiny_text_style(style, options.size);

			result = this.exports.tiny_image_create(image, options.width, options.height, channels);
			if (result !== Err.ok) return { result, image: undefined, metrics: undefined };

			const measured = options.box
				? this.exports.tiny_text_measure_wrapped(
						font,
						text,
						options.box.width,
						style,
						metrics
					)
				: this.exports.tiny_text_measure(font, text, style, metrics);

			if (measured !== Err.ok) {
				return { result: measured, image: undefined, metrics: undefined };
			}

			result = options.box
				? this.exports.tiny_image_draw_text_box(
						image,
						font,
						text,
						0,
						0,
						options.box.width,
						options.box.height,
						style,
						options.box.align,
						color
					)
				: this.exports.tiny_image_draw_text(image, font, text, 0, 0, style, color);

			const read = this.readTextMetrics(metrics);

			if (result !== Err.ok) {
				this.exports.tiny_image_destroy(image);
				return { result, image: undefined, metrics: read };
			}

			const drawn: DecodedImage = {
				width: this.exports.tiny_image_getwidth(image),
				height: this.exports.tiny_image_getheight(image),
				channels: this.exports.tiny_image_getchannels(image),
				format: this.exports.tiny_image_getformat(image),
				pixels: this.copyOut(
					this.exports.tiny_image_getdata(image),
					this.exports.tiny_image_getsize(image)
				),
				meta: this.metadata(image)
			};

			this.exports.tiny_image_destroy(image);
			this.exports.tiny_font_free(font);

			return { result, image: drawn, metrics: read };
		} finally {
			this.exports.tiny_free(color);
			this.exports.tiny_free(text);
			this.exports.tiny_free(id);
			this.exports.tiny_free(image);
			this.exports.tiny_free(metrics);
			this.exports.tiny_free(style);
			this.exports.tiny_free(font);
		}
	}

	/** Reads a TinyTextMetrics, whose layout is five floats then three u32. */
	private readTextMetrics(pointer: number): TextMetrics {
		const view = new DataView(this.exports.memory.buffer);
		const f32 = (offset: number) => view.getFloat32(pointer + offset, true);
		const u32 = (offset: number) => view.getUint32(pointer + offset, true);

		return {
			width: f32(0),
			height: f32(4),
			ascent: f32(8),
			descent: f32(12),
			lineHeight: f32(16),
			lines: u32(20),
			glyphs: u32(24),
			missing: u32(28)
		};
	}

	/**
	 * Decodes an image and finds the faces in it.
	 *
	 * @param bytes The encoded image.
	 * @param opts Overrides for the explicit entry point, or undefined for the default one.
	 */
	detectFaces(
		bytes: Uint8Array,
		opts?: { minSize?: number; maxSize?: number; scaleFactor?: number; minNeighbors?: number }
	): { result: number; boxes: FaceBox[]; source: { width: number; height: number } } {
		const capacity = 16;
		const buffer = this.copyIn(bytes);
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());
		const boxes = this.exports.tiny_alloc(this.exports.tiny_face_box_sizeof() * capacity);
		const count = this.exports.tiny_alloc(4);
		const options = this.exports.tiny_alloc(this.exports.tiny_detect_opts_sizeof());

		try {
			let result = this.exports.tiny_image_load(image, buffer, bytes.byteLength);
			if (result !== Err.ok) {
				return { result, boxes: [], source: { width: 0, height: 0 } };
			}

			const source = {
				width: this.exports.tiny_image_getwidth(image),
				height: this.exports.tiny_image_getheight(image)
			};

			if (opts) {
				this.exports.tiny_detect_opts(options);

				const view = new DataView(this.exports.memory.buffer);
				view.setUint32(options, opts.minSize ?? 0, true);
				view.setUint32(options + 4, opts.maxSize ?? 0, true);
				view.setFloat32(options + 8, opts.scaleFactor ?? 1.1, true);
				view.setUint32(options + 12, opts.minNeighbors ?? 3, true);

				result = this.exports.tiny_image_detect_faces_ex(
					image,
					options,
					boxes,
					capacity,
					count
				);
			} else {
				result = this.exports.tiny_image_detect_faces(image, boxes, capacity, count);
			}

			this.exports.tiny_image_destroy(image);

			if (result !== Err.ok) return { result, boxes: [], source };

			const found = this.readWord(count);
			const view = new DataView(this.exports.memory.buffer);
			const out: FaceBox[] = [];

			for (let i = 0; i < found; i++) {
				const at = boxes + i * 20;
				out.push({
					x: view.getUint32(at, true),
					y: view.getUint32(at + 4, true),
					width: view.getUint32(at + 8, true),
					height: view.getUint32(at + 12, true),
					neighbors: view.getUint32(at + 16, true)
				});
			}

			return { result, boxes: out, source };
		} finally {
			this.exports.tiny_free(options);
			this.exports.tiny_free(count);
			this.exports.tiny_free(boxes);
			this.exports.tiny_free(image);
			this.exports.tiny_free(buffer);
		}
	}

	/**
	 * Reads the fields of a TinyPlanResolution the tests assert on.
	 *
	 * The layout is not part of the ABI, so this reads it the way a host should not: through the
	 * offsets the C compiler chose. It is here rather than in the shipped wrapper for exactly that
	 * reason, and Phase 7 gives the wrapper accessors instead.
	 */
	private readResolution(pointer: number): PlanResolution {
		const view = new DataView(this.exports.memory.buffer);
		const u32 = (offset: number) => view.getUint32(pointer + offset, true);
		const u8 = (offset: number) => view.getUint8(pointer + offset);

		// TinyDecodeOpts is four u32 then two u8, and the doubles that follow are aligned to eight
		return {
			decode: {
				x: u32(0),
				y: u32(4),
				width: u32(8),
				height: u32(12),
				scale: u8(16),
				channels: u8(17)
			},
			decodeWidth: u32(20),
			decodeHeight: u32(24)
		};
	}

	/**
	 * Decodes, re-encodes into the given format, and hands back the encoded bytes.
	 */
	transcode(
		bytes: Uint8Array,
		format: number,
		channels?: number,
		encode?: EncodeOptions
	): { result: number; bytes: Uint8Array | undefined } {
		const buffer = this.copyIn(bytes);
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());
		const writer = this.exports.tiny_alloc(this.exports.tiny_writer_sizeof());

		// four bytes in the order image.h declares them, or a null pointer for the codec's own
		// defaults; the struct is all uint8 so there is no padding to get wrong
		const opts = encode ? this.exports.tiny_alloc(4) : 0;

		if (opts !== 0) {
			this.view().set(
				[
					encode!.quality ?? 0,
					encode!.lossless ? 1 : 0,
					encode!.progressive ? 1 : 0,
					encode!.stripMetadata ? 1 : 0
				],
				opts
			);
		}

		try {
			let result = this.exports.tiny_image_load(image, buffer, bytes.byteLength);
			if (result !== Err.ok) return { result, bytes: undefined };

			if (channels !== undefined) {
				result = this.exports.tiny_image_convert_channels(image, channels);
				if (result !== Err.ok) return { result, bytes: undefined };
			}

			this.exports.tiny_writer_init(writer, 0);
			result = this.exports.tiny_image_encode(image, format, opts, writer);

			const encoded =
				result === Err.ok
					? this.copyOut(
							this.exports.tiny_writer_data(writer),
							this.exports.tiny_writer_size(writer)
						)
					: undefined;

			this.exports.tiny_writer_free(writer);
			this.exports.tiny_image_destroy(image);

			return { result, bytes: encoded };
		} finally {
			if (opts !== 0) this.exports.tiny_free(opts);
			this.exports.tiny_free(writer);
			this.exports.tiny_free(image);
			this.exports.tiny_free(buffer);
		}
	}

	/**
	 * Creates a blank image inside the module and hands it to a callback.
	 *
	 * The drawing surface has no encoded input to start from, so the plan helper above cannot
	 * reach it: a caller draws onto an image it made rather than one it decoded.
	 *
	 * @param width Image width.
	 * @param height Image height.
	 * @param channels How many channels.
	 * @param body Runs with the image pointer; its return value is passed through.
	 * @returns Whatever `body` returned, plus the image's pixels as they ended up.
	 */
	canvas<T>(
		width: number,
		height: number,
		channels: number,
		body: (image: number, abi: TinyAbi) => T
	): { value: T; image: DecodedImage } {
		const image = this.exports.tiny_alloc(this.exports.tiny_image_sizeof());

		try {
			const created = this.exports.tiny_image_create(image, width, height, channels);
			if (created !== Err.ok) throw new Error(`create failed: ${created}`);

			const value = body(image, this);

			const produced: DecodedImage = {
				width: this.exports.tiny_image_getwidth(image),
				height: this.exports.tiny_image_getheight(image),
				channels: this.exports.tiny_image_getchannels(image),
				format: this.exports.tiny_image_getformat(image),
				pixels: this.copyOut(
					this.exports.tiny_image_getdata(image),
					this.exports.tiny_image_getsize(image)
				),
				meta: this.metadata(image)
			};

			this.exports.tiny_image_destroy(image);
			return { value, image: produced };
		} finally {
			this.exports.tiny_free(image);
		}
	}

	/**
	 * Copies bytes into the module and returns the pointer, for the callers that pass a color or
	 * a parameter array.
	 *
	 * @param bytes What to copy.
	 * @returns The pointer, which the caller frees.
	 */
	bytesIn(bytes: Uint8Array): number {
		return this.copyIn(bytes);
	}

	/**
	 * Copies floats into the module and returns the pointer.
	 *
	 * @param values What to copy.
	 * @returns The pointer, which the caller frees.
	 */
	floatsIn(values: number[]): number {
		const pointer = this.exports.tiny_alloc(values.length * 4);
		const view = new DataView(this.exports.memory.buffer);

		for (let i = 0; i < values.length; i++) {
			view.setFloat32(pointer + i * 4, values[i]!, true);
		}

		return pointer;
	}

	/**
	 * Reads back an array of 32-bit unsigned values the module wrote.
	 *
	 * @param pointer Where they are.
	 * @param count How many.
	 * @returns The values.
	 */
	u32sOut(pointer: number, count: number): number[] {
		const view = new DataView(this.exports.memory.buffer);
		const out: number[] = [];

		for (let i = 0; i < count; i++) out.push(view.getUint32(pointer + i * 4, true));

		return out;
	}
}
