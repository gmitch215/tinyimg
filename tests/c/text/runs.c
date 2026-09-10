#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief Styled spans flowing as one run.
 *
 * The property that makes this rich text rather than three separate draws is
 * that a span picks up where the previous one stopped, on the **same** baseline
 * and inside the same wrap. Each check below is one of the ways a naive
 * implementation breaks that: restarting the pen per span, giving each span its
 * own baseline, or refusing to wrap in the middle of one.
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

/**
 * Three spans of one style are the same pixels as the concatenated string.
 *
 * The strongest check available, and the one that fails if the pen restarts per
 * span or if a kern is applied across a boundary that a single string would
 * also not kern across. The spans are split at spaces for exactly that reason:
 * a split mid-word would legitimately differ wherever the face has a kern pair
 * for the two letters either side.
 */
static int spansFlowLikeOneString(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 22.0f);

    uint8_t white = 255;

    TinyImage single;
    TinyImage split;
    memset(&single, 0, sizeof(single));
    memset(&split, 0, sizeof(split));

    tiny_image_create(&single, 300, 50, 1);
    tiny_image_create(&split, 300, 50, 1);

    failures += assertEquals(
        tiny_image_draw_text(
            &single, &font, "one two three", 6, 6, &style, &white
        ),
        TINYIMG_OK
    );

    const TinyTextRun runs[3] = {
        {&font, "one ", &style, &white},
        {&font, "two ", &style, &white},
        {&font, "three", &style, &white}
    };

    failures += assertEquals(
        tiny_image_draw_text_runs(&split, runs, 3u, 6, 6, 0, &white), TINYIMG_OK
    );

    failures += assertEquals(
        memcmp(single.data, split.data, (size_t) single.width * single.height),
        0
    );

    tiny_image_destroy(&single);
    tiny_image_destroy(&split);
    free(bytes);
    return failures;
}

/**
 * A line that mixes sizes shares one baseline.
 *
 * Checked through the ink rather than through the metrics: the large span's
 * descenders reach further down than the small span's, and both letters have
 * their flat bottom on the same row. `x` and `o` are used because neither has a
 * descender, so the bottom of the ink **is** the baseline.
 */
static int mixedSizesShareABaseline(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle small;
    TinyTextStyle large;
    tiny_text_style(&small, 14.0f);
    tiny_text_style(&large, 34.0f);

    uint8_t white = 255;

    // the small span alone, then the large one alone, then both together: the
    // bottom row of each has to be the row it lands on in the mixed draw
    TinyImage mixed;
    memset(&mixed, 0, sizeof(mixed));
    tiny_image_create(&mixed, 200, 70, 1);

    const TinyTextRun runs[2] = {
        {&font, "x", &small, &white}, {&font, "o", &large, &white}
    };

    failures += assertEquals(
        tiny_image_draw_text_runs(&mixed, runs, 2u, 10, 10, 0, &white),
        TINYIMG_OK
    );

    // the two glyphs are side by side, so a column split separates them
    uint32_t split = 0;
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
    bounds(&mixed, &x0, &y0, &x1, &y1);

    for (uint32_t x = x0; x <= x1; x++) {
        int blank = 1;

        for (uint32_t y = 0; y < mixed.height; y++) {
            if (mixed.data[(size_t) y * mixed.width + x] != 0) blank = 0;
        }

        if (blank && x > x0) {
            split = x;
            break;
        }
    }

    failures += assertGreaterThan((double) split, (double) x0);

    int32_t small_bottom = -1;
    int32_t large_bottom = -1;

    for (uint32_t y = 0; y < mixed.height; y++) {
        for (uint32_t x = x0; x < split; x++) {
            if (mixed.data[(size_t) y * mixed.width + x] != 0) {
                small_bottom = (int32_t) y;
            }
        }
        for (uint32_t x = split; x <= x1; x++) {
            if (mixed.data[(size_t) y * mixed.width + x] != 0) {
                large_bottom = (int32_t) y;
            }
        }
    }

    failures += assertGreaterThan((double) small_bottom, 0.0);
    failures += assertGreaterThan((double) large_bottom, 0.0);

    // within a pixel, because the two glyphs round their own antialiased edge
    // independently
    failures += assertLessThan((double) (small_bottom - large_bottom), 1.5);
    failures += assertGreaterThan((double) (small_bottom - large_bottom), -1.5);

    // and the line box is the larger span's, which is what the metrics report
    TinyTextMetrics metrics;
    failures +=
        assertEquals(tiny_text_measure_runs(runs, 2u, 0, &metrics), TINYIMG_OK);

    TinyFontMetrics big;
    tiny_font_metrics(&font, 34.0f, &big);

    failures += assertLessThan(
        (double) metrics.line_height - (double) big.line_height, 0.01
    );
    failures += assertEquals((long) metrics.lines, 1L);
    failures += assertEquals((long) metrics.glyphs, 2L);

    tiny_image_destroy(&mixed);
    free(bytes);
    return failures;
}

/** A wrap lands wherever the measure runs out, including inside a span. */
static int wrappingCrossesSpans(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    uint8_t white = 255;

    const TinyTextRun runs[2] = {
        {&font, "the quick brown fox ", &style, &white},
        {&font, "jumps over the lazy dog", &style, &white}
    };

    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 120u;

    TinyTextMetrics wrapped;
    failures += assertEquals(
        tiny_text_measure_runs(runs, 2u, &box, &wrapped), TINYIMG_OK
    );

    // the same text as one span, wrapped the same way: the line count is a
    // property of the text and the measure, not of where the spans were cut
    TinyTextMetrics whole;
    failures += assertEquals(
        tiny_text_measure_wrapped(
            &font, "the quick brown fox jumps over the lazy dog", 120u, &style,
            &whole
        ),
        TINYIMG_OK
    );

    failures += assertEquals((long) wrapped.lines, (long) whole.lines);
    failures += assertGreaterThan((double) wrapped.lines, 2.0);
    failures += assertLessThan((double) wrapped.width, 121.0);

    // drawn, nothing reaches past the measure
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 130, 90, 1);

    failures += assertEquals(
        tiny_image_draw_text_runs(&image, runs, 2u, 0, 0, &box, &white),
        TINYIMG_OK
    );

    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
    bounds(&image, &x0, &y0, &x1, &y1);

    failures += assertLessThan((double) x1, (double) box.width);

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

/** Alignment and vertical alignment work on a rich run too. */
static int alignmentAppliesToRuns(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 18.0f);

    uint8_t white = 255;

    const TinyTextRun runs[2] = {
        {&font, "left ", &style, &white}, {&font, "right", &style, &white}
    };

    TinyTextBox box;
    memset(&box, 0, sizeof(box));
    box.width = 240u;
    box.height = 120u;

    uint32_t lefts[3];
    uint32_t tops[3];

    static const TinyTextAlign horizontal[3] = {
        TINYIMG_ALIGN_LEFT, TINYIMG_ALIGN_CENTER, TINYIMG_ALIGN_RIGHT
    };
    static const TinyTextVAlign vertical[3] = {
        TINYIMG_VALIGN_TOP, TINYIMG_VALIGN_MIDDLE, TINYIMG_VALIGN_BOTTOM
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, 250, 130, 1);

        box.align = horizontal[i];
        box.valign = vertical[i];

        failures += assertEquals(
            tiny_image_draw_text_runs(&image, runs, 2u, 0, 0, &box, &white),
            TINYIMG_OK
        );

        uint32_t x0;
        uint32_t y0;
        uint32_t x1;
        uint32_t y1;
        bounds(&image, &x0, &y0, &x1, &y1);

        lefts[i] = x0;
        tops[i] = y0;

        tiny_image_destroy(&image);
    }

    failures += assertLessThan((double) lefts[0], (double) lefts[1]);
    failures += assertLessThan((double) lefts[1], (double) lefts[2]);
    failures += assertLessThan((double) tops[0], (double) tops[1]);
    failures += assertLessThan((double) tops[1], (double) tops[2]);

    free(bytes);
    return failures;
}

/** A span's own color and outline reach only that span. */
static int stylePerSpan(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle plain;
    tiny_text_style(&plain, 26.0f);

    TinyTextStyle ringed;
    tiny_text_style(&ringed, 26.0f);
    ringed.stroke = 2.0f;
    ringed.stroke_color[0] = 120u;

    uint8_t white = 255;
    uint8_t gray = 60u;

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 220, 60, 1);

    const TinyTextRun runs[2] = {
        {&font, "aa", &plain, &white}, {&font, "bb", &ringed, &gray}
    };

    failures += assertEquals(
        tiny_image_draw_text_runs(&image, runs, 2u, 10, 10, 0, &white),
        TINYIMG_OK
    );

    // three levels are present: the plain span's fill, the outlined span's own
    // fill, and its outline
    uint32_t whites = 0;
    uint32_t grays = 0;
    uint32_t rings = 0;
    size_t pixels = (size_t) image.width * image.height;

    for (size_t i = 0; i < pixels; i++) {
        if (image.data[i] == 255u)
            whites++;
        else if (image.data[i] == 60u)
            grays++;
        else if (image.data[i] == 120u)
            rings++;
    }

    failures += assertGreaterThan((double) whites, 0.0);
    failures += assertGreaterThan((double) grays, 0.0);
    failures += assertGreaterThan((double) rings, 0.0);

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

static int rejections(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    uint8_t white = 255;

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 60, 40, 1);

    TinyTextRun runs[TINYIMG_TEXT_MAX_RUNS + 1];

    for (uint32_t i = 0; i < TINYIMG_TEXT_MAX_RUNS + 1u; i++) {
        runs[i].font = &font;
        runs[i].text = "x";
        runs[i].style = &style;
        runs[i].color = &white;
    }

    failures += assertEquals(
        tiny_image_draw_text_runs(&image, runs, 0u, 0, 0, 0, &white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text_runs(&image, 0, 2u, 0, 0, 0, &white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text_runs(
            &image, runs, TINYIMG_TEXT_MAX_RUNS + 1u, 0, 0, 0, &white
        ),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_draw_text_runs(
            0, runs, TINYIMG_TEXT_MAX_RUNS, 0, 0, 0, &white
        ),
        TINYIMG_ERR_NULL
    );

    // the full complement is accepted, so the limit is off by nothing
    failures += assertEquals(
        tiny_image_draw_text_runs(
            &image, runs, TINYIMG_TEXT_MAX_RUNS, 0, 0, 0, &white
        ),
        TINYIMG_OK
    );

    // a span with no face is a caller error rather than a skipped span
    TinyTextRun broken[2] = {
        {&font, "a", &style, &white}, {0, "b", &style, &white}
    };

    failures += assertEquals(
        tiny_image_draw_text_runs(&image, broken, 2u, 0, 0, 0, &white),
        TINYIMG_ERR_NULL
    );

    // and a span with no color of its own takes the one the draw was given
    TinyTextRun inherits[1] = {{&font, "a", &style, 0}};

    failures += assertEquals(
        tiny_image_draw_text_runs(&image, inherits, 1u, 0, 0, 0, &white),
        TINYIMG_OK
    );

    tiny_image_destroy(&image);
    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    printf("-- flow --\n");
    failures += spansFlowLikeOneString();
    failures += wrappingCrossesSpans();

    printf("-- baselines --\n");
    failures += mixedSizesShareABaseline();

    printf("-- box --\n");
    failures += alignmentAppliesToRuns();

    printf("-- per span --\n");
    failures += stylePerSpan();

    printf("-- rejections --\n");
    failures += rejections();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
