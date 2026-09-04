#include "test.h"

static const uint8_t RED[4] = {255, 0, 0, 255};
static const uint8_t GREEN[4] = {0, 255, 0, 255};
static const uint8_t CLEAR_RED[4] = {255, 0, 0, 100};

static int make_canvas(TinyImage* image, uint32_t width, uint32_t height) {
    if (tiny_image_create(image, width, height, 4) != TINYIMG_OK) return 0;

    for (uint32_t i = 0; i < width * height; i++) {
        image->data[i * 4u + 3u] = 255u;
    }

    return 1;
}

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * 4u;
}

/** A shape drawn through the list lands where the same shape drawn does. */
static int matches_the_primitive(void) {
    int failures = 0;
    TinyImage direct;
    TinyImage listed;
    TinyDisplayList list;

    if (!make_canvas(&direct, 40, 40)) return 1;
    if (!make_canvas(&listed, 40, 40)) {
        tiny_image_destroy(&direct);
        return 1;
    }

    tiny_image_fill_rectangle(&direct, 5, 7, 12, 9, RED);

    tiny_display_init(&list);
    failures +=
        assertEquals(tiny_display_rect(&list, 5.0f, 7.0f, 12.0f, 9.0f, RED), 0);
    failures += assertEquals(tiny_display_render(&list, &listed), 0);
    failures += assertImageEquals(&direct, &listed);

    tiny_image_destroy(&direct);
    tiny_image_destroy(&listed);

    // and an ellipse, whose upright path goes through the same fill
    if (!make_canvas(&direct, 60, 60)) return failures + 1;
    if (!make_canvas(&listed, 60, 60)) {
        tiny_image_destroy(&direct);
        return failures + 1;
    }

    tiny_image_fill_ellipse(&direct, 30, 30, 20, 12, GREEN);

    tiny_display_init(&list);
    tiny_display_ellipse(&list, 30.0f, 30.0f, 20.0f, 12.0f, GREEN);
    failures += assertEquals(tiny_display_render(&list, &listed), 0);
    failures += assertImageEquals(&direct, &listed);

    tiny_image_destroy(&direct);
    tiny_image_destroy(&listed);
    return failures;
}

/** A translation moves a shape, and save and restore bracket it. */
static int transforms(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 40, 40)) return 1;

    tiny_display_init(&list);
    tiny_display_translate(&list, 10.0f, 5.0f);
    tiny_display_rect(&list, 0.0f, 0.0f, 4.0f, 4.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(at(&image, 10, 5)[0], 255);
    failures += assertEquals(at(&image, 13, 8)[0], 255);
    failures += assertEquals(at(&image, 9, 5)[0], 0);

    // a save and a restore put the transform back exactly
    tiny_display_init(&list);
    tiny_display_translate(&list, 3.0f, 3.0f);
    failures += assertEquals(tiny_display_save(&list), 0);
    tiny_display_translate(&list, 20.0f, 20.0f);
    tiny_display_scale(&list, 2.0f, 2.0f);
    failures += assertEquals(tiny_display_restore(&list), 0);

    failures += assertFloatEquals(list.transform[0], 1.0f, 1e-6f);
    failures += assertFloatEquals(list.transform[4], 3.0f, 1e-6f);
    failures += assertFloatEquals(list.transform[5], 3.0f, 1e-6f);

    // restoring with nothing saved is an error rather than a reset
    failures += assertEquals(tiny_display_restore(&list), TINYIMG_ERR_BOUNDS);

    // the stack is finite and says so
    tiny_display_init(&list);
    for (uint32_t i = 0; i < 8u; i++) {
        failures += assertEquals(tiny_display_save(&list), 0);
    }
    failures += assertEquals(tiny_display_save(&list), TINYIMG_ERR_BOUNDS);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A scale composes with a translation in the order a caller reads.
 *
 * `translate(10, 10)` then `scale(2, 2)` has to put a unit square at (10, 10)
 * two pixels wide, not at (20, 20). Composing the two matrices the other way
 * round gives the second answer and looks correct until a transform stack is
 * more than one deep.
 */
static int composition_order(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 40, 40)) return 1;

    tiny_display_init(&list);
    tiny_display_translate(&list, 10.0f, 10.0f);
    tiny_display_scale(&list, 2.0f, 2.0f);
    tiny_display_rect(&list, 0.0f, 0.0f, 3.0f, 3.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(at(&image, 10, 10)[0], 255);
    failures += assertEquals(at(&image, 15, 15)[0], 255);
    failures += assertEquals(at(&image, 16, 16)[0], 0);
    failures += assertEquals(at(&image, 20, 20)[0], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** A quarter turn puts a shape where the turn points, and stays a rectangle. */
static int rotation(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 60, 60)) return 1;

    tiny_display_init(&list);
    tiny_display_translate(&list, 30.0f, 30.0f);
    tiny_display_rotate(&list, 90.0f);
    tiny_display_rect(&list, 0.0f, 0.0f, 20.0f, 4.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);

    // a bar along positive x, turned a quarter clockwise, runs along positive
    // y. its four columns are 26 through 29: the turned corners land at x = 26
    // and x = 30, and a rect from 26 spanning 4 covers 26 to 29, which is the
    // same convention the upright path uses
    failures += assertEquals(at(&image, 28, 40)[0], 255);
    failures += assertEquals(at(&image, 26, 40)[0], 255);
    failures += assertEquals(at(&image, 30, 40)[0], 0);
    failures += assertEquals(at(&image, 40, 30)[0], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** A shape outside the target never reaches it, and the list says so. */
static int culling(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 20, 20)) return 1;

    tiny_display_init(&list);
    tiny_display_rect(&list, 100.0f, 100.0f, 10.0f, 10.0f, RED);
    tiny_display_rect(&list, -50.0f, 2.0f, 10.0f, 10.0f, RED);
    tiny_display_rect(&list, 2.0f, 2.0f, 4.0f, 4.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.culled, 2);
    failures += assertEquals(list.covered, 0);
    failures += assertEquals(at(&image, 3, 3)[0], 255);

    tiny_image_destroy(&image);
    return failures;
}

/** An opaque shape over an earlier one drops it before any pixel is written. */
static int covering(void) {
    int failures = 0;
    TinyImage image;
    TinyImage kept;
    TinyDisplayList list;

    if (!make_canvas(&image, 30, 30)) return 1;

    // green under an opaque red that covers it entirely: no green survives,
    // and the list reports the shape it never drew
    tiny_display_init(&list);
    tiny_display_rect(&list, 5.0f, 5.0f, 6.0f, 6.0f, GREEN);
    tiny_display_rect(&list, 0.0f, 0.0f, 30.0f, 30.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.covered, 1);
    failures += assertEquals(at(&image, 7, 7)[1], 0);
    failures += assertEquals(at(&image, 7, 7)[0], 255);

    // the elimination has to be invisible: the same list with the cover made
    // one pixel too small keeps the green, so the rule is not merely dropping
    // whatever is underneath
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 30, 30)) return failures + 1;

    tiny_display_init(&list);
    tiny_display_rect(&list, 5.0f, 5.0f, 6.0f, 6.0f, GREEN);
    tiny_display_rect(&list, 6.0f, 5.0f, 24.0f, 25.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.covered, 0);
    failures += assertEquals(at(&image, 5, 7)[1], 255);

    // a partly transparent cover is not a cover, however large
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 30, 30)) return failures + 1;
    if (!make_canvas(&kept, 30, 30)) {
        tiny_image_destroy(&image);
        return failures + 1;
    }

    tiny_display_init(&list);
    tiny_display_rect(&list, 5.0f, 5.0f, 6.0f, 6.0f, GREEN);
    tiny_display_rect(&list, 0.0f, 0.0f, 30.0f, 30.0f, CLEAR_RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.covered, 0);

    // an ellipse does not cover its own box, so it is not a cover either
    tiny_display_init(&list);
    tiny_display_rect(&list, 5.0f, 5.0f, 6.0f, 6.0f, GREEN);
    tiny_display_ellipse(&list, 15.0f, 15.0f, 40.0f, 40.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &kept), 0);
    failures += assertEquals(list.covered, 0);

    tiny_image_destroy(&image);
    tiny_image_destroy(&kept);
    return failures;
}

/** A rotated cover is not a cover, because it leaves its own box uncovered. */
static int rotated_cover(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 40, 40)) return 1;

    tiny_display_init(&list);
    tiny_display_rect(&list, 2.0f, 2.0f, 4.0f, 4.0f, GREEN);
    tiny_display_save(&list);
    tiny_display_translate(&list, 20.0f, 20.0f);
    tiny_display_rotate(&list, 30.0f);
    tiny_display_rect(&list, -60.0f, -60.0f, 120.0f, 120.0f, RED);
    tiny_display_restore(&list);

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.covered, 0);

    tiny_image_destroy(&image);
    return failures;
}

/** The bounds cover every shape and are empty for an empty list. */
static int bounds(void) {
    int failures = 0;
    TinyDisplayList list;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;

    tiny_display_init(&list);
    failures +=
        assertEquals(tiny_display_bounds(&list, &x, &y, &width, &height), 0);
    failures += assertEquals(width, 0);
    failures += assertEquals(height, 0);

    tiny_display_rect(&list, 10.0f, 20.0f, 5.0f, 5.0f, RED);
    tiny_display_rect(&list, -4.0f, 2.0f, 6.0f, 3.0f, RED);

    failures +=
        assertEquals(tiny_display_bounds(&list, &x, &y, &width, &height), 0);
    failures += assertEquals(x, -4);
    failures += assertEquals(y, 2);
    failures += assertEquals(width, 19);
    failures += assertEquals(height, 23);

    return failures;
}

/** The list is finite in shapes and in points, and says so rather than grows.
 */
static int capacity(void) {
    int failures = 0;
    TinyDisplayList list;
    float xs[8] = {0.0f, 4.0f, 4.0f, 0.0f, 1.0f, 2.0f, 3.0f, 1.5f};
    float ys[8] = {0.0f, 0.0f, 4.0f, 4.0f, 1.0f, 2.0f, 3.0f, 2.5f};

    tiny_display_init(&list);

    for (uint32_t i = 0; i < 64u; i++) {
        failures += assertEquals(
            tiny_display_rect(&list, 0.0f, 0.0f, 1.0f, 1.0f, RED), 0
        );
    }

    failures += assertEquals(
        tiny_display_rect(&list, 0.0f, 0.0f, 1.0f, 1.0f, RED),
        TINYIMG_ERR_BOUNDS
    );
    failures += assertEquals(
        tiny_display_ellipse(&list, 0.0f, 0.0f, 1.0f, 1.0f, RED),
        TINYIMG_ERR_BOUNDS
    );

    // the point pool is separate, so a list of polygons runs out of points
    // before it runs out of shapes
    tiny_display_init(&list);
    failures += assertEquals(
        tiny_display_polygon(&list, xs, ys, 2u, RED, TINYIMG_FILL_EVEN_ODD),
        TINYIMG_ERR_RANGE
    );

    for (uint32_t i = 0; i < 32u; i++) {
        failures += assertEquals(
            tiny_display_polygon(&list, xs, ys, 8u, RED, TINYIMG_FILL_EVEN_ODD),
            0
        );
    }

    failures += assertEquals(
        tiny_display_polygon(&list, xs, ys, 8u, RED, TINYIMG_FILL_EVEN_ODD),
        TINYIMG_ERR_BOUNDS
    );

    return failures;
}

/**
 * @brief The two fill rules disagree on a shape that crosses itself.
 *
 * A five-pointed star drawn as one path: the even-odd rule leaves the middle
 * pentagon empty, the nonzero rule fills it. Any convex polygon gives the same
 * answer under both, so this is the only shape that can tell them apart.
 */
static int fill_rules(void) {
    int failures = 0;
    TinyImage odd;
    TinyImage nonzero;
    TinyDisplayList list;
    float xs[5];
    float ys[5];

    for (uint32_t i = 0; i < 5u; i++) {
        // every second vertex, which is what makes the path cross itself
        float angle = (float) i * (4.0f * 3.14159265f / 5.0f) - 1.5707963f;

        xs[i] = 40.0f + 30.0f * tiny_cosf(angle);
        ys[i] = 40.0f + 30.0f * tiny_sinf(angle);
    }

    if (!make_canvas(&odd, 80, 80)) return 1;
    if (!make_canvas(&nonzero, 80, 80)) {
        tiny_image_destroy(&odd);
        return 1;
    }

    tiny_display_init(&list);
    tiny_display_polygon(&list, xs, ys, 5u, RED, TINYIMG_FILL_EVEN_ODD);
    failures += assertEquals(tiny_display_render(&list, &odd), 0);

    tiny_display_init(&list);
    tiny_display_polygon(&list, xs, ys, 5u, RED, TINYIMG_FILL_NONZERO);
    failures += assertEquals(tiny_display_render(&list, &nonzero), 0);

    // the center is inside the pentagon the five points enclose
    failures += assertEquals(at(&odd, 40, 40)[0], 0);
    failures += assertEquals(at(&nonzero, 40, 40)[0], 255);

    // and a point on one arm is inside under both
    failures += assertEquals(at(&odd, 40, 15)[0], 255);
    failures += assertEquals(at(&nonzero, 40, 15)[0], 255);

    tiny_image_destroy(&odd);
    tiny_image_destroy(&nonzero);
    return failures;
}

/** Every shape kind renders, and each lands where its geometry says. */
static int every_shape(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 60, 60)) return 1;

    tiny_display_init(&list);
    failures += assertEquals(
        tiny_display_round_rect(&list, 4.0f, 4.0f, 20.0f, 20.0f, 6.0f, RED), 0
    );
    failures += assertEquals(
        tiny_display_line(&list, 30.0f, 4.0f, 55.0f, 30.0f, 3.0f, GREEN), 0
    );

    float xs[3] = {10.0f, 30.0f, 10.0f};
    float ys[3] = {35.0f, 55.0f, 55.0f};

    failures += assertEquals(
        tiny_display_polygon(&list, xs, ys, 3u, RED, TINYIMG_FILL_NONZERO), 0
    );

    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertEquals(list.culled, 0);
    failures += assertEquals(list.covered, 0);

    // the rounded rectangle keeps its middle and loses its corner
    failures += assertEquals(at(&image, 14, 14)[0], 255);
    failures += assertEquals(at(&image, 4, 4)[0], 0);

    // the line reaches both ends
    failures += assertEquals(at(&image, 30, 4)[1], 255);
    failures += assertEquals(at(&image, 54, 29)[1], 255);

    // the triangle fills its lower left and not its upper right
    failures += assertEquals(at(&image, 12, 53)[0], 255);
    failures += assertEquals(at(&image, 28, 37)[0], 0);

    // and the bounds cover all three
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;

    failures +=
        assertEquals(tiny_display_bounds(&list, &x, &y, &width, &height), 0);
    failures += assertEquals(x, 4);
    failures += assertGreaterThan(width, 50);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A rotated shape goes through the polygon path and still covers.
 *
 * The upright path takes the primitives directly; a rotation cannot, so the
 * rectangle is walked as a quad and the ellipse is flattened. Both have to
 * still land on the right pixels, which is what a turn of 90 degrees checks
 * exactly: the result is a rectangle again and its corners are known.
 */
static int rotated_shapes(void) {
    int failures = 0;
    TinyImage turned;
    TinyImage upright;
    TinyDisplayList list;

    if (!make_canvas(&turned, 60, 60)) return 1;
    if (!make_canvas(&upright, 60, 60)) {
        tiny_image_destroy(&turned);
        return 1;
    }

    // a 20x8 bar about the origin, turned a quarter, is an 8x20 bar
    tiny_display_init(&list);
    tiny_display_translate(&list, 30.0f, 30.0f);
    tiny_display_rotate(&list, 90.0f);
    tiny_display_rect(&list, -10.0f, -4.0f, 20.0f, 8.0f, RED);
    failures += assertEquals(tiny_display_render(&list, &turned), 0);

    tiny_display_init(&list);
    tiny_display_rect(&list, 26.0f, 20.0f, 8.0f, 20.0f, RED);
    failures += assertEquals(tiny_display_render(&list, &upright), 0);

    // the two paths rasterize differently at the boundary, so a handful of
    // edge pixels may disagree; the areas and the interiors may not
    uint32_t differing = 0;
    uint32_t painted = 0;

    for (uint32_t i = 0; i < 60u * 60u; i++) {
        uint8_t a = turned.data[i * 4u];
        uint8_t b = upright.data[i * 4u];

        if (a != b) differing++;
        if (a != 0u) painted++;
    }

    failures += assertIn((double) painted, 150.0, 170.0);
    failures += assertLessThan(differing, 24);

    // the interior of the bar is painted in both, and a point outside it in
    // neither
    failures += assertEquals(at(&turned, 30, 30)[0], 255);
    failures += assertEquals(at(&upright, 30, 30)[0], 255);
    failures += assertEquals(at(&turned, 10, 10)[0], 0);

    tiny_image_destroy(&turned);
    tiny_image_destroy(&upright);

    // a rotated ellipse is flattened to a polygon, and a rotated rounded
    // rectangle falls back to the quad
    if (!make_canvas(&turned, 80, 80)) return failures + 1;

    tiny_display_init(&list);
    tiny_display_translate(&list, 40.0f, 40.0f);
    tiny_display_rotate(&list, 35.0f);
    tiny_display_ellipse(&list, 0.0f, 0.0f, 30.0f, 12.0f, GREEN);
    tiny_display_round_rect(&list, -8.0f, -8.0f, 16.0f, 16.0f, 4.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &turned), 0);

    // the rounded rectangle is drawn last and sits over the middle
    failures += assertEquals(at(&turned, 40, 40)[0], 255);

    // the ellipse's long axis runs at 35 degrees, so it reaches further along
    // that direction than across it
    failures += assertEquals(at(&turned, 62, 53)[1], 255);
    failures += assertEquals(at(&turned, 40, 12)[1], 0);

    tiny_image_destroy(&turned);

    // a shape under a general transform still reports its bounds, which is
    // what culling needs before anything is drawn
    tiny_display_init(&list);
    tiny_display_rotate(&list, 45.0f);
    tiny_display_rect(&list, 0.0f, 0.0f, 10.0f, 10.0f, RED);

    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;

    failures +=
        assertEquals(tiny_display_bounds(&list, &x, &y, &width, &height), 0);

    // a unit square turned by 45 degrees has a box root two times as wide
    failures += assertIn((double) width, 13.0, 16.0);

    tiny_display_init(&list);
    tiny_display_scale(&list, 2.0f, 3.0f);
    tiny_display_line(&list, 0.0f, 0.0f, 10.0f, 10.0f, 2.0f, RED);

    failures +=
        assertEquals(tiny_display_bounds(&list, &x, &y, &width, &height), 0);
    failures += assertGreaterThan(height, width);

    return failures;
}

/** A transform set outright replaces the stack's current one. */
static int explicit_transform(void) {
    int failures = 0;
    TinyImage image;
    TinyDisplayList list;

    if (!make_canvas(&image, 40, 40)) return 1;

    tiny_display_init(&list);
    tiny_display_translate(&list, 100.0f, 100.0f);

    // whatever was there before, this is the transform now
    float identity[6] = {1.0f, 0.0f, 0.0f, 1.0f, 5.0f, 5.0f};
    failures += assertEquals(tiny_display_set_transform(&list, identity), 0);

    tiny_display_rect(&list, 0.0f, 0.0f, 4.0f, 4.0f, RED);
    failures += assertEquals(tiny_display_render(&list, &image), 0);

    failures += assertEquals(list.culled, 0);
    failures += assertEquals(at(&image, 5, 5)[0], 255);

    // the blend mode applies to the shapes added after it
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    tiny_display_init(&list);
    tiny_display_rect(&list, 0.0f, 0.0f, 20.0f, 20.0f, GREEN);
    failures +=
        assertEquals(tiny_display_blend(&list, TINYIMG_BLEND_MULTIPLY), 0);
    tiny_display_rect(&list, 0.0f, 0.0f, 20.0f, 20.0f, RED);

    failures += assertEquals(tiny_display_render(&list, &image), 0);

    // green under red, multiplied, is black; a cover elimination would have
    // dropped the green and left red, so this also proves the mode blocks it
    failures += assertEquals(list.covered, 0);
    failures += assertEquals(at(&image, 10, 10)[0], 0);
    failures += assertEquals(at(&image, 10, 10)[1], 0);

    tiny_image_destroy(&image);
    return failures;
}

static int nulls(void) {
    int failures = 0;
    TinyDisplayList list;
    TinyImage image;

    if (!make_canvas(&image, 4, 4)) return 1;

    tiny_display_init(&list);

    failures += assertEquals(tiny_display_init(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_save(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_restore(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_translate(0, 1, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_scale(0, 1, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_rotate(0, 1), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_display_set_transform(&list, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_blend(0, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_display_rect(&list, 0, 0, 1, 1, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_render(0, &image), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_display_render(&list, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_display_bounds(&list, 0, 0, 0, 0), TINYIMG_ERR_NULL);

    // an empty list renders nothing and is not an error
    failures += assertEquals(tiny_display_render(&list, &image), 0);
    failures += assertGreaterThan(tiny_display_sizeof(), 0);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += matches_the_primitive();
    failures += transforms();
    failures += composition_order();
    failures += rotation();
    failures += culling();
    failures += covering();
    failures += rotated_cover();
    failures += bounds();
    failures += capacity();
    failures += every_shape();
    failures += rotated_shapes();
    failures += explicit_transform();
    failures += fill_rules();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
