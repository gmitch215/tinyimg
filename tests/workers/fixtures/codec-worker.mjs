import module from '../../../bin/tinyimg.wasm';

// its own instance, separate from the probe worker's, so decoding here cannot move the page counts
// that worker's assertions read
const instance = new WebAssembly.Instance(module, {});
const api = instance.exports;

api.tiny_init();

function hex(buffer) {
	return Array.from(new Uint8Array(buffer))
		.map((byte) => byte.toString(16).padStart(2, '0'))
		.join('');
}

function copyIn(bytes) {
	const pointer = api.tiny_alloc(bytes.byteLength);
	if (pointer === 0) return 0;

	// after the view, never before: an allocation may have grown memory and detached the old buffer
	new Uint8Array(api.memory.buffer).set(bytes, pointer);
	return pointer;
}

/**
 * Decodes, optionally re-encodes, and reports what came out.
 *
 * Pixels are reported as a digest rather than as bytes so the response stays small and the check
 * stays exact; the expected values live in tests/support/golden.ts.
 */
async function run(bytes, options) {
	const pagesBefore = api.memory.buffer.byteLength / 65536;

	const buffer = copyIn(bytes);
	if (buffer === 0) return { error: 'alloc' };

	const image = api.tiny_alloc(api.tiny_image_sizeof());

	let result;

	if (options.scale) {
		result = api.tiny_image_load_scaled(
			image,
			buffer,
			bytes.byteLength,
			options.scale[0],
			options.scale[1]
		);
	} else if (options.region) {
		result = api.tiny_image_load_region(
			image,
			buffer,
			bytes.byteLength,
			options.region[0],
			options.region[1],
			options.region[2],
			options.region[3]
		);
	} else {
		result = api.tiny_image_load(image, buffer, bytes.byteLength);
	}

	if (result !== 0) {
		/*
		 * A file that cannot be decoded may still be describable, which is the whole point of
		 * having a separate probe: AVIF answers one and refuses the other, so the header fields go
		 * in the report beside the reason the pixels did not come.
		 */
		const info = api.tiny_alloc(api.tiny_image_info_sizeof());
		const probed = api.tiny_image_probe(buffer, bytes.byteLength, info);
		const fields = new DataView(api.memory.buffer, info, api.tiny_image_info_sizeof());

		const described =
			probed === 0
				? {
						probeResult: probed,
						width: fields.getUint32(0, true),
						height: fields.getUint32(4, true),
						frames: fields.getUint32(8, true),
						format: fields.getUint32(12, true),
						channels: fields.getUint8(16),
						bitDepth: fields.getUint8(17),
						hasAlpha: fields.getUint8(18) !== 0
					}
				: { probeResult: probed };

		api.tiny_free(info);
		api.tiny_free(image);
		api.tiny_free(buffer);

		return { result, errorName: readString(api.tiny_error_name(result)), ...described };
	}

	const size = api.tiny_image_getsize(image);
	const pixels = new Uint8Array(api.memory.buffer).slice(
		api.tiny_image_getdata(image),
		api.tiny_image_getdata(image) + size
	);

	const report = {
		result,
		width: api.tiny_image_getwidth(image),
		height: api.tiny_image_getheight(image),
		channels: api.tiny_image_getchannels(image),
		format: api.tiny_image_getformat(image),
		size,
		digest: hex(await crypto.subtle.digest('SHA-256', pixels)),
		pagesBefore,
		pagesAfter: api.memory.buffer.byteLength / 65536
	};

	if (options.reencode) {
		const writer = api.tiny_alloc(api.tiny_writer_sizeof());
		api.tiny_writer_init(writer, 0);

		// four bytes mirroring TinyEncodeOpts, or a null pointer for the defaults. WebP needs it:
		// its two modes are one flag apart and the lossless one is what a digest can pin
		let opts = 0;

		if (options.lossless || options.quality) {
			opts = api.tiny_alloc(4);

			const fields = new Uint8Array(api.memory.buffer, opts, 4);

			fields[0] = options.quality ?? 0;
			fields[1] = options.lossless ? 1 : 0;
			fields[2] = 0;
			fields[3] = 0;
		}

		const encoded = api.tiny_image_encode(image, options.reencode, opts, writer);
		report.encodeResult = encoded;

		if (encoded === 0) {
			const encodedBytes = new Uint8Array(api.memory.buffer).slice(
				api.tiny_writer_data(writer),
				api.tiny_writer_data(writer) + api.tiny_writer_size(writer)
			);
			report.encodedSize = encodedBytes.byteLength;

			// straight back in, so a corrupt encode shows up as a digest that no longer matches
			const again = api.tiny_alloc(api.tiny_image_sizeof());
			const round = api.tiny_image_load(again, copyIn(encodedBytes), encodedBytes.byteLength);

			if (round === 0) {
				const roundSize = api.tiny_image_getsize(again);
				const roundPixels = new Uint8Array(api.memory.buffer).slice(
					api.tiny_image_getdata(again),
					api.tiny_image_getdata(again) + roundSize
				);
				report.roundTripDigest = hex(await crypto.subtle.digest('SHA-256', roundPixels));
			}

			report.roundTripResult = round;
			api.tiny_image_destroy(again);
			api.tiny_free(again);
		}

		api.tiny_writer_free(writer);
		api.tiny_free(writer);
		if (opts) api.tiny_free(opts);
	}

	api.tiny_image_destroy(image);
	api.tiny_free(image);
	api.tiny_free(buffer);

	return report;
}

/** Copies floats into the module and returns the pointer, which the caller frees. */
function floatsIn(values) {
	const pointer = api.tiny_alloc(Math.max(values.length, 1) * 4);
	const view = new DataView(api.memory.buffer);

	for (let i = 0; i < values.length; i++) view.setFloat32(pointer + i * 4, values[i], true);

	return pointer;
}

/**
 * Builds a plan from a compact description and runs it.
 *
 * The description is the query string rather than a structure, so the spec can name a chain in a
 * line and this worker stays the only thing that knows the export names. Each entry is an operation
 * and its operands: `crop:400,200,900,600` then `resize:300,200` then `brightness:1.2`.
 */
async function plan(bytes, chain, fusion) {
	const buffer = copyIn(bytes);
	if (buffer === 0) return { error: 'alloc' };

	const handle = api.tiny_alloc(api.tiny_plan_sizeof());
	const image = api.tiny_alloc(api.tiny_image_sizeof());
	const resolution = api.tiny_alloc(api.tiny_plan_resolution_sizeof());

	let result = api.tiny_plan_init(handle, buffer, bytes.byteLength);

	if (result === 0) {
		for (const step of chain) {
			const [name, operands] = step.split(':');
			const values = operands ? operands.split(',').map(Number) : [];

			switch (name) {
				case 'crop':
					result = api.tiny_plan_crop(handle, ...values);
					break;
				case 'resize':
					result = api.tiny_plan_resize(handle, ...values);
					break;
				case 'filter':
					result = api.tiny_plan_resize_with(handle, ...values);
					break;
				case 'fit':
					result = api.tiny_plan_fit(handle, ...values);
					break;
				case 'rotate':
					result = api.tiny_plan_rotate(handle, ...values);
					break;
				case 'fliph':
					result = api.tiny_plan_flip_horizontal(handle);
					break;
				case 'flipv':
					result = api.tiny_plan_flip_vertical(handle);
					break;
				case 'brightness':
					result = api.tiny_plan_brightness(handle, ...values);
					break;
				case 'contrast':
					result = api.tiny_plan_contrast(handle, ...values);
					break;
				case 'saturation':
					result = api.tiny_plan_saturation(handle, ...values);
					break;
				case 'hue':
					result = api.tiny_plan_hue(handle, ...values);
					break;
				case 'gamma':
					result = api.tiny_plan_gamma(handle, ...values);
					break;
				case 'grayscale':
					result = api.tiny_plan_grayscale(handle);
					break;
				case 'invert':
					result = api.tiny_plan_invert(handle);
					break;
				case 'blur':
					result = api.tiny_plan_gaussian_blur(handle, ...values);
					break;
				case 'matrix': {
					// twelve numbers, so the operands are the matrix itself
					const pointer = floatsIn(values);
					result = api.tiny_plan_matrix(handle, pointer);
					api.tiny_free(pointer);
					break;
				}
				case 'curve': {
					// the first operand names the curve, the rest are its parameters
					const pointer = floatsIn(values.slice(1));
					result = api.tiny_plan_curve(handle, values[0], pointer, 0);
					api.tiny_free(pointer);
					break;
				}
				case 'effect': {
					const pointer = floatsIn(values.slice(1));
					result = api.tiny_plan_effect(handle, values[0], pointer);
					api.tiny_free(pointer);
					break;
				}
				default:
					result = -2;
					break;
			}

			if (result !== 0) break;
		}
	}

	api.tiny_plan_set_fusion(handle, fusion);

	const report = { result, ops: api.tiny_plan_count(handle) };

	if (result === 0) {
		result = api.tiny_plan_resolve(handle, resolution);
		report.result = result;
	}

	if (result === 0) {
		// TinyDecodeOpts is four u32 then two u8, and the two extents follow it
		const fields = new DataView(api.memory.buffer, resolution, 28);

		report.decode = {
			x: fields.getUint32(0, true),
			y: fields.getUint32(4, true),
			width: fields.getUint32(8, true),
			height: fields.getUint32(12, true),
			scale: fields.getUint8(16),
			channels: fields.getUint8(17)
		};
		report.decodeWidth = fields.getUint32(20, true);
		report.decodeHeight = fields.getUint32(24, true);

		result = api.tiny_plan_run(handle, image);
		report.result = result;
	}

	if (result === 0) {
		const size = api.tiny_image_getsize(image);
		const pixels = new Uint8Array(api.memory.buffer).slice(
			api.tiny_image_getdata(image),
			api.tiny_image_getdata(image) + size
		);

		report.width = api.tiny_image_getwidth(image);
		report.height = api.tiny_image_getheight(image);
		report.channels = api.tiny_image_getchannels(image);
		report.format = api.tiny_image_getformat(image);
		report.size = size;
		report.digest = hex(await crypto.subtle.digest('SHA-256', pixels));

		api.tiny_image_destroy(image);
	} else {
		report.errorName = readString(api.tiny_error_name(result));
	}

	api.tiny_free(resolution);
	api.tiny_free(image);
	api.tiny_free(handle);
	api.tiny_free(buffer);

	return report;
}

function readString(pointer) {
	const memory = new Uint8Array(api.memory.buffer);

	let end = pointer;
	while (end < memory.byteLength && memory[end] !== 0) end++;

	return new TextDecoder().decode(memory.subarray(pointer, end));
}

/**
 * Asks for more than the free space can possibly hold, so growth is forced no matter what earlier
 * requests left behind.
 */
function grow() {
	const pagesBefore = api.memory.buffer.byteLength / 65536;

	// the whole of linear memory cannot fit inside its own free space
	const size = pagesBefore * 65536;
	const pointer = api.tiny_alloc(size);

	const pagesAfter = api.memory.buffer.byteLength / 65536;
	if (pointer === 0) return { pointer, pagesBefore, pagesAfter, usable: false };

	// the new region has to be writable at both ends, not merely reserved
	const memory = new Uint8Array(api.memory.buffer);
	memory[pointer] = 0x11;
	memory[pointer + size - 1] = 0x22;

	const usable = memory[pointer] === 0x11 && memory[pointer + size - 1] === 0x22;
	api.tiny_free(pointer);

	return {
		pointer,
		pagesBefore,
		pagesAfter,
		usable,
		pagesAfterFree: api.memory.buffer.byteLength / 65536
	};
}

export default {
	async fetch(request) {
		const url = new URL(request.url);

		if (url.searchParams.has('grow')) return Response.json(grow());
		if (url.searchParams.has('ceiling')) {
			// --max-memory is 64 MiB, so this has to come back as a null rather than a trap
			return Response.json({ pointer: api.tiny_alloc(200 * 1024 * 1024) });
		}

		const bytes = new Uint8Array(await request.arrayBuffer());

		if (url.searchParams.has('plan')) {
			return Response.json(
				await plan(
					bytes,
					url.searchParams.getAll('plan'),
					url.searchParams.has('eager') ? 0 : 1
				)
			);
		}

		const scale = url.searchParams.get('scale');
		const region = url.searchParams.get('region');
		const reencode = url.searchParams.get('reencode');

		const quality = url.searchParams.get('quality');

		return Response.json(
			await run(bytes, {
				scale: scale ? scale.split(',').map(Number) : null,
				region: region ? region.split(',').map(Number) : null,
				reencode: reencode ? Number(reencode) : 0,
				lossless: url.searchParams.has('lossless'),
				quality: quality ? Number(quality) : 0
			})
		);
	}
};
