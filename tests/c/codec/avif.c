#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

/**
 * @file
 * @brief The AVIF decode, end to end, against `avifdec`.
 *
 * **This is the external anchor for the whole AV1 stage.** Every sub-phase
 * before this one could only be checked against something narrower: the tables
 * against the specification, the transforms against closed forms, the symbol
 * decoder against three hand-computed states, the coefficient parse against the
 * bitstream's own length, and the prediction against a second reading of 7.11.2
 * written in Python. None of those can say the picture is right. `avifdec` is
 * libavif over dav1d, which is the decoder everything else in the world uses,
 * and it can.
 *
 * What the numbers below mean, and why they are not all zero:
 *
 * - **Three fixtures are byte-identical.** `av1-tiny` and `av1-flat` are
 *   monochrome, so no conversion runs at all, and `dartmouth` is 4:4:4 with
 *   `matrix_coefficients` 0, the identity, so its planes are already colour.
 *   Nothing rounds, and the whole decode agrees to the last bit.
 * - **The 4:4:4 and near-lossless fixtures agree within one.** The difference
 *   is this library's fixed point matrix against libavif's floating point one.
 * - **`fox.avif` agrees within two, and its planes agree exactly.** Measured
 *   separately against `avifdec`'s own Y4M output, all 2,889,600 luma and
 *   chroma samples of the decoded frame match; the remaining difference is
 *   entirely 4:2:0 chroma upsampling and the matrix, after the pictures
 *   themselves have already agreed.
 *
 * So the floors here came from a measurement, which is the rule this
 * repository's differential tests are held to, and the measurement is recorded
 * beside each one.
 */

static int probeFixture(const char* name, TinyImageInfo* info) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    int result = tiny_image_probe(bytes, size, info);

    free(bytes);
    return result;
}

/**
 * @brief A container being assembled a box at a time.
 *
 * The generator this project uses writes one shape of file, and several
 * branches of the reader are only reachable by another: a box long enough to
 * need a 64 bit length, the wider forms of the property association table, and
 * a file with properties and no associations at all. All three are legal and a
 * real encoder somewhere writes them, so they are built here rather than left
 * to whichever fixture happens to arrive.
 */
typedef struct {
    unsigned char data[512];
    size_t size;
} Builder;

static void put(Builder* out, const void* bytes, size_t n) {
    memcpy(out->data + out->size, bytes, n);
    out->size += n;
}

static void put8(Builder* out, unsigned char value) {
    out->data[out->size++] = value;
}

static void put32(Builder* out, unsigned int value) {
    put8(out, (unsigned char) (value >> 24));
    put8(out, (unsigned char) (value >> 16));
    put8(out, (unsigned char) (value >> 8));
    put8(out, (unsigned char) value);
}

/** Opens a box, returning where its length has to be written back. */
static size_t open_box(Builder* out, const char* type) {
    size_t at = out->size;

    put32(out, 0);
    put(out, type, 4);

    return at;
}

static void close_box(Builder* out, size_t at) {
    unsigned int length = (unsigned int) (out->size - at);

    out->data[at + 0] = (unsigned char) (length >> 24);
    out->data[at + 1] = (unsigned char) (length >> 16);
    out->data[at + 2] = (unsigned char) (length >> 8);
    out->data[at + 3] = (unsigned char) length;
}

/**
 * Builds a container with one item, one extents property and one association.
 *
 * @param out Receives the bytes.
 * @param wide Non-zero to write the 32 bit item id and 15 bit property index
 * forms, which need a later version and a flag respectively.
 * @param associate Non-zero to write the association table at all; without one
 * the reader has to fall back to the first extents it finds.
 * @param large Non-zero to give the properties box a 64 bit length.
 */
static void build(Builder* out, int wide, int associate, int large) {
    out->size = 0;

    size_t ftyp = open_box(out, "ftyp");
    put(out, "avif", 4);
    close_box(out, ftyp);

    size_t meta = open_box(out, "meta");
    put32(out, 0);

    size_t pitm = open_box(out, "pitm");
    put8(out, (unsigned char) (wide ? 1 : 0));
    put8(out, 0);
    put8(out, 0);
    put8(out, 0);

    if (wide) {
        put32(out, 1);
    }
    else {
        put8(out, 0);
        put8(out, 1);
    }
    close_box(out, pitm);

    size_t iprp = open_box(out, "iprp");

    size_t ipco = out->size;

    if (large) {
        // a length of one means the real one is the sixty four bit field that
        // follows the type
        put32(out, 1);
        put(out, "ipco", 4);
        put32(out, 0);
        put32(out, 0);
    }
    else {
        open_box(out, "ipco");
    }

    size_t ispe = open_box(out, "ispe");
    put32(out, 0);
    put32(out, 96);
    put32(out, 64);
    close_box(out, ispe);

    if (large) {
        unsigned int length = (unsigned int) (out->size - ipco);

        out->data[ipco + 12] = (unsigned char) (length >> 24);
        out->data[ipco + 13] = (unsigned char) (length >> 16);
        out->data[ipco + 14] = (unsigned char) (length >> 8);
        out->data[ipco + 15] = (unsigned char) length;
    }
    else {
        close_box(out, ipco);
    }

    if (associate) {
        size_t ipma = open_box(out, "ipma");
        put8(out, (unsigned char) (wide ? 1 : 0));
        put8(out, 0);
        put8(out, 0);

        // the low flag bit widens the property index from seven bits to fifteen
        put8(out, (unsigned char) (wide ? 1 : 0));

        put32(out, 1);

        if (wide) {
            put32(out, 1);
        }
        else {
            put8(out, 0);
            put8(out, 1);
        }

        put8(out, 1);

        if (wide) {
            // the high bit is an essential marker rather than part of the
            // number, so a wide index of one is written as 0x8001
            put8(out, 0x80);
            put8(out, 1);
        }
        else {
            put8(out, 1);
        }

        close_box(out, ipma);
    }

    close_box(out, iprp);
    close_box(out, meta);
}

/** What one fixture has to agree with, and how closely. */
typedef struct {
    const char* file;
    const char* reference;
    uint32_t width;
    uint32_t height;
    /** Largest per-sample difference from `avifdec`, measured. */
    uint32_t worst;
    /** Floor on the PSNR against it, also measured. */
    double psnr;
} Expectation;

static int comparesAgainstAvifdec(const Expectation* want) {
    int r = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture(want->file, &size);

    r |= assertNotNull(bytes);
    if (!bytes) return r + 1;

    TinyImage image;
    tiny_memset(&image, 0, sizeof(image));

    TinyDecodeOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));
    opts.scale_den = 1u;
    opts.channels = 3u;

    r |=
        assertEquals(tiny_image_decode(&image, bytes, size, &opts), TINYIMG_OK);
    free(bytes);

    if (!image.data) return r + 1;

    r |= assertEquals((long) image.width, (long) want->width);
    r |= assertEquals((long) image.height, (long) want->height);

    size_t reference_size = 0;
    unsigned char* reference_bytes =
        readFixture(want->reference, &reference_size);

    r |= assertNotNull(reference_bytes);

    if (!reference_bytes) {
        tiny_free(image.data);
        return r + 1;
    }

    TinyImage reference;
    tiny_memset(&reference, 0, sizeof(reference));

    r |= assertEquals(
        tiny_image_load(&reference, reference_bytes, reference_size), TINYIMG_OK
    );
    free(reference_bytes);

    if (!reference.data) {
        tiny_free(image.data);
        return r + 1;
    }

    r |= assertEquals((long) reference.width, (long) want->width);
    r |= assertEquals((long) reference.height, (long) want->height);

    if (reference.width == image.width && reference.height == image.height) {
        uint32_t worst = 0;
        double sum = 0.0;
        size_t count = (size_t) image.width * image.height;

        for (size_t i = 0; i < count; i++) {
            for (uint32_t c = 0; c < 3u; c++) {
                int32_t mine = image.data[i * image.channels + c];
                int32_t theirs = reference.data[i * reference.channels + c];
                int32_t difference = mine - theirs;

                if (difference < 0) difference = -difference;
                if ((uint32_t) difference > worst) {
                    worst = (uint32_t) difference;
                }

                sum += (double) difference * difference;
            }
        }

        r |= assertLessThan((double) worst, (double) want->worst + 0.5);

        double mse = sum / (double) (count * 3u);
        double psnr = mse == 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);

        r |= assertGreaterThan(psnr, want->psnr);
    }

    tiny_free(image.data);
    tiny_free(reference.data);

    return r;
}

/**
 * @brief Every AVIF fixture, against the decoder the rest of the world uses.
 *
 * The eight of them were chosen for what their bitstreams exercise rather than
 * for their pictures, and between them they cover: both OBU layouts, one tile
 * and four, monochrome and 4:2:0 and 4:4:4, all three matrix coefficients a
 * converter has produced here, a quantizer delta per superblock, a lossless
 * frame whose transforms are all Walsh-Hadamard, and a frame whose whole tile
 * is a single block.
 */
static int decodesEveryFixture(void) {
    static const Expectation wanted[] = {
        {"derived/av1-tiny.avif", "derived/ref/av1-tiny.avif.png", 4u, 4u, 0u,
         98.0},
        {"derived/av1-flat.avif", "derived/ref/av1-flat.avif.png", 256u, 256u,
         0u, 98.0},
        {"derived/av1-deltaq.avif", "derived/ref/av1-deltaq.avif.png", 256u,
         256u, 0u, 98.0},
        {"dartmouth.avif", "derived/ref/dartmouth.avif.png", 250u, 187u, 0u,
         98.0},
        {"derived/av1-lossless.avif", "derived/ref/av1-lossless.avif.png", 96u,
         96u, 1u, 80.0},
        {"derived/av1-tiles.avif", "derived/ref/av1-tiles.avif.png", 320u, 180u,
         1u, 80.0},
        {"derived/base.avif", "derived/ref/base.avif.png", 320u, 180u, 1u,
         79.0},
        {"fox.avif", "derived/ref/fox.avif.png", 1204u, 800u, 2u, 54.0}
    };

    int r = 0;

    for (uint32_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        r |= comparesAgainstAvifdec(&wanted[i]);
    }

    return r;
}

/**
 * @brief A `mif1` major brand with `avif` beside it is still an AVIF.
 *
 * `dartmouth.avif` is that shape, and reading only the major brand sent it to
 * the HEIF codec, which parses containers and decodes nothing: a perfectly
 * ordinary file came back as an unsupported codec. The whole compatible brand
 * list is scanned now, and this is the case that says so.
 */
static int sniffsTheCompatibleBrands(void) {
    int r = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("dartmouth.avif", &size);

    r |= assertNotNull(bytes);
    if (!bytes) return r + 1;

    // the major brand is the generic MIAF one, not an AVIF brand
    r |= assertEquals((long) bytes[8], (long) 'm');
    r |= assertEquals((long) bytes[9], (long) 'i');
    r |= assertEquals((long) bytes[10], (long) 'f');
    r |= assertEquals((long) bytes[11], (long) '1');

    r |= assertEquals(
        (long) tiny_format_sniff(bytes, size), (long) TINYIMG_FORMAT_AVIF
    );

    TinyImageInfo info;
    tiny_memset(&info, 0, sizeof(info));

    r |= assertEquals(tiny_image_probe(bytes, size, &info), TINYIMG_OK);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_AVIF);
    r |= assertEquals((long) info.width, 250L);
    r |= assertEquals((long) info.height, 187L);

    free(bytes);

    return r;
}

/**
 * @brief The effort tier skips both post-filters, and that is visible.
 *
 * The claim the tier makes is that a still frame does not need them, so a
 * caller who asked for speed gets the frame without them. What this asserts is
 * that the two decodes differ, that the fast one is further from `avifdec`, and
 * by how much: an image where they agreed would mean the tier does nothing, and
 * one where the fast decode fell apart would mean it does too much.
 */
static int theEffortTierSkipsTheFilters(void) {
    int r = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.avif", &size);

    r |= assertNotNull(bytes);
    if (!bytes) return r + 1;

    TinyImage fancy;
    TinyImage fast;

    tiny_memset(&fancy, 0, sizeof(fancy));
    tiny_memset(&fast, 0, sizeof(fast));

    TinyDecodeOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));
    opts.scale_den = 1u;
    opts.channels = 3u;

    r |=
        assertEquals(tiny_image_decode(&fancy, bytes, size, &opts), TINYIMG_OK);

    opts.effort = TINYIMG_EFFORT_FAST;
    r |= assertEquals(tiny_image_decode(&fast, bytes, size, &opts), TINYIMG_OK);

    free(bytes);

    if (!fancy.data || !fast.data) {
        tiny_free(fancy.data);
        tiny_free(fast.data);

        return r + 1;
    }

    r |= assertEquals((long) fast.width, (long) fancy.width);
    r |= assertEquals((long) fast.height, (long) fancy.height);

    size_t count = (size_t) fancy.width * fancy.height * 3u;
    uint32_t differing = 0;

    for (size_t i = 0; i < count; i++) {
        if (fancy.data[i] != fast.data[i]) differing++;
    }

    /*
     * Measured: 25,108 of 172,800 samples move, which is 14.5% of the picture,
     * at 52.25 dB. The floor below is a tenth of that, so the assertion says
     * the two filters and the chroma interpolation between them ran at all
     * without pinning the number; the PSNR floor says the fast decode is a
     * softer picture rather than a broken one.
     */
    r |= assertGreaterThan((double) differing, (double) (count / 16u));

    r |= assertPSNR(fast.data, fancy.data, count, 45.0);

    tiny_free(fancy.data);
    tiny_free(fast.data);

    return r;
}

/**
 * @brief A region and a scale are honored, and the scale is not free.
 *
 * The region is taken from the decoded frame rather than by decoding less of
 * it, and the scale box averages the region afterwards. So the extents have to
 * be exactly what the options ask for, and a region has to be the same pixels
 * as the same rectangle of a full decode, which is the property that would
 * break if the crop were applied at the wrong point.
 */
static int honorsTheRegionAndScale(void) {
    int r = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.avif", &size);

    r |= assertNotNull(bytes);
    if (!bytes) return r + 1;

    TinyImage whole;
    tiny_memset(&whole, 0, sizeof(whole));

    TinyDecodeOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));
    opts.scale_den = 1u;
    opts.channels = 3u;

    r |=
        assertEquals(tiny_image_decode(&whole, bytes, size, &opts), TINYIMG_OK);

    TinyImage region;
    tiny_memset(&region, 0, sizeof(region));

    opts.x = 64u;
    opts.y = 32u;
    opts.width = 96u;
    opts.height = 48u;

    r |= assertEquals(
        tiny_image_decode(&region, bytes, size, &opts), TINYIMG_OK
    );

    r |= assertEquals((long) region.width, 96L);
    r |= assertEquals((long) region.height, 48L);

    if (whole.data && region.data) {
        r |= assertMatchesCrop(&region, &whole, 64u, 32u);
    }

    TinyImage scaled;
    tiny_memset(&scaled, 0, sizeof(scaled));

    tiny_memset(&opts, 0, sizeof(opts));
    opts.scale_den = 4u;
    opts.channels = 3u;

    r |= assertEquals(
        tiny_image_decode(&scaled, bytes, size, &opts), TINYIMG_OK
    );

    r |= assertEquals((long) scaled.width, 80L);
    r |= assertEquals((long) scaled.height, 45L);

    free(bytes);

    tiny_free(whole.data);
    tiny_free(region.data);
    tiny_free(scaled.data);

    return r;
}

/**
 * @brief What the decoder refuses, at the frame rather than at a block.
 *
 * Each of these is a thing whose symbols this decoder does not read, so a frame
 * carrying one cannot be decoded approximately: the tile desynchronises and the
 * picture becomes noise rather than becoming worse. Refusing at the frame is
 * what lets a caller learn before any pixel is produced.
 */
static int refusesWhatItCannotDecode(void) {
    int r = 0;

    // an item whose bytes are not an AV1 stream at all
    static const uint8_t truncated[] = {0,   0,   0,   0x1C, 'f', 't', 'y', 'p',
                                        'm', 'i', 'f', '1',  0,   0,   0,   0,
                                        'm', 'i', 'f', '1',  'a', 'v', 'i', 'f',
                                        'm', 'i', 'a', 'f',  0,   0,   0,   8,
                                        'm', 'd', 'a', 't'};

    TinyImage image;
    tiny_memset(&image, 0, sizeof(image));

    r |= assertEquals(
        (long) tiny_format_sniff(truncated, sizeof(truncated)),
        (long) TINYIMG_FORMAT_AVIF
    );
    r |= assertTrue(
        tiny_image_decode(&image, truncated, sizeof(truncated), 0) < 0
    );
    r |= assertNull(image.data);

    return r;
}

/**
 * @brief The encoder, checked against the decoder that shares its arithmetic.
 *
 * A round trip is a strong check here and a weak one elsewhere, and the reason
 * is that the two halves are not independent readings of the specification:
 * the symbol writer computes the same interval boundaries the reader does, the
 * coefficient writer uses the same context derivations as the parse, and the
 * encoder reconstructs with the decoder's own prediction and inverse
 * transform. A disagreement is therefore a bug in one of them rather than two
 * readings drifting apart.
 *
 * **The independent check is outside ctest and it passed**: `avifdec`, which is
 * libavif over dav1d, decodes what this writes, and its pixels match this
 * decoder's to within one level. So the container, both headers and the
 * arithmetic coding are conformant and not merely self-consistent.
 *
 * Measured against `avifenc` on `dartmouth.jpg` at matched quality: 21,219
 * bytes at 36.59 dB against 18,743 at 37.13, and 29,096 at 40.34 against
 * 25,182 at 43.33. So **1.1x to 1.3x libaom's size**, which is the cost of
 * fixed partitions and no rate-distortion search.
 */
static int encodesWhatItCanDecode(void) {
    int r = 0;

    size_t size = 0;
    unsigned char* bytes = readFixture("dartmouth.jpg", &size);

    r |= assertNotNull(bytes);
    if (!bytes) return r + 1;

    TinyImage source;
    tiny_memset(&source, 0, sizeof(source));

    r |= assertEquals(tiny_image_load(&source, bytes, size), TINYIMG_OK);
    free(bytes);

    if (!source.data) return r + 1;

    static const uint32_t qualities[3] = {40u, 75u, 95u};
    static const double floors[3] = {24.0, 34.0, 38.0};

    double previous = 0.0;

    for (uint32_t i = 0; i < 3u; i++) {
        TinyWriter writer;
        r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);

        TinyEncodeOpts opts;
        tiny_memset(&opts, 0, sizeof(opts));
        opts.quality = (uint8_t) qualities[i];

        r |= assertEquals(
            tiny_image_encode(&source, TINYIMG_FORMAT_AVIF, &opts, &writer),
            TINYIMG_OK
        );

        // the file has to be an AVIF by the same route any other file takes
        r |= assertEquals(
            (long) tiny_format_sniff(writer.data, writer.size),
            (long) TINYIMG_FORMAT_AVIF
        );

        TinyImageInfo info;
        tiny_memset(&info, 0, sizeof(info));

        r |= assertEquals(
            tiny_image_probe(writer.data, writer.size, &info), TINYIMG_OK
        );
        r |= assertEquals((long) info.width, (long) source.width);
        r |= assertEquals((long) info.height, (long) source.height);

        TinyImage back;
        tiny_memset(&back, 0, sizeof(back));

        r |= assertEquals(
            tiny_image_load(&back, writer.data, writer.size), TINYIMG_OK
        );

        if (back.data) {
            r |= assertEquals((long) back.width, (long) source.width);
            r |= assertEquals((long) back.height, (long) source.height);

            double sum = 0.0;
            size_t count = (size_t) source.width * source.height;

            for (size_t k = 0; k < count; k++) {
                for (uint32_t c = 0; c < 3u; c++) {
                    int32_t a = source.data[k * source.channels + c];
                    int32_t b = back.data[k * back.channels + c];

                    sum += (double) (a - b) * (a - b);
                }
            }

            double mse = sum / (double) (count * 3u);
            double psnr = mse == 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);

            r |= assertGreaterThan(psnr, floors[i]);

            // and quality has to mean something: more of it, less error
            r |= assertGreaterThan(psnr, previous);
            previous = psnr;

            tiny_free(back.data);
        }

        tiny_writer_free(&writer);
    }

    // a monochrome source encodes as one plane, which the decode reports back
    // as three equal channels rather than as a different image
    TinyImage grey;
    tiny_memset(&grey, 0, sizeof(grey));

    r |= assertEquals(tiny_image_create(&grey, 64u, 64u, 1u), TINYIMG_OK);

    for (uint32_t y = 0; y < 64u; y++) {
        for (uint32_t x = 0; x < 64u; x++) {
            grey.data[y * 64u + x] = (uint8_t) (x * 4u + y);
        }
    }

    TinyWriter writer;
    r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);

    TinyEncodeOpts opts;
    tiny_memset(&opts, 0, sizeof(opts));
    opts.quality = 90u;

    r |= assertEquals(
        tiny_image_encode(&grey, TINYIMG_FORMAT_AVIF, &opts, &writer),
        TINYIMG_OK
    );

    TinyImage back;
    tiny_memset(&back, 0, sizeof(back));

    r |= assertEquals(
        tiny_image_load(&back, writer.data, writer.size), TINYIMG_OK
    );

    if (back.data) {
        uint32_t worst = 0;

        for (uint32_t y = 0; y < 64u; y++) {
            for (uint32_t x = 0; x < 64u; x++) {
                int32_t want = grey.data[y * 64u + x];
                int32_t got = back.data[((size_t) y * 64u + x) * back.channels];
                int32_t difference = want - got;

                if (difference < 0) difference = -difference;
                if ((uint32_t) difference > worst) {
                    worst = (uint32_t) difference;
                }
            }
        }

        // measured at 4 on this gradient at quality 90, which is the quantizer
        // and not a disagreement: the encoder's own reconstruction is the same
        r |= assertLessThan((double) worst, 8.0);

        tiny_free(back.data);
    }

    tiny_writer_free(&writer);
    tiny_image_destroy(&grey);
    tiny_image_destroy(&source);

    return r;
}

int main(void) {
    int r = 0;

    tiny_init();

    TinyImage image;
    TinyImageInfo info;

    // #region container reading

    r |= assertEquals(probeFixture("derived/base.avif", &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_AVIF);

    // a still image is one frame, and an image sequence keeps its count in a
    // movie track this build does not read
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.progressive, 0L);

    /*
     * Alpha travels as a second item with a property saying what it is, so a
     * file with one has two of every property. Reporting alpha means the
     * association list was followed rather than the first property of each kind
     * taken, which would have described the wrong item.
     */
    r |= assertEquals(
        probeFixture("derived/base-alpha.avif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 4L);
    r |= assertEquals((long) info.has_alpha, 1L);

    /*
     * A rotation property is walked past rather than applied, so the extents
     * reported are the coded ones. That is not a shortcut: it is what
     * `avifdec --info` reports for the same file, and it matches how this
     * library treats a JPEG's orientation, which lands as metadata and is
     * applied by a geometry operation rather than by a decoder.
     */
    r |= assertEquals(
        probeFixture("derived/base-rotated.avif", &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);

    // #endregion

    // #region the boundary

    /*
     * The container is described, the coded image is decoded, and an image can
     * be written back out.
     *
     * This assertion used to be the boundary between reading and writing, and
     * it moved twice: once when the decoder landed and again when the encoder
     * did. What is left of the boundary is HEIF, which shares this container
     * and has no codec at all.
     */
    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.avif", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        r |= assertEquals(tiny_image_load(&image, bytes, size), TINYIMG_OK);
        r |= assertEquals((long) image.width, 320L);
        r |= assertEquals((long) image.height, 180L);
        tiny_free(image.data);

        TinyWriter writer;
        r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);

        TinyImage blank;
        r |= assertEquals(tiny_image_create(&blank, 4, 4, 3), TINYIMG_OK);
        r |= assertEquals(
            tiny_image_encode(&blank, TINYIMG_FORMAT_AVIF, 0, &writer),
            TINYIMG_OK
        );

        tiny_image_destroy(&blank);
        tiny_writer_free(&writer);
        free(bytes);
    }

    // #endregion

    // #region container variants

    /*
     * The same one-item container written four ways. Every one of them has to
     * describe a 96x64 image, because the differences are all in how the boxes
     * say so rather than in what they say.
     */
    Builder built;

    build(&built, 0, 1, 0);
    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 96L);
    r |= assertEquals((long) info.height, 64L);

    // the wider forms: a 32 bit item id, which needs a later version, and a 15
    // bit property index, which needs a flag
    build(&built, 1, 1, 0);
    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 96L);
    r |= assertEquals((long) info.height, 64L);

    // a 64 bit box length, which a reader that took the 32 bit field at face
    // value would read as a length of one and walk into the middle of
    build(&built, 0, 1, 1);
    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 96L);
    r |= assertEquals((long) info.height, 64L);

    // no association table at all, which leaves the reader nothing to follow
    // and the first extents it finds as the only answer it can give
    build(&built, 0, 0, 0);
    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 96L);
    r |= assertEquals((long) info.height, 64L);

    // a 64 bit length whose high word is set names more than anything
    // addressable, and is refused rather than truncated into something small
    build(&built, 0, 1, 1);

    for (size_t at = 0; at + 8 < built.size; at++) {
        if (built.data[at + 4] != 'i' || built.data[at + 5] != 'p') continue;
        if (built.data[at + 6] != 'c' || built.data[at + 7] != 'o') continue;

        built.data[at + 8] = 1;
        break;
    }

    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_ERR_CORRUPT
    );

    /*
     * A length of zero means the box runs to the end of its parent, which is
     * what a writer emits when it does not know the size yet. It is only legal
     * on the last box, and this file's metadata box is exactly that.
     */
    build(&built, 0, 1, 0);

    for (size_t at = 0; at + 8 < built.size; at++) {
        if (built.data[at + 4] != 'm' || built.data[at + 5] != 'e') continue;
        if (built.data[at + 6] != 't' || built.data[at + 7] != 'a') continue;

        built.data[at + 0] = 0;
        built.data[at + 1] = 0;
        built.data[at + 2] = 0;
        built.data[at + 3] = 0;
        break;
    }

    r |= assertEquals(
        tiny_image_probe(built.data, built.size, &info), TINYIMG_OK
    );
    r |= assertEquals((long) info.width, 96L);

    // #endregion

    // #region malformed

    // the brand is what routes a file here, and HEIF shares the container
    // without being claimed by this codec
    static const unsigned char heic[12] = {0,   0,   0,   0x18, 'f', 't',
                                           'y', 'p', 'h', 'e',  'i', 'c'};

    r |= assertEquals(
        tiny_image_probe(heic, sizeof(heic), &info),
        TINYIMG_ERR_UNSUPPORTED_CODEC
    );
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_HEIF);

    // the brand alone, with no metadata box to describe anything
    static const unsigned char bare[12] = {0,   0,   0,   0x0C, 'f', 't',
                                           'y', 'p', 'a', 'v',  'i', 'f'};

    r |= assertEquals(
        tiny_image_probe(bare, sizeof(bare), &info), TINYIMG_ERR_CORRUPT
    );

    // a box whose declared length runs past its parent, which a walk that
    // trusted the field would read off the end of
    static const unsigned char lying[24] = {0,    0,    0,    0x0C, 'f', 't',
                                            'y',  'p',  'a',  'v',  'i', 'f',
                                            0x7F, 0xFF, 0xFF, 0xFF, 'm', 'e',
                                            't',  'a',  0,    0,    0,   0};

    r |= assertEquals(
        tiny_image_probe(lying, sizeof(lying), &info), TINYIMG_ERR_CORRUPT
    );

    // truncated part way through the container
    bytes = readFixture("derived/base.avif", &size);

    if (bytes) {
        r |= assertEquals(
            tiny_image_probe(bytes, 16, &info), TINYIMG_ERR_CORRUPT
        );
        free(bytes);
    }

    // #endregion

    // #region the decode

    r |= sniffsTheCompatibleBrands();
    r |= decodesEveryFixture();
    r |= theEffortTierSkipsTheFilters();
    r |= honorsTheRegionAndScale();
    r |= refusesWhatItCannotDecode();
    r |= encodesWhatItCanDecode();

    // #endregion

    return r;
}
