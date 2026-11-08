#include "tinyimg/work.h"
#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/image.h"
#include "tinyimg/memory.h"
#include "tinyimg/plan.h"
#include "tinyimg/util.h"

static uint32_t bufferDigest(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;

    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619u;

    return hash;
}

static uint32_t digest(const TinyImage* image) {
    return bufferDigest(
        image->data, (size_t) image->width * image->height * image->channels
    );
}

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

    // #region entropy bytes

    /*
     * The compressed bytes a request consumed. Every codec but the two
     * uncompressed layouts has to report something, nothing may report more
     * than the file holds, and a request that stops reading early has to read
     * less than one that does not.
     */
    static const struct {
        const char* name;
        // permille of the file the scan, stream or tile must reach, or 0 where
        // headers, palettes or further frames are a large share of the file
        long floor;
    } compressed[] = {
        {"sf-24.jpg", 800},
        {"derived/base-420.jpg", 800},
        {"derived/base-gray.jpg", 800},
        {"derived/base-restart.jpg", 800},
        {"derived/base-progressive.jpg", 800},
        {"derived/base.png", 800},
        {"derived/base-interlaced.png", 800},
        {"forest.png", 800},
        {"derived/base-lossy.webp", 0},
        {"derived/base-lossless.webp", 0},
        {"derived/base.gif", 0},
        {"derived/base-lzw.tif", 0},
        {"derived/base-deflate.tif", 0},
        {"derived/base-packbits.tif", 0},
        {"derived/base-rle8.bmp", 0},
        {"derived/base.avif", 0},
        {"derived/av1-tiles.avif", 0},
    };

    for (size_t i = 0; i < sizeof(compressed) / sizeof(compressed[0]); i++) {
        size_t file = 0;
        unsigned char* bytes = readFixture(compressed[i].name, &file);
        r |= assertNotNull(bytes);
        if (!bytes) continue;

        TinyImage decoded;
        uint32_t first[TINYIMG_WORK_ENTROPY_BYTES + 1];

        for (int pass = 0; pass < 2; pass++) {
            tiny_work_reset();
            r |= assertEquals(
                tiny_image_decode(&decoded, bytes, file, 0), TINYIMG_OK
            );
            tiny_image_destroy(&decoded);

            // the same request after a reset is the same vector, whatever
            // else the process has done since
            for (int c = 0; c <= TINYIMG_WORK_ENTROPY_BYTES; c++) {
                uint32_t now = tiny_work_read((TinyWorkCounter) c);

                if (pass == 0)
                    first[c] = now;
                else
                    r |= assertEquals((long) now, (long) first[c]);
            }
        }

        // and the same holds for a scaled and a region request
        TinyDecodeOpts shapes[2] = {{0, 0, 0, 0, 4, 0}, {16, 16, 64, 64, 1, 0}};

        for (int shape = 0; shape < 2; shape++) {
            uint32_t seen[TINYIMG_WORK_ENTROPY_BYTES + 1];

            for (int pass = 0; pass < 2; pass++) {
                tiny_work_reset();
                r |= assertEquals(
                    tiny_image_decode(&decoded, bytes, file, &shapes[shape]),
                    TINYIMG_OK
                );
                tiny_image_destroy(&decoded);

                for (int c = 0; c <= TINYIMG_WORK_ENTROPY_BYTES; c++) {
                    uint32_t now = tiny_work_read((TinyWorkCounter) c);

                    if (pass == 0)
                        seen[c] = now;
                    else
                        r |= assertEquals((long) now, (long) seen[c]);
                }
            }
        }

        uint32_t entropy = first[TINYIMG_WORK_ENTROPY_BYTES];

        r |= assertTrue(entropy > 0);
        r |= assertTrue(entropy <= file);
        r |= assertTrue(
            (long) entropy * 1000L >= compressed[i].floor * (long) file
        );

        printf(
            "# %s: %u of %zu entropy bytes (%.1f%%)\n", compressed[i].name,
            entropy, file, 100.0 * entropy / (double) file
        );

        free(bytes);
    }

    // a raw layout has no entropy coded stream to consume
    static const char* raw[] = {
        "derived/base-uncompressed.tif", "derived/base.bmp", "dartmouth.tiff"
    };

    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(decodeWith(raw[i], &decoded, 0), TINYIMG_OK);
        r |=
            assertEquals((long) tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES), 0L);
        r |= assertTrue(tiny_work_read(TINYIMG_WORK_DECODED_SAMPLES) > 0);
        tiny_image_destroy(&decoded);
    }

    // a progressive file at a denominator of eight steps over its AC scans,
    // and those bytes are not consumed
    static const char* progressive[] = {
        "derived/base-progressive.jpg", "road.jpg"
    };

    for (size_t i = 0; i < 2; i++) {
        TinyDecodeOpts one = {0, 0, 0, 0, 1, 3};
        TinyDecodeOpts eight = {0, 0, 0, 0, 8, 3};
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(
            decodeWith(progressive[i], &decoded, &one), TINYIMG_OK
        );
        uint32_t whole = tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES);
        r |=
            assertEquals((long) tiny_work_read(TINYIMG_WORK_SCANS_SKIPPED), 0L);
        tiny_image_destroy(&decoded);

        tiny_work_reset();
        r |= assertEquals(
            decodeWith(progressive[i], &decoded, &eight), TINYIMG_OK
        );
        r |= assertTrue(tiny_work_read(TINYIMG_WORK_SCANS_SKIPPED) > 0);
        r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) < whole);
        tiny_image_destroy(&decoded);
    }

    // a JPEG region still reads the whole scan to reach its rows, so it is
    // bounded by the full decode and never above it
    {
        TinyDecodeOpts full = {0, 0, 0, 0, 1, 3};
        TinyDecodeOpts part = {80, 40, 64, 32, 1, 3};
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(decodeWith("sf-24.jpg", &decoded, &full), TINYIMG_OK);
        uint32_t whole = tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES);
        tiny_image_destroy(&decoded);

        tiny_work_reset();
        r |= assertEquals(decodeWith("sf-24.jpg", &decoded, &part), TINYIMG_OK);
        r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) <= whole);
        tiny_image_destroy(&decoded);
    }

    // a lossy WebP band at the top stops after its macroblock rows, so it
    // consumes fewer bytes than the frame; a PNG band stops inflating
    {
        static const struct {
            const char* name;
            TinyDecodeOpts band;
        } bands[] = {
            {"toyota_racing.webp", {0, 0, 640, 40, 1, 3}},
            {"derived/base-lossy.webp", {0, 0, 320, 32, 1, 3}},
            {"forest.png", {0, 0, 640, 40, 1, 3}},
        };

        for (size_t i = 0; i < 3; i++) {
            TinyDecodeOpts full = {0, 0, 0, 0, 1, 3};
            TinyImage decoded;

            tiny_work_reset();
            r |= assertEquals(
                decodeWith(bands[i].name, &decoded, &full), TINYIMG_OK
            );
            uint32_t whole = tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES);
            tiny_image_destroy(&decoded);

            tiny_work_reset();
            r |= assertEquals(
                decodeWith(bands[i].name, &decoded, &bands[i].band), TINYIMG_OK
            );
            r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) < whole);
            r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) > 0);
            tiny_image_destroy(&decoded);
        }
    }

    // AVIF reports its samples like every other decoder does
    {
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(
            decodeWith("derived/base.avif", &decoded, 0), TINYIMG_OK
        );
        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_SOURCE_SAMPLES), 320L * 180L
        );
        r |= assertEquals(
            (long) tiny_work_read(TINYIMG_WORK_DECODED_SAMPLES),
            (long) (decoded.width * decoded.height)
        );
        tiny_image_destroy(&decoded);
    }

    // #endregion

    // #region encode probes

    /*
     * ENCODED is the same for every PNG of one size and the time is not, so
     * the match finder reports its own search. The compression level is what
     * bounds it: none walks nothing, and each longer chain walks at least as
     * many candidates.
     */
    {
        TinyDecodeOpts quarter = {0, 0, 0, 0, 4, 3};
        TinyImage source;

        r |= assertEquals(
            decodeWith("sf-24.jpg", &source, &quarter), TINYIMG_OK
        );

        static const uint8_t levels[4] = {
            TINYIMG_COMPRESSION_NONE, TINYIMG_COMPRESSION_FAST,
            TINYIMG_COMPRESSION_DEFAULT, TINYIMG_COMPRESSION_BEST
        };
        uint32_t previous = 0;

        for (int i = 0; i < 4; i++) {
            TinyEncodeOpts opts = {82, 0, 0, 0, 0, levels[i]};
            uint32_t seen[2] = {0, 0};

            for (int pass = 0; pass < 2; pass++) {
                TinyWriter out;
                r |= assertEquals(tiny_writer_init(&out, 1024), TINYIMG_OK);

                tiny_work_reset();
                r |= assertEquals(
                    tiny_image_encode(&source, TINYIMG_FORMAT_PNG, &opts, &out),
                    TINYIMG_OK
                );
                seen[pass] = tiny_work_read(TINYIMG_WORK_ENCODE_PROBES);
                tiny_writer_free(&out);
            }

            r |= assertEquals((long) seen[0], (long) seen[1]);

            if (i == 0) r |= assertEquals((long) seen[0], 0L);
            if (i == 1) r |= assertTrue(seen[0] > 0);

            r |= assertTrue(seen[0] >= previous);
            previous = seen[0];
        }

        // a decode never searches for matches
        tiny_work_reset();
        TinyImage again;
        r |=
            assertEquals(decodeWith("derived/base.png", &again, 0), TINYIMG_OK);
        r |=
            assertEquals((long) tiny_work_read(TINYIMG_WORK_ENCODE_PROBES), 0L);

        tiny_image_destroy(&again);
        tiny_image_destroy(&source);
    }

    // #endregion

    // #region alpha dropped

    /*
     * The encoder that cannot write alpha counts what it threw away, so
     * reporting it costs no second pass. JPEG and AVIF lose every sample under
     * 255; GIF keeps a one-bit mask and loses only the samples between.
     */
    {
        enum
        {
            W = 32,
            H = 16
        };

        static const struct {
            TinyImageFormat format;
            uint8_t lossless;
            // 1 counts every partial sample, 2 counts every sample under 255
            int counts;
        } cases[] = {{TINYIMG_FORMAT_JPEG, 0, 2}, {TINYIMG_FORMAT_AVIF, 0, 2},
                     {TINYIMG_FORMAT_GIF, 0, 1},  {TINYIMG_FORMAT_PNG, 0, 0},
                     {TINYIMG_FORMAT_WEBP, 0, 0}, {TINYIMG_FORMAT_WEBP, 1, 0},
                     {TINYIMG_FORMAT_TIFF, 0, 0}, {TINYIMG_FORMAT_BMP, 0, 0}};

        // 0 is RGB, 1 an alpha ramp, 2 a one-bit mask, 3 opaque RGBA
        for (int kind = 0; kind < 4; kind++) {
            TinyImage image;
            uint8_t channels = kind == 0 ? 3 : 4;

            r |= assertEquals(tiny_image_create(&image, W, H, channels), 0);

            long partial = 0;
            long below = 0;

            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    uint8_t* p = image.data + ((size_t) y * W + x) * channels;
                    p[0] = (uint8_t) (x * 8);
                    p[1] = (uint8_t) (y * 16);
                    p[2] = (uint8_t) (x * 4 + y);

                    if (kind == 0) continue;

                    uint8_t a = 255;
                    if (kind == 1) a = (uint8_t) (x * 255 / (W - 1));
                    if (kind == 2) a = x < W / 2 ? 0 : 255;

                    p[3] = a;
                    partial += a != 0 && a != 255;
                    below += a != 255;
                }
            }

            for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
                TinyEncodeOpts opts = {75, cases[c].lossless, 0, 0, 0, 0};
                long seen[2] = {0, 0};

                for (int pass = 0; pass < 2; pass++) {
                    TinyWriter out;
                    r |= assertEquals(tiny_writer_init(&out, 1024), TINYIMG_OK);

                    tiny_work_reset();
                    r |= assertEquals(
                        tiny_image_encode(&image, cases[c].format, &opts, &out),
                        TINYIMG_OK
                    );
                    seen[pass] =
                        (long) tiny_work_read(TINYIMG_WORK_ALPHA_DROPPED);
                    tiny_writer_free(&out);
                }

                long want = cases[c].counts == 2   ? below
                            : cases[c].counts == 1 ? partial
                                                   : 0;

                r |= assertEquals(seen[0], seen[1]);
                r |= assertEquals(seen[0], want);
            }

            tiny_image_destroy(&image);
        }
    }

    // a decode never drops alpha
    tiny_work_reset();
    {
        TinyImage again;
        r |=
            assertEquals(decodeWith("derived/base.png", &again, 0), TINYIMG_OK);
        r |=
            assertEquals((long) tiny_work_read(TINYIMG_WORK_ALPHA_DROPPED), 0L);
        tiny_image_destroy(&again);
    }

    // #endregion

    // #region the work clock is a function of the counters

    // off, the clock is absolute and two runs of one request read alike
    {
        uint32_t reading[2];

        for (int pass = 0; pass < 2; pass++) {
            TinyImage decoded;

            tiny_work_set_limit(0);
            tiny_work_reset();
            r |= assertEquals(decodeWith("sf-24.jpg", &decoded, 0), TINYIMG_OK);
            reading[pass] = tiny_work_clock_us();
            tiny_image_destroy(&decoded);
        }

        r |= assertTrue(reading[0] > 0);
        r |= assertEquals((long) reading[0], (long) reading[1]);

        // the decode term is charged once and the byte term follows the stream
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(decodeWith("sf-24.jpg", &decoded, 0), TINYIMG_OK);
        r |= assertTrue(tiny_work_read(TINYIMG_WORK_DECODE_MICROS) > 0);
        r |= assertTrue(
            tiny_work_clock_us() > tiny_work_read(TINYIMG_WORK_DECODE_MICROS)
        );
        tiny_image_destroy(&decoded);
    }

    // FAST is charged as a saving only where a decoder has one to take: a
    // lossless WebP has no filter to skip
    {
        TinyDecodeOpts fancy = {0, 0, 0, 0, 2, 0, TINYIMG_EFFORT_FANCY};
        TinyDecodeOpts fast = {0, 0, 0, 0, 2, 0, TINYIMG_EFFORT_FAST};
        uint32_t charged[2][2];
        static const char* webps[2] = {
            "derived/base-lossless.webp", "derived/base.avif"
        };

        for (int w = 0; w < 2; w++) {
            TinyImage decoded;

            tiny_work_reset();
            r |= assertEquals(
                decodeWith(webps[w], &decoded, &fancy), TINYIMG_OK
            );
            charged[w][0] = tiny_work_read(TINYIMG_WORK_DECODE_MICROS);
            tiny_image_destroy(&decoded);

            tiny_work_reset();
            r |=
                assertEquals(decodeWith(webps[w], &decoded, &fast), TINYIMG_OK);
            charged[w][1] = tiny_work_read(TINYIMG_WORK_DECODE_MICROS);
            tiny_image_destroy(&decoded);
        }

        r |= assertEquals((long) charged[0][1], (long) charged[0][0]);
        r |= assertTrue(charged[1][1] < charged[1][0]);
    }

    // a limit counts from the call that sets it, so earlier work is not owed
    {
        TinyImage decoded;

        tiny_work_reset();
        r |= assertEquals(decodeWith("sf-24.jpg", &decoded, 0), TINYIMG_OK);
        uint32_t before = tiny_work_clock_us();
        tiny_image_destroy(&decoded);

        tiny_work_set_limit(0xFFFFFFFFu);
        r |= assertEquals((long) tiny_work_clock_us(), 0L);
        r |= assertEquals(tiny_work_over_limit(), 0);

        r |= assertEquals(decodeWith("sf-24.jpg", &decoded, 0), TINYIMG_OK);
        r |= assertEquals((long) tiny_work_clock_us(), (long) before);
        tiny_image_destroy(&decoded);

        // zero removes the limit and makes the clock absolute again
        tiny_work_set_limit(0);
        long twice = (long) tiny_work_clock_us() - (long) (before * 2);
        r |= assertTrue(twice >= -1 && twice <= 1);
    }

    // #endregion

    // #region a limit stops every decoder and leaves the heap as it found it

    static const char* sources[] = {
        "sf-24.jpg",
        "derived/base-progressive.jpg",
        "derived/base.png",
        "derived/base-interlaced.png",
        "derived/base-lossy.webp",
        "derived/base-lossless.webp",
        "derived/base.gif",
        "derived/base-lzw.tif",
        "derived/base-deflate.tif",
        "derived/base-uncompressed.tif",
        "derived/base.bmp",
        "derived/base-rle8.bmp",
        "derived/base.avif",
        "derived/av1-tiles.avif",
    };

    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        TinyImage decoded;
        TinyHeapStats warm;
        TinyHeapStats after;

        // the arena keeps what the first run grew it to, so the baseline is a
        // finished decode rather than a cold heap
        tiny_work_set_limit(0);
        r |= assertEquals(decodeWith(sources[i], &decoded, 0), TINYIMG_OK);
        uint32_t plain = digest(&decoded);
        tiny_image_destroy(&decoded);
        tiny_heap_stats(&warm);

        tiny_work_reset();
        tiny_work_set_limit(1);
        int stopped = decodeWith(sources[i], &decoded, 0);
        tiny_work_set_limit(0);
        tiny_heap_stats(&after);

        if (stopped != TINYIMG_ERR_BUDGET)
            printf("# not stopped: %s\n", sources[i]);

        r |= assertEquals(stopped, TINYIMG_ERR_BUDGET);
        r |= assertEquals((long) after.used, (long) warm.used);

        // a limit nothing reaches changes no byte
        tiny_work_set_limit(0xFFFFFFFFu);
        r |= assertEquals(decodeWith(sources[i], &decoded, 0), TINYIMG_OK);
        r |= assertEquals((long) digest(&decoded), (long) plain);
        tiny_image_destroy(&decoded);
        tiny_work_set_limit(0);
    }

    // the error is the budget's and nothing else's: an image past the pixel
    // ceiling is still too large with a limit set, and a corrupt one corrupt
    {
        size_t size = 0;
        unsigned char* bytes = readFixture("derived/base.png", &size);
        TinyImage decoded;

        if (bytes) {
            unsigned char* wide = (unsigned char*) malloc(size);
            memcpy(wide, bytes, size);

            // 20000 by 20000, with the header's own checksum rewritten
            wide[16] = 0x00, wide[17] = 0x00, wide[18] = 0x4E, wide[19] = 0x20;
            wide[20] = 0x00, wide[21] = 0x00, wide[22] = 0x4E, wide[23] = 0x20;
            uint32_t crc = tiny_crc32(0, wide + 12, 17);
            wide[29] = (unsigned char) (crc >> 24);
            wide[30] = (unsigned char) (crc >> 16);
            wide[31] = (unsigned char) (crc >> 8);
            wide[32] = (unsigned char) crc;

            tiny_work_set_limit(0);
            int unlimited = tiny_image_decode(&decoded, wide, size, 0);

            tiny_work_set_limit(1);
            int limited = tiny_image_decode(&decoded, wide, size, 0);
            tiny_work_set_limit(0);

            r |= assertEquals(unlimited, TINYIMG_ERR_TOO_LARGE);
            r |= assertEquals(limited, unlimited);

            free(wide);
            free(bytes);
        }
    }

    // #endregion

    // #region a limit stops an encoder and leaves the heap as it found it

    {
        static const struct {
            TinyImageFormat format;
            uint8_t lossless;
            const char* name;
        } writers[] = {
            {TINYIMG_FORMAT_JPEG, 0, "jpeg"},
            {TINYIMG_FORMAT_PNG, 0, "png"},
            {TINYIMG_FORMAT_WEBP, 0, "webp"},
            {TINYIMG_FORMAT_WEBP, 1, "webp lossless"},
            {TINYIMG_FORMAT_GIF, 0, "gif"},
            {TINYIMG_FORMAT_TIFF, 0, "tiff"},
            {TINYIMG_FORMAT_BMP, 0, "bmp"},
            {TINYIMG_FORMAT_AVIF, 0, "avif"},
        };

        TinyImage source;
        r |= assertEquals(
            decodeWith("derived/base.png", &source, 0), TINYIMG_OK
        );

        for (size_t i = 0; i < sizeof(writers) / sizeof(writers[0]); i++) {
            TinyEncodeOpts opts = {0, writers[i].lossless, 0, 0, 0, 0};
            TinyWriter out;
            TinyHeapStats warm;
            TinyHeapStats after;

            tiny_work_set_limit(0);
            tiny_writer_init(&out, 0);
            r |= assertEquals(
                tiny_image_encode(&source, writers[i].format, &opts, &out),
                TINYIMG_OK
            );
            uint32_t plain = bufferDigest(tiny_writer_data(&out), out.size);
            tiny_writer_free(&out);
            tiny_heap_stats(&warm);

            tiny_work_reset();
            tiny_work_set_limit(1);
            tiny_writer_init(&out, 0);
            int stopped =
                tiny_image_encode(&source, writers[i].format, &opts, &out);
            tiny_work_set_limit(0);
            tiny_writer_free(&out);
            tiny_heap_stats(&after);

            if (stopped != TINYIMG_ERR_BUDGET)
                printf("# not stopped: %s\n", writers[i].name);

            r |= assertEquals(stopped, TINYIMG_ERR_BUDGET);
            r |= assertEquals((long) after.used, (long) warm.used);

            tiny_work_set_limit(0xFFFFFFFFu);
            tiny_writer_init(&out, 0);
            r |= assertEquals(
                tiny_image_encode(&source, writers[i].format, &opts, &out),
                TINYIMG_OK
            );
            r |= assertEquals(
                (long) bufferDigest(tiny_writer_data(&out), out.size),
                (long) plain
            );
            tiny_writer_free(&out);
            tiny_work_set_limit(0);
        }

        tiny_image_destroy(&source);
    }

    // #endregion

    // #region an abort can land part way through an image

    /*
     * A limit halfway between the decode's sample term and its total fires
     * after some of the stream has been read and before all of it has. Without
     * the polls inside the row loops the decoder would run to the end and
     * return success, so this is the test that fails when they are removed.
     */
    {
        static const char* streams[] = {
            "sf-24.jpg", "forest.png", "derived/base-lossless.webp",
            "toyota_racing.webp"
        };

        for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++) {
            TinyImage decoded;

            tiny_work_set_limit(0);
            tiny_work_reset();
            r |= assertEquals(decodeWith(streams[i], &decoded, 0), TINYIMG_OK);
            tiny_image_destroy(&decoded);

            uint32_t sample = tiny_work_read(TINYIMG_WORK_DECODE_MICROS);
            uint32_t total = tiny_work_clock_us();
            uint32_t bytes = tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES);

            r |= assertTrue(total > sample);

            tiny_work_reset();
            tiny_work_set_limit(sample + (total - sample) / 2u);
            int stopped = decodeWith(streams[i], &decoded, 0);
            tiny_work_set_limit(0);

            r |= assertEquals(stopped, TINYIMG_ERR_BUDGET);
            r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) > 0);
            r |= assertTrue(tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES) < bytes);
        }
    }

    // #endregion

    // #region the counters are a total, and reset clears them

    tiny_work_reset();
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_BLOCKS), 0L);
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_TRANSFORMS), 0L);
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_ENTROPY_BYTES), 0L);
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_ENCODE_PROBES), 0L);
    r |= assertEquals((long) tiny_work_read(TINYIMG_WORK_ALPHA_DROPPED), 0L);

    // out of range reads answer zero rather than reading past the array
    r |= assertEquals((long) tiny_work_read((TinyWorkCounter) 999), 0L);

    // #endregion

    tiny_arena_reset();
    return r;
}
