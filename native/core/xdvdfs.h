/*
 * hakuX native core — XDVDFS (Xbox game disc filesystem) reader.
 *
 * Reads files from an Xbox disc image: an extracted XISO or a full disc dump
 * (the game partition at one of the XGD1/2/3 offsets), either as an ISO or
 * as a CHD (chd.c).
 */
#ifndef NATIVE_CORE_XDVDFS_H
#define NATIVE_CORE_XDVDFS_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_XDVDFS_SECTOR 2048u

/* Mods (native/core/mods.c): host files over the disc's and byte patches of
 * its files. An overlay file or folder's sector is NC_DISC_OVERLAY plus its
 * index in the overlay, sorted by path (the same whatever the mods' order). */
#define NC_DISC_OVERLAY 0xF0000000u

typedef struct NcDiscOverlay NcDiscOverlay;
struct NcChd;

typedef struct NcDisc {
    int fd;
    struct NcChd *chd;     /* a CHD image's reader, NULL for an ISO */
    uint64_t part_off;     /* byte offset of the game partition in the image */
    uint32_t root_sector;
    uint32_t root_size;
    NcDiscOverlay *ov;     /* NULL: the disc as it is */
} NcDisc;

typedef struct NcDiscFile {
    uint32_t sector;
    uint32_t size;
    uint8_t attr;          /* 0x10: directory */
} NcDiscFile;

/* The image in fd, an ISO or a CHD. On failure d holds nothing and fd stays
 * the caller's; else nc_disc_close closes it. */
bool nc_disc_open(NcDisc *d, int fd);
void nc_disc_close(NcDisc *d);

/* Finds a path such as "\\default.xbe" or "media/x.bin" (either slash,
 * case-insensitive). */
bool nc_disc_find(const NcDisc *d, const char *path, NcDiscFile *out);

/* Reads len bytes at offset off of a file. */
bool nc_disc_read(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len);

/* The overlay, before the title boots: a host file at a disc path (either
 * slash); a later one at the same path takes its place. Folders the disc
 * doesn't have come with it. Then seal (sorted; nc_disc_find sees it), then
 * patches: bytes over a file's content at off (the overlay's file if it
 * replaced it). */
bool nc_disc_overlay_file(NcDisc *d, const char *path, const char *host);
void nc_disc_overlay_seal(NcDisc *d);
bool nc_disc_overlay_patch(NcDisc *d, const char *path, uint64_t off, const uint8_t *bytes, uint32_t len);
/* The disc's own file and bytes, past the overlay and its patches (what
 * identifies the game for savestates, the bytes a patch replaced). */
bool nc_disc_find_orig(const NcDisc *d, const char *path, NcDiscFile *out);
bool nc_disc_read_orig(const NcDisc *d, const NcDiscFile *f, uint64_t off, void *buf, size_t len);
/* The overlay's files and folders in the disc folder dir: replaced ones
 * with their new size, added ones (directory listings). */
void nc_disc_overlay_list(const NcDisc *d, const char *dir,
                          void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
