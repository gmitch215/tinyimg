/**
 * @file lzw.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief The variable width LZW shared by the GIF and TIFF codecs.
 * @version 1.0.0
 * @date 2026-09-02
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

/** Codes never exceed twelve bits in either format. */
#define TINY_LZW_MAX_BITS 12u

/** How many codes a dictionary can hold, which is all a twelve bit code names.
 */
#define TINY_LZW_CODES (1u << TINY_LZW_MAX_BITS)

/**
 * @brief The dictionary a stream builds as it is read.
 *
 * Held by the caller because it is 12 KiB and both callers already have an
 * arena open when they need one.
 */
typedef struct {
    /** The code this entry extends, or 0xFFFF for a root symbol. */
    uint16_t prefix[TINY_LZW_CODES];
    /** The byte this entry appends to its prefix. */
    uint8_t suffix[TINY_LZW_CODES];
    /** The first byte of this entry's whole expansion. */
    uint8_t first[TINY_LZW_CODES];
} TinyLzwTable;

/**
 * @brief A code stream being read, in either bit order and framing.
 *
 * The two formats differ in three ways and all three are fields here rather
 * than a second implementation: GIF packs codes least significant bit first and
 * TIFF most significant, GIF frames its data in length prefixed sub-blocks and
 * TIFF does not, and TIFF widens its codes one code earlier than GIF, which is
 * a documented quirk of that format rather than a mistake in either.
 */
typedef struct {
    /** The bytes being read. */
    const uint8_t* data;
    /** How many bytes there are. */
    size_t size;
    /** Next byte to pull into the accumulator. */
    size_t pos;
    /** Bits pulled but not yet consumed, an implementation detail. */
    uint32_t accumulator;
    /** How many bits the accumulator holds, an implementation detail. */
    uint32_t count;
    /** Bytes left in the sub-block being read, when framing applies. */
    uint32_t remaining;
    /** Non-zero when the data is framed in length prefixed sub-blocks. */
    uint8_t chained;
    /** Non-zero to read codes most significant bit first. */
    uint8_t msb;
    /** Set once the stream has run out, whether by marker or by end of buffer.
     */
    int done;
} TinyLzwReader;

/**
 * @brief Points a reader at a stream.
 *
 * @param reader The reader.
 * @param data The bytes.
 * @param size Number of bytes.
 * @param at Offset of the first code.
 * @param chained Non-zero when the data is framed in sub-blocks.
 * @param msb Non-zero to read most significant bit first.
 */
void tiny_lzw_init(
    TinyLzwReader* reader, const uint8_t* data, size_t size, size_t at,
    int chained, int msb
);

/**
 * @brief Expands a stream into a byte plane.
 *
 * Stops at the stream's end code, at the end of the buffer, or when the plane
 * is full, whichever comes first. A stream that stops early leaves the rest of
 * the plane as the caller filled it, which is what lets a truncated file decode
 * the rows that did arrive.
 *
 * @param reader The reader, positioned at the first code.
 * @param table Scratch for the dictionary, which the caller owns.
 * @param code_bits Root symbol width, so the first codes are one bit wider.
 * @param early Non-zero to widen a code one entry early, which is TIFF's rule.
 * @param out Destination plane.
 * @param capacity How many bytes the plane holds.
 * @return int TINYIMG_OK, or TINYIMG_ERR_CORRUPT for a code the dictionary
 * cannot explain.
 */
int tiny_lzw_expand(
    TinyLzwReader* reader, TinyLzwTable* table, uint32_t code_bits, int early,
    uint8_t* out, size_t capacity
);

#ifdef __cplusplus
}
#endif
