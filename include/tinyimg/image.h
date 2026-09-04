/**
 * @file image.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Image processing library utilities.
 * @version 1.0.0
 * @date 2026-08-31
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region image handling

/**
 * @brief Image formats supported by TinyImage for conversion and saving.
 *
 * This enumeration defines the different image formats that are supported by
 * TinyImage for conversion and saving operations.
 */
typedef enum TinyImageFormat
{
    TINYIMG_FORMAT_UNKNOWN = 0,
    TINYIMG_FORMAT_PNG = 1,
    TINYIMG_FORMAT_JPEG = 2,
    TINYIMG_FORMAT_BMP = 3,
    TINYIMG_FORMAT_GIF = 4,
    TINYIMG_FORMAT_TIFF = 5,
    TINYIMG_FORMAT_WEBP = 6,
    TINYIMG_FORMAT_AVIF = 7,
    TINYIMG_FORMAT_HEIF = 8,
} TinyImageFormat;

/**
 * @brief What the channels of an image mean.
 *
 * Derived from the channel count rather than stored, so it cannot disagree with
 * the pixel data.
 */
typedef enum TinyImagePixelType
{
    /**
     * @brief One channel, luminance.
     */
    TINYIMG_PIXEL_GRAY = 0,
    /**
     * @brief Two channels, luminance then alpha.
     */
    TINYIMG_PIXEL_GRAY_ALPHA = 1,
    /**
     * @brief Three channels, red then green then blue.
     */
    TINYIMG_PIXEL_RGB = 2,
    /**
     * @brief Four channels, red then green then blue then alpha.
     */
    TINYIMG_PIXEL_RGBA = 3,
} TinyImagePixelType;

/**
 * @brief Metadata carried alongside the pixels.
 *
 * Opaque here; EXIF and key value entries are reached through the metadata
 * region below.
 */
typedef struct TinyImageMeta TinyImageMeta;

/**
 * @brief Represents an image with its width, height, and pixel data.
 *
 * The pixel data is stored in a contiguous block of memory, where each
 * pixel is represented by a single byte (grayscale) or multiple bytes
 * (e.g., RGB, RGBA). Rows are tightly packed; there is no stride separate from
 * width * channels.
 *
 * `format` records where the pixels came from, or what they will be written as
 * once encoded, and `quality` the setting a lossy encoder should use. Neither
 * affects the pixel data.
 */
typedef struct {
    /** Width in pixels. */
    uint32_t width;
    /** Height in pixels. */
    uint32_t height;
    /** Bytes per pixel, 1 through 4; see TinyImagePixelType. */
    uint8_t channels;
    /** Setting a lossy encoder should use, 0 to 100. Zero means the default. */
    uint8_t quality;
    /** Where the pixels came from, or what they will be written as. */
    TinyImageFormat format;
    /** width * height * channels bytes, rows tightly packed. */
    uint8_t* data;
    /** EXIF and key value entries, or NULL when there are none. */
    TinyImageMeta* meta;
} TinyImage;

/**
 * @brief What a format's header says about an image, before any pixel is
 * decoded.
 *
 * Filled by tiny_image_probe for every format the library recognizes, including
 * the ones it cannot decode.
 *
 * Unlike TinyImage, this layout is part of the ABI: the fields are ordered so
 * there is no padding between them, and a host reads them straight out of
 * linear memory at their documented offsets rather than through an accessor
 * apiece. Changing the order or the widths bumps TINYIMG_ABI_VERSION.
 */
typedef struct {
    /** Width in pixels. */
    uint32_t width;
    /** Height in pixels. */
    uint32_t height;
    /** Frames the file holds; 1 for a still image. */
    uint32_t frames;
    /** The format the magic bytes identified. */
    TinyImageFormat format;
    /** Channels a full decode would produce. */
    uint8_t channels;
    /** Bits per channel in the file, before decoding normalizes them to 8. */
    uint8_t bit_depth;
    /** Non-zero when the file carries transparency. */
    uint8_t has_alpha;
    /** Non-zero for a progressive JPEG, which cannot stream a region. */
    uint8_t progressive;
} TinyImageInfo;

/**
 * @brief Options a decoder honors when only part of an image is wanted.
 *
 * The rectangle is in source pixels and is clamped to the image. A width or
 * height of zero means the full extent in that axis. `scale_den` then
 * subsamples that rectangle, so the decoded image is ceil(width / scale_den)
 * across.
 *
 * Every codec box averages when downscaling, so a scaled decode is a real
 * reduction rather than a nearest neighbor pick. JPEG additionally does the
 * work in the DCT domain, which is why its scaled decode is cheaper as well as
 * smaller.
 */
typedef struct {
    /** Left edge of the region in source pixels. */
    uint32_t x;
    /** Top edge of the region in source pixels. */
    uint32_t y;
    /** Region width; zero means to the right edge. */
    uint32_t width;
    /** Region height; zero means to the bottom edge. */
    uint32_t height;
    /** Subsampling denominator: 1, 2, 4 or 8. Anything else is read as 1. */
    uint8_t scale_den;
    /** Channels to produce, 1 through 4. Zero keeps the file's own count. */
    uint8_t channels;
    /**
     * @brief How much computation the decoder may spend; a TinyEffort.
     *
     * Zero is TINYIMG_EFFORT_FANCY, the default, which decodes to the
     * bitstream's own definition.
     *
     * Only a lossy decoder has anything to drop, and the reason is structural:
     * a lossless format defines its pixels exactly, so every step is needed to
     * produce them and there is no approximation available. PNG, GIF, TIFF and
     * WebP lossless therefore decode identically at either effort.
     *
     * The two that do have a lever both spend it on a smoothing pass:
     *
     * - VP8 skips its deblocking filter, which is 1.53x for 46.8 dB.
     * - JPEG replicates chroma instead of interpolating it, which is 1.11x to
     *   1.25x for 43.6 to 59.5 dB. It reaches subsampled files only, so a
     *   4:4:4 file is unaffected.
     */
    uint8_t effort;
} TinyDecodeOpts;

/**
 * @brief How much computation an operation may spend to do its best work.
 *
 * A separate axis from quality. Quality says what the output should look like;
 * this says how hard to work to get there, and the two are not the same dial:
 * a bounded search at quality 80 produces an image at roughly quality 80, in a
 * few more bytes, for a fraction of the CPU.
 *
 * The distinction matters because the Workers Free plan allows 10 milliseconds
 * of CPU per request and does not let a caller pay for more. A request that
 * cannot fit as TINYIMG_EFFORT_FANCY may fit as TINYIMG_EFFORT_FAST, and a
 * caller who would rather serve a slightly larger file than fail can say so.
 *
 * **Not every operation has both.** An operation with nothing to trade ignores
 * this and does the exact thing, which is why it is a hint about effort rather
 * than a promise about output. Each one that honors it documents what it gives
 * up and carries a measured floor in the tests. Nothing here degrades silently.
 */
typedef enum TinyEffort
{
    /**
     * @brief Spend what it takes to do the best work available.
     *
     * The default, and what every operation did before this existed.
     */
    TINYIMG_EFFORT_FANCY = 0,
    /**
     * @brief Bound the search, take the cheaper kernel, stay inside a floor.
     *
     * What each operation actually does is in its own documentation. It is
     * never "skip a step and hope": every fast path here is measured against
     * the fancy one and its cost in decibels and in bytes is recorded.
     */
    TINYIMG_EFFORT_FAST = 1,
} TinyEffort;

/**
 * @brief Options an encoder honors.
 */
typedef struct {
    /** Quality for a lossy format, 0 to 100. */
    uint8_t quality;
    /** Non-zero to use a format's lossless mode where it has one. */
    uint8_t lossless;
    /** Non-zero to write a progressive or interlaced stream. */
    uint8_t progressive;
    /** Non-zero to drop EXIF and other metadata from the output. */
    uint8_t strip_metadata;
    /**
     * @brief How much computation the encoder may spend; a TinyEffort.
     *
     * Zero is TINYIMG_EFFORT_FANCY, so a zeroed structure asks for the work
     * every caller got before this field existed.
     */
    uint8_t effort;
} TinyEncodeOpts;

/**
 * @brief Creates a new image with the specified width, height and channel
 * count.
 *
 * The pixel data is zeroed, which for an image with alpha means fully
 * transparent and for one without means black.
 *
 * @param image Pointer to a TinyImage structure where the new image will be
 * stored.
 * @param width The width of the new image in pixels.
 * @param height The height of the new image in pixels.
 * @param channels Channels per pixel, 1 through 4.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE for a zero dimension or channel
 * count, TINYIMG_ERR_TOO_LARGE past TINYIMG_MAX_PIXELS or
 * TINYIMG_MAX_IMAGE_BYTES, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_create(
    TinyImage* image, uint32_t width, uint32_t height, uint8_t channels
);

/**
 * @brief Reads an image's header without decoding any pixels.
 *
 * The cheapest of the four ways in. Answers for every format the library
 * recognizes, including AVIF and HEIF, which it can describe but not decode.
 *
 * @param buffer A pointer to the buffer containing the image data.
 * @param buffer_size The size of the buffer in bytes.
 * @param info Receives what the header says.
 * @return int TINYIMG_OK, TINYIMG_ERR_UNKNOWN_FORMAT when no format matched, or
 * TINYIMG_ERR_CORRUPT when the header is malformed.
 */
int tiny_image_probe(
    const uint8_t* buffer, size_t buffer_size, TinyImageInfo* info
);

/**
 * @brief Loads an image, decoding every pixel.
 *
 * The format is identified from the buffer's magic bytes; there is no format
 * argument to get wrong.
 *
 * @param image Pointer to a TinyImage structure where the loaded image will be
 * stored.
 * @param buffer A pointer to the buffer containing the image data.
 * @param buffer_size The size of the buffer in bytes.
 * @return int TINYIMG_OK, or a negative TinyImageError. An image past
 * TINYIMG_MAX_PIXELS or TINYIMG_MAX_IMAGE_BYTES reports
 * TINYIMG_ERR_TOO_LARGE rather than TINYIMG_ERR_MEMORY, because the remedy is
 * tiny_image_load_scaled.
 */
int tiny_image_load(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size
);

/**
 * @brief Loads an image at the cheapest scale that still covers the given box.
 *
 * Codecs offer halves, quarters and eighths, so the result is the smallest of
 * those that is still at least as large as the box in both axes. It is
 * therefore usually larger than the box, never smaller unless the source itself
 * is, which leaves any resampling that follows as a downscale.
 *
 * This is the entry point that makes an oversized source usable: statistics,
 * face detection and any output smaller than the source all read a reduced
 * decode instead of the full pixel count.
 *
 * @param image Pointer to a TinyImage structure where the loaded image will be
 * stored.
 * @param buffer A pointer to the buffer containing the image data.
 * @param buffer_size The size of the buffer in bytes.
 * @param max_width Width the decode must still cover. Zero means unconstrained.
 * @param max_height Height the decode must still cover. Zero means
 * unconstrained.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_load_scaled(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size,
    uint32_t max_width, uint32_t max_height
);

/**
 * @brief Loads one rectangle of an image.
 *
 * Memory is bounded by the rectangle rather than by the source, except for
 * progressive JPEG: successive approximation needs the whole coefficient plane
 * before any pixel is final, so a progressive file is decoded and then cropped.
 *
 * @param image Pointer to a TinyImage structure where the loaded image will be
 * stored.
 * @param buffer A pointer to the buffer containing the image data.
 * @param buffer_size The size of the buffer in bytes.
 * @param x Left edge of the rectangle in source pixels.
 * @param y Top edge of the rectangle in source pixels.
 * @param width Width of the rectangle. Zero means to the right edge.
 * @param height Height of the rectangle. Zero means to the bottom edge.
 * @return int TINYIMG_OK, TINYIMG_ERR_BOUNDS when the rectangle starts outside
 * the image, or a negative TinyImageError.
 */
int tiny_image_load_region(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height
);

/**
 * @brief Loads an image with full control over region, scale and channel count.
 *
 * The primitive the other three loaders are written in terms of.
 *
 * @param image Pointer to a TinyImage structure where the loaded image will be
 * stored.
 * @param buffer A pointer to the buffer containing the image data.
 * @param buffer_size The size of the buffer in bytes.
 * @param opts Region, scale and channel count. NULL decodes everything.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_decode(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size,
    const TinyDecodeOpts* opts
);

/**
 * @brief Encodes an image into a growing byte sink.
 *
 * The writer sizes itself, so no caller has to guess an output length. On
 * success the bytes are `writer->data` for `writer->size` bytes, until the
 * caller releases them with tiny_writer_free or takes them with
 * tiny_writer_detach.
 *
 * @param image Pointer to the TinyImage structure to be encoded.
 * @param format The container to write.
 * @param opts Quality and related settings. NULL uses the image's own quality
 * and the format's defaults.
 * @param writer An initialized TinyWriter to append to.
 * @return int TINYIMG_OK, TINYIMG_ERR_UNSUPPORTED_CODEC when the format has no
 * encoder in this build, or a negative TinyImageError.
 */
int tiny_image_encode(
    const TinyImage* image, TinyImageFormat format, const TinyEncodeOpts* opts,
    TinyWriter* writer
);

/**
 * @brief Destroys an image, freeing its associated memory.
 *
 * Safe to call on a zeroed structure and safe to call twice.
 *
 * @param image Pointer to the TinyImage structure to be destroyed.
 * @return int TINYIMG_OK, or TINYIMG_ERR_NULL if the image is NULL.
 */
int tiny_image_destroy(TinyImage* image);

/**
 * @brief Retrieves the type of the image (grayscale, grayscale with alpha, RGB
 * or RGBA).
 *
 * @param image Pointer to the TinyImage structure whose type is to be
 * determined.
 * @param type Receives the pixel type.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_RANGE when the
 * channel count is not 1 through 4.
 */
int tiny_image_gettype(const TinyImage* image, TinyImagePixelType* type);

/**
 * @brief Retrieves the short name of a format, such as "png" or "jpeg".
 *
 * @param format The format.
 * @return const char* A NUL terminated ASCII name, or "unknown".
 */
const char* tiny_format_name(TinyImageFormat format);

/**
 * @brief Retrieves the conventional file extension of a format, leading dot
 * included.
 *
 * @param format The format.
 * @return const char* A NUL terminated extension, or "" for an unknown format.
 */
const char* tiny_format_extension(TinyImageFormat format);

/**
 * @brief Identifies a format from a buffer's magic bytes.
 *
 * Recognizes more formats than the library can decode, so a caller can tell an
 * unsupported format apart from an unrecognizable one.
 *
 * @param buffer The bytes.
 * @param buffer_size Number of bytes. Fewer than 12 limits what can be
 * identified.
 * @return TinyImageFormat The format, or TINYIMG_FORMAT_UNKNOWN.
 */
TinyImageFormat tiny_format_sniff(const uint8_t* buffer, size_t buffer_size);

/**
 * @brief Retrieves the file extension associated with the image format.
 *
 * @param image Pointer to the TinyImage structure whose file extension is to
 * be determined.
 * @param extension Pointer to a character array where the file extension will
 * be stored. The array should be large enough to hold the extension string.
 * @param max_length The maximum length of the extension string, including the
 * null terminator.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_UNKNOWN_FORMAT, or
 * TINYIMG_ERR_BUFFER_TOO_SMALL when max_length is insufficient.
 */
int tiny_image_getextension(
    const TinyImage* image, char* extension, size_t max_length
);

/**
 * @brief Retrieves the pixel value at the specified (x, y) coordinates in the
 * image.
 *
 * The function calculates the appropriate index in the pixel data array based
 * on the image's width and height, and retrieves the pixel value. The pixel
 * value is returned as a single byte for grayscale images or as multiple bytes
 * for color images (e.g., RGB, RGBA).
 *
 * @param image Pointer to the TinyImage structure from which to retrieve the
 * pixel value.
 * @param x The x-coordinate of the pixel (0-based index).
 * @param y The y-coordinate of the pixel (0-based index).
 * @param pixel Pointer to a uint8_t array where the pixel value will be stored.
 * The size of the array should match the number of channels in the image type.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS when the
 * coordinates fall outside the image.
 */
int tiny_image_getpixel(
    const TinyImage* image, uint32_t x, uint32_t y, uint8_t* pixel
);

/**
 * @brief Writes the pixel value at the specified (x, y) coordinates in the
 * image.
 *
 * A plain bounds checked write with no blending. The drawing region composites.
 *
 * @param image Pointer to the TinyImage structure to write into.
 * @param x The x-coordinate of the pixel (0-based index).
 * @param y The y-coordinate of the pixel (0-based index).
 * @param pixel Pointer to a uint8_t array containing the pixel value. The size
 * of the array should match the number of channels in the image type.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS when the
 * coordinates fall outside the image.
 */
int tiny_image_setpixel(
    TinyImage* image, uint32_t x, uint32_t y, const uint8_t* pixel
);

/**
 * @brief Size of a TinyImageInfo, for a host allocating one across the wasm
 * boundary.
 *
 * @return uint32_t sizeof(TinyImageInfo).
 */
uint32_t tiny_image_info_sizeof(void);

/**
 * @brief Size of a TinyImage, for a host allocating one across the wasm
 * boundary.
 *
 * The struct layout is not part of the ABI; the accessors below are. This
 * exists so a host can reserve the right number of bytes without knowing the
 * layout.
 *
 * @return uint32_t sizeof(TinyImage).
 */
uint32_t tiny_image_sizeof(void);

/**
 * @brief Width of an image in pixels.
 *
 * @param image The image.
 * @return uint32_t The width, or 0 if the image is NULL.
 */
uint32_t tiny_image_getwidth(const TinyImage* image);

/**
 * @brief Height of an image in pixels.
 *
 * @param image The image.
 * @return uint32_t The height, or 0 if the image is NULL.
 */
uint32_t tiny_image_getheight(const TinyImage* image);

/**
 * @brief Channels per pixel.
 *
 * @param image The image.
 * @return uint32_t The channel count, or 0 if the image is NULL.
 */
uint32_t tiny_image_getchannels(const TinyImage* image);

/**
 * @brief Pointer to an image's pixel data.
 *
 * @param image The image.
 * @return uint8_t* The pixels, or NULL if the image is NULL or empty.
 */
uint8_t* tiny_image_getdata(const TinyImage* image);

/**
 * @brief Bytes an image's pixel data occupies.
 *
 * @param image The image.
 * @return uint32_t width * height * channels, or 0 if the image is NULL.
 */
uint32_t tiny_image_getsize(const TinyImage* image);

#pragma endregion

#pragma region image drawing

/**
 * @brief How a shape's color is combined with what is already there.
 *
 * The separable blend modes from the CSS compositing specification, so a mode
 * named here and the same mode named in a stylesheet produce the same pixels.
 * Each is a function of one destination channel and one source channel; the
 * modes that are not separable, and so cannot be, are out of scope.
 */
typedef enum TinyBlendMode
{
    /** Source over destination, weighted by alpha. The default. */
    TINYIMG_BLEND_NORMAL = 0,
    /** Source replaces destination, alpha included. */
    TINYIMG_BLEND_REPLACE = 1,
    /** Product of the two, which always darkens. */
    TINYIMG_BLEND_MULTIPLY = 2,
    /** Inverse of the product of the inverses, which always lightens. */
    TINYIMG_BLEND_SCREEN = 3,
    /** Multiply where the destination is dark, screen where it is light. */
    TINYIMG_BLEND_OVERLAY = 4,
    /** The darker of the two. */
    TINYIMG_BLEND_DARKEN = 5,
    /** The lighter of the two. */
    TINYIMG_BLEND_LIGHTEN = 6,
    /** Absolute difference. */
    TINYIMG_BLEND_DIFFERENCE = 7,
    /** Sum less the product, which is difference without the sign. */
    TINYIMG_BLEND_EXCLUSION = 8,
    /** Multiply where the source is dark, screen where it is light. */
    TINYIMG_BLEND_HARD_LIGHT = 9,
    /** Overlay with a softer curve, which does not clip. */
    TINYIMG_BLEND_SOFT_LIGHT = 10,
    /** Sum, clamped. */
    TINYIMG_BLEND_ADD = 11,
    /** Destination less source, clamped. */
    TINYIMG_BLEND_SUBTRACT = 12,
} TinyBlendMode;

/**
 * @brief Which points a polygon fill considers inside.
 *
 * The two rules agree on every polygon that does not cross itself, so a test
 * written against a convex shape cannot tell them apart.
 */
typedef enum TinyFillRule
{
    /**
     * @brief Inside when a ray to the point crosses an odd number of edges.
     *
     * A self-crossing shape has holes where its lobes overlap.
     */
    TINYIMG_FILL_EVEN_ODD = 0,
    /**
     * @brief Inside when the signed crossings do not cancel.
     *
     * A self-crossing shape is solid, and the direction the edges are wound
     * in decides whether an inner loop is a hole.
     */
    TINYIMG_FILL_NONZERO = 1,
} TinyFillRule;

/**
 * @brief How a source image larger or smaller than its target is placed.
 */
typedef enum TinyDrawMode
{
    /** Once, at the offset, clipped to the destination. */
    TINYIMG_DRAW_ONCE = 0,
    /** Repeated to cover the destination, starting from the offset. */
    TINYIMG_DRAW_TILE = 1,
    /** Once, centered in the destination, ignoring the offset. */
    TINYIMG_DRAW_CENTER = 2,
} TinyDrawMode;

/**
 * @brief Draws a horizontal run of pixels.
 *
 * Coordinates are signed and the run is clipped, so a line that starts or ends
 * outside the image draws the part of it that is inside rather than failing. A
 * run whose endpoints are both outside on the same side draws nothing.
 *
 * @param image The image to draw on.
 * @param x1 One end.
 * @param y1 The row. `y2` is ignored, so the two forms of a line share a
 * signature.
 * @param x2 The other end. May be to the left of `x1`.
 * @param y2 Ignored.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_hline(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    const uint8_t* pixel
);

/**
 * @brief Draws a vertical run of pixels.
 *
 * @param image The image to draw on.
 * @param x1 The column. `x2` is ignored.
 * @param y1 One end.
 * @param x2 Ignored.
 * @param y2 The other end. May be above `y1`.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_vline(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    const uint8_t* pixel
);

/**
 * @brief Draws the outline of a rectangle, one pixel wide.
 *
 * @param image The image to draw on.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
);

/**
 * @brief Fills a rectangle.
 *
 * @param image The image to draw on.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_fill_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
);

/**
 * @brief Fills a rectangle whose corners are rounded.
 *
 * @param image The image to draw on.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param radius Corner radius; clamped to half the shorter side, so a radius
 * past that gives a stadium rather than an error.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_fill_rounded_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t radius, const uint8_t* pixel
);

/**
 * @brief Draws a line of the given thickness.
 *
 * Bresenham for a single-pixel line, and a distance test against the segment
 * for a thicker one, which is what keeps a thick line's ends square and its
 * joins free of the gaps a stamped brush leaves on a steep slope.
 *
 * @param image The image to draw on.
 * @param x1 Start.
 * @param y1 Start.
 * @param x2 End.
 * @param y2 End.
 * @param thickness Width in pixels; zero and one both mean a single pixel.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_line(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    uint32_t thickness, const uint8_t* pixel
);

/**
 * @brief Draws the outline of a circle.
 *
 * @param image The image to draw on.
 * @param center_x Center.
 * @param center_y Center.
 * @param radius Radius in pixels.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_circle(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* pixel
);

/**
 * @brief Fills a circle.
 *
 * @param image The image to draw on.
 * @param center_x Center.
 * @param center_y Center.
 * @param radius Radius in pixels.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_fill_circle(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* pixel
);

/**
 * @brief Draws the outline of an axis-aligned ellipse.
 *
 * @param image The image to draw on.
 * @param center_x Center.
 * @param center_y Center.
 * @param radius_x Horizontal semi-axis.
 * @param radius_y Vertical semi-axis.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_ellipse(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius_x,
    uint32_t radius_y, const uint8_t* pixel
);

/**
 * @brief Fills an axis-aligned ellipse.
 *
 * @param image The image to draw on.
 * @param center_x Center.
 * @param center_y Center.
 * @param radius_x Horizontal semi-axis.
 * @param radius_y Vertical semi-axis.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_fill_ellipse(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius_x,
    uint32_t radius_y, const uint8_t* pixel
);

/**
 * @brief Draws the edges of a polygon, closing it.
 *
 * @param image The image to draw on.
 * @param x_points Vertex x coordinates.
 * @param y_points Vertex y coordinates.
 * @param num_points How many vertices; below two draws nothing.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or a negative TinyImageError.
 */
int tiny_image_polygon(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel
);

/**
 * @brief Fills a polygon by the even-odd rule.
 *
 * @param image The image to draw on.
 * @param x_points Vertex x coordinates.
 * @param y_points Vertex y coordinates.
 * @param num_points How many vertices; below three fills nothing.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or a negative TinyImageError.
 */
int tiny_image_fill_polygon(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel
);

/**
 * @brief Fills a polygon by a named rule, blended.
 *
 * @param image The image to draw on.
 * @param x_points Vertex x coordinates.
 * @param y_points Vertex y coordinates.
 * @param num_points How many vertices.
 * @param pixel As many channels as the image has.
 * @param rule Which points count as inside.
 * @param blend How the color meets what is there.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or a negative TinyImageError.
 */
int tiny_image_fill_polygon_with(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel, TinyFillRule rule,
    TinyBlendMode blend
);

/**
 * @brief Replaces every pixel close to one color with another.
 *
 * @param image The image to change.
 * @param old_color The color to look for, as many channels as the image has.
 * @param new_color What to write instead.
 * @param tolerance How far each channel may differ and still match, as many
 * channels as the image has. NULL means an exact match.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_replace_color(
    TinyImage* image, const uint8_t* old_color, const uint8_t* new_color,
    const uint8_t* tolerance
);

/**
 * @brief Draws one image onto another.
 *
 * The source's alpha is honored, so a transparent overlay composites rather
 * than punching a hole. Channel counts need not match: a source with alpha
 * over a destination without one blends against the destination, and a source
 * without alpha is opaque.
 *
 * @param dest_image The image to draw on.
 * @param src_image The image to draw.
 * @param x Where the source's left edge lands.
 * @param y Where the source's top edge lands.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_image(
    TinyImage* dest_image, const TinyImage* src_image, int32_t x, int32_t y
);

/**
 * @brief Draws one image onto another with an opacity, a placement and a blend
 * mode.
 *
 * @param dest_image The image to draw on.
 * @param src_image The image to draw.
 * @param x Where the source's left edge lands, or the tiling origin.
 * @param y Where the source's top edge lands, or the tiling origin.
 * @param opacity 0 through 1, multiplied into the source's alpha.
 * @param mode Once, tiled, or centered.
 * @param blend How the colors meet.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_image_ex(
    TinyImage* dest_image, const TinyImage* src_image, int32_t x, int32_t y,
    float opacity, TinyDrawMode mode, TinyBlendMode blend
);

/**
 * @brief Composites one image over another in place, Porter-Duff source-over.
 *
 * The two must have the same extent. Unlike tiny_image_draw_image this writes
 * the composited alpha as well, so the result is what a stack of layers means
 * rather than what an opaque backdrop would have shown.
 *
 * @param dest_image The lower layer, replaced by the result.
 * @param src_image The upper layer.
 * @param blend How the colors meet.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when the extents differ, or a
 * negative TinyImageError.
 */
int tiny_image_composite(
    TinyImage* dest_image, const TinyImage* src_image, TinyBlendMode blend
);

/**
 * @brief Multiplies each color channel by its alpha.
 *
 * The form compositing and resampling are correct in. A no-op on an image with
 * no alpha channel.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_premultiply(TinyImage* image);

/**
 * @brief Divides each color channel by its alpha.
 *
 * The inverse of tiny_image_premultiply, and lossy in the same way every
 * inverse of a quantized product is: a channel that was rounded to a multiple
 * of its alpha cannot be recovered exactly. A fully transparent pixel has no
 * color to recover and is left at zero.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_unpremultiply(TinyImage* image);

/**
 * @brief Fills the image with a linear gradient.
 *
 * @param image The image to fill.
 * @param x0 Where the gradient starts.
 * @param y0 Where the gradient starts.
 * @param x1 Where it ends.
 * @param y1 Where it ends.
 * @param from The color at the start, as many channels as the image has.
 * @param to The color at the end.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when the two points coincide, or a
 * negative TinyImageError.
 */
int tiny_image_gradient_linear(
    TinyImage* image, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    const uint8_t* from, const uint8_t* to
);

/**
 * @brief Fills the image with a radial gradient.
 *
 * @param image The image to fill.
 * @param center_x Center.
 * @param center_y Center.
 * @param radius Where the outer color is reached.
 * @param inner The color at the center.
 * @param outer The color at the radius and beyond.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE for a zero radius, or a negative
 * TinyImageError.
 */
int tiny_image_gradient_radial(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* inner, const uint8_t* outer
);

/**
 * @brief Fades the image toward transparency along a direction.
 *
 * @param image The image to change. Gains an alpha channel if it has none.
 * @param angle Degrees clockwise from the positive x axis; the fade runs along
 * it, opaque at the start.
 * @param start Where the fade begins, 0 through 1 across the image.
 * @param end Where it reaches full transparency.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_gradient_fade(
    TinyImage* image, float angle, float start, float end
);

/**
 * @brief Draws a border inside the image's edges.
 *
 * @param image The image to draw on.
 * @param border_width How thick, in pixels. Zero draws nothing.
 * @param pixel As many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_border(
    TinyImage* image, uint32_t border_width, const uint8_t* pixel
);

/**
 * @brief Grows the image by a border around it.
 *
 * Unlike tiny_image_border this changes the extent rather than covering the
 * pixels at the edge, which is what a caller framing an image wants and what
 * Cloudflare Images' `border` parameter does.
 *
 * @param image The image, replaced by the larger one.
 * @param left How many columns to add.
 * @param top How many rows.
 * @param right How many columns.
 * @param bottom How many rows.
 * @param pixel The border's color, as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_expand(
    TinyImage* image, uint32_t left, uint32_t top, uint32_t right,
    uint32_t bottom, const uint8_t* pixel
);

/**
 * @brief Blends one color through an 8-bit coverage mask.
 *
 * @internal The seam between the rasterizers and the compositor. A glyph, and
 * anything else that produces coverage rather than pixels, reaches the image
 * through this rather than through its own blend, so the alpha rule and the
 * blend modes stay written once. Coverage of 255 is the color at its own
 * alpha; 0 writes nothing.
 *
 * @param image The image to draw on.
 * @param x Left edge the mask's first column lands on; may be negative.
 * @param y Top edge; may be negative.
 * @param mask `width * height` coverage bytes, rows tightly packed.
 * @param width Mask width.
 * @param height Mask height.
 * @param color The color, as many channels as the image has.
 * @param blend Which mode.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_draw_coverage(
    TinyImage* image, int32_t x, int32_t y, const uint8_t* mask, uint32_t width,
    uint32_t height, const uint8_t* color, TinyBlendMode blend
);

#pragma endregion

#pragma region image display list

/** How many shapes one display list holds. */
#define TINYIMG_DISPLAY_MAX_SHAPES 64

/** How many polygon vertices one display list holds between every shape. */
#define TINYIMG_DISPLAY_MAX_POINTS 256

/** How deep the transform stack goes. */
#define TINYIMG_DISPLAY_MAX_DEPTH 8

/** The shapes a display list can hold. */
typedef enum TinyShapeKind
{
    /** An unused slot. */
    TINYIMG_SHAPE_NONE = 0,
    /** A filled rectangle. */
    TINYIMG_SHAPE_RECT = 1,
    /** A filled rectangle with rounded corners. */
    TINYIMG_SHAPE_ROUND_RECT = 2,
    /** A filled ellipse. */
    TINYIMG_SHAPE_ELLIPSE = 3,
    /** A line of some thickness. */
    TINYIMG_SHAPE_LINE = 4,
    /** A filled polygon. */
    TINYIMG_SHAPE_POLYGON = 5,
} TinyShapeKind;

/** One shape, with the transform in force when it was added. */
typedef struct {
    /** Which shape. */
    TinyShapeKind kind;
    /** Its geometry, read according to `kind`. */
    float geometry[5];
    /** Where its vertices start, for a polygon. */
    uint32_t point_first;
    /** How many vertices it has. */
    uint32_t point_count;
    /** Its color, as many channels as the target will have. */
    uint8_t color[4];
    /** How it meets what is under it. */
    TinyBlendMode blend;
    /** Which points a polygon fill counts as inside. */
    TinyFillRule rule;
    /**
     * @brief The transform in force when the shape was added, as a 2x3 affine.
     *
     * Held per shape rather than applied when the shape was added, which is
     * what "symbolic until one rasterization" means: the list can be measured,
     * culled and reordered before any pixel exists.
     */
    float transform[6];
} TinyShape;

/**
 * @brief A list of shapes that rasterizes once.
 *
 * The Canvas surface this library exists to provide, without a Canvas. Shapes
 * accumulate with a transform stack and nothing is drawn until
 * tiny_display_render, which is what lets two eliminations run first: a shape
 * outside the target is dropped, and a shape an opaque later one covers
 * entirely is dropped as well. Both are decided from the geometry, so neither
 * costs a pass over pixels.
 *
 * Larger than a TinyPlan; keep one wherever the caller keeps its image rather
 * than deep in a recursion.
 */
typedef struct {
    /** How many shapes have been added. */
    uint32_t count;
    /** The shapes, in the order they were added. */
    TinyShape shapes[TINYIMG_DISPLAY_MAX_SHAPES];

    /** How many vertices have been stored. */
    uint32_t points;
    /** Polygon vertices, in x, y pairs. */
    float point[TINYIMG_DISPLAY_MAX_POINTS * 2];

    /** The current transform. */
    float transform[6];
    /** How many transforms are saved. */
    uint32_t depth;
    /** The saved transforms. */
    float stack[TINYIMG_DISPLAY_MAX_DEPTH][6];

    /** The mode the next shape is added with. */
    TinyBlendMode blend;

    /** Shapes the last render dropped as outside the target. */
    uint32_t culled;
    /** Shapes the last render dropped as covered by an opaque later one. */
    uint32_t covered;
} TinyDisplayList;

/**
 * @brief Size of a TinyDisplayList, for a host allocating one.
 *
 * @return uint32_t sizeof(TinyDisplayList).
 */
uint32_t tiny_display_sizeof(void);

/**
 * @brief Empties a display list and resets its transform to the identity.
 *
 * @param list The list.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_init(TinyDisplayList* list);

/**
 * @brief Pushes the current transform.
 *
 * @param list The list.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS when the
 * stack is full.
 */
int tiny_display_save(TinyDisplayList* list);

/**
 * @brief Pops the transform saved last.
 *
 * @param list The list.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS when
 * nothing is saved.
 */
int tiny_display_restore(TinyDisplayList* list);

/**
 * @brief Moves the current transform.
 *
 * @param list The list.
 * @param x How far along x.
 * @param y How far along y.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_translate(TinyDisplayList* list, float x, float y);

/**
 * @brief Scales the current transform.
 *
 * @param list The list.
 * @param x Factor along x.
 * @param y Factor along y.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_scale(TinyDisplayList* list, float x, float y);

/**
 * @brief Turns the current transform.
 *
 * @param list The list.
 * @param degrees Clockwise, any angle.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_rotate(TinyDisplayList* list, float degrees);

/**
 * @brief Sets the current transform outright.
 *
 * @param list The list.
 * @param matrix Six numbers: a, b, c, d, e, f, mapping (x, y) to
 * (a x + c y + e, b x + d y + f).
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_set_transform(TinyDisplayList* list, const float* matrix);

/**
 * @brief Adds a rectangle.
 *
 * @param list The list.
 * @param x Left edge, before the transform.
 * @param y Top edge.
 * @param width Width.
 * @param height Height.
 * @param color As many channels as the target will have.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS when the
 * list is full.
 */
int tiny_display_rect(
    TinyDisplayList* list, float x, float y, float width, float height,
    const uint8_t* color
);

/**
 * @brief Adds a rectangle with rounded corners.
 *
 * @param list The list.
 * @param x Left edge, before the transform.
 * @param y Top edge.
 * @param width Width.
 * @param height Height.
 * @param radius Corner radius.
 * @param color As many channels as the target will have.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS.
 */
int tiny_display_round_rect(
    TinyDisplayList* list, float x, float y, float width, float height,
    float radius, const uint8_t* color
);

/**
 * @brief Adds an ellipse.
 *
 * @param list The list.
 * @param center_x Center, before the transform.
 * @param center_y Center.
 * @param radius_x Horizontal semi-axis.
 * @param radius_y Vertical semi-axis.
 * @param color As many channels as the target will have.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS.
 */
int tiny_display_ellipse(
    TinyDisplayList* list, float center_x, float center_y, float radius_x,
    float radius_y, const uint8_t* color
);

/**
 * @brief Adds a line.
 *
 * @param list The list.
 * @param x1 Start, before the transform.
 * @param y1 Start.
 * @param x2 End.
 * @param y2 End.
 * @param thickness Width in pixels, before the transform.
 * @param color As many channels as the target will have.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS.
 */
int tiny_display_line(
    TinyDisplayList* list, float x1, float y1, float x2, float y2,
    float thickness, const uint8_t* color
);

/**
 * @brief Adds a polygon.
 *
 * @param list The list.
 * @param x_points Vertex x coordinates, before the transform.
 * @param y_points Vertex y coordinates.
 * @param num_points How many vertices.
 * @param color As many channels as the target will have.
 * @param rule Which points count as inside.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE below three
 * vertices, or TINYIMG_ERR_BOUNDS when the list is full.
 */
int tiny_display_polygon(
    TinyDisplayList* list, const float* x_points, const float* y_points,
    size_t num_points, const uint8_t* color, TinyFillRule rule
);

/**
 * @brief Sets the blend mode the next shapes are added with.
 *
 * @param list The list.
 * @param blend The mode.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_blend(TinyDisplayList* list, TinyBlendMode blend);

/**
 * @brief Draws every shape onto an image, in one pass over the list.
 *
 * @param list The list, whose `culled` and `covered` counts are updated.
 * @param image The image to draw on.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_display_render(TinyDisplayList* list, TinyImage* image);

/**
 * @brief How many shapes the last render dropped as outside the target.
 *
 * @param list The list.
 * @return uint32_t The count, or 0 when the list is NULL.
 */
uint32_t tiny_display_culled(const TinyDisplayList* list);

/**
 * @brief How many shapes the last render dropped as covered by a later opaque
 * one.
 *
 * @param list The list.
 * @return uint32_t The count, or 0 when the list is NULL.
 */
uint32_t tiny_display_covered(const TinyDisplayList* list);

/**
 * @brief The rectangle every shape in the list falls inside.
 *
 * @param list The list.
 * @param x Receives the left edge.
 * @param y Receives the top edge.
 * @param width Receives the width, zero when the list is empty.
 * @param height Receives the height.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_display_bounds(
    const TinyDisplayList* list, int32_t* x, int32_t* y, uint32_t* width,
    uint32_t* height
);

#pragma endregion

#pragma region image manipulation

/**
 * @brief Resamples an image to the specified new width and height.
 *
 * Every pixel of the result is read from the pixels it covers: an area average
 * when an axis is reduced and a Catmull-Rom cubic when it is enlarged, chosen
 * per axis. The aspect ratio is not preserved; tiny_image_fit is the call that
 * preserves it.
 *
 * One operation on its own is one pass. A chain of them is a pass each, and
 * TinyPlan is what collapses a chain into one.
 *
 * @param image Pointer to the TinyImage structure to be resized.
 * @param new_width The desired width in pixels. Zero keeps the aspect ratio
 * against `new_height`.
 * @param new_height The desired height in pixels. Zero keeps the aspect ratio
 * against `new_width`.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when both are zero,
 * TINYIMG_ERR_TOO_LARGE past TINYIMG_MAX_PIXELS, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_resize(
    TinyImage* image, uint32_t new_width, uint32_t new_height
);

/**
 * @brief Crops an image to the specified rectangle defined by the top-left
 * corner (x, y) and the desired width and height.
 *
 * A rectangle reaching past the right or bottom edge is clipped to it, so
 * asking for more than there is gives what there is. An origin outside the
 * image is an error rather than an empty result.
 *
 * @param image Pointer to the TinyImage structure to be cropped.
 * @param x The x-coordinate of the top-left corner of the cropping rectangle.
 * @param y The y-coordinate of the top-left corner of the cropping rectangle.
 * @param crop_width The width in pixels. Zero runs to the right edge.
 * @param crop_height The height in pixels. Zero runs to the bottom edge.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when the origin is outside the
 * image, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_crop(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t crop_width,
    uint32_t crop_height
);

/**
 * @brief Crops an image to a circular region defined by the center (center_x,
 * center_y) and the specified radius.
 *
 * The function creates a circular mask and retains only the pixels within the
 * circle, discarding the rest. The cropped image will replace the original
 * image in the TinyImage structure, and the original pixel data will be freed.
 *
 * @param image Pointer to the TinyImage structure to be cropped.
 * @param center_x The x-coordinate of the center of the circular cropping
 * region.
 * @param center_y The y-coordinate of the center of the circular cropping
 * region.
 * @param radius The radius of the circular cropping region in pixels.
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails or if the circular region exceeds image bounds).
 */
int tiny_image_crop_circle(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius
);

/**
 * @brief Crops an image to an elliptical region defined by the center
 * (center_x, center_y) and the specified radii along the x and y axes.
 *
 * The function creates an elliptical mask and retains only the pixels within
 * the ellipse, discarding the rest. The cropped image will replace the
 * original image in the TinyImage structure, and the original pixel data will
 * be freed.
 *
 * @param image Pointer to the TinyImage structure to be cropped.
 * @param center_x The x-coordinate of the center of the elliptical cropping
 * region.
 * @param center_y The y-coordinate of the center of the elliptical cropping
 * region.
 * @param radius_x The radius of the ellipse along the x-axis in pixels.
 * @param radius_y The radius of the ellipse along the y-axis in pixels.
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails or if the elliptical region exceeds image bounds).
 */
int tiny_image_crop_ellipse(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius_x,
    uint32_t radius_y
);

/**
 * @brief Crops an image to a polygonal region defined by a series of points
 * (x_points, y_points).
 *
 * The function creates a polygonal mask based on the provided vertices and
 * retains only the pixels within the polygon, discarding the rest. The cropped
 * image will replace the original image in the TinyImage structure, and the
 * original pixel data will be freed.
 *
 * @param image Pointer to the TinyImage structure to be cropped.
 * @param x_points Pointer to an array of x-coordinates for the polygon's
 * vertices (0-based indices).
 * @param y_points Pointer to an array of y-coordinates for the polygon's
 * vertices (0-based indices).
 * @param num_points The number of points (vertices) in the polygon. This
 * should match the lengths of the x_points and y_points arrays.
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails, if num_points is less than 3, or if the polygon exceeds image bounds).
 */
int tiny_image_crop_polygon(
    TinyImage* image, const uint32_t* x_points, const uint32_t* y_points,
    size_t num_points
);

/**
 * @brief Flips an image horizontally, mirroring it along the vertical axis.
 *
 * @param image Pointer to the TinyImage structure to be flipped.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_flip_horizontal(TinyImage* image);

/**
 * @brief Flips an image vertically, mirroring it along the horizontal axis.
 *
 * @param image Pointer to the TinyImage structure to be flipped.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_flip_vertical(TinyImage* image);

/**
 * @brief Rotates an image 90 degrees clockwise.
 *
 * @param image Pointer to the TinyImage structure to be rotated.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_rotate_90(TinyImage* image);

/**
 * @brief Rotates an image 180 degrees.
 *
 * @param image Pointer to the TinyImage structure to be rotated.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_rotate_180(TinyImage* image);

/**
 * @brief Rotates an image 270 degrees clockwise (or 90 degrees
 * counterclockwise).
 *
 * @param image Pointer to the TinyImage structure to be rotated.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_rotate_270(TinyImage* image);

/**
 * @brief Adjusts the opacity of an image by modifying its alpha channel.
 *
 * The function multiplies the alpha value of each pixel by the specified
 * opacity factor, which should be in the range [0.0, 1.0]. A value of 0.0
 * makes the image fully transparent, while a value of 1.0 retains the original
 * opacity.
 *
 * @param image Pointer to the TinyImage structure whose opacity is to be
 * adjusted.
 * @param opacity The opacity factor to apply to the image (0.0-1.0).
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the opacity value is out of range).
 */
int tiny_image_opacity(TinyImage* image, float opacity);

/**
 * @brief Removes a uniform border by cropping to what differs from it.
 *
 * The border color is taken from the corners, which is what a caller trimming
 * whitespace or letterboxing means. An image whose corners already differ from
 * each other is left alone.
 *
 * The scan works inward from each edge and stops at the first row or column
 * that differs, so it costs the border it removes rather than the image.
 *
 * This is not a plan operation, and cannot be: tiny_plan_resolve decides the
 * whole pipeline before any pixel is read, and how much a trim removes is a
 * function of the pixels. A caller that wants a trim inside a chain runs it
 * between two plans.
 *
 * @param image The image, replaced by the cropped one.
 * @param tolerance How far a channel may differ from the border color and
 * still count as border, 0 through 255.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when nothing would be left, or a
 * negative TinyImageError.
 */
int tiny_image_trim(TinyImage* image, uint8_t tolerance);

#pragma endregion

#pragma region image transformations

/**
 * @brief Inverts the colors of an image, producing a negative effect.
 *
 * Each pixel's color value is transformed to its complementary color by
 * subtracting the original value from the maximum value (255 for 8-bit
 * images).
 *
 * @param image Pointer to the TinyImage structure to be inverted.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_invert(TinyImage* image);

/**
 * @brief Sets the quality of the image for lossy formats (e.g., JPEG).
 *
 * The quality parameter typically ranges from 0 to 100, where higher values
 * indicate better quality and larger file sizes, while lower values indicate
 * lower quality and smaller file sizes.
 *
 * @param image Pointer to the TinyImage structure whose quality is to be set.
 * @param quality The desired quality level (0-100) for the image.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the quality value is out of range).
 */
int tiny_image_quality(TinyImage* image, int quality);

/**
 * @brief Applies a blur effect to the image.
 *
 * @param image Pointer to the TinyImage structure to be blurred.
 * @param radius The radius of the blur effect. Must be a positive integer.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the radius is invalid).
 */
int tiny_image_blur(TinyImage* image, float radius);

/**
 * @brief Applies a sharpen effect to the image.
 *
 * @param image Pointer to the TinyImage structure to be sharpened.
 * @param amount The amount of sharpening to apply. Must be a positive float.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the amount is invalid).
 */
int tiny_image_sharpen(TinyImage* image, float amount);

/**
 * @brief Adjusts the brightness of the image.
 *
 * @param image Pointer to the TinyImage structure whose brightness is to be
 * adjusted.
 * @param factor The factor by which to adjust the brightness. A value of 1.0
 * means no change, less than 1.0 darkens the image, and greater than 1.0
 * brightens the image.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the factor is invalid).
 */
int tiny_image_brightness(TinyImage* image, float factor);

/**
 * @brief Adjusts the contrast of the image.
 *
 * @param image Pointer to the TinyImage structure whose contrast is to be
 * adjusted.
 * @param factor The factor by which to adjust the contrast. A value of 1.0
 * means no change, less than 1.0 decreases contrast, and greater than 1.0
 * increases contrast.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the factor is invalid).
 */
int tiny_image_contrast(TinyImage* image, float factor);

/**
 * @brief Scales an image by a device pixel ratio.
 *
 * The same resample tiny_image_zoom performs, named for the request parameter
 * it serves: a ratio of 2 asks for twice the pixels in each axis so a display
 * with two device pixels per CSS pixel has one each.
 *
 * @param image Pointer to the TinyImage structure to be scaled.
 * @param dpr The ratio. Must be greater than zero.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a
 * non-positive ratio, or a negative TinyImageError.
 */
int tiny_image_dpr(TinyImage* image, float dpr);

/**
 * @brief Adjusts the saturation of the image.
 *
 * This function modifies the image's color saturation by adjusting its
 * saturation value. A higher saturation value results in more vivid colors,
 * while a lower saturation value results in more muted colors. The function
 * will resample the image data accordingly to maintain the visual quality of
 * the image.
 *
 * @param image Pointer to the TinyImage structure whose saturation is to be
 * adjusted.
 * @param factor The factor by which to adjust the saturation. A value of 1.0
 * means no change, less than 1.0 decreases saturation, and greater than 1.0
 * increases saturation.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the factor is invalid).
 */
int tiny_image_saturation(TinyImage* image, float factor);

/**
 * @brief Adjusts the hue of the image.
 *
 * This function modifies the image's color hue by rotating its hue value by
 * the specified angle. The angle is measured in degrees, and a positive angle
 * rotates the hue clockwise, while a negative angle rotates it
 * counterclockwise. The function will resample the image data accordingly to
 * maintain the visual quality of the image.
 *
 * @param image Pointer to the TinyImage structure whose hue is to be adjusted.
 * @param angle The angle by which to rotate the hue, in degrees. Must be a
 * float value between -360.0 and 360.0.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the angle is invalid).
 */
int tiny_image_hue(TinyImage* image, float angle);

/**
 * @brief How an image is made to fit a target width and height.
 *
 * A target rectangle and a source rectangle rarely share an aspect ratio, so
 * every mode is two independent choices: what happens to the mismatch, and how
 * far the scale is allowed to move. Reading the two columns is quicker than
 * reading eleven paragraphs.
 *
 * | Mode | Mismatch | Scale |
 * |---|---|---|
 * | TINYIMG_FIT_CONTAIN | left alone, so one axis falls short | free |
 * | TINYIMG_FIT_SCALE_DOWN | left alone | never above 1 |
 * | TINYIMG_FIT_SCALE_UP | left alone | never below 1 |
 * | TINYIMG_FIT_PAD | padded to the target | free |
 * | TINYIMG_FIT_ASPECT_CONTAIN | padded to the target's ratio | fixed at 1 |
 * | TINYIMG_FIT_COVER | cropped to the target | free |
 * | TINYIMG_FIT_CROP | cropped to the target | never above 1 |
 * | TINYIMG_FIT_FILL | cropped to the target | never below 1 |
 * | TINYIMG_FIT_ASPECT_COVER | cropped to the target's ratio | fixed at 1 |
 * | TINYIMG_FIT_ASPECT_CROP | cropped to the target's extent | fixed at 1 |
 * | TINYIMG_FIT_STRETCH | absorbed by distorting | free, per axis |
 *
 * A fixed scale means no resampling happens at all, so those three modes cost
 * stride arithmetic and reach the output without touching a pixel value. Where
 * a mode crops or pads, the gravity decides which part survives or where the
 * image sits.
 *
 * The four Cloudflare Images modes are TINYIMG_FIT_SCALE_DOWN,
 * TINYIMG_FIT_CONTAIN, TINYIMG_FIT_COVER, TINYIMG_FIT_CROP and
 * TINYIMG_FIT_PAD, with the same meanings.
 */
typedef enum TinyImageFit
{
    /**
     * @brief Fits inside the target without cropping or padding, and never
     * enlarges.
     *
     * One axis reaches the target and the other falls short, so the output is
     * usually smaller than what was asked for. A source already inside the
     * target comes through untouched.
     */
    TINYIMG_FIT_SCALE_DOWN = 0,
    /**
     * @brief Fits inside the target without cropping or padding.
     *
     * Enlarges a source smaller than the target, which TINYIMG_FIT_SCALE_DOWN
     * does not.
     */
    TINYIMG_FIT_CONTAIN = 1,
    /**
     * @brief Fills the target exactly, cropping whichever axis overflows.
     *
     * The output is always the target extent, and part of the source is lost.
     */
    TINYIMG_FIT_COVER = 2,
    /**
     * @brief Fills the target exactly, cropping the overflow, and never
     * enlarges.
     *
     * A source too small to cover the target produces a smaller output rather
     * than an enlarged one, which is the difference from TINYIMG_FIT_COVER.
     */
    TINYIMG_FIT_CROP = 3,
    /**
     * @brief Crops to the target's extent without resampling.
     *
     * Pixels keep their original scale, so this is a crop and nothing else. A
     * source smaller than the target crops to what there is.
     */
    TINYIMG_FIT_ASPECT_CROP = 4,
    /**
     * @brief Pads to the target's aspect ratio without resampling.
     *
     * Keeps every source pixel at its original scale and adds background on the
     * axis that falls short, so the output has the target's ratio but not
     * necessarily its extent.
     */
    TINYIMG_FIT_ASPECT_CONTAIN = 5,
    /**
     * @brief Crops to the target's aspect ratio without resampling.
     *
     * Keeps as much of the source as the ratio allows, at its original scale,
     * which is the largest crop of that shape the source can give.
     */
    TINYIMG_FIT_ASPECT_COVER = 6,
    /**
     * @brief Fits inside the target, then pads the shortfall with the
     * background.
     *
     * The output is always the target extent and no part of the source is lost.
     */
    TINYIMG_FIT_PAD = 7,
    /**
     * @brief Scales each axis independently to the target, distorting the
     * image.
     *
     * The only mode that does not preserve the aspect ratio.
     */
    TINYIMG_FIT_STRETCH = 8,
    /**
     * @brief Fills the target exactly, cropping the overflow, and never
     * shrinks.
     *
     * The mirror of TINYIMG_FIT_CROP: a source larger than the target is
     * cropped at its original scale rather than reduced.
     */
    TINYIMG_FIT_FILL = 9,
    /**
     * @brief Fits inside the target without cropping or padding, and never
     * shrinks.
     *
     * The mirror of TINYIMG_FIT_SCALE_DOWN.
     */
    TINYIMG_FIT_SCALE_UP = 10,
} TinyImageFit;

/**
 * @brief Which part of an image a crop keeps, or where a pad puts it.
 *
 * The nine fixed positions are arithmetic. The two computed ones read the
 * image, and until the phase that implements them lands they fall back to
 * TINYIMG_GRAVITY_CENTER rather than failing, which is also what they do when
 * the detector finds nothing.
 */
typedef enum TinyImageGravity
{
    /** The middle, and the default. */
    TINYIMG_GRAVITY_CENTER = 0,
    /** The top edge. */
    TINYIMG_GRAVITY_NORTH = 1,
    /** The bottom edge. */
    TINYIMG_GRAVITY_SOUTH = 2,
    /** The left edge. */
    TINYIMG_GRAVITY_WEST = 3,
    /** The right edge. */
    TINYIMG_GRAVITY_EAST = 4,
    /** The top left corner. */
    TINYIMG_GRAVITY_NORTH_WEST = 5,
    /** The top right corner. */
    TINYIMG_GRAVITY_NORTH_EAST = 6,
    /** The bottom left corner. */
    TINYIMG_GRAVITY_SOUTH_WEST = 7,
    /** The bottom right corner. */
    TINYIMG_GRAVITY_SOUTH_EAST = 8,
    /** Wherever the detail is, measured from the image. */
    TINYIMG_GRAVITY_AUTO = 9,
    /** Wherever the faces are. */
    TINYIMG_GRAVITY_FACE = 10,
} TinyImageGravity;

/**
 * @brief Resizes an image to fit within the specified target width and height
 * according to the specified fit mode.
 *
 * The function adjusts the image dimensions based on the selected fit mode,
 * which determines how the image will be scaled, cropped, or padded to fit
 * within the target dimensions while maintaining its aspect ratio.
 *
 * @param image Pointer to the TinyImage structure to be resized.
 * @param target_width The desired width of the resized image in pixels.
 * @param target_height The desired height of the resized image in pixels.
 * @param fit_mode The fit mode that specifies how the image should be resized
 * (e.g., scale down, contain, cover, crop, etc.).
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails or if the new dimensions exceed limits).
 */
int tiny_image_fit(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode
);

/**
 * @brief Resizes an image to fit within the specified target width and height
 * according to the specified fit mode, with optional padding.
 *
 * The function adjusts the image dimensions based on the selected fit mode,
 * which determines how the image will be scaled, cropped, or padded to fit
 * within the target dimensions while maintaining its aspect ratio. If padding
 * is required, the specified padding color will be used to fill the empty
 * space around the image.
 *
 * @param image Pointer to the TinyImage structure to be resized.
 * @param target_width The desired width of the resized image in pixels.
 * @param target_height The desired height of the resized image in pixels.
 * @param fit_mode The fit mode that specifies how the image should be resized
 * (e.g., scale down, contain, cover, crop, etc.).
 * @param padding_color Pointer to a uint8_t array specifying the color to be
 * used for padding. The size of the array should match the number of channels
 * in the image type. If NULL, no padding will be applied.
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails or if the new dimensions exceed limits).
 */
int tiny_image_fit_with_padding(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, const uint8_t* padding_color
);

/**
 * @brief Resizes an image to fit within the specified target width and height
 * according to the specified fit mode, with optional padding and background
 * color.
 *
 * The function adjusts the image dimensions based on the selected fit mode,
 * which determines how the image will be scaled, cropped, or padded to fit
 * within the target dimensions while maintaining its aspect ratio. If padding
 * is required, the specified padding color will be used to fill the empty
 * space around the image. Additionally, a background color can be specified to
 * fill any remaining areas of the image that are not covered by the original
 * image or padding.
 *
 * @param image Pointer to the TinyImage structure to be resized.
 * @param target_width The desired width of the resized image in pixels.
 * @param target_height The desired height of the resized image in pixels.
 * @param fit_mode The fit mode that specifies how the image should be resized
 * (e.g., scale down, contain, cover, crop, etc.).
 * @param padding_color Pointer to a uint8_t array specifying the color to be
 * used for padding. The size of the array should match the number of channels
 * in the image type. If NULL, no padding will be applied.
 * @param background_color Pointer to a uint8_t array specifying the color to be
 * used for filling any remaining areas of the image. The size of the array
 * should match the number of channels in the image type. If NULL, no background
 * color will be applied.
 * @return int 0 on success, non-zero on failure (e.g., if memory allocation
 * fails or if the new dimensions exceed limits).
 */
int tiny_image_fit_with_padding_and_background(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, const uint8_t* padding_color,
    const uint8_t* background_color
);

/**
 * @brief Fits the image to a target, keeping the part the gravity names.
 *
 * @param image The image, replaced by the result.
 * @param target_width Target width.
 * @param target_height Target height.
 * @param fit_mode How the aspect mismatch is absorbed; see TinyImageFit.
 * @param gravity Which part a crop keeps, or where a pad puts the image.
 * @param background What a pad is filled with, or NULL for the default.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_fit_with_gravity(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, TinyImageGravity gravity, const uint8_t* background
);

/**
 * @brief Applies gamma correction to the image.
 *
 * Gamma correction adjusts the brightness of the image based on a specified
 * gamma value. A gamma value greater than 1.0 will darken the image, while a
 * value less than 1.0 will brighten it. The function modifies the pixel values
 * of the image accordingly.
 *
 * @param image Pointer to the TinyImage structure to be gamma corrected.
 * @param gamma The gamma value to be applied for correction. Must be a positive
 * float greater than 0.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the gamma value is invalid).
 */
int tiny_image_gamma_correction(TinyImage* image, float gamma);

/**
 * @brief Applies a sepia tone effect to the image.
 *
 * The sepia effect gives the image a warm, brownish tone, reminiscent of
 * old photographs. This function modifies the pixel values of the image to
 * achieve the sepia effect.
 *
 * @param image Pointer to the TinyImage structure to be modified with the
 * sepia effect.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_apply_sepia(TinyImage* image);

/**
 * @brief Clears the background, making it transparent.
 *
 * A flood fill seeded from the four corners, so what counts as background is
 * whatever is connected to the edge and close in color to it. That is the
 * difference from clearing every pixel near the background color: a white
 * shirt in the middle of a photograph on a white backdrop stays.
 *
 * The edge of what it clears is feathered by the alpha the match was within,
 * so the cutout has a soft boundary rather than a stair-stepped one.
 *
 * @param image The image to change. Gains an alpha channel if it has none.
 * @param tolerance How far a channel may differ from the seed color and still
 * count as background, 0 through 255.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_remove_background(TinyImage* image, uint8_t tolerance);

/**
 * @brief Zooms in or out of the image by the specified zoom factor.
 *
 * A zoom factor greater than 1.0 will zoom in (enlarge) the image, while a
 * zoom factor less than 1.0 will zoom out (shrink) the image. The function
 * modifies the pixel values of the image accordingly.
 *
 * @param image Pointer to the TinyImage structure to be zoomed.
 * @param zoom_factor The factor by which to zoom the image. Must be a positive
 * float greater than 0.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the zoom factor is invalid).
 */
int tiny_image_zoom(TinyImage* image, float zoom_factor);

/**
 * @brief Darkens a specified rectangular region of the image by the given
 * factor.
 *
 * This function reduces the brightness of the pixels within the specified
 * rectangle, making them darker. The darkening effect is controlled by the
 * factor parameter, where a value less than 1.0 will darken the pixels, and a
 * value greater than 1.0 will have no effect (as it would brighten instead).
 *
 * @param image Pointer to the TinyImage structure to be modified.
 * @param x The x-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param y The y-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param width The width of the rectangle in pixels.
 * @param height The height of the rectangle in pixels.
 * @param factor The factor by which to darken the pixels. Must be a float value
 * between 0.0 and 1.0, where lower values result in darker pixels.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_darken(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float factor
);

/**
 * @brief Lightens a specified rectangular region of the image by the given
 * factor.
 *
 * This function increases the brightness of the pixels within the specified
 * rectangle, making them lighter. The lightening effect is controlled by the
 * factor parameter, where a value greater than 1.0 will lighten the pixels,
 * and a value less than 1.0 will have no effect (as it would darken instead).
 *
 * @param image Pointer to the TinyImage structure to be modified.
 * @param x The x-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param y The y-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param width The width of the rectangle in pixels.
 * @param height The height of the rectangle in pixels.
 * @param factor The factor by which to lighten the pixels. Must be a float
 * value greater than 1.0, where higher values result in lighter pixels.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_lighten(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float factor
);

/**
 * @brief Applies a Gaussian blur effect to the image.
 *
 * This function applies a Gaussian blur to the entire image, softening edges
 * and reducing noise. The amount of blurring is controlled by the sigma
 * parameter, which determines the standard deviation of the Gaussian kernel.
 *
 * @param image Pointer to the TinyImage structure to be blurred.
 * @param sigma The standard deviation of the Gaussian kernel. A higher value
 * results in a stronger blur effect. Must be a positive float greater than 0.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the sigma value is invalid).
 */
int tiny_image_gaussian_blur(TinyImage* image, float sigma);

/**
 * @brief Applies a vignette effect to the image.
 *
 * This function darkens the edges of the image, creating a vignette effect.
 * The radius parameter controls how far the darkening extends from the center
 * of the image, and the strength parameter controls how dark the edges become.
 * The color parameter specifies the color to be used for the vignette effect.
 *
 * @param image Pointer to the TinyImage structure to be modified with the
 * vignette effect.
 * @param radius The radius of the vignette effect, in pixels. Must be a
 * positive float greater than 0.
 * @param strength The strength of the vignette effect, where 0.0 means no
 * effect and 1.0 means full effect. Must be a float value between 0.0 and 1.0.
 * @param color Pointer to a uint8_t array specifying the color to be used for
 * the vignette effect. The size of the array should match the number of
 * channels in the image type (e.g., 3 for RGB, 4 for RGBA).
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if parameters are invalid).
 */
int tiny_image_vignette(
    TinyImage* image, float radius, float strength, const uint8_t* color
);

/**
 * @brief Applies a color overlay to the image with the specified color and
 * opacity.
 *
 * This function overlays a solid color onto the entire image, blending it with
 * the original pixel values based on the specified opacity. The color is
 * provided as an array of uint8_t values, and the opacity is a float value
 * between 0.0 (fully transparent) and 1.0 (fully opaque).
 *
 * @param image Pointer to the TinyImage structure to be modified with the
 * color overlay.
 * @param color Pointer to a uint8_t array specifying the color to be used for
 * the overlay. The size of the array should match the number of channels in
 * the image type (e.g., 3 for RGB, 4 for RGBA).
 * @param opacity The opacity of the overlay, where 0.0 means fully transparent
 * and 1.0 means fully opaque. Must be a float value between 0.0 and 1.0.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if parameters are invalid).
 */
int tiny_image_color_overlay(
    TinyImage* image, const uint8_t* color, float opacity
);

/**
 * @brief Applies a color overlay to a specified rectangular region of the
 * image with the given color and opacity.
 *
 * This function overlays a solid color onto a specified rectangular region of
 * the image, blending it with the original pixel values based on the specified
 * opacity. The rectangle is defined by its top-left corner (x, y) and its
 * width and height. The color is provided as an array of uint8_t values, and
 * the opacity is a float value between 0.0 (fully transparent) and 1.0 (fully
 * opaque).
 *
 * @param image Pointer to the TinyImage structure to be modified with the
 * color overlay.
 * @param x The x-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param y The y-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param width The width of the rectangle in pixels.
 * @param height The height of the rectangle in pixels.
 * @param color Pointer to a uint8_t array specifying the color to be used for
 * the overlay. The size of the array should match the number of channels in
 * the image type (e.g., 3 for RGB, 4 for RGBA).
 * @param opacity The opacity of the overlay, where 0.0 means fully transparent
 * and 1.0 means fully opaque. Must be a float value between 0.0 and 1.0.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if parameters are invalid).
 */
int tiny_image_color_overlay_rect(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* color, float opacity
);

#pragma endregion

#pragma region image effects

/**
 * @brief The forms of color blindness the simulation and the assist cover.
 *
 * The three dichromacies plus total loss. Simulated through the Brettel,
 * Vienot and Mollon projection onto the surviving plane, which is the model
 * the accessibility tooling in browsers uses.
 */
typedef enum TinyColorblindKind
{
    /** No long-wavelength cone; red appears dark. */
    TINYIMG_COLORBLIND_PROTANOPIA = 0,
    /** No medium-wavelength cone, and the most common form. */
    TINYIMG_COLORBLIND_DEUTERANOPIA = 1,
    /** No short-wavelength cone; blue and yellow confuse. */
    TINYIMG_COLORBLIND_TRITANOPIA = 2,
    /** No color at all. */
    TINYIMG_COLORBLIND_ACHROMATOPSIA = 3,
} TinyColorblindKind;

/**
 * @brief The named looks, each a fixed stack of the adjustments above.
 *
 * A preset is worth having because the stack collapses: every one of these is
 * a matrix and a curve by the time it runs, whatever it was written as, so a
 * preset costs the same single pass a brightness change does.
 */
typedef enum TinyImagePreset
{
    /** High contrast monochrome. */
    TINYIMG_PRESET_NOIR = 0,
    /** Monochrome with lifted blacks and a cool cast. */
    TINYIMG_PRESET_CHROME = 1,
    /** Plain monochrome at the original contrast. */
    TINYIMG_PRESET_MONO = 2,
    /** Lifted blacks, pulled highlights, reduced saturation. */
    TINYIMG_PRESET_FADE = 3,
    /** Raised saturation and contrast. */
    TINYIMG_PRESET_VIVID = 4,
    /** Shifted toward amber. */
    TINYIMG_PRESET_WARM = 5,
    /** Shifted toward blue. */
    TINYIMG_PRESET_COOL = 6,
    /** Warm, low contrast and slightly faded, like instant film. */
    TINYIMG_PRESET_INSTANT = 7,
    /** Flattened contrast with the midtones held. */
    TINYIMG_PRESET_TONAL = 8,
} TinyImagePreset;

/**
 * @brief Applies a color matrix of the caller's own.
 *
 * The escape hatch under every named adjustment here. It composes with them,
 * so a caller mixing its own matrix with a saturation change still pays for
 * one pass.
 *
 * @param image The image to change.
 * @param matrix Row major 3x4 applied to RGB in the 0..255 range: three
 * channel weights and a constant per row.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_apply_matrix(TinyImage* image, const float* matrix);

/**
 * @brief Applies a 256 entry table of the caller's own to every channel.
 *
 * @param image The image to change.
 * @param lut The table.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_apply_lut(TinyImage* image, const uint8_t* lut);

/**
 * @brief Applies one table per color channel.
 *
 * @param image The image to change.
 * @param red The red channel's table, or NULL to leave it alone.
 * @param green The green channel's table, or NULL.
 * @param blue The blue channel's table, or NULL.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_apply_luts(
    TinyImage* image, const uint8_t* red, const uint8_t* green,
    const uint8_t* blue
);

/**
 * @brief Applies a tone curve through the caller's control points.
 *
 * The points are interpolated monotonically, so a curve through rising
 * control points never dips between them; a cubic spline through the same
 * points does, and the dip shows as a band in a gradient.
 *
 * @param image The image to change.
 * @param x_points Input levels, 0 through 255, strictly increasing.
 * @param y_points Output levels, 0 through 255.
 * @param num_points How many points; at least two.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when the inputs do not increase,
 * or a negative TinyImageError.
 */
int tiny_image_curves(
    TinyImage* image, const uint8_t* x_points, const uint8_t* y_points,
    size_t num_points
);

/**
 * @brief Subtracts every color channel from full scale.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_negate(TinyImage* image);

/**
 * @brief Replaces every color channel with the pixel's luminance, keeping the
 * channel count.
 *
 * Unlike tiny_image_to_grayscale, which drops the channels, this leaves an RGB
 * image RGB so that a later colorize or duotone has three channels to work
 * with.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_blackwhite(TinyImage* image);

/**
 * @brief Mixes every pixel toward one color, keeping its luminance.
 *
 * @param image The image to change.
 * @param color The target color, three channels.
 * @param strength 0 changes nothing, 1 replaces the hue entirely.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_colorize(TinyImage* image, const uint8_t* color, float strength);

/**
 * @brief Adds a color cast without touching the luminance.
 *
 * @param image The image to change.
 * @param color The cast, three channels.
 * @param strength 0 changes nothing.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_tint(TinyImage* image, const uint8_t* color, float strength);

/**
 * @brief Rounds every channel to a number of evenly spaced levels.
 *
 * @param image The image to change.
 * @param levels How many, 2 through 256.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_posterize(TinyImage* image, uint32_t levels);

/**
 * @brief Drives every channel to nothing or to full scale.
 *
 * @param image The image to change.
 * @param level Where the split falls, 0 through 255.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_threshold(TinyImage* image, uint8_t level);

/**
 * @brief Inverts only the channels above a level.
 *
 * @param image The image to change.
 * @param level Where the inversion begins.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_solarize(TinyImage* image, uint8_t level);

/**
 * @brief Maps the tonal range between two colors.
 *
 * @param image The image to change.
 * @param shadow The color black becomes, three channels.
 * @param highlight The color white becomes.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_duotone(
    TinyImage* image, const uint8_t* shadow, const uint8_t* highlight
);

/**
 * @brief Tints the shadows and the highlights differently.
 *
 * @param image The image to change.
 * @param shadow The shadows' cast, three channels.
 * @param highlight The highlights' cast.
 * @param balance Where the two meet, 0 through 1; 0.5 is mid gray.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_split_tone(
    TinyImage* image, const uint8_t* shadow, const uint8_t* highlight,
    float balance
);

/**
 * @brief Scales every channel by a power of two.
 *
 * @param image The image to change.
 * @param stops Positive brightens; 0 changes nothing.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_exposure(TinyImage* image, float stops);

/**
 * @brief Lifts the shadows without moving the highlights.
 *
 * @param image The image to change.
 * @param amount 0 through 1.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_fill_light(TinyImage* image, float amount);

/**
 * @brief Shifts the white point along the blue to amber axis.
 *
 * @param image The image to change.
 * @param amount -1 is fully cool, 1 fully warm, 0 changes nothing.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_temperature(TinyImage* image, float amount);

/**
 * @brief Shifts the white point along both axes at once.
 *
 * @param image The image to change.
 * @param temperature -1 cool through 1 warm.
 * @param tint -1 green through 1 magenta.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_white_balance(TinyImage* image, float temperature, float tint);

/**
 * @brief Raises saturation, and least where it is already high.
 *
 * @param image The image to change.
 * @param amount 0 changes nothing.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_vibrance(TinyImage* image, float amount);

/**
 * @brief Maps an input range onto an output range through a gamma.
 *
 * @param image The image to change.
 * @param in_black Input level that becomes `out_black`.
 * @param in_white Input level that becomes `out_white`; above `in_black`.
 * @param gamma Applied between the two; 1 is linear.
 * @param out_black The output floor.
 * @param out_white The output ceiling.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_levels(
    TinyImage* image, float in_black, float in_white, float gamma,
    float out_black, float out_white
);

/**
 * @brief The same, on one channel.
 *
 * @param image The image to change.
 * @param channel 0 red, 1 green, 2 blue.
 * @param in_black Input level that becomes `out_black`.
 * @param in_white Input level that becomes `out_white`.
 * @param gamma Applied between the two.
 * @param out_black The output floor.
 * @param out_white The output ceiling.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_levels_channel(
    TinyImage* image, uint8_t channel, float in_black, float in_white,
    float gamma, float out_black, float out_white
);

/**
 * @brief Shifts the color of the shadows, midtones and highlights apart.
 *
 * @param image The image to change.
 * @param shadows Three shifts, -1 through 1, for the dark end.
 * @param midtones Three shifts for the middle.
 * @param highlights Three shifts for the light end.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_color_balance(
    TinyImage* image, const float* shadows, const float* midtones,
    const float* highlights
);

/**
 * @brief Rebuilds each output channel from a weighted sum of the inputs.
 *
 * @param image The image to change.
 * @param matrix Nine weights, row major; the identity changes nothing.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_channel_mixer(TinyImage* image, const float* matrix);

/**
 * @brief Scales each channel independently.
 *
 * @param image The image to change.
 * @param red Factor for red.
 * @param green Factor for green.
 * @param blue Factor for blue.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_channel_gain(
    TinyImage* image, float red, float green, float blue
);

/**
 * @brief Shows the image as a given color blindness would see it.
 *
 * @param image The image to change.
 * @param kind Which form.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_colorblind_simulate(TinyImage* image, TinyColorblindKind kind);

/**
 * @brief Moves the colors a given color blindness cannot separate apart.
 *
 * The error the simulation discards, added back along the axes that form can
 * still see. The result is not the original colors and is not meant to be;
 * it is an image whose distinctions survive the viewer's own projection.
 *
 * @param image The image to change.
 * @param kind Which form.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_colorblind_assist(TinyImage* image, TinyColorblindKind kind);

/**
 * @brief Applies a named look.
 *
 * @param image The image to change.
 * @param preset Which look.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_preset(TinyImage* image, TinyImagePreset preset);

#pragma endregion

#pragma region image spatial effects

/**
 * @brief Adds back a multiple of what a blur removed.
 *
 * @param image The image to change.
 * @param sigma The blur's standard deviation.
 * @param amount How much of the difference to add back.
 * @param threshold Differences at or below this are left alone, which keeps
 * flat areas from gaining noise.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_unsharp_mask(
    TinyImage* image, float sigma, float amount, float threshold
);

/**
 * @brief Raises local contrast, which is an unsharp mask at a large radius.
 *
 * @param image The image to change.
 * @param amount 0 changes nothing.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_clarity(TinyImage* image, float amount);

/**
 * @brief Replaces the image with its Sobel gradient magnitude.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_sobel(TinyImage* image);

/**
 * @brief Turns the image into a relief lit from the upper left.
 *
 * @param image The image to change.
 * @param strength Added to every output; 128 keeps a flat area mid gray.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_emboss(TinyImage* image, float strength);

/**
 * @brief Averages the image over square blocks.
 *
 * @param image The image to change.
 * @param size Block size in pixels; below two changes nothing.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_pixelate(TinyImage* image, uint32_t size);

/**
 * @brief The same, inside a rectangle.
 *
 * @param image The image to change.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width; zero means to the right edge.
 * @param height Height; zero means to the bottom edge.
 * @param size Block size in pixels.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_pixelate_region(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    uint32_t size
);

/**
 * @brief Replaces each pixel with the median of its 3x3 neighborhood.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_despeckle(TinyImage* image);

/**
 * @brief Replaces each pixel with the brightest in a radius.
 *
 * @param image The image to change.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_dilate(TinyImage* image, uint32_t radius);

/**
 * @brief Replaces each pixel with the darkest in a radius.
 *
 * @param image The image to change.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_erode(TinyImage* image, uint32_t radius);

/**
 * @brief Erodes then dilates, which removes bright specks.
 *
 * @param image The image to change.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_morphology_open(TinyImage* image, uint32_t radius);

/**
 * @brief Dilates then erodes, which fills dark specks.
 *
 * @param image The image to change.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_morphology_close(TinyImage* image, uint32_t radius);

/**
 * @brief The difference between a dilation and an erosion.
 *
 * @param image The image to change.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_outline(TinyImage* image, uint32_t radius);

/**
 * @brief Averages along a straight path.
 *
 * @param image The image to change.
 * @param length How far, in pixels.
 * @param angle Degrees clockwise from the positive x axis.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_motion_blur(TinyImage* image, float length, float angle);

/**
 * @brief Averages along arcs about the center.
 *
 * @param image The image to change.
 * @param degrees How far around.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_radial_blur(TinyImage* image, float degrees);

/**
 * @brief Averages along rays from the center.
 *
 * @param image The image to change.
 * @param strength How far, as a percentage of the distance from the center.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_zoom_blur(TinyImage* image, float strength);

/**
 * @brief Blurs away from a horizontal band left sharp.
 *
 * @param image The image to change.
 * @param sigma The blur at the furthest row.
 * @param band How much of the height stays sharp, 0 through 1.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_tilt_shift(TinyImage* image, float sigma, float band);

/**
 * @brief Blurs inside a rectangle only.
 *
 * @param image The image to change.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width; zero means to the right edge.
 * @param height Height; zero means to the bottom edge.
 * @param sigma The blur's standard deviation.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_blur_region(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float sigma
);

/**
 * @brief Grows the image and puts a blurred silhouette of it behind.
 *
 * The shadow is cast by the alpha channel, so an image with none casts a
 * rectangle. The extent grows by whatever the offset and the blur need, so
 * nothing is clipped.
 *
 * @param image The image, replaced by the larger one.
 * @param offset_x How far right the shadow falls.
 * @param offset_y How far down.
 * @param sigma How soft.
 * @param color The shadow's color, as many channels as the result has.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_drop_shadow(
    TinyImage* image, int32_t offset_x, int32_t offset_y, float sigma,
    const uint8_t* color
);

/**
 * @brief Adds a blurred copy of the image to itself, which is a bloom.
 *
 * @param image The image to change.
 * @param sigma How wide the glow spreads.
 * @param strength How much of the blurred copy to add.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_glow(TinyImage* image, float sigma, float strength);

/**
 * @brief Adds pseudorandom noise.
 *
 * The generator is a counter hashed per pixel, so the same request over the
 * same image gives the same noise. A generator carrying state between calls
 * would not, and a caller comparing two runs would see a difference that is
 * not in the request.
 *
 * @param image The image to change.
 * @param amount Standard deviation in levels.
 * @param monochrome Non-zero to add the same value to every channel.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_noise(TinyImage* image, float amount, int monochrome);

/**
 * @brief Adds noise weighted toward the midtones, which is how film grains.
 *
 * @param image The image to change.
 * @param amount Standard deviation in levels at mid gray.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_film_grain(TinyImage* image, float amount);

/**
 * @brief Quantizes through an ordered threshold matrix.
 *
 * @param image The image to change.
 * @param levels How many output levels per channel.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_dither(TinyImage* image, uint32_t levels);

/**
 * @brief Turns tone into dot area within a cell.
 *
 * @param image The image to change.
 * @param cell Cell size in pixels.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_halftone(TinyImage* image, uint32_t cell);

/**
 * @brief Offsets the red and blue channels radially.
 *
 * @param image The image to change.
 * @param amount Pixels of separation at the corner.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_chromatic_aberration(TinyImage* image, float amount);

/**
 * @brief Darkens every nth row.
 *
 * @param image The image to change.
 * @param period How many rows between darkened ones.
 * @param strength How much darker, 0 through 1.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_scanlines(TinyImage* image, uint32_t period, float strength);

#pragma endregion

#pragma region image auto correction

/**
 * @brief Moves the mean luminance to the middle of the range.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_auto_brightness(TinyImage* image);

/**
 * @brief Stretches the luminance so that a small fraction clips at each end.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_auto_contrast(TinyImage* image);

/**
 * @brief Scales each channel so their means agree, which is a gray-world
 * balance.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_auto_color(TinyImage* image);

/**
 * @brief Stretches each channel to the full range independently.
 *
 * Unlike tiny_image_auto_contrast this changes the color balance as well,
 * because a channel with a narrow range is stretched further than one with a
 * wide one.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_auto_levels(TinyImage* image);

/**
 * @brief Applies the gamma that brings the mean luminance to mid gray.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_auto_gamma(TinyImage* image);

/**
 * @brief Applies the auto corrections a photograph usually wants together.
 *
 * Levels, then color, then a small saturation lift. All three collapse, so it
 * costs one pass.
 *
 * @param image The image to change.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_improve(TinyImage* image);

/**
 * @brief Recovers detail at both ends of the range.
 *
 * @param image The image to change.
 * @param shadows How much to lift the dark end, 0 through 1.
 * @param highlights How much to pull the light end down, 0 through 1.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_shadows_highlights(
    TinyImage* image, float shadows, float highlights
);

/**
 * @brief Removes a veiling haze by the dark-channel prior.
 *
 * @param image The image to change.
 * @param strength How much to remove, 0 through 1.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_dehaze(TinyImage* image, float strength);

#pragma endregion

#pragma region image warps

/**
 * @brief Slants the image along one or both axes.
 *
 * The extent grows to hold the result, so nothing is cut off.
 *
 * @param image The image, replaced by the result.
 * @param shear_x How far each row moves per row down.
 * @param shear_y How far each column moves per column right.
 * @param background What the corners are filled with, or NULL for
 * transparent.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_shear(
    TinyImage* image, float shear_x, float shear_y, const uint8_t* background
);

/**
 * @brief Turns the image by any angle.
 *
 * The extent grows to hold the turned image, so a 45 degree turn of a square
 * is a larger square with the original standing on a corner.
 *
 * @param image The image, replaced by the result.
 * @param degrees Clockwise. A multiple of 90 goes through the exact kernel and
 * loses nothing.
 * @param background What the corners are filled with, or NULL for
 * transparent.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_rotate(
    TinyImage* image, float degrees, const uint8_t* background
);

/**
 * @brief Maps the image's four corners onto four arbitrary points.
 *
 * @param image The image, replaced by the result.
 * @param quad Eight numbers: the x and y of the destination for the top left,
 * top right, bottom right and bottom left corners, in that order.
 * @param background What the uncovered pixels are filled with, or NULL for
 * transparent.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE when the quad is degenerate, or a
 * negative TinyImageError.
 */
int tiny_image_perspective(
    TinyImage* image, const float* quad, const uint8_t* background
);

/**
 * @brief Bends the image along an arc.
 *
 * @param image The image, replaced by the result.
 * @param degrees How far around; positive bows the top upward.
 * @param background What the uncovered pixels are filled with, or NULL.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_arc(TinyImage* image, float degrees, const uint8_t* background);

/**
 * @brief Corrects or introduces lens distortion.
 *
 * @param image The image to change.
 * @param amount Positive corrects a barrel and so introduces a pincushion;
 * negative does the reverse. 0 changes nothing.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_barrel(TinyImage* image, float amount);

/**
 * @brief Twists the image about its center, most at the center.
 *
 * @param image The image to change.
 * @param degrees How far the center turns.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_swirl(TinyImage* image, float degrees);

/**
 * @brief Maps between rectangular and polar coordinates.
 *
 * @param image The image to change.
 * @param inverse Zero maps the image onto a disc; non-zero unrolls a disc into
 * a rectangle.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_polar(TinyImage* image, int inverse);

/**
 * @brief Rounds the image's corners by clearing what falls outside them.
 *
 * @param image The image to change. Gains an alpha channel if it has none.
 * @param radius Corner radius in pixels.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_corner_radius(TinyImage* image, uint32_t radius);

#pragma endregion

#pragma region image analysis

/**
 * @brief Counts how many pixels fall in each of 256 buckets.
 *
 * @param image The image to read.
 * @param channel Which channel, or 255 for the luminance.
 * @param bins Receives 256 counts.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE for a channel the image has not
 * got, or a negative TinyImageError.
 */
int tiny_image_histogram(
    const TinyImage* image, uint8_t channel, uint32_t* bins
);

/**
 * @brief The color the most pixels are closest to.
 *
 * Found by clustering rather than by the most common exact value, which on a
 * photograph is almost always a color that appears a handful of times.
 *
 * @param image The image to read.
 * @param color Receives as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_dominant_color(const TinyImage* image, uint8_t* color);

/**
 * @brief The colors a palette of the given size would hold.
 *
 * @param image The image to read.
 * @param count How many colors to find, 1 through 256.
 * @param palette Receives `count` entries of as many channels as the image
 * has.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_image_palette(
    const TinyImage* image, uint32_t count, uint8_t* palette
);

/**
 * @brief The mean of every pixel, per channel.
 *
 * @param image The image to read.
 * @param color Receives as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_average_color(const TinyImage* image, uint8_t* color);

/**
 * @brief A 64 bit perceptual hash.
 *
 * The discrete cosine transform of a 32x32 luminance reduction, thresholded at
 * the median of its low frequency block. Two images a viewer would call the
 * same differ in few bits; two unrelated ones differ in about half.
 *
 * @param image The image to read.
 * @param hash Receives the hash, most significant bit first.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_phash(const TinyImage* image, uint64_t* hash);

/**
 * @brief How many bits two perceptual hashes differ in.
 *
 * @param first One hash.
 * @param second The other.
 * @return uint32_t The count, 0 through 64.
 */
uint32_t tiny_phash_distance(uint64_t first, uint64_t second);

/**
 * @brief Where the part of an image worth keeping is.
 *
 * What the two computed gravities resolve to, and useful on its own: a caller
 * cropping by hand wants the same answer the planner would have used.
 *
 * TINYIMG_GRAVITY_AUTO weights every tile of the image by how much local
 * detail it holds and returns the centroid, so a photograph with one sharp
 * subject on a soft background focuses on the subject.
 * TINYIMG_GRAVITY_FACE runs tiny_image_detect_faces and returns the center of
 * the detections, weighted by how confident each one is, and falls back to
 * TINYIMG_GRAVITY_AUTO when no cascade is loaded or nothing was found. Every
 * fixed gravity is arithmetic and reads no pixels.
 *
 * @param image The image to read.
 * @param gravity Which question to ask.
 * @param x Receives the horizontal position, 0 through 1 across the width.
 * @param y Receives the vertical position.
 * @return int TINYIMG_OK, or a negative TinyImageError. A detector that finds
 * nothing is not a failure; it reports TINYIMG_OK with the fallback.
 */
int tiny_image_focus(
    const TinyImage* image, TinyImageGravity gravity, float* x, float* y
);

/**
 * @brief Blurs whatever the detector finds and nothing else.
 *
 * A no-op when no cascade is loaded or no face is found, rather than a blurred
 * image: an anonymizer that blurs the whole photograph when it fails is worse
 * than one that does nothing, because the failure is invisible in the output of
 * the first and obvious in the second.
 *
 * @param image The image to change.
 * @param sigma Blur radius, in pixels of the image. Zero reads as a twelfth of
 * the face's width, which stays proportionate across sizes.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_blur_faces(TinyImage* image, float sigma);

/**
 * @brief Pixelates whatever the detector finds and nothing else.
 *
 * A no-op when nothing is found, for the reason given on
 * tiny_image_blur_faces.
 *
 * @param image The image to change.
 * @param size Block size in pixels. Zero reads as a twelfth of the face's
 * width.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_pixelate_faces(TinyImage* image, uint32_t size);

#pragma endregion

#pragma region image metadata

/**
 * @brief Sets the EXIF metadata for the image.
 *
 * This function allows you to set the EXIF metadata for the image, which can
 * include information such as camera settings, date and time, GPS location,
 * and more. The EXIF data is provided as a byte array, and the size of the
 * data must be specified.
 *
 * @param image Pointer to the TinyImage structure for which to set the EXIF
 * metadata.
 * @param exif_data Pointer to a byte array containing the EXIF metadata to be
 * set for the image.
 * @param exif_size The size of the EXIF data in bytes.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the EXIF data is invalid).
 */
int tiny_image_set_exif(
    TinyImage* image, const char* exif_data, size_t exif_size
);

/**
 * @brief Retrieves the EXIF metadata from the image.
 *
 * This function allows you to retrieve the EXIF metadata from the image, which
 * can include information such as camera settings, date and time, GPS
 * location, and more. The EXIF data is returned as a byte array, and the size
 * of the data is also provided.
 *
 * @param image Pointer to the TinyImage structure from which to retrieve the
 * EXIF metadata.
 * @param exif_data Pointer to a pointer that will be set to point to the
 * retrieved EXIF metadata. The caller is responsible for freeing this memory.
 * @param exif_size Pointer to a size_t variable that will be set to the size of
 * the retrieved EXIF data in bytes.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if there is no EXIF data).
 */
int tiny_image_get_exif(
    const TinyImage* image, char** exif_data, size_t* exif_size
);

/**
 * @brief Checks if the image has EXIF metadata.
 *
 * This function checks whether the image contains any EXIF metadata. It
 * returns a non-zero value if EXIF data is present, and zero if no EXIF data
 * is found.
 *
 * @param image Pointer to the TinyImage structure to be checked for EXIF
 * metadata.
 * @return int Non-zero if EXIF data is present, zero if no EXIF data is found,
 * or a negative value on error (e.g., if the image is NULL).
 */
int tiny_image_has_exif(const TinyImage* image);

/**
 * @brief Strips the EXIF metadata from the image.
 *
 * This function removes any existing EXIF metadata from the image, effectively
 * clearing any camera settings, date and time, GPS location, and other
 * information that may have been stored in the EXIF data.
 *
 * @param image Pointer to the TinyImage structure from which to strip the EXIF
 * metadata.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_strip_exif(TinyImage* image);

/**
 * @brief Sets a custom metadata key-value pair for the image.
 *
 * This function allows you to set a custom metadata key-value pair for the
 * image. The key and value are provided as strings, and they can be used to
 * store additional information about the image that is not covered by standard
 * EXIF metadata.
 *
 * @param image Pointer to the TinyImage structure for which to set the custom
 * metadata.
 * @param key The key for the custom metadata (e.g., "Author", "Description").
 * @param value The value associated with the specified key.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the key or value is invalid).
 */
int tiny_image_set_metadata(
    TinyImage* image, const char* key, const char* value
);

/**
 * @brief Retrieves the value associated with a custom metadata key from the
 * image.
 *
 * This function allows you to retrieve the value associated with a custom
 * metadata key from the image. The key is provided as a string, and the
 * corresponding value is returned as a string. The caller is responsible for
 * freeing the memory allocated for the value.
 *
 * @param image Pointer to the TinyImage structure from which to retrieve the
 * custom metadata.
 * @param key The key for the custom metadata whose value is to be retrieved.
 * @param value Pointer to a pointer that will be set to point to the retrieved
 * value. The caller is responsible for freeing this memory.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the key does not exist).
 */
int tiny_image_get_metadata(
    const TinyImage* image, const char* key, char** value
);

/**
 * @brief Checks if the image has a specific custom metadata key.
 *
 * This function checks whether the image contains a specific custom metadata
 * key. It returns a non-zero value if the key is present, and zero if the key
 * is not found.
 *
 * @param image Pointer to the TinyImage structure to be checked for the custom
 * metadata key.
 * @param key The key for the custom metadata to check for.
 * @return int Non-zero if the key is present, zero if the key is not found, or
 * a negative value on error (e.g., if the image is NULL).
 */
int tiny_image_has_metadata(const TinyImage* image, const char* key);

/**
 * @brief Removes a specific custom metadata key-value pair from the image.
 *
 * This function allows you to remove a specific custom metadata key-value
 * pair from the image. The key is provided as a string, and if the key exists,
 * it will be removed along with its associated value.
 *
 * @param image Pointer to the TinyImage structure from which to remove the
 * custom metadata.
 * @param key The key for the custom metadata to be removed.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the key does not exist).
 */
int tiny_image_remove_metadata(TinyImage* image, const char* key);

/**
 * @brief Retrieves the count of custom metadata key-value pairs in the image.
 *
 * @param image Pointer to the TinyImage structure from which to retrieve the
 * metadata count.
 * @param count Pointer to a size_t variable that will be set to the number of
 * custom metadata key-value pairs in the image.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the count pointer is NULL).
 */
int tiny_image_get_metadata_count(const TinyImage* image, size_t* count);

#pragma endregion

#pragma region image conversion

/**
 * @brief Converts the image to RGB format.
 *
 * @param image Pointer to the TinyImage structure to be converted to RGB.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL).
 */
int tiny_image_to_rgb(TinyImage* image);

/**
 * @brief Converts the image to RGBA format, adding an opaque alpha channel if
 * there was none.
 *
 * @param image Pointer to the TinyImage structure to be converted to RGBA.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_to_rgba(TinyImage* image);

/**
 * @brief Converts the image to grayscale.
 *
 * Luminance is weighted by Rec. 709, matching what a browser's grayscale filter
 * produces.
 *
 * @param image Pointer to the TinyImage structure to be converted to
 * grayscale.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_to_grayscale(TinyImage* image);

/**
 * @brief Converts the image to a given channel count.
 *
 * Widening allocates; narrowing rewrites in place, so dropping a channel never
 * needs room for two copies of a large image. Dropping alpha discards it rather
 * than compositing; tiny_image_set_transparent is the call that composites onto
 * a background.
 *
 * @param image Pointer to the TinyImage structure to convert.
 * @param channels Target channels per pixel, 1 through 4.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a channel
 * count outside 1 through 4, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_convert_channels(TinyImage* image, uint8_t channels);

/**
 * @brief Records the format the image should be written as, dropping anything
 * that format cannot carry.
 *
 * No pixels are re-encoded here; tiny_image_encode does that. Asking for JPEG
 * flattens the alpha channel, because JPEG has nowhere to put it, and every
 * other format keeps the pixels as they are.
 *
 * @param image Pointer to the TinyImage structure to convert.
 * @param format The format to convert the image to (e.g., TINYIMG_FORMAT_PNG,
 * TINYIMG_FORMAT_JPEG).
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE for an unknown format, or a
 * negative TinyImageError.
 */
int tiny_image_convert(TinyImage* image, TinyImageFormat format);

/**
 * @brief Checks if the image has an alpha channel (transparency).
 *
 * This function checks whether the image contains an alpha channel, which
 * indicates that the image supports transparency. It returns a non-zero value
 * if the image has an alpha channel, and zero if it does not.
 *
 * @param image Pointer to the TinyImage structure to be checked for an alpha
 * channel.
 * @return int Non-zero if the image has an alpha channel, zero if it does not,
 * or a negative value on error (e.g., if the image is NULL).
 */
int tiny_image_istransparent(const TinyImage* image);

/**
 * @brief Sets the transparency of the image.
 *
 * This function enables or disables transparency for the image. If
 * enable_transparency is non-zero, the image will be set to support
 * transparency (if applicable). If enable_transparency is zero, the image
 * will be set to not support transparency.
 *
 * Setting a non-transparent image to support transparency may result in the
 * addition of an alpha channel, which can increase the image's memory usage and
 * file size. Setting a transparent image to not support transparency may result
 * in the removal of the alpha channel and the background being filled with
 * white or black, depending on the image format and implementation.
 *
 * @param image Pointer to the TinyImage structure for which to set
 * transparency.
 * @param enable_transparency Non-zero to enable transparency, zero to disable
 * it.
 */
int tiny_image_set_transparent(TinyImage* image, int enable_transparency);

/**
 * @brief Retrieves the format of the image.
 *
 * @param image Pointer to the TinyImage structure from which to retrieve the
 * format.
 * @return TinyImageFormat The format of the image.
 */
TinyImageFormat tiny_image_getformat(const TinyImage* image);

#pragma endregion

#ifdef __cplusplus
}
#endif
