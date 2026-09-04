#include "tinyimg/plan.h"

#include "test.h"

/** The five operations every orientation is built out of. */
static const char* const TURN_NAMES[5] = {
    "flip h", "flip v", "rotate 90", "rotate 180", "rotate 270"
};

static int append_turn(TinyPlan* plan, uint32_t which) {
    switch (which) {
        case 0: return tiny_plan_flip_horizontal(plan);
        case 1: return tiny_plan_flip_vertical(plan);
        case 2: return tiny_plan_rotate(plan, 90);
        case 3: return tiny_plan_rotate(plan, 180);
        default: return tiny_plan_rotate(plan, 270);
    }
}

/** An image whose every pixel identifies its own position. */
static int make_marked(TinyImage* image, uint32_t width, uint32_t height) {
    if (tiny_image_create(image, width, height, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* pixel = image->data + ((size_t) y * width + x) * 3u;
            pixel[0] = (uint8_t) (x * 17u + 1u);
            pixel[1] = (uint8_t) (y * 29u + 2u);
            pixel[2] = (uint8_t) (x * 5u + y * 7u + 3u);
        }
    }

    return 1;
}

/**
 * @brief Runs a plan fused and unfused and compares the two.
 *
 * @param plan The plan, whose fusion setting is left as it was found.
 * @param name What to print when they differ.
 * @param floorDb A PSNR floor, or zero to demand the same bytes.
 * @return int 0 when they agree.
 */
static int both_ways(TinyPlan* plan, const char* name, double floorDb) {
    TinyImage fused;
    TinyImage eager;
    memset(&fused, 0, sizeof(fused));
    memset(&eager, 0, sizeof(eager));

    tiny_plan_set_fusion(plan, 1);
    int a = tiny_plan_run(plan, &fused);

    tiny_plan_set_fusion(plan, 0);
    int b = tiny_plan_run(plan, &eager);

    tiny_plan_set_fusion(plan, 1);

    if (a != TINYIMG_OK || b != TINYIMG_OK) {
        printf("#%d: FAIL\n", count);
        printf("#%d: %s: fused %d, eager %d\n", count, name, a, b);
        count++;
        tiny_image_destroy(&fused);
        tiny_image_destroy(&eager);
        return 1;
    }

    int failures = 0;

    if (fused.width != eager.width || fused.height != eager.height ||
        fused.channels != eager.channels) {
        printf("#%d: FAIL\n", count);
        printf(
            "#%d: %s: fused %ux%ux%u, eager %ux%ux%u\n", count, name,
            fused.width, fused.height, fused.channels, eager.width,
            eager.height, eager.channels
        );
        count++;
        failures = 1;
    }
    else if (floorDb <= 0.0) {
        failures = assertImageEquals(&fused, &eager);
    }
    else {
        size_t bytes = (size_t) fused.width * fused.height * fused.channels;
        failures = assertPSNR(fused.data, eager.data, bytes, floorDb);
    }

    tiny_image_destroy(&fused);
    tiny_image_destroy(&eager);
    return failures;
}

/**
 * @brief Every composition of flips and turns, against applying them one at a
 * time.
 *
 * The eight orientations are a group, so composing them is a two by two matrix
 * multiply rather than a chain of images. This walks every pair and every
 * triple of the five generators, which reaches every element by every route,
 * and compares the one composed pass against the two or three separate ones.
 * That is 150 cases and it is exhaustive, which matters because a sign error in
 * the composition is invisible on a symmetric image and wrong on every other.
 */
static int orientations(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 5, 3)) return 1;

    for (uint32_t a = 0; a < 5u; a++) {
        for (uint32_t b = 0; b < 5u; b++) {
            TinyPlan plan;
            tiny_plan_init_image(&plan, &marked);
            append_turn(&plan, a);
            append_turn(&plan, b);

            char name[64];
            snprintf(
                name, sizeof(name), "%s then %s", TURN_NAMES[a], TURN_NAMES[b]
            );
            failures += both_ways(&plan, name, 0.0);

            for (uint32_t c = 0; c < 5u; c++) {
                TinyPlan triple;
                tiny_plan_init_image(&triple, &marked);
                append_turn(&triple, a);
                append_turn(&triple, b);
                append_turn(&triple, c);

                snprintf(
                    name, sizeof(name), "%s, %s, %s", TURN_NAMES[a],
                    TURN_NAMES[b], TURN_NAMES[c]
                );
                failures += both_ways(&triple, name, 0.0);
            }
        }
    }

    tiny_image_destroy(&marked);
    return failures;
}

/** A quarter turn, worked out by hand on an image that cannot hide an error. */
static int turn_by_hand(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 4, 2)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_rotate(&plan, 90);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 2);
    failures += assertEquals(out.height, 4);

    // turning clockwise puts the bottom left of the source at the top left of
    // the result, so output (0,0) reads source (0, 1)
    for (uint32_t j = 0; j < 4u; j++) {
        for (uint32_t i = 0; i < 2u; i++) {
            const uint8_t* got = out.data + ((size_t) j * 2u + i) * 3u;
            const uint8_t* want =
                marked.data + ((size_t) (1u - i) * 4u + j) * 3u;

            failures += assertBytesMatch(got, want, 3);
        }
    }

    tiny_image_destroy(&out);
    tiny_image_destroy(&marked);
    return failures;
}

/** A crop reaches the output without reading a pixel through a filter. */
static int crop_is_a_copy(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 32, 16)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_crop(&plan, 5, 3, 10, 8);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_RESAMPLE) != 0);
    failures += assertTrue((res.kernels & TINYIMG_KERNEL_REGION) != 0);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 10);
    failures += assertEquals(out.height, 8);

    for (uint32_t y = 0; y < 8u; y++) {
        const uint8_t* got = out.data + (size_t) y * 10u * 3u;
        const uint8_t* want = marked.data + ((size_t) (y + 3u) * 32u + 5u) * 3u;

        failures += assertBytesMatch(got, want, 30);
    }

    tiny_image_destroy(&out);
    tiny_image_destroy(&marked);
    return failures;
}

/**
 * @brief A resize to the extent it already has changes nothing at all.
 *
 * The interesting part is that this holds for every filter, including the cubic
 * one: at a whole sample position the Catmull-Rom weights are one and three
 * zeroes, so the identity comes out of the arithmetic rather than out of a
 * special case.
 */
static int identity_resample(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 16, 9)) return 1;

    TinyResampleFilter filters[4] = {
        TINYIMG_FILTER_NEAREST, TINYIMG_FILTER_BILINEAR, TINYIMG_FILTER_BOX,
        TINYIMG_FILTER_CATMULL_ROM
    };

    for (uint32_t i = 0; i < 4u; i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &marked);

        // a resize to the current extent is eliminated, so the filter is
        // reached through a crop that leaves the extent alone
        tiny_plan_crop(&plan, 1, 1, 15, 8);
        tiny_plan_resize_with(&plan, 15, 8, filters[i]);

        TinyImage out;
        memset(&out, 0, sizeof(out));

        failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
        failures += assertEquals(out.width, 15);

        for (uint32_t y = 0; y < 8u; y++) {
            const uint8_t* got = out.data + (size_t) y * 15u * 3u;
            const uint8_t* want =
                marked.data + ((size_t) (y + 1u) * 16u + 1u) * 3u;

            failures += assertBytesMatch(got, want, 45);
        }

        tiny_image_destroy(&out);
    }

    tiny_image_destroy(&marked);
    return failures;
}

/**
 * @brief A box reduction by a whole factor is the mean of what it covers.
 *
 * Worked out on a ramp, where every output pixel has an exact answer, so this
 * separates a real area average from a nearest neighbor pick dressed up as
 * one.
 */
static int box_reduction(void) {
    int failures = 0;

    TinyImage ramp;
    if (tiny_image_create(&ramp, 8, 8, 1) != TINYIMG_OK) return 1;

    for (uint32_t y = 0; y < 8u; y++) {
        for (uint32_t x = 0; x < 8u; x++) {
            ramp.data[y * 8u + x] = (uint8_t) (x * 8u + y * 2u);
        }
    }

    TinyPlan plan;
    tiny_plan_init_image(&plan, &ramp);
    tiny_plan_resize_with(&plan, 4, 4, TINYIMG_FILTER_BOX);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 4);
    failures += assertEquals(out.height, 4);

    for (uint32_t y = 0; y < 4u; y++) {
        for (uint32_t x = 0; x < 4u; x++) {
            uint32_t sum = 0;

            for (uint32_t dy = 0; dy < 2u; dy++) {
                for (uint32_t dx = 0; dx < 2u; dx++) {
                    sum += ramp.data[(y * 2u + dy) * 8u + x * 2u + dx];
                }
            }

            failures += assertEquals(out.data[y * 4u + x], (sum + 2u) / 4u);
        }
    }

    tiny_image_destroy(&out);
    tiny_image_destroy(&ramp);
    return failures;
}

/** An enlargement of a flat image is flat, whatever the filter. */
static int flat_stays_flat(void) {
    int failures = 0;

    TinyImage flat;
    if (tiny_image_create(&flat, 4, 4, 3) != TINYIMG_OK) return 1;

    memset(flat.data, 137, 48);

    TinyResampleFilter filters[4] = {
        TINYIMG_FILTER_NEAREST, TINYIMG_FILTER_BILINEAR, TINYIMG_FILTER_BOX,
        TINYIMG_FILTER_CATMULL_ROM
    };

    for (uint32_t i = 0; i < 4u; i++) {
        TinyPlan plan;
        tiny_plan_init_image(&plan, &flat);
        tiny_plan_resize_with(&plan, 37, 21, filters[i]);

        TinyImage out;
        memset(&out, 0, sizeof(out));

        failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);

        // this is what the weight rounding correction is for: without it the
        // weights sum to a shade under one and a flat image drifts a level
        uint32_t wrong = 0;
        for (size_t k = 0; k < (size_t) 37u * 21u * 3u; k++) {
            if (out.data[k] != 137) wrong++;
        }

        failures += assertEquals(wrong, 0);
        tiny_image_destroy(&out);
    }

    tiny_image_destroy(&flat);
    return failures;
}

/**
 * @brief A chain fused, against the same chain one operation at a time.
 *
 * Both run the same sampler over a source neither of them chose differently, so
 * any difference is a fault in the rewrites, the collapse or the region
 * arithmetic. Where the operations cannot round, the two are asserted to agree
 * byte for byte; where they can, the floor is the measured cost of rounding
 * once instead of once per operation. Measured at 48 to 55 dB across the four
 * reducing chains, which is a level on some pixels and nothing on most.
 */
static int chains(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyImage decoded;
    memset(&decoded, 0, sizeof(decoded));

    if (tiny_image_load_scaled(&decoded, bytes, size, 240, 140) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    // geometry moves pixels without changing them, so there is nothing to round
    TinyPlan geometry;
    tiny_plan_init_image(&geometry, &decoded);
    tiny_plan_crop(&geometry, 20, 10, 180, 100);
    tiny_plan_flip_horizontal(&geometry);
    tiny_plan_rotate(&geometry, 90);
    failures += both_ways(&geometry, "crop, flip, turn", 0.0);

    // one resample and one color operation is one of each either way, so the
    // fused pass has no intermediate to save and must land on the same bytes
    TinyPlan single;
    tiny_plan_init_image(&single, &decoded);
    tiny_plan_resize(&single, 100, 60);
    tiny_plan_brightness(&single, 1.2f);
    failures += both_ways(&single, "resize then brightness", 0.0);

    /*
     * An enlargement after a crop is the case that caught the sampler reading
     * outside its window: the filter reaches past the edge of the crop, and
     * what it finds there has to be the crop's own edge and not the pixels the
     * crop removed, which are still sitting in the decode next to it.
     */
    TinyPlan bigger;
    tiny_plan_init_image(&bigger, &decoded);
    tiny_plan_crop(&bigger, 50, 30, 100, 60);
    tiny_plan_resize(&bigger, 130, 90);
    tiny_plan_brightness(&bigger, 1.1f);
    failures += both_ways(&bigger, "crop then enlarge", 0.0);

    // a table either side of the resample stays either side of it, so both
    // paths apply the same two tables to the same samples
    TinyPlan sandwich;
    tiny_plan_init_image(&sandwich, &decoded);
    tiny_plan_gamma(&sandwich, 1.8f);
    tiny_plan_resize(&sandwich, 120, 70);
    tiny_plan_gamma(&sandwich, 0.6f);
    failures += both_ways(&sandwich, "a table either side", 0.0);

    /*
     * Every factor below one, so no intermediate can leave the range and the
     * only thing left between the two paths is that one rounds to eight bits
     * after every operation and the other rounds once. Measured at 53 to 58 dB
     * across these four; the floor is where a change in the collapse would have
     * to show up.
     */
    TinyPlan reducing;
    tiny_plan_init_image(&reducing, &decoded);
    tiny_plan_brightness(&reducing, 0.9f);
    tiny_plan_contrast(&reducing, 0.95f);
    tiny_plan_saturation(&reducing, 0.8f);
    failures += both_ways(&reducing, "color, nothing clamping", 45.0);

    TinyPlan curved;
    tiny_plan_init_image(&curved, &decoded);
    tiny_plan_brightness(&curved, 0.9f);
    tiny_plan_contrast(&curved, 0.95f);
    tiny_plan_saturation(&curved, 0.8f);
    tiny_plan_gamma(&curved, 1.4f);
    failures += both_ways(&curved, "and a table after it", 45.0);

    TinyPlan mixed;
    tiny_plan_init_image(&mixed, &decoded);
    tiny_plan_crop(&mixed, 20, 10, 180, 100);
    tiny_plan_resize(&mixed, 90, 60);
    tiny_plan_brightness(&mixed, 0.9f);
    tiny_plan_contrast(&mixed, 0.95f);
    tiny_plan_saturation(&mixed, 0.8f);
    tiny_plan_gamma(&mixed, 1.4f);
    failures += both_ways(&mixed, "the worked chain, reducing", 45.0);

    TinyPlan first;
    tiny_plan_init_image(&first, &decoded);
    tiny_plan_brightness(&first, 0.9f);
    tiny_plan_saturation(&first, 0.8f);
    tiny_plan_crop(&first, 20, 20, 180, 100);
    tiny_plan_resize(&first, 90, 60);
    failures += both_ways(&first, "color before geometry", 45.0);

    tiny_image_destroy(&decoded);
    free(bytes);
    return failures;
}

static double clamp255(double value) {
    if (value < 0.0) return 0.0;
    if (value > 255.0) return 255.0;
    return value;
}

/**
 * @brief Collapsing color operations is more accurate, not just fewer passes.
 *
 * Where an intermediate would have clamped, the two paths do not merely round
 * differently: they compute different functions. Running a brightness, then a
 * contrast, then a saturation over eight bit images clamps twice on the way,
 * and a channel that clamped has lost the value the saturation needed to find
 * the luminance from.
 *
 * So neither path is allowed to define the answer here. The chain is computed
 * again in double precision with one clamp at the end, which is what a pipeline
 * with no eight bit intermediates would produce, and both are measured against
 * it. The collapse is 45 dB closer, which is the whole argument for it stated
 * as a number rather than as a preference.
 */
static int collapse_is_closer(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyImage decoded;
    memset(&decoded, 0, sizeof(decoded));

    if (tiny_image_load_scaled(&decoded, bytes, size, 240, 140) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    size_t count = (size_t) decoded.width * decoded.height;
    unsigned char* ideal = malloc(count * 3u);

    if (!ideal) {
        tiny_image_destroy(&decoded);
        free(bytes);
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        double channel[3];

        for (uint32_t c = 0; c < 3u; c++) {
            double v = decoded.data[i * 3u + c] * 1.2;
            channel[c] = (v - 127.5) * 1.1 + 127.5;
        }

        double luma = 13933.0 / 65536.0 * channel[0] +
                      46871.0 / 65536.0 * channel[1] +
                      4732.0 / 65536.0 * channel[2];

        for (uint32_t c = 0; c < 3u; c++) {
            ideal[i * 3u + c] =
                (unsigned char) (clamp255(0.8 * channel[c] + 0.2 * luma) + 0.5);
        }
    }

    TinyPlan plan;
    tiny_plan_init_image(&plan, &decoded);
    tiny_plan_brightness(&plan, 1.2f);
    tiny_plan_contrast(&plan, 1.1f);
    tiny_plan_saturation(&plan, 0.8f);

    TinyImage fused;
    TinyImage eager;
    memset(&fused, 0, sizeof(fused));
    memset(&eager, 0, sizeof(eager));

    tiny_plan_set_fusion(&plan, 1);
    failures += assertEquals(tiny_plan_run(&plan, &fused), TINYIMG_OK);
    tiny_plan_set_fusion(&plan, 0);
    failures += assertEquals(tiny_plan_run(&plan, &eager), TINYIMG_OK);

    double fused_db = computePSNR(fused.data, ideal, count * 3u);
    double eager_db = computePSNR(eager.data, ideal, count * 3u);

    printf("collapsed %.2f dB, one at a time %.2f dB\n", fused_db, eager_db);

    failures += assertGreaterThan(fused_db, 70.0);
    failures += assertGreaterThan(fused_db - eager_db, 30.0);

    tiny_image_destroy(&fused);
    tiny_image_destroy(&eager);
    tiny_image_destroy(&decoded);
    free(ideal);
    free(bytes);

    return failures;
}

/**
 * @brief The blur before a downscale, against the blur after it.
 *
 * The rewrite is the claim that these are the same operation, and this is the
 * floor it is asserted against rather than assumed at. The unfused run does
 * what the caller wrote, blurring at full resolution; the fused one blurs the
 * reduced image with a reduced sigma.
 */
static int blur_rewrite(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyImage decoded;
    memset(&decoded, 0, sizeof(decoded));

    if (tiny_image_load_scaled(&decoded, bytes, size, 240, 140) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    TinyPlan plan;
    tiny_plan_init_image(&plan, &decoded);
    tiny_plan_gaussian_blur(&plan, 12.0f);
    tiny_plan_resize(&plan, 60, 34);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);
    failures += assertEquals(res.op[0].kind, TINYIMG_OP_RESIZE);
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);

    failures += both_ways(&plan, "blur before downscale", 33.0);

    // and the same rewrite through a fit, which is the shape a real request has
    TinyPlan fitted;
    tiny_plan_init_image(&fitted, &decoded);
    tiny_plan_gaussian_blur(&fitted, 10.0f);
    tiny_plan_fit(&fitted, 100, 100, TINYIMG_FIT_COVER, TINYIMG_GRAVITY_CENTER);
    failures += both_ways(&fitted, "blur before fit", 32.0);

    tiny_image_destroy(&decoded);
    free(bytes);
    return failures;
}

/** A chain over encoded bytes, where the planner also chooses the decode. */
static int from_bytes(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    if (tiny_plan_init(&plan, bytes, size) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    tiny_plan_crop(&plan, 1000, 400, 500, 500);
    tiny_plan_resize(&plan, 100, 100);
    tiny_plan_brightness(&plan, 1.2f);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 100);
    failures += assertEquals(out.height, 100);
    failures += assertEquals(out.channels, 3);
    failures += assertEquals(out.format, TINYIMG_FORMAT_JPEG);

    /*
     * The fused run decoded a quarter of a five hundred pixel square and the
     * eager one decoded all six megapixels, so the floor here is the cost of
     * the whole optimization: what the region and scale decode gives up against
     * reading everything. Anything much below this would mean the scaled decode
     * is not the area average it claims to be.
     */
    failures += both_ways(&plan, "region and scale", 29.0);

    free(bytes);
    tiny_image_destroy(&out);
    return failures;
}

/** Padding is filled with the background, and a later color op reaches it. */
static int padding(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 40, 20)) return 1;

    uint8_t red[4] = {200, 30, 40, 255};

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_background(&plan, red);
    tiny_plan_fit(&plan, 40, 40, TINYIMG_FIT_PAD, TINYIMG_GRAVITY_CENTER);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 40);
    failures += assertEquals(out.height, 40);

    // the top row is padding and the middle is the image
    failures += assertBytesMatch(out.data, red, 3);
    failures += assertEquals(out.data[(size_t) 20u * 40u * 3u], marked.data[0]);

    tiny_image_destroy(&out);

    // an inversion the caller put after the fit applies to the padding too,
    // because running the two operations separately would have inverted it
    TinyPlan inverted;
    tiny_plan_init_image(&inverted, &marked);
    tiny_plan_background(&inverted, red);
    tiny_plan_fit(&inverted, 40, 40, TINYIMG_FIT_PAD, TINYIMG_GRAVITY_CENTER);
    tiny_plan_invert(&inverted);

    failures += both_ways(&inverted, "pad then invert", 0.0);

    tiny_image_destroy(&marked);
    return failures;
}

/** Resampling an image with transparency does not drag color out of it. */
static int premultiplied(void) {
    int failures = 0;

    TinyImage sprite;
    if (tiny_image_create(&sprite, 8, 8, 4) != TINYIMG_OK) return 1;

    // a transparent left half carrying a color nothing should ever see, and an
    // opaque green right half
    for (uint32_t y = 0; y < 8u; y++) {
        for (uint32_t x = 0; x < 8u; x++) {
            uint8_t* pixel = sprite.data + ((size_t) y * 8u + x) * 4u;

            if (x < 4u) {
                pixel[0] = 255;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0;
            }
            else {
                pixel[0] = 0;
                pixel[1] = 200;
                pixel[2] = 0;
                pixel[3] = 255;
            }
        }
    }

    TinyPlan plan;
    tiny_plan_init_image(&plan, &sprite);
    tiny_plan_resize_with(&plan, 4, 4, TINYIMG_FILTER_BILINEAR);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 4);

    // no output pixel carries any of the hidden red, whatever its alpha
    uint32_t bled = 0;
    for (size_t i = 0; i < 16u; i++) {
        if (out.data[i * 4u] > 8) bled++;
    }

    failures += assertEquals(bled, 0);

    tiny_image_destroy(&out);
    tiny_image_destroy(&sprite);
    return failures;
}

/**
 * @brief Operations after a neighborhood one run over what it produced.
 *
 * The rewrite moves a blur to the small side of a downscale where it can, and
 * where it cannot the plan is genuinely two passes: everything up to the blur,
 * the blur on a materialized image, then everything after it. An enlargement is
 * the case the rewrite refuses, so it is the one that exercises the second
 * pass.
 */
static int after_a_blur(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 64, 48)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_crop(&plan, 8, 8, 32, 24);
    tiny_plan_gaussian_blur(&plan, 2.0f);
    tiny_plan_resize(&plan, 96, 72);
    tiny_plan_brightness(&plan, 1.1f);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);

    // the resize enlarges, so the blur stays where the caller put it and the
    // pass ends at it
    failures += assertEquals(res.op[1].kind, TINYIMG_OP_BLUR);
    failures += assertEquals(res.consumed, 1);
    failures += assertEquals(res.passes, 2);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 96);
    failures += assertEquals(out.height, 72);
    tiny_image_destroy(&out);

    failures += both_ways(&plan, "crop, blur, enlarge, brightness", 45.0);

    // two blurs with an operation between them, so the remainder recurses
    TinyPlan twice;
    tiny_plan_init_image(&twice, &marked);
    tiny_plan_blur(&twice, 2.0f);
    tiny_plan_flip_horizontal(&twice);
    tiny_plan_blur(&twice, 1.0f);

    failures += assertEquals(tiny_plan_resolve(&twice, &res), TINYIMG_OK);
    failures += assertEquals(res.passes, 3);

    memset(&out, 0, sizeof(out));
    failures += assertEquals(tiny_plan_run(&twice, &out), TINYIMG_OK);
    failures += assertEquals(out.width, 64);
    failures += assertEquals(out.height, 48);

    tiny_image_destroy(&out);
    tiny_image_destroy(&marked);
    return failures;
}

/** A grayscale beside another color operation reduces on the way out. */
static int channel_change(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 32, 24)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_grayscale(&plan);
    tiny_plan_brightness(&plan, 1.2f);
    tiny_plan_resize(&plan, 16, 12);

    TinyPlanResolution res;
    failures += assertEquals(tiny_plan_resolve(&plan, &res), TINYIMG_OK);

    // two color operations, so the decoder cannot be asked for the luminance
    // and the reduction to one channel happens as the pass writes
    failures += assertFalse((res.kernels & TINYIMG_KERNEL_GRAY_DECODE) != 0);
    failures += assertEquals(res.channels, 1);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);
    failures += assertEquals(out.channels, 1);
    failures += assertEquals(out.width, 16);

    tiny_image_destroy(&out);
    tiny_image_destroy(&marked);
    return failures;
}

/** The one call that runs a plan and encodes what it produced. */
static int encodes(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyPlan plan;
    if (tiny_plan_init(&plan, bytes, size) != TINYIMG_OK) {
        free(bytes);
        return 1;
    }

    tiny_plan_resize(&plan, 120, 68);
    tiny_plan_grayscale(&plan);

    TinyWriter writer;
    tiny_writer_init(&writer, 0);

    failures += assertEquals(
        tiny_plan_encode(&plan, TINYIMG_FORMAT_PNG, 0, &writer), TINYIMG_OK
    );
    failures += assertTrue(writer.size > 8);
    failures += assertEquals(writer.data[1], 'P');

    // and back in, which is what says the bytes are a real image and not merely
    // a non-empty buffer
    TinyImage decoded;
    memset(&decoded, 0, sizeof(decoded));

    failures += assertEquals(
        tiny_image_load(&decoded, writer.data, writer.size), TINYIMG_OK
    );
    failures += assertEquals(decoded.width, 120);
    failures += assertEquals(decoded.height, 68);
    failures += assertEquals(decoded.channels, 1);

    tiny_image_destroy(&decoded);
    tiny_writer_free(&writer);

    // a format with no encoder in this build reports that rather than writing
    TinyPlan again;
    tiny_plan_init(&again, bytes, size);
    tiny_plan_resize(&again, 8, 8);

    tiny_writer_init(&writer, 0);
    failures += assertEquals(
        tiny_plan_encode(&again, TINYIMG_FORMAT_AVIF, 0, &writer),
        TINYIMG_ERR_UNSUPPORTED_CODEC
    );
    tiny_writer_free(&writer);

    // and a plan that cannot run does not reach the encoder at all
    TinyPlan broken;
    tiny_plan_init(&broken, bytes, size);
    tiny_plan_crop(&broken, 9000, 9000, 10, 10);

    tiny_writer_init(&writer, 0);
    failures += assertEquals(
        tiny_plan_encode(&broken, TINYIMG_FORMAT_PNG, 0, &writer),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals((long) writer.size, 0);
    tiny_writer_free(&writer);

    free(bytes);
    return failures;
}

/** What the unfused path does when an operation in the middle fails. */
static int eager_failure(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 32, 24)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &marked);
    tiny_plan_brightness(&plan, 1.2f);
    tiny_plan_crop(&plan, 900, 900, 10, 10);
    tiny_plan_set_fusion(&plan, 0);

    TinyImage out;
    memset(&out, 0, sizeof(out));

    failures += assertEquals(tiny_plan_run(&plan, &out), TINYIMG_ERR_RANGE);
    failures += assertNull(out.data);

    // the default background is transparent, and passing nothing restores it
    TinyPlan plain;
    tiny_plan_init_image(&plain, &marked);

    uint8_t red[4] = {255, 0, 0, 255};
    failures += assertEquals(tiny_plan_background(&plain, red), TINYIMG_OK);
    failures += assertEquals(plain.background[0], 255);
    failures += assertEquals(tiny_plan_background(&plain, 0), TINYIMG_OK);
    failures += assertEquals(plain.background[0], 0);
    failures += assertEquals(plain.background[3], 0);

    tiny_image_destroy(&marked);
    return failures;
}

int main(void) {
    int failures = 0;

    failures += orientations();
    failures += after_a_blur();
    failures += channel_change();
    failures += encodes();
    failures += eager_failure();
    failures += turn_by_hand();
    failures += crop_is_a_copy();
    failures += identity_resample();
    failures += box_reduction();
    failures += flat_stays_flat();
    failures += chains();
    failures += collapse_is_closer();
    failures += blur_rewrite();
    failures += from_bytes();
    failures += padding();
    failures += premultiplied();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
