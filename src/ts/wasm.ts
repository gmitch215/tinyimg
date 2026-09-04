import {
	Err,
	errorFor,
	readSource,
	type BlobKind,
	type FaceBox,
	type ImageFormat,
	type ImageInfo,
	type RawImage,
	type Source
} from './types.js';

/**
 * The ABI version this wrapper understands.
 *
 * Bumped in lockstep with `TINYIMG_ABI_VERSION` in `include/tinyimg/tinyimg.h`. A module built against a
 * different one is refused at load, because the alternative is misreading a struct field silently.
 */
export const SUPPORTED_ABI = 1;

/** The module's work counters, in the order `TinyWorkCounter` declares them. */
const Counter = {
	sourceSamples: 0,
	decodedSamples: 1,
	blocks: 2,
	transforms: 3,
	transformSamples: 4,
	macroblocks: 5,
	filtered: 6,
	resampled: 7,
	encoded: 8,
	passes: 9
} as const;

type Counter = (typeof Counter)[keyof typeof Counter];

/**
 * What an operation actually did, as counted by the module rather than claimed by its name.
 *
 * See {@link TinyImgModule.measure}.
 */
export interface WorkCounters {
	/** Samples the source carries, from its header. */
	sourceSamples: number;
	/** Samples the decoder produced. Against `sourceSamples` this is the reduction. */
	decodedSamples: number;
	/** Coefficient blocks read out of an entropy coded stream, which a scale cannot lower. */
	blocks: number;
	/** Inverse transforms performed. */
	transforms: number;
	/** Samples those transforms wrote. */
	transformSamples: number;
	/** `transformSamples / transforms`: 64 for a full block, 4 for a true quarter scale. */
	samplesPerTransform: number;
	/** Macroblocks reconstructed, for the codecs built out of them. */
	macroblocks: number;
	/** Macroblocks put through a loop filter. */
	filtered: number;
	/** Samples a resampler wrote. */
	resampled: number;
	/** Samples handed to an encoder. */
	encoded: number;
	/** Full passes made over an image. */
	passes: number;
}

/** What {@link TinyImgModule.measure} hands back. */
export interface Measured<T> {
	/** Whatever the measured body returned, unchanged. */
	value: T;
	/** What the module counted while it ran. */
	work: WorkCounters;
}

/** Optional features a build may or may not contain, mirroring `enum TinyImageFeature`. */
export const Feature = {
	/** SIMD128 was enabled at build time. */
	simd: 1 << 0,
	/** PNG decode and encode. */
	png: 1 << 1,
	/** JPEG decode and encode, baseline and progressive. */
	jpeg: 1 << 2,
	/** BMP decode and encode. */
	bmp: 1 << 3,
	/** GIF decode and encode. */
	gif: 1 << 4,
	/** TIFF decode and encode. */
	tiff: 1 << 5,
	/** WebP decode and encode, lossy and lossless. */
	webp: 1 << 6,
	/** AVIF container parse; probe answers and decode refuses. */
	avif: 1 << 7,
	/** Font loading and text drawing. */
	text: 1 << 8,
	/** Face detection through an LBP cascade. */
	detect: 1 << 9,
	/** ICC matrix and TRC color management. */
	icc: 1 << 10
} as const;

/** The name of an optional feature; see {@link Feature}. */
export type FeatureName = keyof typeof Feature;

/** Container ids, mirroring `TinyImageFormat`. */
const FORMATS: (ImageFormat | 'unknown')[] = [
	'unknown',
	'png',
	'jpeg',
	'bmp',
	'gif',
	'tiff',
	'webp',
	'avif',
	'heif'
];

/** Blob kinds, mirroring `TinyBlobKind`. */
const BLOB_KINDS: Record<BlobKind, number> = { font: 0, icc: 1, cascade: 2 };

/**
 * The module's exports, as far as this wrapper drives them.
 *
 * @internal
 */
export interface TinyExports {
	memory: WebAssembly.Memory;
	tiny_init(): void;
	tiny_version(): number;
	tiny_abi_version(): number;
	tiny_features(): number;
	tiny_error_name(code: number): number;

	tiny_work_reset(): void;
	tiny_work_read(counter: number): number;

	tiny_alloc(size: number): number;
	tiny_free(pointer: number): void;
	tiny_arena_reset(): void;

	tiny_image_sizeof(): number;
	tiny_image_info_sizeof(): number;
	tiny_writer_sizeof(): number;
	tiny_plan_sizeof(): number;
	tiny_plan_resolution_sizeof(): number;
	tiny_face_box_sizeof(): number;

	tiny_image_probe(buffer: number, size: number, info: number): number;
	tiny_image_destroy(image: number): number;
	tiny_image_getwidth(image: number): number;
	tiny_image_getheight(image: number): number;
	tiny_image_getchannels(image: number): number;
	tiny_image_getdata(image: number): number;
	tiny_image_getsize(image: number): number;
	tiny_image_getformat(image: number): number;
	tiny_image_load(image: number, buffer: number, size: number): number;
	tiny_image_load_scaled(
		image: number,
		buffer: number,
		size: number,
		maxWidth: number,
		maxHeight: number
	): number;
	tiny_image_encode(image: number, format: number, opts: number, writer: number): number;
	tiny_image_trim(image: number, tolerance: number): number;
	tiny_image_rotate(image: number, degrees: number, background: number): number;
	tiny_image_detect_faces(image: number, boxes: number, capacity: number, count: number): number;

	tiny_writer_init(writer: number, initial: number): number;
	tiny_writer_free(writer: number): void;
	tiny_writer_data(writer: number): number;
	tiny_writer_size(writer: number): number;

	tiny_plan_init(plan: number, buffer: number, size: number): number;
	tiny_plan_init_image(plan: number, image: number): number;
	tiny_plan_set_fusion(plan: number, enabled: number): number;
	tiny_plan_set_effort(plan: number, effort: number): number;
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
	tiny_plan_fit_with(
		plan: number,
		width: number,
		height: number,
		mode: number,
		gravity: number,
		filter: number
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
	tiny_plan_effect(plan: number, kind: number, params: number): number;
	tiny_plan_count(plan: number): number;
	tiny_plan_resolve(plan: number, resolution: number): number;
	tiny_plan_field(resolution: number, field: number): number;
	tiny_plan_cost(plan: number): number;
	tiny_encode_cost(format: number, width: number, height: number): number;
	tiny_plan_run(plan: number, out: number): number;
	tiny_plan_encode(plan: number, format: number, opts: number, writer: number): number;

	tiny_blob_load(kind: number, id: number, data: number, size: number): number;
	tiny_blob_free(kind: number, id: number): number;
	tiny_blob_free_all(): void;
	tiny_cascade_check(id: number): number;
}

/** Thrown when a module cannot be used, as opposed to a call on it failing. */
export class TinyImgLoadError extends Error {
	/** Always `TinyImgLoadError`. */
	override readonly name = 'TinyImgLoadError';
}

/**
 * A loaded tinyimg module.
 *
 * One instance owns one linear memory, so a caller that wants isolation between concurrent requests
 * loads more than one rather than sharing this. Sharing one is normally what you want on Workers:
 * the module is compiled once at worker startup and the memory is reused, and the allocator hands
 * every buffer back at the end of a call.
 */
export class TinyImgModule {
	/** @internal */
	readonly exports: TinyExports;

	private constructor(exports: TinyExports) {
		this.exports = exports;
		exports.tiny_init();
	}

	/**
	 * Instantiates a compiled module.
	 *
	 * Takes a `WebAssembly.Module` rather than bytes because workerd refuses
	 * `WebAssembly.Module(bytes)` outright ("Wasm code generation disallowed by embedder"). On
	 * Workers the module comes from importing the wasm file, which the runtime compiles at worker
	 * startup:
	 *
	 * ```ts
	 * import wasm from '@gmitch215/tinyimg/tinyimg.wasm';
	 * import { TinyImgModule } from '@gmitch215/tinyimg';
	 *
	 * const tinyimg = TinyImgModule.load(wasm);
	 * ```
	 *
	 * @param module The compiled module.
	 * @return A loaded module ready to use.
	 * @throws TinyImgLoadError If the module is not tinyimg, or its ABI is one this wrapper does
	 * not understand.
	 */
	static load(module: WebAssembly.Module): TinyImgModule {
		const instance = new WebAssembly.Instance(module, {});
		const exports = instance.exports as unknown as TinyExports;

		if (typeof exports.tiny_abi_version !== 'function') {
			throw new TinyImgLoadError(
				'this module does not export tiny_abi_version; is it tinyimg?'
			);
		}

		const abi = exports.tiny_abi_version();
		if (abi !== SUPPORTED_ABI) {
			throw new TinyImgLoadError(
				`module ABI ${abi} does not match the ${SUPPORTED_ABI} this wrapper understands`
			);
		}

		return new TinyImgModule(exports);
	}

	/**
	 * Compiles and instantiates from bytes.
	 *
	 * The escape hatch from {@link load}, for node, bun and the browser, where compiling wasm at
	 * runtime is allowed. **It throws on Workers**, and that is the runtime's rule rather than this
	 * library's: import the wasm file there instead.
	 *
	 * @param source The module's bytes, in any shape {@link Source} accepts.
	 * @return A loaded module ready to use.
	 */
	static async loadBytes(source: Source): Promise<TinyImgModule> {
		const bytes = await readSource(source);

		// workers-types omits WebAssembly.compile because the runtime does not have it, so the
		// cast is the honest way to reach it: this path is for the runtimes that do
		const runtime = WebAssembly as {
			compile?: (bytes: Uint8Array) => Promise<WebAssembly.Module>;
		};

		if (!runtime.compile) {
			throw new TinyImgLoadError(
				'this runtime does not allow compiling wasm from bytes; import the module instead'
			);
		}

		return TinyImgModule.load(await runtime.compile(bytes));
	}

	/** The library version, as `[major, minor, patch]`. */
	get version(): [number, number, number] {
		const packed = this.exports.tiny_version();
		return [(packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff];
	}

	/** The library version as `major.minor.patch`. */
	get versionText(): string {
		return this.version.join('.');
	}

	/** The module's ABI version. Always equal to {@link SUPPORTED_ABI} for a loaded module. */
	get abi(): number {
		return this.exports.tiny_abi_version();
	}

	/** The module's own linear memory. */
	get memory(): WebAssembly.Memory {
		return this.exports.memory;
	}

	/** Pages of linear memory the module currently holds. */
	get pages(): number {
		return this.exports.memory.buffer.byteLength / 65536;
	}

	/**
	 * Whether a feature was compiled into this module.
	 *
	 * Worth checking before offering a format in a UI, rather than calling and handling a failure.
	 *
	 * @param name The feature to check.
	 * @return True when the build contains it.
	 */
	has(name: FeatureName): boolean {
		return (this.exports.tiny_features() & Feature[name]) !== 0;
	}

	/** Every feature this build contains. */
	get features(): FeatureName[] {
		const bits = this.exports.tiny_features();
		return (Object.keys(Feature) as FeatureName[]).filter(
			(name) => (bits & Feature[name]) !== 0
		);
	}

	/**
	 * Reads the name of an error code out of the module.
	 *
	 * @param code A negative `TinyImageError` value, or 0.
	 * @return The short name the module carries for it.
	 */
	errorName(code: number): string {
		return this.readString(this.exports.tiny_error_name(code));
	}

	/**
	 * Clears the work counters, then runs `body` and reports what it actually did.
	 *
	 * Every reduction this library performs is a claim that some work does not happen, and a
	 * label is not evidence: the quarter scale decode carried the word "reduced" through the API,
	 * the planner and the decoder while transforming whole blocks and averaging the result away.
	 * `samplesPerTransform` is the figure that catches that, reading 64 for a full block and 4 for
	 * a genuine quarter.
	 *
	 * The counters are a module-wide total, so nothing else may run against this module while
	 * `body` does.
	 *
	 * @param body The operation to measure. Its result is returned unchanged.
	 * @return The operation's result and the counters it produced.
	 * @example
	 * ```ts
	 * const { work } = await tinyimg.measure(() => image.pixels());
	 * console.log(work.samplesPerTransform, work.decodedSamples / work.sourceSamples);
	 * ```
	 */
	async measure<T>(body: () => T | Promise<T>): Promise<Measured<T>> {
		this.exports.tiny_work_reset();

		const value = await body();
		const read = (counter: Counter) => this.exports.tiny_work_read(counter);
		const transforms = read(Counter.transforms);

		return {
			value,
			work: {
				sourceSamples: read(Counter.sourceSamples),
				decodedSamples: read(Counter.decodedSamples),
				blocks: read(Counter.blocks),
				transforms,
				transformSamples: read(Counter.transformSamples),
				samplesPerTransform: transforms ? read(Counter.transformSamples) / transforms : 0,
				macroblocks: read(Counter.macroblocks),
				filtered: read(Counter.filtered),
				resampled: read(Counter.resampled),
				encoded: read(Counter.encoded),
				passes: read(Counter.passes)
			}
		};
	}

	/**
	 * Reads a file's header without decoding any pixels.
	 *
	 * Every format the library recognizes, including the ones it cannot decode: an AVIF answers
	 * fully here and fails with a specific error on decode.
	 *
	 * @param source The encoded image.
	 * @return What the header says.
	 */
	async probe(source: Source): Promise<ImageInfo> {
		const bytes = await readSource(source);
		const buffer = this.copyIn(bytes);
		const info = this.alloc(this.exports.tiny_image_info_sizeof());

		try {
			this.check(
				this.exports.tiny_image_probe(buffer, bytes.byteLength, info),
				'probe the image'
			);

			// TinyImageInfo is part of the ABI: three u32, an enum, then four u8, in that order
			const view = this.view();

			return {
				width: view.getUint32(info, true),
				height: view.getUint32(info + 4, true),
				frames: view.getUint32(info + 8, true),
				format: FORMATS[view.getUint32(info + 12, true)] ?? 'unknown',
				channels: view.getUint8(info + 16),
				bitDepth: view.getUint8(info + 17),
				hasAlpha: view.getUint8(info + 18) !== 0,
				progressive: view.getUint8(info + 19) !== 0
			};
		} finally {
			this.free(info);
			this.free(buffer);
		}
	}

	/**
	 * Decodes to raw pixels.
	 *
	 * The escape hatch below the transformation surface, for a caller who wants the samples
	 * themselves: a histogram, a hand-written kernel, or a comparison against another decoder.
	 * Prefer {@link transform} or {@link Image} when the answer is another image, because those let
	 * the planner decide what to decode and this decodes all of it.
	 *
	 * @param source The encoded image.
	 * @return The extent, the channel count and `width * height * channels` bytes, rows tightly
	 * packed.
	 */
	async decode(source: Source): Promise<RawImage> {
		const bytes = await readSource(source);
		const buffer = this.copyIn(bytes);
		const image = this.alloc(this.exports.tiny_image_sizeof());

		try {
			this.check(
				this.exports.tiny_image_load(image, buffer, bytes.byteLength),
				'decode the image'
			);

			try {
				return {
					width: this.exports.tiny_image_getwidth(image),
					height: this.exports.tiny_image_getheight(image),
					channels: this.exports.tiny_image_getchannels(image),
					pixels: this.copyOut(
						this.exports.tiny_image_getdata(image),
						this.exports.tiny_image_getsize(image)
					)
				};
			} finally {
				this.exports.tiny_image_destroy(image);
			}
		} finally {
			this.free(image);
			this.free(buffer);
		}
	}

	/**
	 * Hands the module a blob it will read later.
	 *
	 * Nothing large or optional is linked in: fonts, color profiles and detection cascades all
	 * arrive at runtime, and the module owns the bytes from here until {@link freeBlob}. Two ways to
	 * deliver one, and no code changes between them:
	 *
	 * ```ts
	 * // from a bucket, which costs one subrequest and no bundle bytes
	 * const font = await env.BLOBS.get('fonts/DejaVuSans.ttf');
	 * if (font) await tinyimg.loadBlob('font', 'sans', font.body);
	 *
	 * // or imported as a wrangler Data module, which costs bundle bytes and no latency
	 * import cascade from './lbp-frontalface.bin';
	 * await tinyimg.loadBlob('cascade', 'frontal', cascade);
	 * ```
	 *
	 * Loading over an existing kind and id replaces it. At most eight are resident at once.
	 *
	 * @param kind What the bytes are.
	 * @param id The name the module will find it by, at most 31 characters.
	 * @param source The bytes, in any shape {@link Source} accepts.
	 * @throws TinyImgError If all eight slots are taken, or a cascade does not parse.
	 */
	async loadBlob(kind: BlobKind, id: string, source: Source): Promise<void> {
		const bytes = await readSource(source);
		const data = this.copyIn(bytes);
		const name = this.writeString(id);

		try {
			// the module takes `data`, so it is not freed here: a refused load is the only path
			// that leaks it and it cannot be double freed
			this.check(
				this.exports.tiny_blob_load(BLOB_KINDS[kind], name, data, bytes.byteLength),
				`load the ${kind} blob '${id}'`
			);

			// a cascade that does not parse is a configuration fault, and finding no faces later
			// would hide it
			if (kind === 'cascade') {
				this.check(this.exports.tiny_cascade_check(name), `parse the cascade blob '${id}'`);
			}
		} finally {
			this.free(name);
		}
	}

	/**
	 * Releases one resident blob.
	 *
	 * @param kind What to release.
	 * @param id The id it was loaded under, or omitted for the first blob of that kind.
	 * @return True when one was released.
	 */
	freeBlob(kind: BlobKind, id?: string): boolean {
		const name = id === undefined ? 0 : this.writeString(id);

		try {
			return this.exports.tiny_blob_free(BLOB_KINDS[kind], name) === Err.ok;
		} finally {
			if (name !== 0) this.free(name);
		}
	}

	/** Releases every resident blob. */
	freeBlobs(): void {
		this.exports.tiny_blob_free_all();
	}

	/**
	 * Finds the faces in an image.
	 *
	 * Needs at least one cascade loaded through {@link loadBlob}; with none it throws a
	 * {@link TinyImgBlobError} rather than reporting no faces, because the two mean different
	 * things. Runs every resident cascade and groups the results, so a frontal and a profile
	 * cascade together find both kinds of face and a face that fires both is one box.
	 *
	 * @param source The encoded image.
	 * @param limit Most boxes to return.
	 * @return The detections, ordered by confidence, in the source image's own coordinates.
	 */
	async detectFaces(source: Source, limit = 16): Promise<FaceBox[]> {
		const bytes = await readSource(source);
		const buffer = this.copyIn(bytes);
		const image = this.alloc(this.exports.tiny_image_sizeof());
		const stride = this.exports.tiny_face_box_sizeof();
		const boxes = this.alloc(stride * limit);
		const count = this.alloc(4);

		try {
			this.check(
				this.exports.tiny_image_load(image, buffer, bytes.byteLength),
				'decode the image'
			);

			try {
				this.check(
					this.exports.tiny_image_detect_faces(image, boxes, limit, count),
					'detect faces'
				);
			} finally {
				this.exports.tiny_image_destroy(image);
			}

			const view = this.view();
			const found = view.getUint32(count, true);
			const out: FaceBox[] = [];

			for (let i = 0; i < found; i++) {
				const at = boxes + i * stride;
				out.push({
					x: view.getUint32(at, true),
					y: view.getUint32(at + 4, true),
					width: view.getUint32(at + 8, true),
					height: view.getUint32(at + 12, true),
					neighbors: view.getUint32(at + 16, true)
				});
			}

			return out;
		} finally {
			this.free(count);
			this.free(boxes);
			this.free(image);
			this.free(buffer);
		}
	}

	// #region internals

	/**
	 * A view over the module's memory, taken fresh every time.
	 *
	 * Never cached: an allocation may grow memory, which detaches the old buffer and leaves any
	 * view over it throwing on every access.
	 *
	 * @internal
	 */
	view(): DataView {
		return new DataView(this.exports.memory.buffer);
	}

	/** @internal */
	bytes(): Uint8Array {
		return new Uint8Array(this.exports.memory.buffer);
	}

	/** @internal */
	alloc(size: number): number {
		const pointer = this.exports.tiny_alloc(size);
		if (pointer === 0) {
			throw errorFor(Err.memory, this.errorName(Err.memory), `allocate ${size} bytes`);
		}

		return pointer;
	}

	/** @internal */
	free(pointer: number): void {
		this.exports.tiny_free(pointer);
	}

	/** @internal */
	copyIn(source: Uint8Array): number {
		const pointer = this.alloc(source.byteLength);
		this.bytes().set(source, pointer);
		return pointer;
	}

	/**
	 * Copies bytes out of the module into a buffer the caller owns.
	 *
	 * The return type names a real `ArrayBuffer` rather than the `ArrayBufferLike` a view over
	 * linear memory carries, because this genuinely allocates one: without that a caller cannot
	 * hand the result to a `Blob` or a `Response` without a cast.
	 *
	 * @internal
	 */
	copyOut(pointer: number, size: number): Uint8Array<ArrayBuffer> {
		const out = new Uint8Array(size);
		out.set(this.bytes().subarray(pointer, pointer + size));

		return out;
	}

	/** @internal */
	readString(pointer: number): string {
		if (pointer === 0) return '';

		const memory = this.bytes().subarray(pointer);
		const end = memory.indexOf(0);

		return new TextDecoder().decode(memory.subarray(0, end < 0 ? undefined : end));
	}

	/** @internal */
	writeString(value: string): number {
		const encoded = new TextEncoder().encode(value);
		const pointer = this.alloc(encoded.byteLength + 1);
		const memory = this.bytes();

		memory.set(encoded, pointer);
		memory[pointer + encoded.byteLength] = 0;

		return pointer;
	}

	/**
	 * Turns a negative result into the error it stands for.
	 *
	 * @internal
	 */
	check(result: number, context: string): void {
		if (result >= Err.ok) return;
		throw errorFor(result, this.errorName(result), context);
	}

	/**
	 * Reads a container id back into its name.
	 *
	 * @internal
	 */
	formatName(id: number): ImageFormat | 'unknown' {
		return FORMATS[id] ?? 'unknown';
	}

	/**
	 * Turns a format name into the id the module uses.
	 *
	 * @internal
	 */
	formatId(name: ImageFormat): number {
		const id = FORMATS.indexOf(name);
		if (id <= 0) throw new TypeError(`'${name}' is not a container format`);
		return id;
	}

	// #endregion
}
