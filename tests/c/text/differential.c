#include "test.h"
#include "tinyimg/text.h"

/**
 * @file
 * @brief Rendered text against ImageMagick's own render of the same face.
 *
 * ImageMagick renders through FreeType with hinting on, which is a different
 * thing from what this library does: hinting moves stems onto pixel boundaries
 * and rounds every advance width to a whole pixel, and tinyimg puts the outline
 * where the outline is. So the two are compared on the two axes where hinting
 * does not decide the answer.
 *
 * **Shape, at a size where hinting is negligible.** A hinted stem is displaced
 * by at most a pixel, so the error is a fixed number of pixels and shrinks
 * against the glyph as the glyph grows. One `H` agrees at 29 dB at 32 pixels
 * and 41 dB at 256, which is the measurement that says the floor below belongs
 * at 256 and not at a readable size.
 *
 * **Coverage, at a readable size.** Total ink is insensitive to where a stem
 * landed, so a whole string can be compared at 32 pixels. It is the weaker
 * assertion of the two and it is the one that covers the advance widths,
 * because a wrong advance changes how many glyphs land inside the box.
 *
 * Comparing whole strings by PSNR is what this deliberately does not do:
 * FreeType's integer advances accumulate, so a twelve glyph run drifts from
 * ours by a pixel or two by the end and reads as 17 dB. That number would
 * measure hinting rather than anything in this library.
 */

/** Em size the per-glyph comparison runs at; see the file comment. */
#define GLYPH_SIZE 256.0f

/** The box `scripts/fixtures.ts` rendered each glyph into. */
#define GLYPH_WIDTH 320u
#define GLYPH_HEIGHT 384u
#define GLYPH_BASELINE 300.0f

/** Where the reference put the pen. */
#define PEN_X 6

/**
 * @brief Floor for one glyph's agreement at GLYPH_SIZE.
 *
 * Measured in this box: x 37.5, H 36.1, W 34.9, g 32.9, o 31.6, S 30.2, e 27.0.
 * The floor sits below the worst of those, because it bounds a difference this
 * library does not control.
 *
 * `e` is the worst by a wide margin and it is the shape that should be: its
 * crossbar is a thin horizontal, which is exactly what hinting snaps onto a
 * pixel row, and a one row displacement of a bar that wide moves more ink than
 * a one column displacement of a stem. Its coverage still agrees to 99.16%, so
 * the ink is all there and in the wrong place by a pixel rather than missing.
 */
#define GLYPH_PSNR_FLOOR 24.0

/** How far total coverage may differ, as a fraction. */
#define COVERAGE_TOLERANCE 0.02

static uint8_t* loadFont(TinyFont* font, size_t* size) {
    unsigned char* bytes = readFixture("derived/fonts/dejavu-latin.ttf", size);
    if (!bytes) return 0;

    if (tiny_font_load_bytes(font, bytes, *size) != TINYIMG_OK) {
        free(bytes);
        return 0;
    }

    return bytes;
}

/** Decodes a reference PNG into a single channel image. */
static int loadReference(const char* name, TinyImage* out) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    memset(out, 0, sizeof(*out));
    int result = tiny_image_load(out, bytes, size);
    free(bytes);

    if (result != TINYIMG_OK) return result;
    return tiny_image_to_grayscale(out);
}

static long coverage(const TinyImage* image) {
    long total = 0;
    size_t pixels = (size_t) image->width * image->height;

    for (size_t i = 0; i < pixels; i++) total += image->data[i];
    return total;
}

/**
 * @brief One glyph, ours against theirs.
 *
 * The reference put the baseline at GLYPH_BASELINE, and this library takes the
 * top of the line box, so the pen goes at the baseline less the ascent. Getting
 * that wrong shifts every pixel and the PSNR collapses, which is what makes the
 * placement part of the assertion rather than something tuned until the numbers
 * agree.
 */
static int oneGlyph(
    const TinyFont* font, const char* glyph, uint32_t codepoint
) {
    int failures = 0;

    char name[256];
    snprintf(name, sizeof(name), "derived/ref/text-%x.png", codepoint);

    TinyImage reference;
    if (loadReference(name, &reference) != TINYIMG_OK) {
        printf("could not read %s\n", name);
        return 1;
    }

    failures += assertEquals(reference.width, GLYPH_WIDTH);
    failures += assertEquals(reference.height, GLYPH_HEIGHT);

    TinyFontMetrics fm;
    tiny_font_metrics(font, GLYPH_SIZE, &fm);

    TinyImage ours;
    memset(&ours, 0, sizeof(ours));
    tiny_image_create(&ours, GLYPH_WIDTH, GLYPH_HEIGHT, 1);

    TinyTextStyle style;
    tiny_text_style(&style, GLYPH_SIZE);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text(
            &ours, font, glyph, PEN_X, (int32_t) (GLYPH_BASELINE - fm.ascent),
            &style, &white
        ),
        TINYIMG_OK
    );

    size_t pixels = (size_t) GLYPH_WIDTH * GLYPH_HEIGHT;

    printf("'%s' shape: ", glyph);
    failures += assertPSNR(ours.data, reference.data, pixels, GLYPH_PSNR_FLOOR);

    long mine = coverage(&ours);
    long theirs = coverage(&reference);

    printf(
        "'%s' coverage %.2f%%: ", glyph, 100.0 * (double) mine / (double) theirs
    );
    failures += assertIn(
        (double) mine, (double) theirs * (1.0 - COVERAGE_TOLERANCE),
        (double) theirs * (1.0 + COVERAGE_TOLERANCE)
    );

    tiny_image_destroy(&ours);
    tiny_image_destroy(&reference);
    return failures;
}

static int glyphs(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont(&font, &size);
    if (!bytes) return 1;

    const char* set[] = {"H", "o", "e", "W", "g", "S", "x"};

    for (unsigned i = 0; i < sizeof(set) / sizeof(set[0]); i++) {
        failures += oneGlyph(&font, set[i], (uint32_t) set[i][0]);
    }

    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief A whole string's coverage at a readable size.
 *
 * What this covers that the per-glyph comparison does not: the advance widths
 * and the kerning. A wrong advance puts a glyph somewhere else, and over twelve
 * of them the run either overflows the reference's box or falls short of it,
 * and either way the coverage moves.
 */
static int string(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont(&font, &size);
    if (!bytes) return 1;

    TinyImage reference;
    if (loadReference("derived/ref/text-string.png", &reference) !=
        TINYIMG_OK) {
        tiny_font_free(&font);
        free(bytes);
        return 1;
    }

    TinyFontMetrics fm;
    tiny_font_metrics(&font, 32.0f, &fm);

    TinyImage ours;
    memset(&ours, 0, sizeof(ours));
    tiny_image_create(&ours, reference.width, reference.height, 1);

    TinyTextStyle style;
    tiny_text_style(&style, 32.0f);

    uint8_t white = 255;
    failures += assertEquals(
        tiny_image_draw_text(
            &ours, &font, "Hamburgefons", PEN_X, (int32_t) (46.0f - fm.ascent),
            &style, &white
        ),
        TINYIMG_OK
    );

    long mine = coverage(&ours);
    long theirs = coverage(&reference);

    printf("string coverage %.2f%%: ", 100.0 * (double) mine / (double) theirs);
    failures += assertIn(
        (double) mine, (double) theirs * (1.0 - COVERAGE_TOLERANCE),
        (double) theirs * (1.0 + COVERAGE_TOLERANCE)
    );

    // and the run occupies the same columns, to within the accumulated advance
    // rounding
    uint32_t mineFirst = ours.width;
    uint32_t mineLast = 0;
    uint32_t theirFirst = reference.width;
    uint32_t theirLast = 0;

    for (uint32_t x = 0; x < ours.width; x++) {
        for (uint32_t y = 0; y < ours.height; y++) {
            size_t at = (size_t) y * ours.width + x;

            if (ours.data[at] != 0) {
                if (x < mineFirst) mineFirst = x;
                if (x > mineLast) mineLast = x;
            }
            if (reference.data[at] != 0) {
                if (x < theirFirst) theirFirst = x;
                if (x > theirLast) theirLast = x;
            }
        }
    }

    printf("left edge: ");
    failures += assertIn(
        (double) mineFirst, (double) theirFirst - 2.0, (double) theirFirst + 2.0
    );
    printf("right edge: ");
    failures += assertIn(
        (double) mineLast, (double) theirLast - 4.0, (double) theirLast + 4.0
    );

    tiny_image_destroy(&ours);
    tiny_image_destroy(&reference);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

/**
 * @brief A wrong baseline is caught, which is what makes the placement above an
 * assertion.
 *
 * Drawing the same glyph a few pixels off has to fall below the floor. Without
 * this the PSNR comparison would pass for any placement that happened to be
 * close, and there would be no evidence that the number means anything.
 */
static int placementMatters(void) {
    int failures = 0;
    size_t size = 0;
    TinyFont font;

    uint8_t* bytes = loadFont(&font, &size);
    if (!bytes) return 1;

    TinyImage reference;
    if (loadReference("derived/ref/text-48.png", &reference) != TINYIMG_OK) {
        tiny_font_free(&font);
        free(bytes);
        return 1;
    }

    TinyFontMetrics fm;
    tiny_font_metrics(&font, GLYPH_SIZE, &fm);

    TinyTextStyle style;
    tiny_text_style(&style, GLYPH_SIZE);

    size_t pixels = (size_t) GLYPH_WIDTH * GLYPH_HEIGHT;
    int32_t right = (int32_t) (GLYPH_BASELINE - fm.ascent);

    const int32_t offsets[] = {-8, 8};

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        TinyImage wrong;
        memset(&wrong, 0, sizeof(wrong));
        tiny_image_create(&wrong, GLYPH_WIDTH, GLYPH_HEIGHT, 1);

        uint8_t white = 255;
        tiny_image_draw_text(
            &wrong, &font, "H", PEN_X, right + offsets[i], &style, &white
        );

        double psnr = computePSNR(wrong.data, reference.data, pixels);

        printf("H offset %+d gives %.2f dB: ", offsets[i], psnr);
        failures += assertLessThan(psnr, GLYPH_PSNR_FLOOR);

        tiny_image_destroy(&wrong);
    }

    tiny_image_destroy(&reference);
    tiny_font_free(&font);
    free(bytes);
    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- glyph shapes at %g px --\n", (double) GLYPH_SIZE);
    failures += glyphs();
    printf("-- string coverage at 32 px --\n");
    failures += string();
    printf("-- placement matters --\n");
    failures += placementMatters();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
