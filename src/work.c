#include "tinyimg/work.h"

#define TINY_WORK_COUNT 10

static uint32_t counters[TINY_WORK_COUNT];

TINYIMG_EXPORT("tiny_work_reset")
void tiny_work_reset(void) {
    for (uint32_t i = 0; i < TINY_WORK_COUNT; i++) counters[i] = 0;
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
