/*
 * hakuX HLE GPU — runtime state, posture, logging, diagnostics.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/core/probe.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/prof.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/vk/back.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

HleRuntime g_hle;
char g_hle_unavailable[256];
static HleTimer *g_diag_timer;

void hle_log(const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, tag, "%s", buf);
#else
    fprintf(stderr, "%s: %s\n", tag, buf);
#endif
}

const char *hle_posture_name(HlePosture p)
{
    switch (p) {
    case HLE_POSTURE_REPLACE: return "replace";
    default:                  return "off";
    }
}

static HlePosture parse_posture(const char *m)
{
    if (!m) {
        return HLE_POSTURE_OFF;
    }
    if (strcmp(m, "replace") == 0) {
        return HLE_POSTURE_REPLACE;
    }
    return HLE_POSTURE_OFF;
}

/* HAKUX_HLE_PCHIST=1 (debug): the vCPU's EIP sampled every millisecond
 * (racy: the last TB exit), title code only, top entries logged each second */
#define PCH_SLOTS 4096
static struct { uint32_t pc, n; } g_pch[PCH_SLOTS];
static HleTimer *g_pch_timer;
static uint64_t g_pch_samples, g_pch_other;

static void pch_tick(void *opaque)
{
    if (hle_host_cpu()) {
        uint32_t pc = hle_host_cpu()->eip;
        g_pch_samples++;
        static int64_t s_reg_pc = -2;       /* HAKUX_HLE_PCREG=<hex pc>: its EAX */
        static unsigned s_reg_n;
        if (s_reg_pc == -2) {
            const char *e = getenv("HAKUX_HLE_PCREG");
            s_reg_pc = e ? (int64_t)strtoul(e, NULL, 16) : -1;
        }
        if (s_reg_pc >= 0 && pc == (uint32_t)s_reg_pc && s_reg_n < 8) {
            s_reg_n++;
            HleCpu *env = hle_host_cpu();
            hle_log("hakuX-hle-hooks", "pcreg %08x eax=%08x ecx=%08x esi=%08x", pc,
                    (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                    (uint32_t)env->regs[R_ESI]);
        }
        if (pc >= 0x11000 && pc < 0x80000000u) {
            unsigned i = (pc * 2654435761u) >> 20 & (PCH_SLOTS - 1);
            for (unsigned k = 0; k < 16; k++, i = (i + 1) & (PCH_SLOTS - 1)) {
                if (!g_pch[i].n || g_pch[i].pc == pc) {
                    g_pch[i].pc = pc;
                    g_pch[i].n++;
                    break;
                }
            }
        } else {
            g_pch_other++;
        }
    }
    hle_timer_mod(g_pch_timer, hle_now_ms() + 1);
}

static void pch_report(void)
{
    if (!g_pch_timer) {
        return;
    }
    char buf[600];
    int m = snprintf(buf, sizeof(buf), "pchist samples=%llu kernel/other=%llu:",
                     (unsigned long long)g_pch_samples, (unsigned long long)g_pch_other);
    for (unsigned t = 0; t < 12 && m < (int)sizeof(buf) - 24; t++) {
        unsigned best = PCH_SLOTS;
        for (unsigned i = 0; i < PCH_SLOTS; i++) {
            if (g_pch[i].n && (best == PCH_SLOTS || g_pch[i].n > g_pch[best].n)) {
                best = i;
            }
        }
        if (best == PCH_SLOTS) {
            break;
        }
        m += snprintf(buf + m, sizeof(buf) - m, " %08x:%u", g_pch[best].pc, g_pch[best].n);
        g_pch[best].n = 0;
    }
    hle_log("hakuX-hle-hooks", "%s", buf);
    memset(g_pch, 0, sizeof(g_pch));
    g_pch_samples = g_pch_other = 0;
}

/* HAKUX_HLE_PEEK=va:len[,va:len] (debug): guest memory logged each second */
static void peek_report(void)
{
    const char *e = getenv("HAKUX_HLE_PEEK");
    if (!e || !hle_host_cpu()) {
        return;
    }
    char *dup = g_strdup(e), *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (tok[0] == '#') {                /* #va:stride:n = nonzero dwords counted */
            uint32_t cva = strtoul(tok + 1, NULL, 16), stride = 4, cnt = 1, used = 0;
            char *c1 = strchr(tok, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c1 && c2) {
                stride = strtoul(c1 + 1, NULL, 16);
                cnt = MIN(strtoul(c2 + 1, NULL, 16), 4096u);
            }
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t v = 0;
                hle_va_rw(cva + i * stride, &v, 4, false);
                used += v != 0;
            }
            hle_log("hakuX-hle-hooks", "peekcount %08x: %u/%u", cva, used, cnt);
            continue;
        }
        bool ind = tok[0] == '*';           /* *va:len = through the pointer at va */
        uint32_t va = strtoul(tok + ind, NULL, 16), len = 64;
        if (ind) {
            hle_va_rw(va, &va, 4, false);
        }
        char *c = strchr(tok, ':');
        if (c) {
            len = MIN(strtoul(c + 1, NULL, 16), 0x400u);
        }
        for (uint32_t off = 0; off < len; off += 32) {
            uint32_t w[8] = { 0 };
            hle_va_rw(va + off, w, sizeof(w), false);
            hle_log("hakuX-hle-hooks", "peek %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    va + off, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    g_free(dup);
}

static void diag_tick(void *opaque)
{
    pch_report();
    peek_report();
    hle_pb_diag();
    hle_prof_diag();
    /* HAKUX_HLE_PCLOG=1: where the vCPU is (racy sample, debug only) */
    static int pclog = -1;
    if (pclog < 0) {
        pclog = getenv("HAKUX_HLE_PCLOG") != NULL;
    }
    if (pclog && hle_host_cpu()) {
        HleCpu *env = hle_host_cpu();
        hle_log("hakuX-hle-hooks", "pc=%08x esp=%08x if=%d halted=%d",
                (uint32_t)env->eip, (uint32_t)env->regs[R_ESP],
                (env->eflags & HLE_EFLAGS_IF) != 0, env->halted);
        uint8_t code[48];
        uint32_t stk[32];
        if (hle_va_rw((uint32_t)env->eip - 16, code, sizeof(code), false) == 0 &&
            hle_va_rw((uint32_t)env->regs[R_ESP], stk, sizeof(stk), false) == 0) {
            char hx[sizeof(code) * 2 + 1], sx[sizeof(stk) / 4 * 9 + 1];
            for (unsigned i = 0; i < sizeof(code); i++) {
                snprintf(hx + 2 * i, 3, "%02x", code[i]);
            }
            for (unsigned i = 0; i < ARRAY_SIZE(stk); i++) {
                snprintf(sx + 9 * i, 10, "%08x ", stk[i]);
            }
            hle_log("hakuX-hle-hooks", "code@pc-16 %s", hx);
            hle_log("hakuX-hle-hooks", "stack %s", sx);
            hle_log("hakuX-hle-hooks", "eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x "
                    "ebp=%08x", (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_ECX],
                    (uint32_t)env->regs[R_EDX], (uint32_t)env->regs[R_EBX],
                    (uint32_t)env->regs[R_ESI], (uint32_t)env->regs[R_EDI],
                    (uint32_t)env->regs[R_EBP]);
        }
    }
    hle_timer_mod(g_diag_timer, hle_now_ms() + 1000);
}

bool g_hle_prof_on;
uint64_t g_hle_prof_ns[HLE_PROF_N], g_hle_prof_cnt[HLE_PROF_N];
double g_hle_prof_ns_per_tick = 1.0;

void hle_prof_init(void)
{
    g_hle_prof_on = getenv("HAKUX_HLE_PROF") != NULL;
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    g_hle_prof_ns_per_tick = f ? 1e9 / (double)f : 1.0;
#endif
}

/* On demand (debug): a new value of the property debug.hakux.perf turns the
 * profile on for debug.hakux.perfsec seconds (default 10); the late-frame
 * report and GPU times follow the profile. */
#ifdef __ANDROID__
/* Guest PC sampler (debug, during a perf capture): ~1000 times a second the
 * vCPU's EIP (racy: synced at translation-block boundaries) or "halted";
 * the hottest EIPs are logged when the capture ends. */
#define SMP_SLOTS 4096
static struct {
    HleThread th;
    bool started;
    volatile bool stop;
    uint32_t eip[SMP_SLOTS], cnt[SMP_SLOTS];
    uint64_t n, halted, dropped;
} g_smp;

static void *smp_thread(void *opaque)
{
    while (!g_smp.stop) {
        HleCpu *cpu = hle_host_cpu();
        if (cpu) {
            g_smp.n++;
            if (hle_atomic_read(&cpu->halted)) {
                g_smp.halted++;
            } else {
                HleCpu *env = cpu;
                uint32_t eip = (uint32_t)(env->eip);
                if (g_hle_pc_sampler && !g_hle_pc_sampler(&eip)) {
                    eip = (uint32_t)(env->eip);
                } else if (g_hle_pc_sampler && eip == HLE_PC_IDLE) {
                    g_smp.halted++;
                    g_usleep(1000);
                    continue;
                }
                unsigned i = (eip * 2654435761u) >> 20 & (SMP_SLOTS - 1), k = 0;
                while (k < SMP_SLOTS && g_smp.cnt[i] && g_smp.eip[i] != eip) {
                    i = (i + 1) & (SMP_SLOTS - 1);
                    k++;
                }
                if (k == SMP_SLOTS) {
                    g_smp.dropped++;
                } else {
                    g_smp.eip[i] = eip;
                    g_smp.cnt[i]++;
                }
            }
        }
        g_usleep(1000);
    }
    return NULL;
}

static void smp_start(void)
{
    if (g_smp.started) {
        return;
    }
    memset(g_smp.eip, 0, sizeof(g_smp.eip));
    memset(g_smp.cnt, 0, sizeof(g_smp.cnt));
    g_smp.n = g_smp.halted = g_smp.dropped = 0;
    g_smp.stop = false;
    hle_thread_create(&g_smp.th, "hle-sampler", smp_thread, NULL, HLE_THREAD_JOINABLE);
    g_smp.started = true;
}

static void smp_stop(void)
{
    if (!g_smp.started) {
        return;
    }
    g_smp.stop = true;
    hle_thread_join(&g_smp.th);
    g_smp.started = false;
    uint64_t n = MAX(g_smp.n, 1);
    hle_log("hakuX-hle-prof", "eip samples=%llu halted=%.1f%% dropped=%llu",
            (unsigned long long)g_smp.n, 100.0 * g_smp.halted / n,
            (unsigned long long)g_smp.dropped);
    for (unsigned r = 0; r < 40; r++) {         /* top 40, selection by count */
        unsigned best = SMP_SLOTS;
        for (unsigned i = 0; i < SMP_SLOTS; i++) {
            if (g_smp.cnt[i] && (best == SMP_SLOTS || g_smp.cnt[i] > g_smp.cnt[best])) {
                best = i;
            }
        }
        if (best == SMP_SLOTS) {
            break;
        }
        const uint32_t e = g_smp.eip[best];
        if (g_hle_pc_sampler && (e & HLE_PC_HOST) == HLE_PC_HOST) {
            /* host code: the offset in the core library (addr2line it) */
            hle_log("hakuX-hle-prof", "  host+%06x %5u %.1f%% (%s)", (e & ~(HLE_PC_HOST | HLE_PC_SYSOP)) << 4,
                    g_smp.cnt[best], 100.0 * g_smp.cnt[best] / n, e & HLE_PC_SYSOP ? "kernel call / hook" : "FEX");
        } else {
            hle_log("hakuX-hle-prof", "  eip %08x %5u %.1f%%", e, g_smp.cnt[best],
                    100.0 * g_smp.cnt[best] / n);
        }
        g_smp.cnt[best] = 0;
    }
}
#endif

static void prof_poll(void)
{
#ifdef __ANDROID__
    static HleProp prop;
    static int64_t until;
    static int env_on = -1;             /* HAKUX_HLE_PROF at boot */
    static bool armed;
    if (env_on < 0) {
        env_on = g_hle_prof_on;
    }
    char v[PROP_VALUE_MAX];
    if (hle_prop_new(&prop, "debug.hakux.perf", v)) {
        char s[PROP_VALUE_MAX] = "";
        hle_prop_get("debug.hakux.perfsec", s);
        int sec = atoi(s) > 0 ? atoi(s) : 10;
        until = hle_now_ns() + (int64_t)sec * 1000000000;
        g_hle_prof_on = armed = true;
        smp_start();
        hle_log("hakuX-hle-prof", "perf capture on for %d s", sec);
    }
    if (armed && hle_now_ns() >= until) {
        armed = false;
        g_hle_prof_on = env_on != 0;
        smp_stop();
        hle_log("hakuX-hle-prof", "perf capture off");
    }
#endif
}

/* Per second: milliseconds of host time per bucket (calls). */
void hle_prof_diag(void)
{
    static uint64_t ns0[HLE_PROF_N], n0[HLE_PROF_N];
    prof_poll();
    if (!g_hle_prof_on) {
        return;
    }
    static const char *const k_name[HLE_PROF_N] = {"back", "submit"};
    char buf[512];
    int off = 0;
    for (int k = 0; k < HLE_PROF_N; k++) {
        uint64_t ns = g_hle_prof_ns[k], n = g_hle_prof_cnt[k];
        off += snprintf(buf + off, sizeof(buf) - off, "%s%s=%.1fms(%llu)", k ? " " : "",
                        k_name[k], (ns - ns0[k]) * g_hle_prof_ns_per_tick / 1e6,
                        (unsigned long long)(n - n0[k]));
        ns0[k] = ns;
        n0[k] = n;
    }
    hle_log("hakuX-hle-prof", "%s", buf);
}


void hle_init(void)
{
    memset(&g_hle, 0, sizeof(g_hle));
    const char *mode = getenv("HAKUX_HLE_MODE");
    g_hle.posture = parse_posture(mode ? mode : hle_host_hle_mode());
    hle_prof_init();
    hle_barrier_init(hle_ram_size());
    hle_log("hakuX-hle", "init posture=%s", hle_posture_name(g_hle.posture));
    if (hle_active()) {
        g_diag_timer = hle_timer_new(diag_tick, NULL);
        hle_timer_mod(g_diag_timer, hle_now_ms() + 1000);
        if (getenv("HAKUX_HLE_PCHIST")) {
            g_pch_timer = hle_timer_new(pch_tick, NULL);
            hle_timer_mod(g_pch_timer, hle_now_ms() + 1);
        }
    }
}

void hle_shutdown(void)
{
    hle_timers_stop();                  /* first: diag_tick reads the renderer */
    g_diag_timer = g_pch_timer = NULL;
    hle_title_stop();                   /* the push-buffer decoder joined */
    hle_back_shutdown();
    hle_hitch_stop();                   /* the threads it watches gone */
    memset(g_pch, 0, sizeof(g_pch));
    g_pch_samples = g_pch_other = 0;
#ifdef __ANDROID__
    smp_stop();
#endif
    hle_pb_reset();                     /* after the readers of its counters */
    hle_probe_reset();
    hle_barrier_free();
    hle_guestmem_reset();               /* last: the teardown's landings wrote guest RAM */
    g_hle_unavailable[0] = 0;
    memset(&g_hle, 0, sizeof(g_hle));
}
