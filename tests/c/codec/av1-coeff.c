#include "codec/av1-item.h"

/**
 * @file
 * @brief The coefficient parse, and the end-to-end anchor it makes possible.
 *
 * **A tile's own length is the anchor.** Every symbol comes from one arithmetic
 * decoder whose position depends on every probability used before it, so the
 * number of bits a tile takes to decode is a checksum over the whole of it: the
 * partition tree, every mode info element, every context selection, every scan
 * and every coefficient. An encoder writes a tile whose last symbol lands in
 * its last byte. A reader that gets any of that wrong stops somewhere else.
 *
 * The five fixtures below are read to their last byte, `fox.avif` after 6,971
 * blocks and 22,873 transform blocks of it. That number comes from the file,
 * not from this decoder, which is what makes it an anchor rather than a
 * recording; the block counts and digests beside it are recordings and are
 * there to say which reading produced the anchor.
 *
 * What it does not prove: that a coefficient landed at the right position
 * inside its transform. A permuted scan changes the contexts and so the bit
 * count too, which is why it is unlikely rather than impossible; the check that
 * closes it is the reconstruction compared against `avifdec`'s pixels, which
 * arrives with sub-phase 1i.
 */

/** Padded context arrays, allocated once at the largest fixture's extent. */
#define COEFF_MAX_CELLS (64 * 1024)

/** What a whole-tile walk saw. */
typedef struct {
    uint32_t blocks;
    uint32_t skipped;
    uint32_t tx_blocks;
    uint32_t coded;
    uint32_t coefficients;
    uint64_t digest;

    /** Which transform types and sizes the fixture actually reached. */
    uint32_t types[16];
    uint32_t sizes[19];

    /** Non-zero once an invariant the specification states has been broken. */
    uint32_t violations;

    /** Every mi position, counted, so a hole or a double is visible. */
    uint8_t* covered;
    uint32_t mi_cols;

    /** The tile the walk covered, so a position outside it counts as a fault.
     */
    uint32_t row_start;
    uint32_t row_end;
    uint32_t col_start;
    uint32_t col_end;

    /** Bits the residual of a skipped block consumed, which has to be none. */
    uint32_t skipped_bits;
    /** Coefficients a skipped block's transform blocks reported. */
    uint32_t skipped_coefficients;
} Seen;

/**
 * Checks one transform block against what the specification bounds, then
 * digests it.
 *
 * The three invariants are the ones a wrong read produces without failing: an
 * end of block past the transform would index the scan out of range, a level
 * past twenty bits would mean the mask was not applied, and more non-zero
 * positions than the end of block says exist would mean a stale coefficient
 * from the previous transform block was left standing.
 */
static int record_tx(TinyAv1Walk* walk, const TinyAv1TxBlock* tx) {
    Seen* seen = (Seen*) walk->context;

    uint32_t width = tx->stride;
    uint32_t height = tiny_av1_tx_height[tx->tx_size];

    if (height > 32u) height = 32u;

    seen->tx_blocks++;

    if (tx->eob > width * height) seen->violations++;
    if (tx->tx_type > TINY_AV1_H_FLIPADST) seen->violations++;

    // the coefficients are readable exactly when there are some, which is what
    // stops a block with no residual from being handed the last block's levels
    if ((tx->eob > 0u) != (tx->quant != 0)) seen->violations++;

    if (tx->eob > 0u) {
        uint32_t nonzero = 0;

        for (uint32_t i = 0; i < width * height; i++) {
            int32_t level = tx->quant[i];
            uint32_t magnitude = (uint32_t) (level < 0 ? -level : level);

            if (magnitude > 0xFFFFFu) seen->violations++;
            if (magnitude != 0u) nonzero++;
        }

        if (nonzero > tx->eob) seen->violations++;
    }

    /*
     * The type, the size and the plane go into the digest beside the levels.
     *
     * The transform type is the field a wrong read shows up in nowhere else:
     * both members of a pair like DCT_DCT and ADST_DCT read the same scan, the
     * same transform class and the same contexts, so replacing one with the
     * other changes the picture and not one bit of the parse. Counting the two
     * separately and asserting neither is what let a mutation to the chroma set
     * membership survive.
     */
    uint32_t fields[4] = {tx->plane, tx->tx_size, tx->tx_type, tx->eob};

    for (uint32_t i = 0; i < 4u; i++) {
        seen->digest = (seen->digest ^ fields[i]) * 1099511628211ULL;
    }

    if (tx->eob > 0u) {
        seen->coded++;
        seen->coefficients += tx->eob;
        seen->types[tx->tx_type]++;
        seen->sizes[tx->tx_size]++;

        for (uint32_t i = 0; i < tx->eob; i++) {
            seen->digest = (seen->digest ^ (uint64_t) (uint32_t) tx->quant[i]) *
                           1099511628211ULL;
        }
    }

    return TINYIMG_OK;
}

/** Reads one block's mode info and then its residual, which is the order. */
static int record_block(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    Seen* seen = (Seen*) walk->context;

    int result = tiny_av1_read_mode(walk, row, col, size);
    if (result != TINYIMG_OK) return result;

    seen->blocks++;
    seen->skipped += walk->mode.skip;

    uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[size];

    for (uint32_t y = 0; y < high && row + y < walk->row_end; y++) {
        for (uint32_t x = 0; x < wide && col + x < walk->col_end; x++) {
            seen->covered[(size_t) (row + y) * seen->mi_cols + col + x]++;
        }
    }

    size_t before = walk->symbol->bit;
    uint32_t coded = seen->coefficients;

    result = tiny_av1_read_residual(walk);

    /*
     * A skipped block codes no residual at all, which is a property rather than
     * a recording: the decoder's position must not move across its residual and
     * none of its transform blocks may report a coefficient. No file `avifenc`
     * or `cavif` produces sets `skip` on an intra block, so the only case that
     * reaches this is the chosen stream below.
     */
    if (walk->mode.skip) {
        seen->skipped_bits += (uint32_t) (walk->symbol->bit - before);
        seen->skipped_coefficients += seen->coefficients - coded;
    }

    return result;
}

/** Everything a whole-tile walk needs, static so the test owns no allocator. */
typedef struct {
    uint8_t sizes[COEFF_MAX_CELLS];
    uint8_t y_modes[COEFF_MAX_CELLS];
    uint8_t skips[COEFF_MAX_CELLS];
    uint8_t segment_ids[COEFF_MAX_CELLS];
    uint8_t tx_sizes[COEFF_MAX_CELLS];
    uint8_t covered[COEFF_MAX_CELLS];
    int8_t cdef[4096];

    uint8_t above_level[3 * (512 + TINY_AV1_CONTEXT_PAD)];
    uint8_t above_dc[3 * (512 + TINY_AV1_CONTEXT_PAD)];
    uint8_t left_level[3 * (512 + TINY_AV1_CONTEXT_PAD)];
    uint8_t left_dc[3 * (512 + TINY_AV1_CONTEXT_PAD)];

    TinyAv1Cdf cdf;
    TinyAv1Residual residual;
    TinyAv1Symbol symbol;
    TinyAv1Walk walk;

    /** The tile the last walk was given, for the byte count assertion. */
    size_t tile_size;
} Arena;

static Arena arena;

/**
 * Walks one tile of a fixture with the residual reader attached.
 *
 * `limit` truncates the tile, which is how the truncation case drives it; zero
 * means the whole tile. The distributions and both context arrays are reset
 * here rather than by the walk, because that is per tile and the caller is what
 * knows a new tile has started.
 */
static int walk_tile_of(
    Opened* item, Seen* seen, uint32_t index, size_t limit
) {
    size_t cells = (size_t) item->frame.mi_cols * item->frame.mi_rows;

    tiny_memset(seen, 0, sizeof(*seen));

    if (cells > COEFF_MAX_CELLS) return TINYIMG_ERR_TOO_LARGE;
    if (item->frame.mi_cols > 512u) return TINYIMG_ERR_TOO_LARGE;

    const uint8_t* bytes = 0;
    size_t bytes_size = 0;

    int found = tiny_av1_tile_bytes(
        &item->frame, item->group, item->group_size, index, &bytes, &bytes_size
    );

    if (found != TINYIMG_OK) return found;

    tiny_memset(&arena.sizes, 0, cells);
    tiny_memset(&arena.y_modes, 0, cells);
    tiny_memset(&arena.skips, 0, cells);
    tiny_memset(&arena.segment_ids, 0, cells);
    tiny_memset(&arena.tx_sizes, 0, cells);
    tiny_memset(&arena.covered, 0, cells);
    tiny_memset(&arena.cdef, -1, sizeof(arena.cdef));

    seen->covered = arena.covered;
    seen->mi_cols = item->frame.mi_cols;

    tiny_av1_cdf_init(&arena.cdf);

    size_t size = limit > 0u && limit < bytes_size ? limit : bytes_size;
    tiny_av1_symbol_init(&arena.symbol, bytes, size);

    arena.tile_size = bytes_size;

    tiny_memset(&arena.residual, 0, sizeof(arena.residual));

    arena.residual.above_level = arena.above_level;
    arena.residual.above_dc = arena.above_dc;
    arena.residual.left_level = arena.left_level;
    arena.residual.left_dc = arena.left_dc;
    arena.residual.above_stride = item->frame.mi_cols + TINY_AV1_CONTEXT_PAD;
    arena.residual.left_stride = item->frame.mi_rows + TINY_AV1_CONTEXT_PAD;
    arena.residual.block = record_tx;

    TinyAv1Walk* walk = &arena.walk;
    tiny_memset(walk, 0, sizeof(*walk));

    walk->symbol = &arena.symbol;
    walk->cdf = &arena.cdf;
    walk->sequence = &item->sequence;
    walk->frame = &item->frame;
    walk->sizes = arena.sizes;
    walk->y_modes = arena.y_modes;
    walk->skips = arena.skips;
    walk->segment_ids = arena.segment_ids;
    walk->tx_sizes = arena.tx_sizes;
    walk->cdef_idx = arena.cdef;
    walk->cdef_stride = (item->frame.mi_cols + 15u) / 16u;
    walk->row_start = item->frame.mi_row_starts[index / item->frame.tile_cols];
    walk->row_end =
        item->frame.mi_row_starts[index / item->frame.tile_cols + 1u];
    walk->col_start = item->frame.mi_col_starts[index % item->frame.tile_cols];
    walk->col_end =
        item->frame.mi_col_starts[index % item->frame.tile_cols + 1u];
    walk->block = record_block;
    walk->context = seen;
    walk->q_index = item->frame.base_q_idx;
    walk->residual = &arena.residual;

    seen->row_start = walk->row_start;
    seen->row_end = walk->row_end;
    seen->col_start = walk->col_start;
    seen->col_end = walk->col_end;

    return tiny_av1_walk_tile(walk);
}

/**
 * Every mi position of the tile covered exactly once, which says the walk
 * finished.
 *
 * A hole is a piece of picture nothing decoded and a double is a block decoded
 * against a neighbor that has already moved. Both are silent, and a
 * desynchronised walk usually produces one before it produces an error.
 */
static int assertCoveredOnce(const Opened* item, const Seen* seen) {
    const TinyAv1Frame* frame = &item->frame;
    uint32_t holes = 0;
    uint32_t doubles = 0;

    for (uint32_t row = 0; row < frame->mi_rows; row++) {
        for (uint32_t col = 0; col < frame->mi_cols; col++) {
            uint8_t count = seen->covered[(size_t) row * frame->mi_cols + col];

            int inside = row >= seen->row_start && row < seen->row_end &&
                         col >= seen->col_start && col < seen->col_end;

            if (inside && count == 0u) holes++;
            if (count > 1u || (!inside && count > 0u)) doubles++;
        }
    }

    int r = 0;

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubles, 0L);

    return r;
}

/**
 * @brief One fixture, read to its last byte.
 *
 * The two assertions that matter are the status and the byte count. A tile is
 * padded to a byte boundary and the arithmetic decoder reads a few bits into
 * that padding, which is what the negative `max_bits` is; more than a byte of
 * it would mean the decode ran past the end rather than finishing inside it.
 */
static int readsWholeTile(
    const char* name, uint32_t blocks, uint32_t tx_blocks, uint32_t coded,
    uint32_t coefficients, uint64_t digest
) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item(name, &item), TINYIMG_OK);
    r |= assertEquals(
        tiny_av1_frame_supported(&item.sequence, &item.frame), TINYIMG_OK
    );

    Seen seen;
    r |= assertEquals(walk_tile_of(&item, &seen, 0u, 0u), TINYIMG_OK);

    r |= assertEquals(
        (long) ((arena.symbol.bit + 7u) / 8u), (long) arena.tile_size
    );
    r |= assertGreaterThan((double) arena.symbol.max_bits, -16.0);
    r |= assertLessThan((double) arena.symbol.max_bits, 1.0);

    r |= assertEquals((long) seen.blocks, (long) blocks);
    r |= assertEquals((long) seen.tx_blocks, (long) tx_blocks);
    r |= assertEquals((long) seen.coded, (long) coded);
    r |= assertEquals((long) seen.coefficients, (long) coefficients);
    r |= assertEquals((long) seen.violations, 0L);
    r |= assertTrue(seen.digest == digest);

    r |= assertCoveredOnce(&item, &seen);

    free(item.file);

    return r;
}

/**
 * The three fixtures whose transforms are all empty.
 *
 * At a quantizer of 255 every transform block codes `all_zero` and nothing
 * else, so these exercise the one path a photograph almost never takes and
 * they still have to land on the tile's last byte.
 */
static int readsFramesWithNoCoefficients(void) {
    int r = 0;

    r |= readsWholeTile(
        "derived/av1-tiny.avif", 1u, 1u, 0u, 0u, 0x08a97b0004e7feabULL
    );
    r |= readsWholeTile(
        "derived/av1-flat.avif", 64u, 64u, 0u, 0u, 0x691fc1e34f187200ULL
    );

    // the same digest as the flat frame above, because the two differ in the
    // quantizer delta on each superblock and in nothing a transform block sees
    r |= readsWholeTile(
        "derived/av1-deltaq.avif", 64u, 64u, 0u, 0u, 0x691fc1e34f187200ULL
    );

    return r;
}

/** 4:4:4 from a third converter, where a 4xN block always has its own chroma.
 */
static int readsAFourFourFourFrame(void) {
    return readsWholeTile(
        "dartmouth.avif", 494u, 1879u, 1813u, 67556u, 0x143788260bb461f2ULL
    );
}

/** 4:2:0 from `cavif`, 80,393 bytes of tile and the largest fixture there is.
 */
static int readsAPhotograph(void) {
    return readsWholeTile(
        "fox.avif", 6971u, 22873u, 16577u, 236096u, 0xe361cc1cc4dc8dd9ULL
    );
}

/**
 * @brief Four tiles, each landing on its own last byte.
 *
 * The only fixture with more than one tile, and it checks three things a
 * single-tile file cannot. `tiny_av1_tile_bytes` has to walk the length
 * prefixes the group puts in front of every tile but the last. Each tile has to
 * start from freshly reset distributions, because a tile that inherited the
 * previous one's adapted state would read different bit counts. And both
 * neighbor context arrays have to be cleared per tile, since a tile's left
 * edge has no decoded neighbor even where the frame does.
 *
 * Every tile's byte count is its own anchor, so this is four of them.
 */
static int readsEveryTileOfAMultiTileFrame(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-tiles.avif", &item), TINYIMG_OK);

    r |= assertEquals((long) item.frame.tile_cols, 2L);
    r |= assertEquals((long) item.frame.tile_rows, 2L);

    static const uint32_t blocks[4] = {151u, 49u, 93u, 59u};
    static const uint32_t coded[4] = {421u, 134u, 226u, 152u};
    static const uint64_t digests[4] = {
        0x35630f1147306fd4ULL, 0x7db60179b41038b0ULL, 0x7e5246427f02e100ULL,
        0xa6f168cd7959c88cULL
    };

    uint32_t total = 0;

    for (uint32_t index = 0; index < 4u; index++) {
        Seen seen;

        r |= assertEquals(walk_tile_of(&item, &seen, index, 0u), TINYIMG_OK);
        r |= assertEquals(
            (long) ((arena.symbol.bit + 7u) / 8u), (long) arena.tile_size
        );
        r |= assertGreaterThan((double) arena.symbol.max_bits, -16.0);

        r |= assertEquals((long) seen.blocks, (long) blocks[index]);
        r |= assertEquals((long) seen.coded, (long) coded[index]);
        r |= assertEquals((long) seen.violations, 0L);
        r |= assertTrue(seen.digest == digests[index]);
        r |= assertCoveredOnce(&item, &seen);

        total += seen.blocks;
    }

    r |= assertEquals((long) total, 352L);

    // and a tile the group does not carry is refused rather than clamped
    Seen seen;
    r |= assertEquals(walk_tile_of(&item, &seen, 4u, 0u), TINYIMG_ERR_BOUNDS);

    free(item.file);

    return r;
}

/**
 * @brief A frame whose quantizer index is zero, which makes every block
 * lossless.
 *
 * Three branches only this fixture reaches. Every transform is forced to 4x4
 * whatever the block size says, so a 16x16 block reads sixteen of them rather
 * than one. The transform type is not coded at all, because the condition that
 * gates it is a quantizer above zero, and every block comes back DCT_DCT. And
 * `lossless` on the transform block is what tells the reconstruction to run the
 * Walsh-Hadamard transform instead.
 */
static int readsALosslessFrame(void) {
    int r = 0;

    Opened item;
    r |=
        assertEquals(open_item("derived/av1-lossless.avif", &item), TINYIMG_OK);
    r |= assertEquals((long) item.frame.base_q_idx, 0L);
    r |= assertEquals((long) item.frame.coded_lossless, 1L);

    Seen seen;
    r |= assertEquals(walk_tile_of(&item, &seen, 0u, 0u), TINYIMG_OK);

    r |= assertEquals(
        (long) ((arena.symbol.bit + 7u) / 8u), (long) arena.tile_size
    );
    r |= assertEquals((long) seen.blocks, 270L);
    r |= assertEquals((long) seen.tx_blocks, 1728L);
    r |= assertEquals((long) seen.coded, 1668L);
    r |= assertEquals((long) seen.coefficients, 22769L);
    r |= assertEquals((long) seen.violations, 0L);
    r |= assertTrue(seen.digest == 0x7ebdeba5e2db78b7ULL);

    // every transform 4x4 and every type DCT, which is what Lossless means
    r |= assertEquals((long) seen.sizes[TINY_AV1_TX_4X4], 1668L);
    r |= assertEquals((long) seen.types[TINY_AV1_DCT_DCT], 1668L);

    r |= assertCoveredOnce(&item, &seen);

    free(item.file);

    return r;
}

/**
 * @brief What the two real fixtures between them reach.
 *
 * Not a coverage report for its own sake: each entry is a code path that would
 * otherwise be reached by no test at all. The row and column scans are only
 * selected for the six one-axis transform types, the identity has its own scan,
 * and a transform with a 64 on either axis reads a 32-wide scan and carries
 * coefficients in a quarter of its area.
 */
static int coversTheScansAndSizes(void) {
    int r = 0;

    Opened item;
    Seen seen;

    r |= assertEquals(open_item("fox.avif", &item), TINYIMG_OK);
    r |= assertEquals(walk_tile_of(&item, &seen, 0u, 0u), TINYIMG_OK);

    // the diagonal scan, from the four two-axis types
    r |= assertGreaterThan((double) seen.types[TINY_AV1_DCT_DCT], 0.0);
    r |= assertGreaterThan((double) seen.types[TINY_AV1_ADST_DCT], 0.0);
    r |= assertGreaterThan((double) seen.types[TINY_AV1_DCT_ADST], 0.0);
    r |= assertGreaterThan((double) seen.types[TINY_AV1_ADST_ADST], 0.0);

    // the identity's own scan, and the row and column scans
    r |= assertGreaterThan((double) seen.types[TINY_AV1_IDTX], 0.0);
    r |= assertGreaterThan((double) seen.types[TINY_AV1_V_DCT], 0.0);
    r |= assertGreaterThan((double) seen.types[TINY_AV1_H_DCT], 0.0);

    // the three eob distributions a small transform never reaches, and the
    // 64-wide sizes whose coefficients live in a 32x32 corner
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_32X32], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_64X64], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_16X32], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_32X16], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_64X32], 0.0);

    // and the four smallest, which is most of any photograph
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_4X4], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_8X8], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_4X8], 0.0);
    r |= assertGreaterThan((double) seen.sizes[TINY_AV1_TX_8X4], 0.0);

    free(item.file);

    return r;
}

/**
 * @brief The skip path, driven by a chosen stream because no encoder emits it.
 *
 * `skip` on an intra block means the whole block codes no residual, and neither
 * `avifenc` nor `cavif` ever sets it: they code `all_zero` per transform block
 * instead. Every real fixture above reports zero skipped blocks, so the branch
 * that matters here, and `reset_block_context` with it, would ship untested.
 *
 * A chosen stream cannot be checked against a reference, so what this asserts
 * is the property rather than the values: across every block the stream makes
 * skipped, the decoder's position does not move over the residual and no
 * transform block reports a coefficient. The digest beside it is a recording,
 * and it is what catches a mutation to the context reset, whose only effect is
 * on the blocks that come after.
 */
static uint8_t synthetic_stream[65536];

/**
 * A frame whose header is written here rather than read from a file.
 *
 * The stream is a fixed multiplicative sequence rather than a constant fill, so
 * the symbols land across their distributions instead of all at one end. It is
 * a valid symbol sequence by construction, since every byte pattern is, and
 * that is what makes a chosen stream reach branches no encoder writes.
 */
static void synthesize(Opened* item, uint32_t sub, uint32_t big_superblock) {
    uint32_t seed = 0x2545F491u;

    for (size_t i = 0; i < sizeof(synthetic_stream); i++) {
        seed = seed * 1103515245u + 12345u;
        synthetic_stream[i] = (uint8_t) (seed >> 17);
    }

    tiny_memset(item, 0, sizeof(*item));

    item->sequence.planes = 3u;
    item->sequence.sub_x = (uint8_t) sub;
    item->sequence.sub_y = (uint8_t) sub;
    item->sequence.enable_cdef = 1u;
    item->sequence.enable_filter_intra = 1u;
    item->sequence.use_128x128_superblock = (uint8_t) big_superblock;

    item->frame.width = 512u;
    item->frame.height = 512u;
    item->frame.mi_cols = 128u;
    item->frame.mi_rows = 128u;
    item->frame.base_q_idx = 100u;
    item->frame.tx_mode = TINY_AV1_TX_MODE_SELECT;
    item->frame.cdef_bits = 2u;
    item->frame.tile_cols = 1u;
    item->frame.tile_rows = 1u;
    item->frame.mi_col_starts[1] = 128u;
    item->frame.mi_row_starts[1] = 128u;
    item->frame.tile_size_bytes = 1u;

    for (uint32_t i = 0; i < TINY_AV1_MAX_SEGMENTS; i++) {
        item->frame.seg_qindex[i] = item->frame.base_q_idx;
    }

    item->group = synthetic_stream;
    item->group_size = sizeof(synthetic_stream);
}

static int readsSkippedBlocks(void) {
    int r = 0;

    Opened item;
    synthesize(&item, 1u, 0u);

    Seen seen;
    r |= assertEquals(walk_tile_of(&item, &seen, 0u, 0u), TINYIMG_OK);

    r |= assertEquals((long) seen.blocks, 687L);
    r |= assertEquals((long) seen.skipped, 118L);
    r |= assertEquals((long) seen.tx_blocks, 3011L);
    r |= assertEquals((long) seen.coded, 1537L);
    r |= assertEquals((long) seen.coefficients, 41144L);

    // the property: a skipped block reads nothing
    r |= assertEquals((long) seen.skipped_bits, 0L);
    r |= assertEquals((long) seen.skipped_coefficients, 0L);

    r |= assertEquals((long) seen.violations, 0L);
    r |= assertCoveredOnce(&item, &seen);

    return r;
}

/**
 * @brief The three paths no fixture reaches, on a chosen stream.
 *
 * Counted before this case was written, with a counter in the reader rather
 * than by reading the code: across all seven fixtures the chroma transform
 * size clamp fires **zero** times, and the chroma transform set membership
 * fires 23 times out of 8,543. Two mutations survived because of it, and both
 * are caught now.
 *
 * - **A block wider or taller than 64 samples** is read as a grid of 64x64
 *   chunks, each with its own planes. Nothing below 128x128 has more than one
 *   chunk, so the loop needs a frame with 128x128 superblocks and no fixture
 *   has one: `avifenc` and `cavif` both pick 64x64.
 * - **A chroma transform with a 64 on either axis** is read through a 32-wide
 *   one, and which of the three replacements applies depends on the other axis.
 *   Reachable at 4:4:4, where the chroma transform is the luma one, from a
 *   16x64 or 64x16 block.
 * - **A chroma transform type outside its block's set** is replaced by DCT. It
 *   changes no context and no scan, so the parse is bit for bit identical and
 *   only the digest of the reported type sees it.
 */
static int readsWhatNoFixtureReaches(void) {
    int r = 0;

    Opened item;
    Seen seen;

    // 4:4:4 with 128x128 superblocks, which is what reaches all three
    synthesize(&item, 0u, 1u);

    r |= assertEquals(walk_tile_of(&item, &seen, 0u, 0u), TINYIMG_OK);
    r |= assertEquals((long) seen.violations, 0L);
    r |= assertCoveredOnce(&item, &seen);

    r |= assertEquals((long) seen.blocks, 415L);
    r |= assertEquals((long) seen.tx_blocks, 2409L);
    r |= assertEquals((long) seen.coded, 880L);
    r |= assertTrue(seen.digest == 0x6f0e6aecd4731e45ULL);

    // 4:2:0 with 128x128 superblocks, where a 128-wide block's chroma is 64
    // wide and takes the same clamp from the other side
    synthesize(&item, 1u, 1u);

    Seen wide;
    r |= assertEquals(walk_tile_of(&item, &wide, 0u, 0u), TINYIMG_OK);
    r |= assertEquals((long) wide.violations, 0L);
    r |= assertCoveredOnce(&item, &wide);
    r |= assertEquals((long) wide.blocks, 253L);
    r |= assertEquals((long) wide.tx_blocks, 1654L);
    r |= assertTrue(wide.digest == 0xf64f6600d6786a92ULL);

    return r;
}

/**
 * @brief A tile cut short must end rather than run.
 *
 * The Golomb prefix is unary and a read past the end of a tile returns zero
 * bits for as long as it is asked, so the loop that counts the prefix has a cap
 * on it. Without one a truncated block does not fail, it hangs, and a Worker
 * that hangs is worse than one that returns an error. Every truncation here
 * either finishes the frame or reports one, and none of them reads outside the
 * arrays, which is what the sanitizer lane checks on the same case.
 */
static int survivesATruncatedTile(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("fox.avif", &item), TINYIMG_OK);

    static const size_t cuts[] = {1u, 2u, 8u, 64u, 1024u, 40000u, 80392u};

    for (uint32_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        Seen seen;
        int status = walk_tile_of(&item, &seen, 0u, cuts[i]);

        // either reading, or refusing; never a third thing
        r |= assertTrue(status == TINYIMG_OK || status < 0);

        // and never more blocks than the frame has positions for
        r |= assertLessThan(
            (double) seen.blocks,
            (double) (item.frame.mi_cols * item.frame.mi_rows + 1u)
        );
    }

    free(item.file);

    return r;
}

/** The residual reader is a pointer, and every array behind it is required. */
static int refusesResidualWithoutContext(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("derived/av1-tiny.avif", &item), TINYIMG_OK);

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    r |= assertEquals(tiny_av1_read_residual(0), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_av1_read_residual(&walk), TINYIMG_ERR_NULL);

    TinyAv1Residual residual;
    tiny_memset(&residual, 0, sizeof(residual));

    walk.residual = &residual;
    r |= assertEquals(tiny_av1_read_residual(&walk), TINYIMG_ERR_NULL);

    free(item.file);

    return r;
}

/**
 * @brief 4:2:2 is refused at the frame rather than read wrong.
 *
 * The specification's own `Subsampled_Size` answers BLOCK_INVALID for every
 * tall block on a 4:2:2 chroma plane, and the residual syntax reads that entry
 * to decide the size of a chroma transform. Refusing is the only honest answer
 * until there is a fixture to check the pair rule against.
 */
static int refusesFourTwoTwo(void) {
    int r = 0;

    Opened item;
    r |= assertEquals(open_item("fox.avif", &item), TINYIMG_OK);

    r |= assertEquals(
        tiny_av1_frame_supported(&item.sequence, &item.frame), TINYIMG_OK
    );

    item.sequence.sub_x = 1u;
    item.sequence.sub_y = 0u;

    r |= assertEquals(
        tiny_av1_frame_supported(&item.sequence, &item.frame),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    free(item.file);

    return r;
}

int main(void) {
    int r = 0;

    tiny_init();

    r |= readsFramesWithNoCoefficients();
    r |= readsAFourFourFourFrame();
    r |= readsAPhotograph();
    r |= readsEveryTileOfAMultiTileFrame();
    r |= readsALosslessFrame();
    r |= readsSkippedBlocks();
    r |= readsWhatNoFixtureReaches();
    r |= coversTheScansAndSizes();
    r |= survivesATruncatedTile();
    r |= refusesResidualWithoutContext();
    r |= refusesFourTwoTwo();

    return r;
}
