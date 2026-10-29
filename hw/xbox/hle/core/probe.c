/*
 * hakuX HLE — debug probes on title code (probe.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/core/probe.h"
#include "hw/xbox/hle/prop.h"

#define MAX_PROBES 8

static struct {
    uint32_t va, first;
    uint64_t hits;
} g_probe[MAX_PROBES];

/* Whether va is in an executable section of the title's XBE. */
static bool in_code(uint32_t va)
{
    const HleXbeInfo *x = &g_hle.title.xbe;
    for (unsigned i = 0; i < x->nsections; i++) {
        if ((x->sections[i].flags & 4) && va - x->sections[i].va < x->sections[i].vsize) {
            return true;
        }
    }
    return false;
}

/* debug (HAKUX_HLE_PROBE_STACK=1): the words of the stack that look like
 * return addresses: in the XBE's code, right after a call (E8 rel32, or
 * FF /2 with a register, a [reg+disp8] or a [disp32/reg+disp32] operand). */
static void backtrace(uint32_t esp, uint32_t va)
{
    static uint32_t st[512];
    unsigned words = 512;
    while (words >= 16 && hle_va_rw(esp, st, words * 4, false)) {
        words /= 2;                     /* the stack's top page is nearer */
    }
    char line[400] = "";
    int o = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < words && n < 24 && o < (int)sizeof(line) - 12; i++) {
        const uint32_t r = st[i];
        uint8_t b[7];
        if (!in_code(r) || hle_va_rw(r - 7, b, sizeof(b), false)) {
            continue;
        }
        const bool call = b[2] == 0xE8 || (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] >> 6) == 3) ||
                          (b[4] == 0xFF && (b[5] & 0x38) == 0x10 && (b[5] >> 6) == 1) ||
                          (b[1] == 0xFF && (b[2] & 0x38) == 0x10 && ((b[2] >> 6) == 2 || (b[2] & 0xC7) == 5));
        if (call) {
            o += snprintf(line + o, sizeof(line) - (size_t)o, " +%x:%x", i * 4, r);
            n++;
        }
    }
    hle_log("hakuX-hle-probe", "%08x stack:%s", va, line);
}

bool hle_probe_at(uint32_t va)
{
    for (unsigned i = 0; i < MAX_PROBES && g_probe[i].va; i++) {
        if (g_probe[i].va == va) {
            return true;
        }
    }
    return false;
}

void hle_probe_hit(const HleCpu *c)
{
    const uint32_t va = c->eip;
    static int stack = -1;
    static uint32_t word_va;
    if (stack < 0) {
        const char *e = getenv("HAKUX_HLE_PROBE_STACK");
        stack = e && atoi(e) > 0;
        /* debug: HAKUX_HLE_PROBE_WORD=<va> (hex), that guest word with each hit */
        e = getenv("HAKUX_HLE_PROBE_WORD");
        word_va = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
    }
#ifdef __ANDROID__
    /* debug.hakux.probe, a new value: each probe logs its first hits again
     * (a scene reached after the boot spent them) */
    static HleProp prop;
    static unsigned polls;
    char v[PROP_VALUE_MAX];
    if ((polls++ & 1023) == 0 && hle_prop_new(&prop, "debug.hakux.probe", v)) {
        for (unsigned i = 0; i < MAX_PROBES; i++) {
            g_probe[i].hits = 0;
        }
        hle_log("hakuX-hle-probe", "re-armed (%s)", v);
    }
#endif
    for (unsigned i = 0; i < MAX_PROBES; i++) {
        if (g_probe[i].va != va) {
            continue;
        }
        uint64_t n = ++g_probe[i].hits;
        if (n <= g_probe[i].first || n % 1000 == 0) {
            uint32_t st[8] = { 0 };
            hle_va_rw(c->regs[R_ESP], st, sizeof(st), false);
            hle_log("hakuX-hle-probe", "%08x #%llu eax %08x ecx %08x edx %08x ebx %08x esi %08x "
                    "edi %08x ebp %08x | esp %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    va, (unsigned long long)n, c->regs[R_EAX], c->regs[R_ECX],
                    c->regs[R_EDX], c->regs[R_EBX], c->regs[R_ESI], c->regs[R_EDI],
                    c->regs[R_EBP], c->regs[R_ESP], st[0], st[1], st[2], st[3], st[4], st[5],
                    st[6], st[7]);
            if (stack) {
                backtrace(c->regs[R_ESP], va);
            }
            uint32_t w = 0;
            if (word_va && !hle_va_rw(word_va, &w, 4, false)) {
                hle_log("hakuX-hle-probe", "%08x #%llu word %08x = %08x", va, (unsigned long long)n,
                        word_va, w);
            }
        }
        break;
    }
}

unsigned hle_probe_register(void)
{
    const char *e = getenv("HAKUX_HLE_PROBE");
    unsigned n = 0;
    memset(g_probe, 0, sizeof(g_probe));
    for (const char *p = e; p && *p && n < MAX_PROBES;) {
        char *end;
        uint32_t va = (uint32_t)strtoul(p, &end, 16);
        uint32_t first = 16;
        if (*end == ':') {
            first = (uint32_t)strtoul(end + 1, &end, 10);
        }
        if (va && !hle_probe_at(va)) {
            g_probe[n].va = va;
            g_probe[n].first = first;
            n++;
            hle_log("hakuX-hle-probe", "probe at %08x (first %u logged)", va, first);
        }
        p = *end == ',' ? end + 1 : end + strlen(end);
    }
    return n;
}

void hle_probe_reset(void)
{
    memset(g_probe, 0, sizeof(g_probe));
}
