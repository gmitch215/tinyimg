#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"

/**
 * @file
 * @brief Specification 5.11.34 through 5.11.39, the residual of one block.
 *
 * The transform blocks of a coded block, and for each one the quantized
 * coefficients. This is 19.0% of the irreducible decode cost measured for this
 * project, second only to the symbol decoder it reads through, and it is what
 * makes a tile stay synchronised: a block that does not skip codes its
 * coefficients immediately after its mode info, so a reader that stops at the
 * mode info is correct for exactly one block.
 *
 * Most of the file is context selection rather than reading. Every coefficient
 * is read against a distribution chosen from the coefficients already read in
 * the same block and from the level context its neighbors left behind, which is
 * why the order inside a transform block matters as much as the order between
 * blocks: the backward pass over the scan builds the magnitudes the next
 * position's context reads.
 *
 * Written for an intra frame. `is_inter` is zero, so the transform tree is
 * unreachable, `TxTypes` never outlives the transform block that wrote it, and
 * the chroma transform type comes from the chroma prediction mode rather than
 * from the luma block underneath.
 */

// #region constants

/** Samples along one side of an mi unit. */
#define AV1_MI_SIZE 4

/** Levels `coeff_base` codes directly, above which `coeff_br` continues. */
#define AV1_NUM_BASE_LEVELS 2

/** How far `coeff_br` can carry a level before the Golomb coding takes over. */
#define AV1_COEFF_BASE_RANGE 12

/** Symbols in the `coeff_br` distribution, the largest of which means more. */
#define AV1_BR_CDF_SIZE 4

/** Contexts for `coeff_base`, and the four at the top reserved for the eob. */
#define AV1_SIG_COEF_CONTEXTS 42
#define AV1_SIG_COEF_CONTEXTS_EOB 4

/** Positions `coeff_base` sums magnitudes over. */
#define AV1_SIG_REF_DIFF_OFFSET_NUM 5

/** Transform classes, which decide the shape of every coefficient context. */
#define AV1_TX_CLASS_2D 0
#define AV1_TX_CLASS_HORIZ 1
#define AV1_TX_CLASS_VERT 2

/** Transform type sets an intra block can code from. */
#define AV1_TX_SET_DCTONLY 0
#define AV1_TX_SET_INTRA_1 1
#define AV1_TX_SET_INTRA_2 2

/**
 * Longest Golomb prefix a conformant stream can write.
 *
 * The level is masked to twenty bits, so the value after the prefix needs at
 * most twenty and the prefix at most twenty-one. The cap is not decoration: a
 * read past the end of a tile returns zero bits forever, and the loop that
 * counts the prefix would never terminate on a truncated block.
 */
#define AV1_GOLOMB_MAX_LENGTH 21

// #region tables

/** Cumulative levels an entry of the level context holds, capped by 9.3. */
#define AV1_MAX_CUL_LEVEL 63

/**
 * The scan a transform block reads its coefficients in.
 *
 * Two pointers rather than one, because the generator emits the narrowest type
 * that holds a table and the two 512-entry scans and the 1024-entry one do not
 * fit in a byte. Exactly one is ever set.
 */
typedef struct {
    const uint8_t* narrow;
    const uint16_t* wide;
} Scan;

static uint32_t scan_at(const Scan* scan, uint32_t c) {
    return scan->narrow ? scan->narrow[c] : scan->wide[c];
}

static Scan default_scan(uint32_t tx_size) {
    Scan scan = {0, 0};

    switch (tx_size) {
        case TINY_AV1_TX_4X4: scan.narrow = tiny_av1_default_scan_4x4; break;
        case TINY_AV1_TX_4X8: scan.narrow = tiny_av1_default_scan_4x8; break;
        case TINY_AV1_TX_8X4: scan.narrow = tiny_av1_default_scan_8x4; break;
        case TINY_AV1_TX_8X8: scan.narrow = tiny_av1_default_scan_8x8; break;
        case TINY_AV1_TX_8X16: scan.narrow = tiny_av1_default_scan_8x16; break;
        case TINY_AV1_TX_16X8: scan.narrow = tiny_av1_default_scan_16x8; break;
        case TINY_AV1_TX_16X16:
            scan.narrow = tiny_av1_default_scan_16x16;
            break;
        case TINY_AV1_TX_16X32: scan.wide = tiny_av1_default_scan_16x32; break;
        case TINY_AV1_TX_32X16: scan.wide = tiny_av1_default_scan_32x16; break;
        case TINY_AV1_TX_4X16: scan.narrow = tiny_av1_default_scan_4x16; break;
        case TINY_AV1_TX_16X4: scan.narrow = tiny_av1_default_scan_16x4; break;
        case TINY_AV1_TX_8X32: scan.narrow = tiny_av1_default_scan_8x32; break;
        case TINY_AV1_TX_32X8: scan.narrow = tiny_av1_default_scan_32x8; break;
        default: scan.wide = tiny_av1_default_scan_32x32; break;
    }

    return scan;
}

static Scan mrow_scan(uint32_t tx_size) {
    Scan scan = {0, 0};

    switch (tx_size) {
        case TINY_AV1_TX_4X4: scan.narrow = tiny_av1_mrow_scan_4x4; break;
        case TINY_AV1_TX_4X8: scan.narrow = tiny_av1_mrow_scan_4x8; break;
        case TINY_AV1_TX_8X4: scan.narrow = tiny_av1_mrow_scan_8x4; break;
        case TINY_AV1_TX_8X8: scan.narrow = tiny_av1_mrow_scan_8x8; break;
        case TINY_AV1_TX_8X16: scan.narrow = tiny_av1_mrow_scan_8x16; break;
        case TINY_AV1_TX_16X8: scan.narrow = tiny_av1_mrow_scan_16x8; break;
        case TINY_AV1_TX_16X16: scan.narrow = tiny_av1_mrow_scan_16x16; break;
        case TINY_AV1_TX_4X16: scan.narrow = tiny_av1_mrow_scan_4x16; break;
        default: scan.narrow = tiny_av1_mrow_scan_16x4; break;
    }

    return scan;
}

static Scan mcol_scan(uint32_t tx_size) {
    Scan scan = {0, 0};

    switch (tx_size) {
        case TINY_AV1_TX_4X4: scan.narrow = tiny_av1_mcol_scan_4x4; break;
        case TINY_AV1_TX_4X8: scan.narrow = tiny_av1_mcol_scan_4x8; break;
        case TINY_AV1_TX_8X4: scan.narrow = tiny_av1_mcol_scan_8x4; break;
        case TINY_AV1_TX_8X8: scan.narrow = tiny_av1_mcol_scan_8x8; break;
        case TINY_AV1_TX_8X16: scan.narrow = tiny_av1_mcol_scan_8x16; break;
        case TINY_AV1_TX_16X8: scan.narrow = tiny_av1_mcol_scan_16x8; break;
        case TINY_AV1_TX_16X16: scan.narrow = tiny_av1_mcol_scan_16x16; break;
        case TINY_AV1_TX_4X16: scan.narrow = tiny_av1_mcol_scan_4x16; break;
        default: scan.narrow = tiny_av1_mcol_scan_16x4; break;
    }

    return scan;
}

/**
 * Which scan a transform block reads, from its size and its type.
 *
 * A transform with the identity on one axis has all its energy along the other,
 * so the scan runs that way; everything else takes the diagonal. The three
 * sizes with a 64 in them are read through a 32-wide scan, because that is
 * where their coefficients are.
 */
static Scan get_scan(uint32_t tx_size, uint32_t tx_type) {
    if (tx_size == TINY_AV1_TX_16X64) {
        Scan scan = {0, tiny_av1_default_scan_16x32};
        return scan;
    }
    if (tx_size == TINY_AV1_TX_64X16) {
        Scan scan = {0, tiny_av1_default_scan_32x16};
        return scan;
    }
    if (tiny_av1_tx_size_sqr_up[tx_size] == TINY_AV1_TX_64X64) {
        Scan scan = {0, tiny_av1_default_scan_32x32};
        return scan;
    }

    if (tx_type == TINY_AV1_IDTX) return default_scan(tx_size);

    if (tx_type == TINY_AV1_V_DCT || tx_type == TINY_AV1_V_ADST ||
        tx_type == TINY_AV1_V_FLIPADST) {
        return mrow_scan(tx_size);
    }

    if (tx_type == TINY_AV1_H_DCT || tx_type == TINY_AV1_H_ADST ||
        tx_type == TINY_AV1_H_FLIPADST) {
        return mcol_scan(tx_size);
    }

    return default_scan(tx_size);
}

/** Which of three classes a transform type falls in. */
static uint32_t tx_class_of(uint32_t tx_type) {
    if (tx_type == TINY_AV1_V_DCT || tx_type == TINY_AV1_V_ADST ||
        tx_type == TINY_AV1_V_FLIPADST) {
        return AV1_TX_CLASS_VERT;
    }
    if (tx_type == TINY_AV1_H_DCT || tx_type == TINY_AV1_H_ADST ||
        tx_type == TINY_AV1_H_FLIPADST) {
        return AV1_TX_CLASS_HORIZ;
    }

    return AV1_TX_CLASS_2D;
}

/**
 * Which of four trained sets of coefficient distributions a frame starts from.
 *
 * The specification's `init_coeff_cdfs` copies one of the four; this indexes
 * into all four instead, because `tiny_av1_cdf_init` copies the whole table and
 * an unused slice costs a memcpy rather than a wrong symbol.
 */
static uint32_t coeff_q_context(uint32_t base_q_idx) {
    if (base_q_idx <= 20u) return 0u;
    if (base_q_idx <= 60u) return 1u;
    if (base_q_idx <= 120u) return 2u;

    return 3u;
}

/**
 * Which transform types a block of this size may code, for an intra frame.
 *
 * The specification's two leading conditions, `txSzSqrUp > TX_32X32` and the
 * intra branch's `txSzSqrUp == TX_32X32`, both answer DCT only, so one
 * comparison covers them.
 */
static uint32_t tx_set_of(const TinyAv1Frame* frame, uint32_t tx_size) {
    if (tiny_av1_tx_size_sqr_up[tx_size] >= TINY_AV1_TX_32X32) {
        return AV1_TX_SET_DCTONLY;
    }
    if (frame->reduced_tx_set) return AV1_TX_SET_INTRA_2;
    if (tiny_av1_tx_size_sqr[tx_size] == TINY_AV1_TX_16X16) {
        return AV1_TX_SET_INTRA_2;
    }

    return AV1_TX_SET_INTRA_1;
}

// #endregion

// #region context

/** One entry of a per-plane context array. */
static uint8_t* above_at(TinyAv1Residual* res, uint32_t plane, uint32_t x4) {
    return &res->above_level[plane * res->above_stride + x4];
}

static uint8_t* above_dc_at(TinyAv1Residual* res, uint32_t plane, uint32_t x4) {
    return &res->above_dc[plane * res->above_stride + x4];
}

static uint8_t* left_at(TinyAv1Residual* res, uint32_t plane, uint32_t y4) {
    return &res->left_level[plane * res->left_stride + y4];
}

static uint8_t* left_dc_at(TinyAv1Residual* res, uint32_t plane, uint32_t y4) {
    return &res->left_dc[plane * res->left_stride + y4];
}

/** How many mi units of a plane the frame covers, which bounds every read. */
static uint32_t plane_max_x4(const TinyAv1Walk* walk, uint32_t plane) {
    uint32_t sub = plane > 0u ? walk->sequence->sub_x : 0u;

    return walk->frame->mi_cols >> sub;
}

static uint32_t plane_max_y4(const TinyAv1Walk* walk, uint32_t plane) {
    uint32_t sub = plane > 0u ? walk->sequence->sub_y : 0u;

    return walk->frame->mi_rows >> sub;
}

/** The residual block size of one plane, which is never BLOCK_INVALID here. */
static uint32_t plane_residual_size(
    const TinyAv1Walk* walk, uint32_t size, uint32_t plane
) {
    uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
    uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

    return tiny_av1_subsampled_size[size][sub_x][sub_y];
}

/**
 * @brief The `all_zero` context, which asks how much the neighbors carried.
 *
 * Two different derivations, and the luma one is not a special case of the
 * chroma one. Luma takes the maximum level either side and buckets it against
 * whether the transform covers the whole block; chroma only asks whether either
 * side carried anything at all, and offsets by three when the block is larger
 * than the transform.
 */
static uint32_t txb_skip_context(
    const TinyAv1Walk* walk, uint32_t plane, uint32_t x4, uint32_t y4,
    uint32_t tx_size, uint32_t w4, uint32_t h4
) {
    TinyAv1Residual* res = walk->residual;
    uint32_t max_x4 = plane_max_x4(walk, plane);
    uint32_t max_y4 = plane_max_y4(walk, plane);

    uint32_t w = tiny_av1_tx_width[tx_size];
    uint32_t h = tiny_av1_tx_height[tx_size];

    uint32_t bsize = plane_residual_size(walk, walk->mode.size, plane);
    uint32_t bw = AV1_MI_SIZE * tiny_av1_num_4x4_blocks_wide[bsize];
    uint32_t bh = AV1_MI_SIZE * tiny_av1_num_4x4_blocks_high[bsize];

    if (plane == 0u) {
        uint32_t top = 0;
        uint32_t left = 0;

        for (uint32_t k = 0; k < w4; k++) {
            if (x4 + k >= max_x4) continue;

            uint32_t value = *above_at(res, plane, x4 + k);
            if (value > top) top = value;
        }
        for (uint32_t k = 0; k < h4; k++) {
            if (y4 + k >= max_y4) continue;

            uint32_t value = *left_at(res, plane, y4 + k);
            if (value > left) left = value;
        }

        // the specification clamps both to 255 here; a level context entry is
        // capped at 63 where it is written, so the clamp cannot fire
        if (bw == w && bh == h) return 0u;
        if (top == 0u && left == 0u) return 1u;
        if (top == 0u || left == 0u) {
            return 2u + ((top > left ? top : left) > 3u ? 1u : 0u);
        }
        if ((top > left ? top : left) <= 3u) return 4u;
        if ((top < left ? top : left) <= 3u) return 5u;

        return 6u;
    }

    uint32_t above = 0;
    uint32_t left = 0;

    for (uint32_t i = 0; i < w4; i++) {
        if (x4 + i >= max_x4) continue;

        above |= *above_at(res, plane, x4 + i);
        above |= *above_dc_at(res, plane, x4 + i);
    }
    for (uint32_t i = 0; i < h4; i++) {
        if (y4 + i >= max_y4) continue;

        left |= *left_at(res, plane, y4 + i);
        left |= *left_dc_at(res, plane, y4 + i);
    }

    uint32_t ctx = (above != 0u ? 1u : 0u) + (left != 0u ? 1u : 0u) + 7u;

    if (bw * bh > w * h) ctx += 3u;

    return ctx;
}

/** The dc sign context, which is the balance of the neighbors' dc signs. */
static uint32_t dc_sign_context(
    const TinyAv1Walk* walk, uint32_t plane, uint32_t x4, uint32_t y4,
    uint32_t w4, uint32_t h4
) {
    TinyAv1Residual* res = walk->residual;
    uint32_t max_x4 = plane_max_x4(walk, plane);
    uint32_t max_y4 = plane_max_y4(walk, plane);
    int32_t sum = 0;

    for (uint32_t k = 0; k < w4; k++) {
        if (x4 + k >= max_x4) continue;

        uint32_t sign = *above_dc_at(res, plane, x4 + k);
        if (sign == 1u)
            sum--;
        else if (sign == 2u)
            sum++;
    }
    for (uint32_t k = 0; k < h4; k++) {
        if (y4 + k >= max_y4) continue;

        uint32_t sign = *left_dc_at(res, plane, y4 + k);
        if (sign == 1u)
            sum--;
        else if (sign == 2u)
            sum++;
    }

    if (sum < 0) return 1u;
    if (sum > 0) return 2u;

    return 0u;
}

/**
 * @brief The `coeff_base` and `coeff_base_eob` context.
 *
 * For the last coefficient in scan order the context is the scan position
 * alone, bucketed against the block's area. For every other one it sums the
 * magnitudes of up to five already-decoded neighbors and offsets that by where
 * the coefficient sits, which is a different offset table for a diagonal scan
 * than for a row or column one.
 */
static uint32_t coeff_base_context(
    const TinyAv1Residual* res, uint32_t tx_size, uint32_t tx_class,
    uint32_t pos, uint32_t c, int is_eob
) {
    uint32_t adjusted = tiny_av1_adjusted_tx_size[tx_size];
    uint32_t bwl = tiny_av1_tx_width_log2[adjusted];
    uint32_t width = 1u << bwl;
    uint32_t height = tiny_av1_tx_height[adjusted];

    if (is_eob) {
        if (c == 0u) return AV1_SIG_COEF_CONTEXTS - 4u;
        if (c <= (height << bwl) / 8u) return AV1_SIG_COEF_CONTEXTS - 3u;
        if (c <= (height << bwl) / 4u) return AV1_SIG_COEF_CONTEXTS - 2u;

        return AV1_SIG_COEF_CONTEXTS - 1u;
    }

    uint32_t row = pos >> bwl;
    uint32_t col = pos - (row << bwl);
    uint32_t mag = 0;

    for (uint32_t idx = 0; idx < AV1_SIG_REF_DIFF_OFFSET_NUM; idx++) {
        uint32_t ref_row = row + tiny_av1_sig_ref_diff_offset[tx_class][idx][0];
        uint32_t ref_col = col + tiny_av1_sig_ref_diff_offset[tx_class][idx][1];

        if (ref_row >= height || ref_col >= width) continue;

        int32_t level = res->quant[(ref_row << bwl) + ref_col];
        uint32_t abs = (uint32_t) (level < 0 ? -level : level);

        mag += abs < 3u ? abs : 3u;
    }

    uint32_t ctx = (mag + 1u) >> 1;
    if (ctx > 4u) ctx = 4u;

    if (tx_class == AV1_TX_CLASS_2D) {
        if (row == 0u && col == 0u) return 0u;

        return ctx +
               tiny_av1_coeff_base_ctx_offset[tx_size][row < 4u ? row : 4u]
                                             [col < 4u ? col : 4u];
    }

    uint32_t along = tx_class == AV1_TX_CLASS_VERT ? row : col;

    return ctx + tiny_av1_coeff_base_pos_ctx_offset[along < 2u ? along : 2u];
}

/**
 * @brief The `coeff_br` context, which is three neighbors rather than five.
 *
 * The three offsets and the position buckets both depend on the transform
 * class, and the dc coefficient takes the magnitude alone with no offset.
 */
static uint32_t coeff_br_context(
    const TinyAv1Residual* res, uint32_t tx_size, uint32_t tx_class,
    uint32_t pos
) {
    uint32_t adjusted = tiny_av1_adjusted_tx_size[tx_size];
    uint32_t bwl = tiny_av1_tx_width_log2[adjusted];
    uint32_t txw = tiny_av1_tx_width[adjusted];
    uint32_t txh = tiny_av1_tx_height[adjusted];

    uint32_t row = pos >> bwl;
    uint32_t col = pos - (row << bwl);
    uint32_t mag = 0;

    for (uint32_t idx = 0; idx < 3u; idx++) {
        uint32_t ref_row =
            row + tiny_av1_mag_ref_offset_with_tx_class[tx_class][idx][0];
        uint32_t ref_col =
            col + tiny_av1_mag_ref_offset_with_tx_class[tx_class][idx][1];

        if (ref_row >= txh || ref_col >= (1u << bwl)) continue;

        uint32_t level = (uint32_t) res->quant[ref_row * txw + ref_col];
        uint32_t cap = AV1_COEFF_BASE_RANGE + AV1_NUM_BASE_LEVELS + 1u;

        mag += level < cap ? level : cap;
    }

    mag = (mag + 1u) >> 1;
    if (mag > 6u) mag = 6u;

    if (pos == 0u) return mag;

    if (tx_class == AV1_TX_CLASS_2D) {
        return row < 2u && col < 2u ? mag + 7u : mag + 14u;
    }
    if (tx_class == AV1_TX_CLASS_HORIZ) return col == 0u ? mag + 7u : mag + 14u;

    return row == 0u ? mag + 7u : mag + 14u;
}

// #endregion

// #region coefficients

/**
 * @brief The transform type a luma block codes, when it codes one at all.
 *
 * Coded per transform block rather than per coded block, and only for luma: the
 * chroma type comes from the chroma prediction mode. A lossless block reaches
 * this with a quantizer index of zero, which is what makes it lossless, so the
 * condition below already refuses to read a symbol for it.
 */
static uint32_t read_tx_type(TinyAv1Walk* walk, uint32_t tx_size) {
    uint32_t set = tx_set_of(walk->frame, tx_size);
    uint32_t qindex = walk->frame->segmentation_enabled
                          ? walk->frame->seg_qindex[walk->mode.segment_id]
                          : walk->frame->base_q_idx;

    if (set == AV1_TX_SET_DCTONLY || qindex == 0u) return TINY_AV1_DCT_DCT;

    uint32_t dir =
        walk->mode.use_filter_intra
            ? tiny_av1_filter_intra_mode_to_intra_dir[walk->mode
                                                          .filter_intra_mode]
            : walk->mode.y_mode;
    uint32_t sqr = tiny_av1_tx_size_sqr[tx_size];

    if (set == AV1_TX_SET_INTRA_1) {
        uint32_t symbol = tiny_av1_symbol_read(
            walk->symbol, walk->cdf->intra_tx_type_set1[sqr][dir], 7u
        );

        return tiny_av1_tx_type_intra_inv_set1[symbol];
    }

    uint32_t symbol = tiny_av1_symbol_read(
        walk->symbol, walk->cdf->intra_tx_type_set2[sqr][dir], 5u
    );

    return tiny_av1_tx_type_intra_inv_set2[symbol];
}

/**
 * @brief The type the transform actually runs with.
 *
 * For luma it is what the block just coded. For chroma it is the type the
 * chroma prediction mode implies, dropped to DCT when the block's size does not
 * offer it, which is the one place a type is silently replaced rather than
 * refused.
 */
static uint32_t compute_tx_type(
    const TinyAv1Walk* walk, uint32_t plane, uint32_t tx_size, uint32_t luma
) {
    if (walk->mode.lossless ||
        tiny_av1_tx_size_sqr_up[tx_size] > TINY_AV1_TX_32X32) {
        return TINY_AV1_DCT_DCT;
    }
    if (plane == 0u) return luma;

    uint32_t set = tx_set_of(walk->frame, tx_size);
    uint32_t type = tiny_av1_mode_to_txfm[walk->mode.uv_mode];

    if (!tiny_av1_tx_type_in_set_intra[set][type]) return TINY_AV1_DCT_DCT;

    return type;
}

/** Reads the end of block position, whose distribution depends on the area. */
static uint32_t read_eob_pt(
    TinyAv1Walk* walk, uint32_t q, uint32_t multisize, uint32_t ptype,
    uint32_t ctx
) {
    TinyAv1Cdf* cdf = walk->cdf;

    switch (multisize) {
        case 0:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_16[q][ptype][ctx], 5u
                        );
        case 1:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_32[q][ptype][ctx], 6u
                        );
        case 2:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_64[q][ptype][ctx], 7u
                        );
        case 3:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_128[q][ptype][ctx], 8u
                        );
        case 4:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_256[q][ptype][ctx], 9u
                        );
        case 5:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_512[q][ptype], 10u
                        );
        default:
            return 1u + tiny_av1_symbol_read(
                            walk->symbol, cdf->eob_pt_1024[q][ptype], 11u
                        );
    }
}

/**
 * @brief Reads one transform block's coefficients.
 *
 * The three passes are the specification's and they cannot be merged. The end
 * of block position comes first, then the levels backwards along the scan,
 * because each level's context reads the magnitudes of the positions after it,
 * and then the signs and the Golomb tails forwards, because the dc sign is
 * coded against a context the neighbors set and the rest are plain bits.
 */
static int read_coeffs(
    TinyAv1Walk* walk, uint32_t plane, uint32_t start_x, uint32_t start_y,
    uint32_t tx_size, uint32_t* out_eob, uint32_t* out_type
) {
    TinyAv1Residual* res = walk->residual;
    TinyAv1Cdf* cdf = walk->cdf;

    uint32_t x4 = start_x >> 2;
    uint32_t y4 = start_y >> 2;
    uint32_t w4 = tiny_av1_tx_width[tx_size] >> 2;
    uint32_t h4 = tiny_av1_tx_height[tx_size] >> 2;

    uint32_t sqr = tiny_av1_tx_size_sqr[tx_size];
    uint32_t sqr_up = tiny_av1_tx_size_sqr_up[tx_size];
    uint32_t tx_ctx = (sqr + sqr_up + 1u) >> 1;
    uint32_t ptype = plane > 0u ? 1u : 0u;
    uint32_t q = coeff_q_context(walk->frame->base_q_idx);

    /*
     * The coefficients live in the transform's top left 32x32 whatever its real
     * size, so the array is `tw` by `th` and `segEob` is exactly their product.
     * The two sizes with a 64 on one axis only carry half of that.
     */
    uint32_t tw = tiny_av1_tx_width[tx_size];
    uint32_t th = tiny_av1_tx_height[tx_size];

    if (tw > 32u) tw = 32u;
    if (th > 32u) th = 32u;

    uint32_t seg_eob = tw * th;

    tiny_memset(res->quant, 0, (size_t) seg_eob * sizeof(res->quant[0]));

    uint32_t eob = 0;
    uint32_t cul_level = 0;
    uint32_t dc_category = 0;
    uint32_t tx_type = TINY_AV1_DCT_DCT;

    uint32_t skip_ctx = txb_skip_context(walk, plane, x4, y4, tx_size, w4, h4);
    uint32_t all_zero = tiny_av1_symbol_read(
        walk->symbol, cdf->txb_skip[q][tx_ctx][skip_ctx], 2u
    );

    if (!all_zero) {
        uint32_t luma = TINY_AV1_DCT_DCT;

        if (plane == 0u) luma = read_tx_type(walk, tx_size);

        tx_type = compute_tx_type(walk, plane, tx_size, luma);

        Scan scan = get_scan(tx_size, tx_type);
        uint32_t tx_class = tx_class_of(tx_type);

        uint32_t log2w = tiny_av1_tx_width_log2[tx_size];
        uint32_t log2h = tiny_av1_tx_height_log2[tx_size];
        uint32_t multisize =
            (log2w < 5u ? log2w : 5u) + (log2h < 5u ? log2h : 5u) - 4u;

        uint32_t eob_ctx = tx_class == AV1_TX_CLASS_2D ? 0u : 1u;
        uint32_t eob_pt = read_eob_pt(walk, q, multisize, ptype, eob_ctx);

        eob = eob_pt < 2u ? eob_pt : (1u << (eob_pt - 2u)) + 1u;

        if (eob_pt >= 3u) {
            uint32_t shift = eob_pt - 3u;
            uint32_t extra = tiny_av1_symbol_read(
                walk->symbol, cdf->eob_extra[q][tx_ctx][ptype][eob_pt - 3u], 2u
            );

            if (extra) eob += 1u << shift;

            for (uint32_t i = 1; i < eob_pt - 2u; i++) {
                shift = eob_pt - 2u - 1u - i;

                if (tiny_av1_symbol_bit(walk->symbol)) eob += 1u << shift;
            }
        }

        // a corrupt stream can code an end of block past the transform, and
        // every write below is indexed by the scan it would run off
        if (eob > seg_eob) return TINYIMG_ERR_CORRUPT;

        for (int32_t c = (int32_t) eob - 1; c >= 0; c--) {
            uint32_t pos = scan_at(&scan, (uint32_t) c);
            uint32_t level;

            if ((uint32_t) c == eob - 1u) {
                uint32_t ctx = coeff_base_context(
                                   res, tx_size, tx_class, pos, (uint32_t) c, 1
                               ) -
                               AV1_SIG_COEF_CONTEXTS +
                               AV1_SIG_COEF_CONTEXTS_EOB;

                level = 1u + tiny_av1_symbol_read(
                                 walk->symbol,
                                 cdf->coeff_base_eob[q][tx_ctx][ptype][ctx], 3u
                             );
            }
            else {
                uint32_t ctx = coeff_base_context(
                    res, tx_size, tx_class, pos, (uint32_t) c, 0
                );
                level = tiny_av1_symbol_read(
                    walk->symbol, cdf->coeff_base[q][tx_ctx][ptype][ctx], 4u
                );
            }

            if (level > AV1_NUM_BASE_LEVELS) {
                uint32_t br_ctx = coeff_br_context(res, tx_size, tx_class, pos);
                uint32_t br_size = tx_ctx < (uint32_t) TINY_AV1_TX_32X32
                                       ? tx_ctx
                                       : (uint32_t) TINY_AV1_TX_32X32;
                uint16_t* br = cdf->coeff_br[q][br_size][ptype][br_ctx];

                for (uint32_t idx = 0;
                     idx < AV1_COEFF_BASE_RANGE / (AV1_BR_CDF_SIZE - 1u);
                     idx++) {
                    uint32_t more =
                        tiny_av1_symbol_read(walk->symbol, br, AV1_BR_CDF_SIZE);

                    level += more;

                    if (more < AV1_BR_CDF_SIZE - 1u) break;
                }
            }

            res->quant[pos] = (int32_t) level;
        }

        for (uint32_t c = 0; c < eob; c++) {
            uint32_t pos = scan_at(&scan, c);
            uint32_t sign = 0;

            if (res->quant[pos] != 0) {
                if (c == 0u) {
                    uint32_t ctx = dc_sign_context(walk, plane, x4, y4, w4, h4);

                    sign = tiny_av1_symbol_read(
                        walk->symbol, cdf->dc_sign[q][ptype][ctx], 2u
                    );
                }
                else {
                    sign = tiny_av1_symbol_bit(walk->symbol);
                }
            }

            uint32_t level = (uint32_t) res->quant[pos];

            if (level > AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE) {
                uint32_t length = 0;
                uint32_t done = 0;

                while (!done) {
                    length++;

                    // the prefix is unary and a read past the tile returns
                    // zeros, so without the cap a truncated block never ends
                    if (length > AV1_GOLOMB_MAX_LENGTH) {
                        return TINYIMG_ERR_CORRUPT;
                    }

                    done = tiny_av1_symbol_bit(walk->symbol);
                }

                uint32_t value = 1;

                for (int32_t i = (int32_t) length - 2; i >= 0; i--) {
                    value = (value << 1) | tiny_av1_symbol_bit(walk->symbol);
                }

                level = value + AV1_COEFF_BASE_RANGE + AV1_NUM_BASE_LEVELS;
            }

            if (pos == 0u && level > 0u) dc_category = sign ? 1u : 2u;

            level &= 0xFFFFFu;
            cul_level += level;

            res->quant[pos] = sign ? -(int32_t) level : (int32_t) level;
        }

        if (cul_level > AV1_MAX_CUL_LEVEL) cul_level = AV1_MAX_CUL_LEVEL;
    }

    /*
     * The context the next blocks read, written across the whole transform
     * whether or not it fits inside the frame. The arrays carry
     * TINY_AV1_CONTEXT_PAD entries past the frame for exactly this, and every
     * read of them is guarded by the frame's own extent instead.
     */
    for (uint32_t i = 0; i < w4; i++) {
        *above_at(res, plane, x4 + i) = (uint8_t) cul_level;
        *above_dc_at(res, plane, x4 + i) = (uint8_t) dc_category;
    }
    for (uint32_t i = 0; i < h4; i++) {
        *left_at(res, plane, y4 + i) = (uint8_t) cul_level;
        *left_dc_at(res, plane, y4 + i) = (uint8_t) dc_category;
    }

    *out_eob = eob;
    *out_type = tx_type;

    return TINYIMG_OK;
}

// #endregion

// #region writing

/**
 * @brief The end of block position that codes a given end of block.
 *
 * The inverse of the reader's derivation: position `p` covers the end of block
 * range `[(1 << (p - 2)) + 1, 1 << (p - 1)]` for `p` above one, so the position
 * is two plus the floor log two of one less than the count.
 */
static uint32_t eob_position(uint32_t eob) {
    if (eob <= 1u) return eob;

    uint32_t log2 = 0;
    uint32_t value = eob - 1u;

    while (value > 1u) {
        value >>= 1;
        log2++;
    }

    return log2 + 2u;
}

/** Writes the end of block position, whose distribution the area chooses. */
static void write_eob_pt(
    TinyAv1SymbolEnc* enc, TinyAv1Cdf* cdf, uint32_t q, uint32_t multisize,
    uint32_t ptype, uint32_t ctx, uint32_t position
) {
    uint32_t symbol = position - 1u;

    switch (multisize) {
        case 0:
            tiny_av1_symbol_write(
                enc, cdf->eob_pt_16[q][ptype][ctx], 5u, symbol
            );
            return;
        case 1:
            tiny_av1_symbol_write(
                enc, cdf->eob_pt_32[q][ptype][ctx], 6u, symbol
            );
            return;
        case 2:
            tiny_av1_symbol_write(
                enc, cdf->eob_pt_64[q][ptype][ctx], 7u, symbol
            );
            return;
        case 3:
            tiny_av1_symbol_write(
                enc, cdf->eob_pt_128[q][ptype][ctx], 8u, symbol
            );
            return;
        case 4:
            tiny_av1_symbol_write(
                enc, cdf->eob_pt_256[q][ptype][ctx], 9u, symbol
            );
            return;
        case 5:
            tiny_av1_symbol_write(enc, cdf->eob_pt_512[q][ptype], 10u, symbol);
            return;
        default:
            tiny_av1_symbol_write(enc, cdf->eob_pt_1024[q][ptype], 11u, symbol);
            return;
    }
}

/**
 * @brief Which symbol codes a transform type, for the intra sets.
 *
 * The reader maps a symbol through an inversion table; this searches the same
 * table rather than carrying a second one, because a table written the other
 * way round is a second copy of the same fact and can disagree with it.
 */
static uint32_t tx_type_symbol(uint32_t set, uint32_t type, uint32_t* count) {
    const uint8_t* table = set == AV1_TX_SET_INTRA_1
                               ? tiny_av1_tx_type_intra_inv_set1
                               : tiny_av1_tx_type_intra_inv_set2;

    *count = set == AV1_TX_SET_INTRA_1 ? 7u : 5u;

    for (uint32_t i = 0; i < *count; i++) {
        if (table[i] == type) return i;
    }

    return 0;
}

int tiny_av1_write_coeffs(
    TinyAv1Residual* res, TinyAv1SymbolEnc* enc, TinyAv1Cdf* cdf,
    const TinyAv1Frame* frame, const TinyAv1Sequence* sequence, uint32_t plane,
    uint32_t x, uint32_t y, uint32_t tx_size, uint32_t tx_type, uint32_t y_mode,
    const int32_t* levels, uint32_t eob
) {
    if (!res || !enc || !cdf || !frame || !sequence) return TINYIMG_ERR_NULL;

    /*
     * A walk is built here rather than passed in, because every context
     * derivation in this file reads one and the encoder has no walk of its own:
     * it visits blocks in the same order without a partition tree to drive it.
     * Only the fields the derivations touch are set.
     */
    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    walk.sequence = sequence;
    walk.frame = frame;
    walk.residual = res;
    walk.mode.size = TINY_AV1_BLOCK_8X8;
    walk.mode.row = (y << (plane > 0u ? sequence->sub_y : 0u)) >> 2;
    walk.mode.col = (x << (plane > 0u ? sequence->sub_x : 0u)) >> 2;
    walk.mode.uv_mode = TINY_AV1_DC_PRED;
    walk.mode.has_chroma = sequence->planes > 1u;

    uint32_t x4 = x >> 2;
    uint32_t y4 = y >> 2;
    uint32_t w4 = tiny_av1_tx_width[tx_size] >> 2;
    uint32_t h4 = tiny_av1_tx_height[tx_size] >> 2;

    uint32_t sqr = tiny_av1_tx_size_sqr[tx_size];
    uint32_t sqr_up = tiny_av1_tx_size_sqr_up[tx_size];
    uint32_t tx_ctx = (sqr + sqr_up + 1u) >> 1;
    uint32_t ptype = plane > 0u ? 1u : 0u;
    uint32_t q = coeff_q_context(frame->base_q_idx);

    uint32_t tw = tiny_av1_tx_width[tx_size];
    uint32_t th = tiny_av1_tx_height[tx_size];

    if (tw > 32u) tw = 32u;
    if (th > 32u) th = 32u;

    uint32_t skip_ctx = txb_skip_context(&walk, plane, x4, y4, tx_size, w4, h4);

    tiny_av1_symbol_write(
        enc, cdf->txb_skip[q][tx_ctx][skip_ctx], 2u, eob == 0u ? 1u : 0u
    );

    uint32_t cul_level = 0;
    uint32_t dc_category = 0;

    if (eob > 0u) {
        if (plane == 0u) {
            uint32_t set = tx_set_of(frame, tx_size);
            uint32_t qindex = frame->segmentation_enabled ? frame->seg_qindex[0]
                                                          : frame->base_q_idx;

            if (set != AV1_TX_SET_DCTONLY && qindex > 0u) {
                uint32_t count = 0;
                uint32_t symbol = tx_type_symbol(set, tx_type, &count);
                uint16_t* row = set == AV1_TX_SET_INTRA_1
                                    ? cdf->intra_tx_type_set1[sqr][y_mode]
                                    : cdf->intra_tx_type_set2[sqr][y_mode];

                tiny_av1_symbol_write(enc, row, count, symbol);
            }
        }

        Scan scan = get_scan(tx_size, tx_type);
        uint32_t tx_class = tx_class_of(tx_type);

        uint32_t log2w = tiny_av1_tx_width_log2[tx_size];
        uint32_t log2h = tiny_av1_tx_height_log2[tx_size];
        uint32_t multisize =
            (log2w < 5u ? log2w : 5u) + (log2h < 5u ? log2h : 5u) - 4u;

        uint32_t eob_ctx = tx_class == AV1_TX_CLASS_2D ? 0u : 1u;
        uint32_t position = eob_position(eob);

        write_eob_pt(enc, cdf, q, multisize, ptype, eob_ctx, position);

        if (position >= 3u) {
            uint32_t extra = eob - ((1u << (position - 2u)) + 1u);
            uint32_t width = position - 2u;

            tiny_av1_symbol_write(
                enc, cdf->eob_extra[q][tx_ctx][ptype][position - 3u], 2u,
                (extra >> (width - 1u)) & 1u
            );

            for (uint32_t i = 1; i < width; i++) {
                tiny_av1_symbol_write_bit(
                    enc, (extra >> (width - 1u - i)) & 1u
                );
            }
        }

        /*
         * The magnitudes backwards along the scan, into `res->quant`.
         *
         * The buffer is the parse's, and it is filled the same way for the same
         * reason: every context reads the magnitudes of the positions after it
         * in scan order, so they have to be in place before the next one is
         * written.
         */
        tiny_memset(res->quant, 0, (size_t) tw * th * sizeof(res->quant[0]));

        for (int32_t c = (int32_t) eob - 1; c >= 0; c--) {
            uint32_t pos = scan_at(&scan, (uint32_t) c);
            int32_t signed_level = levels[pos];
            uint32_t level =
                (uint32_t) (signed_level < 0 ? -signed_level : signed_level);

            uint32_t capped = level;

            if (capped > AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE + 1u) {
                capped = AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE + 1u;
            }

            if ((uint32_t) c == eob - 1u) {
                uint32_t ctx = coeff_base_context(
                                   res, tx_size, tx_class, pos, (uint32_t) c, 1
                               ) -
                               AV1_SIG_COEF_CONTEXTS +
                               AV1_SIG_COEF_CONTEXTS_EOB;
                uint32_t base = capped < 3u ? capped : 3u;

                // three symbols for levels one to three, so the symbol is one
                // less than the level rather than the level
                tiny_av1_symbol_write(
                    enc, cdf->coeff_base_eob[q][tx_ctx][ptype][ctx], 3u,
                    base - 1u
                );
            }
            else {
                uint32_t ctx = coeff_base_context(
                    res, tx_size, tx_class, pos, (uint32_t) c, 0
                );
                uint32_t base = capped < 3u ? capped : 3u;

                tiny_av1_symbol_write(
                    enc, cdf->coeff_base[q][tx_ctx][ptype][ctx], 4u, base
                );
            }

            if (capped > AV1_NUM_BASE_LEVELS) {
                uint32_t br_ctx = coeff_br_context(res, tx_size, tx_class, pos);
                uint32_t br_size = tx_ctx < (uint32_t) TINY_AV1_TX_32X32
                                       ? tx_ctx
                                       : (uint32_t) TINY_AV1_TX_32X32;
                uint16_t* br = cdf->coeff_br[q][br_size][ptype][br_ctx];
                uint32_t remaining = capped - AV1_NUM_BASE_LEVELS - 1u;

                for (uint32_t idx = 0;
                     idx < AV1_COEFF_BASE_RANGE / (AV1_BR_CDF_SIZE - 1u);
                     idx++) {
                    uint32_t step = remaining < AV1_BR_CDF_SIZE - 1u
                                        ? remaining
                                        : AV1_BR_CDF_SIZE - 1u;

                    tiny_av1_symbol_write(enc, br, AV1_BR_CDF_SIZE, step);

                    remaining -= step;

                    if (step < AV1_BR_CDF_SIZE - 1u) break;
                }
            }

            /*
             * The capped magnitude, not the true one.
             *
             * Every context in this pass reads the magnitudes already written,
             * and the reader has only the capped value at this point: the
             * Golomb tail carrying the rest is not read until the pass after.
             * Storing the true level here would select a different
             * distribution from the one the reader selects.
             */
            res->quant[pos] = (int32_t) capped;
        }

        for (uint32_t c = 0; c < eob; c++) {
            uint32_t pos = scan_at(&scan, c);
            int32_t signed_level = levels[pos];
            uint32_t level =
                (uint32_t) (signed_level < 0 ? -signed_level : signed_level);
            uint32_t sign = signed_level < 0 ? 1u : 0u;

            if (level != 0u) {
                if (c == 0u) {
                    uint32_t ctx =
                        dc_sign_context(&walk, plane, x4, y4, w4, h4);

                    tiny_av1_symbol_write(
                        enc, cdf->dc_sign[q][ptype][ctx], 2u, sign
                    );
                }
                else {
                    tiny_av1_symbol_write_bit(enc, sign);
                }
            }

            if (level > AV1_NUM_BASE_LEVELS + AV1_COEFF_BASE_RANGE) {
                /*
                 * The Golomb tail, whose prefix is the length in unary.
                 *
                 * The reader counts zero bits to a terminating one and then
                 * reads one fewer data bit, so the value written here is the
                 * level less the range the base and the continuations already
                 * covered, and its own leading one is implied.
                 */
                uint32_t value =
                    level - AV1_COEFF_BASE_RANGE - AV1_NUM_BASE_LEVELS;
                uint32_t length = 0;
                uint32_t probe = value;

                while (probe > 0u) {
                    probe >>= 1;
                    length++;
                }

                for (uint32_t i = 0; i + 1u < length; i++) {
                    tiny_av1_symbol_write_bit(enc, 0u);
                }

                tiny_av1_symbol_write_bit(enc, 1u);

                for (int32_t i = (int32_t) length - 2; i >= 0; i--) {
                    tiny_av1_symbol_write_bit(enc, (value >> i) & 1u);
                }
            }

            if (pos == 0u && level > 0u) dc_category = sign ? 1u : 2u;

            cul_level += level & 0xFFFFFu;
        }

        if (cul_level > AV1_MAX_CUL_LEVEL) cul_level = AV1_MAX_CUL_LEVEL;
    }

    for (uint32_t i = 0; i < w4; i++) {
        *above_at(res, plane, x4 + i) = (uint8_t) cul_level;
        *above_dc_at(res, plane, x4 + i) = (uint8_t) dc_category;
    }
    for (uint32_t i = 0; i < h4; i++) {
        *left_at(res, plane, y4 + i) = (uint8_t) cul_level;
        *left_dc_at(res, plane, y4 + i) = (uint8_t) dc_category;
    }

    return TINYIMG_OK;
}

// #endregion

// #region residual

/** The transform one plane of a block runs, from the block's own size. */
static uint32_t plane_tx_size(
    const TinyAv1Walk* walk, uint32_t plane, uint32_t tx_size
) {
    if (plane == 0u) return tx_size;

    uint32_t size = plane_residual_size(walk, walk->mode.size, plane);
    uint32_t uv = tiny_av1_max_tx_size_rect[size];

    if (tiny_av1_tx_width[uv] == 64u || tiny_av1_tx_height[uv] == 64u) {
        if (tiny_av1_tx_width[uv] == 16u) return TINY_AV1_TX_16X32;
        if (tiny_av1_tx_height[uv] == 16u) return TINY_AV1_TX_32X16;

        return TINY_AV1_TX_32X32;
    }

    return uv;
}

/**
 * @brief Clears the level context a skipped block leaves behind.
 *
 * `reset_block_context`, called once per coded block before its residual
 * rather than per transform block. It clears the block's own extent, which for
 * a block hanging over the frame's edge is wider than the transform blocks that
 * are actually read; the padding on both arrays is what absorbs that.
 */
static void reset_block_context(TinyAv1Walk* walk) {
    TinyAv1Residual* res = walk->residual;
    const TinyAv1Mode* mode = &walk->mode;

    uint32_t wide = tiny_av1_num_4x4_blocks_wide[mode->size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[mode->size];
    uint32_t planes = 1u + (mode->has_chroma ? 2u : 0u);

    for (uint32_t plane = 0; plane < planes; plane++) {
        uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
        uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

        for (uint32_t i = mode->col >> sub_x; i < (mode->col + wide) >> sub_x;
             i++) {
            *above_at(res, plane, i) = 0u;
            *above_dc_at(res, plane, i) = 0u;
        }
        for (uint32_t i = mode->row >> sub_y; i < (mode->row + high) >> sub_y;
             i++) {
            *left_at(res, plane, i) = 0u;
            *left_dc_at(res, plane, i) = 0u;
        }
    }
}

/** Reads one transform block, then hands it to the sink. */
static int transform_block(
    TinyAv1Walk* walk, uint32_t plane, uint32_t base_x, uint32_t base_y,
    uint32_t tx_size, uint32_t x, uint32_t y
) {
    uint32_t start_x = base_x + AV1_MI_SIZE * x;
    uint32_t start_y = base_y + AV1_MI_SIZE * y;

    uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
    uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

    uint32_t max_x = (walk->frame->mi_cols * AV1_MI_SIZE) >> sub_x;
    uint32_t max_y = (walk->frame->mi_rows * AV1_MI_SIZE) >> sub_y;

    if (start_x >= max_x || start_y >= max_y) return TINYIMG_OK;

    uint32_t eob = 0;
    uint32_t tx_type = TINY_AV1_DCT_DCT;

    if (!walk->mode.skip) {
        int status =
            read_coeffs(walk, plane, start_x, start_y, tx_size, &eob, &tx_type);

        if (status != TINYIMG_OK) return status;
    }

    if (!walk->residual->block) return TINYIMG_OK;

    uint32_t stride = tiny_av1_tx_width[tx_size];
    if (stride > 32u) stride = 32u;

    TinyAv1TxBlock block;

    block.plane = plane;
    block.x = start_x;
    block.y = start_y;
    block.tx_size = (uint8_t) tx_size;
    block.tx_type = (uint8_t) tx_type;
    block.lossless = walk->mode.lossless;
    block.eob = eob;
    block.stride = stride;

    /*
     * NULL when there is nothing to read, rather than a pointer at the previous
     * transform block's levels.
     *
     * A skipped block never enters `read_coeffs`, so the buffer still holds
     * whatever the last transform block left in it. A caller that added that to
     * a block with no residual would get one stale block of picture, and the
     * pointer is what makes that unreachable rather than a rule to remember.
     */
    block.quant = eob > 0u ? walk->residual->quant : 0;

    return walk->residual->block(walk, &block);
}

void tiny_av1_residual_clear_above(TinyAv1Walk* walk) {
    TinyAv1Residual* res = walk->residual;
    size_t bytes = (size_t) res->above_stride * 3u;

    tiny_memset(res->above_level, 0, bytes);
    tiny_memset(res->above_dc, 0, bytes);
}

void tiny_av1_residual_clear_left(TinyAv1Walk* walk) {
    TinyAv1Residual* res = walk->residual;
    size_t bytes = (size_t) res->left_stride * 3u;

    tiny_memset(res->left_level, 0, bytes);
    tiny_memset(res->left_dc, 0, bytes);
}

int tiny_av1_read_residual(TinyAv1Walk* walk) {
    if (!walk || !walk->residual) return TINYIMG_ERR_NULL;

    TinyAv1Residual* res = walk->residual;

    if (!res->above_level || !res->above_dc || !res->left_level ||
        !res->left_dc) {
        return TINYIMG_ERR_NULL;
    }

    const TinyAv1Mode* mode = &walk->mode;

    /*
     * A block wider or taller than 64 samples is read as a grid of 64x64
     * chunks, each with its own planes, rather than plane by plane across the
     * whole block. Nothing below 128x128 has more than one chunk, so the loop
     * runs once for every block a 64x64 superblock can hold.
     */
    uint32_t wide_chunks = tiny_av1_num_4x4_blocks_wide[mode->size] >> 4;
    uint32_t high_chunks = tiny_av1_num_4x4_blocks_high[mode->size] >> 4;

    if (wide_chunks < 1u) wide_chunks = 1u;
    if (high_chunks < 1u) high_chunks = 1u;

    uint32_t chunk_size = wide_chunks > 1u || high_chunks > 1u
                              ? (uint32_t) TINY_AV1_BLOCK_64X64
                              : (uint32_t) mode->size;

    uint32_t planes = 1u + (mode->has_chroma ? 2u : 0u);

    if (mode->skip) reset_block_context(walk);

    for (uint32_t chunk_y = 0; chunk_y < high_chunks; chunk_y++) {
        for (uint32_t chunk_x = 0; chunk_x < wide_chunks; chunk_x++) {
            for (uint32_t plane = 0; plane < planes; plane++) {
                uint32_t tx_size =
                    mode->lossless ? (uint32_t) TINY_AV1_TX_4X4
                                   : plane_tx_size(walk, plane, mode->tx_size);

                uint32_t step_x = tiny_av1_tx_width[tx_size] >> 2;
                uint32_t step_y = tiny_av1_tx_height[tx_size] >> 2;

                uint32_t size = plane_residual_size(walk, chunk_size, plane);
                uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
                uint32_t high = tiny_av1_num_4x4_blocks_high[size];

                uint32_t sub_x = plane > 0u ? walk->sequence->sub_x : 0u;
                uint32_t sub_y = plane > 0u ? walk->sequence->sub_y : 0u;

                uint32_t base_x = (mode->col >> sub_x) * AV1_MI_SIZE;
                uint32_t base_y = (mode->row >> sub_y) * AV1_MI_SIZE;

                for (uint32_t y = 0; y < high; y += step_y) {
                    for (uint32_t x = 0; x < wide; x += step_x) {
                        int status = transform_block(
                            walk, plane, base_x, base_y, tx_size,
                            x + ((chunk_x << 4) >> sub_x),
                            y + ((chunk_y << 4) >> sub_y)
                        );

                        if (status != TINYIMG_OK) return status;
                    }
                }
            }
        }
    }

    return TINYIMG_OK;
}

// #endregion
