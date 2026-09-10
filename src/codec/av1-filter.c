#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

/**
 * @file
 * @brief Specification 7.14 and 7.15, the two post-filters a still frame runs.
 *
 * The deblocking filter and CDEF, both **behind the effort tier**, which is the
 * decision this project made about them before either was written: nothing in a
 * still image references what they write, so a caller who asked for speed can
 * have the frame without them. That is the same argument that made VP8's
 * in-loop deblocking skippable here for 1.53x at 46.8 dB, and it is stronger
 * for AV1 because there are two of them.
 *
 * Loop restoration, the third filter, is refused at the frame instead. Its
 * filter could be skipped like these two, but **its symbols cannot**: they are
 * coded per superblock inside the tile, so a tile read without them
 * desynchronises. Every file measured has it off.
 *
 * Two simplifications hold for an intra frame and are worth naming, because
 * both remove a branch the specification spends a paragraph on:
 *
 * - `applyFilter` reduces to `isTxEdge`. The full condition is
 *   `isTxEdge && (isBlockEdge || !skip || isIntra)`, and `isIntra` is always
 *   true here, so the second half is always true.
 * - `modeType` is always 0, because every mode is an intra one, so the mode
 *   deltas only ever index their first entry.
 */

// #region shared

/** Samples along one side of an mi unit, and its log2. */
#define AV1_MI_SIZE 4
#define AV1_MI_SIZE_LOG2 2

/** The largest filter level, which every clamp here shares. */
#define AV1_MAX_LOOP_FILTER 63

/** Absolute value, which the two mask derivations use on every tap. */
static int32_t filter_abs(int32_t v) {
    return v < 0 ? -v : v;
}

/** FloorLog2, which the damping and the variance scaling both want. */
static uint32_t filter_log2(uint32_t v) {
    uint32_t log2 = 0;

    while (v > 1u) {
        v >>= 1;
        log2++;
    }

    return log2;
}

/** One entry of a frame-sized mi array. */
static uint8_t mi_at(
    const TinyAv1Filter* f, const uint8_t* array, uint32_t row, uint32_t col
) {
    return array[(size_t) row * f->frame->mi_cols + col];
}

// #region loop filter

/** The level, and the three thresholds derived from it. */
typedef struct {
    int32_t level;
    int32_t limit;
    int32_t blimit;
    int32_t thresh;
} Strength;

/**
 * @brief The filter level for one position, plane and direction.
 *
 * The per-segment feature and the reference deltas both adjust it, and the
 * reference delta is not decoration on an intra frame: `loop_filter_ref_deltas`
 * defaults to 1 for the intra reference, so a frame with delta filtering
 * enabled filters one level harder than its header's number says.
 */
static int32_t filter_level(
    const TinyAv1Filter* f, uint32_t row, uint32_t col, uint32_t plane,
    uint32_t pass
) {
    const TinyAv1Frame* frame = f->frame;
    uint32_t which = plane == 0u ? pass : plane + 1u;

    int32_t delta = 0;

    if (f->delta_lf) {
        size_t at = ((size_t) row * frame->mi_cols + col) * 4u;

        delta =
            frame->delta_lf_multi ? f->delta_lf[at + which] : f->delta_lf[at];
    }

    int32_t level = tiny_clampi(
        delta + frame->loop_filter_level[which], 0, AV1_MAX_LOOP_FILTER
    );

    uint32_t segment = mi_at(f, f->segment_ids, row, col);

    if (frame->segmentation_enabled &&
        frame->seg_alt_lf_active[which][segment]) {
        level = tiny_clampi(
            level + frame->seg_alt_lf[which][segment], 0, AV1_MAX_LOOP_FILTER
        );
    }

    if (frame->loop_filter_delta_enabled) {
        int32_t shift = level >> 5;

        // the intra reference is index 0 and is the only one an intra frame
        // reaches, so the mode deltas never apply
        level += frame->loop_filter_ref_deltas[0] * (1 << shift);
        level = tiny_clampi(level, 0, AV1_MAX_LOOP_FILTER);
    }

    return level;
}

static Strength strength_at(
    const TinyAv1Filter* f, uint32_t row, uint32_t col, uint32_t plane,
    uint32_t pass
) {
    Strength s;
    uint32_t sharpness = f->frame->loop_filter_sharpness;

    s.level = filter_level(f, row, col, plane, pass);

    uint32_t shift = sharpness > 4u ? 2u : (sharpness > 0u ? 1u : 0u);

    if (sharpness > 0u) {
        s.limit = tiny_clampi(s.level >> shift, 1, 9 - (int32_t) sharpness);
    }
    else {
        s.limit = s.level >> shift;
        if (s.limit < 1) s.limit = 1;
    }

    s.blimit = 2 * (s.level + 2) + s.limit;
    s.thresh = s.level >> 4;

    return s;
}

/** One sample along the filter's own direction, which is across the edge. */
static int32_t tap(
    const TinyAv1Filter* f, uint32_t plane, int32_t x, int32_t y, int32_t dx,
    int32_t dy, int32_t offset
) {
    size_t px = (size_t) (x + dx * offset);
    size_t py = (size_t) (y + dy * offset);

    return f->plane[plane][py * f->stride[plane] + px];
}

static void put(
    const TinyAv1Filter* f, uint32_t plane, int32_t x, int32_t y, int32_t dx,
    int32_t dy, int32_t offset, int32_t value
) {
    size_t px = (size_t) (x + dx * offset);
    size_t py = (size_t) (y + dy * offset);

    f->plane[plane][py * f->stride[plane] + px] = (uint8_t) value;
}

/** Clip3 to what a signed sample of this depth holds, for the narrow filter. */
static int32_t filter4_clamp(int32_t value, uint32_t bit_depth) {
    int32_t low = -(1 << (bit_depth - 1u));
    int32_t high = (1 << (bit_depth - 1u)) - 1;

    return tiny_clampi(value, low, high);
}

/**
 * @brief The narrow filter, four taps, modifying one or two samples a side.
 *
 * High edge variance is the switch: an edge that is genuinely an edge gets one
 * sample either side moved, built from four samples, and a smooth one gets two
 * either side moved from just the inner pair. Filtering an edge as though it
 * were smooth is what blurs a real boundary.
 */
static void narrow_filter(
    const TinyAv1Filter* f, uint32_t plane, int32_t x, int32_t y, int32_t dx,
    int32_t dy, int hev, uint32_t bit_depth
) {
    int32_t middle = 0x80 << (bit_depth - 8u);

    int32_t q0 = tap(f, plane, x, y, dx, dy, 0) - middle;
    int32_t q1 = tap(f, plane, x, y, dx, dy, 1) - middle;
    int32_t p0 = tap(f, plane, x, y, dx, dy, -1) - middle;
    int32_t p1 = tap(f, plane, x, y, dx, dy, -2) - middle;

    int32_t filter = hev ? filter4_clamp(p1 - q1, bit_depth) : 0;

    filter = filter4_clamp(filter + 3 * (q0 - p0), bit_depth);

    int32_t first = filter4_clamp(filter + 4, bit_depth) >> 3;
    int32_t second = filter4_clamp(filter + 3, bit_depth) >> 3;

    put(f, plane, x, y, dx, dy, 0,
        filter4_clamp(q0 - first, bit_depth) + middle);
    put(f, plane, x, y, dx, dy, -1,
        filter4_clamp(p0 + second, bit_depth) + middle);

    if (hev) return;

    int32_t outer = (first + 1) >> 1;

    put(f, plane, x, y, dx, dy, 1,
        filter4_clamp(q1 - outer, bit_depth) + middle);
    put(f, plane, x, y, dx, dy, -2,
        filter4_clamp(p1 + outer, bit_depth) + middle);
}

/**
 * @brief The wide filter, a low pass over up to fourteen samples.
 *
 * Written as the specification's loop rather than as unrolled taps per size,
 * because the tap weights are a rule: the inner `n2` samples either side count
 * twice and the rest once, which is what gives it unity gain, and the position
 * is clamped so the filter repeats the outermost sample instead of reading past
 * the region it is allowed.
 */
static void wide_filter(
    const TinyAv1Filter* f, uint32_t plane, int32_t x, int32_t y, int32_t dx,
    int32_t dy, uint32_t log2_size
) {
    int32_t n = log2_size == 4u ? 6 : (plane == 0u ? 3 : 2);
    int32_t n2 = (log2_size == 3u && plane == 0u) ? 0 : 1;

    int32_t out[12];

    for (int32_t i = -n; i < n; i++) {
        int32_t total = 0;

        for (int32_t j = -n; j <= n; j++) {
            int32_t at = tiny_clampi(i + j, -(n + 1), n);
            int32_t weight = (j < 0 ? -j : j) <= n2 ? 2 : 1;

            total += tap(f, plane, x, y, dx, dy, at) * weight;
        }

        out[i + n] = (total + (1 << (log2_size - 1u))) >> log2_size;
    }

    for (int32_t i = -n; i < n; i++) {
        put(f, plane, x, y, dx, dy, i, out[i + n]);
    }
}

/** Filters one sample position across an edge, if the masks allow it. */
static void filter_sample(
    const TinyAv1Filter* f, uint32_t plane, int32_t x, int32_t y, int32_t dx,
    int32_t dy, const Strength* s, uint32_t filter_size
) {
    uint32_t bit_depth = f->sequence->bit_depth;
    int32_t shift = (int32_t) bit_depth - 8;

    int32_t q0 = tap(f, plane, x, y, dx, dy, 0);
    int32_t q1 = tap(f, plane, x, y, dx, dy, 1);
    int32_t q2 = tap(f, plane, x, y, dx, dy, 2);
    int32_t q3 = tap(f, plane, x, y, dx, dy, 3);
    int32_t p0 = tap(f, plane, x, y, dx, dy, -1);
    int32_t p1 = tap(f, plane, x, y, dx, dy, -2);
    int32_t p2 = tap(f, plane, x, y, dx, dy, -3);
    int32_t p3 = tap(f, plane, x, y, dx, dy, -4);

    int32_t thresh = s->thresh << shift;
    int32_t limit = s->limit << shift;
    int32_t blimit = s->blimit << shift;

    int hev = filter_abs(p1 - p0) > thresh || filter_abs(q1 - q0) > thresh;

    uint32_t length = filter_size == 4u   ? 4u
                      : plane != 0u       ? 6u
                      : filter_size == 8u ? 8u
                                          : 16u;

    int mask = filter_abs(p1 - p0) > limit || filter_abs(q1 - q0) > limit ||
               filter_abs(p0 - q0) * 2 + filter_abs(p1 - q1) / 2 > blimit;

    if (length >= 6u) {
        mask =
            mask || filter_abs(p2 - p1) > limit || filter_abs(q2 - q1) > limit;
    }
    if (length >= 8u) {
        mask =
            mask || filter_abs(p3 - p2) > limit || filter_abs(q3 - q2) > limit;
    }

    if (mask) return;

    int32_t flat_limit = 1 << shift;
    int flat = 0;
    int flat2 = 0;

    if (filter_size >= 8u) {
        int rough = filter_abs(p1 - p0) > flat_limit ||
                    filter_abs(q1 - q0) > flat_limit ||
                    filter_abs(p2 - p0) > flat_limit ||
                    filter_abs(q2 - q0) > flat_limit;

        if (length >= 8u) {
            rough = rough || filter_abs(p3 - p0) > flat_limit ||
                    filter_abs(q3 - q0) > flat_limit;
        }

        flat = !rough;
    }

    if (filter_size >= 16u) {
        int32_t q4 = tap(f, plane, x, y, dx, dy, 4);
        int32_t q5 = tap(f, plane, x, y, dx, dy, 5);
        int32_t q6 = tap(f, plane, x, y, dx, dy, 6);
        int32_t p4 = tap(f, plane, x, y, dx, dy, -5);
        int32_t p5 = tap(f, plane, x, y, dx, dy, -6);
        int32_t p6 = tap(f, plane, x, y, dx, dy, -7);

        int rough = filter_abs(p6 - p0) > flat_limit ||
                    filter_abs(q6 - q0) > flat_limit ||
                    filter_abs(p5 - p0) > flat_limit ||
                    filter_abs(q5 - q0) > flat_limit ||
                    filter_abs(p4 - p0) > flat_limit ||
                    filter_abs(q4 - q0) > flat_limit;

        flat2 = !rough;
    }

    if (filter_size == 4u || !flat) {
        narrow_filter(f, plane, x, y, dx, dy, hev, bit_depth);

        return;
    }

    if (filter_size == 8u || !flat2) {
        wide_filter(f, plane, x, y, dx, dy, 3u);

        return;
    }

    wide_filter(f, plane, x, y, dx, dy, 4u);
}

/** One edge of one 4x4 position, in one direction, for one plane. */
static void filter_edge(
    const TinyAv1Filter* f, uint32_t plane, uint32_t pass, uint32_t row,
    uint32_t col
) {
    const TinyAv1Frame* frame = f->frame;

    uint32_t sub_x = plane > 0u ? f->sequence->sub_x : 0u;
    uint32_t sub_y = plane > 0u ? f->sequence->sub_y : 0u;

    int32_t dx = pass == 0u ? 1 : 0;
    int32_t dy = pass == 0u ? 0 : 1;

    uint32_t x = col * AV1_MI_SIZE;
    uint32_t y = row * AV1_MI_SIZE;

    row = row | sub_y;
    col = col | sub_x;

    if (x >= frame->width || y >= frame->height) return;
    if (pass == 0u && x == 0u) return;
    if (pass == 1u && y == 0u) return;

    uint32_t xp = x >> sub_x;
    uint32_t yp = y >> sub_y;

    uint32_t prev_row = row - ((uint32_t) dy << sub_y);
    uint32_t prev_col = col - ((uint32_t) dx << sub_x);

    uint32_t size = mi_at(f, f->sizes, row, col);
    uint32_t plane_size = tiny_av1_subsampled_size[size][sub_x][sub_y];

    uint32_t tx =
        f->lf_tx[plane]
                [(size_t) (row >> sub_y) * f->frame->mi_cols + (col >> sub_x)];
    uint32_t prev_tx =
        f->lf_tx[plane]
                [(size_t) (prev_row >> sub_y) * f->frame->mi_cols +
                 (prev_col >> sub_x)];

    // an intra frame filters every transform edge, because the condition the
    // specification writes as three alternatives is always true for one of them
    int tx_edge = pass == 0u ? (xp % tiny_av1_tx_width[tx]) == 0u
                             : (yp % tiny_av1_tx_height[tx]) == 0u;

    if (!tx_edge) return;

    (void) plane_size;

    uint32_t base = pass == 0u
                        ? (tiny_av1_tx_width[prev_tx] < tiny_av1_tx_width[tx]
                               ? tiny_av1_tx_width[prev_tx]
                               : tiny_av1_tx_width[tx])
                        : (tiny_av1_tx_height[prev_tx] < tiny_av1_tx_height[tx]
                               ? tiny_av1_tx_height[prev_tx]
                               : tiny_av1_tx_height[tx]);

    uint32_t cap = plane == 0u ? 16u : 8u;
    uint32_t filter_size = base < cap ? base : cap;

    Strength s = strength_at(f, row, col, plane, pass);

    // an edge whose own side has no level takes the other side's, which is what
    // lets a filtered block clean up against an unfiltered one
    if (s.level == 0) s = strength_at(f, prev_row, prev_col, plane, pass);
    if (s.level == 0) return;

    for (uint32_t i = 0; i < AV1_MI_SIZE; i++) {
        int32_t sx = (int32_t) xp + dy * (int32_t) i;
        int32_t sy = (int32_t) yp + dx * (int32_t) i;

        if ((uint32_t) sx >= f->width[plane]) continue;
        if ((uint32_t) sy >= f->height[plane]) continue;

        filter_sample(f, plane, sx, sy, dx, dy, &s, filter_size);
    }
}

void tiny_av1_loop_filter(const TinyAv1Filter* f) {
    if (!f || !f->frame) return;

    for (uint32_t plane = 0; plane < f->planes; plane++) {
        if (plane > 0u && !f->frame->loop_filter_level[1u + plane]) continue;
        if (plane == 0u && !f->frame->loop_filter_level[0] &&
            !f->frame->loop_filter_level[1]) {
            continue;
        }

        uint32_t row_step = plane == 0u ? 1u : (1u << f->sequence->sub_y);
        uint32_t col_step = plane == 0u ? 1u : (1u << f->sequence->sub_x);

        // every vertical edge before any horizontal one, which is the one
        // ordering constraint the filter has
        for (uint32_t pass = 0; pass < 2u; pass++) {
            for (uint32_t row = 0; row < f->frame->mi_rows; row += row_step) {
                for (uint32_t col = 0; col < f->frame->mi_cols;
                     col += col_step) {
                    filter_edge(f, plane, pass, row, col);
                }
            }
        }
    }
}

// #region cdef

/**
 * @brief Whether a sample may be taken from a position, which is a frame test.
 *
 * **The specification's prose and its own function disagree here, and the
 * function is the one that is right.** 7.15.3 says the region is the tile that
 * decoded the block, in a note about `MiColStart` and friends;
 * `is_inside_filter_region` in 5.11.55 then sets `colStart` to 0 and `colEnd`
 * to `MiCols` and ignores them entirely. So CDEF reads across a tile boundary.
 *
 * This was implemented from the note first, and it cost 1,462 samples of
 * `av1-tiles.avif` in a band two columns either side of each seam, which is
 * exactly CDEF's tap reach. `avifdec` agrees with the function.
 */
static int inside_filter_region(
    const TinyAv1Filter* f, int32_t candidate_row, int32_t candidate_col
) {
    return candidate_row >= 0 && candidate_row < (int32_t) f->frame->mi_rows &&
           candidate_col >= 0 && candidate_col < (int32_t) f->frame->mi_cols;
}

/** The damped, clamped difference one tap contributes. */
static int32_t constrain(int32_t diff, int32_t threshold, int32_t damping) {
    if (!threshold) return 0;

    int32_t magnitude = diff < 0 ? -diff : diff;
    int32_t adjusted = damping - (int32_t) filter_log2((uint32_t) threshold);

    if (adjusted < 0) adjusted = 0;

    int32_t value = threshold - (magnitude >> adjusted);

    value = tiny_clampi(value, 0, magnitude);

    return diff < 0 ? -value : value;
}

/**
 * @brief The direction of one 8x8 block, and how strongly it points that way.
 *
 * Eight directions, each scored by summing the samples along its lines and
 * squaring the sums: a block with a real edge has one line whose samples agree
 * and the rest disagree, so its cost is high. The variance the caller uses to
 * scale the primary strength is the winner's cost less the cost of the
 * direction at right angles to it.
 */
static uint32_t cdef_direction(
    const TinyAv1Filter* f, uint32_t row, uint32_t col, int32_t* variance
) {
    int32_t partial[8][15];
    int64_t cost[8];

    tiny_memset(partial, 0, sizeof(partial));

    for (uint32_t i = 0; i < 8u; i++) cost[i] = 0;

    uint32_t x0 = col << AV1_MI_SIZE_LOG2;
    uint32_t y0 = row << AV1_MI_SIZE_LOG2;
    uint32_t shift = f->sequence->bit_depth - 8u;

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            uint32_t sy = y0 + i;
            uint32_t sx = x0 + j;

            if (sy >= f->height[0]) sy = f->height[0] - 1u;
            if (sx >= f->width[0]) sx = f->width[0] - 1u;

            int32_t x =
                (f->plane[0][(size_t) sy * f->stride[0] + sx] >> shift) - 128;

            partial[0][i + j] += x;
            partial[1][i + j / 2u] += x;
            partial[2][i] += x;
            partial[3][3u + i - j / 2u] += x;
            partial[4][7u + i - j] += x;
            partial[5][3u - i / 2u + j] += x;
            partial[6][j] += x;
            partial[7][i / 2u + j] += x;
        }
    }

    for (uint32_t i = 0; i < 8u; i++) {
        cost[2] += (int64_t) partial[2][i] * partial[2][i];
        cost[6] += (int64_t) partial[6][i] * partial[6][i];
    }

    cost[2] *= tiny_av1_div_table[8];
    cost[6] *= tiny_av1_div_table[8];

    for (uint32_t i = 0; i < 7u; i++) {
        cost[0] += ((int64_t) partial[0][i] * partial[0][i] +
                    (int64_t) partial[0][14u - i] * partial[0][14u - i]) *
                   tiny_av1_div_table[i + 1u];
        cost[4] += ((int64_t) partial[4][i] * partial[4][i] +
                    (int64_t) partial[4][14u - i] * partial[4][14u - i]) *
                   tiny_av1_div_table[i + 1u];
    }

    cost[0] += (int64_t) partial[0][7] * partial[0][7] * tiny_av1_div_table[8];
    cost[4] += (int64_t) partial[4][7] * partial[4][7] * tiny_av1_div_table[8];

    for (uint32_t i = 1; i < 8u; i += 2u) {
        for (uint32_t j = 0; j < 5u; j++) {
            cost[i] += (int64_t) partial[i][3u + j] * partial[i][3u + j];
        }

        cost[i] *= tiny_av1_div_table[8];

        for (uint32_t j = 0; j < 3u; j++) {
            cost[i] += ((int64_t) partial[i][j] * partial[i][j] +
                        (int64_t) partial[i][10u - j] * partial[i][10u - j]) *
                       tiny_av1_div_table[2u * j + 2u];
        }
    }

    int64_t best = 0;
    uint32_t direction = 0;

    for (uint32_t i = 0; i < 8u; i++) {
        if (cost[i] > best) {
            best = cost[i];
            direction = i;
        }
    }

    *variance = (int32_t) ((best - cost[(direction + 4u) & 7u]) >> 10);

    return direction;
}

/** One plane of one 8x8 block, filtered from the unfiltered frame. */
static void cdef_filter(
    const TinyAv1Filter* f, uint8_t* const* out, uint32_t plane, uint32_t row,
    uint32_t col, int32_t primary, int32_t secondary, int32_t damping,
    uint32_t direction
) {
    uint32_t sub_x = plane > 0u ? f->sequence->sub_x : 0u;
    uint32_t sub_y = plane > 0u ? f->sequence->sub_y : 0u;

    uint32_t x0 = (col * AV1_MI_SIZE) >> sub_x;
    uint32_t y0 = (row * AV1_MI_SIZE) >> sub_y;
    uint32_t w = 8u >> sub_x;
    uint32_t h = 8u >> sub_y;

    uint32_t shift = f->sequence->bit_depth - 8u;
    uint32_t primary_tap = (uint32_t) (primary >> shift) & 1u;

    for (uint32_t i = 0; i < h; i++) {
        if (y0 + i >= f->height[plane]) break;

        for (uint32_t j = 0; j < w; j++) {
            if (x0 + j >= f->width[plane]) break;

            int32_t middle =
                f->plane[plane][(size_t) (y0 + i) * f->stride[plane] + x0 + j];
            int32_t sum = 0;
            int32_t high = middle;
            int32_t low = middle;

            for (uint32_t k = 0; k < 2u; k++) {
                for (int32_t sign = -1; sign <= 1; sign += 2) {
                    for (int32_t which = 0; which < 3; which++) {
                        uint32_t at =
                            which == 0
                                ? direction
                                : (direction + (which == 1 ? 6u : 2u)) & 7u;

                        int32_t oy = (int32_t) (y0 + i) +
                                     sign * tiny_av1_cdef_directions[at][k][0];
                        int32_t ox = (int32_t) (x0 + j) +
                                     sign * tiny_av1_cdef_directions[at][k][1];

                        // multiplied rather than shifted, because a tap
                        // above or left of the frame is negative and a left
                        // shift of that is undefined
                        int32_t candidate_row =
                            (oy * (1 << sub_y)) >> AV1_MI_SIZE_LOG2;
                        int32_t candidate_col =
                            (ox * (1 << sub_x)) >> AV1_MI_SIZE_LOG2;

                        if (!inside_filter_region(
                                f, candidate_row, candidate_col
                            )) {
                            continue;
                        }
                        if (oy < 0 || ox < 0) continue;
                        if ((uint32_t) oy >= f->height[plane]) continue;
                        if ((uint32_t) ox >= f->width[plane]) continue;

                        int32_t sample =
                            f->plane[plane]
                                    [(size_t) oy * f->stride[plane] +
                                     (size_t) ox];

                        if (which == 0) {
                            sum += tiny_av1_cdef_pri_taps[primary_tap][k] *
                                   constrain(sample - middle, primary, damping);
                        }
                        else {
                            sum +=
                                tiny_av1_cdef_sec_taps[primary_tap][k] *
                                constrain(sample - middle, secondary, damping);
                        }

                        if (sample > high) high = sample;
                        if (sample < low) low = sample;
                    }
                }
            }

            int32_t value = middle + ((8 + sum - (sum < 0 ? 1 : 0)) >> 4);

            out[plane][(size_t) (y0 + i) * f->stride[plane] + x0 + j] =
                (uint8_t) tiny_clampi(value, low, high);
        }
    }
}

void tiny_av1_cdef(const TinyAv1Filter* f, uint8_t* const* out) {
    if (!f || !f->frame || !out) return;

    const TinyAv1Frame* frame = f->frame;

    /*
     * The output is a second frame, because every tap reads the unfiltered one.
     *
     * Filtering in place would let a block read samples an earlier block has
     * already changed, which is a different picture rather than a rounding
     * difference. A rolling two-row copy would do instead and would save the
     * second frame's memory; it is not written that way because the second
     * frame is simple and memory is this project's third priority, not its
     * first.
     */
    for (uint32_t plane = 0; plane < f->planes; plane++) {
        tiny_memcpy(
            out[plane], f->plane[plane],
            (size_t) f->stride[plane] * ((f->height[plane] + 7u) & ~7u)
        );
    }

    uint32_t shift = f->sequence->bit_depth - 8u;

    for (uint32_t row = 0; row < frame->mi_rows; row += 2u) {
        for (uint32_t col = 0; col < frame->mi_cols; col += 2u) {
            uint32_t base_row = row & ~15u;
            uint32_t base_col = col & ~15u;

            int32_t index = f->cdef_idx
                                [(size_t) (base_row >> 4) * f->cdef_stride +
                                 (base_col >> 4)];

            if (index < 0) continue;

            uint32_t next_row = row + 1u < frame->mi_rows ? row + 1u : row;
            uint32_t next_col = col + 1u < frame->mi_cols ? col + 1u : col;

            int skip = mi_at(f, f->skips, row, col) &&
                       mi_at(f, f->skips, next_row, col) &&
                       mi_at(f, f->skips, row, next_col) &&
                       mi_at(f, f->skips, next_row, next_col);

            if (skip) continue;

            int32_t variance = 0;
            uint32_t y_direction = cdef_direction(f, row, col, &variance);

            int32_t primary = frame->cdef_y_pri[index] << shift;
            int32_t secondary = frame->cdef_y_sec[index] << shift;
            uint32_t direction = primary == 0 ? 0u : y_direction;

            uint32_t strength =
                (variance >> 6) ? filter_log2((uint32_t) (variance >> 6)) : 0u;

            if (strength > 12u) strength = 12u;

            primary =
                variance ? (primary * (4 + (int32_t) strength) + 8) >> 4 : 0;

            int32_t damping = (int32_t) frame->cdef_damping + (int32_t) shift;

            cdef_filter(
                f, out, 0u, row, col, primary, secondary, damping, direction
            );

            if (f->planes == 1u) continue;

            primary = frame->cdef_uv_pri[index] << shift;
            secondary = frame->cdef_uv_sec[index] << shift;
            direction =
                primary == 0
                    ? 0u
                    : tiny_av1_cdef_uv_dir[f->sequence->sub_x]
                                          [f->sequence->sub_y][y_direction];

            damping = (int32_t) frame->cdef_damping + (int32_t) shift - 1;

            cdef_filter(
                f, out, 1u, row, col, primary, secondary, damping, direction
            );
            cdef_filter(
                f, out, 2u, row, col, primary, secondary, damping, direction
            );
        }
    }
}
