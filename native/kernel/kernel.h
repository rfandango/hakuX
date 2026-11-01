/*
 * hakuX native core — the Xbox kernel (xboxkrnl), reimplemented.
 *
 * Kernel memory lives at VA 0x80010000 (physical 0x10000), where the real
 * kernel image sits. Each function export has a trampoline there,
 *   int 0x3F ; dw ordinal
 * which reaches the core as a system op; the kernel reads the arguments for
 * the export's calling convention, runs the implementation and returns to the
 * caller as `ret` would. Data exports are variables in the same area. Until
 * a function is implemented, a call to it stops the guest and reports it
 * (the frontier).
 */
#ifndef NATIVE_KERNEL_KERNEL_H
#define NATIVE_KERNEL_KERNEL_H

#include "native/core/irq.h"
#include "native/core/vm.h"
#include "native/cpu/cpu.h"

#include "native/core/jmp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_KERNEL_VA     0x80010000u
#define NC_KERNEL_PA     0x00010000u
#define NC_KERNEL_SIZE   0x00040000u
#define NC_KCALL_VECTOR  0x3Fu
/* In the KPCR's page, past its fields (guest code reads it at fs:): 1 while
 * DPCs are queued (timer.c), for the guest-code KfLowerIrql (kernel.c). */
#define NC_KPCR_DPC_PENDING 0x800u
#define NC_KORDINAL_MAX  512u
#define NC_KORDINAL_THREAD_RETURN 0x3FFu  /* a thread's start routine returned */
#define NC_KORDINAL_DPC_RETURN    0x3FEu  /* a DPC routine returned */
#define NC_KORDINAL_CALLOUT_RETURN 0x3FDu /* a routine run by nc_kernel_callout returned */
#define NC_KORDINAL_ISR_RETURN    0x3FCu  /* an interrupt service routine returned */
#define NC_KORDINAL_APC_RETURN    0x3FBu  /* an APC routine returned */
#define NC_KORDINAL_TRAP          0x3FAu  /* an exception reached the kernel's IDT handler */
#define NC_KORDINAL_SEH_RETURN    0x3F9u  /* an exception handler returned (seh.c) */
#define NC_KORDINAL_SEH_NESTED    0x3F8u  /* the kernel's handler around a handler call */
#define NC_KORDINAL_SEH_COLLIDED  0x3F7u  /* the same, during an unwind */
#define NC_KORDINAL_APU_SPIN      0x3F5u  /* DirectSound waits on the GP DSP's mailbox (fixups.c) */

enum NcKAbi { NC_KABI_STDCALL, NC_KABI_FASTCALL, NC_KABI_CDECL, NC_KABI_DATA };

typedef struct NcKExport {
    uint16_t ordinal;
    const char *name;
    enum NcKAbi abi;
    uint8_t arg_bytes;
} NcKExport;

typedef struct NcKernel NcKernel;

/* A kernel function: args[] in declaration order; returns EDX:EAX. It may
 * block or end the calling thread (see ps.c) or stop the guest. */
typedef uint64_t (*NcKFn)(NcKernel *k, NcFrame *f, const uint32_t *args);

/* NTSTATUS values. */
#define NC_STATUS_SUCCESS             0x00000000u
#define NC_STATUS_TIMEOUT             0x00000102u
#define NC_STATUS_PENDING             0x00000103u
#define NC_STATUS_UNSUCCESSFUL        0xC0000001u
#define NC_STATUS_NOT_IMPLEMENTED     0xC0000002u
#define NC_STATUS_INVALID_HANDLE      0xC0000008u
#define NC_STATUS_INVALID_PARAMETER   0xC000000Du
#define NC_STATUS_NO_MEMORY           0xC0000017u
#define NC_STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define NC_STATUS_INSUFFICIENT_RESOURCES 0xC000009Au

/* Guest object types: the VA of the kernel's type-object data export. */
enum NcObType { NC_OB_THREAD, NC_OB_EVENT, NC_OB_SEMAPHORE, NC_OB_MUTANT, NC_OB_TIMER, NC_OB_FILE,
                NC_OB_DEVICE, NC_OB_SYMLINK, NC_OB_DIRECTORY, NC_OB_TYPES };

#define NC_MAX_THREADS 128
/* The Xbox kernel's handle table grows as it fills. Super Monkey Ball
 * Deluxe opens its disc's ~4100 files as it boots, most of them twice, and
 * keeps them open: at 4096 the rest failed, and a thread it created
 * suspended got no handle to be resumed by (its title movie never played). */
#define NC_MAX_HANDLES 16384
#define NC_HANDLES_V1 4096              /* the table a KERN v1 state holds */
#define NC_POOL_RANGES 8192

enum NcThreadState { NC_T_FREE, NC_T_READY, NC_T_RUNNING, NC_T_WAITING, NC_T_SUSPENDED,
                     NC_T_TERMINATED };

#define NC_MAX_WAIT_OBJECTS 64
#define NC_MAX_CALLOUTS 4

typedef struct NcWait {
    uint32_t objects[NC_MAX_WAIT_OBJECTS]; /* guest dispatcher objects */
    unsigned count;
    bool wait_all;
    uint64_t deadline_ns;    /* host monotonic; 0: no timeout */
    uint32_t cs;             /* RTL_CRITICAL_SECTION the thread takes when woken, or 0 */
    bool alertable;          /* an APC ends it (STATUS_USER_APC) */
} NcWait;

/* A queued APC (apc.c): a KAPC, or a bare normal routine with its arguments. */
typedef struct NcApc {
    uint32_t kapc;
    uint32_t normal, ctx, a1, a2;
} NcApc;

/* A thread's APCs running for an alertable wait (apc.c): the one being
 * delivered (out of the queue, as NT takes it) and where the wait returns. */
typedef struct NcApcChain {
    NcApc cur;
    bool has_cur;
    uint8_t stage;             /* cur's: 0 none run, 1 kernel routine, 2 normal routine */
    bool in_apc;
    uint32_t esp, eip;         /* where the wait returns once the APCs ran */
} NcApcChain;

#define NC_MAX_APC_NEST 4

/* An exception dispatch or unwind in progress (seh.c). */
typedef struct NcSeh {
    bool unwind;
    uint32_t base;           /* its guest frame, below the raising code's stack */
    uint32_t rec, ctx;       /* EXCEPTION_RECORD, CONTEXT */
    uint32_t reg;            /* the registration whose handler runs */
    uint32_t nested;         /* dispatch: the frame a nested raise came from */
    uint32_t target;         /* unwind: TargetFrame */
} NcSeh;
#define NC_MAX_SEH 8

typedef struct NcThread {
    enum NcThreadState state;
    uint32_t ethread;        /* guest ETHREAD (KTHREAD at offset 0) */
    uint32_t id;
    int priority;
    uint32_t stack_base, stack_limit;
    /* Under the FX save area at the stack's top (ps.c thread_create): the
     * TIB's StackBase (fs:[4], the TLS's top), and where the thread's frames
     * begin. 0 in a state saved before the save area: the stack's top. */
    uint32_t tib_base, frame_top;
    uint32_t exception_list; /* NT_TIB.ExceptionList while not running */
    NcWait wait;             /* while NC_T_WAITING */
    unsigned suspend;        /* suspend count: runnable only at 0 */
    /* Kernel calls finishing in a guest routine (nc_kernel_callout). */
    uint32_t callout_ret[NC_MAX_CALLOUTS], callout_esp[NC_MAX_CALLOUTS];
    uint8_t callout_irql[NC_MAX_CALLOUTS]; /* IRQL to restore on return, 0xFF: leave */
    unsigned ncallouts;
    /* APCs (apc.c): queued, and the chain running for an alertable wait.
     * Unbounded like NT's list: a title can have any number of ReadFileEx
     * completions outstanding (Forza's font streaming queues dozens). */
    NcApc *apcs;
    unsigned napcs, apc_cap;
    NcApcChain apc;          /* the chain running (apc.in_apc) */
    NcApcChain apc_outer[NC_MAX_APC_NEST]; /* chains an alertable wait in an APC interrupted */
    unsigned apc_nest;
    NcSeh seh[NC_MAX_SEH];
    unsigned nseh;
    bool fresh;              /* ctx was just set up (an ISR or DPC entry): the
                                running frame must not be saved over it */
    /* Scheduling (ps.c): threads of one priority run in the order they
     * became ready (ready_seq, 0: not yet ordered), each for its quantum,
     * which preemption pauses rather than restarts (slice_used_ns). */
    uint64_t ready_seq, run_seq;
    uint64_t slice_used_ns, slice_start_ns;
    bool slice_expired;
    NcCpuContext ctx;
} NcThread;

typedef struct NcHandle {
    uint32_t object;         /* guest VA of the object body, 0 if free */
    enum NcObType type;
} NcHandle;

typedef struct NcRange {
    uint32_t va, len;
} NcRange;

struct NcKernel {
    NcVm *vm;
    NcCpu *cpu;
    const struct NcDisc *disc;            /* the title's disc (fd < 0: the tray is empty) */
    const struct NcDiscFile *xbe_file;    /* the running XBE on it */
    int xbe_fd;                           /* or its host file (a hard disk partition's), else -1 */
    uint32_t xbe_base;
    uint32_t xbe_size;                    /* SizeOfImage */
    uint32_t game_region;                 /* from the XBE certificate: 1 NA, 2 JP, 4 elsewhere */
    uint32_t kprocess;                    /* the one KPROCESS, every thread's ApcState.Process */
    uint32_t tramp_va[NC_KORDINAL_MAX];
    uint32_t data_va[NC_KORDINAL_MAX];
    NcKFn impl[NC_KORDINAL_MAX];
    uint32_t stop_va;          /* HLT: leaves the JIT */
    uint32_t thread_return_va; /* return address of thread start routines */
    uint32_t dpc_return_va;    /* return address of DPC routines */
    uint32_t callout_return_va; /* return address of callout routines */
    uint32_t apc_return_va;    /* return address of APC routines */
    uint32_t idt_va;           /* the IDT SIDT reports: 256 interrupt gates */
    uint32_t trap_va;          /* every gate's handler until a title replaces it */
    uint32_t seh_return_va;    /* return address of exception handlers */
    uint32_t code_next;        /* kernel-area code allocator (nc_kernel_code_alloc) */
    uint64_t defer_replayed;   /* unused (in the KERN v1 savestate layout) */
    uint64_t apu_spins;        /* DirectSound mailbox waits (stats) */
    uint32_t seh_nested_va, seh_collided_va; /* the kernel's own handlers (seh.c) */
    uint32_t cr2;              /* the last page fault's address */
    uint64_t page_faults;      /* delivered to the title's handler */
    uint64_t vm_commits, vm_decommits; /* virtual memory pages (stats) */
    NcFrame *frame;            /* the frame of the kernel call being run */
    bool in_call;              /* inside a kernel function (fault reports use frame) */
    uint32_t call_pop;         /* stack bytes the kernel call being run pops */
    bool redirected;           /* it continues in a callout */
    uint32_t kpcr_va;
    uint32_t type_va[NC_OB_TYPES];

    /* Kernel pool (ExAllocatePool and kernel objects): first-fit free list. */
    uint32_t pool_va, pool_len;
    NcRange pool_free[NC_POOL_RANGES];
    unsigned pool_nfree;
    uint32_t pool_ex, pool_ob, pool_lost;   /* live ExAllocatePool bytes, object bytes,
                                               bytes a full free list dropped */

    /* the handle table, stored as its first NC_HANDLES_V1 slots (handles)
     * and, since KERN v2, the rest */
    union {
        NcHandle handle_table[NC_MAX_HANDLES];
        struct {
            NcHandle handles[NC_HANDLES_V1];
            NcHandle handles_more[NC_MAX_HANDLES - NC_HANDLES_V1];
        };
    };

    NcThread threads[NC_MAX_THREADS];
    unsigned threads_hi;     /* slots in use are all below this */
    NcThread *current;
    bool resched;              /* pick another thread before returning to the guest */
    uint32_t next_stack_va;
    /* stack slots given back (nc_ps_stack_free), first fit; not in a
     * savestate: a load starts with none */
    struct {
        uint32_t va, span;
    } stack_free[32];
    unsigned nstack_free;
    uint32_t next_thread_id;

    uint64_t timer_due_ns;     /* earliest inserted timer (host monotonic), 0 if none */
    uint64_t wait_due_ns;      /* earliest wait timeout of a waiting thread, 0 if none */
    /* Time slices (ps.c): a thread runs for the process quantum, then the
     * ready threads of its priority take their turns, as on the Xbox. An ISR
     * or a DPC neither ends nor restarts a slice. */
    NcThread *slice_thread;    /* whose slice runs; NULL: the next thread switched in starts one */
    uint64_t slice_due_ns;     /* when it ends (host monotonic), 0 if none */
    uint64_t ready_clock;      /* NcThread.ready_seq's clock */
    NcThread *dpc_thread;      /* runs DPC routines; created on first use */
    bool in_dpc;
    uint8_t dpc_saved_irql;

    /* Interrupts (intr.c). */
    uint32_t irq_obj[NC_IRQ_LINES]; /* connected KINTERRUPT per bus level */
    uint32_t isr_return_va;
    NcThread *isr_thread;      /* runs ISRs; created on first use */
    bool in_isr;
    uint8_t isr_saved_irql;
    NcThread *isr_interrupted; /* the thread an ISR interrupted */
    NcThread *resume_first;    /* resumes before anything else (was above DISPATCH_LEVEL) */
    bool force_switch;         /* switch threads even though the IRQL is raised (ISR entry) */
    uint64_t interrupts;

    /* The HLE GPU layer (native/hle), when present: NC_SYSOP_HOOK (its debug
     * probes), and work it queues for the end of a system op. */
    void (*hle_hook)(NcKernel *k, NcFrame *f);
    void (*hle_after_sysop)(NcKernel *k);
    /* memory kept for the next title (nc_mm_keep_save): its GPU images go
     * back into guest RAM; the wait is until they are there (false: not in
     * timeout_ns) */
    void (*hle_keep)(uint32_t pa, uint32_t len);
    bool (*hle_keep_wait)(uint64_t timeout_ns);

    bool stop;
    char stop_reason[512];
    bool relaunch;             /* stopped for a quick reboot into another XBE (relaunch.bin) */
    bool quit;                 /* stopped as the title left: to the dashboard, a reboot, a shutdown */
    char image_path[128];      /* XeImageFileName ("" = \Device\CdRom0\default.xbe) */
    uint64_t calls;
    unsigned first_ordinal;    /* first kernel call made (0: none yet) */
};

bool nc_kernel_init(NcKernel *k, NcVm *vm);
/* Before nc_kernel_init: the boot is for a savestate whose process object is
 * at kprocess (0: none), and the kernel pool's first range goes where the
 * boot that made it put it, under that object (kernel.c POOL_BASE). */
void nc_kernel_pool_for(uint32_t kprocess);
/* The process object's address in a savestate's kernel section (0: none, or
 * the state can't be read). */
uint32_t nc_kern_state_kprocess(const char *path);

/* Creates the title's first thread at entry; run it with nc_cpu_run. */
NcThread *nc_kernel_boot_thread(NcKernel *k, NcCpu *cpu, uint32_t entry, uint32_t stack_size);

const NcKExport *nc_kernel_export(unsigned ordinal);

/* --- internal: kernel pool, objects, threads (pool.c, ob.c, ps.c) --- */

/* Installs the implementation of the function export called name. */
void nc_kernel_impl(NcKernel *k, const char *name, NcKFn fn);
void nc_pool_init(NcKernel *k, uint32_t va, uint32_t len);
uint32_t nc_pool_alloc(NcKernel *k, uint32_t size);
void nc_pool_free(NcKernel *k, uint32_t va, uint32_t size);
/* mm.c: physical memory held by contiguous allocations and by committed
 * virtual memory (bytes), and the number of contiguous blocks. */
void nc_mm_usage(NcKernel *k, uint32_t *contig_bytes, uint32_t *virtual_bytes, unsigned *contig_blocks);
/* mm.c: frees the contiguous blocks that code in [lo, hi) allocated (a mod's
 * buffers once its code is gone); returns their bytes. */
uint32_t nc_mm_contig_free_callers(NcKernel *k, uint32_t lo, uint32_t hi);
/* paging.c: the page directory CR3 names (allocated on first use), and
 * applying what the title wrote to it after INVLPG or a CR3 reload. */
uint32_t nc_paging_cr3(NcKernel *k);
void nc_paging_flush(NcKernel *k);
/* A guest access to va faulted (from the host fault handler): true if the
 * title's page tables map it (now applied; the access can run again). */
bool nc_paging_fault(NcKernel *k, uint32_t va);
/* " ret ret ...": up to six return addresses up the guest's EBP frames. */
void nc_kernel_frame_chain(NcKernel *k, NcFrame *f, char *buf, size_t len);

/* Allocates an object (OBJECT_HEADER + zeroed body) and returns the body VA. */
/* ObfDereferenceObject: an object no one holds any more is freed (ob.c). */
void nc_ob_dereference(NcKernel *k, uint32_t object);
uint32_t nc_ob_create(NcKernel *k, enum NcObType type, uint32_t body_size);
uint32_t nc_ob_handle(NcKernel *k, uint32_t object, enum NcObType type);
uint32_t nc_ob_lookup(NcKernel *k, uint32_t handle, enum NcObType *type);
bool nc_ob_close(NcKernel *k, uint32_t handle);
void nc_ob_register(NcKernel *k);

void nc_ps_register(NcKernel *k);
NcThread *nc_ps_thread_of(NcKernel *k, uint32_t ethread);
/* Saves the running thread and loads the next ready one into the frame. */
void nc_ps_schedule(NcKernel *k, NcFrame *f);
/* The running thread's slice is due (clock_tick): a ready thread of its
 * priority or above takes over, or it gets another. */
void nc_ps_slice_end(NcKernel *k, uint64_t now);
/* The running thread gives way (Sleep(0), polling) to a ready thread of its
 * priority or above: rescheduled at the back of its priority's queue. */
void nc_ps_yield(NcKernel *k);
/* debug (debug.hakux.ncresume=<id>): NtResumeThread on thread t<id> */
void nc_ps_debug_resume(NcKernel *k, unsigned id);
void nc_ps_terminate_current(NcKernel *k, uint32_t status);

bool nc_ps_stack_alloc(NcKernel *k, uint32_t size, uint32_t *base);
/* The stack at base (size bytes, its pages gone) gives its slot back. */
void nc_ps_stack_free(NcKernel *k, uint32_t base, uint32_t size);
/* A kernel-owned thread, suspended, with no start context yet. */
NcThread *nc_ps_kernel_thread(NcKernel *k, uint32_t stack_size, int priority);

/* After a state's load: a page over the stacks of threads from a state saved
 * before the FX save area (their frames sit nearer the top). */
void nc_ps_old_stacks(NcKernel *k);

/* Where a thread's frames begin: under its FX save area and TLS (its stack's
 * top in a state saved before the save area). */
static inline uint32_t nc_ps_frame_top(const NcThread *t)
{
    return t->frame_top ? t->frame_top : t->stack_base;
}

/* mm.c, ex.c, rtl.c */
void nc_mm_register(NcKernel *k);
void nc_mm_note_image(uint32_t base, uint32_t size);
void nc_ex_register(NcKernel *k);
void nc_rtl_register(NcKernel *k);
/* Room for len bytes of guest code in the kernel area (stubs), or 0. */
uint32_t nc_kernel_code_alloc(NcKernel *k, uint32_t len);

/* fixups.c: before the title runs, its library code that spins on emulated
 * hardware is pointed at the kernel (DirectSound's GP mailbox wait). */
struct HleXbeInfo;
void nc_kernel_title_fixups(NcKernel *k, const struct HleXbeInfo *xbe);
/* ... and the library code a section holds (XMV's stream wait), at boot and
 * as XeLoadSection loads one */
void nc_kernel_section_fixups(NcKernel *k, uint32_t va, uint32_t len, const char *name);
/* ... and over the sections a savestate's load brought (saved before a fixup) */
void nc_kernel_loaded_fixups(NcKernel *k);

/* ex.c: the console's video settings, as the dashboard keeps them: the
 * EEPROM's video flags (XC_VIDEO_FLAGS: NC_VIDEO_*) and the AV pack the
 * system controller and the TV encoder report. Before the title starts;
 * flags < 0 keeps the stored ones. */
#define NC_VIDEO_WIDESCREEN 0x00010000u
#define NC_VIDEO_720P       0x00020000u
#define NC_VIDEO_1080I      0x00040000u
#define NC_VIDEO_480P       0x00080000u
#define NC_VIDEO_LETTERBOX  0x00100000u
#define NC_VIDEO_PAL60      0x00400000u
enum { NC_AV_PACK_HDTV, NC_AV_PACK_STANDARD, NC_AV_PACK_SVIDEO, NC_AV_PACK_SCART, NC_AV_PACK_VGA };
void nc_kernel_set_video(int64_t flags, int av_pack);

/* ex.c: the console's language, the EEPROM's XC_LANGUAGE that XGetLanguage
 * returns: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian,
 * 7 Korean, 8 Chinese (traditional), 9 Portuguese. Before the title starts;
 * any other value keeps the stored one. */
void nc_kernel_set_language(int language);

/* The console's clock and time zone, before the title starts. ke.c: its
 * time at boot is the phone's (CLOCK_REALTIME) moved by offset_ms, the
 * distance the settings set it from the phone's (0: the phone's time); it
 * runs with the guest's time from there, and a savestate goes on from its
 * own. ex.c: the EEPROM's time zone, which XAPI's local time comes from: the
 * phone's offset from UTC, minutes east. */
void nc_kernel_set_clock(int64_t offset_ms);
void nc_kernel_set_time_zone(int minutes_east);
void nc_kernel_apu_spin(NcKernel *k, NcFrame *f);

/* seh.c: RtlRaiseException and RtlUnwind, and their stubs. */
void nc_seh_register(NcKernel *k);
void nc_seh_stub(NcKernel *k, NcFrame *f, unsigned ordinal);
/* A guest access to va faulted in translated code (f is the guest state
 * there), and the title has no page-fault handler of its own. Dispatches
 * STATUS_ACCESS_VIOLATION to the thread's SEH handlers and returns true.
 * Returns false if the thread has no handlers, no stack room for the
 * dispatch, or the dispatch can't be set up; the fault then crashes the
 * guest. */
bool nc_seh_access_violation(NcKernel *k, NcFrame *f, uint32_t va, bool write);
/* After the XBE is loaded: what the kernel exposes about the title. xbe_fd:
 * the XBE's host file when it isn't on the disc (else -1). */
void nc_kernel_title_loaded(NcKernel *k, const struct NcDisc *disc, const struct NcDiscFile *xbe, int xbe_fd,
                            uint32_t xbe_base, uint32_t image_size);
/* ex.c: XboxHDKey, the key saves locked to the console are signed with: the
 * HD key in the EEPROM's encrypted section, or, for an image from before
 * that kept it in the clear (zero), the EEPROM's bytes 0x36-0x45 (its serial
 * number's last ten digits and its MAC address). It was zero then, and XAPI
 * keyed those saves with these bytes (newer XAPI's fallback for a zero HD
 * key: Forza 0x266ad8) or with the zero key (older XAPI: Halo 0x15b75), a
 * key per XDK; set, every title uses it. An image given an identity of its
 * own keeps these bytes as its HD key, and the app re-signs a console's
 * saves to it (SaveResigner). Set at each title's start, and after a state
 * load (the state holds its run's kernel memory). */
void nc_kernel_set_hd_key(NcKernel *k);

/* XLaunchNewImage: a title quick-reboots (HalReturnToFirmware 2) into another
 * XBE with a launch data page. The page goes to the file at nc_kernel_relaunch_file;
 * the next boot reads it, loads the XBE it names and gives the title the page. */
#define NC_LAUNCH_PAGE 0x1000u

/* set while the vCPU runs a system op (a kernel call, an HLE hook) */
extern volatile int g_nc_in_sysop;
void nc_kernel_relaunch_file(char *out, size_t n);
/* The launch page's XBE as a device path: on the disc ("\Device\CdRom0\morrowind.xbe")
 * or a hard disk partition ("\Device\Harddisk0\Partition2\xboxdash.xbe"); false
 * when it names neither. */
bool nc_kernel_launch_xbe(const uint8_t page[NC_LAUNCH_PAGE], char *out, size_t n);
/* The dashboard, booted when a title asks for one of its pages (Xbox Live's
 * account sign-up, its network settings) or by itself (the app's Dashboard). */
#define NC_DASHBOARD_PATH "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe"
/* After title_loaded on a relaunch: a contiguous page with page's contents
 * becomes LaunchDataPage. */
void nc_kernel_launch_page_restore(NcKernel *k, const uint8_t page[NC_LAUNCH_PAGE]);
uint32_t nc_mm_contig_alloc(NcKernel *k, uint32_t size);

/* ke.c: time, dispatcher objects and waits. */
void nc_ke_register(NcKernel *k);
/* KeSetEvent on a guest event object; returns the previous state. */
uint32_t nc_ke_set_event(NcKernel *k, uint32_t event);
/* KeClearEvent. */
void nc_ke_clear_event(NcKernel *k, uint32_t event);
/* KeInitializeEvent: type 0 notification, 1 synchronization. */
void nc_ke_init_event(NcKernel *k, uint32_t event, unsigned type, bool signaled);
/* The running thread waits for the host's work for its call (no object, no
 * timeout); nc_ke_wake ends it with the call's status. */
void nc_ke_block(NcKernel *k);
void nc_ke_wake(NcKernel *k, NcThread *t, uint32_t status);

/* FILE_OBJECT.Event (a KEVENT): what a wait on a file handle waits for, set
 * when an I/O on the file issued without an event of its own completes. */
#define NC_FO_EVENT 0x38u

uint64_t nc_ke_now_ns(void);
/* nc_kernel_sysop, on the guest's thread: an entry begins and ends; within
 * it nc_ke_now_ns reads the clock once (nc_ke_now_ns in ke.c). Stale: a wait
 * blocked, the next read is fresh. */
void nc_ke_time_enter(void);
void nc_ke_time_leave(void);
void nc_ke_time_stale(void);
/* The guest's CPU, pausing and resuming (nc_kernel_pause_point): the guest's
 * time stands still between the two; _end returns how long that was (ns). */
void nc_ke_pause_begin(void);
uint64_t nc_ke_pause_end(void);
/* The guest's CPU held by the host on its own thread (hle_host_guest_hold):
 * the guest's time goes on for the hold's first 8 ms, then stands still
 * till its end, as through a pause, the devices going on. _begin: false if
 * the time stood still already (paused), the hold nothing to do. _end
 * returns the time left out (ns), *held_ns how long it held. nc_ke_held:
 * the holds since the last call, the time they left out and the longest
 * (ns). */
bool nc_ke_hold_begin(void);
uint64_t nc_ke_hold_end(uint64_t *held_ns);
unsigned nc_ke_held(uint64_t *out_ns, uint64_t *max_ns);
/* A time in nc_ke_now_ns's terms on the host's CLOCK_MONOTONIC (a timed
 * wait's deadline): later by the time the emulation was paused or held;
 * while the time stands still, as if it went on now. */
uint64_t nc_ke_host_ns(uint64_t guest_ns);
/* The activity paused or resumed (NativeCoreActivity): see nc_pause_set. */
void nc_kernel_set_paused(NcKernel *k, bool on);
/* The guest's CPU in a kernel entry: blocks while the emulation is paused,
 * its time (kernel and RDTSC) and the hitch monitor stopped meanwhile. */
void nc_kernel_pause_point(NcKernel *k, NcFrame *f, bool idle);
/* The host cycle counter when the current pause began (RDTSC stood still
 * from there). */
uint64_t nc_kernel_pause_cycles(void);
/* debug: an x86mmio site's state: 1 hooked, -n dropped (n strikes), 0 none */
int nc_x86mmio_site_state(uint32_t eip);
/* Now as NT times: system time (FILETIME) and interrupt time (since boot), 100 ns. */
uint64_t nc_ke_system_time(void);
uint64_t nc_ke_interrupt_time(void);
/* Memory kept across a quick reboot (MmPersistContiguousMemory, mm.c): at the
 * reboot the kept ranges and the saved display address go to a file for the
 * next boot; nc_mm_keep_restore (that boot, before the title loads) puts them
 * back, allocated, and returns the saved display address (0: none);
 * nc_mm_kept lists the kept ranges' physical addresses and sizes. */
void nc_mm_keep_save(NcKernel *k, uint32_t saved);
uint32_t nc_mm_keep_restore(NcKernel *k);
/* The kept range at va kept no more: the launch data page XLaunchNewImage
 * persists goes to the next boot in the relaunch file (a new page there);
 * kept as well, its old page stayed allocated mid-memory relaunch after
 * relaunch, and Forza's updated XBE found no 30 MB contiguous run. */
void nc_mm_keep_drop(uint32_t va);
unsigned nc_mm_kept(uint32_t *pa, uint32_t *len, unsigned max);
/* AvSetSavedDataAddress's address, as the last title left it (a relaunch). */
void nc_kernel_av_saved_set(uint32_t va);
uint32_t nc_kernel_av_saved(void);

/* --- savestates (native/state; kstate.c) --- */
/* ke.c: the guest's time (its boot, NtSetSystemTime's offset, now), and
 * going on from a saved now. */
void nc_ke_state(uint64_t *boot_ns, uint64_t *time_offset, uint64_t *guest_ns);
void nc_ke_state_set(uint64_t boot_ns, uint64_t time_offset, uint64_t guest_ns);
/* intr.c: the line whose ISR runs (0xFF: none) */
uint8_t nc_intr_isr_line(void);
void nc_intr_isr_line_set(uint8_t line);
/* io.c: before a save, paused: the disc reads in flight complete (NULL), or
 * why the state can't be taken now. */
const char *nc_io_state_drain(NcKernel *k, uint64_t timeout_ns);
/* io.c: the cache partitions (3-5, bit n) the title uses: its utility
 * drive's and any it has a file open on. */
unsigned nc_io_cache_partitions(void);
/* kstate.c: the running thread's live frame, or (idle: none runs) the
 * scheduler's turn, as a savestate takes them; and after a load, the
 * context the guest CPU starts from. */
void nc_kstate_capture_frame(NcKernel *k, const NcFrame *f, bool idle);
const NcCpuContext *nc_kstate_start_context(NcKernel *k);
/* Refreshes KeTickCount, KeInterruptTime and KeSystemTime. */
void nc_ke_update_time(NcKernel *k);
/* The same at a kernel entry, from the published time (no clock read) when
 * there is one. */
void nc_ke_tick_time(NcKernel *k);
/* The clock thread's (about every millisecond): the guest's time now. */
void nc_ke_publish_time(uint64_t guest_ns);
/* That time, 0 if none or stale. */
uint64_t nc_ke_published_ns(void);
/* How many refreshes so far (any thread may read it: the clock's tick). */
uint32_t nc_ke_time_refreshes(void);
/* Wakes waiting threads whose waits are satisfied or timed out. Returns the
 * earliest remaining timeout (host ns), or 0 if none. */
uint64_t nc_ke_check_waits(NcKernel *k);

/* timer.c: timers and DPCs. */
void nc_timer_register(NcKernel *k);
/* Fires expired timers; returns the next due time (host ns) or 0. */
uint64_t nc_timer_poll(NcKernel *k, bool *fired);
/* Queues a DPC with its system arguments; false if it was already queued. */
bool nc_dpc_queue(NcKernel *k, uint32_t dpc, uint32_t arg1, uint32_t arg2);
bool nc_dpc_pending(void);
/* The queue's state where guest code reads it (NC_KPCR_DPC_PENDING). */
void nc_dpc_sync(NcKernel *k);
/* Readies the DPC thread for the next queued DPC (scheduler, below DISPATCH_LEVEL). */
void nc_dpc_dispatch(NcKernel *k);
/* A DPC routine returned to the stub. */
void nc_dpc_return(NcKernel *k);

/* io.c: files, devices and the object namespace. */
void nc_io_register(NcKernel *k);
/* xc.c: crypto exports. */
void nc_xc_register(NcKernel *k);
/* Where the hard disk's partitions live on the host (PartitionN under it). */
void nc_io_set_hdd_root(const char *path);
const char *nc_io_hdd_root(void);
/* A file on a hard disk partition ("\Device\Harddisk0\Partition2\xboxdash.xbe",
 * names matched as FATX does): its host path in out; false when the path
 * isn't on a partition or names no file. */
bool nc_io_host_file(const char *xbox_path, char *out, size_t n);
/* \??\D:, the directory the title was launched from (its XBE's). */
void nc_io_set_launch_dir(const char *dir);
/* Console timing: reads take the console's drives' time, their thread waiting (io.c). */
void nc_io_set_device_time(bool on);
/* A file object's handle closed (ob.c). */
void nc_io_close(NcKernel *k, uint32_t object);
/* Completes the overlapped disc reads the reader thread finished (cheap when
 * none did): on every kernel entry and in the scheduler's loop. */
void nc_io_poll(NcKernel *k);

/* What a kernel thunk-table entry for ordinal resolves to, or 0. */
uint32_t nc_kernel_resolve(const NcKernel *k, unsigned ordinal);
/* Whether ordinal is a function export this kernel has (a call to one it
 * hasn't stops the guest). */
bool nc_kernel_implemented(const NcKernel *k, unsigned ordinal);

/* From inside a kernel function: the call returns by first running the guest
 * routine(args...), then returning to its caller with the routine's EAX. */
void nc_kernel_callout(NcKernel *k, NcFrame *f, uint32_t routine, const uint32_t *args, unsigned nargs);
/* The same, with the IRQL at irql while the routine runs. */
void nc_kernel_callout_at_irql(NcKernel *k, NcFrame *f, uint32_t routine, const uint32_t *args, unsigned nargs,
                               uint8_t irql);

/* apc.c: asynchronous procedure calls. */
void nc_apc_register(NcKernel *k);
bool nc_apc_pending(const NcThread *t);
/* Queues an APC to t; one waiting alertably wakes to run it. */
bool nc_apc_queue(NcKernel *k, NcThread *t, const NcApc *a);
/* From an alertable wait of the running thread: run its APCs first, then
 * continue at final_eip/final_esp with STATUS_USER_APC. */
bool nc_apc_begin_frame(NcKernel *k, NcFrame *f, NcThread *t, uint32_t final_esp, uint32_t final_eip);
void nc_apc_return(NcKernel *k, NcFrame *f);
/* rtl.c: RtlNtStatusToDosError. */
uint32_t nc_rtl_status_to_dos(uint32_t status);

/* intr.c: interrupt objects and delivery. */
void nc_intr_register(NcKernel *k);
/* Delivers the highest-priority deliverable asserted line (readies the ISR
 * thread and asks for a switch). cpu_enabled: the running code has IF set. */
bool nc_intr_dispatch(NcKernel *k, bool cpu_enabled);
/* An ISR returned to the stub. */
void nc_intr_return(NcKernel *k);

/* Writes the last kernel calls to fd (fault reports; async-signal tolerant). */
void nc_kernel_dump_recent(int fd);
/* Every guest thread: state, wait objects, top of stack (ps.c). */
void nc_kernel_dump_threads(NcKernel *k, int fd);

/* A guest access to va, a page the guest doesn't have (the host fault in
 * translated code, f its state there): with a page-fault handler of the
 * title's in the IDT (Fable's pager), #PF goes to it as the CPU raises it
 * and true returns; otherwise the fault is the guest's crash. */
bool nc_kernel_page_fault(NcKernel *k, NcFrame *f, uint32_t va, bool write);
/* In a kernel call, before the kernel reads (or, write, fills) the guest
 * buffer [va, va+len) with host I/O, which fails rather than faults on a
 * page the guest doesn't have: such a page is paged in as for the kernel's
 * own access on the Xbox. The title's tables are walked; failing that, #PF
 * goes to the title's handler with the call's trampoline as the faulting
 * instruction, so the call runs again once the page is in. False then: the
 * call returns at once, before any effect. */
bool nc_kernel_resident(NcKernel *k, uint32_t va, uint32_t len, bool write);
/* A kernel call can touch a page the title pages in itself, as the code it
 * stands for would have, faulting into the title's handler. When the title
 * has a page-fault handler of its own, the call runs in a fault scope: on
 * that fault, nc_kernel_call_fault (from the host fault handler) jumps out
 * of the call, which raises #PF at the guest instruction that entered it,
 * so it runs again once the page is in, as QEMU restarts a helper's guest
 * instruction. Otherwise nc_kernel_call_fault returns. */
void nc_kernel_call_fault(NcKernel *k, uint32_t va, bool write);

/* x86mmio.c: device accesses from translated code without host faults. The
 * fault handler notes the guest instruction of an emulated device access
 * (note, async-signal safe); the next system op makes it a hooked address
 * (flush); is_site answers the JIT's hook query; hook emulates it. */
void nc_x86mmio_note(uint32_t eip);
void nc_x86mmio_flush(NcKernel *k);
bool nc_x86mmio_is_site(uint32_t eip);
bool nc_x86mmio_hook(NcKernel *k, NcFrame *f, uint32_t eip);

/* The CPU's system-op callback (opaque = NcKernel). */
bool nc_kernel_sysop(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size);

/* Stops the guest at the next return from a system op. */
void nc_kernel_request_stop(NcKernel *k, NcFrame *f, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The guest clock (kernel.c): stops the running guest when the next timer
 * or wait timeout is due, so it fires on time even if the running thread
 * never enters the kernel. Call after timer_due_ns, wait_due_ns or
 * slice_due_ns changes. */
void nc_kernel_clock_arm(NcKernel *k);

/* Scheduler rates for the statistics line (kernel.c). */
typedef struct NcSchedStats {
    uint64_t schedules, switches, idles, slices; /* slices: ended with another thread to run */
    uint64_t dpc_wait_max_ns, dpc_run_max_ns, dpc_slow; /* DPCs: queued to run, running; over 8 ms */
    uint32_t isr[16]; /* ISRs delivered per bus level */
} NcSchedStats;
extern NcSchedStats g_nc_sched;

/* An event (an opened path, ...): if it contains NC_TRACE_KCALL_AT, the next
 * NC_TRACE_KCALL (default 2000) kernel calls are traced. */
void nc_kernel_trace_trigger(const char *what);

#ifdef __cplusplus
}
#endif

#endif
