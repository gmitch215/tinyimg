#include "codec/av1.h"

#include "test.h"
#include "tinyimg/memory.h"

/**
 * The partition tree's geometry.
 *
 * This cannot be tested against a real bitstream yet, and the reason is worth
 * stating: the partition symbols are interleaved with the mode and coefficient
 * symbols of each block, so reading a partition and then skipping its block
 * desynchronises the decoder. Until the mode info lands, the only honest check
 * is the geometry, driven by a symbol source that returns a chosen sequence.
 *
 * What the geometry has to satisfy is strong enough to be worth checking on its
 * own: **every mi position inside the frame is covered exactly once**. A missed
 * position is a hole in the picture and a doubled one is a block decoded twice
 * against a neighbor that has already moved. Both are silent.
 */

/** How many blocks a walk may record before the test gives up. */
#define MAX_BLOCKS 8192

typedef struct {
    uint32_t rows[MAX_BLOCKS];
    uint32_t cols[MAX_BLOCKS];
    uint8_t sizes[MAX_BLOCKS];
    uint32_t count;

    /** Times each mi position was covered. */
    uint8_t* cover;
    uint32_t mi_cols;
    uint32_t mi_rows;
} Recorded;

static int record(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    Recorded* out = walk->context;

    if (out->count >= MAX_BLOCKS) return TINYIMG_ERR_TOO_LARGE;

    out->rows[out->count] = row;
    out->cols[out->count] = col;
    out->sizes[out->count] = (uint8_t) size;
    out->count++;

    return TINYIMG_OK;
}

/**
 * Drives a walk with a bitstream chosen to make every partition symbol land on
 * a known value.
 *
 * An all-zero tile decodes symbol zero from every distribution, which is
 * PARTITION_NONE, so the tree stays at the superblock size everywhere. An
 * all-ones tile pushes the value to the far end of each interval, which picks
 * the last symbol and splits.
 */
static int walkWith(
    uint8_t fill, uint32_t width, uint32_t height, int superblock128,
    Recorded* out
) {
    static uint8_t stream[65536];
    static uint8_t sizes[1024 * 1024];
    static uint8_t cover[1024 * 1024];

    tiny_memset(stream, fill, sizeof(stream));
    tiny_memset(sizes, 0, sizeof(sizes));
    tiny_memset(cover, 0, sizeof(cover));
    tiny_memset(out, 0, sizeof(*out));

    TinyAv1Sequence sequence;
    tiny_memset(&sequence, 0, sizeof(sequence));
    sequence.use_128x128_superblock = superblock128 ? 1u : 0u;
    sequence.planes = 3u;

    TinyAv1Frame frame;
    tiny_memset(&frame, 0, sizeof(frame));
    frame.width = width;
    frame.height = height;
    frame.mi_cols = 2u * ((width + 7u) >> 3u);
    frame.mi_rows = 2u * ((height + 7u) >> 3u);

    TinyAv1Symbol symbol;
    tiny_av1_symbol_init(&symbol, stream, sizeof(stream));

    static TinyAv1Cdf cdf;
    tiny_av1_cdf_init(&cdf);

    out->cover = cover;
    out->mi_cols = frame.mi_cols;
    out->mi_rows = frame.mi_rows;

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    walk.symbol = &symbol;
    walk.cdf = &cdf;
    walk.sequence = &sequence;
    walk.frame = &frame;
    walk.sizes = sizes;
    walk.row_start = 0;
    walk.row_end = frame.mi_rows;
    walk.col_start = 0;
    walk.col_end = frame.mi_cols;
    walk.block = record;
    walk.context = out;

    return tiny_av1_walk_tile(&walk);
}

/** Marks the area every recorded block covers, and counts the overlaps. */
static void coverage(Recorded* out, uint32_t* holes, uint32_t* doubled) {
    static const uint8_t wide[22] = {1,  1,  2,  2,  2,  4, 4, 4, 8, 8, 8,
                                     16, 16, 16, 32, 32, 1, 4, 2, 8, 4, 16};
    static const uint8_t high[22] = {1, 2,  1,  2,  4,  2, 4, 8, 4, 8,  16,
                                     8, 16, 32, 16, 32, 4, 1, 8, 2, 16, 4};

    for (uint32_t i = 0; i < out->count; i++) {
        uint8_t size = out->sizes[i];

        for (uint32_t y = 0; y < high[size]; y++) {
            for (uint32_t x = 0; x < wide[size]; x++) {
                uint32_t row = out->rows[i] + y;
                uint32_t col = out->cols[i] + x;

                if (row >= out->mi_rows || col >= out->mi_cols) continue;

                out->cover[(size_t) row * out->mi_cols + col]++;
            }
        }
    }

    *holes = 0;
    *doubled = 0;

    for (uint32_t row = 0; row < out->mi_rows; row++) {
        for (uint32_t col = 0; col < out->mi_cols; col++) {
            uint8_t seen = out->cover[(size_t) row * out->mi_cols + col];

            if (seen == 0u)
                (*holes)++;
            else if (seen > 1u)
                (*doubled)++;
        }
    }
}

/** A frame that is a whole number of superblocks, so no edge case applies. */
static int coversAlignedFrame(void) {
    int r = 0;

    Recorded out;

    // 256x256 is exactly two by two 128x128 superblocks
    r |= assertEquals(walkWith(0x00, 256, 256, 1, &out), TINYIMG_OK);
    r |= assertEquals((long) out.count, 4L);

    uint32_t holes = 0;
    uint32_t doubled = 0;
    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    // every block is a whole superblock, because symbol zero is PARTITION_NONE
    for (uint32_t i = 0; i < out.count; i++) {
        r |= assertEquals((long) out.sizes[i], (long) TINY_AV1_BLOCK_128X128);
    }

    return r;
}

/**
 * A frame whose extent is not a superblock multiple.
 *
 * The right and bottom superblocks are partly outside it, so the partition is
 * forced rather than coded and the sub-blocks that fall outside are not coded
 * at all. This is where the coverage invariant earns its place.
 */
static int coversRaggedFrame(void) {
    int r = 0;

    Recorded out;

    // 1204x800 is fox.avif's extent: 302x200 mi, which is neither a multiple of
    // 32 mi (128 samples) nor of 16
    r |= assertEquals(walkWith(0x00, 1204, 800, 1, &out), TINYIMG_OK);
    r |= assertGreaterThan((double) out.count, 0.0);

    uint32_t holes = 0;
    uint32_t doubled = 0;
    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    // and no block starts outside the frame
    int inside = 1;

    for (uint32_t i = 0; i < out.count; i++) {
        if (out.rows[i] >= out.mi_rows || out.cols[i] >= out.mi_cols) {
            inside = 0;
        }
    }

    r |= assertTrue(inside);

    return r;
}

/**
 * The same coverage at 64x64 superblocks, which changes the tree's root.
 *
 * Two frames, because the interesting halves are different. The aligned one
 * pins the block size, since every superblock is whole and PARTITION_NONE keeps
 * it. The ragged one cannot: at the bottom edge the rows are unavailable, so
 * the specification forces `split_or_horz` and the block comes out 64x32 rather
 * than 64x64. Asserting 64x64 there looked like a bug in the walk and was a bug
 * in the expectation.
 */
static int coversWith64Superblocks(void) {
    int r = 0;

    Recorded out;

    // 256x256 is exactly four by four 64x64 superblocks
    r |= assertEquals(walkWith(0x00, 256, 256, 0, &out), TINYIMG_OK);
    r |= assertEquals((long) out.count, 16L);

    uint32_t holes = 0;
    uint32_t doubled = 0;
    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    for (uint32_t i = 0; i < out.count; i++) {
        r |= assertEquals((long) out.sizes[i], (long) TINY_AV1_BLOCK_64X64);
    }

    // and the ragged frame still covers exactly, with the edge blocks halved
    r |= assertEquals(walkWith(0x00, 1204, 800, 0, &out), TINYIMG_OK);

    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    int halved = 0;

    for (uint32_t i = 0; i < out.count; i++) {
        if (out.sizes[i] == TINY_AV1_BLOCK_64X32) halved = 1;
    }

    r |= assertTrue(halved);

    return r;
}

/**
 * A stream that keeps splitting, which is the deep end of the recursion.
 *
 * An all-ones tile drives the value to the far end of every interval, so each
 * partition reads its last symbol. The tree then descends as far as the block
 * sizes allow, and the invariant still has to hold at the bottom.
 */
static int coversWhenSplitting(void) {
    int r = 0;

    Recorded out;

    r |= assertEquals(walkWith(0xFF, 256, 256, 1, &out), TINYIMG_OK);

    // more blocks than the four a non-split walk produced, or the stream did
    // not actually drive a split and this case proves nothing
    r |= assertGreaterThan((double) out.count, 4.0);

    uint32_t holes = 0;
    uint32_t doubled = 0;
    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    return r;
}

/** A single mi column and row, which is the smallest frame that codes at all.
 */
static int coversSmallestFrame(void) {
    int r = 0;

    Recorded out;

    r |= assertEquals(walkWith(0x00, 4, 4, 1, &out), TINYIMG_OK);
    r |= assertEquals((long) out.mi_cols, 2L);
    r |= assertEquals((long) out.mi_rows, 2L);
    r |= assertGreaterThan((double) out.count, 0.0);

    uint32_t holes = 0;
    uint32_t doubled = 0;
    coverage(&out, &holes, &doubled);

    r |= assertEquals((long) holes, 0L);
    r |= assertEquals((long) doubled, 0L);

    return r;
}

/** A missing member is refused rather than dereferenced. */
static int refusesIncompleteWalk(void) {
    int r = 0;

    TinyAv1Walk walk;
    tiny_memset(&walk, 0, sizeof(walk));

    r |= assertEquals(tiny_av1_walk_tile(&walk), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_av1_walk_tile(0), TINYIMG_ERR_NULL);

    return r;
}

int main(void) {
    int r = 0;

    r |= coversAlignedFrame();
    r |= coversRaggedFrame();
    r |= coversWith64Superblocks();
    r |= coversWhenSplitting();
    r |= coversSmallestFrame();
    r |= refusesIncompleteWalk();

    return r;
}
