#include "tinyimg/codec/gif.h"

#include "tinyimg/codec/lzw.h"
#include "tinyimg/memory.h"

#define GIF_HEADER_SIZE 6u
#define GIF_SCREEN_SIZE 7u

#define GIF_EXTENSION 0x21u
#define GIF_IMAGE 0x2Cu
#define GIF_TRAILER 0x3Bu

#define GIF_GRAPHIC_CONTROL 0xF9u
#define GIF_COMMENT 0xFEu
#define GIF_PLAIN_TEXT 0x01u
#define GIF_APPLICATION 0xFFu

// the compressor's own limits; the decoder's live in lzw.h, which TIFF shares
#define GIF_MAX_CODE_BITS TINY_LZW_MAX_BITS
#define GIF_CODE_LIMIT TINY_LZW_CODES

typedef struct {
    uint32_t width;
    uint32_t height;

    const uint8_t* global_palette;
    uint32_t global_count;

    uint32_t frames;
    /** Non-zero once a transparent index applies to the frame being decoded. */
    uint8_t has_transparency;
    uint8_t transparent;
    uint8_t background;

    /** The first frame, as offsets into the file. */
    size_t frame_at;
    uint32_t frame_x;
    uint32_t frame_y;
    uint32_t frame_width;
    uint32_t frame_height;
    uint8_t interlaced;
    const uint8_t* frame_palette;
    uint32_t frame_count;
    uint8_t code_bits;
    size_t data_at;
} GifHeader;

#pragma region blocks

static inline uint32_t read_le16(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static int gif_sniff(const uint8_t* buffer, size_t size) {
    return buffer && size >= GIF_HEADER_SIZE &&
           tiny_memcmp(buffer, "GIF8", 4) == 0 &&
           (buffer[4] == '7' || buffer[4] == '9') && buffer[5] == 'a';
}

/** Steps over a chain of length-prefixed sub-blocks, returning where it ends.
 */
static size_t skip_blocks(const uint8_t* buffer, size_t size, size_t at) {
    while (at < size) {
        uint32_t length = buffer[at];

        if (length == 0) return at + 1;
        at += 1 + length;
    }

    return size;
}

/**
 * Walks the block sequence once, counting frames and stopping at the first.
 *
 * A graphic control extension applies to the image that follows it, so the
 * transparent index is only kept while looking for the first frame; a later
 * one belongs to a frame this codec does not decode.
 */
static int gif_parse(const uint8_t* buffer, size_t size, GifHeader* header) {
    if (!gif_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;
    if (size < GIF_HEADER_SIZE + GIF_SCREEN_SIZE) return TINYIMG_ERR_CORRUPT;

    tiny_memset(header, 0, sizeof(*header));

    const uint8_t* screen = buffer + GIF_HEADER_SIZE;

    header->width = read_le16(screen);
    header->height = read_le16(screen + 2);
    header->background = screen[5];

    if (header->width == 0 || header->height == 0) return TINYIMG_ERR_CORRUPT;

    size_t at = GIF_HEADER_SIZE + GIF_SCREEN_SIZE;

    if (screen[4] & 0x80u) {
        header->global_count = 2u << (screen[4] & 7u);

        if (at + header->global_count * 3 > size) return TINYIMG_ERR_CORRUPT;

        header->global_palette = buffer + at;
        at += header->global_count * 3;
    }

    uint8_t pending_transparency = 0;
    uint8_t pending_index = 0;

    while (at < size) {
        uint8_t block = buffer[at];

        if (block == GIF_TRAILER) break;

        if (block == GIF_EXTENSION) {
            if (at + 2 > size) return TINYIMG_ERR_CORRUPT;

            uint8_t label = buffer[at + 1];
            size_t payload = at + 2;

            if (label == GIF_GRAPHIC_CONTROL) {
                if (payload + 5 > size) return TINYIMG_ERR_CORRUPT;
                if (buffer[payload] < 4) return TINYIMG_ERR_CORRUPT;

                pending_transparency = buffer[payload + 1] & 1u;
                pending_index = buffer[payload + 4];
            }
            else if (
                label != GIF_COMMENT && label != GIF_APPLICATION &&
                label != GIF_PLAIN_TEXT
            ) {
                // an unknown extension is still a length prefixed chain, so it
                // can be stepped over rather than making the file unreadable
            }

            at = skip_blocks(buffer, size, payload);
            continue;
        }

        if (block != GIF_IMAGE) return TINYIMG_ERR_CORRUPT;
        if (at + 10 > size) return TINYIMG_ERR_CORRUPT;

        uint32_t left = read_le16(buffer + at + 1);
        uint32_t top = read_le16(buffer + at + 3);
        uint32_t wide = read_le16(buffer + at + 5);
        uint32_t high = read_le16(buffer + at + 7);
        uint8_t packed = buffer[at + 9];

        size_t after = at + 10;
        const uint8_t* palette = 0;
        uint32_t count = 0;

        if (packed & 0x80u) {
            count = 2u << (packed & 7u);

            if (after + count * 3 > size) return TINYIMG_ERR_CORRUPT;

            palette = buffer + after;
            after += count * 3;
        }

        if (after + 1 > size) return TINYIMG_ERR_CORRUPT;

        uint8_t code_bits = buffer[after];
        size_t data = after + 1;

        if (header->frames == 0) {
            if (wide == 0 || high == 0) return TINYIMG_ERR_CORRUPT;
            if (code_bits < 2 || code_bits > 11) return TINYIMG_ERR_CORRUPT;

            header->frame_at = at;
            header->frame_x = left;
            header->frame_y = top;
            header->frame_width = wide;
            header->frame_height = high;
            header->interlaced = (packed & 0x40u) ? 1u : 0u;
            header->frame_palette = palette;
            header->frame_count = count;
            header->code_bits = code_bits;
            header->data_at = data;

            header->has_transparency = pending_transparency;
            header->transparent = pending_index;
        }

        header->frames++;
        pending_transparency = 0;

        at = skip_blocks(buffer, size, data);
    }

    if (header->frames == 0) return TINYIMG_ERR_CORRUPT;

    if (!header->frame_palette && !header->global_palette) {
        return TINYIMG_ERR_CORRUPT;
    }

    return TINYIMG_OK;
}

static int gif_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    GifHeader header;
    int result = gif_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    info->width = header.width;
    info->height = header.height;
    info->frames = header.frames;
    info->format = TINYIMG_FORMAT_GIF;
    info->channels = header.has_transparency ? 4u : 3u;
    info->bit_depth = 8;
    info->has_alpha = header.has_transparency;

    // interlacing is the format's own progressive layout, and it carries the
    // same consequence: the plane has to exist before a region of it can
    info->progressive = header.interlaced;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region decode

/** The four passes an interlaced frame's rows arrive in, as start and step. */
static const uint8_t gif_interlace[4][2] = {{0, 8}, {4, 8}, {2, 4}, {1, 2}};

/**
 * Lays the frame's indices onto the logical screen.
 *
 * The screen, not the frame, is the image: a frame smaller than the screen or
 * placed at an offset is what makes the two differ, and the screen is what a
 * viewer shows. Everything outside the frame keeps the background, which for a
 * file with a transparent index is transparent.
 */
static void place_frame(
    const GifHeader* header, const uint8_t* frame, uint8_t* screen
) {
    uint32_t rows = header->frame_height;

    for (uint32_t y = 0; y < rows; y++) {
        uint32_t target = y;

        if (header->interlaced) {
            uint32_t seen = 0;

            for (uint32_t pass = 0; pass < 4; pass++) {
                uint32_t start = gif_interlace[pass][0];
                uint32_t step = gif_interlace[pass][1];

                if (start >= rows) continue;

                uint32_t in_pass = (rows - start + step - 1) / step;

                if (y < seen + in_pass) {
                    target = start + (y - seen) * step;
                    break;
                }

                seen += in_pass;
            }
        }

        uint32_t screen_y = header->frame_y + target;
        if (screen_y >= header->height) continue;

        for (uint32_t x = 0; x < header->frame_width; x++) {
            uint32_t screen_x = header->frame_x + x;
            if (screen_x >= header->width) continue;

            screen[(size_t) screen_y * header->width + screen_x] =
                frame[(size_t) y * header->frame_width + x];
        }
    }
}

/** Reads one index through the palette into RGBA. */
static void expand_index(
    const GifHeader* header, const uint8_t* palette, uint32_t count,
    uint32_t index, uint8_t* pixel
) {
    if (index < count) {
        const uint8_t* entry = palette + index * 3;

        pixel[0] = entry[0];
        pixel[1] = entry[1];
        pixel[2] = entry[2];
    }
    else {
        pixel[0] = 0;
        pixel[1] = 0;
        pixel[2] = 0;
    }

    pixel[3] =
        (header->has_transparency && index == header->transparent) ? 0 : 255;
}

static int gif_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* options
) {
    GifHeader header;
    int result = gif_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    TinyDecodeOpts resolved;
    uint32_t out_width;
    uint32_t out_height;

    result = tiny_decode_resolve(
        options, header.width, header.height, &resolved, &out_width, &out_height
    );
    if (result != TINYIMG_OK) return result;

    const uint8_t* palette =
        header.frame_palette ? header.frame_palette : header.global_palette;
    uint32_t entries =
        header.frame_palette ? header.frame_count : header.global_count;

    uint8_t channels = resolved.channels
                           ? resolved.channels
                           : (uint8_t) (header.has_transparency ? 4 : 3);

    result = tiny_image_create(image, out_width, out_height, channels);
    if (result != TINYIMG_OK) return result;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    size_t frame_pixels = (size_t) header.frame_width * header.frame_height;
    size_t screen_pixels = (size_t) header.width * header.height;

    uint8_t* frame = tiny_arena_alloc(frame_pixels, 0);
    uint8_t* screen = tiny_arena_alloc(screen_pixels, 0);
    TinyLzwTable* table = tiny_arena_alloc(sizeof(TinyLzwTable), 0);

    if (!frame || !screen || !table) {
        tiny_arena_release(&mark);
        tiny_image_destroy(image);
        return TINYIMG_ERR_MEMORY;
    }

    // outside the frame the screen shows the background, which is transparent
    // when there is a transparent index and the background entry otherwise
    tiny_memset(
        screen,
        header.has_transparency ? header.transparent : header.background,
        screen_pixels
    );
    tiny_memset(frame, 0, frame_pixels);

    TinyLzwReader reader;

    // chained, because this format frames its codes in sub-blocks, and least
    // significant bit first, which is where it differs from TIFF
    tiny_lzw_init(&reader, buffer, size, header.data_at, 1, 0);

    result = tiny_lzw_expand(
        &reader, table, header.code_bits, 0, frame, frame_pixels
    );

    if (result == TINYIMG_OK) {
        place_frame(&header, frame, screen);

        uint32_t den = resolved.scale_den;

        for (uint32_t oy = 0; oy < out_height; oy++) {
            uint8_t* dest = image->data + (size_t) oy * out_width * channels;

            for (uint32_t ox = 0; ox < out_width; ox++) {
                uint8_t rgba[4];

                if (den == 1) {
                    uint32_t index = screen
                        [(size_t) (resolved.y + oy) * header.width +
                         resolved.x + ox];

                    expand_index(&header, palette, entries, index, rgba);
                }
                else {
                    // averaging in color rather than in index space, since
                    // neighboring indices name unrelated colors
                    uint32_t sums[4] = {0, 0, 0, 0};
                    uint32_t counted = 0;

                    uint32_t top = resolved.y + oy * den;
                    uint32_t bottom = top + den;
                    uint32_t left = resolved.x + ox * den;
                    uint32_t right = left + den;

                    if (bottom > resolved.y + resolved.height) {
                        bottom = resolved.y + resolved.height;
                    }
                    if (right > resolved.x + resolved.width) {
                        right = resolved.x + resolved.width;
                    }

                    for (uint32_t sy = top; sy < bottom; sy++) {
                        for (uint32_t sx = left; sx < right; sx++) {
                            uint8_t sample[4];

                            expand_index(
                                &header, palette, entries,
                                screen[(size_t) sy * header.width + sx], sample
                            );

                            for (uint32_t c = 0; c < 4; c++) {
                                sums[c] += sample[c];
                            }
                            counted++;
                        }
                    }

                    if (counted == 0) counted = 1;

                    for (uint32_t c = 0; c < 4; c++) {
                        rgba[c] = (uint8_t) ((sums[c] + counted / 2) / counted);
                    }
                }

                tiny_pixel_convert(
                    dest + (size_t) ox * channels, channels, rgba, 4
                );
            }
        }
    }

    tiny_arena_release(&mark);

    if (result != TINYIMG_OK) {
        tiny_image_destroy(image);
        return result;
    }

    image->format = TINYIMG_FORMAT_GIF;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region palette

#define GIF_CUBE_BITS 5u
#define GIF_CUBE_SIDE (1u << GIF_CUBE_BITS)
#define GIF_CUBE_CELLS (GIF_CUBE_SIDE * GIF_CUBE_SIDE * GIF_CUBE_SIDE)

/**
 * The image's colors, counted into a five bit per channel cube.
 *
 * Eight bit sums are kept alongside the counts so a box's color is the mean of
 * what it actually held rather than the middle of the cells it spans, which for
 * a box covering one cell is worth up to four levels per channel. A cell's sum
 * cannot overflow: TINYIMG_MAX_PIXELS times 255 is 4.08e9, inside a 32 bit
 * unsigned by a fifth.
 */
typedef struct {
    uint32_t count[GIF_CUBE_CELLS];
    uint32_t sum[GIF_CUBE_CELLS][3];
} GifHistogram;

/** One axis-aligned region of the cube, in cell coordinates. */
typedef struct {
    uint8_t low[3];
    uint8_t high[3];
    uint32_t count;
} GifBox;

static inline uint32_t cube_cell(uint32_t r, uint32_t g, uint32_t b) {
    return (r << (2 * GIF_CUBE_BITS)) | (g << GIF_CUBE_BITS) | b;
}

/** Tightens a box onto the cells that are actually occupied. */
static void box_shrink(GifBox* box, const GifHistogram* histogram) {
    uint8_t low[3] = {255, 255, 255};
    uint8_t high[3] = {0, 0, 0};
    uint32_t total = 0;

    for (uint32_t r = box->low[0]; r <= box->high[0]; r++) {
        for (uint32_t g = box->low[1]; g <= box->high[1]; g++) {
            for (uint32_t b = box->low[2]; b <= box->high[2]; b++) {
                uint32_t count = histogram->count[cube_cell(r, g, b)];
                if (count == 0) continue;

                uint32_t at[3] = {r, g, b};

                for (uint32_t axis = 0; axis < 3; axis++) {
                    if (at[axis] < low[axis]) low[axis] = (uint8_t) at[axis];
                    if (at[axis] > high[axis]) high[axis] = (uint8_t) at[axis];
                }

                total += count;
            }
        }
    }

    box->count = total;

    if (total == 0) return;

    for (uint32_t axis = 0; axis < 3; axis++) {
        box->low[axis] = low[axis];
        box->high[axis] = high[axis];
    }
}

/**
 * Splits a box at the median of its longest axis, weighted by population.
 *
 * @return int Non-zero when the box was split, zero when it holds one cell.
 */
static int box_split(GifBox* box, GifBox* out, const GifHistogram* histogram) {
    uint32_t axis = 0;
    uint32_t widest = 0;

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t extent = (uint32_t) (box->high[i] - box->low[i]);

        if (extent > widest) {
            widest = extent;
            axis = i;
        }
    }

    if (widest == 0) return 0;

    uint32_t running = 0;
    uint32_t cut = box->low[axis];

    for (uint32_t position = box->low[axis]; position < box->high[axis];
         position++) {
        uint32_t slice = 0;

        for (uint32_t a = box->low[(axis + 1) % 3];
             a <= box->high[(axis + 1) % 3]; a++) {
            for (uint32_t b = box->low[(axis + 2) % 3];
                 b <= box->high[(axis + 2) % 3]; b++) {
                uint32_t coordinate[3];

                coordinate[axis] = position;
                coordinate[(axis + 1) % 3] = a;
                coordinate[(axis + 2) % 3] = b;

                slice += histogram->count[cube_cell(
                    coordinate[0], coordinate[1], coordinate[2]
                )];
            }
        }

        running += slice;
        cut = position;

        // the first position that puts half the population behind it, which
        // leaves both halves non-empty because the loop stops short of the end
        if (running * 2 >= box->count) break;
    }

    *out = *box;

    out->low[axis] = (uint8_t) (cut + 1);
    box->high[axis] = (uint8_t) cut;

    box_shrink(box, histogram);
    box_shrink(out, histogram);

    return 1;
}

/**
 * Chooses `wanted` colors by repeatedly splitting the most populous box.
 *
 * Median cut rather than the octree the plan named, and the reason is measured
 * against that octree with the same lookup and the same metric: on the
 * reference photograph it is 8.9 dB better, 33.47 against 42.40. An octree
 * reduction merges up to eight children into one leaf, so it overshoots the
 * target and cannot get the entries back; making the reduction pick its
 * cheapest merge instead recovered 1.6 dB on one photograph and lost 3.3 dB on
 * another, so that heuristic was not converging on a fix. Median cut produces
 * exactly as many boxes as asked for by construction, and needs no node pool,
 * free list or leaf accounting.
 */
static uint32_t median_cut(
    const GifHistogram* histogram, uint32_t wanted, GifBox* boxes,
    uint8_t* palette
) {
    boxes[0].low[0] = 0;
    boxes[0].low[1] = 0;
    boxes[0].low[2] = 0;
    boxes[0].high[0] = GIF_CUBE_SIDE - 1;
    boxes[0].high[1] = GIF_CUBE_SIDE - 1;
    boxes[0].high[2] = GIF_CUBE_SIDE - 1;

    box_shrink(&boxes[0], histogram);

    uint32_t count = boxes[0].count ? 1 : 0;

    while (count > 0 && count < wanted) {
        uint32_t chosen = 0;
        uint32_t best = 0;

        for (uint32_t i = 0; i < count; i++) {
            int splittable = boxes[i].high[0] > boxes[i].low[0] ||
                             boxes[i].high[1] > boxes[i].low[1] ||
                             boxes[i].high[2] > boxes[i].low[2];

            if (splittable && boxes[i].count > best) {
                best = boxes[i].count;
                chosen = i;
            }
        }

        if (best == 0) break;
        if (!box_split(&boxes[chosen], &boxes[count], histogram)) break;

        count++;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sums[3] = {0, 0, 0};
        uint32_t total = 0;

        for (uint32_t r = boxes[i].low[0]; r <= boxes[i].high[0]; r++) {
            for (uint32_t g = boxes[i].low[1]; g <= boxes[i].high[1]; g++) {
                for (uint32_t b = boxes[i].low[2]; b <= boxes[i].high[2]; b++) {
                    uint32_t cell = cube_cell(r, g, b);
                    uint32_t held = histogram->count[cell];

                    if (held == 0) continue;

                    for (uint32_t c = 0; c < 3; c++) {
                        sums[c] += histogram->sum[cell][c];
                    }
                    total += held;
                }
            }
        }

        if (total == 0) total = 1;

        for (uint32_t c = 0; c < 3; c++) {
            palette[i * 3 + c] = (uint8_t) (sums[c] / total);
        }
    }

    return count;
}

#define GIF_LOOKUP_BITS 6u
#define GIF_LOOKUP_SIDE (1u << GIF_LOOKUP_BITS)
#define GIF_LOOKUP_CELLS (GIF_LOOKUP_SIDE * GIF_LOOKUP_SIDE * GIF_LOOKUP_SIDE)

/**
 * A remembered nearest palette entry per cell of a six bit color cube.
 *
 * Two things this is not, both tried and measured. It is not a descent of the
 * quantizer's own tree to the color's octant, which returns that octant's mean
 * rather than the nearest entry. And it is not a table filled in up front,
 * which at this resolution is 67 million distance computations for the few
 * thousand cells an image actually asks about.
 *
 * Filled on demand instead, by an exhaustive search over the palette the first
 * time a cell is asked for. Six bits per channel rather than five, which is
 * worth 0.2 to 2.0 dB across the fixtures: a palette chosen for an illustration
 * can hold several entries inside one five bit cell, and everything but the
 * first becomes unreachable.
 */
typedef struct {
    int16_t index[GIF_LOOKUP_CELLS];
} GifInverse;

static uint32_t inverse_lookup(
    GifInverse* inverse, const uint8_t* palette, uint32_t count, uint32_t skip,
    const uint8_t* color
) {
    uint32_t shift = 8u - GIF_LOOKUP_BITS;
    uint32_t cell = ((uint32_t) (color[0] >> shift) << (2 * GIF_LOOKUP_BITS)) |
                    ((uint32_t) (color[1] >> shift) << GIF_LOOKUP_BITS) |
                    (uint32_t) (color[2] >> shift);

    if (inverse->index[cell] >= 0) return (uint32_t) inverse->index[cell];

    uint32_t best = 0;
    int32_t closest = 0x7FFFFFFF;

    for (uint32_t i = 0; i < count; i++) {
        // the transparent entry is a hole in the palette rather than a color,
        // so nothing may be mapped onto it
        if (i == skip) continue;

        int32_t dr = (int32_t) color[0] - palette[i * 3 + 0];
        int32_t dg = (int32_t) color[1] - palette[i * 3 + 1];
        int32_t db = (int32_t) color[2] - palette[i * 3 + 2];
        int32_t distance = dr * dr + dg * dg + db * db;

        if (distance < closest) {
            closest = distance;
            best = i;
        }
    }

    inverse->index[cell] = (int16_t) best;
    return best;
}

#pragma endregion

#pragma region lzw writing

#define GIF_HASH_SLOTS 5021u

/**
 * The compressor's dictionary, keyed by a prefix code and the byte after it.
 *
 * Three arrays rather than one packed word: a key needs twenty bits and a code
 * twelve, which is exactly thirty two with no room left for a value that means
 * empty, and packing them anyway made a full dictionary indistinguishable from
 * an unused slot.
 */
typedef struct {
    uint32_t key[GIF_HASH_SLOTS];
    uint16_t code[GIF_HASH_SLOTS];
    uint8_t used[GIF_HASH_SLOTS];
} LzwDictionary;

typedef struct {
    TinyWriter* out;
    uint32_t accumulator;
    uint32_t count;
    /** The sub-block being filled, which is flushed at 255 bytes. */
    uint8_t block[255];
    uint32_t filled;
} LzwWriter;

static void lzw_flush_block(LzwWriter* writer) {
    if (writer->filled == 0) return;

    tiny_writer_u8(writer->out, (uint8_t) writer->filled);
    tiny_writer_write(writer->out, writer->block, writer->filled);
    writer->filled = 0;
}

static void lzw_put(LzwWriter* writer, uint32_t code, uint32_t width) {
    writer->accumulator |= code << writer->count;
    writer->count += width;

    while (writer->count >= 8) {
        writer->block[writer->filled++] =
            (uint8_t) (writer->accumulator & 0xFFu);
        writer->accumulator >>= 8;
        writer->count -= 8;

        if (writer->filled == 255) lzw_flush_block(writer);
    }
}

static void lzw_finish(LzwWriter* writer) {
    if (writer->count > 0) {
        writer->block[writer->filled++] =
            (uint8_t) (writer->accumulator & 0xFFu);

        if (writer->filled == 255) lzw_flush_block(writer);

        writer->accumulator = 0;
        writer->count = 0;
    }

    lzw_flush_block(writer);
    tiny_writer_u8(writer->out, 0);
}

/**
 * Compresses an index plane, growing the code width in step with a decoder.
 *
 * The two sides do not add dictionary entries at the same moment: an encoder
 * adds one as it emits a code, a decoder only when it reads the code after it,
 * so it is always one entry behind. The width test therefore has to be applied
 * after the entry is added and before the next code is emitted, which is what
 * puts both sides on the same width for the same code.
 */
static void lzw_compress(
    TinyWriter* out, const uint8_t* indices, size_t size, uint32_t code_bits,
    LzwDictionary* dictionary
) {
    LzwWriter writer;
    tiny_memset(&writer, 0, sizeof(writer));
    writer.out = out;

    uint32_t clear = 1u << code_bits;
    uint32_t end = clear + 1;
    uint32_t next = clear + 2;
    uint32_t width = code_bits + 1;

    tiny_memset(dictionary->used, 0, sizeof(dictionary->used));

    lzw_put(&writer, clear, width);

    if (size == 0) {
        lzw_put(&writer, end, width);
        lzw_finish(&writer);
        return;
    }

    uint32_t prefix = indices[0];

    for (size_t at = 1; at < size; at++) {
        uint32_t suffix = indices[at];
        uint32_t key = (prefix << 8) | suffix;

        // open addressing with a second, coprime step, which is the table shape
        // the format's own sample encoder uses
        uint32_t slot = ((key >> 12) ^ key) % GIF_HASH_SLOTS;
        uint32_t step = slot == 0 ? 1 : GIF_HASH_SLOTS - slot;
        int found = 0;

        while (dictionary->used[slot]) {
            if (dictionary->key[slot] == key) {
                prefix = dictionary->code[slot];
                found = 1;
                break;
            }

            slot = slot >= step ? slot - step : slot + GIF_HASH_SLOTS - step;
        }

        if (found) continue;

        lzw_put(&writer, prefix, width);

        if (next < GIF_CODE_LIMIT) {
            dictionary->used[slot] = 1;
            dictionary->key[slot] = key;
            dictionary->code[slot] = (uint16_t) next;

            next++;

            // one more than the decoder's threshold, and the difference is the
            // whole reason this is easy to get wrong: the decoder cannot add an
            // entry until it reads the code after the one that created it, so
            // it is always one behind and switches width one code later
            if (next > (1u << width) && width < GIF_MAX_CODE_BITS) width++;
        }
        else {
            lzw_put(&writer, clear, width);

            tiny_memset(dictionary->used, 0, sizeof(dictionary->used));

            next = clear + 2;
            width = code_bits + 1;
        }

        prefix = suffix;
    }

    lzw_put(&writer, prefix, width);
    lzw_put(&writer, end, width);
    lzw_finish(&writer);
}

#pragma endregion

#pragma region encode

/** A color to palette index map for the case where nothing was discarded. */
#define GIF_EXACT_SLOTS 1024u

typedef struct {
    uint32_t key[GIF_EXACT_SLOTS];
    uint16_t value[GIF_EXACT_SLOTS];
    uint8_t used[GIF_EXACT_SLOTS];
} GifExact;

static uint32_t exact_slot(uint32_t key) {
    // the low bits of a color are the ones that vary, so they are the ones
    // mixed upward before the table is indexed
    key ^= key >> 13;
    key *= 0x9E3779B1u;

    return (key >> 10) % GIF_EXACT_SLOTS;
}

static int exact_find(const GifExact* map, uint32_t key, uint32_t* index) {
    uint32_t slot = exact_slot(key);

    for (uint32_t probe = 0; probe < GIF_EXACT_SLOTS; probe++) {
        if (!map->used[slot]) return 0;

        if (map->key[slot] == key) {
            *index = map->value[slot];
            return 1;
        }

        slot = (slot + 1) % GIF_EXACT_SLOTS;
    }

    return 0;
}

static void exact_insert(GifExact* map, uint32_t key, uint32_t index) {
    uint32_t slot = exact_slot(key);

    for (uint32_t probe = 0; probe < GIF_EXACT_SLOTS; probe++) {
        if (!map->used[slot]) {
            map->used[slot] = 1;
            map->key[slot] = key;
            map->value[slot] = (uint16_t) index;
            return;
        }

        if (map->key[slot] == key) return;

        slot = (slot + 1) % GIF_EXACT_SLOTS;
    }
}

typedef struct {
    uint8_t palette[256 * 3];
    uint32_t count;
    /** Set when the palette holds every color the image had. */
    uint8_t exact;
    /** The index reserved for transparency, or 256 when there is none. */
    uint32_t transparent;
} GifPalette;

/** Reads one source pixel as RGB plus a coverage flag. */
static void source_pixel(
    const TinyImage* image, size_t at, uint8_t* rgb, int* opaque
) {
    const uint8_t* pixel = image->data + at * image->channels;
    uint8_t rgba[4];

    tiny_pixel_convert(rgba, 4, pixel, image->channels);

    rgb[0] = rgba[0];
    rgb[1] = rgba[1];
    rgb[2] = rgba[2];

    // the format has one fully transparent index and no partial transparency,
    // so a coverage decision has to be made somewhere and the midpoint is it
    *opaque = rgba[3] >= 128;
}

/**
 * Chooses the palette, keeping every color when they fit.
 *
 * An image already inside the format's limit is not quantized at all, which is
 * what lets a logo or a flat illustration round trip losslessly. Counting stops
 * as soon as one color too many is seen, so the case that will be quantized
 * pays almost nothing for the attempt.
 */
static int choose_palette(
    const TinyImage* image, GifPalette* out, GifExact* map,
    GifHistogram* histogram, GifBox* boxes
) {
    size_t pixels = (size_t) image->width * image->height;
    int alpha = image->channels == 2 || image->channels == 4;
    int any_clear = 0;

    tiny_memset(out, 0, sizeof(*out));
    tiny_memset(map, 0, sizeof(*map));

    uint32_t room = 256;

    if (alpha) {
        for (size_t at = 0; at < pixels; at++) {
            uint8_t rgb[3];
            int opaque;

            source_pixel(image, at, rgb, &opaque);

            if (!opaque) {
                any_clear = 1;
                break;
            }
        }
    }

    if (any_clear) room = 255;

    uint32_t count = 0;
    int fits = 1;

    for (size_t at = 0; at < pixels && fits; at++) {
        uint8_t rgb[3];
        int opaque;

        source_pixel(image, at, rgb, &opaque);
        if (!opaque) continue;

        uint32_t key =
            ((uint32_t) rgb[0] << 16) | ((uint32_t) rgb[1] << 8) | rgb[2];
        uint32_t index;

        if (exact_find(map, key, &index)) continue;

        if (count >= room) {
            fits = 0;
            break;
        }

        exact_insert(map, key, count);

        out->palette[count * 3 + 0] = rgb[0];
        out->palette[count * 3 + 1] = rgb[1];
        out->palette[count * 3 + 2] = rgb[2];

        count++;
    }

    if (fits) {
        out->count = count;
        out->exact = 1;
        out->transparent = any_clear ? count : 256;

        if (any_clear) {
            out->palette[count * 3 + 0] = 0;
            out->palette[count * 3 + 1] = 0;
            out->palette[count * 3 + 2] = 0;
            out->count = count + 1;
        }

        // a palette has to hold at least two entries for a code size of two
        if (out->count < 2) out->count = 2;

        return TINYIMG_OK;
    }

    tiny_memset(histogram, 0, sizeof(*histogram));

    for (size_t at = 0; at < pixels; at++) {
        uint8_t rgb[3];
        int opaque;

        source_pixel(image, at, rgb, &opaque);
        if (!opaque) continue;

        uint32_t cell = cube_cell(
            (uint32_t) rgb[0] >> (8 - GIF_CUBE_BITS),
            (uint32_t) rgb[1] >> (8 - GIF_CUBE_BITS),
            (uint32_t) rgb[2] >> (8 - GIF_CUBE_BITS)
        );

        histogram->count[cell]++;

        for (uint32_t c = 0; c < 3; c++) histogram->sum[cell][c] += rgb[c];
    }

    uint32_t assigned = median_cut(histogram, room, boxes, out->palette);

    out->count = assigned;
    out->exact = 0;
    out->transparent = 256;

    if (any_clear && assigned < 256) {
        out->palette[assigned * 3 + 0] = 0;
        out->palette[assigned * 3 + 1] = 0;
        out->palette[assigned * 3 + 2] = 0;
        out->transparent = assigned;
        out->count = assigned + 1;
    }

    if (out->count < 2) out->count = 2;

    return TINYIMG_OK;
}

/**
 * Maps every pixel to a palette index, diffusing the error when there was any.
 *
 * Error diffusion is applied when and only when colors had to be discarded.
 * On an image that kept all of its own there is no error to diffuse, and adding
 * noise to a flat illustration that round tripped exactly would be a strange
 * thing to do.
 */
static void map_indices(
    const TinyImage* image, const GifPalette* palette, const GifExact* map,
    GifInverse* inverse, int16_t* errors, uint8_t* indices
) {
    uint32_t width = image->width;
    size_t row_size = ((size_t) width + 2) * 3;

    if (errors) tiny_memset(errors, 0, row_size * 2 * sizeof(int16_t));

    for (uint32_t y = 0; y < image->height; y++) {
        int16_t* current = errors ? errors + (y & 1u) * row_size : 0;
        int16_t* below = errors ? errors + ((y + 1) & 1u) * row_size : 0;

        if (below) tiny_memset(below, 0, row_size * sizeof(int16_t));

        for (uint32_t x = 0; x < width; x++) {
            size_t at = (size_t) y * width + x;

            uint8_t rgb[3];
            int opaque;

            source_pixel(image, at, rgb, &opaque);

            if (!opaque && palette->transparent < 256) {
                indices[at] = (uint8_t) palette->transparent;
                continue;
            }

            uint8_t wanted[3];

            for (uint32_t c = 0; c < 3; c++) {
                int32_t value = rgb[c];

                if (current) value += current[(x + 1) * 3 + c];
                wanted[c] = tiny_clamp_u8(value);
            }

            uint32_t index = 0;

            if (palette->exact) {
                uint32_t key = ((uint32_t) rgb[0] << 16) |
                               ((uint32_t) rgb[1] << 8) | rgb[2];

                if (!exact_find(map, key, &index)) index = 0;
            }
            else {
                index = inverse_lookup(
                    inverse, palette->palette, palette->count,
                    palette->transparent, wanted
                );
            }

            if (index >= palette->count) index = 0;
            indices[at] = (uint8_t) index;

            if (!errors) continue;

            const uint8_t* chosen = palette->palette + index * 3;

            for (uint32_t c = 0; c < 3; c++) {
                int32_t error = (int32_t) wanted[c] - chosen[c];

                // the Floyd-Steinberg weights, over sixteen
                current[(x + 2) * 3 + c] =
                    (int16_t) (current[(x + 2) * 3 + c] + error * 7 / 16);
                below[x * 3 + c] =
                    (int16_t) (below[x * 3 + c] + error * 3 / 16);
                below[(x + 1) * 3 + c] =
                    (int16_t) (below[(x + 1) * 3 + c] + error * 5 / 16);
                below[(x + 2) * 3 + c] =
                    (int16_t) (below[(x + 2) * 3 + c] + error * 1 / 16);
            }
        }
    }
}

static int gif_encode(
    const TinyImage* image, const TinyEncodeOpts* options, TinyWriter* writer
) {
    (void) options;

    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;
    if (image->width > 65535 || image->height > 65535) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    size_t pixels = (size_t) image->width * image->height;

    GifPalette* palette = tiny_arena_alloc(sizeof(GifPalette), 0);
    GifExact* map = tiny_arena_alloc(sizeof(GifExact), 0);
    GifHistogram* histogram = tiny_arena_alloc(sizeof(GifHistogram), 0);
    GifBox* boxes = tiny_arena_alloc(256 * sizeof(GifBox), 0);
    uint8_t* indices = tiny_arena_alloc(pixels, 0);
    LzwDictionary* dictionary = tiny_arena_alloc(sizeof(LzwDictionary), 0);

    if (!palette || !map || !histogram || !boxes || !indices || !dictionary) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    int result = choose_palette(image, palette, map, histogram, boxes);

    if (result != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return result;
    }

    int16_t* errors = 0;
    GifInverse* inverse = 0;

    if (!palette->exact) {
        errors = tiny_arena_alloc(
            ((size_t) image->width + 2) * 3 * 2 * sizeof(int16_t), 0
        );
        inverse = tiny_arena_alloc(sizeof(GifInverse), 0);

        if (!errors || !inverse) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        tiny_memset(inverse->index, 0xFF, sizeof(inverse->index));
    }

    map_indices(image, palette, map, inverse, errors, indices);

    // the table has to be a power of two entries, so the palette is rounded up
    // and the spare entries left black
    uint32_t bits = 1;
    while ((1u << bits) < palette->count) bits++;

    uint32_t entries = 1u << bits;

    tiny_writer_write(writer, "GIF89a", 6);
    tiny_writer_le16(writer, (uint16_t) image->width);
    tiny_writer_le16(writer, (uint16_t) image->height);
    tiny_writer_u8(writer, (uint8_t) (0x80u | ((bits - 1u) & 7u)));
    tiny_writer_u8(writer, 0);
    tiny_writer_u8(writer, 0);

    tiny_writer_write(writer, palette->palette, palette->count * 3);
    tiny_writer_fill(writer, 0, (entries - palette->count) * 3);

    if (palette->transparent < 256) {
        tiny_writer_u8(writer, GIF_EXTENSION);
        tiny_writer_u8(writer, GIF_GRAPHIC_CONTROL);
        tiny_writer_u8(writer, 4);
        tiny_writer_u8(writer, 1);
        tiny_writer_le16(writer, 0);
        tiny_writer_u8(writer, (uint8_t) palette->transparent);
        tiny_writer_u8(writer, 0);
    }

    tiny_writer_u8(writer, GIF_IMAGE);
    tiny_writer_le16(writer, 0);
    tiny_writer_le16(writer, 0);
    tiny_writer_le16(writer, (uint16_t) image->width);
    tiny_writer_le16(writer, (uint16_t) image->height);
    tiny_writer_u8(writer, 0);

    // two is the smallest code size the format defines, whatever the palette
    uint32_t code_bits = bits < 2 ? 2 : bits;

    tiny_writer_u8(writer, (uint8_t) code_bits);
    lzw_compress(writer, indices, pixels, code_bits, dictionary);

    tiny_writer_u8(writer, GIF_TRAILER);

    tiny_arena_release(&mark);

    return writer->error;
}

#pragma endregion

const TinyCodec tiny_codec_gif = {
    TINYIMG_FORMAT_GIF, gif_sniff, gif_probe, gif_decode, gif_encode
};
