#include "av1-tables.h"
#include "av1.h"

/**
 * Sections 7.13.2 and 7.13.3, transcribed step for step. The bare numbers in
 * the comments are the specification's own step numbers, so the network can be
 * read against the document line by line; that ordering is the whole contract
 * and a step in the wrong place produces a picture that looks nearly right.
 *
 * Two things sit here that the specification puts elsewhere.
 *
 * The row pass writes back into `block` rather than into a separate Residual
 * array, which is safe because nothing reads a row again before its column
 * runs. So the only scratch is one 64 entry 1D array on the stack, and a 64x64
 * block needs no allocation.
 *
 * The flip that 7.12.3 applies while adding the residual to the prediction is
 * applied here instead, by reversing rows or columns at the end. The caller
 * therefore adds in raster order and cannot get flipLR and flipUD the wrong way
 * round; it must not apply them again.
 */

/**
 * @brief Runs the lossless inverse transform over one 4x4 block.
 *
 * The Walsh-Hadamard pair 7.13.3 reaches when `Lossless` is 1. That case is
 * always TX_4X4 with DCT_DCT and no shift on either pass, so the transform size
 * and type carry no information and are not parameters, and
 * `tiny_av1_inverse_transform` cannot serve it: its arguments cannot express
 * `Lossless`. This declaration belongs in `av1.h` beside that one.
 *
 * The Walsh-Hadamard steps have no clamp of their own, so `block` must hold
 * values inside the range 7.12.3 clips a dequantized coefficient to, which is
 * plus or minus `1 << (7 + bit_depth)`.
 *
 * @param block The coefficients, replaced by the residual.
 * @param stride Elements per row of `block`.
 * @param bit_depth 8, 10 or 12, which sets the clamp between the two passes.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_av1_inverse_wht(int32_t* block, uint32_t stride, uint32_t bit_depth);

#define AV1_SINPI_1_9 1321
#define AV1_SINPI_2_9 2482
#define AV1_SINPI_3_9 3344
#define AV1_SINPI_4_9 3803

/** Which 1D transform runs on an axis, indexed into by the tables below. */
#define AV1_TX_1D_DCT 0
#define AV1_TX_1D_ADST 1
#define AV1_TX_1D_IDENTITY 2

/** The row transform each type runs, from 7.13.3's three lists. */
static const uint8_t av1_row_tx_1d[16] = {
    AV1_TX_1D_DCT,      AV1_TX_1D_DCT,      AV1_TX_1D_ADST,     AV1_TX_1D_ADST,
    AV1_TX_1D_DCT,      AV1_TX_1D_ADST,     AV1_TX_1D_ADST,     AV1_TX_1D_ADST,
    AV1_TX_1D_ADST,     AV1_TX_1D_IDENTITY, AV1_TX_1D_IDENTITY, AV1_TX_1D_DCT,
    AV1_TX_1D_IDENTITY, AV1_TX_1D_ADST,     AV1_TX_1D_IDENTITY, AV1_TX_1D_ADST
};

/** The column transform each type runs. */
static const uint8_t av1_col_tx_1d[16] = {
    AV1_TX_1D_DCT,  AV1_TX_1D_ADST,     AV1_TX_1D_DCT,  AV1_TX_1D_ADST,
    AV1_TX_1D_ADST, AV1_TX_1D_DCT,      AV1_TX_1D_ADST, AV1_TX_1D_ADST,
    AV1_TX_1D_ADST, AV1_TX_1D_IDENTITY, AV1_TX_1D_DCT,  AV1_TX_1D_IDENTITY,
    AV1_TX_1D_ADST, AV1_TX_1D_IDENTITY, AV1_TX_1D_ADST, AV1_TX_1D_IDENTITY
};

// #region butterflies

/** Round2, which floors and so needs an arithmetic shift for a negative x. */
static int32_t av1_round2(int64_t x, uint32_t n) {
    if (n == 0) return (int32_t) x;
    return (int32_t) ((x + ((int64_t) 1 << (n - 1))) >> n);
}

/** Clip3 to what a signed integer of `bits` bits can hold. */
static int32_t av1_clip_bits(int64_t v, uint32_t bits) {
    int64_t lo = -((int64_t) 1 << (bits - 1));
    int64_t hi = ((int64_t) 1 << (bits - 1)) - 1;

    return (int32_t) (v < lo ? lo : (v > hi ? hi : v));
}

/** Bit reversal of the low `bits` bits of `x`. */
static uint32_t av1_brev(uint32_t bits, uint32_t x) {
    uint32_t t = 0;

    for (uint32_t i = 0; i < bits; i++) {
        t += ((x >> i) & 1u) << (bits - 1u - i);
    }

    return t;
}

static int32_t av1_cos128(int32_t angle) {
    uint32_t a = (uint32_t) angle & 255u;

    if (a <= 64u) return tiny_av1_cos128_lookup[a];
    if (a <= 128u) return -(int32_t) tiny_av1_cos128_lookup[128u - a];
    if (a <= 192u) return -(int32_t) tiny_av1_cos128_lookup[a - 128u];

    return tiny_av1_cos128_lookup[256u - a];
}

static int32_t av1_sin128(int32_t angle) {
    return av1_cos128(angle - 64);
}

/**
 * The butterfly rotation B, and the flip B(a, b, angle, 1, r) ends with.
 *
 * The specification's `r` is only a conformance requirement here, not a clamp,
 * so it is not a parameter. The products are 64 bit because a 4096 scale on a
 * twenty bit intermediate does not fit in 32.
 */
static void av1_b(
    int32_t* t, uint32_t a, uint32_t b, int32_t angle, uint32_t flip
) {
    int64_t x =
        (int64_t) t[a] * av1_cos128(angle) - (int64_t) t[b] * av1_sin128(angle);
    int64_t y =
        (int64_t) t[a] * av1_sin128(angle) + (int64_t) t[b] * av1_cos128(angle);

    t[a] = av1_round2(x, 12);
    t[b] = av1_round2(y, 12);

    if (flip) {
        int32_t swap = t[a];

        t[a] = t[b];
        t[b] = swap;
    }
}

/** The Hadamard rotation H, which is the only step that clamps. */
static void av1_h(
    int32_t* t, uint32_t a, uint32_t b, uint32_t flip, uint32_t r
) {
    if (flip) {
        uint32_t swap = a;

        a = b;
        b = swap;
    }

    int32_t x = t[a];
    int32_t y = t[b];

    t[a] = av1_clip_bits((int64_t) x + y, r);
    t[b] = av1_clip_bits((int64_t) x - y, r);
}

// #region 1d transforms

/** 7.13.2.2, the bit reversal the inverse DCT is permuted by. */
static void av1_dct_permute(int32_t* t, uint32_t n) {
    int32_t copy[64];
    uint32_t len = 1u << n;

    for (uint32_t i = 0; i < len; i++) {
        copy[i] = t[i];
    }

    for (uint32_t i = 0; i < len; i++) {
        t[i] = copy[av1_brev(n, i)];
    }
}

/** 7.13.2.3, the inverse DCT of length 1 << n for 2 <= n <= 6. */
static void av1_dct(int32_t* t, uint32_t n, uint32_t r) {
    av1_dct_permute(t, n);

    if (n == 6) {
        for (uint32_t i = 0; i < 16; i++) {
            // 2
            av1_b(t, 32 + i, 63 - i, 63 - 4 * (int32_t) av1_brev(4, i), 0);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 8; i++) {
            // 3
            av1_b(
                t, 16 + i, 31 - i, 6 + ((int32_t) av1_brev(3, 7 - i) << 3), 0
            );
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 16; i++) {
            // 4
            av1_h(t, 32 + i * 2, 33 + i * 2, i & 1u, r);
        }
    }

    if (n >= 4) {
        for (uint32_t i = 0; i < 4; i++) {
            // 5
            av1_b(
                t, 8 + i, 15 - i, 12 + ((int32_t) av1_brev(2, 3 - i) << 4), 0
            );
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 8; i++) {
            // 6
            av1_h(t, 16 + 2 * i, 17 + 2 * i, i & 1u, r);
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 2; j++) {
                // 7
                av1_b(
                    t, 62 - i * 4 - j, 33 + i * 4 + j,
                    60 - 16 * (int32_t) av1_brev(2, i) + 64 * (int32_t) j, 1
                );
            }
        }
    }

    if (n >= 3) {
        for (uint32_t i = 0; i < 2; i++) {
            // 8
            av1_b(t, 4 + i, 7 - i, 56 - 32 * (int32_t) i, 0);
        }
    }

    if (n >= 4) {
        for (uint32_t i = 0; i < 4; i++) {
            // 9
            av1_h(t, 8 + 2 * i, 9 + 2 * i, i & 1u, r);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 2; j++) {
                // 10
                av1_b(
                    t, 30 - 4 * i - j, 17 + 4 * i + j,
                    24 + ((int32_t) j << 6) + ((int32_t) (1u - i) << 5), 1
                );
            }
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 8; i++) {
            for (uint32_t j = 0; j < 2; j++) {
                // 11
                av1_h(t, 32 + i * 4 + j, 35 + i * 4 - j, i & 1u, r);
            }
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        // 12
        av1_b(t, 2 * i, 2 * i + 1, 32 + 16 * (int32_t) i, 1u - i);
    }

    if (n >= 3) {
        for (uint32_t i = 0; i < 2; i++) {
            // 13
            av1_h(t, 4 + 2 * i, 5 + 2 * i, i, r);
        }
    }

    if (n >= 4) {
        for (uint32_t i = 0; i < 2; i++) {
            // 14
            av1_b(t, 14 - i, 9 + i, 48 + 64 * (int32_t) i, 1);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 2; j++) {
                // 15
                av1_h(t, 16 + 4 * i + j, 19 + 4 * i - j, i & 1u, r);
            }
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                // 16
                av1_b(
                    t, 61 - i * 8 - j, 34 + i * 8 + j,
                    56 - (int32_t) i * 32 + (int32_t) (j >> 1) * 64, 1
                );
            }
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        // 17
        av1_h(t, i, 3 - i, 0, r);
    }

    // 18
    if (n >= 3) av1_b(t, 6, 5, 32, 1);

    if (n >= 4) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 2; j++) {
                // 19
                av1_h(t, 8 + 4 * i + j, 11 + 4 * i - j, i, r);
            }
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 4; i++) {
            // 20
            av1_b(t, 29 - i, 18 + i, 48 + (int32_t) (i >> 1) * 64, 1);
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 4; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                // 21
                av1_h(t, 32 + 8 * i + j, 39 + 8 * i - j, i & 1u, r);
            }
        }
    }

    if (n >= 3) {
        for (uint32_t i = 0; i < 4; i++) {
            // 22
            av1_h(t, i, 7 - i, 0, r);
        }
    }

    if (n >= 4) {
        for (uint32_t i = 0; i < 2; i++) {
            // 23
            av1_b(t, 13 - i, 10 + i, 32, 1);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                // 24
                av1_h(t, 16 + i * 8 + j, 23 + i * 8 - j, i, r);
            }
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 8; i++) {
            // 25
            av1_b(t, 59 - i, 36 + i, i < 4 ? 48 : 112, 1);
        }
    }

    if (n >= 4) {
        for (uint32_t i = 0; i < 8; i++) {
            // 26
            av1_h(t, i, 15 - i, 0, r);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 4; i++) {
            // 27
            av1_b(t, 27 - i, 20 + i, 32, 1);
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 8; i++) {
            // 28
            av1_h(t, 32 + i, 47 - i, 0, r);
            av1_h(t, 48 + i, 63 - i, 1, r);
        }
    }

    if (n >= 5) {
        for (uint32_t i = 0; i < 16; i++) {
            // 29
            av1_h(t, i, 31 - i, 0, r);
        }
    }

    if (n == 6) {
        for (uint32_t i = 0; i < 8; i++) {
            // 30
            av1_b(t, 55 - i, 40 + i, 32, 1);
        }

        for (uint32_t i = 0; i < 32; i++) {
            // 31
            av1_h(t, i, 63 - i, 0, r);
        }
    }
}

/** 7.13.2.4, the permutation the inverse ADST of 8 or 16 opens with. */
static void av1_adst_permute_in(int32_t* t, uint32_t n) {
    int32_t copy[16];
    uint32_t n0 = 1u << n;

    for (uint32_t i = 0; i < n0; i++) {
        copy[i] = t[i];
    }

    for (uint32_t i = 0; i < n0; i++) {
        t[i] = copy[(i & 1u) ? (i - 1u) : (n0 - i - 1u)];
    }
}

/** 7.13.2.5, the permutation and sign flip it closes with. */
static void av1_adst_permute_out(int32_t* t, uint32_t n) {
    int32_t copy[16];
    uint32_t n0 = 1u << n;

    for (uint32_t i = 0; i < n0; i++) {
        copy[i] = t[i];
    }

    for (uint32_t i = 0; i < n0; i++) {
        uint32_t a = (i >> 3) & 1u;
        uint32_t b = ((i >> 2) & 1u) ^ ((i >> 3) & 1u);
        uint32_t c = ((i >> 1) & 1u) ^ ((i >> 2) & 1u);
        uint32_t d = (i & 1u) ^ ((i >> 1) & 1u);
        uint32_t idx = ((d << 3) | (c << 2) | (b << 1) | a) >> (4u - n);

        t[i] = (i & 1u) ? -copy[idx] : copy[idx];
    }
}

/** 7.13.2.6, which is written out rather than built from butterflies. */
static void av1_adst4(int32_t* t) {
    int64_t s[7];
    int64_t x[4];
    int64_t a7;
    int64_t b7;

    s[0] = (int64_t) AV1_SINPI_1_9 * t[0];
    s[1] = (int64_t) AV1_SINPI_2_9 * t[0];
    s[2] = (int64_t) AV1_SINPI_3_9 * t[1];
    s[3] = (int64_t) AV1_SINPI_4_9 * t[2];
    s[4] = (int64_t) AV1_SINPI_1_9 * t[2];
    s[5] = (int64_t) AV1_SINPI_2_9 * t[3];
    s[6] = (int64_t) AV1_SINPI_4_9 * t[3];
    a7 = t[0] - t[2];
    b7 = a7 + t[3];

    s[0] = s[0] + s[3];
    s[1] = s[1] - s[4];
    s[3] = s[2];
    s[2] = AV1_SINPI_3_9 * b7;

    s[0] = s[0] + s[5];
    s[1] = s[1] - s[6];

    x[0] = s[0] + s[3];
    x[1] = s[1] + s[3];
    x[2] = s[2];
    x[3] = s[0] + s[1];

    x[3] = x[3] - s[3];

    t[0] = av1_round2(x[0], 12);
    t[1] = av1_round2(x[1], 12);
    t[2] = av1_round2(x[2], 12);
    t[3] = av1_round2(x[3], 12);
}

/** 7.13.2.7. */
static void av1_adst8(int32_t* t, uint32_t r) {
    av1_adst_permute_in(t, 3);

    for (uint32_t i = 0; i < 4; i++) {
        av1_b(t, 2 * i, 2 * i + 1, 60 - 16 * (int32_t) i, 1);
    }

    for (uint32_t i = 0; i < 4; i++) {
        av1_h(t, i, 4 + i, 0, r);
    }

    for (uint32_t i = 0; i < 2; i++) {
        av1_b(t, 4 + 3 * i, 5 + i, 48 - 32 * (int32_t) i, 1);
    }

    for (uint32_t i = 0; i < 2; i++) {
        for (uint32_t j = 0; j < 2; j++) {
            av1_h(t, 4 * j + i, 2 + 4 * j + i, 0, r);
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        av1_b(t, 2 + 4 * i, 3 + 4 * i, 32, 1);
    }

    av1_adst_permute_out(t, 3);
}

/** 7.13.2.8. */
static void av1_adst16(int32_t* t, uint32_t r) {
    av1_adst_permute_in(t, 4);

    for (uint32_t i = 0; i < 8; i++) {
        av1_b(t, 2 * i, 2 * i + 1, 62 - 8 * (int32_t) i, 1);
    }

    for (uint32_t i = 0; i < 8; i++) {
        av1_h(t, i, 8 + i, 0, r);
    }

    for (uint32_t i = 0; i < 2; i++) {
        av1_b(t, 8 + 2 * i, 9 + 2 * i, 56 - 32 * (int32_t) i, 1);
        av1_b(t, 13 + 2 * i, 12 + 2 * i, 8 + 32 * (int32_t) i, 1);
    }

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 2; j++) {
            av1_h(t, 8 * j + i, 4 + 8 * j + i, 0, r);
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        for (uint32_t j = 0; j < 2; j++) {
            av1_b(
                t, 4 + 8 * j + 3 * i, 5 + 8 * j + i, 48 - 32 * (int32_t) i, 1
            );
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        for (uint32_t j = 0; j < 4; j++) {
            av1_h(t, 4 * j + i, 2 + 4 * j + i, 0, r);
        }
    }

    for (uint32_t i = 0; i < 4; i++) {
        av1_b(t, 2 + 4 * i, 3 + 4 * i, 32, 1);
    }

    av1_adst_permute_out(t, 4);
}

/** 7.13.2.9, the inverse ADST of length 1 << n for 2 <= n <= 4. */
static void av1_adst(int32_t* t, uint32_t n, uint32_t r) {
    if (n == 2) {
        av1_adst4(t);
    }
    else if (n == 3) {
        av1_adst8(t, r);
    }
    else {
        av1_adst16(t, r);
    }
}

/** 7.13.2.10, the lossless transform, which is length 4 and never clamps. */
static void av1_wht(int32_t* t, uint32_t shift) {
    int64_t a = t[0] >> shift;
    int64_t c = t[1] >> shift;
    int64_t d = t[2] >> shift;
    int64_t b = t[3] >> shift;
    int64_t e;

    a += c;
    d -= b;
    e = (a - d) >> 1;
    b = e - b;
    c = e - c;
    a -= b;
    d += c;

    t[0] = (int32_t) a;
    t[1] = (int32_t) b;
    t[2] = (int32_t) c;
    t[3] = (int32_t) d;
}

/** 7.13.2.15, the identity of length 1 << n for 2 <= n <= 5. */
static void av1_identity(int32_t* t, uint32_t n) {
    uint32_t len = 1u << n;

    for (uint32_t i = 0; i < len; i++) {
        switch (n) {
            case 2: t[i] = av1_round2((int64_t) t[i] * 5793, 12); break;
            case 3: t[i] = (int32_t) ((int64_t) t[i] * 2); break;
            case 4: t[i] = av1_round2((int64_t) t[i] * 11586, 12); break;
            default: t[i] = (int32_t) ((int64_t) t[i] * 4); break;
        }
    }
}

// #region 2d transform

/** Runs one axis, having already checked that its length is in range. */
static void av1_tx_1d(int32_t* t, uint32_t kind, uint32_t n, uint32_t r) {
    if (kind == AV1_TX_1D_DCT) {
        av1_dct(t, n, r);
    }
    else if (kind == AV1_TX_1D_ADST) {
        av1_adst(t, n, r);
    }
    else {
        av1_identity(t, n);
    }
}

/**
 * Whether 7.13.2 defines that 1D transform at that length.
 *
 * The DCT covers 4 to 64, the ADST 4 to 16 and the identity 4 to 32. Every
 * combination a conformant bitstream can carry is inside this, because
 * get_tx_set forces DCT_DCT once an axis reaches 64 and allows only DCT_DCT or
 * IDTX at 32; the pairs this rejects are the ones the specification gives no
 * value for at all.
 */
static int av1_tx_1d_defined(uint32_t kind, uint32_t n) {
    if (kind == AV1_TX_1D_ADST) return n <= 4;
    if (kind == AV1_TX_1D_IDENTITY) return n <= 5;

    return 1;
}

int tiny_av1_inverse_transform(
    int32_t* block, uint32_t stride, TinyAv1TxSize tx_size,
    TinyAv1TxType tx_type, uint32_t bit_depth
) {
    if (!block) return TINYIMG_ERR_NULL;

    if ((uint32_t) tx_size > (uint32_t) TINY_AV1_TX_64X16) {
        return TINYIMG_ERR_RANGE;
    }

    if ((uint32_t) tx_type > (uint32_t) TINY_AV1_H_FLIPADST) {
        return TINYIMG_ERR_RANGE;
    }

    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
        return TINYIMG_ERR_RANGE;
    }

    uint32_t log2w = tiny_av1_tx_width_log2[tx_size];
    uint32_t log2h = tiny_av1_tx_height_log2[tx_size];
    uint32_t w = 1u << log2w;
    uint32_t h = 1u << log2h;

    if (stride < w) return TINYIMG_ERR_BOUNDS;

    uint32_t row_kind = av1_row_tx_1d[tx_type];
    uint32_t col_kind = av1_col_tx_1d[tx_type];

    if (!av1_tx_1d_defined(row_kind, log2w) ||
        !av1_tx_1d_defined(col_kind, log2h)) {
        return TINYIMG_ERR_CORRUPT;
    }

    uint32_t row_shift = tiny_av1_transform_row_shift[tx_size];
    uint32_t row_clamp = bit_depth + 8;
    uint32_t col_clamp = tiny_max_u32(bit_depth + 6, 16);
    uint32_t rect = (log2w > log2h ? log2w - log2h : log2h - log2w) == 1;

    int32_t t[64];

    for (uint32_t i = 0; i < h; i++) {
        int32_t* row = block + (size_t) i * stride;

        for (uint32_t j = 0; j < w; j++) {
            t[j] = (i < 32 && j < 32) ? row[j] : 0;
        }

        if (rect) {
            for (uint32_t j = 0; j < w; j++) {
                t[j] = av1_round2((int64_t) t[j] * 2896, 12);
            }
        }

        av1_tx_1d(t, row_kind, log2w, row_clamp);

        // the between-stage clip folded in, since no row is read again first
        for (uint32_t j = 0; j < w; j++) {
            row[j] = av1_clip_bits(av1_round2(t[j], row_shift), col_clamp);
        }
    }

    for (uint32_t j = 0; j < w; j++) {
        for (uint32_t i = 0; i < h; i++) {
            t[i] = block[(size_t) i * stride + j];
        }

        av1_tx_1d(t, col_kind, log2h, col_clamp);

        for (uint32_t i = 0; i < h; i++) {
            block[(size_t) i * stride + j] = av1_round2(t[i], 4);
        }
    }

    uint32_t flip_ud =
        tx_type == TINY_AV1_FLIPADST_DCT || tx_type == TINY_AV1_FLIPADST_ADST ||
        tx_type == TINY_AV1_V_FLIPADST || tx_type == TINY_AV1_FLIPADST_FLIPADST;
    uint32_t flip_lr =
        tx_type == TINY_AV1_DCT_FLIPADST || tx_type == TINY_AV1_ADST_FLIPADST ||
        tx_type == TINY_AV1_H_FLIPADST || tx_type == TINY_AV1_FLIPADST_FLIPADST;

    if (flip_ud) {
        for (uint32_t i = 0; i < h / 2; i++) {
            int32_t* top = block + (size_t) i * stride;
            int32_t* bottom = block + (size_t) (h - 1 - i) * stride;

            for (uint32_t j = 0; j < w; j++) {
                int32_t swap = top[j];

                top[j] = bottom[j];
                bottom[j] = swap;
            }
        }
    }

    if (flip_lr) {
        for (uint32_t i = 0; i < h; i++) {
            int32_t* row = block + (size_t) i * stride;

            for (uint32_t j = 0; j < w / 2; j++) {
                int32_t swap = row[j];

                row[j] = row[w - 1 - j];
                row[w - 1 - j] = swap;
            }
        }
    }

    return TINYIMG_OK;
}

int tiny_av1_inverse_wht(int32_t* block, uint32_t stride, uint32_t bit_depth) {
    if (!block) return TINYIMG_ERR_NULL;
    if (stride < 4) return TINYIMG_ERR_BOUNDS;

    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
        return TINYIMG_ERR_RANGE;
    }

    uint32_t col_clamp = tiny_max_u32(bit_depth + 6, 16);
    int32_t t[4];

    for (uint32_t i = 0; i < 4; i++) {
        int32_t* row = block + (size_t) i * stride;

        for (uint32_t j = 0; j < 4; j++) {
            t[j] = row[j];
        }

        av1_wht(t, 2);

        for (uint32_t j = 0; j < 4; j++) {
            row[j] = av1_clip_bits(t[j], col_clamp);
        }
    }

    for (uint32_t j = 0; j < 4; j++) {
        for (uint32_t i = 0; i < 4; i++) {
            t[i] = block[(size_t) i * stride + j];
        }

        av1_wht(t, 0);

        for (uint32_t i = 0; i < 4; i++) {
            block[(size_t) i * stride + j] = t[i];
        }
    }

    return TINYIMG_OK;
}
