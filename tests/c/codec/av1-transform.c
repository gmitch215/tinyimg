#include "codec/av1.h"
#include "test.h"

/**
 * Declared here because `av1.h` has no entry for it; see the note on the
 * definition in `src/codec/av1-transform.c`.
 */
int tiny_av1_inverse_wht(int32_t* block, uint32_t stride, uint32_t bit_depth);

/**
 * The transform cannot be checked against itself, and there is no forward
 * transform here to invert it with; an invertible pair written from one reading
 * of the specification shares every misreading of it, which is how this
 * repository shipped inverted ICC tone curves that every self-consistent check
 * passed.
 *
 * So everything below is anchored outside the implementation:
 *
 *   - Tx_Width and Tx_Height are transcribed from section 10 rather than read
 *     out of the generated `av1-tables.h`, so a wrong table index shows up.
 *   - The DC only values were worked out on paper from 7.13.2.3 and 7.13.3
 *     before the code ran. A DC only inverse DCT is flat at Round2(2896 * dc,
 *     12) at every length, because the only nonzero index after the bit
 *     reversal is 0 and every doubling stage pairs the low half against a zero
 *     high half.
 *   - IDTX is asserted against the whole pipeline written out longhand from the
 *     four identity processes, with no butterfly arithmetic in it at all.
 *   - The float reference is the closed form of each 1D transform from its
 *     mathematical definition, not from the integer network.
 *
 * The integer network is the contract, so the float reference is a structural
 * check and never the primary one: a tolerance cannot see an off by one shift.
 */

#define AV1_PI 3.14159265358979323846

/** Tx_Width and Tx_Height, section 10. */
static const uint32_t txWidth[19] = {4,  8,  16, 32, 64, 4, 8,  8,  16, 16,
                                     32, 32, 64, 4,  16, 8, 32, 16, 64};
static const uint32_t txHeight[19] = {4,  8,  16, 32, 64, 8,  4, 16, 8, 32,
                                      16, 64, 32, 16, 4,  32, 8, 64, 16};

/** Transform_Row_Shift, 7.13.3. */
static const uint32_t rowShift[19] = {0, 1, 2, 2, 2, 0, 0, 1, 1, 1,
                                      1, 1, 1, 1, 1, 2, 2, 2, 2};

/** How many rows and columns a stride carries in every buffer below. */
#define AV1_TEST_STRIDE 96

static uint32_t log2Of(uint32_t v) {
    uint32_t n = 0;

    while ((1u << n) < v) n++;

    return n;
}

static int32_t round2(int64_t x, uint32_t n) {
    if (n == 0) return (int32_t) x;

    return (int32_t) ((x + ((int64_t) 1 << (n - 1))) >> n);
}

static uint32_t rngState = 0x1234567u;

static int32_t nextCoeff(int32_t bound) {
    rngState = rngState * 1103515245u + 12345u;

    return (int32_t) ((rngState >> 9) % (uint32_t) (2 * bound + 1)) - bound;
}

/** Fills the top left 32x32 of a block, which is all a transform reads. */
static void fillCoeffs(int32_t* block, uint32_t size, int32_t bound) {
    uint32_t w = txWidth[size] < 32 ? txWidth[size] : 32;
    uint32_t h = txHeight[size] < 32 ? txHeight[size] : 32;

    for (uint32_t i = 0; i < 64; i++) {
        for (uint32_t j = 0; j < AV1_TEST_STRIDE; j++) {
            block[i * AV1_TEST_STRIDE + j] = 0;
        }
    }

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            block[i * AV1_TEST_STRIDE + j] = nextCoeff(bound);
        }
    }
}

// #region exhaustive sweep

/**
 * Every size against every type: zero in has to give zero out, and the pairs
 * 7.13.2 gives no value for have to be refused without touching the block.
 *
 * The counts are asserted so that widening or narrowing the accepted set is a
 * failure rather than a silent change. 111 of the 304 pairs need an ADST longer
 * than 16 or an identity longer than 32.
 */
static int sweep(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];
    int accepted = 0;
    int rejected = 0;
    int zeroFailures = 0;
    int touched = 0;
    int wrongError = 0;

    for (uint32_t size = 0; size < 19; size++) {
        for (uint32_t type = 0; type < 16; type++) {
            for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
                block[i] = 0x5A5A5A;
            }

            for (uint32_t i = 0; i < txHeight[size]; i++) {
                for (uint32_t j = 0; j < txWidth[size]; j++) {
                    block[i * AV1_TEST_STRIDE + j] = 0;
                }
            }

            int rc = tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size,
                (TinyAv1TxType) type, 8
            );

            if (rc == TINYIMG_OK) {
                accepted++;

                for (uint32_t i = 0; i < txHeight[size]; i++) {
                    for (uint32_t j = 0; j < txWidth[size]; j++) {
                        if (block[i * AV1_TEST_STRIDE + j] != 0) {
                            zeroFailures++;
                        }
                    }
                }
            }
            else {
                rejected++;

                if (rc != TINYIMG_ERR_CORRUPT) wrongError++;

                for (uint32_t i = 0; i < txHeight[size]; i++) {
                    for (uint32_t j = 0; j < txWidth[size]; j++) {
                        if (block[i * AV1_TEST_STRIDE + j] != 0) touched++;
                    }
                }
            }

            for (uint32_t i = 0; i < 64; i++) {
                for (uint32_t j = txWidth[size]; j < AV1_TEST_STRIDE; j++) {
                    if (block[i * AV1_TEST_STRIDE + j] != 0x5A5A5A) touched++;
                }
            }
        }
    }

    r |= assertEquals(accepted, 193);
    r |= assertEquals(rejected, 111);
    r |= assertEquals(zeroFailures, 0);
    r |= assertEquals(touched, 0);
    r |= assertEquals(wrongError, 0);

    return r;
}

// #region dc only

/**
 * A DC only DCT_DCT block, whose residual is flat at a value the shifts fix.
 *
 * Worked on paper for dc = 1024. The row pass gives Round2(2896 * 1024, 12) =
 * 724 on row 0 and zero elsewhere, then Round2(724, rowShift); the column pass
 * gives Round2(2896 * that, 12) and then Round2(, 4). For TX_4X4 that is
 * 724 -> 512 -> 32, for TX_8X8 362 -> 256 -> 16, for TX_16X16 and TX_32X32
 * 181 -> 128 -> 8. TX_4X8 also carries the rectangular 2896 prescale, which
 * takes 1024 to 724 before the row transform runs: 512 -> 362 -> 23.
 */
static int dcOnly(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];

    const uint32_t sizes[6] = {TINY_AV1_TX_4X4,   TINY_AV1_TX_8X8,
                               TINY_AV1_TX_16X16, TINY_AV1_TX_32X32,
                               TINY_AV1_TX_64X64, TINY_AV1_TX_4X8};
    const int32_t expect[6] = {32, 16, 8, 8, 8, 23};

    for (uint32_t k = 0; k < 6; k++) {
        uint32_t size = sizes[k];

        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            block[i] = 0;
        }
        block[0] = 1024;

        r |= assertEquals(
            tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size, TINY_AV1_DCT_DCT,
                8
            ),
            TINYIMG_OK
        );

        int flat = 1;

        for (uint32_t i = 0; i < txHeight[size]; i++) {
            for (uint32_t j = 0; j < txWidth[size]; j++) {
                if (block[i * AV1_TEST_STRIDE + j] != expect[k]) flat = 0;
            }
        }

        r |= assertEquals(block[0], expect[k]);
        r |= assertTrue(flat);
    }

    return r;
}

/**
 * H_DCT and V_DCT on a DC only block, which says which axis each one runs on.
 *
 * H_DCT is a DCT along the rows and an identity down the columns, so a lone DC
 * leaves row 0 flat and every other row zero; V_DCT is the transpose of that.
 * The value follows the same paper trail as dcOnly with the identity scale in
 * place of the second DCT: TX_4X4 gives 724 -> 1024 -> 64, TX_8X8 362 -> 724 ->
 * 45, TX_16X16 181 -> 512 -> 32, TX_32X32 181 -> 724 -> 45.
 */
static int oneAxis(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];

    const uint32_t sizes[4] = {
        TINY_AV1_TX_4X4, TINY_AV1_TX_8X8, TINY_AV1_TX_16X16, TINY_AV1_TX_32X32
    };
    const int32_t expect[4] = {64, 45, 32, 45};

    for (uint32_t k = 0; k < 4; k++) {
        uint32_t size = sizes[k];
        int wrong = 0;

        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            block[i] = 0;
        }
        block[0] = 1024;

        r |= assertEquals(
            tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size, TINY_AV1_H_DCT, 8
            ),
            TINYIMG_OK
        );

        for (uint32_t i = 0; i < txHeight[size]; i++) {
            for (uint32_t j = 0; j < txWidth[size]; j++) {
                int32_t want = i == 0 ? expect[k] : 0;

                if (block[i * AV1_TEST_STRIDE + j] != want) wrong++;
            }
        }

        r |= assertEquals(wrong, 0);

        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            block[i] = 0;
        }
        block[0] = 1024;
        wrong = 0;

        r |= assertEquals(
            tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size, TINY_AV1_V_DCT, 8
            ),
            TINYIMG_OK
        );

        for (uint32_t i = 0; i < txHeight[size]; i++) {
            for (uint32_t j = 0; j < txWidth[size]; j++) {
                int32_t want = j == 0 ? expect[k] : 0;

                if (block[i * AV1_TEST_STRIDE + j] != want) wrong++;
            }
        }

        r |= assertEquals(wrong, 0);
    }

    return r;
}

// #region identity

/** The four identity processes of 7.13.2.11 through 7.13.2.14, longhand. */
static int32_t identityScale(int32_t v, uint32_t n) {
    if (n == 2) return round2((int64_t) v * 5793, 12);
    if (n == 3) return (int32_t) ((int64_t) v * 2);
    if (n == 4) return round2((int64_t) v * 11586, 12);

    return (int32_t) ((int64_t) v * 4);
}

/**
 * IDTX against the whole 2D process written out with no butterflies in it.
 *
 * This is the one transform whose exact output can be stated without any
 * butterfly arithmetic, so it pins down the rectangular prescale, both shifts
 * and the clip between the passes on their own.
 */
static int identityExact(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];
    static int32_t input[64 * AV1_TEST_STRIDE];

    for (uint32_t size = 0; size < 19; size++) {
        if (txWidth[size] > 32 || txHeight[size] > 32) continue;

        uint32_t w = txWidth[size];
        uint32_t h = txHeight[size];
        uint32_t log2w = log2Of(w);
        uint32_t log2h = log2Of(h);
        uint32_t rect = (log2w > log2h ? log2w - log2h : log2h - log2w) == 1;
        int wrong = 0;

        fillCoeffs(input, size, 2000);

        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            block[i] = input[i];
        }

        r |= assertEquals(
            tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size, TINY_AV1_IDTX, 8
            ),
            TINYIMG_OK
        );

        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                int32_t v = input[i * AV1_TEST_STRIDE + j];

                if (rect) v = round2((int64_t) v * 2896, 12);

                v = round2(identityScale(v, log2w), rowShift[size]);
                v = round2(identityScale(v, log2h), 4);

                if (block[i * AV1_TEST_STRIDE + j] != v) wrong++;
            }
        }

        r |= assertEquals(wrong, 0);
    }

    return r;
}

// #region flips

/** Runs one transform over a copy of `input` and reports the return code. */
static int runOver(
    int32_t* out, const int32_t* input, uint32_t size, uint32_t type
) {
    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        out[i] = input[i];
    }

    return tiny_av1_inverse_transform(
        out, AV1_TEST_STRIDE, (TinyAv1TxSize) size, (TinyAv1TxType) type, 8
    );
}

/**
 * The flipped types against the unflipped ones they reverse.
 *
 * 7.12.3 derives flipUD from FLIPADST_DCT, FLIPADST_ADST, V_FLIPADST and
 * FLIPADST_FLIPADST and flipLR from DCT_FLIPADST, ADST_FLIPADST, H_FLIPADST and
 * FLIPADST_FLIPADST, and writes the residual to the mirrored position. The
 * inverse transform here does the mirroring instead, so the relationship is an
 * exact reversal of rows, of columns, or of both. Internal consistency rather
 * than an outside anchor, but it is what catches a flip on the wrong axis.
 */
static int flips(void) {
    int r = 0;
    static int32_t input[64 * AV1_TEST_STRIDE];
    static int32_t plain[64 * AV1_TEST_STRIDE];
    static int32_t flipped[64 * AV1_TEST_STRIDE];

    const uint32_t pairs[5][2] = {
        {TINY_AV1_ADST_DCT, TINY_AV1_FLIPADST_DCT},
        {TINY_AV1_DCT_ADST, TINY_AV1_DCT_FLIPADST},
        {TINY_AV1_ADST_ADST, TINY_AV1_FLIPADST_FLIPADST},
        {TINY_AV1_V_ADST, TINY_AV1_V_FLIPADST},
        {TINY_AV1_H_ADST, TINY_AV1_H_FLIPADST}
    };
    const uint32_t reverseRows[5] = {1, 0, 1, 1, 0};
    const uint32_t reverseCols[5] = {0, 1, 1, 0, 1};

    for (uint32_t size = 0; size < 19; size++) {
        for (uint32_t k = 0; k < 5; k++) {
            uint32_t w = txWidth[size];
            uint32_t h = txHeight[size];
            int wrong = 0;

            fillCoeffs(input, size, 700);

            if (runOver(plain, input, size, pairs[k][0]) != TINYIMG_OK) {
                r |= assertEquals(
                    runOver(flipped, input, size, pairs[k][1]),
                    TINYIMG_ERR_CORRUPT
                );
                continue;
            }

            r |= assertEquals(
                runOver(flipped, input, size, pairs[k][1]), TINYIMG_OK
            );

            for (uint32_t i = 0; i < h; i++) {
                for (uint32_t j = 0; j < w; j++) {
                    uint32_t si = reverseRows[k] ? h - 1 - i : i;
                    uint32_t sj = reverseCols[k] ? w - 1 - j : j;

                    if (flipped[i * AV1_TEST_STRIDE + j] !=
                        plain[si * AV1_TEST_STRIDE + sj]) {
                        wrong++;
                    }
                }
            }

            r |= assertEquals(wrong, 0);
        }
    }

    return r;
}

// #region float reference

/**
 * Every AV1 1D inverse transform is its orthonormal form scaled by sqrt(N / 2),
 * which the DC only anchor above fixes independently: a flat 2896 / 4096 is
 * 1 / sqrt(2), and sqrt(N / 2) / sqrt(N) is the same number at every length.
 * The identity scales agree with it too, being sqrt(2), 2, 2 sqrt(2) and 4.
 *
 * So the DCT is the unnormalized inverse DCT-III, and the ADST is DST-VII at
 * length 4 and DST-IV at 8 and 16.
 */
static void refDct(const double* in, double* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double sum = in[0] / sqrt(2.0);

        for (uint32_t k = 1; k < n; k++) {
            sum += in[k] * cos(AV1_PI * (2 * i + 1) * k / (2.0 * n));
        }

        out[i] = sum;
    }
}

static void refAdst(const double* in, double* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double sum = 0.0;

        for (uint32_t k = 0; k < n; k++) {
            if (n == 4) {
                sum += in[k] * sin(AV1_PI * (i + 1) * (2 * k + 1) / 9.0);
            }
            else {
                sum +=
                    in[k] * sin(AV1_PI * (2 * i + 1) * (2 * k + 1) / (4.0 * n));
            }
        }

        out[i] = n == 4 ? sum * 2.0 * sqrt(2.0) / 3.0 : sum;
    }
}

static void refIdentity(const double* in, double* out, uint32_t n) {
    double scale = n == 4 ? 5793.0 / 4096.0
                          : (n == 8 ? 2.0 : (n == 16 ? 11586.0 / 4096.0 : 4.0));

    for (uint32_t i = 0; i < n; i++) {
        out[i] = in[i] * scale;
    }
}

/** 0 for a DCT, 1 for an ADST, 2 for the identity, per 7.13.3's three lists. */
static uint32_t refRowKind(uint32_t type) {
    static const uint32_t kinds[16] = {0, 0, 1, 1, 0, 1, 1, 1,
                                       1, 2, 2, 0, 2, 1, 2, 1};

    return kinds[type];
}

static uint32_t refColKind(uint32_t type) {
    static const uint32_t kinds[16] = {0, 1, 0, 1, 1, 0, 1, 1,
                                       1, 2, 0, 2, 1, 2, 1, 2};

    return kinds[type];
}

static void ref1d(const double* in, double* out, uint32_t kind, uint32_t n) {
    if (kind == 0) {
        refDct(in, out, n);
    }
    else if (kind == 1) {
        refAdst(in, out, n);
    }
    else {
        refIdentity(in, out, n);
    }
}

static void ref2d(
    const int32_t* in, double* out, uint32_t size, uint32_t type
) {
    static double mid[64][64];
    double a[64];
    double b[64];

    uint32_t w = txWidth[size];
    uint32_t h = txHeight[size];
    uint32_t log2w = log2Of(w);
    uint32_t log2h = log2Of(h);
    uint32_t rect = (log2w > log2h ? log2w - log2h : log2h - log2w) == 1;

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            a[j] =
                (i < 32 && j < 32) ? (double) in[i * AV1_TEST_STRIDE + j] : 0.0;

            if (rect) a[j] = a[j] * 2896.0 / 4096.0;
        }

        ref1d(a, b, refRowKind(type), w);

        for (uint32_t j = 0; j < w; j++) {
            mid[i][j] = b[j] / (double) (1u << rowShift[size]);
        }
    }

    for (uint32_t j = 0; j < w; j++) {
        for (uint32_t i = 0; i < h; i++) {
            a[i] = mid[i][j];
        }

        ref1d(a, b, refColKind(type), h);

        for (uint32_t i = 0; i < h; i++) {
            out[i * 64 + j] = b[i] / 16.0;
        }
    }
}

/**
 * The integer network against the closed form, over every size and type.
 *
 * The worst absolute difference measured over 400 random blocks per pair at
 * coefficient bounds of 100, 400 and 800 is 2.08, at TX_32X64 with DCT_DCT; the
 * residuals themselves run to about 850. That ceiling comes from the network's
 * fixed count of Round2 steps rather than from the input, so it does not move
 * with the coefficient magnitude, which is why 3 is a tight bound rather than a
 * guess. Coefficients are kept small enough that neither clamp engages, since
 * the reference does not model them.
 */
static int floatReference(void) {
    int r = 0;
    static int32_t input[64 * AV1_TEST_STRIDE];
    static int32_t block[64 * AV1_TEST_STRIDE];
    static double expect[64 * 64];
    double worst = 0.0;
    int over = 0;

    for (uint32_t size = 0; size < 19; size++) {
        for (uint32_t type = 0; type < 16; type++) {
            for (uint32_t rep = 0; rep < 4; rep++) {
                uint32_t w = txWidth[size];
                uint32_t h = txHeight[size];

                fillCoeffs(input, size, 800);

                if (runOver(block, input, size, type) != TINYIMG_OK) continue;

                ref2d(input, expect, size, type);

                uint32_t flipUd = type == TINY_AV1_FLIPADST_DCT ||
                                  type == TINY_AV1_FLIPADST_ADST ||
                                  type == TINY_AV1_V_FLIPADST ||
                                  type == TINY_AV1_FLIPADST_FLIPADST;
                uint32_t flipLr = type == TINY_AV1_DCT_FLIPADST ||
                                  type == TINY_AV1_ADST_FLIPADST ||
                                  type == TINY_AV1_H_FLIPADST ||
                                  type == TINY_AV1_FLIPADST_FLIPADST;

                for (uint32_t i = 0; i < h; i++) {
                    for (uint32_t j = 0; j < w; j++) {
                        uint32_t si = flipUd ? h - 1 - i : i;
                        uint32_t sj = flipLr ? w - 1 - j : j;
                        double diff = fabs(
                            (double) block[i * AV1_TEST_STRIDE + j] -
                            expect[si * 64 + sj]
                        );

                        if (diff > worst) worst = diff;
                        if (diff > 3.0) over++;
                    }
                }
            }
        }
    }

    printf("float reference: worst absolute difference %.4f\n", worst);

    r |= assertEquals(over, 0);
    r |= assertLessThan(worst, 3.0);

    return r;
}

// #region bounds

/**
 * A stride wider than the transform, which an in place pass has to respect.
 *
 * Everything outside the block, including the columns to the right of it on the
 * block's own rows, has to come back untouched. The residual also has to be the
 * same whatever the stride, which the tight case below checks against this one
 * while watching a guard past the end of a buffer sized exactly to the block.
 */
static int strideRespected(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];
    int touched = 0;

    for (uint32_t size = 0; size < 19; size++) {
        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            block[i] = -777;
        }

        for (uint32_t i = 0; i < 32 && i < txHeight[size]; i++) {
            for (uint32_t j = 0; j < 32 && j < txWidth[size]; j++) {
                block[i * AV1_TEST_STRIDE + j] = nextCoeff(500);
            }
        }

        for (uint32_t i = 0; i < txHeight[size]; i++) {
            for (uint32_t j = txWidth[size]; j < AV1_TEST_STRIDE; j++) {
                block[i * AV1_TEST_STRIDE + j] = -777;
            }
        }

        r |= assertEquals(
            tiny_av1_inverse_transform(
                block, AV1_TEST_STRIDE, (TinyAv1TxSize) size, TINY_AV1_DCT_DCT,
                8
            ),
            TINYIMG_OK
        );

        for (uint32_t i = 0; i < 64; i++) {
            for (uint32_t j = 0; j < AV1_TEST_STRIDE; j++) {
                int inside = i < txHeight[size] && j < txWidth[size];

                if (!inside && block[i * AV1_TEST_STRIDE + j] != -777) {
                    touched++;
                }
            }
        }
    }

    r |= assertEquals(touched, 0);

    return r;
}

/** The same transform at stride == width, against the wide stride run. */
static int strideTight(void) {
    int r = 0;
    static int32_t input[64 * AV1_TEST_STRIDE];
    static int32_t wide[64 * AV1_TEST_STRIDE];
    static int32_t tight[64 * 64 + 64];
    int wrong = 0;
    int guardTouched = 0;

    for (uint32_t size = 0; size < 19; size++) {
        uint32_t w = txWidth[size];
        uint32_t h = txHeight[size];

        fillCoeffs(input, size, 500);

        r |= assertEquals(
            runOver(wide, input, size, TINY_AV1_DCT_DCT), TINYIMG_OK
        );

        for (uint32_t i = 0; i < 64 * 64 + 64; i++) {
            tight[i] = -999;
        }

        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                tight[i * w + j] = input[i * AV1_TEST_STRIDE + j];
            }
        }

        r |= assertEquals(
            tiny_av1_inverse_transform(
                tight, w, (TinyAv1TxSize) size, TINY_AV1_DCT_DCT, 8
            ),
            TINYIMG_OK
        );

        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                if (tight[i * w + j] != wide[i * AV1_TEST_STRIDE + j]) wrong++;
            }
        }

        for (uint32_t i = w * h; i < 64 * 64 + 64; i++) {
            if (tight[i] != -999) guardTouched++;
        }
    }

    r |= assertEquals(wrong, 0);
    r |= assertEquals(guardTouched, 0);

    return r;
}

/**
 * A coefficient outside the top left 32x32, which 7.13.3 reads as zero.
 *
 * Asserted as an equality against the same block with those positions already
 * zero, so a transform that reads them produces a different residual and fails.
 */
static int highFrequencyIgnored(void) {
    int r = 0;
    static int32_t withJunk[64 * AV1_TEST_STRIDE];
    static int32_t clean[64 * AV1_TEST_STRIDE];
    static int32_t a[64 * AV1_TEST_STRIDE];
    static int32_t b[64 * AV1_TEST_STRIDE];

    const uint32_t sizes[5] = {
        TINY_AV1_TX_64X64, TINY_AV1_TX_32X64, TINY_AV1_TX_64X32,
        TINY_AV1_TX_16X64, TINY_AV1_TX_64X16
    };

    for (uint32_t k = 0; k < 5; k++) {
        uint32_t size = sizes[k];
        uint32_t w = txWidth[size];
        uint32_t h = txHeight[size];
        int wrong = 0;

        fillCoeffs(clean, size, 600);

        for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
            withJunk[i] = clean[i];
        }

        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                if (i >= 32 || j >= 32) {
                    withJunk[i * AV1_TEST_STRIDE + j] = 30000;
                }
            }
        }

        r |=
            assertEquals(runOver(a, clean, size, TINY_AV1_DCT_DCT), TINYIMG_OK);
        r |= assertEquals(
            runOver(b, withJunk, size, TINY_AV1_DCT_DCT), TINYIMG_OK
        );

        for (uint32_t i = 0; i < h; i++) {
            for (uint32_t j = 0; j < w; j++) {
                if (a[i * AV1_TEST_STRIDE + j] != b[i * AV1_TEST_STRIDE + j]) {
                    wrong++;
                }
            }
        }

        r |= assertEquals(wrong, 0);
    }

    return r;
}

/**
 * The clamps, which are the only thing bit_depth changes.
 *
 * colClampRange is Max(bit_depth + 6, 16), so a row result of 42422 survives at
 * 12 bits and is clipped to 32767 at 8. Worked on paper for dc = 60000, which
 * only a 12 bit stream could carry: the row pass gives Round2(2896 * 60000, 12)
 * = 42422, and the column pass then gives Round2(2896 * 32767, 12) = 23167 and
 * Round2(23167, 4) = 1448 against Round2(2896 * 42422, 12) = 29994 and
 * Round2(29994, 4) = 1875.
 *
 * The two ranges are the same 16 bits at 8 bit depth, so the third case is the
 * only one that can tell them apart. At 12 bits rowClampRange is 20 and
 * colClampRange 18, and dc = 400000, which the dequantizer's own clip permits
 * at that depth, puts 282813 between them: clipped to 131071 the column pass
 * gives Round2(2896 * 131071, 12) = 92671 and Round2(92671, 4) = 5792. The 8
 * bit run of the same block saturates and lands back on 1448.
 */
static int clampRanges(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = 60000;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_OK
    );
    r |= assertEquals(block[0], 1448);

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = 60000;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 12
        ),
        TINYIMG_OK
    );
    r |= assertEquals(block[0], 1875);

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = 400000;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 12
        ),
        TINYIMG_OK
    );
    r |= assertEquals(block[0], 5792);

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = 400000;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_OK
    );
    r |= assertEquals(block[0], 1448);

    return r;
}

/**
 * The clip between the passes, on a block that actually reaches it.
 *
 * At 8 bit depth rowClampRange and colClampRange are both 16, so nothing there
 * can tell them apart and nothing so far makes either clamp fire. Four dequant
 * DCs of 400000 down column 0 at 12 bits do both: each row pass produces a flat
 * 282813, which the 18 bit clip takes to 131071, and the column DCT's first
 * Hadamard then sums 185343 and 171231 to 356574, which the same 18 bits take
 * to 131071 again. Round2 by 4 of [131071, -70944, 70944, 14112] is the row
 * below. Under a 20 bit clamp neither would bite and the first row would read
 * 22286.
 */
static int betweenPassClip(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];
    const int32_t expect[4] = {8192, -4434, 4434, 882};
    int wrong = 0;

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }

    for (uint32_t i = 0; i < 4; i++) {
        block[i * AV1_TEST_STRIDE] = 400000;
    }

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 12
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 4; j++) {
            if (block[i * AV1_TEST_STRIDE + j] != expect[i]) wrong++;
        }
    }

    r |= assertEquals(wrong, 0);

    return r;
}

/** SINPI_1_9 through SINPI_4_9, the table in 7.13.2.6. */
static const int32_t sinPi[4] = {1321, 2482, 3344, 3803};

/**
 * The four ADST4 constants, which nothing else in this file can see.
 *
 * A DC only ADST4 produces Round2(SINPI_(j + 1) * dc, 12) at position j, so one
 * pass reads out the whole table. It has to be read at a large DC: the residual
 * goes through a shift of four, which puts a one unit error in a 4096 scale
 * constant below the output's resolution at ordinary coefficient magnitudes.
 * 65536 needs 12 bit depth to be a legal dequantized coefficient, and is small
 * enough that neither clamp engages.
 *
 * H_ADST runs the ADST along the rows and the identity down the columns, so the
 * table lands in row 0; V_ADST is the transpose and puts it in column 0.
 */
static int adstConstants(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];
    const int32_t dc = 65536;
    int wrong = 0;

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = dc;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_H_ADST, 12
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 4; j++) {
            int32_t want = 0;

            if (i == 0) {
                int32_t row = round2((int64_t) sinPi[j] * dc, 12);

                want = round2(identityScale(row, 2), 4);
            }

            if (block[i * AV1_TEST_STRIDE + j] != want) wrong++;
        }
    }

    r |= assertEquals(wrong, 0);

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }
    block[0] = dc;
    wrong = 0;

    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_V_ADST, 12
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 4; j++) {
            int32_t want = 0;

            if (j == 0) {
                int32_t row = identityScale(dc, 2);

                want = round2(round2((int64_t) sinPi[i] * row, 12), 4);
            }

            if (block[i * AV1_TEST_STRIDE + j] != want) wrong++;
        }
    }

    r |= assertEquals(wrong, 0);

    return r;
}

/** The arguments the contract rejects. */
static int arguments(void) {
    int r = 0;
    static int32_t block[64 * AV1_TEST_STRIDE];

    for (uint32_t i = 0; i < 64 * AV1_TEST_STRIDE; i++) {
        block[i] = 0;
    }

    r |= assertEquals(
        tiny_av1_inverse_transform(
            0, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, (TinyAv1TxSize) 19, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_ERR_RANGE
    );
    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, (TinyAv1TxType) 16, 8
        ),
        TINYIMG_ERR_RANGE
    );
    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, AV1_TEST_STRIDE, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 9
        ),
        TINYIMG_ERR_RANGE
    );
    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, 3, TINY_AV1_TX_4X4, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_av1_inverse_transform(
            block, 16, TINY_AV1_TX_32X32, TINY_AV1_DCT_DCT, 8
        ),
        TINYIMG_ERR_BOUNDS
    );

    r |= assertEquals(tiny_av1_inverse_wht(0, 4, 8), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_av1_inverse_wht(block, 3, 8), TINYIMG_ERR_BOUNDS);
    r |= assertEquals(tiny_av1_inverse_wht(block, 4, 9), TINYIMG_ERR_RANGE);

    return r;
}

// #region lossless

/**
 * The Walsh-Hadamard pair, on three blocks worked out on paper.
 *
 * A DC of 16 comes out flat at 1: the row pass takes a = 16 >> 2 = 4 through
 * e = 2 to [2, 2, 2, 2], and each column then takes 2 through e = 1 to
 * [1, 1, 1, 1]. A lone horizontal AC of 16 gives c = 4 and the row
 * [2, 2, -2, -2], which columns turn into two columns of 1 and two of -1.
 *
 * The third case is the one that tells an arithmetic shift from a truncating
 * one. A DC of -4 gives a = -1 and e = -1 >> 1, which is -1 by floor and 0 by
 * truncation, so the row is [0, -1, -1, -1] rather than [-1, 0, 0, 0] and the
 * whole residual differs.
 */
static int lossless(void) {
    int r = 0;
    static int32_t block[4 * AV1_TEST_STRIDE];

    const int32_t flatDc[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const int32_t horizontal[16] = {1, 1, -1, -1, 1, 1, -1, -1,
                                    1, 1, -1, -1, 1, 1, -1, -1};
    const int32_t negative[16] = {0, 0,  0,  0,  0, -1, -1, -1,
                                  0, -1, -1, -1, 0, -1, -1, -1};

    const int32_t coeff[3] = {16, 16, -4};
    const uint32_t at[3] = {0, 1, 0};
    const int32_t* expect[3] = {flatDc, horizontal, negative};

    for (uint32_t k = 0; k < 3; k++) {
        int wrong = 0;

        for (uint32_t i = 0; i < 4 * AV1_TEST_STRIDE; i++) {
            block[i] = 0;
        }
        block[at[k]] = coeff[k];

        r |= assertEquals(
            tiny_av1_inverse_wht(block, AV1_TEST_STRIDE, 8), TINYIMG_OK
        );

        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                if (block[i * AV1_TEST_STRIDE + j] != expect[k][i * 4 + j]) {
                    wrong++;
                }
            }
        }

        r |= assertEquals(wrong, 0);
    }

    return r;
}

int main(void) {
    int r = 0;

    r |= sweep();
    r |= dcOnly();
    r |= oneAxis();
    r |= identityExact();
    r |= flips();
    r |= floatReference();
    r |= strideRespected();
    r |= strideTight();
    r |= highFrequencyIgnored();
    r |= clampRanges();
    r |= betweenPassClip();
    r |= adstConstants();
    r |= arguments();
    r |= lossless();

    return r;
}
