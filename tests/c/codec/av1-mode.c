#include "codec/av1-item.h"

/**
 * @file
 * @brief The mode info between the partition tree and the coefficients.
 *
 * The walks here read the mode info and no coefficients, so they are
 * synchronised with a real bitstream for exactly one block: a block whose
 * `skip` is zero codes its residual straight afterwards, and this file's walks
 * step over it as though it were the next block's mode info. What that leaves
 * checkable on a real file is the first block, and that is what these cases
 * assert.
 *
 * - The **first block** of four real bitstreams, read from a synchronised
 *   decoder. Its partition symbol was computed by hand from the raw tile bytes
 *   and the specification's own arithmetic before this code ran, and the frame
 *   header's length was counted bit by bit against 5.9.2, which is what says
 *   the tile starts where this reads it.
 * - The **frame OBU** layout, which `cavif` does not produce and `avifenc`
 *   always does: one OBU carrying the frame header, a byte alignment and the
 *   tile group together.
 * - The refusals, each of which is a thing whose symbols this decoder does not
 *   read and therefore must not decode past.
 * - `coversEveryContext` drives a chosen stream, which cannot desynchronise
 *   because the reader consumes no coefficients from it, and reaches the
 *   distributions no real fixture uses.
 *
 * **The end-to-end anchor for everything here lives in `av1-coeff.c`.** With
 * the coefficient parse attached, a walk over each of these fixtures consumes
 * its tile to the last byte, and a mode info element read in the wrong place
 * or with the wrong distribution moves the bit count and lands somewhere else.
 * That check covers this file's reads as much as its own.
 */

/** What a walk saw, and the first block's mode info. */
typedef struct {
    uint32_t blocks;
    uint32_t skipped;
    uint32_t chroma;
    uint32_t cfl;
    uint32_t filter_intra;
    TinyAv1Mode first;

    /**
     * @brief Every field of every block, digested as it is read.
     *
     * The context arrays alone are not enough: the chroma-from-luma alphas and
     * the angle deltas feed no later context, so a wrong distribution for them
     * changes the picture and nothing a neighbor lookup can see. Swapping the
     * two CFL sign contexts is exactly that mutation, and it is invisible to a
     * digest of the neighbor arrays.
     */
    uint64_t digest;
} Seen;

static int record(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    Seen* seen = (Seen*) walk->context;

    int result = tiny_av1_read_mode(walk, row, col, size);
    if (result != TINYIMG_OK) return result;

    if (seen->blocks == 0u) seen->first = walk->mode;

    seen->blocks++;
    seen->skipped += walk->mode.skip;
    seen->chroma += walk->mode.has_chroma;

    if (walk->mode.uv_mode == TINY_AV1_UV_CFL_PRED) seen->cfl++;
    if (walk->mode.use_filter_intra) seen->filter_intra++;

    const uint8_t* raw = (const uint8_t*) &walk->mode;

    // field by field rather than one memcmp of the structure, because a
    // structure has padding and two runs need not agree there
    static const uint8_t fields[] = {
        offsetof(TinyAv1Mode, skip),
        offsetof(TinyAv1Mode, segment_id),
        offsetof(TinyAv1Mode, lossless),
        offsetof(TinyAv1Mode, y_mode),
        offsetof(TinyAv1Mode, uv_mode),
        offsetof(TinyAv1Mode, has_chroma),
        offsetof(TinyAv1Mode, angle_delta_y),
        offsetof(TinyAv1Mode, angle_delta_uv),
        offsetof(TinyAv1Mode, cfl_alpha_u),
        offsetof(TinyAv1Mode, cfl_alpha_v),
        offsetof(TinyAv1Mode, use_filter_intra),
        offsetof(TinyAv1Mode, filter_intra_mode),
        offsetof(TinyAv1Mode, tx_size),
        offsetof(TinyAv1Mode, q_index),
        offsetof(TinyAv1Mode, cdef_idx)
    };

    for (uint32_t i = 0; i < sizeof(fields); i++) {
        seen->digest = (seen->digest ^ raw[fields[i]]) * 1099511628211ULL;
    }

    // every field the specification bounds, checked here rather than in one
    // case: a context selection that read the wrong distribution usually still
    // lands in range, but a symbol count that is wrong for its distribution
    // does not
    if (walk->mode.y_mode >= TINY_AV1_UV_CFL_PRED) return TINYIMG_ERR_CORRUPT;
    if (walk->mode.uv_mode > TINY_AV1_UV_CFL_PRED) return TINYIMG_ERR_CORRUPT;
    if (walk->mode.tx_size > TINY_AV1_TX_64X16) return TINYIMG_ERR_CORRUPT;
    if (walk->mode.angle_delta_y < -3 || walk->mode.angle_delta_y > 3) {
        return TINYIMG_ERR_CORRUPT;
    }
    if (walk->mode.cfl_alpha_u < -16 || walk->mode.cfl_alpha_u > 16) {
        return TINYIMG_ERR_CORRUPT;
    }
    if (walk->mode.filter_intra_mode > 4u) return TINYIMG_ERR_CORRUPT;

    return TINYIMG_OK;
}

/**
 * Runs a walk with every context array present.
 *
 * The arrays are static rather than allocated, because a frame-sized byte array
 * per plane is what the caller owns in the real decoder and a test does not
 * need to model the ownership.
 */
static int walkItem(Opened* item, Seen* seen) {
    // 302x200 mi is the largest fixture, which is fox.avif at 1204x800
    static uint8_t sizes[64 * 1024];
    static uint8_t y_modes[64 * 1024];
    static uint8_t skips[64 * 1024];
    static uint8_t segment_ids[64 * 1024];
    static uint8_t tx_sizes[64 * 1024];
    static int8_t cdef[4096];
    static TinyAv1Cdf cdf;

    tiny_memset(seen, 0, sizeof(*seen));

    size_t cells = (size_t) item->frame.mi_cols * item->frame.mi_rows;
    if (cells > sizeof(sizes)) return TINYIMG_ERR_TOO_LARGE;

    tiny_memset(sizes, 0, cells);
    tiny_memset(y_modes, 0, cells);
    tiny_memset(skips, 0, cells);
    tiny_memset(segment_ids, 0, cells);
    tiny_memset(tx_sizes, 0, cells);
    tiny_memset(cdef, -1, sizeof(cdef));

    tiny_av1_cdf_init(&cdf);

    TinyAv1Symbol symbol;
    tiny_av1_symbol_init(&symbol, item->tile, item->tile_size);

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    walk.symbol = &symbol;
    walk.cdf = &cdf;
    walk.sequence = &item->sequence;
    walk.frame = &item->frame;
    walk.sizes = sizes;
    walk.y_modes = y_modes;
    walk.skips = skips;
    walk.segment_ids = segment_ids;
    walk.tx_sizes = tx_sizes;
    walk.cdef_idx = cdef;
    walk.cdef_stride = (item->frame.mi_cols + 15u) / 16u;
    walk.row_start = 0u;
    walk.row_end = item->frame.mi_rows;
    walk.col_start = 0u;
    walk.col_end = item->frame.mi_cols;
    walk.block = record;
    walk.context = seen;
    walk.q_index = item->frame.base_q_idx;

    return tiny_av1_walk_tile(&walk);
}

/**
 * A 4x4 image, whose whole tile is one block's mode info.
 *
 * The tightest case there is. A 2x2 mi frame forces the partition all the way
 * down from 64x64 without coding a symbol, because every level's second half is
 * outside the frame; the first symbol that is coded at all is the partition at
 * 8x8, and after it comes one block. Two bytes of tile hold the lot, so a
 * reader that consumed the wrong number of symbols anywhere would run off the
 * end rather than land on a plausible answer.
 */
static int readsTheSmallestFrame(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-tiny.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    // the frame OBU's header is seven bytes, counted bit by bit against 5.9.2:
    // two flags, the tile info's three, the quantizer's twenty-four, the filter
    // levels, the CDEF strengths and the two trailing flags
    r |= assertEquals((long) item.header_bytes, 7L);
    r |= assertEquals((long) item.tile_size, 2L);

    r |= assertEquals((long) item.frame.width, 4L);
    r |= assertEquals((long) item.frame.mi_cols, 2L);
    r |= assertEquals((long) item.frame.mi_rows, 2L);
    r |= assertEquals((long) item.sequence.planes, 1L);
    r |= assertEquals((long) item.frame.base_q_idx, 255L);
    r |= assertEquals((long) item.frame.tx_mode, 1L);
    r |= assertEquals((long) item.frame.using_qmatrix, 1L);
    r |= assertEquals((long) item.frame.loop_filter_level[0], 63L);
    r |= assertEquals((long) item.frame.cdef_bits, 0L);
    r |= assertEquals((long) item.frame.delta_q_present, 0L);
    r |= assertEquals((long) item.frame.segmentation_enabled, 0L);
    r |= assertEquals((long) item.frame.seg_id_pre_skip, 0L);
    r |= assertEquals((long) item.frame.lossless_array[0], 0L);

    Seen seen;
    r |= assertEquals(walkItem(&item, &seen), TINYIMG_OK);

    r |= assertEquals((long) seen.blocks, 1L);
    r |= assertEquals((long) seen.first.size, (long) TINY_AV1_BLOCK_8X8);
    r |= assertEquals((long) seen.first.row, 0L);
    r |= assertEquals((long) seen.first.col, 0L);
    r |= assertEquals((long) seen.first.y_mode, (long) TINY_AV1_DC_PRED);
    r |= assertEquals((long) seen.first.skip, 0L);

    // monochrome, so the chroma mode is never coded and never read
    r |= assertEquals((long) seen.first.has_chroma, 0L);
    r |= assertEquals((long) seen.first.uv_mode, 0L);

    // TX_MODE_LARGEST, so the transform is the block's own largest rather than
    // a coded depth below it
    r |= assertEquals((long) seen.first.tx_size, (long) TINY_AV1_TX_8X8);
    r |= assertEquals((long) seen.first.q_index, 255L);
    r |= assertEquals((long) seen.first.lossless, 0L);
    r |= assertEquals((long) seen.first.segment_id, 0L);

    free(item.file);
    return r;
}

/**
 * A 256x256 frame, where the first partition symbol was computed by hand.
 *
 * The bitstream says PARTITION_SPLIT at the first superblock, which was worked
 * out from the tile's first two bytes and the default distribution before this
 * decoder was pointed at the file. It is not what a flat picture suggests, and
 * that is the point of recording it: at a quantizer of 255 every choice costs
 * the encoder the same and its search order decides, so what the file says is
 * the only authority.
 */
static int readsTheFirstBlockOfAFlatFrame(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-flat.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    r |= assertEquals((long) item.header_bytes, 7L);
    r |= assertEquals((long) item.tile_size, 16L);
    r |= assertEquals((long) item.frame.mi_cols, 64L);
    r |= assertEquals((long) item.frame.mi_rows, 64L);
    r |= assertEquals((long) item.sequence.use_128x128_superblock, 0L);
    r |= assertEquals((long) item.sequence.enable_cdef, 1L);

    Seen seen;
    r |= assertEquals(walkItem(&item, &seen), TINYIMG_OK);

    r |= assertGreaterThan((double) seen.blocks, 0.0);

    // the hand computation: SPLIT at 64x64, then NONE at the first 32x32
    r |= assertEquals((long) seen.first.size, (long) TINY_AV1_BLOCK_32X32);
    r |= assertEquals((long) seen.first.row, 0L);
    r |= assertEquals((long) seen.first.col, 0L);
    r |= assertEquals((long) seen.first.skip, 0L);
    r |= assertEquals((long) seen.first.y_mode, (long) TINY_AV1_DC_PRED);
    r |= assertEquals((long) seen.first.tx_size, (long) TINY_AV1_TX_32X32);
    r |= assertEquals((long) seen.first.q_index, 255L);

    // CDEF is enabled with zero strength bits, so the index is read as a
    // literal of no bits at all and comes back as the one strength the frame
    // carries
    r |= assertEquals((long) seen.first.cdef_idx, 0L);

    // and nothing directional was coded, so neither angle moved
    r |= assertEquals((long) seen.first.angle_delta_y, 0L);
    r |= assertEquals((long) seen.first.angle_delta_uv, 0L);
    r |= assertEquals((long) seen.first.use_filter_intra, 0L);

    free(item.file);
    return r;
}

/**
 * The three frames this decoder refuses, and why each one has to be refused
 * rather than ignored.
 *
 * All three code symbols the reader does not consume, so a frame carrying one
 * cannot be decoded partially: the tile desynchronises and the picture becomes
 * noise rather than becoming approximate.
 */
static int refusesWhatItCannotRead(void) {
    int r = 0;

    TinyAv1Sequence sequence;
    TinyAv1Frame frame;

    tiny_memset(&sequence, 0, sizeof(sequence));
    tiny_memset(&frame, 0, sizeof(frame));

    // a zeroed sequence is not a real one: eight bits a sample is the only
    // depth the reconstruction holds, so it has to be said rather than implied
    sequence.bit_depth = 8u;

    r |= assertEquals(tiny_av1_frame_supported(&sequence, &frame), TINYIMG_OK);

    frame.allow_intrabc = 1u;
    r |= assertEquals(
        tiny_av1_frame_supported(&sequence, &frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    frame.allow_intrabc = 0u;

    frame.allow_screen_content_tools = 1u;
    r |= assertEquals(
        tiny_av1_frame_supported(&sequence, &frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    frame.allow_screen_content_tools = 0u;

    frame.uses_lr = 1u;
    r |= assertEquals(
        tiny_av1_frame_supported(&sequence, &frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    frame.uses_lr = 0u;

    /*
     * 4:2:2, whose chroma residual size the specification's own table answers
     * BLOCK_INVALID for on every tall block, and ten or twelve bit samples,
     * which the reconstruction has nowhere to put. Both are refused at the
     * frame so a caller learns before any pixel is decoded.
     */
    sequence.sub_x = 1u;
    sequence.sub_y = 0u;
    r |= assertEquals(
        tiny_av1_frame_supported(&sequence, &frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    // and 4:2:0 with the same horizontal subsampling is fine
    sequence.sub_y = 1u;
    r |= assertEquals(tiny_av1_frame_supported(&sequence, &frame), TINYIMG_OK);

    sequence.bit_depth = 10u;
    r |= assertEquals(
        tiny_av1_frame_supported(&sequence, &frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    sequence.bit_depth = 8u;

    r |= assertEquals(tiny_av1_frame_supported(0, &frame), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_av1_frame_supported(&sequence, 0), TINYIMG_ERR_NULL);

    return r;
}

/**
 * A walk with no context arrays reports the geometry and refuses the mode info.
 *
 * Which is what lets `tests/c/codec/av1-tile.c` drive the tree with a chosen
 * bitstream: the arrays are the caller's, and their absence is the switch
 * between the two uses rather than a failure.
 */
static int refusesModeInfoWithoutContext(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-tiny.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    static uint8_t sizes[64];
    static TinyAv1Cdf cdf;

    tiny_memset(sizes, 0, sizeof(sizes));
    tiny_av1_cdf_init(&cdf);

    TinyAv1Symbol symbol;
    tiny_av1_symbol_init(&symbol, item.tile, item.tile_size);

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    walk.symbol = &symbol;
    walk.cdf = &cdf;
    walk.sequence = &item.sequence;
    walk.frame = &item.frame;
    walk.sizes = sizes;
    walk.row_end = item.frame.mi_rows;
    walk.col_end = item.frame.mi_cols;

    // no sink at all: the geometry is recorded and nothing is read past the
    // partition symbols
    r |= assertEquals(tiny_av1_walk_tile(&walk), TINYIMG_OK);
    r |= assertEquals((long) sizes[0], (long) TINY_AV1_BLOCK_8X8);

    // and asking for the mode info without the arrays is refused rather than
    // dereferencing them
    r |= assertEquals(
        tiny_av1_read_mode(&walk, 0u, 0u, TINY_AV1_BLOCK_8X8), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_av1_read_mode(0, 0u, 0u, TINY_AV1_BLOCK_8X8), TINYIMG_ERR_NULL
    );

    free(item.file);
    return r;
}

/**
 * The tile group's own header, which is not the frame header.
 *
 * A single-tile frame codes no start and end pair and no per-tile length, so
 * the tile is the whole remaining payload; the count comes from the frame
 * header rather than from the payload, which is why asking for a tile the frame
 * does not have is a bounds error rather than a corrupt read.
 */
static int resolvesTileBytes(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-flat.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    r |= assertEquals((long) item.frame.tile_cols, 1L);
    r |= assertEquals((long) item.frame.tile_rows, 1L);

    const uint8_t* tile = 0;
    size_t size = 0;

    static const uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    r |= assertEquals(
        tiny_av1_tile_bytes(
            &item.frame, payload, sizeof(payload), 0u, &tile, &size
        ),
        TINYIMG_OK
    );
    r |= assertTrue(tile == payload);
    r |= assertEquals((long) size, 8L);

    r |= assertEquals(
        tiny_av1_tile_bytes(
            &item.frame, payload, sizeof(payload), 1u, &tile, &size
        ),
        TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_av1_tile_bytes(0, payload, sizeof(payload), 0u, &tile, &size),
        TINYIMG_ERR_NULL
    );

    free(item.file);
    return r;
}

/**
 * The first block of `fox.avif`, which is the only fixture with chroma.
 *
 * It reaches three paths the flat frames cannot: the chroma mode, the
 * chroma-from-luma alphas behind it, and the transform depth symbol, since this
 * is the one frame whose `tx_mode` is TX_MODE_SELECT. CFL on the very first
 * block is luck rather than design, and it is the reason this fixture earns a
 * case of its own.
 */
static int readsChromaAndCfl(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("fox.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    // split OBUs, so no frame OBU header length to report
    r |= assertEquals((long) item.header_bytes, 0L);
    r |= assertEquals((long) item.tile_size, 80393L);
    r |= assertEquals((long) item.sequence.planes, 3L);
    r |= assertEquals((long) item.sequence.use_128x128_superblock, 1L);
    r |=
        assertEquals((long) item.frame.tx_mode, (long) TINY_AV1_TX_MODE_SELECT);
    r |= assertEquals((long) item.sequence.enable_cdef, 0L);

    Seen seen;
    r |= assertEquals(walkItem(&item, &seen), TINYIMG_OK);

    r |= assertEquals((long) seen.first.size, (long) TINY_AV1_BLOCK_16X16);
    r |= assertEquals((long) seen.first.skip, 0L);
    r |= assertEquals((long) seen.first.y_mode, (long) TINY_AV1_DC_PRED);
    r |= assertEquals((long) seen.first.has_chroma, 1L);
    r |= assertEquals((long) seen.first.uv_mode, (long) TINY_AV1_UV_CFL_PRED);

    // the alphas, whose two signs are packed into one symbol and whose
    // magnitudes are read against a context derived from that symbol
    r |= assertEquals((long) seen.first.cfl_alpha_u, 2L);
    r |= assertEquals((long) seen.first.cfl_alpha_v, -1L);

    // a transform depth of zero, which is still a symbol read: TX_MODE_SELECT
    // codes one per block and this one chose the block's largest
    r |= assertEquals((long) seen.first.tx_size, (long) TINY_AV1_TX_16X16);
    r |= assertEquals((long) seen.first.q_index, 72L);

    // CDEF is disabled for the whole frame, so no square carries an index
    r |= assertEquals((long) seen.first.cdef_idx, -1L);

    free(item.file);
    return r;
}

/**
 * The quantizer delta, which is coded once per superblock and not once per
 * block.
 *
 * `ReadDeltas` is set before each superblock and cleared by the first block's
 * mode info. Reading a delta for every block instead consumes symbols the
 * encoder never wrote; the first block still comes out right, which is why the
 * resolution shift is asserted here rather than only the presence of a delta.
 */
static int readsTheQuantizerDelta(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-deltaq.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    r |= assertEquals((long) item.frame.delta_q_present, 1L);
    r |= assertEquals((long) item.frame.base_q_idx, 148L);
    r |= assertEquals((long) item.frame.delta_q_res, 2L);

    Seen seen;
    r |= assertEquals(walkItem(&item, &seen), TINYIMG_OK);

    // 148 with a delta of -15 at a resolution of two: -15 << 2 is -60
    r |= assertEquals((long) seen.first.q_index, 88L);
    r |= assertEquals((long) seen.first.size, (long) TINY_AV1_BLOCK_32X32);

    free(item.file);
    return r;
}

/**
 * A 4:4:4 frame from a third encoder, which is three new paths at once.
 *
 * `dartmouth.avif` was converted from `dartmouth.jpg` by neither of the two
 * encoders the other fixtures came from, and it differs from both in ways that
 * reach code they cannot:
 *
 * - **No chroma subsampling**, so the 4xN parity rules that suppress a block's
 *   own chroma never fire and every block carries a chroma mode.
 * - **A CDEF strength index of one bit**, so `read_cdef` reads a real literal
 *   rather than one of no width. `avifenc` writes zero bits and `cavif`
 *   disables CDEF outright.
 * - **Profile 1 with the identity matrix and full range**, which is the one
 *   configuration where the coded planes are not YUV at all.
 *
 * Its frame header is ten bytes, counted independently against 5.9.2 by a bit
 * counter written from the specification rather than from this code, which
 * agreed on all nineteen fields as well as on the length.
 */
static int readsAFourFourFourFrame(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("dartmouth.avif", &item), TINYIMG_OK);

    if (!item.tile) {
        free(item.file);
        return r + 1;
    }

    r |= assertEquals((long) item.header_bytes, 10L);
    r |= assertEquals((long) item.tile_size, 21931L);

    r |= assertEquals((long) item.sequence.profile, 1L);
    r |= assertEquals((long) item.sequence.planes, 3L);
    r |= assertEquals((long) item.sequence.sub_x, 0L);
    r |= assertEquals((long) item.sequence.sub_y, 0L);

    // the identity matrix, so the planes are GBR rather than YUV; the other two
    // fixtures carry 9 and 6, which is every matrix a converter has produced
    // here
    r |= assertEquals((long) item.sequence.matrix_coefficients, 0L);
    r |= assertEquals((long) item.sequence.color_range, 1L);
    r |= assertEquals((long) item.sequence.film_grain_params_present, 0L);

    r |= assertEquals((long) item.frame.base_q_idx, 100L);
    r |= assertEquals((long) item.frame.cdef_bits, 1L);
    r |= assertEquals((long) item.frame.using_qmatrix, 0L);
    r |= assertEquals((long) item.frame.loop_filter_level[0], 2L);
    r |=
        assertEquals((long) item.frame.tx_mode, (long) TINY_AV1_TX_MODE_SELECT);

    Seen seen;
    r |= assertEquals(walkItem(&item, &seen), TINYIMG_OK);

    r |= assertEquals((long) seen.first.size, (long) TINY_AV1_BLOCK_16X16);
    r |= assertEquals((long) seen.first.skip, 0L);
    r |= assertEquals((long) seen.first.y_mode, (long) TINY_AV1_DC_PRED);
    r |= assertEquals((long) seen.first.uv_mode, (long) TINY_AV1_DC_PRED);
    r |= assertEquals((long) seen.first.tx_size, (long) TINY_AV1_TX_16X16);
    r |= assertEquals((long) seen.first.q_index, 100L);

    // one strength bit, read as a literal, and this frame's first square took
    // index zero
    r |= assertEquals((long) seen.first.cdef_idx, 0L);

    // at 4:4:4 the parity rules never suppress a block's chroma, so every
    // block carries a mode of its own
    r |= assertEquals((long) seen.first.has_chroma, 1L);
    r |= assertEquals((long) seen.chroma, (long) seen.blocks);

    free(item.file);
    return r;
}

/**
 * Every context path, over a stream this decoder cannot desynchronise on.
 *
 * The reader consumes no coefficients, so a **chosen** stream keeps it
 * synchronised for the whole frame however arbitrary the symbols come out; the
 * two real fixtures cannot do that, because a real encoder writes coefficients
 * between the blocks. What this buys is coverage: thousands of blocks, every
 * skip and transform-depth context reached, chroma on every plane, and CFL
 * wherever the stream lands on it.
 *
 * The digest is **recorded from the build whose first block is externally
 * anchored above**, and it exists for one reason: until the coefficient parse
 * lands there is no reference for the rest of the reader, and a context
 * selection that changed silently would otherwise be found by a wrong picture
 * months later. Two deliberate mutations that the first-block cases cannot see
 * both move it: swapping the two chroma-from-luma sign contexts, and dropping
 * the left neighbor from the skip context.
 *
 * It is not evidence that the reader is correct. It is evidence that it has not
 * changed.
 */
static int coversEveryContext(void) {
    int r = 0;

    static uint8_t stream[65536];
    static uint8_t sizes[64 * 1024];
    static uint8_t y_modes[64 * 1024];
    static uint8_t skips[64 * 1024];
    static uint8_t segment_ids[64 * 1024];
    static uint8_t tx_sizes[64 * 1024];
    static int8_t cdef[4096];
    static TinyAv1Cdf cdf;

    // a fixed multiplicative sequence rather than a constant fill, so the
    // symbols land across their distributions instead of all at one end
    uint32_t seed = 0x2545F491u;

    for (size_t i = 0; i < sizeof(stream); i++) {
        seed = seed * 1103515245u + 12345u;
        stream[i] = (uint8_t) (seed >> 17);
    }

    TinyAv1Sequence sequence;
    tiny_memset(&sequence, 0, sizeof(sequence));

    sequence.planes = 3u;
    sequence.sub_x = 1u;
    sequence.sub_y = 1u;
    sequence.enable_cdef = 1u;
    sequence.enable_filter_intra = 1u;
    sequence.use_128x128_superblock = 0u;

    TinyAv1Frame frame;
    tiny_memset(&frame, 0, sizeof(frame));

    frame.width = 512u;
    frame.height = 512u;
    frame.mi_cols = 128u;
    frame.mi_rows = 128u;
    frame.base_q_idx = 100u;
    frame.tx_mode = TINY_AV1_TX_MODE_SELECT;
    frame.cdef_bits = 2u;
    frame.delta_q_present = 1u;
    frame.delta_q_res = 1u;
    frame.delta_lf_present = 1u;
    frame.delta_lf_res = 1u;
    frame.delta_lf_multi = 1u;

    for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
        frame.seg_qindex[i] = frame.base_q_idx;
    }

    tiny_memset(sizes, 0, sizeof(sizes));
    tiny_memset(y_modes, 0, sizeof(y_modes));
    tiny_memset(skips, 0, sizeof(skips));
    tiny_memset(segment_ids, 0, sizeof(segment_ids));
    tiny_memset(tx_sizes, 0, sizeof(tx_sizes));
    tiny_memset(cdef, -1, sizeof(cdef));

    tiny_av1_cdf_init(&cdf);

    TinyAv1Symbol symbol;
    tiny_av1_symbol_init(&symbol, stream, sizeof(stream));

    Seen seen;
    tiny_memset(&seen, 0, sizeof(seen));
    seen.digest = 1469598103934665603ULL;

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    walk.symbol = &symbol;
    walk.cdf = &cdf;
    walk.sequence = &sequence;
    walk.frame = &frame;
    walk.sizes = sizes;
    walk.y_modes = y_modes;
    walk.skips = skips;
    walk.segment_ids = segment_ids;
    walk.tx_sizes = tx_sizes;
    walk.cdef_idx = cdef;
    walk.cdef_stride = (frame.mi_cols + 15u) / 16u;
    walk.row_end = frame.mi_rows;
    walk.col_end = frame.mi_cols;
    walk.block = record;
    walk.context = &seen;
    walk.q_index = frame.base_q_idx;

    r |= assertEquals(tiny_av1_walk_tile(&walk), TINYIMG_OK);
    r |= assertEquals((long) seen.blocks, 1813L);

    // the paths the two real fixtures cannot reach between them, each reached
    // hundreds of times rather than once
    r |= assertGreaterThan((double) seen.cfl, 100.0);
    r |= assertGreaterThan((double) seen.filter_intra, 100.0);
    r |= assertGreaterThan((double) seen.chroma, 1000.0);
    r |= assertGreaterThan((double) seen.skipped, 40.0);

    // every field of every block, digested
    uint64_t hash = 1469598103934665603ULL;

    for (size_t i = 0; i < (size_t) frame.mi_cols * frame.mi_rows; i++) {
        uint8_t bytes[4] = {sizes[i], y_modes[i], skips[i], tx_sizes[i]};

        for (uint32_t b = 0; b < 4u; b++) {
            hash = (hash ^ bytes[b]) * 1099511628211ULL;
        }
    }

    r |= assertTrue(hash == 0xc9ac219db913fe76ULL);
    r |= assertTrue(seen.digest == 0x0b93b7880093b47bULL);

    /*
     * The same stream with segmentation on, which no real fixture has.
     *
     * `read_segment_id` and its three-neighbor context are unreachable
     * otherwise: every converter tried here leaves segmentation off, so a
     * mutation that made the context always zero survived every other case in
     * this file. The two orders are both run, because `SegIdPreSkip` decides
     * whether the id is coded before the skip flag or after it, and getting
     * that backwards shifts every block.
     */
    for (uint32_t pre_skip = 0; pre_skip < 2u; pre_skip++) {
        frame.segmentation_enabled = 1u;
        frame.seg_id_pre_skip = (uint8_t) pre_skip;
        frame.last_active_seg_id = 7u;

        tiny_memset(sizes, 0, sizeof(sizes));
        tiny_memset(y_modes, 0, sizeof(y_modes));
        tiny_memset(skips, 0, sizeof(skips));
        tiny_memset(segment_ids, 0, sizeof(segment_ids));
        tiny_memset(tx_sizes, 0, sizeof(tx_sizes));
        tiny_memset(cdef, -1, sizeof(cdef));

        tiny_av1_cdf_init(&cdf);
        tiny_av1_symbol_init(&symbol, stream, sizeof(stream));

        Seen segmented;
        tiny_memset(&segmented, 0, sizeof(segmented));
        segmented.digest = 1469598103934665603ULL;

        walk.context = &segmented;
        walk.q_index = frame.base_q_idx;

        r |= assertEquals(tiny_av1_walk_tile(&walk), TINYIMG_OK);
        r |= assertGreaterThan((double) segmented.blocks, 100.0);

        // the ids actually vary, or the context that chooses their distribution
        // is being exercised with one value
        uint32_t distinct = 0;
        uint8_t seen_id[TINY_AV1_MAX_SEGMENTS];
        tiny_memset(seen_id, 0, sizeof(seen_id));

        for (size_t i = 0; i < (size_t) frame.mi_cols * frame.mi_rows; i++) {
            if (segment_ids[i] < TINY_AV1_MAX_SEGMENTS) {
                seen_id[segment_ids[i]] = 1u;
            }
        }

        for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
            distinct += seen_id[i];
        }

        r |= assertGreaterThan((double) distinct, 2.0);

        uint64_t ids = 1469598103934665603ULL;

        for (size_t i = 0; i < (size_t) frame.mi_cols * frame.mi_rows; i++) {
            ids = (ids ^ segment_ids[i]) * 1099511628211ULL;
        }

        // both orders reach all eight segments, and the two digests differ,
        // which is what says the order actually changed the bit stream rather
        // than only the field this decoder reports
        r |= assertTrue(
            ids == (pre_skip ? 0x63c8f1d62a1e33ceULL : 0x734c90a2b91b6bf4ULL)
        );
    }

    return r;
}

/**
 * The segment id coding is a bijection, which is what makes it lossless.
 *
 * An external property rather than a recording: for a fixed prediction and
 * range, every id has to be reachable by some coded value and no two coded
 * values may land on the same id. A version that drops one of the four branches
 * still returns ids in range, and this is the check that notices.
 *
 * Every prediction and range the format allows, which is 36 pairs and 204 coded
 * values in all.
 */
static int segmentIdCodingIsABijection(void) {
    int r = 0;
    uint32_t pairs = 0;

    for (uint32_t max = 1; max <= TINY_AV1_MAX_SEGMENTS; max++) {
        for (uint32_t ref = 0; ref < max; ref++) {
            uint8_t hit[TINY_AV1_MAX_SEGMENTS];
            tiny_memset(hit, 0, sizeof(hit));

            int inside = 1;

            for (uint32_t diff = 0; diff < max; diff++) {
                uint32_t id = tiny_av1_neg_deinterleave(diff, ref, max);

                if (id >= max) {
                    inside = 0;
                    continue;
                }

                hit[id]++;
            }

            r |= assertTrue(inside);

            uint32_t reached = 0;
            uint32_t collided = 0;

            for (uint32_t id = 0; id < max; id++) {
                if (hit[id] == 1u) reached++;
                if (hit[id] > 1u) collided++;
            }

            r |= assertEquals((long) reached, (long) max);
            r |= assertEquals((long) collided, 0L);

            pairs++;
        }
    }

    r |= assertEquals((long) pairs, 36L);

    // and a coded difference of zero is the prediction itself, which is the one
    // value the interleaving has to leave alone
    for (uint32_t max = 1; max <= TINY_AV1_MAX_SEGMENTS; max++) {
        for (uint32_t ref = 0; ref < max; ref++) {
            r |= assertEquals(
                (long) tiny_av1_neg_deinterleave(0u, ref, max), (long) ref
            );
        }
    }

    return r;
}

int main(void) {
    int r = 0;

    tiny_init();

    r |= readsTheSmallestFrame();
    r |= readsTheFirstBlockOfAFlatFrame();
    r |= readsChromaAndCfl();
    r |= readsTheQuantizerDelta();
    r |= readsAFourFourFourFrame();
    r |= refusesWhatItCannotRead();
    r |= refusesModeInfoWithoutContext();
    r |= resolvesTileBytes();
    r |= coversEveryContext();
    r |= segmentIdCodingIsABijection();

    return r;
}
