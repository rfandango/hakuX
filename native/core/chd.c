/*
 * hakuX native core — CHD disc images through libchdr.
 *
 * A CHD holds the image in hunks (a few sectors each), each compressed on its
 * own (LZMA, zlib, zstd, Huffman, FLAC) or a copy of another. A read takes the
 * hunks it touches, the last ones read kept decompressed in a direct-mapped
 * cache: the title's small reads in a row land in one hunk.
 *
 * The CPU's thread and the disc jobs' (kernel/io.c) read at once, and libchdr
 * is not thread-safe (its file position, its decompressors): one lock. Its
 * file is the disc's descriptor, read with pread at the position it seeks to.
 */
#include "native/core/chd.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef NC_HAVE_CHD
#include <libchdr/chd.h>

#define CACHE_BYTES (4u << 20)
#define MAX_HUNK    (1u << 20)

struct NcChd {
    chd_file *chd;
    int fd;
    uint64_t pos;           /* libchdr's file position (cf_seek, cf_read) */
    uint64_t size;          /* the image's bytes */
    uint32_t hunk_bytes;
    pthread_mutex_t lock;
    unsigned slots;
    uint32_t *tag;          /* each slot's hunk + 1; 0: empty */
    uint8_t *data;          /* slots * hunk_bytes */
    unsigned errors;
};

static uint64_t cf_size(void *p)
{
    struct stat st;
    return fstat(((NcChd *)p)->fd, &st) == 0 ? (uint64_t)st.st_size : (uint64_t)-1;
}

static size_t cf_read(void *buf, size_t size, size_t count, void *p)
{
    NcChd *c = p;
    const size_t want = size * count;
    size_t got = 0;
    while (size && got < want) {
        const ssize_t n = pread(c->fd, (uint8_t *)buf + got, want - got, (off_t)(c->pos + got));
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    c->pos += got;
    return size ? got / size : 0;
}

static int cf_close(void *p)
{
    return 0;                   /* the descriptor is the disc's (nc_disc_close) */
}

static int cf_seek(void *p, int64_t off, int whence)
{
    NcChd *c = p;
    const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)c->pos : (int64_t)cf_size(p);
    if (base + off < 0) {
        return -1;
    }
    c->pos = (uint64_t)(base + off);
    return 0;
}

static const core_file_callbacks k_file = {cf_size, cf_read, cf_close, cf_seek};

/* A CHD codec's four letters, "none" for none. */
static const char *codec_name(uint32_t tag, char out[5])
{
    if (!tag) {
        return "none";
    }
    for (int i = 0; i < 4; i++) {
        const char ch = (char)(tag >> (24 - 8 * i));
        out[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
    }
    out[4] = 0;
    return out;
}

static bool is_cd(chd_file *chd)
{
    static const uint32_t k_tags[] = {CDROM_OLD_METADATA_TAG, CDROM_TRACK_METADATA_TAG, CDROM_TRACK_METADATA2_TAG,
                                      GDROM_OLD_METADATA_TAG, GDROM_TRACK_METADATA_TAG};
    char meta[256];
    for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); i++) {
        if (chd_get_metadata(chd, k_tags[i], 0, meta, sizeof(meta), NULL, NULL, NULL) == CHDERR_NONE) {
            return true;
        }
    }
    return false;
}

NcChd *nc_chd_open(int fd, bool *is_chd)
{
    char magic[8];
    *is_chd = pread(fd, magic, sizeof(magic), 0) == (ssize_t)sizeof(magic) && !memcmp(magic, "MComprHD", 8);
    if (!*is_chd) {
        return NULL;
    }
    NcChd *c = calloc(1, sizeof(*c));
    c->fd = fd;
    const chd_error e = chd_open_core_file_callbacks(&k_file, c, CHD_OPEN_READ, NULL, &c->chd);
    if (e != CHDERR_NONE) {
        nc_log("hakuX-native", "disc: CHD not opened: %s", e == CHDERR_REQUIRES_PARENT
                                                               ? "it's a delta of another CHD (a parent), not supported"
                                                               : chd_error_string(e));
        free(c);
        return NULL;
    }
    const chd_header *h = chd_get_header(c->chd);
    const char *why = !h->hunkbytes || h->hunkbytes > MAX_HUNK ? "an unusual hunk size"
                    : !h->logicalbytes                         ? "no data"
                    : is_cd(c->chd) ? "a CD image (chdman createcd); an Xbox disc is a DVD: chdman createdvd"
                                    : NULL;
    if (why) {
        nc_log("hakuX-native", "disc: CHD not read: %s", why);
        chd_close(c->chd);
        free(c);
        return NULL;
    }
    c->size = h->logicalbytes;
    c->hunk_bytes = h->hunkbytes;
    c->slots = CACHE_BYTES / c->hunk_bytes;
    c->slots = c->slots < 8 ? 8 : c->slots > 1024 ? 1024 : c->slots;
    c->tag = calloc(c->slots, sizeof(*c->tag));
    c->data = malloc((size_t)c->slots * c->hunk_bytes);
    pthread_mutex_init(&c->lock, NULL);
    char n[4][5];
    nc_log("hakuX-native", "disc: CHD v%u, %llu bytes in %u hunks of %u (%s, %s, %s, %s)", h->version,
           (unsigned long long)c->size, h->totalhunks, c->hunk_bytes, codec_name(h->compression[0], n[0]),
           codec_name(h->compression[1], n[1]), codec_name(h->compression[2], n[2]),
           codec_name(h->compression[3], n[3]));
    return c;
}

/* The hunk, decompressed into its slot unless it is there. Under c->lock. */
static const uint8_t *hunk(NcChd *c, uint32_t i)
{
    const unsigned s = i % c->slots;
    uint8_t *p = c->data + (size_t)s * c->hunk_bytes;
    if (c->tag[s] == i + 1) {
        return p;
    }
    c->tag[s] = 0;
    const chd_error e = chd_read(c->chd, i, p);
    if (e != CHDERR_NONE) {
        if (c->errors++ < 8) {
            nc_log("hakuX-native", "disc: CHD hunk %u: %s", i, chd_error_string(e));
        }
        return NULL;
    }
    c->tag[s] = i + 1;
    return p;
}

bool nc_chd_read(NcChd *c, void *buf, size_t len, uint64_t off)
{
    if (off > c->size || len > c->size - off) {
        return false;
    }
    uint8_t *out = buf;
    bool ok = true;
    pthread_mutex_lock(&c->lock);
    while (ok && len) {
        const uint32_t at = (uint32_t)(off % c->hunk_bytes);
        const size_t n = len < c->hunk_bytes - at ? len : c->hunk_bytes - at;
        const uint8_t *p = hunk(c, (uint32_t)(off / c->hunk_bytes));
        ok = p != NULL;
        if (ok) {
            memcpy(out, p + at, n);
            out += n;
            off += n;
            len -= n;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return ok;
}

void nc_chd_close(NcChd *c)
{
    if (!c) {
        return;
    }
    chd_close(c->chd);
    pthread_mutex_destroy(&c->lock);
    free(c->tag);
    free(c->data);
    free(c);
}

#else

NcChd *nc_chd_open(int fd, bool *is_chd)
{
    char magic[8];
    *is_chd = pread(fd, magic, sizeof(magic), 0) == (ssize_t)sizeof(magic) && !memcmp(magic, "MComprHD", 8);
    if (*is_chd) {
        nc_log("hakuX-native", "disc: a CHD image, and this build has no libchdr: ISO images only");
    }
    return NULL;
}

bool nc_chd_read(NcChd *c, void *buf, size_t len, uint64_t off)
{
    return false;
}

void nc_chd_close(NcChd *c)
{
}

#endif
