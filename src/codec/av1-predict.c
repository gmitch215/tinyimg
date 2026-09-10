#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

/**
 * @file
 * @brief Specification 7.11.2, the intra prediction of one transform block.
 *
 * **Written plainly and deliberately not optimized**, and that is a
 * measurement rather than a preference: intra prediction is 2.6% to 3.3% of AV1
 * decode, against 57% for the symbol decoder alone. A four-wide kernel here
 * would buy a fraction of a percent and cost the readability of the one stage
 * in this decoder that has thirteen cases.
 *
 * The stage takes two arrays of already-reconstructed neighbors and produces a
 * block of samples. Everything it needs beyond those arrays is in
 * `TinyAv1PredictOpts`, so this file never reads the frame: the caller knows
 * where the neighbors are and whether they exist, and keeping that knowledge
 * out of here is what makes the thirteen modes testable one at a time.
 *
 * **The corner sits at index 0 of both arrays.** The specification indexes it
 * as `AboveRow[-1]` and `LeftCol[-1]`, which are the same sample; passing it in
 * band rather than at a negative subscript is what keeps every caller free of
 * pointer arithmetic that a compiler cannot check. Inside this file the
 * specification's own indexing is restored, because the directional and
 * upsampling processes both index from -2 and transcribing them any other way
 * is how an off-by-one hides.
 */

// #region constants

/** Degrees of angle per unit of the coded delta, and the largest delta. */
#define AV1_ANGLE_STEP 3
#define AV1_MAX_ANGLE_DELTA 3

/** Taps in the edge filter, and the shift the recursive filter rounds by. */
#define AV1_INTRA_EDGE_TAPS 5
#define AV1_FILTER_SCALE_BITS 4

/**
 * Where index zero of an edge buffer sits.
 *
 * Two, because the upsample process writes `buf[-2]`, and the specification
 * indexes both edges from -1 everywhere else.
 */
#define AV1_EDGE_ORIGIN 2

/**
 * Samples one edge buffer holds past its origin.
 *
 * A transform is at most 64 on a side, so `w + h` is at most 128 and the
 * unupsampled reads stop at `w + h - 1`. Upsampling doubles the indices but
 * only happens when `w + h <= 16`, so it cannot reach further. The buffer is
 * sized well past both because the alternative to slack here is a bounds test
 * inside the innermost loop of the one stage that is not worth optimizing.
 */
#define AV1_EDGE_SAMPLES 264

// #region helpers

/** Round2, which floors, so a negative value needs an arithmetic shift. */
static int32_t predict_round2(int32_t x, uint32_t n) {
    if (n == 0u) return x;

    return (x + (1 << (n - 1u))) >> n;
}

/** Round2Signed, which rounds a negative value away from zero. */
static int32_t predict_round2_signed(int32_t x, uint32_t n) {
    return x >= 0 ? predict_round2(x, n) : -predict_round2(-x, n);
}

/** Clip1, to what a sample of this bit depth can hold. */
static int32_t predict_clip1(int32_t v, uint32_t bit_depth) {
    int32_t high = (int32_t) ((1u << bit_depth) - 1u);

    return v < 0 ? 0 : (v > high ? high : v);
}

/** The smooth prediction weights for one axis, chosen by its log2 extent. */
static const uint8_t* smooth_weights(uint32_t log2) {
    switch (log2) {
        case 2: return tiny_av1_sm_weights_tx_4x4;
        case 3: return tiny_av1_sm_weights_tx_8x8;
        case 4: return tiny_av1_sm_weights_tx_16x16;
        case 5: return tiny_av1_sm_weights_tx_32x32;
        default: return tiny_av1_sm_weights_tx_64x64;
    }
}

/** Log base two of a transform extent, which is always a power of two. */
static uint32_t extent_log2(uint32_t value) {
    uint32_t log2 = 0;

    while ((1u << log2) < value) log2++;

    return log2;
}

// #region edges

/**
 * @brief One edge of the block, in the specification's own indexing.
 *
 * `at(edge, -1)` is the corner and `at(edge, i)` is `AboveRow[i]` or
 * `LeftCol[i]`. The buffer carries two samples before the origin because the
 * upsample process writes one there.
 */
typedef struct {
    uint8_t samples[AV1_EDGE_ORIGIN + AV1_EDGE_SAMPLES];
} Edge;

static int32_t edge_get(const Edge* edge, int32_t i) {
    return edge->samples[(int32_t) AV1_EDGE_ORIGIN + i];
}

static void edge_set(Edge* edge, int32_t i, int32_t value) {
    edge->samples[(int32_t) AV1_EDGE_ORIGIN + i] = (uint8_t) value;
}

/**
 * @brief Fills one edge from what the caller has, replicating past it.
 *
 * Three cases, and the two unavailable ones are not defaults in the ordinary
 * sense: an absent row above is filled from the column to the left and an
 * absent column from the row above, so a block at the frame's edge predicts
 * from the neighbor it does have. Only a block with neither takes a constant,
 * and the two constants differ by one, which is not a typo in the
 * specification.
 */
static void fill_edge(
    Edge* edge, const uint8_t* have, uint32_t available, uint32_t count,
    int32_t from_other, int32_t constant
) {
    if (!have) {
        for (uint32_t i = 0; i < count; i++) {
            edge_set(edge, (int32_t) i, from_other);
        }

        return;
    }

    uint32_t real = available < count ? available : count;

    for (uint32_t i = 0; i < real; i++) {
        edge_set(edge, (int32_t) i, have[1u + i]);
    }

    // past the last real sample the specification repeats it, which is what
    // `Min(aboveLimit, x + i)` says
    int32_t last = real > 0u ? edge_get(edge, (int32_t) real - 1) : constant;

    for (uint32_t i = real; i < count; i++) {
        edge_set(edge, (int32_t) i, last);
    }
}

/** The three tap filter the corner takes when both edges are filtered. */
static int32_t filter_corner(const Edge* above, const Edge* left) {
    int32_t s = edge_get(left, 0) * 5 + edge_get(above, -1) * 6 +
                edge_get(above, 0) * 5;

    return predict_round2(s, 4);
}

/** How hard an edge is filtered, from its extent and its angle. */
static uint32_t edge_strength(
    uint32_t w, uint32_t h, uint32_t filter_type, int32_t delta
) {
    uint32_t d = (uint32_t) (delta < 0 ? -delta : delta);
    uint32_t both = w + h;
    uint32_t strength = 0;

    if (!filter_type) {
        if (both <= 8u) {
            if (d >= 56u) strength = 1u;
        }
        else if (both <= 12u) {
            if (d >= 40u) strength = 1u;
        }
        else if (both <= 16u) {
            if (d >= 40u) strength = 1u;
        }
        else if (both <= 24u) {
            if (d >= 8u) strength = 1u;
            if (d >= 16u) strength = 2u;
            if (d >= 32u) strength = 3u;
        }
        else if (both <= 32u) {
            strength = 1u;
            if (d >= 4u) strength = 2u;
            if (d >= 32u) strength = 3u;
        }
        else {
            strength = 3u;
        }

        return strength;
    }

    if (both <= 8u) {
        if (d >= 40u) strength = 1u;
        if (d >= 64u) strength = 2u;
    }
    else if (both <= 16u) {
        if (d >= 20u) strength = 1u;
        if (d >= 48u) strength = 2u;
    }
    else if (both <= 24u) {
        if (d >= 4u) strength = 3u;
    }
    else {
        strength = 3u;
    }

    return strength;
}

/** Whether an edge is upsampled before the directional read walks it. */
static int use_upsample(
    uint32_t w, uint32_t h, uint32_t filter_type, int32_t delta
) {
    uint32_t d = (uint32_t) (delta < 0 ? -delta : delta);
    uint32_t both = w + h;

    if (d == 0u || d >= 40u) return 0;
    if (!filter_type) return both <= 16u;

    return both <= 8u;
}

/** The five tap smoothing the specification runs along an edge. */
static void filter_edge(Edge* edge, uint32_t size, uint32_t strength) {
    if (strength == 0u) return;

    uint8_t copy[AV1_EDGE_SAMPLES];
    uint32_t bound = size < AV1_EDGE_SAMPLES ? size : AV1_EDGE_SAMPLES;

    for (uint32_t i = 0; i < bound; i++) {
        copy[i] = (uint8_t) edge_get(edge, (int32_t) i - 1);
    }

    for (uint32_t i = 1; i < bound; i++) {
        int32_t s = 0;

        for (uint32_t j = 0; j < AV1_INTRA_EDGE_TAPS; j++) {
            int32_t k = tiny_clampi(
                (int32_t) i - 2 + (int32_t) j, 0, (int32_t) bound - 1
            );

            s += tiny_av1_intra_edge_kernel[strength - 1u][j] * copy[k];
        }

        edge_set(edge, (int32_t) i - 1, (s + 8) >> 4);
    }
}

/**
 * @brief Doubles the resolution of an edge in place.
 *
 * A four tap interpolation, and the samples move: after it, index `2 * i` holds
 * what index `i` held and the odd indices hold the interpolated samples, which
 * is why the directional read shifts its own indices by `upsample`.
 */
static void upsample_edge(Edge* edge, uint32_t count, uint32_t bit_depth) {
    uint8_t dup[AV1_EDGE_SAMPLES + 3];
    uint32_t bound = count < AV1_EDGE_SAMPLES ? count : AV1_EDGE_SAMPLES;

    dup[0] = (uint8_t) edge_get(edge, -1);

    for (int32_t i = -1; i < (int32_t) bound; i++) {
        dup[i + 2] = (uint8_t) edge_get(edge, i);
    }

    dup[bound + 2] = (uint8_t) edge_get(edge, (int32_t) bound - 1);

    edge_set(edge, -2, dup[0]);

    for (uint32_t i = 0; i < bound; i++) {
        int32_t s = -(int32_t) dup[i] + 9 * (int32_t) dup[i + 1] +
                    9 * (int32_t) dup[i + 2] - (int32_t) dup[i + 3];

        s = predict_clip1(predict_round2(s, 4), bit_depth);

        edge_set(edge, 2 * (int32_t) i - 1, s);
        edge_set(edge, 2 * (int32_t) i, dup[i + 2]);
    }
}

// #region modes

/** DC, which averages whichever edges exist. */
static void predict_dc(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const Edge* above,
    const Edge* left, int have_above, int have_left, uint32_t bit_depth
) {
    int32_t value;

    if (have_above && have_left) {
        int32_t sum = 0;

        for (uint32_t k = 0; k < h; k++) sum += edge_get(left, (int32_t) k);
        for (uint32_t k = 0; k < w; k++) sum += edge_get(above, (int32_t) k);

        sum += (int32_t) ((w + h) >> 1);
        value = sum / (int32_t) (w + h);
    }
    else if (have_left) {
        int32_t sum = 0;

        for (uint32_t k = 0; k < h; k++) sum += edge_get(left, (int32_t) k);

        value = predict_clip1(
            (sum + (int32_t) (h >> 1)) >> extent_log2(h), bit_depth
        );
    }
    else if (have_above) {
        int32_t sum = 0;

        for (uint32_t k = 0; k < w; k++) sum += edge_get(above, (int32_t) k);

        value = predict_clip1(
            (sum + (int32_t) (w >> 1)) >> extent_log2(w), bit_depth
        );
    }
    else {
        value = (int32_t) (1u << (bit_depth - 1u));
    }

    for (uint32_t i = 0; i < h; i++) {
        tiny_memset(dst + (size_t) i * stride, (uint8_t) value, w);
    }
}

/** Paeth, which picks whichever neighbor the gradient points at. */
static void predict_paeth(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const Edge* above,
    const Edge* left
) {
    int32_t corner = edge_get(above, -1);

    for (uint32_t i = 0; i < h; i++) {
        int32_t l = edge_get(left, (int32_t) i);

        for (uint32_t j = 0; j < w; j++) {
            int32_t a = edge_get(above, (int32_t) j);
            int32_t base = a + l - corner;

            int32_t to_left = base - l;
            int32_t to_top = base - a;
            int32_t to_corner = base - corner;

            if (to_left < 0) to_left = -to_left;
            if (to_top < 0) to_top = -to_top;
            if (to_corner < 0) to_corner = -to_corner;

            int32_t value;

            if (to_left <= to_top && to_left <= to_corner)
                value = l;
            else if (to_top <= to_corner)
                value = a;
            else
                value = corner;

            dst[(size_t) i * stride + j] = (uint8_t) value;
        }
    }
}

/** The three smooth modes, which interpolate between the two edges. */
static void predict_smooth(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const Edge* above,
    const Edge* left, uint32_t mode
) {
    const uint8_t* weights_x = smooth_weights(extent_log2(w));
    const uint8_t* weights_y = smooth_weights(extent_log2(h));

    int32_t bottom = edge_get(left, (int32_t) h - 1);
    int32_t right = edge_get(above, (int32_t) w - 1);

    for (uint32_t i = 0; i < h; i++) {
        int32_t l = edge_get(left, (int32_t) i);

        for (uint32_t j = 0; j < w; j++) {
            int32_t a = edge_get(above, (int32_t) j);
            int32_t value;

            if (mode == TINY_AV1_SMOOTH_PRED) {
                int32_t sum = weights_y[i] * a + (256 - weights_y[i]) * bottom +
                              weights_x[j] * l + (256 - weights_x[j]) * right;

                value = predict_round2(sum, 9);
            }
            else if (mode == TINY_AV1_SMOOTH_V_PRED) {
                int32_t sum = weights_y[i] * a + (256 - weights_y[i]) * bottom;

                value = predict_round2(sum, 8);
            }
            else {
                int32_t sum = weights_x[j] * l + (256 - weights_x[j]) * right;

                value = predict_round2(sum, 8);
            }

            dst[(size_t) i * stride + j] = (uint8_t) value;
        }
    }
}

/**
 * @brief The recursive filter modes, which predict from what they just
 * predicted.
 *
 * Four samples wide and two tall at a time, each block filtered from seven
 * neighbors: the five above it and the two to its left. The blocks after the
 * first row and column read the prediction itself, which is why this writes
 * into `dst` and reads back from it rather than building the block in a
 * temporary.
 */
static void predict_filter_intra(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const Edge* above,
    const Edge* left, uint32_t filter_mode, uint32_t bit_depth
) {
    uint32_t w4 = w >> 2;
    uint32_t h2 = h >> 1;

    for (uint32_t i2 = 0; i2 < h2; i2++) {
        for (uint32_t j4 = 0; j4 < w4; j4++) {
            int32_t p[7];

            for (uint32_t i = 0; i < 7u; i++) {
                if (i < 5u) {
                    if (i2 == 0u) {
                        p[i] = edge_get(above, (int32_t) ((j4 << 2) + i) - 1);
                    }
                    else if (j4 == 0u && i == 0u) {
                        p[i] = edge_get(left, (int32_t) (i2 << 1) - 1);
                    }
                    else {
                        size_t row = (size_t) ((i2 << 1) - 1u);
                        size_t col = (size_t) ((j4 << 2) + i - 1u);

                        p[i] = dst[row * stride + col];
                    }
                }
                else if (j4 == 0u) {
                    p[i] = edge_get(left, (int32_t) ((i2 << 1) + i - 5u));
                }
                else {
                    size_t row = (size_t) ((i2 << 1) + i - 5u);
                    size_t col = (size_t) ((j4 << 2) - 1u);

                    p[i] = dst[row * stride + col];
                }
            }

            for (uint32_t i1 = 0; i1 < 2u; i1++) {
                for (uint32_t j1 = 0; j1 < 4u; j1++) {
                    int32_t pr = 0;

                    for (uint32_t i = 0; i < 7u; i++) {
                        pr += tiny_av1_intra_filter_taps[filter_mode]
                                                        [(i1 << 2) + j1][i] *
                              p[i];
                    }

                    size_t row = (size_t) ((i2 << 1) + i1);
                    size_t col = (size_t) ((j4 << 2) + j1);

                    dst[row * stride + col] = (uint8_t) predict_clip1(
                        predict_round2_signed(pr, AV1_FILTER_SCALE_BITS),
                        bit_depth
                    );
                }
            }
        }
    }
}

/**
 * @brief The eight directional modes, which walk one or both edges at an angle.
 *
 * Three branches, split by where the ray lands rather than by mode. Above 180
 * degrees it only reads the left column, below 90 only the row above, and
 * between the two it reads whichever the ray reaches, which is the one branch
 * that touches both edges and the reason they are filtered together.
 */
static void predict_directional(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, Edge* above,
    Edge* left, uint32_t mode, const TinyAv1PredictOpts* opts
) {
    int32_t angle = (int32_t) tiny_av1_mode_to_angle[mode] +
                    (int32_t) opts->angle_delta * AV1_ANGLE_STEP;

    uint32_t up_above = 0;
    uint32_t up_left = 0;

    if (opts->edge_filter) {
        if (angle != 90 && angle != 180) {
            if (angle > 90 && angle < 180 && (w + h) >= 24u) {
                int32_t corner = filter_corner(above, left);

                edge_set(above, -1, corner);
                edge_set(left, -1, corner);
            }

            if (opts->have_above) {
                uint32_t strength =
                    edge_strength(w, h, opts->filter_type, angle - 90);
                uint32_t inside =
                    opts->above_available < w ? opts->above_available : w;
                uint32_t count = inside + (angle < 90 ? h : 0u) + 1u;

                filter_edge(above, count, strength);
            }

            if (opts->have_left) {
                uint32_t strength =
                    edge_strength(w, h, opts->filter_type, angle - 180);
                uint32_t inside =
                    opts->left_available < h ? opts->left_available : h;
                uint32_t count = inside + (angle > 180 ? w : 0u) + 1u;

                filter_edge(left, count, strength);
            }
        }

        up_above = (uint32_t) use_upsample(w, h, opts->filter_type, angle - 90);

        if (up_above) {
            upsample_edge(above, w + (angle < 90 ? h : 0u), opts->bit_depth);
        }

        up_left = (uint32_t) use_upsample(w, h, opts->filter_type, angle - 180);

        if (up_left) {
            upsample_edge(left, h + (angle > 180 ? w : 0u), opts->bit_depth);
        }
    }

    if (angle == 90) {
        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                dst[(size_t) i * stride + j] =
                    (uint8_t) edge_get(above, (int32_t) j);
            }
        }

        return;
    }

    if (angle == 180) {
        for (uint32_t i = 0; i < h; i++) {
            uint8_t value = (uint8_t) edge_get(left, (int32_t) i);

            tiny_memset(dst + (size_t) i * stride, value, w);
        }

        return;
    }

    int32_t dx = 0;
    int32_t dy = 0;

    // the upsample shift is applied as a multiply, because `idx` is signed in
    // two of the three branches and a left shift of a negative value is
    // undefined
    int32_t scale_above = 1 << up_above;
    int32_t scale_left = 1 << up_left;

    if (angle < 90)
        dx = tiny_av1_dr_intra_derivative[angle];
    else if (angle < 180)
        dx = tiny_av1_dr_intra_derivative[180 - angle];

    if (angle > 90 && angle < 180) {
        dy = tiny_av1_dr_intra_derivative[angle - 90];
    }
    else if (angle > 180) {
        dy = tiny_av1_dr_intra_derivative[270 - angle];
    }

    if (angle < 90) {
        int32_t max_base = (int32_t) ((w + h - 1u) << up_above);

        for (uint32_t i = 0; i < h; i++) {
            int32_t idx = (int32_t) (i + 1u) * dx;

            for (uint32_t j = 0; j < w; j++) {
                int32_t base = (idx >> (6u - up_above)) +
                               (int32_t) ((uint32_t) j << up_above);
                int32_t shift = ((idx * scale_above) >> 1) & 0x1F;
                int32_t value;

                if (base < max_base) {
                    value = predict_round2(
                        edge_get(above, base) * (32 - shift) +
                            edge_get(above, base + 1) * shift,
                        5
                    );
                }
                else {
                    value = edge_get(above, max_base);
                }

                dst[(size_t) i * stride + j] = (uint8_t) value;
            }
        }

        return;
    }

    if (angle < 180) {
        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                int32_t idx =
                    (int32_t) ((uint32_t) j << 6) - (int32_t) (i + 1u) * dx;
                int32_t base = idx >> (6u - up_above);
                int32_t value;

                if (base >= -(int32_t) (1u << up_above)) {
                    int32_t shift = ((idx * scale_above) >> 1) & 0x1F;

                    value = predict_round2(
                        edge_get(above, base) * (32 - shift) +
                            edge_get(above, base + 1) * shift,
                        5
                    );
                }
                else {
                    // the ray has passed the corner, so it lands on the left
                    // column instead and the whole read is recomputed there
                    idx =
                        (int32_t) ((uint32_t) i << 6) - (int32_t) (j + 1u) * dy;
                    base = idx >> (6u - up_left);

                    int32_t shift = ((idx * scale_left) >> 1) & 0x1F;

                    value = predict_round2(
                        edge_get(left, base) * (32 - shift) +
                            edge_get(left, base + 1) * shift,
                        5
                    );
                }

                dst[(size_t) i * stride + j] = (uint8_t) value;
            }
        }

        return;
    }

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            int32_t idx = (int32_t) (j + 1u) * dy;
            int32_t base =
                (idx >> (6u - up_left)) + (int32_t) ((uint32_t) i << up_left);
            int32_t shift = ((idx * scale_left) >> 1) & 0x1F;

            int32_t value = predict_round2(
                edge_get(left, base) * (32 - shift) +
                    edge_get(left, base + 1) * shift,
                5
            );

            dst[(size_t) i * stride + j] = (uint8_t) value;
        }
    }
}

/**
 * @brief One chroma position's luma, subsampled, at three fractional bits.
 *
 * The clamp is what stops the subsample reading luma the decode has not
 * reconstructed: a chroma transform can cover luma past the end of its own
 * block, and `luma_w` and `luma_h` are how far the caller says it may look.
 */
static int32_t cfl_luma(
    const uint8_t* luma, uint32_t luma_stride, uint32_t luma_w, uint32_t luma_h,
    uint32_t i, uint32_t j, uint32_t sub_x, uint32_t sub_y
) {
    uint32_t step_x = 1u << sub_x;
    uint32_t step_y = 1u << sub_y;

    uint32_t y = i << sub_y;
    uint32_t x = j << sub_x;

    if (y + step_y > luma_h) y = luma_h - step_y;
    if (x + step_x > luma_w) x = luma_w - step_x;

    int32_t total = 0;

    for (uint32_t dy = 0; dy <= sub_y; dy++) {
        for (uint32_t dx = 0; dx <= sub_x; dx++) {
            total += luma[(size_t) (y + dy) * luma_stride + x + dx];
        }
    }

    return total << (3u - sub_x - sub_y);
}

// #region entry points

int tiny_av1_predict_intra(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const uint8_t* above,
    const uint8_t* left, TinyAv1IntraMode mode, const TinyAv1PredictOpts* opts
) {
    if (!dst || !opts) return TINYIMG_ERR_NULL;
    if (w == 0u || h == 0u || w > 64u || h > 64u) return TINYIMG_ERR_BOUNDS;
    if (stride < w) return TINYIMG_ERR_BOUNDS;
    if (opts->bit_depth != 8u) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (mode > TINY_AV1_PAETH_PRED) return TINYIMG_ERR_BOUNDS;
    if (opts->have_above && !above) return TINYIMG_ERR_NULL;
    if (opts->have_left && !left) return TINYIMG_ERR_NULL;

    Edge above_edge;
    Edge left_edge;
    uint32_t count = w + h;

    /*
     * The three constants differ by one, and that is the specification's.
     *
     * A block with no neighbor at all takes `1 << (BitDepth - 1)` for the
     * corner, one less along the row above and one more along the column to the
     * left. Written out because it reads like a transcription slip.
     */
    int32_t middle = (int32_t) (1u << (opts->bit_depth - 1u));

    fill_edge(
        &above_edge, opts->have_above ? above : 0, opts->above_available, count,
        opts->have_left ? left[1] : middle - 1, middle - 1
    );

    fill_edge(
        &left_edge, opts->have_left ? left : 0, opts->left_available, count,
        opts->have_above ? above[1] : middle + 1, middle + 1
    );

    int32_t corner;

    if (opts->have_above && opts->have_left)
        corner = above[0];
    else if (opts->have_above)
        corner = above[1];
    else if (opts->have_left)
        corner = left[1];
    else
        corner = middle;

    edge_set(&above_edge, -1, corner);
    edge_set(&left_edge, -1, corner);

    if (opts->use_filter_intra) {
        predict_filter_intra(
            dst, stride, w, h, &above_edge, &left_edge, opts->filter_intra_mode,
            opts->bit_depth
        );

        return TINYIMG_OK;
    }

    if (mode >= TINY_AV1_V_PRED && mode <= TINY_AV1_D67_PRED) {
        predict_directional(
            dst, stride, w, h, &above_edge, &left_edge, mode, opts
        );

        return TINYIMG_OK;
    }

    if (mode == TINY_AV1_SMOOTH_PRED || mode == TINY_AV1_SMOOTH_V_PRED ||
        mode == TINY_AV1_SMOOTH_H_PRED) {
        predict_smooth(dst, stride, w, h, &above_edge, &left_edge, mode);

        return TINYIMG_OK;
    }

    if (mode == TINY_AV1_DC_PRED) {
        predict_dc(
            dst, stride, w, h, &above_edge, &left_edge, opts->have_above,
            opts->have_left, opts->bit_depth
        );

        return TINYIMG_OK;
    }

    predict_paeth(dst, stride, w, h, &above_edge, &left_edge);

    return TINYIMG_OK;
}

int tiny_av1_predict_cfl(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const uint8_t* luma,
    uint32_t luma_stride, uint32_t luma_w, uint32_t luma_h, uint32_t sub_x,
    uint32_t sub_y, int32_t alpha, uint32_t bit_depth
) {
    if (!dst || !luma) return TINYIMG_ERR_NULL;
    if (w == 0u || h == 0u || w > 64u || h > 64u) return TINYIMG_ERR_BOUNDS;
    if (stride < w) return TINYIMG_ERR_BOUNDS;
    if (bit_depth != 8u) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (luma_w < (1u << sub_x) || luma_h < (1u << sub_y)) {
        return TINYIMG_ERR_BOUNDS;
    }

    /*
     * Two passes over the block rather than one pass into a buffer.
     *
     * The average has to be known before any sample can be written, and
     * buffering the subsampled luma to avoid recomputing it would put 4 KiB on
     * the stack for a stage that is a fraction of a percent of decode. The
     * subsample is four loads a sample at 4:2:0 and one at 4:4:4.
     */
    int32_t sum = 0;

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            sum +=
                cfl_luma(luma, luma_stride, luma_w, luma_h, i, j, sub_x, sub_y);
        }
    }

    int32_t average = predict_round2(sum, extent_log2(w) + extent_log2(h));

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            int32_t scaled =
                cfl_luma(luma, luma_stride, luma_w, luma_h, i, j, sub_x, sub_y);
            int32_t dc = dst[(size_t) i * stride + j];
            int32_t high = predict_round2_signed(alpha * (scaled - average), 6);

            dst[(size_t) i * stride + j] =
                (uint8_t) predict_clip1(dc + high, bit_depth);
        }
    }

    return TINYIMG_OK;
}
