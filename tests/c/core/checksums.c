#include "../test.h"
#include "tinyimg/util.h"

int main(void) {
    int r = 0;

    const uint8_t digits[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    // the check value every CRC-32 implementation publishes
    r |= assertEquals((long) tiny_crc32(0, digits, 9), 0xCBF43926L);
    r |= assertEquals((long) tiny_crc32(0, digits, 0), 0L);
    r |= assertEquals((long) tiny_crc32(0, 0, 9), 0L);

    // a split feed has to match a single one, or a chunked PNG writer produces
    // the wrong trailer
    uint32_t split = tiny_crc32(0, digits, 4);
    split = tiny_crc32(split, digits + 4, 5);
    r |= assertEquals((long) split, 0xCBF43926L);

    const uint8_t wikipedia[9] = {'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'};

    r |= assertEquals((long) tiny_adler32(1, wikipedia, 9), 0x11E60398L);
    r |= assertEquals((long) tiny_adler32(1, wikipedia, 0), 1L);

    uint32_t rolling = tiny_adler32(1, wikipedia, 3);
    rolling = tiny_adler32(rolling, wikipedia + 3, 6);
    r |= assertEquals((long) rolling, 0x11E60398L);

    // past 5552 bytes the accumulator has to be reduced, so a long run is the
    // case that catches a missing modulo
    static uint8_t large[20000];
    for (size_t i = 0; i < sizeof(large); i++) {
        large[i] = (uint8_t) (i * 7 + 3);
    }

    uint32_t whole = tiny_adler32(1, large, sizeof(large));
    uint32_t pieces = 1;
    for (size_t i = 0; i < sizeof(large); i += 997) {
        size_t n = sizeof(large) - i < 997 ? sizeof(large) - i : 997;
        pieces = tiny_adler32(pieces, large + i, n);
    }
    r |= assertEquals((long) whole, (long) pieces);

    uint32_t crcWhole = tiny_crc32(0, large, sizeof(large));
    uint32_t crcPieces = 0;
    for (size_t i = 0; i < sizeof(large); i += 997) {
        size_t n = sizeof(large) - i < 997 ? sizeof(large) - i : 997;
        crcPieces = tiny_crc32(crcPieces, large + i, n);
    }
    r |= assertEquals((long) crcWhole, (long) crcPieces);

    return r;
}
