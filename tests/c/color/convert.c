#include <math.h>

#include "tinyimg/color.h"
#include "tinyimg/memory.h"

#include "test.h"

/** Every RGB triple in a coarse lattice, round tripped through a space. */
static int round_trips(void) {
    int failures = 0;
    uint32_t worst_hsv = 0;
    uint32_t worst_hsl = 0;
    uint32_t worst_cmyk = 0;

    for (uint32_t r = 0; r < 256u; r += 5u) {
        for (uint32_t g = 0; g < 256u; g += 5u) {
            for (uint32_t b = 0; b < 256u; b += 5u) {
                float h;
                float s;
                float v;
                uint8_t back[3];

                tiny_rgb_to_hsv(
                    (uint8_t) r, (uint8_t) g, (uint8_t) b, &h, &s, &v
                );
                tiny_hsv_to_rgb(h, s, v, &back[0], &back[1], &back[2]);

                uint32_t in[3] = {r, g, b};

                for (uint32_t c = 0; c < 3u; c++) {
                    uint32_t diff =
                        back[c] > in[c] ? back[c] - in[c] : in[c] - back[c];
                    if (diff > worst_hsv) worst_hsv = diff;
                }

                float l;
                tiny_rgb_to_hsl(
                    (uint8_t) r, (uint8_t) g, (uint8_t) b, &h, &s, &l
                );
                tiny_hsl_to_rgb(h, s, l, &back[0], &back[1], &back[2]);

                for (uint32_t c = 0; c < 3u; c++) {
                    uint32_t diff =
                        back[c] > in[c] ? back[c] - in[c] : in[c] - back[c];
                    if (diff > worst_hsl) worst_hsl = diff;
                }

                float cy;
                float m;
                float ye;
                float k;

                tiny_rgb_to_cmyk(
                    (uint8_t) r, (uint8_t) g, (uint8_t) b, &cy, &m, &ye, &k
                );
                tiny_cmyk_to_rgb(cy, m, ye, k, &back[0], &back[1], &back[2]);

                for (uint32_t c = 0; c < 3u; c++) {
                    uint32_t diff =
                        back[c] > in[c] ? back[c] - in[c] : in[c] - back[c];
                    if (diff > worst_cmyk) worst_cmyk = diff;
                }
            }
        }
    }

    // all three are exactly invertible in the reals, so the only error is the
    // one rounding to a byte costs
    failures += assertLessThan(worst_hsv, 2);
    failures += assertLessThan(worst_hsl, 2);
    failures += assertLessThan(worst_cmyk, 2);

    return failures;
}

/** The named corners of each space are where they should be. */
static int landmarks(void) {
    int failures = 0;
    float h;
    float s;
    float v;
    float l;

    // pure red is hue zero, full saturation, full value
    tiny_rgb_to_hsv(255, 0, 0, &h, &s, &v);
    failures += assertFloatEquals(h, 0.0f, 0.01f);
    failures += assertFloatEquals(s, 1.0f, 0.01f);
    failures += assertFloatEquals(v, 1.0f, 0.01f);

    tiny_rgb_to_hsv(0, 255, 0, &h, &s, &v);
    failures += assertFloatEquals(h, 120.0f, 0.01f);

    tiny_rgb_to_hsv(0, 0, 255, &h, &s, &v);
    failures += assertFloatEquals(h, 240.0f, 0.01f);

    // gray has no hue and no saturation, at any level
    tiny_rgb_to_hsv(128, 128, 128, &h, &s, &v);
    failures += assertFloatEquals(s, 0.0f, 0.01f);
    failures += assertFloatEquals(v, 128.0f / 255.0f, 0.01f);

    // HSL and HSV differ on where full saturation lives: pure red is L = 0.5
    // in HSL and V = 1 in HSV, and that is the whole difference between them
    tiny_rgb_to_hsl(255, 0, 0, &h, &s, &l);
    failures += assertFloatEquals(l, 0.5f, 0.01f);
    failures += assertFloatEquals(s, 1.0f, 0.01f);

    // and white is fully light with no saturation in HSL, where in HSV it is
    // full value with no saturation
    tiny_rgb_to_hsl(255, 255, 255, &h, &s, &l);
    failures += assertFloatEquals(l, 1.0f, 0.01f);
    failures += assertFloatEquals(s, 0.0f, 0.01f);

    tiny_rgb_to_hsl(200, 200, 200, &h, &s, &l);
    failures += assertFloatEquals(s, 0.0f, 0.01f);

    // CMYK: pure red is no cyan, full magenta and yellow, no key
    float cy;
    float m;
    float ye;
    float k;

    tiny_rgb_to_cmyk(255, 0, 0, &cy, &m, &ye, &k);
    failures += assertFloatEquals(cy, 0.0f, 0.01f);
    failures += assertFloatEquals(m, 1.0f, 0.01f);
    failures += assertFloatEquals(ye, 1.0f, 0.01f);
    failures += assertFloatEquals(k, 0.0f, 0.01f);

    // and black is all key with no hue recorded, rather than whatever a
    // division by the zero ink level would have left behind
    tiny_rgb_to_cmyk(0, 0, 0, &cy, &m, &ye, &k);
    failures += assertFloatEquals(k, 1.0f, 0.01f);
    failures += assertFloatEquals(cy, 0.0f, 0.01f);
    failures += assertFloatEquals(m, 0.0f, 0.01f);
    failures += assertFloatEquals(ye, 0.0f, 0.01f);

    // grayscale both ways
    failures += assertEquals(tiny_rgb_to_grayscale(255, 255, 255), 255);
    failures += assertEquals(tiny_rgb_to_grayscale(0, 0, 0), 0);

    // Rec. 709 weights, so green counts for most of it
    failures +=
        assertIn((double) tiny_rgb_to_grayscale(0, 255, 0), 181.0, 184.0);
    failures += assertIn((double) tiny_rgb_to_grayscale(255, 0, 0), 53.0, 56.0);
    failures += assertIn((double) tiny_rgb_to_grayscale(0, 0, 255), 17.0, 20.0);

    uint8_t r;
    uint8_t g;
    uint8_t b;

    failures += assertEquals(tiny_grayscale_to_rgb(90, &r, &g, &b), 0);
    failures += assertEquals(r, 90);
    failures += assertEquals(g, 90);
    failures += assertEquals(b, 90);

    return failures;
}

/** A hue wrapping past 360 lands where the unwrapped angle would. */
static int hue_wrapping(void) {
    int failures = 0;
    uint8_t a[3];
    uint8_t b[3];

    tiny_hsv_to_rgb(30.0f, 1.0f, 1.0f, &a[0], &a[1], &a[2]);
    tiny_hsv_to_rgb(390.0f, 1.0f, 1.0f, &b[0], &b[1], &b[2]);
    failures += assertBytesMatch(a, b, 3);

    tiny_hsv_to_rgb(-330.0f, 1.0f, 1.0f, &b[0], &b[1], &b[2]);
    failures += assertBytesMatch(a, b, 3);

    tiny_hsl_to_rgb(200.0f, 0.7f, 0.4f, &a[0], &a[1], &a[2]);
    tiny_hsl_to_rgb(560.0f, 0.7f, 0.4f, &b[0], &b[1], &b[2]);
    failures += assertBytesMatch(a, b, 3);

    return failures;
}

/**
 * @brief Interpolation between two hues takes the short way round.
 *
 * A hue is an angle, so 350 and 10 are twenty degrees apart. Treating them as
 * plain numbers walks the long way and passes through every other hue, which
 * shows as a rainbow smear in what should be a short red to red blend.
 */
static int interpolation(void) {
    int failures = 0;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    failures += assertEquals(
        tiny_interpolate_rgb(0, 0, 0, 200, 100, 50, 0.5f, &r, &g, &b), 0
    );
    failures += assertEquals(r, 100);
    failures += assertEquals(g, 50);
    failures += assertEquals(b, 25);

    failures += assertEquals(
        tiny_interpolate_rgb(0, 0, 0, 200, 100, 50, 0.0f, &r, &g, &b), 0
    );
    failures += assertEquals(r, 0);

    failures += assertEquals(
        tiny_interpolate_rgb(0, 0, 0, 200, 100, 50, 1.0f, &r, &g, &b), 0
    );
    failures += assertEquals(r, 200);

    failures += assertEquals(
        tiny_interpolate_rgb(0, 0, 0, 1, 1, 1, 2.0f, &r, &g, &b),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_interpolate_rgb(0, 0, 0, 1, 1, 1, 0.5f, 0, &g, &b),
        TINYIMG_ERR_NULL
    );

    float h;
    float s;
    float v;

    // halfway from 350 to 10 is zero, not 180
    failures += assertEquals(
        tiny_interpolate_hsv(
            350.0f, 1.0f, 1.0f, 10.0f, 1.0f, 1.0f, 0.5f, &h, &s, &v
        ),
        0
    );
    failures += assertFloatEquals(h, 0.0f, 0.01f);

    // and the other way round is the same point
    failures += assertEquals(
        tiny_interpolate_hsv(
            10.0f, 1.0f, 1.0f, 350.0f, 1.0f, 1.0f, 0.5f, &h, &s, &v
        ),
        0
    );
    failures += assertFloatEquals(h, 0.0f, 0.01f);

    // a genuine half turn goes one way or the other, and either is 180 away
    failures += assertEquals(
        tiny_interpolate_hsv(
            0.0f, 1.0f, 1.0f, 180.0f, 1.0f, 1.0f, 0.5f, &h, &s, &v
        ),
        0
    );
    failures +=
        assertTrue(fabsf(h - 90.0f) < 0.01f || fabsf(h - 270.0f) < 0.01f);

    // the result always lands in the circle
    for (uint32_t i = 0; i <= 20u; i++) {
        float t = (float) i / 20.0f;

        tiny_interpolate_hsv(
            300.0f, 0.5f, 0.5f, 60.0f, 0.5f, 0.5f, t, &h, &s, &v
        );

        failures += assertTrue(h >= 0.0f && h < 360.0f);
    }

    float l;
    failures += assertEquals(
        tiny_interpolate_hsl(
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.25f, &h, &s, &l
        ),
        0
    );
    failures += assertFloatEquals(s, 0.25f, 0.01f);
    failures += assertFloatEquals(l, 0.25f, 0.01f);

    failures += assertEquals(
        tiny_interpolate_hsl(
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, -1.0f, &h, &s, &l
        ),
        TINYIMG_ERR_RANGE
    );

    return failures;
}

/** A gradient starts and ends at its endpoints and is evenly spaced. */
static int gradients(void) {
    int failures = 0;
    TinyArenaMark mark;

    tiny_arena_mark(&mark);

    int* ramp = tiny_gradient_rgb(0, 0, 0, 255, 255, 255, 5);
    failures += assertNotNull(ramp);

    if (ramp) {
        failures += assertEquals(ramp[0], 0);
        failures += assertEquals(ramp[4], 0xFFFFFF);

        // five even steps of 255, so the middle is 127 or 128
        uint32_t middle = (uint32_t) ramp[2] & 0xFFu;
        failures += assertIn((double) middle, 127.0, 128.0);
    }

    // below two steps has no gradient in it
    failures += assertNull(tiny_gradient_rgb(0, 0, 0, 1, 1, 1, 1));
    failures += assertNull(tiny_gradient_rgb(0, 0, 0, 1, 1, 1, 0));

    int* with_alpha = tiny_gradient_rgba(0, 0, 0, 0, 255, 255, 255, 255, 3);
    failures += assertNotNull(with_alpha);

    if (with_alpha) {
        failures += assertEquals((uint32_t) with_alpha[0], 0u);
        failures += assertEquals((uint32_t) with_alpha[2], 0xFFFFFFFFu);
    }

    float* hsv = tiny_gradient_hsv(0.0f, 1.0f, 1.0f, 120.0f, 1.0f, 1.0f, 3);
    failures += assertNotNull(hsv);

    if (hsv) {
        failures += assertFloatEquals(hsv[0], 0.0f, 0.01f);
        failures += assertFloatEquals(hsv[3], 60.0f, 0.01f);
        failures += assertFloatEquals(hsv[6], 120.0f, 0.01f);
    }

    float* cmyk =
        tiny_gradient_cmyk(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 5);
    failures += assertNotNull(cmyk);

    if (cmyk) {
        failures += assertFloatEquals(cmyk[0], 0.0f, 0.001f);
        failures += assertFloatEquals(cmyk[4 * 4], 1.0f, 0.001f);
        failures += assertFloatEquals(cmyk[2 * 4], 0.5f, 0.001f);
    }

    tiny_arena_release(&mark);
    return failures;
}

/** A multi-gradient passes through every stop and has no seam. */
static int multigradients(void) {
    int failures = 0;
    TinyArenaMark mark;

    tiny_arena_mark(&mark);

    // black, red, white: nine steps, so each leg gets four plus a shared end
    uint8_t stops[9] = {0, 0, 0, 255, 0, 0, 255, 255, 255};
    int* ramp = tiny_multigradient_rgb(stops, 3, 9);

    failures += assertNotNull(ramp);

    if (ramp) {
        failures += assertEquals((uint32_t) ramp[0], 0u);
        failures += assertEquals((uint32_t) ramp[8], 0xFFFFFFu);

        // the middle entry is the middle stop, so both legs really are
        // traversed and the join is exactly on the stop
        failures += assertEquals((uint32_t) ramp[4], 0xFF0000u);

        // red rises monotonically through the first leg and stays at full
        // through the second, which is what makes the join seamless
        uint32_t previous = 0;

        for (uint32_t i = 0; i < 9u; i++) {
            uint32_t red = ((uint32_t) ramp[i] >> 16) & 0xFFu;

            failures += assertTrue(red >= previous);
            previous = red;
        }
    }

    // an even number of steps has no entry exactly on the middle stop, and
    // that is fine: what must not happen is a jump at the join
    int* even = tiny_multigradient_rgb(stops, 3, 8);
    failures += assertNotNull(even);

    if (even) {
        int32_t biggest = 0;

        for (uint32_t i = 1; i < 8u; i++) {
            int32_t before = (int32_t) (((uint32_t) even[i - 1u] >> 8) & 0xFFu);
            int32_t after = (int32_t) (((uint32_t) even[i] >> 8) & 0xFFu);
            int32_t step = after - before;

            if (step < 0) step = -step;
            if (step > biggest) biggest = step;
        }

        // the green channel is flat then rising, so the largest single step is
        // one leg's worth; a leg boundary that rounded badly would double it
        failures += assertLessThan(biggest, 90);
    }

    failures += assertNull(tiny_multigradient_rgb(stops, 1, 5));
    failures += assertNull(tiny_multigradient_rgb(0, 3, 5));
    failures += assertNull(tiny_multigradient_rgb(stops, 3, 1));

    // the sized form weights the legs, so a leg given nine tenths of the
    // gradient occupies nine tenths of it
    float weights[2] = {0.9f, 0.1f};
    int* weighted = tiny_multigradient_rgb_sized(stops, 3, 11, weights);

    failures += assertNotNull(weighted);

    if (weighted) {
        // at halfway the first leg is still running, so red is short of full
        // and green is still nothing
        uint32_t red = ((uint32_t) weighted[5] >> 16) & 0xFFu;
        uint32_t green = ((uint32_t) weighted[5] >> 8) & 0xFFu;

        failures += assertLessThan(red, 200);
        failures += assertEquals(green, 0);

        failures += assertEquals((uint32_t) weighted[10], 0xFFFFFFu);
    }

    // a distribution that does not sum to one is refused, and so is a NULL one
    float bad[2] = {0.9f, 0.9f};
    failures += assertNull(tiny_multigradient_rgb_sized(stops, 3, 11, bad));
    failures += assertNull(tiny_multigradient_rgb_sized(stops, 3, 11, 0));

    float hsv_stops[9] = {0.0f, 1.0f,   1.0f, 120.0f, 1.0f,
                          1.0f, 240.0f, 1.0f, 1.0f};
    float* hues = tiny_multigradient_hsv(hsv_stops, 3, 5);

    failures += assertNotNull(hues);

    if (hues) {
        failures += assertFloatEquals(hues[0], 0.0f, 0.01f);
        failures += assertFloatEquals(hues[2 * 3], 120.0f, 0.01f);
        failures += assertFloatEquals(hues[4 * 3], 240.0f, 0.01f);
    }

    float cmyk_stops[8] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float* inks = tiny_multigradient_cmyk(cmyk_stops, 2, 3);

    failures += assertNotNull(inks);

    if (inks) {
        failures += assertFloatEquals(inks[0], 0.0f, 0.001f);
        failures += assertFloatEquals(inks[4], 0.5f, 0.001f);
        failures += assertFloatEquals(inks[8], 1.0f, 0.001f);
    }

    float* lightness = tiny_multigradient_hsl(hsv_stops, 3, 4);
    failures += assertNotNull(lightness);

    uint8_t rgba_stops[8] = {0, 0, 0, 0, 255, 255, 255, 255};
    int* faded = tiny_multigradient_rgba(rgba_stops, 2, 3);

    failures += assertNotNull(faded);
    if (faded) failures += assertEquals((uint32_t) faded[2], 0xFFFFFFFFu);

    float one[1] = {1.0f};
    failures +=
        assertNotNull(tiny_multigradient_rgba_sized(rgba_stops, 2, 3, one));
    failures +=
        assertNotNull(tiny_multigradient_hsv_sized(hsv_stops, 2, 3, one));
    failures +=
        assertNotNull(tiny_multigradient_hsl_sized(hsv_stops, 2, 3, one));
    failures +=
        assertNotNull(tiny_multigradient_cmyk_sized(cmyk_stops, 2, 3, one));

    tiny_arena_release(&mark);
    return failures;
}

/** Out of range inputs are refused rather than clamped silently. */
static int ranges(void) {
    int failures = 0;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    failures += assertEquals(
        tiny_hsv_to_rgb(0.0f, 2.0f, 1.0f, &r, &g, &b), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_hsv_to_rgb(0.0f, 1.0f, -1.0f, &r, &g, &b), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_hsl_to_rgb(0.0f, 1.0f, 2.0f, &r, &g, &b), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_cmyk_to_rgb(2.0f, 0.0f, 0.0f, 0.0f, &r, &g, &b), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_cmyk_to_rgb(0.0f, 0.0f, 0.0f, 2.0f, &r, &g, &b), TINYIMG_ERR_RANGE
    );

    float h;
    failures +=
        assertEquals(tiny_rgb_to_hsv(0, 0, 0, 0, &h, &h), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_rgb_to_hsl(0, 0, 0, &h, 0, &h), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_rgb_to_cmyk(0, 0, 0, &h, &h, &h, 0), TINYIMG_ERR_NULL
    );
    failures +=
        assertEquals(tiny_grayscale_to_rgb(0, 0, &g, &b), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_hsv_to_rgb(0.0f, 0.0f, 0.0f, 0, &g, &b), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_hsl_to_rgb(0.0f, 0.0f, 0.0f, 0, &g, &b), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_cmyk_to_rgb(0.0f, 0.0f, 0.0f, 0.0f, 0, &g, &b), TINYIMG_ERR_NULL
    );

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += round_trips();
    failures += landmarks();
    failures += hue_wrapping();
    failures += interpolation();
    failures += gradients();
    failures += multigradients();
    failures += ranges();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
