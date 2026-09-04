import { readFileSync } from 'node:fs';
import type { Plugin } from 'vite';

const SUFFIX = '?bin';

/**
 * Resolves `import bytes from '../../bin/tinyimg.wasm?bin'` to a `Uint8Array`.
 *
 * Both lanes need the same import to work and one of them runs inside workerd, which has no
 * filesystem to read from. Vite's own `.wasm` handling produces a URL or an instantiation helper,
 * and neither of those is the bytes.
 */
export function wasmBytes(): Plugin {
	return {
		name: 'tinyimg:wasm-bytes',
		enforce: 'pre',
		resolveId(source, importer) {
			if (!source.endsWith(SUFFIX)) return null;
			const resolved = new URL(source, `file://${importer ?? process.cwd()}`);
			return decodeURIComponent(resolved.pathname) + SUFFIX;
		},
		load(id) {
			if (!id.endsWith(SUFFIX)) return null;
			const base64 = readFileSync(id.slice(0, -SUFFIX.length), 'base64');
			return [
				`const encoded = ${JSON.stringify(base64)};`,
				'const binary = atob(encoded);',
				'const bytes = new Uint8Array(binary.length);',
				'for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);',
				'export default bytes;'
			].join('\n');
		}
	};
}
