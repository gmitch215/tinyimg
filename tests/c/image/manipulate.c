#include "tinyimg/plan.h"

#include "test.h"

/** An image whose every pixel identifies its own position. */
static int make_marked(TinyImage* image, uint32_t width, uint32_t height) {
    if (tiny_image_create(image, width, height, 3) != TINYIMG_OK) return 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* pixel = image->data + ((size_t) y * width + x) * 3u;
            pixel[0] = (uint8_t) (x * 17u + 1u);
            pixel[1] = (uint8_t) (y * 29u + 2u);
            pixel[2] = (uint8_t) (x * 5u + y * 7u + 3u);
        }
    }

    return 1;
}

static int copy_of(const TinyImage* source, TinyImage* out) {
    if (tiny_image_create(
            out, source->width, source->height, source->channels
        ) != TINYIMG_OK) {
        return 0;
    }

    memcpy(
        out->data, source->data,
        (size_t) source->width * source->height * source->channels
    );
    out->format = source->format;
    out->quality = source->quality;

    return 1;
}

static int dimensions(void) {
    int failures = 0;

    TinyImage image;
    if (!make_marked(&image, 40, 20)) return 1;

    failures += assertEquals(tiny_image_resize(&image, 10, 5), TINYIMG_OK);
    failures += assertEquals(image.width, 10);
    failures += assertEquals(image.height, 5);

    // one axis at zero keeps the ratio
    failures += assertEquals(tiny_image_resize(&image, 20, 0), TINYIMG_OK);
    failures += assertEquals(image.width, 20);
    failures += assertEquals(image.height, 10);

    failures +=
        assertEquals(tiny_image_resize(&image, 0, 0), TINYIMG_ERR_RANGE);

    failures += assertEquals(tiny_image_rotate_90(&image), TINYIMG_OK);
    failures += assertEquals(image.width, 10);
    failures += assertEquals(image.height, 20);

    failures += assertEquals(tiny_image_rotate_270(&image), TINYIMG_OK);
    failures += assertEquals(image.width, 20);
    failures += assertEquals(image.height, 10);

    failures += assertEquals(tiny_image_rotate_180(&image), TINYIMG_OK);
    failures += assertEquals(image.width, 20);

    failures += assertEquals(tiny_image_crop(&image, 5, 2, 8, 4), TINYIMG_OK);
    failures += assertEquals(image.width, 8);
    failures += assertEquals(image.height, 4);

    // reaching past the edge gives what there is
    failures += assertEquals(tiny_image_crop(&image, 4, 0, 900, 0), TINYIMG_OK);
    failures += assertEquals(image.width, 4);
    failures += assertEquals(image.height, 4);

    failures +=
        assertEquals(tiny_image_crop(&image, 40, 0, 1, 1), TINYIMG_ERR_RANGE);

    failures += assertEquals(tiny_image_zoom(&image, 3.0f), TINYIMG_OK);
    failures += assertEquals(image.width, 12);
    failures += assertEquals(image.height, 12);

    failures += assertEquals(tiny_image_dpr(&image, 0.5f), TINYIMG_OK);
    failures += assertEquals(image.width, 6);

    failures += assertEquals(tiny_image_zoom(&image, 0.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_image_zoom(&image, -1.0f), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

/** A flip twice over is the image that went in. */
static int involutions(void) {
    int failures = 0;

    TinyImage image;
    TinyImage original;
    if (!make_marked(&image, 13, 7)) return 1;
    if (!copy_of(&image, &original)) return 1;

    failures += assertEquals(tiny_image_flip_horizontal(&image), TINYIMG_OK);
    failures += assertNotEquals(
        memcmp(image.data, original.data, (size_t) 13u * 7u * 3u), 0
    );
    failures += assertEquals(tiny_image_flip_horizontal(&image), TINYIMG_OK);
    failures += assertImageEquals(&image, &original);

    failures += assertEquals(tiny_image_flip_vertical(&image), TINYIMG_OK);
    failures += assertEquals(tiny_image_flip_vertical(&image), TINYIMG_OK);
    failures += assertImageEquals(&image, &original);

    failures += assertEquals(tiny_image_rotate_180(&image), TINYIMG_OK);
    failures += assertEquals(tiny_image_rotate_180(&image), TINYIMG_OK);
    failures += assertImageEquals(&image, &original);

    for (uint32_t i = 0; i < 4u; i++) {
        failures += assertEquals(tiny_image_rotate_90(&image), TINYIMG_OK);
    }
    failures += assertImageEquals(&image, &original);

    failures += assertEquals(tiny_image_invert(&image), TINYIMG_OK);
    failures += assertEquals(tiny_image_invert(&image), TINYIMG_OK);
    failures += assertImageEquals(&image, &original);

    // a turn and its opposite, which is the composition rather than an
    // involution
    failures += assertEquals(tiny_image_rotate_90(&image), TINYIMG_OK);
    failures += assertEquals(tiny_image_rotate_270(&image), TINYIMG_OK);
    failures += assertImageEquals(&image, &original);

    tiny_image_destroy(&image);
    tiny_image_destroy(&original);
    return failures;
}

/**
 * @brief Each eager operation is the same operation through a plan.
 *
 * Which is not a tautology worth skipping: the eager entry points could have
 * grown their own arithmetic, and this is what says they did not. It also means
 * every one of them inherits the sampler and the clamping the planner tests
 * pin down.
 */
static int match_the_plan(void) {
    int failures = 0;

    TinyImage marked;
    if (!make_marked(&marked, 32, 24)) return 1;

    for (uint32_t which = 0; which < 8u; which++) {
        TinyImage eager;
        if (!copy_of(&marked, &eager)) return failures + 1;

        TinyPlan plan;
        tiny_plan_init_image(&plan, &marked);

        switch (which) {
            case 0:
                tiny_image_resize(&eager, 20, 15);
                tiny_plan_resize(&plan, 20, 15);
                break;
            case 1:
                tiny_image_crop(&eager, 3, 4, 10, 8);
                tiny_plan_crop(&plan, 3, 4, 10, 8);
                break;
            case 2:
                tiny_image_brightness(&eager, 1.4f);
                tiny_plan_brightness(&plan, 1.4f);
                break;
            case 3:
                tiny_image_contrast(&eager, 0.7f);
                tiny_plan_contrast(&plan, 0.7f);
                break;
            case 4:
                tiny_image_saturation(&eager, 0.3f);
                tiny_plan_saturation(&plan, 0.3f);
                break;
            case 5:
                tiny_image_gamma_correction(&eager, 2.2f);
                tiny_plan_gamma(&plan, 2.2f);
                break;
            case 6:
                tiny_image_hue(&eager, 60.0f);
                tiny_plan_hue(&plan, 60.0f);
                break;
            default:
                tiny_image_blur(&eager, 2.0f);
                tiny_plan_blur(&plan, 2.0f);
                break;
        }

        TinyImage planned;
        memset(&planned, 0, sizeof(planned));

        failures += assertEquals(tiny_plan_run(&plan, &planned), TINYIMG_OK);
        failures += assertImageEquals(&eager, &planned);

        tiny_image_destroy(&eager);
        tiny_image_destroy(&planned);
    }

    tiny_image_destroy(&marked);
    return failures;
}

/** A fit through the eager surface, for each of the eleven modes. */
static int fits(void) {
    int failures = 0;

    TinyImageFit modes[11] = {
        TINYIMG_FIT_SCALE_DOWN,   TINYIMG_FIT_CONTAIN,
        TINYIMG_FIT_COVER,        TINYIMG_FIT_CROP,
        TINYIMG_FIT_ASPECT_CROP,  TINYIMG_FIT_ASPECT_CONTAIN,
        TINYIMG_FIT_ASPECT_COVER, TINYIMG_FIT_PAD,
        TINYIMG_FIT_STRETCH,      TINYIMG_FIT_FILL,
        TINYIMG_FIT_SCALE_UP
    };

    for (uint32_t i = 0; i < 11u; i++) {
        TinyImage image;
        if (!make_marked(&image, 40, 20)) return failures + 1;

        failures +=
            assertEquals(tiny_image_fit(&image, 30, 30, modes[i]), TINYIMG_OK);

        // every mode produces something, and no mode produces a degenerate
        // extent
        failures += assertTrue(image.width > 0 && image.height > 0);

        TinyImage padded;
        if (!make_marked(&padded, 40, 20)) return failures + 1;

        uint8_t blue[4] = {0, 0, 255, 255};
        failures += assertEquals(
            tiny_image_fit_with_padding(&padded, 30, 30, modes[i], blue),
            TINYIMG_OK
        );
        failures += assertEquals(padded.width, image.width);
        failures += assertEquals(padded.height, image.height);

        tiny_image_destroy(&image);
        tiny_image_destroy(&padded);
    }

    // the padding color reaches the pixels the image does not cover
    TinyImage image;
    if (!make_marked(&image, 40, 20)) return failures + 1;

    uint8_t blue[4] = {0, 0, 255, 255};
    failures += assertEquals(
        tiny_image_fit_with_padding(&image, 40, 40, TINYIMG_FIT_PAD, blue),
        TINYIMG_OK
    );
    failures += assertEquals(image.width, 40);
    failures += assertEquals(image.height, 40);
    failures += assertEquals(image.data[0], 0);
    failures += assertEquals(image.data[2], 255);

    tiny_image_destroy(&image);

    // and the background color is what fills them when no padding was given
    TinyImage second;
    if (!make_marked(&second, 40, 20)) return failures + 1;

    failures += assertEquals(
        tiny_image_fit_with_padding_and_background(
            &second, 40, 40, TINYIMG_FIT_PAD, 0, blue
        ),
        TINYIMG_OK
    );
    failures += assertEquals(second.data[2], 255);

    tiny_image_destroy(&second);
    return failures;
}

/** Metadata survives an operation on the pixels. */
static int metadata_survives(void) {
    int failures = 0;

    TinyImage image;
    if (!make_marked(&image, 20, 10)) return 1;

    failures += assertEquals(
        tiny_image_set_metadata(&image, "author", "gregory"), TINYIMG_OK
    );
    failures += assertEquals(tiny_image_resize(&image, 10, 5), TINYIMG_OK);
    failures += assertEquals(tiny_image_has_metadata(&image, "author"), 1);

    char* value = 0;
    failures += assertEquals(
        tiny_image_get_metadata(&image, "author", &value), TINYIMG_OK
    );
    failures += assertStringsMatch(value, "gregory");

    failures += assertEquals(tiny_image_rotate_90(&image), TINYIMG_OK);
    failures += assertEquals(tiny_image_has_metadata(&image, "author"), 1);

    tiny_image_destroy(&image);
    return failures;
}

/** A grayscale image comes through every operation still grayscale. */
static int channel_counts(void) {
    int failures = 0;

    uint8_t counts[4] = {1, 2, 3, 4};

    for (uint32_t i = 0; i < 4u; i++) {
        TinyImage image;
        if (tiny_image_create(&image, 16, 12, counts[i]) != TINYIMG_OK) {
            return failures + 1;
        }

        memset(image.data, 120, (size_t) 16u * 12u * counts[i]);

        failures += assertEquals(tiny_image_resize(&image, 8, 6), TINYIMG_OK);
        failures += assertEquals(image.channels, counts[i]);

        failures +=
            assertEquals(tiny_image_brightness(&image, 1.1f), TINYIMG_OK);
        failures += assertEquals(image.channels, counts[i]);

        failures += assertEquals(tiny_image_blur(&image, 1.0f), TINYIMG_OK);
        failures += assertEquals(image.channels, counts[i]);

        failures += assertEquals(tiny_image_rotate_90(&image), TINYIMG_OK);
        failures += assertEquals(image.channels, counts[i]);

        tiny_image_destroy(&image);
    }

    return failures;
}

static int nulls(void) {
    int failures = 0;

    failures += assertEquals(tiny_image_resize(0, 1, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_crop(0, 0, 0, 1, 1), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_flip_horizontal(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_flip_vertical(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_rotate_90(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_rotate_180(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_rotate_270(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_zoom(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_dpr(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_invert(0), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_brightness(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_contrast(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_saturation(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_hue(0, 0.0f), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_gamma_correction(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(tiny_image_blur(0, 1.0f), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_image_gaussian_blur(0, 1.0f), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_image_fit(0, 1, 1, TINYIMG_FIT_COVER), TINYIMG_ERR_NULL
    );

    TinyImage image;
    if (tiny_image_create(&image, 8, 8, 3) != TINYIMG_OK) return failures + 1;

    failures += assertEquals(tiny_image_hue(&image, 400.0f), TINYIMG_ERR_RANGE);
    failures +=
        assertEquals(tiny_image_hue(&image, -400.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(
        tiny_image_gamma_correction(&image, 0.0f), TINYIMG_ERR_RANGE
    );
    failures +=
        assertEquals(tiny_image_brightness(&image, -1.0f), TINYIMG_ERR_RANGE);
    failures += assertEquals(tiny_image_blur(&image, -1.0f), TINYIMG_ERR_RANGE);

    tiny_image_destroy(&image);
    return failures;
}

int main(void) {
    int failures = 0;

    failures += dimensions();
    failures += involutions();
    failures += match_the_plan();
    failures += fits();
    failures += metadata_survives();
    failures += channel_counts();
    failures += nulls();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
