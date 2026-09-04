#include "tinyimg/plan.h"

#include "tinyimg/codec/codec.h"
#include "tinyimg/detect.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

#pragma region operations

TINYIMG_EXPORT("tiny_plan_sizeof")
uint32_t tiny_plan_sizeof(void) {
    return (uint32_t) sizeof(TinyPlan);
}

TINYIMG_EXPORT("tiny_plan_resolution_sizeof")
uint32_t tiny_plan_resolution_sizeof(void) {
    return (uint32_t) sizeof(TinyPlanResolution);
}

TinyPlanOpClass tiny_plan_op_class(TinyPlanOpKind kind) {
    switch (kind) {
        case TINYIMG_OP_BRIGHTNESS:
        case TINYIMG_OP_CONTRAST:
        case TINYIMG_OP_SATURATION:
        case TINYIMG_OP_HUE:
        case TINYIMG_OP_GRAYSCALE:
        case TINYIMG_OP_INVERT:
        case TINYIMG_OP_MATRIX: return TINYIMG_OP_CLASS_COLOR_MATRIX;
        case TINYIMG_OP_GAMMA:
        case TINYIMG_OP_CURVE: return TINYIMG_OP_CLASS_COLOR_LUT;
        case TINYIMG_OP_BLUR:
        case TINYIMG_OP_EFFECT: return TINYIMG_OP_CLASS_NEIGHBOURHOOD;
        default: return TINYIMG_OP_CLASS_GEOMETRY;
    }
}

static int plan_append(TinyPlan* plan, const TinyPlanOp* op) {
    if (!plan) return TINYIMG_ERR_NULL;
    if (plan->count >= TINYIMG_PLAN_MAX_OPS) return TINYIMG_ERR_PLAN;

    plan->ops[plan->count++] = *op;
    return TINYIMG_OK;
}

static int plan_scalar(TinyPlan* plan, TinyPlanOpKind kind, float value) {
    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = kind;
    op.scalar.value = value;

    return plan_append(plan, &op);
}

static int plan_start(TinyPlan* plan) {
    if (!plan) return TINYIMG_ERR_NULL;

    tiny_memset(plan, 0, sizeof(*plan));
    plan->fusion = 1;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_init")
int tiny_plan_init(TinyPlan* plan, const uint8_t* buffer, size_t size) {
    if (!plan || !buffer) return TINYIMG_ERR_NULL;

    TinyImageInfo info;
    int result = tiny_image_probe(buffer, size, &info);
    if (result != TINYIMG_OK) return result;

    plan_start(plan);

    plan->buffer = buffer;
    plan->size = size;
    plan->source_width = info.width;
    plan->source_height = info.height;
    plan->source_channels = info.channels;
    plan->source_format = info.format;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_init_image")
int tiny_plan_init_image(TinyPlan* plan, const TinyImage* image) {
    if (!plan || !image || !image->data) return TINYIMG_ERR_NULL;

    plan_start(plan);

    plan->image = image;
    plan->source_width = image->width;
    plan->source_height = image->height;
    plan->source_channels = image->channels;
    plan->source_format = image->format;

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_set_fusion")
int tiny_plan_set_fusion(TinyPlan* plan, int enabled) {
    if (!plan) return TINYIMG_ERR_NULL;

    plan->fusion = enabled ? 1u : 0u;
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_background")
int tiny_plan_background(TinyPlan* plan, const uint8_t* color) {
    if (!plan) return TINYIMG_ERR_NULL;

    if (!color) {
        tiny_memset(plan->background, 0, sizeof(plan->background));
        return TINYIMG_OK;
    }

    for (uint32_t i = 0; i < 4; i++) plan->background[i] = color[i];
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_crop")
int tiny_plan_crop(
    TinyPlan* plan, uint32_t x, uint32_t y, uint32_t width, uint32_t height
) {
    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_CROP;
    op.crop.x = x;
    op.crop.y = y;
    op.crop.width = width;
    op.crop.height = height;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_resize_with")
int tiny_plan_resize_with(
    TinyPlan* plan, uint32_t width, uint32_t height, TinyResampleFilter filter
) {
    if (width == 0 && height == 0) return TINYIMG_ERR_RANGE;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_RESIZE;
    op.resize.width = width;
    op.resize.height = height;
    op.resize.filter = filter;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_resize")
int tiny_plan_resize(TinyPlan* plan, uint32_t width, uint32_t height) {
    return tiny_plan_resize_with(plan, width, height, TINYIMG_FILTER_AUTO);
}

TINYIMG_EXPORT("tiny_plan_fit")
int tiny_plan_fit(
    TinyPlan* plan, uint32_t width, uint32_t height, TinyImageFit mode,
    TinyImageGravity gravity
) {
    if (width == 0 || height == 0) return TINYIMG_ERR_RANGE;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_FIT;
    op.fit.width = width;
    op.fit.height = height;
    op.fit.mode = mode;
    op.fit.gravity = gravity;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_flip_horizontal")
int tiny_plan_flip_horizontal(TinyPlan* plan) {
    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_FLIP_H;
    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_flip_vertical")
int tiny_plan_flip_vertical(TinyPlan* plan) {
    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_FLIP_V;
    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_rotate")
int tiny_plan_rotate(TinyPlan* plan, int32_t degrees) {
    if (degrees % 90 != 0) return TINYIMG_ERR_RANGE;

    // C truncates a negative quotient toward zero, so a negative angle needs
    // the extra turn before the reduction rather than after it
    int32_t turns = ((degrees / 90) % 4 + 4) % 4;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_ROTATE;
    op.rotate.turns = (uint32_t) turns;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_brightness")
int tiny_plan_brightness(TinyPlan* plan, float factor) {
    if (factor < 0.0f) return TINYIMG_ERR_RANGE;
    return plan_scalar(plan, TINYIMG_OP_BRIGHTNESS, factor);
}

TINYIMG_EXPORT("tiny_plan_contrast")
int tiny_plan_contrast(TinyPlan* plan, float factor) {
    if (factor < 0.0f) return TINYIMG_ERR_RANGE;
    return plan_scalar(plan, TINYIMG_OP_CONTRAST, factor);
}

TINYIMG_EXPORT("tiny_plan_saturation")
int tiny_plan_saturation(TinyPlan* plan, float factor) {
    if (factor < 0.0f) return TINYIMG_ERR_RANGE;
    return plan_scalar(plan, TINYIMG_OP_SATURATION, factor);
}

TINYIMG_EXPORT("tiny_plan_hue")
int tiny_plan_hue(TinyPlan* plan, float degrees) {
    return plan_scalar(plan, TINYIMG_OP_HUE, degrees);
}

TINYIMG_EXPORT("tiny_plan_grayscale")
int tiny_plan_grayscale(TinyPlan* plan) {
    return plan_scalar(plan, TINYIMG_OP_GRAYSCALE, 0.0f);
}

TINYIMG_EXPORT("tiny_plan_invert")
int tiny_plan_invert(TinyPlan* plan) {
    return plan_scalar(plan, TINYIMG_OP_INVERT, 0.0f);
}

TINYIMG_EXPORT("tiny_plan_gamma")
int tiny_plan_gamma(TinyPlan* plan, float gamma) {
    if (gamma <= 0.0f) return TINYIMG_ERR_RANGE;
    return plan_scalar(plan, TINYIMG_OP_GAMMA, gamma);
}

static int plan_blur(TinyPlan* plan, float amount, uint8_t gaussian) {
    if (amount < 0.0f) return TINYIMG_ERR_RANGE;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_BLUR;
    op.blur.amount = amount;
    op.blur.gaussian = gaussian;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_blur")
int tiny_plan_blur(TinyPlan* plan, float radius) {
    return plan_blur(plan, radius, 0);
}

TINYIMG_EXPORT("tiny_plan_gaussian_blur")
int tiny_plan_gaussian_blur(TinyPlan* plan, float sigma) {
    return plan_blur(plan, sigma, 1);
}

TINYIMG_EXPORT("tiny_plan_matrix")
int tiny_plan_matrix(TinyPlan* plan, const float* matrix) {
    if (!matrix) return TINYIMG_ERR_NULL;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_MATRIX;
    for (uint32_t i = 0; i < 12u; i++) op.matrix.m[i] = matrix[i];

    return plan_append(plan, &op);
}

/** Whether a curve's parameters name a curve that can be built. */
static int curve_is_valid(TinyCurveKind kind, const float* p) {
    switch (kind) {
        case TINYIMG_CURVE_GAMMA: return p[0] > 0.0f;
        case TINYIMG_CURVE_POSTERIZE: return p[0] >= 2.0f && p[0] <= 256.0f;
        case TINYIMG_CURVE_THRESHOLD:
        case TINYIMG_CURVE_SOLARIZE: return p[0] >= 0.0f && p[0] <= 255.0f;
        case TINYIMG_CURVE_LEVELS:
            // an empty input range has no inverse, so it is a range error
            // rather than a curve that clamps everything to one value
            return p[1] > p[0] && p[2] > 0.0f;
        case TINYIMG_CURVE_GAIN: return p[0] >= 0.0f;
        case TINYIMG_CURVE_FILL_LIGHT: return p[0] >= 0.0f && p[0] <= 1.0f;
        default: return 1;
    }
}

TINYIMG_EXPORT("tiny_plan_curve")
int tiny_plan_curve(
    TinyPlan* plan, TinyCurveKind kind, const float* params, uint8_t channels
) {
    if (channels > 7u) return TINYIMG_ERR_RANGE;

    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_CURVE;
    op.curve.kind = kind;
    op.curve.channels = channels;

    if (params) {
        for (uint32_t i = 0; i < 5u; i++) op.curve.p[i] = params[i];
    }

    if (!curve_is_valid(kind, op.curve.p)) return TINYIMG_ERR_RANGE;

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_effect_rect")
int tiny_plan_effect_rect(
    TinyPlan* plan, TinyEffectKind kind, const float* params, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height
) {
    TinyPlanOp op;
    tiny_memset(&op, 0, sizeof(op));

    op.kind = TINYIMG_OP_EFFECT;
    op.effect.kind = kind;
    op.effect.x = x;
    op.effect.y = y;
    op.effect.width = width;
    op.effect.height = height;

    if (params) {
        for (uint32_t i = 0; i < 4u; i++) op.effect.p[i] = params[i];
    }

    for (uint32_t i = 0; i < 4u; i++) {
        if (op.effect.p[i] < 0.0f && kind != TINYIMG_FX_EMBOSS &&
            kind != TINYIMG_FX_MOTION_BLUR) {
            return TINYIMG_ERR_RANGE;
        }
    }

    return plan_append(plan, &op);
}

TINYIMG_EXPORT("tiny_plan_effect")
int tiny_plan_effect(TinyPlan* plan, TinyEffectKind kind, const float* params) {
    return tiny_plan_effect_rect(plan, kind, params, 0, 0, 0, 0);
}

TINYIMG_EXPORT("tiny_plan_count")
uint32_t tiny_plan_count(const TinyPlan* plan) {
    return plan ? plan->count : 0u;
}

int tiny_plan_op_at(const TinyPlan* plan, uint32_t index, TinyPlanOp* op) {
    if (!plan || !op) return TINYIMG_ERR_NULL;
    if (index >= plan->count) return TINYIMG_ERR_BOUNDS;

    *op = plan->ops[index];
    return TINYIMG_OK;
}

#pragma endregion

#pragma region orientation

/**
 * @brief A flip or a turn, as a signed permutation of the two axes.
 *
 * Row major, acting on coordinates measured from each axis' centre so that a
 * mirror is a sign change and nothing else. That is what makes composition a
 * 2x2 multiply: eight orientations, one matrix, no case analysis and no chain
 * of intermediate images.
 */
typedef struct {
    int8_t m[4];
} Orient;

static const Orient ORIENT_IDENTITY = {{1, 0, 0, 1}};
static const Orient ORIENT_FLIP_X = {{-1, 0, 0, 1}};
static const Orient ORIENT_FLIP_Y = {{1, 0, 0, -1}};

/** Quarter turns clockwise. */
static const Orient ORIENT_TURN[4] = {
    {{1, 0, 0, 1}}, {{0, 1, -1, 0}}, {{-1, 0, 0, -1}}, {{0, -1, 1, 0}}
};

/**
 * @brief Composes two orientations.
 *
 * @param first The one already accumulated.
 * @param second The one being appended.
 * @return Orient The single orientation with the same effect.
 */
static Orient orient_mul(Orient first, Orient second) {
    Orient out;

    out.m[0] = (int8_t) (first.m[0] * second.m[0] + first.m[1] * second.m[2]);
    out.m[1] = (int8_t) (first.m[0] * second.m[1] + first.m[1] * second.m[3]);
    out.m[2] = (int8_t) (first.m[2] * second.m[0] + first.m[3] * second.m[2]);
    out.m[3] = (int8_t) (first.m[2] * second.m[1] + first.m[3] * second.m[3]);

    return out;
}

static int orient_swaps(Orient orient) {
    return orient.m[1] != 0;
}

static int orient_is_identity(Orient orient) {
    return orient.m[0] == 1 && orient.m[1] == 0 && orient.m[2] == 0 &&
           orient.m[3] == 1;
}

/**
 * @brief Maps an output coordinate back to the coordinate it reads.
 *
 * The doubling is what keeps this exact in integers: centred coordinates of an
 * even extent are half integers, so everything is computed at twice scale and
 * halved once at the end, where the sum is always even.
 *
 * @param orient The orientation.
 * @param width Width before the orientation.
 * @param height Height before the orientation.
 * @param x Output column.
 * @param y Output row.
 * @param u Receives the column to read.
 * @param v Receives the row to read.
 */
static void orient_read(
    Orient orient, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    uint32_t* u, uint32_t* v
) {
    int swap = orient_swaps(orient);
    int32_t out_width = (int32_t) (swap ? height : width);
    int32_t out_height = (int32_t) (swap ? width : height);

    int32_t cx = (int32_t) (2u * x) - (out_width - 1);
    int32_t cy = (int32_t) (2u * y) - (out_height - 1);

    int32_t cu = orient.m[0] * cx + orient.m[1] * cy;
    int32_t cv = orient.m[2] * cx + orient.m[3] * cy;

    *u = (uint32_t) ((cu + (int32_t) width - 1) / 2);
    *v = (uint32_t) ((cv + (int32_t) height - 1) / 2);
}

/** Maps a rectangle in output coordinates to the rectangle it reads. */
static void orient_read_rect(
    Orient orient, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h, uint32_t* out_x, uint32_t* out_y,
    uint32_t* out_width, uint32_t* out_height
) {
    uint32_t u0;
    uint32_t v0;
    uint32_t u1;
    uint32_t v1;

    orient_read(orient, width, height, x, y, &u0, &v0);
    orient_read(orient, width, height, x + w - 1u, y + h - 1u, &u1, &v1);

    *out_x = tiny_min_u32(u0, u1);
    *out_y = tiny_min_u32(v0, v1);
    *out_width = (u0 < u1 ? u1 - u0 : u0 - u1) + 1u;
    *out_height = (v0 < v1 ? v1 - v0 : v0 - v1) + 1u;
}

#pragma endregion

#pragma region fit modes

/**
 * @brief What a fit mode resolves to.
 *
 * Every mode lands here as the same three steps, so the walk that applies them
 * is written once: scale to an extent, take a rectangle of that, place it in a
 * possibly larger output.
 */
typedef struct {
    uint32_t scale_width;
    uint32_t scale_height;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t pad_width;
    uint32_t pad_height;
    uint32_t pad_x;
    uint32_t pad_y;
} FitPlan;

static uint32_t round_positive(double value) {
    if (value < 1.0) return 1u;
    return (uint32_t) (value + 0.5);
}

// the builtins lower to f64.floor and f64.ceil, so this costs an instruction
// rather than a call, and casting is not the same thing: a cast truncates
// toward zero and a sample position can be negative
static double floor_d(double value) {
    return __builtin_floor(value);
}

static double ceil_d(double value) {
    return __builtin_ceil(value);
}

/**
 * @brief A fit op's resolved focus for one axis, or a negative when it has
 * none.
 *
 * @param op The operation, whose kind is TINYIMG_OP_FIT.
 * @param vertical Non-zero for the vertical axis.
 * @return float The position, 0 through 1, or -1.
 */
static float fit_focus(const TinyPlanOp* op, int vertical) {
    if (!op->fit.focused) return -1.0f;
    return vertical ? op->fit.focus_y : op->fit.focus_x;
}

/**
 * @brief Where a rectangle of `want` sits inside one of `available`.
 *
 * @param available The larger extent.
 * @param want The smaller extent.
 * @param gravity Where to put it.
 * @param vertical Non-zero for the vertical axis.
 * @param focus Position of the interesting part, 0 through 1, or negative when
 * none was resolved.
 * @return uint32_t The offset.
 */
static uint32_t gravity_offset(
    uint32_t available, uint32_t want, TinyImageGravity gravity, int vertical,
    float focus
) {
    if (want >= available) return 0u;

    uint32_t slack = available - want;

    // a resolved focus centres the kept rectangle on it and then slides it back
    // inside, which is what keeps a face near an edge fully in frame rather
    // than half cropped
    if (focus >= 0.0f) {
        double centre = (double) focus * (double) available;
        double left = centre - (double) want / 2.0;

        if (left < 0.0) left = 0.0;
        if (left > (double) slack) left = (double) slack;

        return (uint32_t) (left + 0.5);
    }

    if (vertical) {
        switch (gravity) {
            case TINYIMG_GRAVITY_NORTH:
            case TINYIMG_GRAVITY_NORTH_WEST:
            case TINYIMG_GRAVITY_NORTH_EAST: return 0u;
            case TINYIMG_GRAVITY_SOUTH:
            case TINYIMG_GRAVITY_SOUTH_WEST:
            case TINYIMG_GRAVITY_SOUTH_EAST: return slack;
            default: return slack / 2u;
        }
    }

    switch (gravity) {
        case TINYIMG_GRAVITY_WEST:
        case TINYIMG_GRAVITY_NORTH_WEST:
        case TINYIMG_GRAVITY_SOUTH_WEST: return 0u;
        case TINYIMG_GRAVITY_EAST:
        case TINYIMG_GRAVITY_NORTH_EAST:
        case TINYIMG_GRAVITY_SOUTH_EAST: return slack;
        default: return slack / 2u;
    }
}

/**
 * @brief Turns a fit mode into a scale, a rectangle and a placement.
 *
 * The eleven modes are two independent choices, so this reads them as two:
 * whether the aspect mismatch is left, cropped, padded or distorted, and
 * whether the scale may rise, fall, both or neither. The table in TinyImageFit
 * is this function.
 *
 * @param mode The mode.
 * @param width Current width.
 * @param height Current height.
 * @param target_width Requested width.
 * @param target_height Requested height.
 * @param gravity Which part a crop keeps, or where a pad puts the image.
 * @param out Receives the three steps.
 */
static void fit_resolve(
    TinyImageFit mode, uint32_t width, uint32_t height, uint32_t target_width,
    uint32_t target_height, TinyImageGravity gravity, float focus_x,
    float focus_y, FitPlan* out
) {
    enum
    {
        ABSORB_CONTAIN,
        ABSORB_PAD,
        ABSORB_COVER,
        ABSORB_STRETCH
    } absorb = ABSORB_CONTAIN;

    enum
    {
        CLAMP_FREE,
        CLAMP_DOWN,
        CLAMP_UP,
        CLAMP_NONE
    } clamp = CLAMP_FREE;

    int to_ratio = 0;

    switch (mode) {
        case TINYIMG_FIT_SCALE_DOWN: clamp = CLAMP_DOWN; break;
        case TINYIMG_FIT_SCALE_UP: clamp = CLAMP_UP; break;
        case TINYIMG_FIT_CONTAIN: break;
        case TINYIMG_FIT_PAD: absorb = ABSORB_PAD; break;
        case TINYIMG_FIT_ASPECT_CONTAIN:
            absorb = ABSORB_PAD;
            clamp = CLAMP_NONE;
            to_ratio = 1;
            break;
        case TINYIMG_FIT_COVER: absorb = ABSORB_COVER; break;
        case TINYIMG_FIT_CROP:
            absorb = ABSORB_COVER;
            clamp = CLAMP_DOWN;
            break;
        case TINYIMG_FIT_FILL:
            absorb = ABSORB_COVER;
            clamp = CLAMP_UP;
            break;
        case TINYIMG_FIT_ASPECT_COVER:
            absorb = ABSORB_COVER;
            clamp = CLAMP_NONE;
            to_ratio = 1;
            break;
        case TINYIMG_FIT_ASPECT_CROP:
            absorb = ABSORB_COVER;
            clamp = CLAMP_NONE;
            break;
        default: absorb = ABSORB_STRETCH; break;
    }

    double fx = (double) target_width / (double) width;
    double fy = (double) target_height / (double) height;

    if (absorb == ABSORB_STRETCH) {
        out->scale_width = target_width;
        out->scale_height = target_height;
    }
    else if (clamp == CLAMP_NONE) {
        out->scale_width = width;
        out->scale_height = height;
    }
    else {
        double scale =
            absorb == ABSORB_COVER ? (fx > fy ? fx : fy) : (fx < fy ? fx : fy);

        if (clamp == CLAMP_DOWN && scale > 1.0) scale = 1.0;
        if (clamp == CLAMP_UP && scale < 1.0) scale = 1.0;

        out->scale_width = round_positive((double) width * scale);
        out->scale_height = round_positive((double) height * scale);
    }

    uint32_t keep_width = out->scale_width;
    uint32_t keep_height = out->scale_height;

    if (absorb == ABSORB_COVER) {
        if (to_ratio) {
            // the largest rectangle of the target's shape that fits inside
            if (fx > fy) {
                keep_height = round_positive(
                    (double) out->scale_width * (double) target_height /
                    (double) target_width
                );
                if (keep_height > out->scale_height) {
                    keep_height = out->scale_height;
                }
            }
            else {
                keep_width = round_positive(
                    (double) out->scale_height * (double) target_width /
                    (double) target_height
                );
                if (keep_width > out->scale_width) {
                    keep_width = out->scale_width;
                }
            }
        }
        else {
            keep_width = tiny_min_u32(target_width, out->scale_width);
            keep_height = tiny_min_u32(target_height, out->scale_height);
        }
    }

    out->crop_width = keep_width;
    out->crop_height = keep_height;
    out->crop_x =
        gravity_offset(out->scale_width, keep_width, gravity, 0, focus_x);
    out->crop_y =
        gravity_offset(out->scale_height, keep_height, gravity, 1, focus_y);

    out->pad_width = keep_width;
    out->pad_height = keep_height;

    if (absorb == ABSORB_PAD) {
        if (to_ratio) {
            // the smallest rectangle of the target's shape that contains it
            if (fx < fy) {
                out->pad_height = round_positive(
                    (double) keep_width * (double) target_height /
                    (double) target_width
                );
                if (out->pad_height < keep_height) {
                    out->pad_height = keep_height;
                }
            }
            else {
                out->pad_width = round_positive(
                    (double) keep_height * (double) target_width /
                    (double) target_height
                );
                if (out->pad_width < keep_width) {
                    out->pad_width = keep_width;
                }
            }
        }
        else {
            out->pad_width = tiny_max_u32(target_width, keep_width);
            out->pad_height = tiny_max_u32(target_height, keep_height);
        }
    }

    out->pad_x =
        gravity_offset(out->pad_width, keep_width, gravity, 0, focus_x);
    out->pad_y =
        gravity_offset(out->pad_height, keep_height, gravity, 1, focus_y);
}

/** Resolves a resize's zero axis against the aspect ratio. */
static void resize_target(
    const TinyPlanOp* op, uint32_t width, uint32_t height, uint32_t* out_width,
    uint32_t* out_height
) {
    uint32_t w = op->resize.width;
    uint32_t h = op->resize.height;

    if (w == 0) {
        w = round_positive((double) width * (double) h / (double) height);
    }
    else if (h == 0) {
        h = round_positive((double) height * (double) w / (double) width);
    }

    *out_width = w;
    *out_height = h;
}

#pragma endregion

#pragma region rewrites

/** How many channels an operation leaves behind. */
static uint8_t op_channels(const TinyPlanOp* op, uint8_t channels) {
    if (op->kind != TINYIMG_OP_GRAYSCALE) return channels;
    return channels == 2u || channels == 4u ? 2u : 1u;
}

/** How the extent a caller sees changes, without the sample window. */
static void op_extent(const TinyPlanOp* op, uint32_t* width, uint32_t* height) {
    switch (op->kind) {
        case TINYIMG_OP_CROP: {
            uint32_t w = op->crop.width;
            uint32_t h = op->crop.height;

            if (op->crop.x >= *width || op->crop.y >= *height) {
                *width = 0;
                *height = 0;
                return;
            }

            if (w == 0 || w > *width - op->crop.x) w = *width - op->crop.x;
            if (h == 0 || h > *height - op->crop.y) h = *height - op->crop.y;

            *width = w;
            *height = h;
            break;
        }
        case TINYIMG_OP_RESIZE:
            resize_target(op, *width, *height, width, height);
            break;
        case TINYIMG_OP_FIT: {
            FitPlan fit;
            fit_resolve(
                op->fit.mode, *width, *height, op->fit.width, op->fit.height,
                op->fit.gravity, fit_focus(op, 0), fit_focus(op, 1), &fit
            );
            *width = fit.pad_width;
            *height = fit.pad_height;
            break;
        }
        case TINYIMG_OP_ROTATE:
            if (op->rotate.turns % 2u == 1u) {
                uint32_t swap = *width;
                *width = *height;
                *height = swap;
            }
            break;
        default: break;
    }
}

static int float_is(float value, float target) {
    float diff = value - target;
    return diff > -1e-6f && diff < 1e-6f;
}

/**
 * @brief Whether an operation leaves the image exactly as it found it.
 *
 * @param op The operation.
 * @param width Current width.
 * @param height Current height.
 * @param channels Current channel count.
 * @return int Non-zero when it can be dropped.
 */
static int op_is_identity(
    const TinyPlanOp* op, uint32_t width, uint32_t height, uint8_t channels
) {
    switch (op->kind) {
        case TINYIMG_OP_NONE: return 1;
        case TINYIMG_OP_BRIGHTNESS:
        case TINYIMG_OP_CONTRAST:
        case TINYIMG_OP_SATURATION:
        case TINYIMG_OP_GAMMA: return float_is(op->scalar.value, 1.0f);
        case TINYIMG_OP_HUE: {
            float turns = op->scalar.value / 360.0f;
            float whole = (float) (int32_t) turns;
            return float_is(turns, whole);
        }
        case TINYIMG_OP_GRAYSCALE: return channels < 3u;
        case TINYIMG_OP_BLUR: return op->blur.amount < 0.5f;
        case TINYIMG_OP_MATRIX: {
            static const float identity[12] = {1, 0, 0, 0, 0, 1,
                                               0, 0, 0, 0, 1, 0};

            for (uint32_t i = 0; i < 12u; i++) {
                if (!float_is(op->matrix.m[i], identity[i])) return 0;
            }
            return 1;
        }
        case TINYIMG_OP_CURVE:
            switch (op->curve.kind) {
                case TINYIMG_CURVE_GAMMA:
                case TINYIMG_CURVE_GAIN: return float_is(op->curve.p[0], 1.0f);
                case TINYIMG_CURVE_EXPOSURE:
                case TINYIMG_CURVE_FILL_LIGHT:
                case TINYIMG_CURVE_SIGMOID:
                    return float_is(op->curve.p[0], 0.0f);
                case TINYIMG_CURVE_POSTERIZE:
                    return float_is(op->curve.p[0], 256.0f);
                case TINYIMG_CURVE_SOLARIZE:
                    return float_is(op->curve.p[0], 255.0f);
                case TINYIMG_CURVE_LEVELS:
                    return float_is(op->curve.p[0], 0.0f) &&
                           float_is(op->curve.p[1], 255.0f) &&
                           float_is(op->curve.p[2], 1.0f) &&
                           float_is(op->curve.p[3], 0.0f) &&
                           float_is(op->curve.p[4], 255.0f);
                default: return 0;
            }
        case TINYIMG_OP_EFFECT:
            switch (op->effect.kind) {
                case TINYIMG_FX_UNSHARP:
                case TINYIMG_FX_CLARITY: return float_is(op->effect.p[1], 0.0f);
                case TINYIMG_FX_PIXELATE:
                case TINYIMG_FX_PIXELATE_REGION: return op->effect.p[0] < 2.0f;
                case TINYIMG_FX_DILATE:
                case TINYIMG_FX_ERODE:
                case TINYIMG_FX_OUTLINE:
                case TINYIMG_FX_MOTION_BLUR:
                case TINYIMG_FX_RADIAL_BLUR:
                case TINYIMG_FX_ZOOM_BLUR:
                case TINYIMG_FX_CHROMATIC:
                case TINYIMG_FX_TILT_SHIFT:
                case TINYIMG_FX_BLUR_REGION:
                    return float_is(op->effect.p[0], 0.0f);
                case TINYIMG_FX_SCANLINES:
                    return op->effect.p[0] < 2.0f ||
                           float_is(op->effect.p[1], 0.0f);
                case TINYIMG_FX_DITHER:
                    return float_is(op->effect.p[0], 256.0f);
                default: return 0;
            }
        case TINYIMG_OP_ROTATE: return op->rotate.turns % 4u == 0u;
        case TINYIMG_OP_CROP:
            return op->crop.x == 0 && op->crop.y == 0 &&
                   (op->crop.width == 0 || op->crop.width >= width) &&
                   (op->crop.height == 0 || op->crop.height >= height);
        case TINYIMG_OP_RESIZE: {
            uint32_t w;
            uint32_t h;
            resize_target(op, width, height, &w, &h);
            return w == width && h == height;
        }
        case TINYIMG_OP_FIT: {
            FitPlan fit;
            fit_resolve(
                op->fit.mode, width, height, op->fit.width, op->fit.height,
                op->fit.gravity, fit_focus(op, 0), fit_focus(op, 1), &fit
            );
            return fit.scale_width == width && fit.scale_height == height &&
                   fit.crop_width == width && fit.crop_height == height &&
                   fit.pad_width == width && fit.pad_height == height;
        }
        default: return 0;
    }
}

/**
 * @brief The next operation a geometry rule may pair with.
 *
 * Colour operations read one pixel and never move it, so a geometry pair is
 * still a pair with any number of them in between and the merge is exact. A
 * neighbourhood operation is not, so the scan stops there.
 *
 * @param ops The operations.
 * @param count How many there are.
 * @param from Where to start looking, exclusive.
 * @return int32_t The index, or -1 when nothing may pair.
 */
static int32_t next_geometry(
    const TinyPlanOp* ops, uint32_t count, uint32_t from
) {
    for (uint32_t i = from + 1u; i < count; i++) {
        TinyPlanOpClass cls = tiny_plan_op_class(ops[i].kind);

        if (cls == TINYIMG_OP_CLASS_GEOMETRY) return (int32_t) i;
        if (cls == TINYIMG_OP_CLASS_NEIGHBOURHOOD) return -1;
    }

    return -1;
}

/** The next colour operation, skipping the geometry it commutes with. */
static int32_t next_color(
    const TinyPlanOp* ops, uint32_t count, uint32_t from
) {
    for (uint32_t i = from + 1u; i < count; i++) {
        TinyPlanOpClass cls = tiny_plan_op_class(ops[i].kind);

        if (cls == TINYIMG_OP_CLASS_COLOR_MATRIX ||
            cls == TINYIMG_OP_CLASS_COLOR_LUT) {
            return (int32_t) i;
        }
        if (cls == TINYIMG_OP_CLASS_NEIGHBOURHOOD) return -1;
    }

    return -1;
}

static void ops_remove(TinyPlanOp* ops, uint32_t* count, uint32_t index) {
    for (uint32_t i = index; i + 1u < *count; i++) ops[i] = ops[i + 1u];
    (*count)--;
}

/**
 * @brief Moves an operation to just after another.
 *
 * @param ops The operations.
 * @param from Index to move.
 * @param after Index to land behind, which must be above `from`.
 */
static void ops_move_after(TinyPlanOp* ops, uint32_t from, uint32_t after) {
    TinyPlanOp moved = ops[from];

    for (uint32_t i = from; i < after; i++) ops[i] = ops[i + 1u];
    ops[after] = moved;
}

/**
 * @brief Merges the second of two crops into the first.
 *
 * The second rectangle is in the coordinates the first produced, so the merged
 * origin is the sum and the merged extent is the second's, clipped to what the
 * first left.
 */
static void crop_merge(
    TinyPlanOp* first, const TinyPlanOp* second, uint32_t width, uint32_t height
) {
    uint32_t w = first->crop.width;
    uint32_t h = first->crop.height;

    if (w == 0 || w > width - first->crop.x) w = width - first->crop.x;
    if (h == 0 || h > height - first->crop.y) h = height - first->crop.y;

    uint32_t x = second->crop.x < w ? second->crop.x : w;
    uint32_t y = second->crop.y < h ? second->crop.y : h;
    uint32_t cw = second->crop.width;
    uint32_t ch = second->crop.height;

    if (cw == 0 || cw > w - x) cw = w - x;
    if (ch == 0 || ch > h - y) ch = h - y;

    first->crop.x += x;
    first->crop.y += y;
    first->crop.width = cw;
    first->crop.height = ch;
}

/**
 * @brief The factor a resampling operation reduces each axis by.
 *
 * @param op The operation.
 * @param width Current width.
 * @param height Current height.
 * @param x Receives the horizontal factor.
 * @param y Receives the vertical factor.
 * @return int Non-zero when the operation resamples at all.
 */
static int op_reduction(
    const TinyPlanOp* op, uint32_t width, uint32_t height, double* x, double* y
) {
    uint32_t w;
    uint32_t h;

    if (op->kind == TINYIMG_OP_RESIZE) {
        resize_target(op, width, height, &w, &h);
    }
    else if (op->kind == TINYIMG_OP_FIT) {
        FitPlan fit;
        fit_resolve(
            op->fit.mode, width, height, op->fit.width, op->fit.height,
            op->fit.gravity, fit_focus(op, 0), fit_focus(op, 1), &fit
        );
        w = fit.scale_width;
        h = fit.scale_height;
    }
    else {
        return 0;
    }

    if (w == 0 || h == 0) return 0;

    *x = (double) width / (double) w;
    *y = (double) height / (double) h;

    return 1;
}

/**
 * @brief Whether a blur may be moved to after a downscale.
 *
 * Gaussian blur commutes with scaling under a scaled sigma, so blurring the
 * smaller image is the same operation for a fraction of the pixels. Two things
 * have to hold before it is taken: the reduction has to be worth it, since
 * below about two the saving does not pay for the accuracy, and it has to be
 * the same in both axes, because the blur this library implements is isotropic
 * and an anisotropic reduction would need it not to be.
 *
 * @param x Horizontal reduction factor.
 * @param y Vertical reduction factor.
 * @return int Non-zero when the swap is exact enough to take.
 */
static int blur_may_move(double x, double y) {
    if (x < 2.0 || y < 2.0) return 0;

    double larger = x > y ? x : y;
    double smaller = x > y ? y : x;

    return larger - smaller <= larger * 0.02;
}

/**
 * @brief Rewrites a plan in place until no rule applies.
 *
 * One forward pass per round, and a round that changes nothing ends it. Every
 * rule either removes an operation or moves one later, so the number of rounds
 * is bounded by the operation count without needing to prove anything subtler.
 *
 * @param ops The operations.
 * @param count How many there are, updated as they are removed.
 * @param width Source width.
 * @param height Source height.
 * @param channels Source channel count.
 * @param eliminated Receives how many identities were dropped.
 * @param collapsed Receives how many were merged into another.
 */
static void rewrite(
    TinyPlanOp* ops, uint32_t* count, uint32_t width, uint32_t height,
    uint8_t channels, uint32_t* eliminated, uint32_t* collapsed
) {
    for (uint32_t round = 0; round <= TINYIMG_PLAN_MAX_OPS; round++) {
        uint32_t changes = 0;
        uint32_t w = width;
        uint32_t h = height;
        uint8_t c = channels;

        for (uint32_t i = 0; i < *count; i++) {
            if (op_is_identity(&ops[i], w, h, c)) {
                ops_remove(ops, count, i);
                (*eliminated)++;
                changes++;
                i--;
                continue;
            }

            int32_t pair =
                tiny_plan_op_class(ops[i].kind) == TINYIMG_OP_CLASS_GEOMETRY
                    ? next_geometry(ops, *count, i)
                    : next_color(ops, *count, i);

            if (pair >= 0) {
                TinyPlanOp* second = &ops[pair];
                int merged = 0;

                if (ops[i].kind == TINYIMG_OP_CROP &&
                    second->kind == TINYIMG_OP_CROP) {
                    crop_merge(&ops[i], second, w, h);
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_RESIZE &&
                    second->kind == TINYIMG_OP_RESIZE
                ) {
                    // the first resample's output is entirely discarded by the
                    // second, and one resample of the source is better than two
                    ops[i] = *second;
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_ROTATE &&
                    second->kind == TINYIMG_OP_ROTATE
                ) {
                    ops[i].rotate.turns =
                        (ops[i].rotate.turns + second->rotate.turns) % 4u;
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_FLIP_H &&
                    second->kind == TINYIMG_OP_FLIP_H
                ) {
                    ops[i].kind = TINYIMG_OP_NONE;
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_FLIP_V &&
                    second->kind == TINYIMG_OP_FLIP_V
                ) {
                    ops[i].kind = TINYIMG_OP_NONE;
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_INVERT &&
                    second->kind == TINYIMG_OP_INVERT
                ) {
                    ops[i].kind = TINYIMG_OP_NONE;
                    merged = 1;
                }
                else if (
                    ops[i].kind == TINYIMG_OP_GRAYSCALE &&
                    (second->kind == TINYIMG_OP_GRAYSCALE ||
                     second->kind == TINYIMG_OP_SATURATION ||
                     second->kind == TINYIMG_OP_HUE)
                ) {
                    // an image whose channels are all equal has no saturation
                    // to change and no hue to turn
                    merged = 1;
                }

                if (merged) {
                    ops_remove(ops, count, (uint32_t) pair);
                    (*collapsed)++;
                    changes++;
                    i--;
                    continue;
                }
            }

            if (ops[i].kind == TINYIMG_OP_BLUR) {
                int32_t next = next_geometry(ops, *count, i);
                double rx;
                double ry;

                if (next >= 0 && op_reduction(&ops[next], w, h, &rx, &ry) &&
                    blur_may_move(rx, ry)) {
                    ops[i].blur.amount /= (float) rx;
                    ops_move_after(ops, i, (uint32_t) next);
                    changes++;
                    i--;
                    continue;
                }
            }

            c = op_channels(&ops[i], c);
            op_extent(&ops[i], &w, &h);
        }

        if (changes == 0) return;
    }
}

#pragma endregion

#pragma region resolution

/**
 * @brief The accumulated geometry of one fused pass.
 *
 * The invariant: the image at this point in the chain is `width` by `height`
 * pixels sampled from the window (`x`, `y`, `width_src`, `height_src`) of the
 * decoded source, with `orient` applied, placed at (`pad_x`, `pad_y`) in an
 * output of (`pad_width`, `pad_height`).
 *
 * The window is fractional on purpose. A crop taken after a resize adjusts it
 * rather than deriving a fresh resize from the integer rectangle, which is what
 * keeps every remaining sample exactly where the uncropped resize put it.
 */
typedef struct {
    double x;
    double y;
    double width_src;
    double height_src;
    uint32_t width;
    uint32_t height;
    Orient orient;
    uint32_t pad_width;
    uint32_t pad_height;
    uint32_t pad_x;
    uint32_t pad_y;
    int padded;
    TinyResampleFilter filter;
} Geometry;

static void geometry_start(
    Geometry* g, double width, double height, uint32_t extent_width,
    uint32_t extent_height
) {
    g->x = 0.0;
    g->y = 0.0;
    g->width_src = width;
    g->height_src = height;
    g->width = extent_width;
    g->height = extent_height;
    g->orient = ORIENT_IDENTITY;
    g->pad_width = extent_width;
    g->pad_height = extent_height;
    g->pad_x = 0;
    g->pad_y = 0;
    g->padded = 0;
    g->filter = TINYIMG_FILTER_AUTO;
}

static void geometry_extent(
    const Geometry* g, uint32_t* width, uint32_t* height
) {
    if (g->padded) {
        *width = g->pad_width;
        *height = g->pad_height;
        return;
    }

    int swap = orient_swaps(g->orient);
    *width = swap ? g->height : g->width;
    *height = swap ? g->width : g->height;
}

static void geometry_crop(
    Geometry* g, uint32_t x, uint32_t y, uint32_t width, uint32_t height
) {
    uint32_t bx;
    uint32_t by;
    uint32_t bw;
    uint32_t bh;

    orient_read_rect(
        g->orient, g->width, g->height, x, y, width, height, &bx, &by, &bw, &bh
    );

    double step_x = g->width_src / (double) g->width;
    double step_y = g->height_src / (double) g->height;

    g->x += (double) bx * step_x;
    g->y += (double) by * step_y;
    g->width_src = (double) bw * step_x;
    g->height_src = (double) bh * step_y;
    g->width = bw;
    g->height = bh;
}

static void geometry_resize(
    Geometry* g, uint32_t width, uint32_t height, TinyResampleFilter filter
) {
    int swap = orient_swaps(g->orient);

    g->width = swap ? height : width;
    g->height = swap ? width : height;
    g->filter = filter;
}

/**
 * @brief Applies one geometry operation to the accumulated window.
 *
 * @param g The accumulator.
 * @param op The operation.
 * @return int TINYIMG_OK, or TINYIMG_ERR_RANGE when the operation leaves
 * nothing to produce.
 */
static int geometry_apply(Geometry* g, const TinyPlanOp* op) {
    uint32_t width;
    uint32_t height;
    geometry_extent(g, &width, &height);

    switch (op->kind) {
        case TINYIMG_OP_CROP: {
            uint32_t w = op->crop.width;
            uint32_t h = op->crop.height;

            if (op->crop.x >= width || op->crop.y >= height) {
                return TINYIMG_ERR_RANGE;
            }

            if (w == 0 || w > width - op->crop.x) w = width - op->crop.x;
            if (h == 0 || h > height - op->crop.y) h = height - op->crop.y;

            geometry_crop(g, op->crop.x, op->crop.y, w, h);
            break;
        }
        case TINYIMG_OP_RESIZE: {
            uint32_t w;
            uint32_t h;
            resize_target(op, width, height, &w, &h);

            if (w == 0 || h == 0) return TINYIMG_ERR_RANGE;
            geometry_resize(g, w, h, op->resize.filter);
            break;
        }
        case TINYIMG_OP_FIT: {
            FitPlan fit;
            fit_resolve(
                op->fit.mode, width, height, op->fit.width, op->fit.height,
                op->fit.gravity, fit_focus(op, 0), fit_focus(op, 1), &fit
            );

            if (fit.scale_width != width || fit.scale_height != height) {
                geometry_resize(
                    g, fit.scale_width, fit.scale_height, TINYIMG_FILTER_AUTO
                );
            }

            if (fit.crop_width != fit.scale_width ||
                fit.crop_height != fit.scale_height) {
                geometry_crop(
                    g, fit.crop_x, fit.crop_y, fit.crop_width, fit.crop_height
                );
            }

            if (fit.pad_width != fit.crop_width ||
                fit.pad_height != fit.crop_height) {
                g->pad_width = fit.pad_width;
                g->pad_height = fit.pad_height;
                g->pad_x = fit.pad_x;
                g->pad_y = fit.pad_y;
                g->padded = 1;
            }
            break;
        }
        case TINYIMG_OP_FLIP_H:
            g->orient = orient_mul(g->orient, ORIENT_FLIP_X);
            break;
        case TINYIMG_OP_FLIP_V:
            g->orient = orient_mul(g->orient, ORIENT_FLIP_Y);
            break;
        case TINYIMG_OP_ROTATE:
            g->orient =
                orient_mul(g->orient, ORIENT_TURN[op->rotate.turns % 4u]);
            break;
        default: break;
    }

    return TINYIMG_OK;
}

/**
 * @brief Walks operations into one pass' geometry.
 *
 * Stops at the first operation the pass cannot absorb: a neighbourhood
 * operation, or any geometry after a pad, since the padding has to exist before
 * anything can move it.
 *
 * @param ops The operations.
 * @param count How many there are.
 * @param g The accumulator, already started.
 * @param channels Source channel count, updated by a greyscale.
 * @param consumed Receives how many operations this pass covers.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int geometry_walk(
    const TinyPlanOp* ops, uint32_t count, Geometry* g, uint8_t* channels,
    uint32_t* consumed
) {
    for (uint32_t i = 0; i < count; i++) {
        TinyPlanOpClass cls = tiny_plan_op_class(ops[i].kind);

        if (cls == TINYIMG_OP_CLASS_NEIGHBOURHOOD) {
            *consumed = i;
            return TINYIMG_OK;
        }

        if (cls == TINYIMG_OP_CLASS_GEOMETRY) {
            if (g->padded) {
                *consumed = i;
                return TINYIMG_OK;
            }

            int result = geometry_apply(g, &ops[i]);
            if (result != TINYIMG_OK) return result;
            continue;
        }

        *channels = op_channels(&ops[i], *channels);
    }

    *consumed = count;
    return TINYIMG_OK;
}

static int color_walk(
    const TinyPlanResolution* resolution, TinyColorStage* stages,
    uint32_t capacity, uint32_t* count, uint32_t* before
);

/** Whether a chain's only colour operation is a single greyscale. */
static int only_grayscale(const TinyPlanOp* ops, uint32_t count) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < count; i++) {
        TinyPlanOpClass cls = tiny_plan_op_class(ops[i].kind);

        if (cls != TINYIMG_OP_CLASS_COLOR_MATRIX &&
            cls != TINYIMG_OP_CLASS_COLOR_LUT) {
            continue;
        }

        if (ops[i].kind != TINYIMG_OP_GRAYSCALE) return 0;
        seen++;
    }

    return seen == 1u;
}

TINYIMG_EXPORT("tiny_plan_resolve")
int tiny_plan_resolve(const TinyPlan* plan, TinyPlanResolution* resolution) {
    if (!plan || !resolution) return TINYIMG_ERR_NULL;

    tiny_memset(resolution, 0, sizeof(*resolution));

    for (uint32_t i = 0; i < plan->count; i++) {
        resolution->op[i] = plan->ops[i];
    }
    resolution->ops = plan->count;

    rewrite(
        resolution->op, &resolution->ops, plan->source_width,
        plan->source_height, plan->source_channels, &resolution->eliminated,
        &resolution->collapsed
    );

    uint8_t channels = plan->source_channels;
    Geometry geometry;

    geometry_start(
        &geometry, (double) plan->source_width, (double) plan->source_height,
        plan->source_width, plan->source_height
    );

    int result = geometry_walk(
        resolution->op, resolution->ops, &geometry, &channels,
        &resolution->consumed
    );
    if (result != TINYIMG_OK) return result;

    // the walk runs once, in source coordinates. what follows is a change of
    // coordinates into the decoded grid, not a second walk: walking again would
    // re-apply crops the decode region has already taken. an already decoded
    // source has no region and no scale, so for it the change is the identity
    uint32_t region_x = 0;
    uint32_t region_y = 0;
    uint32_t region_width = plan->source_width;
    uint32_t region_height = plan->source_height;
    uint8_t den = 1;

    if (plan->buffer) {
        uint32_t region_right =
            (uint32_t) ceil_d(geometry.x + geometry.width_src);
        uint32_t region_bottom =
            (uint32_t) ceil_d(geometry.y + geometry.height_src);

        if (region_right > plan->source_width)
            region_right = plan->source_width;
        if (region_bottom > plan->source_height) {
            region_bottom = plan->source_height;
        }

        region_x = (uint32_t) floor_d(geometry.x);
        region_y = (uint32_t) floor_d(geometry.y);
        region_width = region_right - region_x;
        region_height = region_bottom - region_y;

        // the largest reduction the decoder can take that still leaves the
        // resample as many pixels as it means to produce
        for (uint8_t candidate = 8; candidate > 1u; candidate /= 2u) {
            if (geometry.width_src / candidate >= (double) geometry.width &&
                geometry.height_src / candidate >= (double) geometry.height) {
                den = candidate;
                break;
            }
        }
    }

    resolution->decode.x = region_x;
    resolution->decode.y = region_y;
    resolution->decode.width = region_width;
    resolution->decode.height = region_height;
    resolution->decode.scale_den = den;
    resolution->decode.channels = 0;

    resolution->decode_width = (region_width + den - 1u) / den;
    resolution->decode_height = (region_height + den - 1u) / den;

    if (only_grayscale(resolution->op, resolution->ops) &&
        plan->source_channels > 2u) {
        // the decoders reduce to luminance through the same weights the colour
        // stage would use, so asking them for it is exact and makes every later
        // pass single channel
        resolution->decode.channels = channels;
        resolution->kernels |= TINYIMG_KERNEL_GRAY_DECODE;
    }

    // the region's last decoded pixel covers fewer source pixels than the rest
    // when the extent is not a multiple of the denominator, so the window is
    // scaled by the grid it landed on rather than by the denominator
    double scale_x = (double) resolution->decode_width / (double) region_width;
    double scale_y =
        (double) resolution->decode_height / (double) region_height;

    resolution->source_x = (geometry.x - (double) region_x) * scale_x;
    resolution->source_y = (geometry.y - (double) region_y) * scale_y;
    resolution->source_width = geometry.width_src * scale_x;
    resolution->source_height = geometry.height_src * scale_y;
    resolution->sample_width = geometry.width;
    resolution->sample_height = geometry.height;

    for (uint32_t i = 0; i < 4; i++) {
        resolution->orientation[i] = geometry.orient.m[i];
    }

    geometry_extent(&geometry, &resolution->width, &resolution->height);
    resolution->offset_x = geometry.padded ? geometry.pad_x : 0u;
    resolution->offset_y = geometry.padded ? geometry.pad_y : 0u;
    resolution->channels = channels;

    if ((uint64_t) resolution->width * resolution->height >
        TINYIMG_MAX_PIXELS) {
        return TINYIMG_ERR_TOO_LARGE;
    }

    double step_x =
        resolution->source_width / (double) resolution->sample_width;
    double step_y =
        resolution->source_height / (double) resolution->sample_height;

    resolution->filter_x = geometry.filter;
    resolution->filter_y = geometry.filter;

    if (geometry.filter == TINYIMG_FILTER_AUTO) {
        // at exactly one the box is the identity and the cubic is sixteen taps
        // that reproduce their own input, so the boundary belongs on this side
        resolution->filter_x =
            step_x >= 1.0 ? TINYIMG_FILTER_BOX : TINYIMG_FILTER_CATMULL_ROM;
        resolution->filter_y =
            step_y >= 1.0 ? TINYIMG_FILTER_BOX : TINYIMG_FILTER_CATMULL_ROM;
    }

    // one source pixel per output pixel, in order, is a copy however it got
    // there. anything else reads through a filter
    int resamples =
        resolution->source_width != (double) resolution->sample_width ||
        resolution->source_height != (double) resolution->sample_height ||
        resolution->source_x != floor_d(resolution->source_x) ||
        resolution->source_y != floor_d(resolution->source_y);

    // a rectangle can be reached two ways, and the decode having already taken
    // one is why the window alone cannot tell: after a region decode the window
    // covers everything that came back
    if (region_width != plan->source_width ||
        region_height != plan->source_height || resolution->source_x != 0.0 ||
        resolution->source_y != 0.0 ||
        resolution->source_width != (double) resolution->decode_width ||
        resolution->source_height != (double) resolution->decode_height) {
        resolution->kernels |= TINYIMG_KERNEL_REGION;
    }
    if (den > 1u) resolution->kernels |= TINYIMG_KERNEL_SCALED;
    if (resamples) resolution->kernels |= TINYIMG_KERNEL_RESAMPLE;
    if (!orient_is_identity(geometry.orient)) {
        resolution->kernels |= TINYIMG_KERNEL_ORIENT;
    }
    if (geometry.padded) resolution->kernels |= TINYIMG_KERNEL_PAD;

    uint32_t stages = 0;
    uint32_t before = 0;

    // a stage stays on the side of the resample the caller put it on. it is
    // tempting to move a matrix to the output side, since that is the cheap
    // side of a reduction and an affine function does commute with a weighted
    // average, but the clamp that follows it does not: averaging clamped
    // channels is not clamping averaged ones
    color_walk(resolution, 0, TINYIMG_PLAN_MAX_OPS, &stages, &before);

    resolution->color_stages = stages;
    resolution->color_stages_before = before;
    if (stages > 0) resolution->kernels |= TINYIMG_KERNEL_COLOR;

    if (resolution->consumed < resolution->ops) {
        resolution->kernels |= TINYIMG_KERNEL_NEIGHBOURHOOD;
    }
    if (resolution->kernels == 0 ||
        resolution->kernels == TINYIMG_KERNEL_GRAY_DECODE) {
        resolution->kernels |= TINYIMG_KERNEL_COPY;
    }

    resolution->passes = 1;
    for (uint32_t i = resolution->consumed; i < resolution->ops; i++) {
        if (tiny_plan_op_class(resolution->op[i].kind) ==
            TINYIMG_OP_CLASS_NEIGHBOURHOOD) {
            resolution->passes++;
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region colour stages

/** 1.0 in the fixed point the colour matrices are held in. */
#define COLOR_ONE 65536

/** Rec. 709 luminance weights, the same ones tiny_luma is built from. */
#define LUMA_R 13933
#define LUMA_G 46871
#define LUMA_B 4732

static void matrix_identity(int32_t* m) {
    tiny_memset(m, 0, 12u * sizeof(int32_t));
    m[0] = COLOR_ONE;
    m[5] = COLOR_ONE;
    m[10] = COLOR_ONE;
}

/**
 * @brief Composes two affine colour matrices.
 *
 * @param out Receives `second` applied after `first`. May alias neither.
 * @param first The matrix applied first.
 * @param second The matrix applied second.
 */
static void matrix_mul(
    int32_t* out, const int32_t* first, const int32_t* second
) {
    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            int64_t sum = 0;

            for (uint32_t k = 0; k < 3u; k++) {
                sum += (int64_t) second[row * 4u + k] * first[k * 4u + col];
            }

            out[row * 4u + col] = (int32_t) (sum / COLOR_ONE);
        }

        int64_t offset = (int64_t) second[row * 4u + 3u] * COLOR_ONE;

        for (uint32_t k = 0; k < 3u; k++) {
            offset += (int64_t) second[row * 4u + k] * first[k * 4u + 3u];
        }

        out[row * 4u + 3u] = (int32_t) (offset / COLOR_ONE);
    }
}

static int32_t to_fixed(double value) {
    return (int32_t) (value * COLOR_ONE + (value < 0.0 ? -0.5 : 0.5));
}

/**
 * @brief Builds the matrix for one affine colour operation.
 *
 * @param op The operation.
 * @param m Receives a row major 3x4 in 16.16.
 */
static void matrix_of(const TinyPlanOp* op, int32_t* m) {
    matrix_identity(m);

    switch (op->kind) {
        case TINYIMG_OP_BRIGHTNESS: {
            int32_t factor = to_fixed((double) op->scalar.value);
            m[0] = factor;
            m[5] = factor;
            m[10] = factor;
            break;
        }
        case TINYIMG_OP_CONTRAST: {
            // about the middle of the range rather than about 128, so that a
            // factor and its reciprocal are inverses
            double factor = (double) op->scalar.value;
            int32_t scale = to_fixed(factor);
            int32_t offset = to_fixed(127.5 * (1.0 - factor));

            m[0] = scale;
            m[5] = scale;
            m[10] = scale;
            m[3] = offset;
            m[7] = offset;
            m[11] = offset;
            break;
        }
        case TINYIMG_OP_GRAYSCALE:
        case TINYIMG_OP_SATURATION: {
            double factor = op->kind == TINYIMG_OP_GRAYSCALE
                                ? 0.0
                                : (double) op->scalar.value;
            int32_t rest = to_fixed(1.0 - factor);
            int32_t keep = to_fixed(factor);
            int32_t weight[3] = {LUMA_R, LUMA_G, LUMA_B};

            for (uint32_t row = 0; row < 3u; row++) {
                for (uint32_t col = 0; col < 3u; col++) {
                    int64_t value = (int64_t) rest * weight[col] / COLOR_ONE;
                    if (row == col) value += keep;

                    m[row * 4u + col] = (int32_t) value;
                }
            }
            break;
        }
        case TINYIMG_OP_INVERT: {
            m[0] = -COLOR_ONE;
            m[5] = -COLOR_ONE;
            m[10] = -COLOR_ONE;
            m[3] = 255 * COLOR_ONE;
            m[7] = 255 * COLOR_ONE;
            m[11] = 255 * COLOR_ONE;
            break;
        }
        case TINYIMG_OP_HUE: {
            // the SVG feColorMatrix hueRotate matrix, so a hue change through
            // this library and through a browser's filter agree
            float radians = op->scalar.value * 3.14159265358979f / 180.0f;
            double c = (double) tiny_cosf(radians);
            double s = (double) tiny_sinf(radians);

            static const double keep[9] = {0.787,  -0.715, -0.072,
                                           -0.213, 0.285,  -0.072,
                                           -0.213, -0.715, 0.928};
            static const double turn[9] = {-0.213, -0.715, 0.928, 0.143, 0.140,
                                           -0.283, -0.787, 0.715, 0.072};
            static const double base[9] = {0.213, 0.715, 0.072, 0.213, 0.715,
                                           0.072, 0.213, 0.715, 0.072};

            for (uint32_t row = 0; row < 3u; row++) {
                for (uint32_t col = 0; col < 3u; col++) {
                    uint32_t i = row * 3u + col;
                    m[row * 4u + col] =
                        to_fixed(base[i] + c * keep[i] + s * turn[i]);
                }
            }
            break;
        }
        case TINYIMG_OP_MATRIX:
            for (uint32_t i = 0; i < 12u; i++) {
                m[i] = to_fixed((double) op->matrix.m[i]);
            }
            break;
        default: break;
    }
}

/**
 * @brief Builds one channel's table for a curve.
 *
 * @param lut Receives 256 entries.
 * @param kind Which curve.
 * @param p Its parameters.
 */
static void curve_of(uint8_t* lut, TinyCurveKind kind, const float* p) {
    switch (kind) {
        case TINYIMG_CURVE_GAMMA: tiny_lut_gamma(lut, p[0]); return;
        case TINYIMG_CURVE_SRGB: tiny_lut_srgb(lut, p[0] >= 0.0f); return;
        default: break;
    }

    for (uint32_t i = 0; i < 256u; i++) {
        float v = (float) i;
        float out = v;

        switch (kind) {
            case TINYIMG_CURVE_POSTERIZE: {
                // the levels are the endpoints of the range, so the darkest
                // and the lightest input both survive and a two level
                // posterize is black and white rather than black and grey
                float levels = p[0] - 1.0f;
                float step = 255.0f / levels;
                out = tiny_roundf(v / step) * step;
                break;
            }
            case TINYIMG_CURVE_THRESHOLD:
                out = v >= p[0] ? 255.0f : 0.0f;
                break;
            case TINYIMG_CURVE_SOLARIZE:
                out = v >= p[0] ? 255.0f - v : v;
                break;
            case TINYIMG_CURVE_EXPOSURE: out = v * tiny_powf(2.0f, p[0]); break;
            case TINYIMG_CURVE_LEVELS: {
                float t = (v - p[0]) / (p[1] - p[0]);
                t = tiny_clampf(t, 0.0f, 1.0f);
                if (!float_is(p[2], 1.0f)) t = tiny_powf(t, 1.0f / p[2]);
                out = p[3] + t * (p[4] - p[3]);
                break;
            }
            case TINYIMG_CURVE_FILL_LIGHT: {
                // a shadow lift weighted by how dark the pixel is, so the
                // highlights are left where they are rather than clipped
                float t = v / 255.0f;
                float lift = (1.0f - t) * (1.0f - t);
                out = 255.0f * (t + p[0] * lift * (1.0f - t));
                break;
            }
            case TINYIMG_CURVE_GAIN: out = v * p[0]; break;
            case TINYIMG_CURVE_SIGMOID: {
                float t = v / 255.0f - 0.5f;
                float k = p[0];
                float shaped = t + k * t * (0.25f - t * t) * 4.0f;
                out = 255.0f * (shaped + 0.5f);
                break;
            }
            case TINYIMG_CURVE_NEGATE: out = 255.0f - v; break;
            case TINYIMG_CURVE_BALANCE: {
                // three overlapping windows over the range, so the weights
                // sum to about one everywhere and a shift in one band fades
                // into the next rather than stepping
                float t = v / 255.0f;
                float shadow = (1.0f - t) * (1.0f - t);
                float highlight = t * t;
                float mid = 1.0f - shadow - highlight;

                out = v +
                      255.0f * (p[0] * shadow + p[1] * mid + p[2] * highlight);
                break;
            }
            default: break;
        }

        lut[i] = tiny_clamp_u8f(out);
    }
}

/**
 * @brief Walks a resolved plan's colour operations into stages.
 *
 * The one implementation of the collapse. Passing NULL for `stages` counts them
 * without building them, which is what the resolution reports, so the count a
 * caller reads and the stages the executor runs cannot disagree about where the
 * boundaries are.
 *
 * @param resolution A resolved plan.
 * @param stages Receives the stages, or NULL to count only.
 * @param capacity How many `stages` holds; ignored when it is NULL.
 * @param count Receives how many stages there are.
 * @param before Receives how many of them run before the resample. May be NULL.
 * @return int TINYIMG_OK or TINYIMG_ERR_BUFFER_TOO_SMALL.
 */
static int color_walk(
    const TinyPlanResolution* resolution, TinyColorStage* stages,
    uint32_t capacity, uint32_t* count, uint32_t* before
) {
    *count = 0;
    if (before) *before = 0;

    if (resolution->kernels & TINYIMG_KERNEL_GRAY_DECODE) return TINYIMG_OK;

    TinyPlanOpClass last = TINYIMG_OP_CLASS_GEOMETRY;
    int resampled = 0;
    int barrier = 0;

    for (uint32_t i = 0; i < resolution->consumed; i++) {
        const TinyPlanOp* op = &resolution->op[i];
        TinyPlanOpClass cls = tiny_plan_op_class(op->kind);

        if (cls != TINYIMG_OP_CLASS_COLOR_MATRIX &&
            cls != TINYIMG_OP_CLASS_COLOR_LUT) {
            // a resample separates two stages of the same kind, which merging
            // them by kind alone would run on the wrong side of. a crop or a
            // turn does not: it moves pixels without mixing them
            if (op->kind == TINYIMG_OP_RESIZE || op->kind == TINYIMG_OP_FIT) {
                resampled = 1;
                barrier = 1;
            }
            continue;
        }

        if (*count == 0 || barrier || last != cls) {
            if (stages) {
                if (*count >= capacity) return TINYIMG_ERR_BUFFER_TOO_SMALL;

                TinyColorStage* fresh = &stages[*count];
                fresh->kind = cls;
                matrix_identity(fresh->matrix);

                for (uint32_t channel = 0; channel < 3u; channel++) {
                    tiny_lut_identity(fresh->lut[channel]);
                }
            }
            else if (*count >= capacity) {
                return TINYIMG_ERR_BUFFER_TOO_SMALL;
            }

            (*count)++;
            barrier = 0;
        }

        last = cls;
        if (before && !resampled) *before = *count;

        if (!stages) continue;

        TinyColorStage* stage = &stages[*count - 1u];

        if (cls == TINYIMG_OP_CLASS_COLOR_MATRIX) {
            int32_t single[12];
            int32_t composed[12];

            matrix_of(op, single);
            matrix_mul(composed, stage->matrix, single);

            for (uint32_t k = 0; k < 12u; k++) stage->matrix[k] = composed[k];
        }
        else {
            uint8_t single[256];
            uint8_t mask = 7u;

            if (op->kind == TINYIMG_OP_CURVE) {
                curve_of(single, op->curve.kind, op->curve.p);
                if (op->curve.channels != 0u) mask = op->curve.channels;
            }
            else {
                tiny_lut_gamma(single, op->scalar.value);
            }

            for (uint32_t channel = 0; channel < 3u; channel++) {
                if ((mask & (1u << channel)) == 0u) continue;

                tiny_lut_compose(
                    stage->lut[channel], stage->lut[channel], single
                );
            }
        }
    }

    return TINYIMG_OK;
}

int tiny_plan_color_stages(
    const TinyPlanResolution* resolution, TinyColorStage* stages,
    uint32_t capacity, uint32_t* count
) {
    if (!resolution || !stages || !count) return TINYIMG_ERR_NULL;
    return color_walk(resolution, stages, capacity, count, 0);
}

/**
 * @brief Applies one stage to a pixel in place.
 *
 * A pixel with fewer than three channels is treated as one whose channels are
 * all equal, and the result is taken as its luminance. That is the same answer
 * as widening to RGB, applying the stage and reducing again, without the two
 * conversions: it keeps a hue rotation of a grey image grey, which taking the
 * first row alone would not.
 *
 * @param stage The stage.
 * @param pixel The pixel, `channels` bytes.
 * @param channels How many channels it has.
 */
static void stage_apply(
    const TinyColorStage* stage, uint8_t* pixel, uint8_t channels
) {
    if (stage->kind == TINYIMG_OP_CLASS_COLOR_LUT) {
        if (channels < 3u) {
            pixel[0] = stage->lut[1][pixel[0]];
            return;
        }

        pixel[0] = stage->lut[0][pixel[0]];
        pixel[1] = stage->lut[1][pixel[1]];
        pixel[2] = stage->lut[2][pixel[2]];
        return;
    }

    const int32_t* m = stage->matrix;

    if (channels < 3u) {
        int32_t value = pixel[0];
        int64_t out[3];

        for (uint32_t row = 0; row < 3u; row++) {
            out[row] =
                (int64_t) (m[row * 4u] + m[row * 4u + 1u] + m[row * 4u + 2u]) *
                    value +
                m[row * 4u + 3u];
        }

        int64_t luma =
            (out[0] * LUMA_R + out[1] * LUMA_G + out[2] * LUMA_B) / COLOR_ONE;

        pixel[0] =
            tiny_clamp_u8((int32_t) ((luma + COLOR_ONE / 2) / COLOR_ONE));
        return;
    }

    int32_t r = pixel[0];
    int32_t g = pixel[1];
    int32_t b = pixel[2];

    for (uint32_t row = 0; row < 3u; row++) {
        int64_t sum = (int64_t) m[row * 4u] * r +
                      (int64_t) m[row * 4u + 1u] * g +
                      (int64_t) m[row * 4u + 2u] * b + m[row * 4u + 3u];

        pixel[row] =
            tiny_clamp_u8((int32_t) ((sum + COLOR_ONE / 2) / COLOR_ONE));
    }
}

static void stages_apply(
    const TinyColorStage* stages, uint32_t from, uint32_t to, uint8_t* pixel,
    uint8_t channels
) {
    for (uint32_t i = from; i < to; i++) {
        stage_apply(&stages[i], pixel, channels);
    }
}

#pragma endregion

#pragma region resampling

/**
 * @brief The source samples one output sample reads.
 *
 * The weights live in a pool rather than in the structure, because how many
 * there are depends on the reduction: a filter covering four source pixels at
 * unit scale covers thirty-six at a nine times reduction, and a fixed four
 * would have to either alias or lie. `offset` is where this sample's run starts
 * in that pool.
 */
typedef struct {
    uint32_t first;
    uint32_t count;
    uint32_t offset;
} Tap;

/**
 * @brief The triangle kernel, which is what bilinear interpolation is.
 *
 * @param t Distance from the centre, in filter units.
 * @return double The weight.
 */
static double kernel_triangle(double t) {
    if (t < 0.0) t = -t;
    return t < 1.0 ? 1.0 - t : 0.0;
}

/**
 * @brief The Catmull-Rom cubic, the interpolating member of its family.
 *
 * @param t Distance from the centre, in filter units.
 * @return double The weight, which is negative between one and two and is what
 * gives the filter its edge.
 */
static double kernel_catmull(double t) {
    if (t < 0.0) t = -t;

    double t2 = t * t;

    if (t < 1.0) return 1.5 * t2 * t - 2.5 * t2 + 1.0;
    if (t < 2.0) return -0.5 * t2 * t + 2.5 * t2 - 4.0 * t + 2.0;

    return 0.0;
}

/** How far a filter reaches at unit scale. */
static double filter_radius(TinyResampleFilter filter) {
    switch (filter) {
        case TINYIMG_FILTER_NEAREST: return 0.5;
        case TINYIMG_FILTER_BOX: return 0.5;
        case TINYIMG_FILTER_BILINEAR: return 1.0;
        default: return 2.0;
    }
}

/**
 * @brief The most samples any one output sample can read.
 *
 * Used to size the pool before the weights are known, so it has to be an upper
 * bound rather than an estimate.
 *
 * @param filter The filter.
 * @param step Source pixels per output pixel.
 * @return uint32_t The bound.
 */
static uint32_t taps_bound(TinyResampleFilter filter, double step) {
    if (filter == TINYIMG_FILTER_NEAREST) return 1u;

    double scale = step > 1.0 ? step : 1.0;
    return (uint32_t) (2.0 * filter_radius(filter) * scale) + 3u;
}

/**
 * @brief The weight one source sample carries for one output sample.
 *
 * @param filter The filter.
 * @param sample The source sample's index.
 * @param center Where the output sample sits, in source pixels.
 * @param scale Source pixels per output pixel, never below one.
 * @return double The weight, before normalisation.
 */
static double tap_weight(
    TinyResampleFilter filter, int32_t sample, double center, double scale
) {
    if (filter == TINYIMG_FILTER_NEAREST) return 1.0;

    if (filter == TINYIMG_FILTER_BOX) {
        // the fraction of this source pixel the output pixel covers
        double lower = center - 0.5 * scale;
        double upper = center + 0.5 * scale;
        double left = (double) sample > lower ? (double) sample : lower;
        double right = sample + 1.0 < upper ? sample + 1.0 : upper;

        return right > left ? right - left : 0.0;
    }

    double t = ((double) sample + 0.5 - center) / scale;
    return filter == TINYIMG_FILTER_BILINEAR ? kernel_triangle(t)
                                             : kernel_catmull(t);
}

/**
 * @brief Builds the sample map for one axis.
 *
 * Two things here are what separate a resampler from a pixel picker.
 *
 * The support scales with the reduction. A filter is defined over the output's
 * pixel spacing, so reducing by nine means each output pixel is nine source
 * pixels wide and the filter has to cover all nine; leaving the support at unit
 * width reads two of them and aliases the rest. With it scaled, a bilinear and
 * a Catmull-Rom reduction agree with ImageMagick's Triangle and Catrom to 98
 * and 89 dB, and nearest agrees exactly.
 *
 * The box filter weights by area rather than by membership. An output pixel
 * whose footprint ends three tenths of the way into a source pixel takes three
 * tenths of it, not all of it and not none of it. That makes it exact for a
 * whole number ratio, which is what tests/c/plan/kernels.c asserts against
 * means worked out by hand, and it is why the codecs can use whole pixel
 * footprints for their halves and quarters where this cannot.
 *
 * @param taps Receives `count` entries.
 * @param pool Receives the weights, at least `count * taps_bound` entries.
 * @param count How many output samples there are.
 * @param origin Where the window starts, in source pixels.
 * @param extent How wide the window is, in source pixels.
 * @param dim The source extent.
 * @param filter Which weights to use; never TINYIMG_FILTER_AUTO.
 */
static void taps_build(
    Tap* taps, int32_t* pool, uint32_t count, double origin, double extent,
    uint32_t dim, TinyResampleFilter filter
) {
    double step = extent / (double) count;
    double scale = step > 1.0 ? step : 1.0;
    double support = filter_radius(filter) * scale;
    uint32_t stride = taps_bound(filter, step);

    /*
     * A filter reaches past the sample it is centred on, and what it finds
     * there has to be the window's own edge rather than whatever the decode
     * happens to have next to it. Otherwise a crop followed by an enlargement
     * reads pixels the crop removed, which is how a watermark cropped out of an
     * image comes back along one edge.
     */
    int32_t low = tiny_clampi((int32_t) floor_d(origin), 0, (int32_t) dim - 1);
    int32_t high = tiny_clampi(
        (int32_t) ceil_d(origin + extent) - 1, low, (int32_t) dim - 1
    );

    for (uint32_t i = 0; i < count; i++) {
        double center = origin + ((double) i + 0.5) * step;
        int32_t* weights = pool + (size_t) i * stride;

        int32_t first;
        int32_t last;

        if (filter == TINYIMG_FILTER_NEAREST) {
            first = tiny_clampi((int32_t) floor_d(center), low, high);
            last = first;
        }
        else {
            first = (int32_t) ceil_d(center - support - 0.5);
            last = (int32_t) floor_d(center + support - 0.5);

            if (last < first) last = first;

            first = tiny_clampi(first, low, high);
            last = tiny_clampi(last, first, high);
        }

        taps[i].first = (uint32_t) first;
        taps[i].count = (uint32_t) (last - first) + 1u;
        taps[i].offset = i * stride;

        // the kernel is evaluated twice rather than kept in a scratch array,
        // because the run has no useful upper bound: a large enough reduction
        // makes it thousands of samples long
        double sum = 0.0;

        for (int32_t j = first; j <= last; j++) {
            sum += tap_weight(filter, j, center, scale);
        }

        // a run at the window's edge is clipped, so its weights do not sum to
        // one until they are normalised, and normalising is also what turns the
        // clipping into edge extension
        if (sum <= 0.0) sum = 1.0;

        int32_t total = 0;

        for (uint32_t k = 0; k < taps[i].count; k++) {
            double weight =
                tap_weight(filter, first + (int32_t) k, center, scale) / sum;

            weights[k] = to_fixed(weight);
            total += weights[k];
        }

        // rounding leaves the sum a few parts in 65536 off one, which would
        // show as a flat image drifting a level; the largest weight absorbs it
        uint32_t largest = 0;
        for (uint32_t k = 1; k < taps[i].count; k++) {
            if (weights[k] > weights[largest]) largest = k;
        }
        weights[largest] += COLOR_ONE - total;

        // a sample weighted zero is a read that cannot change the answer, and a
        // cubic at a whole position has three of them. trimming the run is what
        // makes an unscaled axis cost one read per pixel through any filter
        uint32_t front = 0;
        while (front + 1u < taps[i].count && weights[front] == 0) front++;

        while (taps[i].count > front + 1u && weights[taps[i].count - 1u] == 0) {
            taps[i].count--;
        }

        if (front > 0) {
            taps[i].first += front;
            taps[i].count -= front;
            taps[i].offset += front;
        }
    }
}

/**
 * @brief Whether an axis' map is one source pixel per output pixel, in order.
 *
 * An offset is allowed, so the map of a crop counts: it is still a run of
 * consecutive source pixels and still needs no filter, which is what lets a
 * crop reach the output as a row copy per row rather than a sample per pixel.
 */
static int taps_are_copy(const Tap* taps, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (taps[i].first != taps[0].first + i || taps[i].count != 1u) return 0;
    }

    return 1;
}

#pragma endregion
#pragma endregion

#pragma region execution

/** Everything one fused pass needs, gathered so the loops stay readable. */
typedef struct {
    const uint8_t* source;
    uint32_t source_width;
    uint32_t source_height;
    uint8_t source_channels;
    const Tap* taps_x;
    const Tap* taps_y;
    const int32_t* pool_x;
    const int32_t* pool_y;
    const TinyColorStage* stages;
    uint32_t stages_before;
    uint32_t stages_total;
    int premultiply;
    int copies;
    Orient orient;
    uint32_t sample_width;
    uint32_t sample_height;
    uint8_t out_channels;
} Pass;

/** Reads one source pixel with the pass' leading colour stages applied. */
static void pass_read(
    const Pass* pass, uint32_t x, uint32_t y, uint8_t* pixel
) {
    const uint8_t* src = pass->source + ((size_t) y * pass->source_width + x) *
                                            pass->source_channels;

    for (uint32_t c = 0; c < pass->source_channels; c++) pixel[c] = src[c];

    if (pass->stages_before > 0) {
        stages_apply(
            pass->stages, 0, pass->stages_before, pixel, pass->source_channels
        );
    }
}

/**
 * @brief Resamples one output pixel.
 *
 * Two dimensional rather than two separable passes, which trades work for an
 * intermediate: a cubic enlargement reads sixteen samples where a separable
 * version would read eight, and a box reduction reads each source pixel exactly
 * once either way. The intermediate is what the library exists to avoid, and it
 * is also what would have to carry the orientation, so folding the orientation
 * into the gather is free here and is not there.
 *
 * @param pass The pass.
 * @param x Column in sample space.
 * @param y Row in sample space.
 * @param out Receives `source_channels` bytes.
 */
static void pass_sample(
    const Pass* pass, uint32_t x, uint32_t y, uint8_t* out
) {
    const Tap* tx = &pass->taps_x[x];
    const Tap* ty = &pass->taps_y[y];
    uint8_t channels = pass->source_channels;
    uint8_t alpha_at = (uint8_t) (channels - 1u);

    if (tx->count == 1u && ty->count == 1u) {
        pass_read(pass, tx->first, ty->first, out);
        return;
    }

    const int32_t* wx = pass->pool_x + tx->offset;
    const int32_t* wy = pass->pool_y + ty->offset;
    int64_t total[4] = {0, 0, 0, 0};

    for (uint32_t j = 0; j < ty->count; j++) {
        int64_t row_total[4] = {0, 0, 0, 0};

        for (uint32_t i = 0; i < tx->count; i++) {
            uint8_t pixel[4];
            pass_read(pass, tx->first + i, ty->first + j, pixel);

            int32_t weight = wx[i];
            int32_t scale = pass->premultiply ? pixel[alpha_at] : 1;

            for (uint32_t c = 0; c < channels; c++) {
                int32_t value = pixel[c];

                // colour is premultiplied so a transparent pixel cannot bleed
                // into its neighbours; alpha itself is already the weight
                if (pass->premultiply && c != alpha_at) {
                    value = value * scale / 255;
                }

                row_total[c] += (int64_t) value * weight;
            }
        }

        for (uint32_t c = 0; c < channels; c++) {
            total[c] += row_total[c] * wy[j];
        }
    }

    // every axis' weights are normalised to one, so the divisor is always two
    // to the thirty second and this is a shift rather than a sixty four bit
    // divide per channel
    for (uint32_t c = 0; c < channels; c++) {
        out[c] =
            tiny_clamp_u8((int32_t) ((total[c] + ((int64_t) 1 << 31)) >> 32));
    }

    if (pass->premultiply && out[alpha_at] > 0 && out[alpha_at] < 255) {
        for (uint32_t c = 0; c < alpha_at; c++) {
            int32_t value = (out[c] * 255 + out[alpha_at] / 2) / out[alpha_at];
            out[c] = tiny_clamp_u8(value);
        }
    }
}

/**
 * @brief Runs one fused pass into an image.
 *
 * @param pass The pass.
 * @param out The output, already created and already filled with the
 * background where it will not be written.
 * @param offset_x Where the oriented result starts.
 * @param offset_y Where the oriented result starts.
 */
static void pass_run(
    const Pass* pass, TinyImage* out, uint32_t offset_x, uint32_t offset_y
) {
    int swap = orient_swaps(pass->orient);
    uint32_t width = swap ? pass->sample_height : pass->sample_width;
    uint32_t height = swap ? pass->sample_width : pass->sample_height;
    int identity = orient_is_identity(pass->orient);

    /*
     * The special cases the planner reports, taken. Anything that reads one
     * source pixel per output pixel in order needs no sample map, no weights
     * and no divide, and the whole row is one call: a plan that only decodes,
     * one that only crops, and one that only changes colour all land here. The
     * general path below is a hundred and twenty nanoseconds a pixel and this
     * is the memory bandwidth, so it is worth the twenty lines.
     */
    if (identity && pass->copies) {
        uint32_t left = pass->taps_x[0].first;
        int plain = pass->stages_total == 0 &&
                    pass->out_channels == pass->source_channels;

        for (uint32_t j = 0; j < height; j++) {
            const uint8_t* src =
                pass->source +
                ((size_t) pass->taps_y[j].first * pass->source_width + left) *
                    pass->source_channels;
            uint8_t* row =
                out->data + ((size_t) (j + offset_y) * out->width + offset_x) *
                                out->channels;

            if (plain) {
                tiny_memcpy(row, src, (size_t) width * out->channels);
                continue;
            }

            for (uint32_t i = 0; i < width; i++) {
                uint8_t pixel[4] = {0, 0, 0, 255};

                for (uint32_t c = 0; c < pass->source_channels; c++) {
                    pixel[c] = src[(size_t) i * pass->source_channels + c];
                }

                stages_apply(
                    pass->stages, 0, pass->stages_total, pixel,
                    pass->source_channels
                );
                tiny_pixel_convert(
                    row + (size_t) i * out->channels, out->channels, pixel,
                    pass->source_channels
                );
            }
        }

        return;
    }

    for (uint32_t j = 0; j < height; j++) {
        uint8_t* row =
            out->data +
            ((size_t) (j + offset_y) * out->width + offset_x) * out->channels;

        for (uint32_t i = 0; i < width; i++) {
            uint32_t u = i;
            uint32_t v = j;

            if (!identity) {
                orient_read(
                    pass->orient, pass->sample_width, pass->sample_height, i, j,
                    &u, &v
                );
            }

            uint8_t pixel[4] = {0, 0, 0, 255};
            pass_sample(pass, u, v, pixel);

            if (pass->stages_total > pass->stages_before) {
                stages_apply(
                    pass->stages, pass->stages_before, pass->stages_total,
                    pixel, pass->source_channels
                );
            }

            if (pass->out_channels == pass->source_channels) {
                for (uint32_t c = 0; c < pass->out_channels; c++) {
                    row[(size_t) i * pass->out_channels + c] = pixel[c];
                }
            }
            else {
                tiny_pixel_convert(
                    row + (size_t) i * pass->out_channels, pass->out_channels,
                    pixel, pass->source_channels
                );
            }
        }
    }
}

/**
 * @brief Blurs an image in place with a sliding window.
 *
 * The window carries a running sum, so the cost is one add and one subtract per
 * pixel per axis whatever the radius is. That is why there is no frequency
 * domain path anywhere in this library: the thing it would be faster than is
 * already independent of the radius.
 *
 * @param image The image.
 * @param radius Pixels either side.
 * @return int TINYIMG_OK or TINYIMG_ERR_MEMORY.
 */
static int blur_box(TinyImage* image, uint32_t radius) {
    if (radius == 0) return TINYIMG_OK;

    uint32_t channels = image->channels;
    uint32_t longest = tiny_max_u32(image->width, image->height);
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* line = tiny_arena_alloc((size_t) longest * channels, 0);
    uint32_t* sums = tiny_arena_alloc(channels * sizeof(uint32_t), 0);

    if (!line || !sums) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    for (uint32_t axis = 0; axis < 2u; axis++) {
        uint32_t length = axis == 0 ? image->width : image->height;
        uint32_t lines = axis == 0 ? image->height : image->width;
        size_t step = axis == 0 ? channels : (size_t) image->width * channels;
        size_t jump = axis == 0 ? (size_t) image->width * channels : channels;

        if (length < 2u) continue;

        uint32_t span = radius * 2u + 1u;

        for (uint32_t l = 0; l < lines; l++) {
            uint8_t* base = image->data + (size_t) l * jump;

            for (uint32_t i = 0; i < length; i++) {
                for (uint32_t c = 0; c < channels; c++) {
                    line[i * channels + c] = base[(size_t) i * step + c];
                }
            }

            for (uint32_t c = 0; c < channels; c++) {
                sums[c] = (uint32_t) line[c] * (radius + 1u);
            }

            for (uint32_t i = 1; i <= radius; i++) {
                uint32_t at = tiny_min_u32(i, length - 1u);
                for (uint32_t c = 0; c < channels; c++) {
                    sums[c] += line[at * channels + c];
                }
            }

            for (uint32_t i = 0; i < length; i++) {
                for (uint32_t c = 0; c < channels; c++) {
                    base[(size_t) i * step + c] =
                        (uint8_t) ((sums[c] + span / 2u) / span);
                }

                uint32_t leaving = i >= radius ? i - radius : 0u;
                uint32_t entering = tiny_min_u32(i + radius + 1u, length - 1u);

                for (uint32_t c = 0; c < channels; c++) {
                    sums[c] += line[entering * channels + c];
                    sums[c] -= line[leaving * channels + c];
                }
            }
        }
    }

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

/**
 * @brief Applies a blur operation to a materialised image.
 *
 * @param image The image.
 * @param op The operation.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int blur_apply(TinyImage* image, const TinyPlanOp* op) {
    if (!op->blur.gaussian) {
        return blur_box(image, (uint32_t) (op->blur.amount + 0.5f));
    }

    // three boxes of this width have the same variance as the gaussian asked
    // for, which is what makes them converge on it rather than merely resemble
    // it: 12 sigma^2 / n + 1, under the root
    float sigma = op->blur.amount;
    float width = tiny_sqrtf(4.0f * sigma * sigma + 1.0f);
    uint32_t radius = (uint32_t) ((width - 1.0f) / 2.0f + 0.5f);

    for (uint32_t i = 0; i < 3u; i++) {
        int result = blur_box(image, radius);
        if (result != TINYIMG_OK) return result;
    }

    return TINYIMG_OK;
}

int tiny_plan_blur_box(TinyImage* image, uint32_t radius) {
    return blur_box(image, radius);
}

int tiny_plan_replace(TinyImage* image, TinyPlan* plan) {
    if (!image || !plan) return TINYIMG_ERR_NULL;

    TinyImage out;
    tiny_memset(&out, 0, sizeof(out));

    int result = tiny_plan_run(plan, &out);
    if (result != TINYIMG_OK) return result;

    // metadata survives an operation on the pixels, so it moves across rather
    // than being freed with the image it came from
    out.meta = image->meta;
    image->meta = 0;

    tiny_image_destroy(image);
    *image = out;

    return TINYIMG_OK;
}

/** The one place a neighbourhood operation runs, whichever one it is. */
static int neighbourhood_apply(TinyImage* image, const TinyPlanOp* op) {
    if (op->kind == TINYIMG_OP_EFFECT) return tiny_effect_apply(image, op);
    return blur_apply(image, op);
}

static int run_fused(
    const TinyPlan* plan, const TinyPlanResolution* resolution,
    const TinyImage* source, TinyImage* out
);

/**
 * @brief Runs the operations a fused pass did not cover.
 *
 * @param plan The plan, for its background and fusion setting.
 * @param resolution The resolution whose pass has already run.
 * @param image The pass' output, consumed and replaced.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int run_remainder(
    const TinyPlan* plan, const TinyPlanResolution* resolution, TinyImage* image
) {
    uint32_t at = resolution->consumed;

    while (at < resolution->ops) {
        const TinyPlanOp* op = &resolution->op[at];

        if (tiny_plan_op_class(op->kind) == TINYIMG_OP_CLASS_NEIGHBOURHOOD) {
            int result = neighbourhood_apply(image, op);
            if (result != TINYIMG_OK) return result;

            at++;
            continue;
        }

        TinyPlan rest;
        int result = tiny_plan_init_image(&rest, image);
        if (result != TINYIMG_OK) return result;

        rest.fusion = plan->fusion;
        for (uint32_t i = 0; i < 4u; i++) {
            rest.background[i] = plan->background[i];
        }

        while (at < resolution->ops &&
               tiny_plan_op_class(resolution->op[at].kind) !=
                   TINYIMG_OP_CLASS_NEIGHBOURHOOD) {
            rest.ops[rest.count++] = resolution->op[at++];
        }

        TinyImage next;
        tiny_memset(&next, 0, sizeof(next));

        result = tiny_plan_run(&rest, &next);
        if (result != TINYIMG_OK) return result;

        tiny_image_destroy(image);
        *image = next;
    }

    return TINYIMG_OK;
}

/**
 * @brief Runs the operations exactly as they were appended, one pass each.
 *
 * The reference the fused path is measured against, and the benchmark's
 * planner-off arm. It deliberately reads the plan rather than a resolution: no
 * rewrite has run, and the source is a full decode, so a difference between
 * this and the fused path is a fault in the planner and cannot be a fault in
 * the resampler, which both paths share.
 *
 * @param plan The plan.
 * @param source A full decode of the source.
 * @param out Receives the output.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
static int run_eager(
    const TinyPlan* plan, const TinyImage* source, TinyImage* out
) {
    TinyImage current;
    tiny_memset(&current, 0, sizeof(current));

    int result = tiny_image_create(
        &current, source->width, source->height, source->channels
    );
    if (result != TINYIMG_OK) return result;

    tiny_memcpy(
        current.data, source->data,
        (size_t) source->width * source->height * source->channels
    );
    current.format = source->format;
    current.quality = source->quality;

    for (uint32_t i = 0; i < plan->count; i++) {
        const TinyPlanOp* op = &plan->ops[i];

        if (tiny_plan_op_class(op->kind) == TINYIMG_OP_CLASS_NEIGHBOURHOOD) {
            result = neighbourhood_apply(&current, op);
            if (result != TINYIMG_OK) break;
            continue;
        }

        TinyPlan single;
        result = tiny_plan_init_image(&single, &current);
        if (result != TINYIMG_OK) break;

        for (uint32_t k = 0; k < 4u; k++) {
            single.background[k] = plan->background[k];
        }
        single.ops[single.count++] = *op;

        TinyPlanResolution step;
        result = tiny_plan_resolve(&single, &step);

        TinyImage next;
        tiny_memset(&next, 0, sizeof(next));

        if (result == TINYIMG_OK) {
            result = run_fused(&single, &step, &current, &next);
        }
        if (result != TINYIMG_OK) break;

        tiny_image_destroy(&current);
        current = next;
    }

    if (result != TINYIMG_OK) {
        tiny_image_destroy(&current);
        return result;
    }

    *out = current;
    return TINYIMG_OK;
}

static int run_fused(
    const TinyPlan* plan, const TinyPlanResolution* resolution,
    const TinyImage* source, TinyImage* out
) {
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    Tap* taps_x = tiny_arena_alloc(resolution->sample_width * sizeof(Tap), 0);
    Tap* taps_y = tiny_arena_alloc(resolution->sample_height * sizeof(Tap), 0);
    TinyColorStage* stages = 0;
    uint32_t stage_count = 0;

    if (!taps_x || !taps_y) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    if (resolution->color_stages > 0) {
        stages = tiny_arena_alloc(
            resolution->color_stages * sizeof(TinyColorStage), 0
        );
        if (!stages) {
            tiny_arena_release(&mark);
            return TINYIMG_ERR_MEMORY;
        }

        int built = tiny_plan_color_stages(
            resolution, stages, resolution->color_stages, &stage_count
        );
        if (built != TINYIMG_OK) {
            tiny_arena_release(&mark);
            return built;
        }
    }

    // a codec is free to ignore the scale it was asked for, and the pixel
    // budget can clip a region, so the window is fitted to the decode that
    // actually came back rather than to the one that was requested
    double window_x = resolution->source_x;
    double window_y = resolution->source_y;
    double window_width = resolution->source_width;
    double window_height = resolution->source_height;

    if (resolution->decode_width != source->width ||
        resolution->decode_height != source->height) {
        double fit_x =
            (double) source->width / (double) resolution->decode_width;
        double fit_y =
            (double) source->height / (double) resolution->decode_height;

        window_x *= fit_x;
        window_width *= fit_x;
        window_y *= fit_y;
        window_height *= fit_y;
    }

    double step_x = window_width / (double) resolution->sample_width;
    double step_y = window_height / (double) resolution->sample_height;

    int32_t* pool_x = tiny_arena_alloc(
        (size_t) resolution->sample_width *
            taps_bound(resolution->filter_x, step_x) * sizeof(int32_t),
        0
    );
    int32_t* pool_y = tiny_arena_alloc(
        (size_t) resolution->sample_height *
            taps_bound(resolution->filter_y, step_y) * sizeof(int32_t),
        0
    );

    if (!pool_x || !pool_y) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    taps_build(
        taps_x, pool_x, resolution->sample_width, window_x, window_width,
        source->width, resolution->filter_x
    );
    taps_build(
        taps_y, pool_y, resolution->sample_height, window_y, window_height,
        source->height, resolution->filter_y
    );

    int result = tiny_image_create(
        out, resolution->width, resolution->height, resolution->channels
    );
    if (result != TINYIMG_OK) {
        tiny_arena_release(&mark);
        return result;
    }

    out->format = source->format;
    out->quality = source->quality;

    if (resolution->kernels & TINYIMG_KERNEL_PAD) {
        uint8_t background[4];
        for (uint32_t c = 0; c < 4u; c++) background[c] = plan->background[c];

        // a colour operation the caller put after the fit applies to the
        // padding as well, because eagerly it would have
        if (stage_count > resolution->color_stages_before) {
            stages_apply(
                stages, resolution->color_stages_before, stage_count,
                background, resolution->channels
            );
        }

        for (uint32_t y = 0; y < out->height; y++) {
            uint8_t* row = out->data + (size_t) y * out->width * out->channels;

            for (uint32_t x = 0; x < out->width; x++) {
                for (uint32_t c = 0; c < out->channels; c++) {
                    row[(size_t) x * out->channels + c] = background[c];
                }
            }
        }
    }

    int copies = taps_are_copy(taps_x, resolution->sample_width) &&
                 taps_are_copy(taps_y, resolution->sample_height);

    Pass pass;
    pass.source = source->data;
    pass.source_width = source->width;
    pass.source_height = source->height;
    pass.source_channels = source->channels;
    pass.taps_x = taps_x;
    pass.taps_y = taps_y;
    pass.pool_x = pool_x;
    pass.pool_y = pool_y;
    pass.stages = stages;
    pass.stages_before = resolution->color_stages_before;
    pass.stages_total = stage_count;
    pass.copies = copies;
    pass.premultiply =
        !copies && (source->channels == 2u || source->channels == 4u);
    pass.sample_width = resolution->sample_width;
    pass.sample_height = resolution->sample_height;
    pass.out_channels = resolution->channels;

    for (uint32_t i = 0; i < 4u; i++)
        pass.orient.m[i] = resolution->orientation[i];

    pass_run(&pass, out, resolution->offset_x, resolution->offset_y);

    tiny_arena_release(&mark);
    return TINYIMG_OK;
}

/** Whether any fit op still needs a focus resolved from the pixels. */
static int wants_focus(const TinyPlan* plan) {
    for (uint32_t i = 0; i < plan->count; i++) {
        const TinyPlanOp* op = &plan->ops[i];

        if (op->kind != TINYIMG_OP_FIT) continue;
        if (op->fit.focused) continue;

        if (op->fit.gravity == TINYIMG_GRAVITY_AUTO ||
            op->fit.gravity == TINYIMG_GRAVITY_FACE) {
            return 1;
        }
    }

    return 0;
}

/**
 * @brief Answers the computed gravities before the plan is resolved.
 *
 * The one place in the planner that reads a pixel before deciding anything, and
 * it is a separate pass for that reason: tiny_plan_resolve has to stay a
 * function of the plan alone, so a question that needs the image is answered
 * first and written onto the plan as an operand.
 *
 * The read is a scaled decode rather than a full one. Detail statistics are
 * happy at an eighth, and face detection is not, so the box asked for is the
 * long side the detector needs; asking for less loses faces, and asking for the
 * whole image costs more than the transformation it is deciding.
 *
 * @param plan The plan to answer for.
 * @param out Receives a copy with the focus filled in.
 * @return int TINYIMG_OK, or a negative TinyImageError. A detector that finds
 * nothing is not a failure.
 */
static int resolve_focus(const TinyPlan* plan, TinyPlan* out) {
    *out = *plan;

    TinyImage probe;
    tiny_memset(&probe, 0, sizeof(probe));

    const TinyImage* source = plan->image;

    if (!source) {
        int result = tiny_image_load_scaled(
            &probe, plan->buffer, plan->size, TINYIMG_DETECT_LONG_SIDE,
            TINYIMG_DETECT_LONG_SIDE
        );
        if (result != TINYIMG_OK) return result;

        source = &probe;
    }

    float auto_x = 0.5f;
    float auto_y = 0.5f;
    float face_x = 0.5f;
    float face_y = 0.5f;
    int have_auto = 0;
    int have_face = 0;

    for (uint32_t i = 0; i < out->count; i++) {
        TinyPlanOp* op = &out->ops[i];

        if (op->kind != TINYIMG_OP_FIT || op->fit.focused) continue;

        // computed once per kind rather than once per operation; a chain with
        // two face-gravity fits in it asks the detector one question
        if (op->fit.gravity == TINYIMG_GRAVITY_AUTO) {
            if (!have_auto) {
                tiny_image_focus(
                    source, TINYIMG_GRAVITY_AUTO, &auto_x, &auto_y
                );
                have_auto = 1;
            }

            op->fit.focus_x = auto_x;
            op->fit.focus_y = auto_y;
            op->fit.focused = 1u;
        }
        else if (op->fit.gravity == TINYIMG_GRAVITY_FACE) {
            if (!have_face) {
                tiny_image_focus(
                    source, TINYIMG_GRAVITY_FACE, &face_x, &face_y
                );
                have_face = 1;
            }

            op->fit.focus_x = face_x;
            op->fit.focus_y = face_y;
            op->fit.focused = 1u;
        }
    }

    if (source == &probe) tiny_image_destroy(&probe);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_plan_run")
int tiny_plan_run(const TinyPlan* plan, TinyImage* out) {
    if (!plan || !out) return TINYIMG_ERR_NULL;
    if (!plan->buffer && !plan->image) return TINYIMG_ERR_NULL;

    TinyPlan focused;
    int result = TINYIMG_OK;

    if (wants_focus(plan)) {
        result = resolve_focus(plan, &focused);
        if (result != TINYIMG_OK) return result;

        plan = &focused;
    }

    TinyPlanResolution resolution;
    result = plan->fusion ? tiny_plan_resolve(plan, &resolution) : TINYIMG_OK;
    if (result != TINYIMG_OK) return result;

    TinyImage decoded;
    tiny_memset(&decoded, 0, sizeof(decoded));

    const TinyImage* source = plan->image;

    if (!source) {
        result = tiny_image_decode(
            &decoded, plan->buffer, plan->size,
            plan->fusion ? &resolution.decode : 0
        );
        if (result != TINYIMG_OK) return result;

        source = &decoded;
    }

    if (plan->fusion) {
        result = run_fused(plan, &resolution, source, out);

        if (result == TINYIMG_OK) {
            result = run_remainder(plan, &resolution, out);
            if (result != TINYIMG_OK) tiny_image_destroy(out);
        }
    }
    else {
        result = run_eager(plan, source, out);
    }

    if (source == &decoded) tiny_image_destroy(&decoded);
    return result;
}

TINYIMG_EXPORT("tiny_plan_encode")
int tiny_plan_encode(
    const TinyPlan* plan, TinyImageFormat format, const TinyEncodeOpts* opts,
    TinyWriter* writer
) {
    TinyImage image;
    tiny_memset(&image, 0, sizeof(image));

    int result = tiny_plan_run(plan, &image);
    if (result != TINYIMG_OK) return result;

    result = tiny_image_encode(&image, format, opts, writer);
    tiny_image_destroy(&image);

    return result;
}

#pragma endregion
