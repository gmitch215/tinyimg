#include "codec/av1.h"
#include "test.h"

/**
 * Three anchors worked out by hand from the specification's own arithmetic,
 * before any of this code ran.
 *
 * The symbol decoder cannot be checked against itself: an encoder written here
 * to invert it would share every misreading, which is the mistake the ICC tone
 * curves already made in this repository. So the expected values below were
 * derived by stepping section 9.2's ordered steps on paper for a chosen input,
 * and the state after each read is asserted whole, not just the symbol. A wrong
 * intermediate shift changes the state without changing the first symbol.
 */

/** An all-zero tile, which the initialization turns into the maximum value. */
static int zeroTile(void) {
    int r = 0;

    const uint8_t data[2] = {0x00, 0x00};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    // numBits = min(16, 15) = 15, buf = 0, so value = 32767 ^ 0
    r |= assertEquals((long) symbol.value, 32767L);
    r |= assertEquals((long) symbol.range, 32768L);
    r |= assertEquals((long) symbol.max_bits, 1L);

    // an even distribution: cur lands at 16388 and 32767 is above it, so the
    // first interval wins
    r |= assertEquals((long) tiny_av1_symbol_bit(&symbol), 0L);
    r |= assertEquals((long) symbol.range, 65520L);
    r |= assertEquals((long) symbol.value, 65519L);

    // two bits of renormalization against one real bit left, so the decode has
    // entered the padding the specification allows
    r |= assertEquals((long) symbol.max_bits, -1L);

    return r;
}

/** An all-ones tile, which turns into value zero and takes the other branch. */
static int onesTile(void) {
    int r = 0;

    const uint8_t data[2] = {0xFF, 0xFF};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    r |= assertEquals((long) symbol.value, 0L);
    r |= assertEquals((long) symbol.range, 32768L);

    r |= assertEquals((long) tiny_av1_symbol_bit(&symbol), 1L);
    r |= assertEquals((long) symbol.range, 32776L);
    r |= assertEquals((long) symbol.value, 0L);
    r |= assertEquals((long) symbol.max_bits, 0L);

    return r;
}

/**
 * A four symbol distribution, which is the case that exercises adaptation.
 *
 * The rate depends on the symbol count and on how many times the distribution
 * has been read, so a two symbol case cannot reach the `Min(FloorLog2(N), 2)`
 * term at all.
 */
static int adaptation(void) {
    int r = 0;

    const uint8_t data[4] = {0x00, 0x00, 0x00, 0x00};
    uint16_t cdf[5] = {8192, 16384, 24576, 32768, 0};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));
    r |= assertEquals((long) symbol.max_bits, 17L);

    r |= assertEquals((long) tiny_av1_symbol_read(&symbol, cdf, 4), 0L);
    r |= assertEquals((long) symbol.range, 65440L);
    r |= assertEquals((long) symbol.value, 65439L);
    r |= assertEquals((long) symbol.max_bits, 14L);

    // rate is 3 + 0 + 0 + min(FloorLog2(4), 2) = 5, and symbol 0 pulls every
    // entry from its index up toward one
    r |= assertEquals((long) cdf[0], 8960L);
    r |= assertEquals((long) cdf[1], 16896L);
    r |= assertEquals((long) cdf[2], 24832L);

    // the last cumulative value is fixed at one by definition and the entry
    // past it is a decode counter rather than a probability
    r |= assertEquals((long) cdf[3], 32768L);
    r |= assertEquals((long) cdf[4], 1L);

    return r;
}

/** A frozen decoder reads the same symbols and leaves the distribution alone.
 */
static int frozen(void) {
    int r = 0;

    const uint8_t data[4] = {0x37, 0x91, 0xC2, 0x5A};
    uint16_t adapting[5] = {8192, 16384, 24576, 32768, 0};
    uint16_t fixed[5] = {8192, 16384, 24576, 32768, 0};

    TinyAv1Symbol a;
    TinyAv1Symbol b;

    tiny_av1_symbol_init(&a, data, sizeof(data));
    tiny_av1_symbol_init(&b, data, sizeof(data));
    b.frozen = 1;

    // the first read cannot differ, because adaptation happens after it
    r |= assertEquals(
        (long) tiny_av1_symbol_read(&a, adapting, 4),
        (long) tiny_av1_symbol_read(&b, fixed, 4)
    );

    r |= assertNotEquals((long) adapting[0], 8192L);
    r |= assertEquals((long) fixed[0], 8192L);
    r |= assertEquals((long) fixed[4], 0L);

    return r;
}

/**
 * The invariants the coder maintains, over a long run of real-looking bytes.
 *
 * These are weaker than the hand computed anchors and they cover what the
 * anchors cannot: that nothing drifts out of range over thousands of symbols.
 */
static int invariants(void) {
    int r = 0;

    uint8_t data[4096];

    // a cheap deterministic fill, so a failure is reproducible
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < sizeof(data); i++) {
        state = state * 1664525u + 1013904223u;
        data[i] = (uint8_t) (state >> 24);
    }

    uint16_t cdf[5] = {8192, 16384, 24576, 32768, 0};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    int ranged = 1;
    int bounded = 1;
    int ordered = 1;
    uint32_t seen[4] = {0, 0, 0, 0};

    for (int i = 0; i < 8000; i++) {
        uint32_t value = tiny_av1_symbol_read(&symbol, cdf, 4);

        if (value >= 4u)
            bounded = 0;
        else
            seen[value]++;

        // renormalization leaves the range in the top half of sixteen bits,
        // and the value is always inside it
        if (symbol.range < 32768u || symbol.range > 65535u) ranged = 0;
        if (symbol.value >= symbol.range) ranged = 0;

        // the distribution stays monotone and pinned at one, or every later
        // interval computation is nonsense
        for (int k = 1; k < 4; k++) {
            if (cdf[k] < cdf[k - 1]) ordered = 0;
        }

        if (cdf[3] != 32768u) ordered = 0;
        if (cdf[4] > 32u) ordered = 0;
    }

    r |= assertTrue(bounded);
    r |= assertTrue(ranged);
    r |= assertTrue(ordered);

    // every symbol of a distribution this wide should appear over 8000 draws;
    // a decoder stuck on one branch is the failure this catches
    for (int i = 0; i < 4; i++) r |= assertGreaterThan((double) seen[i], 0.0);

    return r;
}

/**
 * A tile shorter than the fifteen bits initialization wants.
 *
 * The specification's f(n) reads past the end as zeros rather than failing, so
 * a one byte tile has to initialize and decode rather than trap.
 */
static int shortTile(void) {
    int r = 0;

    const uint8_t data[1] = {0x80};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    // numBits = min(8, 15) = 8, buf = 0x80, padded = 0x80 << 7 = 16384
    r |= assertEquals((long) symbol.value, 32767L ^ 16384L);
    r |= assertEquals((long) symbol.max_bits, -7L);

    // it must produce symbols out of padding instead of reading out of bounds
    for (int i = 0; i < 64; i++) (void) tiny_av1_symbol_bit(&symbol);

    r |= assertTrue(symbol.range >= 32768u && symbol.range <= 65535u);
    r |= assertTrue(symbol.value < symbol.range);

    return r;
}

/** A literal is bits read most significant first against even distributions. */
static int literals(void) {
    int r = 0;

    const uint8_t data[8] = {0x9E, 0x3C, 0x71, 0xF0, 0x0D, 0xB4, 0x22, 0x68};

    TinyAv1Symbol whole;
    TinyAv1Symbol split;

    tiny_av1_symbol_init(&whole, data, sizeof(data));
    tiny_av1_symbol_init(&split, data, sizeof(data));

    uint32_t four = tiny_av1_symbol_literal(&whole, 4);

    uint32_t high = tiny_av1_symbol_bit(&split);
    uint32_t next = tiny_av1_symbol_bit(&split);
    uint32_t third = tiny_av1_symbol_bit(&split);
    uint32_t low = tiny_av1_symbol_bit(&split);

    // four bits at once has to equal four bits one at a time, most significant
    // first, or the shift in read_literal is the wrong way round
    r |= assertEquals(
        (long) four, (long) ((high << 3) | (next << 2) | (third << 1) | low)
    );

    r |= assertEquals((long) whole.value, (long) split.value);
    r |= assertEquals((long) whole.range, (long) split.range);
    r |= assertEquals((long) whole.max_bits, (long) split.max_bits);

    r |= assertTrue(four <= 15u);

    return r;
}

int main(void) {
    int r = 0;

    r |= zeroTile();
    r |= onesTile();
    r |= adaptation();
    r |= frozen();
    r |= invariants();
    r |= shortTile();
    r |= literals();

    return r;
}
