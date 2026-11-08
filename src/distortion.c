#include "tinyimg/distortion.h"

#include "tinyimg/memory.h"

typedef struct {
    uint8_t format;
    uint8_t effort_delta;
    uint8_t filter_delta;
    uint8_t scale_steps;
    uint8_t klass;
    uint8_t max_abs;
    uint16_t n;
    float p05;
    float median;
} LeverRow;

typedef struct {
    uint8_t format;
    uint8_t quality;
    uint8_t klass;
    uint16_t n;
    float bpp_median;
    float bpp_p95;
    float p05;
    float median;
} EncodeRow;

#include "distortion-table.h"

TinyDistortionClass tiny_distortion_lever(
    TinyImageFormat format, uint8_t effort_delta, uint8_t filter_delta,
    uint8_t scale_steps, TinyDistortion* out
) {
    if (out) tiny_memset(out, 0, sizeof(*out));

    for (uint32_t i = 0; i < TINYIMG_DISTORTION_LEVER_ROWS; i++) {
        const LeverRow* r = &lever_rows[i];

        if (r->format != (uint8_t) format || r->effort_delta != effort_delta ||
            r->filter_delta != filter_delta || r->scale_steps != scale_steps) {
            continue;
        }

        if (out) {
            out->klass = (TinyDistortionClass) r->klass;
            out->p05_db = r->p05;
            out->median_db = r->median;
            out->n = r->n;
            out->max_abs = r->max_abs;
        }
        return (TinyDistortionClass) r->klass;
    }

    return TINYIMG_DISTORTION_UNCHARACTERIZED;
}

TinyDistortionClass tiny_distortion_encode(
    TinyImageFormat format, uint8_t quality, TinyEncodeDistortion* out
) {
    if (out) tiny_memset(out, 0, sizeof(*out));

    for (uint32_t i = 0; i < TINYIMG_DISTORTION_ENCODE_ROWS; i++) {
        const EncodeRow* r = &encode_rows[i];

        if (r->format != (uint8_t) format || r->quality != quality) continue;

        if (out) {
            out->klass = (TinyDistortionClass) r->klass;
            out->bytes_per_pixel = r->bpp_median;
            out->bytes_per_pixel_p95 = r->bpp_p95;
            out->p05_db = r->p05;
            out->median_db = r->median;
            out->n = r->n;
        }
        return (TinyDistortionClass) r->klass;
    }

    return TINYIMG_DISTORTION_UNCHARACTERIZED;
}

void tiny_distortion_rows(uint32_t* lever, uint32_t* encode) {
    if (lever) *lever = TINYIMG_DISTORTION_LEVER_ROWS;
    if (encode) *encode = TINYIMG_DISTORTION_ENCODE_ROWS;
}

int tiny_distortion_lever_row(
    uint32_t index, TinyImageFormat* format, uint8_t* effort_delta,
    uint8_t* filter_delta, uint8_t* scale_steps, TinyDistortion* out
) {
    if (index >= TINYIMG_DISTORTION_LEVER_ROWS) return TINYIMG_ERR_RANGE;

    const LeverRow* r = &lever_rows[index];

    if (format) *format = (TinyImageFormat) r->format;
    if (effort_delta) *effort_delta = r->effort_delta;
    if (filter_delta) *filter_delta = r->filter_delta;
    if (scale_steps) *scale_steps = r->scale_steps;
    if (out) {
        out->klass = (TinyDistortionClass) r->klass;
        out->p05_db = r->p05;
        out->median_db = r->median;
        out->n = r->n;
        out->max_abs = r->max_abs;
    }
    return TINYIMG_OK;
}
