#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static NcCore core;
static uint32_t mmio_value, result, calls;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        exit(1);
    }
}

static void fault(int sig, siginfo_t *info, void *context)
{
    if (core.cpu && nc_core_fault(&core, sig, info, context)) {
        return;
    }
    static const char message[] = "FAIL: unexpected host fault\n";
    write(2, message, sizeof(message) - 1);
    _exit(3);
}

static uint64_t read_mmio(void *opaque, uint32_t offset, unsigned size)
{
    return mmio_value;
}

static void write_mmio(void *opaque, uint32_t offset, uint64_t value, unsigned size)
{
    mmio_value = (uint32_t)value;
}

static bool system_op(void *opaque, NcFrame *frame, enum NcSysOp op, unsigned size)
{
    if (op != NC_SYSOP_INT || size != 2) {
        return false;
    }
    calls++;
    result = nc_frame_gpr(frame, NC_EAX);
    nc_frame_set_eip(frame, nc_frame_eip(frame) + size);
    return true;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    check(sysconf(_SC_PAGESIZE) == NC_PAGE, "host page size supports Xbox page mappings");
    check(nc_vm_init(&core.vm, 64u << 20), "memfd VM initialization");
    check(nc_kernel_init(&core.kernel, &core.vm), "replacement kernel initialization");
    check(nc_vm_alloc_at(&core.vm, 0x10000, NC_PAGE, NULL), "guest code page");
    uint32_t pa;
    check(nc_vm_alloc_at(&core.vm, 0x20000, NC_PAGE, &pa), "guest data page");
    const uint32_t alias = NC_CONTIG_WINDOW + pa;
    /* Volatile accesses preserve ordering across distinct virtual addresses
     * which the C optimizer cannot know alias the same physical page. */
    *(volatile uint32_t *)nc_vm_host(&core.vm, 0x20000) = 0x12345678;
    check(*(volatile uint32_t *)nc_vm_host(&core.vm, alias) == 0x12345678, "memory aliases share bytes");

    nc_hle_host_init(&core.kernel, &core.vm, NULL);
    nc_hle_dirty_init(&core.vm);
    hle_guestmem_init(core.vm.phys, core.vm.ram_size, 0);
    hle_barrier_init(core.vm.ram_size);
    hle_barrier_arm(pa, NC_PAGE, 1);
    check(!hle_host_mem_dirty(pa), "data page is tracked and read-only");
    NcMmioDev dev = {"smoke", 0xFD000000, NC_PAGE, read_mmio, write_mmio, NULL};
    nc_mmio_register(&dev);
    struct sigaction sa = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGSEGV, &sa, NULL) == 0 && sigaction(SIGBUS, &sa, NULL) == 0,
          "glibc signal handlers installed");

    /* mov eax,6; mov ebx,7; add; cmp 13; jne fail; write data;
     * read alias; write/read MMIO; int 42; hlt. Failure reports 0xdead. */
    uint8_t code[] = {
        0xb8,6,0,0,0, 0xbb,7,0,0,0, 0x01,0xd8, 0x83,0xf8,13,
        0x75,23, 0xa3,0,0,2,0, 0xa1,0,0,3,0,
        0xa3,0,0,0,0xfd, 0xa1,0,0,0,0xfd, 0xcd,0x42, 0xf4,
        0xb8,0xad,0xde,0,0, 0xcd,0x42, 0xf4
    };
    nc_wr32(code + 23, alias);
    memcpy(nc_vm_host(&core.vm, 0x10000), code, sizeof(code));
    NcCpuConfig cfg = {.membase = core.vm.base, .sysop = system_op};
    core.cpu = nc_cpu_create(&cfg, core.kernel.kpcr_va);
    check(core.cpu != NULL, "actual NcCpu/FEX embedding initialized");
    NcCpuContext context;
    nc_context_init(core.cpu, &context, 0x10000, 0);
    nc_cpu_run(core.cpu, &context);
    check(calls == 1, "system-operation callback and HLT exit");
    check(result == (argc > 1 && !strcmp(argv[1], "--wrong-expectation") ? 14u : 13u),
          "translated arithmetic, branch and registers");
    check(*(volatile uint32_t *)nc_vm_host(&core.vm, alias) == 13, "translated write visible through alias");
    check(g_nc_faults_dirty == 1 && hle_host_mem_dirty(pa), "translated write fault resumed and dirtied page");
    check(g_nc_faults_mmio == 2 && mmio_value == 13, "translated MMIO read/write faults resumed");
    /* x86 permits a locked dword at an odd address. Older ARM hosts need
     * the SIGBUS handler; newer ones may execute it directly. */
    const uint8_t unaligned[] = {0xf0, 0xff, 0x05, 3, 0, 2, 0, 0xf4}; /* lock inc [0x20003]; hlt */
    memcpy(nc_vm_host(&core.vm, 0x10100), unaligned, sizeof(unaligned));
    nc_wr32(nc_vm_host(&core.vm, 0x20003), 41);
    nc_context_init(core.cpu, &context, 0x10100, 0);
    nc_cpu_run(core.cpu, &context);
    check(nc_rd32(nc_vm_host(&core.vm, 0x20003)) == 42, "translated unaligned locked increment");
    if (argc > 1 && !strcmp(argv[1], "--host-fault")) {
        /* Host code writing the reservation's low guard, after the CPU ran:
         * nc_core_fault must decline it rather than resume it as the guest's. */
        *(volatile uint32_t *)core.vm.reservation = 1;
        return 1;
    }
    return 0;
}
