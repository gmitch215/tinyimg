#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"

/**
 * @file
 * @brief Specification 5.11.5 through 5.11.16, the mode info of an intra block.
 *
 * The syntax elements between the partition tree and the coefficients. Every
 * one of them is a symbol read against a distribution the specification selects
 * from the neighbors above and to the left, so most of this file is those
 * selections rather than the reads.
 *
 * **The order is the whole contract.** Each element consumes symbols from a
 * shared arithmetic decoder, so a field read in the wrong place, or skipped
 * where the specification codes it, shifts every block after it. `SegIdPreSkip`
 * is the sharpest edge: it swaps the segment id and the skip flag, and a frame
 * that sets it decodes as noise from the first block if it is ignored.
 *
 * Written for a still intra frame, which is what AVIF carries. `is_inter` is
 * zero everywhere, so the transform size is read once per block rather than as
 * a tree, and the reference frame, motion vector and compound syntax are not
 * reachable at all.
 */

// #region tables

/** Samples wide a block is, which the specification tabulates as Block_Width.
 */
static uint32_t block_width(TinyAv1BlockSize size) {
    return 4u * tiny_av1_num_4x4_blocks_wide[size];
}

static uint32_t block_height(TinyAv1BlockSize size) {
    return 4u * tiny_av1_num_4x4_blocks_high[size];
}

/** Whether a mode predicts along an angle, and so carries an angle delta. */
static int is_directional(uint32_t mode) {
    return mode >= TINY_AV1_V_PRED && mode <= TINY_AV1_D67_PRED;
}

// #endregion

// #region context

/** Whether an mi position is inside the tile being walked. */
static int available(const TinyAv1Walk* walk, int32_t row, int32_t col) {
    return row >= (int32_t) walk->row_start && row < (int32_t) walk->row_end &&
           col >= (int32_t) walk->col_start && col < (int32_t) walk->col_end;
}

/** One entry of a frame-sized mi array. */
static uint8_t at(
    const TinyAv1Walk* walk, const uint8_t* plane, uint32_t row, uint32_t col
) {
    return plane[(size_t) row * walk->frame->mi_cols + col];
}

/**
 * @brief Whether the block has chroma of its own.
 *
 * A 4xN block on a subsampled plane shares its chroma with the block above or
 * beside it, and only the second of the pair codes it. The condition is on the
 * block's size in mi units and on the position's parity, which is why it cannot
 * be answered from the size alone.
 */
static int has_chroma(
    const TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[size];

    if (high == 1u && walk->sequence->sub_y && (row & 1u) == 0u) {
        return 0;
    }
    if (wide == 1u && walk->sequence->sub_x && (col & 1u) == 0u) {
        return 0;
    }

    return walk->sequence->planes > 1u;
}

/** The skip context, which counts how many neighbors skipped. */
static uint32_t skip_context(
    const TinyAv1Walk* walk, uint32_t row, uint32_t col
) {
    uint32_t ctx = 0;

    if (available(walk, (int32_t) row - 1, (int32_t) col)) {
        ctx += at(walk, walk->skips, row - 1u, col);
    }
    if (available(walk, (int32_t) row, (int32_t) col - 1)) {
        ctx += at(walk, walk->skips, row, col - 1u);
    }

    return ctx;
}

/**
 * @brief The transform depth context.
 *
 * The above and left widths come from the neighbors' transform sizes, and the
 * `IsInters` branch of 9.3 is unreachable here because an intra frame has no
 * inter blocks. The `row == MiRow` guard is what makes an unavailable neighbor
 * read as 64 rather than as zero.
 */
static uint32_t tx_depth_context(
    const TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    uint32_t max_rect = tiny_av1_max_tx_size_rect[size];
    uint32_t max_width = tiny_av1_tx_width[max_rect];
    uint32_t max_height = tiny_av1_tx_height[max_rect];

    uint32_t above = 0;
    uint32_t left = 0;

    if (available(walk, (int32_t) row - 1, (int32_t) col)) {
        above = tiny_av1_tx_width[at(walk, walk->tx_sizes, row - 1u, col)];
    }
    if (available(walk, (int32_t) row, (int32_t) col - 1)) {
        left = tiny_av1_tx_height[at(walk, walk->tx_sizes, row, col - 1u)];
    }

    return (above >= max_width ? 1u : 0u) + (left >= max_height ? 1u : 0u);
}

/**
 * @brief Reads a segment id, which is coded as a difference from a prediction.
 *
 * The prediction is the specification's three-neighbor rule and the coded value
 * is interleaved around it, so a segmented frame spends few bits where the map
 * is smooth. `neg_deinterleave` is the specification's own function.
 */
uint32_t tiny_av1_neg_deinterleave(uint32_t diff, uint32_t ref, uint32_t max) {
    if (ref == 0u) return diff;

    if (ref >= max - 1u) return max - diff - 1u;

    if (2u * ref < max) {
        if (diff <= 2u * ref) {
            if (diff & 1u) return ref + ((diff + 1u) >> 1);
            return ref - (diff >> 1);
        }
        return diff;
    }

    if (diff <= 2u * (max - ref - 1u)) {
        if (diff & 1u) return ref + ((diff + 1u) >> 1);
        return ref - (diff >> 1);
    }

    return max - (diff + 1u);
}

static uint32_t read_segment_id(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, int skip
) {
    int up = available(walk, (int32_t) row - 1, (int32_t) col);
    int left = available(walk, (int32_t) row, (int32_t) col - 1);

    int32_t prev_ul = -1;
    int32_t prev_u = -1;
    int32_t prev_l = -1;

    if (up && left) {
        prev_ul = (int32_t) at(walk, walk->segment_ids, row - 1u, col - 1u);
    }
    if (up) prev_u = (int32_t) at(walk, walk->segment_ids, row - 1u, col);
    if (left) prev_l = (int32_t) at(walk, walk->segment_ids, row, col - 1u);

    int32_t pred;

    if (prev_u == -1) {
        pred = prev_l == -1 ? 0 : prev_l;
    }
    else if (prev_l == -1) {
        pred = prev_u;
    }
    else {
        pred = prev_ul == prev_u ? prev_u : prev_l;
    }

    if (skip) return (uint32_t) pred;

    // the context is the same three neighbors, bucketed by how far apart they
    // are, which is 9.3's own arithmetic
    uint32_t ctx;

    if (prev_ul < 0) {
        ctx = 0u;
    }
    else if (prev_ul == prev_u && prev_ul == prev_l) {
        ctx = 2u;
    }
    else if (prev_ul == prev_u || prev_ul == prev_l || prev_u == prev_l) {
        ctx = 1u;
    }
    else {
        ctx = 0u;
    }

    uint32_t coded =
        tiny_av1_symbol_read(walk->symbol, walk->cdf->segment_id[ctx], 8u);

    return tiny_av1_neg_deinterleave(
        coded, (uint32_t) pred, (uint32_t) walk->frame->last_active_seg_id + 1u
    );
}

// #endregion

// #region deltas

/** The shared shape of `delta_q_abs` and `delta_lf_abs`, which code the same.
 */
static int32_t read_delta_magnitude(
    TinyAv1Walk* walk, uint16_t* cdf, uint32_t symbols
) {
    uint32_t abs = tiny_av1_symbol_read(walk->symbol, cdf, symbols);

    // DELTA_Q_SMALL and DELTA_LF_SMALL are both 3, and the escape is a length
    // followed by that many bits
    if (abs == TINY_AV1_DELTA_SMALL) {
        uint32_t bits = tiny_av1_symbol_literal(walk->symbol, 3u) + 1u;
        abs = tiny_av1_symbol_literal(walk->symbol, bits) + (1u << bits) + 1u;
    }

    if (abs == 0u) return 0;

    return tiny_av1_symbol_bit(walk->symbol) ? -(int32_t) abs : (int32_t) abs;
}

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/**
 * @brief The per-block quantizer and loop filter deltas.
 *
 * Both are skipped for a whole superblock that codes no residual, which is the
 * one place the block's size decides whether a syntax element exists at all.
 */
static void read_deltas(TinyAv1Walk* walk, TinyAv1BlockSize size, int skip) {
    TinyAv1BlockSize sb = walk->sequence->use_128x128_superblock
                              ? TINY_AV1_BLOCK_128X128
                              : TINY_AV1_BLOCK_64X64;

    if (size == sb && skip) return;

    if (!walk->read_deltas) return;

    if (walk->frame->delta_q_present) {
        int32_t delta = read_delta_magnitude(walk, walk->cdf->delta_q, 4u);

        if (delta != 0) {
            // multiplied rather than shifted: the delta is signed and a left
            // shift of a negative value is undefined
            int32_t scale = (int32_t) (1u << walk->frame->delta_q_res);

            walk->q_index = (uint8_t) clamp_i32(
                (int32_t) walk->q_index + delta * scale, 1, 255
            );
        }
    }

    if (!walk->frame->delta_lf_present) return;

    uint32_t count = 1u;

    if (walk->frame->delta_lf_multi) {
        count = walk->sequence->planes > 1u ? TINY_AV1_FRAME_LF_COUNT
                                            : TINY_AV1_FRAME_LF_COUNT - 2u;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint16_t* cdf = walk->frame->delta_lf_multi
                            ? walk->cdf->delta_lf_multi[i]
                            : walk->cdf->delta_lf;

        int32_t delta = read_delta_magnitude(walk, cdf, 4u);

        if (delta == 0) continue;

        int32_t scale = (int32_t) (1u << walk->frame->delta_lf_res);

        walk->delta_lf[i] = (int8_t) clamp_i32(
            (int32_t) walk->delta_lf[i] + delta * scale,
            -TINY_AV1_MAX_LOOP_FILTER, TINY_AV1_MAX_LOOP_FILTER
        );
    }
}

/**
 * @brief The CDEF strength index, one per 64x64 and coded on its first block.
 *
 * Not a symbol: `cdef_idx` is a literal of `cdef_bits`, read straight from the
 * arithmetic decoder without a distribution. The `-1` sentinel is the
 * specification's own way of saying a square has not been read yet.
 */
static void read_cdef(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    int skip
) {
    walk->mode.cdef_idx = -1;

    if (!walk->cdef_idx) return;
    if (skip || walk->frame->coded_lossless || !walk->sequence->enable_cdef ||
        walk->frame->allow_intrabc) {
        return;
    }

    uint32_t step = tiny_av1_num_4x4_blocks_wide[TINY_AV1_BLOCK_64X64];
    uint32_t base_row = row & ~(step - 1u);
    uint32_t base_col = col & ~(step - 1u);
    size_t base =
        (size_t) (base_row / step) * walk->cdef_stride + (base_col / step);

    if (walk->cdef_idx[base] == -1) {
        int8_t value = (int8_t) tiny_av1_symbol_literal(
            walk->symbol, walk->frame->cdef_bits
        );

        uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
        uint32_t high = tiny_av1_num_4x4_blocks_high[size];

        for (uint32_t y = base_row; y < base_row + high; y += step) {
            for (uint32_t x = base_col; x < base_col + wide; x += step) {
                if (y >= walk->frame->mi_rows || x >= walk->frame->mi_cols) {
                    continue;
                }

                walk->cdef_idx
                    [(size_t) (y / step) * walk->cdef_stride + (x / step)] =
                    value;
            }
        }
    }

    walk->mode.cdef_idx = walk->cdef_idx[base];
}

// #endregion

// #region modes

/** The luma mode, against a distribution chosen by both neighbors' modes. */
static uint32_t read_y_mode(TinyAv1Walk* walk, uint32_t row, uint32_t col) {
    uint32_t above = TINY_AV1_DC_PRED;
    uint32_t left = TINY_AV1_DC_PRED;

    if (available(walk, (int32_t) row - 1, (int32_t) col)) {
        above = at(walk, walk->y_modes, row - 1u, col);
    }
    if (available(walk, (int32_t) row, (int32_t) col - 1)) {
        left = at(walk, walk->y_modes, row, col - 1u);
    }

    return tiny_av1_symbol_read(
        walk->symbol,
        walk->cdf->intra_frame_y_mode[tiny_av1_intra_mode_context[above]]
                                     [tiny_av1_intra_mode_context[left]],
        13u
    );
}

/**
 * @brief The chroma mode, whose distribution says whether CFL is even offered.
 *
 * Two distributions with different symbol counts, and which one applies decides
 * how many symbols the read consumes. Reading the 14-symbol form where the
 * 13-symbol one applies takes a different number of bits and desynchronises the
 * decoder rather than only choosing a wrong mode.
 */
static uint32_t read_uv_mode(
    TinyAv1Walk* walk, TinyAv1BlockSize size, int lossless
) {
    uint32_t y_mode = walk->mode.y_mode;

    int cfl_allowed;

    if (lossless) {
        // the chroma residual of a lossless block is 4x4, which is the one
        // lossless case CFL is offered for
        cfl_allowed = block_width(size) == 4u && block_height(size) == 4u;
    }
    else {
        uint32_t wide = block_width(size);
        uint32_t high = block_height(size);
        uint32_t largest = wide > high ? wide : high;

        cfl_allowed = largest <= 32u;
    }

    if (cfl_allowed) {
        return tiny_av1_symbol_read(
            walk->symbol, walk->cdf->uv_mode_cfl_allowed[y_mode], 14u
        );
    }

    return tiny_av1_symbol_read(
        walk->symbol, walk->cdf->uv_mode_cfl_not_allowed[y_mode], 13u
    );
}

/** The chroma-from-luma scale factors, which code their signs jointly. */
static void read_cfl_alphas(TinyAv1Walk* walk) {
    uint32_t signs =
        tiny_av1_symbol_read(walk->symbol, walk->cdf->cfl_sign, 8u);

    uint32_t sign_u = (signs + 1u) / 3u;
    uint32_t sign_v = (signs + 1u) % 3u;

    walk->mode.cfl_alpha_u = 0;
    walk->mode.cfl_alpha_v = 0;

    if (sign_u != TINY_AV1_CFL_SIGN_ZERO) {
        // the specification gives this as a table of eight and as this
        // arithmetic; the arithmetic has no unreachable entries to get wrong
        uint32_t ctx = (sign_u - 1u) * 3u + sign_v;
        int32_t alpha = 1 + (int32_t) tiny_av1_symbol_read(
                                walk->symbol, walk->cdf->cfl_alpha[ctx], 16u
                            );

        walk->mode.cfl_alpha_u =
            (int8_t) (sign_u == TINY_AV1_CFL_SIGN_NEG ? -alpha : alpha);
    }

    if (sign_v != TINY_AV1_CFL_SIGN_ZERO) {
        uint32_t ctx = (sign_v - 1u) * 3u + sign_u;
        int32_t alpha = 1 + (int32_t) tiny_av1_symbol_read(
                                walk->symbol, walk->cdf->cfl_alpha[ctx], 16u
                            );

        walk->mode.cfl_alpha_v =
            (int8_t) (sign_v == TINY_AV1_CFL_SIGN_NEG ? -alpha : alpha);
    }
}

/** An angle offset, present only for a directional mode at 8x8 and above. */
static int8_t read_angle_delta(
    TinyAv1Walk* walk, TinyAv1BlockSize size, uint32_t mode
) {
    if (size < TINY_AV1_BLOCK_8X8) return 0;
    if (!is_directional(mode)) return 0;

    uint32_t coded = tiny_av1_symbol_read(
        walk->symbol, walk->cdf->angle_delta[mode - TINY_AV1_V_PRED], 7u
    );

    return (int8_t) ((int32_t) coded - TINY_AV1_MAX_ANGLE_DELTA);
}

/** The recursive filter predictor, offered for DC blocks up to 32 across. */
static void read_filter_intra(TinyAv1Walk* walk, TinyAv1BlockSize size) {
    walk->mode.use_filter_intra = 0u;
    walk->mode.filter_intra_mode = 0u;

    if (!walk->sequence->enable_filter_intra) return;
    if (walk->mode.y_mode != TINY_AV1_DC_PRED) return;

    uint32_t wide = block_width(size);
    uint32_t high = block_height(size);

    if ((wide > high ? wide : high) > 32u) return;

    if (!tiny_av1_symbol_read(
            walk->symbol, walk->cdf->filter_intra[size], 2u
        )) {
        return;
    }

    walk->mode.use_filter_intra = 1u;
    walk->mode.filter_intra_mode = (uint8_t) tiny_av1_symbol_read(
        walk->symbol, walk->cdf->filter_intra_mode, 5u
    );
}

/**
 * @brief The transform size, read as a depth below the block's largest.
 *
 * An intra block never splits its transform into a tree, so 5.11.16's inter
 * branch is unreachable and this is one symbol. Which distribution it reads
 * against depends on the block's maximum depth, and each has a different symbol
 * count.
 */
static uint8_t read_tx_size(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    int lossless
) {
    if (lossless) return TINY_AV1_TX_4X4;

    uint8_t tx_size = tiny_av1_max_tx_size_rect[size];
    uint32_t max_depth = tiny_av1_max_tx_depth[size];

    if (size == TINY_AV1_BLOCK_4X4 ||
        walk->frame->tx_mode != TINY_AV1_TX_MODE_SELECT) {
        return tx_size;
    }

    uint32_t ctx = tx_depth_context(walk, row, col, size);
    uint32_t depth;

    if (max_depth == 4u) {
        depth =
            tiny_av1_symbol_read(walk->symbol, walk->cdf->tx_64x64[ctx], 3u);
    }
    else if (max_depth == 3u) {
        depth =
            tiny_av1_symbol_read(walk->symbol, walk->cdf->tx_32x32[ctx], 3u);
    }
    else if (max_depth == 2u) {
        depth =
            tiny_av1_symbol_read(walk->symbol, walk->cdf->tx_16x16[ctx], 3u);
    }
    else {
        depth = tiny_av1_symbol_read(walk->symbol, walk->cdf->tx_8x8[ctx], 2u);
    }

    for (uint32_t i = 0; i < depth; i++) {
        tx_size = tiny_av1_split_tx_size[tx_size];
    }

    return tx_size;
}

/** Whether the frame codes a palette for this block, which is refused. */
static int reads_palette(const TinyAv1Walk* walk, TinyAv1BlockSize size) {
    if (!walk->frame->allow_screen_content_tools) return 0;

    return size >= TINY_AV1_BLOCK_8X8 && block_width(size) <= 64u &&
           block_height(size) <= 64u;
}

// #endregion

// #region entry points

int tiny_av1_frame_supported(
    const TinyAv1Sequence* sequence, const TinyAv1Frame* frame
) {
    if (!sequence || !frame) return TINYIMG_ERR_NULL;

    if (frame->allow_intrabc) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (frame->allow_screen_content_tools) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    /*
     * Loop restoration codes symbols per superblock, before the partition tree.
     *
     * Refused rather than ignored: the filter itself belongs behind the effort
     * tier and could be skipped, but its **symbols** cannot, and a tile read
     * without them desynchronises. Every file measured has it off, which is why
     * this is a refusal rather than a reader.
     */
    if (frame->uses_lr) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    /*
     * 4:2:2, which is horizontal subsampling without vertical.
     *
     * Refused because the specification's own `Subsampled_Size` has no answer
     * for it: every tall block, 4x8 upward, maps to BLOCK_INVALID on a 4:2:2
     * chroma plane, and the residual syntax reads that entry directly. Getting
     * the pair rule that resolves it wrong would read a chroma residual of the
     * wrong size, which desynchronises rather than fails. Profile 2 is the only
     * one that can code it and no encoder in the fixture set does.
     */
    if (sequence->sub_x && !sequence->sub_y) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    /*
     * Ten and twelve bit samples.
     *
     * Refused because the reconstruction is eight bits a sample end to end: the
     * frame buffer, the intra prediction and the RGBA conversion all hold a
     * sample in a byte. A deeper AVIF needs sixteen bit buffers through every
     * one of those, which is a change to the decoder's shape rather than a
     * branch, and truncating instead would return a picture that is plausible
     * and wrong.
     */
    if (sequence->bit_depth != 8u) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    return TINYIMG_OK;
}

int tiny_av1_read_mode(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    if (!walk) return TINYIMG_ERR_NULL;
    if (!walk->y_modes || !walk->skips || !walk->segment_ids ||
        !walk->tx_sizes) {
        return TINYIMG_ERR_NULL;
    }

    // a palette block would code a color index map this decoder does not read,
    // and the frame flag is refused before the walk starts; this is the guard
    // that says so if one is reached anyway
    if (reads_palette(walk, size)) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    TinyAv1Mode* mode = &walk->mode;
    tiny_memset(mode, 0, sizeof(*mode));

    mode->row = row;
    mode->col = col;
    mode->size = size;
    mode->has_chroma = (uint8_t) has_chroma(walk, row, col, size);

    /*
     * The order below is 5.11.6's, and it is the reason this function exists
     * rather than the reads being inlined at the call site.
     *
     * `SegIdPreSkip` puts the segment id either side of the skip flag, and the
     * skip flag is itself what decides whether the segment id is coded at all
     * in the other order. Both are read from the same arithmetic decoder, so
     * swapping them is not a wrong value; it is a different bit count and every
     * block after it decodes as noise.
     */
    uint32_t segment_id = 0u;

    if (walk->frame->seg_id_pre_skip && walk->frame->segmentation_enabled) {
        segment_id = read_segment_id(walk, row, col, 0);
    }

    int skip;

    if (walk->frame->seg_id_pre_skip && walk->frame->seg_skip[segment_id]) {
        skip = 1;
    }
    else {
        uint32_t ctx = skip_context(walk, row, col);
        skip =
            (int) tiny_av1_symbol_read(walk->symbol, walk->cdf->skip[ctx], 2u);
    }

    if (!walk->frame->seg_id_pre_skip && walk->frame->segmentation_enabled) {
        segment_id = read_segment_id(walk, row, col, skip);
    }

    mode->skip = (uint8_t) skip;
    mode->segment_id = (uint8_t) segment_id;
    mode->lossless = walk->frame->lossless_array[segment_id];

    read_cdef(walk, row, col, size, skip);
    read_deltas(walk, size, skip);

    // the specification clears ReadDeltas here, after the first block of the
    // superblock has read whatever the deltas were
    walk->read_deltas = 0u;

    mode->q_index = walk->q_index;
    for (uint32_t i = 0; i < TINY_AV1_FRAME_LF_COUNT; i++) {
        mode->delta_lf[i] = walk->delta_lf[i];
    }

    mode->y_mode = (uint8_t) read_y_mode(walk, row, col);
    mode->angle_delta_y = read_angle_delta(walk, size, mode->y_mode);

    if (mode->has_chroma) {
        mode->uv_mode = (uint8_t) read_uv_mode(walk, size, mode->lossless);

        if (mode->uv_mode == TINY_AV1_UV_CFL_PRED) read_cfl_alphas(walk);

        mode->angle_delta_uv = read_angle_delta(walk, size, mode->uv_mode);
    }

    read_filter_intra(walk, size);

    mode->tx_size = read_tx_size(walk, row, col, size, mode->lossless);

    // the neighbor context every block after this one reads, written across
    // every position the block covers
    uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[size];

    for (uint32_t y = 0; y < high && row + y < walk->frame->mi_rows; y++) {
        for (uint32_t x = 0; x < wide && col + x < walk->frame->mi_cols; x++) {
            size_t index = (size_t) (row + y) * walk->frame->mi_cols + col + x;

            walk->y_modes[index] = mode->y_mode;
            walk->skips[index] = mode->skip;
            walk->segment_ids[index] = mode->segment_id;
            walk->tx_sizes[index] = mode->tx_size;

            // only where the block has chroma of its own, which is what
            // `UVModes` means: a 4xN block on a subsampled plane leaves the
            // entry to whichever of the pair coded the mode
            if (walk->uv_modes && mode->has_chroma) {
                walk->uv_modes[index] = mode->uv_mode;
            }
        }
    }

    return TINYIMG_OK;
}

// #endregion
