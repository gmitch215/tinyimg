#include "tinyimg/color.h"

#include "tinyimg/memory.h"
#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#pragma region color conversion

TINYIMG_EXPORT("tiny_rgb_to_grayscale")
int tiny_rgb_to_grayscale(uint8_t r, uint8_t g, uint8_t b) {
    // Rec. 709 in 16.16, the same weights every luminance in the library uses
    uint32_t sum = 13933u * r + 46871u * g + 4732u * b;

    return (int) ((sum + 32768u) >> 16);
}

TINYIMG_EXPORT("tiny_grayscale_to_rgb")
int tiny_grayscale_to_rgb(
    uint8_t grayscale, uint8_t* r, uint8_t* g, uint8_t* b
) {
    if (!r || !g || !b) return TINYIMG_ERR_NULL;

    *r = grayscale;
    *g = grayscale;
    *b = grayscale;

    return TINYIMG_OK;
}

/**
 * @brief The pieces every hue computation needs.
 *
 * @param r Red, 0 through 255.
 * @param g Green.
 * @param b Blue.
 * @param low Receives the smallest channel, 0 through 1.
 * @param high Receives the largest.
 * @param hue Receives the hue in degrees, or zero when there is no chroma.
 */
static void hue_of(
    uint8_t r, uint8_t g, uint8_t b, float* low, float* high, float* hue
) {
    float red = (float) r / 255.0f;
    float green = (float) g / 255.0f;
    float blue = (float) b / 255.0f;

    float top =
        red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
    float bottom =
        red < green ? (red < blue ? red : blue) : (green < blue ? green : blue);
    float chroma = top - bottom;

    *low = bottom;
    *high = top;

    if (chroma <= 0.0f) {
        *hue = 0.0f;
        return;
    }

    float angle;

    if (top == red)
        angle = (green - blue) / chroma;
    else if (top == green)
        angle = 2.0f + (blue - red) / chroma;
    else
        angle = 4.0f + (red - green) / chroma;

    angle *= 60.0f;
    if (angle < 0.0f) angle += 360.0f;

    *hue = angle;
}

TINYIMG_EXPORT("tiny_rgb_to_hsv")
int tiny_rgb_to_hsv(
    uint8_t r, uint8_t g, uint8_t b, float* h, float* s, float* v
) {
    if (!h || !s || !v) return TINYIMG_ERR_NULL;

    float low;
    float high;
    hue_of(r, g, b, &low, &high, h);

    *v = high;
    *s = high <= 0.0f ? 0.0f : (high - low) / high;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_rgb_to_hsl")
int tiny_rgb_to_hsl(
    uint8_t r, uint8_t g, uint8_t b, float* h, float* s, float* l
) {
    if (!h || !s || !l) return TINYIMG_ERR_NULL;

    float low;
    float high;
    hue_of(r, g, b, &low, &high, h);

    float chroma = high - low;
    float lightness = (high + low) * 0.5f;

    *l = lightness;

    // the divisor turns over at half lightness, which is what makes a
    // saturation of one reachable at every lightness rather than only at a
    // half; taking chroma over the maximum instead is HSV's saturation
    if (chroma <= 0.0f) {
        *s = 0.0f;
    }
    else {
        float span = lightness > 0.5f ? 2.0f - high - low : high + low;
        *s = chroma / span;
    }

    return TINYIMG_OK;
}

/**
 * @brief Builds RGB from a hue, a chroma and a floor.
 *
 * Shared by both inverse conversions, which differ only in what they compute
 * for the chroma and the floor.
 *
 * @param hue Degrees.
 * @param chroma How far the largest and smallest channels are apart.
 * @param floor_value What the smallest channel is.
 * @param r Receives red.
 * @param g Receives green.
 * @param b Receives blue.
 */
static void rgb_from_hue(
    float hue, float chroma, float floor_value, uint8_t* r, uint8_t* g,
    uint8_t* b
) {
    float wrapped = tiny_fmodf(hue, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;

    float sector = wrapped / 60.0f;
    float rise = chroma * (1.0f - tiny_fabsf(tiny_fmodf(sector, 2.0f) - 1.0f));

    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;

    if (sector < 1.0f) {
        red = chroma;
        green = rise;
    }
    else if (sector < 2.0f) {
        red = rise;
        green = chroma;
    }
    else if (sector < 3.0f) {
        green = chroma;
        blue = rise;
    }
    else if (sector < 4.0f) {
        green = rise;
        blue = chroma;
    }
    else if (sector < 5.0f) {
        red = rise;
        blue = chroma;
    }
    else {
        red = chroma;
        blue = rise;
    }

    *r = tiny_clamp_u8f((red + floor_value) * 255.0f);
    *g = tiny_clamp_u8f((green + floor_value) * 255.0f);
    *b = tiny_clamp_u8f((blue + floor_value) * 255.0f);
}

TINYIMG_EXPORT("tiny_hsv_to_rgb")
int tiny_hsv_to_rgb(
    float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b
) {
    if (!r || !g || !b) return TINYIMG_ERR_NULL;
    if (s < 0.0f || s > 1.0f || v < 0.0f || v > 1.0f) return TINYIMG_ERR_RANGE;

    float chroma = v * s;
    rgb_from_hue(h, chroma, v - chroma, r, g, b);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_hsl_to_rgb")
int tiny_hsl_to_rgb(
    float h, float s, float l, uint8_t* r, uint8_t* g, uint8_t* b
) {
    if (!r || !g || !b) return TINYIMG_ERR_NULL;
    if (s < 0.0f || s > 1.0f || l < 0.0f || l > 1.0f) return TINYIMG_ERR_RANGE;

    float chroma = (1.0f - tiny_fabsf(2.0f * l - 1.0f)) * s;
    rgb_from_hue(h, chroma, l - chroma * 0.5f, r, g, b);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_rgb_to_cmyk")
int tiny_rgb_to_cmyk(
    uint8_t r, uint8_t g, uint8_t b, float* c, float* m, float* y, float* k
) {
    if (!c || !m || !y || !k) return TINYIMG_ERR_NULL;

    float red = (float) r / 255.0f;
    float green = (float) g / 255.0f;
    float blue = (float) b / 255.0f;

    float top =
        red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);

    *k = 1.0f - top;

    if (top <= 0.0f) {
        // black has no hue to record, and dividing by the zero ink level would
        // otherwise put whatever the rounding gave into all three
        *c = 0.0f;
        *m = 0.0f;
        *y = 0.0f;
        return TINYIMG_OK;
    }

    *c = (top - red) / top;
    *m = (top - green) / top;
    *y = (top - blue) / top;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_cmyk_to_rgb")
int tiny_cmyk_to_rgb(
    float c, float m, float y, float k, uint8_t* r, uint8_t* g, uint8_t* b
) {
    if (!r || !g || !b) return TINYIMG_ERR_NULL;
    if (c < 0.0f || c > 1.0f || m < 0.0f || m > 1.0f) return TINYIMG_ERR_RANGE;
    if (y < 0.0f || y > 1.0f || k < 0.0f || k > 1.0f) return TINYIMG_ERR_RANGE;

    float ink = 1.0f - k;

    *r = tiny_clamp_u8f(255.0f * (1.0f - c) * ink);
    *g = tiny_clamp_u8f(255.0f * (1.0f - m) * ink);
    *b = tiny_clamp_u8f(255.0f * (1.0f - y) * ink);

    return TINYIMG_OK;
}

#pragma endregion

#pragma region color interpolation

TINYIMG_EXPORT("tiny_interpolate_rgb")
int tiny_interpolate_rgb(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2,
    float t, uint8_t* r_out, uint8_t* g_out, uint8_t* b_out
) {
    if (!r_out || !g_out || !b_out) return TINYIMG_ERR_NULL;
    if (t < 0.0f || t > 1.0f) return TINYIMG_ERR_RANGE;

    *r_out = tiny_clamp_u8f((float) r1 + t * ((float) r2 - (float) r1));
    *g_out = tiny_clamp_u8f((float) g1 + t * ((float) g2 - (float) g1));
    *b_out = tiny_clamp_u8f((float) b1 + t * ((float) b2 - (float) b1));

    return TINYIMG_OK;
}

/**
 * @brief Interpolates a hue the short way round the circle.
 *
 * A hue is an angle, so 350 and 10 are twenty degrees apart and not three
 * hundred and forty. Interpolating them as plain numbers walks the long way
 * and passes through every other hue on the wheel, which is visible as a
 * rainbow smear in what should be a short red to red blend.
 *
 * @param from The first hue in degrees.
 * @param to The second.
 * @param t How far, 0 through 1.
 * @return float The hue, in 0 through 360.
 */
static float interpolate_hue(float from, float to, float t) {
    float delta = to - from;

    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;

    float out = from + t * delta;

    out = tiny_fmodf(out, 360.0f);
    if (out < 0.0f) out += 360.0f;

    return out;
}

/** Both cylindrical interpolations, which differ only in what they are named.
 */
static int interpolate_cylindrical(
    float h1, float s1, float z1, float h2, float s2, float z2, float t,
    float* h_out, float* s_out, float* z_out
) {
    if (!h_out || !s_out || !z_out) return TINYIMG_ERR_NULL;
    if (t < 0.0f || t > 1.0f) return TINYIMG_ERR_RANGE;

    *h_out = interpolate_hue(h1, h2, t);
    *s_out = s1 + t * (s2 - s1);
    *z_out = z1 + t * (z2 - z1);

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_interpolate_hsv")
int tiny_interpolate_hsv(
    float h1, float s1, float v1, float h2, float s2, float v2, float t,
    float* h_out, float* s_out, float* v_out
) {
    return interpolate_cylindrical(
        h1, s1, v1, h2, s2, v2, t, h_out, s_out, v_out
    );
}

TINYIMG_EXPORT("tiny_interpolate_hsl")
int tiny_interpolate_hsl(
    float h1, float s1, float l1, float h2, float s2, float l2, float t,
    float* h_out, float* s_out, float* l_out
) {
    return interpolate_cylindrical(
        h1, s1, l1, h2, s2, l2, t, h_out, s_out, l_out
    );
}

#pragma endregion

#pragma region color palette generation

/**
 * @brief Reserves a gradient's output from the arena.
 *
 * @param steps How many entries.
 * @param components How many numbers each entry holds.
 * @param size How wide one number is.
 * @return void* The array, or NULL when the request is empty or the arena is
 * full.
 */
static void* gradient_alloc(size_t steps, size_t components, size_t size) {
    if (steps < 2u) return 0;

    return tiny_arena_alloc(steps * components * size, 4);
}

/** How far along a gradient step `i` of `steps` is. */
static float gradient_t(size_t i, size_t steps) {
    return (float) i / (float) (steps - 1u);
}

TINYIMG_EXPORT("tiny_gradient_rgb")
int* tiny_gradient_rgb(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2,
    size_t steps
) {
    int* out = gradient_alloc(steps, 1, sizeof(int));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        uint8_t r;
        uint8_t g;
        uint8_t b;

        tiny_interpolate_rgb(
            r1, g1, b1, r2, g2, b2, gradient_t(i, steps), &r, &g, &b
        );

        out[i] = (int) (((uint32_t) r << 16) | ((uint32_t) g << 8) | b);
    }

    return out;
}

TINYIMG_EXPORT("tiny_gradient_rgba")
int* tiny_gradient_rgba(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1, uint8_t r2, uint8_t g2,
    uint8_t b2, uint8_t a2, size_t steps
) {
    int* out = gradient_alloc(steps, 1, sizeof(int));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        float t = gradient_t(i, steps);
        uint8_t r;
        uint8_t g;
        uint8_t b;

        tiny_interpolate_rgb(r1, g1, b1, r2, g2, b2, t, &r, &g, &b);

        uint8_t a = tiny_clamp_u8f((float) a1 + t * ((float) a2 - (float) a1));

        out[i] = (int) (((uint32_t) r << 24) | ((uint32_t) g << 16) |
                        ((uint32_t) b << 8) | a);
    }

    return out;
}

TINYIMG_EXPORT("tiny_gradient_hsv")
float* tiny_gradient_hsv(
    float h1, float s1, float v1, float h2, float s2, float v2, size_t steps
) {
    float* out = gradient_alloc(steps, 3, sizeof(float));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        interpolate_cylindrical(
            h1, s1, v1, h2, s2, v2, gradient_t(i, steps), &out[i * 3u],
            &out[i * 3u + 1u], &out[i * 3u + 2u]
        );
    }

    return out;
}

TINYIMG_EXPORT("tiny_gradient_hsl")
float* tiny_gradient_hsl(
    float h1, float s1, float l1, float h2, float s2, float l2, size_t steps
) {
    return tiny_gradient_hsv(h1, s1, l1, h2, s2, l2, steps);
}

TINYIMG_EXPORT("tiny_gradient_cmyk")
float* tiny_gradient_cmyk(
    float c1, float m1, float y1, float k1, float c2, float m2, float y2,
    float k2, size_t steps
) {
    float* out = gradient_alloc(steps, 4, sizeof(float));
    if (!out) return 0;

    const float from[4] = {c1, m1, y1, k1};
    const float to[4] = {c2, m2, y2, k2};

    for (size_t i = 0; i < steps; i++) {
        float t = gradient_t(i, steps);

        for (uint32_t c = 0; c < 4u; c++) {
            out[i * 4u + c] = from[c] + t * (to[c] - from[c]);
        }
    }

    return out;
}

/**
 * @brief Where one entry of a multi-gradient falls.
 *
 * The five multi-gradients differ only in how one leg's colors are mixed, so
 * the walk across the legs is written once.
 *
 * The whole gradient is one parameter from zero to one and each leg owns a
 * slice of it, rather than each leg owning a whole number of steps. That is
 * what keeps the joins seamless at any distribution: handing out whole steps
 * per leg has to round, and a leg that rounds down ends one step short of its
 * own end color, which shows as a step change at the join.
 *
 * @param num_colors How many stops.
 * @param steps The total steps.
 * @param index Which output entry.
 * @param distribution One weight per leg, or NULL for even legs.
 * @param leg Receives which leg the entry falls in.
 * @param t Receives how far along that leg, 0 through 1.
 */
static void multigradient_at(
    size_t num_colors, size_t steps, size_t index, const float* distribution,
    size_t* leg, float* t
) {
    size_t legs = num_colors - 1u;
    float position = gradient_t(index, steps);
    float start = 0.0f;

    for (size_t i = 0; i < legs; i++) {
        float width = distribution ? distribution[i] : 1.0f / (float) legs;

        if (width <= 0.0f) continue;

        if (position <= start + width || i + 1u == legs) {
            *leg = i;
            *t = tiny_clampf((position - start) / width, 0.0f, 1.0f);
            return;
        }

        start += width;
    }

    *leg = legs - 1u;
    *t = 1.0f;
}

/**
 * @brief Whether a distribution is one this can use.
 *
 * @param distribution The weights, or NULL which is always acceptable.
 * @param legs How many there should be.
 * @return int Non-zero when it is usable.
 */
static int distribution_ok(const float* distribution, size_t legs) {
    if (!distribution) return 1;

    float total = 0.0f;

    for (size_t i = 0; i < legs; i++) {
        if (distribution[i] < 0.0f) return 0;
        total += distribution[i];
    }

    return total > 0.99f && total < 1.01f;
}

/** Both packed multi-gradients, which differ only in the alpha channel. */
static int* multigradient_packed(
    const uint8_t* colors, size_t num_colors, size_t steps,
    const float* distribution, int with_alpha
) {
    size_t stride = with_alpha ? 4u : 3u;

    if (!colors || num_colors < 2u) return 0;
    if (!distribution_ok(distribution, num_colors - 1u)) return 0;

    int* out = gradient_alloc(steps, 1, sizeof(int));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        size_t leg;
        float t;
        multigradient_at(num_colors, steps, i, distribution, &leg, &t);

        const uint8_t* from = colors + leg * stride;
        const uint8_t* to = colors + (leg + 1u) * stride;
        uint8_t r;
        uint8_t g;
        uint8_t b;

        tiny_interpolate_rgb(
            from[0], from[1], from[2], to[0], to[1], to[2], t, &r, &g, &b
        );

        if (!with_alpha) {
            out[i] = (int) (((uint32_t) r << 16) | ((uint32_t) g << 8) | b);
            continue;
        }

        uint8_t a = tiny_clamp_u8f(
            (float) from[3] + t * ((float) to[3] - (float) from[3])
        );

        out[i] = (int) (((uint32_t) r << 24) | ((uint32_t) g << 16) |
                        ((uint32_t) b << 8) | a);
    }

    return out;
}

TINYIMG_EXPORT("tiny_multigradient_rgb_sized")
int* tiny_multigradient_rgb_sized(
    uint8_t* colors, size_t num_colors, size_t steps, float* color_distribution
) {
    if (!color_distribution) return 0;

    return multigradient_packed(
        colors, num_colors, steps, color_distribution, 0
    );
}

TINYIMG_EXPORT("tiny_multigradient_rgb")
int* tiny_multigradient_rgb(uint8_t* colors, size_t num_colors, size_t steps) {
    return multigradient_packed(colors, num_colors, steps, 0, 0);
}

TINYIMG_EXPORT("tiny_multigradient_rgba_sized")
int* tiny_multigradient_rgba_sized(
    uint8_t* colors, size_t num_colors, size_t steps, float* color_distribution
) {
    if (!color_distribution) return 0;

    return multigradient_packed(
        colors, num_colors, steps, color_distribution, 1
    );
}

TINYIMG_EXPORT("tiny_multigradient_rgba")
int* tiny_multigradient_rgba(uint8_t* colors, size_t num_colors, size_t steps) {
    return multigradient_packed(colors, num_colors, steps, 0, 1);
}

/** Both cylindrical multi-gradients, which are the same walk. */
static float* multigradient_cylindrical(
    const float* colors, size_t num_colors, size_t steps,
    const float* distribution
) {
    if (!colors || num_colors < 2u) return 0;
    if (!distribution_ok(distribution, num_colors - 1u)) return 0;

    float* out = gradient_alloc(steps, 3, sizeof(float));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        size_t leg;
        float t;
        multigradient_at(num_colors, steps, i, distribution, &leg, &t);

        const float* from = colors + leg * 3u;
        const float* to = colors + (leg + 1u) * 3u;

        interpolate_cylindrical(
            from[0], from[1], from[2], to[0], to[1], to[2], t, &out[i * 3u],
            &out[i * 3u + 1u], &out[i * 3u + 2u]
        );
    }

    return out;
}

TINYIMG_EXPORT("tiny_multigradient_hsv_sized")
float* tiny_multigradient_hsv_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
) {
    if (!color_distribution) return 0;

    return multigradient_cylindrical(
        colors, num_colors, steps, color_distribution
    );
}

TINYIMG_EXPORT("tiny_multigradient_hsv")
float* tiny_multigradient_hsv(float* colors, size_t num_colors, size_t steps) {
    return multigradient_cylindrical(colors, num_colors, steps, 0);
}

TINYIMG_EXPORT("tiny_multigradient_hsl_sized")
float* tiny_multigradient_hsl_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
) {
    return tiny_multigradient_hsv_sized(
        colors, num_colors, steps, color_distribution
    );
}

TINYIMG_EXPORT("tiny_multigradient_hsl")
float* tiny_multigradient_hsl(float* colors, size_t num_colors, size_t steps) {
    return multigradient_cylindrical(colors, num_colors, steps, 0);
}

/** The one multi-gradient whose entries are four numbers wide. */
static float* multigradient_cmyk(
    const float* colors, size_t num_colors, size_t steps,
    const float* distribution
) {
    if (!colors || num_colors < 2u) return 0;
    if (!distribution_ok(distribution, num_colors - 1u)) return 0;

    float* out = gradient_alloc(steps, 4, sizeof(float));
    if (!out) return 0;

    for (size_t i = 0; i < steps; i++) {
        size_t leg;
        float t;
        multigradient_at(num_colors, steps, i, distribution, &leg, &t);

        const float* from = colors + leg * 4u;
        const float* to = colors + (leg + 1u) * 4u;

        for (uint32_t c = 0; c < 4u; c++) {
            out[i * 4u + c] = from[c] + t * (to[c] - from[c]);
        }
    }

    return out;
}

TINYIMG_EXPORT("tiny_multigradient_cmyk_sized")
float* tiny_multigradient_cmyk_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
) {
    if (!color_distribution) return 0;

    return multigradient_cmyk(colors, num_colors, steps, color_distribution);
}

TINYIMG_EXPORT("tiny_multigradient_cmyk")
float* tiny_multigradient_cmyk(float* colors, size_t num_colors, size_t steps) {
    return multigradient_cmyk(colors, num_colors, steps, 0);
}

#pragma endregion

#pragma region icc

static uint32_t icc_u32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | p[3];
}

static uint16_t icc_u16(const uint8_t* p) {
    return (uint16_t) (((uint32_t) p[0] << 8) | p[1]);
}

/** An s15Fixed16Number, which is how a profile writes every coordinate. */
static float icc_s15(const uint8_t* p) {
    return (float) (int32_t) icc_u32(p) / 65536.0f;
}

static int icc_signature_is(const uint8_t* p, const char* four) {
    for (uint32_t i = 0; i < 4u; i++) {
        if (p[i] != (uint8_t) four[i]) return 0;
    }

    return 1;
}

/**
 * @brief Finds a tag in the table.
 *
 * @param data The profile.
 * @param size How many bytes it holds.
 * @param signature The four character tag name.
 * @param offset Receives where the tag's data starts.
 * @param length Receives how long it is.
 * @return int Non-zero when found and inside the profile.
 */
static int icc_find(
    const uint8_t* data, size_t size, const char* signature, uint32_t* offset,
    uint32_t* length
) {
    if (size < 132u) return 0;

    uint32_t count = icc_u32(data + 128);
    if (count > 1024u || (size_t) 132u + (size_t) count * 12u > size) return 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = data + 132u + (size_t) i * 12u;

        if (!icc_signature_is(entry, signature)) continue;

        uint32_t at = icc_u32(entry + 4);
        uint32_t len = icc_u32(entry + 8);

        if ((size_t) at + len > size) return 0;

        *offset = at;
        *length = len;

        return 1;
    }

    return 0;
}

/**
 * @brief Reads an XYZType tag into three floats.
 *
 * @param data The profile.
 * @param size How many bytes.
 * @param signature Which tag.
 * @param out Receives X, Y and Z.
 * @return int Non-zero on success.
 */
static int icc_xyz(
    const uint8_t* data, size_t size, const char* signature, float* out
) {
    uint32_t at;
    uint32_t length;

    if (!icc_find(data, size, signature, &at, &length)) return 0;
    if (length < 20u || !icc_signature_is(data + at, "XYZ ")) return 0;

    for (uint32_t i = 0; i < 3u; i++) {
        out[i] = icc_s15(data + at + 8u + i * 4u);
    }

    return 1;
}

/**
 * @brief Reads a tone curve into a 256 entry table of linear light.
 *
 * Handles the two forms a matrix profile can carry: a sampled `curv` of any
 * length, and a `para` naming one of the five parametric functions. A `curv`
 * of a single entry is the special case where that entry is a gamma exponent
 * rather than a sample, which is easy to read as a one point table and get
 * badly wrong.
 *
 * @param data The profile.
 * @param size How many bytes.
 * @param signature Which tag.
 * @param out Receives 256 samples, 0 through 1.
 * @return int Non-zero on success.
 */
static int icc_curve(
    const uint8_t* data, size_t size, const char* signature, float* out
) {
    uint32_t at;
    uint32_t length;

    if (!icc_find(data, size, signature, &at, &length)) return 0;
    if (length < 12u) return 0;

    const uint8_t* tag = data + at;

    if (icc_signature_is(tag, "curv")) {
        uint32_t count = icc_u32(tag + 8);

        if (count == 0u) {
            for (uint32_t i = 0; i < 256u; i++) {
                out[i] = (float) i / 255.0f;
            }

            return 1;
        }

        if (count == 1u) {
            if (length < 14u) return 0;

            // a single entry is a u8Fixed8Number gamma, not a sample
            float gamma = (float) icc_u16(tag + 12) / 256.0f;
            if (gamma <= 0.0f) return 0;

            for (uint32_t i = 0; i < 256u; i++) {
                out[i] = tiny_powf((float) i / 255.0f, gamma);
            }

            return 1;
        }

        if ((size_t) length < 12u + (size_t) count * 2u) return 0;

        for (uint32_t i = 0; i < 256u; i++) {
            // the table is sampled at `count` points and read at 256, so the
            // position between two samples is interpolated rather than
            // rounded; rounding is up to half a sample of hue error on a short
            // table
            float position = (float) i / 255.0f * (float) (count - 1u);
            uint32_t low = (uint32_t) position;
            uint32_t high = low + 1u < count ? low + 1u : low;
            float t = position - (float) low;

            float a = (float) icc_u16(tag + 12u + low * 2u) / 65535.0f;
            float b = (float) icc_u16(tag + 12u + high * 2u) / 65535.0f;

            out[i] = a + t * (b - a);
        }

        return 1;
    }

    if (!icc_signature_is(tag, "para")) return 0;
    if (length < 16u) return 0;

    uint32_t kind = icc_u16(tag + 8);
    uint32_t needed[5] = {1, 3, 4, 5, 7};

    if (kind > 4u) return 0;
    if ((size_t) length < 12u + (size_t) needed[kind] * 4u) return 0;

    float p[7] = {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < needed[kind]; i++) {
        p[i] = icc_s15(tag + 12u + i * 4u);
    }

    for (uint32_t i = 0; i < 256u; i++) {
        float x = (float) i / 255.0f;
        float value;

        switch (kind) {
            case 0: value = tiny_powf(x, p[0]); break;
            case 1:
                value =
                    x >= -p[2] / p[1] ? tiny_powf(p[1] * x + p[2], p[0]) : 0.0f;
                break;
            case 2:
                value = x >= -p[2] / p[1]
                            ? tiny_powf(p[1] * x + p[2], p[0]) + p[3]
                            : p[3];
                break;
            case 3:
                value = x >= p[4] ? tiny_powf(p[1] * x + p[2], p[0]) : p[3] * x;
                break;
            default:
                value = x >= p[4] ? tiny_powf(p[1] * x + p[2], p[0]) + p[5]
                                  : p[3] * x + p[6];
                break;
        }

        out[i] = tiny_clampf(value, 0.0f, 1.0f);
    }

    return 1;
}

/** Whether a table is the identity to within a level. */
static int curve_is_linear(const float* curve) {
    for (uint32_t i = 0; i < 256u; i++) {
        float want = (float) i / 255.0f;
        float diff = curve[i] - want;

        if (diff > 0.004f || diff < -0.004f) return 0;
    }

    return 1;
}

TINYIMG_EXPORT("tiny_icc_parse")
int tiny_icc_parse(TinyIccProfile* profile, const uint8_t* data, size_t size) {
    if (!profile || !data) return TINYIMG_ERR_NULL;
    if (size < 132u) return TINYIMG_ERR_CORRUPT;

    // the header's own length field has to agree with the buffer, which is the
    // first thing a truncated profile fails
    uint32_t declared = icc_u32(data);
    if (declared > size) return TINYIMG_ERR_CORRUPT;

    uint32_t at;
    uint32_t length;

    // a profile whose transform is a lookup table is checked for before the
    // matrix tags, because some carry both and the table is the one a color
    // managed reader would use; approximating it with the matrix would be a
    // wrong answer that looks like a right one
    if (icc_find(data, size, "A2B0", &at, &length) ||
        icc_find(data, size, "A2B1", &at, &length)) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    float column[3][3];
    static const char* const TAGS[3] = {"rXYZ", "gXYZ", "bXYZ"};

    for (uint32_t c = 0; c < 3u; c++) {
        if (!icc_xyz(data, size, TAGS[c], column[c])) {
            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
        }
    }

    // the tags hold the columns, so the matrix is their transpose
    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            profile->to_xyz[row * 3u + col] = column[col][row];
        }
    }

    if (!icc_xyz(data, size, "wtpt", profile->white)) {
        profile->white[0] = 0.9642f;
        profile->white[1] = 1.0f;
        profile->white[2] = 0.8249f;
    }

    static const char* const CURVES[3] = {"rTRC", "gTRC", "bTRC"};
    profile->linear = 1u;

    for (uint32_t c = 0; c < 3u; c++) {
        if (!icc_curve(data, size, CURVES[c], profile->curve[c])) {
            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
        }

        if (!curve_is_linear(profile->curve[c])) profile->linear = 0u;
    }

    return TINYIMG_OK;
}

/** The D50-adapted sRGB matrix, which is what a tagged sRGB profile holds. */
static const float SRGB_TO_XYZ[9] = {0.4360747f, 0.3850649f, 0.1430804f,
                                     0.2225045f, 0.7168786f, 0.0606169f,
                                     0.0139322f, 0.0971045f, 0.7141733f};

TINYIMG_EXPORT("tiny_icc_srgb")
int tiny_icc_srgb(TinyIccProfile* profile) {
    if (!profile) return TINYIMG_ERR_NULL;

    for (uint32_t i = 0; i < 9u; i++) profile->to_xyz[i] = SRGB_TO_XYZ[i];

    profile->white[0] = 0.9642f;
    profile->white[1] = 1.0f;
    profile->white[2] = 0.8249f;
    profile->linear = 0u;

    for (uint32_t i = 0; i < 256u; i++) {
        float v = (float) i / 255.0f;
        float linear =
            v <= 0.04045f ? v / 12.92f : tiny_powf((v + 0.055f) / 1.055f, 2.4f);

        for (uint32_t c = 0; c < 3u; c++) profile->curve[c][i] = linear;
    }

    return TINYIMG_OK;
}

/** The inverse of a 3x3, by its adjugate. */
static int matrix3_inverse(float* out, const float* m) {
    float a = m[4] * m[8] - m[5] * m[7];
    float b = m[5] * m[6] - m[3] * m[8];
    float c = m[3] * m[7] - m[4] * m[6];
    float determinant = m[0] * a + m[1] * b + m[2] * c;

    if (determinant > -1e-9f && determinant < 1e-9f) return 0;

    out[0] = a / determinant;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / determinant;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / determinant;
    out[3] = b / determinant;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / determinant;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / determinant;
    out[6] = c / determinant;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / determinant;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / determinant;

    return 1;
}

TINYIMG_EXPORT("tiny_icc_matrix_between")
int tiny_icc_matrix_between(
    float* out, const TinyIccProfile* from, const TinyIccProfile* to
) {
    if (!out || !from || !to) return TINYIMG_ERR_NULL;

    float inverse[9];
    if (!matrix3_inverse(inverse, to->to_xyz)) return TINYIMG_ERR_RANGE;

    // both matrices already land in D50, because parse folded each profile's
    // own adaptation in, so going through the connection space needs no
    // further adaptation here
    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            float sum = 0.0f;

            for (uint32_t k = 0; k < 3u; k++) {
                sum += inverse[row * 3u + k] * from->to_xyz[k * 3u + col];
            }

            out[row * 3u + col] = sum;
        }
    }

    return TINYIMG_OK;
}

/**
 * @brief The sRGB encoding of a linear value.
 *
 * @param value Linear light, 0 through 1.
 * @return uint8_t The encoded channel.
 */
static uint8_t srgb_encode(float value) {
    value = tiny_clampf(value, 0.0f, 1.0f);

    float encoded = value <= 0.0031308f
                        ? value * 12.92f
                        : 1.055f * tiny_powf(value, 1.0f / 2.4f) - 0.055f;

    return tiny_clamp_u8f(encoded * 255.0f);
}

TINYIMG_EXPORT("tiny_icc_to_srgb")
int tiny_icc_to_srgb(
    const TinyIccProfile* profile, const uint8_t* in, uint8_t* out
) {
    if (!profile || !in || !out) return TINYIMG_ERR_NULL;

    float xyz[3] = {0.0f, 0.0f, 0.0f};

    for (uint32_t c = 0; c < 3u; c++) {
        float linear = profile->curve[c][in[c]];

        for (uint32_t row = 0; row < 3u; row++) {
            xyz[row] += profile->to_xyz[row * 3u + c] * linear;
        }
    }

    float inverse[9];
    if (!matrix3_inverse(inverse, SRGB_TO_XYZ)) return TINYIMG_ERR_RANGE;

    for (uint32_t row = 0; row < 3u; row++) {
        float sum = 0.0f;

        for (uint32_t k = 0; k < 3u; k++) {
            sum += inverse[row * 3u + k] * xyz[k];
        }

        out[row] = srgb_encode(sum);
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_icc_convert_image")
int tiny_icc_convert_image(TinyImage* image, const TinyIccProfile* profile) {
    if (!image || !image->data || !profile) return TINYIMG_ERR_NULL;
    if (image->channels < 3u) return TINYIMG_ERR_NO_CHANNEL;

    // the whole conversion is a function of one pixel, so it is a table of
    // 256 entries per channel composed with a matrix, and the matrix is what
    // the loop actually spends its time on
    float inverse[9];
    if (!matrix3_inverse(inverse, SRGB_TO_XYZ)) return TINYIMG_ERR_RANGE;

    float combined[9];

    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            float sum = 0.0f;

            for (uint32_t k = 0; k < 3u; k++) {
                sum += inverse[row * 3u + k] * profile->to_xyz[k * 3u + col];
            }

            combined[row * 3u + col] = sum;
        }
    }

    uint8_t encode[4096];
    for (uint32_t i = 0; i < 4096u; i++) {
        encode[i] = srgb_encode((float) i / 4095.0f);
    }

    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = image->data + i * image->channels;
        float linear[3];

        for (uint32_t c = 0; c < 3u; c++) linear[c] = profile->curve[c][p[c]];

        for (uint32_t row = 0; row < 3u; row++) {
            float sum = combined[row * 3u] * linear[0] +
                        combined[row * 3u + 1u] * linear[1] +
                        combined[row * 3u + 2u] * linear[2];

            int32_t index =
                (int32_t) (tiny_clampf(sum, 0.0f, 1.0f) * 4095.0f + 0.5f);

            p[row] = encode[tiny_clampi(index, 0, 4095)];
        }
    }

    return TINYIMG_OK;
}

#pragma endregion
