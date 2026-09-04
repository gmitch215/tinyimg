import module from '../../../bin/tinyimg.wasm';

// instantiated at module scope, which is the only point workerd permits; a deployed tinyimg Worker
// has exactly this shape
const instance = new WebAssembly.Instance(module, {});

export default {
	fetch(request) {
		const { tiny_version, tiny_abi_version, tiny_features, memory } = instance.exports;

		// the instance outlives every request, so growing is opt-in or the page count drifts
		const grow = Number(new URL(request.url).searchParams.get('grow') ?? 0);
		const pages = memory.buffer.byteLength / 65536;
		if (grow > 0) memory.grow(grow);

		return Response.json({
			version: tiny_version(),
			abi: tiny_abi_version(),
			features: tiny_features(),
			imports: WebAssembly.Module.imports(module).length,
			exportsMemory: memory instanceof WebAssembly.Memory,
			pages,
			pagesAfterGrow: memory.buffer.byteLength / 65536
		});
	}
};
