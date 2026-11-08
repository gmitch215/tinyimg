#ifndef TINYIMG_DISTORTION_H
#define TINYIMG_DISTORTION_H

#include "tinyimg/image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How much is known about the quality a lever or an encoder gives up.
 *
 * The classes stay separate in the API so a chooser can treat them
 * differently: only EXACT lets it drop the reference path without a measured
 * risk, and UNCHARACTERIZED means it must keep the reference path or mark the
 * result best effort.
 */
typedef enum
{
    /** Nothing was measured for this combination. Zero, so a cleared struct
     * reads this way. */
    TINYIMG_DISTORTION_UNCHARACTERIZED = 0,
    /** The output was byte-identical to the reference on every image tested. */
    TINYIMG_DISTORTION_EXACT = 1,
    /** An analytic bound exists. No entry carries it yet. */
    TINYIMG_DISTORTION_BOUNDED = 2,
    /** A measured distribution of PSNR against the reference, `n` samples. */
    TINYIMG_DISTORTION_EMPIRICAL = 3,
} TinyDistortionClass;

/**
 * @brief What one lever setting costs in quality.
 *
 * The reference is candidate 0 of tiny_plan_enumerate: the plan's own effort
 * and filter at the decode scale the planner picks undegraded, at the same
 * output extent. Distortion is the PSNR in dB of the lever setting's 8-bit
 * output against the reference's, over every sample of every channel. Two
 * identical outputs are recorded as 99 dB.
 */
typedef struct {
    /** What is known; the other members are zero for UNCHARACTERIZED. */
    TinyDistortionClass klass;
    /** Fifth percentile of PSNR in dB over the measured samples. */
    float p05_db;
    /** Median PSNR in dB over the measured samples. */
    float median_db;
    /** Measured (image, width) samples behind the figures. */
    uint32_t n;
    /** Largest absolute sample difference seen in any sample. */
    uint8_t max_abs;
} TinyDistortion;

/**
 * @brief What an output format costs in bytes at one quality, and what it
 * gives back.
 *
 * Measured at 400 px wide on candidate 0's pixels of opaque images. The PSNR
 * is against the raw pixels after an external decoder, never tinyimg's own.
 * For PNG it is 99 and the class is EXACT; for GIF it is the palette's PSNR,
 * which does not depend on `quality`.
 */
typedef struct {
    /** What is known; the other members are zero for UNCHARACTERIZED. */
    TinyDistortionClass klass;
    /** Median encoded bytes per output pixel. */
    float bytes_per_pixel;
    /** 95th percentile of encoded bytes per output pixel, for a ceiling. */
    float bytes_per_pixel_p95;
    /** Fifth percentile of PSNR in dB. */
    float p05_db;
    /** Median PSNR in dB. */
    float median_db;
    /** Measured images behind the figures. */
    uint32_t n;
} TinyEncodeDistortion;

/**
 * @brief Looks up what a lever setting costs in quality.
 *
 * A pure function of its arguments: the table is compiled in, so the answer
 * never depends on a decode. A combination that was not measured, or measured
 * on too few images, is UNCHARACTERIZED, and nothing is interpolated.
 *
 * @param format The format of the source image, since the decoder is what a
 * lever changes.
 * @param effort_delta 1 when the candidate's effort differs from candidate 0's,
 * else 0.
 * @param filter_delta 1 when either filter differs from candidate 0's, else 0.
 * @param scale_steps Halvings of the decode scale denominator past candidate
 * 0's.
 * @param out Receives the figures, or NULL to ask for the class alone.
 * @return TinyDistortionClass The class, UNCHARACTERIZED when the combination
 * is unknown.
 */
TinyDistortionClass tiny_distortion_lever(
    TinyImageFormat format, uint8_t effort_delta, uint8_t filter_delta,
    uint8_t scale_steps, TinyDistortion* out
);

/**
 * @brief Looks up the bytes and the quality an encoder gives at one setting.
 *
 * A pure function of its arguments. Only the qualities that were measured
 * (50, 60, 70, 75, 80, 85, 90 and 95) are known; any other value, and any
 * format that was not measured, is UNCHARACTERIZED.
 *
 * @param format The output format.
 * @param quality The quality setting, 0 to 100.
 * @param out Receives the figures, or NULL to ask for the class alone.
 * @return TinyDistortionClass The class.
 */
TinyDistortionClass tiny_distortion_encode(
    TinyImageFormat format, uint8_t quality, TinyEncodeDistortion* out
);

/**
 * @brief Counts the rows the compiled-in tables hold.
 *
 * @internal Exposed so a test can walk every row without a second copy of the
 * table, and so the generated header's size can be compared with its JSON.
 *
 * @param lever Receives the lever row count, or NULL.
 * @param encode Receives the encode row count, or NULL.
 */
void tiny_distortion_rows(uint32_t* lever, uint32_t* encode);

/**
 * @brief Reads one lever row by index.
 *
 * @internal The test seam for walking the table.
 *
 * @param index Row number below the count tiny_distortion_rows reports.
 * @param format Receives the source format, or NULL.
 * @param effort_delta Receives the effort delta, or NULL.
 * @param filter_delta Receives the filter delta, or NULL.
 * @param scale_steps Receives the scale steps, or NULL.
 * @param out Receives the figures, or NULL.
 * @return int TINYIMG_OK or TINYIMG_ERR_RANGE for an index past the end.
 */
int tiny_distortion_lever_row(
    uint32_t index, TinyImageFormat* format, uint8_t* effort_delta,
    uint8_t* filter_delta, uint8_t* scale_steps, TinyDistortion* out
);

#ifdef __cplusplus
}
#endif

#endif
