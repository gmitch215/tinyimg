#include "tinyimg/text.h"

#include "tinyimg/memory.h"
#include "tinyimg/util.h"

#pragma region readers

/**
 * @brief A bounds checked view over a face's bytes.
 *
 * Every read goes through this. A font is untrusted input and every offset in
 * one is a number in the file, so a read past the end returns zero rather than
 * reading whatever follows the blob in linear memory.
 */
typedef struct {
    const uint8_t* data;
    size_t size;
} Face;

static uint32_t face_u8(const Face* face, size_t at) {
    return at < face->size ? face->data[at] : 0u;
}

static uint32_t face_u16(const Face* face, size_t at) {
    if (at + 2u > face->size) return 0u;
    return ((uint32_t) face->data[at] << 8) | face->data[at + 1u];
}

static int32_t face_s16(const Face* face, size_t at) {
    return (int32_t) (int16_t) (uint16_t) face_u16(face, at);
}

static uint32_t face_u32(const Face* face, size_t at) {
    if (at + 4u > face->size) return 0u;
    return ((uint32_t) face->data[at] << 24) |
           ((uint32_t) face->data[at + 1u] << 16) |
           ((uint32_t) face->data[at + 2u] << 8) | face->data[at + 3u];
}

/** PSF is the one format here that is little-endian; sfnt is big throughout. */
/** A signed 32 bit big-endian read, for the 16.16 fixed point `fvar` uses. */
static int32_t face_s32(const Face* face, size_t at) {
    return (int32_t) face_u32(face, at);
}

static uint32_t face_u32le(const Face* face, size_t at) {
    if (at + 4u > face->size) return 0u;
    return (uint32_t) face->data[at] | ((uint32_t) face->data[at + 1u] << 8) |
           ((uint32_t) face->data[at + 2u] << 16) |
           ((uint32_t) face->data[at + 3u] << 24);
}

static int face_holds(const Face* face, size_t at, size_t length) {
    return at <= face->size && length <= face->size - at;
}

#pragma endregion

#pragma region utf8

/**
 * @brief Decodes one UTF-8 sequence.
 *
 * A malformed sequence yields U+FFFD and advances one byte, which is what
 * keeps a bad byte from swallowing the rest of the string. Overlong forms,
 * surrogates and anything past U+10FFFF are malformed.
 *
 * @param text The string.
 * @param at Byte position, advanced past what was read.
 * @return uint32_t The codepoint, or zero at the terminator.
 */
static uint32_t utf8_next(const char* text, size_t* at) {
    const uint8_t* bytes = (const uint8_t*) text;
    uint32_t lead = bytes[*at];

    if (lead == 0) return 0u;

    uint32_t extra;
    uint32_t code;
    uint32_t lowest;

    if (lead < 0x80u) {
        (*at)++;
        return lead;
    }
    else if ((lead & 0xE0u) == 0xC0u) {
        extra = 1u;
        code = lead & 0x1Fu;
        lowest = 0x80u;
    }
    else if ((lead & 0xF0u) == 0xE0u) {
        extra = 2u;
        code = lead & 0x0Fu;
        lowest = 0x800u;
    }
    else if ((lead & 0xF8u) == 0xF0u) {
        extra = 3u;
        code = lead & 0x07u;
        lowest = 0x10000u;
    }
    else {
        (*at)++;
        return 0xFFFDu;
    }

    for (uint32_t i = 1u; i <= extra; i++) {
        uint32_t byte = bytes[*at + i];
        if ((byte & 0xC0u) != 0x80u) {
            (*at)++;
            return 0xFFFDu;
        }
        code = (code << 6) | (byte & 0x3Fu);
    }

    *at += extra + 1u;

    if (code < lowest || code > 0x10FFFFu ||
        (code >= 0xD800u && code <= 0xDFFFu)) {
        return 0xFFFDu;
    }

    return code;
}

#pragma endregion

#pragma region truetype tables

#define TAG(a, b, c, d)                                                        \
    (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | \
     (uint32_t) (d))

/**
 * @brief Finds a table in the sfnt directory.
 *
 * @param face The bytes.
 * @param tag The four character tag.
 * @param length Receives the table length. May be NULL.
 * @return uint32_t The offset, or zero when the table is absent or its extent
 * falls outside the file.
 */
static uint32_t table_of(const Face* face, uint32_t tag, uint32_t* length) {
    uint32_t count = face_u16(face, 4);

    for (uint32_t i = 0; i < count; i++) {
        size_t entry = 12u + 16u * i;
        if (!face_holds(face, entry, 16u)) return 0u;
        if (face_u32(face, entry) != tag) continue;

        uint32_t offset = face_u32(face, entry + 8u);
        uint32_t extent = face_u32(face, entry + 12u);

        if (!face_holds(face, offset, extent)) return 0u;
        if (length) *length = extent;

        return offset;
    }

    return 0u;
}

/** Reads a `loca` entry, which is halved or not according to the format. */
static uint32_t loca_at(const TinyFont* font, uint32_t index) {
    Face face = {font->data, font->size};

    if (font->long_loca) {
        return face_u32(&face, font->loca + 4u * (size_t) index);
    }

    return 2u * face_u16(&face, font->loca + 2u * (size_t) index);
}

/**
 * @brief Maps a codepoint through a `cmap` format 4 subtable.
 *
 * The segmented format the BMP uses. The search is linear over the segments
 * rather than through the binary search fields the table carries, because those
 * fields are a number in the file and a wrong one would walk outside it; a
 * subsetted latin face has a handful of segments and a full one has a few
 * hundred.
 */
static uint32_t cmap4_lookup(
    const Face* face, uint32_t table, uint32_t codepoint
) {
    if (codepoint > 0xFFFFu) return 0u;

    uint32_t segments = face_u16(face, table + 6u) / 2u;
    if (segments == 0u) return 0u;

    uint32_t ends = table + 14u;
    uint32_t starts = ends + 2u * segments + 2u;
    uint32_t deltas = starts + 2u * segments;
    uint32_t ranges = deltas + 2u * segments;

    for (uint32_t i = 0; i < segments; i++) {
        if (codepoint > face_u16(face, ends + 2u * i)) continue;

        uint32_t start = face_u16(face, starts + 2u * i);
        if (codepoint < start) return 0u;

        uint32_t range = face_u16(face, ranges + 2u * i);

        if (range == 0u) {
            uint32_t delta = face_u16(face, deltas + 2u * i);
            return (codepoint + delta) & 0xFFFFu;
        }

        // the offset is from the range entry's own address, which is what makes
        // the glyph array shareable between segments
        uint32_t at = ranges + 2u * i + range + 2u * (codepoint - start);
        uint32_t glyph = face_u16(face, at);

        if (glyph == 0u) return 0u;
        return (glyph + face_u16(face, deltas + 2u * i)) & 0xFFFFu;
    }

    return 0u;
}

/** Maps a codepoint through a `cmap` format 12 subtable, which reaches past
 * the BMP. */
static uint32_t cmap12_lookup(
    const Face* face, uint32_t table, uint32_t codepoint
) {
    uint32_t groups = face_u32(face, table + 12u);
    uint32_t at = table + 16u;

    for (uint32_t i = 0; i < groups; i++, at += 12u) {
        uint32_t start = face_u32(face, at);
        if (codepoint < start) return 0u;
        if (codepoint > face_u32(face, at + 4u)) continue;

        return face_u32(face, at + 8u) + (codepoint - start);
    }

    return 0u;
}

/** Maps a codepoint through a `cmap` format 6 subtable, a dense range. */
static uint32_t cmap6_lookup(
    const Face* face, uint32_t table, uint32_t codepoint
) {
    uint32_t first = face_u16(face, table + 6u);
    uint32_t count = face_u16(face, table + 8u);

    if (codepoint < first || codepoint - first >= count) return 0u;
    return face_u16(face, table + 10u + 2u * (codepoint - first));
}

/**
 * @brief Picks the best `cmap` subtable and remembers where it is.
 *
 * Preference order is format 12 then format 4 then format 6, and within a
 * format a Unicode encoding over a Macintosh one. Format 12 first because it is
 * the only one that reaches past the BMP, and a face carrying both has the same
 * BMP mapping in each.
 */
static void cmap_select(TinyFont* font) {
    Face face = {font->data, font->size};
    uint32_t cmap = table_of(&face, TAG('c', 'm', 'a', 'p'), 0);

    font->cmap = 0u;
    if (cmap == 0u) return;

    uint32_t count = face_u16(&face, cmap + 2u);
    uint32_t best = 0u;
    uint32_t best_rank = 0u;

    for (uint32_t i = 0; i < count; i++) {
        size_t entry = cmap + 4u + 8u * (size_t) i;
        uint32_t platform = face_u16(&face, entry);
        uint32_t offset = cmap + face_u32(&face, entry + 4u);

        if (!face_holds(&face, offset, 4u)) continue;

        uint32_t format = face_u16(&face, offset);
        uint32_t unicode = platform == 0u || platform == 3u;
        uint32_t rank;

        if (format == 12u)
            rank = 6u;
        else if (format == 4u)
            rank = 4u;
        else if (format == 6u)
            rank = 2u;
        else
            continue;

        rank += unicode;

        if (rank > best_rank) {
            best_rank = rank;
            best = offset;
        }
    }

    font->cmap = best;
}

/** The glyph a codepoint maps to, or zero for .notdef. */
static uint32_t glyph_of(const TinyFont* font, uint32_t codepoint) {
    if (font->cmap == 0u) return 0u;

    Face face = {font->data, font->size};
    uint32_t glyph;

    switch (face_u16(&face, font->cmap)) {
        case 4u: glyph = cmap4_lookup(&face, font->cmap, codepoint); break;
        case 6u: glyph = cmap6_lookup(&face, font->cmap, codepoint); break;
        case 12u: glyph = cmap12_lookup(&face, font->cmap, codepoint); break;
        default: glyph = 0u; break;
    }

    return glyph < font->glyphs ? glyph : 0u;
}

/** A glyph's advance width in font units. */
static int32_t advance_of(const TinyFont* font, uint32_t glyph) {
    if (font->hmtx == 0u || font->hmetrics == 0u) return 0;

    Face face = {font->data, font->size};

    // the trailing entries are left side bearings only, so every glyph past the
    // last full entry advances by the same amount as that one
    uint32_t index = glyph < font->hmetrics ? glyph : font->hmetrics - 1u;
    return (int32_t) face_u16(&face, font->hmtx + 4u * (size_t) index);
}

/**
 * @brief The kern adjustment between two glyphs, in font units.
 *
 * Format 0 of the Microsoft `kern` table, which is the only one a face in the
 * wild is likely to carry alone. Anything else reports no adjustment rather
 * than guessing, and a face with no `kern` at all is the common case.
 */
/**
 * @brief Where a glyph sits in a coverage table, or -1.
 *
 * Both formats, because a face uses whichever is smaller and there is no
 * choosing: format 1 is a sorted glyph list and format 2 is sorted ranges with
 * a running index.
 */
static int32_t coverage_index(const Face* face, size_t at, uint32_t glyph) {
    uint32_t format = face_u16(face, at);

    if (format == 1u) {
        uint32_t count = face_u16(face, at + 2u);
        uint32_t low = 0u;
        uint32_t high = count;

        while (low < high) {
            uint32_t mid = low + (high - low) / 2u;
            uint32_t found = face_u16(face, at + 4u + 2u * (size_t) mid);

            if (found == glyph) return (int32_t) mid;
            if (found < glyph)
                low = mid + 1u;
            else
                high = mid;
        }

        return -1;
    }

    if (format != 2u) return -1;

    uint32_t ranges = face_u16(face, at + 2u);

    for (uint32_t i = 0; i < ranges; i++) {
        size_t entry = at + 4u + 6u * (size_t) i;
        uint32_t first = face_u16(face, entry);
        uint32_t last = face_u16(face, entry + 2u);

        if (glyph < first) return -1;
        if (glyph > last) continue;

        uint32_t start = face_u16(face, entry + 4u);
        return (int32_t) (start + glyph - first);
    }

    return -1;
}

/** Which class a glyph is in, from a class definition table. */
static uint32_t class_of(const Face* face, size_t at, uint32_t glyph) {
    uint32_t format = face_u16(face, at);

    if (format == 1u) {
        uint32_t first = face_u16(face, at + 2u);
        uint32_t count = face_u16(face, at + 4u);

        if (glyph < first || glyph >= first + count) return 0u;
        return face_u16(face, at + 6u + 2u * (size_t) (glyph - first));
    }

    if (format != 2u) return 0u;

    uint32_t ranges = face_u16(face, at + 2u);

    for (uint32_t i = 0; i < ranges; i++) {
        size_t entry = at + 4u + 6u * (size_t) i;

        if (glyph < face_u16(face, entry)) return 0u;
        if (glyph > face_u16(face, entry + 2u)) continue;

        return face_u16(face, entry + 4u);
    }

    return 0u;
}

/**
 * @brief The x advance a GPOS pair lookup gives two glyphs, in font units.
 *
 * Pair positioning only, and only the first glyph's x advance out of the six
 * values a value record can carry. That is what kerning is; the other five
 * place a mark or move a glyph off its own advance, and a text renderer that
 * applied them without the rest of the shaping pipeline would move glyphs to
 * positions nothing else agreed on.
 *
 * The value format is a bitmask and the fields it names are packed in a fixed
 * order, so the offset of the x advance depends on which of the two lower bits
 * are set. Reading it at a fixed offset works on the common face and puts a
 * placement value into the advance on the next one.
 */
static int32_t gpos_kern_of(
    const TinyFont* font, uint32_t left, uint32_t right
) {
    if (font->gpos_kern == 0u) return 0;

    Face face = {font->data, font->size};

    for (uint32_t i = 0; i < font->gpos_subtables; i++) {
        uint32_t offset =
            face_u16(&face, font->gpos_kern + 6u + 2u * (size_t) i);
        size_t at = font->gpos_kern + offset;

        uint32_t format = face_u16(&face, at);
        uint32_t coverage = face_u16(&face, at + 2u);
        uint32_t first_format = face_u16(&face, at + 4u);
        uint32_t second_format = face_u16(&face, at + 6u);

        int32_t index = coverage_index(&face, at + coverage, left);
        if (index < 0) continue;

        // the x advance is the first field of the record, so it is only at a
        // known offset when it is present
        if ((first_format & 0x0004u) == 0u) continue;

        uint32_t first_size = 0;
        uint32_t second_size = 0;

        for (uint32_t bit = 0; bit < 8u; bit++) {
            if ((first_format >> bit) & 1u) first_size += 2u;
            if ((second_format >> bit) & 1u) second_size += 2u;
        }

        if (format == 1u) {
            uint32_t sets = face_u16(&face, at + 8u);
            if ((uint32_t) index >= sets) continue;

            size_t set = at + face_u16(&face, at + 10u + 2u * (size_t) index);
            uint32_t pairs = face_u16(&face, set);
            uint32_t stride = 2u + first_size + second_size;

            for (uint32_t pair = 0; pair < pairs; pair++) {
                size_t entry = set + 2u + (size_t) stride * pair;

                if (face_u16(&face, entry) != right) continue;

                // the x advance is the first field the mask names, and the two
                // bits below it are placements
                uint32_t skip = 0;
                if (first_format & 0x0001u) skip += 2u;
                if (first_format & 0x0002u) skip += 2u;

                return face_s16(&face, entry + 2u + skip);
            }

            continue;
        }

        if (format != 2u) continue;

        // class zero is everything the class definition does not name, so a
        // glyph outside the coverage would otherwise pick up whatever value
        // sits at [0][0] instead of being left alone
        if (coverage_index(&face, at + coverage, left) < 0) continue;

        size_t first_classes = at + face_u16(&face, at + 8u);
        size_t second_classes = at + face_u16(&face, at + 10u);
        uint32_t first_count = face_u16(&face, at + 12u);
        uint32_t second_count = face_u16(&face, at + 14u);

        uint32_t a = class_of(&face, first_classes, left);
        uint32_t b = class_of(&face, second_classes, right);

        if (a >= first_count || b >= second_count) continue;

        uint32_t stride = first_size + second_size;
        size_t entry =
            at + 16u + (size_t) stride * ((size_t) a * second_count + b);

        uint32_t skip = 0;
        if (first_format & 0x0001u) skip += 2u;
        if (first_format & 0x0002u) skip += 2u;

        return face_s16(&face, entry + skip);
    }

    return 0;
}

/**
 * @brief The ligature glyph for a run starting at `first`, if the face has one.
 *
 * Longest match wins, because `ffi` and `fi` both start at the same glyph and a
 * face lists both; taking the first match would set `ffi` as `fi` followed by a
 * bare `i`.
 *
 * @param glyphs The glyph run, `count` entries, starting with the one to
 * substitute from.
 * @param consumed Receives how many of them the ligature replaces.
 * @return uint32_t The ligature glyph, or 0 when none applies.
 */
static uint32_t ligature_of(
    const TinyFont* font, const uint32_t* glyphs, uint32_t count,
    uint32_t* consumed
) {
    *consumed = 0u;

    if (font->gsub_liga == 0u || count < 2u) return 0u;

    Face face = {font->data, font->size};
    uint32_t best = 0u;
    uint32_t best_length = 0u;

    for (uint32_t i = 0; i < font->gsub_subtables; i++) {
        uint32_t offset =
            face_u16(&face, font->gsub_liga + 6u + 2u * (size_t) i);
        size_t at = font->gsub_liga + offset;

        if (face_u16(&face, at) != 1u) continue;

        uint32_t coverage = face_u16(&face, at + 2u);
        int32_t index = coverage_index(&face, at + coverage, glyphs[0]);

        if (index < 0) continue;

        uint32_t sets = face_u16(&face, at + 4u);
        if ((uint32_t) index >= sets) continue;

        size_t set = at + face_u16(&face, at + 6u + 2u * (size_t) index);
        uint32_t ligatures = face_u16(&face, set);

        for (uint32_t j = 0; j < ligatures; j++) {
            size_t entry = set + face_u16(&face, set + 2u + 2u * (size_t) j);
            uint32_t glyph = face_u16(&face, entry);
            uint32_t components = face_u16(&face, entry + 2u);

            if (components < 2u || components > count) continue;
            if (components <= best_length) continue;

            int matched = 1;

            // the first component is the coverage glyph, so the list holds the
            // rest
            for (uint32_t k = 1; k < components; k++) {
                if (face_u16(&face, entry + 2u + 2u * (size_t) k) !=
                    glyphs[k]) {
                    matched = 0;
                    break;
                }
            }

            if (!matched) continue;

            best = glyph;
            best_length = components;
        }
    }

    *consumed = best_length;
    return best;
}

static int32_t kern_of(const TinyFont* font, uint32_t left, uint32_t right) {
    // GPOS first, because a face that ships both means the GPOS one and keeps
    // the legacy table for readers that predate it
    int32_t modern = gpos_kern_of(font, left, right);
    if (modern != 0) return modern;

    if (font->kern == 0u) return 0;

    Face face = {font->data, font->size};
    uint32_t tables = face_u16(&face, font->kern + 2u);

    size_t at = font->kern + 4u;
    uint32_t want = (left << 16) | right;

    for (uint32_t i = 0; i < tables; i++) {
        uint32_t length = face_u16(&face, at + 2u);
        uint32_t coverage = face_u16(&face, at + 4u);

        if (length < 14u) return 0;

        if ((coverage >> 8) == 0u && (coverage & 1u) != 0u) {
            uint32_t pairs = face_u16(&face, at + 6u);
            size_t first = at + 14u;

            uint32_t low = 0u;
            uint32_t high = pairs;

            while (low < high) {
                uint32_t mid = low + (high - low) / 2u;
                uint32_t key = face_u32(&face, first + 6u * (size_t) mid);

                if (key == want) {
                    return face_s16(&face, first + 6u * (size_t) mid + 4u);
                }
                if (key < want)
                    low = mid + 1u;
                else
                    high = mid;
            }
        }

        at += length;
    }

    return 0;
}

#pragma endregion

#pragma region variations

/** A 2.14 fixed point value, which is how a tuple coordinate is stored. */
static float f2dot14(const Face* face, size_t at) {
    return (float) face_s16(face, at) / 16384.0f;
}

/**
 * @brief Maps a normalized coordinate through the face's `avar` segments.
 *
 * A face ships this when its axis does not respond evenly: a weight axis whose
 * midpoint should be 500 rather than the 550 that even normalization gives has
 * a segment map saying so. Skipping it interpolates to the wrong instance
 * everywhere except the ends, which is the kind of wrong that looks right.
 */
static float avar_map(const TinyFont* font, uint32_t axis, float value) {
    if (font->avar == 0u) return value;

    Face face = {font->data, font->size};
    size_t at = font->avar + 8u;
    uint32_t count = face_u16(&face, font->avar + 6u);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t pairs = face_u16(&face, at);
        at += 2u;

        if (i != axis) {
            at += 4u * (size_t) pairs;
            continue;
        }

        // the pairs are sorted, so the segment the value falls in is the first
        // one whose `to` is past it
        float from_low = -1.0f;
        float to_low = -1.0f;

        for (uint32_t pair = 0; pair < pairs; pair++) {
            float from = f2dot14(&face, at + 4u * (size_t) pair);
            float to = f2dot14(&face, at + 4u * (size_t) pair + 2u);

            if (value == from) return to;

            if (value < from) {
                if (from == from_low) return to_low;
                return to_low +
                       (to - to_low) * (value - from_low) / (from - from_low);
            }

            from_low = from;
            to_low = to;
        }

        return to_low;
    }

    return value;
}

/** Where a value on one axis sits in the -1 to 1 space `gvar` uses. */
static float normalize_axis(
    const TinyFont* font, uint32_t index, const TinyFontAxis* axis, float value
) {
    if (value < axis->min) value = axis->min;
    if (value > axis->max) value = axis->max;

    float normalized = 0.0f;

    if (value < axis->def) {
        if (axis->def > axis->min) {
            normalized = (value - axis->def) / (axis->def - axis->min);
        }
    }
    else if (value > axis->def) {
        if (axis->max > axis->def) {
            normalized = (value - axis->def) / (axis->max - axis->def);
        }
    }

    return avar_map(font, index, normalized);
}

/**
 * @brief How much one variation region applies at the face's coordinates.
 *
 * The product over the axes of each one's own factor, which is what makes a
 * region a box rather than a set of independent axes: a coordinate outside any
 * one axis' span takes the whole region to zero.
 *
 * A region with no intermediate values is the same formula with the span taken
 * from the peak and zero, so there is one code path rather than two.
 */
static float region_scalar(
    const TinyFont* font, const Face* face, size_t peak_at, size_t start_at,
    size_t end_at, uint32_t axes
) {
    float scalar = 1.0f;

    for (uint32_t axis = 0; axis < axes; axis++) {
        float peak = f2dot14(face, peak_at + 2u * (size_t) axis);

        // an axis the region does not mention applies everywhere on it
        if (peak == 0.0f) continue;

        float coord = axis < TINYIMG_FONT_MAX_AXES ? font->coords[axis] : 0.0f;

        if (coord == 0.0f) return 0.0f;
        if (coord == peak) continue;

        float start;
        float end;

        if (start_at != 0u) {
            start = f2dot14(face, start_at + 2u * (size_t) axis);
            end = f2dot14(face, end_at + 2u * (size_t) axis);
        }
        else {
            start = peak < 0.0f ? peak : 0.0f;
            end = peak < 0.0f ? 0.0f : peak;
        }

        if (coord <= start || coord >= end) return 0.0f;

        if (coord < peak) {
            if (peak == start) return 0.0f;
            scalar *= (coord - start) / (peak - start);
        }
        else {
            if (end == peak) return 0.0f;
            scalar *= (end - coord) / (end - peak);
        }
    }

    return scalar;
}

/**
 * @brief Reads a packed point number list.
 *
 * A count of zero means every point, which is the common case and is why the
 * caller checks for it rather than this expanding it into a list.
 *
 * @return uint32_t Numbers read, or 0 for "all points".
 */
static uint32_t read_point_numbers(
    const Face* face, size_t* at, uint16_t* out, uint32_t capacity
) {
    uint32_t count = face_u8(face, *at);
    *at += 1u;

    if (count & 0x80u) {
        count = ((count & 0x7Fu) << 8) | face_u8(face, *at);
        *at += 1u;
    }

    if (count == 0u) return 0u;

    uint32_t read = 0;
    uint32_t value = 0;

    while (read < count) {
        uint32_t control = face_u8(face, *at);
        *at += 1u;

        uint32_t run = (control & 0x7Fu) + 1u;
        int words = (control & 0x80u) != 0;

        for (uint32_t i = 0; i < run && read < count; i++) {
            if (words) {
                value += face_u16(face, *at);
                *at += 2u;
            }
            else {
                value += face_u8(face, *at);
                *at += 1u;
            }

            if (read < capacity) out[read] = (uint16_t) value;
            read++;
        }
    }

    return read;
}

/** Reads a packed delta run into `out`, which holds `count` entries. */
static void read_packed_deltas(
    const Face* face, size_t* at, int32_t* out, uint32_t count
) {
    uint32_t read = 0;

    while (read < count) {
        uint32_t control = face_u8(face, *at);
        *at += 1u;

        uint32_t run = (control & 0x3Fu) + 1u;

        if (control & 0x80u) {
            for (uint32_t i = 0; i < run && read < count; i++) {
                out[read++] = 0;
            }
            continue;
        }

        int words = (control & 0x40u) != 0;

        for (uint32_t i = 0; i < run && read < count; i++) {
            if (words) {
                out[read++] = face_s16(face, *at);
                *at += 2u;
            }
            else {
                out[read++] = (int8_t) face_u8(face, *at);
                *at += 1u;
            }
        }
    }
}

/**
 * @brief Infers the deltas of points no region mentioned.
 *
 * The specification calls this IUP. A point between two that did move follows
 * them proportionally on each axis, and one outside the pair's span shifts by
 * the nearer one. Without it, a tuple that names four points moves four points
 * and leaves the outline between them where it was, which tears the glyph.
 *
 * Runs per contour, because the neighbors are the ones along the same closed
 * curve.
 */
static void infer_deltas(
    const int32_t* base, float* deltas, const uint8_t* touched, uint32_t first,
    uint32_t last
) {
    uint32_t n = last - first + 1u;
    uint32_t moved = 0;

    for (uint32_t i = first; i <= last; i++) {
        if (touched[i]) moved++;
    }

    if (moved == 0u) return;

    if (moved == n) return;

    if (moved == 1u) {
        // one reference moves the whole contour with it
        for (uint32_t i = first; i <= last; i++) {
            if (touched[i]) {
                float only = deltas[i];

                for (uint32_t j = first; j <= last; j++) deltas[j] = only;
                return;
            }
        }
    }

    for (uint32_t i = first; i <= last; i++) {
        if (touched[i]) continue;

        // the referenced points either side, wrapping around the contour
        uint32_t before = i;
        uint32_t after = i;

        for (uint32_t step = 0; step < n; step++) {
            before = before == first ? last : before - 1u;
            if (touched[before]) break;
        }

        for (uint32_t step = 0; step < n; step++) {
            after = after == last ? first : after + 1u;
            if (touched[after]) break;
        }

        int32_t low = base[before];
        int32_t high = base[after];
        float low_delta = deltas[before];
        float high_delta = deltas[after];
        int32_t here = base[i];

        if (low > high) {
            int32_t swap = low;
            low = high;
            high = swap;

            float swap_delta = low_delta;
            low_delta = high_delta;
            high_delta = swap_delta;
        }

        if (here <= low) {
            deltas[i] = low_delta;
        }
        else if (here >= high) {
            deltas[i] = high_delta;
        }
        else if (high == low) {
            deltas[i] = low_delta;
        }
        else {
            float t = (float) (here - low) / (float) (high - low);
            deltas[i] = low_delta + t * (high_delta - low_delta);
        }
    }
}

/**
 * @brief Applies a glyph's `gvar` deltas at the face's coordinates.
 *
 * `xs` and `ys` hold `points` real points followed by four phantom ones, which
 * is the layout `gvar` addresses: the first two carry the advance, so varying
 * them is what makes a bolder instance spaced the way its designer set it.
 *
 * @param glyph Which glyph.
 * @param xs Coordinates, changed in place.
 * @param ys Coordinates, changed in place.
 * @param ends One past the last point of each contour, `contours` entries.
 * @param points Real points plus the four phantom ones.
 */
static void gvar_apply(
    const TinyFont* font, uint32_t glyph, int32_t* xs, int32_t* ys,
    const uint32_t* ends, uint32_t contours, uint32_t points
) {
    if (font->gvar == 0u || !font->varied || points == 0u) return;

    Face face = {font->data, font->size};

    uint32_t axes = face_u16(&face, font->gvar + 4u);
    uint32_t shared_count = face_u16(&face, font->gvar + 6u);
    uint32_t shared_at = font->gvar + face_u32(&face, font->gvar + 8u);
    uint32_t glyphs = face_u16(&face, font->gvar + 12u);
    uint32_t flags = face_u16(&face, font->gvar + 14u);
    uint32_t data_at = font->gvar + face_u32(&face, font->gvar + 16u);

    if (axes == 0u || glyph >= glyphs) return;
    (void) shared_count;

    size_t table = font->gvar + 20u;
    uint32_t from;
    uint32_t to;

    if (flags & 1u) {
        from = face_u32(&face, table + 4u * (size_t) glyph);
        to = face_u32(&face, table + 4u * (size_t) glyph + 4u);
    }
    else {
        from = 2u * face_u16(&face, table + 2u * (size_t) glyph);
        to = 2u * face_u16(&face, table + 2u * (size_t) glyph + 2u);
    }

    if (to <= from) return;

    size_t glyph_at = data_at + from;
    uint32_t tuples = face_u16(&face, glyph_at);
    size_t serial = glyph_at + face_u16(&face, glyph_at + 2u);
    int shared_points = (tuples & 0x8000u) != 0;

    tuples &= 0x0FFFu;
    if (tuples == 0u) return;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    // one block: two float delta arrays, the touched map, the point numbers and
    // the two raw delta runs
    float* dx = (float*) tiny_arena_alloc((size_t) points * sizeof(float), 4);
    float* dy = (float*) tiny_arena_alloc((size_t) points * sizeof(float), 4);
    uint8_t* touched = (uint8_t*) tiny_arena_alloc(points, 1);
    uint16_t* numbers =
        (uint16_t*) tiny_arena_alloc((size_t) points * sizeof(uint16_t), 2);
    uint16_t* shared =
        (uint16_t*) tiny_arena_alloc((size_t) points * sizeof(uint16_t), 2);
    int32_t* raw_x =
        (int32_t*) tiny_arena_alloc((size_t) points * sizeof(int32_t), 4);
    int32_t* raw_y =
        (int32_t*) tiny_arena_alloc((size_t) points * sizeof(int32_t), 4);
    float* total_x =
        (float*) tiny_arena_alloc((size_t) points * sizeof(float), 4);
    float* total_y =
        (float*) tiny_arena_alloc((size_t) points * sizeof(float), 4);

    if (!dx || !dy || !touched || !numbers || !shared || !raw_x || !raw_y ||
        !total_x || !total_y) {
        tiny_arena_release(&mark);
        return;
    }

    for (uint32_t i = 0; i < points; i++) {
        total_x[i] = 0.0f;
        total_y[i] = 0.0f;
    }

    uint32_t shared_read = 0;

    if (shared_points) {
        size_t walk = serial;
        shared_read = read_point_numbers(&face, &walk, shared, points);
        serial = walk;
    }

    size_t header = glyph_at + 4u;

    for (uint32_t tuple = 0; tuple < tuples; tuple++) {
        uint32_t size = face_u16(&face, header);
        uint32_t index = face_u16(&face, header + 2u);
        size_t at = header + 4u;

        size_t peak_at;
        size_t start_at = 0u;
        size_t end_at = 0u;

        if (index & 0x8000u) {
            peak_at = at;
            at += 2u * (size_t) axes;
        }
        else {
            peak_at = shared_at + 2u * (size_t) axes * (index & 0x0FFFu);
        }

        if (index & 0x4000u) {
            start_at = at;
            at += 2u * (size_t) axes;
            end_at = at;
            at += 2u * (size_t) axes;
        }

        header = at;

        float scalar =
            region_scalar(font, &face, peak_at, start_at, end_at, axes);

        size_t walk = serial;
        serial += size;

        if (scalar == 0.0f) continue;

        uint32_t count = shared_read;
        const uint16_t* which = shared;

        if (index & 0x2000u) {
            count = read_point_numbers(&face, &walk, numbers, points);
            which = numbers;
        }

        uint32_t deltas = count == 0u ? points : count;
        if (deltas > points) deltas = points;

        read_packed_deltas(&face, &walk, raw_x, deltas);
        read_packed_deltas(&face, &walk, raw_y, deltas);

        if (count == 0u) {
            for (uint32_t i = 0; i < deltas; i++) {
                total_x[i] += scalar * (float) raw_x[i];
                total_y[i] += scalar * (float) raw_y[i];
            }

            continue;
        }

        // a tuple that names a subset moves those points and infers the rest,
        // per contour, before it is added to the running total
        for (uint32_t i = 0; i < points; i++) {
            dx[i] = 0.0f;
            dy[i] = 0.0f;
            touched[i] = 0u;
        }

        for (uint32_t i = 0; i < deltas; i++) {
            uint32_t point = which[i];
            if (point >= points) continue;

            dx[point] = (float) raw_x[i];
            dy[point] = (float) raw_y[i];
            touched[point] = 1u;
        }

        uint32_t first = 0;

        for (uint32_t contour = 0; contour < contours; contour++) {
            uint32_t last = ends[contour];
            if (last >= points) break;

            infer_deltas(xs, dx, touched, first, last);
            infer_deltas(ys, dy, touched, first, last);

            first = last + 1u;
        }

        // the four phantom points are their own group, and there is nothing to
        // interpolate them between
        for (uint32_t i = 0; i < points; i++) {
            total_x[i] += scalar * dx[i];
            total_y[i] += scalar * dy[i];
        }
    }

    for (uint32_t i = 0; i < points; i++) {
        xs[i] += (int32_t) tiny_roundf(total_x[i]);
        ys[i] += (int32_t) tiny_roundf(total_y[i]);
    }

    tiny_arena_release(&mark);
}

TINYIMG_EXPORT("tiny_font_axis_count")
uint32_t tiny_font_axis_count(const TinyFont* font) {
    return font ? font->axes : 0u;
}

TINYIMG_EXPORT("tiny_font_axis")
int tiny_font_axis(const TinyFont* font, uint32_t index, TinyFontAxis* out) {
    if (!font || !out) return TINYIMG_ERR_NULL;
    if (index >= font->axes) return TINYIMG_ERR_BOUNDS;

    Face face = {font->data, font->size};

    // the header is major, minor, the axes offset, a reserved word, the count
    // and then the record size; the count and the size are adjacent and easy to
    // read the wrong way round
    uint32_t size = face_u16(&face, font->fvar + 10u);
    size_t at =
        font->fvar + face_u16(&face, font->fvar + 4u) + (size_t) size * index;

    out->tag = face_u32(&face, at);
    out->min = (float) face_s32(&face, at + 4u) / 65536.0f;
    out->def = (float) face_s32(&face, at + 8u) / 65536.0f;
    out->max = (float) face_s32(&face, at + 12u) / 65536.0f;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_font_set_axis")
int tiny_font_set_axis(TinyFont* font, uint32_t tag, float value) {
    if (!font) return TINYIMG_ERR_NULL;
    if (font->fvar == 0u || font->axes == 0u) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }
    if (!tiny_finite(value)) return TINYIMG_ERR_RANGE;

    for (uint32_t i = 0; i < font->axes; i++) {
        TinyFontAxis axis;

        if (tiny_font_axis(font, i, &axis) != TINYIMG_OK) break;
        if (axis.tag != tag) continue;

        font->coords[i] = normalize_axis(font, i, &axis, value);
        font->varied = 0u;

        for (uint32_t j = 0; j < font->axes; j++) {
            if (font->coords[j] != 0.0f) font->varied = 1u;
        }

        return TINYIMG_OK;
    }

    return TINYIMG_ERR_NOT_FOUND;
}

TINYIMG_EXPORT("tiny_font_reset_axes")
int tiny_font_reset_axes(TinyFont* font) {
    if (!font) return TINYIMG_ERR_NULL;

    for (uint32_t i = 0; i < TINYIMG_FONT_MAX_AXES; i++) {
        font->coords[i] = 0.0f;
    }

    font->varied = 0u;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region outlines

/** One flattened outline segment, in pixel space with y increasing downward. */
typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} Edge;

/**
 * @brief A fixed edge buffer over arena scratch.
 *
 * Allocated once for a whole run and rewound between glyphs, because a fresh
 * one per glyph was the entire cost of drawing: it exceeds the arena's chunk
 * size, so every glyph took a chunk from the heap and gave it back, and
 * hoisting it took a hundred characters from 0.98 ms to 0.20.
 *
 * A glyph that needs more edges than fit is reported rather than truncated. A
 * dropped edge leaves an outline open, which fills as a wedge across the glyph
 * and looks like a rasterizer fault rather than a buffer that ran out.
 */
typedef struct {
    Edge* edges;
    uint32_t count;
    uint32_t capacity;
    uint8_t overflowed;
} EdgeList;

/**
 * @brief A 2x3 affine in font units, applied to a component's points.
 *
 * Composite glyphs nest and each level may scale, so the transform is carried
 * down the recursion rather than applied at the leaves.
 */
typedef struct {
    float a;
    float b;
    float c;
    float d;
    float e;
    float f;
} Affine;

static void affine_identity(Affine* out) {
    out->a = 1.0f;
    out->b = 0.0f;
    out->c = 0.0f;
    out->d = 1.0f;
    out->e = 0.0f;
    out->f = 0.0f;
}

/** `outer` applied after `inner`. */
static void affine_compose(
    const Affine* outer, const Affine* inner, Affine* out
) {
    out->a = outer->a * inner->a + outer->c * inner->b;
    out->b = outer->b * inner->a + outer->d * inner->b;
    out->c = outer->a * inner->c + outer->c * inner->d;
    out->d = outer->b * inner->c + outer->d * inner->d;
    out->e = outer->a * inner->e + outer->c * inner->f + outer->e;
    out->f = outer->b * inner->e + outer->d * inner->f + outer->f;
}

/** A point in pixel space. */
typedef struct {
    float x;
    float y;
} Point;

/**
 * @brief What turns a font unit into a pixel on the image.
 *
 * Two transforms in one: the glyph's own composite transform in font units,
 * then the em scale and the pen position. Kept together so a point is
 * transformed once.
 */
typedef struct {
    Affine units;
    float scale;
    float pen_x;
    float baseline;
} Placement;

static Point place(const Placement* at, float x, float y) {
    float ux = at->units.a * x + at->units.c * y + at->units.e;
    float uy = at->units.b * x + at->units.d * y + at->units.f;

    Point out;
    out.x = at->pen_x + ux * at->scale;
    // font units run up from the baseline and image rows run down
    out.y = at->baseline - uy * at->scale;

    return out;
}

static void edge_add(EdgeList* list, Point from, Point to) {
    if (from.y == to.y) return;

    if (list->count >= list->capacity) {
        list->overflowed = 1u;
        return;
    }

    Edge* edge = &list->edges[list->count++];
    edge->x0 = from.x;
    edge->y0 = from.y;
    edge->x1 = to.x;
    edge->y1 = to.y;
}

/**
 * @brief Flattens one quadratic into line segments.
 *
 * The step count comes from the curve's own deviation from its chord, which for
 * a quadratic is `|p0 - 2c + p1| / 8n^2` after n uniform steps. Solving that
 * for a tenth of a pixel is what keeps a large glyph smooth without spending
 * segments on a small one.
 */
static void edge_quad(EdgeList* list, Point from, Point control, Point to) {
    float dx = from.x - 2.0f * control.x + to.x;
    float dy = from.y - 2.0f * control.y + to.y;
    float deviation = tiny_sqrtf(dx * dx + dy * dy);

    int32_t steps = (int32_t) tiny_sqrtf(deviation / 0.8f) + 1;
    steps = tiny_clampi(steps, 1, 32);

    Point previous = from;

    for (int32_t i = 1; i <= steps; i++) {
        float t = (float) i / (float) steps;
        float u = 1.0f - t;

        Point next;
        next.x = u * u * from.x + 2.0f * u * t * control.x + t * t * to.x;
        next.y = u * u * from.y + 2.0f * u * t * control.y + t * t * to.y;

        edge_add(list, previous, next);
        previous = next;
    }
}

/** How deep a composite glyph may nest before it is called corrupt. */
#define GLYPH_DEPTH 5

/**
 * @brief Edges one glyph's flattened outline may take.
 *
 * 32 KiB of scratch, taken once per run rather than once per glyph. Half the
 * arena's chunk, and that is the reason for the number rather than any property
 * of a glyph: a buffer that fills a chunk leaves the per-glyph scratch nothing
 * to bump into, so each of those allocations takes a chunk of its own from the
 * heap and gives it back. Nine heap allocations per glyph was two thirds of the
 * cost of drawing.
 *
 * A latin glyph at a readable size needs a few hundred edges. A glyph with
 * hundreds of points drawn at hundreds of pixels reaches the ceiling, and
 * drawing one reports TINYIMG_ERR_MEMORY rather than filling a broken outline.
 */
#define TEXT_MAX_EDGES 2048u

static int glyph_edges(
    const TinyFont* font, uint32_t glyph, const Placement* at, EdgeList* list,
    uint32_t depth
);

/**
 * @brief Reads a simple glyph's contours and flattens them.
 *
 * The coordinate arrays are delta encoded behind a run length encoded flag
 * array, so the three have to be walked in order and the points held. They go
 * in arena scratch, released by the caller's mark.
 */
static int simple_glyph(
    const TinyFont* font, uint32_t glyph, size_t glyph_at, uint32_t contours,
    const Placement* at, EdgeList* list
) {
    Face face = {font->data, font->size};
    size_t ends = glyph_at + 10u;
    uint32_t points =
        face_u16(&face, ends + 2u * (size_t) (contours - 1u)) + 1u;

    if (points == 0u || points > 10000u) return TINYIMG_ERR_CORRUPT;

    size_t at_flags = ends + 2u * (size_t) contours + 2u +
                      face_u16(&face, ends + 2u * (size_t) contours);

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    /*
     * The point arrays carry four more entries than the glyph has points.
     *
     * Those are `gvar`'s phantom points, which is where the advance deltas
     * live; they are read and applied and then not drawn, because the outline
     * ends at the real points.
     */
    uint32_t stored = points + 4u;

    // one block, the two word arrays first so the byte array cannot misalign
    // them, and the contour ends word-aligned after the flags
    size_t padding = ((size_t) stored * 9u + 3u) & ~(size_t) 3u;
    padding -= (size_t) stored * 9u;

    uint8_t* block = (uint8_t*) tiny_arena_alloc(
        (size_t) stored * 9u + padding + (size_t) contours * 4u, 4
    );

    if (!block) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    int32_t* xs = (int32_t*) (void*) block;
    int32_t* ys = (int32_t*) (void*) (block + (size_t) stored * 4u);
    uint8_t* flags = block + (size_t) stored * 8u;
    uint32_t* contour_ends =
        (uint32_t*) (void*) (block + (size_t) stored * 9u + padding);

    size_t walk = at_flags;

    for (uint32_t i = 0; i < points;) {
        uint8_t flag = (uint8_t) face_u8(&face, walk++);
        flags[i++] = flag;

        if ((flag & 8u) == 0u) continue;

        uint32_t repeat = face_u8(&face, walk++);
        while (repeat-- > 0u && i < points) flags[i++] = flag;
    }

    int32_t value = 0;

    for (uint32_t i = 0; i < points; i++) {
        uint8_t flag = flags[i];

        if (flag & 2u) {
            int32_t delta = (int32_t) face_u8(&face, walk++);
            value += (flag & 16u) ? delta : -delta;
        }
        else if ((flag & 16u) == 0u) {
            value += face_s16(&face, walk);
            walk += 2u;
        }

        xs[i] = value;
    }

    value = 0;

    for (uint32_t i = 0; i < points; i++) {
        uint8_t flag = flags[i];

        if (flag & 4u) {
            int32_t delta = (int32_t) face_u8(&face, walk++);
            value += (flag & 32u) ? delta : -delta;
        }
        else if ((flag & 32u) == 0u) {
            value += face_s16(&face, walk);
            walk += 2u;
        }

        ys[i] = value;
    }

    /*
     * The phantom points, at their default positions.
     *
     * The first two are the left side bearing point and the advance point, so
     * the deltas `gvar` gives them are the advance's. `advance_of` computes the
     * same two on its own for layout; here they are read so the deltas that
     * name them do not land on a real point.
     */
    for (uint32_t i = points; i < stored; i++) {
        xs[i] = 0;
        ys[i] = 0;
    }

    if (points < stored) {
        xs[points + 1u] = (int32_t) advance_of(font, glyph);
    }

    for (uint32_t contour = 0; contour < contours; contour++) {
        contour_ends[contour] = face_u16(&face, ends + 2u * (size_t) contour);
    }

    gvar_apply(font, glyph, xs, ys, contour_ends, contours, stored);

    uint32_t first = 0;

    for (uint32_t contour = 0; contour < contours; contour++) {
        uint32_t last = face_u16(&face, ends + 2u * (size_t) contour);
        if (last >= points) break;

        uint32_t n = last - first + 1u;
        if (n < 2u) {
            first = last + 1u;
            continue;
        }

        // a contour may open on an off-curve point, in which case the start is
        // the implied midpoint before it, or the last point when that one is
        // on-curve
        Point start;
        uint32_t index;

        if (flags[first] & 1u) {
            start = place(at, (float) xs[first], (float) ys[first]);
            index = 1u;
        }
        else if (flags[last] & 1u) {
            start = place(at, (float) xs[last], (float) ys[last]);
            index = 0u;
        }
        else {
            start = place(
                at, ((float) xs[first] + (float) xs[last]) * 0.5f,
                ((float) ys[first] + (float) ys[last]) * 0.5f
            );
            index = 0u;
        }

        Point cursor = start;

        while (index < n) {
            uint32_t i = first + (index % n);

            if (flags[i] & 1u) {
                Point to = place(at, (float) xs[i], (float) ys[i]);
                edge_add(list, cursor, to);
                cursor = to;
                index++;
                continue;
            }

            Point control = place(at, (float) xs[i], (float) ys[i]);
            uint32_t j = first + ((index + 1u) % n);
            Point to;

            if (index + 1u < n && (flags[j] & 1u) == 0u) {
                // two off-curve points in a row imply an on-curve midpoint
                to = place(
                    at, ((float) xs[i] + (float) xs[j]) * 0.5f,
                    ((float) ys[i] + (float) ys[j]) * 0.5f
                );
                index++;
            }
            else if (index + 1u < n) {
                to = place(at, (float) xs[j], (float) ys[j]);
                index += 2u;
            }
            else {
                to = start;
                index++;
            }

            edge_quad(list, cursor, control, to);
            cursor = to;
        }

        edge_add(list, cursor, start);
        first = last + 1u;
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

/** Reads a composite glyph's components and recurses into each. */
static int composite_glyph(
    const TinyFont* font, size_t glyph_at, const Placement* at, EdgeList* list,
    uint32_t depth
) {
    Face face = {font->data, font->size};
    size_t walk = glyph_at + 10u;

    for (;;) {
        uint32_t flags = face_u16(&face, walk);
        uint32_t component = face_u16(&face, walk + 2u);
        walk += 4u;

        float dx;
        float dy;

        if (flags & 1u) {
            dx = (float) face_s16(&face, walk);
            dy = (float) face_s16(&face, walk + 2u);
            walk += 4u;
        }
        else {
            dx = (float) (int8_t) face_u8(&face, walk);
            dy = (float) (int8_t) face_u8(&face, walk + 1u);
            walk += 2u;
        }

        Affine unit;
        affine_identity(&unit);

        // F2Dot14 throughout: a signed 16 bit fixed point with two integer bits
        if (flags & 8u) {
            unit.a = unit.d = (float) face_s16(&face, walk) / 16384.0f;
            walk += 2u;
        }
        else if (flags & 0x40u) {
            unit.a = (float) face_s16(&face, walk) / 16384.0f;
            unit.d = (float) face_s16(&face, walk + 2u) / 16384.0f;
            walk += 4u;
        }
        else if (flags & 0x80u) {
            unit.a = (float) face_s16(&face, walk) / 16384.0f;
            unit.b = (float) face_s16(&face, walk + 2u) / 16384.0f;
            unit.c = (float) face_s16(&face, walk + 4u) / 16384.0f;
            unit.d = (float) face_s16(&face, walk + 6u) / 16384.0f;
            walk += 8u;
        }

        // ARGS_ARE_XY_VALUES clear means the arguments are point indices to
        // match rather than an offset, which needs the parent's points; a face
        // using it is rare enough that placing the component at the origin is
        // the honest answer
        if (flags & 2u) {
            unit.e = dx;
            unit.f = dy;
        }

        Placement inner = *at;
        affine_compose(&at->units, &unit, &inner.units);

        int result = glyph_edges(font, component, &inner, list, depth + 1u);
        if (result != TINYIMG_OK) return result;

        if ((flags & 0x20u) == 0u) break;
        if (walk >= font->size) break;
    }

    return TINYIMG_OK;
}

/**
 * @brief What the face's axis coordinates add to a glyph's advance.
 *
 * `gvar`'s first two phantom points are the left side bearing point and the
 * advance point, so the difference of their deltas is the advance's. Without
 * this a bolder instance draws bolder shapes at the regular instance's
 * spacing, which crowds every letter.
 *
 * Only reached when the face is away from its default, so a static face pays
 * nothing and a varied one pays one arena block and one `gvar` walk per glyph
 * measured.
 */
static int32_t advance_delta(const TinyFont* font, uint32_t glyph) {
    if (font->gvar == 0u || !font->varied) return 0;
    if (glyph >= font->glyphs) return 0;

    uint32_t from = loca_at(font, glyph);
    uint32_t to = loca_at(font, glyph + 1u);

    Face face = {font->data, font->size};

    uint32_t contours = 0;
    uint32_t points = 0;

    // an empty glyph still has its four phantom points, and a space's advance
    // varies like anything else
    if (to > from && to - from >= 10u && to <= font->glyf_size) {
        int32_t declared = face_s16(&face, font->glyf + from);

        if (declared > 0) {
            contours = (uint32_t) declared;
            points = face_u16(
                         &face,
                         font->glyf + from + 10u + 2u * (size_t) (contours - 1u)
                     ) +
                     1u;
        }
    }

    if (points > 10000u) return 0;

    uint32_t stored = points + 4u;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    int32_t* xs =
        (int32_t*) tiny_arena_alloc((size_t) stored * sizeof(int32_t), 4);
    int32_t* ys =
        (int32_t*) tiny_arena_alloc((size_t) stored * sizeof(int32_t), 4);
    uint32_t* ends = (uint32_t*) tiny_arena_alloc(
        (size_t) (contours + 1u) * sizeof(uint32_t), 4
    );

    if (!xs || !ys || !ends) {
        tiny_arena_release(&mark);
        return 0;
    }

    for (uint32_t i = 0; i < stored; i++) {
        xs[i] = 0;
        ys[i] = 0;
    }

    // the real points are left at zero: this only reads the phantom entries
    // back, and the interpolation that needs real coordinates never touches
    // them
    for (uint32_t contour = 0; contour < contours; contour++) {
        ends[contour] =
            face_u16(&face, font->glyf + from + 10u + 2u * (size_t) contour);
    }

    gvar_apply(font, glyph, xs, ys, ends, contours, stored);

    int32_t moved = xs[points + 1u] - xs[points];

    tiny_arena_release(&mark);
    return moved;
}

/** Appends one glyph's flattened outline to the edge list. */
static int glyph_edges(
    const TinyFont* font, uint32_t glyph, const Placement* at, EdgeList* list,
    uint32_t depth
) {
    if (depth > GLYPH_DEPTH) return TINYIMG_ERR_CORRUPT;
    if (glyph >= font->glyphs) return TINYIMG_OK;

    uint32_t from = loca_at(font, glyph);
    uint32_t to = loca_at(font, glyph + 1u);

    // an empty entry is a glyph with no outline, which every space is
    if (to <= from) return TINYIMG_OK;
    if (to > font->glyf_size) return TINYIMG_ERR_CORRUPT;
    if (to - from < 10u) return TINYIMG_OK;

    Face face = {font->data, font->size};
    size_t glyph_at = font->glyf + from;
    int32_t contours = face_s16(&face, glyph_at);

    if (contours > 0) {
        return simple_glyph(
            font, glyph, glyph_at, (uint32_t) contours, at, list
        );
    }
    if (contours == -1) {
        return composite_glyph(font, glyph_at, at, list, depth);
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region rasterizer

/** Vertical subsamples per output row. */
#define SUBSAMPLES 4

/** One edge crossing a subsample row. */
typedef struct {
    float x;
    int32_t winding;
} Crossing;

/**
 * @brief Adds one covered interval into a row accumulator.
 *
 * The horizontal coverage is exact rather than subsampled: a pixel the interval
 * partly covers takes the fraction it covers. Only the vertical axis is
 * subsampled, which is where a curve's slope actually needs it.
 *
 * @param acc The row, `width` entries, in units of full coverage.
 * @param width How many.
 * @param left Left end of the interval, in mask columns.
 * @param right Right end.
 * @param weight What full coverage of a pixel is worth, 1 / SUBSAMPLES.
 */
static void add_interval(
    float* acc, uint32_t width, float left, float right, float weight
) {
    if (right <= 0.0f || left >= (float) width) return;
    if (left < 0.0f) left = 0.0f;
    if (right > (float) width) right = (float) width;
    if (right <= left) return;

    uint32_t from = (uint32_t) left;
    uint32_t to = (uint32_t) right;

    if (to >= width) to = width - 1u;

    for (uint32_t x = from; x <= to; x++) {
        float lo = (float) x;
        float hi = lo + 1.0f;

        float a = left > lo ? left : lo;
        float b = right < hi ? right : hi;

        if (b > a) acc[x] += (b - a) * weight;
    }
}

/**
 * @brief Fills one subsample row into the accumulator, by nonzero winding.
 *
 * The half-open test on the edge's vertical span is what makes a vertex shared
 * by two edges cross once. Counting it twice leaves a one-pixel hole at every
 * point where two segments meet, which on a flattened curve is every few
 * pixels.
 */
static void raster_row(
    const EdgeList* list, const uint32_t* active, uint32_t count, float y,
    float origin_x, float* acc, uint32_t width, Crossing* crossings
) {
    uint32_t found = 0;

    for (uint32_t k = 0; k < count; k++) {
        const Edge* edge = &list->edges[active[k]];
        float top = edge->y0 < edge->y1 ? edge->y0 : edge->y1;
        float bottom = edge->y0 < edge->y1 ? edge->y1 : edge->y0;

        if (y < top || y >= bottom) continue;

        float t = (y - edge->y0) / (edge->y1 - edge->y0);

        crossings[found].x = edge->x0 + t * (edge->x1 - edge->x0) - origin_x;
        crossings[found].winding = edge->y1 > edge->y0 ? 1 : -1;
        found++;
    }

    for (uint32_t i = 1; i < found; i++) {
        Crossing hold = crossings[i];
        uint32_t j = i;

        while (j > 0u && crossings[j - 1u].x > hold.x) {
            crossings[j] = crossings[j - 1u];
            j--;
        }

        crossings[j] = hold;
    }

    int32_t winding = 0;
    float start = 0.0f;

    for (uint32_t i = 0; i < found; i++) {
        int32_t before = winding;
        winding += crossings[i].winding;

        if (before == 0 && winding != 0)
            start = crossings[i].x;
        else if (before != 0 && winding == 0) {
            add_interval(
                acc, width, start, crossings[i].x, 1.0f / (float) SUBSAMPLES
            );
        }
    }
}

/**
 * @brief Rasterizes an edge list into an 8-bit coverage mask.
 *
 * @param list The edges, in absolute pixel coordinates.
 * @param origin_x Left edge of the mask in those coordinates.
 * @param origin_y Top edge.
 * @param mask Receives `width * height` coverage bytes.
 * @param width Mask width.
 * @param height Mask height.
 * @return int TINYIMG_OK or TINYIMG_ERR_MEMORY.
 */
static int raster_fill(
    const EdgeList* list, float origin_x, float origin_y, uint8_t* mask,
    uint32_t width, uint32_t height
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t count = list->count;

    // one block carved five ways rather than five allocations. every piece is
    // four byte aligned and every length is a multiple of four, so the carve
    // needs no padding
    size_t floats = (size_t) width * sizeof(float);
    size_t cross = (size_t) (count + 1u) * sizeof(Crossing);
    size_t bucket_bytes = (size_t) height * sizeof(int32_t);
    size_t link_bytes = (size_t) count * sizeof(int32_t);
    size_t active_bytes = (size_t) count * sizeof(uint32_t);

    uint8_t* block = (uint8_t*) tiny_arena_alloc(
        floats + cross + bucket_bytes + link_bytes + active_bytes, 4
    );

    if (!block) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    float* acc = (float*) (void*) block;
    Crossing* crossings = (Crossing*) (void*) (block + floats);
    int32_t* buckets = (int32_t*) (void*) (block + floats + cross);
    int32_t* links = (int32_t*) (void*) (block + floats + cross + bucket_bytes);
    uint32_t* active = (uint32_t*) (void*) (block + floats + cross +
                                            bucket_bytes + link_bytes);

    for (uint32_t row = 0; row < height; row++) buckets[row] = -1;

    // an edge is bucketed by the row it starts in and stays active until the
    // row it ends in. testing every edge against every row instead is what a
    // scanline fill costs without this, and it is quadratic in the glyph: a
    // round 'o' has seventy edges and four of them cross any given row
    for (uint32_t i = 0; i < count; i++) {
        const Edge* edge = &list->edges[i];
        float top = edge->y0 < edge->y1 ? edge->y0 : edge->y1;

        int32_t row = (int32_t) tiny_floorf(top - origin_y);
        row = tiny_clampi(row, 0, (int32_t) height - 1);

        links[i] = buckets[row];
        buckets[row] = (int32_t) i;
    }

    uint32_t live = 0;

    for (uint32_t row = 0; row < height; row++) {
        for (int32_t j = buckets[row]; j >= 0; j = links[j]) {
            active[live++] = (uint32_t) j;
        }

        float top = origin_y + (float) row;

        for (uint32_t k = 0; k < live;) {
            const Edge* edge = &list->edges[active[k]];
            float bottom = edge->y0 < edge->y1 ? edge->y1 : edge->y0;

            if (bottom <= top)
                active[k] = active[--live];
            else
                k++;
        }

        for (uint32_t x = 0; x < width; x++) acc[x] = 0.0f;

        for (uint32_t s = 0; s < SUBSAMPLES; s++) {
            float y = top + ((float) s + 0.5f) / (float) SUBSAMPLES;

            raster_row(list, active, live, y, origin_x, acc, width, crossings);
        }

        uint8_t* out = mask + (size_t) row * width;

        for (uint32_t x = 0; x < width; x++) {
            out[x] = tiny_clamp_u8f(acc[x] * 255.0f);
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

#pragma endregion

#pragma region bitmap faces

/**
 * @brief Parses a PSF header.
 *
 * Both versions: PSF1 is a four byte header with a fixed 8 pixel width, and
 * PSF2 carries its own dimensions. Neither has a character map worth reading
 * here, so the codepoint is the glyph index and a face covers latin-1 at most.
 */
static int psf_load(TinyFont* font) {
    Face face = {font->data, font->size};

    if (face_u16(&face, 0) == 0x3604u) {
        uint32_t mode = face_u8(&face, 2);
        uint32_t height = face_u8(&face, 3);

        if (height == 0u) return TINYIMG_ERR_CORRUPT;

        font->cell_width = 8u;
        font->cell_height = height;
        font->glyph_bytes = height;
        font->bitmap = 4u;
        font->glyphs = (mode & 1u) ? 512u : 256u;
    }
    else {
        uint32_t header = face_u32le(&face, 8);
        uint32_t glyphs = face_u32le(&face, 16);
        uint32_t bytes = face_u32le(&face, 20);
        uint32_t height = face_u32le(&face, 24);
        uint32_t width = face_u32le(&face, 28);

        if (width == 0u || height == 0u || glyphs == 0u) {
            return TINYIMG_ERR_CORRUPT;
        }
        if (bytes < (width + 7u) / 8u * height) return TINYIMG_ERR_CORRUPT;

        font->cell_width = width;
        font->cell_height = height;
        font->glyph_bytes = bytes;
        font->bitmap = header;
        font->glyphs = glyphs;
    }

    if (!face_holds(
            &face, font->bitmap, (size_t) font->glyph_bytes * font->glyphs
        )) {
        return TINYIMG_ERR_CORRUPT;
    }

    font->kind = TINYIMG_FONT_PSF;
    font->units_per_em = font->cell_height;
    font->ascent = (int32_t) font->cell_height;
    font->descent = 0;
    font->line_gap = 0;

    return TINYIMG_OK;
}

/** Whether a BDF line starts with a keyword. */
static int bdf_is(const Face* face, size_t at, const char* keyword) {
    for (size_t i = 0; keyword[i]; i++) {
        if (face_u8(face, at + i) != (uint32_t) (uint8_t) keyword[i]) return 0;
    }

    return 1;
}

/** Advances past the rest of a line. */
static size_t bdf_line(const Face* face, size_t at) {
    while (at < face->size && face->data[at] != '\n') at++;
    return at < face->size ? at + 1u : at;
}

/** Reads a signed decimal, skipping the spaces before it. */
static int32_t bdf_int(const Face* face, size_t* at) {
    while (*at < face->size &&
           (face->data[*at] == ' ' || face->data[*at] == '\t')) {
        (*at)++;
    }

    int32_t sign = 1;

    if (*at < face->size && face->data[*at] == '-') {
        sign = -1;
        (*at)++;
    }

    int32_t value = 0;

    while (*at < face->size && face->data[*at] >= '0' &&
           face->data[*at] <= '9') {
        value = value * 10 + (face->data[*at] - '0');
        (*at)++;
    }

    return sign * value;
}

/**
 * @brief Parses a BDF face and builds its codepoint index.
 *
 * BDF is a text format with no index of its own, so finding a glyph without one
 * means scanning the whole file per character. The index is two words per glyph
 * and is the only thing a face owns.
 */
static int bdf_load(TinyFont* font) {
    Face face = {font->data, font->size};

    font->kind = TINYIMG_FONT_BDF;
    font->cell_width = 0u;
    font->cell_height = 0u;
    font->ascent = 0;
    font->descent = 0;
    font->line_gap = 0;

    uint32_t chars = 0;
    size_t at = 0;

    while (at < face.size) {
        if (bdf_is(&face, at, "CHARS ")) {
            size_t read = at + 6u;
            chars = (uint32_t) bdf_int(&face, &read);
            break;
        }

        at = bdf_line(&face, at);
    }

    if (chars == 0u || chars > 65536u) return TINYIMG_ERR_CORRUPT;

    font->index =
        (uint32_t*) tiny_alloc((size_t) chars * 2u * sizeof(uint32_t));
    if (!font->index) return TINYIMG_ERR_MEMORY;

    font->index_count = 0u;
    at = 0;

    int32_t encoding = -1;

    while (at < face.size) {
        if (bdf_is(&face, at, "FONTBOUNDINGBOX ")) {
            size_t read = at + 16u;
            font->cell_width = (uint32_t) bdf_int(&face, &read);
            font->cell_height = (uint32_t) bdf_int(&face, &read);
            bdf_int(&face, &read);
            font->descent = -bdf_int(&face, &read);
            font->ascent = (int32_t) font->cell_height - font->descent;
        }
        else if (bdf_is(&face, at, "ENCODING ")) {
            size_t read = at + 9u;
            encoding = bdf_int(&face, &read);
        }
        else if (bdf_is(&face, at, "BITMAP")) {
            if (encoding >= 0 && font->index_count < chars) {
                font->index[font->index_count * 2u] = (uint32_t) encoding;
                font->index[font->index_count * 2u + 1u] =
                    (uint32_t) bdf_line(&face, at);
                font->index_count++;
            }
            encoding = -1;
        }

        at = bdf_line(&face, at);
    }

    if (font->cell_width == 0u || font->cell_height == 0u) {
        tiny_free(font->index);
        font->index = 0;
        return TINYIMG_ERR_CORRUPT;
    }

    font->units_per_em = font->cell_height;
    font->glyphs = font->index_count;

    return TINYIMG_OK;
}

/** Reads one hex digit, or 16 when the byte is not one. */
static uint32_t hex_of(uint32_t byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10u;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10u;
    return 16u;
}

/**
 * @brief Renders a bitmap glyph into a coverage mask.
 *
 * @param font The face.
 * @param codepoint What to render.
 * @param mask Receives `cell_width * cell_height` bytes, already zeroed.
 * @return int Non-zero when the face had the glyph.
 */
static int bitmap_glyph(
    const TinyFont* font, uint32_t codepoint, uint8_t* mask
) {
    Face face = {font->data, font->size};
    uint32_t width = font->cell_width;
    uint32_t height = font->cell_height;
    uint32_t stride = (width + 7u) / 8u;

    if (font->kind == TINYIMG_FONT_PSF) {
        if (codepoint >= font->glyphs) return 0;

        size_t at = font->bitmap + (size_t) codepoint * font->glyph_bytes;

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t byte = face_u8(&face, at + y * stride + x / 8u);

                if (byte & (0x80u >> (x & 7u))) {
                    mask[(size_t) y * width + x] = 255u;
                }
            }
        }

        return 1;
    }

    size_t rows = 0;

    for (uint32_t i = 0; i < font->index_count; i++) {
        if (font->index[i * 2u] != codepoint) continue;
        rows = font->index[i * 2u + 1u];
        break;
    }

    if (rows == 0u) return 0;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t nibble = 0; nibble < stride * 2u; nibble++) {
            uint32_t digit = hex_of(face_u8(&face, rows + nibble));
            if (digit > 15u) break;

            for (uint32_t bit = 0; bit < 4u; bit++) {
                uint32_t x = nibble * 4u + bit;
                if (x >= width) break;

                if (digit & (0x8u >> bit)) {
                    mask[(size_t) y * width + x] = 255u;
                }
            }
        }

        rows = bdf_line(&face, rows);
        if (rows >= face.size) break;
    }

    return 1;
}

#pragma endregion

#pragma region loading

/**
 * @brief Finds the GPOS pair lookup the `kern` feature points at.
 *
 * The walk is fixed for the life of the face, so it runs once: GPOS header to
 * the script list, the default script's default language system, its feature
 * indices, the one tagged `kern`, and that feature's first pair-positioning
 * lookup.
 *
 * **The default script rather than a per-run one**, because choosing a script
 * needs to know what language the text is in and nothing in this library does.
 * Every face puts Latin kerning in the default script, so what this misses is a
 * face that kerns Cyrillic differently from Latin, which is a shaping question
 * rather than a kerning one.
 */
static void layout_feature(
    const Face* face, uint32_t table, uint32_t tag, uint32_t type,
    uint32_t* lookup_at, uint32_t* subtables
) {
    *lookup_at = 0u;
    *subtables = 0u;

    if (table == 0u) return;

    uint32_t scripts = table + face_u16(face, table + 4u);
    uint32_t features = table + face_u16(face, table + 6u);
    uint32_t lookups = table + face_u16(face, table + 8u);

    if (face_u16(face, scripts) == 0u) return;

    uint32_t script = scripts + face_u16(face, scripts + 6u);
    uint32_t default_lang = face_u16(face, script);
    if (default_lang == 0u) return;

    uint32_t lang = script + default_lang;
    uint32_t indices = face_u16(face, lang + 4u);

    for (uint32_t i = 0; i < indices; i++) {
        uint32_t index = face_u16(face, lang + 6u + 2u * (size_t) i);
        size_t record = features + 2u + 6u * (size_t) index;

        if (face_u32(face, record) != tag) continue;

        uint32_t feature = features + face_u16(face, record + 4u);
        uint32_t count = face_u16(face, feature + 2u);

        for (uint32_t j = 0; j < count; j++) {
            uint32_t which = face_u16(face, feature + 4u + 2u * (size_t) j);
            uint32_t lookup =
                lookups + face_u16(face, lookups + 2u + 2u * (size_t) which);

            // the feature can also point at an extension or a contextual
            // lookup, and neither is the one thing being looked for
            if (face_u16(face, lookup) != type) continue;

            *lookup_at = lookup;
            *subtables = face_u16(face, lookup + 4u);
            return;
        }
    }
}

/**
 * @brief Finds the two layout lookups this library applies.
 *
 * GPOS pair positioning under `kern` and GSUB ligature substitution under
 * `liga`, both from the first script's default language system.
 *
 * **The default language system rather than a per-run one**, because choosing a
 * script needs to know what language the text is in and nothing in this library
 * does. Every face puts Latin kerning and the standard ligatures there, so what
 * this misses is a face that kerns Cyrillic differently from Latin, which is a
 * shaping question rather than a kerning one.
 */
static void gpos_load(TinyFont* font, const Face* face) {
    layout_feature(
        face, table_of(face, TAG('G', 'P', 'O', 'S'), 0),
        TAG('k', 'e', 'r', 'n'), 2u, &font->gpos_kern, &font->gpos_subtables
    );

    layout_feature(
        face, table_of(face, TAG('G', 'S', 'U', 'B'), 0),
        TAG('l', 'i', 'g', 'a'), 4u, &font->gsub_liga, &font->gsub_subtables
    );
}

static int truetype_load(TinyFont* font) {
    Face face = {font->data, font->size};

    uint32_t head = table_of(&face, TAG('h', 'e', 'a', 'd'), 0);
    uint32_t hhea = table_of(&face, TAG('h', 'h', 'e', 'a'), 0);
    uint32_t maxp = table_of(&face, TAG('m', 'a', 'x', 'p'), 0);
    uint32_t loca_size = 0;

    font->glyf = table_of(&face, TAG('g', 'l', 'y', 'f'), &font->glyf_size);
    font->loca = table_of(&face, TAG('l', 'o', 'c', 'a'), &loca_size);
    font->hmtx = table_of(&face, TAG('h', 'm', 't', 'x'), 0);
    font->kern = table_of(&face, TAG('k', 'e', 'r', 'n'), 0);

    gpos_load(font, &face);

    font->fvar = table_of(&face, TAG('f', 'v', 'a', 'r'), 0);
    font->gvar = table_of(&face, TAG('g', 'v', 'a', 'r'), 0);
    font->avar = table_of(&face, TAG('a', 'v', 'a', 'r'), 0);
    font->axes = 0u;
    font->varied = 0u;

    for (uint32_t i = 0; i < TINYIMG_FONT_MAX_AXES; i++) font->coords[i] = 0.0f;

    if (font->fvar != 0u) {
        uint32_t declared = face_u16(&face, font->fvar + 8u);

        // a face with more axes than this can be set on is still usable at the
        // ones that fit, and its `gvar` regions name the rest at zero, which is
        // the default instance on those axes
        font->axes =
            declared > TINYIMG_FONT_MAX_AXES ? TINYIMG_FONT_MAX_AXES : declared;
    }

    if (head == 0u || maxp == 0u) return TINYIMG_ERR_CORRUPT;

    // an OpenType file with CFF charstrings has no glyf, and its outlines are
    // cubic in a different table with its own interpreter; refusing it is not
    // the same as failing to parse it
    if (font->glyf == 0u || font->loca == 0u) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    font->units_per_em = face_u16(&face, head + 18u);
    font->long_loca = face_s16(&face, head + 50u) != 0;
    font->glyphs = face_u16(&face, maxp + 4u);

    if (font->units_per_em == 0u || font->glyphs == 0u) {
        return TINYIMG_ERR_CORRUPT;
    }

    uint32_t entries = font->long_loca ? loca_size / 4u : loca_size / 2u;
    if (entries < font->glyphs + 1u) return TINYIMG_ERR_CORRUPT;

    if (hhea != 0u) {
        font->ascent = face_s16(&face, hhea + 4u);
        font->descent = -face_s16(&face, hhea + 6u);
        font->line_gap = face_s16(&face, hhea + 8u);
        font->hmetrics = face_u16(&face, hhea + 34u);
    }
    else {
        // no hhea is out of specification but recoverable: the em box is a
        // usable line, and every glyph then shares the first advance
        font->ascent = (int32_t) font->units_per_em;
        font->descent = 0;
        font->hmetrics = 1u;
    }

    if (font->hmetrics > font->glyphs) font->hmetrics = font->glyphs;

    font->kind = TINYIMG_FONT_TRUETYPE;
    cmap_select(font);

    return TINYIMG_OK;
}

int tiny_font_load_bytes(TinyFont* font, const uint8_t* data, size_t size) {
    if (!font || !data) return TINYIMG_ERR_NULL;
    if (size < 16u) return TINYIMG_ERR_UNKNOWN_FORMAT;

    tiny_memset(font, 0, sizeof(*font));
    font->data = data;
    font->size = size;

    Face face = {data, size};
    uint32_t signature = face_u32(&face, 0);

    if (signature == 0x00010000u || signature == TAG('t', 'r', 'u', 'e') ||
        signature == TAG('t', 't', 'c', 'f')) {
        return truetype_load(font);
    }

    if (signature == TAG('O', 'T', 'T', 'O')) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    // 0x864AB572 little-endian, which is how the specification writes it
    if (face_u16(&face, 0) == 0x3604u || face_u32le(&face, 0) == 0x864AB572u) {
        return psf_load(font);
    }

    if (bdf_is(&face, 0, "STARTFONT")) return bdf_load(font);

    return TINYIMG_ERR_UNKNOWN_FORMAT;
}

TINYIMG_EXPORT("tiny_font_load")
int tiny_font_load(TinyFont* font, const char* blob_id) {
    if (!font) return TINYIMG_ERR_NULL;

    size_t size = 0;
    const uint8_t* data = tiny_blob_get(TINYIMG_BLOB_FONT, blob_id, &size);

    if (!data) return TINYIMG_ERR_BLOB_MISSING;
    return tiny_font_load_bytes(font, data, size);
}

TINYIMG_EXPORT("tiny_font_free")
void tiny_font_free(TinyFont* font) {
    if (!font) return;

    if (font->index) tiny_free(font->index);
    tiny_memset(font, 0, sizeof(*font));
}

/** What one font unit is worth in pixels at a size. */
static float unit_scale(const TinyFont* font, float size) {
    if (font->kind != TINYIMG_FONT_TRUETYPE) return 1.0f;
    if (size <= 0.0f) return 1.0f;

    return size / (float) font->units_per_em;
}

/** The size a style asks for, resolved against what the face can do. */
static float style_size(const TinyFont* font, const TinyTextStyle* style) {
    if (font->kind != TINYIMG_FONT_TRUETYPE) {
        return (float) font->cell_height;
    }

    float size = style ? style->size : 0.0f;
    return size > 0.0f ? size : (float) font->units_per_em;
}

TINYIMG_EXPORT("tiny_font_metrics")
int tiny_font_metrics(const TinyFont* font, float size, TinyFontMetrics* out) {
    if (!font || !font->data || !out) return TINYIMG_ERR_NULL;

    TinyTextStyle style;
    tiny_memset(&style, 0, sizeof(style));
    style.size = size;

    float resolved = style_size(font, &style);
    float scale = unit_scale(font, resolved);

    out->size = resolved;
    out->ascent = (float) font->ascent * scale;
    out->descent = (float) font->descent * scale;
    out->line_height =
        (float) (font->ascent + font->descent + font->line_gap) * scale;
    out->glyphs = font->glyphs;
    out->fixed_size = font->kind != TINYIMG_FONT_TRUETYPE;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_font_has_glyph")
int tiny_font_has_glyph(const TinyFont* font, uint32_t codepoint) {
    if (!font || !font->data) return 0;

    if (font->kind == TINYIMG_FONT_TRUETYPE) {
        return glyph_of(font, codepoint) != 0u;
    }

    if (font->kind == TINYIMG_FONT_PSF) return codepoint < font->glyphs;

    for (uint32_t i = 0; i < font->index_count; i++) {
        if (font->index[i * 2u] == codepoint) return 1;
    }

    return 0;
}

TINYIMG_EXPORT("tiny_font_sizeof")
uint32_t tiny_font_sizeof(void) {
    return (uint32_t) sizeof(TinyFont);
}

TINYIMG_EXPORT("tiny_font_metrics_sizeof")
uint32_t tiny_font_metrics_sizeof(void) {
    return (uint32_t) sizeof(TinyFontMetrics);
}

#pragma endregion

#pragma region fallback chains

TINYIMG_EXPORT("tiny_font_set_init")
int tiny_font_set_init(TinyFontSet* set) {
    if (!set) return TINYIMG_ERR_NULL;

    tiny_memset(set, 0, sizeof(*set));
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_font_set_add")
int tiny_font_set_add(TinyFontSet* set, const TinyFont* font) {
    if (!set || !font) return TINYIMG_ERR_NULL;
    if (set->count >= TINYIMG_FONT_MAX_FALLBACK) return TINYIMG_ERR_RANGE;

    set->face[set->count++] = font;
    return TINYIMG_OK;
}

const TinyFont* tiny_font_set_for(const TinyFontSet* set, uint32_t codepoint) {
    if (!set || set->count == 0u) return 0;

    for (uint32_t i = 0; i < set->count; i++) {
        if (tiny_font_has_glyph(set->face[i], codepoint)) return set->face[i];
    }

    return set->face[0];
}

#pragma endregion

#pragma region layout

/**
 * @brief One face, one style, resolved into the numbers layout needs.
 *
 * Computed once per call rather than per glyph, because every one of them is a
 * division or a table read and none of them changes across a run.
 */
typedef struct {
    const TinyFont* font;
    /** The chain the face was taken from, or NULL for a single face. */
    const TinyFontSet* set;
    float size;
    float scale;
    float tracking;
    float ascent;
    float descent;
    float line_height;
    int kerning;
} Setting;

static void setting_of(
    Setting* out, const TinyFont* font, const TinyTextStyle* style
) {
    out->font = font;
    out->set = 0;
    out->size = style_size(font, style);
    out->scale = unit_scale(font, out->size);
    out->tracking = style ? style->tracking : 0.0f;
    out->kerning = style ? style->kerning != 0 : 0;

    out->ascent = (float) font->ascent * out->scale;
    out->descent = (float) font->descent * out->scale;

    float natural =
        (float) (font->ascent + font->descent + font->line_gap) * out->scale;
    float multiple =
        style && style->line_height > 0.0f ? style->line_height : 1.0f;

    out->line_height = natural * multiple;
}

/**
 * @brief Which face draws a codepoint.
 *
 * The primary face when it has a glyph, then the rest of the chain, then the
 * primary again so there is always something to draw. Not `has_glyph` on the
 * primary alone: a face that covers the codepoint is preferred over one that
 * does not even when the primary is listed first, which is the whole point of
 * a chain.
 */
static const TinyFont* face_for(const Setting* setting, uint32_t codepoint) {
    if (!setting->set) return setting->font;

    return tiny_font_set_for(setting->set, codepoint);
}

/** What one font unit of a face is worth at this setting's size. */
static float scale_for(const Setting* setting, const TinyFont* face) {
    if (face == setting->font) return setting->scale;

    return unit_scale(face, setting->size);
}

/** Whether any face in the setting can draw a codepoint. */
static int covered(const Setting* setting, uint32_t codepoint) {
    if (!setting->set) {
        return tiny_font_has_glyph(setting->font, codepoint);
    }

    for (uint32_t i = 0; i < setting->set->count; i++) {
        if (tiny_font_has_glyph(setting->set->face[i], codepoint)) return 1;
    }

    return 0;
}

/** What one glyph of a face advances the pen by, in pixels. */
static float advance_glyph(
    const Setting* setting, const TinyFont* font, uint32_t glyph,
    uint32_t left_glyph
) {
    if (font->kind != TINYIMG_FONT_TRUETYPE) {
        return (float) font->cell_width + setting->tracking;
    }

    float scale = scale_for(setting, font);
    float advance =
        (float) (advance_of(font, glyph) + advance_delta(font, glyph)) * scale;

    if (setting->kerning && left_glyph != 0u) {
        advance += (float) kern_of(font, left_glyph, glyph) * scale;
    }

    return advance + setting->tracking;
}

/** What a codepoint advances the pen by, in pixels. */
static float advance_for(
    const Setting* setting, uint32_t codepoint, uint32_t previous
) {
    const TinyFont* font = face_for(setting, codepoint);
    uint32_t left = 0u;

    // a kern pair is a property of one face, so it is only read when both
    // glyphs came from the same one
    if (previous != 0u && face_for(setting, previous) == font) {
        left = glyph_of(font, previous);
    }

    return advance_glyph(setting, font, glyph_of(font, codepoint), left);
}

/** How many codepoints are worth reading ahead for a ligature. */
#define TEXT_LIGATURE_LOOKAHEAD 4

/**
 * @brief Whether a ligature starts at this byte, and what it replaces.
 *
 * The lookahead is bounded because the longest standard ligature is three
 * components and a face that lists a longer one gets its shorter forms instead,
 * which is a worse setting rather than a wrong one.
 *
 * Every codepoint of the run has to resolve to the same face: a ligature is a
 * property of one face's glyph ids, and reading a fallback face's id as this
 * one's would substitute an unrelated glyph.
 *
 * @param bytes Receives how many bytes of `text` the ligature covers.
 * @param glyph Receives the ligature's glyph.
 * @return int Non-zero when one applies.
 */
static int ligature_at(
    const Setting* setting, const char* text, size_t at, size_t* bytes,
    uint32_t* glyph
) {
    *bytes = 0u;
    *glyph = 0u;

    const TinyFont* font = setting->font;

    if (font->gsub_liga == 0u || font->kind != TINYIMG_FONT_TRUETYPE) return 0;

    uint32_t run[TEXT_LIGATURE_LOOKAHEAD];
    size_t ends[TEXT_LIGATURE_LOOKAHEAD];
    uint32_t count = 0;
    size_t walk = at;

    while (count < TEXT_LIGATURE_LOOKAHEAD) {
        size_t before = walk;
        uint32_t codepoint = utf8_next(text, &walk);

        if (codepoint == 0u || codepoint == '\n' || codepoint == '\r') break;
        if (walk == before) break;
        if (face_for(setting, codepoint) != font) break;

        run[count] = glyph_of(font, codepoint);
        ends[count] = walk;
        count++;
    }

    uint32_t consumed = 0;
    uint32_t found = ligature_of(font, run, count, &consumed);

    if (found == 0u || consumed < 2u) return 0;

    *glyph = found;
    *bytes = ends[consumed - 1u] - at;

    return 1;
}

/**
 * @brief Where one line of a run ends.
 *
 * Reports the byte length of the line, its width, and how many bytes to skip to
 * reach the next one, which differ when the break was a newline or a space that
 * the line itself does not include.
 */
typedef struct {
    size_t length;
    size_t skip;
    float width;
    uint32_t glyphs;
    uint32_t missing;
    /** Spaces inside the line, which is what justification widens. */
    uint32_t spaces;
    /** Non-zero when a newline or the end of the run ended it. */
    uint8_t hard;
} Line;

/**
 * @brief Measures the next line, breaking at `limit` pixels when one is given.
 *
 * Breaks after the last space that fits. A single word wider than the limit has
 * no space to break at, so it breaks mid-word rather than overflowing, which is
 * what keeps a long unbroken string inside the box it was given.
 */
static void line_of(
    const Setting* setting, const char* text, size_t from, float limit,
    Line* out
) {
    size_t at = from;
    uint32_t previous = 0u;
    float width = 0.0f;

    tiny_memset(out, 0, sizeof(*out));

    size_t break_at = 0u;
    size_t break_skip = 0u;
    float break_width = 0.0f;
    uint32_t break_glyphs = 0u;
    uint32_t break_missing = 0u;
    uint32_t break_spaces = 0u;
    int have_break = 0;

    uint32_t glyphs = 0u;
    uint32_t missing = 0u;
    uint32_t spaces = 0u;

    for (;;) {
        size_t start = at;
        uint32_t codepoint = utf8_next(text, &at);

        if (codepoint == 0u) {
            out->length = start - from;
            out->skip = start - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            out->spaces = spaces;
            out->hard = 1u;
            return;
        }

        if (codepoint == '\n') {
            out->length = start - from;
            out->skip = at - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            out->spaces = spaces;
            out->hard = 1u;
            return;
        }

        if (codepoint == '\r') continue;

        /*
         * A ligature replaces the run it covers with one glyph, so the walk
         * consumes those codepoints here and the line's own byte length still
         * covers them. The break candidate and the glyph count are unaffected,
         * because a ligature is one glyph over several characters and neither
         * of those is a character count.
         */
        size_t ligature_bytes = 0u;
        uint32_t ligature_glyph = 0u;
        float advance;

        if (ligature_at(
                setting, text, start, &ligature_bytes, &ligature_glyph
            )) {
            advance = advance_glyph(
                setting, setting->font, ligature_glyph,
                previous != 0u && face_for(setting, previous) == setting->font
                    ? glyph_of(setting->font, previous)
                    : 0u
            );
            at = start + ligature_bytes;
            codepoint = 0xFFFFu;
        }
        else {
            advance = advance_for(setting, codepoint, previous);
        }

        if (limit > 0.0f && width + advance > limit && start > from) {
            if (have_break) {
                out->length = break_at - from;
                out->skip = break_skip - from;
                out->width = break_width;
                out->glyphs = break_glyphs;
                out->missing = break_missing;
                out->spaces = break_spaces;
                return;
            }

            out->length = start - from;
            out->skip = start - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            out->spaces = spaces;
            return;
        }

        width += advance;
        glyphs++;

        if (codepoint != 0xFFFFu && !covered(setting, codepoint)) missing++;

        if (codepoint == ' ') {
            // the break keeps the text before the space and drops the space
            // itself, so a wrapped line has no trailing whitespace to align
            have_break = 1;
            break_at = start;
            break_skip = at;
            break_width = width - advance;
            break_glyphs = glyphs - 1u;
            break_missing = missing;
            break_spaces = spaces;
            spaces++;
        }

        previous = codepoint;
    }
}

/**
 * @brief Walks a run line by line.
 *
 * The one place the layout is decided. Measurement and drawing both go through
 * it, so a caller who measures and then draws gets the same lines rather than
 * two implementations that agree until one of them is changed.
 *
 * @param setting The resolved face and style.
 * @param text The run.
 * @param limit Wrap width in pixels, or zero for no wrapping.
 * @param out Receives the metrics of the whole run.
 * @param each Called per line, or NULL to measure only.
 * @param context Passed through to `each`.
 */
static void run_walk(
    const Setting* setting, const char* text, float limit, TinyTextMetrics* out,
    void (*each)(
        void*, const Setting*, const char*, size_t, const Line*, uint32_t
    ),
    void* context
) {
    size_t at = 0u;
    uint32_t index = 0u;

    out->width = 0.0f;
    out->lines = 0u;
    out->glyphs = 0u;
    out->missing = 0u;
    out->ascent = setting->ascent;
    out->descent = setting->descent;
    out->line_height = setting->line_height;

    for (;;) {
        Line line;
        line_of(setting, text, at, limit, &line);

        if (each) each(context, setting, text + at, line.length, &line, index);

        if (line.width > out->width) out->width = line.width;
        out->glyphs += line.glyphs;
        out->missing += line.missing;
        out->lines++;
        index++;

        if (text[at + line.skip] == '\0') break;
        at += line.skip;
    }

    out->height = setting->line_height * (float) out->lines;
}

TINYIMG_EXPORT("tiny_text_style")
void tiny_text_style(TinyTextStyle* style, float size) {
    if (!style) return;

    tiny_memset(style, 0, sizeof(*style));

    style->size = size;
    style->line_height = 1.0f;
    style->kerning = 1u;
}

TINYIMG_EXPORT("tiny_text_measure")
int tiny_text_measure(
    const TinyFont* font, const char* text, const TinyTextStyle* style,
    TinyTextMetrics* out
) {
    return tiny_text_measure_wrapped(font, text, 0u, style, out);
}

TINYIMG_EXPORT("tiny_text_measure_wrapped")
int tiny_text_measure_wrapped(
    const TinyFont* font, const char* text, uint32_t width,
    const TinyTextStyle* style, TinyTextMetrics* out
) {
    if (!font || !font->data || !text || !out) return TINYIMG_ERR_NULL;
    if (style && (!tiny_finite(style->size) || style->size < 0.0f)) {
        return TINYIMG_ERR_RANGE;
    }

    Setting setting;
    setting_of(&setting, font, style);

    run_walk(&setting, text, (float) width, out, 0, 0);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_text_style_sizeof")
uint32_t tiny_text_style_sizeof(void) {
    return (uint32_t) sizeof(TinyTextStyle);
}

TINYIMG_EXPORT("tiny_text_metrics_sizeof")
uint32_t tiny_text_metrics_sizeof(void) {
    return (uint32_t) sizeof(TinyTextMetrics);
}

TINYIMG_EXPORT("tiny_text_box_sizeof")
uint32_t tiny_text_box_sizeof(void) {
    return (uint32_t) sizeof(TinyTextBox);
}

TINYIMG_EXPORT("tiny_text_line_sizeof")
uint32_t tiny_text_line_sizeof(void) {
    return (uint32_t) sizeof(TinyTextLine);
}

#pragma endregion

#pragma region masks

/**
 * @brief Grows a coverage mask by a disc of the given radius.
 *
 * A disc rather than a square, because a square dilation leaves corners
 * sticking out of every round letter. The disc is not separable, so it is one
 * shifted row per vertical offset with a horizontal span computed from the
 * circle, which is `2r + 1` cheap passes rather than one expensive one.
 *
 * The maximum of the coverage, not a sum: coverage is opacity, and adding two
 * partly covered neighbors would make an outline darker than solid.
 */
static void dilate_mask(
    const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height,
    uint32_t radius
) {
    tiny_memset(out, 0, (size_t) width * height);

    for (int32_t dy = -(int32_t) radius; dy <= (int32_t) radius; dy++) {
        float reach = (float) (radius * radius) - (float) (dy * dy);
        int32_t span = (int32_t) tiny_sqrtf(reach > 0.0f ? reach : 0.0f);

        for (uint32_t y = 0; y < height; y++) {
            int32_t from = (int32_t) y + dy;
            if (from < 0 || from >= (int32_t) height) continue;

            const uint8_t* source = in + (size_t) from * width;
            uint8_t* target = out + (size_t) y * width;

            for (uint32_t x = 0; x < width; x++) {
                uint32_t best = target[x];

                int32_t low = (int32_t) x - span;
                int32_t high = (int32_t) x + span;

                if (low < 0) low = 0;
                if (high > (int32_t) width - 1) high = (int32_t) width - 1;

                for (int32_t at = low; at <= high; at++) {
                    if (source[at] > best) best = source[at];
                }

                target[x] = (uint8_t) best;
            }
        }
    }
}

/** One separable box pass over a mask, with a running sum. */
static void box_pass(
    const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height,
    uint32_t radius
) {
    uint32_t window = 2u * radius + 1u;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = in + (size_t) y * width;
        uint8_t* target = out + (size_t) y * width;

        uint32_t sum = 0;

        for (int32_t at = -(int32_t) radius; at <= (int32_t) radius; at++) {
            if (at >= 0 && at < (int32_t) width) sum += row[at];
        }

        for (uint32_t x = 0; x < width; x++) {
            target[x] = (uint8_t) (sum / window);

            int32_t leaving = (int32_t) x - (int32_t) radius;
            int32_t entering = (int32_t) x + (int32_t) radius + 1;

            if (leaving >= 0 && leaving < (int32_t) width) sum -= row[leaving];
            if (entering >= 0 && entering < (int32_t) width) {
                sum += row[entering];
            }
        }
    }
}

/** Transposes a mask, which is how the vertical box pass reuses the horizontal
 * one. */
static void transpose_mask(
    const uint8_t* in, uint8_t* out, uint32_t width, uint32_t height
) {
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            out[(size_t) x * height + y] = in[(size_t) y * width + x];
        }
    }
}

/**
 * @brief Softens a mask, in place.
 *
 * Three box passes each way, which is the usual approximation of a gaussian and
 * is within a level of one at the radius a shadow uses. `scratch` has to hold
 * as many bytes as `mask`.
 */
static void blur_mask(
    uint8_t* mask, uint8_t* scratch, uint32_t width, uint32_t height,
    uint32_t radius
) {
    if (radius == 0u) return;

    for (uint32_t pass = 0; pass < 3u; pass++) {
        box_pass(mask, scratch, width, height, radius);
        tiny_memcpy(mask, scratch, (size_t) width * height);
    }

    transpose_mask(mask, scratch, width, height);

    for (uint32_t pass = 0; pass < 3u; pass++) {
        box_pass(scratch, mask, height, width, radius);
        tiny_memcpy(scratch, mask, (size_t) width * height);
    }

    transpose_mask(scratch, mask, height, width);
}

#pragma endregion

#pragma region drawing

/**
 * @brief Dilates or softens a padded mask and draws it.
 *
 * The one place a pass's mask transform lives, so the outline path and the
 * bitmap path cannot drift apart.
 */
/**
 * @brief Which layer of a run is being drawn.
 *
 * A run with a shadow or an outline is three walks rather than one: every
 * shadow, then every outline, then every fill. Drawing a glyph's three layers
 * together lets one glyph's outline print over the previous glyph's fill, which
 * is visible wherever two letters touch.
 */
typedef enum
{
    /** The offset, softened copy under everything. */
    PASS_SHADOW = 0,
    /** The outline, under the fill. */
    PASS_STROKE = 1,
    /** The glyph itself. */
    PASS_FILL = 2
} Pass;

static int grow_and_draw(
    TinyImage* image, uint8_t* mask, uint32_t width, uint32_t height, int32_t x,
    int32_t y, const uint8_t* color, Pass pass, const TinyTextStyle* style
);

/**
 * @brief Draws one glyph at a pen position.
 *
 * The mask is built at the size the glyph actually occupies rather than at the
 * em box, so a full stop costs a few hundred bytes of scratch and a capital
 * costs what it needs. Both are released before the next glyph.
 *
 * The shadow and outline passes pad the mask by what they grow it by and then
 * dilate or soften it, so the glyph is rasterized once per pass it takes part
 * in. Rasterizing once and reusing it across passes would need the padding
 * decided before the outline is known, and the glyph raster is not what a text
 * draw spends its time on.
 */
static int draw_glyph_id(
    TinyImage* image, const Setting* setting, const TinyFont* font,
    EdgeList* list, uint32_t glyph, float pen_x, float baseline,
    const uint8_t* color, Pass pass, const TinyTextStyle* style
);

/**
 * @brief Draws one glyph of a bitmap face.
 *
 * Its own function because a bitmap face has one cell per codepoint and no
 * outline at all, so it shares the pass handling with the outline path and
 * nothing else.
 */
static int draw_bitmap_glyph(
    TinyImage* image, const Setting* setting, const TinyFont* font,
    uint32_t codepoint, float pen_x, float baseline, const uint8_t* color,
    Pass pass, const TinyTextStyle* style
) {
    (void) setting;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    int result = TINYIMG_OK;
    uint32_t grow = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    if (pass == PASS_STROKE) {
        grow = (uint32_t) tiny_ceilf(style->stroke);
    }
    else if (pass == PASS_SHADOW) {
        grow = (uint32_t) tiny_ceilf(style->shadow_blur);
        offset_x = style->shadow_x;
        offset_y = style->shadow_y;
    }

    uint32_t width = font->cell_width + 2u * grow;
    uint32_t height = font->cell_height + 2u * grow;
    size_t bytes = (size_t) width * height;

    uint8_t* mask = (uint8_t*) tiny_arena_alloc(bytes, 1);
    uint8_t* cell = (uint8_t*) tiny_arena_alloc(
        (size_t) font->cell_width * font->cell_height, 1
    );

    if (!mask || !cell) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(mask, 0, bytes);
    tiny_memset(cell, 0, (size_t) font->cell_width * font->cell_height);

    if (bitmap_glyph(font, codepoint, cell)) {
        for (uint32_t y = 0; y < font->cell_height; y++) {
            tiny_memcpy(
                mask + (size_t) (y + grow) * width + grow,
                cell + (size_t) y * font->cell_width, font->cell_width
            );
        }

        result = grow_and_draw(
            image, mask, width, height,
            (int32_t) tiny_roundf(pen_x + offset_x) - (int32_t) grow,
            (int32_t) tiny_roundf(baseline + offset_y) -
                (int32_t) font->ascent - (int32_t) grow,
            color, pass, style
        );
    }

    tiny_arena_release(&mark);
    return result;

    tiny_arena_release(&mark);
    return result;
}

static int draw_glyph(
    TinyImage* image, const Setting* setting, EdgeList* list,
    uint32_t codepoint, float pen_x, float baseline, const uint8_t* color,
    Pass pass, const TinyTextStyle* style
) {
    const TinyFont* font = face_for(setting, codepoint);

    if (font->kind != TINYIMG_FONT_TRUETYPE) {
        return draw_bitmap_glyph(
            image, setting, font, codepoint, pen_x, baseline, color, pass, style
        );
    }

    return draw_glyph_id(
        image, setting, font, list, glyph_of(font, codepoint), pen_x, baseline,
        color, pass, style
    );
}

/**
 * @brief Draws one glyph of one face at a pen position.
 *
 * By glyph id rather than by codepoint, because a ligature has an id and no
 * codepoint at all.
 */
static int draw_glyph_id(
    TinyImage* image, const Setting* setting, const TinyFont* font,
    EdgeList* list, uint32_t glyph, float pen_x, float baseline,
    const uint8_t* color, Pass pass, const TinyTextStyle* style
) {
    float scale = scale_for(setting, font);

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    int result = TINYIMG_OK;

    uint32_t grow = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    if (pass == PASS_STROKE) {
        grow = (uint32_t) tiny_ceilf(style->stroke);
    }
    else if (pass == PASS_SHADOW) {
        grow = (uint32_t) tiny_ceilf(style->shadow_blur);
        offset_x = style->shadow_x;
        offset_y = style->shadow_y;
    }

    list->count = 0u;
    list->overflowed = 0u;

    Placement at;
    affine_identity(&at.units);
    at.scale = scale;
    at.pen_x = pen_x;
    at.baseline = baseline;

    result = glyph_edges(font, glyph, &at, list, 0u);

    if (result != TINYIMG_OK || list->count == 0u) {
        tiny_arena_release(&mark);
        return result;
    }

    if (list->overflowed) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    float min_x = list->edges[0].x0;
    float max_x = min_x;
    float min_y = list->edges[0].y0;
    float max_y = min_y;

    for (uint32_t i = 0; i < list->count; i++) {
        const Edge* edge = &list->edges[i];
        float xs[2] = {edge->x0, edge->x1};
        float ys[2] = {edge->y0, edge->y1};

        for (uint32_t j = 0; j < 2u; j++) {
            if (xs[j] < min_x) min_x = xs[j];
            if (xs[j] > max_x) max_x = xs[j];
            if (ys[j] < min_y) min_y = ys[j];
            if (ys[j] > max_y) max_y = ys[j];
        }
    }

    float origin_x = tiny_floorf(min_x) - (float) grow;
    float origin_y = tiny_floorf(min_y) - (float) grow;
    int32_t width =
        (int32_t) tiny_ceilf(max_x) + (int32_t) grow - (int32_t) origin_x;
    int32_t height =
        (int32_t) tiny_ceilf(max_y) + (int32_t) grow - (int32_t) origin_y;

    if (width <= 0 || height <= 0) {
        tiny_arena_release(&mark);
        return TINYIMG_OK;
    }

    // a glyph the image cannot show is not rasterized at all, which is what
    // makes drawing a long string into a small image cost the visible part
    float at_x = origin_x + offset_x;
    float at_y = origin_y + offset_y;

    if (at_x >= (float) image->width || at_y >= (float) image->height ||
        at_x + (float) width <= 0.0f || at_y + (float) height <= 0.0f) {
        tiny_arena_release(&mark);
        return TINYIMG_OK;
    }

    uint8_t* mask =
        (uint8_t*) tiny_arena_alloc((size_t) width * (size_t) height, 1);

    if (!mask) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    result = raster_fill(
        list, origin_x, origin_y, mask, (uint32_t) width, (uint32_t) height
    );

    if (result == TINYIMG_OK) {
        result = grow_and_draw(
            image, mask, (uint32_t) width, (uint32_t) height, (int32_t) at_x,
            (int32_t) at_y, color, pass, style
        );
    }

    tiny_arena_release(&mark);
    return result;
}

static int grow_and_draw(
    TinyImage* image, uint8_t* mask, uint32_t width, uint32_t height, int32_t x,
    int32_t y, const uint8_t* color, Pass pass, const TinyTextStyle* style
) {
    size_t bytes = (size_t) width * height;

    if (pass == PASS_STROKE) {
        TinyArenaMark mark;
        tiny_arena_mark(&mark);

        uint8_t* grown = (uint8_t*) tiny_arena_alloc(bytes, 1);

        if (!grown) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        dilate_mask(
            mask, grown, width, height, (uint32_t) tiny_ceilf(style->stroke)
        );

        int result = tiny_draw_coverage(
            image, x, y, grown, width, height, color, TINYIMG_BLEND_NORMAL
        );

        tiny_arena_release(&mark);
        return result;
    }

    if (pass == PASS_SHADOW && style->shadow_blur > 0.0f) {
        TinyArenaMark mark;
        tiny_arena_mark(&mark);

        uint8_t* scratch = (uint8_t*) tiny_arena_alloc(bytes, 1);

        if (!scratch) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        blur_mask(
            mask, scratch, width, height,
            (uint32_t) tiny_ceilf(style->shadow_blur)
        );

        int result = tiny_draw_coverage(
            image, x, y, mask, width, height, color, TINYIMG_BLEND_NORMAL
        );

        tiny_arena_release(&mark);
        return result;
    }

    return tiny_draw_coverage(
        image, x, y, mask, width, height, color, TINYIMG_BLEND_NORMAL
    );
}

/**
 * @brief What the per-line callback carries while drawing or measuring.
 *
 * `fits` and `total` come from a measuring walk that ran first, because both
 * the vertical alignment and the ellipsis need to know how many lines there are
 * before the first one is placed.
 */
typedef struct {
    TinyImage* image;
    const uint8_t* color;
    const TinyTextStyle* style;
    EdgeList* list;
    int32_t x;
    int32_t y;
    float box_width;
    float box_height;
    TinyTextAlign align;
    TinyTextOverflow overflow;
    Pass pass;

    /** Pixels the whole run is shifted down by, from the vertical alignment. */
    float top;
    /** Lines the box's height has room for, or zero for no limit. */
    uint32_t fits;
    /** Lines the run takes. */
    uint32_t total;

    /** Where to record the layout, or NULL when only drawing. */
    TinyTextLine* lines;
    uint32_t capacity;
    /** Byte offset of the line being placed, tracked across the walk. */
    size_t at;

    int result;
} Draw;

/** The codepoint an ellipsis uses, and the fallback for a face without it. */
#define TEXT_ELLIPSIS 0x2026u

/**
 * @brief What an ellipsis costs on this face, and which form it takes.
 *
 * A subset face often has no U+2026, and a missing glyph box in the middle of a
 * truncated headline is worse than three full stops.
 */
static float ellipsis_width(const Setting* setting, uint32_t* codepoint) {
    if (covered(setting, TEXT_ELLIPSIS)) {
        *codepoint = TEXT_ELLIPSIS;
        return advance_for(setting, TEXT_ELLIPSIS, 0u);
    }

    *codepoint = (uint32_t) '.';
    return 3.0f * advance_for(setting, (uint32_t) '.', 0u);
}

/** Where a line starts, and how much every space inside it widens by. */
static void place_line(
    const Draw* draw, const Line* line, float* pen, float* space_extra
) {
    *pen = (float) draw->x;
    *space_extra = 0.0f;

    if (draw->box_width <= 0.0f) return;

    float slack = draw->box_width - line->width;
    if (slack < 0.0f) slack = 0.0f;

    if (draw->align == TINYIMG_ALIGN_CENTER) {
        *pen += slack * 0.5f;
    }
    else if (draw->align == TINYIMG_ALIGN_RIGHT) {
        *pen += slack;
    }
    else if (draw->align == TINYIMG_ALIGN_JUSTIFY) {
        // the last line of a paragraph is set left, and so is a line with no
        // space to widen: stretching four words across a full measure is worse
        // than the ragged edge it was avoiding
        if (!line->hard && line->spaces > 0u) {
            *space_extra = slack / (float) line->spaces;
        }
    }
}

static void draw_line(
    void* context, const Setting* setting, const char* text, size_t length,
    const Line* line, uint32_t index
) {
    Draw* draw = (Draw*) context;
    if (draw->result != TINYIMG_OK) return;

    float top =
        draw->top + (float) draw->y + setting->line_height * (float) index;

    int clipped = draw->fits > 0u && index >= draw->fits;
    int ellipsize = draw->overflow == TINYIMG_OVERFLOW_ELLIPSIS &&
                    draw->fits > 0u && draw->total > draw->fits &&
                    index + 1u == draw->fits;

    float pen = 0.0f;
    float space_extra = 0.0f;

    place_line(draw, line, &pen, &space_extra);

    // a justified line that is about to end in an ellipsis is set left, since
    // its measure is no longer the one the justification was computed from
    if (ellipsize) space_extra = 0.0f;

    float baseline = top + setting->ascent;
    size_t taken = length;
    uint32_t mark = 0u;
    float mark_width = 0.0f;

    if (ellipsize) {
        mark_width = ellipsis_width(setting, &mark);

        if (draw->box_width > 0.0f) {
            // whole codepoints are dropped from the end until the ellipsis
            // fits, which is what keeps a truncated multi-byte character from
            // becoming a replacement glyph
            float used = line->width;
            size_t cut = length;

            while (cut > 0u && used + mark_width > draw->box_width) {
                size_t back = cut;

                // step back one codepoint, which is the first byte below the
                // cut that is not a continuation
                while (back > 0u) {
                    back--;
                    if ((text[back] & 0xC0) != 0x80) break;
                }

                size_t peek = back;
                uint32_t dropped = utf8_next(text, &peek);

                used -= advance_for(setting, dropped, 0u);
                cut = back;
            }

            taken = cut;
        }
    }

    if (draw->lines && index < draw->capacity) {
        TinyTextLine* record = &draw->lines[index];

        record->at = (uint32_t) draw->at;
        record->length = (uint32_t) taken;
        record->width = line->width;
        record->x = pen;
        record->y = top;
        record->baseline = baseline;
        record->glyphs = line->glyphs;
        record->missing = line->missing;
        record->clipped = (uint8_t) (clipped ? 1 : 0);
        record->ellipsized = (uint8_t) (ellipsize ? 1 : 0);
    }

    draw->at += line->skip;

    if (!draw->image || clipped) return;

    size_t at = 0u;
    uint32_t previous = 0u;

    while (at < taken) {
        size_t start = at;
        uint32_t codepoint = utf8_next(text, &at);

        if (codepoint == 0u) break;
        if (codepoint == '\r') continue;

        size_t ligature_bytes = 0u;
        uint32_t ligature_glyph = 0u;

        if (start + 1u < taken &&
            ligature_at(
                setting, text, start, &ligature_bytes, &ligature_glyph
            ) &&
            start + ligature_bytes <= taken) {
            int result = draw_glyph_id(
                draw->image, setting, setting->font, draw->list, ligature_glyph,
                pen, baseline, draw->color, draw->pass, draw->style
            );

            if (result != TINYIMG_OK) {
                draw->result = result;
                return;
            }

            pen += advance_glyph(
                setting, setting->font, ligature_glyph,
                previous != 0u && face_for(setting, previous) == setting->font
                    ? glyph_of(setting->font, previous)
                    : 0u
            );

            at = start + ligature_bytes;
            previous = 0u;

            continue;
        }

        if (codepoint != ' ') {
            int result = draw_glyph(
                draw->image, setting, draw->list, codepoint, pen, baseline,
                draw->color, draw->pass, draw->style
            );

            if (result != TINYIMG_OK) {
                draw->result = result;
                return;
            }
        }

        pen += advance_for(setting, codepoint, previous);
        if (codepoint == ' ') pen += space_extra;

        previous = codepoint;
    }

    if (!ellipsize) return;

    uint32_t marks = mark == TEXT_ELLIPSIS ? 1u : 3u;

    for (uint32_t i = 0; i < marks; i++) {
        int result = draw_glyph(
            draw->image, setting, draw->list, mark, pen, baseline, draw->color,
            draw->pass, draw->style
        );

        if (result != TINYIMG_OK) {
            draw->result = result;
            return;
        }

        pen += advance_for(setting, mark, 0u);
    }
}

/** Fills in the box-derived numbers a placement needs before the first line. */
static void prepare(
    Draw* draw, const Setting* setting, const char* text, const TinyTextBox* box
) {
    TinyTextMetrics metrics;
    run_walk(setting, text, draw->box_width, &metrics, 0, 0);

    draw->total = metrics.lines;
    draw->fits = 0u;
    draw->top = 0.0f;

    if (draw->box_height <= 0.0f || setting->line_height <= 0.0f) return;

    float room = draw->box_height / setting->line_height;
    draw->fits = room < 1.0f ? 0u : (uint32_t) room;

    // a box too short for one line still draws that line, because clipping the
    // only line of a label to nothing is never what a caller meant
    if (draw->fits == 0u) draw->fits = 1u;

    uint32_t drawn = draw->total < draw->fits ? draw->total : draw->fits;
    float used = setting->line_height * (float) drawn;
    float slack = draw->box_height - used;

    if (slack <= 0.0f) return;

    TinyTextVAlign valign = box ? box->valign : TINYIMG_VALIGN_TOP;

    if (valign == TINYIMG_VALIGN_MIDDLE)
        draw->top = slack * 0.5f;
    else if (valign == TINYIMG_VALIGN_BOTTOM)
        draw->top = slack;
}

/**
 * @brief Everything the drawing and measuring entry points share.
 *
 * `measure` rather than a NULL image deciding it, because a NULL image is a
 * caller error on the drawing entry points and has to stay one.
 */
static int draw_run(
    TinyImage* image, const TinyFont* font, const TinyFontSet* set,
    const char* text, int32_t x, int32_t y, const TinyTextBox* box,
    const TinyTextStyle* style, const uint8_t* color, int measure,
    TinyTextLine* lines, uint32_t capacity, uint32_t* count
) {
    if (!font || !text) return TINYIMG_ERR_NULL;
    if (!measure && (!image || !image->data || !color)) {
        return TINYIMG_ERR_NULL;
    }
    if (!font->data) return TINYIMG_ERR_BLOB_MISSING;
    if (style && (!tiny_finite(style->size) || style->size < 0.0f)) {
        return TINYIMG_ERR_RANGE;
    }

    Setting setting;
    setting_of(&setting, font, style);
    setting.set = set;

    if (!tiny_finite(setting.size) || setting.size <= 0.0f) {
        return TINYIMG_ERR_RANGE;
    }

    TinyTextStyle resolved;
    tiny_memset(&resolved, 0, sizeof(resolved));
    if (style) resolved = *style;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    EdgeList list;
    list.count = 0u;
    list.capacity = TEXT_MAX_EDGES;
    list.overflowed = 0u;
    list.edges = 0;

    if (font->kind == TINYIMG_FONT_TRUETYPE) {
        list.edges =
            (Edge*) tiny_arena_alloc((size_t) TEXT_MAX_EDGES * sizeof(Edge), 4);

        if (!list.edges) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }
    }

    Draw draw;
    tiny_memset(&draw, 0, sizeof(draw));

    draw.image = measure ? 0 : image;
    draw.color = color;
    draw.style = &resolved;
    draw.list = &list;
    draw.x = x;
    draw.y = y;
    draw.box_width = box ? (float) box->width : 0.0f;
    draw.box_height = box ? (float) box->height : 0.0f;
    draw.align = box ? box->align : TINYIMG_ALIGN_LEFT;
    draw.overflow = box ? box->overflow : TINYIMG_OVERFLOW_CLIP;
    draw.lines = lines;
    draw.capacity = capacity;
    draw.result = TINYIMG_OK;

    prepare(&draw, &setting, text, box);

    int shadow = resolved.shadow_x != 0.0f || resolved.shadow_y != 0.0f ||
                 resolved.shadow_blur > 0.0f;
    int stroke = resolved.stroke > 0.0f;

    TinyTextMetrics metrics;

    if (measure) {
        draw.pass = PASS_FILL;
        draw.at = 0u;
        run_walk(&setting, text, draw.box_width, &metrics, draw_line, &draw);
    }
    else {
        for (Pass pass = PASS_SHADOW; pass <= PASS_FILL; pass++) {
            if (pass == PASS_SHADOW && !shadow) continue;
            if (pass == PASS_STROKE && !stroke) continue;

            draw.pass = pass;
            draw.at = 0u;
            draw.color = pass == PASS_SHADOW   ? resolved.shadow_color
                         : pass == PASS_STROKE ? resolved.stroke_color
                                               : color;

            // the layout is recorded once, on the pass that draws the glyphs
            draw.lines = pass == PASS_FILL ? lines : 0;

            run_walk(
                &setting, text, draw.box_width, &metrics, draw_line, &draw
            );

            if (draw.result != TINYIMG_OK) break;
        }
    }

    if (count) *count = draw.total;

    tiny_arena_release(&mark);
    return draw.result;
}

TINYIMG_EXPORT("tiny_image_draw_text")
int tiny_image_draw_text(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, const TinyTextStyle* style, const uint8_t* color
) {
    return draw_run(image, font, 0, text, x, y, 0, style, color, 0, 0, 0u, 0);
}

TINYIMG_EXPORT("tiny_image_draw_text_box")
int tiny_image_draw_text_box(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, uint32_t width, uint32_t height, const TinyTextStyle* style,
    TinyTextAlign align, const uint8_t* color
) {
    TinyTextBox box;
    tiny_memset(&box, 0, sizeof(box));

    box.width = width;
    box.height = height;
    box.align = align;

    return draw_run(
        image, font, 0, text, x, y, &box, style, color, 0, 0, 0u, 0
    );
}

TINYIMG_EXPORT("tiny_image_draw_text_in")
int tiny_image_draw_text_in(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, const TinyTextBox* box, const TinyTextStyle* style,
    const uint8_t* color
) {
    return draw_run(image, font, 0, text, x, y, box, style, color, 0, 0, 0u, 0);
}

#pragma endregion

#pragma region rich runs

/** One span's contribution to one line. */
typedef struct {
    uint32_t run;
    size_t at;
    size_t length;
    float width;
    uint32_t spaces;
} Span;

/** One line of a rich run, and the metrics the spans on it agree to. */
typedef struct {
    Span span[TINYIMG_TEXT_MAX_RUNS];
    uint32_t spans;
    float width;
    float ascent;
    float descent;
    float height;
    uint32_t spaces;
    uint32_t glyphs;
    uint32_t missing;
    uint8_t hard;
} RichLine;

/** Where the walk has got to, across spans. */
typedef struct {
    uint32_t run;
    size_t at;
} Cursor;

/** Appends a span's slice to a line, or reports that the line is full. */
static int line_add(
    RichLine* line, uint32_t run, size_t at, size_t length, float width,
    uint32_t spaces
) {
    if (length == 0u) return 1;

    // one span per run, because a line covers a contiguous range of them
    if (line->spans > 0u && line->span[line->spans - 1u].run == run) {
        Span* last = &line->span[line->spans - 1u];

        last->length += length;
        last->width += width;
        last->spaces += spaces;
        line->width += width;
        line->spaces += spaces;

        return 1;
    }

    if (line->spans >= TINYIMG_TEXT_MAX_RUNS) return 0;

    Span* span = &line->span[line->spans++];

    span->run = run;
    span->at = at;
    span->length = length;
    span->width = width;
    span->spaces = spaces;

    line->width += width;
    line->spaces += spaces;

    return 1;
}

/**
 * @brief Measures the next line of a rich run.
 *
 * The same break rule as the single-style walk, tracked across spans: the
 * position after the last space that fitted, and a mid-word break when one
 * word is wider than the measure.
 *
 * The line's metrics are the largest of the spans on it, so a line that mixes
 * sizes sits on one baseline rather than each span having its own.
 */
static void rich_line_of(
    const Setting* settings, const TinyTextRun* runs, uint32_t count,
    Cursor from, float limit, RichLine* out, Cursor* next
) {
    tiny_memset(out, 0, sizeof(*out));

    Cursor here = from;
    Cursor mark = from;
    int have_mark = 0;

    RichLine at_mark;
    tiny_memset(&at_mark, 0, sizeof(at_mark));

    uint32_t previous = 0u;
    float width = 0.0f;

    while (here.run < count) {
        const Setting* setting = &settings[here.run];
        const char* text = runs[here.run].text;

        if (out->spans == 0u || out->span[out->spans - 1u].run != here.run) {
            if (setting->ascent > out->ascent) out->ascent = setting->ascent;
            if (setting->descent > out->descent)
                out->descent = setting->descent;
            if (setting->line_height > out->height) {
                out->height = setting->line_height;
            }
        }

        size_t start = here.at;
        size_t walk = here.at;
        uint32_t codepoint = utf8_next(text, &walk);

        if (codepoint == 0u) {
            // the end of a span is not the end of a line; the next span
            // continues it, and kerning does not cross the boundary
            here.run++;
            here.at = 0u;
            previous = 0u;
            continue;
        }

        if (codepoint == '\n') {
            here.at = walk;
            out->hard = 1u;
            *next = here;
            return;
        }

        if (codepoint == '\r') {
            here.at = walk;
            continue;
        }

        // a ligature inside a span substitutes the same way it does in a
        // single-style run; one that would span two spans does not, because the
        // glyph ids either side belong to different faces
        size_t ligature_bytes = 0u;
        uint32_t ligature_glyph = 0u;
        float advance;

        if (ligature_at(
                setting, text, start, &ligature_bytes, &ligature_glyph
            )) {
            advance = advance_glyph(setting, setting->font, ligature_glyph, 0u);
            walk = start + ligature_bytes;
            codepoint = 0xFFFFu;
        }
        else {
            advance = advance_for(setting, codepoint, previous);
        }

        if (limit > 0.0f && width + advance > limit &&
            (here.run != from.run || here.at != from.at)) {
            if (have_mark) {
                *out = at_mark;
                *next = mark;
                return;
            }

            *next = here;
            return;
        }

        if (!line_add(
                out, here.run, start, walk - start, advance,
                codepoint == ' ' ? 1u : 0u
            )) {
            *next = here;
            return;
        }

        width += advance;
        out->glyphs++;

        if (!covered(setting, codepoint)) out->missing++;

        if (codepoint == ' ') {
            // the break keeps the text before the space and drops the space
            // itself, which is what leaves a wrapped line with no trailing
            // whitespace to align
            at_mark = *out;

            Span* last = &at_mark.span[at_mark.spans - 1u];
            last->length -= walk - start;
            last->width -= advance;
            last->spaces--;
            at_mark.width -= advance;
            at_mark.spaces--;
            at_mark.glyphs--;

            if (last->length == 0u) at_mark.spans--;

            mark.run = here.run;
            mark.at = walk;
            have_mark = 1;
        }

        here.at = walk;
        previous = codepoint;
    }

    out->hard = 1u;
    *next = here;
}

/** How many lines a rich run takes, and what the first one occupies. */
static void rich_walk(
    const Setting* settings, const TinyTextRun* runs, uint32_t count,
    float limit, uint32_t* lines, float* height, RichLine* first
) {
    Cursor at = {0u, 0u};
    uint32_t seen = 0;
    float total = 0.0f;

    for (;;) {
        RichLine line;
        Cursor next;

        rich_line_of(settings, runs, count, at, limit, &line, &next);

        if (seen == 0u && first) *first = line;

        seen++;
        total += line.height;

        if (next.run >= count) break;
        if (next.run == at.run && next.at == at.at) break;

        at = next;
    }

    if (lines) *lines = seen;
    if (height) *height = total;
}

/** Both rich entry points, once the settings have been resolved. */
static int rich_run(
    TinyImage* image, const TinyTextRun* runs, uint32_t count, int32_t x,
    int32_t y, const TinyTextBox* box, const uint8_t* color, int measure,
    TinyTextMetrics* metrics
) {
    if (!runs || count == 0u) return TINYIMG_ERR_NULL;
    if (count > TINYIMG_TEXT_MAX_RUNS) return TINYIMG_ERR_RANGE;
    if (!measure && (!image || !image->data || !color)) {
        return TINYIMG_ERR_NULL;
    }

    Setting settings[TINYIMG_TEXT_MAX_RUNS];
    TinyTextStyle resolved[TINYIMG_TEXT_MAX_RUNS];

    for (uint32_t i = 0; i < count; i++) {
        if (!runs[i].font || !runs[i].text) return TINYIMG_ERR_NULL;
        if (!runs[i].font->data) return TINYIMG_ERR_BLOB_MISSING;

        const TinyTextStyle* style = runs[i].style;

        if (style && (!tiny_finite(style->size) || style->size < 0.0f)) {
            return TINYIMG_ERR_RANGE;
        }

        tiny_memset(&resolved[i], 0, sizeof(resolved[i]));
        if (style) resolved[i] = *style;

        setting_of(&settings[i], runs[i].font, style);

        if (!tiny_finite(settings[i].size) || settings[i].size <= 0.0f) {
            return TINYIMG_ERR_RANGE;
        }
    }

    float limit = box ? (float) box->width : 0.0f;
    float box_height = box ? (float) box->height : 0.0f;

    uint32_t total = 0;
    float run_height = 0.0f;
    RichLine first;

    rich_walk(settings, runs, count, limit, &total, &run_height, &first);

    if (metrics) {
        tiny_memset(metrics, 0, sizeof(*metrics));

        metrics->lines = total;
        metrics->height = run_height;
        metrics->ascent = first.ascent;
        metrics->descent = first.descent;
        metrics->line_height = first.height;
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    EdgeList list;
    list.count = 0u;
    list.capacity = TEXT_MAX_EDGES;
    list.overflowed = 0u;
    list.edges =
        (Edge*) tiny_arena_alloc((size_t) TEXT_MAX_EDGES * sizeof(Edge), 4);

    if (!list.edges) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    int result = TINYIMG_OK;

    for (Pass pass = PASS_SHADOW; pass <= PASS_FILL && !measure; pass++) {
        Cursor at = {0u, 0u};
        float top = (float) y;

        // the vertical alignment needs the whole run's height, which the walk
        // above has already answered
        if (box_height > 0.0f && box) {
            float slack = box_height - run_height;

            if (slack > 0.0f) {
                if (box->valign == TINYIMG_VALIGN_MIDDLE)
                    top += slack * 0.5f;
                else if (box->valign == TINYIMG_VALIGN_BOTTOM)
                    top += slack;
            }
        }

        for (;;) {
            RichLine line;
            Cursor next;

            rich_line_of(settings, runs, count, at, limit, &line, &next);

            int past =
                box_height > 0.0f && top + line.height > (float) y + box_height;

            if (!past) {
                float pen = (float) x;
                float space_extra = 0.0f;

                if (limit > 0.0f) {
                    float slack = limit - line.width;
                    if (slack < 0.0f) slack = 0.0f;

                    TinyTextAlign align = box ? box->align : TINYIMG_ALIGN_LEFT;

                    if (align == TINYIMG_ALIGN_CENTER)
                        pen += slack * 0.5f;
                    else if (align == TINYIMG_ALIGN_RIGHT)
                        pen += slack;
                    else if (
                        align == TINYIMG_ALIGN_JUSTIFY && !line.hard &&
                        line.spaces > 0u
                    ) {
                        space_extra = slack / (float) line.spaces;
                    }
                }

                float baseline = top + line.ascent;

                for (uint32_t s = 0; s < line.spans && result == TINYIMG_OK;
                     s++) {
                    const Span* span = &line.span[s];
                    const Setting* setting = &settings[span->run];
                    const TinyTextStyle* style = &resolved[span->run];

                    int shadow = style->shadow_x != 0.0f ||
                                 style->shadow_y != 0.0f ||
                                 style->shadow_blur > 0.0f;

                    const uint8_t* fill =
                        pass == PASS_SHADOW     ? style->shadow_color
                        : pass == PASS_STROKE   ? style->stroke_color
                        : runs[span->run].color ? runs[span->run].color
                                                : color;

                    /*
                     * A span with no shadow still walks the shadow pass,
                     * because the pen has to reach the next span at the same
                     * place it will on the fill pass. Only the drawing is
                     * skipped, which is why this is a flag rather than a
                     * `continue`.
                     */
                    int draws = pass == PASS_FILL ||
                                (pass == PASS_SHADOW && shadow) ||
                                (pass == PASS_STROKE && style->stroke > 0.0f);

                    const char* text = runs[span->run].text + span->at;
                    size_t walk = 0u;
                    uint32_t previous = 0u;

                    while (walk < span->length && result == TINYIMG_OK) {
                        size_t start = walk;
                        uint32_t codepoint = utf8_next(text, &walk);

                        if (codepoint == 0u) break;
                        if (codepoint == '\r') continue;

                        size_t bytes = 0u;
                        uint32_t ligature = 0u;

                        if (start + 1u < span->length &&
                            ligature_at(
                                setting, text, start, &bytes, &ligature
                            ) &&
                            start + bytes <= span->length) {
                            if (draws) {
                                result = draw_glyph_id(
                                    image, setting, setting->font, &list,
                                    ligature, pen, baseline, fill, pass, style
                                );
                            }

                            pen += advance_glyph(
                                setting, setting->font, ligature, 0u
                            );
                            walk = start + bytes;
                            previous = 0u;

                            continue;
                        }

                        if (codepoint != ' ' && draws) {
                            result = draw_glyph(
                                image, setting, &list, codepoint, pen, baseline,
                                fill, pass, style
                            );
                        }

                        pen += advance_for(setting, codepoint, previous);
                        if (codepoint == ' ') pen += space_extra;

                        previous = codepoint;
                    }
                }
            }

            top += line.height;

            if (result != TINYIMG_OK) break;
            if (next.run >= count) break;
            if (next.run == at.run && next.at == at.at) break;

            at = next;
        }

        if (result != TINYIMG_OK) break;
    }

    tiny_arena_release(&mark);
    return result;
}

TINYIMG_EXPORT("tiny_image_draw_text_runs")
int tiny_image_draw_text_runs(
    TinyImage* image, const TinyTextRun* runs, uint32_t count, int32_t x,
    int32_t y, const TinyTextBox* box, const uint8_t* color
) {
    return rich_run(image, runs, count, x, y, box, color, 0, 0);
}

TINYIMG_EXPORT("tiny_text_measure_runs")
int tiny_text_measure_runs(
    const TinyTextRun* runs, uint32_t count, const TinyTextBox* box,
    TinyTextMetrics* out
) {
    if (!out) return TINYIMG_ERR_NULL;

    int result = rich_run(0, runs, count, 0, 0, box, 0, 1, out);

    if (result != TINYIMG_OK) return result;

    // the widest line, which the walk does not track because the draw does not
    // need it
    Setting settings[TINYIMG_TEXT_MAX_RUNS];

    for (uint32_t i = 0; i < count; i++) {
        setting_of(&settings[i], runs[i].font, runs[i].style);
    }

    float limit = box ? (float) box->width : 0.0f;
    Cursor at = {0u, 0u};

    for (;;) {
        RichLine line;
        Cursor next;

        rich_line_of(settings, runs, count, at, limit, &line, &next);

        if (line.width > out->width) out->width = line.width;
        out->glyphs += line.glyphs;
        out->missing += line.missing;

        if (next.run >= count) break;
        if (next.run == at.run && next.at == at.at) break;

        at = next;
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region measuring

TINYIMG_EXPORT("tiny_image_draw_text_set")
int tiny_image_draw_text_set(
    TinyImage* image, const TinyFontSet* set, const char* text, int32_t x,
    int32_t y, const TinyTextBox* box, const TinyTextStyle* style,
    const uint8_t* color
) {
    if (!set || set->count == 0u) return TINYIMG_ERR_NULL;

    return draw_run(
        image, set->face[0], set, text, x, y, box, style, color, 0, 0, 0u, 0
    );
}

TINYIMG_EXPORT("tiny_text_measure_set")
int tiny_text_measure_set(
    const TinyFontSet* set, const char* text, const TinyTextBox* box,
    const TinyTextStyle* style, TinyTextMetrics* out
) {
    if (!set || set->count == 0u || !out) return TINYIMG_ERR_NULL;
    if (!set->face[0] || !set->face[0]->data || !text) {
        return TINYIMG_ERR_NULL;
    }
    if (style && (!tiny_finite(style->size) || style->size < 0.0f)) {
        return TINYIMG_ERR_RANGE;
    }

    Setting setting;
    setting_of(&setting, set->face[0], style);
    setting.set = set;

    run_walk(&setting, text, box ? (float) box->width : 0.0f, out, 0, 0);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_text_lines")
int tiny_text_lines(
    const TinyFont* font, const char* text, const TinyTextBox* box,
    const TinyTextStyle* style, TinyTextLine* lines, uint32_t capacity,
    uint32_t* count
) {
    if (!count) return TINYIMG_ERR_NULL;

    return draw_run(
        0, font, 0, text, 0, 0, box, style, 0, 1, lines, capacity, count
    );
}

#pragma endregion
