# tinyimg Technical Report

Measurements, and the approaches they refuted. Every number here came from the artifact that ships
(`bin/tinyimg.wasm`, built with `-Oz -flto`) or from a purpose-built arm; none is an estimate.

Reproduce with `bun run size`, `bun bench/run.ts`, `bun run test`, and
`bun scripts/coverage-c.ts`. Every compressed figure is `gzip -9`, the level `bun run size`
uses; the default level reports about 1.4% larger, so a number here will not match a casual
`gzip -c | wc -c`.

## Size

Small size is the primary goal, ahead of speed and then memory, and every choice below that trades
one against another was decided in that order.

| Measure                    |      Bytes |
| -------------------------- | ---------: |
| raw wasm                   |    190,309 |
| **gzip**                   | **90,360** |
| brotli                     |     79,944 |
| npm tarball, whole package |    138,700 |

The tarball is what a caller installs: the module, the compiled wrapper, the declarations and their
source maps. The wrapper's `.js` is 29,867 raw and 7,831 gzipped across five files. The `.d.ts` files
keep their documentation, because that is where an editor reads it; the JS is emitted with comments
stripped, because nothing reads them there. Two `tsc` passes rather than one, since `removeComments`
strips the declarations too.

### By phase

Each row is the whole module as that phase left it, so the last column is what the phase cost on top
of everything before it. Phase 1's figure was not recorded at the time and is not reconstructible
from the current tree, so the series starts at Phase 2; the Phase 7 wrapper added one export to the
module, which is the difference between the last row and the current figure above.

| Phase                          | Module raw | Module gzip | Added gzip |
| ------------------------------ | ---------: | ----------: | ---------: |
| 2, PNG, JPEG, GIF, TIFF        |     56,976 |      28,638 |            |
| 3, WebP and the AVIF container |     97,615 |      48,324 |    +19,686 |
| 4, the planner                 |    116,506 |      56,852 |     +8,528 |
| 5, operations, effects, color  |    168,742 |      79,687 |    +22,835 |
| 6, text and faces              |    187,569 |      88,932 |     +9,245 |

Per-codec marginal cost, measured by removing one at a time: JPEG +18.3 KiB, GIF +6.9, PNG +5.8,
TIFF +5.2, BMP +3.2, WebP +37.9 (raw). PNG's figure fell from 13.9 once TIFF also used the DEFLATE
unit, which is the difference between a marginal cost and an additive one.

Removing three at once is not the sum of their arms, for the same reason. Dropping WebP, TIFF and
GIF together gives **138,882 raw / 65,438 gzip**, a 27.6% reduction, and `tiny_features` reports the
three as absent:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32.cmake \
  -DCMAKE_C_FLAGS="-DTINYIMG_NO_WEBP -DTINYIMG_NO_TIFF -DTINYIMG_NO_GIF"
```

### Where the estimates were wrong

The plan projected ~131,000 raw and ~45,000 gzip. The module is 190,309 and 90,360, so the estimate
was low by 45% and 101%. Two regions account for almost all of it:

- **WebP came in at +37.9 KiB raw against a 27.2 KiB estimate**, a 39% miss. The estimate is the one
  the plan flagged as most likely to move, and Phase 3 existed to measure it before later phases
  spent the headroom.
- **The operations were estimated per region and summed.** Phase 5 alone added 52.2 KiB raw for
  roughly 150 functions. Adding up per-feature guesses systematically undercounts, because each
  guess is the optimistic case and nothing cancels.

Both still leave the module small enough for what it is asked to do, so the estimate being wrong
cost nothing but the confidence in it.

## Throughput

`sf-24.jpg`, 1835x1032, 1.9 megapixels, on darwin-arm64. Median of 11 runs.

### Codecs

Against `@jsquash`, which is libjpeg, libpng and libwebp built with emscripten and running in the
same runtime. Ratio is `@jsquash` over tinyimg, so above 1 means tinyimg is faster.

| Measurement     | tinyimg | bytes out | @jsquash | bytes out | Ratio |
| --------------- | ------: | --------: | -------: | --------: | ----: |
| decode jpeg     | 24.4 ms |           |  11.4 ms |           | 0.47x |
| encode jpeg q80 | 46.6 ms |    73,513 |   107 ms |    47,340 | 2.30x |
| decode png      | 28.9 ms |           |  24.2 ms |           | 0.84x |
| encode png      |  417 ms |   636,919 |  19.4 ms | 1,209,393 | 0.05x |
| decode webp     | 32.6 ms |           |  18.1 ms |           | 0.55x |
| encode webp q80 |  151 ms |    54,506 |   104 ms |    29,788 | 0.69x |

**The byte counts are part of the measurement.** Two of these rows are not a comparison at all
because the two encoders are at different operating points:

- **PNG encode reads 21x slower and produces a file 47% smaller.** A lossless encoder trades time
  against size, and these are not on the same point of that curve. Neither number alone is a
  verdict, and "21x slower" on its own would be the same mistake as comparing GIF output against a
  reference the format cannot express (see the refuted list below).
- **JPEG and WebP encode both produce larger files than `@jsquash` at the same nominal quality.** The
  quality scale is not a standard, so `q80` means one thing to our tables and another to mozjpeg's;
  matching output size rather than the number on the dial is the comparison worth making, and it has
  not been done.

Decode is the honest comparison, and tinyimg loses it: 2.1x behind libjpeg, 1.8x behind libwebp,
1.2x behind libpng. That is a hand-written scalar decoder against three mature, heavily tuned
libraries, all three of which have SIMD implementations of their hot kernels and this has none.

### Where decode time goes

Measured with `sample` against a `-O2 -flto` build of the shipping sources, 1 ms for 10 s on
`sf-24.jpg`. The fractions are what decide whether an optimization is worth writing, and two rounds
of guessing at them beforehand had already been wrong.

| JPEG                             |     % | WebP                            |     % |
| -------------------------------- | ----: | ------------------------------- | ----: |
| inverse transform                | 49.7% | loop filter                     | 41.5% |
| chroma upsample, color and store | 27.4% | chroma upsample and YUV to RGBA | 24.8% |
| huffman decode                   | 10.5% | mode and coefficient decode     | 20.3% |
| coefficient handling             | 10.3% | region copy                     |  5.8% |
| plane clearing                   |  1.1% | intra prediction                |  3.6% |
|                                  |       | inverse transform               |  1.6% |

Three things follow, and all three contradict a plausible reading of the same problem:

- **Entropy decoding is not the bottleneck in either format.** It is 10.5% of JPEG and 20.3% of
  WebP, so a cleverer Huffman or boolean decoder cannot pay for a 2x gap.
- **WebP's inverse transform is 1.6%.** It is the first thing an approach modelled on libwebp's
  own DSP layer reaches for, because that is where libwebp's SIMD lives; here the loop filter costs
  26x more than the transform it filters.
- **JPEG's inverse transform is half the decode**, but not in the half a SIMD kernel would help.
  See the refuted list below.

### What the profile paid for

Three changes, all of them deleting work rather than widening it, byte-exact against the committed
decode hashes and 190 gzip bytes larger in total.

| Change                                                      | JPEG    | WebP    |
| ----------------------------------------------------------- | ------- | ------- |
| baseline                                                    | 30.4 ms | 45.8 ms |
| WebP: skip the box average when the scale denominator is 1  |         | 32.6 ms |
| JPEG: copy the unscaled component span instead of clamping  | 27.6 ms |         |
| JPEG: transform one row when a block has no vertical detail | 24.4 ms |         |

The first is the largest single win in this report's history relative to its size. `resample_region`
honors the region and scale that neither WebP bitstream can decode itself, and it ran unconditionally:
at a denominator of 1 every output pixel went through a one-pixel box average, which is four integer
divisions by one to move four bytes. In wasm that was 29% of WebP decode.

The third has a spread wider than its headline. A block whose vertical AC
coefficients are all zero leaves the eight intermediate rows identical, so the row pass computes the
same eight samples eight times. How often that happens is a property of the picture:

| Fixture         | columns with no vertical detail | blocks with none |
| --------------- | ------------------------------: | ---------------: |
| `sf-24.jpg`     |                           93.0% |            67.8% |
| `mountains.jpg` |                           47.5% |            10.7% |
| `road.jpg`      |                           48.0% |             9.0% |

So the change is worth 20.8% on the reference fixture and 0.8-2.3% on a detailed photograph. The
reference fixture is a smooth one, and quoting only its number would overstate the general case by
an order of magnitude.

### The reduced transform, which was not reduced

A scaled decode is supposed to produce fewer samples for less work. It produced fewer samples. The
work was the same: `idct_boxed` ran the full 8x8 transform and box averaged the result down, so a
quarter scale block computed 64 spatial samples to keep 4. At 1/4 that was **63.6% of the decode**,
and the scale ladder was not even monotonic, because a half scale decode did the whole transform and
an averaging pass on top of it.

| decode  |   before |       after |
| ------- | -------: | ----------: |
| 1/1     | 10.21 ms |    10.10 ms |
| **1/2** | 10.58 ms | **7.29 ms** |
| **1/4** |  8.73 ms | **5.40 ms** |
| 1/8     |  3.14 ms |     3.14 ms |

The box average is linear and separable, so it folds into the transform and the intermediate never
has to exist. For the two scales that needed it the composition collapses:

- **n = 2**, a quad average, kills every even coefficient, and the two outputs differ only in the
  sign of their sum. Five multiplies for a whole one dimensional pass.
- **n = 4**, a pairwise average, kills coefficient 4 outright and leaves 5, 6 and 7 carrying the same
  cosines as 3, 2 and 1 with the opposite sign, so eight coefficients fold onto four and a 4 point
  transform finishes it.

Verified against a floating point reference at 20,000 random blocks before any of it was written, to
1e-13. Every constant is a product of both stages' factors, so a coefficient meets exactly one of
them: no chained multiply, no intermediate descale, and peak pass 1 sums of 26.4 bits against a
coefficient limit of 4096.

**This changed the output, and the change is an improvement that can be stated as a number.** The
old path averaged 64 samples that had each been rounded and clamped to a byte; this one averages
before rounding. Against the exact area average of the continuous reconstruction:

| n   | implementation                |      RMSE |         PSNR | worst sample |
| --- | ----------------------------- | --------: | -----------: | -----------: |
| 2   | old, round 64 then average    |     1.897 |     42.57 dB |    18 levels |
| 2   | **new, average in transform** | **0.154** | **64.37 dB** |  **1 level** |
| 4   | old                           |     1.850 |     42.79 dB |    30 levels |
| 4   | **new**                       | **0.205** | **61.90 dB** |  **1 level** |

Clamping each of 64 samples before averaging biases hard wherever the block clips, which is where the
old worst case of 30 levels came from. One golden was re-recorded for this, with the native and wasm
builds agreeing on the new digest, the same way the original was established.

The entropy decode is what a scale cannot touch: the coefficient count is identical at every
denominator, because every one of them has to be read to reach the next block. Widening the bit
reader's accumulator to 64 bits with a four byte refill guarded by a SWAR test for `0xFF` took about
5% at every scale, byte-exact. The 8 bit Huffman prefix table it feeds already existed.

### WebP, where a region can only truncate the tail

A region cannot make the WebP decoder skip to it. A macroblock predicts from its left and upper
neighbors and the loop filter reads back four rows into the macroblock above, so the work before a
region is not optional. The work after it is, exactly, because both passes run in raster order and
neither lets a later macroblock reach an earlier one. Reconstruction, filtering and the color
conversion now all stop after the last macroblock row the region touches:

| region      |   native |      |
| ----------- | -------: | ---- |
| full frame  | 20.78 ms |      |
| top half    | 10.19 ms | -51% |
| top quarter |  5.08 ms | -76% |
| bottom half | 20.06 ms | -3%  |

The asymmetry is the whole result. A band at the top is nearly free and a band at the bottom costs
what the picture costs, and a test asserts both so a bound that looked like a saving could not be
one. The color conversion is cut to exactly the rows requested rather than to a raster prefix,
because unlike the other two it has no dependency between rows.

### Operations

Timed as a decode and a plan run with no encoder in the way.

| Operation                  |  Median | ms/Mpx |
| -------------------------- | ------: | -----: |
| decode only, no operations | 24.5 ms |   12.9 |
| crop 500x500               | 8.87 ms |   4.68 |
| resize box to 400          | 19.4 ms |   10.3 |
| resize bilinear to 400     | 23.0 ms |   12.2 |
| resize Catmull-Rom to 400  | 34.6 ms |   18.3 |
| grayscale                  | 25.3 ms |   13.3 |
| gamma                      | 37.0 ms |   19.5 |
| flip horizontal            | 41.1 ms |   21.7 |
| rotate 180                 | 41.6 ms |   22.0 |
| saturation                 | 42.2 ms |   22.3 |
| hue                        | 42.2 ms |   22.3 |
| brightness                 | 42.5 ms |   22.5 |
| four color operations      | 43.8 ms |   23.1 |
| gaussian blur sigma 4      |  100 ms |   52.9 |
| sharpen                    |  109 ms |   57.8 |

Two things to read off it:

- **The resize and crop rows are below the decode baseline.** They reduce the extent, so the planner
  asks the decoder for less and the operation more than pays for itself. That is the whole design,
  visible as a negative marginal cost.
- **Four color operations cost 1.3 ms more than one.** They collapse into one matrix and one table
  and run in a single pass, so the second, third and fourth are close to free. The plan predicted
  this and it holds.

### Text

100 characters at 16 px, DejaVu Sans: **0.156 ms**, against a 0.4 ms target.

An earlier figure of 0.72 ms was measured against the ctest static library, which has no LTO: the
math shims live in `util.c` and every caller is a different translation unit, so `tiny_sqrtf` and
`tiny_floorf` were real calls inside the rasterizer's inner loop. The same code in one LTO unit is
4.6x faster. This is the second time that mistake has cost time on this project; see the refuted
list.

Rasterization is 63% of that 0.156 ms, and 30 of the 102 characters in the test string are distinct,
so the glyph cache the plan called for would save about 0.07 ms on an operation that is 1% of a
request which spends 24 ms decoding a JPEG. It was not built. An active edge list was built instead:
it speeds up every glyph rather than only a repeated one, and it has no lifetime hazard, where a
cache in the arena dies on any `tiny_arena_reset`.

### Face detection

| Setting                                                  | smile.jpg 1470x1920 |
| -------------------------------------------------------- | ------------------: |
| every scale, full resolution                             |              993 ms |
| `min_size = height/10`, reduced to 1200 px (the default) |              155 ms |
| `min_size = height/8`, full resolution                   |             44.7 ms |

`min_size` is the cost knob, not the resolution: the largest pyramid level is the input scaled by
`window height / min_size`, so halving `min_size` quadruples the work.

**A cascade is scale-dependent, not only size-dependent, so no single setting finds every face.**
The frontal face in `smile.jpg` is found at a moderate reduction and lost at full resolution; the
side-facing pair in `man.jpg` is the reverse. Feeding the same inputs through OpenCV's own detector
moves the same way, so this is the method rather than the port.

### The planner

The plan's worked example: a 500x500 crop of a 1.9 megapixel photograph down to 100x100, with four
color operations after it.

| Arm                                 |    Median |
| ----------------------------------- | --------: |
| planner on                          |   8.27 ms |
| planner off, one operation per pass |   26.3 ms |
| **ratio**                           | **3.18x** |

What it decided: decode a 500x500 region at 1/4 scale, one pass, two color stages. Without the
backward ROI walk that is 16 megapixels decoded instead of 125,000 pixels, and without fusion it is
six traversals instead of one.

That 3.18x is the answer to whether the planner earned its bytes. It cost +8,528 gzip in Phase 4,
which is 9.6% of the module, and it removes 69% of the time from a realistic chain. The ratio was
3.63x before the decode work above, because both arms decode and the faster decoder shrinks the
denominator too.

### The encoders, and where a WebP request actually spends

Everything above is decode. For a request that writes WebP, decode is the smaller half. The same
800x450 output, from two different sources:

| stage               | from a JPEG | from a WebP |
| ------------------- | ----------: | ----------: |
| decode and resample |     17.3 ms |     40.4 ms |
| WebP encoder        |     27.0 ms |     27.2 ms |
| **total**           | **44.3 ms** | **67.6 ms** |
| the same to JPEG    |     21.8 ms |             |

The encoder costs the same whichever source it had, and it is 61% of the JPEG-sourced request. The
JPEG encoder produces the same extent in 4.6 ms, a 5.9x spread, which is why the planner can price
formats and choose between them and why doing so is worth more here than any decode work.

Inside the WebP encoder, at 800x450:

| stage                            | share |
| -------------------------------- | ----: |
| 4x4 prediction search (`try_i4`) | 27.0% |
| the predictions it calls         | 19.8% |
| driver and mode decisions        | 11.8% |
| tokenization                     | 10.9% |
| forward transform                |  8.5% |
| color conversion                 |  7.3% |
| entropy                          |  8.8% |
| 16x16 prediction                 |  1.3% |

**The 4x4 mode search is 46.8% of the encoder**, which is what `TINYIMG_EFFORT_FAST` bounds: four
whole-block modes instead of ten. Measured at 800 wide over six fixtures, against the same quality:

| fixture            | speedup |   size | difference |
| ------------------ | ------: | -----: | ---------: |
| `sf-24.jpg`        |   1.45x |  -8.8% |   -0.03 dB |
| `mushroom.jpg`     |   1.21x |  +4.0% |   +0.03 dB |
| `face_art.jpg`     |   1.26x |  +4.7% |   -0.02 dB |
| `mountains.jpg`    |   1.18x |  +5.7% |    0.00 dB |
| `moped.jpg`        |   1.24x |  +7.6% |   -0.05 dB |
| `winter_cabin.jpg` |   1.20x | +17.5% |   -0.02 dB |

Quality is unchanged to within a twentieth of a decibel and the size goes either way, which is worth
explaining rather than averaging: the per-subblock choice minimizes prediction error, not rate, so a
diagonal mode can win on error and then cost more bits to code. On `sf-24.jpg` dropping the diagonals
made the file smaller. The flat-color illustrations pay the most, which is where hard diagonal edges
live and where those modes are actually for.

### What the decode side can give up, and what it cannot

The encoder was the first place effort was spent, and asserting that it was the only place would have
been wrong. Both lossy decoders carry a smoothing pass, and both can drop it. Measured against a
full-effort decode of the same file:

| Decoder             | What `fast` drops                    |       Speedup |      Agreement |
| ------------------- | ------------------------------------ | ------------: | -------------: |
| VP8 lossy           | the deblocking filter                |         1.53x |        46.8 dB |
| JPEG 4:2:0 / 4:2:2  | interpolated chroma, replicated inst | 1.11x - 1.25x | 43.6 - 59.5 dB |
| Resample, enlarging | Catmull-Rom, bilinear instead        |          3.2x |        45.7 dB |

VP8's filter is skippable here for a reason specific to this decoder. The specification defines it as
in-loop, because a later frame predicts from the filtered result; this codec decodes one keyframe and
nothing references what it writes, and intra prediction inside the frame reads the unfiltered
reconstruction. So the call sits after the whole frame and skipping it changes the output and nothing
else.

**Nothing else has anything to drop, and that is a property of the formats rather than a gap.** A
lossless bitstream defines its pixels exactly, so every step is required to produce them: PNG, GIF,
TIFF and lossless WebP measure 1.00x to 1.03x and byte-identical at either effort. So does a 4:4:4
JPEG, which has no chroma to upsample. A reduction keeps its filter too, because an area average is
already both the cheapest option and the correct one.

Two mistakes are recorded here because both were made in the course of measuring this:

- **`sf-24.jpg` is 4:4:4**, and it is the fixture every other figure in this report is quoted on. It
  is the one JPEG in the set that cannot show the chroma lever at all. Measured only there, the
  conclusion would have been that the lever is worthless.
- **The lever scales with output samples; the request's cost scales with source samples.** So it
  reads 1.11x to 1.25x on a full decode and 1.00x on a 200 px thumbnail of the same file, where the
  irreducible entropy decode dominates. A saving is not a saving until it is measured at the extent
  the request actually asks for.

### The filter lever was measured against a comparison the planner never makes

Recorded as the largest untaken lever: box instead of Catmull-Rom, at 5.3x per sample. The figure was
right and the conclusion was not, because `TINYIMG_FILTER_AUTO` **already** picks box for every
reduction. Box is 13x the cubic there and within half a decibel of it, so there was nothing left to
take.

What is actually reachable is enlargement, the only direction that uses the cubic. Box cannot serve
it: 35.7 dB and visibly blocky. Bilinear can, at 3.2x and 45.7 dB, so `fast` steps down one filter
rather than to the bottom. The lever is real, it is a third of the size the note claimed, and it
applies to the less common direction.

Two things came out of writing it. An explicitly named filter is never substituted, because effort
decides what the caller left open rather than overriding what they asked for. And `tiny_plan_fit` had
no way to name a filter at all, so `TransformOptions.filter` was silently dropped whenever both
extents were given; `tiny_plan_fit_with` exists because an option that is accepted and then ignored
is worse than one that does not exist.

### WebP to WebP in the reduced domain, closed

The proposal was to transform a WebP source without fully decoding it, the way the JPEG box average
folds into the inverse transform. The structural objection stands and is recorded above: JPEG blocks
are independent, while VP8's residual is relative to an intra prediction computed from fully
reconstructed neighbors in loop, so taking DC-only per block changes every later prediction and the
error compounds down the picture.

**The mechanism is closed; the objective it served is not.** What it was for was making a WebP-source
request cheaper, and the deblocking skip delivers part of that for five lines instead of a second
decoder path: a 200 px request from a WebP source is 20.3 ms at full effort and 13.5 ms bounded, a
1.38x that is the largest end-to-end effort gain of any route. The remainder is the entropy decode,
which is irreducible in VP8 exactly as it is in JPEG.

### The Free envelope, which the source decides

Workers Free allows 10 milliseconds of CPU per request and is not configurable, so a request either
fits or fails. Measured end to end in wasm from `sf-24.jpg`, at the bounded effort:

| output      |        JPEG |    WebP |
| ----------- | ----------: | ------: |
| 150x84      |     7.16 ms | 7.61 ms |
| **200x112** | **6.99 ms** | 7.68 ms |
| 250x141     |     11.4 ms | 12.9 ms |
| 400x225     |     11.9 ms | 15.4 ms |
| 800x450     |     21.8 ms | 34.4 ms |

The cliff between 200 and 250 is not the output size. It is the scale ladder: 1835/8 is 229, so a
200 wide output fits the eighth-scale decode and a 250 wide one has to take the quarter.

**And the budget is a property of the source, not of the request.** The same 200 wide thumbnail, at
the same eighth scale, from five sources:

| source    | megapixels | 200 wide JPEG |
| --------- | ---------: | ------------: |
| 320x180   |       0.06 |       2.20 ms |
| 1835x1032 |       1.89 |       6.80 ms |
| 1920x1250 |       2.40 |      15.86 ms |
| 2308x3000 |       6.92 |      16.56 ms |
| 5000x4000 |      20.00 |      25.41 ms |

Every one of them decoded 1.57% of its source's samples. The spread is the entropy decode, which no
output size can reduce, so "will this fit" is a question about the picture that arrived. This is why
`tiny_plan_cost` prices a plan against source samples, and why a cache key is part of the library:
for a source past a couple of megapixels there is no output small enough, and the answer has to be
that the artifact is computed once.

**Not reachable:** 800px WebP inside 10 milliseconds. The encoder's non-search work alone is about
14 ms at that extent and decode with resample is 17.3, so a free mode search would still leave 31.
What that closes is the synchronous path for that one request. It does not close serving 800px WebP
on the Free plan, which the cache tier does, at 200 to 250px, which fits, or on Paid.

### SIMD

| Operation             | simd on | simd off | Ratio |
| --------------------- | ------: | -------: | ----: |
| resize box to 400     | 19.5 ms |  18.9 ms | 0.97x |
| four color operations | 44.4 ms |  43.6 ms | 0.98x |
| gaussian blur sigma 4 | 98.2 ms |   100 ms | 1.02x |

**`-msimd128` buys nothing, and the reason is that nothing uses it.** There is not one SIMD intrinsic
in the source: `grep -r 'v128_t\|wasm_i32x4' src/` finds only the `__wasm_simd128__` feature macro in
`version.c`. The flag therefore only enables clang's autovectorizer, which `-Oz` turns off.

It costs 893 raw and 409 gzip bytes, which is 0.46% of the module, and it stays on. Turning it off
would optimize half a percent and leave a trap for whoever writes the first intrinsic, and SIMD is a
capacity lever rather than a capability one, so closing it wrongly costs a percentage rather than a
feature.

The arm is gitignored, so it outlives the sources it was built from. A stale one credited the decode
work above to SIMD and read 1.24x; `bench/run.ts` now compares its mtime against `src/` and
`include/` and refuses to report rather than print that.

**The surviving objective:** the color pass and the resample sampler are both byte-wide loops over
independent lanes, and so are JPEG's row transform and color conversion and WebP's loop filter and
YUV conversion, which the profile above puts at 27.4% and 66.3% of their decoders. Nobody has
written the intrinsics. That is unfinished work, not a refuted approach.

## Partial consumption

Four entry points, cheapest first. `tiny_image_probe` reads headers only and decodes no pixels, for
every format including the ones the library cannot decode. `tiny_image_load_scaled` picks the
cheapest decode covering a box, with true DCT-domain scaling at 1/2, 1/4 and 1/8 for JPEG and
row-and-column skipping for the rest. `tiny_image_load_region` streams rows and keeps only those
intersecting the region, so memory is bounded by the region plus two scanline buffers.

**Progressive JPEG cannot stream in bounded memory**, and does not pretend to. Successive
approximation needs the whole coefficient plane before any pixel is final, so `load_region` on a
progressive file decodes the plane and then crops. `road.jpg` is the fixture that holds this case,
and a test asserts it stays the only progressive one in the set.

## Memory

`-nostdlib`, so there is no malloc. Linear memory starts at one page, grows through
`__builtin_wasm_memory_grow`, and is capped at 64 MiB by `--max-memory`.

- **A first-fit free list with coalescing** over the grown region, for image buffers. First fit over
  an address-ordered list is linear in live blocks rather than constant; an image pipeline holds a
  handful at a time, and a workload holding thousands would want size buckets instead.
- **A bump arena with mark and release** for per-operation scratch. The arena alone cannot serve a
  chain: five operations on a 12 megapixel RGBA image would want 240 MB against a 64 MB cap.
- `tiny_memcpy` and friends are `__builtin_memcpy` under `-mbulk-memory`, which lowers to wasm
  `memory.copy` and `memory.fill` rather than byte loops. The byte-loop bodies stay behind
  `#if !defined(__wasm__)` for the host build.

The wrapper returns every buffer it takes: a run of eight transformations through one module leaves
the page count where it started, asserted in both test lanes.

### The guard was set above the ceiling, so it never fired

`TINYIMG_MAX_PIXELS` was 16,000,000 with a comment reading "correlates to a maximum image size of
4000x4000 pixels". Nothing had measured either number. A 4000x4000 RGB image is 48 MB of a 64 MiB
heap, and no source could have been decoded into one, because a decoder holds its component planes
at the same time as its output.

Full-decode ceilings, bisected against the shipping module, one 4:3 source re-encoded five ways:

| Source            | Bytes per source pixel held |   Ceiling |
| ----------------- | --------------------------: | --------: |
| JPEG 4:2:0 to RGB |                         4.5 | 13.81 Mpx |
| JPEG 4:2:2 to RGB |                           5 | 12.28 Mpx |
| JPEG 4:4:4 to RGB |                           6 | 10.32 Mpx |
| PNG RGB           |                           - | 10.97 Mpx |
| PNG RGBA          |                           - |  8.56 Mpx |

Every one of those is below the guard, so every one of them failed in the allocator with
`TINYIMG_ERR_MEMORY`. The whole reason `TINYIMG_ERR_TOO_LARGE` exists is that its remedy differs -
ask for less, not for more memory - and it was unreachable for all five.

**A pixel count cannot express the constraint, because an allocation is bytes.** So
`TINYIMG_MAX_IMAGE_BYTES` is 32 MiB, half the heap, checked beside the pixel cap in
`tiny_image_create` and again in `tiny_plan_resolve` so a plan that resolves is a plan that can
allocate. The pixel cap still bounds decode time and is now reachable only by grayscale, which is
the one channel count the byte cap does not bind first. Both are asserted at their boundary: 3344
squared of RGB passes and 3345 does not, 4000x4000 of grayscale passes and one pixel wider does not.

The two caps are also why the TypeScript side grew `TinyImgTooLargeError` as a subclass of
`TinyImgMemoryError`. The doc comment claimed the remedy differed while the mapping collapsed both
codes into one class, so no caller could match on it.

### An animation cannot be held, and does not need to be

`ball_kick.gif` is 800x600 and 57 frames. Measured on the shipping module:

|                                 |                               |
| ------------------------------- | ----------------------------: |
| Decode frame one                |                       4.23 ms |
| 57 frames of decode alone       |                        241 ms |
| 57 frames to a 200 px thumbnail |                        335 ms |
| 57 frames at the source extent  |                        977 ms |
| One frame as RGBA               |                       1.8 MiB |
| All 57 at once                  | 104 MiB against a 64 MiB heap |

Full animation support is not blocked by size. Continuing the block walk past the first frame,
handling the three disposal methods, and writing a loop extension with per-frame delays is roughly
800 gzipped bytes, and the memory is solvable by streaming one frame at a time rather than holding
the set.

**It is blocked by the CPU budget.** The cheapest useful animated request, a 200 px thumbnail, is
335 ms: 33x the Free per-request allowance and 57x what the same request costs on a still. So the
first frame stays the answer, and the thing worth fixing was the case next to it.

`transform` refused to pass an animation through, on the reasoning that re-encoding it is a change.
That had the sign backwards: a decode yields frame one, so a request with nothing to do was spending
23 ms to turn 57 frames into a still, where handing the source back is both free and lossless. It
now passes through whole, and a request with real work to do sets `flattened` on the result
rather than dropping 56 frames silently.

## Data, in three buckets

The rule that decides where a table lives:

1. **Derivable, so derived at init.** CRC32 (1 KB of table becomes 10 lines), Adler32, the sRGB and
   gamma tables, JPEG's zigzag order, the AAN IDCT scale factors, the gaussian and Bayer kernels,
   VP8L's distance mapping. Zero bundle bytes, zero fetch, strictly better than either alternative.
2. **Small and unconditional, so inlined.** JPEG's standard Huffman and quantization tables, VP8's
   token trees and default coefficient probabilities, the sRGB primaries. About 3.5 KB.
3. **Large or conditional, so loaded at runtime.** Fonts, ICC profiles and detection cascades, all
   through `tiny_blob_load`. Nothing in `blobs/` is required to use the library, and every feature
   that reads one degrades without it rather than failing.

The cascades are the OpenCV XML repacked to a flat binary by `scripts/cascade.ts`: 7,380 bytes from
54,039 for the frontal one. An XML reader in the module would cost more than the cascade does.

## AVIF

Container parse only: `probe` answers fully and decode reports `TINYIMG_ERR_UNSUPPORTED_CODEC`.

**Size is not the reason.** An AV1 intra decoder is ~40-55 KiB gzipped against a module that is
89.2 KiB, so it would fit. What was measured:

|                                                       |                      packed binary |
| ----------------------------------------------------- | ---------------------------------: |
| AV1 default CDFs (`dav1d/src/cdf.c`, 10,970 literals) |                         ~21-35 KiB |
| `tables.c` plus `dequant_tables.c`                    |                            ~13 KiB |
| scan orders (`scan.c`)                                |       ~3 KiB, derivable at runtime |
| quantizer matrices (`qm.c`, 100,367 literals)         | ~98 KiB, only when `using_qmatrix` |

Intra-relevant dav1d code is **12,385 SLOC** before cutting inter prediction and threading;
realistically 8-10k written from scratch, against 8-10k for the entire rest of tinyimg. Shipped
reference points under the same packager and emscripten settings: `@jsquash/avif` decode wasm is
1,170,930 bytes against `@jsquash/webp` decode at 137,960.

Decode would run 4-12x the JPEG path per pixel, by mechanism rather than by guess: AV1's adaptive
multi-symbol arithmetic coder is serial and admits no SIMD where JPEG uses static Huffman, and AV1
applies three post-filters JPEG has none of. Encode is the wall the cost objective actually hits,
because AVIF's size advantage comes from rate-distortion search over partition trees and intra
modes: with the search it costs seconds per image in wasm, and without it the output loses to WebP
lossy.

**The surviving objective is AVIF output without per-transformation billing.** Codecs register
through a table, so a second wasm module can carry AV1 and be instantiated only by callers who ask
for it; core-module size then stops being a global budget question. What ships instead is WebP
lossy, which lands 25-35% below JPEG at matched quality with 97% browser support. AVIF would add a
further 20-30%.

## Verification

| Lane                    | Command                            |  Count |
| ----------------------- | ---------------------------------- | -----: |
| C unit and differential | `ctest --test-dir build-native`    |     51 |
| Sanitizers              | asan and ubsan over the same suite |     51 |
| Node                    | `bun run test:node`                |    281 |
| Workers runtime         | `bun run test:workers`             |     84 |
| C line coverage         | `bun scripts/coverage-c.ts`        | 96.02% |

Differential comparisons against ImageMagick, with the floor each one is asserted at:

| Comparison                                                                              | Result                             |
| --------------------------------------------------------------------------------------- | ---------------------------------- |
| gamma, brightness multiply, contrast about mid gray, box reduction at an integral ratio | byte identical                     |
| Catmull-Rom resize                                                                      | 103.5 dB                           |
| ICC conversion, Display P3 / Adobe RGB / Rec.2020 to sRGB                               | 49.1 / 49.1 / 49.3 dB              |
| gaussian blur, three box passes against a true gaussian                                 | 45.2 dB                            |
| text glyph shape at 256 px                                                              | 24 dB floor, 27.0 to 37.5 measured |
| text coverage, whole string at 32 px                                                    | within 2%, 99.5% measured          |

Where a floor looks low, the reason is in the test rather than in the code. Text is compared against
FreeType **with hinting on**, which moves stems onto pixel boundaries and rounds every advance to a
whole pixel; the shape error is a fixed number of pixels and shrinks against the glyph as the glyph
grows, from 29 dB at 32 px to 41 dB at 256. Whole strings are not compared by PSNR:
FreeType's integer advances accumulate, a twelve-glyph run drifts by a pixel or two, and the 17 dB
that produces would measure hinting rather than anything in this library.

Face detection is compared against OpenCV's own detector on identical pixels: 20 comparisons agree
within a pixel, and every non-face fixture reports zero detections in both.

### Four references nothing in this repository produced

Almost every codec reference is generated by `scripts/fixtures.ts`, which means it carries whatever
`magick`, `cjpeg` and `cwebp` assume when driven the way that script drives them. A reference set
built entirely that way cannot show a shared assumption being wrong. These four arrived from
elsewhere and are asserted **exact**, because they measured exact:

| Source               | Held against                                                                           | Result                                  |
| -------------------- | -------------------------------------------------------------------------------------- | --------------------------------------- |
| `dartmouth.jpg`      | `dartmouth.tiff`, the same picture decoded by an unrelated tool and written losslessly | byte identical over all 140,250 samples |
| `toyota_racing.webp` | libwebp's read of a lossy file encoded by someone else                                 | byte identical                          |
| `ball_kick.gif`      | ImageMagick's read of frame one of a production animation                              | byte identical                          |
| `digicam.jpg`        | libjpeg's read of a 9.72 Mpx photograph                                                | byte identical                          |

The WebP one is the strongest of the four. VP8 defines its dequantization, inverse transforms and
predictors in exact integer arithmetic, so two correct decoders agree sample for sample, and this
file exercises whatever modes and probabilities its own encoder chose rather than the ones ours
picks. The JPEG pair is the same argument for a format whose IDCT is only specified to a tolerance:
agreeing exactly with a standard decoder is a stronger statement than clearing a floor.

None of these is asserted against a PSNR floor, and that is the point. A floor sized to pass would
absorb a real regression in code that currently has none.

## Refuted approaches, with the measurement that refuted them

Recorded so nobody re-proposes them from a guess.

- **FFT convolution for large blur radii.** The premise is `O(n*r^2)`; a three-pass sliding-window
  box blur is `O(n)` and radius-independent. FFT would be `O(n log n)` plus a complex buffer plus
  3-4 KB of code: worse on every axis.
- **Integral images for box blur and local mean.** Same reason. An integral image costs an extra
  full pass and 4-8 bytes per pixel of sums to replace something already linear. Kept only where
  many overlapping rectangle sums are queried: the face cascade and smart-crop scoring.
- **Mipmaps for statistics.** Building a pyramid costs the O(n) pass the idea exists to avoid.
  `load_scaled` gives the same reduced representation for free, and for JPEG it falls out of the DCT.
- **Octree palette quantization.** The plan named it. Merging up to eight children into one leaf
  overshoots and spends palette entries nothing can recover; median cut produces exactly N boxes by
  construction, needs no node pool or leaf accounting, and measured **8.9 dB better** on `sf-24`
  with the same lookup and metric.
- **A 5-bit inverse color cube for palette lookup.** Cannot separate palette entries sitting inside
  one cell, so on a narrow-gamut illustration most entries were unreachable. A 6-bit table filled on
  demand by exhaustive search is worth **0.2 to 2.0 dB**.
- **A glyph raster cache.** Measured above: saves ~0.07 ms on an operation that is 1% of a request,
  and a cache in the arena dies on any `tiny_arena_reset`.
- **`-msimd128` as it stands.** Measured above at 0.97-1.02x, because nothing uses it. The objective
  survives; the flag as a free win does not.
- **General sparse and RLE raster representation.** Correct for a 10,000x10,000 canvas holding 20
  shapes, which is not this workload; the inputs are photographs. The display list already captures
  the shape case without a second raster representation.
- **Packed narrow pixel formats as a general mechanism** (RGB565, RGBA4444). Conversion cost eats the
  bandwidth saving over a single pass, and it loses precision the encoders then hide. The two useful
  cases, 1-channel gray and 1-channel alpha, the `channels` field already expresses.

## Measurement mistakes that cost real time

Written down because each one produced a number that was actionable and pointed the wrong way, which
is worse than having no number.

1. **Benchmarking the ctest library instead of the LTO build.** PNG decode read 151 ms/Mpx against a
   9 ms/Mpx target, which looked like a 16x design failure; the shipping build measured 54. The
   static library has no LTO, so the bit reader in `util.c` is a real call inside the hottest loop.
   The first instinct was to start rewriting the inflater. **It then happened again in Phase 6**,
   overstating text drawing 4.6x, despite a note describing the cause: a note about a cause does not
   stop you when what you are holding is a number. The tell is in the profile, where `tiny_sqrtf`
   and `tiny_floorf` appear as call frames.
2. **Comparing GIF output against a reference the format cannot produce.** Encode looked 7-12 dB
   behind ImageMagick on two RGBA illustrations. The reference was a 256-color RGBA image that kept
   partial alpha, which no GIF can express. Against ImageMagick's actual GIF output tinyimg wins on
   all seven fixtures. Three rounds of quantizer tuning went into a deficit that did not exist.
3. **Testing an invertible transform only against its own inverse.** The generated ICC profiles had
   their tone curves the wrong way round: `rTRC` is the decoding direction and the generator wrote
   the encoding one. Every self-consistent check passed, because a profile composed with its own
   inverse is the identity whichever way the curve runs. Apple's own sRGB profile caught it, reading
   0.2159 at the midpoint where ours read 0.7366.
4. **Trusting `%k` in ImageMagick as a palette-entry count.** It counts colors _used_. Three rounds
   of tuning chased an under-filled palette that was never under-filled.
5. **Two performance guesses before profiling.** A 64 KiB per-glyph arena allocation and the nine
   chunk spills it caused were both real inefficiencies, and neither moved the clock; one `sample`
   run found the actual 63% in a single attempt. The fixes were kept because they are correct on
   their own terms, taking heap peak from 152 KB to 87.
6. **A stale benchmark arm crediting the wrong cause.** The no-SIMD module is gitignored and cached,
   so after the decode work above the SIMD table read 1.24x for a flag that does nothing. Every
   number in it was real; the column header was wrong. `bench/run.ts` now refuses a stale arm.

The instrument-level version of the same lesson: **run an instrument twice and see whether it agrees
with itself.** `format.sh` skipped every untracked file, `tsc` shipped a package on error, and stale
`gcda` counters reported 44% on fully tested code. Each of those was a check that covered nothing
while reporting success.

### Vectorizing the JPEG inverse transform, as the obvious kernel

The transform is 49.7% of JPEG decode and an 8x8 IDCT is the canonical SIMD kernel, so the first
plan was to write one. Counting first is what stopped it: **93.0% of columns on `sf-24.jpg` carry no
vertical detail** and already skip the transform entirely, so a four-wide column pass would do full
transforms for groups in which almost every lane needs none, and lose. The work is not spread the way
the total suggests. Pass 1 runs ~50,000 real transforms against pass 2's 712,080.

What the count found instead was the redundancy above: when every column is flat the row pass computes
one row eight times. Deleting that was worth 20.8% on the reference fixture for six lines and no
intrinsics, where the vectorized column pass would have been several hundred lines to lose.

The objective survives and is narrower than it was: **pass 2, the row transform, is the SIMD target**,
along with WebP's loop filter at 41.5%. Both are unconditional per-pixel work with no shortcut to
exploit, which is the shape a wider kernel suits.
