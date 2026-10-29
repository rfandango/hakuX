/*
 * hakuX native core — CHD disc images (MAME's compressed hunks of data, made
 * by chdman createdvd), read through libchdr: the image's bytes as an ISO's.
 */
#ifndef NATIVE_CORE_CHD_H
#define NATIVE_CORE_CHD_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcChd NcChd;

/* The CHD in the file fd, or NULL: *is_chd false when the file isn't one
 * (nothing logged), true when it is one this core can't read (logged: a CD
 * image, a delta of another CHD, a build without libchdr). fd stays the
 * caller's to close, after nc_chd_close. */
NcChd *nc_chd_open(int fd, bool *is_chd);
/* len bytes of the image at off; from any thread. */
bool nc_chd_read(NcChd *c, void *buf, size_t len, uint64_t off);
void nc_chd_close(NcChd *c);

#ifdef __cplusplus
}
#endif

#endif
