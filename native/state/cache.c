/*
 * hakuX native core — the title's cache drives in a savestate (CACHE).
 *
 * The Xbox's three cache partitions (X/Y/Z: Partition3-5) are shared: a
 * title that needs one takes the least recently used and formats it, so a
 * state's title found its cache gone once another game had run (Halo's
 * cache000.map, open as it saved). The state carries the drives its title
 * uses (its utility drive, and any it has a file open on): each file's
 * content goes to the game's blob store next to its states (blobs/, one
 * copy per content: the slots share what didn't change, XXH3-128 names
 * it), and the state lists them; partition 0's head goes too (the cache
 * table that says which title owns which drive). On load the drives become
 * what they were, before the open files are reopened (FILE), whatever the
 * game wrote to them since.
 *
 * A blob is gzip (level 1), <hash>.gz: the stores of the RP5's 9 games went
 * from 4.5 GB to 2.4 GB (GTA San Andreas' 650 MB drive to 21 MB, Dead or
 * Alive 2's to 87 %). Blobs stored before, <hash> as the file was, still
 * load; the hash is always the content's.
 *
 * A file counts as unchanged since it was hashed or restored only if its
 * size, its modification time and its change time (ctime) all are as they
 * were then: a game sets modification times itself (copies keep the disc's,
 * FileBasicInformation), and could rewrite a cache file at its old size and
 * time; the ctime moves with every write and every time set, and no one
 * sets it.
 */
#include "native/kernel/kernel.h"
#include "native/state/save.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

typedef struct NcCacheState {
    uint32_t nparts;
} NcCacheState;

typedef struct NcCachePart {
    uint32_t part, raw_len, ndirs, nfiles;
} NcCachePart;

typedef struct NcCacheDir {
    char path[256];
} NcCacheDir;

typedef struct NcCacheFile {
    char path[256];
    uint64_t size, mtime_ns;
    uint8_t hash[16];
} NcCacheFile;

#define NC_DEF_SECTION CACHE
#define NC_DEF_FILE "native/state/cache.def"
#include "native/state/def.h"
#include "native/state/sections.h"

#define RAW_MAX 0x80000u    /* io.c RAW_KEEP: the partition head a raw file holds */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------- the blob store */

/* blobs/index: what each cache file hashed to, at its size and time (a save
 * reads only the files that changed since) */
typedef struct IndexEntry {
    char key[264];          /* "<part>/<path>" */
    uint64_t size, mtime_ns, ctime_ns;
    uint8_t hash[16];
} IndexEntry;

#define INDEX_MAGIC 0x32495848u     /* "HXI2"; then sizeof(IndexEntry); an index otherwise is rebuilt */

static IndexEntry *g_idx;
static unsigned g_nidx, g_capidx;

/* The last path separator in p (Windows paths have either), NULL if none. */
static char *last_sep(char *p)
{
    char *s = strrchr(p, '/');
#ifdef _WIN32
    char *b = strrchr(p, '\\');
    if (b > s) {
        s = b;
    }
#endif
    return s;
}

static void blob_dir(char *out, size_t n)
{
    snprintf(out, n, "%s", nc_state_path());
    char *slash = last_sep(out);
    snprintf(slash ? slash + 1 : out, n - (size_t)(slash ? slash + 1 - out : 0), "blobs");
}

static void hex(const uint8_t h[16], char out[33])
{
    for (unsigned i = 0; i < 16; i++) {
        snprintf(out + 2 * i, 3, "%02x", h[i]);
    }
}

static void blob_path(const char *bdir, const uint8_t h[16], bool gz, char *out, size_t n)
{
    char x[33];
    hex(h, x);
    snprintf(out, n, "%s/%s%s", bdir, x, gz ? ".gz" : "");
}

/* A gzip file's last word: its content's size (mod 2^32). */
static bool gz_size_is(const char *p, uint64_t size)
{
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat sb;
    uint8_t t[4];
    const bool ok = fstat(fd, &sb) == 0 && sb.st_size >= 18 && pread(fd, t, 4, sb.st_size - 4) == 4 &&
                    (uint32_t)(t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24) == (uint32_t)size;
    close(fd);
    return ok;
}

enum { BLOB_NONE, BLOB_RAW, BLOB_GZ };

/* The store's copy of content h (size bytes): out its path. */
static int blob_find(const char *bdir, const uint8_t h[16], uint64_t size, char *out, size_t n)
{
    struct stat sb;
    blob_path(bdir, h, true, out, n);
    if (gz_size_is(out, size)) {
        return BLOB_GZ;
    }
    blob_path(bdir, h, false, out, n);
    if (stat(out, &sb) == 0 && (uint64_t)sb.st_size == size) {
        return BLOB_RAW;
    }
    return BLOB_NONE;
}

static void index_load(const char *bdir)
{
    g_nidx = 0;
    char p[700];
    snprintf(p, sizeof(p), "%s/index", bdir);
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        return;
    }
    uint32_t head[2] = {0, 0};
    if (fread(head, sizeof(head), 1, fp) != 1 || head[0] != INDEX_MAGIC || head[1] != sizeof(IndexEntry)) {
        fclose(fp);                     /* another layout: every file is hashed again */
        return;
    }
    IndexEntry e;
    while (fread(&e, sizeof(e), 1, fp) == 1) {
        if (g_nidx == g_capidx) {
            g_capidx = g_capidx ? g_capidx * 2 : 64;
            IndexEntry *q = realloc(g_idx, g_capidx * sizeof(*q));
            if (!q) {
                break;
            }
            g_idx = q;
        }
        e.key[sizeof(e.key) - 1] = 0;
        g_idx[g_nidx++] = e;
    }
    fclose(fp);
}

static void index_save(const char *bdir)
{
    char p[700], tmp[710];
    snprintf(p, sizeof(p), "%s/index", bdir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return;
    }
    const uint32_t head[2] = {INDEX_MAGIC, sizeof(IndexEntry)};
    const bool ok = fwrite(head, sizeof(head), 1, fp) == 1 && fwrite(g_idx, sizeof(IndexEntry), g_nidx, fp) == g_nidx;
    if (fclose(fp) == 0 && ok) {
        rename(tmp, p);
    } else {
        unlink(tmp);
    }
}

static IndexEntry *index_find(const char *key)
{
    for (unsigned i = 0; i < g_nidx; i++) {
        if (!strcmp(g_idx[i].key, key)) {
            return &g_idx[i];
        }
    }
    return NULL;
}

static void index_set(const char *key, uint64_t size, uint64_t mtime, uint64_t ctime, const uint8_t hash[16])
{
    IndexEntry *e = index_find(key);
    if (!e) {
        if (g_nidx == g_capidx) {
            g_capidx = g_capidx ? g_capidx * 2 : 64;
            IndexEntry *q = realloc(g_idx, g_capidx * sizeof(*q));
            if (!q) {
                return;
            }
            g_idx = q;
        }
        e = &g_idx[g_nidx++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->size = size;
    e->mtime_ns = mtime;
    e->ctime_ns = ctime;
    memcpy(e->hash, hash, 16);
}

static bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *q = p;
    while (n) {
        const ssize_t w = write(fd, q, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        q += w;
        n -= (size_t)w;
    }
    return true;
}

#define CHUNK (1u << 20)

/* A file gzipped on PZ_THREADS threads, PZ_PIECE bytes each, as pigz does:
 * each piece raw deflate primed with the 32 KiB before it and ended on a
 * byte boundary (a sync flush, the last on the final block), the pieces one
 * deflate stream in one gzip member (CRCs combined): any gunzip reads it.
 * On one thread Halo 2's 720 MiB of new cache took 9.2 s of a 9.6 s save. */
#define PZ_THREADS 4
#define PZ_PIECE (4u << 20)
#define PZ_DICT 32768u

typedef struct PzPiece {
    const uint8_t *in, *dict;
    size_t len, dict_len;
    bool last;
    uint8_t *out;
    size_t out_len, out_cap;
    uLong crc;
    bool ok;
    pthread_t th;
    bool started;
} PzPiece;

static void *pz_piece(void *arg)
{
    PzPiece *p = arg;
    z_stream s;
    memset(&s, 0, sizeof(s));
    p->ok = false;
    if (deflateInit2(&s, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    const size_t need = deflateBound(&s, p->len) + 16;
    if (need > p->out_cap) {
        free(p->out);
        p->out = malloc(need);
        p->out_cap = p->out ? need : 0;
    }
    if (p->out && (!p->dict_len || deflateSetDictionary(&s, p->dict, (uInt)p->dict_len) == Z_OK)) {
        s.next_in = (Bytef *)p->in;
        s.avail_in = (uInt)p->len;
        s.next_out = p->out;
        s.avail_out = (uInt)p->out_cap;
        const int r = deflate(&s, p->last ? Z_FINISH : Z_SYNC_FLUSH);
        p->ok = (p->last ? r == Z_STREAM_END : r == Z_OK) && s.avail_in == 0;
        p->out_len = p->out_cap - s.avail_out;
    }
    deflateEnd(&s);
    p->crc = crc32(0, p->in, (uInt)p->len);
    return NULL;
}

/* src (size bytes) to the gzip file out, hashed meanwhile (XXH3-128 into st) */
static bool pz_file(int in, uint64_t size, int out, XXH3_state_t *st)
{
    static const uint8_t head[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 3};
    uint8_t *buf = malloc(PZ_DICT + (size_t)PZ_THREADS * PZ_PIECE);
    PzPiece pc[PZ_THREADS];
    memset(pc, 0, sizeof(pc));
    bool ok = buf && write_all(out, head, sizeof(head));
    uLong crc = crc32(0, NULL, 0);
    uint64_t done = 0;
    size_t dict = 0;                            /* the last round's tail, at buf */
    while (ok) {
        /* a round: up to PZ_THREADS pieces after the dictionary bytes */
        uint8_t *data = buf + PZ_DICT;
        const uint64_t left = size - done, round = (uint64_t)PZ_THREADS * PZ_PIECE;
        size_t have = 0, want = (size_t)(left < round ? left : round);
        while (ok && have < want) {
            const ssize_t r = read(in, data + have, want - have);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            ok = r > 0;
            have += r > 0 ? (size_t)r : 0;
        }
        if (!ok) {
            break;
        }
        XXH3_128bits_update(st, data, have);
        const bool final = done + have == size;
        unsigned n = 0;
        for (size_t at = 0; at < have || (!n && final); at += PZ_PIECE, n++) {
            PzPiece *p = &pc[n];
            p->in = data + at;
            p->len = have - at < PZ_PIECE ? have - at : PZ_PIECE;
            p->dict_len = at ? PZ_DICT : dict;
            p->dict = p->in - p->dict_len;
            p->last = final && at + p->len == have;
            p->started = n && pthread_create(&p->th, NULL, pz_piece, p) == 0;
        }
        for (unsigned i = 0; i < n; i++) {
            if (!i || !pc[i].started) {
                pz_piece(&pc[i]);
            }
        }
        for (unsigned i = 0; i < n; i++) {
            if (pc[i].started) {
                pthread_join(pc[i].th, NULL);
            }
            ok = ok && pc[i].ok && write_all(out, pc[i].out, pc[i].out_len);
            crc = crc32_combine(crc, pc[i].crc, (z_off_t)pc[i].len);
        }
        done += have;
        if (final) {
            break;
        }
        dict = have < PZ_DICT ? have : PZ_DICT;
        memmove(buf + PZ_DICT - dict, data + have - dict, dict);
    }
    if (ok) {
        const uint8_t tail[8] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
                                 (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24)};
        ok = write_all(out, tail, sizeof(tail));
    }
    for (unsigned i = 0; i < PZ_THREADS; i++) {
        free(pc[i].out);
    }
    free(buf);
    return ok;
}

/* src hashed, and its bytes stored as a gzip blob unless that content is
 * there; *stored, *packed: bytes new to the store, as read and as written. */
static bool store_file(const char *src, const char *bdir, uint8_t hash[16], uint64_t *stored, uint64_t *packed)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", bdir);
    const int in = open(src, O_RDONLY | O_CLOEXEC);
    const int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    struct stat isb;
    XXH3_state_t *st = XXH3_createState();
    bool ok = in >= 0 && out >= 0 && fstat(in, &isb) == 0 && st && XXH3_128bits_reset(st) == XXH_OK &&
              pz_file(in, (uint64_t)isb.st_size, out, st);
    const uint64_t n = ok ? (uint64_t)isb.st_size : 0;
    if (ok) {
        XXH128_canonical_t c;
        XXH128_canonicalFromHash(&c, XXH3_128bits_digest(st));
        memcpy(hash, c.digest, 16);
    }
    if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        ok = close(out) == 0 && ok;
    }
    if (st) {
        XXH3_freeState(st);
    }
    char final[700];
    struct stat sb;
    if (ok && blob_find(bdir, hash, n, final, sizeof(final)) != BLOB_NONE) {
        unlink(tmp);                        /* that content is stored already */
    } else if (ok) {
        blob_path(bdir, hash, true, final, sizeof(final));
        *packed += stat(tmp, &sb) == 0 ? (uint64_t)sb.st_size : 0;
        ok = rename(tmp, final) == 0;
        *stored += n;
    } else {
        unlink(tmp);
    }
    return ok;
}

/* Blob src (BLOB_RAW, BLOB_GZ: its CRC and length checked) to dst, which
 * gets size bytes. */
static bool copy_blob(const char *src, int kind, uint64_t size, const char *dst)
{
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.state-tmp", dst);
    const int in = open(src, O_RDONLY | O_CLOEXEC);
    gzFile gz = in >= 0 && kind == BLOB_GZ ? gzdopen(in, "rb") : NULL;   /* closes in */
    const int out = in >= 0 ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664) : -1;
    uint8_t *buf = malloc(CHUNK);
    bool ok = in >= 0 && (kind != BLOB_GZ || (gz && gzbuffer(gz, 256u << 10) == 0)) && out >= 0 && buf;
    uint64_t n = 0;
    while (ok) {
        const ssize_t r = gz ? gzread(gz, buf, CHUNK) : read(in, buf, CHUNK);
        if (r < 0 && !gz && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            ok = r == 0;
            break;
        }
        ok = write_all(out, buf, (size_t)r);
        n += (uint64_t)r;
    }
    if (gz) {
        ok = gzclose(gz) == Z_OK && ok;
    } else if (in >= 0) {
        close(in);
    }
    if (out >= 0) {
        ok = close(out) == 0 && ok;
    }
    free(buf);
    ok = ok && n == size && rename(tmp, dst) == 0;
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

/* ------------------------------------------------------------ the drives */

typedef struct Walk {
    NcCacheDir *dirs;
    unsigned ndirs, capd;
    NcCacheFile *files;
    uint64_t *ctime;        /* per file: its change time (not stored in the state) */
    unsigned nfiles, capf;
    bool fail;              /* out of memory: the lists are incomplete */
} Walk;

static uint64_t ts_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

/* root's folders and files below rel, in pre-order (a folder before what's in it) */
static void walk(const char *root, const char *rel, Walk *w)
{
    char path[700];
    snprintf(path, sizeof(path), "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    struct dirent *de;
    while (!w->fail && (de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char sub[256];
        if (snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name) >= (int)sizeof(sub)) {
            continue;
        }
        char full[960];
        snprintf(full, sizeof(full), "%s/%s", root, sub);
        struct stat sb;
        if (lstat(full, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            if (w->ndirs == w->capd) {
                const unsigned cap = w->capd ? w->capd * 2 : 16;
                NcCacheDir *dirs = realloc(w->dirs, cap * sizeof(*dirs));
                if (!dirs) {
                    w->fail = true;
                    break;
                }
                w->dirs = dirs;
                w->capd = cap;
            }
            snprintf(w->dirs[w->ndirs++].path, 256, "%s", sub);
            walk(root, sub, w);
        } else if (S_ISREG(sb.st_mode)) {
            if (w->nfiles == w->capf) {
                const unsigned cap = w->capf ? w->capf * 2 : 32;
                NcCacheFile *files = realloc(w->files, cap * sizeof(*files));
                if (files) {
                    w->files = files;
                }
                uint64_t *ctime = files ? realloc(w->ctime, cap * sizeof(*ctime)) : NULL;
                if (!ctime) {
                    w->fail = true;
                    break;
                }
                w->ctime = ctime;
                w->capf = cap;
            }
#ifdef _WIN32
            uint64_t mt_ns = 0, ct_ns = 0;
            nc_stat_ns(full, &mt_ns, &ct_ns);
            w->ctime[w->nfiles] = ct_ns;
#else
            w->ctime[w->nfiles] = ts_ns(&sb.st_ctim);
#endif
            NcCacheFile *f = &w->files[w->nfiles++];
            memset(f, 0, sizeof(*f));
            snprintf(f->path, sizeof(f->path), "%s", sub);
            f->size = (uint64_t)sb.st_size;
#ifdef _WIN32
            f->mtime_ns = mt_ns;
#else
            f->mtime_ns = ts_ns(&sb.st_mtim);
#endif
        }
    }
    closedir(d);
}

static void walk_free(Walk *w)
{
    free(w->dirs);
    free(w->files);
    free(w->ctime);
    memset(w, 0, sizeof(*w));
}

static uint32_t raw_read(int part, uint8_t *buf)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/raw-partition%d.bin", nc_io_hdd_root(), part);
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    const ssize_t n = pread(fd, buf, RAW_MAX, 0);
    close(fd);
    return n > 0 ? (uint32_t)n : 0;
}

bool nc_cache_state_save(NcBuf *b)
{
    const uint64_t t0 = mono_ns();
    const unsigned mask = nc_io_cache_partitions() | 1u;     /* partition 0: the cache table */
    char bdir[600];
    blob_dir(bdir, sizeof(bdir));
    mkdir(bdir, 0700);
    index_load(bdir);
    NcCacheState st = {(uint32_t)__builtin_popcount(mask)};
    nc_rec_put(b, &nc_rec_NcCacheState, &st);
    uint8_t *raw = malloc(RAW_MAX);
    bool ok = raw != NULL;
    unsigned files = 0;
    uint64_t bytes = 0, stored = 0, packed = 0;
    for (int p = 0; ok && p <= 5; p++) {
        if (!(mask & (1u << p))) {
            continue;
        }
        Walk w = {0};
        char proot[600];
        snprintf(proot, sizeof(proot), "%s/Partition%d", nc_io_hdd_root(), p);
        if (p >= 3) {
            walk(proot, "", &w);
        }
        if (w.fail) {
            nc_log("hakuX-native-state", "cache: out of memory listing %s", proot);
            ok = false;                         /* not a state missing files */
        }
        for (unsigned i = 0; ok && i < w.nfiles; i++) {
            NcCacheFile *f = &w.files[i];
            char key[264], full[960], blob[700];
            snprintf(key, sizeof(key), "%d/%s", p, f->path);
            snprintf(full, sizeof(full), "%s/%s", proot, f->path);
            const IndexEntry *e = index_find(key);
            if (e && e->size == f->size && e->mtime_ns == f->mtime_ns && e->ctime_ns == w.ctime[i] &&
                blob_find(bdir, e->hash, f->size, blob, sizeof(blob)) != BLOB_NONE) {
                memcpy(f->hash, e->hash, 16);       /* unchanged since it was stored */
                continue;
            }
            ok = store_file(full, bdir, f->hash, &stored, &packed);
            if (ok) {
                index_set(key, f->size, f->mtime_ns, w.ctime[i], f->hash);
            } else {
                nc_log("hakuX-native-state", "cache: %s can't be stored: %s", full, strerror(errno));
            }
        }
        const NcCachePart cp = {(uint32_t)p, raw_read(p, raw), w.ndirs, w.nfiles};
        nc_rec_put(b, &nc_rec_NcCachePart, &cp);
        nc_buf_put(b, raw, cp.raw_len);
        nc_recs_put(b, &nc_rec_NcCacheDir, w.dirs, sizeof(NcCacheDir), w.ndirs);
        nc_recs_put(b, &nc_rec_NcCacheFile, w.files, sizeof(NcCacheFile), w.nfiles);
        for (unsigned i = 0; i < w.nfiles; i++) {
            bytes += w.files[i].size;
        }
        files += w.nfiles;
        walk_free(&w);
    }
    free(raw);
    index_save(bdir);
    nc_log("hakuX-native-state",
           "cache: drives %s%s%s, %u files, %.1f MiB (%.1f MiB new to the store, %.1f MiB gzipped) in %.0f ms",
           mask & 8 ? "X" : "", mask & 16 ? "Y" : "", mask & 32 ? "Z" : "", files, bytes / 1048576.0,
           stored / 1048576.0, packed / 1048576.0, (mono_ns() - t0) / 1e6);
    return ok;
}

static bool in_dirs(const NcCacheDir *dirs, unsigned n, const char *path)
{
    for (unsigned i = 0; i < n; i++) {
        if (!strcmp(dirs[i].path, path)) {
            return true;
        }
    }
    return false;
}

static int in_files_at(const NcCacheFile *files, unsigned n, const char *path)
{
    for (unsigned i = 0; i < n; i++) {
        if (!strcmp(files[i].path, path)) {
            return (int)i;
        }
    }
    return -1;
}

static const NcCacheFile *in_files(const NcCacheFile *files, unsigned n, const char *path)
{
    const int i = in_files_at(files, n, path);
    return i >= 0 ? &files[i] : NULL;
}

/* Partition p's folder made what the state lists: what isn't in it goes,
 * what changed comes back from the store. */
static bool restore_drive(int p, const NcCacheDir *dirs, unsigned ndirs, const NcCacheFile *files, unsigned nfiles,
                          const char *bdir, uint64_t *copied)
{
    char proot[600];
    snprintf(proot, sizeof(proot), "%s/Partition%d", nc_io_hdd_root(), p);
    mkdir(proot, 0775);
    Walk cur = {0};
    walk(proot, "", &cur);
    for (unsigned i = 0; i < cur.nfiles; i++) {
        if (!in_files(files, nfiles, cur.files[i].path)) {
            char full[960];
            snprintf(full, sizeof(full), "%s/%s", proot, cur.files[i].path);
            unlink(full);
        }
    }
    for (unsigned i = cur.ndirs; i-- > 0;) {        /* the deepest first */
        if (!in_dirs(dirs, ndirs, cur.dirs[i].path)) {
            char full[960];
            snprintf(full, sizeof(full), "%s/%s", proot, cur.dirs[i].path);
            rmdir(full);
        }
    }
    for (unsigned i = 0; i < ndirs; i++) {
        char full[960];
        snprintf(full, sizeof(full), "%s/%s", proot, dirs[i].path);
        mkdir(full, 0775);
    }
    bool ok = true;
    for (unsigned i = 0; ok && i < nfiles; i++) {
        const NcCacheFile *f = &files[i];
        char key[264], full[960], blob[700];
        snprintf(key, sizeof(key), "%d/%s", p, f->path);
        snprintf(full, sizeof(full), "%s/%s", proot, f->path);
        const int at = in_files_at(cur.files, cur.nfiles, f->path);
        const IndexEntry *e = index_find(key);
        /* as saved: the index says it held this content, and nothing touched
         * it since (size, times, ctime as the index has them) */
        if (at >= 0 && e && !memcmp(e->hash, f->hash, 16) && e->size == cur.files[at].size &&
            e->mtime_ns == cur.files[at].mtime_ns && e->ctime_ns == cur.ctime[at]) {
            continue;
        }
        const int kind = blob_find(bdir, f->hash, f->size, blob, sizeof(blob));
        if (kind == BLOB_NONE) {
            nc_log("hakuX-native-state", "cache: %s's copy (%s[.gz]) is missing", full, blob);
            nc_state_fail("the state's copy of the game's cache is missing (%s)", f->path);
            ok = false;
            break;
        }
        ok = copy_blob(blob, kind, f->size, full);
        if (ok) {
            const struct timespec ts[2] = {{0, UTIME_OMIT},
                                           {(time_t)(f->mtime_ns / 1000000000ull), (long)(f->mtime_ns % 1000000000ull)}};
            utimensat(AT_FDCWD, full, ts, 0);
            struct stat now;
#ifdef _WIN32
            uint64_t ct_ns = 0;
            index_set(key, f->size, f->mtime_ns, stat(full, &now) == 0 && !nc_stat_ns(full, NULL, &ct_ns) ? ct_ns : 0,
                      f->hash);
#else
            index_set(key, f->size, f->mtime_ns, stat(full, &now) == 0 ? ts_ns(&now.st_ctim) : 0, f->hash);
#endif
            *copied += f->size;
        } else {
            nc_state_fail("the game's cache file %s can't be written back (%s)", f->path, strerror(errno));
        }
    }
    walk_free(&cur);
    return ok;
}

bool nc_cache_state_load(NcRd *r, unsigned v)
{
    const uint64_t t0 = mono_ns();
    char bdir[600];
    blob_dir(bdir, sizeof(bdir));
    index_load(bdir);
    NcCacheState st = {0};
    nc_rec_get(r, &nc_rec_NcCacheState, &st, v);
    bool ok = !r->fail && st.nparts <= 6;
    uint64_t copied = 0;
    unsigned files = 0;
    for (uint32_t i = 0; ok && i < st.nparts; i++) {
        NcCachePart cp = {0};
        nc_rec_get(r, &nc_rec_NcCachePart, &cp, v);
        const void *raw = nc_rd_ptr(r, cp.raw_len);
        NcCacheDir *dirs = calloc(cp.ndirs ? cp.ndirs : 1, sizeof(*dirs));
        NcCacheFile *fl = calloc(cp.nfiles ? cp.nfiles : 1, sizeof(*fl));
        ok = !r->fail && cp.part <= 5 && cp.raw_len <= RAW_MAX && dirs && fl;
        if (ok) {
            nc_recs_get(r, &nc_rec_NcCacheDir, dirs, sizeof(*dirs), cp.ndirs, v);
            nc_recs_get(r, &nc_rec_NcCacheFile, fl, sizeof(*fl), cp.nfiles, v);
            for (unsigned k = 0; k < cp.ndirs; k++) {
                dirs[k].path[sizeof(dirs[k].path) - 1] = 0;
            }
            for (unsigned k = 0; k < cp.nfiles; k++) {
                fl[k].path[sizeof(fl[k].path) - 1] = 0;
            }
            ok = !r->fail;
        }
        if (ok) {
            /* the partition's head (the FATX superblock, the cache table) */
            char p[600];
            snprintf(p, sizeof(p), "%s/raw-partition%u.bin", nc_io_hdd_root(), cp.part);
            if (cp.raw_len) {
                const int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
                ok = fd >= 0 && write_all(fd, raw, cp.raw_len);
                if (fd >= 0) {
                    close(fd);
                }
            } else {
                unlink(p);
            }
        }
        if (ok && cp.part >= 3) {
            ok = restore_drive((int)cp.part, dirs, cp.ndirs, fl, cp.nfiles, bdir, &copied);
            files += cp.nfiles;
        }
        free(dirs);
        free(fl);
    }
    index_save(bdir);
    nc_log("hakuX-native-state", "cache: %u files on the drives as saved, %.1f MiB copied back in %.0f ms", files,
           copied / 1048576.0, (mono_ns() - t0) / 1e6);
    return ok;
}

/* ------------------------------------------------------------- the store */

typedef struct Refs {
    uint8_t (*h)[16];
    unsigned n, cap;
} Refs;

static bool collect(NcRd *r, unsigned v, void *ctx)
{
    Refs *refs = ctx;
    NcCacheState st = {0};
    nc_rec_get(r, &nc_rec_NcCacheState, &st, v);
    for (uint32_t i = 0; i < st.nparts && !r->fail; i++) {
        NcCachePart cp = {0};
        nc_rec_get(r, &nc_rec_NcCachePart, &cp, v);
        nc_rd_ptr(r, cp.raw_len);
        static NcCacheDir dir;
        for (uint32_t k = 0; k < cp.ndirs && !r->fail; k++) {
            nc_rec_get(r, &nc_rec_NcCacheDir, &dir, v);
        }
        static NcCacheFile f;
        for (uint32_t k = 0; k < cp.nfiles && !r->fail; k++) {
            nc_rec_get(r, &nc_rec_NcCacheFile, &f, v);
            if (refs->n == refs->cap) {
                const unsigned cap = refs->cap ? refs->cap * 2 : 64;
                uint8_t (*h)[16] = realloc(refs->h, cap * 16);
                if (!h) {
                    r->fail = true;             /* no list: no blob is let go */
                    break;
                }
                refs->h = h;
                refs->cap = cap;
            }
            memcpy(refs->h[refs->n++], f.hash, 16);
        }
    }
    return !r->fail;
}

void nc_cache_gc(const char *state_path)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", state_path);
    char *slash = last_sep(dir);
    if (!slash) {
        return;
    }
    *slash = 0;
    Refs refs = {0};
    DIR *d = opendir(dir);
    struct dirent *de;
    bool ok = d != NULL;
    while (ok && (de = readdir(d))) {
        const size_t len = strlen(de->d_name);
        if (len > 4 && !strcmp(de->d_name + len - 4, ".hxs")) {
            char p[900];
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            /* a state that can't be read keeps every blob (nothing removed) */
            ok = nc_state_read_section(p, NC_TAG('C', 'A', 'C', 'H'), collect, &refs) == NULL;
        }
    }
    if (d) {
        closedir(d);
    }
    char bdir[700];
    snprintf(bdir, sizeof(bdir), "%s/blobs", dir);
    DIR *b = ok ? opendir(bdir) : NULL;
    unsigned removed = 0;
    uint64_t freed = 0;
    while (b && (de = readdir(b))) {
        const size_t len = strlen(de->d_name);
        if (len != 32 && (len != 35 || strcmp(de->d_name + 32, ".gz"))) {
            continue;                       /* <hash>, <hash>.gz only */
        }
        bool used = false;
        for (unsigned i = 0; i < refs.n && !used; i++) {
            char x[33];
            hex(refs.h[i], x);
            used = !memcmp(x, de->d_name, 32);
        }
        if (!used) {
            char p[800];
            snprintf(p, sizeof(p), "%s/%s", bdir, de->d_name);
            struct stat sb;
            freed += stat(p, &sb) == 0 ? (uint64_t)sb.st_size : 0;
            removed += unlink(p) == 0;
        }
    }
    if (b) {
        closedir(b);
    }
    free(refs.h);
    if (removed) {
        nc_log("hakuX-native-state", "cache: %u copies no state uses removed (%.1f MiB)", removed, freed / 1048576.0);
    }
}
