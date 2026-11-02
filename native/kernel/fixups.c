/*
 * hakuX native core — library code that waits on emulated hardware.
 *
 * DirectSound hands the APU's GP DSP a command through a mailbox in memory
 * and spins until the DSP takes it:
 *
 *     add eax, 0x800
 *     cmp dword [eax+0x10], 0     ; the previous command still pending
 *     jne $-6
 *
 * The emulated GP runs a slice of its program per audio frame unless a CPU
 * waits on it (mcpx_apu_spin_wait: frames then run to completion), so the
 * DSP reaches its command check only every ~40 ms, and the spinning thread
 * (PGR2's audio thread, above the main thread's priority) holds the CPU all
 * that time. QEMU's TCG recognises the spin and tells the APU; here the loop
 * jumps to a stub that asks the kernel (NC_KORDINAL_APU_SPIN) to hurry the
 * APU and wait for the mailbox, the CPU's thread blocked on the host rather
 * than spinning in translated code. The library is the same in every title
 * seen (one copy, in DSOUND). debug.hakux.apuspin=0 turns it off.
 */
#include "native/kernel/kernel.h"
#include "native/dev/dev.h"

#include <stdio.h>
#include <string.h>

#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"

#include "hw/xbox/hle/core/xbe.h"

static const uint8_t k_mailbox_wait[] = {0x05, 0x00, 0x08, 0x00, 0x00,  /* add eax, 0x800 */
                                         0x83, 0x78, 0x10, 0x00,        /* cmp dword [eax+0x10], 0 */
                                         0x75, 0xFA};                   /* jne the cmp */
#define LOOP_OFF 5 /* the cmp */

static bool apu_spin_on(void)
{
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.apuspin", v);
    return strcmp(v, "0") != 0;
}

/* The stub for the loop at va (its cmp; 6 bytes):
 *   L: cmp dword [eax+0x10], 0 ; je out ; int 3Fh APU_SPIN ; jmp L
 *   out: jmp va+6 */
static uint32_t mailbox_stub(NcKernel *k, uint32_t va)
{
    uint8_t b[32];
    unsigned n = 0;
    b[n++] = 0x83, b[n++] = 0x78, b[n++] = 0x10, b[n++] = 0x00;
    b[n++] = 0x74, b[n++] = 6; /* je out */
    b[n++] = 0xCD, b[n++] = NC_KCALL_VECTOR;
    b[n++] = NC_KORDINAL_APU_SPIN & 0xFF, b[n++] = NC_KORDINAL_APU_SPIN >> 8;
    b[n++] = 0xEB, b[n++] = (uint8_t)-12; /* jmp L (from offset 12) */
    const unsigned out = n;                /* 12: the je's target */
    const uint32_t stub = nc_kernel_code_alloc(k, out + 5);
    if (!stub) {
        return 0;
    }
    b[n++] = 0xE9;
    const uint32_t rel = (va + 6) - (stub + out + 5);
    memcpy(b + n, &rel, 4);
    n += 4;
    memcpy(nc_vm_host(k->vm, stub), b, n);
    return stub;
}

static bool section_mapped(const NcVm *vm, uint32_t va, uint32_t len)
{
    for (uint32_t p = va & ~(NC_PAGE - 1); p < va + len; p += NC_PAGE) {
        if (nc_vm_phys(vm, p) == UINT32_MAX) {
            return false;
        }
    }
    return true;
}

void nc_kernel_title_fixups(NcKernel *k, const HleXbeInfo *xbe)
{
    for (unsigned i = 0; i < xbe->nsections; i++) {
        const HleXbeSection *s = &xbe->sections[i];
        if (s->vsize && section_mapped(k->vm, s->va, s->vsize)) {
            nc_kernel_section_fixups(k, s->va, s->vsize, s->name);
        }
    }
    if (!apu_spin_on()) {
        nc_log("hakuX-native", "fixups: DirectSound mailbox waits left spinning (debug.hakux.apuspin=0)");
        return;
    }
    unsigned patched = 0;
    for (unsigned i = 0; i < xbe->nsections; i++) {
        const HleXbeSection *s = &xbe->sections[i];
        if (!s->vsize || !section_mapped(k->vm, s->va, s->vsize)) {
            continue; /* not loaded at boot */
        }
        const uint8_t *base = nc_vm_host(k->vm, s->va);
        const uint8_t *p = base, *end = base + s->vsize;
        while ((p = memmem(p, (size_t)(end - p), k_mailbox_wait, sizeof(k_mailbox_wait))) != NULL) {
            const uint32_t va = s->va + (uint32_t)(p - base) + LOOP_OFF;
            const uint32_t stub = mailbox_stub(k, va);
            if (stub) {
                uint8_t *site = nc_vm_host(k->vm, va);
                const uint32_t rel = stub - (va + 5);
                site[0] = 0xE9;
                memcpy(site + 1, &rel, 4);
                site[5] = 0x90;
                patched++;
                nc_log("hakuX-native", "fixups: DirectSound mailbox wait at %08x (%s) waits on the APU (stub %08x)",
                       va, s->name, stub);
            }
            p += sizeof(k_mailbox_wait);
        }
    }
    if (!patched) {
        nc_log("hakuX-native", "fixups: no DirectSound mailbox wait found");
    }
}

/* XMV's decoder close waits for each of its audio streams to stop before it
 * frees the decoder, where its packets' pdwCompletedSize and pdwStatus are:
 *
 *     call [ecx+0Ch]               ; IDirectSoundStream_GetStatus
 *     test byte [ebp+0Ah], 1       ; DSSTREAMSTATUS_PLAYING
 *     je next
 *     push 1
 *     call Sleep
 *     call DirectSoundDoWork       ; the flushed packets complete here
 *     dec edi                      ; the same stream again
 *   next:
 *     inc edi
 *
 * It counts on the stream still playing at its first look: the console's APU
 * releases a voice turned off at its next frame, 0.67 ms on, and the CPU gets
 * from the flush to here in microseconds. Here the guest can take
 * milliseconds (the code that skips a movie compiled as it first runs), the
 * release's DPC came first, GetStatus said stopped, DirectSoundDoWork never
 * ran and the decoder was freed with the flushed packets still to complete;
 * their completion in the stream's last Release wrote into the freed heap
 * (Soldier of Fortune II, an intro movie skipped). The je goes to a stub that
 * runs DirectSoundDoWork before the next stream: a stopped stream's flushed
 * packets complete there, as when the first look saw it playing. Patched as
 * the code loads: at boot and in XeLoadSection (XMV is a section of its own
 * in some titles, loaded for a movie). */
static const uint8_t k_xmv_wait[] = {0xFF, 0x51, 0x0C,              /* call [ecx+0Ch] */
                                     0xF6, 0x45, 0x0A, 0x01,        /* test byte [ebp+0Ah], 1 */
                                     0x74, 0x0D,                    /* je next */
                                     0x6A, 0x01,                    /* push 1 */
                                     0xE8};                         /* call Sleep */
#define XMV_JE      7   /* the je, patched with a jmp to the stub */
#define XMV_SLEEP   11  /* call Sleep (rel32) */
#define XMV_DOWORK  16  /* call DirectSoundDoWork (rel32) */
#define XMV_DEC     21  /* dec edi: the same stream again */
#define XMV_NEXT    22  /* inc edi: the next stream */

#define XMV_STUB    (2 + 2 + 5 + 5 + 5 + 5 + 5)

/* The stubs made, by the je's VA: a section loaded again reuses its own,
 * when its bytes are still the ones it would get (a savestate's load brings
 * the kernel area of the run that saved it). */
static struct {
    uint32_t site, stub;
} g_xmv_stubs[8];

static uint32_t rel_target(NcKernel *k, uint32_t call)
{
    return call + 5 + nc_vm_rd32(k->vm, call + 1);
}

static void put_rel(uint8_t *b, unsigned *n, uint8_t op, uint32_t from, uint32_t to)
{
    b[(*n)++] = op;
    const uint32_t rel = to - (from + *n + 4);
    memcpy(b + *n, &rel, 4);
    *n += 4;
}

/* The stub for the loop at m (the call [ecx+0Ch]) at stub; the flags of the test still set:
 *   je np ; push 1 ; call Sleep ; call DirectSoundDoWork ; jmp m+21 (dec edi)
 *   np: call DirectSoundDoWork ; jmp m+22 (inc edi) */
static void xmv_stub_bytes(NcKernel *k, uint32_t m, uint32_t stub, uint8_t b[XMV_STUB])
{
    const uint32_t sleep = rel_target(k, m + XMV_SLEEP), dowork = rel_target(k, m + XMV_DOWORK);
    unsigned n = 0;
    b[n++] = 0x74, b[n++] = 2 + 5 + 5 + 5; /* je np */
    b[n++] = 0x6A, b[n++] = 0x01;          /* push 1 */
    put_rel(b, &n, 0xE8, stub, sleep);
    put_rel(b, &n, 0xE8, stub, dowork);
    put_rel(b, &n, 0xE9, stub, m + XMV_DEC);
    put_rel(b, &n, 0xE8, stub, dowork);    /* np: */
    put_rel(b, &n, 0xE9, stub, m + XMV_NEXT);
}

static uint32_t xmv_stub(NcKernel *k, uint32_t m)
{
    const uint32_t site = m + XMV_JE;
    const unsigned slots = sizeof(g_xmv_stubs) / sizeof(g_xmv_stubs[0]);
    uint8_t b[XMV_STUB];
    unsigned slot = slots;
    for (unsigned i = 0; i < slots && slot == slots; i++) {
        if (g_xmv_stubs[i].site == site) {
            slot = i;
            xmv_stub_bytes(k, m, g_xmv_stubs[i].stub, b);
            if (!memcmp(nc_vm_host(k->vm, g_xmv_stubs[i].stub), b, XMV_STUB)) {
                return g_xmv_stubs[i].stub;
            }
        } else if (!g_xmv_stubs[i].site) {
            slot = i;
        }
    }
    const uint32_t stub = nc_kernel_code_alloc(k, XMV_STUB);
    if (!stub) {
        nc_log("hakuX-native", "fixups: XMV's stream wait at %08x left as it is (no room for its stub)", m);
        return 0;
    }
    xmv_stub_bytes(k, m, stub, b);
    memcpy(nc_vm_host(k->vm, stub), b, XMV_STUB);
    if (slot < slots) {
        g_xmv_stubs[slot].site = site;
        g_xmv_stubs[slot].stub = stub;
    }
    return stub;
}

static bool xmv_wait_on(void)
{
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.xmvwait", v);
    return strcmp(v, "0") != 0;
}

/* Crimson Skies (the disc and both title updates) copies a 512x512 DXT1
 * image into its texture with the image's 8-byte header {1, size} in front:
 *
 *     mov ecx, 0x8002              ; 0x20008 bytes, the texture holds 0x20000
 *     mov esi, <header>
 *     push ebp
 *     rep movsd
 *
 * The last 8 bytes land past the texture. On the console what follows takes
 * them; here, in the boot the title restarts into after the profile is
 * picked, the USB driver's HCCA did: interrupt table slot 1 became a pointer
 * to nowhere, the OHCI walked it and retired a TD the driver never queued,
 * and its done-queue handler crashed (0x290B08). The copy stops at the
 * texture's end; what it holds is as before. Only where the header's size
 * shows the copy 8 bytes too long. */
static const uint8_t k_hdr_copy[] = {0xB9, 0x02, 0x80, 0x00, 0x00, 0xBE}; /* mov ecx, 0x8002 ; mov esi, imm32 */
#define HDR_COPY_LEN (sizeof(k_hdr_copy) + 4 + 3) /* ... push ebp ; rep movsd */

static unsigned overrun_copy_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    unsigned patched = 0;
    const uint8_t *base = nc_vm_host(k->vm, va), *end = base + len, *p = base;
    while ((p = memmem(p, (size_t)(end - p), k_hdr_copy, sizeof(k_hdr_copy))) != NULL) {
        const uint32_t m = va + (uint32_t)(p - base);
        if (end - p >= (ptrdiff_t)HDR_COPY_LEN && p[10] == 0x55 && p[11] == 0xF3 && p[12] == 0xA5) {
            uint32_t src;
            memcpy(&src, p + sizeof(k_hdr_copy), 4);
            if (nc_vm_phys(k->vm, src) != UINT32_MAX && nc_vm_phys(k->vm, src + 7) != UINT32_MAX &&
                nc_vm_rd32(k->vm, src) == 1 && nc_vm_rd32(k->vm, src + 4) == 0x20000) {
                ((uint8_t *)nc_vm_host(k->vm, m))[1] = 0x00; /* mov ecx, 0x8000 */
                patched++;
                nc_log("hakuX-native", "fixups: the image copy at %08x (%s) stops at its texture's end "
                       "(8 bytes past it were the USB controller's)", m, name);
            }
        }
        p += sizeof(k_hdr_copy);
    }
    return patched;
}

/* The fixups in [va, va + len) (the overrunning image copy, XMV's stream
 * waits): how many were patched. */
static unsigned section_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    unsigned patched = overrun_copy_fixups(k, va, len, name);
    if (!xmv_wait_on()) {
        return patched;                 /* debug.hakux.xmvwait=0: XMV as it is (A/B) */
    }
    const uint8_t *base = nc_vm_host(k->vm, va), *end = base + len, *p = base;
    while ((p = memmem(p, (size_t)(end - p), k_xmv_wait, sizeof(k_xmv_wait))) != NULL) {
        const uint32_t m = va + (uint32_t)(p - base);
        if (end - p > XMV_NEXT && p[XMV_DOWORK] == 0xE8 && p[XMV_DEC] == 0x4F && p[XMV_NEXT] == 0x47) {
            const uint32_t stub = xmv_stub(k, m);
            if (stub) {
                uint8_t *site = nc_vm_host(k->vm, m + XMV_JE);
                const uint32_t rel = stub - (m + XMV_JE + 5);
                site[0] = 0xE9;
                memcpy(site + 1, &rel, 4);
                memset(site + 5, 0x90, XMV_DEC - XMV_JE - 5);
                patched++;
                nc_log("hakuX-native", "fixups: XMV's stream wait at %08x (%s) runs DirectSoundDoWork for a "
                       "stopped stream too (stub %08x)", m, name, stub);
            }
        }
        p += sizeof(k_xmv_wait);
    }
    return patched;
}

void nc_kernel_section_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name)
{
    section_fixups(k, va, len, name);
}

/* After a savestate's load: the sections loaded then, as the run that saved
 * them had them (a build before a fixup, unpatched). */
void nc_kernel_loaded_fixups(NcKernel *k)
{
    const uint32_t base = k->xbe_base;
    const uint32_t nsec = nc_vm_rd32(k->vm, base + 0x11C), secs = nc_vm_rd32(k->vm, base + 0x120);
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        const uint32_t s = secs + i * 0x38;
        const uint32_t va = nc_vm_rd32(k->vm, s + 0x04), vsize = nc_vm_rd32(k->vm, s + 0x08);
        if (!nc_vm_rd32(k->vm, s + 0x18) || !vsize || !section_mapped(k->vm, va, vsize)) {
            continue;                   /* not loaded */
        }
        const uint32_t name = nc_vm_rd32(k->vm, s + 0x14);
        char nm[9] = "?";
        if (name && nc_vm_phys(k->vm, name) != UINT32_MAX) {
            snprintf(nm, sizeof(nm), "%s", (const char *)nc_vm_host(k->vm, name));
        }
        if (section_fixups(k, va, vsize, nm) && k->cpu) {
            nc_cpu_invalidate(k->cpu, va, vsize);
        }
    }
}

void nc_kernel_apu_spin(NcKernel *k, NcFrame *f)
{
    const uint32_t word = nc_frame_gpr(f, NC_EAX) + 0x10;
    if (nc_vm_phys(k->vm, word) != UINT32_MAX) {
        nc_dev_apu_spin_wait(nc_vm_host(k->vm, word));
    }
    k->apu_spins++;
    nc_frame_set_eip(f, nc_frame_eip(f) + 4); /* past the int: the stub checks again */
}
