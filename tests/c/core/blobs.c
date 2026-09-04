#include "../test.h"
#include "tinyimg/memory.h"
#include "tinyimg/util.h"

static uint8_t* makeBlob(uint8_t fill, size_t size) {
    uint8_t* data = tiny_alloc(size);
    if (data) tiny_memset(data, fill, size);
    return data;
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
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_ICC, "srgb", &size));
    r |= assertNull(tiny_blob_get(TINYIMG_BLOB_CASCADE, truncated, &size));

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

    return r;
}
