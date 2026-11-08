#ifndef TINYIMG_COST_MODEL_API_H
#define TINYIMG_COST_MODEL_API_H

#include "tinyimg/tinyimg.h"

/*
 * The cost model's rates, read by the work clock. The tables live in
 * cost-model.h and are included once, by plan.c; these are the only way out of
 * that translation unit, so a second copy of the tables never ships.
 */

typedef enum
{
    COST_STAGE_RESAMPLE = 0,
    COST_STAGE_NEIGHBORHOOD = 1,
} CostStage;

/**
 * Microseconds the decode's sample term costs for one request, with the
 * effort fraction applied, as tiny_plan_resolve prices it.
 *
 * `x`, `y`, `w` and `h` are the resolved region in source pixels and `den` the
 * scale denominator, the same values a TinyDecodeOpts carries after
 * tiny_decode_resolve.
 */
uint32_t tiny_cost_decode_samples_us(
    TinyImageFormat format, uint8_t variant, uint32_t width, uint32_t height,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t den, uint8_t effort
);

/** Microseconds per million entropy bytes, or 0 for a format with no byte term.
 */
uint32_t tiny_cost_entropy_rate(TinyImageFormat format, uint8_t variant);

/** Microseconds per million samples a resample or neighborhood pass writes. */
uint32_t tiny_cost_stage_rate(CostStage stage);

#endif
