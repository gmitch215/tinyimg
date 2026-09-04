/**
 * @file tiff.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Tagged Image File Format codec, baseline.
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
 * @brief The TIFF codec.
 *
 * Decodes the baseline: either byte order, strip organized, chunky pixels at
 * eight bits per sample, grayscale in either polarity, palette, RGB and RGB
 * with an alpha channel, uncompressed or through PackBits, LZW or Deflate, with
 * or without the horizontal differencing predictor. LZW comes from the unit the
 * GIF codec shares and Deflate from the one the PNG codec shares, so this file
 * carries only the container.
 *
 * `probe` reports the number of directories the file chains, so a multi-page
 * document is identifiable; only the first page is decoded.
 *
 * A region decode reads only the strips it intersects, which is what strips are
 * for: a strip is independently compressed, so unlike a PNG row or a GIF code
 * stream there is nothing to scan through to reach one.
 *
 * Tiled files, planar separation, bit depths other than eight, floating point
 * samples and JPEG-in-TIFF are reported as an unsupported variant rather than
 * as corrupt. They are all legal and none of them is baseline.
 *
 * Encoding writes one little endian, strip organized, Deflate compressed page
 * with the horizontal predictor, which is the combination that compresses best
 * across the fixtures and is read by everything that reads TIFF at all.
 */
extern const TinyCodec tiny_codec_tiff;

#ifdef __cplusplus
}
#endif
