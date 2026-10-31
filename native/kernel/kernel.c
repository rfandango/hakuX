/*
 * hakuX native core — kernel area, kernel calls and system ops.
 */
#include "native/kernel/kernel.h"
#include "native/kernel/xc_crypto.h"
#include "native/state/save.h"
#include "native/dev/dev.h"
#include "native/core/mmio.h"
#include "native/core/mods.h"
#include "native/core/irq.h"
#include "hw/xbox/hle/hitch.h"

#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif

static const NcKExport k_exports[] = {
#define NC_KEXPORT(o, n, a, b) {o, n, a, b},
#include "native/kernel/kexports_table.h"
#undef NC_KEXPORT
};

static const NcKExport *k_by_ordinal[NC_KORDINAL_MAX];

/* Kernel area layout (offsets from NC_KERNEL_VA). */
#define K_STUBS    0x0F00u  /* stop stub, thread-return stub */
#define K_TRAMPS   0x1000u  /* 8 bytes per ordinal */
#define K_DATA     0x2000u  /* 0x40 bytes per data export */
#define K_DATA_SLOT 0x40u
#define K_DATA_BIG 0xA000u  /* data exports larger than a slot, past the last slot */
#define K_KPCR     0x10000u
#define K_IDT      0x11000u /* 256 gates of 8 bytes */
#define K_FAST     0x12000u /* exports run as guest code (install_fast_paths) */
#define K_FAST_END 0x13000u

/* Kernel pool: starts with one 64 KiB chunk of physical RAM, reached through
 * the contiguous window, and grows a chunk at a time as needed (pool.c), as
 * the Xbox kernel's takes pages as it needs them: what it holds unused is
 * the title's memory on the console. A fixed 4 MiB left titles that size
 * their caches by free memory (Forza) 3 MiB short; 512 KiB, ~35 KiB of it
 * used, left Amped 2 188 KiB short of the 1.4 MiB block its first level's
 * load asks for (its heap took the rest at boot): the call failed and it
 * wrote through the null pointer. The first chunk sits just above the
 * kernel, where the Xbox kernel's own memory ends: titles lay out physical
 * memory at fixed addresses from there (Halo, from 0x61000) and below the
 * top MiB (Burnout 2, 0x2377000-0x3F00000 in 73 blocks at start), and a
 * chunk 512 KiB under the top reserve stood in Burnout 2's 6.25 MiB block:
 * the call failed and its traffic data, pointers baked for those
 * addresses, faulted at the first race's load. The pool's later chunks
 * still come from the top (nc_vm_alloc_phys_high): one taken before a title
 * lays out its memory there would stand in it the same way. */
#define POOL_SIZE  0x00010000u
#define POOL_BASE  (NC_KERNEL_PA + NC_KERNEL_SIZE)
_Static_assert(POOL_BASE + POOL_SIZE <= 0x61000, "the pool's first range under Halo's fixed block");

/* A savestate is loading: the pool's first range where the boot that made
 * it put it (the 64 KiB under its process object, whose address
 * nc_kern_state_load checks): states saved before KERN v3, and the ones
 * saved since by a run that loaded one, have it 512 KiB under the top
 * reserve. 0: POOL_BASE. */
static uint32_t g_pool_at;

void nc_kernel_pool_for(uint32_t kprocess)
{
    g_pool_at = kprocess >= NC_CONTIG_WINDOW ? (kprocess - NC_CONTIG_WINDOW) & ~(POOL_SIZE - 1) : 0;
}

const NcKExport *nc_kernel_export(unsigned ordinal)
{
    return ordinal < NC_KORDINAL_MAX ? k_by_ordinal[ordinal] : NULL;
}

uint32_t nc_kernel_resolve(const NcKernel *k, unsigned ordinal)
{
    const NcKExport *e = nc_kernel_export(ordinal);
    if (!e) {
        return 0;
    }
    return e->abi == NC_KABI_DATA ? k->data_va[ordinal] : k->tramp_va[ordinal];
}

bool nc_kernel_implemented(const NcKernel *k, unsigned ordinal)
{
    const NcKExport *e = nc_kernel_export(ordinal);
    return e && e->abi != NC_KABI_DATA && k->impl[ordinal];
}

static const NcKExport *export_named(const char *name)
{
    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        if (strcmp(k_exports[i].name, name) == 0) {
            return &k_exports[i];
        }
    }
    return NULL;
}

static uint8_t *data_ptr(NcKernel *k, const char *name)
{
    const NcKExport *e = export_named(name);
    return e && e->abi == NC_KABI_DATA ? nc_vm_host(k->vm, k->data_va[e->ordinal]) : NULL;
}

void nc_kernel_impl(NcKernel *k, const char *name, NcKFn fn)
{
    const NcKExport *e = export_named(name);
    if (!e || e->abi == NC_KABI_DATA) {
        nc_log("hakuX-native", "kernel: no function export named %s", name);
        abort();
    }
    k->impl[e->ordinal] = fn;
}

/* Exports titles call hundreds of thousands of times a second (Fable takes
 * ~450k critical sections a second, Forza ~13k) run as x86 code in the
 * kernel area, as on the Xbox, and enter the kernel only for what needs it:
 * the uncontended and recursive cases of RtlEnterCriticalSection and
 * RtlLeaveCriticalSection stay in guest code; a contended one undoes its
 * step and makes the kernel call (int 0x3F with the ordinal), which does
 * the whole operation. RTL_CRITICAL_SECTION: event 0, LockCount 0x10,
 * RecursionCount 0x14, OwningThread 0x18; fs:[0x28] is the current thread. */
static void install_fast_paths(NcKernel *k, uint8_t *km)
{
    static const uint8_t enter[] = {
        0x8B, 0x4C, 0x24, 0x04,                   /* mov ecx, [esp+4]           */
        0x64, 0x8B, 0x15, 0x28, 0x00, 0x00, 0x00, /* mov edx, fs:[0x28]         */
        0xF0, 0xFF, 0x41, 0x10,                   /* lock inc dword [ecx+0x10]  */
        0x75, 0x0D,                               /* jnz held                   */
        0x89, 0x51, 0x18,                         /* mov [ecx+0x18], edx        */
        0xC7, 0x41, 0x14, 0x01, 0x00, 0x00, 0x00, /* mov dword [ecx+0x14], 1    */
        0xC2, 0x04, 0x00,                         /* ret 4                      */
        0x39, 0x51, 0x18,                         /* held: cmp [ecx+0x18], edx  */
        0x75, 0x06,                               /* jne other                  */
        0xFF, 0x41, 0x14,                         /* inc dword [ecx+0x14]       */
        0xC2, 0x04, 0x00,                         /* ret 4                      */
        0xF0, 0xFF, 0x49, 0x10,                   /* other: lock dec [ecx+0x10] */
    };
    static const uint8_t leave[] = {
        0x8B, 0x4C, 0x24, 0x04,                   /* mov ecx, [esp+4]           */
        0xFF, 0x49, 0x14,                         /* dec dword [ecx+0x14]       */
        0x75, 0x10,                               /* jnz still_owned            */
        0xC7, 0x41, 0x18, 0x00, 0x00, 0x00, 0x00, /* mov dword [ecx+0x18], 0    */
        0xF0, 0xFF, 0x49, 0x10,                   /* lock dec dword [ecx+0x10]  */
        0x7D, 0x0A,                               /* jge waiters                */
        0xC2, 0x04, 0x00,                         /* ret 4                      */
        0xF0, 0xFF, 0x49, 0x10,                   /* still_owned: lock dec ...  */
        0xC2, 0x04, 0x00,                         /* ret 4                      */
        0xF0, 0xFF, 0x41, 0x10,                   /* waiters: lock inc [ecx+0x10] */
        0x64, 0x8B, 0x15, 0x28, 0x00, 0x00, 0x00, /* mov edx, fs:[0x28]         */
        0x89, 0x51, 0x18,                         /* mov [ecx+0x18], edx        */
        0xC7, 0x41, 0x14, 0x01, 0x00, 0x00, 0x00, /* mov dword [ecx+0x14], 1    */
    };
    /* Whole in guest code (Panzer Dragoon Orta's loader lowercases its file
     * names a character a call, two million calls a second). */
    static const uint8_t lower[] = {
        0x0F, 0xB6, 0x44, 0x24, 0x04,             /* movzx eax, byte [esp+4]    */
        0x8D, 0x48, 0xBF,                         /* lea ecx, [eax-'A']         */
        0x83, 0xF9, 0x19,                         /* cmp ecx, 25                */
        0x77, 0x03,                               /* ja done                    */
        0x83, 0xC0, 0x20,                         /* add eax, 0x20              */
        0xC2, 0x04, 0x00,                         /* done: ret 4                */
    };
    static const uint8_t upper[] = {
        0x0F, 0xB6, 0x44, 0x24, 0x04,             /* movzx eax, byte [esp+4]    */
        0x8D, 0x48, 0x9F,                         /* lea ecx, [eax-'a']         */
        0x83, 0xF9, 0x19,                         /* cmp ecx, 25                */
        0x77, 0x03,                               /* ja done                    */
        0x83, 0xE8, 0x20,                         /* sub eax, 0x20              */
        0xC2, 0x04, 0x00,                         /* done: ret 4                */
    };
    /* The IRQL is the KPCR's byte at fs:[0x24], which the kernel reads there
     * too: raising it and reading it need no kernel (a guest spun on
     * KfRaiseIrql ~50k times a second, each a kernel entry). Lowering enters
     * the kernel only for what that may let run: queued DPCs (lowering below
     * DISPATCH_LEVEL with fs:[NC_KPCR_DPC_PENDING] set) or interrupts held
     * above DISPATCH_LEVEL (lowering from there). KIRQL in CL (fastcall). */
    static const uint8_t lower_irql[] = {
        0x64, 0x80, 0x3D, 0x24, 0x00, 0x00, 0x00, 0x02, /* cmp byte fs:[0x24], 2 */
        0x77, 0x17,                               /* ja kernel                  */
        0x80, 0xF9, 0x02,                         /* cmp cl, 2                  */
        0x73, 0x0A,                               /* jae set                    */
        0x64, 0x80, 0x3D, (uint8_t)NC_KPCR_DPC_PENDING, (uint8_t)(NC_KPCR_DPC_PENDING >> 8),
        0x00, 0x00, 0x00,                         /* cmp byte fs:[DPC_PENDING], 0 */
        0x75, 0x08,                               /* jne kernel                 */
        0x64, 0x88, 0x0D, 0x24, 0x00, 0x00, 0x00, /* set: mov fs:[0x24], cl     */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t raise_irql[] = {
        0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00, /* movzx eax, byte fs:[0x24] */
        0x64, 0x88, 0x0D, 0x24, 0x00, 0x00, 0x00, /* mov fs:[0x24], cl          */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t raise_dpc[] = {
        0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00, /* movzx eax, byte fs:[0x24] */
        0x64, 0xC6, 0x05, 0x24, 0x00, 0x00, 0x00, 0x02, /* mov byte fs:[0x24], 2 */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t raise_synch[] = {
        0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00, /* movzx eax, byte fs:[0x24] */
        0x64, 0xC6, 0x05, 0x24, 0x00, 0x00, 0x00, 0x1C, /* mov byte fs:[0x24], 28 */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t get_irql[] = {
        0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00, /* movzx eax, byte fs:[0x24] */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t get_thread[] = {
        0x64, 0xA1, 0x28, 0x00, 0x00, 0x00,       /* mov eax, fs:[0x28]         */
        0xC3,                                     /* ret                        */
    };
    /* The Interlocked family (fastcall) and the memory routines: whole in
     * guest code, as the Xbox kernel's are (Insignia's setup stopped at its
     * first InterlockedExchange). */
    static const uint8_t inc32[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,             /* mov eax, 1                 */
        0xF0, 0x0F, 0xC1, 0x01,                   /* lock xadd [ecx], eax       */
        0x40,                                     /* inc eax                    */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t dec32[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF,             /* mov eax, -1                */
        0xF0, 0x0F, 0xC1, 0x01,                   /* lock xadd [ecx], eax       */
        0x48,                                     /* dec eax                    */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t xchg32[] = {
        0x89, 0xD0,                               /* mov eax, edx               */
        0x87, 0x01,                               /* xchg [ecx], eax            */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t xadd32[] = {
        0x89, 0xD0,                               /* mov eax, edx               */
        0xF0, 0x0F, 0xC1, 0x01,                   /* lock xadd [ecx], eax       */
        0xC3,                                     /* ret                        */
    };
    static const uint8_t cmpxchg32[] = {
        0x8B, 0x44, 0x24, 0x04,                   /* mov eax, [esp+4]           */
        0xF0, 0x0F, 0xB1, 0x11,                   /* lock cmpxchg [ecx], edx    */
        0xC2, 0x04, 0x00,                         /* ret 4                      */
    };
    static const uint8_t zero_mem[] = {
        0x57,                                     /* push edi                   */
        0x8B, 0x7C, 0x24, 0x08,                   /* mov edi, [esp+8]           */
        0x8B, 0x4C, 0x24, 0x0C,                   /* mov ecx, [esp+12]          */
        0x31, 0xC0,                               /* xor eax, eax               */
        0x89, 0xCA,                               /* mov edx, ecx               */
        0xC1, 0xE9, 0x02,                         /* shr ecx, 2                 */
        0xF3, 0xAB,                               /* rep stosd                  */
        0x89, 0xD1,                               /* mov ecx, edx               */
        0x83, 0xE1, 0x03,                         /* and ecx, 3                 */
        0xF3, 0xAA,                               /* rep stosb                  */
        0x5F,                                     /* pop edi                    */
        0xC2, 0x08, 0x00,                         /* ret 8                      */
    };
    static const uint8_t fill_mem[] = {
        0x57,                                     /* push edi                   */
        0x8B, 0x7C, 0x24, 0x08,                   /* mov edi, [esp+8]           */
        0x8B, 0x4C, 0x24, 0x0C,                   /* mov ecx, [esp+12]          */
        0x0F, 0xB6, 0x44, 0x24, 0x10,             /* movzx eax, byte [esp+16]   */
        0x69, 0xC0, 0x01, 0x01, 0x01, 0x01,       /* imul eax, eax, 0x01010101  */
        0x89, 0xCA,                               /* mov edx, ecx               */
        0xC1, 0xE9, 0x02,                         /* shr ecx, 2                 */
        0xF3, 0xAB,                               /* rep stosd                  */
        0x89, 0xD1,                               /* mov ecx, edx               */
        0x83, 0xE1, 0x03,                         /* and ecx, 3                 */
        0xF3, 0xAA,                               /* rep stosb                  */
        0x5F,                                     /* pop edi                    */
        0xC2, 0x0C, 0x00,                         /* ret 12                     */
    };
    static const uint8_t fill_ulong[] = {
        0x57,                                     /* push edi                   */
        0x8B, 0x7C, 0x24, 0x08,                   /* mov edi, [esp+8]           */
        0x8B, 0x4C, 0x24, 0x0C,                   /* mov ecx, [esp+12]          */
        0x8B, 0x44, 0x24, 0x10,                   /* mov eax, [esp+16]          */
        0xC1, 0xE9, 0x02,                         /* shr ecx, 2                 */
        0xF3, 0xAB,                               /* rep stosd                  */
        0x5F,                                     /* pop edi                    */
        0xC2, 0x0C, 0x00,                         /* ret 12                     */
    };
    static const uint8_t move_mem[] = {
        0x56,                                     /* push esi                   */
        0x57,                                     /* push edi                   */
        0x8B, 0x7C, 0x24, 0x0C,                   /* mov edi, [esp+12]          */
        0x8B, 0x74, 0x24, 0x10,                   /* mov esi, [esp+16]          */
        0x8B, 0x4C, 0x24, 0x14,                   /* mov ecx, [esp+20]          */
        0x39, 0xF7,                               /* cmp edi, esi               */
        0x76, 0x15,                               /* jbe forward                */
        0x8D, 0x04, 0x0E,                         /* lea eax, [esi+ecx]         */
        0x39, 0xC7,                               /* cmp edi, eax               */
        0x73, 0x0E,                               /* jae forward                */
        0x8D, 0x74, 0x0E, 0xFF,                   /* lea esi, [esi+ecx-1]       */
        0x8D, 0x7C, 0x0F, 0xFF,                   /* lea edi, [edi+ecx-1]       */
        0xFD,                                     /* std (overlapping, upward)  */
        0xF3, 0xA4,                               /* rep movsb                  */
        0xFC,                                     /* cld                        */
        0xEB, 0x0E,                               /* jmp done                   */
        0x89, 0xCA,                               /* forward: mov edx, ecx      */
        0xC1, 0xE9, 0x02,                         /* shr ecx, 2                 */
        0xF3, 0xA5,                               /* rep movsd                  */
        0x89, 0xD1,                               /* mov ecx, edx               */
        0x83, 0xE1, 0x03,                         /* and ecx, 3                 */
        0xF3, 0xA4,                               /* rep movsb                  */
        0x5F,                                     /* done: pop edi              */
        0x5E,                                     /* pop esi                    */
        0xC2, 0x0C, 0x00,                         /* ret 12                     */
    };
    static const uint8_t cmp_mem[] = {
        0x56,                                     /* push esi                   */
        0x57,                                     /* push edi                   */
        0x8B, 0x74, 0x24, 0x0C,                   /* mov esi, [esp+12]          */
        0x8B, 0x7C, 0x24, 0x10,                   /* mov edi, [esp+16]          */
        0x8B, 0x4C, 0x24, 0x14,                   /* mov ecx, [esp+20]          */
        0x89, 0xC8,                               /* mov eax, ecx               */
        0x85, 0xC9,                               /* test ecx, ecx              */
        0x74, 0x07,                               /* jz done                    */
        0xF3, 0xA6,                               /* repe cmpsb                 */
        0x74, 0x03,                               /* je done (all alike)        */
        0x29, 0xC8,                               /* sub eax, ecx               */
        0x48,                                     /* dec eax (before the one unlike) */
        0x5F,                                     /* done: pop edi              */
        0x5E,                                     /* pop esi                    */
        0xC2, 0x0C, 0x00,                         /* ret 12                     */
    };
    static const struct {
        const char *name;
        const uint8_t *code;
        size_t len;
    } fast[] = {{"RtlEnterCriticalSection", enter, sizeof(enter)},
                {"RtlLeaveCriticalSection", leave, sizeof(leave)},
                {"RtlLowerChar", lower, sizeof(lower)},
                {"RtlUpperChar", upper, sizeof(upper)},
                {"KfLowerIrql", lower_irql, sizeof(lower_irql)},
                {"KfRaiseIrql", raise_irql, sizeof(raise_irql)},
                {"KeRaiseIrqlToDpcLevel", raise_dpc, sizeof(raise_dpc)},
                {"KeRaiseIrqlToSynchLevel", raise_synch, sizeof(raise_synch)},
                {"KeGetCurrentIrql", get_irql, sizeof(get_irql)},
                {"KeGetCurrentThread", get_thread, sizeof(get_thread)},
                {"InterlockedIncrement", inc32, sizeof(inc32)},
                {"InterlockedDecrement", dec32, sizeof(dec32)},
                {"InterlockedExchange", xchg32, sizeof(xchg32)},
                {"InterlockedExchangeAdd", xadd32, sizeof(xadd32)},
                {"InterlockedCompareExchange", cmpxchg32, sizeof(cmpxchg32)},
                {"RtlZeroMemory", zero_mem, sizeof(zero_mem)},
                {"RtlFillMemory", fill_mem, sizeof(fill_mem)},
                {"RtlFillMemoryUlong", fill_ulong, sizeof(fill_ulong)},
                {"RtlMoveMemory", move_mem, sizeof(move_mem)},
                {"RtlCompareMemory", cmp_mem, sizeof(cmp_mem)}};
    k->code_next = NC_KERNEL_VA + K_FAST;
    if (getenv("NC_NO_FAST")) { /* dev switch: every call enters the kernel */
        return;
    }
    uint32_t off = K_FAST;
    for (size_t i = 0; i < sizeof(fast) / sizeof(fast[0]); i++) {
        const NcKExport *e = export_named(fast[i].name);
        if (!e) {
            continue;
        }
        uint8_t *p = km + off;
        memcpy(p, fast[i].code, fast[i].len);
        p[fast[i].len] = 0xCD; /* the kernel call for the rest */
        p[fast[i].len + 1] = NC_KCALL_VECTOR;
        p[fast[i].len + 2] = (uint8_t)e->ordinal;
        p[fast[i].len + 3] = (uint8_t)(e->ordinal >> 8);
        k->tramp_va[e->ordinal] = NC_KERNEL_VA + off;
        off += (uint32_t)(fast[i].len + 4 + 15) & ~15u;
    }
    k->code_next = NC_KERNEL_VA + off;
}

uint32_t nc_kernel_code_alloc(NcKernel *k, uint32_t len)
{
    const uint32_t va = (k->code_next + 15) & ~15u;
    if (va + len > NC_KERNEL_VA + K_FAST_END) {
        return 0;
    }
    k->code_next = va + len;
    return va;
}

bool nc_kernel_init(NcKernel *k, NcVm *vm)
{
    memset(k, 0, sizeof(*k));
    k->vm = vm;
    if (!nc_vm_claim_phys(vm, NC_KERNEL_PA, NC_KERNEL_SIZE)) {
        nc_log("hakuX-native", "kernel: physical 0x%x-0x%x taken", NC_KERNEL_PA,
               NC_KERNEL_PA + NC_KERNEL_SIZE);
        return false;
    }
    uint8_t *km = nc_vm_host(vm, NC_KERNEL_VA);
    memset(km, 0, NC_KERNEL_SIZE);
    memset(km + K_STUBS, 0xCC, K_DATA - K_STUBS);

    for (size_t i = 0; i < sizeof(k_exports) / sizeof(k_exports[0]); i++) {
        const NcKExport *e = &k_exports[i];
        k_by_ordinal[e->ordinal] = e;
        if (e->abi == NC_KABI_DATA) {
            k->data_va[e->ordinal] = NC_KERNEL_VA + K_DATA + e->ordinal * K_DATA_SLOT;
            /* XboxAlternateSignatureKeys is 16 keys of 16 bytes: in a slot it
             * ran into IdexChannelObject's queue (key 14 wasn't zero). In its
             * own area every key a title signs saves with is the zero
             * placeholder, which the hard disk screen re-signs imported saves
             * to. */
            if (strcmp(e->name, "XboxAlternateSignatureKeys") == 0) {
                k->data_va[e->ordinal] = NC_KERNEL_VA + K_DATA_BIG;
            }
            /* XePublicKeyData (0x11C bytes) goes right after those keys. */
            if (strcmp(e->name, "XePublicKeyData") == 0) {
                k->data_va[e->ordinal] = NC_KERNEL_VA + K_DATA_BIG + 0x100;
            }
        } else {
            uint32_t va = NC_KERNEL_VA + K_TRAMPS + e->ordinal * 8u;
            uint8_t *t = nc_vm_host(vm, va);
            t[0] = 0xCD;
            t[1] = NC_KCALL_VECTOR;
            t[2] = (uint8_t)e->ordinal;
            t[3] = (uint8_t)(e->ordinal >> 8);
            t[4] = 0xF4; /* never reached: the kernel returns to the caller */
            k->tramp_va[e->ordinal] = va;
        }
    }

    install_fast_paths(k, km);

    /* A PE header with an export directory below the stubs, as the real
     * kernel image has: code that looks up exports by ordinal in the image
     * (the HLE GPU layer, some titles) finds the trampolines and data. */
    nc_wr32(km + 0x00, 0x00905A4Du); /* "MZ" */
    nc_wr32(km + 0x3C, 0x80);        /* e_lfanew */
    nc_wr32(km + 0x80, 0x00004550u); /* "PE\0\0" */
    nc_wr32(km + 0x84, 0x0000014Cu); /* i386, no sections */
    nc_wr32(km + 0x94, 0x010200E0u); /* SizeOfOptionalHeader 0xE0, executable 32-bit */
    nc_wr32(km + 0x98, 0x0000010Bu); /* PE32 */
    nc_wr32(km + 0x98 + 28, NC_KERNEL_VA);
    nc_wr32(km + 0x98 + 92, 16);     /* NumberOfRvaAndSizes */
    nc_wr32(km + 0x98 + 96, 0x200);  /* export directory */
    nc_wr32(km + 0x98 + 100, 0x28);
    nc_wr32(km + 0x200 + 0x10, 1);                   /* ordinal base */
    nc_wr32(km + 0x200 + 0x14, NC_KORDINAL_MAX - 1); /* functions */
    nc_wr32(km + 0x200 + 0x1C, 0x300);               /* AddressOfFunctions */
    for (unsigned o = 1; o < NC_KORDINAL_MAX; o++) {
        const uint32_t va = k->tramp_va[o] ? k->tramp_va[o] : k->data_va[o];
        nc_wr32(km + 0x300 + 4 * (o - 1), va ? va - NC_KERNEL_VA : 0);
    }

    /* Stubs: HLT to leave the JIT, then the return addresses of routines the
     * kernel runs (int 0x3F with a pseudo-ordinal), 8 bytes apart. */
    k->stop_va = NC_KERNEL_VA + K_STUBS;
    km[K_STUBS] = 0xF4;
    static const uint16_t stub_ordinals[9] = {
        NC_KORDINAL_THREAD_RETURN, NC_KORDINAL_DPC_RETURN, NC_KORDINAL_CALLOUT_RETURN,
        NC_KORDINAL_ISR_RETURN,    NC_KORDINAL_APC_RETURN, NC_KORDINAL_TRAP,
        NC_KORDINAL_SEH_RETURN,    NC_KORDINAL_SEH_NESTED, NC_KORDINAL_SEH_COLLIDED};
    uint32_t *const stub_va[9] = {&k->thread_return_va, &k->dpc_return_va, &k->callout_return_va,
                                  &k->isr_return_va,    &k->apc_return_va, &k->trap_va,
                                  &k->seh_return_va,    &k->seh_nested_va, &k->seh_collided_va};
    for (unsigned i = 0; i < 9; i++) {
        *stub_va[i] = NC_KERNEL_VA + K_STUBS + 8 * (i + 1);
        uint8_t *s = km + K_STUBS + 8 * (i + 1);
        s[0] = 0xCD;
        s[1] = NC_KCALL_VECTOR;
        s[2] = stub_ordinals[i] & 0xff;
        s[3] = stub_ordinals[i] >> 8;
    }

    /* Processor control region (fs:). KPCR: NT_TIB at 0 (ExceptionList,
     * StackBase, StackLimit, ...), SelfPcr 0x1C, Prcb 0x20, Irql 0x24, PRCB at
     * 0x28 starting with CurrentThread (set by the scheduler). */
    k->kpcr_va = NC_KERNEL_VA + K_KPCR;
    uint8_t *pcr = km + K_KPCR;
    nc_wr32(pcr + 0x00, 0xFFFFFFFFu);
    nc_wr32(pcr + 0x1C, k->kpcr_va);
    nc_wr32(pcr + 0x20, k->kpcr_va + 0x28);
    pcr[0x24] = 0; /* PASSIVE_LEVEL */

    /* The IDT: titles that hook a vector (Fable's pager takes #PF) read it
     * with SIDT and edit gates. Every gate is a 32-bit interrupt gate to the
     * trap stub, which reports an exception nothing handled. */
    k->idt_va = NC_KERNEL_VA + K_IDT;
    for (unsigned v = 0; v < 256; v++) {
        nc_wr32(km + K_IDT + 8 * v, (0x08u << 16) | (k->trap_va & 0xFFFFu));
        nc_wr32(km + K_IDT + 8 * v + 4, (k->trap_va & 0xFFFF0000u) | 0x8E00u);
    }

    /* The pool's first range just above the kernel (POOL_BASE), or for a
     * state, where its boot made it, so that the objects made at boot are
     * where the state's are. */
    const uint32_t pool_lo = g_pool_at ? g_pool_at : POOL_BASE;
    uint32_t pool_pa = nc_vm_alloc_phys_ex(vm, POOL_SIZE, pool_lo, pool_lo + POOL_SIZE, NC_PAGE, false);
    if (pool_pa == UINT32_MAX) {
        pool_pa = nc_vm_alloc_phys_ex(vm, POOL_SIZE, 0, vm->ram_size - NC_TOP_RESERVE, NC_PAGE, true);
    }
    if (pool_pa == UINT32_MAX) {
        return false;
    }
    nc_pool_init(k, NC_CONTIG_WINDOW + pool_pa, POOL_SIZE);

    static const char *const type_names[NC_OB_TYPES] = {
        [NC_OB_THREAD] = "PsThreadObjectType",       [NC_OB_EVENT] = "ExEventObjectType",
        [NC_OB_SEMAPHORE] = "ExSemaphoreObjectType", [NC_OB_MUTANT] = "ExMutantObjectType",
        [NC_OB_TIMER] = "ExTimerObjectType",         [NC_OB_FILE] = "IoFileObjectType",
        [NC_OB_DEVICE] = "IoDeviceObjectType",       [NC_OB_SYMLINK] = "ObSymbolicLinkObjectType",
        [NC_OB_DIRECTORY] = "ObDirectoryObjectType",
    };
    for (unsigned i = 0; i < NC_OB_TYPES; i++) {
        const NcKExport *e = export_named(type_names[i]);
        k->type_va[i] = e ? k->data_va[e->ordinal] : 0;
    }

    nc_ob_register(k);
    nc_ps_register(k);
    nc_ke_register(k);
    nc_mm_register(k);
    nc_ex_register(k);
    nc_rtl_register(k);
    nc_seh_register(k);
    nc_timer_register(k);
    nc_intr_register(k);
    nc_apc_register(k);
    nc_io_register(k);
    nc_xc_register(k);

    /* Data exports read at boot. */
    uint8_t *ver = data_ptr(k, "XboxKrnlVersion");
    if (ver) {
        const uint16_t v[4] = {1, 0, 5838, 1};
        memcpy(ver, v, sizeof(v));
    }
    uint8_t *pub = data_ptr(k, "XePublicKeyData");
    if (pub) {
        nc_xc_public_key(pub);
    }
    uint8_t *inc = data_ptr(k, "KeTimeIncrement");
    if (inc) {
        nc_wr32(inc, 10000); /* 1 ms in 100 ns units */
    }
    uint8_t *parts = data_ptr(k, "HalDiskCachePartitionCount");
    if (parts) {
        nc_wr32(parts, 3);
    }
    /* The IDE channel's device queue (IDE_CHANNEL_OBJECT +0x24), empty: XAPI's
     * CancelIo walks its list for the file's requests (Forza, after a race),
     * and a zero head took it through a NULL entry. KDEVICE_QUEUE: Type
     * DeviceQueueObject, Size 0xC, DeviceListHead at +4 pointing to itself.
     * No request in progress (CurrentIrp +0x20 stays 0). */
    const NcKExport *ide = export_named("IdexChannelObject");
    if (ide && ide->abi == NC_KABI_DATA) {
        const uint32_t q = k->data_va[ide->ordinal] + 0x24;
        uint8_t *p = nc_vm_host(vm, q);
        p[0] = 0x14;
        p[2] = 0x0C;
        nc_wr32(p + 4, q + 4);
        nc_wr32(p + 8, q + 4);
    }
    uint8_t *kdnp = data_ptr(k, "KdDebuggerNotPresent");
    if (kdnp) {
        kdnp[0] = 1;
    }
    return true;
}

void nc_kernel_request_stop(NcKernel *k, NcFrame *f, const char *fmt, ...)
{
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(k->stop_reason, sizeof(k->stop_reason), fmt, ap);
    va_end(ap);
    k->stop = true;
}

/* The last kernel calls, for fault reports. */
#define RECENT 16
static struct {
    uint16_t ordinal;
    uint32_t thread, ret, result;
    uint32_t args[5];
    bool running;
} g_recent[RECENT];
static unsigned g_nrecent;

/* Recorded before the call runs, so a fault inside it is reported with it. */
static long g_trace_left = -1;

void nc_kernel_trace_trigger(const char *what)
{
    static const char *at;
    static bool armed = true;
    if (armed && !at) {
        at = getenv("NC_TRACE_KCALL_AT");
        armed = at != NULL;
    }
    if (armed && strstr(what, at)) {
        armed = false;
        const char *t = getenv("NC_TRACE_KCALL");
        g_trace_left = t ? atol(t) : 2000;
        nc_log("hakuX-ktrace", "trace starts at %s", what);
    }
}

/* The guest's EBP frame chain at a kernel call: up to six return addresses. */
void nc_kernel_frame_chain(NcKernel *k, NcFrame *f, char *buf, size_t len)
{
    size_t n = 0;
    buf[0] = '\0';
    uint32_t bp = nc_frame_gpr(f, NC_EBP);
    for (unsigned d = 0; d < 6 && n + 10 < len; d++) {
        if (nc_vm_phys(k->vm, bp) == UINT32_MAX || nc_vm_phys(k->vm, bp + 4) == UINT32_MAX) {
            break;
        }
        n += (size_t)snprintf(buf + n, len - n, " %08x", nc_vm_rd32(k->vm, bp + 4));
        const uint32_t up = nc_vm_rd32(k->vm, bp);
        if (up <= bp) {
            break;
        }
        bp = up;
    }
}

/* Rates for the 5 s kernel statistics line (nc_kernel_stats_tick). */
static uint32_t g_kcount[NC_KORDINAL_MAX];
static uint64_t g_kcalls, g_sysops, g_sysop_kind[32];
static uint64_t g_clock_ticks; /* the clock's interrupts for the time alone (clock_thread) */
NcSchedStats g_nc_sched;

static void dump_int_samples(NcKernel *k);

/* debug: NC_PEEK="va:len,*va:len" logs guest memory with the stats line:
 * hakuX-native-peek, up to 256 bytes each (hex); *va reads the pointer at
 * va and dumps what it points to. */
static void peek(NcKernel *k)
{
    static const char *spec;
    static bool read;
    if (!read) {
        read = true;
        spec = getenv("NC_PEEK");
    }
    for (const char *s = spec; s && *s;) {
        const bool deref = *s == '*';
        char *end;
        uint32_t va = (uint32_t)strtoul(s + deref, &end, 16);
        uint32_t len = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 64;
        len = len > 256 ? 256 : len;
        if (deref) {
            va = nc_vm_phys(k->vm, va) != UINT32_MAX ? nc_vm_rd32(k->vm, va) : 0;
        }
        char hex[800];
        int n = 0;
        for (uint32_t i = 0; i < len && nc_vm_phys(k->vm, va + i) != UINT32_MAX; i++) {
            const uint8_t b = *(const uint8_t *)nc_vm_host(k->vm, va + i);
            n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02x%s", b, (i & 3) == 3 ? " " : "");
        }
        hex[n] = 0;
        const uint32_t pa = nc_vm_phys(k->vm, va);
        nc_log("hakuX-native-peek", "%s%08x (pa %08x, rmap %08x): %s", deref ? "*" : "", va, pa,
               pa != UINT32_MAX ? k->vm->rmap[pa / NC_PAGE] : 0, hex);
        s = *end == ',' ? end + 1 : end;
        if (*s && *s != '*' && !isxdigit((unsigned char)*s)) {
            break;
        }
    }
}

static void stats_tick(NcKernel *k)
{
    static uint64_t last_ns;
    const uint64_t now = nc_ke_now_ns();
    if (!last_ns) {
        last_ns = now;
        return;
    }
    if (now - last_ns < 5000000000ull) {
        return;
    }
    const double s = (double)(now - last_ns) / 1e9;
    last_ns = now;
    peek(k);
    char top[400], isr[256];
    int n = 0, m = 0;
    top[0] = isr[0] = 0;
    for (unsigned l = 0; l < 16; l++) {
        const uint64_t lat = nc_irq_latency_max(l, true);
        if (g_nc_sched.isr[l] && m < (int)sizeof(isr)) {
            /* per second, and the longest from the line rising to its ISR */
            m += snprintf(isr + m, sizeof(isr) - (size_t)m, " irq%u:%.0f (max %.1f ms)", l,
                          g_nc_sched.isr[l] / s, lat / 1e6);
        }
    }
    for (int r = 0; r < 6; r++) {
        unsigned best = 0;
        for (unsigned o = 1; o < NC_KORDINAL_MAX; o++) {
            if (g_kcount[o] > g_kcount[best]) {
                best = o;
            }
        }
        if (!g_kcount[best]) {
            break;
        }
        const NcKExport *e = nc_kernel_export(best);
        n += snprintf(top + n, sizeof(top) - (size_t)n, " %s:%.0f", e ? e->name : "?", g_kcount[best] / s);
        g_kcount[best] = 0;
    }
    static uint64_t faults_seen, mmio_seen, apu_seen, commits_seen, decommits_seen, ticks_seen;
    const uint64_t ticks = __atomic_load_n(&g_clock_ticks, __ATOMIC_RELAXED);
    const uint64_t mmio = nc_mmio_total();
    uint64_t held_out_ns, held_max_ns;
    const unsigned holds = nc_ke_held(&held_out_ns, &held_max_ns);  /* in the window, not per s */
    nc_log("hakuX-native-stats",
           "per s: kcalls %.0f sysops %.0f (int %.0f, of them ticks %.0f; hook %.0f) schedules %.0f switches %.0f idle %.0f "
           "slices %.0f page faults %.0f mmio %.0f apu waits %.0f vm pages +%.0f -%.0f "
           "isr%s | dpc max wait %.1f run %.1f ms, %llu over 8 ms | holds %u (longest %.1f ms), %.1f ms left out |%s",
           g_kcalls / s, g_sysops / s, g_sysop_kind[NC_SYSOP_INTERRUPT] / s, (double)(ticks - ticks_seen) / s,
           g_sysop_kind[NC_SYSOP_HOOK] / s,
           g_nc_sched.schedules / s, g_nc_sched.switches / s, g_nc_sched.idles / s, g_nc_sched.slices / s,
           (double)(k->page_faults - faults_seen) / s, (double)(mmio - mmio_seen) / s,
           (double)(k->apu_spins - apu_seen) / s,
           (double)(k->vm_commits - commits_seen) / s, (double)(k->vm_decommits - decommits_seen) / s, isr,
           g_nc_sched.dpc_wait_max_ns / 1e6, g_nc_sched.dpc_run_max_ns / 1e6,
           (unsigned long long)g_nc_sched.dpc_slow, holds, held_max_ns / 1e6, held_out_ns / 1e6, top);
    {
        static uint64_t cr, di, mm, ua;
        char ms[240];
        uint32_t fe[6], fn[6];
        const unsigned nf = nc_mmio_fault_top(fe, fn, 6);
        int o = 0;
        ms[0] = 0;
        for (unsigned i = 0; i < nf && o < (int)sizeof(ms) - 32; i++) {
            o += snprintf(ms + o, sizeof(ms) - (size_t)o, " %08x:%u(%d)", fe[i], fn[i],
                          nc_x86mmio_site_state(fe[i]));
        }
        char ds[240];
        uint32_t dp[6], dn[6];
        const unsigned nd = nc_hle_dirty_top(dp, dn, 6);
        int od = 0;
        ds[0] = 0;
        for (unsigned i = 0; i < nd && od < (int)sizeof(ds) - 24; i++) {
            od += snprintf(ds + od, sizeof(ds) - (size_t)od, " %05x000:%u", dp[i], dn[i]);
        }
        const uint64_t ncr = __atomic_load_n(&g_nc_faults_callret, __ATOMIC_RELAXED),
                       ndi = __atomic_load_n(&g_nc_faults_dirty, __ATOMIC_RELAXED),
                       nmm = __atomic_load_n(&g_nc_faults_mmio, __ATOMIC_RELAXED),
                       nua = __atomic_load_n(&g_nc_faults_unaligned, __ATOMIC_RELAXED);
        nc_log("hakuX-native-stats", "host faults per s: call-return %.0f dirty pages %.0f (pages%s) mmio %.0f "
               "(eips%s) unaligned atomics %.0f", (double)(ncr - cr) / s, (double)(ndi - di) / s, ds,
               (double)(nmm - mm) / s, ms, (double)(nua - ua) / s);
        cr = ncr;
        di = ndi;
        mm = nmm;
        ua = nua;
    }
    commits_seen = k->vm_commits;
    decommits_seen = k->vm_decommits;
    apu_seen = k->apu_spins;
    faults_seen = k->page_faults;
    mmio_seen = mmio;
    ticks_seen = ticks;
    uint32_t contig, virt, pool_free = 0;
    unsigned blocks;
    nc_mm_usage(k, &contig, &virt, &blocks);
    for (unsigned i = 0; i < k->pool_nfree; i++) {
        pool_free += k->pool_free[i].len;
    }
    nc_log("hakuX-native-stats",
           "ram: free %u KiB, contiguous %u KiB (%u blocks), virtual %u KiB, kernel pool %u of %u KiB "
           "(Ex %u, objects %u, lost %u KiB, %u free ranges)",
           nc_vm_free_bytes(k->vm) >> 10, contig >> 10, blocks, virt >> 10, (k->pool_len - pool_free) >> 10,
           k->pool_len >> 10, k->pool_ex >> 10, k->pool_ob >> 10, k->pool_lost >> 10, k->pool_nfree);
    memset(g_kcount, 0, sizeof(g_kcount));
    g_kcalls = g_sysops = 0;
    memset(g_sysop_kind, 0, sizeof(g_sysop_kind));
    memset(&g_nc_sched, 0, sizeof(g_nc_sched));
#ifdef __ANDROID__
    /* On demand (the app has no timeout report): a new value in the property
     * debug.hakux.ncdump logs the last kernel calls and the guest threads. */
    static bool dump_seen;
    static char dump_last[PROP_VALUE_MAX];
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.ncdump", v);
    if (dump_seen && strcmp(v, dump_last)) {
        nc_log("hakuX-native-frontier", "dump on demand (debug.hakux.ncdump=%s)", v);
        nc_kernel_dump_recent(-1);
        nc_mmio_dump_stats(-1);
        dump_int_samples(k);
        nc_kernel_dump_threads(k, -1);
    }
    dump_seen = true;
    memcpy(dump_last, v, sizeof(dump_last));
    /* debug.hakux.ncresume=<id>[,salt], a new value: resume thread t<id> */
    static char resume_last[PROP_VALUE_MAX];
    v[0] = 0;
    hle_prop_get("debug.hakux.ncresume", v);
    if (v[0] && strcmp(v, resume_last)) {
        nc_ps_debug_resume(k, (unsigned)atoi(v));
    }
    memcpy(resume_last, v, sizeof(resume_last));
#endif
}

static unsigned recent_add(const NcThread *t, unsigned ordinal, uint32_t ret, const uint32_t *args)
{
    g_kcalls++;
    if (ordinal < NC_KORDINAL_MAX) {
        g_kcount[ordinal]++;
    }
    unsigned i = g_nrecent++ % RECENT;
    g_recent[i].ordinal = (uint16_t)ordinal;
    g_recent[i].thread = t ? t->id : 0;
    g_recent[i].ret = ret;
    g_recent[i].running = true;
    memcpy(g_recent[i].args, args, sizeof(g_recent[i].args));
    return i;
}

void nc_kernel_dump_recent(int fd)
{
    char line[256];
    /* the ring as it stands: the vCPU goes on adding calls (a guest spinning
     * on KfRaiseIrql, ~50k a second), and a bound read each round chased
     * them for ever, the dumping thread (a fault's, the UI's) with it */
    const unsigned end = __atomic_load_n(&g_nrecent, __ATOMIC_RELAXED);
    for (unsigned n = end > RECENT ? end - RECENT : 0; n < end; n++) {
        const unsigned i = n % RECENT;
        const NcKExport *e = nc_kernel_export(g_recent[i].ordinal);
        char result[16];
        snprintf(result, sizeof(result), g_recent[i].running ? "(running)" : "%08x", g_recent[i].result);
        int len = snprintf(line, sizeof(line),
                           "hakuX-native-frontier:   call t%u %s from %08x [%08x %08x %08x %08x %08x] -> %s\n",
                           g_recent[i].thread, e ? e->name : "?", g_recent[i].ret, g_recent[i].args[0],
                           g_recent[i].args[1], g_recent[i].args[2], g_recent[i].args[3], g_recent[i].args[4],
                           result);
        nc_dump_line(fd, line, (size_t)len);
    }
}

static uint8_t *pcr_irql(NcKernel *k)
{
    return (uint8_t *)nc_vm_host(k->vm, k->kpcr_va + 0x24);
}

void nc_kernel_callout_at_irql(NcKernel *k, NcFrame *f, uint32_t routine, const uint32_t *args, unsigned nargs,
                               uint8_t irql)
{
    nc_kernel_callout(k, f, routine, args, nargs);
    NcThread *t = k->current;
    if (k->redirected && t && t->ncallouts) {
        t->callout_irql[t->ncallouts - 1] = *pcr_irql(k);
        *pcr_irql(k) = irql;
    }
}

void nc_kernel_callout(NcKernel *k, NcFrame *f, uint32_t routine, const uint32_t *args, unsigned nargs)
{
    NcThread *t = k->current;
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    if (!t || t->ncallouts == NC_MAX_CALLOUTS) {
        nc_kernel_request_stop(k, f, "callout to %08x: nesting too deep", routine);
        return;
    }
    t->callout_ret[t->ncallouts] = nc_vm_rd32(k->vm, esp);
    t->callout_esp[t->ncallouts] = esp + k->call_pop;
    t->callout_irql[t->ncallouts] = 0xFF;
    t->ncallouts++;
    /* The routine's frame goes below the kernel call's; its stack arguments stay put. */
    uint32_t sp = esp - 16 - 4 * nargs;
    nc_vm_wr32(k->vm, sp, k->callout_return_va);
    for (unsigned i = 0; i < nargs; i++) {
        nc_vm_wr32(k->vm, sp + 4 + 4 * i, args[i]);
    }
    nc_frame_set_gpr(f, NC_ESP, sp);
    nc_frame_set_eip(f, routine);
    k->redirected = true;
}

/* ------------------------------------------------------------- the clock */

static pthread_mutex_t g_clock_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_clock_cv;
static uint64_t g_clock_due; /* host monotonic ns; 0: nothing due */
static NcCpu *g_clock_cpu;

/* The Xbox's clock interrupts every millisecond, KeTickCount and the system
 * time advancing with it. Here every kernel entry refreshes them, and this
 * thread interrupts the guest for what is due (timers, wait timeouts, the
 * slice) and for the time alone when a period passed without a refresh: a
 * title polling KeTickCount without kernel calls sees it advance. Chaos
 * Theory waits ten ticks that way at boot; on the RP5 nothing else was due
 * and it spun for good. NC_CLOCK_TICK=0: the time only with what is due. */
#define CLOCK_PERIOD_NS 1000000ull
/* How far behind now the published time may be (its period, and the
 * thread's wake-up late): within this of a due time, an entry reads the
 * clock (clock_tick). */
#define CLOCK_SLACK_NS (2 * CLOCK_PERIOD_NS)

static void *clock_thread(void *arg)
{
    const char *e = getenv("NC_CLOCK_TICK");
    const bool tick = !e || atoi(e) != 0;
    pthread_mutex_lock(&g_clock_mu);
    uint32_t seen = nc_ke_time_refreshes();
    for (;;) {
        const uint64_t now = nc_ke_now_ns();
        if (tick) {
            nc_ke_publish_time(now);    /* the kernel entries' time (nc_ke_tick_time) */
        }
        uint64_t wake = tick ? now + CLOCK_PERIOD_NS : 0;
        if (g_clock_due && (!wake || g_clock_due < wake)) {
            wake = g_clock_due;
        }
        if (!wake) {
            pthread_cond_wait(&g_clock_cv, &g_clock_mu);
            continue;
        }
        /* the guest's time on the host's clock (later by the time paused);
         * while paused nothing is due: a look now and then */
        uint64_t host = nc_ke_host_ns(wake);
        const bool paused = nc_paused();
        if (paused) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            host = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec + 100000000ull;
        }
        const struct timespec ts = {(time_t)(host / 1000000000ull), (long)(host % 1000000000ull)};
        if (pthread_cond_timedwait(&g_clock_cv, &g_clock_mu, &ts) == 0 || paused) {
            continue; /* re-armed, or paused */
        }
        bool irq = false;
        const uint64_t woke = nc_ke_now_ns();
        if (tick) {
            nc_ke_publish_time(woke);   /* before the interrupt: its clock_tick looks by it */
        }
        if (g_clock_due && woke >= g_clock_due) {
            g_clock_due = 0;
            irq = true;
        }
        /* a period without a refresh, the guest running its own code (not
         * in the kernel, which may be waiting idle) */
        const uint32_t r = nc_ke_time_refreshes();
        if (tick && r == seen && !g_nc_in_sysop) {
            irq = true;
            __atomic_store_n(&g_clock_ticks, g_clock_ticks + 1, __ATOMIC_RELAXED);
        }
        seen = r;
        if (irq && g_clock_cpu) {
            nc_cpu_request_interrupt(g_clock_cpu);
        }
    }
    return NULL;
}

void nc_kernel_clock_arm(NcKernel *k)
{
    uint64_t due = k->timer_due_ns;
    if (k->wait_due_ns && (!due || k->wait_due_ns < due)) {
        due = k->wait_due_ns;
    }
    /* a slice only while its thread runs (not an ISR or a DPC, briefly) */
    if (k->slice_due_ns && k->current == k->slice_thread && (!due || k->slice_due_ns < due)) {
        due = k->slice_due_ns;
    }
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("NC_CLOCK");
        enabled = !e || atoi(e) != 0;
    }
    static bool started;
    if (!k->cpu || !enabled || (!due && started)) {
        return;
    }
    pthread_mutex_lock(&g_clock_mu);
    if (!started) {         /* the first call starts it, due or not (its tick) */
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&g_clock_cv, &attr);
        pthread_condattr_destroy(&attr);
        g_clock_cpu = k->cpu;
        pthread_t t;
        pthread_create(&t, NULL, clock_thread, NULL);
        pthread_detach(t);
        started = true;
    }
    if (due && (!g_clock_due || due < g_clock_due)) {
        g_clock_due = due;
        pthread_cond_signal(&g_clock_cv);
    }
    pthread_mutex_unlock(&g_clock_mu);
}

/* Timers and wait timeouts that are due: fire them, wake their threads, and
 * reschedule if one of those outranks the running thread. Then the running
 * thread's slice. */
static void clock_tick(NcKernel *k)
{
    if (!k->timer_due_ns && !k->wait_due_ns && !k->slice_due_ns) {
        return;
    }
    /* Nothing due for a while by the published time: no clock read (one a
     * kernel entry otherwise, something being armed nearly always). Each due
     * time is armed with the clock thread (nc_kernel_clock_arm), which wakes
     * the guest for it on time if no entry comes after it. */
    const uint64_t pub = nc_ke_published_ns();
    if (pub) {
        uint64_t due = UINT64_MAX;
        if (k->timer_due_ns) {
            due = k->timer_due_ns;
        }
        if (k->wait_due_ns && k->wait_due_ns < due) {
            due = k->wait_due_ns;
        }
        if (k->slice_due_ns && k->slice_due_ns < due) {
            due = k->slice_due_ns;
        }
        if (pub + CLOCK_SLACK_NS < due) {
            return;
        }
    }
    const uint64_t now = nc_ke_now_ns();
    const bool timer_due = k->timer_due_ns && now >= k->timer_due_ns;
    const bool wait_due = k->wait_due_ns && now >= k->wait_due_ns;
    const bool slice_due = k->slice_due_ns && now >= k->slice_due_ns;
    if (!timer_due && !wait_due && !slice_due) {
        return;
    }
    if (timer_due) {
        bool fired;
        nc_timer_poll(k, &fired);
        k->resched |= fired;
    }
    if (wait_due) {
        nc_ke_check_waits(k);
        const NcThread *cur = k->current;
        for (unsigned i = 0; cur && i < k->threads_hi; i++) {
            const NcThread *t = &k->threads[i];
            if (t->state == NC_T_READY && !t->suspend && t->priority > cur->priority) {
                k->resched = true;
                break;
            }
        }
    }
    if (slice_due) {
        nc_ps_slice_end(k, now);
    }
    nc_kernel_clock_arm(k);
}

/* The fault scope (nc_kernel_fault_scope): scopes open only on the guest CPU
 * thread, which g_scope_thread names; not thread-locals, which are emulated
 * (a call per access) on API 28. */
static nc_jmp_buf *t_call_env;
static pthread_t g_scope_thread;
static uint32_t t_fault_va;
static bool t_fault_write;

static uint32_t pf_handler(NcKernel *k)
{
    const uint8_t *gate = nc_vm_host(k->vm, k->idt_va + 8 * 14);
    return (nc_rd32(gate + 4) & 0xFFFF0000u) | (nc_rd32(gate) & 0xFFFFu);
}

static bool nc_kernel_title_pages(NcKernel *k)
{
    return pf_handler(k) != k->trap_va;
}

static void nc_kernel_fault_scope(nc_jmp_buf *env)
{
    g_scope_thread = pthread_self();
    t_call_env = env;
}

static void nc_kernel_fault_scope_end(void)
{
    t_call_env = NULL;
}

void nc_kernel_call_fault(NcKernel *k, uint32_t va, bool write)
{
    nc_jmp_buf *env = t_call_env;
    if (!env || !pthread_equal(g_scope_thread, pthread_self()) || !nc_kernel_title_pages(k)) {
        return;
    }
    t_call_env = NULL;
    t_fault_va = va;
    t_fault_write = write;
    /* Leaving the signal handler by a jump: the fault signals unblock as
     * its return would have done. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    sigaddset(&s, SIGBUS);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    nc_longjmp(*env, 1);
}

/* A kernel call that held the guest's CPU for over 8 ms: nothing else ran
 * meanwhile, interrupts included (a vblank taken late presents its frame
 * late). Up to 10 a second are logged. HLE hooks aren't timed: a clock read
 * takes ~130 ns on the RP5 (the bare counter as well), two a hook at ~840k
 * hooks a second (Blinx); a hook that holds the CPU shows as a late
 * interrupt, the guest CPU's thread running. */
static void slow_note(const char *what, uint32_t at, const uint32_t *args, uint64_t ns)
{
    if (ns < 8000000ull) {
        return;
    }
    static uint64_t window;
    static unsigned logged;
    const uint64_t now = nc_ke_now_ns();
    if (now - window > 1000000000ull) {
        window = now;
        logged = 0;
    }
    if (logged++ < 10) {
        nc_log("hakuX-native-slow", "%s from %08x held the CPU %.1f ms [%08x %08x %08x %08x %08x %08x]", what, at,
               ns / 1e6, args[0], args[1], args[2], args[3], args[4], args[5]);
    }
}

static void kcall(NcKernel *k, NcFrame *f, unsigned ordinal)
{
    nc_ke_tick_time(k);
    clock_tick(k);
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    const uint32_t ret = nc_vm_rd32(k->vm, esp);

    if (ordinal == NC_KORDINAL_DPC_RETURN) {
        nc_dpc_return(k);
        return;
    }
    if (ordinal == NC_KORDINAL_CALLOUT_RETURN) {
        /* The routine's EAX is the kernel call's result. */
        NcThread *t = k->current;
        if (!t || !t->ncallouts) {
            nc_kernel_request_stop(k, f, "callout return with no callout pending");
            return;
        }
        t->ncallouts--;
        nc_frame_set_gpr(f, NC_ESP, t->callout_esp[t->ncallouts]);
        nc_frame_set_eip(f, t->callout_ret[t->ncallouts]);
        if (t->callout_irql[t->ncallouts] != 0xFF) {
            *pcr_irql(k) = t->callout_irql[t->ncallouts];
            /* As KfLowerIrql: DPCs queued while the routine ran at its IRQL
             * (KeSynchronizeExecution's, at the interrupt's) run now. They
             * waited for the next interrupt: Halo's vblank DPC a vblank late. */
            if (*pcr_irql(k) < 2 && nc_dpc_pending()) {
                k->resched = true;
            }
        }
        return;
    }
    if (ordinal == NC_KORDINAL_APC_RETURN) {
        nc_apc_return(k, f);
        return;
    }
    if (ordinal == NC_KORDINAL_ISR_RETURN) {
        nc_intr_return(k);
        return;
    }
    if (ordinal == NC_KORDINAL_SEH_RETURN || ordinal == NC_KORDINAL_SEH_NESTED ||
        ordinal == NC_KORDINAL_SEH_COLLIDED) {
        nc_seh_stub(k, f, ordinal);
        return;
    }
    if (ordinal == NC_KORDINAL_APU_SPIN) {
        nc_kernel_apu_spin(k, f);
        return;
    }
    if (ordinal == NC_KORDINAL_TRAP) {
        /* A page fault the title's handler passed on (the only exception
         * delivered): error code, EIP, CS, EFLAGS on the stack. */
        nc_kernel_request_stop(k, f, "page fault at %08x from eip %08x (error %x) not handled by the title",
                               k->cr2, nc_vm_rd32(k->vm, esp + 4), ret);
        return;
    }
    if (ordinal == NC_KORDINAL_THREAD_RETURN) {
        /* As the kernel's thread startup does when the start routine returns. */
        nc_log("hakuX-native", "ps: thread %u start routine returned (eax=%08x)",
               k->current ? k->current->id : 0, nc_frame_gpr(f, NC_EAX));
        nc_ps_terminate_current(k, NC_STATUS_SUCCESS);
        return;
    }
    const NcKExport *e = nc_kernel_export(ordinal);
    if (!e || e->abi == NC_KABI_DATA) {
        nc_kernel_request_stop(k, f, "call to kernel ordinal %u (not a function) from %08x",
                               ordinal, ret);
        return;
    }

    /* Arguments in declaration order; cdecl: as many as the log shows. */
    uint32_t args[16] = {0};
    unsigned nargs = e->abi == NC_KABI_CDECL ? 6 : e->arg_bytes / 4u;
    unsigned stack_first = 0;
    if (e->abi == NC_KABI_FASTCALL) {
        args[0] = nc_frame_gpr(f, NC_ECX);
        args[1] = nc_frame_gpr(f, NC_EDX);
        stack_first = 2;
    }
    for (unsigned i = stack_first; i < nargs && i < 16; i++) {
        args[i] = nc_vm_rd32(k->vm, esp + 4 + 4 * (i - stack_first));
    }

    k->calls++;
    if (!k->first_ordinal) {
        k->first_ordinal = ordinal;
        nc_log("hakuX-native-frontier", "stage S2 first kernel call: %s (%u) from %08x",
               e->name, ordinal, ret);
    }

    if (!k->impl[ordinal]) {
        char a[160] = "";
        size_t n = 0;
        for (unsigned i = 0; i < nargs && i < 8 && n < sizeof(a) - 12; i++) {
            n += (size_t)snprintf(a + n, sizeof(a) - n, "%s%08x", i ? " " : "", args[i]);
        }
        nc_kernel_request_stop(k, f, "kunimpl %u %s caller=%08x args=[%s]", ordinal, e->name,
                               ret, a);
        return;
    }

    uint32_t pop = 4;
    if (e->abi == NC_KABI_STDCALL) {
        pop += e->arg_bytes;
    } else if (e->abi == NC_KABI_FASTCALL && e->arg_bytes > 8) {
        pop += e->arg_bytes - 8u;
    }
    k->call_pop = pop;
    k->frame = f;
    k->redirected = false;

    NcThread *caller = k->current;
    const unsigned slot = recent_add(caller, ordinal, ret, args);
    nc_jmp_buf env;
    const bool scoped = nc_kernel_title_pages(k);
    if (scoped && nc_setjmp(env)) {
        /* It touched a page the title pages in (nc_kernel_call_fault): it
         * runs again from the trampoline once the title's handler has it. */
        k->in_call = false;
        g_recent[slot].running = false;
        static unsigned s_logged;
        if (s_logged++ < 64) {
            nc_log("hakuX-native", "kernel: %s from %08x touched %08x, paged out: it runs again", e->name, ret,
                   t_fault_va);
        }
        if (!nc_kernel_page_fault(k, f, t_fault_va, t_fault_write)) {
            nc_kernel_request_stop(k, f, "%s touched %08x, a page the guest doesn't have", e->name, t_fault_va);
        }
        return;
    }
    if (scoped) {
        nc_kernel_fault_scope(&env);
    }
    k->in_call = true;
    const uint64_t t0 = nc_ke_now_ns();
    uint64_t r = k->impl[ordinal](k, f, args);
    slow_note(e->name, ret, args, nc_ke_now_ns() - t0);
    k->in_call = false;
    nc_kernel_fault_scope_end();
    g_recent[slot].result = (uint32_t)r;
    g_recent[slot].running = false;
    /* NC_TRACE_KCALL=<n>: the first n kernel calls (or the n after the
     * NC_TRACE_KCALL_AT trigger, see nc_kernel_trace_trigger). */
    if (g_trace_left < 0) {
        const char *t = getenv("NC_TRACE_KCALL");
        g_trace_left = t && !getenv("NC_TRACE_KCALL_AT") ? atol(t) : 0;
    }
    if (g_trace_left > 0) {
        g_trace_left--;
        char calls[96];
        nc_kernel_frame_chain(k, f, calls, sizeof(calls));
        nc_log("hakuX-ktrace", "t%u %s from %08x [%08x %08x %08x %08x %08x %08x] -> %08x%s | frames%s",
               caller ? caller->id : 0, e->name, ret, args[0], args[1], args[2], args[3], args[4], args[5],
               (uint32_t)r, k->redirected ? " (callout)" : "", calls);
    } else if ((uint32_t)r >= 0xC0000000u && !k->redirected &&
               (!strncmp(e->name, "Nt", 2) || !strncmp(e->name, "Io", 2) || !strncmp(e->name, "Ob", 2))) {
        /* Failed object and I/O calls are rare and usually tell the story,
         * with the title code that led there; a title failing one in a loop
         * has its first 400 told, then one in 100. */
        static unsigned failed;
        if (failed++ < 400 || failed % 100 == 0) {
            char calls[96];
            nc_kernel_frame_chain(k, f, calls, sizeof(calls));
            nc_log("hakuX-ktrace", "t%u %s from %08x [%08x %08x %08x %08x %08x %08x] -> %08x | frames%s%s",
                   caller ? caller->id : 0, e->name, ret, args[0], args[1], args[2], args[3], args[4], args[5],
                   (uint32_t)r, calls, failed > 400 ? " (one of 100)" : "");
        }
    }
    if ((caller && caller->state == NC_T_TERMINATED) || k->redirected) {
        return; /* PsTerminateSystemThread doesn't return; a callout returns later */
    }
    nc_frame_set_gpr(f, NC_EAX, (uint32_t)r);
    nc_frame_set_gpr(f, NC_EDX, (uint32_t)(r >> 32));
    nc_frame_set_gpr(f, NC_ESP, esp + pop);
    nc_frame_set_eip(f, ret);
}

/* The Xbox CPU: Pentium III (Coppermine), GenuineIntel, max leaf 2. */
static void cpuid(NcFrame *f)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    switch (nc_frame_gpr(f, NC_EAX)) {
    case 0:
        a = 2;
        b = 0x756E6547; /* Genu */
        d = 0x49656E69; /* ineI */
        c = 0x6C65746E; /* ntel */
        break;
    case 1:
        a = 0x068A;     /* family 6, model 8, stepping 10 */
        d = 0x0383F9FF; /* FPU..CMOV, PAT, PSE36, MMX, FXSR, SSE */
        break;
    case 2:
        a = 0x03020101;
        d = 0x0C040843;
        break;
    default:
        break;
    }
    nc_frame_set_gpr(f, NC_EAX, a);
    nc_frame_set_gpr(f, NC_EBX, b);
    nc_frame_set_gpr(f, NC_ECX, c);
    nc_frame_set_gpr(f, NC_EDX, d);
}

/* IN/OUT: E4/E5/E6/E7 imm8, EC/ED/EE/EF DX; a 66 prefix makes the
 * doubleword forms 16-bit. */
static void port_io(NcFrame *f, const uint8_t *insn, unsigned size, bool out)
{
    unsigned p = 0;
    bool op16 = false;
    while (p < size && (insn[p] == 0x66 || insn[p] == 0xF2 || insn[p] == 0xF3 || insn[p] == 0x2E ||
                        insn[p] == 0x3E)) {
        op16 |= insn[p] == 0x66;
        p++;
    }
    const uint8_t opc = insn[p];
    const bool imm = opc >= 0xE4 && opc <= 0xE7;
    const bool byte = opc == 0xE4 || opc == 0xE6 || opc == 0xEC || opc == 0xEE;
    const unsigned bytes = byte ? 1 : op16 ? 2 : 4;
    const uint16_t port = imm ? insn[p + 1] : (uint16_t)nc_frame_gpr(f, NC_EDX);
    const uint32_t mask = bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1;
    const uint32_t eax = nc_frame_gpr(f, NC_EAX);
    if (out) {
        nc_port_out(port, bytes, eax & mask);
    } else {
        nc_frame_set_gpr(f, NC_EAX, (eax & ~mask) | (nc_port_in(port, bytes) & mask));
    }
}

bool nc_kernel_page_fault(NcKernel *k, NcFrame *f, uint32_t va, bool write)
{
    const uint32_t handler = pf_handler(k);
    const uint32_t esp = nc_frame_gpr(f, NC_ESP) - 16;
    if (handler == k->trap_va || nc_vm_phys(k->vm, esp) == UINT32_MAX || nc_vm_phys(k->vm, esp + 15) == UINT32_MAX) {
        return false;
    }
    /* As a ring-0 interrupt gate: EFLAGS, CS, EIP (the faulting instruction,
     * which runs again after IRET) and the error code (not present,
     * supervisor, W for a write); IF clears. */
    nc_vm_wr32(k->vm, esp + 12, nc_frame_eflags(f));
    nc_vm_wr32(k->vm, esp + 8, nc_frame_cs(f));
    nc_vm_wr32(k->vm, esp + 4, nc_frame_eip(f));
    nc_vm_wr32(k->vm, esp, write ? 2u : 0u);
    nc_frame_set_gpr(f, NC_ESP, esp);
    nc_frame_set_if(f, false);
    nc_frame_set_eip(f, handler);
    k->cr2 = va;
    k->page_faults++;
    return true;
}

bool nc_kernel_resident(NcKernel *k, uint32_t va, uint32_t len, bool write)
{
    if (!len) {
        return true;
    }
    const uint32_t first = va / NC_PAGE, last = (uint32_t)(((uint64_t)va + len - 1) / NC_PAGE);
    for (uint32_t pg = first; pg <= last && pg >= first; pg++) {
        const uint32_t p = pg * NC_PAGE;
        if (nc_vm_phys(k->vm, p) != UINT32_MAX || nc_paging_fault(k, p)) {
            continue;
        }
        if (k->in_call && k->frame && nc_kernel_page_fault(k, k->frame, p, write)) {
            k->redirected = true;
            return false;
        }
        break; /* nothing maps it: the access fails */
    }
    return true;
}

/* Guest EIPs at interrupt exits: a sample of where the guest runs (a
 * guest spin shows up as one block). */
#define INT_SAMPLES 512u
static struct {
    uint32_t eip, count;
} g_int_eips[INT_SAMPLES];

static void int_sample(uint32_t eip)
{
    unsigned h = (eip * 2654435761u) % INT_SAMPLES;
    for (unsigned n = 0; n < 16; n++, h = (h + 1) % INT_SAMPLES) {
        if (g_int_eips[h].eip == eip || !g_int_eips[h].count) {
            g_int_eips[h].eip = eip;
            g_int_eips[h].count++;
            return;
        }
    }
}

static void dump_int_samples(NcKernel *k)
{
    uint32_t total = 0;
    for (unsigned i = 0; i < INT_SAMPLES; i++) {
        total += g_int_eips[i].count;
    }
    for (int r = 0; r < 6; r++) {
        unsigned best = INT_SAMPLES;
        for (unsigned i = 0; i < INT_SAMPLES; i++) {
            if (g_int_eips[i].count && (best == INT_SAMPLES || g_int_eips[i].count > g_int_eips[best].count)) {
                best = i;
            }
        }
        if (best == INT_SAMPLES) {
            break;
        }
        const uint32_t eip = g_int_eips[best].eip;
        char code[100] = "";
        int n = 0;
        for (unsigned b = 0; b < 32 && nc_vm_phys(k->vm, eip + b) != UINT32_MAX; b++) {
            n += snprintf(code + n, sizeof(code) - (size_t)n, "%02x", *(const uint8_t *)nc_vm_host(k->vm, eip + b));
        }
        nc_log("hakuX-native-frontier", "guest at %08x: %u of %u interrupt exits | %s", eip, g_int_eips[best].count,
               total, code);
        g_int_eips[best].count = 0;
    }
    memset(g_int_eips, 0, sizeof(g_int_eips));
}

static bool sysop(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size);

/* int 2Dh, the debug service (DbgPrint, XAPI's OutputDebugStringA, which
 * Project Gotham Racing links): EAX the service, ECX and EDX its arguments,
 * and an int 3 after it. With no debugger the kernel takes a print
 * (BREAKPOINT_PRINT 1, ECX a STRING: Length, MaximumLength, Buffer) and
 * the other services, returns STATUS_SUCCESS (a prompt reads nothing) and
 * resumes past the int 3. Prints are logged, the first 200. */
static void debug_service(NcKernel *k, NcFrame *f, uint32_t eip)
{
    const uint32_t service = nc_frame_gpr(f, NC_EAX), str = nc_frame_gpr(f, NC_ECX);
    static unsigned logged;
    if (service == 1 && str && logged < 200 && nc_vm_phys(k->vm, str) != UINT32_MAX &&
        nc_vm_phys(k->vm, str + 7) != UINT32_MAX) {
        const uint32_t len = nc_vm_rd32(k->vm, str) & 0xFFFF, buf = nc_vm_rd32(k->vm, str + 4);
        char text[256];
        uint32_t n = 0;
        for (; n < len && n < sizeof(text) - 1 && nc_vm_phys(k->vm, buf + n) != UINT32_MAX; n++) {
            const char c = *(const char *)nc_vm_host(k->vm, buf + n);
            text[n] = c == '\n' || c == '\r' ? ' ' : c;
        }
        text[n] = 0;
        logged++;
        nc_log("hakuX-native-dbg", "%s", text);
    }
    nc_frame_set_gpr(f, NC_EAX, 0);
    const uint32_t next = eip + 2;
    const bool bp = nc_vm_phys(k->vm, next) != UINT32_MAX && *(const uint8_t *)nc_vm_host(k->vm, next) == 0xCC;
    nc_frame_set_eip(f, bp ? next + 1 : next);
}

volatile int g_nc_in_sysop;

/* debug (NC_IRQ_SCHED=1): a kernel entry other than an export's call (those
 * are hakuX-native-slow's) the guest CPU spent over 4 ms in, idling in none
 * of it: hooks, MMIO sites, the kernel's own stubs (DPC and callout returns,
 * the APU mailbox wait). Its interrupts waited meanwhile. */
static int g_slow_sysops = -1;

static uint64_t slow_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void slow_sysop(NcKernel *k, enum NcSysOp op, uint32_t eip, uint16_t ordinal, uint64_t ns,
                       uint64_t idles, uint64_t ints)
{
    const NcKExport *e = op == NC_SYSOP_INT && ordinal ? nc_kernel_export(ordinal) : NULL;
    if (ns < 4000000ull || idles || (e && e->name)) {
        return;     /* idle meanwhile: the scheduler took the interrupts */
    }
    static uint64_t window;
    static unsigned logged;
    const uint64_t now = slow_now_ns();
    if (now - window > 1000000000ull) {
        window = now;
        logged = 0;
    }
    if (logged++ >= 10) {
        return;
    }
    nc_log("hakuX-native-irq", "kernel entry took %.1f ms: op %d at %08x (ordinal %u, thread t%u, "
           "%llu interrupts taken in it)", ns / 1e6, (int)op, eip, ordinal, k->current ? k->current->id : 0,
           (unsigned long long)ints);
}

void nc_kernel_set_paused(NcKernel *k, bool on)
{
    nc_log("hakuX-native", "%s requested (the activity: paused, or its menu open)", on ? "pause" : "resume");
    nc_pause_set(on);
}

/* The emulation pauses with the activity (the device suspends, another app
 * comes in front), where the guest's CPU enters the kernel: its threads stop
 * there, the devices that run on their own hold off (nc_paused), and the
 * guest's time stands still (the kernel's, RDTSC's) so that it goes on where
 * it stopped: no time jump, no timers due at once. It went on running, the
 * audio with it. */
static uint64_t g_pause_c0;

uint64_t nc_kernel_pause_cycles(void)
{
    return g_pause_c0;
}

void nc_kernel_pause_point(NcKernel *k, NcFrame *f, bool idle)
{
    if (!nc_paused()) {
        return;
    }
    nc_ke_pause_begin();
    hle_hitch_pause();
    const uint64_t c0 = nc_cpu_host_cycles();
    g_pause_c0 = c0;
    nc_log("hakuX-native", "paused: the guest's CPU waits");
    /* Paused, the guest's CPU still takes a savestate (nc_state_save):
     * here its frame is whole (a kernel entry: nothing of the call done
     * yet; idle: every thread switched out). */
    for (;;) {
        nc_pause_wait();
        if (!nc_paused()) {
            break;
        }
        if (nc_pause_cpu_work()) {
            nc_state_cpu_work(k, f, idle);
        }
    }
    const uint64_t c1 = nc_cpu_host_cycles();
    if (k->cpu) {
        nc_cpu_cycle_offset_add(k->cpu, c1 - c0);
    }
    const uint64_t ns = nc_ke_pause_end();
    nc_log("hakuX-native", "resumed after %.1f s: the guest's time leaves it out", ns / 1e9);
}

/* The samplers tell a kernel call or an HLE hook (set) from FEX itself. */
bool nc_kernel_sysop(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    g_nc_in_sysop = 1;
    nc_kernel_pause_point(opaque, f, false);
    nc_ke_time_enter();
    if (g_slow_sysops < 0) {
        const char *e = getenv("NC_IRQ_SCHED");
        g_slow_sysops = e && atoi(e) != 0;
    }
    uint64_t t0 = 0, idles = 0, ints = 0;
    uint32_t eip = 0;
    uint16_t ordinal = 0;
    if (g_slow_sysops) {
        NcKernel *k = opaque;
        t0 = slow_now_ns();
        idles = g_nc_sched.idles;
        ints = k->interrupts;
        eip = nc_frame_eip(f);
        const uint8_t *insn = nc_vm_host(k->vm, eip);
        if (op == NC_SYSOP_INT && insn && insn[1] == NC_KCALL_VECTOR) {
            ordinal = nc_rd16(insn + 2);
        }
    }
    const bool r = sysop(opaque, f, op, size);
    if (t0) {
        NcKernel *k = opaque;
        slow_sysop(k, op, eip, ordinal, slow_now_ns() - t0, g_nc_sched.idles - idles,
                   k->interrupts - ints);
    }
    nc_ke_time_leave();
    g_nc_in_sysop = 0;
    return r;
}

static bool sysop(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    NcKernel *k = opaque;
    g_sysop_kind[(unsigned)op & 31]++;
    if ((++g_sysops & 0xFFF) == 0) {
        stats_tick(k);
    }
    const uint32_t eip = nc_frame_eip(f);
    const uint8_t *insn = nc_vm_host(k->vm, eip);
    nc_x86mmio_flush(k);

    switch (op) {
    case NC_SYSOP_INT:
        if (insn[1] == NC_KCALL_VECTOR) {
            kcall(k, f, nc_rd16(insn + 2));
            break;
        }
        if (insn[1] == 0x2D) {
            debug_service(k, f, eip);
            break;
        }
        nc_kernel_request_stop(k, f, "int %02xh at %08x", insn[1], eip);
        break;
    case NC_SYSOP_CLI:
        nc_frame_set_if(f, false);
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_STI:
        nc_frame_set_if(f, true);
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_CPUID:
        cpuid(f);
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_IN:
    case NC_SYSOP_OUT:
        port_io(f, insn, size, op == NC_SYSOP_OUT);
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_WBINVD:
    case NC_SYSOP_INVD:
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_INVLPG:
        nc_paging_flush(k); /* a title that edits its page tables (paging.c) */
        nc_frame_set_eip(f, eip + size);
        break;
    case NC_SYSOP_MOVCR:
    case NC_SYSOP_OTHER:
        /* MOV r32, CRn (0F 20 /r) and MOV CRn, r32 (0F 22 /r). */
        if (insn[0] == 0x0F && (insn[1] == 0x20 || insn[1] == 0x22) && (insn[2] & 0xC0) == 0xC0) {
            const unsigned cr = (insn[2] >> 3) & 7, gpr = insn[2] & 7;
            if (insn[1] == 0x20) {
                /* CR0: PG WP NE ET MP PE, as the Xbox kernel leaves it;
                 * CR4: PSE OSFXSR OSXMMEXCPT. */
                const uint32_t v = cr == 0   ? 0x8001003Bu
                                   : cr == 2 ? k->cr2
                                   : cr == 3 ? nc_paging_cr3(k)
                                   : cr == 4 ? 0x610u
                                             : 0;
                nc_frame_set_gpr(f, gpr, v);
            } else if (cr == 3) {
                nc_paging_flush(k); /* a CR3 reload: the TLB flush after PDE edits */
            }
            nc_frame_set_eip(f, eip + 3);
            break;
        }
        nc_kernel_request_stop(k, f, "system op %d at %08x (%02x %02x %02x)", (int)op, eip, insn[0], insn[1],
                               insn[2]);
        break;
    case NC_SYSOP_INTERRUPT:
        /* The guest stopped at a block start for an asserted line or the
         * clock; EIP stays. Where it was, sampled, says where the time goes
         * (the on-demand dump lists the busiest). */
        int_sample(eip);
        nc_frame_clear_interrupt(f);
        nc_ke_update_time(k);
        clock_tick(k);
        break;
    case NC_SYSOP_HOOK:
        if (nc_x86mmio_hook(k, f, eip)) {
            break;
        }
        if (k->hle_hook) {
            k->hle_hook(k, f);
        } else {
            nc_frame_bypass_hook(f);
        }
        break;
    default:
        nc_kernel_request_stop(k, f, "system op %d at %08x (%02x %02x %02x)", (int)op, eip,
                               insn[0], insn[1], insn[2]);
        break;
    }

    if (k->hle_after_sysop) {
        k->hle_after_sysop(k);
    }
    /* The first thread to select x87 single precision turns FEX's rounding
     * to it on (a flush of the translations, once). */
    if (k->cpu) {
        nc_cpu_x87_single_check(k->cpu);
    }

    /* Disc reads done meanwhile complete, and asserted lines are delivered,
     * on the way back to the guest. Not while the thread has just begun a
     * wait: a read completing now would wake it through its saved context
     * (the status, an APC), which the scheduler then overwrites with the
     * live frame, the wait's provisional STATUS_TIMEOUT included. Burnout 3's
     * movie header read waited forever and got a timeout: GetOverlappedResult
     * failed, the XMV decoder was NULL. The scheduler, which runs next,
     * saves the frame first and polls then. */
    const bool running = k->current && k->current->state == NC_T_RUNNING;
    if (!k->stop) {
        if (running) {
            nc_io_poll(k);
        }
        nc_mods_poll();                 /* the cheats, once a vblank */
        nc_intr_dispatch(k, !running || nc_frame_if(f));
    }

    /* No thread switch while the running thread is at DISPATCH_LEVEL or above
     * (a DPC, or code that raised the IRQL); it happens when the IRQL drops.
     * An ISR's entry is the exception. */
    const uint8_t irql = *pcr_irql(k);
    if (k->resched && !k->stop && (k->force_switch || !(running && irql >= 2))) {
        nc_ps_schedule(k, f);
    }
    if (k->stop) {
        nc_frame_set_eip(f, k->stop_va);
    }
    return true;
}
