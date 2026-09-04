#include "tinyimg/plan.h"

#include "test.h"

/**
 * @brief The worked example from the plan, which is the whole point of the
 * planner.
 *
 * A 500x500 rectangle of a six megapixel photograph, resampled to 100x100. The
 * output needs no detail above a quarter, so the decode is asked for a quarter
 * of a rectangle rather than all of the image, and the region and the scale
 * multiply: 15,625 pixels reach the resampler out of 6,009,000.
 */
static int worked_example(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("dog.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    if (tiny_plan_init(&plan, bytes, size) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    failures += assertEquals(plan.source_width, 3000);
    failures += assertEquals(plan.source_height, 2003);

    tiny_plan_crop(&plan, 2000, 1500, 500, 500);
    tiny_plan_resize(&plan, 100, 100);
    tiny_plan_brightness(&plan, 1.2f);
    tiny_plan_contrast(&plan, 1.1f);
    tiny_plan_saturation(&plan, 0.8f);
    tiny_plan_gamma(&plan, 2.2f);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);

    failures += assertEquals(res.decode.x, 2000);
    failures += assertEquals(res.decode.y, 1500);
    failures += assertEquals(res.decode.width, 500);
    failures += assertEquals(res.decode.height, 500);
    failures += assertEquals(res.decode.scale_den, 4);
    failures += assertEquals(res.decode_width, 125);
    failures += assertEquals(res.decode_height, 125);

    failures += assertEquals(res.sample_width, 100);
    failures += assertEquals(res.sample_height, 100);
    failures += assertEquals(res.width, 100);
    failures += assertEquals(res.height, 100);

    // four color operations, two kinds, and the nonlinear one is last, so they
    // collapse to exactly the one matrix and one table the plan claims
    failures += assertEquals(res.color_stages, 2);
    failures += assertEquals(res.color_stages_before, 0);

    failures += assertTrue((res.kernels & TINYIMG_KERNEL_REGION) != 0);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_SCALED) != 0);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_RESAMPLE) != 0);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_COLOR) != 0);
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_ORIENT) != 0);
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_PAD) != 0);

    uint64_t decoded = (uint64_t) res.decode_width * res.decode_height;
    uint64_t whole = (uint64_t) plan.source_width * plan.source_height;

    failures += assertEquals((long) decoded, 15625);
    failures += assertGreaterThan((double) whole / (double) decoded, 380.0);

    free(bytes);
    return failures;
}

/**
 * @brief A crop after a turn has to reach the corner the turn came from.
 *
 * The top left hundred pixels of an image turned a quarter clockwise are the
 * bottom left hundred of the image that was decoded, so this is the assertion
 * that the backward walk inverts the orientation rather than ignoring it. It is
 * also where a planner that folded the turn into a second pass would still be
 * decoding the whole image.
 */
static int through_a_turn(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("dog.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    if (tiny_plan_init(&plan, bytes, size) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    tiny_plan_rotate(&plan, 90);
    tiny_plan_crop(&plan, 0, 0, 100, 100);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);

    failures += assertEquals(res.decode.x, 0);
    failures += assertEquals(res.decode.y, 1903);
    failures += assertEquals(res.decode.width, 100);
    failures += assertEquals(res.decode.height, 100);
    failures += assertEquals(res.decode.scale_den, 1);
    failures += assertEquals(res.width, 100);
    failures += assertEquals(res.height, 100);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_ORIENT) != 0);

    // the turn costs nothing but the addressing, so there is no resample
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_RESAMPLE) != 0);

    tiny_plan_rotate(&plan, 270);

    // and a turn that undoes it leaves the region where it started
    TinyPlanResolution back;
    failures += assertEquals(tiny_plan_resolve(&plan, &back), TINYIMG_OK);
    failures += assertEquals(back.decode.x, 0);
    failures += assertEquals(back.decode.y, 1903);
    failures += assertFalse((back.kernels & TINYIMG_KERNEL_ORIENT) != 0);

    free(bytes);
    return failures;
}

/** The scale the decoder is asked for, against what the output needs. */
static int propagation(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    struct {
        uint32_t width;
        uint32_t height;
        uint8_t den;
    } cases[] = {{100, 56, 8},  {229, 129, 8},   {230, 129, 4},  {300, 169, 4},
                 {458, 258, 4}, {459, 258, 2},   {500, 281, 2},  {917, 516, 2},
                 {918, 516, 1}, {1835, 1032, 1}, {4000, 2250, 1}};

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TinyPlan plan;
        if (tiny_plan_init(&plan, bytes, size) != TINYIMG_OK) {
            free(bytes);
            return failures + 1;
        }

        tiny_plan_resize(&plan, cases[i].width, cases[i].height);

        TinyPlanResolution res;
        if (tiny_plan_resolve(&plan, &res) != TINYIMG_OK) {
            failures++;
            continue;
        }

        failures += assertEquals(res.decode.scale_den, cases[i].den);

        // a reduction never asks the decoder for less than the resampler is
        // about to produce, so nothing is ever shrunk and then enlarged
        if (cases[i].width <= 1835 && cases[i].height <= 1032) {
            failures += assertTrue(res.decode_width >= res.sample_width);
            failures += assertTrue(res.decode_height >= res.sample_height);
        }
    }

    // an already decoded source has no decode to choose, so the scale stays at
    // one however small the output is
    TinyImage image;
    if (tiny_image_create(&image, 800, 600, 3) == TINYIMG_OK) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &image);
        tiny_plan_resize(&plan, 10, 8);

        TinyPlanResolution res;
        failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);
        failures += assertEquals(res.decode.scale_den, 1);
        failures += assertEquals(res.decode_width, 800);

        tiny_image_destroy(&image);
    }

    free(bytes);
    return failures;
}

/**
 * @brief A neighborhood operation stops the propagation unless it was moved.
 *
 * Scaling the decode under a blur would change the blur's radius without
 * anybody having said so, which is the rewrite's job and is measured where the
 * rewrite is. So the conservative answer is the correct one here, and the two
 * cases differ only in whether the reduction was worth the swap.
 */
static int blur_blocks_propagation(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan blocked;
    tiny_plan_init(&blocked, bytes, size);
    tiny_plan_gaussian_blur(&blocked, 4.0f);
    tiny_plan_resize(&blocked, 4000, 2250);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&blocked, &res), TINYIMG_OK);
    failures += assertEquals(res.decode.scale_den, 1);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_BLUR);
    failures += assertEquals(res.consumed, 0);
    failures += assertEquals(res.passes, 2);

    TinyPlan moved;
    tiny_plan_init(&moved, bytes, size);
    tiny_plan_gaussian_blur(&moved, 20.0f);
    tiny_plan_resize(&moved, 100, 56);

    failures += assertEquals(tiny_plan_resolve(&moved, &res), TINYIMG_OK);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);
    failures += assertEquals(res.decode.scale_den, 8);
    failures += assertEquals(res.consumed, 1);

    free(bytes);
    return failures;
}

/** Every fit mode, as arithmetic on a two to one source and a square target. */
static int fit_modes(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    struct {
        TinyImageFit mode;
        uint32_t width;
        uint32_t height;
        uint32_t sample_width;
        uint32_t sample_height;
        int resamples;
    } cases[] = {
        {TINYIMG_FIT_CONTAIN, 100, 50, 100, 50, 1},
        {TINYIMG_FIT_SCALE_DOWN, 100, 50, 100, 50, 1},
        {TINYIMG_FIT_SCALE_UP, 400, 200, 400, 200, 0},
        {TINYIMG_FIT_PAD, 100, 100, 100, 50, 1},
        {TINYIMG_FIT_ASPECT_CONTAIN, 400, 400, 400, 200, 0},
        {TINYIMG_FIT_COVER, 100, 100, 100, 100, 1},
        {TINYIMG_FIT_CROP, 100, 100, 100, 100, 1},
        {TINYIMG_FIT_FILL, 100, 100, 100, 100, 0},
        {TINYIMG_FIT_ASPECT_COVER, 200, 200, 200, 200, 0},
        {TINYIMG_FIT_ASPECT_CROP, 100, 100, 100, 100, 0},
        {TINYIMG_FIT_STRETCH, 100, 100, 100, 100, 1}
    };

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &image);
        tiny_plan_fit(&plan, 100, 100, cases[i].mode, TINYIMG_GRAVITY_CENTER);

        TinyPlanResolution res;
        if (tiny_plan_resolve(&plan, &res) != TINYIMG_OK) {
            failures++;
            continue;
        }

        failures += assertEquals(res.width, cases[i].width);
        failures += assertEquals(res.height, cases[i].height);
        failures += assertEquals(res.sample_width, cases[i].sample_width);
        failures += assertEquals(res.sample_height, cases[i].sample_height);

        // the three fixed scale modes reach the output without reading a pixel
        // through a filter, which is the only thing that separates them from
        // the modes they share an extent with
        failures += assertEquals(
            (res.kernels & TINYIMG_KERNEL_RESAMPLE) != 0, cases[i].resamples
        );
    }

    // padding is the only mode that leaves room to fill
    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    tiny_plan_fit(&plan, 100, 100, TINYIMG_FIT_PAD, TINYIMG_GRAVITY_CENTER);

    TinyPlanResolution res;
    tiny_plan_resolve(&plan, &res);

    failures += assertTrue((res.kernels & TINYIMG_KERNEL_PAD) != 0);
    failures += assertEquals(res.offset_x, 0);
    failures += assertEquals(res.offset_y, 25);

    tiny_image_destroy(&image);
    return failures;
}

/** Where a crop lands, and where a pad puts what it has. */
static int gravities(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    struct {
        TinyImageGravity gravity;
        uint32_t crop_x;
        uint32_t pad_y;
    } cases[] = {
        {TINYIMG_GRAVITY_CENTER, 100, 25},
        {TINYIMG_GRAVITY_NORTH, 100, 0},
        {TINYIMG_GRAVITY_SOUTH, 100, 50},
        {TINYIMG_GRAVITY_WEST, 0, 25},
        {TINYIMG_GRAVITY_EAST, 200, 25},
        {TINYIMG_GRAVITY_NORTH_WEST, 0, 0},
        {TINYIMG_GRAVITY_NORTH_EAST, 200, 0},
        {TINYIMG_GRAVITY_SOUTH_WEST, 0, 50},
        {TINYIMG_GRAVITY_SOUTH_EAST, 200, 50},
        // both computed gravities fall back to the center until the phases that
        // measure them land, and the fallback is what has to be clean
        {TINYIMG_GRAVITY_AUTO, 100, 25},
        {TINYIMG_GRAVITY_FACE, 100, 25}
    };

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TinyPlan crop;
        tiny_plan_init_image(&crop, &image);
        tiny_plan_fit(
            &crop, 200, 200, TINYIMG_FIT_ASPECT_CROP, cases[i].gravity
        );

        TinyPlanResolution res;
        if (tiny_plan_resolve(&crop, &res) != TINYIMG_OK) {
            failures++;
            continue;
        }

        failures += assertEquals(res.width, 200);
        failures += assertEquals(res.height, 200);
        failures += assertFloatEquals(
            (float) res.source_x, (float) cases[i].crop_x, 0.01f
        );

        TinyPlan pad;
        tiny_plan_init_image(&pad, &image);
        tiny_plan_fit(&pad, 100, 100, TINYIMG_FIT_PAD, cases[i].gravity);

        if (tiny_plan_resolve(&pad, &res) != TINYIMG_OK) {
            failures++;
            continue;
        }

        failures += assertEquals(res.offset_y, cases[i].pad_y);
    }

    tiny_image_destroy(&image);
    return failures;
}

/** A crop after a resize is in the coordinates the resize produced. */
static int crop_after_resize(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    tiny_plan_resize(&plan, 100, 50);
    tiny_plan_crop(&plan, 25, 10, 50, 20);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);

    // a quarter scale resize means every output pixel is four source pixels
    // across, so the window is the crop times four and the sample positions are
    // exactly where the uncropped resize would have put them
    failures += assertFloatEquals((float) res.source_x, 100.0f, 0.01f);
    failures += assertFloatEquals((float) res.source_y, 40.0f, 0.01f);
    failures += assertFloatEquals((float) res.source_width, 200.0f, 0.01f);
    failures += assertFloatEquals((float) res.source_height, 80.0f, 0.01f);
    failures += assertEquals(res.sample_width, 50);
    failures += assertEquals(res.sample_height, 20);
    failures += assertEquals(res.width, 50);
    failures += assertEquals(res.height, 20);

    tiny_image_destroy(&image);
    return failures;
}

/** A zero axis on a resize is filled in from the aspect ratio. */
static int aspect_resize(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    TinyPlan wide;
    tiny_plan_init_image(&wide, &image);
    tiny_plan_resize(&wide, 100, 0);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&wide, &res), TINYIMG_OK);
    failures += assertEquals(res.width, 100);
    failures += assertEquals(res.height, 50);

    TinyPlan tall;
    tiny_plan_init_image(&tall, &image);
    tiny_plan_resize(&tall, 0, 50);

    failures += assertEquals(tiny_plan_resolve(&tall, &res), TINYIMG_OK);
    failures += assertEquals(res.width, 100);
    failures += assertEquals(res.height, 50);

    // an axis that would round to nothing is held at one pixel rather than
    // producing an image with no pixels in it
    TinyPlan thin;
    tiny_plan_init_image(&thin, &image);
    tiny_plan_resize(&thin, 1, 0);

    failures += assertEquals(tiny_plan_resolve(&thin, &res), TINYIMG_OK);
    failures += assertEquals(res.width, 1);
    failures += assertEquals(res.height, 1);

    tiny_image_destroy(&image);
    return failures;
}

/** A crop with a zero extent runs to the far edge; one outside is an error. */
static int crop_bounds(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    TinyPlan open;
    tiny_plan_init_image(&open, &image);
    tiny_plan_crop(&open, 100, 50, 0, 0);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&open, &res), TINYIMG_OK);
    failures += assertEquals(res.width, 300);
    failures += assertEquals(res.height, 150);

    TinyPlan over;
    tiny_plan_init_image(&over, &image);
    tiny_plan_crop(&over, 100, 50, 9000, 9000);

    failures += assertEquals(tiny_plan_resolve(&over, &res), TINYIMG_OK);
    failures += assertEquals(res.width, 300);
    failures += assertEquals(res.height, 150);

    TinyPlan outside;
    tiny_plan_init_image(&outside, &image);
    tiny_plan_crop(&outside, 400, 0, 10, 10);

    failures +=
        assertEquals(tiny_plan_resolve(&outside, &res), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief The aspect modes on a source whose mismatch goes the other way.
 *
 * The fit resolver has a branch per axis, and a wide source only ever exercises
 * one of them. A tall one takes the other, which is where a copied line with
 * the wrong dimension in it would sit unnoticed.
 */
static int tall_source(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 200, 400, 3) != TINYIMG_OK) return 1;

    struct {
        TinyImageFit mode;
        uint32_t width;
        uint32_t height;
    } cases[] = {
        // a 1:2 source into a 2:1 target, so every mode has to widen or
        // shorten rather than the reverse
        {TINYIMG_FIT_ASPECT_COVER, 200, 100},
        {TINYIMG_FIT_ASPECT_CONTAIN, 800, 400},
        {TINYIMG_FIT_ASPECT_CROP, 200, 100},
        {TINYIMG_FIT_CONTAIN, 50, 100},
        {TINYIMG_FIT_COVER, 200, 100},
        {TINYIMG_FIT_PAD, 200, 100}
    };

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &image);
        tiny_plan_fit(&plan, 200, 100, cases[i].mode, TINYIMG_GRAVITY_CENTER);

        TinyPlanResolution res;
        if (tiny_plan_resolve(&plan, &res) != TINYIMG_OK) {
            failures++;
            continue;
        }

        failures += assertEquals(res.width, cases[i].width);
        failures += assertEquals(res.height, cases[i].height);
    }

    tiny_image_destroy(&image);
    return failures;
}

/** Geometry after a pad needs the padding to exist, so it ends the pass. */
static int pad_then_geometry(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 400, 200, 3) != TINYIMG_OK) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    tiny_plan_fit(&plan, 400, 400, TINYIMG_FIT_PAD, TINYIMG_GRAVITY_CENTER);
    tiny_plan_resize(&plan, 100, 100);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.ops, 2);
    failures += assertEquals(res.consumed, 1);
    failures += assertEquals(res.width, 400);
    failures += assertEquals(res.height, 400);

    // and running it reaches the extent the second operation asked for
    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 100);
    failures += assertEquals(out.height, 100);

    tiny_image_destroy(&out);
    tiny_image_destroy(&image);
    return failures;
}

/** An output past the pixel budget is refused before anything is allocated. */
static int too_large(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 64, 64, 3) != TINYIMG_OK) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    tiny_plan_resize(&plan, 5000, 5000);

    TinyPlanResolution res;
    failures +=
        assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_ERR_TOO_LARGE);

    TinyImage out;
    memset(&out, 0, sizeof(out));
    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_ERR_TOO_LARGE);

    // just under the budget is allowed, which is what says the limit is the
    // budget and not an arbitrary extent. the source is RGB, so the byte cap is
    // the one that binds and 3344 squared is the last extent under it
    TinyPlan allowed;
    tiny_plan_init_image(&allowed, &image);
    tiny_plan_resize(&allowed, 3344, 3344);

    failures += assertEquals(tiny_plan_resolve(&allowed, &res), TINYIMG_OK);

    // resolve has to refuse what the executor could not allocate, or a plan
    // that resolved would still fail in the middle of running
    TinyPlan overByBytes;
    tiny_plan_init_image(&overByBytes, &image);
    tiny_plan_resize(&overByBytes, 3345, 3345);

    failures += assertEquals(
        tiny_plan_resolve(&overByBytes, &res), TINYIMG_ERR_TOO_LARGE
    );

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    failures += worked_example();
    failures += tall_source();
    failures += pad_then_geometry();
    failures += too_large();
    failures += through_a_turn();
    failures += propagation();
    failures += blur_blocks_propagation();
    failures += fit_modes();
    failures += gravities();
    failures += aspect_resize();
    failures += crop_after_resize();
    failures += crop_bounds();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
