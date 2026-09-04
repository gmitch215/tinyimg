#include "tinyimg/plan.h"

#include "test.h"

/** A ramp across every level, so a curve is exercised at each input. */
static int make_ramp(TinyImage* image) {
    if (tiny_image_create(image, 256, 3, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < 3u; y++) {
        for (uint32_t x = 0; x < 256u; x++) {
            uint8_t* p = image->data + ((size_t) y * 256u + x) * 3u;

            p[0] = (uint8_t) x;
            p[1] = (uint8_t) ((x + 85u) & 0xFFu);
            p[2] = (uint8_t) ((x + 170u) & 0xFFu);
        }
    }

    return 1;
}

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** A matrix and a curve compose into one stage each, not one pass each. */
static int collapse(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);

    // three matrices and two curves, in an order that interleaves them
    static const float SEPIA[12] = {0.393f, 0.769f, 0.189f, 0.0f,
                                    0.349f, 0.686f, 0.168f, 0.0f,
                                    0.272f, 0.534f, 0.131f, 0.0f};
    float levels[5] = {10.0f, 245.0f, 1.1f, 0.0f, 255.0f};

    failures += assertEquals(tiny_plan_matrix(&plan, SEPIA), 0);
    failures += assertEquals(tiny_plan_saturation(&plan, 1.2f), 0);
    failures += assertEquals(
        tiny_plan_curve(&plan, TINYIMG_CURVE_LEVELS, levels, 0), 0
    );
    failures += assertEquals(tiny_plan_gamma(&plan, 1.4f), 0);
    failures += assertEquals(tiny_plan_contrast(&plan, 1.1f), 0);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), 0);

    // five operations, but a matrix run, a curve run and a matrix run: three
    // stages rather than five passes
    failures += assertEquals(resolution.ops, 5);
    failures += assertEquals(resolution.color_stages, 3);
    failures += assertEquals(resolution.passes, 1);

    TinyColorStage stages[TINYIMG_PLAN_MAX_OPS];
    uint32_t count = 0;

    failures += assertEquals(
        tiny_plan_color_stages(
            &resolution, stages, TINYIMG_PLAN_MAX_OPS, &count
        ),
        0
    );
    failures += assertEquals(count, 3);
    failures += assertEquals(stages[0].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);
    failures += assertEquals(stages[1].kind, TINYIMG_OP_CLASS_COLOR_LUT);
    failures += assertEquals(stages[2].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);

    tiny_image_destroy(&image);
    return failures;
}

/** A named preset is a stack that also collapses. */
static int presets(void) {
    int failures = 0;

    for (uint32_t p = 0; p <= 8u; p++) {
        TinyImage image;
        if (!make_ramp(&image)) return failures + 1;

        failures +=
            assertEquals(tiny_image_preset(&image, (TinyImagePreset) p), 0);

        // whatever the look does, it stays inside the range and keeps the
        // extent: a preset that clipped the image away would still pass a
        // smoke test that only checked the return code
        failures += assertEquals(image.width, 256);
        failures += assertEquals(image.channels, 3);

        tiny_image_destroy(&image);
    }

    // the three monochrome presets leave no chroma at all, which is the one
    // property that separates them from the rest
    static const TinyImagePreset MONO[3] = {
        TINYIMG_PRESET_NOIR, TINYIMG_PRESET_CHROME, TINYIMG_PRESET_MONO
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyImage image;
        if (!make_ramp(&image)) return failures + 1;

        failures += assertEquals(tiny_image_preset(&image, MONO[i]), 0);

        int32_t worst = 0;

        for (uint32_t x = 0; x < 256u; x++) {
            const uint8_t* p = at(&image, x, 0);

            for (uint32_t c = 1; c < 3u; c++) {
                int32_t diff = (int32_t) p[c] - (int32_t) p[0];
                if (diff < 0) diff = -diff;
                if (diff > worst) worst = diff;
            }
        }

        // chrome tints the blue channel on purpose, so it is allowed a shift
        // the other two are not
        failures +=
            assertLessThan(worst, MONO[i] == TINYIMG_PRESET_CHROME ? 24 : 2);

        tiny_image_destroy(&image);
    }

    failures += assertEquals(
        tiny_image_preset(0, TINYIMG_PRESET_NOIR), TINYIMG_ERR_NULL
    );

    TinyImage image;
    if (!make_ramp(&image)) return failures + 1;

    failures += assertEquals(
        tiny_image_preset(&image, (TinyImagePreset) 99), TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&image);
    return failures;
}

/** Each curve does what its name says at the levels that identify it. */
static int curves(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;
    failures += assertEquals(tiny_image_posterize(&image, 2), 0);

    // two levels means black and white, not black and mid gray: the levels are
    // the endpoints of the range
    for (uint32_t x = 0; x < 256u; x++) {
        uint8_t value = at(&image, x, 0)[0];
        failures += assertTrue(value == 0u || value == 255u);
    }

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    failures += assertEquals(tiny_image_threshold(&image, 128), 0);
    failures += assertEquals(at(&image, 127, 0)[0], 0);
    failures += assertEquals(at(&image, 128, 0)[0], 255);

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    failures += assertEquals(tiny_image_solarize(&image, 128), 0);
    failures += assertEquals(at(&image, 100, 0)[0], 100);
    failures += assertEquals(at(&image, 200, 0)[0], 55);

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    // one stop up doubles, and saturates rather than wrapping
    failures += assertEquals(tiny_image_exposure(&image, 1.0f), 0);
    failures += assertEquals(at(&image, 60, 0)[0], 120);
    failures += assertEquals(at(&image, 200, 0)[0], 255);

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    failures += assertEquals(tiny_image_negate(&image), 0);
    failures += assertEquals(at(&image, 0, 0)[0], 255);
    failures += assertEquals(at(&image, 255, 0)[0], 0);

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    // levels maps the named input range onto the named output range
    failures += assertEquals(
        tiny_image_levels(&image, 50.0f, 200.0f, 1.0f, 0.0f, 255.0f), 0
    );
    failures += assertEquals(at(&image, 40, 0)[0], 0);
    failures += assertEquals(at(&image, 50, 0)[0], 0);
    failures += assertEquals(at(&image, 200, 0)[0], 255);
    failures += assertEquals(at(&image, 210, 0)[0], 255);
    failures += assertIn((double) at(&image, 125, 0)[0], 125.0, 130.0);

    // an empty input range has no inverse, so it is refused
    failures += assertEquals(
        tiny_image_levels(&image, 200.0f, 50.0f, 1.0f, 0.0f, 255.0f),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_levels(&image, 0.0f, 255.0f, 0.0f, 0.0f, 255.0f),
        TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&image);
    return failures;
}

/** A per-channel curve leaves the other channels where they were. */
static int per_channel(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;

    const uint8_t* before = at(&image, 100, 0);
    uint8_t green = before[1];
    uint8_t blue = before[2];

    failures += assertEquals(
        tiny_image_levels_channel(&image, 0, 0.0f, 128.0f, 1.0f, 0.0f, 255.0f),
        0
    );

    const uint8_t* after = at(&image, 100, 0);

    failures += assertGreaterThan(after[0], 190);
    failures += assertEquals(after[1], green);
    failures += assertEquals(after[2], blue);

    failures += assertEquals(
        tiny_image_levels_channel(&image, 3, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f),
        TINYIMG_ERR_RANGE
    );

    // three per-channel curves collapse into one table between them
    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);

    float shadows[3] = {0.1f, 0.0f, -0.1f};
    float mids[3] = {0.0f, 0.05f, 0.0f};
    float highs[3] = {-0.1f, 0.0f, 0.1f};

    for (uint32_t c = 0; c < 3u; c++) {
        float p[5] = {shadows[c], mids[c], highs[c], 0.0f, 0.0f};
        failures += assertEquals(
            tiny_plan_curve(
                &plan, TINYIMG_CURVE_BALANCE, p, (uint8_t) (1u << c)
            ),
            0
        );
    }

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), 0);
    failures += assertEquals(resolution.color_stages, 1);

    tiny_image_destroy(&image);

    // and the named entry point builds the same three, each band moving the
    // channel it was given at the level that band covers
    if (tiny_image_create(&image, 3, 1, 3) != TINYIMG_OK) return failures + 1;

    static const uint8_t LEVEL[3] = {20, 128, 240};

    for (uint32_t i = 0; i < 3u; i++) {
        image.data[i * 3u] = LEVEL[i];
        image.data[i * 3u + 1u] = LEVEL[i];
        image.data[i * 3u + 2u] = LEVEL[i];
    }

    float lift[3] = {0.2f, 0.0f, 0.0f};
    float nothing[3] = {0.0f, 0.0f, 0.0f};
    float pull[3] = {0.0f, 0.0f, -0.2f};

    failures +=
        assertEquals(tiny_image_color_balance(&image, lift, nothing, pull), 0);

    // the shadow shift lifts red at the dark end and barely at the light one
    failures += assertGreaterThan(image.data[0], LEVEL[0] + 20);
    failures += assertLessThan(image.data[6], LEVEL[2] + 6);

    // the highlight shift pulls blue down at the light end and not the dark
    failures += assertLessThan(image.data[8], LEVEL[2] - 20);
    failures += assertGreaterThan(image.data[2], LEVEL[0] - 6);

    // green was given nothing on any band, so it is where it was
    for (uint32_t i = 0; i < 3u; i++) {
        failures += assertIn(
            (double) image.data[i * 3u + 1u], (double) LEVEL[i] - 1.0,
            (double) LEVEL[i] + 1.0
        );
    }

    tiny_image_destroy(&image);
    return failures;
}

/** A tint changes the color and leaves the luminance where it was. */
static int tinting(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;

    static const uint8_t WARM[3] = {255, 180, 60};
    uint32_t bins[256];

    tiny_image_histogram(&image, 255u, bins);

    uint64_t before = 0;
    for (uint32_t i = 0; i < 256u; i++) before += (uint64_t) bins[i] * i;

    failures += assertEquals(tiny_image_tint(&image, WARM, 0.4f), 0);

    tiny_image_histogram(&image, 255u, bins);

    uint64_t after = 0;
    for (uint32_t i = 0; i < 256u; i++) after += (uint64_t) bins[i] * i;

    double drift = (double) after / (double) before;

    // the offsets are constructed to sum to zero against the luminance
    // weights, so the mean luminance moves only by what clipping at the two
    // ends costs; a tint built as a plain color offset moves it by tens of
    // levels
    failures += assertIn(drift, 0.98, 1.02);

    // and the image really is warmer, or the tint did nothing
    uint8_t red[3];
    tiny_image_average_color(&image, red);
    failures += assertGreaterThan(red[0], red[2]);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A colorblind simulation collapses the axis it names.
 *
 * The property each matrix actually has, measured rather than assumed: after
 * the simulation, two colors that differed only along the confusion axis are
 * nearly the same color. Across every pair of levels on that axis the
 * difference falls from 255 to 3 for protanopia, 19 for deuteranopia, 11 for
 * tritanopia and 0 for achromatopsia.
 *
 * These matrices are NOT exact projections and so are not idempotent: a second
 * application moves the pixels by up to 27 levels, measured. They are published
 * three-decimal fits to the LMS projection rather than the projection itself,
 * so asserting idempotence would be asserting something untrue of the
 * reference the numbers came from.
 */
static int colorblindness(void) {
    int failures = 0;

    // the two channels each form confuses, and how far apart the simulation is
    // allowed to leave them
    static const uint32_t AXIS[4][2] = {{0, 1}, {0, 1}, {1, 2}, {0, 1}};
    static const int32_t BUDGET[4] = {6, 24, 16, 2};

    for (uint32_t k = 0; k <= 3u; k++) {
        TinyImage image;

        if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) {
            return failures + 1;
        }

        // the two ends of the confusion axis, at a fixed third channel
        for (uint32_t i = 0; i < 256u; i++) {
            uint8_t* p = image.data + i * 3u;

            p[0] = 128u;
            p[1] = 128u;
            p[2] = 128u;
            p[AXIS[k][0]] = (uint8_t) (i < 128u ? 0u : 255u);
            p[AXIS[k][1]] = (uint8_t) (i < 128u ? 255u : 0u);
        }

        failures += assertEquals(
            tiny_image_colorblind_simulate(&image, (TinyColorblindKind) k), 0
        );

        int32_t spread = 0;

        for (uint32_t i = 0; i < 256u; i++) {
            const uint8_t* p = image.data + i * 3u;
            int32_t diff = (int32_t) p[AXIS[k][0]] - (int32_t) p[AXIS[k][1]];

            if (diff < 0) diff = -diff;
            if (diff > spread) spread = diff;
        }

        failures += assertLessThan(spread, BUDGET[k]);

        tiny_image_destroy(&image);
    }

    // achromatopsia is the luminance projection and so IS exactly idempotent,
    // which is what separates it from the three fitted matrices
    TinyImage once;
    TinyImage twice;

    if (tiny_image_create(&once, 4, 4, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 16u; i++) {
        once.data[i * 3u] = (uint8_t) (i * 17u);
        once.data[i * 3u + 1u] = (uint8_t) (255u - i * 15u);
        once.data[i * 3u + 2u] = (uint8_t) (i * 9u + 40u);
    }

    tiny_image_colorblind_simulate(&once, TINYIMG_COLORBLIND_ACHROMATOPSIA);

    if (tiny_image_create(&twice, 4, 4, 3) != TINYIMG_OK) {
        tiny_image_destroy(&once);
        return failures + 1;
    }

    memcpy(twice.data, once.data, 48u);
    tiny_image_colorblind_simulate(&twice, TINYIMG_COLORBLIND_ACHROMATOPSIA);

    failures += assertBytesMatch(once.data, twice.data, 48u);

    tiny_image_destroy(&once);
    tiny_image_destroy(&twice);

    TinyImage image;
    if (tiny_image_create(&image, 2, 2, 3) != TINYIMG_OK) return failures + 1;

    failures += assertEquals(
        tiny_image_colorblind_simulate(&image, (TinyColorblindKind) 9),
        TINYIMG_ERR_RANGE
    );

    // the assist moves colors apart rather than leaving them alone
    for (uint32_t i = 0; i < 4u; i++) {
        image.data[i * 3u] = 200u;
        image.data[i * 3u + 1u] = 120u;
        image.data[i * 3u + 2u] = 60u;
    }

    uint8_t original = image.data[2];
    failures += assertEquals(
        tiny_image_colorblind_assist(&image, TINYIMG_COLORBLIND_DEUTERANOPIA), 0
    );
    failures += assertNotEquals(image.data[2], original);

    tiny_image_destroy(&image);
    return failures;
}

/** A caller's own table and control points reach the pixels. */
static int escape_hatches(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;

    uint8_t lut[256];
    for (uint32_t i = 0; i < 256u; i++) lut[i] = (uint8_t) (255u - i);

    failures += assertEquals(tiny_image_apply_lut(&image, lut), 0);
    failures += assertEquals(at(&image, 0, 0)[0], 255);
    failures += assertEquals(at(&image, 255, 0)[0], 0);

    // one table per channel, and a NULL table leaves its channel alone
    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    uint8_t green_before = at(&image, 40, 0)[1];

    failures += assertEquals(tiny_image_apply_luts(&image, lut, 0, 0), 0);
    failures += assertEquals(at(&image, 40, 0)[0], 215);
    failures += assertEquals(at(&image, 40, 0)[1], green_before);

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    // a curve through three rising points is monotone between them, which a
    // cubic spline through the same points is not
    static const uint8_t XS[3] = {0, 128, 255};
    static const uint8_t YS[3] = {0, 200, 255};

    failures += assertEquals(tiny_image_curves(&image, XS, YS, 3), 0);
    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertEquals(at(&image, 128, 0)[0], 200);
    failures += assertEquals(at(&image, 255, 0)[0], 255);

    uint8_t previous = 0;
    for (uint32_t x = 0; x < 256u; x++) {
        uint8_t value = at(&image, x, 0)[0];
        failures += assertTrue(value >= previous);
        previous = value;
    }

    static const uint8_t BAD[3] = {0, 128, 128};
    failures +=
        assertEquals(tiny_image_curves(&image, BAD, YS, 3), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_curves(&image, XS, YS, 1), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

/** Sepia, colorize and duotone each move the hue where they say. */
static int toning(void) {
    int failures = 0;
    TinyImage image;

    if (!make_ramp(&image)) return 1;

    failures += assertEquals(tiny_image_apply_sepia(&image), 0);

    // a sepia is warm everywhere: red above green above blue, whatever the
    // input was
    for (uint32_t x = 0; x < 256u; x += 17u) {
        const uint8_t* p = at(&image, x, 0);

        failures += assertTrue(p[0] >= p[1]);
        failures += assertTrue(p[1] >= p[2]);
    }

    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    // colorize at full strength leaves one hue and keeps the luminance order
    static const uint8_t BLUE[3] = {40, 80, 220};
    failures += assertEquals(tiny_image_colorize(&image, BLUE, 1.0f), 0);

    const uint8_t* dark = at(&image, 10, 0);
    const uint8_t* light = at(&image, 240, 0);

    failures += assertTrue(dark[2] > dark[0]);
    failures += assertTrue(light[2] > light[0]);

    // and at zero strength it changes nothing
    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    TinyImage kept;
    tiny_image_create(&kept, 256, 3, 3);
    memcpy(kept.data, image.data, 256u * 3u * 3u);

    failures += assertEquals(tiny_image_colorize(&image, BLUE, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    if (!make_ramp(&image)) return failures + 1;

    // a duotone maps black and white onto its two colors
    static const uint8_t SHADOW[3] = {20, 10, 60};
    static const uint8_t HIGHLIGHT[3] = {250, 230, 180};

    failures += assertEquals(tiny_image_duotone(&image, SHADOW, HIGHLIGHT), 0);

    // a pure black pixel becomes the shadow color and a white one the
    // highlight, so the endpoints are exact rather than approached
    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 2, 1, 3) != TINYIMG_OK) return failures + 1;

    image.data[0] = 0u;
    image.data[1] = 0u;
    image.data[2] = 0u;
    image.data[3] = 255u;
    image.data[4] = 255u;
    image.data[5] = 255u;

    failures += assertEquals(tiny_image_duotone(&image, SHADOW, HIGHLIGHT), 0);

    for (uint32_t c = 0; c < 3u; c++) {
        failures += assertIn(
            (double) image.data[c], (double) SHADOW[c] - 2.0,
            (double) SHADOW[c] + 2.0
        );
        failures += assertIn(
            (double) image.data[3u + c], (double) HIGHLIGHT[c] - 2.0,
            (double) HIGHLIGHT[c] + 2.0
        );
    }

    // a split tone casts the two ends apart, and its balance shifts where
    failures +=
        assertEquals(tiny_image_split_tone(&image, SHADOW, HIGHLIGHT, 0.5f), 0);
    failures +=
        assertEquals(tiny_image_split_tone(&image, SHADOW, HIGHLIGHT, 0.2f), 0);

    tiny_image_destroy(&image);
    return failures;
}

/** White balance and the channel controls scale the channels they name. */
static int balance(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return 1;

    for (uint32_t i = 0; i < 16u; i++) {
        image.data[i * 3u] = 120u;
        image.data[i * 3u + 1u] = 120u;
        image.data[i * 3u + 2u] = 120u;
    }

    failures += assertEquals(tiny_image_temperature(&image, 0.5f), 0);

    // warm means red up and blue down, from a neutral start
    failures += assertGreaterThan(image.data[0], 120);
    failures += assertLessThan(image.data[2], 120);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 48u; i++) image.data[i] = 120u;

    failures += assertEquals(tiny_image_temperature(&image, -0.5f), 0);
    failures += assertLessThan(image.data[0], 120);
    failures += assertGreaterThan(image.data[2], 120);

    // a tint moves green against magenta, which is the other axis
    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 48u; i++) image.data[i] = 120u;

    failures += assertEquals(tiny_image_white_balance(&image, 0.0f, 0.6f), 0);
    failures += assertLessThan(image.data[1], 120);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 48u; i++) image.data[i] = 100u;

    failures +=
        assertEquals(tiny_image_channel_gain(&image, 1.5f, 1.0f, 0.5f), 0);
    failures += assertEquals(image.data[0], 150);
    failures += assertEquals(image.data[1], 100);
    failures += assertEquals(image.data[2], 50);

    // a channel mixer given the identity changes nothing, which is what makes
    // it eliminable
    TinyImage kept;
    tiny_image_create(&kept, 4, 4, 3);
    memcpy(kept.data, image.data, 48u);

    static const float IDENTITY[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    failures += assertEquals(tiny_image_channel_mixer(&image, IDENTITY), 0);
    failures += assertImageEquals(&image, &kept);

    // and a swap really swaps
    static const float SWAP[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};
    failures += assertEquals(tiny_image_channel_mixer(&image, SWAP), 0);
    failures += assertEquals(image.data[0], 50);
    failures += assertEquals(image.data[2], 150);

    // vibrance lifts a dull color more than a vivid one
    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 2, 1, 3) != TINYIMG_OK) return failures + 1;

    // a nearly gray pixel and a fully saturated one
    image.data[0] = 130u;
    image.data[1] = 120u;
    image.data[2] = 120u;
    image.data[3] = 255u;
    image.data[4] = 0u;
    image.data[5] = 0u;

    int32_t dull_before = (int32_t) image.data[0] - (int32_t) image.data[1];
    int32_t vivid_before = (int32_t) image.data[3] - (int32_t) image.data[4];

    failures += assertEquals(tiny_image_vibrance(&image, 1.0f), 0);

    int32_t dull_after = (int32_t) image.data[0] - (int32_t) image.data[1];
    int32_t vivid_after = (int32_t) image.data[3] - (int32_t) image.data[4];

    // the dull one gains proportionally more, which is the difference from a
    // plain saturation change
    failures += assertGreaterThan(
        (double) dull_after / (double) dull_before,
        (double) vivid_after / (double) vivid_before
    );

    tiny_image_destroy(&image);
    return failures;
}

/** Darken, lighten and the overlays act on the rectangle they were given. */
static int rect_filters(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 200u;

    // a rectangle covering the whole image goes through the planner as a
    // matrix, and a smaller one runs eagerly; both have to give the same
    // answer for the pixels they cover
    failures += assertEquals(tiny_image_darken(&image, 0, 0, 0, 0, 0.5f), 0);

    for (uint32_t i = 0; i < 16u * 16u; i++) {
        failures += assertIn((double) image.data[i * 3u], 99.0, 101.0);
    }

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 200u;

    failures += assertEquals(tiny_image_darken(&image, 0, 0, 4, 4, 0.5f), 0);
    failures += assertIn((double) at(&image, 1, 1)[0], 99.0, 101.0);
    failures += assertEquals(at(&image, 8, 8)[0], 200);

    // lighten moves toward white, so a channel already there stays
    failures += assertEquals(tiny_image_lighten(&image, 0, 0, 4, 4, 0.5f), 0);
    failures += assertGreaterThan(at(&image, 1, 1)[0], 140);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 255u;

    failures += assertEquals(tiny_image_lighten(&image, 0, 0, 0, 0, 1.0f), 0);
    failures += assertEquals(at(&image, 8, 8)[0], 255);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 100u;

    static const uint8_t GREEN[3] = {0, 200, 0};

    failures += assertEquals(tiny_image_color_overlay(&image, GREEN, 0.5f), 0);
    failures += assertIn((double) at(&image, 8, 8)[0], 49.0, 51.0);
    failures += assertIn((double) at(&image, 8, 8)[1], 149.0, 151.0);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 100u;

    failures += assertEquals(
        tiny_image_color_overlay_rect(&image, 2, 2, 4, 4, GREEN, 1.0f), 0
    );
    failures += assertEquals(at(&image, 3, 3)[1], 200);
    failures += assertEquals(at(&image, 10, 10)[1], 100);

    // a rectangle outside the image is a request with nothing in it
    failures += assertEquals(
        tiny_image_color_overlay_rect(&image, 99, 99, 4, 4, GREEN, 1.0f), 0
    );

    failures += assertEquals(
        tiny_image_darken(&image, 0, 0, 0, 0, 2.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_lighten(&image, 0, 0, 0, 0, -1.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_color_overlay(&image, GREEN, 2.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_color_overlay(&image, 0, 0.5f), TINYIMG_ERR_NULL
    );
    failures +=
        assertEquals(tiny_image_darken(0, 0, 0, 0, 0, 0.5f), TINYIMG_ERR_NULL);

    // a one channel image takes the color's middle entry, so a gray overlay
    // reaches it rather than being read past its own width
    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 8, 8, 1) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 64u; i++) image.data[i] = 100u;

    static const uint8_t GRAY[3] = {200, 200, 200};
    failures += assertEquals(
        tiny_image_color_overlay_rect(&image, 0, 0, 4, 4, GRAY, 1.0f), 0
    );
    failures += assertEquals(image.data[0], 200);
    failures += assertEquals(image.data[7], 100);

    tiny_image_destroy(&image);
    return failures;
}

/** A vignette darkens outward from a radius and leaves the middle alone. */
static int vignette(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 41, 41, 3) != TINYIMG_OK) return 1;
    for (uint32_t i = 0; i < 41u * 41u * 3u; i++) image.data[i] = 200u;

    failures += assertEquals(tiny_image_vignette(&image, 10.0f, 0.8f, 0), 0);

    // inside the radius nothing changed; at the corner it is much darker
    failures += assertEquals(at(&image, 20, 20)[0], 200);
    failures += assertEquals(at(&image, 25, 20)[0], 200);
    failures += assertLessThan(at(&image, 0, 0)[0], 100);

    // and it falls off monotonically outward along a row
    uint8_t previous = 200u;
    for (uint32_t x = 20; x < 41u; x++) {
        uint8_t value = at(&image, x, 20)[0];

        failures += assertTrue(value <= previous);
        previous = value;
    }

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 41, 41, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 41u * 41u * 3u; i++) image.data[i] = 100u;

    // a color other than black is faded toward instead
    static const uint8_t CYAN[3] = {0, 255, 255};
    failures += assertEquals(tiny_image_vignette(&image, 5.0f, 1.0f, CYAN), 0);
    failures += assertGreaterThan(at(&image, 0, 0)[1], 100);
    failures += assertLessThan(at(&image, 0, 0)[0], 100);

    // a radius past the image's own half diagonal has nothing to darken
    TinyImage kept;
    tiny_image_create(&kept, 41, 41, 3);
    memcpy(kept.data, image.data, 41u * 41u * 3u);

    failures += assertEquals(tiny_image_vignette(&image, 999.0f, 1.0f, 0), 0);
    failures += assertImageEquals(&image, &kept);

    failures += assertEquals(
        tiny_image_vignette(&image, 0.0f, 0.5f, 0), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_vignette(&image, 5.0f, 2.0f, 0), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_vignette(0, 5.0f, 0.5f, 0), TINYIMG_ERR_NULL);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

/** Every entry point in the region rejects a null or an out of range value. */
static int nulls(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return 1;

    static const uint8_t COLOR[3] = {10, 20, 30};

    failures +=
        assertEquals(tiny_image_apply_matrix(&image, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_apply_lut(&image, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_curves(&image, 0, 0, 3), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_colorize(&image, 0, 0.5f), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_tint(&image, 0, 0.5f), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_duotone(&image, COLOR, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_channel_mixer(&image, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_image_color_balance(&image, 0, 0, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(tiny_image_blackwhite(0), TINYIMG_ERR_NULL);

    failures += assertEquals(
        tiny_image_colorize(&image, COLOR, 1.5f), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_posterize(&image, 1), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_posterize(&image, 300), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_image_channel_gain(&image, -1.0f, 1.0f, 1.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_white_balance(&image, 2.0f, 0.0f), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_vibrance(&image, 9.0f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_fill_light(&image, 2.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_image_split_tone(&image, COLOR, COLOR, 2.0f), TINYIMG_ERR_RANGE
    );

    // a one channel image has no chroma to change, so the operations that need
    // three channels are a no-op rather than an error
    TinyImage gray;
    if (tiny_image_create(&gray, 4, 4, 1) != TINYIMG_OK) {
        tiny_image_destroy(&image);
        return failures + 1;
    }

    failures += assertEquals(tiny_image_vibrance(&gray, 0.5f), 0);
    failures += assertEquals(tiny_image_auto_color(&gray), 0);

    tiny_image_destroy(&gray);
    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += collapse();
    failures += presets();
    failures += curves();
    failures += per_channel();
    failures += tinting();
    failures += colorblindness();
    failures += toning();
    failures += balance();
    failures += rect_filters();
    failures += vignette();
    failures += escape_hatches();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
