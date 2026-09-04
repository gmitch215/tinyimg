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
static int32_t kern_of(const TinyFont* font, uint32_t left, uint32_t right) {
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
 * and looks like a rasteriser fault rather than a buffer that ran out.
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
    const TinyFont* font, size_t glyph_at, uint32_t contours,
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

    // one block, the two word arrays first so the byte array cannot misalign
    // them
    uint8_t* block = (uint8_t*) tiny_arena_alloc((size_t) points * 9u, 4);

    if (!block) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    int32_t* xs = (int32_t*) (void*) block;
    int32_t* ys = (int32_t*) (void*) (block + (size_t) points * 4u);
    uint8_t* flags = block + (size_t) points * 8u;

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
        return simple_glyph(font, glyph_at, (uint32_t) contours, at, list);
    }
    if (contours == -1) {
        return composite_glyph(font, glyph_at, at, list, depth);
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region rasteriser

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
 * @brief Rasterises an edge list into an 8-bit coverage mask.
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

#pragma region layout

/**
 * @brief One face, one style, resolved into the numbers layout needs.
 *
 * Computed once per call rather than per glyph, because every one of them is a
 * division or a table read and none of them changes across a run.
 */
typedef struct {
    const TinyFont* font;
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

/** What a codepoint advances the pen by, in pixels. */
static float advance_for(
    const Setting* setting, uint32_t codepoint, uint32_t previous
) {
    const TinyFont* font = setting->font;

    if (font->kind != TINYIMG_FONT_TRUETYPE) {
        return (float) font->cell_width + setting->tracking;
    }

    uint32_t glyph = glyph_of(font, codepoint);
    float advance = (float) advance_of(font, glyph) * setting->scale;

    if (setting->kerning && previous != 0u) {
        uint32_t left = glyph_of(font, previous);
        advance += (float) kern_of(font, left, glyph) * setting->scale;
    }

    return advance + setting->tracking;
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

    out->length = 0u;
    out->skip = 0u;
    out->width = 0.0f;
    out->glyphs = 0u;
    out->missing = 0u;

    size_t break_at = 0u;
    size_t break_skip = 0u;
    float break_width = 0.0f;
    uint32_t break_glyphs = 0u;
    uint32_t break_missing = 0u;
    int have_break = 0;

    uint32_t glyphs = 0u;
    uint32_t missing = 0u;

    for (;;) {
        size_t start = at;
        uint32_t codepoint = utf8_next(text, &at);

        if (codepoint == 0u) {
            out->length = start - from;
            out->skip = start - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            return;
        }

        if (codepoint == '\n') {
            out->length = start - from;
            out->skip = at - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            return;
        }

        if (codepoint == '\r') continue;

        float advance = advance_for(setting, codepoint, previous);

        if (limit > 0.0f && width + advance > limit && start > from) {
            if (have_break) {
                out->length = break_at - from;
                out->skip = break_skip - from;
                out->width = break_width;
                out->glyphs = break_glyphs;
                out->missing = break_missing;
                return;
            }

            out->length = start - from;
            out->skip = start - from;
            out->width = width;
            out->glyphs = glyphs;
            out->missing = missing;
            return;
        }

        width += advance;
        glyphs++;

        if (!tiny_font_has_glyph(setting->font, codepoint)) missing++;

        if (codepoint == ' ') {
            // the break keeps the text before the space and drops the space
            // itself, so a wrapped line has no trailing whitespace to align
            have_break = 1;
            break_at = start;
            break_skip = at;
            break_width = width - advance;
            break_glyphs = glyphs - 1u;
            break_missing = missing;
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

    style->size = size;
    style->tracking = 0.0f;
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
    if (style && style->size < 0.0f) return TINYIMG_ERR_RANGE;

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

#pragma endregion

#pragma region drawing

/**
 * @brief Draws one glyph at a pen position.
 *
 * The mask is built at the size the glyph actually occupies rather than at the
 * em box, so a full stop costs a few hundred bytes of scratch and a capital
 * costs what it needs. Both are released before the next glyph.
 */
static int draw_glyph(
    TinyImage* image, const Setting* setting, EdgeList* list,
    uint32_t codepoint, float pen_x, float baseline, const uint8_t* color
) {
    const TinyFont* font = setting->font;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    int result = TINYIMG_OK;

    if (font->kind != TINYIMG_FONT_TRUETYPE) {
        uint32_t width = font->cell_width;
        uint32_t height = font->cell_height;
        uint8_t* mask = (uint8_t*) tiny_arena_alloc((size_t) width * height, 1);

        if (!mask) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(mask, 0, (size_t) width * height);

        if (bitmap_glyph(font, codepoint, mask)) {
            result = tiny_draw_coverage(
                image, (int32_t) tiny_roundf(pen_x),
                (int32_t) tiny_roundf(baseline) - (int32_t) font->ascent, mask,
                width, height, color, TINYIMG_BLEND_NORMAL
            );
        }

        tiny_arena_release(&mark);
        return result;
    }

    uint32_t glyph = glyph_of(font, codepoint);

    list->count = 0u;
    list->overflowed = 0u;

    Placement at;
    affine_identity(&at.units);
    at.scale = setting->scale;
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

    float origin_x = tiny_floorf(min_x);
    float origin_y = tiny_floorf(min_y);
    int32_t width = (int32_t) tiny_ceilf(max_x) - (int32_t) origin_x;
    int32_t height = (int32_t) tiny_ceilf(max_y) - (int32_t) origin_y;

    if (width <= 0 || height <= 0) {
        tiny_arena_release(&mark);
        return TINYIMG_OK;
    }

    // a glyph the image cannot show is not rasterised at all, which is what
    // makes drawing a long string into a small image cost the visible part
    if (origin_x >= (float) image->width || origin_y >= (float) image->height ||
        origin_x + (float) width <= 0.0f || origin_y + (float) height <= 0.0f) {
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
        result = tiny_draw_coverage(
            image, (int32_t) origin_x, (int32_t) origin_y, mask,
            (uint32_t) width, (uint32_t) height, color, TINYIMG_BLEND_NORMAL
        );
    }

    tiny_arena_release(&mark);
    return result;
}

/** What run_walk carries into the per-line callback while drawing. */
typedef struct {
    TinyImage* image;
    const uint8_t* color;
    EdgeList* list;
    int32_t x;
    int32_t y;
    float box_width;
    float box_height;
    TinyTextAlign align;
    int result;
} Draw;

static void draw_line(
    void* context, const Setting* setting, const char* text, size_t length,
    const Line* line, uint32_t index
) {
    Draw* draw = (Draw*) context;
    if (draw->result != TINYIMG_OK) return;

    float top = (float) draw->y + setting->line_height * (float) index;

    // a line whose box has run out is not drawn, and the walk still counts it,
    // so the metrics report the overflow rather than hiding it
    if (draw->box_height > 0.0f &&
        top + setting->line_height > (float) draw->y + draw->box_height) {
        return;
    }

    float pen = (float) draw->x;

    if (draw->box_width > 0.0f) {
        float slack = draw->box_width - line->width;
        if (slack < 0.0f) slack = 0.0f;

        if (draw->align == TINYIMG_ALIGN_CENTER)
            pen += slack * 0.5f;
        else if (draw->align == TINYIMG_ALIGN_RIGHT)
            pen += slack;
    }

    float baseline = top + setting->ascent;
    size_t at = 0u;
    uint32_t previous = 0u;

    while (at < length) {
        uint32_t codepoint = utf8_next(text, &at);
        if (codepoint == 0u) break;
        if (codepoint == '\r') continue;

        if (codepoint != ' ') {
            int result = draw_glyph(
                draw->image, setting, draw->list, codepoint, pen, baseline,
                draw->color
            );

            if (result != TINYIMG_OK) {
                draw->result = result;
                return;
            }
        }

        pen += advance_for(setting, codepoint, previous);
        previous = codepoint;
    }
}

TINYIMG_EXPORT("tiny_image_draw_text")
int tiny_image_draw_text(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, const TinyTextStyle* style, const uint8_t* color
) {
    return tiny_image_draw_text_box(
        image, font, text, x, y, 0u, 0u, style, TINYIMG_ALIGN_LEFT, color
    );
}

TINYIMG_EXPORT("tiny_image_draw_text_box")
int tiny_image_draw_text_box(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, uint32_t width, uint32_t height, const TinyTextStyle* style,
    TinyTextAlign align, const uint8_t* color
) {
    if (!image || !image->data || !font || !text || !color) {
        return TINYIMG_ERR_NULL;
    }
    if (!font->data) return TINYIMG_ERR_BLOB_MISSING;
    if (style && style->size < 0.0f) return TINYIMG_ERR_RANGE;

    Setting setting;
    setting_of(&setting, font, style);

    if (setting.size <= 0.0f) return TINYIMG_ERR_RANGE;

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
    draw.image = image;
    draw.color = color;
    draw.list = &list;
    draw.x = x;
    draw.y = y;
    draw.box_width = (float) width;
    draw.box_height = (float) height;
    draw.align = align;
    draw.result = TINYIMG_OK;

    TinyTextMetrics metrics;
    run_walk(&setting, text, (float) width, &metrics, draw_line, &draw);

    tiny_arena_release(&mark);
    return draw.result;
}

#pragma endregion
