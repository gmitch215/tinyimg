#include "tinyimg/detect.h"

#include "tinyimg/memory.h"
#include "tinyimg/util.h"

#pragma region cascade

/**
 * @brief A packed cascade, as a set of views over the blob's bytes.
 *
 * The blob is the OpenCV XML repacked by `scripts/fixtures.ts`, which is where
 * the parsing actually happens. An XML reader in the module would cost more
 * bytes than the cascade does, and the repacked form is what the plan's blob
 * seam exists for.
 *
 * Every classifier in both shipped cascades is a stump: one feature, one
 * subset, two leaves. The repacker refuses a cascade that is not, so nothing
 * here walks a tree.
 */
typedef struct {
    uint32_t window_width;
    uint32_t window_height;
    uint32_t stages;
    uint32_t stumps;
    uint32_t features;

    const uint8_t* stage_threshold;
    const uint8_t* stage_first;
    const uint8_t* stage_count;
    const uint8_t* stump_feature;
    const uint8_t* stump_subset;
    const uint8_t* stump_leaf;
    const uint8_t* feature_rect;
} Cascade;

/** The magic the repacker writes, so a raw XML blob fails loudly. */
#define CASCADE_MAGIC 0x54494341u

/** Bytes the fixed header occupies. */
#define CASCADE_HEADER 28u

/**
 * @brief What OpenCV subtracts from every stage threshold when it loads one.
 *
 * A stage sum that lands exactly on its threshold passes rather than fails.
 * Applied here rather than baked into the blob, so the blob holds the numbers
 * the cascade's author wrote.
 */
#define CASCADE_EPSILON 1.0e-5f

static uint32_t read_le32(const uint8_t* at) {
    return (uint32_t) at[0] | ((uint32_t) at[1] << 8) |
           ((uint32_t) at[2] << 16) | ((uint32_t) at[3] << 24);
}

static float read_f32(const uint8_t* at) {
    uint32_t bits = read_le32(at);
    float out;

    tiny_memcpy(&out, &bits, sizeof(out));
    return out;
}

/**
 * @brief Reads the header and points the views at the arrays behind it.
 *
 * Every extent is checked against the blob's own length, so a truncated or
 * mismatched cascade is rejected here and never during a search.
 */
static int cascade_parse(Cascade* out, const uint8_t* data, size_t size) {
    if (!data || size < CASCADE_HEADER) return TINYIMG_ERR_CORRUPT;
    if (read_le32(data) != CASCADE_MAGIC) return TINYIMG_ERR_CORRUPT;
    if (read_le32(data + 4u) != 1u) return TINYIMG_ERR_UNSUPPORTED_VARIANT;

    out->window_width = read_le32(data + 8u);
    out->window_height = read_le32(data + 12u);
    out->stages = read_le32(data + 16u);
    out->stumps = read_le32(data + 20u);
    out->features = read_le32(data + 24u);

    if (out->window_width == 0u || out->window_height == 0u) {
        return TINYIMG_ERR_CORRUPT;
    }
    if (out->stages == 0u || out->stumps == 0u || out->features == 0u) {
        return TINYIMG_ERR_CORRUPT;
    }

    size_t at = CASCADE_HEADER;
    size_t stage_bytes = (size_t) out->stages * 4u;
    size_t stump_bytes = (size_t) out->stumps * 4u;

    size_t need = stage_bytes * 3u + stump_bytes + stump_bytes * 8u +
                  stump_bytes * 2u + (size_t) out->features * 4u;

    if (size - at < need) return TINYIMG_ERR_CORRUPT;

    out->stage_threshold = data + at;
    at += stage_bytes;
    out->stage_first = data + at;
    at += stage_bytes;
    out->stage_count = data + at;
    at += stage_bytes;
    out->stump_feature = data + at;
    at += stump_bytes;
    out->stump_subset = data + at;
    at += stump_bytes * 8u;
    out->stump_leaf = data + at;
    at += stump_bytes * 2u;
    out->feature_rect = data + at;

    for (uint32_t i = 0; i < out->stages; i++) {
        uint32_t first = read_le32(out->stage_first + 4u * i);
        uint32_t count = read_le32(out->stage_count + 4u * i);

        if (count == 0u || first > out->stumps || count > out->stumps - first) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    for (uint32_t i = 0; i < out->stumps; i++) {
        if (read_le32(out->stump_feature + 4u * i) >= out->features) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    for (uint32_t i = 0; i < out->features; i++) {
        const uint8_t* rect = out->feature_rect + 4u * i;

        // the feature covers three blocks in each axis, so a rect that fits the
        // window is one whose triple fits it
        if (rect[2] == 0u || rect[3] == 0u) return TINYIMG_ERR_CORRUPT;
        if ((uint32_t) rect[0] + 3u * rect[2] > out->window_width) {
            return TINYIMG_ERR_CORRUPT;
        }
        if ((uint32_t) rect[1] + 3u * rect[3] > out->window_height) {
            return TINYIMG_ERR_CORRUPT;
        }
    }

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_cascade_check")
int tiny_cascade_check(const char* blob_id) {
    size_t size = 0;
    const uint8_t* data = tiny_blob_get(TINYIMG_BLOB_CASCADE, blob_id, &size);

    if (!data) return TINYIMG_ERR_BLOB_MISSING;

    Cascade cascade;
    return cascade_parse(&cascade, data, size);
}

#pragma endregion

#pragma region integral image

/**
 * @brief A summed area table with a zero row and column.
 *
 * `at(x, y)` is the sum of every pixel above and left of (x, y), so the sum
 * over a rectangle is four reads regardless of its size. That is the whole
 * reason a cascade is affordable: one feature reads nine block sums, and a
 * search evaluates millions of features over overlapping blocks.
 *
 * `uint32_t` holds it: the largest possible sum is 255 * TINYIMG_MAX_PIXELS,
 * which is 4.08 billion against a ceiling of 4.29.
 */
typedef struct {
    uint32_t* sum;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} Integral;

static void integral_build(
    Integral* out, const uint8_t* gray, uint32_t width, uint32_t height
) {
    out->width = width;
    out->height = height;
    out->stride = width + 1u;

    for (uint32_t x = 0; x <= width; x++) out->sum[x] = 0u;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t* row = out->sum + (size_t) (y + 1u) * out->stride;
        const uint32_t* above = out->sum + (size_t) y * out->stride;
        uint32_t running = 0u;

        row[0] = 0u;

        for (uint32_t x = 0; x < width; x++) {
            running += gray[(size_t) y * width + x];
            row[x + 1u] = above[x + 1u] + running;
        }
    }
}

static uint32_t integral_at(const Integral* integral, uint32_t x, uint32_t y) {
    return integral->sum[(size_t) y * integral->stride + x];
}

/**
 * @brief Takes an image's luminance into one byte per pixel.
 *
 * The same integer weights the rest of the library uses. A cascade is trained
 * on one channel, and which channel it is has to be the same everywhere or the
 * texture statistics it learned do not describe what it is shown.
 *
 * @param out `width * height` bytes.
 * @param image The image, any channel count.
 */
static void luminance(uint8_t* out, const TinyImage* image) {
    size_t pixels = (size_t) image->width * image->height;
    uint8_t channels = image->channels;
    uint8_t colors = channels == 4u ? 3u : channels == 2u ? 1u : channels;

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* pixel = image->data + i * channels;

        out[i] = colors == 1u ? pixel[0]
                              : (uint8_t) ((77u * pixel[0] + 150u * pixel[1] +
                                            29u * pixel[2]) >>
                                           8);
    }
}

/**
 * @brief Rounds half to even, which is what the cascade's own search rounds
 * with.
 *
 * Its scale arithmetic goes through `lrint` under the default rounding mode.
 * Rounding half away from zero instead moves a level's extent by a pixel where
 * the division lands on a half, and every sample position in that level with
 * it.
 */
static uint32_t round_even(float value) {
    if (value <= 0.0f) return 0u;

    float whole = tiny_floorf(value);
    float fraction = value - whole;
    uint32_t below = (uint32_t) whole;

    if (fraction > 0.5f) return below + 1u;
    if (fraction < 0.5f) return below;

    return (below & 1u) ? below + 1u : below;
}

/**
 * @brief Bilinear resample of `src` into `dst`.
 *
 * Bilinear rather than an area average, which is what this library's own
 * resampler would use for a reduction and is the better answer for a picture. A
 * cascade is not a picture: it was trained on bilinear reductions, and its
 * features go down to a single pixel per block, so the filter is part of what
 * the classifier learned rather than a quality choice.
 */
static void reduce_gray(
    const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw,
    uint32_t dh
) {
    float fx = (float) sw / (float) dw;
    float fy = (float) sh / (float) dh;

    for (uint32_t y = 0; y < dh; y++) {
        float sy = ((float) y + 0.5f) * fy - 0.5f;
        if (sy < 0.0f) sy = 0.0f;

        uint32_t y0 = (uint32_t) sy;
        if (y0 > sh - 1u) y0 = sh - 1u;
        uint32_t y1 = y0 + 1u < sh ? y0 + 1u : y0;
        float wy = sy - (float) y0;

        const uint8_t* top = src + (size_t) y0 * sw;
        const uint8_t* bottom = src + (size_t) y1 * sw;
        uint8_t* out = dst + (size_t) y * dw;

        for (uint32_t x = 0; x < dw; x++) {
            float sx = ((float) x + 0.5f) * fx - 0.5f;
            if (sx < 0.0f) sx = 0.0f;

            uint32_t x0 = (uint32_t) sx;
            if (x0 > sw - 1u) x0 = sw - 1u;
            uint32_t x1 = x0 + 1u < sw ? x0 + 1u : x0;
            float wx = sx - (float) x0;

            float a = (float) top[x0];
            float b = (float) top[x1];
            float c = (float) bottom[x0];
            float d = (float) bottom[x1];

            float above = a + (b - a) * wx;
            float below = c + (d - c) * wx;

            out[x] = tiny_clamp_u8f(above + (below - above) * wy);
        }
    }
}

#pragma endregion

#pragma region evaluation

/**
 * @brief The local binary pattern code of one feature at one window position.
 *
 * The center block against its eight neighbors, one bit each, clockwise from
 * the top left. The bit order is the trained cascade's own: a code assembled in
 * a different order indexes the wrong bit of the subset, which passes every
 * self-consistent test and detects nothing.
 */
static uint32_t lbp_code(
    const Integral* integral, const uint8_t* rect, uint32_t window_x,
    uint32_t window_y
) {
    uint32_t xs[4];
    uint32_t ys[4];

    for (uint32_t i = 0; i < 4u; i++) {
        xs[i] = window_x + rect[0] + i * rect[2];
        ys[i] = window_y + rect[1] + i * rect[3];
    }

    uint32_t cells[3][3];

    for (uint32_t i = 0; i < 3u; i++) {
        for (uint32_t j = 0; j < 3u; j++) {
            cells[i][j] = integral_at(integral, xs[j + 1u], ys[i + 1u]) -
                          integral_at(integral, xs[j], ys[i + 1u]) -
                          integral_at(integral, xs[j + 1u], ys[i]) +
                          integral_at(integral, xs[j], ys[i]);
        }
    }

    uint32_t center = cells[1][1];
    uint32_t code = 0u;

    if (cells[0][0] >= center) code |= 128u;
    if (cells[0][1] >= center) code |= 64u;
    if (cells[0][2] >= center) code |= 32u;
    if (cells[1][2] >= center) code |= 16u;
    if (cells[2][2] >= center) code |= 8u;
    if (cells[2][1] >= center) code |= 4u;
    if (cells[2][0] >= center) code |= 2u;
    if (cells[1][0] >= center) code |= 1u;

    return code;
}

/**
 * @brief Runs the whole cascade at one window position.
 *
 * Rejects at the first stage whose sum falls short, which is what makes a
 * cascade cheap: almost every position in a photograph fails stage one, and the
 * nineteen stages are only ever all evaluated on the few that look like faces.
 *
 * @return int Non-zero when every stage passed.
 */
static int cascade_at(
    const Cascade* cascade, const Integral* integral, uint32_t window_x,
    uint32_t window_y
) {
    for (uint32_t s = 0; s < cascade->stages; s++) {
        uint32_t first = read_le32(cascade->stage_first + 4u * s);
        uint32_t count = read_le32(cascade->stage_count + 4u * s);
        float threshold =
            read_f32(cascade->stage_threshold + 4u * s) - CASCADE_EPSILON;
        float sum = 0.0f;

        for (uint32_t i = first; i < first + count; i++) {
            uint32_t feature = read_le32(cascade->stump_feature + 4u * i);
            uint32_t code = lbp_code(
                integral, cascade->feature_rect + 4u * feature, window_x,
                window_y
            );

            const uint8_t* subset = cascade->stump_subset + 32u * i;
            uint32_t word = read_le32(subset + 4u * (code >> 5));
            uint32_t set = (word >> (code & 31u)) & 1u;

            sum += read_f32(cascade->stump_leaf + 8u * i + (set ? 0u : 4u));
        }

        if (sum < threshold) return 0;
    }

    return 1;
}

#pragma endregion

#pragma region grouping

/** Find with path halving, over the raw detections. */
static uint32_t find_root(uint32_t* parent, uint32_t node) {
    while (parent[node] != node) {
        parent[node] = parent[parent[node]];
        node = parent[node];
    }

    return node;
}

/** How far apart two detections may be and still be one face. */
#define GROUP_EPSILON 0.2f

static int32_t difference(uint32_t a, uint32_t b) {
    return a > b ? (int32_t) (a - b) : -(int32_t) (b - a);
}

/**
 * @brief Whether two detections are the same face.
 *
 * All four **edges** within a fifth of the smaller box, which is the cascade's
 * own grouping predicate. Comparing the widths instead of the right edges, or
 * scaling the tolerance by the average of both boxes rather than the smaller
 * one, are both nearly the same test and neither is this one; each splits one
 * face into two groups often enough to change what a detection run reports.
 */
static int boxes_alike(const TinyFaceBox* a, const TinyFaceBox* b) {
    uint32_t narrow = a->width < b->width ? a->width : b->width;
    uint32_t shallow = a->height < b->height ? a->height : b->height;

    float delta = GROUP_EPSILON * ((float) narrow + (float) shallow) * 0.5f;

    int32_t dx = difference(a->x, b->x);
    int32_t dy = difference(a->y, b->y);
    int32_t dr = difference(a->x + a->width, b->x + b->width);
    int32_t db = difference(a->y + a->height, b->y + b->height);

    return tiny_fabsf((float) dx) <= delta && tiny_fabsf((float) dy) <= delta &&
           tiny_fabsf((float) dr) <= delta && tiny_fabsf((float) db) <= delta;
}

/**
 * @brief Whether `inner` sits inside `outer` and should give way to it.
 *
 * A face fires the cascade at its own extent and again at a sub-region of
 * itself, and the sub-region groups separately. Without this a portrait comes
 * back as a face and a smaller box on the mouth.
 */
static int box_swallowed(const TinyFaceBox* inner, const TinyFaceBox* outer) {
    int32_t dx = (int32_t) (GROUP_EPSILON * (float) outer->width);
    int32_t dy = (int32_t) (GROUP_EPSILON * (float) outer->height);

    if ((int32_t) inner->x < (int32_t) outer->x - dx) return 0;
    if ((int32_t) inner->y < (int32_t) outer->y - dy) return 0;
    if ((int32_t) (inner->x + inner->width) >
        (int32_t) (outer->x + outer->width) + dx) {
        return 0;
    }
    if ((int32_t) (inner->y + inner->height) >
        (int32_t) (outer->y + outer->height) + dy) {
        return 0;
    }

    uint32_t floor_count = inner->neighbors > 3u ? inner->neighbors : 3u;

    return outer->neighbors > floor_count || inner->neighbors < 3u;
}

/**
 * @brief Merges overlapping raw detections and drops the thin groups.
 *
 * Union-find over the alike relation rather than a greedy first-match pass,
 * because the relation is not transitive: three detections where the first and
 * third are too far apart to be alike but both match the second are one face,
 * and a greedy pass reports two.
 *
 * The surviving boxes are the average of their group, sorted by how many
 * detections formed them.
 *
 * `threshold` is exclusive, so the default of three keeps a group of four. That
 * is the cascade's own convention and being one out either way moves the false
 * positive rate a long way, because the group sizes cluster at the bottom.
 *
 * @param raw The detections.
 * @param count How many.
 * @param threshold Detections a group must exceed to survive.
 * @param out Receives the groups.
 * @param capacity How many `out` holds.
 * @return uint32_t How many groups were written.
 */
static uint32_t group_boxes(
    const TinyFaceBox* raw, uint32_t count, uint32_t threshold,
    TinyFaceBox* out, uint32_t capacity
) {
    if (count == 0u || capacity == 0u) return 0u;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint32_t* parent =
        (uint32_t*) tiny_arena_alloc((size_t) count * sizeof(uint32_t), 4);
    TinyFaceBox* groups = (TinyFaceBox*) tiny_arena_alloc(
        (size_t) count * sizeof(TinyFaceBox), 4
    );

    if (!parent || !groups) {
        tiny_arena_release(&mark);
        return 0u;
    }

    for (uint32_t i = 0; i < count; i++) parent[i] = i;

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1u; j < count; j++) {
            if (!boxes_alike(&raw[i], &raw[j])) continue;

            uint32_t a = find_root(parent, i);
            uint32_t b = find_root(parent, j);

            if (a != b) parent[a] = b;
        }
    }

    uint32_t found = 0u;

    for (uint32_t i = 0; i < count; i++) {
        if (find_root(parent, i) != i) continue;

        uint64_t x = 0;
        uint64_t y = 0;
        uint64_t width = 0;
        uint64_t height = 0;
        uint32_t members = 0u;

        for (uint32_t j = 0; j < count; j++) {
            if (find_root(parent, j) != i) continue;

            x += raw[j].x;
            y += raw[j].y;
            width += raw[j].width;
            height += raw[j].height;
            members++;
        }

        groups[found].x = (uint32_t) (x / members);
        groups[found].y = (uint32_t) (y / members);
        groups[found].width = (uint32_t) (width / members);
        groups[found].height = (uint32_t) (height / members);
        groups[found].neighbors = members;
        found++;
    }

    uint32_t written = 0u;

    for (uint32_t i = 0; i < found; i++) {
        if (groups[i].neighbors <= threshold) continue;

        uint32_t swallowed = 0u;

        for (uint32_t j = 0; j < found; j++) {
            if (i == j || groups[j].neighbors <= threshold) continue;
            if (!box_swallowed(&groups[i], &groups[j])) continue;

            swallowed = 1u;
            break;
        }

        if (swallowed) continue;
        if (written >= capacity) break;

        out[written++] = groups[i];
    }

    tiny_arena_release(&mark);

    for (uint32_t i = 1; i < written; i++) {
        TinyFaceBox hold = out[i];
        uint32_t j = i;

        while (j > 0u && out[j - 1u].neighbors < hold.neighbors) {
            out[j] = out[j - 1u];
            j--;
        }

        out[j] = hold;
    }

    return written;
}

#pragma endregion

#pragma region searching

/** Scratch one search reuses across every cascade and every scale. */
typedef struct {
    uint8_t* gray;
    uint8_t* level;
    Integral integral;
    TinyFaceBox* raw;
    uint32_t raw_count;
} Search;

/**
 * @brief Slides one cascade's window over one pyramid level.
 *
 * The window stays the size the cascade was trained at and the image is
 * reduced, which is what OpenCV does and is the reason a trained classifier
 * sees the texture statistics it was trained on. Scaling the feature rectangles
 * against one full size integral image is cheaper and rounds every block
 * boundary, which costs detections at exactly the small scales that need them.
 */
static void search_level(
    Search* search, const Cascade* cascade, uint32_t width, uint32_t height,
    float factor, uint32_t step
) {
    if (width < cascade->window_width || height < cascade->window_height) {
        return;
    }

    uint32_t last_x = width - cascade->window_width;
    uint32_t last_y = height - cascade->window_height;

    uint32_t box_width = round_even((float) cascade->window_width * factor);
    uint32_t box_height = round_even((float) cascade->window_height * factor);

    for (uint32_t y = 0; y <= last_y; y += step) {
        for (uint32_t x = 0; x <= last_x; x += step) {
            if (!cascade_at(cascade, &search->integral, x, y)) continue;
            if (search->raw_count >= TINYIMG_MAX_RAW_DETECTIONS) return;

            TinyFaceBox* box = &search->raw[search->raw_count++];

            // back into the coordinates of the image the caller passed, which
            // is the only frame a caller can use a box in
            box->x = round_even((float) x * factor);
            box->y = round_even((float) y * factor);
            box->width = box_width;
            box->height = box_height;
            box->neighbors = 1u;
        }
    }
}

/**
 * @brief Runs one cascade over the whole pyramid.
 *
 * The scales are the powers of `scale_factor` from one upward, and `min_size`
 * drops the leading ones rather than shifting where the sequence starts. That
 * is not a detail: a cascade fires on a face at a handful of nearby scales, and
 * a grid offset from the one the boxes were grouped against changes which
 * detections land close enough to each other to count as one face. It also
 * keeps the cost knob, because a dropped scale is a level that is never built.
 *
 * The window steps two pixels while the level is more than half size and one
 * pixel below that, which is the same trade the cascade's own search makes: the
 * large levels are where almost all the positions are, and a face at that scale
 * spans enough pixels to survive being sampled every other one.
 */
static void search_cascade(
    Search* search, const Cascade* cascade, uint32_t width, uint32_t height,
    const TinyDetectOpts* opts
) {
    float step = opts->scale_factor > 1.001f ? opts->scale_factor : 1.1f;
    uint32_t shortest = opts->min_size;
    uint32_t tallest = opts->max_size > 0u ? opts->max_size : height;

    for (float factor = 1.0f;; factor *= step) {
        uint32_t box_width = round_even((float) cascade->window_width * factor);
        uint32_t box_height =
            round_even((float) cascade->window_height * factor);

        if (box_width > width || box_height > height) return;
        if (box_height > tallest) return;
        if (box_height < shortest) continue;

        uint32_t level_width = round_even((float) width / factor);
        uint32_t level_height = round_even((float) height / factor);

        if (level_width < cascade->window_width ||
            level_height < cascade->window_height) {
            return;
        }

        if (level_width == width && level_height == height) {
            tiny_memcpy(search->level, search->gray, (size_t) width * height);
        }
        else {
            reduce_gray(
                search->gray, width, height, search->level, level_width,
                level_height
            );
        }

        integral_build(
            &search->integral, search->level, level_width, level_height
        );

        search_level(
            search, cascade, level_width, level_height, factor,
            factor >= 2.0f ? 1u : 2u
        );

        if (search->raw_count >= TINYIMG_MAX_RAW_DETECTIONS) return;
    }
}

TINYIMG_EXPORT("tiny_detect_opts")
void tiny_detect_opts(TinyDetectOpts* opts) {
    if (!opts) return;

    opts->min_size = 0u;
    opts->max_size = 0u;
    opts->scale_factor = 1.1f;
    opts->min_neighbors = 3u;
}

TINYIMG_EXPORT("tiny_image_detect_faces_ex")
int tiny_image_detect_faces_ex(
    const TinyImage* image, const TinyDetectOpts* opts, TinyFaceBox* boxes,
    uint32_t capacity, uint32_t* count
) {
    if (!image || !image->data || !boxes || !count) return TINYIMG_ERR_NULL;

    *count = 0u;

    if (!tiny_blob_at(TINYIMG_BLOB_CASCADE, 0u, 0, 0)) {
        return TINYIMG_ERR_BLOB_MISSING;
    }

    TinyDetectOpts resolved;
    tiny_detect_opts(&resolved);
    if (opts) resolved = *opts;

    uint32_t threshold =
        resolved.min_neighbors > 0u ? resolved.min_neighbors : 3u;

    uint32_t width = image->width;
    uint32_t height = image->height;
    size_t pixels = (size_t) width * height;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    Search search;
    search.raw_count = 0u;
    search.gray = (uint8_t*) tiny_arena_alloc(pixels, 16);
    search.level = (uint8_t*) tiny_arena_alloc(pixels, 16);
    search.integral.sum = (uint32_t*) tiny_arena_alloc(
        ((size_t) width + 1u) * ((size_t) height + 1u) * sizeof(uint32_t), 4
    );
    search.raw = (TinyFaceBox*) tiny_arena_alloc(
        (size_t) TINYIMG_MAX_RAW_DETECTIONS * sizeof(TinyFaceBox), 4
    );

    if (!search.gray || !search.level || !search.integral.sum || !search.raw) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    luminance(search.gray, image);

    int result = TINYIMG_OK;

    for (uint32_t i = 0;; i++) {
        size_t size = 0;
        const uint8_t* data = tiny_blob_at(TINYIMG_BLOB_CASCADE, i, 0, &size);

        if (!data) break;

        Cascade cascade;
        result = cascade_parse(&cascade, data, size);
        if (result != TINYIMG_OK) break;

        search_cascade(&search, &cascade, width, height, &resolved);
    }

    if (result == TINYIMG_OK) {
        *count = group_boxes(
            search.raw, search.raw_count, threshold, boxes, capacity
        );
    }

    tiny_arena_release(&mark);
    return result;
}

TINYIMG_EXPORT("tiny_image_detect_faces")
int tiny_image_detect_faces(
    const TinyImage* image, TinyFaceBox* boxes, uint32_t capacity,
    uint32_t* count
) {
    if (!image || !image->data || !boxes || !count) return TINYIMG_ERR_NULL;

    uint32_t longest =
        image->width > image->height ? image->width : image->height;

    if (longest <= TINYIMG_DETECT_LONG_SIDE) {
        TinyDetectOpts opts;
        tiny_detect_opts(&opts);
        opts.min_size = image->height / TINYIMG_DETECT_SIZE_DIVISOR;

        return tiny_image_detect_faces_ex(image, &opts, boxes, capacity, count);
    }

    *count = 0u;

    uint32_t width = image->width * TINYIMG_DETECT_LONG_SIDE / longest;
    uint32_t height = image->height * TINYIMG_DETECT_LONG_SIDE / longest;

    if (width == 0u || height == 0u) return TINYIMG_ERR_RANGE;

    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* gray =
        (uint8_t*) tiny_arena_alloc((size_t) image->width * image->height, 16);
    uint8_t* small = (uint8_t*) tiny_arena_alloc((size_t) width * height, 16);

    if (!gray || !small) {
        tiny_arena_release(&mark);
        return TINYIMG_ERR_MEMORY;
    }

    luminance(gray, image);
    reduce_gray(gray, image->width, image->height, small, width, height);

    TinyImage reduced;
    tiny_memset(&reduced, 0, sizeof(reduced));
    reduced.width = width;
    reduced.height = height;
    reduced.channels = 1u;
    reduced.data = small;

    TinyDetectOpts opts;
    tiny_detect_opts(&opts);
    opts.min_size = height / TINYIMG_DETECT_SIZE_DIVISOR;

    int result =
        tiny_image_detect_faces_ex(&reduced, &opts, boxes, capacity, count);

    if (result == TINYIMG_OK) {
        for (uint32_t i = 0; i < *count; i++) {
            boxes[i].x = boxes[i].x * longest / TINYIMG_DETECT_LONG_SIDE;
            boxes[i].y = boxes[i].y * longest / TINYIMG_DETECT_LONG_SIDE;
            boxes[i].width =
                boxes[i].width * longest / TINYIMG_DETECT_LONG_SIDE;
            boxes[i].height =
                boxes[i].height * longest / TINYIMG_DETECT_LONG_SIDE;
        }
    }

    tiny_arena_release(&mark);
    return result;
}

TINYIMG_EXPORT("tiny_face_box_sizeof")
uint32_t tiny_face_box_sizeof(void) {
    return (uint32_t) sizeof(TinyFaceBox);
}

TINYIMG_EXPORT("tiny_detect_opts_sizeof")
uint32_t tiny_detect_opts_sizeof(void) {
    return (uint32_t) sizeof(TinyDetectOpts);
}

#pragma endregion
