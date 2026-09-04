import module from '../../../bin/tinyimg.wasm';
import { Image, TinyImgError, TinyImgModule, transform } from '../../../dist/index.js';

// compiled by the embedder at worker startup, which is the only way workerd allows: it refuses
// WebAssembly.Module(bytes) outright. One instance for the isolate, which is what a real Worker
// does, and it is also what makes the memory-growth assertions meaningful
const tinyimg = TinyImgModule.load(module);

/** Serves a transformation the way the README's quick start does. */
async function serve(request, url) {
	const options = {};
	const number = (name) => {
		const value = url.searchParams.get(name);
		if (value !== null) options[name] = Number(value);
	};
	const text = (name) => {
		const value = url.searchParams.get(name);
		if (value !== null) options[name] = value;
	};
	const flag = (name) => {
		if (url.searchParams.has(name)) options[name] = true;
	};

	for (const name of [
		'width',
		'height',
		'quality',
		'dpr',
		'blur',
		'sharpen',
		'brightness',
		'contrast',
		'saturation',
		'gamma',
		'hue',
		'rotate'
	]) {
		number(name);
	}
	for (const name of ['fit', 'gravity', 'format', 'flip', 'metadata', 'background']) {
		text(name);
	}
	for (const name of ['grayscale', 'invert', 'trim', 'lossless']) flag(name);

	const result = await transform(tinyimg, request.body, options);

	return result.response({ 'x-tinyimg-format': result.format });
}

export default {
	async fetch(request) {
		const url = new URL(request.url);

		try {
			if (url.searchParams.has('version')) {
				return Response.json({
					version: tinyimg.versionText,
					abi: tinyimg.abi,
					features: tinyimg.features
				});
			}

			if (url.searchParams.has('pages')) {
				return Response.json({ pages: tinyimg.pages });
			}

			if (url.searchParams.has('probe')) {
				const bytes = new Uint8Array(await request.arrayBuffer());
				return Response.json(await tinyimg.probe(bytes));
			}

			if (url.searchParams.has('decide')) {
				const bytes = new Uint8Array(await request.arrayBuffer());
				const image = await Image.open(tinyimg, bytes);

				// try/finally rather than `using`: this file is loaded as-is, and the `using`
				// declaration is syntax rather than a runtime feature, so relying on it here
				// would be relying on the runtime's parser instead of on Symbol.dispose
				try {
					const width = Number(url.searchParams.get('width') ?? 0);
					const height = Number(url.searchParams.get('height') ?? 0);

					if (url.searchParams.has('crop')) {
						const [x, y, w, h] = url.searchParams.get('crop').split(',').map(Number);
						image.crop(x, y, w, h);
					}
					if (width || height) image.resize(width, height);

					return Response.json(image.decide());
				} finally {
					image.dispose();
				}
			}

			if (url.searchParams.has('chain')) {
				const bytes = new Uint8Array(await request.arrayBuffer());
				const image = await Image.open(tinyimg, bytes);

				try {
					image.resize(120, 0).grayscale().sharpen(1);

					// two encodings of one plan, which is what the chain is for
					const png = await image.bytes('png');
					const webp = await image.bytes('webp', { quality: 70 });

					return Response.json({
						png: png.byteLength,
						webp: webp.byteLength,
						operations: image.operations
					});
				} finally {
					image.dispose();
				}
			}

			return await serve(request, url);
		} catch (error) {
			if (error instanceof TinyImgError) {
				return Response.json(
					{ error: error.codeName, code: error.code, name: error.name },
					{ status: 422 }
				);
			}

			return Response.json({ error: String(error), name: 'unknown' }, { status: 500 });
		}
	}
};
