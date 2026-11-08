#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tinyimg/image.h"
#include "tinyimg/plan.h"
#include "tinyimg/util.h"

/*
 * Native probe behind scripts/measure/distortion.ts.
 *
 *   lever <width> <file>...             one JSON line per candidate
 *   hash <width> <budget> <file>...     fnv1a of tiny_plan_run, one line per
 * file encode <width> <repeat> <dir> <file>... reference pixels and encodes
 * into dir
 *
 * Built against the ctest static library, so the timings it prints are the
 * native -O2 build and not the shipped module.
 */

static const char* formatName(TinyImageFormat f) {
    switch (f) {
        case TINYIMG_FORMAT_PNG: return "png";
        case TINYIMG_FORMAT_JPEG: return "jpeg";
        case TINYIMG_FORMAT_BMP: return "bmp";
        case TINYIMG_FORMAT_GIF: return "gif";
        case TINYIMG_FORMAT_TIFF: return "tiff";
        case TINYIMG_FORMAT_WEBP: return "webp";
        case TINYIMG_FORMAT_AVIF: return "avif";
        default: return "unknown";
    }
}

static int ok(int result, const char* what) {
    if (result != TINYIMG_OK) fprintf(stderr, "%s returned %d\n", what, result);
    return result == TINYIMG_OK;
}

static const char* baseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static unsigned char* readAll(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) return 0;

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return 0;
    }

    unsigned char* bytes = malloc((size_t) length);
    if (bytes && fread(bytes, 1, (size_t) length, file) != (size_t) length) {
        free(bytes);
        bytes = 0;
    }

    fclose(file);
    *size = (size_t) length;
    return bytes;
}

static unsigned long long fnv1a(const unsigned char* data, size_t size) {
    unsigned long long h = 1469598103934665603ULL;

    for (size_t i = 0; i < size; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }

    return h;
}

static int stepsOf(int base, int den) {
    int steps = 0;

    while (base < den) {
        base *= 2;
        steps++;
    }

    return steps;
}

static int lever(
    const char* path, const unsigned char* bytes, size_t size, uint32_t width
) {
    TinyPlan plan;
    TinyImageInfo info;

    if (!ok(tiny_image_probe(bytes, size, &info),
            "tiny_image_probe(bytes, size, &info)"))
        return 1;
    if (!ok(tiny_plan_init(&plan, bytes, size),
            "tiny_plan_init(&plan, bytes, size)"))
        return 1;
    if (!ok(tiny_plan_resize(&plan, width, 0u),
            "tiny_plan_resize(&plan, width, 0u)"))
        return 1;

    TinyPlanCandidate lattice[TINYIMG_PLAN_MAX_CANDIDATES];
    uint32_t count = 0;
    if (!ok(tiny_plan_enumerate(
                &plan, lattice, TINYIMG_PLAN_MAX_CANDIDATES, &count
            ),
            "tiny_plan_enumerate")) {
        return 1;
    }

    TinyImage reference;
    memset(&reference, 0, sizeof(reference));
    if (!ok(tiny_plan_run_candidate(&plan, &lattice[0], &reference),
            "tiny_plan_run_candidate")) {
        return 1;
    }

    size_t samples =
        (size_t) reference.width * reference.height * reference.channels;

    for (uint32_t k = 0; k < count; k++) {
        TinyImage out;
        memset(&out, 0, sizeof(out));

        if (tiny_plan_run_candidate(&plan, &lattice[k], &out) != TINYIMG_OK) {
            printf(
                "{\"image\":\"%s\",\"width\":%u,\"k\":%u,\"error\":true}\n",
                baseName(path), width, k
            );
            continue;
        }

        if (out.width != reference.width || out.height != reference.height ||
            out.channels != reference.channels) {
            printf(
                "{\"image\":\"%s\",\"width\":%u,\"k\":%u,\"extent_changed\":"
                "true}\n",
                baseName(path), width, k
            );
            tiny_image_destroy(&out);
            continue;
        }

        unsigned long long sq = 0;
        double sq_pm = 0.0;
        int max_abs = 0;
        int alpha = reference.channels == 2 || reference.channels == 4;
        size_t stride = reference.channels;

        for (size_t i = 0; i < samples; i++) {
            int d = (int) out.data[i] - (int) reference.data[i];
            if (d < 0) d = -d;
            if (d > max_abs) max_abs = d;
            sq += (unsigned long long) (d * d);

            if (alpha) {
                size_t a = (i / stride) * stride + stride - 1u;
                int p = i == a ? out.data[i] * 255 : out.data[i] * out.data[a];
                int q = i == a ? reference.data[i] * 255
                               : reference.data[i] * reference.data[a];
                double e = ((double) p - (double) q) / 255.0;
                sq_pm += e * e;
            }
        }

        double db = 99.0;
        if (sq > 0) {
            double mse = (double) sq / (double) samples;
            db = 10.0 * log10(255.0 * 255.0 / mse);
            if (db > 99.0) db = 99.0;
        }

        double db_pm = db;
        if (alpha) {
            db_pm = 99.0;
            if (sq_pm > 0.0) {
                db_pm = 10.0 * log10(255.0 * 255.0 * (double) samples / sq_pm);
                if (db_pm > 99.0) db_pm = 99.0;
            }
        }

        const TinyPlanCandidate* c = &lattice[k];
        printf(
            "{\"image\":\"%s\",\"src_format\":\"%s\",\"src_width\":%u,"
            "\"src_height\":%u,\"width\":%u,\"out_width\":%u,\"out_height\":%u,"
            "\"channels\":%u,\"k\":%u,\"candidates\":%u,\"effort\":%u,"
            "\"nearest\":%u,\"steps\":%u,\"effort_delta\":%d,\"filter_delta\":"
            "%d,\"scale_steps\":%d,\"filter_x\":%d,\"filter_y\":%d,"
            "\"scale_den\":%u,\"cost\":%u,\"peak_bytes\":%u,\"samples\":%zu,"
            "\"identical\":%s,\"db\":%.4f,\"db_pm\":%.4f,\"max_abs\":%d}\n",
            baseName(path), formatName(info.format), info.width, info.height,
            width, out.width, out.height, out.channels, k, count, c->effort,
            c->nearest, c->steps, c->effort != lattice[0].effort,
            c->filter_x != lattice[0].filter_x ||
                c->filter_y != lattice[0].filter_y,
            stepsOf(lattice[0].scale_den, c->scale_den), c->filter_x,
            c->filter_y, c->scale_den, c->cost, c->peak_bytes, samples,
            sq == 0 ? "true" : "false", db, db_pm, max_abs
        );

        tiny_image_destroy(&out);
    }

    tiny_image_destroy(&reference);
    return 0;
}

static int hashRun(
    const char* path, const unsigned char* bytes, size_t size, uint32_t width,
    uint32_t budget
) {
    TinyPlan plan;
    TinyImage out;
    memset(&out, 0, sizeof(out));

    if (!ok(tiny_plan_init(&plan, bytes, size),
            "tiny_plan_init(&plan, bytes, size)"))
        return 1;
    if (!ok(tiny_plan_resize(&plan, width, 0u),
            "tiny_plan_resize(&plan, width, 0u)"))
        return 1;
    if (budget > 0u) tiny_plan_set_budget(&plan, budget);
    if (!ok(tiny_plan_run(&plan, &out), "tiny_plan_run(&plan, &out)")) return 1;

    printf(
        "{\"image\":\"%s\",\"width\":%u,\"budget\":%u,\"w\":%u,\"h\":%u,"
        "\"channels\":%u,\"fnv1a\":\"%016llx\"}\n",
        baseName(path), width, budget, out.width, out.height, out.channels,
        fnv1a(out.data, (size_t) out.width * out.height * out.channels)
    );

    tiny_image_destroy(&out);
    return 0;
}

static double nowMicros(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e6 + (double) t.tv_nsec / 1e3;
}

static int compareDouble(const void* a, const void* b) {
    double x = *(const double*) a;
    double y = *(const double*) b;
    return (x > y) - (x < y);
}

static int encodeRun(
    const char* path, const unsigned char* bytes, size_t size, uint32_t width,
    int repeat, const char* dir
) {
    static const struct {
        TinyImageFormat format;
        const char* ext;
    } targets[] = {
        {TINYIMG_FORMAT_JPEG, "jpg"},  {TINYIMG_FORMAT_WEBP, "webp"},
        {TINYIMG_FORMAT_AVIF, "avif"}, {TINYIMG_FORMAT_PNG, "png"},
        {TINYIMG_FORMAT_GIF, "gif"},
    };
    static const int qualities[] = {50, 60, 70, 75, 80, 85, 90, 95};

    TinyPlan plan;
    TinyImage image;
    memset(&image, 0, sizeof(image));

    if (!ok(tiny_plan_init(&plan, bytes, size),
            "tiny_plan_init(&plan, bytes, size)"))
        return 1;
    if (!ok(tiny_plan_resize(&plan, width, 0u),
            "tiny_plan_resize(&plan, width, 0u)"))
        return 1;
    if (!ok(tiny_plan_run(&plan, &image), "tiny_plan_run(&plan, &image)"))
        return 1;

    char name[1024];
    const char* id = baseName(path);
    size_t pixels = (size_t) image.width * image.height;

    snprintf(name, sizeof(name), "%s/%s.ref", dir, id);
    FILE* ref = fopen(name, "wb");
    if (!ref) return 1;
    fwrite(image.data, 1, pixels * image.channels, ref);
    fclose(ref);

    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        for (size_t q = 0; q < sizeof(qualities) / sizeof(qualities[0]); q++) {
            TinyEncodeOpts opts;
            memset(&opts, 0, sizeof(opts));
            opts.quality = (uint8_t) qualities[q];

            TinyWriter writer;
            tiny_writer_init(&writer, 0);

            int result =
                tiny_image_encode(&image, targets[t].format, &opts, &writer);
            if (result != TINYIMG_OK) {
                printf(
                    "{\"image\":\"%s\",\"format\":\"%s\",\"quality\":%d,"
                    "\"error\":%d}\n",
                    id, formatName(targets[t].format), qualities[q], result
                );
                tiny_writer_free(&writer);
                continue;
            }

            snprintf(
                name, sizeof(name), "%s/%s.%s.q%d.%s", dir, id,
                formatName(targets[t].format), qualities[q], targets[t].ext
            );
            FILE* file = fopen(name, "wb");
            if (!file) return 1;
            fwrite(writer.data, 1, writer.size, file);
            fclose(file);

            size_t out_size = writer.size;
            tiny_writer_free(&writer);

            double median = -1.0;
            if (repeat >= 5) {
                double times[32];
                int n = repeat > 32 ? 32 : repeat;

                for (int r = 0; r < n; r++) {
                    TinyWriter again;
                    tiny_writer_init(&again, 0);

                    double start = nowMicros();
                    tiny_image_encode(&image, targets[t].format, &opts, &again);
                    times[r] = nowMicros() - start;
                    tiny_writer_free(&again);
                }

                qsort(times, (size_t) n, sizeof(double), compareDouble);
                median = times[n / 2];
            }

            printf(
                "{\"image\":\"%s\",\"width\":%u,\"out_width\":%u,"
                "\"out_height\":%u,\"channels\":%u,\"format\":\"%s\","
                "\"quality\":%d,\"bytes\":%zu,\"bpp\":%.6f,\"file\":\"%s\","
                "\"time_us\":",
                id, width, image.width, image.height, image.channels,
                formatName(targets[t].format), qualities[q], out_size,
                (double) out_size / (double) pixels, name
            );
            if (median < 0.0)
                printf("null}\n");
            else
                printf("%.1f}\n", median);
        }
    }

    tiny_image_destroy(&image);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: see the comment at the top of this file\n");
        return 2;
    }

    const char* mode = argv[1];
    uint32_t width = (uint32_t) atoi(argv[2]);
    int first = 3;
    uint32_t budget = 0;
    int repeat = 1;
    const char* dir = 0;

    if (strcmp(mode, "hash") == 0) {
        budget = (uint32_t) atoi(argv[3]);
        first = 4;
    }
    else if (strcmp(mode, "encode") == 0) {
        if (argc < 6) return 2;
        repeat = atoi(argv[3]);
        dir = argv[4];
        first = 5;
    }

    int failures = 0;

    for (int i = first; i < argc; i++) {
        size_t size = 0;
        unsigned char* bytes = readAll(argv[i], &size);

        if (!bytes) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            failures++;
            continue;
        }

        int result = 1;
        if (strcmp(mode, "lever") == 0) {
            result = lever(argv[i], bytes, size, width);
        }
        else if (strcmp(mode, "hash") == 0) {
            result = hashRun(argv[i], bytes, size, width, budget);
        }
        else if (strcmp(mode, "encode") == 0) {
            result = encodeRun(argv[i], bytes, size, width, repeat, dir);
        }

        if (result != 0) {
            fprintf(stderr, "%s: failed at width %u\n", argv[i], width);
            failures++;
        }

        free(bytes);
    }

    return failures == 0 ? 0 : 1;
}
