#include "tinyimg/image.h"

#include "tinyimg/codec/codec.h"
#include "tinyimg/detect.h"
#include "tinyimg/memory.h"
#include "tinyimg/plan.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"
#include "tinyimg/work.h"

#pragma region image handling

TINYIMG_EXPORT("tiny_image_sizeof")
uint32_t tiny_image_sizeof(void) {
    return (uint32_t) sizeof(TinyImage);
}

TINYIMG_EXPORT("tiny_image_create")
int tiny_image_create(
    TinyImage* image, uint32_t width, uint32_t height, uint8_t channels
) {
    if (!image) return TINYIMG_ERR_NULL;
    if (width == 0 || height == 0 || channels == 0 || channels > 4) {
        return TINYIMG_ERR_RANGE;
    }

    uint64_t pixels = (uint64_t) width * height;
    uint64_t wanted = pixels * channels;
    if (pixels > TINYIMG_MAX_PIXELS || wanted > TINYIMG_MAX_IMAGE_BYTES) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    size_t bytes = (size_t) wanted;

    uint8_t* data = tiny_alloc(bytes);
    if (!data) return TINYIMG_ERR_MEMORY;

    tiny_memset(data, 0, bytes);

    image->width = width;
    image->height = height;
    image->channels = channels;
    image->quality = 0;
    image->format = TINYIMG_FORMAT_UNKNOWN;
    image->data = data;
    image->meta = 0;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_destroy")
int tiny_image_destroy(TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;

    tiny_free(image->data);

    // the metadata block is one allocation by construction, so freeing it here
    // stays correct as the metadata region grows
    tiny_free(image->meta);

    image->width = 0;
    image->height = 0;
    image->channels = 0;
    image->quality = 0;
    image->format = TINYIMG_FORMAT_UNKNOWN;
    image->data = 0;
    image->meta = 0;

    return TINYIMG_OK;
}

TinyImageFormat tiny_format_sniff(const uint8_t* buffer, size_t buffer_size) {
    if (!buffer) return TINYIMG_FORMAT_UNKNOWN;

    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    if (buffer_size >= 8 && tiny_memcmp(buffer, png, 8) == 0) {
        return TINYIMG_FORMAT_PNG;
    }
    if (buffer_size >= 3 && buffer[0] == 0xFF && buffer[1] == 0xD8 &&
        buffer[2] == 0xFF) {
        return TINYIMG_FORMAT_JPEG;
    }
    if (buffer_size >= 2 && buffer[0] == 'B' && buffer[1] == 'M') {
        return TINYIMG_FORMAT_BMP;
    }
    if (buffer_size >= 6 && tiny_memcmp(buffer, "GIF8", 4) == 0 &&
        (buffer[4] == '7' || buffer[4] == '9') && buffer[5] == 'a') {
        return TINYIMG_FORMAT_GIF;
    }
    if (buffer_size >= 4 && ((buffer[0] == 'I' && buffer[1] == 'I' &&
                              buffer[2] == 0x2A && buffer[3] == 0x00) ||
                             (buffer[0] == 'M' && buffer[1] == 'M' &&
                              buffer[2] == 0x00 && buffer[3] == 0x2A))) {
        return TINYIMG_FORMAT_TIFF;
    }
    if (buffer_size >= 12 && tiny_memcmp(buffer, "RIFF", 4) == 0 &&
        tiny_memcmp(buffer + 8, "WEBP", 4) == 0) {
        return TINYIMG_FORMAT_WEBP;
    }

    // both AVIF and HEIF are ISOBMFF, so the brand in the ftyp box is what
    // separates them
    if (buffer_size >= 12 && tiny_memcmp(buffer + 4, "ftyp", 4) == 0) {
        const uint8_t* brand = buffer + 8;

        if (tiny_memcmp(brand, "avif", 4) == 0 ||
            tiny_memcmp(brand, "avis", 4) == 0) {
            return TINYIMG_FORMAT_AVIF;
        }
        if (tiny_memcmp(brand, "heic", 4) == 0 ||
            tiny_memcmp(brand, "heix", 4) == 0 ||
            tiny_memcmp(brand, "heim", 4) == 0 ||
            tiny_memcmp(brand, "heis", 4) == 0 ||
            tiny_memcmp(brand, "hevc", 4) == 0 ||
            tiny_memcmp(brand, "mif1", 4) == 0 ||
            tiny_memcmp(brand, "msf1", 4) == 0) {
            return TINYIMG_FORMAT_HEIF;
        }
    }

    return TINYIMG_FORMAT_UNKNOWN;
}

const char* tiny_format_name(TinyImageFormat format) {
    switch (format) {
        case TINYIMG_FORMAT_PNG: return "png";
        case TINYIMG_FORMAT_JPEG: return "jpeg";
        case TINYIMG_FORMAT_BMP: return "bmp";
        case TINYIMG_FORMAT_GIF: return "gif";
        case TINYIMG_FORMAT_TIFF: return "tiff";
        case TINYIMG_FORMAT_WEBP: return "webp";
        case TINYIMG_FORMAT_AVIF: return "avif";
        case TINYIMG_FORMAT_HEIF: return "heif";
        default: return "unknown";
    }
}

const char* tiny_format_extension(TinyImageFormat format) {
    switch (format) {
        case TINYIMG_FORMAT_PNG: return ".png";
        case TINYIMG_FORMAT_JPEG: return ".jpg";
        case TINYIMG_FORMAT_BMP: return ".bmp";
        case TINYIMG_FORMAT_GIF: return ".gif";
        case TINYIMG_FORMAT_TIFF: return ".tiff";
        case TINYIMG_FORMAT_WEBP: return ".webp";
        case TINYIMG_FORMAT_AVIF: return ".avif";
        case TINYIMG_FORMAT_HEIF: return ".heic";
        default: return "";
    }
}

TINYIMG_EXPORT("tiny_image_info_sizeof")
uint32_t tiny_image_info_sizeof(void) {
    return (uint32_t) sizeof(TinyImageInfo);
}

TINYIMG_EXPORT("tiny_image_probe")
int tiny_image_probe(
    const uint8_t* buffer, size_t buffer_size, TinyImageInfo* info
) {
    if (!buffer || !info) return TINYIMG_ERR_NULL;

    tiny_memset(info, 0, sizeof(*info));

    TinyImageFormat format = tiny_format_sniff(buffer, buffer_size);
    if (format == TINYIMG_FORMAT_UNKNOWN) return TINYIMG_ERR_UNKNOWN_FORMAT;

    info->format = format;
    info->frames = 1;

    const TinyCodec* codec = tiny_codec_find(format);
    if (!codec || !codec->probe) return TINYIMG_ERR_UNSUPPORTED_CODEC;

    return codec->probe(buffer, buffer_size, info);
}

TINYIMG_EXPORT("tiny_image_decode")
int tiny_image_decode(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size,
    const TinyDecodeOpts* opts
) {
    if (!image || !buffer) return TINYIMG_ERR_NULL;

    TinyImageFormat format = tiny_format_sniff(buffer, buffer_size);
    if (format == TINYIMG_FORMAT_UNKNOWN) return TINYIMG_ERR_UNKNOWN_FORMAT;

    const TinyCodec* codec = tiny_codec_find(format);
    if (!codec || !codec->decode) return TINYIMG_ERR_UNSUPPORTED_CODEC;

    TinyDecodeOpts defaults = {0, 0, 0, 0, 1, 0, 0};
    return codec->decode(image, buffer, buffer_size, opts ? opts : &defaults);
}

TINYIMG_EXPORT("tiny_image_load")
int tiny_image_load(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size
) {
    return tiny_image_decode(image, buffer, buffer_size, 0);
}

TINYIMG_EXPORT("tiny_image_load_scaled")
int tiny_image_load_scaled(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size,
    uint32_t max_width, uint32_t max_height
) {
    if (!image || !buffer) return TINYIMG_ERR_NULL;

    TinyImageInfo info;
    int result = tiny_image_probe(buffer, buffer_size, &info);
    if (result != TINYIMG_OK) return result;

    // the largest denominator that still covers the box, so whatever resamples
    // afterwards is always downscaling
    uint8_t den = 1;
    for (uint8_t candidate = 8; candidate > 1; candidate /= 2) {
        uint32_t width = (info.width + candidate - 1u) / candidate;
        uint32_t height = (info.height + candidate - 1u) / candidate;

        if ((max_width == 0 || width >= max_width) &&
            (max_height == 0 || height >= max_height)) {
            den = candidate;
            break;
        }
    }

    TinyDecodeOpts opts = {0, 0, 0, 0, den, 0, 0};
    return tiny_image_decode(image, buffer, buffer_size, &opts);
}

TINYIMG_EXPORT("tiny_image_load_region")
int tiny_image_load_region(
    TinyImage* image, const uint8_t* buffer, size_t buffer_size, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height
) {
    TinyDecodeOpts opts = {x, y, width, height, 1, 0, 0};
    return tiny_image_decode(image, buffer, buffer_size, &opts);
}

TINYIMG_EXPORT("tiny_image_encode")
int tiny_image_encode(
    const TinyImage* image, TinyImageFormat format, const TinyEncodeOpts* opts,
    TinyWriter* writer
) {
    if (!image || !writer) return TINYIMG_ERR_NULL;
    if (!image->data) return TINYIMG_ERR_NULL;

    const TinyCodec* codec = tiny_codec_find(format);
    if (!codec || !codec->encode) return TINYIMG_ERR_UNSUPPORTED_CODEC;

    tiny_work_add(TINYIMG_WORK_ENCODED, image->width * image->height);

    TinyEncodeOpts defaults = {
        image->quality ? image->quality : 82, 0, 0, 0, 0
    };
    return codec->encode(image, opts ? opts : &defaults, writer);
}

int tiny_image_gettype(const TinyImage* image, TinyImagePixelType* type) {
    if (!image || !type) return TINYIMG_ERR_NULL;

    switch (image->channels) {
        case 1: *type = TINYIMG_PIXEL_GRAY; break;
        case 2: *type = TINYIMG_PIXEL_GRAY_ALPHA; break;
        case 3: *type = TINYIMG_PIXEL_RGB; break;
        case 4: *type = TINYIMG_PIXEL_RGBA; break;
        default: return TINYIMG_ERR_RANGE;
    }

    return TINYIMG_OK;
}

int tiny_image_getextension(
    const TinyImage* image, char* extension, size_t max_length
) {
    if (!image || !extension) return TINYIMG_ERR_NULL;
    if (image->format == TINYIMG_FORMAT_UNKNOWN) {
        return TINYIMG_ERR_UNKNOWN_FORMAT;
    }

    return tiny_strcopy(
        extension, tiny_format_extension(image->format), max_length
    );
}

int tiny_image_getpixel(
    const TinyImage* image, uint32_t x, uint32_t y, uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (x >= image->width || y >= image->height) return TINYIMG_ERR_BOUNDS;

    size_t offset = ((size_t) y * image->width + x) * image->channels;
    tiny_memcpy(pixel, image->data + offset, image->channels);

    return TINYIMG_OK;
}

int tiny_image_setpixel(
    TinyImage* image, uint32_t x, uint32_t y, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (x >= image->width || y >= image->height) return TINYIMG_ERR_BOUNDS;

    size_t offset = ((size_t) y * image->width + x) * image->channels;
    tiny_memcpy(image->data + offset, pixel, image->channels);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_getwidth")
uint32_t tiny_image_getwidth(const TinyImage* image) {
    return image ? image->width : 0;
}

TINYIMG_EXPORT("tiny_image_getheight")
uint32_t tiny_image_getheight(const TinyImage* image) {
    return image ? image->height : 0;
}

TINYIMG_EXPORT("tiny_image_getchannels")
uint32_t tiny_image_getchannels(const TinyImage* image) {
    return image ? image->channels : 0;
}

TINYIMG_EXPORT("tiny_image_getdata")
uint8_t* tiny_image_getdata(const TinyImage* image) {
    return image ? image->data : 0;
}

TINYIMG_EXPORT("tiny_image_getsize")
uint32_t tiny_image_getsize(const TinyImage* image) {
    if (!image) return 0;
    return image->width * image->height * image->channels;
}

#pragma endregion

#pragma region image manipulation

TINYIMG_EXPORT("tiny_image_resize")
int tiny_image_resize(
    TinyImage* image, uint32_t new_width, uint32_t new_height
) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_resize(&plan, new_width, new_height);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_crop")
int tiny_image_crop(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t crop_width,
    uint32_t crop_height
) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_crop(&plan, x, y, crop_width, crop_height);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_flip_horizontal")
int tiny_image_flip_horizontal(TinyImage* image) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    tiny_plan_flip_horizontal(&plan);
    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_flip_vertical")
int tiny_image_flip_vertical(TinyImage* image) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    tiny_plan_flip_vertical(&plan);
    return tiny_plan_replace(image, &plan);
}

static int rotate_by(TinyImage* image, int32_t degrees) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    tiny_plan_rotate(&plan, degrees);
    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_rotate_90")
int tiny_image_rotate_90(TinyImage* image) {
    return rotate_by(image, 90);
}

TINYIMG_EXPORT("tiny_image_rotate_180")
int tiny_image_rotate_180(TinyImage* image) {
    return rotate_by(image, 180);
}

TINYIMG_EXPORT("tiny_image_rotate_270")
int tiny_image_rotate_270(TinyImage* image) {
    return rotate_by(image, 270);
}

TINYIMG_EXPORT("tiny_image_zoom")
int tiny_image_zoom(TinyImage* image, float zoom_factor) {
    if (!image) return TINYIMG_ERR_NULL;
    if (zoom_factor <= 0.0f) return TINYIMG_ERR_RANGE;

    uint32_t width = (uint32_t) ((float) image->width * zoom_factor + 0.5f);
    uint32_t height = (uint32_t) ((float) image->height * zoom_factor + 0.5f);

    return tiny_image_resize(
        image, tiny_max_u32(width, 1u), tiny_max_u32(height, 1u)
    );
}

int tiny_image_dpr(TinyImage* image, float dpr) {
    return tiny_image_zoom(image, dpr);
}

TINYIMG_EXPORT("tiny_image_opacity")
int tiny_image_opacity(TinyImage* image, float opacity) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (opacity < 0.0f || opacity > 1.0f) return TINYIMG_ERR_RANGE;

    if (image->channels == 1u || image->channels == 3u) {
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) return result;
    }

    uint8_t at = (uint8_t) (image->channels - 1u);
    uint32_t scale = (uint32_t) (opacity * 256.0f + 0.5f);
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        p[at] = (uint8_t) ((p[at] * scale + 128u) >> 8);
    }

    return TINYIMG_OK;
}

/**
 * @brief Clears the pixels a mask says are outside a shape.
 *
 * The three shape crops are one function: the shape decides which pixels
 * survive and nothing else about them changes, so they share the alpha
 * handling and the channel promotion.
 *
 * @param image The image to change, which gains alpha if it has none.
 * @param keep A byte per pixel, non-zero to keep.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int clear_outside(TinyImage* image, const uint8_t* keep) {
    if (image->channels == 1u || image->channels == 3u) {
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) return result;
    }

    uint8_t at = (uint8_t) (image->channels - 1u);
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        if (keep[i]) continue;

        uint8_t* p = image->data + i * image->channels;
        for (uint8_t c = 0; c < image->channels; c++) p[c] = 0u;
        p[at] = 0u;
    }

    return TINYIMG_OK;
}

/**
 * @brief Builds a keep mask by drawing a shape into a one channel image.
 *
 * The mask comes from the drawing primitives rather than from a second
 * implementation of each shape's inside test, so a crop to a shape and a fill
 * of that shape agree on exactly which pixels it covers.
 *
 * @param image The image whose extent the mask matches.
 * @param mask Receives the mask image, which the caller destroys.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int mask_begin(const TinyImage* image, TinyImage* mask) {
    tiny_memset(mask, 0, sizeof(*mask));

    return tiny_image_create(mask, image->width, image->height, 1);
}

TINYIMG_EXPORT("tiny_image_crop_ellipse")
int tiny_image_crop_ellipse(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius_x,
    uint32_t radius_y
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (radius_x == 0 || radius_y == 0) return TINYIMG_ERR_RANGE;

    TinyImage mask;
    int result = mask_begin(image, &mask);
    if (result != TINYIMG_OK) return result;

    static const uint8_t ON[1] = {255};

    result = tiny_image_fill_ellipse(
        &mask, (int32_t) center_x, (int32_t) center_y, radius_x, radius_y, ON
    );
    if (result == TINYIMG_OK) result = clear_outside(image, mask.data);

    tiny_image_destroy(&mask);
    return result;
}

TINYIMG_EXPORT("tiny_image_crop_circle")
int tiny_image_crop_circle(
    TinyImage* image, uint32_t center_x, uint32_t center_y, uint32_t radius
) {
    return tiny_image_crop_ellipse(image, center_x, center_y, radius, radius);
}

TINYIMG_EXPORT("tiny_image_crop_polygon")
int tiny_image_crop_polygon(
    TinyImage* image, const uint32_t* x_points, const uint32_t* y_points,
    size_t num_points
) {
    if (!image || !image->data || !x_points || !y_points) {
        return TINYIMG_ERR_NULL;
    }
    if (num_points < 3u) return TINYIMG_ERR_RANGE;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    int32_t* xs = tiny_arena_alloc(num_points * sizeof(int32_t), 4);
    int32_t* ys = tiny_arena_alloc(num_points * sizeof(int32_t), 4);

    if (!xs || !ys) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (size_t i = 0; i < num_points; i++) {
        xs[i] = (int32_t) x_points[i];
        ys[i] = (int32_t) y_points[i];
    }

    TinyImage mask;
    int result = mask_begin(image, &mask);

    if (result == TINYIMG_OK) {
        static const uint8_t ON[1] = {255};

        result = tiny_image_fill_polygon(&mask, xs, ys, num_points, ON);
        if (result == TINYIMG_OK) result = clear_outside(image, mask.data);

        tiny_image_destroy(&mask);
    }

    tiny_arena_release(&mark);
    return result;
}

/**
 * @brief Whether a pixel is within tolerance of a color on every channel.
 *
 * @param pixel The pixel.
 * @param color The color.
 * @param channels How many channels to compare.
 * @param tolerance How far each may differ.
 * @return int Non-zero when it matches.
 */
static int color_near(
    const uint8_t* pixel, const uint8_t* color, uint8_t channels,
    int32_t tolerance
) {
    for (uint8_t c = 0; c < channels; c++) {
        int32_t diff = (int32_t) pixel[c] - (int32_t) color[c];

        if (diff < -tolerance || diff > tolerance) return 0;
    }

    return 1;
}

/**
 * @brief Whether a whole row or column is border.
 *
 * @param image The image.
 * @param index Which row or column.
 * @param vertical Non-zero for a column.
 * @param color The border color.
 * @param tolerance How far a channel may differ.
 * @return int Non-zero when every pixel matches.
 */
static int line_is_border(
    const TinyImage* image, uint32_t index, int vertical, const uint8_t* color,
    int32_t tolerance
) {
    uint32_t count = vertical ? image->height : image->width;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t x = vertical ? index : i;
        uint32_t y = vertical ? i : index;
        const uint8_t* p =
            image->data + ((size_t) y * image->width + x) * image->channels;

        if (!color_near(p, color, image->channels, tolerance)) return 0;
    }

    return 1;
}

TINYIMG_EXPORT("tiny_image_trim")
int tiny_image_trim(TinyImage* image, uint8_t tolerance) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    // the border color is the top left corner, and every corner has to agree
    // with it: an image whose corners differ has no uniform border to trim and
    // is left alone rather than cropped to whatever the first corner happened
    // to be
    const uint8_t* corner = image->data;
    uint32_t last_x = image->width - 1u;
    uint32_t last_y = image->height - 1u;

    const uint8_t* others[3] = {
        image->data + (size_t) last_x * image->channels,
        image->data + (size_t) last_y * image->width * image->channels,
        image->data +
            ((size_t) last_y * image->width + last_x) * image->channels
    };

    for (uint32_t i = 0; i < 3u; i++) {
        if (!color_near(
                others[i], corner, image->channels, (int32_t) tolerance
            )) {
            return TINYIMG_OK;
        }
    }

    uint8_t border[4];
    for (uint8_t c = 0; c < image->channels; c++) border[c] = corner[c];

    uint32_t left = 0;
    uint32_t right = last_x;
    uint32_t top = 0;
    uint32_t bottom = last_y;

    while (left < right &&
           line_is_border(image, left, 1, border, (int32_t) tolerance)) {
        left++;
    }
    while (right > left &&
           line_is_border(image, right, 1, border, (int32_t) tolerance)) {
        right--;
    }
    while (top < bottom &&
           line_is_border(image, top, 0, border, (int32_t) tolerance)) {
        top++;
    }
    while (bottom > top &&
           line_is_border(image, bottom, 0, border, (int32_t) tolerance)) {
        bottom--;
    }

    if (left == 0 && top == 0 && right == last_x && bottom == last_y) {
        return TINYIMG_OK;
    }

    return tiny_image_crop(
        image, left, top, right - left + 1u, bottom - top + 1u
    );
}

TINYIMG_EXPORT("tiny_image_remove_background")
int tiny_image_remove_background(TinyImage* image, uint8_t tolerance) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint8_t seed[4];
    for (uint8_t c = 0; c < image->channels; c++) seed[c] = image->data[c];

    uint32_t width = image->width;
    uint32_t height = image->height;
    size_t pixels = (size_t) width * height;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    // the queue holds one entry per pixel at worst, which is what makes this a
    // bounded iterative fill rather than a recursion that would run the stack
    // out on a large flat background
    uint8_t* reach = tiny_arena_alloc(pixels, 1);
    uint32_t* queue = tiny_arena_alloc(pixels * sizeof(uint32_t), 4);

    if (!reach || !queue) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(reach, 0, pixels);

    uint32_t tail = 0;
    uint32_t head = 0;

    const uint32_t corner[4] = {
        0u, width - 1u, (height - 1u) * width, (uint32_t) pixels - 1u
    };

    // a corner that does not match the seed color is not background, so it
    // is not a seed either: seeding all four unconditionally clears a corner
    // whose color the caller never asked to remove
    for (uint32_t i = 0; i < 4u; i++) {
        if (reach[corner[i]]) continue;

        const uint8_t* p = image->data + (size_t) corner[i] * image->channels;
        if (!color_near(p, seed, image->channels, (int32_t) tolerance)) {
            continue;
        }

        reach[corner[i]] = 1u;
        queue[tail++] = corner[i];
    }

    while (head < tail) {
        uint32_t at = queue[head++];
        int32_t x = (int32_t) (at % width);
        int32_t y = (int32_t) (at / width);

        static const int32_t STEP[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

        for (uint32_t k = 0; k < 4u; k++) {
            int32_t nx = x + STEP[k][0];
            int32_t ny = y + STEP[k][1];

            if (nx < 0 || ny < 0 || nx >= (int32_t) width ||
                ny >= (int32_t) height) {
                continue;
            }

            uint32_t next = (uint32_t) ny * width + (uint32_t) nx;
            if (reach[next]) continue;

            const uint8_t* p = image->data + (size_t) next * image->channels;
            if (!color_near(p, seed, image->channels, (int32_t) tolerance)) {
                continue;
            }

            reach[next] = 1u;
            queue[tail++] = next;
        }
    }

    if (image->channels == 1u || image->channels == 3u) {
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) {
            tiny_arena_release(&mark);
            return result;
        }
    }

    uint8_t at_channel = (uint8_t) (image->channels - 1u);

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;

        if (reach[i]) {
            p[at_channel] = 0u;
            continue;
        }

        // a pixel the fill did not reach but which borders one it did is on
        // the boundary, and its alpha is scaled by how far it is from the
        // background color: that is what feathers the cutout rather than
        // leaving a hard stair-stepped edge
        uint32_t x = (uint32_t) (i % width);
        uint32_t y = (uint32_t) (i / width);
        int borders = 0;

        if (x > 0 && reach[i - 1u]) borders = 1;
        if (x + 1u < width && reach[i + 1u]) borders = 1;
        if (y > 0 && reach[i - width]) borders = 1;
        if (y + 1u < height && reach[i + width]) borders = 1;

        if (!borders) continue;

        int32_t worst = 0;

        for (uint8_t c = 0; c < at_channel; c++) {
            int32_t diff = (int32_t) p[c] - (int32_t) seed[c];
            if (diff < 0) diff = -diff;
            if (diff > worst) worst = diff;
        }

        int32_t limit = (int32_t) tolerance + 1;
        if (worst >= limit * 2) continue;

        p[at_channel] =
            (uint8_t) ((int32_t) p[at_channel] * worst / (limit * 2));
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region image transformations

int tiny_image_quality(TinyImage* image, int quality) {
    if (!image) return TINYIMG_ERR_NULL;
    if (quality < 0 || quality > 100) return TINYIMG_ERR_RANGE;

    image->quality = (uint8_t) quality;
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_fit")
int tiny_image_fit(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode
) {
    return tiny_image_fit_with_padding_and_background(
        image, target_width, target_height, fit_mode, 0, 0
    );
}

int tiny_image_fit_with_padding(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, const uint8_t* padding_color
) {
    return tiny_image_fit_with_padding_and_background(
        image, target_width, target_height, fit_mode, padding_color, 0
    );
}

TINYIMG_EXPORT("tiny_image_fit_with_gravity")
int tiny_image_fit_with_gravity(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, TinyImageGravity gravity, const uint8_t* background
) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    if (background) tiny_plan_background(&plan, background);

    result =
        tiny_plan_fit(&plan, target_width, target_height, fit_mode, gravity);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

int tiny_image_fit_with_padding_and_background(
    TinyImage* image, uint32_t target_width, uint32_t target_height,
    TinyImageFit fit_mode, const uint8_t* padding_color,
    const uint8_t* background_color
) {
    // the two colors fill the same pixels, so the padding wins where a caller
    // has given both and the background is what is left when it has not
    const uint8_t* fill = padding_color ? padding_color : background_color;

    return tiny_image_fit_with_gravity(
        image, target_width, target_height, fit_mode, TINYIMG_GRAVITY_CENTER,
        fill
    );
}

static int recolor(TinyImage* image, TinyPlanOpKind kind, float value) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    switch (kind) {
        case TINYIMG_OP_BRIGHTNESS:
            result = tiny_plan_brightness(&plan, value);
            break;
        case TINYIMG_OP_CONTRAST:
            result = tiny_plan_contrast(&plan, value);
            break;
        case TINYIMG_OP_SATURATION:
            result = tiny_plan_saturation(&plan, value);
            break;
        case TINYIMG_OP_HUE: result = tiny_plan_hue(&plan, value); break;
        case TINYIMG_OP_GAMMA: result = tiny_plan_gamma(&plan, value); break;
        default: result = tiny_plan_invert(&plan); break;
    }

    if (result != TINYIMG_OK) return result;
    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_invert")
int tiny_image_invert(TinyImage* image) {
    return recolor(image, TINYIMG_OP_INVERT, 0.0f);
}

TINYIMG_EXPORT("tiny_image_brightness")
int tiny_image_brightness(TinyImage* image, float factor) {
    return recolor(image, TINYIMG_OP_BRIGHTNESS, factor);
}

TINYIMG_EXPORT("tiny_image_contrast")
int tiny_image_contrast(TinyImage* image, float factor) {
    return recolor(image, TINYIMG_OP_CONTRAST, factor);
}

TINYIMG_EXPORT("tiny_image_saturation")
int tiny_image_saturation(TinyImage* image, float factor) {
    return recolor(image, TINYIMG_OP_SATURATION, factor);
}

TINYIMG_EXPORT("tiny_image_hue")
int tiny_image_hue(TinyImage* image, float angle) {
    if (angle < -360.0f || angle > 360.0f) return TINYIMG_ERR_RANGE;
    return recolor(image, TINYIMG_OP_HUE, angle);
}

TINYIMG_EXPORT("tiny_image_gamma_correction")
int tiny_image_gamma_correction(TinyImage* image, float gamma) {
    return recolor(image, TINYIMG_OP_GAMMA, gamma);
}

TINYIMG_EXPORT("tiny_image_blur")
int tiny_image_blur(TinyImage* image, float radius) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_blur(&plan, radius);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_gaussian_blur")
int tiny_image_gaussian_blur(TinyImage* image, float sigma) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_gaussian_blur(&plan, sigma);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

#pragma endregion

#pragma region image metadata

/**
 * @brief Everything an image carries besides its pixels, in one allocation.
 *
 * One block, not a structure of pointers, because tiny_image_destroy frees it
 * with a single call and every caller across the wasm boundary would otherwise
 * have to know how many allocations a particular image happened to hold. The
 * payloads live past the header and are addressed by offset rather than by
 * pointer, so growing the block cannot leave a stale pointer behind.
 *
 * Entries are laid out end to end as a key length, a value length, then both
 * strings with their terminators. Removing one closes the gap, which keeps the
 * block compact at the cost of a copy nobody does in a loop.
 */
struct TinyImageMeta {
    /** Bytes in use, including this header. */
    size_t size;
    /** Bytes the block can hold. */
    size_t capacity;
    /** Offset of the EXIF payload, or zero when there is none. */
    size_t exif;
    /** Length of the EXIF payload. */
    size_t exif_size;
    /** Offset of the first key and value entry, or zero when there are none. */
    size_t entries;
    /** How many entries there are. */
    size_t count;
};

#define META_HEADER                                                            \
    ((sizeof(struct TinyImageMeta) + TINYIMG_ALIGNMENT - 1) &                  \
     ~(size_t) (TINYIMG_ALIGNMENT - 1))

static uint8_t* meta_bytes(TinyImageMeta* meta) {
    return (uint8_t*) meta;
}

/** Grows the block so `extra` more bytes fit, moving it if it has to. */
static int meta_reserve(TinyImage* image, size_t extra) {
    if (!image->meta) {
        size_t initial = META_HEADER + extra;
        TinyImageMeta* meta = tiny_alloc(initial);

        if (!meta) return TINYIMG_ERR_MEMORY;

        tiny_memset(meta, 0, META_HEADER);
        meta->size = META_HEADER;
        meta->capacity = initial;
        image->meta = meta;

        return TINYIMG_OK;
    }

    if (image->meta->size + extra <= image->meta->capacity) return TINYIMG_OK;

    size_t wanted = image->meta->size + extra;
    size_t capacity = image->meta->capacity * 2;

    if (capacity < wanted) capacity = wanted;

    TinyImageMeta* grown = tiny_realloc(image->meta, capacity);
    if (!grown) return TINYIMG_ERR_MEMORY;

    grown->capacity = capacity;
    image->meta = grown;

    return TINYIMG_OK;
}

/** Reads an entry's lengths and the offsets of its key and value. */
static void meta_entry(
    TinyImageMeta* meta, size_t at, size_t* key, size_t* value, size_t* next
) {
    const uint8_t* bytes = meta_bytes(meta) + at;
    uint32_t key_size;
    uint32_t value_size;

    tiny_memcpy(&key_size, bytes, sizeof(key_size));
    tiny_memcpy(&value_size, bytes + sizeof(key_size), sizeof(value_size));

    size_t header = sizeof(key_size) + sizeof(value_size);

    *key = at + header;
    *value = at + header + key_size + 1;
    *next = at + header + key_size + 1 + value_size + 1;
}

/** Finds an entry by key, or returns zero. */
static size_t meta_find(TinyImageMeta* meta, const char* key) {
    if (!meta || !meta->entries) return 0;

    size_t at = meta->entries;

    for (size_t i = 0; i < meta->count; i++) {
        size_t key_at;
        size_t value_at;
        size_t next;

        meta_entry(meta, at, &key_at, &value_at, &next);

        if (tiny_strcmp((const char*) meta_bytes(meta) + key_at, key) == 0) {
            return at;
        }

        at = next;
    }

    return 0;
}

TINYIMG_EXPORT("tiny_image_set_exif")
int tiny_image_set_exif(
    TinyImage* image, const char* exif_data, size_t exif_size
) {
    if (!image) return TINYIMG_ERR_NULL;
    if (!exif_data && exif_size) return TINYIMG_ERR_NULL;

    if (exif_size == 0) return tiny_image_strip_exif(image);

    // replacing a payload of a different length would move every entry after
    // it, so the old one is dropped first and the new one appended
    int result = tiny_image_strip_exif(image);
    if (result != TINYIMG_OK) return result;

    result = meta_reserve(image, exif_size);
    if (result != TINYIMG_OK) return result;

    TinyImageMeta* meta = image->meta;

    // ahead of the entries, so appending an entry later does not have to know
    // the payload is there
    if (meta->entries) {
        size_t block = meta->size - meta->entries;

        tiny_memmove(
            meta_bytes(meta) + meta->entries + exif_size,
            meta_bytes(meta) + meta->entries, block
        );
        meta->exif = meta->entries;
        meta->entries += exif_size;
    }
    else {
        meta->exif = meta->size;
    }

    tiny_memcpy(meta_bytes(meta) + meta->exif, exif_data, exif_size);

    meta->exif_size = exif_size;
    meta->size += exif_size;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_get_exif")
int tiny_image_get_exif(
    const TinyImage* image, char** exif_data, size_t* exif_size
) {
    if (!image || !exif_data || !exif_size) return TINYIMG_ERR_NULL;
    if (!image->meta || !image->meta->exif_size) return TINYIMG_ERR_NOT_FOUND;

    char* copy = tiny_alloc(image->meta->exif_size);
    if (!copy) return TINYIMG_ERR_MEMORY;

    tiny_memcpy(
        copy, meta_bytes(image->meta) + image->meta->exif,
        image->meta->exif_size
    );

    *exif_data = copy;
    *exif_size = image->meta->exif_size;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_has_exif")
int tiny_image_has_exif(const TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;
    return image->meta && image->meta->exif_size ? 1 : 0;
}

TINYIMG_EXPORT("tiny_image_strip_exif")
int tiny_image_strip_exif(TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;
    if (!image->meta || !image->meta->exif_size) return TINYIMG_OK;

    TinyImageMeta* meta = image->meta;
    size_t removed = meta->exif_size;
    size_t after = meta->exif + removed;

    tiny_memmove(
        meta_bytes(meta) + meta->exif, meta_bytes(meta) + after,
        meta->size - after
    );

    meta->size -= removed;
    if (meta->entries) meta->entries -= removed;

    meta->exif = 0;
    meta->exif_size = 0;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_set_metadata")
int tiny_image_set_metadata(
    TinyImage* image, const char* key, const char* value
) {
    if (!image || !key || !value) return TINYIMG_ERR_NULL;
    if (*key == 0) return TINYIMG_ERR_RANGE;

    size_t key_size = tiny_strlen(key);
    size_t value_size = tiny_strlen(value);

    if (key_size > 0xFFFFFFFFu || value_size > 0xFFFFFFFFu) {
        return TINYIMG_ERR_RANGE;
    }

    // a key that is already there is removed rather than overwritten, since the
    // new value need not be the same length
    int result = tiny_image_remove_metadata(image, key);
    if (result != TINYIMG_OK && result != TINYIMG_ERR_NOT_FOUND) return result;

    size_t needed = 8 + key_size + 1 + value_size + 1;

    result = meta_reserve(image, needed);
    if (result != TINYIMG_OK) return result;

    TinyImageMeta* meta = image->meta;
    uint8_t* at = meta_bytes(meta) + meta->size;

    uint32_t stored_key = (uint32_t) key_size;
    uint32_t stored_value = (uint32_t) value_size;

    tiny_memcpy(at, &stored_key, sizeof(stored_key));
    tiny_memcpy(at + 4, &stored_value, sizeof(stored_value));
    tiny_memcpy(at + 8, key, key_size + 1);
    tiny_memcpy(at + 8 + key_size + 1, value, value_size + 1);

    if (!meta->entries) meta->entries = meta->size;

    meta->size += needed;
    meta->count++;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_get_metadata")
int tiny_image_get_metadata(
    const TinyImage* image, const char* key, char** value
) {
    if (!image || !key || !value) return TINYIMG_ERR_NULL;

    size_t at = meta_find(image->meta, key);
    if (!at) return TINYIMG_ERR_NOT_FOUND;

    size_t key_at;
    size_t value_at;
    size_t next;

    meta_entry(image->meta, at, &key_at, &value_at, &next);

    const char* found = (const char*) meta_bytes(image->meta) + value_at;
    size_t size = tiny_strlen(found);

    char* copy = tiny_alloc(size + 1);
    if (!copy) return TINYIMG_ERR_MEMORY;

    tiny_memcpy(copy, found, size + 1);
    *value = copy;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_has_metadata")
int tiny_image_has_metadata(const TinyImage* image, const char* key) {
    if (!image || !key) return TINYIMG_ERR_NULL;
    return meta_find(image->meta, key) ? 1 : 0;
}

TINYIMG_EXPORT("tiny_image_remove_metadata")
int tiny_image_remove_metadata(TinyImage* image, const char* key) {
    if (!image || !key) return TINYIMG_ERR_NULL;

    size_t at = meta_find(image->meta, key);
    if (!at) return TINYIMG_ERR_NOT_FOUND;

    TinyImageMeta* meta = image->meta;

    size_t key_at;
    size_t value_at;
    size_t next;

    meta_entry(meta, at, &key_at, &value_at, &next);

    tiny_memmove(
        meta_bytes(meta) + at, meta_bytes(meta) + next, meta->size - next
    );

    meta->size -= next - at;
    meta->count--;

    if (meta->count == 0) meta->entries = 0;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_get_metadata_count")
int tiny_image_get_metadata_count(const TinyImage* image, size_t* count) {
    if (!image || !count) return TINYIMG_ERR_NULL;

    *count = image->meta ? image->meta->count : 0;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region image conversion

TINYIMG_EXPORT("tiny_image_convert_channels")
int tiny_image_convert_channels(TinyImage* image, uint8_t channels) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (channels == 0 || channels > 4) return TINYIMG_ERR_RANGE;
    if (channels == image->channels) return TINYIMG_OK;

    size_t count = (size_t) image->width * image->height;
    uint8_t from = image->channels;

    if (channels < from) {
        // the write head trails the read head, so narrowing needs no second
        // buffer for what may be a very large image
        for (size_t i = 0; i < count; i++) {
            uint8_t source[4];
            tiny_memcpy(source, image->data + i * from, from);
            tiny_pixel_convert(
                image->data + i * channels, channels, source, from
            );
        }

        image->channels = channels;
        return TINYIMG_OK;
    }

    uint8_t* widened = tiny_alloc(count * channels);
    if (!widened) return TINYIMG_ERR_MEMORY;

    for (size_t i = 0; i < count; i++) {
        tiny_pixel_convert(
            widened + i * channels, channels, image->data + i * from, from
        );
    }

    tiny_free(image->data);
    image->data = widened;
    image->channels = channels;

    return TINYIMG_OK;
}

int tiny_image_to_rgb(TinyImage* image) {
    return tiny_image_convert_channels(image, 3);
}

int tiny_image_to_rgba(TinyImage* image) {
    return tiny_image_convert_channels(image, 4);
}

int tiny_image_to_grayscale(TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;

    // an image with alpha keeps it; dropping it here would be a second,
    // undocumented change
    return tiny_image_convert_channels(
        image, (uint8_t) (image->channels == 2 || image->channels == 4 ? 2 : 1)
    );
}

int tiny_image_istransparent(const TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;
    return image->channels == 2 || image->channels == 4;
}

int tiny_image_set_transparent(TinyImage* image, int enable_transparency) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    int has_alpha = image->channels == 2 || image->channels == 4;

    if (enable_transparency) {
        if (has_alpha) return TINYIMG_OK;

        int result =
            tiny_image_convert_channels(image, (uint8_t) (image->channels + 1));
        if (result != TINYIMG_OK) return result;

        // a new alpha channel starts opaque, since the pixels that were there
        // were visible
        size_t count = (size_t) image->width * image->height;
        uint8_t last = (uint8_t) (image->channels - 1);

        for (size_t i = 0; i < count; i++) {
            image->data[i * image->channels + last] = 255;
        }

        return TINYIMG_OK;
    }

    if (!has_alpha) return TINYIMG_OK;

    // flattening onto white is what a browser does with an image dropped into
    // an opaque container
    size_t count = (size_t) image->width * image->height;
    uint8_t color = (uint8_t) (image->channels - 1);

    for (size_t i = 0; i < count; i++) {
        uint8_t* pixel = image->data + i * image->channels;
        uint32_t alpha = pixel[color];

        for (uint8_t c = 0; c < color; c++) {
            uint32_t blended = pixel[c] * alpha + 255u * (255u - alpha);
            pixel[c] = (uint8_t) ((blended + 127u) / 255u);
        }
    }

    return tiny_image_convert_channels(image, color);
}

int tiny_image_convert(TinyImage* image, TinyImageFormat format) {
    if (!image) return TINYIMG_ERR_NULL;
    if (format == TINYIMG_FORMAT_UNKNOWN) return TINYIMG_ERR_RANGE;

    // recording the target format rather than re-encoding, so asking for JPEG
    // costs the alpha channel it cannot carry and nothing else
    if (format == TINYIMG_FORMAT_JPEG &&
        (image->channels == 2 || image->channels == 4)) {
        int result = tiny_image_set_transparent(image, 0);
        if (result != TINYIMG_OK) return result;
    }

    image->format = format;
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_getformat")
TinyImageFormat tiny_image_getformat(const TinyImage* image) {
    return image ? image->format : TINYIMG_FORMAT_UNKNOWN;
}

#pragma endregion

#pragma region image analysis

/** A pixel's Rec. 709 luminance, whatever its channel count. */
static uint8_t pixel_luma(const uint8_t* pixel, uint8_t channels) {
    if (channels < 3u) return pixel[0];

    uint32_t sum = 13933u * pixel[0] + 46871u * pixel[1] + 4732u * pixel[2];
    return (uint8_t) ((sum + 32768u) >> 16);
}

TINYIMG_EXPORT("tiny_image_histogram")
int tiny_image_histogram(
    const TinyImage* image, uint8_t channel, uint32_t* bins
) {
    if (!image || !image->data || !bins) return TINYIMG_ERR_NULL;
    if (channel != 255u && channel >= image->channels) return TINYIMG_ERR_RANGE;

    for (uint32_t i = 0; i < 256u; i++) bins[i] = 0;

    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* p = image->data + i * image->channels;
        uint8_t value =
            channel == 255u ? pixel_luma(p, image->channels) : p[channel];

        bins[value]++;
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_average_color")
int tiny_image_average_color(const TinyImage* image, uint8_t* color) {
    if (!image || !image->data || !color) return TINYIMG_ERR_NULL;

    uint64_t total[4] = {0, 0, 0, 0};
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* p = image->data + i * image->channels;
        for (uint8_t c = 0; c < image->channels; c++) total[c] += p[c];
    }

    for (uint8_t c = 0; c < image->channels; c++) {
        color[c] = (uint8_t) ((total[c] + pixels / 2u) / pixels);
    }

    return TINYIMG_OK;
}

/**
 * @brief Finds a palette by k-means over a subsample.
 *
 * The dominant color and the palette are one algorithm: the palette is the
 * cluster centers and the dominant color is the largest cluster's center, so
 * asking for one color and taking the first is exactly the right answer
 * rather than a special case.
 *
 * A subsample rather than every pixel because the centers a photograph
 * converges to do not move measurably past a few tens of thousands of samples,
 * and the cost is the sample count times the cluster count times the rounds.
 *
 * @param image The image to read.
 * @param count How many clusters.
 * @param palette Receives `count` colors, largest cluster first.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int cluster_colors(
    const TinyImage* image, uint32_t count, uint8_t* palette
) {
    uint8_t channels = image->channels;
    size_t pixels = (size_t) image->width * image->height;
    size_t step = pixels / 32768u;
    if (step < 1u) step = 1u;

    size_t samples = (pixels + step - 1u) / step;
    if (samples < count) count = (uint32_t) samples;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    float* center = tiny_arena_alloc(count * 4u * sizeof(float), 4);
    float* sum = tiny_arena_alloc(count * 4u * sizeof(float), 4);
    uint32_t* members = tiny_arena_alloc(count * sizeof(uint32_t), 4);

    if (!center || !sum || !members) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    // seeded by spreading the starts along the sample order rather than at
    // random, so the same image gives the same palette every run
    for (uint32_t k = 0; k < count; k++) {
        size_t at = (samples * k / count) * step;
        const uint8_t* p = image->data + at * channels;

        for (uint8_t c = 0; c < channels; c++) {
            center[k * 4u + c] = (float) p[c];
        }
    }

    for (uint32_t round = 0; round < 12u; round++) {
        for (uint32_t k = 0; k < count; k++) {
            members[k] = 0;
            for (uint8_t c = 0; c < 4u; c++) sum[k * 4u + c] = 0.0f;
        }

        for (size_t i = 0; i < pixels; i += step) {
            const uint8_t* p = image->data + i * channels;
            uint32_t best = 0;
            float closest = 0.0f;

            for (uint32_t k = 0; k < count; k++) {
                float distance = 0.0f;

                for (uint8_t c = 0; c < channels; c++) {
                    float d = (float) p[c] - center[k * 4u + c];
                    distance += d * d;
                }

                if (k == 0 || distance < closest) {
                    closest = distance;
                    best = k;
                }
            }

            members[best]++;
            for (uint8_t c = 0; c < channels; c++) {
                sum[best * 4u + c] += (float) p[c];
            }
        }

        for (uint32_t k = 0; k < count; k++) {
            if (members[k] == 0) continue;

            for (uint8_t c = 0; c < channels; c++) {
                center[k * 4u + c] = sum[k * 4u + c] / (float) members[k];
            }
        }
    }

    // largest first, so a caller taking the first entry gets the dominant one
    for (uint32_t i = 0; i + 1u < count; i++) {
        for (uint32_t j = i + 1u; j < count; j++) {
            if (members[j] <= members[i]) continue;

            uint32_t swap = members[i];
            members[i] = members[j];
            members[j] = swap;

            for (uint8_t c = 0; c < 4u; c++) {
                float hold = center[i * 4u + c];
                center[i * 4u + c] = center[j * 4u + c];
                center[j * 4u + c] = hold;
            }
        }
    }

    for (uint32_t k = 0; k < count; k++) {
        for (uint8_t c = 0; c < channels; c++) {
            palette[k * channels + c] = tiny_clamp_u8f(center[k * 4u + c]);
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_palette")
int tiny_image_palette(
    const TinyImage* image, uint32_t count, uint8_t* palette
) {
    if (!image || !image->data || !palette) return TINYIMG_ERR_NULL;
    if (count == 0 || count > 256u) return TINYIMG_ERR_RANGE;

    return cluster_colors(image, count, palette);
}

TINYIMG_EXPORT("tiny_image_dominant_color")
int tiny_image_dominant_color(const TinyImage* image, uint8_t* color) {
    if (!image || !image->data || !color) return TINYIMG_ERR_NULL;

    uint8_t palette[4 * 5];
    int result = cluster_colors(image, 5, palette);
    if (result != TINYIMG_OK) return result;

    for (uint8_t c = 0; c < image->channels; c++) color[c] = palette[c];

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_phash")
int tiny_image_phash(const TinyImage* image, uint64_t* hash) {
    if (!image || !image->data || !hash) return TINYIMG_ERR_NULL;

    // a 32x32 luminance reduction, area averaged from whatever the extent is,
    // so two encodings of the same picture at different sizes reduce to the
    // same grid before the transform sees them
    float grid[32][32];

    for (uint32_t gy = 0; gy < 32u; gy++) {
        for (uint32_t gx = 0; gx < 32u; gx++) {
            uint32_t x0 = gx * image->width / 32u;
            uint32_t x1 = (gx + 1u) * image->width / 32u;
            uint32_t y0 = gy * image->height / 32u;
            uint32_t y1 = (gy + 1u) * image->height / 32u;

            if (x1 == x0) x1 = x0 + 1u;
            if (y1 == y0) y1 = y0 + 1u;
            if (x1 > image->width) x1 = image->width;
            if (y1 > image->height) y1 = image->height;

            uint32_t total = 0;
            uint32_t seen = 0;

            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                    const uint8_t* p =
                        image->data +
                        ((size_t) y * image->width + x) * image->channels;

                    total += pixel_luma(p, image->channels);
                    seen++;
                }
            }

            grid[gy][gx] = seen ? (float) total / (float) seen : 0.0f;
        }
    }

    // the 8x8 low frequency block of the two dimensional transform, built as
    // two one dimensional passes
    float rows[32][8];

    for (uint32_t y = 0; y < 32u; y++) {
        for (uint32_t u = 0; u < 8u; u++) {
            float acc = 0.0f;

            for (uint32_t x = 0; x < 32u; x++) {
                acc += grid[y][x] *
                       tiny_cosf(
                           3.14159265f * (float) u * ((float) x + 0.5f) / 32.0f
                       );
            }

            rows[y][u] = acc;
        }
    }

    float block[8][8];

    for (uint32_t v = 0; v < 8u; v++) {
        for (uint32_t u = 0; u < 8u; u++) {
            float acc = 0.0f;

            for (uint32_t y = 0; y < 32u; y++) {
                acc += rows[y][u] *
                       tiny_cosf(
                           3.14159265f * (float) v * ((float) y + 0.5f) / 32.0f
                       );
            }

            block[v][u] = acc;
        }
    }

    // the median of the block with its DC term left out, because the DC term
    // is the image's mean brightness and including it would make the threshold
    // move with the exposure rather than with the structure
    float flat[63];
    uint32_t at = 0;

    for (uint32_t v = 0; v < 8u; v++) {
        for (uint32_t u = 0; u < 8u; u++) {
            if (u == 0 && v == 0) continue;
            flat[at++] = block[v][u];
        }
    }

    for (uint32_t i = 1; i < 63u; i++) {
        float key = flat[i];
        uint32_t j = i;

        while (j > 0 && flat[j - 1u] > key) {
            flat[j] = flat[j - 1u];
            j--;
        }

        flat[j] = key;
    }

    float median = flat[31];
    uint64_t bits = 0;

    for (uint32_t v = 0; v < 8u; v++) {
        for (uint32_t u = 0; u < 8u; u++) {
            bits <<= 1;
            if (block[v][u] > median) bits |= 1u;
        }
    }

    *hash = bits;
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_phash_distance")
uint32_t tiny_phash_distance(uint64_t first, uint64_t second) {
    uint64_t diff = first ^ second;
    uint32_t count = 0;

    while (diff) {
        diff &= diff - 1u;
        count++;
    }

    return count;
}

#pragma endregion

#pragma region image focus

/** Tiles per axis the detail centroid is measured over. */
#define FOCUS_TILES 16u

/**
 * @brief The centroid of the image's detail, in 0..1 coordinates.
 *
 * Detail is the sum of absolute luminance differences to the right and below,
 * accumulated per tile. A flat sky contributes nothing and a face or a printed
 * label contributes a lot, so the centroid lands on the subject rather than on
 * the middle of the frame.
 *
 * Sixteen tiles per axis rather than per pixel because the answer is a single
 * point and a per-pixel weighting spends the whole image's bandwidth to get the
 * same one; the tiles also keep a single noisy pixel from moving it.
 */
static void detail_centroid(const TinyImage* image, float* x, float* y) {
    double weight[FOCUS_TILES][FOCUS_TILES];

    for (uint32_t ty = 0; ty < FOCUS_TILES; ty++) {
        for (uint32_t tx = 0; tx < FOCUS_TILES; tx++) weight[ty][tx] = 0.0;
    }

    uint8_t channels = image->channels;

    for (uint32_t row = 0; row + 1u < image->height; row++) {
        uint32_t ty = row * FOCUS_TILES / image->height;
        const uint8_t* line =
            image->data + (size_t) row * image->width * channels;
        const uint8_t* next = line + (size_t) image->width * channels;

        for (uint32_t column = 0; column + 1u < image->width; column++) {
            const uint8_t* here = line + (size_t) column * channels;
            uint32_t luma = pixel_luma(here, channels);
            uint32_t right = pixel_luma(here + channels, channels);
            uint32_t below =
                pixel_luma(next + (size_t) column * channels, channels);

            uint32_t dx = luma > right ? luma - right : right - luma;
            uint32_t dy = luma > below ? luma - below : below - luma;

            weight[ty][column * FOCUS_TILES / image->width] +=
                (double) (dx + dy);
        }
    }

    double total = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;

    for (uint32_t ty = 0; ty < FOCUS_TILES; ty++) {
        for (uint32_t tx = 0; tx < FOCUS_TILES; tx++) {
            double w = weight[ty][tx];

            total += w;
            sum_x += w * ((double) tx + 0.5) / (double) FOCUS_TILES;
            sum_y += w * ((double) ty + 0.5) / (double) FOCUS_TILES;
        }
    }

    // a perfectly flat image has no detail to be the centroid of, and the
    // middle is the right answer for one
    if (total <= 0.0) {
        *x = 0.5f;
        *y = 0.5f;
        return;
    }

    *x = (float) (sum_x / total);
    *y = (float) (sum_y / total);
}

/**
 * @brief The center of the detected faces, weighted by confidence.
 *
 * @return int Non-zero when a face was found.
 */
static int face_centroid(const TinyImage* image, float* x, float* y) {
    TinyFaceBox boxes[8];
    uint32_t count = 0;

    if (tiny_image_detect_faces(image, boxes, 8u, &count) != TINYIMG_OK) {
        return 0;
    }
    if (count == 0u) return 0;

    double total = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;

    for (uint32_t i = 0; i < count; i++) {
        // a group of many overlapping detections is more likely to be a face
        // than one that just cleared the threshold, so a group photograph
        // centers on the faces it is surest about
        double w = (double) boxes[i].neighbors;

        total += w;
        sum_x += w * ((double) boxes[i].x + (double) boxes[i].width * 0.5);
        sum_y += w * ((double) boxes[i].y + (double) boxes[i].height * 0.5);
    }

    *x = (float) (sum_x / total / (double) image->width);
    *y = (float) (sum_y / total / (double) image->height);

    return 1;
}

TINYIMG_EXPORT("tiny_image_focus")
int tiny_image_focus(
    const TinyImage* image, TinyImageGravity gravity, float* x, float* y
) {
    if (!image || !image->data || !x || !y) return TINYIMG_ERR_NULL;

    if (gravity == TINYIMG_GRAVITY_FACE && face_centroid(image, x, y)) {
        return TINYIMG_OK;
    }

    if (gravity == TINYIMG_GRAVITY_AUTO || gravity == TINYIMG_GRAVITY_FACE) {
        detail_centroid(image, x, y);
        return TINYIMG_OK;
    }

    switch (gravity) {
        case TINYIMG_GRAVITY_NORTH_WEST:
        case TINYIMG_GRAVITY_WEST:
        case TINYIMG_GRAVITY_SOUTH_WEST: *x = 0.0f; break;
        case TINYIMG_GRAVITY_NORTH_EAST:
        case TINYIMG_GRAVITY_EAST:
        case TINYIMG_GRAVITY_SOUTH_EAST: *x = 1.0f; break;
        default: *x = 0.5f; break;
    }

    switch (gravity) {
        case TINYIMG_GRAVITY_NORTH:
        case TINYIMG_GRAVITY_NORTH_WEST:
        case TINYIMG_GRAVITY_NORTH_EAST: *y = 0.0f; break;
        case TINYIMG_GRAVITY_SOUTH:
        case TINYIMG_GRAVITY_SOUTH_WEST:
        case TINYIMG_GRAVITY_SOUTH_EAST: *y = 1.0f; break;
        default: *y = 0.5f; break;
    }

    return TINYIMG_OK;
}

/**
 * @brief Runs one region effect over every detected face.
 *
 * The two anonymizers differ only in which effect they append, so the
 * detection, the clamping and the no-op-on-nothing rule are written once.
 *
 * @param image The image to change.
 * @param kind TINYIMG_FX_BLUR_REGION or TINYIMG_FX_PIXELATE_REGION.
 * @param amount The effect's first parameter, or zero to size it from the face.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int faces_effect(TinyImage* image, TinyEffectKind kind, float amount) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    TinyFaceBox boxes[16];
    uint32_t count = 0;

    int result = tiny_image_detect_faces(image, boxes, 16u, &count);

    // no cascade and no detection are both "nothing to do" rather than
    // failures, and blurring everything would be the wrong way to fail
    if (result == TINYIMG_ERR_BLOB_MISSING) return TINYIMG_OK;
    if (result != TINYIMG_OK) return result;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t x = tiny_min_u32(boxes[i].x, image->width);
        uint32_t y = tiny_min_u32(boxes[i].y, image->height);
        uint32_t width = tiny_min_u32(boxes[i].width, image->width - x);
        uint32_t height = tiny_min_u32(boxes[i].height, image->height - y);

        if (width == 0u || height == 0u) continue;

        float size = amount > 0.0f ? amount : (float) width / 12.0f;
        if (size < 1.0f) size = 1.0f;

        TinyPlan plan;
        result = tiny_plan_init_image(&plan, image);
        if (result != TINYIMG_OK) return result;

        float params[4] = {size, 0.0f, 0.0f, 0.0f};
        result =
            tiny_plan_effect_rect(&plan, kind, params, x, y, width, height);
        if (result != TINYIMG_OK) return result;

        result = tiny_plan_replace(image, &plan);
        if (result != TINYIMG_OK) return result;
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_blur_faces")
int tiny_image_blur_faces(TinyImage* image, float sigma) {
    return faces_effect(image, TINYIMG_FX_BLUR_REGION, sigma);
}

TINYIMG_EXPORT("tiny_image_pixelate_faces")
int tiny_image_pixelate_faces(TinyImage* image, uint32_t size) {
    return faces_effect(image, TINYIMG_FX_PIXELATE_REGION, (float) size);
}

#pragma endregion
