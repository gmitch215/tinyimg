#include "tinyimg/codec/lzw.h"

#include "tinyimg/tinyimg.h"

void tiny_lzw_init(
    TinyLzwReader* reader, const uint8_t* data, size_t size, size_t at,
    int chained, int msb
) {
    reader->data = data;
    reader->size = size;
    reader->pos = at;
    reader->accumulator = 0;
    reader->count = 0;
    reader->remaining = 0;
    reader->chained = (uint8_t) (chained ? 1 : 0);
    reader->msb = (uint8_t) (msb ? 1 : 0);
    reader->done = 0;
}

static uint32_t lzw_bits(TinyLzwReader* reader, uint32_t want) {
    while (reader->count < want) {
        if (reader->chained && reader->remaining == 0) {
            if (reader->pos >= reader->size) {
                reader->done = 1;
                reader->count = want;
                break;
            }

            reader->remaining = reader->data[reader->pos++];

            // a zero length sub-block is the terminator, not an empty one
            if (reader->remaining == 0) {
                reader->done = 1;
                reader->count = want;
                break;
            }
        }

        if (reader->pos >= reader->size) {
            reader->done = 1;
            reader->count = want;
            break;
        }

        uint8_t byte = reader->data[reader->pos++];
        if (reader->chained) reader->remaining--;

        if (reader->msb) {
            reader->accumulator = (reader->accumulator << 8) | byte;
        }
        else {
            reader->accumulator |= (uint32_t) byte << reader->count;
        }

        reader->count += 8;
    }

    uint32_t value;

    if (reader->msb) {
        value = (reader->accumulator >> (reader->count - want)) &
                ((1u << want) - 1u);
    }
    else {
        value = reader->accumulator & ((1u << want) - 1u);
        reader->accumulator >>= want;
    }

    reader->count -= want;
    return value;
}

int tiny_lzw_expand(
    TinyLzwReader* reader, TinyLzwTable* table, uint32_t code_bits, int early,
    uint8_t* out, size_t capacity
) {
    uint32_t clear = 1u << code_bits;
    uint32_t end = clear + 1;
    uint32_t next = clear + 2;
    uint32_t width = code_bits + 1;
    uint32_t previous = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < clear; i++) {
        table->prefix[i] = 0xFFFFu;
        table->suffix[i] = (uint8_t) i;
        table->first[i] = (uint8_t) i;
    }

    size_t written = 0;
    uint8_t stack[TINY_LZW_CODES];

    while (!reader->done) {
        uint32_t code = lzw_bits(reader, width);

        if (reader->done && written == 0) return TINYIMG_ERR_CORRUPT;
        if (code == end) break;

        if (code == clear) {
            next = clear + 2;
            width = code_bits + 1;
            previous = 0xFFFFFFFFu;
            continue;
        }

        if (code > next) return TINYIMG_ERR_CORRUPT;

        uint32_t walk = code;
        uint32_t depth = 0;

        // the one self referential case the format allows: a code defined by
        // the very sequence it is being used to define
        if (code == next) {
            if (previous == 0xFFFFFFFFu) return TINYIMG_ERR_CORRUPT;

            stack[depth++] = table->first[previous];
            walk = previous;
        }

        while (walk != 0xFFFFFFFFu && depth < TINY_LZW_CODES) {
            stack[depth++] = table->suffix[walk];
            walk = table->prefix[walk] == 0xFFFFu ? 0xFFFFFFFFu
                                                  : table->prefix[walk];
        }

        if (depth == 0) return TINYIMG_ERR_CORRUPT;

        // the stack holds the expansion reversed, so its top is the byte that
        // comes out first, which is what the new entry extends `previous` by.
        // The last byte is a different value and using it decodes most of a
        // file correctly and the rest to noise
        uint8_t leading = stack[depth - 1];

        while (depth > 0) {
            if (written < capacity)
                out[written++] = stack[--depth];
            else
                depth--;
        }

        if (previous != 0xFFFFFFFFu && next < TINY_LZW_CODES) {
            table->prefix[next] = (uint16_t) previous;
            table->suffix[next] = leading;
            table->first[next] = table->first[previous];

            next++;

            uint32_t threshold = early ? (1u << width) - 1u : (1u << width);

            if (next >= threshold && width < TINY_LZW_MAX_BITS) width++;
        }

        previous = code;
    }

    return TINYIMG_OK;
}
