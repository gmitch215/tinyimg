#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

/**
 * @file
 * @brief Specification 7.12.3 and 7.11.5, plus the walk that drives them.
 *
 * Dequantization, the inverse transform, the add, and the conversion of three
 * planes into the pixels a caller asked for. This is where the four stages
 * before it become an image, and where they become checkable against another
 * decoder: everything up to 1g could only be checked against the bitstream's
 * own length, and everything in 1h against a second reading of the
 * specification.
 *
 * **The order inside a block is the specification's, with one reordering.** The
 * coefficient parse reads no pixel, so `av1-coeff.c` parses a transform block
 * and hands it here to predict and reconstruct, where the specification
 * predicts first and parses second. Nothing depends on the difference and it
 * keeps the bitstream reader free of the frame store.
 *
 * Eight bits a sample throughout. A ten or twelve bit AVIF is refused at the
 * frame by `tiny_av1_frame_supported`, because every buffer here would have to
 * widen and the conversion at the end with them.
 */

// #region state

/** Samples along one side of an mi unit. */
#define AV1_MI_SIZE 4

/** Mi units along one side of the largest superblock. */
#define AV1_MAX_SB_MI 32

/** Entries a `BlockDecoded` axis needs, which runs from -1 to the superblock.
 */
#define AV1_DECODED_SPAN (AV1_MAX_SB_MI + 2)

/**
 * @brief The frame store and everything the reconstruction carries between
 * blocks.
 *
 * Allocated once per decode rather than per tile, because the frame store is
 * the frame's and the rest is cheap to reset. It reaches the sinks through
 * `walk->context`.
 */
typedef struct {
    const TinyAv1Sequence* sequence;
    const TinyAv1Frame* frame;

    /** One plane per component, at that plane's own extent. */
    uint8_t* plane[3];
    uint32_t stride[3];
    uint32_t width[3];
    uint32_t height[3];
    uint32_t planes;

    /**
     * @brief The extent the picture actually occupies, per plane.
     *
     * Smaller than `width` and `height`, which are the mi grid's and are what
     * the prediction clamps its neighbors to. The conversion has to use these
     * instead: a frame of 1204 luma samples has 604 decoded chroma samples and
     * only 602 of them are the picture, and reading the other two makes the
     * right edge disagree with every other decoder.
     */
    uint32_t visible_w[3];
    uint32_t visible_h[3];

    /**
     * @brief Whether each position of the current superblock has been decoded.
     *
     * The specification's `BlockDecoded`, indexed from -1 in both axes, which
     * is what the `+ 1` in every access is for. It answers two questions the
     * prediction asks: whether the samples above and to the right of a
     * transform block exist, and whether those below and to the left do. Both
     * are inside the superblock, so this is superblock-sized rather than
     * frame-sized.
     */
    uint8_t decoded[3][AV1_DECODED_SPAN][AV1_DECODED_SPAN];

    /** The luma extent reconstructed in this block, which is what CFL reads. */
    uint32_t max_luma_w;
    uint32_t max_luma_h;

    /** Whether the block above and to the left are in the tile, per plane. */
    uint8_t avail_above;
    uint8_t avail_left;
    uint8_t avail_above_chroma;
    uint8_t avail_left_chroma;

    /** Dequantized coefficients, replaced by the residual, at stride 64. */
    int32_t residual[64 * 64];

    /** The neighbors the prediction reads, corner first. */
    uint8_t above[1 + 2 * 64];
    uint8_t left[1 + 2 * 64];

    /**
     * @brief The transform size each position of each plane was coded at.
     *
     * `LoopfilterTxSizes`, which the deblocking filter reads to decide where a
     * transform edge is. Recorded here rather than derived, because a block's
     * own transform size is in luma units and says nothing about what a
     * subsampled plane actually transformed at.
     */
    uint8_t* lf_tx[3];

    /** Four loop filter deltas per position, or NULL when none are coded. */
    int8_t* delta_lf;
} Recon;

// #region quantizer

/** The dc quantizer for an index, which the bit depth chooses a curve for. */
static int32_t dc_quant(uint32_t bit_depth, int32_t index) {
    uint32_t curve = (bit_depth - 8u) >> 1;

    return tiny_av1_dc_qlookup[curve][tiny_clampi(index, 0, 255)];
}

static int32_t ac_quant(uint32_t bit_depth, int32_t index) {
    uint32_t curve = (bit_depth - 8u) >> 1;

    return tiny_av1_ac_qlookup[curve][tiny_clampi(index, 0, 255)];
}

/**
 * @brief The quantizer index a block codes against.
 *
 * `get_qindex(0, segmentId)`, which is not `seg_qindex`: that one is the same
 * function with the delta ignored, and the two differ on any frame that carries
 * both segmentation and a per-superblock quantizer delta. The alternate
 * quantizer is added to the running index there, not to the frame's.
 */
static int32_t block_qindex(const TinyAv1Walk* walk) {
    const TinyAv1Frame* frame = walk->frame;
    uint32_t id = walk->mode.segment_id;

    if (frame->segmentation_enabled && frame->seg_alt_q_active[id]) {
        int32_t base = frame->delta_q_present ? (int32_t) walk->mode.q_index
                                              : (int32_t) frame->base_q_idx;

        return tiny_clampi(base + frame->seg_alt_q[id], 0, 255);
    }

    if (frame->delta_q_present) return walk->mode.q_index;

    return frame->base_q_idx;
}

/** How much the dequantized coefficient is divided by, from the area. */
static int32_t dequant_denominator(uint32_t tx_size) {
    switch (tx_size) {
        case TINY_AV1_TX_32X32:
        case TINY_AV1_TX_16X32:
        case TINY_AV1_TX_32X16:
        case TINY_AV1_TX_16X64:
        case TINY_AV1_TX_64X16: return 2;
        case TINY_AV1_TX_64X64:
        case TINY_AV1_TX_32X64:
        case TINY_AV1_TX_64X32: return 4;
        default: return 1;
    }
}

/**
 * @brief Dequantizes one transform block into the residual buffer.
 *
 * The buffer is 64 wide whatever the transform is, and everything outside the
 * coefficients' own corner is zeroed here rather than by the inverse transform:
 * a transform larger than 32 on an axis carries coefficients only in its top
 * left 32x32, and the transform reads the whole extent.
 */
static void dequantize(
    const TinyAv1Walk* walk, const TinyAv1TxBlock* tx, int32_t* residual
) {
    const TinyAv1Frame* frame = walk->frame;
    uint32_t bit_depth = walk->sequence->bit_depth;
    uint32_t plane = tx->plane;

    uint32_t w = tiny_av1_tx_width[tx->tx_size];
    uint32_t h = tiny_av1_tx_height[tx->tx_size];
    uint32_t tw = w < 32u ? w : 32u;
    uint32_t th = h < 32u ? h : 32u;

    int32_t qindex = block_qindex(walk);
    int32_t denominator = dequant_denominator(tx->tx_size);

    int32_t dc_delta = plane == 0u   ? frame->delta_q_y_dc
                       : plane == 1u ? frame->delta_q_u_dc
                                     : frame->delta_q_v_dc;
    int32_t ac_delta = plane == 0u   ? 0
                       : plane == 1u ? frame->delta_q_u_ac
                                     : frame->delta_q_v_ac;

    int32_t dc = dc_quant(bit_depth, qindex + dc_delta);
    int32_t ac = ac_quant(bit_depth, qindex + ac_delta);

    uint32_t level = frame->seg_qm_level[plane][walk->mode.segment_id];
    int matrix =
        frame->using_qmatrix && tx->tx_type < TINY_AV1_IDTX && level < 15u;

    const uint8_t* qm =
        matrix ? tiny_av1_quantizer_matrix[level][plane > 0u ? 1u : 0u] : 0;
    uint32_t offset = tiny_av1_qm_offset[tx->tx_size];

    int32_t limit = 1 << (7u + bit_depth);

    for (uint32_t i = 0; i < h; i++) {
        int32_t* row = residual + (size_t) i * 64u;

        for (uint32_t j = 0; j < w; j++) row[j] = 0;

        if (i >= th) continue;

        for (uint32_t j = 0; j < tw; j++) {
            int32_t q = (i == 0u && j == 0u) ? dc : ac;

            if (qm) {
                int32_t weight = qm[offset + i * tw + j];

                q = (q * weight + 16) >> 5;
            }

            int32_t value = tx->quant[i * tw + j] * q;
            int32_t sign = value < 0 ? -1 : 1;
            int32_t magnitude = (value < 0 ? -value : value) & 0xFFFFFF;

            row[j] = tiny_clampi(
                sign * (magnitude / denominator), -limit, limit - 1
            );
        }
    }
}

// #region neighbors

/** Whether an mi position is inside the tile being reconstructed. */
static int inside(const TinyAv1Walk* walk, int32_t row, int32_t col) {
    return row >= (int32_t) walk->row_start && row < (int32_t) walk->row_end &&
           col >= (int32_t) walk->col_start && col < (int32_t) walk->col_end;
}

/** Whether a neighbor's prediction is one of the three smooth ones. */
static int is_smooth(uint32_t mode) {
    return mode == TINY_AV1_SMOOTH_PRED || mode == TINY_AV1_SMOOTH_V_PRED ||
           mode == TINY_AV1_SMOOTH_H_PRED;
}

/**
 * @brief Whether either neighbor predicts smoothly, which softens the edge
 * filter.
 *
 * The chroma lookup moves to the position that actually coded a chroma mode: a
 * 4xN block on a subsampled plane shares its chroma with its neighbor, so the
 * mode is on one of the pair and the adjustment is which one.
 */
static uint32_t filter_type_of(const TinyAv1Walk* walk, uint32_t plane) {
    const TinyAv1Mode* mode = &walk->mode;
    uint32_t sub_x = walk->sequence->sub_x;
    uint32_t sub_y = walk->sequence->sub_y;
    uint32_t cols = walk->frame->mi_cols;

    int above_smooth = 0;
    int left_smooth = 0;

    int have_above =
        plane == 0u
            ? inside(walk, (int32_t) mode->row - 1, (int32_t) mode->col)
            : (int) walk->mode.has_chroma &&
                  inside(walk, (int32_t) mode->row - 1, (int32_t) mode->col);

    if (have_above) {
        int32_t r = (int32_t) mode->row - 1;
        int32_t c = (int32_t) mode->col;

        if (plane > 0u) {
            if (sub_x && !(mode->col & 1u)) c++;
            if (sub_y && (mode->row & 1u)) r--;
        }

        if (r >= 0 && c >= 0 && (uint32_t) c < cols) {
            size_t at = (size_t) r * cols + (uint32_t) c;

            above_smooth =
                is_smooth(plane == 0u ? walk->y_modes[at] : walk->uv_modes[at]);
        }
    }

    int have_left =
        plane == 0u
            ? inside(walk, (int32_t) mode->row, (int32_t) mode->col - 1)
            : (int) walk->mode.has_chroma &&
                  inside(walk, (int32_t) mode->row, (int32_t) mode->col - 1);

    if (have_left) {
        int32_t r = (int32_t) mode->row;
        int32_t c = (int32_t) mode->col - 1;

        if (plane > 0u) {
            if (sub_x && (mode->col & 1u)) c--;
            if (sub_y && !(mode->row & 1u)) r++;
        }

        if (r >= 0 && c >= 0 && (uint32_t) c < cols &&
            (uint32_t) r < walk->frame->mi_rows) {
            size_t at = (size_t) r * cols + (uint32_t) c;

            left_smooth =
                is_smooth(plane == 0u ? walk->y_modes[at] : walk->uv_modes[at]);
        }
    }

    return above_smooth || left_smooth ? 1u : 0u;
}

/**
 * @brief Fills the two neighbor arrays from the frame store.
 *
 * The corner goes at index 0 of both and the samples follow, which is the
 * layout `tiny_av1_predict_intra` documents. How far the real samples run is
 * reported back so the prediction knows where to start replicating: past the
 * frame's own edge, and past the block above and to the right when that has not
 * been decoded yet.
 */
static void gather(
    Recon* recon, uint32_t plane, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h, int have_above, int have_left, int have_above_right,
    int have_below_left, uint32_t* above_count, uint32_t* left_count
) {
    const uint8_t* data = recon->plane[plane];
    uint32_t stride = recon->stride[plane];
    uint32_t max_x = recon->width[plane] - 1u;
    uint32_t max_y = recon->height[plane] - 1u;

    *above_count = 0;
    *left_count = 0;

    if (have_above) {
        uint32_t reach = have_above_right ? 2u * w : w;
        uint32_t limit = x + reach - 1u;

        if (limit > max_x) limit = max_x;

        *above_count = limit - x + 1u;

        for (uint32_t i = 0; i < *above_count; i++) {
            recon->above[1u + i] = data[(size_t) (y - 1u) * stride + x + i];
        }
    }

    if (have_left) {
        uint32_t reach = have_below_left ? 2u * h : h;
        uint32_t limit = y + reach - 1u;

        if (limit > max_y) limit = max_y;

        *left_count = limit - y + 1u;

        for (uint32_t i = 0; i < *left_count; i++) {
            recon->left[1u + i] = data[(size_t) (y + i) * stride + x - 1u];
        }
    }

    uint8_t corner = 0;

    if (have_above && have_left) {
        corner = data[(size_t) (y - 1u) * stride + x - 1u];
    }

    recon->above[0] = corner;
    recon->left[0] = corner;
}

// #region blocks

/** Reads one entry of `BlockDecoded`, whose axes start at -1. */
static int decoded_at(
    const Recon* recon, uint32_t plane, int32_t row, int32_t col
) {
    if (row < -1 || col < -1) return 0;
    if (row + 1 >= AV1_DECODED_SPAN || col + 1 >= AV1_DECODED_SPAN) return 0;

    return recon->decoded[plane][row + 1][col + 1];
}

/**
 * @brief Reconstructs one transform block: predict, then add its residual.
 *
 * Called by the residual reader once per transform block, in bitstream order,
 * including for a block that codes nothing. A skipped block is still predicted;
 * only the add is conditional.
 */
static int reconstruct_tx(TinyAv1Walk* walk, const TinyAv1TxBlock* tx) {
    Recon* recon = (Recon*) walk->context;
    const TinyAv1Mode* mode = &walk->mode;

    uint32_t plane = tx->plane;
    uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
    uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

    uint32_t w = tiny_av1_tx_width[tx->tx_size];
    uint32_t h = tiny_av1_tx_height[tx->tx_size];
    uint32_t step_x = w >> 2;
    uint32_t step_y = h >> 2;

    uint32_t row = (tx->y << sub_y) >> 2;
    uint32_t col = (tx->x << sub_x) >> 2;
    uint32_t sb_mask = walk->sequence->use_128x128_superblock ? 31u : 15u;
    uint32_t sb_row = (row & sb_mask) >> sub_y;
    uint32_t sb_col = (col & sb_mask) >> sub_x;

    uint32_t base_x = (mode->col >> sub_x) * AV1_MI_SIZE;
    uint32_t base_y = (mode->row >> sub_y) * AV1_MI_SIZE;
    uint32_t within_x = (tx->x - base_x) >> 2;
    uint32_t within_y = (tx->y - base_y) >> 2;

    int have_left = plane == 0u ? recon->avail_left : recon->avail_left_chroma;
    int have_above =
        plane == 0u ? recon->avail_above : recon->avail_above_chroma;

    have_left = have_left || within_x > 0u;
    have_above = have_above || within_y > 0u;

    int have_above_right = decoded_at(
        recon, plane, (int32_t) sb_row - 1, (int32_t) (sb_col + step_x)
    );
    int have_below_left = decoded_at(
        recon, plane, (int32_t) (sb_row + step_y), (int32_t) sb_col - 1
    );

    uint32_t above_count = 0;
    uint32_t left_count = 0;

    gather(
        recon, plane, tx->x, tx->y, w, h, have_above, have_left,
        have_above_right, have_below_left, &above_count, &left_count
    );

    int is_cfl = plane > 0u && mode->uv_mode == TINY_AV1_UV_CFL_PRED;

    uint32_t predict_mode = plane == 0u
                                ? mode->y_mode
                                : (is_cfl ? TINY_AV1_DC_PRED : mode->uv_mode);

    TinyAv1PredictOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));

    opts.have_above = (uint8_t) (have_above ? 1 : 0);
    opts.have_left = (uint8_t) (have_left ? 1 : 0);
    opts.above_available = (uint16_t) above_count;
    opts.left_available = (uint16_t) left_count;
    opts.angle_delta = plane == 0u ? mode->angle_delta_y : mode->angle_delta_uv;
    opts.use_filter_intra = plane == 0u ? mode->use_filter_intra : 0u;
    opts.filter_intra_mode = mode->filter_intra_mode;
    opts.edge_filter = walk->sequence->enable_intra_edge_filter;
    opts.filter_type = (uint8_t) filter_type_of(walk, plane);
    opts.bit_depth = walk->sequence->bit_depth;

    uint8_t* dst =
        recon->plane[plane] + (size_t) tx->y * recon->stride[plane] + tx->x;

    int status = tiny_av1_predict_intra(
        dst, recon->stride[plane], w, h, recon->above, recon->left,
        (TinyAv1IntraMode) predict_mode, &opts
    );

    if (status != TINYIMG_OK) return status;

    if (is_cfl) {
        uint32_t luma_x = tx->x << sub_x;
        uint32_t luma_y = tx->y << sub_y;

        if (luma_x >= recon->max_luma_w || luma_y >= recon->max_luma_h) {
            return TINYIMG_ERR_CORRUPT;
        }

        int32_t alpha = plane == 1u ? mode->cfl_alpha_u : mode->cfl_alpha_v;

        status = tiny_av1_predict_cfl(
            dst, recon->stride[plane], w, h,
            recon->plane[0] + (size_t) luma_y * recon->stride[0] + luma_x,
            recon->stride[0], recon->max_luma_w - luma_x,
            recon->max_luma_h - luma_y, sub_x, sub_y, alpha,
            walk->sequence->bit_depth
        );

        if (status != TINYIMG_OK) return status;
    }

    if (plane == 0u) {
        recon->max_luma_w = tx->x + w;
        recon->max_luma_h = tx->y + h;

        if (recon->max_luma_w > recon->width[0]) {
            recon->max_luma_w = recon->width[0];
        }
        if (recon->max_luma_h > recon->height[0]) {
            recon->max_luma_h = recon->height[0];
        }
    }

    if (tx->eob > 0u) {
        dequantize(walk, tx, recon->residual);

        if (tx->lossless) {
            status = tiny_av1_inverse_wht(
                recon->residual, 64u, walk->sequence->bit_depth
            );
        }
        else {
            status = tiny_av1_inverse_transform(
                recon->residual, 64u, (TinyAv1TxSize) tx->tx_size,
                (TinyAv1TxType) tx->tx_type, walk->sequence->bit_depth
            );
        }

        if (status != TINYIMG_OK) return status;

        // the flip is applied inside the transform, so the residual is added in
        // raster order and a caller that mirrored again would undo it
        uint32_t rows = h;
        uint32_t cols = w;

        if (tx->y + rows > recon->height[plane]) {
            rows = recon->height[plane] - tx->y;
        }
        if (tx->x + cols > recon->width[plane]) {
            cols = recon->width[plane] - tx->x;
        }

        for (uint32_t i = 0; i < rows; i++) {
            uint8_t* out = dst + (size_t) i * recon->stride[plane];
            const int32_t* in = recon->residual + (size_t) i * 64u;

            for (uint32_t j = 0; j < cols; j++) {
                out[j] = tiny_clamp_u8((int32_t) out[j] + in[j]);
            }
        }
    }

    uint32_t plane_row = row >> sub_y;
    uint32_t plane_col = col >> sub_x;
    uint32_t cols = walk->frame->mi_cols;

    for (uint32_t i = 0; i < step_y; i++) {
        for (uint32_t j = 0; j < step_x; j++) {
            if (sb_row + i + 1u < AV1_DECODED_SPAN &&
                sb_col + j + 1u < AV1_DECODED_SPAN) {
                recon->decoded[plane][sb_row + i + 1u][sb_col + j + 1u] = 1u;
            }

            if (plane_row + i < walk->frame->mi_rows && plane_col + j < cols) {
                recon->lf_tx[plane]
                            [(size_t) (plane_row + i) * cols + plane_col + j] =
                    (uint8_t) tx->tx_size;
            }
        }
    }

    return TINYIMG_OK;
}

/** Reads one block's mode info, then reconstructs its residual. */
static int reconstruct_block(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    Recon* recon = (Recon*) walk->context;

    int status = tiny_av1_read_mode(walk, row, col, size);
    if (status != TINYIMG_OK) return status;

    uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[size];

    recon->avail_above =
        (uint8_t) (inside(walk, (int32_t) row - 1, (int32_t) col) ? 1 : 0);
    recon->avail_left =
        (uint8_t) (inside(walk, (int32_t) row, (int32_t) col - 1) ? 1 : 0);

    /*
     * The chroma availability is not the luma one on a subsampled plane.
     *
     * A block one mi unit tall shares its chroma with the block above it, so
     * the chroma neighbor is two rows up rather than one; likewise one column
     * wide and two columns left. A block with no chroma of its own has neither.
     */
    if (walk->mode.has_chroma) {
        recon->avail_above_chroma = recon->avail_above;
        recon->avail_left_chroma = recon->avail_left;

        if (walk->sequence->sub_y && high == 1u) {
            recon->avail_above_chroma =
                (uint8_t) (inside(walk, (int32_t) row - 2, (int32_t) col) ? 1
                                                                          : 0);
        }
        if (walk->sequence->sub_x && wide == 1u) {
            recon->avail_left_chroma =
                (uint8_t) (inside(walk, (int32_t) row, (int32_t) col - 2) ? 1
                                                                          : 0);
        }
    }
    else {
        recon->avail_above_chroma = 0u;
        recon->avail_left_chroma = 0u;
    }

    if (recon->delta_lf) {
        uint32_t cols = walk->frame->mi_cols;

        for (uint32_t y = 0; y < high && row + y < walk->frame->mi_rows; y++) {
            for (uint32_t x = 0; x < wide && col + x < cols; x++) {
                size_t at = ((size_t) (row + y) * cols + col + x) * 4u;

                for (uint32_t i = 0; i < TINY_AV1_FRAME_LF_COUNT; i++) {
                    recon->delta_lf[at + i] = walk->mode.delta_lf[i];
                }
            }
        }
    }

    return tiny_av1_read_residual(walk);
}

/**
 * @brief Clears what a new superblock has no history of.
 *
 * `clear_block_decoded_flags`, whose shape is the reason it is a function: the
 * row above and the column to the left of the superblock are marked decoded
 * where they exist, because they belong to the superblock before this one, and
 * the interior is marked undecoded. The one position that is cleared despite
 * being on the left edge is the bottom left corner, which no block can have
 * decoded yet.
 */
static int start_superblock(TinyAv1Walk* walk, uint32_t row, uint32_t col) {
    Recon* recon = (Recon*) walk->context;
    uint32_t span = walk->sequence->use_128x128_superblock ? 32u : 16u;

    for (uint32_t plane = 0; plane < recon->planes; plane++) {
        uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
        uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

        uint32_t wide = (walk->col_end - col) >> sub_x;
        uint32_t high = (walk->row_end - row) >> sub_y;

        for (int32_t y = -1; y <= (int32_t) (span >> sub_y); y++) {
            for (int32_t x = -1; x <= (int32_t) (span >> sub_x); x++) {
                if (y + 1 >= AV1_DECODED_SPAN || x + 1 >= AV1_DECODED_SPAN) {
                    continue;
                }

                uint8_t value = 0;

                if (y < 0 && x < (int32_t) wide)
                    value = 1u;
                else if (x < 0 && y < (int32_t) high)
                    value = 1u;

                recon->decoded[plane][y + 1][x + 1] = value;
            }
        }

        recon->decoded[plane][(span >> sub_y) + 1u][0] = 0u;
    }

    return TINYIMG_OK;
}

// #region output

/** The three coefficients a non-constant-luminance matrix is built from. */
typedef struct {
    /** Round2 shift the fixed point coefficients are scaled by. */
    int32_t to_r_cr;
    int32_t to_g_cb;
    int32_t to_g_cr;
    int32_t to_b_cb;
    /** Applied to the luma before the chroma terms. */
    int32_t y_scale;
    int32_t y_offset;
    /** Non-zero when the planes are GBR rather than YUV. */
    uint8_t identity;
} Matrix;

/** Fixed point shift the conversion coefficients are held at. */
#define AV1_MATRIX_BITS 16

/**
 * @brief Builds the conversion for one `matrix_coefficients` value.
 *
 * Three real matrices reach this from the fixtures alone: 9 from `cavif`, 6
 * from `avifenc` and 0 from a third converter, which is the identity and means
 * the planes are GBR rather than YUV. Getting it wrong gives a picture that is
 * plausible and wrong everywhere, which is why the field is carried through
 * from the sequence header rather than assumed.
 *
 * Anything this does not know is read as BT.601, which is what a decoder with
 * no other information does; `matrix_coefficients` 2 means unspecified and is
 * common enough that refusing it would refuse real files.
 */
static Matrix matrix_for(uint32_t coefficients, uint32_t full_range) {
    Matrix m;
    tiny_memset(&m, 0, sizeof(m));

    if (coefficients == 0u) {
        m.identity = 1u;
        return m;
    }

    float kr = 0.299f;
    float kb = 0.114f;

    if (coefficients == 1u) {
        kr = 0.2126f;
        kb = 0.0722f;
    }
    else if (coefficients == 7u) {
        kr = 0.212f;
        kb = 0.087f;
    }
    else if (coefficients == 9u || coefficients == 10u) {
        kr = 0.2627f;
        kb = 0.0593f;
    }

    float kg = 1.0f - kr - kb;
    float scale = (float) (1 << AV1_MATRIX_BITS);

    /*
     * The studio range expands as it converts.
     *
     * Luma runs 16 to 235 and chroma 16 to 240, so the luma is scaled by
     * 255/219 and the chroma terms by 255/224 before the matrix. A full range
     * file needs neither, and reading one as the other is a visible contrast
     * error rather than a subtle one.
     */
    float y_gain = full_range ? 1.0f : 255.0f / 219.0f;
    float c_gain = full_range ? 1.0f : 255.0f / 224.0f;

    m.y_scale = (int32_t) (y_gain * scale + 0.5f);
    m.y_offset = full_range ? 0 : 16;

    m.to_r_cr = (int32_t) (2.0f * (1.0f - kr) * c_gain * scale + 0.5f);
    m.to_b_cb = (int32_t) (2.0f * (1.0f - kb) * c_gain * scale + 0.5f);
    m.to_g_cb =
        (int32_t) (2.0f * kb * (1.0f - kb) / kg * c_gain * scale + 0.5f);
    m.to_g_cr =
        (int32_t) (2.0f * kr * (1.0f - kr) / kg * c_gain * scale + 0.5f);

    return m;
}

/**
 * @brief One chroma sample at a luma position, interpolated or replicated.
 *
 * The triangle filter is the one libjpeg calls fancy upsampling and this
 * library already runs for JPEG's chroma: the nearer sample on each axis counts
 * three times and the further one once, so a 4:2:0 plane contributes 9, 3, 3
 * and 1 sixteenths. Measured against `avifdec` on `fox.avif` it is worth
 * **48.08 dB to 54.40 dB and a worst case of 16 to 2**, which is what says the
 * reference decoder interpolates rather than replicating.
 *
 * Under TINYIMG_EFFORT_FAST it replicates instead, which is the same trade the
 * JPEG path takes and the second lever the effort tier has on this decoder.
 */
static int32_t chroma_at(
    const Recon* recon, uint32_t plane, uint32_t x, uint32_t y, int fancy
) {
    uint32_t sub_x = recon->sequence->sub_x;
    uint32_t sub_y = recon->sequence->sub_y;
    uint32_t stride = recon->stride[plane];
    const uint8_t* data = recon->plane[plane];

    uint32_t last_x = recon->visible_w[plane] - 1u;
    uint32_t last_y = recon->visible_h[plane] - 1u;

    uint32_t cx = x >> sub_x;
    uint32_t cy = y >> sub_y;

    if (cx > last_x) cx = last_x;
    if (cy > last_y) cy = last_y;

    if (!fancy || (!sub_x && !sub_y)) {
        return data[(size_t) cy * stride + cx];
    }

    // the further sample on each subsampled axis, clamped, so the edges need no
    // case of their own
    uint32_t ox = cx;
    uint32_t oy = cy;

    if (sub_x) {
        ox = (x & 1u) ? cx + 1u : (cx > 0u ? cx - 1u : 0u);
        if (ox > last_x) ox = last_x;
    }
    if (sub_y) {
        oy = (y & 1u) ? cy + 1u : (cy > 0u ? cy - 1u : 0u);
        if (oy > last_y) oy = last_y;
    }

    uint32_t near = data[(size_t) cy * stride + cx];
    uint32_t far_x = data[(size_t) cy * stride + ox];
    uint32_t near_y = data[(size_t) oy * stride + cx];
    uint32_t far = data[(size_t) oy * stride + ox];

    if (!sub_y) return (int32_t) ((near * 3u + far_x + 2u) >> 2);
    if (!sub_x) return (int32_t) ((near * 3u + near_y + 2u) >> 2);

    uint32_t rows = near * 3u + near_y;
    uint32_t other = far_x * 3u + far;

    return (int32_t) ((rows * 3u + other + 8u) >> 4);
}

/** One source pixel, converted, at the plane resolution of the frame. */
static void convert_pixel(
    const Recon* recon, const Matrix* m, uint32_t x, uint32_t y, int fancy,
    uint8_t* out
) {
    int32_t luma = recon->plane[0][(size_t) y * recon->stride[0] + x];

    if (recon->planes == 1u) {
        out[0] = (uint8_t) luma;
        out[1] = (uint8_t) luma;
        out[2] = (uint8_t) luma;

        return;
    }

    int32_t cb = chroma_at(recon, 1u, x, y, fancy);
    int32_t cr = chroma_at(recon, 2u, x, y, fancy);

    if (m->identity) {
        // the identity matrix means the planes are already colour, in the order
        // green, blue, red
        out[0] = (uint8_t) cr;
        out[1] = (uint8_t) luma;
        out[2] = (uint8_t) cb;

        return;
    }

    int32_t base = (luma - m->y_offset) * m->y_scale;
    int32_t u = cb - 128;
    int32_t v = cr - 128;

    int32_t half = 1 << (AV1_MATRIX_BITS - 1);

    int32_t r = (base + v * m->to_r_cr + half) >> AV1_MATRIX_BITS;
    int32_t g =
        (base - u * m->to_g_cb - v * m->to_g_cr + half) >> AV1_MATRIX_BITS;
    int32_t b = (base + u * m->to_b_cb + half) >> AV1_MATRIX_BITS;

    out[0] = tiny_clamp_u8(r);
    out[1] = tiny_clamp_u8(g);
    out[2] = tiny_clamp_u8(b);
}

/**
 * @brief Writes the requested rectangle, at the requested scale, as pixels.
 *
 * A reduction box averages rather than picking a sample, which is what every
 * other codec here does and what makes a scaled decode a real reduction. The
 * decode itself is still full resolution: AV1's reconstruction is not scaled,
 * so the saving of a scaled request is in the resample and the output rather
 * than in the decode, and saying otherwise would be an overclaim.
 */
static void write_pixels(
    const Recon* recon, TinyImage* image, uint32_t region_x, uint32_t region_y,
    uint32_t scale, int fancy
) {
    Matrix m = matrix_for(
        recon->sequence->matrix_coefficients, recon->sequence->color_range
    );

    uint32_t channels = image->channels;

    for (uint32_t dy = 0; dy < image->height; dy++) {
        for (uint32_t dx = 0; dx < image->width; dx++) {
            uint32_t sum[3] = {0, 0, 0};
            uint32_t count = 0;

            for (uint32_t oy = 0; oy < scale; oy++) {
                uint32_t sy = region_y + dy * scale + oy;

                if (sy >= recon->height[0]) break;

                for (uint32_t ox = 0; ox < scale; ox++) {
                    uint32_t sx = region_x + dx * scale + ox;

                    if (sx >= recon->width[0]) break;

                    uint8_t rgb[3];
                    convert_pixel(recon, &m, sx, sy, fancy, rgb);

                    sum[0] += rgb[0];
                    sum[1] += rgb[1];
                    sum[2] += rgb[2];
                    count++;
                }
            }

            if (count == 0u) count = 1u;

            uint8_t* out =
                image->data + ((size_t) dy * image->width + dx) * channels;

            if (channels >= 3u) {
                out[0] = (uint8_t) ((sum[0] + count / 2u) / count);
                out[1] = (uint8_t) ((sum[1] + count / 2u) / count);
                out[2] = (uint8_t) ((sum[2] + count / 2u) / count);

                if (channels == 4u) out[3] = 255u;
            }
            else {
                // one or two channels wants luminance, which for a monochrome
                // file is the plane itself and otherwise the green weight
                uint32_t grey =
                    (sum[0] * 77u + sum[1] * 150u + sum[2] * 29u) >> 8;

                out[0] = (uint8_t) ((grey + count / 2u) / count);

                if (channels == 2u) out[1] = 255u;
            }
        }
    }
}

// #region entry point

/** Frees whatever the decode allocated, in the order it was allocated. */
static void release(
    Recon* recon, uint8_t* sizes, uint8_t* modes, uint8_t* skips,
    uint8_t* segments, uint8_t* tx_sizes, uint8_t* uv_modes, int8_t* cdef,
    TinyAv1Cdf* cdf, TinyAv1Residual* residual
) {
    if (recon) {
        for (uint32_t i = 0; i < 3u; i++) {
            tiny_free(recon->plane[i]);
            tiny_free(recon->lf_tx[i]);
        }

        tiny_free(recon->delta_lf);
        tiny_free(recon);
    }

    tiny_free(sizes);
    tiny_free(modes);
    tiny_free(skips);
    tiny_free(segments);
    tiny_free(tx_sizes);
    tiny_free(uv_modes);
    tiny_free(cdef);
    tiny_free(cdf);

    if (residual) {
        tiny_free(residual->above_level);
        tiny_free(residual->above_dc);
        tiny_free(residual->left_level);
        tiny_free(residual->left_dc);
        tiny_free(residual);
    }
}

int tiny_av1_decode(
    TinyImage* image, const uint8_t* data, size_t size,
    const TinyDecodeOpts* opts
) {
    if (!image || !data) return TINYIMG_ERR_NULL;

    TinyAv1Sequence sequence;
    TinyAv1Frame frame;

    tiny_memset(&sequence, 0, sizeof(sequence));
    tiny_memset(&frame, 0, sizeof(frame));

    const uint8_t* group = 0;
    size_t group_size = 0;
    int seen_sequence = 0;
    int seen_frame = 0;

    size_t at = 0;
    TinyAv1Obu obu;

    while (tiny_av1_next_obu(data, size, &at, &obu)) {
        if (obu.type == TINY_AV1_OBU_SEQUENCE_HEADER) {
            /*
             * The first sequence header wins.
             *
             * `base-alpha.avif` carries two, because its alpha is a separate
             * monochrome AV1 item and the container concatenates the two
             * streams into one `mdat`; reading the second over the first would
             * decode the colour image with the alpha image's plane count.
             */
            if (seen_sequence) continue;

            int status =
                tiny_av1_read_sequence(&sequence, obu.payload, obu.size);

            if (status != TINYIMG_OK) return status;

            seen_sequence = 1;
        }
        else if (obu.type == TINY_AV1_OBU_FRAME_HEADER && !seen_frame) {
            if (!seen_sequence) return TINYIMG_ERR_CORRUPT;

            int status = tiny_av1_read_frame(
                &frame, &sequence, obu.payload, obu.size, 0
            );

            if (status != TINYIMG_OK) return status;

            seen_frame = 1;
        }
        else if (obu.type == TINY_AV1_OBU_TILE_GROUP && !group) {
            if (!seen_frame) return TINYIMG_ERR_CORRUPT;

            group = obu.payload;
            group_size = obu.size;
        }
        else if (obu.type == TINY_AV1_OBU_FRAME && !seen_frame) {
            if (!seen_sequence) return TINYIMG_ERR_CORRUPT;

            size_t consumed = 0;
            int status = tiny_av1_read_frame(
                &frame, &sequence, obu.payload, obu.size, &consumed
            );

            if (status != TINYIMG_OK) return status;

            seen_frame = 1;
            group = obu.payload + consumed;
            group_size = obu.size - consumed;
        }
    }

    if (!seen_sequence || !seen_frame || !group) return TINYIMG_ERR_CORRUPT;

    int supported = tiny_av1_frame_supported(&sequence, &frame);
    if (supported != TINYIMG_OK) return supported;

    if (frame.width == 0u || frame.height == 0u) return TINYIMG_ERR_CORRUPT;

    uint32_t planes = sequence.planes;
    uint32_t cells = frame.mi_cols * frame.mi_rows;

    Recon* recon = (Recon*) tiny_alloc(sizeof(Recon));
    uint8_t* sizes = (uint8_t*) tiny_alloc(cells);
    uint8_t* modes = (uint8_t*) tiny_alloc(cells);
    uint8_t* skips = (uint8_t*) tiny_alloc(cells);
    uint8_t* segments = (uint8_t*) tiny_alloc(cells);
    uint8_t* tx_sizes = (uint8_t*) tiny_alloc(cells);
    uint8_t* uv_modes = (uint8_t*) tiny_alloc(cells);

    uint32_t cdef_stride = (frame.mi_cols + 15u) / 16u;
    uint32_t cdef_rows = (frame.mi_rows + 15u) / 16u;
    int8_t* cdef = (int8_t*) tiny_alloc((size_t) cdef_stride * cdef_rows);

    TinyAv1Cdf* cdf = (TinyAv1Cdf*) tiny_alloc(sizeof(TinyAv1Cdf));
    TinyAv1Residual* residual =
        (TinyAv1Residual*) tiny_alloc(sizeof(TinyAv1Residual));

    if (!recon || !sizes || !modes || !skips || !segments || !tx_sizes ||
        !uv_modes || !cdef || !cdf || !residual) {
        release(
            recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
            residual
        );

        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(recon, 0, sizeof(*recon));
    tiny_memset(residual, 0, sizeof(*residual));

    recon->sequence = &sequence;
    recon->frame = &frame;
    recon->planes = planes;

    /*
     * The store is superblock aligned and the extent is not.
     *
     * A block is coded whole even where the frame ends inside it, and the
     * prediction writes its whole extent: a 64x64 transform starting two mi
     * units before the bottom edge writes sixty-two rows past it. The
     * specification's own `CurrFrame` has the same property, since
     * `transform_block` skips a block only when its **origin** is outside.
     *
     * So the allocation rounds up to a superblock and `width` and `height` stay
     * the frame's own, because they are the `maxX` and `maxY` the neighbor
     * replication clamps to. Writing past the extent is fine; reading past it
     * would be a decode that depends on uninitialized memory.
     */
    uint32_t align = sequence.use_128x128_superblock ? 32u : 16u;
    uint32_t aligned_cols = (frame.mi_cols + align - 1u) / align * align;
    uint32_t aligned_rows = (frame.mi_rows + align - 1u) / align * align;

    for (uint32_t plane = 0; plane < planes; plane++) {
        uint32_t sub_x = plane > 0u ? sequence.sub_x : 0u;
        uint32_t sub_y = plane > 0u ? sequence.sub_y : 0u;

        uint32_t store_w = (aligned_cols * AV1_MI_SIZE) >> sub_x;
        uint32_t store_h = (aligned_rows * AV1_MI_SIZE) >> sub_y;

        recon->width[plane] = (frame.mi_cols * AV1_MI_SIZE) >> sub_x;
        recon->height[plane] = (frame.mi_rows * AV1_MI_SIZE) >> sub_y;
        recon->visible_w[plane] = (frame.width + sub_x) >> sub_x;
        recon->visible_h[plane] = (frame.height + sub_y) >> sub_y;
        recon->stride[plane] = store_w;
        recon->plane[plane] = (uint8_t*) tiny_alloc((size_t) store_w * store_h);

        if (!recon->plane[plane]) {
            release(
                recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef,
                cdf, residual
            );

            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(recon->plane[plane], 0, (size_t) store_w * store_h);

        recon->lf_tx[plane] = (uint8_t*) tiny_alloc(cells);

        if (!recon->lf_tx[plane]) {
            release(
                recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef,
                cdf, residual
            );

            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(recon->lf_tx[plane], 0, cells);
    }

    if (frame.delta_lf_present) {
        recon->delta_lf =
            (int8_t*) tiny_alloc((size_t) cells * TINY_AV1_FRAME_LF_COUNT);

        if (!recon->delta_lf) {
            release(
                recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef,
                cdf, residual
            );

            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(
            recon->delta_lf, 0, (size_t) cells * TINY_AV1_FRAME_LF_COUNT
        );
    }

    uint32_t above_stride = frame.mi_cols + TINY_AV1_CONTEXT_PAD;
    uint32_t left_stride = frame.mi_rows + TINY_AV1_CONTEXT_PAD;

    residual->above_level = (uint8_t*) tiny_alloc((size_t) above_stride * 3u);
    residual->above_dc = (uint8_t*) tiny_alloc((size_t) above_stride * 3u);
    residual->left_level = (uint8_t*) tiny_alloc((size_t) left_stride * 3u);
    residual->left_dc = (uint8_t*) tiny_alloc((size_t) left_stride * 3u);
    residual->above_stride = above_stride;
    residual->left_stride = left_stride;
    residual->block = reconstruct_tx;

    if (!residual->above_level || !residual->above_dc ||
        !residual->left_level || !residual->left_dc) {
        release(
            recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
            residual
        );

        return TINYIMG_ERR_MEMORY;
    }

    /*
     * The requested rectangle, in source pixels, and the denominator.
     *
     * Resolved before the walk so a tile outside the rectangle can be skipped
     * whole, which is the one thing AV1 offers here that VP8 does not: a tile
     * resets the symbol decoder, so skipping one costs nothing and loses
     * nothing. Every file measured has one tile, so it buys nothing today and
     * the seam is what matters.
     */
    uint32_t region_x = opts ? opts->x : 0u;
    uint32_t region_y = opts ? opts->y : 0u;
    uint32_t region_w = opts && opts->width ? opts->width : frame.width;
    uint32_t region_h = opts && opts->height ? opts->height : frame.height;

    if (region_x >= frame.width || region_y >= frame.height) {
        release(
            recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
            residual
        );

        return TINYIMG_ERR_BOUNDS;
    }

    if (region_x + region_w > frame.width) region_w = frame.width - region_x;
    if (region_y + region_h > frame.height) region_h = frame.height - region_y;

    uint32_t scale = opts ? opts->scale_den : 1u;

    if (scale != 2u && scale != 4u && scale != 8u) scale = 1u;

    tiny_memset(sizes, 0, cells);
    tiny_memset(modes, 0, cells);
    tiny_memset(skips, 0, cells);
    tiny_memset(segments, 0, cells);
    tiny_memset(tx_sizes, 0, cells);
    tiny_memset(uv_modes, 0, cells);
    tiny_memset(cdef, -1, (size_t) cdef_stride * cdef_rows);

    uint32_t tiles = frame.tile_cols * frame.tile_rows;
    int status = TINYIMG_OK;

    for (uint32_t index = 0; index < tiles && status == TINYIMG_OK; index++) {
        uint32_t tile_row = index / frame.tile_cols;
        uint32_t tile_col = index % frame.tile_cols;

        uint32_t row_start = frame.mi_row_starts[tile_row];
        uint32_t row_end = frame.mi_row_starts[tile_row + 1u];
        uint32_t col_start = frame.mi_col_starts[tile_col];
        uint32_t col_end = frame.mi_col_starts[tile_col + 1u];

        const uint8_t* bytes = 0;
        size_t bytes_size = 0;

        status = tiny_av1_tile_bytes(
            &frame, group, group_size, index, &bytes, &bytes_size
        );

        if (status != TINYIMG_OK) break;

        // only the distributions are per tile; the mi arrays and the CDEF
        // grid are the frame's, because both post-filters read them after
        // every tile has been decoded
        tiny_av1_cdf_init(cdf);

        TinyAv1Symbol symbol;
        tiny_av1_symbol_init(&symbol, bytes, bytes_size);

        TinyAv1Walk walk;
        tiny_memset(&walk, 0, sizeof(walk));

        walk.symbol = &symbol;
        walk.cdf = cdf;
        walk.sequence = &sequence;
        walk.frame = &frame;
        walk.sizes = sizes;
        walk.y_modes = modes;
        walk.skips = skips;
        walk.segment_ids = segments;
        walk.tx_sizes = tx_sizes;
        walk.uv_modes = uv_modes;
        walk.cdef_idx = cdef;
        walk.cdef_stride = cdef_stride;
        walk.row_start = row_start;
        walk.row_end = row_end;
        walk.col_start = col_start;
        walk.col_end = col_end;
        walk.block = reconstruct_block;
        walk.superblock = start_superblock;
        walk.context = recon;
        walk.q_index = frame.base_q_idx;
        walk.residual = residual;

        status = tiny_av1_walk_tile(&walk);
    }

    if (status != TINYIMG_OK) {
        release(
            recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
            residual
        );

        return status;
    }

    /*
     * The two post-filters, behind the effort tier.
     *
     * Nothing in a still frame references what they write, so a caller who
     * asked for speed gets the frame without them. That is the same lever VP8's
     * in-loop deblocking gave, and there are two of them here.
     */
    int fancy = !opts || opts->effort != TINYIMG_EFFORT_FAST;

    if (fancy) {
        TinyAv1Filter filter;
        tiny_memset(&filter, 0, sizeof(filter));

        filter.sequence = &sequence;
        filter.frame = &frame;
        filter.planes = planes;
        filter.sizes = sizes;
        filter.skips = skips;
        filter.segment_ids = segments;
        filter.delta_lf = recon->delta_lf;
        filter.cdef_idx = cdef;
        filter.cdef_stride = cdef_stride;

        for (uint32_t plane = 0; plane < planes; plane++) {
            filter.plane[plane] = recon->plane[plane];
            filter.stride[plane] = recon->stride[plane];
            filter.width[plane] = recon->width[plane];
            filter.height[plane] = recon->height[plane];
            filter.lf_tx[plane] = recon->lf_tx[plane];
        }

        if (!frame.coded_lossless) tiny_av1_loop_filter(&filter);

        /*
         * CDEF runs only if some 64x64 carries a strength index.
         *
         * The grid starts at -1 and the mode info writes an index only where
         * the frame enables CDEF, so scanning it answers both "is CDEF on" and
         * "does any block want it" at once, and skips a whole frame copy when
         * the answer is no. `cavif` disables CDEF outright and `avifenc` writes
         * zero strength bits, so most files land here.
         */
        int wanted = 0;

        for (size_t i = 0; i < (size_t) cdef_stride * cdef_rows; i++) {
            if (cdef[i] >= 0) {
                wanted = 1;
                break;
            }
        }

        if (wanted && !frame.coded_lossless) {
            uint8_t* filtered[3] = {0, 0, 0};
            int ready = 1;

            for (uint32_t plane = 0; plane < planes; plane++) {
                size_t bytes = (size_t) recon->stride[plane] *
                               ((recon->height[plane] + 7u) & ~7u);

                filtered[plane] = (uint8_t*) tiny_alloc(bytes);

                if (!filtered[plane]) ready = 0;
            }

            if (ready) {
                tiny_av1_cdef(&filter, filtered);

                for (uint32_t plane = 0; plane < planes; plane++) {
                    tiny_free(recon->plane[plane]);
                    recon->plane[plane] = filtered[plane];
                }
            }
            else {
                // a frame that cannot afford the copy keeps the unfiltered
                // picture rather than failing, which is the same degradation
                // the effort tier asks for deliberately
                for (uint32_t plane = 0; plane < planes; plane++) {
                    tiny_free(filtered[plane]);
                }
            }
        }
    }

    uint32_t channels = opts && opts->channels ? opts->channels : 3u;

    if (channels > 4u) channels = 4u;

    /*
     * Allocated through `tiny_image_create` rather than field by field.
     *
     * It is what enforces the pixel and byte ceilings and, less obviously, what
     * leaves every other member of the structure defined: an earlier version of
     * this set the extents and the data and left `meta` holding whatever the
     * caller's stack had, and `tiny_image_destroy` then freed it.
     */
    int created = tiny_image_create(
        image, (region_w + scale - 1u) / scale, (region_h + scale - 1u) / scale,
        (uint8_t) channels
    );

    if (created != TINYIMG_OK) {
        release(
            recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
            residual
        );

        return created;
    }

    image->format = TINYIMG_FORMAT_AVIF;

    write_pixels(recon, image, region_x, region_y, scale, fancy);

    release(
        recon, sizes, modes, skips, segments, tx_sizes, uv_modes, cdef, cdf,
        residual
    );

    return TINYIMG_OK;
}
