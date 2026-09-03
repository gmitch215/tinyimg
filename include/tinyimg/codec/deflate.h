/**
 * @file deflate.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief DEFLATE and zlib streams, shared by the PNG and TIFF codecs.
 * @version 1.0.0
 * @date 2026-09-02
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/tinyimg.h"
#include "tinyimg/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bytes of history a back reference can reach, which RFC 1951 fixes at
 * 32 KiB.
 */
#define TINY_DEFLATE_WINDOW 32768u

/**
 * @brief Bits the Huffman decoder resolves from one table lookup.
 *
 * Nine covers the overwhelming majority of codes in a real stream; longer ones
 * fall back to walking the canonical code a bit at a time. A table this size
 * costs 1 KiB of scratch and turns a per symbol loop of about eight iterations
 * into a single index, which is the difference between meeting the throughput
 * target for PNG and missing it several times over.
 */
#define TINY_DEFLATE_FAST_BITS 9u

/**
 * @brief Most bytes a single read may ask for.
 *
 * The window is a ring, and a match can add 258 bytes past what the caller
 * asked for, so a request has to leave that much room. A caller wanting more
 * loops.
 */
#define TINY_DEFLATE_MAX_READ (TINY_DEFLATE_WINDOW - 258u)

/**
 * @brief A canonical Huffman decode table.
 *
 * Sized for the literal and length alphabet, which is the largest of the three
 * DEFLATE uses.
 */
typedef struct {
    /** How many codes have each length, 1 through 15. */
    uint16_t counts[16];
    /** Symbols ordered by code length then by value. */
    uint16_t symbols[288];
    /** Symbol in the high bits and code length in the low four, or zero for a
     * miss. */
    uint16_t fast[1u << TINY_DEFLATE_FAST_BITS];
} TinyHuffman;

/**
 * @brief A DEFLATE stream being read.
 *
 * Output is produced on demand into a ring window rather than into one buffer
 * the size of the decompressed data, which is what lets the PNG codec unfilter
 * row by row and keep only the rows a region actually needs.
 *
 * The input has to be contiguous. PNG's compressed data arrives split across
 * IDAT chunks, so the codec concatenates them first; that costs a second copy
 * of the compressed bytes, which is small beside the decoded image and avoids a
 * second bit reader that understands segments.
 */
typedef struct {
    /** The compressed bytes, read least significant bit first. */
    TinyBitReader bits;

    /** Ring of already produced output, TINY_DEFLATE_WINDOW bytes. */
    uint8_t* window;
    /** Where the next produced byte goes. */
    size_t head;
    /** Produced but not yet handed to the caller. */
    size_t pending;
    /** Where the next handed out byte comes from. */
    size_t tail;

    /** Literal and length codes for the block being read. */
    TinyHuffman* literals;
    /** Distance codes for the block being read. */
    TinyHuffman* distances;

    /** Non-zero once the final block's last symbol has been read. */
    int done;
    /** Non-zero while inside a block rather than between blocks. */
    int inside;
    /** Non-zero when the block being read is the last one. */
    int final;
    /** The current block's type, as RFC 1951 numbers them. */
    int mode;
    /** Bytes left in a stored block. */
    size_t stored;
    /** Non-zero when a zlib header was consumed and a trailer is expected. */
    int zlib;
    /** Low half of the running Adler-32, folded in as bytes are handed out. */
    uint32_t adler_a;
    /** High half of the running Adler-32. */
    uint32_t adler_b;
    /** The first failure, latched. */
    int error;
} TinyInflate;

/**
 * @brief Prepares a stream for reading.
 *
 * Takes its window and Huffman tables from the arena, so the caller marks the
 * arena before this and releases afterwards rather than freeing anything by
 * hand.
 *
 * @param state The stream.
 * @param data The compressed bytes.
 * @param size Number of bytes.
 * @param zlib Non-zero to consume a two byte zlib header and verify the
 * Adler-32 trailer, which is what PNG's IDAT and TIFF's Deflate both carry;
 * zero for a bare DEFLATE stream.
 * @return int TINYIMG_OK, TINYIMG_ERR_MEMORY, or TINYIMG_ERR_CORRUPT for a bad
 * zlib header.
 */
int tiny_inflate_init(
    TinyInflate* state, const uint8_t* data, size_t size, int zlib
);

/**
 * @brief Reads decompressed bytes, producing only as many as are asked for.
 *
 * Returns fewer than requested at the end of the stream, and zero once it is
 * exhausted. A caller wanting a fixed length loops until it has it or the count
 * comes back zero.
 *
 * @param state The stream.
 * @param out Where to put the bytes.
 * @param size How many to read, at most TINY_DEFLATE_MAX_READ.
 * @return long Bytes produced, or a negative TinyImageError.
 */
long tiny_inflate_read(TinyInflate* state, uint8_t* out, size_t size);

/**
 * @brief Checks the stream ended cleanly, including a zlib trailer if there was
 * a header.
 *
 * @param state The stream.
 * @return int TINYIMG_OK, or TINYIMG_ERR_CORRUPT for a truncated stream or a
 * bad checksum.
 */
int tiny_inflate_finish(TinyInflate* state);

/**
 * @brief Decompresses a whole stream into a growing sink.
 *
 * For callers with no reason to stream, such as a TIFF strip.
 *
 * @param data The compressed bytes.
 * @param size Number of bytes.
 * @param zlib Non-zero for a zlib wrapper.
 * @param out An initialised sink to append to.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_inflate_all(
    const uint8_t* data, size_t size, int zlib, TinyWriter* out
);

/**
 * @brief How hard the compressor should look for matches.
 *
 * These order by effort, not by output size. A level that searches harder
 * usually produces less, but not always: measured on a photograph's filtered
 * PNG rows, TINYIMG_DEFLATE_HUFFMAN beats TINYIMG_DEFLATE_FAST, because taking
 * one short match per position disturbs the byte distribution more than the
 * match saves. TINYIMG_DEFLATE_DEFAULT and above are past that and monotonic.
 */
typedef enum TinyDeflateLevel
{
    /**
     * @brief Entropy code the input without searching for matches at all.
     *
     * Not a stored block: literals are still Huffman coded, so redundancy in
     * the byte distribution is still taken. The floor for anything already
     * compressed.
     */
    TINYIMG_DEFLATE_HUFFMAN = 0,
    /**
     * @brief One hash chain probe per position, no lazy matching.
     */
    TINYIMG_DEFLATE_FAST = 1,
    /**
     * @brief A bounded chain walk with lazy matching, which is the default.
     */
    TINYIMG_DEFLATE_DEFAULT = 2,
    /**
     * @brief A long chain walk, for when output size matters more than time.
     */
    TINYIMG_DEFLATE_BEST = 3,
} TinyDeflateLevel;

/**
 * @brief Compresses a buffer into a growing sink.
 *
 * Emits dynamic Huffman blocks, falling back to a stored block whenever that
 * would be smaller, so incompressible input never grows by more than the five
 * byte block header.
 *
 * Takes its match tables from the arena, so the caller marks and releases
 * around the call.
 *
 * @param data The bytes to compress.
 * @param size Number of bytes.
 * @param level How hard to look for matches.
 * @param zlib Non-zero to write a zlib header and Adler-32 trailer.
 * @param out An initialised sink to append to.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_deflate(
    const uint8_t* data, size_t size, TinyDeflateLevel level, int zlib,
    TinyWriter* out
);

#ifdef __cplusplus
}
#endif
