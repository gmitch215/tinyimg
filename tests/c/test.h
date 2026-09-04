#ifndef TINYIMG_TEST_H
#define TINYIMG_TEST_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyimg/image.h"

#ifndef TINYIMG_FIXTURE_DIR
    #define TINYIMG_FIXTURE_DIR "tests/fixtures"
#endif

static int count = 1;

static inline int assertTrue(int condition) {
    printf("#%d: %s\n", count, condition ? "PASS" : "FAIL");
    if (!condition) {
        printf("#%d: had false, wanted true\n", count);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertFalse(int condition) {
    printf("#%d: %s\n", count, !condition ? "PASS" : "FAIL");
    if (condition) {
        printf("#%d: had true, wanted false\n", count);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertNull(const void* ptr) {
    printf("#%d: %s\n", count, ptr == 0 ? "PASS" : "FAIL");
    if (ptr != 0) {
        printf("#%d: expected <%p> to be null\n", count, ptr);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertNotNull(const void* ptr) {
    printf("#%d: %s\n", count, ptr != 0 ? "PASS" : "FAIL");
    if (ptr == 0) {
        printf("#%d: expected a pointer, got null\n", count);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertEquals(long a, long b) {
    printf("#%d: %s\n", count, a == b ? "PASS" : "FAIL");
    if (a != b) {
        printf("#%d: had <%ld>, wanted <%ld>\n", count, a, b);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertNotEquals(long a, long b) {
    printf("#%d: %s\n", count, a != b ? "PASS" : "FAIL");
    if (a == b) {
        printf("#%d: had <%ld>, didn't want <%ld>\n", count, a, b);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertFloatEquals(float a, float b, float epsilon) {
    printf("#%d: %s\n", count, fabsf(a - b) <= epsilon ? "PASS" : "FAIL");
    if (fabsf(a - b) > epsilon) {
        printf("#%d: had <%f>, wanted <%f> (+/- %f)\n", count, a, b, epsilon);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertGreaterThan(double a, double b) {
    printf("#%d: %s\n", count, a > b ? "PASS" : "FAIL");
    if (a <= b) {
        printf("#%d: wanted <%f> to be greater than <%f>\n", count, a, b);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertLessThan(double a, double b) {
    printf("#%d: %s\n", count, a < b ? "PASS" : "FAIL");
    if (a >= b) {
        printf("#%d: wanted <%f> to be less than <%f>\n", count, a, b);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertIn(double value, double min, double max) {
    printf("#%d: %s\n", count, value >= min && value <= max ? "PASS" : "FAIL");
    if (value < min || value > max) {
        printf(
            "#%d: expected <%f> to be in range <%f> to <%f>\n", count, value,
            min, max
        );
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertStringsMatch(const char* a, const char* b) {
    printf("#%d: %s\n", count, strcmp(a, b) == 0 ? "PASS" : "FAIL");
    if (strcmp(a, b) != 0) {
        printf("#%d: expected strings <%s> and <%s> to match\n", count, a, b);
        count++;
        return 1;
    }

    count++;
    return 0;
}

static inline int assertBytesMatch(const void* a, const void* b, size_t n) {
    const unsigned char* p = (const unsigned char*) a;
    const unsigned char* q = (const unsigned char*) b;

    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) {
            printf("#%d: FAIL\n", count);
            printf(
                "#%d: byte %zu of %zu: had <%u>, wanted <%u>\n", count, i, n,
                p[i], q[i]
            );
            count++;
            return 1;
        }
    }

    printf("#%d: PASS\n", count);
    count++;
    return 0;
}

#pragma region fixtures

/**
 * @brief Reads a fixture into a freshly malloc'd buffer.
 *
 * Resolves against the fixture directory the build passes in, so a test names
 * the file and nothing else. The caller frees the result.
 *
 * @param name File name inside tests/fixtures, e.g. "sf-24.jpg".
 * @param size Receives the byte length on success.
 * @return unsigned char* The bytes, or NULL if the file could not be read.
 */
static inline unsigned char* readFixture(const char* name, size_t* size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", TINYIMG_FIXTURE_DIR, name);

    FILE* file = fopen(path, "rb");
    if (!file) {
        printf("could not open fixture: %s\n", path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long length = ftell(file);
    if (length <= 0) {
        fclose(file);
        return 0;
    }
    rewind(file);

    unsigned char* buffer = (unsigned char*) malloc((size_t) length);
    if (!buffer) {
        fclose(file);
        return 0;
    }

    size_t read = fread(buffer, 1, (size_t) length, file);
    fclose(file);

    if (read != (size_t) length) {
        free(buffer);
        return 0;
    }

    *size = read;
    return buffer;
}

#pragma endregion

#pragma region image assertions

/**
 * @brief Computes the peak signal-to-noise ratio between two pixel buffers.
 *
 * Identical buffers have no error to report, so they come back as infinity;
 * every caller compares against a floor and passes.
 *
 * @param a First pixel buffer.
 * @param b Second pixel buffer.
 * @param n Byte length of both buffers.
 * @return double PSNR in decibels, or INFINITY when the buffers are identical.
 */
static inline double computePSNR(
    const unsigned char* a, const unsigned char* b, size_t n
) {
    double sum = 0.0;

    for (size_t i = 0; i < n; i++) {
        double diff = (double) a[i] - (double) b[i];
        sum += diff * diff;
    }

    if (sum == 0.0) return INFINITY;

    double mse = sum / (double) n;
    return 10.0 * log10((255.0 * 255.0) / mse);
}

static inline int assertPSNR(
    const unsigned char* a, const unsigned char* b, size_t n, double floorDb
) {
    double psnr = computePSNR(a, b, n);
    int ok = psnr >= floorDb;

    printf("#%d: %s\n", count, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf(
            "#%d: psnr %.2f dB is below the %.2f dB floor\n", count, psnr,
            floorDb
        );
        count++;
        return 1;
    }

    printf("#%d: psnr %.2f dB\n", count, psnr);
    count++;
    return 0;
}

/**
 * @brief Asserts two images have the same dimensions, channel count and pixels.
 *
 * @param a First image.
 * @param b Second image.
 * @return int 0 when they match, 1 otherwise.
 */
static inline int assertImageEquals(const TinyImage* a, const TinyImage* b) {
    if (!a || !b) {
        printf("#%d: FAIL\n", count);
        printf("#%d: one of the images is null\n", count);
        count++;
        return 1;
    }

    if (a->width != b->width || a->height != b->height ||
        a->channels != b->channels) {
        printf("#%d: FAIL\n", count);
        printf(
            "#%d: had %ux%ux%u, wanted %ux%ux%u\n", count, a->width, a->height,
            a->channels, b->width, b->height, b->channels
        );
        count++;
        return 1;
    }

    size_t n = (size_t) a->width * a->height * a->channels;
    return assertBytesMatch(a->data, b->data, n);
}

/**
 * @brief Asserts an image equals the same rectangle taken out of a larger one.
 *
 * Which is how a region decode is checked: the codec produces the part and the
 * assertion holds it against the whole, so a wrong offset, a wrong stride or a
 * truncated pass is a mismatch rather than a plausible looking picture.
 *
 * @param part The smaller image.
 * @param whole The image it should be a rectangle of.
 * @param x Left edge of that rectangle in `whole`.
 * @param y Top edge of that rectangle in `whole`.
 * @return int 0 when they match, 1 otherwise.
 */
static inline int assertMatchesCrop(
    const TinyImage* part, const TinyImage* whole, uint32_t x, uint32_t y
) {
    if (part->width + x > whole->width || part->height + y > whole->height) {
        printf(
            "crop %ux%u+%u+%u does not fit\n", part->width, part->height, x, y
        );
        return assertTrue(0);
    }

    for (uint32_t row = 0; row < part->height; row++) {
        const uint8_t* a =
            part->data + (size_t) row * part->width * part->channels;
        const uint8_t* b =
            whole->data +
            ((size_t) (y + row) * whole->width + x) * whole->channels;

        for (uint32_t i = 0; i < part->width * part->channels; i++) {
            if (a[i] != b[i]) {
                printf(
                    "row %u byte %u: had %u, wanted %u\n", row, i, a[i], b[i]
                );
                return assertTrue(0);
            }
        }
    }

    return assertTrue(1);
}

#pragma endregion

#endif
