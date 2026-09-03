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
 * Filled by tiny_image_probe for every format the library recognises, including
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
    /** Bits per channel in the file, before decoding normalises them to 8. */
    uint8_t bit_depth;
    /** Non-zero when the file carries transparency. */
    uint8_t has_alpha;
    /** Non-zero for a progressive JPEG, which cannot stream a region. */
    uint8_t progressive;
} TinyImageInfo;

/**
 * @brief Options a decoder honours when only part of an image is wanted.
 *
 * The rectangle is in source pixels and is clamped to the image. A width or
 * height of zero means the full extent in that axis. `scale_den` then
 * subsamples that rectangle, so the decoded image is ceil(width / scale_den)
 * across.
 *
 * Every codec box averages when downscaling, so a scaled decode is a real
 * reduction rather than a nearest neighbour pick. JPEG additionally does the
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
} TinyDecodeOpts;

/**
 * @brief Options an encoder honours.
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
 * count, TINYIMG_ERR_TOO_LARGE past TINYIMG_MAX_PIXELS, or TINYIMG_ERR_MEMORY.
 */
int tiny_image_create(
    TinyImage* image, uint32_t width, uint32_t height, uint8_t channels
);

/**
 * @brief Reads an image's header without decoding any pixels.
 *
 * The cheapest of the four ways in. Answers for every format the library
 * recognises, including AVIF and HEIF, which it can describe but not decode.
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
 * TINYIMG_MAX_PIXELS reports TINYIMG_ERR_TOO_LARGE rather than
 * TINYIMG_ERR_MEMORY, because the remedy is tiny_image_load_scaled.
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
 * @param writer An initialised TinyWriter to append to.
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
 * Recognises more formats than the library can decode, so a caller can tell an
 * unsupported format apart from an unrecognisable one.
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
 * @brief Draws a horizontal line between two points (x1, y1) and (x2, y2) in
 * the image.
 *
 * @param image Pointer to the TinyImage structure where the line will be drawn.
 * @param x1 The x-coordinate of the starting point of the line (0-based index).
 * @param y1 The y-coordinate of the starting point of the line (0-based index).
 * @param x2 The x-coordinate of the ending point of the line (0-based index).
 * @param y2 The y-coordinate of the ending point of the line (0-based index).
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the line. The size of the array should match the number of
 * channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_hline(
    TinyImage* image, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
    const uint8_t* pixel
);

/**
 * @brief Draws a vertical line between two points (x1, y1) and (x2, y2) in the
 * image.
 *
 * @param image Pointer to the TinyImage structure where the line will be drawn.
 * @param x1 The x-coordinate of the starting point of the line (0-based index).
 * @param y1 The y-coordinate of the starting point of the line (0-based index).
 * @param x2 The x-coordinate of the ending point of the line (0-based index).
 * @param y2 The y-coordinate of the ending point of the line (0-based index).
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the line. The size of the array should match the number of
 * channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_vline(
    TinyImage* image, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
    const uint8_t* pixel
);

/**
 * @brief Draws a rectangle in the image with the specified top-left corner
 * (x, y), width, height, and pixel value.
 *
 * @param image Pointer to the TinyImage structure where the rectangle will be
 * drawn.
 * @param x The x-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param y The y-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param width The width of the rectangle in pixels.
 * @param height The height of the rectangle in pixels.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the rectangle. The size of the array should match the
 * number of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_rectangle(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
);

/**
 * @brief Fills a rectangle in the image with the specified top-left corner
 * (x, y), width, height, and pixel value.
 *
 * @param image Pointer to the TinyImage structure where the rectangle will be
 * filled.
 * @param x The x-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param y The y-coordinate of the top-left corner of the rectangle (0-based
 * index).
 * @param width The width of the rectangle in pixels.
 * @param height The height of the rectangle in pixels.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for filling the rectangle. The size of the array should match the
 * number of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_fill_rectangle(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
);

/**
 * @brief Draws a line between two points (x1, y1) and (x2, y2) in the image
 * using Bresenham's line algorithm.
 *
 * @param image Pointer to the TinyImage structure where the line will be drawn.
 * @param x1 The x-coordinate of the starting point of the line (0-based index).
 * @param y1 The y-coordinate of the starting point of the line (0-based index).
 * @param x2 The x-coordinate of the ending point of the line (0-based index).
 * @param y2 The y-coordinate of the ending point of the line (0-based index).
 * @param thickness The thickness of the line in pixels. A thickness of 1 means
 * a single pixel line, while higher values will create a thicker line.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the line. The size of the array should match the number of
 * channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_draw_line(
    TinyImage* image, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
    uint32_t thickness, const uint8_t* pixel
);

/**
 * @brief Draws a circle in the image with the specified center (center_x,
 * center_y), radius, and pixel value using the Midpoint Circle Algorithm.
 *
 * @param image Pointer to the TinyImage structure where the circle will be
 * drawn.
 * @param center_x The x-coordinate of the center of the circle (0-based index).
 * @param center_y The y-coordinate of the center of the circle (0-based index).
 * @param radius The radius of the circle in pixels.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the circle. The size of the array should match the number
 * of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_draw_circle(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius,
    const uint8_t* pixel
);

/**
 * @brief Fills a circle in the image with the specified center (center_x,
 * center_y), radius, and pixel value using the Midpoint Circle Algorithm.
 *
 * @param image Pointer to the TinyImage structure where the circle will be
 * filled.
 * @param center_x The x-coordinate of the center of the circle (0-based index).
 * @param center_y The y-coordinate of the center of the circle (0-based index).
 * @param radius The radius of the circle in pixels.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for filling the circle. The size of the array should match the number
 * of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds or if the image is NULL).
 */
int tiny_image_fill_circle(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius,
    const uint8_t* pixel
);

/**
 * @brief Draws a polygon in the image defined by a series of points (x_points,
 * y_points) with the specified pixel value.
 *
 * @param image Pointer to the TinyImage structure where the polygon will be
 * drawn.
 * @param x_points Pointer to an array of x-coordinates for the polygon's
 * vertices (0-based indices).
 * @param y_points Pointer to an array of y-coordinates for the polygon's
 * vertices (0-based indices).
 * @param num_points The number of points (vertices) in the polygon. This
 * should match the lengths of the x_points and y_points arrays.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for drawing the polygon. The size of the array should match the number
 * of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds, if num_points is less than 3, or if the image is NULL).
 */
int tiny_image_polygon(
    TinyImage* image, const uint32_t* x_points, const uint32_t* y_points,
    size_t num_points, const uint8_t* pixel
);

/**
 * @brief Fills a polygon in the image defined by a series of points (x_points,
 * y_points) with the specified pixel value.
 *
 * @param image Pointer to the TinyImage structure where the polygon will be
 * filled.
 * @param x_points Pointer to an array of x-coordinates for the polygon's
 * vertices (0-based indices).
 * @param y_points Pointer to an array of y-coordinates for the polygon's
 * vertices (0-based indices).
 * @param num_points The number of points (vertices) in the polygon. This
 * should match the lengths of the x_points and y_points arrays.
 * @param pixel Pointer to a uint8_t array containing the pixel value to be
 * used for filling the polygon. The size of the array should match the number
 * of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if coordinates are out
 * of bounds, if num_points is less than 3, or if the image is NULL).
 */
int tiny_image_fill_polygon(
    TinyImage* image, const uint32_t* x_points, const uint32_t* y_points,
    size_t num_points, const uint8_t* pixel
);

/**
 * @brief Replaces all occurrences of a specific color in the image with a new
 * color.
 *
 * @param image Pointer to the TinyImage structure where the color replacement
 * will occur.
 * @param old_color Pointer to a uint8_t array containing the color to be
 * replaced. The size of the array should match the number of channels in the
 * image type.
 * @param new_color Pointer to a uint8_t array containing the new color to be
 * used for replacement. The size of the array should match the number of
 * channels in the image type.
 * @param tolerance Pointer to a uint8_t array specifying the tolerance for
 * color matching. Each channel's value in the old_color can vary by the
 * corresponding value in the tolerance array for a pixel to be considered a
 * match. The size of the array should match the number of channels in the image
 * type.
 * @return 0 on success, non-zero on failure (e.g., if the image is NULL or if
 * the colors are out of range).
 */
int tiny_image_replace_color(
    TinyImage* image, const uint8_t* old_color, const uint8_t* new_color,
    const uint8_t* tolerance
);

/**
 * @brief Draws one image onto another at the specified (x, y) coordinates.
 *
 * The function copies the pixel data from the source image to the destination
 * image, starting at the specified coordinates. If the source image exceeds
 * the bounds of the destination image, only the overlapping portion will be
 * drawn.
 *
 * @param dest_image Pointer to the TinyImage structure where the source image
 * will be drawn.
 * @param src_image Pointer to the TinyImage structure that will be drawn onto
 * the destination image.
 * @param x The x-coordinate in the destination image where the top-left corner
 * of the source image will be placed (0-based index).
 * @param y The y-coordinate in the destination image where the top-left corner
 * of the source image will be placed (0-based index).
 * @return int 0 on success, non-zero on failure (e.g., if either image is NULL
 * or if coordinates are out of bounds).
 */
int tiny_image_draw_image(
    TinyImage* dest_image, const TinyImage* src_image, uint32_t x, uint32_t y
);

/**
 * @brief Adds a border to the image.
 *
 * The function adds a border of the specified width and color to the image.
 * The border will be added to all sides of the image. If the image is
 * transparent, the border will be traced around the non-transparent pixels.
 *
 * @param image Pointer to the TinyImage structure to which the border will be
 * added.
 * @param border_width The width of the border in pixels.
 * @param pixel Pointer to a uint8_t array containing the color of the border.
 * The size of the array should match the number of channels in the image type.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if the border width is invalid).
 */
int tiny_image_border(
    TinyImage* image, uint32_t border_width, const uint8_t* pixel
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
 * @brief Removes the background from the image, making it transparent.
 *
 * This function analyzes the image and removes the background based on color
 * similarity or other criteria, resulting in a transparent background. The
 * function modifies the pixel values of the image accordingly.
 *
 * @param image Pointer to the TinyImage structure from which to remove the
 * background.
 * @return int 0 on success, non-zero on failure (e.g., if the image is NULL or
 * if background removal fails).
 */
int tiny_image_remove_background(TinyImage* image);

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
