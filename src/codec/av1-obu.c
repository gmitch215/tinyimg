#include "av1.h"

#include "tinyimg/memory.h"

/** Values that mean "the frame header codes this rather than the sequence". */
#define AV1_SELECT_SCREEN_CONTENT_TOOLS 2
#define AV1_SELECT_INTEGER_MV 2

/** Tile geometry limits, in luma samples. */
#define AV1_MAX_TILE_WIDTH 4096
#define AV1_MAX_TILE_AREA (4096 * 2304)

/** Largest loop restoration unit. */
#define AV1_RESTORATION_TILESIZE_MAX 256

/** Superres, which a still image never uses but whose fields still parse. */
#define AV1_SUPERRES_DENOM_BITS 3
#define AV1_SUPERRES_DENOM_MIN 9
#define AV1_SUPERRES_NUM 8

#pragma region bits

/**
 * A most significant bit first reader over one OBU payload.
 *
 * Separate from `TinyBitReader` because the specification's f(n) reads at most
 * 32 bits and needs no refill bookkeeping at this size, and because an overrun
 * here has to latch rather than saturate: a header that runs off the end is a
 * corrupt file, not a decode that continues into padding. The symbol decoder's
 * reader is the one that treats the end differently, and for a stated reason.
 */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t bit;
    int overrun;
} Av1Bits;

static void bits_init(Av1Bits* bits, const uint8_t* data, size_t size) {
    bits->data = data;
    bits->size = size;
    bits->bit = 0;
    bits->overrun = 0;
}

static uint32_t f(Av1Bits* bits, uint32_t count) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < count; i++) {
        size_t index = bits->bit >> 3;

        if (index >= bits->size) {
            bits->overrun = 1;
            return value << (count - i);
        }

        uint32_t shift = 7u - (uint32_t) (bits->bit & 7u);

        value = (value << 1) | ((uint32_t) (bits->data[index] >> shift) & 1u);
        bits->bit++;
    }

    return count == 0 ? 0 : value;
}

/** su(n): a magnitude then a sign bit, in the specification's order. */
static int32_t su(Av1Bits* bits, uint32_t count) {
    uint32_t value = f(bits, count);
    uint32_t sign = 1u << (count - 1u);

    return (value & sign) ? (int32_t) value - (int32_t) (sign << 1)
                          : (int32_t) value;
}

/**
 * ns(n): the specification's non-symmetric code, used for tile sizes.
 *
 * Reads a short code for the low values and a longer one for the rest, so the
 * whole range costs at most one more bit than a fixed width would.
 */
static uint32_t ns(Av1Bits* bits, uint32_t n) {
    uint32_t w = 0;
    uint32_t x = n;

    while (x != 0) {
        x >>= 1;
        w++;
    }

    if (w <= 1) return 0;

    uint32_t m = (1u << w) - n;
    uint32_t v = f(bits, w - 1u);

    if (v < m) return v;

    return (v << 1) - m + f(bits, 1);
}

/** The specification's tile_log2: the smallest k with `blkSize << k >= target`.
 */
static uint32_t tile_log2(uint32_t blk, uint32_t target) {
    uint32_t k = 0;

    while ((blk << k) < target) k++;

    return k;
}

#pragma endregion

#pragma region obu

int tiny_av1_next_obu(
    const uint8_t* data, size_t size, size_t* at, TinyAv1Obu* obu
) {
    if (!data || !at || !obu || *at >= size) return 0;

    uint8_t header = data[*at];

    // the forbidden bit is exactly that, and a set one means this is not an
    // OBU header at all
    if (header & 0x80u) return 0;

    obu->type = (uint8_t) ((header >> 3) & 0x0Fu);

    int has_extension = (header >> 2) & 1;
    int has_size = (header >> 1) & 1;

    size_t p = *at + 1u;

    if (has_extension) {
        if (p >= size) return 0;
        p++;
    }

    size_t payload = size - p;

    if (has_size) {
        // leb128, up to eight bytes, low group first
        uint64_t value = 0;

        for (uint32_t i = 0; i < 8u; i++) {
            if (p >= size) return 0;

            uint8_t byte = data[p++];

            value |= (uint64_t) (byte & 0x7Fu) << (i * 7u);

            if (!(byte & 0x80u)) break;
        }

        if (value > (uint64_t) (size - p)) return 0;

        payload = (size_t) value;
    }

    obu->payload = data + p;
    obu->size = payload;

    *at = p + payload;
    return 1;
}

#pragma endregion

#pragma region sequence header

/** The colour description, which decides the matrix the conversion uses. */
static void read_color_config(Av1Bits* bits, TinyAv1Sequence* sequence) {
    uint32_t high_bitdepth = f(bits, 1);

    if (sequence->profile == 2u && high_bitdepth) {
        sequence->bit_depth = f(bits, 1) ? 12u : 10u;
    }
    else {
        sequence->bit_depth = high_bitdepth ? 10u : 8u;
    }

    sequence->monochrome = sequence->profile == 1u ? 0u : (uint8_t) f(bits, 1);
    sequence->planes = sequence->monochrome ? 1u : 3u;

    if (f(bits, 1)) {
        sequence->color_primaries = (uint8_t) f(bits, 8);
        sequence->transfer_characteristics = (uint8_t) f(bits, 8);
        sequence->matrix_coefficients = (uint8_t) f(bits, 8);
    }
    else {
        // 2 is the unspecified value in each of the three registries
        sequence->color_primaries = 2u;
        sequence->transfer_characteristics = 2u;
        sequence->matrix_coefficients = 2u;
    }

    if (sequence->monochrome) {
        sequence->color_range = (uint8_t) f(bits, 1);
        sequence->sub_x = 1u;
        sequence->sub_y = 1u;
        sequence->chroma_position = 0u;
        sequence->separate_uv_delta_q = (uint8_t) f(bits, 1);
        return;
    }

    // the identity matrix means the three planes are already RGB, which forces
    // 4:4:4 and full range
    if (sequence->color_primaries == 1u &&
        sequence->transfer_characteristics == 13u &&
        sequence->matrix_coefficients == 0u) {
        sequence->color_range = 1u;
        sequence->sub_x = 0u;
        sequence->sub_y = 0u;
    }
    else {
        sequence->color_range = (uint8_t) f(bits, 1);

        if (sequence->profile == 0u) {
            sequence->sub_x = 1u;
            sequence->sub_y = 1u;
        }
        else if (sequence->profile == 1u) {
            sequence->sub_x = 0u;
            sequence->sub_y = 0u;
        }
        else if (sequence->bit_depth == 12u) {
            sequence->sub_x = (uint8_t) f(bits, 1);
            sequence->sub_y = sequence->sub_x ? (uint8_t) f(bits, 1) : 0u;
        }
        else {
            sequence->sub_x = 1u;
            sequence->sub_y = 0u;
        }

        if (sequence->sub_x && sequence->sub_y) {
            sequence->chroma_position = (uint8_t) f(bits, 2);
        }
    }

    sequence->separate_uv_delta_q = (uint8_t) f(bits, 1);
}

int tiny_av1_read_sequence(
    TinyAv1Sequence* sequence, const uint8_t* data, size_t size
) {
    if (!sequence || !data) return TINYIMG_ERR_NULL;

    tiny_memset(sequence, 0, sizeof(*sequence));

    Av1Bits bits;
    bits_init(&bits, data, size);

    sequence->profile = (uint8_t) f(&bits, 3);
    sequence->still_picture = (uint8_t) f(&bits, 1);
    sequence->reduced = (uint8_t) f(&bits, 1);

    if (sequence->reduced) {
        sequence->level = (uint8_t) f(&bits, 5);
    }
    else {
        // an item this build decodes is one still picture, and the timing and
        // decoder model an operating point carries describe a sequence being
        // played. Refusing here is narrower than parsing fields nothing reads
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    sequence->width_bits = (uint8_t) (f(&bits, 4) + 1u);
    sequence->height_bits = (uint8_t) (f(&bits, 4) + 1u);
    sequence->max_width = f(&bits, sequence->width_bits) + 1u;
    sequence->max_height = f(&bits, sequence->height_bits) + 1u;

    sequence->use_128x128_superblock = (uint8_t) f(&bits, 1);
    sequence->enable_filter_intra = (uint8_t) f(&bits, 1);
    sequence->enable_intra_edge_filter = (uint8_t) f(&bits, 1);

    // a reduced header implies all of these rather than coding them
    sequence->enable_order_hint = 0u;
    sequence->order_hint_bits = 0u;
    sequence->force_screen_content_tools = AV1_SELECT_SCREEN_CONTENT_TOOLS;
    sequence->force_integer_mv = AV1_SELECT_INTEGER_MV;

    sequence->enable_superres = (uint8_t) f(&bits, 1);
    sequence->enable_cdef = (uint8_t) f(&bits, 1);
    sequence->enable_restoration = (uint8_t) f(&bits, 1);

    read_color_config(&bits, sequence);

    sequence->film_grain_params_present = (uint8_t) f(&bits, 1);

    if (bits.overrun) return TINYIMG_ERR_CORRUPT;
    if (sequence->max_width == 0u || sequence->max_height == 0u) {
        return TINYIMG_ERR_CORRUPT;
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region frame header

static int32_t read_delta_q(Av1Bits* bits) {
    return f(bits, 1) ? su(bits, 7) : 0;
}

/**
 * Tile geometry.
 *
 * The uniform path is the one every still encoder takes, and it codes the tile
 * count as a run of increment bits rather than as a number. The non-uniform
 * path is parsed too, because `avifenc` does tile above half a megapixel and a
 * file that used explicit widths would otherwise be rejected for no reason.
 */
static int read_tile_info(
    Av1Bits* bits, const TinyAv1Sequence* sequence, TinyAv1Frame* frame
) {
    uint32_t shift = sequence->use_128x128_superblock ? 5u : 4u;
    uint32_t sb_cols = (frame->mi_cols + (1u << shift) - 1u) >> shift;
    uint32_t sb_rows = (frame->mi_rows + (1u << shift) - 1u) >> shift;
    uint32_t sb_size = shift + 2u;

    if (sb_cols == 0u || sb_rows == 0u) return TINYIMG_ERR_CORRUPT;

    uint32_t max_tile_width_sb = AV1_MAX_TILE_WIDTH >> sb_size;
    uint32_t max_tile_area_sb = AV1_MAX_TILE_AREA >> (2u * sb_size);

    uint32_t min_log2_cols = tile_log2(max_tile_width_sb, sb_cols);
    uint32_t max_log2_cols = tile_log2(
        1u, sb_cols < TINY_AV1_MAX_TILE_COLS ? sb_cols : TINY_AV1_MAX_TILE_COLS
    );
    uint32_t max_log2_rows = tile_log2(
        1u, sb_rows < TINY_AV1_MAX_TILE_ROWS ? sb_rows : TINY_AV1_MAX_TILE_ROWS
    );

    uint32_t area_log2 = tile_log2(max_tile_area_sb, sb_rows * sb_cols);
    uint32_t min_log2_tiles =
        min_log2_cols > area_log2 ? min_log2_cols : area_log2;

    if (f(bits, 1)) {
        frame->tile_cols_log2 = (uint8_t) min_log2_cols;

        while (frame->tile_cols_log2 < max_log2_cols) {
            if (!f(bits, 1)) break;
            frame->tile_cols_log2++;
        }

        uint32_t width_sb = (sb_cols + (1u << frame->tile_cols_log2) - 1u) >>
                            frame->tile_cols_log2;

        uint32_t i = 0;

        for (uint32_t start = 0; start < sb_cols; start += width_sb) {
            if (i >= TINY_AV1_MAX_TILE_COLS) return TINYIMG_ERR_CORRUPT;
            frame->mi_col_starts[i++] = start << shift;
        }

        frame->mi_col_starts[i] = frame->mi_cols;
        frame->tile_cols = i;

        uint32_t min_log2_rows = min_log2_tiles > frame->tile_cols_log2
                                     ? min_log2_tiles - frame->tile_cols_log2
                                     : 0u;

        frame->tile_rows_log2 = (uint8_t) min_log2_rows;

        while (frame->tile_rows_log2 < max_log2_rows) {
            if (!f(bits, 1)) break;
            frame->tile_rows_log2++;
        }

        uint32_t height_sb = (sb_rows + (1u << frame->tile_rows_log2) - 1u) >>
                             frame->tile_rows_log2;

        i = 0;

        for (uint32_t start = 0; start < sb_rows; start += height_sb) {
            if (i >= TINY_AV1_MAX_TILE_ROWS) return TINYIMG_ERR_CORRUPT;
            frame->mi_row_starts[i++] = start << shift;
        }

        frame->mi_row_starts[i] = frame->mi_rows;
        frame->tile_rows = i;
    }
    else {
        uint32_t widest = 0;
        uint32_t start = 0;
        uint32_t i = 0;

        for (; start < sb_cols; i++) {
            if (i >= TINY_AV1_MAX_TILE_COLS) return TINYIMG_ERR_CORRUPT;

            frame->mi_col_starts[i] = start << shift;

            uint32_t room = sb_cols - start;
            uint32_t most = room < max_tile_width_sb ? room : max_tile_width_sb;
            uint32_t taken = ns(bits, most) + 1u;

            if (taken > widest) widest = taken;
            start += taken;
        }

        frame->mi_col_starts[i] = frame->mi_cols;
        frame->tile_cols = i;
        frame->tile_cols_log2 = (uint8_t) tile_log2(1u, i);

        if (widest == 0u) return TINYIMG_ERR_CORRUPT;

        uint32_t area = min_log2_tiles > 0u
                            ? (sb_rows * sb_cols) >> (min_log2_tiles + 1u)
                            : sb_rows * sb_cols;

        uint32_t max_height_sb = area / widest;
        if (max_height_sb < 1u) max_height_sb = 1u;

        start = 0;
        i = 0;

        for (; start < sb_rows; i++) {
            if (i >= TINY_AV1_MAX_TILE_ROWS) return TINYIMG_ERR_CORRUPT;

            frame->mi_row_starts[i] = start << shift;

            uint32_t room = sb_rows - start;
            uint32_t most = room < max_height_sb ? room : max_height_sb;

            start += ns(bits, most) + 1u;
        }

        frame->mi_row_starts[i] = frame->mi_rows;
        frame->tile_rows = i;
        frame->tile_rows_log2 = (uint8_t) tile_log2(1u, i);
    }

    if (frame->tile_cols_log2 > 0u || frame->tile_rows_log2 > 0u) {
        frame->context_update_tile_id =
            f(bits, (uint32_t) frame->tile_rows_log2 +
                        (uint32_t) frame->tile_cols_log2);
        frame->tile_size_bytes = (uint8_t) (f(bits, 2) + 1u);
    }
    else {
        frame->context_update_tile_id = 0;
        frame->tile_size_bytes = 1u;
    }

    return TINYIMG_OK;
}

static void read_quantization(
    Av1Bits* bits, const TinyAv1Sequence* sequence, TinyAv1Frame* frame
) {
    frame->base_q_idx = (uint8_t) f(bits, 8);
    frame->delta_q_y_dc = (int8_t) read_delta_q(bits);

    if (sequence->planes > 1u) {
        int diff = sequence->separate_uv_delta_q ? (int) f(bits, 1) : 0;

        frame->delta_q_u_dc = (int8_t) read_delta_q(bits);
        frame->delta_q_u_ac = (int8_t) read_delta_q(bits);

        if (diff) {
            frame->delta_q_v_dc = (int8_t) read_delta_q(bits);
            frame->delta_q_v_ac = (int8_t) read_delta_q(bits);
        }
        else {
            frame->delta_q_v_dc = frame->delta_q_u_dc;
            frame->delta_q_v_ac = frame->delta_q_u_ac;
        }
    }

    frame->using_qmatrix = (uint8_t) f(bits, 1);

    if (frame->using_qmatrix) {
        frame->qm_y = (uint8_t) f(bits, 4);
        frame->qm_u = (uint8_t) f(bits, 4);
        frame->qm_v =
            sequence->separate_uv_delta_q ? (uint8_t) f(bits, 4) : frame->qm_u;
    }
}

/**
 * Segmentation, and the three things derived from it.
 *
 * A still key frame with segmentation enabled updates its data by definition,
 * so every feature is coded. The bit widths are the specification's tables.
 *
 * **`SegIdPreSkip` is what makes this more than a bit count.** It reverses the
 * order of two syntax elements in every block's mode info, so a decoder that
 * stepped over the features without deriving it desynchronises on the first
 * block of any segmented frame. `LastActiveSegId` sizes the segment id's own
 * coding, and the alternate quantizer decides which segments are lossless.
 */
static void read_segmentation(Av1Bits* bits, TinyAv1Frame* frame) {
    static const uint8_t feature_bits[TINY_AV1_SEG_LVL_MAX] = {8, 6, 6, 6,
                                                               6, 3, 0, 0};
    static const uint8_t feature_signed[TINY_AV1_SEG_LVL_MAX] = {1, 1, 1, 1,
                                                                 1, 0, 0, 0};
    static const int32_t feature_max[TINY_AV1_SEG_LVL_MAX] = {
        255,
        TINY_AV1_MAX_LOOP_FILTER,
        TINY_AV1_MAX_LOOP_FILTER,
        TINY_AV1_MAX_LOOP_FILTER,
        TINY_AV1_MAX_LOOP_FILTER,
        7,
        0,
        0
    };

    frame->segmentation_enabled = (uint8_t) f(bits, 1);
    frame->seg_id_pre_skip = 0u;
    frame->last_active_seg_id = 0u;

    int32_t alt_q[TINY_AV1_MAX_SEGMENTS];

    for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
        alt_q[i] = 0;
        frame->seg_skip[i] = 0u;
        frame->seg_alt_q[i] = 0;
        frame->seg_alt_q_active[i] = 0u;

        for (uint32_t j = 0; j < TINY_AV1_FRAME_LF_COUNT; j++) {
            frame->seg_alt_lf[j][i] = 0;
            frame->seg_alt_lf_active[j][i] = 0u;
        }
    }

    if (frame->segmentation_enabled) {
        // primary_ref_frame is always none here, so the map and the data are
        // both updated without a flag being coded for either
        for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
            for (uint32_t j = 0; j < TINY_AV1_SEG_LVL_MAX; j++) {
                if (!f(bits, 1)) continue;

                uint32_t width = feature_bits[j];
                int32_t value;

                if (feature_signed[j]) {
                    value = su(bits, width + 1u);

                    if (value < -feature_max[j]) value = -feature_max[j];
                    if (value > feature_max[j]) value = feature_max[j];
                }
                else {
                    value = (int32_t) f(bits, width);

                    if (value > feature_max[j]) value = feature_max[j];
                }

                // features 1 through 4 are the loop filter's, in the order
                // the filter level indexes them
                if (j >= 1u && j <= TINY_AV1_FRAME_LF_COUNT) {
                    frame->seg_alt_lf[j - 1u][i] = (int16_t) value;
                    frame->seg_alt_lf_active[j - 1u][i] = 1u;
                }

                if (j == TINY_AV1_SEG_LVL_ALT_Q) {
                    alt_q[i] = value;
                    frame->seg_alt_q[i] = (int16_t) value;
                    frame->seg_alt_q_active[i] = 1u;
                }
                if (j == TINY_AV1_SEG_LVL_SKIP) frame->seg_skip[i] = 1u;

                frame->last_active_seg_id = (uint8_t) i;

                if (j >= TINY_AV1_SEG_LVL_REF_FRAME) {
                    frame->seg_id_pre_skip = 1u;
                }
            }
        }
    }

    for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
        int32_t qindex = (int32_t) frame->base_q_idx;

        if (frame->segmentation_enabled) qindex += alt_q[i];

        if (qindex < 0) qindex = 0;
        if (qindex > 255) qindex = 255;

        frame->seg_qindex[i] = (uint8_t) qindex;
        frame->lossless_array[i] =
            (uint8_t) (qindex == 0 && frame->delta_q_y_dc == 0 &&
                       frame->delta_q_u_ac == 0 && frame->delta_q_u_dc == 0 &&
                       frame->delta_q_v_ac == 0 && frame->delta_q_v_dc == 0);

        // a lossless segment takes level 15, which means no matrix at all, so
        // the one thing a quantizer matrix must never do is scale a lossless
        // coefficient
        uint8_t level[3] = {frame->qm_y, frame->qm_u, frame->qm_v};

        for (uint32_t plane = 0; plane < 3u; plane++) {
            frame->seg_qm_level[plane][i] =
                frame->lossless_array[i] ? 15u : level[plane];
        }
    }
}

static void read_loop_filter(
    Av1Bits* bits, const TinyAv1Sequence* sequence, TinyAv1Frame* frame
) {
    static const int8_t defaults[TINY_AV1_TOTAL_REFS] = {1, 0,  0,  0,
                                                         0, -1, -1, -1};

    for (uint32_t i = 0; i < TINY_AV1_TOTAL_REFS; i++) {
        frame->loop_filter_ref_deltas[i] = defaults[i];
    }

    if (frame->coded_lossless || frame->allow_intrabc) return;

    frame->loop_filter_level[0] = (uint8_t) f(bits, 6);
    frame->loop_filter_level[1] = (uint8_t) f(bits, 6);

    if (sequence->planes > 1u &&
        (frame->loop_filter_level[0] || frame->loop_filter_level[1])) {
        frame->loop_filter_level[2] = (uint8_t) f(bits, 6);
        frame->loop_filter_level[3] = (uint8_t) f(bits, 6);
    }

    frame->loop_filter_sharpness = (uint8_t) f(bits, 3);
    frame->loop_filter_delta_enabled = (uint8_t) f(bits, 1);

    if (frame->loop_filter_delta_enabled && f(bits, 1)) {
        for (uint32_t i = 0; i < TINY_AV1_TOTAL_REFS; i++) {
            if (f(bits, 1)) {
                frame->loop_filter_ref_deltas[i] = (int8_t) su(bits, 7);
            }
        }

        for (uint32_t i = 0; i < 2u; i++) {
            if (f(bits, 1)) {
                frame->loop_filter_mode_deltas[i] = (int8_t) su(bits, 7);
            }
        }
    }
}

static void read_cdef(
    Av1Bits* bits, const TinyAv1Sequence* sequence, TinyAv1Frame* frame
) {
    frame->cdef_damping = 3u;

    if (frame->coded_lossless || frame->allow_intrabc ||
        !sequence->enable_cdef) {
        return;
    }

    frame->cdef_damping = (uint8_t) (f(bits, 2) + 3u);
    frame->cdef_bits = (uint8_t) f(bits, 2);

    for (uint32_t i = 0; i < (1u << frame->cdef_bits); i++) {
        frame->cdef_y_pri[i] = (uint8_t) f(bits, 4);
        frame->cdef_y_sec[i] = (uint8_t) f(bits, 2);

        // three is coded for a strength of four, which is the one value the
        // two bit field cannot hold
        if (frame->cdef_y_sec[i] == 3u) frame->cdef_y_sec[i] = 4u;

        if (sequence->planes > 1u) {
            frame->cdef_uv_pri[i] = (uint8_t) f(bits, 4);
            frame->cdef_uv_sec[i] = (uint8_t) f(bits, 2);

            if (frame->cdef_uv_sec[i] == 3u) frame->cdef_uv_sec[i] = 4u;
        }
    }
}

static void read_lr(
    Av1Bits* bits, const TinyAv1Sequence* sequence, TinyAv1Frame* frame
) {
    // the coded order is none, switchable, wiener, sgrproj
    static const uint8_t remap[4] = {0, 3, 1, 2};

    if (frame->all_lossless || frame->allow_intrabc ||
        !sequence->enable_restoration) {
        return;
    }

    uint32_t uses_chroma = 0;

    for (uint32_t i = 0; i < sequence->planes; i++) {
        frame->restoration_type[i] = remap[f(bits, 2)];

        if (frame->restoration_type[i] != 0u) {
            frame->uses_lr = 1u;
            if (i > 0u) uses_chroma = 1u;
        }
    }

    if (!frame->uses_lr) return;

    uint32_t unit_shift;

    if (sequence->use_128x128_superblock) {
        unit_shift = f(bits, 1) + 1u;
    }
    else {
        unit_shift = f(bits, 1);
        if (unit_shift) unit_shift += f(bits, 1);
    }

    frame->restoration_size[0] =
        (uint16_t) (AV1_RESTORATION_TILESIZE_MAX >> (2u - unit_shift));

    uint32_t uv_shift = 0;

    if (sequence->sub_x && sequence->sub_y && uses_chroma) {
        uv_shift = f(bits, 1);
    }

    frame->restoration_size[1] =
        (uint16_t) (frame->restoration_size[0] >> uv_shift);
    frame->restoration_size[2] = frame->restoration_size[1];
}

int tiny_av1_read_frame(
    TinyAv1Frame* frame, const TinyAv1Sequence* sequence, const uint8_t* data,
    size_t size, size_t* consumed
) {
    if (consumed) *consumed = 0u;
    if (!frame || !sequence || !data) return TINYIMG_ERR_NULL;
    if (!sequence->reduced) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    tiny_memset(frame, 0, sizeof(*frame));

    Av1Bits bits;
    bits_init(&bits, data, size);

    // a reduced header implies a shown key frame, so nothing above is coded
    frame->disable_cdf_update = (uint8_t) f(&bits, 1);

    frame->allow_screen_content_tools =
        sequence->force_screen_content_tools == AV1_SELECT_SCREEN_CONTENT_TOOLS
            ? (uint8_t) f(&bits, 1)
            : sequence->force_screen_content_tools;

    // read and discarded: an intra frame forces integer motion vectors anyway,
    // but the bit is still in the stream and skipping it shifts everything
    // after
    if (frame->allow_screen_content_tools &&
        sequence->force_integer_mv == AV1_SELECT_INTEGER_MV) {
        (void) f(&bits, 1);
    }

    // frame_size(), with frame_size_override_flag implied zero
    frame->width = sequence->max_width;
    frame->height = sequence->max_height;
    frame->upscaled_width = frame->width;

    if (sequence->enable_superres && f(&bits, 1)) {
        uint32_t denom =
            f(&bits, AV1_SUPERRES_DENOM_BITS) + AV1_SUPERRES_DENOM_MIN;

        frame->width =
            (frame->upscaled_width * AV1_SUPERRES_NUM + denom / 2u) / denom;
    }

    frame->mi_cols = 2u * ((frame->width + 7u) >> 3u);
    frame->mi_rows = 2u * ((frame->height + 7u) >> 3u);

    // render_size()
    if (f(&bits, 1)) {
        frame->render_width = f(&bits, 16) + 1u;
        frame->render_height = f(&bits, 16) + 1u;
    }
    else {
        frame->render_width = frame->upscaled_width;
        frame->render_height = frame->height;
    }

    if (frame->allow_screen_content_tools &&
        frame->upscaled_width == frame->width) {
        frame->allow_intrabc = (uint8_t) f(&bits, 1);
    }

    int result = read_tile_info(&bits, sequence, frame);
    if (result != TINYIMG_OK) return result;

    read_quantization(&bits, sequence, frame);
    read_segmentation(&bits, frame);

    // delta_q_params
    if (frame->base_q_idx > 0u) frame->delta_q_present = (uint8_t) f(&bits, 1);
    if (frame->delta_q_present) frame->delta_q_res = (uint8_t) f(&bits, 2);

    // delta_lf_params
    if (frame->delta_q_present) {
        if (!frame->allow_intrabc) {
            frame->delta_lf_present = (uint8_t) f(&bits, 1);
        }

        if (frame->delta_lf_present) {
            frame->delta_lf_res = (uint8_t) f(&bits, 2);
            frame->delta_lf_multi = (uint8_t) f(&bits, 1);
        }
    }

    /*
     * Lossless is a property of the quantizer rather than a flag, and it gates
     * all three post-filters, so it has to be derived before they are read.
     *
     * Segmentation can give each segment its own quantizer index, and the frame
     * is only losslessly coded if every one of them lands at zero. This build
     * does not read the per-segment deltas back, so a segmented frame is
     * treated as not lossless, which is the safe direction: it reads the filter
     * parameters that a lossless frame would omit, and a conformant encoder
     * does not emit both at once.
     */
    frame->coded_lossless =
        (uint8_t) (!frame->segmentation_enabled && frame->base_q_idx == 0u &&
                   frame->delta_q_y_dc == 0 && frame->delta_q_u_dc == 0 &&
                   frame->delta_q_u_ac == 0 && frame->delta_q_v_dc == 0 &&
                   frame->delta_q_v_ac == 0);

    frame->all_lossless = (uint8_t) (frame->coded_lossless &&
                                     frame->width == frame->upscaled_width);

    read_loop_filter(&bits, sequence, frame);
    read_cdef(&bits, sequence, frame);
    read_lr(&bits, sequence, frame);

    // read_tx_mode
    if (frame->coded_lossless)
        frame->tx_mode = 0u;
    else
        frame->tx_mode = f(&bits, 1) ? 2u : 1u;

    // frame_reference_mode and skip_mode_params are both empty for an intra
    // frame, and global motion has nothing to send either
    frame->reduced_tx_set = (uint8_t) f(&bits, 1);

    if (sequence->film_grain_params_present) {
        // apply_grain, and the parameters behind it, which this build does not
        // synthesize. Reading the flag is enough to know the frame wanted it
        (void) f(&bits, 1);
    }

    if (bits.overrun) return TINYIMG_ERR_CORRUPT;

    // a frame OBU puts the tile group straight after this header, byte aligned,
    // so the caller needs to know where the header stopped
    if (consumed) *consumed = (bits.bit + 7u) / 8u;

    return TINYIMG_OK;
}

/**
 * One tile's byte range inside a tile group payload.
 *
 * The group's own header is a flag, an optional pair of tile indices and a byte
 * alignment; the tiles that follow are length-prefixed except the last, whose
 * length is whatever is left. A single-tile frame codes no flag at all, which
 * is why the count comes from the frame header rather than from the payload.
 */
int tiny_av1_tile_bytes(
    const TinyAv1Frame* frame, const uint8_t* data, size_t size, uint32_t index,
    const uint8_t** tile, size_t* tile_size
) {
    if (!frame || !data || !tile || !tile_size) return TINYIMG_ERR_NULL;

    uint32_t tiles = frame->tile_cols * frame->tile_rows;
    if (tiles == 0u || index >= tiles) return TINYIMG_ERR_BOUNDS;

    Av1Bits bits;
    bits_init(&bits, data, size);

    uint32_t start = 0u;
    uint32_t end = tiles - 1u;

    if (tiles > 1u && f(&bits, 1)) {
        uint32_t width = frame->tile_cols_log2 + frame->tile_rows_log2;

        start = f(&bits, width);
        end = f(&bits, width);
    }

    if (bits.overrun) return TINYIMG_ERR_CORRUPT;
    if (index < start || index > end) return TINYIMG_ERR_BOUNDS;

    size_t at = (bits.bit + 7u) / 8u;

    for (uint32_t which = start; which <= end; which++) {
        size_t length;

        if (which == end) {
            if (at > size) return TINYIMG_ERR_CORRUPT;
            length = size - at;
        }
        else {
            uint32_t width = frame->tile_size_bytes;

            if (at + width > size) return TINYIMG_ERR_CORRUPT;

            // le(TileSizeBytes), which is little endian and one less than the
            // length
            uint32_t coded = 0;

            for (uint32_t i = 0; i < width; i++) {
                coded |= (uint32_t) data[at + i] << (8u * i);
            }

            at += width;
            length = (size_t) coded + 1u;
        }

        if (at + length > size) return TINYIMG_ERR_CORRUPT;

        if (which == index) {
            *tile = data + at;
            *tile_size = length;

            return TINYIMG_OK;
        }

        at += length;
    }

    return TINYIMG_ERR_BOUNDS;
}

#pragma endregion
