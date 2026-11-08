#include "../test.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The two-term cost model and its calibrated upper bound.
 *
 * Every assertion is about the model's shape and none compares against a
 * clock: the rates were timed on one machine, so a gate that held a live
 * timing to them would fail on a machine of a different speed. What does not
 * depend on speed is the ordering the model has to keep, and that is what is
 * gated here.
 */

static const struct {
    const char* name;
    TinyImageFormat format;
} fixtures[] = {
    {"derived/base-420.jpg", TINYIMG_FORMAT_JPEG},
    {"derived/base.png", TINYIMG_FORMAT_PNG},
    {"derived/base-lossy.webp", TINYIMG_FORMAT_WEBP},
    {"derived/base.gif", TINYIMG_FORMAT_GIF},
    {"derived/base-lzw.tif", TINYIMG_FORMAT_TIFF},
    {"derived/base.bmp", TINYIMG_FORMAT_BMP},
    {"derived/base.avif", TINYIMG_FORMAT_AVIF},
};

#define FIXTURES (sizeof(fixtures) / sizeof(*fixtures))

static int resolveWhole(
    const unsigned char* bytes, size_t size, TinyPlanResolution* out,
    TinyPlan* plan
) {
    int result = tiny_plan_init(plan, bytes, size);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_resolve(plan, out);
}

/** The bound is never below the point estimate, for every format. */
static int upperIsNeverBelowThePoint(void) {
    int failures = 0;

    for (size_t i = 0; i < FIXTURES; i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(fixtures[i].name, &size);

        if (!bytes) {
            failures += assertTrue(0);
            continue;
        }

        TinyPlan plan;
        TinyPlanResolution resolution;

        failures += assertEquals(
            resolveWhole(bytes, size, &resolution, &plan), TINYIMG_OK
        );
        failures +=
            assertEquals((long) plan.source_format, (long) fixtures[i].format);
        failures += assertGreaterThan((double) resolution.cost, 0.0);
        failures += assertTrue(resolution.cost_upper >= resolution.cost);

        // one model, read three ways
        failures += assertEquals(
            (long) tiny_plan_cost_upper(&plan), (long) resolution.cost_upper
        );
        failures += assertEquals(
            (long) tiny_plan_field(&resolution, TINYIMG_FIELD_COST_UPPER),
            (long) resolution.cost_upper
        );

        uint32_t point = tiny_encode_cost_at(
            fixtures[i].format, 640, 480, TINYIMG_COMPRESSION_AUTO
        );
        uint32_t upper = tiny_encode_cost_upper_at(
            fixtures[i].format, 640, 480, TINYIMG_COMPRESSION_AUTO
        );

        failures += assertGreaterThan((double) point, 0.0);
        failures += assertTrue(upper >= point);

        free(bytes);
    }

    // a format the build cannot write is priced at zero in both
    failures += assertEquals(
        (long) tiny_encode_cost_upper_at(
            TINYIMG_FORMAT_HEIF, 640, 480, TINYIMG_COMPRESSION_AUTO
        ),
        0L
    );
    failures += assertEquals(tiny_plan_cost_upper(0), 0u);

    return failures;
}

/**
 * Bytes at a fixed sample count never lower a lossy format's decode estimate.
 *
 * `size` is a plan field the estimate reads and the decoder does not, so
 * scaling it moves the byte term and nothing else.
 */
static int moreBytesNeverLowerADecode(void) {
    int failures = 0;

    for (size_t i = 0; i < FIXTURES; i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(fixtures[i].name, &size);

        if (!bytes) {
            failures += assertTrue(0);
            continue;
        }

        TinyPlan plan;
        TinyPlanResolution resolution;

        failures += assertEquals(
            resolveWhole(bytes, size, &resolution, &plan), TINYIMG_OK
        );
        uint32_t before = resolution.cost;

        plan.size = size * 4u;
        failures +=
            assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);
        // tiff and bmp read the byte count to tell a stored file from a coded
        // one, so a larger size can cross that threshold downward
        if (fixtures[i].format != TINYIMG_FORMAT_TIFF &&
            fixtures[i].format != TINYIMG_FORMAT_BMP) {
            failures += assertTrue(resolution.cost >= before);
        }

        // the three formats whose fit kept a byte term must move strictly
        if (fixtures[i].format == TINYIMG_FORMAT_JPEG ||
            fixtures[i].format == TINYIMG_FORMAT_PNG ||
            fixtures[i].format == TINYIMG_FORMAT_WEBP) {
            failures +=
                assertGreaterThan((double) resolution.cost, (double) before);
        }

        free(bytes);
    }

    return failures;
}

/** A TIFF or BMP stored without compression has no byte term. */
static int storedFormatsHaveNoByteTerm(void) {
    int failures = 0;

    static const char* names[] = {
        "derived/base-uncompressed.tif", "derived/base.bmp"
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(names[i], &size);

        if (!bytes) {
            failures += assertTrue(0);
            continue;
        }

        TinyPlan plan;
        TinyPlanResolution resolution;

        failures += assertEquals(
            resolveWhole(bytes, size, &resolution, &plan), TINYIMG_OK
        );
        uint32_t before = resolution.cost;

        plan.size = size * 4u;
        failures +=
            assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);
        failures += assertEquals((long) resolution.cost, (long) before);

        free(bytes);
    }

    return failures;
}

/**
 * The scale ordering the report documents: a reduced PNG request costs more
 * than the same source decoded whole, and a reduced JPEG request costs less.
 *
 * The comparison includes a small resample on the reduced side, which is a
 * few percent of either decode and cannot reverse the order.
 */
static int scaleOrdersTheFormats(void) {
    int failures = 0;

    static const struct {
        const char* name;
        int cheaper;
    } cases[] = {{"sf-24.jpg", 1}, {"forest.png", 0}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(cases[i].name, &size);

        if (!bytes) {
            failures += assertTrue(0);
            continue;
        }

        TinyPlan full;
        TinyPlan half;
        TinyPlanResolution at_full;
        TinyPlanResolution at_half;

        failures +=
            assertEquals(tiny_plan_init(&full, bytes, size), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_init(&half, bytes, size), TINYIMG_OK);
        failures += assertEquals(tiny_plan_resize(&half, 160u, 0u), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_resolve(&full, &at_full), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_resolve(&half, &at_half), TINYIMG_OK);

        failures += assertTrue(at_half.decode.scale_den > 1u);

        if (cases[i].cheaper) {
            failures +=
                assertLessThan((double) at_half.cost, (double) at_full.cost);
        }
        else {
            failures +=
                assertGreaterThan((double) at_half.cost, (double) at_full.cost);
        }

        free(bytes);
    }

    return failures;
}

/** Compression tiers still order, and AVIF now has an encode price. */
static int encodePricesKeepTheirShape(void) {
    int failures = 0;

    static const TinyImageFormat deflated[] = {
        TINYIMG_FORMAT_PNG, TINYIMG_FORMAT_TIFF
    };

    for (size_t i = 0; i < sizeof(deflated) / sizeof(*deflated); i++) {
        uint32_t none = tiny_encode_cost_upper_at(
            deflated[i], 640, 480, TINYIMG_COMPRESSION_NONE
        );
        uint32_t fast = tiny_encode_cost_upper_at(
            deflated[i], 640, 480, TINYIMG_COMPRESSION_FAST
        );
        uint32_t normal = tiny_encode_cost_upper_at(
            deflated[i], 640, 480, TINYIMG_COMPRESSION_DEFAULT
        );
        uint32_t best = tiny_encode_cost_upper_at(
            deflated[i], 640, 480, TINYIMG_COMPRESSION_BEST
        );

        failures += assertLessThan((double) none, (double) normal);
        failures += assertLessThan((double) fast, (double) normal);
        failures += assertLessThan((double) normal, (double) best);
    }

    failures += assertGreaterThan(
        (double) tiny_encode_cost(TINYIMG_FORMAT_AVIF, 640, 480), 0.0
    );

    return failures;
}

/** The cost of a plan at a decode effort, at whole frame or a reduced width. */
static uint32_t costAtEffort(
    const unsigned char* bytes, size_t size, uint32_t width, uint8_t effort,
    int* failures
) {
    TinyPlan plan;
    TinyPlanResolution resolution;

    *failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
    if (width) {
        *failures +=
            assertEquals(tiny_plan_resize(&plan, width, 0u), TINYIMG_OK);
    }
    *failures += assertEquals(tiny_plan_set_effort(&plan, effort), TINYIMG_OK);
    *failures +=
        assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);

    return resolution.cost;
}

/**
 * FAST never costs more than FANCY, and a lossless decode has no effort to
 * drop, so its price does not move at all.
 */
static int effortFollowsTheCodingNotTheFormat(void) {
    int failures = 0;

    static const struct {
        const char* name;
        int lossless;
    } cases[] = {
        {"derived/base-420.jpg", 0}, {"derived/base-lossy.webp", 0},
        {"derived/base.avif", 0},    {"derived/base.png", 1},
        {"derived/base.gif", 1},     {"derived/base-lzw.tif", 1},
        {"derived/base.bmp", 1},     {"derived/base-lossless.webp", 1},
    };
    static const uint32_t widths[] = {0u, 160u, 80u, 40u};

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        size_t size = 0;
        unsigned char* bytes = readFixture(cases[i].name, &size);

        if (!bytes) {
            failures += assertTrue(0);
            continue;
        }

        for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); w++) {
            uint32_t fancy = costAtEffort(
                bytes, size, widths[w], TINYIMG_EFFORT_FANCY, &failures
            );
            uint32_t fast = costAtEffort(
                bytes, size, widths[w], TINYIMG_EFFORT_FAST, &failures
            );

            failures += assertGreaterThan((double) fancy, 0.0);
            failures += assertTrue(fast <= fancy);
            failures += assertTrue((double) fast <= (double) fancy * 1.5);

            if (cases[i].lossless) {
                failures += assertEquals((long) fast, (long) fancy);
            }
        }

        free(bytes);
    }

    return failures;
}

/** Each lever the encoder reads moves its price in the direction it moves the
 * clock. */
static int encodeLeversMoveThePrice(void) {
    int failures = 0;

    TinyEncodePriceOpts lossy = {0, 0, 0, 3, 0};
    TinyEncodePriceOpts lossless = {0, 1, 0, 3, 0};
    TinyEncodePriceOpts alpha = {0, 0, 0, 4, 0};
    TinyEncodePriceOpts q50 = {50, 0, 0, 3, 0};
    TinyEncodePriceOpts q75 = {75, 0, 0, 3, 0};
    TinyEncodePriceOpts q95 = {95, 0, 0, 3, 0};
    TinyEncodePriceOpts fast = {0, 0, 0, 3, TINYIMG_EFFORT_FAST};

    // lossless WebP is dearer than lossy WebP at the same extent
    failures += assertGreaterThan(
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_WEBP, 640, 480, &lossless),
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_WEBP, 640, 480, &lossy)
    );

    // a non-opaque source costs WebP more than an opaque one
    failures += assertGreaterThan(
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_WEBP, 640, 480, &alpha),
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_WEBP, 640, 480, &lossy)
    );

    // quality orders the three lossy formats
    static const TinyImageFormat lossyFormats[] = {
        TINYIMG_FORMAT_JPEG, TINYIMG_FORMAT_WEBP, TINYIMG_FORMAT_AVIF
    };

    for (size_t i = 0; i < sizeof(lossyFormats) / sizeof(*lossyFormats); i++) {
        TinyImageFormat f = lossyFormats[i];
        uint32_t low = tiny_encode_cost_for(f, 640, 480, &q50);
        uint32_t mid = tiny_encode_cost_for(f, 640, 480, &q75);
        uint32_t high = tiny_encode_cost_for(f, 640, 480, &q95);

        failures += assertLessThan((double) low, (double) mid);
        failures += assertLessThan((double) mid, (double) high);
        failures +=
            assertTrue(tiny_encode_cost_upper_for(f, 640, 480, &q95) >= high);
    }

    // a quality between two measured points takes the dearer neighbor
    TinyEncodePriceOpts q80 = {80, 0, 0, 3, 0};
    TinyEncodePriceOpts q85 = {85, 0, 0, 3, 0};
    failures += assertEquals(
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &q80),
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &q85)
    );

    // a format with no lever for a field ignores it
    failures += assertEquals(
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_PNG, 640, 480, &q95),
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_PNG, 640, 480, &lossy)
    );
    failures += assertEquals(
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &lossless),
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &lossy)
    );

    // PNG reads the encoder effort, which JPEG does not
    failures += assertLessThan(
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_PNG, 640, 480, &fast),
        (double) tiny_encode_cost_for(TINYIMG_FORMAT_PNG, 640, 480, &lossy)
    );
    failures += assertEquals(
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &fast),
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_JPEG, 640, 480, &lossy)
    );

    // an unpriced format stays at zero whatever the request says
    failures += assertEquals(
        (long) tiny_encode_cost_for(TINYIMG_FORMAT_HEIF, 640, 480, &q95), 0L
    );

    return failures;
}

/** The old entry points are the new ones at the defaults, for every format. */
static int oldEntryPointsAreTheNewOnesAtDefaults(void) {
    int failures = 0;

    static const uint32_t extents[][2] = {{1, 1}, {640, 480}, {4000, 3000}};

    for (uint32_t format = 0; format <= TINYIMG_FORMAT_HEIF; format++) {
        for (uint32_t level = 0; level <= TINYIMG_COMPRESSION_BEST; level++) {
            for (size_t e = 0; e < sizeof(extents) / sizeof(*extents); e++) {
                TinyEncodePriceOpts opts = {0, 0, (uint8_t) level, 0, 0};
                uint32_t w = extents[e][0];
                uint32_t h = extents[e][1];

                failures += assertEquals(
                    (long) tiny_encode_cost_at(
                        (TinyImageFormat) format, w, h, (uint8_t) level
                    ),
                    (long) tiny_encode_cost_for(
                        (TinyImageFormat) format, w, h, &opts
                    )
                );
                failures += assertEquals(
                    (long) tiny_encode_cost_upper_at(
                        (TinyImageFormat) format, w, h, (uint8_t) level
                    ),
                    (long) tiny_encode_cost_upper_for(
                        (TinyImageFormat) format, w, h, &opts
                    )
                );
            }
        }

        // NULL is the defaults too
        failures += assertEquals(
            (long) tiny_encode_cost((TinyImageFormat) format, 640, 480),
            (long) tiny_encode_cost_for((TinyImageFormat) format, 640, 480, 0)
        );
    }

    return failures;
}

int main(void) {
    int failures = 0;

    failures += upperIsNeverBelowThePoint();
    failures += moreBytesNeverLowerADecode();
    failures += storedFormatsHaveNoByteTerm();
    failures += scaleOrdersTheFormats();
    failures += encodePricesKeepTheirShape();
    failures += effortFollowsTheCodingNotTheFormat();
    failures += encodeLeversMoveThePrice();
    failures += oldEntryPointsAreTheNewOnesAtDefaults();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
