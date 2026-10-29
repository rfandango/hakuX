/*
 * hakuX native core — the guest CPU (FEXCore, fork branch hakuX).
 *
 * C interface to the JIT for the rest of the core. One CPU runs the guest;
 * privileged and system instructions come back through a system-op callback
 * (FEX system mode), with the guest frame available through the accessors.
 */
#ifndef NATIVE_CPU_CPU_H
#define NATIVE_CPU_CPU_H

#include "native/core/nc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same order as x86 encodings and FEX's register file. */
enum { NC_EAX, NC_ECX, NC_EDX, NC_EBX, NC_ESP, NC_EBP, NC_ESI, NC_EDI };

/* Mirrors FEXCore::Core::SystemOpKind (checked in cpu.cpp). */
enum NcSysOp {
    NC_SYSOP_NONE = 0,
    NC_SYSOP_IN,
    NC_SYSOP_OUT,
    NC_SYSOP_INS,
    NC_SYSOP_OUTS,
    NC_SYSOP_CLI,
    NC_SYSOP_STI,
    NC_SYSOP_HLT,
    NC_SYSOP_INT,
    NC_SYSOP_INTO,
    NC_SYSOP_INT1,
    NC_SYSOP_INT3,
    NC_SYSOP_UD2,
    NC_SYSOP_RDMSR,
    NC_SYSOP_WRMSR,
    NC_SYSOP_RDPMC,
    NC_SYSOP_WBINVD,
    NC_SYSOP_INVD,
    NC_SYSOP_INVLPG,
    NC_SYSOP_CLTS,
    NC_SYSOP_MOVCR,
    NC_SYSOP_MOVDR,
    NC_SYSOP_LGDT,
    NC_SYSOP_LIDT,
    NC_SYSOP_LLDT,
    NC_SYSOP_LTR,
    NC_SYSOP_LMSW,
    NC_SYSOP_CPUID,
    NC_SYSOP_OTHER,
    /* Not an instruction: an interrupt was requested (nc_cpu_request_interrupt);
     * EIP is the first instruction of the guest block about to run, size 0. */
    NC_SYSOP_INTERRUPT,
    /* Not an instruction: EIP is a hooked address (NcCpuConfig.hook_query)
     * about to run, size 0. To run it instead, nc_frame_bypass_hook. */
    NC_SYSOP_HOOK,
};

typedef struct NcCpu NcCpu;
typedef struct NcFrame NcFrame; /* the guest register file while in a system op */

/* A guest thread's architectural CPU state while it isn't running (opaque;
 * big enough for the JIT's register file). */
typedef struct NcCpuContext {
    uint64_t blob[256];
} NcCpuContext;

/* A context's architectural state as a savestate stores it (the JIT's own
 * layout stays private): GPRs, flags, x87 / MMX, SSE, segments. */
typedef struct NcCpuState {
    uint64_t rip;
    uint64_t gregs[16];
    uint32_t pf_raw, af_raw;
    uint8_t flags[48];
    uint64_t xmm[16][2];
    uint64_t mm[8][2];
    uint32_t mxcsr;
    uint16_t fcw;
    uint8_t ftw;
    uint16_t es_idx, cs_idx, ss_idx, ds_idx, gs_idx, fs_idx;
    uint32_t es_cached, cs_cached, ss_cached, ds_cached;
    uint64_t gs_cached, fs_cached;
} NcCpuState;

void nc_context_export(const NcCpuContext *c, NcCpuState *s);
void nc_context_import(NcCpuContext *c, const NcCpuState *s);

/* Context switches happen inside system ops: save the running thread's state
 * from the frame, load another's into it. */
void nc_frame_save(const NcFrame *f, NcCpuContext *c);
void nc_frame_load(NcFrame *f, const NcCpuContext *c);

uint32_t nc_frame_gpr(const NcFrame *f, int reg);
void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v);
uint32_t nc_frame_eip(const NcFrame *f);
void nc_frame_set_eip(NcFrame *f, uint32_t eip);
bool nc_frame_if(const NcFrame *f);
void nc_frame_set_if(NcFrame *f, bool on);

/* Called for every system op with the guest stopped at the instruction (EIP
 * is the instruction, size its length). Returns false if it can't handle it. */
typedef bool (*NcSysOpFn)(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size);

typedef struct NcCpuConfig {
    uint8_t *membase;   /* host address of guest VA 0 */
    NcSysOpFn sysop;
    void *opaque;
    /* Hooked guest addresses, asked at translation time (optional). Answers
     * must stay put for translated code (nc_cpu_invalidate when they change). */
    bool (*hook_query)(void *opaque, uint32_t eip);
    uint32_t idt_base; /* what SIDT reports */
    uint16_t idt_limit;
} NcCpuConfig;

/* Creates the JIT and its one host thread state: flat segments, FS at
 * fs_base (the KPCR, shared by all guest threads). */
NcCpu *nc_cpu_create(const NcCpuConfig *cfg, uint32_t fs_base);
/* After nc_cpu_run has returned: the JIT's thread and context freed (FEX's
 * DestroyThread and the context), the call-ret stack unmapped and FEX's
 * configuration reset, for a later nc_cpu_create. */
void nc_cpu_destroy(NcCpu *cpu);
/* At the end of a thread that ran the CPU (nc_cpu_create, nc_cpu_run): its
 * FEX allocator heap given back for the next thread to reuse. */
void nc_cpu_thread_done(void);

/* A new guest thread's initial state: the JIT's reset state (EFLAGS 0x202,
 * default x87 control word and MXCSR) at eip with esp. */
void nc_context_init(NcCpu *cpu, NcCpuContext *c, uint32_t eip, uint32_t esp);
void nc_context_set_gpr(NcCpuContext *c, int reg, uint32_t v);
uint32_t nc_context_gpr(const NcCpuContext *c, int reg);
uint32_t nc_context_eip(const NcCpuContext *c);
void nc_context_set_eip(NcCpuContext *c, uint32_t eip);

/* Runs the guest from context c until it executes HLT (the core's stop stub). */
void nc_cpu_run(NcCpu *cpu, const NcCpuContext *c);

/* Drops translations of guest code in [va, va+len) (it was replaced). */
void nc_cpu_invalidate(NcCpu *cpu, uint32_t va, uint32_t len);

/* At the end of a system op: the first time the running thread's x87 control
 * word selects single precision, FEX's reduced x87 starts rounding to it
 * (translations made from now on) and every translation so far is dropped;
 * true that once. Titles that never select it keep the plain reduced x87. */
bool nc_cpu_x87_single_check(NcCpu *cpu);

/* From any host thread: the guest leaves with NC_SYSOP_INTERRUPT at the start
 * of its next block (or the next system op sees it pending). */
void nc_cpu_request_interrupt(NcCpu *cpu);
/* The guest CPU's thread: its RDTSC leaves out ticks more host counter ticks
 * (the time the emulation was paused). */
void nc_cpu_cycle_offset_add(NcCpu *cpu, uint64_t ticks);
/* The host cycle counter (CNTVCT), the ticks nc_cpu_cycle_offset_add takes. */
uint64_t nc_cpu_host_cycles(void);
/* The guest's RDTSC count in host counter ticks (the host's less the time
 * left out), and setting it (a savestate's: the count goes on from there). */
uint64_t nc_cpu_guest_cycles(NcCpu *cpu);
void nc_cpu_guest_cycles_set(NcCpu *cpu, uint64_t ticks);
/* Whether a thread selected x87 single precision (nc_cpu_x87_single_check),
 * and selecting it again in a new run (a savestate's). */
bool nc_cpu_x87_single(NcCpu *cpu);
void nc_cpu_x87_single_restore(NcCpu *cpu);
/* Inside a system op: the request was taken. */
void nc_frame_clear_interrupt(NcFrame *f);
/* Inside an NC_SYSOP_HOOK: the hooked instruction at EIP runs next time. */
void nc_frame_bypass_hook(NcFrame *f);
/* The frame's CS selector. */
uint16_t nc_frame_cs(const NcFrame *f);
/* The frame's flags as EFLAGS (for host code that wants them whole). */
uint32_t nc_frame_eflags(NcFrame *f);
void nc_frame_set_eflags(NcFrame *f, uint32_t eflags);
/* The frame's GPRs (x86 order, low 32 bits used) and EIP, for code that
 * moves all of them at once (the HLE hook glue). */
uint64_t *nc_frame_regs(NcFrame *f);
uint64_t *nc_frame_rip(NcFrame *f);

/* Guest EIP for a host PC inside JIT code (fault reports), or 0. */
uint32_t nc_cpu_guest_eip_at(NcCpu *cpu, uintptr_t host_pc);
/* For a sampling profiler (a signal handler on the vCPU thread; lock-free):
 * true when host_pc is in JIT code, *eip then the guest instruction it runs;
 * false in host code (the kernel, a hook, the JIT compiler), *eip the last
 * synced RIP. */
bool nc_cpu_sample_pc(NcCpu *cpu, uintptr_t host_pc, uint32_t *eip);

/* A host fault (ucontext a ucontext_t *) in translated guest code, as for an
 * exception the guest takes: the guest state at the faulting instruction is
 * written back to the frame (EIP is that instruction) and returned, or NULL
 * when the fault isn't in translated code. The frame may then be changed
 * (an exception frame pushed, EIP at a handler) and nc_cpu_fault_resume
 * makes the guest run from it when the signal handler returns. */
NcFrame *nc_cpu_fault_frame(NcCpu *cpu, void *ucontext);
void nc_cpu_fault_resume(NcCpu *cpu, void *ucontext);
/* The faulting access was a write (a data abort's WnR). */
bool nc_cpu_fault_is_write(const void *ucontext);

/* A SIGBUS (si_code code) in translated guest code: an atomic access the host
 * can't make at that alignment (a locked x86 instruction at an address its
 * size doesn't divide), done by FEX's unaligned-access handler, which moves
 * the faulting context's PC past it. True if handled. */
bool nc_cpu_unaligned_fault(NcCpu *cpu, int code, void *ucontext);

/* A fault in the call-ret shadow stack's guard pages (unbalanced guest
 * CALL/RET, e.g. across guest thread switches): resets its pointer in the
 * faulting context (a ucontext_t *) as FEX's front ends do. True if handled. */
bool nc_cpu_callret_fault(NcCpu *cpu, uintptr_t fault_addr, void *ucontext);

#ifdef __cplusplus
}
#endif

#endif
