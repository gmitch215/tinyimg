#include "tinyimg/codec/png.h"

#include "tinyimg/codec/deflate.h"
#include "tinyimg/memory.h"

#define PNG_SIGNATURE_SIZE 8u
#define PNG_IHDR_SIZE 13u

#define PNG_GRAY 0u
#define PNG_RGB 2u
#define PNG_PALETTE 3u
#define PNG_GRAY_ALPHA 4u
#define PNG_RGBA 6u

/** Chunk types, as the four ASCII bytes packed big endian. */
#define PNG_TYPE(a, b, c, d)                                                   \
    (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | \
     (uint32_t) (d))

#define PNG_IHDR PNG_TYPE('I', 'H', 'D', 'R')
#define PNG_IDAT PNG_TYPE('I', 'D', 'A', 'T')
#define PNG_IEND PNG_TYPE('I', 'E', 'N', 'D')
#define PNG_PLTE PNG_TYPE('P', 'L', 'T', 'E')
#define PNG_TRNS PNG_TYPE('t', 'R', 'N', 'S')

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t depth;
    uint8_t color;
    uint8_t interlace;
    uint8_t channels;
    uint32_t bits_per_pixel;
    size_t stride;
    uint8_t filter_step;

    const uint8_t* palette;
    uint32_t palette_count;
    const uint8_t* transparency;
    uint32_t transparency_size;

    uint8_t out_channels;
    uint8_t has_alpha;

    size_t compressed_size;
} PngHeader;

#pragma region chunks

static inline uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static inline uint32_t read_be16(const uint8_t* p) {
    return ((uint32_t) p[0] << 8) | (uint32_t) p[1];
}

static int png_sniff(const uint8_t* buffer, size_t size) {
    static const uint8_t signature[8] = {0x89, 'P',  'N',  'G',
                                         0x0D, 0x0A, 0x1A, 0x0A};

    return buffer && size >= 8 && tiny_memcmp(buffer, signature, 8) == 0;
}

/**
 * Walks the chunk list once, gathering everything a decode needs.
 *
 * Every chunk's CRC is checked here rather than at use, so a corrupt file is
 * rejected before any of it is trusted.
 */
static int png_parse(const uint8_t* buffer, size_t size, PngHeader* header) {
    if (!png_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    tiny_memset(header, 0, sizeof(*header));

    size_t at = PNG_SIGNATURE_SIZE;
    int seen_header = 0;
    int seen_end = 0;

    while (at + 12 <= size) {
        uint32_t length = read_be32(buffer + at);
        uint32_t type = read_be32(buffer + at + 4);

        if (length > 0x7FFFFFFFu || at + 12 + length > size) {
            return TINYIMG_ERR_CORRUPT;
        }

        const uint8_t* data = buffer + at + 8;
        uint32_t stored = read_be32(data + length);

        if (tiny_crc32(0, buffer + at + 4, 4 + (size_t) length) != stored) {
            return TINYIMG_ERR_CORRUPT;
        }

        switch (type) {
            case PNG_IHDR:
                if (seen_header || length != PNG_IHDR_SIZE) {
                    return TINYIMG_ERR_CORRUPT;
                }

                header->width = read_be32(data);
                header->height = read_be32(data + 4);
                header->depth = data[8];
                header->color = data[9];
                header->interlace = data[12];

                // the only compression method and filter method the format
                // defines
                if (data[10] != 0 || data[11] != 0) {
                    return TINYIMG_ERR_UNSUPPORTED_VARIANT;
                }
                if (header->interlace > 1) return TINYIMG_ERR_CORRUPT;

                seen_header = 1;
                break;

            case PNG_PLTE:
                if (length % 3 != 0 || length > 256 * 3) {
                    return TINYIMG_ERR_CORRUPT;
                }
                header->palette = data;
                header->palette_count = length / 3;
                break;

            case PNG_TRNS:
                header->transparency = data;
                header->transparency_size = length;
                break;

            case PNG_IDAT: header->compressed_size += length; break;

            case PNG_IEND: seen_end = 1; break;

            default: break;
        }

        at += 12 + (size_t) length;
        if (seen_end) break;
    }

    if (!seen_header) return TINYIMG_ERR_CORRUPT;
    if (header->width == 0 || header->height == 0) return TINYIMG_ERR_CORRUPT;

    switch (header->color) {
        case PNG_GRAY:
            header->channels = 1;
            if (header->depth != 1 && header->depth != 2 &&
                header->depth != 4 && header->depth != 8 &&
                header->depth != 16) {
                return TINYIMG_ERR_CORRUPT;
            }
            break;
        case PNG_RGB:
            header->channels = 3;
            if (header->depth != 8 && header->depth != 16) {
                return TINYIMG_ERR_CORRUPT;
            }
            break;
        case PNG_PALETTE:
            header->channels = 1;
            if (header->depth != 1 && header->depth != 2 &&
                header->depth != 4 && header->depth != 8) {
                return TINYIMG_ERR_CORRUPT;
            }
            if (!header->palette) return TINYIMG_ERR_CORRUPT;
            break;
        case PNG_GRAY_ALPHA:
            header->channels = 2;
            if (header->depth != 8 && header->depth != 16) {
                return TINYIMG_ERR_CORRUPT;
            }
            break;
        case PNG_RGBA:
            header->channels = 4;
            if (header->depth != 8 && header->depth != 16) {
                return TINYIMG_ERR_CORRUPT;
            }
            break;
        default: return TINYIMG_ERR_CORRUPT;
    }

    if ((uint64_t) header->width * header->height > TINYIMG_MAX_PIXELS) {
        return TINYIMG_ERR_TOO_LARGE;
    }
    if (header->compressed_size == 0) return TINYIMG_ERR_CORRUPT;

    header->bits_per_pixel = (uint32_t) header->channels * header->depth;
    header->stride = ((size_t) header->width * header->bits_per_pixel + 7) / 8;

    // the filter's left neighbor is a whole pixel back, or one byte when a
    // pixel is narrower
    header->filter_step =
        (uint8_t) (header->bits_per_pixel >= 8 ? header->bits_per_pixel / 8
                                               : 1);

    header->has_alpha = header->color == PNG_GRAY_ALPHA ||
                        header->color == PNG_RGBA || header->transparency != 0;

    if (header->color == PNG_GRAY || header->color == PNG_GRAY_ALPHA) {
        header->out_channels = header->has_alpha ? 2 : 1;
    }
    else {
        header->out_channels = header->has_alpha ? 4 : 3;
    }

    return TINYIMG_OK;
}

static int png_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    PngHeader header;
    int result = png_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    info->width = header.width;
    info->height = header.height;
    info->frames = 1;
    info->format = TINYIMG_FORMAT_PNG;
    info->channels = header.out_channels;
    info->bit_depth = header.depth;
    info->has_alpha = header.has_alpha;

    // Adam7 is the format's own name for a progressive layout, and it carries
    // the same consequence: a region cannot be streamed out of it
    info->progressive = header.interlace;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region filters

static inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int32_t p = (int32_t) a + (int32_t) b - (int32_t) c;

    int32_t pa = p > a ? p - a : a - p;
    int32_t pb = p > b ? p - b : b - p;
    int32_t pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/**
 * Reverses one row's filter in place.
 *
 * @param row The filtered bytes, which become the unfiltered ones.
 * @param previous The row above, already unfiltered, or NULL for the first row.
 * @param size Bytes in the row.
 * @param step Bytes to the left neighbor.
 * @param filter Which filter the row was written with.
 * @return int TINYIMG_OK, or TINYIMG_ERR_CORRUPT for a filter the format does
 * not define.
 */
static int unfilter(
    uint8_t* row, const uint8_t* previous, size_t size, uint8_t step,
    uint8_t filter
) {
    switch (filter) {
        case 0: break;

        case 1:
            for (size_t i = step; i < size; i++) {
                row[i] = (uint8_t) (row[i] + row[i - step]);
            }
            break;

        case 2:
            if (previous) {
                for (size_t i = 0; i < size; i++) {
                    row[i] = (uint8_t) (row[i] + previous[i]);
                }
            }
            break;

        case 3:
            for (size_t i = 0; i < size; i++) {
                uint32_t left = i >= step ? row[i - step] : 0;
                uint32_t up = previous ? previous[i] : 0;

                row[i] = (uint8_t) (row[i] + ((left + up) >> 1));
            }
            break;

        case 4:
            for (size_t i = 0; i < size; i++) {
                uint8_t left = i >= step ? row[i - step] : 0;
                uint8_t up = previous ? previous[i] : 0;
                uint8_t corner =
                    (previous && i >= step) ? previous[i - step] : 0;

                row[i] = (uint8_t) (row[i] + paeth(left, up, corner));
            }
            break;

        default: return TINYIMG_ERR_CORRUPT;
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region sample expansion

/** Reads one sample of the file's bit depth out of a row. */
static inline uint32_t sample_at(
    const uint8_t* row, uint32_t index, uint8_t depth
) {
    if (depth == 8) return row[index];
    if (depth == 16) return read_be16(row + index * 2);

    uint32_t per_byte = 8u / depth;
    uint32_t shift = (per_byte - 1u - (index % per_byte)) * depth;

    return (row[index / per_byte] >> shift) & ((1u << depth) - 1u);
}

/** Scales a sample of the file's depth up to a full 8 bit channel. */
static inline uint8_t scale_sample(uint32_t value, uint8_t depth) {
    switch (depth) {
        case 1: return value ? 255 : 0;
        case 2: return (uint8_t) (value * 85u);
        case 4: return (uint8_t) (value * 17u);
        // the high byte, which is what every viewer takes and is exact for a
        // value that was an 8 bit one scaled up
        case 16: return (uint8_t) (value >> 8);
        default: return (uint8_t) value;
    }
}

/**
 * Expands one source pixel into RGBA, whatever the color type and depth.
 *
 * Every path goes through here, so the twelve combinations of color type,
 * depth and transparency are interpreted in one place.
 */
static void expand_pixel(
    const PngHeader* header, const uint8_t* row, uint32_t x, uint8_t* pixel
) {
    {
        pixel[3] = 255;

        switch (header->color) {
            case PNG_GRAY: {
                uint32_t raw = sample_at(row, x, header->depth);
                uint8_t gray = scale_sample(raw, header->depth);

                pixel[0] = gray;
                pixel[1] = gray;
                pixel[2] = gray;

                if (header->transparency_size >= 2 &&
                    raw == read_be16(header->transparency)) {
                    pixel[3] = 0;
                }
                break;
            }

            case PNG_RGB: {
                for (uint32_t c = 0; c < 3; c++) {
                    pixel[c] = scale_sample(
                        sample_at(row, x * 3 + c, header->depth), header->depth
                    );
                }

                if (header->transparency_size >= 6) {
                    uint32_t r = sample_at(row, x * 3, header->depth);
                    uint32_t g = sample_at(row, x * 3 + 1, header->depth);
                    uint32_t b = sample_at(row, x * 3 + 2, header->depth);

                    if (r == read_be16(header->transparency) &&
                        g == read_be16(header->transparency + 2) &&
                        b == read_be16(header->transparency + 4)) {
                        pixel[3] = 0;
                    }
                }
                break;
            }

            case PNG_PALETTE: {
                uint32_t index = sample_at(row, x, header->depth);
                if (index >= header->palette_count) index = 0;

                const uint8_t* entry = header->palette + index * 3;
                pixel[0] = entry[0];
                pixel[1] = entry[1];
                pixel[2] = entry[2];

                if (index < header->transparency_size) {
                    pixel[3] = header->transparency[index];
                }
                break;
            }

            case PNG_GRAY_ALPHA: {
                uint8_t gray = scale_sample(
                    sample_at(row, x * 2, header->depth), header->depth
                );

                pixel[0] = gray;
                pixel[1] = gray;
                pixel[2] = gray;
                pixel[3] = scale_sample(
                    sample_at(row, x * 2 + 1, header->depth), header->depth
                );
                break;
            }

            default:
                for (uint32_t c = 0; c < 4; c++) {
                    pixel[c] = scale_sample(
                        sample_at(row, x * 4 + c, header->depth), header->depth
                    );
                }
                break;
        }
    }
}

static void expand_row(
    const PngHeader* header, const uint8_t* row, uint32_t width, uint8_t* out
) {
    for (uint32_t x = 0; x < width; x++) {
        expand_pixel(header, row, x, out + (size_t) x * 4);
    }
}

/**
 * Writes one output row straight from an unfiltered source row.
 *
 * The unscaled path, which is what almost every request uses. Routing it
 * through the box accumulator instead costs four integer divisions and five
 * additions per pixel to average a box of one, and that showed up as most of
 * the row handling time.
 */
static void convert_row(
    const PngHeader* header, const uint8_t* row, uint32_t x0, uint32_t width,
    uint8_t channels, uint8_t* dest
) {
    // when the file already holds exactly what was asked for, the row is the
    // answer. a palette never qualifies: its one byte per pixel is an index,
    // not a sample, so copying it would write the indices out as gray
    if (header->depth == 8 && !header->transparency &&
        header->color != PNG_PALETTE && channels == header->channels) {
        tiny_memcpy(
            dest, row + (size_t) x0 * channels, (size_t) width * channels
        );
        return;
    }

    for (uint32_t i = 0; i < width; i++) {
        uint8_t rgba[4];

        expand_pixel(header, row, x0 + i, rgba);
        tiny_pixel_convert(dest + (size_t) i * channels, channels, rgba, 4);
    }
}

#pragma endregion

#pragma region decode

/** Adam7's seven passes, as first column, first row, column step and row step.
 */
static const uint8_t adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8},
                                    {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                                    {0, 1, 1, 2}};

/** Joins every IDAT chunk into one buffer, which is what the inflater needs. */
static uint8_t* gather_idat(
    const uint8_t* buffer, size_t size, const PngHeader* header
) {
    uint8_t* joined = tiny_arena_alloc(header->compressed_size, 0);
    if (!joined) return 0;

    size_t at = PNG_SIGNATURE_SIZE;
    size_t written = 0;

    while (at + 12 <= size) {
        uint32_t length = read_be32(buffer + at);
        uint32_t type = read_be32(buffer + at + 4);

        if (type == PNG_IDAT) {
            tiny_memcpy(joined + written, buffer + at + 8, length);
            written += length;
        }

        at += 12 + (size_t) length;
        if (type == PNG_IEND) break;
    }

    return joined;
}

/** Pulls exactly `size` bytes, since a row is only usable whole. */
static int read_exact(TinyInflate* state, uint8_t* out, size_t size) {
    size_t have = 0;

    while (have < size) {
        size_t want = size - have;
        if (want > TINY_DEFLATE_MAX_READ) want = TINY_DEFLATE_MAX_READ;

        long read = tiny_inflate_read(state, out + have, want);
        if (read < 0) return (int) read;
        if (read == 0) return TINYIMG_ERR_CORRUPT;

        have += (size_t) read;
    }

    return TINYIMG_OK;
}

typedef struct {
    uint32_t* sums;
    uint32_t* counts;
    uint32_t width;
} PngAccumulator;

static void accumulate(
    PngAccumulator* box, const uint8_t* rgba, const TinyDecodeOpts* opts,
    uint32_t den
) {
    for (uint32_t ox = 0; ox < box->width; ox++) {
        uint32_t left = opts->x + ox * den;
        uint32_t right = left + den;
        if (right > opts->x + opts->width) right = opts->x + opts->width;

        for (uint32_t sx = left; sx < right; sx++) {
            const uint8_t* pixel = rgba + (size_t) sx * 4;

            box->sums[ox * 4 + 0] += pixel[0];
            box->sums[ox * 4 + 1] += pixel[1];
            box->sums[ox * 4 + 2] += pixel[2];
            box->sums[ox * 4 + 3] += pixel[3];
            box->counts[ox]++;
        }
    }
}

static void emit_row(
    const PngAccumulator* box, uint8_t* dest, uint8_t channels
) {
    for (uint32_t ox = 0; ox < box->width; ox++) {
        uint32_t n = box->counts[ox] ? box->counts[ox] : 1;
        uint8_t rgba[4] = {
            (uint8_t) ((box->sums[ox * 4 + 0] + n / 2) / n),
            (uint8_t) ((box->sums[ox * 4 + 1] + n / 2) / n),
            (uint8_t) ((box->sums[ox * 4 + 2] + n / 2) / n),
            (uint8_t) ((box->sums[ox * 4 + 3] + n / 2) / n),
        };

        tiny_pixel_convert(dest + (size_t) ox * channels, channels, rgba, 4);
    }
}

static void clear_accumulator(PngAccumulator* box) {
    tiny_memset(box->sums, 0, (size_t) box->width * 4 * sizeof(uint32_t));
    tiny_memset(box->counts, 0, (size_t) box->width * sizeof(uint32_t));
}

/**
 * Assembles an Adam7 image, whose rows arrive spread across seven passes.
 *
 * The whole plane has to exist before any region of it can be read, so this is
 * the one path that cannot stream. The intermediate is RGBA at eight bits,
 * which is also what the sampling loop wants, so nothing is expanded twice.
 */
static int decode_interlaced(
    const PngHeader* header, TinyInflate* state, uint8_t* plane
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    size_t widest = (size_t) header->width * header->bits_per_pixel / 8 + 8;

    uint8_t* current = tiny_arena_alloc(widest, 0);
    uint8_t* previous = tiny_arena_alloc(widest, 0);
    uint8_t* rgba = tiny_arena_alloc((size_t) header->width * 4, 0);

    int result = TINYIMG_OK;

    if (!current || !previous || !rgba) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (uint32_t pass = 0; pass < 7 && result == TINYIMG_OK; pass++) {
        uint32_t first_x = adam7[pass][0];
        uint32_t first_y = adam7[pass][1];
        uint32_t step_x = adam7[pass][2];
        uint32_t step_y = adam7[pass][3];

        if (first_x >= header->width || first_y >= header->height) continue;

        uint32_t pass_width = (header->width - first_x + step_x - 1) / step_x;
        uint32_t pass_height = (header->height - first_y + step_y - 1) / step_y;

        size_t stride = ((size_t) pass_width * header->bits_per_pixel + 7) / 8;
        tiny_memset(previous, 0, stride);

        for (uint32_t row = 0; row < pass_height; row++) {
            uint8_t filter = 0;

            result = read_exact(state, &filter, 1);
            if (result != TINYIMG_OK) break;

            result = read_exact(state, current, stride);
            if (result != TINYIMG_OK) break;

            result = unfilter(
                current, row == 0 ? 0 : previous, stride, header->filter_step,
                filter
            );
            if (result != TINYIMG_OK) break;

            expand_row(header, current, pass_width, rgba);

            uint32_t y = first_y + row * step_y;
            for (uint32_t i = 0; i < pass_width; i++) {
                uint32_t x = first_x + i * step_x;

                tiny_memcpy(
                    plane + ((size_t) y * header->width + x) * 4,
                    rgba + (size_t) i * 4, 4
                );
            }

            uint8_t* swap = previous;
            previous = current;
            current = swap;
        }
    }

    tiny_arena_release(&mark);
    return result;
}

static int png_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* opts
) {
    PngHeader header;
    int result = png_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    TinyDecodeOpts resolved;
    uint32_t out_width;
    uint32_t out_height;

    result = tiny_decode_resolve(
        opts, header.width, header.height, &resolved, &out_width, &out_height
    );
    if (result != TINYIMG_OK) return result;

    uint8_t channels =
        resolved.channels ? resolved.channels : header.out_channels;

    result = tiny_image_create(image, out_width, out_height, channels);
    if (result != TINYIMG_OK) return result;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* joined = gather_idat(buffer, size, &header);
    TinyInflate state;

    if (!joined) {
        result = TINYIMG_ERR_MEMORY;
    }
    else {
        result = tiny_inflate_init(&state, joined, header.compressed_size, 1);
    }

    PngAccumulator box;
    box.width = out_width;
    box.sums = tiny_arena_alloc((size_t) out_width * 4 * sizeof(uint32_t), 0);
    box.counts = tiny_arena_alloc((size_t) out_width * sizeof(uint32_t), 0);

    uint8_t* rgba = tiny_arena_alloc((size_t) header.width * 4, 0);

    if (result == TINYIMG_OK && (!box.sums || !box.counts || !rgba)) {
        result = TINYIMG_ERR_MEMORY;
    }

    uint32_t den = resolved.scale_den;

    if (result == TINYIMG_OK && header.interlace) {
        uint8_t* plane =
            tiny_arena_alloc((size_t) header.width * header.height * 4, 0);

        if (!plane) {
            result = TINYIMG_ERR_MEMORY;
        }
        else {
            result = decode_interlaced(&header, &state, plane);

            for (uint32_t oy = 0; oy < out_height && result == TINYIMG_OK;
                 oy++) {
                uint8_t* dest =
                    image->data + (size_t) oy * out_width * channels;

                // the plane is already RGBA, so an unscaled row only needs the
                // channel count adjusting
                if (den == 1) {
                    const uint8_t* source =
                        plane + ((size_t) (resolved.y + oy) * header.width +
                                 resolved.x) *
                                    4;

                    for (uint32_t ox = 0; ox < out_width; ox++) {
                        tiny_pixel_convert(
                            dest + (size_t) ox * channels, channels,
                            source + (size_t) ox * 4, 4
                        );
                    }
                    continue;
                }

                clear_accumulator(&box);

                uint32_t top = resolved.y + oy * den;
                uint32_t bottom = top + den;
                if (bottom > resolved.y + resolved.height) {
                    bottom = resolved.y + resolved.height;
                }

                for (uint32_t sy = top; sy < bottom; sy++) {
                    accumulate(
                        &box, plane + (size_t) sy * header.width * 4, &resolved,
                        den
                    );
                }

                emit_row(&box, dest, channels);
            }
        }
    }
    else if (result == TINYIMG_OK) {
        uint8_t* current = tiny_arena_alloc(header.stride, 0);
        uint8_t* previous = tiny_arena_alloc(header.stride, 0);

        if (!current || !previous) {
            result = TINYIMG_ERR_MEMORY;
        }
        else {
            tiny_memset(previous, 0, header.stride);

            uint32_t last = resolved.y + resolved.height;
            uint32_t oy = 0;

            clear_accumulator(&box);

            // every row up to the region's last one has to be unfiltered,
            // because a filter refers to the row above it; past that, nothing
            // more is read at all
            for (uint32_t y = 0; y < last && result == TINYIMG_OK; y++) {
                uint8_t filter = 0;

                result = read_exact(&state, &filter, 1);
                if (result != TINYIMG_OK) break;

                result = read_exact(&state, current, header.stride);
                if (result != TINYIMG_OK) break;

                result = unfilter(
                    current, y == 0 ? 0 : previous, header.stride,
                    header.filter_step, filter
                );
                if (result != TINYIMG_OK) break;

                if (y >= resolved.y) {
                    uint8_t* dest =
                        image->data + (size_t) oy * out_width * channels;

                    if (den == 1) {
                        convert_row(
                            &header, current, resolved.x, out_width, channels,
                            dest
                        );
                        oy++;
                    }
                    else {
                        expand_row(&header, current, header.width, rgba);
                        accumulate(&box, rgba, &resolved, den);

                        if ((y - resolved.y + 1) % den == 0 || y + 1 == last) {
                            emit_row(&box, dest, channels);
                            oy++;
                            clear_accumulator(&box);
                        }
                    }
                }

                uint8_t* swap = previous;
                previous = current;
                current = swap;
            }
        }
    }

    tiny_arena_release(&mark);

    if (result != TINYIMG_OK) {
        tiny_image_destroy(image);
        return result;
    }

    image->format = TINYIMG_FORMAT_PNG;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region encode

static void write_chunk(
    TinyWriter* writer, uint32_t type, const uint8_t* data, size_t size
) {
    uint8_t tag[4] = {
        (uint8_t) (type >> 24), (uint8_t) (type >> 16), (uint8_t) (type >> 8),
        (uint8_t) type
    };

    tiny_writer_be32(writer, (uint32_t) size);
    tiny_writer_write(writer, tag, 4);
    tiny_writer_write(writer, data, size);

    uint32_t crc = tiny_crc32(0, tag, 4);
    crc = tiny_crc32(crc, data, size);

    tiny_writer_be32(writer, crc);
}

/** Applies one filter to a row, writing into `out`. */
static void apply_filter(
    const uint8_t* row, const uint8_t* previous, size_t size, uint8_t step,
    uint8_t filter, uint8_t* out
) {
    for (size_t i = 0; i < size; i++) {
        uint8_t left = i >= step ? row[i - step] : 0;
        uint8_t up = previous ? previous[i] : 0;
        uint8_t corner = (previous && i >= step) ? previous[i - step] : 0;

        switch (filter) {
            case 0: out[i] = row[i]; break;
            case 1: out[i] = (uint8_t) (row[i] - left); break;
            case 2: out[i] = (uint8_t) (row[i] - up); break;
            case 3:
                out[i] = (uint8_t) (row[i] - (((uint32_t) left + up) >> 1));
                break;
            default:
                out[i] = (uint8_t) (row[i] - paeth(left, up, corner));
                break;
        }
    }
}

/** Sum of absolute differences from zero, the heuristic libpng uses to pick a
 * filter. */
static uint32_t filter_cost(const uint8_t* row, size_t size) {
    uint32_t total = 0;

    for (size_t i = 0; i < size; i++) {
        total += row[i] < 128 ? row[i] : (uint32_t) (256 - row[i]);
    }
    return total;
}

/**
 * Builds the filtered stream that gets compressed: a filter byte then a row,
 * repeated.
 *
 * @param adaptive Non-zero to choose a filter per row, zero to leave every row
 * unfiltered.
 */
static int build_stream(const TinyImage* image, int adaptive, TinyWriter* raw) {
    size_t stride = (size_t) image->width * image->channels;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* candidate = tiny_arena_alloc(stride, 0);
    uint8_t* best = tiny_arena_alloc(stride, 0);

    if (!candidate || !best) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (uint32_t y = 0; y < image->height; y++) {
        const uint8_t* row = image->data + (size_t) y * stride;

        if (!adaptive) {
            tiny_writer_u8(raw, 0);
            tiny_writer_write(raw, row, stride);
            continue;
        }

        const uint8_t* previous =
            y == 0 ? 0 : image->data + (size_t) (y - 1) * stride;

        uint8_t chosen = 0;
        uint32_t lowest = 0xFFFFFFFFu;

        for (uint8_t filter = 0; filter < 5; filter++) {
            apply_filter(
                row, previous, stride, image->channels, filter, candidate
            );

            uint32_t cost = filter_cost(candidate, stride);
            if (cost < lowest) {
                lowest = cost;
                chosen = filter;
                tiny_memcpy(best, candidate, stride);
            }
        }

        tiny_writer_u8(raw, chosen);
        tiny_writer_write(raw, best, stride);
    }

    tiny_arena_release(&mark);
    return raw->error;
}

/**
 * Compresses one candidate stream and keeps it only if it beats what is already
 * held.
 *
 * @param keep Receives the smaller of the two, taking ownership of its buffer.
 */
static int try_stream(
    const TinyImage* image, int adaptive, TinyDeflateLevel level,
    TinyWriter* keep
) {
    size_t stride = (size_t) image->width * image->channels;

    TinyWriter raw;
    tiny_writer_init(&raw, (stride + 1) * image->height);

    int result = build_stream(image, adaptive, &raw);

    if (result == TINYIMG_OK) {
        TinyWriter compressed;
        tiny_writer_init(&compressed, raw.size / 2 + 64);

        result = tiny_deflate(raw.data, raw.size, level, 1, &compressed);

        if (result == TINYIMG_OK &&
            (keep->size == 0 || compressed.size < keep->size)) {
            tiny_writer_free(keep);
            *keep = compressed;
        }
        else {
            tiny_writer_free(&compressed);
        }
    }

    tiny_writer_free(&raw);
    return result;
}

static int png_encode(
    const TinyImage* image, const TinyEncodeOpts* opts, TinyWriter* writer
) {
    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;
    if (image->channels == 0 || image->channels > 4) return TINYIMG_ERR_RANGE;

    static const uint8_t color_for[5] = {
        0, PNG_GRAY, PNG_GRAY_ALPHA, PNG_RGB, PNG_RGBA
    };

    uint8_t color = color_for[image->channels];

    static const uint8_t signature[8] = {0x89, 'P',  'N',  'G',
                                         0x0D, 0x0A, 0x1A, 0x0A};
    tiny_writer_write(writer, signature, 8);

    uint8_t ihdr[PNG_IHDR_SIZE];
    ihdr[0] = (uint8_t) (image->width >> 24);
    ihdr[1] = (uint8_t) (image->width >> 16);
    ihdr[2] = (uint8_t) (image->width >> 8);
    ihdr[3] = (uint8_t) image->width;
    ihdr[4] = (uint8_t) (image->height >> 24);
    ihdr[5] = (uint8_t) (image->height >> 16);
    ihdr[6] = (uint8_t) (image->height >> 8);
    ihdr[7] = (uint8_t) image->height;
    ihdr[8] = 8;
    ihdr[9] = color;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    write_chunk(writer, PNG_IHDR, ihdr, sizeof(ihdr));

    TinyDeflateLevel level = opts && opts->quality >= 90
                                 ? TINYIMG_DEFLATE_BEST
                                 : TINYIMG_DEFLATE_DEFAULT;

    // both candidates are compressed and the smaller is kept. the per row
    // filter heuristic scores a row by how close its bytes are to zero, which
    // estimates entropy but says nothing about whether LZ77 could have matched
    // the row against its neighbors. on flat artwork that is the wrong
    // question and leaving every row unfiltered comes out up to 40% smaller; on
    // a photograph it is the right one and unfiltered is 17% worse. measured on
    // both, with no signal short of compressing that separates them, so both
    // get compressed
    TinyWriter compressed;
    tiny_writer_init(&compressed, 0);

    int result = try_stream(image, 1, level, &compressed);
    if (result == TINYIMG_OK) result = try_stream(image, 0, level, &compressed);

    if (result == TINYIMG_OK) {
        // one IDAT is legal at any size, but splitting keeps a reader's own
        // chunk buffer small and is what every encoder does
        size_t at = 0;
        while (at < compressed.size) {
            size_t take = compressed.size - at;
            if (take > 65536) take = 65536;

            write_chunk(writer, PNG_IDAT, compressed.data + at, take);
            at += take;
        }
    }

    tiny_writer_free(&compressed);

    if (result != TINYIMG_OK) return result;

    write_chunk(writer, PNG_IEND, 0, 0);
    return writer->error;
}

#pragma endregion

const TinyCodec tiny_codec_png = {
    TINYIMG_FORMAT_PNG, png_sniff, png_probe, png_decode, png_encode
};
