/**
 * The ABI version this wrapper understands.
 *
 * Bumped in lockstep with `TINYIMG_ABI_VERSION` in `include/tinyimg.h`. A module built against a
 * different one is refused at load, because the alternative is misreading a struct field silently.
 */
export const SUPPORTED_ABI = 1;

/** Optional features a build may or may not contain, mirroring `enum TinyImageFeature`. */
export const Feature = {
	simd: 1 << 0,
	png: 1 << 1,
	jpeg: 1 << 2,
	bmp: 1 << 3,
	gif: 1 << 4,
	tiff: 1 << 5,
	webp: 1 << 6,
	avif: 1 << 7,
	text: 1 << 8,
	detect: 1 << 9,
	icc: 1 << 10
} as const;

export type FeatureName = keyof typeof Feature;

/** The exports `include/tinyimg.h` declares. Grows with each phase. */
interface Exports {
	memory: WebAssembly.Memory;
	tiny_version(): number;
	tiny_abi_version(): number;
	tiny_features(): number;
	tiny_error_name(code: number): number;
}

/** Thrown when a module cannot be used, as opposed to a call on it failing. */
export class TinyImgLoadError extends Error {
	override readonly name = 'TinyImgLoadError';
}

/**
 * A loaded tinyimg module.
 *
 * One instance owns one linear memory, so a caller that wants isolation between concurrent requests
 * loads more than one rather than sharing this.
 */
export class TinyImgModule {
	readonly #exports: Exports;

	private constructor(exports: Exports) {
		this.#exports = exports;
	}

	/**
	 * Instantiates a compiled module.
	 *
	 * Takes a `WebAssembly.Module` rather than bytes because workerd refuses
	 * `WebAssembly.Module(bytes)` outright ("Wasm code generation disallowed by embedder"). On Workers
	 * the module comes from importing the wasm file, which the runtime compiles at worker startup:
	 *
	 * ```ts
	 * import wasm from '@gmitch215/tinyimg/tinyimg.wasm';
	 *
	 * const tinyimg = TinyImgModule.load(wasm);
	 * ```
	 *
	 * Under node or bun, where compilation is allowed, compile first:
	 *
	 * ```ts
	 * const tinyimg = TinyImgModule.load(await WebAssembly.compile(bytes));
	 * ```
	 *
	 * @param module The compiled module.
	 * @return A loaded module ready to use.
	 * @throws TinyImgLoadError If the module is not tinyimg, or its ABI is one this wrapper does not
	 * understand.
	 */
	static load(module: WebAssembly.Module): TinyImgModule {
		const instance = new WebAssembly.Instance(module, {});
		const exports = instance.exports as unknown as Exports;

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

	/** The library version, as `[major, minor, patch]`. */
	get version(): [number, number, number] {
		const packed = this.#exports.tiny_version();
		return [(packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff];
	}

	/** The library version as `major.minor.patch`. */
	get versionText(): string {
		return this.version.join('.');
	}

	/** The module's ABI version. Always equal to {@link SUPPORTED_ABI} for a loaded module. */
	get abi(): number {
		return this.#exports.tiny_abi_version();
	}

	/** The module's own linear memory. */
	get memory(): WebAssembly.Memory {
		return this.#exports.memory;
	}

	/**
	 * Whether a feature was compiled into this module.
	 *
	 * @param name The feature to check.
	 * @return True when the build contains it.
	 */
	has(name: FeatureName): boolean {
		return (this.#exports.tiny_features() & Feature[name]) !== 0;
	}

	/** Every feature this build contains. */
	get features(): FeatureName[] {
		const bits = this.#exports.tiny_features();
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
		return this.#readString(this.#exports.tiny_error_name(code));
	}

	/** Reads a NUL-terminated ASCII string out of linear memory. */
	#readString(pointer: number): string {
		if (pointer === 0) return '';
		const view = new Uint8Array(this.#exports.memory.buffer, pointer);
		const end = view.indexOf(0);
		return new TextDecoder().decode(view.subarray(0, end < 0 ? undefined : end));
	}
}
