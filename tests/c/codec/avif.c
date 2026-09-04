#include "../test.h"
#include "tinyimg/codec/codec.h"

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

int main(void) {
    int r = 0;

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
     * The container is described and the coded image is not. A caller has to be
     * able to tell that apart from an unreadable file, which is what the
     * specific error is for.
     */
    size_t size = 0;
    unsigned char* bytes = readFixture("derived/base.avif", &size);
    r |= assertNotNull(bytes);

    if (bytes) {
        r |= assertEquals(
            tiny_image_load(&image, bytes, size), TINYIMG_ERR_UNSUPPORTED_CODEC
        );

        TinyWriter writer;
        r |= assertEquals(tiny_writer_init(&writer, 0), TINYIMG_OK);

        TinyImage blank;
        r |= assertEquals(tiny_image_create(&blank, 4, 4, 3), TINYIMG_OK);
        r |= assertEquals(
            tiny_image_encode(&blank, TINYIMG_FORMAT_AVIF, 0, &writer),
            TINYIMG_ERR_UNSUPPORTED_CODEC
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

    return r;
}
