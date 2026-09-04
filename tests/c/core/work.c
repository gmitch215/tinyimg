#include "tinyimg/work.h"
#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/image.h"
#include "tinyimg/memory.h"
#include "tinyimg/plan.h"

static int decodeWith(
    const char* name, TinyImage* image, const TinyDecodeOpts* opts
) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    int result = tiny_image_decode(image, bytes, size, opts);

    free(bytes);
    return result;
}

/*
 * Every reduction in this library is a claim that work does not happen, and a
 * label is not evidence. The 1/4 JPEG arm carried the word through the API, the
 * planner and the decoder while running the full transform and averaging the
 * result away; nothing failed, because nothing was counting.
 *
 * These assertions are what make the claim falsifiable.
 */
int main(void) {
    int r = 0;

    // #region a scaled decode runs a smaller transform

    /*
     * Samples per transform is the number that cannot be talked around. A
     * genuine 1/n decode writes (8/n) squared samples per block; a decode that
     * transforms the whole block and averages it down writes 64 whatever it
     * calls itself.
     *
     * A single component file, because the figure is exact only there. A
     * subsampled one mixes sizes: at a half scale on 4:2:0 the luma blocks take
     * a 4x4 transform and the chroma blocks, already at half resolution, take
     * the full 8x8, which averages to 32 rather than 16.
     */
    static const struct {
        uint8_t den;
        uint32_t per_transform;
        uint32_t blocks;
        // 40 by 23 blocks, and the same count at every scale
    } scales[4] = {{1, 64, 920}, {2, 16, 920}, {4, 4, 920}, {8, 1, 920}};

    for (size_t i = 0; i < 4; i++) {
        TinyDecodeOpts opts = {0, 0, 0, 0, scales[i].den, 1};
        TinyImage image;

        tiny_work_reset();
        r |= assertEquals(
            decodeWith("derived/base-gray.jpg", &image, &opts), TINYIMG_OK
        );

        uint32_t transforms = tiny_work_read(TINYIMG_WORK_TRANSFORMS);
        uint32_t samples = tiny_work_read(TINYIMG_WORK_TRANSFORM_SAMPLES);

        r |= assertTrue(transforms > 0);

        if (transforms > 0) {
            r |= assertEquals(
                (long) (samples / transforms), (long) scales[i].per_transform
            );
        }

        // the entropy work does not fall with the denominator: every
        // coefficient still has to be read to reach the next block
        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_BLOCKS), (long) scales[i].blocks
        );

        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_DECODED_SAMPLES),
            (long) (image.width * image.height)
        );
        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_SOURCE_SAMPLES), 320L * 180L
        );

        tiny_image_destroy(&image);
    }

    // a subsampled file still has to fall, even though its figure is a mixture
    uint32_t previous = 65;

    for (uint8_t den = 1; den <= 8; den = (uint8_t) (den * 2)) {
        TinyDecodeOpts opts = {0, 0, 0, 0, den, 3};
        TinyImage image;

        tiny_work_reset();
        r |= assertEquals(
            decodeWith("derived/base-420.jpg", &image, &opts), TINYIMG_OK
        );

        uint32_t transforms = tiny_work_read(TINYIMG_WORK_TRANSFORMS);
        uint32_t per =
            transforms
                ? tiny_work_read(TINYIMG_WORK_TRANSFORM_SAMPLES) / transforms
                : 0;

        r |= assertTrue(per < previous);
        previous = per;

        tiny_image_destroy(&image);
    }

    // #endregion

    // #region a region decode reconstructs fewer macroblocks

    /*
     * The WebP truncation is the other kind of claim: not a smaller transform
     * but fewer of them. A band at the top stops early, a band at the bottom
     * cannot, and the counter is what tells the two apart.
     */
    TinyDecodeOpts whole = {0, 0, 0, 0, 1, 3};
    TinyImage image;

    tiny_work_reset();
    r |= assertEquals(
        decodeWith("derived/base-lossy.webp", &image, &whole), TINYIMG_OK
    );

    uint32_t all = tiny_work_read(TINYIMG_WORK_MACROBLOCKS);
    r |= assertTrue(all > 0);
    tiny_image_destroy(&image);

    TinyDecodeOpts top = {0, 0, 320, 32, 1, 3};
    tiny_work_reset();
    r |= assertEquals(
        decodeWith("derived/base-lossy.webp", &image, &top), TINYIMG_OK
    );

    uint32_t band = tiny_work_read(TINYIMG_WORK_MACROBLOCKS);
    r |= assertTrue(band < all);
    r |=
        assertEquals((long) tiny_work_read(TINYIMG_WORK_FILTERED), (long) band);
    tiny_image_destroy(&image);

    // a band at the bottom reaches the last row, so it costs what the whole
    // frame costs; a saving here would mean the bound was unsound
    TinyDecodeOpts bottom = {0, 148, 320, 32, 1, 3};
    tiny_work_reset();
    r |= assertEquals(
        decodeWith("derived/base-lossy.webp", &image, &bottom), TINYIMG_OK
    );

    r |= assertEquals(
        (long) tiny_work_read(TINYIMG_WORK_MACROBLOCKS), (long) all
    );
    tiny_image_destroy(&image);

    // #endregion

    // #region the planner decodes a rectangle rather than a picture

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base-420.jpg", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        TinyPlan plan;
        tiny_plan_init(&plan, bytes, size);
        tiny_plan_crop(&plan, 96, 48, 128, 96);
        tiny_plan_resize(&plan, 32, 24);

        TinyImage out;
        tiny_memset(&out, 0, sizeof(out));

        tiny_work_reset();
        r |= assertEquals(tiny_plan_run(&plan, &out), TINYIMG_OK);

        // the source is 57,600 samples and the output 768, so a planner that
        // decoded the picture would show it here
        r |= assertTrue(
            tiny_work_read(TINYIMG_WORK_DECODED_SAMPLES) < 320u * 180u
        );
        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_RESAMPLED), 32L * 24L
        );
        r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_PASSES), 1L);

        tiny_image_destroy(&out);
        free(bytes);
    }

    // #endregion

    // #region the counters are a total, and reset clears them

    tiny_work_reset();
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_BLOCKS), 0L);
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_TRANSFORMS), 0L);

    // out of range reads answer zero rather than reading past the array
    r |= assertEquals((long) tiny_work_read((TinyWorkCounter) 999), 0L);

    // #endregion

    tiny_arena_reset();
    return r;
}
