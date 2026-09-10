#include "codec/av1-item.h"

/**
 * @file
 * @brief The intra prediction of one transform block.
 *
 * Three kinds of check, and the first is the one that makes the other two worth
 * having.
 *
 * **A second implementation agrees on 18,924 cases.** It was written in Python
 * from specification 7.11.2 rather than from this C, and it reads the two
 * tables that matter, `Dr_Intra_Derivative` and `Intra_Filter_Taps`, out of the
 * generated header, which `bun run tables:check` verifies against the
 * specification. The sweep is every transform size against every mode, every
 * angle delta for the eight directional modes, all four combinations of which
 * neighbors exist, both filter types, the edge filter on and off, and all five
 * recursive filter modes. The aggregate digest below is what that agreement was
 * recorded as.
 *
 * **Closed forms, which need no reference at all.** A flat edge has to give a
 * flat block through all eighteen predictors, which is a partition-of-unity
 * statement about every kernel in the stage: the edge filter, the upsample, the
 * smooth weights and the recursive taps all have to sum to their own scale, and
 * any index error moves a sample off the edge and breaks it.
 *
 * **Hand-computed values** for the modes simple enough to work out on paper,
 * because a digest says two readings agree and a hand-computed value says the
 * reading is right.
 *
 * What none of it proves is that the prediction is fed the right neighbors,
 * which is the reconstruction's job; that arrives with 1i, compared against
 * `avifdec`.
 */

/** The transform sizes, in the order the specification numbers them. */
static const uint32_t TX_W[19] = {4,  8,  16, 32, 64, 4, 8,  8,  16, 16,
                                  32, 32, 64, 4,  16, 8, 32, 16, 64};
static const uint32_t TX_H[19] = {4,  8,  16, 32, 64, 8,  4, 16, 8, 32,
                                  16, 64, 32, 16, 4,  32, 8, 64, 16};

static uint8_t above[2 * 64 + 1];
static uint8_t left[2 * 64 + 1];
static uint8_t block[64 * 64];

/** The pseudorandom edge both implementations were driven with. */
static void pattern(uint32_t seed, uint8_t* out, uint32_t count) {
    uint32_t state = seed;

    for (uint32_t i = 0; i < count; i++) {
        state = state * 1103515245u + 12345u;
        out[i] = (uint8_t) (state >> 17);
    }
}

static uint64_t digest_block(uint32_t w, uint32_t h) {
    uint64_t hash = 1469598103934665603ULL;

    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            hash = (hash ^ block[i * 64u + j]) * 1099511628211ULL;
        }
    }

    return hash;
}

static void fold(uint64_t* hash, uint64_t value) {
    for (uint32_t shift = 0; shift < 64u; shift += 8u) {
        *hash = (*hash ^ ((value >> shift) & 0xFFu)) * 1099511628211ULL;
    }
}

/** Defaults every case starts from, so a field left out means what it says. */
static void base_opts(TinyAv1PredictOpts* opts, uint32_t w, uint32_t h) {
    tiny_memset(opts, 0, sizeof(*opts));

    opts->have_above = 1u;
    opts->have_left = 1u;
    opts->above_available = (uint16_t) (2u * w);
    opts->left_available = (uint16_t) (2u * h);
    opts->bit_depth = 8u;
}

/**
 * @brief A flat edge gives a flat block, through every predictor there is.
 *
 * The strongest check in this file that needs no reference. Every kernel in the
 * stage has to sum to its own scale for this to hold: the five tap edge filter
 * to 16, the four tap upsample to 16, the smooth weights to 256 against their
 * complements, and each row of the recursive taps to 16. An index that reads
 * one sample off the end of an edge also breaks it, because the sample past the
 * end is the replicated one and only equals the rest when the edge is flat.
 */
static int flatEdgesGiveAFlatBlock(void) {
    int r = 0;
    uint32_t checked = 0;

    for (uint32_t size = 0; size < 19u; size++) {
        uint32_t w = TX_W[size];
        uint32_t h = TX_H[size];

        tiny_memset(above, 0x5Au, sizeof(above));
        tiny_memset(left, 0x5Au, sizeof(left));

        for (uint32_t mode = 0; mode < 13u; mode++) {
            for (int32_t delta = -3; delta <= 3; delta++) {
                if (!(mode >= 1u && mode <= 8u) && delta != 0) continue;

                TinyAv1PredictOpts opts;
                base_opts(&opts, w, h);

                opts.angle_delta = (int8_t) delta;
                opts.edge_filter = 1u;

                tiny_memset(block, 0, sizeof(block));

                r |= assertEquals(
                    tiny_av1_predict_intra(
                        block, 64u, w, h, above, left, (TinyAv1IntraMode) mode,
                        &opts
                    ),
                    TINYIMG_OK
                );

                uint32_t wrong = 0;

                for (uint32_t i = 0; i < h; i++) {
                    for (uint32_t j = 0; j < w; j++) {
                        if (block[i * 64u + j] != 0x5Au) wrong++;
                    }
                }

                r |= assertEquals((long) wrong, 0L);
                checked++;
            }
        }

        for (uint32_t filter_mode = 0; filter_mode < 5u; filter_mode++) {
            TinyAv1PredictOpts opts;
            base_opts(&opts, w, h);

            opts.edge_filter = 1u;
            opts.use_filter_intra = 1u;
            opts.filter_intra_mode = (uint8_t) filter_mode;

            tiny_memset(block, 0, sizeof(block));

            r |= assertEquals(
                tiny_av1_predict_intra(
                    block, 64u, w, h, above, left, TINY_AV1_DC_PRED, &opts
                ),
                TINYIMG_OK
            );

            uint32_t wrong = 0;

            for (uint32_t i = 0; i < h; i++) {
                for (uint32_t j = 0; j < w; j++) {
                    if (block[i * 64u + j] != 0x5Au) wrong++;
                }
            }

            r |= assertEquals((long) wrong, 0L);
            checked++;
        }
    }

    // 61 mode and delta pairs plus 5 recursive filters, over 19 sizes
    r |= assertEquals((long) checked, 1254L);

    return r;
}

/**
 * @brief Every row of the recursive filter's taps sums to sixteen.
 *
 * The property the case above depends on, asserted directly so a failure says
 * which of the two is wrong. Sixteen because `INTRA_FILTER_SCALE_BITS` is 4,
 * and a row that summed to anything else would make the filter darken or
 * brighten a flat region.
 */
static int filterTapsSumToTheirScale(void) {
    int r = 0;

    for (uint32_t mode = 0; mode < 5u; mode++) {
        for (uint32_t row = 0; row < 8u; row++) {
            int32_t sum = 0;

            for (uint32_t tap = 0; tap < 7u; tap++) {
                sum += tiny_av1_intra_filter_taps[mode][row][tap];
            }

            r |= assertEquals((long) sum, 16L);
        }
    }

    // and the edge kernels, for the same reason
    for (uint32_t strength = 0; strength < 3u; strength++) {
        int32_t sum = 0;

        for (uint32_t tap = 0; tap < 5u; tap++) {
            sum += tiny_av1_intra_edge_kernel[strength][tap];
        }

        r |= assertEquals((long) sum, 16L);
    }

    return r;
}

/**
 * @brief The two axis-aligned modes copy their edge, and the DC averages both.
 *
 * Hand-computed rather than digested. The DC value is the one number in the
 * stage worth working out on paper: an 8x8 block with a row of 100 above and a
 * column of 200 to the left sums to 2,400, takes half the total count as a
 * rounding term for 2,408, and divides by 16 for 150. The division is by
 * `w + h`, which is not a power of two for a rectangular transform, and that is
 * why it is a division rather than a shift.
 */
static int copiesAndAveragesTheEdges(void) {
    int r = 0;

    tiny_memset(above, 0, sizeof(above));
    tiny_memset(left, 0, sizeof(left));

    for (uint32_t i = 0; i < sizeof(above); i++) above[i] = 100u;
    for (uint32_t i = 0; i < sizeof(left); i++) left[i] = 200u;

    TinyAv1PredictOpts opts;
    base_opts(&opts, 8u, 8u);

    tiny_memset(block, 0, sizeof(block));
    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) block[i * 64u + j], 150L);
        }
    }

    // a 4x16 block, where the two extents differ and the divisor is 20
    tiny_memset(block, 0, sizeof(block));
    base_opts(&opts, 4u, 16u);

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 4u, 16u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_OK
    );

    // 4 * 100 + 16 * 200 = 3600, + 10 = 3610, / 20 = 180
    r |= assertEquals((long) block[0], 180L);

    // now a ramp, so a copy is distinguishable from an average
    for (uint32_t i = 0; i < sizeof(above); i++) above[i] = (uint8_t) (i * 3u);
    for (uint32_t i = 0; i < sizeof(left); i++) left[i] = (uint8_t) (200u - i);

    base_opts(&opts, 8u, 8u);
    tiny_memset(block, 0, sizeof(block));

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_V_PRED, &opts
        ),
        TINYIMG_OK
    );

    // V_PRED is every row equal to AboveRow, which starts one past the corner
    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) block[i * 64u + j], (long) above[1u + j]);
        }
    }

    tiny_memset(block, 0, sizeof(block));

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_H_PRED, &opts
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) block[i * 64u + j], (long) left[1u + i]);
        }
    }

    return r;
}

/**
 * @brief Paeth picks whichever of the three neighbors the gradient is nearest.
 *
 * Worked out by hand on a 4x4. With a corner of 10, a row above of 20 and a
 * column to the left of 200, the predicted base is 20 + 200 - 10 = 210, whose
 * distance to the left sample is 10, to the one above 190 and to the corner
 * 200; the left sample wins. Reversing the two edges reverses the answer, which
 * is what says the comparison is not simply picking a fixed one.
 */
static int paethPicksTheNearestNeighbor(void) {
    int r = 0;

    tiny_memset(above, 20u, sizeof(above));
    tiny_memset(left, 200u, sizeof(left));
    above[0] = 10u;

    TinyAv1PredictOpts opts;
    base_opts(&opts, 4u, 4u);

    tiny_memset(block, 0, sizeof(block));

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 4u, 4u, above, left, TINY_AV1_PAETH_PRED, &opts
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4u; i++) {
        for (uint32_t j = 0; j < 4u; j++) {
            r |= assertEquals((long) block[i * 64u + j], 200L);
        }
    }

    tiny_memset(above, 200u, sizeof(above));
    tiny_memset(left, 20u, sizeof(left));
    above[0] = 10u;

    tiny_memset(block, 0, sizeof(block));

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 4u, 4u, above, left, TINY_AV1_PAETH_PRED, &opts
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4u; i++) {
        for (uint32_t j = 0; j < 4u; j++) {
            r |= assertEquals((long) block[i * 64u + j], 200L);
        }
    }

    return r;
}

/**
 * @brief The whole sweep the independent implementation agreed on.
 *
 * 18,924 cases, folded into one digest. The number is a recording, and what
 * makes it worth recording is that a second reading of the specification
 * produced it too: every case's own digest matched, not just the total.
 */
static int agreesWithTheIndependentReference(void) {
    int r = 0;
    uint64_t hash = 1469598103934665603ULL;
    uint32_t cases = 0;

    for (uint32_t size = 0; size < 19u; size++) {
        uint32_t w = TX_W[size];
        uint32_t h = TX_H[size];

        pattern(0x9E3779B9u, above, 2u * w + 1u);
        pattern(0x85EBCA6Bu, left, 2u * h + 1u);

        for (uint32_t mode = 0; mode < 13u; mode++) {
            for (int32_t delta = -3; delta <= 3; delta++) {
                if (!(mode >= 1u && mode <= 8u) && delta != 0) continue;

                for (uint32_t avail = 0; avail < 4u; avail++) {
                    for (uint32_t type = 0; type < 2u; type++) {
                        for (uint32_t edge = 0; edge < 2u; edge++) {
                            TinyAv1PredictOpts opts;
                            base_opts(&opts, w, h);

                            opts.have_above = (avail & 1u) != 0u;
                            opts.have_left = (avail & 2u) != 0u;
                            opts.angle_delta = (int8_t) delta;
                            opts.edge_filter = (uint8_t) edge;
                            opts.filter_type = (uint8_t) type;

                            tiny_memset(block, 0, sizeof(block));

                            if (tiny_av1_predict_intra(
                                    block, 64u, w, h, above, left,
                                    (TinyAv1IntraMode) mode, &opts
                                ) != TINYIMG_OK) {
                                return 1;
                            }

                            fold(&hash, digest_block(w, h));
                            cases++;
                        }
                    }
                }
            }
        }

        for (uint32_t filter_mode = 0; filter_mode < 5u; filter_mode++) {
            for (uint32_t avail = 0; avail < 4u; avail++) {
                TinyAv1PredictOpts opts;
                base_opts(&opts, w, h);

                opts.have_above = (avail & 1u) != 0u;
                opts.have_left = (avail & 2u) != 0u;
                opts.edge_filter = 1u;
                opts.use_filter_intra = 1u;
                opts.filter_intra_mode = (uint8_t) filter_mode;

                tiny_memset(block, 0, sizeof(block));

                if (tiny_av1_predict_intra(
                        block, 64u, w, h, above, left, TINY_AV1_DC_PRED, &opts
                    ) != TINYIMG_OK) {
                    return 1;
                }

                fold(&hash, digest_block(w, h));
                cases++;
            }
        }
    }

    r |= assertEquals((long) cases, 18924L);
    r |= assertTrue(hash == 0x79b4fc71fd508aa1ULL);

    return r;
}

/**
 * @brief Chroma from luma adds the luma's detail and nothing else.
 *
 * Three properties rather than a recording. A scale of zero changes nothing,
 * whatever the luma is. A flat luma changes nothing, whatever the scale is,
 * because the average is removed first and a flat block has no detail to
 * borrow. And the sign follows the scale: with a luma brighter than its own
 * average, a positive scale can only raise the prediction.
 */
static int chromaFromLumaBorrowsOnlyDetail(void) {
    int r = 0;

    static uint8_t luma[32 * 32];
    static uint8_t chroma[16 * 16];

    for (uint32_t i = 0; i < sizeof(luma); i++) {
        luma[i] = (uint8_t) (40u + (i % 97u));
    }

    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 8u, 8u, luma, 32u, 16u, 16u, 1u, 1u, 0, 8u
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) chroma[i * 16u + j], 128L);
        }
    }

    // a flat luma has no detail, so any scale leaves the prediction alone
    tiny_memset(luma, 77u, sizeof(luma));
    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 8u, 8u, luma, 32u, 16u, 16u, 1u, 1u, 16, 8u
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) chroma[i * 16u + j], 128L);
        }
    }

    /*
     * A step in the luma, and the two halves have to move apart.
     *
     * The left half is 60 and the right half 180, so the average sits between
     * them and a positive scale raises the bright half and lowers the dark one.
     * Asserting the direction rather than the value, because the value is what
     * the digest of a real frame will check at 1i.
     */
    for (uint32_t i = 0; i < 16u; i++) {
        for (uint32_t j = 0; j < 16u; j++) {
            luma[i * 32u + j] = j < 8u ? 60u : 180u;
        }
    }

    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 8u, 8u, luma, 32u, 16u, 16u, 1u, 1u, 8, 8u
        ),
        TINYIMG_OK
    );

    r |= assertLessThan((double) chroma[0], 128.0);
    r |= assertGreaterThan((double) chroma[7], 128.0);

    // and a negative scale reverses both
    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 8u, 8u, luma, 32u, 16u, 16u, 1u, 1u, -8, 8u
        ),
        TINYIMG_OK
    );

    r |= assertGreaterThan((double) chroma[0], 128.0);
    r |= assertLessThan((double) chroma[7], 128.0);

    /*
     * The exact values, worked out on paper, because the direction is not
     * enough.
     *
     * A luma of 64 on the left half and 192 on the right, 4:2:0, so each chroma
     * sample sums four luma samples and shifts by one: 512 and 1,536 at three
     * fractional bits. Their average over sixteen samples is 1,024, so the two
     * halves sit 512 either side of it, and a scale of 8 turns that into
     * Round2Signed(4096, 6) = 64 either way. From a DC prediction of 128 that
     * is exactly 64 and 192.
     *
     * The subsampling shift is what this pins down: computing it as a fixed
     * three rather than `3 - subX - subY` scales every sample by four, which
     * the direction assertions above cannot see.
     */
    for (uint32_t i = 0; i < 16u; i++) {
        for (uint32_t j = 0; j < 16u; j++) {
            luma[i * 32u + j] = j < 8u ? 64u : 192u;
        }
    }

    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 8u, 8u, luma, 32u, 16u, 16u, 1u, 1u, 8, 8u
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 8u; i++) {
        for (uint32_t j = 0; j < 8u; j++) {
            r |= assertEquals((long) chroma[i * 16u + j], j < 4u ? 64L : 192L);
        }
    }

    /*
     * A case that lands on a negative rounding boundary, which is the one place
     * Round2 and Round2Signed disagree.
     *
     * 4:4:4, so a chroma sample is one luma sample shifted by three. Rows of
     * 10, 10, 10, 11 give an average of 82 against samples of 80 and 88, so the
     * three dark samples sit two below it; a scale of 16 makes that exactly
     * -32, where Round2 gives 0 and Round2Signed gives -1. The bright sample is
     * 6 above, which is +96 and rounds to 2 either way, so the two columns
     * together say which rounding ran.
     */
    for (uint32_t i = 0; i < 4u; i++) {
        for (uint32_t j = 0; j < 4u; j++) {
            luma[i * 32u + j] = j < 3u ? 10u : 11u;
        }
    }

    tiny_memset(chroma, 128u, sizeof(chroma));

    r |= assertEquals(
        tiny_av1_predict_cfl(
            chroma, 16u, 4u, 4u, luma, 32u, 4u, 4u, 0u, 0u, 16, 8u
        ),
        TINYIMG_OK
    );

    for (uint32_t i = 0; i < 4u; i++) {
        for (uint32_t j = 0; j < 4u; j++) {
            r |= assertEquals((long) chroma[i * 16u + j], j < 3u ? 127L : 130L);
        }
    }

    return r;
}

/** What it refuses, including the bit depth its buffers cannot hold. */
static int refusesWhatItCannotPredict(void) {
    int r = 0;

    TinyAv1PredictOpts opts;
    base_opts(&opts, 8u, 8u);

    r |= assertEquals(
        tiny_av1_predict_intra(
            0, 64u, 8u, 8u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_DC_PRED, 0
        ),
        TINYIMG_ERR_NULL
    );

    // a stride narrower than the block would write into the next row
    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 4u, 8u, 8u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_ERR_BOUNDS
    );
    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 128u, 128u, 8u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_ERR_BOUNDS
    );

    // chroma-from-luma is a mode of its own and never reaches this function
    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_UV_CFL_PRED, &opts
        ),
        TINYIMG_ERR_BOUNDS
    );

    /*
     * Ten and twelve bit samples are refused rather than truncated.
     *
     * The frame this predicts into is eight bits a sample, so a deeper AVIF
     * needs a sixteen bit buffer through the whole reconstruction and not just
     * here. Refusing at the frame is what the decoder does; refusing here as
     * well is what stops a caller reaching it another way.
     */
    opts.bit_depth = 10u;

    r |= assertEquals(
        tiny_av1_predict_intra(
            block, 64u, 8u, 8u, above, left, TINY_AV1_DC_PRED, &opts
        ),
        TINYIMG_ERR_UNSUPPORTED_VARIANT
    );

    static uint8_t luma[16 * 16];

    r |= assertEquals(
        tiny_av1_predict_cfl(
            0, 16u, 8u, 8u, luma, 16u, 16u, 16u, 1u, 1u, 0, 8u
        ),
        TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_av1_predict_cfl(
            block, 16u, 8u, 8u, 0, 16u, 16u, 16u, 1u, 1u, 0, 8u
        ),
        TINYIMG_ERR_NULL
    );

    // a subsampled read needs two luma samples per chroma sample to exist
    r |= assertEquals(
        tiny_av1_predict_cfl(
            block, 16u, 8u, 8u, luma, 16u, 1u, 16u, 1u, 1u, 0, 8u
        ),
        TINYIMG_ERR_BOUNDS
    );

    return r;
}

int main(void) {
    int r = 0;

    tiny_init();

    r |= filterTapsSumToTheirScale();
    r |= flatEdgesGiveAFlatBlock();
    r |= copiesAndAveragesTheEdges();
    r |= paethPicksTheNearestNeighbor();
    r |= agreesWithTheIndependentReference();
    r |= chromaFromLumaBorrowsOnlyDetail();
    r |= refusesWhatItCannotPredict();

    return r;
}
