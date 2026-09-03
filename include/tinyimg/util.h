/**
 * @file util.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Math shims, lookup table builders, checksums and bit level IO.
 * @version 1.0.0
 * @date 2026-09-01
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

#pragma region math

/**
 * @brief Absolute value of a float.
 *
 * @param x The value.
 * @return float |x|.
 */
float tiny_fabsf(float x);

/**
 * @brief Square root of a float.
 *
 * @param x The value. Negative inputs produce NaN.
 * @return float The square root of x.
 */
float tiny_sqrtf(float x);

/**
 * @brief Largest integral value not greater than x.
 *
 * @param x The value.
 * @return float floor(x).
 */
float tiny_floorf(float x);

/**
 * @brief Smallest integral value not less than x.
 *
 * @param x The value.
 * @return float ceil(x).
 */
float tiny_ceilf(float x);

/**
 * @brief Integral part of x, discarding the fraction.
 *
 * @param x The value.
 * @return float trunc(x).
 */
float tiny_truncf(float x);

/**
 * @brief Nearest integral value, halfway cases rounded away from zero.
 *
 * @param x The value.
 * @return float round(x).
 */
float tiny_roundf(float x);

/**
 * @brief Floating point remainder of x/y, with the sign of x.
 *
 * Computed as x - y * trunc(x / y), so the result loses precision once |x / y|
 * grows past the mantissa. Angles and other small ratios are unaffected.
 *
 * @param x The dividend.
 * @param y The divisor. Zero produces NaN.
 * @return float The remainder.
 */
float tiny_fmodf(float x, float y);

/**
 * @brief Exponential of x.
 *
 * Worst measured relative error is 2.2e-7 over -30 to 30, or two units in the
 * last place. Saturates rather than producing subnormals: inputs above 87
 * return infinity and inputs below -87 return zero.
 *
 * @param x The exponent.
 * @return float e raised to x.
 */
float tiny_expf(float x);

/**
 * @brief Natural logarithm of x.
 *
 * Worst measured error over 1e-6 to 1e6 is 5.8e-7 absolute and 2.4e-7 relative.
 * Relative error is unbounded as the result approaches zero at x = 1, which is
 * a property of the function rather than of this implementation, so the
 * absolute figure is the useful one. Subnormal inputs are handled by scaling.
 *
 * @param x The value. Zero returns negative infinity and negatives return NaN.
 * @return float The natural log of x.
 */
float tiny_logf(float x);

/**
 * @brief x raised to the power y.
 *
 * Evaluated as exp(y * log(x)), so the relative error grows with |y * log(x)|.
 * Worst measured relative error is 1.3e-6 at an exponent of 2.2 over 0 to 1,
 * which is the gamma case; that is a quarter of one step of an 8 bit channel.
 * A negative base is only defined for integral exponents and returns NaN
 * otherwise.
 *
 * @param x The base.
 * @param y The exponent.
 * @return float x to the power y.
 */
float tiny_powf(float x, float y);

/**
 * @brief Sine of x.
 *
 * Worst measured absolute error is 9.1e-8 over -20 to 20. Argument reduction
 * uses a two part pi/2, which holds to roughly |x| < 1e5 radians.
 *
 * @param x The angle in radians.
 * @return float sin(x).
 */
float tiny_sinf(float x);

/**
 * @brief Cosine of x.
 *
 * Worst measured absolute error is 9.1e-8 over -20 to 20. Argument reduction
 * uses a two part pi/2, which holds to roughly |x| < 1e5 radians.
 *
 * @param x The angle in radians.
 * @return float cos(x).
 */
float tiny_cosf(float x);

/**
 * @brief Clamps a signed integer into the 0-255 range of one 8 bit channel.
 *
 * @param v The value.
 * @return uint8_t v clamped to 0-255.
 */
static inline uint8_t tiny_clamp_u8(int32_t v) {
    return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

/**
 * @brief Rounds and clamps a float into the 0-255 range of one 8 bit channel.
 *
 * @param v The value.
 * @return uint8_t v rounded to the nearest integer and clamped to 0-255.
 */
static inline uint8_t tiny_clamp_u8f(float v) {
    return (uint8_t) (v <= 0.0f ? 0
                                : (v >= 255.0f ? 255 : (int32_t) (v + 0.5f)));
}

/**
 * @brief Clamps a float into an inclusive range.
 *
 * @param v The value.
 * @param lo Lower bound.
 * @param hi Upper bound.
 * @return float v clamped to lo-hi.
 */
static inline float tiny_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * @brief Clamps a signed integer into an inclusive range.
 *
 * @param v The value.
 * @param lo Lower bound.
 * @param hi Upper bound.
 * @return int32_t v clamped to lo-hi.
 */
static inline int32_t tiny_clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * @brief Smaller of two unsigned values.
 *
 * @param a First value.
 * @param b Second value.
 * @return uint32_t The smaller of a and b.
 */
static inline uint32_t tiny_min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/**
 * @brief Larger of two unsigned values.
 *
 * @param a First value.
 * @param b Second value.
 * @return uint32_t The larger of a and b.
 */
static inline uint32_t tiny_max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

#pragma endregion

#pragma region lookup tables

/**
 * @brief Fills a 256 entry table with the identity mapping.
 *
 * @param lut Table to fill.
 */
void tiny_lut_identity(uint8_t* lut);

/**
 * @brief Fills a 256 entry table with a gamma curve.
 *
 * Values above 1 darken the midtones and values below 1 brighten them, matching
 * the sense of tiny_image_gamma_correction.
 *
 * @param lut Table to fill.
 * @param gamma The exponent. Values at or below zero leave the table as the
 * identity.
 */
void tiny_lut_gamma(uint8_t* lut, float gamma);

/**
 * @brief Composes two 256 entry tables into one.
 *
 * The result applies first and then second, which is what lets a chain of point
 * operations collapse into a single table.
 *
 * @param out Table to fill. May alias `first`, since each entry is read before
 * it is written; aliasing `second` reads entries that have already been
 * overwritten.
 * @param first The table applied first.
 * @param second The table applied second.
 */
void tiny_lut_compose(
    uint8_t* out, const uint8_t* first, const uint8_t* second
);

#pragma endregion

#pragma region checksums

/**
 * @brief Computes a running CRC-32 over a buffer.
 *
 * Uses the reflected 0xEDB88320 polynomial and the zlib calling convention, so
 * passing 0 for the first call produces the value PNG chunks and gzip trailers
 * carry. The 1 KiB table is generated on first use rather than shipped.
 *
 * O(n) time complexity, where n is size.
 *
 * @param crc Running value, 0 to start a new checksum.
 * @param data Bytes to fold in. May be NULL when size is 0.
 * @param size Number of bytes.
 * @return uint32_t The updated checksum.
 */
uint32_t tiny_crc32(uint32_t crc, const uint8_t* data, size_t size);

/**
 * @brief Computes a running Adler-32 over a buffer.
 *
 * Uses the zlib calling convention, so passing 1 for the first call produces
 * the value a zlib stream trailer carries.
 *
 * O(n) time complexity, where n is size.
 *
 * @param adler Running value, 1 to start a new checksum.
 * @param data Bytes to fold in. May be NULL when size is 0.
 * @param size Number of bytes.
 * @return uint32_t The updated checksum.
 */
uint32_t tiny_adler32(uint32_t adler, const uint8_t* data, size_t size);

#pragma endregion

#pragma region strings

/**
 * @brief Length of a NUL terminated string.
 *
 * @param s The string.
 * @return size_t The number of bytes before the terminator, or 0 if s is NULL.
 */
size_t tiny_strlen(const char* s);

/**
 * @brief Compares two NUL terminated strings.
 *
 * @param a First string.
 * @param b Second string.
 * @return int Zero when equal, otherwise the signed difference of the first
 * differing byte.
 */
int tiny_strcmp(const char* a, const char* b);

/**
 * @brief Copies a string into a fixed buffer, always terminating it.
 *
 * Truncates rather than overrunning when the source does not fit.
 *
 * @param dest Destination buffer.
 * @param src Source string.
 * @param capacity Size of dest in bytes, including the terminator.
 * @return int TINYIMG_OK, or TINYIMG_ERR_BUFFER_TOO_SMALL when src was
 * truncated.
 */
int tiny_strcopy(char* dest, const char* src, size_t capacity);

#pragma endregion

#pragma region huffman

/**
 * @brief Builds length limited Huffman code lengths from symbol frequencies.
 *
 * Shared by every encoder that writes a prefix code, which is DEFLATE for its
 * literals, its distances and its own code length alphabet, and WebP lossless
 * for all five of its alphabets. They differ only in how many symbols they have
 * and how deep a code they can express.
 *
 * The lengths are canonical: the caller turns them into codes, and a symbol
 * whose frequency is zero gets a length of zero and no code. An alphabet with
 * one used symbol gets that symbol a length of one, which a canonical builder
 * can express and which the two formats then treat differently.
 *
 * O(n^2) time complexity in the alphabet size, because the two lowest weight
 * nodes are found by scanning rather than through a heap. At the 536 symbols
 * the larger caller reaches that is about 143k comparisons, against a heap's
 * 5k, and it is a third of the code; past a few thousand symbols the trade
 * stops being worth it, which is one reason WebP's colour cache is capped where
 * it is.
 *
 * @param frequencies How often each symbol occurs, `count` entries.
 * @param count Symbols in the alphabet.
 * @param limit Longest code the format can express, in bits.
 * @param lengths Receives `count` lengths, zero for an unused symbol.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_RANGE for a zero count
 * or limit, or TINYIMG_ERR_MEMORY when the arena has no room for the tree.
 */
int tiny_huffman_lengths(
    const uint32_t* frequencies, uint32_t count, uint32_t limit,
    uint8_t* lengths
);

#pragma endregion

#pragma region byte writer

/**
 * @brief A grow on demand byte sink.
 *
 * Encoders write into one of these so no caller has to guess an output size.
 * Writes latch the first failure into `error` and then do nothing, which means
 * an encoder can emit a whole file and check once at the end.
 */
typedef struct {
    /** The bytes written so far, or NULL before the first write. */
    uint8_t* data;
    /** How many bytes have been written. */
    size_t size;
    /** How many bytes `data` can hold before it has to grow. */
    size_t capacity;
    /** TINYIMG_OK, or the first failure, after which writes do nothing. */
    int error;
} TinyWriter;

/**
 * @brief Prepares a writer, optionally reserving space up front.
 *
 * @param writer The writer.
 * @param initial Bytes to reserve. Zero defers the first allocation to the
 * first write.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_init(TinyWriter* writer, size_t initial);

/**
 * @brief Releases a writer's buffer.
 *
 * @param writer The writer. Safe to call twice.
 */
void tiny_writer_free(TinyWriter* writer);

/**
 * @brief Detaches a writer's buffer, handing ownership to the caller.
 *
 * The writer is left empty. The caller releases the buffer with tiny_free.
 *
 * @param writer The writer.
 * @param size Receives the byte length.
 * @return uint8_t* The buffer, or NULL when the writer holds an error.
 */
uint8_t* tiny_writer_detach(TinyWriter* writer, size_t* size);

/**
 * @brief Reserves capacity for at least `extra` more bytes.
 *
 * @param writer The writer.
 * @param extra Additional bytes needed.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_reserve(TinyWriter* writer, size_t extra);

/**
 * @brief Appends raw bytes.
 *
 * @param writer The writer.
 * @param bytes Source buffer.
 * @param size Number of bytes.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_write(TinyWriter* writer, const void* bytes, size_t size);

/**
 * @brief Appends the same byte repeatedly.
 *
 * @param writer The writer.
 * @param value The byte.
 * @param count How many times to append it.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_fill(TinyWriter* writer, uint8_t value, size_t count);

/**
 * @brief Appends one byte.
 *
 * @param writer The writer.
 * @param value The byte.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_u8(TinyWriter* writer, uint8_t value);

/**
 * @brief Appends a 16 bit value, little endian.
 *
 * @param writer The writer.
 * @param value The value.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_le16(TinyWriter* writer, uint16_t value);

/**
 * @brief Appends a 32 bit value, little endian.
 *
 * @param writer The writer.
 * @param value The value.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_le32(TinyWriter* writer, uint32_t value);

/**
 * @brief Appends a 16 bit value, big endian.
 *
 * @param writer The writer.
 * @param value The value.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_be16(TinyWriter* writer, uint16_t value);

/**
 * @brief Appends a 32 bit value, big endian.
 *
 * @param writer The writer.
 * @param value The value.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_writer_be32(TinyWriter* writer, uint32_t value);

/**
 * @brief Size of a TinyWriter, for a host allocating one across the ABI.
 *
 * @return uint32_t sizeof(TinyWriter).
 */
uint32_t tiny_writer_sizeof(void);

/**
 * @brief Pointer to a writer's bytes.
 *
 * @param writer The writer.
 * @return uint8_t* The buffer, or NULL when nothing has been written.
 */
uint8_t* tiny_writer_data(const TinyWriter* writer);

/**
 * @brief Number of bytes a writer holds.
 *
 * @param writer The writer.
 * @return uint32_t The byte length.
 */
uint32_t tiny_writer_size(const TinyWriter* writer);

#pragma endregion

#pragma region bit io

/**
 * @brief Reads a bitstream out of a byte buffer.
 *
 * A stream is either most significant bit first (JPEG) or least significant bit
 * first (DEFLATE, LZW, VP8L). One reader serves both, but a single stream must
 * be read with one family of calls throughout; mixing them mid stream reads the
 * accumulator in the wrong order.
 *
 * Reading past the end yields zero bits and latches `overrun`, so a truncated
 * file is detected once at the end of a scan rather than checked per symbol.
 *
 * `overrun` means a read consumed bits that are not in the buffer, not merely
 * that the accumulator refilled past the end. The two are different: the
 * accumulator pulls whole bytes ahead of need, so decoding the last symbol of a
 * stream that ends on a byte boundary refills past the end while every bit it
 * uses is real. Latching on the refill instead reports a valid stream as
 * truncated, which is what a bare DEFLATE stream with no trailer after it does.
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
    /**
     * @brief How many of those bits came from past the end of the buffer.
     *
     * Always the newest bits, which both orders keep at the end reads reach
     * last, so one comparison serves both. An implementation detail.
     */
    uint32_t phantom;
    /** Set once a read has consumed bits that are not in the buffer. */
    int overrun;
} TinyBitReader;

/**
 * @brief Points a bit reader at a buffer.
 *
 * @param reader The reader.
 * @param data The bytes.
 * @param size Number of bytes.
 */
void tiny_bits_init(TinyBitReader* reader, const uint8_t* data, size_t size);

/**
 * @brief Reads bits, most significant first.
 *
 * @param reader The reader.
 * @param count How many bits to read. Must be 24 or fewer.
 * @return uint32_t The bits, right aligned.
 */
uint32_t tiny_bits_msb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Reads bits without consuming them, most significant first.
 *
 * Used by Huffman decoders, which look at the longest possible code and then
 * skip only the bits the matched symbol actually used.
 *
 * @param reader The reader.
 * @param count How many bits to peek. Must be 24 or fewer.
 * @return uint32_t The bits, right aligned.
 */
uint32_t tiny_bits_peek_msb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Reads bits, least significant first.
 *
 * @param reader The reader.
 * @param count How many bits to read. Must be 24 or fewer.
 * @return uint32_t The bits, right aligned.
 */
uint32_t tiny_bits_lsb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Reads bits without consuming them, least significant first.
 *
 * @param reader The reader.
 * @param count How many bits to peek. Must be 24 or fewer.
 * @return uint32_t The bits, right aligned.
 */
uint32_t tiny_bits_peek_lsb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Discards bits already examined with a most significant first peek.
 *
 * @param reader The reader.
 * @param count How many bits to drop.
 */
void tiny_bits_skip_msb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Discards bits already examined with a least significant first peek.
 *
 * @param reader The reader.
 * @param count How many bits to drop.
 */
void tiny_bits_skip_lsb(TinyBitReader* reader, uint32_t count);

/**
 * @brief Drops the partial byte at the read head of a most significant first
 * stream.
 *
 * JPEG needs this after a restart marker. The two orders keep their unread bits
 * at opposite ends of the accumulator, so aligning is not a shared operation.
 *
 * @param reader The reader.
 */
void tiny_bits_align_msb(TinyBitReader* reader);

/**
 * @brief Drops the partial byte at the read head of a least significant first
 * stream.
 *
 * DEFLATE needs this before a stored block's length field.
 *
 * @param reader The reader.
 */
void tiny_bits_align_lsb(TinyBitReader* reader);

/**
 * @brief Bytes not yet consumed, counting only whole bytes past the head.
 *
 * @param reader The reader.
 * @return size_t The remaining byte count.
 */
size_t tiny_bits_remaining(const TinyBitReader* reader);

/**
 * @brief Writes a bitstream into a TinyWriter.
 *
 * The mirror of TinyBitReader, and subject to the same rule: pick one bit order
 * and stay with it for the whole stream. Bits buffered when the last value does
 * not land on a byte boundary are only emitted by a flush.
 */
typedef struct {
    /** The sink whole bytes are appended to. */
    TinyWriter* out;
    /** Bits not yet part of a whole byte, an implementation detail. */
    uint32_t accumulator;
    /** How many bits the accumulator holds, an implementation detail. */
    uint32_t count;
} TinyBitWriter;

/**
 * @brief Points a bit writer at a byte sink.
 *
 * @param writer The bit writer.
 * @param out The sink, which must outlive the bit writer.
 */
void tiny_bitwriter_init(TinyBitWriter* writer, TinyWriter* out);

/**
 * @brief Writes bits, most significant first.
 *
 * @param writer The bit writer.
 * @param value The bits, right aligned.
 * @param count How many bits to write. Must be 24 or fewer.
 */
void tiny_bitwriter_msb(TinyBitWriter* writer, uint32_t value, uint32_t count);

/**
 * @brief Writes bits, least significant first.
 *
 * @param writer The bit writer.
 * @param value The bits, right aligned.
 * @param count How many bits to write. Must be 24 or fewer.
 */
void tiny_bitwriter_lsb(TinyBitWriter* writer, uint32_t value, uint32_t count);

/**
 * @brief Flushes a most significant bit first stream, padding with one bits.
 *
 * One bits are the padding JPEG entropy coded segments use.
 *
 * @param writer The bit writer.
 */
void tiny_bitwriter_flush_msb(TinyBitWriter* writer);

/**
 * @brief Flushes a least significant bit first stream, padding with zero bits.
 *
 * @param writer The bit writer.
 */
void tiny_bitwriter_flush_lsb(TinyBitWriter* writer);

#pragma endregion

#ifdef __cplusplus
}
#endif
