#include "../test.h"
#include "tinyimg/memory.h"

int main(void) {
    int r = 0;

    unsigned char src[16];
    unsigned char dst[16];

    for (int i = 0; i < 16; i++) src[i] = (unsigned char) (i * 3 + 1);

    r |= assertBytesMatch(tiny_memcpy(dst, src, 16), src, 16);
    r |= assertEquals(tiny_memcmp(dst, src, 16), 0);

    tiny_memset(dst, 0xAB, 16);
    for (int i = 0; i < 16; i++) r |= assertEquals(dst[i], 0xAB);

    // overlapping forward, which memcpy is not allowed to handle
    unsigned char overlap[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char wanted[8] = {1, 2, 1, 2, 3, 4, 5, 6};
    tiny_memmove(overlap + 2, overlap, 6);
    r |= assertBytesMatch(overlap, wanted, 8);

    // overlapping backward
    unsigned char back[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char backWanted[8] = {3, 4, 5, 6, 7, 8, 7, 8};
    tiny_memmove(back, back + 2, 6);
    r |= assertBytesMatch(back, backWanted, 8);

    r |= assertGreaterThan(tiny_memcmp("b", "a", 1), 0);
    r |= assertLessThan(tiny_memcmp("a", "b", 1), 0);
    r |= assertEquals(tiny_memcmp("a", "b", 0), 0);

    return r;
}
