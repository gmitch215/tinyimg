#include "test.h"

/** An image built from a known set of colors in known proportions. */
static int make_mix(TinyImage* image) {
    if (tiny_image_create(image, 100, 100, 3) != TINYIMG_OK) return 0;

    // sixty per cent red, thirty green, ten blue
    for (uint32_t i = 0; i < 10000u; i++) {
        uint8_t* p = image->data + i * 3u;

        if (i < 6000u) {
            p[0] = 200u;
            p[1] = 30u;
            p[2] = 30u;
        }
        else if (i < 9000u) {
            p[0] = 30u;
            p[1] = 200u;
            p[2] = 30u;
        }
        else {
            p[0] = 30u;
            p[1] = 30u;
            p[2] = 200u;
        }
    }

    return 1;
}

/** The histogram counts every pixel exactly once. */
static int histogram(void) {
    int failures = 0;
    TinyImage image;

    if (!make_mix(&image)) return 1;

    uint32_t bins[256];

    failures += assertEquals(tiny_image_histogram(&image, 0, bins), 0);

    uint32_t total = 0;
    for (uint32_t i = 0; i < 256u; i++) total += bins[i];

    failures += assertEquals(total, 10000);
    failures += assertEquals(bins[200], 6000);
    failures += assertEquals(bins[30], 4000);

    // the luminance channel is reachable through 255, which is not a channel
    // index any image has
    failures += assertEquals(tiny_image_histogram(&image, 255u, bins), 0);

    total = 0;
    for (uint32_t i = 0; i < 256u; i++) total += bins[i];
    failures += assertEquals(total, 10000);

    // exactly three distinct luminances, one per color
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        if (bins[i] != 0u) distinct++;
    }

    failures += assertEquals(distinct, 3);

    failures +=
        assertEquals(tiny_image_histogram(&image, 3, bins), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_histogram(&image, 0, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_histogram(0, 0, bins), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

/** The average is the mean, and the dominant color is the majority one. */
static int averages(void) {
    int failures = 0;
    TinyImage image;

    if (!make_mix(&image)) return 1;

    uint8_t mean[3];
    failures += assertEquals(tiny_image_average_color(&image, mean), 0);

    // 0.6 * 200 + 0.4 * 30 for red, and the mirror for the others
    failures += assertIn((double) mean[0], 130.0, 134.0);
    failures += assertIn((double) mean[1], 79.0, 83.0);
    failures += assertIn((double) mean[2], 45.0, 49.0);

    // the dominant color is the majority color, not the mean: a mean of a
    // red and green mix is a muddy yellow that appears in no pixel
    uint8_t dominant[3];
    failures += assertEquals(tiny_image_dominant_color(&image, dominant), 0);

    failures += assertGreaterThan(dominant[0], 150);
    failures += assertLessThan(dominant[1], 60);
    failures += assertLessThan(dominant[2], 60);

    failures +=
        assertEquals(tiny_image_average_color(&image, 0), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_dominant_color(0, dominant), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

/** A palette finds the colors that are there, largest cluster first. */
static int palette(void) {
    int failures = 0;
    TinyImage image;

    if (!make_mix(&image)) return 1;

    uint8_t colors[3 * 3];
    failures += assertEquals(tiny_image_palette(&image, 3, colors), 0);

    // the three colors the image is made of, in order of how much of it they
    // are: red, then green, then blue
    failures += assertGreaterThan(colors[0], 150);
    failures += assertGreaterThan(colors[4], 150);
    failures += assertGreaterThan(colors[8], 150);

    // and the first entry is the dominant one, which is what lets the two
    // functions share an implementation
    uint8_t dominant[3];
    tiny_image_dominant_color(&image, dominant);
    failures += assertIn(
        (double) colors[0], (double) dominant[0] - 12.0,
        (double) dominant[0] + 12.0
    );

    // the same image twice gives the same palette, because the seeding is by
    // position rather than at random
    uint8_t again[3 * 3];
    failures += assertEquals(tiny_image_palette(&image, 3, again), 0);
    failures += assertBytesMatch(colors, again, sizeof(colors));

    failures +=
        assertEquals(tiny_image_palette(&image, 0, colors), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_image_palette(&image, 300, colors), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_palette(&image, 3, 0), TINYIMG_ERR_NULL);

    // asking for more colors than an image holds is not an error
    TinyImage flat;
    if (tiny_image_create(&flat, 4, 4, 3) != TINYIMG_OK) {
        tiny_image_destroy(&image);
        return failures + 1;
    }

    uint8_t many[3 * 16];
    failures += assertEquals(tiny_image_palette(&flat, 16, many), 0);

    tiny_image_destroy(&flat);
    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief A perceptual hash survives a re-encode and separates two pictures.
 *
 * Both halves matter. A hash that is stable but not discriminating is a
 * constant, and one that is discriminating but not stable is a checksum; only
 * a threshold between the two distances makes it useful.
 */
static int phash(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("sf-24.jpg", &size);
    if (!bytes) return 1;

    TinyImage original;
    memset(&original, 0, sizeof(original));

    int result = tiny_image_load(&original, bytes, size);
    free(bytes);

    failures += assertEquals(result, 0);
    if (result != TINYIMG_OK) return failures;

    uint64_t first = 0;
    failures += assertEquals(tiny_image_phash(&original, &first), 0);

    // the same image at a quarter the size hashes to nearly the same value,
    // because the hash reduces to a 32x32 grid before it looks at anything
    TinyImage smaller;
    tiny_image_create(
        &smaller, original.width, original.height, original.channels
    );
    memcpy(
        smaller.data, original.data,
        (size_t) original.width * original.height * original.channels
    );

    failures += assertEquals(
        tiny_image_resize(&smaller, original.width / 4u, original.height / 4u),
        0
    );

    uint64_t scaled = 0;
    failures += assertEquals(tiny_image_phash(&smaller, &scaled), 0);
    failures += assertLessThan(tiny_phash_distance(first, scaled), 8);

    // and a brightness change does not move it, because the DC term is left
    // out of the median
    TinyImage brighter;
    tiny_image_create(
        &brighter, original.width, original.height, original.channels
    );
    memcpy(
        brighter.data, original.data,
        (size_t) original.width * original.height * original.channels
    );

    failures += assertEquals(tiny_image_brightness(&brighter, 1.15f), 0);

    uint64_t lifted = 0;
    failures += assertEquals(tiny_image_phash(&brighter, &lifted), 0);
    failures += assertLessThan(tiny_phash_distance(first, lifted), 8);

    // an unrelated picture is far away
    size = 0;
    bytes = readFixture("road.jpg", &size);
    if (!bytes) {
        tiny_image_destroy(&original);
        tiny_image_destroy(&smaller);
        tiny_image_destroy(&brighter);
        return failures + 1;
    }

    TinyImage other;
    memset(&other, 0, sizeof(other));

    result = tiny_image_load(&other, bytes, size);
    free(bytes);

    failures += assertEquals(result, 0);

    if (result == TINYIMG_OK) {
        uint64_t different = 0;
        failures += assertEquals(tiny_image_phash(&other, &different), 0);
        failures +=
            assertGreaterThan(tiny_phash_distance(first, different), 18);

        tiny_image_destroy(&other);
    }

    // the distance is a bit count, so it is symmetric and zero on itself
    failures += assertEquals(tiny_phash_distance(first, first), 0);
    failures += assertEquals(
        tiny_phash_distance(first, scaled), tiny_phash_distance(scaled, first)
    );
    failures += assertEquals(tiny_phash_distance(0, ~(uint64_t) 0), 64);

    failures += assertEquals(tiny_image_phash(&original, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_phash(0, &first), TINYIMG_ERR_NULL);

    tiny_image_destroy(&original);
    tiny_image_destroy(&smaller);
    tiny_image_destroy(&brighter);
    return failures;
}

/** A one channel image is analyzed on its one channel. */
static int grayscale(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 16, 16, 1) != TINYIMG_OK) return 1;
    for (uint32_t i = 0; i < 256u; i++) image.data[i] = (uint8_t) i;

    uint32_t bins[256];
    failures += assertEquals(tiny_image_histogram(&image, 255u, bins), 0);

    for (uint32_t i = 0; i < 256u; i++) {
        failures += assertEquals(bins[i], 1);
    }

    uint8_t mean[1];
    failures += assertEquals(tiny_image_average_color(&image, mean), 0);
    failures += assertIn((double) mean[0], 126.0, 129.0);

    uint64_t hash = 0;
    failures += assertEquals(tiny_image_phash(&image, &hash), 0);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += histogram();
    failures += averages();
    failures += palette();
    failures += phash();
    failures += grayscale();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
