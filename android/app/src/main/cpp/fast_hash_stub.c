#include "qemu/osdep.h"
#include "qemu/fast-hash.h"

#ifdef __aarch64__
#include <arm_acle.h>

__attribute__((target("crc")))
uint64_t fast_hash(const uint8_t *data, size_t len)
{
    uint32_t c0 = 0x811c9dc5;
    uint32_t c1 = 0xc1aee535;
    uint32_t c2 = 0x5f356495;
    uint32_t c3 = 0x9e3779b9;

    const uint64_t *p = (const uint64_t *)data;
    size_t n64 = len / 64;

    while (n64--) {
        c0 = __crc32cd(c0, p[0]);
        c1 = __crc32cd(c1, p[1]);
        c2 = __crc32cd(c2, p[2]);
        c3 = __crc32cd(c3, p[3]);
        c0 = __crc32cd(c0, p[4]);
        c1 = __crc32cd(c1, p[5]);
        c2 = __crc32cd(c2, p[6]);
        c3 = __crc32cd(c3, p[7]);
        p += 8;
    }

    const uint8_t *tail = (const uint8_t *)p;
    const uint8_t *end = data + len;
    while (tail + 8 <= end) {
        uint64_t v;
        memcpy(&v, tail, 8);
        c0 = __crc32cd(c0, v);
        tail += 8;
    }
    uint32_t rem = 0;
    while (tail < end) {
        rem = (rem << 8) | *tail++;
    }
    c0 = __crc32cw(c0, rem);

    uint64_t hash = ((uint64_t)c0 << 32) | c1;
    hash ^= ((uint64_t)c2 << 32) | c3;
    return hash;
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
