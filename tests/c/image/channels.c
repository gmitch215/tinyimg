#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

// a color whose Rec. 709 luminance is not any of its own channels, so a wrong
// weighting cannot pass by accident
#define SRC_R 200
#define SRC_G 100
#define SRC_B 50

static int fill(TinyImage* image, uint8_t channels) {
    int result = tiny_image_create(image, 4, 3, channels);
    if (result != TINYIMG_OK) return result;

    const uint8_t source[4] = {SRC_R, SRC_G, SRC_B, 128};

    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            uint8_t pixel[4];
            tiny_pixel_convert(pixel, channels, source, 4);
            tiny_image_setpixel(image, x, y, pixel);
        }
    }

    return TINYIMG_OK;
}

int main(void) {
    int r = 0;

    uint8_t luma = tiny_luma(SRC_R, SRC_G, SRC_B);

    // 0.2126 * 200 + 0.7152 * 100 + 0.0722 * 50 is 117.6
    r |= assertEquals((long) luma, 118L);

    // a gray source has to survive the weighting exactly, which only holds if
    // the fixed point weights sum to 65536
    int grayExact = 1;
    for (int v = 0; v < 256; v++) {
        if (tiny_luma((uint8_t) v, (uint8_t) v, (uint8_t) v) != (uint8_t) v) {
            grayExact = 0;
        }
    }
    r |= assertTrue(grayExact);

    TinyImage image;
    uint8_t pixel[4];

    // every widening and narrowing transition, checked on the same color
    for (uint8_t from = 1; from <= 4; from++) {
        for (uint8_t to = 1; to <= 4; to++) {
            r |= assertEquals(fill(&image, from), TINYIMG_OK);
            r |= assertEquals(
                tiny_image_convert_channels(&image, to), TINYIMG_OK
            );
            r |= assertEquals((long) image.channels, (long) to);
            r |= assertEquals(
                (long) tiny_image_getsize(&image), 4L * 3L * (long) to
            );

            // the last pixel matters as much as the first: an in place
            // narrowing that miscomputes its stride corrupts the tail
            r |= assertEquals(
                tiny_image_getpixel(&image, 3, 2, pixel), TINYIMG_OK
            );

            uint8_t source[4] = {SRC_R, SRC_G, SRC_B, 128};
            uint8_t intermediate[4];
            uint8_t wanted[4];
            tiny_pixel_convert(intermediate, from, source, 4);
            tiny_pixel_convert(wanted, to, intermediate, from);

            r |= assertBytesMatch(pixel, wanted, to);
            tiny_image_destroy(&image);
        }
    }

    // color to gray uses the weighting, and gray to color replicates
    r |= assertEquals(fill(&image, 3), TINYIMG_OK);
    r |= assertEquals(tiny_image_to_grayscale(&image), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 1L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], (long) luma);

    r |= assertEquals(tiny_image_to_rgb(&image), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 3L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], (long) luma);
    r |= assertEquals((long) pixel[1], (long) luma);
    r |= assertEquals((long) pixel[2], (long) luma);

    r |= assertEquals(tiny_image_to_rgba(&image), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 4L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[3], 255L);
    tiny_image_destroy(&image);

    // grayscaling an image with alpha keeps the alpha, since dropping it would
    // be a second undocumented change
    r |= assertEquals(fill(&image, 4), TINYIMG_OK);
    r |= assertEquals(tiny_image_to_grayscale(&image), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 2L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], (long) luma);
    r |= assertEquals((long) pixel[1], 128L);
    tiny_image_destroy(&image);

    // converting to the same count is a no-op rather than a reallocation
    r |= assertEquals(fill(&image, 3), TINYIMG_OK);
    uint8_t* before = image.data;
    r |= assertEquals(tiny_image_convert_channels(&image, 3), TINYIMG_OK);
    r |= assertTrue(image.data == before);

    r |=
        assertEquals(tiny_image_convert_channels(&image, 0), TINYIMG_ERR_RANGE);
    r |=
        assertEquals(tiny_image_convert_channels(&image, 5), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_image_convert_channels(0, 3), TINYIMG_ERR_NULL);
    tiny_image_destroy(&image);

    // enabling transparency adds an opaque channel, because the pixels that
    // were there were visible
    r |= assertEquals(fill(&image, 3), TINYIMG_OK);
    r |= assertEquals(tiny_image_istransparent(&image), 0);
    r |= assertEquals(tiny_image_set_transparent(&image, 1), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 4L);
    r |= assertEquals(tiny_image_istransparent(&image), 1);

    int opaque = 1;
    for (uint32_t i = 0; i < 12; i++) {
        if (image.data[i * 4 + 3] != 255) opaque = 0;
    }
    r |= assertTrue(opaque);

    // already transparent is a no-op
    r |= assertEquals(tiny_image_set_transparent(&image, 1), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 4L);

    // an opaque pixel survives flattening untouched
    r |= assertEquals(tiny_image_set_transparent(&image, 0), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 3L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], (long) SRC_R);
    r |= assertEquals((long) pixel[1], (long) SRC_G);
    r |= assertEquals((long) pixel[2], (long) SRC_B);
    r |= assertEquals(tiny_image_set_transparent(&image, 0), TINYIMG_OK);
    tiny_image_destroy(&image);

    // flattening blends onto white, which is what a browser does with an image
    // dropped into an opaque container
    r |= assertEquals(tiny_image_create(&image, 2, 1, 4), TINYIMG_OK);

    const uint8_t clear[4] = {10, 20, 30, 0};
    const uint8_t half[4] = {0, 0, 0, 128};
    tiny_image_setpixel(&image, 0, 0, clear);
    tiny_image_setpixel(&image, 1, 0, half);

    r |= assertEquals(tiny_image_set_transparent(&image, 0), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 3L);

    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], 255L);
    r |= assertEquals((long) pixel[1], 255L);
    r |= assertEquals((long) pixel[2], 255L);

    // 0 * 128/255 + 255 * 127/255 rounds to 127
    r |= assertEquals(tiny_image_getpixel(&image, 1, 0, pixel), TINYIMG_OK);
    r |= assertEquals((long) pixel[0], 127L);
    tiny_image_destroy(&image);

    // asking for JPEG drops the alpha it cannot carry, and nothing else does
    r |= assertEquals(fill(&image, 4), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_convert(&image, TINYIMG_FORMAT_JPEG), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 3L);
    r |= assertEquals((long) image.format, (long) TINYIMG_FORMAT_JPEG);
    tiny_image_destroy(&image);

    r |= assertEquals(fill(&image, 4), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_convert(&image, TINYIMG_FORMAT_PNG), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 4L);
    tiny_image_destroy(&image);

    r |= assertEquals(tiny_image_set_transparent(0, 1), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_istransparent(0), TINYIMG_ERR_NULL);

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    return r;
}
