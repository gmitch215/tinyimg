/**
 * @file gif.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Graphics Interchange Format codec.
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
 * @brief The GIF codec.
 *
 * Decodes the first frame of either version, with a global or a local colour
 * table, interlaced or not, and honours the graphic control extension's
 * transparent index. `probe` reports how many frames the file holds, so an
 * animation is identifiable without decoding it; composing or re-timing frames
 * is out of scope.
 *
 * The canvas is the logical screen the file declares rather than the frame,
 * which for a frame smaller than the screen or placed at an offset is what a
 * viewer shows. A file with a transparent index decodes to four channels and
 * one without to three.
 *
 * Encoding writes a single frame with one global colour table. An image already
 * inside 256 colours keeps them exactly, so a logo or a flat illustration round
 * trips without loss; anything wider goes through an octree quantiser with
 * Floyd-Steinberg error diffusion, which is applied when and only when colours
 * had to be discarded. A source with alpha spends one palette entry on a
 * transparent index, since the format has no partial transparency to spend it
 * on.
 *
 * @note LZW is decoded into a whole index plane rather than streamed a row at a
 * time. The plane is one byte per pixel, an eighth of what the RGBA plane the
 * other row-streaming codecs avoid would cost, and the format's codes cross row
 * boundaries freely, so streaming would need a pull interface for what is
 * already the cheapest buffer in the library.
 */
extern const TinyCodec tiny_codec_gif;

#ifdef __cplusplus
}
#endif
