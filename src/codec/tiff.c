#include "tinyimg/codec/tiff.h"

#include "tinyimg/codec/deflate.h"
#include "tinyimg/codec/lzw.h"
#include "tinyimg/memory.h"

#define TIFF_TAG_WIDTH 256u
#define TIFF_TAG_HEIGHT 257u
#define TIFF_TAG_BITS 258u
#define TIFF_TAG_COMPRESSION 259u
#define TIFF_TAG_PHOTOMETRIC 262u
#define TIFF_TAG_STRIP_OFFSETS 273u
#define TIFF_TAG_SAMPLES 277u
#define TIFF_TAG_ROWS_PER_STRIP 278u
#define TIFF_TAG_STRIP_BYTES 279u
#define TIFF_TAG_PLANAR 284u
#define TIFF_TAG_PREDICTOR 317u
#define TIFF_TAG_COLOUR_MAP 320u
#define TIFF_TAG_TILE_WIDTH 322u
#define TIFF_TAG_EXTRA_SAMPLES 338u
#define TIFF_TAG_SAMPLE_FORMAT 339u

#define TIFF_COMPRESS_NONE 1u
#define TIFF_COMPRESS_LZW 5u
#define TIFF_COMPRESS_DEFLATE_OLD 8u
#define TIFF_COMPRESS_PACKBITS 32773u
#define TIFF_COMPRESS_DEFLATE 32946u

#define TIFF_WHITE_IS_ZERO 0u
#define TIFF_BLACK_IS_ZERO 1u
#define TIFF_RGB 2u
#define TIFF_PALETTE 3u

typedef struct {
    const uint8_t* data;
    size_t size;
    int big;

    uint32_t width;
    uint32_t height;
    uint32_t samples;
    uint32_t bits;
    uint32_t compression;
    uint32_t photometric;
    uint32_t rows_per_strip;
    uint32_t predictor;
    uint32_t planar;
    uint32_t extra;

    /** Where the strip tables are, and how many entries each holds. */
    size_t offsets_at;
    uint32_t offsets_type;
    uint32_t offsets_count;
    size_t counts_at;
    uint32_t counts_type;
    uint32_t counts_count;

    /** The colour map, as an offset and an entry count per channel. */
    size_t map_at;
    uint32_t map_entries;

    /** How many directories the file chains, which is its page count. */
    uint32_t pages;

    uint32_t out_channels;
    uint8_t has_alpha;
    size_t stride;
    uint32_t strips;
} TiffHeader;

#pragma region directory

static uint32_t read16(const TiffHeader* header, size_t at) {
    const uint8_t* p = header->data + at;

    return header->big ? (((uint32_t) p[0] << 8) | p[1])
                       : (((uint32_t) p[1] << 8) | p[0]);
}

static uint32_t read32(const TiffHeader* header, size_t at) {
    const uint8_t* p = header->data + at;

    if (header->big) {
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
               ((uint32_t) p[2] << 8) | p[3];
    }

    return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[1] << 8) | p[0];
}

static uint32_t type_size(uint32_t type) {
    switch (type) {
        case 1:
        case 2:
        case 6:
        case 7: return 1;
        case 3:
        case 8: return 2;
        case 4:
        case 9:
        case 11: return 4;
        case 5:
        case 10:
        case 12: return 8;
        default: return 0;
    }
}

/** Reads one element of a tag's value array, whatever width it was stored at.
 */
static uint32_t element(
    const TiffHeader* header, size_t at, uint32_t type, uint32_t index
) {
    size_t width = type_size(type);
    size_t position = at + (size_t) index * width;

    if (position + width > header->size) return 0;

    switch (width) {
        case 1: return header->data[position];
        case 2: return read16(header, position);
        default: return read32(header, position);
    }
}

static int tiff_sniff(const uint8_t* buffer, size_t size) {
    if (!buffer || size < 8) return 0;

    if (buffer[0] == 'I' && buffer[1] == 'I' && buffer[2] == 0x2A &&
        buffer[3] == 0x00) {
        return 1;
    }

    return buffer[0] == 'M' && buffer[1] == 'M' && buffer[2] == 0x00 &&
           buffer[3] == 0x2A;
}

/**
 * Reads the first directory, and counts the rest without reading them.
 *
 * A tag's value sits inside its entry when it fits in four bytes and at an
 * offset when it does not, so an array of one is indistinguishable from a
 * scalar and both are handled by taking the address of the value field either
 * way.
 */
static int tiff_parse(const uint8_t* buffer, size_t size, TiffHeader* header) {
    if (!tiff_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    tiny_memset(header, 0, sizeof(*header));

    header->data = buffer;
    header->size = size;
    header->big = buffer[0] == 'M';

    // the baseline defaults, which a file only overrides when it differs
    header->samples = 1;
    header->bits = 1;
    header->compression = TIFF_COMPRESS_NONE;
    header->photometric = 0xFFFFFFFFu;
    header->rows_per_strip = 0xFFFFFFFFu;
    header->predictor = 1;
    header->planar = 1;

    uint32_t directory = read32(header, 4);
    int tiled = 0;
    int first = 1;

    while (directory != 0) {
        if (directory + 2 > size) return TINYIMG_ERR_CORRUPT;

        uint32_t count = read16(header, directory);
        size_t entries = directory + 2;

        if (entries + (size_t) count * 12 + 4 > size) {
            return TINYIMG_ERR_CORRUPT;
        }

        header->pages++;

        if (first) {
            for (uint32_t i = 0; i < count; i++) {
                size_t entry = entries + (size_t) i * 12;

                uint32_t tag = read16(header, entry);
                uint32_t type = read16(header, entry + 2);
                uint32_t length = read32(header, entry + 4);
                size_t value = entry + 8;

                // past four bytes the field holds an offset instead
                if (type_size(type) * (size_t) length > 4) {
                    value = read32(header, value);
                    if (value >= size) return TINYIMG_ERR_CORRUPT;
                }

                switch (tag) {
                    case TIFF_TAG_WIDTH:
                        header->width = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_HEIGHT:
                        header->height = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_SAMPLES:
                        header->samples = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_COMPRESSION:
                        header->compression = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_PHOTOMETRIC:
                        header->photometric = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_ROWS_PER_STRIP:
                        header->rows_per_strip =
                            element(header, value, type, 0);
                        break;
                    case TIFF_TAG_PREDICTOR:
                        header->predictor = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_PLANAR:
                        header->planar = element(header, value, type, 0);
                        break;
                    case TIFF_TAG_EXTRA_SAMPLES:
                        header->extra = element(header, value, type, 0);
                        break;

                    case TIFF_TAG_BITS:
                        // one entry per sample, and this codec only reads files
                        // where they agree
                        header->bits = element(header, value, type, 0);

                        for (uint32_t s = 1; s < length; s++) {
                            if (element(header, value, type, s) !=
                                header->bits) {
                                return TINYIMG_ERR_UNSUPPORTED_VARIANT;
                            }
                        }
                        break;

                    case TIFF_TAG_STRIP_OFFSETS:
                        header->offsets_at = value;
                        header->offsets_type = type;
                        header->offsets_count = length;
                        break;

                    case TIFF_TAG_STRIP_BYTES:
                        header->counts_at = value;
                        header->counts_type = type;
                        header->counts_count = length;
                        break;

                    case TIFF_TAG_COLOUR_MAP:
                        header->map_at = value;
                        header->map_entries = length / 3;
                        break;

                    case TIFF_TAG_TILE_WIDTH: tiled = 1; break;

                    case TIFF_TAG_SAMPLE_FORMAT:
                        // 1 is unsigned integer; anything else is legal and not
                        // baseline
                        if (element(header, value, type, 0) > 1) {
                            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
                        }
                        break;

                    default: break;
                }
            }

            first = 0;
        }

        uint32_t next = read32(header, entries + (size_t) count * 12);

        // a directory chain that points backward or at itself would not
        // terminate, and a file can be built that way
        if (next != 0 && next <= directory) break;
        directory = next;
    }

    if (header->width == 0 || header->height == 0) return TINYIMG_ERR_CORRUPT;
    if (header->photometric == 0xFFFFFFFFu) return TINYIMG_ERR_CORRUPT;
    if (!header->offsets_at || !header->counts_at) return TINYIMG_ERR_CORRUPT;

    if (header->rows_per_strip == 0) return TINYIMG_ERR_CORRUPT;
    if (header->rows_per_strip > header->height) {
        header->rows_per_strip = header->height;
    }

    header->strips =
        (header->height + header->rows_per_strip - 1) / header->rows_per_strip;

    if (header->offsets_count < header->strips ||
        header->counts_count < header->strips) {
        return TINYIMG_ERR_CORRUPT;
    }

    switch (header->photometric) {
        case TIFF_WHITE_IS_ZERO:
        case TIFF_BLACK_IS_ZERO:
            header->out_channels = header->samples >= 2 ? 2u : 1u;
            break;
        case TIFF_PALETTE:
            if (!header->map_at) return TINYIMG_ERR_CORRUPT;
            header->out_channels = 3;
            break;
        case TIFF_RGB:
            header->out_channels = header->samples >= 4 ? 4u : 3u;
            break;
        default: return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    header->has_alpha =
        (uint8_t) (header->out_channels == 2 || header->out_channels == 4);

    if (tiled) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (header->planar != 1) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (header->samples == 0 || header->samples > 4) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }
    if (header->predictor != 1 && header->predictor != 2) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    // eight bits per sample is the whole of what this reads. One, four and
    // sixteen are all baseline or near it, and each needs its own unpacking
    if (header->bits != 8) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    switch (header->compression) {
        case TIFF_COMPRESS_NONE:
        case TIFF_COMPRESS_LZW:
        case TIFF_COMPRESS_DEFLATE_OLD:
        case TIFF_COMPRESS_PACKBITS:
        case TIFF_COMPRESS_DEFLATE: break;
        default: return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    header->stride = (size_t) header->width * header->samples;

    return TINYIMG_OK;
}

static int tiff_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    TiffHeader header;
    int result = tiff_parse(buffer, size, &header);

    // a variant this build cannot decode still has a readable directory, which
    // is what a probe is for
    if (result != TINYIMG_OK && result != TINYIMG_ERR_UNSUPPORTED_VARIANT) {
        return result;
    }

    info->width = header.width;
    info->height = header.height;
    info->frames = header.pages ? header.pages : 1;
    info->format = TINYIMG_FORMAT_TIFF;
    info->channels = (uint8_t) (header.out_channels ? header.out_channels : 3);
    info->bit_depth = (uint8_t) header.bits;
    info->has_alpha = header.has_alpha;
    info->progressive = 0;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region strips

/**
 * Expands one PackBits run length encoded strip.
 *
 * The format's own description is in terms of a signed count, which is why the
 * byte is read as one: 0 to 127 is that many plus one literals, -1 to -127 is
 * one byte repeated, and -128 is a no-op nobody emits but everybody has to
 * skip.
 */
static int unpack_bits(
    const uint8_t* in, size_t size, uint8_t* out, size_t capacity
) {
    size_t read = 0;
    size_t written = 0;

    while (read < size && written < capacity) {
        int8_t count = (int8_t) in[read++];

        if (count >= 0) {
            size_t run = (size_t) count + 1;

            if (read + run > size) run = size - read;
            if (written + run > capacity) run = capacity - written;

            tiny_memcpy(out + written, in + read, run);

            read += (size_t) count + 1;
            written += run;
            continue;
        }

        if (count == -128) continue;
        if (read >= size) break;

        size_t run = (size_t) (1 - count);
        uint8_t value = in[read++];

        if (written + run > capacity) run = capacity - written;

        tiny_memset(out + written, value, run);
        written += run;
    }

    return TINYIMG_OK;
}

/** Undoes horizontal differencing, which is per channel and per row. */
static void unpredict(
    uint8_t* rows, size_t stride, uint32_t count, uint32_t samples
) {
    for (uint32_t row = 0; row < count; row++) {
        uint8_t* at = rows + (size_t) row * stride;

        for (size_t i = samples; i < stride; i++) {
            at[i] = (uint8_t) (at[i] + at[i - samples]);
        }
    }
}

/**
 * Decompresses one strip into the row buffer.
 *
 * @param header The parsed directory.
 * @param strip Which strip to read.
 * @param rows Destination, at least `rows_per_strip * stride` bytes.
 * @param capacity Bytes the destination holds.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int read_strip(
    const TiffHeader* header, uint32_t strip, uint8_t* rows, size_t capacity
) {
    uint32_t offset =
        element(header, header->offsets_at, header->offsets_type, strip);
    uint32_t bytes =
        element(header, header->counts_at, header->counts_type, strip);

    if (offset >= header->size) return TINYIMG_ERR_CORRUPT;
    if ((size_t) offset + bytes > header->size) {
        bytes = (uint32_t) (header->size - offset);
    }

    const uint8_t* in = header->data + offset;

    // a strip that decompresses short leaves the rest of the buffer as it was,
    // so a truncated file gives back the rows that did arrive
    tiny_memset(rows, 0, capacity);

    int result = TINYIMG_OK;

    switch (header->compression) {
        case TIFF_COMPRESS_NONE: {
            size_t take = bytes < capacity ? bytes : capacity;
            tiny_memcpy(rows, in, take);
            break;
        }

        case TIFF_COMPRESS_PACKBITS:
            result = unpack_bits(in, bytes, rows, capacity);
            break;

        case TIFF_COMPRESS_LZW: {
            TinyArenaMark mark;
            tiny_arena_mark(&mark);

            TinyLzwTable* table = tiny_arena_alloc(sizeof(TinyLzwTable), 0);

            if (!table) {
                tiny_arena_release(&mark);
                return TINYIMG_ERR_MEMORY;
            }

            TinyLzwReader reader;

            // unframed and most significant bit first, and it widens a code one
            // entry early, which is this format's departure from GIF's
            tiny_lzw_init(&reader, in, bytes, 0, 0, 1);

            result = tiny_lzw_expand(&reader, table, 8, 1, rows, capacity);

            tiny_arena_release(&mark);
            break;
        }

        default: {
            TinyArenaMark mark;
            tiny_arena_mark(&mark);

            TinyWriter out;
            result = tiny_writer_init(&out, capacity);

            if (result == TINYIMG_OK) {
                // both of this format's Deflate tags carry a zlib wrapper
                result = tiny_inflate_all(in, bytes, 1, &out);

                if (result == TINYIMG_OK) {
                    size_t take = out.size < capacity ? out.size : capacity;
                    tiny_memcpy(rows, out.data, take);
                }

                tiny_writer_free(&out);
            }

            tiny_arena_release(&mark);
            break;
        }
    }

    if (result != TINYIMG_OK) return result;

    if (header->predictor == 2) {
        uint32_t rows_here = header->rows_per_strip;
        uint32_t first = strip * header->rows_per_strip;

        if (first + rows_here > header->height) {
            rows_here = header->height - first;
        }

        unpredict(rows, header->stride, rows_here, header->samples);
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region decode

/** Reads one source pixel through the photometric interpretation into RGBA. */
static void expand_pixel(
    const TiffHeader* header, const uint8_t* row, uint32_t x, uint8_t* pixel
) {
    const uint8_t* sample = row + (size_t) x * header->samples;

    pixel[3] = 255;

    switch (header->photometric) {
        case TIFF_WHITE_IS_ZERO: {
            // the polarity is the whole difference from the other grey mode,
            // and a file that gets it wrong looks like a negative
            uint8_t grey = (uint8_t) (255u - sample[0]);

            pixel[0] = grey;
            pixel[1] = grey;
            pixel[2] = grey;
            if (header->samples >= 2) pixel[3] = sample[1];
            break;
        }

        case TIFF_BLACK_IS_ZERO:
            pixel[0] = sample[0];
            pixel[1] = sample[0];
            pixel[2] = sample[0];
            if (header->samples >= 2) pixel[3] = sample[1];
            break;

        case TIFF_PALETTE: {
            uint32_t index = sample[0];

            if (index >= header->map_entries) index = 0;

            // three runs of one channel each, not interleaved triples, and
            // every value is sixteen bits whatever the sample depth
            for (uint32_t c = 0; c < 3; c++) {
                size_t at = header->map_at +
                            ((size_t) c * header->map_entries + index) * 2;

                // the correctly rounded reduction, not the high byte. PNG can
                // take the high byte because its 16 bit samples are an 8 bit
                // value times 257, so it comes back exactly; a colour map is
                // scaled through whatever range the writer used, and truncating
                // it loses a level on most entries
                pixel[c] = (uint8_t) ((read16(header, at) + 128u) / 257u);
            }
            break;
        }

        default:
            pixel[0] = sample[0];
            pixel[1] = header->samples >= 2 ? sample[1] : sample[0];
            pixel[2] = header->samples >= 3 ? sample[2] : sample[0];
            if (header->samples >= 4) pixel[3] = sample[3];
            break;
    }
}

static int tiff_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* options
) {
    TiffHeader header;
    int result = tiff_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    TinyDecodeOpts resolved;
    uint32_t out_width;
    uint32_t out_height;

    result = tiny_decode_resolve(
        options, header.width, header.height, &resolved, &out_width, &out_height
    );
    if (result != TINYIMG_OK) return result;

    uint8_t channels =
        resolved.channels ? resolved.channels : (uint8_t) header.out_channels;

    result = tiny_image_create(image, out_width, out_height, channels);
    if (result != TINYIMG_OK) return result;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint64_t buffered = (uint64_t) header.stride * header.rows_per_strip;

    if (buffered > 0xFFFFFFFFu) {
        tiny_arena_release(&mark);
        tiny_image_destroy(image);
        return TINYIMG_ERR_MEMORY;
    }

    uint8_t* rows = tiny_arena_alloc((size_t) buffered, 0);
    uint32_t* sums =
        tiny_arena_alloc((size_t) out_width * 4 * sizeof(uint32_t), 0);
    uint32_t* counts =
        tiny_arena_alloc((size_t) out_width * sizeof(uint32_t), 0);

    if (!rows || !sums || !counts) {
        tiny_arena_release(&mark);
        tiny_image_destroy(image);
        return TINYIMG_ERR_MEMORY;
    }

    uint32_t den = resolved.scale_den;
    uint32_t last = resolved.y + resolved.height;

    // the strip a row belongs to is read when that row is wanted and remembered
    // until a different one is, so a region reads only the strips it touches.
    // That is what strips are for: each is compressed independently, so unlike
    // a PNG row or a GIF code stream there is nothing to scan through to reach
    // one
    uint32_t loaded = 0xFFFFFFFFu;

    for (uint32_t oy = 0; oy < out_height && result == TINYIMG_OK; oy++) {
        uint8_t* dest = image->data + (size_t) oy * out_width * channels;

        tiny_memset(sums, 0, (size_t) out_width * 4 * sizeof(uint32_t));
        tiny_memset(counts, 0, (size_t) out_width * sizeof(uint32_t));

        uint32_t top = resolved.y + oy * den;
        uint32_t bottom = top + den;

        if (bottom > last) bottom = last;

        for (uint32_t y = top; y < bottom && result == TINYIMG_OK; y++) {
            uint32_t strip = y / header.rows_per_strip;

            if (strip != loaded) {
                result = read_strip(&header, strip, rows, (size_t) buffered);
                if (result != TINYIMG_OK) break;

                loaded = strip;
            }

            const uint8_t* row =
                rows + (size_t) (y % header.rows_per_strip) * header.stride;

            for (uint32_t ox = 0; ox < out_width; ox++) {
                uint32_t left = resolved.x + ox * den;
                uint32_t right = left + den;

                if (right > resolved.x + resolved.width) {
                    right = resolved.x + resolved.width;
                }

                for (uint32_t x = left; x < right; x++) {
                    uint8_t rgba[4];

                    expand_pixel(&header, row, x, rgba);

                    for (uint32_t c = 0; c < 4; c++)
                        sums[ox * 4 + c] += rgba[c];
                    counts[ox]++;
                }
            }
        }

        if (result != TINYIMG_OK) break;

        for (uint32_t ox = 0; ox < out_width; ox++) {
            uint32_t n = counts[ox] ? counts[ox] : 1;
            uint8_t rgba[4];

            for (uint32_t c = 0; c < 4; c++) {
                rgba[c] = (uint8_t) ((sums[ox * 4 + c] + n / 2) / n);
            }

            tiny_pixel_convert(
                dest + (size_t) ox * channels, channels, rgba, 4
            );
        }
    }

    tiny_arena_release(&mark);

    if (result != TINYIMG_OK) {
        tiny_image_destroy(image);
        return result;
    }

    image->format = TINYIMG_FORMAT_TIFF;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region encode

/** One directory entry, whose value is inline when it fits in four bytes. */
static void write_entry(
    TinyWriter* out, uint16_t tag, uint16_t type, uint32_t count, uint32_t value
) {
    tiny_writer_le16(out, tag);
    tiny_writer_le16(out, type);
    tiny_writer_le32(out, count);

    // a short is written into the low half of the field, which is where a
    // little endian reader looks for it
    if (type == 3 && count == 1) {
        tiny_writer_le16(out, (uint16_t) value);
        tiny_writer_le16(out, 0);
        return;
    }

    tiny_writer_le32(out, value);
}

/**
 * Applies horizontal differencing to one row, in place.
 *
 * Backward, because each output depends on the input to its left: forward would
 * subtract a value that had already been replaced.
 */
static void predict(uint8_t* row, size_t stride, uint32_t samples) {
    for (size_t i = stride; i-- > samples;) {
        row[i] = (uint8_t) (row[i] - row[i - samples]);
    }
}

static int tiff_encode(
    const TinyImage* image, const TinyEncodeOpts* options, TinyWriter* writer
) {
    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t samples = image->channels;
    size_t stride = (size_t) image->width * samples;

    // the same mapping PNG uses, so a quality number means one thing across
    // both lossless formats rather than something format specific
    TinyDeflateLevel level = options && options->quality >= 90
                                 ? TINYIMG_DEFLATE_BEST
                                 : TINYIMG_DEFLATE_DEFAULT;

    /*
     * Strips of about 64 KiB, which is what the specification recommends.
     *
     * It costs compression, because each strip restarts the window: measured
     * against one strip over the whole image the difference is a few percent,
     * and against ImageMagick's single strip 3 to 15%. What it buys is the
     * reason strips exist at all, that a region decode reads only the strips it
     * touches instead of everything up to them.
     */
    uint32_t rows_per_strip = (uint32_t) (65536u / (stride ? stride : 1));

    if (rows_per_strip == 0) rows_per_strip = 1;
    if (rows_per_strip > image->height) rows_per_strip = image->height;

    uint32_t strips = (image->height + rows_per_strip - 1) / rows_per_strip;

    uint8_t* scratch = tiny_arena_alloc(stride * rows_per_strip, 0);
    uint32_t* offsets = tiny_arena_alloc((size_t) strips * 4, 0);
    uint32_t* counts = tiny_arena_alloc((size_t) strips * 4, 0);

    if (!scratch || !offsets || !counts) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_writer_write(writer, "II", 2);
    tiny_writer_le16(writer, 42);
    tiny_writer_le32(writer, 8);

    // the strips come first and the directory after them, so their offsets are
    // known by the time the directory is written
    for (uint32_t strip = 0; strip < strips; strip++) {
        uint32_t first = strip * rows_per_strip;
        uint32_t rows = rows_per_strip;

        if (first + rows > image->height) rows = image->height - first;

        for (uint32_t row = 0; row < rows; row++) {
            uint8_t* target = scratch + (size_t) row * stride;

            tiny_memcpy(
                target, image->data + (size_t) (first + row) * stride, stride
            );
            predict(target, stride, samples);
        }

        offsets[strip] = (uint32_t) writer->size;

        int result =
            tiny_deflate(scratch, (size_t) rows * stride, level, 1, writer);

        if (result != TINYIMG_OK) {
            tiny_arena_release(&mark);
            return result;
        }

        counts[strip] = (uint32_t) writer->size - offsets[strip];
    }

    // the two strip tables, when they are too long to sit inside their entries
    uint32_t offsets_at = (uint32_t) writer->size;

    if (strips > 1) {
        for (uint32_t strip = 0; strip < strips; strip++) {
            tiny_writer_le32(writer, offsets[strip]);
        }
    }

    uint32_t counts_at = (uint32_t) writer->size;

    if (strips > 1) {
        for (uint32_t strip = 0; strip < strips; strip++) {
            tiny_writer_le32(writer, counts[strip]);
        }
    }

    uint32_t bits_at = (uint32_t) writer->size;

    if (samples > 2) {
        for (uint32_t i = 0; i < samples; i++) tiny_writer_le16(writer, 8);
    }

    uint32_t directory_at = (uint32_t) writer->size;
    uint32_t entries = samples >= 4 || samples == 2 ? 11u : 10u;

    tiny_writer_le16(writer, (uint16_t) entries);

    write_entry(writer, TIFF_TAG_WIDTH, 3, 1, image->width);
    write_entry(writer, TIFF_TAG_HEIGHT, 3, 1, image->height);
    write_entry(
        writer, TIFF_TAG_BITS, 3, samples,
        samples > 2 ? bits_at : (samples == 2 ? (8u | (8u << 16)) : 8u)
    );
    write_entry(writer, TIFF_TAG_COMPRESSION, 3, 1, TIFF_COMPRESS_DEFLATE);
    write_entry(
        writer, TIFF_TAG_PHOTOMETRIC, 3, 1,
        samples >= 3 ? TIFF_RGB : TIFF_BLACK_IS_ZERO
    );
    write_entry(
        writer, TIFF_TAG_STRIP_OFFSETS, 4, strips,
        strips > 1 ? offsets_at : offsets[0]
    );
    write_entry(writer, TIFF_TAG_SAMPLES, 3, 1, samples);
    write_entry(writer, TIFF_TAG_ROWS_PER_STRIP, 3, 1, rows_per_strip);
    write_entry(
        writer, TIFF_TAG_STRIP_BYTES, 4, strips,
        strips > 1 ? counts_at : counts[0]
    );
    write_entry(writer, TIFF_TAG_PREDICTOR, 3, 1, 2);

    // an alpha channel has to be declared, or a reader treats it as an unknown
    // extra sample and drops it
    if (samples == 2 || samples >= 4) {
        write_entry(writer, TIFF_TAG_EXTRA_SAMPLES, 3, 1, 2);
    }

    tiny_writer_le32(writer, 0);

    // the header's pointer to the directory, now that its offset is known
    if (writer->data && writer->size >= 8) {
        writer->data[4] = (uint8_t) (directory_at & 0xFFu);
        writer->data[5] = (uint8_t) ((directory_at >> 8) & 0xFFu);
        writer->data[6] = (uint8_t) ((directory_at >> 16) & 0xFFu);
        writer->data[7] = (uint8_t) ((directory_at >> 24) & 0xFFu);
    }

    tiny_arena_release(&mark);

    return writer->error;
}

#pragma endregion

const TinyCodec tiny_codec_tiff = {
    TINYIMG_FORMAT_TIFF, tiff_sniff, tiff_probe, tiff_decode, tiff_encode
};
