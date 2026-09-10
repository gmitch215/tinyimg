#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"

/** Deepest the partition tree can go, from 128x128 down to 4x4. */
#define AV1_MAX_PARTITION_DEPTH 6

#pragma region partition

/** Whether an mi position is inside the tile being walked. */
static int is_inside(const TinyAv1Walk* walk, int32_t row, int32_t col) {
    return col >= (int32_t) walk->col_start && col < (int32_t) walk->col_end &&
           row >= (int32_t) walk->row_start && row < (int32_t) walk->row_end;
}

/**
 * The partition context, from the two neighbors already decoded.
 *
 * A neighbor narrower than this block means the picture is splitting up around
 * here, which is what the context encodes. The above neighbor is compared on
 * width and the left one on height, which is not symmetric and is easy to write
 * the same way twice.
 */
static uint32_t partition_context(
    const TinyAv1Walk* walk, uint32_t row, uint32_t col, uint32_t bsl
) {
    uint32_t above = 0;
    uint32_t left = 0;

    if (is_inside(walk, (int32_t) row - 1, (int32_t) col)) {
        uint8_t size =
            walk->sizes[(size_t) (row - 1u) * walk->frame->mi_cols + col];

        above = tiny_av1_mi_width_log2[size] < bsl ? 1u : 0u;
    }

    if (is_inside(walk, (int32_t) row, (int32_t) col - 1)) {
        uint8_t size =
            walk->sizes[(size_t) row * walk->frame->mi_cols + (col - 1u)];

        left = tiny_av1_mi_height_log2[size] < bsl ? 1u : 0u;
    }

    return left * 2u + above;
}

/** The distribution and symbol count for a partition at this block size. */
static uint16_t* partition_cdf(
    TinyAv1Walk* walk, uint32_t bsl, uint32_t ctx, uint32_t* count
) {
    switch (bsl) {
        case 1: *count = 4u; return walk->cdf->partition_w8[ctx];
        case 2: *count = 10u; return walk->cdf->partition_w16[ctx];
        case 3: *count = 10u; return walk->cdf->partition_w32[ctx];
        case 4: *count = 10u; return walk->cdf->partition_w64[ctx];
        default:
            // 128x128 has no four-way split, so eight symbols rather than ten
            *count = 8u;
            return walk->cdf->partition_w128[ctx];
    }
}

/**
 * One interval of a cumulative distribution.
 *
 * The specification writes the split_or_horz distribution as a sum of the
 * intervals of several symbols, and an interval is the difference between a
 * cumulative value and the one before it. Index zero has no predecessor, so its
 * interval runs from the start.
 */
static uint32_t interval(const uint16_t* cdf, uint32_t index) {
    uint32_t high = cdf[index];
    uint32_t low = index == 0u ? 0u : cdf[index - 1u];

    return high - low;
}

/**
 * Reads the partition at one node of the tree.
 *
 * Three cases beyond the ordinary one, all at the frame's edges. With only
 * columns available the choice collapses to split or horizontal, with only rows
 * to split or vertical, and with neither it is a split with nothing coded. The
 * two collapsed cases are read against a distribution built from the full one
 * by summing the intervals of every symbol they stand for, which is why they
 * cannot simply reuse it.
 */
static TinyAv1Partition read_partition(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    int has_rows, int has_cols
) {
    if (size < TINY_AV1_BLOCK_8X8) return TINY_AV1_PARTITION_NONE;

    uint32_t bsl = tiny_av1_mi_width_log2[size];
    uint32_t ctx = partition_context(walk, row, col, bsl);

    uint32_t count = 0;
    uint16_t* cdf = partition_cdf(walk, bsl, ctx, &count);

    if (has_rows && has_cols) {
        return (TinyAv1Partition) tiny_av1_symbol_read(
            walk->symbol, cdf, count
        );
    }

    if (!has_rows && !has_cols) return TINY_AV1_PARTITION_SPLIT;

    /*
     * The collapsed distributions, built rather than stored.
     *
     * Summing the intervals of the symbols that imply a split gives the
     * probability of splitting; the rest is the one other choice. The sum's
     * membership differs between the two cases and excludes the four-way split
     * at 128x128, where it does not exist.
     */
    uint32_t psum;

    if (has_cols) {
        psum = interval(cdf, TINY_AV1_PARTITION_VERT) +
               interval(cdf, TINY_AV1_PARTITION_SPLIT) +
               interval(cdf, TINY_AV1_PARTITION_HORZ_A) +
               interval(cdf, TINY_AV1_PARTITION_VERT_A) +
               interval(cdf, TINY_AV1_PARTITION_VERT_B);

        if (size != TINY_AV1_BLOCK_128X128) {
            psum += interval(cdf, TINY_AV1_PARTITION_VERT_4);
        }
    }
    else {
        psum = interval(cdf, TINY_AV1_PARTITION_HORZ) +
               interval(cdf, TINY_AV1_PARTITION_SPLIT) +
               interval(cdf, TINY_AV1_PARTITION_HORZ_A) +
               interval(cdf, TINY_AV1_PARTITION_HORZ_B) +
               interval(cdf, TINY_AV1_PARTITION_VERT_A);

        if (size != TINY_AV1_BLOCK_128X128) {
            psum += interval(cdf, TINY_AV1_PARTITION_HORZ_4);
        }
    }

    // a two symbol distribution, and the read must not adapt it: the values
    // are derived from another distribution and are thrown away
    uint16_t collapsed[3] = {(uint16_t) ((1u << 15) - psum), 1u << 15, 0};

    uint8_t was_frozen = walk->symbol->frozen;
    walk->symbol->frozen = 1u;

    uint32_t split = tiny_av1_symbol_read(walk->symbol, collapsed, 2);

    walk->symbol->frozen = was_frozen;

    if (has_cols) {
        return split ? TINY_AV1_PARTITION_SPLIT : TINY_AV1_PARTITION_HORZ;
    }

    return split ? TINY_AV1_PARTITION_SPLIT : TINY_AV1_PARTITION_VERT;
}

/** Records a block's size across the positions it covers, then calls the sink.
 */
static int emit(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size
) {
    if (walk->error != TINYIMG_OK) return walk->error;
    if (row >= walk->frame->mi_rows || col >= walk->frame->mi_cols) {
        return TINYIMG_OK;
    }

    uint32_t wide = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t high = tiny_av1_num_4x4_blocks_high[size];

    for (uint32_t y = 0; y < high && row + y < walk->frame->mi_rows; y++) {
        for (uint32_t x = 0; x < wide && col + x < walk->frame->mi_cols; x++) {
            walk->sizes[(size_t) (row + y) * walk->frame->mi_cols + (col + x)] =
                (uint8_t) size;
        }
    }

    if (walk->block) {
        walk->error = walk->block(walk, row, col, size);
    }

    return walk->error;
}

static int decode_partition(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    uint32_t depth
);

/**
 * The eight non-square partitions, each with its own sub-block placement.
 *
 * Written out rather than derived, because the specification writes them out
 * and the difference between HORZ_A and HORZ_B is which half is split. Deriving
 * them from a rule would be shorter and would hide exactly the kind of mistake
 * this is prone to.
 */
static int decode_children(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    TinyAv1Partition partition, uint32_t depth
) {
    uint32_t num4x4 = tiny_av1_num_4x4_blocks_wide[size];
    uint32_t half = num4x4 >> 1;
    uint32_t quarter = half >> 1;

    TinyAv1BlockSize sub =
        (TinyAv1BlockSize) tiny_av1_partition_subsize[partition][size];
    TinyAv1BlockSize split = (TinyAv1BlockSize)
        tiny_av1_partition_subsize[TINY_AV1_PARTITION_SPLIT][size];

    int has_rows = (row + half) < walk->frame->mi_rows;
    int has_cols = (col + half) < walk->frame->mi_cols;

    switch (partition) {
        case TINY_AV1_PARTITION_NONE: return emit(walk, row, col, sub);

        case TINY_AV1_PARTITION_HORZ:
            emit(walk, row, col, sub);
            if (has_rows) emit(walk, row + half, col, sub);
            return walk->error;

        case TINY_AV1_PARTITION_VERT:
            emit(walk, row, col, sub);
            if (has_cols) emit(walk, row, col + half, sub);
            return walk->error;

        case TINY_AV1_PARTITION_SPLIT:
            decode_partition(walk, row, col, sub, depth + 1u);
            decode_partition(walk, row, col + half, sub, depth + 1u);
            decode_partition(walk, row + half, col, sub, depth + 1u);
            decode_partition(walk, row + half, col + half, sub, depth + 1u);
            return walk->error;

        case TINY_AV1_PARTITION_HORZ_A:
            emit(walk, row, col, split);
            emit(walk, row, col + half, split);
            emit(walk, row + half, col, sub);
            return walk->error;

        case TINY_AV1_PARTITION_HORZ_B:
            emit(walk, row, col, sub);
            emit(walk, row + half, col, split);
            emit(walk, row + half, col + half, split);
            return walk->error;

        case TINY_AV1_PARTITION_VERT_A:
            emit(walk, row, col, split);
            emit(walk, row + half, col, split);
            emit(walk, row, col + half, sub);
            return walk->error;

        case TINY_AV1_PARTITION_VERT_B:
            emit(walk, row, col, sub);
            emit(walk, row, col + half, split);
            emit(walk, row + half, col + half, split);
            return walk->error;

        case TINY_AV1_PARTITION_HORZ_4:
            for (uint32_t i = 0; i < 4u; i++) {
                uint32_t at = row + quarter * i;

                // the fourth strip is only coded when it is inside the frame,
                // and the first three always are
                if (i == 3u && at >= walk->frame->mi_rows) break;

                emit(walk, at, col, sub);
            }
            return walk->error;

        default:
            for (uint32_t i = 0; i < 4u; i++) {
                uint32_t at = col + quarter * i;

                if (i == 3u && at >= walk->frame->mi_cols) break;

                emit(walk, row, at, sub);
            }
            return walk->error;
    }
}

static int decode_partition(
    TinyAv1Walk* walk, uint32_t row, uint32_t col, TinyAv1BlockSize size,
    uint32_t depth
) {
    if (walk->error != TINYIMG_OK) return walk->error;

    if (row >= walk->frame->mi_rows || col >= walk->frame->mi_cols) {
        return TINYIMG_OK;
    }

    // the tree cannot be deeper than 128x128 down to 4x4, so a deeper one is a
    // corrupt stream rather than a picture, and the recursion has to stop
    // before it runs the stack out
    if (depth > AV1_MAX_PARTITION_DEPTH) {
        walk->error = TINYIMG_ERR_CORRUPT;
        return walk->error;
    }

    if (size >= TINY_AV1_BLOCK_INVALID) {
        walk->error = TINYIMG_ERR_CORRUPT;
        return walk->error;
    }

    uint32_t half = tiny_av1_num_4x4_blocks_wide[size] >> 1;
    int has_rows = (row + half) < walk->frame->mi_rows;
    int has_cols = (col + half) < walk->frame->mi_cols;

    TinyAv1Partition partition =
        read_partition(walk, row, col, size, has_rows, has_cols);

    if (tiny_av1_partition_subsize[partition][size] >= TINY_AV1_BLOCK_INVALID) {
        walk->error = TINYIMG_ERR_CORRUPT;
        return walk->error;
    }

    return decode_children(walk, row, col, size, partition, depth);
}

#pragma endregion

int tiny_av1_walk_tile(TinyAv1Walk* walk) {
    if (!walk || !walk->frame || !walk->sequence || !walk->symbol ||
        !walk->cdf || !walk->sizes) {
        return TINYIMG_ERR_NULL;
    }

    walk->error = TINYIMG_OK;

    // the loop filter deltas start at zero per tile, and so does the quantizer,
    // which the caller sets from base_q_idx before it gets here
    for (uint32_t i = 0; i < TINY_AV1_FRAME_LF_COUNT; i++) {
        walk->delta_lf[i] = 0;
    }

    if (walk->residual) tiny_av1_residual_clear_above(walk);

    TinyAv1BlockSize sb = walk->sequence->use_128x128_superblock
                              ? TINY_AV1_BLOCK_128X128
                              : TINY_AV1_BLOCK_64X64;

    uint32_t step = tiny_av1_num_4x4_blocks_wide[sb];

    for (uint32_t row = walk->row_start; row < walk->row_end; row += step) {
        // the left level context is per superblock row, not per tile: a new row
        // of superblocks has no decoded neighbor to its left
        if (walk->residual) tiny_av1_residual_clear_left(walk);

        for (uint32_t col = walk->col_start; col < walk->col_end; col += step) {
            /*
             * Per superblock, before the partition tree.
             *
             * `ReadDeltas` is set here rather than once per tile, because the
             * quantizer delta is coded on the first block of each superblock
             * and the mode info clears the flag once it has read it. Loop
             * restoration units would also be read at this point; every file
             * measured has restoration off, and when one does not,
             * tiny_av1_frame_supported refuses it rather than reading past its
             * symbols.
             */
            walk->read_deltas = walk->frame->delta_q_present;

            if (walk->superblock) {
                walk->error = walk->superblock(walk, row, col);

                if (walk->error != TINYIMG_OK) return walk->error;
            }

            decode_partition(walk, row, col, sb, 0);

            if (walk->error != TINYIMG_OK) return walk->error;
        }
    }

    return TINYIMG_OK;
}
