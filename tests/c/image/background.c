#include "test.h"

/** A bordered image: a uniform frame around a patterned middle. */
static int make_bordered(
    TinyImage* image, uint32_t size, uint32_t border, uint8_t frame
) {
    if (tiny_image_create(image, size, size, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            uint8_t* p = image->data + ((size_t) y * size + x) * 3u;
            int inside = x >= border && y >= border && x < size - border &&
                         y < size - border;
            uint8_t value = inside ? (uint8_t) (30u + ((x + y) & 63u)) : frame;

            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
    }

    return 1;
}

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** Trim removes exactly the uniform frame. */
static int trim(void) {
    int failures = 0;
    TinyImage image;

    if (!make_bordered(&image, 40, 6, 255)) return 1;

    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertEquals(image.width, 28);
    failures += assertEquals(image.height, 28);

    tiny_image_destroy(&image);

    // an off-white frame needs the tolerance, and without it nothing is
    // trimmed rather than the wrong thing being trimmed
    if (!make_bordered(&image, 40, 6, 250)) return failures + 1;

    for (uint32_t y = 0; y < 40u; y++) {
        for (uint32_t x = 0; x < 40u; x++) {
            int frame = !(x >= 6u && y >= 6u && x < 34u && y < 34u);
            if (!frame) continue;

            uint8_t* p = image.data + ((size_t) y * 40u + x) * 3u;
            uint8_t jitter = (uint8_t) (248u + ((x * 7u + y * 3u) % 5u));

            p[0] = jitter;
            p[1] = jitter;
            p[2] = jitter;
        }
    }

    TinyImage kept;
    tiny_image_create(&kept, 40, 40, 3);
    memcpy(kept.data, image.data, 40u * 40u * 3u);

    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertEquals(image.width, 40);

    failures += assertEquals(tiny_image_trim(&image, 8), 0);
    failures += assertEquals(image.width, 28);

    tiny_image_destroy(&kept);
    tiny_image_destroy(&image);

    // an image whose corners disagree has no uniform frame, so it is left
    // alone rather than cropped to whatever the first corner happened to be
    if (!make_bordered(&image, 40, 6, 255)) return failures + 1;

    image.data[((size_t) 39 * 40 + 39) * 3u] = 0u;

    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertEquals(image.width, 40);

    tiny_image_destroy(&image);

    // an image that is entirely one color has no content, so the trim stops
    // rather than cropping to nothing
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 200u;

    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertGreaterThan(image.width, 0);
    failures += assertGreaterThan(image.height, 0);

    failures += assertEquals(tiny_image_trim(0, 0), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

/**
 * @brief Background removal clears what is connected to the edge, not what
 * merely matches.
 *
 * A white subject on a white backdrop is the case that separates a flood fill
 * from a color key: the subject matches the background exactly and has to
 * survive because nothing joins it to the edge.
 */
static int remove_background(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 40, 40, 3) != TINYIMG_OK) return 1;

    // a white field, a dark ring, and a white disc inside the ring
    for (uint32_t y = 0; y < 40u; y++) {
        for (uint32_t x = 0; x < 40u; x++) {
            float dx = (float) x - 19.5f;
            float dy = (float) y - 19.5f;
            float distance = tiny_sqrtf(dx * dx + dy * dy);
            uint8_t value = distance > 14.0f   ? 255u
                            : distance > 10.0f ? 20u
                                               : 255u;
            uint8_t* p = image.data + ((size_t) y * 40u + x) * 3u;

            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
    }

    failures += assertEquals(tiny_image_remove_background(&image, 10), 0);
    failures += assertEquals(image.channels, 4);

    // the field is gone
    failures += assertEquals(at(&image, 0, 0)[3], 0);
    failures += assertEquals(at(&image, 39, 39)[3], 0);

    // the ring is opaque
    failures += assertEquals(at(&image, 20, 8)[3], 255);

    // and so is the white disc inside it, which matches the background exactly
    // and is not connected to the edge
    failures += assertEquals(at(&image, 20, 20)[3], 255);
    failures += assertEquals(at(&image, 20, 20)[0], 255);

    tiny_image_destroy(&image);

    // an image with no uniform edge loses nothing, because nothing near the
    // corner matches within the tolerance
    if (tiny_image_create(&image, 20, 20, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 400u; i++) {
        image.data[i * 3u] = (uint8_t) (i & 0xFFu);
        image.data[i * 3u + 1u] = (uint8_t) ((i * 3u) & 0xFFu);
        image.data[i * 3u + 2u] = (uint8_t) ((i * 7u) & 0xFFu);
    }

    failures += assertEquals(tiny_image_remove_background(&image, 0), 0);

    uint32_t cleared = 0;
    for (uint32_t i = 0; i < 400u; i++) {
        if (image.data[i * 4u + 3u] == 0u) cleared++;
    }

    // only the top left corner, which is the seed and so matches itself. the
    // other three do not match the seed color and are therefore not seeds
    failures += assertEquals(cleared, 1);

    tiny_image_destroy(&image);

    // a wide tolerance clears everything reachable, which on a flat image is
    // all of it
    if (tiny_image_create(&image, 16, 16, 3) != TINYIMG_OK) return failures + 1;
    for (uint32_t i = 0; i < 16u * 16u * 3u; i++) image.data[i] = 128u;

    failures += assertEquals(tiny_image_remove_background(&image, 0), 0);

    for (uint32_t i = 0; i < 256u; i++) {
        failures += assertEquals(image.data[i * 4u + 3u], 0);
    }

    failures +=
        assertEquals(tiny_image_remove_background(0, 10), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

/** The forest fixture already has a removed background, so this is a no-op. */
static int against_a_fixture(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("forest.png", &size);
    if (!bytes) return 1;

    TinyImage image;
    memset(&image, 0, sizeof(image));

    int result = tiny_image_load(&image, bytes, size);
    free(bytes);

    failures += assertEquals(result, 0);
    if (result != TINYIMG_OK) return failures;

    failures += assertEquals(image.channels, 4);

    uint32_t clear_before = 0;
    size_t pixels = (size_t) image.width * image.height;

    for (size_t i = 0; i < pixels; i++) {
        if (image.data[i * 4u + 3u] == 0u) clear_before++;
    }

    failures += assertGreaterThan(clear_before, 0);

    // the corners are already transparent, so the seed color is a clear
    // pixel and the fill spreads through the region that is already clear
    failures += assertEquals(tiny_image_remove_background(&image, 4), 0);

    uint32_t clear_after = 0;
    for (size_t i = 0; i < pixels; i++) {
        if (image.data[i * 4u + 3u] == 0u) clear_after++;
    }

    // it can only ever clear more, never restore, and it must not clear the
    // whole frame
    failures += assertGreaterThan(clear_after + 1u, clear_before);
    failures += assertLessThan((double) clear_after, (double) pixels * 0.95);

    tiny_image_destroy(&image);
    return failures;
}

/** Trim against a fixture with a known letterbox. */
static int trims_a_letterbox(void) {
    int failures = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.png", &size);
    if (!bytes) return 1;

    TinyImage image;
    memset(&image, 0, sizeof(image));

    int result = tiny_image_load(&image, bytes, size);
    free(bytes);

    failures += assertEquals(result, 0);
    if (result != TINYIMG_OK) return failures;

    uint32_t width = image.width;
    uint32_t height = image.height;

    // a photograph has no uniform border, so a trim leaves it exactly as it
    // was; that is the case a trim which cropped on the first differing row
    // rather than the first differing line would get wrong
    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertEquals(image.width, width);
    failures += assertEquals(image.height, height);

    // pad it and the trim gives the original extent back
    static const uint8_t BLACK[3] = {0, 0, 0};
    failures += assertEquals(tiny_image_expand(&image, 12, 7, 5, 9, BLACK), 0);
    failures += assertEquals(image.width, width + 17u);

    failures += assertEquals(tiny_image_trim(&image, 0), 0);
    failures += assertEquals(image.width, width);
    failures += assertEquals(image.height, height);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += trim();
    failures += remove_background();
    failures += against_a_fixture();
    failures += trims_a_letterbox();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
