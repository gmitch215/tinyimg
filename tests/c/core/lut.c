#include "../test.h"
#include "tinyimg/util.h"

int main(void) {
    int r = 0;

    uint8_t identity[256];
    tiny_lut_identity(identity);

    int exact = 1;
    for (int i = 0; i < 256; i++) {
        if (identity[i] != (uint8_t) i) exact = 0;
    }
    r |= assertTrue(exact);

    // black and white are fixed points of any gamma curve
    uint8_t darker[256];
    tiny_lut_gamma(darker, 2.2f);
    r |= assertEquals((long) darker[0], 0L);
    r |= assertEquals((long) darker[255], 255L);

    // an exponent above one pulls the midtones down, matching the sense
    // tiny_image_gamma_correction documents
    r |= assertLessThan((double) darker[128], 128.0);

    uint8_t lighter[256];
    tiny_lut_gamma(lighter, 1.0f / 2.2f);
    r |= assertGreaterThan((double) lighter[128], 128.0);

    // the whole table against libm, so the curve is checked rather than one
    // sample of it
    int worstCurve = 0;
    for (int i = 0; i < 256; i++) {
        int wantDark = (int) (pow((double) i / 255.0, 2.2) * 255.0 + 0.5);
        int wantLight =
            (int) (pow((double) i / 255.0, 1.0 / 2.2) * 255.0 + 0.5);

        int driftDark = abs((int) darker[i] - wantDark);
        int driftLight = abs((int) lighter[i] - wantLight);

        if (driftDark > worstCurve) worstCurve = driftDark;
        if (driftLight > worstCurve) worstCurve = driftLight;
    }
    r |= assertLessThan((double) worstCurve, 2.0);

    // an exponent of one is the identity, so an image with no gamma change is
    // untouched rather than requantized
    uint8_t neutral[256];
    tiny_lut_gamma(neutral, 1.0f);
    r |= assertBytesMatch(neutral, identity, 256);

    // an invalid exponent falls back to the identity instead of producing a
    // table of zeroes
    uint8_t invalid[256];
    tiny_lut_gamma(invalid, 0.0f);
    r |= assertBytesMatch(invalid, identity, 256);
    tiny_lut_gamma(invalid, -1.0f);
    r |= assertBytesMatch(invalid, identity, 256);

    // composition is what lets a chain of point operations collapse into one
    // table, so it has to apply in the order the chain did
    uint8_t composed[256];
    tiny_lut_compose(composed, identity, darker);
    r |= assertBytesMatch(composed, darker, 256);

    tiny_lut_compose(composed, darker, identity);
    r |= assertBytesMatch(composed, darker, 256);

    // inverse curves compose back towards identity, but not exactly: an eight
    // bit intermediate crushes the shadows, where a gamma of 2.2 maps the first
    // fifteen input levels onto zero and the inverse cannot get them back. this
    // is the measurement behind collapsing a chain into one table rather than
    // applying each in turn
    tiny_lut_compose(composed, darker, lighter);

    int worstShadow = 0;
    int worstAbove = 0;

    for (int i = 0; i < 256; i++) {
        int drift = (int) composed[i] - i;
        if (drift < 0) drift = -drift;

        if (i < 32) {
            if (drift > worstShadow) worstShadow = drift;
        }
        else if (drift > worstAbove) {
            worstAbove = drift;
        }
    }

    r |= assertEquals((long) worstShadow, 14L);
    r |= assertLessThan((double) worstAbove, 3.0);

    // aliasing the first table is supported, so a chain can accumulate into one
    // buffer without a scratch copy
    uint8_t aliased[256];
    memcpy(aliased, darker, 256);
    tiny_lut_compose(aliased, aliased, lighter);
    r |= assertBytesMatch(aliased, composed, 256);

    tiny_lut_identity(0);
    tiny_lut_gamma(0, 2.2f);
    tiny_lut_compose(0, identity, identity);
    tiny_lut_compose(composed, 0, identity);

    return r;
}
