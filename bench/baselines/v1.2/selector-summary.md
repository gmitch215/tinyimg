# B5: the selector ablation

Machine darwin-arm64, Apple M2 Pro (12 cores), node v26.3.0. Module `/private/tmp/claude-502/-Users-gamer-gmitch215-c-c---tinyimg/4475b78a-0df4-461c-b5cb-b6b8dea9d9ca/scratchpad/results/A4B5/tinyimg-v120.wasm` sha256 534f7a45247c1f34609d09563d8b40be6aa5e5ea61105484e1915d1d3b4184b9. Native probe: `build-a4/lib/libtinyimg.a`, library sha256 be425f42460e1ea8eb2f87b1042c7389c1a4261f4924e77e8283450109a24a2d, probe source sha256 5a74a970a014bf90924bd4da5c9ad00aa05790ae605e26ff8e5a5e9d8e4e9100. Mode timed, load 2.07 at start and 2.66 at end, limit 3.00. 190 units; budgets 10, 20, 50 ms at machine factor 1, floors none and 35 dB; objective fewest bytes, uncharacterized candidates refused. Reference for PSNR: candidate 0 of the same plan, encoded as PNG, decoded by magick. Bootstrap seed 20261108, 10000 resamples.

Alpha sources left out, as in A4 (2 units): local-drupflare@200, local-drupflare@400.

## Joint arm, native probe against the module

1140 (unit, constraint) joint choices compared, 0 disagreements.

## Served

1137 (unit, constraint) pairs over 190 units; 3 pairs left out because a run was timed above the load limit, 0 incomplete.

Served is feasible, measured T at or under B, and the PSNR floor met when there is one.

| group | pairs | joint | ladder | independent | joint minus ladder |
| --- | ---: | ---: | ---: | ---: | ---: |
| all | 1137 | 430 (37.82%) | 195 (17.15%) | 384 (33.77%) | +20.67 |

Joint minus ladder (cells are units): +20.67 points, 95% [+17.78, +23.70]; the interval above zero.

Joint minus independent (cells are units): +4.05 points, 95% [+3.00, +5.11]; the interval above zero.

Sensitivity, clusters are images: joint minus ladder +20.67 points, 95% [+16.75, +24.79]; joint minus independent +4.05 points, 95% [+3.07, +5.10].

Cells joint serves and ladder does not (lattice rescues): 236. The reverse: 1. Against independent: 47 and 1.

Bytes, joint over ladder: 194 co-served cells, median 0.389, p10 0.192, p90 0.580, total 0.316. Joint over independent: 383 co-served cells, median 1.000, p10 0.665, p90 1.032, total 0.941.

### By constraint

| group | pairs | joint | ladder | independent | joint minus ladder |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10 ms, 35 dB floor | 190 | 20 (10.53%) | 0 (0.00%) | 20 (10.53%) | +10.53 |
| 10 ms, no floor | 190 | 39 (20.53%) | 33 (17.37%) | 24 (12.63%) | +3.16 |
| 20 ms, 35 dB floor | 190 | 63 (33.16%) | 0 (0.00%) | 63 (33.16%) | +33.16 |
| 20 ms, no floor | 190 | 75 (39.47%) | 49 (25.79%) | 64 (33.68%) | +13.68 |
| 50 ms, 35 dB floor | 190 | 108 (56.84%) | 11 (5.79%) | 108 (56.84%) | +51.05 |
| 50 ms, no floor | 187 | 125 (66.84%) | 102 (54.55%) | 105 (56.15%) | +12.30 |

### By source format

| group | pairs | joint | ladder | independent | joint minus ladder |
| --- | ---: | ---: | ---: | ---: | ---: |
| avif | 108 | 33 (30.56%) | 10 (9.26%) | 25 (23.15%) | +21.30 |
| gif | 36 | 16 (44.44%) | 7 (19.44%) | 16 (44.44%) | +25.00 |
| jpeg | 735 | 272 (37.01%) | 145 (19.73%) | 240 (32.65%) | +17.28 |
| png | 150 | 83 (55.33%) | 22 (14.67%) | 80 (53.33%) | +40.67 |
| webp | 108 | 26 (24.07%) | 11 (10.19%) | 23 (21.30%) | +13.89 |

## Control

`tests/fixtures/sf-24.jpg` width 200, webp output.

| label | ms | load before | load after |
| --- | ---: | ---: | ---: |
| start | 7.662 | 2.07 | 2.07 |
| middle | 5.443 | 2.76 | 2.76 |
| end | 5.389 | 2.63 | 2.66 |

Spread (max over min) 1.422, last over first 0.703.

## How to reproduce

```sh
cmake -S . -B build-a4 -DCMAKE_C_FLAGS=-O2 && cmake --build build-a4 -j3 --target tinyimg
bun scripts/measure/selector-ablation.ts run --wasm <module> --expect-sha <prefix>
bun scripts/measure/selector-ablation.ts summarize
```
