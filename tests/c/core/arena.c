#include "../test.h"
#include "tinyimg/memory.h"

static uint8_t heap[1024 * 1024];

int main(void) {
    int r = 0;

    r |= assertEquals(tiny_heap_init(heap, sizeof(heap)), TINYIMG_OK);

    r |= assertNull(tiny_arena_alloc(0, 16));

    // alignment must be a power of two, and zero means the default
    r |= assertNull(tiny_arena_alloc(32, 24));

    uint8_t* p = tiny_arena_alloc(32, 0);
    r |= assertNotNull(p);
    r |= assertEquals((long) ((uintptr_t) p % TINYIMG_ALIGNMENT), 0L);

    // a chunk comes from the heap, so the arena shows up in heap occupancy
    r |= assertGreaterThan((double) tiny_arena_reserved(), 0.0);

    TinyHeapStats stats;
    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertGreaterThan((double) stats.used, 0.0);

    for (size_t alignment = 1; alignment <= 256; alignment *= 2) {
        uint8_t* aligned = tiny_arena_alloc(17, alignment);
        r |= assertNotNull(aligned);
        r |= assertEquals((long) ((uintptr_t) aligned % alignment), 0L);
    }

    // a mark and release pair puts the head back without touching the chunk
    TinyArenaMark mark;
    tiny_arena_mark(&mark);

    uint8_t* first = tiny_arena_alloc(64, 16);
    r |= assertNotNull(first);
    tiny_arena_release(&mark);

    uint8_t* second = tiny_arena_alloc(64, 16);
    r |= assertTrue(second == first);

    // a fresh chunk never invalidates a pointer the arena already handed out,
    // which is what separates it from one growing buffer
    tiny_arena_reset();
    r |= assertEquals((long) tiny_arena_reserved(), 0L);

    uint8_t* early = tiny_arena_alloc(1024, 16);
    r |= assertNotNull(early);
    tiny_memset(early, 0x5A, 1024);

    size_t reservedBefore = tiny_arena_reserved();

    // well past the 64 KiB chunk, so several chunks have to be in play
    int served = 1;
    for (int i = 0; i < 200; i++) {
        uint8_t* filler = tiny_arena_alloc(1024, 16);
        if (!filler) {
            served = 0;
            break;
        }
        tiny_memset(filler, (uint8_t) i, 1024);
    }
    r |= assertTrue(served);

    r |= assertGreaterThan(
        (double) tiny_arena_reserved(), (double) reservedBefore
    );

    int intact = 1;
    for (int i = 0; i < 1024; i++) {
        if (early[i] != 0x5A) intact = 0;
    }
    r |= assertTrue(intact);

    // releasing back across a chunk boundary hands the chunks back to the heap
    TinyArenaMark boundary;
    tiny_arena_mark(&boundary);

    size_t atBoundary = tiny_arena_reserved();
    served = 1;
    for (int i = 0; i < 200; i++) {
        if (!tiny_arena_alloc(1024, 16)) served = 0;
    }
    r |= assertTrue(served);
    r |= assertGreaterThan((double) tiny_arena_reserved(), (double) atBoundary);

    tiny_arena_release(&boundary);
    r |= assertEquals((long) tiny_arena_reserved(), (long) atBoundary);

    intact = 1;
    for (int i = 0; i < 1024; i++) {
        if (early[i] != 0x5A) intact = 0;
    }
    r |= assertTrue(intact);

    // a request larger than the default chunk gets its own chunk
    tiny_arena_reset();
    uint8_t* big = tiny_arena_alloc(200000, 16);
    r |= assertNotNull(big);
    tiny_memset(big, 0xC3, 200000);
    r |= assertEquals((long) big[199999], 0xC3L);

    tiny_arena_reset();
    r |= assertEquals((long) tiny_arena_reserved(), 0L);

    r |= assertEquals(tiny_heap_stats(&stats), TINYIMG_OK);
    r |= assertEquals((long) stats.used, 0L);
    r |= assertEquals((long) stats.blocks, 1L);

    // a NULL mark is ignored rather than releasing everything by accident
    uint8_t* kept = tiny_arena_alloc(64, 16);
    r |= assertNotNull(kept);
    tiny_arena_mark(0);
    tiny_arena_release(0);
    r |= assertGreaterThan((double) tiny_arena_reserved(), 0.0);

    tiny_arena_reset();

    return r;
}
