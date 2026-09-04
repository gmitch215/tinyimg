#include "test.h"

/** An image whose luminance sits in a narrow band with an off balance. */
static int make_flat(
    TinyImage* image, uint8_t low, uint8_t high, int32_t red_shift
) {
    if (tiny_image_create(image, 64, 64, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 64u; x++) {
            uint8_t* p = image->data + ((size_t) y * 64u + x) * 3u;
            uint32_t span = high - low;
            uint8_t value = (uint8_t) (low + (x * span) / 63u);

            p[0] = tiny_clamp_u8((int32_t) value + red_shift);
            p[1] = value;
            p[2] = value;
        }
    }

    return 1;
}

/** The lowest and highest level a channel reaches. */
static void extent_of(
    const TinyImage* image, uint8_t channel, uint32_t* low, uint32_t* high
) {
    uint32_t bins[256];

    tiny_image_histogram(image, channel, bins);

    *low = 255;
    *high = 0;

    for (uint32_t i = 0; i < 256u; i++) {
        if (bins[i] == 0u) continue;
        if (i < *low) *low = i;
        if (i > *high) *high = i;
    }
}

/** A channel's mean. */
static double mean_of(const TinyImage* image, uint8_t channel) {
    uint32_t bins[256];
    uint64_t sum = 0;
    uint64_t total = 0;

    tiny_image_histogram(image, channel, bins);

    for (uint32_t i = 0; i < 256u; i++) {
        sum += (uint64_t) bins[i] * i;
        total += bins[i];
    }

    return total ? (double) sum / (double) total : 0.0;
}

/** Auto contrast opens the range and leaves the balance alone. */
static int contrast(void) {
    int failures = 0;
    TinyImage image;

    if (!make_flat(&image, 90, 150, 30)) return 1;

    double red_before = mean_of(&image, 0) - mean_of(&image, 1);

    failures += assertEquals(tiny_image_auto_contrast(&image), 0);

    uint32_t low;
    uint32_t high;
    extent_of(&image, 1, &low, &high);

    // the range opens out toward both ends
    failures += assertLessThan(low, 30);
    failures += assertGreaterThan(high, 225);

    // and the cast survives, because the luminance was stretched rather than
    // each channel separately. that is the difference from auto levels
    double red_after = mean_of(&image, 0) - mean_of(&image, 1);
    failures += assertGreaterThan(red_after, red_before * 0.8);

    tiny_image_destroy(&image);
    return failures;
}

/** Auto levels opens each channel and so removes the cast. */
static int levels(void) {
    int failures = 0;
    TinyImage image;

    if (!make_flat(&image, 90, 150, 30)) return 1;

    failures += assertEquals(tiny_image_auto_levels(&image), 0);

    for (uint8_t c = 0; c < 3u; c++) {
        uint32_t low;
        uint32_t high;

        extent_of(&image, c, &low, &high);

        failures += assertLessThan(low, 30);
        failures += assertGreaterThan(high, 225);
    }

    // each channel filling the range on its own is what removes the cast
    double drift = mean_of(&image, 0) - mean_of(&image, 1);
    if (drift < 0.0) drift = -drift;

    failures += assertLessThan(drift, 12.0);

    tiny_image_destroy(&image);

    // a one channel image has nothing per-channel to do, so it falls back to
    // the luminance stretch rather than failing
    if (tiny_image_create(&image, 8, 8, 1) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 64u; i++) {
        image.data[i] = (uint8_t) (100u + (i % 8u) * 4u);
    }

    failures += assertEquals(tiny_image_auto_levels(&image), 0);

    uint32_t low;
    uint32_t high;
    extent_of(&image, 0, &low, &high);
    failures += assertGreaterThan(high - low, 100);

    tiny_image_destroy(&image);
    return failures;
}

/** Auto brightness and auto gamma both move the mean to the middle. */
static int brightness(void) {
    int failures = 0;
    TinyImage image;

    if (!make_flat(&image, 20, 60, 0)) return 1;

    failures += assertEquals(tiny_image_auto_brightness(&image), 0);
    failures += assertIn(mean_of(&image, 1), 115.0, 140.0);

    tiny_image_destroy(&image);
    if (!make_flat(&image, 20, 60, 0)) return failures + 1;

    failures += assertEquals(tiny_image_auto_gamma(&image), 0);
    failures += assertIn(mean_of(&image, 1), 110.0, 145.0);

    // a gamma leaves both ends where they are, which is what separates it from
    // a scale: black stays black
    uint32_t low;
    uint32_t high;
    extent_of(&image, 1, &low, &high);
    failures += assertLessThan(high, 250);

    tiny_image_destroy(&image);

    // an image already centered is left alone by both, within rounding
    if (!make_flat(&image, 120, 136, 0)) return failures + 1;

    double before = mean_of(&image, 1);
    failures += assertEquals(tiny_image_auto_brightness(&image), 0);
    failures += assertIn(mean_of(&image, 1), before - 3.0, before + 3.0);

    tiny_image_destroy(&image);

    // a fully black image has no mean to work from and is a no-op rather than
    // a division by zero
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return failures + 1;

    failures += assertEquals(tiny_image_auto_brightness(&image), 0);
    failures += assertEquals(tiny_image_auto_gamma(&image), 0);
    failures += assertEquals(image.data[0], 0);

    tiny_image_destroy(&image);
    return failures;
}

/** Auto color brings the three channel means together. */
static int color(void) {
    int failures = 0;
    TinyImage image;

    if (!make_flat(&image, 80, 170, 40)) return 1;

    double spread_before = mean_of(&image, 0) - mean_of(&image, 2);

    failures += assertEquals(tiny_image_auto_color(&image), 0);

    double spread_after = mean_of(&image, 0) - mean_of(&image, 2);
    if (spread_after < 0.0) spread_after = -spread_after;

    failures += assertGreaterThan(spread_before, 20.0);
    failures += assertLessThan(spread_after, 6.0);

    tiny_image_destroy(&image);
    return failures;
}

/** Improve runs the stack and stays inside the range. */
static int improve(void) {
    int failures = 0;
    TinyImage image;

    if (!make_flat(&image, 70, 140, 25)) return 1;

    failures += assertEquals(tiny_image_improve(&image), 0);
    failures += assertEquals(image.width, 64);
    failures += assertEquals(image.channels, 3);

    uint32_t low;
    uint32_t high;
    extent_of(&image, 1, &low, &high);

    failures += assertGreaterThan(high - low, 150);

    tiny_image_destroy(&image);
    return failures;
}

/** Shadows and highlights each move one end and leave the other. */
static int shadows_highlights(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 3, 1, 3) != TINYIMG_OK) return 1;

    static const uint8_t LEVELS[3] = {20, 128, 240};

    for (uint32_t i = 0; i < 3u; i++) {
        image.data[i * 3u] = LEVELS[i];
        image.data[i * 3u + 1u] = LEVELS[i];
        image.data[i * 3u + 2u] = LEVELS[i];
    }

    failures +=
        assertEquals(tiny_image_shadows_highlights(&image, 0.8f, 0.0f), 0);

    // the dark end lifts a long way, the mid a little, and the light end
    // barely at all
    failures += assertGreaterThan(image.data[0], LEVELS[0] + 20);
    failures += assertGreaterThan(image.data[6], 230);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 3, 1, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 3u; i++) {
        image.data[i * 3u] = LEVELS[i];
        image.data[i * 3u + 1u] = LEVELS[i];
        image.data[i * 3u + 2u] = LEVELS[i];
    }

    failures +=
        assertEquals(tiny_image_shadows_highlights(&image, 0.0f, 1.0f), 0);

    // the highlight recovery pulls the top down and leaves black where it was
    failures += assertLessThan(image.data[6], LEVELS[2]);
    failures += assertLessThan(image.data[0], LEVELS[0] + 3);

    // both at zero is the identity
    failures +=
        assertEquals(tiny_image_shadows_highlights(&image, 0.0f, 0.0f), 0);

    failures += assertEquals(
        tiny_image_shadows_highlights(&image, 2.0f, 0.0f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_shadows_highlights(&image, 0.0f, -1.0f), TINYIMG_ERR_RANGE
    );

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief Dehaze raises contrast where the dark channel is bright.
 *
 * A veil is modelled as a constant added to every channel, which is what makes
 * the dark channel bright everywhere; removing it has to bring the darkest
 * pixels back down without moving the airlight.
 */
static int dehaze(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 64, 64, 3) != TINYIMG_OK) return 1;

    // a scene with real dark pixels, plus a uniform veil over all of it
    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 64u; x++) {
            uint8_t* p = image.data + ((size_t) y * 64u + x) * 3u;
            uint8_t base = (uint8_t) (((x / 8u) * 30u) & 0xFFu);

            p[0] = tiny_clamp_u8((int32_t) base / 2 + 110);
            p[1] = tiny_clamp_u8((int32_t) base / 2 + 110);
            p[2] = tiny_clamp_u8((int32_t) base / 2 + 120);
        }
    }

    uint32_t low_before;
    uint32_t high_before;
    extent_of(&image, 1, &low_before, &high_before);

    failures += assertEquals(tiny_image_dehaze(&image, 0.8f), 0);

    uint32_t low_after;
    uint32_t high_after;
    extent_of(&image, 1, &low_after, &high_after);

    // the range opens downward, because what the veil added is taken back off
    failures += assertLessThan(low_after, low_before);
    failures +=
        assertGreaterThan(high_after - low_after, high_before - low_before);

    // a strength of zero is the identity
    TinyImage kept;
    tiny_image_create(&kept, 64, 64, 3);
    memcpy(kept.data, image.data, 64u * 64u * 3u);

    failures += assertEquals(tiny_image_dehaze(&image, 0.0f), 0);
    failures += assertImageEquals(&image, &kept);

    failures +=
        assertEquals(tiny_image_dehaze(&image, 2.0f), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);

    // a one channel image has no dark channel, so it is a no-op
    if (tiny_image_create(&image, 8, 8, 1) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 64u; i++) image.data[i] = 150u;

    failures += assertEquals(tiny_image_dehaze(&image, 0.5f), 0);
    failures += assertEquals(image.data[0], 150);

    tiny_image_destroy(&image);
    return failures;
}

static int nulls(void) {
    int failures = 0;

    failures += assertEquals(tiny_image_auto_brightness(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_auto_contrast(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_auto_color(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_auto_levels(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_auto_gamma(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_improve(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_dehaze(0, 0.5f), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_image_shadows_highlights(0, 0.5f, 0.5f), TINYIMG_ERR_NULL
    );

    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += contrast();
    failures += levels();
    failures += brightness();
    failures += color();
    failures += improve();
    failures += shadows_highlights();
    failures += dehaze();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
