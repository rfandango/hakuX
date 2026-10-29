/*
 * hakuX native core — common definitions.
 *
 * The native core runs an Xbox title without QEMU: FEXCore (fork, branch
 * hakuX) executes the 32-bit guest behind a guest memory base, and the Xbox
 * kernel is reimplemented in native/kernel. See
 * docs/superpowers/plans/2026-10-12-native-core-00-roadmap.md.
 */
#ifndef NATIVE_CORE_NC_H
#define NATIVE_CORE_NC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PAGE 4096u

/* Log line to stderr and the Android log under the given tag. */
void nc_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* A report line ("tag: text\n") to fd, or with fd < 0 to the log (the app's
 * on-demand dumps). */
void nc_dump_line(int fd, const char *line, size_t len);

static inline uint16_t nc_rd16(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t nc_rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void nc_wr16(void *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

static inline void nc_wr32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* A large zeroed block for a boot (MiBs), mapped rather than from the heap:
 * freeing it gives it back. glibc keeps a freed heap block in the arena it
 * came from once its mmap threshold has risen, and in one process (the
 * libretro core) each boot's threads get other arenas: every boot would
 * leave its copy. NULL if it can't be mapped. */
void *nc_big_alloc(size_t size);
void nc_big_free(void *p, size_t size);

static inline uint32_t nc_align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

#ifdef __cplusplus
}
#endif

#endif
