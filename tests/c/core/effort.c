#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/image.h"
#include "tinyimg/plan.h"

static int decodeAt(const char* name, TinyImage* image, uint8_t effort) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    TinyDecodeOpts opts = {0, 0, 0, 0, 1, 3, effort};
    int result = tiny_image_decode(image, bytes, size, &opts);

    free(bytes);
    return result;
}

/**
 * @brief Asserts a format decodes to the same bytes at either effort.
 *
 * Which is the whole contract for a lossless one: the pixels are defined
 * exactly, so there is no step to approximate and no reason for the two arms to
 * differ. A difference here means FAST reached code it had no business in.
 */
static int unaffected(const char* name) {
    int failures = 0;
    TinyImage fancy;
    TinyImage fast;

    printf("  %s: identical at either effort\n", name);

    if (decodeAt(name, &fancy, TINYIMG_EFFORT_FANCY) != TINYIMG_OK) return 1;
    if (decodeAt(name, &fast, TINYIMG_EFFORT_FAST) != TINYIMG_OK) {
        tiny_image_destroy(&fancy);
        return 1;
    }

    failures += assertImageEquals(&fancy, &fast);

    tiny_image_destroy(&fancy);
    tiny_image_destroy(&fast);
    return failures;
}

/**
 * @brief Asserts a lossy format's two arms differ, and by how little.
 *
 * Both halves matter. Identical output would mean the lever is not wired up,
 * and a floor breach would mean it gave away more than it was measured to.
 *
 * @param name Fixture path.
 * @param floor_db The floor the agreement has to clear.
 */
static int approximated(const char* name, double floor_db) {
    int failures = 0;
    TinyImage fancy;
    TinyImage fast;

    printf("  %s: within %.0f dB, and not identical\n", name, floor_db);

    if (decodeAt(name, &fancy, TINYIMG_EFFORT_FANCY) != TINYIMG_OK) return 1;
    if (decodeAt(name, &fast, TINYIMG_EFFORT_FAST) != TINYIMG_OK) {
        tiny_image_destroy(&fancy);
        return 1;
    }

    size_t n = (size_t) fancy.width * fancy.height * fancy.channels;

    failures += assertEquals((long) fast.width, (long) fancy.width);
    failures += assertEquals((long) fast.height, (long) fancy.height);
    failures += assertPSNR(fancy.data, fast.data, n, floor_db);

    // a lever that produced identical pixels would pass the floor while doing
    // nothing, so the difference is asserted too
    failures += assertTrue(computePSNR(fancy.data, fast.data, n) != INFINITY);

    tiny_image_destroy(&fancy);
    tiny_image_destroy(&fast);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    /*
     * Only a lossy decoder has anything to trade, and that is a property of the
     * formats rather than a gap in the implementation. A lossless bitstream
     * defines its pixels exactly, so every step is required to produce them.
     *
     * Floors are measured, not chosen: 46.75 dB on the lossy WebP and 43.64 on
     * the 4:2:0 JPEG, held at 40 so the assertion is about the lever staying
     * within its class rather than about the exact number.
     */
    printf("lossy formats, where a smoothing pass can be dropped\n");

    // vp8 skips deblocking, worth 1.53x
    failures += approximated("toyota_racing.webp", 40.0);
    failures += approximated("derived/base-lossy.webp", 40.0);

    // jpeg replicates chroma instead of interpolating it, worth 1.11x to 1.25x
    failures += approximated("road.jpg", 40.0);
    failures += approximated("mountains.jpg", 40.0);

    printf("\nlossless formats, where there is nothing to drop\n");
    failures += unaffected("derived/base-lossless.webp");
    failures += unaffected("forest.png");
    failures += unaffected("ball_kick.gif");
    failures += unaffected("dartmouth.tiff");

    /*
     * A 4:4:4 JPEG carries chroma at full resolution, so the upsampler is the
     * identity and the only lossy format in the set decodes the same either
     * way. sf-24.jpg is the fixture every other number in this repository is
     * quoted on, and it is the one JPEG that cannot show this lever at all.
     */
    printf("\na jpeg with no chroma to upsample\n");
    failures += unaffected("sf-24.jpg");
    failures += unaffected("derived/base-444.jpg");

    /*
     * The resample filter is inspectable, so this asserts the resolution rather
     * than the pixels: two filters can produce a similar image and only one of
     * them was the one asked for.
     *
     * A reduction is the same at either effort because AUTO already picks the
     * cheap filter for it. The lever recorded as the largest untaken one was
     * box over the cubic at 5.3x per sample, which is a comparison the planner
     * never makes: it picks box for every reduction already. What is actually
     * reachable is enlargement, where box is 35.7 dB and blocky, so FAST steps
     * to bilinear at 3.2x and 45.7 dB instead.
     */
    printf("\nthe filter AUTO picks\n");

    TinyImage big;
    if (tiny_image_create(&big, 64, 64, 3) == TINYIMG_OK) {
        static const struct {
            const char* what;
            uint32_t extent;
            uint8_t effort;
            TinyResampleFilter expected;
        } cases[] = {
            {"enlarge, fancy", 128, TINYIMG_EFFORT_FANCY,
             TINYIMG_FILTER_CATMULL_ROM},
            {"enlarge, fast", 128, TINYIMG_EFFORT_FAST,
             TINYIMG_FILTER_BILINEAR},
            {"reduce, fancy", 32, TINYIMG_EFFORT_FANCY, TINYIMG_FILTER_BOX},
            {"reduce, fast", 32, TINYIMG_EFFORT_FAST, TINYIMG_FILTER_BOX}
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            TinyPlan plan;
            TinyPlanResolution res;

            tiny_plan_init_image(&plan, &big);
            tiny_plan_set_effort(&plan, cases[i].effort);
            tiny_plan_resize(&plan, cases[i].extent, cases[i].extent);

            printf("  %s\n", cases[i].what);
            failures +=
                assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);
            failures +=
                assertEquals((long) res.filter_x, (long) cases[i].expected);
            failures +=
                assertEquals((long) res.filter_y, (long) cases[i].expected);
        }

        // an explicit filter is what the caller asked for, so effort leaves it
        TinyPlan explicit_plan;
        TinyPlanResolution explicit_res;

        tiny_plan_init_image(&explicit_plan, &big);
        tiny_plan_set_effort(&explicit_plan, TINYIMG_EFFORT_FAST);
        tiny_plan_resize_with(
            &explicit_plan, 128, 128, TINYIMG_FILTER_CATMULL_ROM
        );

        printf("  an explicit catmull-rom survives fast\n");
        failures += assertEquals(
            tiny_plan_resolve(&explicit_plan, &explicit_res), TINYIMG_OK
        );
        failures += assertEquals(
            (long) explicit_res.filter_x, (long) TINYIMG_FILTER_CATMULL_ROM
        );

        tiny_image_destroy(&big);
    }
    else {
        failures += assertTrue(0);
    }

    printf("\nthe setting itself\n");

    TinyImage image;
    if (tiny_image_create(&image, 8, 8, 3) == TINYIMG_OK) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &image);

        // fancy is the default, so a plan nobody configured decodes exactly
        failures +=
            assertEquals((long) plan.effort, (long) TINYIMG_EFFORT_FANCY);

        failures += assertEquals(
            tiny_plan_set_effort(&plan, TINYIMG_EFFORT_FAST), TINYIMG_OK
        );
        failures +=
            assertEquals((long) plan.effort, (long) TINYIMG_EFFORT_FAST);

        failures +=
            assertEquals(tiny_plan_set_effort(&plan, 2), TINYIMG_ERR_RANGE);
        failures += assertEquals(tiny_plan_set_effort(0, 0), TINYIMG_ERR_NULL);

        // and the rejected value left the plan alone
        failures +=
            assertEquals((long) plan.effort, (long) TINYIMG_EFFORT_FAST);

        tiny_image_destroy(&image);
    }
    else {
        failures += assertTrue(0);
    }

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
