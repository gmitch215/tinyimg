#ifndef TINYIMG_WORK_H
#define TINYIMG_WORK_H

#include "tinyimg/tinyimg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What the last operation actually did, counted rather than claimed.
 *
 * Every reduction in this library is a claim that some work does not happen: a
 * scaled decode says it produces fewer samples, a region says it reconstructs
 * fewer macroblocks, the planner says it decodes a rectangle rather than a
 * picture. A label is not evidence. The 1/4 JPEG arm carried the word "reduced"
 * through the API, the planner and the decoder while running the full transform
 * and averaging the result away, and it took a profile to notice.
 *
 * These counters are the evidence. They are incremented per block and per
 * macroblock rather than per pixel, so the cost is a handful of increments
 * against work measured in thousands of operations.
 */
typedef enum
{
    /** Samples the source carries, from its header. */
    TINYIMG_WORK_SOURCE_SAMPLES = 0,
    /** Samples a decoder produced, which a reduction is supposed to lower. */
    TINYIMG_WORK_DECODED_SAMPLES = 1,
    /** Coefficient blocks read out of an entropy coded stream. */
    TINYIMG_WORK_BLOCKS = 2,
    /** Inverse transforms performed. */
    TINYIMG_WORK_TRANSFORMS = 3,
    /**
     * @brief Samples those transforms wrote.
     *
     * The one that catches a reduction that is not reducing: against
     * TINYIMG_WORK_TRANSFORMS it gives the samples per transform, which is 64
     * for a full block and 4 for a genuine quarter scale one.
     */
    TINYIMG_WORK_TRANSFORM_SAMPLES = 4,
    /** Macroblocks reconstructed, for the codecs built out of them. */
    TINYIMG_WORK_MACROBLOCKS = 5,
    /** Macroblocks put through a loop filter. */
    TINYIMG_WORK_FILTERED = 6,
    /** Samples a resampler wrote. */
    TINYIMG_WORK_RESAMPLED = 7,
    /** Samples handed to an encoder. */
    TINYIMG_WORK_ENCODED = 8,
    /** Full passes made over an image. */
    TINYIMG_WORK_PASSES = 9,
} TinyWorkCounter;

/**
 * @brief Clears every counter.
 *
 * Call before the operation being measured. Nothing calls this on a caller's
 * behalf, because a caller measuring a chain wants the total.
 */
void tiny_work_reset(void);

/**
 * @brief Reads one counter.
 *
 * A named field rather than a struct a host reads by offset, for the reason
 * tiny_plan_field exists: the layout is not part of the ABI.
 *
 * @param counter Which counter to read.
 * @return uint32_t Its value, or 0 for a counter this build does not know.
 */
uint32_t tiny_work_read(TinyWorkCounter counter);

/**
 * @brief Adds to one counter.
 *
 * Public so the codecs and the planner can reach it across translation units,
 * and so a caller adding its own stage to a measurement can use the same total.
 *
 * @param counter Which counter to add to.
 * @param amount How much to add.
 */
void tiny_work_add(TinyWorkCounter counter, uint32_t amount);

#ifdef __cplusplus
}
#endif

#endif
