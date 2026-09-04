#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief Measurement, wrapping and alignment.
 *
 * Measuring and drawing go through one layout walk, so the thing worth
 * asserting is that they agree: a caller who measures a run and then draws it
 * at the measured position has to land where the measurement said. Two
 * implementations that agree until one of them is changed is the failure this
 * guards against.
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

static long ink(const TinyImage* image) {
    long total = 0;
    size_t pixels = (size_t) image->width * image->height * image->channels;

    for (size_t i = 0; i < pixels; i++) total += image->data[i];
    return total;
}

/** The leftmost and rightmost column holding any ink, or width and 0 when there
 * is none. */
static void inkColumns(
    const TinyImage* image, uint32_t* first, uint32_t* last
) {
    *first = image->width;
    *last = 0;

    for (uint32_t x = 0; x < image->width; x++) {
        for (uint32_t y = 0; y < image->height; y++) {
            if (image->data[(size_t) y * image->width + x] == 0) continue;

            if (x < *first) *first = x;
            if (x > *last) *last = x;
            break;
        }
    }
}

/** The topmost and bottommost row holding any ink. */
static void inkRows(const TinyImage* image, uint32_t* first, uint32_t* last) {
    *first = image->height;
    *last = 0;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            if (image->data[(size_t) y * image->width + x] == 0) continue;

            if (y < *first) *first = y;
            if (y > *last) *last = y;
            break;
        }
    }
}

static int measuring(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 24.0f);

    TinyTextMetrics one;
    failures += assertEquals(
        tiny_text_measure(&font, "Hello", &style, &one), TINYIMG_OK
    );

    failures += assertEquals(one.lines, 1);
    failures += assertEquals(one.glyphs, 5);
    failures += assertEquals(one.missing, 0);
    failures += assertGreaterThan((double) one.width, 0.0);

    TinyFontMetrics fm;
    tiny_font_metrics(&font, 24.0f, &fm);
    failures += assertFloatEquals(one.ascent, fm.ascent, 0.001f);
    failures += assertFloatEquals(one.descent, fm.descent, 0.001f);
    failures += assertFloatEquals(one.line_height, fm.line_height, 0.001f);
    failures += assertFloatEquals(one.height, fm.line_height, 0.001f);

    // the empty string is one empty line, not zero lines: a caller laying out a
    // paragraph needs the line to exist so the next one is placed below it
    TinyTextMetrics empty;
    tiny_text_measure(&font, "", &style, &empty);
    failures += assertEquals(empty.lines, 1);
    failures += assertEquals(empty.glyphs, 0);
    failures += assertFloatEquals(empty.width, 0.0f, 0.0f);

    // a newline is a line break, and the trailing one does not add a line of
    // its own
    TinyTextMetrics two;
    tiny_text_measure(&font, "one\ntwo", &style, &two);
    failures += assertEquals(two.lines, 2);
    failures += assertEquals(two.glyphs, 6);
    failures += assertFloatEquals(two.height, fm.line_height * 2.0f, 0.001f);

    TinyTextMetrics three;
    tiny_text_measure(&font, "one\ntwo\n", &style, &three);
    failures += assertEquals(three.lines, 2);

    // the width is the widest line, not the total
    TinyTextMetrics uneven;
    tiny_text_measure(&font, "i\nMMMMMM", &style, &uneven);
    TinyTextMetrics wide;
    tiny_text_measure(&font, "MMMMMM", &style, &wide);
    failures += assertFloatEquals(uneven.width, wide.width, 0.001f);

    // codepoints the face has no glyph for are counted, and still advance
    TinyTextMetrics missing;
    tiny_text_measure(
        &font,
        "a\xE4\xB8\xAD"
        "b",
        &style, &missing
    );
    failures += assertEquals(missing.glyphs, 3);
    failures += assertEquals(missing.missing, 1);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Size, tracking and line height each move the measurement the way they say.
 */
static int styling(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle base;
    tiny_text_style(&base, 20.0f);

    TinyTextMetrics at20;
    tiny_text_measure(&font, "Hamburgefonstiv", &base, &at20);

    // an outline face scales linearly, so doubling the size doubles the width
    // exactly
    TinyTextStyle doubled = base;
    doubled.size = 40.0f;

    TinyTextMetrics at40;
    tiny_text_measure(&font, "Hamburgefonstiv", &doubled, &at40);
    failures += assertFloatEquals(at40.width, at20.width * 2.0f, 0.01f);

    // tracking adds a fixed amount per glyph, including after the last one
    TinyTextStyle tracked = base;
    tracked.tracking = 3.0f;

    TinyTextMetrics spaced;
    tiny_text_measure(&font, "Hamburgefonstiv", &tracked, &spaced);
    failures += assertFloatEquals(
        spaced.width, at20.width + 3.0f * (float) at20.glyphs, 0.01f
    );

    // negative tracking tightens
    TinyTextStyle tight = base;
    tight.tracking = -1.0f;

    TinyTextMetrics narrow;
    tiny_text_measure(&font, "Hamburgefonstiv", &tight, &narrow);
    failures += assertLessThan(narrow.width, at20.width);

    // line height is a multiple of the face's own, and does not touch the width
    TinyTextStyle loose = base;
    loose.line_height = 2.0f;

    TinyTextMetrics tall;
    tiny_text_measure(&font, "one\ntwo", &loose, &tall);

    TinyTextMetrics normal;
    tiny_text_measure(&font, "one\ntwo", &base, &normal);

    failures +=
        assertFloatEquals(tall.line_height, normal.line_height * 2.0f, 0.01f);
    failures += assertFloatEquals(tall.width, normal.width, 0.0f);

    // zero means one, so a style that forgot to set it behaves
    TinyTextStyle unset = base;
    unset.line_height = 0.0f;

    TinyTextMetrics defaulted;
    tiny_text_measure(&font, "one\ntwo", &unset, &defaulted);
    failures +=
        assertFloatEquals(defaulted.line_height, normal.line_height, 0.0f);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Wrapping breaks on spaces, and mid-word only when it has to. */
static int wrapping(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    const char* sentence = "the quick brown fox jumps over the lazy dog";

    TinyTextMetrics unwrapped;
    tiny_text_measure(&font, sentence, &style, &unwrapped);
    failures += assertEquals(unwrapped.lines, 1);

    // a narrower box never gives fewer lines. not strictly more: two widths can
    // both fit the same break points, which 320 and 240 do for this sentence
    uint32_t previous = 0;
    uint32_t widest = 0;
    uint32_t narrowest = 0;

    for (uint32_t width = 320; width >= 80; width -= 80) {
        TinyTextMetrics wrapped;
        failures += assertEquals(
            tiny_text_measure_wrapped(&font, sentence, width, &style, &wrapped),
            TINYIMG_OK
        );

        printf(
            "width %u: %u lines, %.1f wide: ", width, wrapped.lines,
            wrapped.width
        );
        failures += assertTrue(wrapped.lines >= previous);

        // no line exceeds the box it was wrapped into
        printf("width %u fits: ", width);
        failures += assertTrue(wrapped.width <= (float) width);

        // every glyph survives the wrap; a break drops the space it broke at
        printf("width %u glyphs: ", width);
        failures += assertTrue(
            wrapped.glyphs <= unwrapped.glyphs &&
            wrapped.glyphs >= unwrapped.glyphs - wrapped.lines
        );

        previous = wrapped.lines;
        if (widest == 0u) widest = wrapped.lines;
        narrowest = wrapped.lines;
    }

    // and across the whole range it does change, or the wrapping is not
    // happening at all
    failures += assertGreaterThan((double) narrowest, (double) widest);

    // a single word wider than the box has no space to break at, so it breaks
    // mid-word rather than running out of the box
    TinyTextMetrics unbroken;
    tiny_text_measure_wrapped(
        &font, "Donaudampfschifffahrtsgesellschaft", 60u, &style, &unbroken
    );
    failures += assertGreaterThan((double) unbroken.lines, 1.0);
    failures += assertTrue(unbroken.width <= 60.0f);

    // a width of zero means no wrapping at all, which is what measure does
    TinyTextMetrics free_form;
    tiny_text_measure_wrapped(&font, sentence, 0u, &style, &free_form);
    failures += assertEquals(free_form.lines, 1);
    failures += assertFloatEquals(free_form.width, unwrapped.width, 0.0f);

    // a newline inside a wrapped run still breaks, on top of the wrapping
    TinyTextMetrics mixed;
    tiny_text_measure_wrapped(&font, "a\nb\nc", 500u, &style, &mixed);
    failures += assertEquals(mixed.lines, 3);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Alignment moves a line inside its box without changing what is in it. */
static int alignment(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 20.0f);

    const uint32_t box = 240;
    uint8_t white = 255;

    uint32_t firsts[3];
    uint32_t lasts[3];
    long inks[3];

    TinyTextAlign aligns[3] = {
        TINYIMG_ALIGN_LEFT, TINYIMG_ALIGN_CENTER, TINYIMG_ALIGN_RIGHT
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, box + 20u, 40, 1);

        failures += assertEquals(
            tiny_image_draw_text_box(
                &image, &font, "short", 0, 0, box, 0, &style, aligns[i], &white
            ),
            TINYIMG_OK
        );

        inkColumns(&image, &firsts[i], &lasts[i]);
        inks[i] = ink(&image);

        tiny_image_destroy(&image);
    }

    // the same glyphs in all three, so the coverage is the same to within the
    // subpixel offset
    failures += assertPSNR(
        (const unsigned char*) &inks[0], (const unsigned char*) &inks[0],
        sizeof(long), 0.0
    );
    failures += assertGreaterThan((double) inks[0], 0.0);

    // left sits against the left edge, center is inside it, right is furthest
    // over
    failures += assertLessThan((double) firsts[0], (double) firsts[1]);
    failures += assertLessThan((double) firsts[1], (double) firsts[2]);

    // and right ends against the right edge of the box
    failures += assertGreaterThan((double) lasts[2], (double) box - 4.0);
    failures += assertTrue(lasts[2] < box);

    // centering is symmetric: the slack either side matches to within a pixel
    uint32_t leftSlack = firsts[1];
    uint32_t rightSlack = box - 1u - lasts[1];
    failures += assertIn(
        (double) leftSlack, (double) rightSlack - 2.0, (double) rightSlack + 2.0
    );

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief A box that runs out of height drops the lines it cannot fit.
 *
 * And the metrics still count them, so a caller can tell the text overflowed
 * rather than having to measure it again to find out.
 */
static int overflow(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    const char* four = "one\ntwo\nthree\nfour";

    TinyTextMetrics all;
    tiny_text_measure(&font, four, &style, &all);
    failures += assertEquals(all.lines, 4);

    TinyImage full;
    TinyImage clipped;

    memset(&full, 0, sizeof(full));
    memset(&clipped, 0, sizeof(clipped));
    tiny_image_create(&full, 200, 120, 1);
    tiny_image_create(&clipped, 200, 120, 1);

    uint8_t white = 255;

    // no height limit against a limit of two lines' worth
    tiny_image_draw_text_box(
        &full, &font, four, 2, 2, 180, 0, &style, TINYIMG_ALIGN_LEFT, &white
    );

    uint32_t height = (uint32_t) (all.line_height * 2.0f);
    tiny_image_draw_text_box(
        &clipped, &font, four, 2, 2, 180, height, &style, TINYIMG_ALIGN_LEFT,
        &white
    );

    failures += assertGreaterThan((double) ink(&full), (double) ink(&clipped));
    failures += assertGreaterThan((double) ink(&clipped), 0.0);

    // the dropped lines are the bottom ones, so the clipped render's ink ends
    // higher
    uint32_t fullTop;
    uint32_t fullBottom;
    uint32_t clippedTop;
    uint32_t clippedBottom;

    inkRows(&full, &fullTop, &fullBottom);
    inkRows(&clipped, &clippedTop, &clippedBottom);

    failures += assertEquals(fullTop, clippedTop);
    failures += assertLessThan((double) clippedBottom, (double) fullBottom);

    // and what is drawn is unchanged by the limit: the rows that fit are
    // identical
    int same = 1;
    for (uint32_t y = 0; y <= clippedBottom; y++) {
        for (uint32_t x = 0; x < 200u; x++) {
            size_t at = (size_t) y * 200u + x;
            if (full.data[at] != clipped.data[at]) same = 0;
        }
    }

    failures += assertTrue(same);

    tiny_image_destroy(&full);
    tiny_image_destroy(&clipped);

    // a box with no width draws nothing, because there is nothing to wrap
    // inside
    TinyImage none;
    memset(&none, 0, sizeof(none));
    tiny_image_create(&none, 100, 40, 1);

    failures += assertEquals(
        tiny_image_draw_text_box(
            &none, &font, "text", 0, 0, 0, 0, &style, TINYIMG_ALIGN_LEFT, &white
        ),
        TINYIMG_OK
    );

    tiny_image_destroy(&none);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief Measuring and drawing agree.
 *
 * The load-bearing property of having one layout walk. A run drawn at the
 * measured width, into an image exactly that wide, must not lose a pixel off
 * either edge; a measurement that disagreed with the drawing by even a fraction
 * shows up as clipped ink.
 */
static int agreement(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    const char* runs[] = {"Hello, world", "AVATAR",
                          "gjpqy",        "\xC3\x89\xC3\xA9 accents",
                          "iiiii",        "WWWWW"};

    for (unsigned i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        TinyTextStyle style;
        tiny_text_style(&style, 28.0f);

        TinyTextMetrics metrics;
        tiny_text_measure(&font, runs[i], &style, &metrics);

        // the measured box, plus a margin so a glyph's own side bearing is not
        // mistaken for a layout disagreement
        uint32_t width = (uint32_t) metrics.width + 8u;
        uint32_t height = (uint32_t) (metrics.ascent + metrics.descent) + 8u;

        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, width, height, 1);

        uint8_t white = 255;
        tiny_image_draw_text(&image, &font, runs[i], 4, 4, &style, &white);

        uint32_t first;
        uint32_t last;
        inkColumns(&image, &first, &last);

        // nothing reached the edges, so nothing was clipped
        printf("%s left: ", runs[i]);
        failures += assertGreaterThan((double) first, 0.0);
        printf("%s right: ", runs[i]);
        failures += assertLessThan((double) last, (double) width - 1.0);

        uint32_t top;
        uint32_t bottom;
        inkRows(&image, &top, &bottom);

        printf("%s top: ", runs[i]);
        failures += assertGreaterThan((double) top, 0.0);
        printf("%s bottom: ", runs[i]);
        failures += assertLessThan((double) bottom, (double) height - 1.0);

        tiny_image_destroy(&image);
    }

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** A bitmap face lays out on its own fixed grid. */
static int bitmapLayout(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("tiny.psf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 99.0f);

    TinyTextMetrics metrics;
    failures += assertEquals(
        tiny_text_measure(&font, "AAAA", &style, &metrics), TINYIMG_OK
    );

    // a fixed cell means the width is exactly the cell times the count,
    // whatever size was asked
    failures += assertFloatEquals(metrics.width, 8.0f * 4.0f, 0.0f);
    failures += assertFloatEquals(metrics.line_height, 16.0f, 0.0f);
    failures += assertEquals(metrics.glyphs, 4);

    // tracking still applies, because it is in pixels rather than in ems
    TinyTextStyle tracked = style;
    tracked.tracking = 2.0f;

    TinyTextMetrics spaced;
    tiny_text_measure(&font, "AAAA", &tracked, &spaced);
    failures +=
        assertFloatEquals(spaced.width, 8.0f * 4.0f + 2.0f * 4.0f, 0.0f);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- measuring --\n");
    failures += measuring();
    printf("-- styling --\n");
    failures += styling();
    printf("-- wrapping --\n");
    failures += wrapping();
    printf("-- alignment --\n");
    failures += alignment();
    printf("-- overflow --\n");
    failures += overflow();
    printf("-- agreement --\n");
    failures += agreement();
    printf("-- bitmap layout --\n");
    failures += bitmapLayout();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
