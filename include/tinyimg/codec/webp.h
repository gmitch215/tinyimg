/**
 * @file webp.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief WebP codec, both the lossless and the lossy bitstream.
 * @version 1.0.0
 * @date 2026-09-03
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
 * @brief The WebP codec.
 *
 * Reads the simple and the extended container: a bare `VP8 ` or `VP8L` chunk,
 * or a `VP8X` file carrying any of `ALPH`, `ICCP`, `EXIF` and an animation. An
 * animation decodes its first frame composited onto the canvas the file
 * declares, and `probe` reports the frame count, so an animation is
 * identifiable without decoding it; composing or re-timing frames is out of
 * scope, as it is for GIF.
 *
 * Lossless is complete: all four transforms, the colour cache, meta Huffman
 * groups and LZ77 backward references. Lossy decodes an intra coded frame,
 * which is every still image the format holds, with both loop filters. An
 * `ALPH` chunk is read whether it is stored raw or through the lossless coder,
 * and its three filtering methods are undone.
 *
 * Encoding writes lossless for `TinyEncodeOpts::lossless` and lossy otherwise.
 * The lossy encoder is intra only: it searches the 16x16 and 4x4 luma modes
 * with a rate weighted metric, and its quality number is mapped so that a given
 * quality lands within a few percent of `cwebp -q` on both size and PSNR.
 *
 * @note There is no scaled or region decode. Both bitstreams are one sequential
 * pass whose predictors reach backward across the whole image, so a decoder
 * cannot start in the middle of one or skip a part of it: lossless predicts
 * from the pixel above and to the left and copies from anywhere already
 * written, and lossy's intra prediction chains the same way. `TinyDecodeOpts`
 * is honoured by decoding the frame and then taking the region, which is what
 * the contract permits and what a progressive JPEG does for the same reason.
 */
extern const TinyCodec tiny_codec_webp;

/**
 * @brief Fills in the table that maps a short distance code to a nearby pixel.
 *
 * The 120 entries are derived rather than shipped, from the rule that they are
 * every offset with a row of 0 to 7 and a column of -7 to 8 naming an already
 * written pixel, ordered by squared distance, then by descending row, then by
 * descending column. That saves 120 bytes of the module and is only safe if the
 * derivation is exactly right, so this exists for the test that checks it
 * against the specification's own listing.
 *
 * @param out Receives 120 bytes, each a row in its high nibble and eight minus
 * a column in its low nibble.
 *
 * @internal Not part of the public surface; present so a derived table can be
 * asserted rather than trusted.
 */
void tiny_webp_plane_codes(uint8_t* out);

#ifdef __cplusplus
}
#endif
