/**
 * Any path with a `?bin` query resolves to its bytes.
 *
 * One wildcard rather than one declaration per extension, because the `tinyimg:wasm-bytes` vite
 * plugin keys on the query and nothing else. Listing extensions meant the two could disagree, and
 * they did: adding font and cascade fixtures broke the typecheck until this was collapsed.
 */
declare module '*?bin' {
	const bytes: Uint8Array<ArrayBuffer>;
	export default bytes;
}
