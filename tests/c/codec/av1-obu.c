#include "codec/av1.h"

#include "test.h"
#include "tinyimg/memory.h"

/**
 * The OBU layer and the two headers, against `fox.avif`.
 *
 * **The expected values came from a separate parser.** Before any of this C
 * existed, the same bytes were walked by a throwaway TypeScript implementation
 * of the same specification sections, and the numbers below are what that
 * produced. Two independent readings of one document agreeing is a real check;
 * this file asserting whatever the C happens to compute would not be one.
 *
 * `fox.avif` is `cavif` output: profile 0, a reduced still picture header,
 * 1204x800, 128x128 superblocks, filter intra and the edge filter on, superres,
 * CDEF and loop restoration all off, 8-bit 4:2:0, and no film grain.
 */

/**
 * Locates the item's bytes inside the container.
 *
 * `base` is what the caller frees and `item` points into it, so nothing is
 * copied and the two cannot get out of step.
 */
static int itemBytes(
    unsigned char** base, const unsigned char** item, size_t* size
) {
    size_t whole = 0;
    unsigned char* file = readFixture("fox.avif", &whole);

    if (!file) return TINYIMG_ERR_NOT_FOUND;

    // the `mdat` payload is the item, and this file has exactly one. Found by
    // box walk rather than by a recorded offset, so a regenerated fixture works
    size_t at = 0;

    while (at + 8 <= whole) {
        uint32_t length = ((uint32_t) file[at] << 24) |
                          ((uint32_t) file[at + 1] << 16) |
                          ((uint32_t) file[at + 2] << 8) | file[at + 3];

        int is_mdat = file[at + 4] == 'm' && file[at + 5] == 'd' &&
                      file[at + 6] == 'a' && file[at + 7] == 't';

        if (length < 8 || at + length > whole) break;

        if (is_mdat) {
            *base = file;
            *item = file + at + 8;
            *size = length - 8u;
            return TINYIMG_OK;
        }

        at += length;
    }

    free(file);
    return TINYIMG_ERR_CORRUPT;
}

static int obuLayer(void) {
    int r = 0;

    unsigned char* base = 0;
    const unsigned char* item = 0;
    size_t size = 0;

    r |= assertEquals(itemBytes(&base, &item, &size), TINYIMG_OK);
    if (!item) return r;

    // three units, in this order, which is what a still item carries
    size_t at = 0;
    TinyAv1Obu obu;

    r |= assertTrue(tiny_av1_next_obu(item, size, &at, &obu));
    r |= assertEquals((long) obu.type, (long) TINY_AV1_OBU_SEQUENCE_HEADER);
    r |= assertEquals((long) obu.size, 10L);

    TinyAv1Sequence sequence;
    r |= assertEquals(
        tiny_av1_read_sequence(&sequence, obu.payload, obu.size), TINYIMG_OK
    );

    r |= assertEquals((long) sequence.profile, 0L);
    r |= assertEquals((long) sequence.still_picture, 1L);
    r |= assertEquals((long) sequence.reduced, 1L);
    r |= assertEquals((long) sequence.level, 5L);
    r |= assertEquals((long) sequence.max_width, 1204L);
    r |= assertEquals((long) sequence.max_height, 800L);
    r |= assertEquals((long) sequence.use_128x128_superblock, 1L);
    r |= assertEquals((long) sequence.enable_filter_intra, 1L);
    r |= assertEquals((long) sequence.enable_intra_edge_filter, 1L);
    r |= assertEquals((long) sequence.enable_superres, 0L);
    r |= assertEquals((long) sequence.enable_cdef, 0L);
    r |= assertEquals((long) sequence.enable_restoration, 0L);
    r |= assertEquals((long) sequence.bit_depth, 8L);
    r |= assertEquals((long) sequence.monochrome, 0L);
    r |= assertEquals((long) sequence.planes, 3L);
    r |= assertEquals((long) sequence.color_primaries, 1L);
    r |= assertEquals((long) sequence.transfer_characteristics, 13L);

    // BT.2020 non-constant luminance, which is what this encoder writes even
    // beside BT.709 primaries. Converting with 1 instead gives wrong colour
    r |= assertEquals((long) sequence.matrix_coefficients, 9L);

    // studio range, so the conversion has to expand it
    r |= assertEquals((long) sequence.color_range, 0L);
    r |= assertEquals((long) sequence.sub_x, 1L);
    r |= assertEquals((long) sequence.sub_y, 1L);
    r |= assertEquals((long) sequence.separate_uv_delta_q, 0L);
    r |= assertEquals((long) sequence.film_grain_params_present, 0L);

    r |= assertTrue(tiny_av1_next_obu(item, size, &at, &obu));
    r |= assertEquals((long) obu.type, (long) TINY_AV1_OBU_FRAME_HEADER);
    r |= assertEquals((long) obu.size, 7L);

    TinyAv1Frame frame;
    r |= assertEquals(
        tiny_av1_read_frame(&frame, &sequence, obu.payload, obu.size, 0),
        TINYIMG_OK
    );

    r |= assertEquals((long) frame.width, 1204L);
    r |= assertEquals((long) frame.height, 800L);
    r |= assertEquals((long) frame.upscaled_width, 1204L);
    r |= assertEquals((long) frame.render_width, 1204L);
    r |= assertEquals((long) frame.render_height, 800L);

    // mi units are four luma samples, rounded up to an eight sample pair
    r |= assertEquals((long) frame.mi_cols, 302L);
    r |= assertEquals((long) frame.mi_rows, 200L);

    // a real photograph, so the quantizer is not lossless and the derived flag
    // has to say so or the post-filters would be skipped
    r |= assertGreaterThan((double) frame.base_q_idx, 0.0);
    r |= assertEquals((long) frame.coded_lossless, 0L);
    r |= assertEquals((long) frame.all_lossless, 0L);

    // cdef and restoration are off in the sequence header, so their parameters
    // are implied rather than coded
    r |= assertEquals((long) frame.cdef_bits, 0L);
    r |= assertEquals((long) frame.cdef_damping, 3L);
    r |= assertEquals((long) frame.uses_lr, 0L);
    r |= assertEquals((long) frame.restoration_type[0], 0L);

    // one tile, which is what leaves the region lever with no seam to use
    r |= assertEquals((long) frame.tile_cols, 1L);
    r |= assertEquals((long) frame.tile_rows, 1L);
    r |= assertEquals((long) frame.tile_cols_log2, 0L);
    r |= assertEquals((long) frame.tile_rows_log2, 0L);
    r |= assertEquals((long) frame.mi_col_starts[0], 0L);
    r |= assertEquals((long) frame.mi_col_starts[1], 302L);
    r |= assertEquals((long) frame.mi_row_starts[1], 200L);

    r |= assertTrue(tiny_av1_next_obu(item, size, &at, &obu));
    r |= assertEquals((long) obu.type, (long) TINY_AV1_OBU_TILE_GROUP);
    r |= assertEquals((long) obu.size, 80393L);

    // and nothing after it
    r |= assertFalse(tiny_av1_next_obu(item, size, &at, &obu));

    free(base);
    return r;
}

/** A truncated or malformed unit has to stop the walk rather than read on. */
static int refusesBadObu(void) {
    int r = 0;

    // the forbidden bit set, which means this is not an OBU header
    const uint8_t forbidden[4] = {0x80, 0x00, 0x00, 0x00};
    size_t at = 0;
    TinyAv1Obu obu;

    r |=
        assertFalse(tiny_av1_next_obu(forbidden, sizeof(forbidden), &at, &obu));

    // a size field claiming more than the buffer holds
    const uint8_t oversize[4] = {0x0A, 0x7F, 0x00, 0x00};
    at = 0;
    r |= assertFalse(tiny_av1_next_obu(oversize, sizeof(oversize), &at, &obu));

    // a header byte with nothing after it
    const uint8_t bare[1] = {0x0A};
    at = 0;
    r |= assertFalse(tiny_av1_next_obu(bare, sizeof(bare), &at, &obu));

    return r;
}

/**
 * A header that runs off the end is corrupt rather than a decode in padding.
 *
 * The symbol decoder deliberately reads zeros past the end, because a
 * conformant tile finishes inside padding that was never written. A header has
 * no such allowance, so the two readers differ here on purpose.
 */
static int refusesTruncatedHeader(void) {
    int r = 0;

    const uint8_t partial[2] = {0x0C, 0x00};
    TinyAv1Sequence sequence;

    r |= assertTrue(
        tiny_av1_read_sequence(&sequence, partial, sizeof(partial)) < 0
    );

    r |= assertEquals(
        tiny_av1_read_sequence(0, partial, sizeof(partial)), TINYIMG_ERR_NULL
    );

    return r;
}

/** A sequence header without the reduced flag is refused, not half read. */
static int refusesFullSequenceHeader(void) {
    int r = 0;

    // profile 0, still_picture 0, reduced 0, so the parse reaches the timing
    // and operating point fields this build does not read. Bit four is the
    // reduced flag, so 0x08 would set it and take the other branch entirely
    const uint8_t full[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    TinyAv1Sequence sequence;

    r |= assertEquals(
        tiny_av1_read_sequence(&sequence, full, sizeof(full)),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    return r;
}

int main(void) {
    int r = 0;

    r |= obuLayer();
    r |= refusesBadObu();
    r |= refusesTruncatedHeader();
    r |= refusesFullSequenceHeader();

    return r;
}
