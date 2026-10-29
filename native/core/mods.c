/*
 * hakuX native core — mods and cheats (mods.h).
 */
#include "native/core/mods.h"

#include "native/core/cheats.h"
#include "native/core/memscan.h"
#include "native/cpu/cpu.h"
#include "native/dev/dev.h"
#include "native/kernel/kernel.h"
#include "hw/xbox/hle/host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char g_manifest[512];
static NcVm *g_vm;
static NcCpu *g_cpu;
static struct { uint32_t lo, hi; } g_exec[HLE_XBE_MAX_SECTIONS];  /* executable sections' VAs */
static unsigned g_nexec;
static uint32_t g_base;             /* the XBE's (its header page) */
static uint64_t g_vblank;           /* the cheats ran at this one */
static bool g_logged[NC_CHEATS_MAX];

void nc_mods_set_manifest(const char *path)
{
    snprintf(g_manifest, sizeof(g_manifest), "%s", path);
}

/* ---- XBE patches ---- */

/* An XBE's sections by its path on the disc (the last one asked for). */
static char g_xbe_last[256];
static HleXbeInfo g_xbe_info;
static bool g_xbe_ok;

static bool xbe_info(NcDisc *disc, const char *path, HleXbeInfo *x)
{
    if (!strcmp(g_xbe_last, path)) {
        *x = g_xbe_info;
        return g_xbe_ok;
    }
    snprintf(g_xbe_last, sizeof(g_xbe_last), "%s", path);
    g_xbe_ok = false;
    NcDiscFile f;
    uint8_t probe[0x110];
    if (!nc_disc_find(disc, path, &f) || f.size < sizeof(probe) || !nc_disc_read(disc, &f, 0, probe, sizeof(probe))) {
        return false;
    }
    const uint32_t hdr_size = nc_rd32(probe + 0x108);
    if (hdr_size < sizeof(probe) || hdr_size > f.size || hdr_size > (1u << 20)) {
        return false;
    }
    uint8_t *hdr = malloc(hdr_size);
    if (hdr && nc_disc_read(disc, &f, 0, hdr, hdr_size)) {
        g_xbe_ok = hle_xbe_parse(hdr, hdr_size, &g_xbe_info) == HLE_XBE_OK;
    }
    free(hdr);
    *x = g_xbe_info;
    return g_xbe_ok;
}

/* VA -> the XBE file's offset: its headers, or a section's raw data. The
 * headers' part of the file runs to the first section's data, past
 * sizeof_headers: a mod can put code there and raise sizeof_headers (0x108)
 * so the loader reads it into the header page. */
static bool xbe_offset(const HleXbeInfo *x, uint32_t va, uint32_t len, uint32_t *off)
{
    uint32_t hdr_end = 0x1000;
    for (unsigned i = 0; i < x->nsections; i++) {
        hdr_end = MIN(hdr_end, x->sections[i].raw_off);
    }
    hdr_end = MAX(hdr_end, x->sizeof_headers);
    if (va >= x->base && va - x->base + len <= hdr_end) {
        *off = va - x->base;
        return true;
    }
    for (unsigned i = 0; i < x->nsections; i++) {
        const HleXbeSection *s = &x->sections[i];
        if (va >= s->va && va - s->va + len <= s->raw_size) {
            *off = s->raw_off + (va - s->va);
            return true;
        }
    }
    return false;
}

static unsigned hex_bytes(const char *s, uint8_t *out, unsigned cap, const char **end)
{
    unsigned n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) || n == cap) {
            break;
        }
        char b[3] = {s[0], s[1], 0};
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    *end = s;
    return n;
}

/* The patches of the mods on (applied) and off, for memory a savestate
 * brings back (it holds the code as it was saved, with the mods of then):
 * the XBE's path, the VA, the bytes the disc has there and the mod's. */
typedef struct MemPatch {
    char xbe[128];
    uint32_t va;
    unsigned n;
    bool on;
    uint8_t *old, *neu;
} MemPatch;
static MemPatch *g_mp;
static unsigned g_nmp;
/* a savestate's (its MODS section): the patches of the mods on when it was
 * saved; none in a state from before the section */
static MemPatch *g_saved;
static unsigned g_nsaved;
static bool g_have_saved;

static void remember(MemPatch **list, unsigned *count, const char *xbe, uint32_t va, const uint8_t *old,
                     const uint8_t *neu, unsigned n, bool on)
{
    MemPatch *mp = realloc(*list, (*count + 1) * sizeof(*mp));
    if (!mp) {
        return;
    }
    *list = mp;
    MemPatch *p = &mp[*count];
    snprintf(p->xbe, sizeof(p->xbe), "%s", xbe);
    p->va = va;
    p->n = n;
    p->on = on;
    p->old = malloc(n);
    p->neu = malloc(n);
    if (p->old && p->neu) {
        memcpy(p->old, old, n);
        memcpy(p->neu, neu, n);
        (*count)++;
    }
}

/* "0x0011A2B4: 90 90" or "0x0011A2B4: 74 05 -> EB 05"; a mod that is off
 * (on false) is only remembered, with the disc's bytes */
static void patch(NcDisc *disc, const char *xbe, const char *mod, const char *line, bool on)
{
    char *colon;
    const uint32_t va = (uint32_t)strtoul(line, &colon, 0);
    while (*colon == ' ') {
        colon++;
    }
    uint8_t old[256], neu[256];
    const char *p = colon;
    unsigned nold = 0, nnew = 0;
    if (*colon == ':') {
        nnew = hex_bytes(colon + 1, neu, sizeof(neu), &p);
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (p[0] == '-' && p[1] == '>') {
            memcpy(old, neu, nnew);
            nold = nnew;
            nnew = hex_bytes(p + 2, neu, sizeof(neu), &p);
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    HleXbeInfo x;
    uint32_t off;
    if (*colon != ':' || !nnew || (nold && nold != nnew) || (*p && *p != '#')) {
        nc_log("hakuX-native-mods", "%s: [%s] \"%s\": not a patch", mod, xbe, line);
    } else if (!xbe_info(disc, xbe, &x)) {
        nc_log("hakuX-native-mods", "%s: [%s] isn't an XBE on the disc", mod, xbe);
    } else if (!xbe_offset(&x, va, nnew, &off)) {
        nc_log("hakuX-native-mods", "%s: [%s] %08x: not in the XBE's file", mod, xbe, va);
    } else if (!on) {
        NcDiscFile f;
        uint8_t orig[256];
        if (nc_disc_find_orig(disc, xbe, &f) && nc_disc_read_orig(disc, &f, off, orig, nnew)) {
            remember(&g_mp, &g_nmp, xbe, va, orig, neu, nnew, false);
        }
    } else {
        NcDiscFile f;
        uint8_t cur[256];
        if (!nc_disc_find(disc, xbe, &f) || !nc_disc_read(disc, &f, off, cur, nnew) ||
            (nold && memcmp(cur, old, nold))) {
            nc_log("hakuX-native-mods", "%s: [%s] %08x: other bytes there, skipped", mod, xbe, va);
        } else if (nc_disc_overlay_patch(disc, xbe, off, neu, nnew)) {
            remember(&g_mp, &g_nmp, xbe, va, cur, neu, nnew, true);
            nc_log("hakuX-native-mods", "%s: [%s] %08x: %u bytes", mod, xbe, va, nnew);
        }
    }
}

/* The file name of a path (either slash). */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            b = p + 1;
        }
    }
    return b;
}

static bool mem_read(void *opaque, uint32_t va, void *buf, unsigned n);
static bool mem_write(void *opaque, uint32_t va, const void *buf, unsigned n);

static bool of_image(const MemPatch *p, const char *image_path)
{
    return !strcasecmp(base_name(p->xbe), base_name(image_path));
}

static bool same_patch(const MemPatch *a, const MemPatch *b)
{
    return a->va == b->va && a->n == b->n && !memcmp(a->old, b->old, a->n) && !memcmp(a->neu, b->neu, a->n);
}

/* A patch put in (on) where memory holds the disc's bytes, or taken back
 * where it holds the mod's. */
static bool flip(const MemPatch *p, bool on)
{
    uint8_t cur[256];
    const uint8_t *from = on ? p->old : p->neu, *to = on ? p->neu : p->old;
    return mem_read(NULL, p->va, cur, p->n) && !memcmp(cur, from, p->n) && mem_write(NULL, p->va, to, p->n);
}

/* The patch is in the XBE's header page (the mods' code past its headers). */
static bool in_header(const MemPatch *p)
{
    return p->va - g_base < 0x1000;
}

/* The state was saved with the XBE's patches of the mods on now (header:
 * those in its header page). */
static bool saved_as_now(const char *image_path, bool header)
{
    unsigned i = 0, k = 0;
    for (;;) {
        while (i < g_nsaved && !(of_image(&g_saved[i], image_path) && (!header || in_header(&g_saved[i])))) {
            i++;
        }
        while (k < g_nmp && !(g_mp[k].on && of_image(&g_mp[k], image_path) && (!header || in_header(&g_mp[k])))) {
            k++;
        }
        if (i == g_nsaved || k == g_nmp) {
            return i == g_nsaved && k == g_nmp;
        }
        if (!same_patch(&g_saved[i++], &g_mp[k++])) {
            return false;
        }
    }
}

/* The disc's sizeof_headers, from a patch over it (0: none, no mod has code
 * in the header page). */
static uint32_t disc_headers(const char *image_path)
{
    const uint32_t va = g_base + 0x108;
    for (unsigned l = 0; l < 2; l++) {
        const MemPatch *list = l ? g_saved : g_mp;
        for (unsigned i = 0, n = l ? g_nsaved : g_nmp; i < n; i++) {
            const MemPatch *p = &list[i];
            if (of_image(p, image_path) && p->va <= va && va + 4 <= p->va + p->n) {
                uint32_t h;
                memcpy(&h, p->old + (va - p->va), 4);
                return h;
            }
        }
    }
    return 0;
}

/* A state from before the MODS section: its header page has the code of the
 * mods on now (each patch there holds the mod's bytes); *any: there is some. */
static bool header_as_now(const char *image_path, bool *any)
{
    *any = false;
    for (unsigned i = 0; i < g_nmp; i++) {
        const MemPatch *p = &g_mp[i];
        uint8_t cur[256];
        if (p->on && of_image(p, image_path) && in_header(p)) {
            *any = true;
            if (!mem_read(NULL, p->va, cur, p->n) || memcmp(cur, p->neu, p->n)) {
                return false;
            }
        }
    }
    return true;
}

/* Past the disc's headers the header page holds the mods' code (patches,
 * sizeof_headers raised) and their variables (not patched, so a state's
 * values survive the patches written over it). The file has zeros there:
 * cleared, the mods on now start as after a boot, where the code or the
 * variables of the state's mods were. Returns the bytes that were set. */
static unsigned clear_header_page(const char *image_path)
{
    uint8_t now[0x1000];
    const uint32_t h0 = disc_headers(image_path);
    if (!h0 || h0 >= sizeof(now) || !mem_read(NULL, g_base, now, sizeof(now))) {
        return 0;
    }
    static const uint8_t zero = 0;
    unsigned cleared = 0;
    for (uint32_t o = h0; o < sizeof(now); o++) {
        if (now[o] && mem_write(NULL, g_base + o, &zero, 1)) {
            cleared++;
        }
    }
    return cleared;
}

void nc_mods_after_load(NcKernel *k, const char *image_path)
{
    if (!image_path[0]) {
        image_path = "\\default.xbe";   /* XeImageFileName's default */
    }
    if (g_have_saved && saved_as_now(image_path, false)) {
        return;                         /* its memory has the patches of now */
    }
    bool hdr_on = false;
    const bool hdr_as_now = g_have_saved ? saved_as_now(image_path, true) : header_as_now(image_path, &hdr_on);
    unsigned done = 0, undone = 0, kept = 0, hdr_undone = 0;
    if (g_have_saved) {
        /* the state's patches taken back, the last first (one may be over
         * another's bytes) */
        for (unsigned i = g_nsaved; i-- > 0;) {
            if (of_image(&g_saved[i], image_path)) {
                *(flip(&g_saved[i], false) ? &undone : &kept) += 1;
            }
        }
    } else {
        /* a state from before the MODS section: the mods off taken back where
         * their bytes are, before the mods on go in (a 120 fps mod and its
         * game's 60 fps one share patches) */
        for (unsigned i = 0; i < g_nmp; i++) {
            const MemPatch *p = &g_mp[i];
            if (!p->on && of_image(p, image_path)) {
                const bool back = flip(p, false);
                *(back ? &undone : &kept) += 1;
                hdr_undone += back && in_header(p);
            }
        }
    }
    /* The header page gets other code (old states: the mods on don't have
     * theirs there, or none and a mod off had): its mods' space is cleared
     * before the mods on now go in, and the contiguous memory the code of
     * then allocated is freed (nothing else runs from the header page). */
    unsigned cleared = 0, freed = 0;
    if (!hdr_as_now || (!g_have_saved && hdr_undone && !hdr_on)) {
        cleared = clear_header_page(image_path);
        freed = nc_mm_contig_free_callers(k, g_base, g_base + 0x1000);
    }
    /* the mods on: their bytes where the disc's are */
    for (unsigned i = 0; i < g_nmp; i++) {
        if (g_mp[i].on && of_image(&g_mp[i], image_path)) {
            *(flip(&g_mp[i], true) ? &done : &kept) += 1;
        }
    }
    if (done || undone || kept || cleared || freed) {
        nc_log("hakuX-native-mods", "savestate %s: %u XBE patches applied to its memory, %u of %s taken back, "
               "%u left as they were; header page: %u bytes cleared, %u KiB its code allocated freed",
               g_have_saved ? "made with other mods" : "from before its mods were saved", done, undone,
               g_have_saved ? "its mods" : "mods off", kept, cleared, freed >> 10);
    }
}

/* ---- the manifest ---- */

static char *field(char **s)
{
    char *f = *s, *t = strchr(f, '\t');
    if (t) {
        *t = 0;
        *s = t + 1;
    } else {
        *s = f + strlen(f);
    }
    return f;
}

void nc_mods_install(NcDisc *disc)
{
    if (!g_manifest[0]) {
        return;
    }
    FILE *fp = fopen(g_manifest, "r");
    if (!fp) {
        nc_log("hakuX-native-mods", "no manifest at %s", g_manifest);
        return;
    }
    char line[1024];
    unsigned files = 0, bad = 0, lines = 0;
    bool sealed = false, have_cheat = false;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        const char *kind = field(&s);
        if (!strcmp(kind, "refresh")) {
            const unsigned hz = (unsigned)atoi(s);
            nc_dev_nv2a_vblank_multiply((hz + 30) / 60);
            nc_log("hakuX-native-mods", "vblank at %u Hz for a 60 Hz mode", (hz + 30) / 60 * 60);
            continue;
        }
        if (!strcmp(kind, "file")) {
            const char *path = field(&s);
            if (nc_disc_overlay_file(disc, path, s)) {
                files++;
            } else {
                nc_log("hakuX-native-mods", "file %s from %s: can't be used", path, s);
            }
            continue;
        }
        if (!sealed) {
            nc_disc_overlay_seal(disc);
            sealed = true;
        }
        if (!strcmp(kind, "patch") || !strcmp(kind, "patchoff")) {
            const char *xbe = field(&s), *mod = field(&s);
            patch(disc, xbe, mod, s, kind[5] != 'o');
        } else if (!strcmp(kind, "cheat")) {
            const bool on = atoi(field(&s)) != 0, once = atoi(field(&s)) != 0;
            have_cheat = nc_cheat_add(s, on, once) >= 0;
        } else if (!strcmp(kind, "code") && have_cheat) {
            char err[64];
            lines++;
            if (!nc_cheat_line(s, err, sizeof(err))) {
                bad++;
                nc_log("hakuX-native-mods", "cheat %s: \"%s\": %s", nc_cheat_name((int)nc_cheat_count() - 1), s,
                       err);
            }
        }
    }
    fclose(fp);
    if (!sealed) {
        nc_disc_overlay_seal(disc);
    }
    unsigned on = 0;
    for (unsigned i = 0; i < nc_cheat_count(); i++) {
        on += nc_cheat_on((int)i);
    }
    nc_log("hakuX-native-mods", "%u files over the disc; %u cheats (%u on), %u code lines (%u bad)", files,
           nc_cheat_count(), on, lines, bad);
}

/* ---- cheats ---- */

static bool mem_read(void *opaque, uint32_t va, void *buf, unsigned n)
{
    uint8_t *b = buf;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(g_vm, va + i);
        if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
            return false;
        }
        b[i] = g_vm->phys[pa];
    }
    return true;
}

static bool mem_write(void *opaque, uint32_t va, const void *buf, unsigned n)
{
    const uint8_t *b = buf;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(g_vm, va + i);
        if (pa == UINT32_MAX || pa >= g_vm->ram_size) {
            return false;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(g_vm, va + i);
        g_vm->phys[pa] = b[i];
        /* the HLE GPU's tracking (and the GPU's view) sees a host write */
        hle_host_mem_set_dirty(pa, 1);
    }
    for (unsigned i = 0; i < g_nexec; i++) {
        if (va < g_exec[i].hi && va + n > g_exec[i].lo) {
            nc_cpu_invalidate(g_cpu, va, n);    /* code: translated again */
            break;
        }
    }
    return true;
}

void nc_mods_bind(NcVm *vm, NcCpu *cpu, const HleXbeInfo *xbe)
{
    g_vm = vm;
    g_cpu = cpu;
    g_base = xbe->base;
    g_nexec = 0;
    for (unsigned i = 0; i < xbe->nsections && i < HLE_XBE_MAX_SECTIONS; i++) {
        if (xbe->sections[i].flags & 4) {       /* XBE_SECTION_EXECUTABLE */
            g_exec[g_nexec].lo = xbe->sections[i].va;
            g_exec[g_nexec].hi = xbe->sections[i].va + xbe->sections[i].vsize;
            g_nexec++;
        }
    }
}

void nc_mods_poll(void)
{
    if (!g_vm) {
        return;
    }
    const uint64_t v = nc_dev_nv2a_vblanks();
    if (v == g_vblank) {
        return;
    }
    g_vblank = v;
    nc_memscan_poll(g_vm);              /* debug.hakux.memscan */
    if (!nc_cheat_count()) {
        return;
    }
    static const NcCheatMem mem = {NULL, mem_read, mem_write};
    nc_cheats_run(&mem);
    for (unsigned i = 0; i < nc_cheat_count(); i++) {
        if (!g_logged[i] && nc_cheat_misses((int)i)) {
            g_logged[i] = true;
            nc_log("hakuX-native-mods", "cheat %s: an address isn't mapped (yet): its line skipped",
                   nc_cheat_name((int)i));
        }
    }
}

/* ---- the savestate's MODS section (native/state/mods.def) ---- */

typedef struct NcModsState {
    uint32_t npatches;
} NcModsState;

typedef struct NcModsPatch {
    char xbe[128];
    uint32_t va;
    uint32_t n;
} NcModsPatch;

#define NC_DEF_SECTION MODS
#define NC_DEF_FILE "native/state/mods.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_mods_state_save(NcBuf *b)
{
    NcModsState st = {0};
    for (unsigned i = 0; i < g_nmp; i++) {
        st.npatches += g_mp[i].on;
    }
    nc_rec_put(b, &nc_rec_NcModsState, &st);
    for (unsigned i = 0; i < g_nmp; i++) {
        const MemPatch *p = &g_mp[i];
        if (p->on) {
            NcModsPatch rec = {.va = p->va, .n = p->n};
            snprintf(rec.xbe, sizeof(rec.xbe), "%s", p->xbe);
            nc_rec_put(b, &nc_rec_NcModsPatch, &rec);
            nc_buf_put(b, p->old, p->n);
            nc_buf_put(b, p->neu, p->n);
        }
    }
    return true;
}

bool nc_mods_state_load(NcRd *r, unsigned v)
{
    NcModsState st = {0};
    nc_rec_get(r, &nc_rec_NcModsState, &st, v);
    for (uint32_t i = 0; !r->fail && i < st.npatches; i++) {
        NcModsPatch rec = {{0}};
        nc_rec_get(r, &nc_rec_NcModsPatch, &rec, v);
        rec.xbe[sizeof(rec.xbe) - 1] = 0;
        const uint8_t *old = rec.n && rec.n <= 256 ? nc_rd_ptr(r, rec.n) : NULL;
        const uint8_t *neu = old ? nc_rd_ptr(r, rec.n) : NULL;
        if (!neu) {
            return false;
        }
        remember(&g_saved, &g_nsaved, rec.xbe, rec.va, old, neu, rec.n, true);
    }
    g_have_saved = !r->fail;
    return !r->fail;
}

static void free_patches(MemPatch **list, unsigned *count)
{
    for (unsigned i = 0; i < *count; i++) {
        free((*list)[i].old);
        free((*list)[i].neu);
    }
    free(*list);
    *list = NULL;
    *count = 0;
}

void nc_mods_reset(void)
{
    g_vm = NULL;
    g_cpu = NULL;
    memset(g_exec, 0, sizeof(g_exec));
    g_nexec = 0;
    g_vblank = 0;
    memset(g_logged, 0, sizeof(g_logged));
    free_patches(&g_mp, &g_nmp);
    free_patches(&g_saved, &g_nsaved);
    g_have_saved = false;
    g_xbe_last[0] = 0;
    g_xbe_ok = false;
    memset(&g_xbe_info, 0, sizeof(g_xbe_info));
    nc_cheats_reset();
}
