#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyimg/image.h"
#include "tinyimg/plan.h"
#include "tinyimg/select.h"

/*
 * Native probe behind scripts/measure/selector-ablation.ts.
 *
 *   select-probe <width> <budgets-us> <min-dbs> <file>...
 *
 * For each file, each budget in the comma list (0 is no limit) and each dB
 * floor in the comma list (0 is no floor), writes one JSON line per arm
 * (joint, ladder, independent) with the choice and its predicted figures.
 * The formats mask is the source's own, JPEG and WebP, the objective is
 * fewest bytes, uncharacterized candidates are refused, and the machine factor
 * is 1. The three search functions are native-only, so this is the only way
 * to reach the ladder and independent arms.
 */

typedef int (*SelectFn)(
    const TinyPlan* plan, const TinySelectOpts* opts, TinySelection* selection
);

static const char* formatName(uint32_t f) {
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

static size_t parseList(const char* text, double* out, size_t max) {
    size_t n = 0;
    char* end;

    while (*text && n < max) {
        out[n++] = strtod(text, &end);
        if (end == text) break;
        text = *end == ',' ? end + 1 : end;
    }

    return n;
}

static void emit(
    const char* image, uint32_t width, uint32_t budgetUs, double minDb,
    const char* arm, int result, const TinySelection* s
) {
    const TinySelectConsidered* c = &s->choice;

    if (result != TINYIMG_OK) {
        printf(
            "{\"image\":\"%s\",\"width\":%u,\"budget_us\":%u,\"min_db\":%g,"
            "\"arm\":\"%s\",\"error\":%d}\n",
            image, width, budgetUs, minDb, arm, result
        );
        return;
    }

    printf(
        "{\"image\":\"%s\",\"width\":%u,\"budget_us\":%u,\"min_db\":%g,"
        "\"arm\":\"%s\",\"feasible\":%u,\"reason\":%u,\"candidate\":%u,"
        "\"format\":\"%s\",\"quality\":%u,\"lossless\":%u,"
        "\"cost_upper_us\":%u,\"peak\":%u,\"db\":%.4f,\"db_known\":%u,"
        "\"db_median\":%.4f,\"bytes_per_pixel\":%.6f,\"bytes_known\":%u,"
        "\"predicted_bytes\":%u,\"out_width\":%u,\"out_height\":%u,"
        "\"alpha\":%u,\"lattice\":%u,\"considered\":%u}\n",
        image, width, budgetUs, minDb, arm, s->feasible, s->reason,
        c->candidate, s->feasible ? formatName(c->format) : "none", c->quality,
        c->lossless, c->cost_upper_us, c->peak_bytes, (double) c->db,
        c->db_known, (double) s->db_median, (double) c->bytes_per_pixel,
        c->bytes_known, s->predicted_bytes, s->width, s->height, s->alpha,
        s->lattice, s->count
    );
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(
            stderr,
            "usage: select-probe <width> <budgets-us> <min-dbs> <file>...\n"
        );
        return 2;
    }

    uint32_t width = (uint32_t) strtoul(argv[1], 0, 10);
    double budgets[16];
    double dbs[16];
    size_t nb = parseList(argv[2], budgets, 16);
    size_t nd = parseList(argv[3], dbs, 16);
    static const struct {
        const char* name;
        SelectFn fn;
    } arms[] = {
        {"joint", tiny_plan_select},
        {"ladder", tiny_plan_select_ladder},
        {"independent", tiny_plan_select_independent},
    };
    static TinySelection selection;
    int status = 0;

    for (int f = 4; f < argc; f++) {
        size_t size = 0;
        unsigned char* bytes = readAll(argv[f], &size);
        TinyImageInfo info;
        TinyPlan plan;
        const char* image = baseName(argv[f]);

        if (!bytes || tiny_image_probe(bytes, size, &info) != TINYIMG_OK ||
            tiny_plan_init(&plan, bytes, size) != TINYIMG_OK ||
            tiny_plan_resize(&plan, width, 0u) != TINYIMG_OK) {
            fprintf(stderr, "%s: could not plan\n", image);
            printf(
                "{\"image\":\"%s\",\"width\":%u,\"error\":-1}\n", image, width
            );
            free(bytes);
            status = 1;
            continue;
        }

        for (size_t b = 0; b < nb; b++) {
            for (size_t d = 0; d < nd; d++) {
                TinySelectOpts opts;
                memset(&opts, 0, sizeof(opts));
                opts.budget_us = (uint32_t) budgets[b];
                opts.scale_milli = 1000u;
                opts.min_db = (float) dbs[d];
                opts.formats = TINYIMG_SELECT_FORMAT(info.format) |
                               TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_JPEG) |
                               TINYIMG_SELECT_FORMAT(TINYIMG_FORMAT_WEBP);
                opts.objective = TINYIMG_SELECT_MIN_BYTES;
                opts.uncharacterized = TINYIMG_SELECT_REFUSE;

                for (size_t a = 0; a < 3; a++) {
                    memset(&selection, 0, sizeof(selection));
                    int result = arms[a].fn(&plan, &opts, &selection);
                    emit(
                        image, width, opts.budget_us, dbs[d], arms[a].name,
                        result, &selection
                    );
                }
            }
        }

        free(bytes);
    }

    return status;
}
