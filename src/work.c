#include "tinyimg/work.h"

#include "cost-model-api.h"
#include "tinyimg/plan.h"

#define TINY_WORK_COUNT 15

static uint32_t counters[TINY_WORK_COUNT];
static uint32_t base[TINY_WORK_COUNT];
static uint32_t limit;

// the formats the clock prices the byte and encode terms by
static TinyImageFormat decode_format;
static uint8_t decode_variant;
static TinyImageFormat encode_format;
static TinyEncodePriceOpts encode_price;

TINYIMG_EXPORT("tiny_work_reset")
void tiny_work_reset(void) {
    for (uint32_t i = 0; i < TINY_WORK_COUNT; i++) {
        counters[i] = 0;
        base[i] = 0;
    }
}

TINYIMG_EXPORT("tiny_work_read")
uint32_t tiny_work_read(TinyWorkCounter counter) {
    uint32_t index = (uint32_t) counter;
    return index < TINY_WORK_COUNT ? counters[index] : 0;
}

void tiny_work_add(TinyWorkCounter counter, uint32_t amount) {
    uint32_t index = (uint32_t) counter;
    if (index < TINY_WORK_COUNT) counters[index] += amount;
}

void tiny_work_count_alpha(const TinyImage* image, int partial_only) {
    if (!image || !image->data) return;

    uint32_t channels = image->channels;
    if (channels != 2 && channels != 4) return;

    size_t pixels = (size_t) image->width * image->height;
    const uint8_t* alpha = image->data + (channels - 1);

    // a one-bit mask keeps 0 and 255, so only the values between are lost
    uint32_t lowest = partial_only ? 1u : 0u;
    uint32_t dropped = 0;

    for (size_t i = 0; i < pixels; i++) {
        uint32_t a = alpha[i * channels];
        dropped += (uint32_t) ((a < 255u) & (a >= lowest));
    }

    tiny_work_add(TINYIMG_WORK_ALPHA_DROPPED, dropped);
}

// #region clock

static uint32_t since(TinyWorkCounter counter) {
    return counters[counter] - base[counter];
}

TINYIMG_EXPORT("tiny_work_set_limit")
void tiny_work_set_limit(uint32_t micros) {
    limit = micros;

    for (uint32_t i = 0; i < TINY_WORK_COUNT; i++) {
        base[i] = micros ? counters[i] : 0;
    }
}

void tiny_work_begin_decode(TinyImageFormat format, int variant) {
    decode_format = format;
    decode_variant = variant ? 1u : 0u;
}

void tiny_work_begin_encode(
    TinyImageFormat format, const TinyEncodeOpts* opts, uint8_t channels
) {
    encode_format = format;
    encode_price.quality = opts ? opts->quality : 0u;
    encode_price.lossless = opts ? opts->lossless : 0u;
    encode_price.compression = opts ? opts->compression : 0u;
    encode_price.effort = opts ? opts->effort : 0u;
    encode_price.channels = channels;
}

void tiny_work_charge_decode(
    uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h, uint8_t den, uint8_t effort
) {
    tiny_work_add(
        TINYIMG_WORK_DECODE_MICROS, tiny_cost_decode_samples_us(
                                        decode_format, decode_variant, width,
                                        height, x, y, w, h, den, effort
                                    )
    );
}

TINYIMG_EXPORT("tiny_work_clock_us")
uint32_t tiny_work_clock_us(void) {
    uint64_t micros = since(TINYIMG_WORK_DECODE_MICROS);

    micros += (uint64_t) since(TINYIMG_WORK_ENTROPY_BYTES) *
              tiny_cost_entropy_rate(decode_format, decode_variant) / 1000000u;

    // a resample that writes as many samples as the decoder produced is a
    // copy, which the planner prices at nothing
    uint64_t resampled = since(TINYIMG_WORK_RESAMPLED);

    if (resampled != since(TINYIMG_WORK_DECODED_SAMPLES)) {
        micros +=
            resampled * tiny_cost_stage_rate(COST_STAGE_RESAMPLE) / 1000000u;
    }

    uint32_t passes = since(TINYIMG_WORK_PASSES);

    if (passes > 1u) {
        micros += resampled * (passes - 1u) *
                  tiny_cost_stage_rate(COST_STAGE_NEIGHBORHOOD) / 1000000u;
    }

    // the price is linear in samples, so a width of N and a height of 1 is N
    // samples
    micros += tiny_encode_cost_for(
        encode_format, since(TINYIMG_WORK_ENCODED), 1u, &encode_price
    );

    return micros > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) micros;
}

int tiny_work_over_limit(void) {
    return limit != 0u && tiny_work_clock_us() > limit;
}

// #endregion
