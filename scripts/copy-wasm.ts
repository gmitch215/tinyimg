/**
 * Copies the linked module into `dist/`, where the published package points at it.
 *
 * `tsc` only emits JavaScript, so the wasm has to be placed beside it or the package's
 * `./tinyimg.wasm` export resolves to nothing.
 */

import { copyFileSync, existsSync, mkdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

const root = join(import.meta.dirname, '..');
const source = join(root, 'bin', 'tinyimg.wasm');
const target = join(root, 'dist', 'tinyimg.wasm');

if (!existsSync(source)) {
	console.error(`no module at ${source}; run "bun run build:wasm" first`);
	process.exit(1);
}

mkdirSync(join(root, 'dist'), { recursive: true });
copyFileSync(source, target);
console.log(`tinyimg: copied ${statSync(target).size.toLocaleString()} bytes to dist/tinyimg.wasm`);
