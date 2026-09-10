/**
 * @file av1.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief The AV1 intra decoder AVIF sits on, split into the stages a still
 * picture needs.
 * @version 1.0.1
 * @date 2026-09-10
 *
 * @copyright Copyright (c) 2026
 *
 * Internal to the codec. `avif.c` owns the container and calls in here with the
 * bytes of one `av01` item; nothing outside `src/codec/` includes this.
 *
 * Every enumerator below carries the value the specification gives it, because
 * `av1-tables.h` is indexed by exactly these numbers. Renumbering one silently
 * reads the wrong row of a trained probability table, which desynchronises a
 * bitstream rather than failing.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/image.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#ifdef __cplusplus
extern "C" {
#endif

// #region sizes and modes

/** Transform sizes, `TX_SIZES_ALL` of them. */
typedef enum TinyAv1TxSize
{
    TINY_AV1_TX_4X4 = 0,
    TINY_AV1_TX_8X8 = 1,
    TINY_AV1_TX_16X16 = 2,
    TINY_AV1_TX_32X32 = 3,
    TINY_AV1_TX_64X64 = 4,
    TINY_AV1_TX_4X8 = 5,
    TINY_AV1_TX_8X4 = 6,
    TINY_AV1_TX_8X16 = 7,
    TINY_AV1_TX_16X8 = 8,
    TINY_AV1_TX_16X32 = 9,
    TINY_AV1_TX_32X16 = 10,
    TINY_AV1_TX_32X64 = 11,
    TINY_AV1_TX_64X32 = 12,
    TINY_AV1_TX_4X16 = 13,
    TINY_AV1_TX_16X4 = 14,
    TINY_AV1_TX_8X32 = 15,
    TINY_AV1_TX_32X8 = 16,
    TINY_AV1_TX_16X64 = 17,
    TINY_AV1_TX_64X16 = 18
} TinyAv1TxSize;

/**
 * Transform types.
 *
 * The name reads columns first: `ADST_DCT` is an ADST down the columns and a
 * DCT along the rows, which is the opposite of the reading most people reach
 * for. `V_` and `H_` apply one transform on one axis and the identity on the
 * other.
 */
typedef enum TinyAv1TxType
{
    TINY_AV1_DCT_DCT = 0,
    TINY_AV1_ADST_DCT = 1,
    TINY_AV1_DCT_ADST = 2,
    TINY_AV1_ADST_ADST = 3,
    TINY_AV1_FLIPADST_DCT = 4,
    TINY_AV1_DCT_FLIPADST = 5,
    TINY_AV1_FLIPADST_FLIPADST = 6,
    TINY_AV1_ADST_FLIPADST = 7,
    TINY_AV1_FLIPADST_ADST = 8,
    TINY_AV1_IDTX = 9,
    TINY_AV1_V_DCT = 10,
    TINY_AV1_H_DCT = 11,
    TINY_AV1_V_ADST = 12,
    TINY_AV1_H_ADST = 13,
    TINY_AV1_V_FLIPADST = 14,
    TINY_AV1_H_FLIPADST = 15
} TinyAv1TxType;

/** Intra prediction modes. `UV_CFL_PRED` is reachable on chroma only. */
typedef enum TinyAv1IntraMode
{
    TINY_AV1_DC_PRED = 0,
    TINY_AV1_V_PRED = 1,
    TINY_AV1_H_PRED = 2,
    TINY_AV1_D45_PRED = 3,
    TINY_AV1_D135_PRED = 4,
    TINY_AV1_D113_PRED = 5,
    TINY_AV1_D157_PRED = 6,
    TINY_AV1_D203_PRED = 7,
    TINY_AV1_D67_PRED = 8,
    TINY_AV1_SMOOTH_PRED = 9,
    TINY_AV1_SMOOTH_V_PRED = 10,
    TINY_AV1_SMOOTH_H_PRED = 11,
    TINY_AV1_PAETH_PRED = 12,
    TINY_AV1_UV_CFL_PRED = 13
} TinyAv1IntraMode;

/** The five recursive filter modes, used instead of a directional prediction.
 */
typedef enum TinyAv1FilterIntraMode
{
    TINY_AV1_FILTER_DC_PRED = 0,
    TINY_AV1_FILTER_V_PRED = 1,
    TINY_AV1_FILTER_H_PRED = 2,
    TINY_AV1_FILTER_D157_PRED = 3,
    TINY_AV1_FILTER_PAETH_PRED = 4
} TinyAv1FilterIntraMode;

/** Block sizes, `BLOCK_SIZES` of them. */
typedef enum TinyAv1BlockSize
{
    TINY_AV1_BLOCK_4X4 = 0,
    TINY_AV1_BLOCK_4X8 = 1,
    TINY_AV1_BLOCK_8X4 = 2,
    TINY_AV1_BLOCK_8X8 = 3,
    TINY_AV1_BLOCK_8X16 = 4,
    TINY_AV1_BLOCK_16X8 = 5,
    TINY_AV1_BLOCK_16X16 = 6,
    TINY_AV1_BLOCK_16X32 = 7,
    TINY_AV1_BLOCK_32X16 = 8,
    TINY_AV1_BLOCK_32X32 = 9,
    TINY_AV1_BLOCK_32X64 = 10,
    TINY_AV1_BLOCK_64X32 = 11,
    TINY_AV1_BLOCK_64X64 = 12,
    TINY_AV1_BLOCK_64X128 = 13,
    TINY_AV1_BLOCK_128X64 = 14,
    TINY_AV1_BLOCK_128X128 = 15,
    TINY_AV1_BLOCK_4X16 = 16,
    TINY_AV1_BLOCK_16X4 = 17,
    TINY_AV1_BLOCK_8X32 = 18,
    TINY_AV1_BLOCK_32X8 = 19,
    TINY_AV1_BLOCK_16X64 = 20,
    TINY_AV1_BLOCK_64X16 = 21,
    TINY_AV1_BLOCK_INVALID = 22
} TinyAv1BlockSize;

/** Partition choices at one node of the tree. */
typedef enum TinyAv1Partition
{
    TINY_AV1_PARTITION_NONE = 0,
    TINY_AV1_PARTITION_HORZ = 1,
    TINY_AV1_PARTITION_VERT = 2,
    TINY_AV1_PARTITION_SPLIT = 3,
    TINY_AV1_PARTITION_HORZ_A = 4,
    TINY_AV1_PARTITION_HORZ_B = 5,
    TINY_AV1_PARTITION_VERT_A = 6,
    TINY_AV1_PARTITION_VERT_B = 7,
    TINY_AV1_PARTITION_HORZ_4 = 8,
    TINY_AV1_PARTITION_VERT_4 = 9
} TinyAv1Partition;

// #region headers

/** Tile columns and rows the specification permits. */
#define TINY_AV1_MAX_TILE_COLS 64
#define TINY_AV1_MAX_TILE_ROWS 64

/** Segments, and features per segment. */
#define TINY_AV1_MAX_SEGMENTS 8
#define TINY_AV1_SEG_LVL_MAX 8

/** The three segment features this decoder reads, by their index. */
#define TINY_AV1_SEG_LVL_ALT_Q 0
#define TINY_AV1_SEG_LVL_REF_FRAME 5
#define TINY_AV1_SEG_LVL_SKIP 6

/** Largest loop filter level, which bounds the per-segment and delta values. */
#define TINY_AV1_MAX_LOOP_FILTER 63

/** Loop filter deltas a frame carries, one per plane and direction. */
#define TINY_AV1_FRAME_LF_COUNT 4

/** How far the transform size tree may be split from a block's own size. */
#define TINY_AV1_MAX_TX_DEPTH 2

/** Largest angle offset a directional mode carries, either way. */
#define TINY_AV1_MAX_ANGLE_DELTA 3

/** The escape value both delta magnitudes use, DELTA_Q_SMALL and its twin. */
#define TINY_AV1_DELTA_SMALL 3

/** The transform mode that codes a size per block rather than one per frame. */
#define TINY_AV1_TX_MODE_SELECT 2

/** Chroma-from-luma sign values, packed two to a symbol. */
#define TINY_AV1_CFL_SIGN_ZERO 0
#define TINY_AV1_CFL_SIGN_NEG 1
#define TINY_AV1_CFL_SIGN_POS 2

/** Reference frame types, including the intra one. */
#define TINY_AV1_TOTAL_REFS 8

/**
 * @brief What the sequence header says, which holds for every frame in the
 * item.
 *
 * A still AVIF sets `reduced` and then most of this is implied rather than
 * coded: no timing information, one operating point, no frame ids, no order
 * hints, and screen content tools and integer motion vectors both left for the
 * frame header to choose. The fields are kept even so, because the frame header
 * branches on them and reading them from one place is what keeps the two
 * parsers agreeing.
 */
typedef struct {
    uint8_t profile;
    uint8_t still_picture;
    uint8_t reduced;
    uint8_t level;

    uint32_t max_width;
    uint32_t max_height;
    uint8_t width_bits;
    uint8_t height_bits;

    uint8_t use_128x128_superblock;
    uint8_t enable_filter_intra;
    uint8_t enable_intra_edge_filter;
    uint8_t enable_superres;
    uint8_t enable_cdef;
    uint8_t enable_restoration;
    uint8_t enable_order_hint;

    /** 8, 10 or 12, and the plane count that follows from `monochrome`. */
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t planes;

    uint8_t color_primaries;
    uint8_t transfer_characteristics;

    /**
     * The matrix the YUV to RGB conversion has to use.
     *
     * Not decorative: `cavif` writes 9, which is BT.2020 non-constant
     * luminance, alongside BT.709 primaries. Converting with the wrong matrix
     * gives a picture that is plausible and wrong everywhere.
     */
    uint8_t matrix_coefficients;

    /** Zero is the studio range, so the conversion has to expand it. */
    uint8_t color_range;

    uint8_t sub_x;
    uint8_t sub_y;
    uint8_t chroma_position;
    uint8_t separate_uv_delta_q;
    uint8_t film_grain_params_present;

    /** Both left as the select value by a reduced header. */
    uint8_t force_screen_content_tools;
    uint8_t force_integer_mv;
    uint8_t order_hint_bits;
} TinyAv1Sequence;

/** What one frame header says. A still item carries exactly one. */
typedef struct {
    uint8_t disable_cdf_update;
    uint8_t allow_screen_content_tools;
    uint8_t allow_intrabc;

    uint32_t width;
    uint32_t height;
    uint32_t upscaled_width;
    uint32_t render_width;
    uint32_t render_height;

    /** The frame in units of four luma samples, which is what the walk uses. */
    uint32_t mi_cols;
    uint32_t mi_rows;

    uint8_t base_q_idx;
    int8_t delta_q_y_dc;
    int8_t delta_q_u_dc;
    int8_t delta_q_u_ac;
    int8_t delta_q_v_dc;
    int8_t delta_q_v_ac;

    /**
     * Non-zero when the frame carries quantizer matrix levels.
     *
     * Worth expecting rather than treating as rare: every file `avifenc`
     * produced in testing sets it, and none of the 115 conformance files do.
     */
    uint8_t using_qmatrix;
    uint8_t qm_y;
    uint8_t qm_u;
    uint8_t qm_v;

    uint8_t segmentation_enabled;
    /**
     * @brief Non-zero when the segment id is coded before the skip flag.
     *
     * Derived rather than coded: any enabled feature at SEG_LVL_REF_FRAME or
     * above sets it. Getting it wrong reverses the order of two syntax elements
     * and desynchronises every block after the first.
     */
    uint8_t seg_id_pre_skip;
    /** The highest segment id any enabled feature names. */
    uint8_t last_active_seg_id;
    /** Whether each segment's quantizer makes its blocks lossless. */
    uint8_t lossless_array[TINY_AV1_MAX_SEGMENTS];
    /** Whether SEG_LVL_SKIP is enabled per segment, which forces `skip`. */
    uint8_t seg_skip[TINY_AV1_MAX_SEGMENTS];
    /**
     * @brief The per-segment loop filter deltas, one per plane and direction.
     *
     * Indexed the way the filter level is: 0 and 1 are the luma's two
     * directions, then one each for the two chroma planes.
     */
    int16_t seg_alt_lf[TINY_AV1_FRAME_LF_COUNT][TINY_AV1_MAX_SEGMENTS];
    uint8_t seg_alt_lf_active[TINY_AV1_FRAME_LF_COUNT][TINY_AV1_MAX_SEGMENTS];

    /** The per-segment quantizer index, after SEG_LVL_ALT_Q. */
    uint8_t seg_qindex[TINY_AV1_MAX_SEGMENTS];
    /**
     * @brief The raw SEG_LVL_ALT_Q data, and whether the feature is on.
     *
     * Kept beside the resolved index because the two are not interchangeable:
     * `seg_qindex` is `get_qindex(1, id)`, which ignores the per-superblock
     * delta, and dequantization needs `get_qindex(0, id)`, which adds this to
     * the running index instead of to the frame's.
     */
    int16_t seg_alt_q[TINY_AV1_MAX_SEGMENTS];
    uint8_t seg_alt_q_active[TINY_AV1_MAX_SEGMENTS];
    /** The quantizer matrix level per plane and segment, 15 meaning none. */
    uint8_t seg_qm_level[3][TINY_AV1_MAX_SEGMENTS];

    uint8_t delta_q_present;
    uint8_t delta_q_res;
    uint8_t delta_lf_present;
    uint8_t delta_lf_res;
    uint8_t delta_lf_multi;

    /** Both derived, and both gate the three post-filters. */
    uint8_t coded_lossless;
    uint8_t all_lossless;

    uint8_t loop_filter_level[4];
    uint8_t loop_filter_sharpness;
    uint8_t loop_filter_delta_enabled;
    int8_t loop_filter_ref_deltas[TINY_AV1_TOTAL_REFS];
    int8_t loop_filter_mode_deltas[2];

    uint8_t cdef_damping;
    uint8_t cdef_bits;
    uint8_t cdef_y_pri[8];
    uint8_t cdef_y_sec[8];
    uint8_t cdef_uv_pri[8];
    uint8_t cdef_uv_sec[8];

    uint8_t restoration_type[3];
    uint16_t restoration_size[3];
    uint8_t uses_lr;

    uint8_t tx_mode;
    uint8_t reduced_tx_set;

    uint32_t tile_cols;
    uint32_t tile_rows;
    uint8_t tile_cols_log2;
    uint8_t tile_rows_log2;
    uint32_t mi_col_starts[TINY_AV1_MAX_TILE_COLS + 1];
    uint32_t mi_row_starts[TINY_AV1_MAX_TILE_ROWS + 1];
    uint32_t context_update_tile_id;
    uint8_t tile_size_bytes;
} TinyAv1Frame;

/**
 * @brief Reads the sequence header out of one OBU payload.
 *
 * @param sequence Receives it.
 * @param data First byte of the payload.
 * @param size Bytes in the payload.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_read_sequence(
    TinyAv1Sequence* sequence, const uint8_t* data, size_t size
);

/**
 * @brief Reads the frame header out of one OBU payload.
 *
 * Only a still picture's shape is accepted: one shown key frame, which is what
 * an AVIF item carries. Anything referencing another frame reports
 * TINYIMG_ERR_UNSUPPORTED_VARIANT rather than being half read.
 *
 * @param frame Receives it.
 * @param sequence The sequence header, which most of this branches on.
 * @param data First byte of the payload.
 * @param size Bytes in the payload.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_read_frame(
    TinyAv1Frame* frame, const TinyAv1Sequence* sequence, const uint8_t* data,
    size_t size, size_t* consumed
);

/**
 * @brief Where one tile's bytes are inside a tile group payload.
 *
 * The payload starts with a header of its own: a flag when the frame has more
 * than one tile, then a byte alignment, then either one tile or a length-
 * prefixed list of them. Resolving that is separate from walking a tile so a
 * caller can hand the walk a byte range and nothing else.
 *
 * @param frame The frame header, for the tile layout.
 * @param data First byte of the tile group payload.
 * @param size Bytes in the payload.
 * @param index Which tile, in the frame's raster order.
 * @param tile Receives the first byte of that tile's data.
 * @param tile_size Receives how many bytes it holds.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_BOUNDS for an index the
 * group does not carry, or TINYIMG_ERR_CORRUPT for a truncated payload.
 */
int tiny_av1_tile_bytes(
    const TinyAv1Frame* frame, const uint8_t* data, size_t size, uint32_t index,
    const uint8_t** tile, size_t* tile_size
);

/** OBU types, as the specification numbers them. */
typedef enum TinyAv1ObuType
{
    TINY_AV1_OBU_SEQUENCE_HEADER = 1,
    TINY_AV1_OBU_TEMPORAL_DELIMITER = 2,
    TINY_AV1_OBU_FRAME_HEADER = 3,
    TINY_AV1_OBU_TILE_GROUP = 4,
    TINY_AV1_OBU_METADATA = 5,
    TINY_AV1_OBU_FRAME = 6,
    TINY_AV1_OBU_REDUNDANT_FRAME_HEADER = 7,
    TINY_AV1_OBU_TILE_LIST = 8,
    TINY_AV1_OBU_PADDING = 15
} TinyAv1ObuType;

/** One open bitstream unit, already located inside the item's bytes. */
typedef struct {
    uint8_t type;
    const uint8_t* payload;
    size_t size;
} TinyAv1Obu;

/**
 * @brief Walks to the next OBU.
 *
 * `at` is advanced past the unit returned. An OBU without a size field runs to
 * the end of what it was given, which is legal only as the last one.
 *
 * @param data First byte of the item.
 * @param size Bytes in the item.
 * @param at Read cursor, advanced.
 * @param obu Receives the unit.
 * @return int Non-zero while a unit was read.
 */
int tiny_av1_next_obu(
    const uint8_t* data, size_t size, size_t* at, TinyAv1Obu* obu
);

/**
 * @brief The distributions one tile adapts as it decodes.
 *
 * Every symbol read updates the distribution it came from, so a tile needs its
 * own mutable copy of each default and the tables in `av1-tables.h` stay
 * `const`. The specification calls these the `Tile` prefixed arrays and resets
 * them at each tile's `init_symbol`.
 *
 * Only what an intra still picture reads is here. The inter distributions exist
 * in the tables and are never copied, so `--gc-sections` drops them: a frame
 * with no references cannot reach a reference distribution, and carrying one
 * would be dead state that a future bug could read.
 *
 * The four `delta_lf_multi` entries are four independent copies of one default,
 * which is what the specification asks for and is easy to misread as one.
 */
typedef struct {
    uint16_t intra_frame_y_mode[5][5][14];
    uint16_t y_mode[4][14];
    uint16_t uv_mode_cfl_allowed[13][15];
    uint16_t uv_mode_cfl_not_allowed[13][14];
    uint16_t angle_delta[8][8];
    uint16_t cfl_alpha[6][17];
    uint16_t cfl_sign[9];
    uint16_t partition_w8[4][5];
    uint16_t partition_w16[4][11];
    uint16_t partition_w32[4][11];
    uint16_t partition_w64[4][11];
    uint16_t partition_w128[4][9];
    uint16_t skip[3][3];
    uint16_t segment_id[3][9];
    uint16_t delta_q[5];
    uint16_t delta_lf[5];
    uint16_t tx_8x8[3][3];
    uint16_t tx_16x16[3][4];
    uint16_t tx_32x32[3][4];
    uint16_t tx_64x64[3][4];
    uint16_t txfm_split[21][3];
    uint16_t filter_intra[22][3];
    uint16_t filter_intra_mode[6];
    uint16_t palette_y_size[7][8];
    uint16_t palette_uv_size[7][8];
    uint16_t palette_y_mode[7][3][3];
    uint16_t palette_uv_mode[2][3];
    uint16_t intrabc[3];
    uint16_t intra_tx_type_set1[2][13][8];
    uint16_t intra_tx_type_set2[3][13][6];
    uint16_t txb_skip[4][5][13][3];
    uint16_t eob_extra[4][5][2][9][3];
    uint16_t coeff_base[4][5][2][42][5];
    uint16_t coeff_base_eob[4][5][2][4][4];
    uint16_t coeff_br[4][5][2][21][5];
    uint16_t dc_sign[4][2][3][3];
    uint16_t restoration_type[4];
    uint16_t use_wiener[3];
    uint16_t use_sgrproj[3];
    uint16_t eob_pt_16[4][2][2][6];
    uint16_t eob_pt_32[4][2][2][7];
    uint16_t eob_pt_64[4][2][2][8];
    uint16_t eob_pt_128[4][2][2][9];
    uint16_t eob_pt_256[4][2][2][10];
    uint16_t eob_pt_512[4][2][11];
    uint16_t eob_pt_1024[4][2][12];
    uint16_t palette_size_2_y_color[5][3];
    uint16_t palette_size_3_y_color[5][4];
    uint16_t palette_size_4_y_color[5][5];
    uint16_t palette_size_5_y_color[5][6];
    uint16_t palette_size_6_y_color[5][7];
    uint16_t palette_size_7_y_color[5][8];
    uint16_t palette_size_8_y_color[5][9];
    uint16_t palette_size_2_uv_color[5][3];
    uint16_t palette_size_3_uv_color[5][4];
    uint16_t palette_size_4_uv_color[5][5];
    uint16_t palette_size_5_uv_color[5][6];
    uint16_t palette_size_6_uv_color[5][7];
    uint16_t palette_size_7_uv_color[5][8];
    uint16_t palette_size_8_uv_color[5][9];
    uint16_t mv_joint[5];
    uint16_t mv_class[2][12];
    uint16_t mv_class0_bit[3];
    uint16_t mv_class0_fr[2][2][5];
    uint16_t mv_class0_hp[3];
    uint16_t mv_bit[10][3];
    uint16_t mv_fr[2][5];
    uint16_t mv_hp[3];
    uint16_t mv_sign[3];

    /** Four adapting copies of the single `delta_lf` default. */
    uint16_t delta_lf_multi[4][5];
} TinyAv1Cdf;

/**
 * @brief Resets every distribution to its default.
 *
 * Called at the start of each tile. A frame with more than one tile decodes
 * each from the same starting state, which is what makes tiles independent.
 *
 * @param cdf Receives the copies.
 */
void tiny_av1_cdf_init(TinyAv1Cdf* cdf);

// #region symbol decoder

/**
 * @brief The multi-symbol arithmetic decoder, and the one stage nothing can
 * skip.
 *
 * Every symbol updates the distribution it was read from, so the decoder's
 * state depends on the whole prefix of the bitstream. That is what makes a tile
 * serial and what makes entropy decoding proportional to the source rather than
 * to the size anyone asked for. Tiles are the only seam, because each one
 * resets this.
 */
typedef struct {
    const uint8_t* data;
    size_t size;

    /** Bit position into `data`, most significant bit of a byte first. */
    size_t bit;

    /** The specification's SymbolValue and SymbolRange. */
    uint32_t value;
    uint32_t range;

    /**
     * The specification's SymbolMaxBits, which is bits still available and is
     * allowed to go negative.
     *
     * Negative means every real bit has been read and its magnitude counts
     * padding zeros the decode has consumed. Those zeros are not in the
     * bitstream, so a reader past the end must return them rather than fail:
     * a conformant tile ends inside that padding.
     */
    int32_t max_bits;

    /** The frame header's `disable_cdf_update`, which freezes adaptation. */
    uint8_t frozen;
} TinyAv1Symbol;

/**
 * @brief Starts a symbol decoder over one tile's bytes.
 *
 * @param symbol Receives the state.
 * @param data First byte of the tile.
 * @param size Bytes in the tile.
 */
void tiny_av1_symbol_init(
    TinyAv1Symbol* symbol, const uint8_t* data, size_t size
);

/**
 * @brief Reads one symbol from an adapting distribution and updates it.
 *
 * `cdf` holds `count` cumulative values followed by an adaptation counter, so
 * an N-symbol distribution occupies N + 1 entries. That layout is the
 * specification's and is what `av1-tables.h` emits.
 *
 * @param symbol The decoder.
 * @param cdf The distribution, updated in place.
 * @param count How many symbols it distinguishes.
 * @return uint32_t The symbol, in `[0, count)`.
 */
uint32_t tiny_av1_symbol_read(
    TinyAv1Symbol* symbol, uint16_t* cdf, uint32_t count
);

/**
 * @brief Reads one bit against a fixed even distribution.
 *
 * @param symbol The decoder.
 * @return uint32_t The bit.
 */
uint32_t tiny_av1_symbol_bit(TinyAv1Symbol* symbol);

/**
 * @brief Reads `count` bits against fixed even distributions, most significant
 * first.
 *
 * @param symbol The decoder.
 * @param count How many bits, at most 32.
 * @return uint32_t The value.
 */
uint32_t tiny_av1_symbol_literal(TinyAv1Symbol* symbol, uint32_t count);

/**
 * @brief The arithmetic encoder, which is the decoder's inverse.
 *
 * **The specification defines no encoder**, so this is derived from the decoder
 * above rather than transcribed. The decoder splits `[0, range)` into one
 * sub-interval per symbol, where symbol `s` occupies `[cur(s), cur(s - 1))` and
 * `cur(-1)` is `range`; writing the same symbol therefore means narrowing the
 * interval to that sub-range, which works out to `low += range - cur(s - 1)`
 * and `range = cur(s - 1) - cur(s)`. Everything else here is carrying the bits
 * of `low` out in order.
 *
 * `pending` holds bytes that may still receive a carry, one per entry and wider
 * than a byte for exactly that reason: a carry out of `low` increments the last
 * entry, which may take it past 255, and one pass at the end resolves the chain
 * into bytes. The alternative is propagating carries through the output as they
 * happen, which is the same work with more places to get it wrong.
 */
typedef struct {
    /** Bytes that may still take a carry; resolved by tiny_av1_symbol_finish.
     */
    uint16_t* pending;
    size_t capacity;
    size_t count;

    /**
     * @brief The interval's lower bound, holding `16 + held` significant bits.
     *
     * Sixteen and not fifteen, and the difference is the whole correctness of
     * the carry. `range` is renormalized into `[2^15, 2^16)`, so a retained
     * window of sixteen bits guarantees `low + range < 2^17`, which is one
     * carry out of the window and no more; a fifteen bit window allows two, and
     * absorbing a carry of two by incrementing one byte corrupts the stream.
     * That failed after twenty thousand symbols of a skewed distribution and
     * passed a million symbols of a uniform one.
     *
     * `held` starts at -1 so the first window is the fifteen bits the reader
     * takes at initialization, and rises to `[0, 8)` after the first byte
     * leaves.
     */
    uint64_t low;
    uint32_t range;
    int32_t held;

    /** Set once the buffer has run out, so the caller learns at the end. */
    uint8_t overflow;
} TinyAv1SymbolEnc;

/**
 * @brief Starts an encoder over a caller-owned scratch buffer.
 *
 * @param enc Receives the state.
 * @param pending Scratch, one entry per output byte plus a little; a tile
 * cannot need more entries than its own byte count.
 * @param capacity Entries in `pending`.
 */
void tiny_av1_symbol_enc_init(
    TinyAv1SymbolEnc* enc, uint16_t* pending, size_t capacity
);

/**
 * @brief Writes one symbol against a distribution and adapts it.
 *
 * The adaptation is the decoder's, unchanged, because the two have to walk the
 * same states in the same order or every symbol after the first disagrees.
 *
 * @param enc The encoder.
 * @param cdf The distribution, updated in place.
 * @param count How many symbols it distinguishes.
 * @param symbol Which one, in `[0, count)`.
 */
void tiny_av1_symbol_write(
    TinyAv1SymbolEnc* enc, uint16_t* cdf, uint32_t count, uint32_t symbol
);

/**
 * @brief Writes one symbol without adapting the distribution.
 *
 * The partition tree's two collapsed cases need it: their distribution is built
 * on the fly by summing the intervals of the symbols it stands for, and the
 * reader throws it away rather than adapting it. Adapting a copy the reader
 * does not would desynchronise nothing here and everything on the next block.
 *
 * @param enc The encoder.
 * @param cdf The distribution, left alone.
 * @param count How many symbols it distinguishes.
 * @param symbol Which one.
 */
void tiny_av1_symbol_write_frozen(
    TinyAv1SymbolEnc* enc, const uint16_t* cdf, uint32_t count, uint32_t symbol
);

/**
 * @brief Writes one bit against a fixed even distribution.
 *
 * @param enc The encoder.
 * @param bit The bit.
 */
void tiny_av1_symbol_write_bit(TinyAv1SymbolEnc* enc, uint32_t bit);

/**
 * @brief Writes `count` bits, most significant first.
 *
 * @param enc The encoder.
 * @param value The value.
 * @param count How many bits, at most 32.
 */
void tiny_av1_symbol_write_literal(
    TinyAv1SymbolEnc* enc, uint32_t value, uint32_t count
);

/**
 * @brief Finishes the interval and resolves every carry into bytes.
 *
 * The value written is the one the specification's exit process requires: the
 * interval is at least 2^15 wide, so it contains an odd multiple of 2^14, and
 * choosing that one puts a single 1 bit at the end of the tile followed by
 * zeros, which is what makes the trailing bits conformant rather than merely
 * decodable.
 *
 * @param enc The encoder.
 * @param out Receives the tile's bytes.
 * @param capacity Bytes in `out`.
 * @param size Receives how many were written.
 * @return int TINYIMG_OK, or TINYIMG_ERR_BUFFER_TOO_SMALL if either buffer ran
 * out at any point.
 */
int tiny_av1_symbol_finish(
    TinyAv1SymbolEnc* enc, uint8_t* out, size_t capacity, size_t* size
);

// #region tile walk

/**
 * @brief What the partition tree hands to whatever consumes a block.
 *
 * The sink is a parameter rather than a call to a fixed function so the tree's
 * geometry can be tested on its own. The tree is where off-by-ones live: a
 * block at the frame's right or bottom edge is coded without its missing half,
 * and the sub-block offsets for the eight non-square partitions are each
 * written out separately in the specification.
 */
typedef struct TinyAv1Walk TinyAv1Walk;

/** The residual reader's state, declared below where its own region begins. */
typedef struct TinyAv1Residual TinyAv1Residual;

/**
 * @brief Called once per coded block, in bitstream order.
 *
 * @param walk The walk in progress.
 * @param row Block row, in units of four luma samples.
 * @param col Block column, in the same units.
 * @param size Which block size.
 * @return int TINYIMG_OK, or a negative TinyImageError to abandon the tile.
 */
typedef int (*TinyAv1BlockFn)(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
);

/**
 * @brief What one block's mode info says, before any coefficient is read.
 *
 * Everything `intra_frame_mode_info` and `read_block_tx_size` produce, in one
 * structure, because the coefficient parse and the prediction both read all of
 * it and neither wants to know which syntax element each field came from.
 */
typedef struct {
    /** Block position and size, the same three the sink is handed. */
    uint32_t row;
    uint32_t col;
    TinyAv1BlockSize size;

    /** Non-zero when the block carries no residual. */
    uint8_t skip;
    /** Which segment, or zero when the frame is not segmented. */
    uint8_t segment_id;
    /** Non-zero when this segment's quantizer makes the block lossless. */
    uint8_t lossless;

    /** Luma prediction mode, a TinyAv1IntraMode below UV_CFL_PRED. */
    uint8_t y_mode;
    /** Chroma prediction mode, a TinyAv1IntraMode; only set when has_chroma. */
    uint8_t uv_mode;
    /** Non-zero when the block has chroma of its own to predict. */
    uint8_t has_chroma;

    /** Angle offsets for the directional modes, -3 to 3. */
    int8_t angle_delta_y;
    int8_t angle_delta_uv;

    /** Chroma-from-luma scaling, -16 to 16, when uv_mode is CFL. */
    int8_t cfl_alpha_u;
    int8_t cfl_alpha_v;

    /** Non-zero when the recursive filter predictor is used instead. */
    uint8_t use_filter_intra;
    /** Which of the five filters, when it is. */
    uint8_t filter_intra_mode;

    /** The transform size the whole block uses; intra never splits it. */
    uint8_t tx_size;

    /** The quantizer this block codes against, after any delta. */
    uint8_t q_index;
    /** Loop filter deltas in force, one per FRAME_LF_COUNT entry. */
    int8_t delta_lf[4];
    /** The CDEF strength index for the 64x64 this block sits in, or -1. */
    int8_t cdef_idx;
} TinyAv1Mode;

struct TinyAv1Walk {
    TinyAv1Symbol* symbol;
    TinyAv1Cdf* cdf;
    const TinyAv1Sequence* sequence;
    const TinyAv1Frame* frame;

    /**
     * The block size at each mi position, which the partition context reads
     * from its neighbors above and to the left.
     *
     * Only the sizes are kept, because that is all the partition context needs;
     * a decoder that also predicted from neighbors would keep more.
     */
    uint8_t* sizes;

    /**
     * @brief The neighbor context the mode info reads, one entry per mi
     * position.
     *
     * Four frame-sized arrays rather than fields on a block, because every
     * context in 9.3 is expressed as a lookup at `MiRow - 1` or `MiCol - 1` and
     * those neighbors belong to blocks that have already been decoded and
     * forgotten. The caller allocates them so a tile walk holds no memory of
     * its own.
     *
     * All four may be NULL, and then the mode info is not read at all: the walk
     * reports the partition geometry and nothing else, which is what the
     * geometry tests drive.
     */
    uint8_t* y_modes;
    /** Whether each position's block coded no residual. */
    uint8_t* skips;
    /** Which segment each position is in. */
    uint8_t* segment_ids;
    /** The transform size at each position, for the tx depth context. */
    uint8_t* tx_sizes;
    /**
     * @brief The chroma mode at each position, or NULL when nothing reads it.
     *
     * Written only where a block has chroma of its own, which is what the
     * specification's `UVModes` holds. The intra edge filter is the one reader:
     * it asks whether a neighbor predicts smoothly, and for a chroma plane the
     * answer is on whichever of a 4xN pair coded the chroma.
     */
    uint8_t* uv_modes;

    /**
     * @brief One CDEF strength index per 64x64, or NULL to skip reading them.
     *
     * Indexed by the 64x64 grid rather than by mi position, because that is the
     * unit the strength is coded at: the first block of each 64x64 carries it
     * and the rest of that square inherits it. `-1` means not yet read, which
     * is how the specification spells the same thing.
     */
    int8_t* cdef_idx;
    /** Entries per row of `cdef_idx`. */
    uint32_t cdef_stride;

    /** The tile being walked, in mi units, as a half-open rectangle. */
    uint32_t row_start;
    uint32_t row_end;
    uint32_t col_start;
    uint32_t col_end;

    /** Called per block. */
    TinyAv1BlockFn block;

    /**
     * @brief Called once per superblock, before its partition tree.
     *
     * The seam for everything the specification resets at that point: the
     * decoded-position flags the prediction reads, and the loop restoration
     * units a frame carrying them would code. NULL to do neither.
     */
    int (*superblock)(TinyAv1Walk* walk, uint32_t row, uint32_t col);

    /**
     * @brief The mode info of the block being handed to the sink.
     *
     * Filled in before every call when the context arrays are present, so the
     * sink reads it rather than being passed a dozen arguments. Zeroed when
     * they are not.
     */
    TinyAv1Mode mode;

    /**
     * @brief The quantizer index in force, carried across blocks.
     *
     * `delta_q_present` makes it a running value rather than a frame constant,
     * which is why it lives on the walk and not on the frame.
     */
    uint8_t q_index;
    /** The loop filter deltas in force, carried the same way. */
    int8_t delta_lf[TINY_AV1_FRAME_LF_COUNT];

    /**
     * @brief Whether the next block's mode info codes the quantizer deltas.
     *
     * `ReadDeltas` in the specification, and it is **per superblock, not per
     * block**: `decode_tile` sets it from `delta_q_present` before each
     * superblock and `intra_frame_mode_info` clears it once it has read them.
     * Reading a delta for every block instead consumes symbols the encoder
     * never wrote, which desynchronises the tile a few blocks in rather than
     * immediately, so it looks like a rare corruption bug.
     */
    uint8_t read_deltas;

    /**
     * @brief The residual reader, or NULL to read no coefficients at all.
     *
     * A walk with this NULL reports the partition geometry and, when the
     * context arrays are present, the mode info; it does not stay synchronised
     * with a real bitstream past the first block that codes a residual, which
     * is what the geometry and mode tests drive it as.
     */
    TinyAv1Residual* residual;

    /** Whatever the caller wants to reach from the sink. */
    void* context;

    /** The first failure, latched, so the recursion unwinds without checking.
     */
    int error;
};

/**
 * @brief Walks one tile's partition trees, calling the sink per block.
 *
 * Superblock by superblock in raster order, each one a recursive partition. The
 * caller has already started the symbol decoder over the tile's bytes and reset
 * the distributions, because both are per tile.
 *
 * @param walk The walk, filled in by the caller.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_walk_tile(TinyAv1Walk* walk);

/**
 * @brief Undoes the interleaving a segment id is coded with.
 *
 * The specification's own function. A segment id is coded as a difference from
 * a prediction, interleaved around it so a smooth map spends few bits, and this
 * is what turns the coded value back into an id.
 *
 * Exposed because the property that makes it right is otherwise unreachable:
 * for a fixed prediction and range the map from coded value to id has to be a
 * **bijection** onto the range, since every id has to be codable and no two may
 * collide. An implementation that drops a case still returns plausible ids.
 *
 * @param diff The coded value.
 * @param ref The prediction from the neighbors.
 * @param max One past the largest id, which is `LastActiveSegId + 1`.
 * @return uint32_t The segment id.
 */
uint32_t tiny_av1_neg_deinterleave(uint32_t diff, uint32_t ref, uint32_t max);

/**
 * @brief Reads one block's mode info, filling in `walk->mode`.
 *
 * Specification 5.11.6 for an intra frame, plus the transform size. Called from
 * the walk's sink rather than by the walk itself, because the sink is what
 * decides whether the block's own symbols are read at all: the geometry tests
 * drive the tree with no mode info, and a decode reads it before the
 * coefficients.
 *
 * The order of the reads is the contract. Everything comes from one arithmetic
 * decoder, so an element read in the wrong place shifts every block after it.
 *
 * @param walk The walk in progress, whose context arrays must all be present.
 * @param row Block row in mi units.
 * @param col Block column in mi units.
 * @param size The block size the partition tree produced.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL when a context array is missing, or
 * TINYIMG_ERR_UNSUPPORTED_VARIANT for a block that would code a palette.
 */
int tiny_av1_read_mode(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
);

/**
 * @brief Whether a frame is one this decoder reads the mode info of.
 *
 * Two things it refuses, and both are refused at the frame rather than at the
 * block so a caller learns before any pixel is decoded:
 *
 * - **Intra block copy.** `allow_intrabc` makes an intra frame code motion
 *   vectors into itself, which is a prediction mode rather than a variation on
 *   one.
 * - **Palette.** A screen-content frame codes a color index map per block, and
 *   nothing photographic uses it. `allow_screen_content_tools` is the frame
 *   level flag; a frame that sets it and then codes no palette is refused too,
 *   which is the conservative direction rather than the exact one.
 *
 * @param sequence The sequence header.
 * @param frame The frame header.
 * @return int TINYIMG_OK or TINYIMG_ERR_UNSUPPORTED_VARIANT.
 */
int tiny_av1_frame_supported(
    const TinyAv1Sequence* sequence, const TinyAv1Frame* frame
);

// #region residual

/**
 * Positions a context array needs past the frame's own width or height.
 *
 * A block is coded whole even where the frame ends inside it, so the largest
 * one, 128 samples on a side, can start on the frame's last mi column and write
 * thirty-one entries past it.
 */
#define TINY_AV1_CONTEXT_PAD 32

/** Largest coefficient array a transform block reads, which is 32x32. */
#define TINY_AV1_MAX_COEFFS 1024

/**
 * @brief One transform block, parsed and ready to reconstruct.
 *
 * `quant` is row major with `stride` entries per row, `stride` being
 * `Min(32, Tx_Width)`: a transform wider or taller than 32 carries
 * coefficients only in its top left 32x32 and the specification indexes them
 * by the clamped width, not the real one.
 */
typedef struct {
    /** 0 for luma, 1 and 2 for the two chroma planes. */
    uint32_t plane;
    /** Top left sample of the block, in that plane's own coordinates. */
    uint32_t x;
    uint32_t y;
    uint8_t tx_size;
    /** The transform type, already resolved through the plane's own rules. */
    uint8_t tx_type;
    /** Non-zero when this block is coded losslessly, so the WHT applies. */
    uint8_t lossless;

    /**
     * One past the last coefficient in scan order, or zero for a block that
     * codes none. A skipped block and an `all_zero` block both report zero.
     */
    uint32_t eob;

    /**
     * Quantized levels, zero everywhere the scan did not reach.
     *
     * NULL when `eob` is zero. A block that codes no residual leaves the
     * reader's buffer holding the previous transform block's levels, and
     * handing back a pointer to those is the one way this structure could be
     * misread.
     */
    const int32_t* quant;
    uint32_t stride;
} TinyAv1TxBlock;

/**
 * @brief Called once per transform block, after its coefficients are parsed.
 *
 * Called for every transform block the residual walk visits, including one that
 * codes nothing, because a block with no residual is still predicted.
 *
 * @param walk The walk in progress; `walk->mode` is the block it belongs to.
 * @param block The transform block.
 * @return int TINYIMG_OK, or a negative TinyImageError to abandon the tile.
 */
typedef int (*TinyAv1TxFn)(TinyAv1Walk* walk, const TinyAv1TxBlock* block);

/**
 * @brief What the coefficient parse carries from one transform block to the
 * next.
 *
 * The four context arrays are the specification's `AboveLevelContext`,
 * `AboveDcContext`, `LeftLevelContext` and `LeftDcContext`, each three planes
 * deep and indexed by that plane's own column or row in units of four samples.
 * The caller allocates them so a tile walk owns no memory, the same way the mi
 * arrays on the walk work.
 *
 * **Both strides need `TINY_AV1_CONTEXT_PAD` past the frame.** A transform
 * block that starts inside the frame may extend past its right or bottom edge,
 * and the specification writes the context across the block's whole width while
 * reading it back only inside the frame. Writing unconditionally into a padded
 * array is what makes those two agree without a branch per entry.
 */
struct TinyAv1Residual {
    /** `cul_level` per position, `above_stride` entries per plane. */
    uint8_t* above_level;
    /** The dc sign category, 0 for none, 1 for negative and 2 for positive. */
    uint8_t* above_dc;
    /** The same two down the left edge, `left_stride` entries per plane. */
    uint8_t* left_level;
    uint8_t* left_dc;

    uint32_t above_stride;
    uint32_t left_stride;

    /** The block being read, handed to `block` and reused by the next one. */
    int32_t quant[TINY_AV1_MAX_COEFFS];

    /** Called per transform block, or NULL to parse and discard. */
    TinyAv1TxFn block;
};

/**
 * @brief Writes one transform block's coefficients.
 *
 * The mirror of the parse, in the same file and using the same context
 * derivations, because the two have to select the same distribution for every
 * symbol and the only way to be sure of that is for there to be one copy of
 * each derivation.
 *
 * @param residual The context arrays, updated as the parse would update them.
 * @param enc The symbol encoder.
 * @param cdf The distributions, adapted as the parse would adapt them.
 * @param frame The frame header, for the quantizer and the transform set.
 * @param sequence The sequence header, for the subsampling.
 * @param plane Which plane.
 * @param x Top left sample of the block in that plane's coordinates.
 * @param y The same, vertically.
 * @param tx_size Which transform.
 * @param tx_type Which type, which must be one the block's set carries.
 * @param y_mode The block's luma prediction mode, which is what selects the
 * transform type's own distribution: the reader indexes it by the intra
 * direction, and a writer that indexed it by anything else adapts a row the
 * reader never touches.
 * @param levels The quantized levels, row major at the transform's width.
 * @param eob One past the last non-zero level in scan order, or zero for none.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_write_coeffs(
    TinyAv1Residual* residual, TinyAv1SymbolEnc* enc, TinyAv1Cdf* cdf,
    const TinyAv1Frame* frame, const TinyAv1Sequence* sequence, uint32_t plane,
    uint32_t x, uint32_t y, uint32_t tx_size, uint32_t tx_type, uint32_t y_mode,
    const int32_t* levels, uint32_t eob
);

/**
 * @brief Encodes one image as an AV1 still picture, as a sequence of OBUs.
 *
 * Fixed partitions and no rate-distortion search, which is what makes it one
 * pass: every block is 8x8, the luma mode is picked from four candidates by
 * absolute difference, and chroma always predicts DC.
 *
 * @param image The pixels.
 * @param quality 1 to 100; the quantizer index is derived from it.
 * @param out Receives the OBUs.
 * @param capacity Bytes in `out`.
 * @param size Receives how many were written.
 * @param frame Receives the frame's own extents and plane count, which the
 * container has to describe.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_encode(
    const TinyImage* image, uint32_t quality, uint8_t* out, size_t capacity,
    size_t* size, TinyAv1Sequence* frame
);

/**
 * @brief Clears the above context, which happens once per tile.
 *
 * @param walk The walk, whose residual reader must be set.
 */
void tiny_av1_residual_clear_above(TinyAv1Walk* walk);

/**
 * @brief Clears the left context, which happens once per superblock row.
 *
 * @param walk The walk, whose residual reader must be set.
 */
void tiny_av1_residual_clear_left(TinyAv1Walk* walk);

/**
 * @brief Reads the residual of the block whose mode info the walk holds.
 *
 * Specification 5.11.34 through 5.11.39: the transform blocks of one coded
 * block, in bitstream order, each one parsed and handed to the sink.
 *
 * Call it immediately after tiny_av1_read_mode for the same block and before
 * the walk moves on, because both read from the same arithmetic decoder.
 *
 * **The specification predicts before it parses and this does not**, which is
 * a reordering rather than a change: the parse reads no pixel, so nothing in it
 * depends on a prediction, and doing the pixels in the sink keeps this file to
 * the bitstream. A caller that needs the specification's order can predict
 * inside the sink, which is the same place the reconstruction goes.
 *
 * @param walk The walk in progress.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL when the residual reader or a
 * context array is missing, or TINYIMG_ERR_CORRUPT for a coefficient the
 * specification gives no encoding of.
 */
int tiny_av1_read_residual(TinyAv1Walk* walk);

/**
 * @brief Decodes one AV1 item into an image.
 *
 * The whole of phase 1 behind one call: the OBU walk, both headers, every tile,
 * and the conversion of three planes into pixels. `avif.c` calls this with the
 * bytes of one `av01` item and owns everything outside it.
 *
 * The region and the scale in `opts` are honored on output rather than in the
 * decode, and the difference matters for what a request costs: a tile outside
 * the region is skipped whole, because a tile resets the arithmetic decoder,
 * but the reconstruction of a tile that is needed is full resolution. A scaled
 * request therefore costs what a full one costs plus a cheaper resample, which
 * is the opposite of JPEG's scaled decode and is worth saying rather than
 * implying.
 *
 * @param image Receives the pixels; the caller owns `image->data` afterwards.
 * @param data First byte of the item.
 * @param size Bytes in the item.
 * @param opts The region, scale, channel count and effort, or NULL for all of
 * it at the file's own extent.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_decode(
    TinyImage* image, const uint8_t* data, size_t size,
    const TinyDecodeOpts* opts
);

// #region post filters

/**
 * @brief What the two post-filters read, which is a decoded frame and the mode
 * info that produced it.
 *
 * A structure rather than a dozen parameters, because both filters want all of
 * it and neither wants to know how the reconstruction stored it. The caller
 * owns every pointer.
 */
typedef struct {
    const TinyAv1Sequence* sequence;
    const TinyAv1Frame* frame;

    /** The reconstructed planes, modified in place by the loop filter. */
    uint8_t* plane[3];
    uint32_t stride[3];
    uint32_t width[3];
    uint32_t height[3];
    uint32_t planes;

    /** The block size at each mi position, the specification's `MiSizes`. */
    const uint8_t* sizes;
    /** Whether each position's block coded no residual. */
    const uint8_t* skips;
    /** Which segment each position is in. */
    const uint8_t* segment_ids;

    /**
     * @brief The transform size at each position of each plane.
     *
     * `LoopfilterTxSizes`, which is not the walk's `tx_sizes`: that one is the
     * block's own size in luma units, and this is the size the plane actually
     * transformed at, which differs on a subsampled plane and again wherever a
     * transform is smaller than its block.
     */
    const uint8_t* lf_tx[3];

    /**
     * @brief Four loop filter deltas per mi position, or NULL for none.
     *
     * Allocated only by a frame that codes them, which no file measured does.
     * The filter reads the frame's own levels when this is NULL.
     */
    const int8_t* delta_lf;

    /** One CDEF strength index per 64x64, and entries per row of it. */
    const int8_t* cdef_idx;
    uint32_t cdef_stride;
} TinyAv1Filter;

/**
 * @brief Runs the deblocking filter over a decoded frame, in place.
 *
 * Every vertical edge of every plane before any horizontal one, which is the
 * filter's one ordering constraint; within a pass the order does not matter,
 * which is what the specification's own note says and what would make a
 * parallel implementation possible.
 *
 * @param filter The frame and its mode info.
 */
void tiny_av1_loop_filter(const TinyAv1Filter* filter);

/**
 * @brief Runs CDEF over a decoded frame into a second one.
 *
 * Every tap reads the unfiltered frame, so this cannot be done in place: a
 * block would otherwise filter from samples an earlier block had already
 * changed, which is a different picture rather than a rounding difference.
 *
 * @param filter The frame and its mode info, read but not modified.
 * @param out One plane pointer per plane, at the same strides, which receive
 * the filtered frame. A block CDEF does not touch is copied through.
 */
void tiny_av1_cdef(const TinyAv1Filter* filter, uint8_t* const* out);

// #region transforms

/**
 * @brief Runs the 2D inverse transform over one block of dequantized
 * coefficients.
 *
 * `block` is row major with `stride` elements per row and holds at least the
 * transform's height rows. On return it holds the residual, which the caller
 * adds to the prediction and clamps.
 *
 * A transform larger than 32 in either axis carries coefficients only in its
 * top left 32x32, which the caller has already zeroed beyond; this honors that
 * rather than rediscovering it.
 *
 * **The flip is applied here, so the caller must not apply it again.** The
 * specification splits this differently: 7.13.3 never flips, and 7.12.3 step 3
 * derives `flipUD` and `flipLR` and writes the residual to a mirrored position.
 * This function reverses the rows or columns itself and hands back a residual
 * to add in raster order, because a caller that never sees the axes cannot get
 * them the wrong way round. A caller that implements 7.12.3 verbatim on top of
 * this will mirror six of the sixteen transform types twice, and the result
 * looks nearly right, which is the worst kind of wrong.
 *
 * @param block The coefficients, replaced by the residual, flip included.
 * @param stride Elements per row of `block`.
 * @param tx_size Which transform.
 * @param tx_type Which pair of one dimensional transforms.
 * @param bit_depth 8, 10 or 12.
 * @return int TINYIMG_OK, or TINYIMG_ERR_CORRUPT for a size and type the
 * specification gives no value for.
 */
int tiny_av1_inverse_transform(
    int32_t* block, uint32_t stride, TinyAv1TxSize tx_size,
    TinyAv1TxType tx_type, uint32_t bit_depth
);

/**
 * @brief Runs the inverse Walsh-Hadamard transform, which the lossless mode
 * uses.
 *
 * Separate from tiny_av1_inverse_transform because `Lossless` carries no size
 * or type to dispatch on: it is always TX_4X4 with DCT_DCT, so both parameters
 * would be constants and the caller would be choosing between two functions
 * anyway.
 *
 * @param block The coefficients, replaced by the residual.
 * @param stride Elements per row of `block`.
 * @param bit_depth 8, 10 or 12, which the between-pass clip depends on.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_inverse_wht(int32_t* block, uint32_t stride, uint32_t bit_depth);

// #region intra prediction

/**
 * @brief What a prediction needs beyond its neighbors and its mode.
 */
typedef struct {
    /** Non-zero when the block above is inside the tile and already decoded. */
    uint8_t have_above;
    /** Non-zero when the block to the left is. */
    uint8_t have_left;

    /**
     * @brief How many samples of `above` and `left` are real.
     *
     * `above[1 + i]` is a real sample for `i < above_available` and the last
     * real one is replicated past that, which is what the specification does
     * with `aboveLimit`. A directional mode reads up to twice the block's
     * extent, so a caller that has the samples should pass them: with
     * `haveAboveRight` the count runs to `2 * w`, clamped by the frame's own
     * right edge.
     */
    uint16_t above_available;
    uint16_t left_available;

    /** Signed multiple of 3 degrees applied to a directional mode. */
    int8_t angle_delta;

    /** Non-zero to run a filter intra mode instead of `mode`. */
    uint8_t use_filter_intra;
    /** Which one, when `use_filter_intra` is set. */
    uint8_t filter_intra_mode;

    /** The sequence header's `enable_intra_edge_filter`. */
    uint8_t edge_filter;

    /**
     * @brief Non-zero when the block above or to the left predicts smoothly.
     *
     * The specification's `filterType`, and it decides how hard the edge is
     * filtered and whether it is upsampled at all. A smooth neighbor means the
     * edge is already smooth, so it takes the gentler kernel; the caller knows
     * the neighbors' modes and this file does not.
     */
    uint8_t filter_type;

    /** 8, 10 or 12. */
    uint8_t bit_depth;
} TinyAv1PredictOpts;

/**
 * @brief Predicts one transform block from its decoded neighbors.
 *
 * **The corner sits at index 0 of both neighbor arrays**, so `above[0]` and
 * `left[0]` are the same sample, `above[1 + i]` is the specification's
 * `AboveRow[i]` and `left[1 + i]` its `LeftCol[i]`. Passing the corner in band
 * rather than at index -1 keeps every caller free of negative subscripts, which
 * is where an off-by-one in this stage would otherwise hide.
 *
 * `above` must hold `1 + 2 * w` samples and `left` `1 + 2 * h`, both filled to
 * the extent `opts` declares available; this replicates past that.
 *
 * @param dst Receives the prediction, `h` rows of `w` samples.
 * @param stride Bytes per row of `dst`.
 * @param w Transform width in samples.
 * @param h Transform height in samples.
 * @param above The row above, corner first.
 * @param left The column to the left, corner first.
 * @param mode Which prediction.
 * @param opts The rest of the context.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_predict_intra(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const uint8_t* above,
    const uint8_t* left, TinyAv1IntraMode mode, const TinyAv1PredictOpts* opts
);

/**
 * @brief Predicts chroma from the reconstructed luma of the same block.
 *
 * The luma is subsampled to the chroma grid, its average removed, and the
 * result scaled by a signed alpha per plane and added to a DC prediction that
 * `dst` already holds.
 *
 * @param dst The DC prediction, replaced by the chroma from luma prediction.
 * @param stride Bytes per row of `dst`.
 * @param w Chroma block width.
 * @param h Chroma block height.
 * @param luma Reconstructed luma at the block's own top left sample.
 * @param luma_stride Bytes per row of `luma`.
 * @param luma_w Luma samples available to the right of that point, which is
 * the specification's `MaxLumaW` measured from the block rather than from the
 * frame. The subsampling reads past the block's own width and this is what
 * stops it reading luma that has not been reconstructed yet.
 * @param luma_h Luma samples available below it.
 * @param sub_x Non-zero when chroma is horizontally subsampled.
 * @param sub_y Non-zero when chroma is vertically subsampled.
 * @param alpha Signed scale, in the specification's units.
 * @param bit_depth 8, 10 or 12.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_predict_cfl(
    uint8_t* dst, uint32_t stride, uint32_t w, uint32_t h, const uint8_t* luma,
    uint32_t luma_stride, uint32_t luma_w, uint32_t luma_h, uint32_t sub_x,
    uint32_t sub_y, int32_t alpha, uint32_t bit_depth
);

#ifdef __cplusplus
}
#endif
