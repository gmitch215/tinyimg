#include "../test.h"
#include "tinyimg/util.h"

// libm is the reference. one assertion per function against the accuracy the
// header promises, rather than a numbered line per sample point
static double worstRelative(
    float (*ours)(float), double (*reference)(double), float lo, float hi,
    int steps
) {
    double worst = 0.0;

    for (int i = 0; i <= steps; i++) {
        float x = lo + (hi - lo) * ((float) i / (float) steps);

        double want = reference((double) x);
        double got = (double) ours(x);
        double scale = fabs(want) > 1e-9 ? fabs(want) : 1.0;
        double error = fabs(got - want) / scale;

        if (error > worst) worst = error;
    }

    return worst;
}

static double worstAbsolute(
    float (*ours)(float), double (*reference)(double), float lo, float hi,
    int steps
) {
    double worst = 0.0;

    for (int i = 0; i <= steps; i++) {
        float x = lo + (hi - lo) * ((float) i / (float) steps);
        double error = fabs((double) ours(x) - reference((double) x));

        if (error > worst) worst = error;
    }

    return worst;
}

static double worstPow(float lo, float hi, float exponent, int steps) {
    double worst = 0.0;

    for (int i = 1; i <= steps; i++) {
        float x = lo + (hi - lo) * ((float) i / (float) steps);

        double want = pow((double) x, (double) exponent);
        double got = (double) tiny_powf(x, exponent);
        double scale = fabs(want) > 1e-9 ? fabs(want) : 1.0;
        double error = fabs(got - want) / scale;

        if (error > worst) worst = error;
    }

    return worst;
}

int main(void) {
    int r = 0;

    // exact, because each is one wasm instruction
    r |= assertFloatEquals(tiny_fabsf(-3.5f), 3.5f, 0.0f);
    r |= assertFloatEquals(tiny_sqrtf(144.0f), 12.0f, 0.0f);
    r |= assertFloatEquals(tiny_floorf(-2.5f), -3.0f, 0.0f);
    r |= assertFloatEquals(tiny_ceilf(-2.5f), -2.0f, 0.0f);
    r |= assertFloatEquals(tiny_truncf(-2.9f), -2.0f, 0.0f);

    // halfway away from zero in both directions, which is what round means and
    // rint would not give
    r |= assertFloatEquals(tiny_roundf(2.5f), 3.0f, 0.0f);
    r |= assertFloatEquals(tiny_roundf(-2.5f), -3.0f, 0.0f);
    r |= assertFloatEquals(tiny_roundf(2.4f), 2.0f, 0.0f);

    r |= assertFloatEquals(tiny_fmodf(7.5f, 2.0f), 1.5f, 1e-6f);
    r |= assertFloatEquals(tiny_fmodf(-7.5f, 2.0f), -1.5f, 1e-6f);
    r |= assertFloatEquals(tiny_fmodf(370.0f, 360.0f), 10.0f, 1e-4f);

    // the bounds each function's header states. float eps is 1.2e-7, so these
    // are two to twelve units in the last place: correct single precision, and
    // low enough that the 0-255 quantization that follows swamps them
    r |= assertLessThan(
        worstRelative(tiny_expf, exp, -30.0f, 30.0f, 6001), 3e-7
    );
    r |= assertLessThan(worstRelative(tiny_expf, exp, -1.0f, 1.0f, 2001), 3e-7);
    r |= assertLessThan(worstRelative(tiny_logf, log, 1e-6f, 1.0f, 4001), 5e-7);
    r |= assertLessThan(worstRelative(tiny_logf, log, 1.0f, 1e6f, 4001), 5e-7);

    // absolute is the measure that means something for a log, whose relative
    // error is unbounded as the result approaches zero at x = 1
    r |= assertLessThan(worstAbsolute(tiny_logf, log, 1e-6f, 1e6f, 8001), 1e-6);

    r |= assertLessThan(
        worstAbsolute(tiny_sinf, sin, -20.0f, 20.0f, 8001), 2e-7
    );
    r |= assertLessThan(
        worstAbsolute(tiny_cosf, cos, -20.0f, 20.0f, 8001), 2e-7
    );

    // pow carries log's error multiplied by the exponent, which is why its
    // bound is an order of magnitude looser than either
    r |= assertLessThan(worstPow(0.0f, 1.0f, 2.2f, 2001), 2e-6);
    r |= assertLessThan(worstPow(0.0f, 1.0f, 1.0f / 2.2f, 2001), 1e-6);
    r |= assertLessThan(worstPow(0.0f, 255.0f, 0.5f, 2001), 1e-6);

    // saturation replaces subnormals rather than returning garbage
    r |= assertFloatEquals(tiny_expf(-200.0f), 0.0f, 0.0f);
    r |= assertTrue(isinf(tiny_expf(200.0f)));

    r |= assertTrue(isinf(tiny_logf(0.0f)) && tiny_logf(0.0f) < 0.0f);
    r |= assertTrue(isnan(tiny_logf(-1.0f)));

    // a subnormal input is scaled into the normals rather than read as zero.
    // the reference has to be the log of the float, not of the literal: a
    // subnormal near 1e-42 carries about ten bits, so the two differ by more
    // than the tolerance being checked
    float subnormal = 1e-42f;
    r |= assertFloatEquals(
        tiny_logf(subnormal), (float) log((double) subnormal), 1e-4f
    );

    r |= assertFloatEquals(tiny_powf(2.0f, 10.0f), 1024.0f, 0.01f);
    r |= assertFloatEquals(tiny_powf(5.0f, 0.0f), 1.0f, 0.0f);
    r |= assertFloatEquals(tiny_powf(0.0f, 2.0f), 0.0f, 0.0f);
    r |= assertFloatEquals(tiny_powf(-2.0f, 3.0f), -8.0f, 1e-4f);
    r |= assertFloatEquals(tiny_powf(-2.0f, 2.0f), 4.0f, 1e-4f);
    r |= assertTrue(isnan(tiny_powf(-2.0f, 0.5f)));

    r |= assertEquals(tiny_clamp_u8(-5), 0);
    r |= assertEquals(tiny_clamp_u8(300), 255);
    r |= assertEquals(tiny_clamp_u8(128), 128);
    r |= assertEquals(tiny_clamp_u8f(-0.4f), 0);
    r |= assertEquals(tiny_clamp_u8f(254.6f), 255);
    r |= assertEquals(tiny_clamp_u8f(127.5f), 128);
    r |= assertEquals(tiny_clamp_u8f(1000.0f), 255);

    r |= assertFloatEquals(tiny_clampf(5.0f, 0.0f, 1.0f), 1.0f, 0.0f);
    r |= assertEquals(tiny_clampi(-7, -3, 3), -3);
    r |= assertEquals((long) tiny_min_u32(4, 9), 4);
    r |= assertEquals((long) tiny_max_u32(4, 9), 9);

    return r;
}
