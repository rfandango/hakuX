#include "qemu/osdep.h"
#include "qemu/fast-hash.h"

#ifdef __aarch64__
#include <arm_acle.h>

__attribute__((target("crc")))
uint64_t fast_hash(const uint8_t *data, size_t len)
{
    uint32_t h1 = 0x811c9dc5;
    uint32_t h2 = 0xc1aee535;

    const uint8_t *end = data + len;
    const uint8_t *limit8 = data + (len & ~(size_t)7);

    while (data < limit8) {
        uint64_t v;
        memcpy(&v, data, 8);
        h1 = __crc32cd(h1, v);
        h2 = __crc32cd(h2, v * 0x9E3779B97F4A7C15ULL);
        data += 8;
    }

    uint32_t tail = 0;
    while (data < end) {
        tail = (tail << 8) | *data++;
    }
    h1 = __crc32cw(h1, tail);

    return ((uint64_t)h1 << 32) | h2;
}

#else

uint64_t fast_hash(const uint8_t *data, size_t len)
{
    const uint64_t fnv_offset = 1469598103934665603ULL;
    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = fnv_offset;

    const uint8_t *end = data + len;
    const uint8_t *limit8 = data + (len & ~(size_t)7);

    while (data < limit8) {
        uint64_t v;
        memcpy(&v, data, 8);
        hash ^= v;
        hash *= fnv_prime;
        data += 8;
    }
    while (data < end) {
        hash ^= (uint64_t)*data++;
        hash *= fnv_prime;
    }

    return hash;
}

#endif
