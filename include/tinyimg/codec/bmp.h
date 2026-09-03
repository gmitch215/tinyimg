/**
 * @file bmp.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Windows bitmap codec.
 * @version 1.0.0
 * @date 2026-09-01
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
 * @brief The BMP codec.
 *
 * Decodes 1, 4 and 8 bit palettes, 16 bit packed, 24 bit BGR and 32 bit BGRA,
 * uncompressed or RLE8, in either row order. Encodes 8 bit greyscale, 24 bit
 * BGR, or 32 bit BGRA behind a V4 header when the image carries alpha.
 *
 * A 32 bit BI_RGB file with only the 40 byte header is read as opaque, because
 * the fourth byte is padding in that layout and reading it as alpha turns a
 * normal image transparent. Alpha is honoured when a mask says it is there.
 *
 * Region and scaled decode read only the rows and columns they need. RLE8 is
 * sequential, so a region of an RLE8 file expands the index plane first.
 */
extern const TinyCodec tiny_codec_bmp;

#ifdef __cplusplus
}
#endif
