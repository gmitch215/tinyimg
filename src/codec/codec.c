#include "tinyimg/codec/codec.h"

#include "tinyimg/memory.h"
#include "tinyimg/work.h"

#if !defined(TINYIMG_NO_AVIF)
    #include "tinyimg/codec/avif.h"
#endif
#if !defined(TINYIMG_NO_BMP)
    #include "tinyimg/codec/bmp.h"
#endif
#if !defined(TINYIMG_NO_GIF)
    #include "tinyimg/codec/gif.h"
#endif
#if !defined(TINYIMG_NO_JPEG)
    #include "tinyimg/codec/jpeg.h"
#endif
#if !defined(TINYIMG_NO_PNG)
    #include "tinyimg/codec/png.h"
#endif
#if !defined(TINYIMG_NO_TIFF)
    #include "tinyimg/codec/tiff.h"
#endif
#if !defined(TINYIMG_NO_WEBP)
    #include "tinyimg/codec/webp.h"
#endif

// the trailing NULL keeps the array legal when every codec is compiled out, and
// the count skips it
static const TinyCodec* const registry[] = {
#if !defined(TINYIMG_NO_JPEG)
    &tiny_codec_jpeg,
#endif
#if !defined(TINYIMG_NO_PNG)
    &tiny_codec_png,
#endif
#if !defined(TINYIMG_NO_WEBP)
    &tiny_codec_webp,
#endif
#if !defined(TINYIMG_NO_GIF)
    &tiny_codec_gif,
#endif
#if !defined(TINYIMG_NO_TIFF)
    &tiny_codec_tiff,
#endif
#if !defined(TINYIMG_NO_BMP)
    &tiny_codec_bmp,
#endif
// last, because it only answers probe and every other codec should get the
// chance to claim a buffer first
#if !defined(TINYIMG_NO_AVIF)
    &tiny_codec_avif,
#endif
    0
};

#define REGISTRY_COUNT ((uint32_t) (sizeof(registry) / sizeof(registry[0]) - 1))

uint32_t tiny_codec_count(void) {
    return REGISTRY_COUNT;
}

const TinyCodec* tiny_codec_at(uint32_t index) {
    if (index >= REGISTRY_COUNT) return 0;
    return registry[index];
}

const TinyCodec* tiny_codec_find(TinyImageFormat format) {
    for (uint32_t i = 0; i < REGISTRY_COUNT; i++) {
        if (registry[i]->format == format) return registry[i];
    }
    return 0;
}

const TinyCodec* tiny_codec_sniff(const uint8_t* buffer, size_t size) {
    if (!buffer) return 0;

    for (uint32_t i = 0; i < REGISTRY_COUNT; i++) {
        if (registry[i]->sniff && registry[i]->sniff(buffer, size)) {
            return registry[i];
        }
    }
    return 0;
}

int tiny_decode_resolve(
    const TinyDecodeOpts* opts, uint32_t width, uint32_t height,
    TinyDecodeOpts* out, uint32_t* out_width, uint32_t* out_height
) {
    if (!out || !out_width || !out_height) return TINYIMG_ERR_NULL;
    if (width == 0 || height == 0) return TINYIMG_ERR_CORRUPT;

    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = width;
    uint32_t h = height;
    uint8_t den = 1;
    uint8_t channels = 0;

    if (opts) {
        x = opts->x;
        y = opts->y;
        w = opts->width ? opts->width : width;
        h = opts->height ? opts->height : height;
        den = opts->scale_den;
        channels = opts->channels;
    }

    if (x >= width || y >= height) return TINYIMG_ERR_BOUNDS;
    if (channels > 4) return TINYIMG_ERR_RANGE;

    if (w > width - x) w = width - x;
    if (h > height - y) h = height - y;

    if (den != 1 && den != 2 && den != 4 && den != 8) den = 1;

    uint32_t resolved_width = (w + den - 1u) / den;
    uint32_t resolved_height = (h + den - 1u) / den;

    if ((uint64_t) resolved_width * resolved_height > TINYIMG_MAX_PIXELS) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    out->x = x;
    out->y = y;
    out->width = w;
    out->height = h;
    out->scale_den = den;
    out->channels = channels;

    *out_width = resolved_width;
    *out_height = resolved_height;

    // every codec resolves before it decodes, so this is the one place both
    // numbers are known and the ratio between them is the reduction's claim
    tiny_work_add(TINYIMG_WORK_SOURCE_SAMPLES, width * height);
    tiny_work_add(
        TINYIMG_WORK_DECODED_SAMPLES, resolved_width * resolved_height
    );

    return TINYIMG_OK;
}

void tiny_pixel_convert(
    uint8_t* dest, uint8_t dest_channels, const uint8_t* src,
    uint8_t src_channels
) {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a = 255;

    switch (src_channels) {
        case 1: r = g = b = src[0]; break;
        case 2:
            r = g = b = src[0];
            a = src[1];
            break;
        case 3:
            r = src[0];
            g = src[1];
            b = src[2];
            break;
        default:
            r = src[0];
            g = src[1];
            b = src[2];
            a = src[3];
            break;
    }

    switch (dest_channels) {
        case 1:
            // the weights sum to exactly 65536, so a gray source round trips
            dest[0] = tiny_luma(r, g, b);
            break;
        case 2:
            dest[0] = tiny_luma(r, g, b);
            dest[1] = a;
            break;
        case 3:
            dest[0] = r;
            dest[1] = g;
            dest[2] = b;
            break;
        default:
            dest[0] = r;
            dest[1] = g;
            dest[2] = b;
            dest[3] = a;
            break;
    }
}
