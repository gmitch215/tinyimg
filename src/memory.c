#include "tinyimg/memory.h"

#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#if !defined(__wasm__)
    #include <stdlib.h>
#endif

#pragma region primitives

// bulk memory lowers all three of these to memory.copy and memory.fill; memcmp
// has no wasm instruction, so it stays a loop rather than becoming an import
void* tiny_memcpy(void* dest, const void* src, size_t n) {
    if (n > 0) __builtin_memcpy(dest, src, n);
    return dest;
}

void* tiny_memset(void* s, int c, size_t n) {
    if (n > 0) __builtin_memset(s, c, n);
    return s;
}

void* tiny_memmove(void* dest, const void* src, size_t n) {
    if (n > 0) __builtin_memmove(dest, src, n);
    return dest;
}

int tiny_memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int) p1[i] - (int) p2[i];
        }
    }
    return 0;
}

#pragma endregion

#pragma region heap

typedef struct TinyBlock {
    struct TinyBlock* prev;
    struct TinyBlock* next;
    size_t size;
    uint32_t flags;
} TinyBlock;

#define BLOCK_FREE 0x00000001u
#define BLOCK_TAG 0x54494D00u
#define BLOCK_TAG_MASK 0xFFFFFF00u
#define BLOCK_HEADER                                                           \
    ((sizeof(TinyBlock) + TINYIMG_ALIGNMENT - 1) &                             \
     ~(size_t) (TINYIMG_ALIGNMENT - 1))

#define WASM_PAGE 65536u

static TinyBlock* heap_first;
static TinyBlock* heap_last;
static uint8_t* heap_end;
static size_t heap_capacity;
static size_t heap_used;
static size_t heap_peak;
static int heap_growable;

static void arena_drop_all(void);
static void blob_drop_all(void);

static inline size_t align_up(size_t n) {
    return (n + (TINYIMG_ALIGNMENT - 1)) & ~(size_t) (TINYIMG_ALIGNMENT - 1);
}

static inline uint8_t* payload_of(TinyBlock* block) {
    return (uint8_t*) block + BLOCK_HEADER;
}

static inline TinyBlock* block_of(const void* pointer) {
    return (TinyBlock*) (void*) ((uint8_t*) pointer - BLOCK_HEADER);
}

static inline int block_live(const TinyBlock* block) {
    return (block->flags & BLOCK_TAG_MASK) == BLOCK_TAG;
}

static inline int block_free(const TinyBlock* block) {
    return (block->flags & BLOCK_FREE) != 0;
}

static void heap_install(void* memory, size_t size, int growable) {
    arena_drop_all();
    blob_drop_all();

    uint8_t* base = (uint8_t*) memory;
    size_t offset =
        align_up((size_t) (uintptr_t) base) - (size_t) (uintptr_t) base;

    if (size < offset + BLOCK_HEADER + TINYIMG_ALIGNMENT) {
        heap_first = 0;
        heap_last = 0;
        heap_end = 0;
        heap_capacity = 0;
        heap_used = 0;
        heap_peak = 0;
        heap_growable = 0;
        return;
    }

    base += offset;
    size -= offset;

    TinyBlock* block = (TinyBlock*) (void*) base;
    block->prev = 0;
    block->next = 0;
    block->size = size - BLOCK_HEADER;
    block->flags = BLOCK_TAG | BLOCK_FREE;

    heap_first = block;
    heap_last = block;
    heap_end = base + size;
    heap_capacity = size;
    heap_used = 0;
    heap_peak = 0;
    heap_growable = growable;
}

int tiny_heap_init(void* memory, size_t size) {
    if (!memory) return TINYIMG_ERR_NULL;

    heap_install(memory, size, 0);
    return heap_first ? TINYIMG_OK : TINYIMG_ERR_RANGE;
}

TINYIMG_EXPORT("tiny_heap_bootstrap")
void tiny_heap_bootstrap(void) {
    if (heap_first) return;

#if defined(__wasm__)
    extern uint8_t __heap_base;

    uint8_t* base = &__heap_base;
    size_t end = (size_t) __builtin_wasm_memory_size(0) * WASM_PAGE;
    size_t start = (size_t) (uintptr_t) base;

    // the link settings put __heap_base well inside the initial memory, so this
    // only ever fires if someone shrinks --initial-memory below the stack
    if (end < start + BLOCK_HEADER + TINYIMG_ALIGNMENT) {
        size_t pages = (start + WASM_PAGE - 1) / WASM_PAGE + 1;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t) -1) return;
        end = (size_t) __builtin_wasm_memory_size(0) * WASM_PAGE;
    }

    heap_install(base, end - start, 1);
#else
    // untouched address space costs nothing, so the host takes its ceiling in
    // one reservation and never has to stitch two regions together
    size_t size = TINYIMG_HEAP_MAX;

    while (size >= 65536u) {
        void* memory = malloc(size);
        if (memory) {
            heap_install(memory, size, 0);
            return;
        }
        size /= 4;
    }
#endif
}

static TinyBlock* coalesce(TinyBlock* block) {
    TinyBlock* next = block->next;
    if (next && block_free(next)) {
        block->size += BLOCK_HEADER + next->size;
        block->next = next->next;

        if (block->next)
            block->next->prev = block;
        else
            heap_last = block;
    }

    TinyBlock* prev = block->prev;
    if (prev && block_free(prev)) {
        prev->size += BLOCK_HEADER + block->size;
        prev->next = block->next;

        if (prev->next)
            prev->next->prev = prev;
        else
            heap_last = prev;

        block = prev;
    }

    return block;
}

static int heap_grow(size_t needed) {
    if (!heap_growable) return 0;

#if defined(__wasm__)
    size_t bytes = needed + BLOCK_HEADER;

    // sixteen pages minimum so a decode does not grow once per scanline
    size_t pages = (bytes + WASM_PAGE - 1) / WASM_PAGE;
    if (pages < 16) pages = 16;

    size_t previous = (size_t) __builtin_wasm_memory_grow(0, pages);
    if (previous == (size_t) -1) return 0;

    size_t added = (size_t) ((previous + pages) * WASM_PAGE) -
                   (size_t) (uintptr_t) heap_end;
    if (added < BLOCK_HEADER + TINYIMG_ALIGNMENT) return 0;

    TinyBlock* block = (TinyBlock*) (void*) heap_end;
    block->prev = heap_last;
    block->next = 0;
    block->size = added - BLOCK_HEADER;
    block->flags = BLOCK_TAG | BLOCK_FREE;

    if (heap_last)
        heap_last->next = block;
    else
        heap_first = block;

    heap_last = block;
    heap_end += added;
    heap_capacity += added;

    coalesce(block);
    return 1;
#else
    (void) needed;
    return 0;
#endif
}

static void block_split(TinyBlock* block, size_t size) {
    if (block->size < size + BLOCK_HEADER + TINYIMG_ALIGNMENT) return;

    TinyBlock* tail = (TinyBlock*) (void*) (payload_of(block) + size);
    tail->prev = block;
    tail->next = block->next;
    tail->size = block->size - size - BLOCK_HEADER;
    tail->flags = BLOCK_TAG | BLOCK_FREE;

    if (tail->next)
        tail->next->prev = tail;
    else
        heap_last = tail;

    block->next = tail;
    block->size = size;
}

static TinyBlock* heap_take(size_t size) {
    for (TinyBlock* block = heap_first; block; block = block->next) {
        if (block_free(block) && block->size >= size) {
            block_split(block, size);
            block->flags = BLOCK_TAG;
            return block;
        }
    }
    return 0;
}

TINYIMG_EXPORT("tiny_alloc")
void* tiny_alloc(size_t size) {
    if (size == 0) return 0;

    tiny_heap_bootstrap();
    if (!heap_first) return 0;

    size_t need = align_up(size);
    if (need < size) return 0;

    TinyBlock* block = heap_take(need);
    if (!block) {
        if (!heap_grow(need)) return 0;
        block = heap_take(need);
        if (!block) return 0;
    }

    heap_used += block->size;
    if (heap_used > heap_peak) heap_peak = heap_used;

    return payload_of(block);
}

TINYIMG_EXPORT("tiny_free")
void tiny_free(void* pointer) {
    if (!pointer) return;

    TinyBlock* block = block_of(pointer);
    if (!block_live(block) || block_free(block)) return;

    heap_used -= block->size;
    block->flags = BLOCK_TAG | BLOCK_FREE;
    coalesce(block);
}

size_t tiny_alloc_size(const void* pointer) {
    if (!pointer) return 0;

    const TinyBlock* block = block_of(pointer);
    if (!block_live(block) || block_free(block)) return 0;

    return block->size;
}

TINYIMG_EXPORT("tiny_realloc")
void* tiny_realloc(void* pointer, size_t size) {
    if (!pointer) return tiny_alloc(size);
    if (size == 0) {
        tiny_free(pointer);
        return 0;
    }

    TinyBlock* block = block_of(pointer);
    if (!block_live(block) || block_free(block)) return 0;

    size_t need = align_up(size);
    if (need < size) return 0;

    if (block->size >= need) {
        size_t before = block->size;
        block_split(block, need);

        if (block->size != before) {
            heap_used -= before - block->size;
            coalesce(block->next);
        }
        return pointer;
    }

    // absorbing a free successor is what keeps an append heavy writer from
    // copying its whole buffer on every doubling
    TinyBlock* next = block->next;
    if (next && block_free(next) &&
        block->size + BLOCK_HEADER + next->size >= need) {
        size_t before = block->size;

        block->size += BLOCK_HEADER + next->size;
        block->next = next->next;
        if (block->next)
            block->next->prev = block;
        else
            heap_last = block;

        block_split(block, need);
        heap_used += block->size - before;
        if (heap_used > heap_peak) heap_peak = heap_used;
        return pointer;
    }

    void* moved = tiny_alloc(size);
    if (!moved) return 0;

    tiny_memcpy(moved, pointer, block->size);
    tiny_free(pointer);
    return moved;
}

int tiny_heap_stats(TinyHeapStats* stats) {
    if (!stats) return TINYIMG_ERR_NULL;

    stats->capacity = heap_capacity;
    stats->used = heap_used;
    stats->available = 0;
    stats->peak = heap_peak;
    stats->blocks = 0;
    stats->free_blocks = 0;

    for (const TinyBlock* block = heap_first; block; block = block->next) {
        stats->blocks++;

        if (block_free(block)) {
            stats->free_blocks++;
            stats->available += block->size;
        }
    }

    return TINYIMG_OK;
}

#pragma endregion

#pragma region arena

typedef struct TinyArenaChunk {
    struct TinyArenaChunk* prev;
    size_t size;
    size_t used;
} TinyArenaChunk;

#define ARENA_HEADER                                                           \
    ((sizeof(TinyArenaChunk) + TINYIMG_ALIGNMENT - 1) &                        \
     ~(size_t) (TINYIMG_ALIGNMENT - 1))
#define ARENA_CHUNK 65536u

static TinyArenaChunk* arena_top;
static size_t arena_reserved;

static void arena_drop_all(void) {
    // called from heap_install, where the chunks are about to stop existing, so
    // the list is dropped rather than freed
    arena_top = 0;
    arena_reserved = 0;
}

static inline uint8_t* arena_base(TinyArenaChunk* chunk) {
    return (uint8_t*) chunk + ARENA_HEADER;
}

void* tiny_arena_alloc(size_t size, size_t alignment) {
    if (size == 0) return 0;

    if (alignment == 0) alignment = TINYIMG_ALIGNMENT;
    if ((alignment & (alignment - 1)) != 0) return 0;

    if (arena_top) {
        uint8_t* base = arena_base(arena_top);
        uintptr_t head = (uintptr_t) (base + arena_top->used);
        uintptr_t aligned =
            (head + alignment - 1) & ~(uintptr_t) (alignment - 1);
        size_t offset = (size_t) (aligned - (uintptr_t) base);

        if (offset <= arena_top->size && size <= arena_top->size - offset) {
            arena_top->used = offset + size;
            return base + offset;
        }
    }

    size_t payload = size + alignment;
    if (payload < ARENA_CHUNK) payload = ARENA_CHUNK;

    TinyArenaChunk* chunk = tiny_alloc(ARENA_HEADER + payload);
    if (!chunk) return 0;

    chunk->prev = arena_top;
    chunk->size = tiny_alloc_size(chunk) - ARENA_HEADER;
    chunk->used = 0;

    arena_top = chunk;
    arena_reserved += ARENA_HEADER + chunk->size;

    uint8_t* base = arena_base(chunk);
    uintptr_t aligned =
        ((uintptr_t) base + alignment - 1) & ~(uintptr_t) (alignment - 1);
    size_t offset = (size_t) (aligned - (uintptr_t) base);

    chunk->used = offset + size;
    return base + offset;
}

void tiny_arena_mark(TinyArenaMark* mark) {
    if (!mark) return;

    mark->chunk = arena_top;
    mark->used = arena_top ? arena_top->used : 0;
}

void tiny_arena_release(const TinyArenaMark* mark) {
    if (!mark) return;

    while (arena_top && (void*) arena_top != mark->chunk) {
        TinyArenaChunk* prev = arena_top->prev;

        arena_reserved -= ARENA_HEADER + arena_top->size;
        tiny_free(arena_top);
        arena_top = prev;
    }

    if (arena_top) arena_top->used = mark->used;
}

TINYIMG_EXPORT("tiny_arena_reset")
void tiny_arena_reset(void) {
    TinyArenaMark mark = {0, 0};
    tiny_arena_release(&mark);
}

size_t tiny_arena_reserved(void) {
    return arena_reserved;
}

#pragma endregion

#pragma region blobs

typedef struct {
    TinyBlobKind kind;
    char id[TINYIMG_BLOB_ID_MAX];
    const uint8_t* data;
    size_t size;
} TinyBlobSlot;

static TinyBlobSlot blobs[TINYIMG_MAX_BLOBS];

static void blob_drop_all(void) {
    // heap_install is replacing the memory the blobs live in, so the slots are
    // cleared without freeing
    tiny_memset(blobs, 0, sizeof(blobs));
}

static TinyBlobSlot* blob_find(TinyBlobKind kind, const char* id) {
    for (uint32_t i = 0; i < TINYIMG_MAX_BLOBS; i++) {
        if (!blobs[i].data || blobs[i].kind != kind) continue;
        if (!id || tiny_strcmp(blobs[i].id, id) == 0) return &blobs[i];
    }
    return 0;
}

TINYIMG_EXPORT("tiny_blob_load")
int tiny_blob_load(
    TinyBlobKind kind, const char* id, const uint8_t* data, size_t size
) {
    if (!data || size == 0) return TINYIMG_ERR_NULL;

    char name[TINYIMG_BLOB_ID_MAX];
    tiny_strcopy(name, id, sizeof(name));

    TinyBlobSlot* slot = blob_find(kind, name);
    if (slot) {
        tiny_free((void*) slot->data);
    }
    else {
        for (uint32_t i = 0; i < TINYIMG_MAX_BLOBS; i++) {
            if (!blobs[i].data) {
                slot = &blobs[i];
                break;
            }
        }
    }

    if (!slot) return TINYIMG_ERR_MEMORY;

    slot->kind = kind;
    slot->data = data;
    slot->size = size;
    tiny_strcopy(slot->id, name, sizeof(slot->id));

    return TINYIMG_OK;
}

const uint8_t* tiny_blob_get(TinyBlobKind kind, const char* id, size_t* size) {
    TinyBlobSlot* slot = blob_find(kind, id);
    if (!slot) return 0;

    if (size) *size = slot->size;
    return slot->data;
}

const uint8_t* tiny_blob_at(
    TinyBlobKind kind, uint32_t index, const char** id, size_t* size
) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < TINYIMG_MAX_BLOBS; i++) {
        if (!blobs[i].data || blobs[i].kind != kind) continue;
        if (seen++ != index) continue;

        if (id) *id = blobs[i].id;
        if (size) *size = blobs[i].size;
        return blobs[i].data;
    }

    return 0;
}

TINYIMG_EXPORT("tiny_blob_free")
int tiny_blob_free(TinyBlobKind kind, const char* id) {
    TinyBlobSlot* slot = blob_find(kind, id);
    if (!slot) return TINYIMG_ERR_NOT_FOUND;

    tiny_free((void*) slot->data);
    tiny_memset(slot, 0, sizeof(*slot));

    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_blob_free_all")
void tiny_blob_free_all(void) {
    for (uint32_t i = 0; i < TINYIMG_MAX_BLOBS; i++) {
        if (blobs[i].data) tiny_free((void*) blobs[i].data);
    }
    tiny_memset(blobs, 0, sizeof(blobs));
}

#pragma endregion
