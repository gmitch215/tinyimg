/**
 * @file codec.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief The contract every container codec implements, and the registry that
 * finds one.
 * @version 1.0.0
 * @date 2026-09-01
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/image.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tests whether a buffer looks like this codec's format.
 *
 * Reads magic bytes only and never rejects on anything a decoder would catch,
 * so a corrupt file of a known format still routes to the codec that can report
 * why.
 *
 * @param buffer The bytes.
 * @param size Number of bytes.
 * @return int Non-zero when the format matches.
 */
typedef int (*TinySniffFn)(const uint8_t* buffer, size_t size);

/**
 * @brief Fills in what the header says, decoding no pixels.
 *
 * @param buffer The bytes.
 * @param size Number of bytes.
 * @param info Receives the header fields.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
typedef int (*TinyProbeFn)(
    const uint8_t* buffer, size_t size, TinyImageInfo* info
);

/**
 * @brief Decodes pixels, honoring the region, scale and channel count asked
 * for.
 *
 * A decoder is free to ignore `scale_den` and decode at full resolution, in
 * which case it must still return an image of the size the options imply; the
 * planner treats scale as an optimization, not a promise.
 *
 * @param image Receives the decoded image.
 * @param buffer The bytes.
 * @param size Number of bytes.
 * @param opts Region, scale and channel count. Never NULL by the time a codec
 * sees it.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
typedef int (*TinyDecodeFn)(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* opts
);

/**
 * @brief Encodes an image into a byte sink.
 *
 * @param image The image to encode.
 * @param opts Quality and related settings. Never NULL by the time a codec sees
 * it.
 * @param writer The sink to append to.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
typedef int (*TinyEncodeFn)(
    const TinyImage* image, const TinyEncodeOpts* opts, TinyWriter* writer
);

/**
 * @brief One container format's implementation.
 *
 * A NULL decode or encode means the format is recognized but that direction is
 * not implemented, which callers see as TINYIMG_ERR_UNSUPPORTED_CODEC rather
 * than as an unknown format.
 */
typedef struct {
    /** The format this codec handles. */
    TinyImageFormat format;
    /** Tests a buffer's magic bytes. */
    TinySniffFn sniff;
    /** Reads the header without decoding pixels. */
    TinyProbeFn probe;
    /** Decodes pixels, or NULL when this build cannot read the format. */
    TinyDecodeFn decode;
    /** Encodes pixels, or NULL when this build cannot write the format. */
    TinyEncodeFn encode;
} TinyCodec;

/**
 * @brief Finds the codec for a format.
 *
 * @param format The format.
 * @return const TinyCodec* The codec, or NULL when this build has none.
 */
const TinyCodec* tiny_codec_find(TinyImageFormat format);

/**
 * @brief Finds the codec whose format a buffer's magic bytes match.
 *
 * @param buffer The bytes.
 * @param size Number of bytes.
 * @return const TinyCodec* The codec, or NULL when nothing matched.
 */
const TinyCodec* tiny_codec_sniff(const uint8_t* buffer, size_t size);

/**
 * @brief How many codecs this build registered.
 *
 * @return uint32_t The count.
 */
uint32_t tiny_codec_count(void);

/**
 * @brief The registered codec at an index, for enumerating the build's support.
 *
 * @param index Zero based index below tiny_codec_count.
 * @return const TinyCodec* The codec, or NULL when the index is out of range.
 */
const TinyCodec* tiny_codec_at(uint32_t index);

#pragma region shared helpers

/**
 * @brief Clamps decode options against an image's real dimensions.
 *
 * Every codec calls this before allocating, so the region and scale rules are
 * interpreted in one place rather than eight.
 *
 * @param opts The options as the caller gave them.
 * @param width Source width in pixels.
 * @param height Source height in pixels.
 * @param out Receives the clamped region, a scale denominator of 1, 2, 4 or 8,
 * and the channel count to produce.
 * @param out_width Receives the resulting image width.
 * @param out_height Receives the resulting image height.
 * @return int TINYIMG_OK, TINYIMG_ERR_BOUNDS when the region starts outside the
 * image, or TINYIMG_ERR_TOO_LARGE past TINYIMG_MAX_PIXELS.
 */
int tiny_decode_resolve(
    const TinyDecodeOpts* opts, uint32_t width, uint32_t height,
    TinyDecodeOpts* out, uint32_t* out_width, uint32_t* out_height
);

/**
 * @brief Writes one source pixel into a destination row, converting channels.
 *
 * Codecs decode into whatever channel count the file carries and hand each
 * pixel through here, so the requested channel count costs no second pass over
 * the image.
 *
 * @param dest Destination pixel, `dest_channels` bytes.
 * @param dest_channels Channels to write, 1 through 4.
 * @param src Source pixel, `src_channels` bytes.
 * @param src_channels Channels to read, 1 through 4.
 */
void tiny_pixel_convert(
    uint8_t* dest, uint8_t dest_channels, const uint8_t* src,
    uint8_t src_channels
);

/**
 * @brief Rec. 709 luminance of an RGB triple.
 *
 * @param r Red.
 * @param g Green.
 * @param b Blue.
 * @return uint8_t The luminance.
 */
static inline uint8_t tiny_luma(uint8_t r, uint8_t g, uint8_t b) {
    // 13933 + 46871 + 4732 == 65536, so the sum cannot drift off 255
    uint32_t sum = 13933u * r + 46871u * g + 4732u * b;
    return (uint8_t) ((sum + 32768u) >> 16);
}

#pragma endregion

#ifdef __cplusplus
}
#endif
