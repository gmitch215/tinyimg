# Estimate against the clock, bench/arms/tinyimg-v1.1.1.wasm

Machine darwin-arm64, Apple M2 Pro (12 cores), node v26.3.0, wasm sha256 8851e695962e8796caae504858b86c70823dd96e3cd443ee0365705c2e81b441, load average 2.91 before and 2.93 after, 2 warmups and 9 timed runs per cell, median reported.

Error is `log2(estimate / measured)`: 0 is exact, +1 is a 2x over-estimate, -1 is a 2x under-estimate. The band estimate.ts asserts, 0.6x to 2.0x, is -0.74 to +1.00. The estimate is `Image.decide().estimateMs` (decode and resample, no encoder); the clock is the whole request including the encode. The `worst over` and `worst under` columns give the log2 value and the ratio.

## Cells

3132 cells: 2512 timed, 110 failed, 510 skipped. 0 timed cells have a zero estimate (excluded from every table). 62 timed cells are flagged `alpha` (webp output from a 4-channel plan; the model predates the ALPH encode) and are listed apart from the headline.

Failed cells by error (10 images):

- 18 cells: TinyImgDataError: probe the image: corrupt data
- 60 cells: TinyImgFormatError: run and encode: unsupported variant
- 20 cells: TinyImgMemoryError: run and encode: out of memory
- 12 cells: TinyImgTooLargeError: run and encode: image too large

Failed images:

- `commons-jpeg-67065530` jpeg 8.90 Mpx: TinyImgMemoryError: run and encode: out of memory
- `commons-webp-133583128` webp 12.94 Mpx: TinyImgMemoryError: run and encode: out of memory
- `commons-webp-137998502` webp 11.92 Mpx: TinyImgMemoryError: run and encode: out of memory
- `commons-webp-153900841` webp 12.67 Mpx: TinyImgMemoryError: run and encode: out of memory
- `commons-webp-176333003` webp 17.19 Mpx: TinyImgTooLargeError: run and encode: image too large
- `commons-tiff-157663857` tiff 8.55 Mpx: TinyImgFormatError: run and encode: unsupported variant
- `commons-tiff-165927995` tiff 0.48 Mpx: TinyImgFormatError: run and encode: unsupported variant
- `commons-tiff-187158638` tiff 53.25 Mpx: TinyImgDataError: probe the image: corrupt data
- `commons-tiff-55648005` tiff 0.25 Mpx: TinyImgFormatError: run and encode: unsupported variant
- `avif-commons-jpeg-2955836` avif 3.15 Mpx: TinyImgFormatError: run and encode: unsupported variant

Skipped cells:

- 510 cells: width exceeds source width (enlargement)

## The 12.8x question

- test (headline, alpha excluded): 10.97x under-estimate (log2 -3.46), cell `commons-jpeg-8099929` jpeg 2.97 Mpx, width 400, budget 10, output webp, scale 1/4, estimate 9.68 ms, measured 106.22 ms.
- cal (alpha excluded): 9.65x under-estimate (log2 -3.27), cell `avif-commons-jpeg-2717440` avif 0.26 Mpx, width 200, budget 10, output source, scale 1/2, estimate 7.36 ms, measured 71.02 ms.
- alpha cells: 3.46x over-estimate (log2 1.79), cell `commons-tiff-100837335` tiff 0.96 Mpx, width 200, budget 10, output webp, scale 1/8, estimate 11.71 ms, measured 3.39 ms.
- the five local images (alpha excluded): 4.96x under-estimate (log2 -2.31), cell `local-steam` png 2.07 Mpx, width 800, budget 10, output source, scale 1/1, estimate 60.47 ms, measured 299.70 ms.
- the 12 JPEGs README.md:169 describes: the corpus manifest carries no `legacy` marker, so they cannot be identified and are not reported apart.

- test: worst over-estimate 2.71x, worst under-estimate 10.97x.
- cal: worst over-estimate 4.33x, worst under-estimate 9.65x.

## Headline: test split, alpha cells excluded

### by source format

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 878 | -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) | 38.6% |
| jpeg | 492 | -1.04 | -2.22 | 0.02 | 1.44 (2.71x over) | -3.46 (10.97x under) | 31.9% |
| png | 158 | -0.62 | -2.34 | 0.08 | 0.54 (1.45x over) | -2.95 (7.74x under) | 60.8% |
| webp | 84 | -0.53 | -1.27 | 0.10 | 0.58 (1.49x over) | -1.87 (3.65x under) | 59.5% |
| gif | 36 | -0.03 | -0.44 | 0.18 | 0.24 (1.18x over) | -0.73 (1.65x under) | 100.0% |
| avif | 108 | -2.19 | -2.93 | -1.32 | -0.89 (1.85x under) | -3.07 (8.42x under) | 0.0% |

### by scale denominator

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 878 | -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) | 38.6% |
| 1/1 | 204 | -0.78 | -2.58 | -0.08 | 0.54 (1.45x over) | -2.95 (7.74x under) | 48.0% |
| 1/2 | 232 | -0.97 | -2.80 | -0.19 | 0.40 (1.32x over) | -3.44 (10.84x under) | 41.4% |
| 1/4 | 134 | -1.25 | -2.05 | -0.06 | 0.33 (1.26x over) | -3.46 (10.97x under) | 29.1% |
| 1/8 | 308 | -1.08 | -2.23 | 0.34 | 1.44 (2.71x over) | -2.89 (7.41x under) | 34.4% |

### by budget

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 878 | -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) | 38.6% |
| none | 433 | -1.02 | -2.67 | -0.07 | 1.43 (2.70x over) | -3.33 (10.08x under) | 37.2% |
| 10 ms | 445 | -0.99 | -2.21 | 0.17 | 1.44 (2.71x over) | -3.46 (10.97x under) | 40.0% |

### by output format

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 878 | -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) | 38.6% |
| source | 390 | -1.04 | -2.37 | 0.16 | 1.44 (2.71x over) | -3.42 (10.68x under) | 35.1% |
| jpeg | 144 | -0.51 | -2.53 | 0.18 | 0.58 (1.49x over) | -3.02 (8.13x under) | 63.9% |
| webp | 344 | -1.08 | -2.36 | -0.19 | 1.14 (2.21x over) | -3.46 (10.97x under) | 32.0% |

### by width

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 878 | -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) | 38.6% |
| 200 | 364 | -1.06 | -2.55 | 0.10 | 1.44 (2.71x over) | -3.07 (8.42x under) | 36.5% |
| 400 | 342 | -1.02 | -2.44 | -0.08 | 1.27 (2.41x over) | -3.46 (10.97x under) | 36.0% |
| 800 | 172 | -0.74 | -1.58 | 0.24 | 1.29 (2.44x over) | -2.90 (7.47x under) | 48.3% |

## Calibration split, alpha cells excluded

### by source format

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 1572 | -0.90 | -2.05 | 0.21 | 2.11 (4.33x over) | -3.27 (9.65x under) | 43.0% |
| jpeg | 974 | -1.12 | -2.00 | -0.06 | 1.58 (2.99x over) | -2.91 (7.52x under) | 31.3% |
| png | 226 | -0.51 | -2.34 | 0.18 | 0.60 (1.51x over) | -2.93 (7.64x under) | 64.6% |
| webp | 94 | -0.56 | -1.18 | 0.46 | 0.92 (1.89x over) | -1.88 (3.69x under) | 70.2% |
| gif | 98 | -0.16 | -0.87 | 0.36 | 0.68 (1.60x over) | -2.56 (5.90x under) | 84.7% |
| tiff | 30 | 1.01 | -0.06 | 1.79 | 2.11 (4.33x over) | -1.79 (3.46x under) | 40.0% |
| bmp | 72 | -0.29 | -0.76 | 0.34 | 0.86 (1.81x over) | -0.88 (1.84x under) | 88.9% |
| avif | 78 | -2.56 | -3.10 | -1.45 | -0.98 (1.98x under) | -3.27 (9.65x under) | 0.0% |

### by scale denominator

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 1572 | -0.90 | -2.05 | 0.21 | 2.11 (4.33x over) | -3.27 (9.65x under) | 43.0% |
| 1/1 | 279 | -0.65 | -2.56 | 0.01 | 0.86 (1.81x over) | -2.95 (7.70x under) | 55.6% |
| 1/2 | 382 | -0.78 | -1.65 | -0.06 | 1.21 (2.32x over) | -3.27 (9.65x under) | 48.4% |
| 1/4 | 275 | -1.11 | -1.85 | 0.01 | 1.97 (3.91x over) | -2.99 (7.95x under) | 31.3% |
| 1/8 | 636 | -1.13 | -2.06 | 0.34 | 2.11 (4.33x over) | -3.04 (8.25x under) | 39.3% |

### by budget

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 1572 | -0.90 | -2.05 | 0.21 | 2.11 (4.33x over) | -3.27 (9.65x under) | 43.0% |
| none | 782 | -0.90 | -2.10 | 0.12 | 1.97 (3.91x over) | -3.20 (9.22x under) | 41.7% |
| 10 ms | 790 | -0.87 | -2.00 | 0.23 | 2.11 (4.33x over) | -3.27 (9.65x under) | 44.3% |

## The five local images

Gregory's images, never part of the old calibration; held-out cells. Alpha cells excluded here and listed below.

### per image

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 68 | -0.09 | -1.83 | 0.39 | 0.54 (1.45x over) | -2.31 (4.96x under) | 75.0% |
| local-drupflare | 8 | 0.39 | -0.34 | 0.48 | 0.49 (1.40x over) | -0.35 (1.27x under) | 100.0% |
| local-lake-sunapee | 12 | -1.79 | -2.23 | -1.07 | -0.98 (1.97x under) | -2.24 (4.71x under) | 0.0% |
| local-lil-bit | 18 | 0.18 | -0.30 | 0.36 | 0.54 (1.45x over) | -1.38 (2.61x under) | 88.9% |
| local-long-walk | 12 | -0.40 | -0.64 | 0.01 | 0.52 (1.43x over) | -0.64 (1.56x under) | 100.0% |
| local-steam | 18 | 0.06 | -0.96 | 0.21 | 0.47 (1.38x over) | -2.31 (4.96x under) | 83.3% |

## Alpha cells

Webp output where the plan carries 4 channels. The old model prices no ALPH encode, so these are expected to under-estimate; kept out of the headline.

### by source format

| group | cells | median | p10 | p90 | worst over | worst under | in 0.6x-2.0x |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: |
| all | 62 | 0.22 | -0.33 | 0.74 | 1.79 (3.46x over) | -0.86 (1.81x under) | 91.9% |
| png | 30 | -0.02 | -0.44 | 0.46 | 0.56 (1.47x over) | -0.86 (1.81x under) | 93.3% |
| webp | 16 | 0.47 | -0.20 | 0.83 | 0.88 (1.84x over) | -0.23 (1.17x under) | 100.0% |
| gif | 10 | 0.20 | -0.61 | 0.34 | 0.38 (1.30x over) | -0.61 (1.53x under) | 100.0% |
| tiff | 6 | 1.16 | 0.44 | 1.79 | 1.79 (3.46x over) | -0.18 (1.14x under) | 50.0% |

## How to reproduce

```sh
bun scripts/corpus.ts check
bun scripts/measure/baseline.ts --wasm bench/arms/tinyimg-v1.1.1.wasm --out bench/baselines/v1.1.1
bun scripts/measure/baseline.ts summarize --out bench/baselines/v1.1.1
```

A sweep takes the timing lock, waits for the load average to fall below a quarter of the core count and appends each cell to `cells.jsonl` in the output directory (a working file, not committed), so an interrupted run resumes. The sweep writes `cells.json` and `summary.md` when it finishes, and `summarize` rewrites `summary.md` from `cells.json`. Compare another build by pointing `--wasm` at it and `--out` at a new directory.
