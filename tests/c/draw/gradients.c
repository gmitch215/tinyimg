#include "test.h"

static const uint8_t BLACK[4] = {0, 0, 0, 255};
static const uint8_t WHITE[4] = {255, 255, 255, 255};
static const uint8_t RED[4] = {255, 0, 0, 255};

static const uint8_t* at(const TinyImage* image, uint32_t x, uint32_t y) {
    return image->data + ((size_t) y * image->width + x) * image->channels;
}

/** A linear gradient runs along the line it was given and clamps past it. */
static int linear(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 64, 16, 3) != TINYIMG_OK) return 1;

    failures += assertEquals(
        tiny_image_gradient_linear(&image, 0, 0, 63, 0, BLACK, WHITE), 0
    );

    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertEquals(at(&image, 63, 0)[0], 255);
    failures += assertIn((double) at(&image, 32, 0)[0], 126.0, 132.0);

    // the gradient is a function of the projection onto the line, so every row
    // is the same
    for (uint32_t y = 1; y < 16u; y++) {
        failures += assertEquals(at(&image, 20, y)[0], at(&image, 20, 0)[0]);
    }

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 32, 32, 3) != TINYIMG_OK) return failures + 1;

    // a short line inside the image, so the ends clamp
    failures += assertEquals(
        tiny_image_gradient_linear(&image, 8, 8, 24, 24, BLACK, WHITE), 0
    );

    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertEquals(at(&image, 31, 31)[0], 255);
    failures += assertEquals(at(&image, 4, 4)[0], 0);

    // two coincident points name no direction
    failures += assertEquals(
        tiny_image_gradient_linear(&image, 5, 5, 5, 5, BLACK, WHITE),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_gradient_linear(&image, 0, 0, 1, 1, 0, WHITE),
        TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_gradient_linear(0, 0, 0, 1, 1, BLACK, WHITE),
        TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/** A radial gradient is symmetric about its center. */
static int radial(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 41, 41, 3) != TINYIMG_OK) return 1;

    failures += assertEquals(
        tiny_image_gradient_radial(&image, 20, 20, 20, WHITE, BLACK), 0
    );

    failures += assertEquals(at(&image, 20, 20)[0], 255);
    failures += assertEquals(at(&image, 0, 20)[0], 0);

    // the four points at the same distance agree
    uint8_t reference = at(&image, 30, 20)[0];
    failures += assertEquals(at(&image, 10, 20)[0], reference);
    failures += assertEquals(at(&image, 20, 30)[0], reference);
    failures += assertEquals(at(&image, 20, 10)[0], reference);

    // past the radius the outer color is held rather than extrapolated
    failures += assertEquals(at(&image, 0, 0)[0], 0);

    failures += assertEquals(
        tiny_image_gradient_radial(&image, 20, 20, 0, WHITE, BLACK),
        TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_gradient_radial(&image, 20, 20, 5, 0, BLACK),
        TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/** A fade takes the alpha down along its direction and adds one if needed. */
static int fade(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 64, 8, 3) != TINYIMG_OK) return 1;

    for (uint32_t i = 0; i < 64u * 8u * 3u; i++) image.data[i] = 200u;

    failures +=
        assertEquals(tiny_image_gradient_fade(&image, 0.0f, 0.0f, 1.0f), 0);

    // gained an alpha channel to have something to fade
    failures += assertEquals(image.channels, 4);
    failures += assertEquals(at(&image, 0, 0)[3], 255);
    failures += assertEquals(at(&image, 63, 0)[3], 0);
    failures += assertIn((double) at(&image, 32, 0)[3], 124.0, 132.0);

    // the color is untouched; only the alpha moves
    failures += assertEquals(at(&image, 63, 0)[0], 200);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 8, 64, 4) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 8u * 64u; i++) {
        image.data[i * 4u] = 100u;
        image.data[i * 4u + 3u] = 255u;
    }

    // 90 degrees runs down the image instead
    failures +=
        assertEquals(tiny_image_gradient_fade(&image, 90.0f, 0.0f, 1.0f), 0);
    failures += assertEquals(at(&image, 0, 0)[3], 255);
    failures += assertEquals(at(&image, 0, 63)[3], 0);

    tiny_image_destroy(&image);
    if (tiny_image_create(&image, 64, 8, 4) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 64u * 8u; i++) image.data[i * 4u + 3u] = 255u;

    // a band inside the image: opaque before it, clear after it
    failures +=
        assertEquals(tiny_image_gradient_fade(&image, 0.0f, 0.25f, 0.75f), 0);
    failures += assertEquals(at(&image, 8, 0)[3], 255);
    failures += assertEquals(at(&image, 56, 0)[3], 0);

    // a fade that runs backwards or past the ends is a range error
    failures += assertEquals(
        tiny_image_gradient_fade(&image, 0.0f, 0.8f, 0.2f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_gradient_fade(&image, 0.0f, -0.1f, 0.5f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_gradient_fade(&image, 0.0f, 0.0f, 1.5f), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_image_gradient_fade(0, 0.0f, 0.0f, 1.0f), TINYIMG_ERR_NULL
    );

    // a negative direction is still a direction, and the extent normalization
    // has to account for where it starts from
    failures +=
        assertEquals(tiny_image_gradient_fade(&image, 180.0f, 0.0f, 1.0f), 0);

    tiny_image_destroy(&image);
    return failures;
}

/** A border draws inside the edges; expand grows the image around them. */
static int borders(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 20, 20, 3) != TINYIMG_OK) return 1;

    failures += assertEquals(tiny_image_border(&image, 0, RED), 0);
    failures += assertEquals(at(&image, 0, 0)[0], 0);

    failures += assertEquals(tiny_image_border(&image, 3, RED), 0);

    // three rings inside the edge, and nothing past them
    failures += assertEquals(at(&image, 0, 10)[0], 255);
    failures += assertEquals(at(&image, 2, 10)[0], 255);
    failures += assertEquals(at(&image, 3, 10)[0], 0);
    failures += assertEquals(at(&image, 19, 10)[0], 255);
    failures += assertEquals(at(&image, 10, 0)[0], 255);
    failures += assertEquals(at(&image, 10, 10)[0], 0);

    // a width past the extent is clamped rather than refused
    failures += assertEquals(tiny_image_border(&image, 999, RED), 0);
    failures += assertEquals(at(&image, 10, 10)[0], 255);

    failures += assertEquals(tiny_image_border(&image, 1, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_border(0, 1, RED), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);

    // expand changes the extent and puts the original inside it
    if (tiny_image_create(&image, 8, 6, 3) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 48u * 3u; i += 3u) image.data[i + 1u] = 200u;

    failures += assertEquals(tiny_image_expand(&image, 0, 0, 0, 0, RED), 0);
    failures += assertEquals(image.width, 8);

    failures += assertEquals(tiny_image_expand(&image, 2, 3, 4, 5, RED), 0);
    failures += assertEquals(image.width, 14);
    failures += assertEquals(image.height, 14);

    // the border is the color given and the middle is the original
    failures += assertEquals(at(&image, 0, 0)[0], 255);
    failures += assertEquals(at(&image, 13, 13)[0], 255);
    failures += assertEquals(at(&image, 2, 3)[1], 200);
    failures += assertEquals(at(&image, 9, 8)[1], 200);

    tiny_image_destroy(&image);

    // a NULL color leaves the new pixels as the zeros create gave them
    if (tiny_image_create(&image, 4, 4, 4) != TINYIMG_OK) return failures + 1;

    for (uint32_t i = 0; i < 16u; i++) image.data[i * 4u + 3u] = 255u;

    failures += assertEquals(tiny_image_expand(&image, 1, 1, 1, 1, 0), 0);
    failures += assertEquals(at(&image, 0, 0)[3], 0);
    failures += assertEquals(at(&image, 1, 1)[3], 255);

    failures +=
        assertEquals(tiny_image_expand(0, 1, 1, 1, 1, RED), TINYIMG_ERR_NULL);

    tiny_image_destroy(&image);
    return failures;
}

/** Replace_color matches within a per-channel tolerance. */
static int replacement(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 4, 4, 3) != TINYIMG_OK) return 1;

    for (uint32_t i = 0; i < 16u; i++) {
        image.data[i * 3u] = (uint8_t) (100u + i);
        image.data[i * 3u + 1u] = 50u;
        image.data[i * 3u + 2u] = 50u;
    }

    static const uint8_t LOOK_FOR[3] = {100, 50, 50};
    static const uint8_t REPLACE[3] = {0, 255, 0};

    failures +=
        assertEquals(tiny_image_replace_color(&image, LOOK_FOR, REPLACE, 0), 0);

    // exact only, so one pixel changed
    failures += assertEquals(image.data[1], 255);
    failures += assertEquals(image.data[4], 50);

    // a tolerance of five reaches six of them, counting the one already changed
    static const uint8_t TOLERANCE[3] = {5, 5, 5};
    failures += assertEquals(
        tiny_image_replace_color(&image, LOOK_FOR, REPLACE, TOLERANCE), 0
    );

    uint32_t changed = 0;
    for (uint32_t i = 0; i < 16u; i++) {
        if (image.data[i * 3u + 1u] == 255u) changed++;
    }

    failures += assertEquals(changed, 6);

    failures += assertEquals(
        tiny_image_replace_color(&image, 0, REPLACE, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_replace_color(0, LOOK_FOR, REPLACE, 0), TINYIMG_ERR_NULL
    );

    tiny_image_destroy(&image);
    return failures;
}

/** A one channel target takes a color of its own width. */
static int single_channel(void) {
    int failures = 0;
    TinyImage image;

    if (tiny_image_create(&image, 16, 16, 1) != TINYIMG_OK) return 1;

    static const uint8_t ON[1] = {255};

    failures +=
        assertEquals(tiny_image_fill_rectangle(&image, 2, 2, 4, 4, ON), 0);
    failures += assertEquals(at(&image, 3, 3)[0], 255);
    failures += assertEquals(at(&image, 8, 8)[0], 0);

    // the opaque fast path memsets a one channel run, which is the branch a
    // three channel image never reaches
    failures += assertEquals(tiny_image_fill_circle(&image, 8, 8, 4, ON), 0);
    failures += assertEquals(at(&image, 8, 8)[0], 255);

    static const uint8_t OFF[1] = {0};
    failures += assertEquals(
        tiny_image_gradient_linear(&image, 0, 0, 15, 0, OFF, ON), 0
    );
    failures += assertEquals(at(&image, 0, 0)[0], 0);
    failures += assertEquals(at(&image, 15, 0)[0], 255);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    tiny_init();

    failures += linear();
    failures += radial();
    failures += fade();
    failures += borders();
    failures += replacement();
    failures += single_channel();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
