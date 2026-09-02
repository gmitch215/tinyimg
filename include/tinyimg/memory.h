/**
 * @file memory.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Lightweight memory management implementatins in C
 * @version 1.0.0
 * @date 2026-08-31
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of pixels allowed in an image. This limit is set to
 * prevent excessive memory usage and potential performance issues when
 * processing large images.
 *
 * Correlates to a maximum image size of 4000x4000 pixels.
 */
#define TINYIMG_MAX_PIXELS 16000000u

/**
 * @brief Alignment every allocation is rounded up to.
 *
 * Sixteen bytes so a pixel buffer can be read with aligned SIMD128 loads.
 */
#define TINYIMG_ALIGNMENT 16u

/**
 * @brief Largest heap the host build reserves, matching the wasm module's
 * --max-memory.
 */
#define TINYIMG_HEAP_MAX 67108864u

#pragma region primitives

/**
 * @brief Replicates the behavior of the standard memcpy function, copying n
 * bytes from src to dest.
 *
 * O(n) time complexity, where n is the number of bytes to copy.
 *
 * @param dest Destination buffer where the content is to be copied.
 * @param src Source buffer from which the content is to be copied.
 * @param n Number of bytes to copy from src to dest.
 * @return void* Pointer to the destination buffer dest.
 */
void* tiny_memcpy(void* dest, const void* src, size_t n);

/**
 * @brief Replicates the behavior of the standard memset function, filling the
 * first n bytes of the memory area pointed to by s with the constant byte c.
 *
 * O(n) time complexity, where n is the number of bytes to set.
 *
 * @param s Pointer to the memory area to be filled.
 * @param c Constant byte value to fill the memory area with.
 * @param n Number of bytes to fill in the memory area.
 * @return void* Pointer to the memory area s.
 */
void* tiny_memset(void* s, int c, size_t n);

/**
 * @brief Replicates the behavior of the standard memmove function, copying n
 * bytes from src to dest. Unlike memcpy, memmove is safe to use when the source
 * and destination memory areas overlap.
 *
 * O(n) time complexity, where n is the number of bytes to copy.
 *
 * @param dest Destination buffer where the content is to be copied.
 * @param src Source buffer from which the content is to be copied.
 * @param n Number of bytes to copy from src to dest.
 * @return void* Pointer to the destination buffer dest.
 */
void* tiny_memmove(void* dest, const void* src, size_t n);

/**
 * @brief Replicates the behavior of the standard memcmp function, comparing the
 * first n bytes of the memory areas pointed to by s1 and s2.
 *
 * O(n) time complexity, where n is the number of bytes to compare.
 *
 * @param s1 Pointer to the first memory area to be compared.
 * @param s2 Pointer to the second memory area to be compared.
 * @param n Number of bytes to compare.
 * @return int An integer less than, equal to, or greater than zero if the first
 * n bytes of s1 are found, respectively, to be less than, equal to, or greater
 * than the first n bytes of s2.
 */
int tiny_memcmp(const void* s1, const void* s2, size_t n);

#pragma endregion

#pragma region heap

/**
 * @brief Installs a caller supplied block as the heap.
 *
 * The block is never grown, which is what makes it the right choice for a test
 * that wants exhaustion to happen at a predictable size. A wasm module or a
 * host process should call tiny_heap_bootstrap instead and let the heap grow.
 *
 * Any pointer handed out by a previous heap is dangling afterwards.
 *
 * @param memory Pointer to the memory block to be used for the heap.
 * @param size Size of the memory block in bytes.
 * @return int TINYIMG_OK, or TINYIMG_ERR_RANGE when the block is too small to
 * hold a single allocation.
 */
int tiny_heap_init(void* memory, size_t size);

/**
 * @brief Installs the platform's default heap if no heap exists yet.
 *
 * On wasm the heap starts at __heap_base and grows through memory.grow up to
 * the module's --max-memory. On a host build it is one reservation of
 * TINYIMG_HEAP_MAX, which stays untouched address space until written to.
 *
 * Idempotent, and called by every allocation, so nothing has to remember to
 * initialise the library before using it.
 */
void tiny_heap_bootstrap(void);

/**
 * @brief A snapshot of heap occupancy.
 *
 * Reported by the size and benchmark harnesses; `peak` is the number the wasm
 * module's memory ceiling has to cover.
 */
typedef struct {
    /** Bytes the heap region spans. */
    size_t capacity;
    /** Payload bytes currently handed out. */
    size_t used;
    /** Payload bytes sitting in free blocks. */
    size_t available;
    /** Highest `used` has reached since the heap was installed. */
    size_t peak;
    /** Blocks in the address ordered list, free and live together. */
    uint32_t blocks;
    /** How many of those blocks are free. */
    uint32_t free_blocks;
} TinyHeapStats;

/**
 * @brief Reads the current heap occupancy.
 *
 * O(n) time complexity, where n is the number of blocks.
 *
 * @param stats Receives the snapshot.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_heap_stats(TinyHeapStats* stats);

/**
 * @brief Allocates a block from the heap.
 *
 * First fit over an address ordered block list, so the cost is linear in the
 * number of live blocks rather than constant. An image pipeline holds a handful
 * of blocks at a time; a workload that holds thousands would want a size
 * bucketed free list instead.
 *
 * The returned block is aligned to TINYIMG_ALIGNMENT and its contents are
 * undefined.
 *
 * @param size Number of bytes to allocate. Zero returns NULL.
 * @return void* The block, or NULL when the heap could not grow enough.
 */
void* tiny_alloc(size_t size);

/**
 * @brief Resizes a block, moving it only when it cannot grow in place.
 *
 * A block whose successor is free absorbs it rather than moving, which is what
 * keeps an append heavy TinyWriter from copying on every doubling.
 *
 * @param pointer Block to resize, or NULL to allocate.
 * @param size New size in bytes. Zero frees the block and returns NULL.
 * @return void* The block, or NULL on failure. The original block is left
 * untouched when the call fails.
 */
void* tiny_realloc(void* pointer, size_t size);

/**
 * @brief Returns a block to the heap, coalescing it with free neighbours.
 *
 * A NULL pointer, a pointer the heap never handed out, and a second free of the
 * same pointer are all ignored rather than corrupting the block list.
 *
 * @param pointer The block to free.
 */
void tiny_free(void* pointer);

/**
 * @brief Usable size of a block, which may exceed what was asked for.
 *
 * @param pointer The block.
 * @return size_t The payload size, or 0 if the pointer is not a live block.
 */
size_t tiny_alloc_size(const void* pointer);

#pragma endregion

#pragma region arena

/**
 * @brief A saved arena position.
 *
 * Opaque; the fields exist so a caller can keep one on the stack.
 */
typedef struct {
    /** The chunk that was current, an implementation detail. */
    void* chunk;
    /** How far into that chunk the head had reached. */
    size_t used;
} TinyArenaMark;

/**
 * @brief Allocates a block from the arena, aligned to the requested boundary.
 *
 * The arena never frees an individual block. It is reclaimed in bulk, either
 * back to a saved position with tiny_arena_release or entirely with
 * tiny_arena_reset. Use it for scratch that lives no longer than the call that
 * asked for it.
 *
 * Amortised O(1) time complexity. The arena takes chunks from the heap as it
 * needs them, so an allocation never invalidates a pointer it handed out
 * earlier.
 *
 * @param size Number of bytes to allocate.
 * @param alignment Required alignment in bytes. Must be a power of two; zero
 * means TINYIMG_ALIGNMENT.
 * @return void* Pointer to the block, or NULL if the heap has no room.
 */
void* tiny_arena_alloc(size_t size, size_t alignment);

/**
 * @brief Records the current arena position.
 *
 * @param mark Receives the position.
 */
void tiny_arena_mark(TinyArenaMark* mark);

/**
 * @brief Rewinds the arena to a recorded position.
 *
 * Any pointer handed out after the mark was taken is dangling afterwards.
 *
 * @param mark A position from tiny_arena_mark.
 */
void tiny_arena_release(const TinyArenaMark* mark);

/**
 * @brief Releases every block the arena has handed out.
 *
 * O(n) time complexity, where n is the number of chunks. Any pointer previously
 * returned by tiny_arena_alloc is dangling afterwards.
 */
void tiny_arena_reset(void);

/**
 * @brief Bytes the arena currently holds from the heap.
 *
 * @return size_t The total size of the arena's chunks.
 */
size_t tiny_arena_reserved(void);

#pragma endregion

#pragma region blobs

/**
 * @brief Kinds of data the library reads but does not ship.
 *
 * Anything large enough to dominate the module, or only needed by some callers,
 * arrives at runtime instead of being linked in. The host either imports it as
 * a wrangler Data module or fetches it from a bucket; the library only ever
 * sees bytes already in linear memory.
 */
typedef enum TinyBlobKind
{
    /**
     * @brief A TrueType, OpenType, PSF or BDF face.
     */
    TINYIMG_BLOB_FONT = 0,
    /**
     * @brief An ICC colour profile.
     */
    TINYIMG_BLOB_ICC = 1,
    /**
     * @brief A packed detection cascade.
     */
    TINYIMG_BLOB_CASCADE = 2,
} TinyBlobKind;

/**
 * @brief How many blobs can be resident at once.
 */
#define TINYIMG_MAX_BLOBS 8

/**
 * @brief Longest blob id, including the terminator.
 */
#define TINYIMG_BLOB_ID_MAX 32

/**
 * @brief Registers a blob under an id.
 *
 * The library takes ownership of `data`, which must have come from tiny_alloc,
 * and releases it with tiny_blob_free or tiny_blob_free_all. Loading over an
 * existing kind and id replaces it and frees what was there.
 *
 * @param kind What the bytes are.
 * @param id Name the caller will ask for it by. Truncated at
 * TINYIMG_BLOB_ID_MAX.
 * @param data The bytes, owned by the library from here on.
 * @param size Number of bytes.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_MEMORY when all
 * TINYIMG_MAX_BLOBS slots are taken.
 */
int tiny_blob_load(
    TinyBlobKind kind, const char* id, const uint8_t* data, size_t size
);

/**
 * @brief Looks up a resident blob.
 *
 * @param kind What to look for.
 * @param id The id it was loaded under, or NULL for the first blob of that
 * kind.
 * @param size Receives the byte length. May be NULL.
 * @return const uint8_t* The bytes, or NULL when nothing matches.
 */
const uint8_t* tiny_blob_get(TinyBlobKind kind, const char* id, size_t* size);

/**
 * @brief Releases one resident blob.
 *
 * @param kind What to release.
 * @param id The id it was loaded under, or NULL for the first blob of that
 * kind.
 * @return int TINYIMG_OK or TINYIMG_ERR_NOT_FOUND.
 */
int tiny_blob_free(TinyBlobKind kind, const char* id);

/**
 * @brief Releases every resident blob.
 */
void tiny_blob_free_all(void);

#pragma endregion

#ifdef __cplusplus
}
#endif
