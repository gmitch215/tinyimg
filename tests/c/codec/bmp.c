#include "tinyimg/codec/bmp.h"
#include "../test.h"
#include "tinyimg/memory.h"

// the target image every synthetic case below encodes, top-down and in RGB
static const uint8_t target[6][3] = {
    {255, 0, 0},     {0, 255, 0}, {0, 0, 255},
    {255, 255, 255}, {0, 0, 0},   {128, 128, 128},
};

// the file header plus the first 40 bytes of the DIB header, which every layout
// starts with; the caller appends masks, palette and pixels
static void bmpHeader(
    TinyWriter* writer, uint32_t width, int32_t height, uint16_t bpp,
    uint32_t compression, uint32_t header_size, uint32_t palette_count,
    uint32_t pixel_offset, uint32_t pixel_bytes
) {
    tiny_writer_u8(writer, 'B');
    tiny_writer_u8(writer, 'M');
    tiny_writer_le32(writer, pixel_offset + pixel_bytes);
    tiny_writer_le16(writer, 0);
    tiny_writer_le16(writer, 0);
    tiny_writer_le32(writer, pixel_offset);

    tiny_writer_le32(writer, header_size);
    tiny_writer_le32(writer, width);
    tiny_writer_le32(writer, (uint32_t) height);
    tiny_writer_le16(writer, 1);
    tiny_writer_le16(writer, bpp);
    tiny_writer_le32(writer, compression);
    tiny_writer_le32(writer, pixel_bytes);
    tiny_writer_le32(writer, 0);
    tiny_writer_le32(writer, 0);
    tiny_writer_le32(writer, palette_count);
    tiny_writer_le32(writer, 0);
}

static void writePalette(TinyWriter* writer, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        tiny_writer_u8(writer, target[i][2]);
        tiny_writer_u8(writer, target[i][1]);
        tiny_writer_u8(writer, target[i][0]);
        tiny_writer_u8(writer, 0);
    }
}

// checks a decoded 3x2 image against `target`, so every depth is held to the
// same pixels
static int matchesTarget(const TinyImage* image, int expect_alpha) {
    if (image->width != 3 || image->height != 2) return 0;

    for (uint32_t y = 0; y < 2; y++) {
        for (uint32_t x = 0; x < 3; x++) {
            uint8_t pixel[4];
            if (tiny_image_getpixel(image, x, y, pixel) != TINYIMG_OK) return 0;

            const uint8_t* wanted = target[y * 3 + x];
            if (pixel[0] != wanted[0] || pixel[1] != wanted[1] ||
                pixel[2] != wanted[2]) {
                printf(
                    "pixel %u,%u had %u,%u,%u wanted %u,%u,%u\n", x, y,
                    pixel[0], pixel[1], pixel[2], wanted[0], wanted[1],
                    wanted[2]
                );
                return 0;
            }

            if (expect_alpha && pixel[3] != 255) return 0;
        }
    }

    return 1;
}

static int decode24(TinyImage* image) {
    TinyWriter writer;
    tiny_writer_init(&writer, 0);

    // 3 pixels of 3 bytes pads to a 12 byte stride
    bmpHeader(&writer, 3, 2, 24, 0, 40, 0, 54, 24);

    for (int row = 1; row >= 0; row--) {
        for (int x = 0; x < 3; x++) {
            const uint8_t* pixel = target[row * 3 + x];
            tiny_writer_u8(&writer, pixel[2]);
            tiny_writer_u8(&writer, pixel[1]);
            tiny_writer_u8(&writer, pixel[0]);
        }
        tiny_writer_fill(&writer, 0, 3);
    }

    int result = tiny_image_load(image, writer.data, writer.size);
    tiny_writer_free(&writer);
    return result;
}

int main(void) {
    int r = 0;

    TinyImage image;
    TinyWriter writer;

    // #region synthetic depths

    r |= assertEquals(decode24(&image), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 3L);
    r |= assertEquals((long) image.format, (long) TINYIMG_FORMAT_BMP);
    r |= assertTrue(matchesTarget(&image, 0));
    tiny_image_destroy(&image);

    // a negative height is a top-down file, so the rows come out the same way
    // round
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, -2, 24, 0, 40, 0, 54, 24);
    for (int row = 0; row < 2; row++) {
        for (int x = 0; x < 3; x++) {
            const uint8_t* pixel = target[row * 3 + x];
            tiny_writer_u8(&writer, pixel[2]);
            tiny_writer_u8(&writer, pixel[1]);
            tiny_writer_u8(&writer, pixel[0]);
        }
        tiny_writer_fill(&writer, 0, 3);
    }
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertTrue(matchesTarget(&image, 0));
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // 8 bit palette
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 8, 0, 40, 6, 54 + 24, 8);
    writePalette(&writer, 6);
    for (int row = 1; row >= 0; row--) {
        for (int x = 0; x < 3; x++) {
            tiny_writer_u8(&writer, (uint8_t) (row * 3 + x));
        }
        tiny_writer_fill(&writer, 0, 1);
    }
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertTrue(matchesTarget(&image, 0));
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // 4 bit palette, two pixels per byte with the first in the high nibble
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 4, 0, 40, 6, 54 + 24, 8);
    writePalette(&writer, 6);
    tiny_writer_u8(&writer, 0x34);
    tiny_writer_u8(&writer, 0x50);
    tiny_writer_fill(&writer, 0, 2);
    tiny_writer_u8(&writer, 0x01);
    tiny_writer_u8(&writer, 0x20);
    tiny_writer_fill(&writer, 0, 2);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertTrue(matchesTarget(&image, 0));
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // 1 bit, most significant bit leftmost
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 1, 0, 40, 2, 54 + 8, 8);
    tiny_writer_u8(&writer, 0);
    tiny_writer_u8(&writer, 0);
    tiny_writer_u8(&writer, 0);
    tiny_writer_u8(&writer, 0);
    tiny_writer_u8(&writer, 255);
    tiny_writer_u8(&writer, 255);
    tiny_writer_u8(&writer, 255);
    tiny_writer_u8(&writer, 0);
    tiny_writer_u8(&writer, 0xA0);
    tiny_writer_fill(&writer, 0, 3);
    tiny_writer_u8(&writer, 0x60);
    tiny_writer_fill(&writer, 0, 3);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );

    uint8_t bit[4];
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, bit), TINYIMG_OK);
    r |= assertEquals((long) bit[0], 0L);
    r |= assertEquals(tiny_image_getpixel(&image, 1, 0, bit), TINYIMG_OK);
    r |= assertEquals((long) bit[0], 255L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 1, bit), TINYIMG_OK);
    r |= assertEquals((long) bit[0], 255L);
    r |= assertEquals(tiny_image_getpixel(&image, 1, 1, bit), TINYIMG_OK);
    r |= assertEquals((long) bit[0], 0L);
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // 16 bit 555, where the five bit channels have to be scaled rather than
    // shifted
    static const uint16_t packed[6] = {0x7C00, 0x03E0, 0x001F,
                                       0x7FFF, 0x0000, 0x4210};
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 16, 0, 40, 0, 54, 16);
    for (int row = 1; row >= 0; row--) {
        for (int x = 0; x < 3; x++) {
            tiny_writer_le16(&writer, packed[row * 3 + x]);
        }
        tiny_writer_fill(&writer, 0, 2);
    }
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );

    uint8_t five[4];
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, five), TINYIMG_OK);
    r |= assertEquals((long) five[0], 255L);
    r |= assertEquals((long) five[1], 0L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 1, five), TINYIMG_OK);
    r |= assertEquals((long) five[0], 255L);
    r |= assertEquals((long) five[2], 255L);

    // 0x4210 is 16,16,16, which scales to 132 rather than the 128 a shift by
    // three would give
    r |= assertEquals(tiny_image_getpixel(&image, 2, 1, five), TINYIMG_OK);
    r |= assertEquals((long) five[0], 132L);
    r |= assertEquals((long) five[1], 132L);
    r |= assertEquals((long) five[2], 132L);
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // 32 bit BI_RGB behind a 40 byte header: the fourth byte is padding, so
    // reading it as alpha would turn the whole image transparent
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 32, 0, 40, 0, 54, 24);
    for (int row = 1; row >= 0; row--) {
        for (int x = 0; x < 3; x++) {
            const uint8_t* pixel = target[row * 3 + x];
            tiny_writer_u8(&writer, pixel[2]);
            tiny_writer_u8(&writer, pixel[1]);
            tiny_writer_u8(&writer, pixel[0]);
            tiny_writer_u8(&writer, 0);
        }
    }
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 3L);
    r |= assertTrue(matchesTarget(&image, 0));
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // the same pixels behind a V4 header that declares an alpha mask do carry
    // alpha
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 32, 3, 108, 0, 14 + 108, 24);
    tiny_writer_le32(&writer, 0x00FF0000u);
    tiny_writer_le32(&writer, 0x0000FF00u);
    tiny_writer_le32(&writer, 0x000000FFu);
    tiny_writer_le32(&writer, 0xFF000000u);
    tiny_writer_fill(&writer, 0, 108 - 56);
    for (int row = 1; row >= 0; row--) {
        for (int x = 0; x < 3; x++) {
            const uint8_t* pixel = target[row * 3 + x];
            tiny_writer_u8(&writer, pixel[2]);
            tiny_writer_u8(&writer, pixel[1]);
            tiny_writer_u8(&writer, pixel[0]);
            tiny_writer_u8(&writer, (uint8_t) (row == 0 ? 255 : 64));
        }
    }
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 4L);

    uint8_t alpha[4];
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, alpha), TINYIMG_OK);
    r |= assertEquals((long) alpha[3], 255L);
    r |= assertEquals(tiny_image_getpixel(&image, 0, 1, alpha), TINYIMG_OK);
    r |= assertEquals((long) alpha[3], 64L);
    tiny_image_destroy(&image);
    tiny_writer_free(&writer);

    // #endregion

    // #region rejected and malformed

    // BI_RLE4 and the JPEG and PNG in BMP wrappers are named as unsupported
    // rather than reported as corrupt
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 4, 2, 40, 6, 54 + 24, 8);
    writePalette(&writer, 6);
    tiny_writer_fill(&writer, 0, 8);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    tiny_writer_free(&writer);

    // the 12 byte OS/2 header keeps its dimensions elsewhere
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 24, 0, 12, 0, 26, 24);
    tiny_writer_fill(&writer, 0, 24);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    tiny_writer_free(&writer);

    // a depth no BMP defines
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 12, 0, 40, 0, 54, 24);
    tiny_writer_fill(&writer, 0, 24);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );
    tiny_writer_free(&writer);

    // dimensions the header cannot mean
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 0, 2, 24, 0, 40, 0, 54, 24);
    tiny_writer_fill(&writer, 0, 24);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&writer);

    // past the pixel budget, which is a different remedy from running out of
    // memory
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 5000, 4000, 24, 0, 40, 0, 54, 1024);
    tiny_writer_fill(&writer, 0, 1024);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_ERR_TOO_LARGE
    );
    tiny_writer_free(&writer);

    // a file that claims more pixel data than it carries
    tiny_writer_init(&writer, 0);
    bmpHeader(&writer, 3, 2, 24, 0, 40, 0, 54, 24);
    tiny_writer_fill(&writer, 0, 8);
    r |= assertEquals(
        tiny_image_load(&image, writer.data, writer.size), TINYIMG_ERR_CORRUPT
    );
    tiny_writer_free(&writer);

    // the signature alone
    static const uint8_t stub[2] = {'B', 'M'};
    r |= assertEquals(tiny_image_load(&image, stub, 2), TINYIMG_ERR_CORRUPT);

    r |= assertEquals(tiny_image_load(&image, 0, 100), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_load(0, stub, 2), TINYIMG_ERR_NULL);

    // #endregion

    // #region fixtures

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.bmp", &size);
    r |= assertNotNull(bytes);
    if (!bytes) return 1;

    TinyImageInfo info;
    r |= assertEquals(tiny_image_probe(bytes, size, &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.progressive, 0L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_BMP);

    TinyImage full;
    r |= assertEquals(tiny_image_load(&full, bytes, size), TINYIMG_OK);
    r |= assertEquals((long) full.width, 320L);
    r |= assertEquals((long) full.height, 180L);
    r |= assertEquals((long) full.channels, 3L);

    // a region has to equal the same rectangle of a full decode, or the planner
    // cannot substitute one for the other
    TinyImage region;
    r |= assertEquals(
        tiny_image_load_region(&region, bytes, size, 37, 21, 64, 48), TINYIMG_OK
    );
    r |= assertEquals((long) region.width, 64L);
    r |= assertEquals((long) region.height, 48L);

    int regionMatches = 1;
    for (uint32_t y = 0; y < 48; y++) {
        const uint8_t* wanted = full.data + ((size_t) (y + 21) * 320 + 37) * 3;
        const uint8_t* got = region.data + (size_t) y * 64 * 3;

        if (tiny_memcmp(wanted, got, 64 * 3) != 0) regionMatches = 0;
    }
    r |= assertTrue(regionMatches);
    tiny_image_destroy(&region);

    // a region running off the edge is clamped rather than refused
    r |= assertEquals(
        tiny_image_load_region(&region, bytes, size, 300, 170, 100, 100),
        TINYIMG_OK
    );
    r |= assertEquals((long) region.width, 20L);
    r |= assertEquals((long) region.height, 10L);
    tiny_image_destroy(&region);

    // a region starting outside the image is an error, since clamping it would
    // silently return the wrong pixels
    r |= assertEquals(
        tiny_image_load_region(&region, bytes, size, 320, 0, 10, 10),
        TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_image_load_region(&region, bytes, size, 0, 180, 10, 10),
        TINYIMG_ERR_BOUNDS
    );

    // a scaled decode has to be the box average of a full one, which is the
    // property that lets statistics run on the reduced image
    for (uint8_t den = 2; den <= 8; den *= 2) {
        TinyDecodeOpts opts = {0, 0, 0, 0, den, 0};
        TinyImage scaled;

        r |= assertEquals(
            tiny_image_decode(&scaled, bytes, size, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) scaled.width, (long) ((320 + den - 1) / den));
        r |= assertEquals((long) scaled.height, (long) ((180 + den - 1) / den));

        int averaged = 1;
        for (uint32_t y = 0; y < scaled.height; y++) {
            for (uint32_t x = 0; x < scaled.width; x++) {
                uint32_t sums[3] = {0, 0, 0};
                uint32_t count = 0;

                for (uint32_t sy = y * den; sy < (y + 1u) * den && sy < 180;
                     sy++) {
                    for (uint32_t sx = x * den; sx < (x + 1u) * den && sx < 320;
                         sx++) {
                        const uint8_t* pixel =
                            full.data + ((size_t) sy * 320 + sx) * 3;

                        sums[0] += pixel[0];
                        sums[1] += pixel[1];
                        sums[2] += pixel[2];
                        count++;
                    }
                }

                const uint8_t* got =
                    scaled.data + ((size_t) y * scaled.width + x) * 3;

                for (int c = 0; c < 3; c++) {
                    if (got[c] != (uint8_t) ((sums[c] + count / 2) / count)) {
                        averaged = 0;
                    }
                }
            }
        }
        r |= assertTrue(averaged);
        tiny_image_destroy(&scaled);
    }

    // the scaled loader picks the cheapest denominator that still covers the
    // box, so what follows it is always a downscale
    TinyImage covered;
    r |= assertEquals(
        tiny_image_load_scaled(&covered, bytes, size, 100, 50), TINYIMG_OK
    );
    r |= assertEquals((long) covered.width, 160L);
    r |= assertEquals((long) covered.height, 90L);
    tiny_image_destroy(&covered);

    r |= assertEquals(
        tiny_image_load_scaled(&covered, bytes, size, 40, 20), TINYIMG_OK
    );
    r |= assertEquals((long) covered.width, 40L);
    r |= assertEquals((long) covered.height, 23L);
    tiny_image_destroy(&covered);

    // a box the source cannot cover even at full resolution decodes whole
    r |= assertEquals(
        tiny_image_load_scaled(&covered, bytes, size, 4000, 4000), TINYIMG_OK
    );
    r |= assertEquals((long) covered.width, 320L);
    tiny_image_destroy(&covered);

    // a decoder asked for a channel count produces it without a second pass
    TinyDecodeOpts grey = {0, 0, 0, 0, 1, 1};
    TinyImage mono;
    r |= assertEquals(tiny_image_decode(&mono, bytes, size, &grey), TINYIMG_OK);
    r |= assertEquals((long) mono.channels, 1L);

    int lumaMatches = 1;
    for (uint32_t i = 0; i < 320 * 180; i++) {
        const uint8_t* pixel = full.data + (size_t) i * 3;
        if (mono.data[i] != tiny_luma(pixel[0], pixel[1], pixel[2])) {
            lumaMatches = 0;
        }
    }
    r |= assertTrue(lumaMatches);
    tiny_image_destroy(&mono);

    TinyDecodeOpts tooMany = {0, 0, 0, 0, 1, 9};
    r |= assertEquals(
        tiny_image_decode(&mono, bytes, size, &tooMany), TINYIMG_ERR_RANGE
    );

    // #endregion

    // #region round trip

    // 24 bit out and back has to be lossless, since neither direction quantises
    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&full, TINYIMG_FORMAT_BMP, 0, &writer), TINYIMG_OK
    );
    r |= assertEquals(
        (long) tiny_format_sniff(writer.data, writer.size),
        (long) TINYIMG_FORMAT_BMP
    );

    TinyImage again;
    r |= assertEquals(
        tiny_image_load(&again, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertImageEquals(&again, &full);
    tiny_image_destroy(&again);
    tiny_writer_free(&writer);

    // and so does 32 bit with alpha, which is the case the V4 header exists for
    TinyImage rgba;
    r |= assertEquals(tiny_image_load(&rgba, bytes, size), TINYIMG_OK);
    r |= assertEquals(tiny_image_convert_channels(&rgba, 4), TINYIMG_OK);
    for (uint32_t i = 0; i < 320 * 180; i++) {
        rgba.data[i * 4 + 3] = (uint8_t) (i & 0xFF);
    }

    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&rgba, TINYIMG_FORMAT_BMP, 0, &writer), TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_load(&again, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertEquals((long) again.channels, 4L);
    r |= assertImageEquals(&again, &rgba);
    tiny_image_destroy(&again);
    tiny_image_destroy(&rgba);
    tiny_writer_free(&writer);

    // one channel goes out as an 8 bit greyscale palette rather than as three
    // copies of the same byte
    TinyImage grey8;
    r |= assertEquals(tiny_image_load(&grey8, bytes, size), TINYIMG_OK);
    r |= assertEquals(tiny_image_convert_channels(&grey8, 1), TINYIMG_OK);

    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&grey8, TINYIMG_FORMAT_BMP, 0, &writer), TINYIMG_OK
    );

    // 14 + 40 + 1024 palette + 320 * 180
    r |= assertEquals((long) writer.size, 1078L + 320L * 180L);

    r |= assertEquals(
        tiny_image_load(&again, writer.data, writer.size), TINYIMG_OK
    );
    r |= assertEquals((long) again.channels, 3L);

    int greyRoundTrip = 1;
    for (uint32_t i = 0; i < 320 * 180; i++) {
        if (again.data[i * 3] != grey8.data[i]) greyRoundTrip = 0;
    }
    r |= assertTrue(greyRoundTrip);
    tiny_image_destroy(&again);
    tiny_image_destroy(&grey8);
    tiny_writer_free(&writer);

    r |= assertEquals(
        tiny_image_encode(0, TINYIMG_FORMAT_BMP, 0, &writer), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_image_encode(&full, TINYIMG_FORMAT_BMP, 0, 0), TINYIMG_ERR_NULL
    );
    // a format this build has no encoder for, which is what the error is
    // about. GIF served here, then WebP; AVIF is the one left, since its codec
    // answers probe and neither direction of pixels
    r |= assertEquals(
        tiny_image_encode(&full, TINYIMG_FORMAT_AVIF, 0, &writer),
        TINYIMG_ERR_UNSUPPORTED_CODEC
    );

    // #endregion

    // #region rle8

    size_t rleSize = 0;
    unsigned char* rleBytes = readFixture("derived/base-rle8.bmp", &rleSize);
    r |= assertNotNull(rleBytes);

    if (rleBytes) {
        TinyImage rle;
        r |= assertEquals(tiny_image_load(&rle, rleBytes, rleSize), TINYIMG_OK);
        r |= assertEquals((long) rle.width, 320L);
        r |= assertEquals((long) rle.height, 180L);
        r |= assertEquals((long) rle.channels, 3L);

        // the same source image through two independent BMP encoders, so the
        // gap is the 256 colour palette and nothing else
        r |= assertPSNR(rle.data, full.data, (size_t) 320 * 180 * 3, 28.0);

        // a region of an RLE8 file expands the plane first, and still has to
        // agree with a region of the plain file
        TinyImage rleRegion;
        r |= assertEquals(
            tiny_image_load_region(
                &rleRegion, rleBytes, rleSize, 37, 21, 64, 48
            ),
            TINYIMG_OK
        );
        r |= assertEquals((long) rleRegion.width, 64L);

        int rleRegionMatches = 1;
        for (uint32_t y = 0; y < 48; y++) {
            const uint8_t* wanted =
                rle.data + ((size_t) (y + 21) * 320 + 37) * 3;
            const uint8_t* got = rleRegion.data + (size_t) y * 64 * 3;

            if (tiny_memcmp(wanted, got, 64 * 3) != 0) rleRegionMatches = 0;
        }
        r |= assertTrue(rleRegionMatches);

        tiny_image_destroy(&rleRegion);
        tiny_image_destroy(&rle);
        free(rleBytes);
    }

    // #endregion

    tiny_image_destroy(&full);
    free(bytes);

    // nothing the codec allocated, including the arena chunk the box average
    // uses, is still held
    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    return r;
}
