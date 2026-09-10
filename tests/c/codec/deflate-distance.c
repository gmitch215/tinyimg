#include "tinyimg/codec/deflate.h"

#include "test.h"
#include "tinyimg/memory.h"

/**
 * A back reference must not reach past what the stream has produced.
 *
 * The window is a 32 KiB ring taken from the arena, which does not zero, so a
 * distance larger than the output so far names a slot that was never written
 * rather than one that scrolled away. Checking only against the window size let
 * a crafted PNG or a DEFLATE-compressed TIFF return whatever the allocator last
 * left there, as image data, with a success code. In a Worker the arena is
 * where the previous request's planes, fonts and profiles lived.
 *
 * Both streams below were built by hand and **checked against zlib**, which is
 * what makes this a real check rather than a comparison with our own reading of
 * RFC 1951. They are the same distance and length; only the number of literals
 * before the reference differs, so a fix that rejects the first while still
 * accepting the second is bounded correctly. A guard that refused both would
 * pass a test that only asserted the rejection.
 */

/**
 * One literal, then length 4 at distance 9.
 *
 * zlib: "invalid distance too far back".
 */
static const uint8_t far_distance[] = {0x78, 0x01, 0x63, 0x00, 0x31, 0x00};

/**
 * Ten literals, then the same length 4 at the same distance 9.
 *
 * zlib decompresses it to fourteen bytes,
 * `00 01 02 03 04 05 06 07 08 09 01 02 03 04`.
 */
static const uint8_t near_distance[] = {0x78, 0x01, 0x63, 0x60, 0x64,
                                        0x62, 0x66, 0x61, 0x65, 0x63,
                                        0xE7, 0xE0, 0x04, 0x31, 0x00,
                                        0x01, 0x7B, 0x00, 0x38};

/** What zlib produces from `near_distance`. */
static const uint8_t expected[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                   0x07, 0x08, 0x09, 0x01, 0x02, 0x03, 0x04};

static int rejectsFarDistance(void) {
    int r = 0;

    TinyInflate state;
    r |= assertEquals(
        tiny_inflate_init(&state, far_distance, sizeof(far_distance), 1),
        TINYIMG_OK
    );

    uint8_t out[64];
    long read = tiny_inflate_read(&state, out, sizeof(out));

    // the failure has to be reported rather than absorbed, and it has to be
    // the corruption code rather than a short read
    r |= assertTrue(read < 0);
    r |= assertEquals(read, (long) TINYIMG_ERR_CORRUPT);

    return r;
}

static int acceptsReachableDistance(void) {
    int r = 0;

    TinyInflate state;
    r |= assertEquals(
        tiny_inflate_init(&state, near_distance, sizeof(near_distance), 1),
        TINYIMG_OK
    );

    uint8_t out[64];
    long read = tiny_inflate_read(&state, out, sizeof(out));

    r |= assertEquals(read, (long) sizeof(expected));

    if (read == (long) sizeof(expected)) {
        r |= assertBytesMatch(out, expected, sizeof(expected));
    }

    return r;
}

/** The whole-buffer entry point is the one PNG and TIFF actually call. */
static int rejectsThroughInflateAll(void) {
    int r = 0;

    TinyWriter writer;
    r |= assertEquals(tiny_writer_init(&writer, 64), TINYIMG_OK);

    int result =
        tiny_inflate_all(far_distance, sizeof(far_distance), 1, &writer);

    r |= assertTrue(result < 0);

    tiny_writer_free(&writer);

    return r;
}

int main(void) {
    int r = 0;

    // the inflate state takes its window and code tables from the arena, which
    // a caller scopes rather than frees, so this is the codecs' own pattern
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    r |= rejectsFarDistance();
    r |= acceptsReachableDistance();
    r |= rejectsThroughInflateAll();

    tiny_arena_release(&mark);

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    return r;
}
