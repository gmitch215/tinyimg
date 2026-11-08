#include "tinyimg/select.h"
#include "../test.h"
#include "tinyimg/memory.h"

/**
 * @file
 * @brief The constrained selector over the lattice, the format and the tier.
 *
 * Nothing here reads a clock or a number out of the generated cost and
 * distortion tables. The assertions are relations that survive a refit: the
 * budget boundary, the order of the objectives, the alpha rule, the
 * composition arithmetic and the property that the joint search contains the
 * fixed policies.
 */

typedef struct {
    const char* name;
    uint32_t width;
} Case;

static const Case cases[] = {
    {"sf-24.jpg", 400u},
    {"winter_forest.png", 250u},
    {"toyota_racing.webp", 400u},
    {"fox.avif", 300u},
    {"derived/base-alpha.png", 160u},
    {"derived/base.gif", 160u},
    {"dartmouth.tiff", 120u},
};

#define CASES (sizeof(cases) / sizeof(cases[0]))

static TinySelection joint;
static TinySelection other;
static TinySelection third;

static int planFor(const Case* c, unsigned char** bytes, TinyPlan* plan) {
    size_t size = 0;
    *bytes = readFixture(c->name, &size);
    if (!*bytes) return 0;

    if (tiny_plan_init(plan, *bytes, size) != TINYIMG_OK) return 0;
    return tiny_plan_resize(plan, c->width, 0u) == TINYIMG_OK;
}

static uint32_t field(const TinySelection* s, TinySelectionField f) {
    return tiny_selection_field(s, f);
}

static uint32_t entry(
    const TinySelection* s, uint32_t i, TinyConsideredField f
) {
    return tiny_selection_considered_field(s, i, f);
}

static int isAlphaBlind(uint32_t format) {
    return format == TINYIMG_FORMAT_JPEG || format == TINYIMG_FORMAT_AVIF ||
           format == TINYIMG_FORMAT_GIF;
}

static const uint32_t everything = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_PNG) |
                                   TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_JPEG) |
                                   TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_GIF) |
                                   TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP) |
                                   TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_AVIF);

// #region alpha

static int alphaSourceNeverSelectsJpegOrAvif(void) {
    int failures = 0;
    const Case alpha[] = {
        {"winter_forest.png", 250u}, {"derived/base-alpha.png", 160u}
    };

    for (size_t k = 0; k < 2; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&alpha[k], &bytes, &plan));

        for (uint32_t objective = 0; objective < 3u; objective++) {
            for (float level = 0.0f; level <= 30.0f; level += 30.0f) {
                TinySelectOpts opts = {0};
                opts.formats = everything;
                opts.objective = objective;
                opts.min_db = level;

                failures += assertEquals(
                    tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK
                );
                failures += assertEquals(
                    field(&joint, TINYIMG_SELECTION_FIELD_ALPHA), 1
                );
                failures += assertEquals(
                    field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1
                );
                failures += assertFalse(
                    isAlphaBlind(field(&joint, TINYIMG_SELECTION_FIELD_FORMAT))
                );

                for (uint32_t i = 0; i < joint.count; i++) {
                    if (isAlphaBlind(
                            entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT)
                        )) {
                        failures += assertEquals(
                            entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON),
                            TINYIMG_SELECT_REASON_ALPHA
                        );
                    }
                }
            }
        }

        TinySelectOpts jpeg = {0};
        jpeg.formats = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_JPEG);
        failures +=
            assertEquals(tiny_plan_select(&plan, &jpeg, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_REASON),
            TINYIMG_SELECT_REASON_ALPHA
        );

        jpeg.assume_opaque = 1;
        failures +=
            assertEquals(tiny_plan_select(&plan, &jpeg, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_FORMAT), TINYIMG_FORMAT_JPEG
        );

        free(bytes);
    }

    return failures;
}

static int opaqueSourceMayUseJpeg(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.formats = everything;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures += assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_ALPHA), 0);

    int sawJpeg = 0;
    for (uint32_t i = 0; i < joint.count; i++) {
        if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT) ==
                TINYIMG_FORMAT_JPEG &&
            entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON) ==
                TINYIMG_SELECT_REASON_NONE) {
            sawJpeg = 1;
        }
    }
    failures += assertTrue(sawJpeg);

    free(bytes);
    return failures;
}

// #endregion

// #region the space searched

static int searchesTheLatticeTimesTheVariants(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;

        failures += assertTrue(planFor(&cases[k], &bytes, &plan));
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        TinySelectOpts opts = {0};
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_LATTICE), count);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_CONSIDERED),
            count * TINYIMG_SELECT_MAX_VARIANTS
        );
        failures += assertTrue(
            field(&joint, TINYIMG_SELECTION_FIELD_CONSIDERED) <=
            TINYIMG_SELECT_MAX_CONSIDERED
        );

        free(bytes);
    }

    return failures;
}

static int pricesTheTimeBoundFromThePublicFunctions(void) {
    int failures = 0;
    uint32_t scales[3] = {0u, 1000u, 1900u};

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinyPlanResolution base;
        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;
        failures += assertEquals(tiny_plan_resolve(&plan, &base), TINYIMG_OK);
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        for (int s = 0; s < 3; s++) {
            TinySelectOpts opts = {0};
            opts.scale_milli = scales[s];
            opts.formats = everything;
            opts.uncharacterized = TINYIMG_SELECT_ALLOW;
            failures += assertEquals(
                tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK
            );

            uint32_t scale = scales[s] ? scales[s] : 1000u;

            for (uint32_t i = 0; i < joint.count; i++) {
                if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_COST_UPPER) == 0u)
                    continue;
                uint32_t at =
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_CANDIDATE);

                TinyEncodePriceOpts price = {
                    (uint8_t)
                        entry(&joint, i, TINYIMG_CONSIDERED_FIELD_QUALITY),
                    (uint8_t) (entry(
                                   &joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT
                               ) == TINYIMG_FORMAT_WEBP &&
                               entry(
                                   &joint, i, TINYIMG_CONSIDERED_FIELD_LOSSLESS
                               )),
                    0, base.channels, 0
                };
                uint64_t encode = tiny_encode_cost_upper_for(
                    (TinyImageFormat)
                        entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT),
                    base.width, base.height, &price
                );
                uint32_t bound =
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_COST_UPPER);

                // candidate 0 is the plan's own bound; any other point is
                // bounded below by its point estimate
                if (at == 0u) {
                    uint64_t total =
                        ((uint64_t) base.cost_upper + encode) * scale;
                    failures +=
                        assertEquals(bound, (long) ((total + 999u) / 1000u));
                }
                else {
                    uint64_t total =
                        ((uint64_t) lattice[at].cost + encode) * scale;
                    failures += assertTrue((uint64_t) bound * 1000u >= total);
                }
            }
        }

        free(bytes);
    }

    return failures;
}

// #endregion

// #region the budget

static uint32_t cheapest(const TinyPlan* plan, TinySelectOpts opts) {
    opts.objective = TINYIMG_SELECT_MIN_TIME;
    opts.budget_us = 0u;

    if (tiny_plan_select(plan, &opts, &joint) != TINYIMG_OK) return 0u;
    if (!field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE)) return 0u;
    return field(&joint, TINYIMG_SELECTION_FIELD_COST_UPPER);
}

static int aBudgetAtTheCheapestBoundSelectsIt(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.objective = TINYIMG_SELECT_MIN_BYTES;

        uint32_t bound = cheapest(&plan, opts);
        failures += assertGreaterThan((double) bound, 0.0);

        opts.budget_us = bound;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);
        failures += assertTrue(
            field(&joint, TINYIMG_SELECTION_FIELD_COST_UPPER) <= bound
        );

        opts.objective = TINYIMG_SELECT_MIN_TIME;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_COST_UPPER), bound
        );

        opts.budget_us = bound - 1u;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_REASON),
            TINYIMG_SELECT_REASON_BUDGET
        );

        free(bytes);
    }

    return failures;
}

static int theMachineFactorMovesTheBoundary(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.scale_milli = 2000u;

    uint32_t bound = cheapest(&plan, opts);

    opts.objective = TINYIMG_SELECT_MIN_TIME;
    opts.budget_us = bound;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures +=
        assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);

    opts.scale_milli = 2100u;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures +=
        assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);

    free(bytes);
    return failures;
}

static int pricesTheBoundOnlyAfterTheCheapChecks(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[1], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.formats = everything;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

    for (uint32_t i = 0; i < joint.count; i++) {
        uint32_t reason = entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON);
        uint32_t cost = entry(&joint, i, TINYIMG_CONSIDERED_FIELD_COST_UPPER);

        if (reason != TINYIMG_SELECT_REASON_NONE &&
            reason < TINYIMG_SELECT_REASON_QUALITY) {
            failures += assertEquals(cost, 0);
        }
        else {
            failures += assertGreaterThan((double) cost, 0.0);
        }
    }

    free(bytes);
    return failures;
}

// #endregion

// #region quality

static int aFloorOf99NeedsAnExactCandidate(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.min_db = 99.0f;
        opts.formats = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_JPEG) |
                       TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP) |
                       TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_AVIF);
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_REASON),
            TINYIMG_SELECT_REASON_QUALITY
        );

        opts.formats |= TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_PNG);
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_FORMAT), TINYIMG_FORMAT_PNG
        );
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_CLASS),
            TINYIMG_DISTORTION_EXACT
        );
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_DB_MILLI), 99000
        );
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_CANDIDATE), 0);

        free(bytes);
    }

    return failures;
}

static int everyFeasibleCandidateMeetsTheFloor(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.formats = everything;
        opts.min_db = 28.0f;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

        for (uint32_t i = 0; i < joint.count; i++) {
            if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON) !=
                TINYIMG_SELECT_REASON_NONE) {
                continue;
            }
            failures += assertEquals(
                entry(&joint, i, TINYIMG_CONSIDERED_FIELD_DB_KNOWN), 1
            );
            failures += assertTrue(
                entry(&joint, i, TINYIMG_CONSIDERED_FIELD_DB_MILLI) >= 28000u
            );
        }

        free(bytes);
    }

    return failures;
}

static int refusesAnUncharacterizedClassUnlessAllowed(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.formats = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_BMP);

    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures +=
        assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_REASON),
        TINYIMG_SELECT_REASON_UNCHARACTERIZED
    );

    opts.uncharacterized = TINYIMG_SELECT_ALLOW;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures +=
        assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_FORMAT), TINYIMG_FORMAT_BMP
    );
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_CLASS),
        TINYIMG_DISTORTION_UNCHARACTERIZED
    );
    failures +=
        assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_BYTES_KNOWN), 0);

    free(bytes);
    return failures;
}

static int anUnknownLeverCannotMeetAFloor(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[6], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.uncharacterized = TINYIMG_SELECT_ALLOW;
    opts.formats = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP);
    opts.min_db = 1.0f;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

    for (uint32_t i = 0; i < joint.count; i++) {
        if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON) !=
            TINYIMG_SELECT_REASON_NONE) {
            continue;
        }
        failures += assertEquals(
            entry(&joint, i, TINYIMG_CONSIDERED_FIELD_DB_KNOWN), 1
        );
    }

    free(bytes);
    return failures;
}

// #endregion

// #region objectives

static int theObjectivesPickTheirOwnMinimum(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.formats = everything;
        opts.uncharacterized = TINYIMG_SELECT_ALLOW;

        for (uint32_t objective = 0; objective < 3u; objective++) {
            opts.objective = objective;
            failures += assertEquals(
                tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK
            );
            failures += assertEquals(
                field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1
            );

            uint32_t cost = field(&joint, TINYIMG_SELECTION_FIELD_COST_UPPER);
            uint32_t bpp =
                field(&joint, TINYIMG_SELECTION_FIELD_BYTES_PER_PIXEL_MICRO);
            uint32_t db = field(&joint, TINYIMG_SELECTION_FIELD_DB_MILLI);
            uint32_t known = field(&joint, TINYIMG_SELECTION_FIELD_BYTES_KNOWN);

            for (uint32_t i = 0; i < joint.count; i++) {
                if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON) !=
                    TINYIMG_SELECT_REASON_NONE) {
                    continue;
                }

                uint32_t c =
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_COST_UPPER);
                uint32_t b = entry(
                    &joint, i, TINYIMG_CONSIDERED_FIELD_BYTES_PER_PIXEL_MICRO
                );
                uint32_t d =
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_DB_MILLI);
                uint32_t bk =
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_BYTES_KNOWN);

                if (objective == TINYIMG_SELECT_MIN_TIME) {
                    failures += assertTrue(cost <= c);
                }
                else if (objective == TINYIMG_SELECT_MIN_BYTES) {
                    // an unknown byte figure ranks last
                    failures += assertTrue(!known ? !bk : (!bk || bpp <= b));
                }
                else {
                    failures += assertTrue(db >= d);
                }
            }
        }

        free(bytes);
    }

    return failures;
}

static int minBytesPrefersTheCheapestPointOfTheCheapestFormat(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.formats = everything;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

    // among equal bytes the lower time bound wins
    uint32_t bpp = field(&joint, TINYIMG_SELECTION_FIELD_BYTES_PER_PIXEL_MICRO);
    uint32_t cost = field(&joint, TINYIMG_SELECTION_FIELD_COST_UPPER);

    for (uint32_t i = 0; i < joint.count; i++) {
        if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_REASON) ==
                TINYIMG_SELECT_REASON_NONE &&
            entry(&joint, i, TINYIMG_CONSIDERED_FIELD_BYTES_PER_PIXEL_MICRO) ==
                bpp) {
            failures += assertTrue(
                cost <= entry(&joint, i, TINYIMG_CONSIDERED_FIELD_COST_UPPER)
            );
        }
    }

    free(bytes);
    return failures;
}

// #endregion

// #region memory

static int aPeakCapBelowTheCheapestPeakIsInfeasible(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;

        failures += assertTrue(planFor(&cases[k], &bytes, &plan));
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        uint32_t least = lattice[0].peak_bytes;
        for (uint32_t i = 1; i < count; i++) {
            if (lattice[i].peak_bytes < least) least = lattice[i].peak_bytes;
        }

        TinySelectOpts opts = {0};
        opts.peak_cap_bytes = least - 1u;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
        failures += assertEquals(
            field(&joint, TINYIMG_SELECTION_FIELD_REASON),
            TINYIMG_SELECT_REASON_PEAK
        );

        opts.peak_cap_bytes = least;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE), 1);
        failures +=
            assertTrue(field(&joint, TINYIMG_SELECTION_FIELD_PEAK) <= least);

        free(bytes);
    }

    return failures;
}

static int theDefaultCapIsTheHeap(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    TinySelectOpts same = {0};
    same.peak_cap_bytes = TINYIMG_HEAP_MAX;

    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures +=
        assertEquals(tiny_plan_select(&plan, &same, &other), TINYIMG_OK);
    failures += assertBytesMatch(&joint, &other, sizeof(joint));

    free(bytes);
    return failures;
}

// #endregion

// #region the fixed policies

static long keyOf(const TinySelection* s, uint32_t objective) {
    switch (objective) {
        case TINYIMG_SELECT_MIN_TIME:
            return (long) field(s, TINYIMG_SELECTION_FIELD_COST_UPPER);
        case TINYIMG_SELECT_MAX_QUALITY:
            return -(long) field(s, TINYIMG_SELECTION_FIELD_DB_MILLI);
        default:
            return field(s, TINYIMG_SELECTION_FIELD_BYTES_KNOWN)
                       ? (long) field(
                             s, TINYIMG_SELECTION_FIELD_BYTES_PER_PIXEL_MICRO
                         )
                       : 0x7FFFFFFFL;
    }
}

static int theLadderIsAMemberOfTheLatticeAndNeverBeatsTheJointSearch(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
        uint32_t count = 0;

        failures += assertTrue(planFor(&cases[k], &bytes, &plan));
        failures += assertEquals(
            tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            TINYIMG_OK
        );

        TinySelectOpts probe = {0};
        uint32_t floor = cheapest(&plan, probe);
        uint32_t budgets[4] = {0u, floor, floor * 2u, floor * 4u};
        uint32_t scales[2] = {1000u, 1900u};
        float floors[2] = {0.0f, 25.0f};

        for (int b = 0; b < 4; b++) {
            for (int s = 0; s < 2; s++) {
                for (int q = 0; q < 2; q++) {
                    for (uint32_t objective = 0; objective < 3u; objective++) {
                        TinySelectOpts opts = {0};
                        opts.budget_us = budgets[b];
                        opts.scale_milli = scales[s];
                        opts.min_db = floors[q];
                        opts.objective = objective;
                        opts.formats = everything;

                        failures += assertEquals(
                            tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK
                        );
                        failures += assertEquals(
                            tiny_plan_select_ladder(&plan, &opts, &other),
                            TINYIMG_OK
                        );
                        failures += assertEquals(
                            tiny_plan_select_independent(&plan, &opts, &third),
                            TINYIMG_OK
                        );

                        const TinySelection* arms[2] = {&other, &third};

                        for (int a = 0; a < 2; a++) {
                            failures += assertEquals(
                                field(
                                    arms[a], TINYIMG_SELECTION_FIELD_CONSIDERED
                                ),
                                1
                            );
                            failures += assertTrue(
                                field(
                                    arms[a], TINYIMG_SELECTION_FIELD_LATTICE
                                ) == count
                            );

                            if (!field(
                                    arms[a], TINYIMG_SELECTION_FIELD_FEASIBLE
                                ))
                                continue;

                            uint32_t at = field(
                                arms[a], TINYIMG_SELECTION_FIELD_CANDIDATE
                            );
                            failures += assertTrue(at < count);

                            // the joint search contains it, so it is feasible
                            // and at least as good on the objective
                            failures += assertEquals(
                                field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE),
                                1
                            );
                            failures += assertTrue(
                                keyOf(&joint, objective) <=
                                keyOf(arms[a], objective)
                            );
                        }
                    }
                }
            }
        }

        free(bytes);
    }

    return failures;
}

static int theLadderTakesTheSourceFormatAtItsDefaultTier(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    failures +=
        assertEquals(tiny_plan_select_ladder(&plan, &opts, &other), TINYIMG_OK);
    failures += assertEquals(
        field(&other, TINYIMG_SELECTION_FIELD_FORMAT), TINYIMG_FORMAT_JPEG
    );
    failures +=
        assertEquals(field(&other, TINYIMG_SELECTION_FIELD_QUALITY), 85);
    failures +=
        assertEquals(field(&other, TINYIMG_SELECTION_FIELD_CANDIDATE), 0);

    opts.formats = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP);
    failures +=
        assertEquals(tiny_plan_select_ladder(&plan, &opts, &other), TINYIMG_OK);
    failures += assertEquals(
        field(&other, TINYIMG_SELECTION_FIELD_FORMAT), TINYIMG_FORMAT_WEBP
    );
    failures +=
        assertEquals(field(&other, TINYIMG_SELECTION_FIELD_QUALITY), 75);

    free(bytes);
    return failures;
}

static int theLadderIgnoresThePeakCapUntilItIsChecked(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;
    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;

    failures += assertTrue(planFor(&cases[2], &bytes, &plan));
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    TinySelectOpts opts = {0};
    opts.peak_cap_bytes = lattice[0].peak_bytes - 1u;

    failures +=
        assertEquals(tiny_plan_select_ladder(&plan, &opts, &other), TINYIMG_OK);
    failures +=
        assertEquals(field(&other, TINYIMG_SELECTION_FIELD_FEASIBLE), 0);
    failures += assertEquals(
        field(&other, TINYIMG_SELECTION_FIELD_REASON),
        TINYIMG_SELECT_REASON_PEAK
    );

    // the joint search finds a lattice point that fits when one exists
    uint32_t least = lattice[0].peak_bytes;
    for (uint32_t i = 1; i < count; i++) {
        if (lattice[i].peak_bytes < least) least = lattice[i].peak_bytes;
    }
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_FEASIBLE),
        least < lattice[0].peak_bytes
    );

    free(bytes);
    return failures;
}

// #endregion

// #region composition

static int composesNoisePower(void) {
    int failures = 0;
    const TinyDistortionClass emp = TINYIMG_DISTORTION_EMPIRICAL;
    const TinyDistortionClass exact = TINYIMG_DISTORTION_EXACT;

    failures += assertFloatEquals(
        tiny_select_compose_db(exact, 99.0f, exact, 99.0f), 99.0f, 0.0f
    );
    failures += assertFloatEquals(
        tiny_select_compose_db(exact, 99.0f, emp, 31.5f), 31.5f, 0.01f
    );
    failures += assertFloatEquals(
        tiny_select_compose_db(emp, 31.5f, exact, 99.0f), 31.5f, 0.01f
    );
    failures += assertFloatEquals(
        tiny_select_compose_db(emp, 30.0f, emp, 30.0f), 26.9897f, 0.02f
    );
    failures += assertFloatEquals(
        tiny_select_compose_db(emp, 40.0f, emp, 30.0f), 29.5861f, 0.02f
    );

    return failures;
}

static int addingNoiseNeverRaisesTheDb(void) {
    int failures = 0;
    const float lever[6] = {13.0f, 21.0f, 30.0f, 45.0f, 60.0f, 99.0f};
    const float encode[6] = {17.0f, 28.0f, 33.0f, 40.0f, 55.0f, 99.0f};

    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            float both = tiny_select_compose_db(
                TINYIMG_DISTORTION_EMPIRICAL, lever[i],
                TINYIMG_DISTORTION_EMPIRICAL, encode[j]
            );
            float onlyLever = tiny_select_compose_db(
                TINYIMG_DISTORTION_EMPIRICAL, lever[i],
                TINYIMG_DISTORTION_EXACT, 99.0f
            );
            float onlyEncode = tiny_select_compose_db(
                TINYIMG_DISTORTION_EXACT, 99.0f, TINYIMG_DISTORTION_EMPIRICAL,
                encode[j]
            );

            failures += assertTrue(both <= onlyLever + 0.01f);
            failures += assertTrue(both <= onlyEncode + 0.01f);

            if (j > 0) {
                float quieter = tiny_select_compose_db(
                    TINYIMG_DISTORTION_EMPIRICAL, lever[i],
                    TINYIMG_DISTORTION_EMPIRICAL, encode[j]
                );
                float noisier = tiny_select_compose_db(
                    TINYIMG_DISTORTION_EMPIRICAL, lever[i],
                    TINYIMG_DISTORTION_EMPIRICAL, encode[j - 1]
                );
                failures += assertTrue(noisier <= quieter + 0.001f);
            }
        }
    }

    return failures;
}

static int theWeakestClassWins(void) {
    int failures = 0;
    const TinyDistortionClass order[4] = {
        TINYIMG_DISTORTION_UNCHARACTERIZED, TINYIMG_DISTORTION_EMPIRICAL,
        TINYIMG_DISTORTION_BOUNDED, TINYIMG_DISTORTION_EXACT
    };

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            failures += assertEquals(
                tiny_select_weakest(order[i], order[j]), order[i < j ? i : j]
            );
        }
    }

    return failures;
}

static int composedClassIsTheWeakestOfTheParts(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.formats = everything;
        opts.uncharacterized = TINYIMG_SELECT_ALLOW;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

        for (uint32_t i = 0; i < joint.count; i++) {
            uint32_t klass = entry(&joint, i, TINYIMG_CONSIDERED_FIELD_CLASS);
            uint32_t at = entry(&joint, i, TINYIMG_CONSIDERED_FIELD_CANDIDATE);
            TinyEncodeDistortion row;
            TinyDistortionClass enc = tiny_distortion_encode(
                (TinyImageFormat)
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT),
                entry(&joint, i, TINYIMG_CONSIDERED_FIELD_QUALITY)
                    ? (uint8_t)
                          entry(&joint, i, TINYIMG_CONSIDERED_FIELD_QUALITY)
                    : 75u,
                &row
            );

            // candidate 0 is the reference, exact by definition; a lossless
            // coding that has no row is still UNCHARACTERIZED
            if (at == 0u &&
                entry(&joint, i, TINYIMG_CONSIDERED_FIELD_LOSSLESS) &&
                enc == TINYIMG_DISTORTION_EXACT) {
                failures += assertEquals(klass, TINYIMG_DISTORTION_EXACT);
            }
            if (at == 0u &&
                !entry(&joint, i, TINYIMG_CONSIDERED_FIELD_LOSSLESS) &&
                enc == TINYIMG_DISTORTION_EMPIRICAL) {
                failures += assertEquals(klass, TINYIMG_DISTORTION_EMPIRICAL);
            }
            if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT) ==
                TINYIMG_FORMAT_BMP) {
                failures +=
                    assertEquals(klass, TINYIMG_DISTORTION_UNCHARACTERIZED);
            }
        }

        free(bytes);
    }

    return failures;
}

static int candidateZeroNeverLosesQualityToALever(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.formats = everything;
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

        for (uint32_t i = 0; i < joint.count; i++) {
            for (uint32_t j = 0; j < joint.count; j++) {
                if (entry(&joint, i, TINYIMG_CONSIDERED_FIELD_CANDIDATE) !=
                        0u ||
                    entry(&joint, j, TINYIMG_CONSIDERED_FIELD_CANDIDATE) ==
                        0u ||
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_FORMAT) !=
                        entry(&joint, j, TINYIMG_CONSIDERED_FIELD_FORMAT) ||
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_QUALITY) !=
                        entry(&joint, j, TINYIMG_CONSIDERED_FIELD_QUALITY) ||
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_LOSSLESS) !=
                        entry(&joint, j, TINYIMG_CONSIDERED_FIELD_LOSSLESS) ||
                    !entry(&joint, j, TINYIMG_CONSIDERED_FIELD_DB_KNOWN)) {
                    continue;
                }

                failures += assertTrue(
                    entry(&joint, i, TINYIMG_CONSIDERED_FIELD_DB_MILLI) >=
                    entry(&joint, j, TINYIMG_CONSIDERED_FIELD_DB_MILLI)
                );
            }
        }

        free(bytes);
    }

    return failures;
}

// #endregion

// #region report

static int carriesTheMeasuredCoverage(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[2], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.objective = TINYIMG_SELECT_MIN_BYTES;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_LEVER_COVERAGE_ALL), 893
    );
    failures += assertGreaterThan(
        (double) field(&joint, TINYIMG_SELECTION_FIELD_LEVER_COVERAGE), 0.0
    );
    failures += assertGreaterThan(
        (double) field(&joint, TINYIMG_SELECTION_FIELD_ENCODE_COVERAGE), 0.0
    );
    failures += assertGreaterThan(
        (double) field(&joint, TINYIMG_SELECTION_FIELD_PREDICTED_BYTES), 0.0
    );
    failures += assertTrue(
        field(&joint, TINYIMG_SELECTION_FIELD_DB_MEDIAN_MILLI) >=
        field(&joint, TINYIMG_SELECTION_FIELD_DB_MILLI)
    );

    free(bytes);
    return failures;
}

static int isDeterministic(void) {
    int failures = 0;

    for (size_t k = 0; k < CASES; k++) {
        unsigned char* bytes = 0;
        TinyPlan plan;
        failures += assertTrue(planFor(&cases[k], &bytes, &plan));

        TinySelectOpts opts = {0};
        opts.formats = everything;
        opts.uncharacterized = TINYIMG_SELECT_ALLOW;
        opts.min_db = 20.0f;
        opts.budget_us = 40000u;

        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_select(&plan, &opts, &other), TINYIMG_OK);
        failures += assertBytesMatch(&joint, &other, sizeof(joint));

        free(bytes);
    }

    return failures;
}

static int ignoresThePlansOwnBudget(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);

    tiny_plan_set_budget(&plan, 1u);
    tiny_plan_set_budget_basis(&plan, TINYIMG_BASIS_UPPER);
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &other), TINYIMG_OK);
    failures += assertBytesMatch(&joint, &other, sizeof(joint));

    free(bytes);
    return failures;
}

static int rejectsBadArguments(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    failures +=
        assertEquals(tiny_plan_select(0, &opts, &joint), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_plan_select_ladder(0, &opts, &joint), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_plan_select_independent(&plan, &opts, 0), TINYIMG_ERR_NULL
    );

    opts.objective = 3u;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_ERR_RANGE);
    opts.objective = 0u;
    opts.uncharacterized = 2u;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_ERR_RANGE);

    failures += assertEquals(tiny_plan_select(&plan, 0, &joint), TINYIMG_OK);

    failures += assertEquals(
        tiny_selection_field(0, TINYIMG_SELECTION_FIELD_FEASIBLE), 0
    );
    failures +=
        assertEquals(tiny_selection_field(&joint, (TinySelectionField) 999), 0);
    failures += assertEquals(
        tiny_selection_considered_field(
            &joint, 100000u, TINYIMG_CONSIDERED_FIELD_FORMAT
        ),
        0
    );
    failures += assertEquals(
        tiny_selection_considered_field(0, 0u, TINYIMG_CONSIDERED_FIELD_FORMAT),
        0
    );
    failures += assertEquals(
        tiny_selection_considered_field(&joint, 0u, (TinyConsideredField) 999),
        0
    );
    failures +=
        assertEquals(tiny_selection_sizeof(), (long) sizeof(TinySelection));
    failures +=
        assertEquals(tiny_select_opts_sizeof(), (long) sizeof(TinySelectOpts));

    free(bytes);
    return failures;
}

static int reportsTheNearestMissWhenNothingFits(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinySelectOpts opts = {0};
    opts.formats = 0x40000000u;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_REASON),
        TINYIMG_SELECT_REASON_FORMAT
    );

    // the floor is met and the budget is not, and the budget is the later check
    opts.formats = 0u;
    opts.min_db = 1.0f;
    opts.budget_us = 1u;
    failures +=
        assertEquals(tiny_plan_select(&plan, &opts, &joint), TINYIMG_OK);
    failures += assertEquals(
        field(&joint, TINYIMG_SELECTION_FIELD_REASON),
        TINYIMG_SELECT_REASON_BUDGET
    );

    free(bytes);
    return failures;
}

// #region run

static int sameOutput(const TinyImage* a, const TinyImage* b) {
    uint32_t size = tiny_image_getsize(a);

    return tiny_image_getwidth(a) == tiny_image_getwidth(b) &&
           tiny_image_getheight(a) == tiny_image_getheight(b) &&
           tiny_image_getchannels(a) == tiny_image_getchannels(b) &&
           size == tiny_image_getsize(b) &&
           memcmp(tiny_image_getdata(a), tiny_image_getdata(b), size) == 0;
}

static int runsALatticePointByIndex(void) {
    int failures = 0;
    unsigned char* bytes = 0;
    TinyPlan plan;

    failures += assertTrue(planFor(&cases[0], &bytes, &plan));

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );
    failures += assertTrue(count > 1u);

    for (uint32_t i = 0; i < count && i < TINYIMG_PLAN_MAX_CANDIDATES; i++) {
        TinyImage byIndex, byCandidate;
        memset(&byIndex, 0, sizeof(byIndex));
        memset(&byCandidate, 0, sizeof(byCandidate));

        failures +=
            assertEquals(tiny_plan_run_lattice(&plan, i, &byIndex), TINYIMG_OK);
        failures += assertEquals(
            tiny_plan_run_candidate(&plan, &lattice[i], &byCandidate),
            TINYIMG_OK
        );
        failures += assertTrue(sameOutput(&byIndex, &byCandidate));

        tiny_image_destroy(&byIndex);
        tiny_image_destroy(&byCandidate);
    }

    TinyImage out;
    memset(&out, 0, sizeof(out));
    failures += assertEquals(
        tiny_plan_run_lattice(&plan, count, &out), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_plan_run_lattice(0, 0u, &out), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_plan_run_lattice(&plan, 0u, 0), TINYIMG_ERR_NULL);

    free(bytes);
    return failures;
}

// #endregion

int main(void) {
    int failures = 0;

    tiny_init();

    failures += rejectsBadArguments();
    failures += searchesTheLatticeTimesTheVariants();
    failures += pricesTheTimeBoundFromThePublicFunctions();
    failures += alphaSourceNeverSelectsJpegOrAvif();
    failures += opaqueSourceMayUseJpeg();
    failures += aBudgetAtTheCheapestBoundSelectsIt();
    failures += theMachineFactorMovesTheBoundary();
    failures += pricesTheBoundOnlyAfterTheCheapChecks();
    failures += aFloorOf99NeedsAnExactCandidate();
    failures += everyFeasibleCandidateMeetsTheFloor();
    failures += refusesAnUncharacterizedClassUnlessAllowed();
    failures += anUnknownLeverCannotMeetAFloor();
    failures += theObjectivesPickTheirOwnMinimum();
    failures += minBytesPrefersTheCheapestPointOfTheCheapestFormat();
    failures += aPeakCapBelowTheCheapestPeakIsInfeasible();
    failures += theDefaultCapIsTheHeap();
    failures += theLadderIsAMemberOfTheLatticeAndNeverBeatsTheJointSearch();
    failures += theLadderTakesTheSourceFormatAtItsDefaultTier();
    failures += theLadderIgnoresThePeakCapUntilItIsChecked();
    failures += composesNoisePower();
    failures += addingNoiseNeverRaisesTheDb();
    failures += theWeakestClassWins();
    failures += composedClassIsTheWeakestOfTheParts();
    failures += candidateZeroNeverLosesQualityToALever();
    failures += carriesTheMeasuredCoverage();
    failures += isDeterministic();
    failures += ignoresThePlansOwnBudget();
    failures += reportsTheNearestMissWhenNothingFits();
    failures += runsALatticePointByIndex();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
