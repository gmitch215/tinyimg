#include "av1.h"

/** Bits of CDF precision the arithmetic coder discards. */
#define AV1_EC_PROB_SHIFT 6

/** Probability floor every symbol is guaranteed. */
#define AV1_EC_MIN_PROB 4

/** The coder works at fifteen bits of precision throughout. */
#define AV1_CDF_ONE (1u << 15)

#pragma region bits

/**
 * Reads `count` bits, most significant first, returning zeros past the end.
 *
 * The specification's f(n) over a tile, with one deliberate difference that is
 * not a shortcut: reading past the last byte yields zeros. SymbolMaxBits going
 * negative is how the specification says a decode has entered padding that was
 * never written, and a conformant tile finishes inside it, so refusing here
 * would reject valid streams.
 */
static uint32_t av1_f(TinyAv1Symbol* symbol, uint32_t count) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < count; i++) {
        size_t index = symbol->bit >> 3;
        uint32_t bit = 0;

        if (index < symbol->size) {
            uint32_t shift = 7u - (uint32_t) (symbol->bit & 7u);
            bit = (uint32_t) (symbol->data[index] >> shift) & 1u;
        }

        value = (value << 1) | bit;
        symbol->bit++;
    }

    return value;
}

/** Position of the highest set bit, which the specification calls FloorLog2. */
static uint32_t av1_floor_log2(uint32_t value) {
    uint32_t log = 0;

    while (value > 1u) {
        value >>= 1;
        log++;
    }

    return log;
}

#pragma endregion

#pragma region decoder

void tiny_av1_symbol_init(
    TinyAv1Symbol* symbol, const uint8_t* data, size_t size
) {
    symbol->data = data;
    symbol->size = size;
    symbol->bit = 0;
    symbol->frozen = 0;

    // a tile cannot approach the point where this overflows, since an image is
    // capped well below it, but the clamp keeps the arithmetic signed-safe
    size_t total = size > (size_t) 0x0FFFFFFF ? (size_t) 0x0FFFFFFF : size;
    uint32_t available = (uint32_t) total * 8u;

    uint32_t count = available < 15u ? available : 15u;
    uint32_t buf = av1_f(symbol, count);
    uint32_t padded = buf << (15u - count);

    symbol->value = (AV1_CDF_ONE - 1u) ^ padded;
    symbol->range = AV1_CDF_ONE;
    symbol->max_bits = (int32_t) available - 15;
}

/**
 * Renormalizes after a symbol, pulling in the bits the range shrank by.
 *
 * `bits` can be zero, in which case none of this has any effect, which is why
 * the specification writes it as ordered steps rather than a loop.
 */
static void av1_renormalize(TinyAv1Symbol* symbol) {
    uint32_t bits = 15u - av1_floor_log2(symbol->range);

    symbol->range <<= bits;

    int32_t available = symbol->max_bits > 0 ? symbol->max_bits : 0;
    uint32_t read = bits < (uint32_t) available ? bits : (uint32_t) available;

    uint32_t data = av1_f(symbol, read);
    uint32_t padded = data << (bits - read);

    symbol->value = padded ^ (((symbol->value + 1u) << bits) - 1u);
    symbol->max_bits -= (int32_t) bits;
}

/**
 * Adapts a distribution toward the symbol just read.
 *
 * The last entry counts decodes to a ceiling of 32 and is what makes the rate
 * depend on how much has been seen, so it is state rather than a probability
 * and must not be treated as one.
 */
static void av1_adapt(uint16_t* cdf, uint32_t count, uint32_t symbol) {
    uint32_t seen = cdf[count];
    uint32_t log = av1_floor_log2(count);
    uint32_t rate = 3u + (seen > 15u ? 1u : 0u) + (seen > 31u ? 1u : 0u) +
                    (log < 2u ? log : 2u);
    uint32_t target = 0;

    for (uint32_t i = 0; i + 1u < count; i++) {
        if (i == symbol) target = AV1_CDF_ONE;

        if (target < cdf[i]) {
            cdf[i] -= (uint16_t) ((cdf[i] - target) >> rate);
        }
        else {
            cdf[i] += (uint16_t) ((target - cdf[i]) >> rate);
        }
    }

    if (seen < 32u) cdf[count] = (uint16_t) (seen + 1u);
}

uint32_t tiny_av1_symbol_read(
    TinyAv1Symbol* symbol, uint16_t* cdf, uint32_t count
) {
    uint32_t cur = symbol->range;
    uint32_t prev;
    uint32_t index = 0;

    // walks the distribution until the value falls outside the current
    // interval; the do-while shape is the specification's, and index lands one
    // past the last interval the value was inside
    do {
        prev = cur;

        uint32_t f = AV1_CDF_ONE - cdf[index];
        cur = ((symbol->range >> 8) * (f >> AV1_EC_PROB_SHIFT)) >>
              (7u - AV1_EC_PROB_SHIFT);
        cur += AV1_EC_MIN_PROB * (count - index - 1u);

        index++;
    } while (symbol->value < cur);

    index--;

    symbol->range = prev - cur;
    symbol->value -= cur;

    av1_renormalize(symbol);

    if (!symbol->frozen) av1_adapt(cdf, count, index);

    return index;
}

uint32_t tiny_av1_symbol_bit(TinyAv1Symbol* symbol) {
    // an even distribution, rebuilt per call because the adaptation the read
    // performs on it is never looked at again
    uint16_t cdf[3] = {1u << 14, 1u << 15, 0};

    return tiny_av1_symbol_read(symbol, cdf, 2);
}

uint32_t tiny_av1_symbol_literal(TinyAv1Symbol* symbol, uint32_t count) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < count; i++) {
        value = (value << 1) | tiny_av1_symbol_bit(symbol);
    }

    return value;
}

#pragma endregion

#pragma region encoder

/**
 * One sub-interval boundary, which is the decoder's `cur` for the same index.
 *
 * Shared arithmetic rather than a second copy of it: the encoder's whole
 * correctness is that it computes the same boundaries the decoder will, so the
 * two must not be able to drift.
 */
static uint32_t av1_boundary(
    uint32_t range, const uint16_t* cdf, uint32_t count, uint32_t index
) {
    uint32_t f = AV1_CDF_ONE - cdf[index];
    uint32_t cur =
        ((range >> 8) * (f >> AV1_EC_PROB_SHIFT)) >> (7u - AV1_EC_PROB_SHIFT);

    return cur + AV1_EC_MIN_PROB * (count - index - 1u);
}

void tiny_av1_symbol_enc_init(
    TinyAv1SymbolEnc* enc, uint16_t* pending, size_t capacity
) {
    enc->pending = pending;
    enc->capacity = capacity;
    enc->count = 0;
    enc->low = 0;
    enc->range = AV1_CDF_ONE;
    enc->held = -1;
    enc->overflow = 0;
}

/** Queues one byte that a later carry may still increment. */
static void av1_queue(TinyAv1SymbolEnc* enc, uint32_t byte) {
    if (enc->count >= enc->capacity) {
        enc->overflow = 1u;
        return;
    }

    enc->pending[enc->count++] = (uint16_t) byte;
}

/**
 * Absorbs a carry out of `low` into the bytes already queued.
 *
 * The entry is widened rather than propagated here, because an increment that
 * takes one entry to 256 is resolved by the same pass that turns every entry
 * into a byte, and doing it twice is how a carry chain gets a bug.
 */
static void av1_carry(TinyAv1SymbolEnc* enc) {
    if (enc->count == 0u) {
        // nothing queued yet, so there is nowhere for the carry to go; this
        // cannot happen from a conformant sequence, and dropping it silently
        // would corrupt the stream instead of reporting it
        enc->overflow = 1u;
        return;
    }

    enc->pending[enc->count - 1u]++;
}

/** Narrows the interval to one symbol's share of it, then renormalizes. */
static void av1_encode(
    TinyAv1SymbolEnc* enc, const uint16_t* cdf, uint32_t count, uint32_t symbol
) {
    uint32_t range = enc->range;
    uint32_t high =
        symbol == 0u ? range : av1_boundary(range, cdf, count, symbol - 1u);
    uint32_t low = av1_boundary(range, cdf, count, symbol);

    enc->low += range - high;
    enc->range = high - low;

    uint64_t limit = (uint64_t) 1 << (16 + enc->held);

    if (enc->low >= limit) {
        enc->low -= limit;
        av1_carry(enc);
    }

    uint32_t bits = 15u - av1_floor_log2(enc->range);

    enc->range <<= bits;
    enc->low <<= bits;
    enc->held += (int32_t) bits;

    while (enc->held >= 8) {
        uint32_t shift = (uint32_t) enc->held + 8u;

        av1_queue(enc, (uint32_t) (enc->low >> shift) & 0xFFu);

        enc->low &= ((uint64_t) 1 << shift) - 1u;
        enc->held -= 8;
    }
}

void tiny_av1_symbol_write(
    TinyAv1SymbolEnc* enc, uint16_t* cdf, uint32_t count, uint32_t symbol
) {
    av1_encode(enc, cdf, count, symbol);
    av1_adapt(cdf, count, symbol);
}

void tiny_av1_symbol_write_frozen(
    TinyAv1SymbolEnc* enc, const uint16_t* cdf, uint32_t count, uint32_t symbol
) {
    av1_encode(enc, cdf, count, symbol);
}

void tiny_av1_symbol_write_bit(TinyAv1SymbolEnc* enc, uint32_t bit) {
    uint16_t cdf[3] = {1u << 14, 1u << 15, 0};

    // the same distribution the reader rebuilds per call, and adapted the same
    // way, because the copy is thrown away either side
    tiny_av1_symbol_write(enc, cdf, 2u, bit);
}

void tiny_av1_symbol_write_literal(
    TinyAv1SymbolEnc* enc, uint32_t value, uint32_t count
) {
    for (uint32_t i = 0; i < count; i++) {
        tiny_av1_symbol_write_bit(enc, (value >> (count - 1u - i)) & 1u);
    }
}

int tiny_av1_symbol_finish(
    TinyAv1SymbolEnc* enc, uint8_t* out, size_t capacity, size_t* size
) {
    /*
     * The interval is at least 2^15 wide, so it holds an odd multiple of 2^14.
     *
     * Choosing that one is what puts the tile's trailing 1 bit where the exit
     * process requires it: every bit below it is zero, so the tile ends with a
     * single set bit and then padding, and the bits after it need not be
     * written at all because the reader supplies zeros past the end.
     */
    uint64_t mark = (uint64_t) 1 << 14;
    uint64_t value = ((enc->low + mark - 1u) & ~(mark - 1u)) | mark;
    uint64_t limit = (uint64_t) 1 << (16 + enc->held);

    if (value >= limit) {
        value -= limit;
        av1_carry(enc);
    }

    // the significant bits run from the top of the window down to the mark, and
    // whatever is below it is zero and need not be written at all
    uint32_t remaining = (uint32_t) (enc->held + 2);
    uint32_t top = (uint32_t) (enc->held + 16);

    while (remaining > 0u) {
        uint32_t shift = top - 8u;

        av1_queue(enc, (uint32_t) (value >> shift) & 0xFFu);

        value &= ((uint64_t) 1 << shift) - 1u;
        top = shift;
        remaining = remaining > 8u ? remaining - 8u : 0u;
    }

    if (enc->overflow || enc->count > capacity) {
        return TINYIMG_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t carry = 0;

    for (size_t i = enc->count; i-- > 0u;) {
        uint32_t total = enc->pending[i] + carry;

        out[i] = (uint8_t) total;
        carry = total >> 8;
    }

    // a carry out of the first byte would mean the value needed one more byte
    // than the interval can, which the construction above makes unreachable
    if (carry) return TINYIMG_ERR_BUFFER_TOO_SMALL;

    *size = enc->count;

    return TINYIMG_OK;
}

#pragma endregion
