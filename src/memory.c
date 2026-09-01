#include <stddef.h>
#include <stdint.h>

#include "memory.h"

void* tiny_memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* tiny_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char) c;
    }
    return s;
}

void* tiny_memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;

    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    }
    else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int tiny_memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

// arena allocator

static uint8_t* tiny_heap;
static size_t tiny_offset;
static size_t tiny_capacity;

void tiny_heap_init(void* memory, size_t size) {
    tiny_heap = (uint8_t*) memory;
    tiny_offset = 0;
    tiny_capacity = size;
}

void* tiny_arena_alloc(size_t size, size_t alignment) {
    size_t mask = alignment - 1;
    size_t aligned = (tiny_offset + mask) & ~mask;

    if (aligned > tiny_capacity || size > tiny_capacity - aligned) {
        return 0; // not enough space
    }

    tiny_offset = aligned + size;
    return tiny_heap + aligned;
}

void tiny_arena_reset() {
    tiny_offset = 0;
}
