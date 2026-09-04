#include "tinyimg/plan.h"

#include "test.h"

/** A single bright pixel in the middle of a dark field. */
static int make_impulse(TinyImage* image, uint32_t size) {
    if (tiny_image_create(image, size, size, 3) != TINYIMG_OK) return 0;

    uint8_t* center =
        image->data + ((size_t) (size / 2u) * size + size / 2u) * 3u;

    center[0] = 255u;
    center[1] = 255u;
    center[2] = 255u;

    return 1;
}

/** Half dark and half light, split down the middle. */
static int make_edge(TinyImage* image, uint32_t size) {
    if (tiny_image_create(image, size, size, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            uint8_t value = x < size / 2u ? 40u : 200u;
            uint8_t* p = image->data + ((size_t) y * size + x) * 3u;

            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
    }

    return 1;
}

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** The total light in an image, which several kernels have to preserve. */
static uint64_t energy(const TinyImage* image) {
    uint64_t total = 0;

    for (size_t i = 0; i < (size_t) image->width * image->height; i++) {
        total += image->data[i * image->channels];
    }

    return total;
}

/** A spatial effect ends the fused pass rather than folding into it. */
static int ends_the_pass(void) {
    int failures = 0;
    TinyImage image;

    if (!make_edge(&image, 32)) return 1;

    TinyPlan plan;
    tiny_plan_init_image(&plan, &image);

    float sharpen[4] = {1.0f, 0.5f, 0.0f, 0.0f};

    failures += assertEquals(tiny_plan_brightness(&plan, 1.1f), 0);
    failures +=
        assertEquals(tiny_plan_effect(&plan, TINYIMG_FX_UNSHARP, sharpen), 0);
    failures += assertEquals(tiny_plan_contrast(&plan, 1.1f), 0);

    TinyPlanResolution resolution;
    failures += assertEquals(tiny_plan_resolve(&plan, &resolution), 0);

    // the brightness fuses into the first pass, the effect materializes, and
    // the contrast runs after it: two passes, not one and not three
    failures += assertEquals(resolution.passes, 2);
    failures += assertEquals(resolution.consumed, 1);
    failures +=
        assertTrue((resolution.kernels & TINYIMG_KERNEL_NEIGHBORHOOD) != 0u);
    failures += assertEquals(
        tiny_plan_op_class(TINYIMG_OP_EFFECT), TINYIMG_OP_CLASS_NEIGHBORHOOD
    );

    tiny_image_destroy(&image);
    return failures;
}

/** A sharpen raises the step at an edge and leaves a flat field alone. */
static int sharpening(void) {
    int failures = 0;
    TinyImage image;

    if (!make_edge(&image, 32)) return 1;

    int32_t before =
        (int32_t) at(&image, 16, 16)[0] - (int32_t) at(&image, 15, 16)[0];

    failures +=
        assertEquals(tiny_image_unsharp_mask(&image, 1.5f, 1.0f, 0.0f), 0);

    int32_t after =
        (int32_t) at(&image, 16, 16)[0] - (int32_t) at(&image, 15, 16)[0];

    failures += assertGreaterThan(after, before);

    tiny_image_destroy(&image);

    // a threshold above the difference present leaves the image untouched,
    // which is the whole point of the parameter
    if (!make_edge(&image, 32)) return failures + 1;

    TinyImage kept;
    tiny_image_create(&kept, 32, 32, 3);
    memcpy(kept.data, image.data, 32u * 32u * 3u);

    failures +=
        assertEquals(tiny_image_unsharp_mask(&image, 1.5f, 1.0f, 255.0f), 0);
    failures += assertImageEquals(&image, &kept);

    // and an amount of zero is the identity whatever the sigma
    failures +=
        assertEquals(tiny_image_unsharp_mask(&image, 8.0f, 0.0f, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

/** An edge detector finds the edge and nothing else. */
static int edges(void) {
    int failures = 0;
    TinyImage image;

    if (!make_edge(&image, 32)) return 1;
    failures += assertEquals(tiny_image_sobel(&image), 0);

    // bright along the boundary columns, dark everywhere else
    failures += assertGreaterThan(at(&image, 15, 16)[0], 150);
    failures += assertGreaterThan(at(&image, 16, 16)[0], 150);
    failures += assertLessThan(at(&image, 4, 16)[0], 10);
    failures += assertLessThan(at(&image, 28, 16)[0], 10);

    tiny_image_destroy(&image);

    // an emboss on a flat field is the offset it was given, because the kernel
    // sums to zero
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 64u * 3u; i++) image.data[i] = 90u;

    failures += assertEquals(tiny_image_emboss(&image, 128.0f), 0);
    failures += assertEquals(at(&image, 4, 4)[0], 128);

    tiny_image_destroy(&image);
    return failures;
}

/** A pixelate flattens each block and a region one leaves the rest alone. */
static int blocks(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return 1;

    for (uint32_t i = 0; i < 256u; i++) {
        image.data[i * 3u] = (uint8_t) i;
        image.data[i * 3u + 1u] = (uint8_t) i;
        image.data[i * 3u + 2u] = (uint8_t) i;
    }

    failures += assertEquals(tiny_image_pixelate(&image, 4), 0);

    // every pixel of a block is the block's mean, so the four in one corner
    // agree
    uint8_t corner = at(&image, 0, 0)[0];

    for (uint32_t y = 0; y < 4u; y++) {
        for (uint32_t x = 0; x < 4u; x++) {
            failures += assertEquals(at(&image, x, y)[0], corner);
        }
    }

    failures += assertNotEquals(at(&image, 4, 0)[0], corner);

    // a block size below two changes nothing rather than dividing by zero
    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 192u; i++) image.data[i] = (uint8_t) (i * 3u);

    TinyImage kept;
    tiny_image_create(&kept, 8, 8, 3);
    memcpy(kept.data, image.data, 192u);

    failures += assertEquals(tiny_image_pixelate(&image, 1), 0);
    failures += assertImageEquals(&image, &kept);

    // a region pixelate touches only its rectangle
    failures +=
        assertEquals(tiny_image_pixelate_region(&image, 0, 0, 4, 4, 4), 0);
    failures += assertNotEquals(at(&image, 1, 1)[0], at(&kept, 1, 1)[0]);
    failures += assertEquals(at(&image, 6, 6)[0], at(&kept, 6, 6)[0]);

    // a rectangle outside the image is a request with nothing in it
    failures +=
        assertEquals(tiny_image_pixelate_region(&image, 99, 99, 4, 4, 4), 0);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

/** A median removes an impulse; a mean would only spread it. */
static int median(void) {
    int failures = 0;
    TinyImage image;

    if (!make_impulse(&image, 9)) return 1;

    failures += assertEquals(tiny_image_despeckle(&image), 0);

    // the single bright pixel is gone entirely, and so is any trace of it in
    // its neighbors. a blur would leave every neighbor lifted
    for (uint32_t y = 0; y < 9u; y++) {
        for (uint32_t x = 0; x < 9u; x++) {
            failures += assertEquals(at(&image, x, y)[0], 0);
        }
    }

    tiny_image_destroy(&image);
    return failures;
}

/** Dilation grows the bright region and erosion shrinks it, and they undo. */
static int morphology(void) {
    int failures = 0;
    TinyImage image;

    if (!make_impulse(&image, 21)) return 1;

    failures += assertEquals(tiny_image_dilate(&image, 2), 0);

    // a radius of two over a square structuring element is a five by five
    // block, which is what the separable pass has to produce
    uint32_t bright = 0;
    for (uint32_t i = 0; i < 21u * 21u; i++) {
        if (image.data[i * 3u] == 255u) bright++;
    }

    failures += assertEquals(bright, 25);

    // eroding it back gives the single pixel again, so the two are inverses on
    // a shape larger than the radius
    failures += assertEquals(tiny_image_erode(&image, 2), 0);

    bright = 0;
    for (uint32_t i = 0; i < 21u * 21u; i++) {
        if (image.data[i * 3u] == 255u) bright++;
    }

    failures += assertEquals(bright, 1);

    tiny_image_destroy(&image);

    // an open removes a speck smaller than the radius, and a close does not
    if (!make_impulse(&image, 21)) return failures + 1;

    failures += assertEquals(tiny_image_morphology_open(&image, 2), 0);
    failures += assertEquals(energy(&image), 0);

    tiny_image_destroy(&image);
    if (!make_impulse(&image, 21)) return failures + 1;

    failures += assertEquals(tiny_image_morphology_close(&image, 2), 0);
    failures += assertGreaterThan(energy(&image), 0);

    tiny_image_destroy(&image);

    // an outline of a solid block is its boundary, so the middle goes dark
    if (tiny_image_create(&image, 21, 21, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t y = 6; y < 15u; y++) {
        for (uint32_t x = 6; x < 15u; x++) {
            uint8_t* p = image.data + ((size_t) y * 21u + x) * 3u;
            p[0] = 255u;
            p[1] = 255u;
            p[2] = 255u;
        }
    }

    failures += assertEquals(tiny_image_outline(&image, 1), 0);
    failures += assertEquals(at(&image, 10, 10)[0], 0);
    failures += assertGreaterThan(at(&image, 6, 10)[0], 200);

    // a radius of zero is the identity for all four
    tiny_image_destroy(&image);
    if (!make_impulse(&image, 9)) return failures + 1;

    TinyImage kept;
    tiny_image_create(&kept, 9, 9, 3);
    memcpy(kept.data, image.data, 9u * 9u * 3u);

    failures += assertEquals(tiny_image_dilate(&image, 0), 0);
    failures += assertEquals(tiny_image_erode(&image, 0), 0);
    failures += assertEquals(tiny_image_outline(&image, 0), 0);
    failures += assertImageEquals(&image, &kept);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A directional blur spreads an impulse along its own direction.
 *
 * The check that the accumulation is centered: a zoom blur's center pixel has
 * no displacement at all, so an implementation that steps from zero rather
 * than from either side of unity counts it repeatedly and leaves a hot spot.
 */
static int directional(void) {
    int failures = 0;
    TinyImage image;

    if (!make_impulse(&image, 41)) return 1;

    failures += assertEquals(tiny_image_motion_blur(&image, 10.0f, 0.0f), 0);

    // horizontal at zero degrees, so the row through the center carries the
    // light and the column does not
    uint32_t row = 0;
    uint32_t column = 0;

    for (uint32_t i = 0; i < 41u; i++) {
        if (at(&image, i, 20)[0] > 0u) row++;
        if (at(&image, 20, i)[0] > 0u) column++;
    }

    failures += assertGreaterThan(row, 5);
    failures += assertEquals(column, 1);

    tiny_image_destroy(&image);
    if (!make_impulse(&image, 41)) return failures + 1;

    failures += assertEquals(tiny_image_motion_blur(&image, 10.0f, 90.0f), 0);

    row = 0;
    column = 0;

    for (uint32_t i = 0; i < 41u; i++) {
        if (at(&image, i, 20)[0] > 0u) row++;
        if (at(&image, 20, i)[0] > 0u) column++;
    }

    failures += assertEquals(row, 1);
    failures += assertGreaterThan(column, 5);

    tiny_image_destroy(&image);

    // a zoom blur on a flat field is the identity, which is what catches an
    // uncentered accumulation: any drift shows as a gradient from the middle
    if (tiny_image_create(&image, 33, 33, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 33u * 33u * 3u; i++) image.data[i] = 120u;

    failures += assertEquals(tiny_image_zoom_blur(&image, 20.0f), 0);

    int32_t worst = 0;
    for (uint32_t i = 0; i < 33u * 33u; i++) {
        int32_t diff = (int32_t) image.data[i * 3u] - 120;
        if (diff < 0) diff = -diff;
        if (diff > worst) worst = diff;
    }

    failures += assertLessThan(worst, 2);

    // and so is a radial blur, for the same reason
    failures += assertEquals(tiny_image_radial_blur(&image, 30.0f), 0);

    worst = 0;
    for (uint32_t i = 0; i < 33u * 33u; i++) {
        int32_t diff = (int32_t) image.data[i * 3u] - 120;
        if (diff < 0) diff = -diff;
        if (diff > worst) worst = diff;
    }

    failures += assertLessThan(worst, 3);

    // a strength of zero is the identity for all three
    TinyImage kept;
    tiny_image_create(&kept, 33, 33, 3);
    memcpy(kept.data, image.data, 33u * 33u * 3u);

    failures += assertEquals(tiny_image_motion_blur(&image, 0.0f, 45.0f), 0);
    failures += assertEquals(tiny_image_radial_blur(&image, 0.0f), 0);
    failures += assertEquals(tiny_image_zoom_blur(&image, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

/** A region blur reads across its own boundary, so the edge does not darken. */
static int region_blur(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 40, 40, 3) != TINYIMG_OK) return 1;
    for (uint32_t i = 0; i < 40u * 40u * 3u; i++) image.data[i] = 180u;

    failures +=
        assertEquals(tiny_image_blur_region(&image, 10, 10, 20, 20, 3.0f), 0);

    // a flat field blurred inside a rectangle is unchanged everywhere,
    // including at the rectangle's edge. blurring only the rectangle's own
    // pixels leaves nothing outside to average against and darkens the border
    int32_t worst = 0;
    for (uint32_t i = 0; i < 40u * 40u; i++) {
        int32_t diff = (int32_t) image.data[i * 3u] - 180;
        if (diff < 0) diff = -diff;
        if (diff > worst) worst = diff;
    }

    failures += assertLessThan(worst, 2);

    // and it really does blur inside: an impulse in the rectangle spreads
    tiny_image_destroy(&image);
    if (!make_impulse(&image, 40)) return failures + 1;

    failures +=
        assertEquals(tiny_image_blur_region(&image, 10, 10, 20, 20, 2.0f), 0);
    failures += assertGreaterThan(at(&image, 21, 20)[0], 0);

    // outside its rectangle the impulse would still be sharp
    tiny_image_destroy(&image);
    if (!make_impulse(&image, 40)) return failures + 1;

    failures +=
        assertEquals(tiny_image_blur_region(&image, 0, 0, 5, 5, 2.0f), 0);
    failures += assertEquals(at(&image, 20, 20)[0], 255);
    failures += assertEquals(at(&image, 21, 20)[0], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** Tilt shift keeps the band sharp and blurs away from it. */
static int tilt_shift(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 8, 64, 3) != TINYIMG_OK) return 1;

    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 8u; x++) {
            uint8_t value = (y & 1u) ? 220u : 30u;
            uint8_t* p = image.data + ((size_t) y * 8u + x) * 3u;

            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
    }

    failures += assertEquals(tiny_image_tilt_shift(&image, 4.0f, 0.25f), 0);

    // the stripe contrast survives in the middle band and is gone at the top
    int32_t middle =
        (int32_t) at(&image, 4, 32)[0] - (int32_t) at(&image, 4, 33)[0];
    int32_t edge =
        (int32_t) at(&image, 4, 2)[0] - (int32_t) at(&image, 4, 3)[0];

    if (middle < 0) middle = -middle;
    if (edge < 0) edge = -edge;

    failures += assertGreaterThan(middle, 100);
    failures += assertLessThan(edge, 40);

    tiny_image_destroy(&image);
    return failures;
}

/** A glow and a drop shadow grow the image or lighten it as promised. */
static int glows(void) {
    int failures = 0;
    TinyImage image;

    if (!make_impulse(&image, 32)) return 1;

    uint64_t before = energy(&image);
    failures += assertEquals(tiny_image_glow(&image, 3.0f, 1.0f), 0);

    // the glow only ever adds, and it spreads
    failures += assertGreaterThan(energy(&image), before);
    failures += assertEquals(at(&image, 16, 16)[0], 255);
    failures += assertGreaterThan(at(&image, 18, 16)[0], 0);

    tiny_image_destroy(&image);

    // a drop shadow grows the extent by enough to hold the offset and the blur
    if (tiny_image_create(&image, 20, 20, 4) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 400u; i++) {
        image.data[i * 4u] = 255u;
        image.data[i * 4u + 3u] = 255u;
    }

    failures += assertEquals(tiny_image_drop_shadow(&image, 4, 4, 2.0f, 0), 0);

    failures += assertGreaterThan(image.width, 20);
    failures += assertGreaterThan(image.height, 20);
    failures += assertEquals(image.channels, 4);

    // the shadow falls down and to the right, so a point just past the
    // content's bottom right has gained alpha and its mirror past the top left
    // has not. the outermost corner is past the blur's reach and is opaque in
    // neither direction, so it cannot tell the two apart
    uint32_t inset = 4u;
    const uint8_t* below =
        at(&image, image.width - 1u - inset, image.height - 1u - inset);
    const uint8_t* above = at(&image, inset, inset);

    failures += assertGreaterThan(below[3], 0);
    failures += assertEquals(above[3], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** Noise is the same every run, and grain lands in the midtones. */
static int noise(void) {
    int failures = 0;
    TinyImage first;
    TinyImage second;

    if (tiny_image_create(&first, 32, 32, 3) != TINYIMG_OK) return 1;
    if (tiny_image_create(&second, 32, 32, 3) != TINYIMG_OK) {
        tiny_image_destroy(&first);
        return 1;
    }

    for (uint32_t i = 0; i < 32u * 32u * 3u; i++) {
        first.data[i] = 128u;
        second.data[i] = 128u;
    }

    failures += assertEquals(tiny_image_noise(&first, 20.0f, 0), 0);
    failures += assertEquals(tiny_image_noise(&second, 20.0f, 0), 0);

    // the same request over the same image twice gives the same noise, because
    // the generator is a hash of the position rather than a running state
    failures += assertImageEquals(&first, &second);

    // and it actually varies
    uint32_t distinct = 0;
    for (uint32_t i = 1; i < 32u * 32u; i++) {
        if (first.data[i * 3u] != first.data[(i - 1u) * 3u]) distinct++;
    }

    failures += assertGreaterThan(distinct, 500);

    // monochrome noise moves all three channels together
    tiny_image_destroy(&first);
    if (tiny_image_create(&first, 8, 8, 3) != TINYIMG_OK) {
        tiny_image_destroy(&second);
        return failures + 1;
    }

    for (uint32_t i = 0; i < 192u; i++) first.data[i] = 128u;

    failures += assertEquals(tiny_image_noise(&first, 20.0f, 1), 0);

    for (uint32_t i = 0; i < 64u; i++) {
        failures += assertEquals(first.data[i * 3u], first.data[i * 3u + 1u]);
    }

    // film grain leaves black and white alone and moves mid gray
    tiny_image_destroy(&first);
    if (tiny_image_create(&first, 3, 1, 3) != TINYIMG_OK) {
        tiny_image_destroy(&second);
        return failures + 1;
    }

    first.data[0] = 0u;
    first.data[1] = 0u;
    first.data[2] = 0u;
    first.data[3] = 128u;
    first.data[4] = 128u;
    first.data[5] = 128u;
    first.data[6] = 255u;
    first.data[7] = 255u;
    first.data[8] = 255u;

    failures += assertEquals(tiny_image_film_grain(&first, 30.0f), 0);

    failures += assertEquals(first.data[0], 0);
    failures += assertEquals(first.data[6], 255);

    tiny_image_destroy(&first);
    tiny_image_destroy(&second);
    return failures;
}

/** Dither, halftone and scanlines each reduce or mark as promised. */
static int patterns(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 64, 64, 3) != TINYIMG_OK) return 1;

    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 64u; x++) {
            uint8_t* p = image.data + ((size_t) y * 64u + x) * 3u;

            p[0] = (uint8_t) (x * 4u);
            p[1] = (uint8_t) (x * 4u);
            p[2] = (uint8_t) (x * 4u);
        }
    }

    failures += assertEquals(tiny_image_dither(&image, 2), 0);

    // two levels means only two values survive, but the pattern still carries
    // the gradient: the count of set pixels rises across the image
    for (uint32_t i = 0; i < 64u * 64u; i++) {
        uint8_t value = image.data[i * 3u];
        failures += assertTrue(value == 0u || value == 255u);
    }

    uint32_t left = 0;
    uint32_t right = 0;

    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 8u; x++) {
            if (at(&image, x, y)[0] != 0u) left++;
            if (at(&image, 56u + x, y)[0] != 0u) right++;
        }
    }

    failures += assertGreaterThan(right, left);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 32, 32, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 32u * 32u * 3u; i++) image.data[i] = 128u;

    failures += assertEquals(tiny_image_halftone(&image, 8), 0);

    // a mid gray becomes a dot covering about half the cell
    uint32_t dark = 0;
    for (uint32_t i = 0; i < 32u * 32u; i++) {
        if (image.data[i * 3u] == 0u) dark++;
    }

    failures += assertIn((double) dark, 300.0, 700.0);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 192u; i++) image.data[i] = 200u;

    failures += assertEquals(tiny_image_scanlines(&image, 2, 0.5f), 0);

    failures += assertEquals(at(&image, 0, 0)[0], 100);
    failures += assertEquals(at(&image, 0, 1)[0], 200);
    failures += assertEquals(at(&image, 0, 2)[0], 100);

    tiny_image_destroy(&image);
    return failures;
}

/** Chromatic aberration moves red and blue and leaves green. */
static int chromatic(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 32, 32, 3) != TINYIMG_OK) return 1;

    for (uint32_t y = 0; y < 32u; y++) {
        for (uint32_t x = 0; x < 32u; x++) {
            uint8_t* p = image.data + ((size_t) y * 32u + x) * 3u;
            uint8_t value = x < 16u ? 20u : 220u;

            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
    }

    TinyImage kept;
    tiny_image_create(&kept, 32, 32, 3);
    memcpy(kept.data, image.data, 32u * 32u * 3u);

    failures += assertEquals(tiny_image_chromatic_aberration(&image, 6.0f), 0);

    // green is untouched everywhere, and red and blue have moved apart at the
    // edge where the image has a gradient to move
    for (uint32_t i = 0; i < 32u * 32u; i++) {
        failures +=
            assertEquals(image.data[i * 3u + 1u], kept.data[i * 3u + 1u]);
    }

    int32_t split =
        (int32_t) at(&image, 15, 4)[0] - (int32_t) at(&image, 15, 4)[2];

    failures += assertNotEquals(split, 0);

    // a one channel image has nothing to split, so it is a no-op
    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);

    if (tiny_image_create(&image, 8, 8, 1) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 64u; i++) image.data[i] = (uint8_t) (i * 3u);

    tiny_image_create(&kept, 8, 8, 1);
    memcpy(kept.data, image.data, 64u);

    failures += assertEquals(tiny_image_chromatic_aberration(&image, 4.0f), 0);
    failures += assertImageEquals(&image, &kept);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);
    return failures;
}

static int nulls(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return 1;

    failures += assertEquals(tiny_image_sobel(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_despeckle(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_glow(0, 1.0f, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_noise(0, 1.0f, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_image_drop_shadow(0, 1, 1, 1.0f, 0), TINYIMG_ERR_NULL
    );

    failures += assertEquals(tiny_image_dither(&image, 1), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_image_halftone(&image, 1), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_scanlines(&image, 1, 0.5f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_scanlines(&image, 4, 2.0f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_clarity(&image, -1.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_image_motion_blur(&image, -1.0f, 0.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_tilt_shift(&image, 1.0f, 2.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_blur_region(&image, 0, 0, 4, 4, -1.0f), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_sharpen(&image, -1.0f), TINYIMG_ERR_RANGE);

    // clarity picks its own radius from the extent, so a tiny image still works
    failures += assertEquals(tiny_image_clarity(&image, 0.5f), 0);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += ends_the_pass();
    failures += sharpening();
    failures += edges();
    failures += blocks();
    failures += median();
    failures += morphology();
    failures += directional();
    failures += region_blur();
    failures += tilt_shift();
    failures += glows();
    failures += noise();
    failures += patterns();
    failures += chromatic();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
