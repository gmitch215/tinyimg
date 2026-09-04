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

/** Encodes an image and reads it straight back, which is the whole round trip.
 */
static int roundTrip(const TinyImage* source, TinyImage* back, size_t* bytes) {
    TinyWriter out;
    int result = tiny_writer_init(&out, 0);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_encode(source, TINYIMG_FORMAT_GIF, 0, &out);

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

    r |= assertEquals(probeFixture("derived/base.gif", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_GIF);
    r |= assertEquals((long) info.progressive, 0L);

    // interlacing is this format's progressive layout, and probe says so for
    // the same reason it does for Adam7: a region cannot be streamed out of it
    r |= assertEquals(
        probeFixture("derived/base-interlaced.gif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.progressive, 1L);

    // a transparent index is the only transparency the format has, and it is
    // what decides the channel count
    r |= assertEquals(
        probeFixture("derived/base-transparent.gif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.channels, 4L);
    r |= assertEquals((long) info.has_alpha, 1L);

    // an animation is identifiable without decoding it, which is the point of
    // reporting a frame count for a codec that only reads the first
    r |= assertEquals(
        probeFixture("derived/base-animation.gif", &info), TINYIMG_OK
    );
    r |= assertTrue(info.frames > 1);
    r |= assertEquals((long) info.width, 160L);
    r |= assertEquals((long) info.height, 90L);

    // the logical screen, not the frame: this file's frame is 120x90 at an
    // offset inside a 160x120 screen, and the screen is what a viewer shows
    r |= assertEquals(
        probeFixture("derived/base-offset.gif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 160L);
    r |= assertEquals((long) info.height, 120L);

    // #endregion

    // #region the same picture in two layouts

    /*
     * An interlaced file and a sequential one have to decode to the same
     * pixels.
     *
     * The format's four passes are a layout choice and nothing else, so this
     * checks the pass arithmetic against a file that needs none of it, with no
     * reference to trust.
     */
    TinyImage sequential;
    TinyImage interlaced;

    r |= assertEquals(
        decodeFixture("derived/base.gif", &sequential, 3), TINYIMG_OK
    );
    r |= assertEquals(
        decodeFixture("derived/base-interlaced.gif", &interlaced, 3), TINYIMG_OK
    );
    r |= assertImageEquals(&interlaced, &sequential);
    tiny_image_destroy(&interlaced);

    // and both are close to the picture they were quantized from
    TinyImage source;
    r |=
        assertEquals(decodeFixture("derived/base.png", &source, 3), TINYIMG_OK);

    size_t pixels = (size_t) 320 * 180 * 3;
    r |= assertPSNR(sequential.data, source.data, pixels, 28.0);

    // #endregion

    // #region the palette is not the pixels

    // two colors is the smallest palette a file can carry, and the minimum
    // code size stays two whatever the palette holds
    r |= assertEquals(
        decodeFixture("derived/base-mono.gif", &image, 3), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);

    int only_two = 1;
    uint32_t seen[2] = {0, 0};
    uint32_t distinct = 0;

    for (size_t at = 0; at < (size_t) 320 * 180; at++) {
        uint32_t color = ((uint32_t) image.data[at * 3] << 16) |
                         ((uint32_t) image.data[at * 3 + 1] << 8) |
                         image.data[at * 3 + 2];

        if (distinct > 0 && seen[0] == color) continue;
        if (distinct > 1 && seen[1] == color) continue;

        if (distinct == 2) {
            only_two = 0;
            break;
        }

        seen[distinct++] = color;
    }
    r |= assertTrue(only_two);
    tiny_image_destroy(&image);

    // a transparent index has to come out as a transparent pixel, and there
    // have to be some, or the fixture is not exercising what it was made for
    r |= assertEquals(
        decodeFixture("derived/base-transparent.gif", &image, 4), TINYIMG_OK
    );

    uint32_t clear = 0;
    for (size_t at = 0; at < (size_t) 320 * 180; at++) {
        if (image.data[at * 4 + 3] == 0) clear++;
    }
    r |= assertTrue(clear > 0);
    r |= assertTrue(clear < (uint32_t) 320 * 180);
    tiny_image_destroy(&image);

    // #endregion

    // #region region and scaled decode

    static const uint32_t boxes[3][4] = {
        {0, 0, 64, 64}, {37, 21, 77, 55}, {319, 179, 1, 1}
    };

    for (size_t b = 0; b < 3; b++) {
        TinyDecodeOpts opts = {boxes[b][0], boxes[b][1], boxes[b][2],
                               boxes[b][3], 1,           3};

        r |= assertEquals(
            decodeWith("derived/base.gif", &image, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) boxes[b][2]);
        r |= assertEquals((long) image.height, (long) boxes[b][3]);

        int matches = 1;
        for (uint32_t y = 0; y < boxes[b][3]; y++) {
            for (uint32_t x = 0; x < boxes[b][2] * 3; x++) {
                uint8_t got = image.data[(size_t) y * boxes[b][2] * 3 + x];
                uint8_t wanted =
                    sequential.data
                        [((size_t) (boxes[b][1] + y) * 320 + boxes[b][0]) * 3 +
                         x];

                if (got != wanted) matches = 0;
            }
        }
        r |= assertTrue(matches);
        tiny_image_destroy(&image);
    }

    static const uint8_t denominators[4] = {1, 2, 4, 8};
    static const uint32_t widths[4] = {320, 160, 80, 40};
    static const uint32_t heights[4] = {180, 90, 45, 23};

    for (size_t i = 0; i < 4; i++) {
        TinyDecodeOpts opts = {0, 0, 0, 0, denominators[i], 3};

        r |= assertEquals(
            decodeWith("derived/base-interlaced.gif", &image, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) widths[i]);
        r |= assertEquals((long) image.height, (long) heights[i]);
        tiny_image_destroy(&image);
    }

    // #endregion

    // #region encoding

    /*
     * A palette that already fits is kept exactly, so the round trip is
     * lossless.
     *
     * This is the case worth being exact about: a logo or a flat illustration
     * has fewer than 256 colors, and quantizing it when nothing had to be
     * discarded would lose colors for no reason.
     */
    size_t written = 0;
    TinyImage back;

    r |= assertEquals(roundTrip(&sequential, &back, &written), TINYIMG_OK);
    r |= assertImageEquals(&back, &sequential);
    r |= assertTrue(written > 0);
    tiny_image_destroy(&back);

    // the same holds through the transparent index
    TinyImage transparent;
    r |= assertEquals(
        decodeFixture("derived/base-transparent.gif", &transparent, 4),
        TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&transparent, &back, &written), TINYIMG_OK);
    r |= assertImageEquals(&back, &transparent);
    tiny_image_destroy(&back);
    tiny_image_destroy(&transparent);

    // and through two colors, where the palette is smaller than the format's
    // minimum code size allows for
    TinyImage mono;
    r |= assertEquals(
        decodeFixture("derived/base-mono.gif", &mono, 3), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&mono, &back, &written), TINYIMG_OK);
    r |= assertImageEquals(&back, &mono);
    tiny_image_destroy(&back);
    tiny_image_destroy(&mono);

    // a photograph has to be quantized, so it comes back close rather than
    // equal
    TinyImage photo;
    r |= assertEquals(decodeFixture("sf-24.jpg", &photo, 3), TINYIMG_OK);
    r |= assertEquals(roundTrip(&photo, &back, &written), TINYIMG_OK);
    r |= assertEquals((long) back.width, (long) photo.width);
    r |= assertEquals((long) back.height, (long) photo.height);
    r |= assertPSNR(
        back.data, photo.data, (size_t) photo.width * photo.height * 3, 34.0
    );

    // the file has to be smaller than the pixels it describes, or the LZW is
    // doing nothing
    r |= assertTrue(written < (size_t) photo.width * photo.height * 3);

    tiny_image_destroy(&back);
    tiny_image_destroy(&photo);

    // dimensions that are not a whole number of anything. The single pixel is
    // one color so it round trips exactly; the odd one holds 655 and so has to
    // be quantized, which is the distinction the exact path is drawn on
    static const struct {
        const char* name;
        int lossless;
    } awkward[2] = {
        {"derived/single-pixel.png", 1}, {"derived/tiny-odd.png", 0}
    };

    for (size_t i = 0; i < 2; i++) {
        TinyImage small;
        r |=
            assertEquals(decodeFixture(awkward[i].name, &small, 3), TINYIMG_OK);
        r |= assertEquals(roundTrip(&small, &back, &written), TINYIMG_OK);
        r |= assertEquals((long) back.width, (long) small.width);
        r |= assertEquals((long) back.height, (long) small.height);

        if (awkward[i].lossless) {
            r |= assertImageEquals(&back, &small);
        }
        else {
            r |= assertPSNR(
                back.data, small.data, (size_t) small.width * small.height * 3,
                30.0
            );
        }

        tiny_image_destroy(&back);
        tiny_image_destroy(&small);
    }

    // enough colors to force the quantizer, from a source with an alpha
    // channel it has to spend an entry on
    TinyImage wide;
    r |= assertEquals(decodeFixture("forest.png", &wide, 4), TINYIMG_OK);
    r |= assertEquals(roundTrip(&wide, &back, &written), TINYIMG_OK);
    r |= assertEquals((long) back.channels, 4L);

    // every pixel the source had clear stays clear, since the format can
    // express exactly that much transparency and no more
    uint32_t kept = 0;
    uint32_t clear_source = 0;

    for (size_t at = 0; at < (size_t) wide.width * wide.height; at++) {
        if (wide.data[at * 4 + 3] < 128) {
            clear_source++;
            if (back.data[at * 4 + 3] == 0) kept++;
        }
    }

    r |= assertTrue(clear_source > 0);
    r |= assertEquals((long) kept, (long) clear_source);

    tiny_image_destroy(&back);
    tiny_image_destroy(&wide);

    TinyImage empty = {0, 0, 3, 0, TINYIMG_FORMAT_UNKNOWN, 0, 0};
    TinyWriter nothing;
    r |= assertEquals(tiny_writer_init(&nothing, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&empty, TINYIMG_FORMAT_GIF, 0, &nothing),
        TINYIMG_ERR_NULL
    );
    tiny_writer_free(&nothing);

    // #endregion

    // #region malformed input

    r |= assertEquals(
        decodeFixture("derived/malformed/not-an-image.bin", &image, 3),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    // a valid signature over nothing at all
    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.gif", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        r |= assertEquals(
            tiny_image_load(&image, bytes, 6), TINYIMG_ERR_CORRUPT
        );
        r |= assertEquals(
            tiny_image_load(&image, bytes, 13), TINYIMG_ERR_CORRUPT
        );

        // a stream cut off mid frame decodes what arrived, the way every other
        // reader does, rather than refusing the whole file
        r |= assertEquals(tiny_image_load(&image, bytes, size / 2), TINYIMG_OK);
        r |= assertEquals((long) image.width, 320L);
        tiny_image_destroy(&image);

        free(bytes);
    }

    // #endregion

    // #region codec surface

    const TinyCodec* codec = tiny_codec_find(TINYIMG_FORMAT_GIF);
    r |= assertNotNull(codec);
    r |= assertTrue(codec->sniff != 0);
    r |= assertTrue(codec->probe != 0);
    r |= assertTrue(codec->decode != 0);
    r |= assertTrue(codec->encode != 0);

    // #endregion

    tiny_image_destroy(&sequential);
    tiny_image_destroy(&source);

    tiny_arena_reset();

    return r;
}
