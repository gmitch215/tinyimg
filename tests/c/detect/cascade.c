#include "test.h"
#include "tinyimg/detect.h"

/**
 * @file
 * @brief Loading a cascade, and what a search costs.
 *
 * The blobs the detector reads are the OpenCV XML repacked by
 * `scripts/cascade.ts`. The repacked copies are committed under
 * `derived/cascades` for the same reason the fonts are: `blobs/` is gitignored,
 * so a test that read only from there would not gate.
 */

/** Loads a committed cascade into the blob table, which takes ownership. */
static int install(const char* id, const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "derived/cascades/%s.bin", name);

    size_t size = 0;
    unsigned char* bytes = readFixture(path, &size);
    if (!bytes) return TINYIMG_ERR_NOT_FOUND;

    uint8_t* owned = (uint8_t*) tiny_alloc(size);
    if (!owned) {
        free(bytes);
        return TINYIMG_ERR_MEMORY;
    }

    memcpy(owned, bytes, size);
    free(bytes);

    return tiny_blob_load(TINYIMG_BLOB_CASCADE, id, owned, size);
}

static int loading(void) {
    int failures = 0;

    tiny_blob_free_all();

    // nothing resident, so a search has nothing to run rather than finding
    // nothing
    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 200, 200, 1);

    TinyFaceBox boxes[4];
    uint32_t count = 99;

    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 4u, &count),
        TINYIMG_ERR_BLOB_MISSING
    );
    failures += assertEquals(count, 0);
    failures += assertEquals(tiny_cascade_check(0), TINYIMG_ERR_BLOB_MISSING);

    failures += assertEquals(install("frontal", "lbp-frontalface"), TINYIMG_OK);
    failures += assertEquals(tiny_cascade_check(0), TINYIMG_OK);
    failures += assertEquals(tiny_cascade_check("frontal"), TINYIMG_OK);
    failures +=
        assertEquals(tiny_cascade_check("nothing"), TINYIMG_ERR_BLOB_MISSING);

    failures += assertEquals(install("profile", "lbp-profileface"), TINYIMG_OK);
    failures += assertEquals(tiny_cascade_check("profile"), TINYIMG_OK);

    // a header whose magic is right and whose counts are not: rejected at load
    // rather than walked off the end of the blob during a search
    failures += assertEquals(install("broken", "malformed"), TINYIMG_OK);
    failures += assertEquals(tiny_cascade_check("broken"), TINYIMG_ERR_CORRUPT);

    // and a search refuses while it is resident, because a cascade that does
    // not parse is a configuration fault and finding nothing would hide it
    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 4u, &count), TINYIMG_ERR_CORRUPT
    );

    failures += assertEquals(
        tiny_blob_free(TINYIMG_BLOB_CASCADE, "broken"), TINYIMG_OK
    );
    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 4u, &count), TINYIMG_OK
    );

    tiny_image_destroy(&image);
    tiny_blob_free_all();
    return failures;
}

/** The XML the repacker was given is not a cascade the detector reads. */
static int rejectsXml(void) {
    int failures = 0;

    tiny_blob_free_all();

    // "<?xml" where the magic should be
    const char* xml =
        "<?xml version=\"1.0\"?><opencv_storage></opencv_storage>";
    size_t size = strlen(xml);

    uint8_t* owned = (uint8_t*) tiny_alloc(size);
    if (!owned) return 1;

    memcpy(owned, xml, size);
    failures += assertEquals(
        tiny_blob_load(TINYIMG_BLOB_CASCADE, "xml", owned, size), TINYIMG_OK
    );
    failures += assertEquals(tiny_cascade_check("xml"), TINYIMG_ERR_CORRUPT);

    tiny_blob_free_all();
    return failures;
}

/** Truncation at every length, none of which may read past the blob. */
static int truncation(void) {
    int failures = 0;
    size_t size = 0;

    unsigned char* bytes =
        readFixture("derived/cascades/lbp-frontalface.bin", &size);
    if (!bytes) return 1;

    int rejected = 1;

    // every prefix shorter than the whole thing has to be refused. a length
    // check that used the declared counts without checking them against the
    // blob would pass some of these and read outside the allocation on the one
    // after
    for (size_t at = 1; at < size; at += 97) {
        uint8_t* owned = (uint8_t*) tiny_alloc(at);
        if (!owned) break;

        memcpy(owned, bytes, at);
        tiny_blob_load(TINYIMG_BLOB_CASCADE, "cut", owned, at);

        if (tiny_cascade_check("cut") == TINYIMG_OK) {
            printf("a %zu byte prefix of %zu was accepted\n", at, size);
            rejected = 0;
        }
    }

    failures += assertTrue(rejected);

    free(bytes);
    tiny_blob_free_all();
    return failures;
}

/**
 * @brief The window the two shipped cascades were trained at.
 *
 * Not the same shape as each other: the frontal one is square and the profile
 * one is taller than it is wide, which is why the detector carries both extents
 * rather than one size. A `min_size` read as a square would exclude a profile
 * detection on its width.
 */
static int windows(void) {
    int failures = 0;

    tiny_blob_free_all();
    failures += assertEquals(install("frontal", "lbp-frontalface"), TINYIMG_OK);

    // a 45 pixel window cannot search a 44 pixel image, and says so by finding
    // nothing rather than by failing
    TinyImage tiny;
    memset(&tiny, 0, sizeof(tiny));
    tiny_image_create(&tiny, 44, 44, 1);

    TinyFaceBox boxes[4];
    uint32_t count = 99;

    failures += assertEquals(
        tiny_image_detect_faces(&tiny, boxes, 4u, &count), TINYIMG_OK
    );
    failures += assertEquals(count, 0);
    tiny_image_destroy(&tiny);

    // exactly the window size is one position, which is searchable
    TinyImage exact;
    memset(&exact, 0, sizeof(exact));
    tiny_image_create(&exact, 45, 45, 1);

    TinyDetectOpts opts;
    tiny_detect_opts(&opts);
    opts.min_neighbors = 1u;

    failures += assertEquals(
        tiny_image_detect_faces_ex(&exact, &opts, boxes, 4u, &count), TINYIMG_OK
    );

    // a flat gray image is not a face, whatever the threshold
    failures += assertEquals(count, 0);
    tiny_image_destroy(&exact);

    tiny_blob_free_all();
    return failures;
}

/** Arguments a caller can get wrong. */
static int rejections(void) {
    int failures = 0;

    tiny_blob_free_all();
    install("frontal", "lbp-frontalface");

    TinyImage image;
    memset(&image, 0, sizeof(image));
    tiny_image_create(&image, 100, 100, 3);

    TinyFaceBox boxes[4];
    uint32_t count = 0;

    failures += assertEquals(
        tiny_image_detect_faces(0, boxes, 4u, &count), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_detect_faces(&image, 0, 4u, &count), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 4u, 0), TINYIMG_ERR_NULL
    );
    failures += assertEquals(
        tiny_image_detect_faces_ex(&image, 0, boxes, 4u, 0), TINYIMG_ERR_NULL
    );

    // a zero capacity is not an error; there is simply nowhere to write a box
    failures += assertEquals(
        tiny_image_detect_faces(&image, boxes, 0u, &count), TINYIMG_OK
    );
    failures += assertEquals(count, 0);

    // a zeroed options structure means the defaults, so it searches rather than
    // doing nothing
    TinyDetectOpts zero;
    memset(&zero, 0, sizeof(zero));
    failures += assertEquals(
        tiny_image_detect_faces_ex(&image, &zero, boxes, 4u, &count), TINYIMG_OK
    );

    // a min_size larger than the image has no scale to search
    TinyDetectOpts huge;
    tiny_detect_opts(&huge);
    huge.min_size = 5000u;
    failures += assertEquals(
        tiny_image_detect_faces_ex(&image, &huge, boxes, 4u, &count), TINYIMG_OK
    );
    failures += assertEquals(count, 0);

    // every channel count goes through the same luminance
    for (uint8_t channels = 1; channels <= 4u; channels++) {
        TinyImage any;
        memset(&any, 0, sizeof(any));
        tiny_image_create(&any, 80, 80, channels);

        printf("%u channel: ", channels);
        failures += assertEquals(
            tiny_image_detect_faces(&any, boxes, 4u, &count), TINYIMG_OK
        );

        tiny_image_destroy(&any);
    }

    tiny_image_destroy(&image);
    tiny_blob_free_all();
    return failures;
}

/** The structure sizes a host reads an array of boxes with. */
static int layout(void) {
    int failures = 0;

    failures +=
        assertEquals(tiny_face_box_sizeof(), (long) sizeof(TinyFaceBox));
    failures +=
        assertEquals(tiny_detect_opts_sizeof(), (long) sizeof(TinyDetectOpts));

    TinyDetectOpts opts;
    tiny_detect_opts(&opts);

    failures += assertEquals(opts.min_size, 0);
    failures += assertEquals(opts.max_size, 0);
    failures += assertFloatEquals(opts.scale_factor, 1.1f, 0.0f);
    failures += assertEquals(opts.min_neighbors, 3);

    return failures;
}

int main(void) {
    int failures = 0;

    printf("-- loading --\n");
    failures += loading();
    printf("-- rejects xml --\n");
    failures += rejectsXml();
    printf("-- truncation --\n");
    failures += truncation();
    printf("-- windows --\n");
    failures += windows();
    printf("-- rejections --\n");
    failures += rejections();
    printf("-- layout --\n");
    failures += layout();

    if (failures > 0) printf("%d assertion(s) failed\n", failures);
    return failures > 0 ? 1 : 0;
}
