#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

typedef struct {
    const char* name;
    TinyImageFormat format;
} FixtureFormat;

// every fixture the derived set produces, so adding a format to the generator
// without teaching the sniffer about it fails here
static const FixtureFormat fixtures[] = {
    {"sf-24.jpg", TINYIMG_FORMAT_JPEG},
    {"road.jpg", TINYIMG_FORMAT_JPEG},
    {"mountains.jpg", TINYIMG_FORMAT_JPEG},
    {"mushroom.jpg", TINYIMG_FORMAT_JPEG},
    {"budapest.jpg", TINYIMG_FORMAT_JPEG},
    {"dog.jpg", TINYIMG_FORMAT_JPEG},
    {"flower.jpg", TINYIMG_FORMAT_JPEG},
    {"man.jpg", TINYIMG_FORMAT_JPEG},
    {"smile.jpg", TINYIMG_FORMAT_JPEG},
    {"moped.jpg", TINYIMG_FORMAT_JPEG},
    {"winter_cabin.jpg", TINYIMG_FORMAT_JPEG},
    {"face_art.jpg", TINYIMG_FORMAT_JPEG},
    {"family.jpg", TINYIMG_FORMAT_JPEG},
    {"woman.jpg", TINYIMG_FORMAT_JPEG},
    {"webassembly.png", TINYIMG_FORMAT_PNG},
    {"forest.png", TINYIMG_FORMAT_PNG},
    {"winter_forest.png", TINYIMG_FORMAT_PNG},
    {"derived/base-420.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base-422.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base-444.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base-cmyk.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base-gray.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base-progressive.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/oversized.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/tiny-odd.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base.png", TINYIMG_FORMAT_PNG},
    {"derived/base-rgb8.png", TINYIMG_FORMAT_PNG},
    {"derived/base-rgba8.png", TINYIMG_FORMAT_PNG},
    {"derived/base-rgb16.png", TINYIMG_FORMAT_PNG},
    {"derived/base-gray.png", TINYIMG_FORMAT_PNG},
    {"derived/base-gray8.png", TINYIMG_FORMAT_PNG},
    {"derived/base-gray-alpha8.png", TINYIMG_FORMAT_PNG},
    {"derived/base-palette.png", TINYIMG_FORMAT_PNG},
    {"derived/base-interlaced.png", TINYIMG_FORMAT_PNG},
    {"derived/base-alpha.png", TINYIMG_FORMAT_PNG},
    {"derived/base-display-p3.png", TINYIMG_FORMAT_PNG},
    {"derived/base-adobe-rgb-1998.png", TINYIMG_FORMAT_PNG},
    {"derived/base-rec2020.png", TINYIMG_FORMAT_PNG},
    {"derived/flat.png", TINYIMG_FORMAT_PNG},
    {"derived/logo.png", TINYIMG_FORMAT_PNG},
    {"derived/single-pixel.png", TINYIMG_FORMAT_PNG},
    {"derived/subject.png", TINYIMG_FORMAT_PNG},
    {"derived/tiny-odd.png", TINYIMG_FORMAT_PNG},
    {"derived/trim.png", TINYIMG_FORMAT_PNG},
    {"derived/base.bmp", TINYIMG_FORMAT_BMP},
    {"derived/base-rle8.bmp", TINYIMG_FORMAT_BMP},
    {"derived/base.gif", TINYIMG_FORMAT_GIF},
    {"derived/base-interlaced.gif", TINYIMG_FORMAT_GIF},
    {"derived/base-uncompressed.tif", TINYIMG_FORMAT_TIFF},
    {"derived/base-lzw.tif", TINYIMG_FORMAT_TIFF},
    {"derived/base-packbits.tif", TINYIMG_FORMAT_TIFF},
    {"derived/base-deflate.tif", TINYIMG_FORMAT_TIFF},
    {"derived/base-lossy.webp", TINYIMG_FORMAT_WEBP},
    {"derived/base-lossless.webp", TINYIMG_FORMAT_WEBP},
    {"derived/base-alpha.webp", TINYIMG_FORMAT_WEBP},
    {"derived/base.avif", TINYIMG_FORMAT_AVIF},
};

int main(void) {
    int r = 0;

    int identified = 0;
    int missing = 0;

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(fixtures[i].name, &size);

        if (!bytes) {
            missing++;
            continue;
        }

        if (tiny_format_sniff(bytes, size) == fixtures[i].format) {
            identified++;
        }
        else {
            printf(
                "sniffed %s as %s, wanted %s\n", fixtures[i].name,
                tiny_format_name(tiny_format_sniff(bytes, size)),
                tiny_format_name(fixtures[i].format)
            );
        }

        free(bytes);
    }

    r |= assertEquals((long) missing, 0L);
    r |= assertEquals(
        (long) identified, (long) (sizeof(fixtures) / sizeof(fixtures[0]))
    );

    // a buffer that is not an image at all is unknown, not a corrupt something
    size_t size = 0;
    unsigned char* junk =
        readFixture("derived/malformed/not-an-image.bin", &size);
    r |= assertNotNull(junk);
    if (junk) {
        r |= assertEquals(
            (long) tiny_format_sniff(junk, size), (long) TINYIMG_FORMAT_UNKNOWN
        );
        free(junk);
    }

    // the magic bytes alone are enough to route a truncated file to the codec
    // that can explain what is wrong with it
    unsigned char* stub =
        readFixture("derived/malformed/signature-only.png", &size);
    r |= assertNotNull(stub);
    if (stub) {
        r |= assertEquals(
            (long) tiny_format_sniff(stub, size), (long) TINYIMG_FORMAT_PNG
        );
        free(stub);
    }

    r |= assertEquals(
        (long) tiny_format_sniff(0, 100), (long) TINYIMG_FORMAT_UNKNOWN
    );
    r |= assertEquals(
        (long) tiny_format_sniff((const uint8_t*) "BM", 1),
        (long) TINYIMG_FORMAT_UNKNOWN
    );

    // an ISOBMFF file whose brand is neither AVIF nor HEIF stays unknown rather
    // than being guessed at
    static const uint8_t mp4[12] = {0,   0,   0,   0x18, 'f', 't',
                                    'y', 'p', 'i', 's',  'o', 'm'};
    r |= assertEquals(
        (long) tiny_format_sniff(mp4, 12), (long) TINYIMG_FORMAT_UNKNOWN
    );

    static const uint8_t heic[12] = {0,   0,   0,   0x18, 'f', 't',
                                     'y', 'p', 'h', 'e',  'i', 'c'};
    r |= assertEquals(
        (long) tiny_format_sniff(heic, 12), (long) TINYIMG_FORMAT_HEIF
    );

    // both TIFF byte orders
    static const uint8_t little[4] = {'I', 'I', 0x2A, 0x00};
    static const uint8_t big[4] = {'M', 'M', 0x00, 0x2A};
    r |= assertEquals(
        (long) tiny_format_sniff(little, 4), (long) TINYIMG_FORMAT_TIFF
    );
    r |= assertEquals(
        (long) tiny_format_sniff(big, 4), (long) TINYIMG_FORMAT_TIFF
    );

    r |= assertStringsMatch(tiny_format_name(TINYIMG_FORMAT_WEBP), "webp");
    r |=
        assertStringsMatch(tiny_format_name(TINYIMG_FORMAT_UNKNOWN), "unknown");
    r |= assertStringsMatch(tiny_format_extension(TINYIMG_FORMAT_JPEG), ".jpg");
    r |= assertStringsMatch(tiny_format_extension(TINYIMG_FORMAT_UNKNOWN), "");

    // every format has a name and an extension, so a new one cannot be added
    // without giving it both
    int described = 1;
    for (int format = TINYIMG_FORMAT_PNG; format <= TINYIMG_FORMAT_HEIF;
         format++) {
        if (tiny_strcmp(
                tiny_format_name((TinyImageFormat) format), "unknown"
            ) == 0) {
            described = 0;
        }
        if (tiny_strlen(tiny_format_extension((TinyImageFormat) format)) == 0) {
            described = 0;
        }
    }
    r |= assertTrue(described);

    // AVIF describes its container without decoding it, which is the whole
    // point of a probe: the dimensions come back and a decode does not
    TinyImageInfo info;
    unsigned char* avif = readFixture("derived/base.avif", &size);
    r |= assertNotNull(avif);
    if (avif) {
        r |= assertEquals(tiny_image_probe(avif, size, &info), TINYIMG_OK);
        r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_AVIF);
        r |= assertEquals(info.width, 320);
        r |= assertEquals(info.height, 180);

        TinyImage image;
        r |= assertEquals(
            tiny_image_load(&image, avif, size), TINYIMG_ERR_UNSUPPORTED_CODEC
        );
        free(avif);
    }

    /*
     * A recognized format with no codec at all has to say so specifically
     * rather than reporting the file as unrecognizable, and HEIF is what is
     * left in that state: it shares AVIF's container and its brands are
     * separated by the sniffer, but no codec claims them.
     */
    r |= assertEquals(
        tiny_image_probe(heic, sizeof(heic), &info),
        TINYIMG_ERR_UNSUPPORTED_CODEC
    );
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_HEIF);

    junk = readFixture("derived/malformed/not-an-image.bin", &size);
    if (junk) {
        r |= assertEquals(
            tiny_image_probe(junk, size, &info), TINYIMG_ERR_UNKNOWN_FORMAT
        );
        free(junk);
    }

    r |= assertEquals(tiny_image_probe(0, 10, &info), TINYIMG_ERR_NULL);
    r |= assertEquals(
        tiny_image_probe((const uint8_t*) "BM", 2, 0), TINYIMG_ERR_NULL
    );

    // the registry is enumerable, which is what lets a host report support.
    // every entry has to carry a format and a sniffer, or a build could
    // register a codec nothing can route to
    uint32_t registered = tiny_codec_count();
    r |= assertGreaterThan((double) registered, 1.0);
    r |= assertNull(tiny_codec_at(registered));

    int complete = 1;
    for (uint32_t i = 0; i < registered; i++) {
        const TinyCodec* codec = tiny_codec_at(i);

        if (!codec || !codec->sniff || !codec->probe) complete = 0;
        if (codec && codec->format == TINYIMG_FORMAT_UNKNOWN) complete = 0;
        if (codec && tiny_codec_find(codec->format) != codec) complete = 0;
    }
    r |= assertTrue(complete);

    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_BMP));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_PNG));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_JPEG));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_GIF));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_TIFF));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_WEBP));
    r |= assertNotNull(tiny_codec_find(TINYIMG_FORMAT_AVIF));
    r |= assertNull(tiny_codec_find(TINYIMG_FORMAT_HEIF));
    r |= assertNull(tiny_codec_sniff(0, 10));

    // the AVIF codec is registered for probe alone, and a caller that reaches
    // for the wrong direction has to get the specific error rather than a crash
    const TinyCodec* container = tiny_codec_find(TINYIMG_FORMAT_AVIF);
    r |= assertNull((const void*) container->decode);
    r |= assertNull((const void*) container->encode);

    return r;
}
