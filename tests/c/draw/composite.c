#include <math.h>

#include "test.h"

/**
 * @brief One channel through a blend mode, in floating point.
 *
 * Written from the CSS compositing specification rather than from draw.c, so
 * the numbers the tests compare against are the specification's answer and not
 * this library's answer repeated back.
 *
 * @param mode Which mode.
 * @param b The backdrop channel, 0 through 1.
 * @param s The source channel, 0 through 1.
 * @return double The blended channel.
 */
static double blend(TinyBlendMode mode, double b, double s) {
    switch (mode) {
        case TINYIMG_BLEND_MULTIPLY: return b * s;
        case TINYIMG_BLEND_SCREEN: return b + s - b * s;
        case TINYIMG_BLEND_OVERLAY:
            return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
        case TINYIMG_BLEND_HARD_LIGHT:
            return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
        case TINYIMG_BLEND_SOFT_LIGHT: {
            double d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
            return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b)
                            : b + (2.0 * s - 1.0) * (d - b);
        }
        case TINYIMG_BLEND_DARKEN: return b < s ? b : s;
        case TINYIMG_BLEND_LIGHTEN: return b > s ? b : s;
        case TINYIMG_BLEND_DIFFERENCE: return fabs(b - s);
        case TINYIMG_BLEND_EXCLUSION: return b + s - 2.0 * b * s;
        case TINYIMG_BLEND_ADD: return b + s > 1.0 ? 1.0 : b + s;
        case TINYIMG_BLEND_SUBTRACT: return b - s < 0.0 ? 0.0 : b - s;
        default: return s;
    }
}

/**
 * @brief What one pixel over another should come to.
 *
 * The specification's Co and ao, unpremultiplied at the end because that is
 * what an image stores.
 *
 * @param mode Which mode.
 * @param cb Backdrop channel, 0 through 255.
 * @param ab Backdrop alpha.
 * @param cs Source channel.
 * @param as Source alpha.
 * @param out_alpha Receives the composited alpha, 0 through 255.
 * @return double The composited channel, 0 through 255.
 */
static double expected(
    TinyBlendMode mode, double cb, double ab, double cs, double as,
    double* out_alpha
) {
    double b = cb / 255.0;
    double s = cs / 255.0;
    double alpha_b = ab / 255.0;
    double alpha_s = as / 255.0;
    double alpha_o = alpha_s + alpha_b * (1.0 - alpha_s);

    *out_alpha = alpha_o * 255.0;

    if (alpha_o <= 0.0) return 0.0;

    double premultiplied = alpha_s * (1.0 - alpha_b) * s +
                           alpha_s * alpha_b * blend(mode, b, s) +
                           (1.0 - alpha_s) * alpha_b * b;

    return premultiplied / alpha_o * 255.0;
}

static const TinyBlendMode MODES[12] = {
    TINYIMG_BLEND_NORMAL,     TINYIMG_BLEND_MULTIPLY,  TINYIMG_BLEND_SCREEN,
    TINYIMG_BLEND_OVERLAY,    TINYIMG_BLEND_DARKEN,    TINYIMG_BLEND_LIGHTEN,
    TINYIMG_BLEND_DIFFERENCE, TINYIMG_BLEND_EXCLUSION, TINYIMG_BLEND_HARD_LIGHT,
    TINYIMG_BLEND_SOFT_LIGHT, TINYIMG_BLEND_ADD,       TINYIMG_BLEND_SUBTRACT
};

/** One RGBA pixel over another, for every mode and a spread of alphas. */
static int against_the_specification(void) {
    int failures = 0;
    static const uint8_t LEVEL[5] = {0, 64, 128, 200, 255};

    for (uint32_t m = 0; m < 12u; m++) {
        double worst = 0.0;

        for (uint32_t bi = 0; bi < 5u; bi++) {
            for (uint32_t si = 0; si < 5u; si++) {
                for (uint32_t ai = 0; ai < 5u; ai++) {
                    for (uint32_t bj = 0; bj < 5u; bj++) {
                        TinyImage back;
                        TinyImage front;

                        if (tiny_image_create(&back, 1, 1, 4) != TINYIMG_OK) {
                            return failures + 1;
                        }
                        if (tiny_image_create(&front, 1, 1, 4) != TINYIMG_OK) {
                            tiny_image_destroy(&back);
                            return failures + 1;
                        }

                        back.data[0] = LEVEL[bi];
                        back.data[1] = LEVEL[bi];
                        back.data[2] = LEVEL[bi];
                        back.data[3] = LEVEL[bj];

                        front.data[0] = LEVEL[si];
                        front.data[1] = LEVEL[si];
                        front.data[2] = LEVEL[si];
                        front.data[3] = LEVEL[ai];

                        tiny_image_composite(&back, &front, MODES[m]);

                        double want_alpha;
                        double want = expected(
                            MODES[m], LEVEL[bi], LEVEL[bj], LEVEL[si],
                            LEVEL[ai], &want_alpha
                        );

                        double alpha_error =
                            fabs((double) back.data[3] - want_alpha);
                        if (alpha_error > worst) worst = alpha_error;

                        // the color of a pixel that ends fully transparent is
                        // not observable and the specification's Co/ao is not
                        // defined there, so only the alpha is asserted
                        if (want_alpha >= 0.5) {
                            double color_error =
                                fabs((double) back.data[0] - want);
                            if (color_error > worst) worst = color_error;
                        }

                        tiny_image_destroy(&back);
                        tiny_image_destroy(&front);
                    }
                }
            }
        }

        // the whole thing runs in integers, and one rounded level is what
        // that costs: the worst case measured across every mode and every
        // alpha pair here is 0.84. a bound of one level is therefore a real
        // constraint rather than a wide net, and a different formula fails it
        failures += assertLessThan(worst, 1.0);
    }

    return failures;
}

/** Compositing a clear layer changes nothing, and an opaque one replaces. */
static int the_two_extremes(void) {
    int failures = 0;
    TinyImage back;
    TinyImage front;

    if (tiny_image_create(&back, 4, 4, 4) != TINYIMG_OK) return 1;
    if (tiny_image_create(&front, 4, 4, 4) != TINYIMG_OK) {
        tiny_image_destroy(&back);
        return 1;
    }

    for (uint32_t i = 0; i < 16u; i++) {
        back.data[i * 4u] = (uint8_t) (i * 13u);
        back.data[i * 4u + 1u] = (uint8_t) (i * 7u);
        back.data[i * 4u + 2u] = (uint8_t) (i * 3u);
        back.data[i * 4u + 3u] = 255u;
    }

    TinyImage before;
    tiny_image_create(&before, 4, 4, 4);
    memcpy(before.data, back.data, 16u * 4u);

    // a source with zero alpha, whatever its color
    for (uint32_t i = 0; i < 16u; i++) {
        front.data[i * 4u] = 255u;
        front.data[i * 4u + 1u] = 255u;
        front.data[i * 4u + 2u] = 255u;
        front.data[i * 4u + 3u] = 0u;
    }

    failures += assertEquals(
        tiny_image_composite(&back, &front, TINYIMG_BLEND_MULTIPLY), 0
    );
    failures += assertImageEquals(&back, &before);

    // an opaque source in the default mode replaces
    for (uint32_t i = 0; i < 16u; i++) front.data[i * 4u + 3u] = 255u;

    failures += assertEquals(
        tiny_image_composite(&back, &front, TINYIMG_BLEND_NORMAL), 0
    );
    failures += assertImageEquals(&back, &front);

    tiny_image_destroy(&before);
    tiny_image_destroy(&back);
    tiny_image_destroy(&front);
    return failures;
}

/** Premultiply and unpremultiply are inverses within the rounding. */
static int premultiplication(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 256, 256, 4) != TINYIMG_OK) return 1;

    for (uint32_t a = 0; a < 256u; a++) {
        for (uint32_t v = 0; v < 256u; v++) {
            uint8_t* p = image.data + ((size_t) a * 256u + v) * 4u;

            // only a channel at or below its own alpha can survive the round
            // trip, because a premultiplied channel above its alpha is not a
            // value any premultiplied image can hold
            p[0] = (uint8_t) (v * a / 255u);
            p[1] = p[0];
            p[2] = p[0];
            p[3] = (uint8_t) a;
        }
    }

    TinyImage before;
    tiny_image_create(&before, 256, 256, 4);
    memcpy(before.data, image.data, 256u * 256u * 4u);

    failures += assertEquals(tiny_image_premultiply(&image), 0);
    failures += assertEquals(tiny_image_unpremultiply(&image), 0);

    uint32_t worst = 0;

    for (uint32_t i = 0; i < 256u * 256u; i++) {
        uint32_t alpha = before.data[i * 4u + 3u];
        if (alpha == 0u) continue;

        int32_t diff =
            (int32_t) image.data[i * 4u] - (int32_t) before.data[i * 4u];
        uint32_t error = (uint32_t) (diff < 0 ? -diff : diff);

        // the error a division by alpha can leave is one level at full alpha
        // and grows as alpha falls, which is why the bound scales
        uint32_t budget = 1u + 255u / (alpha > 8u ? alpha : 8u);
        if (error > budget) worst = error > worst ? error : worst;
    }

    failures += assertEquals(worst, 0);

    // a fully transparent pixel has no color to recover
    for (uint32_t v = 0; v < 256u; v++) {
        failures += assertEquals(image.data[v * 4u], 0);
    }

    // an image with no alpha channel is left alone by both
    TinyImage opaque;
    tiny_image_create(&opaque, 4, 4, 3);
    for (uint32_t i = 0; i < 48u; i++) opaque.data[i] = (uint8_t) (i * 5u);

    TinyImage kept;
    tiny_image_create(&kept, 4, 4, 3);
    memcpy(kept.data, opaque.data, 48u);

    failures += assertEquals(tiny_image_premultiply(&opaque), 0);
    failures += assertImageEquals(&opaque, &kept);
    failures += assertEquals(tiny_image_unpremultiply(&opaque), 0);
    failures += assertImageEquals(&opaque, &kept);

    tiny_image_destroy(&opaque);
    tiny_image_destroy(&kept);
    tiny_image_destroy(&before);
    tiny_image_destroy(&image);
    return failures;
}

/** Drawing honors the source's alpha, its opacity and its placement. */
static int drawing(void) {
    int failures = 0;
    TinyImage back;
    TinyImage front;

    if (tiny_image_create(&back, 10, 10, 3) != TINYIMG_OK) return 1;
    if (tiny_image_create(&front, 4, 4, 4) != TINYIMG_OK) {
        tiny_image_destroy(&back);
        return 1;
    }

    for (uint32_t i = 0; i < 16u; i++) {
        front.data[i * 4u] = 255u;
        front.data[i * 4u + 1u] = 0u;
        front.data[i * 4u + 2u] = 0u;
        front.data[i * 4u + 3u] = 128u;
    }

    failures += assertEquals(tiny_image_draw_image(&back, &front, 2, 3), 0);

    // half alpha over black is half red, whatever the destination's channel
    // count is
    const uint8_t* hit = back.data + ((size_t) 3 * 10 + 2) * 3u;
    failures += assertIn((double) hit[0], 126.0, 130.0);
    failures += assertEquals(hit[1], 0);

    const uint8_t* miss = back.data + ((size_t) 2 * 10 + 2) * 3u;
    failures += assertEquals(miss[0], 0);

    // an opacity of zero draws nothing at all
    TinyImage kept;
    tiny_image_create(&kept, 10, 10, 3);
    memcpy(kept.data, back.data, 300u);

    failures += assertEquals(
        tiny_image_draw_image_ex(
            &back, &front, 0, 0, 0.0f, TINYIMG_DRAW_ONCE, TINYIMG_BLEND_NORMAL
        ),
        0
    );
    failures += assertImageEquals(&back, &kept);

    failures += assertEquals(
        tiny_image_draw_image_ex(
            &back, &front, 0, 0, 1.5f, TINYIMG_DRAW_ONCE, TINYIMG_BLEND_NORMAL
        ),
        TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&kept);
    tiny_image_destroy(&back);
    tiny_image_destroy(&front);
    return failures;
}

/** Tiling covers every pixel, and centering puts the source in the middle. */
static int placement(void) {
    int failures = 0;
    TinyImage back;
    TinyImage tile;

    if (tiny_image_create(&back, 21, 13, 3) != TINYIMG_OK) return 1;
    if (tiny_image_create(&tile, 4, 4, 3) != TINYIMG_OK) {
        tiny_image_destroy(&back);
        return 1;
    }

    for (uint32_t i = 0; i < 48u; i++) tile.data[i] = 200u;

    failures += assertEquals(
        tiny_image_draw_image_ex(
            &back, &tile, 5, 2, 1.0f, TINYIMG_DRAW_TILE, TINYIMG_BLEND_NORMAL
        ),
        0
    );

    // an extent that is not a multiple of the tile is the case a loop that
    // starts at the offset rather than before it leaves a strip of
    uint32_t blank = 0;
    for (uint32_t i = 0; i < 21u * 13u; i++) {
        if (back.data[i * 3u] != 200u) blank++;
    }

    failures += assertEquals(blank, 0);

    tiny_image_destroy(&back);
    if (tiny_image_create(&back, 20, 20, 3) != TINYIMG_OK) {
        tiny_image_destroy(&tile);
        return failures + 1;
    }

    failures += assertEquals(
        tiny_image_draw_image_ex(
            &back, &tile, 99, 99, 1.0f, TINYIMG_DRAW_CENTER,
            TINYIMG_BLEND_NORMAL
        ),
        0
    );

    // centered ignores the offset it was given
    failures += assertEquals(back.data[((size_t) 9 * 20 + 9) * 3u], 200);
    failures += assertEquals(back.data[((size_t) 2 * 20 + 2) * 3u], 0);

    tiny_image_destroy(&back);
    tiny_image_destroy(&tile);
    return failures;
}

/** Composite refuses a mismatched extent, and both refuse nulls. */
static int nulls(void) {
    int failures = 0;
    TinyImage a;
    TinyImage b;

    if (tiny_image_create(&a, 4, 4, 4) != TINYIMG_OK) return 1;
    if (tiny_image_create(&b, 5, 4, 4) != TINYIMG_OK) {
        tiny_image_destroy(&a);
        return 1;
    }

    failures += assertEquals(
        tiny_image_composite(&a, &b, TINYIMG_BLEND_NORMAL), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_composite(0, &b, TINYIMG_BLEND_NORMAL), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_composite(&a, 0, TINYIMG_BLEND_NORMAL), TINYIMG_ERR_NULL
    );
    failures +=
        assertEquals(tiny_image_draw_image(&a, 0, 0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_premultiply(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_unpremultiply(0), TINYIMG_ERR_NULL);

    tiny_image_destroy(&a);
    tiny_image_destroy(&b);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += against_the_specification();
    failures += the_two_extremes();
    failures += premultiplication();
    failures += drawing();
    failures += placement();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
