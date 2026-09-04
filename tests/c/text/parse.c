#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief Loading and reading a face, across the three formats.
 *
 * The fixtures are committed rather than read from `blobs/`, which is
 * gitignored: a test that skipped when a blob was absent would not gate.
 * `dejavu-latin.ttf` is the real DejaVu Sans cut down to 110 glyphs by
 * `scripts/fonts.ts`, and it renders identically to the full face; the bitmap
 * faces are synthesized so the expected pixels are known rather than measured.
 */

static uint8_t* loadFont(const char* name, TinyFont* font, size_t* size) {
    char path[256];
    snprintf(path, sizeof(path), "derived/fonts/%s", name);

    unsigned char* bytes = readFixture(path, size);
    if (!bytes) return 0;

    int result = tiny_font_load_bytes(font, bytes, *size);
    if (result != TINYIMG_OK) {
        printf("load %s failed with %d\n", name, result);
        free(bytes);
        return 0;
    }

    return bytes;
}

static int truetype(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_TRUETYPE);
    failures += assertEquals(font.units_per_em, 2048);
    failures += assertEquals(font.glyphs, 110);

    // the subset keeps the source face's own vertical metrics, so these are
    // DejaVu's
    failures += assertEquals(font.ascent, 1901);
    failures += assertEquals(font.descent, 483);
    failures += assertTrue(font.cmap != 0);
    failures += assertTrue(font.glyf != 0);
    failures += assertTrue(font.loca != 0);
    failures += assertTrue(font.hmtx != 0);
    failures += assertTrue(font.kern != 0);

    // a face this small still uses the long loca format, because the source did
    // and the format is a property of head rather than of the size
    failures += assertEquals(font.long_loca, 1);

    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 'z'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, ' '), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0xE9), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0xC5), 1);

    // outside the subset, and outside the BMP, which format 4 cannot express at
    // all
    failures += assertEquals(tiny_font_has_glyph(&font, 0x2014), 0);
    failures += assertEquals(tiny_font_has_glyph(&font, 0x4E2D), 0);
    failures += assertEquals(tiny_font_has_glyph(&font, 0x1F600), 0);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief The format 12 subset, which is the same glyphs behind a different
 * table.
 *
 * The two differ in exactly one mapping: U+1F600 is aliased onto the glyph `A`
 * uses, because it is past the BMP and only format 12 reaches there. Nothing in
 * the subset needs it, so it is the one observable difference and it is what
 * says the format 12 reader ran.
 */
static int cmap12(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin-cmap12.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_TRUETYPE);
    failures += assertEquals(font.glyphs, 110);
    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0xE9), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0x1F600), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0x1F601), 0);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief The format 6 subtable and the short `loca` form.
 *
 * Both are variants the reader claims to handle, and neither is reachable
 * through the other two subsets: they carry format 4 and 12 and the long `loca`
 * the source face uses. A claim of support with nothing exercising it is the
 * same as no support.
 *
 * Format 6 cannot express a gap, so this variant maps only the longest dense
 * run of the wanted set, which is ASCII. The accents fall outside it, and that
 * is the observable difference.
 */
static int cmap6(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin-cmap6.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_TRUETYPE);
    failures += assertEquals(font.glyphs, 110);

    // the short form, which halves every offset
    failures += assertEquals(font.long_loca, 0);

    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, '~'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, ' '), 1);

    // outside the dense run format 6 could carry
    failures += assertEquals(tiny_font_has_glyph(&font, 0xE9), 0);
    failures += assertEquals(tiny_font_has_glyph(&font, 0x1F), 0);

    // and the glyphs it does carry render, so the halved offsets were read
    // right
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 60, 50, 1);

    TinyTextStyle style;
    tiny_text_style(&style, 32.0f);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text(&image, &font, "Rg", 2, 2, &style, &white),
        TINYIMG_OK
    );

    long ink = 0;
    for (uint32_t i = 0; i < 60u * 50u; i++) ink += image.data[i];
    failures += assertGreaterThan((double) ink, 0.0);

    tiny_image_destroy(&image);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief A face with no `hhea`, which is out of specification and recoverable.
 *
 * The em box stands in for the line height and every glyph takes the first
 * advance. A reader that refused would be within its rights and would also be
 * useless on a face like this; a reader that read a null would be a fault.
 */
static int noHhea(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin-no-hhea.ttf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_TRUETYPE);

    // the em box, since there is no ascender to read
    failures += assertEquals(font.ascent, (long) font.units_per_em);
    failures += assertEquals(font.descent, 0);
    failures += assertEquals(font.hmetrics, 1);

    TinyTextStyle style;
    tiny_text_style(&style, 20.0f);

    TinyTextMetrics wide;
    TinyTextMetrics narrow;

    failures += assertEquals(
        tiny_text_measure(&font, "MMMM", &style, &wide), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_text_measure(&font, "iiii", &style, &narrow), TINYIMG_OK
    );

    // every glyph shares one advance, so a wide string and a narrow one measure
    // the same
    failures += assertFloatEquals(wide.width, narrow.width, 0.0f);
    failures += assertGreaterThan((double) wide.width, 0.0);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** BDF faces missing a header the reader needs. */
static int bdfRejections(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    struct {
        const char* name;
        int expected;
    } cases[] = {
        {"bdf-no-chars.bdf", TINYIMG_ERR_CORRUPT},
        {"bdf-no-bbox.bdf", TINYIMG_ERR_CORRUPT}
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "derived/fonts/%s", cases[i].name);

        unsigned char* bytes = readFixture(path, &size);
        if (!bytes) {
            failures++;
            continue;
        }

        printf("%s: ", cases[i].name);
        failures += assertEquals(
            tiny_font_load_bytes(&font, bytes, size), cases[i].expected
        );
        free(bytes);
    }

    // a negative descent, which is the one signed number the BDF reader parses
    unsigned char* bytes =
        readFixture("derived/fonts/bdf-descender.bdf", &size);
    if (!bytes) return failures + 1;

    failures +=
        assertEquals(tiny_font_load_bytes(&font, bytes, size), TINYIMG_OK);
    failures += assertEquals(font.cell_height, 10);
    failures += assertEquals(font.descent, 2);
    failures += assertEquals(font.ascent, 8);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

static int metrics(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyFontMetrics at32;
    failures +=
        assertEquals(tiny_font_metrics(&font, 32.0f, &at32), TINYIMG_OK);

    // 1901 font units of ascent at 32 pixels per 2048 units
    failures +=
        assertFloatEquals(at32.ascent, 1901.0f * 32.0f / 2048.0f, 0.001f);
    failures +=
        assertFloatEquals(at32.descent, 483.0f * 32.0f / 2048.0f, 0.001f);
    failures += assertFloatEquals(at32.size, 32.0f, 0.0f);
    failures += assertEquals(at32.glyphs, 110);
    failures += assertEquals(at32.fixed_size, 0);

    // an outline face scales linearly, so twice the size is twice every metric
    TinyFontMetrics at64;
    tiny_font_metrics(&font, 64.0f, &at64);
    failures += assertFloatEquals(at64.ascent, at32.ascent * 2.0f, 0.001f);
    failures +=
        assertFloatEquals(at64.line_height, at32.line_height * 2.0f, 0.001f);

    // zero means the face's own em, which is its unitsPerEm in pixels
    TinyFontMetrics natural;
    tiny_font_metrics(&font, 0.0f, &natural);
    failures += assertFloatEquals(natural.size, 2048.0f, 0.0f);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief Kerning, which is only observable as a width.
 *
 * `AV` is the canonical kerned pair: the two diagonals tuck together, so the
 * pair is narrower than the sum of its advances. Asserting the direction and
 * that the two settings disagree is what says the table was read; asserting the
 * exact number would be asserting DejaVu's design.
 */
static int kerning(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("dejavu-latin.ttf", &font, &size);
    if (!bytes) return 1;

    TinyTextStyle on;
    tiny_text_style(&on, 32.0f);

    TinyTextStyle off = on;
    off.kerning = 0;

    TinyTextMetrics kerned;
    TinyTextMetrics plain;

    failures += assertEquals(
        tiny_text_measure(&font, "AVATAR", &on, &kerned), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_text_measure(&font, "AVATAR", &off, &plain), TINYIMG_OK
    );

    failures += assertLessThan(kerned.width, plain.width);
    failures += assertEquals(kerned.glyphs, 6);
    failures += assertEquals(plain.glyphs, 6);

    // a string with no kern pair in it measures the same either way
    TinyTextMetrics unkernable;
    TinyTextMetrics unkernableOff;
    tiny_text_measure(&font, "illili", &on, &unkernable);
    tiny_text_measure(&font, "illili", &off, &unkernableOff);
    failures += assertFloatEquals(unkernable.width, unkernableOff.width, 0.0f);

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/** Both PSF versions, whose bitmaps the generator wrote to a known pattern. */
static int psf(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("tiny.psf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_PSF);
    failures += assertEquals(font.cell_width, 8);
    failures += assertEquals(font.cell_height, 16);
    failures += assertEquals(font.glyphs, 128);
    failures += assertEquals(font.glyph_bytes, 16);

    // a bitmap face has one size, and asking for another does not change it
    TinyFontMetrics fm;
    tiny_font_metrics(&font, 48.0f, &fm);
    failures += assertFloatEquals(fm.size, 16.0f, 0.0f);
    failures += assertEquals(fm.fixed_size, 1);
    failures += assertFloatEquals(fm.ascent, 16.0f, 0.0f);

    // the codepoint is the glyph index, so coverage is the glyph count
    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 127), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 128), 0);

    tiny_font_free(&font);
    free(bytes);

    TinyFont one;
    uint8_t* psf1 = loadFont("tiny-psf1.psf", &one, &size);
    if (!psf1) return failures + 1;

    failures += assertEquals(one.kind, TINYIMG_FONT_PSF);
    failures += assertEquals(one.cell_width, 8);
    failures += assertEquals(one.cell_height, 8);
    failures += assertEquals(one.glyphs, 256);

    tiny_font_free(&one);
    free(psf1);
    return failures;
}

static int bdf(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont("tiny.bdf", &font, &size);
    if (!bytes) return 1;

    failures += assertEquals(font.kind, TINYIMG_FONT_BDF);
    failures += assertEquals(font.cell_width, 8);
    failures += assertEquals(font.cell_height, 8);

    // six glyphs were declared and six were indexed, which is the whole point
    // of the index: BDF has none of its own and finding a glyph without one
    // costs a scan of the file per character
    failures += assertEquals(font.index_count, 6);
    failures += assertEquals(font.glyphs, 6);
    failures += assertNotNull(font.index);

    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 'C'), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 0xE9), 1);
    failures += assertEquals(tiny_font_has_glyph(&font, 'Z'), 0);

    tiny_font_free(&font);

    // free leaves the face safe to load into again, and safe to free twice
    failures += assertNull(font.index);
    tiny_font_free(&font);

    free(bytes);
    return failures;
}

/** Every way a load can fail, each with its own code. */
static int rejections(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    failures += assertEquals(
        tiny_font_load_bytes(0, (const uint8_t*) "x", 1), TINYIMG_ERR_NULL
    );
    failures +=
        assertEquals(tiny_font_load_bytes(&font, 0, 1), TINYIMG_ERR_NULL);

    // too short to hold a directory, which is a different answer from
    // unrecognized
    uint8_t stub[4] = {0, 1, 0, 0};
    failures += assertEquals(
        tiny_font_load_bytes(&font, stub, sizeof(stub)),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    struct {
        const char* name;
        int expected;
    } cases[] = {// an OpenType wrapper around CFF charstrings: recognized, and
                 // a format this library
                 // does not read rather than one it failed to parse
                 {"cff.otf", TINYIMG_ERR_UNSUPPORTED_VARIANT},
                 {"not-a-font.bin", TINYIMG_ERR_UNKNOWN_FORMAT},
                 // a real directory whose tables fall outside the file
                 {"truncated.ttf", TINYIMG_ERR_CORRUPT}
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "derived/fonts/%s", cases[i].name);

        unsigned char* bytes = readFixture(path, &size);
        if (!bytes) {
            failures++;
            continue;
        }

        printf("%s: ", cases[i].name);
        failures += assertEquals(
            tiny_font_load_bytes(&font, bytes, size), cases[i].expected
        );
        free(bytes);
    }

    // a face that never loaded cannot be drawn with, and says so rather than
    // reading a null
    TinyFont empty;
    memset(&empty, 0, sizeof(empty));

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 32, 32, 3);

    uint8_t white[3] = {255, 255, 255};
    failures += assertEquals(
        tiny_image_draw_text(&image, &empty, "hi", 0, 0, 0, white),
        TINYIMG_ERR_BLOB_MISSING
    );

    tiny_image_destroy(&image);
    return failures;
}

/** The blob path, which is how a Worker loads a face. */
static int blobs(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    tiny_blob_free_all();

    failures += assertEquals(
        tiny_font_load(&font, "missing"), TINYIMG_ERR_BLOB_MISSING
    );

    unsigned char* bytes = readFixture("derived/fonts/dejavu-latin.ttf", &size);
    if (!bytes) return failures + 1;

    // the blob table owns what it is given, so the copy comes from the
    // library's allocator
    uint8_t* owned = (uint8_t*) tiny_alloc(size);
    if (!owned) {
        free(bytes);
        return failures + 1;
    }

    memcpy(owned, bytes, size);
    free(bytes);

    failures += assertEquals(
        tiny_blob_load(TINYIMG_BLOB_FONT, "latin", owned, size), TINYIMG_OK
    );

    // NULL asks for the first font blob, which is the only one loaded
    failures += assertEquals(tiny_font_load(&font, 0), TINYIMG_OK);
    failures += assertEquals(font.glyphs, 110);
    failures += assertEquals(tiny_font_has_glyph(&font, 'A'), 1);
    tiny_font_free(&font);

    failures += assertEquals(tiny_font_load(&font, "latin"), TINYIMG_OK);
    failures += assertEquals(font.kind, TINYIMG_FONT_TRUETYPE);
    tiny_font_free(&font);

    failures +=
        assertEquals(tiny_font_load(&font, "other"), TINYIMG_ERR_BLOB_MISSING);

    tiny_blob_free_all();
    return failures;
}

/** The structure sizes a host reserves memory from. */
static int layout(void) {
    int failures = 0;

    failures += assertEquals(tiny_font_sizeof(), (long) sizeof(TinyFont));
    failures += assertEquals(
        tiny_font_metrics_sizeof(), (long) sizeof(TinyFontMetrics)
    );
    failures +=
        assertEquals(tiny_text_style_sizeof(), (long) sizeof(TinyTextStyle));
    failures += assertEquals(
        tiny_text_metrics_sizeof(), (long) sizeof(TinyTextMetrics)
    );

    // the defaults a caller should start from, rather than a zeroed structure
    TinyTextStyle style;
    tiny_text_style(&style, 18.0f);

    failures += assertFloatEquals(style.size, 18.0f, 0.0f);
    failures += assertFloatEquals(style.line_height, 1.0f, 0.0f);
    failures += assertFloatEquals(style.tracking, 0.0f, 0.0f);
    failures += assertEquals(style.kerning, 1);

    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- truetype --\n");
    failures += truetype();
    printf("-- cmap format 12 --\n");
    failures += cmap12();
    printf("-- cmap format 6 and short loca --\n");
    failures += cmap6();
    printf("-- no hhea --\n");
    failures += noHhea();
    printf("-- bdf rejections --\n");
    failures += bdfRejections();
    printf("-- metrics --\n");
    failures += metrics();
    printf("-- kerning --\n");
    failures += kerning();
    printf("-- psf --\n");
    failures += psf();
    printf("-- bdf --\n");
    failures += bdf();
    printf("-- rejections --\n");
    failures += rejections();
    printf("-- blobs --\n");
    failures += blobs();
    printf("-- layout --\n");
    failures += layout();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
