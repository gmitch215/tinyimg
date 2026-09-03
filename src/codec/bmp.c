#include "tinyimg/codec/bmp.h"

#include "tinyimg/memory.h"

#define BMP_FILE_HEADER 14u
#define BMP_INFO_HEADER 40u
#define BMP_V4_HEADER 108u

#define BI_RGB 0u
#define BI_RLE8 1u
#define BI_BITFIELDS 3u

typedef struct {
    uint32_t width;
    uint32_t height;
    uint16_t bpp;
    uint32_t compression;
    uint32_t data_offset;
    uint32_t header_size;
    uint32_t stride;
    uint32_t palette_count;
    const uint8_t* palette;
    uint32_t mask[4];
    int top_down;
    uint8_t channels;
} BmpHeader;

#pragma region header

static inline uint16_t read_le16(const uint8_t* p) {
    return (uint16_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8));
}

static inline uint32_t read_le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static inline uint32_t mask_shift(uint32_t mask) {
    if (mask == 0) return 0;

    uint32_t shift = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static inline uint32_t mask_width(uint32_t mask) {
    uint32_t bits = 0;
    while (mask != 0) {
        bits += mask & 1u;
        mask >>= 1;
    }
    return bits;
}

static int bmp_sniff(const uint8_t* buffer, size_t size) {
    return buffer && size >= 2 && buffer[0] == 'B' && buffer[1] == 'M';
}

static int bmp_parse(const uint8_t* buffer, size_t size, BmpHeader* header) {
    if (!bmp_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;
    if (size < BMP_FILE_HEADER + BMP_INFO_HEADER) return TINYIMG_ERR_CORRUPT;

    tiny_memset(header, 0, sizeof(*header));

    header->data_offset = read_le32(buffer + 10);
    header->header_size = read_le32(buffer + 14);

    // the 12 byte OS/2 header puts the dimensions elsewhere and is the only
    // layout this rejects outright
    if (header->header_size < BMP_INFO_HEADER) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }
    if ((size_t) BMP_FILE_HEADER + header->header_size > size) {
        return TINYIMG_ERR_CORRUPT;
    }

    int32_t raw_width = (int32_t) read_le32(buffer + 18);
    int32_t raw_height = (int32_t) read_le32(buffer + 22);

    if (raw_width <= 0 || raw_height == 0) return TINYIMG_ERR_CORRUPT;

    header->top_down = raw_height < 0;
    header->width = (uint32_t) raw_width;
    header->height = header->top_down ? (uint32_t) (-(int64_t) raw_height)
                                      : (uint32_t) raw_height;

    header->bpp = read_le16(buffer + 28);
    header->compression = read_le32(buffer + 30);
    header->palette_count = read_le32(buffer + 46);

    if ((uint64_t) header->width * header->height > TINYIMG_MAX_PIXELS) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    switch (header->bpp) {
        case 1:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32: break;
        default: return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    if (header->compression == BI_RLE8) {
        if (header->bpp != 8 || header->top_down) {
            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
        }
    }
    else if (header->compression == BI_BITFIELDS) {
        if (header->bpp != 16 && header->bpp != 32) {
            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
        }
    }
    else if (header->compression != BI_RGB) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    if (header->compression == BI_BITFIELDS) {
        // a 40 byte header keeps the masks in a block right after it and a
        // larger one carries them inside, which lands them at the same offset
        // either way
        uint32_t mask_offset = BMP_FILE_HEADER + BMP_INFO_HEADER;
        if ((size_t) mask_offset + 12 > size) return TINYIMG_ERR_CORRUPT;

        header->mask[0] = read_le32(buffer + mask_offset);
        header->mask[1] = read_le32(buffer + mask_offset + 4);
        header->mask[2] = read_le32(buffer + mask_offset + 8);

        if (header->header_size >= 56u && (size_t) mask_offset + 16 <= size) {
            header->mask[3] = read_le32(buffer + mask_offset + 12);
        }
    }
    else if (header->bpp == 16) {
        header->mask[0] = 0x7C00u;
        header->mask[1] = 0x03E0u;
        header->mask[2] = 0x001Fu;
    }
    else if (header->bpp == 32) {
        header->mask[0] = 0x00FF0000u;
        header->mask[1] = 0x0000FF00u;
        header->mask[2] = 0x000000FFu;

        // the fourth byte of a plain 32 bit BI_RGB row is padding, so alpha is
        // only read when a V4 or later header declares a mask for it
        if (header->header_size >= BMP_V4_HEADER) {
            uint32_t alpha = read_le32(buffer + BMP_FILE_HEADER + 52u);
            if (alpha != 0) header->mask[3] = alpha;
        }
    }

    if (header->compression == BI_BITFIELDS && header->mask[0] == 0 &&
        header->mask[1] == 0 && header->mask[2] == 0) {
        return TINYIMG_ERR_CORRUPT;
    }

    if (header->bpp <= 8) {
        uint32_t maximum = 1u << header->bpp;
        if (header->palette_count == 0 || header->palette_count > maximum) {
            header->palette_count = maximum;
        }

        // only depths of 8 and below reach here, and those are never
        // BI_BITFIELDS, so the palette always follows the header directly
        uint32_t offset = BMP_FILE_HEADER + header->header_size;

        if ((size_t) offset + (size_t) header->palette_count * 4 > size) {
            return TINYIMG_ERR_CORRUPT;
        }
        header->palette = buffer + offset;
    }

    header->stride =
        (((uint32_t) header->bpp * header->width + 31u) / 32u) * 4u;
    header->channels = header->mask[3] != 0 ? 4 : 3;

    if (header->data_offset >= size) return TINYIMG_ERR_CORRUPT;

    if (header->compression == BI_RGB) {
        uint64_t needed = (uint64_t) header->stride * header->height;
        if ((uint64_t) header->data_offset + needed > size) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    return TINYIMG_OK;
}

static int bmp_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    BmpHeader header;
    int result = bmp_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    info->width = header.width;
    info->height = header.height;
    info->frames = 1;
    info->format = TINYIMG_FORMAT_BMP;
    info->channels = header.channels;
    info->bit_depth = 8;
    info->has_alpha = header.mask[3] != 0;
    info->progressive = 0;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region decode

typedef struct {
    uint32_t shift[4];
    uint32_t maximum[4];
} BmpUnpack;

static void unpack_init(const BmpHeader* header, BmpUnpack* unpack) {
    for (uint32_t i = 0; i < 4; i++) {
        unpack->shift[i] = mask_shift(header->mask[i]);

        uint32_t bits = mask_width(header->mask[i]);
        unpack->maximum[i] = bits > 0 ? (1u << bits) - 1u : 0;
    }
}

static inline uint8_t scale_to_8(uint32_t value, uint32_t maximum) {
    if (maximum == 255u) return (uint8_t) value;
    if (maximum == 0) return 255;

    return (uint8_t) ((value * 255u + maximum / 2u) / maximum);
}

// one source pixel to RGBA, whatever the file's depth and layout
static void bmp_pixel(
    const BmpHeader* header, const BmpUnpack* unpack, const uint8_t* row,
    uint32_t x, uint8_t* out
) {
    if (header->bpp <= 8) {
        uint32_t index;

        if (header->bpp == 8) {
            index = row[x];
        }
        else if (header->bpp == 4) {
            index = (x & 1u) ? (row[x >> 1] & 0x0Fu) : (row[x >> 1] >> 4);
        }
        else {
            index = (row[x >> 3] >> (7u - (x & 7u))) & 1u;
        }

        if (index >= header->palette_count) index = 0;

        const uint8_t* entry = header->palette + index * 4u;
        out[0] = entry[2];
        out[1] = entry[1];
        out[2] = entry[0];
        out[3] = 255;
        return;
    }

    if (header->bpp == 24) {
        const uint8_t* pixel = row + x * 3u;
        out[0] = pixel[2];
        out[1] = pixel[1];
        out[2] = pixel[0];
        out[3] = 255;
        return;
    }

    uint32_t raw = header->bpp == 16 ? (uint32_t) read_le16(row + x * 2u)
                                     : read_le32(row + x * 4u);

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = (raw & header->mask[i]) >> unpack->shift[i];
        out[i] = scale_to_8(value, unpack->maximum[i]);
    }

    if (header->mask[3] != 0) {
        uint32_t value = (raw & header->mask[3]) >> unpack->shift[3];
        out[3] = scale_to_8(value, unpack->maximum[3]);
    }
    else {
        out[3] = 255;
    }
}

// RLE8 has to be walked from the start, so a region of one costs the whole
// index plane; every other layout is random access and reads only what it needs
static uint8_t* rle8_expand(
    const uint8_t* buffer, size_t size, const BmpHeader* header
) {
    size_t plane_size = (size_t) header->width * header->height;

    uint8_t* plane = tiny_alloc(plane_size);
    if (!plane) return 0;

    tiny_memset(plane, 0, plane_size);

    const uint8_t* in = buffer + header->data_offset;
    const uint8_t* end = buffer + size;

    uint32_t x = 0;
    uint32_t y = 0;

    while (in + 1 < end && y < header->height) {
        uint8_t count = in[0];
        uint8_t value = in[1];
        in += 2;

        if (count > 0) {
            for (uint32_t i = 0; i < count && x < header->width; i++, x++) {
                plane[(size_t) y * header->width + x] = value;
            }
            continue;
        }

        if (value == 0) {
            x = 0;
            y++;
        }
        else if (value == 1) {
            break;
        }
        else if (value == 2) {
            if (in + 1 >= end) break;

            x += in[0];
            y += in[1];
            in += 2;
        }
        else {
            for (uint32_t i = 0; i < value; i++) {
                if (in >= end) break;

                if (x < header->width) {
                    plane[(size_t) y * header->width + x] = *in;
                    x++;
                }
                in++;
            }
            // absolute runs are padded to an even byte count
            if (value & 1u) in++;
        }
    }

    return plane;
}

static int bmp_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* opts
) {
    BmpHeader header;
    int result = bmp_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    TinyDecodeOpts resolved;
    uint32_t out_width;
    uint32_t out_height;

    result = tiny_decode_resolve(
        opts, header.width, header.height, &resolved, &out_width, &out_height
    );
    if (result != TINYIMG_OK) return result;

    uint8_t channels = resolved.channels ? resolved.channels : header.channels;

    result = tiny_image_create(image, out_width, out_height, channels);
    if (result != TINYIMG_OK) return result;

    BmpUnpack unpack;
    unpack_init(&header, &unpack);

    uint8_t* plane = 0;
    BmpHeader source = header;

    if (header.compression == BI_RLE8) {
        plane = rle8_expand(buffer, size, &header);
        if (!plane) {
            tiny_image_destroy(image);
            return TINYIMG_ERR_MEMORY;
        }

        // the plane is an unpadded 8 bit image in the same row order the
        // compressed rows arrived in, so only the stride changes
        source.compression = BI_RGB;
        source.stride = header.width;
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t* sums =
        tiny_arena_alloc((size_t) out_width * 4 * sizeof(uint32_t), 0);
    uint32_t* counts =
        tiny_arena_alloc((size_t) out_width * sizeof(uint32_t), 0);

    if (!sums || !counts) {
        tiny_arena_release(&mark);
        tiny_free(plane);
        tiny_image_destroy(image);
        return TINYIMG_ERR_MEMORY;
    }

    uint32_t den = resolved.scale_den;

    for (uint32_t oy = 0; oy < out_height; oy++) {
        tiny_memset(sums, 0, (size_t) out_width * 4 * sizeof(uint32_t));
        tiny_memset(counts, 0, (size_t) out_width * sizeof(uint32_t));

        uint32_t block_top = resolved.y + oy * den;
        uint32_t block_bottom = block_top + den;
        if (block_bottom > resolved.y + resolved.height) {
            block_bottom = resolved.y + resolved.height;
        }

        for (uint32_t sy = block_top; sy < block_bottom; sy++) {
            uint32_t file_row = source.top_down ? sy : source.height - 1u - sy;

            const uint8_t* row = plane
                                     ? plane + (size_t) file_row * source.stride
                                     : buffer + source.data_offset +
                                           (size_t) file_row * source.stride;

            for (uint32_t ox = 0; ox < out_width; ox++) {
                uint32_t block_left = resolved.x + ox * den;
                uint32_t block_right = block_left + den;
                if (block_right > resolved.x + resolved.width) {
                    block_right = resolved.x + resolved.width;
                }

                for (uint32_t sx = block_left; sx < block_right; sx++) {
                    uint8_t rgba[4];
                    bmp_pixel(&source, &unpack, row, sx, rgba);

                    sums[ox * 4 + 0] += rgba[0];
                    sums[ox * 4 + 1] += rgba[1];
                    sums[ox * 4 + 2] += rgba[2];
                    sums[ox * 4 + 3] += rgba[3];
                    counts[ox]++;
                }
            }
        }

        uint8_t* dest = image->data + (size_t) oy * out_width * channels;

        for (uint32_t ox = 0; ox < out_width; ox++) {
            uint32_t n = counts[ox] ? counts[ox] : 1;
            uint8_t rgba[4] = {
                (uint8_t) ((sums[ox * 4 + 0] + n / 2) / n),
                (uint8_t) ((sums[ox * 4 + 1] + n / 2) / n),
                (uint8_t) ((sums[ox * 4 + 2] + n / 2) / n),
                (uint8_t) ((sums[ox * 4 + 3] + n / 2) / n),
            };

            tiny_pixel_convert(
                dest + (size_t) ox * channels, channels, rgba, 4
            );
        }
    }

    tiny_arena_release(&mark);
    tiny_free(plane);

    image->format = TINYIMG_FORMAT_BMP;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region encode

static int bmp_encode(
    const TinyImage* image, const TinyEncodeOpts* opts, TinyWriter* writer
) {
    (void) opts;

    if (!image || !image->data) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;

    int has_alpha = image->channels == 2 || image->channels == 4;
    int greyscale = image->channels == 1;

    uint16_t bpp = has_alpha ? 32 : (greyscale ? 8 : 24);
    uint32_t header_size = has_alpha ? BMP_V4_HEADER : BMP_INFO_HEADER;
    uint32_t palette_bytes = greyscale ? 1024u : 0u;
    uint32_t stride = (((uint32_t) bpp * image->width + 31u) / 32u) * 4u;

    uint32_t offset = BMP_FILE_HEADER + header_size + palette_bytes;
    uint64_t pixels = (uint64_t) stride * image->height;
    if (offset + pixels > 0xFFFFFFFFu) return TINYIMG_ERR_TOO_LARGE;

    tiny_writer_reserve(writer, (size_t) (offset + pixels));

    tiny_writer_u8(writer, 'B');
    tiny_writer_u8(writer, 'M');
    tiny_writer_le32(writer, (uint32_t) (offset + pixels));
    tiny_writer_le16(writer, 0);
    tiny_writer_le16(writer, 0);
    tiny_writer_le32(writer, offset);

    tiny_writer_le32(writer, header_size);
    tiny_writer_le32(writer, image->width);
    tiny_writer_le32(writer, image->height);
    tiny_writer_le16(writer, 1);
    tiny_writer_le16(writer, bpp);
    tiny_writer_le32(writer, has_alpha ? BI_BITFIELDS : BI_RGB);
    tiny_writer_le32(writer, (uint32_t) pixels);
    tiny_writer_le32(writer, 0);
    tiny_writer_le32(writer, 0);
    tiny_writer_le32(writer, greyscale ? 256u : 0u);
    tiny_writer_le32(writer, 0);

    if (has_alpha) {
        tiny_writer_le32(writer, 0x00FF0000u);
        tiny_writer_le32(writer, 0x0000FF00u);
        tiny_writer_le32(writer, 0x000000FFu);
        tiny_writer_le32(writer, 0xFF000000u);
        tiny_writer_le32(writer, 0x73524742u); // 'sRGB'
        tiny_writer_fill(writer, 0, 36);       // endpoints, ignored for sRGB
        tiny_writer_le32(writer, 0);
        tiny_writer_le32(writer, 0);
        tiny_writer_le32(writer, 0);
    }

    if (greyscale) {
        for (uint32_t i = 0; i < 256; i++) {
            uint8_t entry[4] = {(uint8_t) i, (uint8_t) i, (uint8_t) i, 0};
            tiny_writer_write(writer, entry, sizeof(entry));
        }
    }

    uint32_t used = bpp / 8u * image->width;
    uint32_t padding = stride - used;

    for (uint32_t y = 0; y < image->height; y++) {
        // rows go out bottom-up, which is what every reader expects
        const uint8_t* row = image->data + (size_t) (image->height - 1u - y) *
                                               image->width * image->channels;

        for (uint32_t x = 0; x < image->width; x++) {
            const uint8_t* pixel = row + (size_t) x * image->channels;

            if (greyscale) {
                tiny_writer_u8(writer, pixel[0]);
                continue;
            }

            uint8_t rgba[4];
            tiny_pixel_convert(rgba, 4, pixel, image->channels);

            uint8_t bgra[4] = {rgba[2], rgba[1], rgba[0], rgba[3]};
            tiny_writer_write(writer, bgra, has_alpha ? 4u : 3u);
        }

        if (padding > 0) tiny_writer_fill(writer, 0, padding);
    }

    return writer->error;
}

#pragma endregion

const TinyCodec tiny_codec_bmp = {
    TINYIMG_FORMAT_BMP, bmp_sniff, bmp_probe, bmp_decode, bmp_encode
};
