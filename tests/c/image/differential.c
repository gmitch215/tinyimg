#include "tinyimg/plan.h"

#include "test.h"

/**
 * @brief Loads a fixture as pixels.
 *
 * @param image Receives the decoded image.
 * @param name Fixture path.
 * @return int Non-zero on success.
 */
static int load(TinyImage* image, const char* name) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);

    if (!bytes) return 0;

    memset(image, 0, sizeof(*image));

    int result = tiny_image_load(image, bytes, size);
    free(bytes);

    return result == TINYIMG_OK;
}

/**
 * @brief Compares an operation's output against ImageMagick's.
 *
 * @param name What the case is called, for the reader.
 * @param ours Our result.
 * @param reference The fixture holding magick's result.
 * @param floor_db The floor the agreement has to clear.
 * @return int How many assertions failed.
 */
static int compare(
    const char* name, const TinyImage* ours, const char* reference,
    double floor_db
) {
    int failures = 0;
    TinyImage theirs;

    printf("  %s\n", name);

    if (!load(&theirs, reference)) return 1;

    failures += assertEquals(ours->width, theirs.width);
    failures += assertEquals(ours->height, theirs.height);
    failures += assertEquals(ours->channels, theirs.channels);

    if (ours->width == theirs.width && ours->height == theirs.height &&
        ours->channels == theirs.channels) {
        failures += assertPSNR(
            ours->data, theirs.data,
            (size_t) ours->width * ours->height * ours->channels, floor_db
        );
    }

    tiny_image_destroy(&theirs);
    return failures;
}

/**
 * @brief A gamma is exactly the function magick's own -gamma applies.
 *
 * So the two agree to within the byte rounding, and the floor says so: this is
 * the case where a disagreement is a fault rather than a difference of method.
 */
static int gamma_correction(void) {
    int failures = 0;
    TinyImage image;

    if (!load(&image, "derived/base.png")) return 1;

    failures += assertEquals(tiny_image_gamma_correction(&image, 2.2f), 0);
    // byte identical, measured: the two apply the same power to the same
    // input and round it the same way
    failures +=
        compare("gamma 2.2", &image, "derived/ref/base-gamma.png", 90.0);

    tiny_image_destroy(&image);
    return failures;
}

/** A brightness change is a channel multiply, which is -evaluate multiply. */
static int brightness(void) {
    int failures = 0;
    TinyImage image;

    if (!load(&image, "derived/base.png")) return 1;

    failures += assertEquals(tiny_image_brightness(&image, 1.25f), 0);
    // byte identical, measured
    failures += compare(
        "brightness 1.25", &image, "derived/ref/base-brightness.png", 90.0
    );

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A contrast change about mid grey is an affine function of the channel.
 *
 * Which is what -function polynomial applies, so the two are the same
 * operation. magick's own -brightness-contrast is a sigmoid and is not.
 */
static int contrast(void) {
    int failures = 0;
    TinyImage image;

    if (!load(&image, "derived/base.png")) return 1;

    failures += assertEquals(tiny_image_contrast(&image, 1.4f), 0);
    // byte identical, measured
    failures +=
        compare("contrast 1.4", &image, "derived/ref/base-contrast.png", 90.0);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief Three box passes against a true gaussian.
 *
 * An approximation rather than the same operation, so the floor is lower and
 * is a stated one. Three boxes converge on a gaussian as the count rises; at
 * three the difference is concentrated in the tails.
 */
static int blur(void) {
    int failures = 0;
    TinyImage image;

    if (!load(&image, "derived/base.png")) return 1;

    failures += assertEquals(tiny_image_gaussian_blur(&image, 4.0f), 0);
    // 45.2 dB measured, which is closer than three box passes are usually
    // credited with: the floor is set just under it rather than at the 34 dB
    // an approximation argument would have guessed
    failures +=
        compare("gaussian sigma 4", &image, "derived/ref/base-blur.png", 44.0);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief The two resample filters against magick's own.
 *
 * Catmull-Rom for an enlargement and a box average for a reduction, which are
 * the two the planner picks between. Both are named filters magick implements,
 * so a disagreement past the floor is a fault in the sample map rather than a
 * different choice of kernel.
 */
static int resample(void) {
    int failures = 0;
    TinyImage image;

    if (!load(&image, "derived/base.png")) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    failures += assertEquals(
        tiny_plan_resize_with(&plan, 640, 360, TINYIMG_FILTER_CATMULL_ROM), 0
    );
    failures += assertEquals(tiny_plan_replace(&image, &plan), 0);

    // 103 dB, so the sample positions and the weights agree with magick's own
    // Catrom to the last bit that a byte can hold
    failures += compare(
        "catmull-rom to 640x360", &image, "derived/ref/base-catrom.png", 95.0
    );

    tiny_image_destroy(&image);
    if (!load(&image, "derived/base.png")) return failures + 1;

    tiny_plan_init_image(&plan, &image);
    failures += assertEquals(
        tiny_plan_resize_with(&plan, 80, 45, TINYIMG_FILTER_BOX), 0
    );
    failures += assertEquals(tiny_plan_replace(&image, &plan), 0);

    // byte identical: an area average over an integral ratio has one answer
    failures +=
        compare("box to 80x45", &image, "derived/ref/base-box.png", 90.0);

    tiny_image_destroy(&image);
    return failures;
}

/*
 * There is deliberately no fused-against-unfused case here. The two paths do
 * not agree and are not meant to: collapsing removes the intermediate clamps,
 * which is worth about 27 dB between them. Which of the two is closer to the
 * truth is settled in tests/c/plan/fuse.c against a double precision
 * reference, and repeating it here with a tuned floor would assert only that
 * the gap is the size it happens to be.
 */

int main(void) {
    int failures = 0;

    tiny_init();

    failures += gamma_correction();
    failures += brightness();
    failures += contrast();
    failures += blur();
    failures += resample();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
