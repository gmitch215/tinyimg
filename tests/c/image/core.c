#include "../test.h"
#include "tinyimg/memory.h"

int main(void) {
    int r = 0;

    TinyImage image;

    r |= assertEquals(tiny_image_create(0, 4, 4, 3), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_create(&image, 0, 4, 3), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_image_create(&image, 4, 0, 3), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_image_create(&image, 4, 4, 0), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_image_create(&image, 4, 4, 5), TINYIMG_ERR_RANGE);

    // past the pixel budget the answer is TOO_LARGE rather than MEMORY, because
    // the remedy is a scaled decode and not a bigger heap
    r |= assertEquals(
        tiny_image_create(&image, 5000, 4000, 3), TINYIMG_ERR_TOO_LARGE
    );
    r |= assertEquals(tiny_image_create(&image, 4000, 4000, 3), TINYIMG_OK);
    r |= assertEquals(tiny_image_destroy(&image), TINYIMG_OK);

    // width * height must not be allowed to wrap before the check
    r |= assertEquals(
        tiny_image_create(&image, 65536, 65536, 4), TINYIMG_ERR_TOO_LARGE
    );

    r |= assertEquals(tiny_image_create(&image, 8, 6, 4), TINYIMG_OK);
    r |= assertEquals((long) image.width, 8L);
    r |= assertEquals((long) image.height, 6L);
    r |= assertEquals((long) image.channels, 4L);
    r |= assertEquals((long) image.format, (long) TINYIMG_FORMAT_UNKNOWN);
    r |= assertNotNull(image.data);
    r |= assertNull(image.meta);

    // a new image starts zeroed, so an alpha image is transparent rather than
    // holding whatever the allocator handed over
    int zeroed = 1;
    for (size_t i = 0; i < 8 * 6 * 4; i++) {
        if (image.data[i] != 0) zeroed = 0;
    }
    r |= assertTrue(zeroed);

    r |= assertEquals((long) tiny_image_getwidth(&image), 8L);
    r |= assertEquals((long) tiny_image_getheight(&image), 6L);
    r |= assertEquals((long) tiny_image_getchannels(&image), 4L);
    r |= assertEquals((long) tiny_image_getsize(&image), 8L * 6L * 4L);
    r |= assertTrue(tiny_image_getdata(&image) == image.data);
    r |= assertGreaterThan((double) tiny_image_sizeof(), 0.0);

    r |= assertEquals((long) tiny_image_getwidth(0), 0L);
    r |= assertEquals((long) tiny_image_getheight(0), 0L);
    r |= assertEquals((long) tiny_image_getchannels(0), 0L);
    r |= assertEquals((long) tiny_image_getsize(0), 0L);
    r |= assertNull(tiny_image_getdata(0));

    const uint8_t magenta[4] = {255, 0, 255, 128};
    uint8_t read[4] = {0, 0, 0, 0};

    r |= assertEquals(tiny_image_setpixel(&image, 3, 2, magenta), TINYIMG_OK);
    r |= assertEquals(tiny_image_getpixel(&image, 3, 2, read), TINYIMG_OK);
    r |= assertBytesMatch(read, magenta, 4);

    // the write landed where the row stride says it should
    size_t offset = (2 * 8 + 3) * 4;
    r |= assertBytesMatch(image.data + offset, magenta, 4);

    // the last addressable pixel is in bounds and one past it is not
    r |= assertEquals(tiny_image_setpixel(&image, 7, 5, magenta), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_setpixel(&image, 8, 5, magenta), TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_image_setpixel(&image, 7, 6, magenta), TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_image_getpixel(&image, 8, 0, read), TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_image_getpixel(&image, 0, 6, read), TINYIMG_ERR_BOUNDS
    );

    r |= assertEquals(tiny_image_getpixel(0, 0, 0, read), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, 0), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_setpixel(0, 0, 0, magenta), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_setpixel(&image, 0, 0, 0), TINYIMG_ERR_NULL);

    TinyImagePixelType type;
    r |= assertEquals(tiny_image_gettype(&image, &type), TINYIMG_OK);
    r |= assertEquals((long) type, (long) TINYIMG_PIXEL_RGBA);
    r |= assertEquals(tiny_image_gettype(0, &type), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_gettype(&image, 0), TINYIMG_ERR_NULL);

    r |= assertEquals(tiny_image_istransparent(&image), 1);

    // quality is clamped to the range a lossy encoder accepts
    r |= assertEquals(tiny_image_quality(&image, 90), TINYIMG_OK);
    r |= assertEquals((long) image.quality, 90L);
    r |= assertEquals(tiny_image_quality(&image, 101), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_image_quality(&image, -1), TINYIMG_ERR_RANGE);
    r |= assertEquals((long) image.quality, 90L);
    r |= assertEquals(tiny_image_quality(0, 50), TINYIMG_ERR_NULL);

    char extension[16];
    r |= assertEquals(
        tiny_image_getextension(&image, extension, 16),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    r |= assertEquals(
        tiny_image_convert(&image, TINYIMG_FORMAT_PNG), TINYIMG_OK
    );
    r |= assertEquals(
        (long) tiny_image_getformat(&image), (long) TINYIMG_FORMAT_PNG
    );
    r |= assertEquals(
        tiny_image_getextension(&image, extension, 16), TINYIMG_OK
    );
    r |= assertStringsMatch(extension, ".png");

    // a buffer that cannot hold the extension is reported rather than overrun
    char narrow[3] = {0, 0, 0};
    r |= assertEquals(
        tiny_image_getextension(&image, narrow, sizeof(narrow)),
        TINYIMG_ERR_BUFFER_TOO_SMALL
    );
    r |= assertStringsMatch(narrow, ".p");

    r |= assertEquals(
        tiny_image_convert(&image, TINYIMG_FORMAT_UNKNOWN), TINYIMG_ERR_RANGE
    );
    r |= assertEquals(
        tiny_image_convert(0, TINYIMG_FORMAT_PNG), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        (long) tiny_image_getformat(0), (long) TINYIMG_FORMAT_UNKNOWN
    );

    // destroying twice has to be safe, since the plan releases every image it
    // owns whether the pipeline succeeded or not
    r |= assertEquals(tiny_image_destroy(&image), TINYIMG_OK);
    r |= assertNull(image.data);
    r |= assertEquals((long) image.width, 0L);
    r |= assertEquals(tiny_image_destroy(&image), TINYIMG_OK);
    r |= assertEquals(tiny_image_destroy(0), TINYIMG_ERR_NULL);

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    // a single pixel image is legal and its data is one pixel wide
    r |= assertEquals(tiny_image_create(&image, 1, 1, 1), TINYIMG_OK);
    r |= assertEquals((long) tiny_image_getsize(&image), 1L);
    r |= assertEquals(tiny_image_gettype(&image, &type), TINYIMG_OK);
    r |= assertEquals((long) type, (long) TINYIMG_PIXEL_GRAY);
    r |= assertEquals(tiny_image_istransparent(&image), 0);
    tiny_image_destroy(&image);

    return r;
}
