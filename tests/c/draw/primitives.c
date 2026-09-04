#include "test.h"

static const uint8_t RED[4] = {255, 0, 0, 255};
static const uint8_t BLACK[4] = {0, 0, 0, 255};

/** An RGBA canvas cleared to opaque black. */
static int make_canvas(TinyImage* image, uint32_t width, uint32_t height) {
    if (tiny_image_create(image, width, height, 4) != TINYIMG_OK) return 0;

    for (uint32_t i = 0; i < width * height; i++) {
        image->data[i * 4u + 3u] = 255u;
    }

    return 1;
}

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** How many pixels differ from the cleared canvas. */
static uint32_t painted(const TinyImage* image) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < image->width * image->height; i++) {
        const uint8_t* p = image->data + i * image->channels;
        if (p[0] != 0u || p[1] != 0u || p[2] != 0u) count++;
    }

    return count;
}

/**
 * @brief Whether a 4-connected flood from a point reaches the image's edge.
 *
 * The test an outline has to pass, and the one a pixel count cannot make: a
 * curve with a single diagonal-only gap has the right length and leaks.
 *
 * @param image The image; a painted pixel is a wall.
 * @param x Where to start, which must not itself be painted.
 * @param y Where to start.
 * @return int Non-zero when the flood escapes.
 */
static int escapes(const TinyImage* image, uint32_t x, uint32_t y) {
    uint32_t total = image->width * image->height;
    uint8_t* seen = calloc(total, 1);
    uint32_t* queue = malloc(total * sizeof(uint32_t));
    uint32_t head = 0;
    uint32_t tail = 0;
    int out = 0;

    if (!seen || !queue) {
        free(seen);
        free(queue);
        return 1;
    }

    queue[tail++] = y * image->width + x;
    seen[y * image->width + x] = 1u;

    while (head < tail && !out) {
        uint32_t at_index = queue[head++];
        int32_t cx = (int32_t) (at_index % image->width);
        int32_t cy = (int32_t) (at_index / image->width);

        static const int32_t STEP[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

        for (uint32_t k = 0; k < 4u; k++) {
            int32_t nx = cx + STEP[k][0];
            int32_t ny = cy + STEP[k][1];

            if (nx < 0 || ny < 0 || nx >= (int32_t) image->width ||
                ny >= (int32_t) image->height) {
                out = 1;
                break;
            }

            uint32_t next = (uint32_t) ny * image->width + (uint32_t) nx;
            if (seen[next]) continue;
            if (at(image, (uint32_t) nx, (uint32_t) ny)[0] != 0u) continue;

            seen[next] = 1u;
            queue[tail++] = next;
        }
    }

    free(seen);
    free(queue);
    return out;
}

/** A run of pixels is exactly as long as the coordinates asked for. */
static int runs(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 20, 20)) return 1;

    failures += assertEquals(tiny_image_hline(&image, 3, 5, 9, 0, RED), 0);
    failures += assertEquals(painted(&image), 7);
    failures += assertEquals(at(&image, 3, 5)[0], 255);
    failures += assertEquals(at(&image, 9, 5)[0], 255);
    failures += assertEquals(at(&image, 2, 5)[0], 0);
    failures += assertEquals(at(&image, 10, 5)[0], 0);

    // the ends may be given in either order, and a reversed run is the same
    // set of pixels rather than none
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    failures += assertEquals(tiny_image_hline(&image, 9, 5, 3, 0, RED), 0);
    failures += assertEquals(painted(&image), 7);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    failures += assertEquals(tiny_image_vline(&image, 4, 2, 0, 11, RED), 0);
    failures += assertEquals(painted(&image), 10);
    failures += assertEquals(at(&image, 4, 2)[0], 255);
    failures += assertEquals(at(&image, 4, 11)[0], 255);

    tiny_image_destroy(&image);
    return failures;
}

/** A shape crossing the edge draws the part inside and does not fail. */
static int clipping(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 10, 10)) return 1;

    failures += assertEquals(tiny_image_hline(&image, -5, 4, 4, 0, RED), 0);
    failures += assertEquals(painted(&image), 5);

    failures += assertEquals(tiny_image_hline(&image, 6, 4, 100, 0, RED), 0);
    failures += assertEquals(painted(&image), 9);

    // entirely outside, on both sides and above
    failures += assertEquals(tiny_image_hline(&image, -20, 4, -10, 0, RED), 0);
    failures += assertEquals(tiny_image_hline(&image, 40, 4, 60, 0, RED), 0);
    failures += assertEquals(tiny_image_hline(&image, 0, -1, 9, 0, RED), 0);
    failures += assertEquals(tiny_image_hline(&image, 0, 10, 9, 0, RED), 0);
    failures += assertEquals(painted(&image), 9);

    failures +=
        assertEquals(tiny_image_fill_rectangle(&image, -3, -3, 5, 5, RED), 0);
    failures += assertEquals(painted(&image), 9 + 4);

    tiny_image_destroy(&image);
    return failures;
}

/** An outline is the border of the fill, and both cover what they say. */
static int rectangles(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 20, 20)) return 1;

    failures +=
        assertEquals(tiny_image_fill_rectangle(&image, 2, 3, 6, 4, RED), 0);
    failures += assertEquals(painted(&image), 24);
    failures += assertEquals(at(&image, 2, 3)[0], 255);
    failures += assertEquals(at(&image, 7, 6)[0], 255);
    failures += assertEquals(at(&image, 8, 6)[0], 0);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    failures += assertEquals(tiny_image_rectangle(&image, 2, 3, 6, 4, RED), 0);
    // the perimeter of a 6x4 box, counted once at each corner
    failures += assertEquals(painted(&image), 2 * 6 + 2 * (4 - 2));
    failures += assertEquals(at(&image, 4, 4)[0], 0);

    // a one-row rectangle is a line, not two overlapping ones
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    failures += assertEquals(tiny_image_rectangle(&image, 1, 1, 5, 1, RED), 0);
    failures += assertEquals(painted(&image), 5);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 20, 20)) return failures + 1;

    failures += assertEquals(tiny_image_rectangle(&image, 1, 1, 1, 5, RED), 0);
    failures += assertEquals(painted(&image), 5);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A rounded rectangle loses only its corners, and a large radius is
 * clamped rather than refused.
 */
static int rounded(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 40, 40)) return 1;

    failures += assertEquals(
        tiny_image_fill_rounded_rectangle(&image, 4, 4, 20, 20, 0, RED), 0
    );
    failures += assertEquals(painted(&image), 400);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 40, 40)) return failures + 1;

    failures += assertEquals(
        tiny_image_fill_rounded_rectangle(&image, 4, 4, 20, 20, 6, RED), 0
    );

    uint32_t count = painted(&image);
    failures += assertLessThan(count, 400);
    failures += assertGreaterThan(count, 340);
    failures += assertEquals(at(&image, 4, 4)[0], 0);
    failures += assertEquals(at(&image, 14, 4)[0], 255);
    failures += assertEquals(at(&image, 14, 14)[0], 255);

    // a radius past half the shorter side gives a circle's area, not an error
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 40, 40)) return failures + 1;

    failures += assertEquals(
        tiny_image_fill_rounded_rectangle(&image, 4, 4, 20, 20, 999, RED), 0
    );

    double area = (double) painted(&image);
    failures += assertIn(area, 300.0, 320.0);

    tiny_image_destroy(&image);
    return failures;
}

/** A line reaches both endpoints and thickens about its own axis. */
static int lines(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 30, 30)) return 1;

    failures +=
        assertEquals(tiny_image_draw_line(&image, 2, 2, 20, 2, 1, RED), 0);
    failures += assertEquals(painted(&image), 19);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 30, 30)) return failures + 1;

    failures +=
        assertEquals(tiny_image_draw_line(&image, 2, 2, 20, 20, 1, RED), 0);
    failures += assertEquals(painted(&image), 19);
    failures += assertEquals(at(&image, 2, 2)[0], 255);
    failures += assertEquals(at(&image, 20, 20)[0], 255);
    failures += assertEquals(at(&image, 11, 11)[0], 255);

    // a thick diagonal has no gaps: every row it spans is painted, which a
    // disc stamped at each Bresenham step does not guarantee on a steep slope
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 30, 30)) return failures + 1;

    failures +=
        assertEquals(tiny_image_draw_line(&image, 5, 2, 8, 25, 5, RED), 0);

    for (uint32_t y = 3; y < 24; y++) {
        uint32_t on = 0;
        for (uint32_t x = 0; x < 30u; x++) {
            if (at(&image, x, y)[0] != 0u) on++;
        }

        failures += assertGreaterThan(on, 2);
    }

    tiny_image_destroy(&image);
    return failures;
}

/** A disc has the area a disc has, and an outline is one pixel thick. */
static int circles(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 60, 60)) return 1;

    failures +=
        assertEquals(tiny_image_fill_circle(&image, 30, 30, 20, RED), 0);

    double area = (double) painted(&image);
    double expected = 3.14159265 * 20.0 * 20.0;

    // within two percent of pi r squared, which a scanline fill of a circle
    // is by construction and a broken radius test is not
    failures += assertIn(area, expected * 0.98, expected * 1.02);
    failures += assertEquals(at(&image, 30, 30)[0], 255);
    failures += assertEquals(at(&image, 30, 9)[0], 0);

    tiny_image_destroy(&image);
    if (!make_canvas(&image, 60, 60)) return failures + 1;

    failures +=
        assertEquals(tiny_image_draw_circle(&image, 30, 30, 20, RED), 0);

    failures += assertEquals(at(&image, 30, 30)[0], 0);
    failures += assertEquals(at(&image, 30, 10)[0], 255);

    // the property that matters is that the outline is closed, which a count
    // cannot check: a 4-connected flood from the centre must not escape it.
    // measured for reference, the count is 5.6 r, which is the telescoping sum
    // of the per-row widths rather than the 2 pi r an arc-length guess gives
    failures += assertFalse(escapes(&image, 30, 30));

    // an ellipse's outline is closed for a radius pair the axes differ
    // sharply on, which is where a per-octant midpoint walk leaves gaps
    tiny_image_destroy(&image);
    if (!make_canvas(&image, 60, 60)) return failures + 1;

    failures +=
        assertEquals(tiny_image_draw_ellipse(&image, 30, 30, 25, 5, RED), 0);

    // a 5:1 ellipse is where a per-octant midpoint walk leaks, because one
    // step along the flat side skips a row
    failures += assertFalse(escapes(&image, 30, 30));

    tiny_image_destroy(&image);
    return failures;
}

/** An ellipse's area follows its two radii independently. */
static int ellipses(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 80, 40)) return 1;

    failures +=
        assertEquals(tiny_image_fill_ellipse(&image, 40, 20, 30, 10, RED), 0);

    double area = (double) painted(&image);
    double expected = 3.14159265 * 30.0 * 10.0;

    failures += assertIn(area, expected * 0.96, expected * 1.04);
    failures += assertEquals(at(&image, 40, 20)[0], 255);
    failures += assertEquals(at(&image, 69, 20)[0], 255);
    failures += assertEquals(at(&image, 40, 9)[0], 0);

    // a zero radius draws nothing rather than a line or a crash
    failures +=
        assertEquals(tiny_image_fill_ellipse(&image, 40, 20, 0, 10, RED), 0);

    tiny_image_destroy(&image);
    return failures;
}

/** Every entry point rejects a null image or colour. */
static int nulls(void) {
    int failures = 0;
    TinyImage image;

    if (!make_canvas(&image, 4, 4)) return 1;

    failures +=
        assertEquals(tiny_image_hline(0, 0, 0, 1, 0, RED), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_vline(&image, 0, 0, 0, 1, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_image_rectangle(&image, 0, 0, 1, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_fill_rectangle(&image, 0, 0, 1, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_fill_rounded_rectangle(&image, 0, 0, 1, 1, 1, 0),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_line(&image, 0, 0, 1, 1, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_circle(&image, 0, 0, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_fill_circle(&image, 0, 0, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_ellipse(&image, 0, 0, 1, 1, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_fill_ellipse(&image, 0, 0, 1, 1, 0), TINYIMG_ERR_NULL
    );

    // a zero extent is a request with nothing in it, not an error
    failures +=
        assertEquals(tiny_image_fill_rectangle(&image, 0, 0, 0, 4, BLACK), 0);
    failures +=
        assertEquals(tiny_image_rectangle(&image, 0, 0, 4, 0, BLACK), 0);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += runs();
    failures += clipping();
    failures += rectangles();
    failures += rounded();
    failures += lines();
    failures += circles();
    failures += ellipses();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
