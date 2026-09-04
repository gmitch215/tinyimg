#include "tinyimg/plan.h"

#include "test.h"

static int build(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;

    // the header is read at init, so the source extent is known before a single
    // pixel has been decoded
    failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
    failures += assertEquals(plan.source_width, 1835);
    failures += assertEquals(plan.source_height, 1032);
    failures += assertEquals(plan.source_format, TINYIMG_FORMAT_JPEG);
    failures += assertEquals(tiny_plan_count(&plan), 0);
    failures += assertEquals(plan.fusion, 1);

    failures += assertEquals(tiny_plan_crop(&plan, 10, 20, 30, 40), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 100, 0), TINYIMG_OK);
    failures += assertEquals(tiny_plan_brightness(&plan, 1.2f), TINYIMG_OK);
    failures += assertEquals(tiny_plan_count(&plan), 3);

    TinyPlanOp op;
    failures += assertEquals(tiny_plan_op_at(&plan, 0, &op), TINYIMG_OK);
    failures += assertEquals(op.kind, TINYIMG_OP_CROP);
    failures += assertEquals(op.crop.x, 10);
    failures += assertEquals(op.crop.y, 20);
    failures += assertEquals(op.crop.width, 30);
    failures += assertEquals(op.crop.height, 40);

    failures += assertEquals(tiny_plan_op_at(&plan, 1, &op), TINYIMG_OK);
    failures += assertEquals(op.kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(op.resize.width, 100);
    failures += assertEquals(op.resize.height, 0);
    failures += assertEquals(op.resize.filter, TINYIMG_FILTER_AUTO);

    failures += assertEquals(tiny_plan_op_at(&plan, 2, &op), TINYIMG_OK);
    failures += assertEquals(op.kind, TINYIMG_OP_BRIGHTNESS);
    failures += assertFloatEquals(op.scalar.value, 1.2f, 1e-6f);

    failures +=
        assertEquals(tiny_plan_op_at(&plan, 3, &op), TINYIMG_ERR_BOUNDS);

    free(bytes);
    return failures;
}

static int capacity(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);

    for (uint32_t i = 0; i < TINYIMG_PLAN_MAX_OPS; i++) {
        if (tiny_plan_invert(&plan) != TINYIMG_OK) failures++;
    }

    failures += assertEquals(tiny_plan_count(&plan), TINYIMG_PLAN_MAX_OPS);

    // a chain longer than the capacity is refused rather than silently dropped
    failures += assertEquals(tiny_plan_invert(&plan), TINYIMG_ERR_PLAN);
    failures +=
        assertEquals(tiny_plan_crop(&plan, 0, 0, 1, 1), TINYIMG_ERR_PLAN);

    tiny_image_destroy(&image);
    return failures;
}

static int turns(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return 1;

    struct {
        int32_t degrees;
        uint32_t turns;
    } cases[] = {{0, 0},    {90, 1},   {180, 2},  {270, 3},
                 {360, 0},  {450, 1},  {-90, 3},  {-180, 2},
                 {-270, 1}, {-360, 0}, {-450, 3}, {1080, 0}};

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &image);

        failures +=
            assertEquals(tiny_plan_rotate(&plan, cases[i].degrees), TINYIMG_OK);

        TinyPlanOp op;
        tiny_plan_op_at(&plan, 0, &op);
        failures += assertEquals(op.rotate.turns, cases[i].turns);
    }

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);
    failures += assertEquals(tiny_plan_rotate(&plan, 45), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_plan_rotate(&plan, -1), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

static int ranges(void) {
    int failures = 0;

    TinyImage image;
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);

    failures +=
        assertEquals(tiny_plan_brightness(&plan, -0.1f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_plan_contrast(&plan, -1.0f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_plan_saturation(&plan, -1.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_plan_gamma(&plan, 0.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_plan_gamma(&plan, -2.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_plan_blur(&plan, -1.0f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_plan_gaussian_blur(&plan, -1.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_plan_resize(&plan, 0, 0), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_plan_fit(&plan, 0, 10, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_plan_fit(&plan, 10, 0, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER),
        TINYIMG_ERR_RANGE
    );

    // zero in one axis of a resize is not a range error, it asks for the aspect
    // ratio to be kept
    failures += assertEquals(tiny_plan_resize(&plan, 0, 4), TINYIMG_OK);
    failures += assertEquals(tiny_plan_count(&plan), 1);

    failures += assertEquals(tiny_plan_init(&plan, 0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_init_image(&plan, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_crop(0, 0, 0, 1, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_count(0), 0);
    failures += assertEquals(tiny_plan_op_at(&plan, 0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_resolve(0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_run(0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_set_fusion(0, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_background(0, 0), TINYIMG_ERR_NULL);

    // a plan with neither a buffer nor an image has no source to run over
    TinyPlan empty;
    memset(&empty, 0, sizeof(empty));
    TinyImage out;
    failures += assertEquals(tiny_plan_run(&empty, &out), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

static int classes(void) {
    int failures = 0;

    struct {
        TinyPlanOpKind kind;
        TinyPlanOpClass expected;
    } cases[] = {
        {TINYIMG_OP_CROP, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_RESIZE, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_FIT, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_FLIP_H, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_FLIP_V, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_ROTATE, TINYIMG_OP_CLASS_GEOMETRY},
        {TINYIMG_OP_BRIGHTNESS, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_CONTRAST, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_SATURATION, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_HUE, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_GRAYSCALE, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_INVERT, TINYIMG_OP_CLASS_COLOR_MATRIX},
        {TINYIMG_OP_GAMMA, TINYIMG_OP_CLASS_COLOR_LUT},
        {TINYIMG_OP_BLUR, TINYIMG_OP_CLASS_NEIGHBORHOOD},
        {TINYIMG_OP_NONE, TINYIMG_OP_CLASS_GEOMETRY}
    };

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failures +=
            assertEquals(tiny_plan_op_class(cases[i].kind), cases[i].expected);
    }

    // gamma is the only operation in the library that is not affine in its
    // channels, and the class is what tells the executor it cannot move across
    // a resample
    failures += assertNotEquals(
        tiny_plan_op_class(TINYIMG_OP_GAMMA),
        tiny_plan_op_class(TINYIMG_OP_BRIGHTNESS)
    );

    return failures;
}

static int sizes(void) {
    int failures = 0;

    failures += assertEquals(tiny_plan_sizeof(), sizeof(TinyPlan));
    failures +=
        assertEquals(tiny_plan_resolution_sizeof(), sizeof(TinyPlanResolution));

    // both are meant to sit on a stack, so a capacity change that made either
    // of them enormous should be noticed here rather than in a workerd trap
    failures += assertLessThan(sizeof(TinyPlan), 4096);
    failures += assertLessThan(sizeof(TinyPlanResolution), 4096);

    return failures;
}

int main(void) {
    int failures = 0;

    failures += build();
    failures += capacity();
    failures += turns();
    failures += ranges();
    failures += classes();
    failures += sizes();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
