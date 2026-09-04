/**
 * @file png.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Portable Network Graphics codec.
 * @version 1.0.0
 * @date 2026-09-02
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include "tinyimg/codec/codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The PNG codec.
 *
 * Decodes every color type at every bit depth the format defines: grayscale
 * and palette at 1, 2, 4 and 8 bits, truecolor at 8 and 16, either of the
 * latter with an alpha channel, and palette or grayscale transparency through
 * tRNS. Sixteen bit samples are reduced to eight by taking the high byte, which
 * is what every viewer does. Encodes 8 bit grayscale, grayscale with alpha,
 * truecolor and truecolor with alpha, choosing a row filter per row by the
 * minimum sum of absolute differences and compressing with the shared DEFLATE
 * unit.
 *
 * A non-interlaced image is unfiltered one row at a time, so a region decode
 * holds the region plus two scanlines rather than the whole image, and stops
 * reading once it is past the region's last row. An Adam7 image cannot work
 * that way: its rows arrive spread across seven passes, so the whole plane is
 * assembled first and the region taken afterwards.
 *
 * Every chunk's CRC-32 is checked. A stream whose compressed data is split
 * across many IDAT chunks is joined before inflating, which a decoder that
 * inflated each chunk on its own would read as corrupt.
 */
extern const TinyCodec tiny_codec_png;

#ifdef __cplusplus
}
#endif
