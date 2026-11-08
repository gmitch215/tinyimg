#ifndef TINYIMG_SELECT_H
#define TINYIMG_SELECT_H

#include "tinyimg/distortion.h"
#include "tinyimg/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Picks the way to run a request that meets the caller's limits and
 * costs least.
 *
 * A request can be run several ways: the decode scale, the filter and the
 * decode effort (the lattice of tiny_plan_enumerate), then the output format
 * and its quality. The budget ladder walks one path through the first three
 * and takes the output format as given. The selector searches all of it.
 *
 * What it knows, and from where:
 * - time: the calibrated upper bound of tiny_plan_cost_upper for the lattice
 *   point plus tiny_encode_cost_upper_for for the output, times the caller's
 *   machine factor;
 * - memory: tiny_plan_peak_bytes through the lattice;
 * - quality: tiny_distortion_lever and tiny_distortion_encode, composed by
 *   adding noise power, as a predicted PSNR against the plan's own undegraded
 *   output;
 * - bytes: the median bytes per output pixel of tiny_distortion_encode.
 *
 * What it does not know: how many bytes this image will encode to, and how
 * much error this image will show. Both are table medians and percentiles over
 * other images. See tiny_select_compose_db and TinySelection.
 */

/**
 * @brief Output formats times quality tiers the search tries per lattice point.
 *
 * JPEG at 50, 75, 85 (its default), 90 and 95; lossy WebP at 50, 75, 90 and
 * 95; lossless WebP; AVIF at 50, 75, 90 and 95; and one each for PNG, GIF, BMP
 * and TIFF, which take no quality. 5 + 4 + 1 + 4 + 4 = 18.
 */
#define TINYIMG_SELECT_MAX_VARIANTS 18u

/**
 * @brief Most candidates one search can consider: the lattice's 16 points
 * times 18 variants, 288.
 */
#define TINYIMG_SELECT_MAX_CONSIDERED                                          \
    (TINYIMG_PLAN_MAX_CANDIDATES * TINYIMG_SELECT_MAX_VARIANTS)

/** @brief The bit of a TinyImageFormat in TinySelectOpts::formats. */
#define TINYIMG_SELECT_FORMAT(format) (1u << (uint32_t) (format))

/** @brief What the search minimizes among the candidates that fit. */
typedef enum
{
    /**
     * Fewest predicted bytes per output pixel. A candidate with no byte
     * figure ranks last. Ties go to the lower time bound.
     */
    TINYIMG_SELECT_MIN_BYTES = 0,
    /** Lowest time upper bound. Ties go to fewer predicted bytes. */
    TINYIMG_SELECT_MIN_TIME = 1,
    /**
     * Highest predicted dB. A candidate with no dB figure ranks last. Ties go
     * to fewer predicted bytes.
     */
    TINYIMG_SELECT_MAX_QUALITY = 2,
} TinySelectObjective;

/** @brief What the search does with a candidate whose quality is unknown. */
typedef enum
{
    /**
     * Drop it. A candidate is unknown when its composed class is
     * TINYIMG_DISTORTION_UNCHARACTERIZED: a lever setting measured on too few
     * images, or an output with no encode row (BMP, TIFF, lossless WebP).
     */
    TINYIMG_SELECT_REFUSE = 0,
    /**
     * Keep it. It cannot satisfy a `min_db` above zero when its dB is unknown,
     * and its bytes are unknown when its encode row is missing.
     */
    TINYIMG_SELECT_ALLOW = 1,
} TinySelectUncharacterized;

/**
 * @brief The constraint that removed a candidate, in the order they are
 * checked.
 *
 * A candidate is removed by the first check it fails, so a candidate removed
 * by TINYIMG_SELECT_REASON_PEAK was never tested against the later ones. The
 * time bound is computed only for candidates that passed every check before
 * the budget. The numbers order the checks, which is what a whole-search
 * `reason` is read from: the largest value among the candidates, the
 * constraint the nearest miss failed.
 */
typedef enum
{
    /** Nothing removed it. */
    TINYIMG_SELECT_REASON_NONE = 0,
    /** The output format is not in `formats`. */
    TINYIMG_SELECT_REASON_FORMAT = 1,
    /**
     * The output is a format that cannot carry the source's alpha channel
     * (JPEG, AVIF, GIF), and the caller did not say the source is opaque.
     */
    TINYIMG_SELECT_REASON_ALPHA = 2,
    /** The quality class is UNCHARACTERIZED and the caller said REFUSE. */
    TINYIMG_SELECT_REASON_UNCHARACTERIZED = 3,
    /** The peak heap bound exceeds `peak_cap_bytes`. */
    TINYIMG_SELECT_REASON_PEAK = 4,
    /** The predicted dB is below `min_db`, or unknown while `min_db` is set. */
    TINYIMG_SELECT_REASON_QUALITY = 5,
    /** The time upper bound, scaled, exceeds `budget_us`. */
    TINYIMG_SELECT_REASON_BUDGET = 6,
} TinySelectReason;

/**
 * @brief What a request may use and what it wants.
 *
 * A zeroed structure is the default of every member.
 */
typedef struct {
    /**
     * CPU the request may spend in microseconds, on the machine it runs on;
     * zero means no limit, as it does for tiny_plan_set_budget.
     */
    uint32_t budget_us;
    /**
     * How much slower the running machine is than the one the cost model was
     * calibrated on, times 1000, as in TinyAdmitOpts. Zero reads as 1000.
     */
    uint32_t scale_milli;
    /**
     * Largest peak heap bound a candidate may have. Zero reads as
     * TINYIMG_HEAP_MAX, the heap the module can grow to: the peak is a heap
     * figure, and a single image is separately limited by
     * TINYIMG_MAX_IMAGE_BYTES. The bound covers the plan run and not the
     * encoder.
     */
    uint32_t peak_cap_bytes;
    /**
     * Quality floor, as predicted PSNR in dB against the plan's undegraded
     * output. Zero or negative means no floor. It is compared with the
     * composed fifth percentile (see TinySelection), so it is a prediction
     * that held for 89.3 percent of lever cells and 94.8 percent of encode
     * cells out of sample, not a guarantee.
     */
    float min_db;
    /**
     * A bitmask of TINYIMG_SELECT_FORMAT bits for the output formats the
     * search may use. Zero reads as the source's own format (when it is one
     * of the writable ones), JPEG and WebP.
     */
    uint32_t formats;
    /** A TinySelectObjective. */
    uint32_t objective;
    /** A TinySelectUncharacterized. */
    uint32_t uncharacterized;
    /**
     * Non-zero when the caller knows the pixels are all opaque even though the
     * source has an alpha channel. The planner reads channel counts, not
     * pixels, so an opaque RGBA source is otherwise treated as having alpha
     * and cannot go to JPEG, AVIF or GIF.
     */
    uint32_t assume_opaque;
} TinySelectOpts;

/**
 * @brief One candidate the search looked at.
 *
 * A candidate is a point of the lattice (an index into tiny_plan_enumerate's
 * list for the same plan) at one output format and quality tier.
 */
typedef struct {
    /** Index into the plan's tiny_plan_enumerate list. */
    uint8_t candidate;
    /** The output TinyImageFormat. */
    uint8_t format;
    /**
     * The quality tier to pass in TinyEncodeOpts: 50, 75, 85, 90 or 95, or
     * zero for a format that takes none.
     */
    uint8_t quality;
    /** Non-zero for the lossless coding (PNG, BMP, TIFF, lossless WebP). */
    uint8_t lossless;
    /** A TinySelectReason; zero when nothing removed it. */
    uint8_t reason;
    /** The composed TinyDistortionClass, the weakest of the two parts. */
    uint8_t klass;
    /** Non-zero when `db` was predicted. */
    uint8_t db_known;
    /** Non-zero when `bytes_per_pixel` was predicted. */
    uint8_t bytes_known;
    /**
     * The time upper bound, scaled, in microseconds, or zero when the
     * candidate was removed before it was computed.
     */
    uint32_t cost_upper_us;
    /** The peak heap bound of the lattice point. */
    uint32_t peak_bytes;
    /** Predicted PSNR in dB, from the composed fifth percentiles. */
    float db;
    /** Predicted median encoded bytes per output pixel. */
    float bytes_per_pixel;
} TinySelectConsidered;

/**
 * @brief What the search decided and everything it looked at.
 *
 * Read it through tiny_selection_field and tiny_selection_considered_field;
 * the layout is not part of the ABI. It is about 7 KB, so keep one on the
 * stack only where the stack allows.
 *
 * **What the predicted figures are.** The dB is the PSNR against the plan's
 * own undegraded output (candidate 0), composed from two table lookups by
 * tiny_select_compose_db: the lever's fifth percentile and the encoder's fifth
 * percentile. Composing percentiles of two independent distributions is not
 * the fifth percentile of their sum; it is conservative if both rarely fall
 * low together. The measured coverage of each part's own p05 on images that
 * were not in the fit is carried in the selection, because it is under the 95
 * percent the percentile names: 89.3 percent for the lever table overall and
 * 64.5 percent for WebP sources, 94.8 percent for the encode table. The
 * coverage of the composition was not measured.
 *
 * **What the bytes are.** The median bytes per output pixel of the output
 * format at its tier, over 99 opaque images at 400 px wide. Image-to-image
 * spread is large, so only the order of two formats at the same tier is
 * relied on; see TECHNICAL_REPORT.md for the measured ordering accuracy. The
 * table was measured on the undegraded pixels, so a candidate with a coarser
 * decode encodes to fewer bytes than predicted.
 *
 * **Alpha.** A source with an alpha channel is priced from tables fitted on
 * opaque images, and the lever table's raw PSNR counts RGB under alpha 0, so
 * the dB of an alpha source is pessimistic and its bytes omit the alpha plane.
 * `alpha` says so.
 */
typedef struct {
    /** Non-zero when a candidate met every limit. */
    uint32_t feasible;
    /** A TinySelectReason for the whole search; zero when feasible. */
    uint32_t reason;
    /** Position of the choice in `considered`; valid when feasible. */
    uint32_t chosen;
    /** Number of entries of `considered` written. */
    uint32_t count;
    /** Distinct lattice points tiny_plan_enumerate listed. */
    uint32_t lattice;
    /** Width of the output image. */
    uint32_t width;
    /** Height of the output image. */
    uint32_t height;
    /** Channels of the output image. */
    uint32_t channels;
    /** Non-zero when the source was treated as having alpha. */
    uint32_t alpha;
    /** Measured held-out coverage of the lever p05 for the source format, in
     * thousandths. */
    uint32_t lever_coverage_milli;
    /** The same over every lever cell, in thousandths. */
    uint32_t lever_coverage_all_milli;
    /** The same for the encode table at the chosen format; zero when not
     * measured. */
    uint32_t encode_coverage_milli;
    /** The choice, copied from `considered`; zeroed when not feasible. */
    TinySelectConsidered choice;
    /** Predicted encoded bytes of the whole output, or zero when unknown. */
    uint32_t predicted_bytes;
    /** Predicted median PSNR in dB of the choice, composed like `choice.db`. */
    float db_median;
    /** Every candidate looked at, in search order, with what removed it. */
    TinySelectConsidered considered[TINYIMG_SELECT_MAX_CONSIDERED];
} TinySelection;

/** @brief A field of a TinySelection, named rather than offset. */
typedef enum TinySelectionField
{
    /** Non-zero when a candidate met every limit. */
    TINYIMG_SELECTION_FIELD_FEASIBLE = 0,
    /** A TinySelectReason for the whole search. */
    TINYIMG_SELECTION_FIELD_REASON = 1,
    /** Index of the choice in the plan's tiny_plan_enumerate list. */
    TINYIMG_SELECTION_FIELD_CANDIDATE = 2,
    /** The choice's output TinyImageFormat. */
    TINYIMG_SELECTION_FIELD_FORMAT = 3,
    /** The choice's quality tier; zero for a format that takes none. */
    TINYIMG_SELECTION_FIELD_QUALITY = 4,
    /** Non-zero when the choice is lossless. */
    TINYIMG_SELECTION_FIELD_LOSSLESS = 5,
    /** The choice's time upper bound, scaled. */
    TINYIMG_SELECTION_FIELD_COST_UPPER = 6,
    /** The choice's peak heap bound. */
    TINYIMG_SELECTION_FIELD_PEAK = 7,
    /** The choice's predicted dB, times 1000, rounded; zero when unknown. */
    TINYIMG_SELECTION_FIELD_DB_MILLI = 8,
    /** Predicted median dB times 1000, rounded; zero when unknown. */
    TINYIMG_SELECTION_FIELD_DB_MEDIAN_MILLI = 9,
    /** Non-zero when the choice's dB was predicted. */
    TINYIMG_SELECTION_FIELD_DB_KNOWN = 10,
    /** The choice's TinyDistortionClass. */
    TINYIMG_SELECTION_FIELD_CLASS = 11,
    /** Predicted bytes per output pixel times a million, rounded. */
    TINYIMG_SELECTION_FIELD_BYTES_PER_PIXEL_MICRO = 12,
    /** Non-zero when the choice's bytes were predicted. */
    TINYIMG_SELECTION_FIELD_BYTES_KNOWN = 13,
    /** Predicted bytes of the whole output; zero when unknown. */
    TINYIMG_SELECTION_FIELD_PREDICTED_BYTES = 14,
    /** Entries in `considered`. */
    TINYIMG_SELECTION_FIELD_CONSIDERED = 15,
    /** Distinct lattice points. */
    TINYIMG_SELECTION_FIELD_LATTICE = 16,
    /** Output width. */
    TINYIMG_SELECTION_FIELD_WIDTH = 17,
    /** Output height. */
    TINYIMG_SELECTION_FIELD_HEIGHT = 18,
    /** Output channels. */
    TINYIMG_SELECTION_FIELD_CHANNELS = 19,
    /** Non-zero when the source was treated as having alpha. */
    TINYIMG_SELECTION_FIELD_ALPHA = 20,
    /** Held-out coverage of the lever p05 for the source format, in
     * thousandths. */
    TINYIMG_SELECTION_FIELD_LEVER_COVERAGE = 21,
    /** Held-out coverage of the lever p05 over every format, in thousandths. */
    TINYIMG_SELECTION_FIELD_LEVER_COVERAGE_ALL = 22,
    /** Held-out coverage of the encode p05 at the chosen format, in
     * thousandths. */
    TINYIMG_SELECTION_FIELD_ENCODE_COVERAGE = 23,
    /** Position of the choice in `considered`. */
    TINYIMG_SELECTION_FIELD_CHOSEN = 24,
} TinySelectionField;

/** @brief A field of one entry of a TinySelection's `considered` list. */
typedef enum TinyConsideredField
{
    /** Index into the plan's tiny_plan_enumerate list. */
    TINYIMG_CONSIDERED_FIELD_CANDIDATE = 0,
    /** The output TinyImageFormat. */
    TINYIMG_CONSIDERED_FIELD_FORMAT = 1,
    /** The quality tier; zero for a format that takes none. */
    TINYIMG_CONSIDERED_FIELD_QUALITY = 2,
    /** Non-zero for a lossless coding. */
    TINYIMG_CONSIDERED_FIELD_LOSSLESS = 3,
    /** The TinySelectReason that removed it, zero if none. */
    TINYIMG_CONSIDERED_FIELD_REASON = 4,
    /** The composed TinyDistortionClass. */
    TINYIMG_CONSIDERED_FIELD_CLASS = 5,
    /** Predicted dB times 1000, rounded; zero when unknown. */
    TINYIMG_CONSIDERED_FIELD_DB_MILLI = 6,
    /** Non-zero when the dB was predicted. */
    TINYIMG_CONSIDERED_FIELD_DB_KNOWN = 7,
    /** Predicted bytes per output pixel times a million, rounded. */
    TINYIMG_CONSIDERED_FIELD_BYTES_PER_PIXEL_MICRO = 8,
    /** Non-zero when the bytes were predicted. */
    TINYIMG_CONSIDERED_FIELD_BYTES_KNOWN = 9,
    /** The time upper bound, scaled; zero when not computed. */
    TINYIMG_CONSIDERED_FIELD_COST_UPPER = 10,
    /** The peak heap bound. */
    TINYIMG_CONSIDERED_FIELD_PEAK = 11,
} TinyConsideredField;

/**
 * @brief Bytes a host reserves for a TinySelectOpts.
 *
 * @return uint32_t `sizeof(TinySelectOpts)`.
 */
uint32_t tiny_select_opts_sizeof(void);

/**
 * @brief Bytes a host reserves for a TinySelection.
 *
 * @return uint32_t `sizeof(TinySelection)`.
 */
uint32_t tiny_selection_sizeof(void);

/**
 * @brief Reads one named field of a selection.
 *
 * @param selection A selection from one of the tiny_plan_select functions.
 * @param field Which field.
 * @return uint32_t Its value, or 0 for a NULL selection or an unknown field.
 */
uint32_t tiny_selection_field(
    const TinySelection* selection, TinySelectionField field
);

/**
 * @brief Reads one named field of one considered candidate.
 *
 * @param selection A selection.
 * @param index Below the selection's `considered` count.
 * @param field Which field.
 * @return uint32_t Its value, or 0 for a NULL selection, an index past the
 * end or an unknown field.
 */
uint32_t tiny_selection_considered_field(
    const TinySelection* selection, uint32_t index, TinyConsideredField field
);

/**
 * @brief Composes a lever's quality and an encoder's by adding noise power.
 *
 * `db = -10 log10(10^(-lever_db / 10) + 10^(-encode_db / 10))`, with an EXACT
 * part contributing no noise and the result capped at 99 dB, the figure the
 * tables use for identical outputs.
 *
 * **The assumption is that the two errors are independent**, so their noise
 * powers add. The encoder's figure was measured on the undegraded pixels, not
 * on the lever's output, and both figures are percentiles of distributions
 * over images, so the composition is a model of the quality and not a
 * measurement of it. Adding a part never raises the result: the composed
 * value is at most the lower of the two.
 *
 * @internal Exposed so a test can assert the arithmetic directly.
 *
 * @param lever_class The lever's class.
 * @param lever_db The lever's dB.
 * @param encode_class The encode's class.
 * @param encode_db The encode's dB.
 * @return float The composed PSNR in dB.
 */
float tiny_select_compose_db(
    TinyDistortionClass lever_class, float lever_db,
    TinyDistortionClass encode_class, float encode_db
);

/**
 * @brief The weaker of two classes: EXACT over BOUNDED over EMPIRICAL over
 * UNCHARACTERIZED.
 *
 * @internal Exposed so a test can assert the order directly.
 *
 * @param a A class.
 * @param b A class.
 * @return TinyDistortionClass The one that says less.
 */
TinyDistortionClass tiny_select_weakest(
    TinyDistortionClass a, TinyDistortionClass b
);

/**
 * @brief Searches every way to run the plan and picks the best that fits.
 *
 * Tries each point of tiny_plan_enumerate's lattice (at most 16) at each
 * output format and quality tier (at most 18 variants), 288 candidates at
 * most, removing a candidate by the first check it fails, in the order of
 * TinySelectReason: the format mask, alpha, an UNCHARACTERIZED class, the peak
 * heap cap, the quality floor and then the time budget. The time bound is
 * priced only for candidates that reach the budget check. Among those that
 * meet every limit it takes the best by `objective`, a tie goes to the
 * secondary key the objective names, and a tie in both goes to the earlier
 * candidate in search order (lattice point, then variant), so the answer is a
 * pure function of the plan and the options.
 *
 * Cost: the lattice enumeration, one extra resolution of the plan and 18
 * encoder prices, then table lookups per candidate. No pixel is read and no
 * clock is. The plan's own budget, basis and degraded state are ignored; the
 * limits are the options'.
 *
 * Each point's time bound is its TinyPlanCandidate::cost_upper, the same
 * figure tiny_plan_cost_upper gives for the plan resolved at that point. The
 * encoder's upper bound is added and the sum is scaled by `scale_milli`,
 * rounded up.
 *
 * **A prediction under exchangeability, not a guarantee.** See TinySelection
 * for what each predicted figure is and its measured coverage.
 *
 * @param plan The plan, which may carry a budget.
 * @param opts The limits and objective, or NULL for the defaults.
 * @param selection Receives the decision.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for an unknown
 * objective or uncharacterized mode, or what tiny_plan_enumerate returns. An
 * infeasible search is TINYIMG_OK with `feasible` zero.
 */
int tiny_plan_select(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
);

/**
 * @brief The fixed policy: today's budget ladder, then the encoder as given.
 *
 * Takes the output format to be the source's own when `formats` allows it,
 * else the lowest format in the mask, at that format's default quality. The
 * encoder is priced first, the ladder of tiny_plan_resolve runs on the upper
 * bound with the budget left after it, as tiny_plan_admit does in
 * TINYIMG_ADMIT_DEGRADE mode, and the rung it settles on is the one candidate
 * evaluated. The limits are checked on it as on any candidate, so a ladder
 * choice that breaks the peak cap or the floor is reported infeasible rather
 * than adjusted. `objective` is not read.
 *
 * Because the format and tier are in the joint search's space and the rung is
 * in its lattice, the joint search is never worse than this on the objective
 * when this is feasible.
 *
 * @param plan The plan.
 * @param opts The limits, or NULL for the defaults.
 * @param selection Receives the decision, with one entry in `considered`.
 * @return int As tiny_plan_select.
 */
int tiny_plan_select_ladder(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
);

/**
 * @brief The independent policy: format and quality by bytes, then the ladder.
 *
 * Picks the output format and tier with the fewest predicted bytes per pixel
 * that pass the alpha, mask, UNCHARACTERIZED and quality checks at the
 * undegraded decode, ignoring CPU and memory, whatever `objective` says. Then
 * runs the ladder as tiny_plan_select_ladder does, with the budget left after
 * that encoder, and evaluates the rung it settles on against every limit. This
 * is the way a caller who chooses the format first and the degradation second
 * would behave.
 *
 * @param plan The plan.
 * @param opts The limits, or NULL for the defaults.
 * @param selection Receives the decision, with one entry in `considered`.
 * @return int As tiny_plan_select.
 */
int tiny_plan_select_independent(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
);

/**
 * @brief Runs the plan at one point of its lattice by index.
 *
 * Does what tiny_plan_run does with the decode scale, filter and effort of
 * the point at `index` in tiny_plan_enumerate's list; a selection names the
 * point it chose as TINYIMG_SELECTION_FIELD_CANDIDATE. Encode the result with
 * the format and tier the selection names. The index is only meaningful for
 * the plan it was chosen on, unchanged since. The plan's budget is ignored.
 *
 * @param plan The plan.
 * @param index A position in the plan's tiny_plan_enumerate list.
 * @param out Receives the output. Its previous contents are not freed.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for an index the
 * plan does not list, or a negative TinyImageError from the run.
 */
int tiny_plan_run_lattice(const TinyPlan* plan, uint32_t index, TinyImage* out);

#ifdef __cplusplus
}
#endif

#endif
