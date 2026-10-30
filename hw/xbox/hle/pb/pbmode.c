/*
 * hakuX HLE GPU — push-buffer mode (pbmode.h): the decode thread, the
 * methods that synchronize D3D with the GPU, and the report gate's scout.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/pb/kelvin.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/pb/pbstate.h"
#include "hw/xbox/hle/pb/pusher.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/display.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* The NV2A's channel-0 FIFO registers and its instance memory. */
#define NV_PFIFO_RAMHT               0x002210u  /* base 8:4 (4 KiB units), size 17:16 */
#define NV_PFIFO_CACHE1_PUSH1        0x003204u  /* bits 4:0: the channel */
#define NV_PFIFO_CACHE1_DMA_INSTANCE 0x00322Cu  /* bits 15:0: the push buffer's DMA object >> 4 */
#define NV_PFIFO_CACHE1_DMA_PUT      0x003240u
#define NV_PFIFO_CACHE1_DMA_GET      0x003244u
#define NV_PFIFO_CACHE1_DMA_SUBROUTINE 0x00324Cu /* the return offset | 1 inside a call */
#define NV_PGRAPH_INTR               0x400100u  /* pending; the ISR writes 1s to clear */
#define NV_PGRAPH_NSOURCE            0x400108u
#define NV_PGRAPH_TRAPPED_ADDR       0x400704u  /* method 12:2, subchannel 18:16, channel 24:20 */
#define NV_PGRAPH_TRAPPED_DATA_LOW   0x400708u
#define NV_PGRAPH_SURFACE            0x400710u  /* flip counters: write 22:20, read 26:24, modulo 30:28 */
#define NV_PGRAPH_FIFO               0x400720u  /* bit 0: methods go to PGRAPH */
#define NV_PGRAPH_COLORCLEARVALUE    0x40186Cu
#define NV_PGRAPH_ZSTENCILCLEARVALUE 0x401A88u
#define NV_PGRAPH_PATT_COLOR0        0x400B10u  /* NV044 SET_MONOCHROME_COLOR0 */

/* the 2D classes */
#define NV_CLASS_M2MF       0x39u
#define NV_CLASS_PATTERN    0x44u
#define NV_CLASS_SURFACES2D 0x62u
#define NV_CLASS_BLIT       0x9Fu
#define PGRAPH_INTR_ERROR            (1u << 20)
#define PGRAPH_NSOURCE_NOTIFICATION  1u
#define NV_PRAMIN                    0x700000u
#define NV_PRAMIN_SIZE               0x100000u

#define SURF_WRITE  0x00700000u
#define SURF_READ   0x07000000u
#define SURF_MODULO 0x70000000u

#define NV_NO_OPERATION 0x0100u               /* any class */

/* Kelvin (class 0x97) methods with an effect here. */
#define NV097_SET_FLIP_READ                    0x0120u
#define NV097_SET_FLIP_WRITE                   0x0124u
#define NV097_SET_FLIP_MODULO                  0x0128u
#define NV097_FLIP_INCREMENT_WRITE             0x012Cu
#define NV097_FLIP_STALL                       0x0130u
#define NV097_SET_CONTEXT_DMA_SEMAPHORE        0x01A4u
#define NV097_SET_CONTEXT_DMA_REPORT           0x01A8u
#define NV097_GET_REPORT                       0x17D0u  /* offset 23:0, type 31:24 (1: z-pass pixels) */
#define NV097_SET_ZSTENCIL_CLEAR_VALUE         0x1D8Cu
#define NV097_SET_COLOR_CLEAR_VALUE            0x1D90u
#define NV097_SET_SEMAPHORE_OFFSET             0x1D6Cu
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70u

#define PB_METHODS (0x2000u / 4)

static struct {
    bool on;
    volatile bool stop;
    HleThread th;
    HleSem kick;
    uint32_t kicks;                     /* hle_pb_kick calls */
    HlePusher p;
    bool synced;                        /* the pusher's DMA object and GET are the GPU's */
    uint32_t inst;
    uint32_t last_get;                  /* the GET the decoder last reported */
    uint32_t last_sub;                  /* ... and DMA_SUBROUTINE */
    uint32_t sub_mail;                  /* the guest's last write of it (any thread) */
    bool sub_req;                       /* ... not taken yet */
    uint64_t sub_writes;                /* the guest's own writes of it, taken */
    unsigned faults;
    uint32_t hist[8][PB_METHODS];       /* methods by subchannel (decode thread), sampled */
    uint32_t hist_tick;
    /* the subchannels' objects: instance (PRAMIN offset) and class */
    uint32_t obj_inst[8];
    uint8_t obj_class[8];
    uint32_t sem_inst, sem_offset;      /* Kelvin's semaphore DMA object, offset */
    uint32_t report_inst;               /* its report DMA object (visibility tests) */
    uint64_t reports;
    bool flip_wait;                     /* FLIP_STALL until a flip reads */
    int64_t flip_wait_ns;               /* ... since (a wait over 100 ms dumps the GPU trace) */
    uint32_t flip_mark;                 /* ... the trace's position as it began */
    bool dumped;                        /* the trace dumped (once a run) */
    bool trap_wait;                     /* a software trap until the ISR clears it */
    uint32_t sub_r[8][0x400 / 4];       /* the 2D objects' methods, by subchannel */
    uint32_t s2d[0x400 / 4];            /* the 2D surfaces' (one at a time in D3D) */
    uint64_t blits, blit_bytes, m2mfs, m2mf_bytes, copies_skipped;
    volatile bool in_run;               /* debug: the decode thread is in the pusher */
    uint32_t disp_pa;                   /* hle_pb_display_start's, for the decode thread */
    bool disp_req;
    bool disp_flip;                     /* ... it was D3D's flip (hle_pbd_flip_due) */
    bool overlay_req;                   /* hle_pb_overlay_changed's */
    bool overlay_show;
    bool scan_req;                      /* hle_pb_vblank's */
    uint8_t gamma[3][256];              /* hle_pb_gamma's, for the decode thread */
    bool gamma_req;
    bool gamma_set;                     /* ... once at least (a savestate applies it again) */
    uint64_t traps;
    bool runs_ok;                       /* run records done here (run_record) */
    uint64_t runs;                      /* ... done */
    struct { uint32_t v, n; } trapk[32]; /* traps by code (the log line's top 8) */
    uint32_t trap_other;
    uint64_t sem_releases, flips, binds;
    unsigned misses;
} g_pb;

/* The report gate on (g_sc_gate, HAKUX_HLE_PB_VISGATE), and the report
 * scout reading the guest's pushes (g_sc_on): only once the decoder has met
 * the title's first GET_REPORT. A title that never reports doesn't pay for
 * it (the pusher's second pass, on the guest's thread: ~8% of it in Halo 2,
 * which reports); one that does has its first frame of reports ungated
 * (read as without the gate). */
static bool g_sc_gate, g_sc_on;

bool hle_pb_active(void)
{
    return g_pb.on;
}

static uint64_t g_disp_mode;            /* pitch | xfmt << 32 | hz << 48, 0: none */
static uint32_t g_disp_modes;           /* modes set */

void hle_pb_display_mode(uint32_t pitch, uint32_t xfmt, unsigned hz)
{
    __atomic_store_n(&g_disp_mode, (uint64_t)(pitch & 0xFFFFFFFFu) | (uint64_t)(xfmt & 0xFFFF) << 32 |
                     (uint64_t)(hz & 0xFFFF) << 48, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_disp_modes, 1, __ATOMIC_RELEASE);
}

uint32_t hle_pb_display_mode_get(uint32_t *pitch, uint32_t *xfmt, unsigned *hz)
{
    const uint32_t n = __atomic_load_n(&g_disp_modes, __ATOMIC_ACQUIRE);
    const uint64_t m = __atomic_load_n(&g_disp_mode, __ATOMIC_ACQUIRE);
    *pitch = (uint32_t)m;
    *xfmt = (uint32_t)(m >> 32) & 0xFFFF;
    *hz = (unsigned)(m >> 48);
    return n;
}

void hle_pb_display_start(uint32_t pa)
{
    if (!g_pb.on) {
        return;
    }
    __atomic_store_n(&g_pb.disp_pa, pa, __ATOMIC_RELAXED);
    if (hle_pbd_flip_due()) {
        /* now, before the increment that follows takes the swap: the decode
         * thread may see the start after that */
        __atomic_store_n(&g_pb.disp_flip, true, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_pb.disp_req, true, __ATOMIC_RELEASE);
    hle_pb_kick();
}

void hle_pb_vblank(void)
{
    /* flipping titles present well within the idle time: no kick for them */
    if (g_pb.on && hle_pbd_scan_due()) {
        __atomic_store_n(&g_pb.scan_req, true, __ATOMIC_RELEASE);
        hle_pb_kick();
    }
}

void hle_pb_gamma(const uint8_t rgb[3][256])
{
    if (!g_pb.on) {
        return;
    }
    memcpy(g_pb.gamma, rgb, sizeof(g_pb.gamma));
    g_pb.gamma_set = true;
    __atomic_store_n(&g_pb.gamma_req, true, __ATOMIC_RELEASE);
    hle_pb_kick();
}

void hle_pb_overlay_changed(bool show)
{
    if (!g_pb.on) {
        return;
    }
    __atomic_store_n(&g_pb.overlay_show, show, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pb.overlay_req, true, __ATOMIC_RELEASE);
    hle_pb_kick();
}

bool hle_pb_flip_waiting(void)
{
    return __atomic_load_n(&g_pb.flip_wait, __ATOMIC_ACQUIRE);
}

static const uint32_t *fetch(void *ctx, uint32_t pa, uint32_t n)
{
    return (const uint32_t *)hle_pa_ptr(pa, 4 * n);
}

static uint32_t pramin(uint32_t off)
{
    return off + 4 <= NV_PRAMIN_SIZE ? hle_host_gpu_reg(NV_PRAMIN + off) : 0;
}

/* An object handle in the channel's hash table (RAMHT): its instance's
 * PRAMIN offset, or 0. The hash folds the handle in pieces of the table's
 * index width, then the channel. */
static uint32_t ramht_find(uint32_t handle)
{
    uint32_t ht = hle_host_gpu_reg(NV_PFIFO_RAMHT);
    uint32_t base = (ht & 0x1F0u) << 8;
    uint32_t size = 1u << (((ht >> 16) & 3) + 12);
    unsigned bits = hle_ctz32(size) - 1;            /* entries are 8 bytes */
    uint32_t hash = 0;
    for (uint32_t h = handle; h; h >>= bits) {
        hash ^= h & ((1u << bits) - 1);
    }
    hash ^= (hle_host_gpu_reg(NV_PFIFO_CACHE1_PUSH1) & 0x1Fu) << (bits - 4);
    uint32_t e = base + 8 * (hash & (size / 8 - 1));
    if (pramin(e) != handle || !(pramin(e + 4) & 0x80000000u)) {
        if (g_pb.misses++ < 16) {
            hle_log("hakuX-hle-pb", "object %08x not in RAMHT (entry %05x: %08x %08x)", handle, e,
                    pramin(e), pramin(e + 4));
        }
        return 0;
    }
    return (pramin(e + 4) & 0xFFFFu) << 4;
}

/* A DMA object's physical base (its frame and the adjust in its flags). */
static uint32_t dma_base(uint32_t inst)
{
    return ((pramin(inst + 8) & 0xFFFFF000u) | (pramin(inst) >> 20)) & 0x07FFFFFFu;
}

static void surf_field(uint32_t mask, uint32_t v)
{
    hle_host_gpu_reg_mask(NV_PGRAPH_SURFACE, mask, v << hle_ctz32(mask));
}

static uint32_t surf_get(uint32_t mask)
{
    return (hle_host_gpu_reg(NV_PGRAPH_SURFACE) & mask) >> hle_ctz32(mask);
}

static bool flip_done(void)
{
    return surf_get(SURF_READ) != surf_get(SURF_WRITE);
}

/* The Kelvin methods kelvin() acts on (its cases, by method / 4): every
 * other one is only its two register shadows' (hle_kelvin_method,
 * hle_pbd_method) and takes no look through the switch. */
static const bool k_kelvin_acts[2048] = {
    [NV097_SET_FLIP_READ / 4 ... NV097_FLIP_STALL / 4] = true,
    [NV097_SET_CONTEXT_DMA_SEMAPHORE / 4] = true,
    [NV097_SET_CONTEXT_DMA_REPORT / 4] = true,
    [NV097_GET_REPORT / 4] = true,
    [NV097_SET_SEMAPHORE_OFFSET / 4] = true,
    [NV097_BACK_END_WRITE_SEMAPHORE_RELEASE / 4] = true,
    [NV097_SET_ZSTENCIL_CLEAR_VALUE / 4] = true,
    [NV097_SET_COLOR_CLEAR_VALUE / 4] = true,
};

static int kelvin(uint32_t m, uint32_t v)
{
    if (!k_kelvin_acts[(m >> 2) & 0x7FF]) {
        return HLE_PB_GO;
    }
    if (m >= NV097_SET_FLIP_READ && m <= NV097_FLIP_STALL) {
        static unsigned s_logged;
        if (s_logged++ < 24) {
            hle_log("hakuX-hle-pb", "flip method %04x = %u (read %u write %u modulo %u)", m, v,
                    surf_get(SURF_READ), surf_get(SURF_WRITE), surf_get(SURF_MODULO));
        }
    }
    switch (m) {
    case NV097_SET_FLIP_READ:
        surf_field(SURF_READ, v);
        break;
    case NV097_SET_FLIP_WRITE:
        surf_field(SURF_WRITE, v);
        break;
    case NV097_SET_FLIP_MODULO:
        surf_field(SURF_MODULO, v);
        break;
    case NV097_FLIP_INCREMENT_WRITE: {
        uint32_t mod = surf_get(SURF_MODULO);
        surf_field(SURF_WRITE, mod ? (surf_get(SURF_WRITE) + 1) % mod : 0);
        hle_host_gpu_trace(HLE_GTR_FLIP_INC, surf_get(SURF_WRITE));
        g_pb.flips++;
        hle_back_frame_pace();          /* one frame in flight: the guest, not the
                                           ring's depth, sets the input lag */
        break;
    }
    case NV097_FLIP_STALL:
        if (!flip_done()) {
            hle_host_gpu_trace(HLE_GTR_FLIP_STALL, surf_get(SURF_READ) << 4 | surf_get(SURF_WRITE));
            g_pb.flip_wait = true;
            g_pb.flip_wait_ns = hle_now_ns();
            g_pb.flip_mark = hle_host_gpu_trace_mark();
            return HLE_PB_STALL;
        }
        break;
    case NV097_SET_ZSTENCIL_CLEAR_VALUE:
        hle_host_gpu_reg_set(NV_PGRAPH_ZSTENCILCLEARVALUE, v);
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        hle_host_gpu_reg_set(NV_PGRAPH_COLORCLEARVALUE, v);
        break;
    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
        g_pb.sem_inst = ramht_find(v);
        break;
    case NV097_SET_CONTEXT_DMA_REPORT:
        g_pb.report_inst = ramht_find(v);
        break;
    case NV097_GET_REPORT:
        /* a visibility test's record: timestamp, the pixels that passed,
         * status 0 (done); D3D waits for the status it set to -1. The back
         * end writes it once the GPU ran the test's draws; with nothing to
         * count them yet, no pixels passed. The title's first arms the
         * report scout (g_sc_on). */
        if (g_sc_gate && !__atomic_load_n(&g_sc_on, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_sc_on, true, __ATOMIC_RELEASE);
            hle_log("hakuX-hle-pb", "the title reports: the report scout starts");
        }
        if (g_pb.report_inst) {
            uint32_t pa = dma_base(g_pb.report_inst) + (v & 0x00FFFFFFu);
            uint8_t *p = hle_pa_ptr(pa, 16);
            static unsigned s_logged;
            if (s_logged++ < 8) {
                /* the guest stamped its report pending (-1 at +0xc) before this */
                hle_log("hakuX-hle-pb", "GET_REPORT %08x: object %05x base %08x -> pa %08x [%08x %08x "
                        "%08x %08x]", v, g_pb.report_inst, dma_base(g_pb.report_inst), pa,
                        p ? hle_ldl_le(p) : 0, p ? hle_ldl_le(p + 4) : 0, p ? hle_ldl_le(p + 8) : 0,
                        p ? hle_ldl_le(p + 12) : 0);
            }
            hle_barrier_report_decoded(pa);     /* on its way: the gate may wait for it */
            if (p && hle_pbd_vis_report(pa)) {
                g_pb.reports++;
            } else if (p) {
                uint64_t ts = (uint64_t)hle_now_ns();
                memcpy(p, &ts, 8);
                hle_stl_le(p + 8, hle_pbd_novis() ? 0x100000u : 0);  /* debug: all passed */
                hle_stl_le(p + 12, 0);
                hle_host_mem_set_dirty(pa, 16);
                hle_barrier_report_written(pa);
                g_pb.reports++;
            }
        }
        break;
    case NV097_SET_SEMAPHORE_OFFSET:
        g_pb.sem_offset = v;
        break;
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
        if (g_pb.sem_inst) {
            uint32_t pa = dma_base(g_pb.sem_inst) + g_pb.sem_offset;
            static unsigned s_logged;
            if (s_logged++ < 8) {
                hle_log("hakuX-hle-pb", "semaphore release %08x at pa %08x (object %05x: %08x %08x "
                        "%08x, offset %08x)", v, pa, g_pb.sem_inst, pramin(g_pb.sem_inst),
                        pramin(g_pb.sem_inst + 4), pramin(g_pb.sem_inst + 8), g_pb.sem_offset);
            }
            uint8_t *p = hle_pa_ptr(pa, 4);
            if (p) {
                hle_stl_le(p, v);
                hle_host_mem_set_dirty(pa, 4);
                g_pb.sem_releases++;
            }
        }
        break;
    default:
        break;
    }
    return HLE_PB_GO;
}

/* debug: HAKUX_HLE_PBLOG=<n> / debug.hakux.pblog: the first n methods,
 * and the objects SET_OBJECT and the DMA context methods name. */
/* HAKUX_HLE_PBLOG=n: the first n methods; a new value of debug.hakux.pblog
 * (polled once a decode pass, pblog_poll): the next n */
static int g_pblog = -1;

static void pblog_poll(void)
{
    if (g_pblog < 0) {
        const char *e = getenv("HAKUX_HLE_PBLOG");
        g_pblog = e ? atoi(e) : 0;
    }
#ifdef __ANDROID__
    static HleProp prop;
    static unsigned polls;
    char v[PROP_VALUE_MAX];
    if ((polls++ & 255) == 0 && hle_prop_new(&prop, "debug.hakux.pblog", v)) {
        g_pblog = atoi(v);
    }
#endif
}

static inline unsigned pblog_left(void)
{
    return g_pblog > 0 ? (unsigned)g_pblog-- : 0;
}

static void log_object(const char *what, uint32_t handle)
{
    uint32_t inst = ramht_find(handle);
    hle_log("hakuX-hle-pb", "  %s %08x -> instance %05x: %08x %08x %08x %08x", what, handle, inst,
            inst ? pramin(inst) : 0, inst ? pramin(inst + 4) : 0, inst ? pramin(inst + 8) : 0,
            inst ? pramin(inst + 12) : 0);
}

/* PGRAPH takes methods: D3D's ISR turns its FIFO access off while it
 * reads a trap's registers, which the methods after the trap overwrite. */
static bool fifo_open(void)
{
    return hle_host_gpu_reg(NV_PGRAPH_FIFO) & 1;
}

/* GET as the decoder consumes, and the return offset inside a call: D3D
 * measures the GPU's progress by them (reading the subroutine register when
 * GET is outside the push buffer). A fence slot it judges far enough ahead
 * it patches into a trap and sleeps until the trap's interrupt (4134
 * 0x19dee0: 32 KiB past GET); with GET reported only after a whole run, the
 * slot was already decoded, the trap never came and GunValkyrie's thread
 * slept for good. */
/* D3D writes the subroutine state itself: 4928's RunPushBuffer comes back
 * from a run it CALLed by a JUMP (patched in by its trap handler, or at
 * once when it can link the run), and clears the state then, return offset
 * with bit 0 clear, from the handler or from the CPU while the GPU runs.
 * The write is taken at the decoder's next method (a published GET would
 * overwrite the register first). Lost, every later run's CALL was a call
 * inside a call: the push buffer faulted and Crimson Sea's GPU work
 * stopped. */
void hle_pb_subroutine_written(uint32_t v)
{
    if (!g_pb.on) {
        return;
    }
    __atomic_store_n(&g_pb.sub_mail, v, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pb.sub_req, true, __ATOMIC_RELEASE);
}

static void sub_take(void)
{
    if (!__atomic_exchange_n(&g_pb.sub_req, false, __ATOMIC_ACQ_REL)) {
        return;
    }
    const uint32_t v = __atomic_load_n(&g_pb.sub_mail, __ATOMIC_RELAXED);
    HlePusher *p = &g_pb.p;
    if (g_pb.sub_writes < 4) {
        hle_log("hakuX-hle-pb", "DMA_SUBROUTINE %08x -> %08x (the guest's) at GET %08x",
                p->sub_active ? p->sub_return | 1 : 0, v, p->get);
    }
    p->sub_active = v & 1;
    p->sub_return = v & ~3u;
    g_pb.last_sub = v;
    g_pb.sub_writes++;
}

static void publish_get(void)
{
    const HlePusher *p = &g_pb.p;
    g_pb.last_sub = p->sub_active ? p->sub_return | 1 : 0;
    hle_host_gpu_reg_set(NV_PFIFO_CACHE1_DMA_SUBROUTINE, g_pb.last_sub);
    hle_host_gpu_reg_set(NV_PFIFO_CACHE1_DMA_GET, p->get);
    g_pb.last_get = p->get;
}

/* ---- the 2D classes ---- */

/* The DMA object a context method names (its handle): its base, 0 if none */
static uint32_t ctx_base(uint32_t handle)
{
    uint32_t inst = handle ? ramht_find(handle) : 0;
    return inst ? dma_base(inst) : 0;
}

/* NV09F SIZE: the blit between the 2D surfaces' memory (source and
 * destination offsets in their DMA objects, pitches, format) */
static void blit(const uint32_t *r)
{
    static const uint8_t k_bpp[16] = { 0, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4 };
    const uint32_t *s = g_pb.s2d;
    const uint32_t bpp = k_bpp[s[0x300 >> 2] & 0xF];
    const uint32_t pin = r[0x300 >> 2], pout = r[0x304 >> 2], size = r[0x308 >> 2];
    const uint32_t w = size & 0xFFFF, h = size >> 16, sp = s[0x304 >> 2] & 0xFFFF, dp = s[0x304 >> 2] >> 16;
    const uint32_t sbase = (ctx_base(s[0x184 >> 2]) + s[0x308 >> 2]) & 0x07FFFFFFu;
    const uint32_t dbase = (ctx_base(s[0x188 >> 2]) + s[0x30C >> 2]) & 0x07FFFFFFu;
    const uint32_t ix = pin & 0xFFFF, iy = pin >> 16, ox = pout & 0xFFFF, oy = pout >> 16;
    const uint32_t row = w * bpp;
    if (!bpp || !w || !h || (h > 1 && (!sp || !dp)) || (uint64_t)row * h > (16u << 20)) {
        g_pb.copies_skipped++;
        return;
    }
    hle_pbd_blit_begin(sbase, dbase);       /* the CPU's writes before the blit's */
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t so = sbase + (iy + y) * sp + ix * bpp, d = dbase + (oy + y) * dp + ox * bpp;
        uint8_t *sr = hle_pa_ptr(so, row), *dr = hle_pa_ptr(d, row);
        if (!sr || !dr) {
            g_pb.copies_skipped++;
            return;
        }
        memmove(dr, sr, row);
        hle_mem_mark_written(d, row);
    }
    /* the back end's images: a render target's pixels are in its image */
    hle_pbd_blit(sbase, sp, dbase, dp, bpp, ix, iy, ox, oy, w, h);
    g_pb.blits++;
    g_pb.blit_bytes += (uint64_t)row * h;
}

/* NV039 BUFFER_NOTIFY: LINE_COUNT lines of LINE_LENGTH_IN bytes, OFFSET_IN
 * + PITCH_IN steps to OFFSET_OUT + PITCH_OUT steps */
static void m2mf(const uint32_t *r)
{
    const uint32_t in = (ctx_base(r[0x184 >> 2]) + r[0x30C >> 2]) & 0x07FFFFFFu;
    const uint32_t out = (ctx_base(r[0x188 >> 2]) + r[0x310 >> 2]) & 0x07FFFFFFu;
    const int32_t pin = (int32_t)r[0x314 >> 2], pout = (int32_t)r[0x318 >> 2];
    const uint32_t len = r[0x31C >> 2], lines = r[0x320 >> 2];
    if (!len || !lines || (uint64_t)len * lines > (16u << 20)) {
        g_pb.copies_skipped++;
        return;
    }
    for (uint32_t y = 0; y < lines; y++) {
        uint8_t *s = hle_pa_ptr(in + y * pin, len), *d = hle_pa_ptr(out + y * pout, len);
        if (!s || !d) {
            g_pb.copies_skipped++;
            return;
        }
        memmove(d, s, len);
        hle_mem_mark_written(out + y * pout, len);
    }
    g_pb.m2mfs++;
    g_pb.m2mf_bytes += (uint64_t)len * lines;
}

static void method_2d(unsigned subch, uint32_t m, uint32_t v)
{
    uint32_t *r = g_pb.sub_r[subch];
    r[m >> 2] = v;
    switch (g_pb.obj_class[subch]) {
    case NV_CLASS_PATTERN:
        if (m == 0x310) {
            /* D3D 5558 reads the pattern colour back as its GPU progress
             * (Blinx waits on its low bits) */
            hle_host_gpu_reg_set(NV_PGRAPH_PATT_COLOR0, v);
        }
        break;
    case NV_CLASS_SURFACES2D:
        g_pb.s2d[m >> 2] = v;
        break;
    case NV_CLASS_BLIT:
        if (m == 0x308) {
            blit(r);
        }
        break;
    case NV_CLASS_M2MF:
        if (m == 0x328) {
            m2mf(r);
        }
        break;
    default:
        break;
    }
}

/* The first-write barrier, on the guest's thread: a store waits for
 * `value`, which can be the submission this decoder still has open; nothing
 * else closes it, and the decoder may itself be stalled on a trap that the
 * waiting guest's ISR must clear. The decode thread closes it at its next
 * method or pass (boundary_take). */
static uint64_t g_bnd_req;

static void boundary_request(uint64_t value)
{
    uint64_t cur = __atomic_load_n(&g_bnd_req, __ATOMIC_RELAXED);
    while (cur < value && !__atomic_compare_exchange_n(&g_bnd_req, &cur, value, true,
                                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
    hle_pb_kick();
}

static inline void boundary_take(void)
{
    if (__builtin_expect(__atomic_load_n(&g_bnd_req, __ATOMIC_RELAXED) != 0, 0)) {
        hle_pbd_boundary(__atomic_exchange_n(&g_bnd_req, 0, __ATOMIC_ACQUIRE));
    }
}

/* D3D 4928's RunPushBuffer, when the GPU may still be running the recorded
 * buffer's last use, puts a NOP trap of kind 3 (CALL) or 4 (JUMP) before the
 * branch into it: the argument >> 5 is the PA of the 4-word record after
 * the branch {fixups, buffer, end, return} (Crimson Sea 0x3ad930). D3D's
 * DPC then copies the fixups ({bytes, offset, data} entries until bytes is
 * -1) into the buffer, ends it with a JUMP to the return and, for a CALL,
 * writes DMA_SUBROUTINE (the return: not in a call) (0x3b2d50); its other
 * effects are a device counter nothing reads and a cache flush. The
 * decoder does the same in place: no interrupt, DPC and handler per run
 * (Crimson Sea ~217 a frame). False if the record doesn't look like one
 * (the guest's handler takes the trap). D3D 4928 only (where it was read);
 * HAKUX_HLE_PB_RUNS=0: always the guest's. */
#define RUN_FIXUPS_MAX 4096u

static bool contig_pa(uint32_t va, uint32_t *pa)
{
    if (va < 0x80000000u || (uint64_t)(va - 0x80000000u) >= hle_ram_size()) {
        return false;
    }
    *pa = va - 0x80000000u;
    return true;
}

static bool run_record(uint32_t v)
{
    const uint32_t *w = (const uint32_t *)hle_pa_ptr(v >> 5, 16);
    uint32_t fix = 0, buf, ret;
    if (!w || (w[0] && !contig_pa(w[0], &fix)) || !contig_pa(w[1], &buf) || !contig_pa(w[3], &ret) ||
        (w[2] & 3) || !hle_pa_ptr(buf + w[2], 4)) {
        return false;
    }
    /* the fixups, checked whole before any is applied */
    unsigned n = 0;
    for (uint32_t e = fix; e; n++) {
        const uint32_t *h = (const uint32_t *)hle_pa_ptr(e, 8);
        if (!h || n >= RUN_FIXUPS_MAX) {
            return false;
        }
        if (h[0] == 0xFFFFFFFFu) {
            break;
        }
        if (!hle_pa_ptr(e + 8, h[0]) || !hle_pa_ptr(buf + h[1], h[0])) {
            return false;
        }
        e += 8 + h[0];
    }
    for (uint32_t e = fix; e;) {
        const uint32_t *h = (const uint32_t *)hle_pa_ptr(e, 8);
        if (h[0] == 0xFFFFFFFFu) {
            break;
        }
        memmove(hle_pa_ptr(buf + h[1], h[0]), hle_pa_ptr(e + 8, h[0]), h[0]);
        hle_mem_mark_written(buf + h[1], h[0]);
        e += 8 + h[0];
    }
    const uint32_t jump = (w[3] & 0x0FFFFFFFu) + 1;
    memcpy(hle_pa_ptr(buf + w[2], 4), &jump, 4);
    hle_mem_mark_written(buf + w[2], 4);
    if ((v & 0x1F) == 3) {
        /* the DPC's DMA_SUBROUTINE write, through the guest's way in (the
         * mailbox: the newest write wins), taken now */
        const uint32_t sub = w[3] & 0x0FFFFFFFu;
        hle_host_gpu_reg_set(NV_PFIFO_CACHE1_DMA_SUBROUTINE, sub);
        hle_pb_subroutine_written(sub);
        sub_take();
    }
    (void)ret;
    return true;
}

/* The software traps the decoder is past, for the report scout (scout_take):
 * each trap word's PA, in a ring by their count. Past: the guest's handler
 * done (its interrupt cleared, the FIFO access back), or a run's record done
 * here. */
static struct {
    uint64_t n;                         /* (atomic) */
    uint32_t pa[16];
    uint32_t held;                      /* the trap the decoder waits at */
    bool pending;
} g_trel;

static void trap_past(uint32_t pa)
{
    const uint64_t n = __atomic_load_n(&g_trel.n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_trel.pa[n & 15], pa, __ATOMIC_RELAXED);
    __atomic_store_n(&g_trel.n, n + 1, __ATOMIC_RELEASE);
}

/* Per packet, not per method: PGRAPH's FIFO access (only D3D's trap
 * handler turns it off, with the decoder stalled at the trap; consume
 * checks it to go on) and GET, published at a packet's start and every 64
 * words (D3D patches slots it judges far enough past GET: 4134's are 32 KiB
 * past). Per method the two were a tenth of Crimson Sea's decoder. */
static inline bool packet_start(void)
{
    const HlePusher *p = &g_pb.p;
    return p->left == p->count || p->get - g_pb.last_get >= 256;
}

static int count_method(void *opaque, unsigned subch, uint32_t m, uint32_t v)
{
    boundary_take();
    if (packet_start()) {
        if (!fifo_open()) {
            return HLE_PB_HOLD;
        }
        publish_get();
    }
    subch &= 7;
    if (pblog_left()) {
        hle_log("hakuX-hle-pb", "method %u:%04x = %08x (class %02x)", subch, m, v,
                g_pb.obj_class[subch]);
        if (m == 0 || (m >= 0x0180 && m < 0x0200)) {
            log_object(m ? "context" : "object", v);
        }
    }
    /* debug (the log line's top methods): one method in 16, counted 16;
     * every method's count cost ~3% of the decoder (a 64 KB table) */
    if (!(++g_pb.hist_tick & 15)) {
        g_pb.hist[subch][(m & 0x1FFFu) / 4] += 16;
    }
    if (m == NV_NO_OPERATION && !v) {
        return HLE_PB_GO;                       /* padding (Crimson Sea: 1.3M a second) */
    }
    if (m == NV_NO_OPERATION && g_pb.runs_ok && ((v & 0x1F) == 3 || (v & 0x1F) == 4) && run_record(v)) {
        g_pb.runs++;
        trap_past(g_pb.p.dma_base + g_pb.p.get);
        return HLE_PB_GO;
    }
    if (m == NV_NO_OPERATION && v) {
        /* a software trap: D3D's ISR does the work (state its own way,
         * callbacks) from the trap registers and the clear values, then
         * clears the interrupt, and the decoder goes on */
        static unsigned s_logged;
        if (s_logged++ < 64 || (g_pb.traps & 65535) == 0) {
            hle_log("hakuX-hle-pb", "trap %03x (packet of %u%s, word %u): arg %08x color %08x "
                    "(flip read %u write %u)", v, g_pb.p.count, g_pb.p.noninc ? " non-incrementing" : "",
                    g_pb.p.count - g_pb.p.left, hle_host_gpu_reg(NV_PGRAPH_ZSTENCILCLEARVALUE),
                    hle_host_gpu_reg(NV_PGRAPH_COLORCLEARVALUE), surf_get(SURF_READ),
                    surf_get(SURF_WRITE));
        }
        hle_host_gpu_trace(HLE_GTR_TRAP, v);
        hle_pbd_trap();                         /* before its handler, which may flip */
        hle_host_gpu_reg_set(NV_PGRAPH_TRAPPED_ADDR, subch << 16 | m);
        hle_host_gpu_reg_set(NV_PGRAPH_TRAPPED_DATA_LOW, v);
        hle_host_gpu_reg_set(NV_PGRAPH_NSOURCE, PGRAPH_NSOURCE_NOTIFICATION);
        hle_host_gpu_reg_mask(NV_PGRAPH_INTR, PGRAPH_INTR_ERROR, PGRAPH_INTR_ERROR);
        g_pb.trap_wait = true;
        g_trel.held = g_pb.p.dma_base + g_pb.p.get;
        g_trel.pending = true;
        g_pb.traps++;
        const uint32_t kv = v & 0xFF;           /* the low byte: the trap's kind */
        unsigned k = 0;
        while (k < 32 && g_pb.trapk[k].n && g_pb.trapk[k].v != kv) {
            k++;
        }
        if (k < 32) {
            g_pb.trapk[k].v = kv;
            g_pb.trapk[k].n++;
        } else {
            g_pb.trap_other++;
        }
        hle_host_gpu_irq_update();
        return HLE_PB_STALL;
    }
    if (m == 0) {                                   /* SET_OBJECT */
        uint32_t inst = ramht_find(v);
        g_pb.obj_inst[subch] = inst;
        g_pb.obj_class[subch] = inst ? (uint8_t)pramin(inst) : 0;
        g_pb.binds++;
        static uint32_t s_seen[8];                  /* each class logged once */
        const uint8_t cls = g_pb.obj_class[subch];
        if (!(s_seen[cls >> 5] & (1u << (cls & 31)))) {
            s_seen[cls >> 5] |= 1u << (cls & 31);
            hle_log("hakuX-hle-pb", "subchannel %u: object %08x, class %02x", subch, v, cls);
        }
        return HLE_PB_GO;
    }
    if (g_pb.obj_class[subch] == 0x97) {
        int r = kelvin(m, v);
        hle_kelvin_method(m, v);                /* its register shadow: the draw state */
        hle_pbd_method(m, v);
        return r;
    }
    if (m < 0x400) {
        method_2d(subch, m, v);
    }
    return HLE_PB_GO;
}

/* The methods count_run batches: data no method here acts on. Program and
 * constants (0x0B00-0x0FFC) either way; inline vertices (0x1818) and
 * indices (0x1800, 0x1808) in non-increasing packets. The rest go to
 * count_method without a call (per word, a sixth of the decoder's work). */
static const uint8_t k_count_runs[2048] = {
    [0x0B00 / 4 ... 0x0FFC / 4] = HLE_PB_RUN_INC | HLE_PB_RUN_NONINC,
    [0x1800 / 4] = HLE_PB_RUN_NONINC,
    [0x1808 / 4] = HLE_PB_RUN_NONINC,
    [0x1818 / 4] = HLE_PB_RUN_NONINC,
};

static uint32_t count_run(void *opaque, unsigned subch, uint32_t m, bool noninc,
                          const uint32_t *w, uint32_t n, int *stall)
{
    /* only the methods of k_count_runs */
    boundary_take();
    if (!fifo_open()) {
        return 0;
    }
    publish_get();
    if (g_pb.obj_class[subch & 7] == 0x97) {
        hle_pbd_run(m, noninc, w, n);
    }
    g_pb.hist[subch & 7][(m & 0x1FFFu) / 4] += n;    /* debug: a run counts at its first method */
    return n;
}

static void count_branch(void *opaque, HlePusher *p)
{
    sub_take();
}

static const HlePbSink k_count = { NULL, count_method, count_run, count_branch, k_count_runs };

/* What the GPU's registers say the push buffer is: its DMA object in
 * instance memory (class, limit, frame). False if it isn't set up yet. */
static bool dma_object(uint32_t *base, uint32_t *limit, uint32_t *inst)
{
    *inst = (hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_INSTANCE) & 0xFFFFu) << 4;
    if (!*inst || *inst + 12 > NV_PRAMIN_SIZE) {
        return false;
    }
    uint32_t flags = hle_host_gpu_reg(NV_PRAMIN + *inst);
    uint32_t frame = hle_host_gpu_reg(NV_PRAMIN + *inst + 8);
    *limit = hle_host_gpu_reg(NV_PRAMIN + *inst + 4);
    *base = ((frame & 0xFFFFF000u) | (flags >> 20)) & 0x07FFFFFFu;
    return true;
}

static void consume_pass(void)
{
    pblog_poll();
    boundary_take();
    hle_pbd_front_serve();      /* before the stalls below: the guest's write waits on it */
    hle_back_pace_serve();      /* the flip's pace: the wait and submit here, at a
                                   packet boundary, never mid-packet */
    if (__atomic_exchange_n(&g_pb.disp_req, false, __ATOMIC_ACQ_REL)) {
        hle_pbd_display(__atomic_load_n(&g_pb.disp_pa, __ATOMIC_RELAXED),
                        __atomic_exchange_n(&g_pb.disp_flip, false, __ATOMIC_ACQ_REL));
    }
    if (__atomic_exchange_n(&g_pb.scan_req, false, __ATOMIC_ACQ_REL)) {
        hle_pbd_scanout();
    }
    if (__atomic_exchange_n(&g_pb.overlay_req, false, __ATOMIC_ACQ_REL)) {
        hle_pbd_overlay(__atomic_load_n(&g_pb.overlay_show, __ATOMIC_RELAXED));
    }
    if (__atomic_exchange_n(&g_pb.gamma_req, false, __ATOMIC_ACQ_REL)) {
        hle_pbd_gamma(g_pb.gamma);
    }
    if (g_pb.trap_wait) {
        if (hle_host_gpu_reg(NV_PGRAPH_INTR) & PGRAPH_INTR_ERROR) {
            return;                                 /* the ISR's clear comes back */
        }
        g_pb.trap_wait = false;
        hle_host_gpu_trace(HLE_GTR_RESUME, 1);
    }
    if (!fifo_open()) {
        return;                                     /* the ISR's FIFO access comes back */
    }
    if (g_trel.pending) {
        g_trel.pending = false;
        trap_past(g_trel.held);
    }
    if (g_pb.flip_wait) {
        if (!flip_done()) {
            return;                                 /* the next kick or flip comes back */
        }
        g_pb.flip_wait = false;
        hle_host_gpu_trace(HLE_GTR_RESUME, 2);
    }
    uint32_t base, limit, inst;
    uint32_t put = hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT);
    if (!dma_object(&base, &limit, &inst)) {
        hle_host_gpu_reg_set(NV_PFIFO_CACHE1_DMA_GET, put);    /* nothing to read it with */
        g_pb.last_get = put;
        /* the channel is being set up again (CreateDevice resets GET / PUT
         * and the DMA object): the decoder starts over from the registers
         * once it is back. Synced to its old object, it went on from its own
         * stale GET: Halo 2's second set-up ran the first one's init block
         * forever (it ends in a jump to itself) instead of the jump at 0 to
         * the new ring, and CreateDevice never saw the GPU idle. */
        g_pb.synced = false;
        return;
    }
    HlePusher *p = &g_pb.p;
    uint32_t reg_get = hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_GET);
    if (!g_pb.synced || inst != g_pb.inst || base != p->dma_base || limit != p->dma_limit ||
        reg_get != g_pb.last_get) {
        /* a new push buffer, or D3D set GET itself (a reset) */
        hle_pb_pusher_init(p, base, limit);
        p->get = reg_get;
        g_pb.inst = inst;
        g_pb.synced = true;
    } else {
        sub_take();
    }
    g_pb.in_run = true;
    int e = hle_pb_pusher_run(p, put, fetch, NULL, &k_count);
    g_pb.in_run = false;
    if (e) {
        if (g_pb.faults++ < 16) {
            hle_log("hakuX-hle-pb", "push buffer fault: %s at GET %08x (word %08x), PUT %08x, "
                    "object %05x base %08x limit %08x: skipped to PUT (subroutine %08x, "
                    "register %08x, guest writes %llu)", hle_pb_error_name(e), p->error_get,
                    p->error_word, put, inst, base, limit,
                    p->sub_active ? p->sub_return | 1 : 0,
                    hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_SUBROUTINE),
                    (unsigned long long)g_pb.sub_writes);
        }
        hle_pb_pusher_init(p, base, limit);                     /* the spike goes on */
        p->get = put;
    }
    hle_pbd_idle();
    publish_get();
}

/* ---- guest stores behind the decoder ----
 * The console's GPU runs close behind the pushes; the decoder can be most of
 * a frame behind the guest, whose CPU is faster than the console's. A title
 * that rewrites vertices in place for its next frame without waiting for
 * the GPU (Lock with NOOVERWRITE) then changes them under draws of this
 * frame the decoder hasn't reached. Dead or Alive 2's character select
 * draws its translucent parts twice, the second pass LESSEQUAL over the
 * first, and the next frame's skinning between the two moved whole
 * triangles of the second: the fishnet and the hair flickered. A store into
 * bytes copied for a draw a later one may read again (hle_barrier_arm_copied)
 * waits here, on the guest's thread, until the decoder has read what the
 * guest pushed before it: up to the PUT it wrote, which holds while the
 * guest runs nothing. The decoder waiting on the guest (a trap for its
 * interrupt handler, the FIFO access it turned off, a FLIP_STALL) or for the
 * GPU (arena room), or 50 ms, end the wait. */
static struct {
    uint32_t target;                    /* the PUT the decoder must reach */
    int state;                          /* 1 a store waits, 2 released (atomic) */
    HleSem sem;
    uint64_t waits, caught, timeouts, ns;
    uint64_t gave_up[4];                /* the decoder at a trap, a FLIP_STALL, the FIFO off,
                                         * waiting for arena room */
} g_cu;
static __thread bool t_decoder;

/* the decode thread, after each pass */
static void catch_up_check(void)
{
    if (__atomic_load_n(&g_cu.state, __ATOMIC_ACQUIRE) != 1) {
        return;
    }
    const bool there = g_pb.p.get == g_cu.target;
    if (!there && !g_pb.trap_wait && !g_pb.flip_wait && fifo_open()) {
        return;
    }
    int one = 1;
    if (__atomic_compare_exchange_n(&g_cu.state, &one, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (there) {
            g_cu.caught++;
        } else {
            g_cu.gave_up[g_pb.trap_wait ? 0 : g_pb.flip_wait ? 1 : 2]++;
        }
        hle_sem_post(&g_cu.sem);
    }
}

static void consume(void)
{
    consume_pass();
    catch_up_check();
}

/* the guest's thread, at its store (barrier.c) */
static void catch_up(void)
{
    if (!g_pb.on || t_decoder) {
        return;
    }
    const uint32_t put = hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT);
    if (__atomic_load_n(&g_pb.p.get, __ATOMIC_RELAXED) == put) {
        return;
    }
    const int64_t t0 = hle_now_ns();
    while (hle_sem_trywait(&g_cu.sem)) {
        /* a post that came as an earlier wait timed out */
    }
    g_cu.target = put;
    __atomic_store_n(&g_cu.state, 1, __ATOMIC_RELEASE);
    hle_pb_kick();
    /* in 2 ms slices: a decoder out of arena room waits for the GPU to finish
     * all it was given (uncapped, the Story fight: over 50 ms), and the guest
     * doesn't wait for the GPU */
    bool posted = false;
    for (int ms = 0, arena = 0; ms < 50 && !posted; ms += 2) {
        posted = hle_sem_timedwait(&g_cu.sem, 2);
        const char *ph = hle_pbd_phase();
        arena = ph && *ph == 'a' ? arena + 1 : 0;   /* twice: a wait, not an allocation */
        if (!posted && arena == 2) {
            int one = 1;
            if (__atomic_compare_exchange_n(&g_cu.state, &one, 0, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                g_cu.gave_up[3]++;
            }
            posted = true;
        }
    }
    if (!posted) {
        int one = 1;
        if (__atomic_compare_exchange_n(&g_cu.state, &one, 0, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE) && g_cu.timeouts++ < 16) {
            const char *ph = hle_pbd_phase();
            hle_log("hakuX-hle-pb", "catch-up timed out: decoder GET %08x for PUT %08x (now %08x), "
                    "%s%s%s%s, phase %s", __atomic_load_n(&g_pb.p.get, __ATOMIC_RELAXED), put,
                    hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT), g_pb.in_run ? "in a run" : "between runs",
                    g_pb.trap_wait ? ", trap" : "", g_pb.flip_wait ? ", flip stall" : "",
                    fifo_open() ? "" : ", FIFO off", ph ? ph : "none");
        }
    }
    __atomic_store_n(&g_cu.state, 0, __ATOMIC_RELEASE);
    g_cu.ns += (uint64_t)(hle_now_ns() - t0);
    __atomic_fetch_add(&g_cu.waits, 1, __ATOMIC_RELAXED);
}

/* ---- the report scout (mem/barrier.h hle_barrier_report_pushed) ----
 * The guest's thread, at each PUT it writes and at its access to a report
 * page: a second pusher over what the guest pushed since, which reads only
 * the 3D object's GET_REPORTs (with the decoder's report DMA object) and its
 * FLIP_INCREMENT_WRITEs (the frames), a packet's data at once. Synced at the
 * PUT, the first time and after the push buffer changed, the guest set GET
 * or the stream lost it: what came before that isn't gated. At a software
 * trap it waits until the decoder is past it (scout_take). The small blits
 * D3D patches a recorded buffer's closing JUMP with, before it jumps there
 * (RunPushBuffer), it applies to the words it reads next (scout_blit). Debug:
 * HAKUX_HLE_PB_VISGATE=0, no gate (A/B). */
#define SC_PATCHES 16

static struct {
    HlePusher p;
    bool synced;
    uint32_t base, limit, inst;
    uint32_t frame;
    bool at_trap;                       /* stopped at the trap word at PA trap */
    uint32_t trap;
    uint64_t past;                      /* the decoder's traps past (g_trel) looked through */
    uint32_t s2d[0x400 / 4];            /* the 2D surfaces' methods, as read */
    uint32_t blit_in[8], blit_out[8];   /* the blits' POINT_IN / POINT_OUT, by subchannel */
    struct { uint32_t pa, v; } patch[SC_PATCHES];   /* words the blits read will write */
    unsigned npatch;
} g_sc;

static void scout_report(uint32_t v)
{
    const uint32_t ri = __atomic_load_n(&g_pb.report_inst, __ATOMIC_RELAXED);
    if (ri && v >> 24 == 1) {                       /* z-pass pixel counts */
        hle_barrier_report_pushed(dma_base(ri) + (v & 0x00FFFFFFu), g_sc.frame);
    }
}

/* An NV09F SIZE the scout reads: a blit of one to four 32-bit words in a
 * row, D3D's patch of a recorded buffer's closing JUMP (Riddick runs dozens
 * a frame), is noted as the words it will write. Read before the decoder
 * made it, the JUMP went back to the buffer's last run and the scout round
 * old commands to its budget at most PUTs: a third of Riddick's guest
 * thread on the RP5. */
static void scout_blit(unsigned subch, uint32_t size)
{
    static const uint8_t k_bpp[16] = { 0, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4 };
    const uint32_t *s = g_sc.s2d;
    const uint32_t w = size & 0xFFFF, h = size >> 16;
    if (k_bpp[s[0x300 >> 2] & 0xF] != 4 || h != 1 || !w || w > 4) {
        return;
    }
    const uint32_t pin = g_sc.blit_in[subch], pout = g_sc.blit_out[subch];
    const uint32_t so = ((ctx_base(s[0x184 >> 2]) + s[0x308 >> 2]) & 0x07FFFFFFu) +
                        (pin >> 16) * (s[0x304 >> 2] & 0xFFFF) + (pin & 0xFFFF) * 4;
    const uint32_t d = ((ctx_base(s[0x188 >> 2]) + s[0x30C >> 2]) & 0x07FFFFFFu) +
                       (pout >> 16) * (s[0x304 >> 2] >> 16) + (pout & 0xFFFF) * 4;
    const uint32_t *src = (const uint32_t *)hle_pa_ptr(so, 4 * w);
    for (uint32_t i = 0; src && i < w; i++) {
        unsigned k = g_sc.npatch++ % SC_PATCHES;
        g_sc.patch[k].pa = d + 4 * i;
        g_sc.patch[k].v = src[i];
    }
}

static void scout_2d(uint8_t cls, unsigned subch, uint32_t m, uint32_t v)
{
    if (m >= 0x400) {
        return;
    }
    if (cls == NV_CLASS_SURFACES2D) {
        g_sc.s2d[m >> 2] = v;
    } else if (m == 0x300) {
        g_sc.blit_in[subch] = v;
    } else if (m == 0x304) {
        g_sc.blit_out[subch] = v;
    } else if (m == 0x308) {
        scout_blit(subch, v);
    }
}

/* The push buffer's words as the scout reads them: a method or branch word
 * a blit read before will write, that one (newest first, once: the next
 * run of the buffer comes with its own blit). */
static const uint32_t *scout_fetch(void *ctx, uint32_t pa, uint32_t n)
{
    static uint32_t v;
    for (unsigned i = 0; n == 1 && i < MIN(g_sc.npatch, SC_PATCHES); i++) {
        const unsigned k = (g_sc.npatch - 1 - i) % SC_PATCHES;
        if (g_sc.patch[k].pa == pa) {
            g_sc.patch[k].pa = 0;
            v = g_sc.patch[k].v;
            return &v;
        }
    }
    return fetch(ctx, pa, n);
}

static uint32_t scout_take(void *opaque, unsigned subch, uint32_t m, bool noninc,
                           const uint32_t *w, uint32_t n, int *stall)
{
    (void)opaque;
    /* A software trap (a NOP with an argument) right before a branch: D3D
     * runs a recorded buffer so, and its handler ends the buffer with a
     * JUMP to the run's return. Read before that, the JUMP still went to
     * the last run's return, behind, and the scout went round old commands
     * up to its budget at every PUT (Sudeki: ~90% of the guest's thread):
     * the scout waits there until the decoder is past the trap
     * (scout_trap_past), as the console's GPU reads on after the handler.
     * It reads on past other traps: stopped at Blinx's (~4 a frame), it saw
     * the reports Blinx reads a frame later too late, and its decals
     * flickered again. */
    const uint32_t nop = noninc ? (m == NV_NO_OPERATION ? 0 : n) :
                         m <= NV_NO_OPERATION && NV_NO_OPERATION < m + 4 * n ? (NV_NO_OPERATION - m) / 4 : n;
    for (uint32_t i = nop; i < n && (noninc || i == nop); i++) {
        if (!w[i]) {
            continue;
        }
        const uint32_t pa = g_sc.p.dma_base + g_sc.p.get + 4 * i;
        const uint32_t *next = i + 1 == g_sc.p.left ? (const uint32_t *)hle_pa_ptr(pa + 4, 4) : NULL;
        const bool branch = next && ((*next & 3) || (*next & 0xE0000003u) == 0x20000000u ||
                                     *next == 0x00020000u);
        if (branch) {
            g_sc.at_trap = true;
            g_sc.trap = pa;
            *stall = HLE_PB_STALL;
            return i + 1;
        }
    }
    const uint8_t cls = g_pb.obj_class[subch & 7];
    if (cls == NV_CLASS_SURFACES2D || cls == NV_CLASS_BLIT) {
        for (uint32_t i = 0; i < n; i++) {
            scout_2d(cls, subch & 7, noninc ? m : m + 4 * i, w[i]);
        }
        return n;
    }
    if (cls != 0x97) {
        return n;
    }
    if (noninc) {
        if (m == NV097_GET_REPORT) {
            for (uint32_t i = 0; i < n; i++) {
                scout_report(w[i]);
            }
        } else if (m == NV097_FLIP_INCREMENT_WRITE) {
            g_sc.frame += n;
        }
        return n;
    }
    if (m <= NV097_FLIP_INCREMENT_WRITE && NV097_FLIP_INCREMENT_WRITE < m + 4 * n) {
        g_sc.frame++;
    }
    if (m <= NV097_GET_REPORT && NV097_GET_REPORT < m + 4 * n) {
        scout_report(w[(NV097_GET_REPORT - m) / 4]);
    }
    return n;
}

static int scout_method(void *opaque, unsigned subch, uint32_t m, uint32_t v)
{
    int stall = HLE_PB_GO;
    scout_take(opaque, subch, m, false, &v, 1, &stall);
    return stall;
}

static const HlePbSink k_scout = { NULL, scout_method, scout_take, NULL };

/* Words the scout reads at one PUT at most (16 MiB, more than a frame's
 * push buffer): off the stream, it went round old commands forever in the
 * guest's thread (Halo 2's boot: a channel set-up through CACHE1, then a new
 * push buffer; the scout stayed in the old one, which jumps to its start). */
#define SCOUT_BUDGET (4u << 20)

/* Whether the decoder is past the trap the scout waits at: among its traps
 * past since the scout's last find (more than its ring holds: past it). */
static bool scout_trap_past(void)
{
    const uint64_t n = __atomic_load_n(&g_trel.n, __ATOMIC_ACQUIRE);
    if (n - g_sc.past > 16) {
        g_sc.past = n;
        return true;
    }
    for (uint64_t k = g_sc.past; k < n; k++) {
        if (__atomic_load_n(&g_trel.pa[k & 15], __ATOMIC_RELAXED) == g_sc.trap) {
            g_sc.past = k + 1;
            return true;
        }
    }
    return false;
}

/* The scout up to put (the guest's thread); returns the frame it is in. */
static uint32_t scout_to(uint32_t put)
{
    uint32_t base, limit, inst;
    if (!g_pb.on || !dma_object(&base, &limit, &inst)) {
        g_sc.synced = false;
        return g_sc.frame;
    }
    if (!g_sc.synced || g_sc.p.error || base != g_sc.base || limit != g_sc.limit || inst != g_sc.inst) {
        hle_pb_pusher_init(&g_sc.p, base, limit);
        g_sc.p.budget = SCOUT_BUDGET;
        g_sc.p.get = put;
        g_sc.base = base;
        g_sc.limit = limit;
        g_sc.inst = inst;
        g_sc.synced = true;
        g_sc.at_trap = false;
        g_sc.npatch = 0;
        /* the format, pitches and DMA objects come with the device's set-up,
         * long before: the decoder's (its offsets are behind, the scout's
         * own come with each blit) */
        memcpy(g_sc.s2d, g_pb.s2d, sizeof(g_sc.s2d));
        g_sc.past = __atomic_load_n(&g_trel.n, __ATOMIC_ACQUIRE);
        return g_sc.frame;
    }
    if (g_sc.at_trap && !scout_trap_past()) {
        return g_sc.frame;
    }
    g_sc.at_trap = false;
    if (hle_pb_pusher_run(&g_sc.p, put, scout_fetch, NULL, &k_scout) == HLE_PB_ERR_LOOP) {
        static unsigned logged;                 /* it syncs again at the next PUT */
        if (logged++ < 8) {
            hle_log("hakuX-hle-pb", "report scout: lost the stream at GET %08x (PUT %08x, %s)",
                    g_sc.p.error_get, put, g_sc.p.error_word ? "branches only" : "over its budget");
        }
    }
    return g_sc.frame;
}

/* the barrier's, at an access to a report page: the PUT as written */
static uint32_t scout(void)
{
    return scout_to(hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT));
}

/* the barrier's, while a read of a report waits: the decoder waits on the
 * guest itself (the catch-up's reasons to give up) */
static bool decoder_held(void)
{
    return g_pb.trap_wait || g_pb.flip_wait || !fifo_open();
}

void hle_pb_pushed(uint32_t put)
{
    /* armed by the decoder (GET_REPORT): the scout syncs at this PUT */
    if (__atomic_load_n(&g_sc_on, __ATOMIC_ACQUIRE)) {
        scout_to(put);
    }
}

void hle_pb_get(void)
{
    g_sc.synced = false;
}

/* After a pass: up to ~200 us waiting for the next kick in WFE on the kick
 * count (hle_pb_kick's increment clears the exclusive monitor; interrupts
 * and the timer event stream bound each WFE), before sleeping on the
 * semaphore. A software trap's round trip is three guest writes (FIFO off,
 * INTR cleared, FIFO on), each a kick: asleep, each cost the guest a futex
 * wake and the decoder a wake-up before it went on, and Crimson Sea traps
 * ~5900 times a second. */
static bool kick_wait_brief(uint32_t seen)
{
#if defined(__aarch64__)
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
    for (;;) {
        uint32_t k;
        __asm__ volatile("ldaxr %w0, [%1]" : "=r"(k) : "r"(&g_pb.kicks) : "memory");
        if (k != seen || g_pb.stop) {
            return true;
        }
        __asm__ volatile("wfe" ::: "memory");
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
        if (t - t0 > f / 5000) {
            return false;
        }
    }
#else
    (void)seen;
    return false;
#endif
}

/* Savestates (hle_pb_park): the decoder stops between passes, its state
 * whole: everything it took is in the GPU's hands, the GPU done with it and
 * the colour targets it drew back in guest memory (hle_pbd_park). */
static struct {
    int req;                            /* 0 none, 1 park, 2 park at a flip stall */
    bool parked;
} g_park;

static bool park_due(void)
{
    const int req = __atomic_load_n(&g_park.req, __ATOMIC_ACQUIRE);
    return req == 1 || (req == 2 && g_pb.flip_wait);
}

static void park(void)
{
    hle_pbd_park();
    hle_log("hakuX-hle-pb", "parked for a savestate at GET %08x%s%s", g_pb.p.get,
            g_pb.flip_wait ? " (flip stall)" : "", g_pb.trap_wait ? " (trap)" : "");
    __atomic_store_n(&g_park.parked, true, __ATOMIC_RELEASE);
    while (__atomic_load_n(&g_park.req, __ATOMIC_ACQUIRE) && !g_pb.stop) {
        g_usleep(1000);
    }
    __atomic_store_n(&g_park.parked, false, __ATOMIC_RELEASE);
}

bool hle_pb_park(bool at_flip, uint64_t timeout_ns)
{
    if (!g_pb.on) {
        return true;
    }
    __atomic_store_n(&g_park.req, at_flip ? 2 : 1, __ATOMIC_RELEASE);
    const int64_t t0 = hle_now_ns();
    while (!__atomic_load_n(&g_park.parked, __ATOMIC_ACQUIRE)) {
        if ((uint64_t)(hle_now_ns() - t0) > timeout_ns) {
            return false;
        }
        hle_pb_kick();
        g_usleep(1000);
    }
    return true;
}

void hle_pb_unpark(void)
{
    __atomic_store_n(&g_park.req, 0, __ATOMIC_RELEASE);
    hle_pb_kick();
}

void hle_pb_state_get(HlePbState *s)
{
    memset(s, 0, sizeof(*s));
    const HlePusher *p = &g_pb.p;
    s->dma_base = p->dma_base;
    s->dma_limit = p->dma_limit;
    s->get = p->get;
    s->sub_active = p->sub_active;
    s->sub_return = p->sub_return;
    s->method = p->method;
    s->left = p->left;
    s->count = p->count;
    s->subch = p->subch;
    s->noninc = p->noninc;
    s->error = p->error;
    s->error_get = p->error_get;
    s->error_word = p->error_word;
    s->synced = g_pb.synced;
    s->inst = g_pb.inst;
    s->last_get = g_pb.last_get;
    s->last_sub = g_pb.last_sub;
    s->sub_mail = g_pb.sub_mail;
    s->sub_req = g_pb.sub_req;
    s->sub_writes = g_pb.sub_writes;
    memcpy(s->obj_inst, g_pb.obj_inst, sizeof(s->obj_inst));
    memcpy(s->obj_class, g_pb.obj_class, sizeof(s->obj_class));
    s->sem_inst = g_pb.sem_inst;
    s->sem_offset = g_pb.sem_offset;
    s->report_inst = g_pb.report_inst;
    s->flip_wait = g_pb.flip_wait;
    s->trap_wait = g_pb.trap_wait;
    memcpy(s->sub_r, g_pb.sub_r, sizeof(s->sub_r));
    memcpy(s->s2d, g_pb.s2d, sizeof(s->s2d));
    s->disp_pa = g_pb.disp_pa;
    s->disp_req = g_pb.disp_req;
    s->disp_flip = g_pb.disp_flip;
    s->overlay_req = g_pb.overlay_req;
    s->overlay_show = g_pb.overlay_show;
    memcpy(s->gamma, g_pb.gamma, sizeof(s->gamma));
    s->gamma_req = g_pb.gamma_req;
    s->gamma_set = g_pb.gamma_set;
}

/* A load, before the guest runs (the decode thread asleep, no kick yet). */
void hle_pb_state_set(const HlePbState *s)
{
    HlePusher *p = &g_pb.p;
    hle_pb_pusher_init(p, s->dma_base, s->dma_limit);
    p->get = s->get;
    p->sub_active = s->sub_active;
    p->sub_return = s->sub_return;
    p->method = s->method;
    p->left = s->left;
    p->count = s->count;
    p->subch = s->subch;
    p->noninc = s->noninc;
    p->error = s->error;
    p->error_get = s->error_get;
    p->error_word = s->error_word;
    g_pb.synced = s->synced;
    g_pb.inst = s->inst;
    g_pb.last_get = s->last_get;
    g_pb.last_sub = s->last_sub;
    g_pb.sub_mail = s->sub_mail;
    g_pb.sub_req = s->sub_req;
    g_pb.sub_writes = s->sub_writes;
    memcpy(g_pb.obj_inst, s->obj_inst, sizeof(s->obj_inst));
    memcpy(g_pb.obj_class, s->obj_class, sizeof(s->obj_class));
    g_pb.sem_inst = s->sem_inst;
    g_pb.sem_offset = s->sem_offset;
    g_pb.report_inst = s->report_inst;
    g_pb.flip_wait = s->flip_wait;
    g_pb.flip_wait_ns = hle_now_ns();
    g_pb.trap_wait = s->trap_wait;
    g_trel.pending = false;                 /* the scout syncs again at the next PUT */
    g_sc.synced = false;
    memcpy(g_pb.sub_r, s->sub_r, sizeof(s->sub_r));
    memcpy(g_pb.s2d, s->s2d, sizeof(s->s2d));
    g_pb.disp_pa = s->disp_pa;
    g_pb.disp_req = s->disp_req;
    g_pb.disp_flip = s->disp_flip;
    g_pb.overlay_req = s->overlay_req || s->overlay_show;  /* shown again */
    g_pb.overlay_show = s->overlay_show;
    memcpy(g_pb.gamma, s->gamma, sizeof(s->gamma));
    g_pb.gamma_set = s->gamma_set;
    g_pb.gamma_req = s->gamma_req || s->gamma_set;      /* the ramp applied again */
}

static void *decode_thread(void *opaque)
{
    hle_hitch_thread(HLE_HT_DECODE);    /* the stutter / hang monitor's (hitch.h) */
    t_decoder = true;
    while (!g_pb.stop) {
        /* asleep until a kick: waiting for the guest, the flip it stalls on
         * (FLIP_STALL) or its interrupt handler (a trap); once a sleep, not
         * a method */
        const uint32_t pa = hle_hitch_enter(g_pb.flip_wait ? HLE_ACT_FLIP :
                                            g_pb.trap_wait ? HLE_ACT_TRAP : HLE_ACT_IDLE);
        hle_sem_wait(&g_pb.kick);
        hle_hitch_leave(pa);
        while (!g_pb.stop) {
            /* a pass reads the registers fresh: it covers every kick before it */
            while (hle_sem_trywait(&g_pb.kick)) {
            }
            const uint32_t seen = __atomic_load_n(&g_pb.kicks, __ATOMIC_ACQUIRE);
            consume();
            if (park_due()) {
                park();
            }
            if (!kick_wait_brief(seen)) {
                break;
            }
        }
    }
    return NULL;
}

void hle_pb_debug_pos(uint32_t *get, uint32_t *put)
{
    *get = g_pb.p.get;
    *put = hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT);
}

void hle_pb_kick(void)
{
    if (g_pb.on) {
        hle_atomic_inc(&g_pb.kicks);
        hle_sem_post(&g_pb.kick);
    }
}

void hle_pb_put(void)
{
    if (t_hitch < 0 && g_hitch_on) {
        hle_hitch_thread(HLE_HT_VCPU);  /* the vCPU's CPU time for the monitor */
    }
    hle_pb_kick();
}

void hle_pb_start(void)
{
    if (g_pb.on) {
        return;
    }
    memset(&g_pb, 0, sizeof(g_pb));
    hle_sem_init(&g_pb.kick, 0);
    g_pb.on = true;
    {
        const HleXbeLib *lib = hle_xbe_d3d_lib(&g_hle.title.xbe);
        const char *e = getenv("HAKUX_HLE_PB_RUNS");
        g_pb.runs_ok = lib && lib->build == 4928 && !(e && !strcmp(e, "0"));
    }
    hle_pbd_start();
    hle_host_gpu_reg_set(NV_PGRAPH_FIFO, 1);        /* methods go to PGRAPH until D3D says not */
    memset(&g_cu, 0, sizeof(g_cu));
    hle_sem_init(&g_cu.sem, 0);
    hle_thread_create(&g_pb.th, "hle-pb", decode_thread, NULL, HLE_THREAD_JOINABLE);
    __atomic_store_n(&g_bnd_req, 0, __ATOMIC_RELAXED);
    hle_barrier_set_flush(boundary_request);
    {   /* debug: HAKUX_HLE_PB_CATCHUP=0, no catch-up (A/B) */
        const char *e = getenv("HAKUX_HLE_PB_CATCHUP");
        hle_barrier_set_catch_up(e && !strcmp(e, "0") ? NULL : catch_up);
    }
    {   /* debug: HAKUX_HLE_PB_VISGATE=0, no report gate (A/B) */
        const char *e = getenv("HAKUX_HLE_PB_VISGATE");
        memset(&g_sc, 0, sizeof(g_sc));
        g_sc_gate = !(e && !strcmp(e, "0"));
        g_sc_on = false;                /* until the first GET_REPORT */
        hle_barrier_set_report_scout(g_sc_gate ? scout : NULL, decoder_held);
    }
    hle_host_pb_mode(true);
    hle_log("hakuX-hle-pb", "push-buffer mode: the title's D3D runs; its push buffer is decoded");
}

void hle_pb_stop(void)
{
    if (!g_pb.on) {
        return;
    }
    hle_host_pb_mode(false);
    hle_barrier_set_flush(NULL);
    hle_barrier_set_catch_up(NULL);
    g_sc_gate = false;
    __atomic_store_n(&g_sc_on, false, __ATOMIC_RELEASE);
    hle_barrier_set_report_scout(NULL, NULL);
    g_pb.stop = true;
    hle_sem_post(&g_pb.kick);
    hle_thread_join(&g_pb.th);
    hle_sem_destroy(&g_pb.kick);
    hle_sem_destroy(&g_cu.sem);
    hle_pbd_stop();
    g_pb.on = false;
}

void hle_pb_diag(void)
{
    if (!g_pb.on) {
        return;
    }
    if (g_pb.flip_wait && !g_pb.dumped && hle_now_ns() - g_pb.flip_wait_ns > 100000000) {
        g_pb.dumped = true;                 /* once a run */
        hle_host_gpu_trace_dump("flip stall over 100 ms", g_pb.flip_mark);
    }
#ifdef __ANDROID__
    {   /* debug: a new value of debug.hakux.pbtrace dumps the last GPU events */
        static HleProp prop;
        char v[PROP_VALUE_MAX];
        if (hle_prop_new(&prop, "debug.hakux.pbtrace", v)) {
            uint32_t now = hle_host_gpu_trace_mark();
            hle_host_gpu_trace_dump("on demand", now > 2000 ? now - 2000 + 150 : 0);
        }
    }
#endif
    static uint32_t prev[8][PB_METHODS];
    static uint64_t prev_words, prev_heads;
    static uint32_t prev_kicks;
    struct { uint32_t n; uint16_t sm; } top[12] = { { 0 } };
    uint64_t total = 0;
    for (unsigned s = 0; s < 8; s++) {
        for (unsigned m = 0; m < PB_METHODS; m++) {
            uint32_t now = g_pb.hist[s][m], d = now - prev[s][m];
            prev[s][m] = now;
            total += d;
            if (d <= top[11].n) {
                continue;
            }
            int k = 11;
            while (k > 0 && top[k - 1].n < d) {
                top[k] = top[k - 1];
                k--;
            }
            top[k].n = d;
            top[k].sm = (uint16_t)(s << 13 | m * 4);
        }
    }
    char buf[640];
    int o = 0;
    for (int k = 0; k < 12 && top[k].n; k++) {
        o += snprintf(buf + o, sizeof(buf) - o, " %u:%04x=%u", top[k].sm >> 13, top[k].sm & 0x1FFF,
                      top[k].n);
    }
    buf[MIN(o, (int)sizeof(buf) - 1)] = 0;
    uint32_t kicks = hle_atomic_read(&g_pb.kicks);
    uint64_t clears, swaps, draws, ff, skipped, tex_defs, rt_uploads;
    unsigned objs;
    hle_pbd_counts(&clears, &swaps, &objs, &rt_uploads);
    hle_pbd_draw_counts(&draws, &ff, &skipped, &tex_defs);
    uint64_t vis_tests, vis_kicks;
    hle_pbd_vis_counts(&vis_tests, &vis_kicks);
    const HleDispStats ds = hle_disp_stats();
    uint64_t vb_place, vb_copied, bnd_kicks, unindexed;
    hle_pbd_vb_counts(&vb_place, &vb_copied, &bnd_kicks, &unindexed);
    char wp[64] = "";
    {
        uint32_t pg[4];
        const unsigned np = hle_barrier_wait_pages(pg, 4);
        for (unsigned i = 0, o = 0; i < np && o < sizeof(wp) - 12; i++) {
            o += (unsigned)snprintf(wp + o, sizeof(wp) - o, " %05x000", pg[i]);
        }
    }
    uint64_t words = g_pb.p.words, heads = g_pb.p.headers;
    char why[160];
    hle_pbd_skip_reasons(why, sizeof(why));
    char tk[200] = "";
    {
        bool used[32] = { false };
        unsigned o = 0;
        for (unsigned r = 0; r < 8 && o < sizeof(tk) - 24; r++) {
            int best = -1;
            for (unsigned k = 0; k < 32; k++) {
                if (g_pb.trapk[k].n && !used[k] && (best < 0 || g_pb.trapk[k].n > g_pb.trapk[best].n)) {
                    best = (int)k;
                }
            }
            if (best < 0) {
                break;
            }
            used[best] = true;
            o += (unsigned)snprintf(tk + o, sizeof(tk) - o, " %x=%u", g_pb.trapk[best].v,
                                    g_pb.trapk[best].n);
        }
        if (g_pb.trap_other && o < sizeof(tk) - 24) {
            snprintf(tk + o, sizeof(tk) - o, " other=%u", g_pb.trap_other);
        }
    }
    /* where the decoder is: waiting for the ISR (trap), a flip, the FIFO
     * access, arena or ring space in a draw, or decoding */
    const char *ph = hle_pbd_phase(), *state = g_pb.trap_wait ? " (trap)" : g_pb.flip_wait ? " (flip stall)"
                                              : !fifo_open()      ? " (fifo closed)"
                                              : ph && *ph == 'a'  ? " (arena wait)"
                                              : ph                ? " (ring wait)"
                                              : g_pb.in_run       ? " (decoding)" : "";
    hle_log("hakuX-hle-pb", "kicks %u words %llu headers %llu methods %llu faults %u GET %08x "
            "PUT %08x binds %llu semaphores %llu flips %llu traps %llu (%s) runs %llu surfaces %u clears %llu "
            "swaps %llu draws %llu ff %llu skipped %llu (%s) textures %llu rt uploads %llu blits %llu "
            "m2mf %llu copies "
            "skipped %llu vis %llu (kicks %llu) overlays %llu repeats %llu const reuses %llu "
            "vb in place %llu copied %llu (barrier kicks %llu) unindexed %llu first writes %llu (armed %llu, waits %llu, "
            "%.0f ms, pages%s) cache cleans %llu pages writebacks %llu "
            "display written %llu presented %llu dropped %llu waits %llu%s | top%s", kicks - prev_kicks,
            (unsigned long long)(words - prev_words), (unsigned long long)(heads - prev_heads),
            (unsigned long long)total, g_pb.faults, g_pb.last_get,
            hle_host_gpu_reg(NV_PFIFO_CACHE1_DMA_PUT), (unsigned long long)g_pb.binds,
            (unsigned long long)g_pb.sem_releases, (unsigned long long)g_pb.flips,
            (unsigned long long)g_pb.traps, tk + (tk[0] == ' '), (unsigned long long)g_pb.runs, objs,
            (unsigned long long)clears,
            (unsigned long long)swaps, (unsigned long long)draws, (unsigned long long)ff,
            (unsigned long long)skipped, why + (why[0] == ' '), (unsigned long long)tex_defs,
            (unsigned long long)rt_uploads,
            (unsigned long long)g_pb.blits,
            (unsigned long long)g_pb.m2mfs, (unsigned long long)g_pb.copies_skipped,
            (unsigned long long)vis_tests, (unsigned long long)vis_kicks,
            (unsigned long long)hle_pbd_overlays(),
            (unsigned long long)hle_pbd_display_repeats(),
            (unsigned long long)hle_pbd_const_reuses(), (unsigned long long)vb_place,
            (unsigned long long)vb_copied, (unsigned long long)bnd_kicks, (unsigned long long)unindexed,
            (unsigned long long)hle_barrier_stats().traps, (unsigned long long)hle_barrier_stats().armed,
            (unsigned long long)hle_barrier_stats().waits, hle_barrier_stats().wait_ns / 1e6, wp,
            (unsigned long long)hle_barrier_stats().cleaned,
            (unsigned long long)hle_pbd_writebacks(), (unsigned long long)ds.written,
            (unsigned long long)ds.presented, (unsigned long long)ds.dropped,
            (unsigned long long)ds.write_waits, state, buf);
    prev_kicks = kicks;
    prev_words = words;
    prev_heads = heads;
    static uint64_t prev_cu;
    const uint64_t cu = __atomic_load_n(&g_cu.waits, __ATOMIC_RELAXED);
    if (cu != prev_cu) {
        prev_cu = cu;
        hle_log("hakuX-hle-pb", "catch-up: %llu stores waited for the decoder (caught up %llu; it "
                "waited on the guest at a trap %llu, a flip stall %llu, the FIFO off %llu; for arena "
                "room %llu; timed out %llu), %.1f ms", (unsigned long long)cu,
                (unsigned long long)g_cu.caught, (unsigned long long)g_cu.gave_up[0],
                (unsigned long long)g_cu.gave_up[1], (unsigned long long)g_cu.gave_up[2],
                (unsigned long long)g_cu.gave_up[3], (unsigned long long)g_cu.timeouts,
                (double)g_cu.ns / 1e6);
    }
    static uint64_t prev_rw;
    uint64_t rw, rns, rto, rheld;
    hle_barrier_report_stats(&rw, &rns, &rto, &rheld);
    if (rw != prev_rw) {
        hle_log("hakuX-hle-pb", "report gate: %llu reads waited for their visibility tests (+%llu), "
                "%.1f ms in all, %llu given up, %llu at once (the decoder held short of them)",
                (unsigned long long)rw, (unsigned long long)(rw - prev_rw), (double)rns / 1e6,
                (unsigned long long)rto, (unsigned long long)rheld);
        prev_rw = rw;
    }
    hle_back_draw_diag();               /* the back end's state recorded per draw (hakuX-hle-emit) */
}
