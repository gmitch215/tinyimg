#include "tinyimg/codec/avif.h"

#include "tinyimg/memory.h"

/** The URN an auxiliary item uses to say it carries alpha. */
#define AVIF_ALPHA_URN "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"

typedef struct {
    const uint8_t* data;
    size_t size;

    /** Which item the file says to show. */
    uint32_t primary;

    /** The ordered property container and the associations into it. */
    size_t properties;
    size_t properties_end;
    size_t associations;
    size_t associations_end;

    uint32_t width;
    uint32_t height;
    uint8_t channels;
    uint8_t depth;
    uint8_t has_alpha;
} AvifHeader;

#pragma region boxes

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | p[3];
}

static uint32_t read_be16(const uint8_t* p) {
    return ((uint32_t) p[0] << 8) | p[1];
}

static int is_box(const uint8_t* p, const char* type) {
    return p[0] == (uint8_t) type[0] && p[1] == (uint8_t) type[1] &&
           p[2] == (uint8_t) type[2] && p[3] == (uint8_t) type[3];
}

/**
 * Walks the boxes of one container.
 *
 * Every level of the format is the same shape, a length and a four character
 * type, so one walker serves the whole file. A length of one means the real
 * one is the sixty four bit field that follows, and a length of zero means the
 * box runs to the end of its parent.
 */
typedef struct {
    const uint8_t* data;
    size_t at;
    size_t end;
} AvifWalk;

static void walk_init(
    AvifWalk* walk, const uint8_t* data, size_t at, size_t end
) {
    walk->data = data;
    walk->at = at;
    walk->end = end;
}

/**
 * Advances to the next box.
 *
 * @param walk The walker.
 * @param type Receives a pointer to the four type bytes.
 * @param payload Receives the offset of the box's contents.
 * @param size Receives the length of those contents.
 * @return int Non-zero while a box was read.
 */
static int walk_next(
    AvifWalk* walk, const uint8_t** type, size_t* payload, size_t* size
) {
    if (walk->at + 8 > walk->end) return 0;

    uint64_t length = read_be32(walk->data + walk->at);
    size_t header = 8;

    if (length == 1) {
        if (walk->at + 16 > walk->end) return 0;

        // a sixty four bit length, whose high word this build refuses rather
        // than truncating: nothing addressable is that large
        if (read_be32(walk->data + walk->at + 8) != 0) return 0;

        length = read_be32(walk->data + walk->at + 12);
        header = 16;
    }
    else if (length == 0) {
        length = walk->end - walk->at;
    }

    if (length < header || walk->at + length > walk->end) {
        // a box claiming more than its parent holds is read to the parent's
        // end, which is what lets a truncated file still describe itself
        length = walk->end - walk->at;
        if (length < header) return 0;
    }

    *type = walk->data + walk->at + 4;
    *payload = walk->at + header;
    *size = (size_t) length - header;

    walk->at += (size_t) length;
    return 1;
}

/**
 * Matches the two AVIF brands, and deliberately not the HEIF ones.
 *
 * It has to agree exactly with `tiny_format_sniff`, which separates the two
 * families by the same brand: a codec that claimed more than the sniffer routes
 * to it would be found by one path and not the other.
 */
static int avif_sniff(const uint8_t* buffer, size_t size) {
    if (!buffer || size < 12) return 0;
    if (!is_box(buffer + 4, "ftyp")) return 0;

    return is_box(buffer + 8, "avif") || is_box(buffer + 8, "avis");
}

#pragma endregion

#pragma region properties

/**
 * Reads the property associations for the primary item.
 *
 * Every item in the file has a list of one based indices into the ordered
 * property container, and this fills in the ones that describe an image. Taking
 * the first property of each kind instead would read the alpha plane's extents
 * on any file that has one, which is most of them.
 */
static void read_associations(AvifHeader* header) {
    if (!header->associations) return;

    const uint8_t* data = header->data;
    size_t at = header->associations;
    size_t end = header->associations_end;

    if (at + 4 > end) return;

    uint32_t version = data[at];
    uint32_t flags = read_be32(data + at) & 0xFFFFFFu;

    at += 4;
    if (at + 4 > end) return;

    uint32_t entries = read_be32(data + at);
    at += 4;

    for (uint32_t i = 0; i < entries && at < end; i++) {
        uint32_t item;

        if (version < 1) {
            if (at + 2 > end) return;
            item = read_be16(data + at);
            at += 2;
        }
        else {
            if (at + 4 > end) return;
            item = read_be32(data + at);
            at += 4;
        }

        if (at >= end) return;

        uint32_t count = data[at++];

        for (uint32_t j = 0; j < count; j++) {
            uint32_t index;

            // the low flag bit widens the index from seven bits to fifteen,
            // and the high bit of the first byte is an essential marker rather
            // than part of the number
            if (flags & 1u) {
                if (at + 2 > end) return;
                index = read_be16(data + at) & 0x7FFFu;
                at += 2;
            }
            else {
                if (at >= end) return;
                index = data[at] & 0x7Fu;
                at++;
            }

            if (item != header->primary || index == 0) continue;

            // the index counts the container's children, so the container is
            // walked rather than indexed
            AvifWalk walk;
            walk_init(&walk, data, header->properties, header->properties_end);

            const uint8_t* type;
            size_t payload;
            size_t size;
            uint32_t seen = 0;

            while (walk_next(&walk, &type, &payload, &size)) {
                if (++seen != index) continue;

                if (is_box(type, "ispe") && size >= 12) {
                    header->width = read_be32(data + payload + 4);
                    header->height = read_be32(data + payload + 8);
                }
                else if (is_box(type, "pixi") && size >= 5) {
                    header->channels = data[payload + 4];

                    if (size >= 6) header->depth = data[payload + 5];
                }

                break;
            }
        }
    }
}

/** Tests whether an auxiliary property declares the alpha type. */
static int is_alpha_aux(const uint8_t* data, size_t payload, size_t size) {
    static const char urn[] = AVIF_ALPHA_URN;
    size_t length = sizeof(urn) - 1;

    // a full box, so four bytes of version and flags come before the string
    if (size < 4 + length) return 0;

    for (size_t i = 0; i < length; i++) {
        if (data[payload + 4 + i] != (uint8_t) urn[i]) return 0;
    }

    return 1;
}

/**
 * Reads the container far enough to describe the primary item.
 *
 * The boxes that matter are nested three deep and may appear in any order, so
 * the offsets are recorded on the way past and resolved afterwards.
 */
static int avif_parse(const uint8_t* buffer, size_t size, AvifHeader* header) {
    if (!avif_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    tiny_memset(header, 0, sizeof(*header));

    header->data = buffer;
    header->size = size;
    header->depth = 8;

    AvifWalk top;
    walk_init(&top, buffer, 0, size);

    const uint8_t* type;
    size_t payload;
    size_t length;
    size_t meta = 0;
    size_t meta_end = 0;

    while (walk_next(&top, &type, &payload, &length)) {
        if (is_box(type, "meta")) {
            // a full box, so its four version and flags bytes come before the
            // children rather than at the start of the payload
            if (length < 4) return TINYIMG_ERR_CORRUPT;

            meta = payload + 4;
            meta_end = payload + length;
            break;
        }
    }

    if (!meta) return TINYIMG_ERR_CORRUPT;

    AvifWalk inside;
    walk_init(&inside, buffer, meta, meta_end);

    while (walk_next(&inside, &type, &payload, &length)) {
        if (is_box(type, "pitm")) {
            if (length < 6) continue;

            uint32_t version = buffer[payload];

            header->primary = version < 1 ? read_be16(buffer + payload + 4)
                                          : read_be32(buffer + payload + 4);
        }
        else if (is_box(type, "iprp")) {
            AvifWalk properties;
            walk_init(&properties, buffer, payload, payload + length);

            const uint8_t* child;
            size_t at;
            size_t child_size;

            while (walk_next(&properties, &child, &at, &child_size)) {
                if (is_box(child, "ipco")) {
                    header->properties = at;
                    header->properties_end = at + child_size;
                }
                else if (is_box(child, "ipma")) {
                    header->associations = at;
                    header->associations_end = at + child_size;
                }
            }
        }
    }

    read_associations(header);

    if (header->properties) {
        AvifWalk properties;
        walk_init(
            &properties, buffer, header->properties, header->properties_end
        );

        while (walk_next(&properties, &type, &payload, &length)) {
            if (is_box(type, "auxC") && is_alpha_aux(buffer, payload, length)) {
                header->has_alpha = 1;
            }

            // a file with no associations at all still has extents somewhere,
            // and taking the first is better than reporting nothing
            if (!header->associations && is_box(type, "ispe") && length >= 12 &&
                header->width == 0) {
                header->width = read_be32(buffer + payload + 4);
                header->height = read_be32(buffer + payload + 8);
            }
        }
    }

    if (header->width == 0 || header->height == 0) return TINYIMG_ERR_CORRUPT;

    if (header->channels == 0) header->channels = 3;
    if (header->has_alpha && header->channels < 4) header->channels = 4;

    return TINYIMG_OK;
}

#pragma endregion

static int avif_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    AvifHeader header;
    int result = avif_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    info->width = header.width;
    info->height = header.height;
    info->frames = 1;
    info->format = TINYIMG_FORMAT_AVIF;
    info->channels = header.channels;
    info->bit_depth = header.depth;
    info->has_alpha = header.has_alpha;
    info->progressive = 0;

    return TINYIMG_OK;
}

const TinyCodec tiny_codec_avif = {
    TINYIMG_FORMAT_AVIF, avif_sniff, avif_probe, 0, 0
};
