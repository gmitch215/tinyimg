# 🏞️ tinyimg

> Lightweight & fast image processing library for Cloudflare Workers

tinyimg decodes, transforms, draws on and re-encodes images inside a Worker, with no binding, no
subrequest and no per-transformation charge. It is freestanding C compiled to one wasm32 module,
wrapped by a TypeScript package.

Cloudflare Images bills per transformation, and the Workers runtime has no Canvas API: no
`OffscreenCanvas`, no `createImageBitmap`, no 2D context. tinyimg does both jobs inside the Worker's
own CPU budget.

## Table of Contents

- [Why tinyimg](#why-tinyimg)
- [Compared With Cloudflare Images](#compared-with-cloudflare-images)
- [Install](#install)
- [Getting Started](#getting-started)
- [Transformations](#transformations)
- [Chaining](#chaining)
- [The Planner](#the-planner)
- [Codec Support](#codec-support)
- [Drawing](#drawing)
- [Effects](#effects)
- [Text](#text)
- [Face Detection](#face-detection)
- [Color Management](#color-management)
- [Budget and Effort](#budget-and-effort)
- [Caching](#caching)
- [Work Counters](#work-counters)
- [Blobs](#blobs)
- [Error Handling](#error-handling)
- [Size](#size)
- [Platform Limits](#platform-limits)
- [Out of Scope](#out-of-scope)
- [API Reference](#api-reference)
- [Contributing](#contributing)
- [License](#license)

## Why tinyimg

- **No per-transformation cost.** A Worker serving many derivatives of many images pays per
  derivative through Images. tinyimg does the same work for CPU time.
- **Workers-native.** One wasm module with zero imports, instantiated at worker startup. Nothing
  depends on Node, and there is no filesystem access, no subrequest and no binding.
- **Lazy by design.** Operations do not run when called. They append to a plan, and the plan decides
  what to decode before decoding it; see [The Planner](#the-planner).
- **A drawing surface.** Shapes, polygons, gradients, compositing with blend modes, and a display
  list with a transform stack, which is the Canvas-shaped API the runtime does not have.
- **Nothing bundled that you might not use.** Fonts, color profiles and cascades load at runtime.
  The module ships no font data.
- **TypeScript-first.** The types are the contract and the documentation.

## Compared With Cloudflare Images

Both do the same job in different places. Images Transformations runs on Cloudflare's own pipeline and
bills per transformation. tinyimg runs in your Worker's isolate and bills as CPU time, which on the
Workers Paid plan is a resource the $5 subscription already includes 30 million milliseconds of.

|                       | Cloudflare Images                       | tinyimg                              |
| --------------------- | --------------------------------------- | ------------------------------------ |
| Charge per transform  | $0.50 / 1,000 unique, 5,000 free / mo   | none                                 |
| What you pay instead  | nothing else                            | Worker CPU time                      |
| Billing granularity   | unique source and flags, once per month | every invocation that is not cached  |
| Subrequest or binding | required                                | none                                 |
| AVIF output           | yes                                     | no                                   |
| Runs on Workers Free  | 5,000 transforms / mo                   | sources up to roughly 2 MP           |
| Raw pixel access      | no                                      | yes                                  |
| Shapes and gradients  | no                                      | yes                                  |
| Text                  | one string, one font, one size          | layout, wrapping, alignment, metrics |

### Cost

Past the included CPU, Workers bills $0.02 per million CPU milliseconds, so a transformation costs
its own compute multiplied by that. Three routes to the same 800 px page:

| Route       | `effort` | Compute | Per million | Within the included CPU |
| ----------- | -------- | ------: | ----------: | ----------------------: |
| 800 px WebP | fancy    | 41.6 ms |       $0.83 |                 721,000 |
| 800 px WebP | fast     | 33.4 ms |       $0.67 |                 898,000 |
| 800 px JPEG | either   | 20.8 ms |       $0.42 |               1,442,000 |

Against **$500 per million** for Images at $0.50 per 1,000. The absolute figures are small whichever
route you take. What the spread buys is CPU headroom, and headroom is what the Free plan rations.

Images bills a unique source and flag combination **once per calendar month** however often it is
served, so a small set of derivatives under heavy traffic is already cheap there. tinyimg charges for
every invocation that runs, so put [Workers Cache](https://developers.cloudflare.com/workers/cache/)
in front of it and a hit costs no CPU at all. tinyimg wins on many distinct derivatives, Images wins
on few you have not cached.

### Speed

Median wall clock in bun on darwin-arm64 over `sf-24.jpg` at 1835x1032. This is not Worker CPU time
on Cloudflare's hardware, so profile your own images.

| Request                | `effort: 'fancy'` | `effort: 'fast'` |
| ---------------------- | ----------------: | ---------------: |
| 200 px cover, JPEG q80 |            7.2 ms |           6.9 ms |
| 200 px cover, WebP q80 |            8.3 ms |           7.7 ms |
| 400 px cover, JPEG q80 |           11.7 ms |          11.8 ms |
| 400 px cover, WebP q80 |           17.2 ms |          14.9 ms |
| 800 px cover, JPEG q80 |           20.8 ms |          20.8 ms |
| 800 px cover, WebP q80 |           41.6 ms |          33.4 ms |

`effort` moves WebP at every size and leaves JPEG output flat, because the WebP encoder has a 4x4
prediction search to bound and the JPEG encoder has no equivalent.

JPEG's own lever is on the decode side, and a thumbnail cannot show it. Replicating chroma instead
of filtering it is worth 1.11x to 1.25x on a full decode, but the saving scales with output samples
while a reduced request's cost is dominated by the entropy decode, which the source fixes. On a
4:2:0 source it measures 1.00x at 200 px and 1.07x at the source's own extent. `sf-24.jpg` cannot
show it at any size, being 4:4:4 with no chroma to upsample.

A request the source already satisfies is 0.01 ms and a plan decision with no pixels decoded is
under 0.1 ms; neither depends on effort.

The Workers Free plan allows 10 ms of CPU per request and does not let a caller pay for more, so a
request either fits or fails. Workers Paid defaults to 30 seconds, raised to 5 minutes with
`limits.cpu_ms`, and nothing above comes close to it.

**What fits Free is decided by the source, not by the size you ask for.** Every row below produces
the same 200 px thumbnail from the same eighth-scale decode, and every one of them decodes 1.57% of
its source's samples. The spread is the entropy decode, which no output size can reduce.

| Source    | Megapixels | 200 px wide, JPEG |
| --------- | ---------: | ----------------: |
| 320x180   |       0.06 |           2.20 ms |
| 1835x1032 |       1.89 |           6.80 ms |
| 1920x1250 |       2.40 |          15.86 ms |
| 5000x4000 |      20.00 |          25.41 ms |

So roughly two megapixels is the Free ceiling for a synchronous transformation, whatever the output
size. Past that the artifact has to be computed once and cached; see [Caching](#caching).

Decode is 1.2x to 2.1x behind libjpeg, libpng and libwebp, which have SIMD implementations of their
hot kernels and tinyimg has none. WebP and PNG encode are slower still.
[`TECHNICAL_REPORT.md`](https://github.com/gmitch215/tinyimg/blob/master/TECHNICAL_REPORT.md) has
the per-stage profile and the comparison against `@jsquash`.

### Gaps

- **No AVIF output.** Images serves AVIF through `format=auto`. tinyimg parses the container and
  reports its dimensions, and encodes WebP instead, which lands 25-35% below JPEG at matched quality.
  See [Out of Scope](#out-of-scope).
- **No animation.** Both decode a first frame; neither re-times or composes one.
- **You own the caching.** Images caches derivatives at the edge as a product feature. tinyimg gives
  you the key and leaves the cache to you; see [Caching](#caching).

### What tinyimg Adds

Images covers resize, fit, gravity, the standard adjustments, image overlays with Porter-Duff
compositing, and a rasterized text string. Past that:

- **A drawing surface.** Lines, rectangles, rounded rectangles, circles, ellipses, polygons with both
  fill rules, linear and radial gradients, and a display list with a transform matrix stack.
- **Text as layout.** Wrapping, alignment, text boxes and measurement, against Images' single styled
  string.
- **Raw pixels.** `decode` and `Image.pixels` hand back samples, so anything not in the operation set
  is still reachable.
- **Effects and warps.** Sobel, emboss, morphology, median, dither, halftone, duotone, split-tone,
  color blindness simulation, curves, channel mixer, shear, perspective, barrel and swirl.
- **Face boxes, not just face gravity.** `detectFaces` returns rectangles, and `tiny_image_blur_faces`
  and `tiny_image_pixelate_faces` act on them.
- **Analysis.** Histogram, dominant color, palette extraction and a perceptual hash.

The chainable `Image` class covers the transformation set. Drawing, text, the effects and the
analysis helpers are module exports, reached through `tinyimg.exports` with the same names the
headers use.

## Install

```sh
npm install @gmitch215/tinyimg
```

```sh
bun add @gmitch215/tinyimg
```

## Getting Started

The module has to arrive already compiled. workerd refuses `WebAssembly.Module(bytes)`, so importing
the wasm file is what makes it work: the runtime compiles it at worker startup.

```ts
import wasm from '@gmitch215/tinyimg/tinyimg.wasm';
import { TinyImgModule, transform } from '@gmitch215/tinyimg';

const tinyimg = TinyImgModule.load(wasm);

export default {
	async fetch(request: Request): Promise<Response> {
		const source = await fetch('https://example.com/photo.jpg');

		const thumb = await transform(tinyimg, source, {
			width: 400,
			height: 400,
			fit: 'cover',
			format: 'webp',
			quality: 80
		});

		return thumb.response({ 'cache-control': 'public, max-age=86400' });
	}
};
```

Under node, bun or a browser, compile from bytes instead:

```ts
import { readFileSync } from 'node:fs';
import { TinyImgModule } from '@gmitch215/tinyimg';

const tinyimg = await TinyImgModule.loadBytes(readFileSync('tinyimg.wasm'));
```

Input accepts a `Uint8Array`, an `ArrayBuffer`, a `Blob`, a `Response` or a `ReadableStream`. Results
carry `.bytes()`, `.blob()`, `.response()` and `.dataUrl()`.

## Transformations

`transform` takes one option object. The names mirror Cloudflare Images' own parameters.

| Option                                          | Type                                   | What it does                                                          |
| ----------------------------------------------- | -------------------------------------- | --------------------------------------------------------------------- |
| `width`, `height`                               | `number`                               | Target extent. One alone keeps the aspect ratio.                      |
| `fit`                                           | `Fit`                                  | How an aspect mismatch is absorbed. Read when both extents are given. |
| `gravity`                                       | `Gravity`                              | Which part a crop keeps, or where a pad puts the image.               |
| `filter`                                        | `ResampleFilter`                       | Weights the resample reads through.                                   |
| `crop`                                          | `Rect`                                 | Source rectangle, taken before anything else.                         |
| `dpr`                                           | `number`                               | Multiplies `width` and `height`.                                      |
| `rotate`                                        | `0 \| 90 \| 180 \| 270`                | Quarter turns clockwise.                                              |
| `flip`                                          | `'horizontal' \| 'vertical' \| 'both'` | Which axes to mirror.                                                 |
| `brightness`, `contrast`, `saturation`, `gamma` | `number`                               | 1 is unchanged.                                                       |
| `hue`                                           | `number`                               | Degrees.                                                              |
| `grayscale`, `invert`                           | `boolean`                              |                                                                       |
| `blur`                                          | `number`                               | Gaussian radius in pixels.                                            |
| `sharpen`                                       | `number`                               | Unsharp mask amount.                                                  |
| `trim`                                          | `boolean \| number`                    | Trims a uniform border, at this tolerance.                            |
| `background`                                    | `Color`                                | Fills whatever a pad or a rotation leaves empty.                      |
| `format`                                        | `ImageFormat`                          | Container to encode as. Defaults to the source's own.                 |
| `quality`                                       | `number`                               | 1 through 100, for a lossy format.                                    |
| `lossless`, `progressive`                       | `boolean`                              | Where the format has the mode.                                        |
| `metadata`                                      | `'keep' \| 'none'`                     | Whether EXIF survives the encode. Defaults to `keep`.                 |

The options apply in a fixed order, exported as `TRANSFORM_ORDER`: crop, resize or fit, rotate, flip,
the color adjustments, blur, sharpen, trim. An option object has no order of its own, so two callers
writing the same keys get the same image.

### Fit Modes

| Mode             | Absorbs the mismatch by        | Scale may    |
| ---------------- | ------------------------------ | ------------ |
| `scale-down`     | leaving it                     | only fall    |
| `contain`        | leaving it                     | rise or fall |
| `scale-up`       | leaving it                     | only rise    |
| `cover`          | cropping                       | rise or fall |
| `crop`           | cropping                       | only fall    |
| `fill`           | cropping                       | only rise    |
| `aspect-crop`    | cropping                       | neither      |
| `aspect-cover`   | cropping to the target's shape | neither      |
| `pad`            | padding                        | rise or fall |
| `aspect-contain` | padding to the target's shape  | neither      |
| `stretch`        | distorting                     | rise or fall |

### Gravity

The nine fixed positions are `center`, `north`, `south`, `west`, `east`, `north-west`, `north-east`,
`south-west` and `south-east`.

Two are computed from the image. `auto` weights every tile by local detail and centers on the
centroid, so a photograph with one sharp subject on a soft background focuses on the subject. `face`
runs the detector and centers on what it finds, falling back to `auto` when no cascade is loaded or
nothing is found.

## Chaining

`Image` is the same planner with a method per operation, for when you want two encodings of one
transformation or an operation the option object does not cover.

```ts
import { Image } from '@gmitch215/tinyimg';

using image = await Image.open(tinyimg, request.body);

image.crop(400, 200, 900, 600).resize(300, 200).brightness(1.2).sharpen(1);

const webp = await image.bytes('webp', { quality: 80 });
const png = await image.bytes('png');
```

The handle lives in the module's memory, so `using` is the shortest correct way to hold one. Without
it, call `dispose()` in a `finally`. Encoding does not consume the plan.

`pixels()` hands back the raw samples for a caller who wants them rather than a container.

## The Planner

Nothing runs until the output is asked for. Operations append to a fixed-capacity list, and the plan
runs once:

```ts
image.crop(2000, 1500, 500, 500).resize(100, 100);
image.brightness(1.2).contrast(1.1).saturation(0.8).gamma(2.2);
```

- The output needs a 500x500 source rectangle out of 16 megapixels, so the decoder is asked for that
  rectangle and nothing else.
- A 100x100 output from a 500x500 source needs no detail above a quarter, so JPEG decodes at 1/4
  through a reduced IDCT.
- Brightness, contrast and gamma become one 256-entry table; saturation becomes one matrix.
- Six traversals become one pass over 10,000 output pixels.
- An added `brightness(1.0)` never runs.

`decide()` reports all of it without producing the image:

```ts
using image = await Image.open(tinyimg, source);
image.crop(400, 200, 900, 600).resize(300, 200);

const decided = image.decide();
// { region: { x: 400, y: 200, width: 900, height: 600 }, scale: 4,
//   decoded: { width: 225, height: 150 }, output: { width: 300, height: 200, channels: 3 },
//   eliminated: 0, collapsed: 0, colorStages: 0, passes: 1,
//   kernels: ['region', 'scaled', 'resample'] }
```

On that chain the planner is worth 3.63x against running the same operations one pass each.

## Codec Support

| Format     | Decode                                                                          | Encode                                                            |
| ---------- | ------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| PNG        | 8 and 16 bit, gray/palette/truecolor with and without alpha, Adam7              | hash-chain LZ77, static and dynamic Huffman, the five filters     |
| JPEG       | baseline and progressive, 4:4:4 / 4:2:2 / 4:2:0, CMYK and YCCK, restart markers | quality-scaled tables, optimized Huffman, progressive scan script |
| WebP       | VP8 lossy and VP8L lossless                                                     | both modes                                                        |
| GIF        | LZW, local and global palettes, interlace, transparency                         | median-cut palette with Floyd-Steinberg dithering                 |
| TIFF       | strips, uncompressed / PackBits / LZW / Deflate, both byte orders               | uncompressed and PackBits                                         |
| BMP        | uncompressed and RLE8                                                           | uncompressed                                                      |
| AVIF, HEIF | container only; `probe` answers and decode reports a specific error             | no                                                                |

JPEG carries true DCT-domain scaled decode at 1/2, 1/4 and 1/8, and region decode that Huffman-scans
past blocks outside the box without transforming them. PNG, GIF, TIFF and BMP row-skip and
column-skip.

`probe` reads headers only, for every format including the ones the library cannot decode:

```ts
const info = await tinyimg.probe(source);
// { width, height, frames, format, channels, bitDepth, hasAlpha, progressive }
```

## Drawing

Pixels, lines with thickness, rectangles, rounded rectangles, circles, ellipses, and polygons filled
by scanline with either the even-odd or the nonzero rule. Linear and radial gradients, borders,
per-channel color replacement, and image compositing with opacity, tiling and edge offsets.

Compositing carries the CSS separable blend modes: multiply, screen, overlay, darken, lighten,
hard-light, soft-light, difference, exclusion, add and subtract, composed in premultiplied form so a
stack of partly transparent layers ends up as transparent as it should be.

A display list keeps shapes symbolic behind a 2x3 affine stack until one rasterization, and drops a
shape that falls outside the target or that a later opaque shape completely covers.

## Effects

Roughly ninety, reaching the planner through three generic operations, so they compose into one
matrix and one table wherever they can.

- **Tone and color.** negate, grayscale, black and white, colorize, tint, posterize, threshold,
  solarize, duotone, split-tone, exposure, fill light, temperature and tint white balance, vibrance,
  levels, curves from control points, color balance by band, channel mixer, per-channel gain,
  colorblind simulation and assist, and nine named presets.
- **Spatial.** unsharp mask, clarity, sobel, emboss, pixelate, median despeckle, morphology, outline,
  motion / radial / zoom blur, tilt-shift, region blur, drop shadow, glow, gradient fade, noise, film
  grain, ordered dither, halftone, chromatic aberration, scanlines.
- **Auto-correction.** auto brightness, contrast, color, levels and gamma; improve; shadow and
  highlight recovery; dehaze by dark-channel prior.
- **Geometry.** shear, quad and perspective distort by inverse homography, arc warp, barrel and
  pincushion, swirl, polar, corner radius.
- **Analysis.** histogram, dominant color, palette extraction, average color, and a 64-bit
  perceptual hash with a Hamming-distance helper.

## Text

TrueType and OpenType with `glyf` outlines, plus PSF and BDF bitmap faces, dispatched from the magic
bytes. Quadratic flattening and a scanline fill with four vertical subsamples and exact horizontal
coverage.

```ts
const font = await env.BLOBS.get('fonts/DejaVuSans.ttf');
if (font) await tinyimg.loadBlob('font', 'sans', font.body);
```

`tiny_image_draw_text`, `tiny_image_draw_text_box` with wrapping and alignment, and `tiny_text_measure`
share one layout walk, so measuring and then drawing at the measured position lands where the
measurement said. Kern pairs are applied when the face carries a `kern` table.

100 characters at 16 px cost 0.156 ms.

## Face Detection

An LBP cascade over an integral image, multi-scale, with union-find grouping. The cascades are
OpenCV's, repacked from XML to a flat binary.

```ts
await tinyimg.loadBlob('cascade', 'frontal', frontalBytes);
await tinyimg.loadBlob('cascade', 'profile', profileBytes);

const faces = await tinyimg.detectFaces(source);
// [{ x, y, width, height, neighbors }]
```

Every resident cascade runs and the results are grouped together, so a frontal and a profile cascade
find both kinds of face and a face that fires both is one box. Detections match OpenCV's own detector
within a pixel on identical pixels.

Feeds `gravity: 'face'`. With no cascade loaded, `detectFaces` reports a `TinyImgBlobError` and
`gravity: 'face'` falls back to `auto`.

## Color Management

ICC matrix-and-TRC profiles: the `rXYZ` / `gXYZ` / `bXYZ` primaries, `curv` and parametric `para`
tone curves, and Bradford chromatic adaptation folded into the matrix. Conversion between a tagged
profile and sRGB agrees with ImageMagick at 49 dB.

LUT-based A2B and B2A profiles, which is mostly CMYK printer profiles, are rejected with a specific
error rather than approximated. The matrix and TRC path covers sRGB, Display P3, Adobe RGB and
Rec.2020, which is what web images carry.

## Budget and Effort

Three things let a caller work inside a CPU limit instead of discovering it.

**A request the source already satisfies costs a header read.** `transform` compares the request
against the source's header first, and hands the original bytes back when there is nothing to do. A
600 px bound on a 400 px WebP decodes nothing.

**`effort: 'fast'` bounds the work rather than the quality.** Quality says what the output should
look like; effort says how hard to work to get there. It applies to the decode, the resize and the
encode, and each part gives up something measured:

| Stage  | What `fast` does                            | Speed          | Quality         |
| ------ | ------------------------------------------- | -------------- | --------------- |
| Decode | WebP skips deblocking                       | 1.53x          | 46.8 dB         |
| Decode | JPEG replicates chroma instead of filtering | 1.11x to 1.25x | 43.6 to 59.5 dB |
| Resize | Bilinear instead of Catmull-Rom, enlarging  | 3.2x           | 45.7 dB         |
| Encode | WebP bounds the 4x4 prediction search       | 1.18x to 1.45x | within 0.05 dB  |

**Only a lossy format has anything to trade.** A lossless one defines its pixels exactly, so every
step is needed to produce them: PNG, GIF, TIFF and lossless WebP decode identically at either
effort, and so does a 4:4:4 JPEG, which has no chroma to upsample. A reduction also keeps its
filter, because an area average is already both the cheapest and the right answer; only enlarging
has a filter to step down.

A filter you name is never substituted, and neither is a format. Effort decides what the library
left open, not what you asked for.

End to end, on requests through the whole pipeline:

| Request                       | `fancy` |  `fast` |       |
| ----------------------------- | ------: | ------: | ----: |
| 200 px, WebP source, WebP out | 18.5 ms | 13.5 ms | 1.38x |
| 800 px, WebP source, WebP out | 54.6 ms | 42.2 ms | 1.29x |
| 800 px, JPEG source, WebP out | 42.2 ms | 34.0 ms | 1.24x |
| 800 px, JPEG source, JPEG out | 21.6 ms | 21.5 ms | 1.01x |

A WebP source gains most, because it is the only one where both the decode and the encode have
something to give. The last row is the contract working rather than a failure: a 4:4:4 JPEG decoded
and re-encoded as JPEG has no deblocking pass, no chroma to replicate and no prediction search, so
there is nothing for effort to spend.

Encoded size can go either way under `fast`, because the per-subblock choice minimizes prediction
error rather than rate: a diagonal mode can win on error and then cost more bits to code. Flat-color
illustrations pay the most, which is what those modes are for.

**`decide().estimateMs` prices a plan before it runs, and `budgetMs` acts on it.** The encoders differ
by roughly a factor of five per sample, so `format: 'auto'` with a budget takes WebP when it fits and
JPEG when it does not:

```ts
const result = await transform(tinyimg, source, {
	width: 400,
	format: 'auto',
	budgetMs: 7,
	effort: 'fast'
});

result.format; // 'webp' if it fits inside 7 ms, otherwise 'jpeg'
```

A format you name is never substituted, however far over budget it is. The estimate is accurate to
about 20% on the machine its rates were measured on, so give a budget below the limit rather than at
it, and use [`measure`](#work-counters) when you need what a request actually cost.

## Caching

A cache hit costs no CPU at all, which is how a request too expensive to serve synchronously still
gets served: it is computed once. `artifactKey` builds the canonical key, identifying the source by
the hash of its bytes so the same picture from two places is one artifact:

```ts
import { artifactKey, transform } from '@gmitch215/tinyimg';

const key = await artifactKey(source, options);
const hit = await caches.default.match(key);

if (hit) return hit;

const out = await transform(tinyimg, source, options);
const response = out.response({ 'cache-control': 'public, max-age=31536000, immutable' });

ctx.waitUntil(caches.default.put(key, response.clone()));
return response;
```

## Work Counters

Every reduction in this library is a claim that some work does not happen, and a label is not
evidence. `measure` returns what the module actually did:

```ts
const { work } = await tinyimg.measure(() => transform(tinyimg, source, { width: 200 }));

work.samplesPerTransform; // 1 at an eighth scale, 4 at a quarter, 64 at full
work.decodedSamples / work.sourceSamples; // what the reduction was worth
work.filtered; // macroblocks a region did not skip
```

`samplesPerTransform` is the one that matters most: a decode transforming whole blocks and averaging
them away reads 64 whatever its name is.

## Blobs

Three kinds of data load at runtime rather than shipping in the module: fonts, ICC profiles and
detection cascades. Two delivery modes, and no code change between them.

```ts
// from a bucket: one subrequest, no bundle bytes
const cascade = await env.BLOBS.get('cascades/lbp-frontalface.bin');
if (cascade) await tinyimg.loadBlob('cascade', 'frontal', cascade.body);
```

```ts
// as a wrangler Data module: bundle bytes, no latency
import cascade from './lbp-frontalface.bin';
await tinyimg.loadBlob('cascade', 'frontal', cascade);
```

```jsonc
{
	"r2_buckets": [{ "binding": "BLOBS", "bucket_name": "tinyimg-blobs" }]
}
```

Eight blobs can be resident at once. `freeBlob` releases one and `freeBlobs` releases all. A cascade
is parsed at load, so a bad one is a startup failure rather than a search that finds nothing.

## Error Handling

Every failure carries the module's own error code and its stable name, so a caller branches on the
code rather than on a message.

```ts
import { TinyImgBlobError, TinyImgFormatError } from '@gmitch215/tinyimg';

try {
	return (await transform(tinyimg, source, { width: 400 })).response();
} catch (error) {
	if (error instanceof TinyImgFormatError) return new Response('unsupported image', { status: 415 });
	if (error instanceof TinyImgBlobError) return new Response('font not loaded', { status: 500 });

	throw error;
}
```

| Class                  | Covers                                                                  |
| ---------------------- | ----------------------------------------------------------------------- |
| `TinyImgArgumentError` | a null argument, a value out of range, a rectangle outside the image    |
| `TinyImgFormatError`   | an unrecognized format, or a variant this build cannot read             |
| `TinyImgDataError`     | a malformed, truncated or inconsistent bitstream                        |
| `TinyImgMemoryError`   | the allocator refused a request that was within budget                  |
| `TinyImgTooLargeError` | the image is past the budget, so a smaller decode is the remedy         |
| `TinyImgBlobError`     | a font, profile or cascade nobody loaded                                |
| `TinyImgPlanError`     | the plan is full, or holds an operation it cannot combine               |
| `TinyImgLoadError`     | the module is not tinyimg, or its ABI is one this wrapper does not know |

## Size

Small size is the primary goal, ahead of speed and then memory. The published package is 138.7 kB,
of which the wasm module is 90.4 kB gzipped and the wrapper is 7.8 kB. `bun run size` prints the
breakdown.

Every codec can be compiled out, so a caller who only handles PNG can link a module with nothing
else in it. `--gc-sections` then drops the implementation along with its registry entry.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32.cmake \
  -DCMAKE_C_FLAGS="-DTINYIMG_NO_WEBP -DTINYIMG_NO_TIFF -DTINYIMG_NO_GIF"
cmake --build build
```

That build is 65.4 kB gzipped against 90.4, and `tinyimg.features` reports the three as absent, so a
caller can check before offering a format rather than calling and handling a failure.

Build it into its own directory and `bin/tinyimg.wasm` is still where it lands, because that path is
absolute in `CMakeLists.txt`. Run `bun run build:wasm` afterwards to put the full module back.

## Platform Limits

- **The module must arrive compiled.** workerd refuses `WebAssembly.Module(bytes)` with "Wasm code
  generation disallowed by embedder". Import the `.wasm` file; `loadBytes` is for node, bun and the
  browser and throws on Workers.
- **The Workers Free plan allows 10 ms of CPU per request**, which is not configurable and is below
  the cost of most transformations; see [Speed](#speed). Workers Paid defaults to 30 seconds and goes
  to 5 minutes with `limits.cpu_ms`. A Worker over the limit returns error 1102.
- **32 MiB per decoded image**, inside 64 MiB of linear memory, with a 16 megapixel ceiling on top
  that only grayscale reaches. The byte cap is the one that binds, because an allocation is bytes: it
  allows 11.2 megapixels of RGB and 8.4 of RGBA. Past either, the library reports
  `TinyImgTooLargeError`, whose remedy is a smaller decode rather than more memory. A larger source
  is still usable; that is what a scaled or region decode is for, and the planner reaches for one on
  its own.
- **Progressive JPEG cannot stream a region.** Successive approximation needs the whole coefficient
  plane before any pixel is final, so a region request on a progressive file decodes the plane and
  then crops. `probe` reports `progressive` so a caller can tell in advance.
- **One module owns one linear memory.** Load more than one for isolation between concurrent
  requests; sharing one is normally what you want, and the allocator returns every buffer at the end
  of a call.
- **Animation is first-frame only.** GIF and WebP animations decode their first frame, and `probe`
  reports the frame count. A request with nothing to do passes the source through whole, so serving
  an animation unchanged keeps every frame; a request that has real work sets `flattened` on the
  result, because there is no animated encoder to write the rest back. Transforming all 57 frames of
  an 800x600 GIF to a 200 px thumbnail measures 335 ms, which is why the first frame is the default
  rather than a limitation waiting to be lifted.
- **A cascade is scale-dependent.** No single search setting finds every face: a frontal face is
  found at a moderate reduction and lost at full resolution, and a side-facing one can be the
  reverse. `detectFaces` picks a default that suits frontal faces; the C entry point
  `tiny_image_detect_faces_ex` searches exactly what it is told.
- **`-msimd128` currently buys nothing.** No code path uses SIMD intrinsics, so the flag only
  enables an autovectorizer that `-Oz` disables. Measured at 0.97-1.02x on three operations.

## Out of Scope

- **AVIF and HEIF decode and encode.** An AV1 intra decoder is 8-10k lines against 8-10k for the rest
  of the library, decode runs 4-12x the JPEG path per pixel, and encode needs a rate-distortion
  search that costs seconds per image in wasm. The container parse ships so `probe` answers. Codecs
  register through a table, so a second module can carry AV1 for callers who ask.
- **JPEG XL.** No browser ships it by default.
- **Animation editing.** Composing or re-timing frames is a different product.
- **LUT-based ICC profiles.** Rejected with a specific error rather than approximated.
- **AI-backed effects.** No model, and a Worker is the wrong place for one.

## API Reference

Generated documentation lives at [tinyimg.gmitch215.dev](https://tinyimg.gmitch215.dev): the
TypeScript surface under `/typedoc`, and the C headers alongside it.

## Contributing

`CLAUDE.md` carries the build and test commands, the codec contract, the planner contract for adding
an operation, and the conventions. `TECHNICAL_REPORT.md` carries the measurements and the approaches
they refuted.

```sh
bun install
bun run build
bun run test
```

## License

MIT
