# tinyimg

Freestanding C compiled to one wasm32 module, wrapped by a TypeScript package for Cloudflare
Workers. `README.md` is the product document; `TECHNICAL_REPORT.md` holds the measurements and the
approaches they refuted. This file is how to work in the repository.

Priorities, in order: **small size, then speed, then memory.** `bun run size` reports the shipped
package against the target and fails past the limit; both figures live in the script rather than
here.

## Commands

```sh
bun install                    # also installs the git hooks
bun run build                  # build:wasm then build:ts
bun run build:wasm             # cmake + wasm-ld + wasm-opt -> bin/tinyimg.wasm
bun run build:native           # the host static library and the ctest executables
bun run build:ts               # dist/, declarations first then comment-free JS

bun run test                   # all three lanes
bun run test:c                 # builds native, then ctest
bun run test:node              # vitest, node lane
bun run test:workers           # builds dist/, then vitest inside workerd
bun run typecheck              # tsc against both tsconfigs

bun run size                   # size against the target, per region
bun bench/run.ts               # every benchmark arm
bun scripts/coverage-c.ts      # C line coverage into coverage/
bun run fixtures               # regenerate tests/fixtures/derived and blobs/
bun run fixtures:check         # verify the committed fixtures reproduce
bun run format                 # clang-format and prettier
bun run docs:c                 # doxygen into build-native/docs
bun run docs:build             # typedoc into typedoc/
```

## Never run these

`./doxygen.sh` and `./typedoc.sh` publish to `gh-pages`. They `git switch` and then
`find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +`, so **they delete uncommitted work
in the working tree.** One of them has already cost a day of it. To build the documentation locally
use `bun run docs:c` or `bun run docs:build`; the two scripts exist for CI.

They also used to run `git config --local user.name "GitHub Action"`, which persists in
`.git/config` and silently reattributes every later commit in the clone. They now pass the identity
per-command with `git -c`.

## Layout

```text
include/tinyimg/     public headers, one per concern
src/                 memory, util, image, plan, draw, effects, color, text, detect, version
src/codec/           one file per format behind one contract
src/ts/              wasm, types, image, transform, index
tests/c/             ctest, one file per concern, grouped by directory
tests/node/          vitest against a module compiled from bytes
tests/workers/       vitest inside workerd, through miniflare service bindings
tests/fixtures/      source images; derived/ is generated and committed
bench/               harness, arms, report
scripts/             fixtures, icc, fonts, cascade, size-report, coverage-c, measure/
blobs/               gitignored; generated locally, uploaded by hand
```

`image.c`, `draw.c`, `effects.c` and `color.c` are split by `#pragma region` matching the header
regions. A new operation goes in the region its header declares it in.

## Adding an operation

The whole contract is the op class. Pick the one that describes **how the operation reads its
input**, and every rewrite, the backward ROI walk and the executor already know what to do with it.
Nothing else in the planner is written per operation.

| Class          | Reads                              | Composes by                                                |
| -------------- | ---------------------------------- | ---------------------------------------------------------- |
| `GEOMETRY`     | moves pixels without changing them | folding into the source window, sample map and orientation |
| `COLOR_MATRIX` | one pixel, affine in its channels  | matrix multiplication                                      |
| `COLOR_LUT`    | one pixel, not affine              | composing through the table                                |
| `NEIGHBORHOOD` | a neighborhood around each pixel   | nothing; it ends a fused pass                              |

Phase 5 added roughly ninety operations and needed **three** new `TinyPlanOpKind` values, no new
rewrite rules, no new ROI arithmetic and no new executor branches:

- `TINYIMG_OP_MATRIX` carries a `float m[12]` inline. Sepia, colorize, tint, duotone, split-tone,
  white balance, the channel mixer and the colorblind simulations are all one of these.
- `TINYIMG_OP_CURVE` carries a `TinyCurveKind`, five parameters and a channel mask. Twelve curve
  shapes cover posterize, threshold, solarize, exposure, levels, fill light, gain, sigmoid, negate,
  sRGB and color balance.
- `TINYIMG_OP_EFFECT` carries a `TinyEffectKind`, four parameters and a rect, dispatched by
  `tiny_effect_apply`.

**A curve is parameterized rather than carrying its table**, because a 768-byte table inline would
make `TinyPlanOp` ~772 bytes, and a `TinyPlan` holds 32 of them on a type documented as one a caller
keeps on the stack. The escape hatch is `tiny_image_apply_lut` and friends, which take a caller's own
table and run one eager pass.

Two operations cannot be plan operations, and the reason is the same for both: **the planner decides
the decode region and scale before any pixel is read**, and these need the pixels to decide.

- `trim`, whose extent is a function of the border color.
- `rotate` at an arbitrary angle, which changes the extent by a non-integer factor.

Both are applied to the materialized image after the plan runs. `Image` records them in `#after` and
the wrapper documents the ordering.

## Adding a codec

One contract, in `include/tinyimg/codec/codec.h`:

```c
typedef struct {
    TinyImageFormat format;
    TinySniffFn     sniff;
    TinyProbeFn     probe;
    TinyDecodeFn    decode;
    TinyEncodeFn    encode;
} TinyCodec;
```

`TinyDecodeOpts` carries the region and the scale denominator the planner computed; honor both or
report why you cannot. `TinyWriter` is a grow-on-demand sink, so an encoder never asks a caller to
guess an output size. `--gc-sections` drops a codec nobody references, and the registry is the seam
a second wasm module would plug into.

Every callback type is typedef'd so no member declaration carries a `(*`, and every pointer binds
left to its type, which is what `.clang-format` enforces.

## Where static data goes

Three buckets, and the bucket decides:

1. **Derivable, so derive at init.** CRC32, Adler32, the sRGB and gamma tables, JPEG's zigzag order,
   the AAN IDCT scale factors, the gaussian and Bayer kernels, VP8L's distance mapping. Zero bundle
   bytes and strictly better than either alternative.
2. **Small and unconditional, so inline.** JPEG's standard Huffman and quantization tables, VP8's
   token trees and default probabilities, the sRGB primaries. About 3.5 KB total.
3. **Large or conditional, so load at runtime** through `tiny_blob_load`. Fonts, ICC profiles and
   cascades. Nothing in `blobs/` is required, and every feature that reads one degrades without it
   rather than failing.

A table that could be derived and is inlined instead is a size regression with no upside.

## Conventions

- **Indentation is not the same everywhere, and the tools are the authority.** LF and UTF-8
  throughout; run `bun run format` rather than matching by eye.

  | Files                  | Indent    | Width | Set by                 |
  | ---------------------- | --------- | ----- | ---------------------- |
  | C and headers          | 4 spaces  | 80    | `.clang-format`        |
  | TypeScript, JavaScript | tabs at 4 | 100   | `.prettierrc`          |
  | YAML                   | 2 spaces  | 100   | `.prettierrc` override |
  | Markdown               | 2 spaces  | 100   | `.prettierrc` override |

- **Comments are terse, lowercase, no trailing period**, and only where the _why_ is not obvious. One
  comment is at most two lines; anything longer belongs in a doxygen or TSDoc block on the export it
  documents. No file-header `//` banners.
- **Use `// #region name` / `// #endregion`** to group a section, in TS as well as C, not dashed
  dividers.
- **No comments in configuration files**: `.github/**`, `.editorconfig`, `.prettierrc`,
  `.prettierignore`, `.gitattributes`, `package.json`. A conventional one-line header or a real
  machine-read directive stays. If a choice needs explaining, explain it here.
- **Plain ASCII in strings, comments and identifiers.** No em dashes, no fancy quotes, no arrows.
- **Every header prototype carries doxygen** in the voice already there, and every TS export carries
  TSDoc. `bun run docs:c` and `bun run docs:build` both run clean, and typedoc's `notDocumented`
  validation is on.

## Testing

Two lanes with different budgets.

- **ctest** is deterministic, local, free and fast. It runs on every commit through the pre-commit
  hook and must never be flaky.
- **The differential tests** compare against `magick`, `cjpeg`, `cwebp` and `avifdec` within a
  **stated floor that came from a measurement**, never from an approximation argument. The references
  are generated by `scripts/fixtures.ts` and committed, so CI needs no image tooling, and
  `bun run fixtures:check` proves they still reproduce.

Three rules that came from getting them wrong:

1. **A reference has to be something the format can produce.** GIF encode looked 7-12 dB behind
   ImageMagick against a 256-color RGBA reference that kept partial alpha, which no GIF can express.
   Three rounds of quantizer tuning went into a deficit that did not exist.
2. **An invertible transform tested only against its own inverse is untested.** The ICC profiles had
   their tone curves the wrong way round and every self-consistent check passed.
3. **Assert the plan, not only the pixels.** A resolution is inspectable, so a rewrite test asserts
   `eliminated`, `collapsed`, `colorStages` and the kernel bitmask. Two plans can produce the same
   image and only one of them can have been optimized.

One spec file per concern. Never add `*-extra` or `*-part2` beside an existing one; extend it.

## Measuring

**Benchmark the build that ships.** `bin/tinyimg.wasm` is `-Oz -flto`; the ctest static library is
neither, and the math shims and bit reader live in different translation units from their callers.
Measuring the library overstated PNG decode 3x and text drawing 4.6x, and both mistakes cost real
time. Build a purpose-made arm if you need one:

```sh
clang -O2 -flto -I include src/*.c src/codec/*.c probe.c -lm -o probe
```

The tell that you are on the wrong build: `tiny_sqrtf`, `tiny_floorf`, `tiny_clamp_u8f` or
`tiny_clampi` appearing as call frames in a profile. They are one-liners and inline under LTO.

**Profile before optimizing.** Two plausible guesses at what made text drawing slow were both real
inefficiencies and neither moved the clock; one `sample` run found the actual 63% on the first
attempt.

**Then count, before widening anything.** A profile says where the time is, not what shape the work
has. JPEG's inverse transform is half of decode, which reads as the obvious SIMD kernel until you
count the inputs: 93% of columns on `sf-24.jpg` carry no vertical detail and already skip the
transform, so a four-wide column pass would do full transforms for groups where almost every lane
needs none. The same count found the row pass computing one row eight times, which was worth 20.8%
for six lines. Add a counter and print it; the two decode wins this repo has both came from one.

**A win on `sf-24.jpg` is not a win.** It is a smooth photograph and it is the fixture every number
is quoted on, so anything that keys off flat blocks or low detail reads high on it: the same change
worth 20.8% there is worth 0.8% on `road.jpg`. Quote a second fixture before believing a number.

**Rebuild `bench/arms/tinyimg.wasm` when the sources change.** It is gitignored, so it outlives them,
and a stale arm reported an unrelated codec change as a 1.24x SIMD win. `bench/run.ts` compares its
mtime against `src/` and `include/` and refuses to report rather than print that.

## Git

Work stays uncommitted on `master` unless asked. Never `--no-verify`; if a hook fails, fix the cause.
No secrets. `blobs/`, `bench/arms/`, `bench/results/`, `dist/`, `typedoc/` and `coverage/` are
generated and gitignored.

Commit message prefixes are the ones `.github/release.json` sorts into categories: `feat:`, `fix:`,
`test:`, `docs:`, `build:`, `chore:`, `style:`, plus `breaking` for a breaking change. An optional
scope goes in parentheses, as in `feat(testing):`. Lowercase, imperative, no trailing period. A
workflow change is `build:` or `chore:`, which is what the log uses; there is no `ci:` category.

Small commits grouped by concern, because pinning and bisecting are done by commit.
