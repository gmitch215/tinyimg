#include "tinyimg/distortion.h"
#include "../test.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief The distortion table and the runner that measures a lever setting.
 *
 * The table is a compiled-in record of measurements, so what is checked here
 * is its shape: an unknown combination is not guessed at, the percentiles are
 * ordered, an EXACT row says 99 dB, and the data is consistent with the
 * ordering a deeper reduction implies. tiny_plan_run_candidate is what the
 * measurement runs every lattice point through, so it has to produce the bytes
 * tiny_plan_run produces at that lever setting.
 */

static const char* const fixtures[] = {
    "ball_kick.gif",    "budapest.jpg",       "dartmouth.jpg",
    "dartmouth.tiff",   "digicam.jpg",        "dog.jpg",
    "drupflare.png",    "face_art.jpg",       "family.jpg",
    "flower.jpg",       "forest.png",         "lake_sunapee.jpeg",
    "lil_bit.png",      "long_walk.jpg",      "man.jpg",
    "moped.jpg",        "mountains.jpg",      "mushroom.jpg",
    "road.jpg",         "sf-24.jpg",          "smile.jpg",
    "steam.png",        "toyota_racing.webp", "webassembly.png",
    "winter_cabin.jpg", "winter_forest.png",  "woman.jpg",
};

#define FIXTURE_COUNT (sizeof(fixtures) / sizeof(fixtures[0]))

static int plan_for(
    const char* name, uint32_t width, unsigned char** bytes, TinyPlan* plan
) {
    size_t size = 0;
    *bytes = readFixture(name, &size);
    if (!*bytes) return 0;

    if (tiny_plan_init(plan, *bytes, size) != TINYIMG_OK) return 0;
    return tiny_plan_resize(plan, width, 0u) == TINYIMG_OK;
}

static int sameImage(const TinyImage* a, const TinyImage* b) {
    return a->width == b->width && a->height == b->height &&
           a->channels == b->channels &&
           memcmp(
               a->data, b->data, (size_t) a->width * a->height * a->channels
           ) == 0;
}

// #region the table

static int doesNotGuessAtAnUnknownCombination(void) {
    int failures = 0;
    TinyDistortion d;
    TinyEncodeDistortion e;

    memset(&d, 0xAB, sizeof(d));
    failures += assertEquals(
        tiny_distortion_lever(TINYIMG_FORMAT_BMP, 7u, 7u, 7u, &d),
        TINYIMG_DISTORTION_UNCHARACTERIZED
    );
    failures += assertEquals((long) d.klass, 0L);
    failures += assertEquals((long) d.n, 0L);
    failures += assertFloatEquals(d.p05_db, 0.0f, 0.0f);

    failures += assertEquals(
        tiny_distortion_lever(TINYIMG_FORMAT_UNKNOWN, 0u, 0u, 0u, 0),
        TINYIMG_DISTORTION_UNCHARACTERIZED
    );

    memset(&e, 0xAB, sizeof(e));
    failures += assertEquals(
        tiny_distortion_encode(TINYIMG_FORMAT_JPEG, 77u, &e),
        TINYIMG_DISTORTION_UNCHARACTERIZED
    );
    failures += assertEquals((long) e.n, 0L);
    failures += assertFloatEquals(e.bytes_per_pixel, 0.0f, 0.0f);

    failures += assertEquals(
        tiny_distortion_encode(TINYIMG_FORMAT_BMP, 75u, 0),
        TINYIMG_DISTORTION_UNCHARACTERIZED
    );

    uint32_t levers = 0;
    tiny_distortion_rows(&levers, 0);
    failures += assertEquals(
        tiny_distortion_lever_row(levers, 0, 0, 0, 0, 0), TINYIMG_ERR_RANGE
    );

    return failures;
}

static int ordersEveryLeverRow(void) {
    int failures = 0;
    uint32_t levers = 0;
    tiny_distortion_rows(&levers, 0);

    failures += assertGreaterThan((double) levers, 0.0);

    int sawExact = 0;
    int sawEmpirical = 0;

    for (uint32_t i = 0; i < levers; i++) {
        TinyImageFormat f;
        uint8_t ed, fd, ss;
        TinyDistortion d;

        failures += assertEquals(
            tiny_distortion_lever_row(i, &f, &ed, &fd, &ss, &d), TINYIMG_OK
        );

        // the lookup finds the row the walk read
        TinyDistortion found;
        failures += assertEquals(
            tiny_distortion_lever(f, ed, fd, ss, &found), (long) d.klass
        );
        failures += assertFloatEquals(found.median_db, d.median_db, 0.0f);

        failures += assertTrue(d.p05_db <= d.median_db);
        failures += assertTrue(d.n >= 8u);
        failures += assertTrue(
            d.klass == TINYIMG_DISTORTION_EXACT ||
            d.klass == TINYIMG_DISTORTION_EMPIRICAL
        );

        if (d.klass == TINYIMG_DISTORTION_EXACT) {
            sawExact = 1;
            failures += assertFloatEquals(d.p05_db, 99.0f, 0.0f);
            failures += assertFloatEquals(d.median_db, 99.0f, 0.0f);
            failures += assertEquals((long) d.max_abs, 0L);
        }
        else {
            sawEmpirical = 1;
            failures += assertTrue(d.p05_db < 99.0f);
        }

        // the reference against itself is the one row every format has
        if (ed == 0u && fd == 0u && ss == 0u) {
            failures += assertEquals((long) d.klass, TINYIMG_DISTORTION_EXACT);
        }
    }

    failures += assertTrue(sawExact);
    failures += assertTrue(sawEmpirical);
    return failures;
}

static int ordersEveryEncodeRow(void) {
    static const TinyImageFormat formats[] = {
        TINYIMG_FORMAT_JPEG, TINYIMG_FORMAT_WEBP, TINYIMG_FORMAT_AVIF,
        TINYIMG_FORMAT_PNG,  TINYIMG_FORMAT_GIF,
    };
    static const uint8_t qualities[] = {50, 60, 70, 75, 80, 85, 90, 95};

    int failures = 0;
    int characterized = 0;

    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        float lastBytes = 0.0f;

        for (size_t q = 0; q < sizeof(qualities); q++) {
            TinyEncodeDistortion e;
            TinyDistortionClass klass =
                tiny_distortion_encode(formats[f], qualities[q], &e);

            if (klass == TINYIMG_DISTORTION_UNCHARACTERIZED) continue;
            characterized++;

            failures += assertEquals((long) e.klass, (long) klass);
            failures += assertTrue(e.p05_db <= e.median_db);
            failures += assertTrue(e.bytes_per_pixel <= e.bytes_per_pixel_p95);
            failures += assertGreaterThan((double) e.bytes_per_pixel, 0.0);
            failures += assertTrue(e.n >= 8u);

            if (klass == TINYIMG_DISTORTION_EXACT) {
                failures += assertFloatEquals(e.p05_db, 99.0f, 0.0f);
                failures += assertFloatEquals(e.median_db, 99.0f, 0.0f);
            }

            // a higher quality never costs fewer bytes in a lossy encoder
            if (formats[f] != TINYIMG_FORMAT_PNG &&
                formats[f] != TINYIMG_FORMAT_GIF) {
                failures += assertTrue(e.bytes_per_pixel >= lastBytes);
            }
            lastBytes = e.bytes_per_pixel;
        }
    }

    failures += assertGreaterThan((double) characterized, 0.0);
    return failures;
}

// a deeper reduction cannot sit closer to the reference than a shallower one
static int medianNeverRisesWithDeeperReduction(void) {
    int failures = 0;
    uint32_t levers = 0;
    tiny_distortion_rows(&levers, 0);

    for (uint32_t i = 0; i < levers; i++) {
        TinyImageFormat f;
        uint8_t ed, fd, ss;
        TinyDistortion d;

        tiny_distortion_lever_row(i, &f, &ed, &fd, &ss, &d);
        if (ss == 0u) continue;

        // the nearest shallower row in the same chain
        for (int back = (int) ss - 1; back >= 0; back--) {
            TinyDistortion shallower;

            if (tiny_distortion_lever(f, ed, fd, (uint8_t) back, &shallower) ==
                TINYIMG_DISTORTION_UNCHARACTERIZED) {
                continue;
            }

            if (d.median_db > shallower.median_db) {
                printf(
                    "format %d effort %u filter %u: steps %u median %.2f "
                    "above steps %d median %.2f\n",
                    (int) f, ed, fd, ss, (double) d.median_db, back,
                    (double) shallower.median_db
                );
                failures += 1;
            }
            break;
        }
    }

    return failures;
}

// #endregion

// #region the runner

static int rejectsMissingArguments(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for("sf-24.jpg", 200u, &bytes, &plan)) return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(
        tiny_plan_run_candidate(0, &lattice[0], &out), TINYIMG_ERR_NULL
    );
    failures +=
        assertEquals(tiny_plan_run_candidate(&plan, 0, &out), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_plan_run_candidate(&plan, &lattice[0], 0), TINYIMG_ERR_NULL
    );

    free(bytes);
    return failures;
}

static int candidateZeroIsThePlansOwnRun(void) {
    int failures = 0;

    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        unsigned char* bytes = 0;
        TinyPlan plan;

        if (!plan_for(fixtures[i], 160u, &bytes, &plan)) {
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

        TinyImage ran, candidate;
        memset(&ran, 0, sizeof(ran));
        memset(&candidate, 0, sizeof(candidate));

        failures += assertEquals(tiny_plan_run(&plan, &ran), TINYIMG_OK);
        failures += assertEquals(
            tiny_plan_run_candidate(&plan, &lattice[0], &candidate), TINYIMG_OK
        );

        if (!sameImage(&ran, &candidate)) {
            printf("%s: candidate 0 differs from tiny_plan_run\n", fixtures[i]);
            failures += 1;
        }

        tiny_image_destroy(&ran);
        tiny_image_destroy(&candidate);
        free(bytes);
    }

    return failures;
}

static int budgetPicksTheCandidateThatRunsTheSame(void) {
    static const char* const names[] = {
        "sf-24.jpg",         "road.jpg",       "toyota_racing.webp",
        "winter_forest.png", "dartmouth.tiff", "ball_kick.gif",
    };
    static const uint32_t widths[] = {300u};

    int failures = 0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
            unsigned char* bytes = 0;
            TinyPlan plan;

            if (!plan_for(names[i], widths[w], &bytes, &plan)) {
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
            failures +=
                assertEquals(tiny_plan_resolve(&plan, &base), TINYIMG_OK);

            uint32_t budgets[2] = {1u, base.cost / 2u};

            for (int b = 0; b < 2; b++) {
                tiny_plan_set_budget(&plan, budgets[b]);

                TinyPlanResolution picked;
                failures +=
                    assertEquals(tiny_plan_resolve(&plan, &picked), TINYIMG_OK);

                const TinyPlanCandidate* match = 0;
                for (uint32_t k = 0; k < count; k++) {
                    if (lattice[k].effort == picked.decode.effort &&
                        lattice[k].scale_den == picked.decode.scale_den &&
                        lattice[k].filter_x == picked.filter_x &&
                        lattice[k].filter_y == picked.filter_y) {
                        match = &lattice[k];
                        break;
                    }
                }

                if (!match) {
                    printf(
                        "%s at %u wide, budget %u: no candidate\n", names[i],
                        widths[w], budgets[b]
                    );
                    failures += 1;
                    continue;
                }

                TinyImage ran, candidate;
                memset(&ran, 0, sizeof(ran));
                memset(&candidate, 0, sizeof(candidate));

                failures +=
                    assertEquals(tiny_plan_run(&plan, &ran), TINYIMG_OK);
                failures += assertEquals(
                    tiny_plan_run_candidate(&plan, match, &candidate),
                    TINYIMG_OK
                );

                if (!sameImage(&ran, &candidate)) {
                    printf(
                        "%s at %u wide, budget %u: candidate differs from "
                        "the budgeted run\n",
                        names[i], widths[w], budgets[b]
                    );
                    failures += 1;
                }

                tiny_image_destroy(&ran);
                tiny_image_destroy(&candidate);
            }

            free(bytes);
        }
    }

    return failures;
}

static int keepsTheOutputExtentAtEveryCandidate(void) {
    int failures = 0;

    unsigned char* bytes = 0;
    TinyPlan plan;
    if (!plan_for("sf-24.jpg", 300u, &bytes, &plan)) return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    failures += assertEquals(
        tiny_plan_enumerate(
            &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
        ),
        TINYIMG_OK
    );
    failures += assertGreaterThan((double) count, 1.0);

    TinyImage first;
    memset(&first, 0, sizeof(first));
    failures += assertEquals(
        tiny_plan_run_candidate(&plan, &lattice[0], &first), TINYIMG_OK
    );

    for (uint32_t k = 1; k < count; k++) {
        TinyImage out;
        memset(&out, 0, sizeof(out));

        failures += assertEquals(
            tiny_plan_run_candidate(&plan, &lattice[k], &out), TINYIMG_OK
        );
        failures += assertEquals((long) out.width, (long) first.width);
        failures += assertEquals((long) out.height, (long) first.height);
        failures += assertEquals((long) out.channels, (long) first.channels);

        tiny_image_destroy(&out);
    }

    tiny_image_destroy(&first);
    free(bytes);
    return failures;
}

// #endregion

int main(void) {
    int failures = 0;

    tiny_init();

    failures += doesNotGuessAtAnUnknownCombination();
    failures += ordersEveryLeverRow();
    failures += ordersEveryEncodeRow();
    failures += medianNeverRisesWithDeeperReduction();
    failures += rejectsMissingArguments();
    failures += candidateZeroIsThePlansOwnRun();
    failures += budgetPicksTheCandidateThatRunsTheSame();
    failures += keepsTheOutputExtentAtEveryCandidate();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
