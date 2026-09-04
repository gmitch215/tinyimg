#include "tinyimg/codec/webp.h"
#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

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

/** Encodes an image and reads it straight back, which is the whole round trip.
 */
static int roundTrip(
    const TinyImage* source, const TinyEncodeOpts* opts, TinyImage* back,
    size_t* bytes
) {
    TinyWriter out;
    int result = tiny_writer_init(&out, 0);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_encode(source, TINYIMG_FORMAT_WEBP, opts, &out);

    if (result == TINYIMG_OK) {
        *bytes = out.size;
        result = tiny_image_decode(back, out.data, out.size, 0);
    }

    tiny_writer_free(&out);
    return result;
}

/**
 * The distance code table, as the specification lists it.
 *
 * The library derives this rather than shipping it, from the rule that the
 * ordering is every offset with a row of 0 to 7 and a column of -7 to 8 that
 * names an already written pixel, sorted by squared distance, then by
 * descending row, then by descending column. These 120 bytes are the answer
 * that rule has to reproduce, and they are the reason the derivation is a rule
 * rather than a guess.
 */
static const unsigned char kPlaneCodes[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a,
    0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
    0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
    0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
    0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e,
    0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
    0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
    0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};

int main(void) {
    int r = 0;

    TinyImage image;
    TinyImageInfo info;

    // #region header reading

    r |= assertEquals(
        probeFixture("derived/base-lossy.webp", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_WEBP);

    // neither bitstream can be read out of order, so nothing here is
    // progressive in the sense the field means
    r |= assertEquals((long) info.progressive, 0L);

    // a lossless file declares its own alpha in its five byte header, with no
    // extended container to have declared it already
    r |= assertEquals(
        probeFixture("derived/base-alpha.webp", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.channels, 4L);
    r |= assertEquals((long) info.has_alpha, 1L);

    // a lossy frame keeps its alpha in a chunk of its own, and the flag that
    // says so is in the extended header rather than in the frame
    r |= assertEquals(
        probeFixture("derived/base-lossy-alpha.webp", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.channels, 4L);
    r |= assertEquals((long) info.has_alpha, 1L);

    /*
     * An animation reports its frame count, which is what makes one
     * identifiable without decoding it, and its canvas rather than its first
     * frame's extents. The second frame of this fixture is a row shorter than
     * the first, so a decoder reporting the frame would be wrong by a row.
     */
    r |= assertEquals(
        probeFixture("derived/base-animation.webp", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.frames, 2L);
    r |= assertEquals((long) info.has_alpha, 1L);

    r |= assertEquals(
        probeFixture("derived/tiny-odd-lossy.webp", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 65L);
    r |= assertEquals((long) info.height, 33L);

    // #endregion

    // #region derived tables

    /*
     * The plane code table, checked against the specification's own listing.
     * Deriving it saves 120 bytes of the module and is only safe if the
     * derivation is exactly right, so this is the assertion that makes the
     * saving legitimate rather than a gamble: one wrong entry names the wrong
     * pixel for one distance code, which a round trip through this codec alone
     * would never notice because both sides would be wrong together.
     */
    unsigned char derived[120];

    tiny_webp_plane_codes(derived);
    r |= assertBytesMatch(derived, kPlaneCodes, sizeof(kPlaneCodes));

    // #endregion

    // #region lossless decode

    r |= assertEquals(
        decodeFixture("derived/base-lossless.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);
    r |= assertEquals((long) image.height, 180L);
    r |= assertEquals((long) image.channels, 3L);
    r |= assertEquals((long) image.format, (long) TINYIMG_FORMAT_WEBP);

    /*
     * The same picture through the lossless coder and through PNG has to come
     * out identical, because both are lossless and both carry the same source.
     * This is the strongest check available without a committed reference: a
     * transform inverted slightly wrong shows here and nowhere else.
     */
    TinyImage viaPng;
    r |= assertEquals(
        decodeFixture("derived/base-rgb8.png", &viaPng, 0), TINYIMG_OK
    );
    r |= assertImageEquals(&image, &viaPng);

    tiny_image_destroy(&viaPng);
    tiny_image_destroy(&image);

    r |= assertEquals(
        decodeFixture("derived/base-alpha.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 4L);

    r |= assertEquals(
        decodeFixture("derived/base-rgba8.png", &viaPng, 0), TINYIMG_OK
    );
    r |= assertImageEquals(&image, &viaPng);

    tiny_image_destroy(&viaPng);
    tiny_image_destroy(&image);

    // odd extents, where the coded rows of a bundled image are narrower than
    // the picture and the last one is partly padding
    r |= assertEquals(
        decodeFixture("derived/tiny-odd-lossless.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 65L);
    r |= assertEquals((long) image.height, 33L);

    r |= assertEquals(
        decodeFixture("derived/tiny-odd.png", &viaPng, 0), TINYIMG_OK
    );
    r |= assertImageEquals(&image, &viaPng);

    tiny_image_destroy(&viaPng);
    tiny_image_destroy(&image);

    // #endregion

    // #region lossy decode

    r |= assertEquals(
        decodeFixture("derived/base-lossy.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);
    r |= assertEquals((long) image.height, 180L);
    r |= assertEquals((long) image.channels, 3L);

    /*
     * Against the source rather than against a reference decode, so the floor
     * is what the format lost and not what this decoder did. 30 dB is the
     * quality `cwebp -q 80` delivers; a decoder with a wrong predictor or a
     * wrong filter lands far below it, since both are per block and their
     * errors accumulate down the picture.
     */
    TinyImage source;
    r |= assertEquals(
        decodeFixture("derived/base-rgb8.png", &source, 0), TINYIMG_OK
    );

    if (source.data && image.data && source.width == image.width) {
        r |= assertPSNR(
            image.data, source.data, (size_t) image.width * image.height * 3u,
            30.0
        );
    }

    tiny_image_destroy(&image);

    // both loop filters, which are different algorithms and not two strengths
    // of one
    for (uint32_t i = 0; i < 2; i++) {
        const char* name =
            i == 0 ? "derived/base-strong.webp" : "derived/base-simple.webp";

        r |= assertEquals(decodeFixture(name, &image, 0), TINYIMG_OK);

        if (source.data && image.data) {
            r |= assertPSNR(
                image.data, source.data,
                (size_t) image.width * image.height * 3u, 29.0
            );
        }

        tiny_image_destroy(&image);
    }

    // a non-zero sharpness reduces the filter's interior limit, and the
    // encoder's default never writes one
    r |= assertEquals(
        decodeFixture("derived/base-sharp.webp", &image, 0), TINYIMG_OK
    );
    if (source.data && image.data) {
        r |= assertPSNR(
            image.data, source.data, (size_t) image.width * image.height * 3u,
            29.0
        );
    }
    tiny_image_destroy(&image);

    // segmentation switched off, which is a separate path: one quantizer for
    // the frame and no segment map to read per macroblock
    r |= assertEquals(
        decodeFixture("derived/base-onesegment.webp", &image, 0), TINYIMG_OK
    );
    if (source.data && image.data) {
        r |= assertPSNR(
            image.data, source.data, (size_t) image.width * image.height * 3u,
            29.0
        );
    }
    tiny_image_destroy(&image);

    tiny_image_destroy(&source);

    // odd extents, where the chroma upsampler has a column with no pair and a
    // row with no pair
    r |= assertEquals(
        decodeFixture("derived/tiny-odd-lossy.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 65L);
    r |= assertEquals((long) image.height, 33L);
    tiny_image_destroy(&image);

    // #endregion

    // #region alpha planes

    /*
     * The two ways an alpha plane can be stored. One goes through the lossless
     * coder and reads out of its green channel; the other is the bytes
     * outright. Both then have one of three filters undone, and a file gets to
     * pick, so the two fixtures reach different code.
     */
    for (uint32_t i = 0; i < 2; i++) {
        const char* name = i == 0 ? "derived/base-lossy-alpha.webp"
                                  : "derived/base-raw-alpha.webp";

        r |= assertEquals(decodeFixture(name, &image, 0), TINYIMG_OK);
        r |= assertEquals((long) image.channels, 4L);

        // the fixture's alpha ramps down the rows, opaque at the top and clear
        // at the bottom, which is what a plane read upside down or left
        // unfiltered gets wrong
        if (image.data) {
            uint8_t top = image.data[3];
            uint8_t bottom =
                image
                    .data[((size_t) image.height - 1u) * image.width * 4u + 3u];

            r |= assertGreaterThan((double) top, 200.0);
            r |= assertLessThan((double) bottom, 60.0);
        }

        tiny_image_destroy(&image);
    }

    /*
     * A plane the encoder pre-filtered before compressing it. Its default
     * writes no filter at all, so without a fixture that asked for one the
     * decoder's unfiltering never runs.
     */
    r |= assertEquals(
        decodeFixture("derived/base-filtered-alpha.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 4L);

    if (image.data) {
        r |= assertGreaterThan((double) image.data[3], 200.0);
        r |= assertLessThan(
            (
                double
            ) image.data[((size_t) image.height - 1u) * image.width * 4u + 3u],
            60.0
        );
    }

    tiny_image_destroy(&image);

    /*
     * The other two filters, which no encoder to hand writes: the search in
     * `cwebp -alpha_filter best` only ever settles on the horizontal one. So
     * rather than assert that a hand-built file merely decodes, the forward
     * filter is computed here from the specification and the decoder has to
     * invert it back to the plane it started from, which is an answer known
     * independently of the code being tested.
     */
    for (uint32_t method = 2; method <= 3; method++) {
        size_t size = 0;
        unsigned char* file = readFixture("derived/base-raw-alpha.webp", &size);

        r |= assertNotNull(file);
        if (!file) continue;

        // the chunk walk, far enough to find where the plane lives
        size_t at = 12;
        size_t alpha = 0;

        while (at + 8 <= size) {
            unsigned int length = (unsigned int) file[at + 4] |
                                  ((unsigned int) file[at + 5] << 8) |
                                  ((unsigned int) file[at + 6] << 16) |
                                  ((unsigned int) file[at + 7] << 24);

            if (memcmp(file + at, "ALPH", 4) == 0) {
                alpha = at + 8;
                break;
            }

            at += 8 + length + (length & 1u);
        }

        r |= assertNotEquals((long) alpha, 0L);

        if (alpha) {
            // stored raw, so the payload past its header byte is the plane
            r |= assertEquals((long) (file[alpha] & 3u), 0L);

            unsigned char* plane = file + alpha + 1;
            unsigned int width = 320;
            unsigned int height = 180;

            unsigned char* original = malloc((size_t) width * height);
            r |= assertNotNull(original);

            if (original) {
                memcpy(original, plane, (size_t) width * height);

                // backward, so each row is differenced against the source of
                // the one above rather than against its residual
                for (unsigned int y = height; y-- > 1;) {
                    unsigned char* row = plane + (size_t) y * width;
                    const unsigned char* above =
                        original + (size_t) (y - 1u) * width;
                    const unsigned char* here = original + (size_t) y * width;

                    int left = above[0];
                    int corner = left;

                    for (unsigned int x = 0; x < width; x++) {
                        int top = above[x];
                        int guess = method == 2
                                        ? top
                                        : (left + top - corner < 0
                                               ? 0
                                               : (left + top - corner > 255
                                                      ? 255
                                                      : left + top - corner));

                        row[x] = (unsigned char) (here[x] - guess);
                        corner = top;
                        left = here[x];
                    }
                }

                // the first row has nothing above it, so every filter falls
                // back to the horizontal one there
                int left = 0;

                for (unsigned int x = 0; x < width; x++) {
                    unsigned char value = original[x];

                    plane[x] = (unsigned char) (value - left);
                    left = value;
                }

                file[alpha] =
                    (unsigned char) ((file[alpha] & ~0x0Cu) | (method << 2));

                r |= assertEquals(
                    tiny_image_decode(&image, file, size, 0), TINYIMG_OK
                );

                if (image.data && image.channels == 4) {
                    int recovered = 1;

                    for (size_t i = 0; i < (size_t) width * height; i++) {
                        if (image.data[i * 4u + 3u] != original[i]) {
                            recovered = 0;
                            break;
                        }
                    }

                    r |= assertTrue(recovered);
                }

                tiny_image_destroy(&image);
                free(original);
            }
        }

        free(file);
    }

    // #endregion

    // #region animation

    /*
     * The first frame, composited onto the canvas the file declares, which is
     * the same rule GIF follows. Composing or re-timing the rest is out of
     * scope for both.
     */
    r |= assertEquals(
        decodeFixture("derived/base-animation.webp", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.width, 320L);
    r |= assertEquals((long) image.height, 180L);
    tiny_image_destroy(&image);

    // #endregion

    // #region region and scale

    /*
     * Neither bitstream can decode a part of itself, so the region is taken
     * after the frame is decoded. That still has to produce the same pixels as
     * cropping a full decode, which is what the contract promises a caller.
     */
    r |= assertEquals(
        decodeFixture("derived/base-lossless.webp", &image, 4), TINYIMG_OK
    );

    TinyDecodeOpts region = {41, 17, 100, 50, 1, 4};
    TinyImage part;

    r |= assertEquals(
        decodeWith("derived/base-lossless.webp", &part, &region), TINYIMG_OK
    );
    r |= assertEquals((long) part.width, 100L);
    r |= assertEquals((long) part.height, 50L);

    int matches = 1;

    if (image.data && part.data) {
        for (uint32_t y = 0; y < part.height; y++) {
            const uint8_t* a = part.data + (size_t) y * part.width * 4u;
            const uint8_t* b =
                image.data + ((size_t) (y + 17u) * image.width + 41u) * 4u;

            for (uint32_t x = 0; x < part.width * 4u; x++) {
                if (a[x] != b[x]) matches = 0;
            }
        }
    }

    r |= assertTrue(matches);
    tiny_image_destroy(&part);

    // a scaled decode box averages, the same as every other codec's, so a
    // scaled WebP and a scaled PNG of one picture agree
    TinyDecodeOpts scaled = {0, 0, 0, 0, 4, 3};

    r |= assertEquals(
        decodeWith("derived/base-lossless.webp", &part, &scaled), TINYIMG_OK
    );
    r |= assertEquals((long) part.width, 80L);
    r |= assertEquals((long) part.height, 45L);

    TinyImage pngScaled;
    r |= assertEquals(
        decodeWith("derived/base-rgb8.png", &pngScaled, &scaled), TINYIMG_OK
    );
    r |= assertImageEquals(&part, &pngScaled);

    tiny_image_destroy(&pngScaled);
    tiny_image_destroy(&part);
    tiny_image_destroy(&image);

    // #endregion

    // #region lossless encode

    r |= assertEquals(
        decodeFixture("derived/base-rgb8.png", &image, 0), TINYIMG_OK
    );

    TinyEncodeOpts lossless = {0, 1, 0, 0};
    TinyImage back;
    size_t bytes = 0;

    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);

    // lossless means lossless, and this is the assertion that says so
    r |= assertImageEquals(&image, &back);
    r |= assertLessThan((double) bytes, (double) (320 * 180 * 3));

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // with alpha, which the palette path spends an entry on and the transform
    // path carries through all four channels
    r |= assertEquals(
        decodeFixture("derived/base-rgba8.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);
    r |= assertImageEquals(&image, &back);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    /*
     * An image inside 256 colours keeps them exactly, through the palette
     * transform rather than through the predictors. A logo is the case that
     * matters: it is what the format is best at and what a photograph's
     * pipeline would handle badly.
     */
    r |= assertEquals(decodeFixture("derived/logo.png", &image, 0), TINYIMG_OK);
    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);
    r |= assertImageEquals(&image, &back);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // one colour, which is the smallest palette there is and bundles eight
    // indices into every byte
    r |= assertEquals(decodeFixture("derived/flat.png", &image, 0), TINYIMG_OK);
    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);
    r |= assertImageEquals(&image, &back);
    r |= assertLessThan((double) bytes, 100.0);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // one pixel, where there is no neighbour to predict from and no pair to
    // hash
    r |= assertEquals(
        decodeFixture("derived/single-pixel.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);
    r |= assertImageEquals(&image, &back);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // odd extents, so the bundled rows are padded and the padding must not
    // reach the picture
    r |= assertEquals(
        decodeFixture("derived/tiny-odd.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&image, &lossless, &back, &bytes), TINYIMG_OK);
    r |= assertImageEquals(&image, &back);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    /*
     * Greyscale. The format has no greyscale mode, so one channel goes in as
     * three equal ones and comes back as three: the pixels survive and the
     * channel count does not, which is what a caller has to expect. Asking for
     * one channel back returns exactly what went in.
     */
    r |= assertEquals(
        decodeFixture("derived/base-gray8.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 1L);

    TinyWriter grey;
    r |= assertEquals(tiny_writer_init(&grey, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&image, TINYIMG_FORMAT_WEBP, &lossless, &grey),
        TINYIMG_OK
    );

    TinyDecodeOpts asGrey = {0, 0, 0, 0, 1, 1};
    r |= assertEquals(
        tiny_image_decode(&back, grey.data, grey.size, &asGrey), TINYIMG_OK
    );
    r |= assertImageEquals(&image, &back);

    tiny_writer_free(&grey);
    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // #endregion

    // #region lossy encode

    r |= assertEquals(
        decodeFixture("derived/base-rgb8.png", &image, 0), TINYIMG_OK
    );

    TinyEncodeOpts lossy = {80, 0, 0, 0};
    size_t at80 = 0;

    r |= assertEquals(roundTrip(&image, &lossy, &back, &at80), TINYIMG_OK);
    r |= assertEquals((long) back.width, 320L);
    r |= assertEquals((long) back.height, 180L);

    /*
     * Encode and decode are checked together and against the source, which is
     * the only way a lossy pair can be: a wrong forward transform and a wrong
     * inverse cancel each other in a round trip and show up here.
     */
    if (image.data && back.data) {
        r |= assertPSNR(back.data, image.data, (size_t) 320 * 180 * 3, 32.0);
    }

    tiny_image_destroy(&back);

    // quality has to mean something monotone, or a caller cannot use the
    // number: more of it costs more bytes and loses less
    TinyEncodeOpts lower = {40, 0, 0, 0};
    size_t at40 = 0;

    r |= assertEquals(roundTrip(&image, &lower, &back, &at40), TINYIMG_OK);
    r |= assertLessThan((double) at40, (double) at80);

    double coarse = computePSNR(back.data, image.data, (size_t) 320 * 180 * 3);

    tiny_image_destroy(&back);

    TinyEncodeOpts higher = {95, 0, 0, 0};
    size_t at95 = 0;

    r |= assertEquals(roundTrip(&image, &higher, &back, &at95), TINYIMG_OK);
    r |= assertGreaterThan((double) at95, (double) at80);
    r |= assertGreaterThan(
        computePSNR(back.data, image.data, (size_t) 320 * 180 * 3), coarse
    );

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // an odd sized picture, where the last macroblock of a row and of a column
    // is mostly padding the encoder invented
    r |= assertEquals(
        decodeFixture("derived/tiny-odd.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&image, &lossy, &back, &bytes), TINYIMG_OK);
    r |= assertEquals((long) back.width, 65L);
    r |= assertEquals((long) back.height, 33L);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // a single pixel, which is one macroblock of which one pixel is real
    r |= assertEquals(
        decodeFixture("derived/single-pixel.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals(roundTrip(&image, &lossy, &back, &bytes), TINYIMG_OK);
    r |= assertEquals((long) back.width, 1L);
    r |= assertEquals((long) back.height, 1L);

    tiny_image_destroy(&back);
    tiny_image_destroy(&image);

    // #endregion

    // #region malformed

    static const unsigned char truncated[16] = {'R', 'I', 'F', 'F', 0x08, 0,
                                                0,   0,   'W', 'E', 'B',  'P',
                                                'V', 'P', '8', 'L'};

    r |= assertEquals(
        tiny_image_decode(&image, truncated, sizeof(truncated), 0),
        TINYIMG_ERR_CORRUPT
    );

    // a container with no bitstream chunk at all
    static const unsigned char empty[12] = {'R', 'I', 'F', 'F', 0x04, 0,
                                            0,   0,   'W', 'E', 'B',  'P'};

    r |= assertEquals(
        tiny_image_decode(&image, empty, sizeof(empty), 0), TINYIMG_ERR_CORRUPT
    );

    // a lossless signature byte that is not the one the format defines
    unsigned char* bytes8 = 0;
    size_t size8 = 0;

    bytes8 = readFixture("derived/base-lossless.webp", &size8);
    r |= assertNotNull(bytes8);

    if (bytes8 && size8 > 24) {
        unsigned char keep = bytes8[20];

        bytes8[20] = 0x00;
        r |= assertEquals(
            tiny_image_decode(&image, bytes8, size8, 0), TINYIMG_ERR_CORRUPT
        );
        bytes8[20] = keep;

        // truncating the payload has to be caught rather than read past
        r |= assertEquals(
            tiny_image_decode(&image, bytes8, 40, 0), TINYIMG_ERR_CORRUPT
        );

        free(bytes8);
    }

    // a lossy frame whose start code is wrong, which is what separates a
    // keyframe from a stream this codec cannot use
    bytes8 = readFixture("derived/base-lossy.webp", &size8);
    r |= assertNotNull(bytes8);

    if (bytes8 && size8 > 32) {
        unsigned char keep = bytes8[23];

        bytes8[23] = 0x00;
        r |= assertEquals(
            tiny_image_decode(&image, bytes8, size8, 0), TINYIMG_ERR_CORRUPT
        );
        bytes8[23] = keep;

        // the low bit of the frame tag clear means a keyframe; set means an
        // interframe, which needs a reference frame a still image never has
        keep = bytes8[20];
        bytes8[20] |= 1u;
        r |= assertEquals(
            tiny_image_decode(&image, bytes8, size8, 0),
            TINYIMG_ERR_UNSUPPORTED_VARIANT
        );
        bytes8[20] = keep;

        free(bytes8);
    }

    r |= assertEquals(
        tiny_image_encode(0, TINYIMG_FORMAT_WEBP, 0, 0), TINYIMG_ERR_NULL
    );

    // #endregion

    tiny_arena_reset();
    return r;
}
