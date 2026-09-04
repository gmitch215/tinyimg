/**
 * @file jpeg.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief JPEG codec, baseline and progressive.
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
 * @brief The JPEG codec.
 *
 * Decodes baseline and extended sequential Huffman streams and progressive
 * ones, at any sampling factors, with restart markers, and for one, three or
 * four components: grayscale, YCbCr, and CMYK or YCCK through the Adobe APP14
 * transform. Arithmetic coded and lossless streams are recognized and reported
 * as an unsupported variant rather than as corrupt.
 *
 * Chroma is upsampled with the triangle filter libjpeg calls fancy upsampling
 * for the 2x cases and by replication otherwise, so the output matches what
 * `djpeg` produces rather than being a nearest neighbor approximation of it.
 *
 * A scaled decode is done in the DCT domain: the top left NxN coefficients of
 * each block go through an N point inverse transform, which is both smaller and
 * cheaper than transforming all 64 and averaging. That is a low pass reduction
 * rather than a box average, so it differs from the other codecs' scaled
 * decode by a fraction of a level, and the region origin is rounded down to a
 * multiple of `scale_den`, since a scaled plane has no sample at a finer
 * offset.
 *
 * A region decode holds only the MCU rows the region touches, plus one row
 * either side for the upsampler's context. The entropy coded data before them
 * still has to be scanned, because a Huffman stream cannot be entered at an
 * arbitrary point and DC coefficients are differential, but nothing outside the
 * region is transformed or stored.
 *
 * **Progressive streams are the one exception to bounded memory.** Successive
 * approximation means no coefficient is final until the last scan that touches
 * it, so the whole coefficient plane exists before any pixel does: two bytes
 * per sample, which for 4:4:4 is six bytes per pixel. A region decode of a
 * progressive file therefore costs the same as a full one, and a large enough
 * file reports TINYIMG_ERR_MEMORY where the same image baseline coded would
 * decode.
 *
 * Encoding writes baseline or progressive streams with Huffman tables built
 * from the image's own symbol frequencies rather than the Annex K examples,
 * which costs one extra pass over the coefficients and is worth 2 to 6 percent.
 * Quantization tables are the Annex K ones scaled by the libjpeg quality
 * mapping.
 */
extern const TinyCodec tiny_codec_jpeg;

#ifdef __cplusplus
}
#endif
