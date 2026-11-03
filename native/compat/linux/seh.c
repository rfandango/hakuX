/*
 * xbased on Linux x86-64: vectored exception handlers (nc_winapi.h) over
 * one SIGSEGV/SIGBUS handler. The signal's ucontext becomes a CONTEXT
 * (Windows' register order and an FXSAVE image), the handlers run in turn,
 * and one that continues execution has its CONTEXT written back. None:
 * the previous disposition (a crash report, the default action).
 *
 * SA_NODEFER: the core leaves a handler by nc_longjmp (fault scopes) as it
 * did out of Android's signal handler, and the signal must not stay
 * blocked behind it.
 */
#define _GNU_SOURCE
#include "nc_winapi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#define MAX_HANDLERS 8

static PVECTORED_EXCEPTION_HANDLER g_h[MAX_HANDLERS];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction g_old_segv, g_old_bus;
static int g_installed;

static void to_context(const ucontext_t *uc, CONTEXT *c)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = (DWORD64)g[REG_RAX];
    c->Rcx = (DWORD64)g[REG_RCX];
    c->Rdx = (DWORD64)g[REG_RDX];
    c->Rbx = (DWORD64)g[REG_RBX];
    c->Rsp = (DWORD64)g[REG_RSP];
    c->Rbp = (DWORD64)g[REG_RBP];
    c->Rsi = (DWORD64)g[REG_RSI];
    c->Rdi = (DWORD64)g[REG_RDI];
    c->R8 = (DWORD64)g[REG_R8];
    c->R9 = (DWORD64)g[REG_R9];
    c->R10 = (DWORD64)g[REG_R10];
    c->R11 = (DWORD64)g[REG_R11];
    c->R12 = (DWORD64)g[REG_R12];
    c->R13 = (DWORD64)g[REG_R13];
    c->R14 = (DWORD64)g[REG_R14];
    c->R15 = (DWORD64)g[REG_R15];
    c->Rip = (DWORD64)g[REG_RIP];
    c->EFlags = (DWORD)g[REG_EFL];
    if (uc->uc_mcontext.fpregs) {
        memcpy(&c->FltSave, uc->uc_mcontext.fpregs, 512);
    } else {
        memset(&c->FltSave, 0, 512);
    }
}

static void from_context(const CONTEXT *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = (greg_t)c->Rax;
    g[REG_RCX] = (greg_t)c->Rcx;
    g[REG_RDX] = (greg_t)c->Rdx;
    g[REG_RBX] = (greg_t)c->Rbx;
    g[REG_RSP] = (greg_t)c->Rsp;
    g[REG_RBP] = (greg_t)c->Rbp;
    g[REG_RSI] = (greg_t)c->Rsi;
    g[REG_RDI] = (greg_t)c->Rdi;
    g[REG_R8] = (greg_t)c->R8;
    g[REG_R9] = (greg_t)c->R9;
    g[REG_R10] = (greg_t)c->R10;
    g[REG_R11] = (greg_t)c->R11;
    g[REG_R12] = (greg_t)c->R12;
    g[REG_R13] = (greg_t)c->R13;
    g[REG_R14] = (greg_t)c->R14;
    g[REG_R15] = (greg_t)c->R15;
    g[REG_RIP] = (greg_t)c->Rip;
    g[REG_EFL] = (greg_t)c->EFlags;
    if (uc->uc_mcontext.fpregs) {
        /* the FXSAVE image's first 512 bytes; the kernel's XSAVE extension
         * behind them (the upper YMM halves) is left as it was */
        memcpy(uc->uc_mcontext.fpregs, &c->FltSave, 512);
    }
}

static void chain(const struct sigaction *old, int sig, siginfo_t *si, void *uc)
{
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) {
            old->sa_sigaction(sig, si, uc);
            return;
        }
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    /* the default: a crash, with the core it leaves */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_signal(int sig, siginfo_t *si, void *ucp)
{
    ucontext_t *uc = (ucontext_t *)ucp;
    CONTEXT c;
    to_context(uc, &c);
    EXCEPTION_RECORD er;
    memset(&er, 0, sizeof(er));
    er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    er.ExceptionAddress = (PVOID)c.Rip;
    er.NumberParameters = 2;
    /* page fault error code bit 1: a write */
    er.ExceptionInformation[0] = (uc->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0;
    er.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    EXCEPTION_POINTERS ep = {&er, &c};
    for (int i = 0; i < g_n; i++) {
        if (g_h[i] && g_h[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            from_context(&c, uc);
            return;
        }
    }
    chain(sig == SIGBUS ? &g_old_bus : &g_old_segv, sig, si, ucp);
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h)
{
    pthread_mutex_lock(&g_mu);
    if (g_n < MAX_HANDLERS) {
        if (first) {
            memmove(&g_h[1], &g_h[0], sizeof(g_h[0]) * (size_t)g_n);
            g_h[0] = h;
        } else {
            g_h[g_n] = h;
        }
        g_n++;
    }
    if (!g_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
        g_installed = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return (PVOID)h;
}

ULONG RemoveVectoredExceptionHandler(PVOID h)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_n; i++) {
        if ((PVOID)g_h[i] == h) {
            memmove(&g_h[i], &g_h[i + 1], sizeof(g_h[0]) * (size_t)(g_n - i - 1));
            g_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return 1;
}
