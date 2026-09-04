import { describe, expect, it } from 'vitest';
import bytes from '../../bin/tinyimg.wasm?bin';
import { Feature, SUPPORTED_ABI, TinyImgLoadError, TinyImgModule } from '../../src/ts/index.js';

const compiled = await WebAssembly.compile(bytes);

describe('TinyImgModule.load', () => {
	it('loads the module this build produced', () => {
		const tinyimg = TinyImgModule.load(compiled);
		expect(tinyimg.abi).toBe(SUPPORTED_ABI);
		expect(tinyimg.versionText).toBe('1.0.0');
		expect(tinyimg.version).toEqual([1, 0, 0]);
	});

	it('exposes the module memory', () => {
		expect(TinyImgModule.load(compiled).memory).toBeInstanceOf(WebAssembly.Memory);
	});

	it('gives each load its own memory, so two do not share state', () => {
		const first = TinyImgModule.load(compiled);
		const second = TinyImgModule.load(compiled);
		expect(first.memory).not.toBe(second.memory);
	});

	it('refuses a module that is not tinyimg', async () => {
		// (module) with no exports at all
		const empty = await WebAssembly.compile(new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]));
		expect(() => TinyImgModule.load(empty)).toThrow(TinyImgLoadError);
		expect(() => TinyImgModule.load(empty)).toThrow(/does not export tiny_abi_version/);
	});

	it('reports the features the build was compiled with', () => {
		const tinyimg = TinyImgModule.load(compiled);
		expect(tinyimg.has('simd')).toBe(true);
		expect(tinyimg.has('png')).toBe(true);
		expect(tinyimg.features).toContain('jpeg');
		expect(tinyimg.features).toContain('webp');
	});

	it('names every error code the header declares', () => {
		const tinyimg = TinyImgModule.load(compiled);
		expect(tinyimg.errorName(0)).toBe('ok');
		expect(tinyimg.errorName(-4)).toBe('out of memory');
		expect(tinyimg.errorName(-5)).toBe('image too large');
		expect(tinyimg.errorName(-7)).toBe('unsupported codec');
		expect(tinyimg.errorName(-14)).toBe('invalid plan');
	});

	it('names an unrecognized code rather than reading past the table', () => {
		expect(TinyImgModule.load(compiled).errorName(-9999)).toBe('unknown');
	});

	it('has one flag per feature, none colliding', () => {
		const values = Object.values(Feature);
		expect(new Set(values).size).toBe(values.length);
		for (const value of values) {
			expect(value & (value - 1), `${value} is not a single bit`).toBe(0);
		}
	});
});
