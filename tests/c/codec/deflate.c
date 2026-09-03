#include "tinyimg/codec/deflate.h"
#include "../test.h"
#include "tinyimg/memory.h"

/**
 * @brief Streams a buffer out and back, and reports whether it survived.
 *
 * The round trip is the check that matters, but on its own it would pass if
 * both directions shared the same misreading of the format. The vectors below
 * pin the decoder against streams this code did not write, and the node lane
 * checks the encoder's output against zlib itself.
 */
static int roundTrip(
    const uint8_t* data, size_t size, TinyDeflateLevel level, int zlib,
    size_t* compressed
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    TinyWriter out;
    tiny_writer_init(&out, 0);

    int ok = 0;
    if (tiny_deflate(data, size, level, zlib, &out) == TINYIMG_OK) {
        if (compressed) *compressed = out.size;

        TinyWriter back;
        tiny_writer_init(&back, 0);

        if (tiny_inflate_all(out.data, out.size, zlib, &back) == TINYIMG_OK) {
            ok = back.size == size &&
                 (size == 0 || tiny_memcmp(back.data, data, size) == 0);
        }
        tiny_writer_free(&back);
    }

    tiny_writer_free(&out);
    tiny_arena_release(&mark);
    return ok;
}

int main(void) {
    int r = 0;

    // #region vectors this code did not write

    // "hello" as a stored block: 01 05 00 fa ff then the bytes
    static const uint8_t stored[] = {0x01, 0x05, 0x00, 0xFA, 0xFF,
                                     'h',  'e',  'l',  'l',  'o'};
    TinyWriter out;
    TinyArenaMark mark;

    tiny_arena_mark(&mark);
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_inflate_all(stored, sizeof(stored), 0, &out), TINYIMG_OK
    );
    r |= assertEquals((long) out.size, 5L);
    r |= assertBytesMatch(out.data, "hello", 5);
    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    // the same five bytes under fixed Huffman, which zlib -1 produces
    static const uint8_t fixed[] = {0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x07, 0x00};

    tiny_arena_mark(&mark);
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_inflate_all(fixed, sizeof(fixed), 0, &out), TINYIMG_OK
    );
    r |= assertEquals((long) out.size, 5L);
    r |= assertBytesMatch(out.data, "hello", 5);
    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    // a zlib wrapped stream, header and Adler-32 trailer included
    static const uint8_t wrapped[] = {0x78, 0x9C, 0xCB, 0x48, 0xCD, 0xC9, 0xC9,
                                      0x07, 0x00, 0x06, 0x2C, 0x02, 0x15};

    tiny_arena_mark(&mark);
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_inflate_all(wrapped, sizeof(wrapped), 1, &out), TINYIMG_OK
    );
    r |= assertEquals((long) out.size, 5L);
    r |= assertBytesMatch(out.data, "hello", 5);
    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    // a back reference that overlaps itself, which is how a run is coded:
    // "aaaaaaaa" from one literal and a length 7 distance 1 match
    static const uint8_t overlapping[] = {0x78, 0x9C, 0x4B, 0x4C, 0x84, 0x00,
                                          0x00, 0x0D, 0xAC, 0x03, 0x09};

    tiny_arena_mark(&mark);
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_inflate_all(overlapping, sizeof(overlapping), 1, &out), TINYIMG_OK
    );
    r |= assertEquals((long) out.size, 8L);
    r |= assertBytesMatch(out.data, "aaaaaaaa", 8);
    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    // #endregion

    // #region malformed input

    tiny_arena_mark(&mark);
    tiny_writer_init(&out, 0);

    // block type 3 is reserved
    static const uint8_t reserved[] = {0x07, 0x00};
    r |= assertEquals(
        tiny_inflate_all(reserved, sizeof(reserved), 0, &out),
        TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);

    // a stored block whose length and complement disagree
    tiny_writer_init(&out, 0);
    static const uint8_t badLength[] = {0x01, 0x05, 0x00, 0x00, 0x00,
                                        'h',  'e',  'l',  'l',  'o'};
    r |= assertEquals(
        tiny_inflate_all(badLength, sizeof(badLength), 0, &out),
        TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);

    // a truncated stream is corrupt rather than a short read
    tiny_writer_init(&out, 0);
    r |= assertEquals(
        tiny_inflate_all(fixed, sizeof(fixed) - 3, 0, &out), TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);

    // a zlib header with the wrong method, and one whose check bits fail
    tiny_writer_init(&out, 0);
    static const uint8_t badMethod[] = {0x79, 0x9C, 0x03, 0x00};
    r |= assertEquals(
        tiny_inflate_all(badMethod, sizeof(badMethod), 1, &out),
        TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);

    tiny_writer_init(&out, 0);
    static const uint8_t badCheck[] = {0x78, 0x9D, 0x03, 0x00};
    r |= assertEquals(
        tiny_inflate_all(badCheck, sizeof(badCheck), 1, &out),
        TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);

    // a good stream with a corrupted Adler trailer, which is the only thing the
    // trailer catches
    uint8_t tampered[sizeof(wrapped)];
    tiny_memcpy(tampered, wrapped, sizeof(wrapped));
    tampered[sizeof(wrapped) - 1] ^= 0xFF;

    tiny_writer_init(&out, 0);
    r |= assertEquals(
        tiny_inflate_all(tampered, sizeof(tampered), 1, &out),
        TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    r |= assertEquals(tiny_inflate_all(0, 4, 0, 0), TINYIMG_ERR_NULL);
    r |= assertEquals(
        tiny_deflate(0, 4, TINYIMG_DEFLATE_DEFAULT, 0, 0), TINYIMG_ERR_NULL
    );

    // #endregion

    // #region round trips

    static const uint8_t empty[1] = {0};
    for (int level = 0; level <= 3; level++) {
        r |= assertTrue(roundTrip(empty, 0, (TinyDeflateLevel) level, 1, 0));
        r |= assertTrue(roundTrip(empty, 1, (TinyDeflateLevel) level, 1, 0));
        r |= assertTrue(roundTrip(empty, 1, (TinyDeflateLevel) level, 0, 0));
    }

    // a run longer than the 258 byte maximum match, so it has to be coded as
    // several
    static uint8_t run[100000];
    tiny_memset(run, 0x5A, sizeof(run));

    size_t compressed = 0;
    r |= assertTrue(
        roundTrip(run, sizeof(run), TINYIMG_DEFLATE_DEFAULT, 1, &compressed)
    );
    r |= assertLessThan((double) compressed, 500.0);

    // text, where matching and entropy coding both have something to work with
    static uint8_t text[44 * 400];
    for (size_t i = 0; i < sizeof(text); i++) {
        text[i] =
            (uint8_t) "the quick brown fox jumps over the lazy dog "[i % 44];
    }
    r |= assertTrue(
        roundTrip(text, sizeof(text), TINYIMG_DEFLATE_DEFAULT, 1, &compressed)
    );
    r |= assertLessThan((double) compressed, 200.0);

    // incompressible input must not grow by more than a stored block's header
    static uint8_t noise[50000];
    uint32_t seed = 12345;
    for (size_t i = 0; i < sizeof(noise); i++) {
        seed = seed * 1103515245u + 12345u;
        noise[i] = (uint8_t) (seed >> 16);
    }
    r |= assertTrue(
        roundTrip(noise, sizeof(noise), TINYIMG_DEFLATE_DEFAULT, 1, &compressed)
    );
    r |= assertLessThan((double) compressed, (double) sizeof(noise) + 64.0);

    // every level has to be correct, and every one has to be at or under what
    // no matching gives once the effort is past a single probe
    size_t sizes[4] = {0, 0, 0, 0};
    for (int level = 0; level <= 3; level++) {
        r |= assertTrue(roundTrip(
            text, sizeof(text), (TinyDeflateLevel) level, 1, &sizes[level]
        ));
    }
    r |= assertLessThan((double) sizes[2], (double) sizes[0]);
    r |= assertLessThan((double) sizes[3], (double) sizes[0] + 1.0);
    r |= assertLessThan((double) sizes[3], (double) sizes[2] + 1.0);

    // a bare stream and a wrapped one both work, and a wrapped one is four
    // bytes longer for the trailer plus two for the header
    size_t bare = 0;
    size_t wrappedSize = 0;
    r |= assertTrue(
        roundTrip(text, sizeof(text), TINYIMG_DEFLATE_DEFAULT, 0, &bare)
    );
    r |= assertTrue(
        roundTrip(text, sizeof(text), TINYIMG_DEFLATE_DEFAULT, 1, &wrappedSize)
    );
    r |= assertEquals((long) (wrappedSize - bare), 6L);

    // #endregion

    // #region streaming

    // the reader hands back what it is asked for and no more, which is what
    // lets the PNG codec unfilter one row at a time instead of holding the
    // whole decompressed image
    tiny_arena_mark(&mark);
    tiny_writer_init(&out, 0);
    r |= assertEquals(
        tiny_deflate(text, sizeof(text), TINYIMG_DEFLATE_DEFAULT, 1, &out),
        TINYIMG_OK
    );

    TinyInflate state;
    r |= assertEquals(
        tiny_inflate_init(&state, out.data, out.size, 1), TINYIMG_OK
    );

    uint8_t piece[97];
    size_t total = 0;
    int matched = 1;

    for (;;) {
        long read = tiny_inflate_read(&state, piece, sizeof(piece));
        if (read < 0) {
            matched = 0;
            break;
        }
        if (read == 0) break;

        if (total + (size_t) read > sizeof(text) ||
            tiny_memcmp(piece, text + total, (size_t) read) != 0) {
            matched = 0;
            break;
        }
        total += (size_t) read;
    }

    r |= assertTrue(matched);
    r |= assertEquals((long) total, (long) sizeof(text));
    r |= assertEquals(tiny_inflate_finish(&state), TINYIMG_OK);

    // a read larger than the window is clamped rather than overrunning the ring
    r |= assertEquals(
        tiny_inflate_init(&state, out.data, out.size, 1), TINYIMG_OK
    );

    uint8_t* wide = tiny_arena_alloc(TINY_DEFLATE_WINDOW * 2, 0);
    r |= assertNotNull(wide);
    if (wide) {
        long read = tiny_inflate_read(&state, wide, TINY_DEFLATE_WINDOW * 2);
        r |= assertGreaterThan((double) read, 0.0);
        r |=
            assertLessThan((double) read, (double) TINY_DEFLATE_MAX_READ + 1.0);
    }

    // finishing a stream that has not reached its final block is an error, not
    // a silent success
    r |= assertEquals(
        tiny_inflate_init(&state, out.data, out.size, 1), TINYIMG_OK
    );
    r |= assertEquals(tiny_inflate_finish(&state), TINYIMG_ERR_CORRUPT);

    tiny_writer_free(&out);
    tiny_arena_release(&mark);

    r |= assertEquals(tiny_inflate_init(0, stored, 4, 0), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_inflate_finish(0), TINYIMG_ERR_NULL);

    // #endregion

    // nothing the arena handed out is still held
    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    return r;
}
