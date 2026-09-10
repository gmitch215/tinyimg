#pragma once

#include "codec/av1.h"

#include "codec/av1-tables.h"
#include "test.h"
#include "tinyimg/memory.h"

/**
 * @file
 * @brief Opening one AVIF item down to the bytes of its first tile.
 *
 * Shared by the mode info, coefficient and reconstruction tests, which each
 * need a synchronised symbol decoder over a real tile and nothing else in
 * common. It handles both OBU layouts in play: `cavif` writes a frame header
 * and a tile group separately, `avifenc` writes one frame OBU carrying both.
 */

/** The `mdat` payload, which is the whole AV1 item for these files. */
static inline int itemBytes(
    const char* name, unsigned char** base, const unsigned char** item,
    size_t* size
) {
    size_t whole = 0;
    unsigned char* file = readFixture(name, &whole);

    if (!file) return TINYIMG_ERR_NOT_FOUND;

    size_t at = 0;

    while (at + 8 <= whole) {
        uint32_t length = ((uint32_t) file[at] << 24) |
                          ((uint32_t) file[at + 1] << 16) |
                          ((uint32_t) file[at + 2] << 8) | file[at + 3];

        int is_mdat = file[at + 4] == 'm' && file[at + 5] == 'd' &&
                      file[at + 6] == 'a' && file[at + 7] == 't';

        if (length < 8 || at + length > whole) break;

        if (is_mdat) {
            *base = file;
            *item = file + at + 8;
            *size = length - 8u;

            return TINYIMG_OK;
        }

        at += length;
    }

    free(file);
    return TINYIMG_ERR_CORRUPT;
}

/** Everything one of these files needs to reach its first block. */
typedef struct {
    unsigned char* file;
    TinyAv1Sequence sequence;
    TinyAv1Frame frame;

    /** The first tile, which is all a single-tile fixture has. */
    const uint8_t* tile;
    size_t tile_size;

    /**
     * The whole tile group payload, so a caller can resolve any tile.
     *
     * Kept beside the first tile because resolving tile `n` needs the group's
     * own header and the length prefixes in front of every tile but the last,
     * which is what tiny_av1_tile_bytes walks.
     */
    const uint8_t* group;
    size_t group_size;

    size_t header_bytes;
} Opened;

/**
 * Walks the OBUs, reads both headers and resolves the first tile.
 *
 * Handles a frame OBU and a split header plus tile group, because the two
 * encoders in play emit one each.
 */
static inline int open_item(const char* name, Opened* out) {
    tiny_memset(out, 0, sizeof(*out));

    const unsigned char* item = 0;
    size_t size = 0;

    int result = itemBytes(name, &out->file, &item, &size);
    if (result != TINYIMG_OK) return result;

    size_t at = 0;
    TinyAv1Obu obu;

    while (tiny_av1_next_obu(item, size, &at, &obu)) {
        if (obu.type == TINY_AV1_OBU_SEQUENCE_HEADER) {
            result =
                tiny_av1_read_sequence(&out->sequence, obu.payload, obu.size);

            if (result != TINYIMG_OK) return result;
        }
        else if (obu.type == TINY_AV1_OBU_FRAME_HEADER) {
            result = tiny_av1_read_frame(
                &out->frame, &out->sequence, obu.payload, obu.size, 0
            );

            if (result != TINYIMG_OK) return result;
        }
        else if (obu.type == TINY_AV1_OBU_TILE_GROUP) {
            out->group = obu.payload;
            out->group_size = obu.size;

            result = tiny_av1_tile_bytes(
                &out->frame, obu.payload, obu.size, 0u, &out->tile,
                &out->tile_size
            );

            if (result != TINYIMG_OK) return result;
        }
        else if (obu.type == TINY_AV1_OBU_FRAME) {
            result = tiny_av1_read_frame(
                &out->frame, &out->sequence, obu.payload, obu.size,
                &out->header_bytes
            );

            if (result != TINYIMG_OK) return result;

            out->group = obu.payload + out->header_bytes;
            out->group_size = obu.size - out->header_bytes;

            result = tiny_av1_tile_bytes(
                &out->frame, out->group, out->group_size, 0u, &out->tile,
                &out->tile_size
            );

            if (result != TINYIMG_OK) return result;
        }
    }

    return out->tile ? TINYIMG_OK : TINYIMG_ERR_CORRUPT;
}
