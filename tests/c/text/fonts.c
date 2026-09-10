#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief Fallback chains and GPOS kerning.
 *
 * Both exist because a face is not one thing. A subset face covers what it was
 * cut down to and nothing else, so a chain is how a caller adds a script
 * without replacing what they had; and a modern face puts its kerning in GPOS
 * and often carries no `kern` table at all, so a reader that only knows the
 * legacy table reads most fonts as unkerned.
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

/**
 * A chain picks the first face that covers each codepoint.
 *
 * The bitmap face is the second link, because the two are unmistakably
 * different: the outline face has no glyph for the PSF face's high codepoints
 * and the widths do not match, so a chain that silently used the wrong one
 * measures wrong.
 */
static int chainPicksThePerCodepointFace(void) {
    int failures = 0;

    TinyFont latin;
    TinyFont bitmap;
    size_t latin_size = 0;
    size_t bitmap_size = 0;

    uint8_t* latin_bytes = loadFont("dejavu-latin.ttf", &latin, &latin_size);
    uint8_t* bitmap_bytes = loadFont("tiny.psf", &bitmap, &bitmap_size);

    if (!latin_bytes || !bitmap_bytes) {
        free(latin_bytes);
        free(bitmap_bytes);
        return 1;
    }

    TinyFontSet set;
    failures += assertEquals(tiny_font_set_init(&set), TINYIMG_OK);
    failures += assertEquals((long) set.count, 0L);
    failures += assertEquals(tiny_font_set_add(&set, &latin), TINYIMG_OK);
    failures += assertEquals(tiny_font_set_add(&set, &bitmap), TINYIMG_OK);
    failures += assertEquals((long) set.count, 2L);

    // the outline face has 'A'; only the bitmap face has codepoint 0x7F
    failures += assertTrue(tiny_font_set_for(&set, 'A') == &latin);
    failures += assertTrue(tiny_font_set_for(&set, 0x7Fu) == &bitmap);

    // and a codepoint neither face has falls back to the first, so there is
    // always something to draw
    failures += assertTrue(tiny_font_set_for(&set, 0x4E2Du) == &latin);

    // the chain limit is enforced rather than overwriting a link
    TinyFontSet full;
    tiny_font_set_init(&full);

    for (uint32_t i = 0; i < TINYIMG_FONT_MAX_FALLBACK; i++) {
        failures += assertEquals(tiny_font_set_add(&full, &latin), TINYIMG_OK);
    }

    failures +=
        assertEquals(tiny_font_set_add(&full, &latin), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_font_set_init(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_font_set_add(&full, 0), TINYIMG_ERR_NULL);
    failures += assertNull(tiny_font_set_for(0, 'A'));

    free(latin_bytes);
    free(bitmap_bytes);
    return failures;
}

/**
 * A chain of one face is the single-face path, byte for byte.
 *
 * The check that a fallback chain costs a caller who does not need one nothing:
 * if the chain took a different route through the layout, this is where it
 * would show.
 */
static int chainOfOneIsUnchanged(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 22.0f);

    uint8_t white = 255;

    TinyImage direct;
    TinyImage chained;
    memset(&direct, 0, sizeof(direct));
    memset(&chained, 0, sizeof(chained));

    tiny_image_create(&direct, 240, 60, 1);
    tiny_image_create(&chained, 240, 60, 1);

    TinyFontSet set;
    tiny_font_set_init(&set);
    tiny_font_set_add(&set, &font);

    failures += assertEquals(
        tiny_image_draw_text(&direct, &font, "AVeToWa", 6, 6, &style, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text_set(
            &chained, &set, "AVeToWa", 6, 6, 0, &style, &white
        ),
        TINYIMG_OK
    );

    failures += assertEquals(
        memcmp(
            direct.data, chained.data, (size_t) direct.width * direct.height
        ),
        0
    );

    TinyTextMetrics one;
    TinyTextMetrics via;

    failures += assertEquals(
        tiny_text_measure(&font, "AVeToWa", &style, &one), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_text_measure_set(&set, "AVeToWa", 0, &style, &via), TINYIMG_OK
    );

    failures += assertLessThan((double) (via.width - one.width), 0.01);
    failures += assertGreaterThan((double) (via.width - one.width), -0.01);
    failures += assertEquals((long) via.glyphs, (long) one.glyphs);
    failures += assertEquals((long) via.missing, (long) one.missing);

    tiny_image_destroy(&direct);
    tiny_image_destroy(&chained);
    free(bytes);
    return failures;
}

/**
 * A codepoint the first face lacks is counted as covered when a later one has
 * it.
 *
 * `missing` is what a caller checks before serving a card with boxes in the
 * headline, so it has to mean "no face in the chain has this" and not "the
 * first one does not".
 */
static int chainAnswersWhatIsMissing(void) {
    int failures = 0;

    TinyFont latin;
    TinyFont bitmap;
    size_t latin_size = 0;
    size_t bitmap_size = 0;

    uint8_t* latin_bytes = loadFont("dejavu-latin.ttf", &latin, &latin_size);
    uint8_t* bitmap_bytes = loadFont("tiny.psf", &bitmap, &bitmap_size);

    if (!latin_bytes || !bitmap_bytes) {
        free(latin_bytes);
        free(bitmap_bytes);
        return 1;
    }

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    // 0x7F is in the bitmap face and not in the Latin subset
    static const char* text = "a\x7F";

    TinyTextMetrics alone;
    failures += assertEquals(
        tiny_text_measure(&latin, text, &style, &alone), TINYIMG_OK
    );
    failures += assertEquals((long) alone.missing, 1L);

    TinyFontSet set;
    tiny_font_set_init(&set);
    tiny_font_set_add(&set, &latin);
    tiny_font_set_add(&set, &bitmap);

    TinyTextMetrics covered;
    failures += assertEquals(
        tiny_text_measure_set(&set, text, 0, &style, &covered), TINYIMG_OK
    );
    failures += assertEquals((long) covered.missing, 0L);

    /*
     * The second face contributes its own advance, so the run is a different
     * width from the one that drew a missing glyph box.
     *
     * Not wider, and the first attempt at this assertion said wider: the
     * fallback here is a bitmap face with an eight pixel cell and the Latin
     * face's notdef box is broader than that, so covering the codepoint made
     * the run **narrower**. Which direction it moves is a property of the two
     * faces; that it moves at all is the property of the chain.
     */
    failures += assertTrue(covered.width != alone.width);

    // and the line box is still the first face's, because a line's height
    // cannot depend on which characters are on it
    TinyFontMetrics primary;
    tiny_font_metrics(&latin, 16.0f, &primary);

    failures += assertLessThan(
        (double) (covered.line_height - primary.line_height), 0.01
    );

    // drawn, the fallback glyph puts ink down rather than being skipped
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 80, 40, 1);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text_set(&image, &set, text, 4, 4, 0, &style, &white),
        TINYIMG_OK
    );

    long ink = 0;
    size_t pixels = (size_t) image.width * image.height;
    for (size_t i = 0; i < pixels; i++) ink += image.data[i];

    failures += assertGreaterThan((double) ink, 0.0);

    tiny_image_destroy(&image);
    free(latin_bytes);
    free(bitmap_bytes);
    return failures;
}

/**
 * GPOS kerning is read, in both pair formats.
 *
 * The fixture carries -240 units on `AV` through format 1 and -180 on `To`
 * through format 2, both an order of magnitude larger than a real kern pair, so
 * the measured difference cannot be anything else. The same face without the
 * GPOS table is the control.
 */
static int gposKerningIsApplied(void) {
    int failures = 0;

    TinyFont plain;
    TinyFont kerned;
    size_t plain_size = 0;
    size_t kerned_size = 0;

    uint8_t* plain_bytes = loadFont("dejavu-latin.ttf", &plain, &plain_size);
    uint8_t* kerned_bytes = loadFont("dejavu-gpos.ttf", &kerned, &kerned_size);

    if (!plain_bytes || !kerned_bytes) {
        free(plain_bytes);
        free(kerned_bytes);
        return 1;
    }

    // the control has no GPOS at all and the fixture does
    failures += assertEquals((long) plain.gpos_kern, 0L);
    failures += assertGreaterThan((double) kerned.gpos_kern, 0.0);
    failures += assertEquals((long) kerned.gpos_subtables, 2L);

    TinyTextStyle on;
    TinyTextStyle off;
    tiny_text_style(&on, 100.0f);
    tiny_text_style(&off, 100.0f);
    off.kerning = 0u;

    // 100 pixels per em over 2048 units per em
    float per_unit = 100.0f / (float) kerned.units_per_em;

    static const char* pairs[2] = {"AV", "To"};
    static const float wanted[2] = {-240.0f, -180.0f};

    /*
     * The control is the same face with kerning off, not the face without GPOS.
     *
     * The first attempt used the plain face as the control and could not agree
     * with the fixture: `dejavu-latin.ttf` has a `kern` table that already
     * kerns both of these pairs, by -131 and -348 units, so the difference
     * between the two faces was the difference between two kerns rather than
     * the GPOS value. Turning kerning off on one face is the control that
     * isolates it.
     */
    for (uint32_t i = 0; i < 2u; i++) {
        TinyTextMetrics kerned_on;
        TinyTextMetrics kerned_off;

        failures += assertEquals(
            tiny_text_measure(&kerned, pairs[i], &on, &kerned_on), TINYIMG_OK
        );
        failures += assertEquals(
            tiny_text_measure(&kerned, pairs[i], &off, &kerned_off), TINYIMG_OK
        );

        float moved = kerned_on.width - kerned_off.width;
        float expected = wanted[i] * per_unit;

        failures += assertLessThan((double) (moved - expected), 0.02);
        failures += assertGreaterThan((double) (moved - expected), -0.02);

        // and the legacy table on the control face moves the same pair by a
        // different amount, so the value above demonstrably came from GPOS
        TinyTextMetrics legacy_on;
        TinyTextMetrics legacy_off;

        failures += assertEquals(
            tiny_text_measure(&plain, pairs[i], &on, &legacy_on), TINYIMG_OK
        );
        failures += assertEquals(
            tiny_text_measure(&plain, pairs[i], &off, &legacy_off), TINYIMG_OK
        );

        float legacy = legacy_on.width - legacy_off.width;

        failures += assertLessThan((double) legacy, 0.0);
        failures += assertGreaterThan(
            (double) (legacy - expected) * (legacy - expected), 1.0
        );
    }

    // a pair no lookup names is not moved, so the coverage tests are doing
    // something rather than every pair reading the same record
    TinyTextMetrics quiet_on;
    TinyTextMetrics quiet_off;

    failures += assertEquals(
        tiny_text_measure(&kerned, "xy", &on, &quiet_on), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_text_measure(&kerned, "xy", &off, &quiet_off), TINYIMG_OK
    );
    failures +=
        assertLessThan((double) (quiet_on.width - quiet_off.width), 0.01);
    failures +=
        assertGreaterThan((double) (quiet_on.width - quiet_off.width), -0.01);

    free(plain_bytes);
    free(kerned_bytes);
    return failures;
}

/** Kerning does not cross a face boundary inside a chain. */
static int kerningStaysInsideOneFace(void) {
    int failures = 0;

    TinyFont kerned;
    TinyFont bitmap;
    size_t kerned_size = 0;
    size_t bitmap_size = 0;

    uint8_t* kerned_bytes = loadFont("dejavu-gpos.ttf", &kerned, &kerned_size);
    uint8_t* bitmap_bytes = loadFont("tiny.psf", &bitmap, &bitmap_size);

    if (!kerned_bytes || !bitmap_bytes) {
        free(kerned_bytes);
        free(bitmap_bytes);
        return 1;
    }

    TinyFontSet set;
    tiny_font_set_init(&set);
    tiny_font_set_add(&set, &kerned);
    tiny_font_set_add(&set, &bitmap);

    TinyTextStyle style;
    tiny_text_style(&style, 100.0f);

    // "AV" is a kern pair in the first face and both glyphs come from it, so it
    // kerns through the chain exactly as it does directly
    TinyTextMetrics together;
    failures += assertEquals(
        tiny_text_measure_set(&set, "AV", 0, &style, &together), TINYIMG_OK
    );

    TinyTextMetrics direct;
    failures += assertEquals(
        tiny_text_measure(&kerned, "AV", &style, &direct), TINYIMG_OK
    );

    failures += assertLessThan((double) (together.width - direct.width), 0.01);

    // with a codepoint from the other face between them, neither boundary is a
    // pair either face knows, so nothing is kerned and the run is wider than
    // the sum would be if a kern had leaked across
    TinyTextMetrics apart;
    failures += assertEquals(
        tiny_text_measure_set(&set, "A\x7FV", 0, &style, &apart), TINYIMG_OK
    );

    failures +=
        assertGreaterThan((double) apart.width, (double) together.width);

    free(kerned_bytes);
    free(bitmap_bytes);
    return failures;
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

/** Draws one glyph at one weight and reports its ink box and its advance. */
static int atWeight(TinyFont* font, float weight, uint32_t* box, float* width) {
    int failures = 0;

    failures += assertEquals(tiny_font_reset_axes(font), TINYIMG_OK);

    if (weight > 0.0f) {
        failures += assertEquals(
            tiny_font_set_axis(font, TINYIMG_AXIS_WEIGHT, weight), TINYIMG_OK
        );
    }

    TinyTextStyle style;
    tiny_text_style(&style, 100.0f);

    TinyTextMetrics metrics;
    failures += assertEquals(
        tiny_text_measure(font, "A", &style, &metrics), TINYIMG_OK
    );
    *width = metrics.width;

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 300, 200, 1);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text(&image, font, "A", 50, 40, &style, &white),
        TINYIMG_OK
    );

    bounds(&image, &box[0], &box[1], &box[2], &box[3]);
    tiny_image_destroy(&image);

    return failures;
}

/**
 * A variable face interpolates its outlines and its advances.
 *
 * The fixture's deltas are chosen so every number here is arithmetic rather
 * than a recording: at the top of the axis every point of `A` moves 100 font
 * units right and the advance grows 120, and a second region peaking at the
 * axis' midpoint raises the glyph 80 units and is back to nothing at the top.
 * At 2048 units per em and 100 pixels per em, 100 units is 4.88 pixels.
 */
static int variableOutlinesInterpolate(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-variable.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals((long) tiny_font_axis_count(&font), 1L);

    TinyFontAxis axis;
    failures += assertEquals(tiny_font_axis(&font, 0u, &axis), TINYIMG_OK);
    failures += assertEquals((long) axis.tag, (long) TINYIMG_AXIS_WEIGHT);
    failures += assertEquals((long) axis.min, 400L);
    failures += assertEquals((long) axis.def, 400L);
    failures += assertEquals((long) axis.max, 1000L);
    failures +=
        assertEquals(tiny_font_axis(&font, 1u, &axis), TINYIMG_ERR_BOUNDS);

    uint32_t light[4];
    uint32_t middle[4];
    uint32_t heavy[4];
    uint32_t quarter[4];
    float light_width = 0.0f;
    float middle_width = 0.0f;
    float heavy_width = 0.0f;
    float quarter_width = 0.0f;

    failures += atWeight(&font, 400.0f, light, &light_width);
    failures += atWeight(&font, 550.0f, quarter, &quarter_width);
    failures += atWeight(&font, 700.0f, middle, &middle_width);
    failures += atWeight(&font, 1000.0f, heavy, &heavy_width);

    // the advance grows by the advance delta scaled by the coordinate: 120
    // units is 5.86 pixels at the top of the axis, a quarter of it at 550
    failures += assertLessThan(
        (double) (heavy_width - light_width) - 120.0 * 100.0 / 2048.0, 0.02
    );
    failures += assertGreaterThan(
        (double) (heavy_width - light_width) - 120.0 * 100.0 / 2048.0, -0.02
    );
    failures += assertLessThan(
        (double) (quarter_width - light_width) - 30.0 * 100.0 / 2048.0, 0.02
    );

    // and the outline moves right by the point delta, which is 4.88 pixels at
    // the top and rounds to five whole pixels of ink
    failures += assertEquals((long) (heavy[0] - light[0]), 5L);
    failures += assertEquals((long) (heavy[2] - light[2]), 5L);
    failures += assertEquals((long) (quarter[0] - light[0]), 2L);

    /*
     * The intermediate region fires at its peak and nowhere near the ends.
     *
     * At weight 700 the coordinate is exactly the region's peak, so the glyph
     * rises 80 units, which is 3.9 pixels; at 1000 the coordinate is past the
     * region's end and the rise is gone. A reader that treated every tuple as
     * covering the whole axis would raise the glyph at 1000 as well.
     */
    failures += assertEquals((long) (light[1] - middle[1]), 4L);
    failures += assertEquals((long) (light[1] - heavy[1]), 0L);
    failures += assertEquals((long) (light[3] - heavy[3]), 0L);

    // a value past the axis is clamped rather than refused, and a tag the face
    // does not declare is a miss
    failures += assertEquals(
        tiny_font_set_axis(&font, TINYIMG_AXIS_WEIGHT, 5000.0f), TINYIMG_OK
    );
    failures += assertLessThan((double) font.coords[0] - 1.0, 0.001);
    failures += assertEquals(
        tiny_font_set_axis(&font, TINYIMG_AXIS_OPTICAL_SIZE, 12.0f),
        TINYIMG_ERR_NOT_FOUND
    );

    free(bytes);
    return failures;
}

/** A static face reports no axes and refuses to be set on one. */
static int staticFacesHaveNoAxes(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals((long) tiny_font_axis_count(&font), 0L);
    failures += assertEquals((long) font.fvar, 0L);
    failures += assertEquals(
        tiny_font_set_axis(&font, TINYIMG_AXIS_WEIGHT, 700.0f),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    TinyFontAxis axis;
    failures +=
        assertEquals(tiny_font_axis(&font, 0u, &axis), TINYIMG_ERR_BOUNDS);
    failures += assertEquals((long) tiny_font_axis_count(0), 0L);
    failures += assertEquals(tiny_font_reset_axes(0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_font_set_axis(0, TINYIMG_AXIS_WEIGHT, 1.0f), TINYIMG_ERR_NULL
    );

    free(bytes);
    return failures;
}

/**
 * The segment map is applied, so the axis' midpoint is not its normalized
 * midpoint.
 *
 * Two faces with the same axis and the same deltas, one with an `avar` mapping
 * the axis' halfway point to three quarters of the way along. At weight 700 the
 * mapped face therefore has to be further along than the plain one, and both
 * ends have to agree because the map pins them.
 */
static int avarRemapsTheAxis(void) {
    int failures = 0;
    size_t plain_size = 0;
    size_t mapped_size = 0;

    TinyFont plain;
    TinyFont mapped;

    uint8_t* plain_bytes = loadFont("dejavu-variable.ttf", &plain, &plain_size);
    uint8_t* mapped_bytes = loadFont("dejavu-avar.ttf", &mapped, &mapped_size);

    if (!plain_bytes || !mapped_bytes) {
        free(plain_bytes);
        free(mapped_bytes);
        return 1;
    }

    failures += assertEquals((long) plain.avar, 0L);
    failures += assertGreaterThan((double) mapped.avar, 0.0);

    tiny_font_set_axis(&plain, TINYIMG_AXIS_WEIGHT, 700.0f);
    tiny_font_set_axis(&mapped, TINYIMG_AXIS_WEIGHT, 700.0f);

    failures += assertLessThan((double) plain.coords[0] - 0.5, 0.001);
    failures += assertLessThan((double) mapped.coords[0] - 0.75, 0.001);
    failures += assertGreaterThan((double) mapped.coords[0] - 0.75, -0.001);

    // the ends are pinned, so the two faces agree there
    tiny_font_set_axis(&plain, TINYIMG_AXIS_WEIGHT, 1000.0f);
    tiny_font_set_axis(&mapped, TINYIMG_AXIS_WEIGHT, 1000.0f);

    failures +=
        assertLessThan((double) (plain.coords[0] - mapped.coords[0]), 0.001);

    free(plain_bytes);
    free(mapped_bytes);
    return failures;
}

/**
 * A tuple that names one point per contour moves the whole contour.
 *
 * The one subset whose inferred result is computable without walking the
 * outline, and the reason it is the check: a reader that left the unnamed
 * points where they were would tear `A` open rather than move it, and the ink
 * box would grow instead of shifting.
 */
static int inferredPointsMoveWithTheirContour(void) {
    int failures = 0;
    size_t all_size = 0;
    size_t some_size = 0;

    TinyFont all;
    TinyFont some;

    uint8_t* all_bytes = loadFont("dejavu-variable.ttf", &all, &all_size);
    uint8_t* some_bytes = loadFont("dejavu-iup.ttf", &some, &some_size);

    if (!all_bytes || !some_bytes) {
        free(all_bytes);
        free(some_bytes);
        return 1;
    }

    uint32_t named[4];
    uint32_t inferred[4];
    float named_width = 0.0f;
    float inferred_width = 0.0f;

    failures += atWeight(&all, 1000.0f, named, &named_width);
    failures += atWeight(&some, 1000.0f, inferred, &inferred_width);

    for (uint32_t i = 0; i < 4u; i++) {
        failures += assertEquals((long) inferred[i], (long) named[i]);
    }

    // the phantom points are not named, so this face's advance does not vary;
    // its outline still moved, which is what separates the two deltas
    float base = 0.0f;
    uint32_t box[4];
    failures += atWeight(&some, 400.0f, box, &base);

    failures += assertLessThan((double) (inferred_width - base), 0.01);
    failures += assertGreaterThan((double) (named_width - base), 5.0);

    free(all_bytes);
    free(some_bytes);
    return failures;
}

/**
 * A ligature replaces a run of codepoints with one glyph, longest match first.
 *
 * The fixture maps `fi` onto `W` and `ffi` onto `M`, which is nonsense
 * typographically and exactly what makes it measurable: the replacements are
 * glyphs this face already has, so the expected width of a substituted run is
 * the width of the replacement drawn on its own. A reader that took the first
 * match rather than the longest would set `ffi` as `W` followed by `i`, and the
 * width says which happened.
 */
static int ligaturesSubstitute(void) {
    int failures = 0;
    size_t plain_size = 0;
    size_t liga_size = 0;

    TinyFont plain;
    TinyFont liga;

    uint8_t* plain_bytes = loadFont("dejavu-latin.ttf", &plain, &plain_size);
    uint8_t* liga_bytes = loadFont("dejavu-liga.ttf", &liga, &liga_size);

    if (!plain_bytes || !liga_bytes) {
        free(plain_bytes);
        free(liga_bytes);
        return 1;
    }

    failures += assertEquals((long) plain.gsub_liga, 0L);
    failures += assertGreaterThan((double) liga.gsub_liga, 0.0);

    TinyTextStyle style;
    tiny_text_style(&style, 100.0f);

    TinyTextMetrics w;
    TinyTextMetrics m;
    TinyTextMetrics fi;
    TinyTextMetrics ffi;
    TinyTextMetrics twice;
    TinyTextMetrics none;

    failures +=
        assertEquals(tiny_text_measure(&liga, "W", &style, &w), TINYIMG_OK);
    failures +=
        assertEquals(tiny_text_measure(&liga, "M", &style, &m), TINYIMG_OK);
    failures +=
        assertEquals(tiny_text_measure(&liga, "fi", &style, &fi), TINYIMG_OK);
    failures +=
        assertEquals(tiny_text_measure(&liga, "ffi", &style, &ffi), TINYIMG_OK);
    failures += assertEquals(
        tiny_text_measure(&liga, "fifi", &style, &twice), TINYIMG_OK
    );
    failures +=
        assertEquals(tiny_text_measure(&liga, "fx", &style, &none), TINYIMG_OK);

    // two codepoints becoming one glyph, at the replacement's own width
    failures += assertEquals((long) fi.glyphs, 1L);
    failures += assertLessThan((double) (fi.width - w.width), 0.01);
    failures += assertGreaterThan((double) (fi.width - w.width), -0.01);

    // three becoming one, which is the longest match rather than the first
    failures += assertEquals((long) ffi.glyphs, 1L);
    failures += assertLessThan((double) (ffi.width - m.width), 0.01);
    failures += assertGreaterThan((double) (ffi.width - m.width), -0.01);

    // and it applies wherever it matches, not only at the start of a run
    failures += assertEquals((long) twice.glyphs, 2L);
    failures += assertLessThan((double) (twice.width - 2.0 * w.width), 0.02);

    // a run the feature does not name is untouched
    TinyTextMetrics control;
    failures += assertEquals(
        tiny_text_measure(&plain, "fx", &style, &control), TINYIMG_OK
    );
    failures += assertLessThan((double) (none.width - control.width), 0.01);

    // the control face substitutes nothing at all, so its `fi` is two glyphs
    failures += assertEquals(
        tiny_text_measure(&plain, "fi", &style, &control), TINYIMG_OK
    );
    failures += assertEquals((long) control.glyphs, 2L);
    failures += assertGreaterThan(
        (double) (fi.width - control.width) * (fi.width - control.width), 1.0
    );

    // drawing agrees with measuring, which is what the shared walk is for: the
    // substituted run has to be the same pixels as the replacement glyph
    TinyImage substituted;
    TinyImage direct;
    memset(&substituted, 0, sizeof(substituted));
    memset(&direct, 0, sizeof(direct));

    tiny_image_create(&substituted, 200, 140, 1);
    tiny_image_create(&direct, 200, 140, 1);

    uint8_t white = 255;

    failures += assertEquals(
        tiny_image_draw_text(&substituted, &liga, "fi", 10, 10, &style, &white),
        TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_draw_text(&direct, &liga, "W", 10, 10, &style, &white),
        TINYIMG_OK
    );

    failures += assertEquals(
        memcmp(
            substituted.data, direct.data, (size_t) direct.width * direct.height
        ),
        0
    );

    tiny_image_destroy(&substituted);
    tiny_image_destroy(&direct);

    free(plain_bytes);
    free(liga_bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    printf("-- chains --\n");
    failures += chainPicksThePerCodepointFace();
    failures += chainOfOneIsUnchanged();
    failures += chainAnswersWhatIsMissing();

    printf("-- gpos --\n");
    failures += gposKerningIsApplied();
    failures += kerningStaysInsideOneFace();

    printf("-- gsub --\n");
    failures += ligaturesSubstitute();

    printf("-- variations --\n");
    failures += staticFacesHaveNoAxes();
    failures += variableOutlinesInterpolate();
    failures += avarRemapsTheAxis();
    failures += inferredPointsMoveWithTheirContour();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
