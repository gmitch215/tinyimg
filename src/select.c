#include "tinyimg/select.h"

#include "tinyimg/memory.h"
#include "tinyimg/util.h"

// #region tables

typedef struct {
    uint8_t format;
    uint8_t quality;
    uint8_t lossless;
} Variant;

// the order is the search order, which breaks every tie
static const Variant variants[TINYIMG_SELECT_MAX_VARIANTS] = {
    {TINYIMG_FORMAT_PNG, 0, 1},   {TINYIMG_FORMAT_JPEG, 50, 0},
    {TINYIMG_FORMAT_JPEG, 75, 0}, {TINYIMG_FORMAT_JPEG, 85, 0},
    {TINYIMG_FORMAT_JPEG, 90, 0}, {TINYIMG_FORMAT_JPEG, 95, 0},
    {TINYIMG_FORMAT_GIF, 0, 0},   {TINYIMG_FORMAT_BMP, 0, 1},
    {TINYIMG_FORMAT_TIFF, 0, 1},  {TINYIMG_FORMAT_WEBP, 0, 1},
    {TINYIMG_FORMAT_WEBP, 50, 0}, {TINYIMG_FORMAT_WEBP, 75, 0},
    {TINYIMG_FORMAT_WEBP, 90, 0}, {TINYIMG_FORMAT_WEBP, 95, 0},
    {TINYIMG_FORMAT_AVIF, 50, 0}, {TINYIMG_FORMAT_AVIF, 75, 0},
    {TINYIMG_FORMAT_AVIF, 90, 0}, {TINYIMG_FORMAT_AVIF, 95, 0},
};

/** The encoder's own default tier, which the fixed policy writes at. */
static uint8_t default_quality(TinyImageFormat format) {
    switch (format) {
        case TINYIMG_FORMAT_JPEG: return 85u;
        case TINYIMG_FORMAT_WEBP:
        case TINYIMG_FORMAT_AVIF: return 75u;
        default: return 0u;
    }
}

/*
 * Held-out coverage of each table's fifth percentile, in thousandths, from
 * `bench/baselines/distortion` (`--check-test`, the test split as it stood).
 * Zero is "not measured", which is not zero coverage.
 */
#define LEVER_COVERAGE_ALL 893u
static const uint16_t lever_coverage[8] = {0, 989, 888, 0, 879, 0, 645, 946};
static const uint16_t encode_coverage[8] = {0, 1000, 900, 0, 933, 0, 953, 956};

// #endregion

// #region composition

TinyDistortionClass tiny_select_weakest(
    TinyDistortionClass a, TinyDistortionClass b
) {
    static const uint8_t strength[4] = {0, 3, 2, 1};
    return strength[(uint32_t) a & 3u] <= strength[(uint32_t) b & 3u] ? a : b;
}

#define LN10 2.302585093f

static float noise_power(TinyDistortionClass klass, float db) {
    if (klass == TINYIMG_DISTORTION_EXACT) return 0.0f;
    return tiny_powf(10.0f, -db / 10.0f);
}

float tiny_select_compose_db(
    TinyDistortionClass lever_class, float lever_db,
    TinyDistortionClass encode_class, float encode_db
) {
    float noise = noise_power(lever_class, lever_db) +
                  noise_power(encode_class, encode_db);

    if (noise <= 0.0f) return 99.0f;

    float db = -10.0f * tiny_logf(noise) / LN10;
    return db > 99.0f ? 99.0f : db;
}

// #endregion

// #region search

typedef struct {
    TinyDistortionClass klass;
    uint8_t known;
    float p05;
    float median;
} LeverInfo;

typedef struct {
    TinyDistortionClass klass;
    // an encode with no row that is lossless by construction adds no noise
    TinyDistortionClass noise_class;
    uint8_t has_row;
    uint8_t alpha_ok;
    uint32_t upper_us;
    float p05;
    float median;
    float bytes_per_pixel;
} VariantInfo;

typedef struct {
    TinySelectOpts opts;
    uint32_t scale;
    uint32_t formats;
    uint32_t peak_cap;
    uint32_t alpha;
    TinyImageFormat source;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count;
    TinyPlanResolution base;
    LeverInfo lever[TINYIMG_PLAN_MAX_CANDIDATES];
    VariantInfo variant[TINYIMG_SELECT_MAX_VARIANTS];
} Search;

static uint32_t saturate(uint64_t value) {
    return value > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) value;
}

static uint32_t scaled_up(uint64_t micros, uint32_t scale_milli) {
    return saturate((micros * scale_milli + 999u) / 1000u);
}

static uint32_t round_milli(float value, float unit) {
    if (value <= 0.0f) return 0u;

    float scaled = value * unit + 0.5f;
    return scaled >= 4294967040.0f ? 0xFFFFFFFFu : (uint32_t) scaled;
}

static uint32_t default_formats(TinyImageFormat source) {
    uint32_t mask = TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_JPEG) |
                    TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP);

    if (source >= TINYIMG_FORMAT_PNG && source <= TINYIMG_FORMAT_AVIF) {
        mask |= TINYIMG_SELECT_FORMAT(source);
    }

    return mask;
}

static uint32_t only_writable(uint32_t mask) {
    return mask & 0xFEu;
}

static int format_carries_alpha(TinyImageFormat format) {
    return format != TINYIMG_FORMAT_JPEG && format != TINYIMG_FORMAT_AVIF &&
           format != TINYIMG_FORMAT_GIF;
}

static uint8_t steps_between(uint8_t from, uint8_t to) {
    uint8_t steps = 0;

    while (from < to && steps < 3u) {
        from = (uint8_t) (from * 2u);
        steps++;
    }

    return steps;
}

static void fill_lever(Search* s) {
    for (uint32_t i = 0; i < s->count; i++) {
        const TinyPlanCandidate* c = &s->lattice[i];
        const TinyPlanCandidate* b = &s->lattice[0];
        LeverInfo* info = &s->lever[i];
        TinyDistortion row;

        tiny_memset(info, 0, sizeof(*info));

        // candidate 0 is the reference, so it is exact by definition
        if (i == 0u) {
            info->klass = TINYIMG_DISTORTION_EXACT;
            info->known = 1;
            info->p05 = 99.0f;
            info->median = 99.0f;
            continue;
        }

        uint8_t effort_delta = c->effort != b->effort ? 1u : 0u;
        uint8_t filter_delta =
            c->filter_x != b->filter_x || c->filter_y != b->filter_y ? 1u : 0u;

        info->klass = tiny_distortion_lever(
            s->source, effort_delta, filter_delta,
            steps_between(b->scale_den, c->scale_den), &row
        );

        if (info->klass != TINYIMG_DISTORTION_UNCHARACTERIZED) {
            info->known = 1;
            info->p05 = row.p05_db;
            info->median = row.median_db;
        }
    }
}

static void fill_variants(Search* s) {
    uint32_t width = s->base.width;
    uint32_t height = s->base.height;

    for (uint32_t v = 0; v < TINYIMG_SELECT_MAX_VARIANTS; v++) {
        const Variant* var = &variants[v];
        VariantInfo* info = &s->variant[v];
        TinyEncodeDistortion row;
        TinyEncodePriceOpts price = {
            var->quality, var->lossless && var->format == TINYIMG_FORMAT_WEBP,
            0, s->base.channels, 0
        };

        tiny_memset(info, 0, sizeof(*info));

        // GIF ignores quality and PNG ignores it too; their rows are
        // identical at every tier, so any measured tier reads them
        uint8_t lookup = var->quality ? var->quality : 75u;
        int lossless_coding = var->lossless;

        if (var->format == TINYIMG_FORMAT_WEBP && var->lossless) {
            info->klass = TINYIMG_DISTORTION_UNCHARACTERIZED;
        }
        else {
            info->klass = tiny_distortion_encode(
                (TinyImageFormat) var->format, lookup, &row
            );
        }

        info->has_row = info->klass != TINYIMG_DISTORTION_UNCHARACTERIZED;

        if (info->has_row) {
            info->p05 = row.p05_db;
            info->median = row.median_db;
            info->bytes_per_pixel = row.bytes_per_pixel;
            info->noise_class = info->klass;
        }
        else if (lossless_coding) {
            info->noise_class = TINYIMG_DISTORTION_EXACT;
            info->p05 = 99.0f;
            info->median = 99.0f;
        }
        else {
            info->noise_class = TINYIMG_DISTORTION_UNCHARACTERIZED;
        }

        info->alpha_ok =
            (uint8_t) format_carries_alpha((TinyImageFormat) var->format);
        info->upper_us = tiny_encode_cost_upper_for(
            (TinyImageFormat) var->format, width, height, &price
        );
    }
}

/** The time bound of a lattice point, before the encoder and the scale. */
static uint32_t lattice_upper(const Search* s, uint32_t index) {
    return s->lattice[index].cost_upper;
}

static int prepare(const TinyPlan* plan, const TinySelectOpts* in, Search* s) {
    tiny_memset(s, 0, sizeof(*s));

    if (in) s->opts = *in;
    if (s->opts.objective > TINYIMG_SELECT_MAX_QUALITY ||
        s->opts.uncharacterized > TINYIMG_SELECT_ALLOW) {
        return TINYIMG_ERR_RANGE;
    }

    s->scale = s->opts.scale_milli ? s->opts.scale_milli : 1000u;
    s->peak_cap =
        s->opts.peak_cap_bytes ? s->opts.peak_cap_bytes : TINYIMG_HEAP_MAX;
    s->source = plan->source_format;
    s->formats = only_writable(
        s->opts.formats ? s->opts.formats : default_formats(s->source)
    );

    int result = tiny_plan_enumerate(
        plan, s->lattice, TINYIMG_PLAN_MAX_CANDIDATES, &s->count
    );
    if (result != TINYIMG_OK) return result;
    if (s->count > TINYIMG_PLAN_MAX_CANDIDATES) {
        s->count = TINYIMG_PLAN_MAX_CANDIDATES;
    }

    TinyPlan priced = *plan;
    priced.budget = 0u;
    priced.budget_basis = TINYIMG_BASIS_POINT;

    result = tiny_plan_resolve(&priced, &s->base);
    if (result != TINYIMG_OK) return result;

    s->alpha = !s->opts.assume_opaque &&
               (s->base.channels == 2u || s->base.channels == 4u);
    fill_lever(s);
    fill_variants(s);

    return TINYIMG_OK;
}

/**
 * Runs one candidate through the checks. `static_only` skips the peak and the
 * budget, which is how the independent policy asks "would this format do".
 */
static void evaluate(
    const Search* s, uint32_t index, uint32_t v, int static_only,
    TinySelectConsidered* out
) {
    const Variant* var = &variants[v];
    const VariantInfo* enc = &s->variant[v];
    const LeverInfo* lever = &s->lever[index];

    tiny_memset(out, 0, sizeof(*out));
    out->candidate = (uint8_t) index;
    out->format = var->format;
    out->quality = var->quality;
    out->lossless = var->lossless;
    out->peak_bytes = s->lattice[index].peak_bytes;

    out->klass = (uint8_t) tiny_select_weakest(lever->klass, enc->klass);
    out->bytes_known = enc->has_row;
    out->bytes_per_pixel = enc->bytes_per_pixel;

    // an unknown lever leaves the dB unknown; an unknown encode noise too
    out->db_known =
        lever->known && enc->noise_class != TINYIMG_DISTORTION_UNCHARACTERIZED;
    if (out->db_known) {
        out->db = tiny_select_compose_db(
            lever->klass, lever->p05, enc->noise_class, enc->p05
        );
    }

    if (!(s->formats & TINYIMG_SELECT_FORMAT(var->format))) {
        out->reason = TINYIMG_SELECT_REASON_FORMAT;
        return;
    }
    if (s->alpha && !enc->alpha_ok) {
        out->reason = TINYIMG_SELECT_REASON_ALPHA;
        return;
    }
    if (out->klass == TINYIMG_DISTORTION_UNCHARACTERIZED &&
        s->opts.uncharacterized == TINYIMG_SELECT_REFUSE) {
        out->reason = TINYIMG_SELECT_REASON_UNCHARACTERIZED;
        return;
    }
    if (!static_only && out->peak_bytes > s->peak_cap) {
        out->reason = TINYIMG_SELECT_REASON_PEAK;
        return;
    }
    if (s->opts.min_db > 0.0f && (!out->db_known || out->db < s->opts.min_db)) {
        out->reason = TINYIMG_SELECT_REASON_QUALITY;
        return;
    }

    out->cost_upper_us =
        scaled_up((uint64_t) lattice_upper(s, index) + enc->upper_us, s->scale);

    if (!static_only && s->opts.budget_us &&
        out->cost_upper_us > s->opts.budget_us) {
        out->reason = TINYIMG_SELECT_REASON_BUDGET;
    }
}

/** Whether `a` beats `b` on the objective; ties fall through to search order.
 */
static int better(
    const TinySelectConsidered* a, const TinySelectConsidered* b,
    uint32_t objective
) {
    float abytes = a->bytes_known ? a->bytes_per_pixel : 3.0e38f;
    float bbytes = b->bytes_known ? b->bytes_per_pixel : 3.0e38f;
    float adb = a->db_known ? a->db : -1.0f;
    float bdb = b->db_known ? b->db : -1.0f;

    switch (objective) {
        case TINYIMG_SELECT_MIN_TIME:
            if (a->cost_upper_us != b->cost_upper_us) {
                return a->cost_upper_us < b->cost_upper_us;
            }
            return abytes < bbytes;
        case TINYIMG_SELECT_MAX_QUALITY:
            if (adb != bdb) return adb > bdb;
            return abytes < bbytes;
        default:
            if (abytes != bbytes) return abytes < bbytes;
            return a->cost_upper_us < b->cost_upper_us;
    }
}

static void finish(
    const Search* s, uint32_t considered, int chosen, TinySelection* out
) {
    out->count = considered;
    out->lattice = s->count;
    out->width = s->base.width;
    out->height = s->base.height;
    out->channels = s->base.channels;
    out->alpha = s->alpha;
    out->lever_coverage_all_milli = LEVER_COVERAGE_ALL;

    if (chosen < 0) {
        uint32_t furthest = 0;

        for (uint32_t i = 0; i < considered; i++) {
            if (out->considered[i].reason > furthest) {
                furthest = out->considered[i].reason;
            }
        }

        out->reason = furthest;
        return;
    }

    const TinySelectConsidered* c = &out->considered[chosen];
    const LeverInfo* lever = &s->lever[c->candidate];
    uint32_t v = 0;

    for (; v < TINYIMG_SELECT_MAX_VARIANTS; v++) {
        if (variants[v].format == c->format &&
            variants[v].quality == c->quality &&
            variants[v].lossless == c->lossless) {
            break;
        }
    }

    const VariantInfo* enc = &s->variant[v];

    out->feasible = 1;
    out->chosen = (uint32_t) chosen;
    out->choice = *c;

    if (c->db_known) {
        out->db_median = tiny_select_compose_db(
            lever->klass, lever->median, enc->noise_class, enc->median
        );
    }

    if (c->bytes_known) {
        out->predicted_bytes = round_milli(
            c->bytes_per_pixel * (float) out->width * (float) out->height, 1.0f
        );
    }

    out->lever_coverage_milli =
        lever->klass == TINYIMG_DISTORTION_EXACT
            ? 1000u
            : (s->source < 8u ? lever_coverage[s->source] : 0u);
    out->encode_coverage_milli =
        enc->has_row && c->format < 8u ? encode_coverage[c->format] : 0u;

    // alpha sources were not in the opaque encode fit
    if (s->alpha && c->format != TINYIMG_FORMAT_PNG) {
        out->encode_coverage_milli = 0u;
    }
}

TINYIMG_EXPORT("tiny_plan_select")
int tiny_plan_select(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
) {
    if (!plan || !selection) return TINYIMG_ERR_NULL;

    tiny_memset(selection, 0, sizeof(*selection));

    Search s;
    int result = prepare(plan, opts, &s);
    if (result != TINYIMG_OK) return result;

    uint32_t considered = 0;
    int chosen = -1;

    for (uint32_t i = 0; i < s.count; i++) {
        for (uint32_t v = 0; v < TINYIMG_SELECT_MAX_VARIANTS; v++) {
            TinySelectConsidered* c = &selection->considered[considered];

            evaluate(&s, i, v, 0, c);

            if (c->reason == TINYIMG_SELECT_REASON_NONE &&
                (chosen < 0 ||
                 better(c, &selection->considered[chosen], s.opts.objective))) {
                chosen = (int) considered;
            }

            considered++;
        }
    }

    finish(&s, considered, chosen, selection);
    return TINYIMG_OK;
}

// #endregion

// #region fixed policies

/** The lattice index the budget ladder settles on with `encode_upper` spent. */
static int ladder_index(
    const TinyPlan* plan, const Search* s, uint32_t encode_upper,
    uint32_t* index
) {
    *index = 0;

    if (s->opts.budget_us == 0u) return TINYIMG_OK;

    uint64_t limit = (uint64_t) s->opts.budget_us * 1000u;

    if ((uint64_t) (s->base.cost_upper + (uint64_t) encode_upper) * s->scale <=
        limit) {
        return TINYIMG_OK;
    }

    TinyPlan priced = *plan;
    TinyPlanResolution resolution;
    uint64_t room = limit / s->scale;

    priced.budget = room > encode_upper ? (uint32_t) (room - encode_upper) : 1u;
    priced.budget_basis = TINYIMG_BASIS_UPPER;

    int result = tiny_plan_resolve(&priced, &resolution);
    if (result != TINYIMG_OK) return result;

    for (uint32_t i = 0; i < s->count; i++) {
        const TinyPlanCandidate* c = &s->lattice[i];

        if (c->effort == resolution.decode.effort &&
            c->scale_den == resolution.decode.scale_den &&
            c->filter_x == resolution.filter_x &&
            c->filter_y == resolution.filter_y && c->cost == resolution.cost) {
            *index = i;
            return TINYIMG_OK;
        }
    }

    return TINYIMG_ERR_PLAN;
}

static uint32_t default_variant(TinyImageFormat format) {
    for (uint32_t v = 0; v < TINYIMG_SELECT_MAX_VARIANTS; v++) {
        if (variants[v].format == format &&
            variants[v].quality == default_quality(format) &&
            !(format == TINYIMG_FORMAT_WEBP && variants[v].lossless)) {
            return v;
        }
    }

    return 0;
}

/** The format the fixed policy writes: the source's own, else the lowest. */
static uint32_t fixed_format_variant(const Search* s) {
    if (s->formats & TINYIMG_SELECT_FORMAT(s->source)) {
        return default_variant(s->source);
    }

    for (uint32_t f = TINYIMG_FORMAT_PNG; f <= TINYIMG_FORMAT_AVIF; f++) {
        if (s->formats & TINYIMG_SELECT_FORMAT(f)) {
            return default_variant((TinyImageFormat) f);
        }
    }

    return default_variant(TINYIMG_FORMAT_PNG);
}

static int run_fixed(
    const TinyPlan* plan, const TinySelectOpts* opts, int independent,
    TinySelection* selection
) {
    if (!plan || !selection) return TINYIMG_ERR_NULL;

    tiny_memset(selection, 0, sizeof(*selection));

    Search s;
    int result = prepare(plan, opts, &s);
    if (result != TINYIMG_OK) return result;

    uint32_t v = fixed_format_variant(&s);

    if (independent) {
        TinySelectConsidered probe = {0};
        int found = 0;

        for (uint32_t w = 0; w < TINYIMG_SELECT_MAX_VARIANTS; w++) {
            TinySelectConsidered c;

            evaluate(&s, 0u, w, 1, &c);
            if (c.reason != TINYIMG_SELECT_REASON_NONE) continue;

            // the byte objective, with search order breaking ties
            if (!found ||
                (c.bytes_known ? c.bytes_per_pixel : 3.0e38f) <
                    (probe.bytes_known ? probe.bytes_per_pixel : 3.0e38f)) {
                probe = c;
                v = w;
                found = 1;
            }
        }
    }

    uint32_t index = 0;
    result = ladder_index(plan, &s, s.variant[v].upper_us, &index);
    if (result != TINYIMG_OK) return result;

    TinySelectConsidered* c = &selection->considered[0];
    evaluate(&s, index, v, 0, c);

    finish(&s, 1u, c->reason == TINYIMG_SELECT_REASON_NONE ? 0 : -1, selection);
    return TINYIMG_OK;
}

int tiny_plan_select_ladder(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
) {
    return run_fixed(plan, opts, 0, selection);
}

int tiny_plan_select_independent(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
) {
    return run_fixed(plan, opts, 1, selection);
}

TINYIMG_EXPORT("tiny_plan_run_lattice")
int tiny_plan_run_lattice(
    const TinyPlan* plan, uint32_t index, TinyImage* out
) {
    if (!plan || !out) return TINYIMG_ERR_NULL;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;

    int result =
        tiny_plan_enumerate(plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count);
    if (result != TINYIMG_OK) return result;

    if (index >= count || index >= TINYIMG_PLAN_MAX_CANDIDATES) {
        return TINYIMG_ERR_RANGE;
    }

    return tiny_plan_run_candidate(plan, &lattice[index], out);
}

// #endregion

// #region accessors

TINYIMG_EXPORT("tiny_select_opts_sizeof")
uint32_t tiny_select_opts_sizeof(void) {
    return (uint32_t) sizeof(TinySelectOpts);
}

TINYIMG_EXPORT("tiny_selection_sizeof")
uint32_t tiny_selection_sizeof(void) {
    return (uint32_t) sizeof(TinySelection);
}

static uint32_t considered_field(
    const TinySelectConsidered* c, TinyConsideredField field
) {
    switch (field) {
        case TINYIMG_CONSIDERED_FIELD_CANDIDATE: return c->candidate;
        case TINYIMG_CONSIDERED_FIELD_FORMAT: return c->format;
        case TINYIMG_CONSIDERED_FIELD_QUALITY: return c->quality;
        case TINYIMG_CONSIDERED_FIELD_LOSSLESS: return c->lossless;
        case TINYIMG_CONSIDERED_FIELD_REASON: return c->reason;
        case TINYIMG_CONSIDERED_FIELD_CLASS: return c->klass;
        case TINYIMG_CONSIDERED_FIELD_DB_MILLI:
            return round_milli(c->db, 1000.0f);
        case TINYIMG_CONSIDERED_FIELD_DB_KNOWN: return c->db_known;
        case TINYIMG_CONSIDERED_FIELD_BYTES_PER_PIXEL_MICRO:
            return round_milli(c->bytes_per_pixel, 1000000.0f);
        case TINYIMG_CONSIDERED_FIELD_BYTES_KNOWN: return c->bytes_known;
        case TINYIMG_CONSIDERED_FIELD_COST_UPPER: return c->cost_upper_us;
        case TINYIMG_CONSIDERED_FIELD_PEAK: return c->peak_bytes;
        default: return 0u;
    }
}

TINYIMG_EXPORT("tiny_selection_considered_field")
uint32_t tiny_selection_considered_field(
    const TinySelection* selection, uint32_t index, TinyConsideredField field
) {
    if (!selection || index >= selection->count ||
        index >= TINYIMG_SELECT_MAX_CONSIDERED) {
        return 0u;
    }

    return considered_field(&selection->considered[index], field);
}

TINYIMG_EXPORT("tiny_selection_field")
uint32_t tiny_selection_field(
    const TinySelection* selection, TinySelectionField field
) {
    if (!selection) return 0u;

    const TinySelectConsidered* c = &selection->choice;

    switch (field) {
        case TINYIMG_SELECTION_FIELD_FEASIBLE: return selection->feasible;
        case TINYIMG_SELECTION_FIELD_REASON: return selection->reason;
        case TINYIMG_SELECTION_FIELD_CANDIDATE: return c->candidate;
        case TINYIMG_SELECTION_FIELD_FORMAT: return c->format;
        case TINYIMG_SELECTION_FIELD_QUALITY: return c->quality;
        case TINYIMG_SELECTION_FIELD_LOSSLESS: return c->lossless;
        case TINYIMG_SELECTION_FIELD_COST_UPPER: return c->cost_upper_us;
        case TINYIMG_SELECTION_FIELD_PEAK: return c->peak_bytes;
        case TINYIMG_SELECTION_FIELD_DB_MILLI:
            return round_milli(c->db, 1000.0f);
        case TINYIMG_SELECTION_FIELD_DB_MEDIAN_MILLI:
            return round_milli(selection->db_median, 1000.0f);
        case TINYIMG_SELECTION_FIELD_DB_KNOWN: return c->db_known;
        case TINYIMG_SELECTION_FIELD_CLASS: return c->klass;
        case TINYIMG_SELECTION_FIELD_BYTES_PER_PIXEL_MICRO:
            return round_milli(c->bytes_per_pixel, 1000000.0f);
        case TINYIMG_SELECTION_FIELD_BYTES_KNOWN: return c->bytes_known;
        case TINYIMG_SELECTION_FIELD_PREDICTED_BYTES:
            return selection->predicted_bytes;
        case TINYIMG_SELECTION_FIELD_CONSIDERED: return selection->count;
        case TINYIMG_SELECTION_FIELD_LATTICE: return selection->lattice;
        case TINYIMG_SELECTION_FIELD_WIDTH: return selection->width;
        case TINYIMG_SELECTION_FIELD_HEIGHT: return selection->height;
        case TINYIMG_SELECTION_FIELD_CHANNELS: return selection->channels;
        case TINYIMG_SELECTION_FIELD_ALPHA: return selection->alpha;
        case TINYIMG_SELECTION_FIELD_LEVER_COVERAGE:
            return selection->lever_coverage_milli;
        case TINYIMG_SELECTION_FIELD_LEVER_COVERAGE_ALL:
            return selection->lever_coverage_all_milli;
        case TINYIMG_SELECTION_FIELD_ENCODE_COVERAGE:
            return selection->encode_coverage_milli;
        case TINYIMG_SELECTION_FIELD_CHOSEN: return selection->chosen;
        default: return 0u;
    }
}

// #endregion
