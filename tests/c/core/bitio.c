#include "../test.h"
#include "tinyimg/util.h"

int main(void) {
    int r = 0;

    // 0xB5 is 10110101, so the two orders have to disagree on every read
    const uint8_t byte[1] = {0xB5};

    TinyBitReader reader;
    tiny_bits_init(&reader, byte, 1);
    r |= assertEquals((long) tiny_bits_msb(&reader, 3), 5L);  // 101
    r |= assertEquals((long) tiny_bits_msb(&reader, 5), 21L); // 10101

    tiny_bits_init(&reader, byte, 1);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 3), 5L);  // 101
    r |= assertEquals((long) tiny_bits_lsb(&reader, 5), 22L); // 10110

    // a peek leaves the head where it was
    tiny_bits_init(&reader, byte, 1);
    r |= assertEquals((long) tiny_bits_peek_msb(&reader, 4), 11L);
    r |= assertEquals((long) tiny_bits_peek_msb(&reader, 4), 11L);
    r |= assertEquals((long) tiny_bits_msb(&reader, 4), 11L);
    r |= assertEquals((long) tiny_bits_msb(&reader, 4), 5L);

    tiny_bits_init(&reader, byte, 1);
    r |= assertEquals((long) tiny_bits_peek_lsb(&reader, 4), 5L);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 4), 5L);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 4), 11L);

    // aligning drops the partial byte at the head, and the two orders keep that
    // partial byte at opposite ends of the accumulator
    const uint8_t pair[2] = {0xB5, 0x3C};

    tiny_bits_init(&reader, pair, 2);
    tiny_bits_msb(&reader, 3);
    tiny_bits_align_msb(&reader);
    r |= assertEquals((long) tiny_bits_msb(&reader, 8), 0x3CL);

    tiny_bits_init(&reader, pair, 2);
    tiny_bits_lsb(&reader, 3);
    tiny_bits_align_lsb(&reader);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 8), 0x3CL);

    // an aligned reader is already aligned
    tiny_bits_init(&reader, pair, 2);
    tiny_bits_msb(&reader, 8);
    tiny_bits_align_msb(&reader);
    r |= assertEquals((long) tiny_bits_msb(&reader, 8), 0x3CL);

    // reading past the end yields zeroes and latches, so a truncated scan is
    // detected once instead of per symbol
    tiny_bits_init(&reader, byte, 1);
    r |= assertFalse(reader.overrun);
    tiny_bits_msb(&reader, 8);
    r |= assertFalse(reader.overrun);
    r |= assertEquals((long) tiny_bits_msb(&reader, 8), 0L);
    r |= assertTrue(reader.overrun);

    // refilling past the end is not itself an overrun; consuming those bits is.
    // the accumulator pulls whole bytes ahead of need, so a stream whose last
    // symbol ends exactly on the final byte refills past it while every bit it
    // uses is real. latching on the refill reported valid bare DEFLATE streams
    // as truncated
    tiny_bits_init(&reader, pair, 2);
    r |= assertEquals((long) tiny_bits_peek_lsb(&reader, 24), 0x3CB5L);
    r |= assertFalse(reader.overrun);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 16), 0x3CB5L);
    r |= assertFalse(reader.overrun);
    r |= assertEquals((long) tiny_bits_lsb(&reader, 1), 0L);
    r |= assertTrue(reader.overrun);

    tiny_bits_init(&reader, pair, 2);
    r |= assertEquals((long) tiny_bits_peek_msb(&reader, 24), 0xB53C00L);
    r |= assertFalse(reader.overrun);
    r |= assertEquals((long) tiny_bits_msb(&reader, 16), 0xB53CL);
    r |= assertFalse(reader.overrun);
    r |= assertEquals((long) tiny_bits_msb(&reader, 1), 0L);
    r |= assertTrue(reader.overrun);

    // aligning away a partial byte that came from past the end must not leave
    // the reader thinking it still holds phantom bits
    tiny_bits_init(&reader, pair, 2);
    tiny_bits_lsb(&reader, 20);
    r |= assertTrue(reader.overrun);
    tiny_bits_align_lsb(&reader);
    r |= assertEquals((long) reader.phantom, 0L);

    // out of range widths are refused rather than corrupting the accumulator
    tiny_bits_init(&reader, pair, 2);
    r |= assertEquals((long) tiny_bits_msb(&reader, 0), 0L);
    r |= assertEquals((long) tiny_bits_msb(&reader, 25), 0L);
    r |= assertEquals((long) tiny_bits_msb(&reader, 8), 0xB5L);

    tiny_bits_init(&reader, pair, 2);
    r |= assertEquals((long) tiny_bits_remaining(&reader), 2L);
    tiny_bits_msb(&reader, 4);
    r |= assertEquals((long) tiny_bits_remaining(&reader), 1L);

    tiny_bits_init(&reader, pair, 2);
    tiny_bits_skip_msb(&reader, 12);
    r |= assertEquals((long) tiny_bits_msb(&reader, 4), 0xCL);

    // the writer is the reader's inverse, which is the only check that pins the
    // bit order of both at once
    static const uint32_t values[12] = {1,    0, 5,     255, 1023, 7,
                                        4095, 2, 16383, 9,   31,   1048575};
    static const uint32_t widths[12] = {1, 3, 3, 8, 10, 4, 12, 2, 14, 5, 5, 20};

    TinyWriter out;
    TinyBitWriter bits;

    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    tiny_bitwriter_init(&bits, &out);

    for (int i = 0; i < 12; i++) {
        tiny_bitwriter_msb(&bits, values[i], widths[i]);
    }
    tiny_bitwriter_flush_msb(&bits);
    r |= assertEquals(out.error, TINYIMG_OK);

    tiny_bits_init(&reader, out.data, out.size);
    for (int i = 0; i < 12; i++) {
        r |= assertEquals(
            (long) tiny_bits_msb(&reader, widths[i]), (long) values[i]
        );
    }
    r |= assertFalse(reader.overrun);
    tiny_writer_free(&out);

    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    tiny_bitwriter_init(&bits, &out);

    for (int i = 0; i < 12; i++) {
        tiny_bitwriter_lsb(&bits, values[i], widths[i]);
    }
    tiny_bitwriter_flush_lsb(&bits);

    tiny_bits_init(&reader, out.data, out.size);
    for (int i = 0; i < 12; i++) {
        r |= assertEquals(
            (long) tiny_bits_lsb(&reader, widths[i]), (long) values[i]
        );
    }
    r |= assertFalse(reader.overrun);

    // entropy coded JPEG segments pad with one bits
    tiny_writer_free(&out);
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    tiny_bitwriter_init(&bits, &out);
    tiny_bitwriter_msb(&bits, 0, 4);
    tiny_bitwriter_flush_msb(&bits);
    r |= assertEquals((long) out.size, 1L);
    r |= assertEquals((long) out.data[0], 0x0FL);
    tiny_writer_free(&out);

    // and DEFLATE pads with zeroes
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    tiny_bitwriter_init(&bits, &out);
    tiny_bitwriter_lsb(&bits, 0x0F, 4);
    tiny_bitwriter_flush_lsb(&bits);
    r |= assertEquals((long) out.size, 1L);
    r |= assertEquals((long) out.data[0], 0x0FL);
    tiny_writer_free(&out);

    return r;
}
