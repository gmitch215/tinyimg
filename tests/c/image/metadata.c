#include "../test.h"
#include "tinyimg/memory.h"

/**
 * The metadata block, which is one allocation holding two unrelated things.
 *
 * EXIF is a byte payload and the key and value pairs are a list, and they share
 * a block because tiny_image_destroy frees it with a single call. That means
 * every operation on either one can move the other, and most of what is checked
 * here is that it does so correctly: adding EXIF after entries exist shifts
 * them, stripping it shifts them back, and removing an entry from the middle
 * closes the gap the ones after it are addressed through.
 */
int main(void) {
    int r = 0;

    TinyImage image;
    r |= assertEquals(tiny_image_create(&image, 4, 4, 3), TINYIMG_OK);

    // #region nothing yet

    size_t count = 99;

    r |= assertEquals(tiny_image_has_exif(&image), 0);
    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 0L);
    r |= assertEquals(tiny_image_has_metadata(&image, "Author"), 0);
    r |= assertNull(image.meta);

    char* out = 0;
    size_t size = 0;

    r |= assertEquals(
        tiny_image_get_exif(&image, &out, &size), TINYIMG_ERR_NOT_FOUND
    );
    r |= assertEquals(
        tiny_image_get_metadata(&image, "Author", &out), TINYIMG_ERR_NOT_FOUND
    );

    // stripping what is not there is not an error, so a caller can strip
    // unconditionally
    r |= assertEquals(tiny_image_strip_exif(&image), TINYIMG_OK);

    // #endregion

    // #region exif

    static const char payload[] = "MM\0*\0\0\0\10\0\1\1\22\0\3\0\0\0\1\0\6";
    size_t payload_size = sizeof(payload) - 1;

    r |= assertEquals(
        tiny_image_set_exif(&image, payload, payload_size), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_has_exif(&image), 1);
    r |= assertNotNull(image.meta);

    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertEquals((long) size, (long) payload_size);
    r |= assertBytesMatch(out, payload, payload_size);

    // the caller owns what get handed back, so freeing it must not disturb the
    // block it was copied out of
    tiny_free(out);
    r |= assertEquals(tiny_image_has_exif(&image), 1);

    // replacing with a longer payload, which cannot be written in place
    static const char longer[] =
        "II*\0\10\0\0\0\1\0\22\1\3\0\1\0\0\0\1\0\0\0 padding to make it longer";

    r |= assertEquals(
        tiny_image_set_exif(&image, longer, sizeof(longer) - 1), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertEquals((long) size, (long) (sizeof(longer) - 1));
    r |= assertBytesMatch(out, longer, sizeof(longer) - 1);
    tiny_free(out);

    // and back to a shorter one
    r |= assertEquals(
        tiny_image_set_exif(&image, payload, payload_size), TINYIMG_OK
    );
    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertEquals((long) size, (long) payload_size);
    tiny_free(out);

    r |= assertEquals(tiny_image_strip_exif(&image), TINYIMG_OK);
    r |= assertEquals(tiny_image_has_exif(&image), 0);
    r |= assertEquals(
        tiny_image_get_exif(&image, &out, &size), TINYIMG_ERR_NOT_FOUND
    );

    // an empty payload is the same request as stripping
    r |= assertEquals(tiny_image_set_exif(&image, payload, 0), TINYIMG_OK);
    r |= assertEquals(tiny_image_has_exif(&image), 0);

    // #endregion

    // #region key and value entries

    r |= assertEquals(
        tiny_image_set_metadata(&image, "Author", "Gregory"), TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_set_metadata(&image, "Description", "a test image"),
        TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_set_metadata(&image, "Software", "tinyimg"), TINYIMG_OK
    );

    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 3L);

    r |= assertEquals(tiny_image_has_metadata(&image, "Author"), 1);
    r |= assertEquals(tiny_image_has_metadata(&image, "Missing"), 0);

    // a key that is a prefix of another one is a different key
    r |= assertEquals(tiny_image_has_metadata(&image, "Auth"), 0);

    r |= assertEquals(
        tiny_image_get_metadata(&image, "Description", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "a test image");
    tiny_free(out);

    // replacing a value with a longer one, then a shorter one, and the entries
    // around it have to survive both
    r |= assertEquals(
        tiny_image_set_metadata(
            &image, "Description",
            "a considerably longer "
            "description than before"
        ),
        TINYIMG_OK
    );
    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 3L);

    r |= assertEquals(
        tiny_image_get_metadata(&image, "Author", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "Gregory");
    tiny_free(out);

    r |= assertEquals(
        tiny_image_get_metadata(&image, "Software", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "tinyimg");
    tiny_free(out);

    r |= assertEquals(
        tiny_image_set_metadata(&image, "Description", "short"), TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_get_metadata(&image, "Description", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "short");
    tiny_free(out);

    // removing the middle one, which is what closes a gap the last one is
    // addressed through
    r |= assertEquals(
        tiny_image_remove_metadata(&image, "Description"), TINYIMG_OK
    );
    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 2L);
    r |= assertEquals(tiny_image_has_metadata(&image, "Description"), 0);

    r |= assertEquals(
        tiny_image_get_metadata(&image, "Software", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "tinyimg");
    tiny_free(out);

    r |= assertEquals(
        tiny_image_remove_metadata(&image, "Description"), TINYIMG_ERR_NOT_FOUND
    );

    // an empty value is a value
    r |= assertEquals(tiny_image_set_metadata(&image, "Empty", ""), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_get_metadata(&image, "Empty", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "");
    tiny_free(out);

    // #endregion

    // #region the two sharing one block

    // EXIF added while entries exist has to shift them, and they have to still
    // read afterwards
    r |= assertEquals(
        tiny_image_set_exif(&image, payload, payload_size), TINYIMG_OK
    );

    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 3L);

    r |= assertEquals(
        tiny_image_get_metadata(&image, "Author", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "Gregory");
    tiny_free(out);

    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertEquals((long) size, (long) payload_size);
    r |= assertBytesMatch(out, payload, payload_size);
    tiny_free(out);

    // and stripping it shifts them back
    r |= assertEquals(tiny_image_strip_exif(&image), TINYIMG_OK);
    r |= assertEquals(
        tiny_image_get_metadata(&image, "Software", &out), TINYIMG_OK
    );
    r |= assertStringsMatch(out, "tinyimg");
    tiny_free(out);

    // an entry added after EXIF goes on the end and does not move it
    r |= assertEquals(
        tiny_image_set_exif(&image, payload, payload_size), TINYIMG_OK
    );
    r |= assertEquals(
        tiny_image_set_metadata(&image, "Last", "one"), TINYIMG_OK
    );

    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertBytesMatch(out, payload, payload_size);
    tiny_free(out);

    r |=
        assertEquals(tiny_image_get_metadata(&image, "Last", &out), TINYIMG_OK);
    r |= assertStringsMatch(out, "one");
    tiny_free(out);

    // #endregion

    // #region enough entries to force the block to grow several times

    for (uint32_t i = 0; i < 64; i++) {
        char key[16];
        char value[64];

        snprintf(key, sizeof(key), "key%u", i);
        snprintf(
            value, sizeof(value), "value %u padded out a little further", i
        );

        r |= assertEquals(
            tiny_image_set_metadata(&image, key, value), TINYIMG_OK
        );
    }

    r |=
        assertEquals(tiny_image_get_metadata_count(&image, &count), TINYIMG_OK);
    r |= assertEquals((long) count, 68L);

    // every one of them still reads, which is what a realloc that moved the
    // block would break if anything held a pointer into it
    int all = 1;
    for (uint32_t i = 0; i < 64; i++) {
        char key[16];
        char value[64];

        snprintf(key, sizeof(key), "key%u", i);
        snprintf(
            value, sizeof(value), "value %u padded out a little further", i
        );

        if (tiny_image_get_metadata(&image, key, &out) != TINYIMG_OK) {
            all = 0;
            continue;
        }

        if (strcmp(out, value) != 0) all = 0;
        tiny_free(out);
    }
    r |= assertTrue(all);

    // and so does the EXIF that has been shifted along with them
    r |= assertEquals(tiny_image_get_exif(&image, &out, &size), TINYIMG_OK);
    r |= assertBytesMatch(out, payload, payload_size);
    tiny_free(out);

    // #endregion

    // #region arguments

    r |= assertEquals(tiny_image_has_exif(0), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_strip_exif(0), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_set_exif(0, payload, 1), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_set_exif(&image, 0, 4), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_get_exif(&image, 0, &size), TINYIMG_ERR_NULL);
    r |= assertEquals(tiny_image_get_exif(&image, &out, 0), TINYIMG_ERR_NULL);
    r |= assertEquals(
        tiny_image_set_metadata(&image, 0, "value"), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_image_set_metadata(&image, "key", 0), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_image_set_metadata(&image, "", "value"), TINYIMG_ERR_RANGE
    );
    r |= assertEquals(tiny_image_has_metadata(&image, 0), TINYIMG_ERR_NULL);
    r |= assertEquals(
        tiny_image_get_metadata_count(&image, 0), TINYIMG_ERR_NULL
    );
    r |= assertEquals(
        tiny_image_get_metadata_count(0, &count), TINYIMG_ERR_NULL
    );

    // #endregion

    // one call releases the block and the pixels both, which is the reason the
    // block is one allocation
    r |= assertEquals(tiny_image_destroy(&image), TINYIMG_OK);
    r |= assertNull(image.meta);
    r |= assertNull(image.data);

    return r;
}
