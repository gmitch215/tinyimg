/**
 * The option and error vocabulary the wrapper speaks.
 *
 * The names mirror Cloudflare Images' own transformation parameters wherever there is one to mirror,
 * because the point of the library is to do that work inside the Worker instead of paying for it:
 * a caller moving off the binding should be able to keep the option object they already have.
 */

/** A container format, mirroring `TinyImageFormat`. */
export type ImageFormat = 'png' | 'jpeg' | 'bmp' | 'gif' | 'tiff' | 'webp' | 'avif' | 'heif';

/**
 * How an aspect mismatch is absorbed, mirroring `TinyImageFit`.
 *
 * The eleven are two independent choices read as one: whether the mismatch is left, cropped, padded
 * or distorted, and whether the scale may rise, fall, both or neither.
 */
export type Fit =
	| 'scale-down'
	| 'contain'
	| 'cover'
	| 'crop'
	| 'aspect-crop'
	| 'aspect-contain'
	| 'aspect-cover'
	| 'pad'
	| 'stretch'
	| 'fill'
	| 'scale-up';

/**
 * Which part of an image a crop keeps, mirroring `TinyImageGravity`.
 *
 * `auto` weights the image by local detail and centers on the result. `face` runs the detector and
 * centers on what it finds, falling back to `auto` when no cascade is loaded or nothing is found;
 * see {@link TinyImgModule.loadBlob}.
 */
export type Gravity =
	| 'center'
	| 'north'
	| 'south'
	| 'west'
	| 'east'
	| 'north-west'
	| 'north-east'
	| 'south-west'
	| 'south-east'
	| 'auto'
	| 'face';

/** Which weights a resample reads through, mirroring `TinyResampleFilter`. */
export type ResampleFilter = 'auto' | 'nearest' | 'bilinear' | 'box' | 'catmull-rom';

/** Which axes a flip mirrors. */
export type FlipAxis = 'horizontal' | 'vertical' | 'both';

/** What to do with metadata on encode. */
export type MetadataPolicy = 'keep' | 'none';

/**
 * A color.
 *
 * Either CSS-style hex (`'#rgb'`, `'#rrggbb'`, `'#rrggbbaa'`, with or without the hash) or channel
 * values as numbers. A three number array is RGB and a four is RGBA; a single number is gray.
 */
export type Color = string | readonly number[];

/**
 * Everything a one-shot transformation can ask for.
 *
 * Every field is optional and an empty object is a valid request: it decodes and re-encodes. The
 * order the operations run in is fixed and documented on {@link transform}, because an option
 * object has no order of its own and two callers writing the same keys have to get the same image.
 */
export interface TransformOptions {
	/** Target width in pixels. With `height` unset, the aspect ratio is kept. */
	width?: number;
	/** Target height in pixels. With `width` unset, the aspect ratio is kept. */
	height?: number;
	/** How an aspect mismatch is absorbed. Only read when both `width` and `height` are set. */
	fit?: Fit;
	/** Which part a crop keeps, or where a pad puts the image. */
	gravity?: Gravity;
	/** Weights the resample reads through. `auto` is an area average down and a cubic up. */
	filter?: ResampleFilter;

	/** Source rectangle, taken before anything else. */
	crop?: Rect;
	/** Multiplies `width` and `height`, for serving a retina variant of one request. */
	dpr?: number;

	/** Quarter turns clockwise. Anything else is rejected; use {@link Image.rotateFree}. */
	rotate?: 0 | 90 | 180 | 270;
	/** Which axes to mirror. */
	flip?: FlipAxis;

	/** Scales every channel. 1 is unchanged. */
	brightness?: number;
	/** Scales every channel about mid gray. 1 is unchanged. */
	contrast?: number;
	/** Moves every channel toward or away from its luminance. 1 is unchanged. */
	saturation?: number;
	/** Rotates the hue, in degrees. */
	hue?: number;
	/** Raises every channel to a power. 1 is unchanged. */
	gamma?: number;
	/** Replaces every channel with the pixel's luminance. */
	grayscale?: boolean;
	/** Subtracts every channel from full scale, leaving alpha alone. */
	invert?: boolean;

	/** Gaussian blur radius in pixels. */
	blur?: number;
	/** Unsharp mask amount. */
	sharpen?: number;

	/** Trims a uniform border, at this tolerance. `true` means a tolerance of 8. */
	trim?: boolean | number;
	/** Fills whatever a pad or a rotation leaves empty. */
	background?: Color;

	/**
	 * Container to encode as. Defaults to the source's own format.
	 *
	 * `'auto'` picks one. On its own that is WebP, which is the better artifact. With
	 * {@link TransformOptions.budgetMs} it becomes the best format that fits the budget, which
	 * matters because the encoders differ by about a factor of five per sample.
	 */
	format?: ImageFormat | 'auto';
	/**
	 * How much computation the encoder may spend. `'fancy'` is the default.
	 *
	 * A separate axis from {@link TransformOptions.quality}: quality says what the output should
	 * look like, this says how hard to work to get there. See the `effort` option on
	 * `Image.bytes` for what each format gives up and what it was measured to cost.
	 */
	effort?: 'fancy' | 'fast';
	/**
	 * A CPU budget in milliseconds, which changes what `format: 'auto'` chooses.
	 *
	 * The Workers Free plan allows 10 milliseconds of CPU per request and it is not configurable,
	 * so a request either fits or fails; there is no paying slightly more. Give a budget below the
	 * limit rather than at it, since the estimate behind the choice is accurate to about 20%.
	 *
	 * An explicitly named `format` is never substituted, however far over budget it is: naming one
	 * is a decision, and this option does not overrule it.
	 */
	budgetMs?: number;
	/** Quality for a lossy format, 1 through 100. */
	quality?: number;
	/** Use a format's lossless mode where it has one. */
	lossless?: boolean;
	/** Write a progressive or interlaced stream. */
	progressive?: boolean;
	/** Whether to carry EXIF and other metadata into the output. Defaults to `keep`. */
	metadata?: MetadataPolicy;
}

/** A rectangle in pixels. */
export interface Rect {
	/** Left edge. */
	x: number;
	/** Top edge. */
	y: number;
	/** Width. */
	width: number;
	/** Height. */
	height: number;
}

/** An extent in pixels. */
export interface Extent {
	/** Width. */
	width: number;
	/** Height. */
	height: number;
}

/** Kinds of data the library reads but does not ship, mirroring `TinyBlobKind`. */
export type BlobKind = 'font' | 'icc' | 'cascade';

/** What a decoded image is, without its pixels. */
export interface ImageInfo {
	/** Width in pixels. */
	width: number;
	/** Height in pixels. */
	height: number;
	/** Frames the file holds; 1 for a still image. */
	frames: number;
	/** The format the magic bytes identified. */
	format: ImageFormat | 'unknown';
	/** Channels a full decode would produce. */
	channels: number;
	/** Bits per channel in the file, before decoding normalizes them to 8. */
	bitDepth: number;
	/** Whether the file carries transparency. */
	hasAlpha: boolean;
	/** Whether this is a progressive JPEG, which cannot stream a region. */
	progressive: boolean;
}

/** Decoded samples, with the extent they describe. */
export interface RawImage {
	/** Width in pixels. */
	width: number;
	/** Height in pixels. */
	height: number;
	/** Bytes per pixel, 1 through 4. */
	channels: number;
	/** `width * height * channels` bytes, rows tightly packed. */
	pixels: Uint8Array<ArrayBuffer>;
}

/** One detection, in the coordinates of the image that was searched. */
export interface FaceBox extends Rect {
	/**
	 * Overlapping raw detections this box was formed from.
	 *
	 * A usable ranking: a face the cascade is confident about fires at several nearby positions and
	 * scales, so the highest is the most likely to be a face.
	 */
	neighbors: number;
}

/** What the planner decided, before any pixel was touched. */
export interface PlanDecision {
	/** The region of the source the decoder was asked for. */
	region: Rect;
	/** The subsampling denominator the decoder was asked for: 1, 2, 4 or 8. */
	scale: number;
	/** Extent the decode produced. */
	decoded: Extent;
	/** Extent of the final image, and how many channels it carries. */
	output: Extent & {
		/** Channels per pixel, 1 through 4. */
		channels: number;
	};
	/** Operations left after the rewrites. */
	operations: number;
	/** Operations an identity or annihilation rule removed. */
	eliminated: number;
	/** Operations a pair rule merged into another. */
	collapsed: number;
	/** Color stages the operations collapsed into. */
	colorStages: number;
	/** Fused passes the run will make. */
	passes: number;
	/** Special cases the planner took, by name. */
	kernels: string[];
	/**
	 * What running this plan is expected to cost in milliseconds, before any encoder.
	 *
	 * An estimate and not a promise: the rates behind it were measured on one machine, and against
	 * real transforms it lands within about 20%. That is the accuracy the question needs when the
	 * question is whether work will fit inside a CPU limit, and it is not a substitute for
	 * measuring, which {@link TinyImgModule.measure} does.
	 *
	 * 0 when the source header could not be read.
	 */
	estimateMs: number;
}

/**
 * A failed call.
 *
 * Every failure carries the module's own error code, so a caller can branch on the code rather than
 * on a message, and the subclasses group the codes a caller is likely to treat the same way.
 */
export class TinyImgError extends Error {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name: string = 'TinyImgError';

	/** The negative `TinyImageError` the module returned. */
	readonly code: number;

	/** The short, stable name the module carries for that code. */
	readonly codeName: string;

	constructor(code: number, codeName: string, context?: string) {
		super(context ? `${context}: ${codeName}` : codeName);
		this.code = code;
		this.codeName = codeName;
	}
}

/** An argument was NULL, outside its range, or a rectangle fell outside the image. */
export class TinyImgArgumentError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgArgumentError';
}

/** The bytes are not a format this build can read, or not a variant of it that it can. */
export class TinyImgFormatError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgFormatError';
}

/** The bitstream is malformed, truncated or internally inconsistent. */
export class TinyImgDataError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgDataError';
}

/**
 * The allocator could not satisfy a request, or the image is past the budget.
 *
 * The two are separate codes because the remedy differs, so the budget one is
 * {@link TinyImgTooLargeError}, a subclass. Catching this catches both.
 */
export class TinyImgMemoryError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name: string = 'TinyImgMemoryError';
}

/**
 * The image is past the pixel or byte budget, so no amount of memory would help.
 *
 * The remedy is a smaller decode rather than a bigger heap: ask for a region, a scale denominator,
 * or a transformation whose output fits. A plain {@link TinyImgMemoryError} means the allocator ran
 * out on a request that was within budget, which is a different problem.
 */
export class TinyImgTooLargeError extends TinyImgMemoryError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgTooLargeError';
}

/** The operation needs a blob nobody has loaded; see {@link TinyImgModule.loadBlob}. */
export class TinyImgBlobError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgBlobError';
}

/** The plan is full, or holds an operation that cannot be combined with the others in it. */
export class TinyImgPlanError extends TinyImgError {
	/** The error's class, for a caller matching on it without instanceof. */
	override readonly name = 'TinyImgPlanError';
}

/** Error codes, mirroring `enum TinyImageError`. */
export const Err = {
	/** The call succeeded. */
	ok: 0,
	/** A required pointer argument was NULL. */
	null: -1,
	/** An argument was outside its documented range. */
	range: -2,
	/** Coordinates or a rectangle fell outside the image. */
	bounds: -3,
	/** The allocator could not satisfy a request. */
	memory: -4,
	/** The image exceeds the pixel budget; reach for a scaled decode. */
	tooLarge: -5,
	/** The buffer matched no format tinyimg recognizes. */
	unknownFormat: -6,
	/** Recognized, and this build cannot decode or encode it. */
	unsupportedCodec: -7,
	/** The bitstream is malformed, truncated or inconsistent. */
	corrupt: -8,
	/** Supported format, unsupported variant of it. */
	unsupportedVariant: -9,
	/** The output buffer was too small. */
	bufferTooSmall: -10,
	/** A font, profile or cascade nobody has loaded. */
	blobMissing: -11,
	/** The image has no channel the operation needs. */
	noChannel: -12,
	/** The requested key or metadata entry does not exist. */
	notFound: -13,
	/** The plan is full, or holds an operation it cannot combine. */
	plan: -14
} as const;

/**
 * Builds the error for a code.
 *
 * The mapping lives here rather than beside the module so the classes and the codes they cover stay
 * in one place; `codeName` comes from the module, which is the only thing that knows it.
 *
 * @param code The negative code the module returned.
 * @param codeName What the module calls it.
 * @param context What was being attempted, for the message.
 * @return The error to throw. Never returns for a code of zero, which is not a failure.
 */
export function errorFor(code: number, codeName: string, context?: string): TinyImgError {
	switch (code) {
		case Err.null:
		case Err.range:
		case Err.bounds:
		case Err.noChannel:
		case Err.bufferTooSmall:
			return new TinyImgArgumentError(code, codeName, context);
		case Err.unknownFormat:
		case Err.unsupportedCodec:
		case Err.unsupportedVariant:
			return new TinyImgFormatError(code, codeName, context);
		case Err.corrupt:
			return new TinyImgDataError(code, codeName, context);
		case Err.memory:
			return new TinyImgMemoryError(code, codeName, context);
		case Err.tooLarge:
			return new TinyImgTooLargeError(code, codeName, context);
		case Err.blobMissing:
			return new TinyImgBlobError(code, codeName, context);
		case Err.plan:
			return new TinyImgPlanError(code, codeName, context);
		default:
			return new TinyImgError(code, codeName, context);
	}
}

/** Anything the wrapper will take as an image or a blob. */
export type Source =
	Uint8Array | ArrayBuffer | ArrayBufferView | Blob | Response | ReadableStream<Uint8Array>;

/**
 * Reads any accepted source into bytes.
 *
 * A caller should never have to reach for a `Response.arrayBuffer()` or drain a stream themselves to
 * hand this library an image; every shape a Worker is likely to be holding one in is accepted.
 *
 * @param source The image or blob.
 * @return Its bytes.
 */
export async function readSource(source: Source): Promise<Uint8Array> {
	if (source instanceof Uint8Array) return source;
	if (source instanceof ArrayBuffer) return new Uint8Array(source);

	if (source instanceof Response) {
		if (!source.body) throw new TypeError('the Response has no body');
		return new Uint8Array(await source.arrayBuffer());
	}

	if (typeof Blob !== 'undefined' && source instanceof Blob) {
		return new Uint8Array(await source.arrayBuffer());
	}

	if (source instanceof ReadableStream) {
		return new Uint8Array(await new Response(source).arrayBuffer());
	}

	if (ArrayBuffer.isView(source)) {
		return new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
	}

	throw new TypeError('expected bytes, an ArrayBuffer, a Blob, a Response or a ReadableStream');
}

/**
 * Turns a color into channel bytes.
 *
 * @param color The color.
 * @param channels How many the image has.
 * @return Exactly `channels` bytes.
 */
export function readColor(color: Color, channels: number): Uint8Array {
	const out = new Uint8Array(channels);

	if (typeof color !== 'string') {
		// a single number is gray, three are RGB, four are RGBA; a shorter array than the image
		// needs repeats its last value rather than leaving the rest black
		const values = [...color];
		if (values.length === 0) throw new TypeError('the color has no channels');

		for (let i = 0; i < channels; i++) {
			const value = values[Math.min(i, values.length - 1)]!;
			out[i] = Math.max(0, Math.min(255, Math.round(value)));
		}

		// an image with alpha and a color without one is opaque, not transparent
		if ((channels === 2 || channels === 4) && values.length < channels) {
			out[channels - 1] = 255;
		}

		return out;
	}

	const hex = color.startsWith('#') ? color.slice(1) : color;
	const wide = hex.length <= 4 ? [...hex].map((digit) => digit + digit).join('') : hex;

	if (!/^[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(wide)) {
		throw new TypeError(`'${color}' is not a hex color`);
	}

	const rgba = [
		parseInt(wide.slice(0, 2), 16),
		parseInt(wide.slice(2, 4), 16),
		parseInt(wide.slice(4, 6), 16),
		wide.length === 8 ? parseInt(wide.slice(6, 8), 16) : 255
	];

	if (channels >= 3) {
		for (let i = 0; i < channels; i++) out[i] = rgba[i === 3 ? 3 : i]!;
		return out;
	}

	// a gray image takes the color's luminance, so a caller can pass one color whatever the
	// channel count turns out to be
	out[0] = Math.round(0.2126 * rgba[0]! + 0.7152 * rgba[1]! + 0.0722 * rgba[2]!);
	if (channels === 2) out[1] = rgba[3]!;

	return out;
}
