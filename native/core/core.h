/*
 * hakuX native core — boot and run a title.
 */
#ifndef NATIVE_CORE_CORE_H
#define NATIVE_CORE_CORE_H

#include "native/core/vm.h"
#include "native/core/xbe_load.h"
#include "native/core/xdvdfs.h"
#include "native/cpu/cpu.h"
#include "native/kernel/kernel.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcCore {
    NcVm vm;
    NcDisc disc;
    NcKernel kernel;
    NcXbe xbe;
    NcCpu *cpu;
} NcCore;

/* Sets up guest memory and the kernel, loads default.xbe from the disc and
 * prepares the title's main thread. Takes ownership of disc_fd. */
bool nc_core_boot(NcCore *c, int disc_fd);
/* The next boot goes into the savestate at path (native/state): the XBE it
 * was running boots, then the state replaces what the boot made. */
void nc_core_set_loadstate(const char *path);
/* The next boot is a relaunch: it boots the XBE the saved launch data page
 * names (nc_kernel_relaunch_file) instead of default.xbe. */
void nc_core_set_relaunch(bool on);

/* Runs the guest until it stops; logs the frontier summary. */
void nc_core_run(NcCore *c);
/* Stopping the core in its process, in order:
 * - nc_core_request_stop, any thread: the guest stops (nc_core_run returns)
 *   at the CPU's next block start, or where it idles or pauses;
 * - nc_core_halt, once nc_core_run has returned (or nc_core_boot failed):
 *   the devices' and the I/O threads stopped and joined (written files
 *   synced, queued reads landed), nothing of the guest's running;
 * - the renderer stopped, by the caller (it reads guest memory and the
 *   devices' registers);
 * - nc_core_destroy: everything the boot made freed (the CPU, the devices,
 *   guest memory, files, the disc) and the core's state reset, so that
 *   nc_core_boot can run again as in a new process. */
void nc_core_request_stop(NcCore *c);
void nc_core_halt(NcCore *c);
void nc_core_destroy(NcCore *c);

/* The core works through faults: device registers (MMIO), the HLE GPU's
 * dirty tracking and the JIT's call-ret stack all trap. A front end's
 * SIGSEGV/SIGBUS handler calls this first; true means handled (return from
 * the handler), false that the fault is someone else's. */
bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext);

#ifdef __cplusplus
}
#endif

#endif
