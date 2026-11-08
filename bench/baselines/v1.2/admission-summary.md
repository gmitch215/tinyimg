# A4: admission against the clock

Machine darwin-arm64, Apple M2 Pro (12 cores), node v26.3.0. Module `/private/tmp/claude-502/-Users-gamer-gmitch215-c-c---tinyimg/4475b78a-0df4-461c-b5cb-b6b8dea9d9ca/scratchpad/results/A4B5/tinyimg-v120.wasm` sha256 534f7a45247c1f34609d09563d8b40be6aa5e5ea61105484e1915d1d3b4184b9. FIXED arm: v1.1.1 module sha256 8851e695962e8796caae504858b86c70823dd96e3cd443ee0365705c2e81b441. Mode timed, load average 2.93 at start and 2.07 at end, load limit 3.00, 2 warmups and 9 timed runs per cell, median is T, encode quality 80. Budgets 5, 10, 20, 50, 100 ms, platformScale 1.

## Cells

433 cells: 428 timed, 1 failed, 4 skipped (alpha sources are skipped). 5 timed cells were recorded above the load limit and are excluded from the gate. 423 cells are eligible, 2115 (cell, budget) pairs.

## Gate

| arm | pairs | admitted | overruns | overrun rate | false rejects / fit | false-reject rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NEW, strict admit | 2115 | 739 | 67 | 9.07% | 53 / 725 | 7.31% |
| FIXED, v1.1.1 decide plus encoder | 2115 | 1174 | 451 | 38.42% | 2 / 725 | 0.28% |
| FIXED A, v1.1.1 decide alone (information) | 2115 | 1332 | 608 | 45.65% | 1 / 725 | 0.14% |
| NEW at quality 80 (sensitivity) | 2115 | 735 | 64 | 8.71% | 54 / 725 | 7.45% |

Overrun ratio FIXED over NEW: 6.73x. NEW overrun rate 9.07% against the 5% limit; ratio against the 2x minimum.

**Gate: FAIL.** NEW overrun rate 9.07% is over 5%.

A failure closes the two-term upper-bound mechanism and goes under refuted approaches; the objective stays.

## Rows reported, not gated

### by budget

| arm | pairs | admitted | overruns | overrun rate | false rejects / fit | false-reject rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 5 ms, NEW | 423 | 19 | 5 | 26.32% | 8 / 22 | 36.36% |
| 5 ms, FIXED | 423 | 44 | 24 | 54.55% | 2 / 22 | 9.09% |
| 10 ms, NEW | 423 | 44 | 2 | 4.55% | 14 / 56 | 25.00% |
| 10 ms, FIXED | 423 | 106 | 50 | 47.17% | 0 / 56 | 0.00% |
| 20 ms, NEW | 423 | 129 | 15 | 11.63% | 9 / 123 | 7.32% |
| 20 ms, FIXED | 423 | 255 | 132 | 51.76% | 0 / 123 | 0.00% |
| 50 ms, NEW | 423 | 202 | 7 | 3.47% | 10 / 205 | 4.88% |
| 50 ms, FIXED | 423 | 360 | 155 | 43.06% | 0 / 205 | 0.00% |
| 100 ms, NEW | 423 | 345 | 38 | 11.01% | 12 / 319 | 3.76% |
| 100 ms, FIXED | 423 | 409 | 90 | 22.00% | 0 / 319 | 0.00% |

### by source format

| arm | pairs | admitted | overruns | overrun rate | false rejects / fit | false-reject rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| avif, NEW | 270 | 32 | 26 | 81.25% | 0 / 6 | 0.00% |
| avif, FIXED | 270 | 161 | 155 | 96.27% | 0 / 6 | 0.00% |
| gif, NEW | 90 | 35 | 0 | 0.00% | 3 / 38 | 7.89% |
| gif, FIXED | 90 | 42 | 4 | 9.52% | 0 / 38 | 0.00% |
| jpeg, NEW | 1220 | 471 | 30 | 6.37% | 26 / 467 | 5.57% |
| jpeg, FIXED | 1220 | 715 | 250 | 34.97% | 2 / 467 | 0.43% |
| png, NEW | 375 | 158 | 11 | 6.96% | 2 / 149 | 1.34% |
| png, FIXED | 375 | 176 | 27 | 15.34% | 0 / 149 | 0.00% |
| webp, NEW | 160 | 43 | 0 | 0.00% | 22 / 65 | 33.85% |
| webp, FIXED | 160 | 80 | 15 | 18.75% | 0 / 65 | 0.00% |

### by output format

| arm | pairs | admitted | overruns | overrun rate | false rejects / fit | false-reject rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| avif, NEW | 90 | 8 | 6 | 75.00% | 0 / 2 | 0.00% |
| avif, FIXED | 90 | 55 | 53 | 96.36% | 0 / 2 | 0.00% |
| gif, NEW | 30 | 10 | 0 | 0.00% | 3 / 13 | 23.08% |
| gif, FIXED | 30 | 14 | 1 | 7.14% | 0 / 13 | 0.00% |
| jpeg, NEW | 920 | 368 | 37 | 10.05% | 25 / 356 | 7.02% |
| jpeg, FIXED | 920 | 553 | 199 | 35.99% | 2 / 356 | 0.56% |
| png, NEW | 125 | 31 | 0 | 0.00% | 1 / 32 | 3.13% |
| png, FIXED | 125 | 32 | 0 | 0.00% | 0 / 32 | 0.00% |
| webp, NEW | 950 | 322 | 24 | 7.45% | 24 / 322 | 7.45% |
| webp, FIXED | 950 | 520 | 198 | 38.08% | 0 / 322 | 0.00% |

Not covered by the test: alpha sources (excluded), lossy WebP with alpha and TIFF encode by compression, the two coverage misses A2c named (0.820 and 0.891).

## Control

`tests/fixtures/sf-24.jpg` width 200, webp output, same request as a cell.

| label | ms | load before | load after |
| --- | ---: | ---: | ---: |
| start | 7.945 | 2.93 | 2.93 |
| middle | 7.713 | 2.54 | 2.54 |
| end | 7.707 | 2.07 | 2.07 |

Spread (max over min) 1.031, last over first 0.970.

## How to reproduce

```sh
bun scripts/measure/admission-eval.ts run --wasm <module> --expect-sha <prefix>
bun scripts/measure/admission-eval.ts summarize
```
