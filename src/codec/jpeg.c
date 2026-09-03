#include "tinyimg/codec/jpeg.h"

#include "tinyimg/memory.h"

#define JPEG_MAX_COMPONENTS 4
#define JPEG_FAST_BITS 8

/**
 * The largest dequantized coefficient the transforms accept.
 *
 * An 8 bit source cannot produce a coefficient past about 1310, so this only
 * engages on a stream carrying values no image could have been encoded from,
 * where every affected pixel clamps at the output anyway. It is not a quality
 * choice: without it the column pass of the integer transform overflows a
 * signed 32 bit accumulator, which is undefined behaviour rather than merely
 * wrong.
 */
#define JPEG_COEF_LIMIT 4096

#define JPEG_SOF0 0xC0u
#define JPEG_SOF1 0xC1u
#define JPEG_SOF2 0xC2u
#define JPEG_SOF3 0xC3u
#define JPEG_DHT 0xC4u
#define JPEG_SOF5 0xC5u
#define JPEG_SOF7 0xC7u
#define JPEG_JPG 0xC8u
#define JPEG_SOF9 0xC9u
#define JPEG_SOF11 0xCBu
#define JPEG_DAC 0xCCu
#define JPEG_SOF13 0xCDu
#define JPEG_SOF15 0xCFu
#define JPEG_RST0 0xD0u
#define JPEG_RST7 0xD7u
#define JPEG_SOI 0xD8u
#define JPEG_EOI 0xD9u
#define JPEG_SOS 0xDAu
#define JPEG_DQT 0xDBu
#define JPEG_DNL 0xDCu
#define JPEG_DRI 0xDDu
#define JPEG_APP0 0xE0u
#define JPEG_APP1 0xE1u
#define JPEG_APP14 0xEEu
#define JPEG_APP15 0xEFu
#define JPEG_COM 0xFEu

/**
 * Zigzag order, shipped rather than generated.
 *
 * The plan's rule is to derive a table where deriving is cheaper than carrying
 * it, and here it is not: the boustrophedon walk that produces these 64 bytes
 * is more code than the bytes it saves.
 */
static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32,
                                   25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                   41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35,
                                   42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                   30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39,
                                   46, 53, 60, 61, 54, 47, 55, 62, 63};

#pragma region bit reader

/**
 * An entropy coded segment's bits, with byte stuffing removed.
 *
 * The shared TinyBitReader cannot serve: a 0xFF inside a scan is followed by a
 * stuffed 0x00 that carries no bits, and a 0xFF followed by anything else is a
 * marker that ends the segment. Past the end of a segment the reader yields
 * zero bits, which is what the format tells a decoder to assume, and records
 * the marker so a restart is distinguishable from a truncation.
 */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    uint32_t accumulator;
    uint32_t count;
    /** The marker byte that ended the segment, or 0 while inside it. */
    uint8_t marker;
    /** Offset of the 0xFF that introduced `marker`. */
    size_t marker_at;
} JpegBits;

static void bits_init(
    JpegBits* bits, const uint8_t* data, size_t size, size_t at
) {
    bits->data = data;
    bits->size = size;
    bits->pos = at;
    bits->accumulator = 0;
    bits->count = 0;
    bits->marker = 0;
    bits->marker_at = size;
}

static void bits_fill(JpegBits* bits, uint32_t need) {
    while (bits->count < need) {
        if (bits->marker || bits->pos >= bits->size) {
            bits->accumulator <<= 8;
            bits->count += 8;
            continue;
        }

        uint8_t byte = bits->data[bits->pos++];

        if (byte == 0xFF) {
            // any number of 0xFF fill bytes may precede a marker
            while (bits->pos < bits->size && bits->data[bits->pos] == 0xFF) {
                bits->pos++;
            }

            uint8_t next = bits->pos < bits->size ? bits->data[bits->pos]
                                                  : (uint8_t) JPEG_EOI;

            if (next == 0x00) {
                bits->pos++;
            }
            else {
                bits->marker = next;
                bits->marker_at = bits->pos - 1;
                bits->accumulator <<= 8;
                bits->count += 8;
                continue;
            }
        }

        bits->accumulator = (bits->accumulator << 8) | byte;
        bits->count += 8;
    }
}

static inline uint32_t bits_peek(JpegBits* bits, uint32_t n) {
    bits_fill(bits, n);
    return (bits->accumulator >> (bits->count - n)) & ((1u << n) - 1u);
}

static inline uint32_t bits_get(JpegBits* bits, uint32_t n) {
    if (n == 0) return 0;

    bits_fill(bits, n);
    bits->count -= n;

    return (bits->accumulator >> bits->count) & ((1u << n) - 1u);
}

static inline int is_restart(uint8_t marker) {
    return marker >= JPEG_RST0 && marker <= JPEG_RST7;
}

/**
 * Steps over a restart marker, discarding the partial byte before it.
 *
 * The accumulator reads ahead, so the marker may already have been reached and
 * recorded; when it has not, the encoder padded the interval and the marker is
 * somewhere ahead of the read head.
 */
static void bits_restart(JpegBits* bits) {
    bits->accumulator = 0;
    bits->count = 0;

    if (is_restart(bits->marker)) {
        bits->pos = bits->marker_at + 2;
        bits->marker = 0;
        bits->marker_at = bits->size;
        return;
    }

    if (bits->marker) return;

    while (bits->pos + 1 < bits->size) {
        if (bits->data[bits->pos] == 0xFF) {
            uint8_t next = bits->data[bits->pos + 1];

            if (is_restart(next)) {
                bits->pos += 2;
                return;
            }

            if (next != 0x00 && next != 0xFF) {
                bits->marker = next;
                bits->marker_at = bits->pos;
                return;
            }
        }

        bits->pos++;
    }

    bits->pos = bits->size;
}

/**
 * Finds the marker that ends the segment, when the read head stopped short of
 * it.
 *
 * A scan whose last MCU ends mid byte leaves padding behind, and an encoder may
 * emit restart markers the decoder never needed, so the end of a segment is
 * wherever the next marker that is not a restart appears.
 */
static void bits_seek_marker(JpegBits* bits) {
    while (!bits->marker && bits->pos + 1 < bits->size) {
        if (bits->data[bits->pos] == 0xFF) {
            uint8_t next = bits->data[bits->pos + 1];

            if (next != 0x00 && next != 0xFF && !is_restart(next)) {
                bits->marker = next;
                bits->marker_at = bits->pos;
                return;
            }
        }

        bits->pos++;
    }
}

#pragma endregion

#pragma region huffman

typedef struct {
    /** Codes of each length 1 through 16. */
    uint8_t bits[17];
    /** Symbols in code order. */
    uint8_t values[256];

    int32_t mincode[17];
    int32_t maxcode[17];
    uint32_t valptr[17];

    /** Symbol and length for every code of JPEG_FAST_BITS or fewer bits. */
    uint16_t fast[1u << JPEG_FAST_BITS];

    uint8_t defined;
} JpegHuffman;

static int huff_build(JpegHuffman* table) {
    uint32_t total = 0;
    for (uint32_t length = 1; length <= 16; length++) {
        total += table->bits[length];
    }
    if (total == 0 || total > 256) return TINYIMG_ERR_CORRUPT;

    int32_t code = 0;
    uint32_t index = 0;

    for (uint32_t length = 1; length <= 16; length++) {
        table->valptr[length] = index;
        table->mincode[length] = code;

        index += table->bits[length];
        code += table->bits[length];

        table->maxcode[length] = table->bits[length] ? code - 1 : -1;
        code <<= 1;

        // a code longer than its length allows means the counts describe no
        // canonical tree
        if (code > (int32_t) (1u << (length + 1))) return TINYIMG_ERR_CORRUPT;
    }

    for (uint32_t i = 0; i < (1u << JPEG_FAST_BITS); i++) {
        table->fast[i] = 0xFFFF;
    }

    code = 0;
    index = 0;

    for (uint32_t length = 1; length <= 16; length++) {
        for (uint32_t i = 0; i < table->bits[length]; i++) {
            if (length <= JPEG_FAST_BITS) {
                uint32_t shift = JPEG_FAST_BITS - length;
                uint32_t base = (uint32_t) code << shift;

                for (uint32_t j = 0; j < (1u << shift); j++) {
                    table->fast[base + j] =
                        (uint16_t) (((uint32_t) table->values[index] << 8) |
                                    length);
                }
            }

            code++;
            index++;
        }
        code <<= 1;
    }

    table->defined = 1;
    return TINYIMG_OK;
}

static int huff_decode(JpegBits* bits, const JpegHuffman* table) {
    uint16_t entry = table->fast[bits_peek(bits, JPEG_FAST_BITS)];

    if (entry != 0xFFFF) {
        bits->count -= entry & 0xFFu;
        return (int) (entry >> 8);
    }

    int32_t code = 0;

    for (uint32_t length = 1; length <= 16; length++) {
        code = (code << 1) | (int32_t) bits_get(bits, 1);

        if (table->maxcode[length] >= 0 && code <= table->maxcode[length]) {
            uint32_t index = table->valptr[length] +
                             (uint32_t) (code - table->mincode[length]);

            if (index >= 256) return -1;
            return table->values[index];
        }
    }

    return -1;
}

/** Sign extends an `n` bit magnitude the way the format defines it. */
static inline int32_t extend(int32_t value, uint32_t n) {
    return value < (1 << (n - 1)) ? value - (1 << n) + 1 : value;
}

static inline int32_t receive_extend(JpegBits* bits, uint32_t n) {
    if (n == 0) return 0;
    return extend((int32_t) bits_get(bits, n), n);
}

#pragma endregion

#pragma region inverse transform

#define JPEG_CONST_BITS 13
#define JPEG_PASS1_BITS 2

#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

static inline int32_t descale(int32_t value, uint32_t bits) {
    return (value + (1 << (bits - 1))) >> bits;
}

static inline int32_t dequantize(
    const int16_t* coefficients, const uint16_t* quant, uint32_t i
) {
    int32_t value = (int32_t) coefficients[i] * (int32_t) quant[i];

    if (value < -JPEG_COEF_LIMIT) return -JPEG_COEF_LIMIT;
    if (value > JPEG_COEF_LIMIT) return JPEG_COEF_LIMIT;

    return value;
}

/** One row or column of the 8 point inverse transform, over eight values. */
static void idct_pass(const int32_t* in, int32_t* out, uint32_t shift) {
    int32_t z1;
    int32_t z2;
    int32_t z3;
    int32_t z4;
    int32_t z5;
    int32_t tmp0;
    int32_t tmp1;
    int32_t tmp2;
    int32_t tmp3;
    int32_t tmp10;
    int32_t tmp11;
    int32_t tmp12;
    int32_t tmp13;

    z2 = in[2];
    z3 = in[6];

    z1 = (z2 + z3) * FIX_0_541196100;
    tmp2 = z1 + z3 * -FIX_1_847759065;
    tmp3 = z1 + z2 * FIX_0_765366865;

    z2 = in[0];
    z3 = in[4];

    tmp0 = (z2 + z3) << JPEG_CONST_BITS;
    tmp1 = (z2 - z3) << JPEG_CONST_BITS;

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = in[7];
    tmp1 = in[5];
    tmp2 = in[3];
    tmp3 = in[1];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = (z3 + z4) * FIX_1_175875602;

    tmp0 = tmp0 * FIX_0_298631336;
    tmp1 = tmp1 * FIX_2_053119869;
    tmp2 = tmp2 * FIX_3_072711026;
    tmp3 = tmp3 * FIX_1_501321110;

    z1 = z1 * -FIX_0_899976223;
    z2 = z2 * -FIX_2_562915447;
    z3 = z3 * -FIX_1_961570560 + z5;
    z4 = z4 * -FIX_0_390180644 + z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    out[0] = descale(tmp10 + tmp3, shift);
    out[7] = descale(tmp10 - tmp3, shift);
    out[1] = descale(tmp11 + tmp2, shift);
    out[6] = descale(tmp11 - tmp2, shift);
    out[2] = descale(tmp12 + tmp1, shift);
    out[5] = descale(tmp12 - tmp1, shift);
    out[3] = descale(tmp13 + tmp0, shift);
    out[4] = descale(tmp13 - tmp0, shift);
}

/**
 * The full 8x8 inverse transform, writing eight rows of eight samples.
 *
 * Columns first, then rows, which is libjpeg's order and not an arbitrary one:
 * the transform is separable, so either order is mathematically the same, but
 * the intermediate is rounded between the passes and swapping them moves about
 * one pixel in twenty by a single level.
 *
 * A column whose AC terms are all zero, which after quantisation is most of
 * them, skips the transform entirely.
 */
static void idct_8x8(
    const int16_t* coefficients, const uint16_t* quant, uint8_t* out,
    uint32_t stride
) {
    int32_t workspace[64];

    for (uint32_t column = 0; column < 8; column++) {
        const int16_t* source = coefficients + column;
        const uint16_t* scale = quant + column;

        if (source[8] == 0 && source[16] == 0 && source[24] == 0 &&
            source[32] == 0 && source[40] == 0 && source[48] == 0 &&
            source[56] == 0) {
            int32_t value = dequantize(source, scale, 0) << JPEG_PASS1_BITS;

            for (uint32_t i = 0; i < 8; i++) workspace[i * 8 + column] = value;
            continue;
        }

        int32_t input[8];
        int32_t output[8];

        for (uint32_t i = 0; i < 8; i++) {
            input[i] = dequantize(source, scale, i * 8);
        }

        idct_pass(input, output, JPEG_CONST_BITS - JPEG_PASS1_BITS);

        for (uint32_t i = 0; i < 8; i++) workspace[i * 8 + column] = output[i];
    }

    for (uint32_t row = 0; row < 8; row++) {
        int32_t output[8];

        idct_pass(
            workspace + row * 8, output, JPEG_CONST_BITS + JPEG_PASS1_BITS + 3
        );

        for (uint32_t i = 0; i < 8; i++) {
            out[row * stride + i] = tiny_clamp_u8(output[i] + 128);
        }
    }
}

/**
 * The full transform followed by a box average down to NxN, for N of 2 or 4.
 *
 * Truncating the coefficients to the top left NxN and shortening the transform
 * is smaller and faster than this, and it was measured 15 dB worse: spectral
 * truncation rings, so on a photograph it lands 35 dB from a true area average
 * where libjpeg's reduced kernels land at 50. Those kernels are not a
 * truncation, and their constants do not follow from the transform, so what is
 * left is the definition the rest of the library already documents: every codec
 * box averages when it downscales, and now so does this one.
 *
 * The 1x1 case keeps its own path, where a block's mean is its DC term over
 * eight with no transform at all.
 */
static void idct_boxed(
    const int16_t* coefficients, const uint16_t* quant, uint8_t* out,
    uint32_t stride, uint32_t n
) {
    uint8_t block[64];

    idct_8x8(coefficients, quant, block, 8);

    uint32_t span = 8u / n;
    uint32_t area = span * span;

    for (uint32_t y = 0; y < n; y++) {
        for (uint32_t x = 0; x < n; x++) {
            uint32_t sum = 0;

            for (uint32_t sy = 0; sy < span; sy++) {
                const uint8_t* row = block + (y * span + sy) * 8 + x * span;

                for (uint32_t sx = 0; sx < span; sx++) sum += row[sx];
            }

            out[y * stride + x] = (uint8_t) ((sum + area / 2) / area);
        }
    }
}

static void idct_block(
    const int16_t* coefficients, const uint16_t* quant, uint8_t* out,
    uint32_t stride, uint32_t n
) {
    if (n == 8) {
        idct_8x8(coefficients, quant, out, stride);
        return;
    }

    // a block's mean is its DC term over eight, exactly and for nothing, which
    // is the scale statistics run at
    if (n == 1) {
        out[0] =
            tiny_clamp_u8(descale(dequantize(coefficients, quant, 0), 3) + 128);
        return;
    }

    idct_boxed(coefficients, quant, out, stride, n);
}

#pragma endregion

#pragma region components

typedef struct {
    uint8_t id;
    uint8_t h;
    uint8_t v;
    uint8_t quant_table;

    uint8_t dc_table;
    uint8_t ac_table;

    /** Samples across and down at full resolution, not padded. */
    uint32_t width;
    uint32_t height;
    /** Blocks across and down, padded out to whole MCUs. */
    uint32_t blocks_x;
    uint32_t blocks_y;
    /** Blocks across and down covering only the real samples. */
    uint32_t used_x;
    uint32_t used_y;

    /**
     * Samples per block edge this component is transformed at.
     *
     * Not necessarily the output's. A subsampled component is transformed at a
     * larger block size where that lands it on the output grid exactly, which
     * costs nothing and saves interpolating chroma back up after having thrown
     * it away.
     */
    uint32_t dct;
    /** How many output samples one of this component's samples covers. */
    uint32_t h_ratio;
    uint32_t v_ratio;

    /** Quantized coefficients for the whole component; progressive only. */
    int16_t* coefficients;

    /** Decoded samples at the output scale. */
    uint8_t* plane;
    uint32_t stride;
    uint32_t plane_width;
    uint32_t plane_height;
    /** First and last scaled row the plane holds, and how many it holds. */
    uint32_t row0;
    uint32_t row_last;
    uint32_t rows;
    /** First and last block column that is transformed. */
    uint32_t block_first;
    uint32_t block_last;

    int32_t dc_pred;
} JpegComponent;

typedef struct {
    const uint8_t* data;
    size_t size;

    uint32_t width;
    uint32_t height;
    uint8_t precision;
    uint8_t count;
    uint8_t progressive;
    uint8_t max_h;
    uint8_t max_v;
    uint32_t mcus_x;
    uint32_t mcus_y;
    uint32_t restart_interval;

    /** Non-zero once an Adobe APP14 segment named a colour transform. */
    uint8_t adobe;
    uint8_t transform;
    /** Non-zero when the component ids spell out RGB rather than YCbCr. */
    uint8_t rgb_ids;

    /** The EXIF payload inside APP1, starting at its TIFF header. */
    const uint8_t* exif;
    uint32_t exif_size;

    uint16_t quant[4][64];
    uint8_t quant_defined[4];

    JpegHuffman dc[4];
    JpegHuffman ac[4];

    JpegComponent components[JPEG_MAX_COMPONENTS];

    uint8_t scan_count;
    JpegComponent* scan[JPEG_MAX_COMPONENTS];
    uint8_t ss;
    uint8_t se;
    uint8_t ah;
    uint8_t al;
    uint32_t eobrun;

    JpegBits bits;

    /** The scale denominator in force, 1, 2, 4 or 8. */
    uint32_t den;
    /** Samples per block edge in the output, which is 8 / den. */
    uint32_t dct;
} JpegDecoder;

static inline uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1u) / b;
}

#pragma endregion

#pragma region markers

static inline uint32_t read_be16(const uint8_t* p) {
    return ((uint32_t) p[0] << 8) | (uint32_t) p[1];
}

/**
 * Reads the orientation tag out of an EXIF payload, or zero when there is none.
 *
 * A payload is a TIFF file with no image in it, so this walks the first
 * directory and stops: orientation is the only tag the library acts on, and
 * following the sub-directory pointers to find the rest would be a TIFF parser,
 * which is a different file.
 */
static uint32_t exif_orientation(const uint8_t* data, size_t size) {
    if (size < 8) return 0;

    int big;

    if (data[0] == 'I' && data[1] == 'I') {
        big = 0;
    }
    else if (data[0] == 'M' && data[1] == 'M') {
        big = 1;
    }
    else {
        return 0;
    }

// reading a 16 and a 32 bit field in whichever order the header declared
#define EXIF16(at)                                                             \
    (big ? (((uint32_t) (at)[0] << 8) | (at)[1])                               \
         : (((uint32_t) (at)[1] << 8) | (at)[0]))
#define EXIF32(at)                                                             \
    (big ? (((uint32_t) (at)[0] << 24) | ((uint32_t) (at)[1] << 16) |          \
            ((uint32_t) (at)[2] << 8) | (at)[3])                               \
         : (((uint32_t) (at)[3] << 24) | ((uint32_t) (at)[2] << 16) |          \
            ((uint32_t) (at)[1] << 8) | (at)[0]))

    if (EXIF16(data + 2) != 42) return 0;

    uint32_t directory = EXIF32(data + 4);
    if (directory + 2 > size) return 0;

    uint32_t count = EXIF16(data + directory);
    if (directory + 2 + (uint64_t) count * 12 > size) return 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = data + directory + 2 + i * 12;

        if (EXIF16(entry) != 0x0112) continue;
        if (EXIF16(entry + 2) != 3) continue;

        uint32_t value = EXIF16(entry + 8);
        return value >= 1 && value <= 8 ? value : 0;
    }

#undef EXIF16
#undef EXIF32

    return 0;
}

static int jpeg_sniff(const uint8_t* buffer, size_t size) {
    return buffer && size >= 3 && buffer[0] == 0xFF && buffer[1] == JPEG_SOI &&
           buffer[2] == 0xFF;
}

/** True for a start of frame marker this build cannot decode. */
static int unsupported_frame(uint8_t marker) {
    if (marker == JPEG_SOF3) return 1;
    if (marker >= JPEG_SOF5 && marker <= JPEG_SOF7) return 1;
    if (marker >= JPEG_SOF9 && marker <= JPEG_SOF11) return 1;
    if (marker >= JPEG_SOF13 && marker <= JPEG_SOF15) return 1;

    return marker == JPEG_DAC || marker == JPEG_JPG;
}

static int parse_frame(
    JpegDecoder* decoder, const uint8_t* segment, uint32_t length,
    uint8_t marker
) {
    if (length < 6) return TINYIMG_ERR_CORRUPT;

    decoder->precision = segment[0];
    decoder->height = read_be16(segment + 1);
    decoder->width = read_be16(segment + 3);
    decoder->count = segment[5];
    decoder->progressive = marker == JPEG_SOF2;

    if (decoder->width == 0 || decoder->height == 0) {
        return TINYIMG_ERR_CORRUPT;
    }
    if (decoder->count == 0 || decoder->count > JPEG_MAX_COMPONENTS) {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }
    if (length < 6u + (uint32_t) decoder->count * 3u) {
        return TINYIMG_ERR_CORRUPT;
    }

    decoder->max_h = 1;
    decoder->max_v = 1;

    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];
        const uint8_t* entry = segment + 6 + i * 3;

        component->id = entry[0];
        component->h = (uint8_t) (entry[1] >> 4);
        component->v = (uint8_t) (entry[1] & 15);
        component->quant_table = entry[2];

        if (component->h == 0 || component->h > 4 || component->v == 0 ||
            component->v > 4) {
            return TINYIMG_ERR_CORRUPT;
        }
        if (component->quant_table > 3) return TINYIMG_ERR_CORRUPT;

        if (component->h > decoder->max_h) decoder->max_h = component->h;
        if (component->v > decoder->max_v) decoder->max_v = component->v;
    }

    decoder->rgb_ids =
        decoder->count == 3 && decoder->components[0].id == 'R' &&
        decoder->components[1].id == 'G' && decoder->components[2].id == 'B';

    uint32_t mcu_width = (uint32_t) decoder->max_h * 8u;
    uint32_t mcu_height = (uint32_t) decoder->max_v * 8u;

    decoder->mcus_x = ceil_div(decoder->width, mcu_width);
    decoder->mcus_y = ceil_div(decoder->height, mcu_height);

    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];

        component->width =
            ceil_div(decoder->width * component->h, decoder->max_h);
        component->height =
            ceil_div(decoder->height * component->v, decoder->max_v);

        component->blocks_x = decoder->mcus_x * component->h;
        component->blocks_y = decoder->mcus_y * component->v;
        component->used_x = ceil_div(component->width, 8);
        component->used_y = ceil_div(component->height, 8);
    }

    if (unsupported_frame(marker)) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (decoder->precision != 8) return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    if (decoder->count == 2) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    // deliberately no check against the pixel budget here. A source over the
    // budget is the case a scaled decode exists for, and a probe has to answer
    // for it at all; the budget is enforced against what a decode will actually
    // produce, in tiny_decode_resolve

    return TINYIMG_OK;
}

static int parse_quant(
    JpegDecoder* decoder, const uint8_t* segment, uint32_t length
) {
    uint32_t at = 0;

    while (at < length) {
        uint8_t spec = segment[at++];
        uint32_t slot = spec & 15u;
        uint32_t wide = spec >> 4;

        if (slot > 3 || wide > 1) return TINYIMG_ERR_CORRUPT;

        uint32_t need = wide ? 128u : 64u;
        if (at + need > length) return TINYIMG_ERR_CORRUPT;

        for (uint32_t i = 0; i < 64; i++) {
            uint32_t value =
                wide ? read_be16(segment + at + i * 2) : segment[at + i];

            if (value == 0) return TINYIMG_ERR_CORRUPT;
            decoder->quant[slot][zigzag[i]] = (uint16_t) value;
        }

        decoder->quant_defined[slot] = 1;
        at += need;
    }

    return TINYIMG_OK;
}

static int parse_huffman(
    JpegDecoder* decoder, const uint8_t* segment, uint32_t length
) {
    uint32_t at = 0;

    while (at < length) {
        uint8_t spec = segment[at++];
        uint32_t slot = spec & 15u;
        uint32_t is_ac = spec >> 4;

        if (slot > 3 || is_ac > 1) return TINYIMG_ERR_CORRUPT;
        if (at + 16 > length) return TINYIMG_ERR_CORRUPT;

        JpegHuffman* table = is_ac ? &decoder->ac[slot] : &decoder->dc[slot];
        uint32_t total = 0;

        table->bits[0] = 0;
        for (uint32_t i = 1; i <= 16; i++) {
            table->bits[i] = segment[at + i - 1];
            total += table->bits[i];
        }
        at += 16;

        if (total > 256 || at + total > length) return TINYIMG_ERR_CORRUPT;

        for (uint32_t i = 0; i < total; i++) {
            table->values[i] = segment[at + i];
        }
        at += total;

        int result = huff_build(table);
        if (result != TINYIMG_OK) return result;
    }

    return TINYIMG_OK;
}

static int parse_scan(
    JpegDecoder* decoder, const uint8_t* segment, uint32_t length
) {
    if (length < 1) return TINYIMG_ERR_CORRUPT;

    uint32_t count = segment[0];
    if (count == 0 || count > JPEG_MAX_COMPONENTS) return TINYIMG_ERR_CORRUPT;
    if (length < 1 + count * 2 + 3) return TINYIMG_ERR_CORRUPT;

    decoder->scan_count = (uint8_t) count;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t id = segment[1 + i * 2];
        uint8_t tables = segment[2 + i * 2];
        JpegComponent* found = 0;

        for (uint32_t j = 0; j < decoder->count; j++) {
            if (decoder->components[j].id == id) {
                found = &decoder->components[j];
                break;
            }
        }

        if (!found) return TINYIMG_ERR_CORRUPT;

        found->dc_table = (uint8_t) (tables >> 4);
        found->ac_table = (uint8_t) (tables & 15);

        if (found->dc_table > 3 || found->ac_table > 3) {
            return TINYIMG_ERR_CORRUPT;
        }

        decoder->scan[i] = found;
    }

    const uint8_t* tail = segment + 1 + count * 2;

    decoder->ss = tail[0];
    decoder->se = tail[1];
    decoder->ah = (uint8_t) (tail[2] >> 4);
    decoder->al = (uint8_t) (tail[2] & 15);

    if (!decoder->progressive) {
        decoder->ss = 0;
        decoder->se = 63;
        decoder->ah = 0;
        decoder->al = 0;
    }

    if (decoder->ss > 63 || decoder->se > 63 || decoder->ss > decoder->se) {
        return TINYIMG_ERR_CORRUPT;
    }
    if (decoder->al > 13 || decoder->ah > 13) return TINYIMG_ERR_CORRUPT;

    // a scan that carries AC coefficients can only carry one component's, and
    // one that carries DC may interleave
    if (decoder->ss != 0 && count != 1) return TINYIMG_ERR_CORRUPT;

    decoder->eobrun = 0;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region scan decoding

static int decode_baseline_block(
    JpegDecoder* decoder, JpegComponent* component, int16_t* block
) {
    const JpegHuffman* dc = &decoder->dc[component->dc_table];
    const JpegHuffman* ac = &decoder->ac[component->ac_table];

    if (!dc->defined || !ac->defined) return TINYIMG_ERR_CORRUPT;

    int symbol = huff_decode(&decoder->bits, dc);
    if (symbol < 0 || symbol > 16) return TINYIMG_ERR_CORRUPT;

    component->dc_pred += receive_extend(&decoder->bits, (uint32_t) symbol);
    block[0] = (int16_t) component->dc_pred;

    uint32_t k = 1;

    while (k < 64) {
        symbol = huff_decode(&decoder->bits, ac);
        if (symbol < 0) return TINYIMG_ERR_CORRUPT;

        uint32_t run = (uint32_t) symbol >> 4;
        uint32_t size = (uint32_t) symbol & 15u;

        if (size == 0) {
            if (run != 15) break;
            k += 16;
            continue;
        }

        k += run;
        if (k > 63) return TINYIMG_ERR_CORRUPT;

        block[zigzag[k]] = (int16_t) receive_extend(&decoder->bits, size);
        k++;
    }

    return TINYIMG_OK;
}

static int decode_dc_first(
    JpegDecoder* decoder, JpegComponent* component, int16_t* block
) {
    const JpegHuffman* dc = &decoder->dc[component->dc_table];
    if (!dc->defined) return TINYIMG_ERR_CORRUPT;

    int symbol = huff_decode(&decoder->bits, dc);
    if (symbol < 0 || symbol > 16) return TINYIMG_ERR_CORRUPT;

    component->dc_pred += receive_extend(&decoder->bits, (uint32_t) symbol);
    block[0] = (int16_t) ((uint32_t) component->dc_pred << decoder->al);

    return TINYIMG_OK;
}

static int decode_dc_refine(JpegDecoder* decoder, int16_t* block) {
    if (bits_get(&decoder->bits, 1)) {
        block[0] = (int16_t) (block[0] | (1 << decoder->al));
    }

    return TINYIMG_OK;
}

static int decode_ac_first(
    JpegDecoder* decoder, JpegComponent* component, int16_t* block
) {
    const JpegHuffman* ac = &decoder->ac[component->ac_table];
    if (!ac->defined) return TINYIMG_ERR_CORRUPT;

    if (decoder->eobrun > 0) {
        decoder->eobrun--;
        return TINYIMG_OK;
    }

    uint32_t k = decoder->ss;

    while (k <= decoder->se) {
        int symbol = huff_decode(&decoder->bits, ac);
        if (symbol < 0) return TINYIMG_ERR_CORRUPT;

        uint32_t run = (uint32_t) symbol >> 4;
        uint32_t size = (uint32_t) symbol & 15u;

        if (size == 0) {
            if (run != 15) {
                decoder->eobrun = (1u << run) - 1u;
                if (run) decoder->eobrun += bits_get(&decoder->bits, run);
                break;
            }

            k += 16;
            continue;
        }

        k += run;
        if (k > decoder->se) return TINYIMG_ERR_CORRUPT;

        block[zigzag[k]] =
            (int16_t) (receive_extend(&decoder->bits, size) << decoder->al);
        k++;
    }

    return TINYIMG_OK;
}

/**
 * Appends one bit of precision to the AC coefficients of one block.
 *
 * The shape is the format's, not a choice: a correction bit belongs to every
 * coefficient already known to be non-zero, so the run length in a symbol
 * counts only the coefficients still at zero and the walk has to step over the
 * others without consuming from the run.
 */
static int decode_ac_refine(
    JpegDecoder* decoder, JpegComponent* component, int16_t* block
) {
    const JpegHuffman* ac = &decoder->ac[component->ac_table];
    if (!ac->defined) return TINYIMG_ERR_CORRUPT;

    int32_t positive = 1 << decoder->al;
    int32_t negative = -(1 << decoder->al);

    uint32_t k = decoder->ss;

    if (decoder->eobrun == 0) {
        while (k <= decoder->se) {
            int symbol = huff_decode(&decoder->bits, ac);
            if (symbol < 0) return TINYIMG_ERR_CORRUPT;

            uint32_t run = (uint32_t) symbol >> 4;
            uint32_t size = (uint32_t) symbol & 15u;
            int32_t value = 0;

            if (size != 0) {
                if (size != 1) return TINYIMG_ERR_CORRUPT;
                value = bits_get(&decoder->bits, 1) ? positive : negative;
            }
            else if (run != 15) {
                // the current block is a member of the run and is not
                // discounted here, unlike in a first scan: the coefficients
                // after this symbol still carry correction bits, and the run
                // is only spent once those have been read below
                decoder->eobrun = 1u << run;
                if (run) decoder->eobrun += bits_get(&decoder->bits, run);
                break;
            }

            while (k <= decoder->se) {
                int16_t* coefficient = &block[zigzag[k]];

                if (*coefficient != 0) {
                    if (bits_get(&decoder->bits, 1) &&
                        (*coefficient & positive) == 0) {
                        *coefficient =
                            (int16_t) (*coefficient + (*coefficient >= 0
                                                           ? positive
                                                           : negative));
                    }
                }
                else {
                    if (run == 0) break;
                    run--;
                }

                k++;
            }

            if (value != 0 && k <= decoder->se) {
                block[zigzag[k]] = (int16_t) value;
            }

            k++;
        }
    }

    if (decoder->eobrun > 0) {
        while (k <= decoder->se) {
            int16_t* coefficient = &block[zigzag[k]];

            if (*coefficient != 0) {
                if (bits_get(&decoder->bits, 1) &&
                    (*coefficient & positive) == 0) {
                    *coefficient =
                        (int16_t) (*coefficient +
                                   (*coefficient >= 0 ? positive : negative));
                }
            }

            k++;
        }

        decoder->eobrun--;
    }

    return TINYIMG_OK;
}

/** Where a block's samples land in a component's plane, or NULL to skip it. */
static uint8_t* block_target(
    const JpegComponent* component, uint32_t block_x, uint32_t block_y
) {
    uint32_t first = block_y * component->dct;

    if (block_x < component->block_first || block_x > component->block_last) {
        return 0;
    }
    if (first < component->row0 ||
        first + component->dct > component->row0 + component->rows) {
        return 0;
    }

    return component->plane +
           (size_t) (first - component->row0) * component->stride +
           (size_t) block_x * component->dct;
}

static void reset_predictors(JpegDecoder* decoder) {
    for (uint32_t i = 0; i < decoder->count; i++) {
        decoder->components[i].dc_pred = 0;
    }
    decoder->eobrun = 0;
}

/**
 * Decodes one block, either into the coefficient plane or straight to samples.
 *
 * A sequential scan finishes a block in one pass, so it transforms immediately
 * and never stores coefficients. A progressive one cannot: no coefficient is
 * final until the last scan that touches it.
 */
static int decode_block_at(
    JpegDecoder* decoder, JpegComponent* component, uint32_t block_x,
    uint32_t block_y
) {
    if (block_x >= component->blocks_x || block_y >= component->blocks_y) {
        return TINYIMG_ERR_CORRUPT;
    }

    if (decoder->progressive) {
        int16_t* block =
            component->coefficients +
            ((size_t) block_y * component->blocks_x + block_x) * 64;

        if (decoder->ss == 0) {
            return decoder->ah == 0 ? decode_dc_first(decoder, component, block)
                                    : decode_dc_refine(decoder, block);
        }

        return decoder->ah == 0 ? decode_ac_first(decoder, component, block)
                                : decode_ac_refine(decoder, component, block);
    }

    int16_t block[64];
    for (uint32_t i = 0; i < 64; i++) block[i] = 0;

    int result = decode_baseline_block(decoder, component, block);
    if (result != TINYIMG_OK) return result;

    uint8_t* target = block_target(component, block_x, block_y);
    if (target) {
        idct_block(
            block, decoder->quant[component->quant_table], target,
            component->stride, component->dct
        );
    }

    return TINYIMG_OK;
}

/**
 * Runs one scan's entropy coded segment to its end.
 *
 * @param decoder The decoder, with the scan header already parsed.
 * @param at Offset of the first entropy coded byte.
 * @param next Receives the offset of the marker that ended the segment.
 */
static int decode_scan(JpegDecoder* decoder, size_t at, size_t* next) {
    bits_init(&decoder->bits, decoder->data, decoder->size, at);
    reset_predictors(decoder);

    uint32_t mcus_x;
    uint32_t mcus_y;

    if (decoder->scan_count == 1) {
        mcus_x = decoder->scan[0]->used_x;
        mcus_y = decoder->scan[0]->used_y;
    }
    else {
        mcus_x = decoder->mcus_x;
        mcus_y = decoder->mcus_y;
    }

    if (mcus_x == 0 || mcus_y == 0) return TINYIMG_ERR_CORRUPT;

    uint32_t interval = decoder->restart_interval;
    uint32_t until_restart = interval;
    int result = TINYIMG_OK;

    for (uint32_t my = 0; my < mcus_y && result == TINYIMG_OK; my++) {
        for (uint32_t mx = 0; mx < mcus_x && result == TINYIMG_OK; mx++) {
            if (interval && until_restart == 0) {
                bits_restart(&decoder->bits);
                reset_predictors(decoder);
                until_restart = interval;
            }

            if (decoder->scan_count == 1) {
                result = decode_block_at(decoder, decoder->scan[0], mx, my);
            }
            else {
                for (uint32_t i = 0;
                     i < decoder->scan_count && result == TINYIMG_OK; i++) {
                    JpegComponent* component = decoder->scan[i];

                    for (uint32_t by = 0;
                         by < component->v && result == TINYIMG_OK; by++) {
                        for (uint32_t bx = 0;
                             bx < component->h && result == TINYIMG_OK; bx++) {
                            result = decode_block_at(
                                decoder, component, mx * component->h + bx,
                                my * component->v + by
                            );
                        }
                    }
                }
            }

            if (interval) until_restart--;
        }
    }

    if (result != TINYIMG_OK) return result;

    // a restart marker the last MCU did not need still sits in the way, so step
    // over any of those before looking for the one that ends the segment
    while (is_restart(decoder->bits.marker)) bits_restart(&decoder->bits);
    bits_seek_marker(&decoder->bits);

    *next = decoder->bits.marker ? decoder->bits.marker_at : decoder->size;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region upsampling

static inline uint32_t clamp_row(const JpegComponent* component, int64_t row) {
    if (row < (int64_t) component->row0) row = component->row0;
    if (row > (int64_t) component->row_last) row = component->row_last;

    return (uint32_t) (row - (int64_t) component->row0);
}

static inline uint32_t clamp_column(
    const JpegComponent* component, int64_t column
) {
    if (column < 0) return 0;
    if (column > (int64_t) component->plane_width - 1) {
        return component->plane_width - 1;
    }

    return (uint32_t) column;
}

/**
 * Fills one row of a component at the output's resolution.
 *
 * The triangle filter for the 2x cases is the one libjpeg calls fancy
 * upsampling, computed per pixel with clamped neighbour indices rather than as
 * a streaming loop with special cased ends; clamping reproduces those ends
 * exactly and there is then no edge case to get wrong.
 */
static void sample_row(
    const JpegComponent* component, uint32_t row, uint32_t x0, uint32_t count,
    uint32_t hr, uint32_t vr, uint8_t* out
) {
    if (hr == 1 && vr == 1) {
        const uint8_t* source =
            component->plane +
            (size_t) clamp_row(component, row) * component->stride;

        for (uint32_t i = 0; i < count; i++) {
            out[i] = source[clamp_column(component, (int64_t) x0 + i)];
        }
        return;
    }

    if (hr == 2 && vr == 1) {
        const uint8_t* source =
            component->plane +
            (size_t) clamp_row(component, row) * component->stride;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t x = x0 + i;
            uint32_t index = x >> 1;
            int64_t neighbour =
                (x & 1u) ? (int64_t) index + 1 : (int64_t) index - 1;

            uint32_t near = source[clamp_column(component, (int64_t) index)];
            uint32_t far = source[clamp_column(component, neighbour)];

            out[i] = (uint8_t) ((near * 3u + far + ((x & 1u) ? 2u : 1u)) >> 2);
        }
        return;
    }

    if (hr == 2 && vr == 2) {
        uint32_t index_y = row >> 1;
        int64_t other_y =
            (row & 1u) ? (int64_t) index_y + 1 : (int64_t) index_y - 1;

        const uint8_t* near_row =
            component->plane +
            (size_t) clamp_row(component, (int64_t) index_y) *
                component->stride;
        const uint8_t* far_row =
            component->plane +
            (size_t) clamp_row(component, other_y) * component->stride;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t x = x0 + i;
            uint32_t index = x >> 1;
            int64_t neighbour =
                (x & 1u) ? (int64_t) index + 1 : (int64_t) index - 1;

            uint32_t here = clamp_column(component, (int64_t) index);
            uint32_t there = clamp_column(component, neighbour);

            uint32_t sum = near_row[here] * 3u + far_row[here];
            uint32_t other = near_row[there] * 3u + far_row[there];

            out[i] = (uint8_t) ((sum * 3u + other + ((x & 1u) ? 7u : 8u)) >> 4);
        }
        return;
    }

    const uint8_t* source =
        component->plane +
        (size_t) clamp_row(component, (int64_t) (row / vr)) * component->stride;

    for (uint32_t i = 0; i < count; i++) {
        out[i] = source[clamp_column(component, (int64_t) ((x0 + i) / hr))];
    }
}

#pragma endregion

#pragma region colour

/**
 * Rec. 601 inverse, in 16 bit fixed point.
 *
 * The constants and the placement of the rounding term are libjpeg's, down to
 * the green channel summing both products before the shift rather than
 * shifting each: rounding after one addition and rounding after two disagree by
 * a level on some inputs, and matching means a decode can be compared against
 * `djpeg` byte for byte instead of through a tolerance.
 */
static void ycbcr_to_rgb(uint32_t y, uint32_t cb, uint32_t cr, uint8_t* out) {
    int32_t blue = (int32_t) cb - 128;
    int32_t red = (int32_t) cr - 128;

    out[0] = tiny_clamp_u8((int32_t) y + ((91881 * red + 32768) >> 16));
    out[1] = tiny_clamp_u8(
        (int32_t) y + ((-22554 * blue - 46802 * red + 32768) >> 16)
    );
    out[2] = tiny_clamp_u8((int32_t) y + ((116130 * blue + 32768) >> 16));
}

/** Multiplies an inverted ink value by the black channel, both 0 to 255. */
static inline uint8_t ink(uint32_t value, uint32_t black) {
    return (uint8_t) ((value * black + 127u) / 255u);
}

#pragma endregion

#pragma region decode

static int jpeg_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info);

/**
 * Chooses which scaled rows of each component the planes have to hold.
 *
 * A region needs one component row either side of it for the upsampler's
 * context, and whole blocks, since a block is transformed in one piece. Both
 * are rounded outward rather than clipped, so nothing downstream has to know
 * that the plane is a window.
 */
static int plan_planes(
    JpegDecoder* decoder, uint32_t x0, uint32_t y0, uint32_t out_width,
    uint32_t out_height
) {
    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];

        // grow this component's transform until one more doubling would
        // overshoot the output grid, which is what turns a 4:2:0 chroma
        // component at half scale into a full block decode with no upsampling
        uint32_t size = decoder->dct;

        while (size < 8 &&
               component->h * size * 2 <= decoder->max_h * decoder->dct &&
               component->v * size * 2 <= decoder->max_v * decoder->dct) {
            size *= 2;
        }

        component->dct = size;

        uint32_t across = component->h * size;
        uint32_t down = component->v * size;
        uint32_t wanted_across = (uint32_t) decoder->max_h * decoder->dct;
        uint32_t wanted_down = (uint32_t) decoder->max_v * decoder->dct;

        // libjpeg calls this fractional sampling and does not implement it
        // either; nothing but a hand written file reaches it
        if (wanted_across % across != 0 || wanted_down % down != 0) {
            return TINYIMG_ERR_UNSUPPORTED_VARIANT;
        }

        component->h_ratio = wanted_across / across;
        component->v_ratio = wanted_down / down;

        component->plane_width = ceil_div(component->width * size, 8);
        component->plane_height = ceil_div(component->height * size, 8);
        component->stride = component->blocks_x * size;

        uint32_t first = y0 / component->v_ratio;
        uint32_t last = (y0 + out_height - 1) / component->v_ratio;

        if (first > 0) first--;
        last++;

        if (last > component->plane_height - 1) {
            last = component->plane_height - 1;
        }
        if (first > last) first = last;

        uint32_t first_block = first / size;
        uint32_t last_block = last / size;

        component->row0 = first_block * size;
        component->rows = (last_block - first_block + 1) * size;
        component->row_last = component->row0 + component->rows - 1;

        if (component->row_last > component->plane_height - 1) {
            component->row_last = component->plane_height - 1;
        }

        uint32_t left = x0 / component->h_ratio;
        uint32_t right = (x0 + out_width - 1) / component->h_ratio;

        if (left > 0) left--;
        right++;

        if (right > component->plane_width - 1) {
            right = component->plane_width - 1;
        }
        if (left > right) left = right;

        component->block_first = left / size;
        component->block_last = right / size;
    }

    return TINYIMG_OK;
}

static int allocate_planes(JpegDecoder* decoder) {
    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];

        // dimensions are 16 bit in the format, so a plane can be described that
        // a 32 bit size cannot hold; the cast below has to be safe before the
        // allocator gets a chance to refuse it
        uint64_t bytes = (uint64_t) component->stride * component->rows;
        if (bytes > 0xFFFFFFFFu) return TINYIMG_ERR_MEMORY;

        component->plane = tiny_arena_alloc((size_t) bytes, 0);
        if (!component->plane) return TINYIMG_ERR_MEMORY;

        // padding blocks and skipped columns are never read, but a plane that
        // is partly uninitialised is a sanitizer report waiting to happen
        tiny_memset(component->plane, 128, (size_t) bytes);
    }

    return TINYIMG_OK;
}

static int allocate_coefficients(JpegDecoder* decoder) {
    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];

        uint64_t count =
            (uint64_t) component->blocks_x * component->blocks_y * 64;
        uint64_t bytes = count * sizeof(int16_t);

        if (bytes > 0xFFFFFFFFu) return TINYIMG_ERR_MEMORY;

        component->coefficients = tiny_arena_alloc((size_t) bytes, 0);
        if (!component->coefficients) return TINYIMG_ERR_MEMORY;

        tiny_memset(component->coefficients, 0, (size_t) bytes);
    }

    return TINYIMG_OK;
}

/** Transforms every block a progressive decode kept, once all scans are in. */
static void transform_coefficients(JpegDecoder* decoder) {
    for (uint32_t i = 0; i < decoder->count; i++) {
        JpegComponent* component = &decoder->components[i];

        for (uint32_t block_y = 0; block_y < component->blocks_y; block_y++) {
            for (uint32_t block_x = component->block_first;
                 block_x <= component->block_last; block_x++) {
                uint8_t* target = block_target(component, block_x, block_y);
                if (!target) continue;

                const int16_t* block =
                    component->coefficients +
                    ((size_t) block_y * component->blocks_x + block_x) * 64;

                idct_block(
                    block, decoder->quant[component->quant_table], target,
                    component->stride, component->dct
                );
            }
        }
    }
}

static int write_pixels(
    JpegDecoder* decoder, TinyImage* image, uint32_t x0, uint32_t y0,
    uint8_t channels
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* rows[JPEG_MAX_COMPONENTS];

    for (uint32_t i = 0; i < decoder->count; i++) {
        rows[i] = tiny_arena_alloc(image->width, 0);
        if (!rows[i]) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }
    }

    int ycbcr =
        decoder->count >= 3 && !decoder->rgb_ids &&
        !(decoder->count == 3 && decoder->adobe && decoder->transform == 0) &&
        !(decoder->count == 4 && decoder->transform == 0);

    for (uint32_t oy = 0; oy < image->height; oy++) {
        for (uint32_t i = 0; i < decoder->count; i++) {
            const JpegComponent* component = &decoder->components[i];

            sample_row(
                component, y0 + oy, x0, image->width, component->h_ratio,
                component->v_ratio, rows[i]
            );
        }

        uint8_t* dest = image->data + (size_t) oy * image->width * channels;

        for (uint32_t ox = 0; ox < image->width; ox++) {
            uint8_t rgba[4] = {0, 0, 0, 255};

            if (decoder->count == 1) {
                rgba[0] = rows[0][ox];
                rgba[1] = rgba[0];
                rgba[2] = rgba[0];
            }
            else if (decoder->count == 3) {
                if (ycbcr) {
                    ycbcr_to_rgb(rows[0][ox], rows[1][ox], rows[2][ox], rgba);
                }
                else {
                    rgba[0] = rows[0][ox];
                    rgba[1] = rows[1][ox];
                    rgba[2] = rows[2][ox];
                }
            }
            else {
                uint8_t cmy[4] = {rows[0][ox], rows[1][ox], rows[2][ox], 255};

                // the two four component transforms are not symmetric. Adobe
                // stores CMYK inverted, so a stored channel is already the
                // amount of light left; YCCK codes the ink amounts themselves,
                // so the inverse transform gives ink and it is the one that
                // needs turning back around
                if (ycbcr) {
                    ycbcr_to_rgb(rows[0][ox], rows[1][ox], rows[2][ox], cmy);

                    cmy[0] = (uint8_t) (255u - cmy[0]);
                    cmy[1] = (uint8_t) (255u - cmy[1]);
                    cmy[2] = (uint8_t) (255u - cmy[2]);
                }

                uint32_t black = rows[3][ox];

                rgba[0] = ink(cmy[0], black);
                rgba[1] = ink(cmy[1], black);
                rgba[2] = ink(cmy[2], black);
            }

            tiny_pixel_convert(
                dest + (size_t) ox * channels, channels, rgba, 4
            );
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

/**
 * Hands the EXIF payload to the image, along with the orientation it names.
 *
 * The orientation is parsed but not applied. Rotating here would be work the
 * planner then has to undo: it propagates a region backward through every
 * geometry operation to decide what to decode, so an orientation baked into the
 * decoder is a transform it cannot see through, and one the caller's own rotate
 * would then compose with twice. The tag is stated as metadata so the plan can
 * pick it up as an operation like any other. Namespaced, so a caller's own key
 * can never collide with it.
 */
static int attach_metadata(JpegDecoder* decoder, TinyImage* image) {
    if (!decoder->exif) return TINYIMG_OK;

    int result = tiny_image_set_exif(
        image, (const char*) decoder->exif, decoder->exif_size
    );
    if (result != TINYIMG_OK) return result;

    uint32_t orientation = exif_orientation(decoder->exif, decoder->exif_size);

    if (orientation == 0) return TINYIMG_OK;

    char digit[2] = {(char) ('0' + orientation), 0};

    return tiny_image_set_metadata(image, "exif:Orientation", digit);
}

/**
 * Walks the marker sequence, decoding every scan it finds.
 *
 * Segments are parsed where they appear rather than gathered first, because a
 * quantisation or Huffman table may be redefined between scans and only the
 * definition in force at a scan applies to it.
 */
static int walk(
    JpegDecoder* decoder, TinyImage* image, const TinyDecodeOpts* options,
    int* created
) {
    size_t at = 2;
    int seen_frame = 0;
    int planned = 0;
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint8_t channels = 0;

    while (at + 1 < decoder->size) {
        if (decoder->data[at] != 0xFF) {
            at++;
            continue;
        }

        uint8_t marker = decoder->data[at + 1];
        at += 2;

        if (marker == 0xFF || marker == 0x00) continue;
        if (marker == JPEG_EOI) break;
        if (marker >= JPEG_RST0 && marker <= JPEG_RST7) continue;

        if (at + 2 > decoder->size) return TINYIMG_ERR_CORRUPT;

        uint32_t length = read_be16(decoder->data + at);
        if (length < 2 || at + length > decoder->size) {
            return TINYIMG_ERR_CORRUPT;
        }

        const uint8_t* segment = decoder->data + at + 2;
        uint32_t payload = length - 2;
        int result = TINYIMG_OK;

        switch (marker) {
            case JPEG_DQT:
                result = parse_quant(decoder, segment, payload);
                break;
            case JPEG_DHT:
                result = parse_huffman(decoder, segment, payload);
                break;

            case JPEG_DRI:
                if (payload < 2) return TINYIMG_ERR_CORRUPT;
                decoder->restart_interval = read_be16(segment);
                break;

            case JPEG_APP1:
                // "Exif" and two zero bytes, then a whole TIFF header. Only the
                // first such segment counts; a second is either a different
                // profile or a file that has been through two writers
                if (!decoder->exif && payload > 6 && segment[0] == 'E' &&
                    segment[1] == 'x' && segment[2] == 'i' &&
                    segment[3] == 'f' && segment[4] == 0) {
                    decoder->exif = segment + 6;
                    decoder->exif_size = payload - 6;
                }
                break;

            case JPEG_APP14:
                // "Adobe" then a version, two flag words and the transform
                if (payload >= 12 && segment[0] == 'A' && segment[1] == 'd' &&
                    segment[2] == 'o' && segment[3] == 'b' &&
                    segment[4] == 'e') {
                    decoder->adobe = 1;
                    decoder->transform = segment[11];
                }
                break;

            case JPEG_SOS: {
                if (!seen_frame) return TINYIMG_ERR_CORRUPT;

                result = parse_scan(decoder, segment, payload);
                if (result != TINYIMG_OK) return result;

                if (!planned) {
                    uint32_t out_width;
                    uint32_t out_height;
                    TinyDecodeOpts resolved;

                    result = tiny_decode_resolve(
                        options, decoder->width, decoder->height, &resolved,
                        &out_width, &out_height
                    );
                    if (result != TINYIMG_OK) return result;

                    decoder->den = resolved.scale_den;
                    decoder->dct = 8u / resolved.scale_den;
                    x0 = resolved.x / resolved.scale_den;
                    y0 = resolved.y / resolved.scale_den;

                    channels = resolved.channels
                                   ? resolved.channels
                                   : (uint8_t) (decoder->count == 1 ? 1 : 3);

                    result = tiny_image_create(
                        image, out_width, out_height, channels
                    );
                    if (result != TINYIMG_OK) return result;

                    *created = 1;

                    result =
                        plan_planes(decoder, x0, y0, out_width, out_height);
                    if (result != TINYIMG_OK) return result;

                    result = allocate_planes(decoder);
                    if (result != TINYIMG_OK) return result;

                    if (decoder->progressive) {
                        result = allocate_coefficients(decoder);
                        if (result != TINYIMG_OK) return result;
                    }

                    planned = 1;
                }

                size_t next = decoder->size;

                result = decode_scan(decoder, at + length, &next);
                if (result != TINYIMG_OK) return result;

                at = next;
                continue;
            }

            case JPEG_DNL: break;

            default:
                if (marker >= JPEG_SOF0 && marker <= JPEG_SOF15 &&
                    marker != JPEG_DHT && marker != JPEG_JPG &&
                    marker != JPEG_DAC) {
                    if (seen_frame) return TINYIMG_ERR_CORRUPT;

                    result = parse_frame(decoder, segment, payload, marker);
                    if (result != TINYIMG_OK) return result;

                    seen_frame = 1;
                }
                break;
        }

        if (result != TINYIMG_OK) return result;
        at += length;
    }

    if (!seen_frame) return TINYIMG_ERR_CORRUPT;
    if (!planned) return TINYIMG_ERR_CORRUPT;

    for (uint32_t i = 0; i < decoder->count; i++) {
        if (!decoder->quant_defined[decoder->components[i].quant_table]) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    if (decoder->progressive) transform_coefficients(decoder);

    image->format = TINYIMG_FORMAT_JPEG;

    int written = write_pixels(decoder, image, x0, y0, channels);
    if (written != TINYIMG_OK) return written;

    return attach_metadata(decoder, image);
}

static int run(
    JpegDecoder* decoder, TinyImage* image, const TinyDecodeOpts* options
) {
    int created = 0;
    int result = walk(decoder, image, options, &created);

    if (result != TINYIMG_OK && created) tiny_image_destroy(image);

    return result;
}

static int jpeg_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* options
) {
    if (!jpeg_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    JpegDecoder* decoder = tiny_arena_alloc(sizeof(JpegDecoder), 0);
    if (!decoder) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(decoder, 0, sizeof(*decoder));
    decoder->data = buffer;
    decoder->size = size;

    int result = run(decoder, image, options);

    tiny_arena_release(&mark);

    return result;
}

static int jpeg_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    if (!jpeg_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    size_t at = 2;
    JpegDecoder decoder;

    tiny_memset(&decoder, 0, sizeof(decoder));

    while (at + 1 < size) {
        if (buffer[at] != 0xFF) {
            at++;
            continue;
        }

        uint8_t marker = buffer[at + 1];
        at += 2;

        if (marker == 0xFF || marker == 0x00) continue;
        if (marker == JPEG_EOI || marker == JPEG_SOS) break;
        if (marker >= JPEG_RST0 && marker <= JPEG_RST7) continue;

        if (at + 2 > size) return TINYIMG_ERR_CORRUPT;

        uint32_t length = read_be16(buffer + at);
        if (length < 2 || at + length > size) return TINYIMG_ERR_CORRUPT;

        if (marker >= JPEG_SOF0 && marker <= JPEG_SOF15 && marker != JPEG_DHT &&
            marker != JPEG_JPG && marker != JPEG_DAC) {
            int result =
                parse_frame(&decoder, buffer + at + 2, length - 2, marker);

            // a variant this build cannot decode still has a readable header,
            // which is the whole point of a probe
            if (result != TINYIMG_OK &&
                result != TINYIMG_ERR_UNSUPPORTED_VARIANT) {
                return result;
            }

            info->width = decoder.width;
            info->height = decoder.height;
            info->frames = 1;
            info->format = TINYIMG_FORMAT_JPEG;
            info->channels = (uint8_t) (decoder.count == 1 ? 1 : 3);
            info->bit_depth = decoder.precision;
            info->has_alpha = 0;
            info->progressive = decoder.progressive;

            return TINYIMG_OK;
        }

        at += length;
    }

    return TINYIMG_ERR_CORRUPT;
}

#pragma endregion

#pragma region forward transform

/**
 * The 8x8 forward transform, reading eight rows of samples.
 *
 * The same algorithm and the same constants as the inverse, run backwards, so
 * the encoder adds no table of its own. The output is eight times the true
 * transform, which the quantisation divisors absorb.
 */
static void fdct_8x8(const uint8_t* in, uint32_t stride, int32_t* out) {
    int32_t workspace[64];

    for (uint32_t row = 0; row < 8; row++) {
        const uint8_t* source = in + (size_t) row * stride;
        int32_t* target = workspace + row * 8;

        int32_t tmp0 = (int32_t) source[0] + source[7] - 256;
        int32_t tmp7 = (int32_t) source[0] - source[7];
        int32_t tmp1 = (int32_t) source[1] + source[6] - 256;
        int32_t tmp6 = (int32_t) source[1] - source[6];
        int32_t tmp2 = (int32_t) source[2] + source[5] - 256;
        int32_t tmp5 = (int32_t) source[2] - source[5];
        int32_t tmp3 = (int32_t) source[3] + source[4] - 256;
        int32_t tmp4 = (int32_t) source[3] - source[4];

        int32_t tmp10 = tmp0 + tmp3;
        int32_t tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2;
        int32_t tmp12 = tmp1 - tmp2;

        target[0] = (tmp10 + tmp11) << JPEG_PASS1_BITS;
        target[4] = (tmp10 - tmp11) << JPEG_PASS1_BITS;

        int32_t z1 = (tmp12 + tmp13) * FIX_0_541196100;

        target[2] = descale(
            z1 + tmp13 * FIX_0_765366865, JPEG_CONST_BITS - JPEG_PASS1_BITS
        );
        target[6] = descale(
            z1 + tmp12 * -FIX_1_847759065, JPEG_CONST_BITS - JPEG_PASS1_BITS
        );

        z1 = tmp4 + tmp7;

        int32_t z2 = tmp5 + tmp6;
        int32_t z3 = tmp4 + tmp6;
        int32_t z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 = tmp4 * FIX_0_298631336;
        tmp5 = tmp5 * FIX_2_053119869;
        tmp6 = tmp6 * FIX_3_072711026;
        tmp7 = tmp7 * FIX_1_501321110;

        z1 = z1 * -FIX_0_899976223;
        z2 = z2 * -FIX_2_562915447;
        z3 = z3 * -FIX_1_961570560 + z5;
        z4 = z4 * -FIX_0_390180644 + z5;

        target[7] = descale(tmp4 + z1 + z3, JPEG_CONST_BITS - JPEG_PASS1_BITS);
        target[5] = descale(tmp5 + z2 + z4, JPEG_CONST_BITS - JPEG_PASS1_BITS);
        target[3] = descale(tmp6 + z2 + z3, JPEG_CONST_BITS - JPEG_PASS1_BITS);
        target[1] = descale(tmp7 + z1 + z4, JPEG_CONST_BITS - JPEG_PASS1_BITS);
    }

    for (uint32_t column = 0; column < 8; column++) {
        const int32_t* source = workspace + column;

        int32_t tmp0 = source[0] + source[56];
        int32_t tmp7 = source[0] - source[56];
        int32_t tmp1 = source[8] + source[48];
        int32_t tmp6 = source[8] - source[48];
        int32_t tmp2 = source[16] + source[40];
        int32_t tmp5 = source[16] - source[40];
        int32_t tmp3 = source[24] + source[32];
        int32_t tmp4 = source[24] - source[32];

        int32_t tmp10 = tmp0 + tmp3;
        int32_t tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2;
        int32_t tmp12 = tmp1 - tmp2;

        out[column] = descale(tmp10 + tmp11, JPEG_PASS1_BITS);
        out[32 + column] = descale(tmp10 - tmp11, JPEG_PASS1_BITS);

        int32_t z1 = (tmp12 + tmp13) * FIX_0_541196100;

        out[16 + column] = descale(
            z1 + tmp13 * FIX_0_765366865, JPEG_CONST_BITS + JPEG_PASS1_BITS
        );
        out[48 + column] = descale(
            z1 + tmp12 * -FIX_1_847759065, JPEG_CONST_BITS + JPEG_PASS1_BITS
        );

        z1 = tmp4 + tmp7;

        int32_t z2 = tmp5 + tmp6;
        int32_t z3 = tmp4 + tmp6;
        int32_t z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 = tmp4 * FIX_0_298631336;
        tmp5 = tmp5 * FIX_2_053119869;
        tmp6 = tmp6 * FIX_3_072711026;
        tmp7 = tmp7 * FIX_1_501321110;

        z1 = z1 * -FIX_0_899976223;
        z2 = z2 * -FIX_2_562915447;
        z3 = z3 * -FIX_1_961570560 + z5;
        z4 = z4 * -FIX_0_390180644 + z5;

        out[56 + column] =
            descale(tmp4 + z1 + z3, JPEG_CONST_BITS + JPEG_PASS1_BITS);
        out[40 + column] =
            descale(tmp5 + z2 + z4, JPEG_CONST_BITS + JPEG_PASS1_BITS);
        out[24 + column] =
            descale(tmp6 + z2 + z3, JPEG_CONST_BITS + JPEG_PASS1_BITS);
        out[8 + column] =
            descale(tmp7 + z1 + z4, JPEG_CONST_BITS + JPEG_PASS1_BITS);
    }
}

#pragma endregion

#pragma region quantisation tables

/**
 * The example tables from the standard's Annex K.
 *
 * Not derivable and not replaceable by anything this codec could compute, so
 * they are the one thing it carries. mozjpeg ships tables tuned past these, but
 * writing down numbers attributed to a project without having them in front of
 * me is worse than using the ones the standard states, so the encoder takes its
 * gains from optimised Huffman coding and the progressive script instead.
 */
static const uint8_t annex_k_luma[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};

static const uint8_t annex_k_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

/** The libjpeg quality mapping, so a quality number means what it does there.
 */
static void scale_quant(uint16_t* out, const uint8_t* base, uint32_t quality) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    uint32_t scale = quality < 50 ? 5000u / quality : 200u - quality * 2u;

    for (uint32_t i = 0; i < 64; i++) {
        uint32_t value = (base[i] * scale + 50u) / 100u;

        if (value < 1) value = 1;
        if (value > 255) value = 255;

        out[i] = (uint16_t) value;
    }
}

#pragma endregion

#pragma region huffman tables for writing

typedef struct {
    uint32_t code[256];
    uint8_t size[256];
    uint8_t bits[17];
    uint8_t values[256];
    uint32_t total;
} JpegEncodeTable;

/**
 * Builds the shortest legal code lengths for a set of symbol frequencies.
 *
 * The tree is built by repeatedly merging the two least frequent symbols, then
 * lengths past sixteen are folded back in, which the format requires. A
 * pseudo-symbol with frequency one is counted and then removed at the end, so
 * that no real symbol receives the all ones code; a decoder is entitled to
 * treat that code as a marker.
 */
static void optimal_lengths(const uint32_t* frequencies, JpegEncodeTable* out) {
    uint32_t freq[257];
    int32_t others[257];
    uint8_t lengths[257];
    uint32_t bits[33];

    for (uint32_t i = 0; i < 256; i++) {
        freq[i] = frequencies[i];
        others[i] = -1;
        lengths[i] = 0;
    }

    freq[256] = 1;
    others[256] = -1;
    lengths[256] = 0;

    for (;;) {
        // ties go to the higher symbol, which is what keeps the result
        // reproducible rather than dependent on the scan order
        int32_t first = -1;
        int32_t second = -1;
        uint32_t best = 0xFFFFFFFFu;

        for (uint32_t i = 0; i <= 256; i++) {
            if (freq[i] && freq[i] <= best) {
                best = freq[i];
                first = (int32_t) i;
            }
        }

        best = 0xFFFFFFFFu;

        for (uint32_t i = 0; i <= 256; i++) {
            if (freq[i] && freq[i] <= best && (int32_t) i != first) {
                best = freq[i];
                second = (int32_t) i;
            }
        }

        if (second < 0) break;

        freq[first] += freq[second];
        freq[second] = 0;

        lengths[first]++;
        while (others[first] >= 0) {
            first = others[first];
            lengths[first]++;
        }

        others[first] = second;

        lengths[second]++;
        while (others[second] >= 0) {
            second = others[second];
            lengths[second]++;
        }
    }

    for (uint32_t i = 0; i < 33; i++) bits[i] = 0;
    for (uint32_t i = 0; i <= 256; i++) {
        if (lengths[i]) bits[lengths[i]]++;
    }

    // a pure Huffman tree can reach 32 bits on a skewed distribution; two
    // symbols at the longest length become one there and two one step deeper in
    // an existing prefix, which is the standard adjustment
    for (uint32_t i = 32; i > 16; i--) {
        while (bits[i] > 0) {
            uint32_t j = i - 2;
            while (bits[j] == 0) j--;

            bits[i] -= 2;
            bits[i - 1]++;
            bits[j + 1] += 2;
            bits[j]--;
        }
    }

    uint32_t longest = 16;
    while (longest > 0 && bits[longest] == 0) longest--;
    if (longest > 0) bits[longest]--;

    out->bits[0] = 0;
    for (uint32_t i = 1; i <= 16; i++) out->bits[i] = (uint8_t) bits[i];

    // sorted by the unadjusted length, which is all the canonical assignment
    // needs: the counts above fix the lengths and this fixes the order
    uint32_t at = 0;
    for (uint32_t length = 1; length <= 32; length++) {
        for (uint32_t symbol = 0; symbol < 256; symbol++) {
            if (lengths[symbol] == length) out->values[at++] = (uint8_t) symbol;
        }
    }

    out->total = at;
}

/** Turns code lengths into the code each symbol is written with. */
static void assign_codes(JpegEncodeTable* table) {
    for (uint32_t i = 0; i < 256; i++) {
        table->code[i] = 0;
        table->size[i] = 0;
    }

    uint32_t code = 0;
    uint32_t at = 0;

    for (uint32_t length = 1; length <= 16; length++) {
        for (uint32_t i = 0; i < table->bits[length]; i++) {
            table->code[table->values[at]] = code++;
            table->size[table->values[at]] = (uint8_t) length;
            at++;
        }

        code <<= 1;
    }
}

#pragma endregion

#pragma region emitter

typedef struct {
    TinyWriter* out;
    uint32_t accumulator;
    uint32_t count;
} JpegEmitter;

/** Appends bits, stuffing a zero after any 0xFF so it cannot read as a marker.
 */
static void emit_bits(JpegEmitter* emitter, uint32_t code, uint32_t size) {
    if (size == 0) return;

    emitter->accumulator =
        (emitter->accumulator << size) | (code & ((1u << size) - 1u));
    emitter->count += size;

    while (emitter->count >= 8) {
        emitter->count -= 8;

        uint8_t byte =
            (uint8_t) ((emitter->accumulator >> emitter->count) & 0xFFu);

        tiny_writer_u8(emitter->out, byte);
        if (byte == 0xFF) tiny_writer_u8(emitter->out, 0);
    }
}

/** Pads the last byte with one bits, which is what the format's segments use.
 */
static void emit_flush(JpegEmitter* emitter) {
    if (emitter->count > 0) {
        uint32_t pad = 8 - emitter->count;
        emit_bits(emitter, (1u << pad) - 1u, pad);
    }

    emitter->accumulator = 0;
    emitter->count = 0;
}

#pragma endregion

#pragma region encoder

#define JPEG_MAX_ENCODE_COMPONENTS 3
#define JPEG_MAX_CORRECTIONS 1000

typedef struct {
    uint8_t id;
    uint8_t h;
    uint8_t v;
    uint8_t quant_table;
    uint8_t dc_table;
    uint8_t ac_table;

    uint32_t blocks_x;
    uint32_t blocks_y;
    uint32_t used_x;
    uint32_t used_y;

    uint8_t* plane;
    uint32_t stride;

    int16_t* coefficients;
    int32_t dc_pred;
} JpegEncodeComponent;

typedef struct {
    const TinyImage* image;
    TinyWriter* out;

    uint8_t count;
    uint8_t max_h;
    uint8_t max_v;
    uint8_t progressive;
    uint32_t mcus_x;
    uint32_t mcus_y;

    uint16_t quant[2][64];
    uint8_t quant_tables;

    JpegEncodeComponent components[JPEG_MAX_ENCODE_COMPONENTS];

    JpegEncodeTable dc[2];
    JpegEncodeTable ac[2];

    JpegEmitter emitter;

    /** The scan in progress. */
    uint8_t scan_count;
    JpegEncodeComponent* scan[JPEG_MAX_ENCODE_COMPONENTS];
    uint8_t ss;
    uint8_t se;
    uint8_t ah;
    uint8_t al;

    /** Counting rather than writing, for the pass that measures frequencies. */
    uint8_t counting;
    uint32_t dc_frequencies[2][256];
    uint32_t ac_frequencies[2][256];

    uint32_t eobrun;
    /** Correction bits waiting on the end of band run they sit behind. */
    uint32_t buffered;
    uint8_t correction[JPEG_MAX_CORRECTIONS];
} JpegEncoder;

/** Adds a symbol to a frequency count, or writes it, depending on the pass. */
static void put_symbol(
    JpegEncoder* encoder, int is_ac, uint32_t table, uint32_t symbol
) {
    if (encoder->counting) {
        if (is_ac) {
            encoder->ac_frequencies[table][symbol]++;
        }
        else {
            encoder->dc_frequencies[table][symbol]++;
        }
        return;
    }

    const JpegEncodeTable* codes =
        is_ac ? &encoder->ac[table] : &encoder->dc[table];

    emit_bits(&encoder->emitter, codes->code[symbol], codes->size[symbol]);
}

static void put_value(JpegEncoder* encoder, uint32_t value, uint32_t size) {
    if (!encoder->counting) emit_bits(&encoder->emitter, value, size);
}

/**
 * How many bits a value's magnitude needs, and the payload written after it.
 *
 * A negative value is written as the bitwise complement of its magnitude, which
 * is what makes the decoder's sign extension the exact inverse.
 */
static uint32_t magnitude(int32_t value, uint32_t* payload) {
    uint32_t absolute = (uint32_t) (value < 0 ? -value : value);
    uint32_t scan = absolute;
    uint32_t bits = 0;

    while (scan) {
        bits++;
        scan >>= 1;
    }

    *payload = value < 0 ? ~absolute : absolute;
    return bits;
}

static void encode_baseline_block(
    JpegEncoder* encoder, JpegEncodeComponent* component, const int16_t* block
) {
    uint32_t payload = 0;
    uint32_t bits = magnitude(block[0] - component->dc_pred, &payload);

    component->dc_pred = block[0];

    put_symbol(encoder, 0, component->dc_table, bits);
    put_value(encoder, payload, bits);

    uint32_t run = 0;

    for (uint32_t k = 1; k < 64; k++) {
        int32_t value = block[zigzag[k]];

        if (value == 0) {
            run++;
            continue;
        }

        while (run > 15) {
            put_symbol(encoder, 1, component->ac_table, 0xF0);
            run -= 16;
        }

        bits = magnitude(value, &payload);

        put_symbol(encoder, 1, component->ac_table, (run << 4) | bits);
        put_value(encoder, payload, bits);

        run = 0;
    }

    if (run > 0) put_symbol(encoder, 1, component->ac_table, 0);
}

static void encode_dc_first(
    JpegEncoder* encoder, JpegEncodeComponent* component, const int16_t* block
) {
    int32_t value = block[0] >> encoder->al;
    uint32_t payload = 0;
    uint32_t bits = magnitude(value - component->dc_pred, &payload);

    component->dc_pred = value;

    put_symbol(encoder, 0, component->dc_table, bits);
    put_value(encoder, payload, bits);
}

static void encode_dc_refine(JpegEncoder* encoder, const int16_t* block) {
    put_value(encoder, (uint32_t) (block[0] >> encoder->al) & 1u, 1);
}

/**
 * Writes a pending end of band run and the correction bits held behind it.
 *
 * A run cannot be written when it starts, because its length is only known once
 * a block interrupts it, and any correction bits produced while it was
 * accumulating belong after the run's own symbol.
 */
static void flush_eobrun(JpegEncoder* encoder, uint32_t table) {
    if (encoder->eobrun == 0) return;

    uint32_t run = encoder->eobrun;
    uint32_t nbits = 0;

    while (run >>= 1) nbits++;

    put_symbol(encoder, 1, table, nbits << 4);
    if (nbits) put_value(encoder, encoder->eobrun, nbits);

    for (uint32_t i = 0; i < encoder->buffered; i++) {
        put_value(encoder, encoder->correction[i], 1);
    }

    encoder->eobrun = 0;
    encoder->buffered = 0;
}

static void encode_ac_first(
    JpegEncoder* encoder, JpegEncodeComponent* component, const int16_t* block
) {
    uint32_t table = component->ac_table;
    uint32_t run = 0;

    for (uint32_t k = encoder->ss; k <= encoder->se; k++) {
        int32_t value = block[zigzag[k]];
        int negative = value < 0;
        uint32_t absolute = (uint32_t) (negative ? -value : value);

        // the point transform divides toward zero, so the shift goes on the
        // magnitude; a coefficient can survive the file and not the transform
        absolute >>= encoder->al;

        if (absolute == 0) {
            run++;
            continue;
        }

        flush_eobrun(encoder, table);

        while (run > 15) {
            put_symbol(encoder, 1, table, 0xF0);
            run -= 16;
        }

        uint32_t payload = 0;
        uint32_t bits = magnitude(
            negative ? -(int32_t) absolute : (int32_t) absolute, &payload
        );

        put_symbol(encoder, 1, table, (run << 4) | bits);
        put_value(encoder, payload, bits);

        run = 0;
    }

    if (run > 0) {
        encoder->eobrun++;
        if (encoder->eobrun == 0x7FFF) flush_eobrun(encoder, table);
    }
}

/**
 * Appends one bit of precision to the AC coefficients of one block.
 *
 * A coefficient that was already non-zero takes a correction bit and does not
 * interrupt the zero run, so the bits have to be held until whatever ends the
 * run is written. The pre-pass finds the last coefficient this scan makes
 * non-zero: past it every remaining run folds into the end of band symbol, so a
 * zero run marker there would be wasted bits.
 */
static void encode_ac_refine(
    JpegEncoder* encoder, JpegEncodeComponent* component, const int16_t* block
) {
    uint32_t table = component->ac_table;
    uint32_t absolutes[64];
    uint32_t last = 0;

    for (uint32_t k = encoder->ss; k <= encoder->se; k++) {
        int32_t value = block[zigzag[k]];
        uint32_t absolute = (uint32_t) (value < 0 ? -value : value);

        absolutes[k] = absolute >> encoder->al;
        if (absolutes[k] == 1) last = k;
    }

    uint32_t run = 0;
    uint32_t added = 0;
    uint8_t* buffer = encoder->correction + encoder->buffered;

    for (uint32_t k = encoder->ss; k <= encoder->se; k++) {
        if (absolutes[k] == 0) {
            run++;
            continue;
        }

        while (run > 15 && k <= last) {
            flush_eobrun(encoder, table);
            put_symbol(encoder, 1, table, 0xF0);
            run -= 16;

            for (uint32_t i = 0; i < added; i++) {
                put_value(encoder, buffer[i], 1);
            }

            buffer = encoder->correction;
            added = 0;
        }

        if (absolutes[k] > 1) {
            if (encoder->buffered + added < JPEG_MAX_CORRECTIONS) {
                buffer[added] = (uint8_t) (absolutes[k] & 1u);
            }
            added++;
            continue;
        }

        flush_eobrun(encoder, table);

        put_symbol(encoder, 1, table, (run << 4) | 1u);
        put_value(encoder, block[zigzag[k]] < 0 ? 0u : 1u, 1);

        for (uint32_t i = 0; i < added; i++) put_value(encoder, buffer[i], 1);

        buffer = encoder->correction;
        added = 0;
        run = 0;
    }

    if (run > 0 || added > 0) {
        encoder->eobrun++;
        encoder->buffered += added;

        // forced out before either the run counter or the correction buffer can
        // overflow during the next block
        if (encoder->eobrun == 0x7FFF ||
            encoder->buffered > JPEG_MAX_CORRECTIONS - 64) {
            flush_eobrun(encoder, table);
        }
    }
}

/** Rec. 601 forward, in the same 16 bit fixed point as the inverse. */
static void rgb_to_ycbcr(uint32_t r, uint32_t g, uint32_t b, uint8_t* out) {
    // 19595 + 38470 + 7471 is exactly 65536, and each chroma row sums to zero,
    // so a grey input produces a flat 128 rather than drifting
    out[0] = (uint8_t) ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16);
    out[1] =
        tiny_clamp_u8((int32_t) ((-11059 * (int32_t) r - 21709 * (int32_t) g +
                                  32768 * (int32_t) b + 32768 + (128 << 16)) >>
                                 16));
    out[2] =
        tiny_clamp_u8((int32_t) ((32768 * (int32_t) r - 27439 * (int32_t) g -
                                  5329 * (int32_t) b + 32768 + (128 << 16)) >>
                                 16));
}

/**
 * Fills the component planes from the image, converting and subsampling.
 *
 * Chroma is averaged over the pixels it covers rather than point sampled, and
 * the average counts only real pixels, which at the right and bottom edges is
 * the same answer as replicating them and cheaper to say.
 */
static int build_planes(JpegEncoder* encoder) {
    const TinyImage* image = encoder->image;
    uint8_t channels = image->channels;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t chroma_width =
        encoder->count == 1
            ? 0
            : ceil_div(image->width, encoder->max_h / encoder->components[1].h);

    uint32_t* sums = 0;
    uint32_t* counts = 0;

    if (chroma_width) {
        sums =
            tiny_arena_alloc((size_t) chroma_width * 2 * sizeof(uint32_t), 0);
        counts = tiny_arena_alloc((size_t) chroma_width * sizeof(uint32_t), 0);

        if (!sums || !counts) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }
    }

    uint32_t v_ratio =
        encoder->count == 1 ? 1 : encoder->max_v / encoder->components[1].v;
    uint32_t h_ratio =
        encoder->count == 1 ? 1 : encoder->max_h / encoder->components[1].h;

    for (uint32_t y = 0; y < image->height; y++) {
        if (chroma_width && y % v_ratio == 0) {
            tiny_memset(sums, 0, (size_t) chroma_width * 2 * sizeof(uint32_t));
            tiny_memset(counts, 0, (size_t) chroma_width * sizeof(uint32_t));
        }

        const uint8_t* source =
            image->data + (size_t) y * image->width * channels;
        uint8_t* luma = encoder->components[0].plane +
                        (size_t) y * encoder->components[0].stride;

        for (uint32_t x = 0; x < image->width; x++) {
            const uint8_t* pixel = source + (size_t) x * channels;
            uint8_t rgba[4];

            tiny_pixel_convert(rgba, 3, pixel, channels);

            if (encoder->count == 1) {
                luma[x] = tiny_luma(rgba[0], rgba[1], rgba[2]);
                continue;
            }

            uint8_t ycc[3];
            rgb_to_ycbcr(rgba[0], rgba[1], rgba[2], ycc);

            luma[x] = ycc[0];

            uint32_t at = x / h_ratio;
            sums[at * 2] += ycc[1];
            sums[at * 2 + 1] += ycc[2];
            counts[at]++;
        }

        if (!chroma_width) continue;

        // written on the last source row of each group, so a group cut short by
        // the image's height still averages what it has
        if (y % v_ratio == v_ratio - 1 || y == image->height - 1) {
            uint32_t row = y / v_ratio;

            for (uint32_t c = 1; c < 3; c++) {
                uint8_t* plane = encoder->components[c].plane +
                                 (size_t) row * encoder->components[c].stride;

                for (uint32_t at = 0; at < chroma_width; at++) {
                    uint32_t n = counts[at] ? counts[at] : 1;

                    plane[at] =
                        (uint8_t) ((sums[at * 2 + (c - 1)] + n / 2) / n);
                }
            }
        }
    }

    // the transform works on whole blocks, so the padding past the image is the
    // edge repeated rather than whatever the allocation held
    for (uint32_t i = 0; i < encoder->count; i++) {
        JpegEncodeComponent* component = &encoder->components[i];

        uint32_t real_width =
            ceil_div(image->width * component->h, encoder->max_h);
        uint32_t real_height =
            ceil_div(image->height * component->v, encoder->max_v);
        uint32_t rows = component->blocks_y * 8;

        for (uint32_t y = 0; y < real_height; y++) {
            uint8_t* row = component->plane + (size_t) y * component->stride;

            for (uint32_t x = real_width; x < component->stride; x++) {
                row[x] = row[real_width - 1];
            }
        }

        for (uint32_t y = real_height; y < rows; y++) {
            tiny_memcpy(
                component->plane + (size_t) y * component->stride,
                component->plane +
                    (size_t) (real_height - 1) * component->stride,
                component->stride
            );
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

/** Transforms and quantises every block of every component. */
static void build_coefficients(JpegEncoder* encoder) {
    for (uint32_t i = 0; i < encoder->count; i++) {
        JpegEncodeComponent* component = &encoder->components[i];
        const uint16_t* quant = encoder->quant[component->quant_table];

        for (uint32_t by = 0; by < component->blocks_y; by++) {
            for (uint32_t bx = 0; bx < component->blocks_x; bx++) {
                int32_t work[64];

                fdct_8x8(
                    component->plane + (size_t) by * 8 * component->stride +
                        (size_t) bx * 8,
                    component->stride, work
                );

                int16_t* block = component->coefficients +
                                 ((size_t) by * component->blocks_x + bx) * 64;

                for (uint32_t k = 0; k < 64; k++) {
                    // the transform's output is eight times the true one, which
                    // the divisor absorbs; rounding is away from zero, so a
                    // coefficient never drifts toward the sign it started on
                    int32_t divisor = (int32_t) quant[k] << 3;
                    int32_t value = work[k];
                    int negative = value < 0;

                    if (negative) value = -value;

                    value = (value + (divisor >> 1)) / divisor;
                    block[k] = (int16_t) (negative ? -value : value);
                }
            }
        }
    }
}

/** One scan, either counting the symbols it would write or writing them. */
static void run_scan(JpegEncoder* encoder) {
    for (uint32_t i = 0; i < encoder->scan_count; i++) {
        encoder->scan[i]->dc_pred = 0;
    }

    encoder->eobrun = 0;
    encoder->buffered = 0;

    uint32_t mcus_x = encoder->mcus_x;
    uint32_t mcus_y = encoder->mcus_y;

    if (encoder->scan_count == 1) {
        mcus_x = encoder->scan[0]->used_x;
        mcus_y = encoder->scan[0]->used_y;
    }

    for (uint32_t my = 0; my < mcus_y; my++) {
        for (uint32_t mx = 0; mx < mcus_x; mx++) {
            for (uint32_t i = 0; i < encoder->scan_count; i++) {
                JpegEncodeComponent* component = encoder->scan[i];

                uint32_t wide = encoder->scan_count == 1 ? 1 : component->h;
                uint32_t high = encoder->scan_count == 1 ? 1 : component->v;

                for (uint32_t by = 0; by < high; by++) {
                    for (uint32_t bx = 0; bx < wide; bx++) {
                        uint32_t block_x =
                            encoder->scan_count == 1 ? mx : mx * wide + bx;
                        uint32_t block_y =
                            encoder->scan_count == 1 ? my : my * high + by;

                        const int16_t* block =
                            component->coefficients +
                            ((size_t) block_y * component->blocks_x + block_x) *
                                64;

                        if (!encoder->progressive) {
                            encode_baseline_block(encoder, component, block);
                        }
                        else if (encoder->ss == 0) {
                            if (encoder->ah == 0) {
                                encode_dc_first(encoder, component, block);
                            }
                            else {
                                encode_dc_refine(encoder, block);
                            }
                        }
                        else if (encoder->ah == 0) {
                            encode_ac_first(encoder, component, block);
                        }
                        else {
                            encode_ac_refine(encoder, component, block);
                        }
                    }
                }
            }
        }
    }

    flush_eobrun(encoder, encoder->scan[0]->ac_table);
    if (!encoder->counting) emit_flush(&encoder->emitter);
}

#pragma endregion

#pragma region encode markers

static void write_marker(TinyWriter* out, uint8_t marker) {
    tiny_writer_u8(out, 0xFF);
    tiny_writer_u8(out, marker);
}

static void write_jfif(TinyWriter* out) {
    write_marker(out, JPEG_APP0);
    tiny_writer_be16(out, 16);
    tiny_writer_write(out, "JFIF", 5);
    tiny_writer_u8(out, 1);
    tiny_writer_u8(out, 1);
    tiny_writer_u8(out, 0);
    tiny_writer_be16(out, 1);
    tiny_writer_be16(out, 1);
    tiny_writer_u8(out, 0);
    tiny_writer_u8(out, 0);
}

/** Writes the image's EXIF back out, which is what makes a re-encode lossless
 * for metadata. */
static int write_exif(JpegEncoder* encoder) {
    if (tiny_image_has_exif(encoder->image) != 1) return TINYIMG_OK;

    char* exif = 0;
    size_t size = 0;

    int result = tiny_image_get_exif(encoder->image, &exif, &size);
    if (result != TINYIMG_OK) return result;

    // the segment carries a two byte length, so a payload that cannot fit is
    // dropped rather than written truncated
    if (size + 8 <= 0xFFFFu) {
        write_marker(encoder->out, JPEG_APP1);
        tiny_writer_be16(encoder->out, (uint16_t) (size + 8));
        tiny_writer_write(encoder->out, "Exif", 5);
        tiny_writer_u8(encoder->out, 0);
        tiny_writer_write(encoder->out, exif, size);
    }

    tiny_free(exif);
    return TINYIMG_OK;
}

static void write_quant_tables(JpegEncoder* encoder) {
    for (uint32_t slot = 0; slot < encoder->quant_tables; slot++) {
        write_marker(encoder->out, JPEG_DQT);
        tiny_writer_be16(encoder->out, 67);
        tiny_writer_u8(encoder->out, (uint8_t) slot);

        for (uint32_t i = 0; i < 64; i++) {
            tiny_writer_u8(
                encoder->out, (uint8_t) encoder->quant[slot][zigzag[i]]
            );
        }
    }
}

static void write_frame(JpegEncoder* encoder) {
    write_marker(encoder->out, encoder->progressive ? JPEG_SOF2 : JPEG_SOF0);
    tiny_writer_be16(encoder->out, (uint16_t) (8 + encoder->count * 3));
    tiny_writer_u8(encoder->out, 8);
    tiny_writer_be16(encoder->out, (uint16_t) encoder->image->height);
    tiny_writer_be16(encoder->out, (uint16_t) encoder->image->width);
    tiny_writer_u8(encoder->out, encoder->count);

    for (uint32_t i = 0; i < encoder->count; i++) {
        const JpegEncodeComponent* component = &encoder->components[i];

        tiny_writer_u8(encoder->out, component->id);
        tiny_writer_u8(
            encoder->out, (uint8_t) ((component->h << 4) | component->v)
        );
        tiny_writer_u8(encoder->out, component->quant_table);
    }
}

static void write_huffman_table(
    JpegEncoder* encoder, uint32_t is_ac, uint32_t slot
) {
    const JpegEncodeTable* table =
        is_ac ? &encoder->ac[slot] : &encoder->dc[slot];

    uint32_t total = 0;
    for (uint32_t i = 1; i <= 16; i++) total += table->bits[i];

    write_marker(encoder->out, JPEG_DHT);
    tiny_writer_be16(encoder->out, (uint16_t) (19 + total));
    tiny_writer_u8(encoder->out, (uint8_t) ((is_ac << 4) | slot));

    for (uint32_t i = 1; i <= 16; i++) {
        tiny_writer_u8(encoder->out, table->bits[i]);
    }

    tiny_writer_write(encoder->out, table->values, total);
}

static void write_scan_header(JpegEncoder* encoder) {
    write_marker(encoder->out, JPEG_SOS);
    tiny_writer_be16(encoder->out, (uint16_t) (6 + encoder->scan_count * 2));
    tiny_writer_u8(encoder->out, encoder->scan_count);

    for (uint32_t i = 0; i < encoder->scan_count; i++) {
        const JpegEncodeComponent* component = encoder->scan[i];

        tiny_writer_u8(encoder->out, component->id);
        tiny_writer_u8(
            encoder->out,
            (uint8_t) ((component->dc_table << 4) | component->ac_table)
        );
    }

    tiny_writer_u8(encoder->out, encoder->ss);
    tiny_writer_u8(encoder->out, encoder->se);
    tiny_writer_u8(encoder->out, (uint8_t) ((encoder->ah << 4) | encoder->al));
}

/**
 * Measures one scan, builds the tables it needs, writes them and then writes
 * it.
 *
 * Two passes over the coefficients rather than one, which is what optimised
 * tables cost: the frequencies are not known until every symbol the scan will
 * write has been seen. It is worth between two and six percent against the
 * example tables in the standard, on every image measured.
 */
static void emit_scan(JpegEncoder* encoder) {
    for (uint32_t slot = 0; slot < 2; slot++) {
        for (uint32_t i = 0; i < 256; i++) {
            encoder->dc_frequencies[slot][i] = 0;
            encoder->ac_frequencies[slot][i] = 0;
        }
    }

    encoder->counting = 1;
    run_scan(encoder);
    encoder->counting = 0;

    int dc_used[2] = {0, 0};
    int ac_used[2] = {0, 0};

    for (uint32_t i = 0; i < encoder->scan_count; i++) {
        // a scan that carries no DC coefficients needs no DC table, and one
        // that carries only DC needs no AC table
        if (encoder->ss == 0) dc_used[encoder->scan[i]->dc_table] = 1;
        if (encoder->se > 0) ac_used[encoder->scan[i]->ac_table] = 1;
    }

    // a refinement scan writes raw bits for the DC band, so it names a table it
    // never reads from
    if (encoder->ss == 0 && encoder->ah != 0) {
        dc_used[0] = 0;
        dc_used[1] = 0;
    }

    for (uint32_t slot = 0; slot < 2; slot++) {
        if (dc_used[slot]) {
            optimal_lengths(encoder->dc_frequencies[slot], &encoder->dc[slot]);
            assign_codes(&encoder->dc[slot]);
            write_huffman_table(encoder, 0, slot);
        }
        if (ac_used[slot]) {
            optimal_lengths(encoder->ac_frequencies[slot], &encoder->ac[slot]);
            assign_codes(&encoder->ac[slot]);
            write_huffman_table(encoder, 1, slot);
        }
    }

    write_scan_header(encoder);
    run_scan(encoder);
}

#pragma endregion

#pragma region encode

/**
 * The progressive scan script, which is the one libjpeg calls simple.
 *
 * Each row is the components it covers, the coefficient band, and the bit
 * positions it refines from and to. The DC band goes out first at one bit less
 * than full precision so an early preview is stable, chroma follows luma
 * because chroma converges faster, and the refinement scans come last.
 */
typedef struct {
    uint8_t components;
    uint8_t which[3];
    uint8_t ss;
    uint8_t se;
    uint8_t ah;
    uint8_t al;
} JpegScanSpec;

static const JpegScanSpec colour_script[10] = {
    {3, {0, 1, 2}, 0, 0, 0, 1},  {1, {0, 0, 0}, 1, 5, 0, 2},
    {1, {2, 0, 0}, 1, 63, 0, 1}, {1, {1, 0, 0}, 1, 63, 0, 1},
    {1, {0, 0, 0}, 6, 63, 0, 2}, {1, {0, 0, 0}, 1, 63, 2, 1},
    {3, {0, 1, 2}, 0, 0, 1, 0},  {1, {2, 0, 0}, 1, 63, 1, 0},
    {1, {1, 0, 0}, 1, 63, 1, 0}, {1, {0, 0, 0}, 1, 63, 1, 0}
};

/**
 * The same shape for one component, and it has to end with a DC refinement.
 *
 * Leaving that scan out costs the DC band's lowest bit for good, which is a
 * silent quality loss rather than an error: the file decodes, and every
 * coefficient is one step coarser than the quality asked for. Caught by
 * requiring a progressive encode and a baseline one to decode to the same
 * pixels, which is the only check that could see it.
 */
static const JpegScanSpec grey_script[6] = {
    {1, {0, 0, 0}, 0, 0, 0, 1},  {1, {0, 0, 0}, 1, 5, 0, 2},
    {1, {0, 0, 0}, 6, 63, 0, 2}, {1, {0, 0, 0}, 1, 63, 2, 1},
    {1, {0, 0, 0}, 0, 0, 1, 0},  {1, {0, 0, 0}, 1, 63, 1, 0}
};

static int jpeg_encode(
    const TinyImage* image, const TinyEncodeOpts* options, TinyWriter* writer
) {
    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;

    // the format's dimensions are 16 bit, and nothing scales that away
    if (image->width > 65535 || image->height > 65535) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    JpegEncoder* encoder = tiny_arena_alloc(sizeof(JpegEncoder), 0);
    if (!encoder) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(encoder, 0, sizeof(*encoder));
    encoder->image = image;
    encoder->out = writer;
    encoder->emitter.out = writer;

    uint32_t quality = options && options->quality ? options->quality : 85u;
    if (quality > 100) quality = 100;

    encoder->progressive = options ? options->progressive : 0;

    // JPEG carries no alpha, so a channel it cannot represent is dropped rather
    // than composited against a background this codec would have to invent
    encoder->count = (uint8_t) (image->channels <= 2 ? 1 : 3);

    if (encoder->count == 1) {
        encoder->components[0].id = 1;
        encoder->components[0].h = 1;
        encoder->components[0].v = 1;
        encoder->quant_tables = 1;
    }
    else {
        // chroma at full resolution above quality 90, where halving it is the
        // dominant error, and halved below, where it is nearly free
        uint8_t sub = (uint8_t) (quality >= 90 ? 1 : 2);

        for (uint32_t i = 0; i < 3; i++) {
            encoder->components[i].id = (uint8_t) (i + 1);
            encoder->components[i].h = i == 0 ? sub : 1;
            encoder->components[i].v = i == 0 ? sub : 1;
            encoder->components[i].quant_table = (uint8_t) (i == 0 ? 0 : 1);
            encoder->components[i].dc_table = (uint8_t) (i == 0 ? 0 : 1);
            encoder->components[i].ac_table = (uint8_t) (i == 0 ? 0 : 1);
        }

        encoder->quant_tables = 2;
    }

    encoder->max_h = encoder->components[0].h;
    encoder->max_v = encoder->components[0].v;

    scale_quant(encoder->quant[0], annex_k_luma, quality);
    if (encoder->quant_tables > 1) {
        scale_quant(encoder->quant[1], annex_k_chroma, quality);
    }

    encoder->mcus_x = ceil_div(image->width, (uint32_t) encoder->max_h * 8u);
    encoder->mcus_y = ceil_div(image->height, (uint32_t) encoder->max_v * 8u);

    int result = TINYIMG_OK;

    for (uint32_t i = 0; i < encoder->count && result == TINYIMG_OK; i++) {
        JpegEncodeComponent* component = &encoder->components[i];

        component->blocks_x = encoder->mcus_x * component->h;
        component->blocks_y = encoder->mcus_y * component->v;
        component->stride = component->blocks_x * 8;

        component->used_x =
            ceil_div(ceil_div(image->width * component->h, encoder->max_h), 8);
        component->used_y =
            ceil_div(ceil_div(image->height * component->v, encoder->max_v), 8);

        uint64_t plane = (uint64_t) component->stride * component->blocks_y * 8;
        uint64_t coefficients =
            (uint64_t) component->blocks_x * component->blocks_y * 64 * 2;

        if (plane > 0xFFFFFFFFu || coefficients > 0xFFFFFFFFu) {
            result = TINYIMG_ERR_MEMORY;
            break;
        }

        component->plane = tiny_arena_alloc((size_t) plane, 0);
        component->coefficients = tiny_arena_alloc((size_t) coefficients, 0);

        if (!component->plane || !component->coefficients) {
            result = TINYIMG_ERR_MEMORY;
        }
    }

    if (result == TINYIMG_OK) result = build_planes(encoder);

    if (result != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return result;
    }

    build_coefficients(encoder);

    write_marker(writer, JPEG_SOI);
    write_jfif(writer);

    if (!options || !options->strip_metadata) {
        result = write_exif(encoder);

        if (result != TINYIMG_OK) {
            tiny_arena_release(&mark);
            return result;
        }
    }

    write_quant_tables(encoder);
    write_frame(encoder);

    if (encoder->progressive) {
        const JpegScanSpec* script =
            encoder->count == 1 ? grey_script : colour_script;
        uint32_t scans = encoder->count == 1 ? 6u : 10u;

        for (uint32_t i = 0; i < scans; i++) {
            encoder->scan_count = script[i].components;

            for (uint32_t c = 0; c < script[i].components; c++) {
                encoder->scan[c] = &encoder->components[script[i].which[c]];
            }

            encoder->ss = script[i].ss;
            encoder->se = script[i].se;
            encoder->ah = script[i].ah;
            encoder->al = script[i].al;

            emit_scan(encoder);
        }
    }
    else {
        encoder->scan_count = encoder->count;

        for (uint32_t i = 0; i < encoder->count; i++) {
            encoder->scan[i] = &encoder->components[i];
        }

        encoder->ss = 0;
        encoder->se = 63;
        encoder->ah = 0;
        encoder->al = 0;

        emit_scan(encoder);
    }

    write_marker(writer, JPEG_EOI);

    tiny_arena_release(&mark);

    return writer->error;
}

#pragma endregion

const TinyCodec tiny_codec_jpeg = {
    TINYIMG_FORMAT_JPEG, jpeg_sniff, jpeg_probe, jpeg_decode, jpeg_encode
};
