#include "../test.h"
#include "tinyimg/memory.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The upper bound a plan's peak heap use is held under.
 *
 * The bound is read before anything runs, so the property that carries it is
 * that it is never below what a run measures. Every cell here runs the plan
 * on a fresh heap and compares against `tiny_heap_stats().peak`; a cell that
 * comes out under is a defect, and one that comes out far over is only a cost.
 * The shape tests need no fixture and pin the structure of the bound rather
 * than its constants.
 */

#define HEAP_BYTES (64u << 20)

static unsigned char* heap;

typedef enum
{
    SCENARIO_NONE,
    SCENARIO_RESIZE_400,
    SCENARIO_RESIZE_200,
    SCENARIO_CROP_RESIZE,
    SCENARIO_ROTATE,
    SCENARIO_BLUR,
    SCENARIO_GRAY,
    SCENARIO_AFTER_BLUR,
    SCENARIO_COUNT
} Scenario;

#define BIT(s) (1u << (s))
#define ALL (BIT(SCENARIO_COUNT) - 1u)
#define SIX (BIT(SCENARIO_BLUR + 1) - 1u)
#define PLAIN (BIT(SCENARIO_NONE))
#define SCALED (BIT(SCENARIO_NONE) | BIT(SCENARIO_RESIZE_200))
#define REGION (SCALED | BIT(SCENARIO_CROP_RESIZE))

typedef struct {
    const char* name;
    /** Non-zero when the bound is charged a worst case the file may not hold.
     */
    int loose;
    /** The scenarios run on it, which are the ones that differ for its format.
     */
    unsigned scenarios;
} Fixture;

static const Fixture fixtures[] = {
    {"derived/base-alpha.png", 0, ALL},
    {"derived/base-interlaced.png", 0, SCALED},
    {"derived/base-palette.png", 0, PLAIN},
    {"derived/base-rgb16.png", 0, PLAIN},
    {"derived/single-pixel.png", 0, PLAIN},
    {"derived/base-420.jpg", 0, ALL},
    {"derived/base-422.jpg", 0, REGION},
    {"derived/base-444.jpg", 0, REGION},
    {"derived/base-gray.jpg", 0, SCALED},
    {"derived/base-cmyk.jpg", 0, PLAIN},
    {"derived/base-progressive.jpg", 0, SCALED},
    {"derived/tiny-odd.jpg", 0, SCALED},
    {"derived/base.bmp", 0, SCALED},
    {"derived/base-rle8.bmp", 0, SCALED},
    {"derived/base.gif", 0, SCALED},
    {"derived/base-interlaced.gif", 0, PLAIN},
    {"derived/base-gray.tif", 1, PLAIN},
    {"derived/base-lzw.tif", 1, PLAIN},
    {"derived/base-deflate.tif", 1, SCALED},
    {"derived/base-lossy.webp", 0, SIX},
    {"derived/base-lossless.webp", 1, SCALED},
    {"derived/base-lossy-alpha.webp", 1, SCALED},
    {"derived/base-raw-alpha.webp", 0, PLAIN},
    {"derived/tiny-odd-lossy.webp", 0, SCALED},
    {"derived/base.avif", 0, SCALED},
    {"derived/base-alpha.avif", 0, PLAIN},
    {"derived/av1-lossless.avif", 0, PLAIN},
    {"derived/av1-tiny.avif", 0, PLAIN},
    {"dartmouth.jpg", 0, REGION},
    {"sf-24.jpg", 0, BIT(SCENARIO_RESIZE_200)},
    {"toyota_racing.webp", 0, BIT(SCENARIO_RESIZE_200)},
};

/** The most a bound that is not marked loose may exceed what was measured. */
#define TIGHT_RATIO 4.0

static void build(
    TinyPlan* plan, Scenario scenario, uint32_t width, uint32_t height
) {
    uint32_t half_w = width / 2u > 0u ? width / 2u : 1u;
    uint32_t half_h = height / 2u > 0u ? height / 2u : 1u;

    switch (scenario) {
        case SCENARIO_RESIZE_400: tiny_plan_resize(plan, 400u, 0u); break;
        case SCENARIO_RESIZE_200: tiny_plan_resize(plan, 200u, 0u); break;
        case SCENARIO_CROP_RESIZE:
            tiny_plan_crop(plan, width / 4u, height / 4u, half_w, half_h);
            tiny_plan_resize(plan, 300u, 0u);
            break;
        case SCENARIO_ROTATE: tiny_plan_rotate(plan, 90); break;
        case SCENARIO_BLUR:
            tiny_plan_resize(plan, 400u, 0u);
            tiny_plan_gaussian_blur(plan, 3.0f);
            break;
        case SCENARIO_GRAY:
            tiny_plan_resize(plan, 200u, 0u);
            tiny_plan_grayscale(plan);
            break;
        case SCENARIO_AFTER_BLUR:
            tiny_plan_resize(plan, 300u, 0u);
            tiny_plan_gaussian_blur(plan, 2.0f);
            tiny_plan_resize(plan, 120u, 0u);
            tiny_plan_rotate(plan, 90);
            break;
        default: break;
    }
}

static int lossy(const char* name) {
    size_t length = strlen(name);

    return strcmp(name + length - 4u, ".jpg") == 0 ||
           strcmp(name + length - 5u, ".webp") == 0;
}

/** Runs the plan on a fresh heap and returns the peak it reached. */
static int measure(const TinyPlan* plan, size_t* peak) {
    tiny_heap_init(heap, HEAP_BYTES);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    int result = tiny_plan_run(plan, &out);

    TinyHeapStats stats;
    tiny_heap_stats(&stats);
    *peak = stats.peak;

    return result;
}

/** One cell: bound against measurement, and the tightness it is held to. */
static int checkCell(const TinyPlan* plan, int loose) {
    int failures = 0;

    TinyPlanResolution resolution;
    if (tiny_plan_resolve(plan, &resolution) != TINYIMG_OK) return 0;

    size_t bound = 0;
    failures += assertEquals(
        tiny_plan_peak_bytes(plan, &resolution, &bound), TINYIMG_OK
    );

    size_t peak = 0;
    if (measure(plan, &peak) != TINYIMG_OK) return failures;

    if (bound < peak) {
        printf("bound %zu is under the measured peak %zu\n", bound, peak);
        failures += 1;
    }

    if (!loose && (double) bound > TIGHT_RATIO * (double) peak) {
        printf(
            "bound %zu is over %.1fx the measured peak %zu\n", bound,
            TIGHT_RATIO, peak
        );
        failures += 1;
    }

    return failures;
}

static int coversMeasuredPeakAcrossFixtures(void) {
    int failures = 0;

    for (size_t f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); f++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(fixtures[f].name, &size);
        if (!bytes) {
            failures += 1;
            continue;
        }

        TinyPlan probe;
        if (tiny_plan_init(&probe, bytes, size) != TINYIMG_OK) {
            free(bytes);
            failures += 1;
            continue;
        }

        for (int s = 0; s < SCENARIO_COUNT; s++) {
            if (!(fixtures[f].scenarios & BIT(s))) continue;

            // only the two lossy decoders have anything for effort to drop
            uint8_t efforts = lossy(fixtures[f].name) ? 2u : 1u;

            for (uint8_t effort = 0; effort < efforts; effort++) {
                TinyPlan plan;
                tiny_plan_init(&plan, bytes, size);
                build(
                    &plan, (Scenario) s, probe.source_width, probe.source_height
                );
                tiny_plan_set_effort(&plan, effort);

                failures += checkCell(&plan, fixtures[f].loose);

                // the ladder's own pick at the smallest budget, which is the
                // cell that reaches the deepest scale; a plan with no resample
                // has nothing for it to give up
                int resamples = s == SCENARIO_RESIZE_400 ||
                                s == SCENARIO_RESIZE_200 ||
                                s == SCENARIO_CROP_RESIZE;
                if (resamples) {
                    tiny_plan_set_budget(&plan, 1u);
                    failures += checkCell(&plan, fixtures[f].loose);
                }
            }
        }

        free(bytes);
    }

    return failures;
}

static int coversTheUnfusedPath(void) {
    int failures = 0;
    const char* names[] = {
        "derived/base-420.jpg", "derived/base.png", "derived/base-lossy.webp",
        "derived/base.avif"
    };

    for (size_t f = 0; f < 4u; f++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(names[f], &size);
        if (!bytes) {
            failures += 1;
            continue;
        }

        TinyPlan plan;
        tiny_plan_init(&plan, bytes, size);
        tiny_plan_set_fusion(&plan, 0);
        tiny_plan_resize(&plan, 150u, 0u);
        tiny_plan_gaussian_blur(&plan, 2.0f);
        tiny_plan_rotate(&plan, 90);

        failures += checkCell(&plan, 0);
        free(bytes);
    }

    return failures;
}

static int coversFocusResolution(void) {
    int failures = 0;
    const char* names[] = {"dartmouth.jpg"};

    for (size_t f = 0; f < 1u; f++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(names[f], &size);
        if (!bytes) {
            failures += 1;
            continue;
        }

        TinyImageGravity gravity[2] = {
            TINYIMG_GRAVITY_AUTO, TINYIMG_GRAVITY_FACE
        };

        for (int g = 0; g < 2; g++) {
            TinyPlan plan;
            tiny_plan_init(&plan, bytes, size);
            tiny_plan_fit(&plan, 120u, 120u, TINYIMG_FIT_COVER, gravity[g]);

            failures += checkCell(&plan, 0);
        }

        free(bytes);
    }

    return failures;
}

static int coversEveryEffectKind(void) {
    int failures = 0;

    uint32_t extents[2][3] = {{300u, 200u, 3u}, {61u, 49u, 4u}};
    float params[2][4] = {{3.0f, 1.0f, 0.0f, 0.0f}, {9.0f, 2.0f, 0.2f, 0.0f}};

    for (int e = 0; e < 2; e++) {
        for (int kind = 0; kind <= TINYIMG_FX_SCANLINES; kind++) {
            for (int p = e; p <= e; p++) {
                tiny_heap_init(heap, HEAP_BYTES);

                TinyImage source;
                memset(&source, 0, sizeof(source));
                failures += assertEquals(
                    tiny_image_create(
                        &source, extents[e][0], extents[e][1],
                        (uint8_t) extents[e][2]
                    ),
                    TINYIMG_OK
                );

                size_t bytes =
                    (size_t) source.width * source.height * source.channels;
                for (size_t i = 0; i < bytes; i++) {
                    source.data[i] = (uint8_t) (i * 31u + (i >> 7));
                }

                TinyPlan plan;
                tiny_plan_init_image(&plan, &source);

                int appended =
                    kind == TINYIMG_FX_BLUR_REGION ||
                            kind == TINYIMG_FX_PIXELATE_REGION
                        ? tiny_plan_effect_rect(
                              &plan, (TinyEffectKind) kind, params[p], 8u, 8u,
                              source.width / 2u, source.height / 2u
                          )
                        : tiny_plan_effect(
                              &plan, (TinyEffectKind) kind, params[p]
                          );
                if (appended != TINYIMG_OK) continue;

                TinyPlanResolution resolution;
                failures += assertEquals(
                    tiny_plan_resolve(&plan, &resolution), TINYIMG_OK
                );

                size_t bound = 0;
                failures += assertEquals(
                    tiny_plan_peak_bytes(&plan, &resolution, &bound), TINYIMG_OK
                );

                // the source was allocated before the run, so the heap's peak
                // already counts it; the bound counts only what the run adds
                // beside the decoded image, and a borrowed source is not one
                TinyHeapStats before;
                tiny_heap_stats(&before);

                TinyImage out;
                memset(&out, 0, sizeof(out));
                failures +=
                    assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);

                TinyHeapStats stats;
                tiny_heap_stats(&stats);

                size_t added = stats.peak - before.used;
                if (bound < added) {
                    printf(
                        "effect %d: bound %zu is under %zu\n", kind, bound,
                        added
                    );
                    failures += 1;
                }
            }
        }
    }

    return failures;
}

static int addsNothingForAnInMemorySource(void) {
    int failures = 0;

    tiny_heap_init(heap, HEAP_BYTES);

    TinyImage source;
    memset(&source, 0, sizeof(source));
    failures +=
        assertEquals(tiny_image_create(&source, 2000u, 1500u, 3u), TINYIMG_OK);
    memset(source.data, 90, (size_t) 2000u * 1500u * 3u);

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init_image(&plan, &source), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 50u, 0u), TINYIMG_OK);

    size_t bound = 0;
    failures +=
        assertEquals(tiny_plan_peak_bytes(&plan, 0, &bound), TINYIMG_OK);

    // a 50x38 output and its tap tables: nothing close to the 9 MB source,
    // which an encoded source's decode term would have included
    failures += assertGreaterThan((double) bound, 50.0 * 38.0 * 3.0);
    failures += assertLessThan((double) bound, 1024.0 * 1024.0);

    tiny_image_destroy(&source);
    return failures;
}

static int neverShrinksWhenAnOperationIsAdded(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    for (int added = 0; added < 6; added++) {
        TinyPlan base;
        tiny_plan_init(&base, bytes, size);
        tiny_plan_resize(&base, 500u, 0u);

        TinyPlan more = base;

        switch (added) {
            case 0: tiny_plan_brightness(&more, 1.2f); break;
            case 1: tiny_plan_contrast(&more, 1.1f); break;
            case 2: tiny_plan_rotate(&more, 90); break;
            case 3: tiny_plan_flip_horizontal(&more); break;
            case 4: tiny_plan_gaussian_blur(&more, 2.0f); break;
            default: {
                float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                tiny_plan_effect(&more, TINYIMG_FX_MEDIAN, p);
                break;
            }
        }

        size_t before = 0;
        size_t after = 0;
        failures +=
            assertEquals(tiny_plan_peak_bytes(&base, 0, &before), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_peak_bytes(&more, 0, &after), TINYIMG_OK);

        if (after < before) {
            printf(
                "operation %d lowered the bound from %zu to %zu\n", added,
                before, after
            );
            failures += 1;
        }
    }

    free(bytes);
    return failures;
}

static int fallsAsTheJpegDecodeShrinks(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    // widths that land the scale ladder on 1, 2, 4 and 8
    uint32_t widths[4] = {1835u, 900u, 450u, 200u};
    uint8_t expected[4] = {1u, 2u, 4u, 8u};
    size_t previous = 0;
    size_t first = 0;
    size_t last = 0;

    for (int i = 0; i < 4; i++) {
        TinyPlan plan;
        tiny_plan_init(&plan, bytes, size);
        tiny_plan_resize(&plan, widths[i], 0u);

        TinyPlanResolution resolution;
        failures +=
            assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);
        failures += assertEquals(
            (long) resolution.decode.scale_den, (long) expected[i]
        );

        size_t bound = 0;
        failures += assertEquals(
            tiny_plan_peak_bytes(&plan, &resolution, &bound), TINYIMG_OK
        );

        if (i > 0 && bound > previous) {
            printf(
                "bound rose from %zu to %zu at denominator %d\n", previous,
                bound, expected[i]
            );
            failures += 1;
        }

        if (i == 0) first = bound;
        last = bound;
        previous = bound;
    }

    // not merely non-increasing: the eighth-scale decode holds a fraction
    failures += assertLessThan((double) last, (double) first / 8.0);

    free(bytes);
    return failures;
}

static int growsWithTheProgressiveCoefficientPlane(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base-progressive.jpg", &size);
    size_t flat_size = 0;
    unsigned char* flat = readFixture("derived/base-420.jpg", &flat_size);
    if (!bytes || !flat) return 1;

    TinyPlan progressive;
    TinyPlan baseline;
    tiny_plan_init(&progressive, bytes, size);
    tiny_plan_init(&baseline, flat, flat_size);

    size_t with = 0;
    size_t without = 0;
    failures +=
        assertEquals(tiny_plan_peak_bytes(&progressive, 0, &with), TINYIMG_OK);
    failures +=
        assertEquals(tiny_plan_peak_bytes(&baseline, 0, &without), TINYIMG_OK);

    // 320x180 held as 16-bit coefficients is 345,600 bytes at 4:2:0 and the
    // two fixtures share their extent
    failures += assertGreaterThan((double) with, (double) without + 300000.0);

    free(bytes);
    free(flat);
    return failures;
}

static int predictsTheHeapCeiling(void) {
    int failures = 0;

    // an image the byte cap admits (3344 squared of RGB is 33.5 MB), whose
    // plan holds an equal output beside the source it borrows. The bound
    // counts what the run adds, so the source and the bound together cross the
    // heap exactly where the run fails to allocate
    tiny_heap_init(heap, HEAP_BYTES);

    TinyImage huge;
    memset(&huge, 0, sizeof(huge));
    failures +=
        assertEquals(tiny_image_create(&huge, 3344u, 3344u, 3u), TINYIMG_OK);

    TinyPlan over;
    failures += assertEquals(tiny_plan_init_image(&over, &huge), TINYIMG_OK);
    failures += assertEquals(tiny_plan_rotate(&over, 90), TINYIMG_OK);

    size_t bound = 0;
    size_t held = (size_t) 3344u * 3344u * 3u;
    failures +=
        assertEquals(tiny_plan_peak_bytes(&over, 0, &bound), TINYIMG_OK);
    failures +=
        assertGreaterThan((double) (held + bound), (double) TINYIMG_HEAP_MAX);

    TinyImage refused;
    memset(&refused, 0, sizeof(refused));
    failures +=
        assertEquals(tiny_plan_run(&over, &refused), TINYIMG_ERR_MEMORY);

    // 1600 squared fits with room to spare, and the same bound holds the run
    tiny_heap_init(heap, HEAP_BYTES);

    TinyImage fits;
    memset(&fits, 0, sizeof(fits));
    failures +=
        assertEquals(tiny_image_create(&fits, 1600u, 1600u, 3u), TINYIMG_OK);
    memset(fits.data, 40, (size_t) 1600u * 1600u * 3u);
    held = (size_t) 1600u * 1600u * 3u;

    TinyPlan ok;
    failures += assertEquals(tiny_plan_init_image(&ok, &fits), TINYIMG_OK);
    failures += assertEquals(tiny_plan_rotate(&ok, 90), TINYIMG_OK);
    failures += assertEquals(tiny_plan_peak_bytes(&ok, 0, &bound), TINYIMG_OK);
    failures +=
        assertLessThan((double) (held + bound), (double) TINYIMG_HEAP_MAX);

    TinyImage out;
    memset(&out, 0, sizeof(out));
    failures += assertEquals(tiny_plan_run(&ok, &out), TINYIMG_OK);

    TinyHeapStats stats;
    tiny_heap_stats(&stats);
    failures += assertTrue(held + bound >= stats.peak);

    return failures;
}

static int rejectsMissingArguments(void) {
    int failures = 0;

    size_t bound = 0;
    TinyPlan plan;
    memset(&plan, 0, sizeof(plan));

    failures +=
        assertEquals(tiny_plan_peak_bytes(0, 0, &bound), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_plan_peak_bytes(&plan, 0, 0), TINYIMG_ERR_NULL);

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();
    heap = (unsigned char*) malloc(HEAP_BYTES);
    if (!heap) return 1;

    failures += rejectsMissingArguments();
    failures += addsNothingForAnInMemorySource();
    failures += fallsAsTheJpegDecodeShrinks();
    failures += growsWithTheProgressiveCoefficientPlane();
    failures += neverShrinksWhenAnOperationIsAdded();
    failures += predictsTheHeapCeiling();
    failures += coversEveryEffectKind();
    failures += coversTheUnfusedPath();
    failures += coversFocusResolution();
    failures += coversMeasuredPeakAcrossFixtures();

    free(heap);

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
