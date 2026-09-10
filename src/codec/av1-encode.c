#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

/**
 * @file
 * @brief An AV1 intra encoder with fixed partitions and no rate-distortion
 * search.
 *
 * **What it does and does not do, plainly.** Every block is 8x8 with one 8x8
 * luma transform and one 4x4 transform per chroma plane, chosen once rather
 * than searched. The luma prediction mode is picked from four candidates by the
 * absolute difference against the source, which is a distortion comparison and
 * not a rate-distortion one: no candidate is ever costed in bits. Chroma always
 * predicts DC. Every post-filter is off, there is one tile, and segmentation,
 * quantizer deltas, palette, filter intra and chroma from luma are all
 * unreachable by construction.
 *
 * So the output is bigger than libaom's at the same quality, and the reason is
 * structural rather than a missing optimization: a partition search is where
 * most of AV1's compression lives. What this buys instead is an encoder that
 * runs in one pass over the image with no trial encodes.
 *
 * **The encoder is the decoder's mirror and shares its arithmetic.** The
 * symbol writer computes the same interval boundaries the reader does, the
 * coefficient writer uses the same context functions as the parse, and the
 * reconstruction runs the same prediction and inverse transform the decoder
 * will. Nothing here is a second reading of the specification, which is what
 * makes a round trip through the pair a real check.
 *
 * The forward transform is calibrated against the inverse rather than derived:
 * a dc coefficient of 1,024 comes back as a flat residual of 32 at 4x4 and 16
 * at 8x8, so the forward is the orthonormal DCT scaled by eight at both sizes.
 * Measured, not assumed.
 */

// #region constants

/** Samples along one side of an mi unit. */
#define AV1_MI_SIZE 4

/** The block and transform sizes this encoder uses, and nothing else. */
#define AV1_ENC_BLOCK TINY_AV1_BLOCK_8X8
#define AV1_ENC_LUMA_TX TINY_AV1_TX_8X8
#define AV1_ENC_CHROMA_TX TINY_AV1_TX_4X4

/** Scale the forward transform applies over the orthonormal one. */
#define AV1_FORWARD_SCALE 8.0f

/** Levels the coefficient writer codes before the Golomb tail takes over. */
#define AV1_NUM_BASE_LEVELS 2
#define AV1_COEFF_BASE_RANGE 12
#define AV1_BR_CDF_SIZE 4

// #region bit writer

/** The header bit writer, most significant bit first, mirroring `Av1Bits`. */
typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t bit;
    uint8_t overflow;
} Av1Write;

static void write_init(Av1Write* w, uint8_t* data, size_t capacity) {
    w->data = data;
    w->capacity = capacity;
    w->bit = 0;
    w->overflow = 0;

    tiny_memset(data, 0, capacity);
}

static void write_bits(Av1Write* w, uint32_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        size_t index = (w->bit + i) >> 3;

        if (index >= w->capacity) {
            w->overflow = 1u;
            return;
        }

        uint32_t bit = (value >> (count - 1u - i)) & 1u;

        if (bit) {
            w->data[index] |= (uint8_t) (0x80u >> ((w->bit + i) & 7u));
        }
    }

    w->bit += count;
}

/** Pads to the next byte with a single one bit, which every OBU ends with. */
static void write_trailing(Av1Write* w) {
    write_bits(w, 1u, 1u);

    while (w->bit & 7u) write_bits(w, 0u, 1u);
}

static size_t write_bytes(const Av1Write* w) {
    return (w->bit + 7u) >> 3;
}

/** leb128, which every OBU's size field is. */
static size_t write_leb128(uint8_t* out, size_t capacity, uint64_t value) {
    size_t at = 0;

    do {
        if (at >= capacity) return 0;

        uint8_t byte = (uint8_t) (value & 0x7Fu);
        value >>= 7;

        out[at++] = value ? (uint8_t) (byte | 0x80u) : byte;
    } while (value);

    return at;
}

// #region headers

/**
 * @brief Writes the sequence header for a still picture.
 *
 * Mirrors `tiny_av1_read_sequence` field for field, including the fields a
 * reduced header implies rather than codes. Six of them are deliberately zero
 * and each removes a whole stage from both sides: no 128x128 superblocks, no
 * filter intra, no intra edge filter, no superres, no CDEF and no loop
 * restoration.
 */
static void write_sequence(
    Av1Write* w, uint32_t width, uint32_t height, uint32_t planes,
    uint32_t sub_x, uint32_t sub_y
) {
    uint32_t profile = sub_x == 0u && sub_y == 0u && planes > 1u ? 1u : 0u;

    write_bits(w, profile, 3u);
    write_bits(w, 1u, 1u);
    write_bits(w, 1u, 1u);

    /*
     * Level 5.3, which is an upper bound rather than a description.
     *
     * A level says what a decoder must be able to handle, so naming a large one
     * for a small picture is legal and naming a small one for a large picture
     * is not. This library's own pixel ceiling is well inside 5.3.
     */
    write_bits(w, 15u, 5u);

    uint32_t width_bits = 1u;
    uint32_t height_bits = 1u;

    while ((1u << width_bits) < width) width_bits++;
    while ((1u << height_bits) < height) height_bits++;

    write_bits(w, width_bits - 1u, 4u);
    write_bits(w, height_bits - 1u, 4u);
    write_bits(w, width - 1u, width_bits);
    write_bits(w, height - 1u, height_bits);

    write_bits(w, 0u, 1u);
    write_bits(w, 0u, 1u);
    write_bits(w, 0u, 1u);

    write_bits(w, 0u, 1u);
    write_bits(w, 0u, 1u);
    write_bits(w, 0u, 1u);

    // colour config: eight bit, and monochrome only when there is one plane
    write_bits(w, 0u, 1u);

    if (profile != 1u) write_bits(w, planes == 1u ? 1u : 0u, 1u);

    /*
     * BT.709 primaries and transfer with BT.601 coefficients would be a lie, so
     * the description is written out rather than left unspecified: primaries 1,
     * transfer 13 which is sRGB, and matrix 6.
     */
    write_bits(w, 1u, 1u);
    write_bits(w, 1u, 8u);
    write_bits(w, 13u, 8u);
    write_bits(w, planes == 1u ? 0u : 6u, 8u);

    if (planes == 1u) {
        write_bits(w, 1u, 1u);
        write_bits(w, 0u, 1u);
        write_bits(w, 0u, 1u);

        return;
    }

    // full range, so the conversion neither compresses nor expands
    write_bits(w, 1u, 1u);

    // 4:2:0 is implied by profile 0 and 4:4:4 by profile 1, and only a fully
    // subsampled plane carries a sample position
    if (profile == 0u) write_bits(w, 0u, 2u);

    write_bits(w, 0u, 1u);

    // film grain, which is the sequence header's last field and is easy to
    // leave out: the reader would take the trailing one bit for it
    write_bits(w, 0u, 1u);
}

/**
 * @brief Writes the frame header for one shown key frame.
 *
 * The mirror of `tiny_av1_read_frame`, and the same rule applies: a field
 * skipped where the reader expects one shifts everything after it, so the two
 * are written in the same order with the same conditions.
 */
static void write_frame(
    Av1Write* w, uint32_t q_index, uint32_t planes, uint32_t max_cols_log2,
    uint32_t max_rows_log2
) {
    write_bits(w, 0u, 1u);
    write_bits(w, 0u, 1u);

    // frame_size takes its default and render_size says it is the same
    write_bits(w, 0u, 1u);

    /*
     * Tile info: uniform spacing, and one tile.
     *
     * The increment bits are the trap. The reader only reads one while its
     * running log2 is below the maximum the frame's size allows, so a picture
     * small enough that the maximum is already zero codes **no** increment bit
     * at all; writing one anyway shifts the quantizer index by two bits and the
     * frame header stops parsing several fields later, which is what happened.
     */
    write_bits(w, 1u, 1u);

    if (max_cols_log2 > 0u) write_bits(w, 0u, 1u);
    if (max_rows_log2 > 0u) write_bits(w, 0u, 1u);

    // quantization: the base index, no plane deltas, no matrix
    write_bits(w, q_index, 8u);
    write_bits(w, 0u, 1u);

    if (planes > 1u) {
        write_bits(w, 0u, 1u);
        write_bits(w, 0u, 1u);
    }

    write_bits(w, 0u, 1u);

    // segmentation off
    write_bits(w, 0u, 1u);

    // delta q, which is only coded at all above a zero quantizer
    if (q_index > 0u) write_bits(w, 0u, 1u);

    // loop filter: both levels zero, which is what turns the deblock off
    write_bits(w, 0u, 6u);
    write_bits(w, 0u, 6u);
    write_bits(w, 0u, 3u);
    write_bits(w, 0u, 1u);

    // CDEF and loop restoration are both disabled in the sequence header, so
    // neither has parameters here

    // tx mode: largest, so no per-block transform size is coded
    write_bits(w, 0u, 1u);

    // the reduced transform set, which makes the type five symbols not seven
    write_bits(w, 1u, 1u);
}

// #region forward transform

/**
 * @brief The forward DCT, as the inverse's adjoint at float precision.
 *
 * Written as the mathematical transform rather than as an integer butterfly,
 * because the encoder's transform does not have to be the bit-exact inverse of
 * the decoder's; it has to be close enough that quantizing its output and
 * running the decoder's inverse gives back the residual. The scale is measured
 * against the inverse that ships rather than derived.
 */
static void forward_dct(const int32_t* residual, int32_t* out, uint32_t n) {
    float rows[32 * 32];

    float scale = AV1_FORWARD_SCALE / (float) n;

    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t u = 0; u < n; u++) {
            float sum = 0.0f;

            for (uint32_t j = 0; j < n; j++) {
                float angle = 3.14159265358979f * ((float) j + 0.5f) *
                              (float) u / (float) n;

                sum += (float) residual[i * 32u + j] * tiny_cosf(angle);
            }

            rows[i * 32u + u] = sum * (u == 0u ? 1.0f : 1.41421356f);
        }
    }

    for (uint32_t u = 0; u < n; u++) {
        for (uint32_t v = 0; v < n; v++) {
            float sum = 0.0f;

            for (uint32_t i = 0; i < n; i++) {
                float angle = 3.14159265358979f * ((float) i + 0.5f) *
                              (float) v / (float) n;

                sum += rows[i * 32u + u] * tiny_cosf(angle);
            }

            sum *= (v == 0u ? 1.0f : 1.41421356f) * scale;

            out[v * 32u + u] = (int32_t) (sum < 0.0f ? sum - 0.5f : sum + 0.5f);
        }
    }
}

// #region encoder state

/** Everything one frame's encode carries. */
typedef struct {
    TinyAv1Sequence sequence;
    TinyAv1Frame frame;

    /** The source planes, and the reconstruction the prediction reads. */
    uint8_t* source[3];
    uint8_t* recon[3];
    uint32_t stride[3];
    uint32_t width[3];
    uint32_t height[3];
    uint32_t planes;

    TinyAv1Cdf* cdf;
    TinyAv1SymbolEnc symbol;

    /** The mi arrays the mode contexts read, exactly as the decoder's. */
    uint8_t* sizes;
    uint8_t* y_modes;
    uint8_t* skips;
    uint8_t* tx_sizes;

    /** The coefficient contexts, which the shared writer reads. */
    TinyAv1Residual* residual;

    /** Scratch for one transform block. */
    int32_t block[32 * 32];
    int32_t coeffs[32 * 32];
    uint8_t above[1 + 2 * 64];
    uint8_t left[1 + 2 * 64];

    uint32_t q_index;
} Encoder;

/** Whether an mi position is inside the frame, which is the whole tile here. */
static int enc_inside(const Encoder* e, int32_t row, int32_t col) {
    return row >= 0 && col >= 0 && row < (int32_t) e->frame.mi_rows &&
           col < (int32_t) e->frame.mi_cols;
}

// #region prediction

/**
 * @brief Fills the neighbor arrays from the reconstruction, corner first.
 *
 * The same layout `tiny_av1_predict_intra` documents and the same replication
 * rule, because this is the array the decoder will build for itself: an
 * encoder that predicted from the source rather than from the reconstruction
 * would drift a little further from the decoder with every block.
 */
static void enc_gather(
    Encoder* e, uint32_t plane, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    int have_above, int have_left, uint32_t* above_count, uint32_t* left_count
) {
    const uint8_t* data = e->recon[plane];
    uint32_t stride = e->stride[plane];

    *above_count = 0;
    *left_count = 0;

    if (have_above) {
        uint32_t limit = x + w - 1u;

        if (limit > e->width[plane] - 1u) limit = e->width[plane] - 1u;

        *above_count = limit - x + 1u;

        for (uint32_t i = 0; i < *above_count; i++) {
            e->above[1u + i] = data[(size_t) (y - 1u) * stride + x + i];
        }
    }

    if (have_left) {
        uint32_t limit = y + h - 1u;

        if (limit > e->height[plane] - 1u) limit = e->height[plane] - 1u;

        *left_count = limit - y + 1u;

        for (uint32_t i = 0; i < *left_count; i++) {
            e->left[1u + i] = data[(size_t) (y + i) * stride + x - 1u];
        }
    }

    uint8_t corner = 0;

    if (have_above && have_left) {
        corner = data[(size_t) (y - 1u) * stride + x - 1u];
    }

    e->above[0] = corner;
    e->left[0] = corner;
}

/** Predicts one transform block into `dst`, at the given mode. */
static void enc_predict(
    Encoder* e, uint32_t w, uint32_t h, uint32_t mode, int have_above,
    int have_left, uint32_t above_count, uint32_t left_count, uint8_t* dst,
    uint32_t dst_stride
) {
    TinyAv1PredictOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));

    opts.have_above = (uint8_t) (have_above ? 1 : 0);
    opts.have_left = (uint8_t) (have_left ? 1 : 0);
    opts.above_available = (uint16_t) above_count;
    opts.left_available = (uint16_t) left_count;
    opts.bit_depth = 8u;

    tiny_av1_predict_intra(
        dst, dst_stride, w, h, e->above, e->left, (TinyAv1IntraMode) mode, &opts
    );
}

/**
 * @brief Picks the luma mode by absolute difference against the source.
 *
 * Four candidates, and the reason it is four rather than thirteen is that the
 * other nine are directional modes whose angle deltas would each need their own
 * trial. This is a distortion comparison: the candidate that predicts the
 * source most closely wins, and nothing is costed in bits, so it is not a
 * rate-distortion search.
 */
static uint32_t enc_choose_mode(
    Encoder* e, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int have_above,
    int have_left, uint32_t above_count, uint32_t left_count
) {
    static const uint32_t candidates[4] = {
        TINY_AV1_DC_PRED, TINY_AV1_V_PRED, TINY_AV1_H_PRED, TINY_AV1_PAETH_PRED
    };

    uint8_t trial[64 * 64];
    uint32_t best = TINY_AV1_DC_PRED;
    uint32_t best_cost = 0xFFFFFFFFu;

    for (uint32_t c = 0; c < 4u; c++) {
        enc_predict(
            e, w, h, candidates[c], have_above, have_left, above_count,
            left_count, trial, 64u
        );

        uint32_t cost = 0;

        for (uint32_t i = 0; i < h; i++) {
            const uint8_t* row =
                e->source[0] + (size_t) (y + i) * e->stride[0] + x;

            for (uint32_t j = 0; j < w; j++) {
                int32_t difference = (int32_t) row[j] - trial[i * 64u + j];

                cost += (uint32_t) (difference < 0 ? -difference : difference);
            }
        }

        if (cost < best_cost) {
            best_cost = cost;
            best = candidates[c];
        }
    }

    return best;
}

// #region blocks

/**
 * @brief Transforms, quantizes, writes and reconstructs one transform block.
 *
 * The reconstruction is the point of the last step: the decoder will predict
 * the next block from what it reconstructs, so the encoder has to hold the same
 * samples or the two diverge. That is why this dequantizes its own output and
 * runs the decoder's inverse transform rather than adding back the residual it
 * started with.
 */
static void enc_transform_block(
    Encoder* e, uint32_t plane, uint32_t x, uint32_t y, uint32_t tx_size,
    uint32_t mode, uint32_t y_mode, int have_above, int have_left
) {
    uint32_t w = tiny_av1_tx_width[tx_size];
    uint32_t h = tiny_av1_tx_height[tx_size];

    uint32_t above_count = 0;
    uint32_t left_count = 0;

    enc_gather(
        e, plane, x, y, w, h, have_above, have_left, &above_count, &left_count
    );

    uint8_t* dst = e->recon[plane] + (size_t) y * e->stride[plane] + x;

    enc_predict(
        e, w, h, mode, have_above, have_left, above_count, left_count, dst,
        e->stride[plane]
    );

    // the residual against the source, at the plane's own extent; a block that
    // hangs over the edge takes the last real sample, which is what the decoder
    // will predict there anyway
    for (uint32_t i = 0; i < h; i++) {
        uint32_t sy = y + i < e->height[plane] ? y + i : e->height[plane] - 1u;

        for (uint32_t j = 0; j < w; j++) {
            uint32_t sx =
                x + j < e->width[plane] ? x + j : e->width[plane] - 1u;

            int32_t source =
                e->source[plane][(size_t) sy * e->stride[plane] + sx];
            int32_t predicted = dst[(size_t) i * e->stride[plane] + j];

            e->block[i * 32u + j] = source - predicted;
        }
    }

    forward_dct(e->block, e->coeffs, w);

    int32_t dc = tiny_av1_dc_qlookup[0][e->q_index];
    int32_t ac = tiny_av1_ac_qlookup[0][e->q_index];

    int32_t levels[32 * 32];
    uint32_t eob = 0;

    const uint8_t* scan = tiny_av1_default_scan_8x8;

    if (tx_size == TINY_AV1_TX_4X4) scan = tiny_av1_default_scan_4x4;

    for (uint32_t i = 0; i < w * h; i++) levels[i] = 0;

    for (uint32_t c = 0; c < w * h; c++) {
        uint32_t pos = scan[c];
        int32_t quantizer = pos == 0u ? dc : ac;
        int32_t value = e->coeffs[(pos / w) * 32u + (pos % w)];

        /*
         * Rounded toward zero with a dead zone, which is the one quantizer
         * decision here.
         *
         * A plain round-to-nearest spends bits on coefficients whose
         * reconstruction is almost the prediction anyway; biasing toward zero
         * costs a little accuracy and removes whole coefficients, which is
         * where a fast encoder's compression comes from.
         */
        int32_t magnitude = value < 0 ? -value : value;
        int32_t level = (magnitude * 2 + quantizer) / (quantizer * 2);

        if (level > 0 && magnitude * 4 < quantizer * 3) level = 0;

        levels[pos] = value < 0 ? -level : level;

        if (level != 0) eob = c + 1u;
    }

    uint32_t tx_type = TINY_AV1_DCT_DCT;

    tiny_av1_write_coeffs(
        e->residual, &e->symbol, e->cdf, &e->frame, &e->sequence, plane, x, y,
        tx_size, tx_type, y_mode, levels, eob
    );

    // and the reconstruction, from the levels that were actually written
    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            int32_t level = levels[i * w + j];
            int32_t quantizer = (i == 0u && j == 0u) ? dc : ac;

            e->block[i * 32u + j] = level * quantizer;
        }
    }

    if (eob > 0u) {
        tiny_av1_inverse_transform(
            e->block, 32u, (TinyAv1TxSize) tx_size, (TinyAv1TxType) tx_type, 8u
        );

        for (uint32_t i = 0; i < h; i++) {
            uint8_t* row = dst + (size_t) i * e->stride[plane];

            for (uint32_t j = 0; j < w; j++) {
                row[j] =
                    tiny_clamp_u8((int32_t) row[j] + e->block[i * 32u + j]);
            }
        }
    }
}

/** The skip context, which is the decoder's own derivation. */
static uint32_t enc_skip_context(const Encoder* e, uint32_t row, uint32_t col) {
    uint32_t ctx = 0;

    if (enc_inside(e, (int32_t) row - 1, (int32_t) col)) {
        ctx += e->skips[(size_t) (row - 1u) * e->frame.mi_cols + col];
    }
    if (enc_inside(e, (int32_t) row, (int32_t) col - 1)) {
        ctx += e->skips[(size_t) row * e->frame.mi_cols + (col - 1u)];
    }

    return ctx;
}

/** Writes one block's mode info, then its residual. */
static void enc_block(Encoder* e, uint32_t row, uint32_t col) {
    uint32_t x = col * AV1_MI_SIZE;
    uint32_t y = row * AV1_MI_SIZE;

    int have_above = enc_inside(e, (int32_t) row - 1, (int32_t) col);
    int have_left = enc_inside(e, (int32_t) row, (int32_t) col - 1);

    uint32_t above_count = 0;
    uint32_t left_count = 0;

    enc_gather(
        e, 0u, x, y, 8u, 8u, have_above, have_left, &above_count, &left_count
    );

    uint32_t y_mode = enc_choose_mode(
        e, x, y, 8u, 8u, have_above, have_left, above_count, left_count
    );

    // skip, which this encoder never sets: an empty transform is cheaper to say
    // with `all_zero` per plane than to promise for the whole block
    uint32_t ctx = enc_skip_context(e, row, col);
    tiny_av1_symbol_write(&e->symbol, e->cdf->skip[ctx], 2u, 0u);

    uint32_t above_mode = TINY_AV1_DC_PRED;
    uint32_t left_mode = TINY_AV1_DC_PRED;

    if (have_above) {
        above_mode = e->y_modes[(size_t) (row - 1u) * e->frame.mi_cols + col];
    }
    if (have_left) {
        left_mode = e->y_modes[(size_t) row * e->frame.mi_cols + (col - 1u)];
    }

    tiny_av1_symbol_write(
        &e->symbol,
        e->cdf->intra_frame_y_mode[tiny_av1_intra_mode_context[above_mode]]
                                  [tiny_av1_intra_mode_context[left_mode]],
        13u, y_mode
    );

    // a directional mode carries an angle delta, and zero is the middle symbol
    if (y_mode >= TINY_AV1_V_PRED && y_mode <= TINY_AV1_D67_PRED) {
        tiny_av1_symbol_write(
            &e->symbol, e->cdf->angle_delta[y_mode - TINY_AV1_V_PRED], 7u,
            TINY_AV1_MAX_ANGLE_DELTA
        );
    }

    if (e->planes > 1u) {
        // chroma from luma is offered at this block size, so the distribution
        // is the one with fourteen symbols
        tiny_av1_symbol_write(
            &e->symbol, e->cdf->uv_mode_cfl_allowed[y_mode], 14u,
            TINY_AV1_DC_PRED
        );
    }

    uint32_t wide = tiny_av1_num_4x4_blocks_wide[AV1_ENC_BLOCK];
    uint32_t high = tiny_av1_num_4x4_blocks_high[AV1_ENC_BLOCK];

    for (uint32_t i = 0; i < high && row + i < e->frame.mi_rows; i++) {
        for (uint32_t j = 0; j < wide && col + j < e->frame.mi_cols; j++) {
            size_t at = (size_t) (row + i) * e->frame.mi_cols + col + j;

            e->sizes[at] = (uint8_t) AV1_ENC_BLOCK;
            e->y_modes[at] = (uint8_t) y_mode;
            e->skips[at] = 0u;
            e->tx_sizes[at] = (uint8_t) AV1_ENC_LUMA_TX;
        }
    }

    enc_transform_block(
        e, 0u, x, y, AV1_ENC_LUMA_TX, y_mode, y_mode, have_above, have_left
    );

    if (e->planes == 1u) return;

    uint32_t sub_x = e->sequence.sub_x;
    uint32_t sub_y = e->sequence.sub_y;

    uint32_t cx = x >> sub_x;
    uint32_t cy = y >> sub_y;

    for (uint32_t plane = 1; plane < e->planes; plane++) {
        uint32_t tx = sub_x || sub_y ? AV1_ENC_CHROMA_TX : AV1_ENC_LUMA_TX;

        enc_transform_block(
            e, plane, cx, cy, tx, TINY_AV1_DC_PRED, y_mode, have_above,
            have_left
        );
    }
}

/** One interval of a cumulative distribution, which is a difference. */
static uint32_t enc_interval(const uint16_t* cdf, uint32_t index) {
    uint32_t high = cdf[index];
    uint32_t low = index == 0u ? 0u : cdf[index - 1u];

    return high - low;
}

/**
 * @brief Writes the partition tree down to 8x8, one node at a time.
 *
 * The mirror of `read_partition`, including its two collapsed cases: at the
 * frame's right or bottom edge the choice is a single derived bit rather than a
 * symbol, and with neither rows nor columns available nothing is coded at all.
 * Writing a full symbol where the decoder expects the collapsed one is the
 * error this shape exists to prevent.
 */
static void enc_partition(
    Encoder* e, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    if (row >= e->frame.mi_rows || col >= e->frame.mi_cols) return;

    uint32_t half = tiny_av1_num_4x4_blocks_wide[size] >> 1;
    int has_rows = (row + half) < e->frame.mi_rows;
    int has_cols = (col + half) < e->frame.mi_cols;

    if (size == AV1_ENC_BLOCK) {
        uint32_t bsl = tiny_av1_mi_width_log2[size];
        uint32_t ctx = 0;

        if (enc_inside(e, (int32_t) row - 1, (int32_t) col)) {
            uint8_t above =
                e->sizes[(size_t) (row - 1u) * e->frame.mi_cols + col];

            ctx += tiny_av1_mi_width_log2[above] < bsl ? 1u : 0u;
        }
        if (enc_inside(e, (int32_t) row, (int32_t) col - 1)) {
            uint8_t left =
                e->sizes[(size_t) row * e->frame.mi_cols + (col - 1u)];

            ctx += tiny_av1_mi_height_log2[left] < bsl ? 2u : 0u;
        }

        if (has_rows && has_cols) {
            tiny_av1_symbol_write(
                &e->symbol, e->cdf->partition_w8[ctx], 4u,
                TINY_AV1_PARTITION_NONE
            );
        }

        // at an edge the decoder derives the partition, and an 8x8 block that
        // is over one is still coded whole
        enc_block(e, row, col);

        return;
    }

    uint32_t bsl = tiny_av1_mi_width_log2[size];
    uint32_t ctx = 0;

    if (enc_inside(e, (int32_t) row - 1, (int32_t) col)) {
        uint8_t above = e->sizes[(size_t) (row - 1u) * e->frame.mi_cols + col];

        ctx += tiny_av1_mi_width_log2[above] < bsl ? 1u : 0u;
    }
    if (enc_inside(e, (int32_t) row, (int32_t) col - 1)) {
        uint8_t left = e->sizes[(size_t) row * e->frame.mi_cols + (col - 1u)];

        ctx += tiny_av1_mi_height_log2[left] < bsl ? 2u : 0u;
    }

    uint16_t* cdf = e->cdf->partition_w16[ctx];
    uint32_t count = 10u;

    if (bsl == 3u) cdf = e->cdf->partition_w32[ctx];
    if (bsl == 4u) cdf = e->cdf->partition_w64[ctx];

    if (has_rows && has_cols) {
        tiny_av1_symbol_write(&e->symbol, cdf, count, TINY_AV1_PARTITION_SPLIT);
    }
    else if (has_rows || has_cols) {
        /*
         * The collapsed choice, which is one bit against a distribution built
         * from the full one.
         *
         * The reader sums the intervals of every symbol that implies a split
         * and reads a two symbol distribution that it does not adapt; writing
         * the same bit against the same construction is what keeps the two in
         * step. A split is symbol 1 either way round.
         */
        uint32_t psum;

        if (has_cols) {
            psum = enc_interval(cdf, TINY_AV1_PARTITION_VERT) +
                   enc_interval(cdf, TINY_AV1_PARTITION_SPLIT) +
                   enc_interval(cdf, TINY_AV1_PARTITION_HORZ_A) +
                   enc_interval(cdf, TINY_AV1_PARTITION_VERT_A) +
                   enc_interval(cdf, TINY_AV1_PARTITION_VERT_B) +
                   enc_interval(cdf, TINY_AV1_PARTITION_VERT_4);
        }
        else {
            psum = enc_interval(cdf, TINY_AV1_PARTITION_HORZ) +
                   enc_interval(cdf, TINY_AV1_PARTITION_SPLIT) +
                   enc_interval(cdf, TINY_AV1_PARTITION_HORZ_A) +
                   enc_interval(cdf, TINY_AV1_PARTITION_HORZ_B) +
                   enc_interval(cdf, TINY_AV1_PARTITION_VERT_A) +
                   enc_interval(cdf, TINY_AV1_PARTITION_HORZ_4);
        }

        uint16_t collapsed[3] = {(uint16_t) ((1u << 15) - psum), 1u << 15, 0};

        tiny_av1_symbol_write_frozen(&e->symbol, collapsed, 2u, 1u);
    }

    TinyAv1BlockSize sub = (TinyAv1BlockSize)
        tiny_av1_partition_subsize[TINY_AV1_PARTITION_SPLIT][size];

    enc_partition(e, row, col, sub);
    enc_partition(e, row, col + half, sub);
    enc_partition(e, row + half, col, sub);
    enc_partition(e, row + half, col + half, sub);
}

// #region entry point

/** Frees what the encode allocated. */
static void encoder_release(Encoder* e) {
    if (!e) return;

    for (uint32_t i = 0; i < 3u; i++) {
        tiny_free(e->source[i]);
        tiny_free(e->recon[i]);
    }

    tiny_free(e->sizes);
    tiny_free(e->y_modes);
    tiny_free(e->skips);
    tiny_free(e->tx_sizes);
    tiny_free(e->cdf);

    if (e->residual) {
        tiny_free(e->residual->above_level);
        tiny_free(e->residual->above_dc);
        tiny_free(e->residual->left_level);
        tiny_free(e->residual->left_dc);
        tiny_free(e->residual);
    }

    tiny_free(e);
}

/**
 * @brief Turns the caller's pixels into planes, at full range BT.601.
 *
 * Full range because the encoder writes `color_range` 1, which is the honest
 * choice for an image that arrived as RGB: expanding to studio range and back
 * loses levels at both ends for nothing. The chroma is box averaged when it is
 * subsampled, which is the same reduction every other codec here uses.
 */
static void encoder_planes(Encoder* e, const TinyImage* image) {
    uint32_t sub_x = e->sequence.sub_x;
    uint32_t sub_y = e->sequence.sub_y;
    uint32_t channels = image->channels;

    for (uint32_t y = 0; y < e->height[0]; y++) {
        uint32_t sy = y < image->height ? y : image->height - 1u;

        for (uint32_t x = 0; x < e->width[0]; x++) {
            uint32_t sx = x < image->width ? x : image->width - 1u;
            const uint8_t* pixel =
                image->data + ((size_t) sy * image->width + sx) * channels;

            int32_t r = pixel[0];
            int32_t g = channels > 2u ? pixel[1] : pixel[0];
            int32_t b = channels > 2u ? pixel[2] : pixel[0];

            // BT.601 at full range, in sixteen bit fixed point
            int32_t luma = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;

            e->source[0][(size_t) y * e->stride[0] + x] = tiny_clamp_u8(luma);
        }
    }

    if (e->planes == 1u) return;

    for (uint32_t y = 0; y < e->height[1]; y++) {
        for (uint32_t x = 0; x < e->width[1]; x++) {
            int32_t sum_u = 0;
            int32_t sum_v = 0;
            uint32_t count = 0;

            for (uint32_t dy = 0; dy <= sub_y; dy++) {
                uint32_t sy = (y << sub_y) + dy;

                if (sy >= image->height) sy = image->height - 1u;

                for (uint32_t dx = 0; dx <= sub_x; dx++) {
                    uint32_t sx = (x << sub_x) + dx;

                    if (sx >= image->width) sx = image->width - 1u;

                    const uint8_t* pixel =
                        image->data +
                        ((size_t) sy * image->width + sx) * channels;

                    int32_t r = pixel[0];
                    int32_t g = channels > 2u ? pixel[1] : pixel[0];
                    int32_t b = channels > 2u ? pixel[2] : pixel[0];

                    int32_t luma =
                        (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;

                    // 1/1.772 and 1/1.402 at sixteen bits, which are the
                    // inverses of the two the decoder multiplies by; the
                    // studio-range pair would compress every colour by a
                    // thirteenth and cap the colour PSNR at 31.5 dB
                    sum_u += 128 + (((b - luma) * 36985 + 32768) >> 16);
                    sum_v += 128 + (((r - luma) * 46748 + 32768) >> 16);
                    count++;
                }
            }

            e->source[1][(size_t) y * e->stride[1] + x] =
                tiny_clamp_u8((sum_u + (int32_t) count / 2) / (int32_t) count);
            e->source[2][(size_t) y * e->stride[2] + x] =
                tiny_clamp_u8((sum_v + (int32_t) count / 2) / (int32_t) count);
        }
    }
}

/**
 * @brief The quantizer index for a quality, which is the one quality knob.
 *
 * Quality 100 is index zero, which is lossless in the quantizer's own terms;
 * quality 1 is index 255. The curve between them is the linear one, because a
 * shaped curve would be a claim about perceptual quality that nothing here has
 * measured.
 */
static uint32_t encoder_qindex(uint32_t quality) {
    if (quality == 0u) quality = 75u;
    if (quality > 100u) quality = 100u;

    return (100u - quality) * 255u / 100u;
}

int tiny_av1_encode(
    const TinyImage* image, uint32_t quality, uint8_t* out, size_t capacity,
    size_t* size, TinyAv1Sequence* described
) {
    if (!image || !image->data || !out || !size) return TINYIMG_ERR_NULL;
    if (image->width == 0u || image->height == 0u) return TINYIMG_ERR_RANGE;

    Encoder* e = (Encoder*) tiny_alloc(sizeof(Encoder));
    if (!e) return TINYIMG_ERR_MEMORY;

    tiny_memset(e, 0, sizeof(*e));

    e->planes = image->channels < 3u ? 1u : 3u;
    e->sequence.planes = (uint8_t) e->planes;
    e->sequence.monochrome = e->planes == 1u ? 1u : 0u;
    e->sequence.bit_depth = 8u;
    e->sequence.reduced = 1u;
    e->sequence.still_picture = 1u;
    e->sequence.profile = 0u;
    e->sequence.color_range = 1u;
    e->sequence.matrix_coefficients = e->planes == 1u ? 0u : 6u;
    e->sequence.color_primaries = 1u;
    e->sequence.transfer_characteristics = 13u;
    e->sequence.sub_x = 1u;
    e->sequence.sub_y = 1u;
    e->sequence.max_width = image->width;
    e->sequence.max_height = image->height;

    e->frame.width = image->width;
    e->frame.height = image->height;
    e->frame.upscaled_width = image->width;
    e->frame.mi_cols = 2u * ((image->width + 7u) >> 3);
    e->frame.mi_rows = 2u * ((image->height + 7u) >> 3);
    e->frame.tile_cols = 1u;
    e->frame.tile_rows = 1u;
    e->frame.mi_col_starts[1] = e->frame.mi_cols;
    e->frame.mi_row_starts[1] = e->frame.mi_rows;
    e->frame.tile_size_bytes = 1u;
    e->frame.tx_mode = 1u;
    e->frame.reduced_tx_set = 1u;
    e->frame.base_q_idx = (uint8_t) encoder_qindex(quality);
    e->q_index = e->frame.base_q_idx;

    for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
        e->frame.seg_qindex[i] = e->frame.base_q_idx;
    }

    e->frame.coded_lossless = (uint8_t) (e->frame.base_q_idx == 0u ? 1u : 0u);

    uint32_t cells = e->frame.mi_cols * e->frame.mi_rows;

    for (uint32_t plane = 0; plane < e->planes; plane++) {
        uint32_t sub_x = plane > 0u ? e->sequence.sub_x : 0u;
        uint32_t sub_y = plane > 0u ? e->sequence.sub_y : 0u;

        // superblock aligned, for the same reason the decoder's store is: a
        // block at the edge is coded whole and its prediction writes it whole
        uint32_t aligned_cols = (e->frame.mi_cols + 15u) / 16u * 16u;
        uint32_t aligned_rows = (e->frame.mi_rows + 15u) / 16u * 16u;

        uint32_t store_w = (aligned_cols * AV1_MI_SIZE) >> sub_x;
        uint32_t store_h = (aligned_rows * AV1_MI_SIZE) >> sub_y;

        e->stride[plane] = store_w;
        e->width[plane] = (e->frame.mi_cols * AV1_MI_SIZE) >> sub_x;
        e->height[plane] = (e->frame.mi_rows * AV1_MI_SIZE) >> sub_y;

        e->source[plane] = (uint8_t*) tiny_alloc((size_t) store_w * store_h);
        e->recon[plane] = (uint8_t*) tiny_alloc((size_t) store_w * store_h);

        if (!e->source[plane] || !e->recon[plane]) {
            encoder_release(e);
            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(e->source[plane], 128, (size_t) store_w * store_h);
        tiny_memset(e->recon[plane], 128, (size_t) store_w * store_h);
    }

    e->sizes = (uint8_t*) tiny_alloc(cells);
    e->y_modes = (uint8_t*) tiny_alloc(cells);
    e->skips = (uint8_t*) tiny_alloc(cells);
    e->tx_sizes = (uint8_t*) tiny_alloc(cells);
    e->cdf = (TinyAv1Cdf*) tiny_alloc(sizeof(TinyAv1Cdf));
    e->residual = (TinyAv1Residual*) tiny_alloc(sizeof(TinyAv1Residual));

    if (!e->sizes || !e->y_modes || !e->skips || !e->tx_sizes || !e->cdf ||
        !e->residual) {
        encoder_release(e);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(e->sizes, 0, cells);
    tiny_memset(e->y_modes, 0, cells);
    tiny_memset(e->skips, 0, cells);
    tiny_memset(e->tx_sizes, 0, cells);
    tiny_memset(e->residual, 0, sizeof(*e->residual));

    uint32_t above_stride = e->frame.mi_cols + TINY_AV1_CONTEXT_PAD;
    uint32_t left_stride = e->frame.mi_rows + TINY_AV1_CONTEXT_PAD;

    e->residual->above_level =
        (uint8_t*) tiny_alloc((size_t) above_stride * 3u);
    e->residual->above_dc = (uint8_t*) tiny_alloc((size_t) above_stride * 3u);
    e->residual->left_level = (uint8_t*) tiny_alloc((size_t) left_stride * 3u);
    e->residual->left_dc = (uint8_t*) tiny_alloc((size_t) left_stride * 3u);
    e->residual->above_stride = above_stride;
    e->residual->left_stride = left_stride;

    if (!e->residual->above_level || !e->residual->above_dc ||
        !e->residual->left_level || !e->residual->left_dc) {
        encoder_release(e);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(e->residual->above_level, 0, (size_t) above_stride * 3u);
    tiny_memset(e->residual->above_dc, 0, (size_t) above_stride * 3u);
    tiny_memset(e->residual->left_level, 0, (size_t) left_stride * 3u);
    tiny_memset(e->residual->left_dc, 0, (size_t) left_stride * 3u);

    encoder_planes(e, image);
    tiny_av1_cdf_init(e->cdf);

    /*
     * The tile, encoded into its own buffer first.
     *
     * Its length is not known until it is finished and the tile group's header
     * needs it, so the alternative is writing the OBU twice. The pending buffer
     * the symbol writer needs is the same size, since it holds one entry per
     * output byte.
     */
    size_t tile_capacity = capacity;
    uint8_t* tile = (uint8_t*) tiny_alloc(tile_capacity);
    uint16_t* pending =
        (uint16_t*) tiny_alloc(tile_capacity * sizeof(uint16_t));

    if (!tile || !pending) {
        tiny_free(tile);
        tiny_free(pending);
        encoder_release(e);

        return TINYIMG_ERR_MEMORY;
    }

    tiny_av1_symbol_enc_init(&e->symbol, pending, tile_capacity);

    for (uint32_t row = 0; row < e->frame.mi_rows; row += 16u) {
        // the left level context is per superblock row, which is the one piece
        // of per-row state the encoder has to reset as the decoder does
        tiny_memset(e->residual->left_level, 0, (size_t) left_stride * 3u);
        tiny_memset(e->residual->left_dc, 0, (size_t) left_stride * 3u);

        for (uint32_t col = 0; col < e->frame.mi_cols; col += 16u) {
            enc_partition(e, row, col, TINY_AV1_BLOCK_64X64);
        }
    }

    size_t tile_size = 0;
    int status =
        tiny_av1_symbol_finish(&e->symbol, tile, tile_capacity, &tile_size);

    tiny_free(pending);

    if (status != TINYIMG_OK) {
        tiny_free(tile);
        encoder_release(e);

        return status;
    }

    /*
     * The three OBUs, each with a one byte header and a leb128 size.
     *
     * A temporal delimiter is not written: an AVIF item is one still frame and
     * the container says so, so the delimiter would be a byte of nothing. The
     * frame header and the tile group are written separately rather than as one
     * frame OBU, because the reader accepts both and two are simpler to size.
     */
    uint8_t header[64];
    Av1Write w;

    size_t at = 0;

    write_init(&w, header, sizeof(header));
    write_sequence(
        &w, image->width, image->height, e->planes, e->sequence.sub_x,
        e->sequence.sub_y
    );
    write_trailing(&w);

    if (w.overflow) {
        tiny_free(tile);
        encoder_release(e);

        return TINYIMG_ERR_BUFFER_TOO_SMALL;
    }

    size_t sequence_bytes = write_bytes(&w);

    if (at + 1u >= capacity) goto too_small;

    // type 1 in bits 3 to 6, and has_size_field set
    out[at++] = 0x0Au;

    size_t written = write_leb128(out + at, capacity - at, sequence_bytes);
    if (written == 0u) goto too_small;
    at += written;

    if (at + sequence_bytes > capacity) goto too_small;
    tiny_memcpy(out + at, header, sequence_bytes);
    at += sequence_bytes;

    /*
     * The two maxima the tile info's increment bits depend on, derived the way
     * the reader derives them.
     *
     * A picture wider than 4,096 samples or larger than the tile area cap
     * cannot be one tile at all, and this encoder writes one; refusing is
     * honest, and the container reports a specific error rather than producing
     * a file no decoder would read.
     */
    uint32_t sb_cols = (e->frame.mi_cols + 15u) >> 4;
    uint32_t sb_rows = (e->frame.mi_rows + 15u) >> 4;

    uint32_t max_cols_log2 = 0;
    uint32_t max_rows_log2 = 0;

    while ((1u << max_cols_log2) < sb_cols) max_cols_log2++;
    while ((1u << max_rows_log2) < sb_rows) max_rows_log2++;

    if (sb_cols > 64u || sb_rows > 64u) {
        tiny_free(tile);
        encoder_release(e);

        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    write_init(&w, header, sizeof(header));
    write_frame(
        &w, e->frame.base_q_idx, e->planes, max_cols_log2, max_rows_log2
    );
    write_trailing(&w);

    if (w.overflow) {
        tiny_free(tile);
        encoder_release(e);

        return TINYIMG_ERR_BUFFER_TOO_SMALL;
    }

    size_t frame_bytes = write_bytes(&w);

    if (at + 1u >= capacity) goto too_small;

    // type 3, the frame header
    out[at++] = 0x1Au;

    written = write_leb128(out + at, capacity - at, frame_bytes);
    if (written == 0u) goto too_small;
    at += written;

    if (at + frame_bytes > capacity) goto too_small;
    tiny_memcpy(out + at, header, frame_bytes);
    at += frame_bytes;

    if (at + 1u >= capacity) goto too_small;

    // type 4, the tile group, whose payload for one tile is the tile itself
    out[at++] = 0x22u;

    written = write_leb128(out + at, capacity - at, tile_size);
    if (written == 0u) goto too_small;
    at += written;

    if (at + tile_size > capacity) goto too_small;
    tiny_memcpy(out + at, tile, tile_size);
    at += tile_size;

    if (described) *described = e->sequence;

    *size = at;

    tiny_free(tile);
    encoder_release(e);

    return TINYIMG_OK;

too_small:
    tiny_free(tile);
    encoder_release(e);

    return TINYIMG_ERR_BUFFER_TOO_SMALL;
}
