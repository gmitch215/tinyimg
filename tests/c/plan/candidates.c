#include "../test.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The set of resolutions the budget ladder is one path through.
 *
 * The ladder gives up effort, then the filter, then scale, one rung at a time.
 * The enumeration lists every combination of those levers, so the property
 * that makes it the same space is that whatever the ladder resolves to, at
 * any budget, is one of the entries. Without it a chooser that picks from the
 * list could be compared against a baseline that was never in the list.
 */

typedef struct {
    const char* name;
    uint32_t width;
} Case;

static const Case cases[] = {
    {"sf-24.jpg", 600u},
    {"sf-24.jpg", 200u},
    {"road.jpg", 300u},
    {"toyota_racing.webp", 400u},
    {"toyota_racing.webp", 1400u},
    {"fox.avif", 300u},
    {"winter_forest.png", 250u},
    {"winter_forest.png", 900u},
    {"dartmouth.tiff", 120u},
    {"derived/base.gif", 160u},
    {"derived/base.bmp", 500u},
    {"derived/base-420.jpg", 800u},
};

static int plan_for(const Case* c, unsigned char** bytes, TinyPlan* plan) {
    size_t size = 0;
    *bytes = readFixture(c->name, &size);
    if (!*bytes) return 0;

    if (tiny_plan_init(plan, *bytes, size) != TINYIMG_OK) return 0;
    return tiny_plan_resize(plan, c->width, 0u) == TINYIMG_OK;
}

static int matches(const TinyPlanCandidate* c, const TinyPlanResolution* r) {
    return c->effort == r->decode.effort &&
           c->scale_den == r->decode.scale_den && c->filter_x == r->filter_x &&
           c->filter_y == r->filter_y && c->cost == r->cost &&
           c->degraded == r->degraded;
}

static int ladderIsAPathThroughTheLattice(void) {
    int failures = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned char* bytes = 0;
        TinyPlan plan;

        if (!plan_for(&cases[i], &bytes, &plan)) {
            failures += 1;
            continue;
        }

        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        TinyPlanResolution base;
        failures += assertEquals(tiny_plan_resolve(&plan, &base), TINYIMG_OK);

        uint32_t budgets[5] = {
            0u, 1u, base.cost - 1u, base.cost / 2u, base.cost / 4u
        };

        for (int b = 0; b < 5; b++) {
            tiny_plan_set_budget(&plan, budgets[b]);

            TinyPlanResolution picked;
            failures +=
                assertEquals(tiny_plan_resolve(&plan, &picked), TINYIMG_OK);

            int found = 0;
            for (uint32_t k = 0; k < count && k < TINYIMG_PLAN_MAX_CANDIDATES;
                 k++) {
                if (matches(&lattice[k], &picked)) found = 1;
            }

            if (!found) {
                printf(
                    "%s at %u: effort %d den %d cost %u degraded %u is not "
                    "in the lattice\n",
                    cases[i].name, budgets[b], picked.decode.effort,
                    picked.decode.scale_den, picked.cost, picked.degraded
                );
                failures += 1;
            }
        }

        free(bytes);
    }

    return failures;
}

static int startsFromTheUndegradedPlan(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for(&cases[0], &bytes, &plan)) return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    TinyPlanResolution base;
    failures += assertEquals(tiny_plan_resolve(&plan, &base), TINYIMG_OK);

    failures += assertGreaterThan((double) count, 1.0);

    // some candidate is cheaper than the undegraded one, or the levers would
    // leave nothing to choose between
    uint32_t cheapest = lattice[0].cost;
    for (uint32_t k = 1u; k < count; k++) {
        if (lattice[k].cost < cheapest) cheapest = lattice[k].cost;
    }
    failures += assertLessThan((double) cheapest, (double) lattice[0].cost);

    failures += assertEquals((long) lattice[0].degraded, 0L);
    failures += assertEquals((long) lattice[0].cost, (long) base.cost);
    failures +=
        assertEquals((long) lattice[0].effort, (long) base.decode.effort);
    failures +=
        assertEquals((long) lattice[0].scale_den, (long) base.decode.scale_den);
    failures += assertEquals((long) lattice[0].steps, 0L);
    failures += assertEquals((long) lattice[0].nearest, 0L);
    failures += assertEquals((long) lattice[0].output_format, 0L);

    // the ceiling bound is what tiny_plan_peak_bytes says of the same
    // resolution
    size_t peak = 0;
    failures +=
        assertEquals(tiny_plan_peak_bytes(&plan, &base, &peak), TINYIMG_OK);
    failures += assertEquals((long) lattice[0].peak_bytes, (long) peak);

    free(bytes);
    return failures;
}

static int listsOnlyDistinctResolutions(void) {
    int failures = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        if (!plan_for(&cases[i], &bytes, &plan)) {
            failures += 1;
            continue;
        }

        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        failures +=
            assertTrue(count >= 1u && count <= TINYIMG_PLAN_MAX_CANDIDATES);

        for (uint32_t a = 0; a < count; a++) {
            for (uint32_t b = a + 1u; b < count; b++) {
                int same = lattice[a].effort == lattice[b].effort &&
                           lattice[a].scale_den == lattice[b].scale_den &&
                           lattice[a].filter_x == lattice[b].filter_x &&
                           lattice[a].filter_y == lattice[b].filter_y;

                if (same) {
                    printf(
                        "%s: candidates %u and %u are one resolution\n",
                        cases[i].name, a, b
                    );
                    failures += 1;
                }
            }
        }

        free(bytes);
    }

    return failures;
}

static int reportsWhatEachCandidateGaveUp(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for(&cases[0], &bytes, &plan)) return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    for (uint32_t k = 1u; k < count; k++) {
        uint32_t expected = 0u;

        if (lattice[k].effort != lattice[0].effort) {
            expected |= TINYIMG_DEGRADE_EFFORT;
        }
        if (lattice[k].filter_x != lattice[0].filter_x ||
            lattice[k].filter_y != lattice[0].filter_y) {
            expected |= TINYIMG_DEGRADE_FILTER;
        }
        if (lattice[k].scale_den > lattice[0].scale_den) {
            expected |= TINYIMG_DEGRADE_SCALE;
        }

        failures += assertEquals((long) lattice[k].degraded, (long) expected);
        failures += assertNotEquals((long) lattice[k].degraded, 0L);
    }

    free(bytes);
    return failures;
}

static int holdsLessMemoryAsTheDecodeCoarsens(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for(&cases[0], &bytes, &plan)) return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    int coarser = 0;

    for (uint32_t a = 0; a < count; a++) {
        for (uint32_t b = 0; b < count; b++) {
            if (lattice[a].effort != lattice[b].effort ||
                lattice[a].filter_x != lattice[b].filter_x ||
                lattice[a].scale_den >= lattice[b].scale_den) {
                continue;
            }

            coarser += 1;

            if (lattice[b].peak_bytes > lattice[a].peak_bytes) {
                printf(
                    "peak rose from %u to %u as the denominator went from "
                    "%d to %d\n",
                    lattice[a].peak_bytes, lattice[b].peak_bytes,
                    lattice[a].scale_den, lattice[b].scale_den
                );
                failures += 1;
            }
        }
    }

    failures += assertGreaterThan((double) coarser, 0.0);

    free(bytes);
    return failures;
}

static int neverOverridesANamedFilter(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    tiny_plan_init(&plan, bytes, size);
    failures += assertEquals(
        tiny_plan_resize_with(&plan, 500u, 0u, TINYIMG_FILTER_CATMULL_ROM),
        TINYIMG_OK
    );

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    for (uint32_t k = 0; k < count; k++) {
        failures += assertEquals(
            (long) lattice[k].filter_x, (long) TINYIMG_FILTER_CATMULL_ROM
        );
        failures += assertEquals(
            (long) lattice[k].filter_y, (long) TINYIMG_FILTER_CATMULL_ROM
        );
        failures += assertEquals(
            (long) (lattice[k].degraded & TINYIMG_DEGRADE_FILTER), 0L
        );
    }

    free(bytes);
    return failures;
}

static int collapsesWithoutADecoderToAsk(void) {
    int failures = 0;

    TinyImage source;
    memset(&source, 0, sizeof(source));
    failures +=
        assertEquals(tiny_image_create(&source, 1200u, 800u, 3u), TINYIMG_OK);

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init_image(&plan, &source), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 900u, 0u), TINYIMG_OK);

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    // effort and the nearest filter cross to four; the scale levers have
    // nothing to reduce, so they add none
    failures += assertEquals((long) count, 4L);

    for (uint32_t k = 0; k < count; k++) {
        failures += assertEquals((long) lattice[k].scale_den, 1L);
        failures += assertEquals(
            (long) (lattice[k].degraded & TINYIMG_DEGRADE_SCALE), 0L
        );
    }

    tiny_image_destroy(&source);
    return failures;
}

static int skipsTheEffortLeverAtFast(void) {
    int failures = 0;

    TinyImage source;
    memset(&source, 0, sizeof(source));
    failures +=
        assertEquals(tiny_image_create(&source, 1200u, 800u, 3u), TINYIMG_OK);

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init_image(&plan, &source), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 900u, 0u), TINYIMG_OK);
    failures += assertEquals(
        tiny_plan_set_effort(&plan, TINYIMG_EFFORT_FAST), TINYIMG_OK
    );

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    failures += assertEquals((long) count, 2L);

    for (uint32_t k = 0; k < count; k++) {
        failures += assertEquals(
            (long) (lattice[k].degraded & TINYIMG_DEGRADE_EFFORT), 0L
        );
    }

    tiny_image_destroy(&source);
    return failures;
}

static int countsPastTheCapacityItWasGiven(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for(&cases[0], &bytes, &plan)) return 1;

    TinyPlanCandidate all[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t total = 0;
    failures += assertEquals(
        tiny_plan_enumerate(&plan, all, TINYIMG_PLAN_MAX_CANDIDATES, &total),
        TINYIMG_OK
    );
    failures += assertGreaterThan((double) total, 2.0);

    TinyPlanCandidate two[2];
    memset(two, 0xAB, sizeof(two));
    uint32_t count = 0;
    failures +=
        assertEquals(tiny_plan_enumerate(&plan, two, 2u, &count), TINYIMG_OK);
    failures += assertEquals((long) count, (long) total);
    failures += assertEquals((long) two[1].cost, (long) all[1].cost);

    failures +=
        assertEquals(tiny_plan_enumerate(&plan, 0, 0u, &count), TINYIMG_OK);
    failures += assertEquals((long) count, (long) total);

    free(bytes);
    return failures;
}

static int ignoresTheBudget(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for(&cases[0], &bytes, &plan)) return 1;

    TinyPlanCandidate without[TINYIMG_PLAN_MAX_CANDIDATES];
    TinyPlanCandidate with[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t a = 0;
    uint32_t b = 0;

    failures += assertEquals(
        tiny_plan_enumerate(&plan, without, TINYIMG_PLAN_MAX_CANDIDATES, &a),
        TINYIMG_OK
    );
    tiny_plan_set_budget(&plan, 1u);
    failures += assertEquals(
        tiny_plan_enumerate(&plan, with, TINYIMG_PLAN_MAX_CANDIDATES, &b),
        TINYIMG_OK
    );

    failures += assertEquals((long) a, (long) b);
    for (uint32_t k = 0; k < a && k < b; k++) {
        failures += assertEquals((long) without[k].cost, (long) with[k].cost);
        failures += assertEquals(
            (long) without[k].peak_bytes, (long) with[k].peak_bytes
        );
    }

    free(bytes);
    return failures;
}

static int rejectsMissingArguments(void) {
    int failures = 0;

    TinyPlan plan;
    memset(&plan, 0, sizeof(plan));
    TinyPlanCandidate out[1];
    uint32_t count = 0;

    failures +=
        assertEquals(tiny_plan_enumerate(0, out, 1u, &count), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_plan_enumerate(&plan, out, 1u, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_plan_enumerate(&plan, 0, 1u, &count), TINYIMG_ERR_NULL
    );

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += rejectsMissingArguments();
    failures += startsFromTheUndegradedPlan();
    failures += listsOnlyDistinctResolutions();
    failures += reportsWhatEachCandidateGaveUp();
    failures += holdsLessMemoryAsTheDecodeCoarsens();
    failures += neverOverridesANamedFilter();
    failures += collapsesWithoutADecoderToAsk();
    failures += skipsTheEffortLeverAtFast();
    failures += countsPastTheCapacityItWasGiven();
    failures += ignoresTheBudget();
    failures += ladderIsAPathThroughTheLattice();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
