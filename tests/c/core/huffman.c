#include "tinyimg/util.h"

#include "test.h"

/**
 * @brief Whether a set of lengths is a complete prefix code.
 *
 * The Kraft sum of a code that leaves no bit pattern unassigned and no pattern
 * assigned twice is exactly one. Computed in 2^-limit units so it is integer
 * arithmetic: every length contributes 2^(limit - length).
 *
 * This is the property a decoder actually checks, so it is the one worth
 * asserting. Lengths can be optimal, respect the limit, and still be refused
 * for failing it.
 *
 * @param lengths The code lengths.
 * @param count How many symbols there are.
 * @param limit The longest length allowed.
 * @return long The sum in 2^-limit units; a complete code gives 1 << limit.
 */
static long kraft(const uint8_t* lengths, uint32_t count, uint32_t limit) {
    long sum = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (lengths[i] == 0) continue;
        sum += 1L << (limit - lengths[i]);
    }

    return sum;
}

/** Total bits the code spends on the frequencies it was built from. */
static long cost(
    const uint32_t* frequencies, const uint8_t* lengths, uint32_t count
) {
    long bits = 0;

    for (uint32_t i = 0; i < count; i++) {
        bits += (long) frequencies[i] * lengths[i];
    }

    return bits;
}

/**
 * @brief Checks one distribution end to end.
 *
 * @param label What the distribution is, for the failure line.
 * @param frequencies The frequencies.
 * @param count How many symbols.
 * @param limit The longest length allowed.
 * @return int 0 when the code is complete, inside the limit, and covers exactly
 * the symbols that were used.
 */
static int check(
    const char* label, const uint32_t* frequencies, uint32_t count,
    uint32_t limit
) {
    uint8_t lengths[600];
    int failures = 0;

    if (count > sizeof(lengths)) return 1;

    int result = tiny_huffman_lengths(frequencies, count, limit, lengths);

    if (result != TINYIMG_OK) {
        printf("#%d: FAIL\n", count);
        printf("#%d: %s: builder returned %d\n", count, label, result);
        return 1;
    }

    uint32_t used = 0;
    uint32_t coded = 0;
    uint32_t longest = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (frequencies[i] > 0) used++;
        if (lengths[i] > 0) coded++;
        if (lengths[i] > longest) longest = lengths[i];

        // a symbol nothing used gets no code, and one that was used gets one
        if ((frequencies[i] > 0) != (lengths[i] > 0)) failures++;
    }

    failures += assertEquals(coded, used);
    failures += assertTrue(longest <= limit);

    if (used > 1) {
        failures += assertEquals(kraft(lengths, count, limit), 1L << limit);
    }
    else if (used == 1) {
        // one symbol cannot fill a code, and both formats know it
        failures += assertEquals(longest, 1);
    }

    if (failures != 0)
        printf("(%s, %u symbols, limit %u)\n", label, count, limit);
    return failures;
}

/** The distributions a real encoder hands it. */
static int distributions(void) {
    int failures = 0;
    uint32_t freq[600];

    // nothing used at all
    memset(freq, 0, sizeof(freq));
    uint8_t lengths[600];
    memset(lengths, 0xFF, sizeof(lengths));

    failures +=
        assertEquals(tiny_huffman_lengths(freq, 286, 15, lengths), TINYIMG_OK);
    for (uint32_t i = 0; i < 286u; i++) failures += assertEquals(lengths[i], 0);

    // one symbol
    memset(freq, 0, sizeof(freq));
    freq[7] = 100;
    failures += check("one symbol", freq, 286, 15);

    // two symbols, which is the smallest real code
    memset(freq, 0, sizeof(freq));
    freq[0] = 1;
    freq[1] = 1;
    failures += check("two equal", freq, 286, 15);

    memset(freq, 0, sizeof(freq));
    freq[0] = 1000000;
    freq[285] = 1;
    failures += check("two, far apart", freq, 286, 15);

    // uniform, where every code is the same length
    for (uint32_t i = 0; i < 256u; i++) freq[i] = 4;
    failures += check("uniform 256", freq, 256, 15);

    for (uint32_t i = 0; i < 19u; i++) freq[i] = 1;
    failures += check("uniform 19, limit 7", freq, 19, 7);

    /*
     * Fibonacci weights are the worst case for a Huffman tree: each node pairs
     * with the sum of everything below it, so the tree is a chain and the
     * natural depth is the symbol count. Every one of these has to come back
     * inside the limit, and the code still has to be complete afterwards, which
     * is exactly what a length limiter gets wrong.
     */
    uint32_t a = 1;
    uint32_t b = 1;
    for (uint32_t i = 0; i < 32u; i++) {
        freq[i] = a;
        uint32_t next = a + b;
        a = b;
        b = next;
    }
    failures += check("fibonacci 32, limit 15", freq, 32, 15);
    failures += check("fibonacci 32, limit 7", freq, 32, 7);

    for (uint32_t i = 0; i < 19u; i++) {
        freq[i] = i == 0 ? 1u : freq[i - 1] * 2u;
    }
    failures += check("powers of two, limit 7", freq, 19, 7);

    // a long tail of ones under a few heavy symbols, which is what a real
    // literal alphabet looks like
    memset(freq, 0, sizeof(freq));
    freq[0] = 50000;
    freq[1] = 20000;
    freq[2] = 9000;
    for (uint32_t i = 3; i < 286u; i++) freq[i] = 1;
    failures += check("heavy head, long tail", freq, 286, 15);

    // the largest alphabet in the library, which is WebP's green channel with a
    // full color cache
    for (uint32_t i = 0; i < 536u; i++) freq[i] = (i * 37u) % 101u;
    failures += check("536 symbols", freq, 536, 15);

    // one heavy symbol and a great many ones, which forces the limiter hardest
    memset(freq, 0, sizeof(freq));
    freq[0] = 1u << 30;
    for (uint32_t i = 1; i < 536u; i++) freq[i] = 1;
    failures += check("one huge, 535 ones", freq, 536, 15);

    return failures;
}

/** The lengths are not merely legal, they are the ones a Huffman code gives. */
static int optimality(void) {
    int failures = 0;

    /*
     * Four symbols at 1, 1, 2 and 4 build a known tree: the two ones pair at
     * depth 3, the two joins with them at depth 2, and the four sits at
     * depth 1. Any other complete code over these frequencies costs more bits.
     */
    uint32_t freq[4] = {4, 2, 1, 1};
    uint8_t lengths[4];

    failures +=
        assertEquals(tiny_huffman_lengths(freq, 4, 15, lengths), TINYIMG_OK);
    failures += assertEquals(lengths[0], 1);
    failures += assertEquals(lengths[1], 2);
    failures += assertEquals(lengths[2], 3);
    failures += assertEquals(lengths[3], 3);
    failures += assertEquals(cost(freq, lengths, 4), 14L);

    // eight equal symbols is a flat tree of depth three
    uint32_t flat[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t even[8];

    failures +=
        assertEquals(tiny_huffman_lengths(flat, 8, 15, even), TINYIMG_OK);
    for (uint32_t i = 0; i < 8u; i++) failures += assertEquals(even[i], 3);

    // a more frequent symbol never gets a longer code than a rarer one
    uint32_t sloped[16];
    uint8_t ordered[16];

    for (uint32_t i = 0; i < 16u; i++) sloped[i] = 16u - i;

    failures +=
        assertEquals(tiny_huffman_lengths(sloped, 16, 15, ordered), TINYIMG_OK);

    for (uint32_t i = 1; i < 16u; i++) {
        failures += assertTrue(ordered[i] >= ordered[i - 1]);
    }

    return failures;
}

/**
 * @brief A tighter limit costs bits and never validity.
 *
 * Which is the whole point of a length limited code: the encoder gives up some
 * compression so the decoder can use a table of a fixed depth. Every limit has
 * to produce a complete code inside it, and that part is not a tolerance.
 *
 * The cost is a tolerance, and deliberately. Redistributing the histogram is a
 * heuristic rather than the optimal package merge, so it is close to the best
 * code of a given depth without being it, and the closeness is not monotonic in
 * the limit: on these weights it costs more at seven bits than at six, by 0.5%.
 * An optimal limiter could not do that, and this asserts the bound the
 * heuristic actually holds to rather than one it does not.
 */
static int limits(void) {
    int failures = 0;
    uint32_t freq[64];

    uint32_t a = 1;
    uint32_t b = 1;
    for (uint32_t i = 0; i < 64u; i++) {
        freq[i] = a;
        uint32_t next = a + b;
        a = b;
        b = next;
    }

    uint8_t unlimited[64];
    failures +=
        assertEquals(tiny_huffman_lengths(freq, 64, 32, unlimited), TINYIMG_OK);

    double best = (double) cost(freq, unlimited, 64);

    for (uint32_t limit = 15; limit >= 6u; limit--) {
        uint8_t lengths[64];

        failures += assertEquals(
            tiny_huffman_lengths(freq, 64, limit, lengths), TINYIMG_OK
        );
        failures += assertEquals(kraft(lengths, 64, limit), 1L << limit);

        // fibonacci weights are the worst case there is: the tree wants a depth
        // of 63 and seven bits is a ninth of that. It costs 1.2013 times the
        // unbounded code, measured, and that is the whole price of the bound
        failures +=
            assertLessThan((double) cost(freq, lengths, 64), best * 1.25);
    }

    return failures;
}

static int arguments(void) {
    int failures = 0;

    uint32_t freq[4] = {1, 1, 1, 1};
    uint8_t lengths[4];

    failures +=
        assertEquals(tiny_huffman_lengths(0, 4, 15, lengths), TINYIMG_ERR_NULL);
    failures +=
        assertEquals(tiny_huffman_lengths(freq, 4, 15, 0), TINYIMG_ERR_NULL);
    failures += assertEquals(
        tiny_huffman_lengths(freq, 0, 15, lengths), TINYIMG_ERR_RANGE
    );
    failures += assertEquals(
        tiny_huffman_lengths(freq, 4, 0, lengths), TINYIMG_ERR_RANGE
    );

    return failures;
}

int main(void) {
    int failures = 0;

    failures += distributions();
    failures += optimality();
    failures += limits();
    failures += arguments();

    printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
