# The shipped total, estimate plus encode, against the clock

Same cells as `summary.md` (test split, alpha cells excluded). Arm A is `Image.decide().estimateMs`, decode and resample only. Arm B adds `tiny_encode_cost(format, width, height)`, which is what `transform({ budgetMs })` compares against. Error is `log2(estimate / measured)`: -1 is a 2x under-estimate. Produced by `scripts/measure/baseline-total.ts` from `cells.json`, the manifest dimensions and the frozen arm.

## Arm A, decode and resample only

| group | cells | median |   p10 |  p90 | worst over        | worst under          | in 0.6x-2.0x |
| ----- | ----: | -----: | ----: | ---: | ----------------- | -------------------- | -----------: |
| all   |   878 |  -1.01 | -2.37 | 0.06 | 1.44 (2.71x over) | -3.46 (10.97x under) |        38.6% |

## Arm B, plus the encoder

| group | cells | median |   p10 |  p90 | worst over        | worst under         | in 0.6x-2.0x |
| ----- | ----: | -----: | ----: | ---: | ----------------- | ------------------- | -----------: |
| all   |   878 |  -0.41 | -2.01 | 0.50 | 2.90 (7.44x over) | -3.18 (9.04x under) |        59.8% |

### Arm B by source format

| group | cells | median |   p10 |   p90 | worst over          | worst under         | in 0.6x-2.0x |
| ----- | ----: | -----: | ----: | ----: | ------------------- | ------------------- | -----------: |
| avif  |   108 |  -1.95 | -2.89 | -1.04 | -0.45 (1.37x under) | -3.07 (8.42x under) |         3.7% |
| gif   |    36 |   0.20 | -0.00 |  0.53 | 1.18 (2.27x over)   | -0.05 (1.04x under) |        94.4% |
| jpeg  |   492 |  -0.57 | -1.92 |  0.56 | 1.76 (3.38x over)   | -3.18 (9.04x under) |        55.1% |
| png   |   158 |  -0.01 | -0.46 |  0.53 | 2.90 (7.44x over)   | -0.56 (1.47x under) |        95.6% |
| webp  |    84 |  -0.19 | -1.21 |  0.49 | 0.82 (1.76x over)   | -1.85 (3.60x under) |        77.4% |

### Arm B by output format

| group | cells | median |   p10 |   p90 | worst over          | worst under         | in 0.6x-2.0x |
| ----- | ----: | -----: | ----: | ----: | ------------------- | ------------------- | -----------: |
| avif  |    36 |  -2.44 | -2.93 | -1.58 | -1.52 (2.86x under) | -3.07 (8.42x under) |         0.0% |
| gif   |    12 |   0.31 |  0.12 |  1.17 | 1.18 (2.27x over)   | 0.11 (1.08x over)   |        83.3% |
| jpeg  |   390 |  -0.45 | -1.97 |  0.53 | 1.76 (3.38x over)   | -3.18 (9.04x under) |        58.2% |
| png   |    54 |   0.38 |  0.16 |  1.37 | 2.90 (7.44x over)   | 0.05 (1.03x over)   |        87.0% |
| webp  |   386 |  -0.41 | -1.87 |  0.44 | 1.32 (2.49x over)   | -2.95 (7.74x under) |        62.4% |
