#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief What the rasterizer puts on the image.
 *
 * A rendered glyph is compared against properties rather than against a
 * reference bitmap. A stored bitmap would pin the antialiasing to whatever the
 * rasterizer did on the day it was written, so it would fail on any improvement
 * and pass on any regression that kept the same shape. What is asserted instead
 * is the things a broken rasterizer gets wrong: the ink lands inside the
 * glyph's own box, the coverage is antialiased rather than binary, a composite
 * carries its accent, the fill rule closes the counter of an `o`, and drawing
 * is idempotent in position.
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

/** Draws one string onto a fresh single channel image. */
static int render(
    TinyImage* image, const TinyFont* font, const char* text, float size,
    uint32_t width, uint32_t height
) {
    memset(image, 0, sizeof(*image));

    int result = tiny_image_create(image, width, height, 1);
    if (result != TINYIMG_OK) return result;

    TinyTextStyle style;
    tiny_text_style(&style, size);

    uint8_t white = 255;
    return tiny_image_draw_text(image, font, text, 4, 4, &style, &white);
}

/** Total coverage, which stands in for how much of the glyph was drawn. */
static long ink(const TinyImage* image) {
    long total = 0;
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) total += image->data[i];
    return total;
}

/** The bounding box of everything non-zero. */
static void inkBounds(
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

static int basics(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage image;
    failures +=
        assertEquals(render(&image, &font, "H", 40.0f, 60, 60), TINYIMG_OK);
    failures += assertGreaterThan((double) ink(&image), 0.0);

    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
    inkBounds(&image, &x0, &y0, &x1, &y1);

    // the pen is at x = 4 and the baseline at 4 + ascent, so an H sits between
    // the top of the line box and the baseline; ink outside that is the sign of
    // a wrong origin
    TinyFontMetrics fm;
    tiny_font_metrics(&font, 40.0f, &fm);

    failures += assertGreaterThan((double) x0, 3.0);
    failures += assertGreaterThan((double) y0, 3.0);
    failures += assertLessThan((double) y1, 4.0 + (double) fm.ascent + 1.0);

    // a capital H has no descender, so nothing may fall below the baseline
    failures += assertLessThan((double) y1, 4.0 + (double) fm.ascent);

    tiny_image_destroy(&image);

    // a lowercase g does descend, and that is what says the baseline is where
    // the metrics say
    failures +=
        assertEquals(render(&image, &font, "g", 40.0f, 60, 60), TINYIMG_OK);
    inkBounds(&image, &x0, &y0, &x1, &y1);
    failures += assertGreaterThan((double) y1, 4.0 + (double) fm.ascent);
    tiny_image_destroy(&image);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief Coverage is antialiased, not binary.
 *
 * A curve's edge lands between pixels, so a correct rasterizer produces partial
 * values there. An implementation that thresholded its coverage, or that
 * subsampled only in one axis without accumulating, would pass every ink test
 * and produce a jagged glyph.
 */
static int antialiasing(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage image;
    failures +=
        assertEquals(render(&image, &font, "S", 48.0f, 70, 70), TINYIMG_OK);

    uint32_t full = 0;
    uint32_t partial = 0;
    uint32_t empty = 0;
    size_t pixels = (size_t) image.width * image.height;

    for (size_t i = 0; i < pixels; i++) {
        uint8_t value = image.data[i];

        if (value == 0)
            empty++;
        else if (value == 255)
            full++;
        else
            partial++;
    }

    failures += assertGreaterThan((double) full, 0.0);
    failures += assertGreaterThan((double) empty, 0.0);

    // a curved glyph at this size has more edge than interior, so partial
    // coverage is not a handful of pixels; it is most of the ink
    failures += assertGreaterThan((double) partial, (double) full * 0.5);

    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief The nonzero fill rule leaves the counter of an `o` empty.
 *
 * An `o` is two contours wound opposite ways. A rasterizer that counted
 * crossings without their direction, or that counted a shared vertex twice,
 * fills the middle in or punches holes in the ring. Flood filling from the
 * center and checking it does not escape is the property; counting pixels is
 * not, because the count depends on the antialiasing.
 */
static int fillRule(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage image;
    failures +=
        assertEquals(render(&image, &font, "o", 64.0f, 80, 80), TINYIMG_OK);

    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
    inkBounds(&image, &x0, &y0, &x1, &y1);

    uint32_t centerX = (x0 + x1) / 2u;
    uint32_t centerY = (y0 + y1) / 2u;

    // the middle of the ring is uncovered
    failures +=
        assertEquals(image.data[(size_t) centerY * image.width + centerX], 0);

    // and it is enclosed: a four connected walk from the center over uncovered
    // pixels never reaches the edge of the image
    size_t pixels = (size_t) image.width * image.height;
    uint8_t* seen = (uint8_t*) calloc(pixels, 1);
    uint32_t* queue = (uint32_t*) malloc(pixels * sizeof(uint32_t));

    if (!seen || !queue) {
        free(seen);
        free(queue);
        tiny_image_destroy(&image);
        tiny_font_free(&font);
        free(bytes);
        return failures + 1;
    }

    uint32_t head = 0;
    uint32_t tail = 0;
    int escaped = 0;

    queue[tail++] = centerY * image.width + centerX;
    seen[centerY * image.width + centerX] = 1;

    while (head < tail) {
        uint32_t at = queue[head++];
        uint32_t x = at % image.width;
        uint32_t y = at / image.width;

        if (x == 0 || y == 0 || x + 1u == image.width ||
            y + 1u == image.height) {
            escaped = 1;
            break;
        }

        const int32_t steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

        for (uint32_t i = 0; i < 4u; i++) {
            uint32_t nx = (uint32_t) ((int32_t) x + steps[i][0]);
            uint32_t ny = (uint32_t) ((int32_t) y + steps[i][1]);
            uint32_t next = ny * image.width + nx;

            // any coverage at all is a wall, so a leak through the antialiased
            // rim counts
            if (seen[next] || image.data[next] != 0) continue;

            seen[next] = 1;
            queue[tail++] = next;
        }
    }

    failures += assertFalse(escaped);

    free(seen);
    free(queue);
    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief A composite glyph carries its components.
 *
 * DejaVu builds every accented letter as the base letter plus a combining
 * accent, and the accent lives near the top of the glyph order, which is why
 * the subsetter has to close the glyph set under components. A reader that
 * ignored composites draws the bare letter: same ink in the letter's own rows,
 * nothing above it, and no error anywhere.
 */
static int composites(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    struct {
        const char* accented;
        const char* bare;
        // `i` carries a tittle of its own at the diaeresis' height, so the
        // accented form starts no higher than the bare one and only the ink
        // comparison says anything
        int rises;
    } pairs[] = {
        {"\xC3\x89", "E", 1},
        {"\xC3\x85", "A", 1},
        {"\xC3\xAF", "i", 0},
        {"\xC3\xB1", "n", 1},
        {"\xC3\xBC", "u", 1}
    };

    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        TinyImage with;
        TinyImage without;

        failures += assertEquals(
            render(&with, &font, pairs[i].accented, 44.0f, 70, 90), TINYIMG_OK
        );
        failures += assertEquals(
            render(&without, &font, pairs[i].bare, 44.0f, 70, 90), TINYIMG_OK
        );

        printf("%s: ", pairs[i].bare);
        failures +=
            assertGreaterThan((double) ink(&with), (double) ink(&without));

        uint32_t ax;
        uint32_t ay;
        uint32_t bx;
        uint32_t by;
        uint32_t ignored;

        inkBounds(&with, &ax, &ay, &ignored, &ignored);
        inkBounds(&without, &bx, &by, &ignored, &ignored);
        (void) ax;
        (void) bx;

        // the accent sits above the letter, so the accented form's ink starts
        // higher
        if (pairs[i].rises) {
            printf("%s top: ", pairs[i].bare);
            failures += assertLessThan((double) ay, (double) by);
        }

        tiny_image_destroy(&with);
        tiny_image_destroy(&without);
    }

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** A bitmap face draws the pattern the generator wrote, exactly. */
static int bitmapGlyphs(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("tiny.psf", &font, &size);
    if (!bytes) return 1;

    // glyph 3 has its left column set and row 3 filled, so the ink is one
    // column plus one row less the pixel they share
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 8, 16, 1);

    TinyTextStyle style;
    tiny_text_style(&style, 16.0f);

    uint8_t white = 255;
    char one[2] = {3, 0};

    failures += assertEquals(
        tiny_image_draw_text(&image, &font, one, 0, 0, &style, &white),
        TINYIMG_OK
    );

    uint32_t lit = 0;
    for (uint32_t i = 0; i < 8u * 16u; i++) {
        if (image.data[i] == 255u)
            lit++;
        else if (image.data[i] != 0u)
            failures += assertEquals(image.data[i], 0);
    }

    // a bitmap face has no antialiasing, so every pixel is 0 or 255 and the
    // count is exact
    failures += assertEquals(lit, 16 + 8 - 1);
    failures += assertEquals(image.data[3u * 8u + 7u], 255);
    failures += assertEquals(image.data[0], 255);
    failures += assertEquals(image.data[7], 0);

    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);

    // the BDF face was written to the same pattern, so the hex nibble reader
    // has to agree with the bit reader
    TinyFont bdf;
    uint8_t* text = loadFont("tiny.bdf", &bdf, &size);
    if (!text) return failures + 1;

    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 8, 8, 1);
    tiny_text_style(&style, 8.0f);

    // the generator fills the row at the glyph's index in the declared order,
    // and 'C' is the fifth of the six codepoints it wrote
    const uint32_t filled = 4u;

    failures += assertEquals(
        tiny_image_draw_text(&image, &bdf, "C", 0, 0, &style, &white),
        TINYIMG_OK
    );

    lit = 0;
    for (uint32_t i = 0; i < 8u * 8u; i++) {
        if (image.data[i] == 255u) lit++;
    }

    failures += assertEquals(lit, 8 + 8 - 1);
    failures += assertEquals(image.data[filled * 8u + 7u], 255);
    failures += assertEquals(image.data[0], 255);

    tiny_image_destroy(&image);
    tiny_font_free(&bdf);
    free(text);
    return failures;
}

/**
 * @brief Clipping, at every edge and past every edge.
 *
 * A glyph the image cannot show is not rasterized at all, which is what keeps a
 * long string in a small image cheap. The property that matters is that the
 * visible part is unchanged by how much of the rest fell outside.
 */
static int clipping(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 30.0f);

    uint8_t white = 255;

    // entirely outside, four ways, each a no-op rather than a fault
    const int32_t places[4][2] = {{-500, 10}, {500, 10}, {10, -500}, {10, 500}};

    for (uint32_t i = 0; i < 4u; i++) {
        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, 60, 40, 1);

        failures += assertEquals(
            tiny_image_draw_text(
                &image, &font, "Hello", places[i][0], places[i][1], &style,
                &white
            ),
            TINYIMG_OK
        );
        failures += assertEquals(ink(&image), 0);

        tiny_image_destroy(&image);
    }

    // partly outside: the visible half is the same as it would be on a larger
    // image
    TinyImage narrow;
    TinyImage wide;

    memset(&narrow, 0, sizeof(narrow));
    memset(&wide, 0, sizeof(wide));
    tiny_image_create(&narrow, 40, 50, 1);
    tiny_image_create(&wide, 300, 50, 1);

    tiny_image_draw_text(&narrow, &font, "Hello, world", 2, 2, &style, &white);
    tiny_image_draw_text(&wide, &font, "Hello, world", 2, 2, &style, &white);

    int same = 1;
    for (uint32_t y = 0; y < 50u; y++) {
        for (uint32_t x = 0; x < 40u; x++) {
            if (narrow.data[(size_t) y * 40u + x] !=
                wide.data[(size_t) y * 300u + x]) {
                same = 0;
            }
        }
    }

    failures += assertTrue(same);
    failures += assertGreaterThan((double) ink(&narrow), 0.0);
    failures += assertGreaterThan((double) ink(&wide), (double) ink(&narrow));

    tiny_image_destroy(&narrow);
    tiny_image_destroy(&wide);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Color and channel counts, which go through the one compositor. */
static int colors(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle style;
    tiny_text_style(&style, 32.0f);

    for (uint8_t channels = 1; channels <= 4u; channels++) {
        TinyImage image;
        memset(&image, 0, sizeof(image));
        tiny_image_create(&image, 60, 50, channels);

        // as many channels as the image has, so the last one is the alpha
        // wherever there is one: a four channel color handed to a two channel
        // image would pass 100 as the alpha
        uint8_t color[4] = {200, 100, 50, 255};
        if (channels == 2u) color[1] = 255u;

        printf("%u channel: ", channels);
        failures += assertEquals(
            tiny_image_draw_text(&image, &font, "W", 2, 2, &style, color),
            TINYIMG_OK
        );

        // a fully covered pixel is the color asked for, whatever the channel
        // count
        int found = 0;
        size_t pixels = (size_t) image.width * image.height;

        for (size_t i = 0; i < pixels && !found; i++) {
            const uint8_t* pixel = image.data + i * channels;
            uint8_t alpha =
                channels == 2u || channels == 4u ? pixel[channels - 1u] : 255u;

            if (alpha != 255u) continue;

            uint8_t colors = channels == 4u   ? 3u
                             : channels == 2u ? 1u
                                              : channels;
            int matches = 1;

            for (uint8_t c = 0; c < colors; c++) {
                if (pixel[c] != color[c]) matches = 0;
            }

            found = matches;
        }

        printf("%u channel exact: ", channels);
        failures += assertTrue(found);

        tiny_image_destroy(&image);
    }

    // a transparent color draws nothing
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 60, 50, 4);

    uint8_t clear[4] = {255, 255, 255, 0};
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "W", 2, 2, &style, clear),
        TINYIMG_OK
    );
    failures += assertEquals(ink(&image), 0);

    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Arguments a caller can get wrong. */
static int rejections(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 40, 40, 3);

    uint8_t white[3] = {255, 255, 255};
    TinyTextStyle style;
    tiny_text_style(&style, 20.0f);

    failures += assertEquals(
        tiny_image_draw_text(0, &font, "x", 0, 0, &style, white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text(&image, 0, "x", 0, 0, &style, white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, 0, 0, 0, &style, white),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "x", 0, 0, &style, 0),
        TINYIMG_ERR_NULL
    );

    // a zeroed style means the face's own em, the same as passing NULL, so it
    // draws
    TinyTextStyle zero;
    memset(&zero, 0, sizeof(zero));
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "x", 0, 0, &zero, white), TINYIMG_OK
    );

    // a negative size is a caller error rather than a size to interpret
    TinyTextStyle backwards;
    tiny_text_style(&backwards, -12.0f);
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "x", 0, 0, &backwards, white),
        TINYIMG_ERR_RANGE
    );

    TinyTextMetrics ignored;
    failures += assertEquals(
        tiny_text_measure(&font, "x", &backwards, &ignored), TINYIMG_ERR_RANGE
    );

    // an empty string is not an error, and draws nothing
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "", 0, 0, &style, white), TINYIMG_OK
    );
    failures += assertEquals(ink(&image), 0);

    // a codepoint the face has no glyph for draws the notdef and reports
    // success, which is what a text renderer does with one
    failures += assertEquals(
        tiny_image_draw_text(
            &image, &font, "\xE4\xB8\xAD", 0, 0, &style, white
        ),
        TINYIMG_OK
    );

    // a malformed UTF-8 sequence is drawn as the replacement rather than
    // failing, and advances one byte so it does not swallow what follows it
    char malformed[4] = {'a', (char) 0xFF, 'b', 0};

    TinyTextMetrics broken;
    TinyTextMetrics clean;
    tiny_text_measure(&font, malformed, &style, &broken);
    tiny_text_measure(&font, "aXb", &style, &clean);
    failures += assertEquals(broken.glyphs, 3);
    failures += assertEquals(clean.glyphs, 3);

    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- basics --\n");
    failures += basics();
    printf("-- antialiasing --\n");
    failures += antialiasing();
    printf("-- fill rule --\n");
    failures += fillRule();
    printf("-- composites --\n");
    failures += composites();
    printf("-- bitmap glyphs --\n");
    failures += bitmapGlyphs();
    printf("-- clipping --\n");
    failures += clipping();
    printf("-- colors --\n");
    failures += colors();
    printf("-- rejections --\n");
    failures += rejections();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
