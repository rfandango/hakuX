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
/* The next boot is the dashboard's (C:\xboxdash.xbe), with no disc: the app's
 * Dashboard, where Xbox Live accounts are made. */
void nc_core_set_dashboard(bool on);

/* Runs the guest until it stops; logs the frontier summary. */
void nc_core_run(NcCore *c);

/* The core works through faults: device registers (MMIO), the HLE GPU's
 * dirty tracking and the JIT's call-ret stack all trap. A front end's
 * SIGSEGV/SIGBUS handler calls this first; true means handled (return from
 * the handler), false that the fault is someone else's. */
bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext);

#ifdef __cplusplus
}
#endif

#endif
