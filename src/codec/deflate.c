#include "tinyimg/codec/deflate.h"

#include "tinyimg/memory.h"

#pragma region tables

// RFC 1951 section 3.2.5. small, unconditional and needed by both directions,
// so inlined rather than derived
static const uint16_t length_base[29] = {3,   4,   5,   6,   7,  8,  9,  10,
                                         11,  13,  15,  17,  19, 23, 27, 31,
                                         35,  43,  51,  59,  67, 83, 99, 115,
                                         131, 163, 195, 227, 258};

static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                         1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                         4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t distance_base[30] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t distance_extra[30] = {0,  0,  0,  0,  1,  1, 2,  2,
                                           3,  3,  4,  4,  5,  5, 6,  6,
                                           7,  7,  8,  8,  9,  9, 10, 10,
                                           11, 11, 12, 12, 13, 13};

// the order the code length alphabet's own lengths are written in
static const uint8_t length_order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                         11, 4,  12, 3, 13, 2, 14, 1, 15};

static uint32_t length_code(uint32_t length) {
    uint32_t code = 28;
    while (code > 0 && length < length_base[code]) {
        code--;
    }
    return code;
}

static uint32_t distance_code(uint32_t distance) {
    uint32_t code = 29;
    while (code > 0 && distance < distance_base[code]) {
        code--;
    }
    return code;
}

#pragma endregion

#pragma region huffman

// deflate packs a code most significant bit first into a stream read least
// significant bit first, so both directions work with the code reversed
static uint32_t reverse_bits(uint32_t code, uint32_t length) {
    uint32_t reversed = 0;

    for (uint32_t i = 0; i < length; i++) {
        reversed = (reversed << 1) | (code & 1u);
        code >>= 1;
    }
    return reversed;
}

/**
 * Builds a decode table from a list of code lengths.
 *
 * @return int TINYIMG_OK, or TINYIMG_ERR_CORRUPT when the lengths do not form a
 * complete code.
 */
static int huffman_build(
    TinyHuffman* table, const uint8_t* lengths, uint32_t n
) {
    tiny_memset(table->counts, 0, sizeof(table->counts));
    tiny_memset(table->fast, 0, sizeof(table->fast));

    for (uint32_t i = 0; i < n; i++) {
        table->counts[lengths[i]]++;
    }

    // a single symbol alphabet is legal for distances and cannot be
    // over-subscribed
    if (table->counts[0] == n) return TINYIMG_OK;

    table->counts[0] = 0;

    int32_t left = 1;
    for (uint32_t length = 1; length <= 15; length++) {
        left <<= 1;
        left -= (int32_t) table->counts[length];
        if (left < 0) return TINYIMG_ERR_CORRUPT;
    }

    uint16_t offsets[16];
    offsets[0] = 0;
    offsets[1] = 0;
    for (uint32_t length = 1; length < 15; length++) {
        offsets[length + 1] =
            (uint16_t) (offsets[length] + table->counts[length]);
    }

    for (uint32_t i = 0; i < n; i++) {
        if (lengths[i] != 0)
            table->symbols[offsets[lengths[i]]++] = (uint16_t) i;
    }

    uint32_t code = 0;
    uint32_t index = 0;

    for (uint32_t length = 1; length <= 15; length++) {
        for (uint32_t k = 0; k < table->counts[length]; k++) {
            uint16_t symbol = table->symbols[index++];

            if (length <= TINY_DEFLATE_FAST_BITS) {
                uint32_t reversed = reverse_bits(code, length);
                uint16_t entry = (uint16_t) (((uint32_t) symbol << 4) | length);

                for (uint32_t fill = reversed;
                     fill < (1u << TINY_DEFLATE_FAST_BITS);
                     fill += (1u << length)) {
                    table->fast[fill] = entry;
                }
            }
            code++;
        }
        code <<= 1;
    }

    return TINYIMG_OK;
}

// the fast table resolves nine bits at a time; anything longer is walked one
// bit at a time the way RFC 1951's own description does
static int huffman_decode(TinyBitReader* bits, const TinyHuffman* table) {
    uint16_t entry =
        table->fast[tiny_bits_peek_lsb(bits, TINY_DEFLATE_FAST_BITS)];

    if ((entry & 0x0Fu) != 0) {
        tiny_bits_skip_lsb(bits, entry & 0x0Fu);
        return entry >> 4;
    }

    int32_t code = 0;
    int32_t first = 0;
    int32_t index = 0;

    for (uint32_t length = 1; length <= 15; length++) {
        code |= (int32_t) tiny_bits_lsb(bits, 1);

        int32_t count = (int32_t) table->counts[length];
        if (code - first < count) return table->symbols[index + (code - first)];

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }

    return -1;
}

static void huffman_fixed(TinyHuffman* literals, TinyHuffman* distances) {
    uint8_t lengths[288];

    for (uint32_t i = 0; i < 144; i++) lengths[i] = 8;
    for (uint32_t i = 144; i < 256; i++) lengths[i] = 9;
    for (uint32_t i = 256; i < 280; i++) lengths[i] = 7;
    for (uint32_t i = 280; i < 288; i++) lengths[i] = 8;
    huffman_build(literals, lengths, 288);

    for (uint32_t i = 0; i < 30; i++) lengths[i] = 5;
    huffman_build(distances, lengths, 30);
}

#pragma endregion

#pragma region inflate

static inline void window_put(TinyInflate* state, uint8_t byte) {
    state->window[state->head] = byte;
    state->head = (state->head + 1) & (TINY_DEFLATE_WINDOW - 1);
    state->pending++;
}

/**
 * Copies a back reference into the window.
 *
 * A run whose distance is shorter than its length is how DEFLATE codes a
 * repeat, and it has to be copied a byte at a time because it reads what it has
 * just written. Anything else is a plain copy, split only where it wraps the
 * ring.
 */
static void window_copy(
    TinyInflate* state, uint32_t distance, uint32_t length
) {
    size_t from = (state->head - distance) & (TINY_DEFLATE_WINDOW - 1);

    if (distance < length) {
        for (uint32_t i = 0; i < length; i++) {
            state->window[state->head] = state->window[from];
            state->head = (state->head + 1) & (TINY_DEFLATE_WINDOW - 1);
            from = (from + 1) & (TINY_DEFLATE_WINDOW - 1);
        }

        state->pending += length;
        return;
    }

    uint32_t left = length;
    while (left > 0) {
        size_t take = left;

        if (take > TINY_DEFLATE_WINDOW - from)
            take = TINY_DEFLATE_WINDOW - from;
        if (take > TINY_DEFLATE_WINDOW - state->head) {
            take = TINY_DEFLATE_WINDOW - state->head;
        }

        // a move rather than a copy: the ring positions do not overlap, but a
        // distance near the full window puts the source just ahead of the head,
        // so the two linear ranges do. a forward copy is right either way and
        // this is the one that is also defined
        tiny_memmove(state->window + state->head, state->window + from, take);

        state->head = (state->head + take) & (TINY_DEFLATE_WINDOW - 1);
        from = (from + take) & (TINY_DEFLATE_WINDOW - 1);
        left -= (uint32_t) take;
    }

    state->pending += length;
}

int tiny_inflate_init(
    TinyInflate* state, const uint8_t* data, size_t size, int zlib
) {
    if (!state || !data) return TINYIMG_ERR_NULL;

    tiny_memset(state, 0, sizeof(*state));

    state->window = tiny_arena_alloc(TINY_DEFLATE_WINDOW, 0);
    state->literals = tiny_arena_alloc(sizeof(TinyHuffman), 0);
    state->distances = tiny_arena_alloc(sizeof(TinyHuffman), 0);

    if (!state->window || !state->literals || !state->distances) {
        return TINYIMG_ERR_MEMORY;
    }

    state->adler_a = 1;
    state->zlib = zlib;

    if (zlib) {
        if (size < 2) return TINYIMG_ERR_CORRUPT;

        uint32_t cmf = data[0];
        uint32_t flg = data[1];

        // method 8 is the only one defined, FDICT is not allowed in PNG or
        // TIFF, and the two bytes together have to be a multiple of 31
        if ((cmf & 0x0Fu) != 8 || (flg & 0x20u) != 0 ||
            ((cmf << 8) | flg) % 31u != 0) {
            return TINYIMG_ERR_CORRUPT;
        }

        data += 2;
        size -= 2;
    }

    tiny_bits_init(&state->bits, data, size);
    return TINYIMG_OK;
}

static int inflate_dynamic(TinyInflate* state) {
    uint32_t literal_count = tiny_bits_lsb(&state->bits, 5) + 257;
    uint32_t distance_count = tiny_bits_lsb(&state->bits, 5) + 1;
    uint32_t code_count = tiny_bits_lsb(&state->bits, 4) + 4;

    if (literal_count > 286 || distance_count > 30) return TINYIMG_ERR_CORRUPT;

    uint8_t lengths[288 + 32];
    tiny_memset(lengths, 0, sizeof(lengths));

    for (uint32_t i = 0; i < code_count; i++) {
        lengths[length_order[i]] = (uint8_t) tiny_bits_lsb(&state->bits, 3);
    }
    for (uint32_t i = code_count; i < 19; i++) {
        lengths[length_order[i]] = 0;
    }

    // the code length alphabet is itself Huffman coded, into the table the two
    // real alphabets are then read with
    int result = huffman_build(state->literals, lengths, 19);
    if (result != TINYIMG_OK) return result;

    tiny_memset(lengths, 0, sizeof(lengths));
    uint32_t total = literal_count + distance_count;

    for (uint32_t i = 0; i < total;) {
        int symbol = huffman_decode(&state->bits, state->literals);
        if (symbol < 0) return TINYIMG_ERR_CORRUPT;

        if (symbol < 16) {
            lengths[i++] = (uint8_t) symbol;
            continue;
        }

        uint32_t repeat;
        uint8_t value = 0;

        if (symbol == 16) {
            if (i == 0) return TINYIMG_ERR_CORRUPT;
            value = lengths[i - 1];
            repeat = tiny_bits_lsb(&state->bits, 2) + 3;
        }
        else if (symbol == 17) {
            repeat = tiny_bits_lsb(&state->bits, 3) + 3;
        }
        else {
            repeat = tiny_bits_lsb(&state->bits, 7) + 11;
        }

        if (i + repeat > total) return TINYIMG_ERR_CORRUPT;
        while (repeat-- > 0) {
            lengths[i++] = value;
        }
    }

    if (lengths[256] == 0) return TINYIMG_ERR_CORRUPT;

    result = huffman_build(state->literals, lengths, literal_count);
    if (result != TINYIMG_OK) return result;

    return huffman_build(
        state->distances, lengths + literal_count, distance_count
    );
}

// produces at least one symbol's worth of output, or moves to the next block,
// or finishes
static void inflate_step(TinyInflate* state) {
    if (state->error != TINYIMG_OK || state->done) return;

    if (!state->inside) {
        if (state->bits.overrun) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        state->final = (int) tiny_bits_lsb(&state->bits, 1);
        state->mode = (int) tiny_bits_lsb(&state->bits, 2);

        if (state->mode == 3) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        if (state->mode == 0) {
            tiny_bits_align_lsb(&state->bits);

            uint32_t len = tiny_bits_lsb(&state->bits, 16);
            uint32_t nlen = tiny_bits_lsb(&state->bits, 16);

            if ((len ^ 0xFFFFu) != nlen) {
                state->error = TINYIMG_ERR_CORRUPT;
                return;
            }
            state->stored = len;
        }
        else if (state->mode == 1) {
            huffman_fixed(state->literals, state->distances);
        }
        else {
            int result = inflate_dynamic(state);
            if (result != TINYIMG_OK) {
                state->error = result;
                return;
            }
        }

        state->inside = 1;
        return;
    }

    if (state->mode == 0) {
        while (state->stored > 0 && state->pending < TINY_DEFLATE_MAX_READ) {
            window_put(state, (uint8_t) tiny_bits_lsb(&state->bits, 8));
            state->stored--;
        }

        if (state->stored == 0) {
            state->inside = 0;
            if (state->final) state->done = 1;
        }

        if (state->bits.overrun) state->error = TINYIMG_ERR_CORRUPT;
        return;
    }

    while (state->pending < TINY_DEFLATE_MAX_READ) {
        int symbol = huffman_decode(&state->bits, state->literals);

        if (symbol < 0 || state->bits.overrun) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        if (symbol < 256) {
            window_put(state, (uint8_t) symbol);
            continue;
        }

        if (symbol == 256) {
            state->inside = 0;
            if (state->final) state->done = 1;
            return;
        }

        uint32_t code = (uint32_t) symbol - 257;
        if (code >= 29) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        uint32_t length = length_base[code];
        if (length_extra[code] > 0) {
            length += tiny_bits_lsb(&state->bits, length_extra[code]);
        }

        int distance_symbol = huffman_decode(&state->bits, state->distances);
        if (distance_symbol < 0 || distance_symbol >= 30) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        uint32_t distance = distance_base[distance_symbol];
        if (distance_extra[distance_symbol] > 0) {
            distance +=
                tiny_bits_lsb(&state->bits, distance_extra[distance_symbol]);
        }

        if (distance > TINY_DEFLATE_WINDOW) {
            state->error = TINYIMG_ERR_CORRUPT;
            return;
        }

        window_copy(state, distance, length);
    }
}

long tiny_inflate_read(TinyInflate* state, uint8_t* out, size_t size) {
    if (!state || !out) return TINYIMG_ERR_NULL;
    if (state->error != TINYIMG_OK) return state->error;
    if (size > TINY_DEFLATE_MAX_READ) size = TINY_DEFLATE_MAX_READ;

    while (state->pending < size && !state->done) {
        inflate_step(state);
        if (state->error != TINYIMG_OK) return state->error;
    }

    size_t produced = state->pending < size ? state->pending : size;

    // at most two runs, since the only discontinuity is where the ring wraps
    size_t first = TINY_DEFLATE_WINDOW - state->tail;
    if (first > produced) first = produced;

    tiny_memcpy(out, state->window + state->tail, first);
    if (produced > first) {
        tiny_memcpy(out + first, state->window, produced - first);
    }

    state->tail = (state->tail + produced) & (TINY_DEFLATE_WINDOW - 1);
    state->pending -= produced;

    // folded here rather than as each byte is produced, so it costs one tight
    // loop over a contiguous buffer instead of four operations inside the
    // symbol loop. a caller that stops early leaves it incomplete, which is
    // consistent: tiny_inflate_finish refuses an unfinished stream before it
    // looks at the checksum
    if (state->zlib) {
        uint32_t running = (state->adler_b << 16) | state->adler_a;

        running = tiny_adler32(running, out, produced);
        state->adler_a = running & 0xFFFFu;
        state->adler_b = (running >> 16) & 0xFFFFu;
    }

    return (long) produced;
}

int tiny_inflate_finish(TinyInflate* state) {
    if (!state) return TINYIMG_ERR_NULL;
    if (state->error != TINYIMG_OK) return state->error;
    if (!state->done) return TINYIMG_ERR_CORRUPT;

    if (!state->zlib) return TINYIMG_OK;

    tiny_bits_align_lsb(&state->bits);

    uint32_t stored = 0;
    for (uint32_t i = 0; i < 4; i++) {
        stored = (stored << 8) | tiny_bits_lsb(&state->bits, 8);
    }

    if (state->bits.overrun) return TINYIMG_ERR_CORRUPT;
    if (stored != ((state->adler_b << 16) | state->adler_a)) {
        return TINYIMG_ERR_CORRUPT;
    }

    return TINYIMG_OK;
}

int tiny_inflate_all(
    const uint8_t* data, size_t size, int zlib, TinyWriter* out
) {
    if (!out) return TINYIMG_ERR_NULL;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    TinyInflate state;
    int result = tiny_inflate_init(&state, data, size, zlib);

    if (result == TINYIMG_OK) {
        uint8_t* chunk = tiny_arena_alloc(TINY_DEFLATE_MAX_READ, 0);

        if (!chunk) {
            result = TINYIMG_ERR_MEMORY;
        }
        else {
            for (;;) {
                long read =
                    tiny_inflate_read(&state, chunk, TINY_DEFLATE_MAX_READ);
                if (read < 0) {
                    result = (int) read;
                    break;
                }
                if (read == 0) {
                    result = tiny_inflate_finish(&state);
                    break;
                }

                result = tiny_writer_write(out, chunk, (size_t) read);
                if (result != TINYIMG_OK) break;
            }
        }
    }

    tiny_arena_release(&mark);
    return result;
}

#pragma endregion

#pragma region deflate

#define DEFLATE_HASH_BITS 15u
#define DEFLATE_HASH_SIZE (1u << DEFLATE_HASH_BITS)
#define DEFLATE_BLOCK 16384u
#define DEFLATE_MIN_MATCH 3u
#define DEFLATE_MAX_MATCH 258u

/** Canonical codes ready for the writer, so already reversed. */
typedef struct {
    uint16_t code[288];
    uint8_t length[288];
} TinyCodes;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;

    int32_t* head;
    int32_t* chain;

    uint16_t* symbols;
    uint16_t* distances;
    uint32_t count;
    size_t block_start;

    uint32_t literal_freq[286];
    uint32_t distance_freq[30];

    TinyWriter* out;
    TinyBitWriter bits;

    uint32_t chain_limit;
    int lazy;
} TinyDeflateState;

static void codes_from_lengths(
    TinyCodes* codes, const uint8_t* lengths, uint32_t n
) {
    uint16_t counts[16];
    tiny_memset(counts, 0, sizeof(counts));

    for (uint32_t i = 0; i < n; i++) {
        counts[lengths[i]]++;
    }
    counts[0] = 0;

    uint16_t next[16];
    uint32_t code = 0;

    for (uint32_t length = 1; length <= 15; length++) {
        next[length] = (uint16_t) code;
        code = (code + counts[length]) << 1;
    }

    for (uint32_t i = 0; i < n; i++) {
        codes->length[i] = lengths[i];
        codes->code[i] =
            lengths[i] > 0
                ? (uint16_t) reverse_bits(next[lengths[i]]++, lengths[i])
                : 0;
    }
}

static inline void write_code(
    TinyBitWriter* bits, const TinyCodes* codes, uint32_t symbol
) {
    tiny_bitwriter_lsb(bits, codes->code[symbol], codes->length[symbol]);
}

/** Extra bits each code length repeat symbol carries. */
static inline uint32_t repeat_extra(uint32_t symbol) {
    if (symbol == 16) return 2;
    if (symbol == 17) return 3;
    if (symbol == 18) return 7;
    return 0;
}

/**
 * Rewrites a run of code lengths using the repeat symbols 16, 17 and 18.
 *
 * @return uint32_t How many entries were written.
 */
static uint32_t rle_lengths(
    const uint8_t* lengths, uint32_t n, uint8_t* symbols, uint8_t* extras
) {
    uint32_t out = 0;
    uint32_t i = 0;

    while (i < n) {
        uint8_t value = lengths[i];
        uint32_t run = 1;

        while (i + run < n && lengths[i + run] == value) {
            run++;
        }

        if (value == 0) {
            while (run >= 11) {
                uint32_t take = run > 138 ? 138 : run;
                symbols[out] = 18;
                extras[out] = (uint8_t) (take - 11);
                out++;
                run -= take;
                i += take;
            }
            while (run >= 3) {
                uint32_t take = run > 10 ? 10 : run;
                symbols[out] = 17;
                extras[out] = (uint8_t) (take - 3);
                out++;
                run -= take;
                i += take;
            }
        }
        else {
            // the value has to be written once before a repeat can refer to it
            symbols[out] = value;
            extras[out] = 0;
            out++;
            i++;
            run--;

            while (run >= 3) {
                uint32_t take = run > 6 ? 6 : run;
                symbols[out] = 16;
                extras[out] = (uint8_t) (take - 3);
                out++;
                run -= take;
                i += take;
            }
        }

        while (run > 0) {
            symbols[out] = value;
            extras[out] = 0;
            out++;
            run--;
            i++;
        }
    }

    return out;
}

static void emit_stored(
    TinyDeflateState* d, size_t start, size_t end, int final
) {
    size_t left = end - start;

    do {
        size_t take = left > 65535 ? 65535 : left;
        int last = final && take == left;

        tiny_bitwriter_lsb(&d->bits, (uint32_t) (last ? 1 : 0), 1);
        tiny_bitwriter_lsb(&d->bits, 0, 2);
        tiny_bitwriter_flush_lsb(&d->bits);

        tiny_writer_le16(d->out, (uint16_t) take);
        tiny_writer_le16(d->out, (uint16_t) ~(uint16_t) take);
        tiny_writer_write(d->out, d->data + start, take);

        start += take;
        left -= take;
    } while (left > 0);
}

/** What this block's symbols would cost under a given pair of code length
 * tables. */
static uint64_t symbol_cost(
    const TinyDeflateState* d, const uint8_t* literal_lengths,
    const uint8_t* distance_lengths
) {
    uint64_t bits = literal_lengths[256];

    for (uint32_t i = 0; i < d->count; i++) {
        if (d->distances[i] == 0) {
            bits += literal_lengths[d->symbols[i]];
            continue;
        }

        uint32_t lc = length_code(d->symbols[i]);
        uint32_t dc = distance_code(d->distances[i]);

        bits += literal_lengths[257 + lc] + length_extra[lc];
        bits += distance_lengths[dc] + distance_extra[dc];
    }

    return bits;
}

// RFC 1951 defines the fixed alphabet over 288 symbols, and the last two matter
// even though no stream can use them: canonical codes are assigned from the
// count of codes at each length, so leaving 286 and 287 out shifts the 9 bit
// codes down by two and every literal from 144 up gets the wrong one. Nothing
// catches that until a fixed block happens to carry a high literal
static void fixed_lengths(uint8_t* literals, uint8_t* distances) {
    for (uint32_t i = 0; i < 144; i++) literals[i] = 8;
    for (uint32_t i = 144; i < 256; i++) literals[i] = 9;
    for (uint32_t i = 256; i < 280; i++) literals[i] = 7;
    for (uint32_t i = 280; i < 288; i++) literals[i] = 8;
    for (uint32_t i = 0; i < 30; i++) distances[i] = 5;
}

static void emit_symbols(
    TinyDeflateState* d, const TinyCodes* literals, const TinyCodes* distances
) {
    for (uint32_t i = 0; i < d->count; i++) {
        if (d->distances[i] == 0) {
            write_code(&d->bits, literals, d->symbols[i]);
            continue;
        }

        uint32_t length = d->symbols[i];
        uint32_t lc = length_code(length);

        write_code(&d->bits, literals, 257 + lc);
        if (length_extra[lc] > 0) {
            tiny_bitwriter_lsb(
                &d->bits, length - length_base[lc], length_extra[lc]
            );
        }

        uint32_t distance = d->distances[i];
        uint32_t dc = distance_code(distance);

        write_code(&d->bits, distances, dc);
        if (distance_extra[dc] > 0) {
            tiny_bitwriter_lsb(
                &d->bits, distance - distance_base[dc], distance_extra[dc]
            );
        }
    }

    write_code(&d->bits, literals, 256);
}

static void emit_block(TinyDeflateState* d, size_t end, int final) {
    d->literal_freq[256]++;

    uint8_t literal_lengths[286];
    uint8_t distance_lengths[30];

    // a stored block carries no tables at all, so it is what this falls back to
    // when the scratch to build them cannot be had
    if (tiny_huffman_lengths(d->literal_freq, 286, 15, literal_lengths) !=
            TINYIMG_OK ||
        tiny_huffman_lengths(d->distance_freq, 30, 15, distance_lengths) !=
            TINYIMG_OK) {
        emit_stored(d, d->block_start, end, final);
        return;
    }

    uint32_t hlit = 286;
    while (hlit > 257 && literal_lengths[hlit - 1] == 0) {
        hlit--;
    }

    uint32_t hdist = 30;
    while (hdist > 1 && distance_lengths[hdist - 1] == 0) {
        hdist--;
    }

    uint8_t combined[316];
    for (uint32_t i = 0; i < hlit; i++) {
        combined[i] = literal_lengths[i];
    }
    for (uint32_t i = 0; i < hdist; i++) {
        combined[hlit + i] = distance_lengths[i];
    }

    uint8_t repeat_symbols[320];
    uint8_t repeat_extras[320];
    uint32_t repeats =
        rle_lengths(combined, hlit + hdist, repeat_symbols, repeat_extras);

    uint32_t code_freq[19];
    tiny_memset(code_freq, 0, sizeof(code_freq));
    for (uint32_t i = 0; i < repeats; i++) {
        code_freq[repeat_symbols[i]]++;
    }

    uint8_t code_lengths[19];

    if (tiny_huffman_lengths(code_freq, 19, 7, code_lengths) != TINYIMG_OK) {
        emit_stored(d, d->block_start, end, final);
        return;
    }

    uint32_t hclen = 19;
    while (hclen > 4 && code_lengths[length_order[hclen - 1]] == 0) {
        hclen--;
    }

    TinyCodes literals;
    TinyCodes distances;
    TinyCodes code_codes;

    codes_from_lengths(&literals, literal_lengths, 286);
    codes_from_lengths(&distances, distance_lengths, 30);
    codes_from_lengths(&code_codes, code_lengths, 19);

    // all three forms are costed before anything is written. the fixed tables
    // win on small blocks, where the code length table costs more than it
    // saves, and a stored block wins on incompressible input, which can then
    // never grow by more than its own header
    uint64_t dynamic_bits = 3 + 5 + 5 + 4 + 3 * (uint64_t) hclen;

    for (uint32_t i = 0; i < repeats; i++) {
        dynamic_bits += code_lengths[repeat_symbols[i]];
        dynamic_bits += repeat_extra(repeat_symbols[i]);
    }
    dynamic_bits += symbol_cost(d, literal_lengths, distance_lengths);

    uint8_t fixed_literal[288];
    uint8_t fixed_distance[30];
    fixed_lengths(fixed_literal, fixed_distance);

    uint64_t fixed_bits = 3 + symbol_cost(d, fixed_literal, fixed_distance);

    size_t span = end - d->block_start;
    uint64_t stored_bits = 8 * (uint64_t) span + 40 * ((span / 65535) + 1);

    if (stored_bits <= dynamic_bits && stored_bits <= fixed_bits) {
        emit_stored(d, d->block_start, end, final);
        return;
    }

    tiny_bitwriter_lsb(&d->bits, (uint32_t) (final ? 1 : 0), 1);

    if (fixed_bits < dynamic_bits) {
        tiny_bitwriter_lsb(&d->bits, 1, 2);

        TinyCodes fixed_literals;
        TinyCodes fixed_distances;
        codes_from_lengths(&fixed_literals, fixed_literal, 288);
        codes_from_lengths(&fixed_distances, fixed_distance, 30);

        emit_symbols(d, &fixed_literals, &fixed_distances);
        return;
    }

    tiny_bitwriter_lsb(&d->bits, 2, 2);
    tiny_bitwriter_lsb(&d->bits, hlit - 257, 5);
    tiny_bitwriter_lsb(&d->bits, hdist - 1, 5);
    tiny_bitwriter_lsb(&d->bits, hclen - 4, 4);

    for (uint32_t i = 0; i < hclen; i++) {
        tiny_bitwriter_lsb(&d->bits, code_lengths[length_order[i]], 3);
    }

    for (uint32_t i = 0; i < repeats; i++) {
        write_code(&d->bits, &code_codes, repeat_symbols[i]);

        uint32_t extra = repeat_extra(repeat_symbols[i]);
        if (extra > 0) tiny_bitwriter_lsb(&d->bits, repeat_extras[i], extra);
    }

    emit_symbols(d, &literals, &distances);
}

static void flush_block(TinyDeflateState* d, size_t end, int final) {
    emit_block(d, end, final);

    d->count = 0;
    d->block_start = end;
    tiny_memset(d->literal_freq, 0, sizeof(d->literal_freq));
    tiny_memset(d->distance_freq, 0, sizeof(d->distance_freq));
}

static inline uint32_t hash3(const uint8_t* p) {
    return (((uint32_t) p[0] << 10) ^ ((uint32_t) p[1] << 5) ^
            (uint32_t) p[2]) &
           (DEFLATE_HASH_SIZE - 1);
}

static void insert_position(TinyDeflateState* d, size_t pos) {
    if (pos + DEFLATE_MIN_MATCH > d->size) return;

    uint32_t h = hash3(d->data + pos);
    d->chain[pos & (TINY_DEFLATE_WINDOW - 1)] = d->head[h];
    d->head[h] = (int32_t) pos;
}

static uint32_t find_match(
    TinyDeflateState* d, size_t pos, uint32_t* out_distance
) {
    if (pos + DEFLATE_MIN_MATCH > d->size) return 0;

    size_t limit = d->size - pos;
    if (limit > DEFLATE_MAX_MATCH) limit = DEFLATE_MAX_MATCH;

    int32_t candidate = d->head[hash3(d->data + pos)];
    uint32_t best = 0;
    uint32_t best_distance = 0;
    uint32_t probes = d->chain_limit;

    while (candidate >= 0 && probes-- > 0) {
        size_t at = (size_t) candidate;
        size_t distance = pos - at;

        if (distance == 0 || distance > TINY_DEFLATE_WINDOW) break;

        uint32_t length = 0;
        while (length < limit &&
               d->data[at + length] == d->data[pos + length]) {
            length++;
        }

        if (length > best) {
            best = length;
            best_distance = (uint32_t) distance;
            if (length >= limit) break;
        }

        candidate = d->chain[at & (TINY_DEFLATE_WINDOW - 1)];
    }

    if (best < DEFLATE_MIN_MATCH) return 0;

    // a match is only worth taking if it codes for fewer bits than the bytes it
    // replaces. the code lengths are not known until the block is Huffman
    // coded, so this estimates them at nine bits for a length and five for a
    // distance and adds the extra bits, which are exact. without this a short
    // match at a long distance is accepted and compresses worse than not
    // matching at all
    uint32_t cost = 9 + length_extra[length_code(best)] + 5 +
                    distance_extra[distance_code(best_distance)];

    if (cost >= best * 8) return 0;

    *out_distance = best_distance;
    return best;
}

static inline void push_literal(TinyDeflateState* d, uint8_t byte) {
    d->symbols[d->count] = byte;
    d->distances[d->count] = 0;
    d->count++;
    d->literal_freq[byte]++;
}

static inline void push_match(
    TinyDeflateState* d, uint32_t length, uint32_t distance
) {
    d->symbols[d->count] = (uint16_t) length;
    d->distances[d->count] = (uint16_t) distance;
    d->count++;
    d->literal_freq[257 + length_code(length)]++;
    d->distance_freq[distance_code(distance)]++;
}

int tiny_deflate(
    const uint8_t* data, size_t size, TinyDeflateLevel level, int zlib,
    TinyWriter* out
) {
    if (!out) return TINYIMG_ERR_NULL;
    if (!data && size > 0) return TINYIMG_ERR_NULL;

    if (zlib) {
        // method 8, 32 KiB window, no preset dictionary; 0x78 0x01 is a
        // multiple of 31
        tiny_writer_u8(out, 0x78);
        tiny_writer_u8(out, 0x01);
    }

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    TinyDeflateState d;
    tiny_memset(&d, 0, sizeof(d));

    d.data = data;
    d.size = size;
    d.out = out;

    switch (level) {
        case TINYIMG_DEFLATE_HUFFMAN:
            d.chain_limit = 0;
            d.lazy = 0;
            break;
        case TINYIMG_DEFLATE_FAST:
            d.chain_limit = 1;
            d.lazy = 0;
            break;
        case TINYIMG_DEFLATE_BEST:
            d.chain_limit = 512;
            d.lazy = 1;
            break;
        default:
            d.chain_limit = 32;
            d.lazy = 1;
            break;
    }

    d.head = tiny_arena_alloc(DEFLATE_HASH_SIZE * sizeof(int32_t), 0);
    d.chain = tiny_arena_alloc(TINY_DEFLATE_WINDOW * sizeof(int32_t), 0);
    d.symbols = tiny_arena_alloc(DEFLATE_BLOCK * sizeof(uint16_t), 0);
    d.distances = tiny_arena_alloc(DEFLATE_BLOCK * sizeof(uint16_t), 0);

    if (!d.head || !d.chain || !d.symbols || !d.distances) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (uint32_t i = 0; i < DEFLATE_HASH_SIZE; i++) {
        d.head[i] = -1;
    }

    tiny_bitwriter_init(&d.bits, out);

    while (d.pos < size) {
        uint32_t distance = 0;
        uint32_t length =
            d.chain_limit > 0 ? find_match(&d, d.pos, &distance) : 0;

        // a longer match one byte later is worth a literal here, which is most
        // of what separates this from a greedy pass
        if (length >= DEFLATE_MIN_MATCH && d.lazy &&
            d.pos + 1 + DEFLATE_MIN_MATCH <= size) {
            uint32_t ahead_distance = 0;
            uint32_t ahead = find_match(&d, d.pos + 1, &ahead_distance);

            if (ahead > length) {
                push_literal(&d, data[d.pos]);
                insert_position(&d, d.pos);
                d.pos++;

                if (d.count >= DEFLATE_BLOCK) flush_block(&d, d.pos, 0);
                continue;
            }
        }

        if (length >= DEFLATE_MIN_MATCH) {
            push_match(&d, length, distance);

            for (uint32_t i = 0; i < length; i++) {
                insert_position(&d, d.pos + i);
            }
            d.pos += length;
        }
        else {
            push_literal(&d, data[d.pos]);
            insert_position(&d, d.pos);
            d.pos++;
        }

        if (d.count >= DEFLATE_BLOCK) flush_block(&d, d.pos, 0);
    }

    // always one final block, even for empty input
    flush_block(&d, size, 1);
    tiny_bitwriter_flush_lsb(&d.bits);

    if (zlib) {
        uint32_t adler = tiny_adler32(1, data, size);
        tiny_writer_be32(out, adler);
    }

    tiny_arena_release(&mark);
    return out->error;
}

#pragma endregion
