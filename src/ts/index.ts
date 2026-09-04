/**
 * tinyimg: image decoding, transformation and drawing for Cloudflare Workers.
 *
 * The module has to arrive already compiled, because workerd refuses to compile wasm from bytes.
 * Importing the file is what makes it work there; {@link TinyImgModule.loadBytes} is for node, bun
 * and the browser.
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
 *     const thumb = await transform(tinyimg, source, {
 *       width: 400,
 *       height: 400,
 *       fit: 'cover',
 *       format: 'webp',
 *       quality: 80
 *     });
 *
 *     return thumb.response();
 *   }
 * };
 * ```
 *
 * Three tiers, and they are the same planner underneath:
 *
 * - {@link transform} for one call with an option object, which is most requests.
 * - {@link Image} for a chain, when you want two encodings of one transformation or an operation
 *   the option object does not cover.
 * - {@link TinyImgModule} for the things that are not transformations: {@link TinyImgModule.probe},
 *   {@link TinyImgModule.detectFaces} and {@link TinyImgModule.loadBlob}.
 *
 * Nothing runs until the output is asked for. A 400x400 thumbnail of a 16 megapixel photograph
 * decodes a region at a reduced scale rather than 16 megapixels, and that decision cannot be made
 * after the first operation has already run.
 */

export { Image, mimeFor, type EncodeOptions, type FitOptions } from './image.js';
export {
	TRANSFORM_ORDER,
	apply,
	artifactKey,
	transform,
	type TransformResult
} from './transform.js';
export {
	Err,
	TinyImgArgumentError,
	TinyImgBlobError,
	TinyImgDataError,
	TinyImgError,
	TinyImgFormatError,
	TinyImgMemoryError,
	TinyImgPlanError,
	TinyImgTooLargeError,
	readColor,
	readSource,
	type BlobKind,
	type Color,
	type Extent,
	type FaceBox,
	type Fit,
	type FlipAxis,
	type Gravity,
	type ImageFormat,
	type ImageInfo,
	type MetadataPolicy,
	type PlanDecision,
	type RawImage,
	type Rect,
	type ResampleFilter,
	type Source,
	type TransformOptions
} from './types.js';
export {
	Feature,
	SUPPORTED_ABI,
	TinyImgLoadError,
	TinyImgModule,
	type FeatureName,
	type Measured,
	type WorkCounters
} from './wasm.js';
