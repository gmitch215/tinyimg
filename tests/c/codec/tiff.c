#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

static int decodeWith(
    const char* name, TinyImage* image, const TinyDecodeOpts* opts
) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    int result = tiny_image_decode(image, bytes, size, opts);

    free(bytes);
    return result;
}

static int decodeFixture(const char* name, TinyImage* image, uint8_t channels) {
    TinyDecodeOpts opts = {0, 0, 0, 0, 1, channels};
    return decodeWith(name, image, &opts);
}

static int probeFixture(const char* name, TinyImageInfo* info) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    int result = tiny_image_probe(bytes, size, info);

    free(bytes);
    return result;
}

static int roundTrip(
    const TinyImage* source, TinyImage* back, uint8_t quality, size_t* bytes
) {
    TinyEncodeOpts opts = {quality, 0, 0, 0};
    TinyWriter out;

    int result = tiny_writer_init(&out, 0);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_encode(source, TINYIMG_FORMAT_TIFF, &opts, &out);

    if (result == TINYIMG_OK) {
        *bytes = out.size;
        result = tiny_image_decode(back, out.data, out.size, 0);
    }

    tiny_writer_free(&out);
    return result;
}

int main(void) {
    int r = 0;

    TinyImage image;
    TinyImageInfo info;

    // #region header reading

    r |= assertEquals(
        probeFixture("derived/base-uncompressed.tif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_TIFF);
    r |= assertEquals((long) info.progressive, 0L);

    // the byte order is a header field, not a variant: the same picture either
    // way round has to read the same
    r |= assertEquals(probeFixture("derived/base-msb.tif", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);

    r |= assertEquals(probeFixture("derived/base-gray.tif", &info), TINYIMG_OK);
    r |= assertEquals((long) info.channels, 1L);

    r |=
        assertEquals(probeFixture("derived/base-alpha.tif", &info), TINYIMG_OK);
    r |= assertEquals((long) info.channels, 4L);
    r |= assertEquals((long) info.has_alpha, 1L);

    // a palette file is one sample per pixel in the file and three out of it
    r |= assertEquals(
        probeFixture("derived/base-palette.tif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.channels, 3L);

    // #endregion

    // #region one picture, every way the container can hold it

    /*
     * Nine files carrying the same pixels, and all of them have to agree.
     *
     * Four compressions, two byte orders, the horizontal predictor, a strip
     * height of seven rows against one strip for the whole image, and a
     * palette. Every one of those is a separate path through the reader, and
     * none of them is allowed to change a pixel, so agreement pins all nine at
     * once with no reference to trust.
     */
    TinyImage reference;
    r |= assertEquals(
        decodeFixture("derived/base-uncompressed.tif", &reference, 3),
        TINYIMG_OK
    );
    r |= assertEquals((long) reference.width, 320L);

    static const char* identical[6] = {
        "derived/base-packbits.tif",  "derived/base-lzw.tif",
        "derived/base-deflate.tif",   "derived/base-msb.tif",
        "derived/base-predictor.tif", "derived/base-strips.tif"
    };

    for (size_t i = 0; i < 6; i++) {
        r |= assertEquals(decodeFixture(identical[i], &image, 3), TINYIMG_OK);
        r |= assertImageEquals(&image, &reference);
        tiny_image_destroy(&image);
    }

    // the palette copy went through a 256 colour quantiser, so it only has to
    // be close
    r |= assertEquals(
        decodeFixture("derived/base-palette.tif", &image, 3), TINYIMG_OK
    );
    r |= assertPSNR(image.data, reference.data, (size_t) 320 * 180 * 3, 28.0);
    tiny_image_destroy(&image);

    // and the same picture through an entirely different codec agrees too
    TinyImage png;
    r |= assertEquals(decodeFixture("derived/base.png", &png, 3), TINYIMG_OK);
    r |= assertImageEquals(&reference, &png);
    tiny_image_destroy(&png);

    // #endregion

    // #region channels

    r |= assertEquals(
        decodeFixture("derived/base-alpha.tif", &image, 4), TINYIMG_OK
    );

    uint32_t clear = 0;
    for (size_t at = 0; at < (size_t) 320 * 180; at++) {
        if (image.data[at * 4 + 3] != 255) clear++;
    }
    r |= assertTrue(clear > 0);
    tiny_image_destroy(&image);

    // a greyscale file asked for one channel is the file's own samples
    r |= assertEquals(
        decodeFixture("derived/base-gray.tif", &image, 1), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 1L);
    tiny_image_destroy(&image);

    for (uint8_t channels = 1; channels <= 4; channels++) {
        r |= assertEquals(
            decodeFixture("derived/base-lzw.tif", &image, channels), TINYIMG_OK
        );
        r |= assertEquals((long) image.channels, (long) channels);
        tiny_image_destroy(&image);
    }

    // #endregion

    // #region region and scaled decode

    static const uint32_t boxes[3][4] = {
        {0, 0, 64, 64}, {37, 21, 77, 55}, {319, 179, 1, 1}
    };

    // the strip height matters here rather than incidentally: a seven row strip
    // means a region crosses several, and a full height strip means it crosses
    // one, and both have to give the same answer
    static const char* layouts[2] = {
        "derived/base-strips.tif", "derived/base-deflate.tif"
    };

    for (size_t l = 0; l < 2; l++) {
        for (size_t b = 0; b < 3; b++) {
            TinyDecodeOpts opts = {boxes[b][0], boxes[b][1], boxes[b][2],
                                   boxes[b][3], 1,           3};

            r |=
                assertEquals(decodeWith(layouts[l], &image, &opts), TINYIMG_OK);
            r |= assertEquals((long) image.width, (long) boxes[b][2]);
            r |= assertEquals((long) image.height, (long) boxes[b][3]);

            int matches = 1;
            for (uint32_t y = 0; y < boxes[b][3]; y++) {
                for (uint32_t x = 0; x < boxes[b][2] * 3; x++) {
                    uint8_t got = image.data[(size_t) y * boxes[b][2] * 3 + x];
                    uint8_t wanted =
                        reference.data
                            [((size_t) (boxes[b][1] + y) * 320 + boxes[b][0]) *
                                 3 +
                             x];

                    if (got != wanted) matches = 0;
                }
            }
            r |= assertTrue(matches);
            tiny_image_destroy(&image);
        }
    }

    static const uint8_t denominators[4] = {1, 2, 4, 8};
    static const uint32_t widths[4] = {320, 160, 80, 40};
    static const uint32_t heights[4] = {180, 90, 45, 23};

    for (size_t i = 0; i < 4; i++) {
        TinyDecodeOpts opts = {0, 0, 0, 0, denominators[i], 3};

        r |= assertEquals(
            decodeWith("derived/base-strips.tif", &image, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) widths[i]);
        r |= assertEquals((long) image.height, (long) heights[i]);
        tiny_image_destroy(&image);
    }

    // #endregion

    // #region encoding

    /*
     * The format is lossless, so a round trip has to be exact at every channel
     * count, and the predictor has to be undone exactly as it was applied.
     */
    for (uint8_t channels = 1; channels <= 4; channels++) {
        TinyImage source;
        TinyImage back;
        size_t written = 0;

        r |= assertEquals(
            decodeFixture("derived/base-alpha.png", &source, channels),
            TINYIMG_OK
        );
        r |= assertEquals(roundTrip(&source, &back, 80, &written), TINYIMG_OK);
        r |= assertImageEquals(&back, &source);
        r |= assertTrue(written > 0);

        tiny_image_destroy(&back);
        tiny_image_destroy(&source);
    }

    // asking for more effort has to produce a smaller file and the same pixels,
    // which is the same mapping PNG uses for the same reason
    TinyImage cheap;
    TinyImage dear;
    size_t small = 0;
    size_t large = 0;

    r |= assertEquals(roundTrip(&reference, &cheap, 80, &large), TINYIMG_OK);
    r |= assertEquals(roundTrip(&reference, &dear, 95, &small), TINYIMG_OK);
    r |= assertImageEquals(&cheap, &reference);
    r |= assertImageEquals(&dear, &reference);
    r |= assertLessThan((double) small, (double) large);

    tiny_image_destroy(&cheap);
    tiny_image_destroy(&dear);

    // an image tall enough to need several strips, so the strip tables go out
    // of line and the offsets have to be right
    TinyImage tall;
    TinyImage back;
    size_t written = 0;

    r |= assertEquals(decodeFixture("sf-24.jpg", &tall, 3), TINYIMG_OK);
    r |= assertEquals(roundTrip(&tall, &back, 80, &written), TINYIMG_OK);
    r |= assertImageEquals(&back, &tall);

    // and a region of what was just written still reads, which is the whole
    // reason for writing several strips
    TinyWriter out;
    r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&tall, TINYIMG_FORMAT_TIFF, 0, &out), TINYIMG_OK
    );

    TinyDecodeOpts window = {100, 500, 128, 96, 1, 3};
    r |= assertEquals(
        tiny_image_decode(&image, out.data, out.size, &window), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 128L);

    int matches = 1;
    for (uint32_t y = 0; y < 96; y++) {
        for (uint32_t x = 0; x < 128 * 3; x++) {
            if (image.data[(size_t) y * 128 * 3 + x] !=
                tall.data[((size_t) (500 + y) * tall.width + 100) * 3 + x]) {
                matches = 0;
            }
        }
    }
    r |= assertTrue(matches);

    tiny_image_destroy(&image);
    tiny_writer_free(&out);
    tiny_image_destroy(&back);
    tiny_image_destroy(&tall);

    // one pixel, and dimensions that are not a whole number of strips
    static const char* awkward[2] = {
        "derived/single-pixel.png", "derived/tiny-odd.png"
    };

    for (size_t i = 0; i < 2; i++) {
        TinyImage small_image;
        TinyImage returned;

        r |= assertEquals(
            decodeFixture(awkward[i], &small_image, 3), TINYIMG_OK
        );
        r |= assertEquals(
            roundTrip(&small_image, &returned, 80, &written), TINYIMG_OK
        );
        r |= assertImageEquals(&returned, &small_image);

        tiny_image_destroy(&returned);
        tiny_image_destroy(&small_image);
    }

    TinyImage empty = {0, 0, 3, 0, TINYIMG_FORMAT_UNKNOWN, 0, 0};
    TinyWriter nothing;
    r |= assertEquals(tiny_writer_init(&nothing, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&empty, TINYIMG_FORMAT_TIFF, 0, &nothing),
        TINYIMG_ERR_NULL
    );
    tiny_writer_free(&nothing);

    // #endregion

    // #region malformed input

    r |= assertEquals(
        decodeFixture("derived/malformed/not-an-image.bin", &image, 3),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base-lzw.tif", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        // the magic bytes and nothing to follow them
        r |= assertEquals(
            tiny_image_load(&image, bytes, 8), TINYIMG_ERR_CORRUPT
        );

        // a directory offset pointing outside the file
        unsigned char* broken = malloc(size);
        r |= assertNotNull(broken);

        if (broken) {
            tiny_memcpy(broken, bytes, size);

            broken[4] = 0xFF;
            broken[5] = 0xFF;
            broken[6] = 0xFF;
            broken[7] = 0x7F;

            r |= assertEquals(
                tiny_image_load(&image, broken, size), TINYIMG_ERR_CORRUPT
            );
            free(broken);
        }

        free(bytes);
    }

    // #endregion

    // #region codec surface

    const TinyCodec* codec = tiny_codec_find(TINYIMG_FORMAT_TIFF);
    r |= assertNotNull(codec);
    r |= assertTrue(codec->sniff != 0);
    r |= assertTrue(codec->probe != 0);
    r |= assertTrue(codec->decode != 0);
    r |= assertTrue(codec->encode != 0);

    bytes = readFixture("derived/base-msb.tif", &size);
    if (bytes) {
        // both byte orders sniff, which is what routes them here at all
        r |= assertTrue(codec->sniff(bytes, size));
        free(bytes);
    }

    // #endregion

    tiny_image_destroy(&reference);

    tiny_arena_reset();

    return r;
}
