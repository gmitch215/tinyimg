#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

/** Decodes a fixture with the given options, or reports why it could not. */
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

static int decodeFixture(const char* name, TinyImage* image, uint8_t channels) {
    TinyDecodeOpts opts = {0, 0, 0, 0, 1, channels};
    return decodeWith(name, image, &opts);
}

static int probeFixture(const char* name, TinyImageInfo* info) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    int result = tiny_image_probe(bytes, size, info);

    free(bytes);
    return result;
}

int main(void) {
    int r = 0;

    TinyImage image;
    TinyImageInfo info;

    // #region header reading

    r |= assertEquals(probeFixture("derived/base-444.jpg", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.progressive, 0L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_JPEG);

    r |= assertEquals(
        probeFixture("derived/base-progressive.jpg", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.progressive, 1L);
    r |= assertEquals((long) info.channels, 3L);

    // a grayscale file says one channel, and a four component one still says
    // three, because CMYK and YCCK both come out as RGB
    r |= assertEquals(probeFixture("derived/base-gray.jpg", &info), TINYIMG_OK);
    r |= assertEquals((long) info.channels, 1L);

    r |= assertEquals(probeFixture("derived/base-cmyk.jpg", &info), TINYIMG_OK);
    r |= assertEquals((long) info.channels, 3L);

    // road.jpg is the only progressive source fixture, and a node test asserts
    // it stays the only one
    r |= assertEquals(probeFixture("road.jpg", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 1281L);
    r |= assertEquals((long) info.height, 1920L);
    r |= assertEquals((long) info.progressive, 1L);

    // #endregion

    // #region entropy coding is not part of the picture

    /*
     * A progressive stream and a sequential one carrying the same coefficients
     * have to decode to the same pixels, byte for byte.
     *
     * This is the strongest check in the file and it needs no reference:
     * progressive coding differs from sequential only in how the coefficients
     * are split across scans, so spectral selection, successive approximation,
     * the EOB run and the correction bits all have to be exactly right for the
     * two to land on one answer. Both fixtures are 4:4:4 at the same quality
     * from the same source, so the coefficients really are identical.
     */
    TinyImage sequential;
    TinyImage progressive;

    r |= assertEquals(
        decodeFixture("derived/base-444.jpg", &sequential, 3), TINYIMG_OK
    );
    r |= assertEquals(
        decodeFixture("derived/base-progressive.jpg", &progressive, 3),
        TINYIMG_OK
    );
    r |= assertImageEquals(&progressive, &sequential);
    tiny_image_destroy(&progressive);

    // #endregion

    // #region against the picture the fixtures were made from

    TinyImage source;
    r |=
        assertEquals(decodeFixture("derived/base.png", &source, 3), TINYIMG_OK);

    size_t pixels = (size_t) 320 * 180 * 3;

    // quality 92, so the error is small and it is chroma that carries it: full
    // resolution chroma is nearly transparent, halving it costs about 5 dB and
    // halving both axes another 4
    r |= assertPSNR(sequential.data, source.data, pixels, 41.0);
    tiny_image_destroy(&sequential);

    static const struct {
        const char* name;
        double floorDb;
    } lossy[] = {
        {"derived/base-422.jpg", 37.0},
        {"derived/base-420.jpg", 33.0},
        {"derived/base-411.jpg", 31.0},
        {"derived/base-restart.jpg", 33.0},
        // CMYK went through a color space round trip, which is what the
        // higher floor is: the quantization is the same
        {"derived/base-cmyk.jpg", 44.0}
    };

    for (size_t i = 0; i < sizeof(lossy) / sizeof(lossy[0]); i++) {
        r |= assertEquals(decodeFixture(lossy[i].name, &image, 3), TINYIMG_OK);
        r |= assertEquals((long) image.width, 320L);
        r |= assertEquals((long) image.height, 180L);
        r |= assertPSNR(image.data, source.data, pixels, lossy[i].floorDb);
        tiny_image_destroy(&image);
    }

    // the restart fixture is 4:2:0 with a restart marker every four MCUs, so it
    // has to agree with the plain 4:2:0 file exactly. Resynchronizing wrongly
    // would shift a color band, not lose a decibel
    TinyImage plain;
    TinyImage restarted;

    r |= assertEquals(
        decodeFixture("derived/base-420.jpg", &plain, 3), TINYIMG_OK
    );
    r |= assertEquals(
        decodeFixture("derived/base-restart.jpg", &restarted, 3), TINYIMG_OK
    );
    r |= assertImageEquals(&restarted, &plain);
    tiny_image_destroy(&restarted);

    // #endregion

    // #region channel counts

    static const uint8_t counts[4] = {1, 2, 3, 4};

    for (size_t i = 0; i < 4; i++) {
        r |= assertEquals(
            decodeFixture("derived/base-444.jpg", &image, counts[i]), TINYIMG_OK
        );
        r |= assertEquals((long) image.channels, (long) counts[i]);

        // JPEG carries no transparency, so an alpha channel it did not have
        // comes out opaque
        if (counts[i] == 2 || counts[i] == 4) {
            int opaque = 1;

            for (size_t p = 0; p < (size_t) 320 * 180; p++) {
                if (image.data[p * counts[i] + counts[i] - 1] != 255) {
                    opaque = 0;
                }
            }
            r |= assertTrue(opaque);
        }

        tiny_image_destroy(&image);
    }

    // a grayscale file asked for one channel is the file's own samples, so it
    // has to match the luminance of the color decode closely
    r |= assertEquals(
        decodeFixture("derived/base-gray.jpg", &image, 1), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 1L);
    tiny_image_destroy(&image);

    // #endregion

    // #region region decode

    /*
     * A region has to be the same bytes as the same rectangle of a full decode.
     *
     * The offsets are deliberately odd. An even one would hide the chroma
     * context: the upsampler's triangle filter reaches a sample either side, so
     * a region that starts mid chroma sample only comes out right if the plane
     * window carries the row and column outside it.
     */
    static const char* streams[] = {
        "derived/base-420.jpg", "derived/base-progressive.jpg",
        "derived/base-411.jpg"
    };

    static const uint32_t boxes[4][4] = {
        {0, 0, 64, 64}, {17, 23, 77, 55}, {100, 50, 128, 96}, {319, 179, 1, 1}
    };

    for (size_t s = 0; s < sizeof(streams) / sizeof(streams[0]); s++) {
        TinyImage whole;
        r |= assertEquals(decodeFixture(streams[s], &whole, 3), TINYIMG_OK);

        for (size_t b = 0; b < 4; b++) {
            TinyDecodeOpts opts = {boxes[b][0], boxes[b][1], boxes[b][2],
                                   boxes[b][3], 1,           3};

            r |=
                assertEquals(decodeWith(streams[s], &image, &opts), TINYIMG_OK);
            r |= assertEquals((long) image.width, (long) boxes[b][2]);
            r |= assertEquals((long) image.height, (long) boxes[b][3]);
            r |= assertMatchesCrop(&image, &whole, boxes[b][0], boxes[b][1]);
            tiny_image_destroy(&image);
        }

        tiny_image_destroy(&whole);
    }

    // a region starting outside the image is a caller error, not a clamp
    TinyDecodeOpts outside = {320, 0, 8, 8, 1, 3};
    r |= assertEquals(
        decodeWith("derived/base-444.jpg", &image, &outside), TINYIMG_ERR_BOUNDS
    );

    // #endregion

    // #region scaled decode

    static const uint8_t denominators[4] = {1, 2, 4, 8};
    static const uint32_t widths[4] = {320, 160, 80, 40};
    static const uint32_t heights[4] = {180, 90, 45, 23};

    for (size_t i = 0; i < 4; i++) {
        TinyDecodeOpts opts = {0, 0, 0, 0, denominators[i], 3};

        r |= assertEquals(
            decodeWith("derived/base-420.jpg", &image, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) widths[i]);
        r |= assertEquals((long) image.height, (long) heights[i]);
        tiny_image_destroy(&image);

        // the same must hold for a progressive stream, which reaches the
        // transform from a coefficient plane rather than block by block
        r |= assertEquals(
            decodeWith("derived/base-progressive.jpg", &image, &opts),
            TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) widths[i]);
        r |= assertEquals((long) image.height, (long) heights[i]);
        tiny_image_destroy(&image);
    }

    // an unsupported denominator reads as 1 rather than failing
    TinyDecodeOpts odd = {0, 0, 0, 0, 3, 3};
    r |= assertEquals(
        decodeWith("derived/base-444.jpg", &image, &odd), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);
    tiny_image_destroy(&image);

    /*
     * A scaled region, aligned to the scale, equals the same rectangle of a
     * scaled full decode.
     *
     * Alignment is required rather than incidental: a scaled plane has no
     * sample at a finer offset than the denominator, so the codec rounds the
     * region origin down and says so in its header.
     */
    for (uint8_t den = 2; den <= 4; den = (uint8_t) (den * 2)) {
        TinyDecodeOpts full = {0, 0, 0, 0, den, 3};
        TinyImage scaled;

        r |= assertEquals(
            decodeWith("derived/base-420.jpg", &scaled, &full), TINYIMG_OK
        );

        TinyDecodeOpts part = {64, 32, 128, 64, den, 3};
        r |= assertEquals(
            decodeWith("derived/base-420.jpg", &image, &part), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, (long) (128u / den));
        r |= assertMatchesCrop(&image, &scaled, 64u / den, 32u / den);

        tiny_image_destroy(&image);
        tiny_image_destroy(&scaled);
    }

    /*
     * A reduced decode is the area average of the full one, so averaging the
     * full decode over the same boxes has to land on it.
     *
     * A single component file, because with three the chroma upsampler and the
     * color conversion sit between the two decodes and neither commutes with an
     * average, which makes a three channel comparison measure those instead.
     *
     * The tolerances are measured rather than chosen. Halves and quarters fold
     * the average into the transform and land within one level, which is the
     * rounding: this averages bytes that were already rounded, the codec rounds
     * once at the end. The eighth is a block's DC term, whose average covers
     * the padding an incomplete edge block carries, so only whole boxes are
     * compared and its floor is looser.
     */
    TinyImage gray;
    r |= assertEquals(
        decodeFixture("derived/base-gray.jpg", &gray, 1), TINYIMG_OK
    );

    static const struct {
        uint8_t den;
        uint32_t width;
        uint32_t height;
        int tolerance;
    } reduced[3] = {{2, 160, 90, 1}, {4, 80, 45, 1}, {8, 40, 23, 2}};

    for (size_t i = 0; i < 3; i++) {
        TinyDecodeOpts opts = {0, 0, 0, 0, reduced[i].den, 1};
        TinyImage small;

        r |= assertEquals(
            decodeWith("derived/base-gray.jpg", &small, &opts), TINYIMG_OK
        );
        r |= assertEquals((long) small.width, (long) reduced[i].width);
        r |= assertEquals((long) small.height, (long) reduced[i].height);

        uint32_t den = reduced[i].den;
        uint32_t across = gray.width / den;
        uint32_t down = gray.height / den;
        int close = 1;
        int worst = 0;

        for (uint32_t by = 0; by < down; by++) {
            for (uint32_t bx = 0; bx < across; bx++) {
                uint32_t sum = 0;

                for (uint32_t y = 0; y < den; y++) {
                    for (uint32_t x = 0; x < den; x++) {
                        sum += gray.data
                                   [(size_t) (by * den + y) * gray.width +
                                    bx * den + x];
                    }
                }

                int mean = (int) ((sum + den * den / 2) / (den * den));
                int got = small.data[(size_t) by * small.width + bx];
                int delta = got > mean ? got - mean : mean - got;

                if (delta > worst) worst = delta;
                if (delta > reduced[i].tolerance) close = 0;
            }
        }

        if (!close) {
            printf(
                "1/%u drifted %d levels from the area average, over %d\n", den,
                worst, reduced[i].tolerance
            );
        }
        r |= assertTrue(close);

        tiny_image_destroy(&small);
    }

    tiny_image_destroy(&gray);

    /*
     * The largest source in the set, both ways round.
     *
     * digicam.jpg is 3600x2700, which is 29.2 MB of RGB against the 32 MiB
     * TINYIMG_MAX_IMAGE_BYTES cap, so a whole decode is the closest any fixture
     * comes to the ceiling and has to succeed. Asking for four channels of the
     * same picture is 38.9 MB and has to be refused with the specific error,
     * which is the pair that says the cap is a budget and not an extent.
     *
     * The scaled decode is what a request actually takes: 1/8 of this source is
     * 450x338, and it is the reason a source this size is usable at all.
     */
    TinyImage large;
    r |= assertEquals(decodeFixture("digicam.jpg", &large, 3), TINYIMG_OK);
    r |= assertEquals((long) large.width, 3600L);
    r |= assertEquals((long) large.height, 2700L);

    TinyImage magick;
    r |= assertEquals(
        decodeFixture("derived/ref/digicam.crop.png", &magick, 3), TINYIMG_OK
    );
    r |= assertMatchesCrop(&magick, &large, 1200, 900);

    tiny_image_destroy(&magick);
    tiny_image_destroy(&large);

    r |= assertEquals(
        decodeFixture("digicam.jpg", &image, 4), TINYIMG_ERR_TOO_LARGE
    );

    TinyDecodeOpts eighth = {0, 0, 0, 0, 8, 3};
    r |= assertEquals(decodeWith("digicam.jpg", &image, &eighth), TINYIMG_OK);
    r |= assertEquals((long) image.width, 450L);
    r |= assertEquals((long) image.height, 338L);
    tiny_image_destroy(&image);

    // #endregion

    // #region malformed input

    // the empty fixture cannot come through readFixture, which has no way to
    // return a zero length buffer, so the empty case is put in directly
    r |= assertEquals(
        tiny_image_load(&image, (const uint8_t*) "", 0),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );
    r |= assertEquals(
        decodeFixture("derived/malformed/empty.jpg", &image, 3),
        TINYIMG_ERR_NOT_FOUND
    );
    r |= assertEquals(
        decodeFixture("derived/malformed/not-an-image.bin", &image, 3),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    // a valid signature over garbage: the third byte is part of the signature,
    // because a real file's SOI is always followed by another marker
    r |= assertEquals(
        decodeFixture("derived/malformed/signature-only.jpg", &image, 3),
        TINYIMG_ERR_UNKNOWN_FORMAT
    );

    // four bytes of marker and nothing to decode
    r |= assertEquals(
        decodeFixture("derived/malformed/header-only.jpg", &image, 3),
        TINYIMG_ERR_CORRUPT
    );

    // past the pixel budget, which reports its own error so the caller knows to
    // ask for a scaled decode instead
    r |= assertEquals(
        decodeFixture("derived/oversized.jpg", &image, 3), TINYIMG_ERR_TOO_LARGE
    );

    // and the header still reads, because a probe that cannot answer for an
    // oversized file is no use for deciding what to ask for
    r |= assertEquals(probeFixture("derived/oversized.jpg", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 5000L);
    r |= assertEquals((long) info.height, 4000L);

    // at a scale that fits, it decodes
    TinyDecodeOpts smaller = {0, 0, 0, 0, 8, 3};
    r |= assertEquals(
        decodeWith("derived/oversized.jpg", &image, &smaller), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 625L);
    r |= assertEquals((long) image.height, 500L);
    tiny_image_destroy(&image);

    // so does a region of it, which is the other way out of the budget
    TinyDecodeOpts window = {1000, 800, 512, 512, 1, 3};
    r |= assertEquals(
        decodeWith("derived/oversized.jpg", &image, &window), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 512L);
    tiny_image_destroy(&image);

    /*
     * A truncated scan decodes rather than failing, which is deliberate.
     *
     * The format is built so that a reader can stop early: the entropy decoder
     * treats missing bits as zeros, so the rows that arrived are correct and
     * the rest is flat. Every other decoder behaves this way, and rejecting the
     * file would break the case the design exists for. What is asserted here is
     * that it stays bounded and keeps the header's dimensions.
     */
    r |= assertEquals(
        decodeFixture("derived/malformed/truncated.jpg", &image, 3), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);
    r |= assertEquals((long) image.height, 180L);
    tiny_image_destroy(&image);

    // #endregion

    // #region encoding

    /** Encodes an image and hands back the bytes, which the caller frees. */
    TinyImage subject;
    r |= assertEquals(
        decodeFixture("derived/base.png", &subject, 3), TINYIMG_OK
    );

    static const uint8_t qualities[4] = {40, 60, 85, 92};
    size_t sizes[4] = {0, 0, 0, 0};
    double scores[4] = {0, 0, 0, 0};

    for (size_t i = 0; i < 4; i++) {
        TinyEncodeOpts opts = {qualities[i], 0, 0, 0, 0};
        TinyWriter out;

        r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
        r |= assertEquals(
            tiny_image_encode(&subject, TINYIMG_FORMAT_JPEG, &opts, &out),
            TINYIMG_OK
        );
        r |= assertTrue(out.size > 0);

        // and it reads back through this codec's own decoder at the same size
        r |= assertEquals(
            tiny_image_decode(&image, out.data, out.size, 0), TINYIMG_OK
        );
        r |= assertEquals((long) image.width, 320L);
        r |= assertEquals((long) image.height, 180L);
        r |= assertEquals((long) image.channels, 3L);

        sizes[i] = out.size;
        scores[i] = computePSNR(image.data, subject.data, pixels);

        tiny_image_destroy(&image);
        tiny_writer_free(&out);
    }

    // quality has to buy something in both directions, or the mapping is wrong
    for (size_t i = 1; i < 4; i++) {
        r |= assertGreaterThan((double) sizes[i], (double) sizes[i - 1]);
        r |= assertGreaterThan(scores[i], scores[i - 1]);
    }

    // quality 85 on a photograph is a well known place to be
    r |= assertIn(scores[2], 30.0, 40.0);

    /*
     * A progressive encode and a baseline one have to decode to the same
     * pixels.
     *
     * The two write the same coefficients through different entropy coding, so
     * this is the encoder's side of the check the decoder has: it caught a gray
     * scan script that never finished DC successive approximation, which cost
     * the DC band's lowest bit and showed up nowhere else, because the file was
     * perfectly valid and simply one step coarser than asked for.
     */
    for (uint32_t channels = 1; channels <= 3; channels += 2) {
        TinyImage source;
        r |= assertEquals(
            decodeFixture("derived/base.png", &source, (uint8_t) channels),
            TINYIMG_OK
        );

        TinyImage flat;
        TinyImage staged;

        TinyEncodeOpts baseline = {85, 0, 0, 0, 0};
        TinyEncodeOpts stepped = {85, 0, 1, 0, 0};

        TinyWriter first;
        TinyWriter second;

        r |= assertEquals(tiny_writer_init(&first, 0), TINYIMG_OK);
        r |= assertEquals(tiny_writer_init(&second, 0), TINYIMG_OK);

        r |= assertEquals(
            tiny_image_encode(&source, TINYIMG_FORMAT_JPEG, &baseline, &first),
            TINYIMG_OK
        );
        r |= assertEquals(
            tiny_image_encode(&source, TINYIMG_FORMAT_JPEG, &stepped, &second),
            TINYIMG_OK
        );

        r |= assertEquals(
            tiny_image_decode(&flat, first.data, first.size, 0), TINYIMG_OK
        );
        r |= assertEquals(
            tiny_image_decode(&staged, second.data, second.size, 0), TINYIMG_OK
        );
        r |= assertImageEquals(&staged, &flat);

        // and the progressive one says so in its header
        r |= assertEquals(
            tiny_image_probe(second.data, second.size, &info), TINYIMG_OK
        );
        r |= assertEquals((long) info.progressive, 1L);
        r |= assertEquals(
            tiny_image_probe(first.data, first.size, &info), TINYIMG_OK
        );
        r |= assertEquals((long) info.progressive, 0L);

        tiny_image_destroy(&flat);
        tiny_image_destroy(&staged);
        tiny_image_destroy(&source);
        tiny_writer_free(&first);
        tiny_writer_free(&second);
    }

    // a single channel source writes a single component file, and an alpha
    // channel is dropped rather than composited against a background this codec
    // would have to invent
    static const struct {
        uint8_t channels;
        uint8_t expected;
    } components[4] = {{1, 1}, {2, 1}, {3, 3}, {4, 3}};

    for (size_t i = 0; i < 4; i++) {
        TinyImage source;
        r |= assertEquals(
            decodeFixture(
                "derived/base-alpha.png", &source, components[i].channels
            ),
            TINYIMG_OK
        );

        TinyEncodeOpts opts = {85, 0, 0, 0, 0};
        TinyWriter out;

        r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
        r |= assertEquals(
            tiny_image_encode(&source, TINYIMG_FORMAT_JPEG, &opts, &out),
            TINYIMG_OK
        );
        r |= assertEquals(
            tiny_image_probe(out.data, out.size, &info), TINYIMG_OK
        );
        r |= assertEquals((long) info.channels, (long) components[i].expected);

        tiny_image_destroy(&source);
        tiny_writer_free(&out);
    }

    // dimensions that are not a whole number of blocks, and the smallest image
    // there is
    static const char* awkward[3] = {
        "derived/single-pixel.png", "derived/tiny-odd.png", "derived/logo.png"
    };

    for (size_t i = 0; i < 3; i++) {
        TinyImage source;
        r |= assertEquals(decodeFixture(awkward[i], &source, 3), TINYIMG_OK);

        for (uint8_t stepped = 0; stepped <= 1; stepped++) {
            TinyEncodeOpts opts = {85, 0, stepped, 0, 0};
            TinyWriter out;

            r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
            r |= assertEquals(
                tiny_image_encode(&source, TINYIMG_FORMAT_JPEG, &opts, &out),
                TINYIMG_OK
            );
            r |= assertEquals(
                tiny_image_decode(&image, out.data, out.size, 0), TINYIMG_OK
            );
            r |= assertEquals((long) image.width, (long) source.width);
            r |= assertEquals((long) image.height, (long) source.height);

            tiny_image_destroy(&image);
            tiny_writer_free(&out);
        }

        tiny_image_destroy(&source);
    }

    // zero quality is the default rather than the worst possible, and a value
    // past the range is clamped instead of wrapping
    TinyEncodeOpts defaulted = {0, 0, 0, 0, 0};
    TinyEncodeOpts absurd = {200, 0, 0, 0, 0};
    TinyWriter one;
    TinyWriter two;

    r |= assertEquals(tiny_writer_init(&one, 0), TINYIMG_OK);
    r |= assertEquals(tiny_writer_init(&two, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&subject, TINYIMG_FORMAT_JPEG, &defaulted, &one),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_encode(&subject, TINYIMG_FORMAT_JPEG, &absurd, &two),
        TINYIMG_OK
    );
    r |= assertEquals((long) one.size, (long) sizes[2]);
    r |= assertGreaterThan((double) two.size, (double) one.size);

    tiny_writer_free(&one);
    tiny_writer_free(&two);

    TinyImage empty = {0, 0, 3, 0, TINYIMG_FORMAT_UNKNOWN, 0, 0};
    TinyWriter nothing;
    r |= assertEquals(tiny_writer_init(&nothing, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&empty, TINYIMG_FORMAT_JPEG, 0, &nothing),
        TINYIMG_ERR_NULL
    );
    tiny_writer_free(&nothing);

    tiny_image_destroy(&subject);

    // #endregion

    // #region blocks with no vertical detail

    /*
     * A picture whose columns do not vary leaves every vertical AC coefficient
     * zero, so each block is one row repeated eight times and the inverse
     * transform computes that row once. The shortcut is only sound if the rows
     * really are identical, which is what this asserts; horizontal detail is
     * deliberately present, because it is the axis that must survive.
     */
    TinyImage columns;
    r |= assertEquals(tiny_image_create(&columns, 64, 32, 3), TINYIMG_OK);

    for (uint32_t y = 0; y < columns.height; y++) {
        for (uint32_t x = 0; x < columns.width; x++) {
            uint8_t* pixel =
                columns.data + ((size_t) y * columns.width + x) * 3;

            pixel[0] = (uint8_t) (x * 4u);
            pixel[1] = (uint8_t) (255u - x * 4u);
            pixel[2] = (x & 1u) ? 30u : 220u;
        }
    }

    TinyEncodeOpts column_opts = {92, 0, 0, 0, 0};
    TinyWriter column_bytes;

    r |= assertEquals(tiny_writer_init(&column_bytes, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(
            &columns, TINYIMG_FORMAT_JPEG, &column_opts, &column_bytes
        ),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_decode(&image, column_bytes.data, column_bytes.size, 0),
        TINYIMG_OK
    );

    int rows_agree = 1;
    uint32_t row_bytes = image.width * image.channels;

    for (uint32_t y = 1; y < image.height; y++) {
        for (uint32_t i = 0; i < row_bytes; i++) {
            if (image.data[(size_t) y * row_bytes + i] != image.data[i]) {
                rows_agree = 0;
            }
        }
    }

    r |= assertTrue(rows_agree);

    // and the horizontal detail is still there, so this is not a flat image
    r |= assertGreaterThan((double) image.data[3 * 3], (double) image.data[0]);

    tiny_image_destroy(&image);
    tiny_image_destroy(&columns);
    tiny_writer_free(&column_bytes);

    // #endregion

    // #region exif

    /*
     * The orientation is read and reported, not applied.
     *
     * Rotating during a decode would be work the planner has to undo: it walks
     * a region backward through every geometry operation to decide what to
     * decode, so a rotation baked in here is one it cannot see through, and one
     * a caller's own rotate would then compose with twice. The tag is stated as
     * metadata instead, under a namespaced key so it cannot collide with the
     * caller's own.
     */
    TinyImage tagged;
    r |= assertEquals(
        decodeFixture("derived/base-exif-rotated.jpg", &tagged, 3), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_has_exif(&tagged), 1);

    // the file says rotate 90 clockwise; the pixels are still as stored
    r |= assertEquals((long) tagged.width, 320L);
    r |= assertEquals((long) tagged.height, 180L);

    char* value = 0;
    r |= assertEquals(
        tiny_image_get_metadata(&tagged, "exif:Orientation", &value), TINYIMG_OK
    );
    r |= assertStringsMatch(value, "6");
    tiny_free(value);

    char* payload = 0;
    size_t payload_size = 0;
    r |= assertEquals(
        tiny_image_get_exif(&tagged, &payload, &payload_size), TINYIMG_OK
    );
    r |= assertTrue(payload_size > 8);

    // the payload starts at its own TIFF header, not at the segment's "Exif"
    // identifier, which is what every other library means by the EXIF block
    r |= assertTrue(
        (payload[0] == 'M' && payload[1] == 'M') ||
        (payload[0] == 'I' && payload[1] == 'I')
    );

    // an unrotated file carries the same block with orientation 1
    TinyImage upright;
    r |= assertEquals(
        decodeFixture("derived/base-exif.jpg", &upright, 3), TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_get_metadata(&upright, "exif:Orientation", &value),
        TINYIMG_OK
    );
    r |= assertStringsMatch(value, "1");
    tiny_free(value);
    tiny_image_destroy(&upright);

    // a file with no APP1 has no metadata at all rather than an empty block
    TinyImage bare;
    r |= assertEquals(
        decodeFixture("derived/base-444.jpg", &bare, 3), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_has_exif(&bare), 0);
    r |= assertNull(bare.meta);
    tiny_image_destroy(&bare);

    // re-encoding carries the payload through byte for byte
    TinyEncodeOpts keep = {85, 0, 0, 0, 0};
    TinyEncodeOpts drop = {85, 0, 0, 1, 0};
    TinyWriter kept;
    TinyWriter stripped;

    r |= assertEquals(tiny_writer_init(&kept, 0), TINYIMG_OK);
    r |= assertEquals(tiny_writer_init(&stripped, 0), TINYIMG_OK);

    r |= assertEquals(
        tiny_image_encode(&tagged, TINYIMG_FORMAT_JPEG, &keep, &kept),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_encode(&tagged, TINYIMG_FORMAT_JPEG, &drop, &stripped),
        TINYIMG_OK
    );

    // the difference is the segment and nothing else
    r |= assertEquals(
        (long) (kept.size - stripped.size), (long) (payload_size + 10)
    );

    TinyImage reloaded;
    r |= assertEquals(
        tiny_image_decode(&reloaded, kept.data, kept.size, 0), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_has_exif(&reloaded), 1);

    char* again = 0;
    size_t again_size = 0;
    r |= assertEquals(
        tiny_image_get_exif(&reloaded, &again, &again_size), TINYIMG_OK
    );
    r |= assertEquals((long) again_size, (long) payload_size);
    r |= assertBytesMatch(again, payload, payload_size);

    tiny_free(again);
    tiny_image_destroy(&reloaded);

    TinyImage without;
    r |= assertEquals(
        tiny_image_decode(&without, stripped.data, stripped.size, 0), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_has_exif(&without), 0);

    // and stripping the metadata leaves the pixels alone
    r |= assertEquals(
        tiny_image_decode(&image, kept.data, kept.size, 0), TINYIMG_OK
    );
    r |= assertImageEquals(&without, &image);

    tiny_image_destroy(&image);
    tiny_image_destroy(&without);
    tiny_free(payload);
    tiny_writer_free(&kept);
    tiny_writer_free(&stripped);
    tiny_image_destroy(&tagged);

    // #endregion

    // #region codec surface

    const TinyCodec* codec = tiny_codec_find(TINYIMG_FORMAT_JPEG);
    r |= assertNotNull(codec);
    r |= assertNotNull((const void*) (size_t) codec->sniff);
    r |= assertNotNull((const void*) (size_t) codec->probe);
    r |= assertNotNull((const void*) (size_t) codec->decode);

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base-444.jpg", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        r |= assertTrue(codec->sniff(bytes, size));
        r |= assertEquals(
            (long) (size_t) tiny_codec_sniff(bytes, size), (long) (size_t) codec
        );
        free(bytes);
    }

    // #endregion

    tiny_image_destroy(&source);
    tiny_image_destroy(&plain);

    // the arena is scratch, so nothing a decode allocated in it should still be
    // reserved once the decodes are done
    tiny_arena_reset();

    return r;
}
