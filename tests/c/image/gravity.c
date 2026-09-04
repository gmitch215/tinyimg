#include "test.h"
#include "tinyimg/detect.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The two computed gravities, and the face-targeted effects.
 *
 * These are the only operations in the library that read pixels to decide what
 * to do, so what is asserted is mostly what happens when the reading finds
 * nothing. The detector is allowed to fail; the fallback is not. A `gravity:
 * face` request with no cascade loaded, or on a photograph with no face in it,
 * has to produce exactly what `gravity: auto` would, with no error and no
 * degenerate crop.
 */

static int install(const char* id, const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "derived/cascades/%s.bin", name);

    size_t size = 0;
    unsigned char* bytes = readFixture(path, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    uint8_t* owned = (uint8_t*) tiny_alloc(size);
    if (!owned) {
        free(bytes);
        return TINYIMG_ERR_MEMORY;
    }

    memcpy(owned, bytes, size);
    free(bytes);

    return tiny_blob_load(TINYIMG_BLOB_CASCADE, id, owned, size);
}

static int loadScaled(const char* name, TinyImage* out, uint32_t box) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    memset(out, 0, sizeof(*out));
    int result = tiny_image_load_scaled(out, bytes, size, box, box);
    free(bytes);

    return result;
}

/** A synthetic image whose detail is all in one corner. */
static int cornerImage(TinyImage* out, uint32_t corner) {
    memset(out, 0, sizeof(*out));

    int result = tiny_image_create(out, 200, 200, 1);
    if (result != TINYIMG_OK) return result;

    // a flat gray field, so everything outside the patch weighs nothing
    memset(out->data, 128, (size_t) 200 * 200);

    uint32_t x0 = corner & 1u ? 140u : 20u;
    uint32_t y0 = corner & 2u ? 140u : 20u;

    // a checkerboard, which is the densest detail a byte image can hold
    for (uint32_t y = y0; y < y0 + 40u; y++) {
        for (uint32_t x = x0; x < x0 + 40u; x++) {
            out->data[(size_t) y * 200u + x] = ((x + y) & 1u) ? 255u : 0u;
        }
    }

    return TINYIMG_OK;
}

/** The detail centroid lands on the detail. */
static int autoFocus(void) {
    int failures = 0;

    tiny_blob_free_all();

    for (uint32_t corner = 0; corner < 4u; corner++) {
        TinyImage image;
        if (cornerImage(&image, corner) != TINYIMG_OK) return failures + 1;

        float x = -1.0f;
        float y = -1.0f;

        failures += assertEquals(
            tiny_image_focus(&image, TINYIMG_GRAVITY_AUTO, &x, &y), TINYIMG_OK
        );

        printf("corner %u -> (%.3f, %.3f): ", corner, (double) x, (double) y);

        // the patch center is at 0.2 or 0.8 of each axis, and the flat field
        // pulls the centroid nowhere, so the answer is on the patch's side of
        // the middle
        if (corner & 1u)
            failures += assertGreaterThan((double) x, 0.6);
        else
            failures += assertLessThan((double) x, 0.4);

        if (corner & 2u)
            failures += assertGreaterThan((double) y, 0.6);
        else
            failures += assertLessThan((double) y, 0.4);

        tiny_image_destroy(&image);
    }

    // a flat image has no detail to be the centroid of, and the middle is the
    // right answer
    TinyImage flat;
    memset(&flat, 0, sizeof(flat));
    tiny_image_create(&flat, 100, 100, 3);
    memset(flat.data, 90, (size_t) 100 * 100 * 3);

    float x = 0.0f;
    float y = 0.0f;
    failures += assertEquals(
        tiny_image_focus(&flat, TINYIMG_GRAVITY_AUTO, &x, &y), TINYIMG_OK
    );
    failures += assertFloatEquals(x, 0.5f, 0.0f);
    failures += assertFloatEquals(y, 0.5f, 0.0f);

    tiny_image_destroy(&flat);
    return failures;
}

/** The nine fixed gravities are arithmetic and read no pixels. */
static int fixedFocus(void) {
    int failures = 0;

    TinyImage image;
    if (cornerImage(&image, 0) != TINYIMG_OK) return 1;

    const struct {
        TinyImageGravity gravity;
        float x;
        float y;
    } cases[] = {
        {TINYIMG_GRAVITY_CENTER, 0.5f, 0.5f},
        {TINYIMG_GRAVITY_NORTH, 0.5f, 0.0f},
        {TINYIMG_GRAVITY_SOUTH, 0.5f, 1.0f},
        {TINYIMG_GRAVITY_WEST, 0.0f, 0.5f},
        {TINYIMG_GRAVITY_EAST, 1.0f, 0.5f},
        {TINYIMG_GRAVITY_NORTH_WEST, 0.0f, 0.0f},
        {TINYIMG_GRAVITY_NORTH_EAST, 1.0f, 0.0f},
        {TINYIMG_GRAVITY_SOUTH_WEST, 0.0f, 1.0f},
        {TINYIMG_GRAVITY_SOUTH_EAST, 1.0f, 1.0f}
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float x = -1.0f;
        float y = -1.0f;

        failures += assertEquals(
            tiny_image_focus(&image, cases[i].gravity, &x, &y), TINYIMG_OK
        );
        failures += assertFloatEquals(x, cases[i].x, 0.0f);
        failures += assertFloatEquals(y, cases[i].y, 0.0f);
    }

    failures += assertEquals(
        tiny_image_focus(0, TINYIMG_GRAVITY_AUTO, 0, 0), TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief Face gravity falls back to auto, and does so identically.
 *
 * Identically rather than approximately: the fallback is the auto answer, not a
 * second computation that happens to be close. Three ways to reach it, all of
 * which have to.
 */
static int faceFallback(void) {
    int failures = 0;

    const char* names[] = {"smile.jpg", "sf-24.jpg", "mountains.jpg"};

    // no cascade at all
    tiny_blob_free_all();

    for (unsigned i = 0; i < 3u; i++) {
        TinyImage image;
        if (loadScaled(names[i], &image, 400) != TINYIMG_OK)
            return failures + 1;

        float ax = 0.0f;
        float ay = 0.0f;
        float fx = 0.0f;
        float fy = 0.0f;

        failures += assertEquals(
            tiny_image_focus(&image, TINYIMG_GRAVITY_AUTO, &ax, &ay), TINYIMG_OK
        );
        failures += assertEquals(
            tiny_image_focus(&image, TINYIMG_GRAVITY_FACE, &fx, &fy), TINYIMG_OK
        );

        printf("%s with no cascade: ", names[i]);
        failures += assertFloatEquals(fx, ax, 0.0f);
        failures += assertFloatEquals(fy, ay, 0.0f);

        tiny_image_destroy(&image);
    }

    // a cascade loaded, on images it finds nothing in
    failures += assertEquals(install("frontal", "lbp-frontalface"), TINYIMG_OK);

    for (unsigned i = 1; i < 3u; i++) {
        TinyImage image;
        if (loadScaled(names[i], &image, 400) != TINYIMG_OK)
            return failures + 1;

        float ax = 0.0f;
        float ay = 0.0f;
        float fx = 0.0f;
        float fy = 0.0f;

        tiny_image_focus(&image, TINYIMG_GRAVITY_AUTO, &ax, &ay);
        tiny_image_focus(&image, TINYIMG_GRAVITY_FACE, &fx, &fy);

        printf("%s with a cascade and no face: ", names[i]);
        failures += assertFloatEquals(fx, ax, 0.0f);
        failures += assertFloatEquals(fy, ay, 0.0f);

        tiny_image_destroy(&image);
    }

    // and where there is a face, the two differ: the fallback is not being
    // taken
    TinyImage face;
    if (loadScaled("smile.jpg", &face, 4000) != TINYIMG_OK) return failures + 1;

    float ax = 0.0f;
    float ay = 0.0f;
    float fx = 0.0f;
    float fy = 0.0f;

    tiny_image_focus(&face, TINYIMG_GRAVITY_AUTO, &ax, &ay);
    tiny_image_focus(&face, TINYIMG_GRAVITY_FACE, &fx, &fy);

    printf(
        "smile.jpg auto (%.3f, %.3f) face (%.3f, %.3f): ", (double) ax,
        (double) ay, (double) fx, (double) fy
    );
    failures += assertTrue(fx != ax || fy != ay);

    // and the face answer is on the face, which is above and left of the middle
    // in this fixture
    failures += assertIn((double) fx, 0.3, 0.6);
    failures += assertIn((double) fy, 0.25, 0.55);

    tiny_image_destroy(&face);
    tiny_blob_free_all();
    return failures;
}

/**
 * @brief A computed gravity through the planner produces a different crop from
 * center.
 *
 * The plumbing worth asserting: tiny_plan_resolve is a pure function of the
 * plan, so the focus has to be answered before it and written onto the plan as
 * an operand. If that preflight did not run, the two computed gravities would
 * silently behave as center, which is what they do when no focus has been
 * resolved.
 */
static int plannedGravity(void) {
    int failures = 0;

    tiny_blob_free_all();
    install("frontal", "lbp-frontalface");
    install("profile", "lbp-profileface");

    size_t size = 0;
    unsigned char* bytes = readFixture("smile.jpg", &size);
    if (!bytes) return 1;

    uint64_t hashes[3];
    const TinyImageGravity gravities[3] = {
        TINYIMG_GRAVITY_CENTER, TINYIMG_GRAVITY_AUTO, TINYIMG_GRAVITY_FACE
    };

    for (unsigned i = 0; i < 3u; i++) {
        TinyPlan plan;
        failures +=
            assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
        failures += assertEquals(
            tiny_plan_fit(&plan, 300, 300, TINYIMG_FIT_COVER, gravities[i]),
            TINYIMG_OK
        );

        TinyImage out;
        memset(&out, 0, sizeof(out));

        failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
        failures += assertEquals(out.width, 300);
        failures += assertEquals(out.height, 300);

        failures +=
            assertEquals(tiny_image_phash(&out, &hashes[i]), TINYIMG_OK);
        tiny_image_destroy(&out);
    }

    // all three differ, so both computed gravities moved the crop
    printf("center vs auto: ");
    failures += assertNotEquals((long) hashes[0], (long) hashes[1]);
    printf("center vs face: ");
    failures += assertNotEquals((long) hashes[0], (long) hashes[2]);
    printf("auto vs face: ");
    failures += assertNotEquals((long) hashes[1], (long) hashes[2]);

    // and with no cascade, face gives exactly what auto gives
    tiny_blob_free_all();

    uint64_t fallback = 0;
    TinyPlan plan;
    tiny_plan_init(&plan, bytes, size);
    tiny_plan_fit(&plan, 300, 300, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_FACE);

    TinyImage out;
    memset(&out, 0, sizeof(out));
    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    tiny_image_phash(&out, &fallback);
    tiny_image_destroy(&out);

    printf("face with no cascade equals auto: ");
    failures += assertEquals((long) fallback, (long) hashes[1]);

    // resolution alone is unchanged by any of this: it is a function of the
    // plan, and a plan whose focus has not been answered still resolves
    TinyPlanResolution resolution;
    TinyPlan unresolved;
    tiny_plan_init(&unresolved, bytes, size);
    tiny_plan_fit(
        &unresolved, 300, 300, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_FACE
    );

    failures +=
        assertEquals(tiny_plan_resolve(&unresolved, &resolution), TINYIMG_OK);
    failures += assertEquals(resolution.width, 300);
    failures += assertEquals(resolution.height, 300);

    free(bytes);
    return failures;
}

/**
 * @brief The face-targeted effects change the face and leave the rest alone.
 *
 * And do nothing at all when there is no face, which is the case that matters:
 * an anonymizer that blurs the whole photograph when its detector fails is
 * worse than one that does nothing, because the failure is invisible in the
 * first output and obvious in the second.
 */
static int faceEffects(void) {
    int failures = 0;

    // no cascade: both are no-ops, byte for byte
    tiny_blob_free_all();

    for (unsigned i = 0; i < 2u; i++) {
        TinyImage image;
        TinyImage before;

        if (loadScaled("smile.jpg", &image, 500) != TINYIMG_OK)
            return failures + 1;
        if (loadScaled("smile.jpg", &before, 500) != TINYIMG_OK)
            return failures + 1;

        printf("%s with no cascade: ", i ? "pixelate_faces" : "blur_faces");
        failures += assertEquals(
            i ? tiny_image_pixelate_faces(&image, 0u)
              : tiny_image_blur_faces(&image, 0.0f),
            TINYIMG_OK
        );
        failures += assertImageEquals(&image, &before);

        tiny_image_destroy(&image);
        tiny_image_destroy(&before);
    }

    install("frontal", "lbp-frontalface");
    install("profile", "lbp-profileface");

    // a photograph with no face is also untouched
    TinyImage none;
    TinyImage noneBefore;
    if (loadScaled("mountains.jpg", &none, 500) != TINYIMG_OK)
        return failures + 1;
    if (loadScaled("mountains.jpg", &noneBefore, 500) != TINYIMG_OK) {
        return failures + 1;
    }

    printf("no face in the image: ");
    failures += assertEquals(tiny_image_blur_faces(&none, 0.0f), TINYIMG_OK);
    failures += assertImageEquals(&none, &noneBefore);

    tiny_image_destroy(&none);
    tiny_image_destroy(&noneBefore);

    // and where there is a face, only the face changes
    TinyFaceBox boxes[8];
    uint32_t count = 0;

    TinyImage image;
    TinyImage before;
    if (loadScaled("smile.jpg", &image, 4000) != TINYIMG_OK)
        return failures + 1;
    if (loadScaled("smile.jpg", &before, 4000) != TINYIMG_OK)
        return failures + 1;

    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 8u, &count), TINYIMG_OK
    );
    failures += assertGreaterThan((double) count, 0.0);

    failures += assertEquals(tiny_image_blur_faces(&image, 0.0f), TINYIMG_OK);

    size_t changedInside = 0;
    size_t changedOutside = 0;
    uint8_t channels = image.channels;

    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            size_t at = ((size_t) y * image.width + x) * channels;
            int differs = 0;

            for (uint8_t c = 0; c < channels; c++) {
                if (image.data[at + c] != before.data[at + c]) differs = 1;
            }

            if (!differs) continue;

            int inside = 0;
            for (uint32_t b = 0; b < count; b++) {
                if (x >= boxes[b].x && x < boxes[b].x + boxes[b].width &&
                    y >= boxes[b].y && y < boxes[b].y + boxes[b].height) {
                    inside = 1;
                }
            }

            if (inside)
                changedInside++;
            else
                changedOutside++;
        }
    }

    printf("changed inside the boxes: ");
    failures += assertGreaterThan((double) changedInside, 0.0);
    printf("nothing changed outside them: ");
    failures += assertEquals(changedOutside, 0);

    tiny_image_destroy(&image);
    tiny_image_destroy(&before);

    // a null image is an error rather than a no-op
    failures += assertEquals(tiny_image_blur_faces(0, 1.0f), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_pixelate_faces(0, 4u), TINYIMG_ERR_NULL);

    tiny_blob_free_all();
    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- auto focus --\n");
    failures += autoFocus();
    printf("-- fixed focus --\n");
    failures += fixedFocus();
    printf("-- face fallback --\n");
    failures += faceFallback();
    printf("-- planned gravity --\n");
    failures += plannedGravity();
    printf("-- face effects --\n");
    failures += faceEffects();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
