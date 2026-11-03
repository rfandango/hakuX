/*
 * hakuX native core — the core's faults on arm64 Linux (FEX backend, the
 * desktop front end): a SIGSEGV/SIGBUS handler hands faults to
 * nc_core_fault (MMIO, the HLE GPU's dirty tracking, the JIT's call-ret
 * stack, a title's pager) as Android's does (app/app_main.c). FEX installs
 * no handler of its own here: without this every guest device-register
 * access takes the process down.
 *
 * Faults that aren't the core's are logged (host pc, address, the guest
 * EIP when the pc is translated code) and go to whoever handled the signal
 * before, or the default action.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "native/core/core.h"

static NcCore *g_fault_core;
static struct sigaction g_old_segv, g_old_bus;

static void report(int sig, siginfo_t *info, void *ctx)
{
    const mcontext_t *mc = &((ucontext_t *)ctx)->uc_mcontext;
    char buf[512];
    const uint64_t guest = nc_vm_guest_of(&g_fault_core->vm, info->si_addr);
    const uint32_t eip = nc_cpu_guest_eip_at(g_fault_core->cpu, (uintptr_t)mc->pc);
    int n = snprintf(buf, sizeof(buf),
                     "hakuX-native: crash: sig %d code %d at pc %llx lr %llx, address %p (guest %s%llx), guest eip %08x\n",
                     sig, info->si_code, (unsigned long long)mc->pc, (unsigned long long)mc->regs[30], info->si_addr,
                     guest == UINT64_MAX ? "none:" : "", (unsigned long long)(guest == UINT64_MAX ? 0 : guest), eip);
    write(2, buf, (size_t)n);
    Dl_info dl;
    if (!eip && dladdr((void *)(uintptr_t)mc->pc, &dl) && dl.dli_fbase) {
        n = snprintf(buf, sizeof(buf), "hakuX-native: crash:   host %s+0x%llx (%s)\n", dl.dli_fname,
                     (unsigned long long)((uintptr_t)mc->pc - (uintptr_t)dl.dli_fbase),
                     dl.dli_sname ? dl.dli_sname : "?");
        write(2, buf, (size_t)n);
    }
}

static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (g_fault_core && nc_core_fault(g_fault_core, sig, info, ctx)) {
        return;
    }
    if (g_fault_core) {
        report(sig, info, ctx);
    }
    const struct sigaction *old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) {
            old->sa_sigaction(sig, info, ctx);
            return;
        }
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL); /* the fault repeats on return: the default action (a core dump) */
}

void nc_core_fault_install(NcCore *c)
{
    static int installed;
    g_fault_core = c;
    if (!installed) {
        installed = 1;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
    }
}
