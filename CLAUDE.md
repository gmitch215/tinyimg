# tinyimg

Freestanding C compiled to one wasm32 module, wrapped by a TypeScript package for Cloudflare
Workers. `README.md` is the product document; `TECHNICAL_REPORT.md` holds the measurements and the
approaches they refuted. This file is how to work in the repository.

Priorities, in order: **speed, then size, then memory.** They used to run size first, and that
changed on 2026-09-04 when Cloudflare removed the compressed Worker size limit: the only platform
limit is now 64 MiB uncompressed on every plan, against a module of about 760 KB. `bun run size`
reports the shipped package uncompressed, which is the axis Cloudflare checks, and fails past the
ceiling; both figures live in the script rather than here.

Size still matters enough to measure, because `tiny` is in the name and a bigger module costs
startup time, but it no longer wins an argument against speed. Any decision in the history of this
repository that was made on size grounds is worth re-reading before it is cited.

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
bun run tables                 # regenerate the VP8 and AV1 constant tables
bun run tables:check           # verify the committed tables match their specifications
bun run format                 # clang-format and prettier
bun run docs:c                 # doxygen into build-native/docs
bun run docs:build             # typedoc into typedoc/
```

## Never run these

`./doxygen.sh` and `./typedoc.sh` publish to `gh-pages`. Both `git switch -f`, so **they discard
uncommitted changes to tracked files.** One of them has already cost a day of work. To build the
documentation locally use `bun run docs:c` or `bun run docs:build`; the two scripts exist for CI.

Each publishes into its own subdirectory, `/doxygen` and `/typedoc`, with the README-derived
`index.html` and the `CNAME` at the root. That is what lets the two coexist on one branch: each
removes only the directory it owns and stages a scoped `git add -A -- <its paths>`. Both stage into a
temporary directory before switching and restore the original branch from an `EXIT` trap, so the one
that runs second still has its sources.

Two rules keep the pair from destroying each other's work, and both came from a broken deploy.

**Neither may build into a directory named after a published one.** Doxygen builds into
`build-native/docs/html` and TypeDoc into `build-typedoc/`. TypeDoc used to build into `typedoc/`,
which gh-pages tracks: the sibling's `git switch -f gh-pages` checked the published copy out over the
fresh build and made it tracked, and the trap switching back to `master` then deleted it as a file
that branch does not carry. The first deploy passed because there was no `gh-pages` to check out yet,
so this failed only on the second one.

**Neither may clear the branch root.** `doxygen.sh` used to run
`find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +` after switching, which deleted the
sibling's build output mid-job.

They also used to run `git config --local user.name "GitHub Action"`, which persists in
`.git/config` and silently reattributes every later commit in the clone. They now pass the identity
per-command with `git -c`.

## Layout

```text
include/tinyimg/     public headers, one per concern
src/                 memory, util, image, plan, draw, effects, color, text, detect, version
src/codec/           one file per format behind one contract
src/codec/av1*.c     the AV1 intra decoder AVIF sits on, split by stage rather than by format
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

## The AV1 decoder

AVIF is the one format whose decoder does not fit in one file, so it is split by stage:
`av1.h` is the internal contract, `av1-tables.h` is generated, and `av1-symbol.c`,
`av1-transform.c` and the rest implement one stage each. `avif.c` owns the container and calls in.

**The specification is the only authority for the tables and the arithmetic.** `scripts/av1-tables.ts`
extracts 205 tables from it and `bun run tables:check` verifies them. Do not take a number from
dav1d or libaom: dav1d stores its CDFs pre-inverted behind designated initializers keyed to its own
enum order, and the only libaom mirror reachable on GitHub predates the final bitstream. Mixing two
references is how VP8's prediction modes were mixed up here before.

The files, in the order a reader meets them: `av1-tables.h` is generated, `av1-symbol.c` holds the
arithmetic coder **and its inverse**, `av1-cdf.c` the distributions, `av1-obu.c` the OBU layer and
both headers, `av1-tile.c` the partition tree, `av1-mode.c` the mode info, `av1-coeff.c` the
coefficient parse **and the writer**, `av1-predict.c` the intra prediction, `av1-recon.c` the
reconstruction and the conversion, `av1-filter.c` the deblock and CDEF, and `av1-encode.c` the
encoder that drives them.

**The encoder lives beside the decoder deliberately.** The symbol writer is in the same file as the
reader because it has to compute the same interval boundaries; the coefficient writer is in the same
file as the parse because it has to select the same distribution for every symbol. One copy of each
derivation is what makes a round trip a real check rather than two readings agreeing by luck.

Write the stages in the order their measured cost justifies, which is not the intuitive order. The
symbol decoder is 57% of the irreducible floor on its own and 83% with the coefficient parse;
**intra prediction is 2.6% to 3.3%, so write it plainly and do not optimize it**. Transforms are
94%-100% square with the flip variants at 0.0% of every file measured, so 4x4, 8x8 and 16x16 cover
over 90% of blocks and the flips go last. The three post-filters belong behind the effort tier from
the day they are written.

**A prose note in the specification is not normative over the function it annotates.** 7.15.3 says
CDEF's filter region is the tile that decoded the block; `is_inside_filter_region` in 5.11.55 sets
the bounds to the whole frame and ignores the note. The function wins, and `avifdec` agrees with it.
Implementing the note cost 1,462 samples of the four-tile fixture in a band two columns either side
of each seam.

**The flip is applied inside `tiny_av1_inverse_transform`.** The specification splits it differently,
deriving `flipUD` and `flipLR` in 7.12.3 and writing the residual to a mirrored position; a caller
that implements that on top of this function mirrors six of the sixteen transform types twice, and
the result looks nearly right.

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

**A green local gate does not mean the C compiles.** Every local build is clang, and CI's ctest,
sanitizer and CodeQL jobs are GCC on ubuntu, so a clang-only extension passes here and fails three
jobs there. `__builtin_elementwise_min` and its max counterpart did exactly that: GCC has no such
builtin, reads it as an implicit `int` function, and then rejects the vector return type. Check
anything that uses a compiler extension against the other compiler before pushing:

```sh
for f in src/*.c src/codec/*.c; do gcc-16 -c -O2 -std=c17 -I include -I src -o /tmp/o.o $f || echo "FAILED $f"; done
```

Vector code is where this bites, and the portable subset is smaller than it looks: the GNU vector
ternary `(v < low) ? low : v` is rejected by **both** compilers in C, so a clamp is a subscript loop
or a comparison mask with a bitwise select. Both lower to the same two instructions.

**No gate test compares a timing against a threshold.** The cost model's rates were measured on one
machine, so an assertion that the estimate is within some factor of a live clock fails on a runner
of a different speed rather than when the model is wrong. CI timed a 200 px transform at 12.33 ms
against this machine's 4.89 and failed a 3x band. A ratio between two extents cancels the machine
but then passes a mutation that flattens the resample term, so it gates nothing. What the lane gates
instead is the model's shape, which does not depend on speed: relative stage costs and which ladder
rung is picked. Flattening the scale fraction fails three of those. The absolute comparison is
`bun run eval:estimate`, a release check with a stated band and a named machine.

Two lanes with different budgets.

- **ctest** is deterministic, local, free and fast, and must never be flaky. **It does not run on
  commit.** `.githooks/pre-commit` runs `clang-format` and `prettier` and re-stages the result, so
  it cannot fail on formatting either; the only gates are in CI. This file claimed otherwise for a
  while, which is the failure the "verify a constraint before you write it down" rule exists to
  stop.
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
No secrets. `blobs/`, `bench/arms/`, `bench/results/`, `dist/`, `build-typedoc/` and `coverage/` are
generated and gitignored.

Commit message prefixes are the ones `.github/release.json` sorts into categories: `feat:`, `fix:`,
`test:`, `docs:`, `build:`, `chore:`, `style:`, plus `breaking` for a breaking change. An optional
scope goes in parentheses, as in `feat(testing):`. Lowercase, imperative, no trailing period. A
workflow change is `build:` or `chore:`, which is what the log uses; there is no `ci:` category.

Small commits grouped by concern, because pinning and bisecting are done by commit.
