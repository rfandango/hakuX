/*
 * hakuX HLE GPU — stutter / hang monitor (see hitch.h).
 *
 * Cost: activity spans at rare transitions (a counter read each), one
 * snapshot per guest flip (two thread-CPU clock reads, a short lock), and a
 * watchdog thread that wakes every 10 ms, or every millisecond only while a
 * frame is overdue (vCPU EIP / halted samples for the event's report).
 */
#include "hw/xbox/hle/base.h"
#include <pthread.h>
#include <time.h>
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/vk/back.h"

HleHitchThr g_hitch_thr[HLE_HT_N];
__thread int8_t t_hitch = -1;
bool g_hitch_on;

#define TAG          "hakuX-hle-hitch"
#define EIP_SLOTS    64
#define TICK_IDLE_US 10000
#define TICK_SMP_US  1000

/* Everything a frame window is measured against: taken at each flip. */
typedef struct Snap {
    int64_t t;                          /* counter ticks */
    int64_t us;                         /* monotonic, for "ago" */
    int64_t acc[HLE_HT_N][HLE_ACT_N];   /* in-progress spans included */
    uint32_t n[HLE_HT_N][HLE_ACT_N];
    uint64_t draws, swaps, vblanks, tl_sub, tl_done;
    uint64_t dvd_ops, dvd_bytes, dvd_ns;
    unsigned jit;
    int64_t cpu_ns[HLE_HT_N];
} Snap;

typedef struct Eip {
    uint32_t eip[EIP_SLOTS], cnt[EIP_SLOTS];
    uint32_t n, halted;
    uint32_t host_jit, host_call;       /* host code samples: FEX / kernel calls */
} Eip;

typedef struct Last {
    bool valid;
    int64_t start_us;
    float ms;
    char reason[96];
} Last;

/* A frame's timeline (monotonic us), by swap id. */
#define FR_RING 256
typedef struct FrameT {
    uint32_t id;
    int64_t t[HLE_FR_N];
} FrameT;

/* Guest windows that ran long, for the display path's attribution. */
#define GW_RING 8
typedef struct GuestWin {
    uint32_t id;
    float ms;
    char reason[96];
    char det[960];
} GuestWin;

static struct {
    HleMutex mx;                       /* the window, samples and status */
    bool started, init;
    float stutter_ms_env, hang_ms;
    clockid_t clk[HLE_HT_N];
    bool clk_ok[HLE_HT_N];
    /* the open window */
    bool have_win, paused;
    Snap win;
    uint64_t win_id;
    Eip eip;                            /* samples of the open window */
    /* live hang (watchdog) */
    bool live;
    int64_t live_logged_us;
    char live_reason[96];
    /* status */
    unsigned stutters, hangs;
    Last last_hang, last_stutter;
    /* windows closed by the decoder (hle_hitch_pb_swap) */
    uint64_t swaps;
    float swap_ms;                      /* the guest's recent frame time (average) */
    int64_t gpu_done_t;
    HleThread th;
    int quit;                           /* hle_hitch_stop (atomic) */
    /* display path */
    FrameT fr[FR_RING];
    GuestWin gw_ring[GW_RING];
    unsigned gw_next;
    uint32_t shown_id;                  /* the last frame the UI picked up */
    int64_t shown_front, shown_us;      /* its front and pickup times */
    int64_t shown_comp;                 /* the render thread's compile time then */
    uint32_t shown_gmark;               /* debug: the GPU event trace's position then */
    unsigned dropped;
    uint8_t cad[16];                    /* recent front intervals, in vblanks */
    unsigned ncad;
    double last_fint;                   /* the last front interval, ms */
    unsigned gtrace_left;               /* debug: GPU event traces still to log */
} g_h;

static inline double t2ms(int64_t ticks)
{
    return ticks * g_hle_prof_ns_per_tick / 1e6;
}

void hle_hitch_thread(int slot)
{
    t_hitch = (int8_t)slot;
    HleHitchThr *h = &g_hitch_thr[slot];
    memset(h, 0, sizeof(*h));
    h->since = hle_prof_now();
    h->act = HLE_ACT_RUN;
    g_h.clk_ok[slot] = pthread_getcpuclockid(pthread_self(), &g_h.clk[slot]) == 0;
}

void hle_hitch_gpu_done(void)
{
    __atomic_store_n(&g_h.gpu_done_t, hle_prof_now(), __ATOMIC_RELAXED);
}

static int64_t cpu_ns(int slot)
{
    struct timespec ts;
    if (!g_h.clk_ok[slot] || clock_gettime(g_h.clk[slot], &ts)) {
        return 0;
    }
    return ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Racy reads of the other threads' counters: diagnostics only. */
static void snap_take(Snap *s)
{
    s->t = hle_prof_now();
    s->us = g_get_monotonic_time();
    for (int k = 0; k < HLE_HT_N; k++) {
        HleHitchThr *h = &g_hitch_thr[k];
        uint32_t act = __atomic_load_n(&h->act, __ATOMIC_ACQUIRE);
        int64_t since = __atomic_load_n(&h->since, __ATOMIC_RELAXED);
        for (int a = 0; a < HLE_ACT_N; a++) {
            s->acc[k][a] = h->acc[a];
            s->n[k][a] = h->n[a];
        }
        if (act < HLE_ACT_N && since && s->t > since) {
            s->acc[k][act] += s->t - since;
        }
        s->cpu_ns[k] = cpu_ns(k);
    }
    uint64_t ff, skipped, tex_defs;
    hle_pbd_draw_counts(&s->draws, &ff, &skipped, &tex_defs);
    s->swaps = g_h.swaps;
    s->vblanks = hle_host_vblank_count();
    s->tl_sub = hle_tl_submitted();
    s->tl_done = hle_tl_completed();
    if (!hle_host_disc_stats(&s->dvd_ops, &s->dvd_bytes, &s->dvd_ns)) {
        s->dvd_ops = s->dvd_bytes = s->dvd_ns = 0;
    }
    s->jit = hle_host_code_flushes();
}

static float vblank_ms(void)
{
    int64_t p = hle_host_vblank_period_ns();
    return p > 0 ? p / 1e6f : 16.68f;
}

/* The title's frame interval as the screen shows it: the median of the
 * recent front intervals in whole vblanks (a title pacing itself, such as
 * PGR2's movies at 30 fps on interval 1, is not late every frame); 0 until
 * there are enough. */
static float cad_median(void)
{
    unsigned n = MIN(g_h.ncad, (unsigned)ARRAY_SIZE(g_h.cad));
    if (n < 8) {
        return 0.0f;
    }
    unsigned cnt[9] = { 0 }, half = n / 2, acc = 0;
    for (unsigned i = 0; i < n; i++) {
        cnt[MIN(g_h.cad[i], 8u)]++;
    }
    for (unsigned q = 1; q <= 8; q++) {
        acc += cnt[q];
        if (acc > half) {
            return q * vblank_ms();
        }
    }
    return 0.0f;
}

/* The frame time: there is no presentation interval to go by (the title's
 * own D3D paces its flips), so the cadence, a vblank until it is known. */
static float expected_ms(void)
{
    float c = cad_median();
    return c ? c : vblank_ms();
}

/* a frame reaching the front this much after the previous one is late (a
 * missed vblank), the guest window explained for it likewise */
static float late_ms(void)
{
    return g_h.stutter_ms_env > 0 ? g_h.stutter_ms_env : expected_ms() + 0.5f * vblank_ms();
}

/* the UI picked up a frame within the last second: the display path counts */
static bool display_active(int64_t now_us)
{
    int64_t s = __atomic_load_n(&g_h.shown_us, __ATOMIC_RELAXED);
    return s && now_us - s < 1000000;
}

static float stutter_ms(void)
{
    return g_h.stutter_ms_env > 0 ? g_h.stutter_ms_env : MAX(2.0f * expected_ms(), 50.0f);
}

bool (*g_hle_pc_sampler)(uint32_t *pc);

static bool is_host(uint32_t eip)
{
    return g_hle_pc_sampler && (eip & HLE_PC_HOST) == HLE_PC_HOST;
}

static const char *region(uint32_t eip)
{
    return is_host(eip) ? (eip & HLE_PC_SYSOP ? "host call" : "host JIT") : eip >= 0x80000000u ? "kernel" :
           hle_title_in_lib(eip) ? "D3D" : "title";
}

/* as printed: a host sample is its offset in the core library */
static uint32_t shown(uint32_t eip)
{
    return is_host(eip) ? (eip & ~(HLE_PC_HOST | HLE_PC_SYSOP)) << 4 : eip;
}

/* the three most sampled EIPs (0 = none) */
static void eip_top3(const Eip *e, uint32_t eip[3], uint32_t cnt[3])
{
    uint32_t c[EIP_SLOTS];
    memcpy(c, e->cnt, sizeof(c));
    for (unsigned r = 0; r < 3; r++) {
        unsigned best = 0;
        for (unsigned i = 1; i < EIP_SLOTS; i++) {
            best = c[i] > c[best] ? i : best;
        }
        eip[r] = c[best] ? e->eip[best] : 0;
        cnt[r] = c[best];
        c[best] = 0;
    }
}

/* The reason (short, for the overlay) and the full breakdown (log) of the
 * window a -> b of w ms. */
static void explain(const Snap *a, const Snap *b, const Eip *e, double w, char *r, size_t rn,
                    char *d, size_t dn)
{
#define D(k, act) t2ms(b->acc[k][act] - a->acc[k][act])
    double comp = D(HLE_HT_RENDER, HLE_ACT_COMPILE), sub = D(HLE_HT_RENDER, HLE_ACT_SUBMIT);
    double rgpu = D(HLE_HT_RENDER, HLE_ACT_GPU), disp = D(HLE_HT_RENDER, HLE_ACT_DISPLAY);
    double idle = D(HLE_HT_RENDER, HLE_ACT_IDLE), rrun = D(HLE_HT_RENDER, HLE_ACT_RUN);
    /* the decoder: its waits on the guest and the back end */
    double dring = D(HLE_HT_DECODE, HLE_ACT_RING), darena = D(HLE_HT_DECODE, HLE_ACT_ARENA);
    double dgpu = D(HLE_HT_DECODE, HLE_ACT_GPU), dflip = D(HLE_HT_DECODE, HLE_ACT_FLIP);
    double dtrap = D(HLE_HT_DECODE, HLE_ACT_TRAP), didle = D(HLE_HT_DECODE, HLE_ACT_IDLE);
#undef D
    double dcpu = (b->cpu_ns[HLE_HT_DECODE] - a->cpu_ns[HLE_HT_DECODE]) / 1e6;
    unsigned ncomp = b->n[HLE_HT_RENDER][HLE_ACT_COMPILE] - a->n[HLE_HT_RENDER][HLE_ACT_COMPILE];
    double vcpu = (b->cpu_ns[HLE_HT_VCPU] - a->cpu_ns[HLE_HT_VCPU]) / 1e6;
    double rcpu = (b->cpu_ns[HLE_HT_RENDER] - a->cpu_ns[HLE_HT_RENDER]) / 1e6;
    unsigned dops = (unsigned)(b->dvd_ops - a->dvd_ops);
    double dmb = (b->dvd_bytes - a->dvd_bytes) / 1048576.0, dms = (b->dvd_ns - a->dvd_ns) / 1e6;
    unsigned jit = b->jit - a->jit;
    bool gpu_stuck = b->tl_done == a->tl_done && a->tl_sub > a->tl_done;
    double halted = e->n ? 100.0 * e->halted / e->n : -1.0;
    int64_t done_t = __atomic_load_n(&g_h.gpu_done_t, __ATOMIC_RELAXED);
    double since_done = done_t ? t2ms(b->t - done_t) : -1.0;
    uint32_t ev[3], ec[3];
    eip_top3(e, ev, ec);
    uint32_t e0 = ev[0], e1 = ev[1], e2 = ev[2], c0 = ec[0], c1 = ec[1], c2 = ec[2];

    double lim = 0.4 * w;
    if (gpu_stuck && rgpu >= lim) {
        snprintf(r, rn, "GPU stalled: no progress for %.0f ms (%llu submits pending)",
                 since_done, (unsigned long long)(b->tl_sub - b->tl_done));
    } else if (comp >= lim) {
        snprintf(r, rn, "pipeline compiles %.0f ms (%u)", comp, ncomp);
    } else if (dring + darena + dgpu >= lim) {
        const char *who = dring >= darena && dring >= dgpu ? "command ring full (render thread)" :
                          darena >= dgpu ? "upload arena full (GPU)" : "waiting for the GPU";
        snprintf(r, rn, "decoder blocked %.0f ms: %s", dring + darena + dgpu, who);
    } else if (dflip >= lim) {
        snprintf(r, rn, "decoder waited %.0f ms for the flip (display)", dflip);
    } else if (dtrap >= lim) {
        snprintf(r, rn, "decoder waited %.0f ms on the guest's GPU interrupt handler", dtrap);
    } else if (dcpu >= 0.6 * w && didle < 0.2 * w) {
        snprintf(r, rn, "decoder busy: %.0f ms CPU decoding the push buffer", dcpu);
    } else if (dops && (dms >= 0.25 * w || (halted >= 40.0))) {
        snprintf(r, rn, "disc reads: %u, %.1f MB, %.0f ms", dops, dmb, dms);
    } else if (jit) {
        snprintf(r, rn, "JIT cache flush (%u)", jit);
    } else if (halted >= 50.0) {
        snprintf(r, rn, "guest idle: vCPU halted %.0f%%", halted);
    } else if (vcpu > 0 && vcpu < 0.5 * w) {
        snprintf(r, rn, "vCPU starved: %.0f ms CPU of %.0f (host)", vcpu, w);
    } else if (e->n >= 8 && e->host_jit * 100 >= 40u * e->n) {
        snprintf(r, rn, "JIT compiling: %u%% of the vCPU in FEX", 100 * e->host_jit / e->n);
    } else if (e0) {
        snprintf(r, rn, "%s: %s %08x (%u%%)", is_host(e0) ? "vCPU in host code" : "guest CPU-bound",
                 region(e0), shown(e0), e->n ? 100 * c0 / e->n : 0);
    } else {
        snprintf(r, rn, "guest CPU-bound (%llu draws)", (unsigned long long)(b->draws - a->draws));
    }
    snprintf(d, dn,
             "vCPU cpu %.0f ms halted %.0f%% | render cpu %.0f run %.0f idle %.0f "
             "compile %.0f (%u) submit %.0f (%u) gpu %.0f display %.0f | "
             "GPU %llu/%llu, last done %.0f ms ago | swaps %llu vblanks %llu draws %llu "
             "| dvd %u ops %.1f MB %.0f ms | jit %u | eip %08x(%s) %u, "
             "%08x %u, %08x %u of %u, host: FEX %u kernel %u | decoder cpu %.0f idle %.0f "
             "flip %.0f trap %.0f ring %.0f arena %.0f gpu %.0f",
             vcpu, halted, rcpu, rrun, idle, comp, ncomp, sub,
             b->n[HLE_HT_RENDER][HLE_ACT_SUBMIT] - a->n[HLE_HT_RENDER][HLE_ACT_SUBMIT], rgpu,
             disp, (unsigned long long)b->tl_done, (unsigned long long)b->tl_sub, since_done,
             (unsigned long long)(b->swaps - a->swaps),
             (unsigned long long)(b->vblanks - a->vblanks),
             (unsigned long long)(b->draws - a->draws), dops, dmb, dms, jit, shown(e0),
             region(e0), c0, shown(e1), c1, shown(e2), c2, e->n, e->host_jit, e->host_call,
             dcpu, didle, dflip, dtrap, dring, darena, dgpu);
}

static void init_once(void)
{
    if (g_h.init) {
        return;
    }
    g_h.init = true;
    hle_mutex_init(&g_h.mx);
    const char *e = getenv("HAKUX_HLE_HITCH_MS");
    g_h.stutter_ms_env = e ? atof(e) : 0.0f;
    e = getenv("HAKUX_HLE_HANG_MS");
    g_h.hang_ms = e && atof(e) > 0 ? atof(e) : 500.0f;
    e = getenv("HAKUX_HLE_HITCH_GTRACE");
    g_h.gtrace_left = e ? (unsigned)atoi(e) : 0;
}

/* ---- watchdog ------------------------------------------------------------------ */

static void hexs(char *b, size_t n, const uint8_t *p, unsigned len)
{
    size_t o = 0;
    for (unsigned i = 0; i < len && o + 2 < n; i++) {
        o += snprintf(b + o, n - o, "%02x", p[i]);
    }
}

/* The vCPU as a hang starts (racy reads, logged once per hang): registers,
 * interrupt state, the kernel IRQL (KPCR + 0x24), code at EIP, the stack. */
static void hang_snapshot(void)
{
    HleCpu *cpu = hle_host_cpu();
    if (!cpu) {
        return;
    }
    HleCpu *env = cpu;
    uint32_t eip = (uint32_t)(env->eip);
    uint32_t esp = (uint32_t)env->regs[R_ESP], fs = (uint32_t)env->fs_base;
    uint8_t irql = 0xFF, code[48] = { 0 };
    uint32_t stk[24] = { 0 };
    hle_va_rw(fs + 0x24, &irql, 1, false);
    hle_va_rw(eip - 16, code, sizeof(code), false);
    hle_va_rw(esp, (uint8_t *)stk, sizeof(stk), false);
    char hx[2 * sizeof(code) + 1], sk[24 * 9 + 1];
    hexs(hx, sizeof(hx), code, sizeof(code));
    size_t o = 0;
    for (unsigned i = 0; i < 24; i++) {
        o += snprintf(sk + o, sizeof(sk) - o, "%08x ", stk[i]);
    }
    hle_log(TAG, "hang vCPU: eip %08x eflags %08x (IF %u) irql %u halted %u "
            "eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x fs %08x",
            eip, (uint32_t)env->eflags, !!(env->eflags & HLE_EFLAGS_IF), irql,
            hle_atomic_read(&cpu->halted),
            (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_EBX], (uint32_t)env->regs[R_ECX],
            (uint32_t)env->regs[R_EDX], (uint32_t)env->regs[R_ESI], (uint32_t)env->regs[R_EDI],
            (uint32_t)env->regs[R_EBP], esp, fs);
    hle_log(TAG, "hang code @%08x: %s", eip - 16, hx);
    hle_log(TAG, "hang stack @%08x: %s", esp, sk);
    /* debug: HAKUX_HLE_HITCH_PEEK=<hex va>:<len>[,...], guest memory at the hang */
    const char *pk = getenv("HAKUX_HLE_HITCH_PEEK");
    while (pk && *pk) {
        char *end;
        uint32_t va = (uint32_t)strtoul(pk, &end, 16), len = 64;
        if (*end == ':') {
            len = MIN((uint32_t)strtoul(end + 1, &end, 0), 512u);
        }
        for (uint32_t off = 0; off < len; off += 64) {
            uint8_t m[64] = { 0 };
            char h[129];
            uint32_t n = MIN(64u, len - off);
            hle_va_rw(va + off, m, n, false);
            hexs(h, sizeof(h), m, n);
            hle_log(TAG, "hang peek @%08x: %s", va + off, h);
        }
        pk = strchr(end, ',');
        pk = pk ? pk + 1 : NULL;
    }
}

static void sample(Eip *e)
{
    HleCpu *cpu = hle_host_cpu();
    if (!cpu) {
        return;
    }
    e->n++;
    if (hle_atomic_read(&cpu->halted)) {
        e->halted++;
        return;
    }
    HleCpu *env = cpu;
    uint32_t eip = (uint32_t)(env->eip);
    if (g_hle_pc_sampler && !g_hle_pc_sampler(&eip)) {
        eip = (uint32_t)(env->eip);
    } else if (g_hle_pc_sampler && eip == HLE_PC_IDLE) {
        e->halted++;
        return;
    }
    if (is_host(eip)) {
        if (eip & HLE_PC_SYSOP) {
            e->host_call++;
        } else {
            e->host_jit++;
        }
    }
    unsigned i = (eip * 2654435761u) >> 26 & (EIP_SLOTS - 1);
    for (unsigned k = 0; k < EIP_SLOTS; k++, i = (i + 1) & (EIP_SLOTS - 1)) {
        if (!e->cnt[i] || e->eip[i] == eip) {
            e->eip[i] = eip;
            e->cnt[i]++;
            return;
        }
    }
}

static void *wd_thread(void *opaque)
{
    bool sampling = false, paused = false;
    while (!__atomic_load_n(&g_h.quit, __ATOMIC_ACQUIRE)) {
        g_usleep(paused ? 100000 : sampling ? TICK_SMP_US : TICK_IDLE_US);  /* paused: a look now and then */
        hle_mutex_lock(&g_h.mx);
        paused = g_h.paused;
        if (!g_h.have_win || g_h.paused) {
            hle_mutex_unlock(&g_h.mx);
            sampling = false;
            continue;
        }
        double el = t2ms(hle_prof_now() - g_h.win.t);
        sampling = el >= 0.8f * stutter_ms();
        if (sampling) {
            sample(&g_h.eip);
        }
        if (el >= g_h.hang_ms) {
            int64_t now_us = g_get_monotonic_time();
            bool first = !g_h.live;
            if (first || now_us - g_h.live_logged_us >= 1000000) {
                Snap b;
                char det[960];
                snap_take(&b);
                explain(&g_h.win, &b, &g_h.eip, el, g_h.live_reason, sizeof(g_h.live_reason),
                        det, sizeof(det));
                g_h.live = true;
                g_h.live_logged_us = now_us;
                const HleHitchThr *rt = &g_hitch_thr[HLE_HT_RENDER];
                static const char *k_act[HLE_ACT_N] = {
                    "run", "idle", "gpu", "ring", "arena", "compile", "submit", "display",
                    "flip", "trap" };
                uint32_t act = __atomic_load_n(&rt->act, __ATOMIC_ACQUIRE);
                if (first) {
                    hang_snapshot();
                }
                hle_log(TAG, "HANG %s %.2f s since frame %llu: %s | render %s for %.0f ms | %s",
                        first ? "started" : "ongoing", el / 1000.0,
                        (unsigned long long)g_h.win.swaps, g_h.live_reason,
                        act < HLE_ACT_N ? k_act[act] : "?",
                        t2ms(hle_prof_now() - __atomic_load_n(&rt->since, __ATOMIC_RELAXED)),
                        det);
            }
        }
        hle_mutex_unlock(&g_h.mx);
    }
    return NULL;
}

/* ---- frame windows ------------------------------------------------------------- */

/* The monitor's start, at the first frame (HAKUX_HLE_HITCH=0: off); true
 * when it just started. */
static bool start(void)
{
    if (g_h.started) {
        return false;
    }
    const char *e = getenv("HAKUX_HLE_HITCH");
    g_h.started = true;
    if (e && atoi(e) == 0) {
        return false;
    }
    init_once();
    g_h.quit = 0;
    hle_thread_create(&g_h.th, "hle.hitch", wd_thread, NULL, HLE_THREAD_JOINABLE);
    __atomic_store_n(&g_hitch_on, true, __ATOMIC_RELEASE);
    hle_log(TAG, "monitor on: frames late by half a vblank or dropped (display), "
            "hang >= %.0f ms", g_h.hang_ms);
    return true;
}

static void window(uint32_t id);

void hle_hitch_pb_swap(uint32_t id)
{
    start();
    window(id);
}

void hle_hitch_pause(void)
{
    if (!g_h.init) {
        return;
    }
    hle_mutex_lock(&g_h.mx);
    g_h.paused = true;                  /* the next window isn't measured */
    g_h.live = false;
    hle_mutex_unlock(&g_h.mx);
}

/* Frame id's Swap (or flip): closes the open frame window, a stutter or a
 * hang when it ran long. */
static void window(uint32_t id)
{
    if (!g_hitch_on) {
        return;
    }
    g_h.swaps++;
    hle_hitch_frame(id, HLE_FR_SWAP);
    Snap now;
    snap_take(&now);
    hle_mutex_lock(&g_h.mx);
    Snap prev = g_h.win;
    Eip eip = g_h.eip;
    bool measured = g_h.have_win && !g_h.paused, live = g_h.live;
    g_h.win = now;
    g_h.have_win = true;
    g_h.paused = false;
    g_h.live = false;
    g_h.win_id++;
    memset(&g_h.eip, 0, sizeof(g_h.eip));
    double ms = t2ms(now.t - prev.t);
    if (measured && ms < 1000.0) {
        g_h.swap_ms = g_h.swap_ms ? 0.9f * g_h.swap_ms + 0.1f * (float)ms : (float)ms;
    }
    bool disp = display_active(now.us);
    if (!measured || ms < (disp ? late_ms() : stutter_ms())) {
        hle_mutex_unlock(&g_h.mx);
        return;
    }
    bool hang = ms >= g_h.hang_ms;
    GuestWin *gw = &g_h.gw_ring[g_h.gw_next++ % GW_RING];
    char *reason = gw->reason, *det = gw->det;
    explain(&prev, &now, &eip, ms, gw->reason, sizeof(gw->reason), gw->det, sizeof(gw->det));
    gw->ms = ms;
    __atomic_store_n(&gw->id, id, __ATOMIC_RELEASE);
    if (!hang && disp) {
        hle_mutex_unlock(&g_h.mx);     /* the display path decides what showed */
        return;
    }
    Last *l = hang ? &g_h.last_hang : &g_h.last_stutter;  /* (lock held) */
    unsigned no = hang ? ++g_h.hangs : ++g_h.stutters;
    l->valid = true;
    l->start_us = prev.us;
    l->ms = ms;
    hle_strlcpy(l->reason, sizeof(l->reason), reason);
    char r2[96], d2[960];
    hle_strlcpy(r2, sizeof(r2), reason);
    hle_strlcpy(d2, sizeof(d2), det);
    hle_mutex_unlock(&g_h.mx);
    hle_log(TAG, "%s #%u frame %llu: %.1f ms (expected %.1f%s): %s | %s",
            hang ? "HANG" : "stutter", no, (unsigned long long)now.swaps, ms, expected_ms(),
            live ? ", reported live" : "", r2, d2);
}

/* ---- display path -------------------------------------------------------------- */

/* The render thread's compile time so far, a compile in progress included
 * (racy: diagnostics only). */
static int64_t render_compile(void)
{
    HleHitchThr *h = &g_hitch_thr[HLE_HT_RENDER];
    int64_t c = h->acc[HLE_ACT_COMPILE], since = __atomic_load_n(&h->since, __ATOMIC_RELAXED);
    if (__atomic_load_n(&h->act, __ATOMIC_ACQUIRE) == HLE_ACT_COMPILE && since) {
        int64_t now = hle_prof_now();
        c += now > since ? now - since : 0;
    }
    return c;
}

static const GuestWin *guest_win(uint32_t id)
{
    for (unsigned i = 0; i < GW_RING; i++) {
        if (__atomic_load_n(&g_h.gw_ring[i].id, __ATOMIC_ACQUIRE) == id) {
            return &g_h.gw_ring[i];
        }
    }
    return NULL;
}

/* UI thread: frame id is on screen. Late (its front more than half a vblank
 * after its interval) or frames skipped since the last one shown: a
 * stutter, put on the stage that made it late. */
static void display_eval(uint32_t id, const FrameT *f)
{
    uint32_t pid = g_h.shown_id;
    int64_t pfront = g_h.shown_front, pshown = g_h.shown_us;
    int64_t comp = render_compile(), pcomp = g_h.shown_comp;
    g_h.shown_comp = comp;
    const uint32_t pmark = g_h.shown_gmark;
    if (g_h.gtrace_left) {
        g_h.shown_gmark = hle_host_gpu_trace_mark();
    }
    g_h.shown_id = id;
    g_h.shown_front = f->t[HLE_FR_FRONT] ? f->t[HLE_FR_FRONT] : f->t[HLE_FR_SHOWN];
    __atomic_store_n(&g_h.shown_us, f->t[HLE_FR_SHOWN], __ATOMIC_RELAXED);
    if (!pid || id <= pid || !pfront || f->t[HLE_FR_SHOWN] - pshown > 2000000) {
        return;                         /* first, out of order, or after a pause */
    }
    float vb = vblank_ms();
    if (g_h.swap_ms && g_h.swap_ms < 0.8f * vb) {
        return;     /* IMMEDIATE (or uncapped): frames outrun the UI by design */
    }
    float exp = expected_ms();
    unsigned skipped = id - pid - 1;
    double fint = (g_h.shown_front - pfront) / 1000.0;
    bool late = fint >= late_ms();
    if (!skipped) {                     /* the cadence: whole vblanks between fronts */
        g_h.cad[g_h.ncad++ % ARRAY_SIZE(g_h.cad)] = (uint8_t)MIN(MAX(fint / vb + 0.5, 1.0), 8.0);
    }
    const double pfint = g_h.last_fint;
    g_h.last_fint = fint;
    if (late && fint < stutter_ms() &&
        (!cad_median() || fint < pfint + 0.5 * vb)) {
        /* there is no presentation interval: the title's pace
         * is what the screen showed. Unknown yet (a 30 fps movie on 60 Hz is
         * not late), or a new pace held from the frame before (60 -> 30 fps
         * is one late frame, not the eight the cadence takes to follow). */
        late = false;
    }
    if (!late && !skipped) {
        return;
    }
    if (skipped) {
        g_h.dropped += skipped;
    }
#define T(st) (f->t[st] ? (f->t[st] - pfront) / 1000.0 : 0.0)
    double ts = T(HLE_FR_SWAP), tw = T(HLE_FR_WRITE), tr = T(HLE_FR_READY);
    double tf = T(HLE_FR_FRONT), tu = T(HLE_FR_SHOWN), tl = T(HLE_FR_FLIP);
    bool hw = f->t[HLE_FR_WRITE] != 0, hr = f->t[HLE_FR_READY] != 0;
    bool hl = f->t[HLE_FR_FLIP] != 0;   /* push-buffer mode: the guest's flip */
#undef T
    double target = exp;                /* its vblank, ms after the previous front */
    const GuestWin *g = guest_win(id);
    char reason[96];
    if (!late) {
        snprintf(reason, sizeof(reason), "%u frame%s dropped: shown %.0f ms after front",
                 skipped, skipped > 1 ? "s" : "", tu - tf);
    } else if (!hr || tr > target + 1.0) {
        /* not ready by its vblank: the guest frame ran long, or the render
         * thread or the GPU took long, or the swap came in just too late */
        if (g) {
            snprintf(reason, sizeof(reason), "guest %.0f ms: %s", g->ms, g->reason);
        } else if (!hw || tw - ts > vb) {
            double cms = pcomp ? t2ms(comp - pcomp) : 0.0;
            if (cms >= 0.5 * (tw - ts)) {
                snprintf(reason, sizeof(reason), "render thread late: pipeline compiles %.0f ms "
                         "(slot written %.0f ms after the swap)", cms, tw - ts);
            } else {
                snprintf(reason, sizeof(reason), "render thread late: slot written %.0f ms "
                         "after the swap", tw - ts);
            }
        } else if (tr - tw > 0.5 * exp) {
            snprintf(reason, sizeof(reason), "GPU late: done %.0f ms after the write",
                     tr - tw);
        } else {
            snprintf(reason, sizeof(reason), "late swap: %.0f ms before its vblank, "
                     "render + GPU %.0f ms", target - ts, tr - ts);
        }
    } else if (hl && tl > target + 1.0) {
        /* push-buffer mode: the guest's vblank handler (or display start)
         * shows a frame, not the presentation interval */
        if (g) {
            snprintf(reason, sizeof(reason), "guest flipped %.0f ms late (guest %.0f ms: %s)",
                     tl - target, g->ms, g->reason);
        } else {
            snprintf(reason, sizeof(reason), "guest flipped %.0f ms after its vblank, ready "
                     "%.0f ms before", tl - target, target - tr);
        }
    } else {
        snprintf(reason, sizeof(reason), "presentation late: ready %.0f ms before its vblank, "
                 "front %.0f ms after", target - tr, tf - target);
    }
    /* how far the frames never shown got: written to a display slot, GPU
     * done, made the front image (then the UI missed them) */
    unsigned nw = 0, nr = 0, nf = 0;
    double fgap = 0, plast = pfront / 1000.0;
    for (uint32_t k = pid + 1; k < id && k - pid <= FR_RING / 2; k++) {
        const FrameT *s = &g_h.fr[k % FR_RING];
        if (__atomic_load_n(&s->id, __ATOMIC_ACQUIRE) != k) {
            continue;
        }
        nw += s->t[HLE_FR_WRITE] != 0;
        nr += s->t[HLE_FR_READY] != 0;
        if (s->t[HLE_FR_FRONT]) {
            nf++;
            fgap = MAX(fgap, s->t[HLE_FR_FRONT] / 1000.0 - plast);
            plast = s->t[HLE_FR_FRONT] / 1000.0;
        }
    }
    char sk[160] = "";
    if (skipped) {
        snprintf(sk, sizeof(sk), " | skipped: %u written %u ready %u front (largest front "
                 "gap %.0f ms), UI pickups %.0f ms apart", nw, nr, nf,
                 MAX(fgap, g_h.shown_front / 1000.0 - plast), (f->t[HLE_FR_SHOWN] - pshown) / 1000.0);
        if (nf && nf == skipped) {
            snprintf(reason, sizeof(reason), "UI missed %u front frame%s (pickups %.0f ms apart)",
                     nf, nf > 1 ? "s" : "", (f->t[HLE_FR_SHOWN] - pshown) / 1000.0);
        } else if (nw < skipped) {
            snprintf(reason, sizeof(reason), "%u frame%s got no display slot (slots full)",
                     skipped - nw, skipped - nw > 1 ? "s" : "");
        } else if (nf < skipped) {
            snprintf(reason, sizeof(reason), "%u written frame%s never made front",
                     skipped - nf, skipped - nf > 1 ? "s" : "");
        }
    }
    hle_mutex_lock(&g_h.mx);
    unsigned no = ++g_h.stutters;
    Last *l = &g_h.last_stutter;
    l->valid = true;
    l->start_us = pfront;
    l->ms = fint;
    hle_strlcpy(l->reason, sizeof(l->reason), reason);
    hle_mutex_unlock(&g_h.mx);
    char fl[24] = "";
    if (hl) {
        snprintf(fl, sizeof(fl), " flip %+.1f", tl);
    }
    hle_log(TAG, "stutter #%u frame %u: front %.1f ms after the last (expected %.1f), "
            "%u dropped: %s | swap %+.1f write %+.1f ready %+.1f%s front %+.1f shown %+.1f ms "
            "(vs the last front)%s%s%s", no, id, fint, exp, skipped, reason, ts, tw, tr, fl, tf,
            tu, sk, g ? " | guest: " : "", g ? g->det : "");
    if (g_h.gtrace_left) {
        /* debug (HAKUX_HLE_HITCH_GTRACE=n): the vblanks, flips and the
         * guest's interrupt handling that led here */
        g_h.gtrace_left--;
        char why[32];
        snprintf(why, sizeof(why), "stutter #%u", no);
        hle_host_gpu_trace_dump(why, pmark);    /* from the last frame's pickup */
    }
}

void hle_hitch_frame(uint32_t id, int stage)
{
    if (!g_hitch_on || !id || stage < 0 || stage >= HLE_FR_N) {
        return;
    }
    FrameT *f = &g_h.fr[id % FR_RING];
    if (stage == HLE_FR_SWAP) {
        memset(f->t, 0, sizeof(f->t));
        __atomic_store_n(&f->id, id, __ATOMIC_RELEASE);
    } else if (__atomic_load_n(&f->id, __ATOMIC_ACQUIRE) != id) {
        return;
    }
    f->t[stage] = g_get_monotonic_time();
    if (stage == HLE_FR_SHOWN) {
        display_eval(id, f);
    }
}

/* ---- overlay ------------------------------------------------------------------- */

static void ago(char *b, size_t n, int64_t us)
{
    int64_t s = us / 1000000;
    if (s < 60) {
        snprintf(b, n, "%llds", (long long)s);
    } else if (s < 3600) {
        snprintf(b, n, "%lldm%02llds", (long long)(s / 60), (long long)(s % 60));
    } else {
        snprintf(b, n, "%lldh%02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
    }
}

bool hle_hitch_overlay(char *buf, size_t n)
{
    if (!g_hitch_on || !n) {
        return false;
    }
    int64_t now = g_get_monotonic_time();
    char a[24];
    size_t o = 0;
    hle_mutex_lock(&g_h.mx);
    o += snprintf(buf + o, n - o, "stutters %u  |  hangs %u  |  dropped %u", g_h.stutters,
                  g_h.hangs, g_h.dropped);
    if (g_h.live && o < n) {
        double el = t2ms(hle_prof_now() - g_h.win.t) / 1000.0;
        o += snprintf(buf + o, n - o, "\nHANG NOW %.1fs: %s", el, g_h.live_reason);
    } else if (g_h.last_hang.valid && o < n) {
        ago(a, sizeof(a), now - g_h.last_hang.start_us);
        o += snprintf(buf + o, n - o, "\nlast hang %.2fs, %s ago: %s", g_h.last_hang.ms / 1000.0,
                      a, g_h.last_hang.reason);
    }
    if (g_h.last_stutter.valid && o < n) {
        ago(a, sizeof(a), now - g_h.last_stutter.start_us);
        o += snprintf(buf + o, n - o, "\nlast stutter %.0fms, %s ago: %s", g_h.last_stutter.ms,
                      a, g_h.last_stutter.reason);
    }
    hle_mutex_unlock(&g_h.mx);
    return true;
}

void hle_hitch_stop(void)
{
    if (__atomic_load_n(&g_hitch_on, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_h.quit, 1, __ATOMIC_RELEASE);
        hle_thread_join(&g_h.th);       /* within its 100 ms tick */
        __atomic_store_n(&g_hitch_on, false, __ATOMIC_RELEASE);
    }
    /* as at a new process, its mutex kept (init_once's) */
    const bool init = g_h.init;
    HleMutex mx = g_h.mx;
    const float env = g_h.stutter_ms_env, hang = g_h.hang_ms;
    const unsigned gtrace = g_h.gtrace_left;
    memset(&g_h, 0, sizeof(g_h));
    g_h.init = init;
    g_h.mx = mx;
    g_h.stutter_ms_env = env;
    g_h.hang_ms = hang;
    g_h.gtrace_left = gtrace;
    memset(g_hitch_thr, 0, sizeof(g_hitch_thr));
}
