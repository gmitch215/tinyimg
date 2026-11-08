#ifndef TINYIMG_WORK_H
#define TINYIMG_WORK_H

#include "tinyimg/image.h"
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
    /**
     * Progressive JPEG scans stepped over rather than entropy decoded.
     *
     * Non-zero only at a scale denominator of eight, where the transform reads
     * the DC term alone. A caller that expected the saving and sees zero here
     * is looking at a baseline file, not at a lever that failed.
     */
    TINYIMG_WORK_SCANS_SKIPPED = 10,
    /**
     * @brief Compressed bytes the entropy decoder pulled in.
     *
     * Counted from the position of the decoder's reader, so it is the bytes the
     * reader has reached rather than the bits it has used: a reader that
     * buffers runs ahead of the symbols by a few bytes. It is added at row,
     * strip, tile or stream granularity and never per symbol.
     *
     * What counts is the compressed payload and what sits inside it. JPEG is
     * the Huffman scan bytes, including stuffing and restart markers, and
     * excludes a progressive scan stepped over at a scale denominator of eight,
     * which TINYIMG_WORK_SCANS_SKIPPED counts instead. PNG is the deflate
     * stream, zlib header included. VP8 is the first partition plus the token
     * partitions, and VP8L the bitstream. GIF is the LZW data including its
     * sub-block length bytes. TIFF is the bytes of each compressed strip, so an
     * uncompressed strip adds nothing. BMP is the RLE8 data, so an uncompressed
     * bitmap adds nothing. AVIF is the arithmetic decoder's bytes over each
     * tile it decoded.
     *
     * Against TINYIMG_WORK_DECODED_SAMPLES it gives the bytes per sample, which
     * is the term that explains a small thumbnail's decode time when the source
     * is large.
     */
    TINYIMG_WORK_ENTROPY_BYTES = 11,
    /**
     * @brief Hash chain candidates the deflate match finder compared.
     *
     * The search work of a PNG or Deflate TIFF encode, which `ENCODED` cannot
     * see: two images of the same size differ by an order of magnitude here.
     * It is added once per encode.
     */
    TINYIMG_WORK_ENCODE_PROBES = 12,
    /**
     * @brief Samples whose alpha an encoder wrote without it.
     *
     * Counted by the encoder from the alpha bytes it was handed, so a caller
     * learns that transparency was lost without running the pipeline a second
     * time. JPEG and AVIF count every sample with alpha below 255. GIF carries
     * a one-bit mask, so it counts only the samples that are neither 0 nor 255.
     * Zero for RGB input and for every encoder that keeps alpha.
     */
    TINYIMG_WORK_ALPHA_DROPPED = 13,
    /**
     * @brief Microseconds the cost model charges for a decode's sample term.
     *
     * Added once per decode, when its extent is resolved, from the format, the
     * region and the scale denominator of that decode. Together with
     * TINYIMG_WORK_ENTROPY_BYTES it is the decode half of tiny_work_clock_us.
     */
    TINYIMG_WORK_DECODE_MICROS = 14,
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

/**
 * @brief Adds an image's unrepresentable alpha to TINYIMG_WORK_ALPHA_DROPPED.
 *
 * One branch-free pass over the alpha bytes. Does nothing for an image with no
 * alpha channel.
 *
 * @param image The image an encoder is about to write.
 * @param partial_only Nonzero to count only alpha strictly between 0 and 255,
 *        which is what a one-bit mask cannot keep; zero to count every alpha
 *        below 255.
 */
void tiny_work_count_alpha(const TinyImage* image, int partial_only);

/**
 * @brief Sets how much work the library may do before it stops itself.
 *
 * The limit is in the units of tiny_work_clock_us and counts from this call,
 * so the work already done on this module does not count against it. Zero
 * removes the limit and makes the clock absolute again.
 *
 * **This is a counter, not a clock.** A Worker's timers do not advance while
 * code runs, so the only guard that fires there is one built from work the
 * library has counted. The decoders and encoders check it at the grain they
 * already count at, and an operation past the limit returns
 * TINYIMG_ERR_BUDGET with the heap as clean as after any other error.
 *
 * @param micros The limit, in calibrated microseconds, or zero for none.
 */
void tiny_work_set_limit(uint32_t micros);

/**
 * @brief What the work counters add up to, in microseconds of the machine the
 * cost model was calibrated on.
 *
 * A pure function of the counters and of the formats of the last decode and
 * encode: the decode's sample term, the entropy bytes counted so far at the
 * format's byte rate, the samples resampled, each neighborhood pass, and the
 * samples handed to an encoder at the encoder's rate. Two runs of the same
 * request read the same value. A host multiplies by its own machine factor to
 * compare against a clock it trusts; the library applies none.
 *
 * Counts from the last tiny_work_set_limit, or from tiny_work_reset.
 *
 * @return uint32_t Microseconds, saturating.
 */
uint32_t tiny_work_clock_us(void);

/**
 * @brief Whether the work clock has passed the limit.
 *
 * @return int Non-zero past the limit; always zero with no limit set.
 */
int tiny_work_over_limit(void);

/**
 * @brief Tells the clock which format the decode about to run reads.
 *
 * Called by each decoder before it resolves its extent.
 *
 * @param format The source format.
 * @param variant Non-zero for a progressive JPEG or a lossless WebP, the two
 *        sources the cost model prices apart.
 */
void tiny_work_begin_decode(TinyImageFormat format, int variant);

/**
 * @brief Tells the clock which encode is about to run, so it prices the
 * samples handed to the encoder the way tiny_encode_cost_for does.
 *
 * Called by each encoder before it does any work.
 *
 * @param format The output format.
 * @param opts The encoder's options; NULL prices the defaults.
 * @param channels Channels of the image being written.
 */
void tiny_work_begin_encode(
    TinyImageFormat format, const TinyEncodeOpts* opts, uint8_t channels
);

/**
 * @brief Charges a decode's sample term to TINYIMG_WORK_DECODE_MICROS.
 *
 * @param width Source width.
 * @param height Source height.
 * @param x Left edge of the decoded region.
 * @param y Top edge of the decoded region.
 * @param w Width of the decoded region.
 * @param h Height of the decoded region.
 * @param den Scale denominator.
 * @param effort A TinyEffort.
 */
void tiny_work_charge_decode(
    uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h, uint8_t den, uint8_t effort
);

#ifdef __cplusplus
}
#endif

#endif
