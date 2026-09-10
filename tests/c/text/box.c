#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief What an OG card needs: stroke, shadow, justify, vertical align,
 * ellipsis and per-line metrics.
 *
 * Each one is asserted through a property a broken implementation gets wrong,
 * not against a stored bitmap. The two strongest are worth naming: a hard
 * shadow's support has to be **exactly** the fill's support unioned with itself
 * translated, and a stroke must not change the fill's own coverage at all,
 * because the fill is drawn last and on top.
 */

static uint8_t* loadFont(const char* name, TinyFont* font, size_t* size) {
    char path[256];
    snprintf(path, sizeof(path), "derived/fonts/%s", name);

    unsigned char* bytes = readFixture(path, size);
    if (!bytes) return 0;

    if (tiny_font_load_bytes(font, bytes, *size) != TINYIMG_OK) {
        free(bytes);
        return 0;
    }

    return bytes;
}

/** Where the topmost, leftmost, bottommost and rightmost ink is. */
static void bounds(
    const TinyImage* image, uint32_t* x0, uint32_t* y0, uint32_t* x1,
    uint32_t* y1
) {
    *x0 = image->width;
    *y0 = image->height;
    *x1 = 0;
    *y1 = 0;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            if (image->data[(size_t) y * image->width + x] == 0) continue;

            if (x < *x0) *x0 = x;
            if (y < *y0) *y0 = y;
            if (x > *x1) *x1 = x;
            if (y > *y1) *y1 = y;
        }
    }
}

/** Rightmost ink on one row, or -1 for a blank row. */
static int32_t right_edge(const TinyImage* image, uint32_t y) {
    for (int32_t x = (int32_t) image->width - 1; x >= 0; x--) {
        if (image->data[(size_t) y * image->width + (uint32_t) x] != 0) {
            return x;
        }
    }

    return -1;
}

/**
 * A hard shadow is the same coverage drawn at an offset, and nothing else.
 *
 * The support is the check rather than the pixels: an antialiased edge covered
 * partly by the shadow and partly by the fill composites twice and comes out
 * darker than either, so the two images agree on **which** pixels carry ink and
 * not on how much. Which pixels is the property the offset has to get right.
 */
static int shadowIsAnOffsetCopy(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    const int32_t dx = 6;
    const int32_t dy = 5;

    TinyImage plain;
    TinyImage shadowed;
    memset(&plain, 0, sizeof(plain));
    memset(&shadowed, 0, sizeof(shadowed));

    failures += assertEquals(tiny_image_create(&plain, 160, 60, 1), TINYIMG_OK);
    failures +=
        assertEquals(tiny_image_create(&shadowed, 160, 60, 1), TINYIMG_OK);

    uint8_t white = 255;

    TinyTextStyle bare;
    tiny_text_style(&bare, 34.0f);

    TinyTextStyle cast;
    tiny_text_style(&cast, 34.0f);
    cast.shadow_x = (float) dx;
    cast.shadow_y = (float) dy;
    cast.shadow_color[0] = 255u;

    failures += assertEquals(
        tiny_image_draw_text(&plain, &font, "Hey", 8, 8, &bare, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text(&shadowed, &font, "Hey", 8, 8, &cast, &white),
        TINYIMG_OK
    );

    uint32_t wrong = 0;

    for (uint32_t y = 0; y < plain.height; y++) {
        for (uint32_t x = 0; x < plain.width; x++) {
            int here = plain.data[(size_t) y * plain.width + x] != 0;

            int32_t from_x = (int32_t) x - dx;
            int32_t from_y = (int32_t) y - dy;
            int shifted = 0;

            if (from_x >= 0 && from_y >= 0 && from_x < (int32_t) plain.width &&
                from_y < (int32_t) plain.height) {
                shifted =
                    plain.data
                        [(size_t) from_y * plain.width + (uint32_t) from_x] !=
                    0;
            }

            int want = here || shifted;
            int have = shadowed.data[(size_t) y * shadowed.width + x] != 0;

            if (want != have) wrong++;
        }
    }

    failures += assertEquals((long) wrong, 0L);

    // and the shadow put ink somewhere the plain draw did not, or the union
    // above would hold trivially
    uint32_t px0;
    uint32_t py0;
    uint32_t px1;
    uint32_t py1;
    uint32_t sx0;
    uint32_t sy0;
    uint32_t sx1;
    uint32_t sy1;

    bounds(&plain, &px0, &py0, &px1, &py1);
    bounds(&shadowed, &sx0, &sy0, &sx1, &sy1);

    failures += assertEquals((long) sx1, (long) px1 + dx);
    failures += assertEquals((long) sy1, (long) py1 + dy);
    failures += assertEquals((long) sx0, (long) px0);
    failures += assertEquals((long) sy0, (long) py0);

    tiny_image_destroy(&plain);
    tiny_image_destroy(&shadowed);
    free(bytes);
    return failures;
}

/** A softened shadow spreads the same total ink over a wider area. */
static int blurSoftensRatherThanMoves(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage hard;
    TinyImage soft;
    memset(&hard, 0, sizeof(hard));
    memset(&soft, 0, sizeof(soft));

    tiny_image_create(&hard, 160, 70, 1);
    tiny_image_create(&soft, 160, 70, 1);

    uint8_t white = 255;

    TinyTextStyle a;
    tiny_text_style(&a, 34.0f);
    a.shadow_x = 4.0f;
    a.shadow_y = 4.0f;
    a.shadow_color[0] = 255u;

    TinyTextStyle b = a;
    b.shadow_blur = 3.0f;

    failures += assertEquals(
        tiny_image_draw_text(&hard, &font, "Hey", 8, 8, &a, &white), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text(&soft, &font, "Hey", 8, 8, &b, &white), TINYIMG_OK
    );

    uint32_t hx0;
    uint32_t hy0;
    uint32_t hx1;
    uint32_t hy1;
    uint32_t sx0;
    uint32_t sy0;
    uint32_t sx1;
    uint32_t sy1;

    bounds(&hard, &hx0, &hy0, &hx1, &hy1);
    bounds(&soft, &sx0, &sy0, &sx1, &sy1);

    /*
     * Wider at the far corner, because the blur reaches past the coverage it
     * started from.
     *
     * Not at the near corner, and that is the trap: the shadow sits four pixels
     * right and down, so its softened left edge is still inside the fill's and
     * the fill is what sets the bounding box there. The zero-offset case below
     * is the one that can see all four sides.
     */
    failures += assertEquals((long) sx0, (long) hx0);
    failures += assertEquals((long) sy0, (long) hy0);
    failures += assertGreaterThan((double) sx1, (double) hx1);
    failures += assertGreaterThan((double) sy1, (double) hy1);

    // and lighter, because the same ink is spread over more pixels; the fill on
    // top is the same in both, so the difference is all shadow
    long hard_ink = 0;
    long soft_ink = 0;
    size_t pixels = (size_t) hard.width * hard.height;

    for (size_t i = 0; i < pixels; i++) {
        hard_ink += hard.data[i];
        soft_ink += soft.data[i];
    }

    failures += assertLessThan((double) soft_ink, (double) hard_ink);

    // a shadow with no offset at all, where the blur is the only thing moving
    // the edges, so all four sides have to grow
    TinyImage centered;
    memset(&centered, 0, sizeof(centered));
    tiny_image_create(&centered, 160, 70, 1);

    TinyTextStyle halo;
    tiny_text_style(&halo, 34.0f);
    halo.shadow_blur = 4.0f;
    halo.shadow_color[0] = 255u;

    TinyImage bare;
    memset(&bare, 0, sizeof(bare));
    tiny_image_create(&bare, 160, 70, 1);

    TinyTextStyle none;
    tiny_text_style(&none, 34.0f);

    failures += assertEquals(
        tiny_image_draw_text(&centered, &font, "Hey", 20, 14, &halo, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text(&bare, &font, "Hey", 20, 14, &none, &white),
        TINYIMG_OK
    );

    uint32_t cx0;
    uint32_t cy0;
    uint32_t cx1;
    uint32_t cy1;
    uint32_t bx0;
    uint32_t by0;
    uint32_t bx1;
    uint32_t by1;

    bounds(&centered, &cx0, &cy0, &cx1, &cy1);
    bounds(&bare, &bx0, &by0, &bx1, &by1);

    failures += assertLessThan((double) cx0, (double) bx0);
    failures += assertLessThan((double) cy0, (double) by0);
    failures += assertGreaterThan((double) cx1, (double) bx1);
    failures += assertGreaterThan((double) cy1, (double) by1);

    tiny_image_destroy(&centered);
    tiny_image_destroy(&bare);
    tiny_image_destroy(&hard);
    tiny_image_destroy(&soft);
    free(bytes);
    return failures;
}

/**
 * An outline grows the glyph and leaves the fill alone.
 *
 * The second half is the one worth asserting. Every pixel the plain draw
 * covered fully has to still be fully covered, because the fill is the last
 * pass and goes on top; an implementation that drew the outline over the fill
 * would pass a bounding box check and fail this.
 */
static int strokeGrowsWithoutCoveringTheFill(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage plain;
    TinyImage outlined;
    memset(&plain, 0, sizeof(plain));
    memset(&outlined, 0, sizeof(outlined));

    tiny_image_create(&plain, 160, 70, 1);
    tiny_image_create(&outlined, 160, 70, 1);

    uint8_t white = 255;

    TinyTextStyle bare;
    tiny_text_style(&bare, 34.0f);

    TinyTextStyle ringed;
    tiny_text_style(&ringed, 34.0f);
    ringed.stroke = 3.0f;
    ringed.stroke_color[0] = 90u;

    failures += assertEquals(
        tiny_image_draw_text(&plain, &font, "Hey", 12, 12, &bare, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text(&outlined, &font, "Hey", 12, 12, &ringed, &white),
        TINYIMG_OK
    );

    uint32_t px0;
    uint32_t py0;
    uint32_t px1;
    uint32_t py1;
    uint32_t ox0;
    uint32_t oy0;
    uint32_t ox1;
    uint32_t oy1;

    bounds(&plain, &px0, &py0, &px1, &py1);
    bounds(&outlined, &ox0, &oy0, &ox1, &oy1);

    // three pixels of outline, so the box grows by about three on every side
    failures += assertEquals((long) (px0 - ox0), 3L);
    failures += assertEquals((long) (py0 - oy0), 3L);
    failures += assertEquals((long) (ox1 - px1), 3L);
    failures += assertEquals((long) (oy1 - py1), 3L);

    uint32_t dimmed = 0;
    size_t pixels = (size_t) plain.width * plain.height;

    for (size_t i = 0; i < pixels; i++) {
        if (plain.data[i] == 255u && outlined.data[i] != 255u) dimmed++;
    }

    failures += assertEquals((long) dimmed, 0L);

    tiny_image_destroy(&plain);
    tiny_image_destroy(&outlined);
    free(bytes);
    return failures;
}

/** Justification reaches both edges on every line but the last. */
static int justifyFillsTheMeasure(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    static const char* copy =
        "the quick brown fox jumps over the lazy dog and then it does it again";

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 260, 160, 1);

    uint8_t white = 255;

    TinyTextStyle style;
    tiny_text_style(&style, 18.0f);

    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 240u;
    box.height = 150u;
    box.align = TINYIMG_ALIGN_JUSTIFY;

    TinyTextLine lines[8];
    uint32_t count = 0;

    failures += assertEquals(
        tiny_text_lines(&font, copy, &box, &style, lines, 8u, &count),
        TINYIMG_OK
    );
    failures += assertGreaterThan((double) count, 2.0);

    failures += assertEquals(
        tiny_image_draw_text_in(
            &image, &font, copy, 0, 0, &box, &style, &white
        ),
        TINYIMG_OK
    );

    // every line but the last ends within a glyph's width of the right edge
    uint32_t reached = 0;

    for (uint32_t line = 0; line + 1u < count; line++) {
        int32_t best = -1;

        uint32_t from = (uint32_t) lines[line].y;
        uint32_t to = (uint32_t) (lines[line].y + style.line_height * 18.0f);

        if (to > image.height) to = image.height;

        for (uint32_t y = from; y < to; y++) {
            int32_t edge = right_edge(&image, y);
            if (edge > best) best = edge;
        }

        if (best >= (int32_t) box.width - 6) reached++;
    }

    failures += assertEquals((long) reached, (long) count - 1L);

    // and the last line is ragged, which is the half a naive implementation
    // gets wrong by stretching four words across the whole measure
    failures +=
        assertLessThan((double) lines[count - 1u].width, (double) box.width);

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

/** Vertical alignment moves the run by exactly the slack it was given. */
static int verticalAlignmentMovesTheRun(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    uint8_t white = 255;

    TinyTextStyle style;
    tiny_text_style(&style, 20.0f);

    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 200u;
    box.height = 120u;

    uint32_t tops[3];
    static const TinyTextVAlign order[3] = {
        TINYIMG_VALIGN_TOP, TINYIMG_VALIGN_MIDDLE, TINYIMG_VALIGN_BOTTOM
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, 210, 130, 1);

        box.valign = order[i];

        failures += assertEquals(
            tiny_image_draw_text_in(
                &image, &font, "one line", 0, 0, &box, &style, &white
            ),
            TINYIMG_OK
        );

        uint32_t x0;
        uint32_t y0;
        uint32_t x1;
        uint32_t y1;
        bounds(&image, &x0, &y0, &x1, &y1);

        tops[i] = y0;
        tiny_image_destroy(&image);
    }

    TinyFontMetrics metrics;
    failures +=
        assertEquals(tiny_font_metrics(&font, 20.0f, &metrics), TINYIMG_OK);

    float slack = 120.0f - metrics.line_height;

    // middle is half the slack down and bottom is all of it, to the pixel the
    // rasterizer rounded to
    failures += assertLessThan(
        (double) tops[1] - (double) tops[0] - (double) slack * 0.5, 1.5
    );
    failures += assertGreaterThan(
        (double) tops[1] - (double) tops[0] - (double) slack * 0.5, -1.5
    );
    failures += assertLessThan(
        (double) tops[2] - (double) tops[0] - (double) slack, 1.5
    );
    failures += assertGreaterThan(
        (double) tops[2] - (double) tops[0] - (double) slack, -1.5
    );

    free(bytes);
    return failures;
}

/** An ellipsis replaces the tail of the last line that fits. */
static int ellipsisEndsTheLastLineThatFits(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    static const char* copy =
        "a headline long enough to need three whole lines";

    TinyTextStyle style;
    tiny_text_style(&style, 18.0f);

    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 150u;
    box.height = 46u;

    TinyTextLine clipped[8];
    uint32_t clipped_count = 0;

    failures += assertEquals(
        tiny_text_lines(&font, copy, &box, &style, clipped, 8u, &clipped_count),
        TINYIMG_OK
    );

    // more lines than the box holds, or the case under test does not arise
    failures += assertGreaterThan((double) clipped_count, 2.0);
    failures += assertEquals((long) clipped[0].ellipsized, 0L);
    failures += assertEquals((long) clipped[2].clipped, 1L);

    box.overflow = TINYIMG_OVERFLOW_ELLIPSIS;

    TinyTextLine cut[8];
    uint32_t cut_count = 0;

    failures += assertEquals(
        tiny_text_lines(&font, copy, &box, &style, cut, 8u, &cut_count),
        TINYIMG_OK
    );

    // the count is what the run takes either way; what changes is which line
    // carries the mark and how much of it is drawn
    failures += assertEquals((long) cut_count, (long) clipped_count);
    failures += assertEquals((long) cut[1].ellipsized, 1L);
    failures += assertEquals((long) cut[0].ellipsized, 0L);
    failures +=
        assertLessThan((double) cut[1].length, (double) clipped[1].length);

    // drawn, the mark's line stays inside the measure
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 160, 60, 1);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text_in(
            &image, &font, copy, 0, 0, &box, &style, &white
        ),
        TINYIMG_OK
    );

    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
    bounds(&image, &x0, &y0, &x1, &y1);

    failures += assertLessThan((double) x1, (double) box.width);

    // and nothing was drawn below the box
    failures += assertLessThan((double) y1, (double) box.height + 1.0);

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

/** The per-line report reconstructs the run and agrees with the whole-run
 * measure. */
static int linesDescribeTheLayout(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    static const char* copy = "first line\nsecond line here\nthird";

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    TinyTextLine lines[4];
    uint32_t count = 0;

    failures += assertEquals(
        tiny_text_lines(&font, copy, 0, &style, lines, 4u, &count), TINYIMG_OK
    );
    failures += assertEquals((long) count, 3L);

    // the offsets and lengths cut the original string back into its lines
    failures += assertEquals((long) lines[0].at, 0L);
    failures += assertEquals((long) lines[0].length, 10L);
    failures += assertEquals((long) lines[1].at, 11L);
    failures += assertEquals((long) lines[1].length, 16L);
    failures += assertEquals((long) lines[2].at, 28L);
    failures += assertEquals((long) lines[2].length, 5L);

    // the baselines step by exactly one line height
    float step = lines[1].baseline - lines[0].baseline;
    failures += assertEquals(
        (long) (lines[2].baseline - lines[1].baseline), (long) step
    );

    TinyFontMetrics metrics;
    tiny_font_metrics(&font, 16.0f, &metrics);
    failures +=
        assertLessThan((double) step - (double) metrics.line_height, 0.01);

    // and the glyph counts add up to the whole-run measure's
    TinyTextMetrics whole;
    failures += assertEquals(
        tiny_text_measure(&font, copy, &style, &whole), TINYIMG_OK
    );

    uint32_t glyphs = 0;
    for (uint32_t i = 0; i < count; i++) glyphs += lines[i].glyphs;

    failures += assertEquals((long) glyphs, (long) whole.glyphs);
    failures += assertEquals((long) count, (long) whole.lines);

    // alignment is in the report, so a caller can place something beside a line
    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 300u;
    box.align = TINYIMG_ALIGN_RIGHT;

    TinyTextLine right[4];
    failures += assertEquals(
        tiny_text_lines(&font, copy, &box, &style, right, 4u, &count),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < count; i++) {
        failures += assertLessThan(
            (double) (right[i].x + right[i].width) - 300.0, 0.01
        );
        failures += assertGreaterThan(
            (double) (right[i].x + right[i].width) - 300.0, -0.01
        );
    }

    // a capacity below the line count still reports the count
    uint32_t only = 0;
    failures += assertEquals(
        tiny_text_lines(&font, copy, 0, &style, right, 1u, &only), TINYIMG_OK
    );
    failures += assertEquals((long) only, 3L);

    free(bytes);
    return failures;
}

/** A zeroed style still draws, and a zeroed box means no box at all. */
static int zeroedStructuresStillDraw(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage plain;
    TinyImage boxed;
    memset(&plain, 0, sizeof(plain));
    memset(&boxed, 0, sizeof(boxed));

    tiny_image_create(&plain, 120, 50, 1);
    tiny_image_create(&boxed, 120, 50, 1);

    uint8_t white = 255;

    TinyTextStyle zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    zeroed.size = 24.0f;

    TinyTextBox nothing;
    memset(&nothing, 0, sizeof(nothing));

    failures += assertEquals(
        tiny_image_draw_text(&plain, &font, "Hi", 4, 4, &zeroed, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text_in(
            &boxed, &font, "Hi", 4, 4, &nothing, &zeroed, &white
        ),
        TINYIMG_OK
    );

    // a zeroed box has to be the same image as no box at all
    failures += assertEquals(
        memcmp(plain.data, boxed.data, (size_t) plain.width * plain.height), 0
    );

    tiny_image_destroy(&plain);
    tiny_image_destroy(&boxed);
    free(bytes);
    return failures;
}

static int rejections(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 40, 40, 1);

    uint8_t white = 255;
    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    uint32_t count = 0;

    failures += assertEquals(
        tiny_image_draw_text_in(0, &font, "x", 0, 0, 0, &style, &white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text_in(&image, 0, "x", 0, 0, 0, &style, &white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_text_lines(&font, "x", 0, &style, 0, 0u, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_text_lines(0, "x", 0, &style, 0, 0u, &count), TINYIMG_ERR_NULL
    );

    // counting with no output array is the cheap way to ask how many lines a
    // string takes
    failures += assertEquals(
        tiny_text_lines(&font, "a\nb\nc", 0, &style, 0, 0u, &count), TINYIMG_OK
    );
    failures += assertEquals((long) count, 3L);

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    printf("-- shadow --\n");
    failures += shadowIsAnOffsetCopy();
    failures += blurSoftensRatherThanMoves();

    printf("-- stroke --\n");
    failures += strokeGrowsWithoutCoveringTheFill();

    printf("-- alignment --\n");
    failures += justifyFillsTheMeasure();
    failures += verticalAlignmentMovesTheRun();

    printf("-- overflow --\n");
    failures += ellipsisEndsTheLastLineThatFits();

    printf("-- metrics --\n");
    failures += linesDescribeTheLayout();
    failures += zeroedStructuresStillDraw();

    printf("-- rejections --\n");
    failures += rejections();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
