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

/**
 * @brief Initializes a simple arena allocator with a given memory block and
 * size. The allocator will manage memory allocations within this block,
 * allowing for efficient allocation and deallocation of memory.
 *
 * @param memory Pointer to the memory block to be used for the arena allocator.
 * @param size Size of the memory block in bytes.
 */
void tiny_heap_init(void* memory, size_t size);

#ifdef __cplusplus
}
#endif
