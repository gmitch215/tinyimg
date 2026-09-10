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
  - [Setting a Run](#setting-a-run)
  - [Mixed Styles](#mixed-styles)
  - [What the Face Says](#what-the-face-says)
  - [Open Graph Cards](#open-graph-cards)
- [Face Detection](#face-detection)
- [Color Management](#color-management)
- [Reading the dB Figures](#reading-the-db-figures)
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
- **Works off a bare instantiate.** A Latin face, four ICC profiles and two face cascades are
  compiled in, so text, color conversion and face detection need no blob and no subrequest. A wider
  glyph set or a cascade of your own loads at runtime.
- **TypeScript-first.** The types are the contract and the documentation.

## Compared With Cloudflare Images

Both do the same job in different places. Images Transformations runs on Cloudflare's own pipeline and
bills per transformation. tinyimg runs in your Worker's isolate and bills as CPU time, which on the
Workers Paid plan is a resource the $5 subscription already includes 30 million milliseconds of.

|                       | Cloudflare Images                       | tinyimg                             |
| --------------------- | --------------------------------------- | ----------------------------------- |
| Charge per transform  | $0.50 / 1,000 unique, 5,000 free / mo   | none                                |
| What you pay instead  | nothing else                            | Worker CPU time                     |
| Billing granularity   | unique source and flags, once per month | every invocation that is not cached |
| Subrequest or binding | required                                | none                                |
| AVIF output           | yes                                     | yes                                 |
| Runs on Workers Free  | 5,000 transforms / mo                   | see [Speed](#speed)                 |
| Raw pixel access      | no                                      | yes                                 |
| Shapes and gradients  | no                                      | yes                                 |
| Text measurement      | no                                      | yes                                 |
| Text wrapping         | no                                      | yes                                 |
| Text stroke or shadow | no                                      | yes                                 |
| Mixed styles in a run | no                                      | yes                                 |
| Ligatures and GPOS    | no                                      | yes                                 |
| Variable font axes    | no                                      | yes                                 |

Images renders text as of 2026: `.text(content, options)` takes a font by URL (TrueType, OpenType
or WOFF, up to 20 MB), a colour and a size, up to 1,000 characters and 4096 x 4096 pixels, positioned
through the `draw` array with Porter-Duff compositing. The row that used to read "one string, one
font, one size" understated it.

What it does not do is give the caller any measurements back, so nothing can be positioned relative
to the text. That is the difference the text rows above describe: wrapping a caption to a box,
centring a line inside a plate, or fitting a title to a width all need the metrics first, and
`measureText` is the entry point Images has no equivalent of. The rest follows from having a layout
rather than a string: an outline and a shadow so a headline survives being drawn over a photograph,
several styles on one baseline, and one variable file serving every weight.

### Cost

Past the included CPU, Workers bills $0.02 per million CPU milliseconds, so a transformation costs
its own compute multiplied by that. Four routes to the same 800 px page:

| Route       | `effort` | Compute | Per million | Within the included CPU |
| ----------- | -------- | ------: | ----------: | ----------------------: |
| 800 px JPEG | either   | 16.7 ms |       $0.33 |               1,796,000 |
| 800 px WebP | fast     | 22.4 ms |       $0.45 |               1,339,000 |
| 800 px WebP | fancy    | 25.5 ms |       $0.51 |               1,176,000 |
| 800 px AVIF | either   |   45 ms |       $0.90 |                 666,000 |

Against **$500 per million** for Images at $0.50 per 1,000. The absolute figures are small whichever
route you take. What the spread buys is CPU headroom, and headroom is what the Free plan rations.
AVIF costs the most compute and produces the smallest file: 17,860 bytes against WebP's 20,572 and
JPEG's 21,561 on the same 800 px request.

Images bills a unique source and flag combination **once per calendar month** however often it is
served, so a small set of derivatives under heavy traffic is already cheap there. Since July 2026
the Images binding bills the same way rather than per call, and `.info()` is not billed at all.
tinyimg charges for every invocation that runs, so put
[Workers Cache](https://developers.cloudflare.com/workers/cache/) in front of it and a hit costs no
CPU at all.

The split follows from that. Images is cheaper for a fixed set of derivatives you serve repeatedly.
tinyimg is cheaper when the derivatives are unbounded, and the clearest case is text that varies per
request: a social card carrying a page title is a unique transformation per page, so it bills every
page and never reuses one.

### Speed

Fastest of 15 runs in bun on darwin-arm64 over `sf-24.jpg` at 1835x1032. The minimum rather than the
median, because a busy machine can only move a timing upward. This is not Worker CPU time on
Cloudflare's hardware, so profile your own images.

| Request                | `effort: 'fancy'` | `effort: 'fast'` |
| ---------------------- | ----------------: | ---------------: |
| 200 px cover, JPEG q80 |            5.1 ms |           5.0 ms |
| 400 px cover, JPEG q80 |            9.1 ms |           9.2 ms |
| 800 px cover, JPEG q80 |           16.7 ms |          16.8 ms |
| 800 px cover, WebP q80 |           25.5 ms |          22.4 ms |
| 800 px cover, AVIF q80 |             45 ms |            45 ms |

`effort` moves WebP and leaves JPEG and AVIF output flat, because the WebP encoder has a 4x4
prediction search to bound and neither of the others has an equivalent. A PNG output is the other
one it moves; see [Budget and Effort](#budget-and-effort).

JPEG's own lever is on the decode side, and a thumbnail cannot show it. Replicating chroma instead
of filtering it is worth 1.11x to 1.25x on a full decode, but the saving scales with output samples
while a reduced request's cost is dominated by the entropy decode, which the source fixes. On a
4:2:0 source it measures 1.00x at 200 px and 1.07x at the source's own extent. `sf-24.jpg` cannot
show it at any size, being 4:4:4 with no chroma to upsample.

A request the source already satisfies is 0.01 ms and a plan decision with no pixels decoded is
under 0.1 ms; neither depends on effort.

The Workers Free plan allows 10 ms of CPU per request and does not let a caller pay for more, so a
request either fits or fails. Workers Paid defaults to 30 seconds, raised to 5 minutes with
`limits.cpu_ms`, and nothing above comes close to it. Every millisecond below is measured on
darwin-arm64; Workers is 1.9x slower at the median and up to 4.7x on the cheapest requests, which are
the ones nearest the limit, so scale before comparing.

**What fits Free is decided by the source, not by the size you ask for.** Every row below produces
the same 200 px thumbnail from the same eighth-scale decode. The spread is the entropy decode, which
no output size can reduce.

| Source    | Megapixels | Compressed | 200 px wide, JPEG |
| --------- | ---------: | ---------: | ----------------: |
| 250x187   |       0.05 |      23 KB |            2.2 ms |
| 2272x3000 |       6.82 |     215 KB |            8.0 ms |
| 2308x3000 |       6.92 |     435 KB |           12.1 ms |
| 1920x1280 |       2.46 |     828 KB |           15.4 ms |
| 3600x2700 |       9.72 |    2721 KB |             53 ms |

**Compressed bytes predict it and megapixels do not.** The second and fourth rows make the point on
their own: 2272x3000 carries 2.8x the pixels of 1920x1280, a quarter of the bytes, and finishes in
half the time. Across twelve JPEGs, milliseconds per megapixel spread 7.1x while milliseconds per
megabyte spread 1.95x at this scale. The practical rule for a 200 px thumbnail is roughly **480 KB of
compressed source**, whatever its dimensions.

The estimate `decide()` returns still prices against source samples and is wrong by up to 12.8x for
that reason, so treat `budgetMs` as a coarse guard rather than a promise. Past the ceiling the
artifact has to be computed once and cached; see [Caching](#caching).

A progressive JPEG is the exception worth knowing about. At an eighth scale the transform reads the
DC term alone, so the scans that carry only high-frequency detail are stepped over rather than
decoded: `road.jpg` at 160 px goes from 28.5 ms to **6.5 ms**, byte-identical, which moves that
request from outside the Free budget to inside it.

JPEG and PNG decode are slightly ahead of libjpeg and libpng, at 1.06x-1.09x and 1.03x-1.08x; WebP
decode is 0.68x-0.72x behind libwebp. JPEG and WebP encode are ahead at 3.8x-4.0x and 1.31x-1.38x.
PNG encode is 12x behind, and that row is an operating-point difference as much as a speed one, since
it produces a file 47% smaller; `compression: 'fast'` closes most of it at a stated size cost.
[`TECHNICAL_REPORT.md`](https://github.com/gmitch215/tinyimg/blob/master/TECHNICAL_REPORT.md) has
the per-stage profile and the comparison against `@jsquash`.

### Gaps

- **AVIF encode has no partition search.** Decode is complete and matches `dav1d` exactly. The
  encoder uses fixed 8x8 blocks and picks the luma mode from four candidates by absolute
  difference, so its files run 1.1x to 1.3x libaom's size at matched quality. A partition search is
  where most of AV1's compression lives.
- **No 10 or 12 bit AVIF.** The reconstruction is 8 bits a sample end to end, so an HDR AVIF is
  refused rather than truncated. 4:2:2 is refused too, for a reason in the format: the
  specification's own subsampled-size table has no answer for a tall block on such a plane.
- **No animation.** Both decode a first frame; neither re-times or composes one.
- **You own the caching.** Images caches derivatives at the edge as a product feature. tinyimg gives
  you the key and leaves the cache to you; see [Caching](#caching).

### What tinyimg Adds

Images covers resize, fit, gravity, the standard adjustments, image overlays with Porter-Duff
compositing, and a rasterized text string. Past that:

- **A drawing surface.** Lines, rectangles, rounded rectangles, circles, ellipses, polygons with both
  fill rules, linear and radial gradients, and a display list with a transform matrix stack.
- **Text as layout.** Wrapping, alignment, justification, vertical alignment, ellipsis, per-line
  metrics, outlines, shadows, mixed styles on one baseline, fallback chains, ligatures, GPOS kerning
  and variable font axes, against Images' single styled string. `og` renders a whole Open Graph card
  in one call.
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

| Format | Decode                                                                          | Encode                                                            |
| ------ | ------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| PNG    | 8 and 16 bit, gray/palette/truecolor with and without alpha, Adam7              | hash-chain LZ77, static and dynamic Huffman, the five filters     |
| JPEG   | baseline and progressive, 4:4:4 / 4:2:2 / 4:2:0, CMYK and YCCK, restart markers | quality-scaled tables, optimized Huffman, progressive scan script |
| WebP   | VP8 lossy and VP8L lossless                                                     | both modes                                                        |
| GIF    | LZW, local and global palettes, interlace, transparency                         | median-cut palette with Floyd-Steinberg dithering                 |
| TIFF   | strips, uncompressed / PackBits / LZW / Deflate, both byte orders               | uncompressed and PackBits                                         |
| BMP    | uncompressed and RLE8                                                           | uncompressed                                                      |
| AVIF   | AV1 intra: every transform, all 13 intra modes, deblock and CDEF                | fixed 8x8 partitions, no rate-distortion search                   |
| HEIF   | container only; `probe` answers and decode reports a specific error             | no                                                                |

JPEG carries true DCT-domain scaled decode at 1/2, 1/4 and 1/8, and region decode that Huffman-scans
past blocks outside the box without transforming them. PNG, GIF, TIFF and BMP row-skip and
column-skip.

PNG and deflate-compressed TIFF take a `compression` option, which is a third axis beside quality and
effort: for a lossless format the whole output is the compressor's, so quality says nothing about it.

| `compression` | What it does                                                                                |
| ------------- | ------------------------------------------------------------------------------------------- |
| `auto`        | Reads quality, which is what every caller got before the option existed                     |
| `none`        | Entropy codes without searching for matches, which beats `fast` on filtered photograph rows |
| `fast`        | One hash chain probe per position                                                           |
| `default`     | A bounded chain walk with lazy matching                                                     |
| `best`        | A long chain walk                                                                           |

On a 96 px logo `best` is 664 bytes against 835 at `default`, for 4.3x the CPU.

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

A Latin face is compiled into the module, so drawing text needs no setup:

```ts
using font = Font.load(tinyimg);
```

It covers ASCII and eight accented letters in 21 kB. A wider glyph set or a different face is a blob
under a name of your own, and a fallback chain puts one behind the other rather than replacing it:

```ts
const noto = await fetch('https://example.com/NotoSansSC-Regular.ttf');
await tinyimg.loadBlob('font', 'cjk', await noto.arrayBuffer());
```

`tiny_image_draw_text`, `tiny_image_draw_text_in` and `tiny_text_measure` share one layout walk, so
measuring and then drawing at the measured position lands where the measurement said.

### Setting a Run

`TinyTextStyle` carries the size, tracking, line height, kerning, an outline and a shadow.
`TinyTextBox` carries the rectangle: a wrap width, a height, horizontal alignment including justify,
vertical alignment, and whether text the box cannot hold is clipped or ends in an ellipsis.

A run with an outline or a shadow is drawn in three passes, every shadow then every outline then
every fill, so a letter's outline never prints over its neighbor's fill. The outline is a dilation of
the glyph's coverage rather than an offset of its curve, which rounds a sharp interior corner that a
true offset would extend to a point; at the one to four pixels a card uses the difference is not
visible.

`tiny_text_lines` reports the layout one line at a time: the byte range, the width, where the line
sits once aligned, its baseline, and whether the box clipped it or an ellipsis ended it. It is what
tells a caller a headline was truncated before the card is served.

### Mixed Styles

`tiny_image_draw_text_runs` takes up to sixteen spans, each with its own face, size, color, outline
and shadow, and flows them as one run: a wrap can land in the middle of a span, and a line that mixes
sizes sits on one baseline, which is the largest ascent on that line. Kerning does not cross a span
boundary, because a kern pair belongs to one face and the glyphs either side may not.

### What the Face Says

| Table  | Read for                                                                |
| ------ | ----------------------------------------------------------------------- |
| `kern` | Legacy pair kerning                                                     |
| `GPOS` | Pair kerning under the `kern` feature, formats 1 and 2                  |
| `GSUB` | Ligatures under the `liga` feature, longest match first                 |
| `fvar` | Variation axes, their ranges and their defaults                         |
| `avar` | Axis remapping, so the midpoint of an axis is where the designer put it |
| `gvar` | Outline and advance deltas, with regions and inferred points            |

Modern faces ship `GPOS` and often no `kern` at all, so a reader that only knew the legacy table
would read most fonts as unkerned. `GPOS` is consulted first and the legacy table is the fallback.

One variable file serves every weight:

```ts
tiny_font_set_axis(font, TINYIMG_AXIS_WEIGHT, 700);
```

The outlines are interpolated from the face's own deltas, and so are the advances, which come from
the same deltas' phantom points. Points no region names are inferred from their neighbors along the
contour, which is what keeps an outline whole rather than torn.

### Open Graph Cards

`og` renders one:

```ts
const card = await og(tinyimg, {
	title: 'How a planner made the decode cheap',
	subtitle: 'Deciding what to decode before decoding it',
	footer: 'tinyimg.gmitch215.dev',
	background: '#0b1020'
});

return card.response({ 'cache-control': 'public, max-age=86400' });
```

1200x630 by default, with the headline wrapped, truncated with an ellipsis when it does not fit, and
a shadow under the text. `image` covers the card with a photograph, fitted by the planner, so a
4000 px source is never decoded at 4000 px to end up 1200 wide. The result reports `titleTruncated`
and `missingGlyphs`, which are the two ways a card goes out wrong.

A Worker that serves cards for its own pages, cached so each one is rendered once:

```ts
import wasm from '@gmitch215/tinyimg/tinyimg.wasm';
import { og, TinyImgModule } from '@gmitch215/tinyimg';

const tinyimg = TinyImgModule.load(wasm);

export default {
	async fetch(request: Request, env: unknown, ctx: ExecutionContext): Promise<Response> {
		const url = new URL(request.url);
		const cache = caches.default;

		const hit = await cache.match(request);
		if (hit) return hit;

		const card = await og(tinyimg, {
			title: url.searchParams.get('title') ?? 'Untitled',
			subtitle: url.searchParams.get('subtitle') ?? undefined,
			footer: url.hostname,
			background: '#0b1020',
			format: 'png'
		});

		if (card.missingGlyphs > 0) {
			// the compiled-in face is a Latin subset; load a wider one for anything else
			console.warn(`${card.missingGlyphs} codepoints had no glyph`);
		}

		const response = card.response({
			'cache-control': 'public, max-age=604800',
			'x-title-truncated': String(card.titleTruncated)
		});

		ctx.waitUntil(cache.put(request, response.clone()));

		return response;
	}
};
```

Nothing is loaded and no subrequest is made, so the whole card is CPU and one cache write.

100 characters at 16 px cost 0.156 ms.

## Face Detection

An LBP cascade over an integral image, multi-scale, with union-find grouping. The cascades are
OpenCV's `lbpcascade_frontalface_improved.xml` and `lbpcascade_profileface.xml`.

Both cascades are compiled into the module, so detection needs no setup:

```ts
const faces = await tinyimg.detectFaces(source);
// [{ x, y, width, height, neighbors }]
```

The module carries no XML parser, so a cascade is a packed form that `scripts/fixtures.ts` builds
from the OpenCV source: 7.4 kB for the frontal cascade against 54 kB of XML. `loadBlob` rejects one
still in XML, and `bun run blobs` rebuilds the packed pair if you would rather train your own.

Loading a cascade of your own replaces both builtins rather than joining them, which is what a host
substituting a default expects. With none loaded, the two that ship both run and their results are
grouped, so a frontal and a profile cascade find both kinds of face and a face that fires both is one
box. Detections match OpenCV's own detector within a pixel on identical pixels.

Feeds `gravity: 'face'`, which therefore works without loading anything.

## Color Management

ICC matrix-and-TRC profiles: the `rXYZ` / `gXYZ` / `bXYZ` primaries, `curv` and parametric `para`
tone curves, and Bradford chromatic adaptation folded into the matrix. Conversion between a tagged
profile and sRGB agrees with ImageMagick at 49 dB.

### Reading the dB Figures

Every accuracy number here and in the technical report is PSNR against a stated reference:
`10 * log10(255^2 / MSE)`, where the mean squared error runs over every channel sample of both
images. It is a log scale, so 6 dB is one bit of precision and each 10 dB is a tenfold drop in
squared error. Higher is closer, and infinity means the two images are byte-identical.

Where the thresholds fall, for 8-bit images:

| PSNR        | What it looks like                                                              |
| ----------- | ------------------------------------------------------------------------------- |
| above 50 dB | below one level of quantization; no viewer can distinguish the two              |
| 40 to 50 dB | indistinguishable in normal viewing, and separable in a difference blend        |
| 30 to 40 dB | artifacts findable if you look for them, around the quality of a JPEG at q75-85 |
| below 30 dB | visible on inspection                                                           |

Those bands are the conventional reading of the scale rather than something measured here, and PSNR
scores per-sample error without regard to where the error sits. A few badly wrong pixels and a faint
haze across the whole frame can score the same while looking nothing alike, which is why the codec
tests assert a floor per fixture instead of one number for the library. The floors that matter are
the ones the comparison is stated at: 49 dB for an ICC conversion is a rounding difference, and the
24 dB floor on a glyph shape is a hinting difference measured against FreeType.

LUT-based A2B and B2A profiles, which is mostly CMYK printer profiles, are rejected with a specific
error rather than approximated. The matrix and TRC path covers sRGB, Display P3, Adobe RGB and
Rec.2020, the profiles web images carry.

## Budget and Effort

Three things let a caller work inside a CPU limit instead of discovering it.

**A request the source already satisfies costs a header read.** `transform` compares the request
against the source's header first, and hands the original bytes back when there is nothing to do. A
600 px bound on a 400 px WebP decodes nothing.

**`effort: 'fast'` bounds the work rather than the quality.** Quality says what the output should
look like; effort says how hard to work to get there. It applies to the decode, the resize and the
encode, and each part gives up something measured:

| Stage  | What `fast` does                             | Speed          | Quality         |
| ------ | -------------------------------------------- | -------------- | --------------- |
| Decode | WebP skips deblocking                        | 1.53x          | 46.8 dB         |
| Decode | WebP reduces in the plane domain             | 2.05x to 3.35x | 38.9 dB         |
| Decode | JPEG replicates chroma instead of filtering  | 1.11x to 1.25x | 43.6 to 59.5 dB |
| Resize | Bilinear instead of Catmull-Rom, enlarging   | 3.2x           | 45.7 dB         |
| Encode | WebP bounds the 4x4 prediction search        | 1.18x to 1.45x | within 0.05 dB  |
| Encode | PNG compresses one candidate stream, not two | 1.59x to 2.08x | see below       |

The plane-domain reduction reaches a scaled WebP request only, and its speed figure includes the
deblocking skip on the same request. It averages the luma and chroma planes over each output pixel
and converts once, where the default converts the frame and averages the result, so it also never
allocates the full frame of RGBA.

The PNG row is the one whose cost depends on what you are encoding. The encoder compresses an
adaptively filtered stream and an unfiltered one and keeps the smaller; `fast` compresses only the
adaptive one. On a photograph the adaptive stream wins anyway, so the output is byte-identical and
the speed is free: `sf-24.jpg` at 800 px measured 142 ms against 68. On flat artwork the unfiltered
stream is the one that would have won, so `fast` pays 7.0% more bytes on `base-mono.gif`, 17.2% on
`webassembly.png` and 63.1% on a 96x96 logo. Resizing a photograph gets the speed for nothing;
encoding an icon should stay at `fancy`.

**A lossy decoder has more to trade than a lossless one.** A lossless bitstream defines its pixels
exactly, so every step is needed to produce them and PNG, GIF, TIFF and lossless WebP decode
identically at either effort. So does a 4:4:4 JPEG, which has no chroma to upsample. A reduction
keeps its filter too, because an area average is already both the cheapest and the right answer;
only enlarging has a filter to step down. What the lossless formats do have is an encoder with a
search to bound, which is the PNG row above.

A filter you name is never substituted, and neither is a format. Effort decides what the library
left open, not what you asked for.

End to end, on requests through the whole pipeline. Fastest of 25 interleaved runs, so contention
moves a row one way only:

| Request                       | `fancy` | `fast` |           |
| ----------------------------- | ------: | -----: | --------- |
| 200 px, WebP source, WebP out |   15 ms | 7.6 ms | 1.9x      |
| 800 px, JPEG source, PNG out  |  103 ms |  57 ms | 1.8x      |
| 800 px, WebP source, WebP out |   44 ms |  36 ms | 1.3x      |
| 800 px, JPEG source, WebP out |   28 ms |  22 ms | 1.1x-1.4x |
| 800 px, JPEG source, JPEG out |   17 ms |  17 ms | no lever  |
| 800 px, JPEG source, AVIF out |   53 ms |  53 ms | no lever  |

A WebP source gains most at 200 px, where both the decode and the encode have something to give and
the decode additionally reduces in the plane domain. The two bottom rows produce byte-identical
output at either effort, which is how you can tell there is nothing to spend rather than something
being spent badly: a 4:4:4 JPEG re-encoded as JPEG has no deblocking pass, no chroma to replicate
and no prediction search, and the AVIF encoder does not read the effort field at all. Timings on
those rows differ between runs by a few percent in either direction, which is the machine.

Encoded size can go either way under `fast`, because the per-subblock choice minimizes prediction
error rather than rate: a diagonal mode can win on error and then cost more bits to code. Flat-color
illustrations pay the most, and hard diagonal edges are what those modes exist for.

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
it, and use [`measure`](#work-counters) when you need what a request cost.

**A budget the container change cannot meet degrades the plan, and the result says what it gave up.**
Container first, because a cheaper encoder costs bytes and nothing else; then the planner's own
levers, in the order of what they cost the picture:

| Reported as   | What it gives up                                                                       |
| ------------- | -------------------------------------------------------------------------------------- |
| `compression` | A lossless output's compressor is turned down, which costs bytes and not fidelity      |
| `effort`      | The decode runs fast, and an enlargement left open steps down a filter                 |
| `filter`      | A filter left open goes to nearest                                                     |
| `scale`       | The decoder is asked for a coarser grid than the output needs, so the result is softer |

`compression` comes first because on a PNG or TIFF output the encoder costs more than everything the
planner controls, so softening the picture to pay for the compressor would be the wrong trade. It
measured 2.8x to 3.7x on a budgeted PNG request. Naming `compression` pins the level and the planner
degrades instead.

```ts
const result = await transform(tinyimg, source, { width: 600, budgetMs: 4 });

result.degraded; // ['effort'] when the effort tier was enough to fit
```

The order is not the intuitive one, and a measurement decided it: reducing the decode before the
filter turns a cheap box reduction into an interpolating enlargement, which on one 600 px request
measured 22,780 microseconds against 21,801 for no degradation at all. A rung that is not cheaper is
skipped, so what comes back is never softer for more CPU than the undegraded plan.

The output extent never changes. A budget decides how the pixels are made, not how many, and nothing
is degraded when the estimate already fits. `degraded` is empty on a pass-through, which needs no
lever at all.

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
evidence. `measure` returns what the module did:

```ts
const { work } = await tinyimg.measure(() => transform(tinyimg, source, { width: 200 }));

work.samplesPerTransform; // 1 at an eighth scale, 4 at a quarter, 64 at full
work.decodedSamples / work.sourceSamples; // what the reduction was worth
work.filtered; // macroblocks a region did not skip
```

`samplesPerTransform` is the one to read for a scaled decode: a decode transforming whole blocks and
averaging them away reads 64 whatever its name is.

## Blobs

A default of each kind is compiled into the module: a Latin face, the four ICC profiles and both LBP
cascades, about 45 kB. Text, profile conversion and face detection therefore work off a bare
instantiate, and `builtinBlobs` lists what is there.

A blob is how you replace one or add to it: a wider glyph set, another cascade, a profile of your
own. `loadBlob` takes bytes from anywhere, so there are three ways to deliver them and no code change
between them.

A name resolves against the loaded blobs first and then the builtins, so `sans` always answers. An
enumeration is different: one loaded blob of a kind hides every builtin of that kind, which is what
stops a host that installed its own cascade from also running the two that ship.

The prebuilt set is published at `https://cdn.gmitch215.dev/tinyimg/`, with
`https://cdn.gmitch215.xyz/tinyimg/` and `https://cdn.gmitch215.blog/tinyimg/` as mirrors.
`blobs.json` sits at that prefix and each entry's `path` resolves against it, so one URL is enough to
find the rest. A plain `fetch` needs no binding and costs one subrequest:

```ts
const font = await fetch('https://cdn.gmitch215.dev/tinyimg/fonts/DejaVuSans.ttf');
await tinyimg.loadBlob('font', 'sans', await font.arrayBuffer());
```

A binding costs the same one subrequest and keeps the bytes on your own account. `BLOBS` below is an
R2 bucket you have uploaded `blobs/` to, and an Assets binding works the same way:

```ts
const cascade = await env.BLOBS.get('cascades/lbp-frontalface.bin');
if (cascade) await tinyimg.loadBlob('cascade', 'frontal', cascade.body);
```

```jsonc
{
	"r2_buckets": [{ "binding": "BLOBS", "bucket_name": "tinyimg-blobs" }]
}
```

Or bundle one as a wrangler Data module, which costs bundle bytes and no latency:

```ts
import cascade from './lbp-frontalface.bin';
await tinyimg.loadBlob('cascade', 'frontal', cascade);
```

Eight blobs can be resident at once. `freeBlob` releases one and `freeBlobs` releases all; neither
touches a builtin, which lives in the module's data section. A cascade is parsed at load, so a bad
one fails at startup instead of becoming a search that finds nothing.

The face and the cascades are third-party data. Their notices are in `LICENSES/`.

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

The wasm module is 778 kB, of which 45 kB is the data it carries: a Latin face, four ICC profiles and
two detection cascades. The AV1 codec behind AVIF is 283 kB of it, about half of that in generated
tables. The wrapper is 52 kB of JavaScript. `bun run size` prints the breakdown per translation unit.

Speed comes first, then size, then memory. Size still gets measured, because a bigger module costs
startup time and `tiny` is in the name, but it no longer wins an argument against speed.

Every codec can be compiled out, so a caller who only handles PNG can link a module with nothing else
in it. `--gc-sections` then drops the implementation along with its registry entry.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32.cmake \
  -DCMAKE_C_FLAGS="-DTINYIMG_NO_WEBP -DTINYIMG_NO_TIFF -DTINYIMG_NO_GIF -DTINYIMG_NO_AVIF"
cmake --build build
```

That build is 366 kB, and `tinyimg.features` reports the four as absent, so a caller can check
before offering a format rather than calling and handling a failure.

Build it into its own directory and `bin/tinyimg.wasm` is still where it lands, because that path is
absolute in `CMakeLists.txt`. Run `bun run build:wasm` afterwards to put the full module back.

## Platform Limits

- **The module must arrive compiled.** workerd refuses `WebAssembly.Module(bytes)` with "Wasm code
  generation disallowed by embedder". Import the `.wasm` file; `loadBytes` is for node, bun and the
  browser and throws on Workers.
- **The Workers Free plan allows 10 ms of CPU per request**, which is not configurable and is below
  the cost of most transformations; see [Speed](#speed). Workers Paid defaults to 30 seconds and goes
  to 5 minutes with `limits.cpu_ms`. A Worker over the limit returns error 1102. Enforcement is
  elastic, so an isolated request over the limit may still complete; sustained ones do not, and a
  single test request that passed is not evidence that a request fits.
- **Response body bytes are not billed as CPU.** A 2.88 MB body measured 99 ms of wall clock and zero
  CPU, so the limit applies to the transformation and not to serving it.
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
- **No hand-written SIMD kernels.** The flag is on and the autovectorizer runs, which is worth
  1.04x to 1.15x now that the build is `-O2` rather than `-Oz`. Nothing uses intrinsics directly, so
  the widest stages are still scalar.

## Out of Scope

- **HEIF decode and encode.** HEIF shares AVIF's container and carries HEVC rather than AV1. HEVC
  decoders are covered by active patent pools, which is a licensing question rather than a technical
  one and the reason this is declined rather than queued. The container parse ships so `probe`
  answers, and Safari is the only browser that displays HEIF, so it matters as a camera source
  format rather than an output one.
- **JPEG XL.** No browser ships it by default.
- **Animation editing.** Composing or re-timing frames is a different product.
- **LUT-based ICC profiles.** Rejected with a specific error rather than approximated.
- **AI-backed effects.** No model, and a Worker is the wrong place for one.

## API Reference

Generated documentation lives at [tinyimg.gmitch215.dev](https://tinyimg.gmitch215.dev): the
TypeScript surface under `/typedoc`, and the C headers under `/doxygen`.

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

MIT.

The module compiles in two third-party works, and their notices ship with the package in
`LICENSES/`. The Latin face is a subset of DejaVu Sans, under the Bitstream Vera Fonts license
(`LICENSES/DejaVu.txt`). The two face cascades are packed from OpenCV's LBP data files, under the
3-clause BSD license (`LICENSES/OpenCV-LBP-Cascades.txt`). The four ICC profiles are generated from
published primaries and transfer curves rather than copied from anyone's files.
