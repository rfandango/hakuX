/*
 * hakuX native core — files, devices and the object namespace.
 *
 * Devices: \Device\CdRom0 is the title's disc (XDVDFS, read-only);
 * \Device\Harddisk0\PartitionN are host directories (hdd root/PartitionN),
 * matched case-insensitively as FATX does. \??\X: names are symbolic links
 * (IoCreateSymbolicLink); D: starts out at the disc.
 *
 * Xbox names are ANSI: OBJECT_ATTRIBUTES { RootDirectory, ObjectName
 * (ANSI_STRING*), Attributes }; FILE_DIRECTORY_INFORMATION carries ANSI
 * names. I/O completes synchronously: IoStatusBlock and the event are set
 * before the call returns; APC completion routines are not delivered yet.
 */
#include "native/kernel/kernel.h"
#include "native/core/irq.h"
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define STATUS_NO_MORE_FILES          0x80000006u
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_END_OF_FILE            0xC0000011u
#define STATUS_NO_SUCH_FILE           0xC000000Fu
#define STATUS_ACCESS_DENIED          0xC0000022u
#define STATUS_BUFFER_TOO_SMALL       0xC0000023u
#define STATUS_OBJECT_NAME_INVALID    0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION  0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND  0xC000003Au
#define STATUS_DELETE_PENDING         0xC0000056u
#define STATUS_MEDIA_WRITE_PROTECTED  0xC00000A2u
#define STATUS_FILE_IS_A_DIRECTORY    0xC00000BAu
#define STATUS_NOT_A_DIRECTORY        0xC0000103u
#define STATUS_DIRECTORY_NOT_EMPTY    0xC0000101u

#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5
#define FILE_DIRECTORY_FILE     0x01u
#define FILE_SYNCHRONOUS_IO     0x30u /* _ALERT 0x10, _NONALERT 0x20 */
#define FILE_NON_DIRECTORY_FILE 0x40u
#define FILE_NO_INTERMEDIATE_BUFFERING 0x08u
#define STATUS_PENDING          0x00000103u

/* OBJECT_ATTRIBUTES.RootDirectory pseudo-handles (ObDosDevicesDirectory). */
#define OB_DOS_DEVICES 0xFFFFFFFDu

#define FILE_ATTRIBUTE_READONLY  0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL    0x80u

#define DISC_TIME 0x01C0739BEB8F8000ull /* 2001-01-01 */

enum FileKind { F_DISC, F_HOST, F_VOLUME, F_LINK };
struct DirEntry;

typedef struct NcFile {
    bool used;
    enum FileKind kind;
    char path[512];      /* canonical device path */
    NcDiscFile disc;     /* F_DISC */
    char host[512];      /* F_HOST: host path */
    int fd;              /* F_HOST files */
    bool is_dir;
    uint64_t pos;
    unsigned dir_next;   /* directory enumeration */
    char mask[128];
    int partition;       /* F_VOLUME: -1 disc, else partition number */
    /* Opened with an overwrite disposition: the old contents moved to a side
     * file outside the partitions, and the file itself is empty on the host
     * from the start, so the disk always holds what the guest sees. FATX
     * doesn't zero what a file grows back into: growing it again (SetEndOfFile,
     * a write past the end) brings the old bytes back in that range (Forza
     * trims its unpacked cabs so). The side file goes when the handle closes;
     * one a dead process left behind is swept at the next overwrite. */
    char old_path[320];  /* "" = none */
    uint64_t old_size;
    bool wrote;          /* written through this handle: synced when it closes */
    /* A directory's enumeration works on a snapshot taken when it starts:
     * XDeleteSaveGame deletes each file as the listing returns it, and a live
     * listing shifted under it and skipped every other file. */
    struct DirEntry *listing;
    unsigned nlisting;
    /* Opened without FILE_SYNCHRONOUS_IO_*: a read or write with an event or
     * APC (on the disc, any) that completes reports STATUS_PENDING, as the
     * Xbox's DMA disk and DVD transfers do, with its result in the
     * IoStatusBlock (io_finish); a write only with no_buffering. */
    bool async;
    /* The handle may change the file's data or size (its DesiredAccess has
     * FILE_WRITE_DATA / FILE_APPEND_DATA, GENERIC_WRITE / _ALL or
     * MAXIMUM_ALLOWED): writes and SetEndOfFile through one that can't are
     * refused, as the Xbox kernel refuses them (handle_can_write). */
    bool can_write;
    /* Deleted through this handle (FileDispositionInformation): the file
     * stays, as the Xbox's does, until the last handle on it closes. Sudeki
     * deletes its boot.baf with a handle on it still open: unlinked at
     * once, a savestate's copy of the drive left it out, and the state
     * couldn't reopen it. */
    bool del;
    /* Opened with FILE_NO_INTERMEDIATE_BUFFERING: its writes go to the disk
     * and pend (async). Without it they go through the file system's cache
     * and complete in the call, as on the Xbox: Breakdown copies an area to
     * its cache partition in 16 KiB overlapped writes and takes a write that
     * pends for a failed one; its copy stopped after the first, and the area
     * then loaded from a file of zeros (no room drawn, nor in cutscenes). */
    bool no_buffering;
} NcFile;

/* DesiredAccess bits that grant writing the data. */
#define ACCESS_WRITES (0x00000002u /* FILE_WRITE_DATA */ | 0x00000004u /* FILE_APPEND_DATA */ | \
                       0x40000000u /* GENERIC_WRITE */ | 0x10000000u /* GENERIC_ALL */ |       \
                       0x02000000u /* MAXIMUM_ALLOWED */)

/* Open files at once: the handle table's size. Fatal Frame opens most of its
 * disc's 1821 files as it boots and keeps them open; at 1024 the rest failed
 * (insufficient resources), and the load waited for one forever. Untouched
 * slots cost nothing: a slot is cleared as it is taken (file_new). */
#define MAX_FILES NC_MAX_HANDLES
#define MAX_LINKS 64
/* FILE_OBJECT as the Xbox lays it out: Type +0, FsContext +8, Lock (a
 * KEVENT) +0x28, Event (a KEVENT, NC_FO_EVENT) +0x38; past its 0x48 bytes,
 * our file index, then the file system's control block FsContext points at.
 * XAPI's XGetFilePhysicalSortKey reads it by the volume's name: GDFX, the
 * start sector at +0; FATX, flags at +0 (bit 0: no key) and the first
 * cluster at +0x1C. */
#define FILE_OBJECT_SIZE 0x70u
#define FO_FSCONTEXT 0x08u
#define FO_LOCK 0x28u
#define FO_INDEX 0x48u
#define FO_FCB 0x4Cu
#define FCB_FATX_FLAGS 0x00u
#define FCB_FATX_CLUSTER 0x1Cu

static NcFile g_files[MAX_FILES];
typedef struct NcLink {
    char name[64];
    char target[256];
} NcLink;

static NcLink g_links[MAX_LINKS];
static char g_hdd_root[256] = "/data/local/tmp/native/hdd";

/* The retail hard disk's layout (8 GB, 512-byte sectors): partition 0 is the
 * config area, 1 the data drive (E:), 2 the system drive (C:), 3-5 the
 * title cache drives. Offset and length per partition. */
#define HDD_BYTES 0x1DD156000ull
/* Sectors per cluster a partition was last formatted with (its FATX superblock). */
static uint32_t g_cluster_sectors[6];
static const uint64_t k_partitions[6][2] = {
    {0x00000000ull, 0x00080000ull},  {0xABE80000ull, 0x1312D6000ull}, {0x8CA80000ull, 0x1F400000ull},
    {0x00080000ull, 0x2EE00000ull}, {0x2EE80000ull, 0x2EE00000ull},  {0x5DC80000ull, 0x2EE00000ull},
};
static bool g_trace_io; /* NC_TRACE_IO: every read */

void nc_io_set_hdd_root(const char *path)
{
    snprintf(g_hdd_root, sizeof(g_hdd_root), "%s", path);
}

const char *nc_io_hdd_root(void)
{
    return g_hdd_root;
}

/* ------------------------------------------------------------- strings */

static bool prefix_ci(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void ansi(NcKernel *k, uint32_t str, char *out, size_t cap)
{
    out[0] = 0;
    if (!str) {
        return;
    }
    uint16_t len = *(uint16_t *)nc_vm_host(k->vm, str);
    uint32_t buf = nc_vm_rd32(k->vm, str + 4);
    if (len >= cap) {
        len = (uint16_t)(cap - 1);
    }
    memcpy(out, nc_vm_host(k->vm, buf), len);
    out[len] = 0;
}

static bool wildcard(const char *pat, const char *s)
{
    if (!*pat) {
        return !*s;
    }
    if (*pat == '*') {
        for (;; s++) {
            if (wildcard(pat + 1, s)) {
                return true;
            }
            if (!*s) {
                return false;
            }
        }
    }
    return *s && (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*s)) &&
           wildcard(pat + 1, s + 1);
}

/* ----------------------------------------------------------- namespace */

static int link_find(const char *name)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (g_links[i].name[0] && strcasecmp(g_links[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool link_set(const char *name, const char *target)
{
    int i = link_find(name);
    for (int j = 0; i < 0 && j < MAX_LINKS; j++) {
        if (!g_links[j].name[0]) {
            i = j;
        }
    }
    if (i < 0) {
        return false;
    }
    snprintf(g_links[i].name, sizeof(g_links[i].name), "%s", name);
    snprintf(g_links[i].target, sizeof(g_links[i].target), "%s", target);
    nc_log("hakuX-native-io", "link %s -> %s", name, target);
    return true;
}

/* Canonical device path: '\' separators, no duplicate separators, '.' and
 * '..' folded, \??\ links resolved. */
static bool canon(const char *in, char *out, size_t cap)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", in);
    for (char *p = tmp; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    if (prefix_ci(tmp, "\\??\\") && isalpha((unsigned char)tmp[4]) && tmp[5] == ':') {
        char name[8] = {'\\', '?', '?', '\\', (char)toupper((unsigned char)tmp[4]), ':', 0};
        int l = link_find(name);
        if (l < 0) {
            return false;
        }
        char joined[512];
        snprintf(joined, sizeof(joined), "%s\\%s", g_links[l].target, tmp + 6);
        snprintf(tmp, sizeof(tmp), "%s", joined);
    }
    /* Fold components. */
    char *parts[64];
    int n = 0;
    for (char *tok = strtok(tmp, "\\"); tok; tok = strtok(NULL, "\\")) {
        if (!strcmp(tok, ".")) {
            continue;
        }
        if (!strcmp(tok, "..")) {
            if (n) {
                n--;
            }
            continue;
        }
        if (n < 64) {
            parts[n++] = tok;
        }
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len < cap - 2; i++) {
        len += (size_t)snprintf(out + len, cap - len, "\\%s", parts[i]);
    }
    if (!n) {
        snprintf(out, cap, "\\");
    }
    return true;
}

/* Which device a canonical path is on: -1 the disc, 0..7 a partition, -2
 * unknown. rel receives the path inside the device ("" for its root). */
static int device_of(const char *path, const char **rel)
{
    static const char cd[] = "\\Device\\CdRom0";
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    if (prefix_ci(path, cd) && (!path[sizeof(cd) - 1] || path[sizeof(cd) - 1] == '\\')) {
        *rel = path + sizeof(cd) - 1;
        return -1;
    }
    if (prefix_ci(path, hd) && isdigit((unsigned char)path[sizeof(hd) - 1])) {
        const char *p = path + sizeof(hd) - 1;
        int part = *p - '0';
        p++;
        if (*p && *p != '\\') {
            return -2;
        }
        *rel = p;
        return part;
    }
    return -2;
}

static bool mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    return mkdir(tmp, 0775) == 0 || errno == EEXIST;
}

/* Host path for a path inside partition part, matching existing names
 * case-insensitively. Returns 1 if it exists, 0 if only the last component is
 * missing, -1 if a parent is missing. */
static int host_path(int part, const char *rel, char *out, size_t cap)
{
    size_t len = (size_t)snprintf(out, cap, "%s/Partition%d", g_hdd_root, part);
    mkdirs(out);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        const bool last = !save || !*save;
        DIR *d = opendir(out);
        const char *match = NULL;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strcasecmp(de->d_name, tok) == 0) {
                match = de->d_name;
                break;
            }
        }
        len += (size_t)snprintf(out + len, cap - len, "/%s", match ? match : tok);
        if (d) {
            closedir(d);
        }
        if (!match) {
            return last ? 0 : -1;
        }
    }
    return 1;
}

/* --------------------------------------------------------- file objects */

static NcFile *file_new(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            g_files[i].used = true;
            g_files[i].fd = -1;
            return &g_files[i];
        }
    }
    return NULL;
}

/* Another open handle on the host file, or NULL; with del: one that
 * deleted it. */
static NcFile *file_other(const NcFile *nf, const char *host, bool del)
{
    for (int i = 0; i < MAX_FILES; i++) {
        NcFile *o = &g_files[i];
        if (o != nf && o->used && o->kind == F_HOST && !o->is_dir && (o->del || !del) &&
            !strcmp(o->host, host)) {
            return o;
        }
    }
    return NULL;
}

static uint32_t file_handle(NcKernel *k, NcFile *nf)
{
    uint32_t obj = nc_ob_create(k, nf->kind == F_LINK ? NC_OB_SYMLINK : NC_OB_FILE, FILE_OBJECT_SIZE);
    if (!obj) {
        return 0;
    }
    *(uint16_t *)nc_vm_host(k->vm, obj) = 5; /* IO_TYPE_FILE */
    nc_ke_init_event(k, obj + FO_LOCK, 1, false);
    nc_ke_init_event(k, obj + NC_FO_EVENT, 0, false);
    nc_vm_wr32(k->vm, obj + FO_INDEX, (uint32_t)(nf - g_files));
    const uint32_t fcb = obj + FO_FCB;
    nc_vm_wr32(k->vm, obj + FO_FSCONTEXT, fcb);
    if (nf->kind == F_DISC) {
        nc_vm_wr32(k->vm, fcb, nf->disc.sector);
    } else if (nf->kind == F_HOST && !nf->is_dir) {
        /* Host files have no clusters: the inode orders them stably. */
        struct stat st;
        nc_vm_wr32(k->vm, fcb + FCB_FATX_CLUSTER, stat(nf->host, &st) == 0 ? (uint32_t)st.st_ino : 0);
    } else {
        nc_vm_wr32(k->vm, fcb + FCB_FATX_FLAGS, 1);
    }
    return nc_ob_handle(k, obj, nf->kind == F_LINK ? NC_OB_SYMLINK : NC_OB_FILE);
}

static NcFile *file_of(NcKernel *k, uint32_t handle)
{
    enum NcObType type;
    uint32_t obj = nc_ob_lookup(k, handle, &type);
    if (!obj || (type != NC_OB_FILE && type != NC_OB_SYMLINK)) {
        return NULL;
    }
    uint32_t i = nc_vm_rd32(k->vm, obj + FO_INDEX);
    return i < MAX_FILES && g_files[i].used ? &g_files[i] : NULL;
}

/* What the title wrote is on the disk soon after it closes the file, so an
 * app killed or hung later can't leave a half-written save: the closer
 * thread syncs it, closes it and drops its overwritten contents, in that
 * order. On the guest's CPU the sync took 10-110 ms per file with nothing
 * else running, interrupts included: Forza's frames came out late while it
 * wrote its caches and replays. Reopening the path meanwhile reads the same
 * data (the page cache). A full queue closes in place. */
typedef struct CloseJob {
    int fd;
    char path[64];       /* for the log */
    char old_path[320];
} CloseJob;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    CloseJob q[64];
    unsigned head, n;
    bool started, quit;     /* quit: nc_io_stop (what is queued still closes first) */
    pthread_t th;
} g_closer = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void close_now(const CloseJob *j)
{
    if (fsync(j->fd) != 0) {
        nc_log("hakuX-native-io", "sync %s: %s", j->path, strerror(errno));
    }
    close(j->fd);
    if (j->old_path[0]) {
        unlink(j->old_path);
    }
}

static void *closer_thread(void *arg)
{
    pthread_mutex_lock(&g_closer.mu);
    for (;;) {
        while (!g_closer.n && !g_closer.quit) {
            pthread_cond_wait(&g_closer.cv, &g_closer.mu);
        }
        if (!g_closer.n) {
            break;
        }
        const CloseJob j = g_closer.q[g_closer.head];
        g_closer.head = (g_closer.head + 1) % 64u;
        g_closer.n--;
        pthread_mutex_unlock(&g_closer.mu);
        close_now(&j);
        pthread_mutex_lock(&g_closer.mu);
    }
    pthread_mutex_unlock(&g_closer.mu);
    return NULL;
}

static void close_later(const NcFile *nf)
{
    CloseJob j = {.fd = nf->fd};
    snprintf(j.path, sizeof(j.path), "%s", nf->path);
    snprintf(j.old_path, sizeof(j.old_path), "%s", nf->old_path);
    pthread_mutex_lock(&g_closer.mu);
    if (!g_closer.started) {
        g_closer.quit = false;
        g_closer.started = pthread_create(&g_closer.th, NULL, closer_thread, NULL) == 0;
    }
    const bool queued = g_closer.started && g_closer.n < 64u;
    if (queued) {
        g_closer.q[(g_closer.head + g_closer.n++) % 64u] = j;
        pthread_cond_signal(&g_closer.cv);
    }
    pthread_mutex_unlock(&g_closer.mu);
    if (!queued) {
        close_now(&j);
    }
}

/* NtFlushBuffersFile: the same sync on the closer thread, of a duplicate of
 * the descriptor (the title may close the file meanwhile), in order with the
 * closes. On the guest's CPU it took up to 211 ms (Forza's saves in the
 * menus), nothing else running. */
static void flush_later(const NcFile *nf)
{
    NcFile d = *nf;
    d.fd = dup(nf->fd);
    if (d.fd < 0) {
        fsync(nf->fd);
        return;
    }
    d.old_path[0] = 0;          /* the overwritten contents go at the close */
    close_later(&d);
}

void nc_io_close(NcKernel *k, uint32_t object)
{
    uint32_t i = nc_vm_rd32(k->vm, object + FO_INDEX);
    if (i < MAX_FILES && g_files[i].used) {
        NcFile *nf = &g_files[i];
        if (nf->fd >= 0 && nf->wrote) {
            close_later(nf);
        } else {
            if (nf->fd >= 0) {
                close(nf->fd);
            }
            if (nf->old_path[0]) {
                unlink(nf->old_path);
            }
        }
        if (nf->del) {
            NcFile *o = file_other(nf, nf->host, false);
            if (o) {
                o->del = true;                      /* the last handle deletes it */
            } else {
                unlink(nf->host);
            }
        }
        free(nf->listing);
        nf->listing = NULL;
        nf->used = false;
    }
}

static void io_status(NcKernel *k, uint32_t iosb, uint32_t status, uint32_t info)
{
    if (iosb) {
        nc_vm_wr32(k->vm, iosb, status);
        nc_vm_wr32(k->vm, iosb + 4, info);
    }
}

/* An I/O call finished: a[] is (FileHandle, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, ...), as NtReadFile and its siblings take it. The event is
 * set; the APC routine runs as ApcRoutine(ApcContext, IoStatusBlock, 0) when
 * the caller next waits alertably. */
static void complete(NcKernel *k, const uint32_t *a)
{
    if (a[1]) {
        uint32_t ev = nc_ob_lookup(k, a[1], NULL);
        if (ev) {
            nc_ke_set_event(k, ev);
        }
    } else {
        /* no event of its own: the file object's, which a wait on the file
         * handle waits for (cleared as an I/O starts, set as it completes:
         * ours complete at once) */
        enum NcObType type;
        const uint32_t fo = nc_ob_lookup(k, a[0], &type);
        if (fo && type == NC_OB_FILE) {
            nc_ke_set_event(k, fo + NC_FO_EVENT);
        }
    }
    if (a[2] && k->current) {
        const NcApc apc = {.normal = a[2], .ctx = a[3], .a1 = a[4], .a2 = 0};
        nc_apc_queue(k, k->current, &apc);
    }
}

/* ------------------------------------------------------- the disc reader */

/* An overlapped disc read (NcFile.async) runs on the reader thread and the
 * call reports STATUS_PENDING at once, as the Xbox's DVD transfers do. Read
 * in place, one from the image on the SD card held the guest's CPU for
 * 10-70 ms, interrupts included: Dead or Alive 2 Ultimate streams during its
 * fights, and each read was a dropped frame. The kernel completes a read on
 * the guest's CPU (nc_io_poll): its IoStatusBlock, then its event (the file
 * object's without one of its own, cleared as the read starts, as NT does)
 * and its APC, queued to the thread that issued it. A full queue reads in
 * place. Overlapped reads of hard disk files go the same way (a duplicate
 * of the descriptor, closed when done): Forza streams its track from the Z:
 * cache during a race, 8-28 ms a read in place, a frame a vblank late now
 * and then. Long synchronous reads go there too, only their thread waiting
 * (NtReadFile, SYNC_LATER_MIN).
 *
 * The reader writes the buffer's physical pages through the RAM view, as a
 * device does: a page the GPU may still read (the HLE GPU's first-write
 * barrier) is waited for there, not on the guest's CPU (Forza's buffers
 * held it 10-19 ms that way, the read itself queued), and the write is
 * reported dirty when the read completes (nc_vm_dma). */
#define DISC_JOBS 64

typedef struct DiscJob {
    const NcDisc *disc;
    NcDiscFile file;
    int fd;                  /* a hard disk file's (a duplicate), else -1 */
    uint64_t off;
    struct DmaSeg {
        uint8_t *host;       /* the RAM view's */
        uint32_t pa, len;
    } *seg;                  /* the buffer's physical runs */
    unsigned nseg;
    uint64_t gpu;            /* the GPU value to wait for first (nc_vm_dma.fence), or 0 */
    uint32_t va, n;
    uint32_t iosb, event, apc, apc_ctx;
    NcThread *thread;
    uint32_t thread_id;      /* its id then: the slot may hold another thread by the end */
    bool sync;               /* a synchronous handle's: the thread waits for it (nc_ke_block) */
    bool ok;
    int err;
    char path[64]; /* for the log */
} DiscJob;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    DiscJob q[DISC_JOBS];    /* for the reader */
    unsigned head, n;
    DiscJob done[DISC_JOBS]; /* read, for the kernel to complete */
    unsigned ndone;
    unsigned busy;           /* queued, reading or done: at most DISC_JOBS */
    bool started, quit;      /* quit: nc_io_stop (what is queued is still read first) */
    pthread_t th;
} g_reader = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *reader_thread(void *arg)
{
    pthread_mutex_lock(&g_reader.mu);
    for (;;) {
        while (!g_reader.n && !g_reader.quit) {
            pthread_cond_wait(&g_reader.cv, &g_reader.mu);
        }
        if (!g_reader.n) {
            break;
        }
        DiscJob j = g_reader.q[g_reader.head];
        g_reader.head = (g_reader.head + 1) % DISC_JOBS;
        g_reader.n--;
        pthread_mutex_unlock(&g_reader.mu);
        if (j.gpu && nc_vm_dma.wait) {
            nc_vm_dma.wait(j.gpu);
        }
        errno = 0;
        j.ok = true;
        uint64_t o = j.off;
        for (unsigned i = 0; j.ok && i < j.nseg; i++) {
            const struct DmaSeg *g = &j.seg[i];
            j.ok = j.fd >= 0 ? pread(j.fd, g->host, g->len, (off_t)o) == (ssize_t)g->len
                             : nc_disc_read(j.disc, &j.file, o, g->host, g->len);
            o += g->len;
        }
        j.err = j.ok ? 0 : errno;
        pthread_mutex_lock(&g_reader.mu);
        g_reader.done[g_reader.ndone++] = j;
        pthread_mutex_unlock(&g_reader.mu);
        nc_irq_post_work();
        pthread_mutex_lock(&g_reader.mu);
    }
    pthread_mutex_unlock(&g_reader.mu);
    return NULL;
}

/* The buffer's pages stay put while the reader thread writes them: a
 * contiguous allocation relocates virtual pages (vm.c), and a page copied
 * while the read went on lost what the reader wrote after the copy. Pinned
 * and unpinned on the guest's CPU, where relocation runs. */
static void pin_buffer(NcKernel *k, uint32_t va, uint32_t n, int lock)
{
    for (uint32_t p = va & ~(NC_PAGE - 1); n && p < va + n; p += NC_PAGE) {
        const uint32_t pa = nc_vm_phys(k->vm, p);
        if (pa != UINT32_MAX) {
            nc_vm_pin(k->vm, pa & ~(NC_PAGE - 1), false, lock);
        }
    }
}

/* Queues a read of n bytes at off into the buffer a[5] (resident and
 * writable); false when it must happen in place. sync: the calling thread
 * waits for it, every other one runs on. */
static bool disc_read_later(NcKernel *k, const NcFile *nf, const uint32_t *a, uint64_t off, uint32_t n, bool sync)
{
    static int in_place = -1; /* NC_DISC_SYNC: every disc read in place, as before */
    if (in_place < 0) {
        in_place = getenv("NC_DISC_SYNC") != NULL;
    }
    if (in_place) {
        return false;
    }
    uint32_t event = 0;
    if (a[1]) {
        event = nc_ob_lookup(k, a[1], NULL);
        if (!event) {
            return false;
        }
    } else {
        enum NcObType type;
        const uint32_t fo = nc_ob_lookup(k, a[0], &type);
        if (fo && type == NC_OB_FILE) {
            event = fo + NC_FO_EVENT;
        }
    }
    DiscJob j = {.disc = k->disc, .file = nf->disc, .fd = -1, .off = off, .va = a[5], .n = n, .iosb = a[4],
                 .event = event, .apc = a[2], .apc_ctx = a[3], .thread = k->current,
                 .thread_id = k->current ? k->current->id : 0, .sync = sync};
    snprintf(j.path, sizeof(j.path), "%s", nf->path);
    j.seg = malloc(sizeof(*j.seg) * ((n + NC_PAGE - 1) / NC_PAGE + 1));
    if (!j.seg) {
        return false;
    }
    for (uint32_t va = a[5], left = n; left;) {
        const uint32_t room = NC_PAGE - (va & (NC_PAGE - 1)), chunk = left < room ? left : room;
        const uint32_t pa = nc_vm_phys(k->vm, va);
        if (pa == UINT32_MAX || (uint64_t)pa + chunk > k->vm->ram_size) {
            free(j.seg);
            return false;                   /* not all RAM: in place */
        }
        struct DmaSeg *last = j.nseg ? &j.seg[j.nseg - 1] : NULL;
        if (last && last->pa + last->len == pa) {
            last->len += chunk;
        } else {
            j.seg[j.nseg++] = (struct DmaSeg){k->vm->phys + pa, pa, chunk};
        }
        va += chunk;
        left -= chunk;
    }
    if (nf->kind == F_HOST && (j.fd = dup(nf->fd)) < 0) {
        free(j.seg);
        return false;
    }
    for (unsigned i = 0; nc_vm_dma.fence && i < j.nseg; i++) {
        const uint64_t v = nc_vm_dma.fence(j.seg[i].pa, j.seg[i].len);
        j.gpu = v > j.gpu ? v : j.gpu;
    }
    pthread_mutex_lock(&g_reader.mu);
    if (!g_reader.started) {
        g_reader.quit = false;
        g_reader.started = pthread_create(&g_reader.th, NULL, reader_thread, NULL) == 0;
    }
    const bool queued = g_reader.started && g_reader.busy < DISC_JOBS;
    if (queued) {
        pin_buffer(k, j.va, j.n, 1);
        g_reader.q[(g_reader.head + g_reader.n++) % DISC_JOBS] = j;
        g_reader.busy++;
        pthread_cond_signal(&g_reader.cv);
    }
    pthread_mutex_unlock(&g_reader.mu);
    if (!queued) {
        if (j.fd >= 0) {
            close(j.fd);
        }
        free(j.seg);
    }
    if (queued && event) {
        nc_ke_clear_event(k, event);
    }
    return queued;
}

static void disc_read_done(NcKernel *k, const DiscJob *j)
{
    pin_buffer(k, j->va, j->n, -1);
    const bool ok = j->ok;
    const int err = j->err;
    for (unsigned i = 0; i < j->nseg; i++) {
        if (nc_vm_dma.wrote) {
            nc_vm_dma.wrote(j->seg[i].pa, j->seg[i].len);    /* what a guest store would mark */
        }
    }
    free(j->seg);
    if (j->fd >= 0) {
        close(j->fd);
    }
    uint32_t st = NC_STATUS_SUCCESS, n = j->n;
    if (!ok) {
        nc_log("hakuX-native-io", "read %s +%llx (%u) into %08x failed: %s", j->path, (unsigned long long)j->off,
               j->n, j->va, strerror(err));
        st = 0xC000009Cu; /* STATUS_DEVICE_DATA_ERROR */
        n = 0;
    }
    io_status(k, j->iosb, st, n);
    if (j->event) {
        nc_ke_set_event(k, j->event);
    }
    if (j->sync && j->thread && j->thread->id == j->thread_id && j->thread->state == NC_T_WAITING) {
        nc_ke_wake(k, j->thread, st);   /* its NtReadFile returns now */
    }
    if (j->apc && j->thread && j->thread->id == j->thread_id && j->thread->state != NC_T_FREE &&
        j->thread->state != NC_T_TERMINATED) {
        const NcApc apc = {.normal = j->apc, .ctx = j->apc_ctx, .a1 = j->iosb, .a2 = 0};
        nc_apc_queue(k, j->thread, &apc);
    }
}

void nc_io_poll(NcKernel *k)
{
    if (!nc_irq_take_work()) {
        return;
    }
    static DiscJob done[DISC_JOBS]; /* the guest CPU's thread only */
    pthread_mutex_lock(&g_reader.mu);
    const unsigned n = g_reader.ndone;
    memcpy(done, g_reader.done, n * sizeof(done[0]));
    g_reader.ndone = 0;
    g_reader.busy -= n;
    pthread_mutex_unlock(&g_reader.mu);
    for (unsigned i = 0; i < n; i++) {
        disc_read_done(k, &done[i]);
    }
}

/* ---------------------------------------------------------------- open */

static void remove_tree(const char *path, bool self);

/* side files a dead process left were swept (or kept: a savestate's files
 * use theirs) */
static bool g_swept;

/* Moves an overwritten file's old contents to <hdd>/.overwrite (NcFile.old_path),
 * outside every partition. The first time in a run, side files a dead process
 * left there go. */
static bool set_aside(const char *host, char *old_path, size_t len)
{
    static unsigned serial;
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/.overwrite", g_hdd_root);
    if (!g_swept) {
        remove_tree(dir, false);
        g_swept = true;
    }
    mkdir(dir, 0775);
    snprintf(old_path, len, "%s/%u-%u", dir, (unsigned)getpid(), ++serial);
    if (rename(host, old_path) != 0) {
        nc_log("hakuX-native-io", "overwrite %s: keeping its old bytes failed: %s", host, strerror(errno));
        return false;
    }
    return true;
}

/* An overwritten file growing into [from, to): FATX shows what its clusters
 * held, the old contents (NcFile.old_path). */
static void regrow(NcFile *nf, uint64_t from, uint64_t to)
{
    if (!nf->old_path[0] || from >= nf->old_size) {
        return;
    }
    to = to < nf->old_size ? to : nf->old_size;
    const int old = open(nf->old_path, O_RDONLY | O_CLOEXEC);
    if (old < 0) {
        return;
    }
    static uint8_t buf[1u << 16];
    for (uint64_t o = from; o < to;) {
        const size_t want = to - o < sizeof(buf) ? (size_t)(to - o) : sizeof(buf);
        const ssize_t r = pread(old, buf, want, (off_t)o);
        if (r <= 0 || pwrite(nf->fd, buf, (size_t)r, (off_t)o) != r) {
            break;
        }
        o += (uint64_t)r;
    }
    close(old);
}

/* name relative to a RootDirectory (ObjectAttributes, a rename's): \?? for
 * XAPI's DOS paths ("D:\x"), else an open directory's path. False when the
 * handle isn't a file's. */
static bool resolve_name(NcKernel *k, uint32_t root, char *name, size_t cap)
{
    char joined[1024];
    if (root == OB_DOS_DEVICES) {
        snprintf(joined, sizeof(joined), "\\??\\%s", name);
    } else if (root) {
        const NcFile *rf = file_of(k, root);
        if (!rf) {
            return false;
        }
        snprintf(joined, sizeof(joined), "%s\\%s", rf->path, name);
    } else {
        return true;
    }
    snprintf(name, cap, "%s", joined);
    return true;
}

/* Opens (and creates) a file for NtCreateFile/NtOpenFile. Returns the status;
 * *info receives the IoStatusBlock information. */
/* An open whose FILE_DIRECTORY_FILE / FILE_NON_DIRECTORY_FILE doesn't fit
 * what the path is: logged (the first 20), as the title may retry it. */
static uint32_t dir_mismatch(const char *path, uint32_t options, uint32_t st)
{
    static unsigned logged;
    if (logged++ < 20) {
        nc_log("hakuX-native-io", "open %s (options %x): %s", path, options,
               st == STATUS_FILE_IS_A_DIRECTORY ? "a directory" : "not a directory");
    }
    return st;
}

static uint32_t do_open(NcKernel *k, uint32_t access, uint32_t attrs, uint32_t disposition, uint32_t options,
                        uint32_t *handle, uint32_t *info)
{
    char name[512], path[512];
    ansi(k, nc_vm_rd32(k->vm, attrs + 4), name, sizeof(name));
    const uint32_t root = nc_vm_rd32(k->vm, attrs);
    if (!resolve_name(k, root, name, sizeof(name))) {
        nc_log("hakuX-native-io", "open %s relative to handle %08x: not a directory handle", name, root);
        return NC_STATUS_INVALID_HANDLE;
    }
    if (!canon(name, path, sizeof(path))) {
        nc_log("hakuX-native-io", "open %s: no such drive", name);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    const char *rel;
    const int dev = device_of(path, &rel);
    *info = 5; /* FILE_DOES_NOT_EXIST */
    NcFile *nf = NULL;

    if (dev == -2) {
        /* No such object. As the object manager's lookup has it, a name
         * missing from an object directory (\, \??, \Device) is a missing
         * name, one under a missing component a missing path. Titles tell
         * them apart: GTA III probes a bare file name ("\??\x.dds" from
         * CreateFile("x.dds")) and shows its bad-disc screen on a bad path. */
        const char *last = strrchr(path, '\\');
        const size_t at = last ? (size_t)(last - path) : 0;
        const bool in_dir = at == 0 || (at == 3 && !strncmp(path, "\\??", 3)) ||
                            (at == 7 && !strncasecmp(path, "\\Device", 7));
        nc_log("hakuX-native-io", "open %s: unknown device (a missing %s)", path, in_dir ? "name" : "path");
        return in_dir ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (dev == -1) {
        if (disposition != FILE_OPEN && disposition != FILE_OPEN_IF) {
            return STATUS_MEDIA_WRITE_PROTECTED;
        }
        NcDiscFile df;
        if (!nc_disc_find(k->disc, rel, &df)) {
            nc_log("hakuX-native-io", "open %s: not on the disc", path);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        const bool dir = df.attr & 0x10;
        if ((options & FILE_DIRECTORY_FILE) && !dir) {
            return dir_mismatch(path, options, STATUS_NOT_A_DIRECTORY);
        }
        if ((options & FILE_NON_DIRECTORY_FILE) && dir) {
            return dir_mismatch(path, options, STATUS_FILE_IS_A_DIRECTORY);
        }
        nf = file_new();
        if (!nf) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = *rel ? F_DISC : F_VOLUME;
        nf->partition = -1;
        nf->disc = df;
        nf->is_dir = dir;
        *info = 1; /* FILE_OPENED */
    } else if (dev == 0) {
        /* Partition0 is the whole disk, opened raw for its config sectors:
         * they read as zeros (no refurb or cache records) and writes drop. */
        if (*rel) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        nf = file_new();
        if (!nf) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = F_VOLUME;
        nf->partition = 0;
        *info = 1;
    } else {
        char host[512];
        int exists = host_path(dev, rel, host, sizeof(host));
        if (exists < 0) {
            if (g_trace_io) {
                nc_log("hakuX-native-io", "open %s: path not found", path);
            }
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        struct stat st;
        bool is_dir = exists && stat(host, &st) == 0 && S_ISDIR(st.st_mode);
        if (!*rel) {
            is_dir = true; /* the partition root */
        }
        if (exists && !is_dir && file_other(NULL, host, true)) {
            nc_log("hakuX-native-io", "open %s: delete pending", path);
            return STATUS_DELETE_PENDING;
        }
        if (exists && disposition == FILE_CREATE) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
            if (g_trace_io) {
                nc_log("hakuX-native-io", "open %s: not found", path);
            }
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (exists && is_dir && (options & FILE_NON_DIRECTORY_FILE)) {
            return dir_mismatch(path, options, STATUS_FILE_IS_A_DIRECTORY);
        }
        if (exists && !is_dir && (options & FILE_DIRECTORY_FILE)) {
            return dir_mismatch(path, options, STATUS_NOT_A_DIRECTORY);
        }
        int fd = -1;
        char old_path[320] = "";
        uint64_t old_size = 0;
        if (!exists && (options & FILE_DIRECTORY_FILE)) {
            if (mkdir(host, 0775) != 0) {
                return STATUS_ACCESS_DENIED;
            }
            is_dir = true;
            *info = 2; /* FILE_CREATED */
        } else if (!is_dir) {
            int flags = O_RDWR | O_CLOEXEC;
            if (!exists) {
                flags |= O_CREAT;
                *info = 2;
            } else if (disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF ||
                       disposition == FILE_SUPERSEDE) {
                /* Empty now; the old bytes wait in a side file (NcFile.old_path). */
                old_size = (uint64_t)st.st_size;
                if (!set_aside(host, old_path, sizeof(old_path))) {
                    old_path[0] = '\0';
                }
                flags |= O_CREAT | O_TRUNC;
                *info = disposition == FILE_SUPERSEDE ? 0 : 3;
            } else {
                *info = 1;
            }
            fd = open(host, flags, 0664);
            if (fd < 0) {
                nc_log("hakuX-native-io", "open %s (%s): %s", path, host, strerror(errno));
                if (old_path[0]) {
                    rename(old_path, host); /* the file as it was */
                }
                return STATUS_ACCESS_DENIED;
            }
        } else {
            *info = 1;
        }
        nf = file_new();
        if (!nf) {
            if (fd >= 0) {
                close(fd);
            }
            if (old_path[0]) {
                rename(old_path, host);
            }
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        nf->kind = F_HOST;
        nf->partition = dev;
        nf->fd = fd;
        nf->is_dir = is_dir;
        snprintf(nf->old_path, sizeof(nf->old_path), "%s", old_path);
        nf->old_size = old_size;
        snprintf(nf->host, sizeof(nf->host), "%s", host);
    }
    snprintf(nf->path, sizeof(nf->path), "%s", path);
    nf->async = !(options & FILE_SYNCHRONOUS_IO);
    nf->no_buffering = (options & FILE_NO_INTERMEDIATE_BUFFERING) != 0;
    nf->can_write = (access & ACCESS_WRITES) != 0;
    nc_kernel_trace_trigger(path);
    *handle = file_handle(k, nf);
    nc_log("hakuX-native-io", "open %s -> %s%s", path, nf->is_dir ? "dir " : "",
           *info == 2 ? "(created)" : *info == 3 ? "(overwritten)" : *info == 0 ? "(superseded)" : "");
    return *handle ? NC_STATUS_SUCCESS : NC_STATUS_INSUFFICIENT_RESOURCES;
}

/* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
 *   FileAttributes, ShareAccess, CreateDisposition, CreateOptions) */
static uint64_t NtCreateFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, a[1], a[2], a[7], a[8], &h, &info);
    io_status(k, a[3], st, info);
    if (st == NC_STATUS_SUCCESS) {
        nc_vm_wr32(k->vm, a[0], h);
    }
    return st;
}

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
static uint64_t NtOpenFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, a[1], a[2], FILE_OPEN, a[5], &h, &info);
    io_status(k, a[3], st, info);
    if (st == NC_STATUS_SUCCESS) {
        nc_vm_wr32(k->vm, a[0], h);
    }
    return st;
}

/* --------------------------------------------------------- read, write */

static uint64_t file_size(const NcFile *nf)
{
    if (nf->kind == F_DISC) {
        return nf->disc.size;
    }
    struct stat st;
    return nf->fd >= 0 && fstat(nf->fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

/* NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
/* Raw sectors that must survive a reboot: the disk's config area before the
 * first partition (XAPI's cache drive records, refurb info) and the head of
 * each cache partition (its FATX superblock), kept in <hdd>/raw-partitionN.bin.
 * Past RAW_KEEP bytes they read as zeros and writes drop. */
#define RAW_KEEP 0x80000u

static void raw_io(int part, uint64_t off, uint8_t *buf, uint32_t len, bool write)
{
    const uint32_t n = off < RAW_KEEP ? (uint32_t)(RAW_KEEP - off < len ? RAW_KEEP - off : len) : 0;
    if (!write) {
        memset(buf, 0, len);
    }
    if (!n) {
        return;
    }
    char p[320];
    snprintf(p, sizeof(p), "%s/raw-partition%d.bin", g_hdd_root, part);
    const int fd = open(p, write ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0664);
    if (fd < 0) {
        return;
    }
    if (write ? pwrite(fd, buf, n, (off_t)off) != (ssize_t)n : pread(fd, buf, n, (off_t)off) < 0) {
        nc_log("hakuX-native-io", "raw partition%d %s +%llx: %s", part, write ? "write" : "read",
               (unsigned long long)off, strerror(errno));
    }
    close(fd);
}

/* The offset an NtReadFile/NtWriteFile works at: ByteOffset, or the file position. */
static uint64_t io_offset(NcKernel *k, const NcFile *nf, uint32_t byte_offset)
{
    if (byte_offset) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, byte_offset) | ((uint64_t)nc_vm_rd32(k->vm, byte_offset + 4) << 32);
        if (o != 0xFFFFFFFFFFFFFFFEull) { /* FILE_USE_FILE_POINTER_POSITION */
            return o;
        }
    }
    return nf->pos;
}

static int partition_root(const NcFile *nf);

/* Ends a read or write: its IoStatusBlock, event and APC, and what it returns.
 * An overlapped request that completes reports STATUS_PENDING (NcFile.async);
 * one that fails up front (end of file) returns the error and, as NT does for
 * a failure before pending, leaves the IoStatusBlock, event and APC alone:
 * XAPI set the OVERLAPPED to STATUS_PENDING, which GetOverlappedResult reads
 * as success. Forza's read-ahead past the end of a file relies on that.
 * The DVD drive pends every transfer, an overlapped one with neither event
 * nor APC too (its completion sets the file object's event): Otogi's stream
 * reader (XDK 5455) counts the bytes only after ERROR_IO_PENDING, through
 * GetOverlappedResult, and took a disc read that succeeded at once as 0 bytes
 * read, a dirty disc. pend: a success may pend (not a read through the cache,
 * disc_cached). */
static uint32_t io_finish(NcKernel *k, const NcFile *nf, const uint32_t *a, uint32_t st, uint32_t n, bool pend)
{
    if (nf->async && st >= 0xC0000000u) {
        return st;
    }
    io_status(k, a[4], st, n);
    complete(k, a);
    return pend && nf->async && (a[1] || a[2] || nf->kind == F_DISC) ? STATUS_PENDING : st;
}

/* A disc read of part of a sector goes through the file system's cache, which
 * reads in the caller's thread: it completes in the call, overlapped or not
 * (only whole sectors go to the drive and pend). Wreckless reads Font18.xpr's
 * 12-byte header overlapped, then loads the OVERLAPPED's status into a
 * register once and loops while it is STATUS_PENDING: pended, it hung at
 * boot unless the read happened to land before that load. */
#define DISC_SECTOR 2048u

static bool disc_cached(const NcFile *nf, uint64_t off, uint32_t len)
{
    return nf->kind == F_DISC && ((off | len) & (DISC_SECTOR - 1));
}

/* A synchronous read this long goes to the reader thread too, the calling
 * thread waiting for it (XAPI's ReadFile on an ordinary handle): in place,
 * every guest thread, the vblank DPC and the interrupts stopped for the
 * whole read (8-70 ms from the SD card, as the overlapped ones were).
 * Shorter ones cost less than the trip to the reader. */
#define SYNC_LATER_MIN 0x8000u

/* The running thread may wait in its call: an ordinary thread at
 * PASSIVE_LEVEL, not a DPC or an ISR. */
static bool can_block(const NcKernel *k)
{
    static int in_place = -1; /* NC_SYNC_IN_PLACE: synchronous reads in place, as before (A/B) */
    if (in_place < 0) {
        in_place = getenv("NC_SYNC_IN_PLACE") != NULL;
    }
    const NcThread *t = k->current;
    return !in_place && t && t->state == NC_T_RUNNING && t != k->dpc_thread && t != k->isr_thread && !k->in_isr &&
           !k->redirected && *(const uint8_t *)nc_vm_host(k->vm, k->kpcr_va + 0x24) == 0;
}

static uint64_t NtReadFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const int raw = nf->kind == F_VOLUME && nf->partition == 0 ? 0
                  : nf->kind == F_HOST && nf->is_dir ? partition_root(nf) : -1;
    if (raw == 0 || raw >= 3) {
        /* The raw disk's config sectors, or a cache partition read raw. */
        if (!nc_kernel_resident(k, a[5], a[6], true)) {
            return 0;
        }
        const uint64_t off = io_offset(k, nf, a[7]);
        nc_vm_will_write(a[5], a[6]);
        raw_io(raw, off, nc_vm_host(k->vm, a[5]), a[6], false);
        nf->pos = off + a[6];
        if (g_trace_io) {
            nc_log("hakuX-native-io", "read %s raw +%llx %u", nf->path, (unsigned long long)off, a[6]);
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], true);
    }
    if (nf->is_dir || nf->kind == F_VOLUME) {
        io_status(k, a[4], STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    uint64_t off = nf->pos;
    if (a[7]) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, a[7]) | ((uint64_t)nc_vm_rd32(k->vm, a[7] + 4) << 32);
        if (o != 0xFFFFFFFFFFFFFFFEull) { /* FILE_USE_FILE_POINTER_POSITION */
            off = o;
        }
    }
    const uint64_t size = file_size(nf);
    const bool async = nf->async && !disc_cached(nf, off, a[6]);
    uint32_t st = NC_STATUS_SUCCESS, n = 0;
    if (!a[6]) {
        /* Zero bytes succeed, past the end too, as the file systems check
         * the length first: Halo's map streaming queues reads of what is
         * left, 0 at the end, and waits for each one's completion. */
    } else if (off >= size) {
        st = STATUS_END_OF_FILE;
    } else {
        n = (uint32_t)(size - off < a[6] ? size - off : a[6]);
        if (!nc_kernel_resident(k, a[5], n, true)) {
            return 0;
        }
        const bool later = async || (n >= SYNC_LATER_MIN && can_block(k));
        if ((nf->kind == F_DISC || (nf->kind == F_HOST && nf->fd >= 0)) && later &&
            disc_read_later(k, nf, a, off, n, !async)) {
            nf->pos = off + n;
            if (g_trace_io) {
                nc_log("hakuX-native-io", "read %s +%llx %u into %08x %s%s%s", nf->path, (unsigned long long)off,
                       n, a[5], async ? "pending" : "waited for", a[1] ? " event" : "", a[2] ? " apc" : "");
            }
            if (!async) {
                nc_ke_block(k);         /* the read's status replaces this when it completes */
            }
            return STATUS_PENDING;
        }
        nc_vm_will_write(a[5], n); /* pread into a protected page fails rather than faults */
        bool ok = nf->kind == F_DISC ? nc_disc_read(k->disc, &nf->disc, off, nc_vm_host(k->vm, a[5]), n)
                                     : pread(nf->fd, nc_vm_host(k->vm, a[5]), n, (off_t)off) == (ssize_t)n;
        if (!ok) {
            nc_log("hakuX-native-io", "read %s +%llx (%u) into %08x failed: %s", nf->path, (unsigned long long)off, n,
                   a[5], strerror(errno));
            st = 0xC000009Cu; /* STATUS_DEVICE_DATA_ERROR */
            n = 0;
        }
    }
    nf->pos = off + n;
    if (g_trace_io) {
        uint8_t b[8] = {0};
        if (n >= 8) {
            memcpy(b, nc_vm_host(k->vm, a[5]), 8);
        }
        nc_log("hakuX-native-io", "read %s +%llx %u into %08x -> %08x n=%u%s%s [%02x%02x%02x%02x %02x%02x%02x%02x]",
               nf->path, (unsigned long long)off, a[6], a[5], st, n, a[1] ? " event" : "", a[2] ? " apc" : "",
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    return io_finish(k, nf, a, st, n, async);
}

/* The partition number if nf is a hard disk partition's root, else -1. */
static int partition_root(const NcFile *nf)
{
    const char *rel;
    const int dev = device_of(nf->path, &rel);
    return dev >= 0 && !*rel ? dev : -1;
}

/* Deletes everything under path (and path itself if self). */
static void remove_tree(const char *path, bool self)
{
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_tree(p, true);
        } else {
            unlink(p);
        }
    }
    if (d) {
        closedir(d);
    }
    if (self) {
        rmdir(path);
    }
}

/* A host file's data or size changed through nf (what: for the log): true
 * if its handle was opened to write. The Xbox kernel refuses the rest
 * (STATUS_ACCESS_DENIED), and titles rely on it: Splinter Cell reads its
 * profile (Profile.inf, 2040 bytes and a 20-byte signature) through a
 * GENERIC_READ handle and then calls SetEndOfFile at the read's end; that
 * fails on the console, and here cut the signature off, so every profile
 * failed to load ("corrupted") the next time. */
static bool handle_can_write(const NcFile *nf, const char *what)
{
    if (nf->can_write) {
        return true;
    }
    static unsigned logged;
    if (logged++ < 16) {
        nc_log("hakuX-native-io", "%s %s through a handle opened without write access: refused "
               "(first 16 logged)", what, nf->path);
    }
    return false;
}

/* NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset) */
static uint64_t NtWriteFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    if (!nc_kernel_resident(k, a[5], a[6], false)) {
        return 0;
    }
    if (nf->kind == F_VOLUME) {
        if (nf->partition == 0) {
            const uint64_t off = io_offset(k, nf, a[7]);
            raw_io(0, off, nc_vm_host(k->vm, a[5]), a[6], true);
            nf->pos = off + a[6];
            if (g_trace_io) {
                nc_log("hakuX-native-io", "write %s raw +%llx %u", nf->path, (unsigned long long)off, a[6]);
            }
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], true);
    }
    if (nf->kind == F_HOST && nf->is_dir && partition_root(nf) >= 3) {
        /* A cache drive formatted raw (XMountUtilityDrive): the superblock
         * write empties the folder; the head of the partition is kept. */
        const uint64_t o = io_offset(k, nf, a[7]);
        nf->pos = o + a[6];
        raw_io(partition_root(nf), o, nc_vm_host(k->vm, a[5]), a[6], true);
        /* The FATX superblock: "FATX", volume id, sectors per cluster. */
        const uint8_t *sb = nc_vm_host(k->vm, a[5]);
        if (o == 0 && a[6] >= 12 && !memcmp(sb, "FATX", 4)) {
            const int part = partition_root(nf);
            g_cluster_sectors[part] = nc_rd32(sb + 8);
            nc_log("hakuX-native-io", "%s formatted (%u-sector clusters): emptied %s", nf->path,
                   g_cluster_sectors[part], nf->host);
            remove_tree(nf->host, false);
        }
        return io_finish(k, nf, a, NC_STATUS_SUCCESS, a[6], true);
    }
    if (nf->kind != F_HOST || nf->is_dir || !handle_can_write(nf, "write to")) {
        io_status(k, a[4], STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }
    uint64_t off = nf->pos;
    if (a[7]) {
        const uint64_t o = (uint64_t)nc_vm_rd32(k->vm, a[7]) | ((uint64_t)nc_vm_rd32(k->vm, a[7] + 4) << 32);
        if (o == 0xFFFFFFFFFFFFFFFFull) { /* FILE_WRITE_TO_END_OF_FILE */
            off = file_size(nf);
        } else if (o != 0xFFFFFFFFFFFFFFFEull) {
            off = o;
        }
    }
    if (nf->old_path[0]) {
        const uint64_t size = file_size(nf);
        if (off > size) {
            regrow(nf, size, off); /* the gap a write past the end leaves */
        }
    }
    ssize_t n = pwrite(nf->fd, nc_vm_host(k->vm, a[5]), a[6], (off_t)off);
    uint32_t st = n == (ssize_t)a[6] ? NC_STATUS_SUCCESS : 0xC000007Fu; /* STATUS_DISK_FULL */
    nf->wrote |= n > 0;
    if (g_trace_io) {
        uint8_t b[8] = {0};
        if (n >= 8) {
            memcpy(b, nc_vm_host(k->vm, a[5]), 8);
        }
        nc_log("hakuX-native-io", "write %s +%llx %u from %08x -> %08x [%02x%02x%02x%02x %02x%02x%02x%02x]", nf->path,
               (unsigned long long)off, a[6], a[5], st, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    nf->pos = off + (n > 0 ? (uint64_t)n : 0);
    return io_finish(k, nf, a, st, n > 0 ? (uint32_t)n : 0, nf->no_buffering);   /* cached: in the call */
}

/* ---------------------------------------------------------- information */

static uint64_t host_time(time_t t)
{
    return (uint64_t)(t + 11644473600ll) * 10000000ull;
}

/* Times, size and attributes common to the information classes. */
static void file_facts(const NcFile *nf, uint64_t times[4], uint64_t *size, uint32_t *attrs)
{
    if (nf->kind == F_HOST) {
        struct stat st;
        if (stat(nf->host, &st) == 0) {
            times[0] = host_time(st.st_ctime);
            times[1] = host_time(st.st_atime);
            times[2] = times[3] = host_time(st.st_mtime);
            *size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
            *attrs = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            return;
        }
    }
    times[0] = times[1] = times[2] = times[3] = DISC_TIME;
    *size = nf->is_dir ? 0 : file_size(nf);
    *attrs = (nf->is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0) | (nf->kind == F_HOST ? 0 : FILE_ATTRIBUTE_READONLY);
    if (!*attrs) {
        *attrs = FILE_ATTRIBUTE_NORMAL;
    }
}

static void put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

/* FILE_NETWORK_OPEN_INFORMATION: four times, AllocationSize, EndOfFile, FileAttributes (0x38). */
static void network_open_info(const NcFile *nf, uint8_t *o)
{
    uint64_t t[4], size;
    uint32_t attrs;
    file_facts(nf, t, &size, &attrs);
    memset(o, 0, 0x38);
    for (int i = 0; i < 4; i++) {
        put64(o + 8 * i, t[i]);
    }
    put64(o + 0x20, nc_align_up((uint32_t)size, 4096));
    put64(o + 0x28, size);
    nc_wr32(o + 0x30, attrs);
}

/* NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtQueryInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint8_t o[0x38];                    /* the largest class's; the caller's buffer once it fits */
    uint64_t t[4], size;
    uint32_t attrs, len;
    file_facts(nf, t, &size, &attrs);
    switch (a[4]) {
    case 4: /* FileBasicInformation: times, FileAttributes */
        len = 0x28;
        memset(o, 0, len);
        for (int i = 0; i < 4; i++) {
            put64(o + 8 * i, t[i]);
        }
        nc_wr32(o + 0x20, attrs);
        break;
    case 5: /* FileStandardInformation: AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory */
        len = 0x18;
        memset(o, 0, len);
        put64(o, nc_align_up((uint32_t)size, 4096));
        put64(o + 8, size);
        nc_wr32(o + 0x10, 1);
        o[0x15] = nf->is_dir;
        break;
    case 14: /* FilePositionInformation */
        len = 8;
        put64(o, nf->pos);
        break;
    case 6: { /* FileInternalInformation: IndexNumber, one per file on its volume */
        /* GetFileInformationByHandle asks for it and gives up without it,
         * leaving the rest unset: DOA3 sized its cache copy of loadfile.afs
         * from that garbage and waited for the last chunk forever. */
        uint64_t index = 0;
        struct stat st;
        if (nf->kind == F_DISC) {
            index = nf->disc.sector;
        } else if (nf->kind == F_HOST && stat(nf->host, &st) == 0) {
            index = (uint64_t)st.st_ino;
        }
        len = 8;
        put64(o, index);
        break;
    }
    case 34: /* FileNetworkOpenInformation */
        len = 0x38;
        network_open_info(nf, o);
        break;
    case 17: /* FileAlignmentInformation */
        len = 4;
        nc_wr32(o, 0);
        break;
    default:
        nc_log("hakuX-native-io", "query information class %u on %s not implemented", a[4], nf->path);
        io_status(k, a[1], 0xC0000003u, 0); /* STATUS_INVALID_INFO_CLASS */
        return 0xC0000003u;
    }
    if (a[3] < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(nc_vm_host(k->vm, a[2]), o, len);
    io_status(k, a[1], NC_STATUS_SUCCESS, len);
    return NC_STATUS_SUCCESS;
}

/* NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t NtSetInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const uint8_t *in = nc_vm_host(k->vm, a[2]);
    uint64_t v;
    uint32_t st = NC_STATUS_SUCCESS;
    switch (a[4]) {
    case 14: /* FilePositionInformation */
        memcpy(&v, in, 8);
        nf->pos = v;
        break;
    case 19: /* FileAllocationInformation (the size is FATX's to keep: nothing to do) */
        if (nf->kind == F_HOST && !nf->is_dir && !handle_can_write(nf, "allocation size of")) {
            st = STATUS_ACCESS_DENIED;
        }
        break;
    case 4: /* FileBasicInformation: Creation, LastAccess, LastWrite, Change times; 0 keeps one */
        if (nf->kind == F_HOST && !nf->is_dir) {
            /* Forza's installer stamps each copy with its source's time and
             * recopies a Z: file whose LastWriteTime is over 4 s off it. */
            struct timespec ts[2];
            for (int i = 0; i < 2; i++) {
                memcpy(&v, in + 8 + 8 * i, 8);
                if (v && v != ~0ull) {
                    const int64_t unix100 = (int64_t)v - 116444736000000000ll;
                    ts[i].tv_sec = (time_t)(unix100 / 10000000);
                    ts[i].tv_nsec = (long)(unix100 % 10000000) * 100;
                    if (ts[i].tv_nsec < 0) {
                        ts[i].tv_sec--;
                        ts[i].tv_nsec += 1000000000;
                    }
                } else {
                    ts[i].tv_sec = 0;
                    ts[i].tv_nsec = UTIME_OMIT;
                }
            }
            if ((nf->fd >= 0 ? futimens(nf->fd, ts) : utimensat(AT_FDCWD, nf->host, ts, 0)) != 0) {
                nc_log("hakuX-native-io", "set times on %s: %s", nf->path, strerror(errno));
            }
        }
        break;
    case 20: { /* FileEndOfFileInformation */
        memcpy(&v, in, 8);
        const uint64_t was = file_size(nf);
        if (nf->kind != F_HOST || nf->fd < 0 || !handle_can_write(nf, "end of file of") ||
            ftruncate(nf->fd, (off_t)v) != 0) {
            st = STATUS_ACCESS_DENIED;
        } else {
            nf->wrote = true;
            if (v > was) {
                regrow(nf, was, v);
            }
        }
        break;
    }
    case 13: /* FileDispositionInformation: DeleteFile */
        if (nf->kind == F_HOST && !nf->is_dir) {
            nf->del = in[0] != 0;                   /* the file goes at its last close (NcFile.del) */
        }
        if (in[0]) {
            if (nf->kind != F_HOST) {
                st = STATUS_ACCESS_DENIED;
            } else if (nf->is_dir && rmdir(nf->host) != 0) {
                st = errno == ENOTEMPTY ? STATUS_DIRECTORY_NOT_EMPTY : STATUS_ACCESS_DENIED;
            }
            /* deletes are few, and a save that loses a file is hard to trace back */
            char calls[96];
            nc_kernel_frame_chain(k, f, calls, sizeof(calls));
            nc_log("hakuX-native-io", "delete %s -> %x (from %08x |%s)", nf->path, st,
                   nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)), calls);
        }
        break;
    case 10: { /* FileRenameInformation: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING) */
        /* The new name is relative to RootDirectory as an open's is: XAPI's
         * MoveFile passes "Z:\x" under \?? (Riddick's file cache renames each
         * finished X.tmp to X; read as a bare name every rename failed and
         * the cache never completed). */
        char name[1024], path[512], host[512];
        ansi(k, a[2] + 8, name, sizeof(name));
        const char *rel;
        int dev, exists = -1;
        if (nf->kind != F_HOST || !resolve_name(k, nc_rd32(in + 4), name, sizeof(name)) ||
            !canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 0 ||
            (exists = host_path(dev, rel, host, sizeof(host))) < 0) {
            st = exists < 0 && nf->kind == F_HOST ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_ACCESS_DENIED;
        } else if (exists && !in[0]) {
            st = STATUS_OBJECT_NAME_COLLISION;
        } else if (rename(nf->host, host) != 0) {
            st = STATUS_ACCESS_DENIED;
        } else {
            snprintf(nf->host, sizeof(nf->host), "%s", host);
            snprintf(nf->path, sizeof(nf->path), "%s", path);
        }
        if (st != NC_STATUS_SUCCESS) {
            nc_log("hakuX-native-io", "rename %s to %s (replace %u) -> %x", nf->path, name, in[0], st);
        }
        break;
    }
    default:
        nc_log("hakuX-native-io", "set information class %u on %s not implemented", a[4], nf->path);
        st = 0xC0000003u;
        break;
    }
    io_status(k, a[1], st, 0);
    return st;
}

/* A partition's cluster size in sectors: from its last format (this boot's,
 * or the superblock kept from an earlier one), else FATX's 16 KiB. */
static uint32_t cluster_sectors(int part)
{
    if (!g_cluster_sectors[part] && part >= 3) {
        uint8_t sb[12];
        raw_io(part, 0, sb, sizeof(sb), false);
        if (!memcmp(sb, "FATX", 4)) {
            g_cluster_sectors[part] = nc_rd32(sb + 8);
        }
    }
    return g_cluster_sectors[part] ? g_cluster_sectors[part] : 32;
}

/* Bytes the files under a partition's directory take, in whole clusters. */
static uint64_t g_usage_bytes, g_usage_cluster;

static int usage_add(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    if (type == FTW_F) {
        g_usage_bytes += ((uint64_t)st->st_size + g_usage_cluster - 1) / g_usage_cluster * g_usage_cluster;
    }
    return 0;
}

static uint64_t partition_used(int part, uint32_t cluster_bytes)
{
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/Partition%d", g_hdd_root, part);
    g_usage_bytes = 0;
    g_usage_cluster = cluster_bytes ? cluster_bytes : 16384;
    nftw(dir, usage_add, 16, FTW_PHYS);
    return g_usage_bytes;
}

/* A partition's use, from a scan of its folder at most every 2 s. */
static uint64_t g_part_used[6], g_part_scanned_ns[6];

static uint32_t cluster_sectors(int part);

static uint64_t part_used(int part)
{
    const uint64_t now = nc_ke_now_ns();
    if (!g_part_scanned_ns[part] || now - g_part_scanned_ns[part] > 2000000000ull) {
        g_part_used[part] = partition_used(part, cluster_sectors(part) * 512u);
        g_part_scanned_ns[part] = now;
    }
    return g_part_used[part];
}

/* Volume information of the volume a file is on; *len receives the bytes written. */
static uint32_t volume_info(NcKernel *k, const NcFile *nf, uint32_t cls, uint32_t buf, uint32_t *len_out)
{
    const bool disc = nf->partition < 0 && nf->kind != F_HOST;
    uint8_t *o = nc_vm_host(k->vm, buf);
    uint32_t len;
    switch (cls) {
    case 1: /* FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label */
        len = 0x12;
        memset(o, 0, len);
        put64(o, DISC_TIME);
        nc_wr32(o + 8, disc ? 0x12345678u : 0x5678ABCDu);
        break;
    case 3: /* FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits,
               SectorsPerAllocationUnit, BytesPerSector */
    case 7: { /* FileFsFullSizeInformation: Total, CallerAvailable, ActualAvailable, Sectors, Bytes */
        uint64_t total, avail;
        uint32_t spu, bps;
        if (disc) {
            total = (uint64_t)7 * 1024 * 1024 * 1024 / 2048; /* a full DVD-9 */
            avail = 0;
            spu = 1;
            bps = 2048;
        } else {
            struct statvfs vfs;
            uint64_t freeb = statvfs(g_hdd_root, &vfs) == 0 ? (uint64_t)vfs.f_bavail * vfs.f_frsize : 0;
            const int part = nf->partition >= 1 && nf->partition <= 5 ? nf->partition : 1;
            spu = cluster_sectors(part);
            bps = 512;
            /* The partition's own size, less what its files hold (a title
             * filling a cache drive sees it fill). The folders themselves have
             * no limit: Riddick's file cache copies what it opens and never
             * asks, so it outgrows a 750 MB drive (on hardware it apparently
             * doesn't; how is open). */
            const uint64_t cluster = (uint64_t)spu * 512u, size = k_partitions[part][1];
            const uint64_t used = part_used(part);
            uint64_t free_part = used < size ? size - used : 0;
            if (freeb < free_part) {
                free_part = freeb;
            }
            total = size / cluster;
            avail = free_part / cluster;
        }
        if (cls == 3) {
            len = 0x18;
            put64(o, total);
            put64(o + 8, avail);
            nc_wr32(o + 0x10, spu);
            nc_wr32(o + 0x14, bps);
        } else {
            len = 0x20;
            put64(o, total);
            put64(o + 8, avail);
            put64(o + 0x10, avail);
            nc_wr32(o + 0x18, spu);
            nc_wr32(o + 0x1C, bps);
        }
        break;
    }
    case 4: /* FileFsDeviceInformation: DeviceType, Characteristics */
        len = 8;
        nc_wr32(o, disc ? 0x02u : 0x07u); /* FILE_DEVICE_CD_ROM / FILE_DEVICE_DISK */
        nc_wr32(o + 4, disc ? 0x01u : 0u); /* FILE_REMOVABLE_MEDIA */
        break;
    case 5: { /* FileFsAttributeInformation: Attributes, MaxComponentNameLength, NameLength, Name */
        /* The Xbox's: GDFX the game disc's (XDVDFS), FATX the hard disk's.
         * XGetFilePhysicalSortKey knows the file control block by this name
         * (FILE_OBJECT_SIZE) and fails for another: with CDFS every disc
         * file's key was -1 (invalid parameter), and Fatal Frame II, which
         * sorts its load queue by them to read in disc order, read in
         * whatever order its sort left. */
        const char *fs = disc ? "GDFX" : "FATX";
        len = 12 + 4;
        nc_wr32(o, 0);
        nc_wr32(o + 4, 42);
        nc_wr32(o + 8, 4);
        memcpy(o + 12, fs, 4);
        break;
    }
    default:
        nc_log("hakuX-native-io", "volume information class %u not implemented", cls);
        *len_out = 0;
        return 0xC0000003u;
    }
    *len_out = len;
    return NC_STATUS_SUCCESS;
}

/* NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
static uint64_t NtQueryVolumeInformationFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t len;
    const uint32_t st = volume_info(k, nf, a[4], a[2], &len);
    io_status(k, a[1], st, len);
    return st;
}

/* IoQueryVolumeInformation(FileObject, FsInformationClass, Length, FsInformation, ReturnedLength):
 * the same, on a file object from ObReferenceObjectByHandle. */
static uint64_t IoQueryVolumeInformation(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (!a[0] || nc_rd16(nc_vm_host(k->vm, a[0])) != 5) { /* IO_TYPE_FILE */
        return 0xC0000008u; /* STATUS_INVALID_HANDLE */
    }
    const uint32_t i = nc_vm_rd32(k->vm, a[0] + FO_INDEX);
    if (i >= MAX_FILES || !g_files[i].used) {
        return 0xC0000008u;
    }
    uint32_t len;
    const uint32_t st = volume_info(k, &g_files[i], a[1], a[3], &len);
    if (a[4]) {
        nc_vm_wr32(k->vm, a[4], len);
    }
    return st;
}

/* ------------------------------------------------------------ directories */

typedef struct DirEntry {
    char name[64];
    uint64_t size;
    bool dir;
} DirEntry;

static void disc_dir_walk(const uint8_t *dir, uint32_t size, uint32_t off, DirEntry *out, unsigned *n, unsigned cap,
                          unsigned depth)
{
    if (off + 14 > size || depth > 64 || *n >= cap) {
        return;
    }
    const uint8_t *e = dir + off;
    const uint16_t left = nc_rd16(e), right = nc_rd16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF && e[12] == 0xFF) {
        return;
    }
    if (left && left != 0xFFFF) { /* 0: no subtree */
        disc_dir_walk(dir, size, left * 4u, out, n, cap, depth + 1);
    }
    const uint8_t nlen = e[13];
    if (*n < cap && off + 14 + nlen <= size && nlen < sizeof(out->name)) {
        DirEntry *d = &out[(*n)++];
        memcpy(d->name, e + 14, nlen);
        d->name[nlen] = 0;
        d->size = nc_rd32(e + 8);
        d->dir = e[12] & 0x10;
    }
    if (right && right != 0xFFFF) {
        disc_dir_walk(dir, size, right * 4u, out, n, cap, depth + 1);
    }
}

/* A mod's file or folder in a disc folder's listing (nc_disc_overlay_list):
 * a replaced file with its new size, an added one at the end. */
typedef struct OvList {
    DirEntry *out;
    unsigned *n, cap;
} OvList;

static void ov_entry(void *ctx, const char *name, uint32_t size, bool is_dir)
{
    OvList *l = ctx;
    for (unsigned i = 0; i < *l->n; i++) {
        if (!strcasecmp(l->out[i].name, name)) {
            l->out[i].size = size;
            l->out[i].dir = is_dir;
            return;
        }
    }
    if (*l->n < l->cap && strlen(name) < sizeof(l->out->name)) {
        DirEntry *e = &l->out[(*l->n)++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->size = size;
        e->dir = is_dir;
    }
}

static unsigned list_dir(NcKernel *k, const NcFile *nf, DirEntry *out, unsigned cap)
{
    unsigned n = 0;
    if (nf->kind != F_HOST) {
        if (nf->disc.size && nf->disc.sector < NC_DISC_OVERLAY) {
            uint8_t *dir = malloc(nf->disc.size);
            if (dir && nc_disc_read(k->disc, &nf->disc, 0, dir, nf->disc.size)) {
                disc_dir_walk(dir, nf->disc.size, 0, out, &n, cap, 0);
            }
            free(dir);
        }
        const char *rel;
        if (k->disc->ov && device_of(nf->path, &rel) == -1) {
            OvList l = {out, &n, cap};
            nc_disc_overlay_list(k->disc, rel, ov_entry, &l);
        }
        return n;
    }
    DIR *d = opendir(nf->host);
    struct dirent *de;
    while (d && n < cap && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || strlen(de->d_name) >= sizeof(out->name)) {
            continue;
        }
        char p[600];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", nf->host, de->d_name);
        DirEntry *e = &out[n++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        e->dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        e->size = e->dir ? 0 : (uint64_t)st.st_size;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation,
 *   Length, FileInformationClass, FileMask, RestartScan): one entry per call.
 * FILE_DIRECTORY_INFORMATION: NextEntryOffset, FileIndex, four times, EndOfFile, AllocationSize,
 * FileAttributes 0x38, FileNameLength 0x3C, FileName (ANSI) 0x40. */
static uint64_t NtQueryDirectoryFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf || !nf->is_dir) {
        return nf ? STATUS_INVALID_DEVICE_REQUEST : NC_STATUS_INVALID_HANDLE;
    }
    if (a[7] != 1) {
        nc_log("hakuX-native-io", "directory information class %u not implemented", a[7]);
        return 0xC0000003u;
    }
    char mask[128] = "*";
    if (a[8]) {
        ansi(k, a[8], mask, sizeof(mask));
        if (!mask[0]) {
            snprintf(mask, sizeof(mask), "*"); /* an empty mask matches everything */
        }
    } else if (nf->listing) {
        /* FindNextFile passes no name: the enumeration goes on with its first
         * call's mask, as NT keeps it per handle. Read as '*', Riddick's
         * FindFirstFile("_filler???") / FindNextFile loop deleting its save
         * fillers deleted every file of the save, SaveMeta.xbx and its profile
         * with them, and the next checkpoint failed (a null object, then an
         * unhandled CCException and KeBugCheck). */
        snprintf(mask, sizeof(mask), "%s", nf->mask);
    }
    if ((a[9] & 0xff) || strcmp(mask, nf->mask) != 0 || !nf->listing) {
        /* A new enumeration lists the directory once (NcFile.listing). */
        static DirEntry scratch[2048];
        nf->dir_next = 0;
        snprintf(nf->mask, sizeof(nf->mask), "%s", mask);
        const unsigned count = list_dir(k, nf, scratch, 2048);
        free(nf->listing);
        nf->listing = malloc((count ? count : 1) * sizeof(DirEntry));
        nf->nlisting = nf->listing ? count : 0;
        if (nf->listing) {
            memcpy(nf->listing, scratch, count * sizeof(DirEntry));
        }
    }
    const DirEntry *entries = nf->listing;
    const unsigned n = nf->nlisting;
    unsigned i = nf->dir_next;
    while (i < n && !wildcard(nf->mask, entries[i].name)) {
        i++;
    }
    uint32_t st;
    if (g_trace_io) {
        nc_log("hakuX-native-io", "list %s mask '%s' (%u entries): %s", nf->path, nf->mask, n,
               i < n ? entries[i].name : "no more");
    }
    if (i >= n) {
        st = nf->dir_next == 0 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        static unsigned none_logged;
        if (st == STATUS_NO_SUCH_FILE && none_logged++ < 50) {
            nc_log("hakuX-native-io", "list %s mask '%s': nothing matches (%u entries)", nf->path, nf->mask, n);
        }
        io_status(k, a[4], st, 0);
        complete(k, a);
        return st;
    }
    nf->dir_next = i + 1;
    const DirEntry *e = &entries[i];
    const uint32_t nlen = (uint32_t)strlen(e->name);
    if (a[6] < 0x40 + nlen) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    uint8_t *o = nc_vm_host(k->vm, a[5]);
    memset(o, 0, 0x40);
    nc_wr32(o + 4, i);
    for (int t = 0; t < 4; t++) {
        put64(o + 8 + 8 * t, DISC_TIME);
    }
    put64(o + 0x28, e->size);
    put64(o + 0x30, nc_align_up((uint32_t)e->size, 4096));
    nc_wr32(o + 0x38, e->dir ? FILE_ATTRIBUTE_DIRECTORY : (nf->kind == F_HOST ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY));
    nc_wr32(o + 0x3C, nlen);
    memcpy(o + 0x40, e->name, nlen);
    io_status(k, a[4], NC_STATUS_SUCCESS, 0x40 + nlen);
    complete(k, a);
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------------- path operations */

/* NtQueryFullAttributesFile(ObjectAttributes, FileInformation (FILE_NETWORK_OPEN_INFORMATION)) */
static uint64_t NtQueryFullAttributesFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t h = 0, info = 0;
    uint32_t st = do_open(k, 0, a[0], FILE_OPEN, 0, &h, &info);
    if (st != NC_STATUS_SUCCESS) {
        return st;
    }
    network_open_info(file_of(k, h), nc_vm_host(k->vm, a[1]));
    nc_ob_close(k, h);
    return NC_STATUS_SUCCESS;
}

/* NtDeleteFile(ObjectAttributes) */
static uint64_t NtDeleteFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[512], path[512], host[512];
    ansi(k, nc_vm_rd32(k->vm, a[0] + 4), name, sizeof(name));
    if (!resolve_name(k, nc_vm_rd32(k->vm, a[0]), name, sizeof(name))) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const char *rel;
    int dev;
    if (!canon(name, path, sizeof(path)) || (dev = device_of(path, &rel)) < 0) {
        nc_log("hakuX-native-io", "delete %s: no such drive", name);
        return STATUS_ACCESS_DENIED;
    }
    if (host_path(dev, rel, host, sizeof(host)) != 1) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    struct stat st;
    bool dir = stat(host, &st) == 0 && S_ISDIR(st.st_mode);
    NcFile *open_on = dir ? NULL : file_other(NULL, host, false);
    if (open_on) {
        open_on->del = true;                        /* it goes with the last handle on it */
        nc_log("hakuX-native-io", "delete %s: at its last close (from %08x)", path,
               nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
        return NC_STATUS_SUCCESS;
    }
    const uint32_t r = (dir ? rmdir(host) : unlink(host)) == 0 ? NC_STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    nc_log("hakuX-native-io", "delete %s: %s (from %08x)", path, r == NC_STATUS_SUCCESS ? "done" : strerror(errno),
           nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
    return r;
}

/* NtFlushBuffersFile(FileHandle, IoStatusBlock) */
static uint64_t NtFlushBuffersFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (nf && nf->fd >= 0) {
        flush_later(nf);
    }
    io_status(k, a[1], NC_STATUS_SUCCESS, 0);
    return NC_STATUS_SUCCESS;
}

#define IOCTL_DISK_GET_DRIVE_GEOMETRY  0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO  0x00074004u

/* Disk ioctls on the hard disk's partitions; returns the output length. */
static uint32_t disk_ioctl(const NcFile *nf, uint32_t code, uint8_t *out, uint32_t out_len, uint32_t *status)
{
    const int part = nf->partition;
    if (part < 0 || part > 5) {
        *status = STATUS_INVALID_DEVICE_REQUEST;
        return 0;
    }
    switch (code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY: /* Cylinders, MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector */
        if (out_len < 24) {
            break;
        }
        put64(out, HDD_BYTES / 512);
        nc_wr32(out + 8, 12); /* FixedMedia */
        nc_wr32(out + 12, 1);
        nc_wr32(out + 16, 1);
        nc_wr32(out + 20, 512);
        return 24;
    case IOCTL_DISK_GET_PARTITION_INFO: /* StartingOffset, PartitionLength, HiddenSectors,
                                           PartitionNumber, Type, BootIndicator, Recognized, Rewrite */
        if (out_len < 32) {
            break;
        }
        memset(out, 0, 32);
        put64(out, k_partitions[part][0]);
        put64(out + 8, part ? k_partitions[part][1] : HDD_BYTES);
        nc_wr32(out + 16, (uint32_t)(k_partitions[part][0] / 512));
        nc_wr32(out + 20, (uint32_t)part);
        out[26] = 1;
        return 32;
    default:
        *status = STATUS_INVALID_DEVICE_REQUEST;
        return 0;
    }
    *status = STATUS_BUFFER_TOO_SMALL;
    return 0;
}

/* IOCTL_SCSI_PASS_THROUGH_DIRECT to the DVD drive: a SCSI_PASS_THROUGH_DIRECT
 * (0x2C bytes: ScsiStatus +2, DataTransferLength +0xC, DataBuffer +0x14,
 * Cdb +0x1C). XAPI checks the disc with MODE SENSE(10) of the Xbox security
 * page (0x3E, the DVDX2 authentication page) and reboots to the dashboard
 * unless the drive reports the Xbox partition authenticated (Panzer Dragoon
 * Orta, XDK 4928): CDFValid, PartitionArea 1, Authentication. Other commands
 * read zeros. */
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x4D014u

static void scsi_pass_through(NcKernel *k, uint32_t sptd, uint32_t len)
{
    if (!sptd || len < 0x2C) {
        return;
    }
    const uint8_t *cdb = nc_vm_host(k->vm, sptd + 0x1C);
    const uint32_t n = nc_vm_rd32(k->vm, sptd + 0x0C), buf = nc_vm_rd32(k->vm, sptd + 0x14);
    *(uint8_t *)nc_vm_host(k->vm, sptd + 2) = 0; /* SCSI status: good */
    if (!buf || !n) {
        return;
    }
    uint8_t *d = nc_vm_host(k->vm, buf);
    memset(d, 0, n);
    if (cdb[0] == 0x5A && (cdb[2] & 0x3F) == 0x3E && n >= 13) {
        d[1] = (uint8_t)(n < 28 ? n - 2 : 26); /* mode data length: past itself */
        d[8] = 0x3E;                           /* the page, 0x12 bytes past its header */
        d[9] = 0x12;
        d[10] = 1;                             /* CDFValid */
        d[11] = 1;                             /* PartitionArea: the Xbox partition */
        d[12] = 1;                             /* Authentication */
    } else {
        nc_log("hakuX-native-io", "scsi %02x (%u bytes): zeros", cdb[0], n);
    }
}

/* A cache drive's format (XMountUtilityDrive) locks and dismounts its
 * partition: there's no volume state here to hold or drop. */
#define FSCTL_LOCK_VOLUME     0x00090018u
#define FSCTL_UNLOCK_VOLUME   0x0009001Cu
#define FSCTL_DISMOUNT_VOLUME 0x00090020u

/* NtFsControlFile / NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext,
 *   IoStatusBlock, ControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength).
 * A code not known here fails as the kernel's own does (zeros and success let a
 * title carry on with them); each told once. 38 titles' first 20 s sent only
 * the ones handled. */
static uint64_t NtControlFile(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    nc_log("hakuX-native-io", "control %08x on %s (in %u, out %u)", a[5], nf ? nf->path : "?", a[7], a[9]);
    uint32_t status = NC_STATUS_SUCCESS, info = 0;
    if (nf && nf->partition >= 0 && (a[5] >> 16) == 7) { /* FILE_DEVICE_DISK */
        info = disk_ioctl(nf, a[5], a[8] ? nc_vm_host(k->vm, a[8]) : NULL, a[8] ? a[9] : 0, &status);
    } else if (a[5] == IOCTL_SCSI_PASS_THROUGH_DIRECT) {
        scsi_pass_through(k, a[6], a[7]);
    } else if (a[5] != FSCTL_LOCK_VOLUME && a[5] != FSCTL_UNLOCK_VOLUME && a[5] != FSCTL_DISMOUNT_VOLUME) {
        static uint32_t told[16];
        unsigned i = 0;
        while (i < 16 && told[i] && told[i] != a[5]) {
            i++;
        }
        if (i < 16 && !told[i]) {
            told[i] = a[5];
            nc_log("hakuX-native-io", "control %08x on %s: not known here (invalid device request)", a[5],
                   nf ? nf->path : "?");
        }
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    io_status(k, a[4], status, info);
    complete(k, a);
    return status;
}

/* IoSynchronousDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
 *   OutputBuffer, OutputBufferLength, ReturnedOutputBufferLength, InternalDeviceIoControl) */
static uint64_t IoSynchronousDeviceIoControlRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native-io", "device control %08x on device %08x (out %u)", a[0], a[1], a[5]);
    if (a[4] && a[5]) {
        memset(nc_vm_host(k->vm, a[4]), 0, a[5]);
    }
    if (a[6]) {
        nc_vm_wr32(k->vm, a[6], 0);
    }
    return NC_STATUS_SUCCESS;
}

/* -------------------------------------------------------- symbolic links */

/* IoCreateSymbolicLink(SymbolicLinkName, DeviceName) */
static uint64_t IoCreateSymbolicLink(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64], target[256], canon_target[256];
    ansi(k, a[0], name, sizeof(name));
    ansi(k, a[1], target, sizeof(target));
    if (!canon(target, canon_target, sizeof(canon_target))) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    return link_set(name, canon_target) ? NC_STATUS_SUCCESS : NC_STATUS_INSUFFICIENT_RESOURCES;
}

/* IoDeleteSymbolicLink(SymbolicLinkName) */
static uint64_t IoDeleteSymbolicLink(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64];
    ansi(k, a[0], name, sizeof(name));
    int i = link_find(name);
    if (i < 0) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    g_links[i].name[0] = 0;
    return NC_STATUS_SUCCESS;
}

/* NtOpenSymbolicLinkObject(LinkHandle, ObjectAttributes) */
static uint64_t NtOpenSymbolicLinkObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[64];
    ansi(k, nc_vm_rd32(k->vm, a[1] + 4), name, sizeof(name));
    int i = link_find(name);
    if (i < 0) {
        nc_log("hakuX-native-io", "symbolic link %s: none", name);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    NcFile *nf = file_new();
    if (!nf) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nf->kind = F_LINK;
    snprintf(nf->path, sizeof(nf->path), "%s", g_links[i].target);
    nc_vm_wr32(k->vm, a[0], file_handle(k, nf));
    return NC_STATUS_SUCCESS;
}

/* NtQuerySymbolicLinkObject(LinkHandle, LinkTarget (ANSI_STRING), ReturnedLength) */
static uint64_t NtQuerySymbolicLinkObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcFile *nf = file_of(k, a[0]);
    if (!nf || nf->kind != F_LINK) {
        return NC_STATUS_INVALID_HANDLE;
    }
    const uint16_t len = (uint16_t)strlen(nf->path);
    const uint16_t max = *(uint16_t *)nc_vm_host(k->vm, a[1] + 2);
    if (a[2]) {
        nc_vm_wr32(k->vm, a[2], len);
    }
    if (max < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    memcpy(nc_vm_host(k->vm, nc_vm_rd32(k->vm, a[1] + 4)), nf->path, len);
    *(uint16_t *)nc_vm_host(k->vm, a[1]) = len;
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------------- device objects */

/* DEVICE_OBJECT (nxdk xboxkrnl.h, CC0): Type 0x00, Size 0x02, ReferenceCount
 * 0x04, DriverObject 0x08, MountedOrSelfDevice 0x0C, CurrentIrp 0x10, Flags
 * 0x14, DeviceExtension 0x18, DeviceType 0x1C, StartIoFlags 0x1D, StackSize
 * 0x1E, DeletePending 0x1F, SectorSize 0x20, AlignmentRequirement 0x24,
 * DeviceQueue 0x28 (KDEVICE_QUEUE: Type, Size, Busy, DeviceListHead),
 * DeviceLock 0x34 (KEVENT), StartIoKey 0x44; 0x48 bytes, the extension after it. */
#define DEVOBJ_SIZE     0x48u
#define DO_DEVICE_INITIALIZING 0x80u

static struct {
    char name[64];
    uint32_t object;
} g_devices[32];

static uint32_t device_named(const char *path)
{
    for (int i = 0; i < 32; i++) {
        if (g_devices[i].object && !strcasecmp(g_devices[i].name, path)) {
            return g_devices[i].object;
        }
    }
    return 0;
}

static void device_add(const char *path, uint32_t object)
{
    for (int i = 0; i < 32; i++) {
        if (!g_devices[i].object) {
            snprintf(g_devices[i].name, sizeof(g_devices[i].name), "%s", path);
            g_devices[i].object = object;
            return;
        }
    }
    nc_log("hakuX-native-io", "device table full: %s not named", path);
}

static uint32_t device_new(NcKernel *k, uint32_t driver, uint32_t ext_size, uint32_t type)
{
    const uint32_t ext = nc_align_up(ext_size, 16);
    const uint32_t d = nc_ob_create(k, NC_OB_DEVICE, DEVOBJ_SIZE + ext);
    if (!d) {
        return 0;
    }
    uint8_t *p = nc_vm_host(k->vm, d);
    *(uint16_t *)p = 3; /* IO_TYPE_DEVICE */
    *(uint16_t *)(p + 2) = (uint16_t)(DEVOBJ_SIZE + ext);
    nc_vm_wr32(k->vm, d + 0x08, driver);
    nc_vm_wr32(k->vm, d + 0x14, DO_DEVICE_INITIALIZING);
    nc_vm_wr32(k->vm, d + 0x18, ext_size ? d + DEVOBJ_SIZE : 0);
    p[0x1C] = (uint8_t)type;
    p[0x1E] = 1; /* StackSize */
    nc_vm_wr32(k->vm, d + 0x24, 0);
    *(int16_t *)(p + 0x28) = 0x14;      /* DeviceQueueObject */
    p[0x2A] = 0x0C / 4;
    nc_vm_wr32(k->vm, d + 0x2C, d + 0x2C);
    nc_vm_wr32(k->vm, d + 0x30, d + 0x2C);
    p[0x34] = 1;                        /* DeviceLock: synchronization event, signaled */
    p[0x36] = 4;
    nc_vm_wr32(k->vm, d + 0x38, 1);
    nc_vm_wr32(k->vm, d + 0x3C, d + 0x3C);
    nc_vm_wr32(k->vm, d + 0x40, d + 0x3C);
    return d;
}

/* IoCreateDevice(DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, DeviceObject) */
static uint64_t IoCreateDevice(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[128] = "", path[256] = "";
    if (a[2]) {
        ansi(k, a[2], name, sizeof(name));
        if (!canon(name, path, sizeof(path))) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        if (device_named(path)) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    const uint32_t d = device_new(k, a[0], a[1], a[3]);
    if (!d) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (path[0]) {
        device_add(path, d);
    }
    nc_vm_wr32(k->vm, a[5], d);
    nc_log("hakuX-native-io", "device %s created: type %02x, driver %08x, extension %u", path[0] ? path : "(unnamed)",
           a[3], a[0], a[1]);
    return NC_STATUS_SUCCESS;
}

/* IoDeleteDevice(DeviceObject) */
static uint64_t IoDeleteDevice(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    for (int i = 0; i < 32; i++) {
        if (g_devices[i].object == a[0]) {
            g_devices[i].object = 0;
        }
    }
    *((uint8_t *)nc_vm_host(k->vm, a[0]) + 0x1F) = 1; /* DeletePending */
    return 0;
}

/* ObReferenceObjectByName(ObjectName, Attributes, ObjectType, ParseContext, Object): devices only.
 * Created devices by name; the built-in disc and partitions on first reference. */
static uint64_t ObReferenceObjectByName(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[512], path[512];
    ansi(k, a[0], name, sizeof(name));
    const char *rel;
    if (!canon(name, path, sizeof(path))) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    uint32_t d = device_named(path);
    if (!d) {
        if (device_of(path, &rel) == -2 || *rel) {
            nc_log("hakuX-native-io", "reference by name %s: not found", name);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        d = device_new(k, 0, 0, device_of(path, &rel) == -1 ? 0x02 : 0x07);
        if (!d) {
            return NC_STATUS_INSUFFICIENT_RESOURCES;
        }
        device_add(path, d);
    }
    nc_vm_wr32(k->vm, a[4], d);
    return NC_STATUS_SUCCESS;
}

void nc_io_register(NcKernel *k)
{
    g_trace_io = getenv("NC_TRACE_IO") != NULL;
    link_set("\\??\\D:", "\\Device\\CdRom0");

    nc_kernel_impl(k, "NtCreateFile", NtCreateFile);
    nc_kernel_impl(k, "NtOpenFile", NtOpenFile);
    nc_kernel_impl(k, "NtReadFile", NtReadFile);
    nc_kernel_impl(k, "NtWriteFile", NtWriteFile);
    nc_kernel_impl(k, "NtQueryInformationFile", NtQueryInformationFile);
    nc_kernel_impl(k, "NtSetInformationFile", NtSetInformationFile);
    nc_kernel_impl(k, "NtQueryVolumeInformationFile", NtQueryVolumeInformationFile);
    nc_kernel_impl(k, "NtQueryDirectoryFile", NtQueryDirectoryFile);
    nc_kernel_impl(k, "IoQueryVolumeInformation", IoQueryVolumeInformation);
    nc_kernel_impl(k, "NtQueryFullAttributesFile", NtQueryFullAttributesFile);
    nc_kernel_impl(k, "NtDeleteFile", NtDeleteFile);
    nc_kernel_impl(k, "NtFlushBuffersFile", NtFlushBuffersFile);
    nc_kernel_impl(k, "NtFsControlFile", NtControlFile);
    nc_kernel_impl(k, "NtDeviceIoControlFile", NtControlFile);
    nc_kernel_impl(k, "IoSynchronousDeviceIoControlRequest", IoSynchronousDeviceIoControlRequest);
    nc_kernel_impl(k, "IoCreateSymbolicLink", IoCreateSymbolicLink);
    nc_kernel_impl(k, "IoDeleteSymbolicLink", IoDeleteSymbolicLink);
    nc_kernel_impl(k, "NtOpenSymbolicLinkObject", NtOpenSymbolicLinkObject);
    nc_kernel_impl(k, "NtQuerySymbolicLinkObject", NtQuerySymbolicLinkObject);
    nc_kernel_impl(k, "ObReferenceObjectByName", ObReferenceObjectByName);
    nc_kernel_impl(k, "IoCreateDevice", IoCreateDevice);
    nc_kernel_impl(k, "IoDeleteDevice", IoDeleteDevice);
}

/* ---------------------------------------------------------- savestates */

unsigned nc_io_cache_partitions(void)
{
    /* the utility drive (XMountUtilityDrive's link) and any file open there */
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    unsigned mask = 0;
    for (unsigned i = 0; i < MAX_LINKS; i++) {
        const char *t = g_links[i].target;
        if (g_links[i].name[0] && prefix_ci(t, hd) && t[sizeof(hd) - 1] >= '3' && t[sizeof(hd) - 1] <= '5') {
            mask |= 1u << (t[sizeof(hd) - 1] - '0');
        }
    }
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (nf->used && nf->kind == F_HOST && nf->partition >= 3 && nf->partition <= 5) {
            mask |= 1u << nf->partition;
        }
    }
    return mask;
}

typedef struct NcFileState {
    uint32_t nfiles;
} NcFileState;

#define NC_DEF_SECTION FILE
#define NC_DEF_FILE "native/state/file.def"
#include "native/state/def.h"
#include "native/state/sections.h"

const char *nc_io_state_drain(NcKernel *k, uint64_t timeout_ns)
{
    /* the game's own save on E: half written: not a state to come back to */
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (nf->used && nf->kind == F_HOST && nf->partition == 1 && nf->wrote) {
            nc_log("hakuX-native-state", "%s is being written", nf->path);
            return "the game is writing its save; try again in a moment";
        }
    }
    /* the reads in flight land, and the kernel completes them (the host's
     * clock: the guest's stands still) */
    for (uint64_t waited = 0;; waited += 1000000) {
        pthread_mutex_lock(&g_reader.mu);
        const bool idle = g_reader.busy == g_reader.ndone;
        pthread_mutex_unlock(&g_reader.mu);
        if (idle) {
            break;
        }
        if (waited > timeout_ns) {
            return "a disc read did not finish";
        }
        usleep(1000);
    }
    nc_irq_post_work();
    nc_io_poll(k);
    return NULL;
}

static void file_drop(NcFile *nf);

void nc_io_stop(void)
{
    pthread_mutex_lock(&g_closer.mu);
    const bool closer = g_closer.started;
    g_closer.quit = true;
    pthread_cond_signal(&g_closer.cv);
    pthread_mutex_unlock(&g_closer.mu);
    if (closer) {
        pthread_join(g_closer.th, NULL);    /* every queued sync and close done */
    }
    pthread_mutex_lock(&g_reader.mu);
    const bool reader = g_reader.started;
    g_reader.quit = true;
    pthread_cond_signal(&g_reader.cv);
    pthread_mutex_unlock(&g_reader.mu);
    if (reader) {
        pthread_join(g_reader.th, NULL);    /* every queued read landed */
    }
    /* read, but never completed for the guest: their descriptors and runs */
    for (unsigned i = 0; i < g_reader.ndone; i++) {
        if (g_reader.done[i].fd >= 0) {
            close(g_reader.done[i].fd);
        }
        free(g_reader.done[i].seg);
    }
    g_closer.started = g_closer.quit = false;
    g_closer.head = g_closer.n = 0;
    g_reader.started = g_reader.quit = false;
    g_reader.head = g_reader.n = g_reader.ndone = g_reader.busy = 0;
}

void nc_io_reset(void)
{
    /* as the process's end leaves them: closed, the deleted and the side
     * files (.overwrite) as they are, swept by the next run (set_aside) */
    for (unsigned i = 0; i < MAX_FILES; i++) {
        file_drop(&g_files[i]);
    }
    memset(g_links, 0, sizeof(g_links));
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_cluster_sectors, 0, sizeof(g_cluster_sectors));
    g_swept = false;
    g_usage_bytes = g_usage_cluster = 0;
    memset(g_part_used, 0, sizeof(g_part_used));
    memset(g_part_scanned_ns, 0, sizeof(g_part_scanned_ns));
}

bool nc_file_state_save(NcBuf *b)
{
    NcFileState st = {0};
    for (unsigned i = 0; i < MAX_FILES; i++) {
        st.nfiles += g_files[i].used;
    }
    nc_rec_put(b, &nc_rec_NcFileState, &st);
    nc_recs_put(b, &nc_rec_NcLink, g_links, sizeof(g_links[0]), MAX_LINKS);
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const NcFile *nf = &g_files[i];
        if (!nf->used) {
            continue;
        }
        nc_buf_u32(b, i);
        nc_rec_put(b, &nc_rec_NcFile, nf);
        nc_recs_put(b, &nc_rec_DirEntry, nf->listing, sizeof(DirEntry), nf->listing ? nf->nlisting : 0);
    }
    return true;
}

static void file_drop(NcFile *nf)
{
    if (nf->used) {
        if (nf->fd >= 0) {
            close(nf->fd);
        }
        free(nf->listing);
        memset(nf, 0, sizeof(*nf));     /* only those used: the rest stay untouched */
    }
}

bool nc_file_state_load(NcRd *r, unsigned v)
{
    NcFileState st = {0};
    nc_rec_get(r, &nc_rec_NcFileState, &st, v);
    for (unsigned i = 0; i < MAX_FILES; i++) {
        file_drop(&g_files[i]);
    }
    memset(g_links, 0, sizeof(g_links));
    nc_recs_get(r, &nc_rec_NcLink, g_links, sizeof(g_links[0]), MAX_LINKS, v);
    bool ok = !r->fail && st.nfiles <= MAX_FILES;
    for (unsigned n = 0; ok && n < st.nfiles; n++) {
        const uint32_t i = nc_rd_u32(r);
        if (i >= MAX_FILES || g_files[i].used) {
            ok = false;
            break;
        }
        NcFile *nf = &g_files[i];
        nc_rec_get(r, &nc_rec_NcFile, nf, v);
        nf->path[sizeof(nf->path) - 1] = nf->host[sizeof(nf->host) - 1] = 0;
        nf->mask[sizeof(nf->mask) - 1] = nf->old_path[sizeof(nf->old_path) - 1] = 0;
        nf->listing = NULL;
        if (nf->nlisting) {
            nf->listing = calloc(nf->nlisting, sizeof(DirEntry));
            if (!nf->listing) {
                ok = false;
                break;
            }
            nc_recs_get(r, &nc_rec_DirEntry, nf->listing, sizeof(DirEntry), nf->nlisting, v);
        }
        nf->fd = -1;
        if (nf->kind == F_HOST && !nf->is_dir) {
            nf->fd = open(nf->host, O_RDWR | O_CLOEXEC);
            if (nf->fd < 0 && errno == ENOENT && nf->partition >= 3 && v < 2) {
                /* not on the drive as saved (CACHE, restored first): deleted
                 * with this handle open, in a state from before deletes waited
                 * for the last close (NcFile.del). Its bytes weren't kept:
                 * empty, deleted at its last close as it would be now, so the
                 * next state keeps it (Sudeki's boot.baf). */
                nf->fd = open(nf->host, O_RDWR | O_CREAT | O_CLOEXEC, 0664);
                if (nf->fd >= 0) {
                    nf->del = true;
                    nc_log("hakuX-native-state", "%s: deleted while open when the state was saved; empty",
                           nf->path);
                }
            }
            if (nf->fd < 0) {
                nc_log("hakuX-native-state", "%s (%s) can't be opened again: %s", nf->path, nf->host,
                       strerror(errno));
                if (nf->partition >= 3) {
                    nc_state_fail("the game's cache file %s is gone: another game has used its cache drive "
                                  "since this state was saved", nf->path);
                } else {
                    nc_state_fail("a file the game had open, %s, can't be opened (%s)", nf->path, strerror(errno));
                }
                ok = false;
                break;
            }
        }
        if (nf->old_path[0]) {
            struct stat sb;
            if (stat(nf->old_path, &sb) != 0) {
                nc_log("hakuX-native-state", "%s: its overwritten bytes (%s) are gone", nf->path, nf->old_path);
                nf->old_path[0] = 0;
                nf->old_size = 0;
            } else {
                g_swept = true;                 /* not this run: the file still needs it */
            }
        }
        if (nc_state_trace()) {
            nc_log("hakuX-native-state", "file %u: %s at %llu", i, nf->path, (unsigned long long)nf->pos);
        }
    }
    return ok && !r->fail;
}
