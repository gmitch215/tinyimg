#include "tinyimg/codec/avif.h"

#include "av1.h"

#include "tinyimg/memory.h"

/** The URN an auxiliary item uses to say it carries alpha. */
#define AVIF_ALPHA_URN "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"

/** Items a file may declare before the parse gives up describing them all. */
#define AVIF_MAX_ITEMS 32

/** Extents one item's data may be split across. */
#define AVIF_MAX_EXTENTS 8

/** Packs a four character box type into a comparable word. */
#define AVIF_FOURCC(a, b, c, d)                                                \
    (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | \
     (uint32_t) (d))

/** One run of an item's bytes, already resolved to a file offset. */
typedef struct {
    size_t offset;
    size_t length;
} AvifExtent;

/**
 * One entry of the item table.
 *
 * An AVIF still is normally one `av01` item, optionally a second one carrying
 * alpha and pointed at by an `auxl` reference, and optionally a thumbnail
 * pointed at by a `thmb`. Metadata items (`Exif`, `mime`) also appear here and
 * are walked past.
 */
typedef struct {
    uint32_t id;
    uint32_t type;

    AvifExtent extents[AVIF_MAX_EXTENTS];
    uint32_t extent_count;

    /**
     * Non-zero when the offsets are relative to the `idat` box rather than to
     * the file, which is `iloc`'s construction method 1.
     */
    uint8_t in_item_data;
} AvifItem;

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

    /** The item table, and the `idat` payload construction method 1 needs. */
    AvifItem items[AVIF_MAX_ITEMS];
    uint32_t item_count;
    size_t item_data;
    size_t item_data_end;

    /**
     * The item ids the references resolve to, zero when the file has none.
     *
     * `alpha` is an auxiliary item whose `auxC` declares the alpha URN, so it
     * is only trusted once that property has been seen.
     */
    uint32_t alpha;
    uint32_t thumbnail;

    /** The primary item's `av1C`, which carries the sequence header OBU. */
    size_t config;
    size_t config_size;

    uint32_t width;
    uint32_t height;
    uint8_t channels;
    uint8_t depth;
    uint8_t has_alpha;

    /** From `av1C`, so the decoder need not re-read the sequence header. */
    uint8_t monochrome;
    uint8_t sub_x;
    uint8_t sub_y;
} AvifHeader;

#pragma region boxes

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | p[3];
}

static uint32_t read_be16(const uint8_t* p) {
    return ((uint32_t) p[0] << 8) | p[1];
}

/**
 * Reads a big endian field whose width the container declared.
 *
 * `iloc` sizes its offset, length, base and index fields in a nibble each, and
 * a width of zero is legal and reads as zero rather than as absent.
 */
static size_t read_variable(const uint8_t* p, uint32_t width) {
    size_t value = 0;

    for (uint32_t i = 0; i < width; i++) value = (value << 8) | p[i];

    return value;
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
 * Defers to `tiny_format_sniff` rather than matching brands again.
 *
 * The two have to agree exactly, because a codec that claimed more than the
 * sniffer routes to it would be found by one path and not the other. They used
 * to agree by both matching the major brand, and both were wrong about a file
 * whose major brand is `mif1` with `avif` in the compatible list. Calling the
 * one that decides is what makes them agree by construction.
 */
static int avif_sniff(const uint8_t* buffer, size_t size) {
    return tiny_format_sniff(buffer, size) == TINYIMG_FORMAT_AVIF;
}

#pragma endregion

#pragma region properties

/**
 * Reads the AV1 codec configuration record.
 *
 * The subsampling and monochrome flags are duplicated here and in the sequence
 * header OBU that follows them in the same box. This records the box's copy so
 * the container can describe the image without starting a bitstream decode, and
 * the decoder reads the OBU, which is authoritative if they ever disagree.
 */
static void read_av1_config(AvifHeader* header, size_t payload, size_t size) {
    if (size < 4) return;

    const uint8_t* data = header->data;

    header->depth = (data[payload + 2] & 0x40u)
                        ? ((data[payload + 2] & 0x20u) ? 12u : 10u)
                        : 8u;
    header->monochrome = (data[payload + 2] & 0x10u) ? 1u : 0u;
    header->sub_x = (data[payload + 2] & 0x08u) ? 1u : 0u;
    header->sub_y = (data[payload + 2] & 0x04u) ? 1u : 0u;

    header->config = payload + 4;
    header->config_size = size - 4;
}

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
                else if (is_box(type, "av1C")) {
                    read_av1_config(header, payload, size);
                }

                break;
            }
        }
    }
}

#pragma region items

/** Finds an item by id, or NULL. */
static AvifItem* find_item(AvifHeader* header, uint32_t id) {
    for (uint32_t i = 0; i < header->item_count; i++) {
        if (header->items[i].id == id) return &header->items[i];
    }

    return 0;
}

/** Adds an item, or returns NULL once the table is full. */
static AvifItem* add_item(AvifHeader* header, uint32_t id) {
    AvifItem* existing = find_item(header, id);
    if (existing) return existing;

    if (header->item_count >= AVIF_MAX_ITEMS) return 0;

    AvifItem* item = &header->items[header->item_count++];

    tiny_memset(item, 0, sizeof(*item));
    item->id = id;

    return item;
}

/**
 * Reads the item information box, which is what gives an item its type.
 *
 * The type is the only way to tell a coded image from an Exif blob, and the
 * alpha plane from the picture, so an item with no `infe` entry is unusable
 * even when `iloc` says where its bytes are.
 */
static void read_item_info(AvifHeader* header, size_t at, size_t end) {
    const uint8_t* data = header->data;

    if (at + 4 > end) return;

    uint32_t version = data[at];
    at += 4;

    uint32_t count;

    if (version == 0) {
        if (at + 2 > end) return;
        count = read_be16(data + at);
        at += 2;
    }
    else {
        if (at + 4 > end) return;
        count = read_be32(data + at);
        at += 4;
    }

    AvifWalk walk;
    walk_init(&walk, data, at, end);

    const uint8_t* type;
    size_t payload;
    size_t size;
    uint32_t seen = 0;

    while (seen < count && walk_next(&walk, &type, &payload, &size)) {
        if (!is_box(type, "infe")) continue;

        seen++;
        if (size < 4) continue;

        uint32_t entry = data[payload];
        size_t p = payload + 4;

        uint32_t id;

        if (entry < 3) {
            // version 0 and 1 carry a sixteen bit id, a protection index, and
            // then the type
            if (p + 8 > payload + size) continue;

            id = read_be16(data + p);
            p += 4;
        }
        else {
            if (p + 10 > payload + size) continue;

            id = read_be32(data + p);
            p += 6;
        }

        AvifItem* item = add_item(header, id);
        if (!item) return;

        item->type =
            AVIF_FOURCC(data[p], data[p + 1], data[p + 2], data[p + 3]);
    }
}

/**
 * Reads the item location box.
 *
 * The four nibble-wide size fields are what make this box awkward: an offset
 * field of zero bytes is legal and means the base offset carries the whole
 * address, which is what `cavif` emits. Reading a zero width field as anything
 * other than zero puts the primary item's bytes at the wrong place.
 */
static void read_item_location(AvifHeader* header, size_t at, size_t end) {
    const uint8_t* data = header->data;

    if (at + 6 > end) return;

    uint32_t version = data[at];
    at += 4;

    uint32_t offset_size = (uint32_t) (data[at] >> 4);
    uint32_t length_size = (uint32_t) (data[at] & 0x0Fu);
    uint32_t base_size = (uint32_t) (data[at + 1] >> 4);
    uint32_t index_size =
        version == 1 || version == 2 ? (uint32_t) (data[at + 1] & 0x0Fu) : 0;

    at += 2;

    uint32_t count;

    if (version < 2) {
        if (at + 2 > end) return;
        count = read_be16(data + at);
        at += 2;
    }
    else {
        if (at + 4 > end) return;
        count = read_be32(data + at);
        at += 4;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id;

        if (version < 2) {
            if (at + 2 > end) return;
            id = read_be16(data + at);
            at += 2;
        }
        else {
            if (at + 4 > end) return;
            id = read_be32(data + at);
            at += 4;
        }

        uint32_t construction = 0;

        if (version == 1 || version == 2) {
            if (at + 2 > end) return;
            construction = read_be16(data + at) & 0x0Fu;
            at += 2;
        }

        // data_reference_index, which a self-contained file leaves at zero
        if (at + 2 > end) return;
        at += 2;

        if (at + base_size > end) return;

        size_t base = read_variable(data + at, base_size);
        at += base_size;

        if (at + 2 > end) return;

        uint32_t extents = read_be16(data + at);
        at += 2;

        AvifItem* item = add_item(header, id);

        for (uint32_t e = 0; e < extents; e++) {
            if (at + index_size + offset_size + length_size > end) return;

            at += index_size;

            size_t offset = read_variable(data + at, offset_size);
            at += offset_size;

            size_t length = read_variable(data + at, length_size);
            at += length_size;

            if (!item || item->extent_count >= AVIF_MAX_EXTENTS) continue;

            item->in_item_data = construction == 1u;
            item->extents[item->extent_count].offset = base + offset;
            item->extents[item->extent_count].length = length;
            item->extent_count++;
        }
    }
}

/**
 * Reads the item reference box.
 *
 * `auxl` points from the auxiliary item to the one it describes, so the alpha
 * plane's own id is the `from` and the picture's is the `to`. `thmb` runs the
 * same direction, from the thumbnail to the image it stands for.
 */
static void read_item_references(AvifHeader* header, size_t at, size_t end) {
    const uint8_t* data = header->data;

    if (at + 4 > end) return;

    uint32_t version = data[at];
    at += 4;

    AvifWalk walk;
    walk_init(&walk, data, at, end);

    const uint8_t* type;
    size_t payload;
    size_t size;

    while (walk_next(&walk, &type, &payload, &size)) {
        size_t p = payload;
        size_t limit = payload + size;
        uint32_t from;

        if (version == 0) {
            if (p + 2 > limit) continue;
            from = read_be16(data + p);
            p += 2;
        }
        else {
            if (p + 4 > limit) continue;
            from = read_be32(data + p);
            p += 4;
        }

        if (p + 2 > limit) continue;

        uint32_t references = read_be16(data + p);
        p += 2;

        for (uint32_t i = 0; i < references; i++) {
            uint32_t to;

            if (version == 0) {
                if (p + 2 > limit) break;
                to = read_be16(data + p);
                p += 2;
            }
            else {
                if (p + 4 > limit) break;
                to = read_be32(data + p);
                p += 4;
            }

            if (to != header->primary) continue;

            if (is_box(type, "auxl"))
                header->alpha = from;
            else if (is_box(type, "thmb"))
                header->thumbnail = from;
        }
    }
}

#pragma endregion

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
    size_t references = 0;
    size_t references_end = 0;

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
            // version 1 widens the id to four bytes, so the length it needs is
            // eight rather than six; the version byte has to be read before the
            // bound can be checked against it
            if (length < 5) continue;

            uint32_t version = buffer[payload];
            if (length < (version < 1 ? 6u : 8u)) continue;

            header->primary = version < 1 ? read_be16(buffer + payload + 4)
                                          : read_be32(buffer + payload + 4);
        }
        else if (is_box(type, "iinf")) {
            read_item_info(header, payload, payload + length);
        }
        else if (is_box(type, "iloc")) {
            read_item_location(header, payload, payload + length);
        }
        else if (is_box(type, "idat")) {
            // construction method 1 addresses item bytes relative to this box
            header->item_data = payload;
            header->item_data_end = payload + length;
        }
        else if (is_box(type, "iref")) {
            // resolved after the walk, because a reference is only interesting
            // once pitm has said which item is primary
            references = payload;
            references_end = payload + length;
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

    if (references) read_item_references(header, references, references_end);

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

/**
 * @brief Gathers one item's extents into a contiguous buffer.
 *
 * An item can be split across extents, and construction method 1 puts them
 * inside `idat` rather than in the file. The AV1 decoder needs one range of
 * bytes, so a single-extent item is handed its own bytes and anything else is
 * copied; almost every file measured has one extent.
 */
static int item_bytes(
    const AvifHeader* header, const AvifItem* item, const uint8_t** out,
    size_t* out_size, uint8_t** owned
) {
    *owned = 0;

    if (item->extent_count == 0u) return TINYIMG_ERR_CORRUPT;

    size_t base = item->in_item_data ? header->item_data : 0u;
    size_t limit = item->in_item_data ? header->item_data_end : header->size;
    size_t total = 0;

    for (uint32_t i = 0; i < item->extent_count; i++) {
        size_t at = base + item->extents[i].offset;
        size_t length = item->extents[i].length;

        if (at > limit || length > limit - at) return TINYIMG_ERR_CORRUPT;

        total += length;
    }

    if (total == 0u) return TINYIMG_ERR_CORRUPT;

    if (item->extent_count == 1u) {
        *out = header->data + base + item->extents[0].offset;
        *out_size = total;

        return TINYIMG_OK;
    }

    uint8_t* joined = (uint8_t*) tiny_alloc(total);
    if (!joined) return TINYIMG_ERR_MEMORY;

    size_t written = 0;

    for (uint32_t i = 0; i < item->extent_count; i++) {
        size_t at = base + item->extents[i].offset;
        size_t length = item->extents[i].length;

        tiny_memcpy(joined + written, header->data + at, length);
        written += length;
    }

    *owned = joined;
    *out = joined;
    *out_size = total;

    return TINYIMG_OK;
}

/**
 * @brief Decodes the primary item, and its alpha when the file carries one.
 *
 * The alpha is a second AV1 item, monochrome, whose decoded plane becomes the
 * fourth channel. It is decoded only when the caller asked for four channels,
 * because decoding it otherwise is a whole second AV1 decode whose result is
 * thrown away.
 */
static int avif_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* opts
) {
    AvifHeader header;
    int result = avif_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    AvifItem* primary = find_item(&header, header.primary);
    if (!primary) return TINYIMG_ERR_CORRUPT;

    const uint8_t* bytes = 0;
    size_t bytes_size = 0;
    uint8_t* owned = 0;

    result = item_bytes(&header, primary, &bytes, &bytes_size, &owned);
    if (result != TINYIMG_OK) return result;

    /*
     * A caller who named no channel count gets the file's own, which for a file
     * with an alpha item is four.
     *
     * The AV1 decoder cannot know that: the alpha is a second item and only the
     * container says it exists, so defaulting inside the bitstream decoder
     * would return three channels for a transparent image and quietly drop the
     * transparency.
     */
    TinyDecodeOpts resolved;

    if (opts)
        resolved = *opts;
    else
        tiny_memset(&resolved, 0, sizeof(resolved));

    if (!resolved.channels) resolved.channels = header.channels;
    if (!resolved.scale_den) resolved.scale_den = 1u;

    result = tiny_av1_decode(image, bytes, bytes_size, &resolved);
    tiny_free(owned);

    if (result != TINYIMG_OK) return result;

    if (!header.alpha || image->channels != 4u) return TINYIMG_OK;

    AvifItem* alpha = find_item(&header, header.alpha);
    if (!alpha) return TINYIMG_OK;

    const uint8_t* alpha_bytes = 0;
    size_t alpha_size = 0;

    result = item_bytes(&header, alpha, &alpha_bytes, &alpha_size, &owned);
    if (result != TINYIMG_OK) return TINYIMG_OK;

    TinyImage plane;
    tiny_memset(&plane, 0, sizeof(plane));

    TinyDecodeOpts alpha_opts = resolved;

    alpha_opts.channels = 1u;

    int alpha_result =
        tiny_av1_decode(&plane, alpha_bytes, alpha_size, &alpha_opts);

    tiny_free(owned);

    /*
     * A file whose alpha will not decode keeps its colour and loses its
     * transparency, rather than failing.
     *
     * The alpha is a separate bitstream with its own sequence header, so it can
     * be a variant this decoder refuses while the colour item is not. Returning
     * an opaque image is the degradation the rest of this library takes when a
     * secondary feature is unavailable.
     */
    if (alpha_result != TINYIMG_OK) return TINYIMG_OK;

    if (plane.width == image->width && plane.height == image->height) {
        for (size_t i = 0; i < (size_t) image->width * image->height; i++) {
            image->data[i * 4u + 3u] = plane.data[i];
        }
    }

    tiny_free(plane.data);

    return TINYIMG_OK;
}

/** A box being written, whose length is filled in when it closes. */
typedef struct {
    TinyWriter* writer;
    size_t at;
} Box;

static int put_bytes(TinyWriter* writer, const void* bytes, size_t n) {
    return tiny_writer_write(writer, bytes, n);
}

static int put8(TinyWriter* writer, uint8_t value) {
    return tiny_writer_write(writer, &value, 1u);
}

static int put16(TinyWriter* writer, uint32_t value) {
    uint8_t bytes[2] = {(uint8_t) (value >> 8), (uint8_t) value};

    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

static int put32(TinyWriter* writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) (value >> 24), (uint8_t) (value >> 16),
        (uint8_t) (value >> 8), (uint8_t) value
    };

    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

/** Opens a box, leaving its length to be written back when it closes. */
static Box open_box(TinyWriter* writer, const char* type) {
    Box box;

    box.writer = writer;
    box.at = writer->size;

    put32(writer, 0u);
    put_bytes(writer, type, 4u);

    return box;
}

static void close_box(Box* box) {
    uint32_t length = (uint32_t) (box->writer->size - box->at);
    uint8_t* at = box->writer->data + box->at;

    at[0] = (uint8_t) (length >> 24);
    at[1] = (uint8_t) (length >> 16);
    at[2] = (uint8_t) (length >> 8);
    at[3] = (uint8_t) length;
}

/**
 * @brief Writes the container around one AV1 item.
 *
 * The smallest set of boxes a conformant AVIF still needs, which is more than
 * it sounds: the file type, a metadata box holding the handler, which item to
 * show, where its bytes are, what type they are, and three properties bound to
 * it by an association table. Leaving any of them out produces a file this
 * library would read and other decoders would not, which is the failure mode
 * worth avoiding.
 */
static int avif_encode(
    const TinyImage* image, const TinyEncodeOpts* opts, TinyWriter* writer
) {
    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0u || image->height == 0u) return TINYIMG_ERR_RANGE;

    uint32_t quality = opts && opts->quality ? opts->quality : 75u;

    size_t capacity = (size_t) image->width * image->height * 3u + (1u << 16);
    uint8_t* item = (uint8_t*) tiny_alloc(capacity);

    if (!item) return TINYIMG_ERR_MEMORY;

    size_t item_size = 0;
    TinyAv1Sequence described;

    int result =
        tiny_av1_encode(image, quality, item, capacity, &item_size, &described);

    if (result != TINYIMG_OK) {
        tiny_free(item);
        return result;
    }

    uint32_t planes = described.planes;

    Box ftyp = open_box(writer, "ftyp");

    put_bytes(writer, "avif", 4u);
    put32(writer, 0u);
    put_bytes(writer, "avif", 4u);
    put_bytes(writer, "mif1", 4u);
    put_bytes(writer, "miaf", 4u);

    close_box(&ftyp);

    Box meta = open_box(writer, "meta");
    put32(writer, 0u);

    Box hdlr = open_box(writer, "hdlr");
    put32(writer, 0u);
    put32(writer, 0u);
    put_bytes(writer, "pict", 4u);
    put32(writer, 0u);
    put32(writer, 0u);
    put32(writer, 0u);
    put8(writer, 0u);
    close_box(&hdlr);

    Box pitm = open_box(writer, "pitm");
    put32(writer, 0u);
    put16(writer, 1u);
    close_box(&pitm);

    /*
     * The item location, whose extent offset points into `mdat`.
     *
     * It is written before the payload exists, so the offset is patched once
     * the payload's position is known. Writing the payload first and the
     * metadata after would avoid that and put `mdat` before `meta`, which is
     * legal and which some readers handle less well.
     */
    Box iloc = open_box(writer, "iloc");
    put32(writer, 0u);
    put8(writer, 0x44u);
    put8(writer, 0u);
    put16(writer, 1u);
    put16(writer, 1u);
    put16(writer, 0u);
    put16(writer, 1u);

    size_t offset_at = writer->size;

    put32(writer, 0u);
    put32(writer, (uint32_t) item_size);
    close_box(&iloc);

    Box iinf = open_box(writer, "iinf");
    put32(writer, 0u);
    put16(writer, 1u);

    Box infe = open_box(writer, "infe");
    put32(writer, 2u << 24u);
    put16(writer, 1u);
    put16(writer, 0u);
    put_bytes(writer, "av01", 4u);
    put8(writer, 0u);
    close_box(&infe);

    close_box(&iinf);

    Box iprp = open_box(writer, "iprp");
    Box ipco = open_box(writer, "ipco");

    Box av1c = open_box(writer, "av1C");
    put8(writer, 0x81u);
    put8(writer, (uint8_t) ((uint32_t) described.profile << 5u | 15u));
    put8(
        writer, (uint8_t) ((planes == 1u ? 0x10u : 0u) |
                           (described.sub_x ? 0x08u : 0u) |
                           (described.sub_y ? 0x04u : 0u))
    );
    put8(writer, 0u);
    close_box(&av1c);

    Box ispe = open_box(writer, "ispe");
    put32(writer, 0u);
    put32(writer, image->width);
    put32(writer, image->height);
    close_box(&ispe);

    Box pixi = open_box(writer, "pixi");
    put32(writer, 0u);
    put8(writer, (uint8_t) planes);

    for (uint32_t i = 0; i < planes; i++) put8(writer, 8u);

    close_box(&pixi);
    close_box(&ipco);

    Box ipma = open_box(writer, "ipma");
    put32(writer, 0u);
    put32(writer, 1u);
    put16(writer, 1u);
    put8(writer, 3u);

    // the codec configuration is essential, which is what the high bit says;
    // the extents and the depth are descriptive
    put8(writer, 0x81u);
    put8(writer, 0x02u);
    put8(writer, 0x03u);

    close_box(&ipma);
    close_box(&iprp);
    close_box(&meta);

    Box mdat = open_box(writer, "mdat");
    size_t payload = writer->size;

    int written = put_bytes(writer, item, item_size);

    close_box(&mdat);
    tiny_free(item);

    if (written != TINYIMG_OK) return written;

    uint8_t* at = writer->data + offset_at;

    at[0] = (uint8_t) (payload >> 24);
    at[1] = (uint8_t) (payload >> 16);
    at[2] = (uint8_t) (payload >> 8);
    at[3] = (uint8_t) payload;

    return TINYIMG_OK;
}

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
    TINYIMG_FORMAT_AVIF, avif_sniff, avif_probe, avif_decode, avif_encode
};
