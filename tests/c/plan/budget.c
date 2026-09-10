#include "../test.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The budget the planner degrades a request to fit.
 *
 * Two properties carry the feature. **Nothing is degraded when the estimate
 * already fits**, so a budget a caller sets generously changes no pixel; and
 * **whatever is given up is reported**, so a caller who would rather fail than
 * serve a softer image can. The rest is the ladder's order: the effort tier,
 * then nearest for a filter left open, then reductions of the decode one at a
 * time.
 */

/** Resolves a fit request at a budget, and reports what came back. */
static int resolveAt(
    const unsigned char* bytes, size_t size, uint32_t width, uint32_t budget,
    TinyPlanResolution* out
) {
    TinyPlan plan;

    int result = tiny_plan_init(&plan, bytes, size);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_resize(&plan, width, 0u);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_set_budget(&plan, budget);
    if (result != TINYIMG_OK) return result;

    return tiny_plan_resolve(&plan, out);
}

static int leavesAffordableRequestsAlone(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlanResolution none;
    failures +=
        assertEquals(resolveAt(bytes, size, 200u, 0u, &none), TINYIMG_OK);
    failures += assertEquals((long) none.degraded, 0L);
    failures += assertGreaterThan((double) none.cost, 0.0);

    // a budget with room to spare, so the ladder is not entered at all and
    // every field is the one the undegraded resolution had
    TinyPlanResolution roomy;
    failures += assertEquals(
        resolveAt(bytes, size, 200u, none.cost * 4u, &roomy), TINYIMG_OK
    );

    failures += assertEquals((long) roomy.degraded, 0L);
    failures += assertEquals((long) roomy.cost, (long) none.cost);
    failures += assertEquals(
        (long) roomy.decode.scale_den, (long) none.decode.scale_den
    );
    failures +=
        assertEquals((long) roomy.decode.effort, (long) none.decode.effort);

    // exactly the estimate is affordable, since the test is "over budget"
    TinyPlanResolution exact;
    failures += assertEquals(
        resolveAt(bytes, size, 200u, none.cost, &exact), TINYIMG_OK
    );
    failures += assertEquals((long) exact.degraded, 0L);

    free(bytes);
    return failures;
}

/**
 * A budget under the estimate takes the effort tier first.
 *
 * The rung is only reachable at all because the cost model knows what the
 * effort tier is worth: a model that priced FAST the same as FANCY would
 * report it as buying nothing and skip to a rung that softens the picture.
 */
static int takesEffortFirst(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlanResolution none;
    failures +=
        assertEquals(resolveAt(bytes, size, 400u, 0u, &none), TINYIMG_OK);

    // just under the estimate, which the effort tier alone should cover: the
    // decode gets cheaper and the resample keeps the filter it had, because at
    // 400 wide from 2400 this is a reduction and a reduction already takes box
    TinyPlanResolution eased;
    failures += assertEquals(
        resolveAt(bytes, size, 400u, none.cost - 1u, &eased), TINYIMG_OK
    );

    failures += assertEquals(
        (long) (eased.degraded & TINYIMG_DEGRADE_EFFORT),
        (long) TINYIMG_DEGRADE_EFFORT
    );
    failures +=
        assertEquals((long) eased.decode.effort, (long) TINYIMG_EFFORT_FAST);
    failures += assertLessThan((double) eased.cost, (double) none.cost);

    // and the output extent is untouched, which is what separates degrading
    // from resizing: a budget changes how the pixels are made, never how many
    failures += assertEquals((long) eased.width, (long) none.width);
    failures += assertEquals((long) eased.height, (long) none.height);

    free(bytes);
    return failures;
}

/** A budget nothing can meet reduces the decode as well, and says so. */
static int reducesTheDecodeNext(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("mountains.jpg", &size);
    if (!bytes) return 1;

    TinyPlanResolution none;
    failures +=
        assertEquals(resolveAt(bytes, size, 600u, 0u, &none), TINYIMG_OK);

    TinyPlanResolution floored;
    failures +=
        assertEquals(resolveAt(bytes, size, 600u, 1u, &floored), TINYIMG_OK);

    failures += assertEquals(
        (long) floored.degraded,
        (long) (TINYIMG_DEGRADE_EFFORT | TINYIMG_DEGRADE_FILTER |
                TINYIMG_DEGRADE_SCALE)
    );

    // the decoder was asked for a coarser grid than the ladder picked, so the
    // decode is now smaller than the output and the resample enlarges
    failures += assertGreaterThan(
        (double) floored.decode.scale_den, (double) none.decode.scale_den
    );
    failures +=
        assertLessThan((double) floored.decode_width, (double) floored.width);

    // still the same picture, at the same extent, for less cost: 12,846
    // microseconds against 21,801, which is 1.70x and is what every rung
    // together buys on this request
    failures += assertEquals((long) floored.width, (long) none.width);
    failures += assertEquals((long) floored.height, (long) none.height);
    failures +=
        assertLessThan((double) floored.cost, (double) none.cost * 0.75);

    // eight is the coarsest grid any codec offers, so the ladder stops there
    // rather than reporting a reduction it did not take
    failures += assertLessThan((double) floored.decode.scale_den, 9.0);

    free(bytes);
    return failures;
}

/**
 * The ladder stops at the first rung that fits.
 *
 * Which is the property that makes the report worth reading: a budget missed by
 * a little must not spend the coarsest decode available.
 */
static int stopsAtTheFirstFit(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("mountains.jpg", &size);
    if (!bytes) return 1;

    TinyPlanResolution none;
    failures +=
        assertEquals(resolveAt(bytes, size, 600u, 0u, &none), TINYIMG_OK);

    TinyPlanResolution effortOnly;
    failures += assertEquals(
        resolveAt(bytes, size, 600u, none.cost - 1u, &effortOnly), TINYIMG_OK
    );

    TinyPlanResolution floored;
    failures +=
        assertEquals(resolveAt(bytes, size, 600u, 1u, &floored), TINYIMG_OK);

    // a budget one microsecond under the estimate took the cheap rung; an
    // unmeetable one went further. 20,077 microseconds against 12,846, from a
    // baseline of 21,801
    failures +=
        assertEquals((long) effortOnly.degraded, (long) TINYIMG_DEGRADE_EFFORT);
    failures +=
        assertGreaterThan((double) effortOnly.cost, (double) floored.cost);
    failures += assertEquals(
        (long) effortOnly.decode.scale_den, (long) none.decode.scale_den
    );

    free(bytes);
    return failures;
}

/**
 * A source with no reduction to give reports only what it actually gave.
 *
 * An already decoded source has no decoder to ask, so the scale rung is
 * unavailable and reporting it would be a lie; the effort and filter rungs are
 * still there. The estimate for one carries no decode term either, so this is
 * also the case where the cost model is only the resample.
 */
static int reportsOnlyWhatItGave(void) {
    int failures = 0;

    TinyImage source;
    memset(&source, 0, sizeof(source));
    failures +=
        assertEquals(tiny_image_create(&source, 1200u, 800u, 3u), TINYIMG_OK);

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init_image(&plan, &source), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 900u, 0u), TINYIMG_OK);
    failures += assertEquals(tiny_plan_set_budget(&plan, 1u), TINYIMG_OK);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);

    failures += assertEquals(
        (long) resolution.degraded,
        (long) (TINYIMG_DEGRADE_EFFORT | TINYIMG_DEGRADE_FILTER)
    );
    failures += assertEquals((long) resolution.decode.scale_den, 1L);

    tiny_image_destroy(&source);
    return failures;
}

/**
 * A filter the caller named is never stepped down.
 *
 * The same rule the effort tier follows, for the same reason: both say how hard
 * to work at what was left open, not what to do instead of what was asked for.
 *
 * The reduction rung is still taken, and the reason is worth stating because it
 * is the opposite of the AUTO case. A named filter's rate is the same in either
 * direction, so a coarser decode is unconditionally cheaper; it is only under
 * AUTO that reducing first turns a box reduction into a bilinear enlargement
 * and costs more than it saves. The `cost >= best` guard is what decides that
 * per request rather than a rule about which rungs go together.
 */
static int neverOverridesANamedFilter(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("mountains.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
    failures += assertEquals(
        tiny_plan_fit_with(
            &plan, 600u, 400u, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER,
            TINYIMG_FILTER_CATMULL_ROM
        ),
        TINYIMG_OK
    );
    failures += assertEquals(tiny_plan_set_budget(&plan, 1u), TINYIMG_OK);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);

    failures += assertEquals(
        (long) resolution.filter_x, (long) TINYIMG_FILTER_CATMULL_ROM
    );
    failures +=
        assertEquals((long) (resolution.degraded & TINYIMG_DEGRADE_FILTER), 0L);
    failures += assertEquals(
        (long) resolution.degraded,
        (long) (TINYIMG_DEGRADE_EFFORT | TINYIMG_DEGRADE_SCALE)
    );

    free(bytes);
    return failures;
}

/** The named accessor reads the two new fields, which is how a host sees them.
 */
static int readsThroughTheAccessor(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlanResolution resolution;
    failures +=
        assertEquals(resolveAt(bytes, size, 400u, 1u, &resolution), TINYIMG_OK);

    failures += assertEquals(
        (long) tiny_plan_field(&resolution, TINYIMG_FIELD_COST),
        (long) resolution.cost
    );
    failures += assertEquals(
        (long) tiny_plan_field(&resolution, TINYIMG_FIELD_DEGRADED),
        (long) resolution.degraded
    );

    free(bytes);
    return failures;
}

/** tiny_plan_cost is the resolution's own estimate rather than a second model.
 */
static int costAgreesWithTheResolution(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 400u, 0u), TINYIMG_OK);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);
    failures +=
        assertEquals((long) tiny_plan_cost(&plan), (long) resolution.cost);

    // and a budget moves both together, because there is one model
    failures += assertEquals(tiny_plan_set_budget(&plan, 1u), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);
    failures +=
        assertEquals((long) tiny_plan_cost(&plan), (long) resolution.cost);

    free(bytes);
    return failures;
}

static int refusesNothing(void) {
    int failures = 0;

    failures += assertEquals(tiny_plan_set_budget(0, 10u), TINYIMG_ERR_NULL);

    return failures;
}

/**
 * The compression level is a lever on the encoder, which is the larger half of
 * a lossless request and the half no plan lever reaches.
 */
static int pricesTheCompressionLevel(void) {
    int failures = 0;

    static const TinyImageFormat deflated[] = {
        TINYIMG_FORMAT_PNG, TINYIMG_FORMAT_TIFF
    };

    for (size_t i = 0; i < sizeof(deflated) / sizeof(*deflated); i++) {
        uint32_t none = tiny_encode_cost_at(
            deflated[i], 400, 225, TINYIMG_COMPRESSION_NONE
        );
        uint32_t fast = tiny_encode_cost_at(
            deflated[i], 400, 225, TINYIMG_COMPRESSION_FAST
        );
        uint32_t normal = tiny_encode_cost_at(
            deflated[i], 400, 225, TINYIMG_COMPRESSION_DEFAULT
        );
        uint32_t best = tiny_encode_cost_at(
            deflated[i], 400, 225, TINYIMG_COMPRESSION_BEST
        );

        failures += assertLessThan((double) none, (double) fast);
        failures += assertLessThan((double) fast, (double) normal);
        failures += assertLessThan((double) normal, (double) best);

        // AUTO is the default level, which is what a caller who names nothing
        // gets from the encoder as well
        failures += assertEquals(
            (long) tiny_encode_cost_at(
                deflated[i], 400, 225, TINYIMG_COMPRESSION_AUTO
            ),
            (long) normal
        );
        failures += assertEquals(
            (long) tiny_encode_cost(deflated[i], 400, 225), (long) normal
        );
    }

    /*
     * A format with no deflate stream ignores the level, the same way its
     * encoder does. Asserted rather than assumed, because pricing a lever a
     * format does not have would make the planner take it for nothing.
     */
    static const TinyImageFormat plain[] = {
        TINYIMG_FORMAT_JPEG, TINYIMG_FORMAT_WEBP, TINYIMG_FORMAT_BMP,
        TINYIMG_FORMAT_GIF
    };

    for (size_t i = 0; i < sizeof(plain) / sizeof(*plain); i++) {
        uint32_t baseline = tiny_encode_cost(plain[i], 400, 225);

        failures += assertTrue(baseline > 0u);

        for (uint8_t level = TINYIMG_COMPRESSION_AUTO;
             level <= TINYIMG_COMPRESSION_BEST; level++) {
            failures += assertEquals(
                (long) tiny_encode_cost_at(plain[i], 400, 225, level),
                (long) baseline
            );
        }
    }

    // a format this build cannot write is priced at nothing either way
    failures += assertEquals(
        (long) tiny_encode_cost_at(TINYIMG_FORMAT_HEIF, 8, 8, 1), 0L
    );

    return failures;
}

/**
 * A scaled decode is charged per format, because only two codecs have one that
 * shrinks.
 *
 * The planner used to apply JPEG's factors to everything, which told it a
 * scaled PNG decode was 33% cheaper when it measures 62% dearer. This asserts
 * the sign rather than the figure: the estimate for a reduced PNG has to be
 * above the full-decode estimate, and JPEG's has to be below it.
 */
static int chargesAScaledDecodePerFormat(void) {
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
        TinyPlan reduced;
        TinyPlanResolution at_full;
        TinyPlanResolution at_eighth;

        tiny_plan_init(&full, bytes, size);
        tiny_plan_init(&reduced, bytes, size);

        // resolved through the scale ladder rather than by setting a
        // denominator, since that is the path a request takes
        tiny_plan_resize(&full, 0, 0);
        tiny_plan_resize(&reduced, 160, 0);

        if (tiny_plan_resolve(&full, &at_full) == TINYIMG_OK &&
            tiny_plan_resolve(&reduced, &at_eighth) == TINYIMG_OK) {
            failures += assertTrue(at_eighth.decode.scale_den > 1u);

            if (cases[i].cheaper) {
                failures += assertLessThan(
                    (double) at_eighth.cost, (double) at_full.cost
                );
            }
            else {
                failures += assertGreaterThan(
                    (double) at_eighth.cost, (double) at_full.cost * 0.9
                );
            }
        }
        else {
            failures += assertTrue(0);
        }

        free(bytes);
    }

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += leavesAffordableRequestsAlone();
    failures += takesEffortFirst();
    failures += reducesTheDecodeNext();
    failures += stopsAtTheFirstFit();
    failures += reportsOnlyWhatItGave();
    failures += neverOverridesANamedFilter();
    failures += readsThroughTheAccessor();
    failures += costAgreesWithTheResolution();
    failures += refusesNothing();
    failures += pricesTheCompressionLevel();
    failures += chargesAScaledDecodePerFormat();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
