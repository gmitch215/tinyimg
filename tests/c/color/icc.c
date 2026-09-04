#include <math.h>

#include "tinyimg/color.h"

#include "test.h"

/** Loads a profile from the fixture set. */
static int load(TinyIccProfile* profile, const char* name) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);

    if (!bytes) return 0;

    int result = tiny_icc_parse(profile, bytes, size);
    free(bytes);

    return result == TINYIMG_OK;
}

/**
 * @brief The primaries a profile records, back out as chromaticities.
 *
 * The check that the matrix was read the right way round: the tags hold the
 * columns, so a transposed read gives a matrix that is still plausible and
 * whose primaries are wrong.
 *
 * @param profile The profile.
 * @param column Which primary, 0 red through 2 blue.
 * @param x Receives the x chromaticity.
 * @param y Receives the y chromaticity.
 */
static void primary_of(
    const TinyIccProfile* profile, uint32_t column, float* x, float* y
) {
    float sum = 0.0f;

    for (uint32_t row = 0; row < 3u; row++) {
        sum += profile->to_xyz[row * 3u + column];
    }

    *x = profile->to_xyz[column] / sum;
    *y = profile->to_xyz[3u + column] / sum;
}

/** Every generated profile parses, and its primaries are what was written. */
static int parses(void) {
    int failures = 0;
    TinyIccProfile srgb;

    if (!load(&srgb, "derived/icc/srgb.icc")) return 1;

    // the matrix entry, not the chromaticity: D50 adapted sRGB has X_r 0.4361,
    // where the unadapted D65 matrix has 0.4124. that gap is the evidence the
    // generator's Bradford adaptation was read back rather than ignored, and
    // 0.4361 is also what Apple's own sRGB profile carries
    failures += assertFloatEquals(srgb.to_xyz[0], 0.4361f, 0.002f);

    // the primary's chromaticity, which the adaptation leaves near sRGB's own
    // red corner; a matrix read transposed puts this somewhere else entirely
    float x;
    float y;
    primary_of(&srgb, 0, &x, &y);

    failures += assertFloatEquals(x, 0.6485f, 0.004f);
    failures += assertFloatEquals(y, 0.3309f, 0.004f);

    // the white point is D50, which is what the connection space is
    failures += assertFloatEquals(srgb.white[0], 0.9642f, 0.002f);
    failures += assertFloatEquals(srgb.white[1], 1.0f, 0.002f);
    failures += assertFloatEquals(srgb.white[2], 0.8249f, 0.002f);

    // the second row of a to-XYZ matrix is the luminance weights, so it sums
    // to the white point's Y, which is one
    float luminance = srgb.to_xyz[3] + srgb.to_xyz[4] + srgb.to_xyz[5];
    failures += assertFloatEquals(luminance, 1.0f, 0.003f);

    // and the curve is the sRGB transfer function, not a plain 2.2 power: the
    // two differ most in the shadows, which is where this looks
    failures += assertFloatEquals(srgb.curve[0][0], 0.0f, 0.001f);
    failures += assertFloatEquals(srgb.curve[0][255], 1.0f, 0.002f);
    failures += assertFloatEquals(srgb.curve[0][128], 0.2158f, 0.004f);
    failures += assertFalse(srgb.linear);

    static const char* const NAMES[3] = {
        "derived/icc/display-p3.icc", "derived/icc/adobe-rgb-1998.icc",
        "derived/icc/rec2020.icc"
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyIccProfile profile;

        failures += assertTrue(load(&profile, NAMES[i]));

        float sum = profile.to_xyz[3] + profile.to_xyz[4] + profile.to_xyz[5];
        failures += assertFloatEquals(sum, 1.0f, 0.004f);
    }

    return failures;
}

/**
 * @brief A wider profile's primaries sit outside sRGB's.
 *
 * The ordering is the check that each profile carries its own primaries rather
 * than all four parsing to the same matrix: Rec. 2020 is wider than P3, which
 * is wider than sRGB, and a red x has to follow that order.
 */
static int gamuts_are_ordered(void) {
    int failures = 0;
    TinyIccProfile srgb;
    TinyIccProfile p3;
    TinyIccProfile rec2020;

    if (!load(&srgb, "derived/icc/srgb.icc")) return 1;
    if (!load(&p3, "derived/icc/display-p3.icc")) return 1;
    if (!load(&rec2020, "derived/icc/rec2020.icc")) return 1;

    float srgb_x;
    float p3_x;
    float wide_x;
    float ignored;

    primary_of(&srgb, 0, &srgb_x, &ignored);
    primary_of(&p3, 0, &p3_x, &ignored);
    primary_of(&rec2020, 0, &wide_x, &ignored);

    failures += assertGreaterThan(p3_x, srgb_x);
    failures += assertGreaterThan(wide_x, p3_x);

    return failures;
}

/** sRGB to sRGB is the identity, on every level of every channel. */
static int srgb_is_the_identity(void) {
    int failures = 0;
    TinyIccProfile srgb;

    if (!load(&srgb, "derived/icc/srgb.icc")) return 1;

    uint32_t worst = 0;

    for (uint32_t v = 0; v < 256u; v++) {
        uint8_t in[3] = {(uint8_t) v, (uint8_t) v, (uint8_t) v};
        uint8_t out[3];

        failures += assertEquals(tiny_icc_to_srgb(&srgb, in, out), 0);

        for (uint32_t c = 0; c < 3u; c++) {
            uint32_t diff = out[c] > v ? out[c] - v : v - out[c];
            if (diff > worst) worst = diff;
        }
    }

    // one level, which is the round trip through a 256 entry table and a
    // matrix inverse; a transposed matrix or a wrong curve is tens of levels
    failures += assertLessThan(worst, 2);

    // the built-in sRGB and the parsed one agree, so the constant in the
    // source and the bytes the generator wrote are the same profile
    TinyIccProfile built;
    failures += assertEquals(tiny_icc_srgb(&built), 0);

    for (uint32_t i = 0; i < 9u; i++) {
        failures += assertFloatEquals(built.to_xyz[i], srgb.to_xyz[i], 0.003f);
    }

    for (uint32_t v = 0; v < 256u; v++) {
        failures +=
            assertFloatEquals(built.curve[1][v], srgb.curve[1][v], 0.006f);
    }

    return failures;
}

/**
 * @brief A wide gamut color converts inward, and a neutral one stays neutral.
 *
 * Full red in P3 is outside sRGB, so converting it has to clip rather than
 * stay at 255 across the board, and gray has to stay gray: an adaptation
 * applied in the wrong direction breaks the second of those first.
 */
static int conversion_moves_the_right_way(void) {
    int failures = 0;
    TinyIccProfile p3;

    if (!load(&p3, "derived/icc/display-p3.icc")) return 1;

    uint8_t red[3] = {255, 0, 0};
    uint8_t out[3];

    failures += assertEquals(tiny_icc_to_srgb(&p3, red, out), 0);

    // P3's red is more saturated than sRGB can hold, so it clips at full red
    // and the other two channels stay at nothing
    failures += assertEquals(out[0], 255);
    failures += assertLessThan(out[1], 20);
    failures += assertLessThan(out[2], 20);

    // a mid gray is inside both gamuts and both are D50 adapted, so it has to
    // come back neutral and at the same level
    uint8_t gray[3] = {128, 128, 128};
    failures += assertEquals(tiny_icc_to_srgb(&p3, gray, out), 0);

    failures += assertIn((double) out[0], 126.0, 130.0);

    // the three channels go through three matrix rows and round separately, so
    // a level between them is the arithmetic rather than a color cast; an
    // adaptation applied in the wrong direction is worth tens of levels here
    int32_t spread = 0;

    for (uint32_t c = 1; c < 3u; c++) {
        int32_t diff = (int32_t) out[c] - (int32_t) out[0];
        if (diff < 0) diff = -diff;
        if (diff > spread) spread = diff;
    }

    failures += assertLessThan(spread, 2);

    // a P3 green is inside sRGB's green corner in hue but outside in
    // saturation, so its blue stays low and its green clips
    uint8_t green[3] = {0, 255, 0};
    failures += assertEquals(tiny_icc_to_srgb(&p3, green, out), 0);
    failures += assertEquals(out[1], 255);
    failures += assertLessThan(out[0], 40);

    return failures;
}

/**
 * @brief The per-image conversion agrees with the per-pixel one.
 *
 * The image path folds the two matrices and precomputes the encode, so it is a
 * different arrangement of the same arithmetic; a difference between them is a
 * fault in the folding.
 */
static int image_matches_pixel(void) {
    int failures = 0;
    TinyIccProfile p3;
    TinyImage image;

    if (!load(&p3, "derived/icc/display-p3.icc")) return 1;
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return 1;

    for (uint32_t i = 0; i < 256u; i++) {
        image.data[i * 3u] = (uint8_t) i;
        image.data[i * 3u + 1u] = (uint8_t) (255u - i);
        image.data[i * 3u + 2u] = (uint8_t) ((i * 7u) & 0xFFu);
    }

    TinyImage expected;
    if (tiny_image_create(&expected, 16, 16, 3) != TINYIMG_OK) {
        tiny_image_destroy(&image);
        return failures + 1;
    }

    for (uint32_t i = 0; i < 256u; i++) {
        tiny_icc_to_srgb(&p3, image.data + i * 3u, expected.data + i * 3u);
    }

    failures += assertEquals(tiny_icc_convert_image(&image, &p3), 0);

    uint32_t worst = 0;

    for (uint32_t i = 0; i < 256u * 3u; i++) {
        uint32_t diff = image.data[i] > expected.data[i]
                            ? image.data[i] - expected.data[i]
                            : expected.data[i] - image.data[i];

        if (diff > worst) worst = diff;
    }

    // the image path encodes through a 4096 entry table where the pixel path
    // computes the power directly, which is worth under a level
    failures += assertLessThan(worst, 2);

    tiny_image_destroy(&image);
    tiny_image_destroy(&expected);
    return failures;
}

/** The matrix between two profiles is the identity when they are the same. */
static int matrix_between(void) {
    int failures = 0;
    TinyIccProfile p3;

    if (!load(&p3, "derived/icc/display-p3.icc")) return 1;

    float m[9];
    failures += assertEquals(tiny_icc_matrix_between(m, &p3, &p3), 0);

    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            failures += assertFloatEquals(
                m[row * 3u + col], row == col ? 1.0f : 0.0f, 0.001f
            );
        }
    }

    // and P3 to sRGB expands: the off-diagonal terms are what pull a
    // saturated P3 color back inside sRGB, so they cannot all be zero
    TinyIccProfile srgb;
    failures += assertEquals(tiny_icc_srgb(&srgb), 0);
    failures += assertEquals(tiny_icc_matrix_between(m, &p3, &srgb), 0);

    failures += assertGreaterThan(m[0], 1.0f);
    failures += assertLessThan(m[1], 0.0f);

    // each row sums to one, or a neutral color would not stay neutral
    for (uint32_t row = 0; row < 3u; row++) {
        float sum = m[row * 3u] + m[row * 3u + 1u] + m[row * 3u + 2u];

        failures += assertFloatEquals(sum, 1.0f, 0.004f);
    }

    return failures;
}

/**
 * @brief Our conversion against ImageMagick's, through the same profile.
 *
 * The reference is `magick -profile srgb.icc` over the tagged source, so this
 * compares against a color managed reader's answer rather than against our
 * own arithmetic rearranged. It is what catches a curve read in the wrong
 * direction: that produces a profile which round-trips against itself
 * perfectly and disagrees with everybody else.
 */
static int against_magick(void) {
    int failures = 0;

    static const char* const CASES[3][3] = {
        {"derived/base-display-p3.png",
         "derived/ref/base-display-p3-to-srgb.png",
         "derived/icc/display-p3.icc"},
        {"derived/base-adobe-rgb-1998.png",
         "derived/ref/base-adobe-rgb-1998-to-srgb.png",
         "derived/icc/adobe-rgb-1998.icc"},
        {"derived/base-rec2020.png", "derived/ref/base-rec2020-to-srgb.png",
         "derived/icc/rec2020.icc"}
    };

    for (uint32_t i = 0; i < 3u; i++) {
        TinyIccProfile profile;
        if (!load(&profile, CASES[i][2])) return failures + 1;

        size_t size = 0;
        unsigned char* tagged = readFixture(CASES[i][0], &size);
        if (!tagged) return failures + 1;

        TinyImage ours;
        memset(&ours, 0, sizeof(ours));

        int result = tiny_image_load(&ours, tagged, size);
        free(tagged);

        failures += assertEquals(result, 0);
        if (result != TINYIMG_OK) continue;

        failures += assertEquals(tiny_icc_convert_image(&ours, &profile), 0);

        size_t reference_size = 0;
        unsigned char* reference_bytes =
            readFixture(CASES[i][1], &reference_size);
        if (!reference_bytes) {
            tiny_image_destroy(&ours);
            return failures + 1;
        }

        TinyImage reference;
        memset(&reference, 0, sizeof(reference));

        result = tiny_image_load(&reference, reference_bytes, reference_size);
        free(reference_bytes);

        failures += assertEquals(result, 0);

        if (result == TINYIMG_OK) {
            failures += assertEquals(ours.width, reference.width);
            failures += assertEquals(ours.height, reference.height);

            if (ours.width == reference.width &&
                ours.channels == reference.channels) {
                // 42 dB is about one level of average error over the frame,
                // which is what two independent implementations of the same
                // matrix and curve reach; a wrong curve direction lands in the
                // teens and a wrong adaptation in the twenties
                failures += assertPSNR(
                    ours.data, reference.data,
                    (size_t) ours.width * ours.height * ours.channels, 42.0
                );
            }

            tiny_image_destroy(&reference);
        }

        tiny_image_destroy(&ours);
    }

    return failures;
}

/** Writes a big-endian 32-bit value. */
static void put32(unsigned char* at, uint32_t value) {
    at[0] = (unsigned char) (value >> 24);
    at[1] = (unsigned char) (value >> 16);
    at[2] = (unsigned char) (value >> 8);
    at[3] = (unsigned char) value;
}

/** Writes an s15Fixed16Number. */
static void putFixed(unsigned char* at, double value) {
    put32(
        at, (uint32_t) (int32_t) (value * 65536.0 + (value < 0 ? -0.5 : 0.5))
    );
}

/**
 * @brief Builds a minimal matrix profile with a caller-chosen curve tag.
 *
 * Our own generator only ever writes a sampled `curv` of 1024 points, so the
 * other two forms a matrix profile may legally carry are unreachable through
 * the fixtures. This assembles one by hand, which is the only way to reach
 * them: a profile in the wild carrying a `para` tag would otherwise decode
 * through a branch nothing here had run.
 *
 * @param out Receives the profile bytes; must hold at least 400.
 * @param curve The curve tag's body.
 * @param curve_size How long it is.
 * @return size_t The profile's length.
 */
static size_t buildProfile(
    unsigned char* out, const unsigned char* curve, size_t curve_size
) {
    // six: three primaries and three tone curves. all three curves point at
    // one block, which is what a real profile does when the channels share a
    // curve and is why the parser follows offsets rather than assuming order
    const uint32_t tags = 6;
    const uint32_t table = 132u + tags * 12u;
    uint32_t at = table;

    memset(out, 0, 400);

    // three XYZ tags then the shared curve, which all three channels point at
    static const char* const NAMES[3] = {"rXYZ", "gXYZ", "bXYZ"};
    static const double COLUMNS[3][3] = {
        {0.4361, 0.2225, 0.0139},
        {0.3851, 0.7169, 0.0971},
        {0.1431, 0.0606, 0.7142}
    };

    for (uint32_t i = 0; i < 3u; i++) {
        unsigned char* entry = out + 132u + i * 12u;

        memcpy(entry, NAMES[i], 4);
        put32(entry + 4, at);
        put32(entry + 8, 20);

        memcpy(out + at, "XYZ ", 4);
        for (uint32_t c = 0; c < 3u; c++) {
            putFixed(out + at + 8u + c * 4u, COLUMNS[i][c]);
        }

        at += 20u;
    }

    static const char* const CURVES[3] = {"rTRC", "gTRC", "bTRC"};

    for (uint32_t i = 0; i < 3u; i++) {
        unsigned char* entry = out + 132u + (3u + i) * 12u;

        memcpy(entry, CURVES[i], 4);
        put32(entry + 4, at);
        put32(entry + 8, (uint32_t) curve_size);
    }

    memcpy(out + at, curve, curve_size);
    at += (uint32_t) curve_size;

    put32(out + 128, tags);
    put32(out, at);

    return at;
}

/**
 * @brief The three curve forms a matrix profile may carry all parse.
 *
 * A sampled table, a single-entry table which is a gamma rather than a sample,
 * and a parametric function. The middle one is the trap: reading it as a
 * one-point table gives a curve that is flat at whatever that byte pair
 * happened to be, and the profile still parses.
 */
static int curve_forms(void) {
    int failures = 0;
    unsigned char profile[400];
    TinyIccProfile parsed;

    // a single-entry curv, which is a u8Fixed8 gamma of 2.2
    unsigned char single[14];
    memset(single, 0, sizeof(single));
    memcpy(single, "curv", 4);
    put32(single + 8, 1);
    single[12] = 2;
    single[13] = 51;

    size_t size = buildProfile(profile, single, sizeof(single));

    failures += assertEquals(tiny_icc_parse(&parsed, profile, size), 0);

    // 2.2 read as a gamma gives 0.5^2.2 at the midpoint; read as a one point
    // table it would be flat at 563/65535, which is nothing like it
    failures += assertFloatEquals(parsed.curve[0][128], 0.2176f, 0.004f);
    failures += assertFloatEquals(parsed.curve[0][255], 1.0f, 0.002f);
    failures += assertFalse(parsed.linear);

    // an empty curv is the identity, which is what a linear profile carries
    unsigned char empty[12];
    memset(empty, 0, sizeof(empty));
    memcpy(empty, "curv", 4);

    size = buildProfile(profile, empty, sizeof(empty));

    failures += assertEquals(tiny_icc_parse(&parsed, profile, size), 0);
    failures +=
        assertFloatEquals(parsed.curve[0][128], 128.0f / 255.0f, 0.002f);
    failures += assertTrue(parsed.linear);

    // a type 0 para, which is a bare exponent
    unsigned char para[16];
    memset(para, 0, sizeof(para));
    memcpy(para, "para", 4);
    para[8] = 0;
    para[9] = 0;
    putFixed(para + 12, 2.2);

    size = buildProfile(profile, para, sizeof(para));

    failures += assertEquals(tiny_icc_parse(&parsed, profile, size), 0);
    failures += assertFloatEquals(parsed.curve[0][128], 0.2176f, 0.004f);

    // a type 3 para, which is the sRGB shape: gamma, a, b, c, d
    unsigned char srgb_para[32];
    memset(srgb_para, 0, sizeof(srgb_para));
    memcpy(srgb_para, "para", 4);
    srgb_para[9] = 3;
    putFixed(srgb_para + 12, 2.4);
    putFixed(srgb_para + 16, 1.0 / 1.055);
    putFixed(srgb_para + 20, 0.055 / 1.055);
    putFixed(srgb_para + 24, 1.0 / 12.92);
    putFixed(srgb_para + 28, 0.04045);

    size = buildProfile(profile, srgb_para, sizeof(srgb_para));

    failures += assertEquals(tiny_icc_parse(&parsed, profile, size), 0);

    // the parametric sRGB and the sampled one agree, which is the check that
    // the parameters were read in the right order
    failures += assertFloatEquals(parsed.curve[0][128], 0.2158f, 0.006f);
    failures += assertFloatEquals(parsed.curve[0][10], 0.00304f, 0.002f);

    // a para naming a type that does not exist is not a curve
    unsigned char bad[16];
    memset(bad, 0, sizeof(bad));
    memcpy(bad, "para", 4);
    bad[9] = 9;

    size = buildProfile(profile, bad, sizeof(bad));
    failures += assertEquals(
        tiny_icc_parse(&parsed, profile, size), TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    // and a tag that is neither form is not a curve either
    unsigned char nonsense[16];
    memset(nonsense, 0, sizeof(nonsense));
    memcpy(nonsense, "junk", 4);

    size = buildProfile(profile, nonsense, sizeof(nonsense));
    failures += assertEquals(
        tiny_icc_parse(&parsed, profile, size), TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    // a curv whose sample count runs past the tag is truncated, not trusted
    unsigned char lying[12];
    memset(lying, 0, sizeof(lying));
    memcpy(lying, "curv", 4);
    put32(lying + 8, 1000);

    size = buildProfile(profile, lying, sizeof(lying));
    failures += assertEquals(
        tiny_icc_parse(&parsed, profile, size), TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    // a single-entry curv with a zero gamma has no curve in it
    unsigned char zero[14];
    memset(zero, 0, sizeof(zero));
    memcpy(zero, "curv", 4);
    put32(zero + 8, 1);

    size = buildProfile(profile, zero, sizeof(zero));
    failures += assertEquals(
        tiny_icc_parse(&parsed, profile, size), TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    return failures;
}

/** A profile that is not a matrix profile is refused with its own error. */
static int refuses_what_it_cannot_read(void) {
    int failures = 0;
    TinyIccProfile profile;

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/icc/srgb.icc", &size);
    if (!bytes) return 1;

    failures += assertEquals(
        tiny_icc_parse(&profile, bytes, size - 1u), TINYIMG_ERR_CORRUPT
    );
    failures +=
        assertEquals(tiny_icc_parse(&profile, bytes, 100), TINYIMG_ERR_CORRUPT);
    failures += assertEquals(tiny_icc_parse(0, bytes, size), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_icc_parse(&profile, 0, size), TINYIMG_ERR_NULL);

    // an A2B0 tag makes it a lookup profile, which is refused rather than
    // approximated through whatever matrix tags it also happens to carry.
    // written by renaming the description tag, so everything else about the
    // profile is still valid and only the one signature decides
    uint32_t count = ((uint32_t) bytes[128] << 24) |
                     ((uint32_t) bytes[129] << 16) |
                     ((uint32_t) bytes[130] << 8) | bytes[131];
    failures += assertGreaterThan(count, 0);

    bytes[132] = 'A';
    bytes[133] = '2';
    bytes[134] = 'B';
    bytes[135] = '0';

    failures += assertEquals(
        tiny_icc_parse(&profile, bytes, size), TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    free(bytes);

    // a profile with a valid header and no tags at all has no matrix, which is
    // the other thing that makes it unreadable rather than corrupt
    unsigned char header[132];
    memset(header, 0, sizeof(header));
    header[3] = 132;

    failures += assertEquals(
        tiny_icc_parse(&profile, header, sizeof(header)),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += parses();
    failures += gamuts_are_ordered();
    failures += srgb_is_the_identity();
    failures += conversion_moves_the_right_way();
    failures += image_matches_pixel();
    failures += matrix_between();
    failures += curve_forms();
    failures += against_magick();
    failures += refuses_what_it_cannot_read();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
