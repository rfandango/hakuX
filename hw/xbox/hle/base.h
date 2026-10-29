/*
 * hakuX HLE GPU — the C library and the few macros the HLE code uses
 * everywhere. Every HLE source includes this first (pure modules include
 * hle_pure.h, which includes it outside the host tests). GLib's subset comes
 * from the host build's glib.h; the host core's services from host.h.
 */
#ifndef HW_XBOX_HLE_BASE_H
#define HW_XBOX_HLE_BASE_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "glib.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define HLE_BUILD_BUG_ON(x) _Static_assert(!(x), "not expecting: " #x)
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))

/* Guest pages: 4 KiB (the Xbox's small pages). */
#define HLE_PAGE_BITS 12
#define HLE_PAGE_SIZE (1u << HLE_PAGE_BITS)
#define HLE_PAGE_MASK (~(HLE_PAGE_SIZE - 1))

/* Atomics and barriers between the HLE's threads (the guest CPU, the
 * render thread, the pipeline builders). */
#define hle_atomic_read(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#define hle_atomic_set(p, v) __atomic_store_n(p, v, __ATOMIC_RELAXED)
#define hle_atomic_load_acquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define hle_atomic_inc(p) ((void)__atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST))
#define hle_atomic_dec(p) ((void)__atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST))
#define hle_atomic_cmpxchg(p, o, n)                                                                \
    ({                                                                                             \
        __typeof__(*(p)) _old = (o);                                                               \
        __atomic_compare_exchange_n(p, &_old, (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);     \
        _old;                                                                                      \
    })
#define hle_smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define hle_smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

/* Little-endian loads and stores from a host pointer (guest data). */
static inline uint32_t hle_ldl_le(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint32_t hle_lduw_le(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline void hle_stl_le(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static inline int hle_ctz32(uint32_t v)
{
    return v ? __builtin_ctz(v) : 32;
}

/* strncpy that always terminates. */
static inline void hle_strlcpy(char *dst, size_t size, const char *src)
{
    if (size) {
        size_t n = strnlen(src, size - 1);
        memcpy(dst, src, n);
        dst[n] = 0;
    }
}

static inline void *hle_memalign(size_t alignment, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size)) {
        abort();
    }
    return p;
}

/* A large zeroed block for a boot (MiBs), mapped rather than from the heap:
 * freeing it gives it back. glibc keeps a freed heap block in the arena it
 * came from once its mmap threshold has risen, and in one process (the
 * libretro core) each boot's threads get other arenas: every boot would
 * leave its copy. */
static inline void *hle_big_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        abort();
    }
    return p;
}

static inline void hle_big_free(void *p, size_t size)
{
    if (p) {
        munmap(p, size);
    }
}

static inline size_t hle_host_page_size(void)
{
    return (size_t)sysconf(_SC_PAGESIZE);
}

#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/util/thread.h"

#endif
