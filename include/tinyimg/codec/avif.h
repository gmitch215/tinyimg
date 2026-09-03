/**
 * @file avif.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief AVIF container reader; the coded image is not decoded.
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
 * @brief The AVIF codec, which answers `probe` and nothing else.
 *
 * Walks the ISO base media container far enough to describe the image: the
 * brand, the primary item, and that item's spatial extents, channel count and
 * bit depth, found by following the property associations rather than by taking
 * the first property of each kind, since a file carrying alpha has two of
 * everything. An alpha channel is reported when an auxiliary item declares the
 * alpha type.
 *
 * `decode` and `encode` are NULL, so both report
 * TINYIMG_ERR_UNSUPPORTED_CODEC: the container is described, the AV1 bitstream
 * inside it is not read. That is a deliberate boundary rather than an
 * omission. An AV1 intra decoder is between 8 and 10 thousand lines, about what
 * the rest of this library comes to, and its default probability tables alone
 * are larger than this module's whole size target; the surviving objective and
 * the measurements behind the decision are in `TECHNICAL_REPORT.md`.
 *
 * @note An `irot` or `imir` property is walked past rather than read. The
 * extents reported are the coded ones, which is what `avifdec --info` reports
 * for a rotated file, and there is no field on TinyImageInfo for an
 * orientation to land in. A rotation is a geometry operation, and applying one
 * needs a decode.
 *
 * @note The frame count is always one. A still image is one frame, and an
 * image sequence keeps its frame count in a movie track, which is a separate
 * parse for a format this build cannot decode either way.
 *
 * @note HEIF shares the container and is deliberately not claimed here. Its
 * brands route to TINYIMG_FORMAT_HEIF, which no codec registers, so a HEIF file
 * reports TINYIMG_ERR_UNSUPPORTED_CODEC from `probe` as well. The sniff here
 * matches `tiny_format_sniff` exactly: a codec that claimed a format the
 * sniffer routes elsewhere would be reachable by one path and not the other.
 */
extern const TinyCodec tiny_codec_avif;

#ifdef __cplusplus
}
#endif
