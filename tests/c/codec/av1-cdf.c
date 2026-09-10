#include "codec/av1.h"

#include "codec/av1-tables.h"
#include "test.h"
#include "tinyimg/memory.h"

/**
 * The mutable distributions a tile adapts.
 *
 * Three properties, and only the third is obvious. A reset has to reproduce the
 * defaults exactly, two tiles have to start from the same state, and **reading
 * a symbol must not touch the `const` tables the copies came from**. The last
 * one is what a struct assignment or a stray pointer would break, and it would
 * break it silently: the first tile would decode correctly and every later one
 * would start from the first one's adapted state.
 *
 * The sizes are already checked at compile time by 70 static assertions in
 * `av1-cdf.c`, so nothing here re-checks them.
 */

static int resetsToTheDefaults(void) {
    int r = 0;

    TinyAv1Cdf cdf;

    // filled with a pattern first, so a member the reset forgets shows up as
    // the pattern rather than as a plausible probability
    tiny_memset(&cdf, 0xA5, sizeof(cdf));
    tiny_av1_cdf_init(&cdf);

    r |= assertEquals(
        tiny_memcmp(
            cdf.intra_frame_y_mode, tiny_av1_default_intra_frame_y_mode_cdf,
            sizeof(cdf.intra_frame_y_mode)
        ),
        0
    );

    r |= assertEquals(
        tiny_memcmp(
            cdf.partition_w128, tiny_av1_default_partition_w128_cdf,
            sizeof(cdf.partition_w128)
        ),
        0
    );

    // the coefficient distributions are the largest and the ones a wrong size
    // would truncate
    r |= assertEquals(
        tiny_memcmp(
            cdf.coeff_base, tiny_av1_default_coeff_base_cdf,
            sizeof(cdf.coeff_base)
        ),
        0
    );

    r |= assertEquals(
        tiny_memcmp(
            cdf.coeff_br, tiny_av1_default_coeff_br_cdf, sizeof(cdf.coeff_br)
        ),
        0
    );

    r |= assertEquals(
        tiny_memcmp(
            cdf.eob_pt_1024, tiny_av1_default_eob_pt_1024_cdf,
            sizeof(cdf.eob_pt_1024)
        ),
        0
    );

    // four independent copies of one default, which is the entry easiest to
    // mistake for a single copy
    for (uint32_t i = 0; i < 4u; i++) {
        r |= assertEquals(
            tiny_memcmp(
                cdf.delta_lf_multi[i], tiny_av1_default_delta_lf_cdf,
                sizeof(cdf.delta_lf_multi[i])
            ),
            0
        );
    }

    // nothing anywhere in the structure is still the fill pattern
    const uint8_t* raw = (const uint8_t*) &cdf;
    size_t untouched = 0;

    for (size_t i = 0; i + 1 < sizeof(cdf); i++) {
        if (raw[i] == 0xA5u && raw[i + 1] == 0xA5u) untouched++;
    }

    r |= assertEquals((long) untouched, 0L);

    return r;
}

static int twoTilesStartTheSame(void) {
    int r = 0;

    TinyAv1Cdf first;
    TinyAv1Cdf second;

    tiny_av1_cdf_init(&first);

    // adapt the first one, the way a tile would
    const uint8_t data[8] = {0x37, 0x91, 0xC2, 0x5A, 0x0F, 0xE3, 0x71, 0x88};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    for (int i = 0; i < 24; i++) {
        (void) tiny_av1_symbol_read(&symbol, first.partition_w8[0], 4);
    }

    r |= assertNotEquals(
        (long) tiny_memcmp(
            first.partition_w8, tiny_av1_default_partition_w8_cdf,
            sizeof(first.partition_w8)
        ),
        0L
    );

    // a second tile resets, so it starts where the first one did rather than
    // where the first one finished
    tiny_av1_cdf_init(&second);

    r |= assertEquals(
        tiny_memcmp(
            second.partition_w8, tiny_av1_default_partition_w8_cdf,
            sizeof(second.partition_w8)
        ),
        0
    );

    return r;
}

/**
 * Adapting a copy must leave the table it came from alone.
 *
 * The failure this catches is silent and compounding: if the copies aliased the
 * defaults, tile one would decode correctly and every later tile, and every
 * later image in the same isolate, would start from adapted state.
 */
static int leavesTheDefaultsAlone(void) {
    int r = 0;

    // a digest of the whole default block, taken before anything adapts
    uint32_t before = 0;
    const uint8_t* tables = (const uint8_t*) tiny_av1_default_coeff_base_cdf;

    for (size_t i = 0; i < sizeof(tiny_av1_default_coeff_base_cdf); i++) {
        before = before * 16777619u + tables[i];
    }

    TinyAv1Cdf cdf;
    tiny_av1_cdf_init(&cdf);

    const uint8_t data[16] = {0x4C, 0x2A, 0xFF, 0x03, 0x91, 0x7E, 0x18, 0xB5,
                              0x60, 0xD2, 0x0B, 0xA7, 0x35, 0xEE, 0x52, 0xC9};
    TinyAv1Symbol symbol;

    tiny_av1_symbol_init(&symbol, data, sizeof(data));

    for (int i = 0; i < 40; i++) {
        (void) tiny_av1_symbol_read(&symbol, cdf.coeff_base[0][0][0][0], 4);
    }

    uint32_t after = 0;

    for (size_t i = 0; i < sizeof(tiny_av1_default_coeff_base_cdf); i++) {
        after = after * 16777619u + tables[i];
    }

    r |= assertEquals((long) after, (long) before);

    // and the copy did move, or the check above proves nothing
    r |= assertNotEquals(
        (long) tiny_memcmp(
            cdf.coeff_base, tiny_av1_default_coeff_base_cdf,
            sizeof(cdf.coeff_base)
        ),
        0L
    );

    return r;
}

int main(void) {
    int r = 0;

    r |= resetsToTheDefaults();
    r |= twoTilesStartTheSame();
    r |= leavesTheDefaultsAlone();

    // a null context is ignored rather than crashing, which is the contract
    tiny_av1_cdf_init(0);

    return r;
}
