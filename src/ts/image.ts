import {
	readColor,
	readSource,
	type Color,
	type Fit,
	type FlipAxis,
	type Gravity,
	type ImageFormat,
	type PlanDecision,
	type RawImage,
	type ResampleFilter,
	type Source
} from './types.js';
import { TinyImgModule } from './wasm.js';

/** Fit modes, mirroring `TinyImageFit`. */
const FITS: Record<Fit, number> = {
	'scale-down': 0,
	contain: 1,
	cover: 2,
	crop: 3,
	'aspect-crop': 4,
	'aspect-contain': 5,
	'aspect-cover': 6,
	pad: 7,
	stretch: 8,
	fill: 9,
	'scale-up': 10
};

/** Gravities, mirroring `TinyImageGravity`. */
const GRAVITIES: Record<Gravity, number> = {
	center: 0,
	north: 1,
	south: 2,
	west: 3,
	east: 4,
	'north-west': 5,
	'north-east': 6,
	'south-west': 7,
	'south-east': 8,
	auto: 9,
	face: 10
};

/** Resample filters, mirroring `TinyResampleFilter`. */
const FILTERS: Record<ResampleFilter, number> = {
	auto: 0,
	nearest: 1,
	bilinear: 2,
	box: 3,
	'catmull-rom': 4
};

/** Special cases the planner reports, mirroring `TinyPlanKernel`. */
const KERNELS: [number, string][] = [
	[1 << 0, 'region'],
	[1 << 1, 'scaled'],
	[1 << 2, 'copy'],
	[1 << 3, 'resample'],
	[1 << 4, 'orient'],
	[1 << 5, 'color'],
	[1 << 6, 'pad'],
	[1 << 7, 'gray-decode'],
	[1 << 8, 'neighborhood']
];

/** `TINYIMG_FX_UNSHARP`. */
const FX_UNSHARP = 0;

/** Named fields of a resolution, mirroring `TinyPlanField`. */
const Field = {
	regionX: 0,
	regionY: 1,
	regionWidth: 2,
	regionHeight: 3,
	scale: 4,
	decodeWidth: 5,
	decodeHeight: 6,
	width: 7,
	height: 8,
	channels: 9,
	ops: 10,
	eliminated: 11,
	collapsed: 12,
	colorStages: 13,
	passes: 14,
	kernels: 15
} as const;

type PlanField = keyof typeof Field;

/** What {@link Image.bytes} and its siblings take. */
export interface EncodeOptions {
	/** Quality for a lossy format, 1 through 100. */
	quality?: number;
	/** Use the format's lossless mode where it has one. */
	lossless?: boolean;
	/** Write a progressive or interlaced stream. */
	progressive?: boolean;
	/** Drop EXIF and other metadata from the output. */
	stripMetadata?: boolean;
	/**
	 * How much computation the encoder may spend. `'fancy'` is the default.
	 *
	 * A separate axis from {@link EncodeOptions.quality}. Quality says what the output should look
	 * like; this says how hard to work to get there. `'fast'` bounds the WebP encoder's 4x4
	 * prediction search to its four whole-block modes, which over six fixtures at 800 wide measured
	 * 1.18x to 1.45x faster for between 8.8% smaller and 17.5% larger, never more than 0.05 dB
	 * apart. Flat-color illustrations pay the most, because hard diagonal edges are what the
	 * omitted modes are for.
	 *
	 * Formats with nothing to trade ignore it and do the exact thing.
	 */
	effort?: 'fancy' | 'fast';
}

/** What {@link Image.fit} takes beyond the target extent. */
export interface FitOptions {
	/** Which part a crop keeps, or where a pad puts the image. */
	gravity?: Gravity;
	/** Fills whatever a pad leaves empty. */
	background?: Color;
}

/**
 * A chainable transformation over one source.
 *
 * **Nothing runs until you ask for the output.** Every method here appends to a plan, and the plan
 * runs once, when {@link bytes} or one of its siblings is called. That is not an implementation
 * detail: it is what lets a 100x100 thumbnail of a 16 megapixel photograph decode a 500x500 region
 * at a quarter scale instead of 16 megapixels, and the decision cannot be made after the first
 * operation has already run.
 *
 * The handle lives in the module's memory, so an image has to be released. Prefer `using`:
 *
 * ```ts
 * using image = await tinyimg.open(request.body);
 * const webp = await image.fit(800, 600, { gravity: 'face' }).sharpen(1).bytes('webp', { quality: 80 });
 * ```
 *
 * Without `using`, call {@link dispose} in a `finally`. {@link bytes} does not dispose, because a
 * caller may want two encodings of one plan.
 */
export class Image {
	readonly #module: TinyImgModule;
	readonly #plan: number;
	readonly #buffer: number;
	readonly #size: number;
	readonly #format: ImageFormat | 'unknown';
	readonly #frames: number;
	readonly #after: ((image: number) => void)[] = [];

	#background: Uint8Array | undefined;
	#disposed = false;

	private constructor(
		module: TinyImgModule,
		plan: number,
		buffer: number,
		size: number,
		format: ImageFormat | 'unknown',
		frames: number
	) {
		this.#module = module;
		this.#plan = plan;
		this.#buffer = buffer;
		this.#size = size;
		this.#format = format;
		this.#frames = frames;
	}

	/**
	 * Opens a source without decoding it.
	 *
	 * The header is read so the planner knows the extent it is working from; no pixel is touched
	 * until the plan runs.
	 *
	 * @param module The loaded module.
	 * @param source The encoded image, in any shape {@link Source} accepts.
	 * @return A transformation with no operations in it yet.
	 */
	static async open(module: TinyImgModule, source: Source): Promise<Image> {
		const bytes = await readSource(source);
		const info = await module.probe(bytes);

		const buffer = module.copyIn(bytes);
		const plan = module.alloc(module.exports.tiny_plan_sizeof());

		try {
			module.check(
				module.exports.tiny_plan_init(plan, buffer, bytes.byteLength),
				'open the image'
			);
		} catch (error) {
			module.free(plan);
			module.free(buffer);
			throw error;
		}

		return new Image(module, plan, buffer, bytes.byteLength, info.format, info.frames);
	}

	/** The container the source arrived in, and the default {@link bytes} encodes back to. */
	get sourceFormat(): ImageFormat | 'unknown' {
		return this.#format;
	}

	/**
	 * Frames the source holds, which is 1 for a still image.
	 *
	 * Only the first is decoded, so a value above 1 means an encode of this plan is a still taken
	 * from an animation. The count comes from the header read {@link open} already performed.
	 */
	get sourceFrames(): number {
		return this.#frames;
	}

	/**
	 * Operations appended so far.
	 *
	 * The count before any rewrite, so it says what was asked for rather than what will run; see
	 * {@link decide} for the latter.
	 */
	get operations(): number {
		this.#alive();
		return this.#module.exports.tiny_plan_count(this.#plan);
	}

	// #region geometry

	/**
	 * Resamples to an extent.
	 *
	 * @param width Target width. Zero, or omitted with a height given, keeps the aspect ratio.
	 * @param height Target height. Same rule.
	 * @param filter Weights to sample through. `auto` is an area average down and a cubic up,
	 * which is the right answer for almost every request.
	 */
	resize(width: number, height = 0, filter: ResampleFilter = 'auto'): this {
		return this.#append(
			filter === 'auto'
				? this.#module.exports.tiny_plan_resize(this.#plan, width, height)
				: this.#module.exports.tiny_plan_resize_with(
						this.#plan,
						width,
						height,
						FILTERS[filter]
					),
			'resize'
		);
	}

	/** Takes a rectangle, in the coordinates the previous operation produced. */
	crop(x: number, y: number, width: number, height: number): this {
		return this.#append(
			this.#module.exports.tiny_plan_crop(this.#plan, x, y, width, height),
			'crop'
		);
	}

	/**
	 * Resamples and then crops or pads to an exact extent.
	 *
	 * @param width Target width.
	 * @param height Target height.
	 * @param options `fit` defaults to `cover`; `gravity` to `center`.
	 */
	fit(width: number, height: number, options: FitOptions & { fit?: Fit } = {}): this {
		if (options.background !== undefined) this.background(options.background);

		return this.#append(
			this.#module.exports.tiny_plan_fit(
				this.#plan,
				width,
				height,
				FITS[options.fit ?? 'cover'],
				GRAVITIES[options.gravity ?? 'center']
			),
			'fit'
		);
	}

	/**
	 * Turns by a multiple of 90 degrees clockwise.
	 *
	 * Folded into the output addressing rather than run as a pass, so any number of turns and flips
	 * cost one pass between them. {@link rotateFree} is the arbitrary-angle form.
	 */
	rotate(degrees: 0 | 90 | 180 | 270): this {
		return this.#append(this.#module.exports.tiny_plan_rotate(this.#plan, degrees), 'rotate');
	}

	/** Mirrors along one axis or both. */
	flip(axis: FlipAxis = 'horizontal'): this {
		if (axis === 'horizontal' || axis === 'both') {
			this.#append(this.#module.exports.tiny_plan_flip_horizontal(this.#plan), 'flip');
		}
		if (axis === 'vertical' || axis === 'both') {
			this.#append(this.#module.exports.tiny_plan_flip_vertical(this.#plan), 'flip');
		}

		return this;
	}

	/**
	 * Turns by an arbitrary angle, filling the corners with the background.
	 *
	 * **Runs after the plan rather than inside it.** An arbitrary rotation changes the extent by a
	 * non-integer factor, so it cannot fold into the sample map the way a quarter turn does; it is
	 * applied to the materialized image. A quarter turn passed here is still handed to
	 * {@link rotate}, which is free.
	 */
	rotateFree(degrees: number): this {
		this.#alive();

		const quarter = ((degrees % 360) + 360) % 360;
		if (quarter % 90 === 0) return this.rotate(quarter as 0 | 90 | 180 | 270);

		const background = this.#background;

		this.#after.push((image) => {
			const color = background ? this.#module.copyIn(background) : 0;

			try {
				this.#module.check(
					this.#module.exports.tiny_image_rotate(image, degrees, color),
					'rotate'
				);
			} finally {
				if (color !== 0) this.#module.free(color);
			}
		});

		return this;
	}

	/**
	 * Trims a uniform border.
	 *
	 * **Runs after the plan rather than inside it.** How much to trim is a function of the pixels,
	 * and the planner decides the decode region and scale before a pixel is read, so a trim cannot
	 * be a plan operation. Chain it before a resize and the resize still happens first.
	 *
	 * @param tolerance How far a pixel may differ from the border color and still be trimmed.
	 */
	trim(tolerance = 8): this {
		this.#alive();

		this.#after.push((image) => {
			this.#module.check(this.#module.exports.tiny_image_trim(image, tolerance), 'trim');
		});

		return this;
	}

	// #endregion

	// #region color

	/** Scales every channel. 1 is unchanged. */
	brightness(factor: number): this {
		return this.#append(
			this.#module.exports.tiny_plan_brightness(this.#plan, factor),
			'brightness'
		);
	}

	/** Scales every channel about mid gray. 1 is unchanged. */
	contrast(factor: number): this {
		return this.#append(
			this.#module.exports.tiny_plan_contrast(this.#plan, factor),
			'contrast'
		);
	}

	/** Moves every channel toward or away from its luminance. 1 is unchanged. */
	saturation(factor: number): this {
		return this.#append(
			this.#module.exports.tiny_plan_saturation(this.#plan, factor),
			'saturation'
		);
	}

	/** Rotates the hue, in degrees. */
	hue(degrees: number): this {
		return this.#append(this.#module.exports.tiny_plan_hue(this.#plan, degrees), 'hue');
	}

	/** Raises every channel to a power. 1 is unchanged. */
	gamma(value: number): this {
		return this.#append(this.#module.exports.tiny_plan_gamma(this.#plan, value), 'gamma');
	}

	/** Replaces every channel with the pixel's luminance, and drops the color channels. */
	grayscale(): this {
		return this.#append(this.#module.exports.tiny_plan_grayscale(this.#plan), 'grayscale');
	}

	/** Subtracts every channel from full scale, leaving alpha alone. */
	invert(): this {
		return this.#append(this.#module.exports.tiny_plan_invert(this.#plan), 'invert');
	}

	/**
	 * Blurs.
	 *
	 * A true gaussian, as three box passes. The planner moves a blur that feeds a reduction of two
	 * or more to after the reduction, at a scaled sigma, which is exact for a gaussian.
	 *
	 * @param sigma Radius in pixels. Zero is a no-op and is eliminated.
	 */
	blur(sigma: number): this {
		return this.#append(
			this.#module.exports.tiny_plan_gaussian_blur(this.#plan, sigma),
			'blur'
		);
	}

	/**
	 * Sharpens with an unsharp mask.
	 *
	 * @param amount How much of the high-frequency difference to add back. 1 is a typical value.
	 * @param sigma Radius of the blur it is measured against.
	 */
	sharpen(amount: number, sigma = 1): this {
		this.#alive();

		const params = this.#module.alloc(16);
		const view = this.#module.view();

		view.setFloat32(params, sigma, true);
		view.setFloat32(params + 4, amount, true);
		view.setFloat32(params + 8, 0, true);
		view.setFloat32(params + 12, 0, true);

		try {
			return this.#append(
				this.#module.exports.tiny_plan_effect(this.#plan, FX_UNSHARP, params),
				'sharpen'
			);
		} finally {
			this.#module.free(params);
		}
	}

	/**
	 * Sets what a pad or an arbitrary rotation fills with.
	 *
	 * Not an operation: it is a property of the plan, so calling it twice replaces rather than
	 * stacking, and calling it after {@link fit} still applies to that fit.
	 */
	background(color: Color): this {
		this.#alive();

		// four channels, which is the most an output can have; the module reads as many as it needs
		this.#background = readColor(color, 4);

		const pointer = this.#module.copyIn(this.#background);

		try {
			this.#module.check(
				this.#module.exports.tiny_plan_background(this.#plan, pointer),
				'set the background'
			);
		} finally {
			this.#module.free(pointer);
		}

		return this;
	}

	/**
	 * Turns operation fusion off, so every operation runs as its own pass.
	 *
	 * **Leave this on.** It exists to measure what the planner is worth: with it off the same chain
	 * costs one traversal and one buffer per operation, which is what the library would be without
	 * the planner. `bench/` is the only caller that should ever pass `false`.
	 *
	 * @param enabled False to run one operation per pass.
	 */
	fusion(enabled: boolean): this {
		return this.#append(
			this.#module.exports.tiny_plan_set_fusion(this.#plan, enabled ? 1 : 0),
			'fusion setting'
		);
	}

	// #endregion

	// #region output

	/**
	 * Reports what the planner decided, without producing the image.
	 *
	 * The saving is visible here: `region` and `scale` are what the decoder will be asked for, and
	 * `eliminated` and `collapsed` are what the rewrites removed. Useful in a log line, and it is
	 * how the library's own tests assert a plan rather than inferring it from the pixels.
	 */
	decide(): PlanDecision {
		this.#alive();

		const module = this.#module;
		const resolution = module.alloc(module.exports.tiny_plan_resolution_sizeof());

		try {
			module.check(
				module.exports.tiny_plan_resolve(this.#plan, resolution),
				'resolve the plan'
			);

			// through the module's own named accessor rather than by computing offsets: the
			// structure holds the operation array in the middle, so every counter after it moves
			// whenever an operand grows
			const field = (name: PlanField) =>
				module.exports.tiny_plan_field(resolution, Field[name]);

			const bits = field('kernels');

			return {
				region: {
					x: field('regionX'),
					y: field('regionY'),
					width: field('regionWidth'),
					height: field('regionHeight')
				},
				scale: field('scale'),
				decoded: { width: field('decodeWidth'), height: field('decodeHeight') },
				output: {
					width: field('width'),
					height: field('height'),
					channels: field('channels')
				},
				operations: field('ops'),
				eliminated: field('eliminated'),
				collapsed: field('collapsed'),
				colorStages: field('colorStages'),
				passes: field('passes'),
				kernels: KERNELS.filter(([bit]) => (bits & bit) !== 0).map(([, name]) => name),
				estimateMs: module.exports.tiny_plan_cost(this.#plan) / 1000
			};
		} finally {
			module.free(resolution);
		}
	}

	/**
	 * Runs the plan and encodes the result.
	 *
	 * The plan is not consumed, so this can be called twice for two formats of one transformation
	 * and the planner's work is done once.
	 *
	 * @param format Container to encode as. Defaults to the source's own.
	 * @param options Encoder settings.
	 * @return The encoded bytes.
	 */
	async bytes(
		format?: ImageFormat,
		options: EncodeOptions = {}
	): Promise<Uint8Array<ArrayBuffer>> {
		this.#alive();

		const target = format ?? this.#format;
		if (target === 'unknown') {
			throw new TypeError('the source format is unknown, so a format has to be given');
		}

		const module = this.#module;
		const writer = module.alloc(module.exports.tiny_writer_sizeof());
		const opts = module.alloc(5);

		const view = module.view();
		view.setUint8(opts, options.quality ?? 0);
		view.setUint8(opts + 1, options.lossless ? 1 : 0);
		view.setUint8(opts + 2, options.progressive ? 1 : 0);
		view.setUint8(opts + 3, options.stripMetadata ? 1 : 0);
		view.setUint8(opts + 4, options.effort === 'fast' ? 1 : 0);

		try {
			module.check(module.exports.tiny_writer_init(writer, 0), 'prepare the encoder');

			try {
				if (this.#after.length === 0) {
					module.check(
						module.exports.tiny_plan_encode(
							this.#plan,
							module.formatId(target),
							opts,
							writer
						),
						'run and encode'
					);
				} else {
					this.#runWithAfter(target, opts, writer);
				}

				return module.copyOut(
					module.exports.tiny_writer_data(writer),
					module.exports.tiny_writer_size(writer)
				);
			} finally {
				module.exports.tiny_writer_free(writer);
			}
		} finally {
			module.free(opts);
			module.free(writer);
		}
	}

	/**
	 * Runs the plan and hands back the raw samples.
	 *
	 * Below the encoders, for a caller who wants the pixels themselves: a histogram, a hand-written
	 * kernel, or a measurement that should not include an encoder's time. Rows are tightly packed
	 * and there is no stride separate from `width * channels`.
	 *
	 * @return The extent, the channel count and `width * height * channels` bytes.
	 */
	async pixels(): Promise<RawImage> {
		this.#alive();

		const module = this.#module;
		const image = module.alloc(module.exports.tiny_image_sizeof());

		try {
			module.check(module.exports.tiny_plan_run(this.#plan, image), 'run the plan');

			try {
				for (const step of this.#after) step(image);

				return {
					width: module.exports.tiny_image_getwidth(image),
					height: module.exports.tiny_image_getheight(image),
					channels: module.exports.tiny_image_getchannels(image),
					pixels: module.copyOut(
						module.exports.tiny_image_getdata(image),
						module.exports.tiny_image_getsize(image)
					)
				};
			} finally {
				module.exports.tiny_image_destroy(image);
			}
		} finally {
			module.free(image);
		}
	}

	/**
	 * The result as a `Blob`, ready to hand to a `FormData` or a `Response`.
	 *
	 * @param format Container to encode as.
	 * @param options Encoder settings.
	 */
	async blob(format?: ImageFormat, options: EncodeOptions = {}): Promise<Blob> {
		const target = format ?? this.#format;
		const bytes = await this.bytes(format, options);

		return new Blob([bytes], { type: mimeFor(target) });
	}

	/**
	 * The result as a `Response`, which is what a Worker returns.
	 *
	 * @param format Container to encode as.
	 * @param options Encoder settings, plus `headers` merged into the response's own.
	 */
	async response(
		format?: ImageFormat,
		options: EncodeOptions & { headers?: HeadersInit } = {}
	): Promise<Response> {
		const target = format ?? this.#format;
		const bytes = await this.bytes(format, options);
		const headers = new Headers(options.headers);

		headers.set('content-type', mimeFor(target));
		headers.set('content-length', String(bytes.byteLength));

		return new Response(bytes, { headers });
	}

	/**
	 * The result as a `data:` URL.
	 *
	 * For an inline `<img src>` or a placeholder. Base64 costs a third more bytes than the image,
	 * so this is for small results.
	 *
	 * @param format Container to encode as.
	 * @param options Encoder settings.
	 */
	async dataUrl(format?: ImageFormat, options: EncodeOptions = {}): Promise<string> {
		const target = format ?? this.#format;
		const bytes = await this.bytes(format, options);

		let binary = '';
		for (const byte of bytes) binary += String.fromCharCode(byte);

		return `data:${mimeFor(target)};base64,${btoa(binary)}`;
	}

	// #endregion

	// #region lifetime

	/**
	 * Releases the plan and the source bytes.
	 *
	 * Safe to call twice. Every later call on the image throws instead of reading freed memory.
	 */
	dispose(): void {
		if (this.#disposed) return;

		this.#disposed = true;
		this.#module.free(this.#plan);
		this.#module.free(this.#buffer);
	}

	/** Lets `using` release the handle; see {@link dispose}. */
	[Symbol.dispose](): void {
		this.dispose();
	}

	// #endregion

	// #region internals

	#alive(): void {
		if (this.#disposed) throw new TypeError('this image has been disposed');
	}

	#append(result: number, what: string): this {
		this.#alive();
		this.#module.check(result, `append a ${what}`);

		return this;
	}

	/** Runs the plan, applies the operations that cannot be plan operations, then encodes. */
	#runWithAfter(target: ImageFormat, opts: number, writer: number): void {
		const module = this.#module;
		const image = module.alloc(module.exports.tiny_image_sizeof());

		try {
			module.check(module.exports.tiny_plan_run(this.#plan, image), 'run the plan');

			try {
				for (const step of this.#after) step(image);

				module.check(
					module.exports.tiny_image_encode(image, module.formatId(target), opts, writer),
					'encode'
				);
			} finally {
				module.exports.tiny_image_destroy(image);
			}
		} finally {
			module.free(image);
		}
	}

	// #endregion
}

/** The content type a container is served as. */
export function mimeFor(format: ImageFormat | 'unknown'): string {
	switch (format) {
		case 'png':
			return 'image/png';
		case 'jpeg':
			return 'image/jpeg';
		case 'bmp':
			return 'image/bmp';
		case 'gif':
			return 'image/gif';
		case 'tiff':
			return 'image/tiff';
		case 'webp':
			return 'image/webp';
		case 'avif':
			return 'image/avif';
		case 'heif':
			return 'image/heif';
		default:
			return 'application/octet-stream';
	}
}
