/*
 * hakuX native core — structured exception handling: RtlRaiseException and
 * RtlUnwind, as the Xbox kernel runs x86 SEH.
 *
 * Handlers are title code, so the kernel's loops over the registration
 * chain (fs:[0]) run one handler at a time: each is called on the guest
 * stack with its return address at the SEH-return stub, and the loop goes on
 * when it returns there. A thread keeps a level per dispatch or unwind in
 * progress, since handlers raise and unwind in turn (a C++ handler unwinds
 * from inside the dispatch that found its catch). Around each call the
 * kernel links a registration of its own, as NT's RtlpExecuteHandler2 does:
 * an exception raised inside a handler reports the frame being handled
 * (nested), and an unwind that runs into one in progress takes over where
 * it was (collided).
 *
 * A C++ catch leaves by jumping to its continuation and never returns to
 * the dispatch: a level whose frame lies below the stack pointer is gone.
 */
#include "native/kernel/kernel.h"

#include <stdio.h>

/* EXCEPTION_RECORD */
#define ER_CODE    0x00
#define ER_FLAGS   0x04
#define ER_ADDRESS 0x0C
#define ER_NPARAMS 0x10
#define ER_INFO    0x14
#define ER_SIZE    0x50

#define EXCEPTION_NONCONTINUABLE 0x01u
#define EXCEPTION_UNWINDING      0x02u
#define EXCEPTION_EXIT_UNWIND    0x04u
#define EXCEPTION_STACK_INVALID  0x08u
#define EXCEPTION_NESTED_CALL    0x10u

enum { CONTINUE_EXECUTION, CONTINUE_SEARCH, NESTED_EXCEPTION, COLLIDED_UNWIND };

/* CONTEXT, the Xbox's: FloatSave is the FXSAVE image plus Cr0NpxState; no
 * debug registers and no data segment registers. */
#define CX_FLAGS  0x000
#define CX_FCW    0x004
#define CX_MXCSR  0x01C
#define CX_EDI    0x208
#define CX_ESI    0x20C
#define CX_EBX    0x210
#define CX_EDX    0x214
#define CX_ECX    0x218
#define CX_EAX    0x21C
#define CX_EBP    0x220
#define CX_EIP    0x224
#define CX_CS     0x228
#define CX_EFLAGS 0x22C
#define CX_ESP    0x230
#define CX_SS     0x234
#define CX_SIZE   0x238
#define CONTEXT_FULL 0x10007u /* control, integer, segments */

#define CHAIN_END     0xFFFFFFFFu
#define STATUS_UNWIND 0xC0000027u
#define CXX_EXCEPTION 0xE06D7363u /* MSVC's throw */

/* A level's guest frame, from its base up:
 *   +0   return address, the SEH-return stub (ESP as the handler starts)
 *   +4   ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext
 *   +20  DispatcherContext: the frame a nested raise or collided unwind reports
 *   +24  the kernel's registration around the call: Next, Handler, the frame
 *        being handled
 *   +36  CONTEXT; then the EXCEPTION_RECORD of an unwind given none */
#define LV_DISP 20
#define LV_REG  24
#define LV_CTX  36
#define LV_REC  (LV_CTX + CX_SIZE)
#define LV_SIZE (LV_REC + ER_SIZE)

static uint32_t rd(NcKernel *k, uint32_t va)
{
    return nc_vm_rd32(k->vm, va);
}

static void wr(NcKernel *k, uint32_t va, uint32_t v)
{
    nc_vm_wr32(k->vm, va, v);
}

static uint32_t chain_head(NcKernel *k)
{
    return rd(k, k->kpcr_va); /* NT_TIB.ExceptionList */
}

static void set_chain_head(NcKernel *k, uint32_t reg)
{
    wr(k, k->kpcr_va, reg);
}

/* The CONTEXT a dispatch or unwind starts from: the caller's registers,
 * returning to eip with esp. */
static void capture(NcKernel *k, NcFrame *f, uint32_t ctx, uint32_t eip, uint32_t esp)
{
    uint8_t *c = nc_vm_host(k->vm, ctx);
    memset(c, 0, CX_SIZE);
    nc_wr32(c + CX_FLAGS, CONTEXT_FULL);
    nc_wr16(c + CX_FCW, 0x027F);
    nc_wr32(c + CX_MXCSR, 0x1F80);
    nc_wr32(c + CX_EDI, nc_frame_gpr(f, NC_EDI));
    nc_wr32(c + CX_ESI, nc_frame_gpr(f, NC_ESI));
    nc_wr32(c + CX_EBX, nc_frame_gpr(f, NC_EBX));
    nc_wr32(c + CX_EDX, nc_frame_gpr(f, NC_EDX));
    nc_wr32(c + CX_ECX, nc_frame_gpr(f, NC_ECX));
    nc_wr32(c + CX_EAX, nc_frame_gpr(f, NC_EAX));
    nc_wr32(c + CX_EBP, nc_frame_gpr(f, NC_EBP));
    nc_wr32(c + CX_EIP, eip);
    nc_wr32(c + CX_CS, nc_frame_cs(f));
    nc_wr32(c + CX_EFLAGS, nc_frame_eflags(f));
    nc_wr32(c + CX_ESP, esp);
    nc_wr32(c + CX_SS, 0x10);
}

/* NtContinue: the guest runs on from the CONTEXT's control and integer registers. */
static void resume(NcKernel *k, NcFrame *f, uint32_t ctx)
{
    static const struct {
        int reg;
        uint32_t off;
    } regs[] = {{NC_EDI, CX_EDI}, {NC_ESI, CX_ESI}, {NC_EBX, CX_EBX}, {NC_EDX, CX_EDX},
                {NC_ECX, CX_ECX}, {NC_EAX, CX_EAX}, {NC_EBP, CX_EBP}, {NC_ESP, CX_ESP}};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        nc_frame_set_gpr(f, regs[i].reg, rd(k, ctx + regs[i].off));
    }
    nc_frame_set_eflags(f, rd(k, ctx + CX_EFLAGS));
    nc_frame_set_eip(f, rd(k, ctx + CX_EIP));
}

/* What a C++ exception throws: the first catchable type's decorated name. */
static const char *cxx_type(NcKernel *k, uint32_t rec, char *buf, size_t len)
{
    snprintf(buf, len, "?");
    if (rd(k, rec + ER_CODE) != CXX_EXCEPTION || rd(k, rec + ER_NPARAMS) < 3) {
        return buf;
    }
    const uint32_t info = rd(k, rec + ER_INFO + 8);
    const uint32_t types = info && nc_vm_phys(k->vm, info) != UINT32_MAX ? rd(k, info + 12) : 0;
    const uint32_t first = types && nc_vm_phys(k->vm, types) != UINT32_MAX && rd(k, types) ? rd(k, types + 4) : 0;
    const uint32_t td = first && nc_vm_phys(k->vm, first) != UINT32_MAX ? rd(k, first + 4) : 0;
    if (td && nc_vm_phys(k->vm, td + 8) != UINT32_MAX) {
        const char *name = nc_vm_host(k->vm, td + 8);
        size_t n = 0;
        while (n + 1 < len && name[n] >= 0x20 && name[n] < 0x7F) {
            buf[n] = name[n];
            n++;
        }
        buf[n] = 0;
    }
    return buf;
}

/* The object a C++ exception throws: its first words, and the text any of
 * them points at (error exceptions carry their message). */
static void log_thrown(NcKernel *k, uint32_t obj)
{
    if (!obj || nc_vm_phys(k->vm, obj) == UINT32_MAX || nc_vm_phys(k->vm, obj + 31) == UINT32_MAX) {
        return;
    }
    char line[400];
    int n = snprintf(line, sizeof(line), "seh:   thrown object %08x:", obj);
    for (unsigned i = 0; i < 8; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", rd(k, obj + 4 * i));
    }
    for (unsigned i = 0; i < 8 && n < (int)sizeof(line) - 80; i++) {
        const uint32_t p = rd(k, obj + 4 * i);
        if (p < 0x10000 || nc_vm_phys(k->vm, p) == UINT32_MAX || nc_vm_phys(k->vm, p + 63) == UINT32_MAX) {
            continue;
        }
        const char *s = nc_vm_host(k->vm, p);
        unsigned len = 0;
        while (len < 64 && s[len] >= 0x20 && s[len] < 0x7F) {
            len++;
        }
        if (len >= 4) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " [%u]\"%.*s\"", i, (int)len, s);
        }
    }
    nc_log("hakuX-native", "%s", line);
}

/* A new level below esp; levels a jump out of a handler left behind go. */
static NcSeh *push_level(NcKernel *k, NcFrame *f, NcThread *t, uint32_t esp)
{
    while (t->nseh && t->seh[t->nseh - 1].base < esp) {
        t->nseh--;
    }
    if (t->nseh == NC_MAX_SEH) {
        nc_kernel_request_stop(k, f, "seh: exceptions nested too deep");
        return NULL;
    }
    NcSeh *l = &t->seh[t->nseh++];
    memset(l, 0, sizeof(*l));
    l->base = (esp - LV_SIZE) & ~15u;
    l->ctx = l->base + LV_CTX;
    return l;
}

static void unhandled(NcKernel *k, NcFrame *f, NcSeh *l, const char *why)
{
    char type[96];
    nc_kernel_request_stop(k, f, "seh: unhandled exception %08x at %08x (%s)%s%s", rd(k, l->rec + ER_CODE),
                           rd(k, l->rec + ER_ADDRESS), why, rd(k, l->rec + ER_CODE) == CXX_EXCEPTION ? ", C++ " : "",
                           rd(k, l->rec + ER_CODE) == CXX_EXCEPTION ? cxx_type(k, l->rec, type, sizeof(type)) : "");
}

/* Calls the handler of the level's registration, or ends the level. */
static void step(NcKernel *k, NcFrame *f, NcThread *t)
{
    NcSeh *l = &t->seh[t->nseh - 1];
    if (l->unwind && (l->reg == l->target || (l->reg == CHAIN_END && !l->target))) {
        t->nseh--;
        resume(k, f, l->ctx); /* back from RtlUnwind */
        return;
    }
    if (l->reg == CHAIN_END) {
        if (l->unwind) {
            nc_kernel_request_stop(k, f, "seh: unwind to %08x ran off the chain", l->target);
        } else {
            unhandled(k, f, l, "no handler took it");
        }
        return;
    }
    if (l->unwind && l->target && l->target < l->reg) {
        nc_kernel_request_stop(k, f, "seh: unwind target %08x is below the chain (%08x)", l->target, l->reg);
        return;
    }
    const uint32_t base = rd(k, k->kpcr_va + 4), limit = rd(k, k->kpcr_va + 8);
    if ((l->reg & 3) || l->reg < limit || l->reg + 8 > base) {
        wr(k, l->rec + ER_FLAGS, rd(k, l->rec + ER_FLAGS) | EXCEPTION_STACK_INVALID);
        if (l->unwind) {
            nc_kernel_request_stop(k, f, "seh: registration %08x outside the stack %08x-%08x", l->reg, limit, base);
        } else {
            unhandled(k, f, l, "registration outside the stack");
        }
        return;
    }
    const uint32_t b = l->base;
    wr(k, b, k->seh_return_va);
    wr(k, b + 4, l->rec);
    wr(k, b + 8, l->reg);
    wr(k, b + 12, l->ctx);
    wr(k, b + 16, b + LV_DISP);
    wr(k, b + LV_DISP, 0);
    wr(k, b + LV_REG, chain_head(k));
    wr(k, b + LV_REG + 4, l->unwind ? k->seh_collided_va : k->seh_nested_va);
    wr(k, b + LV_REG + 8, l->reg);
    set_chain_head(k, b + LV_REG);
    nc_frame_set_gpr(f, NC_ESP, b);
    nc_frame_set_eip(f, rd(k, l->reg + 4));
}

/* RtlRaiseException(ExceptionRecord): first-chance dispatch from the caller. */
static uint64_t RtlRaiseException(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = k->current;
    const uint32_t esp = nc_frame_gpr(f, NC_ESP), ret = rd(k, esp);
    NcSeh *l = t ? push_level(k, f, t, esp) : NULL;
    if (!l) {
        return 0;
    }
    l->rec = a[0];
    capture(k, f, l->ctx, ret, esp + 8);
    wr(k, l->rec + ER_ADDRESS, ret);
    l->reg = chain_head(k);
    k->redirected = true;
    static unsigned s_logged;
    if (s_logged++ < 32) {
        char type[96];
        char calls[96];
        nc_kernel_frame_chain(k, f, calls, sizeof(calls));
        nc_log("hakuX-native", "seh: t%u raises %08x at %08x%s%s | frames%s", t->id, rd(k, l->rec + ER_CODE), ret,
               rd(k, l->rec + ER_CODE) == CXX_EXCEPTION ? ", C++ " : "",
               rd(k, l->rec + ER_CODE) == CXX_EXCEPTION ? cxx_type(k, l->rec, type, sizeof(type)) : "", calls);
        if (rd(k, l->rec + ER_CODE) == CXX_EXCEPTION) {
            log_thrown(k, rd(k, l->rec + ER_INFO + 4));
        }
    }
    step(k, f, t);
    return 0;
}

/* Called when a guest memory access faults in translated code; f holds the
 * guest state at the faulting instruction. When the Xbox kernel's page-fault
 * handler can't satisfy an access, it raises STATUS_ACCESS_VIOLATION and lets
 * the thread's SEH handlers deal with it. This does the same.
 * ExceptionInformation[0] is 1 for a write and 0 for a read, and [1] is the
 * address.
 *
 * Some titles' file streams depend on this: they only reserve their buffers,
 * and their exception handler commits and fills each page the first time the
 * game reads it.
 *
 * Returns false if there is no current thread, the thread has no SEH
 * handler, there is no mapped stack below ESP to run the dispatch on, or the
 * dispatch can't be set up. The caller then reports the fault as a crash. */
#define STATUS_ACCESS_VIOLATION 0xC0000005u

bool nc_seh_access_violation(NcKernel *k, NcFrame *f, uint32_t va, bool write)
{
    NcThread *t = k->current;
    const uint32_t esp = nc_frame_gpr(f, NC_ESP), eip = nc_frame_eip(f);
    const uint32_t lo = (esp - LV_SIZE) & ~15u;
    if (!t || chain_head(k) == CHAIN_END || nc_vm_phys(k->vm, lo) == UINT32_MAX ||
        nc_vm_phys(k->vm, esp - 1) == UINT32_MAX) {
        return false;
    }
    NcSeh *l = push_level(k, f, t, esp);
    if (!l) {
        return false;
    }
    l->rec = l->base + LV_REC;
    uint8_t *r = nc_vm_host(k->vm, l->rec);
    memset(r, 0, ER_SIZE);
    nc_wr32(r + ER_CODE, STATUS_ACCESS_VIOLATION);
    nc_wr32(r + ER_ADDRESS, eip);
    nc_wr32(r + ER_NPARAMS, 2);
    nc_wr32(r + ER_INFO, write ? 1u : 0u);
    nc_wr32(r + ER_INFO + 4, va);
    capture(k, f, l->ctx, eip, esp);
    l->reg = chain_head(k);
    static unsigned s_logged;
    if (s_logged++ < 32) {
        nc_log("hakuX-native", "seh: t%u access violation at %08x, %s %08x: to the title's handlers", t->id, eip,
               write ? "writing" : "reading", va);
    }
    step(k, f, t);
    return true;
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): calls the
 * handlers above TargetFrame to unwind and unlinks them, then returns. */
static uint64_t RtlUnwind(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcThread *t = k->current;
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    NcSeh *l = t ? push_level(k, f, t, esp) : NULL;
    if (!l) {
        return 0;
    }
    l->unwind = true;
    l->target = a[0];
    capture(k, f, l->ctx, rd(k, esp), esp + 4 + 16);
    wr(k, l->ctx + CX_EAX, a[3]);
    l->rec = a[2];
    if (!l->rec) {
        l->rec = l->base + LV_REC;
        memset(nc_vm_host(k->vm, l->rec), 0, ER_SIZE);
        wr(k, l->rec + ER_CODE, STATUS_UNWIND);
        wr(k, l->rec + ER_ADDRESS, rd(k, esp));
    }
    wr(k, l->rec + ER_FLAGS,
       rd(k, l->rec + ER_FLAGS) | EXCEPTION_UNWINDING | (a[0] ? 0 : EXCEPTION_EXIT_UNWIND));
    l->reg = chain_head(k);
    k->redirected = true;
    step(k, f, t);
    return 0;
}

/* A handler returned to the SEH-return stub with its disposition in EAX. */
static void handler_returned(NcKernel *k, NcFrame *f)
{
    NcThread *t = k->current;
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    while (t && t->nseh && t->seh[t->nseh - 1].base + 4 < esp) {
        t->nseh--;
    }
    if (!t || !t->nseh || t->seh[t->nseh - 1].base + 4 != esp) {
        nc_kernel_request_stop(k, f, "seh: a handler returned with no dispatch of its own (esp %08x)", esp);
        return;
    }
    NcSeh *l = &t->seh[t->nseh - 1];
    set_chain_head(k, rd(k, l->base + LV_REG)); /* the kernel's registration goes */
    const uint32_t disp = nc_frame_gpr(f, NC_EAX), reported = rd(k, l->base + LV_DISP);
    uint32_t flags = rd(k, l->rec + ER_FLAGS);
    if (!l->unwind) {
        if (l->nested == l->reg) {
            flags &= ~EXCEPTION_NESTED_CALL;
            l->nested = 0;
        }
        switch (disp) {
        case CONTINUE_EXECUTION:
            wr(k, l->rec + ER_FLAGS, flags);
            if (flags & EXCEPTION_NONCONTINUABLE) {
                unhandled(k, f, l, "a handler continued it, but it is noncontinuable");
                return;
            }
            t->nseh--;
            resume(k, f, l->ctx);
            return;
        case CONTINUE_SEARCH:
            break;
        case NESTED_EXCEPTION:
            flags |= EXCEPTION_NESTED_CALL;
            if (reported > l->nested) {
                l->nested = reported;
            }
            break;
        default:
            nc_kernel_request_stop(k, f, "seh: handler at %08x returned disposition %u", rd(k, l->reg + 4), disp);
            return;
        }
        wr(k, l->rec + ER_FLAGS, flags);
        l->reg = rd(k, l->reg);
    } else {
        if (disp == COLLIDED_UNWIND) {
            l->reg = reported;
        } else if (disp != CONTINUE_SEARCH) {
            nc_kernel_request_stop(k, f, "seh: unwind handler at %08x returned disposition %u", rd(k, l->reg + 4),
                                   disp);
            return;
        }
        l->reg = rd(k, l->reg);
        set_chain_head(k, l->reg); /* the frame unwound is unlinked */
    }
    step(k, f, t);
}

/* The kernel's own registrations' handlers (cdecl: ExceptionRecord,
 * EstablisherFrame, ContextRecord, DispatcherContext): during a dispatch, a
 * raise from inside a handler is nested; during an unwind, another unwind
 * reaching it has collided. Either reports the frame being handled. */
static void own_handler(NcKernel *k, NcFrame *f, bool unwind)
{
    const uint32_t esp = nc_frame_gpr(f, NC_ESP);
    const uint32_t rec = rd(k, esp + 4), frame = rd(k, esp + 8), dctx = rd(k, esp + 16);
    const bool unwinding = rd(k, rec + ER_FLAGS) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND);
    uint32_t r = CONTINUE_SEARCH;
    if (unwinding == unwind) {
        wr(k, dctx, rd(k, frame + 8));
        r = unwind ? COLLIDED_UNWIND : NESTED_EXCEPTION;
    }
    nc_frame_set_gpr(f, NC_EAX, r);
    nc_frame_set_gpr(f, NC_ESP, esp + 4);
    nc_frame_set_eip(f, rd(k, esp));
}

void nc_seh_stub(NcKernel *k, NcFrame *f, unsigned ordinal)
{
    if (ordinal == NC_KORDINAL_SEH_RETURN) {
        handler_returned(k, f);
    } else {
        own_handler(k, f, ordinal == NC_KORDINAL_SEH_COLLIDED);
    }
}

void nc_seh_register(NcKernel *k)
{
    nc_kernel_impl(k, "RtlRaiseException", RtlRaiseException);
    nc_kernel_impl(k, "RtlUnwind", RtlUnwind);
}
