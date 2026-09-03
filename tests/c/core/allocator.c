#include "../test.h"
#include "tinyimg/memory.h"

// a fixed heap so exhaustion, splitting and coalescing happen at sizes the test
// can predict; the growing heap is what the wasm lane exercises
static uint8_t heap[64 * 1024];

int main(void) {
    int r = 0;

    r |= assertEquals(tiny_heap_init(heap, sizeof(heap)), TINYIMG_OK);
    r |= assertEquals(tiny_heap_init(heap, 8), TINYIMG_ERR_RANGE);
    r |= assertEquals(tiny_heap_init(0, sizeof(heap)), TINYIMG_ERR_NULL);

    r |= assertEquals(tiny_heap_init(heap, sizeof(heap)), TINYIMG_OK);

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);
    r |= assertEquals((long) stats.free_blocks, 1L);
    r |= assertEquals((long) stats.used, 0L);
    r |= assertGreaterThan((double) stats.available, 60000.0);

    r |= assertNull(tiny_alloc(0));

    uint8_t* a = tiny_alloc(100);
    r |= assertNotNull(a);

    // every payload is aligned so a pixel buffer can be read with aligned SIMD
    r |= assertEquals((long) ((uintptr_t) a % TINYIMG_ALIGNMENT), 0L);

    // 100 rounds up to the alignment, and the caller may use the slack
    r |= assertEquals((long) tiny_alloc_size(a), 112L);

    uint8_t* b = tiny_alloc(200);
    uint8_t* c = tiny_alloc(300);
    r |= assertNotNull(b);
    r |= assertNotNull(c);
    r |= assertEquals((long) ((uintptr_t) b % TINYIMG_ALIGNMENT), 0L);
    r |= assertEquals((long) ((uintptr_t) c % TINYIMG_ALIGNMENT), 0L);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 4L);
    r |= assertEquals((long) stats.used, 112L + 208L + 304L);
    r |= assertEquals((long) stats.peak, 112L + 208L + 304L);

    size_t peakOfThree = stats.peak;

    // a freed hole in the middle is reused rather than skipped
    tiny_free(b);
    uint8_t* again = tiny_alloc(200);
    r |= assertTrue(again == b);

    // two adjacent free blocks become one, so a later request larger than
    // either of them still fits
    tiny_free(again);
    tiny_free(a);
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.free_blocks, 2L);

    uint8_t* merged = tiny_alloc(112 + 208 + 16);
    r |= assertTrue(merged == a);
    tiny_free(merged);
    tiny_free(c);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);
    r |= assertEquals((long) stats.used, 0L);

    // the peak is a high water mark and does not fall back to what is live. it
    // is above the three block figure because a coalesced block is handed over
    // whole, so the reused hole was larger than the request that took it
    r |= assertGreaterThan((double) stats.peak, (double) peakOfThree - 1.0);

    // a fixed heap does not grow, so exhaustion is a NULL rather than a trap
    r |= assertNull(tiny_alloc(sizeof(heap) * 2));
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);

    // freeing something the heap never handed out, and freeing twice, both have
    // to leave the block list alone
    uint8_t* live = tiny_alloc(64);
    r |= assertNotNull(live);
    tiny_free(live);
    tiny_free(live);
    tiny_free(0);

    uint8_t stack[128];
    tiny_memset(stack, 0, sizeof(stack));
    tiny_free(stack + 64);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);
    r |= assertEquals((long) stats.used, 0L);
    r |= assertEquals((long) tiny_alloc_size(0), 0L);

    // realloc with no pointer allocates, and with no size frees
    uint8_t* grown = tiny_realloc(0, 64);
    r |= assertNotNull(grown);
    r |= assertNull(tiny_realloc(grown, 0));

    // growing into a free successor keeps the address, which is what stops an
    // append heavy writer copying on every doubling
    uint8_t* head = tiny_alloc(256);
    r |= assertNotNull(head);
    for (int i = 0; i < 256; i++) head[i] = (uint8_t) i;

    uint8_t* inPlace = tiny_realloc(head, 1024);
    r |= assertTrue(inPlace == head);
    r |= assertEquals((long) inPlace[255], 255L);

    // with a live block in the way it has to move, and the bytes have to come
    // with it
    uint8_t* blocker = tiny_alloc(64);
    r |= assertNotNull(blocker);

    uint8_t* moved = tiny_realloc(inPlace, 4096);
    r |= assertNotNull(moved);
    r |= assertTrue(moved != inPlace);
    for (int i = 0; i < 256; i++) {
        if (moved[i] != (uint8_t) i) {
            r |= assertEquals((long) moved[i], (long) i);
            break;
        }
    }

    // shrinking returns the tail to the heap rather than holding it
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    size_t before = stats.used;

    uint8_t* shrunk = tiny_realloc(moved, 128);
    r |= assertTrue(shrunk == moved);
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertLessThan((double) stats.used, (double) before);
    r |= assertEquals((long) tiny_alloc_size(shrunk), 128L);

    tiny_free(shrunk);
    tiny_free(blocker);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);
    r |= assertEquals((long) stats.used, 0L);
    r |= assertEquals(tiny_heap_stats(0), TINYIMG_ERR_NULL);

    // many small blocks freed in a scattered order still collapse back to one,
    // which is the property that stops a long pipeline fragmenting the heap
    uint8_t* many[32];
    for (int i = 0; i < 32; i++) {
        many[i] = tiny_alloc(64 + (size_t) i * 16);
        r |= assertNotNull(many[i]);
    }

    for (int i = 0; i < 32; i += 2) tiny_free(many[i]);
    for (int i = 1; i < 32; i += 2) tiny_free(many[i]);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.blocks, 1L);
    r |= assertEquals((long) stats.used, 0L);

    return r;
}
