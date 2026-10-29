/*
 * hakuX native core — device register accesses without host faults.
 *
 * A guest access to a device window faults in the host and is emulated in
 * the signal handler (core/mmio.c): a few microseconds each, and DirectSound
 * makes thousands a second (the APU's voice methods). The guest instruction
 * that faulted becomes a hooked address instead: its block is translated
 * again with an exit to the kernel before it, and the kernel emulates the
 * move (MOV/MOVZX between a register or immediate and memory) against the
 * device directly, or the compare (TEST/CMP of memory with an immediate:
 * the flags from the register's value). An instruction that turns out to touch ordinary memory,
 * or isn't a move this decodes, goes back to running as translated.
 */
#include "native/kernel/kernel.h"
#include "native/core/mmio.h"

#define SITES      4096u
#define PENDING    64u
#define MAX_STRIKE 3u

static uint32_t g_site[SITES];   /* open addressing on the guest EIP; 0 = empty */
static uint8_t g_strikes[SITES]; /* removals, kept per slot of a removed site */
static uint32_t g_pending[PENDING];
static volatile unsigned g_npending;

static unsigned slot_of(uint32_t eip)
{
    unsigned h = (eip * 2654435761u) % SITES;
    for (unsigned n = 0; n < SITES; n++, h = (h + 1) % SITES) {
        if (g_site[h] == eip || g_site[h] == 0) {
            return h;
        }
    }
    return SITES;
}

/* Each change retranslates every block holding the instruction (a long
 * function's multiblocks: milliseconds each), so they are logged, at most
 * 10 a second. */
static void site_log(const char *what, uint32_t eip)
{
    static uint64_t window;
    static unsigned logged;
    const uint64_t now = nc_ke_now_ns();
    if (now - window > 1000000000ull) {
        window = now;
        logged = 0;
    }
    if (logged++ < 10) {
        nc_log("hakuX-native", "x86mmio: site %08x %s: its blocks are translated again", eip, what);
    }
}

int nc_x86mmio_site_state(uint32_t eip)
{
    const unsigned s = slot_of(eip);
    if (s < SITES && g_site[s] == eip) {
        return 1;
    }
    const unsigned d = slot_of(eip | 0x80000000u);
    if (d < SITES && g_site[d] == (eip | 0x80000000u)) {
        return -(int)g_strikes[d];
    }
    return 0;
}

bool nc_x86mmio_is_site(uint32_t eip)
{
    const unsigned s = slot_of(eip);
    return s < SITES && g_site[s] == eip;
}

void nc_x86mmio_note(uint32_t eip)
{
    if (!eip || eip >= 0x80000000u) {
        return; /* title code only (the slot marker of a removed site is the top bit) */
    }
    const unsigned n = g_npending;
    if (n < PENDING) {
        g_pending[n] = eip;
        g_npending = n + 1;
    }
}

/* Removed sites keep their slot with the top bit set, so lookups still
 * probe past them and a site that keeps failing stays out. */
static void drop_site(NcKernel *k, uint32_t eip)
{
    const unsigned s = slot_of(eip);
    if (s < SITES && g_site[s] == eip) {
        g_site[s] = eip | 0x80000000u;
        g_strikes[s]++;
        site_log("dropped", eip);
        nc_cpu_invalidate(k->cpu, eip, 1);
    }
}

void nc_x86mmio_flush(NcKernel *k)
{
    const unsigned n = g_npending;
    if (!n) {
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        const uint32_t eip = g_pending[i];
        unsigned s = slot_of(eip);
        if (s < SITES && g_site[s] == eip) {
            continue;
        }
        /* A site dropped before: back in the same slot unless it struck out. */
        const unsigned dropped = slot_of(eip | 0x80000000u);
        if (dropped < SITES && g_site[dropped] == (eip | 0x80000000u)) {
            if (g_strikes[dropped] >= MAX_STRIKE) {
                continue;
            }
            s = dropped;
        }
        if (s == SITES) {
            continue;
        }
        g_site[s] = eip;
        site_log("added", eip);
        nc_cpu_invalidate(k->cpu, eip, 1);
    }
    g_npending = 0;
}

/* ------------------------------------------------------------- decoding */

typedef struct Op {
    unsigned len;
    unsigned bytes;   /* 1, 2 or 4 */
    bool store;
    bool imm;         /* a store of imm */
    uint32_t immv;
    int reg;          /* the register operand (x86 encoding), -1 for moffs (EAX) */
    bool zx;          /* MOVZX */
    int cmp;          /* 1 TEST, 2 CMP: memory against immv, the flags set */
    uint32_t addr;
} Op;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The effective address of a ModRM memory operand (32-bit addressing). */
static bool modrm_addr(NcFrame *f, const uint8_t *p, unsigned *len, uint32_t *addr, int *reg)
{
    const uint8_t m = p[0];
    const unsigned mod = m >> 6, rm = m & 7;
    *reg = (m >> 3) & 7;
    unsigned n = 1;
    uint32_t a = 0;
    if (mod == 3) {
        return false; /* a register, not memory */
    }
    if (rm == 4) {
        const uint8_t sib = p[n++];
        const unsigned scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4) {
            a += nc_frame_gpr(f, (int)index) << scale;
        }
        if (base == 5 && mod == 0) {
            a += rd32(p + n);
            n += 4;
        } else {
            a += nc_frame_gpr(f, (int)base);
        }
    } else if (rm == 5 && mod == 0) {
        a = rd32(p + n);
        n += 4;
    } else {
        a = nc_frame_gpr(f, (int)rm);
    }
    if (mod == 1) {
        a += (uint32_t)(int32_t)(int8_t)p[n];
        n += 1;
    } else if (mod == 2) {
        a += rd32(p + n);
        n += 4;
    }
    *len = n;
    *addr = a;
    return true;
}

static bool decode(NcKernel *k, NcFrame *f, const uint8_t *p, Op *op)
{
    unsigned i = 0;
    bool op16 = false;
    uint32_t seg = 0;
    for (;; i++) {
        if (i >= 4) {
            return false;
        }
        if (p[i] == 0x66) {
            op16 = true;
        } else if (p[i] == 0x64) {
            seg = k->kpcr_va; /* FS: the KPCR */
        } else if (p[i] == 0x2E || p[i] == 0x36 || p[i] == 0x3E || p[i] == 0x26) {
            /* flat */
        } else {
            break;
        }
    }
    memset(op, 0, sizeof(*op));
    const uint8_t opc = p[i++];
    unsigned mlen;
    switch (opc) {
    case 0x8B: case 0x89: case 0x8A: case 0x88:
        if (!modrm_addr(f, p + i, &mlen, &op->addr, &op->reg)) {
            return false;
        }
        i += mlen;
        op->store = opc == 0x89 || opc == 0x88;
        op->bytes = (opc & 1) ? (op16 ? 2 : 4) : 1;
        break;
    case 0xC7: case 0xC6:
        if (((p[i] >> 3) & 7) != 0 || !modrm_addr(f, p + i, &mlen, &op->addr, &op->reg)) {
            return false;
        }
        i += mlen;
        op->store = op->imm = true;
        op->bytes = opc == 0xC6 ? 1 : op16 ? 2 : 4;
        op->immv = op->bytes == 1 ? p[i] : op->bytes == 2 ? (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 : rd32(p + i);
        i += op->bytes;
        break;
    case 0xF7: case 0xF6:                       /* TEST r/m, imm */
    case 0x81: case 0x83: case 0x80: {          /* CMP r/m, imm */
        const unsigned ext = (p[i] >> 3) & 7;
        const bool test = opc == 0xF7 || opc == 0xF6;
        if ((test ? ext != 0 : ext != 7) || !modrm_addr(f, p + i, &mlen, &op->addr, &op->reg)) {
            return false;
        }
        i += mlen;
        op->cmp = test ? 1 : 2;
        op->bytes = opc == 0xF6 || opc == 0x80 ? 1 : op16 ? 2 : 4;
        if (opc == 0x83) {
            op->immv = (uint32_t)(int32_t)(int8_t)p[i];     /* sign-extended */
            i += 1;
        } else if (op->bytes == 1) {
            op->immv = p[i];
            i += 1;
        } else if (op->bytes == 2) {
            op->immv = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
            i += 2;
        } else {
            op->immv = rd32(p + i);
            i += 4;
        }
        break;
    }
    case 0xA1: case 0xA3: case 0xA0: case 0xA2:
        op->addr = rd32(p + i);
        i += 4;
        op->reg = -1;
        op->store = opc == 0xA3 || opc == 0xA2;
        op->bytes = (opc & 1) ? (op16 ? 2 : 4) : 1;
        break;
    case 0x0F:
        if (p[i] != 0xB6 && p[i] != 0xB7) {
            return false;
        }
        op->bytes = p[i] == 0xB6 ? 1 : 2;
        i++;
        if (!modrm_addr(f, p + i, &mlen, &op->addr, &op->reg)) {
            return false;
        }
        i += mlen;
        op->zx = true;
        if (op16) {
            return false; /* MOVZX r16: not worth it */
        }
        break;
    default:
        return false;
    }
    op->addr += seg;
    op->len = i;
    return true;
}

/* An 8-bit register: AL, CL, DL, BL, AH, CH, DH, BH. */
static uint32_t get_reg(NcFrame *f, int reg, unsigned bytes)
{
    if (bytes == 1) {
        const uint32_t v = nc_frame_gpr(f, reg & 3);
        return reg < 4 ? v & 0xFF : (v >> 8) & 0xFF;
    }
    const uint32_t v = nc_frame_gpr(f, reg);
    return bytes == 2 ? v & 0xFFFF : v;
}

static void set_reg(NcFrame *f, int reg, unsigned bytes, uint32_t v)
{
    if (bytes == 1) {
        const uint32_t old = nc_frame_gpr(f, reg & 3);
        nc_frame_set_gpr(f, reg & 3, reg < 4 ? (old & ~0xFFu) | (v & 0xFF) : (old & ~0xFF00u) | ((v & 0xFF) << 8));
    } else if (bytes == 2) {
        nc_frame_set_gpr(f, reg, (nc_frame_gpr(f, reg) & ~0xFFFFu) | (v & 0xFFFF));
    } else {
        nc_frame_set_gpr(f, reg, v);
    }
}

/* EFLAGS after TEST (a & b) or CMP (a - b) of bytes-wide values: CF PF AF
 * ZF SF OF set as the x86 does (AF clear after TEST), the rest kept. D3D's
 * write-back cache flush polls its register with TEST (0x3aa330 in
 * Crimson Sea), once per push-buffer trap. */
static uint32_t cmp_flags(uint32_t old, int kind, uint32_t a, uint32_t b, unsigned bytes)
{
    const uint32_t mask = bytes == 4 ? 0xFFFFFFFFu : (1u << (8 * bytes)) - 1;
    const uint32_t sign = 1u << (8 * bytes - 1);
    a &= mask;
    b &= mask;
    const uint32_t r = kind == 1 ? a & b : (a - b) & mask;
    uint32_t fl = 0;
    if (kind == 2) {
        fl |= a < b ? 0x001u : 0;                       /* CF: a borrow */
        fl |= ((a ^ b) & (a ^ r) & sign) ? 0x800u : 0;  /* OF */
        fl |= ((a ^ b ^ r) & 0x10) ? 0x010u : 0;        /* AF */
    }
    fl |= __builtin_parity(r & 0xFF) ? 0 : 0x004u;      /* PF: even parity */
    fl |= r ? 0 : 0x040u;                               /* ZF */
    fl |= (r & sign) ? 0x080u : 0;                      /* SF */
    return (old & ~0x8D5u) | fl;
}

bool nc_x86mmio_hook(NcKernel *k, NcFrame *f, uint32_t eip)
{
    if (!nc_x86mmio_is_site(eip)) {
        return false;
    }
    Op op;
    if (!decode(k, f, nc_vm_host(k->vm, eip), &op)) {
        drop_site(k, eip);
        nc_frame_bypass_hook(f);
        return true;
    }
    const int reg = op.reg < 0 ? NC_EAX : op.reg;
    uint64_t v = op.store ? (op.imm ? op.immv : get_reg(f, reg, op.bytes)) : 0;
    if (!nc_mmio_access(op.addr, op.bytes, op.store, &v)) {
        /* Ordinary memory this time: the translated instruction does it. */
        drop_site(k, eip);
        nc_frame_bypass_hook(f);
        return true;
    }
    if (op.store && nc_mmio_take_retry()) {
        return true;    /* EIP stays: it stores again, interrupts taken between */
    }
    if (op.cmp) {
        nc_frame_set_eflags(f, cmp_flags(nc_frame_eflags(f), op.cmp, (uint32_t)v, op.immv, op.bytes));
        nc_frame_set_eip(f, eip + op.len);
        return true;
    }
    if (!op.store) {
        if (op.zx) {
            nc_frame_set_gpr(f, reg, (uint32_t)v);
        } else {
            set_reg(f, reg, op.bytes, (uint32_t)v);
        }
    }
    nc_frame_set_eip(f, eip + op.len);
    return true;
}

void nc_x86mmio_reset(void)
{
    memset(g_site, 0, sizeof(g_site));
    memset(g_strikes, 0, sizeof(g_strikes));
    memset(g_pending, 0, sizeof(g_pending));
    g_npending = 0;
}
