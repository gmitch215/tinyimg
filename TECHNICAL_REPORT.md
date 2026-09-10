# tinyimg Technical Report

Measurements, and the approaches they refuted. Every number here came from the artifact that ships
(`bin/tinyimg.wasm`, built with `-Oz -flto`) or from a purpose-built arm; none is an estimate.

Reproduce with `bun run size`, `bun bench/run.ts`, `bun run test`, and
`bun scripts/coverage-c.ts`. Every compressed figure is `gzip -9`, the level `bun run size`
uses; the default level reports about 1.4% larger, so a number here will not match a casual
`gzip -c | wc -c`.

## Size

**Size stopped being the primary goal on 2026-09-04**, when Cloudflare removed the compressed Worker
size limit. The only platform limit is now 64 MiB uncompressed, on every plan; the 3 MB and 10 MB
compressed limits this library was shaped around are gone. Speed comes first now, then size, then
memory.

The figures below are uncompressed, because that is the axis Cloudflare checks. `bun run size`
reports against a 1 MiB target and a 1.5 MiB ceiling, both of which are this project's own numbers
rather than the platform's, and they exist so that `tiny` keeps meaning something.

| Measure      |       Bytes |
| ------------ | ----------: |
| **raw wasm** | **777,426** |
| gzip         |     312,073 |
| brotli       |     257,373 |

The module was 190,309 raw at `-Oz`. Building the same sources at `-O2` is **1.45x to 3.54x faster**
depending on the stage and every output digest is byte-identical, so the switch cost bytes and
nothing else. `-O3` is indistinguishable from `-O2` on every measurement and costs a further 71 KB,
so `-O2` is what ships.

Startup does not constrain this either. Compile is linear at 6.8 to 8.9 microseconds per kilobyte,
so a 1 MiB module is about 8 ms of the 1 second Workers allows a global scope, and instantiate,
which is what a warm isolate actually pays, is 0.18 to 0.23 ms and roughly flat.

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

Removing several at once is not the sum of their arms, for the same reason. Dropping WebP, TIFF, GIF
and AVIF together gives **366,325 raw / 152,733 gzip**, a 52.9% reduction, and `tiny_features`
reports the four as absent:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32.cmake \
  -DCMAKE_C_FLAGS="-DTINYIMG_NO_WEBP -DTINYIMG_NO_TIFF -DTINYIMG_NO_GIF -DTINYIMG_NO_AVIF"
```

### Where the estimates were wrong

The plan projected ~131,000 raw and ~45,000 gzip. At the phase the projection covered the module was
190,309 and 90,360, so the estimate was low by 45% and 101%; the current figure is larger again
because it now carries an AV1 decoder and encoder the plan did not price at all. Two regions account
for almost all of the original miss:

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
| decode jpeg     | 11.1 ms |           |  12.1 ms |           | 1.09x |
| encode jpeg q80 | 28.5 ms |    73,513 |   114 ms |    47,340 | 3.99x |
| decode png      | 24.0 ms |           |  25.9 ms |           | 1.08x |
| encode png      |  260 ms |   636,919 |  19.9 ms | 1,209,393 | 0.08x |
| decode webp     | 25.9 ms |           |  18.7 ms |           | 0.72x |
| encode webp q80 | 78.8 ms |    54,506 |   109 ms |    29,788 | 1.38x |

From `bench/results/latest.md`, which `bun bench/run.ts` regenerates. A second run of the same
harness put the ratios at 1.06x, 3.79x, 1.03x, 0.08x, 0.69x and 1.31x, so read the third digit as
the machine rather than the code.

Those are the `-O2` figures. At `-Oz` the same rows read 0.47x, 2.30x, 0.84x, 0.05x, 0.55x and
0.69x, so **the build level accounted for most of what used to look like an algorithmic gap.** PNG
encode is the row it did not rescue, and the two operating points below still apply to it.

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

Decode is the comparison without an operating point to argue about. tinyimg used to lose all three
rows of it; it now wins JPEG and PNG by a few percent and loses WebP by 1.4x. PNG moved most
recently, from 0.96x, when the DEFLATE fast table went from nine bits to eleven.

**An earlier version of this report explained the gap by saying all three comparison libraries have
SIMD implementations of their hot kernels and this one has none. Both halves were false.** Counted
with `wasm2wat --enable-all`: `mozjpeg_dec.wasm`, `webp_dec.wasm` and `squoosh_png_bg.wasm` contain
**zero** SIMD instructions between them, and `bin/tinyimg.wasm` contained **548** at `-Oz` and 8,552
at `-O3`. `squoosh_png_bg.wasm`'s own target-features section lists `mutable-globals` and `sign-ext`
and nothing else.

The gap was an optimization level and a code structure. Building the same sources at `-O2` closed
half of it, and the rest is accounted for stage by stage below. A wasm module is inspectable, so
there was never a reason to infer this.

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

**Those JPEG figures are a `-O2` native profile of one smooth 4:4:4 photograph and they do not
generalize.** Re-measured in wasm at the level that ships, by replacing one stage at a time with a
stub:

| fixture                   | output stage | inverse transform |
| ------------------------- | -----------: | ----------------: |
| `sf-24.jpg`, 4:4:4        |        38.9% |             23.8% |
| `mountains.jpg`, 4:2:0    |        43.5% |             13.8% |
| `road.jpg`, progressive   |        32.4% |             11.3% |
| `dog.jpg`, 4:2:0, 6.0 Mpx |        59.8% |             15.1% |

**The output stage is the largest block in JPEG decode, not the transform**, and entropy decoding is
**28.8%** on a detailed 4:2:0 photograph rather than the 10.5% above. 10.5% is what a smooth 4:4:4
file with 3.68 coded symbols per block looks like. The clue was already in the ratio table: a 4:4:4
JPEG sits at 0.84x and a 4:2:0 file of nearly the same size at 0.60x, and 4:4:4 is the one case with
no chroma to upsample.

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

#### The cost model's scale factor was one codec's, applied to all of them

`cost_of` charged a scaled decode as the full decode times a factor per denominator, and the factors
were **JPEG's**, measured on the reference photograph. Only two codecs earn them: JPEG reduces in the
DCT domain and BMP skips rows, while the rest decode the whole frame and box average afterwards.

Measured per format, five interleaved repeats of a median of fifteen, every cell within 2%:

| Format | 1/2 factor | 1/4 factor | 1/8 factor |
| ------ | ---------: | ---------: | ---------: |
| JPEG   |      0.350 |      0.290 |      0.203 |
| BMP    |      0.527 |      0.384 |      0.358 |
| TIFF   |      0.847 |      0.811 |      0.804 |
| AVIF   |      0.919 |      0.879 |      0.865 |
| GIF    |      0.953 |      0.882 |      0.872 |
| WebP   |      1.024 |      0.993 |      0.975 |
| PNG    |      1.619 |      1.559 |      1.547 |

The shared factors were 0.674, 0.540 and 0.337. So **PNG was charged a 33% saving where a half scale
decode costs 62% more**, and the sign is the problem rather than the magnitude: a budget took the
scale rung expecting the request to get cheaper. At a denominator of one a PNG row is a single
`tiny_memcpy`; above it the same row goes through `expand_row` and `accumulate`, which is work the
denominator adds rather than removes. WebP and GIF are close enough to 1.0 that the rung buys them
nothing either.

Three things fell out of fixing it. **The capability rule this was going to need is unnecessary**:
the ladder already skips a rung that does not come out cheaper, so it was blind rather than wrong,
and correct factors are what let it see. **AVIF was priced at zero**, having no case in
`decode_rate`, so a budgeted AVIF request was free according to the model and is now the most
expensive format at 26,461 us per megapixel. And **`cost_of` was re-probing the source header on
every call**, six times for a budget ladder, when `tiny_plan_init` had already put the extent and
format on the plan.

`effort_fraction` had to become denominator-aware for WebP in the same pass, because the two levers
do not multiply: FAST skips the deblocking filter at any scale and additionally reduces in the plane
domain above a denominator of one, so its saving grows with the denominator while the scale factor
was measured at full effort. One number over-charged a scaled FAST request by 2.2x to 2.6x.

`scripts/measure/estimate.ts` scores the model against the clock per format and per budget, with a
band of 0.6x to 2.0x. It is a periodic eval and not a gate test, because the rates describe one
machine. It refuses to run at a load average above a quarter of the core count: a run at load 24
doubled every clock and left every estimate identical, which reads as the model under-predicting by
half across the board, and the estimates being byte-identical is the only thing that gave it away.

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
| VP8 lossy, scaled   | the full resolution conversion       | 2.05x - 3.35x | 38.9 - 48.7 dB |
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
JPEG, which has no chroma to upsample.

#### The compressor is a budget lever, and it was the one nothing could reach

For a PNG or TIFF output the encoder is the larger half of the request, and the plan was handed
`budgetMs - encodeMs`. For PNG that difference is already negative at every useful extent, so it was
floored at a microsecond and the planner then gave up the filter and three halvings of the decode to
save a fraction of what the compressor was spending. Softening the picture to pay for the compressor,
in that order.

`tiny_encode_cost_at` prices a level, and a budgeted request steps `default` to `fast` to `none`
before touching the plan. Measured on the reference photograph, five interleaved repeats:

| Level     | 400 wide | 800 wide | Bytes at 400 | Bytes at 800 |
| --------- | -------: | -------: | -----------: | -----------: |
| `none`    |    2.45x |    1.55x |       64,783 |      222,574 |
| `fast`    |    2.06x |    1.57x |       78,102 |      225,332 |
| `default` |    1.00x |    1.00x |       69,337 |      200,371 |
| `best`    |    0.34x |    0.32x |       65,798 |      188,866 |

End to end a budgeted PNG request measures **3.7x at 400 wide (46.3 ms to 12.6) and 2.8x at 800
(89.0 to 31.3)**, and it reports `compression` in `degraded`. Naming a level pins it.

**`none` is not always a trade.** At 400 wide it produced a file 6.6% smaller than `default` as well
as 2.45x faster, because one greedy short match per position codes a filtered photograph row worse
than entropy coding the literals. It becomes a real trade at 800, where it costs 11% more bytes. The
conservative end of each measured range is what the constants use, because a budget promised a saving
it does not get reports that a request fits when it does not.

#### Widening the DEFLATE fast table, and the file it does not help

`TINY_DEFLATE_FAST_BITS` went from 9 to 11. On `forest.png` that is **1.10x (40.28 ms to 36.50)**;
on a 2400x1350 photograph it is 1.01x.

The difference between the two files is how well they compress. `forest.png` carries 3.74 MB of IDAT
for 4.99 MB of pixels, a ratio of 1.33, so its literals take long codes that a nine bit table misses
and that fall back to walking the canonical code a bit at a time. The photograph compresses 10.9x and
its codes were already inside nine bits. **The win lands on the low-ratio files, which are the
expensive ones**, and that is the useful direction. Twelve bits bought a further 0.6% for twice the
table and was not taken. The cost is 6 KiB of arena, and nothing holds a table on the stack.

#### A reduction had one thing to drop after all, and it was not a filter

The line above used to end by saying a reduction keeps its filter, because an area average is both
the cheapest option and the correct one. The average was never the cost. **Converting the samples
before averaging them was**, and a scaled WebP request pays it 64 times over at a denominator of
eight.

Stubbing out `planes_to_rgba` prices it: 4.85 ms of 19.46 at `den` 1 and 5.25 of 19.02 at `den` 8, so
**25% to 27% of the decode, and it does not shrink as the output does.** A scaled WebP decode was
flat to within 8% across the whole ladder, which is what a stage proportional to the source rather
than the output looks like.

The conversion is affine in Y, U and V, so `planes_to_region` averages the planes over each output
pixel's box and converts once. At `den` 8 the decode lands on 13.80 ms against the stub arm's 13.77,
which is the confirmation the stage is gone rather than merely cheaper, and the full frame of RGBA is
never allocated: 7.57 MB on this fixture.

**It is behind FAST, and the measurement is why.** Two references disagree about it:

| Reference                           | `den` 2 | `den` 4 | `den` 8 |
| ----------------------------------- | ------: | ------: | ------: |
| the original, box-reduced (neutral) |   +0.18 |   +0.22 |   -0.04 |
| the exact box average of the decode | -46 lvl | -20 lvl | -11 lvl |

Against a reduction of the source image the fold is a wash, marginally ahead at two of three
denominators. Against the box average of the library's own full decode it is up to 46 levels off, and
that one is not a preference: `resample_region` computes exactly that average, `TinyDecodeOpts`
documents it, and the agreement between a scaled WebP and a scaled PNG of one picture follows from
it. Chroma is the whole difference. The fold takes the mean of the box's chroma samples; the exact
path takes the mean of the triangle upsample of them, and the conversion's clamp lands per source
pixel instead of once.

So the speed is real, the fidelity is a wash, and the exactness is a documented property. The tier is
where this library puts that trade, and it already puts VP8's filter there. FANCY stays byte-exact,
which the tests now assert for the first time; FAST reduces in the plane domain for **2.05x to 3.35x
together with the filter skip**, floor 38.9 dB. Alpha carries no chroma and no clamp, so it stays
byte-exact at either effort.

One case is deliberately not folded. A frame coded smaller than its canvas has no zeroed gap to
average, because the gap only exists in the RGBA plane the fold skips, so those convert as before.

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

Workers Free allows 10 milliseconds of CPU per request, not configurable, so a request either fits
or fails. The limit is enforced elastically rather than as a hard ceiling, which is worth knowing
before testing against it: see "The limit is enforced elastically" below.

Measured end to end in wasm from `sf-24.jpg`, at the bounded effort:

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

| source    | megapixels | fixture                | 200 wide JPEG |
| --------- | ---------: | ---------------------- | ------------: |
| 320x180   |       0.06 | `derived/base-444.jpg` |       1.63 ms |
| 1835x1032 |       1.89 | `sf-24.jpg`            |       5.05 ms |
| 1920x1250 |       2.40 | `mushroom.jpg`         |      10.57 ms |
| 2308x3000 |       6.92 | `family.jpg`           |      12.33 ms |
| 3600x2700 |       9.72 | `digicam.jpg`          |      53.95 ms |

Every one of them decoded a comparable fraction of its source's samples. The spread is the entropy
decode, which no output size can reduce, so "will this fit" is a question about the picture that
arrived. This is why `tiny_plan_cost` prices a plan against source samples, and why a cache key is
part of the library: for a source past a couple of megapixels there is no output small enough, and
the answer has to be that the artifact is computed once.

The fixture column is there because an earlier version of this table quoted a 5000x4000 source that
no fixture in the repository has, which made the row unreproducible. Every row above names the file
it was measured on.

**Not reachable:** 800px WebP inside 10 milliseconds. The encoder's non-search work alone is about
14 ms at that extent and decode with resample is 17.3, so a free mode search would still leave 31.
What that closes is the synchronous path for that one request. It does not close serving 800px WebP
on the Free plan, which the cache tier does, at 200 to 250px, which fits, or on Paid.

### The limit is enforced elastically, so a one-shot measurement of it means little

Deploying a Worker to a real Free account and running a pure CPU burner **consumed 1,637 ms and
returned a result**, with terminations between 1,459 and 2,020 ms reported as `outcome: exceededCpu`.
That is roughly 150x the documented 10 ms.

**It is not a 150x allowance, and reading it as one would be the mistake.** Cloudflare's per-request
limits are not hard ceilings: an isolated request over the limit passes through a flexibility scope,
and sustained pressure against it starts being enforced. So a single burst measures the slack, not
the budget. The same code deployed in succession, or serving real traffic, meets the documented
figure.

Which makes the 10 ms above the right line to plan against, and every figure in this section is
compared against it deliberately. The measurement is worth recording for one reason only: **a
single test request is not evidence that a request fits.** Anything sized against the observed
1,637 ms would fail under load, and the failure would arrive later and look unrelated.

Two other results from the same run are properties of the platform rather than of its enforcement,
and are reused throughout this report.

**Nothing bills as I/O.** A 2.88 MB response body added 99 ms of wall clock and **zero** CPU, and a
`noop` handler measures 0 ms. So there is no arrangement of the work that hides compute behind a
stream, and wall clock is not the axis to optimize.

**darwin to Workers is 1.90x at the median and 2.4x to 4.7x on the cheap cells.** The spread matters
more than the median, because the cheap cells are exactly the ones near any ceiling: a local figure
under 20 ms should be scaled by about 3.5x rather than by the median before it is compared against a
per-request allowance. Every millisecond in this report is darwin unless it says otherwise.

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

Both directions ship. Decode is complete AV1 intra and matches `dav1d` exactly; encode is fixed
partitions with no rate-distortion search.

### What the decode agrees with

`avifdec`, which is libavif over `dav1d`, is the anchor. Every fixture, in RGB:

| fixture                                   | worst sample | PSNR      | exact |
| ----------------------------------------- | -----------: | --------- | ----: |
| av1-tiny, av1-flat, av1-deltaq, dartmouth |        **0** | identical |  100% |
| av1-lossless                              |            1 | 81.8 dB   |  100% |
| av1-tiles                                 |            1 | 82.1 dB   |  100% |
| base                                      |            1 | 80.5 dB   | 99.9% |
| fox                                       |            2 | 54.4 dB   | 76.8% |

**`fox.avif`'s decoded planes are byte-identical to `dav1d`'s**, all 2,889,600 of them, measured
against `avifdec`'s own Y4M output. So the difference in RGB is the conversion alone: a fixed-point
matrix against libavif's floating-point one, and 4:2:0 chroma interpolation. The decode of a
1204x800 photograph through the symbol decoder, the partition tree, the mode info, 236,096
coefficients, the prediction and both post-filters matches bit for bit.

The chroma upsampling had to be measured rather than chosen. Nearest neighbour gives 48.08 dB
against `avifdec` and a worst case of 16; the triangle filter libjpeg calls fancy upsampling, which
this library already runs for JPEG, gives **54.40 dB and a worst case of 2**. So the reference
decoder interpolates. `FANCY` interpolates and `FAST` replicates, which is the same trade the JPEG
path takes.

### Two places the specification misleads

**A prose note is not normative over the function it annotates.** Section 7.15.3 says CDEF's filter
region is the tile that decoded the block, in a note about `MiColStart` and friends;
`is_inside_filter_region` in 5.11.55 then sets the bounds to the whole frame and ignores them.
Implementing the note cost 1,462 samples of a four-tile fixture in a band two columns either side of
each seam, which is exactly CDEF's tap reach. `avifdec` agrees with the function.

**`ReadDeltas` is per superblock, not per block.** Reading a quantizer delta for every block
consumes symbols the encoder never wrote, and the first block still comes out right, so it reads as
rare corruption rather than as a bug.

### The encoder, and what it costs

Fixed 8x8 blocks, one 8x8 luma transform and one 4x4 per chroma plane, the luma mode picked from
four candidates by absolute difference against the source. That is a distortion comparison and not a
rate-distortion one: no candidate is ever costed in bits. Against `avifenc` on `dartmouth.jpg`:

| encoder     |  bytes | PSNR     |
| ----------- | -----: | -------- |
| avifenc q63 | 13,864 | 31.82 dB |
| tinyimg q60 | 16,940 | 33.38 dB |
| avifenc q75 | 18,743 | 37.13 dB |
| tinyimg q75 | 21,219 | 36.59 dB |
| avifenc q90 | 25,182 | 43.33 dB |
| tinyimg q90 | 29,096 | 40.34 dB |

So **1.1x to 1.3x libaom's size at matched quality**, widening as quality rises. An earlier
prediction in this section was that the output would "roughly match WebP lossy instead of beating it
by 20-30%"; the measurement is better than that, and the gap is a partition search rather than a
missing optimization.

`avifdec` decodes what the encoder writes, and its pixels match this decoder's to within one level,
so the container, both headers and the arithmetic coding are conformant rather than merely
self-consistent.

### The arithmetic encoder's window, which is where the bug was

The specification defines no encoder, so the writer is derived from the reader: symbol `s` occupies
`[cur(s), cur(s - 1))` of `[0, range)`, which makes writing it `low += range - cur(s - 1)` and
`range = cur(s - 1) - cur(s)`.

The retained window has to be **16 bits and not 15**. `range` renormalizes into `[2^15, 2^16)`, so a
sixteen-bit window guarantees `low + range < 2^17`, which is one carry out of the window and no
more; a fifteen-bit window allows two, and absorbing a carry of two by incrementing one byte
corrupts the stream. That passed a million symbols of uniform distributions and failed after twenty
thousand symbols of a skewed one, which is what an adapted coefficient distribution looks like. The
regression test drives skewed adapting distributions for exactly that reason.

### What is refused, and why

**Ten and twelve bit samples**, because the reconstruction is eight bits a sample end to end: the
frame store, the prediction and the conversion would all have to widen, and truncating instead would
return a plausible wrong picture. 40 of the 115 conformance files are 10-bit, so this is a real gap
rather than a theoretical one.

**4:2:2**, because the specification's own `Subsampled_Size` answers `BLOCK_INVALID` for every tall
block on such a plane and the residual syntax reads that entry directly.

**Loop restoration**, because its filter is skippable and its symbols are not: they are coded per
superblock inside the tile, so a tile read without them desynchronises.

**Palette and intra block copy**, at the frame rather than at a block, so a caller learns before any
pixel is decoded.

### What the sizing estimates got right and wrong

**Size is not the reason, and neither is the cost.** All of dav1d 1.5.1 compiled to wasm32, scalar,
8-bit only, _including_ the inter prediction and threading this library does not need, is 356,669
bytes. What was measured for the tables:

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

**An earlier version of this section said decode would run 4-12x the JPEG path per pixel, "by
mechanism rather than by guess". No experiment produced that range, and the measured figure is 2.1x
to 2.7x.** The error was the comparison arm: the inference was priced against `@jsquash/avif`, which
contains the string `AOMedia Project AV1 Decoder v3.7.0` and zero SIMD instructions. That is
libaom, the reference decoder, scalar, and libaom is 5.5x to 6.4x the JPEG path where a dav1d-class
decoder is 2.1x to 2.7x. One `strings` call on the arm would have caught it.

Measured in wasm, milliseconds per megapixel:

| arm                             | 1 Mpx | `road.jpg` |    vs JPEG |
| ------------------------------- | ----: | ---------: | ---------: |
| tinyimg JPEG                    | 10.72 |      12.76 |      1.00x |
| **AV1 intra, no post-filters**  | 16.77 |      25.98 | 1.51-2.04x |
| tinyimg WebP, which ships today | 20.20 |      30.15 | 1.88-2.36x |
| AV1 intra, all filters          | 23.72 |      34.29 | 2.08-2.69x |
| libaom through `@jsquash/avif`  | 61.30 |      81.67 | 5.52-6.40x |

**An AVIF decode with the post-filters skipped is faster than the WebP decode already shipping
here.** At 10 ms of CPU the source ceilings are 0.78-0.93 Mpx for JPEG, 0.38-0.62 for AVIF without
filters, 0.33-0.50 for WebP and 0.29-0.42 for AVIF with them, so AVIF fits the request shape WebP
already occupies.

Skipping those filters is legal by construction rather than by analogy. Section 7 puts the loop
filter, CDEF and loop restoration in the frame wrapup process, after all tile decode; intra
prediction reads `CurrFrame` during tile decode, before any of them run. Over 115 files from the AOM
conformance suite the skip measures a median **1.72x at 54.2 dB**, with one file below 45 dB and none
below 40. The VP8 lever already shipping is 1.53x at 46.8 dB, so this one is faster and cleaner.

Two things the tables above got wrong and that are worth correcting rather than deleting. Quantizer
matrices are listed as "only when `using_qmatrix`", but **every file `avifenc` produced in testing
has `qm=1`**, so they are the default path for the dominant still encoder and absent from all 115
conformance files. And high bit depth costs code size rather than speed: 33.02 against 33.09 ms per
megapixel for the same picture, but roughly 100 KB of module, against 40 of those 115 files being
10-bit and 9 being 12-bit.

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
- **Writing the inflate output into the caller's buffer instead of through the ring.** Estimated at
  16.2% of a PNG decode and 1.15x, for 60-90 lines and a memory increase. Measured at the sizes a
  real decode moves through it, the ring's copy-out is **0.079 ms on `forest.png` and 0.157 on a
  2400x1350 photograph, which is 0.20% and 0.41%** of those decodes. `gather_idat`'s copy of the
  whole compressed stream is **0.04% to 0.15%**, so a chunk-walking source buys no measurable time
  either; it would save the allocation, which is a memory item and not a speed one. The 16.2% came
  from attributing a 33.4% block called "inflate + ring + `gather_idat`" to the copying around the
  inflate rather than to the inflate. **The objective survives and is now specific**: the symbol
  decode is where that third of the time is, and widening the fast table is the first lever on it.
  The largest of the three passes turned out to be the Adler-32 at 4.1% to 8.3%, which is the
  checksum that makes a corrupt PNG detectable and is not a candidate for removal.
- **The exact-fit decode denominator, on every resize.** The denominator pick in `plan.c` compares
  the real quotient `region / den` against the output width, so it refuses a denominator whose
  `ceil(region / den)` would have hit the output size exactly. Matching the ceiling instead admits
  that exact fit, and there the resample degenerates to a copy: the decoder's scaler becomes the
  only filter in the path. Measured against a 16-bit linear-light Lanczos reduction, five of six
  exact-fit cells lose **1.3 to 2.4 dB** for 1.3x-6.0x, and the one free cell (`sf-24` at
  459x258, -0.02 dB) does not reproduce on `road.jpg`, where the same denominator step costs
  2.37 dB. The speedup is real and the budget ladder's scale rung already reaches it under a
  budget; what is refuted is taking it by default.

## Measurement mistakes that cost real time

Written down because each one produced a number that looked usable and pointed the wrong way, which
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
7. **Scoring two resamplers against a reference one of them ends in.** The denominator arms above
   were first compared against a Catrom reduction, which is the kernel `fixtures.ts` uses and the
   kernel the surviving arm finishes with. That reference read the gap as 3.46-4.83 dB; a
   linear-light Lanczos reduction, which neither arm imitates, read the same gap as 1.31-1.64. Two
   thirds of the deficit was agreement with one filter rather than fidelity to the scene. This is
   mistake 2 one level up: the reference was something the pipeline can produce, and that is what
   made it the wrong reference.

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

8. **Explaining a comparison without inspecting the arm.** The decode deficit against `@jsquash` was
   attributed to SIMD in their kernels and none in ours. Both halves were false, and one `wasm2wat`
   call over the three modules would have shown it: zero SIMD instructions in theirs, 548 in ours.
   The same mistake cost the AVIF section a 4-12x figure that was really 2.1-2.7x, because it was
   priced against `@jsquash/avif`, which carries libaom rather than a modern decoder. Check what the
   comparison contains before explaining why it wins.

9. **Measuring one lever through a flag that moves four.** `effort` changes the chroma upsample, the
   enlargement filter, the loop filter and the encoder search at once, so pricing the PNG encoder
   through it produced output that was _smaller_ at FAST than at FANCY, twice, on different
   fixtures. FANCY keeps the smaller of two candidate streams, so that result is arithmetically
   impossible, and the impossibility was the only thing that flagged it; both runs were comparing
   different images. Isolate the stage, then quote the number.

10. **Recalibrating a model whose input variable is wrong.** Every cost constant was stale once the
    build moved to `-O2`, so all of them were re-measured. Accuracy did not move: worst error 12.93x
    before and 12.78x after. The constants were never the problem. Source samples and compressed
    bytes each predict well on one half of the scale ladder and badly on the other, so the model wants
    both terms and fitting one of them harder cannot help.

11. **Believing a saving before measuring it at the extent the request asks for.** The progressive
    scan skip is exact at a scale denominator of eight and worth 4.4x there. On the fixture that
    motivated it a 200 px output picks a denominator of four, so the lever never fires. Its first
    implementation also tested the frame's transform size rather than the component's, which read 2x
    better and quietly dropped the chroma planes' detail.
