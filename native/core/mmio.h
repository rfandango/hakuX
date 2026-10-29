/*
 * hakuX native core — device MMIO.
 *
 * Device windows (0xFD000000 NV2A, 0xFE800000 APU, ...) stay unmapped in the
 * guest reservation. A guest access faults; the fault handler decodes the
 * arm64 load or store the JIT emitted, calls the device, writes a load's
 * result into the destination register and resumes after the instruction.
 * Accesses are counted per (guest address, direction) for the frontier report.
 */
#ifndef NATIVE_CORE_MMIO_H
#define NATIVE_CORE_MMIO_H

#include "native/core/vm.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcMmioDev {
    const char *name;
    uint32_t base, size;
    /* off is relative to base; size is 1, 2, 4 or 8 bytes. */
    uint64_t (*read)(void *opaque, uint32_t off, unsigned size);
    void (*write)(void *opaque, uint32_t off, uint64_t value, unsigned size);
    void *opaque;
} NcMmioDev;

void nc_mmio_register(const NcMmioDev *dev);
/* Every device unregistered and the access log cleared (the core stopped). */
void nc_mmio_reset(void);

/* From a SIGSEGV/SIGBUS handler: handles the fault if it is a device access
 * the decoder knows. Returns false to let the caller report the fault. */
bool nc_mmio_fault(const NcVm *vm, const siginfo_t *info, void *ucontext);

/* Writes the busiest MMIO sites to fd. */
void nc_mmio_dump_stats(int fd);
/* A device access at guest physical/virtual addr (the windows are identity
 * mapped) without a fault: false if no device is there. */
bool nc_mmio_access(uint32_t addr, unsigned bytes, bool store, uint64_t *v);
/* A device's write handler, its store unable to complete yet (the APU's
 * voice locks): true if the guest's instruction runs again (nc_mmio_access's
 * stores, the x86mmio sites: interrupts are taken in between), the write
 * then undone; false, it must complete as it is. */
bool nc_mmio_retry(void);
/* After nc_mmio_access: the device asked for its store to run again. */
bool nc_mmio_take_retry(void);
/* Accesses emulated so far. */
uint64_t nc_mmio_total(void);
/* Host faults the core handled, by kind (core.c nc_core_fault; each costs a
 * signal): FEX's call-return stack, guest stores to pages write tracking
 * cleaned, device registers reached by a fault (not yet hooked sites), atomics
 * at an alignment the host can't do (emulated each time). */
extern uint64_t g_nc_faults_callret, g_nc_faults_dirty, g_nc_faults_mmio, g_nc_faults_unaligned;
/* debug: the guest EIPs of MMIO faults, sampled (1 in 64): " eip:n ..." of
 * the busiest since the last call, then cleared */
void nc_mmio_fault_sites(char *buf, size_t len);
/* ... the same as arrays: up to max of the busiest, their counts; cleared */
unsigned nc_mmio_fault_top(uint32_t *eips, uint32_t *counts, unsigned max);
/* debug: the guest RAM pages (PA >> 12) of write-tracking faults, sampled
 * (1 in 64), the busiest since the last call, with their counts; cleared
 * (native/hle/dirty.c) */
unsigned nc_hle_dirty_top(uint32_t *pages, uint32_t *counts, unsigned max);

#ifdef __cplusplus
}
#endif

#endif
