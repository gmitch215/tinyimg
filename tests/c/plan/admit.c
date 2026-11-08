#include "../test.h"
#include "tinyimg/plan.h"

/**
 * @file
 * @brief Admission on the calibrated bound.
 *
 * A request is admitted when its upper bound, times the caller's machine
 * factor, fits the budget. The properties are the decision table, that STRICT
 * never gives anything up, that DEGRADE only settles on a rung whose bound
 * fits, that the factor scales the bound exactly, and that the point basis the
 * ladder has always used is unchanged.
 */

static int admitAt(
    const unsigned char* bytes, size_t size, uint32_t width, uint32_t budget,
    uint32_t scale, TinyAdmitMode mode, TinyAdmission* out
) {
    TinyPlan plan;

    int result = tiny_plan_init(&plan, bytes, size);
    if (result != TINYIMG_OK) return result;

    result = tiny_plan_resize(&plan, width, 0u);
    if (result != TINYIMG_OK) return result;

    TinyAdmitOpts opts = {
        budget, scale, (uint32_t) mode, TINYIMG_FORMAT_WEBP, 0u, 0u, 0u, 0u
    };

    return tiny_plan_admit(&plan, &opts, out);
}

static uint32_t field(const TinyAdmission* admission, TinyAdmissionField name) {
    return tiny_admission_field(admission, name);
}

/** The table: exactly at the bound, one under, well over, and nothing. */
static int decidesOnTheBound(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyAdmission whole;
    failures += assertEquals(
        admitAt(
            bytes, size, 200u, 0xFFFFFFFFu, 1000u, TINYIMG_ADMIT_STRICT, &whole
        ),
        TINYIMG_OK
    );

    uint32_t bound = field(&whole, TINYIMG_ADMISSION_FIELD_COST_UPPER);

    failures += assertEquals(
        (long) field(&whole, TINYIMG_ADMISSION_FIELD_DECISION),
        (long) TINYIMG_ADMISSION_ADMIT
    );
    failures += assertGreaterThan((double) bound, 0.0);
    failures += assertEquals(
        (long) field(&whole, TINYIMG_ADMISSION_FIELD_DEGRADED), 0L
    );

    // the bound never sits under the point estimate it is a bound on
    failures +=
        assertTrue(bound >= field(&whole, TINYIMG_ADMISSION_FIELD_COST));

    static const TinyAdmitMode modes[2] = {
        TINYIMG_ADMIT_STRICT, TINYIMG_ADMIT_DEGRADE
    };

    for (int m = 0; m < 2; m++) {
        TinyAdmission exact;
        failures += assertEquals(
            admitAt(bytes, size, 200u, bound, 1000u, modes[m], &exact),
            TINYIMG_OK
        );

        // exactly at the bound is within budget, with no margin left
        failures += assertEquals(
            (long) field(&exact, TINYIMG_ADMISSION_FIELD_DECISION),
            (long) TINYIMG_ADMISSION_ADMIT
        );
        failures += assertEquals(
            (long) (int32_t) field(&exact, TINYIMG_ADMISSION_FIELD_MARGIN), 0L
        );
        failures += assertEquals(
            (long) field(&exact, TINYIMG_ADMISSION_FIELD_DEGRADED), 0L
        );

        TinyAdmission roomy;
        failures += assertEquals(
            admitAt(bytes, size, 200u, bound * 3u, 1000u, modes[m], &roomy),
            TINYIMG_OK
        );
        failures += assertEquals(
            (long) field(&roomy, TINYIMG_ADMISSION_FIELD_DECISION),
            (long) TINYIMG_ADMISSION_ADMIT
        );
        failures += assertEquals(
            (long) (int32_t) field(&roomy, TINYIMG_ADMISSION_FIELD_MARGIN),
            (long) (bound * 2u)
        );

        // a budget nothing meets is rejected in both modes
        TinyAdmission none;
        failures += assertEquals(
            admitAt(bytes, size, 200u, 1u, 1000u, modes[m], &none), TINYIMG_OK
        );
        failures += assertEquals(
            (long) field(&none, TINYIMG_ADMISSION_FIELD_DECISION),
            (long) TINYIMG_ADMISSION_REJECT
        );
        failures += assertLessThan(
            (double) (int32_t) field(&none, TINYIMG_ADMISSION_FIELD_MARGIN), 0.0
        );
    }

    // one under the bound: strict rejects with the undegraded numbers
    TinyAdmission under;
    failures += assertEquals(
        admitAt(
            bytes, size, 200u, bound - 1u, 1000u, TINYIMG_ADMIT_STRICT, &under
        ),
        TINYIMG_OK
    );
    failures += assertEquals(
        (long) field(&under, TINYIMG_ADMISSION_FIELD_DECISION),
        (long) TINYIMG_ADMISSION_REJECT
    );
    failures += assertEquals(
        (long) field(&under, TINYIMG_ADMISSION_FIELD_DEGRADED), 0L
    );
    failures += assertEquals(
        (long) field(&under, TINYIMG_ADMISSION_FIELD_COST_UPPER), (long) bound
    );
    failures += assertEquals(
        (long) (int32_t) field(&under, TINYIMG_ADMISSION_FIELD_MARGIN), -1L
    );

    free(bytes);
    return failures;
}

/** Degrade settles on a rung whose bound fits, or rejects. */
static int degradesOnTheUpperBound(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("mountains.jpg", &size);
    if (!bytes) return 1;

    TinyAdmission whole;
    failures += assertEquals(
        admitAt(
            bytes, size, 600u, 0xFFFFFFFFu, 1000u, TINYIMG_ADMIT_DEGRADE, &whole
        ),
        TINYIMG_OK
    );
    uint32_t bound = field(&whole, TINYIMG_ADMISSION_FIELD_COST_UPPER);

    // find the cheapest budget the ladder meets by halving until it stops
    // fitting, then check every figure it reported on the way down
    int degraded = 0;
    int rejected = 0;

    for (uint32_t budget = bound - 1u; budget > 1u;
         budget = budget * 9u / 10u) {
        TinyAdmission got;
        failures += assertEquals(
            admitAt(
                bytes, size, 600u, budget, 1000u, TINYIMG_ADMIT_DEGRADE, &got
            ),
            TINYIMG_OK
        );

        uint32_t decision = field(&got, TINYIMG_ADMISSION_FIELD_DECISION);
        uint32_t upper = field(&got, TINYIMG_ADMISSION_FIELD_COST_UPPER);

        if (decision == TINYIMG_ADMISSION_DEGRADED) {
            degraded++;

            // the rung it took fits, and took something to get there
            failures += assertTrue(upper <= budget);
            failures += assertTrue(upper < bound);
            failures +=
                assertTrue(field(&got, TINYIMG_ADMISSION_FIELD_DEGRADED) != 0u);
            failures += assertTrue(
                (int32_t) field(&got, TINYIMG_ADMISSION_FIELD_MARGIN) >= 0
            );
        }
        else {
            failures +=
                assertEquals((long) decision, (long) TINYIMG_ADMISSION_REJECT);
            rejected++;

            // a rejection reports how far over the cheapest rung still was
            failures += assertTrue(upper > budget);
            failures += assertTrue(
                (int32_t) field(&got, TINYIMG_ADMISSION_FIELD_MARGIN) < 0
            );
        }
    }

    // the ladder both rescued requests and ran out, on this one fixture
    failures += assertTrue(degraded > 0);
    failures += assertTrue(rejected > 0);

    // strict at the same budgets never gives anything up
    for (uint32_t budget = bound - 1u; budget > 1u;
         budget = budget * 9u / 10u) {
        TinyAdmission got;
        failures += assertEquals(
            admitAt(
                bytes, size, 600u, budget, 1000u, TINYIMG_ADMIT_STRICT, &got
            ),
            TINYIMG_OK
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_DECISION),
            (long) TINYIMG_ADMISSION_REJECT
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_DEGRADED), 0L
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_COST_UPPER), (long) bound
        );
    }

    free(bytes);
    return failures;
}

/** The machine factor multiplies the bound, and moves the decision with it. */
static int scalesTheBound(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyAdmission one;
    TinyAdmission two;
    TinyAdmission half;

    failures += assertEquals(
        admitAt(
            bytes, size, 200u, 0xFFFFFFFFu, 1000u, TINYIMG_ADMIT_STRICT, &one
        ),
        TINYIMG_OK
    );
    failures += assertEquals(
        admitAt(
            bytes, size, 200u, 0xFFFFFFFFu, 2000u, TINYIMG_ADMIT_STRICT, &two
        ),
        TINYIMG_OK
    );
    failures += assertEquals(
        admitAt(
            bytes, size, 200u, 0xFFFFFFFFu, 500u, TINYIMG_ADMIT_STRICT, &half
        ),
        TINYIMG_OK
    );

    uint32_t bound = field(&one, TINYIMG_ADMISSION_FIELD_COST_UPPER);

    failures += assertEquals(
        (long) field(&two, TINYIMG_ADMISSION_FIELD_COST_UPPER),
        (long) (bound * 2u)
    );
    failures += assertEquals(
        (long) field(&two, TINYIMG_ADMISSION_FIELD_COST),
        (long) (field(&one, TINYIMG_ADMISSION_FIELD_COST) * 2u)
    );
    failures += assertEquals(
        (long) field(&two, TINYIMG_ADMISSION_FIELD_ENCODE),
        (long) (field(&one, TINYIMG_ADMISSION_FIELD_ENCODE) * 2u)
    );

    // rounded up, so a half never reports less than the true half
    failures += assertTrue(
        field(&half, TINYIMG_ADMISSION_FIELD_COST_UPPER) * 2u >= bound
    );

    // a budget the bound meets on the calibrated machine is not met on one
    // twice as slow
    TinyAdmission fits;
    TinyAdmission misses;

    failures += assertEquals(
        admitAt(bytes, size, 200u, bound, 1000u, TINYIMG_ADMIT_STRICT, &fits),
        TINYIMG_OK
    );
    failures += assertEquals(
        admitAt(bytes, size, 200u, bound, 2000u, TINYIMG_ADMIT_STRICT, &misses),
        TINYIMG_OK
    );
    failures += assertEquals(
        (long) field(&fits, TINYIMG_ADMISSION_FIELD_DECISION),
        (long) TINYIMG_ADMISSION_ADMIT
    );
    failures += assertEquals(
        (long) field(&misses, TINYIMG_ADMISSION_FIELD_DECISION),
        (long) TINYIMG_ADMISSION_REJECT
    );

    free(bytes);
    return failures;
}

/** The encoder is in the total, priced at the format being written. */
static int includesTheEncoder(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plan, 200u, 0u), TINYIMG_OK);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), TINYIMG_OK);

    static const TinyImageFormat formats[3] = {
        TINYIMG_FORMAT_UNKNOWN, TINYIMG_FORMAT_JPEG, TINYIMG_FORMAT_PNG
    };

    uint32_t last = 0;

    for (int i = 0; i < 3; i++) {
        TinyAdmitOpts opts = {0xFFFFFFFFu,
                              1000u,
                              TINYIMG_ADMIT_STRICT,
                              (uint32_t) formats[i],
                              0u,
                              0u,
                              0u,
                              0u};
        TinyAdmission got;

        failures +=
            assertEquals(tiny_plan_admit(&plan, &opts, &got), TINYIMG_OK);

        uint32_t encode = tiny_encode_cost_at(
            formats[i], resolution.width, resolution.height, 0u
        );
        uint32_t upper = tiny_encode_cost_upper_at(
            formats[i], resolution.width, resolution.height, 0u
        );

        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_ENCODE), (long) encode
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_ENCODE_UPPER),
            (long) upper
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_COST),
            (long) (resolution.cost + encode)
        );
        failures += assertEquals(
            (long) field(&got, TINYIMG_ADMISSION_FIELD_COST_UPPER),
            (long) (resolution.cost_upper + upper)
        );

        // an unpriced format adds nothing, and the dearer encoder adds more
        if (i == 0) failures += assertEquals((long) encode, 0L);
        failures +=
            assertTrue(field(&got, TINYIMG_ADMISSION_FIELD_COST_UPPER) >= last);
        last = field(&got, TINYIMG_ADMISSION_FIELD_COST_UPPER);
    }

    // quality and lossless reach the price, so a request that is dearer to
    // encode is admitted against a larger bound
    TinyAdmitOpts plain = {
        0xFFFFFFFFu, 1000u, TINYIMG_ADMIT_STRICT, TINYIMG_FORMAT_WEBP, 0u, 0u,
        0u,          0u
    };
    TinyAdmitOpts lossless = plain;
    TinyAdmitOpts crisp = plain;
    TinyAdmission base;
    TinyAdmission dear;
    TinyAdmission high;

    lossless.lossless = 1u;
    crisp.quality = 95u;

    failures += assertEquals(tiny_plan_admit(&plan, &plain, &base), TINYIMG_OK);
    failures +=
        assertEquals(tiny_plan_admit(&plan, &lossless, &dear), TINYIMG_OK);
    failures += assertEquals(tiny_plan_admit(&plan, &crisp, &high), TINYIMG_OK);

    failures += assertGreaterThan(
        (double) field(&dear, TINYIMG_ADMISSION_FIELD_ENCODE_UPPER),
        (double) field(&base, TINYIMG_ADMISSION_FIELD_ENCODE_UPPER)
    );
    failures += assertGreaterThan(
        (double) field(&high, TINYIMG_ADMISSION_FIELD_ENCODE_UPPER),
        (double) field(&base, TINYIMG_ADMISSION_FIELD_ENCODE_UPPER)
    );

    free(bytes);
    return failures;
}

/** The point basis is the ladder every earlier plan ran, and is the default. */
static int pointBasisIsUnchanged(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("mountains.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plain;
    TinyPlan point;
    TinyPlan upper;

    failures += assertEquals(tiny_plan_init(&plain, bytes, size), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resize(&plain, 600u, 0u), TINYIMG_OK);
    point = plain;
    upper = plain;

    failures += assertEquals(
        tiny_plan_set_budget_basis(&point, TINYIMG_BASIS_POINT), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_plan_set_budget_basis(&upper, TINYIMG_BASIS_UPPER), TINYIMG_OK
    );
    failures +=
        assertEquals(tiny_plan_set_budget_basis(&plain, 2u), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_plan_set_budget_basis(0, 0u), TINYIMG_ERR_NULL);

    TinyPlanResolution none;
    failures += assertEquals(tiny_plan_resolve(&plain, &none), TINYIMG_OK);

    // the default and an explicit point basis resolve identically at every
    // budget the ladder can be asked about
    uint32_t budgets[6] = {
        0u, 1u, none.cost / 2u, none.cost - 1u, none.cost, none.cost * 2u
    };

    for (int i = 0; i < 6; i++) {
        TinyPlanResolution a;
        TinyPlanResolution b;

        failures +=
            assertEquals(tiny_plan_set_budget(&plain, budgets[i]), TINYIMG_OK);
        failures +=
            assertEquals(tiny_plan_set_budget(&point, budgets[i]), TINYIMG_OK);
        failures += assertEquals(tiny_plan_resolve(&plain, &a), TINYIMG_OK);
        failures += assertEquals(tiny_plan_resolve(&point, &b), TINYIMG_OK);

        failures += assertEquals((long) a.cost, (long) b.cost);
        failures += assertEquals((long) a.cost_upper, (long) b.cost_upper);
        failures += assertEquals((long) a.degraded, (long) b.degraded);
        failures +=
            assertEquals((long) a.decode.scale_den, (long) b.decode.scale_den);
        failures +=
            assertEquals((long) a.decode.effort, (long) b.decode.effort);
        failures += assertEquals((long) a.filter_x, (long) b.filter_x);
    }

    // a budget the point estimate meets but the bound does not: the point
    // basis leaves the request alone and the upper basis takes a lever
    failures += assertGreaterThan((double) none.cost_upper, (double) none.cost);

    failures +=
        assertEquals(tiny_plan_set_budget(&point, none.cost), TINYIMG_OK);
    failures +=
        assertEquals(tiny_plan_set_budget(&upper, none.cost), TINYIMG_OK);

    TinyPlanResolution kept;
    TinyPlanResolution eased;

    failures += assertEquals(tiny_plan_resolve(&point, &kept), TINYIMG_OK);
    failures += assertEquals(tiny_plan_resolve(&upper, &eased), TINYIMG_OK);

    failures += assertEquals((long) kept.degraded, 0L);
    failures += assertTrue(eased.degraded != 0u);
    failures +=
        assertLessThan((double) eased.cost_upper, (double) none.cost_upper);

    free(bytes);
    return failures;
}

static int refusesBadArguments(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    failures += assertEquals(tiny_plan_init(&plan, bytes, size), TINYIMG_OK);

    TinyAdmitOpts opts = {1000u, 1000u, TINYIMG_ADMIT_STRICT, 0u, 0u, 0u,
                          0u,    0u};
    TinyAdmission got;

    failures += assertEquals(tiny_plan_admit(0, &opts, &got), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_plan_admit(&plan, 0, &got), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_plan_admit(&plan, &opts, 0), TINYIMG_ERR_NULL);

    opts.scale_milli = 0u;
    failures +=
        assertEquals(tiny_plan_admit(&plan, &opts, &got), TINYIMG_ERR_RANGE);

    opts.scale_milli = 1000u;
    opts.mode = 7u;
    failures +=
        assertEquals(tiny_plan_admit(&plan, &opts, &got), TINYIMG_ERR_RANGE);

    failures += assertEquals(
        (long) tiny_admission_field(0, TINYIMG_ADMISSION_FIELD_COST), 0L
    );
    failures += assertEquals(
        (long) tiny_admission_field(&got, (TinyAdmissionField) 99), 0L
    );
    failures += assertTrue(tiny_admission_sizeof() >= 7u * 4u);
    failures += assertEquals((long) tiny_admit_opts_sizeof(), 8L * 4L);

    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += decidesOnTheBound();
    failures += degradesOnTheUpperBound();
    failures += scalesTheBound();
    failures += includesTheEncoder();
    failures += pointBasisIsUnchanged();
    failures += refusesBadArguments();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
