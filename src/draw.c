#include "tinyimg/image.h"
#include "tinyimg/memory.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#pragma region blending

#define PI 3.14159265358979f

/** Which channel carries alpha, or the channel count when none does. */
static uint8_t alpha_index(const TinyImage* image) {
    return image->channels == 2u || image->channels == 4u
               ? (uint8_t) (image->channels - 1u)
               : image->channels;
}

static uint8_t* pixel_at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** A product scaled back down, rounded rather than truncated. */
static int32_t over255(int32_t value) {
    return (value + 127) / 255;
}

/**
 * @brief One channel through a separable blend mode.
 *
 * Both arguments and the result are unpremultiplied, which is where these
 * functions are defined. Premultiplying first and blending after gives the
 * right answer only where alpha is full, so it looks correct on exactly the
 * case a test written with opaque colours can reach.
 *
 * @param mode Which mode.
 * @param dst The destination channel.
 * @param src The source channel.
 * @return int32_t The blended channel, in range.
 */
static int32_t blend_channel(TinyBlendMode mode, int32_t dst, int32_t src) {
    switch (mode) {
        case TINYIMG_BLEND_MULTIPLY: return over255(dst * src);
        case TINYIMG_BLEND_SCREEN:
            return 255 - over255((255 - dst) * (255 - src));
        case TINYIMG_BLEND_OVERLAY:
            return dst <= 127 ? over255(2 * dst * src)
                              : 255 - over255(2 * (255 - dst) * (255 - src));
        case TINYIMG_BLEND_HARD_LIGHT:
            return src <= 127 ? over255(2 * dst * src)
                              : 255 - over255(2 * (255 - dst) * (255 - src));
        case TINYIMG_BLEND_SOFT_LIGHT: {
            float d = (float) dst / 255.0f;
            float s = (float) src / 255.0f;
            float out;

            if (s <= 0.5f) {
                out = d - (1.0f - 2.0f * s) * d * (1.0f - d);
            }
            else {
                // the specification's three-part D(d); the root is what keeps
                // the curve continuous where the pieces meet at a quarter
                float g = d <= 0.25f ? ((16.0f * d - 12.0f) * d + 4.0f) * d
                                     : tiny_sqrtf(d);
                out = d + (2.0f * s - 1.0f) * (g - d);
            }

            return (int32_t) (out * 255.0f + 0.5f);
        }
        case TINYIMG_BLEND_DARKEN: return dst < src ? dst : src;
        case TINYIMG_BLEND_LIGHTEN: return dst > src ? dst : src;
        case TINYIMG_BLEND_DIFFERENCE: return dst > src ? dst - src : src - dst;
        case TINYIMG_BLEND_EXCLUSION: return dst + src - over255(2 * dst * src);
        case TINYIMG_BLEND_ADD: return dst + src;
        case TINYIMG_BLEND_SUBTRACT: return dst - src;
        default: return src;
    }
}

/**
 * @brief Composites one source pixel over one destination pixel, in place.
 *
 * Source-over, with the blend mode applied to the colour before the alpha
 * weighting. Alpha is written when the destination has one, so a stack of
 * partly transparent layers ends up as transparent as it should be rather than
 * opaque wherever anything was drawn.
 *
 * @param dst The destination pixel.
 * @param channels How many channels it has.
 * @param src The source colour, `channels` long, unpremultiplied.
 * @param alpha The source's alpha, 0 through 255, already scaled by any
 * opacity the caller asked for.
 * @param mode Which blend mode.
 */
static void blend_pixel(
    uint8_t* dst, uint8_t channels, const uint8_t* src, int32_t alpha,
    TinyBlendMode mode
) {
    if (alpha <= 0) return;

    uint8_t colours = channels == 4u ? 3u : channels == 2u ? 1u : channels;
    int has_alpha = channels == 2u || channels == 4u;

    if (mode == TINYIMG_BLEND_REPLACE) {
        for (uint8_t c = 0; c < channels; c++) dst[c] = src[c];
        if (has_alpha) dst[channels - 1u] = (uint8_t) alpha;
        return;
    }

    int32_t backdrop = has_alpha ? dst[channels - 1u] : 255;
    // never zero, since the early return above has already taken the only
    // case that could make it so
    int32_t out_alpha = over255(alpha * 255 + backdrop * (255 - alpha));

    for (uint8_t c = 0; c < colours; c++) {
        int32_t mixed =
            tiny_clampi(blend_channel(mode, dst[c], src[c]), 0, 255);

        // the specification's Co, in premultiplied form: the source alone
        // where the backdrop is clear, the blend where both are present, and
        // the backdrop alone where the source is clear. dividing by the
        // composited alpha is what brings it back to the unpremultiplied
        // channels an image stores, and doing the whole thing premultiplied
        // instead is only right where the backdrop is already opaque
        int32_t premultiplied = alpha * (255 - backdrop) * src[c] +
                                alpha * backdrop * mixed +
                                (255 - alpha) * backdrop * dst[c];

        // one rounded division rather than two, since dividing by 255 first
        // throws away the bits the divide by the alpha then needs
        int32_t divisor = 255 * out_alpha;

        dst[c] = tiny_clamp_u8((premultiplied + divisor / 2) / divisor);
    }

    if (has_alpha) dst[channels - 1u] = (uint8_t) out_alpha;
}

/** The alpha a source pixel contributes, scaled by an opacity in 0..255. */
static int32_t source_alpha(
    const uint8_t* src, uint8_t channels, int32_t opacity
) {
    int32_t alpha = channels == 2u || channels == 4u ? src[channels - 1u] : 255;

    return alpha * opacity / 255;
}

/**
 * @brief Widens or narrows a colour to an image's channel count.
 *
 * @param out Receives up to four channels.
 * @param color The caller's colour, as many channels as the image has.
 * @param channels How many the image has.
 */
static void colour_for(uint8_t* out, const uint8_t* color, uint8_t channels) {
    for (uint8_t c = 0; c < channels; c++) out[c] = color[c];
}

#pragma endregion

#pragma region spans

/**
 * @brief Blends one horizontal run, clipped.
 *
 * The one place a filled shape writes pixels. Every primitive here reduces to
 * a set of spans, so clipping, blending and the alpha rule are written once.
 *
 * @param image The image.
 * @param x0 Left end, inclusive; may be negative.
 * @param x1 Right end, inclusive; may be past the edge.
 * @param y The row; a row outside the image draws nothing.
 * @param color The colour, as many channels as the image has.
 * @param blend Which mode.
 */
static void span(
    TinyImage* image, int32_t x0, int32_t x1, int32_t y, const uint8_t* color,
    TinyBlendMode blend
) {
    if (y < 0 || y >= (int32_t) image->height) return;
    if (x1 < x0) return;

    x0 = tiny_clampi(x0, 0, (int32_t) image->width - 1);
    x1 = tiny_clampi(x1, 0, (int32_t) image->width - 1);
    if (x1 < x0) return;

    int32_t alpha = source_alpha(color, image->channels, 255);

    // an opaque run in the default mode is what the pixels are anyway, so it
    // is a fill rather than a per-pixel blend
    if (alpha >= 255 &&
        (blend == TINYIMG_BLEND_NORMAL || blend == TINYIMG_BLEND_REPLACE)) {
        uint8_t* out = pixel_at(image, (uint32_t) x0, (uint32_t) y);
        uint32_t count = (uint32_t) (x1 - x0 + 1);

        if (image->channels == 1u) {
            tiny_memset(out, color[0], count);
            return;
        }

        for (uint32_t i = 0; i < count; i++) {
            for (uint8_t c = 0; c < image->channels; c++) {
                out[i * image->channels + c] = color[c];
            }
        }
        return;
    }

    for (int32_t x = x0; x <= x1; x++) {
        blend_pixel(
            pixel_at(image, (uint32_t) x, (uint32_t) y), image->channels, color,
            alpha, blend
        );
    }
}

/** One pixel, clipped and blended. */
static void plot(
    TinyImage* image, int32_t x, int32_t y, const uint8_t* color,
    TinyBlendMode blend
) {
    span(image, x, x, y, color, blend);
}

#pragma endregion

#pragma region primitives

TINYIMG_EXPORT("tiny_image_hline")
int tiny_image_hline(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    const uint8_t* pixel
) {
    (void) y2;
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;

    span(
        image, x1 < x2 ? x1 : x2, x1 < x2 ? x2 : x1, y1, pixel,
        TINYIMG_BLEND_NORMAL
    );

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_vline")
int tiny_image_vline(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    const uint8_t* pixel
) {
    (void) x2;
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;

    int32_t from = y1 < y2 ? y1 : y2;
    int32_t to = y1 < y2 ? y2 : y1;

    for (int32_t y = from; y <= to; y++) {
        span(image, x1, x1, y, pixel, TINYIMG_BLEND_NORMAL);
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_fill_rectangle")
int tiny_image_fill_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (width == 0 || height == 0) return TINYIMG_OK;

    for (uint32_t row = 0; row < height; row++) {
        span(
            image, x, x + (int32_t) width - 1, y + (int32_t) row, pixel,
            TINYIMG_BLEND_NORMAL
        );
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_rectangle")
int tiny_image_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (width == 0 || height == 0) return TINYIMG_OK;

    int32_t right = x + (int32_t) width - 1;
    int32_t bottom = y + (int32_t) height - 1;

    span(image, x, right, y, pixel, TINYIMG_BLEND_NORMAL);
    if (height > 1u) span(image, x, right, bottom, pixel, TINYIMG_BLEND_NORMAL);

    for (int32_t row = y + 1; row < bottom; row++) {
        span(image, x, x, row, pixel, TINYIMG_BLEND_NORMAL);
        if (width > 1u)
            span(image, right, right, row, pixel, TINYIMG_BLEND_NORMAL);
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_fill_rounded_rectangle")
int tiny_image_fill_rounded_rectangle(
    TinyImage* image, int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t radius, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (width == 0 || height == 0) return TINYIMG_OK;

    uint32_t limit = tiny_min_u32(width, height) / 2u;
    if (radius > limit) radius = limit;

    if (radius == 0) {
        return tiny_image_fill_rectangle(image, x, y, width, height, pixel);
    }

    float r = (float) radius;

    for (uint32_t row = 0; row < height; row++) {
        int32_t inset = 0;
        float dy = 0.0f;

        if (row < radius)
            dy = r - ((float) row + 0.5f);
        else if (row >= height - radius) {
            dy = ((float) row + 0.5f) - ((float) height - r);
        }

        if (dy > 0.0f) {
            float dx = r * r - dy * dy;
            dx = dx > 0.0f ? tiny_sqrtf(dx) : 0.0f;
            inset = (int32_t) (r - dx + 0.5f);
        }

        span(
            image, x + inset, x + (int32_t) width - 1 - inset,
            y + (int32_t) row, pixel, TINYIMG_BLEND_NORMAL
        );
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_draw_line")
int tiny_image_draw_line(
    TinyImage* image, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    uint32_t thickness, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;

    if (thickness <= 1u) {
        int32_t dx = x2 > x1 ? x2 - x1 : x1 - x2;
        int32_t dy = y2 > y1 ? y2 - y1 : y1 - y2;
        int32_t sx = x1 < x2 ? 1 : -1;
        int32_t sy = y1 < y2 ? 1 : -1;
        int32_t error = dx - dy;

        for (;;) {
            plot(image, x1, y1, pixel, TINYIMG_BLEND_NORMAL);
            if (x1 == x2 && y1 == y2) break;

            int32_t twice = error * 2;
            if (twice > -dy) {
                error -= dy;
                x1 += sx;
            }
            if (twice < dx) {
                error += dx;
                y1 += sy;
            }
        }

        return TINYIMG_OK;
    }

    // a thick line is the set of pixels within half its width of the segment,
    // which keeps its ends square and leaves no gaps on a steep slope the way
    // a disc stamped at every Bresenham step does
    float half = (float) thickness * 0.5f;
    float ax = (float) x1;
    float ay = (float) y1;
    float bx = (float) x2;
    float by = (float) y2;
    float ex = bx - ax;
    float ey = by - ay;
    float length = ex * ex + ey * ey;

    int32_t low_x = (x1 < x2 ? x1 : x2) - (int32_t) half - 1;
    int32_t high_x = (x1 > x2 ? x1 : x2) + (int32_t) half + 1;
    int32_t low_y = (y1 < y2 ? y1 : y2) - (int32_t) half - 1;
    int32_t high_y = (y1 > y2 ? y1 : y2) + (int32_t) half + 1;

    for (int32_t y = low_y; y <= high_y; y++) {
        for (int32_t x = low_x; x <= high_x; x++) {
            float px = (float) x - ax;
            float py = (float) y - ay;
            float t = length > 0.0f ? (px * ex + py * ey) / length : 0.0f;

            t = tiny_clampf(t, 0.0f, 1.0f);

            float qx = px - t * ex;
            float qy = py - t * ey;

            if (qx * qx + qy * qy <= half * half) {
                plot(image, x, y, pixel, TINYIMG_BLEND_NORMAL);
            }
        }
    }

    return TINYIMG_OK;
}

/**
 * @brief How far an ellipse reaches either side of its centre on one row.
 *
 * @param rx Horizontal semi-axis.
 * @param ry Vertical semi-axis.
 * @param dy Rows from the centre.
 * @return int32_t The half width, or -1 when the row is outside.
 */
static int32_t ellipse_half_width(uint32_t rx, uint32_t ry, int32_t dy) {
    if (ry == 0) return -1;

    float ny = (float) dy / (float) ry;
    float inside = 1.0f - ny * ny;

    if (inside < 0.0f) return -1;

    return (int32_t) ((float) rx * tiny_sqrtf(inside));
}

/**
 * @brief Fills an ellipse as one span per row.
 *
 * @param image The image.
 * @param cx Centre.
 * @param cy Centre.
 * @param rx Horizontal semi-axis.
 * @param ry Vertical semi-axis.
 * @param color The colour.
 * @param blend Which mode.
 */
static void ellipse_fill(
    TinyImage* image, int32_t cx, int32_t cy, uint32_t rx, uint32_t ry,
    const uint8_t* color, TinyBlendMode blend
) {
    if (rx == 0 || ry == 0) return;

    for (int32_t dy = -(int32_t) ry; dy <= (int32_t) ry; dy++) {
        int32_t dx = ellipse_half_width(rx, ry, dy);
        if (dx < 0) continue;

        span(image, cx - dx, cx + dx, cy + dy, color, blend);
    }
}

TINYIMG_EXPORT("tiny_image_fill_ellipse")
int tiny_image_fill_ellipse(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius_x,
    uint32_t radius_y, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;

    ellipse_fill(
        image, center_x, center_y, radius_x, radius_y, pixel,
        TINYIMG_BLEND_NORMAL
    );

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_fill_circle")
int tiny_image_fill_circle(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* pixel
) {
    return tiny_image_fill_ellipse(
        image, center_x, center_y, radius, radius, pixel
    );
}

TINYIMG_EXPORT("tiny_image_draw_ellipse")
int tiny_image_draw_ellipse(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius_x,
    uint32_t radius_y, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (radius_x == 0 || radius_y == 0) return TINYIMG_OK;

    // the outline is the inner boundary of the filled set: a pixel inside the
    // ellipse with a 4-neighbour outside it. that set is 8-connected, which is
    // what makes a drawn outline closed against a 4-connected fill, and it
    // stays closed for any pair of radii; a midpoint walk per octant does not,
    // once the axes differ enough for one step to skip a row
    for (int32_t dy = -(int32_t) radius_y; dy <= (int32_t) radius_y; dy++) {
        int32_t dx = ellipse_half_width(radius_x, radius_y, dy);
        if (dx < 0) continue;

        int32_t above = ellipse_half_width(radius_x, radius_y, dy - 1);
        int32_t below = ellipse_half_width(radius_x, radius_y, dy + 1);
        int32_t inner = above < below ? above : below;

        // a row whose neighbours are no narrower still has its two end pixels
        // on the boundary, because their outward neighbour is outside
        if (inner >= dx) inner = dx - 1;

        span(
            image, center_x - dx, center_x - inner - 1, center_y + dy, pixel,
            TINYIMG_BLEND_NORMAL
        );
        span(
            image, center_x + inner + 1, center_x + dx, center_y + dy, pixel,
            TINYIMG_BLEND_NORMAL
        );
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_draw_circle")
int tiny_image_draw_circle(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* pixel
) {
    return tiny_image_draw_ellipse(
        image, center_x, center_y, radius, radius, pixel
    );
}

#pragma endregion

#pragma region polygons

TINYIMG_EXPORT("tiny_image_polygon")
int tiny_image_polygon(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel
) {
    if (!image || !image->data || !x_points || !y_points || !pixel) {
        return TINYIMG_ERR_NULL;
    }
    if (num_points < 2u) return TINYIMG_OK;

    for (size_t i = 0; i < num_points; i++) {
        size_t next = (i + 1u) % num_points;
        int result = tiny_image_draw_line(
            image, x_points[i], y_points[i], x_points[next], y_points[next], 1u,
            pixel
        );

        if (result != TINYIMG_OK) return result;
    }

    return TINYIMG_OK;
}

/**
 * @brief Fills a polygon by scanline.
 *
 * Crossings are gathered per row and sorted; the even-odd rule takes them in
 * pairs, the nonzero rule sums the direction each edge was crossed in and
 * fills where the sum is not zero. The two differ only where a path crosses
 * itself.
 *
 * The coordinates are continuous, not pixel indices: a row is filled when its
 * centre falls inside the path. The two callers differ on that and each has to
 * convert. `tiny_image_fill_polygon` takes vertices that NAME pixels, so it
 * adds a half to move them to the pixel's centre; a display list's geometry is
 * already continuous and adds nothing. Adding the half in both places makes
 * every display shape one row and one column too large, which is invisible
 * until a rotated rectangle is compared against the upright one.
 *
 * @param image The image.
 * @param xs Vertex x coordinates, continuous.
 * @param ys Vertex y coordinates, continuous.
 * @param count How many vertices.
 * @param color The colour.
 * @param rule Which rule.
 * @param blend Which mode.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int polygon_fill(
    TinyImage* image, const float* xs, const float* ys, size_t count,
    const uint8_t* color, TinyFillRule rule, TinyBlendMode blend
) {
    if (count < 3u) return TINYIMG_OK;

    float low = ys[0];
    float high = ys[0];

    for (size_t i = 1; i < count; i++) {
        if (ys[i] < low) low = ys[i];
        if (ys[i] > high) high = ys[i];
    }

    int32_t first_row =
        tiny_clampi((int32_t) tiny_floorf(low), 0, (int32_t) image->height - 1);
    int32_t last_row =
        tiny_clampi((int32_t) tiny_ceilf(high), 0, (int32_t) image->height - 1);

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    float* crossing = tiny_arena_alloc(count * sizeof(float), 4);
    int32_t* winding = tiny_arena_alloc(count * sizeof(int32_t), 4);

    if (!crossing || !winding) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (int32_t row = first_row; row <= last_row; row++) {
        float centre = (float) row + 0.5f;
        size_t found = 0;

        for (size_t i = 0; i < count; i++) {
            size_t next = (i + 1u) % count;
            float y0 = ys[i];
            float y1 = ys[next];

            // a half-open test on each edge, so a vertex exactly on the
            // scanline is counted by one of its two edges and not both
            if ((y0 <= centre && y1 > centre) ||
                (y1 <= centre && y0 > centre)) {
                float t = (centre - y0) / (y1 - y0);

                crossing[found] = xs[i] + t * (xs[next] - xs[i]);
                winding[found] = y1 > y0 ? 1 : -1;
                found++;
            }
        }

        for (size_t i = 1; i < found; i++) {
            float key = crossing[i];
            int32_t direction = winding[i];
            size_t j = i;

            while (j > 0 && crossing[j - 1u] > key) {
                crossing[j] = crossing[j - 1u];
                winding[j] = winding[j - 1u];
                j--;
            }

            crossing[j] = key;
            winding[j] = direction;
        }

        if (rule == TINYIMG_FILL_EVEN_ODD) {
            for (size_t i = 0; i + 1u < found; i += 2u) {
                span(
                    image, (int32_t) tiny_ceilf(crossing[i] - 0.5f),
                    (int32_t) tiny_floorf(crossing[i + 1u] - 0.5f), row, color,
                    blend
                );
            }
        }
        else {
            int32_t sum = 0;

            for (size_t i = 0; i + 1u < found; i++) {
                sum += winding[i];
                if (sum == 0) continue;

                span(
                    image, (int32_t) tiny_ceilf(crossing[i] - 0.5f),
                    (int32_t) tiny_floorf(crossing[i + 1u] - 0.5f), row, color,
                    blend
                );
            }
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_fill_polygon_with")
int tiny_image_fill_polygon_with(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel, TinyFillRule rule,
    TinyBlendMode blend
) {
    if (!image || !image->data || !x_points || !y_points || !pixel) {
        return TINYIMG_ERR_NULL;
    }
    if (num_points < 3u) return TINYIMG_OK;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    float* xs = tiny_arena_alloc(num_points * sizeof(float), 4);
    float* ys = tiny_arena_alloc(num_points * sizeof(float), 4);

    if (!xs || !ys) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    // a vertex names a pixel, and a pixel's centre is at its index plus a
    // half, which is where the scanline test is taken
    for (size_t i = 0; i < num_points; i++) {
        xs[i] = (float) x_points[i] + 0.5f;
        ys[i] = (float) y_points[i] + 0.5f;
    }

    int result = polygon_fill(image, xs, ys, num_points, pixel, rule, blend);

    tiny_arena_release(&mark);
    return result;
}

TINYIMG_EXPORT("tiny_image_fill_polygon")
int tiny_image_fill_polygon(
    TinyImage* image, const int32_t* x_points, const int32_t* y_points,
    size_t num_points, const uint8_t* pixel
) {
    return tiny_image_fill_polygon_with(
        image, x_points, y_points, num_points, pixel, TINYIMG_FILL_EVEN_ODD,
        TINYIMG_BLEND_NORMAL
    );
}

#pragma endregion

#pragma region images

TINYIMG_EXPORT("tiny_image_replace_color")
int tiny_image_replace_color(
    TinyImage* image, const uint8_t* old_color, const uint8_t* new_color,
    const uint8_t* tolerance
) {
    if (!image || !image->data || !old_color || !new_color) {
        return TINYIMG_ERR_NULL;
    }

    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        int matches = 1;

        for (uint8_t c = 0; c < image->channels && matches; c++) {
            int32_t diff = (int32_t) p[c] - (int32_t) old_color[c];
            int32_t limit = tolerance ? tolerance[c] : 0;

            if (diff < -limit || diff > limit) matches = 0;
        }

        if (!matches) continue;

        for (uint8_t c = 0; c < image->channels; c++) p[c] = new_color[c];
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_draw_image_ex")
int tiny_image_draw_image_ex(
    TinyImage* dest_image, const TinyImage* src_image, int32_t x, int32_t y,
    float opacity, TinyDrawMode mode, TinyBlendMode blend
) {
    if (!dest_image || !dest_image->data || !src_image || !src_image->data) {
        return TINYIMG_ERR_NULL;
    }
    if (opacity < 0.0f || opacity > 1.0f) return TINYIMG_ERR_RANGE;

    int32_t alpha_scale = (int32_t) (opacity * 255.0f + 0.5f);
    if (alpha_scale == 0) return TINYIMG_OK;

    if (mode == TINYIMG_DRAW_CENTER) {
        x = ((int32_t) dest_image->width - (int32_t) src_image->width) / 2;
        y = ((int32_t) dest_image->height - (int32_t) src_image->height) / 2;
    }

    int32_t first_x = x;
    int32_t first_y = y;
    int32_t step_x = (int32_t) src_image->width;
    int32_t step_y = (int32_t) src_image->height;

    if (mode == TINYIMG_DRAW_TILE) {
        // start at the last tile origin at or before the left edge, so a
        // positive offset tiles leftward as well as rightward
        while (first_x > 0) first_x -= step_x;
        while (first_y > 0) first_y -= step_y;
    }

    uint8_t colour[4];

    for (int32_t ty = first_y; ty < (int32_t) dest_image->height;
         ty += step_y) {
        for (int32_t tx = first_x; tx < (int32_t) dest_image->width;
             tx += step_x) {
            int32_t x0 = tiny_clampi(tx, 0, (int32_t) dest_image->width);
            int32_t y0 = tiny_clampi(ty, 0, (int32_t) dest_image->height);
            int32_t x1 =
                tiny_clampi(tx + step_x, 0, (int32_t) dest_image->width);
            int32_t y1 =
                tiny_clampi(ty + step_y, 0, (int32_t) dest_image->height);

            for (int32_t row = y0; row < y1; row++) {
                for (int32_t col = x0; col < x1; col++) {
                    const uint8_t* src = pixel_at(
                        src_image, (uint32_t) (col - tx), (uint32_t) (row - ty)
                    );
                    int32_t alpha =
                        source_alpha(src, src_image->channels, alpha_scale);

                    if (alpha <= 0) continue;

                    // the source's channel count need not match, so its
                    // colour is widened or reduced to the destination's here
                    // rather than in the blend
                    for (uint8_t c = 0; c < dest_image->channels; c++) {
                        colour[c] = c < src_image->channels
                                        ? src[c]
                                        : src[src_image->channels - 1u];
                    }

                    blend_pixel(
                        pixel_at(dest_image, (uint32_t) col, (uint32_t) row),
                        dest_image->channels, colour, alpha, blend
                    );
                }
            }

            if (mode != TINYIMG_DRAW_TILE) break;
        }

        if (mode != TINYIMG_DRAW_TILE) break;
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_draw_image")
int tiny_image_draw_image(
    TinyImage* dest_image, const TinyImage* src_image, int32_t x, int32_t y
) {
    return tiny_image_draw_image_ex(
        dest_image, src_image, x, y, 1.0f, TINYIMG_DRAW_ONCE,
        TINYIMG_BLEND_NORMAL
    );
}

TINYIMG_EXPORT("tiny_image_composite")
int tiny_image_composite(
    TinyImage* dest_image, const TinyImage* src_image, TinyBlendMode blend
) {
    if (!dest_image || !dest_image->data || !src_image || !src_image->data) {
        return TINYIMG_ERR_NULL;
    }
    if (dest_image->width != src_image->width ||
        dest_image->height != src_image->height) {
        return TINYIMG_ERR_RANGE;
    }

    return tiny_image_draw_image_ex(
        dest_image, src_image, 0, 0, 1.0f, TINYIMG_DRAW_ONCE, blend
    );
}

TINYIMG_EXPORT("tiny_image_premultiply")
int tiny_image_premultiply(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint8_t at = alpha_index(image);
    if (at >= image->channels) return TINYIMG_OK;

    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        uint32_t alpha = p[at];

        for (uint8_t c = 0; c < at; c++) {
            p[c] = (uint8_t) ((p[c] * alpha + 127u) / 255u);
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_unpremultiply")
int tiny_image_unpremultiply(TinyImage* image) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;

    uint8_t at = alpha_index(image);
    if (at >= image->channels) return TINYIMG_OK;

    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        uint32_t alpha = p[at];

        if (alpha == 0u || alpha == 255u) continue;

        for (uint8_t c = 0; c < at; c++) {
            p[c] =
                tiny_clamp_u8((int32_t) ((p[c] * 255u + alpha / 2u) / alpha));
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region gradients

TINYIMG_EXPORT("tiny_image_gradient_linear")
int tiny_image_gradient_linear(
    TinyImage* image, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    const uint8_t* from, const uint8_t* to
) {
    if (!image || !image->data || !from || !to) return TINYIMG_ERR_NULL;

    float ex = (float) (x1 - x0);
    float ey = (float) (y1 - y0);
    float length = ex * ex + ey * ey;

    if (length <= 0.0f) return TINYIMG_ERR_RANGE;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float px = (float) x - (float) x0;
            float py = (float) y - (float) y0;
            float t = tiny_clampf((px * ex + py * ey) / length, 0.0f, 1.0f);
            uint8_t* p = pixel_at(image, x, y);

            for (uint8_t c = 0; c < image->channels; c++) {
                p[c] = tiny_clamp_u8f(
                    (float) from[c] + t * ((float) to[c] - (float) from[c])
                );
            }
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_gradient_radial")
int tiny_image_gradient_radial(
    TinyImage* image, int32_t center_x, int32_t center_y, uint32_t radius,
    const uint8_t* inner, const uint8_t* outer
) {
    if (!image || !image->data || !inner || !outer) return TINYIMG_ERR_NULL;
    if (radius == 0) return TINYIMG_ERR_RANGE;

    float r = (float) radius;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float dx = (float) x - (float) center_x;
            float dy = (float) y - (float) center_y;
            float t =
                tiny_clampf(tiny_sqrtf(dx * dx + dy * dy) / r, 0.0f, 1.0f);
            uint8_t* p = pixel_at(image, x, y);

            for (uint8_t c = 0; c < image->channels; c++) {
                p[c] = tiny_clamp_u8f(
                    (float) inner[c] + t * ((float) outer[c] - (float) inner[c])
                );
            }
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_gradient_fade")
int tiny_image_gradient_fade(
    TinyImage* image, float angle, float start, float end
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (start < 0.0f || end > 1.0f || end <= start) return TINYIMG_ERR_RANGE;

    if (alpha_index(image) >= image->channels) {
        int result = tiny_image_to_rgba(image);
        if (result != TINYIMG_OK) return result;
    }

    uint8_t at = alpha_index(image);
    float radians = angle * PI / 180.0f;
    float dx = tiny_cosf(radians);
    float dy = tiny_sinf(radians);

    // the projection is normalised by the image's extent along the direction,
    // so start and end mean the same fraction whatever the angle. one less
    // than the extent, because the last pixel's index is one less than the
    // count: dividing by the count leaves an end of 1.0 four levels short of
    // clear on a 64 wide image, which is visible as a band at the edge
    float extent = tiny_fabsf(dx) * (float) image->width +
                   tiny_fabsf(dy) * (float) image->height - 1.0f;
    float origin =
        (dx < 0.0f ? (float) (image->width - 1u) : 0.0f) * tiny_fabsf(dx) +
        (dy < 0.0f ? (float) (image->height - 1u) : 0.0f) * tiny_fabsf(dy);

    if (extent <= 0.0f) return TINYIMG_ERR_RANGE;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            float along = (dx * (float) x + dy * (float) y + origin) / extent;
            float t = tiny_clampf((along - start) / (end - start), 0.0f, 1.0f);
            uint8_t* p = pixel_at(image, x, y);

            p[at] = (uint8_t) ((float) p[at] * (1.0f - t) + 0.5f);
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region borders

TINYIMG_EXPORT("tiny_image_border")
int tiny_image_border(
    TinyImage* image, uint32_t border_width, const uint8_t* pixel
) {
    if (!image || !image->data || !pixel) return TINYIMG_ERR_NULL;
    if (border_width == 0) return TINYIMG_OK;

    uint32_t thick =
        tiny_min_u32(border_width, tiny_min_u32(image->width, image->height));

    for (uint32_t i = 0; i < thick; i++) {
        int result = tiny_image_rectangle(
            image, (int32_t) i, (int32_t) i, image->width - 2u * i,
            image->height - 2u * i, pixel
        );

        if (result != TINYIMG_OK) return result;
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_image_expand")
int tiny_image_expand(
    TinyImage* image, uint32_t left, uint32_t top, uint32_t right,
    uint32_t bottom, const uint8_t* pixel
) {
    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (left == 0 && top == 0 && right == 0 && bottom == 0) return TINYIMG_OK;

    TinyImage out;
    tiny_memset(&out, 0, sizeof(out));

    int result = tiny_image_create(
        &out, image->width + left + right, image->height + top + bottom,
        image->channels
    );
    if (result != TINYIMG_OK) return result;

    out.format = image->format;
    out.quality = image->quality;

    if (pixel) {
        uint8_t colour[4];
        colour_for(colour, pixel, image->channels);

        for (uint32_t y = 0; y < out.height; y++) {
            span(
                &out, 0, (int32_t) out.width - 1, (int32_t) y, colour,
                TINYIMG_BLEND_REPLACE
            );
        }
    }

    for (uint32_t y = 0; y < image->height; y++) {
        tiny_memcpy(
            pixel_at(&out, left, y + top), pixel_at(image, 0, y),
            (size_t) image->width * image->channels
        );
    }

    out.meta = image->meta;
    image->meta = 0;

    tiny_image_destroy(image);
    *image = out;

    return TINYIMG_OK;
}

int tiny_draw_coverage(
    TinyImage* image, int32_t x, int32_t y, const uint8_t* mask, uint32_t width,
    uint32_t height, const uint8_t* color, TinyBlendMode blend
) {
    if (!image || !image->data || !mask || !color) return TINYIMG_ERR_NULL;

    uint8_t colour[4];
    colour_for(colour, color, image->channels);

    int32_t own = source_alpha(colour, image->channels, 255);
    if (own <= 0) return TINYIMG_OK;

    for (uint32_t row = 0; row < height; row++) {
        int32_t iy = y + (int32_t) row;
        if (iy < 0 || iy >= (int32_t) image->height) continue;

        const uint8_t* line = mask + (size_t) row * width;

        for (uint32_t column = 0; column < width; column++) {
            if (line[column] == 0) continue;

            int32_t ix = x + (int32_t) column;
            if (ix < 0 || ix >= (int32_t) image->width) continue;

            blend_pixel(
                pixel_at(image, (uint32_t) ix, (uint32_t) iy), image->channels,
                colour, own * line[column] / 255, blend
            );
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region display list

TINYIMG_EXPORT("tiny_display_sizeof")
uint32_t tiny_display_sizeof(void) {
    return (uint32_t) sizeof(TinyDisplayList);
}

/** The identity, written into a 2x3 affine. */
static void affine_identity(float* m) {
    m[0] = 1.0f;
    m[1] = 0.0f;
    m[2] = 0.0f;
    m[3] = 1.0f;
    m[4] = 0.0f;
    m[5] = 0.0f;
}

/** `out = first` then `second`, as 2x3 affines. */
static void affine_mul(float* out, const float* first, const float* second) {
    float m[6];

    m[0] = first[0] * second[0] + first[1] * second[2];
    m[1] = first[0] * second[1] + first[1] * second[3];
    m[2] = first[2] * second[0] + first[3] * second[2];
    m[3] = first[2] * second[1] + first[3] * second[3];
    m[4] = first[4] * second[0] + first[5] * second[2] + second[4];
    m[5] = first[4] * second[1] + first[5] * second[3] + second[5];

    for (uint32_t i = 0; i < 6u; i++) out[i] = m[i];
}

static void affine_apply(
    const float* m, float x, float y, float* out_x, float* out_y
) {
    *out_x = m[0] * x + m[2] * y + m[4];
    *out_y = m[1] * x + m[3] * y + m[5];
}

TINYIMG_EXPORT("tiny_display_init")
int tiny_display_init(TinyDisplayList* list) {
    if (!list) return TINYIMG_ERR_NULL;

    tiny_memset(list, 0, sizeof(*list));
    affine_identity(list->transform);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_save")
int tiny_display_save(TinyDisplayList* list) {
    if (!list) return TINYIMG_ERR_NULL;
    if (list->depth >= TINYIMG_DISPLAY_MAX_DEPTH) return TINYIMG_ERR_BOUNDS;

    for (uint32_t i = 0; i < 6u; i++) {
        list->stack[list->depth][i] = list->transform[i];
    }

    list->depth++;
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_restore")
int tiny_display_restore(TinyDisplayList* list) {
    if (!list) return TINYIMG_ERR_NULL;
    if (list->depth == 0) return TINYIMG_ERR_BOUNDS;

    list->depth--;

    for (uint32_t i = 0; i < 6u; i++) {
        list->transform[i] = list->stack[list->depth][i];
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_translate")
int tiny_display_translate(TinyDisplayList* list, float x, float y) {
    if (!list) return TINYIMG_ERR_NULL;

    float move[6] = {1.0f, 0.0f, 0.0f, 1.0f, x, y};
    affine_mul(list->transform, move, list->transform);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_scale")
int tiny_display_scale(TinyDisplayList* list, float x, float y) {
    if (!list) return TINYIMG_ERR_NULL;

    float scale[6] = {x, 0.0f, 0.0f, y, 0.0f, 0.0f};
    affine_mul(list->transform, scale, list->transform);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_rotate")
int tiny_display_rotate(TinyDisplayList* list, float degrees) {
    if (!list) return TINYIMG_ERR_NULL;

    float radians = degrees * PI / 180.0f;
    float c = tiny_cosf(radians);
    float s = tiny_sinf(radians);
    float turn[6] = {c, s, -s, c, 0.0f, 0.0f};

    affine_mul(list->transform, turn, list->transform);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_set_transform")
int tiny_display_set_transform(TinyDisplayList* list, const float* matrix) {
    if (!list || !matrix) return TINYIMG_ERR_NULL;

    for (uint32_t i = 0; i < 6u; i++) list->transform[i] = matrix[i];

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_blend")
int tiny_display_blend(TinyDisplayList* list, TinyBlendMode blend) {
    if (!list) return TINYIMG_ERR_NULL;

    list->blend = blend;
    return TINYIMG_OK;
}

/**
 * @brief Reserves a shape and fills in what every shape carries.
 *
 * @param list The list.
 * @param kind Which shape.
 * @param color Its colour.
 * @return TinyShape* The shape, or NULL when the list is full.
 */
static TinyShape* shape_add(
    TinyDisplayList* list, TinyShapeKind kind, const uint8_t* color
) {
    if (list->count >= TINYIMG_DISPLAY_MAX_SHAPES) return 0;

    TinyShape* shape = &list->shapes[list->count++];
    tiny_memset(shape, 0, sizeof(*shape));

    shape->kind = kind;
    shape->blend = list->blend;

    for (uint32_t i = 0; i < 4u; i++) shape->color[i] = color[i];
    for (uint32_t i = 0; i < 6u; i++) shape->transform[i] = list->transform[i];

    return shape;
}

TINYIMG_EXPORT("tiny_display_rect")
int tiny_display_rect(
    TinyDisplayList* list, float x, float y, float width, float height,
    const uint8_t* color
) {
    if (!list || !color) return TINYIMG_ERR_NULL;

    TinyShape* shape = shape_add(list, TINYIMG_SHAPE_RECT, color);
    if (!shape) return TINYIMG_ERR_BOUNDS;

    shape->geometry[0] = x;
    shape->geometry[1] = y;
    shape->geometry[2] = width;
    shape->geometry[3] = height;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_round_rect")
int tiny_display_round_rect(
    TinyDisplayList* list, float x, float y, float width, float height,
    float radius, const uint8_t* color
) {
    if (!list || !color) return TINYIMG_ERR_NULL;

    TinyShape* shape = shape_add(list, TINYIMG_SHAPE_ROUND_RECT, color);
    if (!shape) return TINYIMG_ERR_BOUNDS;

    shape->geometry[0] = x;
    shape->geometry[1] = y;
    shape->geometry[2] = width;
    shape->geometry[3] = height;
    shape->geometry[4] = radius;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_ellipse")
int tiny_display_ellipse(
    TinyDisplayList* list, float center_x, float center_y, float radius_x,
    float radius_y, const uint8_t* color
) {
    if (!list || !color) return TINYIMG_ERR_NULL;

    TinyShape* shape = shape_add(list, TINYIMG_SHAPE_ELLIPSE, color);
    if (!shape) return TINYIMG_ERR_BOUNDS;

    shape->geometry[0] = center_x;
    shape->geometry[1] = center_y;
    shape->geometry[2] = radius_x;
    shape->geometry[3] = radius_y;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_line")
int tiny_display_line(
    TinyDisplayList* list, float x1, float y1, float x2, float y2,
    float thickness, const uint8_t* color
) {
    if (!list || !color) return TINYIMG_ERR_NULL;

    TinyShape* shape = shape_add(list, TINYIMG_SHAPE_LINE, color);
    if (!shape) return TINYIMG_ERR_BOUNDS;

    shape->geometry[0] = x1;
    shape->geometry[1] = y1;
    shape->geometry[2] = x2;
    shape->geometry[3] = y2;
    shape->geometry[4] = thickness;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_display_polygon")
int tiny_display_polygon(
    TinyDisplayList* list, const float* x_points, const float* y_points,
    size_t num_points, const uint8_t* color, TinyFillRule rule
) {
    if (!list || !x_points || !y_points || !color) return TINYIMG_ERR_NULL;
    if (num_points < 3u) return TINYIMG_ERR_RANGE;
    if (list->points + num_points > TINYIMG_DISPLAY_MAX_POINTS) {
        return TINYIMG_ERR_BOUNDS;
    }

    TinyShape* shape = shape_add(list, TINYIMG_SHAPE_POLYGON, color);
    if (!shape) return TINYIMG_ERR_BOUNDS;

    shape->rule = rule;
    shape->point_first = list->points;
    shape->point_count = (uint32_t) num_points;

    for (size_t i = 0; i < num_points; i++) {
        list->point[(list->points + i) * 2u] = x_points[i];
        list->point[(list->points + i) * 2u + 1u] = y_points[i];
    }

    list->points += (uint32_t) num_points;
    return TINYIMG_OK;
}

/**
 * @brief The axis-aligned box a shape's transformed outline falls inside.
 *
 * Its four corners are transformed and bounded, which is exact for a
 * rectangle and a conservative cover for the others. Conservative is the
 * right direction: culling on it can only keep a shape that would have drawn
 * nothing, never drop one that would have drawn something.
 *
 * @param list The list, for the polygon vertices.
 * @param shape The shape.
 * @param box Receives x0, y0, x1, y1.
 */
static void shape_bounds(
    const TinyDisplayList* list, const TinyShape* shape, float* box
) {
    float xs[4];
    float ys[4];
    uint32_t count = 4;

    switch (shape->kind) {
        case TINYIMG_SHAPE_RECT:
        case TINYIMG_SHAPE_ROUND_RECT:
            xs[0] = shape->geometry[0];
            ys[0] = shape->geometry[1];
            xs[1] = shape->geometry[0] + shape->geometry[2];
            ys[1] = shape->geometry[1];
            xs[2] = shape->geometry[0] + shape->geometry[2];
            ys[2] = shape->geometry[1] + shape->geometry[3];
            xs[3] = shape->geometry[0];
            ys[3] = shape->geometry[1] + shape->geometry[3];
            break;
        case TINYIMG_SHAPE_ELLIPSE:
            xs[0] = shape->geometry[0] - shape->geometry[2];
            ys[0] = shape->geometry[1] - shape->geometry[3];
            xs[1] = shape->geometry[0] + shape->geometry[2];
            ys[1] = shape->geometry[1] - shape->geometry[3];
            xs[2] = shape->geometry[0] + shape->geometry[2];
            ys[2] = shape->geometry[1] + shape->geometry[3];
            xs[3] = shape->geometry[0] - shape->geometry[2];
            ys[3] = shape->geometry[1] + shape->geometry[3];
            break;
        case TINYIMG_SHAPE_LINE: {
            float half = shape->geometry[4] * 0.5f + 1.0f;

            xs[0] = shape->geometry[0] - half;
            ys[0] = shape->geometry[1] - half;
            xs[1] = shape->geometry[2] + half;
            ys[1] = shape->geometry[1] - half;
            xs[2] = shape->geometry[2] + half;
            ys[2] = shape->geometry[3] + half;
            xs[3] = shape->geometry[0] - half;
            ys[3] = shape->geometry[3] + half;
            break;
        }
        default: count = 0; break;
    }

    if (shape->kind == TINYIMG_SHAPE_POLYGON) {
        float first_x;
        float first_y;

        affine_apply(
            shape->transform, list->point[shape->point_first * 2u],
            list->point[shape->point_first * 2u + 1u], &first_x, &first_y
        );

        box[0] = first_x;
        box[1] = first_y;
        box[2] = first_x;
        box[3] = first_y;

        for (uint32_t i = 1; i < shape->point_count; i++) {
            uint32_t at = (shape->point_first + i) * 2u;
            float px;
            float py;

            affine_apply(
                shape->transform, list->point[at], list->point[at + 1u], &px,
                &py
            );

            if (px < box[0]) box[0] = px;
            if (py < box[1]) box[1] = py;
            if (px > box[2]) box[2] = px;
            if (py > box[3]) box[3] = py;
        }

        return;
    }

    if (count == 0) {
        box[0] = 0.0f;
        box[1] = 0.0f;
        box[2] = 0.0f;
        box[3] = 0.0f;
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        float px;
        float py;

        affine_apply(shape->transform, xs[i], ys[i], &px, &py);
        xs[i] = px;
        ys[i] = py;
    }

    box[0] = xs[0];
    box[1] = ys[0];
    box[2] = xs[0];
    box[3] = ys[0];

    for (uint32_t i = 1; i < count; i++) {
        if (xs[i] < box[0]) box[0] = xs[i];
        if (ys[i] < box[1]) box[1] = ys[i];
        if (xs[i] > box[2]) box[2] = xs[i];
        if (ys[i] > box[3]) box[3] = ys[i];
    }
}

/**
 * @brief Whether a shape covers a box completely and opaquely.
 *
 * Only an axis-aligned opaque rectangle in the default or replace mode can
 * answer yes; anything else is either not a full cover of its own box or not
 * opaque, and a wrong yes would drop a shape that shows through. Being wrong
 * in the other direction only costs a pass.
 *
 * @param shape The shape.
 * @param channels The target's channel count.
 * @param box The box to test, as x0, y0, x1, y1.
 * @return int Non-zero when nothing under the box can show through.
 */
static int shape_covers(
    const TinyShape* shape, uint8_t channels, const float* box
) {
    if (shape->kind != TINYIMG_SHAPE_RECT) return 0;
    if (shape->blend != TINYIMG_BLEND_NORMAL &&
        shape->blend != TINYIMG_BLEND_REPLACE) {
        return 0;
    }
    if ((channels == 2u || channels == 4u) &&
        shape->color[channels - 1u] != 255u) {
        return 0;
    }

    // a rotation or a shear leaves the rectangle covering less than its own
    // box, so only a scale and a translation are allowed to claim a cover
    if (shape->transform[1] != 0.0f || shape->transform[2] != 0.0f) return 0;

    float own[4];
    float corners[4] = {
        shape->geometry[0], shape->geometry[1],
        shape->geometry[0] + shape->geometry[2],
        shape->geometry[1] + shape->geometry[3]
    };

    affine_apply(shape->transform, corners[0], corners[1], &own[0], &own[1]);
    affine_apply(shape->transform, corners[2], corners[3], &own[2], &own[3]);

    if (own[0] > own[2]) {
        float swap = own[0];
        own[0] = own[2];
        own[2] = swap;
    }
    if (own[1] > own[3]) {
        float swap = own[1];
        own[1] = own[3];
        own[3] = swap;
    }

    return own[0] <= box[0] && own[1] <= box[1] && own[2] >= box[2] &&
           own[3] >= box[3];
}

/** Draws one shape, with its own transform in force. */
static int shape_render(
    const TinyDisplayList* list, const TinyShape* shape, TinyImage* image
) {
    const float* m = shape->transform;
    uint8_t colour[4];

    colour_for(colour, shape->color, image->channels);

    // a transform with no rotation or shear maps a rectangle to a rectangle
    // and an ellipse to an ellipse, so the primitives take it directly; a
    // general one is walked as a polygon, which is exact for the straight
    // shapes and a flattening for the round ones
    int upright = m[1] == 0.0f && m[2] == 0.0f;

    switch (shape->kind) {
        case TINYIMG_SHAPE_RECT:
        case TINYIMG_SHAPE_ROUND_RECT: {
            float x0;
            float y0;
            float x1;
            float y1;

            affine_apply(m, shape->geometry[0], shape->geometry[1], &x0, &y0);
            affine_apply(
                m, shape->geometry[0] + shape->geometry[2],
                shape->geometry[1] + shape->geometry[3], &x1, &y1
            );

            if (!upright) {
                float xs[4];
                float ys[4];

                affine_apply(
                    m, shape->geometry[0], shape->geometry[1], &xs[0], &ys[0]
                );
                affine_apply(
                    m, shape->geometry[0] + shape->geometry[2],
                    shape->geometry[1], &xs[1], &ys[1]
                );
                affine_apply(
                    m, shape->geometry[0] + shape->geometry[2],
                    shape->geometry[1] + shape->geometry[3], &xs[2], &ys[2]
                );
                affine_apply(
                    m, shape->geometry[0],
                    shape->geometry[1] + shape->geometry[3], &xs[3], &ys[3]
                );

                return polygon_fill(
                    image, xs, ys, 4u, colour, TINYIMG_FILL_NONZERO,
                    shape->blend
                );
            }

            int32_t left = (int32_t) (x0 < x1 ? x0 : x1);
            int32_t top = (int32_t) (y0 < y1 ? y0 : y1);
            uint32_t width = (uint32_t) tiny_fabsf(x1 - x0);
            uint32_t height = (uint32_t) tiny_fabsf(y1 - y0);

            if (shape->kind == TINYIMG_SHAPE_ROUND_RECT) {
                float scale = tiny_fabsf(m[0]) < tiny_fabsf(m[3])
                                  ? tiny_fabsf(m[0])
                                  : tiny_fabsf(m[3]);

                return tiny_image_fill_rounded_rectangle(
                    image, left, top, width, height,
                    (uint32_t) (shape->geometry[4] * scale + 0.5f), colour
                );
            }

            for (uint32_t row = 0; row < height; row++) {
                span(
                    image, left, left + (int32_t) width - 1,
                    top + (int32_t) row, colour, shape->blend
                );
            }

            return TINYIMG_OK;
        }
        case TINYIMG_SHAPE_ELLIPSE: {
            float cx;
            float cy;

            affine_apply(m, shape->geometry[0], shape->geometry[1], &cx, &cy);

            if (upright) {
                ellipse_fill(
                    image, (int32_t) cx, (int32_t) cy,
                    (uint32_t) (shape->geometry[2] * tiny_fabsf(m[0]) + 0.5f),
                    (uint32_t) (shape->geometry[3] * tiny_fabsf(m[3]) + 0.5f),
                    colour, shape->blend
                );

                return TINYIMG_OK;
            }

            // 32 segments, which is under a third of a pixel of chord error
            // for a radius up to about 400 and is what a flattening tolerance
            // of a quarter pixel works out to at this size
            float xs[32];
            float ys[32];

            for (uint32_t i = 0; i < 32u; i++) {
                float angle = (float) i * (2.0f * PI / 32.0f);
                float px;
                float py;

                affine_apply(
                    m,
                    shape->geometry[0] + shape->geometry[2] * tiny_cosf(angle),
                    shape->geometry[1] + shape->geometry[3] * tiny_sinf(angle),
                    &px, &py
                );

                xs[i] = px;
                ys[i] = py;
            }

            return polygon_fill(
                image, xs, ys, 32u, colour, TINYIMG_FILL_NONZERO, shape->blend
            );
        }
        case TINYIMG_SHAPE_LINE: {
            float x0;
            float y0;
            float x1;
            float y1;

            affine_apply(m, shape->geometry[0], shape->geometry[1], &x0, &y0);
            affine_apply(m, shape->geometry[2], shape->geometry[3], &x1, &y1);

            float scale = tiny_sqrtf(tiny_fabsf(m[0] * m[3] - m[1] * m[2]));

            return tiny_image_draw_line(
                image, (int32_t) x0, (int32_t) y0, (int32_t) x1, (int32_t) y1,
                (uint32_t) (shape->geometry[4] * scale + 0.5f), colour
            );
        }
        case TINYIMG_SHAPE_POLYGON: {
            TinyArenaMark mark;
            tiny_arena_mark(&mark);

            float* xs = tiny_arena_alloc(shape->point_count * sizeof(float), 4);
            float* ys = tiny_arena_alloc(shape->point_count * sizeof(float), 4);

            if (!xs || !ys) {
                tiny_arena_release(&mark);
                return TINYIMG_ERR_MEMORY;
            }

            for (uint32_t i = 0; i < shape->point_count; i++) {
                uint32_t at = (shape->point_first + i) * 2u;
                float px;
                float py;

                affine_apply(
                    m, list->point[at], list->point[at + 1u], &px, &py
                );

                xs[i] = px;
                ys[i] = py;
            }

            int result = polygon_fill(
                image, xs, ys, shape->point_count, colour, shape->rule,
                shape->blend
            );

            tiny_arena_release(&mark);
            return result;
        }
        default: return TINYIMG_OK;
    }
}

TINYIMG_EXPORT("tiny_display_render")
int tiny_display_render(TinyDisplayList* list, TinyImage* image) {
    if (!list || !image || !image->data) return TINYIMG_ERR_NULL;

    list->culled = 0;
    list->covered = 0;

    if (list->count == 0) return TINYIMG_OK;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    float* boxes = tiny_arena_alloc(list->count * 4u * sizeof(float), 4);
    uint8_t* draw = tiny_arena_alloc(list->count, 1);

    if (!boxes || !draw) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (uint32_t i = 0; i < list->count; i++) {
        shape_bounds(list, &list->shapes[i], &boxes[i * 4u]);

        const float* box = &boxes[i * 4u];
        int outside = box[2] < 0.0f || box[3] < 0.0f ||
                      box[0] > (float) image->width ||
                      box[1] > (float) image->height || box[0] >= box[2] ||
                      box[1] >= box[3];

        draw[i] = outside ? 0u : 1u;
        if (outside) list->culled++;
    }

    // a shape a later opaque one covers entirely never reaches the image, and
    // the geometry says so before any pixel exists. this is the whole reason
    // the list is symbolic rather than drawn as it is built
    for (uint32_t i = 0; i < list->count; i++) {
        if (!draw[i]) continue;

        for (uint32_t j = i + 1u; j < list->count; j++) {
            if (!draw[j]) continue;

            if (shape_covers(
                    &list->shapes[j], image->channels, &boxes[i * 4u]
                )) {
                draw[i] = 0u;
                list->covered++;
                break;
            }
        }
    }

    int result = TINYIMG_OK;

    for (uint32_t i = 0; i < list->count && result == TINYIMG_OK; i++) {
        if (!draw[i]) continue;

        result = shape_render(list, &list->shapes[i], image);
    }

    tiny_arena_release(&mark);
    return result;
}

TINYIMG_EXPORT("tiny_display_culled")
uint32_t tiny_display_culled(const TinyDisplayList* list) {
    return list ? list->culled : 0u;
}

TINYIMG_EXPORT("tiny_display_covered")
uint32_t tiny_display_covered(const TinyDisplayList* list) {
    return list ? list->covered : 0u;
}

TINYIMG_EXPORT("tiny_display_bounds")
int tiny_display_bounds(
    const TinyDisplayList* list, int32_t* x, int32_t* y, uint32_t* width,
    uint32_t* height
) {
    if (!list || !x || !y || !width || !height) return TINYIMG_ERR_NULL;

    *x = 0;
    *y = 0;
    *width = 0;
    *height = 0;

    if (list->count == 0) return TINYIMG_OK;

    float box[4];
    float total[4];

    shape_bounds(list, &list->shapes[0], total);

    for (uint32_t i = 1; i < list->count; i++) {
        shape_bounds(list, &list->shapes[i], box);

        if (box[0] < total[0]) total[0] = box[0];
        if (box[1] < total[1]) total[1] = box[1];
        if (box[2] > total[2]) total[2] = box[2];
        if (box[3] > total[3]) total[3] = box[3];
    }

    *x = (int32_t) tiny_floorf(total[0]);
    *y = (int32_t) tiny_floorf(total[1]);
    *width = (uint32_t) (tiny_ceilf(total[2]) - tiny_floorf(total[0]));
    *height = (uint32_t) (tiny_ceilf(total[3]) - tiny_floorf(total[1]));

    return TINYIMG_OK;
}

#pragma endregion
