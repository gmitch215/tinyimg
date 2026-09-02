/**
 * @file tinyimg.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Umbrella header, version and error codes.
 * @version 1.0.0
 * @date 2026-08-31
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma region version

/**
 * @brief Major version of the library.
 */
#define TINYIMG_VERSION_MAJOR 1

/**
 * @brief Minor version of the library.
 */
#define TINYIMG_VERSION_MINOR 0

/**
 * @brief Patch version of the library.
 */
#define TINYIMG_VERSION_PATCH 0

/**
 * @brief The version packed into a single integer as
 * `(major << 16) | (minor << 8) | patch`.
 */
#define TINYIMG_VERSION                                                        \
    ((TINYIMG_VERSION_MAJOR << 16) | (TINYIMG_VERSION_MINOR << 8) |            \
     TINYIMG_VERSION_PATCH)

/**
 * @brief Version of the contract between the wasm module and its host wrapper.
 *
 * Bumped whenever an exported signature, a struct layout or an error code
 * changes. The TypeScript loader refuses a module whose ABI it does not know,
 * which turns a silent misread of a struct into a startup failure.
 */
#define TINYIMG_ABI_VERSION 1

/**
 * @brief Marks a function as exported from the wasm module.
 *
 * Exporting by attribute rather than by linker flag means `--gc-sections` keeps
 * exactly the public surface and its dependencies, and drops everything else.
 */
#if defined(__wasm__)
    #define TINYIMG_EXPORT(name) __attribute__((export_name(name)))
#else
    #define TINYIMG_EXPORT(name)
#endif

/**
 * @brief Retrieves the packed library version.
 *
 * @return uint32_t The value of TINYIMG_VERSION the module was built with.
 */
uint32_t tiny_version(void);

/**
 * @brief Retrieves the ABI version of the compiled module.
 *
 * @return uint32_t The value of TINYIMG_ABI_VERSION the module was built with.
 */
uint32_t tiny_abi_version(void);

/**
 * @brief Reports which optional features were compiled in.
 *
 * The host wrapper reads this to decide whether to offer a codec at all, rather
 * than calling it and handling a failure.
 *
 * @return uint32_t A bitwise OR of TinyImageFeature values.
 */
uint32_t tiny_features(void);

/**
 * @brief Optional features a build may or may not contain.
 *
 * Every codec can be compiled out, so a caller that only ever handles PNG can
 * link a module with nothing else in it.
 */
enum TinyImageFeature
{
    TINYIMG_FEATURE_SIMD = 1 << 0,
    TINYIMG_FEATURE_PNG = 1 << 1,
    TINYIMG_FEATURE_JPEG = 1 << 2,
    TINYIMG_FEATURE_BMP = 1 << 3,
    TINYIMG_FEATURE_GIF = 1 << 4,
    TINYIMG_FEATURE_TIFF = 1 << 5,
    TINYIMG_FEATURE_WEBP = 1 << 6,
    TINYIMG_FEATURE_AVIF = 1 << 7,
    TINYIMG_FEATURE_TEXT = 1 << 8,
    TINYIMG_FEATURE_DETECT = 1 << 9,
    TINYIMG_FEATURE_ICC = 1 << 10,
};

#pragma endregion

#pragma region error codes

/**
 * @brief Result of a tinyimg call.
 *
 * Every function that can fail returns one of these. Zero is success and every
 * failure is negative, so `if (result < 0)` is the only check a caller needs.
 */
enum TinyImageError
{
    /**
     * @brief The call succeeded.
     */
    TINYIMG_OK = 0,
    /**
     * @brief A required pointer argument was NULL.
     */
    TINYIMG_ERR_NULL = -1,
    /**
     * @brief An argument was outside its documented range.
     */
    TINYIMG_ERR_RANGE = -2,
    /**
     * @brief Coordinates or a rectangle fell outside the image.
     */
    TINYIMG_ERR_BOUNDS = -3,
    /**
     * @brief The allocator could not satisfy a request.
     */
    TINYIMG_ERR_MEMORY = -4,
    /**
     * @brief The image exceeds TINYIMG_MAX_PIXELS.
     *
     * Distinct from TINYIMG_ERR_MEMORY because the remedy is different: a
     * caller sees this and reaches for tiny_image_load_scaled instead of
     * asking for more memory.
     */
    TINYIMG_ERR_TOO_LARGE = -5,
    /**
     * @brief The buffer did not match any format tinyimg recognises.
     */
    TINYIMG_ERR_UNKNOWN_FORMAT = -6,
    /**
     * @brief The format was recognised but this build cannot decode or encode
     * it. AVIF and HEIF always report this.
     */
    TINYIMG_ERR_UNSUPPORTED_CODEC = -7,
    /**
     * @brief The bitstream is malformed, truncated or internally inconsistent.
     */
    TINYIMG_ERR_CORRUPT = -8,
    /**
     * @brief The format is recognised and supported, but this particular
     * variant of it is not (a 12-bit JPEG, an arithmetic-coded scan, a
     * LUT-based ICC profile).
     */
    TINYIMG_ERR_UNSUPPORTED_VARIANT = -9,
    /**
     * @brief The output buffer was too small. The required size is reported
     * through the call's size out-parameter.
     */
    TINYIMG_ERR_BUFFER_TOO_SMALL = -10,
    /**
     * @brief The operation needs a blob that has not been loaded, such as a
     * font or a detection cascade.
     */
    TINYIMG_ERR_BLOB_MISSING = -11,
    /**
     * @brief The image has no channel the operation needs, such as an alpha
     * channel for an opacity change.
     */
    TINYIMG_ERR_NO_CHANNEL = -12,
    /**
     * @brief The requested key or metadata entry does not exist.
     */
    TINYIMG_ERR_NOT_FOUND = -13,
    /**
     * @brief The transform plan is full or holds an operation that cannot be
     * combined with the others in it.
     */
    TINYIMG_ERR_PLAN = -14,
};

/**
 * @brief Retrieves a short, stable name for an error code.
 *
 * Intended for logs and for the message the host wrapper throws; the text is
 * not localised and is not meant to be shown to an end user.
 *
 * @param error The error code to describe.
 * @return const char* A NUL-terminated ASCII name, or "unknown" if the code is
 * not one of TinyImageError.
 */
const char* tiny_error_name(int error);

#pragma endregion

#ifdef __cplusplus
}
#endif
