/*
 * hakuX native core — device MMIO through fault decoding.
 *
 * The JIT reaches guest memory with ordinary arm64 loads and stores: [x19,
 * wAddr, uxtw] register-offset forms, or immediate/unscaled forms off a
 * computed address. The fault address is the access address, so only the
 * transfer (size, direction, extension, register) needs decoding.
 */
#include "native/core/mmio.h"
#include <stdlib.h>
#include <time.h>

#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_DEVS  16
#define MAX_SITES 1024
#define LOG_FIRST 400

static NcMmioDev g_devs[MAX_DEVS];
static unsigned g_ndevs;

static struct {
    uint32_t addr;
    bool write;
    uint32_t count;
    uint64_t last;
} g_sites[MAX_SITES];
static unsigned g_logged;
static uint64_t g_total;
static uint64_t g_other;       /* accesses whose site found no slot in a few probes */

void nc_mmio_reset(void)
{
    memset(g_devs, 0, sizeof(g_devs));
    g_ndevs = 0;
    memset(g_sites, 0, sizeof(g_sites));
    g_logged = 0;
    g_total = g_other = 0;
}

void nc_mmio_register(const NcMmioDev *dev)
{
    if (g_ndevs < MAX_DEVS) {
        g_devs[g_ndevs++] = *dev;
    }
}

static const NcMmioDev *dev_at(uint32_t addr)
{
    for (unsigned i = 0; i < g_ndevs; i++) {
        if (addr - g_devs[i].base < g_devs[i].size) {
            return &g_devs[i];
        }
    }
    return NULL;
}

/* The per-site tally (nc_mmio_dump_stats), a few probes at most: with the
 * table full (the APU's voice registers alone fill it), each access probed
 * all 1024 slots, a fifth of Crimson Sea's guest thread in push-buffer mode
 * (its trap handler's ~110k register accesses a second). */
#define SITE_PROBES 8

static void count(uint32_t addr, bool write, uint64_t value)
{
    unsigned h = ((addr ^ write) * 2654435761u) % MAX_SITES;
    for (unsigned n = 0; n < SITE_PROBES; n++, h = (h + 1) % MAX_SITES) {
        if (!g_sites[h].count || (g_sites[h].addr == addr && g_sites[h].write == write)) {
            g_sites[h].addr = addr;
            g_sites[h].write = write;
            g_sites[h].count++;
            g_sites[h].last = value;
            return;
        }
    }
    g_other++;
}

/* The transfer a load/store instruction makes: one or two elements (LDP/STP)
 * of `bytes` each (16 for a Q register), and the base register writeback of
 * the pre- and post-indexed forms. */
typedef struct Access {
    unsigned bytes, count;
    bool store, sign, to64, fp, prefetch;
    unsigned rt, rt2, rn;
    int wb;        /* 0 none, 1 post-index, 2 pre-index */
    int64_t imm;   /* writeback amount; for pairs also the offset of the first element */
    bool pair_offset; /* pair: the first element is at Rn + imm (signed-offset form) */
} Access;

static int64_t sext(uint64_t v, unsigned bits)
{
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* LDR/STR family (single register). */
static bool decode_single(uint32_t insn, Access *a)
{
    const bool imm12 = (insn & 0x3b000000u) == 0x39000000u;
    const bool regoff = (insn & 0x3b200c00u) == 0x38200800u;
    const bool unscaled = (insn & 0x3b200c00u) == 0x38000000u;
    const bool post = (insn & 0x3b200c00u) == 0x38000400u;
    const bool pre = (insn & 0x3b200c00u) == 0x38000c00u;
    if (!imm12 && !regoff && !unscaled && !post && !pre) {
        return false;
    }
    const unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    memset(a, 0, sizeof(*a));
    a->count = 1;
    a->rt = insn & 31;
    a->rn = (insn >> 5) & 31;
    if (post || pre) {
        a->wb = post ? 1 : 2;
        a->imm = sext((insn >> 12) & 0x1ff, 9);
    }
    a->fp = (insn >> 26) & 1;
    if (a->fp) {
        if (opc & 2) {
            if (size != 0) {
                return false;
            }
            a->bytes = 16; /* Q */
            a->store = opc == 2;
            return true;
        }
        a->bytes = 1u << size;
        a->store = opc == 0;
        return true;
    }
    a->bytes = 1u << size;
    switch (opc) {
    case 0:
        a->store = true;
        break;
    case 1:
        a->to64 = size == 3;
        break;
    case 2:
        if (size == 3) {
            a->prefetch = true;
        } else {
            a->sign = a->to64 = true;
        }
        break;
    default:
        if (size >= 2) {
            return false;
        }
        a->sign = true;
        break;
    }
    return true;
}

/* LDP/STP/LDNP/STNP/LDPSW: opc 101 V 0 idx L imm7 Rt2 Rn Rt. */
static bool decode_pair(uint32_t insn, Access *a)
{
    if ((insn & 0x3a000000u) != 0x28000000u) {
        return false;
    }
    const unsigned opc = insn >> 30, idx = (insn >> 23) & 3;
    memset(a, 0, sizeof(*a));
    a->count = 2;
    a->fp = (insn >> 26) & 1;
    a->store = !((insn >> 22) & 1);
    a->rt = insn & 31;
    a->rt2 = (insn >> 10) & 31;
    a->rn = (insn >> 5) & 31;
    if (a->fp) {
        if (opc == 3) {
            return false;
        }
        a->bytes = 4u << opc; /* S, D, Q */
    } else {
        if (opc == 3 || (opc == 1 && a->store)) {
            return false;
        }
        a->bytes = opc == 2 ? 8 : 4;
        a->sign = a->to64 = opc == 1; /* LDPSW */
        a->to64 |= opc == 2;
    }
    a->imm = sext((insn >> 15) & 0x7f, 7) * (int64_t)a->bytes;
    a->wb = idx == 1 ? 1 : idx == 3 ? 2 : 0;
    a->pair_offset = idx == 0 || idx == 2;
    return true;
}

static bool decode(uint32_t insn, Access *a)
{
    return decode_single(insn, a) || decode_pair(insn, a);
}

/* The FP/SIMD register file in the signal frame. */
static struct fpsimd_context *fpsimd(ucontext_t *uc)
{
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)uc->uc_mcontext.__reserved;
    while (h->magic) {
        if (h->magic == FPSIMD_MAGIC) {
            return (struct fpsimd_context *)h;
        }
        h = (struct _aarch64_ctx *)((uint8_t *)h + h->size);
    }
    return NULL;
}

bool nc_mmio_fault(const NcVm *vm, const siginfo_t *info, void *ucontext)
{
    const uint64_t guest = nc_vm_guest_of(vm, info->si_addr);
    if (guest == UINT64_MAX) {
        return false;
    }
    const NcMmioDev *dev = dev_at((uint32_t)guest);
    if (!dev) {
        return false;
    }
    ucontext_t *uc = ucontext;
    mcontext_t *mc = &uc->uc_mcontext;
    Access a;
    if (!decode(*(const uint32_t *)(uintptr_t)mc->pc, &a)) {
        return false;
    }
    if (a.prefetch) {
        mc->pc += 4;
        return true;
    }
    struct fpsimd_context *fp = a.fp ? fpsimd(uc) : NULL;
    if (a.fp && !fp) {
        return false;
    }
    /* The first element's guest address: the fault address, except for pairs
     * (either element may be the one that faulted): from the base register. */
    uint64_t *rn = a.rn == 31 ? (uint64_t *)&mc->sp : (uint64_t *)&mc->regs[a.rn];
    uint32_t first = (uint32_t)guest;
    if (a.count == 2) {
        const uint64_t host = *rn + (a.wb == 1 ? 0 : a.imm);
        const uint64_t g = nc_vm_guest_of(vm, (void *)(uintptr_t)host);
        if (g == UINT64_MAX) {
            return false;
        }
        first = (uint32_t)g;
    }
    uint64_t v = 0;
    for (unsigned e = 0; e < a.count; e++) {
        const unsigned rt = e ? a.rt2 : a.rt;
        const uint32_t off = first + e * a.bytes - dev->base;
        if (a.bytes == 16) {
            if (a.store) {
                const __uint128_t q = fp->vregs[rt];
                dev->write(dev->opaque, off, (uint64_t)q, 8);
                dev->write(dev->opaque, off + 8, (uint64_t)(q >> 64), 8);
                v = (uint64_t)q;
            } else {
                const uint64_t lo = dev->read(dev->opaque, off, 8), hi = dev->read(dev->opaque, off + 8, 8);
                fp->vregs[rt] = (__uint128_t)lo | ((__uint128_t)hi << 64);
                v = lo;
            }
            continue;
        }
        const uint64_t mask = a.bytes < 8 ? (1ull << (8 * a.bytes)) - 1 : ~0ull;
        if (a.store) {
            v = (a.fp ? (uint64_t)fp->vregs[rt] : rt == 31 ? 0 : mc->regs[rt]) & mask;
            dev->write(dev->opaque, off, v, a.bytes);
        } else {
            v = dev->read(dev->opaque, off, a.bytes) & mask;
            if (a.fp) {
                fp->vregs[rt] = (__uint128_t)v;
            } else if (rt != 31) {
                uint64_t r = v;
                if (a.sign) {
                    const int64_t sv = sext(v, 8 * a.bytes);
                    r = a.to64 ? (uint64_t)sv : (uint32_t)sv;
                }
                mc->regs[rt] = r;
            }
        }
    }
    if (a.wb) {
        *rn += a.imm;
    }
    mc->pc += 4;

    g_total++;
    count((uint32_t)guest, a.store, v);
    /* NC_TRACE_MMIO=lo-hi (hex): every access in the range, the first 3000. */
    static int trace_left = -1;
    static uint32_t trace_lo, trace_hi;
    if (trace_left < 0) {
        const char *t = getenv("NC_TRACE_MMIO");
        trace_left = 0;
        if (t && sscanf(t, "%x-%x", &trace_lo, &trace_hi) == 2) {
            trace_left = 3000;
        }
    }
    if (trace_left > 0 && (uint32_t)guest >= trace_lo && (uint32_t)guest < trace_hi) {
        trace_left--;
        char line[160];
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int n = snprintf(line, sizeof(line), "hakuX-native-mmio: trace %ld.%06ld %s %08x %s %0*llx\n", (long)ts.tv_sec,
                         ts.tv_nsec / 1000, dev->name, first, a.store ? "<-" : "->", (int)(2 * (a.bytes > 8 ? 8 : a.bytes)),
                         (unsigned long long)v);
        write(2, line, (size_t)n);
    }
    if (g_logged < LOG_FIRST) {
        g_logged++;
        char line[160];
        int n = snprintf(line, sizeof(line), "hakuX-native-mmio: %s %s+%06x %s %0*llx (%u)\n", dev->name,
                         a.store ? "w" : "r", first - dev->base, a.store ? "<-" : "->", (int)(2 * (a.bytes > 8 ? 8 : a.bytes)),
                         (unsigned long long)v, a.bytes);
        write(2, line, (size_t)n);
    }
    return true;
}

/* nc_mmio_access's store may run again (nc_mmio_retry); one was asked for */
static __thread bool t_retry_ok, t_retry;

bool nc_mmio_retry(void)
{
    if (!t_retry_ok) {
        return false;
    }
    t_retry = true;
    return true;
}

bool nc_mmio_take_retry(void)
{
    const bool r = t_retry;
    t_retry = false;
    return r;
}

bool nc_mmio_access(uint32_t addr, unsigned bytes, bool store, uint64_t *v)
{
    const NcMmioDev *dev = dev_at(addr);
    if (!dev) {
        return false;
    }
    const uint64_t mask = bytes < 8 ? (1ull << (8 * bytes)) - 1 : ~0ull;
    if (store) {
        t_retry_ok = true;
        dev->write(dev->opaque, addr - dev->base, *v & mask, bytes);
        t_retry_ok = false;
    } else {
        *v = dev->read(dev->opaque, addr - dev->base, bytes) & mask;
    }
    g_total++;
    count(addr, store, *v);
    return true;
}

uint64_t nc_mmio_total(void)
{
    return g_total;
}

void nc_mmio_dump_stats(int fd)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "hakuX-native-mmio: %llu accesses (%llu at sites not tallied); "
                     "busiest sites:\n", (unsigned long long)g_total, (unsigned long long)g_other);
    nc_dump_line(fd, line, (size_t)n);
    bool shown[MAX_SITES] = {0};
    const char *top = getenv("NC_MMIO_TOP");
    const int limit = top ? atoi(top) : 16;
    for (int k = 0; k < limit; k++) {
        int best = -1;
        for (int i = 0; i < MAX_SITES; i++) {
            if (g_sites[i].count && !shown[i] && (best < 0 || g_sites[i].count > g_sites[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        shown[best] = true;
        const NcMmioDev *dev = dev_at(g_sites[best].addr);
        n = snprintf(line, sizeof(line), "hakuX-native-mmio:   %s %08x %s x%u last %llx\n", dev ? dev->name : "?",
                     g_sites[best].addr, g_sites[best].write ? "w" : "r", g_sites[best].count,
                     (unsigned long long)g_sites[best].last);
        nc_dump_line(fd, line, (size_t)n);
    }
}
