/*
 * hakuX native core — XBE loader.
 *
 * Header fields beyond hle_xbe_parse (offsets from the base address):
 *   0x12C TLS directory VA, 0x130 PE stack commit size.
 *
 * As the Xbox kernel loads an XBE, the headers and the preloaded sections
 * get memory and the rest of the image is address space only, until
 * XeLoadSection (ex.c) brings a section in; XeUnloadSection gives its pages
 * back. Titles with many sections count on that memory (Fable preloads 15
 * of 107: backing the whole image left it 4 MiB short in gameplay).
 */
#include "native/core/xbe_load.h"
#include "native/core/mods.h"

#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define XBE_SECTIONS     0x120
#define XBE_TLS_DIR      0x12C
#define XBE_STACK_COMMIT 0x130
#define SEC_HDR_SIZE     0x38
#define SEC_REFS         0x18

bool nc_xbe_back(NcVm *vm, uint32_t va, uint32_t len)
{
    const uint64_t end = nc_align_up((uint64_t)va + len, NC_PAGE);
    for (uint64_t p = va & ~(NC_PAGE - 1); p < end;) {
        if (nc_vm_phys(vm, (uint32_t)p) != UINT32_MAX) {
            p += NC_PAGE;
            continue;
        }
        uint64_t q = p;
        while (q < end && nc_vm_phys(vm, (uint32_t)q) == UINT32_MAX) {
            q += NC_PAGE;
        }
        if (!nc_vm_alloc_at(vm, (uint32_t)p, (uint32_t)(q - p), NULL)) {
            return false;
        }
        p = q;
    }
    return true;
}

static char g_host[512];        /* the booted XBE's host file, when it has one */

bool nc_xbe_host_read(int fd, uint64_t off, void *buf, size_t len)
{
    if (pread(fd, buf, len, (off_t)off) != (ssize_t)len) {
        return false;
    }
    nc_mods_host_patch(g_host, off, buf, len);
    return true;
}

/* The XBE's bytes: from its host file, or the disc. */
static bool xbe_read(const NcDisc *disc, const NcXbe *x, uint64_t off, void *buf, size_t len)
{
    if (x->fd >= 0) {
        return nc_xbe_host_read(x->fd, off, buf, len);
    }
    return nc_disc_read(disc, &x->file, off, buf, len);
}

bool nc_xbe_load(NcVm *vm, const NcDisc *disc, const char *path, const NcKernel *k, NcXbe *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    static const char cd[] = "\\Device\\CdRom0";
    char host[512];
    struct stat st;
    if (nc_io_host_file(path, host, sizeof(host))) {
        snprintf(g_host, sizeof(g_host), "%s", host);
        out->fd = open(host, O_RDONLY | O_CLOEXEC);
        if (out->fd < 0 || fstat(out->fd, &st) != 0 || st.st_size > UINT32_MAX) {
            nc_log("hakuX-native", "xbe: %s (%s) can't be opened", path, host);
            return false;
        }
        out->file.size = (uint32_t)st.st_size;
    } else if (!nc_disc_find(disc, !strncasecmp(path, cd, sizeof(cd) - 1) ? path + sizeof(cd) - 1 : path,
                             &out->file)) {
        nc_log("hakuX-native", "xbe: %s not on the disc", path);
        return false;
    }

    uint8_t probe[0x180];
    if (out->file.size < sizeof(probe) || !xbe_read(disc, out, 0, probe, sizeof(probe))) {
        return false;
    }
    const uint32_t base = nc_rd32(probe + 0x104);
    const uint32_t hdr_size = nc_rd32(probe + 0x108);
    const uint32_t image_size = nc_rd32(probe + 0x10C);
    if (hdr_size < sizeof(probe) || hdr_size > out->file.size || base & (NC_PAGE - 1)) {
        nc_log("hakuX-native", "xbe: bad header (base %08x, headers %u)", base, hdr_size);
        return false;
    }

    const uint32_t span = nc_align_up(image_size > hdr_size ? image_size : hdr_size, NC_PAGE);
    if (!nc_xbe_back(vm, base, hdr_size)) {
        return false;
    }
    uint8_t *hdr = nc_vm_host(vm, base);
    if (!xbe_read(disc, out, 0, hdr, hdr_size)) {
        return false;
    }
    int rc = hle_xbe_parse(hdr, hdr_size, &out->info);
    if (rc != HLE_XBE_OK) {
        nc_log("hakuX-native", "xbe: parse failed (%d)", rc);
        return false;
    }
    const HleXbeInfo *x = &out->info;
    out->tls_dir = nc_rd32(hdr + XBE_TLS_DIR);
    out->stack_commit = nc_rd32(hdr + XBE_STACK_COMMIT);

    unsigned loaded = 0;
    for (unsigned i = 0; i < x->nsections; i++) {
        const HleXbeSection *s = &x->sections[i];
        if (!(s->flags & HLE_XBE_SEC_PRELOAD)) {
            continue; /* XeLoadSection brings it in */
        }
        if ((uint64_t)s->va + s->vsize > (uint64_t)base + span || s->va < base) {
            nc_log("hakuX-native", "xbe: section %s outside the image", s->name);
            return false;
        }
        if (!nc_xbe_back(vm, s->va, s->vsize)) {
            nc_log("hakuX-native", "xbe: no memory for section %s", s->name);
            return false;
        }
        uint32_t n = s->raw_size < s->vsize ? s->raw_size : s->vsize;
        if (n && !xbe_read(disc, out, s->raw_off, nc_vm_host(vm, s->va), n)) {
            nc_log("hakuX-native", "xbe: read of section %s failed", s->name);
            return false;
        }
        /* Loaded once, as XeLoadSection counts: an XeUnloadSection frees it. */
        nc_vm_wr32(vm, nc_rd32(hdr + XBE_SECTIONS) + i * SEC_HDR_SIZE + SEC_REFS, 1);
        loaded++;
    }
    uint32_t backed = 0;
    for (uint32_t p = base; p < base + span; p += NC_PAGE) {
        backed += nc_vm_phys(vm, p) != UINT32_MAX ? NC_PAGE : 0;
    }

    /* Kernel thunk table: 0x80000000 | ordinal entries, zero-terminated. */
    for (uint32_t t = x->kernel_thunk;; t += 4) {
        uint32_t v = nc_vm_rd32(vm, t);
        if (!v) {
            break;
        }
        if (!(v & 0x80000000u)) {
            nc_log("hakuX-native", "xbe: thunk %08x is not an ordinal import (%08x)", t, v);
            return false;
        }
        unsigned ordinal = v & 0x7FFFFFFFu;
        uint32_t target = nc_kernel_resolve(k, ordinal);
        if (!target) {
            nc_log("hakuX-native", "xbe: import of unknown kernel ordinal %u", ordinal);
            return false;
        }
        nc_vm_wr32(vm, t, target);
        out->thunks++;
    }

    char imports[1024] = "", missing[1024] = "";
    size_t n = 0, nm = 0;
    unsigned nmissing = 0;
    for (uint32_t t = x->kernel_thunk; nc_vm_rd32(vm, t) && n < sizeof(imports) - 8; t += 4) {
        uint32_t target = nc_vm_rd32(vm, t);
        for (unsigned o = 1; o < NC_KORDINAL_MAX; o++) {
            if (nc_kernel_resolve(k, o) == target) {
                n += (size_t)snprintf(imports + n, sizeof(imports) - n, "%s%u", n ? "," : "", o);
                const NcKExport *e = nc_kernel_export(o);
                if (e->abi != NC_KABI_DATA && !nc_kernel_implemented(k, o)) {
                    nmissing++;
                    if (nm < sizeof(missing) - 40) {
                        nm += (size_t)snprintf(missing + nm, sizeof(missing) - nm, "%s%s", nm ? ", " : "", e->name);
                    }
                }
                break;
            }
        }
    }

    nc_log("hakuX-native-frontier",
           "stage S0 xbe loaded: title %08x base %08x image %u KiB (%u KiB in memory), %u/%u sections, "
           "entry %08x, %u kernel imports, tls %08x",
           x->title_id, base, span >> 10, backed >> 10, loaded, x->nsections, x->entry, out->thunks,
           out->tls_dir);
    nc_log("hakuX-native-imports", "title %08x %s", x->title_id, imports);
    if (nmissing) {
        /* told now, not at the call (that stops the game) */
        nc_log("hakuX-native-imports", "title %08x imports %u kernel functions not implemented here: %s",
               x->title_id, nmissing, missing);
    }
    return true;
}
