#include "tinyimg/image.h"
#include "tinyimg/memory.h"
#include "tinyimg/plan.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#pragma region shared

#define PI 3.14159265358979f

/** How many colour channels an image has, which is all but the alpha. */
static uint8_t colour_channels(const TinyImage* image) {
    return image->channels == 4u   ? 3u
           : image->channels == 2u ? 1u
                                   : image->channels;
}

static uint8_t* pixel_at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** A pixel clamped to the edge, which is what every kernel here reads. */
static const uint8_t* pixel_clamped(
    const TinyImage* image, int32_t x, int32_t y
) {
    int32_t cx = tiny_clampi(x, 0, (int32_t) image->width - 1);
    int32_t cy = tiny_clampi(y, 0, (int32_t) image->height - 1);

    return pixel_at(image, (uint32_t) cx, (uint32_t) cy);
}

/** A bilinear sample, for the warps and the blurs that read off the grid. */
static void sample_bilinear(
    const TinyImage* image, float x, float y, uint8_t* out
) {
    int32_t x0 = (int32_t) tiny_floorf(x);
    int32_t y0 = (int32_t) tiny_floorf(y);
    float fx = x - (float) x0;
    float fy = y - (float) y0;

    const uint8_t* p00 = pixel_clamped(image, x0, y0);
    const uint8_t* p10 = pixel_clamped(image, x0 + 1, y0);
    const uint8_t* p01 = pixel_clamped(image, x0, y0 + 1);
    const uint8_t* p11 = pixel_clamped(image, x0 + 1, y0 + 1);

    for (uint8_t c = 0; c < image->channels; c++) {
        float top = (float) p00[c] + fx * ((float) p10[c] - (float) p00[c]);
        float bottom = (float) p01[c] + fx * ((float) p11[c] - (float) p01[c]);

        out[c] = tiny_clamp_u8f(top + fy * (bottom - top));
    }
}

/**
 * @brief A copy of an image, for the kernels that cannot read and write one
 * buffer.
 *
 * @param image The image to copy.
 * @param out Receives the copy.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int image_clone(const TinyImage* image, TinyImage* out) {
    tiny_memset(out, 0, sizeof(*out));

    int result =
        tiny_image_create(out, image->width, image->height, image->channels);
    if (result != TINYIMG_OK) return result;

    tiny_memcpy(
        out->data, image->data,
        (size_t) image->width * image->height * image->channels
    );
    out->format = image->format;

    return TINYIMG_OK;
}

/** A rectangle clipped to the image, returning zero when nothing is left. */
static int clip_rect(
    const TinyImage* image, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t* out
) {
    if (x >= image->width || y >= image->height) return 0;

    if (width == 0 || width > image->width - x) width = image->width - x;
    if (height == 0 || height > image->height - y) height = image->height - y;

    out[0] = x;
    out[1] = y;
    out[2] = width;
    out[3] = height;

    return width != 0 && height != 0;
}

#pragma endregion

#pragma region convolution

/**
 * @brief Runs a 3x3 kernel over the colour channels.
 *
 * @param image The image, replaced in place.
 * @param kernel Nine weights, row major.
 * @param divisor What the sum is divided by; zero uses the kernel's own sum,
 * or one when that sum is zero.
 * @param offset Added to every output, which is what makes an emboss visible.
 * @param magnitude Non-zero to take the root of the sum of two passes'
 * squares, which is what a gradient means and a single pass cannot express.
 * @param second The second kernel, read only when `magnitude` is set.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int convolve3(
    TinyImage* image, const int32_t* kernel, int32_t divisor, int32_t offset,
    int magnitude, const int32_t* second
) {
    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    if (divisor == 0) {
        divisor = 0;
        for (uint32_t i = 0; i < 9u; i++) divisor += kernel[i];
        if (divisor == 0) divisor = 1;
    }

    uint8_t colours = colour_channels(image);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* out = pixel_at(image, x, y);

            for (uint8_t c = 0; c < colours; c++) {
                int32_t sum = 0;
                int32_t sum2 = 0;

                for (int32_t ky = -1; ky <= 1; ky++) {
                    for (int32_t kx = -1; kx <= 1; kx++) {
                        uint32_t k = (uint32_t) ((ky + 1) * 3 + (kx + 1));
                        int32_t value = pixel_clamped(
                            &source, (int32_t) x + kx, (int32_t) y + ky
                        )[c];

                        sum += kernel[k] * value;
                        if (magnitude) sum2 += second[k] * value;
                    }
                }

                if (magnitude) {
                    float total =
                        (float) sum * (float) sum + (float) sum2 * (float) sum2;
                    out[c] = tiny_clamp_u8f(tiny_sqrtf(total));
                }
                else {
                    out[c] = tiny_clamp_u8(sum / divisor + offset);
                }
            }
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region sharpening

/**
 * @brief Adds back a multiple of what a blur removed.
 *
 * @param image The image, replaced in place.
 * @param sigma The blur's standard deviation.
 * @param amount How much of the difference to add.
 * @param threshold Differences at or below this are left alone, which is what
 * keeps a sharpen from amplifying noise in flat areas.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int unsharp(
    TinyImage* image, float sigma, float amount, float threshold
) {
    TinyImage blurred;
    int result = image_clone(image, &blurred);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_gaussian_blur(&blurred, sigma);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&blurred);
        return result;
    }

    uint8_t colours = colour_channels(image);
    size_t pixels = (size_t) image->width * image->height;
    int32_t limit = (int32_t) threshold;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* out = image->data + i * image->channels;
        const uint8_t* soft = blurred.data + i * blurred.channels;

        for (uint8_t c = 0; c < colours; c++) {
            int32_t diff = (int32_t) out[c] - (int32_t) soft[c];
            if (diff > -limit && diff < limit) continue;

            out[c] = tiny_clamp_u8f((float) out[c] + amount * (float) diff);
        }
    }

    tiny_image_destroy(&blurred);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region blocks and medians

/** Averages each block and writes the average back over it. */
static int pixelate(TinyImage* image, uint32_t size, const uint32_t* rect) {
    if (size < 2u) return TINYIMG_OK;

    for (uint32_t by = rect[1]; by < rect[1] + rect[3]; by += size) {
        for (uint32_t bx = rect[0]; bx < rect[0] + rect[2]; bx += size) {
            uint32_t x1 = tiny_min_u32(bx + size, rect[0] + rect[2]);
            uint32_t y1 = tiny_min_u32(by + size, rect[1] + rect[3]);
            uint32_t count = (x1 - bx) * (y1 - by);
            uint32_t total[4] = {0, 0, 0, 0};

            for (uint32_t y = by; y < y1; y++) {
                for (uint32_t x = bx; x < x1; x++) {
                    const uint8_t* p = pixel_at(image, x, y);
                    for (uint8_t c = 0; c < image->channels; c++) {
                        total[c] += p[c];
                    }
                }
            }

            uint8_t mean[4];
            for (uint8_t c = 0; c < image->channels; c++) {
                mean[c] = (uint8_t) ((total[c] + count / 2u) / count);
            }

            for (uint32_t y = by; y < y1; y++) {
                for (uint32_t x = bx; x < x1; x++) {
                    uint8_t* p = pixel_at(image, x, y);
                    for (uint8_t c = 0; c < image->channels; c++) {
                        p[c] = mean[c];
                    }
                }
            }
        }
    }

    return TINYIMG_OK;
}

/** The median of a 3x3 neighbourhood, per channel. */
static int median3(TinyImage* image) {
    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    uint8_t colours = colour_channels(image);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* out = pixel_at(image, x, y);

            for (uint8_t c = 0; c < colours; c++) {
                uint8_t window[9];
                uint32_t at = 0;

                for (int32_t ky = -1; ky <= 1; ky++) {
                    for (int32_t kx = -1; kx <= 1; kx++) {
                        window[at++] = pixel_clamped(
                            &source, (int32_t) x + kx, (int32_t) y + ky
                        )[c];
                    }
                }

                // an insertion sort of nine values, which beats a partial
                // selection network in code size and is not the hot loop
                for (uint32_t i = 1; i < 9u; i++) {
                    uint8_t key = window[i];
                    uint32_t j = i;

                    while (j > 0 && window[j - 1u] > key) {
                        window[j] = window[j - 1u];
                        j--;
                    }

                    window[j] = key;
                }

                out[c] = window[4];
            }
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

/**
 * @brief Replaces each pixel with the extreme of its neighbourhood.
 *
 * @param image The image, replaced in place.
 * @param radius Pixels either side.
 * @param maximum Non-zero to dilate, zero to erode.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int morphology(TinyImage* image, uint32_t radius, int maximum) {
    if (radius == 0) return TINYIMG_OK;

    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    uint8_t colours = colour_channels(image);
    int32_t r = (int32_t) radius;

    // separable, because the extreme over a square is the extreme of the
    // extremes over its rows; two O(r) passes rather than one O(r^2)
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t y = 0; y < image->height; y++) {
            for (uint32_t x = 0; x < image->width; x++) {
                uint8_t* out = pixel_at(image, x, y);

                for (uint8_t c = 0; c < colours; c++) {
                    int32_t best = maximum ? 0 : 255;

                    for (int32_t k = -r; k <= r; k++) {
                        int32_t value =
                            pass == 0
                                ? pixel_clamped(
                                      &source, (int32_t) x + k, (int32_t) y
                                  )[c]
                                : pixel_clamped(
                                      &source, (int32_t) x, (int32_t) y + k
                                  )[c];

                        if (maximum ? value > best : value < best) best = value;
                    }

                    out[c] = (uint8_t) best;
                }
            }
        }

        if (pass == 0) {
            tiny_memcpy(
                source.data, image->data,
                (size_t) image->width * image->height * image->channels
            );
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

/** The difference between a dilation and an erosion, which is an edge map. */
static int outline(TinyImage* image, uint32_t radius) {
    if (radius == 0) return TINYIMG_OK;

    TinyImage eroded;
    int result = image_clone(image, &eroded);
    if (result != TINYIMG_OK) return result;

    result = morphology(&eroded, radius, 0);
    if (result == TINYIMG_OK) result = morphology(image, radius, 1);

    if (result != TINYIMG_OK) {
        tiny_image_destroy(&eroded);
        return result;
    }

    uint8_t colours = colour_channels(image);
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* out = image->data + i * image->channels;
        const uint8_t* low = eroded.data + i * eroded.channels;

        for (uint8_t c = 0; c < colours; c++) {
            out[c] = (uint8_t) (out[c] - low[c]);
        }
    }

    tiny_image_destroy(&eroded);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region directional blurs

/**
 * @brief Averages along a path each pixel walks, which is every blur that has
 * a direction.
 *
 * One loop for the three of them, because they differ only in where the k-th
 * sample of a pixel comes from: a straight line for a motion blur, an arc for
 * a radial one, a ray for a zoom. Writing them separately would be the same
 * accumulator three times.
 *
 * @param image The image, replaced in place.
 * @param kind Which of the three.
 * @param strength Length in pixels, or the angle or scale span.
 * @param angle Degrees, for the motion blur only.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int directional(
    TinyImage* image, TinyEffectKind kind, float strength, float angle
) {
    if (strength <= 0.0f) return TINYIMG_OK;

    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    uint32_t steps = (uint32_t) (strength + 0.5f);
    if (steps < 1u) steps = 1u;
    if (steps > 128u) steps = 128u;

    float centre_x = (float) image->width * 0.5f;
    float centre_y = (float) image->height * 0.5f;
    float radians = angle * PI / 180.0f;
    float step_x = tiny_cosf(radians);
    float step_y = tiny_sinf(radians);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float dx = (float) x - centre_x;
            float dy = (float) y - centre_y;
            uint32_t total[4] = {0, 0, 0, 0};

            for (uint32_t k = 0; k <= steps; k++) {
                float t = (float) k / (float) steps;
                float sx;
                float sy;

                if (kind == TINYIMG_FX_MOTION_BLUR) {
                    float offset = (t - 0.5f) * strength;
                    sx = (float) x + offset * step_x;
                    sy = (float) y + offset * step_y;
                }
                else if (kind == TINYIMG_FX_RADIAL_BLUR) {
                    float turn = (t - 0.5f) * strength * PI / 180.0f;
                    float c = tiny_cosf(turn);
                    float s = tiny_sinf(turn);

                    sx = centre_x + dx * c - dy * s;
                    sy = centre_y + dx * s + dy * c;
                }
                else {
                    // the scale spans one either side of unity, so the centre
                    // pixel contributes once rather than every step
                    float scale = 1.0f + (t - 0.5f) * strength * 0.01f;
                    sx = centre_x + dx * scale;
                    sy = centre_y + dy * scale;
                }

                uint8_t sampled[4];
                sample_bilinear(&source, sx, sy, sampled);

                for (uint8_t c = 0; c < image->channels; c++) {
                    total[c] += sampled[c];
                }
            }

            uint8_t* out = pixel_at(image, x, y);
            for (uint8_t c = 0; c < image->channels; c++) {
                out[c] =
                    (uint8_t) ((total[c] + (steps + 1u) / 2u) / (steps + 1u));
            }
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

/**
 * @brief Blends a blurred copy in by distance from a band, which is a tilt
 * shift.
 *
 * @param image The image, replaced in place.
 * @param sigma The blur at the furthest point.
 * @param band How much of the height stays sharp, as a fraction.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int tilt_shift(TinyImage* image, float sigma, float band) {
    TinyImage blurred;
    int result = image_clone(image, &blurred);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_gaussian_blur(&blurred, sigma);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&blurred);
        return result;
    }

    if (band <= 0.0f) band = 0.25f;

    float centre = (float) image->height * 0.5f;
    float sharp = (float) image->height * band * 0.5f;
    float falloff = centre - sharp;
    if (falloff <= 0.0f) falloff = 1.0f;

    for (uint32_t y = 0; y < image->height; y++) {
        float distance = tiny_fabsf((float) y + 0.5f - centre);
        float t = tiny_clampf((distance - sharp) / falloff, 0.0f, 1.0f);
        uint32_t weight = (uint32_t) (t * 256.0f + 0.5f);

        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* out = pixel_at(image, x, y);
            const uint8_t* soft = pixel_at(&blurred, x, y);

            for (uint8_t c = 0; c < image->channels; c++) {
                out[c] = (uint8_t) (((256u - weight) * out[c] +
                                     weight * soft[c] + 128u) >>
                                    8);
            }
        }
    }

    tiny_image_destroy(&blurred);
    return TINYIMG_OK;
}

/** Blurs inside a rectangle only, by blurring a copy and pasting it back. */
static int blur_region(TinyImage* image, float sigma, const uint32_t* rect) {
    TinyImage blurred;
    int result = image_clone(image, &blurred);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_gaussian_blur(&blurred, sigma);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&blurred);
        return result;
    }

    // the whole image is blurred and the rectangle copied back, rather than
    // blurring the rectangle alone: a blur that reads only its own rectangle
    // has nothing outside the edge and darkens along it
    for (uint32_t y = rect[1]; y < rect[1] + rect[3]; y++) {
        tiny_memcpy(
            pixel_at(image, rect[0], y), pixel_at(&blurred, rect[0], y),
            (size_t) rect[2] * image->channels
        );
    }

    tiny_image_destroy(&blurred);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region patterns

/**
 * @brief Shifts the red and blue channels apart, which is a lens aberration.
 *
 * @param image The image, replaced in place.
 * @param amount Pixels of separation at the edge.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int chromatic(TinyImage* image, float amount) {
    if (image->channels < 3u) return TINYIMG_OK;

    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    float centre_x = (float) image->width * 0.5f;
    float centre_y = (float) image->height * 0.5f;
    float span = tiny_sqrtf(centre_x * centre_x + centre_y * centre_y);
    if (span <= 0.0f) span = 1.0f;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float dx = (float) x - centre_x;
            float dy = (float) y - centre_y;
            float scale = amount / span;
            uint8_t* out = pixel_at(image, x, y);
            uint8_t red[4];
            uint8_t blue[4];

            sample_bilinear(
                &source, (float) x + dx * scale, (float) y + dy * scale, red
            );
            sample_bilinear(
                &source, (float) x - dx * scale, (float) y - dy * scale, blue
            );

            out[0] = red[0];
            out[2] = blue[2];
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

/** The 8x8 Bayer matrix, generated rather than carried as a table. */
static uint32_t bayer8(uint32_t x, uint32_t y) {
    uint32_t value = 0;

    // interleaving the bits of x^y and y is the recursive construction of the
    // matrix written out; the table it would replace is 64 bytes
    for (uint32_t bit = 0; bit < 3u; bit++) {
        uint32_t shift = 2u - bit;

        value |= (((y ^ x) >> shift) & 1u) << (2u * bit + 1u);
        value |= ((x >> shift) & 1u) << (2u * bit);
    }

    return value;
}

/**
 * @brief Quantises with a Bayer threshold, which trades banding for texture.
 *
 * @param image The image, replaced in place.
 * @param levels How many output levels per channel.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int dither(TinyImage* image, uint32_t levels) {
    if (levels < 2u) levels = 2u;
    if (levels >= 256u) return TINYIMG_OK;

    uint8_t colours = colour_channels(image);
    float step = 255.0f / (float) (levels - 1u);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* p = pixel_at(image, x, y);
            float bias = ((float) bayer8(x & 7u, y & 7u) + 0.5f) / 64.0f - 0.5f;

            for (uint8_t c = 0; c < colours; c++) {
                float value = (float) p[c] + bias * step;
                float index = tiny_roundf(value / step);

                p[c] = tiny_clamp_u8f(index * step);
            }
        }
    }

    return TINYIMG_OK;
}

/**
 * @brief Turns tone into dot area within a cell, which is a halftone screen.
 *
 * @param image The image, replaced in place.
 * @param cell Cell size in pixels.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int halftone(TinyImage* image, uint32_t cell) {
    if (cell < 2u) return TINYIMG_OK;

    TinyImage source;
    int result = image_clone(image, &source);
    if (result != TINYIMG_OK) return result;

    uint8_t colours = colour_channels(image);
    float half = (float) cell * 0.5f;

    for (uint32_t cy = 0; cy < image->height; cy += cell) {
        for (uint32_t cx = 0; cx < image->width; cx += cell) {
            uint32_t x1 = tiny_min_u32(cx + cell, image->width);
            uint32_t y1 = tiny_min_u32(cy + cell, image->height);
            uint32_t count = (x1 - cx) * (y1 - cy);
            uint32_t total[3] = {0, 0, 0};

            for (uint32_t y = cy; y < y1; y++) {
                for (uint32_t x = cx; x < x1; x++) {
                    const uint8_t* p = pixel_at(&source, x, y);
                    for (uint8_t c = 0; c < colours; c++) total[c] += p[c];
                }
            }

            for (uint32_t y = cy; y < y1; y++) {
                for (uint32_t x = cx; x < x1; x++) {
                    uint8_t* p = pixel_at(image, x, y);
                    float dx = (float) (x - cx) + 0.5f - half;
                    float dy = (float) (y - cy) + 0.5f - half;
                    float distance = tiny_sqrtf(dx * dx + dy * dy);

                    for (uint8_t c = 0; c < colours; c++) {
                        float mean = (float) total[c] / (float) count;
                        // the dot's area is the tone, so its radius is the
                        // root: a linear radius would darken the midtones
                        float area = 1.0f - mean / 255.0f;
                        float radius = half * tiny_sqrtf(area);

                        p[c] = distance <= radius ? 0u : 255u;
                    }
                }
            }
        }
    }

    tiny_image_destroy(&source);
    return TINYIMG_OK;
}

/** Darkens every nth row, which is a CRT scanline. */
static int scanlines(TinyImage* image, uint32_t period, float strength) {
    if (period < 2u) return TINYIMG_OK;

    uint8_t colours = colour_channels(image);
    uint32_t keep =
        (uint32_t) (tiny_clampf(1.0f - strength, 0.0f, 1.0f) * 256.0f + 0.5f);

    for (uint32_t y = 0; y < image->height; y++) {
        if (y % period != 0u) continue;

        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* p = pixel_at(image, x, y);
            for (uint8_t c = 0; c < colours; c++) {
                p[c] = (uint8_t) ((p[c] * keep + 128u) >> 8);
            }
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region colour

/** Rec. 709 luminance weights, the same ones the planner's matrices use. */
static const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};

/** A pixel's luminance from three channels. */
static float luma_of(const uint8_t* color) {
    return LUMA[0] * (float) color[0] + LUMA[1] * (float) color[1] +
           LUMA[2] * (float) color[2];
}

static void matrix_identity(float* m) {
    for (uint32_t i = 0; i < 12u; i++) m[i] = 0.0f;

    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
}

/** Runs one colour matrix over an image as its own plan. */
static int run_matrix(TinyImage* image, const float* m) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_matrix(&plan, m);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

/** Runs one tone curve over an image as its own plan. */
static int run_curve(
    TinyImage* image, TinyCurveKind kind, const float* p, uint8_t channels
) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_curve(&plan, kind, p, channels);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

/** Runs one neighbourhood effect over an image as its own plan. */
static int run_effect(TinyImage* image, TinyEffectKind kind, const float* p) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_effect(&plan, kind, p);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_apply_matrix")
int tiny_image_apply_matrix(TinyImage* image, const float* matrix) {
    if (!image || !matrix) return TINYIMG_ERR_NULL;
    return run_matrix(image, matrix);
}

TINYIMG_EXPORT("tiny_image_apply_luts")
int tiny_image_apply_luts(
    TinyImage* image, const uint8_t* red, const uint8_t* green,
    const uint8_t* blue
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    const uint8_t* table[3] = {red, green, blue};
    uint8_t colours = colour_channels(image);
    size_t pixels = (size_t) image->width * image->height;

    // a caller's own table cannot be carried in an operation, so this is the
    // one colour path that runs eagerly rather than through the planner. it is
    // the escape hatch under the named adjustments, which do collapse
    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;

        for (uint8_t c = 0; c < colours; c++) {
            const uint8_t* lut = colours < 3u ? table[1] : table[c];
            if (lut) p[c] = lut[p[c]];
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_apply_lut")
int tiny_image_apply_lut(TinyImage* image, const uint8_t* lut) {
    if (!lut) return TINYIMG_ERR_NULL;
    return tiny_image_apply_luts(image, lut, lut, lut);
}

TINYIMG_EXPORT("tiny_image_curves")
int tiny_image_curves(
    TinyImage* image, const uint8_t* x_points, const uint8_t* y_points,
    size_t num_points
) {
    if (!image || !x_points || !y_points) return TINYIMG_ERR_NULL;
    if (num_points < 2u) return TINYIMG_ERR_RANGE;

    for (size_t i = 1; i < num_points; i++) {
        if (x_points[i] <= x_points[i - 1u]) return TINYIMG_ERR_RANGE;
    }

    uint8_t lut[256];

    for (uint32_t v = 0; v < 256u; v++) {
        if (v <= x_points[0]) {
            lut[v] = y_points[0];
            continue;
        }
        if (v >= x_points[num_points - 1u]) {
            lut[v] = y_points[num_points - 1u];
            continue;
        }

        size_t at = 0;
        while (at + 1u < num_points && x_points[at + 1u] < v) at++;

        // linear between the control points, which is monotone between rising
        // points by construction. a cubic spline through the same points dips
        // between them, and the dip shows as a band in a gradient
        float span = (float) x_points[at + 1u] - (float) x_points[at];
        float t = ((float) v - (float) x_points[at]) / span;

        lut[v] = tiny_clamp_u8f(
            (float) y_points[at] +
            t * ((float) y_points[at + 1u] - (float) y_points[at])
        );
    }

    return tiny_image_apply_lut(image, lut);
}

TINYIMG_EXPORT("tiny_image_negate")
int tiny_image_negate(TinyImage* image) {
    return tiny_image_invert(image);
}

TINYIMG_EXPORT("tiny_image_blackwhite")
int tiny_image_blackwhite(TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;

    float m[12];
    matrix_identity(m);

    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) m[row * 4u + col] = LUMA[col];
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_colorize")
int tiny_image_colorize(
    TinyImage* image, const uint8_t* color, float strength
) {
    if (!image || !color) return TINYIMG_ERR_NULL;
    if (strength < 0.0f || strength > 1.0f) return TINYIMG_ERR_RANGE;

    float m[12];
    matrix_identity(m);

    for (uint32_t row = 0; row < 3u; row++) {
        float target = (float) color[row] / 255.0f;

        for (uint32_t col = 0; col < 3u; col++) {
            float keep = row == col ? 1.0f - strength : 0.0f;

            m[row * 4u + col] = keep + strength * target * LUMA[col];
        }
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_tint")
int tiny_image_tint(TinyImage* image, const uint8_t* color, float strength) {
    if (!image || !color) return TINYIMG_ERR_NULL;
    if (strength < -1.0f || strength > 1.0f) return TINYIMG_ERR_RANGE;

    float m[12];
    matrix_identity(m);

    // the colour's own luminance is subtracted from the offset, so the
    // weighted sum of the three offsets is exactly zero and the cast carries
    // no brightness change with it
    float neutral = luma_of(color);

    for (uint32_t row = 0; row < 3u; row++) {
        m[row * 4u + 3u] = strength * ((float) color[row] - neutral);
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_posterize")
int tiny_image_posterize(TinyImage* image, uint32_t levels) {
    float p[5] = {(float) levels, 0.0f, 0.0f, 0.0f, 0.0f};
    return run_curve(image, TINYIMG_CURVE_POSTERIZE, p, 0);
}

TINYIMG_EXPORT("tiny_image_threshold")
int tiny_image_threshold(TinyImage* image, uint8_t level) {
    float p[5] = {(float) level, 0.0f, 0.0f, 0.0f, 0.0f};
    return run_curve(image, TINYIMG_CURVE_THRESHOLD, p, 0);
}

TINYIMG_EXPORT("tiny_image_solarize")
int tiny_image_solarize(TinyImage* image, uint8_t level) {
    float p[5] = {(float) level, 0.0f, 0.0f, 0.0f, 0.0f};
    return run_curve(image, TINYIMG_CURVE_SOLARIZE, p, 0);
}

TINYIMG_EXPORT("tiny_image_duotone")
int tiny_image_duotone(
    TinyImage* image, const uint8_t* shadow, const uint8_t* highlight
) {
    if (!image || !shadow || !highlight) return TINYIMG_ERR_NULL;

    float m[12];
    matrix_identity(m);

    for (uint32_t row = 0; row < 3u; row++) {
        float range = ((float) highlight[row] - (float) shadow[row]) / 255.0f;

        for (uint32_t col = 0; col < 3u; col++) {
            m[row * 4u + col] = range * LUMA[col];
        }

        m[row * 4u + 3u] = (float) shadow[row];
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_split_tone")
int tiny_image_split_tone(
    TinyImage* image, const uint8_t* shadow, const uint8_t* highlight,
    float balance
) {
    if (!image || !shadow || !highlight) return TINYIMG_ERR_NULL;
    if (balance < 0.0f || balance > 1.0f) return TINYIMG_ERR_RANGE;

    float m[12];
    matrix_identity(m);

    // the ramp between the two casts is linear in the luminance rather than a
    // smoothstep, because a smoothstep is quadratic in the pixel and so
    // cannot be one matrix; the difference is a slightly softer transition
    float shift = 0.5f - balance;

    for (uint32_t row = 0; row < 3u; row++) {
        float low = ((float) shadow[row] - 128.0f) / 255.0f;
        float high = ((float) highlight[row] - 128.0f) / 255.0f;

        for (uint32_t col = 0; col < 3u; col++) {
            m[row * 4u + col] =
                (row == col ? 1.0f : 0.0f) + (high - low) * LUMA[col];
        }

        m[row * 4u + 3u] = 255.0f * (low + (high - low) * shift);
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_exposure")
int tiny_image_exposure(TinyImage* image, float stops) {
    float p[5] = {stops, 0.0f, 0.0f, 0.0f, 0.0f};
    return run_curve(image, TINYIMG_CURVE_EXPOSURE, p, 0);
}

TINYIMG_EXPORT("tiny_image_fill_light")
int tiny_image_fill_light(TinyImage* image, float amount) {
    float p[5] = {amount, 0.0f, 0.0f, 0.0f, 0.0f};
    return run_curve(image, TINYIMG_CURVE_FILL_LIGHT, p, 0);
}

/**
 * @brief The channel gains that move the white point.
 *
 * @param out Receives three gains.
 * @param temperature -1 cool through 1 warm.
 * @param tint -1 green through 1 magenta.
 */
static void balance_gains(float* out, float temperature, float tint) {
    // a small linear approximation of the blackbody locus over the range a
    // caller would ask for, which is what every editor's slider is; a full
    // Planckian fit would need a table and would move the same three gains
    out[0] = 1.0f + 0.30f * temperature;
    out[1] = 1.0f - 0.15f * tint;
    out[2] = 1.0f - 0.30f * temperature;

    out[0] += 0.05f * tint;
    out[2] += 0.05f * tint;
}

TINYIMG_EXPORT("tiny_image_white_balance")
int tiny_image_white_balance(TinyImage* image, float temperature, float tint) {
    if (!image) return TINYIMG_ERR_NULL;
    if (temperature < -1.0f || temperature > 1.0f) return TINYIMG_ERR_RANGE;
    if (tint < -1.0f || tint > 1.0f) return TINYIMG_ERR_RANGE;

    float gain[3];
    balance_gains(gain, temperature, tint);

    float m[12];
    matrix_identity(m);

    for (uint32_t row = 0; row < 3u; row++) m[row * 4u + row] = gain[row];

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_temperature")
int tiny_image_temperature(TinyImage* image, float amount) {
    return tiny_image_white_balance(image, amount, 0.0f);
}

TINYIMG_EXPORT("tiny_image_vibrance")
int tiny_image_vibrance(TinyImage* image, float amount) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (amount < -1.0f || amount > 2.0f) return TINYIMG_ERR_RANGE;
    if (image->channels < 3u) return TINYIMG_OK;

    size_t pixels = (size_t) image->width * image->height;

    // not affine in the pixel, because how much it lifts depends on how
    // saturated the pixel already is, so it cannot be a matrix and runs as its
    // own pass. that is the whole difference from a saturation change
    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        float grey = luma_of(p);

        uint8_t low = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2])
                                  : (p[1] < p[2] ? p[1] : p[2]);
        uint8_t high = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2])
                                   : (p[1] > p[2] ? p[1] : p[2]);

        float saturation =
            high == 0u ? 0.0f : (float) (high - low) / (float) high;
        float factor = 1.0f + amount * (1.0f - saturation);

        for (uint8_t c = 0; c < 3u; c++) {
            p[c] = tiny_clamp_u8f(grey + ((float) p[c] - grey) * factor);
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_levels_channel")
int tiny_image_levels_channel(
    TinyImage* image, uint8_t channel, float in_black, float in_white,
    float gamma, float out_black, float out_white
) {
    if (channel > 2u) return TINYIMG_ERR_RANGE;

    float p[5] = {in_black, in_white, gamma, out_black, out_white};
    return run_curve(image, TINYIMG_CURVE_LEVELS, p, (uint8_t) (1u << channel));
}

TINYIMG_EXPORT("tiny_image_levels")
int tiny_image_levels(
    TinyImage* image, float in_black, float in_white, float gamma,
    float out_black, float out_white
) {
    float p[5] = {in_black, in_white, gamma, out_black, out_white};
    return run_curve(image, TINYIMG_CURVE_LEVELS, p, 0);
}

TINYIMG_EXPORT("tiny_image_color_balance")
int tiny_image_color_balance(
    TinyImage* image, const float* shadows, const float* midtones,
    const float* highlights
) {
    if (!image || !shadows || !midtones || !highlights) {
        return TINYIMG_ERR_NULL;
    }

    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    // one curve per channel, so the three collapse into one table between them
    // rather than costing a pass each
    for (uint32_t c = 0; c < 3u; c++) {
        float p[5] = {shadows[c], midtones[c], highlights[c], 0.0f, 0.0f};

        result = tiny_plan_curve(
            &plan, TINYIMG_CURVE_BALANCE, p, (uint8_t) (1u << c)
        );
        if (result != TINYIMG_OK) return result;
    }

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_channel_mixer")
int tiny_image_channel_mixer(TinyImage* image, const float* matrix) {
    if (!image || !matrix) return TINYIMG_ERR_NULL;

    float m[12];
    matrix_identity(m);

    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            m[row * 4u + col] = matrix[row * 3u + col];
        }
    }

    return run_matrix(image, m);
}

TINYIMG_EXPORT("tiny_image_channel_gain")
int tiny_image_channel_gain(
    TinyImage* image, float red, float green, float blue
) {
    if (!image) return TINYIMG_ERR_NULL;
    if (red < 0.0f || green < 0.0f || blue < 0.0f) return TINYIMG_ERR_RANGE;

    float m[12];
    matrix_identity(m);

    m[0] = red;
    m[5] = green;
    m[10] = blue;

    return run_matrix(image, m);
}

/**
 * @brief The projection a given colour blindness performs.
 *
 * The Brettel, Vienot and Mollon matrices in sRGB, which is what browser
 * accessibility tooling applies. Achromatopsia is the luminance projection.
 *
 * @param out Receives nine weights, row major.
 * @param kind Which form.
 * @return int TINYIMG_OK or TINYIMG_ERR_RANGE.
 */
static int colorblind_matrix(float* out, TinyColorblindKind kind) {
    static const float PROTAN[9] = {0.567f, 0.433f, 0.0f,   0.558f, 0.442f,
                                    0.0f,   0.0f,   0.242f, 0.758f};
    static const float DEUTAN[9] = {0.625f, 0.375f, 0.0f, 0.7f, 0.3f,
                                    0.0f,   0.0f,   0.3f, 0.7f};
    static const float TRITAN[9] = {0.95f,  0.05f, 0.0f,   0.0f,  0.433f,
                                    0.567f, 0.0f,  0.475f, 0.525f};

    const float* source;

    switch (kind) {
        case TINYIMG_COLORBLIND_PROTANOPIA: source = PROTAN; break;
        case TINYIMG_COLORBLIND_DEUTERANOPIA: source = DEUTAN; break;
        case TINYIMG_COLORBLIND_TRITANOPIA: source = TRITAN; break;
        case TINYIMG_COLORBLIND_ACHROMATOPSIA:
            for (uint32_t row = 0; row < 3u; row++) {
                for (uint32_t col = 0; col < 3u; col++) {
                    out[row * 3u + col] = LUMA[col];
                }
            }
            return TINYIMG_OK;
        default: return TINYIMG_ERR_RANGE;
    }

    for (uint32_t i = 0; i < 9u; i++) out[i] = source[i];

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_colorblind_simulate")
int tiny_image_colorblind_simulate(TinyImage* image, TinyColorblindKind kind) {
    if (!image) return TINYIMG_ERR_NULL;

    float projection[9];
    int result = colorblind_matrix(projection, kind);
    if (result != TINYIMG_OK) return result;

    return tiny_image_channel_mixer(image, projection);
}

TINYIMG_EXPORT("tiny_image_colorblind_assist")
int tiny_image_colorblind_assist(TinyImage* image, TinyColorblindKind kind) {
    if (!image) return TINYIMG_ERR_NULL;

    float projection[9];
    int result = colorblind_matrix(projection, kind);
    if (result != TINYIMG_OK) return result;

    // the error the projection throws away, added back along the channels the
    // form can still tell apart. the result is not the original colours and is
    // not meant to be: it is an image whose distinctions survive the viewer's
    // own projection
    static const float SPREAD[9] = {0.0f, 0.0f, 0.0f, 0.7f, 1.0f,
                                    0.0f, 0.7f, 0.0f, 1.0f};
    float mixer[9];

    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            float error =
                (row == col ? 1.0f : 0.0f) - projection[row * 3u + col];
            float added = 0.0f;

            for (uint32_t k = 0; k < 3u; k++) {
                added += SPREAD[row * 3u + k] *
                         ((k == col ? 1.0f : 0.0f) - projection[k * 3u + col]);
            }

            mixer[row * 3u + col] = (row == col ? 1.0f : 0.0f) - error + added;
        }
    }

    return tiny_image_channel_mixer(image, mixer);
}

TINYIMG_EXPORT("tiny_image_preset")
int tiny_image_preset(TinyImage* image, TinyImagePreset preset) {
    if (!image) return TINYIMG_ERR_NULL;

    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    float m[12];
    float p[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // each of these is a short stack of the adjustments above, appended to one
    // plan so the whole look collapses into one matrix and one table
    switch (preset) {
        case TINYIMG_PRESET_NOIR:
            result = tiny_plan_saturation(&plan, 0.0f);
            if (result == TINYIMG_OK) result = tiny_plan_contrast(&plan, 1.45f);
            break;
        case TINYIMG_PRESET_CHROME:
            result = tiny_plan_saturation(&plan, 0.0f);
            if (result == TINYIMG_OK) {
                p[0] = 12.0f;
                p[1] = 235.0f;
                p[2] = 1.0f;
                p[3] = 18.0f;
                p[4] = 255.0f;
                result = tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, p, 0);
            }
            if (result == TINYIMG_OK) {
                matrix_identity(m);
                m[10] = 1.08f;
                result = tiny_plan_matrix(&plan, m);
            }
            break;
        case TINYIMG_PRESET_MONO:
            result = tiny_plan_saturation(&plan, 0.0f);
            break;
        case TINYIMG_PRESET_FADE:
            p[0] = 0.0f;
            p[1] = 255.0f;
            p[2] = 1.0f;
            p[3] = 28.0f;
            p[4] = 232.0f;
            result = tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, p, 0);
            if (result == TINYIMG_OK) {
                result = tiny_plan_saturation(&plan, 0.75f);
            }
            break;
        case TINYIMG_PRESET_VIVID:
            result = tiny_plan_saturation(&plan, 1.4f);
            if (result == TINYIMG_OK) result = tiny_plan_contrast(&plan, 1.15f);
            break;
        case TINYIMG_PRESET_WARM:
        case TINYIMG_PRESET_COOL: {
            float gain[3];
            balance_gains(
                gain, preset == TINYIMG_PRESET_WARM ? 0.35f : -0.35f, 0.0f
            );

            matrix_identity(m);
            for (uint32_t row = 0; row < 3u; row++) {
                m[row * 4u + row] = gain[row];
            }

            result = tiny_plan_matrix(&plan, m);
            break;
        }
        case TINYIMG_PRESET_INSTANT: {
            float gain[3];
            balance_gains(gain, 0.25f, 0.1f);

            matrix_identity(m);
            for (uint32_t row = 0; row < 3u; row++) {
                m[row * 4u + row] = gain[row];
            }

            result = tiny_plan_matrix(&plan, m);
            if (result == TINYIMG_OK) {
                p[0] = 0.0f;
                p[1] = 248.0f;
                p[2] = 1.1f;
                p[3] = 22.0f;
                p[4] = 244.0f;
                result = tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, p, 0);
            }
            if (result == TINYIMG_OK) {
                result = tiny_plan_saturation(&plan, 0.85f);
            }
            break;
        }
        case TINYIMG_PRESET_TONAL:
            p[0] = 0.0f;
            p[1] = 255.0f;
            p[2] = 1.0f;
            p[3] = 34.0f;
            p[4] = 216.0f;
            result = tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, p, 0);
            if (result == TINYIMG_OK) {
                result = tiny_plan_saturation(&plan, 0.9f);
            }
            break;
        default: return TINYIMG_ERR_RANGE;
    }

    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

#pragma endregion

#pragma region base filters

TINYIMG_EXPORT("tiny_image_apply_sepia")
int tiny_image_apply_sepia(TinyImage* image) {
    if (!image) return TINYIMG_ERR_NULL;

    // the 3x3 everyone quotes, from the Microsoft imaging documentation. note
    // that magick's own -sepia-tone is a tone-mapped duotone rather than this,
    // so the two are different operations and comparing them is not a
    // measurement of either
    static const float SEPIA[9] = {0.393f, 0.769f, 0.189f, 0.349f, 0.686f,
                                   0.168f, 0.272f, 0.534f, 0.131f};

    return tiny_image_channel_mixer(image, SEPIA);
}

/**
 * @brief Mixes a rectangle toward a colour.
 *
 * The one implementation under darken, lighten and the colour overlays: each
 * is a weighted move toward a fixed colour, and they differ only in which
 * colour and how the weight is named. A rectangle covering the whole image
 * goes through the planner as a matrix, so it composes with the adjustments
 * around it; a smaller one cannot, because the planner's colour class is for
 * operations that read one pixel wherever it is.
 *
 * @param image The image to change.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width; zero means to the right edge.
 * @param height Height; zero means to the bottom edge.
 * @param color Three channels to move toward.
 * @param weight How far, 0 through 1.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int mix_toward(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* color, float weight
) {
    uint32_t rect[4];
    if (!clip_rect(image, x, y, width, height, rect)) return TINYIMG_OK;

    if (rect[0] == 0 && rect[1] == 0 && rect[2] == image->width &&
        rect[3] == image->height) {
        float m[12];
        matrix_identity(m);

        for (uint32_t row = 0; row < 3u; row++) {
            m[row * 4u + row] = 1.0f - weight;
            m[row * 4u + 3u] = weight * (float) color[row];
        }

        return run_matrix(image, m);
    }

    uint8_t colours = colour_channels(image);
    uint32_t scale = (uint32_t) (weight * 256.0f + 0.5f);

    for (uint32_t row = rect[1]; row < rect[1] + rect[3]; row++) {
        for (uint32_t col = rect[0]; col < rect[0] + rect[2]; col++) {
            uint8_t* p = pixel_at(image, col, row);

            for (uint8_t c = 0; c < colours; c++) {
                uint32_t target = colours < 3u ? color[1] : color[c];

                p[c] = (uint8_t) (((256u - scale) * p[c] + scale * target +
                                   128u) >>
                                  8);
            }
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_darken")
int tiny_image_darken(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float factor
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (factor < 0.0f || factor > 1.0f) return TINYIMG_ERR_RANGE;

    static const uint8_t BLACK[3] = {0, 0, 0};

    return mix_toward(image, x, y, width, height, BLACK, 1.0f - factor);
}

TINYIMG_EXPORT("tiny_image_lighten")
int tiny_image_lighten(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float factor
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (factor < 0.0f || factor > 1.0f) return TINYIMG_ERR_RANGE;

    static const uint8_t WHITE[3] = {255, 255, 255};

    // toward white rather than scaled up, so a channel already at full scale
    // stays there rather than clipping a whole region to one flat value
    return mix_toward(image, x, y, width, height, WHITE, 1.0f - factor);
}

TINYIMG_EXPORT("tiny_image_color_overlay")
int tiny_image_color_overlay(
    TinyImage* image, const uint8_t* color, float opacity
) {
    if (!image || !image->data || !color) return TINYIMG_ERR_NULL;
    if (opacity < 0.0f || opacity > 1.0f) return TINYIMG_ERR_RANGE;

    return mix_toward(image, 0, 0, 0, 0, color, opacity);
}

TINYIMG_EXPORT("tiny_image_color_overlay_rect")
int tiny_image_color_overlay_rect(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* color, float opacity
) {
    if (!image || !image->data || !color) return TINYIMG_ERR_NULL;
    if (opacity < 0.0f || opacity > 1.0f) return TINYIMG_ERR_RANGE;

    return mix_toward(image, x, y, width, height, color, opacity);
}

TINYIMG_EXPORT("tiny_image_vignette")
int tiny_image_vignette(
    TinyImage* image, float radius, float strength, const uint8_t* color
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (radius <= 0.0f || strength < 0.0f || strength > 1.0f) {
        return TINYIMG_ERR_RANGE;
    }

    uint8_t colours = colour_channels(image);
    float centre_x = (float) image->width * 0.5f;
    float centre_y = (float) image->height * 0.5f;
    float span = tiny_sqrtf(centre_x * centre_x + centre_y * centre_y);

    if (radius >= span) return TINYIMG_OK;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float dx = (float) x + 0.5f - centre_x;
            float dy = (float) y + 0.5f - centre_y;
            float distance = tiny_sqrtf(dx * dx + dy * dy);

            if (distance <= radius) continue;

            // squared past the radius, so the falloff starts gently where the
            // effect begins and steepens toward the corner, which is what a
            // lens does rather than a hard ring at the radius
            float t = (distance - radius) / (span - radius);
            float weight = strength * t * t;
            uint8_t* p = pixel_at(image, x, y);

            for (uint8_t c = 0; c < colours; c++) {
                float target = color ? (float) color[c] : 0.0f;

                p[c] = tiny_clamp_u8f(
                    (float) p[c] + weight * (target - (float) p[c])
                );
            }
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_sharpen")
int tiny_image_sharpen(TinyImage* image, float amount) {
    if (amount < 0.0f) return TINYIMG_ERR_RANGE;
    return tiny_image_unsharp_mask(image, 1.0f, amount, 0.0f);
}

#pragma endregion

#pragma region spatial

TINYIMG_EXPORT("tiny_image_unsharp_mask")
int tiny_image_unsharp_mask(
    TinyImage* image, float sigma, float amount, float threshold
) {
    if (sigma < 0.0f || amount < 0.0f || threshold < 0.0f) {
        return TINYIMG_ERR_RANGE;
    }

    float p[4] = {sigma, amount, threshold, 0.0f};
    return run_effect(image, TINYIMG_FX_UNSHARP, p);
}

TINYIMG_EXPORT("tiny_image_clarity")
int tiny_image_clarity(TinyImage* image, float amount) {
    if (!image) return TINYIMG_ERR_NULL;
    if (amount < 0.0f) return TINYIMG_ERR_RANGE;

    // a large radius, so what it raises is the contrast between regions rather
    // than the contrast at an edge, which is what separates it from a sharpen
    float sigma = (float) tiny_min_u32(image->width, image->height) / 40.0f;
    if (sigma < 2.0f) sigma = 2.0f;

    float p[4] = {sigma, amount, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_CLARITY, p);
}

TINYIMG_EXPORT("tiny_image_sobel")
int tiny_image_sobel(TinyImage* image) {
    float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_SOBEL, p);
}

TINYIMG_EXPORT("tiny_image_emboss")
int tiny_image_emboss(TinyImage* image, float strength) {
    float p[4] = {strength, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_EMBOSS, p);
}

TINYIMG_EXPORT("tiny_image_pixelate")
int tiny_image_pixelate(TinyImage* image, uint32_t size) {
    float p[4] = {(float) size, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_PIXELATE, p);
}

TINYIMG_EXPORT("tiny_image_pixelate_region")
int tiny_image_pixelate_region(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    uint32_t size
) {
    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    float p[4] = {(float) size, 0.0f, 0.0f, 0.0f};
    result = tiny_plan_effect_rect(
        &plan, TINYIMG_FX_PIXELATE_REGION, p, x, y, width, height
    );
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_despeckle")
int tiny_image_despeckle(TinyImage* image) {
    float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_MEDIAN, p);
}

TINYIMG_EXPORT("tiny_image_dilate")
int tiny_image_dilate(TinyImage* image, uint32_t radius) {
    float p[4] = {(float) radius, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_DILATE, p);
}

TINYIMG_EXPORT("tiny_image_erode")
int tiny_image_erode(TinyImage* image, uint32_t radius) {
    float p[4] = {(float) radius, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_ERODE, p);
}

TINYIMG_EXPORT("tiny_image_morphology_open")
int tiny_image_morphology_open(TinyImage* image, uint32_t radius) {
    int result = tiny_image_erode(image, radius);
    if (result != TINYIMG_OK) return result;

    return tiny_image_dilate(image, radius);
}

TINYIMG_EXPORT("tiny_image_morphology_close")
int tiny_image_morphology_close(TinyImage* image, uint32_t radius) {
    int result = tiny_image_dilate(image, radius);
    if (result != TINYIMG_OK) return result;

    return tiny_image_erode(image, radius);
}

TINYIMG_EXPORT("tiny_image_outline")
int tiny_image_outline(TinyImage* image, uint32_t radius) {
    float p[4] = {(float) radius, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_OUTLINE, p);
}

TINYIMG_EXPORT("tiny_image_motion_blur")
int tiny_image_motion_blur(TinyImage* image, float length, float angle) {
    if (length < 0.0f) return TINYIMG_ERR_RANGE;

    float p[4] = {length, angle, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_MOTION_BLUR, p);
}

TINYIMG_EXPORT("tiny_image_radial_blur")
int tiny_image_radial_blur(TinyImage* image, float degrees) {
    if (degrees < 0.0f) return TINYIMG_ERR_RANGE;

    float p[4] = {degrees, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_RADIAL_BLUR, p);
}

TINYIMG_EXPORT("tiny_image_zoom_blur")
int tiny_image_zoom_blur(TinyImage* image, float strength) {
    if (strength < 0.0f) return TINYIMG_ERR_RANGE;

    float p[4] = {strength, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_ZOOM_BLUR, p);
}

TINYIMG_EXPORT("tiny_image_tilt_shift")
int tiny_image_tilt_shift(TinyImage* image, float sigma, float band) {
    if (sigma < 0.0f || band < 0.0f || band > 1.0f) return TINYIMG_ERR_RANGE;

    float p[4] = {sigma, band, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_TILT_SHIFT, p);
}

TINYIMG_EXPORT("tiny_image_blur_region")
int tiny_image_blur_region(
    TinyImage* image, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    float sigma
) {
    if (sigma < 0.0f) return TINYIMG_ERR_RANGE;

    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    float p[4] = {sigma, 0.0f, 0.0f, 0.0f};
    result = tiny_plan_effect_rect(
        &plan, TINYIMG_FX_BLUR_REGION, p, x, y, width, height
    );
    if (result != TINYIMG_OK) return result;

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_chromatic_aberration")
int tiny_image_chromatic_aberration(TinyImage* image, float amount) {
    float p[4] = {amount < 0.0f ? -amount : amount, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_CHROMATIC, p);
}

TINYIMG_EXPORT("tiny_image_dither")
int tiny_image_dither(TinyImage* image, uint32_t levels) {
    if (levels < 2u || levels > 256u) return TINYIMG_ERR_RANGE;

    float p[4] = {(float) levels, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_DITHER, p);
}

TINYIMG_EXPORT("tiny_image_halftone")
int tiny_image_halftone(TinyImage* image, uint32_t cell) {
    if (cell < 2u) return TINYIMG_ERR_RANGE;

    float p[4] = {(float) cell, 0.0f, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_HALFTONE, p);
}

TINYIMG_EXPORT("tiny_image_scanlines")
int tiny_image_scanlines(TinyImage* image, uint32_t period, float strength) {
    if (period < 2u || strength < 0.0f || strength > 1.0f) {
        return TINYIMG_ERR_RANGE;
    }

    float p[4] = {(float) period, strength, 0.0f, 0.0f};
    return run_effect(image, TINYIMG_FX_SCANLINES, p);
}

TINYIMG_EXPORT("tiny_image_glow")
int tiny_image_glow(TinyImage* image, float sigma, float strength) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (sigma < 0.0f || strength < 0.0f) return TINYIMG_ERR_RANGE;

    TinyImage soft;
    int result = image_clone(image, &soft);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_gaussian_blur(&soft, sigma);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&soft);
        return result;
    }

    uint8_t colours = colour_channels(image);
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        const uint8_t* halo = soft.data + i * soft.channels;

        for (uint8_t c = 0; c < colours; c++) {
            // screened rather than added, so a bright area gains less than a
            // dark one and nothing clips to a flat white plateau
            float base = (float) p[c] / 255.0f;
            float add = strength * (float) halo[c] / 255.0f;

            p[c] = tiny_clamp_u8f(255.0f * (base + add - base * add));
        }
    }

    tiny_image_destroy(&soft);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_drop_shadow")
int tiny_image_drop_shadow(
    TinyImage* image, int32_t offset_x, int32_t offset_y, float sigma,
    const uint8_t* color
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (sigma < 0.0f) return TINYIMG_ERR_RANGE;

    if (image->channels == 1u || image->channels == 3u) {
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) return result;
    }

    // the extent has to hold the offset silhouette and the blur's reach, or
    // the shadow is clipped at the edge the offset pushes it past
    uint32_t reach = (uint32_t) (sigma * 3.0f + 1.0f);
    uint32_t left = reach + (offset_x < 0 ? (uint32_t) -offset_x : 0u);
    uint32_t top = reach + (offset_y < 0 ? (uint32_t) -offset_y : 0u);
    uint32_t right = reach + (offset_x > 0 ? (uint32_t) offset_x : 0u);
    uint32_t bottom = reach + (offset_y > 0 ? (uint32_t) offset_y : 0u);

    int result = tiny_image_expand(image, left, top, right, bottom, 0);
    if (result != TINYIMG_OK) return result;

    TinyImage shadow;
    result = image_clone(image, &shadow);
    if (result != TINYIMG_OK) return result;

    uint8_t at = (uint8_t) (shadow.channels - 1u);
    uint8_t tone[4] = {0, 0, 0, 0};

    if (color) {
        for (uint8_t c = 0; c < shadow.channels; c++) tone[c] = color[c];
    }
    else {
        tone[at] = 255u;
    }

    // the silhouette is the alpha channel with the colour replaced, moved by
    // the offset, then softened
    size_t pixels = (size_t) shadow.width * shadow.height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = shadow.data + i * shadow.channels;
        uint8_t alpha = p[at];

        for (uint8_t c = 0; c < at; c++) p[c] = tone[c];
        p[at] = alpha;
    }

    TinyImage moved;
    tiny_memset(&moved, 0, sizeof(moved));

    result =
        tiny_image_create(&moved, shadow.width, shadow.height, shadow.channels);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&shadow);
        return result;
    }

    result = tiny_image_draw_image(&moved, &shadow, offset_x, offset_y);
    tiny_image_destroy(&shadow);

    if (result == TINYIMG_OK) result = tiny_image_gaussian_blur(&moved, sigma);
    if (result == TINYIMG_OK) {
        result = tiny_image_draw_image(&moved, image, 0, 0);
    }

    if (result != TINYIMG_OK) {
        tiny_image_destroy(&moved);
        return result;
    }

    moved.meta = image->meta;
    image->meta = 0;
    moved.format = image->format;

    tiny_image_destroy(image);
    *image = moved;

    return TINYIMG_OK;
}

/**
 * @brief A pseudorandom value for one pixel and channel.
 *
 * A hash of the coordinates rather than a running generator, so the same
 * request over the same image gives the same noise. A generator carrying state
 * between calls would not, and a caller comparing two runs would see a
 * difference that is not in the request.
 *
 * @param x Column.
 * @param y Row.
 * @param channel Which channel.
 * @return float Roughly normal, mean zero, unit standard deviation.
 */
static float noise_at(uint32_t x, uint32_t y, uint32_t channel) {
    uint32_t h = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu ^ channel * 0xC2B2AE35u;

    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;

    // the sum of three uniforms, which is close enough to normal for grain and
    // costs three shifts rather than a log and a root
    float sum = 0.0f;
    for (uint32_t i = 0; i < 3u; i++) {
        sum += (float) ((h >> (i * 10u)) & 0x3FFu) / 1023.0f - 0.5f;
    }

    return sum * 2.0f;
}

TINYIMG_EXPORT("tiny_image_noise")
int tiny_image_noise(TinyImage* image, float amount, int monochrome) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (amount < 0.0f) return TINYIMG_ERR_RANGE;

    uint8_t colours = colour_channels(image);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* p = pixel_at(image, x, y);

            for (uint8_t c = 0; c < colours; c++) {
                float value = noise_at(x, y, monochrome ? 0u : c);
                p[c] = tiny_clamp_u8f((float) p[c] + amount * value);
            }
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_film_grain")
int tiny_image_film_grain(TinyImage* image, float amount) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (amount < 0.0f) return TINYIMG_ERR_RANGE;

    uint8_t colours = colour_channels(image);

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint8_t* p = pixel_at(image, x, y);
            float value = noise_at(x, y, 0u);

            for (uint8_t c = 0; c < colours; c++) {
                // weighted toward the midtones, which is where film's grain
                // actually is: a fully exposed or unexposed grain has no
                // variance left to show
                float t = (float) p[c] / 255.0f;
                float weight = 4.0f * t * (1.0f - t);

                p[c] = tiny_clamp_u8f((float) p[c] + amount * weight * value);
            }
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region auto correction

/**
 * @brief Where a histogram's mass starts and ends, ignoring a fraction at each
 * tail.
 *
 * The fraction matters: taking the outright minimum and maximum makes the
 * result turn on a handful of pixels, so one stuck sensor pixel or one
 * compression overshoot decides the whole stretch.
 *
 * @param bins 256 counts.
 * @param total How many pixels they cover.
 * @param clip What fraction to ignore at each end.
 * @param low Receives the first level kept.
 * @param high Receives the last level kept.
 */
static void histogram_range(
    const uint32_t* bins, uint32_t total, float clip, uint32_t* low,
    uint32_t* high
) {
    uint32_t budget = (uint32_t) ((float) total * clip);
    uint32_t seen = 0;

    *low = 0;
    *high = 255;

    for (uint32_t i = 0; i < 256u; i++) {
        seen += bins[i];
        if (seen > budget) {
            *low = i;
            break;
        }
    }

    seen = 0;

    for (uint32_t i = 256; i > 0; i--) {
        seen += bins[i - 1u];
        if (seen > budget) {
            *high = i - 1u;
            break;
        }
    }

    if (*high <= *low) {
        *low = 0;
        *high = 255;
    }
}

/** A histogram's mean level. */
static float histogram_mean(const uint32_t* bins, uint32_t total) {
    uint64_t sum = 0;

    for (uint32_t i = 0; i < 256u; i++) sum += (uint64_t) bins[i] * i;

    return total ? (float) sum / (float) total : 0.0f;
}

TINYIMG_EXPORT("tiny_image_auto_contrast")
int tiny_image_auto_contrast(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint32_t bins[256];
    int result = tiny_image_histogram(image, 255u, bins);
    if (result != TINYIMG_OK) return result;

    uint32_t total = image->width * image->height;
    uint32_t low;
    uint32_t high;

    histogram_range(bins, total, 0.005f, &low, &high);

    return tiny_image_levels(
        image, (float) low, (float) high, 1.0f, 0.0f, 255.0f
    );
}

TINYIMG_EXPORT("tiny_image_auto_levels")
int tiny_image_auto_levels(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint8_t colours = colour_channels(image);
    if (colours < 3u) return tiny_image_auto_contrast(image);

    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    uint32_t total = image->width * image->height;

    // each channel stretched on its own, which changes the colour balance as
    // well as the contrast; that is the difference from auto contrast, which
    // stretches the luminance and leaves the balance alone
    for (uint8_t c = 0; c < 3u; c++) {
        uint32_t bins[256];

        result = tiny_image_histogram(image, c, bins);
        if (result != TINYIMG_OK) return result;

        uint32_t low;
        uint32_t high;
        histogram_range(bins, total, 0.005f, &low, &high);

        float p[5] = {(float) low, (float) high, 1.0f, 0.0f, 255.0f};

        result = tiny_plan_curve(
            &plan, TINYIMG_CURVE_LEVELS, p, (uint8_t) (1u << c)
        );
        if (result != TINYIMG_OK) return result;
    }

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_auto_brightness")
int tiny_image_auto_brightness(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint32_t bins[256];
    int result = tiny_image_histogram(image, 255u, bins);
    if (result != TINYIMG_OK) return result;

    float mean = histogram_mean(bins, image->width * image->height);
    if (mean < 1.0f) return TINYIMG_OK;

    return tiny_image_brightness(image, 127.5f / mean);
}

TINYIMG_EXPORT("tiny_image_auto_gamma")
int tiny_image_auto_gamma(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint32_t bins[256];
    int result = tiny_image_histogram(image, 255u, bins);
    if (result != TINYIMG_OK) return result;

    float mean = histogram_mean(bins, image->width * image->height) / 255.0f;
    if (mean <= 0.0f || mean >= 1.0f) return TINYIMG_OK;

    // the exponent that maps the mean onto the middle of the range, which is
    // what makes this a gamma rather than a scale: it moves the midtones and
    // leaves both ends where they are
    float gamma = tiny_logf(0.5f) / tiny_logf(mean);

    return tiny_image_gamma_correction(image, gamma);
}

TINYIMG_EXPORT("tiny_image_auto_color")
int tiny_image_auto_color(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (colour_channels(image) < 3u) return TINYIMG_OK;

    uint32_t total = image->width * image->height;
    float mean[3];

    for (uint8_t c = 0; c < 3u; c++) {
        uint32_t bins[256];

        int result = tiny_image_histogram(image, c, bins);
        if (result != TINYIMG_OK) return result;

        mean[c] = histogram_mean(bins, total);
        if (mean[c] < 1.0f) return TINYIMG_OK;
    }

    // grey world: the scene's average is assumed neutral, so each channel is
    // scaled to the average of the three averages
    float target = (mean[0] + mean[1] + mean[2]) / 3.0f;

    return tiny_image_channel_gain(
        image, target / mean[0], target / mean[1], target / mean[2]
    );
}

TINYIMG_EXPORT("tiny_image_shadows_highlights")
int tiny_image_shadows_highlights(
    TinyImage* image, float shadows, float highlights
) {
    if (!image) return TINYIMG_ERR_NULL;
    if (shadows < 0.0f || shadows > 1.0f) return TINYIMG_ERR_RANGE;
    if (highlights < 0.0f || highlights > 1.0f) return TINYIMG_ERR_RANGE;

    TinyPlan plan;
    int result = tiny_plan_init_image(&plan, image);
    if (result != TINYIMG_OK) return result;

    if (shadows > 0.0f) {
        float p[5] = {shadows, 0.0f, 0.0f, 0.0f, 0.0f};

        result = tiny_plan_curve(&plan, TINYIMG_CURVE_FILL_LIGHT, p, 0);
        if (result != TINYIMG_OK) return result;
    }

    if (highlights > 0.0f) {
        // pulling the highlights down is a levels ceiling, which is the
        // mirror of the shadow lift and collapses into the same table
        float p[5] = {0.0f, 255.0f, 1.0f, 0.0f, 255.0f - 40.0f * highlights};

        result = tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, p, 0);
        if (result != TINYIMG_OK) return result;
    }

    return tiny_plan_replace(image, &plan);
}

TINYIMG_EXPORT("tiny_image_improve")
int tiny_image_improve(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    int result = tiny_image_auto_levels(image);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_auto_color(image);
    if (result != TINYIMG_OK) return result;

    return tiny_image_saturation(image, 1.1f);
}

TINYIMG_EXPORT("tiny_image_dehaze")
int tiny_image_dehaze(TinyImage* image, float strength) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (strength < 0.0f || strength > 1.0f) return TINYIMG_ERR_RANGE;
    if (colour_channels(image) < 3u) return TINYIMG_OK;

    // the dark channel prior: in a haze-free outdoor image most small patches
    // hold at least one channel near zero, so where the darkest channel over a
    // patch is bright the patch is veiled, and how bright says by how much
    size_t pixels = (size_t) image->width * image->height;
    uint32_t patch = tiny_min_u32(image->width, image->height) / 32u;
    if (patch < 3u) patch = 3u;

    TinyImage dark;
    int result = tiny_image_create(&dark, image->width, image->height, 1);
    if (result != TINYIMG_OK) return result;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            const uint8_t* p = pixel_at(image, x, y);
            uint8_t low = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2])
                                      : (p[1] < p[2] ? p[1] : p[2]);

            dark.data[(size_t) y * image->width + x] = low;
        }
    }

    result = morphology(&dark, patch / 2u, 0);
    if (result != TINYIMG_OK) {
        tiny_image_destroy(&dark);
        return result;
    }

    // the atmospheric light is the brightest the veiled pixels reach, taken
    // from the top tenth of a percent of the dark channel rather than from the
    // single brightest pixel, which a specular highlight would win
    uint32_t bins[256];
    for (uint32_t i = 0; i < 256u; i++) bins[i] = 0;
    for (size_t i = 0; i < pixels; i++) bins[dark.data[i]]++;

    uint32_t budget = (uint32_t) (pixels / 1000u) + 1u;
    uint32_t seen = 0;
    uint32_t floor_level = 255;

    for (uint32_t i = 256; i > 0; i--) {
        seen += bins[i - 1u];
        if (seen >= budget) {
            floor_level = i - 1u;
            break;
        }
    }

    float airlight = 0.0f;
    uint32_t counted = 0;

    for (size_t i = 0; i < pixels; i++) {
        if (dark.data[i] < floor_level) continue;

        const uint8_t* p = image->data + i * image->channels;
        airlight += luma_of(p);
        counted++;
    }

    airlight = counted ? airlight / (float) counted : 255.0f;
    if (airlight < 1.0f) airlight = 1.0f;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        float veil = (float) dark.data[i] / airlight;

        // the transmission, floored so a fully veiled pixel is recovered
        // rather than divided by nothing
        float transmission = 1.0f - strength * 0.95f * veil;
        if (transmission < 0.1f) transmission = 0.1f;

        for (uint8_t c = 0; c < 3u; c++) {
            float value = ((float) p[c] - airlight) / transmission + airlight;

            p[c] = tiny_clamp_u8f(value);
        }
    }

    tiny_image_destroy(&dark);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region warps

/**
 * @brief Runs an inverse-mapped warp.
 *
 * Every warp here is the same loop: for each output pixel, work out where it
 * came from and sample there. Mapping forward instead leaves holes wherever
 * the map expands, which is why the inverse is the only direction a resampler
 * is written in.
 *
 * @param image The image, replaced by the result.
 * @param out_width The result's width.
 * @param out_height The result's height.
 * @param map Fills in the source position for one output position.
 * @param context Passed to `map`.
 * @param background What an out of range source becomes, or NULL for
 * transparent.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
typedef void (*TinyWarpMap)(
    const void* context, float x, float y, float* out_x, float* out_y
);

static int warp_run(
    TinyImage* image, uint32_t out_width, uint32_t out_height, TinyWarpMap map,
    const void* context, const uint8_t* background
) {
    if (out_width == 0 || out_height == 0) return TINYIMG_ERR_RANGE;

    uint8_t channels = image->channels;
    int gained_alpha = 0;

    if (!background && channels != 2u && channels != 4u) {
        // an uncovered pixel has to be something, and transparent is the only
        // answer that is not a colour the caller did not choose
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) return result;

        channels = image->channels;
        gained_alpha = 1;
    }

    TinyImage out;
    tiny_memset(&out, 0, sizeof(out));

    int result = tiny_image_create(&out, out_width, out_height, channels);
    if (result != TINYIMG_OK) return result;

    uint8_t fill[4] = {0, 0, 0, 0};
    if (background) {
        for (uint8_t c = 0; c < channels; c++) fill[c] = background[c];
    }

    for (uint32_t y = 0; y < out_height; y++) {
        for (uint32_t x = 0; x < out_width; x++) {
            float sx;
            float sy;

            map(context, (float) x + 0.5f, (float) y + 0.5f, &sx, &sy);

            uint8_t* target = pixel_at(&out, x, y);

            if (sx < 0.0f || sy < 0.0f || sx >= (float) image->width ||
                sy >= (float) image->height) {
                for (uint8_t c = 0; c < channels; c++) target[c] = fill[c];
                continue;
            }

            sample_bilinear(image, sx - 0.5f, sy - 0.5f, target);
        }
    }

    out.format = image->format;
    out.quality = image->quality;
    out.meta = image->meta;
    image->meta = 0;

    tiny_image_destroy(image);
    *image = out;

    (void) gained_alpha;
    return TINYIMG_OK;
}

/** A 3x3 homography, applied in the inverse direction. */
typedef struct {
    float m[9];
} Homography;

static void map_homography(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Homography* h = context;
    float w = h->m[6] * x + h->m[7] * y + h->m[8];

    if (w > -1e-6f && w < 1e-6f) w = 1e-6f;

    *out_x = (h->m[0] * x + h->m[1] * y + h->m[2]) / w;
    *out_y = (h->m[3] * x + h->m[4] * y + h->m[5]) / w;
}

/** A 2x3 affine, applied in the inverse direction. */
typedef struct {
    float m[6];
} Affine;

static void map_affine(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Affine* a = context;

    *out_x = a->m[0] * x + a->m[1] * y + a->m[2];
    *out_y = a->m[3] * x + a->m[4] * y + a->m[5];
}

TINYIMG_EXPORT("tiny_image_rotate")
int tiny_image_rotate(
    TinyImage* image, float degrees, const uint8_t* background
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    float turns = degrees / 90.0f;
    float whole = (float) (int32_t) (turns + (turns < 0.0f ? -0.5f : 0.5f));

    // a quarter turn is a permutation of the pixels and loses nothing, so it
    // goes to the planner's exact kernel rather than through a resampler that
    // would blur an operation with no interpolation in it
    if (tiny_fabsf(turns - whole) < 1e-4f) {
        TinyPlan plan;
        int result = tiny_plan_init_image(&plan, image);
        if (result != TINYIMG_OK) return result;

        result = tiny_plan_rotate(&plan, (int32_t) (whole * 90.0f));
        if (result != TINYIMG_OK) return result;

        return tiny_plan_replace(image, &plan);
    }

    float radians = degrees * PI / 180.0f;
    float c = tiny_cosf(radians);
    float s = tiny_sinf(radians);
    float w = (float) image->width;
    float h = (float) image->height;

    uint32_t out_width =
        (uint32_t) (tiny_fabsf(w * c) + tiny_fabsf(h * s) + 0.5f);
    uint32_t out_height =
        (uint32_t) (tiny_fabsf(w * s) + tiny_fabsf(h * c) + 0.5f);

    Affine inverse;
    float cx = (float) out_width * 0.5f;
    float cy = (float) out_height * 0.5f;

    inverse.m[0] = c;
    inverse.m[1] = s;
    inverse.m[2] = w * 0.5f - c * cx - s * cy;
    inverse.m[3] = -s;
    inverse.m[4] = c;
    inverse.m[5] = h * 0.5f + s * cx - c * cy;

    return warp_run(
        image, out_width, out_height, map_affine, &inverse, background
    );
}

TINYIMG_EXPORT("tiny_image_shear")
int tiny_image_shear(
    TinyImage* image, float shear_x, float shear_y, const uint8_t* background
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    float determinant = 1.0f - shear_x * shear_y;
    if (determinant > -1e-4f && determinant < 1e-4f) return TINYIMG_ERR_RANGE;

    float w = (float) image->width;
    float h = (float) image->height;
    uint32_t out_width = (uint32_t) (w + tiny_fabsf(shear_x) * h + 0.5f);
    uint32_t out_height = (uint32_t) (h + tiny_fabsf(shear_y) * w + 0.5f);

    // the forward map is (x + sx y, sy x + y) plus a shift that keeps the
    // result inside the new extent; the inverse is its 2x2 inverse
    float shift_x = shear_x < 0.0f ? -shear_x * h : 0.0f;
    float shift_y = shear_y < 0.0f ? -shear_y * w : 0.0f;

    Affine inverse;
    inverse.m[0] = 1.0f / determinant;
    inverse.m[1] = -shear_x / determinant;
    inverse.m[3] = -shear_y / determinant;
    inverse.m[4] = 1.0f / determinant;
    inverse.m[2] = -(inverse.m[0] * shift_x + inverse.m[1] * shift_y);
    inverse.m[5] = -(inverse.m[3] * shift_x + inverse.m[4] * shift_y);

    return warp_run(
        image, out_width, out_height, map_affine, &inverse, background
    );
}

/**
 * @brief Solves for the homography taking the unit square's corners to a quad.
 *
 * The standard construction: three of the corners fix the affine part and the
 * fourth fixes the two projective terms, which is why four points are needed
 * and three would leave a family of answers.
 *
 * @param out Receives nine coefficients, row major.
 * @param quad Eight numbers, the four corners in order.
 * @return int TINYIMG_OK or TINYIMG_ERR_RANGE when the quad is degenerate.
 */
static int homography_from_unit(float* out, const float* quad) {
    float x0 = quad[0];
    float y0 = quad[1];
    float x1 = quad[2];
    float y1 = quad[3];
    float x2 = quad[4];
    float y2 = quad[5];
    float x3 = quad[6];
    float y3 = quad[7];

    float dx1 = x1 - x2;
    float dx2 = x3 - x2;
    float dy1 = y1 - y2;
    float dy2 = y3 - y2;
    float sx = x0 - x1 + x2 - x3;
    float sy = y0 - y1 + y2 - y3;

    float determinant = dx1 * dy2 - dx2 * dy1;
    if (determinant > -1e-9f && determinant < 1e-9f) {
        if (sx * sx + sy * sy > 1e-6f) return TINYIMG_ERR_RANGE;

        // an affine quad, where the two projective terms are zero
        out[0] = x1 - x0;
        out[1] = x2 - x1;
        out[2] = x0;
        out[3] = y1 - y0;
        out[4] = y2 - y1;
        out[5] = y0;
        out[6] = 0.0f;
        out[7] = 0.0f;
        out[8] = 1.0f;

        return TINYIMG_OK;
    }

    float g = (sx * dy2 - dx2 * sy) / determinant;
    float hh = (dx1 * sy - sx * dy1) / determinant;

    out[0] = x1 - x0 + g * x1;
    out[1] = x3 - x0 + hh * x3;
    out[2] = x0;
    out[3] = y1 - y0 + g * y1;
    out[4] = y3 - y0 + hh * y3;
    out[5] = y0;
    out[6] = g;
    out[7] = hh;
    out[8] = 1.0f;

    return TINYIMG_OK;
}

/** The inverse of a 3x3, by its adjugate. */
static int matrix3_invert(float* out, const float* m) {
    float a = m[4] * m[8] - m[5] * m[7];
    float b = m[5] * m[6] - m[3] * m[8];
    float c = m[3] * m[7] - m[4] * m[6];
    float determinant = m[0] * a + m[1] * b + m[2] * c;

    if (determinant > -1e-9f && determinant < 1e-9f) return TINYIMG_ERR_RANGE;

    out[0] = a / determinant;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / determinant;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / determinant;
    out[3] = b / determinant;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / determinant;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / determinant;
    out[6] = c / determinant;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / determinant;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / determinant;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_perspective")
int tiny_image_perspective(
    TinyImage* image, const float* quad, const uint8_t* background
) {
    if (!image || !image->data || !quad) return TINYIMG_ERR_NULL;

    float low_x = quad[0];
    float high_x = quad[0];
    float low_y = quad[1];
    float high_y = quad[1];

    for (uint32_t i = 1; i < 4u; i++) {
        if (quad[i * 2u] < low_x) low_x = quad[i * 2u];
        if (quad[i * 2u] > high_x) high_x = quad[i * 2u];
        if (quad[i * 2u + 1u] < low_y) low_y = quad[i * 2u + 1u];
        if (quad[i * 2u + 1u] > high_y) high_y = quad[i * 2u + 1u];
    }

    uint32_t out_width = (uint32_t) (high_x - low_x + 0.5f);
    uint32_t out_height = (uint32_t) (high_y - low_y + 0.5f);

    if (out_width == 0 || out_height == 0) return TINYIMG_ERR_RANGE;

    float shifted[8];
    for (uint32_t i = 0; i < 4u; i++) {
        shifted[i * 2u] = quad[i * 2u] - low_x;
        shifted[i * 2u + 1u] = quad[i * 2u + 1u] - low_y;
    }

    float forward[9];
    int result = homography_from_unit(forward, shifted);
    if (result != TINYIMG_OK) return result;

    Homography inverse;
    result = matrix3_invert(inverse.m, forward);
    if (result != TINYIMG_OK) return result;

    // the homography works in the unit square, so the inverse's output is
    // scaled back up to source pixels here rather than in the map
    for (uint32_t i = 0; i < 3u; i++) {
        inverse.m[i] *= (float) image->width;
        inverse.m[i + 3u] *= (float) image->height;
    }

    return warp_run(
        image, out_width, out_height, map_homography, &inverse, background
    );
}

/** Context the radial warps share. */
typedef struct {
    float centre_x;
    float centre_y;
    float span;
    float amount;
} Radial;

static void map_barrel(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Radial* r = context;
    float dx = (x - r->centre_x) / r->span;
    float dy = (y - r->centre_y) / r->span;
    float radius = dx * dx + dy * dy;
    float scale = 1.0f + r->amount * radius;

    *out_x = r->centre_x + dx * r->span * scale;
    *out_y = r->centre_y + dy * r->span * scale;
}

static void map_swirl(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Radial* r = context;
    float dx = x - r->centre_x;
    float dy = y - r->centre_y;
    float distance = tiny_sqrtf(dx * dx + dy * dy);
    float t = 1.0f - distance / r->span;

    if (t < 0.0f) t = 0.0f;

    // most at the centre and nothing at the rim, so the edge of the image is
    // where it was and the twist has no seam
    float angle = r->amount * t * t;
    float c = tiny_cosf(angle);
    float s = tiny_sinf(angle);

    *out_x = r->centre_x + dx * c - dy * s;
    *out_y = r->centre_y + dx * s + dy * c;
}

static void map_polar(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Radial* r = context;

    if (r->amount < 0.0f) {
        // a disc unrolled: the output's x is the angle and its y the radius
        float angle = x / (r->centre_x * 2.0f) * 2.0f * PI - PI;
        float radius = y / (r->centre_y * 2.0f) * r->span;

        *out_x = r->centre_x + radius * tiny_sinf(angle);
        *out_y = r->centre_y - radius * tiny_cosf(angle);
        return;
    }

    float dx = x - r->centre_x;
    float dy = y - r->centre_y;
    float distance = tiny_sqrtf(dx * dx + dy * dy);
    float angle = tiny_atan2f(dx, -dy);

    *out_x = (angle + PI) / (2.0f * PI) * r->centre_x * 2.0f;
    *out_y = distance / r->span * r->centre_y * 2.0f;
}

/** Fills in the centre and span every radial warp works from. */
static void radial_of(Radial* r, const TinyImage* image, float amount) {
    r->centre_x = (float) image->width * 0.5f;
    r->centre_y = (float) image->height * 0.5f;
    r->span = tiny_sqrtf(r->centre_x * r->centre_x + r->centre_y * r->centre_y);
    r->amount = amount;
}

TINYIMG_EXPORT("tiny_image_barrel")
int tiny_image_barrel(TinyImage* image, float amount) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (amount == 0.0f) return TINYIMG_OK;

    Radial r;
    radial_of(&r, image, amount);

    return warp_run(image, image->width, image->height, map_barrel, &r, 0);
}

TINYIMG_EXPORT("tiny_image_swirl")
int tiny_image_swirl(TinyImage* image, float degrees) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (degrees == 0.0f) return TINYIMG_OK;

    Radial r;
    radial_of(&r, image, degrees * PI / 180.0f);

    return warp_run(image, image->width, image->height, map_swirl, &r, 0);
}

TINYIMG_EXPORT("tiny_image_polar")
int tiny_image_polar(TinyImage* image, int inverse) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    Radial r;
    radial_of(&r, image, inverse ? -1.0f : 1.0f);

    return warp_run(image, image->width, image->height, map_polar, &r, 0);
}

/** Context the arc warp works from. */
typedef struct {
    float centre_x;
    float height;
    float amount;
} Arc;

static void map_arc(
    const void* context, float x, float y, float* out_x, float* out_y
) {
    const Arc* a = context;
    float t = (x - a->centre_x) / a->centre_x;

    // the row a pixel came from is lifted by a parabola in the horizontal
    // position, which is the arc to within the fraction of a pixel a caller
    // asking for a bend would notice
    *out_x = x;
    *out_y = y + a->amount * a->height * (1.0f - t * t) * 0.5f -
             a->amount * a->height * 0.5f;
}

TINYIMG_EXPORT("tiny_image_arc")
int tiny_image_arc(TinyImage* image, float degrees, const uint8_t* background) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (degrees < -180.0f || degrees > 180.0f) return TINYIMG_ERR_RANGE;
    if (degrees == 0.0f) return TINYIMG_OK;

    Arc a;
    a.centre_x = (float) image->width * 0.5f;
    a.height = (float) image->height;
    a.amount = degrees / 180.0f;

    uint32_t grown =
        image->height + (uint32_t) (tiny_fabsf(a.amount) * a.height * 0.5f);

    return warp_run(image, image->width, grown, map_arc, &a, background);
}

TINYIMG_EXPORT("tiny_image_corner_radius")
int tiny_image_corner_radius(TinyImage* image, uint32_t radius) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (radius == 0) return TINYIMG_OK;

    TinyImage mask;
    tiny_memset(&mask, 0, sizeof(mask));

    int result = tiny_image_create(&mask, image->width, image->height, 1);
    if (result != TINYIMG_OK) return result;

    static const uint8_t ON[1] = {255};

    result = tiny_image_fill_rounded_rectangle(
        &mask, 0, 0, image->width, image->height, radius, ON
    );

    if (result == TINYIMG_OK &&
        (image->channels == 1u || image->channels == 3u)) {
        result = tiny_image_to_rgba(image);
    }

    if (result != TINYIMG_OK) {
        tiny_image_destroy(&mask);
        return result;
    }

    uint8_t at = (uint8_t) (image->channels - 1u);
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        if (mask.data[i]) continue;

        uint8_t* p = image->data + i * image->channels;
        for (uint8_t c = 0; c < image->channels; c++) p[c] = 0u;
        p[at] = 0u;
    }

    tiny_image_destroy(&mask);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region dispatch

int tiny_effect_apply(TinyImage* image, const TinyPlanOp* op) {
    if (!image || !image->data || !op) return TINYIMG_ERR_NULL;

    const float* p = op->effect.p;
    uint32_t rect[4];

    if (!clip_rect(
            image, op->effect.x, op->effect.y, op->effect.width,
            op->effect.height, rect
        )) {
        // a rectangle outside the image is a request with nothing in it,
        // which is not an error any more than an empty crop would be
        return TINYIMG_OK;
    }

    static const int32_t SOBEL_X[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
    static const int32_t SOBEL_Y[9] = {-1, -2, -1, 0, 0, 0, 1, 2, 1};
    // zero sum, so a flat area comes out as the offset alone and `strength`
    // means what its documentation says. the commonly quoted emboss kernel has
    // a 1 in the centre and sums to one, which leaves a flat area at its own
    // value plus the offset and so clips everything above mid grey
    static const int32_t EMBOSS[9] = {-2, -1, 0, -1, 0, 1, 0, 1, 2};

    switch (op->effect.kind) {
        case TINYIMG_FX_UNSHARP:
        case TINYIMG_FX_CLARITY: return unsharp(image, p[0], p[1], p[2]);
        case TINYIMG_FX_SOBEL:
            return convolve3(image, SOBEL_X, 1, 0, 1, SOBEL_Y);
        case TINYIMG_FX_EMBOSS:
            return convolve3(image, EMBOSS, 1, (int32_t) p[0], 0, 0);
        case TINYIMG_FX_PIXELATE:
        case TINYIMG_FX_PIXELATE_REGION:
            return pixelate(image, (uint32_t) (p[0] + 0.5f), rect);
        case TINYIMG_FX_MEDIAN: return median3(image);
        case TINYIMG_FX_DILATE:
            return morphology(image, (uint32_t) (p[0] + 0.5f), 1);
        case TINYIMG_FX_ERODE:
            return morphology(image, (uint32_t) (p[0] + 0.5f), 0);
        case TINYIMG_FX_OUTLINE:
            return outline(image, (uint32_t) (p[0] + 0.5f));
        case TINYIMG_FX_MOTION_BLUR:
        case TINYIMG_FX_RADIAL_BLUR:
        case TINYIMG_FX_ZOOM_BLUR:
            return directional(image, op->effect.kind, p[0], p[1]);
        case TINYIMG_FX_TILT_SHIFT: return tilt_shift(image, p[0], p[1]);
        case TINYIMG_FX_BLUR_REGION: return blur_region(image, p[0], rect);
        case TINYIMG_FX_CHROMATIC: return chromatic(image, p[0]);
        case TINYIMG_FX_DITHER: return dither(image, (uint32_t) (p[0] + 0.5f));
        case TINYIMG_FX_HALFTONE:
            return halftone(image, (uint32_t) (p[0] + 0.5f));
        case TINYIMG_FX_SCANLINES:
            return scanlines(image, (uint32_t) (p[0] + 0.5f), p[1]);
        default: return TINYIMG_ERR_RANGE;
    }
}

#pragma endregion
