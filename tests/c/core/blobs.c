#include "../test.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

static uint8_t* makeBlob(uint8_t fill, size_t size) {
    uint8_t* data = tiny_alloc(size);
    if (data) tiny_memset(data, fill, size);
    return data;
}

/**
 * The data the module carries, which resolves with nothing loaded.
 *
 * Two rules, and the second is the one a caller notices. A name resolves
 * against the residents and then the builtins, so `sans` always works. An
 * enumeration is different: **one resident blob of a kind hides every builtin
 * of that kind**, which is what stops a host that installed its own cascade
 * from also running the two that ship.
 */
static int builtins(void) {
    int r = 0;
    size_t size = 0;

    tiny_blob_free_all();

    r |= assertEquals((long) tiny_blob_builtin_count, 7L);

    // one face, four profiles, two cascades, and nothing under a kind's name
    // that belongs to another kind
    r |= assertNotNull(tiny_blob_get(TINYIMG_BLOB_FONT, "sans", &size));
    r |= assertGreaterThan((double) size, 1000.0);
    r |= assertNotNull(tiny_blob_get(TINYIMG_BLOB_ICC, "display-p3", 0));
    r |= assertNotNull(tiny_blob_get(TINYIMG_BLOB_ICC, "adobe-rgb-1998", 0));
    r |= assertNotNull(tiny_blob_get(TINYIMG_BLOB_ICC, "rec2020", 0));
    r |= assertNotNull(
        tiny_blob_get(TINYIMG_BLOB_CASCADE, "lbp-frontalface", 0)
    );
    r |= assertNotNull(
        tiny_blob_get(TINYIMG_BLOB_CASCADE, "lbp-profileface", 0)
    );
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_FONT, "srgb", 0));
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_ICC, "sans", 0));

    // a NULL id resolves to the first builtin of the kind
    r |= assertTrue(
        tiny_blob_get(TINYIMG_BLOB_FONT, 0, 0) ==
        tiny_blob_get(TINYIMG_BLOB_FONT, "sans", 0)
    );

    // and the walk reaches all of them with their ids
    const char* id = 0;
    r |= assertNotNull(tiny_blob_at(TINYIMG_BLOB_CASCADE, 0u, &id, 0));
    r |= assertEquals(tiny_strcmp(id, "lbp-frontalface"), 0);
    r |= assertNotNull(tiny_blob_at(TINYIMG_BLOB_CASCADE, 1u, &id, 0));
    r |= assertEquals(tiny_strcmp(id, "lbp-profileface"), 0);
    r |= assertNull(tiny_blob_at(TINYIMG_BLOB_CASCADE, 2u, 0, 0));

    // a builtin lives in the data section, so releasing it is a miss rather
    // than a free of something that was never allocated
    r |= assertEquals(
        tiny_blob_free(TINYIMG_BLOB_FONT, "sans"), TINYIMG_ERR_NOT_FOUND
    );
    r |= assertEquals(
        tiny_blob_free(TINYIMG_BLOB_CASCADE, 0), TINYIMG_ERR_NOT_FOUND
    );
    r |= assertNotNull(tiny_blob_get(TINYIMG_BLOB_FONT, "sans", 0));

    // one resident cascade, and the walk stops reporting the builtins
    uint8_t* own = makeBlob(0x11, 24);
    r |= assertNotNull(own);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_CASCADE, "mine", own, 24), TINYIMG_OK
    );

    r |= assertTrue(tiny_blob_at(TINYIMG_BLOB_CASCADE, 0u, 0, 0) == own);
    r |= assertNull(tiny_blob_at(TINYIMG_BLOB_CASCADE, 1u, 0, 0));

    // the other kinds are untouched by that, and a name still reaches the
    // hidden builtin
    r |= assertNotNull(tiny_blob_at(TINYIMG_BLOB_FONT, 0u, 0, 0));
    r |= assertNotNull(
        tiny_blob_get(TINYIMG_BLOB_CASCADE, "lbp-profileface", 0)
    );

    // tiny_blob_builtin_at reports what ships whatever is resident, which is
    // what a host listing the available faces reads
    r |= assertNotNull(tiny_blob_builtin_at(TINYIMG_BLOB_CASCADE, 0u, &id, 0));
    r |= assertEquals(tiny_strcmp(id, "lbp-frontalface"), 0);
    r |= assertNull(tiny_blob_builtin_at(TINYIMG_BLOB_CASCADE, 2u, 0, 0));
    r |= assertNull(tiny_blob_builtin_at(TINYIMG_BLOB_FONT, 1u, 0, 0));

    tiny_blob_free_all();
    return r;
}

int main(void) {
    int r = 0;

    tiny_blob_free_all();

    size_t size = 0;
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_FONT, "inter", &size));
    r |= assertEquals(
        tiny_blob_free(TINYIMG_BLOB_FONT, "inter"), TINYIMG_ERR_NOT_FOUND
    );

    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_FONT, "inter", 0, 10), TINYIMG_ERR_NULL
    );

    uint8_t* font = makeBlob(0xAA, 64);
    r |= assertNotNull(font);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_FONT, "inter", font, 64), TINYIMG_OK
    );

    const uint8_t* found = tiny_blob_get(TINYIMG_BLOB_FONT, "inter", &size);
    r |= assertTrue(found == font);
    r |= assertEquals((long) size, 64L);
    r |= assertEquals((long) found[63], 0xAAL);

    // the kind is part of the key, so the same id under another kind is a miss
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_ICC, "inter", &size));
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_FONT, "dejavu", &size));

    // a NULL id takes the first of that kind, which is how a caller asks for
    // whichever font happens to be loaded
    r |= assertTrue(tiny_blob_get(TINYIMG_BLOB_FONT, 0, &size) == font);

    // loading over the same kind and id replaces it and releases what was there
    uint8_t* replacement = makeBlob(0xBB, 128);
    r |= assertNotNull(replacement);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_FONT, "inter", replacement, 128), TINYIMG_OK
    );

    found = tiny_blob_get(TINYIMG_BLOB_FONT, "inter", &size);
    r |= assertTrue(found == replacement);
    r |= assertEquals((long) size, 128L);

    // the replaced block went back to the heap, so occupancy tracks the live
    // blobs rather than every blob ever loaded
    TinyHeapStats afterReplace;
    r |= assertEquals(tiny_heap_stats(&afterReplace), TINYIMG_OK);
    r |= assertGreaterThan((double) afterReplace.used, 0.0);

    uint8_t* profile = makeBlob(0xCC, 32);
    r |= assertNotNull(profile);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_ICC, "srgb", profile, 32), TINYIMG_OK
    );
    r |= assertTrue(tiny_blob_get(TINYIMG_BLOB_ICC, "srgb", &size) == profile);
    r |= assertEquals((long) size, 32L);

    // an id longer than the slot is truncated, not overrun, and the truncated
    // form is what a lookup has to use
    static const char* longId = "a-cascade-name-that-will-not-fit-in-the-slot";
    uint8_t* cascade = makeBlob(0xDD, 16);
    r |= assertNotNull(cascade);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_CASCADE, longId, cascade, 16), TINYIMG_OK
    );

    char truncated[TINYIMG_BLOB_ID_MAX];
    tiny_strcopy(truncated, longId, sizeof(truncated));
    r |= assertEquals(
        (long) tiny_strlen(truncated), (long) TINYIMG_BLOB_ID_MAX - 1L
    );
    r |= assertTrue(
        tiny_blob_get(TINYIMG_BLOB_CASCADE, truncated, &size) == cascade
    );

    // filling the table and asking for one more is refused rather than
    // overwriting something a caller still needs
    int loaded = 0;
    for (int i = 0; i < TINYIMG_MAX_BLOBS + 4; i++) {
        char id[8];
        id[0] = 'f';
        id[1] = (char) ('0' + i / 10);
        id[2] = (char) ('0' + i % 10);
        id[3] = '\0';

        uint8_t* extra = makeBlob((uint8_t) i, 8);
        if (!extra) break;

        if (tiny_blob_load(TINYIMG_BLOB_FONT, id, extra, 8) == TINYIMG_OK) {
            loaded++;
        }
        else {
            tiny_free(extra);
        }
    }

    // three slots were already taken by the font, the profile and the cascade
    r |= assertEquals((long) loaded, (long) TINYIMG_MAX_BLOBS - 3L);

    tiny_blob_free_all();
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_FONT, "inter", &size));
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_CASCADE, truncated, &size));

    // "srgb" is not gone, because it names a builtin: what free_all released
    // was the resident blob that had been shadowing it
    const uint8_t* shipped = tiny_blob_get(TINYIMG_BLOB_ICC, "srgb", &size);
    r |= assertNotNull(shipped);
    r |= assertTrue(shipped != profile);
    r |= assertEquals((long) size, 2588L);

    // every blob the table held is back in the heap
    TinyHeapStats empty;
    r |= assertEquals(tiny_heap_stats(&empty), TINYIMG_OK);
    r |= assertEquals((long) empty.used, 0L);

    // a lookup with a size out-parameter of NULL still works
    uint8_t* last = makeBlob(0xEE, 8);
    r |= assertNotNull(last);
    r |= assertEquals(
        tiny_blob_load(TINYIMG_BLOB_ICC, "p3", last, 8), TINYIMG_OK
    );
    r |= assertTrue(tiny_blob_get(TINYIMG_BLOB_ICC, "p3", 0) == last);
    r |= assertEquals(tiny_blob_free(TINYIMG_BLOB_ICC, "p3"), TINYIMG_OK);
    r |= assertEquals(
        tiny_blob_free(TINYIMG_BLOB_ICC, "p3"), TINYIMG_ERR_NOT_FOUND
    );

    r |= builtins();

    return r;
}
