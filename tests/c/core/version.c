#include "../test.h"
#include "tinyimg/codec/codec.h"

/**
 * The module's own identity and feature reporting.
 *
 * The vitest lanes check these through the wasm module, which left them at zero
 * percent here; a host reads them to decide whether to offer a codec at all, so
 * a build that reports the wrong thing offers the wrong surface.
 */
int main(void) {
    int r = 0;

    r |= assertEquals((long) tiny_version(), (long) TINYIMG_VERSION);
    r |= assertEquals((long) tiny_version(), 1L << 16);
    r |= assertEquals((long) tiny_abi_version(), (long) TINYIMG_ABI_VERSION);

    // tiny_init has to be safe to call twice, and safe not to call at all:
    // every entry point that needs it calls it itself
    tiny_init();
    tiny_init();

    uint32_t features = tiny_features();

    // a feature bit has to agree with whether the codec is actually reachable,
    // or a host offers a format this build cannot handle
    r |= assertEquals(
        (long) ((features & TINYIMG_FEATURE_PNG) != 0),
        (long) (tiny_codec_find(TINYIMG_FORMAT_PNG) != 0)
    );
    r |= assertEquals(
        (long) ((features & TINYIMG_FEATURE_BMP) != 0),
        (long) (tiny_codec_find(TINYIMG_FORMAT_BMP) != 0)
    );

    // AVIF's bit means the container can be described, not decoded, so it is
    // set even with no codec
    r |= assertTrue((features & TINYIMG_FEATURE_AVIF) != 0);

    // every error code has a name, and an unknown one is named rather than read
    // past the table
    static const int codes[15] = {
        TINYIMG_OK,
        TINYIMG_ERR_NULL,
        TINYIMG_ERR_RANGE,
        TINYIMG_ERR_BOUNDS,
        TINYIMG_ERR_MEMORY,
        TINYIMG_ERR_TOO_LARGE,
        TINYIMG_ERR_UNKNOWN_FORMAT,
        TINYIMG_ERR_UNSUPPORTED_CODEC,
        TINYIMG_ERR_CORRUPT,
        TINYIMG_ERR_UNSUPPORTED_VARIANT,
        TINYIMG_ERR_BUFFER_TOO_SMALL,
        TINYIMG_ERR_BLOB_MISSING,
        TINYIMG_ERR_NO_CHANNEL,
        TINYIMG_ERR_NOT_FOUND,
        TINYIMG_ERR_PLAN,
    };

    int named = 1;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char* name = tiny_error_name(codes[i]);

        if (!name || tiny_strlen(name) == 0) named = 0;
        if (tiny_strcmp(name, "unknown") == 0) named = 0;
    }
    r |= assertTrue(named);

    r |= assertStringsMatch(tiny_error_name(TINYIMG_OK), "ok");
    r |= assertStringsMatch(tiny_error_name(-9999), "unknown");
    r |= assertStringsMatch(tiny_error_name(1), "unknown");

    return r;
}
