#include "../test.h"
#include "tinyimg/codec/codec.h"
#include "tinyimg/memory.h"

/** Decodes a fixture at a requested channel count, or reports why it could not.
 */
static int decodeFixture(const char* name, TinyImage* image, uint8_t channels) {
    size_t size = 0;
    unsigned char* bytes = readFixture(name, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    TinyDecodeOpts opts = {0, 0, 0, 0, 1, channels};
    int result = tiny_image_decode(image, bytes, size, &opts);

    free(bytes);
    return result;
}

int main(void) {
    int r = 0;

    TinyImage image;
    TinyImageInfo info;

    // #region header reading

    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.png", &size);
    r |= assertNotNull(bytes);
    if (!bytes) return 1;

    r |= assertEquals(tiny_image_probe(bytes, size, &info), TINYIMG_OK);
    r |= assertEquals((long) info.width, 320L);
    r |= assertEquals((long) info.height, 180L);
    r |= assertEquals((long) info.channels, 3L);
    r |= assertEquals((long) info.bit_depth, 8L);
    r |= assertEquals((long) info.has_alpha, 0L);
    r |= assertEquals((long) info.progressive, 0L);
    r |= assertEquals((long) info.frames, 1L);
    r |= assertEquals((long) info.format, (long) TINYIMG_FORMAT_PNG);

    // the probe is the last read of this buffer; the helpers below own theirs
    free(bytes);

    // #endregion

    // #region the same picture through every representation

    // one image written six ways. every one has to decode to the same pixels,
    // which is what pins the 16 bit reduction, the palette lookup and the whole
    // Adam7 reconstruction at once, with no reference file to trust
    TinyImage reference;
    r |= assertEquals(
        decodeFixture("derived/base.png", &reference, 3), TINYIMG_OK
    );
    r |= assertEquals((long) reference.width, 320L);

    static const char* identical[] = {
        "derived/base-rgb8.png", "derived/base-rgb16.png",
        "derived/base-interlaced.png"
    };

    for (size_t i = 0; i < sizeof(identical) / sizeof(identical[0]); i++) {
        r |= assertEquals(decodeFixture(identical[i], &image, 3), TINYIMG_OK);
        r |= assertImageEquals(&image, &reference);
        tiny_image_destroy(&image);
    }

    // a different codec entirely, on the same source picture, has to agree too.
    // BMP stores rows bottom-up in BGR and PNG stores them top-down in RGB, so
    // agreement rules out both a row order and a channel order mistake in
    // either one
    r |= assertEquals(decodeFixture("derived/base.bmp", &image, 3), TINYIMG_OK);
    r |= assertImageEquals(&image, &reference);
    tiny_image_destroy(&image);

    // the palette copy went through a 256 color quantizer, so it only has to
    // be close
    r |= assertEquals(
        decodeFixture("derived/base-palette.png", &image, 3), TINYIMG_OK
    );
    r |= assertPSNR(image.data, reference.data, (size_t) 320 * 180 * 3, 28.0);
    tiny_image_destroy(&image);

    tiny_image_destroy(&reference);

    // #endregion

    // #region color types

    r |= assertEquals(
        decodeFixture("derived/base-gray8.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 1L);
    tiny_image_destroy(&image);

    r |= assertEquals(
        decodeFixture("derived/base-gray-alpha8.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 2L);
    tiny_image_destroy(&image);

    r |= assertEquals(
        decodeFixture("derived/base-rgba8.png", &image, 0), TINYIMG_OK
    );
    r |= assertEquals((long) image.channels, 4L);
    tiny_image_destroy(&image);

    // a 4 bit palette with tRNS: the sub-byte depth, the palette and palette
    // transparency all at once, and the only fixture that reaches any of them
    r |= assertEquals(tiny_image_probe(0, 0, &info), TINYIMG_ERR_NULL);

    size_t logo_size = 0;
    unsigned char* logo = readFixture("webassembly.png", &logo_size);
    r |= assertNotNull(logo);

    if (logo) {
        r |= assertEquals(tiny_image_probe(logo, logo_size, &info), TINYIMG_OK);
        r |= assertEquals((long) info.bit_depth, 4L);
        r |= assertEquals((long) info.has_alpha, 1L);
        r |= assertEquals((long) info.channels, 4L);
        free(logo);
    }

    r |= assertEquals(decodeFixture("webassembly.png", &image, 0), TINYIMG_OK);
    r |= assertEquals((long) image.channels, 4L);
    r |= assertEquals((long) image.width, 512L);

    // the logo sits on an opaque square with transparency around it, so both
    // have to be present. ignoring tRNS would leave every pixel opaque, and
    // mapping the wrong palette entry to it would leave the whole image
    // transparent
    uint32_t clear = 0;
    uint32_t opaque = 0;

    for (uint32_t i = 0; i < 512u * 512u; i++) {
        if (image.data[i * 4 + 3] == 0) clear++;
        if (image.data[i * 4 + 3] == 255) opaque++;
    }
    r |= assertEquals((long) clear, 37847L);
    r |= assertGreaterThan((double) opaque, 100000.0);

    // and the opaque part is the logo's own purple, which pins the palette
    // lookup itself
    uint8_t corner[4];
    r |= assertEquals(tiny_image_getpixel(&image, 0, 0, corner), TINYIMG_OK);
    r |= assertEquals((long) corner[0], 101L);
    r |= assertEquals((long) corner[1], 79L);
    r |= assertEquals((long) corner[2], 240L);
    r |= assertEquals((long) corner[3], 255L);
    tiny_image_destroy(&image);

    // a stream split across many IDAT chunks. a decoder that inflated each one
    // separately reads this as corrupt
    r |= assertEquals(decodeFixture("forest.png", &image, 0), TINYIMG_OK);
    r |= assertEquals((long) image.width, 2000L);
    r |= assertEquals((long) image.height, 831L);
    r |= assertEquals((long) image.channels, 4L);
    tiny_image_destroy(&image);

    // #endregion

    // #region region and scale

    TinyImage full;
    r |= assertEquals(decodeFixture("derived/base.png", &full, 3), TINYIMG_OK);

    TinyImage region;
    unsigned char* source = readFixture("derived/base.png", &size);
    r |= assertNotNull(source);

    if (source) {
        r |= assertEquals(
            tiny_image_load_region(&region, source, size, 37, 21, 64, 48),
            TINYIMG_OK
        );
        r |= assertEquals((long) region.width, 64L);
        r |= assertEquals((long) region.height, 48L);

        int matches = 1;
        for (uint32_t y = 0; y < 48; y++) {
            const uint8_t* wanted =
                full.data + ((size_t) (y + 21) * 320 + 37) * 3;
            const uint8_t* got = region.data + (size_t) y * 64 * 3;

            if (tiny_memcmp(wanted, got, 64 * 3) != 0) matches = 0;
        }
        r |= assertTrue(matches);
        tiny_image_destroy(&region);

        // a region starting outside the image is refused rather than clamped
        // into the wrong pixels
        r |= assertEquals(
            tiny_image_load_region(&region, source, size, 320, 0, 8, 8),
            TINYIMG_ERR_BOUNDS
        );

        // a scaled decode has to be the box average of a full one, exactly as
        // for any other codec
        for (uint8_t den = 2; den <= 8; den = (uint8_t) (den * 2)) {
            TinyDecodeOpts opts = {0, 0, 0, 0, den, 3};
            TinyImage scaled;

            r |= assertEquals(
                tiny_image_decode(&scaled, source, size, &opts), TINYIMG_OK
            );
            r |= assertEquals(
                (long) scaled.width, (long) ((320 + den - 1) / den)
            );

            int averaged = 1;
            for (uint32_t y = 0; y < scaled.height; y++) {
                for (uint32_t x = 0; x < scaled.width; x++) {
                    uint32_t sums[3] = {0, 0, 0};
                    uint32_t count = 0;

                    for (uint32_t sy = y * den; sy < (y + 1u) * den && sy < 180;
                         sy++) {
                        for (uint32_t sx = x * den;
                             sx < (x + 1u) * den && sx < 320; sx++) {
                            const uint8_t* pixel =
                                full.data + ((size_t) sy * 320 + sx) * 3;

                            sums[0] += pixel[0];
                            sums[1] += pixel[1];
                            sums[2] += pixel[2];
                            count++;
                        }
                    }

                    const uint8_t* got =
                        scaled.data + ((size_t) y * scaled.width + x) * 3;

                    for (int c = 0; c < 3; c++) {
                        if (got[c] !=
                            (uint8_t) ((sums[c] + count / 2) / count)) {
                            averaged = 0;
                        }
                    }
                }
            }
            r |= assertTrue(averaged);
            tiny_image_destroy(&scaled);
        }

        // an interlaced file cannot stream, but a region of one still has to be
        // right
        size_t laced_size = 0;
        unsigned char* laced =
            readFixture("derived/base-interlaced.png", &laced_size);
        r |= assertNotNull(laced);

        if (laced) {
            r |= assertEquals(
                tiny_image_load_region(
                    &region, laced, laced_size, 37, 21, 64, 48
                ),
                TINYIMG_OK
            );

            int lacedMatches = 1;
            for (uint32_t y = 0; y < 48; y++) {
                const uint8_t* wanted =
                    full.data + ((size_t) (y + 21) * 320 + 37) * 3;
                const uint8_t* got = region.data + (size_t) y * 64 * 3;

                if (tiny_memcmp(wanted, got, 64 * 3) != 0) lacedMatches = 0;
            }
            r |= assertTrue(lacedMatches);
            tiny_image_destroy(&region);
            free(laced);
        }

        free(source);
    }

    // #endregion

    // #region round trip

    // PNG is lossless, so every channel count has to come back exactly as it
    // went out
    for (uint8_t channels = 1; channels <= 4; channels++) {
        TinyImage copy;
        r |= assertEquals(
            decodeFixture("derived/base.png", &copy, channels), TINYIMG_OK
        );

        TinyWriter out;
        r |= assertEquals(tiny_writer_init(&out, 0), TINYIMG_OK);
        r |= assertEquals(
            tiny_image_encode(&copy, TINYIMG_FORMAT_PNG, 0, &out), TINYIMG_OK
        );
        r |= assertEquals(
            (long) tiny_format_sniff(out.data, out.size),
            (long) TINYIMG_FORMAT_PNG
        );

        TinyImage back;
        r |= assertEquals(
            tiny_image_load(&back, out.data, out.size), TINYIMG_OK
        );
        r |= assertImageEquals(&back, &copy);

        tiny_image_destroy(&back);
        tiny_writer_free(&out);
        tiny_image_destroy(&copy);
    }

    // a one pixel image is where a fixed Huffman block gets chosen, which is
    // the only place a wrong fixed code table shows up
    TinyImage single;
    r |= assertEquals(
        decodeFixture("derived/single-pixel.png", &single, 3), TINYIMG_OK
    );
    r |= assertEquals((long) single.width, 1L);

    TinyWriter tiny;
    r |= assertEquals(tiny_writer_init(&tiny, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&single, TINYIMG_FORMAT_PNG, 0, &tiny), TINYIMG_OK
    );

    TinyImage singleBack;
    r |= assertEquals(
        tiny_image_load(&singleBack, tiny.data, tiny.size), TINYIMG_OK
    );
    r |= assertImageEquals(&singleBack, &single);
    tiny_image_destroy(&singleBack);
    tiny_writer_free(&tiny);
    tiny_image_destroy(&single);

    // flat artwork is the case the unfiltered candidate exists for: it has to
    // come out no larger than the adaptive filter alone would give
    TinyImage flat;
    r |= assertEquals(decodeFixture("derived/logo.png", &flat, 4), TINYIMG_OK);

    TinyWriter flatOut;
    r |= assertEquals(tiny_writer_init(&flatOut, 0), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_encode(&flat, TINYIMG_FORMAT_PNG, 0, &flatOut), TINYIMG_OK
    );

    // 96x96 RGBA is 36,864 bytes raw and the adaptive stream alone came to
    // 1,789
    r |= assertLessThan((double) flatOut.size, 1500.0);

    TinyImage flatBack;
    r |= assertEquals(
        tiny_image_load(&flatBack, flatOut.data, flatOut.size), TINYIMG_OK
    );
    r |= assertImageEquals(&flatBack, &flat);
    tiny_image_destroy(&flatBack);
    tiny_writer_free(&flatOut);
    tiny_image_destroy(&flat);

    tiny_image_destroy(&full);

    // #endregion

    // #region malformed

    // the signature alone, with no IHDR
    r |= assertEquals(
        decodeFixture("derived/malformed/signature-only.png", &image, 0),
        TINYIMG_ERR_CORRUPT
    );
    r |= assertEquals(
        decodeFixture("derived/malformed/truncated.png", &image, 0),
        TINYIMG_ERR_CORRUPT
    );

    // the absurd dimensions fixture was made by editing IHDR's width without
    // repairing its CRC, so the honest answer is that the chunk is corrupt.
    // reporting the size instead would mean trusting a field the checksum has
    // already failed
    r |= assertEquals(
        decodeFixture("derived/malformed/absurd-dimensions.png", &image, 0),
        TINYIMG_ERR_CORRUPT
    );

    // so the pixel budget needs a header that is oversized and correct, which
    // the test builds rather than borrowing
    TinyWriter oversized;
    r |= assertEquals(tiny_writer_init(&oversized, 0), TINYIMG_OK);

    static const uint8_t png_signature[8] = {0x89, 'P',  'N',  'G',
                                             0x0D, 0x0A, 0x1A, 0x0A};
    tiny_writer_write(&oversized, png_signature, 8);

    // 20000 x 20000 is 400 Mpx against a 16 Mpx budget
    uint8_t big[17] = {'I',  'H',  'D',  'R', 0x00, 0x00, 0x4E, 0x20, 0x00,
                       0x00, 0x4E, 0x20, 8,   2,    0,    0,    0};

    tiny_writer_be32(&oversized, 13);
    tiny_writer_write(&oversized, big, sizeof(big));
    tiny_writer_be32(&oversized, tiny_crc32(0, big, sizeof(big)));

    // an IDAT so the parse does not stop for want of one
    uint8_t idat[5] = {'I', 'D', 'A', 'T', 0x78};
    tiny_writer_be32(&oversized, 1);
    tiny_writer_write(&oversized, idat, sizeof(idat));
    tiny_writer_be32(&oversized, tiny_crc32(0, idat, sizeof(idat)));

    r |= assertEquals(
        tiny_image_load(&image, oversized.data, oversized.size),
        TINYIMG_ERR_TOO_LARGE
    );
    tiny_writer_free(&oversized);

    // a single flipped byte anywhere in a chunk has to be caught by its CRC
    unsigned char* tampered = readFixture("derived/base.png", &size);
    r |= assertNotNull(tampered);

    if (tampered) {
        tampered[size / 2] ^= 0xFF;
        r |= assertEquals(
            tiny_image_load(&image, tampered, size), TINYIMG_ERR_CORRUPT
        );
        free(tampered);
    }

    // a truncated chunk length that runs off the end of the buffer
    unsigned char* clipped = readFixture("derived/base.png", &size);
    r |= assertNotNull(clipped);

    if (clipped) {
        r |= assertEquals(
            tiny_image_load(&image, clipped, 40), TINYIMG_ERR_CORRUPT
        );
        free(clipped);
    }

    // #endregion

    r |= assertEquals(
        tiny_image_encode(0, TINYIMG_FORMAT_PNG, 0, 0), TINYIMG_ERR_NULL
    );

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);

    // #region the encoder's effort tier

    /*
     * FAST compresses one candidate stream instead of two. The tier is only
     * worth having if it changes the output, and only defensible if the change
     * is confined to where it was measured, so both halves are asserted: a
     * photograph comes out byte-identical (adaptive wins either way) and flat
     * artwork comes out larger (unfiltered was the winner FAST gave up).
     *
     * A test that only checked the photograph would pass against an encoder
     * that ignored `effort` entirely, which is what this one did until now.
     */
    TinyImage photo;
    r |= assertEquals(decodeFixture("sf-24.jpg", &photo, 3), TINYIMG_OK);

    TinyWriter photoFancy;
    TinyWriter photoFast;
    tiny_writer_init(&photoFancy, 0);
    tiny_writer_init(&photoFast, 0);

    TinyEncodeOpts fancy;
    TinyEncodeOpts quick;
    tiny_memset(&fancy, 0, sizeof(fancy));
    tiny_memset(&quick, 0, sizeof(quick));
    quick.effort = TINYIMG_EFFORT_FAST;

    r |= assertEquals(
        tiny_image_encode(&photo, TINYIMG_FORMAT_PNG, &fancy, &photoFancy),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_encode(&photo, TINYIMG_FORMAT_PNG, &quick, &photoFast),
        TINYIMG_OK
    );

    // adaptive filtering is the better candidate on a photograph, so giving up
    // the other one costs nothing
    r |= assertEquals((long) photoFast.size, (long) photoFancy.size);

    tiny_writer_free(&photoFast);
    tiny_writer_free(&photoFancy);
    tiny_image_destroy(&photo);

    TinyImage artwork;
    r |= assertEquals(
        decodeFixture("derived/base-mono.gif", &artwork, 3), TINYIMG_OK
    );

    TinyWriter flatFancy;
    TinyWriter flatFast;
    tiny_writer_init(&flatFancy, 0);
    tiny_writer_init(&flatFast, 0);

    r |= assertEquals(
        tiny_image_encode(&artwork, TINYIMG_FORMAT_PNG, &fancy, &flatFancy),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_encode(&artwork, TINYIMG_FORMAT_PNG, &quick, &flatFast),
        TINYIMG_OK
    );

    // here unfiltered was the winner, so FAST is larger; measured at +7.0%
    r |= assertGreaterThan((double) flatFast.size, (double) flatFancy.size);

    tiny_writer_free(&flatFast);
    tiny_writer_free(&flatFancy);
    tiny_image_destroy(&artwork);

    // #endregion

    // #region the compression knob, and that its bytes have not moved

    /*
     * Every TinyCompression value on two fixtures, size and digest.
     *
     * Recorded from the build **before** the match finder was rewritten, so
     * they anchor the rewrite rather than snapshot its own output. Two zlib
     * filters and a four-byte comparison replaced a byte-at-a-time walk from
     * offset zero, worth 1.14x-2.01x on a photograph and 4.55x on this logo at
     * the longest chain, and none of it may change which candidate wins.
     *
     * **The first attempt at this anchor could not fail.** It compressed a
     * synthetic buffer of text, a run and noise; a filter deliberately keyed to
     * the wrong neighbor byte produced byte-identical output at every level,
     * because that input never puts a candidate exactly one byte longer than
     * the incumbent in the chain. On these two images the same mutation moves
     * six of the ten cells, which is what makes this a check rather than a
     * record. The four it leaves alone are the two NONE cells, which do no
     * matching, and the two FAST cells, where one probe per position means
     * there is no incumbent for the filter to compare against.
     *
     * One of the six comes out two bytes **smaller** under the mutation, which
     * is the reminder that a size comparison is not a correctness check here.
     *
     * AUTO is at quality 82, so it has to equal DEFAULT exactly. That is the
     * whole compatibility claim of the new field.
     */
    static const char* levelFiles[2] = {
        "derived/logo.png", "derived/base-mono.gif"
    };
    static const size_t levelSizes[2][5] = {
        {835u, 4346u, 948u, 835u, 664u}, {790u, 21952u, 900u, 790u, 627u}
    };
    static const uint64_t levelDigests[2][5] = {
        {0x1b287c9ca34037b8ULL, 0xc0fdaa071f429df9ULL, 0x49bf5c8b1ffd703fULL,
         0x1b287c9ca34037b8ULL, 0x333bcc15ff5cfc91ULL},
        {0xf3e48b054115c5d1ULL, 0xf0a076991bd8bb8cULL, 0xb771ac19a7997e8cULL,
         0xf3e48b054115c5d1ULL, 0x6231f69d76f595a5ULL}
    };

    for (uint32_t file = 0; file < 2u; file++) {
        TinyImage source;
        r |= assertEquals(
            decodeFixture(levelFiles[file], &source, 3), TINYIMG_OK
        );

        for (uint32_t level = 0; level < 5u; level++) {
            TinyEncodeOpts opts;
            tiny_memset(&opts, 0, sizeof(opts));
            opts.quality = 82;
            opts.compression = (uint8_t) level;

            TinyWriter written;
            tiny_writer_init(&written, 0);

            r |= assertEquals(
                tiny_image_encode(&source, TINYIMG_FORMAT_PNG, &opts, &written),
                TINYIMG_OK
            );
            r |= assertEquals(
                (long) written.size, (long) levelSizes[file][level]
            );

            uint64_t hash = 1469598103934665603ULL;
            for (size_t i = 0; i < written.size; i++) {
                hash = (hash ^ written.data[i]) * 1099511628211ULL;
            }

            r |= assertTrue(hash == levelDigests[file][level]);

            // and every level is still a lossless round trip of the source
            TinyImage back;
            r |= assertEquals(
                tiny_image_decode(&back, written.data, written.size, 0),
                TINYIMG_OK
            );
            r |= assertEquals((long) back.width, (long) source.width);
            r |= assertEquals(
                tiny_memcmp(
                    back.data, source.data,
                    (size_t) source.width * source.height * source.channels
                ),
                0
            );

            tiny_image_destroy(&back);
            tiny_writer_free(&written);
        }

        tiny_image_destroy(&source);
    }

    // #endregion

    return r;
}
