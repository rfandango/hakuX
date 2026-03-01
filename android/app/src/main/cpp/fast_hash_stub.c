#include "qemu/osdep.h"
#include "qemu/fast-hash.h"

#ifdef __aarch64__
#include <arm_acle.h>

#define LSH_STRIDE      16
#define LSH_QUANT_MASK  0xE0E0E0E0E0E0E0E0ULL

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

__attribute__((target("crc")))
uint64_t fast_hash_lsh(const uint8_t *data, size_t len)
{
    uint32_t c0 = 0xa5b9c3d1;
    uint32_t c1 = 0x7e4f2a68;
    uint32_t c2 = 0x3c91d705;
    uint32_t c3 = 0xf0826b4e;

    const uint64_t *p = (const uint64_t *)data;
    size_t n_blocks = len / 64;
    size_t step = LSH_STRIDE * 8;

    for (size_t i = 0; i + 7 < n_blocks * 8; i += step) {
        c0 = __crc32cd(c0, p[i + 0] & LSH_QUANT_MASK);
        c1 = __crc32cd(c1, p[i + 1] & LSH_QUANT_MASK);
        c2 = __crc32cd(c2, p[i + 2] & LSH_QUANT_MASK);
        c3 = __crc32cd(c3, p[i + 3] & LSH_QUANT_MASK);
        c0 = __crc32cd(c0, p[i + 4] & LSH_QUANT_MASK);
        c1 = __crc32cd(c1, p[i + 5] & LSH_QUANT_MASK);
        c2 = __crc32cd(c2, p[i + 6] & LSH_QUANT_MASK);
        c3 = __crc32cd(c3, p[i + 7] & LSH_QUANT_MASK);
    }

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

uint64_t fast_hash_lsh(const uint8_t *data, size_t len)
{
    const uint64_t fnv_offset = 0xa5b9c3d17e4f2a68ULL;
    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = fnv_offset;

    const uint8_t *limit8 = data + (len & ~(size_t)7);
    size_t stride_bytes = LSH_STRIDE * 64;

    for (const uint8_t *p = data; p + 8 <= limit8; p += stride_bytes) {
        const uint8_t *block_end = p + 64;
        if (block_end > limit8) block_end = limit8;
        while (p + 8 <= block_end) {
            uint64_t v;
            memcpy(&v, p, 8);
            hash ^= v & LSH_QUANT_MASK;
            hash *= fnv_prime;
            p += 8;
        }
    }

    return hash;
}

#endif
