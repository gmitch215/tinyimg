/**
 * @file plan.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief The transform IR, the rewrites that run over it, and the executor.
 * @version 1.0.0
 * @date 2026-09-03
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/image.h"
#include "tinyimg/tinyimg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How many operations one plan holds.
 *
 * A fixed capacity, so a TinyPlan is an ordinary structure a caller keeps on
 * the stack and the planner never touches the allocator to build one. A chain
 * longer than this is a caller that means to run two transforms.
 */
#define TINYIMG_PLAN_MAX_OPS 32

#pragma region the ir

/**
 * @brief The operations a plan can hold.
 *
 * Appending to this enum is additive; the values are part of the ABI and are
 * read by the host wrapper, so an operation is never renumbered.
 */
typedef enum TinyPlanOpKind
{
    /**
     * @brief An empty slot, which a rewrite leaves behind and the resolved op
     * list never contains.
     */
    TINYIMG_OP_NONE = 0,
    /** Take a rectangle, in the coordinates the previous operation produced. */
    TINYIMG_OP_CROP = 1,
    /** Resample to an extent. */
    TINYIMG_OP_RESIZE = 2,
    /** Resample and crop or pad according to a TinyImageFit mode. */
    TINYIMG_OP_FIT = 3,
    /** Mirror along the vertical axis. */
    TINYIMG_OP_FLIP_H = 4,
    /** Mirror along the horizontal axis. */
    TINYIMG_OP_FLIP_V = 5,
    /** Turn by a multiple of 90 degrees clockwise. */
    TINYIMG_OP_ROTATE = 6,
    /** Scale every channel by a factor. */
    TINYIMG_OP_BRIGHTNESS = 7,
    /** Scale every channel about mid gray. */
    TINYIMG_OP_CONTRAST = 8,
    /** Move every channel toward or away from its luminance. */
    TINYIMG_OP_SATURATION = 9,
    /** Rotate the hue by an angle in degrees. */
    TINYIMG_OP_HUE = 10,
    /** Replace every channel with the pixel's luminance. */
    TINYIMG_OP_GRAYSCALE = 11,
    /** Subtract every channel from 255, leaving alpha alone. */
    TINYIMG_OP_INVERT = 12,
    /** Raise every channel to a power. */
    TINYIMG_OP_GAMMA = 13,
    /** Average each pixel with its neighbors. */
    TINYIMG_OP_BLUR = 14,
    /** Apply a caller-supplied 3x4 color matrix. */
    TINYIMG_OP_MATRIX = 15,
    /** Apply a named tone curve, per channel. */
    TINYIMG_OP_CURVE = 16,
    /** Apply a named neighborhood effect. */
    TINYIMG_OP_EFFECT = 17,
} TinyPlanOpKind;

/**
 * @brief The tone curves TINYIMG_OP_CURVE can carry.
 *
 * A curve is named and parameterized rather than carried as a table, because a
 * 768 byte table inline would make one operation larger than the whole rest of
 * a plan and a plan is a structure a caller keeps on the stack. A caller's own
 * table is still reachable: tiny_image_apply_lut takes one and runs it as a
 * single pass, which is the escape hatch rather than the default.
 */
typedef enum TinyCurveKind
{
    /** Raise to a power; `p[0]` is the exponent. */
    TINYIMG_CURVE_GAMMA = 0,
    /** Round to `p[0]` evenly spaced levels. */
    TINYIMG_CURVE_POSTERIZE = 1,
    /** Zero below `p[0]`, full above it. */
    TINYIMG_CURVE_THRESHOLD = 2,
    /** Invert above `p[0]`, leaving the rest alone. */
    TINYIMG_CURVE_SOLARIZE = 3,
    /** Scale by two to the power `p[0]`, in stops. */
    TINYIMG_CURVE_EXPOSURE = 4,
    /**
     * @brief Map `p[0]`..`p[1]` onto `p[3]`..`p[4]` through gamma `p[2]`.
     *
     * The five-number levels control, so one curve covers the whole of it and
     * a per-channel levels request is three of these with different masks.
     */
    TINYIMG_CURVE_LEVELS = 5,
    /** Lift the shadows by `p[0]` without moving the highlights. */
    TINYIMG_CURVE_FILL_LIGHT = 6,
    /** Multiply by `p[0]`. */
    TINYIMG_CURVE_GAIN = 7,
    /** An S-curve of strength `p[0]` about mid gray. */
    TINYIMG_CURVE_SIGMOID = 8,
    /** Subtract from full scale. */
    TINYIMG_CURVE_NEGATE = 9,
    /** Encode linear light as sRGB, or decode it when `p[0]` is negative. */
    TINYIMG_CURVE_SRGB = 10,
    /**
     * @brief Shift the shadows by `p[0]`, the midtones by `p[1]` and the
     * highlights by `p[2]`, each weighted by how much of the pixel is in that
     * band.
     *
     * One channel of a color balance. The three bands overlap, so a shift
     * applied to one of them does not leave a step where it meets the next.
     */
    TINYIMG_CURVE_BALANCE = 11,
} TinyCurveKind;

/**
 * @brief The neighborhood effects TINYIMG_OP_EFFECT can carry.
 *
 * Every one of these reads more than one pixel, so they share the class that
 * ends a fused pass. What separates them is the kernel and the parameters, not
 * anything the planner needs to know, which is why one operation kind carries
 * them all.
 */
typedef enum TinyEffectKind
{
    /** Unsharp mask; `p[0]` sigma, `p[1]` amount, `p[2]` threshold. */
    TINYIMG_FX_UNSHARP = 0,
    /** Local contrast, an unsharp mask at a large radius. */
    TINYIMG_FX_CLARITY = 1,
    /** Sobel gradient magnitude. */
    TINYIMG_FX_SOBEL = 2,
    /** Directional 3x3 emboss. */
    TINYIMG_FX_EMBOSS = 3,
    /** Average over `p[0]` by `p[0]` blocks. */
    TINYIMG_FX_PIXELATE = 4,
    /** Replace each pixel with the median of its 3x3 neighborhood. */
    TINYIMG_FX_MEDIAN = 5,
    /** Maximum over a `p[0]` radius. */
    TINYIMG_FX_DILATE = 6,
    /** Minimum over a `p[0]` radius. */
    TINYIMG_FX_ERODE = 7,
    /** Difference between a dilation and an erosion. */
    TINYIMG_FX_OUTLINE = 8,
    /** Average along `p[0]` pixels at `p[1]` degrees. */
    TINYIMG_FX_MOTION_BLUR = 9,
    /** Average along arcs about the center; `p[0]` is the strength. */
    TINYIMG_FX_RADIAL_BLUR = 10,
    /** Average along rays from the center; `p[0]` is the strength. */
    TINYIMG_FX_ZOOM_BLUR = 11,
    /** A blur that grows away from a band; `p[0]` sigma, `p[1]` band. */
    TINYIMG_FX_TILT_SHIFT = 12,
    /** Gaussian blur inside the rectangle only. */
    TINYIMG_FX_BLUR_REGION = 13,
    /** Pixelate inside the rectangle only. */
    TINYIMG_FX_PIXELATE_REGION = 14,
    /** Offset the red and blue channels by `p[0]` pixels. */
    TINYIMG_FX_CHROMATIC = 15,
    /** Ordered dither to `p[0]` levels through a Bayer matrix. */
    TINYIMG_FX_DITHER = 16,
    /** Cluster-dot halftone at cell size `p[0]`. */
    TINYIMG_FX_HALFTONE = 17,
    /** Darken every `p[0]`th row by `p[1]`. */
    TINYIMG_FX_SCANLINES = 18,
} TinyEffectKind;

/**
 * @brief What the planner needs to know about an operation to place it.
 *
 * The class is the whole contract for adding an operation: pick the one that
 * describes how the operation reads its input, and every rewrite, the ROI walk
 * and the executor already know what to do with it. Nothing else in the planner
 * is written per operation.
 */
typedef enum TinyPlanOpClass
{
    /**
     * @brief Moves pixels without changing their values.
     *
     * Folds into the accumulated source window, the sample map and the
     * orientation, so any number of these cost one pass between them.
     */
    TINYIMG_OP_CLASS_GEOMETRY = 0,
    /**
     * @brief Reads one pixel and is affine in its channels.
     *
     * Composes with its neighbors by matrix multiplication, so any number of
     * these cost one matrix between them.
     */
    TINYIMG_OP_CLASS_COLOR_MATRIX = 1,
    /**
     * @brief Reads one pixel and is not affine in its channels.
     *
     * Composes with its neighbors through the lookup table, so any number of
     * these cost one table between them.
     */
    TINYIMG_OP_CLASS_COLOR_LUT = 2,
    /**
     * @brief Reads a neighborhood around each pixel.
     *
     * Cannot fold into a sample map, so it ends a fused pass and its input is
     * materialized. It is also what stops resolution propagation, unless a
     * rewrite has already moved it after the downscale.
     */
    TINYIMG_OP_CLASS_NEIGHBORHOOD = 3,
} TinyPlanOpClass;

/**
 * @brief Which weights the resampler reads a source pixel through.
 */
typedef enum TinyResampleFilter
{
    /**
     * @brief Box when the axis is being reduced, Catmull-Rom when it is being
     * enlarged, chosen per axis.
     *
     * The right answer for almost every request: an area average is what a
     * reduction means, and it is what every codec in this library does for its
     * own scaled decode, so a plan that reduces through the decoder and then
     * through the resampler is doing one kind of thing twice rather than two
     * different kinds once.
     */
    TINYIMG_FILTER_AUTO = 0,
    /** One source pixel per output pixel. */
    TINYIMG_FILTER_NEAREST = 1,
    /** Two samples per axis, weighted by the fractional position. */
    TINYIMG_FILTER_BILINEAR = 2,
    /** Every source pixel the output pixel covers, averaged. */
    TINYIMG_FILTER_BOX = 3,
    /** Four samples per axis through the Catmull-Rom cubic. */
    TINYIMG_FILTER_CATMULL_ROM = 4,
} TinyResampleFilter;

/**
 * @brief One operation and its operands.
 *
 * The layout is not part of the ABI; tiny_plan_op_at reads an operation out for
 * a host, and tiny_plan_sizeof gives it the byte count to reserve.
 */
typedef struct {
    /** Which operation this is. */
    TinyPlanOpKind kind;

    /** The operands, read according to `kind`. */
    union {
        struct {
            /** Left edge, in the coordinates the previous operation produced.
             */
            uint32_t x;
            /** Top edge. */
            uint32_t y;
            /** Width; zero means to the right edge. */
            uint32_t width;
            /** Height; zero means to the bottom edge. */
            uint32_t height;
        } crop; /**< Operands of TINYIMG_OP_CROP. */

        struct {
            /** Target width; zero keeps the aspect ratio. */
            uint32_t width;
            /** Target height; zero keeps the aspect ratio. */
            uint32_t height;
            /** Weights to sample through. */
            TinyResampleFilter filter;
        } resize; /**< Operands of TINYIMG_OP_RESIZE. */

        struct {
            /** Target width. */
            uint32_t width;
            /** Target height. */
            uint32_t height;
            /** How the aspect mismatch is absorbed and the scale clamped. */
            TinyImageFit mode;
            /** Which part a crop keeps, or where a pad puts the image. */
            TinyImageGravity gravity;
            /**
             * @brief Where the interesting part is, 0 through 1 across each
             * axis.
             *
             * Only read when `focused` is set, and only meaningful for the two
             * computed gravities. It exists so tiny_plan_resolve stays a
             * function of the plan alone: the question "where are the faces"
             * needs pixels, and resolution happens before any pixel is read, so
             * tiny_plan_run answers it first and writes the answer here. A fit
             * op that reaches resolution without one falls back to the center.
             */
            float focus_x;
            /** The vertical position; see `focus_x`. */
            float focus_y;
            /** Non-zero when `focus_x` and `focus_y` have been resolved. */
            uint8_t focused;
        } fit; /**< Operands of TINYIMG_OP_FIT. */

        struct {
            /** Quarter turns clockwise, 0 through 3. */
            uint32_t turns;
        } rotate; /**< Operands of TINYIMG_OP_ROTATE. */

        struct {
            /** Box radius in pixels, or the gaussian's sigma. */
            float amount;
            /** Non-zero for three box passes rather than one. */
            uint8_t gaussian;
        } blur; /**< Operands of TINYIMG_OP_BLUR. */

        struct {
            /**
             * @brief Row major 3x4, applied to RGB.
             *
             * Three channel weights and a constant per row, in the 0..255
             * range the pixels are in, so a row of {1,0,0,0} is the identity
             * and a constant of 255 is full scale.
             */
            float m[12];
        } matrix; /**< Operands of TINYIMG_OP_MATRIX. */

        struct {
            /** Which curve. */
            TinyCurveKind kind;
            /** Its parameters, read according to `kind`. */
            float p[5];
            /**
             * @brief Which channels it applies to, bit 0 red through bit 2
             * blue.
             *
             * Zero means all three, so an operation built by zeroing the
             * structure and naming a curve does what a caller expects.
             */
            uint8_t channels;
        } curve; /**< Operands of TINYIMG_OP_CURVE. */

        struct {
            /** Which effect. */
            TinyEffectKind kind;
            /** Its parameters, read according to `kind`. */
            float p[4];
            /** The rectangle the region effects act inside. */
            uint32_t x;
            /** Top edge of that rectangle. */
            uint32_t y;
            /** Width of that rectangle; zero means the whole image. */
            uint32_t width;
            /** Height of that rectangle. */
            uint32_t height;
        } effect; /**< Operands of TINYIMG_OP_EFFECT. */

        struct {
            /** The factor, angle or exponent. */
            float value;
        } scalar; /**< Operands of the operations that take one number:
                       TINYIMG_OP_BRIGHTNESS, TINYIMG_OP_CONTRAST,
                       TINYIMG_OP_SATURATION, TINYIMG_OP_HUE and
                       TINYIMG_OP_GAMMA. TINYIMG_OP_GRAYSCALE and
                       TINYIMG_OP_INVERT take nothing and leave it at zero. */
    };
} TinyPlanOp;

/**
 * @brief A sequence of operations over one source, and the source itself.
 *
 * Operations do not run when they are appended. The plan runs once, when
 * tiny_plan_run asks for the output, and the planner decides on the way what to
 * decode and how much of it. That is the whole reason the type exists: a chain
 * that ends at a 100x100 thumbnail of a 16 megapixel photograph should never
 * have 16 megapixels in memory, and it cannot make that decision after the
 * first operation has already run.
 *
 * Keep one on the stack. The layout is not part of the ABI.
 */
typedef struct {
    /** Encoded source bytes, or NULL when the source is already decoded. */
    const uint8_t* buffer;
    /** Length of `buffer`. */
    size_t size;
    /** Decoded source, or NULL when the source is encoded. Borrowed. */
    const TinyImage* image;

    /** Width of the source in pixels. */
    uint32_t source_width;
    /** Height of the source in pixels. */
    uint32_t source_height;
    /** Channels the source decodes to. */
    uint8_t source_channels;
    /** The source's container format. */
    TinyImageFormat source_format;

    /** Zero to run one operation per pass; see tiny_plan_set_fusion. */
    uint8_t fusion;
    /** How much work the decode may spend; see tiny_plan_set_effort. */
    uint8_t effort;
    /** What padding is filled with, as many channels as the output has. */
    uint8_t background[4];

    /** How many operations have been appended. */
    uint32_t count;
    /** The operations, in the order they were appended. */
    TinyPlanOp ops[TINYIMG_PLAN_MAX_OPS];
} TinyPlan;

#pragma endregion

#pragma region resolution

/**
 * @brief Special cases the planner took, reported as a bitmask.
 *
 * Every bit is an assertion a test can make about a plan rather than about its
 * pixels, which is the only way to tell a plan that was optimized from one that
 * happened to produce the same image.
 */
typedef enum TinyPlanKernel
{
    /** The pass reads a rectangle of its source rather than all of it. */
    TINYIMG_KERNEL_REGION = 1 << 0,
    /** The decode ran at a scale denominator above 1. */
    TINYIMG_KERNEL_SCALED = 1 << 1,
    /** The output is the decoded pixels with nothing done to them. */
    TINYIMG_KERNEL_COPY = 1 << 2,
    /** A resample runs. */
    TINYIMG_KERNEL_RESAMPLE = 1 << 3,
    /** A flip or a quarter turn runs, folded into the output addressing. */
    TINYIMG_KERNEL_ORIENT = 1 << 4,
    /** At least one color stage runs. */
    TINYIMG_KERNEL_COLOR = 1 << 5,
    /** The output is larger than the resampled image and the rest is filled. */
    TINYIMG_KERNEL_PAD = 1 << 6,
    /** The decoder produced the luminance, so no color stage was needed. */
    TINYIMG_KERNEL_GRAY_DECODE = 1 << 7,
    /** A neighborhood operation runs on a materialized image. */
    TINYIMG_KERNEL_NEIGHBORHOOD = 1 << 8,
} TinyPlanKernel;

/**
 * @brief What the planner decided, before any pixel is touched.
 *
 * Reported by tiny_plan_resolve so a caller can see the saving, and so a test
 * can assert the decision rather than infer it from the output. The by-hand
 * check in the plan prints `decode` and `kernels` from here.
 */
typedef struct {
    /** Region, scale and channel count the decoder is asked for. */
    TinyDecodeOpts decode;
    /** Width the decode produces. */
    uint32_t decode_width;
    /** Height the decode produces. */
    uint32_t decode_height;

    /**
     * @brief The window of the decoded image the resample reads, in decoded
     * pixels.
     *
     * Fractional, and that is the point: a crop applied after a resize adjusts
     * this window and the extent below, so the sample positions stay exactly
     * where the uncropped resize would have put them. Deriving a fresh resize
     * from the integer crop instead moves every sample by the fractional part.
     */
    double source_x;
    /** Top edge of that window. */
    double source_y;
    /** Width of that window. */
    double source_width;
    /** Height of that window. */
    double source_height;

    /** Width the resample produces, before orientation. */
    uint32_t sample_width;
    /** Height the resample produces, before orientation. */
    uint32_t sample_height;
    /** Filter chosen for the horizontal axis, never TINYIMG_FILTER_AUTO. */
    TinyResampleFilter filter_x;
    /** Filter chosen for the vertical axis. */
    TinyResampleFilter filter_y;

    /**
     * @brief The composed flip and quarter turn, as a signed permutation.
     *
     * Row major, mapping output coordinates to sample coordinates about each
     * axis' center. Composition of any number of flips and turns is one matrix
     * multiply, so the eight orientations cost one pass between them.
     */
    int8_t orientation[4];

    /** Width of the final image. */
    uint32_t width;
    /** Height of the final image. */
    uint32_t height;
    /** Channels of the final image. */
    uint8_t channels;
    /** Where the oriented result sits in the final image. */
    uint32_t offset_x;
    /** Where the oriented result sits in the final image. */
    uint32_t offset_y;

    /** Operations left after the rewrites. */
    uint32_t ops;
    /** Those operations, which is what the rewrite tests assert on. */
    TinyPlanOp op[TINYIMG_PLAN_MAX_OPS];
    /** Operations an identity or annihilation rule removed. */
    uint32_t eliminated;
    /** Operations a pair rule merged into another. */
    uint32_t collapsed;
    /** Color stages the operations collapsed into. */
    uint32_t color_stages;
    /**
     * @brief How many of those run on the source samples rather than the output
     * ones.
     *
     * Which is decided by where the caller put them: a color operation written
     * before a resize runs before the resample. Moving one to the output side
     * would be cheaper on a reduction and is not the same image, because the
     * clamp that follows an affine function does not survive being averaged.
     */
    uint32_t color_stages_before;
    /** Operations of `op` this pass consumes; the rest run after it. */
    uint32_t consumed;
    /** Fused passes tiny_plan_run will make. */
    uint32_t passes;
    /** A bitmask of TinyPlanKernel. */
    uint32_t kernels;
} TinyPlanResolution;

/**
 * @brief One collapsed color operation.
 *
 * A chain of color operations becomes a list of these, and adjacent entries of
 * the same kind are merged into one, so the worked chain of a brightness, a
 * contrast, a saturation and a gamma is one matrix and one table.
 */
typedef struct {
    /**
     * @brief TINYIMG_OP_CLASS_COLOR_MATRIX or TINYIMG_OP_CLASS_COLOR_LUT.
     */
    TinyPlanOpClass kind;
    /**
     * @brief Row major 3x4 in 16.16 fixed point, applied to RGB.
     *
     * Each row is three channel weights and a constant. Alpha is not read and
     * not written; no color operation in the library touches it.
     */
    int32_t matrix[12];
    /** One 256 entry table per channel. */
    uint8_t lut[3][256];
} TinyColorStage;

#pragma endregion

#pragma region building

/**
 * @brief Size of a TinyPlan, for a host allocating one across the wasm
 * boundary.
 *
 * @return uint32_t sizeof(TinyPlan).
 */
uint32_t tiny_plan_sizeof(void);

/**
 * @brief Size of a TinyPlanResolution, for the same reason.
 *
 * @return uint32_t sizeof(TinyPlanResolution).
 */
uint32_t tiny_plan_resolution_sizeof(void);

/**
 * @brief Starts a plan over encoded bytes.
 *
 * Reads the header to learn the source's dimensions and nothing else; no pixel
 * is decoded until tiny_plan_run, which is what lets the planner choose the
 * decode. The buffer is borrowed and must outlive the plan.
 *
 * @param plan The plan to initialize.
 * @param buffer The encoded image.
 * @param size Number of bytes.
 * @return int TINYIMG_OK, TINYIMG_ERR_UNKNOWN_FORMAT, or a negative
 * TinyImageError from the probe.
 */
int tiny_plan_init(TinyPlan* plan, const uint8_t* buffer, size_t size);

/**
 * @brief Starts a plan over pixels that are already decoded.
 *
 * The escape hatch, and the form the eager operations in image.h are written
 * in. There is no decode to choose, so the region and resolution propagation
 * have nothing to do; every other rewrite still runs. The image is borrowed and
 * is never written to.
 *
 * @param plan The plan to initialize.
 * @param image The source pixels.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_plan_init_image(TinyPlan* plan, const TinyImage* image);

/**
 * @brief Chooses whether the plan fuses.
 *
 * On, which is the default, the plan collapses into as few passes as the
 * operations allow.
 *
 * Off, nothing is rewritten, the source is decoded whole, and every operation
 * as appended runs as its own pass over a materialized image. That is what the
 * fused path is measured against, and it is the benchmark's planner-off arm.
 * Both paths share one resampler, so a difference between them is a fault in
 * the rewrites, the collapse or the region arithmetic, and cannot be a fault in
 * the sampling.
 *
 * @param plan The plan.
 * @param enabled Non-zero to fuse.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_plan_set_fusion(TinyPlan* plan, int enabled);

/**
 * @brief Chooses how much work the plan's decode may spend.
 *
 * TINYIMG_EFFORT_FANCY, the default, decodes to the bitstream's definition.
 * TINYIMG_EFFORT_FAST lets a lossy decoder drop its smoothing pass: VP8 skips
 * deblocking and JPEG replicates chroma rather than interpolating it. A
 * lossless format has nothing to drop and is unaffected.
 *
 * Separate from the encoder's effort, which is set on TinyEncodeOpts, because a
 * request can want one and not the other: a thumbnail small enough to hide a
 * decode approximation may still want a carefully searched encode.
 *
 * @param plan The plan.
 * @param effort A TinyEffort.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_RANGE for a value
 * that is not a TinyEffort.
 */
int tiny_plan_set_effort(TinyPlan* plan, uint8_t effort);

/**
 * @brief Sets what padding is filled with.
 *
 * @param plan The plan.
 * @param color As many channels as the output will have. NULL restores the
 * default, which is transparent, or black when the output has no alpha.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_plan_background(TinyPlan* plan, const uint8_t* color);

/**
 * @brief Appends a crop.
 *
 * The rectangle is in the coordinates the previous operation produces, not in
 * source pixels, so a crop after a resize means what it reads like.
 *
 * @param plan The plan.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width; zero means to the right edge.
 * @param height Height; zero means to the bottom edge.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN when the plan
 * is full.
 */
int tiny_plan_crop(
    TinyPlan* plan, uint32_t x, uint32_t y, uint32_t width, uint32_t height
);

/**
 * @brief Appends a resize with the filter the planner would choose.
 *
 * @param plan The plan.
 * @param width Target width. Zero keeps the aspect ratio against `height`.
 * @param height Target height. Zero keeps the aspect ratio against `width`.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE when both are
 * zero, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_resize(TinyPlan* plan, uint32_t width, uint32_t height);

/**
 * @brief Appends a resize through a named filter.
 *
 * @param plan The plan.
 * @param width Target width. Zero keeps the aspect ratio.
 * @param height Target height. Zero keeps the aspect ratio.
 * @param filter The weights to sample through.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE, or
 * TINYIMG_ERR_PLAN.
 */
int tiny_plan_resize_with(
    TinyPlan* plan, uint32_t width, uint32_t height, TinyResampleFilter filter
);

/**
 * @brief Appends a fit, which resolves to a scale and a crop or a pad.
 *
 * @param plan The plan.
 * @param width Target width.
 * @param height Target height.
 * @param mode How the aspect mismatch is absorbed and how the scale is
 * clamped; see TinyImageFit.
 * @param gravity Which part of the image a crop keeps, or where a pad puts it.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE, or
 * TINYIMG_ERR_PLAN.
 */
int tiny_plan_fit(
    TinyPlan* plan, uint32_t width, uint32_t height, TinyImageFit mode,
    TinyImageGravity gravity
);

/**
 * @brief Appends a horizontal flip.
 *
 * @param plan The plan.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_flip_horizontal(TinyPlan* plan);

/**
 * @brief Appends a vertical flip.
 *
 * @param plan The plan.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_flip_vertical(TinyPlan* plan);

/**
 * @brief Appends a turn.
 *
 * @param plan The plan.
 * @param degrees Clockwise, and a multiple of 90. Negative and past 360 are
 * both reduced.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE when the angle is
 * not a multiple of 90, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_rotate(TinyPlan* plan, int32_t degrees);

/**
 * @brief Appends a brightness change.
 *
 * @param plan The plan.
 * @param factor 1.0 changes nothing and is eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a negative
 * factor, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_brightness(TinyPlan* plan, float factor);

/**
 * @brief Appends a contrast change.
 *
 * @param plan The plan.
 * @param factor 1.0 changes nothing and is eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a negative
 * factor, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_contrast(TinyPlan* plan, float factor);

/**
 * @brief Appends a saturation change.
 *
 * @param plan The plan.
 * @param factor 1.0 changes nothing and is eliminated; 0.0 is a grayscale that
 * keeps three channels.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a negative
 * factor, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_saturation(TinyPlan* plan, float factor);

/**
 * @brief Appends a hue rotation.
 *
 * @param plan The plan.
 * @param degrees Any angle; a multiple of 360 changes nothing and is
 * eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_hue(TinyPlan* plan, float degrees);

/**
 * @brief Appends a conversion to luminance.
 *
 * The output keeps its alpha channel and loses the other two, so an RGBA source
 * becomes two channels and an RGB source becomes one.
 *
 * @param plan The plan.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_grayscale(TinyPlan* plan);

/**
 * @brief Appends an inversion.
 *
 * @param plan The plan.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_invert(TinyPlan* plan);

/**
 * @brief Appends a gamma correction.
 *
 * @param plan The plan.
 * @param gamma Above 1.0 darkens; 1.0 changes nothing and is eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a
 * non-positive gamma, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_gamma(TinyPlan* plan, float gamma);

/**
 * @brief Appends a box blur.
 *
 * @param plan The plan.
 * @param radius Pixels either side. Zero changes nothing and is eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a negative
 * radius, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_blur(TinyPlan* plan, float radius);

/**
 * @brief Appends a gaussian blur.
 *
 * Three box passes, which converge on a gaussian and cost the same as one
 * regardless of sigma.
 *
 * @param plan The plan.
 * @param sigma Standard deviation in pixels. Zero changes nothing and is
 * eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a negative
 * sigma, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_gaussian_blur(TinyPlan* plan, float sigma);

/**
 * @brief Appends a color matrix.
 *
 * The generic form of every affine color operation in the library. Sepia, a
 * channel mixer, a white balance and a colorblind simulation are all one of
 * these, so they compose with each other and with brightness, contrast,
 * saturation and hue into the single matrix the executor applies.
 *
 * @param plan The plan.
 * @param matrix Row major 3x4 applied to RGB, in the 0..255 range. The
 * identity is eliminated.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_matrix(TinyPlan* plan, const float* matrix);

/**
 * @brief Appends a tone curve.
 *
 * The generic form of every color operation that is not affine. Adjacent
 * curves compose through the table, so any number of them cost one table.
 *
 * @param plan The plan.
 * @param kind Which curve.
 * @param params Its parameters, up to five, read according to `kind`. NULL is
 * the same as all zero.
 * @param channels Which channels to apply it to, bit 0 red through bit 2 blue;
 * zero means all three.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a parameter
 * the curve cannot take, or TINYIMG_ERR_PLAN.
 */
int tiny_plan_curve(
    TinyPlan* plan, TinyCurveKind kind, const float* params, uint8_t channels
);

/**
 * @brief Appends a neighborhood effect.
 *
 * @param plan The plan.
 * @param kind Which effect.
 * @param params Its parameters, up to four. NULL is the same as all zero.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE, or
 * TINYIMG_ERR_PLAN.
 */
int tiny_plan_effect(TinyPlan* plan, TinyEffectKind kind, const float* params);

/**
 * @brief Appends a neighborhood effect confined to a rectangle.
 *
 * @param plan The plan.
 * @param kind Which effect.
 * @param params Its parameters, up to four. NULL is the same as all zero.
 * @param x Left edge.
 * @param y Top edge.
 * @param width Width; zero means to the right edge.
 * @param height Height; zero means to the bottom edge.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE, or
 * TINYIMG_ERR_PLAN.
 */
int tiny_plan_effect_rect(
    TinyPlan* plan, TinyEffectKind kind, const float* params, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height
);

#pragma endregion

#pragma region inspecting

/**
 * @brief How many operations a plan holds, before any rewrite.
 *
 * @param plan The plan.
 * @return uint32_t The count, or 0 when the plan is NULL.
 */
uint32_t tiny_plan_count(const TinyPlan* plan);

/**
 * @brief Reads one appended operation out.
 *
 * @param plan The plan.
 * @param index Zero based, below tiny_plan_count.
 * @param op Receives the operation.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS.
 */
int tiny_plan_op_at(const TinyPlan* plan, uint32_t index, TinyPlanOp* op);

/**
 * @brief What class the planner puts an operation in.
 *
 * @param kind The operation.
 * @return TinyPlanOpClass Its class; an unknown kind reads as geometry, which
 * is the class that assumes the least about the pixels.
 */
TinyPlanOpClass tiny_plan_op_class(TinyPlanOpKind kind);

/**
 * @brief What running this plan is expected to cost, in microseconds.
 *
 * For deciding whether a request fits a CPU budget before spending any of it.
 * The plan is resolved, which reads the source header and no pixels, so this
 * costs about as much as tiny_plan_resolve and nothing like the plan itself.
 *
 * **This is an estimate and says so.** The rates come from
 * `scripts/measure/calibrate.ts` on one machine, and a machine of a different
 * speed wants all of them scaled. Measured against real transforms it lands
 * within about 20%, which is the accuracy a budget question needs when the
 * question is whether 7 milliseconds of work will fit inside 10. It is not a
 * substitute for measuring: a caller that needs to know what a request cost
 * should read the work counters afterwards.
 *
 * The encoder is not included, because a plan does not carry one. Add
 * tiny_encode_cost for the format being written.
 *
 * @param plan The plan to price.
 * @return uint32_t Microseconds, or 0 when the plan cannot be resolved or its
 * source header cannot be read.
 */
uint32_t tiny_plan_cost(const TinyPlan* plan);

/**
 * @brief What encoding an image of this extent is expected to cost.
 *
 * Separate from tiny_plan_cost so a caller choosing between formats can price
 * each one without building a plan per candidate. The spread is the reason the
 * function exists: at the rates measured, PNG costs 29 times JPEG per sample
 * and WebP costs 4, so a request that does not fit as WebP may fit as JPEG.
 *
 * @param format The format to write.
 * @param width Output width.
 * @param height Output height.
 * @return uint32_t Microseconds, or 0 for a format this build cannot write.
 */
uint32_t tiny_encode_cost(
    TinyImageFormat format, uint32_t width, uint32_t height
);

/**
 * @brief A field of a TinyPlanResolution, named rather than offset.
 *
 * TinyPlanResolution's layout is not part of the ABI, so a host that read it by
 * computing offsets would be reading whatever the C compiler chose that day.
 * The awkward part is not the prefix, which is fixed; it is that
 * `TinyPlanOp op[TINYIMG_PLAN_MAX_OPS]` sits in the middle, so every counter
 * after it moves whenever an operand grows. Naming the fields is what makes the
 * decision readable from the outside without pinning the structure.
 */
typedef enum TinyPlanField
{
    /** Left edge of the region the decoder is asked for, in source pixels. */
    TINYIMG_FIELD_REGION_X = 0,
    /** Top edge of that region. */
    TINYIMG_FIELD_REGION_Y = 1,
    /** Width of that region. */
    TINYIMG_FIELD_REGION_WIDTH = 2,
    /** Height of that region. */
    TINYIMG_FIELD_REGION_HEIGHT = 3,
    /** Subsampling denominator the decoder is asked for: 1, 2, 4 or 8. */
    TINYIMG_FIELD_SCALE = 4,
    /** Width the decode produces. */
    TINYIMG_FIELD_DECODE_WIDTH = 5,
    /** Height the decode produces. */
    TINYIMG_FIELD_DECODE_HEIGHT = 6,
    /** Width of the final image. */
    TINYIMG_FIELD_WIDTH = 7,
    /** Height of the final image. */
    TINYIMG_FIELD_HEIGHT = 8,
    /** Channels of the final image. */
    TINYIMG_FIELD_CHANNELS = 9,
    /** Operations left after the rewrites. */
    TINYIMG_FIELD_OPS = 10,
    /** Operations an identity or annihilation rule removed. */
    TINYIMG_FIELD_ELIMINATED = 11,
    /** Operations a pair rule merged into another. */
    TINYIMG_FIELD_COLLAPSED = 12,
    /** Color stages the operations collapsed into. */
    TINYIMG_FIELD_COLOR_STAGES = 13,
    /** Fused passes tiny_plan_run will make. */
    TINYIMG_FIELD_PASSES = 14,
    /** A bitmask of TinyPlanKernel. */
    TINYIMG_FIELD_KERNELS = 15,
} TinyPlanField;

/**
 * @brief Reads one named field of a resolution.
 *
 * One accessor rather than sixteen, because every field a host wants is an
 * unsigned integer and sixteen exports would cost more module bytes than the
 * switch does.
 *
 * @param resolution A resolution from tiny_plan_resolve.
 * @param field Which field.
 * @return uint32_t Its value, or 0 for a NULL resolution or an unknown field.
 */
uint32_t tiny_plan_field(
    const TinyPlanResolution* resolution, TinyPlanField field
);

/**
 * @brief Runs the rewrites and the propagation, touching no pixels.
 *
 * Everything the planner decides is here: which operations survived, what
 * rectangle of the source at what scale the decoder is asked for, the window
 * and extent the resample works over, the composed orientation, and which
 * special cases were taken. Calling it is free relative to a decode, so a
 * caller that wants to log the saving can.
 *
 * @param plan The plan.
 * @param resolution Receives the decision.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for an operation
 * that leaves nothing to produce, or TINYIMG_ERR_TOO_LARGE past
 * TINYIMG_MAX_PIXELS or TINYIMG_MAX_IMAGE_BYTES. Both caps are checked here
 * rather than left to the executor, so a plan that resolves is a plan that can
 * allocate its output.
 */
int tiny_plan_resolve(const TinyPlan* plan, TinyPlanResolution* resolution);

/**
 * @brief Collapses a resolved plan's color operations into stages.
 *
 * @internal Not part of the public surface; the executor calls it, and it is
 * declared here so a test can assert the composed matrix and table directly
 * rather than inferring them from pixels.
 *
 * @param resolution A resolved plan.
 * @param stages Receives the stages.
 * @param capacity How many `stages` holds; the resolution's operation count is
 * always enough.
 * @param count Receives how many were written.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BUFFER_TOO_SMALL.
 */
int tiny_plan_color_stages(
    const TinyPlanResolution* resolution, TinyColorStage* stages,
    uint32_t capacity, uint32_t* count
);

/**
 * @brief Runs a plan over an image and puts the result in its place.
 *
 * @internal What every eager operation in the library is: an operation called
 * on its own is a plan with one operation in it, and a named effect that is
 * really three adjustments is a plan with three, which then collapse into the
 * single pass the planner would have made anyway. Metadata moves across rather
 * than being freed with the image it came from.
 *
 * @param image The image, replaced by the result. Must be the plan's source.
 * @param plan A plan initialized over `image`.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_plan_replace(TinyImage* image, TinyPlan* plan);

/**
 * @brief Runs one TINYIMG_OP_EFFECT operation over a materialized image.
 *
 * @internal The seam between the planner and the effects. The planner decides
 * where a neighborhood operation runs and what it reads; what the kernel does
 * is none of its business, so the whole of it lives in effects.c and this is
 * the only thing plan.c calls.
 *
 * @param image The image, replaced in place by the result.
 * @param op The operation, whose kind is TINYIMG_OP_EFFECT.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_effect_apply(TinyImage* image, const TinyPlanOp* op);

/**
 * @brief Blurs an image with one box pass of the given radius.
 *
 * @internal The sliding-window box blur, which is radius independent and is
 * what every blur in the library is built from. Exposed so the effects that
 * need a blurred copy of their input do not each grow one.
 *
 * @param image The image, blurred in place.
 * @param radius Pixels either side. Zero does nothing.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_plan_blur_box(TinyImage* image, uint32_t radius);

#pragma endregion

#pragma region running

/**
 * @brief Runs the plan and produces the output image.
 *
 * @param plan The plan.
 * @param out Receives the output. Its previous contents are not freed.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_plan_run(const TinyPlan* plan, TinyImage* out);

/**
 * @brief Runs the plan and encodes the result in one call.
 *
 * @param plan The plan.
 * @param format The container to write.
 * @param opts Quality and related settings, or NULL for the defaults.
 * @param writer An initialized TinyWriter to append to.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_plan_encode(
    const TinyPlan* plan, TinyImageFormat format, const TinyEncodeOpts* opts,
    TinyWriter* writer
);

#pragma endregion

#ifdef __cplusplus
}
#endif
