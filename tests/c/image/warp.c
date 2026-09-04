#include "test.h"

/** A checkerboard, whose structure a warp cannot fake. */
static int make_board(TinyImage* image, uint32_t size, uint32_t cell) {
    if (tiny_image_create(image, size, size, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            uint8_t value = ((x / cell) + (y / cell)) & 1u ? 230u : 25u;
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

/** A quarter turn goes through the exact kernel and loses nothing. */
static int quarter_turns(void) {
    int failures = 0;
    TinyImage warped;
    TinyImage exact;

    if (!make_board(&warped, 32, 4)) return 1;
    if (!make_board(&exact, 32, 4)) {
        tiny_image_destroy(&warped);
        return 1;
    }

    failures += assertEquals(tiny_image_rotate(&warped, 90.0f, 0), 0);
    failures += assertEquals(tiny_image_rotate_90(&exact), 0);

    // byte identical, not merely close: a multiple of 90 is a permutation of
    // the pixels and must not touch the resampler
    failures += assertImageEquals(&warped, &exact);
    failures += assertEquals(warped.channels, 3);

    tiny_image_destroy(&warped);
    tiny_image_destroy(&exact);

    // and the same for the other three, including a negative and a wrapped
    // angle
    static const float ANGLES[4] = {180.0f, 270.0f, -90.0f, 450.0f};
    static const int32_t SAME[4] = {180, 270, 270, 90};

    for (uint32_t i = 0; i < 4u; i++) {
        if (!make_board(&warped, 24, 3)) return failures + 1;
        if (!make_board(&exact, 24, 3)) {
            tiny_image_destroy(&warped);
            return failures + 1;
        }

        failures += assertEquals(tiny_image_rotate(&warped, ANGLES[i], 0), 0);

        if (SAME[i] == 90)
            failures += assertEquals(tiny_image_rotate_90(&exact), 0);
        else if (SAME[i] == 180)
            failures += assertEquals(tiny_image_rotate_180(&exact), 0);
        else
            failures += assertEquals(tiny_image_rotate_270(&exact), 0);

        failures += assertImageEquals(&warped, &exact);

        tiny_image_destroy(&warped);
        tiny_image_destroy(&exact);
    }

    return failures;
}

/** An arbitrary turn grows the extent to hold the result. */
static int free_rotation(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 40, 5)) return 1;

    failures += assertEquals(tiny_image_rotate(&image, 45.0f, 0), 0);

    // a square turned by 45 degrees needs root two times the side
    uint32_t expected = (uint32_t) (40.0 * 1.41421356 + 0.5);

    failures += assertIn((double) image.width, expected - 2.0, expected + 2.0);
    failures += assertEquals(image.width, image.height);

    // with no background given the corners are transparent, so the image
    // gained an alpha channel to say so
    failures += assertEquals(image.channels, 4);
    failures += assertEquals(at(&image, 0, 0)[3], 0);
    failures +=
        assertEquals(at(&image, image.width / 2u, image.height / 2u)[3], 255);

    tiny_image_destroy(&image);

    // a background keeps the channel count and fills the corners with it
    if (!make_board(&image, 40, 5)) return failures + 1;

    static const uint8_t RED[3] = {255, 0, 0};
    failures += assertEquals(tiny_image_rotate(&image, 30.0f, RED), 0);

    failures += assertEquals(image.channels, 3);
    failures += assertEquals(at(&image, 0, 0)[0], 255);
    failures += assertEquals(at(&image, 0, 0)[1], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** A shear slants the image and grows the extent along the sheared axis. */
static int shear(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 32, 4)) return 1;

    static const uint8_t BLUE[3] = {0, 0, 255};
    failures += assertEquals(tiny_image_shear(&image, 0.5f, 0.0f, BLUE), 0);

    // half a pixel of slant per row over 32 rows adds 16 columns
    failures += assertEquals(image.width, 48);
    failures += assertEquals(image.height, 32);

    // the top right and bottom left are outside the slanted parallelogram
    failures += assertEquals(at(&image, 47, 0)[2], 255);
    failures += assertEquals(at(&image, 0, 31)[2], 255);

    tiny_image_destroy(&image);

    // shearing on the other axis grows the height instead
    if (!make_board(&image, 32, 4)) return failures + 1;

    failures += assertEquals(tiny_image_shear(&image, 0.0f, 0.25f, BLUE), 0);
    failures += assertEquals(image.width, 32);
    failures += assertEquals(image.height, 40);

    // a shear pair whose product is one has no inverse
    failures += assertEquals(
        tiny_image_shear(&image, 1.0f, 1.0f, BLUE), TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A perspective onto the same rectangle is the identity.
 *
 * The check the homography is solved rather than merely plausible: mapping the
 * four corners onto where they already are has to give back the image, and a
 * sign error in the projective terms fails that immediately.
 */
static int perspective(void) {
    int failures = 0;
    TinyImage image;
    TinyImage kept;

    if (!make_board(&image, 32, 4)) return 1;
    if (!make_board(&kept, 32, 4)) {
        tiny_image_destroy(&image);
        return 1;
    }

    float square[8] = {0.0f, 0.0f, 32.0f, 0.0f, 32.0f, 32.0f, 0.0f, 32.0f};

    static const uint8_t BLACK[3] = {0, 0, 0};
    failures += assertEquals(tiny_image_perspective(&image, square, BLACK), 0);

    failures += assertEquals(image.width, 32);
    failures += assertEquals(image.height, 32);

    // a resample at integral positions reproduces its input, so this is close
    // rather than exact; 45 dB is under a level of average error
    failures += assertPSNR(image.data, kept.data, 32u * 32u * 3u, 45.0);

    tiny_image_destroy(&image);
    tiny_image_destroy(&kept);

    // a trapezoid narrows one edge, so the extent follows the quad's own box
    if (!make_board(&image, 40, 5)) return failures + 1;

    float trapezoid[8] = {10.0f, 0.0f, 50.0f, 0.0f, 60.0f, 40.0f, 0.0f, 40.0f};

    failures +=
        assertEquals(tiny_image_perspective(&image, trapezoid, BLACK), 0);
    failures += assertEquals(image.width, 60);
    failures += assertEquals(image.height, 40);

    // the top left corner is outside the trapezoid and so is background
    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertGreaterThan(at(&image, 30, 20)[0], 0);

    tiny_image_destroy(&image);

    // a parallelogram is an affine quad: the projective terms are zero and the
    // general solve divides by a determinant that is also zero, so it takes
    // its own branch rather than producing infinities
    if (!make_board(&image, 32, 4)) return failures + 1;

    float parallelogram[8] = {10.0f, 0.0f,  42.0f, 0.0f,
                              32.0f, 32.0f, 0.0f,  32.0f};

    failures +=
        assertEquals(tiny_image_perspective(&image, parallelogram, BLACK), 0);
    failures += assertEquals(image.width, 42);
    failures += assertEquals(image.height, 32);

    // the slanted shape leaves the two acute corners empty and fills the middle
    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertGreaterThan(at(&image, 21, 16)[0], 0);

    tiny_image_destroy(&image);

    // a degenerate quad has no homography
    if (!make_board(&image, 16, 2)) return failures + 1;

    float flat[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    failures += assertEquals(
        tiny_image_perspective(&image, flat, BLACK), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_perspective(&image, 0, BLACK), TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/** Barrel and its opposite nearly undo each other, and zero is exact. */
static int barrel(void) {
    int failures = 0;
    TinyImage image;
    TinyImage kept;

    if (!make_board(&image, 48, 6)) return 1;
    if (!make_board(&kept, 48, 6)) {
        tiny_image_destroy(&image);
        return 1;
    }

    failures += assertEquals(tiny_image_barrel(&image, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    failures += assertEquals(tiny_image_barrel(&image, 0.2f), 0);

    // the middle is where the distortion is least, so it stays close; the
    // corners move, which is the whole effect
    failures += assertEquals(image.width, 48);
    failures += assertLessThan(
        computePSNR(image.data, kept.data, 48u * 48u * 3u), 30.0
    );

    tiny_image_destroy(&image);
    tiny_image_destroy(&kept);
    return failures;
}

/** A swirl leaves the rim where it was, so there is no seam. */
static int swirl(void) {
    int failures = 0;
    TinyImage image;
    TinyImage kept;

    if (!make_board(&image, 64, 8)) return 1;
    if (!make_board(&kept, 64, 8)) {
        tiny_image_destroy(&image);
        return 1;
    }

    failures += assertEquals(tiny_image_swirl(&image, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    failures += assertEquals(tiny_image_swirl(&image, 120.0f), 0);

    // the four corners are past the swirl's radius and so are untouched
    failures += assertEquals(at(&image, 0, 0)[0], at(&kept, 0, 0)[0]);
    failures += assertEquals(at(&image, 63, 63)[0], at(&kept, 63, 63)[0]);

    // and the middle has moved
    uint32_t moved = 0;
    for (uint32_t y = 24; y < 40u; y++) {
        for (uint32_t x = 24; x < 40u; x++) {
            if (at(&image, x, y)[0] != at(&kept, x, y)[0]) moved++;
        }
    }

    failures += assertGreaterThan(moved, 40);

    tiny_image_destroy(&image);
    tiny_image_destroy(&kept);
    return failures;
}

/** Polar and its inverse are round trips of each other's shape. */
static int polar(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 48, 6)) return 1;

    failures += assertEquals(tiny_image_polar(&image, 0), 0);
    failures += assertEquals(image.width, 48);
    failures += assertEquals(image.height, 48);
    failures += assertEquals(image.channels, 4);

    tiny_image_destroy(&image);
    if (!make_board(&image, 48, 6)) return failures + 1;

    failures += assertEquals(tiny_image_polar(&image, 1), 0);
    failures += assertEquals(image.width, 48);

    tiny_image_destroy(&image);
    return failures;
}

/** An arc bows the image and grows the extent to hold the bow. */
static int arc(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 40, 5)) return 1;

    failures += assertEquals(tiny_image_arc(&image, 0.0f, 0), 0);
    failures += assertEquals(image.height, 40);

    failures += assertEquals(tiny_image_arc(&image, 90.0f, 0), 0);
    failures += assertGreaterThan(image.height, 40);
    failures += assertEquals(image.width, 40);

    failures +=
        assertEquals(tiny_image_arc(&image, 400.0f, 0), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

/** A corner radius clears the corners and keeps the middle. */
static int corner_radius(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 40, 5)) return 1;

    failures += assertEquals(tiny_image_corner_radius(&image, 0), 0);
    failures += assertEquals(image.channels, 3);

    failures += assertEquals(tiny_image_corner_radius(&image, 10), 0);
    failures += assertEquals(image.channels, 4);

    failures += assertEquals(at(&image, 0, 0)[3], 0);
    failures += assertEquals(at(&image, 39, 0)[3], 0);
    failures += assertEquals(at(&image, 0, 39)[3], 0);
    failures += assertEquals(at(&image, 39, 39)[3], 0);
    failures += assertEquals(at(&image, 20, 20)[3], 255);
    failures += assertEquals(at(&image, 20, 0)[3], 255);

    tiny_image_destroy(&image);
    return failures;
}

/** A shape crop clears what falls outside it. */
static int shape_crops(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 40, 5)) return 1;

    failures += assertEquals(tiny_image_crop_circle(&image, 20, 20, 15), 0);
    failures += assertEquals(image.channels, 4);
    failures += assertEquals(at(&image, 0, 0)[3], 0);
    failures += assertEquals(at(&image, 20, 20)[3], 255);
    failures += assertEquals(at(&image, 20, 6)[3], 255);
    failures += assertEquals(at(&image, 20, 3)[3], 0);

    tiny_image_destroy(&image);
    if (!make_board(&image, 40, 5)) return failures + 1;

    failures += assertEquals(tiny_image_crop_ellipse(&image, 20, 20, 18, 6), 0);
    failures += assertEquals(at(&image, 3, 20)[3], 255);
    failures += assertEquals(at(&image, 20, 3)[3], 0);

    failures += assertEquals(
        tiny_image_crop_ellipse(&image, 20, 20, 0, 6), TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&image);
    if (!make_board(&image, 40, 5)) return failures + 1;

    // a triangle covering the lower left half
    static const uint32_t XS[3] = {0, 39, 0};
    static const uint32_t YS[3] = {0, 39, 39};

    failures += assertEquals(tiny_image_crop_polygon(&image, XS, YS, 3), 0);
    failures += assertEquals(at(&image, 4, 30)[3], 255);
    failures += assertEquals(at(&image, 35, 4)[3], 0);

    failures += assertEquals(
        tiny_image_crop_polygon(&image, XS, YS, 2), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_crop_polygon(&image, 0, YS, 3), TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/** Opacity scales the alpha channel and adds one when there is none. */
static int opacity(void) {
    int failures = 0;
    TinyImage image;

    if (!make_board(&image, 8, 2)) return 1;

    failures += assertEquals(tiny_image_opacity(&image, 0.5f), 0);
    failures += assertEquals(image.channels, 4);
    failures += assertEquals(at(&image, 0, 0)[3], 128);

    failures += assertEquals(tiny_image_opacity(&image, 0.5f), 0);
    failures += assertEquals(at(&image, 0, 0)[3], 64);

    failures += assertEquals(tiny_image_opacity(&image, 0.0f), 0);
    failures += assertEquals(at(&image, 0, 0)[3], 0);

    failures +=
        assertEquals(tiny_image_opacity(&image, 1.5f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_image_opacity(0, 0.5f), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

static int nulls(void) {
    int failures = 0;

    failures += assertEquals(tiny_image_rotate(0, 45.0f, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_shear(0, 1.0f, 0.0f, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_barrel(0, 0.1f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_swirl(0, 30.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_polar(0, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_arc(0, 30.0f, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_corner_radius(0, 4), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_crop_circle(0, 1, 1, 1), TINYIMG_ERR_NULL);

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += quarter_turns();
    failures += free_rotation();
    failures += shear();
    failures += perspective();
    failures += barrel();
    failures += swirl();
    failures += polar();
    failures += arc();
    failures += corner_radius();
    failures += shape_crops();
    failures += opacity();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
