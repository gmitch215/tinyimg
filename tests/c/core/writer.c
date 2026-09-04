#include "../test.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

int main(void) {
    int r = 0;

    r |= assertEquals(tiny_writer_init(0, 0), TINYIMG_ERR_NULL);

    TinyWriter writer;
    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);

    // nothing is reserved until the first write
    r |= assertNull(writer.data);
    r |= assertEquals((long) writer.size, 0L);
    r |= assertEquals((long) writer.capacity, 0L);

    r |= assertEquals(tiny_writer_u8(&writer, 0x41), TINYIMG_OK);
    r |= assertEquals(tiny_writer_le16(&writer, 0x1234), TINYIMG_OK);
    r |= assertEquals(tiny_writer_be16(&writer, 0x1234), TINYIMG_OK);
    r |= assertEquals(tiny_writer_le32(&writer, 0xDEADBEEFu), TINYIMG_OK);
    r |= assertEquals(tiny_writer_be32(&writer, 0xDEADBEEFu), TINYIMG_OK);

    static const uint8_t wanted[13] = {0x41, 0x34, 0x12, 0x12, 0x34, 0xEF, 0xBE,
                                       0xAD, 0xDE, 0xDE, 0xAD, 0xBE, 0xEF};
    r |= assertEquals((long) writer.size, 13L);
    r |= assertBytesMatch(writer.data, wanted, 13);

    r |= assertEquals(tiny_writer_fill(&writer, 0x77, 3), TINYIMG_OK);
    r |= assertEquals((long) writer.size, 16L);
    r |= assertEquals((long) writer.data[15], 0x77L);

    // a zero length write is not an error and changes nothing
    r |= assertEquals(tiny_writer_write(&writer, wanted, 0), TINYIMG_OK);
    r |= assertEquals(tiny_writer_fill(&writer, 0, 0), TINYIMG_OK);
    r |= assertEquals((long) writer.size, 16L);

    r |= assertEquals((long) tiny_writer_size(&writer), 16L);
    r |= assertTrue(tiny_writer_data(&writer) == writer.data);
    r |= assertEquals((long) tiny_writer_size(0), 0L);
    r |= assertNull(tiny_writer_data(0));
    r |= assertGreaterThan((double) tiny_writer_sizeof(), 0.0);

    // growing past the initial capacity keeps everything already written
    for (int i = 0; i < 5000; i++) {
        tiny_writer_u8(&writer, (uint8_t) i);
    }
    r |= assertEquals(writer.error, TINYIMG_OK);
    r |= assertEquals((long) writer.size, 5016L);
    r |= assertBytesMatch(writer.data, wanted, 13);
    r |= assertEquals((long) writer.data[16], 0L);
    r |= assertEquals((long) writer.data[5015], (long) (uint8_t) 4999);
    r |=
        assertGreaterThan((double) writer.capacity, (double) writer.size - 1.0);

    // detaching hands the bytes over and leaves the writer empty
    size_t detached = 0;
    uint8_t* bytes = tiny_writer_detach(&writer, &detached);
    r |= assertNotNull(bytes);
    r |= assertEquals((long) detached, 5016L);
    r |= assertEquals((long) writer.size, 0L);
    r |= assertNull(writer.data);
    r |= assertEquals((long) bytes[0], 0x41L);
    tiny_free(bytes);

    // a writer is reusable after a free, and freeing twice is safe
    tiny_writer_free(&writer);
    tiny_writer_free(&writer);

    r |= assertEquals(tiny_writer_init(&writer, 1024), TINYIMG_OK);
    r |= assertNotNull(writer.data);
    r |= assertGreaterThan((double) writer.capacity, 1023.0);
    r |= assertEquals((long) writer.size, 0L);
    tiny_writer_free(&writer);

    // the first failure latches, so an encoder can emit a whole file and check
    // once at the end
    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);
    r |= assertEquals(tiny_writer_write(&writer, 0, 4), TINYIMG_ERR_NULL);
    r |= assertEquals(writer.error, TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_writer_u8(&writer, 1), TINYIMG_ERR_NULL);
    r |= assertEquals((long) writer.size, 0L);
    r |= assertNull(tiny_writer_detach(&writer, &detached));
    tiny_writer_free(&writer);

    return r;
}
