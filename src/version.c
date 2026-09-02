#include "tinyimg.h"

TINYIMG_EXPORT("tiny_version")
uint32_t tiny_version(void) {
    return TINYIMG_VERSION;
}

TINYIMG_EXPORT("tiny_abi_version")
uint32_t tiny_abi_version(void) {
    return TINYIMG_ABI_VERSION;
}

TINYIMG_EXPORT("tiny_features")
uint32_t tiny_features(void) {
    uint32_t features = 0;

#if defined(__wasm_simd128__)
    features |= TINYIMG_FEATURE_SIMD;
#endif
#if !defined(TINYIMG_NO_PNG)
    features |= TINYIMG_FEATURE_PNG;
#endif
#if !defined(TINYIMG_NO_JPEG)
    features |= TINYIMG_FEATURE_JPEG;
#endif
#if !defined(TINYIMG_NO_BMP)
    features |= TINYIMG_FEATURE_BMP;
#endif
#if !defined(TINYIMG_NO_GIF)
    features |= TINYIMG_FEATURE_GIF;
#endif
#if !defined(TINYIMG_NO_TIFF)
    features |= TINYIMG_FEATURE_TIFF;
#endif
#if !defined(TINYIMG_NO_WEBP)
    features |= TINYIMG_FEATURE_WEBP;
#endif
    // avif is container-parse only; the bit means probe works, nothing more
    features |= TINYIMG_FEATURE_AVIF;
#if !defined(TINYIMG_NO_TEXT)
    features |= TINYIMG_FEATURE_TEXT;
#endif
#if !defined(TINYIMG_NO_DETECT)
    features |= TINYIMG_FEATURE_DETECT;
#endif
#if !defined(TINYIMG_NO_ICC)
    features |= TINYIMG_FEATURE_ICC;
#endif

    return features;
}

TINYIMG_EXPORT("tiny_error_name")
const char* tiny_error_name(int error) {
    switch (error) {
        case TINYIMG_OK: return "ok";
        case TINYIMG_ERR_NULL: return "null argument";
        case TINYIMG_ERR_RANGE: return "argument out of range";
        case TINYIMG_ERR_BOUNDS: return "out of bounds";
        case TINYIMG_ERR_MEMORY: return "out of memory";
        case TINYIMG_ERR_TOO_LARGE: return "image too large";
        case TINYIMG_ERR_UNKNOWN_FORMAT: return "unknown format";
        case TINYIMG_ERR_UNSUPPORTED_CODEC: return "unsupported codec";
        case TINYIMG_ERR_CORRUPT: return "corrupt data";
        case TINYIMG_ERR_UNSUPPORTED_VARIANT: return "unsupported variant";
        case TINYIMG_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case TINYIMG_ERR_BLOB_MISSING: return "blob not loaded";
        case TINYIMG_ERR_NO_CHANNEL: return "channel missing";
        case TINYIMG_ERR_NOT_FOUND: return "not found";
        case TINYIMG_ERR_PLAN: return "invalid plan";
        default: return "unknown";
    }
}
