#include "tinyimg/plan.h"

#include "test.h"

static TinyImage source;

static int resolve(const TinyPlan* plan, TinyPlanResolution* res) {
    return tiny_plan_resolve(plan, res);
}

/** An operation that changes nothing never reaches the executor. */
static int identities(void) {
    int failures = 0;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);

    tiny_plan_brightness(&plan, 1.0f);
    tiny_plan_contrast(&plan, 1.0f);
    tiny_plan_saturation(&plan, 1.0f);
    tiny_plan_gamma(&plan, 1.0f);
    tiny_plan_hue(&plan, 0.0f);
    tiny_plan_hue(&plan, 720.0f);
    tiny_plan_hue(&plan, -360.0f);
    tiny_plan_blur(&plan, 0.0f);
    tiny_plan_gaussian_blur(&plan, 0.0f);
    tiny_plan_rotate(&plan, 360);
    tiny_plan_resize(&plan, 400, 200);
    tiny_plan_crop(&plan, 0, 0, 400, 200);
    tiny_plan_crop(&plan, 0, 0, 0, 0);
    tiny_plan_fit(&plan, 400, 200, TINYIMG_FIT_CONTAIN, TINYIMG_GRAVITY_CENTER);

    failures += assertEquals(tiny_plan_count(&plan), 14);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);

    failures += assertEquals(res.ops, 0);
    failures += assertEquals(res.eliminated, 14);
    failures += assertEquals(res.collapsed, 0);
    failures += assertEquals(res.color_stages, 0);
    failures += assertEquals(res.width, 400);
    failures += assertEquals(res.height, 200);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_COPY) != 0);

    return failures;
}

/** A pair that cancels leaves nothing behind. */
static int annihilations(void) {
    int failures = 0;

    struct {
        const char* name;
        uint32_t appended;
    } names[] = {{"flip h", 2}, {"flip v", 2}, {"invert", 2}, {"turn", 4}};

    for (uint32_t which = 0; which < 4u; which++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &source);

        for (uint32_t i = 0; i < names[which].appended; i++) {
            switch (which) {
                case 0: tiny_plan_flip_horizontal(&plan); break;
                case 1: tiny_plan_flip_vertical(&plan); break;
                case 2: tiny_plan_invert(&plan); break;
                default: tiny_plan_rotate(&plan, 90); break;
            }
        }

        TinyPlanResolution res;
        failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);
        failures += assertEquals(res.ops, 0);
        failures += assertEquals(res.width, 400);
        failures += assertEquals(res.height, 200);
    }

    // three quarter turns are not four, so they leave one behind and swap the
    // extent
    TinyPlan three;
    tiny_plan_init_image(&three, &source);
    tiny_plan_rotate(&three, 90);
    tiny_plan_rotate(&three, 90);
    tiny_plan_rotate(&three, 90);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&three, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_ROTATE);
    failures += assertEquals(res.op[0].rotate.turns, 3);
    failures += assertEquals(res.width, 200);
    failures += assertEquals(res.height, 400);

    return failures;
}

/** Two of a kind become one. */
static int pairs(void) {
    int failures = 0;

    TinyPlan crops;
    tiny_plan_init_image(&crops, &source);
    tiny_plan_crop(&crops, 10, 10, 100, 100);
    tiny_plan_crop(&crops, 5, 5, 50, 50);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&crops, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_CROP);
    failures += assertEquals(res.op[0].crop.x, 15);
    failures += assertEquals(res.op[0].crop.y, 15);
    failures += assertEquals(res.op[0].crop.width, 50);
    failures += assertEquals(res.op[0].crop.height, 50);
    failures += assertEquals(res.collapsed, 1);

    // the second crop reaching past the first is clipped to what the first left
    TinyPlan clipped;
    tiny_plan_init_image(&clipped, &source);
    tiny_plan_crop(&clipped, 10, 10, 100, 100);
    tiny_plan_crop(&clipped, 50, 50, 500, 500);

    failures += assertEquals(resolve(&clipped, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].crop.x, 60);
    failures += assertEquals(res.op[0].crop.width, 50);
    failures += assertEquals(res.op[0].crop.height, 50);

    // the first resample's output is thrown away by the second, and one
    // resample of the source beats two
    TinyPlan resizes;
    tiny_plan_init_image(&resizes, &source);
    tiny_plan_resize(&resizes, 200, 100);
    tiny_plan_resize(&resizes, 50, 25);

    failures += assertEquals(resolve(&resizes, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].resize.width, 50);
    failures += assertEquals(res.op[0].resize.height, 25);
    failures += assertFloatEquals((float) res.source_width, 400.0f, 0.01f);

    return failures;
}

/**
 * @brief A color operation between two geometry ones does not stop them
 * pairing.
 *
 * A color operation reads one pixel and never moves it, so the two flips are
 * still adjacent as far as geometry is concerned and the merge is exact. This
 * is the rule that makes a chain built by a fluent API collapse at all, since
 * nobody writes their operations pre-sorted.
 */
static int commuting(void) {
    int failures = 0;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);
    tiny_plan_flip_horizontal(&plan);
    tiny_plan_brightness(&plan, 1.3f);
    tiny_plan_flip_horizontal(&plan);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_BRIGHTNESS);
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_ORIENT) != 0);

    TinyPlan crops;
    tiny_plan_init_image(&crops, &source);
    tiny_plan_crop(&crops, 10, 10, 100, 100);
    tiny_plan_gamma(&crops, 2.2f);
    tiny_plan_saturation(&crops, 0.5f);
    tiny_plan_crop(&crops, 5, 5, 50, 50);

    failures += assertEquals(resolve(&crops, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 3);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_CROP);
    failures += assertEquals(res.op[0].crop.x, 15);
    failures += assertEquals(res.op[0].crop.width, 50);

    /*
     * A neighborhood operation is a different matter, because it reads the
     * pixels around the one it writes. The crops stay apart.
     */
    TinyPlan blurred;
    tiny_plan_init_image(&blurred, &source);
    tiny_plan_crop(&blurred, 10, 10, 100, 100);
    tiny_plan_blur(&blurred, 3.0f);
    tiny_plan_crop(&blurred, 5, 5, 50, 50);

    failures += assertEquals(resolve(&blurred, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 3);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_CROP);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);
    failures += assertEquals(res.op[2].kind, TINYIMG_OP_CROP);
    failures += assertEquals(res.consumed, 1);
    failures += assertEquals(res.passes, 2);

    return failures;
}

/**
 * @brief A blur moves to the small side of a downscale, and only when it is
 * exact enough to.
 *
 * Gaussian blur commutes with scaling under a scaled sigma, so this is the same
 * operation over a fraction of the pixels. The two cases that refuse it are the
 * interesting ones: a reduction too small for the trade to pay, and one that
 * differs per axis, which an isotropic blur cannot follow.
 */
static int blur_reorder(void) {
    int failures = 0;

    TinyPlan moved;
    tiny_plan_init_image(&moved, &source);
    tiny_plan_gaussian_blur(&moved, 8.0f);
    tiny_plan_resize(&moved, 100, 50);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&moved, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 2);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);
    failures += assertFloatEquals(res.op[1].blur.amount, 2.0f, 1e-4f);
    failures += assertEquals(res.op[1].blur.gaussian, 1);

    // a reduction below two leaves the blur where it was
    TinyPlan small;
    tiny_plan_init_image(&small, &source);
    tiny_plan_gaussian_blur(&small, 8.0f);
    tiny_plan_resize(&small, 300, 150);

    failures += assertEquals(resolve(&small, &res), TINYIMG_OK);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_BLUR);
    failures += assertFloatEquals(res.op[0].blur.amount, 8.0f, 1e-4f);

    // an anisotropic reduction would need an anisotropic blur, which this
    // library does not have, so the swap is refused rather than approximated
    TinyPlan stretched;
    tiny_plan_init_image(&stretched, &source);
    tiny_plan_gaussian_blur(&stretched, 8.0f);
    tiny_plan_resize_with(&stretched, 40, 100, TINYIMG_FILTER_BOX);

    failures += assertEquals(resolve(&stretched, &res), TINYIMG_OK);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_BLUR);

    // a fit reduces too, and the same rule applies to it
    TinyPlan fitted;
    tiny_plan_init_image(&fitted, &source);
    tiny_plan_gaussian_blur(&fitted, 8.0f);
    tiny_plan_fit(&fitted, 100, 100, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER);

    failures += assertEquals(resolve(&fitted, &res), TINYIMG_OK);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_FIT);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);
    failures += assertFloatEquals(res.op[1].blur.amount, 4.0f, 1e-4f);

    // a blur scaled down far enough stops being a blur at all
    TinyPlan vanished;
    tiny_plan_init_image(&vanished, &source);
    tiny_plan_gaussian_blur(&vanished, 2.0f);
    tiny_plan_resize(&vanished, 20, 10);

    failures += assertEquals(resolve(&vanished, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(res.passes, 1);

    return failures;
}

/** A grayscale image has no saturation to change and no hue to turn. */
static int grayscale_absorbs(void) {
    int failures = 0;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);
    tiny_plan_grayscale(&plan);
    tiny_plan_saturation(&plan, 2.0f);
    tiny_plan_hue(&plan, 90.0f);
    tiny_plan_grayscale(&plan);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_GRAYSCALE);
    failures += assertEquals(res.channels, 1);

    // and only one color operation is left, so the decoder can produce the
    // luminance itself and the stage disappears with it
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_GRAY_DECODE) != 0);
    failures += assertEquals(res.color_stages, 0);

    // the other order is not the same rule: a saturation that clips a channel
    // changes the luminance, so it stays
    TinyPlan before;
    tiny_plan_init_image(&before, &source);
    tiny_plan_saturation(&before, 2.0f);
    tiny_plan_grayscale(&before);

    failures += assertEquals(resolve(&before, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 2);
    failures += assertEquals(res.color_stages, 1);
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_GRAY_DECODE) != 0);

    // a grayscale over an image that is already one channel is an identity
    TinyImage gray;
    if (tiny_image_create(&gray, 16, 16, 1) == TINYIMG_OK) {
        TinyPlan already;
        tiny_plan_init_image(&already, &gray);
        tiny_plan_grayscale(&already);

        failures += assertEquals(resolve(&already, &res), TINYIMG_OK);
        failures += assertEquals(res.ops, 0);

        tiny_image_destroy(&gray);
    }

    return failures;
}

/**
 * @brief Two of the same table are two operations and one stage.
 *
 * The rewrites do not merge them, because a gamma and a gamma are not one
 * gamma; the collapse does, because their tables compose. Keeping the two
 * mechanisms apart is what stops a rewrite rule having to know arithmetic.
 */
static int stages_not_rewrites(void) {
    int failures = 0;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);
    tiny_plan_gamma(&plan, 2.0f);
    tiny_plan_gamma(&plan, 1.5f);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 2);
    failures += assertEquals(res.collapsed, 0);
    failures += assertEquals(res.color_stages, 1);

    return failures;
}

/** A chain no rule touches comes through exactly as it was written. */
static int untouched(void) {
    int failures = 0;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &source);
    tiny_plan_crop(&plan, 10, 20, 200, 100);
    tiny_plan_resize(&plan, 80, 40);
    tiny_plan_rotate(&plan, 90);
    tiny_plan_brightness(&plan, 1.2f);
    tiny_plan_gamma(&plan, 2.2f);

    TinyPlanResolution res;
    failures += assertEquals(resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 5);
    failures += assertEquals(res.eliminated, 0);
    failures += assertEquals(res.collapsed, 0);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_CROP);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(res.op[2].kind, TINYIMG_OP_ROTATE);
    failures += assertEquals(res.op[3].kind, TINYIMG_OP_BRIGHTNESS);
    failures += assertEquals(res.op[4].kind, TINYIMG_OP_GAMMA);
    failures += assertEquals(res.width, 40);
    failures += assertEquals(res.height, 80);

    return failures;
}

int main(void) {
    if (tiny_image_create(&source, 400, 200, 3) != TINYIMG_OK) return 1;

    int failures = 0;

    failures += identities();
    failures += annihilations();
    failures += pairs();
    failures += commuting();
    failures += blur_reorder();
    failures += grayscale_absorbs();
    failures += stages_not_rewrites();
    failures += untouched();

    tiny_image_destroy(&source);

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
