#include "tinyimg/codec/codec.h"
#include "tinyimg/plan.h"
#include "tinyimg/util.h"

#include "test.h"

#define ONE 65536

static TinyImage source;
static TinyColorStage stages[TINYIMG_PLAN_MAX_OPS];

/** Resolves a plan and collapses its colour operations. */
static uint32_t collapse(const TinyPlan* plan, TinyPlanResolution* res) {
    uint32_t count = 0;

    if (tiny_plan_resolve(plan, res) != TINYIMG_OK) return 0;
    if (tiny_plan_color_stages(res, stages, TINYIMG_PLAN_MAX_OPS, &count) !=
        TINYIMG_OK) {
        return 0;
    }

    return count;
}

/** One affine operation is one matrix. */
static int single_matrices(void) {
    int failures = 0;

    TinyPlanResolution res;

    TinyPlan bright;
    tiny_plan_init_image(&bright, &source);
    tiny_plan_brightness(&bright, 2.0f);

    failures += assertEquals(collapse(&bright, &res), 1);
    failures += assertEquals(stages[0].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);
    failures += assertEquals(stages[0].matrix[0], 2 * ONE);
    failures += assertEquals(stages[0].matrix[5], 2 * ONE);
    failures += assertEquals(stages[0].matrix[10], 2 * ONE);
    failures += assertEquals(stages[0].matrix[3], 0);

    TinyPlan negate;
    tiny_plan_init_image(&negate, &source);
    tiny_plan_invert(&negate);

    failures += assertEquals(collapse(&negate, &res), 1);
    failures += assertEquals(stages[0].matrix[0], -ONE);
    failures += assertEquals(stages[0].matrix[3], 255 * ONE);

    TinyPlan harder;
    tiny_plan_init_image(&harder, &source);
    tiny_plan_contrast(&harder, 2.0f);

    failures += assertEquals(collapse(&harder, &res), 1);
    failures += assertEquals(stages[0].matrix[0], 2 * ONE);
    failures += assertEquals(stages[0].matrix[3], -(127 * ONE + ONE / 2));

    // a saturation of nothing is a luminance in every channel, which is the
    // same matrix a greyscale asks for
    TinyPlan flat;
    tiny_plan_init_image(&flat, &source);
    tiny_plan_saturation(&flat, 0.0f);

    failures += assertEquals(collapse(&flat, &res), 1);

    for (uint32_t row = 0; row < 3u; row++) {
        failures += assertEquals(stages[0].matrix[row * 4u], 13933);
        failures += assertEquals(stages[0].matrix[row * 4u + 1u], 46871);
        failures += assertEquals(stages[0].matrix[row * 4u + 2u], 4732);
        failures += assertEquals(stages[0].matrix[row * 4u + 3u], 0);
    }

    return failures;
}

/** Affine operations compose by multiplication, and order is not free. */
static int composition(void) {
    int failures = 0;

    TinyPlanResolution res;

    TinyPlan twice;
    tiny_plan_init_image(&twice, &source);
    tiny_plan_brightness(&twice, 2.0f);
    tiny_plan_brightness(&twice, 3.0f);

    failures += assertEquals(collapse(&twice, &res), 1);
    failures += assertEquals(stages[0].matrix[0], 6 * ONE);

    /*
     * Inverting then brightening is 2(255 - x), and brightening then inverting
     * is 255 - 2x. Both are affine and both collapse to one matrix, but they
     * are different matrices, so this is where a composition written the wrong
     * way round shows up.
     */
    TinyPlan first;
    tiny_plan_init_image(&first, &source);
    tiny_plan_invert(&first);
    tiny_plan_brightness(&first, 2.0f);

    failures += assertEquals(collapse(&first, &res), 1);
    failures += assertEquals(stages[0].matrix[0], -2 * ONE);
    failures += assertEquals(stages[0].matrix[3], 510 * ONE);

    TinyPlan second;
    tiny_plan_init_image(&second, &source);
    tiny_plan_brightness(&second, 2.0f);
    tiny_plan_invert(&second);

    failures += assertEquals(collapse(&second, &res), 1);
    failures += assertEquals(stages[0].matrix[0], -2 * ONE);
    failures += assertEquals(stages[0].matrix[3], 255 * ONE);

    return failures;
}

/**
 * @brief A hue rotation leaves the luminance where it was.
 *
 * Which is a property of the matrix and not of any image: the three channel
 * weights of every row sum to one, so a pixel whose channels are equal comes
 * through unchanged at any angle. It is also why a hue rotation after a
 * greyscale can be dropped.
 */
static int hue_preserves_luma(void) {
    int failures = 0;

    float angles[] = {30.0f, 90.0f, 120.0f, 180.0f, 240.0f, -45.0f};
    TinyPlanResolution res;

    for (uint32_t i = 0; i < sizeof(angles) / sizeof(angles[0]); i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &source);
        tiny_plan_hue(&plan, angles[i]);

        failures += assertEquals(collapse(&plan, &res), 1);

        for (uint32_t row = 0; row < 3u; row++) {
            int32_t sum = stages[0].matrix[row * 4u] +
                          stages[0].matrix[row * 4u + 1u] +
                          stages[0].matrix[row * 4u + 2u];

            failures += assertIn((double) sum, ONE - 40, ONE + 40);
            failures += assertEquals(stages[0].matrix[row * 4u + 3u], 0);
        }
    }

    return failures;
}

/** Tables compose into one table. */
static int tables(void) {
    int failures = 0;

    TinyPlanResolution res;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);
    tiny_plan_gamma(&plan, 2.0f);
    tiny_plan_gamma(&plan, 1.5f);

    failures += assertEquals(collapse(&plan, &res), 1);
    failures += assertEquals(stages[0].kind, TINYIMG_OP_CLASS_COLOR_LUT);

    uint8_t coarse[256];
    uint8_t fine[256];
    tiny_lut_gamma(coarse, 2.0f);
    tiny_lut_gamma(fine, 1.5f);

    for (uint32_t v = 0; v < 256u; v++) {
        failures += assertEquals(stages[0].lut[0][v], fine[coarse[v]]);
        failures += assertEquals(stages[0].lut[1][v], fine[coarse[v]]);
        failures += assertEquals(stages[0].lut[2][v], fine[coarse[v]]);
    }

    return failures;
}

/**
 * @brief Interleaved kinds cost a stage each, and the worked chain does not
 * interleave.
 *
 * A matrix cannot pass through a table, so a chain that alternates between them
 * keeps its stages. That still costs one traversal, not one per operation,
 * which is where the saving actually comes from.
 */
static int interleaving(void) {
    int failures = 0;

    TinyPlanResolution res;

    TinyPlan worked;
    tiny_plan_init_image(&worked, &source);
    tiny_plan_brightness(&worked, 1.2f);
    tiny_plan_contrast(&worked, 1.1f);
    tiny_plan_saturation(&worked, 0.8f);
    tiny_plan_gamma(&worked, 2.2f);

    failures += assertEquals(collapse(&worked, &res), 2);
    failures += assertEquals(stages[0].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);
    failures += assertEquals(stages[1].kind, TINYIMG_OP_CLASS_COLOR_LUT);

    TinyPlan mixed;
    tiny_plan_init_image(&mixed, &source);
    tiny_plan_brightness(&mixed, 1.2f);
    tiny_plan_gamma(&mixed, 2.2f);
    tiny_plan_saturation(&mixed, 0.8f);

    failures += assertEquals(collapse(&mixed, &res), 3);
    failures += assertEquals(stages[0].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);
    failures += assertEquals(stages[1].kind, TINYIMG_OP_CLASS_COLOR_LUT);
    failures += assertEquals(stages[2].kind, TINYIMG_OP_CLASS_COLOR_MATRIX);

    TinyPlan many;
    tiny_plan_init_image(&many, &source);
    for (uint32_t i = 0; i < 4u; i++) {
        tiny_plan_brightness(&many, 1.1f);
        tiny_plan_contrast(&many, 1.1f);
        tiny_plan_saturation(&many, 1.1f);
    }

    // twelve affine operations, still one matrix
    failures += assertEquals(collapse(&many, &res), 1);

    uint32_t count = 0;
    tiny_plan_resolve(&many, &res);
    failures += assertEquals(res.ops, 12);
    failures += assertEquals(
        tiny_plan_color_stages(&res, stages, 0, &count),
        TINYIMG_ERR_BUFFER_TOO_SMALL
    );
    failures += assertEquals(
        tiny_plan_color_stages(0, stages, 1, &count), TINYIMG_ERR_NULL
    );

    return failures;
}

/**
 * @brief Which side of the resample each stage runs on.
 *
 * Where the caller put it, and nowhere else. The output side is the cheap side
 * of a reduction, and an affine function does commute with a weighted average,
 * so moving a matrix there is tempting; the clamp that follows it does not
 * commute, and the measurement is in the chain tests.
 */
static int split(void) {
    int failures = 0;

    TinyPlanResolution res;

    TinyPlan after;
    tiny_plan_init_image(&after, &source);
    tiny_plan_resize(&after, 100, 50);
    tiny_plan_gamma(&after, 2.2f);
    tiny_plan_brightness(&after, 1.2f);

    failures += assertEquals(collapse(&after, &res), 2);
    failures += assertEquals(res.color_stages_before, 0);

    TinyPlan before;
    tiny_plan_init_image(&before, &source);
    tiny_plan_gamma(&before, 2.2f);
    tiny_plan_brightness(&before, 1.2f);
    tiny_plan_resize(&before, 100, 50);

    failures += assertEquals(collapse(&before, &res), 2);
    failures += assertEquals(res.color_stages_before, 2);

    TinyPlan matrix;
    tiny_plan_init_image(&matrix, &source);
    tiny_plan_brightness(&matrix, 1.2f);
    tiny_plan_resize(&matrix, 100, 50);

    failures += assertEquals(collapse(&matrix, &res), 1);
    failures += assertEquals(res.color_stages_before, 1);

    // one either side of the resize, which is where the split earns its keep
    TinyPlan straddling;
    tiny_plan_init_image(&straddling, &source);
    tiny_plan_brightness(&straddling, 1.2f);
    tiny_plan_resize(&straddling, 100, 50);
    tiny_plan_gamma(&straddling, 2.2f);

    failures += assertEquals(collapse(&straddling, &res), 2);
    failures += assertEquals(res.color_stages_before, 1);

    // a fit resamples too, unless its mode fixes the scale
    TinyPlan fitted;
    tiny_plan_init_image(&fitted, &source);
    tiny_plan_brightness(&fitted, 1.2f);
    tiny_plan_fit(&fitted, 100, 100, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER);

    failures += assertEquals(collapse(&fitted, &res), 1);
    failures += assertEquals(res.color_stages_before, 1);

    return failures;
}

/** The collapsed stages, applied, on values worked out by hand. */
static int pixels(void) {
    int failures = 0;

    TinyImage strip;
    if (tiny_image_create(&strip, 2, 1, 3) != TINYIMG_OK) return 1;

    strip.data[0] = 100;
    strip.data[1] = 150;
    strip.data[2] = 200;
    strip.data[3] = 10;
    strip.data[4] = 20;
    strip.data[5] = 30;

    // brightness of one and a half, so the third channel of the first pixel
    // clamps and nothing else does
    uint8_t brightened[6] = {150, 225, 255, 15, 30, 45};

    TinyPlan bright;
    tiny_plan_init_image(&bright, &strip);
    tiny_plan_brightness(&bright, 1.5f);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&bright, &out), TINYIMG_OK);
    failures += assertBytesMatch(out.data, brightened, 6);
    tiny_image_destroy(&out);

    // an inversion
    uint8_t inverted[6] = {155, 105, 55, 245, 235, 225};

    TinyPlan negate;
    tiny_plan_init_image(&negate, &strip);
    tiny_plan_invert(&negate);

    failures += assertEquals(tiny_plan_run(&negate, &out), TINYIMG_OK);
    failures += assertBytesMatch(out.data, inverted, 6);
    tiny_image_destroy(&out);

    // a doubled contrast about the middle of the range, so 100 lands on 73 and
    // 10 falls off the bottom
    uint8_t contrasted[6] = {73, 173, 255, 0, 0, 0};

    TinyPlan harder;
    tiny_plan_init_image(&harder, &strip);
    tiny_plan_contrast(&harder, 2.0f);

    failures += assertEquals(tiny_plan_run(&harder, &out), TINYIMG_OK);
    failures += assertBytesMatch(out.data, contrasted, 6);
    tiny_image_destroy(&out);

    // a saturation of nothing puts the luminance in all three channels, and it
    // is the same luminance the codecs reduce with
    uint8_t grey[6];
    grey[0] = grey[1] = grey[2] = tiny_luma(100, 150, 200);
    grey[3] = grey[4] = grey[5] = tiny_luma(10, 20, 30);

    TinyPlan flat;
    tiny_plan_init_image(&flat, &strip);
    tiny_plan_saturation(&flat, 0.0f);

    failures += assertEquals(tiny_plan_run(&flat, &out), TINYIMG_OK);
    failures += assertBytesMatch(out.data, grey, 6);
    tiny_image_destroy(&out);

    // and a greyscale is the same values in one channel
    TinyPlan mono;
    tiny_plan_init_image(&mono, &strip);
    tiny_plan_grayscale(&mono);

    failures += assertEquals(tiny_plan_run(&mono, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 1);
    failures += assertEquals(out.data[0], grey[0]);
    failures += assertEquals(out.data[1], grey[3]);
    tiny_image_destroy(&out);

    // a gamma is the library's own table, applied per channel
    uint8_t curve[256];
    tiny_lut_gamma(curve, 2.2f);

    uint8_t curved[6];
    for (uint32_t i = 0; i < 6u; i++) curved[i] = curve[strip.data[i]];

    TinyPlan gamma;
    tiny_plan_init_image(&gamma, &strip);
    tiny_plan_gamma(&gamma, 2.2f);

    failures += assertEquals(tiny_plan_run(&gamma, &out), TINYIMG_OK);
    failures += assertBytesMatch(out.data, curved, 6);
    tiny_image_destroy(&out);

    tiny_image_destroy(&strip);
    return failures;
}

/**
 * @brief A colour operation on a single channel image reads as if it had three.
 *
 * Widening to RGB, applying and reducing again is what a caller means, and
 * taking the matrix's first row alone is not the same thing: it would turn a
 * hue rotation of a grey image into a brightness change.
 */
static int grey_channels(void) {
    int failures = 0;

    TinyImage strip;
    if (tiny_image_create(&strip, 2, 1, 1) != TINYIMG_OK) return 1;

    strip.data[0] = 100;
    strip.data[1] = 200;

    TinyImage out;
    memset(&out, 0, sizeof(out));

    TinyPlan turned;
    tiny_plan_init_image(&turned, &strip);
    tiny_plan_hue(&turned, 120.0f);

    failures += assertEquals(tiny_plan_run(&turned, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 1);
    failures += assertIn(out.data[0], 99, 101);
    failures += assertIn(out.data[1], 199, 201);
    tiny_image_destroy(&out);

    TinyPlan bright;
    tiny_plan_init_image(&bright, &strip);
    tiny_plan_brightness(&bright, 1.2f);

    failures += assertEquals(tiny_plan_run(&bright, &out), TINYIMG_OK);
    failures += assertEquals(out.data[0], 120);
    failures += assertEquals(out.data[1], 240);
    tiny_image_destroy(&out);

    TinyPlan negate;
    tiny_plan_init_image(&negate, &strip);
    tiny_plan_invert(&negate);

    failures += assertEquals(tiny_plan_run(&negate, &out), TINYIMG_OK);
    failures += assertEquals(out.data[0], 155);
    failures += assertEquals(out.data[1], 55);
    tiny_image_destroy(&out);

    // a table reads the green channel's entry, since that is the one the
    // luminance weights most and a single channel is a luminance
    uint8_t curve[256];
    tiny_lut_gamma(curve, 2.2f);

    TinyPlan gamma;
    tiny_plan_init_image(&gamma, &strip);
    tiny_plan_gamma(&gamma, 2.2f);

    failures += assertEquals(tiny_plan_run(&gamma, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 1);
    failures += assertEquals(out.data[0], curve[100]);
    failures += assertEquals(out.data[1], curve[200]);
    tiny_image_destroy(&out);

    tiny_image_destroy(&strip);
    return failures;
}

/** Alpha is never read and never written by a colour operation. */
static int alpha_untouched(void) {
    int failures = 0;

    TinyImage strip;
    if (tiny_image_create(&strip, 2, 1, 4) != TINYIMG_OK) return 1;

    uint8_t pixels[8] = {100, 150, 200, 64, 10, 20, 30, 200};
    memcpy(strip.data, pixels, 8);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    TinyPlan plan;
    tiny_plan_init_image(&plan, &strip);
    tiny_plan_invert(&plan);
    tiny_plan_gamma(&plan, 2.2f);

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 4);
    failures += assertEquals(out.data[3], 64);
    failures += assertEquals(out.data[7], 200);

    tiny_image_destroy(&out);
    tiny_image_destroy(&strip);
    return failures;
}

int main(void) {
    if (tiny_image_create(&source, 400, 200, 3) != TINYIMG_OK) return 1;

    int failures = 0;

    failures += single_matrices();
    failures += composition();
    failures += hue_preserves_luma();
    failures += tables();
    failures += interleaving();
    failures += split();
    failures += pixels();
    failures += grey_channels();
    failures += alpha_untouched();

    tiny_image_destroy(&source);

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
