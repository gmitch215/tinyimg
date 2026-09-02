#include "tinyimg/util.h"

#include "tinyimg/memory.h"
#include "tinyimg/tinyimg.h"

#pragma region bit patterns

static inline uint32_t bits_of(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, sizeof(u));
    return u;
}

static inline float float_of(uint32_t u) {
    float f;
    __builtin_memcpy(&f, &u, sizeof(f));
    return f;
}

#pragma endregion

#pragma region math

// every builtin used here lowers to a single wasm instruction; __builtin_roundf
// does not, so tiny_roundf is written out instead of calling it
float tiny_fabsf(float x) {
    return __builtin_fabsf(x);
}

float tiny_sqrtf(float x) {
    return __builtin_sqrtf(x);
}

float tiny_floorf(float x) {
    return __builtin_floorf(x);
}

float tiny_ceilf(float x) {
    return __builtin_ceilf(x);
}

float tiny_truncf(float x) {
    return __builtin_truncf(x);
}

float tiny_roundf(float x) {
    return __builtin_truncf(x + (x >= 0.0f ? 0.5f : -0.5f));
}

float tiny_fmodf(float x, float y) {
    if (y == 0.0f) return __builtin_nanf("");
    return x - y * __builtin_truncf(x / y);
}

float tiny_expf(float x) {
    // saturating instead of producing subnormals keeps the scale step below to
    // a single exponent insert
    if (x > 87.0f) return __builtin_inff();
    if (x < -87.0f) return 0.0f;

    float k = __builtin_rintf(x * 1.4426950408889634f);

    // ln2 split in two so the reduction keeps its low bits
    float r = (x - k * 0.693359375f) + k * 2.12194440e-4f;
    float p =
        1.0f +
        r * (1.0f +
             r * (0.5f + r * (0.16666667f +
                              r * (0.041666668f +
                                   r * (0.008333334f + r * 0.0013888889f)))));

    uint32_t scale = (uint32_t) (127 + (int32_t) k) << 23;
    return p * float_of(scale);
}

float tiny_logf(float x) {
    if (x < 0.0f) return __builtin_nanf("");
    if (x == 0.0f) return -__builtin_inff();

    int32_t exponent = 0;
    uint32_t u = bits_of(x);

    // a subnormal has no usable exponent field, so scale it into the normals
    if ((u & 0x7F800000u) == 0) {
        x *= 33554432.0f; // 2^25
        exponent = -25;
        u = bits_of(x);
    }

    exponent += (int32_t) ((u >> 23) & 0xFFu) - 127;

    float m = float_of((u & 0x007FFFFFu) | 0x3F800000u);
    if (m > 1.4142135f) {
        m *= 0.5f;
        exponent += 1;
    }

    // 2 * atanh(f / (2 + f)) converges fastest over the halved mantissa range
    float f = m - 1.0f;
    float s = f / (2.0f + f);
    float s2 = s * s;
    float p =
        2.0f * s * (1.0f + s2 * (0.33333333f + s2 * (0.2f + s2 * 0.14285714f)));

    return p + (float) exponent * 0.6931471805599453f;
}

float tiny_powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (x == 1.0f) return 1.0f;

    if (x == 0.0f) return y > 0.0f ? 0.0f : __builtin_inff();

    if (x < 0.0f) {
        float integral = __builtin_truncf(y);
        if (integral != y) return __builtin_nanf("");

        float magnitude = tiny_expf(y * tiny_logf(-x));
        return tiny_fmodf(integral, 2.0f) != 0.0f ? -magnitude : magnitude;
    }

    return tiny_expf(y * tiny_logf(x));
}

static float sin_poly(float r) {
    float r2 = r * r;
    return r * (1.0f + r2 * (-0.16666667f +
                             r2 * (0.008333333f + r2 * (-1.9841270e-4f +
                                                        r2 * 2.7557319e-6f))));
}

static float cos_poly(float r) {
    float r2 = r * r;
    return 1.0f +
           r2 * (-0.5f + r2 * (0.041666668f +
                               r2 * (-0.0013888889f + r2 * 2.4801587e-5f)));
}

// x = k * pi/2 + r with |r| <= pi/4, then the quadrant picks which polynomial
// and which sign applies
static inline float quadrant_reduce(float x, int32_t* quadrant) {
    float k = __builtin_rintf(x * 0.63661977236758134f);
    *quadrant = (int32_t) k & 3;
    return (x - k * 1.5707963f) - k * 7.5497900e-8f;
}

float tiny_sinf(float x) {
    int32_t quadrant;
    float r = quadrant_reduce(x, &quadrant);

    switch (quadrant) {
        case 0: return sin_poly(r);
        case 1: return cos_poly(r);
        case 2: return -sin_poly(r);
        default: return -cos_poly(r);
    }
}

float tiny_cosf(float x) {
    int32_t quadrant;
    float r = quadrant_reduce(x, &quadrant);

    switch (quadrant) {
        case 0: return cos_poly(r);
        case 1: return -sin_poly(r);
        case 2: return -cos_poly(r);
        default: return sin_poly(r);
    }
}

#pragma endregion

#pragma region lookup tables

void tiny_lut_identity(uint8_t* lut) {
    if (!lut) return;

    for (uint32_t i = 0; i < 256; i++) {
        lut[i] = (uint8_t) i;
    }
}

void tiny_lut_gamma(uint8_t* lut, float gamma) {
    if (!lut) return;

    if (gamma <= 0.0f) {
        tiny_lut_identity(lut);
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        float normalized = (float) i * (1.0f / 255.0f);
        lut[i] = tiny_clamp_u8f(tiny_powf(normalized, gamma) * 255.0f);
    }
}

void tiny_lut_compose(
    uint8_t* out, const uint8_t* first, const uint8_t* second
) {
    if (!out || !first || !second) return;

    for (uint32_t i = 0; i < 256; i++) {
        out[i] = second[first[i]];
    }
}

#pragma endregion

#pragma region checksums

static uint32_t crc32_table[256];
static int crc32_ready;

static void crc32_build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;

        for (int bit = 0; bit < 8; bit++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

TINYIMG_EXPORT("tiny_init")
void tiny_init(void) {
    if (!crc32_ready) crc32_build();
    tiny_heap_bootstrap();
}

uint32_t tiny_crc32(uint32_t crc, const uint8_t* data, size_t size) {
    if (!crc32_ready) crc32_build();
    if (!data) return crc;

    uint32_t c = ~crc;
    for (size_t i = 0; i < size; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

uint32_t tiny_adler32(uint32_t adler, const uint8_t* data, size_t size) {
    if (!data) return adler;

    uint32_t a = adler & 0xFFFFu;
    uint32_t b = (adler >> 16) & 0xFFFFu;

    // 5552 is the most bytes that can be summed before b can overflow 32 bits
    while (size > 0) {
        size_t block = size < 5552 ? size : 5552;

        for (size_t i = 0; i < block; i++) {
            a += data[i];
            b += a;
        }

        a %= 65521u;
        b %= 65521u;
        data += block;
        size -= block;
    }

    return (b << 16) | a;
}

#pragma endregion

#pragma region strings

size_t tiny_strlen(const char* s) {
    if (!s) return 0;

    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int tiny_strcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

int tiny_strcopy(char* dest, const char* src, size_t capacity) {
    if (!dest || capacity == 0) return TINYIMG_ERR_NULL;

    if (!src) {
        dest[0] = '\0';
        return TINYIMG_OK;
    }

    size_t i = 0;
    while (src[i] != '\0' && i + 1 < capacity) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';

    return src[i] == '\0' ? TINYIMG_OK : TINYIMG_ERR_BUFFER_TOO_SMALL;
}

#pragma endregion

#pragma region byte writer

TINYIMG_EXPORT("tiny_writer_init")
int tiny_writer_init(TinyWriter* writer, size_t initial) {
    if (!writer) return TINYIMG_ERR_NULL;

    writer->data = 0;
    writer->size = 0;
    writer->capacity = 0;
    writer->error = TINYIMG_OK;

    if (initial > 0) return tiny_writer_reserve(writer, initial);
    return TINYIMG_OK;
}

TINYIMG_EXPORT("tiny_writer_free")
void tiny_writer_free(TinyWriter* writer) {
    if (!writer) return;

    tiny_free(writer->data);
    writer->data = 0;
    writer->size = 0;
    writer->capacity = 0;
}

TINYIMG_EXPORT("tiny_writer_detach")
uint8_t* tiny_writer_detach(TinyWriter* writer, size_t* size) {
    if (!writer || writer->error != TINYIMG_OK) return 0;

    uint8_t* data = writer->data;
    if (size) *size = writer->size;

    writer->data = 0;
    writer->size = 0;
    writer->capacity = 0;

    return data;
}

int tiny_writer_reserve(TinyWriter* writer, size_t extra) {
    if (!writer) return TINYIMG_ERR_NULL;
    if (writer->error != TINYIMG_OK) return writer->error;

    size_t needed = writer->size + extra;
    if (needed < writer->size) {
        writer->error = TINYIMG_ERR_TOO_LARGE;
        return writer->error;
    }
    if (needed <= writer->capacity) return TINYIMG_OK;

    size_t capacity = writer->capacity < 256 ? 256 : writer->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    uint8_t* grown = tiny_realloc(writer->data, capacity);
    if (!grown) {
        writer->error = TINYIMG_ERR_MEMORY;
        return writer->error;
    }

    writer->data = grown;
    writer->capacity = capacity;
    return TINYIMG_OK;
}

int tiny_writer_write(TinyWriter* writer, const void* bytes, size_t size) {
    if (!writer) return TINYIMG_ERR_NULL;
    if (writer->error != TINYIMG_OK) return writer->error;
    if (size == 0) return TINYIMG_OK;
    if (!bytes) {
        writer->error = TINYIMG_ERR_NULL;
        return writer->error;
    }

    int result = tiny_writer_reserve(writer, size);
    if (result != TINYIMG_OK) return result;

    tiny_memcpy(writer->data + writer->size, bytes, size);
    writer->size += size;
    return TINYIMG_OK;
}

int tiny_writer_fill(TinyWriter* writer, uint8_t value, size_t count) {
    if (!writer) return TINYIMG_ERR_NULL;
    if (writer->error != TINYIMG_OK) return writer->error;
    if (count == 0) return TINYIMG_OK;

    int result = tiny_writer_reserve(writer, count);
    if (result != TINYIMG_OK) return result;

    tiny_memset(writer->data + writer->size, value, count);
    writer->size += count;
    return TINYIMG_OK;
}

int tiny_writer_u8(TinyWriter* writer, uint8_t value) {
    if (!writer) return TINYIMG_ERR_NULL;
    if (writer->error != TINYIMG_OK) return writer->error;

    int result = tiny_writer_reserve(writer, 1);
    if (result != TINYIMG_OK) return result;

    writer->data[writer->size++] = value;
    return TINYIMG_OK;
}

int tiny_writer_le16(TinyWriter* writer, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t) (value & 0xFFu), (uint8_t) ((value >> 8) & 0xFFu)
    };
    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

int tiny_writer_le32(TinyWriter* writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) (value & 0xFFu), (uint8_t) ((value >> 8) & 0xFFu),
        (uint8_t) ((value >> 16) & 0xFFu), (uint8_t) ((value >> 24) & 0xFFu)
    };
    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

int tiny_writer_be16(TinyWriter* writer, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t) ((value >> 8) & 0xFFu), (uint8_t) (value & 0xFFu)
    };
    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

int tiny_writer_be32(TinyWriter* writer, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t) ((value >> 24) & 0xFFu), (uint8_t) ((value >> 16) & 0xFFu),
        (uint8_t) ((value >> 8) & 0xFFu), (uint8_t) (value & 0xFFu)
    };
    return tiny_writer_write(writer, bytes, sizeof(bytes));
}

TINYIMG_EXPORT("tiny_writer_sizeof")
uint32_t tiny_writer_sizeof(void) {
    return (uint32_t) sizeof(TinyWriter);
}

TINYIMG_EXPORT("tiny_writer_data")
uint8_t* tiny_writer_data(const TinyWriter* writer) {
    return writer ? writer->data : 0;
}

TINYIMG_EXPORT("tiny_writer_size")
uint32_t tiny_writer_size(const TinyWriter* writer) {
    return writer ? (uint32_t) writer->size : 0;
}

#pragma endregion

#pragma region bit io

#define TINY_BITS_MAX 24u

void tiny_bits_init(TinyBitReader* reader, const uint8_t* data, size_t size) {
    if (!reader) return;

    reader->data = data;
    reader->size = data ? size : 0;
    reader->pos = 0;
    reader->accumulator = 0;
    reader->count = 0;
    reader->phantom = 0;
    reader->overrun = 0;
}

// the accumulator holds at most 31 bits, which is why no call may ask for more
// than 24: 23 held plus one refill is the worst case
static inline void refill_msb(TinyBitReader* reader, uint32_t count) {
    while (reader->count < count) {
        uint8_t byte = 0;
        if (reader->pos < reader->size) {
            byte = reader->data[reader->pos++];
        }
        else {
            reader->phantom += 8;
        }

        reader->accumulator = (reader->accumulator << 8) | byte;
        reader->count += 8;
    }
}

static inline void refill_lsb(TinyBitReader* reader, uint32_t count) {
    while (reader->count < count) {
        uint8_t byte = 0;
        if (reader->pos < reader->size) {
            byte = reader->data[reader->pos++];
        }
        else {
            reader->phantom += 8;
        }

        reader->accumulator |= (uint32_t) byte << reader->count;
        reader->count += 8;
    }
}

// a refill past the end is not itself a failure; consuming those bits is. both
// orders read the oldest bits first and keep the newest last, so the same
// comparison serves each
static inline void consume(TinyBitReader* reader, uint32_t count) {
    if (reader->count - reader->phantom < count) reader->overrun = 1;

    reader->count -= count;
    if (reader->phantom > reader->count) reader->phantom = reader->count;
}

uint32_t tiny_bits_peek_msb(TinyBitReader* reader, uint32_t count) {
    if (!reader || count == 0 || count > TINY_BITS_MAX) return 0;

    refill_msb(reader, count);
    return (reader->accumulator >> (reader->count - count)) &
           ((1u << count) - 1u);
}

uint32_t tiny_bits_msb(TinyBitReader* reader, uint32_t count) {
    uint32_t value = tiny_bits_peek_msb(reader, count);
    if (reader && count > 0 && count <= TINY_BITS_MAX) consume(reader, count);
    return value;
}

uint32_t tiny_bits_peek_lsb(TinyBitReader* reader, uint32_t count) {
    if (!reader || count == 0 || count > TINY_BITS_MAX) return 0;

    refill_lsb(reader, count);
    return reader->accumulator & ((1u << count) - 1u);
}

uint32_t tiny_bits_lsb(TinyBitReader* reader, uint32_t count) {
    uint32_t value = tiny_bits_peek_lsb(reader, count);
    if (reader && count > 0 && count <= TINY_BITS_MAX) {
        reader->accumulator >>= count;
        consume(reader, count);
    }
    return value;
}

void tiny_bits_skip_msb(TinyBitReader* reader, uint32_t count) {
    if (!reader) return;

    while (count > 0) {
        uint32_t step = count > TINY_BITS_MAX ? TINY_BITS_MAX : count;
        tiny_bits_msb(reader, step);
        count -= step;
    }
}

void tiny_bits_skip_lsb(TinyBitReader* reader, uint32_t count) {
    if (!reader) return;

    while (count > 0) {
        uint32_t step = count > TINY_BITS_MAX ? TINY_BITS_MAX : count;
        tiny_bits_lsb(reader, step);
        count -= step;
    }
}

void tiny_bits_align_msb(TinyBitReader* reader) {
    if (!reader) return;

    // the unread bits sit at the top, so dropping the partial byte is a
    // shortened count plus a mask over what is left
    uint32_t partial = reader->count & 7u;
    reader->count -= partial;
    reader->accumulator &= (1u << reader->count) - 1u;

    if (reader->phantom > reader->count) reader->phantom = reader->count;
}

void tiny_bits_align_lsb(TinyBitReader* reader) {
    if (!reader) return;

    uint32_t partial = reader->count & 7u;
    reader->accumulator >>= partial;
    reader->count -= partial;

    if (reader->phantom > reader->count) reader->phantom = reader->count;
}

size_t tiny_bits_remaining(const TinyBitReader* reader) {
    if (!reader) return 0;

    size_t buffered = reader->count / 8u;
    size_t left = reader->size - reader->pos;
    return left + buffered;
}

void tiny_bitwriter_init(TinyBitWriter* writer, TinyWriter* out) {
    if (!writer) return;

    writer->out = out;
    writer->accumulator = 0;
    writer->count = 0;
}

void tiny_bitwriter_msb(TinyBitWriter* writer, uint32_t value, uint32_t count) {
    if (!writer || !writer->out || count == 0 || count > TINY_BITS_MAX) return;

    value &= (1u << count) - 1u;
    writer->accumulator = (writer->accumulator << count) | value;
    writer->count += count;

    while (writer->count >= 8) {
        writer->count -= 8;
        tiny_writer_u8(
            writer->out,
            (uint8_t) ((writer->accumulator >> writer->count) & 0xFFu)
        );
    }
}

void tiny_bitwriter_lsb(TinyBitWriter* writer, uint32_t value, uint32_t count) {
    if (!writer || !writer->out || count == 0 || count > TINY_BITS_MAX) return;

    value &= (1u << count) - 1u;
    writer->accumulator |= value << writer->count;
    writer->count += count;

    while (writer->count >= 8) {
        tiny_writer_u8(writer->out, (uint8_t) (writer->accumulator & 0xFFu));
        writer->accumulator >>= 8;
        writer->count -= 8;
    }
}

void tiny_bitwriter_flush_msb(TinyBitWriter* writer) {
    if (!writer || !writer->out) return;

    if (writer->count > 0) {
        uint32_t pad = 8 - writer->count;
        tiny_bitwriter_msb(writer, (1u << pad) - 1u, pad);
    }

    writer->accumulator = 0;
    writer->count = 0;
}

void tiny_bitwriter_flush_lsb(TinyBitWriter* writer) {
    if (!writer || !writer->out) return;

    if (writer->count > 0) {
        tiny_writer_u8(writer->out, (uint8_t) (writer->accumulator & 0xFFu));
    }

    writer->accumulator = 0;
    writer->count = 0;
}

#pragma endregion
