#include "tinyimg/codec/webp.h"

#include "tinyimg/memory.h"
#include "tinyimg/work.h"
#include "vp8-tables.h"

#define WEBP_ALPHA_FLAG 0x10u
#define WEBP_ANIMATION_FLAG 0x02u

/** Codes never exceed fifteen bits, as in DEFLATE. */
#define VP8L_MAX_BITS 15u

#define VP8L_LITERALS 256u
#define VP8L_LENGTHS 24u
#define VP8L_DISTANCES 40u
#define VP8L_CODE_LENGTHS 19u
#define VP8L_MAX_CACHE_BITS 11u
#define VP8L_CODES_PER_GROUP 5u

/** How many of the 40 distance codes name a nearby pixel rather than a run. */
#define VP8L_PLANE_CODES 120u

#define VP8L_PREDICTOR 0u
#define VP8L_CROSS_COLOR 1u
#define VP8L_SUBTRACT_GREEN 2u
#define VP8L_COLOR_INDEX 3u

typedef struct {
    const uint8_t* data;
    size_t size;

    uint32_t width;
    uint32_t height;
    uint32_t frames;
    uint8_t has_alpha;
    uint8_t lossless;
    uint8_t animation;

    /** The `VP8 ` or `VP8L` payload of the still image or the first frame. */
    size_t bitstream_at;
    size_t bitstream_size;
    /** The `ALPH` payload that goes with a lossy frame, or zero. */
    size_t alpha_at;
    size_t alpha_size;
} WebpHeader;

#pragma region container

static uint32_t read_le24(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static int fourcc(const uint8_t* p, const char* tag) {
    return p[0] == (uint8_t) tag[0] && p[1] == (uint8_t) tag[1] &&
           p[2] == (uint8_t) tag[2] && p[3] == (uint8_t) tag[3];
}

static int webp_sniff(const uint8_t* buffer, size_t size) {
    if (!buffer || size < 12) return 0;

    return fourcc(buffer, "RIFF") && fourcc(buffer + 8, "WEBP");
}

/**
 * Reads a lossless stream's own header, which carries the dimensions.
 *
 * Five bytes: a signature, then two fourteen bit extents, an alpha hint and a
 * version. The hint is advisory, so it is read and not trusted; whether an
 * alpha channel survives the decode is decided by the pixels.
 */
static int parse_vp8l(
    const uint8_t* data, size_t size, uint32_t* width, uint32_t* height,
    uint8_t* has_alpha
) {
    if (size < 5) return TINYIMG_ERR_CORRUPT;
    if (data[0] != 0x2F) return TINYIMG_ERR_CORRUPT;

    uint32_t packed = read_le32(data + 1);

    *width = (packed & 0x3FFFu) + 1u;
    *height = ((packed >> 14) & 0x3FFFu) + 1u;
    *has_alpha = (uint8_t) ((packed >> 28) & 1u);

    // three version bits follow the hint, and only version zero is defined
    if (((packed >> 29) & 7u) != 0) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    return TINYIMG_OK;
}

/**
 * Reads a lossy keyframe's header, which carries the dimensions.
 *
 * The three byte frame tag comes first, then a start code that is the only
 * thing separating a keyframe from an interframe this codec cannot use.
 */
static int parse_vp8(
    const uint8_t* data, size_t size, uint32_t* width, uint32_t* height
) {
    if (size < 10) return TINYIMG_ERR_CORRUPT;

    uint32_t tag = read_le24(data);

    // bit zero clear means a keyframe; an interframe needs a reference frame
    // this codec never has, since a still image is one frame
    if ((tag & 1u) != 0) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A) {
        return TINYIMG_ERR_CORRUPT;
    }

    *width = ((uint32_t) data[6] | ((uint32_t) data[7] << 8)) & 0x3FFFu;
    *height = ((uint32_t) data[8] | ((uint32_t) data[9] << 8)) & 0x3FFFu;

    // the two high bits of each extent are an upscaling hint for the renderer
    // rather than part of the coded size, so they are deliberately dropped
    return TINYIMG_OK;
}

/**
 * Walks the RIFF chunks, recording where the pieces of the first image are.
 *
 * A simple file is one `VP8 ` or `VP8L` chunk and nothing else. An extended one
 * opens with `VP8X`, whose canvas extents win over the bitstream's own, and may
 * then carry alpha, color and metadata chunks in any order, so this records
 * offsets on the way past rather than assuming a layout.
 */
static int webp_parse(const uint8_t* buffer, size_t size, WebpHeader* header) {
    if (!webp_sniff(buffer, size)) return TINYIMG_ERR_UNKNOWN_FORMAT;

    tiny_memset(header, 0, sizeof(*header));

    header->data = buffer;
    header->size = size;
    header->frames = 0;

    // the RIFF length excludes the eight bytes before it, and a file whose
    // container claims less than it holds is read to the shorter of the two
    size_t limit = (size_t) read_le32(buffer + 4) + 8u;
    if (limit > size || limit < 12) limit = size;

    size_t at = 12;
    int seen_canvas = 0;
    int inside_frame = 0;
    size_t frame_end = 0;

    while (at + 8 <= limit) {
        const uint8_t* tag = buffer + at;
        uint32_t length = read_le32(buffer + at + 4);
        size_t payload = at + 8;

        if (length > limit - payload) length = (uint32_t) (limit - payload);

        if (fourcc(tag, "VP8X")) {
            if (length < 10) return TINYIMG_ERR_CORRUPT;

            uint32_t flags = read_le32(buffer + payload);

            header->has_alpha = (uint8_t) ((flags & WEBP_ALPHA_FLAG) != 0);
            header->animation = (uint8_t) ((flags & WEBP_ANIMATION_FLAG) != 0);
            header->width = read_le24(buffer + payload + 4) + 1u;
            header->height = read_le24(buffer + payload + 7) + 1u;

            seen_canvas = 1;
        }
        else if (fourcc(tag, "ANMF")) {
            header->frames++;

            // a frame is a container of its own, so the walk steps inside it
            // rather than over it, and the first one's chunks are the ones that
            // get recorded
            if (header->frames == 1 && length > 16) {
                inside_frame = 1;
                frame_end = payload + length;
                at = payload + 16;
                continue;
            }
        }
        else if (fourcc(tag, "ALPH")) {
            if (!header->alpha_at) {
                header->alpha_at = payload;
                header->alpha_size = length;
            }
        }
        else if (fourcc(tag, "VP8L") || fourcc(tag, "VP8 ")) {
            if (!header->bitstream_at) {
                header->bitstream_at = payload;
                header->bitstream_size = length;
                header->lossless = (uint8_t) fourcc(tag, "VP8L");
            }
        }

        // chunks are padded to an even length, and the padding byte is not
        // counted in the length field
        at = payload + length + (length & 1u);

        if (inside_frame && at >= frame_end) inside_frame = 0;
    }

    if (!header->bitstream_at) return TINYIMG_ERR_CORRUPT;

    const uint8_t* stream = buffer + header->bitstream_at;
    uint32_t width = 0;
    uint32_t height = 0;
    int result;

    if (header->lossless) {
        uint8_t hint = 0;
        result =
            parse_vp8l(stream, header->bitstream_size, &width, &height, &hint);

        // a lossless stream declares its own alpha, and a simple file has no
        // VP8X to have declared it already
        if (!seen_canvas) header->has_alpha = hint;
    }
    else {
        result = parse_vp8(stream, header->bitstream_size, &width, &height);
        if (header->alpha_at) header->has_alpha = 1;
    }

    if (result != TINYIMG_OK && !seen_canvas) return result;

    if (!seen_canvas) {
        header->width = width;
        header->height = height;
    }

    if (header->width == 0 || header->height == 0) return TINYIMG_ERR_CORRUPT;
    if (header->frames == 0) header->frames = 1;

    return result;
}

static int webp_probe(const uint8_t* buffer, size_t size, TinyImageInfo* info) {
    WebpHeader header;
    int result = webp_parse(buffer, size, &header);

    // a variant this build cannot decode still has a readable container, which
    // is what a probe is for
    if (result != TINYIMG_OK && result != TINYIMG_ERR_UNSUPPORTED_VARIANT) {
        return result;
    }

    info->width = header.width;
    info->height = header.height;
    info->frames = header.frames;
    info->format = TINYIMG_FORMAT_WEBP;
    info->channels = (uint8_t) (header.has_alpha ? 4 : 3);
    info->bit_depth = 8;
    info->has_alpha = header.has_alpha;
    info->progressive = 0;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region lossless huffman

/**
 * One canonical Huffman code, as counts per length plus symbols in order.
 *
 * The same shape DEFLATE uses, because the two formats agree on the convention
 * whether or not they agree on anything else: codes are assigned canonically
 * and written most significant bit of the code first into a stream read least
 * significant bit first, so the bits arrive reversed and the walk below
 * unreverses them for free.
 */
typedef struct {
    /** How many codes have each length, 1 through 15. */
    uint16_t counts[VP8L_MAX_BITS + 1];
    /** Symbols ordered by code length then by value. */
    const uint16_t* symbols;
    /**
     * @brief The only symbol, when the code is one symbol wide.
     *
     * Such a code reads no bits at all, which is this format's rule and not an
     * optimization: an encoder that finds one green value in a tile writes a
     * tree of one symbol, and a decoder that spent a bit on it would desync.
     */
    int32_t single;
} WebpHuffman;

/**
 * Builds a code from its per symbol lengths.
 *
 * @param code Receives the code.
 * @param lengths One length per symbol, zero where the symbol is unused.
 * @param count Alphabet size.
 * @param symbols Scratch for `count` symbols, which the code borrows.
 * @return int TINYIMG_OK or TINYIMG_ERR_CORRUPT for a tree that is over or
 * under subscribed.
 */
static int huffman_build(
    WebpHuffman* code, const uint8_t* lengths, uint32_t count, uint16_t* symbols
) {
    tiny_memset(code->counts, 0, sizeof(code->counts));

    code->symbols = symbols;
    code->single = -1;

    uint32_t used = 0;
    uint32_t only = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] > VP8L_MAX_BITS) return TINYIMG_ERR_CORRUPT;
        if (lengths[i] == 0) continue;

        code->counts[lengths[i]]++;
        used++;
        only = i;
    }

    if (used == 0) return TINYIMG_ERR_CORRUPT;

    if (used == 1) {
        code->single = (int32_t) only;
        return TINYIMG_OK;
    }

    int32_t left = 1;

    for (uint32_t length = 1; length <= VP8L_MAX_BITS; length++) {
        left <<= 1;
        left -= (int32_t) code->counts[length];

        if (left < 0) return TINYIMG_ERR_CORRUPT;
    }

    // unlike DEFLATE's distance tree, no code here is allowed to be incomplete:
    // every symbol the alphabet holds must be reachable
    if (left != 0) return TINYIMG_ERR_CORRUPT;

    uint16_t offsets[VP8L_MAX_BITS + 2];
    offsets[1] = 0;

    for (uint32_t length = 1; length <= VP8L_MAX_BITS; length++) {
        offsets[length + 1] =
            (uint16_t) (offsets[length] + code->counts[length]);
    }

    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] > 0) symbols[offsets[lengths[i]]++] = (uint16_t) i;
    }

    return TINYIMG_OK;
}

/**
 * Reads one symbol.
 *
 * Peeks the longest code the format allows and then skips only the bits the
 * matched length used, so a symbol costs one peek and one skip rather than one
 * call per bit. Peeking past the end of the buffer is harmless; the reader
 * latches an overrun only when a skip consumes bits that were never there.
 *
 * @param bits The stream.
 * @param code The code to read with.
 * @return int32_t The symbol, or -1 when no code of any length matched.
 */
static int32_t huffman_read(TinyBitReader* bits, const WebpHuffman* code) {
    if (code->single >= 0) return code->single;

    uint32_t look = tiny_bits_peek_lsb(bits, VP8L_MAX_BITS);
    uint32_t value = 0;
    uint32_t first = 0;
    uint32_t index = 0;

    for (uint32_t length = 1; length <= VP8L_MAX_BITS; length++) {
        value |= (look >> (length - 1)) & 1u;

        uint32_t here = code->counts[length];

        if (value - first < here) {
            tiny_bits_skip_lsb(bits, length);
            return code->symbols[index + (value - first)];
        }

        index += here;
        first = (first + here) << 1;
        value <<= 1;
    }

    return -1;
}

/** The order the nineteen code length symbols are written in. */
static const uint8_t length_order[VP8L_CODE_LENGTHS] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/**
 * Reads the code lengths of one alphabet, themselves Huffman coded.
 *
 * Two forms. A simple code names one or two symbols outright, for a tile whose
 * channel barely varies. Otherwise a code over nineteen length symbols is read
 * first and used to read the lengths, with three repeat symbols for runs.
 */
static int read_code_lengths(
    TinyBitReader* bits, uint32_t count, uint8_t* lengths
) {
    tiny_memset(lengths, 0, count);

    if (tiny_bits_lsb(bits, 1)) {
        uint32_t symbols = tiny_bits_lsb(bits, 1) + 1u;
        uint32_t wide = tiny_bits_lsb(bits, 1);

        // the first symbol is one bit when it is 0 or 1 and eight otherwise,
        // which is what makes a two color tile nearly free
        uint32_t first = tiny_bits_lsb(bits, wide ? 8 : 1);

        if (first >= count) return TINYIMG_ERR_CORRUPT;
        lengths[first] = 1;

        if (symbols == 2) {
            uint32_t second = tiny_bits_lsb(bits, 8);

            if (second >= count) return TINYIMG_ERR_CORRUPT;
            lengths[second] = 1;
        }

        return TINYIMG_OK;
    }

    uint8_t code_lengths[VP8L_CODE_LENGTHS];
    tiny_memset(code_lengths, 0, sizeof(code_lengths));

    uint32_t present = tiny_bits_lsb(bits, 4) + 4u;

    for (uint32_t i = 0; i < present; i++) {
        code_lengths[length_order[i]] = (uint8_t) tiny_bits_lsb(bits, 3);
    }

    WebpHuffman tree;
    uint16_t tree_symbols[VP8L_CODE_LENGTHS];

    int result =
        huffman_build(&tree, code_lengths, VP8L_CODE_LENGTHS, tree_symbols);
    if (result != TINYIMG_OK) return result;

    /*
     * An optional count of how many symbols carry a length at all.
     *
     * It is not the same as the alphabet size and it is not redundant: an
     * encoder that used only the first few symbols writes it so the decoder
     * stops rather than reading lengths for the rest, and the field itself is
     * variable width so that saying "the first six" costs about as little as
     * saying nothing.
     */
    uint32_t remaining = count;

    if (tiny_bits_lsb(bits, 1)) {
        uint32_t width = 2u + 2u * tiny_bits_lsb(bits, 3);
        remaining = 2u + tiny_bits_lsb(bits, width);
    }

    // eight until a non-zero length is read, which is the format's own default
    // and matters because the first repeat symbol may come before any of them
    uint32_t previous = 8;
    uint32_t symbol = 0;

    static const uint8_t repeat_extra[3] = {2, 3, 7};
    static const uint8_t repeat_offset[3] = {3, 3, 11};

    while (symbol < count) {
        if (remaining-- == 0) break;

        int32_t length = huffman_read(bits, &tree);
        if (length < 0) return TINYIMG_ERR_CORRUPT;

        if (length < 16) {
            lengths[symbol++] = (uint8_t) length;
            if (length != 0) previous = (uint32_t) length;
            continue;
        }

        uint32_t slot = (uint32_t) length - 16u;
        uint32_t repeat =
            tiny_bits_lsb(bits, repeat_extra[slot]) + repeat_offset[slot];

        if (symbol + repeat > count) return TINYIMG_ERR_CORRUPT;

        // symbol 16 repeats the last length written, 17 and 18 repeat a gap
        uint8_t fill = slot == 0 ? (uint8_t) previous : 0;

        while (repeat-- > 0) {
            lengths[symbol++] = fill;
        }
    }

    if (bits->overrun) return TINYIMG_ERR_CORRUPT;

    return TINYIMG_OK;
}

#pragma endregion

#pragma region lossless transforms

/** Adds two pixels channel by channel, letting each wrap on its own. */
static uint32_t add_pixels(uint32_t a, uint32_t b) {
    uint32_t high = (a & 0xFF00FF00u) + (b & 0xFF00FF00u);
    uint32_t low = (a & 0x00FF00FFu) + (b & 0x00FF00FFu);

    return (high & 0xFF00FF00u) | (low & 0x00FF00FFu);
}

/** Averages two pixels channel by channel, without carrying between them. */
static uint32_t average2(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

static int32_t sub3(int32_t a, int32_t b, int32_t c) {
    int32_t left = b - c;
    int32_t right = a - c;

    if (left < 0) left = -left;
    if (right < 0) right = -right;

    return left - right;
}

/**
 * Picks whichever of the two neighbors the gradient points at.
 *
 * Summed over all four channels rather than decided per channel, so the
 * predictor either takes a whole pixel or takes none of it.
 */
static uint32_t select_pixel(uint32_t a, uint32_t b, uint32_t c) {
    int32_t total =
        sub3((int32_t) (a >> 24), (int32_t) (b >> 24), (int32_t) (c >> 24)) +
        sub3(
            (int32_t) ((a >> 16) & 0xFFu), (int32_t) ((b >> 16) & 0xFFu),
            (int32_t) ((c >> 16) & 0xFFu)
        ) +
        sub3(
            (int32_t) ((a >> 8) & 0xFFu), (int32_t) ((b >> 8) & 0xFFu),
            (int32_t) ((c >> 8) & 0xFFu)
        ) +
        sub3(
            (int32_t) (a & 0xFFu), (int32_t) (b & 0xFFu), (int32_t) (c & 0xFFu)
        );

    return total <= 0 ? a : b;
}

static uint32_t clip_add_full(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t out = 0;

    for (uint32_t shift = 0; shift < 32; shift += 8) {
        int32_t value = (int32_t) ((a >> shift) & 0xFFu) +
                        (int32_t) ((b >> shift) & 0xFFu) -
                        (int32_t) ((c >> shift) & 0xFFu);

        out |= (uint32_t) tiny_clamp_u8(value) << shift;
    }

    return out;
}

static uint32_t clip_add_half(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t mean = average2(a, b);
    uint32_t out = 0;

    for (uint32_t shift = 0; shift < 32; shift += 8) {
        int32_t left = (int32_t) ((mean >> shift) & 0xFFu);
        int32_t right = (int32_t) ((c >> shift) & 0xFFu);

        out |= (uint32_t) tiny_clamp_u8(left + (left - right) / 2) << shift;
    }

    return out;
}

/**
 * Predicts one pixel from its already decoded neighbors.
 *
 * @param mode Which of the fourteen predictors to use.
 * @param left The pixel to the left.
 * @param top The row above, positioned at the same column.
 * @return uint32_t The prediction.
 */
static uint32_t predict(uint32_t mode, uint32_t left, const uint32_t* top) {
    switch (mode) {
        case 0: return 0xFF000000u;
        case 1: return left;
        case 2: return top[0];
        case 3: return top[1];
        case 4: return top[-1];
        case 5: return average2(average2(left, top[1]), top[0]);
        case 6: return average2(left, top[-1]);
        case 7: return average2(left, top[0]);
        case 8: return average2(top[-1], top[0]);
        case 9: return average2(top[0], top[1]);
        case 10:
            return average2(average2(left, top[-1]), average2(top[0], top[1]));
        case 11: return select_pixel(top[0], left, top[-1]);
        case 12: return clip_add_full(left, top[0], top[-1]);
        default: return clip_add_half(left, top[0], top[-1]);
    }
}

static uint32_t subsample_size(uint32_t size, uint32_t bits) {
    return (size + (1u << bits) - 1u) >> bits;
}

/**
 * Undoes the predictor transform in place.
 *
 * The mode comes from the green channel of a block sized image, and the edges
 * are forced: the very first pixel is predicted from opaque black, the rest of
 * the first row from the left, and the first column of every other row from
 * above. Nothing in the block image overrides those, so an encoder cannot
 * choose a predictor that would reach outside the image.
 */
static void inverse_predictor(
    uint32_t* pixels, uint32_t width, uint32_t height, uint32_t bits,
    const uint32_t* modes
) {
    uint32_t blocks = subsample_size(width, bits);

    pixels[0] = add_pixels(pixels[0], 0xFF000000u);

    for (uint32_t x = 1; x < width; x++) {
        pixels[x] = add_pixels(pixels[x], pixels[x - 1]);
    }

    for (uint32_t y = 1; y < height; y++) {
        uint32_t* row = pixels + (size_t) y * width;
        const uint32_t* above = row - width;
        const uint32_t* block = modes + (size_t) (y >> bits) * blocks;

        row[0] = add_pixels(row[0], above[0]);

        for (uint32_t x = 1; x < width; x++) {
            uint32_t mode = (block[x >> bits] >> 8) & 0x0Fu;

            // the top right neighbor of the last column would be off the end
            // of the row above, and the format's answer is that it wraps to the
            // first pixel of the current row, which is what indexing the flat
            // plane already does
            row[x] = add_pixels(row[x], predict(mode, row[x - 1], above + x));
        }
    }
}

static int32_t color_delta(int32_t multiplier, int32_t channel) {
    return (multiplier * channel) >> 5;
}

/** Undoes the cross color transform in place. */
static void inverse_cross_color(
    uint32_t* pixels, uint32_t width, uint32_t height, uint32_t bits,
    const uint32_t* codes
) {
    uint32_t blocks = subsample_size(width, bits);

    for (uint32_t y = 0; y < height; y++) {
        uint32_t* row = pixels + (size_t) y * width;
        const uint32_t* block = codes + (size_t) (y >> bits) * blocks;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t code = block[x >> bits];

            int32_t green_to_red = (int8_t) (code & 0xFFu);
            int32_t green_to_blue = (int8_t) ((code >> 8) & 0xFFu);
            int32_t red_to_blue = (int8_t) ((code >> 16) & 0xFFu);

            uint32_t pixel = row[x];
            int32_t green = (int8_t) ((pixel >> 8) & 0xFFu);

            int32_t red = (int32_t) ((pixel >> 16) & 0xFFu);
            red += color_delta(green_to_red, green);
            red &= 0xFF;

            int32_t blue = (int32_t) (pixel & 0xFFu);
            blue += color_delta(green_to_blue, green);

            // against the reconstructed red, not the residual one, so the two
            // deltas do not have to be undone in the order they were applied
            blue += color_delta(red_to_blue, (int8_t) red);
            blue &= 0xFF;

            row[x] = (pixel & 0xFF00FF00u) | ((uint32_t) red << 16) |
                     (uint32_t) blue;
        }
    }
}

/** Undoes the subtract green transform in place. */
static void inverse_subtract_green(uint32_t* pixels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i];
        uint32_t green = (pixel >> 8) & 0xFFu;
        uint32_t pair = (pixel & 0x00FF00FFu) + ((green << 16) | green);

        pixels[i] = (pixel & 0xFF00FF00u) | (pair & 0x00FF00FFu);
    }
}

/**
 * Expands the color indexing transform, widening the row as it goes.
 *
 * With sixteen colors or fewer several indices share a byte, so the coded row
 * is narrower than the image and this is the one inverse that cannot run in
 * place.
 */
static void inverse_color_index(
    const uint32_t* packed, uint32_t width, uint32_t height, uint32_t bits,
    const uint32_t* palette, uint32_t* out
) {
    uint32_t per_pixel = 8u >> bits;
    uint32_t per_byte = 1u << bits;
    uint32_t mask = (1u << per_pixel) - 1u;
    uint32_t coded = subsample_size(width, bits);

    for (uint32_t y = 0; y < height; y++) {
        const uint32_t* row = packed + (size_t) y * coded;
        uint32_t* dest = out + (size_t) y * width;
        uint32_t bundle = 0;

        for (uint32_t x = 0; x < width; x++) {
            if ((x & (per_byte - 1u)) == 0) {
                bundle = (row[x >> bits] >> 8) & 0xFFu;
            }

            dest[x] = palette[bundle & mask];
            bundle >>= per_pixel;
        }
    }
}

#pragma endregion

#pragma region lossless decode

typedef struct {
    uint8_t type;
    uint8_t bits;
    /** The width the transform's own data describes, before it changed one. */
    uint32_t width;
    uint32_t height;
    uint32_t* data;
} WebpTransform;

typedef struct {
    TinyBitReader bits;

    WebpTransform transforms[4];
    uint32_t count;
    uint32_t seen;

    /** The distance code to pixel offset table, built once per decode. */
    uint8_t planes[VP8L_PLANE_CODES];
} WebpLossless;

/**
 * Builds the table that turns a short distance code into a nearby pixel.
 *
 * The format's own table is 120 entries of a packed row and column offset,
 * ordered so that the codes an encoder reaches for first are the neighbors a
 * copy is most likely to come from. It is not shipped, because the ordering is
 * a rule rather than a list: every offset with a row of 0 to 7 and a column of
 * -7 to 8 that names a pixel already written, sorted by squared distance, then
 * by descending row, then by descending column. That reproduces the
 * specification's table exactly, which `tests/c/codec/webp.c` asserts against
 * the literal 120 bytes.
 */
static void build_planes(uint8_t* planes) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < VP8L_PLANE_CODES; i++) {
        planes[i] = 0;
    }

    for (int32_t y = 0; y <= 7; y++) {
        for (int32_t x = -7; x <= 8; x++) {
            if (y == 0 && x <= 0) continue;

            int32_t key = x * x + y * y;
            uint8_t packed =
                (uint8_t) (((uint32_t) y << 4) | (uint32_t) (8 - x));

            // insertion into an already ordered prefix, which is enough at 120
            // entries and needs no scratch
            uint32_t at = count;

            while (at > 0) {
                int32_t oy = (int32_t) (planes[at - 1] >> 4);
                int32_t ox = 8 - (int32_t) (planes[at - 1] & 0x0Fu);
                int32_t other = ox * ox + oy * oy;

                if (other < key) break;
                if (other == key && (oy > y || (oy == y && ox > x))) break;

                planes[at] = planes[at - 1];
                at--;
            }

            planes[at] = packed;
            count++;
        }
    }
}

/** Turns a distance code into how many pixels back the copy starts. */
static uint32_t plane_distance(
    const uint8_t* planes, uint32_t width, uint32_t code
) {
    if (code > VP8L_PLANE_CODES) return code - VP8L_PLANE_CODES;

    uint8_t packed = planes[code - 1];
    uint32_t y = packed >> 4;
    int32_t x = 8 - (int32_t) (packed & 0x0Fu);

    int64_t distance = (int64_t) y * width + x;

    return distance >= 1 ? (uint32_t) distance : 1u;
}

/**
 * Reads a length or a distance, which share one prefix coding.
 *
 * The first four codes are the values one to four outright; past that each pair
 * of codes doubles the range it covers and carries the offset inside it in
 * extra bits.
 */
static uint32_t prefix_value(TinyBitReader* bits, uint32_t symbol) {
    if (symbol < 4) return symbol + 1u;

    uint32_t extra = (symbol - 2u) >> 1;
    uint32_t offset = (2u + (symbol & 1u)) << extra;

    return offset + tiny_bits_lsb(bits, extra) + 1u;
}

typedef struct {
    /** Five codes per group: green with the lengths and cache, then r/b/a and
     * the distances. */
    WebpHuffman codes[VP8L_CODES_PER_GROUP];
} WebpGroup;

typedef struct {
    WebpGroup* groups;
    uint32_t count;
    /** The meta index per block, or NULL when the whole image is one group. */
    const uint32_t* image;
    uint32_t image_width;
    uint32_t bits;
    uint32_t cache_bits;
} WebpEntropy;

static int decode_image_stream(
    WebpLossless* state, uint32_t width, uint32_t height, int level0,
    uint32_t** out, uint32_t* out_width
);

/**
 * Reads the Huffman codes for every group the image uses.
 *
 * A meta Huffman image splits the picture into blocks and gives each its own
 * five codes, which is what lets one file carry a photograph and a run of flat
 * color without either paying for the other's statistics.
 */
static int read_entropy(
    WebpLossless* state, uint32_t width, uint32_t height, int level0,
    WebpEntropy* entropy
) {
    tiny_memset(entropy, 0, sizeof(*entropy));

    uint32_t groups = 1;

    // the color cache comes first, before the meta Huffman flag, and getting
    // that order wrong reads the cache's width as a group count
    if (tiny_bits_lsb(&state->bits, 1)) {
        entropy->cache_bits = tiny_bits_lsb(&state->bits, 4);

        if (entropy->cache_bits < 1 ||
            entropy->cache_bits > VP8L_MAX_CACHE_BITS) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    if (level0 && tiny_bits_lsb(&state->bits, 1)) {
        uint32_t bits = tiny_bits_lsb(&state->bits, 3) + 2u;
        uint32_t across = subsample_size(width, bits);
        uint32_t down = subsample_size(height, bits);

        uint32_t* image = 0;
        uint32_t coded = 0;
        int result =
            decode_image_stream(state, across, down, 0, &image, &coded);

        if (result != TINYIMG_OK) return result;

        // the group index is the red and green channels together, so a file may
        // name up to 65536 of them
        for (size_t i = 0; i < (size_t) across * down; i++) {
            uint32_t group = (image[i] >> 8) & 0xFFFFu;

            image[i] = group;
            if (group + 1u > groups) groups = group + 1u;
        }

        entropy->image = image;
        entropy->image_width = across;
        entropy->bits = bits;
    }

    uint32_t cache = entropy->cache_bits ? 1u << entropy->cache_bits : 0u;

    uint32_t sizes[VP8L_CODES_PER_GROUP] = {
        VP8L_LITERALS + VP8L_LENGTHS + cache, VP8L_LITERALS, VP8L_LITERALS,
        VP8L_LITERALS, VP8L_DISTANCES
    };

    uint32_t total = 0;
    for (uint32_t i = 0; i < VP8L_CODES_PER_GROUP; i++) {
        total += sizes[i];
    }

    entropy->groups = tiny_arena_alloc((size_t) groups * sizeof(WebpGroup), 0);
    uint16_t* symbols =
        tiny_arena_alloc((size_t) groups * total * sizeof(uint16_t), 0);
    uint8_t* lengths = tiny_arena_alloc(sizes[0], 0);

    if (!entropy->groups || !symbols || !lengths) return TINYIMG_ERR_MEMORY;

    entropy->count = groups;

    for (uint32_t g = 0; g < groups; g++) {
        for (uint32_t c = 0; c < VP8L_CODES_PER_GROUP; c++) {
            int result = read_code_lengths(&state->bits, sizes[c], lengths);
            if (result != TINYIMG_OK) return result;

            result = huffman_build(
                &entropy->groups[g].codes[c], lengths, sizes[c], symbols
            );
            if (result != TINYIMG_OK) return result;

            symbols += sizes[c];
        }
    }

    return TINYIMG_OK;
}

static const WebpGroup* group_at(
    const WebpEntropy* entropy, uint32_t x, uint32_t y
) {
    if (!entropy->image) return &entropy->groups[0];

    uint32_t index = entropy->image
                         [(size_t) (y >> entropy->bits) * entropy->image_width +
                          (x >> entropy->bits)];

    if (index >= entropy->count) index = 0;

    return &entropy->groups[index];
}

/**
 * Decodes the pixels of one image stream.
 *
 * Three kinds of symbol share the green alphabet, which is what makes the
 * format's compression work at all: a literal, a copy of a run already written,
 * or a reference into a small cache of recent colors.
 */
static int decode_pixels(
    WebpLossless* state, const WebpEntropy* entropy, uint32_t* pixels,
    uint32_t width, uint32_t height
) {
    size_t count = (size_t) width * height;
    uint32_t* cache = 0;
    uint32_t shift = 0;

    if (entropy->cache_bits) {
        cache = tiny_arena_alloc(
            ((size_t) 1u << entropy->cache_bits) * sizeof(uint32_t), 0
        );
        if (!cache) return TINYIMG_ERR_MEMORY;

        tiny_memset(cache, 0, ((size_t) 1u << entropy->cache_bits) * 4u);
        shift = 32u - entropy->cache_bits;
    }

    uint32_t cache_limit =
        VP8L_LITERALS + VP8L_LENGTHS +
        (entropy->cache_bits ? 1u << entropy->cache_bits : 0u);

    size_t at = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    const WebpGroup* group = group_at(entropy, 0, 0);

    while (at < count) {
        if (entropy->image && (x & ((1u << entropy->bits) - 1u)) == 0) {
            group = group_at(entropy, x, y);
        }

        int32_t code = huffman_read(&state->bits, &group->codes[0]);
        if (code < 0 || state->bits.overrun) return TINYIMG_ERR_CORRUPT;

        if ((uint32_t) code < VP8L_LITERALS) {
            int32_t red = huffman_read(&state->bits, &group->codes[1]);
            int32_t blue = huffman_read(&state->bits, &group->codes[2]);
            int32_t alpha = huffman_read(&state->bits, &group->codes[3]);

            if (red < 0 || blue < 0 || alpha < 0) return TINYIMG_ERR_CORRUPT;

            pixels[at] = ((uint32_t) alpha << 24) | ((uint32_t) red << 16) |
                         ((uint32_t) code << 8) | (uint32_t) blue;
        }
        else if ((uint32_t) code < VP8L_LITERALS + VP8L_LENGTHS) {
            uint32_t length =
                prefix_value(&state->bits, (uint32_t) code - VP8L_LITERALS);

            int32_t symbol = huffman_read(&state->bits, &group->codes[4]);
            if (symbol < 0) return TINYIMG_ERR_CORRUPT;

            uint32_t distance = plane_distance(
                state->planes, width,
                prefix_value(&state->bits, (uint32_t) symbol)
            );

            if (distance > at || length > count - at) {
                return TINYIMG_ERR_CORRUPT;
            }

            // forward, one pixel at a time, because a distance shorter than the
            // run is legal and means the copy reads what it has just written
            for (uint32_t i = 0; i < length; i++) {
                pixels[at + i] = pixels[at + i - distance];

                if (cache) {
                    cache[(pixels[at + i] * 0x1E35A7BDu) >> shift] =
                        pixels[at + i];
                }
            }

            at += length;
            x += length;

            while (x >= width) {
                x -= width;
                y++;
            }

            if (at < count && entropy->image) {
                group = group_at(entropy, x, y);
            }

            continue;
        }
        else if ((uint32_t) code < cache_limit) {
            uint32_t key = (uint32_t) code - (VP8L_LITERALS + VP8L_LENGTHS);
            pixels[at] = cache[key];
        }
        else {
            return TINYIMG_ERR_CORRUPT;
        }

        if (cache) {
            cache[(pixels[at] * 0x1E35A7BDu) >> shift] = pixels[at];
        }

        at++;

        if (++x >= width) {
            x = 0;
            y++;
        }
    }

    return state->bits.overrun ? TINYIMG_ERR_CORRUPT : TINYIMG_OK;
}

/**
 * Reads one transform's header and its own image.
 *
 * A transform is recorded with the width it saw, not the width it leaves
 * behind: color indexing narrows the coded rows, and every transform read
 * after it describes the narrower ones, so the inverses undo in reverse and
 * each finds the width it expects.
 */
static int read_transform(
    WebpLossless* state, uint32_t* width, uint32_t height
) {
    uint32_t type = tiny_bits_lsb(&state->bits, 2);

    if (state->seen & (1u << type)) return TINYIMG_ERR_CORRUPT;
    state->seen |= 1u << type;

    WebpTransform* transform = &state->transforms[state->count++];

    transform->type = (uint8_t) type;
    transform->bits = 0;
    transform->width = *width;
    transform->height = height;
    transform->data = 0;

    switch (type) {
        case VP8L_PREDICTOR:
        case VP8L_CROSS_COLOR: {
            transform->bits = (uint8_t) (tiny_bits_lsb(&state->bits, 3) + 2u);

            uint32_t coded = 0;
            return decode_image_stream(
                state, subsample_size(*width, transform->bits),
                subsample_size(height, transform->bits), 0, &transform->data,
                &coded
            );
        }

        case VP8L_SUBTRACT_GREEN: return TINYIMG_OK;

        default: {
            uint32_t colors = tiny_bits_lsb(&state->bits, 8) + 1u;

            // fewer colors pack more indices into a byte, and the coded rows
            // narrow by the same factor
            transform->bits = (uint8_t) (colors > 16  ? 0
                                         : colors > 4 ? 1
                                         : colors > 2 ? 2
                                                      : 3);

            *width = subsample_size(*width, transform->bits);

            uint32_t* palette = 0;
            uint32_t coded = 0;
            int result =
                decode_image_stream(state, colors, 1, 0, &palette, &coded);
            if (result != TINYIMG_OK) return result;

            /*
             * The palette is delta coded byte by byte and then padded out to
             * however many entries an index can name, so a file whose indices
             * outrun its palette reads black rather than reading off the end.
             */
            uint32_t entries = 1u << (8u >> transform->bits);
            transform->data =
                tiny_arena_alloc((size_t) entries * sizeof(uint32_t), 0);

            if (!transform->data) return TINYIMG_ERR_MEMORY;

            uint8_t* bytes = (uint8_t*) transform->data;
            const uint8_t* source = (const uint8_t*) palette;

            transform->data[0] = palette[0];

            for (size_t i = 4; i < (size_t) colors * 4; i++) {
                bytes[i] = (uint8_t) (source[i] + bytes[i - 4]);
            }

            for (size_t i = (size_t) colors * 4; i < (size_t) entries * 4;
                 i++) {
                bytes[i] = 0;
            }

            return TINYIMG_OK;
        }
    }
}

/**
 * Decodes one image stream, which is the format's single recursive unit.
 *
 * The picture itself, a predictor's block modes, a cross color transform's
 * multipliers, a palette and a meta Huffman index image are all the same thing
 * read at different sizes. Only the outermost one carries transforms, so the
 * recursion is two deep at most.
 */
static int decode_image_stream(
    WebpLossless* state, uint32_t width, uint32_t height, int level0,
    uint32_t** out, uint32_t* out_width
) {
    uint32_t coded = width;

    if (level0) {
        while (tiny_bits_lsb(&state->bits, 1)) {
            if (state->count >= 4) return TINYIMG_ERR_CORRUPT;

            int result = read_transform(state, &coded, height);
            if (result != TINYIMG_OK) return result;
        }
    }

    WebpEntropy entropy;
    int result = read_entropy(state, coded, height, level0, &entropy);
    if (result != TINYIMG_OK) return result;

    uint32_t* pixels =
        tiny_arena_alloc((size_t) coded * height * sizeof(uint32_t), 0);
    if (!pixels) return TINYIMG_ERR_MEMORY;

    result = decode_pixels(state, &entropy, pixels, coded, height);
    if (result != TINYIMG_OK) return result;

    *out = pixels;
    *out_width = coded;

    return TINYIMG_OK;
}

/**
 * Undoes every transform, in the reverse of the order they were read.
 *
 * @param state The decoder, holding the transforms.
 * @param pixels The coded plane, which may be narrower than the image.
 * @param width Receives the width after each widening transform.
 * @param height Image height.
 * @return uint32_t* The plane, which color indexing may have replaced.
 */
static uint32_t* apply_transforms(
    const WebpLossless* state, uint32_t* pixels, uint32_t* width,
    uint32_t height
) {
    for (uint32_t i = state->count; i-- > 0;) {
        const WebpTransform* transform = &state->transforms[i];

        switch (transform->type) {
            case VP8L_PREDICTOR:
                inverse_predictor(
                    pixels, *width, height, transform->bits, transform->data
                );
                break;

            case VP8L_CROSS_COLOR:
                inverse_cross_color(
                    pixels, *width, height, transform->bits, transform->data
                );
                break;

            case VP8L_SUBTRACT_GREEN:
                inverse_subtract_green(pixels, (size_t) *width * height);
                break;

            default: {
                uint32_t* wide = tiny_arena_alloc(
                    (size_t) transform->width * height * sizeof(uint32_t), 0
                );
                if (!wide) return 0;

                inverse_color_index(
                    pixels, transform->width, height, transform->bits,
                    transform->data, wide
                );

                *width = transform->width;
                pixels = wide;
                break;
            }
        }
    }

    return pixels;
}

/**
 * Decodes a lossless stream into a plane of packed pixels.
 *
 * @param data The stream, starting at the five byte header unless `raw` is set.
 * @param size Number of bytes.
 * @param raw Non-zero for an alpha stream, which has no header of its own
 * because the frame it belongs to already gave the dimensions.
 * @param width Image width, which `raw` requires and otherwise receives.
 * @param height Image height, likewise.
 * @param out Receives the plane, allocated from the arena.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int decode_lossless(
    const uint8_t* data, size_t size, int raw, uint32_t* width,
    uint32_t* height, uint32_t** out
) {
    WebpLossless state;

    tiny_memset(&state, 0, sizeof(state));
    build_planes(state.planes);

    if (!raw) {
        uint8_t hint = 0;
        int result = parse_vp8l(data, size, width, height, &hint);
        if (result != TINYIMG_OK) return result;

        tiny_bits_init(&state.bits, data, size);

        // the header is five bytes of the same least significant bit first
        // stream the pixels are in, so it is skipped through the bit reader
        tiny_bits_skip_lsb(&state.bits, 40);
    }
    else {
        tiny_bits_init(&state.bits, data, size);
    }

    if (*width == 0 || *height == 0) return TINYIMG_ERR_CORRUPT;

    uint32_t coded = 0;
    uint32_t* pixels = 0;

    int result =
        decode_image_stream(&state, *width, *height, 1, &pixels, &coded);
    if (result != TINYIMG_OK) return result;

    pixels = apply_transforms(&state, pixels, &coded, *height);
    if (!pixels) return TINYIMG_ERR_MEMORY;

    if (coded != *width) return TINYIMG_ERR_CORRUPT;

    *out = pixels;
    return TINYIMG_OK;
}

#pragma endregion

#pragma region alpha

static int32_t gradient(int32_t left, int32_t top, int32_t corner) {
    int32_t value = left + top - corner;

    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

/**
 * Undoes one of the three filters an alpha plane may have been written with.
 *
 * The first row has nothing above it, so every filter falls back to the
 * horizontal one there, and the first pixel of the image predicts from zero.
 */
static void unfilter_alpha(
    uint8_t* plane, uint32_t width, uint32_t height, uint32_t method
) {
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = plane + (size_t) y * width;
        const uint8_t* above = y > 0 ? row - width : 0;

        if (method == 1 || !above) {
            uint32_t left = above ? above[0] : 0u;

            for (uint32_t x = 0; x < width; x++) {
                row[x] = (uint8_t) (row[x] + left);
                left = row[x];
            }
            continue;
        }

        if (method == 2) {
            for (uint32_t x = 0; x < width; x++) {
                row[x] = (uint8_t) (row[x] + above[x]);
            }
            continue;
        }

        int32_t left = above[0];
        int32_t corner = left;

        for (uint32_t x = 0; x < width; x++) {
            int32_t top = above[x];

            left = (uint8_t) (row[x] + gradient(left, top, corner));
            corner = top;
            row[x] = (uint8_t) left;
        }
    }
}

/**
 * Reads an ALPH chunk into an alpha plane.
 *
 * Either the bytes are stored outright, or they are the green channel of a
 * lossless stream with no header of its own. The preprocessing field says
 * whether the encoder reduced the plane's levels before writing it, which is a
 * choice the decoder cannot and need not undo.
 */
static int decode_alpha(
    const uint8_t* data, size_t size, uint32_t width, uint32_t height,
    uint8_t* plane
) {
    if (size < 1) return TINYIMG_ERR_CORRUPT;

    uint32_t header = data[0];
    uint32_t method = header & 3u;
    uint32_t filter = (header >> 2) & 3u;

    size_t count = (size_t) width * height;

    if (method == 0) {
        if (size - 1 < count) return TINYIMG_ERR_CORRUPT;

        tiny_memcpy(plane, data + 1, count);
    }
    else if (method == 1) {
        uint32_t coded_width = width;
        uint32_t coded_height = height;
        uint32_t* pixels = 0;

        int result = decode_lossless(
            data + 1, size - 1, 1, &coded_width, &coded_height, &pixels
        );
        if (result != TINYIMG_OK) return result;

        for (size_t i = 0; i < count; i++) {
            plane[i] = (uint8_t) ((pixels[i] >> 8) & 0xFFu);
        }
    }
    else {
        return TINYIMG_ERR_UNSUPPORTED_VARIANT;
    }

    if (filter > 0) unfilter_alpha(plane, width, height, filter);

    return TINYIMG_OK;
}

#pragma endregion

#pragma region lossy bitstream

/**
 * The arithmetic decoder every lossy field is read through.
 *
 * Nothing in a lossy frame is stored as plain bits. Each value is a sequence of
 * binary decisions, each carrying its own probability, and the decoder tracks a
 * range and a value inside it rather than a bit position. That is why a lossy
 * frame cannot be read out of order or resumed in the middle.
 */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    /** Sixteen live bits: the top eight are compared, the rest look ahead. */
    uint32_t value;
    uint32_t range;
    /** Bits shifted out of the value since the last whole byte arrived. */
    uint32_t count;
    /** How many bytes past the end of the partition have been asked for. */
    uint32_t phantom;
    /** Set once more of those were asked for than the lookahead explains. */
    int eof;
} Vp8Bool;

/**
 * The lookahead is why this is not simply an end of buffer test.
 *
 * The decoder keeps sixteen live bits, eight of them ahead of the decision
 * being made, and may also straddle a partial byte, so a partition that ends
 * exactly still asks for three bytes it does not have and is not truncated.
 * Measured on the reference lossy fixture, whose first partition over-reads by
 * exactly three; the allowance is four so the check still catches a genuine
 * truncation, which is short by however many bytes were cut rather than by one.
 */
static uint8_t bool_byte(Vp8Bool* bits) {
    if (bits->pos >= bits->size) {
        if (++bits->phantom > 4) bits->eof = 1;
        return 0;
    }

    return bits->data[bits->pos++];
}

static void bool_init(Vp8Bool* bits, const uint8_t* data, size_t size) {
    bits->data = data;
    bits->size = size;
    bits->pos = 0;
    bits->range = 255;
    bits->count = 0;
    bits->phantom = 0;
    bits->eof = 0;
    bits->value = 0;

    for (uint32_t i = 0; i < 2; i++) {
        bits->value = (bits->value << 8) | bool_byte(bits);
    }
}

static int bool_get(Vp8Bool* bits, uint32_t probability) {
    uint32_t split = 1u + (((bits->range - 1u) * probability) >> 8);
    uint32_t scaled = split << 8;
    int bit;

    if (bits->value >= scaled) {
        bits->range -= split;
        bits->value -= scaled;
        bit = 1;
    }
    else {
        bits->range = split;
        bit = 0;
    }

    while (bits->range < 128u) {
        bits->value <<= 1;
        bits->range <<= 1;

        if (++bits->count == 8) {
            bits->count = 0;
            bits->value |= bool_byte(bits);
        }
    }

    return bit;
}

/** Reads a plain value, which is a run of decisions at even odds. */
static uint32_t bool_uint(Vp8Bool* bits, uint32_t count) {
    uint32_t value = 0;

    while (count-- > 0) {
        value = (value << 1) | (uint32_t) bool_get(bits, 128);
    }

    return value;
}

static int32_t bool_int(Vp8Bool* bits, uint32_t count) {
    int32_t value = (int32_t) bool_uint(bits, count);

    return bool_get(bits, 128) ? -value : value;
}

/** Reads a flag, then a signed value only if the flag was set. */
static int32_t bool_maybe_int(Vp8Bool* bits, uint32_t count) {
    return bool_get(bits, 128) ? bool_int(bits, count) : 0;
}

/**
 * Walks a coding tree.
 *
 * A node holds the indices of its two children, negated where a child is a
 * leaf, so one array describes both the shape and the symbols, and the
 * probability of a node is found at half its index.
 */
static int32_t bool_tree(
    Vp8Bool* bits, const int8_t* tree, const uint8_t* probabilities
) {
    int32_t at = 0;

    while ((at = tree[at + bool_get(bits, probabilities[at >> 1])]) > 0) {
    }

    return -at;
}

#pragma endregion

#pragma region lossy header

#define VP8_MAX_PARTITIONS 8u
#define VP8_SEGMENTS 4u

/** DC, TM, V and H, in the order the format numbers them. */
#define VP8_DC_PRED 0
#define VP8_TM_PRED 1
#define VP8_V_PRED 2
#define VP8_H_PRED 3

/** The three DC variants for a block missing neighbors. */
#define VP8_DC_NO_TOP 4
#define VP8_DC_NO_LEFT 5
#define VP8_DC_NO_TOP_LEFT 6

/** Which of the three dequantization tables a block belongs to. */
#define VP8_BLOCK_Y1 0
#define VP8_BLOCK_UV 1
#define VP8_BLOCK_Y2 2

/** Stride of the working area one macroblock is predicted in. */
#define VP8_BPS 32

/**
 * The subblock mode tree, with the specification's own numbering.
 *
 * Which numbering matters, and the two in circulation disagree: libwebp orders
 * the diagonals right-down, vertical-right, left-down, and the specification
 * orders them left-down, right-down, vertical-right. The probability table is
 * indexed by whichever the leaves use, so the tree, the table and the
 * predictors have to agree. All three here follow the specification, because
 * that is where the table was read from.
 */
static const int8_t vp8_bmode_tree[18] = {-0, 2,  -1, 4,  -2, 6,  8,  12, -3,
                                          10, -5, -6, -4, 14, -7, 16, -8, -9};

static const int8_t vp8_segment_tree[6] = {2, 4, -0, -1, -2, -3};

/** Where each of the sixteen luma subblocks sits inside the working area. */
static const uint16_t vp8_scan[16] = {
    0 + 0 * VP8_BPS,  4 + 0 * VP8_BPS,  8 + 0 * VP8_BPS,  12 + 0 * VP8_BPS,
    0 + 4 * VP8_BPS,  4 + 4 * VP8_BPS,  8 + 4 * VP8_BPS,  12 + 4 * VP8_BPS,
    0 + 8 * VP8_BPS,  4 + 8 * VP8_BPS,  8 + 8 * VP8_BPS,  12 + 8 * VP8_BPS,
    0 + 12 * VP8_BPS, 4 + 12 * VP8_BPS, 8 + 12 * VP8_BPS, 12 + 12 * VP8_BPS
};

typedef struct {
    Vp8Bool part0;
    Vp8Bool tokens[VP8_MAX_PARTITIONS];
    uint32_t partitions;

    uint32_t width;
    uint32_t height;
    uint32_t mb_w;
    uint32_t mb_h;

    int segments_on;
    int update_map;
    int absolute;
    int32_t segment_quant[VP8_SEGMENTS];
    int32_t segment_filter[VP8_SEGMENTS];
    uint8_t segment_probs[3];

    int simple_filter;
    int32_t filter_level;
    uint32_t sharpness;
    int delta_on;
    int32_t ref_delta[4];
    int32_t mode_delta[4];

    /** Per segment, per block type, the DC and AC multipliers. */
    uint16_t quant[VP8_SEGMENTS][3][2];

    uint8_t coeff_probs[4][8][3][11];
    int skip_on;
    uint8_t skip_prob;

    uint8_t* y;
    uint8_t* u;
    uint8_t* v;
    size_t y_stride;
    size_t uv_stride;

    /** Nine non-zero flags per macroblock: four luma columns, two each of
     * chroma, one for the second order block. */
    uint8_t* above_nz;
    uint8_t left_nz[9];

    /** The subblock modes of the row above, four per macroblock. */
    uint8_t* above_modes;
    uint8_t left_modes[4];

    /** Filter level and inner edge flag per macroblock, for the second pass. */
    uint8_t* levels;
    uint8_t* inner;

    /** The working area one macroblock is predicted and reconstructed in. */
    uint8_t work_y[VP8_BPS * 17];
    uint8_t work_u[VP8_BPS * 9];
    uint8_t work_v[VP8_BPS * 9];

    /** Twenty five blocks of sixteen coefficients, the last one second order.
     */
    int16_t coeffs[25 * 16];
    uint32_t nonzero;
} Vp8Decoder;

static int32_t clamp_q(int32_t q) {
    return q < 0 ? 0 : (q > 127 ? 127 : q);
}

/**
 * Builds the six multipliers each segment dequantizes with.
 *
 * The frame carries one index and five deltas, and the two second order
 * multipliers are then scaled: the DC doubled and the AC taken up by 155/100
 * with a floor, which are the format's own constants and follow from nothing.
 */
static void setup_quant(Vp8Decoder* vp8, int32_t base, const int32_t* deltas) {
    for (uint32_t i = 0; i < VP8_SEGMENTS; i++) {
        int32_t q = base;

        if (vp8->segments_on) {
            q = vp8->absolute ? vp8->segment_quant[i]
                              : base + vp8->segment_quant[i];
        }

        vp8->quant[i][VP8_BLOCK_Y1][0] = vp8_dc_quant[clamp_q(q + deltas[0])];
        vp8->quant[i][VP8_BLOCK_Y1][1] = vp8_ac_quant[clamp_q(q)];

        vp8->quant[i][VP8_BLOCK_UV][0] = vp8_dc_quant[clamp_q(q + deltas[3])];
        vp8->quant[i][VP8_BLOCK_UV][1] = vp8_ac_quant[clamp_q(q + deltas[4])];

        // the chroma DC multiplier is capped where the table reaches 132, which
        // is the one clamp that is not a table lookup
        if (vp8->quant[i][VP8_BLOCK_UV][0] > 132) {
            vp8->quant[i][VP8_BLOCK_UV][0] = 132;
        }

        vp8->quant[i][VP8_BLOCK_Y2][0] =
            (uint16_t) (vp8_dc_quant[clamp_q(q + deltas[1])] * 2);

        uint32_t y2_ac = (uint32_t) vp8_ac_quant[clamp_q(q + deltas[2])];
        y2_ac = y2_ac * 155u / 100u;

        vp8->quant[i][VP8_BLOCK_Y2][1] = (uint16_t) (y2_ac < 8 ? 8 : y2_ac);
    }
}

/**
 * Reads the first partition, which is everything but the coefficients.
 *
 * The order is fixed and not obvious: segmentation, then the loop filter, then
 * the coefficient partition count, then the quantizer, then one entropy flag,
 * and only then the coefficient probability updates. Reading the quantizer
 * before the partition count desynchronizes everything after it.
 */
static int parse_header(
    Vp8Decoder* vp8, const uint8_t* data, size_t size, size_t part0
) {
    bool_init(&vp8->part0, data, part0);

    Vp8Bool* bits = &vp8->part0;

    // color space and clamping type, both of which have one defined value
    if (bool_uint(bits, 2) != 0) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    vp8->segments_on = bool_get(bits, 128);

    if (vp8->segments_on) {
        vp8->update_map = bool_get(bits, 128);

        int update_data = bool_get(bits, 128);

        if (update_data) {
            // one means the values replace the frame's index, zero means they
            // are added to it. The prose in the specification has this the
            // wrong way round; its own reference decoder does not
            vp8->absolute = bool_get(bits, 128);

            for (uint32_t i = 0; i < VP8_SEGMENTS; i++) {
                vp8->segment_quant[i] = bool_maybe_int(bits, 7);
            }
            for (uint32_t i = 0; i < VP8_SEGMENTS; i++) {
                vp8->segment_filter[i] = bool_maybe_int(bits, 6);
            }
        }

        if (vp8->update_map) {
            for (uint32_t i = 0; i < 3; i++) {
                vp8->segment_probs[i] =
                    bool_get(bits, 128) ? (uint8_t) bool_uint(bits, 8) : 255u;
            }
        }
    }

    vp8->simple_filter = bool_get(bits, 128);
    vp8->filter_level = (int32_t) bool_uint(bits, 6);
    vp8->sharpness = bool_uint(bits, 3);
    vp8->delta_on = bool_get(bits, 128);

    if (vp8->delta_on && bool_get(bits, 128)) {
        for (uint32_t i = 0; i < 4; i++) {
            vp8->ref_delta[i] = bool_maybe_int(bits, 6);
        }
        for (uint32_t i = 0; i < 4; i++) {
            vp8->mode_delta[i] = bool_maybe_int(bits, 6);
        }
    }

    vp8->partitions = 1u << bool_uint(bits, 2);

    const uint8_t* rest = data + part0;
    size_t left = size - part0;
    size_t table = 3u * (vp8->partitions - 1u);

    if (left < table) return TINYIMG_ERR_CORRUPT;

    const uint8_t* payload = rest + table;
    size_t remaining = left - table;

    for (uint32_t i = 0; i < vp8->partitions; i++) {
        size_t length = remaining;

        if (i + 1 < vp8->partitions) {
            length = read_le24(rest + (size_t) i * 3);
            if (length > remaining) return TINYIMG_ERR_CORRUPT;
        }

        bool_init(&vp8->tokens[i], payload, length);

        payload += length;
        remaining -= length;
    }

    int32_t base = (int32_t) bool_uint(bits, 7);
    int32_t deltas[5];

    for (uint32_t i = 0; i < 5; i++) {
        deltas[i] = bool_maybe_int(bits, 4);
    }

    setup_quant(vp8, base, deltas);

    // a keyframe resets the probabilities, so whether they are kept for the
    // next frame is read and discarded: there is no next frame here
    (void) bool_get(bits, 128);

    tiny_memcpy(vp8->coeff_probs, vp8_coeff_probs, sizeof(vp8->coeff_probs));

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            for (uint32_t k = 0; k < 3; k++) {
                for (uint32_t t = 0; t < 11; t++) {
                    if (bool_get(bits, vp8_coeff_update[i][j][k][t])) {
                        vp8->coeff_probs[i][j][k][t] =
                            (uint8_t) bool_uint(bits, 8);
                    }
                }
            }
        }
    }

    vp8->skip_on = bool_get(bits, 128);
    if (vp8->skip_on) vp8->skip_prob = (uint8_t) bool_uint(bits, 8);

    return vp8->part0.eof ? TINYIMG_ERR_CORRUPT : TINYIMG_OK;
}

#pragma endregion

#pragma region lossy coefficients

/** Which context slot a block's left neighbor flag lives in. */
static uint32_t left_slot(uint32_t block) {
    if (block < 16) return block >> 2;
    if (block < 20) return 4u + ((block - 16u) >> 1);
    if (block < 24) return 6u + ((block - 20u) >> 1);
    return 8;
}

static uint32_t above_slot(uint32_t block) {
    if (block < 16) return block & 3u;
    if (block < 20) return 4u + ((block - 16u) & 1u);
    if (block < 24) return 6u + ((block - 20u) & 1u);
    return 8;
}

static const uint8_t vp8_cat_probs[4][12] = {
    {173, 148, 140, 0},
    {176, 155, 140, 135, 0},
    {180, 157, 141, 134, 130, 0},
    {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0}
};

/**
 * Reads a coefficient past four, whose value is a range plus extra bits.
 *
 * The six categories double in width as they go, and every extra bit carries
 * its own probability, which is what makes a large coefficient expensive
 * without making it impossible.
 */
static int32_t large_value(Vp8Bool* bits, const uint8_t* probabilities) {
    if (!bool_get(bits, probabilities[3])) {
        if (!bool_get(bits, probabilities[4])) return 2;

        return 3 + bool_get(bits, probabilities[5]);
    }

    if (!bool_get(bits, probabilities[6])) {
        if (!bool_get(bits, probabilities[7])) {
            return 5 + bool_get(bits, 159);
        }

        int32_t value = 7 + 2 * bool_get(bits, 165);
        return value + bool_get(bits, 145);
    }

    uint32_t high = (uint32_t) bool_get(bits, probabilities[8]);
    uint32_t low = (uint32_t) bool_get(bits, probabilities[9 + high]);
    uint32_t category = 2u * high + low;

    int32_t value = 0;

    for (const uint8_t* probability = vp8_cat_probs[category]; *probability;
         probability++) {
        value += value + bool_get(bits, *probability);
    }

    return value + 3 + (int32_t) (8u << category);
}

/**
 * Reads one block's coefficients, dequantizing as it goes.
 *
 * @param bits The coefficient partition.
 * @param probabilities The eight bands of this block type.
 * @param context Zero, one or two, from the neighbors' non-zero flags.
 * @param quant The DC and AC multipliers.
 * @param first Where to start, which is one for a luma block whose DC term
 * came from the second order block.
 * @param out Sixteen coefficients in raster order.
 * @return uint32_t One past the last position written, so `first` means the
 * block was empty.
 */
static uint32_t read_coefficients(
    Vp8Bool* bits, const uint8_t probabilities[8][3][11], uint32_t context,
    const uint16_t* quant, uint32_t first, int16_t* out
) {
    uint32_t at = first;
    const uint8_t* probability = probabilities[vp8_bands[at]][context];

    while (at < 16) {
        if (!bool_get(bits, probability[0])) return at;

        // a run of zeros cannot end the block, so the end of block decision is
        // not read again until a non-zero coefficient has been
        while (!bool_get(bits, probability[1])) {
            if (++at == 16) return 16;
            probability = probabilities[vp8_bands[at]][0];
        }

        int32_t value;
        uint32_t next;

        if (!bool_get(bits, probability[2])) {
            value = 1;
            next = 1;
        }
        else {
            value = large_value(bits, probability);
            next = 2;
        }

        if (bool_get(bits, 128)) value = -value;

        out[vp8_zigzag[at]] = (int16_t) (value * quant[at > 0 ? 1 : 0]);

        if (++at == 16) return 16;

        probability = probabilities[vp8_bands[at]][next];
    }

    return 16;
}

/**
 * Reads every block of one macroblock.
 *
 * @return int Non-zero when the macroblock turned out to hold nothing, which
 * decides whether its interior edges are filtered.
 */
static int read_residuals(
    Vp8Decoder* vp8, Vp8Bool* bits, uint32_t mb_x, uint32_t segment, int is_i4x4
) {
    uint8_t* above = vp8->above_nz + (size_t) mb_x * 9u;
    uint8_t* left = vp8->left_nz;

    tiny_memset(vp8->coeffs, 0, sizeof(vp8->coeffs));

    uint32_t nonzero = 0;
    int empty = 1;

    if (!is_i4x4) {
        uint32_t context = (uint32_t) left[8] + above[8];
        uint32_t end = read_coefficients(
            bits, vp8->coeff_probs[1], context,
            vp8->quant[segment][VP8_BLOCK_Y2], 0, vp8->coeffs + 24 * 16
        );

        left[8] = above[8] = (uint8_t) (end != 0);
        if (end != 0) empty = 0;
    }

    // a luma block whose DC term came from the second order block starts at one
    // and reads a different set of probabilities for doing so
    uint32_t type = is_i4x4 ? 3u : 0u;
    uint32_t first = is_i4x4 ? 0u : 1u;

    for (uint32_t block = 0; block < 16; block++) {
        uint32_t l = left_slot(block);
        uint32_t a = above_slot(block);
        uint32_t context = (uint32_t) left[l] + above[a];

        uint32_t end = read_coefficients(
            bits, vp8->coeff_probs[type], context,
            vp8->quant[segment][VP8_BLOCK_Y1], first,
            vp8->coeffs + (size_t) block * 16
        );

        left[l] = above[a] = (uint8_t) (end != first);
        if (end != first) empty = 0;

        // more than one coefficient means the full inverse transform is needed
        // rather than the shortcut that spreads a lone DC term
        if (end > 1) nonzero |= 1u << block;
    }

    for (uint32_t block = 16; block < 24; block++) {
        uint32_t l = left_slot(block);
        uint32_t a = above_slot(block);
        uint32_t context = (uint32_t) left[l] + above[a];

        uint32_t end = read_coefficients(
            bits, vp8->coeff_probs[2], context,
            vp8->quant[segment][VP8_BLOCK_UV], 0,
            vp8->coeffs + (size_t) block * 16
        );

        left[l] = above[a] = (uint8_t) (end != 0);
        if (end != 0) empty = 0;

        if (end > 1) nonzero |= 1u << block;
    }

    vp8->nonzero = nonzero;
    return empty;
}

#pragma endregion

#pragma region lossy transforms

/**
 * Inverts the Walsh-Hadamard transform of a macroblock's DC terms.
 *
 * The second order block holds the sixteen luma DC values, so its inverse
 * scatters one result into the DC slot of each luma block rather than producing
 * a picture of its own.
 */
static void inverse_wht(const int16_t* in, int16_t* out) {
    int32_t tmp[16];

    for (uint32_t i = 0; i < 4; i++) {
        int32_t a0 = in[0 + i] + in[12 + i];
        int32_t a1 = in[4 + i] + in[8 + i];
        int32_t a2 = in[4 + i] - in[8 + i];
        int32_t a3 = in[0 + i] - in[12 + i];

        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }

    for (uint32_t i = 0; i < 4; i++) {
        int32_t dc = tmp[0 + i * 4] + 3;
        int32_t a0 = dc + tmp[3 + i * 4];
        int32_t a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int32_t a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
        int32_t a3 = dc - tmp[3 + i * 4];

        out[0] = (int16_t) ((a0 + a1) >> 3);
        out[16] = (int16_t) ((a3 + a2) >> 3);
        out[32] = (int16_t) ((a0 - a1) >> 3);
        out[48] = (int16_t) ((a3 - a2) >> 3);

        out += 64;
    }
}

// the two multiplier approximations the transform is defined in terms of, which
// is why they are not the cosines they approximate
static int32_t mul1(int32_t a) {
    return ((a * 20091) >> 16) + a;
}

static int32_t mul2(int32_t a) {
    return (a * 35468) >> 16;
}

/** Inverts one 4x4 transform and adds the result to the prediction. */
static void inverse_dct(const int16_t* in, uint8_t* dest) {
    int32_t tmp[16];

    for (uint32_t i = 0; i < 4; i++) {
        int32_t a = in[0] + in[8];
        int32_t b = in[0] - in[8];
        int32_t c = mul2(in[4]) - mul1(in[12]);
        int32_t d = mul1(in[4]) + mul2(in[12]);

        tmp[i * 4 + 0] = a + d;
        tmp[i * 4 + 1] = b + c;
        tmp[i * 4 + 2] = b - c;
        tmp[i * 4 + 3] = a - d;

        in++;
    }

    for (uint32_t i = 0; i < 4; i++) {
        int32_t dc = tmp[i] + 4;
        int32_t a = dc + tmp[i + 8];
        int32_t b = dc - tmp[i + 8];
        int32_t c = mul2(tmp[i + 4]) - mul1(tmp[i + 12]);
        int32_t d = mul1(tmp[i + 4]) + mul2(tmp[i + 12]);

        dest[0] = tiny_clamp_u8(dest[0] + ((a + d) >> 3));
        dest[1] = tiny_clamp_u8(dest[1] + ((b + c) >> 3));
        dest[2] = tiny_clamp_u8(dest[2] + ((b - c) >> 3));
        dest[3] = tiny_clamp_u8(dest[3] + ((a - d) >> 3));

        dest += VP8_BPS;
    }
}

/** Spreads a lone DC term, which is what most blocks in a frame hold. */
static void inverse_dc_only(const int16_t* in, uint8_t* dest) {
    int32_t dc = (in[0] + 4) >> 3;

    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            dest[x] = tiny_clamp_u8(dest[x] + dc);
        }
        dest += VP8_BPS;
    }
}

static void add_residual(
    const Vp8Decoder* vp8, uint32_t block, uint8_t* dest, int full
) {
    const int16_t* in = vp8->coeffs + (size_t) block * 16;

    if (full) {
        inverse_dct(in, dest);
    }
    else if (in[0] != 0) {
        inverse_dc_only(in, dest);
    }
}

#pragma endregion

#pragma region lossy prediction

static uint8_t avg3(int32_t x, int32_t y, int32_t z) {
    return (uint8_t) ((x + y + y + z + 2) >> 2);
}

static uint8_t avg3p(const uint8_t* p) {
    return avg3(p[-1], p[0], p[1]);
}

static uint8_t avg2(int32_t x, int32_t y) {
    return (uint8_t) ((x + y + 1) >> 1);
}

static uint8_t avg2p(const uint8_t* p) {
    return avg2(p[0], p[1]);
}

static void fill_block(uint8_t* dest, uint32_t size, uint8_t value) {
    for (uint32_t y = 0; y < size; y++) {
        tiny_memset(dest + (size_t) y * VP8_BPS, value, size);
    }
}

/**
 * Predicts a whole 16x16 or 8x8 block from its edges.
 *
 * The DC mode has three extra variants for a block at the frame's edge, because
 * averaging the neighbors it does not have would mean averaging the fill
 * values. The other three modes read those fills deliberately: a top row of 127
 * and a left column of 129 are what the format specifies, and they differ by
 * two so that a vertical and a horizontal prediction of the same corner do not
 * agree by accident.
 */
static void predict_block(uint8_t* dest, uint32_t size, uint32_t mode) {
    const uint8_t* top = dest - VP8_BPS;

    switch (mode) {
        case VP8_DC_PRED: {
            uint32_t sum = size;

            for (uint32_t i = 0; i < size; i++) {
                sum += top[i];
                sum += dest[(size_t) i * VP8_BPS - 1];
            }

            fill_block(dest, size, (uint8_t) (sum / (2u * size)));
            break;
        }

        case VP8_TM_PRED: {
            int32_t corner = top[-1];

            for (uint32_t y = 0; y < size; y++) {
                uint8_t* row = dest + (size_t) y * VP8_BPS;
                int32_t left = row[-1];

                for (uint32_t x = 0; x < size; x++) {
                    row[x] = tiny_clamp_u8(left + top[x] - corner);
                }
            }
            break;
        }

        case VP8_V_PRED:
            for (uint32_t y = 0; y < size; y++) {
                tiny_memcpy(dest + (size_t) y * VP8_BPS, top, size);
            }
            break;

        case VP8_H_PRED:
            for (uint32_t y = 0; y < size; y++) {
                uint8_t* row = dest + (size_t) y * VP8_BPS;
                tiny_memset(row, row[-1], size);
            }
            break;

        case VP8_DC_NO_TOP: {
            uint32_t sum = size / 2u;

            for (uint32_t i = 0; i < size; i++) {
                sum += dest[(size_t) i * VP8_BPS - 1];
            }

            fill_block(dest, size, (uint8_t) (sum / size));
            break;
        }

        case VP8_DC_NO_LEFT: {
            uint32_t sum = size / 2u;

            for (uint32_t i = 0; i < size; i++) {
                sum += top[i];
            }

            fill_block(dest, size, (uint8_t) (sum / size));
            break;
        }

        default: fill_block(dest, size, 0x80); break;
    }
}

/**
 * Predicts one 4x4 luma subblock.
 *
 * Ten modes, of which the last six have no full block counterpart: each fills
 * the block along diagonals of a fixed slope, taking every pixel on a diagonal
 * from the same edge sample. The two modes with a half slope synthesize samples
 * midway between two real ones, which is what `avg2p` is for.
 */
static void predict_subblock(uint8_t* dest, uint32_t mode) {
    const uint8_t* above = dest - VP8_BPS;
    uint8_t left[4];
    uint8_t edge[9];

    for (uint32_t i = 0; i < 4; i++) {
        left[i] = dest[(size_t) i * VP8_BPS - 1];
    }

    edge[0] = left[3];
    edge[1] = left[2];
    edge[2] = left[1];
    edge[3] = left[0];
    edge[4] = above[-1];
    edge[5] = above[0];
    edge[6] = above[1];
    edge[7] = above[2];
    edge[8] = above[3];

    uint8_t block[4][4];

    switch (mode) {
        case 0: {
            uint32_t sum = 4;

            for (uint32_t i = 0; i < 4; i++) {
                sum += above[i] + left[i];
            }

            uint8_t value = (uint8_t) (sum >> 3);

            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) block[y][x] = value;
            }
            break;
        }

        case 1:
            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) {
                    block[y][x] = tiny_clamp_u8(left[y] + above[x] - above[-1]);
                }
            }
            break;

        case 2:
            for (uint32_t x = 0; x < 4; x++) {
                uint8_t value = avg3p(above + x);

                block[0][x] = value;
                block[1][x] = value;
                block[2][x] = value;
                block[3][x] = value;
            }
            break;

        case 3: {
            // the bottom row is the exception, because there is no left sample
            // below the block to center an average on
            uint8_t value = avg3(left[2], left[3], left[3]);
            uint32_t y = 3;

            while (1) {
                for (uint32_t x = 0; x < 4; x++) block[y][x] = value;

                if (y-- == 0) break;
                value = y == 0 ? avg3(above[-1], left[0], left[1])
                               : avg3(left[y - 1], left[y], left[y + 1]);
            }
            break;
        }

        case 4:
            block[0][0] = avg3p(above + 1);
            block[0][1] = block[1][0] = avg3p(above + 2);
            block[0][2] = block[1][1] = block[2][0] = avg3p(above + 3);
            block[0][3] = block[1][2] = block[2][1] = block[3][0] =
                avg3p(above + 4);
            block[1][3] = block[2][2] = block[3][1] = avg3p(above + 5);
            block[2][3] = block[3][2] = avg3p(above + 6);
            block[3][3] = avg3(above[6], above[7], above[7]);
            break;

        case 5:
            block[3][0] = avg3p(edge + 1);
            block[3][1] = block[2][0] = avg3p(edge + 2);
            block[3][2] = block[2][1] = block[1][0] = avg3p(edge + 3);
            block[3][3] = block[2][2] = block[1][1] = block[0][0] =
                avg3p(edge + 4);
            block[2][3] = block[1][2] = block[0][1] = avg3p(edge + 5);
            block[1][3] = block[0][2] = avg3p(edge + 6);
            block[0][3] = avg3p(edge + 7);
            break;

        case 6:
            block[3][0] = avg3p(edge + 2);
            block[2][0] = avg3p(edge + 3);
            block[3][1] = block[1][0] = avg3p(edge + 4);
            block[2][1] = block[0][0] = avg2p(edge + 4);
            block[3][2] = block[1][1] = avg3p(edge + 5);
            block[2][2] = block[0][1] = avg2p(edge + 5);
            block[3][3] = block[1][2] = avg3p(edge + 6);
            block[2][3] = block[0][2] = avg2p(edge + 6);
            block[1][3] = avg3p(edge + 7);
            block[0][3] = avg2p(edge + 7);
            break;

        case 7:
            block[0][0] = avg2p(above);
            block[1][0] = avg3p(above + 1);
            block[2][0] = block[0][1] = avg2p(above + 1);
            block[1][1] = block[3][0] = avg3p(above + 2);
            block[2][1] = block[0][2] = avg2p(above + 2);
            block[3][1] = block[1][2] = avg3p(above + 3);
            block[2][2] = block[0][3] = avg2p(above + 3);
            block[3][2] = block[1][3] = avg3p(above + 4);

            // the last two break the pattern; no already built pixel lies on
            // their diagonal
            block[2][3] = avg3p(above + 5);
            block[3][3] = avg3p(above + 6);
            break;

        case 8:
            block[3][0] = avg2p(edge);
            block[3][1] = avg3p(edge + 1);
            block[2][0] = block[3][2] = avg2p(edge + 1);
            block[2][1] = block[3][3] = avg3p(edge + 2);
            block[2][2] = block[1][0] = avg2p(edge + 2);
            block[2][3] = block[1][1] = avg3p(edge + 3);
            block[1][2] = block[0][0] = avg2p(edge + 3);
            block[1][3] = block[0][1] = avg3p(edge + 4);
            block[0][2] = avg3p(edge + 5);
            block[0][3] = avg3p(edge + 6);
            break;

        default:
            block[0][0] = avg2p(left);
            block[0][1] = avg3p(left + 1);
            block[0][2] = block[1][0] = avg2p(left + 1);
            block[0][3] = block[1][1] = avg3p(left + 2);
            block[1][2] = block[2][0] = avg2p(left + 2);
            block[1][3] = block[2][1] = avg3(left[2], left[3], left[3]);

            block[2][2] = block[2][3] = block[3][0] = block[3][1] =
                block[3][2] = block[3][3] = left[3];
            break;
    }

    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            dest[(size_t) y * VP8_BPS + x] = block[y][x];
        }
    }
}

/** Substitutes a DC variant when the block is missing a neighbor. */
static uint32_t dc_variant(uint32_t mode, uint32_t mb_x, uint32_t mb_y) {
    if (mode != VP8_DC_PRED) return mode;

    if (mb_x == 0) {
        return mb_y == 0 ? (uint32_t) VP8_DC_NO_TOP_LEFT
                         : (uint32_t) VP8_DC_NO_LEFT;
    }

    return mb_y == 0 ? (uint32_t) VP8_DC_NO_TOP : (uint32_t) VP8_DC_PRED;
}

#pragma endregion

#pragma region lossy loop filter

static int32_t clamp_s8(int32_t v) {
    return v < -128 ? -128 : (v > 127 ? 127 : v);
}

static int32_t difference(int32_t a, int32_t b) {
    int32_t d = a - b;

    return d < 0 ? -d : d;
}

/**
 * Brings two pixels either side of an edge closer together.
 *
 * The arithmetic is in signed eight bit values throughout, and the two rounding
 * terms differ on purpose: adding four to one side and three to the other keeps
 * the pair's average where it was when the adjustment lands exactly halfway.
 */
static int32_t common_adjust(
    int use_outer, uint8_t* p1, uint8_t* p0, uint8_t* q0, uint8_t* q1
) {
    int32_t v1 = (int32_t) *p1 - 128;
    int32_t v0 = (int32_t) *p0 - 128;
    int32_t w0 = (int32_t) *q0 - 128;
    int32_t w1 = (int32_t) *q1 - 128;

    int32_t a = clamp_s8((use_outer ? clamp_s8(v1 - w1) : 0) + 3 * (w0 - v0));
    int32_t b = clamp_s8(a + 3) >> 3;

    a = clamp_s8(a + 4) >> 3;

    *q0 = (uint8_t) (clamp_s8(w0 - a) + 128);
    *p0 = (uint8_t) (clamp_s8(v0 + b) + 128);

    return a;
}

static int filter_yes(
    uint32_t interior, uint32_t edge, const uint8_t* q0, int32_t step
) {
    int32_t p3 = q0[-4 * step];
    int32_t p2 = q0[-3 * step];
    int32_t p1 = q0[-2 * step];
    int32_t p0 = q0[-step];
    int32_t v0 = q0[0];
    int32_t v1 = q0[step];
    int32_t v2 = q0[2 * step];
    int32_t v3 = q0[3 * step];

    return difference(p0, v0) * 2 + difference(p1, v1) / 2 <= (int32_t) edge &&
           difference(p3, p2) <= (int32_t) interior &&
           difference(p2, p1) <= (int32_t) interior &&
           difference(p1, p0) <= (int32_t) interior &&
           difference(v3, v2) <= (int32_t) interior &&
           difference(v2, v1) <= (int32_t) interior &&
           difference(v1, v0) <= (int32_t) interior;
}

static int high_variance(uint32_t threshold, const uint8_t* q0, int32_t step) {
    return difference(q0[-2 * step], q0[-step]) > (int32_t) threshold ||
           difference(q0[step], q0[0]) > (int32_t) threshold;
}

/** The two tap filter, which is all the simple loop filter ever applies. */
static void filter_simple(uint8_t* q0, int32_t step, uint32_t edge) {
    int32_t d0 = difference(q0[-step], q0[0]);
    int32_t d1 = difference(q0[-2 * step], q0[step]);

    if (d0 * 2 + d1 / 2 <= (int32_t) edge) {
        common_adjust(1, q0 - 2 * step, q0 - step, q0, q0 + step);
    }
}

/** The normal filter's treatment of an edge between two subblocks. */
static void filter_inner(
    uint8_t* q0, int32_t step, uint32_t threshold, uint32_t interior,
    uint32_t edge
) {
    if (!filter_yes(interior, edge, q0, step)) return;

    int variance = high_variance(threshold, q0, step);
    int32_t a =
        (common_adjust(variance, q0 - 2 * step, q0 - step, q0, q0 + step) +
         1) >>
        1;

    if (variance) return;

    int32_t v1 = (int32_t) q0[step] - 128;
    int32_t p1 = (int32_t) q0[-2 * step] - 128;

    q0[step] = (uint8_t) (clamp_s8(v1 - a) + 128);
    q0[-2 * step] = (uint8_t) (clamp_s8(p1 + a) + 128);
}

/**
 * The normal filter's treatment of an edge between two macroblocks.
 *
 * Six pixels move rather than four, by decreasing fractions of the same
 * measured step: three sevenths, two sevenths and one seventh, which the
 * multipliers 27, 18 and 9 over 128 approximate.
 */
static void filter_edge(
    uint8_t* q0, int32_t step, uint32_t threshold, uint32_t interior,
    uint32_t edge
) {
    if (!filter_yes(interior, edge, q0, step)) return;

    if (high_variance(threshold, q0, step)) {
        common_adjust(1, q0 - 2 * step, q0 - step, q0, q0 + step);
        return;
    }

    int32_t p2 = (int32_t) q0[-3 * step] - 128;
    int32_t p1 = (int32_t) q0[-2 * step] - 128;
    int32_t p0 = (int32_t) q0[-step] - 128;
    int32_t v0 = (int32_t) q0[0] - 128;
    int32_t v1 = (int32_t) q0[step] - 128;
    int32_t v2 = (int32_t) q0[2 * step] - 128;

    int32_t w = clamp_s8(clamp_s8(p1 - v1) + 3 * (v0 - p0));

    int32_t a = clamp_s8((27 * w + 63) >> 7);
    q0[0] = (uint8_t) (clamp_s8(v0 - a) + 128);
    q0[-step] = (uint8_t) (clamp_s8(p0 + a) + 128);

    a = clamp_s8((18 * w + 63) >> 7);
    q0[step] = (uint8_t) (clamp_s8(v1 - a) + 128);
    q0[-2 * step] = (uint8_t) (clamp_s8(p1 + a) + 128);

    a = clamp_s8((9 * w + 63) >> 7);
    q0[2 * step] = (uint8_t) (clamp_s8(v2 - a) + 128);
    q0[-3 * step] = (uint8_t) (clamp_s8(p2 + a) + 128);
}

/**
 * Filters the whole frame, after every macroblock has been reconstructed.
 *
 * It has to be a second pass: intra prediction reads the pixels of the
 * macroblocks above and to the left, and it must read them unfiltered, so a
 * macroblock cannot be filtered until every macroblock that predicts from it
 * has been built.
 *
 * The order within a macroblock is fixed and matters, because a pixel near a
 * corner belongs to more than one segment and is filtered more than once: the
 * left edge, then the vertical interior edges, then the top edge, then the
 * horizontal interior edges.
 */
static void filter_frame(Vp8Decoder* vp8, uint32_t mb_rows) {
    if (vp8->filter_level == 0 && !vp8->segments_on && !vp8->delta_on) return;

    for (uint32_t mb_y = 0; mb_y < mb_rows; mb_y++) {
        for (uint32_t mb_x = 0; mb_x < vp8->mb_w; mb_x++) {
            size_t index = (size_t) mb_y * vp8->mb_w + mb_x;
            uint32_t level = vp8->levels[index];

            if (level == 0) continue;

            tiny_work_add(TINYIMG_WORK_FILTERED, 1);

            uint32_t interior = level;

            if (vp8->sharpness > 0) {
                interior >>= vp8->sharpness > 4 ? 2 : 1;

                if (interior > 9u - vp8->sharpness) {
                    interior = 9u - vp8->sharpness;
                }
            }
            if (interior == 0) interior = 1;

            // a keyframe's thresholds; an interframe has a third step this
            // codec never reaches
            uint32_t threshold = level >= 40 ? 2u : (level >= 15 ? 1u : 0u);

            uint32_t edge = 2u * level + interior + 4u;
            uint32_t inner_edge = 2u * level + interior;
            int inner = vp8->inner[index];

            uint8_t* y = vp8->y + (size_t) mb_y * 16u * vp8->y_stride +
                         (size_t) mb_x * 16u;
            uint8_t* u = vp8->u + (size_t) mb_y * 8u * vp8->uv_stride +
                         (size_t) mb_x * 8u;
            uint8_t* v = vp8->v + (size_t) mb_y * 8u * vp8->uv_stride +
                         (size_t) mb_x * 8u;

            int32_t ys = (int32_t) vp8->y_stride;
            int32_t cs = (int32_t) vp8->uv_stride;

            if (vp8->simple_filter) {
                // the simple filter leaves chroma alone entirely
                if (mb_x > 0) {
                    for (uint32_t i = 0; i < 16; i++) {
                        filter_simple(y + (size_t) i * vp8->y_stride, 1, edge);
                    }
                }

                if (inner) {
                    for (uint32_t k = 4; k < 16; k += 4) {
                        for (uint32_t i = 0; i < 16; i++) {
                            filter_simple(
                                y + (size_t) i * vp8->y_stride + k, 1,
                                inner_edge
                            );
                        }
                    }
                }

                if (mb_y > 0) {
                    for (uint32_t i = 0; i < 16; i++) {
                        filter_simple(y + i, ys, edge);
                    }
                }

                if (inner) {
                    for (uint32_t k = 4; k < 16; k += 4) {
                        for (uint32_t i = 0; i < 16; i++) {
                            filter_simple(
                                y + (size_t) k * vp8->y_stride + i, ys,
                                inner_edge
                            );
                        }
                    }
                }

                continue;
            }

            if (mb_x > 0) {
                for (uint32_t i = 0; i < 16; i++) {
                    filter_edge(
                        y + (size_t) i * vp8->y_stride, 1, threshold, interior,
                        edge
                    );
                }
                for (uint32_t i = 0; i < 8; i++) {
                    filter_edge(
                        u + (size_t) i * vp8->uv_stride, 1, threshold, interior,
                        edge
                    );
                    filter_edge(
                        v + (size_t) i * vp8->uv_stride, 1, threshold, interior,
                        edge
                    );
                }
            }

            if (inner) {
                for (uint32_t k = 4; k < 16; k += 4) {
                    for (uint32_t i = 0; i < 16; i++) {
                        filter_inner(
                            y + (size_t) i * vp8->y_stride + k, 1, threshold,
                            interior, inner_edge
                        );
                    }
                }
                for (uint32_t i = 0; i < 8; i++) {
                    filter_inner(
                        u + (size_t) i * vp8->uv_stride + 4, 1, threshold,
                        interior, inner_edge
                    );
                    filter_inner(
                        v + (size_t) i * vp8->uv_stride + 4, 1, threshold,
                        interior, inner_edge
                    );
                }
            }

            if (mb_y > 0) {
                for (uint32_t i = 0; i < 16; i++) {
                    filter_edge(y + i, ys, threshold, interior, edge);
                }
                for (uint32_t i = 0; i < 8; i++) {
                    filter_edge(u + i, cs, threshold, interior, edge);
                    filter_edge(v + i, cs, threshold, interior, edge);
                }
            }

            if (inner) {
                for (uint32_t k = 4; k < 16; k += 4) {
                    for (uint32_t i = 0; i < 16; i++) {
                        filter_inner(
                            y + (size_t) k * vp8->y_stride + i, ys, threshold,
                            interior, inner_edge
                        );
                    }
                }
                for (uint32_t i = 0; i < 8; i++) {
                    filter_inner(
                        u + 4u * vp8->uv_stride + i, cs, threshold, interior,
                        inner_edge
                    );
                    filter_inner(
                        v + 4u * vp8->uv_stride + i, cs, threshold, interior,
                        inner_edge
                    );
                }
            }
        }
    }
}

#pragma endregion

#pragma region lossy reconstruction

/**
 * Reads one macroblock's prediction modes.
 *
 * A keyframe codes the subblock modes against the modes of the subblock above
 * and to the left, which is why the row above has to be carried across the
 * frame rather than kept per macroblock.
 */
static void read_modes(
    Vp8Decoder* vp8, uint32_t mb_x, uint8_t* modes, int* is_i4x4,
    uint32_t* uv_mode
) {
    Vp8Bool* bits = &vp8->part0;
    uint8_t* top = vp8->above_modes + (size_t) mb_x * 4u;
    uint8_t* left = vp8->left_modes;

    *is_i4x4 = !bool_get(bits, 145);

    if (!*is_i4x4) {
        uint32_t mode = bool_get(bits, 156)
                            ? (bool_get(bits, 128) ? (uint32_t) VP8_TM_PRED
                                                   : (uint32_t) VP8_H_PRED)
                            : (bool_get(bits, 163) ? (uint32_t) VP8_V_PRED
                                                   : (uint32_t) VP8_DC_PRED);

        modes[0] = (uint8_t) mode;

        // a whole block prediction stands in for all sixteen subblock modes
        // when the next macroblock codes its own against them
        tiny_memset(top, (uint8_t) mode, 4);
        tiny_memset(left, (uint8_t) mode, 4);
    }
    else {
        for (uint32_t y = 0; y < 4; y++) {
            uint32_t mode = left[y];

            for (uint32_t x = 0; x < 4; x++) {
                mode = (uint32_t) bool_tree(
                    bits, vp8_bmode_tree, vp8_bmode_probs[top[x]][mode]
                );

                top[x] = (uint8_t) mode;
                modes[y * 4 + x] = (uint8_t) mode;
            }

            left[y] = (uint8_t) mode;
        }
    }

    *uv_mode = !bool_get(bits, 142)
                   ? (uint32_t) VP8_DC_PRED
                   : (!bool_get(bits, 114)
                          ? (uint32_t) VP8_V_PRED
                          : (bool_get(bits, 183) ? (uint32_t) VP8_TM_PRED
                                                 : (uint32_t) VP8_H_PRED));
}

/**
 * Fills the working area's borders for a macroblock.
 *
 * The row above and column to the left come from the frame where they exist and
 * from the format's fill values where they do not. The four samples past the
 * top right corner are the fiddly part: the subblocks on the right edge of a
 * macroblock predict from pixels that belong to the macroblock diagonally above
 * and to the right, which for the last column of the frame does not exist and
 * repeats the rightmost pixel instead.
 */
static void load_borders(
    Vp8Decoder* vp8, uint32_t mb_x, uint32_t mb_y, int is_i4x4
) {
    uint8_t* y = vp8->work_y + VP8_BPS + 8;
    uint8_t* u = vp8->work_u + VP8_BPS + 8;
    uint8_t* v = vp8->work_v + VP8_BPS + 8;

    if (mb_x > 0) {
        const uint8_t* source = vp8->y + (size_t) mb_y * 16u * vp8->y_stride +
                                (size_t) mb_x * 16u - 1u;

        for (uint32_t i = 0; i < 16; i++) {
            y[(size_t) i * VP8_BPS - 1] = source[(size_t) i * vp8->y_stride];
        }

        const uint8_t* cu = vp8->u + (size_t) mb_y * 8u * vp8->uv_stride +
                            (size_t) mb_x * 8u - 1u;
        const uint8_t* cv = vp8->v + (size_t) mb_y * 8u * vp8->uv_stride +
                            (size_t) mb_x * 8u - 1u;

        for (uint32_t i = 0; i < 8; i++) {
            u[(size_t) i * VP8_BPS - 1] = cu[(size_t) i * vp8->uv_stride];
            v[(size_t) i * VP8_BPS - 1] = cv[(size_t) i * vp8->uv_stride];
        }
    }
    else {
        for (uint32_t i = 0; i < 16; i++) {
            y[(size_t) i * VP8_BPS - 1] = 129;
        }
        for (uint32_t i = 0; i < 8; i++) {
            u[(size_t) i * VP8_BPS - 1] = 129;
            v[(size_t) i * VP8_BPS - 1] = 129;
        }
    }

    if (mb_y > 0) {
        const uint8_t* source = vp8->y +
                                ((size_t) mb_y * 16u - 1u) * vp8->y_stride +
                                (size_t) mb_x * 16u;

        tiny_memcpy(y - VP8_BPS, source, 16);

        const uint8_t* cu = vp8->u +
                            ((size_t) mb_y * 8u - 1u) * vp8->uv_stride +
                            (size_t) mb_x * 8u;
        const uint8_t* cv = vp8->v +
                            ((size_t) mb_y * 8u - 1u) * vp8->uv_stride +
                            (size_t) mb_x * 8u;

        tiny_memcpy(u - VP8_BPS, cu, 8);
        tiny_memcpy(v - VP8_BPS, cv, 8);

        y[-1 - VP8_BPS] = mb_x > 0 ? source[-1] : 129u;
        u[-1 - VP8_BPS] = mb_x > 0 ? cu[-1] : 129u;
        v[-1 - VP8_BPS] = mb_x > 0 ? cv[-1] : 129u;

        if (is_i4x4) {
            if (mb_x + 1 < vp8->mb_w) {
                tiny_memcpy(y - VP8_BPS + 16, source + 16, 4);
            }
            else {
                tiny_memset(y - VP8_BPS + 16, source[15], 4);
            }
        }
    }
    else {
        // above the frame everything reads 127, including the corner and the
        // four samples past the top right
        tiny_memset(y - VP8_BPS - 1, 127, 16 + 4 + 1);
        tiny_memset(u - VP8_BPS - 1, 127, 8 + 1);
        tiny_memset(v - VP8_BPS - 1, 127, 8 + 1);
    }

    if (is_i4x4) {
        /*
         * The three subblocks below the top right one predict from the same
         * four samples it does, so they are copied down rather than special
         * cased in the reader.
         *
         * This runs for the top macroblock row as well, where those samples
         * are the 127 fill: leaving them out there reads whatever the previous
         * macroblock wrote, which is stale and not 127.
         */
        for (uint32_t k = 3; k < 12; k += 4) {
            tiny_memcpy(y + (size_t) k * VP8_BPS + 16, y - VP8_BPS + 16, 4);
        }
    }
}

/** Copies the finished macroblock out of the working area into the frame. */
static void store_block(Vp8Decoder* vp8, uint32_t mb_x, uint32_t mb_y) {
    const uint8_t* y = vp8->work_y + VP8_BPS + 8;
    const uint8_t* u = vp8->work_u + VP8_BPS + 8;
    const uint8_t* v = vp8->work_v + VP8_BPS + 8;

    uint8_t* dest_y =
        vp8->y + (size_t) mb_y * 16u * vp8->y_stride + (size_t) mb_x * 16u;
    uint8_t* dest_u =
        vp8->u + (size_t) mb_y * 8u * vp8->uv_stride + (size_t) mb_x * 8u;
    uint8_t* dest_v =
        vp8->v + (size_t) mb_y * 8u * vp8->uv_stride + (size_t) mb_x * 8u;

    for (uint32_t i = 0; i < 16; i++) {
        tiny_memcpy(
            dest_y + (size_t) i * vp8->y_stride, y + (size_t) i * VP8_BPS, 16
        );
    }

    for (uint32_t i = 0; i < 8; i++) {
        tiny_memcpy(
            dest_u + (size_t) i * vp8->uv_stride, u + (size_t) i * VP8_BPS, 8
        );
        tiny_memcpy(
            dest_v + (size_t) i * vp8->uv_stride, v + (size_t) i * VP8_BPS, 8
        );
    }
}

/** Predicts one macroblock and adds its residual. */
static void reconstruct(
    Vp8Decoder* vp8, uint32_t mb_x, uint32_t mb_y, const uint8_t* modes,
    int is_i4x4, uint32_t uv_mode
) {
    uint8_t* y = vp8->work_y + VP8_BPS + 8;
    uint8_t* u = vp8->work_u + VP8_BPS + 8;
    uint8_t* v = vp8->work_v + VP8_BPS + 8;

    if (is_i4x4) {
        // each subblock is predicted from pixels the subblock before it just
        // wrote, so prediction and reconstruction interleave
        for (uint32_t n = 0; n < 16; n++) {
            uint8_t* dest = y + vp8_scan[n];

            predict_subblock(dest, modes[n]);
            add_residual(vp8, n, dest, (vp8->nonzero >> n) & 1u);
        }
    }
    else {
        predict_block(y, 16, dc_variant(modes[0], mb_x, mb_y));

        for (uint32_t n = 0; n < 16; n++) {
            add_residual(vp8, n, y + vp8_scan[n], (vp8->nonzero >> n) & 1u);
        }
    }

    uint32_t chroma = dc_variant(uv_mode, mb_x, mb_y);

    predict_block(u, 8, chroma);
    predict_block(v, 8, chroma);

    for (uint32_t n = 0; n < 4; n++) {
        uint32_t at = (n >> 1) * 4u * VP8_BPS + (n & 1u) * 4u;

        add_residual(vp8, 16 + n, u + at, (vp8->nonzero >> (16 + n)) & 1u);
        add_residual(vp8, 20 + n, v + at, (vp8->nonzero >> (20 + n)) & 1u);
    }
}

#pragma endregion

#pragma region lossy color

/**
 * Converts one pixel out of the format's own color space.
 *
 * The coefficients and the 8708 and 14234 offsets are libwebp's fixed point
 * form of the studio range conversion, kept exactly so that a decode can be
 * compared against that implementation byte for byte rather than through a
 * tolerance.
 */
static void yuv_to_rgb(int32_t y, int32_t u, int32_t v, uint8_t* out) {
    int32_t luma = (y * 19077) >> 8;

    int32_t r = luma + ((v * 26149) >> 8) - 14234;
    int32_t g = luma - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708;
    int32_t b = luma + ((u * 33050) >> 8) - 17685;

    out[0] = tiny_clamp_u8(r >> 6);
    out[1] = tiny_clamp_u8(g >> 6);
    out[2] = tiny_clamp_u8(b >> 6);
    out[3] = 255;
}

/**
 * Upsamples two chroma rows onto two luma rows while converting.
 *
 * Chroma is stored at half resolution in both directions, and taking the
 * nearest sample leaves visible blocks on any diagonal edge. This weights the
 * four surrounding samples 9:3:3:1 along each diagonal, which is what libwebp
 * does by default and therefore what a comparison against it has to do.
 */
static void upsample_pair(
    const uint8_t* top_y, const uint8_t* bottom_y, const uint8_t* top_u,
    const uint8_t* top_v, const uint8_t* cur_u, const uint8_t* cur_v,
    uint8_t* top_dest, uint8_t* bottom_dest, uint32_t length
) {
    uint32_t last = (length - 1u) >> 1;

    // the two chroma channels are carried in one word so the averages are one
    // set of adds rather than two
    uint32_t corner = (uint32_t) top_u[0] | ((uint32_t) top_v[0] << 16);
    uint32_t left = (uint32_t) cur_u[0] | ((uint32_t) cur_v[0] << 16);

    uint32_t value = (3u * corner + left + 0x00020002u) >> 2;
    yuv_to_rgb(
        top_y[0], (int32_t) (value & 0xFFu), (int32_t) (value >> 16), top_dest
    );

    if (bottom_y) {
        value = (3u * left + corner + 0x00020002u) >> 2;
        yuv_to_rgb(
            bottom_y[0], (int32_t) (value & 0xFFu), (int32_t) (value >> 16),
            bottom_dest
        );
    }

    for (uint32_t x = 1; x <= last; x++) {
        uint32_t top = (uint32_t) top_u[x] | ((uint32_t) top_v[x] << 16);
        uint32_t here = (uint32_t) cur_u[x] | ((uint32_t) cur_v[x] << 16);

        uint32_t total = corner + top + left + here + 0x00080008u;
        uint32_t across = (total + 2u * (top + left)) >> 3;
        uint32_t down = (total + 2u * (corner + here)) >> 3;

        uint32_t first = (across + corner) >> 1;
        uint32_t second = (down + top) >> 1;

        yuv_to_rgb(
            top_y[2 * x - 1], (int32_t) (first & 0xFFu),
            (int32_t) (first >> 16), top_dest + (2 * x - 1) * 4
        );
        yuv_to_rgb(
            top_y[2 * x], (int32_t) (second & 0xFFu), (int32_t) (second >> 16),
            top_dest + 2 * x * 4
        );

        if (bottom_y) {
            first = (down + left) >> 1;
            second = (across + here) >> 1;

            yuv_to_rgb(
                bottom_y[2 * x - 1], (int32_t) (first & 0xFFu),
                (int32_t) (first >> 16), bottom_dest + (2 * x - 1) * 4
            );
            yuv_to_rgb(
                bottom_y[2 * x], (int32_t) (second & 0xFFu),
                (int32_t) (second >> 16), bottom_dest + 2 * x * 4
            );
        }

        corner = top;
        left = here;
    }

    if ((length & 1u) == 0) {
        value = (3u * corner + left + 0x00020002u) >> 2;
        yuv_to_rgb(
            top_y[length - 1], (int32_t) (value & 0xFFu),
            (int32_t) (value >> 16), top_dest + (length - 1) * 4
        );

        if (bottom_y) {
            value = (3u * left + corner + 0x00020002u) >> 2;
            yuv_to_rgb(
                bottom_y[length - 1], (int32_t) (value & 0xFFu),
                (int32_t) (value >> 16), bottom_dest + (length - 1) * 4
            );
        }
    }
}

/**
 * Converts the planes into RGBA, upsampling chroma as it goes.
 *
 * The pairing is the part worth stating, because the obvious one is wrong: a
 * chroma sample sits at the center of the two by two luma block it covers, so
 * the pair of output rows a chroma row pair straddles is offset by one. Rows
 * `2k-1` and `2k` come from chroma rows `k-1` and `k`, which leaves the first
 * row, and the last row of an even height image, taking a single chroma row
 * with no second one to weight against.
 *
 * Stops after `rows`, which is what the region asked for. The conversion has no
 * dependency between rows, so unlike the reconstruction and the filter this can
 * be cut to exactly the rows a caller reads rather than to a raster prefix.
 */
static void planes_to_rgba(
    const Vp8Decoder* vp8, uint8_t* rgba, uint32_t rows
) {
    uint32_t width = vp8->width;
    uint32_t full = vp8->height;

    // one row pair beyond the request, so every row a caller reads still gets
    // the chroma pair its position calls for
    uint32_t height = (rows + 2u >= full) ? full : rows + 2u;
    size_t row_bytes = (size_t) width * 4u;

    const uint8_t* first_u = vp8->u;
    const uint8_t* first_v = vp8->v;

    upsample_pair(
        vp8->y, 0, first_u, first_v, first_u, first_v, rgba, 0, width
    );

    for (uint32_t y = 0; y + 2 < height; y += 2) {
        uint32_t k = y / 2u + 1u;

        const uint8_t* top_u = vp8->u + (size_t) (k - 1u) * vp8->uv_stride;
        const uint8_t* top_v = vp8->v + (size_t) (k - 1u) * vp8->uv_stride;
        const uint8_t* cur_u = vp8->u + (size_t) k * vp8->uv_stride;
        const uint8_t* cur_v = vp8->v + (size_t) k * vp8->uv_stride;

        upsample_pair(
            vp8->y + (size_t) (y + 1u) * vp8->y_stride,
            vp8->y + (size_t) (y + 2u) * vp8->y_stride, top_u, top_v, cur_u,
            cur_v, rgba + (size_t) (y + 1u) * row_bytes,
            rgba + (size_t) (y + 2u) * row_bytes, width
        );
    }

    // the single chroma row belongs to the image's last row, so it runs only
    // when the conversion actually reached it rather than at a truncation
    if (height == full && (full & 1u) == 0) {
        uint32_t last = (full - 1u) / 2u;

        const uint8_t* cur_u = vp8->u + (size_t) last * vp8->uv_stride;
        const uint8_t* cur_v = vp8->v + (size_t) last * vp8->uv_stride;

        upsample_pair(
            vp8->y + (size_t) (full - 1u) * vp8->y_stride, 0, cur_u, cur_v,
            cur_u, cur_v, rgba + (size_t) (full - 1u) * row_bytes, 0, width
        );
    }
}

#pragma endregion

#pragma region lossy decode

/**
 * Decodes a lossy frame into a plane of RGBA.
 *
 * Two passes over the macroblocks. The first reads each one's modes and
 * coefficients and reconstructs it, which has to happen in order because every
 * macroblock predicts from its neighbors. The second filters, which has to
 * come after all of the first for the same reason.
 *
 * `rows` is how many luma rows the caller will read. Both passes run in raster
 * order and neither lets a later macroblock reach an earlier one, so everything
 * past the last macroblock row the region touches can be dropped: not the whole
 * region's complement, because a macroblock reads its left and upper neighbors,
 * but the tail. A region that reaches the last row costs what it always did.
 */
static int decode_lossy(
    const uint8_t* data, size_t size, uint32_t width, uint32_t height,
    uint8_t* rgba, uint32_t rows, uint8_t effort
) {
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;

    int result = parse_vp8(data, size, &coded_width, &coded_height);
    if (result != TINYIMG_OK) return result;

    uint32_t tag = read_le24(data);
    size_t part0 = (tag >> 5) & 0x7FFFFu;

    if (part0 == 0 || part0 + 10u > size) return TINYIMG_ERR_CORRUPT;

    Vp8Decoder* vp8 = tiny_arena_alloc(sizeof(Vp8Decoder), 8);
    if (!vp8) return TINYIMG_ERR_MEMORY;

    tiny_memset(vp8, 0, sizeof(*vp8));

    vp8->width = coded_width < width ? coded_width : width;
    vp8->height = coded_height < height ? coded_height : height;
    vp8->mb_w = (coded_width + 15u) / 16u;
    vp8->mb_h = (coded_height + 15u) / 16u;

    result = parse_header(vp8, data + 10, size - 10u, part0);
    if (result != TINYIMG_OK) return result;

    vp8->y_stride = (size_t) vp8->mb_w * 16u;
    vp8->uv_stride = (size_t) vp8->mb_w * 8u;

    vp8->y = tiny_arena_alloc(vp8->y_stride * vp8->mb_h * 16u, 0);
    vp8->u = tiny_arena_alloc(vp8->uv_stride * vp8->mb_h * 8u, 0);
    vp8->v = tiny_arena_alloc(vp8->uv_stride * vp8->mb_h * 8u, 0);

    vp8->above_nz = tiny_arena_alloc((size_t) vp8->mb_w * 9u, 0);
    vp8->above_modes = tiny_arena_alloc((size_t) vp8->mb_w * 4u, 0);

    size_t blocks = (size_t) vp8->mb_w * vp8->mb_h;

    vp8->levels = tiny_arena_alloc(blocks, 0);
    vp8->inner = tiny_arena_alloc(blocks, 0);

    if (!vp8->y || !vp8->u || !vp8->v || !vp8->above_nz || !vp8->above_modes ||
        !vp8->levels || !vp8->inner) {
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(vp8->above_nz, 0, (size_t) vp8->mb_w * 9u);
    tiny_memset(vp8->above_modes, 0, (size_t) vp8->mb_w * 4u);

    /*
     * Filtering macroblock row `m` writes luma rows 16m-4 through 16m+15, since
     * a top edge reaches four rows back, so a region ending at row R needs
     * every m with 16m-4 <= R-1. The reconstruction has to cover the same rows,
     * because that is what the filter reads.
     */
    uint32_t wanted = rows == 0 || rows > vp8->height ? vp8->height : rows;
    uint32_t mb_rows = (wanted + 3u) / 16u + 1u;

    if (mb_rows > vp8->mb_h) mb_rows = vp8->mb_h;

    for (uint32_t mb_y = 0; mb_y < mb_rows; mb_y++) {
        Vp8Bool* tokens = &vp8->tokens[mb_y & (vp8->partitions - 1u)];

        tiny_memset(vp8->left_nz, 0, sizeof(vp8->left_nz));
        tiny_memset(vp8->left_modes, 0, sizeof(vp8->left_modes));

        for (uint32_t mb_x = 0; mb_x < vp8->mb_w; mb_x++) {
            uint32_t segment = 0;

            if (vp8->segments_on && vp8->update_map) {
                segment = (uint32_t) bool_tree(
                    &vp8->part0, vp8_segment_tree, vp8->segment_probs
                );
            }

            int skip = vp8->skip_on ? bool_get(&vp8->part0, vp8->skip_prob) : 0;

            uint8_t modes[16];
            int is_i4x4 = 0;
            uint32_t uv_mode = 0;

            read_modes(vp8, mb_x, modes, &is_i4x4, &uv_mode);

            int empty = 1;

            if (!skip) {
                empty = read_residuals(vp8, tokens, mb_x, segment, is_i4x4);
            }
            else {
                tiny_memset(vp8->coeffs, 0, sizeof(vp8->coeffs));
                vp8->nonzero = 0;

                // the eight luma and chroma contexts clear, but the second
                // order one only when the macroblock would have had such a
                // block: a skipped subblock coded macroblock never has one, so
                // the context stays as the last macroblock that did left it
                uint8_t y2 = vp8->left_nz[8];
                uint8_t above_y2 = vp8->above_nz[(size_t) mb_x * 9u + 8u];

                tiny_memset(vp8->left_nz, 0, sizeof(vp8->left_nz));
                tiny_memset(vp8->above_nz + (size_t) mb_x * 9u, 0, 9);

                if (is_i4x4) {
                    vp8->left_nz[8] = y2;
                    vp8->above_nz[(size_t) mb_x * 9u + 8u] = above_y2;
                }
            }

            if (!is_i4x4) {
                inverse_wht(vp8->coeffs + 24 * 16, vp8->coeffs);
            }

            load_borders(vp8, mb_x, mb_y, is_i4x4);
            reconstruct(vp8, mb_x, mb_y, modes, is_i4x4, uv_mode);
            store_block(vp8, mb_x, mb_y);

            int32_t level = vp8->filter_level;

            if (vp8->segments_on) {
                level = vp8->absolute
                            ? vp8->segment_filter[segment]
                            : vp8->filter_level + vp8->segment_filter[segment];
            }

            if (vp8->delta_on) {
                level += vp8->ref_delta[0];
                if (is_i4x4) level += vp8->mode_delta[0];
            }

            level = level < 0 ? 0 : (level > 63 ? 63 : level);

            size_t index = (size_t) mb_y * vp8->mb_w + mb_x;

            vp8->levels[index] = (uint8_t) level;
            vp8->inner[index] = (uint8_t) (is_i4x4 || !empty);

            tiny_work_add(TINYIMG_WORK_MACROBLOCKS, 1);
        }

        // the coefficient partition is the one worth checking. The first
        // partition ends where the last macroblock's modes end, so its tail is
        // always inside the lookahead, and a check there rejects valid frames
        if (tokens->eof) return TINYIMG_ERR_CORRUPT;
    }

    /*
     * The deblocking filter is a post-pass here, not an in-loop one.
     *
     * VP8 defines it as in-loop because a later frame predicts from the
     * filtered result, but this codec decodes one keyframe and nothing
     * references what it writes. Intra prediction inside the frame reads the
     * unfiltered reconstruction, which is why the call sits after the whole
     * frame rather than between macroblock rows. So skipping it changes the
     * output and nothing else, which is what makes it the one thing a lossy
     * decode can drop.
     */
    if (effort != TINYIMG_EFFORT_FAST) filter_frame(vp8, mb_rows);

    planes_to_rgba(vp8, rgba, wanted);

    // a frame coded larger than the canvas leaves the rest of the plane as the
    // caller had it, which for a still image cannot happen and for an animation
    // frame is the canvas showing through
    return TINYIMG_OK;
}

#pragma endregion

#pragma region lossless encode

/**
 * @brief How many colors a cache may remember, as a power of two.
 *
 * The format allows eleven bits. Eight is used because the shared Huffman
 * length builder picks the two lightest nodes by scanning, which is quadratic
 * in the alphabet: eight bits puts the green alphabet at 536 symbols and about
 * 143k comparisons, and eleven would put it at 2328 and 2.7 million. The
 * decoder still reads all eleven, since reading a code costs nothing to build.
 */
#define VP8L_ENCODE_CACHE_BITS 8u

/** Shorter copies cost more to name than they save. */
#define VP8L_MIN_MATCH 4u

/**
 * @brief The shortest copy worth taking from somewhere other than nearby.
 *
 * A copy from one pixel back or one row back is named by a short code with no
 * extra bits. Anywhere else costs up to eighteen extra bits plus a symbol from
 * a thinly used alphabet, so it has to be long to pay, and a short one is
 * almost always a loss even where the arithmetic says otherwise: the pixels a
 * copy replaces are the most predictable in the image, so they were cheaper
 * than the average literal the estimate is built from.
 */
#define VP8L_FAR_MATCH 64u

/** The longest run one length code can name. */
#define VP8L_MAX_MATCH 4096u

#define VP8L_HASH_BITS 15u
#define VP8L_WINDOW (1u << 20)
#define VP8L_CHAIN 24u

/** Tile size for the predictor transform, as a power of two. */
#define VP8L_TILE_BITS 4u

typedef struct {
    uint16_t code;
    uint8_t length;
} WebpCode;

typedef struct {
    uint32_t* freq;
    uint8_t* lengths;
    WebpCode* codes;
    uint32_t count;
    /**
     * @brief The only symbol, when one symbol is all the alphabet used.
     *
     * Such a symbol is written with no bits at all, because that is how the
     * decoder reads it. Giving it the one bit a canonical code would assign
     * desynchronizes the stream immediately, and an opaque image's alpha
     * channel is exactly this case, so it is not a corner.
     */
    int32_t single;
} WebpAlphabet;

typedef struct {
    uint32_t width;
    size_t count;

    uint32_t cache_bits;
    uint32_t* cache;

    /** Hash chain over pairs of pixels, for finding a repeated run. */
    int32_t* head;
    int32_t* chain;

    /** Packed row and column offset back to a distance code. */
    uint8_t inverse[128];

    WebpAlphabet green;
    WebpAlphabet red;
    WebpAlphabet blue;
    WebpAlphabet alpha;
    WebpAlphabet distance;

    /** NULL while the pass is only counting symbols. */
    TinyBitWriter* out;
    /** Bits the counting pass added up. */
    uint64_t cost;

    /**
     * @brief What one literal pixel costs, in bits, for this image.
     *
     * Measured by a first pass rather than assumed, because it decides whether
     * a distant copy is worth its extra bits and the answer differs by an
     * order of magnitude between a photograph and a drawing.
     */
    uint32_t literal_bits;
    /** Zero during the first pass, which has no cost model to judge with. */
    int use_chain;
} WebpEncoder;

/**
 * Subtracts two pixels channel by channel.
 *
 * One channel at a time rather than two at once, which the addition it undoes
 * can get away with: adding a masked pair discards the carry out of the high
 * channel, but subtracting one borrows out of the low channel, straight
 * through the masked off byte between them and into the high channel. That
 * turns an alpha of zero into an alpha of 255 whenever green underflows.
 */
static uint32_t subtract_pixels(uint32_t a, uint32_t b) {
    uint32_t out = 0;

    for (uint32_t shift = 0; shift < 32; shift += 8) {
        uint32_t d = ((a >> shift) & 0xFFu) - ((b >> shift) & 0xFFu);

        out |= (d & 0xFFu) << shift;
    }

    return out;
}

/** Reverses a canonical code so a least significant bit first reader
 * unreverses it. */
static uint32_t reverse_code(uint32_t code, uint32_t length) {
    uint32_t out = 0;

    for (uint32_t i = 0; i < length; i++) {
        out = (out << 1) | ((code >> i) & 1u);
    }

    return out;
}

static void codes_from_lengths(
    WebpCode* codes, const uint8_t* lengths, uint32_t count
) {
    uint16_t counts[16];
    tiny_memset(counts, 0, sizeof(counts));

    for (uint32_t i = 0; i < count; i++) {
        counts[lengths[i]]++;
    }
    counts[0] = 0;

    uint16_t next[16];
    uint32_t code = 0;

    for (uint32_t length = 1; length <= VP8L_MAX_BITS; length++) {
        next[length] = (uint16_t) code;
        code = (code + counts[length]) << 1;
    }

    for (uint32_t i = 0; i < count; i++) {
        codes[i].length = lengths[i];
        codes[i].code =
            lengths[i] > 0
                ? (uint16_t) reverse_code(next[lengths[i]]++, lengths[i])
                : 0;
    }
}

/**
 * Splits a value into the symbol that names its range and the offset inside.
 *
 * The inverse of the decoder's prefix reader, derived from it rather than
 * tabulated: the first four values are their own symbols, and past that each
 * power of two spans two symbols, one for its lower half and one for its
 * upper.
 */
static void prefix_encode(
    uint32_t value, uint32_t* symbol, uint32_t* bits, uint32_t* extra
) {
    uint32_t n = value - 1u;

    if (n < 4) {
        *symbol = n;
        *bits = 0;
        *extra = 0;
        return;
    }

    uint32_t top = 31u - (uint32_t) __builtin_clz(n);
    uint32_t upper = 3u << (top - 1u);

    *symbol = n < upper ? 2u * top : 2u * top + 1u;
    *bits = top - 1u;
    *extra = n - (n < upper ? (2u << (top - 1u)) : upper);
}

/** Turns a pixel distance into the code that names it. */
static uint32_t distance_to_code(const WebpEncoder* enc, uint32_t distance) {
    uint32_t y = distance / enc->width;
    int32_t x = (int32_t) (distance - y * enc->width);

    if (x <= 8 && y < 8) {
        uint8_t code = enc->inverse[(y << 4) | (uint32_t) (8 - x)];
        if (code != 0xFFu) return (uint32_t) code + 1u;
    }

    // a reference just off the right edge is a row further up and a few columns
    // right, which the table also names
    if (x > (int32_t) enc->width - 8 && y < 7) {
        uint32_t at =
            ((y + 1u) << 4) | (uint32_t) (8 + ((int32_t) enc->width - x));

        if (at < 128) {
            uint8_t code = enc->inverse[at];
            if (code != 0xFFu) return (uint32_t) code + 1u;
        }
    }

    return distance + VP8L_PLANE_CODES;
}

static void count_or_write(
    WebpEncoder* enc, const WebpAlphabet* alphabet, uint32_t symbol
) {
    if (!enc->out) {
        alphabet->freq[symbol]++;
        return;
    }

    tiny_bitwriter_lsb(
        enc->out, alphabet->codes[symbol].code, alphabet->codes[symbol].length
    );
}

static void put_extra(WebpEncoder* enc, uint32_t value, uint32_t bits) {
    if (bits == 0) return;

    if (!enc->out) {
        enc->cost += bits;
        return;
    }

    tiny_bitwriter_lsb(enc->out, value, bits);
}

/**
 * Hashes the two pixels starting here.
 *
 * Deliberately 32 bit arithmetic that wraps, then the top bits of the result.
 * A 64 bit product shifted down instead leaves the high bits zero for any
 * pixel below 2^24, which is most of them, and collapses the whole table into
 * a handful of buckets: with that version the chain found nothing and looked
 * like evidence that chains do not help.
 */
static uint32_t hash_pair(const uint32_t* pixels) {
    uint32_t key = pixels[1] * 0xC6A4A793u;

    key += pixels[0] * 0x5BD1E996u;

    return key >> (32u - VP8L_HASH_BITS);
}

/**
 * Finds the longest run already written that repeats the one starting here.
 *
 * A distance shorter than the length is allowed and useful: it names a
 * repeating pattern, and the decoder copies one pixel at a time so it reads
 * what it has just written.
 */
static uint32_t match_at(
    const uint32_t* pixels, size_t at, size_t back, size_t limit
) {
    uint32_t length = 0;

    while (length < limit &&
           pixels[at - back + length] == pixels[at + length]) {
        length++;
    }

    return length;
}

/**
 * Scores a candidate copy in bits saved.
 *
 * The longest match is not the best one, and this is why: a copy pays for its
 * distance as well as its length, and the format's distance codes are not
 * uniform. A distance of one pixel or of exactly one row is one of 120 short
 * codes with no extra bits; anything else costs its distance in extra bits, up
 * to eighteen of them. So a long match far away can be worth less than a short
 * one directly above, and picking by length alone measurably loses: taking the
 * hash chain's longest find rather than scoring it cost 5% on a photograph and
 * 18% on a line drawing, while on tiled content the chain is worth 32 times
 * the file. Both effects are the same rule seen from opposite ends.
 */
static int32_t match_gain(
    const WebpEncoder* enc, uint32_t length, uint32_t distance
) {
    uint32_t symbol;
    uint32_t length_bits;
    uint32_t extra;

    prefix_encode(length, &symbol, &length_bits, &extra);

    uint32_t distance_bits;
    prefix_encode(
        distance_to_code(enc, distance), &symbol, &distance_bits, &extra
    );

    // what a literal costs is the whole question, and it is not a constant: a
    // line drawing codes one in two bits and a photograph in twenty, so it is
    // measured from the image itself rather than fitted across images
    return (int32_t) (length * enc->literal_bits) -
           (int32_t) (length_bits + distance_bits + 16u);
}

static uint32_t find_match(
    const WebpEncoder* enc, const uint32_t* pixels, size_t at,
    uint32_t* distance
) {
    if (at + 1 >= enc->count) return 0;

    uint32_t best = 0;
    int32_t richest = 0;
    size_t limit = enc->count - at;

    if (limit > VP8L_MAX_MATCH) limit = VP8L_MAX_MATCH;

    /*
     * The row above and its two diagonal neighbors are tried outright rather
     * than left to the hash chain to find. A picture repeats vertically far
     * more than a byte stream does, and a flat region fills one hash bucket
     * with identical recent positions, so a bounded chain walk may never reach
     * back the one row that matters. On a line drawing this alone is worth a
     * third of the file.
     */
    size_t candidates[4] = {1, enc->width, enc->width - 1u, enc->width + 1u};

    for (uint32_t i = 0; i < 4; i++) {
        size_t back = candidates[i];

        if (back == 0 || back > at) continue;

        uint32_t length = match_at(pixels, at, back, limit);

        if (length < VP8L_MIN_MATCH) continue;

        int32_t gain = match_gain(enc, length, (uint32_t) back);

        if (gain > richest) {
            richest = gain;
            best = length;
            *distance = (uint32_t) back;
        }
    }

    if (!enc->use_chain) return best;

    uint32_t key = hash_pair(pixels + at);
    int32_t candidate = enc->head[key];

    for (uint32_t probe = 0; probe < VP8L_CHAIN && candidate >= 0; probe++) {
        size_t back = at - (size_t) candidate;

        if (back == 0 || back > VP8L_WINDOW) break;

        uint32_t length = match_at(pixels, at, back, limit);

        if (length >= VP8L_FAR_MATCH) {
            int32_t gain = match_gain(enc, length, (uint32_t) back);

            if (gain > richest) {
                richest = gain;
                best = length;
                *distance = (uint32_t) back;
            }
        }

        candidate = enc->chain[candidate];
    }

    return best;
}

/**
 * Walks the image once, either counting symbols or writing them.
 *
 * The two passes have to agree exactly, so neither may look at anything the
 * other does not: the match search and the color cache both depend only on
 * the pixels, so running the walk twice costs time and produces the same
 * tokens rather than needing a token list, which for a photograph would be
 * larger than the image.
 */
static void walk_pixels(WebpEncoder* enc, const uint32_t* pixels) {
    // the buffer is allocated once and shared by every stream, so whether a
    // cache is in use is the width rather than the pointer: a shift of thirty
    // two is undefined, and a zero width one would index past the end
    uint32_t* cache = enc->cache_bits ? enc->cache : 0;
    uint32_t shift = enc->cache_bits ? 32u - enc->cache_bits : 0u;

    if (cache) {
        tiny_memset(cache, 0, ((size_t) 1u << enc->cache_bits) * 4u);
    }

    for (uint32_t i = 0; i < (1u << VP8L_HASH_BITS); i++) {
        enc->head[i] = -1;
    }

    size_t at = 0;

    while (at < enc->count) {
        uint32_t distance = 0;
        uint32_t length = at > 0 ? find_match(enc, pixels, at, &distance) : 0;

        if (length >= VP8L_MIN_MATCH) {
            uint32_t symbol;
            uint32_t bits;
            uint32_t extra;

            prefix_encode(length, &symbol, &bits, &extra);
            count_or_write(enc, &enc->green, VP8L_LITERALS + symbol);
            put_extra(enc, extra, bits);

            uint32_t code = distance_to_code(enc, distance);

            prefix_encode(code, &symbol, &bits, &extra);
            count_or_write(enc, &enc->distance, symbol);
            put_extra(enc, extra, bits);

            for (uint32_t i = 0; i < length; i++) {
                if (cache) {
                    cache[(pixels[at + i] * 0x1E35A7BDu) >> shift] =
                        pixels[at + i];
                }

                if (at + i + 1 < enc->count) {
                    uint32_t key = hash_pair(pixels + at + i);

                    enc->chain[at + i] = enc->head[key];
                    enc->head[key] = (int32_t) (at + i);
                }
            }

            at += length;
            continue;
        }

        uint32_t pixel = pixels[at];
        uint32_t slot = cache ? (pixel * 0x1E35A7BDu) >> shift : 0u;

        if (cache && cache[slot] == pixel) {
            count_or_write(
                enc, &enc->green, VP8L_LITERALS + VP8L_LENGTHS + slot
            );
        }
        else {
            count_or_write(enc, &enc->green, (pixel >> 8) & 0xFFu);
            count_or_write(enc, &enc->red, (pixel >> 16) & 0xFFu);
            count_or_write(enc, &enc->blue, pixel & 0xFFu);
            count_or_write(enc, &enc->alpha, pixel >> 24);
        }

        if (cache) cache[slot] = pixel;

        if (at + 1 < enc->count) {
            uint32_t key = hash_pair(pixels + at);

            enc->chain[at] = enc->head[key];
            enc->head[key] = (int32_t) at;
        }

        at++;
    }
}

/** The order the nineteen code length symbols are written in. */
static uint32_t used_symbols(const uint8_t* lengths, uint32_t count) {
    uint32_t used = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] > 0) used++;
    }

    return used;
}

/**
 * Writes one alphabet's code lengths.
 *
 * An alphabet down to one or two symbols gets the format's short form, which
 * matters more than it sounds: an opaque image's alpha channel has exactly one
 * value, and naming it outright costs eleven bits against the hundred or so
 * bytes a full length table would.
 */
static void write_huffman_code(
    TinyBitWriter* out, const uint8_t* lengths, uint32_t count
) {
    uint32_t used = used_symbols(lengths, count);

    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t seen = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] == 0) continue;

        if (seen++ == 0) {
            first = i;
        }
        else if (seen == 2) {
            second = i;
        }
    }

    // the short form names a symbol in eight bits, so it cannot express one
    // past 255. The green alphabet's length and cache symbols all are, and a
    // flat image uses exactly two symbols of which one is a length
    if (used <= 2 && first < VP8L_LITERALS && second < VP8L_LITERALS) {
        tiny_bitwriter_lsb(out, 1, 1);
        tiny_bitwriter_lsb(out, used > 1 ? 1u : 0u, 1);

        // the first symbol fits in one bit when it is 0 or 1, which is the
        // common case for a channel that never varies
        if (first < 2) {
            tiny_bitwriter_lsb(out, 0, 1);
            tiny_bitwriter_lsb(out, first, 1);
        }
        else {
            tiny_bitwriter_lsb(out, 1, 1);
            tiny_bitwriter_lsb(out, first, 8);
        }

        if (used > 1) tiny_bitwriter_lsb(out, second, 8);

        return;
    }

    tiny_bitwriter_lsb(out, 0, 1);

    /*
     * The lengths are themselves coded, with three symbols that repeat a run:
     * one repeating the last non-zero length, and two repeating a gap. A
     * sparse alphabet is mostly gaps, which is where most of the saving is,
     * and a dense one has long stretches at one length, which is the rest.
     */
    // one entry per symbol at worst, since a run only ever shortens the list,
    // and the largest alphabet is the green one with a full color cache
    uint8_t
        symbols[VP8L_LITERALS + VP8L_LENGTHS + (1u << VP8L_ENCODE_CACHE_BITS)];
    uint8_t
        extras[VP8L_LITERALS + VP8L_LENGTHS + (1u << VP8L_ENCODE_CACHE_BITS)];
    uint32_t total = 0;
    uint32_t previous = 8;
    uint32_t i = 0;

    while (i < count) {
        uint8_t value = lengths[i];
        uint32_t run = 1;

        while (i + run < count && lengths[i + run] == value) {
            run++;
        }

        i += run;

        if (value == 0) {
            while (run > 0) {
                // a gap of one or two costs less written out than named
                if (run < 3) {
                    symbols[total] = 0;
                    extras[total++] = 0;
                    run--;
                    continue;
                }

                uint32_t take = run > 138 ? 138u : run;

                if (take <= 10) {
                    symbols[total] = 17;
                    extras[total++] = (uint8_t) (take - 3u);
                }
                else {
                    symbols[total] = 18;
                    extras[total++] = (uint8_t) (take - 11u);
                }

                run -= take;
            }

            continue;
        }

        // the repeat symbol names whatever the last non-zero length was, so
        // the first of a run is written outright unless that already matches
        if (value != previous) {
            symbols[total] = value;
            extras[total++] = 0;
            run--;
        }

        previous = value;

        while (run > 0) {
            if (run < 3) {
                symbols[total] = value;
                extras[total++] = 0;
                run--;
                continue;
            }

            uint32_t take = run > 6 ? 6u : run;

            symbols[total] = 16;
            extras[total++] = (uint8_t) (take - 3u);

            run -= take;
        }
    }

    uint32_t histogram[VP8L_CODE_LENGTHS];
    tiny_memset(histogram, 0, sizeof(histogram));

    for (uint32_t k = 0; k < total; k++) {
        histogram[symbols[k]]++;
    }

    uint8_t code_lengths[VP8L_CODE_LENGTHS];

    if (tiny_huffman_lengths(histogram, VP8L_CODE_LENGTHS, 7, code_lengths) !=
        TINYIMG_OK) {
        tiny_memset(code_lengths, 0, sizeof(code_lengths));
    }

    WebpCode code_codes[VP8L_CODE_LENGTHS];
    codes_from_lengths(code_codes, code_lengths, VP8L_CODE_LENGTHS);

    uint32_t present = VP8L_CODE_LENGTHS;
    while (present > 4 && code_lengths[length_order[present - 1]] == 0) {
        present--;
    }

    tiny_bitwriter_lsb(out, present - 4u, 4);

    for (uint32_t k = 0; k < present; k++) {
        tiny_bitwriter_lsb(out, code_lengths[length_order[k]], 3);
    }

    // no count of how many symbols carry a length, so the decoder reads them
    // all
    tiny_bitwriter_lsb(out, 0, 1);

    for (uint32_t k = 0; k < total; k++) {
        tiny_bitwriter_lsb(
            out, code_codes[symbols[k]].code, code_codes[symbols[k]].length
        );

        if (symbols[k] == 16) tiny_bitwriter_lsb(out, extras[k], 2);
        if (symbols[k] == 17) tiny_bitwriter_lsb(out, extras[k], 3);
        if (symbols[k] == 18) tiny_bitwriter_lsb(out, extras[k], 7);
    }
}

static int build_alphabet(WebpAlphabet* alphabet, uint32_t limit) {
    uint8_t* lengths = alphabet->lengths;

    int result =
        tiny_huffman_lengths(alphabet->freq, alphabet->count, limit, lengths);
    if (result != TINYIMG_OK) return result;

    // a code has to name something, so an alphabet nothing used still gets its
    // first symbol a length
    uint32_t used = used_symbols(lengths, alphabet->count);

    if (used == 0) {
        lengths[0] = 1;
        used = 1;
    }

    codes_from_lengths(alphabet->codes, lengths, alphabet->count);

    alphabet->single = -1;

    if (used == 1) {
        for (uint32_t i = 0; i < alphabet->count; i++) {
            if (lengths[i] == 0) continue;

            // the length stays as it is for the table that gets written, which
            // names the symbol outright; only the emission drops to no bits
            alphabet->single = (int32_t) i;
            alphabet->codes[i].length = 0;
            alphabet->codes[i].code = 0;
            break;
        }
    }

    return TINYIMG_OK;
}

/**
 * Encodes one image stream, which is the unit the format nests.
 *
 * @param enc The encoder, holding the scratch both passes share.
 * @param pixels The plane to code.
 * @param width Plane width.
 * @param height Plane height.
 * @param level0 Non-zero for the picture itself, which alone carries the meta
 * Huffman flag the decoder only reads there.
 * @param out The bitstream.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int encode_image_stream(
    WebpEncoder* enc, const uint32_t* pixels, uint32_t width, uint32_t height,
    int level0, TinyBitWriter* out
) {
    enc->width = width;
    enc->count = (size_t) width * height;

    uint32_t cache = enc->cache_bits;

    enc->green.count =
        VP8L_LITERALS + VP8L_LENGTHS + (cache ? 1u << cache : 0u);
    enc->red.count = VP8L_LITERALS;
    enc->blue.count = VP8L_LITERALS;
    enc->alpha.count = VP8L_LITERALS;
    enc->distance.count = VP8L_DISTANCES;

    WebpAlphabet* all[VP8L_CODES_PER_GROUP] = {
        &enc->green, &enc->red, &enc->blue, &enc->alpha, &enc->distance
    };

    /*
     * Three walks, not two. The first has no cost model to judge a distant
     * copy with, so it takes only the near ones and its statistics are what a
     * literal really costs in this image. The second takes that number and
     * looks properly. The third writes what the second decided.
     */
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < VP8L_CODES_PER_GROUP; i++) {
            tiny_memset(all[i]->freq, 0, all[i]->count * sizeof(uint32_t));
        }

        enc->out = 0;
        enc->cost = 0;
        enc->use_chain = (int) pass;

        walk_pixels(enc, pixels);

        for (uint32_t i = 0; i < VP8L_CODES_PER_GROUP; i++) {
            int result = build_alphabet(all[i], VP8L_MAX_BITS);
            if (result != TINYIMG_OK) return result;
        }

        if (pass > 0) break;

        uint64_t bits = 0;
        uint64_t literals = 0;

        for (uint32_t s = 0; s < VP8L_LITERALS; s++) {
            literals += enc->green.freq[s];
            bits += (uint64_t) enc->green.freq[s] * enc->green.lengths[s];
            bits += (uint64_t) enc->red.freq[s] * enc->red.lengths[s];
            bits += (uint64_t) enc->blue.freq[s] * enc->blue.lengths[s];
            bits += (uint64_t) enc->alpha.freq[s] * enc->alpha.lengths[s];
        }

        // an image coded entirely as copies has no literal to measure, and
        // eight bits is then as good a guess as any since nothing will use it
        enc->literal_bits = literals ? (uint32_t) (bits / literals) : 8u;

        if (enc->literal_bits < 1) enc->literal_bits = 1;
    }

    tiny_bitwriter_lsb(out, cache ? 1u : 0u, 1);
    if (cache) tiny_bitwriter_lsb(out, cache, 4);

    if (level0) tiny_bitwriter_lsb(out, 0, 1);

    for (uint32_t i = 0; i < VP8L_CODES_PER_GROUP; i++) {
        write_huffman_code(out, all[i]->lengths, all[i]->count);
    }

    enc->out = out;
    walk_pixels(enc, pixels);

    return TINYIMG_OK;
}

static int alloc_alphabet(WebpAlphabet* alphabet, uint32_t count) {
    alphabet->count = count;
    alphabet->freq = tiny_arena_alloc(count * sizeof(uint32_t), 4);
    alphabet->lengths = tiny_arena_alloc(count, 0);
    alphabet->codes = tiny_arena_alloc(count * sizeof(WebpCode), 2);

    return alphabet->freq && alphabet->lengths && alphabet->codes
               ? TINYIMG_OK
               : TINYIMG_ERR_MEMORY;
}

/**
 * Chooses a predictor per tile and writes the residual plane over the source.
 *
 * The prediction reads the source pixels rather than the residuals, so every
 * residual can be computed in one pass with no feedback: the decoder's
 * reconstruction reproduces the source exactly, so what it predicts from is
 * what this predicts from.
 */
static void apply_predictor(
    uint32_t* pixels, uint32_t width, uint32_t height, uint32_t* modes
) {
    uint32_t across = subsample_size(width, VP8L_TILE_BITS);
    uint32_t down = subsample_size(height, VP8L_TILE_BITS);

    for (uint32_t ty = 0; ty < down; ty++) {
        for (uint32_t tx = 0; tx < across; tx++) {
            uint32_t left = tx << VP8L_TILE_BITS;
            uint32_t top = ty << VP8L_TILE_BITS;
            uint32_t right = left + (1u << VP8L_TILE_BITS);
            uint32_t bottom = top + (1u << VP8L_TILE_BITS);

            if (right > width) right = width;
            if (bottom > height) bottom = height;

            uint32_t best = 0;
            uint64_t cheapest = 0;

            for (uint32_t mode = 0; mode < 14; mode++) {
                uint64_t total = 0;

                for (uint32_t y = top; y < bottom; y++) {
                    // the first row and column are predicted by a fixed rule,
                    // so they say nothing about which mode suits the tile
                    if (y == 0) continue;

                    const uint32_t* row = pixels + (size_t) y * width;
                    const uint32_t* above = row - width;

                    for (uint32_t x = left < 1 ? 1 : left; x < right; x++) {
                        uint32_t guess = predict(mode, row[x - 1], above + x);

                        for (uint32_t shift = 0; shift < 32; shift += 8) {
                            uint32_t d = ((row[x] >> shift) & 0xFFu) -
                                         ((guess >> shift) & 0xFFu);

                            d &= 0xFFu;
                            total += d < 128 ? d : 256u - d;
                        }
                    }
                }

                if (mode == 0 || total < cheapest) {
                    cheapest = total;
                    best = mode;
                }
            }

            modes[(size_t) ty * across + tx] = 0xFF000000u | (best << 8);
        }
    }

    // backward, because a residual must be computed from source neighbors and
    // writing forward would overwrite them first
    for (uint32_t y = height; y-- > 0;) {
        uint32_t* row = pixels + (size_t) y * width;
        const uint32_t* above = row - width;
        const uint32_t* block = modes + (size_t) (y >> VP8L_TILE_BITS) * across;

        for (uint32_t x = width; x-- > 0;) {
            uint32_t guess;

            if (y == 0) {
                guess = x == 0 ? 0xFF000000u : row[x - 1];
            }
            else if (x == 0) {
                guess = above[0];
            }
            else {
                guess = predict(
                    (block[x >> VP8L_TILE_BITS] >> 8) & 0x0Fu, row[x - 1],
                    above + x
                );
            }

            row[x] = subtract_pixels(row[x], guess);
        }
    }
}

/** Subtracts green from red and blue, which the inverse undoes. */
static void apply_subtract_green(uint32_t* pixels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i];
        uint32_t green = (pixel >> 8) & 0xFFu;

        uint32_t red = (((pixel >> 16) & 0xFFu) - green) & 0xFFu;
        uint32_t blue = ((pixel & 0xFFu) - green) & 0xFFu;

        pixels[i] = (pixel & 0xFF00FF00u) | (red << 16) | blue;
    }
}

/**
 * Collects the image's colors, giving up past 256.
 *
 * A palette is what makes the format lossless on flat artwork rather than
 * merely correct on it, so this runs first and the rest of the pipeline is
 * chosen by whether it succeeded.
 */
static uint32_t collect_palette(
    const uint32_t* pixels, size_t count, uint32_t* palette
) {
    uint32_t found = 0;

    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i];
        uint32_t at = 0;

        while (at < found && palette[at] != pixel) {
            at++;
        }

        if (at < found) continue;
        if (found == 256) return 0;

        palette[found++] = pixel;
    }

    // ascending, so the delta coding the format applies to the palette has
    // small differences to code
    for (uint32_t i = 1; i < found; i++) {
        uint32_t value = palette[i];
        uint32_t at = i;

        while (at > 0 && palette[at - 1] > value) {
            palette[at] = palette[at - 1];
            at--;
        }

        palette[at] = value;
    }

    return found;
}

static int encode_lossless(const TinyImage* image, TinyWriter* writer) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t width = image->width;
    uint32_t height = image->height;
    size_t count = (size_t) width * height;

    uint32_t* pixels = tiny_arena_alloc(count * sizeof(uint32_t), 4);
    uint32_t* palette = tiny_arena_alloc(256 * sizeof(uint32_t), 4);

    WebpEncoder* enc = tiny_arena_alloc(sizeof(WebpEncoder), 8);

    if (!pixels || !palette || !enc) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(enc, 0, sizeof(*enc));

    int opaque = 1;

    for (size_t i = 0; i < count; i++) {
        uint8_t rgba[4];

        tiny_pixel_convert(
            rgba, 4, image->data + i * image->channels, image->channels
        );

        pixels[i] = ((uint32_t) rgba[3] << 24) | ((uint32_t) rgba[0] << 16) |
                    ((uint32_t) rgba[1] << 8) | rgba[2];

        if (rgba[3] != 255) opaque = 0;
    }

    uint32_t colors = collect_palette(pixels, count, palette);

    uint8_t planes[VP8L_PLANE_CODES];
    build_planes(planes);

    tiny_memset(enc->inverse, 0xFF, sizeof(enc->inverse));

    for (uint32_t i = 0; i < VP8L_PLANE_CODES; i++) {
        enc->inverse[planes[i]] = (uint8_t) i;
    }

    enc->head = tiny_arena_alloc((size_t) (1u << VP8L_HASH_BITS) * 4u, 4);
    enc->chain = tiny_arena_alloc(count * sizeof(int32_t), 4);
    enc->cache =
        tiny_arena_alloc(((size_t) 1u << VP8L_ENCODE_CACHE_BITS) * 4u, 4);

    int ok = enc->head && enc->chain && enc->cache;

    ok = ok && alloc_alphabet(
                   &enc->green,
                   VP8L_LITERALS + VP8L_LENGTHS + (1u << VP8L_ENCODE_CACHE_BITS)
               ) == TINYIMG_OK;
    ok = ok && alloc_alphabet(&enc->red, VP8L_LITERALS) == TINYIMG_OK;
    ok = ok && alloc_alphabet(&enc->blue, VP8L_LITERALS) == TINYIMG_OK;
    ok = ok && alloc_alphabet(&enc->alpha, VP8L_LITERALS) == TINYIMG_OK;
    ok = ok && alloc_alphabet(&enc->distance, VP8L_DISTANCES) == TINYIMG_OK;

    TinyWriter payload;

    if (!ok || tiny_writer_init(&payload, count / 2u + 1024u) != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    TinyBitWriter bits;
    tiny_bitwriter_init(&bits, &payload);

    tiny_bitwriter_lsb(&bits, 0x2F, 8);
    tiny_bitwriter_lsb(&bits, width - 1u, 14);
    tiny_bitwriter_lsb(&bits, height - 1u, 14);
    tiny_bitwriter_lsb(&bits, opaque ? 0u : 1u, 1);
    tiny_bitwriter_lsb(&bits, 0, 3);

    uint32_t coded_width = width;
    int result = TINYIMG_OK;

    if (colors > 0) {
        /*
         * A palette, and nothing else. The indices go in the green channel
         * because that is where the inverse reads them, and with sixteen
         * colors or fewer several share a byte, which narrows the coded rows.
         */
        uint32_t tile = colors > 16  ? 0u
                        : colors > 4 ? 1u
                        : colors > 2 ? 2u
                                     : 3u;

        coded_width = subsample_size(width, tile);

        uint32_t* deltas = tiny_arena_alloc(colors * sizeof(uint32_t), 4);
        uint32_t* packed =
            tiny_arena_alloc((size_t) coded_width * height * 4u, 4);

        if (!deltas || !packed) {
            tiny_writer_free(&payload);
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        // the format stores the palette as differences between neighboring
        // entries, byte by byte across all four channels
        uint8_t* delta_bytes = (uint8_t*) deltas;
        const uint8_t* entry_bytes = (const uint8_t*) palette;

        deltas[0] = palette[0];

        for (size_t i = 4; i < (size_t) colors * 4; i++) {
            delta_bytes[i] = (uint8_t) (entry_bytes[i] - entry_bytes[i - 4]);
        }

        uint32_t per_pixel = 8u >> tile;
        uint32_t per_byte = 1u << tile;

        tiny_memset(packed, 0, (size_t) coded_width * height * 4u);

        for (uint32_t y = 0; y < height; y++) {
            const uint32_t* row = pixels + (size_t) y * width;
            uint32_t* dest = packed + (size_t) y * coded_width;

            for (uint32_t x = 0; x < width; x++) {
                uint32_t index = 0;

                while (index + 1 < colors && palette[index] != row[x]) {
                    index++;
                }

                dest[x >> tile] |= index
                                   << (8u + (x & (per_byte - 1u)) * per_pixel);
            }
        }

        tiny_bitwriter_lsb(&bits, 1, 1);
        tiny_bitwriter_lsb(&bits, VP8L_COLOR_INDEX, 2);
        tiny_bitwriter_lsb(&bits, colors - 1u, 8);

        enc->cache_bits = 0;
        result = encode_image_stream(enc, deltas, colors, 1, 0, &bits);

        tiny_bitwriter_lsb(&bits, 0, 1);

        if (result == TINYIMG_OK) {
            enc->cache_bits = 0;
            result =
                encode_image_stream(enc, packed, coded_width, height, 1, &bits);
        }
    }
    else {
        uint32_t across = subsample_size(width, VP8L_TILE_BITS);
        uint32_t down = subsample_size(height, VP8L_TILE_BITS);

        uint32_t* modes = tiny_arena_alloc((size_t) across * down * 4u, 4);

        if (!modes) {
            tiny_writer_free(&payload);
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        apply_subtract_green(pixels, count);
        apply_predictor(pixels, width, height, modes);

        /*
         * Written in the order they were applied, because the decoder undoes
         * them in reverse: subtract green first here means its inverse runs
         * last there, which is where it belongs.
         */
        tiny_bitwriter_lsb(&bits, 1, 1);
        tiny_bitwriter_lsb(&bits, VP8L_SUBTRACT_GREEN, 2);

        tiny_bitwriter_lsb(&bits, 1, 1);
        tiny_bitwriter_lsb(&bits, VP8L_PREDICTOR, 2);
        tiny_bitwriter_lsb(&bits, VP8L_TILE_BITS - 2u, 3);

        enc->cache_bits = 0;
        result = encode_image_stream(enc, modes, across, down, 0, &bits);

        tiny_bitwriter_lsb(&bits, 0, 1);

        if (result == TINYIMG_OK) {
            enc->cache_bits = VP8L_ENCODE_CACHE_BITS;
            result = encode_image_stream(enc, pixels, width, height, 1, &bits);
        }
    }

    tiny_bitwriter_flush_lsb(&bits);

    if (result == TINYIMG_OK) result = payload.error;

    if (result != TINYIMG_OK) {
        tiny_writer_free(&payload);
        tiny_arena_release(&mark);
        return result;
    }

    size_t chunk = payload.size;

    tiny_writer_write(writer, "RIFF", 4);
    tiny_writer_le32(writer, (uint32_t) (12u + chunk + (chunk & 1u)));
    tiny_writer_write(writer, "WEBP", 4);
    tiny_writer_write(writer, "VP8L", 4);
    tiny_writer_le32(writer, (uint32_t) chunk);
    tiny_writer_write(writer, payload.data, chunk);

    // every chunk is padded to an even length
    if (chunk & 1u) tiny_writer_u8(writer, 0);

    tiny_writer_free(&payload);
    tiny_arena_release(&mark);

    return writer->error;
}

#pragma endregion

#pragma region lossy encode

/** The largest level one coefficient token can name. */
#define VP8_MAX_LEVEL 2047

/**
 * Writes bools into a byte buffer.
 *
 * The mirror of the decoder, and it has one property worth knowing about: a
 * carry out of the interval can reach back into bytes already written, which is
 * why this writes into a plain buffer it owns rather than into a growing sink
 * whose earlier bytes may have moved.
 */
typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t size;
    uint32_t range;
    uint32_t bottom;
    int32_t count;
    int overflow;
} Vp8Writer;

static void writer_init(Vp8Writer* out, uint8_t* data, size_t capacity) {
    out->data = data;
    out->capacity = capacity;
    out->size = 0;
    out->range = 255;
    out->bottom = 0;
    out->count = 24;
    out->overflow = 0;
}

static void writer_byte(Vp8Writer* out, uint8_t value) {
    if (out->size >= out->capacity) {
        out->overflow = 1;
        return;
    }

    out->data[out->size++] = value;
}

/** Propagates a carry backward through what has already been written. */
static void writer_carry(Vp8Writer* out) {
    size_t at = out->size;

    while (at > 0 && out->data[at - 1] == 255) {
        out->data[--at] = 0;
    }

    if (at > 0) out->data[at - 1]++;
}

static void write_bool(Vp8Writer* out, uint32_t probability, int value) {
    uint32_t split = 1u + (((out->range - 1u) * probability) >> 8);

    if (value) {
        out->bottom += split;
        out->range -= split;
    }
    else {
        out->range = split;
    }

    while (out->range < 128u) {
        out->range <<= 1;

        if (out->bottom & 0x80000000u) writer_carry(out);

        out->bottom <<= 1;

        if (--out->count == 0) {
            writer_byte(out, (uint8_t) (out->bottom >> 24));

            out->bottom &= (1u << 24) - 1u;
            out->count = 8;
        }
    }
}

static void write_uint(Vp8Writer* out, uint32_t value, uint32_t bits) {
    while (bits-- > 0) {
        write_bool(out, 128, (int) ((value >> bits) & 1u));
    }
}

static void writer_flush(Vp8Writer* out) {
    int32_t c = out->count;
    uint32_t v = out->bottom;

    if (v & (1u << (32 - c))) writer_carry(out);

    v <<= (uint32_t) (c & 7);
    c >>= 3;

    while (--c >= 0) {
        v <<= 8;
    }

    for (int32_t i = 0; i < 4; i++) {
        writer_byte(out, (uint8_t) (v >> 24));
        v <<= 8;
    }
}

#pragma endregion

#pragma region lossy forward transforms

/**
 * The forward transform, with the format's own constants.
 *
 * @param source The pixels being coded.
 * @param prediction What was predicted for them.
 * @param out Sixteen coefficients in raster order.
 */
static void forward_dct(
    const uint8_t* source, const uint8_t* prediction, int16_t* out
) {
    int32_t tmp[16];

    for (uint32_t i = 0; i < 4; i++) {
        int32_t d0 = (int32_t) source[0] - prediction[0];
        int32_t d1 = (int32_t) source[1] - prediction[1];
        int32_t d2 = (int32_t) source[2] - prediction[2];
        int32_t d3 = (int32_t) source[3] - prediction[3];

        int32_t a0 = d0 + d3;
        int32_t a1 = d1 + d2;
        int32_t a2 = d1 - d2;
        int32_t a3 = d0 - d3;

        tmp[0 + i * 4] = (a0 + a1) * 8;
        tmp[1 + i * 4] = (a2 * 2217 + a3 * 5352 + 1812) >> 9;
        tmp[2 + i * 4] = (a0 - a1) * 8;
        tmp[3 + i * 4] = (a3 * 2217 - a2 * 5352 + 937) >> 9;

        source += VP8_BPS;
        prediction += VP8_BPS;
    }

    for (uint32_t i = 0; i < 4; i++) {
        int32_t a0 = tmp[0 + i] + tmp[12 + i];
        int32_t a1 = tmp[4 + i] + tmp[8 + i];
        int32_t a2 = tmp[4 + i] - tmp[8 + i];
        int32_t a3 = tmp[0 + i] - tmp[12 + i];

        out[0 + i] = (int16_t) ((a0 + a1 + 7) >> 4);
        out[8 + i] = (int16_t) ((a0 - a1 + 7) >> 4);

        // the correction on a3 is the format's, not a rounding choice: without
        // it the transform is not the exact inverse of the one that undoes it
        out[4 + i] = (int16_t) (((a2 * 2217 + a3 * 5352 + 12000) >> 16) +
                                (a3 != 0 ? 1 : 0));
        out[12 + i] = (int16_t) ((a3 * 2217 - a2 * 5352 + 51000) >> 16);
    }
}

/** The forward Walsh-Hadamard transform over the sixteen luma DC terms. */
static void forward_wht(const int16_t* in, int16_t* out) {
    int32_t tmp[16];

    for (uint32_t i = 0; i < 4; i++) {
        int32_t a0 = in[0 * 16] + in[2 * 16];
        int32_t a1 = in[1 * 16] + in[3 * 16];
        int32_t a2 = in[1 * 16] - in[3 * 16];
        int32_t a3 = in[0 * 16] - in[2 * 16];

        tmp[0 + i * 4] = a0 + a1;
        tmp[1 + i * 4] = a3 + a2;
        tmp[2 + i * 4] = a3 - a2;
        tmp[3 + i * 4] = a0 - a1;

        in += 64;
    }

    for (uint32_t i = 0; i < 4; i++) {
        int32_t a0 = tmp[0 + i] + tmp[8 + i];
        int32_t a1 = tmp[4 + i] + tmp[12 + i];
        int32_t a2 = tmp[4 + i] - tmp[12 + i];
        int32_t a3 = tmp[0 + i] - tmp[8 + i];

        out[0 + i] = (int16_t) ((a0 + a1) >> 1);
        out[4 + i] = (int16_t) ((a3 + a2) >> 1);
        out[8 + i] = (int16_t) ((a3 - a2) >> 1);
        out[12 + i] = (int16_t) ((a0 - a1) >> 1);
    }
}

/**
 * Quantizes one block, leaving the dequantized values behind for the
 * reconstruction.
 *
 * The bias decides how readily a coefficient rounds up, and the two are
 * different on purpose: a DC term rounds normally, and an AC term is biased
 * toward zero so that quantization noise is dropped rather than coded.
 *
 * @param coeffs Coefficients in raster order, replaced by the dequantized
 * values.
 * @param levels Receives the levels in zigzag order.
 * @param quant The DC and AC multipliers.
 * @param first Where to start, one for a luma block whose DC is coded
 * elsewhere.
 * @return int32_t The last position holding a level, or `first - 1` for an
 * empty block.
 */
static int32_t quantize_block(
    int16_t* coeffs, int16_t* levels, const uint16_t* quant, uint32_t first
) {
    int32_t last = (int32_t) first - 1;

    for (uint32_t n = first; n < 16; n++) {
        uint32_t at = vp8_zigzag[n];
        int32_t value = coeffs[at];
        int32_t sign = value < 0;
        uint32_t magnitude = (uint32_t) (sign ? -value : value);
        uint32_t step = quant[n > 0 ? 1 : 0];

        uint32_t bias = n > 0 ? step * 2u / 5u : step / 2u;
        int32_t level = (int32_t) ((magnitude + bias) / step);

        if (level > VP8_MAX_LEVEL) level = VP8_MAX_LEVEL;

        levels[n] = (int16_t) (sign ? -level : level);
        coeffs[at] = (int16_t) (levels[n] * (int32_t) step);

        if (level != 0) last = (int32_t) n;
    }

    for (uint32_t n = 0; n < first; n++) {
        levels[n] = 0;
    }

    return last;
}

#pragma endregion

#pragma region lossy tokens out

/** Writes a magnitude past four, mirroring the decoder's category tree. */
static void write_large_value(
    Vp8Writer* out, const uint8_t* probabilities, uint32_t magnitude
) {
    if (magnitude < 5) {
        write_bool(out, probabilities[3], 0);

        if (magnitude == 2) {
            write_bool(out, probabilities[4], 0);
            return;
        }

        write_bool(out, probabilities[4], 1);
        write_bool(out, probabilities[5], magnitude == 4);
        return;
    }

    write_bool(out, probabilities[3], 1);

    if (magnitude <= 10) {
        write_bool(out, probabilities[6], 0);

        if (magnitude <= 6) {
            write_bool(out, probabilities[7], 0);
            write_bool(out, 159, (int) (magnitude - 5u));
            return;
        }

        write_bool(out, probabilities[7], 1);

        uint32_t offset = magnitude - 7u;

        write_bool(out, 165, (int) ((offset >> 1) & 1u));
        write_bool(out, 145, (int) (offset & 1u));
        return;
    }

    write_bool(out, probabilities[6], 1);

    uint32_t category = magnitude <= 18   ? 0u
                        : magnitude <= 34 ? 1u
                        : magnitude <= 66 ? 2u
                                          : 3u;

    write_bool(out, probabilities[8], (int) (category >> 1));
    write_bool(out, probabilities[9 + (category >> 1)], (int) (category & 1u));

    uint32_t bits = 0;
    while (vp8_cat_probs[category][bits] != 0) {
        bits++;
    }

    uint32_t residual = magnitude - (3u + (8u << category));

    for (uint32_t i = 0; i < bits; i++) {
        write_bool(
            out, vp8_cat_probs[category][i],
            (int) ((residual >> (bits - 1u - i)) & 1u)
        );
    }
}

/**
 * Writes one block's coefficients.
 *
 * Mirrors the decoder's loop step for step rather than being derived
 * independently, because the two have to agree on where the end of block
 * decision is read and where it is not: after a run of zeros it is skipped,
 * and an encoder that wrote one there would desynchronize the stream.
 */
static void write_coefficients(
    Vp8Writer* out, const uint8_t probabilities[8][3][11], uint32_t context,
    const int16_t* levels, uint32_t first, int32_t last
) {
    uint32_t at = first;
    const uint8_t* probability = probabilities[vp8_bands[at]][context];

    while (at < 16) {
        if ((int32_t) at > last) {
            write_bool(out, probability[0], 0);
            return;
        }

        write_bool(out, probability[0], 1);

        while (levels[at] == 0) {
            write_bool(out, probability[1], 0);
            at++;
            probability = probabilities[vp8_bands[at]][0];
        }

        write_bool(out, probability[1], 1);

        int32_t value = levels[at];
        uint32_t magnitude = (uint32_t) (value < 0 ? -value : value);
        uint32_t next;

        if (magnitude == 1) {
            write_bool(out, probability[2], 0);
            next = 1;
        }
        else {
            write_bool(out, probability[2], 1);
            write_large_value(out, probability, magnitude);
            next = 2;
        }

        write_bool(out, 128, value < 0);

        if (++at == 16) return;

        probability = probabilities[vp8_bands[at]][next];
    }
}

#pragma endregion

#pragma region lossy encode

/** One macroblock's decisions, kept so the first partition can be written
 * after the frame's skip statistics are known. */
typedef struct {
    uint8_t modes[16];
    uint8_t uv_mode;
    uint8_t is_i4x4;
    uint8_t skip;
} Vp8Record;

/**
 * @brief The decisions that reach one subblock mode, in reading order.
 *
 * A decoder walks the tree downward and learns the symbol at a leaf; an
 * encoder knows the symbol and needs the path. Rather than searching the tree
 * upward from the leaf, which has to disambiguate a leaf value of zero from
 * the root's index, the whole tree is walked once and every path recorded.
 */
typedef struct {
    uint8_t nodes[10];
    uint8_t bits[10];
    uint8_t length;
} Vp8ModePath;

static void build_mode_paths(
    Vp8ModePath* paths, int32_t at, uint8_t depth, uint8_t* nodes, uint8_t* bits
) {
    for (int32_t bit = 0; bit < 2; bit++) {
        int32_t child = vp8_bmode_tree[at + bit];

        nodes[depth] = (uint8_t) (at >> 1);
        bits[depth] = (uint8_t) bit;

        if (child > 0) {
            build_mode_paths(paths, child, (uint8_t) (depth + 1u), nodes, bits);
            continue;
        }

        // a leaf holds the negated symbol, and only the root sits at index
        // zero, so a child of zero is the symbol zero rather than the root
        Vp8ModePath* path = &paths[-child];

        path->length = (uint8_t) (depth + 1u);

        for (uint32_t i = 0; i <= depth; i++) {
            path->nodes[i] = nodes[i];
            path->bits[i] = bits[i];
        }
    }
}

typedef struct {
    Vp8Decoder* rec;

    /** The picture being coded, on the same grid as the reconstruction. */
    uint8_t* source_y;
    uint8_t* source_u;
    uint8_t* source_v;

    int16_t levels[25 * 16];
    int32_t last[25];

    /** How many bits of distortion one coded coefficient is worth. */
    uint32_t lambda;

    /**
     * How many of the ten 4x4 prediction modes to try per subblock.
     *
     * The search is 47% of this encoder, so this is the one number that decides
     * whether a request fits a small CPU budget.
     */
    uint32_t i4_modes;

    Vp8Record* records;
    Vp8ModePath paths[10];
} Vp8Encoder;

/**
 * Converts the image to the format's color space on a macroblock grid.
 *
 * The padding past the picture repeats its edge rather than being left black,
 * because the last macroblock of a row or column predicts from it and a hard
 * edge there costs bits for detail nobody sees.
 */
static void image_to_yuv(const TinyImage* image, Vp8Encoder* enc) {
    Vp8Decoder* rec = enc->rec;
    uint32_t width = rec->mb_w * 16u;
    uint32_t height = rec->mb_h * 16u;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = y < image->height ? y : image->height - 1u;
        uint8_t* row = enc->source_y + (size_t) y * rec->y_stride;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t sx = x < image->width ? x : image->width - 1u;
            uint8_t rgba[4];

            tiny_pixel_convert(
                rgba, 4,
                image->data +
                    ((size_t) sy * image->width + sx) * image->channels,
                image->channels
            );

            int32_t luma = 16839 * rgba[0] + 33059 * rgba[1] + 6420 * rgba[2];

            row[x] = (uint8_t) ((luma + 32768 + (16 << 16)) >> 16);
        }
    }

    for (uint32_t y = 0; y < height / 2u; y++) {
        uint8_t* row_u = enc->source_u + (size_t) y * rec->uv_stride;
        uint8_t* row_v = enc->source_v + (size_t) y * rec->uv_stride;

        for (uint32_t x = 0; x < width / 2u; x++) {
            int32_t r = 0;
            int32_t g = 0;
            int32_t b = 0;

            // chroma is one sample per two by two block, taken from their sum,
            // which is why the fixed point shift below carries two extra bits
            for (uint32_t dy = 0; dy < 2; dy++) {
                for (uint32_t dx = 0; dx < 2; dx++) {
                    uint32_t py = y * 2u + dy;
                    uint32_t px = x * 2u + dx;

                    if (py >= image->height) py = image->height - 1u;
                    if (px >= image->width) px = image->width - 1u;

                    uint8_t rgba[4];

                    tiny_pixel_convert(
                        rgba, 4,
                        image->data +
                            ((size_t) py * image->width + px) * image->channels,
                        image->channels
                    );

                    r += rgba[0];
                    g += rgba[1];
                    b += rgba[2];
                }
            }

            int32_t u = -9719 * r - 19081 * g + 28800 * b;
            int32_t v = 28800 * r - 24116 * g - 4684 * b;

            row_u[x] = tiny_clamp_u8((u + (32768 << 2) + (128 << 18)) >> 18);
            row_v[x] = tiny_clamp_u8((v + (32768 << 2) + (128 << 18)) >> 18);
        }
    }
}

static uint32_t block_error(
    const uint8_t* source, const uint8_t* other, uint32_t size
) {
    uint32_t total = 0;

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            int32_t d = (int32_t) source[(size_t) y * VP8_BPS + x] -
                        other[(size_t) y * VP8_BPS + x];

            total += (uint32_t) (d * d);
        }
    }

    return total;
}

/** Copies the source macroblock into a working area shaped like the
 * prediction's, so one stride serves both. */
static void load_source(
    Vp8Encoder* enc, uint32_t mb_x, uint32_t mb_y, uint8_t* y, uint8_t* u,
    uint8_t* v
) {
    const Vp8Decoder* rec = enc->rec;

    for (uint32_t i = 0; i < 16; i++) {
        tiny_memcpy(
            y + (size_t) i * VP8_BPS,
            enc->source_y + ((size_t) mb_y * 16u + i) * rec->y_stride +
                (size_t) mb_x * 16u,
            16
        );
    }

    for (uint32_t i = 0; i < 8; i++) {
        tiny_memcpy(
            u + (size_t) i * VP8_BPS,
            enc->source_u + ((size_t) mb_y * 8u + i) * rec->uv_stride +
                (size_t) mb_x * 8u,
            8
        );
        tiny_memcpy(
            v + (size_t) i * VP8_BPS,
            enc->source_v + ((size_t) mb_y * 8u + i) * rec->uv_stride +
                (size_t) mb_x * 8u,
            8
        );
    }
}

/**
 * Transforms, quantizes and reconstructs one 4x4 block in place.
 *
 * @return uint32_t How many coefficients were coded, which the mode decision
 * charges for.
 */
static uint32_t code_block(
    Vp8Encoder* enc, uint32_t block, const uint8_t* source, uint8_t* dest,
    const uint16_t* quant, uint32_t first
) {
    Vp8Decoder* rec = enc->rec;
    int16_t* coeffs = rec->coeffs + (size_t) block * 16;

    forward_dct(source, dest, coeffs);

    enc->last[block] =
        quantize_block(coeffs, enc->levels + (size_t) block * 16, quant, first);

    uint32_t coded = 0;

    for (uint32_t n = first; n < 16; n++) {
        if (enc->levels[block * 16 + n] != 0) coded++;
    }

    add_residual(rec, block, dest, enc->last[block] > 0);
    return coded;
}

/**
 * Codes a macroblock with whole block luma prediction.
 *
 * @return uint64_t The rate weighted cost, for comparison against the subblock
 * form.
 */
static uint64_t try_i16(
    Vp8Encoder* enc, uint32_t mb_x, uint32_t mb_y, const uint8_t* source,
    uint32_t* chosen
) {
    Vp8Decoder* rec = enc->rec;
    uint8_t* y = rec->work_y + VP8_BPS + 8;

    uint32_t best = VP8_DC_PRED;
    uint32_t cheapest = 0;

    for (uint32_t mode = 0; mode < 4; mode++) {
        predict_block(y, 16, dc_variant(mode, mb_x, mb_y));

        uint32_t error = block_error(source, y, 16);

        if (mode == 0 || error < cheapest) {
            cheapest = error;
            best = mode;
        }
    }

    *chosen = best;
    predict_block(y, 16, dc_variant(best, mb_x, mb_y));

    // every subblock's DC term is coded together through the second order
    // transform, so the luma blocks start at one
    int16_t dc[16];

    for (uint32_t n = 0; n < 16; n++) {
        forward_dct(
            source + vp8_scan[n], y + vp8_scan[n], rec->coeffs + (size_t) n * 16
        );
    }

    forward_wht(rec->coeffs, dc);

    for (uint32_t n = 0; n < 16; n++) {
        rec->coeffs[24 * 16 + n] = dc[n];
    }

    enc->last[24] = quantize_block(
        rec->coeffs + 24 * 16, enc->levels + 24 * 16,
        rec->quant[0][VP8_BLOCK_Y2], 0
    );

    uint32_t coded = 0;

    for (uint32_t n = 0; n < 16; n++) {
        if (enc->levels[24 * 16 + n] != 0) coded++;
    }

    inverse_wht(rec->coeffs + 24 * 16, rec->coeffs);

    for (uint32_t n = 0; n < 16; n++) {
        int16_t* coeffs = rec->coeffs + (size_t) n * 16;

        enc->last[n] = quantize_block(
            coeffs, enc->levels + (size_t) n * 16, rec->quant[0][VP8_BLOCK_Y1],
            1
        );

        for (uint32_t k = 1; k < 16; k++) {
            if (enc->levels[n * 16 + k] != 0) coded++;
        }

        add_residual(rec, n, y + vp8_scan[n], enc->last[n] > 0);
    }

    return block_error(source, y, 16) + (uint64_t) enc->lambda * coded;
}

/** Codes a macroblock with per subblock luma prediction. */
static uint64_t try_i4(Vp8Encoder* enc, const uint8_t* source, uint8_t* modes) {
    Vp8Decoder* rec = enc->rec;
    uint8_t* y = rec->work_y + VP8_BPS + 8;
    uint32_t coded = 0;

    for (uint32_t n = 0; n < 16; n++) {
        uint8_t* dest = y + vp8_scan[n];
        const uint8_t* here = source + vp8_scan[n];

        uint32_t best = 0;
        uint32_t cheapest = 0;

        /*
         * The first four modes are DC, TrueMotion, vertical and horizontal, and
         * the other six are the diagonals. Trying only the four is what
         * TINYIMG_EFFORT_FAST buys: this loop is 47% of the encoder, so the
         * search is where a CPU budget is won or lost. What it costs in
         * decibels and in bytes is measured in the tests rather than assumed.
         */
        for (uint32_t mode = 0; mode < enc->i4_modes; mode++) {
            predict_subblock(dest, mode);

            uint32_t error = block_error(here, dest, 4);

            if (mode == 0 || error < cheapest) {
                cheapest = error;
                best = mode;
            }
        }

        modes[n] = (uint8_t) best;

        // the winner is predicted again and reconstructed, because the next
        // subblock predicts from these pixels and must see what the decoder
        // will
        predict_subblock(dest, best);

        coded += code_block(enc, n, here, dest, rec->quant[0][VP8_BLOCK_Y1], 0);
    }

    return block_error(source, y, 16) + (uint64_t) enc->lambda * coded;
}

/** Chooses and codes the chroma planes. */
static uint32_t code_chroma(
    Vp8Encoder* enc, uint32_t mb_x, uint32_t mb_y, const uint8_t* source_u,
    const uint8_t* source_v
) {
    Vp8Decoder* rec = enc->rec;
    uint8_t* u = rec->work_u + VP8_BPS + 8;
    uint8_t* v = rec->work_v + VP8_BPS + 8;

    uint32_t best = VP8_DC_PRED;
    uint32_t cheapest = 0;

    for (uint32_t mode = 0; mode < 4; mode++) {
        uint32_t variant = dc_variant(mode, mb_x, mb_y);

        predict_block(u, 8, variant);
        predict_block(v, 8, variant);

        uint32_t error =
            block_error(source_u, u, 8) + block_error(source_v, v, 8);

        if (mode == 0 || error < cheapest) {
            cheapest = error;
            best = mode;
        }
    }

    uint32_t variant = dc_variant(best, mb_x, mb_y);

    predict_block(u, 8, variant);
    predict_block(v, 8, variant);

    for (uint32_t n = 0; n < 4; n++) {
        uint32_t at = (n >> 1) * 4u * VP8_BPS + (n & 1u) * 4u;

        code_block(
            enc, 16 + n, source_u + at, u + at, rec->quant[0][VP8_BLOCK_UV], 0
        );
        code_block(
            enc, 20 + n, source_v + at, v + at, rec->quant[0][VP8_BLOCK_UV], 0
        );
    }

    return best;
}

/**
 * Writes one macroblock's coefficients into the token partition.
 *
 * The non-zero contexts are carried exactly as the decoder carries them,
 * including the case a skipped macroblock leaves behind, since the two sides
 * read and write the same probabilities from them.
 */
static void write_residuals(
    Vp8Encoder* enc, Vp8Writer* out, uint32_t mb_x, int is_i4x4
) {
    Vp8Decoder* rec = enc->rec;
    uint8_t* above = rec->above_nz + (size_t) mb_x * 9u;
    uint8_t* left = rec->left_nz;

    if (!is_i4x4) {
        uint32_t context = (uint32_t) left[8] + above[8];

        write_coefficients(
            out, rec->coeff_probs[1], context, enc->levels + 24 * 16, 0,
            enc->last[24]
        );

        left[8] = above[8] = (uint8_t) (enc->last[24] >= 0);
    }

    uint32_t type = is_i4x4 ? 3u : 0u;
    uint32_t first = is_i4x4 ? 0u : 1u;

    for (uint32_t block = 0; block < 16; block++) {
        uint32_t l = left_slot(block);
        uint32_t a = above_slot(block);
        uint32_t context = (uint32_t) left[l] + above[a];

        write_coefficients(
            out, rec->coeff_probs[type], context,
            enc->levels + (size_t) block * 16, first, enc->last[block]
        );

        left[l] = above[a] = (uint8_t) (enc->last[block] >= (int32_t) first);
    }

    for (uint32_t block = 16; block < 24; block++) {
        uint32_t l = left_slot(block);
        uint32_t a = above_slot(block);
        uint32_t context = (uint32_t) left[l] + above[a];

        write_coefficients(
            out, rec->coeff_probs[2], context,
            enc->levels + (size_t) block * 16, 0, enc->last[block]
        );

        left[l] = above[a] = (uint8_t) (enc->last[block] >= 0);
    }
}

/**
 * Maps a quality number to the format's quantizer index.
 *
 * The curve is libwebp's, because a quality number has to mean the same thing
 * across encoders for a caller to move between them: file size grows about as
 * the cube of the quantizer, so the quality is linearized and then cube rooted.
 */
static int32_t quality_to_quant(uint32_t quality) {
    float value = (float) quality / 100.0f;
    float linear = value < 0.75f ? value * (2.0f / 3.0f) : 2.0f * value - 1.0f;

    if (linear < 0.0f) linear = 0.0f;

    float compression = tiny_powf(linear, 1.0f / 3.0f);
    int32_t quant = (int32_t) (127.0f * (1.0f - compression));

    return clamp_q(quant);
}

static int encode_lossy(
    const TinyImage* image, const TinyEncodeOpts* options, TinyWriter* writer
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t quality = options && options->quality ? options->quality : 75u;
    if (quality > 100) quality = 100;

    // the four whole-block modes under FAST, all ten otherwise
    uint32_t i4_modes =
        options && options->effort == TINYIMG_EFFORT_FAST ? 4u : 10u;

    Vp8Encoder* enc = tiny_arena_alloc(sizeof(Vp8Encoder), 8);
    Vp8Decoder* rec = tiny_arena_alloc(sizeof(Vp8Decoder), 8);

    if (!enc || !rec) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    tiny_memset(enc, 0, sizeof(*enc));
    tiny_memset(rec, 0, sizeof(*rec));

    enc->rec = rec;

    {
        uint8_t nodes[10];
        uint8_t bits[10];

        build_mode_paths(enc->paths, 0, 0, nodes, bits);
    }

    rec->width = image->width;
    rec->height = image->height;
    rec->mb_w = (image->width + 15u) / 16u;
    rec->mb_h = (image->height + 15u) / 16u;
    rec->y_stride = (size_t) rec->mb_w * 16u;
    rec->uv_stride = (size_t) rec->mb_w * 8u;

    size_t luma = rec->y_stride * rec->mb_h * 16u;
    size_t chroma = rec->uv_stride * rec->mb_h * 8u;
    size_t blocks = (size_t) rec->mb_w * rec->mb_h;

    rec->y = tiny_arena_alloc(luma, 0);
    rec->u = tiny_arena_alloc(chroma, 0);
    rec->v = tiny_arena_alloc(chroma, 0);
    rec->above_nz = tiny_arena_alloc(blocks ? rec->mb_w * 9u : 9u, 0);
    rec->above_modes = tiny_arena_alloc(rec->mb_w * 4u, 0);

    enc->source_y = tiny_arena_alloc(luma, 0);
    enc->source_u = tiny_arena_alloc(chroma, 0);
    enc->source_v = tiny_arena_alloc(chroma, 0);
    enc->records = tiny_arena_alloc(blocks * sizeof(Vp8Record), 8);

    // one byte per pixel of luma is far more than an intra frame needs and
    // costs nothing but arena, and the writer latches an overflow rather than
    // running past it
    size_t capacity = luma + chroma * 2u + 65536u;
    uint8_t* tokens = tiny_arena_alloc(capacity, 0);
    uint8_t* header = tiny_arena_alloc(65536u + blocks * 32u, 0);

    if (!rec->y || !rec->u || !rec->v || !rec->above_nz || !rec->above_modes ||
        !enc->source_y || !enc->source_u || !enc->source_v || !enc->records ||
        !tokens || !header) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    enc->i4_modes = i4_modes;

    int32_t quant = quality_to_quant(quality);
    int32_t deltas[5] = {0, 0, 0, 0, 0};

    setup_quant(rec, quant, deltas);
    tiny_memcpy(rec->coeff_probs, vp8_coeff_probs, sizeof(rec->coeff_probs));

    /*
     * How much a coded coefficient has to save in squared error to be worth
     * its bits. Tied to the square of the quantizer because that is the scale
     * of the error one coefficient can remove; the constant was measured
     * against `cwebp -q` on the reference fixtures.
     */
    uint32_t step = rec->quant[0][VP8_BLOCK_Y1][1];
    enc->lambda = step * step / 2u;

    image_to_yuv(image, enc);

    Vp8Writer token_writer;
    writer_init(&token_writer, tokens, capacity);

    tiny_memset(rec->above_nz, 0, rec->mb_w * 9u);
    tiny_memset(rec->above_modes, 0, rec->mb_w * 4u);

    uint32_t skipped = 0;

    uint8_t source_block[VP8_BPS * 17];
    uint8_t source_u[VP8_BPS * 9];
    uint8_t source_v[VP8_BPS * 9];

    for (uint32_t mb_y = 0; mb_y < rec->mb_h; mb_y++) {
        tiny_memset(rec->left_nz, 0, sizeof(rec->left_nz));
        tiny_memset(rec->left_modes, 0, sizeof(rec->left_modes));

        for (uint32_t mb_x = 0; mb_x < rec->mb_w; mb_x++) {
            Vp8Record* record = &enc->records[(size_t) mb_y * rec->mb_w + mb_x];

            uint8_t* y = source_block + VP8_BPS + 8;
            uint8_t* u = source_u + VP8_BPS + 8;
            uint8_t* v = source_v + VP8_BPS + 8;

            load_source(enc, mb_x, mb_y, y, u, v);

            uint8_t modes[16];

            load_borders(rec, mb_x, mb_y, 1);
            uint64_t cost4 = try_i4(enc, y, modes);

            uint32_t mode16 = 0;

            load_borders(rec, mb_x, mb_y, 1);
            uint64_t cost16 = try_i16(enc, mb_x, mb_y, y, &mode16);

            record->is_i4x4 = (uint8_t) (cost4 < cost16);

            if (record->is_i4x4) {
                // the whole block trial overwrote the working area, so the
                // winner is coded again to leave the reconstruction it implies
                load_borders(rec, mb_x, mb_y, 1);
                try_i4(enc, y, modes);

                tiny_memcpy(record->modes, modes, 16);
            }
            else {
                tiny_memset(record->modes, (uint8_t) mode16, 16);
            }

            record->uv_mode = (uint8_t) code_chroma(enc, mb_x, mb_y, u, v);

            int empty = 1;

            for (uint32_t block = 0; block < 25; block++) {
                if (block == 24 && record->is_i4x4) continue;

                int32_t floor = block < 16 && !record->is_i4x4 ? 0 : -1;
                if (enc->last[block] > floor) empty = 0;
            }

            record->skip = (uint8_t) empty;
            if (empty) skipped++;

            if (!empty) {
                write_residuals(enc, &token_writer, mb_x, record->is_i4x4);
            }
            else {
                uint8_t y2 = rec->left_nz[8];
                uint8_t above_y2 = rec->above_nz[(size_t) mb_x * 9u + 8u];

                tiny_memset(rec->left_nz, 0, sizeof(rec->left_nz));
                tiny_memset(rec->above_nz + (size_t) mb_x * 9u, 0, 9);

                if (record->is_i4x4) {
                    rec->left_nz[8] = y2;
                    rec->above_nz[(size_t) mb_x * 9u + 8u] = above_y2;
                }
            }

            store_block(rec, mb_x, mb_y);
        }
    }

    writer_flush(&token_writer);

    if (token_writer.overflow) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    /*
     * The first partition, written after the frame because the skip
     * probability is a property of the whole of it and comes before the
     * per-macroblock data in one continuous arithmetic stream.
     */
    uint32_t total = (uint32_t) blocks;
    uint32_t coded = total - skipped;
    uint32_t skip_prob = total ? (uint32_t) (255u * coded / total) : 128u;

    if (skip_prob < 1) skip_prob = 1;
    if (skip_prob > 254) skip_prob = 254;

    Vp8Writer head;
    writer_init(&head, header, 65536u + blocks * 32u);

    write_uint(&head, 0, 2);
    write_bool(&head, 128, 0);

    /*
     * The loop filter costs the encoder nothing, which is worth stating
     * because it looks like it should: the decoder filters after every
     * macroblock is reconstructed, and intra prediction deliberately reads the
     * unfiltered pixels, so what this encoder predicts from is still exactly
     * what the decoder will. Only the picture handed back changes.
     *
     * The level rises with the quantizer because that is what it is
     * compensating for; the divisor was measured.
     */
    int32_t filter = quant / 2;

    if (filter > 63) filter = 63;

    write_bool(&head, 128, 0);
    write_uint(&head, (uint32_t) filter, 6);
    write_uint(&head, 0, 3);
    write_bool(&head, 128, 0);

    write_uint(&head, 0, 2);

    write_uint(&head, (uint32_t) quant, 7);
    for (uint32_t i = 0; i < 5; i++) {
        write_bool(&head, 128, 0);
    }

    write_bool(&head, 128, 0);

    // no probability updates, so the decoder keeps the defaults this encoder
    // measured its own statistics against
    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            for (uint32_t k = 0; k < 3; k++) {
                for (uint32_t t = 0; t < 11; t++) {
                    write_bool(&head, vp8_coeff_update[i][j][k][t], 0);
                }
            }
        }
    }

    write_bool(&head, 128, 1);
    write_uint(&head, skip_prob, 8);

    tiny_memset(rec->above_modes, 0, rec->mb_w * 4u);

    for (uint32_t mb_y = 0; mb_y < rec->mb_h; mb_y++) {
        tiny_memset(rec->left_modes, 0, sizeof(rec->left_modes));

        for (uint32_t mb_x = 0; mb_x < rec->mb_w; mb_x++) {
            const Vp8Record* record =
                &enc->records[(size_t) mb_y * rec->mb_w + mb_x];

            write_bool(&head, skip_prob, record->skip);

            uint8_t* top = rec->above_modes + (size_t) mb_x * 4u;
            uint8_t* left = rec->left_modes;

            write_bool(&head, 145, !record->is_i4x4);

            if (!record->is_i4x4) {
                uint32_t mode = record->modes[0];

                write_bool(
                    &head, 156, mode == VP8_TM_PRED || mode == VP8_H_PRED
                );

                if (mode == VP8_TM_PRED || mode == VP8_H_PRED) {
                    write_bool(&head, 128, mode == VP8_TM_PRED);
                }
                else {
                    write_bool(&head, 163, mode == VP8_V_PRED);
                }

                tiny_memset(top, (uint8_t) mode, 4);
                tiny_memset(left, (uint8_t) mode, 4);
            }
            else {
                for (uint32_t y = 0; y < 4; y++) {
                    uint32_t context = left[y];

                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t mode = record->modes[y * 4 + x];
                        const uint8_t* probabilities =
                            vp8_bmode_probs[top[x]][context];
                        const Vp8ModePath* path = &enc->paths[mode];

                        for (uint32_t i = 0; i < path->length; i++) {
                            write_bool(
                                &head, probabilities[path->nodes[i]],
                                path->bits[i]
                            );
                        }

                        top[x] = (uint8_t) mode;
                        context = mode;
                    }

                    left[y] = (uint8_t) context;
                }
            }

            uint32_t uv = record->uv_mode;

            write_bool(&head, 142, uv != VP8_DC_PRED);

            if (uv != VP8_DC_PRED) {
                write_bool(&head, 114, uv != VP8_V_PRED);

                if (uv != VP8_V_PRED) {
                    write_bool(&head, 183, uv == VP8_TM_PRED);
                }
            }
        }
    }

    writer_flush(&head);

    if (head.overflow) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    size_t frame = 10u + head.size + token_writer.size;

    TinyWriter payload;

    if (tiny_writer_init(&payload, frame) != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    // the frame tag: a keyframe, version zero, shown, and the length of the
    // first partition
    uint32_t tag = ((uint32_t) head.size << 5) | (1u << 4);

    tiny_writer_u8(&payload, (uint8_t) (tag & 0xFFu));
    tiny_writer_u8(&payload, (uint8_t) ((tag >> 8) & 0xFFu));
    tiny_writer_u8(&payload, (uint8_t) ((tag >> 16) & 0xFFu));

    tiny_writer_u8(&payload, 0x9D);
    tiny_writer_u8(&payload, 0x01);
    tiny_writer_u8(&payload, 0x2A);

    tiny_writer_le16(&payload, (uint16_t) image->width);
    tiny_writer_le16(&payload, (uint16_t) image->height);

    tiny_writer_write(&payload, head.data, head.size);
    tiny_writer_write(&payload, token_writer.data, token_writer.size);

    size_t chunk = payload.size;

    tiny_writer_write(writer, "RIFF", 4);
    tiny_writer_le32(writer, (uint32_t) (12u + chunk + (chunk & 1u)));
    tiny_writer_write(writer, "WEBP", 4);
    tiny_writer_write(writer, "VP8 ", 4);
    tiny_writer_le32(writer, (uint32_t) chunk);
    tiny_writer_write(writer, payload.data, chunk);

    if (chunk & 1u) tiny_writer_u8(writer, 0);

    int result = payload.error;

    tiny_writer_free(&payload);
    tiny_arena_release(&mark);

    return result != TINYIMG_OK ? result : writer->error;
}

#pragma endregion

#pragma region decode

/**
 * Copies a region of a decoded plane into the image, box averaging as it goes.
 *
 * Neither bitstream can decode a part of itself, so the region and the scale
 * are honored here instead. The averaging matches every other codec's, so a
 * scaled WebP and a scaled PNG of the same picture agree.
 */
static void resample_region(
    const uint8_t* rgba, uint32_t plane_width, const TinyDecodeOpts* resolved,
    TinyImage* image
) {
    uint32_t den = resolved->scale_den;
    uint8_t channels = image->channels;

    // an unscaled region is a copy, and the loop below would reach it by
    // averaging one pixel: four divisions by one to move four bytes
    if (den == 1) {
        for (uint32_t oy = 0; oy < image->height; oy++) {
            const uint8_t* row =
                rgba +
                ((size_t) (resolved->y + oy) * plane_width + resolved->x) * 4u;
            uint8_t* dest = image->data + (size_t) oy * image->width * channels;

            if (channels == 4) {
                tiny_memcpy(dest, row, (size_t) image->width * 4u);
                continue;
            }

            for (uint32_t ox = 0; ox < image->width; ox++) {
                tiny_pixel_convert(
                    dest + (size_t) ox * channels, channels,
                    row + (size_t) ox * 4u, 4
                );
            }
        }
        return;
    }

    for (uint32_t oy = 0; oy < image->height; oy++) {
        uint8_t* dest = image->data + (size_t) oy * image->width * channels;

        uint32_t top = resolved->y + oy * den;
        uint32_t bottom = top + den;

        if (bottom > resolved->y + resolved->height) {
            bottom = resolved->y + resolved->height;
        }

        for (uint32_t ox = 0; ox < image->width; ox++) {
            uint32_t left = resolved->x + ox * den;
            uint32_t right = left + den;

            if (right > resolved->x + resolved->width) {
                right = resolved->x + resolved->width;
            }

            uint32_t sums[4] = {0, 0, 0, 0};
            uint32_t taken = 0;

            for (uint32_t y = top; y < bottom; y++) {
                const uint8_t* row = rgba + (size_t) y * plane_width * 4u;

                for (uint32_t x = left; x < right; x++) {
                    for (uint32_t c = 0; c < 4; c++) {
                        sums[c] += row[(size_t) x * 4u + c];
                    }
                    taken++;
                }
            }

            if (taken == 0) taken = 1;

            uint8_t pixel[4];
            for (uint32_t c = 0; c < 4; c++) {
                pixel[c] = (uint8_t) ((sums[c] + taken / 2) / taken);
            }

            tiny_pixel_convert(
                dest + (size_t) ox * channels, channels, pixel, 4
            );
        }
    }
}

static int webp_decode(
    TinyImage* image, const uint8_t* buffer, size_t size,
    const TinyDecodeOpts* options
) {
    WebpHeader header;
    int result = webp_parse(buffer, size, &header);
    if (result != TINYIMG_OK) return result;

    TinyDecodeOpts resolved;
    uint32_t out_width;
    uint32_t out_height;

    result = tiny_decode_resolve(
        options, header.width, header.height, &resolved, &out_width, &out_height
    );
    if (result != TINYIMG_OK) return result;

    if ((uint64_t) header.width * header.height > TINYIMG_MAX_PIXELS) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    size_t count = (size_t) header.width * header.height;
    uint8_t* rgba = tiny_arena_alloc(count * 4u, 4);

    if (!rgba) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    const uint8_t* stream = buffer + header.bitstream_at;

    if (header.lossless) {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t* pixels = 0;

        result = decode_lossless(
            stream, header.bitstream_size, 0, &width, &height, &pixels
        );

        if (result == TINYIMG_OK) {
            // a frame smaller than the canvas leaves the rest transparent, the
            // way an offset GIF frame does
            tiny_memset(rgba, 0, count * 4u);

            uint32_t rows = height < header.height ? height : header.height;
            uint32_t columns = width < header.width ? width : header.width;

            for (uint32_t y = 0; y < rows; y++) {
                uint8_t* dest = rgba + (size_t) y * header.width * 4u;
                const uint32_t* row = pixels + (size_t) y * width;

                for (uint32_t x = 0; x < columns; x++) {
                    dest[x * 4u + 0] = (uint8_t) ((row[x] >> 16) & 0xFFu);
                    dest[x * 4u + 1] = (uint8_t) ((row[x] >> 8) & 0xFFu);
                    dest[x * 4u + 2] = (uint8_t) (row[x] & 0xFFu);
                    dest[x * 4u + 3] = (uint8_t) ((row[x] >> 24) & 0xFFu);
                }
            }
        }
    }
    else {
        // an alpha chunk is decoded over the whole plane, so it needs all of it
        uint32_t rows =
            header.alpha_at ? header.height : resolved.y + resolved.height;

        result = decode_lossy(
            stream, header.bitstream_size, header.width, header.height, rgba,
            rows, resolved.effort
        );

        if (result == TINYIMG_OK && header.alpha_at) {
            uint8_t* plane = tiny_arena_alloc(count, 0);

            if (!plane) {
                result = TINYIMG_ERR_MEMORY;
            }
            else {
                result = decode_alpha(
                    buffer + header.alpha_at, header.alpha_size, header.width,
                    header.height, plane
                );

                if (result == TINYIMG_OK) {
                    for (size_t i = 0; i < count; i++) {
                        rgba[i * 4u + 3] = plane[i];
                    }
                }
            }
        }
    }

    if (result != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return result;
    }

    uint8_t channels = resolved.channels ? resolved.channels
                                         : (uint8_t) (header.has_alpha ? 4 : 3);

    result = tiny_image_create(image, out_width, out_height, channels);

    if (result != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return result;
    }

    resample_region(rgba, header.width, &resolved, image);
    tiny_arena_release(&mark);

    image->format = TINYIMG_FORMAT_WEBP;
    return TINYIMG_OK;
}

#pragma endregion

static int webp_encode(
    const TinyImage* image, const TinyEncodeOpts* options, TinyWriter* writer
) {
    if (!image || !image->data || !writer) return TINYIMG_ERR_NULL;
    if (image->width == 0 || image->height == 0) return TINYIMG_ERR_RANGE;

    // fourteen bits each, which is the format's own limit rather than this
    // build's
    if (image->width > 16383 || image->height > 16383) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    if (options && options->lossless) return encode_lossless(image, writer);

    return encode_lossy(image, options, writer);
}

void tiny_webp_plane_codes(uint8_t* out) {
    if (out) build_planes(out);
}

const TinyCodec tiny_codec_webp = {
    TINYIMG_FORMAT_WEBP, webp_sniff, webp_probe, webp_decode, webp_encode
};
