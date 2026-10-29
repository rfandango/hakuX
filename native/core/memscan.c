/*
 * hakuX native core — debug: guest memory scans for mods and cheats.
 *
 * debug.hakux.memscan, a new value, on the guest CPU's thread at a vblank:
 *   snap        keeps a copy of guest RAM and the vblank count
 *   diff        lists what changed since like a frame counter: 32 / 16-bit
 *               integers up by the vblanks elapsed (V, +-2), floats up by
 *               about V (frames) or V / 60 (seconds); hakuX-native-scan,
 *               with each page's VA (the title's mapping, else the
 *               contiguous window)
 *   u32=<n>, u16=<n>, f32=<x>
 *               lists the addresses holding that value (the first 300)
 *   poke=<va>:<value>[:<bytes>]
 *               writes 1, 2 or 4 bytes there once (hex)
 *   peek=<va>:<len>
 *               logs len bytes from va (hex, up to 4 KiB), 32 a line
 *   mats        lists runs of rigid 4x4 matrices a stride apart (bone
 *               palettes) and how many changed since the snapshot
 *   mats1[:<va>] a snapshot, then mats at the next vblank (what one frame
 *               changes), with the guest word at va (hex) at both ends
 * A trailing ",<salt>" makes a repeat a new value.
 */
#include "native/core/memscan.h"

#include "native/dev/dev.h"
#include "hw/xbox/hle/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/hle/prop.h"

#define SCAN_MAX 300

static uint8_t *g_snap;
static uint64_t g_snap_vblank;

static uint32_t va_of(const NcVm *vm, uint32_t pa)
{
    const uint32_t v = vm->rmap[pa / NC_PAGE];
    return v ? (v & ~(NC_PAGE - 1)) | (pa & (NC_PAGE - 1)) : NC_CONTIG_WINDOW | pa;
}

static void snap(const NcVm *vm)
{
    if (!g_snap) {
        g_snap = malloc(vm->ram_size);
    }
    if (g_snap) {
        memcpy(g_snap, vm->phys, vm->ram_size);
        g_snap_vblank = nc_dev_nv2a_vblanks();
        nc_log("hakuX-native-scan", "snapshot of %u MiB at vblank %llu", vm->ram_size >> 20,
               (unsigned long long)g_snap_vblank);
    }
}

static void diff(const NcVm *vm)
{
    if (!g_snap) {
        nc_log("hakuX-native-scan", "no snapshot");
        return;
    }
    const int64_t v = (int64_t)(nc_dev_nv2a_vblanks() - g_snap_vblank);
    unsigned n = 0;
    nc_log("hakuX-native-scan", "diff over %lld vblanks", (long long)v);
    for (uint32_t pa = 0; pa + 4 <= vm->ram_size && n < SCAN_MAX; pa += 4) {
        uint32_t a, b;
        memcpy(&a, g_snap + pa, 4);
        memcpy(&b, vm->phys + pa, 4);
        if (a == b) {
            continue;
        }
        const char *kind = NULL;
        const int64_t d32 = (int64_t)(int32_t)(b - a);
        const int16_t d16 = (int16_t)((uint16_t)b - (uint16_t)a);
        float fa, fb;
        memcpy(&fa, &a, 4);
        memcpy(&fb, &b, 4);
        const float df = fb - fa;
        if (d32 >= v - 2 && d32 <= v + 2) {
            kind = "u32 +frames";
        } else if ((uint16_t)(b >> 16) == (uint16_t)(a >> 16) && d16 >= v - 2 && d16 <= v + 2) {
            kind = "u16 +frames";
        } else if (isfinite(fa) && isfinite(fb) && fabsf(fa) < 1e7f && fabsf(df - (float)v) <= 0.6f) {
            kind = "f32 +frames";
        } else if (isfinite(fa) && isfinite(fb) && fabsf(fa) < 1e7f && v &&
                   fabsf(df - (float)v / 60.0f) <= (float)v / 60.0f * 0.03f) {
            kind = "f32 +seconds";
        }
        if (kind) {
            n++;
            nc_log("hakuX-native-scan", "%08x (pa %08x) %s: %08x -> %08x (%g -> %g)", va_of(vm, pa), pa, kind,
                   a, b, fa, fb);
        }
    }
    nc_log("hakuX-native-scan", "%u found%s", n, n == SCAN_MAX ? " (the first ones)" : "");
}

static void find(const NcVm *vm, const char *what)
{
    unsigned n = 0;
    uint32_t want = 0;
    unsigned width = 4;
    if (!strncmp(what, "u16=", 4)) {
        want = (uint32_t)strtoul(what + 4, NULL, 0);
        width = 2;
    } else if (!strncmp(what, "f32=", 4)) {
        const float f = strtof(what + 4, NULL);
        memcpy(&want, &f, 4);
    } else {
        want = (uint32_t)strtoul(what + 4, NULL, 0);
    }
    for (uint32_t pa = 0; pa + width <= vm->ram_size && n < SCAN_MAX; pa += width) {
        uint32_t x = 0;
        memcpy(&x, vm->phys + pa, width);
        if (x == want) {
            n++;
            nc_log("hakuX-native-scan", "%08x (pa %08x) = %s", va_of(vm, pa), pa, what);
        }
    }
    nc_log("hakuX-native-scan", "%s: %u found%s", what, n, n == SCAN_MAX ? " (the first ones)" : "");
}

/* A rigid D3D matrix (row major): rows 0-2 unit length and orthogonal, the
 * last column 0, 0, 0, 1, a translation of sane size. */
static bool rigid(const uint8_t *p)
{
    float m[16];
    memcpy(m, p, sizeof(m));
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) {
        return false;
    }
    for (int r = 0; r < 3; r++) {
        const float *a = m + 4 * r;
        const float len = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(fabsf(len - 1.0f) < 2e-3f)) {
            return false;
        }
        const float *b = m + 4 * ((r + 1) % 3);
        if (!(fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) < 2e-3f)) {
            return false;
        }
    }
    return isfinite(m[12]) && isfinite(m[13]) && isfinite(m[14]) &&
           fabsf(m[12]) < 1e5f && fabsf(m[13]) < 1e5f && fabsf(m[14]) < 1e5f;
}

typedef struct {
    uint32_t va, pa;
} PageVa;

static int by_va(const void *a, const void *b)
{
    const uint32_t x = ((const PageVa *)a)->va, y = ((const PageVa *)b)->va;
    return x < y ? -1 : x > y;
}

typedef struct {
    unsigned runs;
    uint32_t start, stride, last, count, changed;
    float t[3];
} Runs;

static void run_end(Runs *r)
{
    if (r->count >= 4 && r->runs < SCAN_MAX) {
        r->runs++;
        nc_log("hakuX-native-scan", "%08x stride %x: %u matrices, %u changed; first at %.2f %.2f %.2f", r->start,
               r->stride, r->count, r->changed, r->t[0], r->t[1], r->t[2]);
    }
    r->count = 0;
}

static void run_add(Runs *r, uint32_t va, bool ch, const uint8_t *m)
{
    if (r->count && va - r->last == r->stride) {
        r->count++;
    } else if (r->count == 1 && va - r->last >= 48 && va - r->last <= 0x400) {
        r->stride = va - r->last;       /* the second of a run */
        r->count = 2;
    } else {
        run_end(r);
        r->start = va;
        r->count = 1;
        r->changed = 0;
        memcpy(r->t, m + 48, sizeof(r->t));
    }
    r->changed += ch;
    r->last = va;
}

/* "mats": runs of 4 or more rigid matrices a fixed stride apart in the
 * guest's VA space (bone palettes, object transforms): where, the stride,
 * how many, how many changed since the snapshot, the first one's
 * translation. */
static void mats(const NcVm *vm)
{
    const uint32_t pages = vm->ram_size / NC_PAGE;
    PageVa *pv = malloc(pages * sizeof(*pv));
    uint8_t *buf = malloc(4 * NC_PAGE);
    if (!pv || !buf) {
        free(pv);
        free(buf);
        return;
    }
    for (uint32_t p = 0; p < pages; p++) {
        const uint32_t v = vm->rmap[p];
        pv[p].va = v ? v & ~(NC_PAGE - 1) : NC_CONTIG_WINDOW | p * NC_PAGE;
        pv[p].pa = p * NC_PAGE;
    }
    qsort(pv, pages, sizeof(*pv), by_va);
    Runs r = {0};
    unsigned n = 0;
    uint8_t *cur = buf, *old = buf + 2 * NC_PAGE;
    for (uint32_t i = 0; i < pages; i++) {
        /* this page and the next one in VA (a matrix may cross into it) */
        const bool next = i + 1 < pages && pv[i + 1].va == pv[i].va + NC_PAGE;
        memcpy(cur, vm->phys + pv[i].pa, NC_PAGE);
        memset(cur + NC_PAGE, 0, NC_PAGE);
        if (next) {
            memcpy(cur + NC_PAGE, vm->phys + pv[i + 1].pa, NC_PAGE);
        }
        if (g_snap) {
            memcpy(old, g_snap + pv[i].pa, NC_PAGE);
            memset(old + NC_PAGE, 0, NC_PAGE);
            if (next) {
                memcpy(old + NC_PAGE, g_snap + pv[i + 1].pa, NC_PAGE);
            }
        }
        if (i && pv[i - 1].va + NC_PAGE != pv[i].va) {
            run_end(&r);
        }
        for (uint32_t off = 0; off < NC_PAGE; off += 4) {
            if (rigid(cur + off)) {
                n++;
                run_add(&r, pv[i].va + off, g_snap && memcmp(old + off, cur + off, 64) != 0, cur + off);
            }
        }
    }
    run_end(&r);
    free(pv);
    free(buf);
    nc_log("hakuX-native-scan", "mats: %u rigid matrices, %u runs of 4 or more%s", n, r.runs,
           g_snap ? "" : " (no snapshot: changed counts are 0)");
}

/* "poke=<va>:<value>[:<bytes>]" (hex): writes 1, 2 or 4 (default) bytes once */
static void poke(const NcVm *vm, const char *what)
{
    char *end;
    const uint32_t va = (uint32_t)strtoul(what, &end, 16);
    const uint32_t val = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
    const unsigned n = *end == ':' ? (unsigned)strtoul(end + 1, NULL, 0) : 4;
    if (n != 1 && n != 2 && n != 4) {
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(vm, va + i);
        if (pa == UINT32_MAX || pa >= vm->ram_size) {
            nc_log("hakuX-native-scan", "poke %08x: not mapped", va + i);
            return;
        }
        vm->phys[pa] = (uint8_t)(val >> (8 * i));
        hle_host_mem_set_dirty(pa, 1);
    }
    nc_log("hakuX-native-scan", "poke %08x = %x (%u bytes)", va, val, n);
}

/* "peek=<va>:<len>" (hex): the bytes there, as 32-bit words */
static void peek(const NcVm *vm, const char *what)
{
    char *end;
    const uint32_t va = (uint32_t)strtoul(what, &end, 16) & ~3u;
    uint32_t len = *end == ':' ? (uint32_t)strtoul(end + 1, NULL, 16) : 64;
    len = len > 4096 ? 4096 : len;
    for (uint32_t o = 0; o < len; o += 32) {
        char line[160];
        int n = snprintf(line, sizeof(line), "%08x:", va + o);
        for (uint32_t w = 0; w < 32 && o + w < len; w += 4) {
            const uint32_t pa = nc_vm_phys(vm, va + o + w);
            uint32_t v = 0;
            if (pa == UINT32_MAX || pa + 4 > vm->ram_size) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " --------");
                continue;
            }
            memcpy(&v, vm->phys + pa, 4);
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", v);
        }
        nc_log("hakuX-native-scan", "%s", line);
    }
}

static void log_word(const NcVm *vm, uint32_t va)
{
    const uint32_t pa = va ? nc_vm_phys(vm, va) : UINT32_MAX;
    if (pa != UINT32_MAX && pa + 4 <= vm->ram_size) {
        uint32_t w;
        memcpy(&w, vm->phys + pa, 4);
        nc_log("hakuX-native-scan", "%08x (pa %08x) = %08x at vblank %llu", va, pa, w,
               (unsigned long long)nc_dev_nv2a_vblanks());
    }
}

void nc_memscan_poll(const NcVm *vm)
{
    static unsigned polls;
    static char last[NC_PROPERTY_VALUE_MAX];
    static bool mats_next;
    static uint32_t mats_word;          /* "mats1:<va>": that guest word at both ends */
    if (mats_next) {
        mats_next = false;              /* "mats1": over one vblank */
        log_word(vm, mats_word);
        mats(vm);
        return;
    }
    if (polls++ % 30) {
        return;
    }
    char v[NC_PROPERTY_VALUE_MAX] = "";
    if (hle_prop_get("debug.hakux.memscan", v) <= 0 || !strcmp(v, last)) {
        return;
    }
    const bool first = !last[0] && polls == 1;
    snprintf(last, sizeof(last), "%s", v);
    if (first) {
        return;                         /* the value at start */
    }
    char *salt = strchr(v, ',');
    if (salt) {
        *salt = 0;
    }
    if (!strcmp(v, "snap")) {
        snap(vm);
    } else if (!strcmp(v, "diff")) {
        diff(vm);
    } else if (!strncmp(v, "u32=", 4) || !strncmp(v, "u16=", 4) || !strncmp(v, "f32=", 4)) {
        find(vm, v);
    } else if (!strncmp(v, "poke=", 5)) {
        poke(vm, v + 5);
    } else if (!strncmp(v, "peek=", 5)) {
        peek(vm, v + 5);
    } else if (!strcmp(v, "mats")) {
        mats(vm);
    } else if (!strncmp(v, "mats1", 5)) {
        mats_word = v[5] == ':' ? (uint32_t)strtoul(v + 6, NULL, 16) : 0;
        log_word(vm, mats_word);
        snap(vm);
        mats_next = true;
    }
}
