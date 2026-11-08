/**
 * @file avif.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief AVIF container reader, AV1 intra decoder and encoder.
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
 * @brief The AVIF codec: probe, an AV1 intra decoder and an encoder.
 *
 * Walks the ISO base media container far enough to describe the image: the
 * brand, the primary item, and that item's spatial extents, channel count and
 * bit depth, found by following the property associations rather than by taking
 * the first property of each kind, since a file carrying alpha has two of
 * everything. An alpha channel is reported when an auxiliary item declares the
 * alpha type and an `auxl` reference ties it to the primary item.
 *
 * `decode` reads the primary item, and its alpha item when the caller asks for
 * four channels. A file the decoder would show wrongly is an error, never a
 * picture. Each of these returns TINYIMG_ERR_UNSUPPORTED_VARIANT with `probe`
 * still describing the file, and none of them leaves an image behind:
 *
 * - film grain, when the frame header sets `apply_grain`
 * - superres, when the coded width differs from the upscaled width
 * - a primary item of type `grid` or `iovl`
 * - a `clap` clean aperture that crops anything; one the size of the frame,
 *   with no offset, is ignored
 * - PQ (transfer 16) or HLG (transfer 18) in a `colr nclx` property
 * - 10 and 12 bit samples, 4:2:2, loop restoration, palette and intra block
 *   copy
 *
 * An alpha item that cannot be located, read or decoded makes the whole decode
 * fail with that item's error, and so does one whose extent differs from the
 * colour's, with TINYIMG_ERR_CORRUPT. The colour is not returned opaque.
 *
 * @note An `irot` or `imir` property is read and not applied. The decoder
 * returns the coded pixels and `probe` reports the coded extents, which is what
 * `avifdec --info` reports for a rotated file. Both properties are mapped to
 * the EXIF orientation they equal by tiny_avif_orientation, so the planner's
 * `autoOrient` turns the image the same way it turns a JPEG.
 *
 * @note The frame count is always one. A still image is one frame, and an
 * image sequence keeps its frame count in a movie track, which is a separate
 * parse this build does not do.
 *
 * @note HEIF shares the container and is deliberately not claimed here. Its
 * brands route to TINYIMG_FORMAT_HEIF, which no codec registers, so a HEIF file
 * reports TINYIMG_ERR_UNSUPPORTED_CODEC from `probe` as well. The sniff here
 * matches `tiny_format_sniff` exactly: a codec that claimed a format the
 * sniffer routes elsewhere would be reachable by one path and not the other.
 */
extern const TinyCodec tiny_codec_avif;

/**
 * @brief Reads the orientation an AVIF's `irot` and `imir` properties equal.
 *
 * Maps the primary item's rotation (counter-clockwise quarter turns) and
 * mirror axis to the EXIF number that turns the coded image upright the same
 * way, applying `irot` first and `imir` second as `avifdec` does. No pixel is
 * decoded. The mapping was checked against `avifdec` output for every angle
 * with and without each axis. An AVIF's EXIF orientation is not consulted: the
 * container transform is the one that applies.
 *
 * @param buffer The encoded AVIF.
 * @param size Length of `buffer`.
 * @return uint32_t 1 through 8, numbered as EXIF numbers them, or 0 when the
 * buffer is not an AVIF or its transforms amount to none.
 */
uint32_t tiny_avif_orientation(const uint8_t* buffer, size_t size);

#ifdef __cplusplus
}
#endif
