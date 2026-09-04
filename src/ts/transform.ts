import { Image, mimeFor, type EncodeOptions } from './image.js';
import { readSource, type ImageFormat, type Source, type TransformOptions } from './types.js';
import type { FeatureName, TinyImgModule } from './wasm.js';

/**
 * The result of a one-shot transformation.
 *
 * Carries the bytes and the conveniences a Worker actually wants, so nobody has to build a
 * `Response` or a `data:` URL by hand.
 */
export interface TransformResult {
	/** The encoded image. */
	readonly data: Uint8Array<ArrayBuffer>;
	/** What it was encoded as. */
	readonly format: ImageFormat;
	/** Its content type. */
	readonly contentType: string;
	/** Width of the output in pixels. */
	readonly width: number;
	/** Height of the output in pixels. */
	readonly height: number;
	/**
	 * Set when the source held several frames and this output holds only the first.
	 *
	 * There is no animated encoder, so any request that actually has work to do reduces an
	 * animation to a still. That is a lossy outcome a caller may want to avoid rather than
	 * discover, by serving the original or by asking for a request that passes through.
	 */
	readonly flattened: boolean;

	/** The encoded bytes. */
	bytes(): Uint8Array<ArrayBuffer>;
	/** The bytes as a `Blob`, typed. */
	blob(): Blob;
	/** The bytes as a `Response`, typed and length-set. */
	response(headers?: HeadersInit): Response;
	/** The bytes as a `data:` URL. */
	dataUrl(): string;
}

/**
 * The order the options apply in.
 *
 * An option object has no order of its own, so this is fixed and documented rather than left to
 * whatever `Object.keys` happens to return: two callers writing the same keys have to get the same
 * image. Geometry first, because everything after it then works on fewer pixels; color next,
 * because it collapses into one pass; the neighborhood operations last, because they cannot.
 */
const ORDER = [
	'crop',
	'resize or fit',
	'rotate',
	'flip',
	'brightness',
	'contrast',
	'saturation',
	'hue',
	'gamma',
	'grayscale',
	'invert',
	'blur',
	'sharpen',
	'trim'
] as const;

/**
 * Applies an option object to an image, in {@link ORDER}.
 *
 * Split out from {@link transform} so {@link Image} is still the only thing that knows how to talk
 * to the module, and so a caller holding an `Image` can apply an option object to it mid-chain.
 *
 * @param image The image to append to.
 * @param options What to apply.
 * @return The same image, for chaining.
 */
export function apply(image: Image, options: TransformOptions): Image {
	if (options.background !== undefined) image.background(options.background);

	if (options.crop) {
		image.crop(options.crop.x, options.crop.y, options.crop.width, options.crop.height);
	}

	const dpr = options.dpr ?? 1;
	const width = options.width === undefined ? undefined : Math.round(options.width * dpr);
	const height = options.height === undefined ? undefined : Math.round(options.height * dpr);

	if (width !== undefined && height !== undefined) {
		// both extents given, so the aspect mismatch has to be absorbed somehow and `fit` is the
		// operation that says how. one extent alone has no mismatch to absorb
		image.fit(width, height, {
			fit: options.fit ?? 'cover',
			...(options.gravity === undefined ? {} : { gravity: options.gravity })
		});
	} else if (width !== undefined || height !== undefined) {
		image.resize(width ?? 0, height ?? 0, options.filter ?? 'auto');
	}

	if (options.rotate) image.rotate(options.rotate);
	if (options.flip) image.flip(options.flip);

	if (options.brightness !== undefined) image.brightness(options.brightness);
	if (options.contrast !== undefined) image.contrast(options.contrast);
	if (options.saturation !== undefined) image.saturation(options.saturation);
	if (options.hue !== undefined) image.hue(options.hue);
	if (options.gamma !== undefined) image.gamma(options.gamma);
	if (options.grayscale) image.grayscale();
	if (options.invert) image.invert();

	if (options.blur !== undefined) image.blur(options.blur);
	if (options.sharpen !== undefined) image.sharpen(options.sharpen);

	if (options.trim) image.trim(options.trim === true ? 8 : options.trim);

	return image;
}

/**
 * Transforms an image in one call.
 *
 * The entry point to reach for. It builds a plan from the options, lets the planner pull only the
 * source it needs, and hands back the encoded result:
 *
 * ```ts
 * import wasm from '@gmitch215/tinyimg/tinyimg.wasm';
 * import { TinyImgModule, transform } from '@gmitch215/tinyimg';
 *
 * const tinyimg = TinyImgModule.load(wasm);
 *
 * export default {
 *   async fetch(request: Request): Promise<Response> {
 *     const source = await fetch('https://example.com/photo.jpg');
 *     const result = await transform(tinyimg, source, {
 *       width: 800,
 *       height: 600,
 *       fit: 'cover',
 *       format: 'webp',
 *       quality: 80
 *     });
 *
 *     return result.response({ 'cache-control': 'public, max-age=86400' });
 *   }
 * };
 * ```
 *
 * The handle is released before this returns, so there is nothing to dispose. Use {@link Image}
 * directly when you want two encodings of one transformation, or an operation this option object
 * does not cover.
 *
 * @param module The loaded module.
 * @param source The encoded image, in any shape {@link Source} accepts.
 * @param options What to do to it. An empty object decodes and re-encodes.
 * @return The encoded result and its conveniences.
 */
export async function transform(
	module: TinyImgModule,
	source: Source,
	options: TransformOptions = {}
): Promise<TransformResult> {
	// read once, because a Response or a stream cannot be read twice and the pass-through check
	// below needs the bytes it would otherwise hand straight back
	const bytes = await readSource(source);
	const passed = await passThrough(module, bytes, options);

	if (passed) return passed;

	const image = await Image.open(module, bytes);

	try {
		apply(image, options);

		const decided = image.decide();
		const format = chooseFormat(module, image, options, decided);

		const encode: EncodeOptions = {
			...(options.quality === undefined ? {} : { quality: options.quality }),
			...(options.lossless === undefined ? {} : { lossless: options.lossless }),
			...(options.progressive === undefined ? {} : { progressive: options.progressive }),
			...(options.metadata === undefined
				? {}
				: { stripMetadata: options.metadata === 'none' }),
			...(options.effort === undefined ? {} : { effort: options.effort })
		};

		const data = await image.bytes(format, encode);

		return result(
			data,
			format,
			decided.output.width,
			decided.output.height,
			image.sourceFrames > 1
		);
	} finally {
		image.dispose();
	}
}

/**
 * A stable key for the artifact a request would produce.
 *
 * The point of a cache in front of this library is that the expensive path runs once. That only
 * works if the key is canonical: two requests that mean the same thing have to produce the same
 * string, and two that mean different things must not. Option order, absent options and the source
 * itself all have to be normalized, which is easy to get subtly wrong by hand and is why this is
 * here rather than in a README snippet.
 *
 * The source is identified by the SHA-256 of its bytes rather than by a URL, so the same picture
 * served from two places is one artifact and a picture that changed under a stable URL is not
 * mistaken for the old one.
 *
 * The result is shaped as a URL because that is what the Workers Cache API takes as a key:
 *
 * ```ts
 * const key = await artifactKey(source, options);
 * const hit = await caches.default.match(key);
 *
 * if (hit) return hit;
 *
 * const out = await transform(tinyimg, source, options);
 * const response = out.response({ 'cache-control': 'public, max-age=31536000, immutable' });
 *
 * ctx.waitUntil(caches.default.put(key, response.clone()));
 * return response;
 * ```
 *
 * A cache hit costs no CPU at all, which is the only way a request the budget cannot afford still
 * gets served: it is paid for once.
 *
 * @param source The image the request is about, in any shape {@link Source} accepts.
 * @param options The request. `budgetMs` and `effort` are included, because both can change the
 * bytes that come out.
 * @param origin Prefix for the returned URL. Anything stable; it never leaves the cache.
 * @return A URL-shaped key.
 */
export async function artifactKey(
	source: Source,
	options: TransformOptions = {},
	origin = 'https://tinyimg.invalid/'
): Promise<string> {
	const bytes = await readSource(source);
	const digest = await crypto.subtle.digest('SHA-256', bytes.slice().buffer);
	const hex = Array.from(new Uint8Array(digest), (byte) =>
		byte.toString(16).padStart(2, '0')
	).join('');

	// sorted, and absent options omitted rather than written as their default, so a request that
	// spells a default out loud lands on the same artifact as one that leaves it alone
	const query = Object.keys(options)
		.sort()
		.flatMap((key) => {
			const value = options[key as keyof TransformOptions];
			if (value === undefined) return [];

			const text =
				typeof value === 'object' && value !== null
					? JSON.stringify(
							Object.fromEntries(
								Object.entries(value).sort(([a], [b]) => (a < b ? -1 : 1))
							)
						)
					: String(value);

			return [`${encodeURIComponent(key)}=${encodeURIComponent(text)}`];
		})
		.join('&');

	return query ? `${origin}${hex}?${query}` : `${origin}${hex}`;
}

/**
 * Every option that changes a pixel or a byte.
 *
 * Listed rather than inferred, so adding an option to {@link TransformOptions} without deciding
 * whether it defeats a pass-through is a compile error rather than a wrong answer served fast.
 *
 * `effort` is deliberately absent: it says how hard to work, not what to produce, and handing back
 * the source is the least work available. `dpr` is present although on its own it does nothing,
 * because it only means anything beside an extent and treating its presence as intent to scale
 * costs one missed pass-through rather than needing an argument about interactions.
 */
const CHANGES_OUTPUT = [
	'crop',
	'rotate',
	'flip',
	'brightness',
	'contrast',
	'saturation',
	'hue',
	'gamma',
	'grayscale',
	'invert',
	'blur',
	'sharpen',
	'trim',
	'background',
	'quality',
	'lossless',
	'progressive',
	'metadata',
	'dpr'
] as const satisfies readonly (keyof TransformOptions)[];

/**
 * Hands back the source unchanged when the request does not ask for anything.
 *
 * The cheapest transformation is the one that does not happen. A request for a WebP at 800 wide
 * against a WebP that is already 700 wide has nothing to do: it decodes 1.9 megapixels and encodes
 * them again to produce bytes it was given. This reads the header, which decodes no pixels, and
 * returns the original buffer.
 *
 * An upper bound rather than an exact match on the extent, because `width` alone means "no wider
 * than", so a source already inside it is already the answer. Enlarging is not identity and is not
 * treated as one.
 *
 * This matters most for an animation, which is why the frame count is not checked here. Decoding a
 * 57-frame GIF yields its first frame, so a request with nothing to do that went down the decode
 * path would spend real time turning an animation into a still. Handing the source back is both the
 * cheaper answer and the only one that keeps the other 56 frames.
 *
 * @returns The original bytes wrapped as a result, or undefined when there is real work to do.
 */
async function passThrough(
	module: TinyImgModule,
	bytes: Uint8Array,
	options: TransformOptions
): Promise<TransformResult | undefined> {
	for (const key of CHANGES_OUTPUT) {
		if (options[key] !== undefined) return undefined;
	}

	let info;

	try {
		info = await module.probe(bytes);
	} catch {
		// an unreadable header is not a pass-through; let the decode report it properly
		return undefined;
	}

	if (info.format === 'unknown') return undefined;

	const asked = options.format;
	const wanted = asked === 'auto' ? (module.has('webp') ? 'webp' : 'jpeg') : asked;

	if (wanted !== undefined && wanted !== info.format) return undefined;

	const width = options.width;
	const height = options.height;

	if (width !== undefined && width < info.width) return undefined;
	if (height !== undefined && height < info.height) return undefined;

	// `fit` with both extents pads or crops to reach them exactly, so it is never a pass-through
	if (width !== undefined && height !== undefined) return undefined;

	// zero copy in the ordinary case; a shared buffer cannot back a Blob, so that one is copied
	const view: Uint8Array<ArrayBuffer> =
		bytes.buffer instanceof ArrayBuffer
			? (bytes as Uint8Array<ArrayBuffer>)
			: new Uint8Array(bytes);

	return result(view, info.format, info.width, info.height);
}

/** Container ids as `TinyImageFormat` numbers them, for the cost estimate. */
const FORMAT_ID: Record<ImageFormat, number> = {
	png: 1,
	jpeg: 2,
	bmp: 3,
	gif: 4,
	tiff: 5,
	webp: 6,
	avif: 7,
	heif: 8
};

/**
 * The formats `format: 'auto'` will consider, cheapest encoder last so a tie keeps the better one.
 *
 * WebP first because it is the better artifact: 25 to 35% below JPEG at matched quality with 97%
 * browser support. JPEG is the fallback because its encoder costs about a fifth as much per sample,
 * which is what makes the choice worth making at all.
 */
const AUTO_ORDER = ['webp', 'jpeg'] as const satisfies readonly (ImageFormat & FeatureName)[];

/**
 * Picks the output format, honouring a CPU budget when one is given.
 *
 * `format: 'auto'` with no budget is WebP, which is the better artifact. With a budget it becomes a
 * choice: the plan's own estimate is fixed by the time this runs, so the only variable left is the
 * encoder, and the encoders differ by about a factor of five per sample. A request that does not fit
 * as WebP may fit as JPEG at a larger file, and on a plan where the CPU limit is hard and bandwidth
 * is not billed that is usually the trade a caller wants.
 *
 * No `format` at all stays the source's own, which is the least surprising answer and is not a
 * choice this makes.
 *
 * Nothing here silently degrades quality. It changes container, and the result says which one it
 * chose.
 */
function chooseFormat(
	module: TinyImgModule,
	image: Image,
	options: TransformOptions,
	decided: { output: { width: number; height: number }; estimateMs: number }
): ImageFormat {
	const asked = options.format;

	if (asked !== undefined && asked !== 'auto') return asked;

	const budget = options.budgetMs;
	const { width, height } = decided.output;

	const cost = (format: (typeof AUTO_ORDER)[number]) =>
		decided.estimateMs +
		module.exports.tiny_encode_cost(FORMAT_ID[format], width, height) / 1000;

	if (asked === 'auto') {
		if (budget === undefined) {
			// no budget to satisfy, so the better artifact wins outright
			return module.has('webp') ? 'webp' : 'jpeg';
		}

		const affordable = AUTO_ORDER.filter(
			(format) => module.has(format) && cost(format) <= budget
		);

		// nothing fits, so the cheapest candidate is the closest a caller can get
		return affordable[0] ?? 'jpeg';
	}

	const source = image.sourceFormat;
	if (source === 'unknown') {
		throw new TypeError('the source format is unknown, so `format` has to be given');
	}

	// an explicit format is never substituted, even over budget: the caller named it
	return source;
}

/** Wraps encoded bytes in the conveniences. */
function result(
	data: Uint8Array<ArrayBuffer>,
	format: ImageFormat,
	width: number,
	height: number,
	flattened = false
): TransformResult {
	const contentType = mimeFor(format);

	return {
		data,
		format,
		contentType,
		width,
		height,
		flattened,
		bytes: () => data,
		blob: () => new Blob([data], { type: contentType }),
		response: (headers?: HeadersInit) => {
			const merged = new Headers(headers);
			merged.set('content-type', contentType);
			merged.set('content-length', String(data.byteLength));

			return new Response(data, { headers: merged });
		},
		dataUrl: () => {
			let binary = '';
			for (const byte of data) binary += String.fromCharCode(byte);

			return `data:${contentType};base64,${btoa(binary)}`;
		}
	};
}

export { ORDER as TRANSFORM_ORDER };
