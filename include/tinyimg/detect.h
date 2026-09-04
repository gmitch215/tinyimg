/**
 * @file detect.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Face detection through a local binary pattern cascade.
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
#include "tinyimg/memory.h"
#include "tinyimg/tinyimg.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region detection

/**
 * @brief One detection, in the coordinates of the image that was searched.
 */
typedef struct {
    /** Left edge. */
    uint32_t x;
    /** Top edge. */
    uint32_t y;
    /** Width. */
    uint32_t width;
    /** Height. */
    uint32_t height;
    /**
     * @brief Overlapping raw detections this box was formed from.
     *
     * A face a cascade is confident about fires at several nearby positions
     * and scales, so this is a usable ranking: the highest is the most likely
     * to be a face, and a box that only just cleared `min_neighbors` is the
     * one to be suspicious of.
     */
    uint32_t neighbors;
} TinyFaceBox;

/**
 * @brief How hard to look.
 *
 * A zeroed structure is valid and means the defaults; tiny_detect_opts fills
 * one in with them explicitly.
 */
typedef struct {
    /**
     * @brief Shortest face to look for, as a box height in pixels.
     *
     * The cost knob, and the only one that matters. The search reduces the
     * image until a face of this height fills the cascade's window, so the
     * largest image the detector actually scans is the input scaled by
     * `window height / min_size`. Halving this quadruples the work.
     *
     * Zero means the cascade's own window, which on a full resolution
     * photograph is a search over millions of positions. Statistics in this
     * library run on a reduced image for the same reason; see
     * tiny_image_detect_faces.
     */
    uint32_t min_size;
    /** Tallest face to look for, as a box height in pixels. Zero means the
     * image. */
    uint32_t max_size;
    /**
     * @brief Ratio between one search scale and the next. Zero reads as 1.1.
     *
     * Smaller finds more and costs more. Below 1.05 the levels overlap so far
     * that the extra detections are the same faces again.
     */
    float scale_factor;
    /**
     * @brief Overlapping raw detections a box needs to survive. Zero reads as
     * 3.
     *
     * The false positive control. One is almost no filtering; a real face
     * usually gathers ten or more.
     */
    uint32_t min_neighbors;
} TinyDetectOpts;

/**
 * @brief Raw detections collected before grouping.
 *
 * A search over a large image at a small `min_size` can fire more than this,
 * and the ones past it are dropped rather than growing a buffer inside a hot
 * loop. Reaching it means the search was far wider than the picture needs.
 */
#define TINYIMG_MAX_RAW_DETECTIONS 2048u

/**
 * @brief Longest side tiny_image_detect_faces searches at.
 *
 * A cascade needs the face to fill its window and to still carry texture, so
 * reducing further than this loses faces rather than time. Measured: the
 * frontal cascade finds `smile.jpg` at 800 pixels and at 1200 and misses it at
 * 480, and 800 also produced a false positive on `dog.jpg` that 1200 does not.
 */
#define TINYIMG_DETECT_LONG_SIDE 1200u

/**
 * @brief What tiny_image_detect_faces divides the height by for its `min_size`.
 *
 * A tenth of the height is about the smallest face worth finding in a
 * photograph, and it is also where the cost lands somewhere a request can
 * afford. An eighth misses the face in `smile.jpg`, which is 7% of its height;
 * a sixteenth finds two boxes on it and triples the time.
 */
#define TINYIMG_DETECT_SIZE_DIVISOR 10u

/**
 * @brief Fills an options structure in with the defaults.
 *
 * `min_size` comes back zero, which means search every scale. That is the
 * honest default for the explicit entry point and it is expensive; see
 * tiny_image_detect_faces for the one that picks a size from the image.
 *
 * @param opts Receives them.
 */
void tiny_detect_opts(TinyDetectOpts* opts);

/**
 * @brief Finds faces, at a resolution and a minimum size chosen from the image.
 *
 * The entry point to reach for. It searches a copy reduced to
 * TINYIMG_DETECT_LONG_SIDE with `min_size` set to a
 * TINYIMG_DETECT_SIZE_DIVISOR of the height, then scales the boxes back, which
 * on a 1470x1920 photograph is 155 ms against 993 for searching every scale of
 * the original. The boxes are in the coordinates of the image passed in.
 *
 * Both constants are measurements rather than preferences, and the reason the
 * reduction is moderate is that a cascade stops working before it stops being
 * slow: statistics elsewhere in this library run happily on a one-eighth
 * decode, and face detection does not, because a face at 7% of an image's
 * height is 17 pixels there against a 45 pixel window.
 *
 * **A cascade is scale-dependent, not just size-dependent**, so no single
 * setting finds every face. A frontal face is found at a moderate reduction and
 * lost at full resolution; a side-facing one can be the other way round. This
 * default is tuned for the first, which is the common case, and searching at
 * another scale is what tiny_image_detect_faces_ex is for. The behavior is the
 * cascade's rather than this implementation's: the same inputs through OpenCV's
 * own detector move the same way.
 *
 * @param image The image to search. Any channel count; the luminance is taken.
 * @param boxes Receives the detections, ordered by `neighbors` descending.
 * @param capacity How many `boxes` holds.
 * @param count Receives how many were written, which is capped at `capacity`.
 * @return int TINYIMG_OK, TINYIMG_ERR_BLOB_MISSING when no cascade is
 * resident, TINYIMG_ERR_CORRUPT for a cascade that does not parse, or
 * TINYIMG_ERR_MEMORY.
 */
int tiny_image_detect_faces(
    const TinyImage* image, TinyFaceBox* boxes, uint32_t capacity,
    uint32_t* count
);

/**
 * @brief Finds faces exactly as asked, on the pixels given.
 *
 * No reduction and no chosen size: it searches the image passed in with the
 * options passed in, and the boxes are in that image's coordinates. Use it to
 * search a region, to search at a size the default would skip, or to search a
 * reduction the caller made itself.
 *
 * Runs every blob of kind TINYIMG_BLOB_CASCADE and groups the results
 * together, so loading a frontal and a profile cascade finds both kinds of face
 * and a face that fires both is one box rather than two. Nothing here names a
 * cascade; a caller loads the ones they have.
 *
 * @param image The image to search.
 * @param opts How hard to look, or NULL for tiny_detect_opts' defaults, which
 * search every scale.
 * @param boxes Receives the detections, ordered by `neighbors` descending.
 * @param capacity How many `boxes` holds.
 * @param count Receives how many were written.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_detect_faces_ex(
    const TinyImage* image, const TinyDetectOpts* opts, TinyFaceBox* boxes,
    uint32_t capacity, uint32_t* count
);

/**
 * @brief Reports whether a cascade blob parses, without searching anything.
 *
 * What a host calls after loading one, so a bad blob is a startup failure
 * rather than a detection that silently finds nothing.
 *
 * @param blob_id The id it was loaded under, or NULL for the first cascade.
 * @return int TINYIMG_OK, TINYIMG_ERR_BLOB_MISSING or TINYIMG_ERR_CORRUPT.
 */
int tiny_cascade_check(const char* blob_id);

/**
 * @brief Size of a TinyFaceBox, for a host reading an array of them out of
 * linear memory.
 *
 * @return uint32_t sizeof(TinyFaceBox).
 */
uint32_t tiny_face_box_sizeof(void);

/**
 * @brief Size of a TinyDetectOpts, for the same reason.
 *
 * @return uint32_t sizeof(TinyDetectOpts).
 */
uint32_t tiny_detect_opts_sizeof(void);

#pragma endregion

#ifdef __cplusplus
}
#endif
