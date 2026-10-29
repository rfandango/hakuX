/*
 * hakuX native core — NV2A register space (0xFD000000, 16 MiB), first cut.
 *
 * A register file (reads return what was last written) plus the behavior the
 * XDK's D3D needs, added as the frontier shows it:
 *   PMC_BOOT_0      0x000000  chip identification
 *   PMC_INTR_0      0x000100  pending summary (bit 24 PCRTC), read-only
 *   PMC_INTR_EN_0   0x000140  bit 0: interrupts to the CPU
 *   PTIMER          0x009400/0x009410  free-running time (ns), low/high
 *   PFB_CSTATUS     0x10020C  installed memory size
 *   PCRTC_INTR_0    0x600100  bit 0 vblank, write 1 to clear
 *   PCRTC_INTR_EN_0 0x600140
 *   PCRTC_RASTER    0x600808  current scanline
 *   PFIFO_RUNOUT_STATUS 0x002400, CACHE1_STATUS 0x003214  always empty (low
 *                   mark), CACHE1_DMA_PUSH 0x003220 never busy: the HLE GPU
 *                   takes the push buffer, so the FIFO is idle whenever D3D
 *                   waits for it (Forza's wait-for-idle spins otherwise).
 *                   In push-buffer mode they follow the HLE's GET.
 *   NV_USER         0x800000  channel 0's DMA_PUT/GET/REF (+0x40/44/48), the
 *                   FIFO's CACHE1 DMA_PUT/GET/REF; a DMA_PUT write in
 *                   push-buffer mode kicks the HLE's decoder (hle_pb_kick)
 * Vblank comes from the host clock at 60 Hz. The interrupt goes out on bus
 * level 3. Written from the register map, without the LLE device.
 */
#include "native/dev/dev.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/hle/prop.h"
#include <time.h>

#define NV2A_BASE 0xFD000000u
#define NV2A_SIZE 0x01000000u
#define NV2A_IRQ  3

#define NV_PMC_BOOT_0       0x000000u
#define NV_PMC_INTR_0       0x000100u
#define NV_PMC_INTR_EN_0    0x000140u
#define NV_PFIFO_RUNOUT_STATUS 0x002400u
#define NV_PFIFO_CACHE1_STATUS 0x003214u
#define NV_PFIFO_CACHE1_DMA_PUSH 0x003220u
#define NV_PFIFO_CACHE1_DMA_PUT  0x003240u
#define NV_PFIFO_CACHE1_DMA_GET  0x003244u
#define NV_PFIFO_CACHE1_REF      0x003248u
#define NV_PFIFO_CACHE1_DMA_SUBROUTINE 0x00324Cu
#define NV_PGRAPH_INTR      0x400100u
#define NV_PGRAPH_INTR_EN   0x400140u
#define NV_PGRAPH_SURFACE   0x400710u
#define NV_PGRAPH_INCREMENT 0x40071Cu
#define NV_PGRAPH_FIFO      0x400720u
#define NV_USER_DMA_PUT     0x800040u
#define NV_USER_DMA_GET     0x800044u
#define NV_USER_REF         0x800048u
#define NV_PVIDEO_BUFFER    0x008700u   /* bit 0: buffer 0 in use (the overlay shows it) */
#define NV_PVIDEO_STOP      0x008704u
#define NV_PTIMER_TIME_0    0x009400u
#define NV_PTIMER_TIME_1    0x009410u
#define NV_PFB_CSTATUS      0x10020Cu
#define NV_PFB_WBC          0x100410u
#define NV_PCRTC_INTR_0     0x600100u
#define NV_PCRTC_INTR_EN_0  0x600140u
#define NV_PCRTC_START      0x600800u
#define NV_PCRTC_RASTER     0x600808u

#define PMC_INTR_PCRTC   (1u << 24)
#define PMC_INTR_PGRAPH  (1u << 12)
#define PCRTC_VBLANK     1u
#define PFIFO_LOW_MARK   (1u << 4) /* RUNOUT_STATUS / CACHE1_STATUS: empty */
#define PFIFO_DMA_BUSY   (1u << 4) /* CACHE1_DMA_PUSH state */

#define FRAME_NS   16683333ull /* 59.94 Hz, whole ns: 60000/1001 Hz is 16683333 1/3 */
#define LINES      525u

static uint8_t *g_regs;
static uint64_t g_vblanks;
/* The vblank's period: the video mode's exact rate, 60000/1001 Hz as the
 * Xbox's 525-line output or 50 Hz, as g_frame_ns whole nanoseconds and
 * g_frame_rem / g_frame_den of one, which the vblank thread carries
 * (frame_after) so that the free-running vblank keeps the exact rate. */
static uint64_t g_frame_ns = FRAME_NS;
static uint64_t g_frame_rem = 20000, g_frame_den = 60000;
static uint64_t g_last_vblank_ns;     /* when the last vblank came (the raster counts from it) */

/* The panel's vsync (the app's Choreographer, nc_dev_nv2a_display_vsync):
 * when the display's period is a whole multiple of a frame's (60 Hz, 120 Hz),
 * the vblank runs at it. Free running at 59.94 Hz beside a 60 Hz panel, two
 * frames now and then fell in one refresh and one was never shown (JSRF at
 * 60 fps: one a second). Its phase: half a period from where the presenter
 * picks frames up (nc_dev_nv2a_display_pickup), the most either may jitter
 * before a frame is shown twice and the next never. The presenter's loop is
 * paced by its window's buffer queue, not the vsync: on the RP5 it picked up
 * 1-3 ms before vblanks set VSYNC_LEAD before the vsync, and one ~3 ms late
 * pickup skipped a frame (NGB at 60 fps: one every ~20 s). VSYNC_LEAD is the
 * phase until pickups are seen. */
static uint64_t g_disp_vsync_ns, g_disp_period_ns;
static int64_t g_pick_off_ns = -1;    /* pickups after a vsync (smoothed, 0..period) */
#define VSYNC_LEAD   3000000ll
#define VSYNC_STALE  200000000ull
/* Push-buffer mode (hle_host_pb_mode): the HLE consumes channel 0's FIFO. */
static bool g_pbmode;

/* debug: NC_NV2A_CENSUS=1 / debug.hakux.nvcensus=1 at boot: the registers the
 * guest reads and writes, by offset: each new one logged as it first comes
 * (the first 512), the busiest once a second (hakuX-native-nv2a). */
#define CENSUS_SLOTS 4096u
static struct {
    bool on;
    struct {
        uint32_t off;               /* offset + 1; 0: free */
        uint32_t rd, wr, last;
    } r[CENSUS_SLOTS];
    unsigned nnew;
    uint64_t t_log;
} g_census;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t *reg(uint32_t off)
{
    return (uint32_t *)(g_regs + off);
}

static uint32_t ld(uint32_t off)
{
    return __atomic_load_n(reg(off), __ATOMIC_ACQUIRE);
}

static void census(uint32_t off, bool write, uint32_t v)
{
    uint32_t key = off & ~3u;
    unsigned i = (key * 2654435761u) >> 20 & (CENSUS_SLOTS - 1);
    for (unsigned k = 0; k < 16; k++, i = (i + 1) & (CENSUS_SLOTS - 1)) {
        if (g_census.r[i].off == key + 1) {
            break;
        }
        if (!g_census.r[i].off) {
            g_census.r[i].off = key + 1;
            if (g_census.nnew++ < 512) {
                nc_log("hakuX-native-nv2a", "new register %06x: first %s %08x", key,
                       write ? "write" : "read", v);
            }
            break;
        }
    }
    if (write) {
        g_census.r[i].wr++;
    } else {
        g_census.r[i].rd++;
    }
    g_census.r[i].last = v;
}

/* Once a second from the vblank thread: the busiest registers since. */
static void census_log(void)
{
    struct { uint32_t n, off, rd, wr, last; } top[16] = { { 0 } };
    for (unsigned i = 0; i < CENSUS_SLOTS; i++) {
        uint32_t n = g_census.r[i].rd + g_census.r[i].wr;
        if (!g_census.r[i].off || n <= top[15].n) {
            continue;
        }
        int k = 15;
        while (k > 0 && top[k - 1].n < n) {
            top[k] = top[k - 1];
            k--;
        }
        top[k].n = n;
        top[k].off = g_census.r[i].off - 1;
        top[k].rd = g_census.r[i].rd;
        top[k].wr = g_census.r[i].wr;
        top[k].last = g_census.r[i].last;
        g_census.r[i].rd = g_census.r[i].wr = 0;
    }
    char buf[900];
    int o = 0;
    for (int k = 0; k < 16 && top[k].n && o < (int)sizeof(buf) - 48; k++) {
        o += snprintf(buf + o, sizeof(buf) - o, " %06x:r%u/w%u=%08x", top[k].off, top[k].rd,
                      top[k].wr, top[k].last);
    }
    buf[o < (int)sizeof(buf) ? o : (int)sizeof(buf) - 1] = 0;
    if (o) {
        nc_log("hakuX-native-nv2a", "registers/s:%s", buf);
    }
}

/* The FIFO in push-buffer mode: empty when the HLE has consumed to PUT. */
static bool fifo_empty(void)
{
    return !g_pbmode || ld(NV_PFIFO_CACHE1_DMA_GET) == ld(NV_PFIFO_CACHE1_DMA_PUT);
}

/* ---- debug: the GPU event trace (host.h hle_host_gpu_trace) ---- */
uint64_t nc_ke_now_ns(void);           /* native/kernel/ke.c */
#define GTR_N 32768u
static struct {
    uint64_t ns;
    uint32_t kind, value;
} g_gtr[GTR_N];
static uint32_t g_gtr_pos;

void hle_host_gpu_trace(unsigned kind, uint32_t value)
{
    if (!g_pbmode) {
        return;
    }
    uint32_t i = __atomic_fetch_add(&g_gtr_pos, 1, __ATOMIC_RELAXED) % GTR_N;
    g_gtr[i].ns = nc_ke_now_ns();      /* the guest thread: once per kernel entry */
    g_gtr[i].kind = kind;
    g_gtr[i].value = value;
}

uint32_t hle_host_gpu_trace_mark(void)
{
    return __atomic_load_n(&g_gtr_pos, __ATOMIC_ACQUIRE);
}

void hle_host_gpu_trace_dump(const char *why, uint32_t mark)
{
    static const char *const k_names[] = {
        "?", "vblank", "pcrtc-clr", "pmc-en", "intr-clr", "fifo", "increment", "start",
        "trap", "flip-inc", "flip-stall", "resume", "dpc-queue", "dpc-run", "dpc-refused",
        "isr-ret", "irql",
    };
    uint32_t now = __atomic_load_n(&g_gtr_pos, __ATOMIC_ACQUIRE);
    uint32_t from = mark > 150 ? mark - 150 : 0, end = mark + 6000 < now ? mark + 6000 : now;
    if (now - from > GTR_N) {
        from = now - GTR_N;                     /* overwritten since */
    }
    uint32_t n = end > from ? end - from : 0;
    if (!n) {
        return;
    }
    const uint64_t t0 = g_gtr[(end - n) % GTR_N].ns;
    nc_log("hakuX-native-trace", "%s: %u GPU events, from 150 before the mark", why, n);
    char line[400];
    int o = 0;
    for (uint32_t j = end - n; j != end; j++) {
        const uint32_t i = j % GTR_N;
        const uint32_t k = g_gtr[i].kind < sizeof(k_names) / sizeof(k_names[0]) ? g_gtr[i].kind : 0;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " +%.2f %s %x",
                      (g_gtr[i].ns - t0) / 1e6, k_names[k], g_gtr[i].value);
        if (o > 300 || j + 1 == end) {
            nc_log("hakuX-native-trace", "%s", line);
            o = 0;
        }
    }
}

static uint32_t pmc_intr(void)
{
    uint32_t v = 0;
    if (ld(NV_PCRTC_INTR_0) & ld(NV_PCRTC_INTR_EN_0)) {
        v |= PMC_INTR_PCRTC;
    }
    if (g_pbmode && (ld(NV_PGRAPH_INTR) & ld(NV_PGRAPH_INTR_EN))) {
        v |= PMC_INTR_PGRAPH;                       /* push-buffer mode: a software trap */
    }
    return v;
}

static void update_irq(void)
{
    nc_irq_set(NV2A_IRQ, (ld(NV_PMC_INTR_EN_0) & 1) && pmc_intr());
}

static uint64_t nv2a_read_reg(uint32_t off, unsigned size);

static uint64_t nv2a_read(void *opaque, uint32_t off, unsigned size)
{
    const uint64_t v = nv2a_read_reg(off, size);
    if (__builtin_expect(g_census.on, 0)) {
        census(off, false, (uint32_t)v);
    }
    return v;
}

static uint64_t nv2a_read_reg(uint32_t off, unsigned size)
{
    /* Idle FIFO bits, for any access width inside the register (D3D tests
     * CACHE1_STATUS with a byte read). */
    const uint32_t word = off & ~3u, shift = (off & 3u) * 8;
    if (size <= 4 && (off & 3u) + size <= 4) {
        uint32_t v;
        switch (word) {
        case NV_PFIFO_RUNOUT_STATUS:
        case NV_PFIFO_CACHE1_STATUS:
            v = fifo_empty() ? PFIFO_LOW_MARK : 0;
            break;
        case NV_PFIFO_CACHE1_DMA_PUSH:
            v = fifo_empty() ? ld(word) & ~PFIFO_DMA_BUSY : ld(word) | PFIFO_DMA_BUSY;
            break;
        case NV_USER_DMA_PUT:
        case NV_USER_DMA_GET:
        case NV_USER_REF:
            if (!g_pbmode) {
                goto plain;
            }
            v = ld(word - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT);
            break;
        case NV_PFB_WBC:
            v = 0; /* no write-combine flush pending (Riddick spins on it) */
            break;
        default:
            goto plain;
        }
        return size == 4 ? v : (v >> shift) & ((1u << (size * 8)) - 1);
    }
plain:
    switch (off) {
    case NV_PMC_INTR_0:
        return pmc_intr();
    case NV_PTIMER_TIME_0:
        return (uint32_t)now_ns() & ~0x1Fu;
    case NV_PTIMER_TIME_1:
        return (uint32_t)(now_ns() >> 32) & 0x1FFFFFFFu;
    case NV_PCRTC_RASTER: { /* the line since the last vblank */
        const uint64_t since = now_ns() - __atomic_load_n(&g_last_vblank_ns, __ATOMIC_ACQUIRE);
        return (uint32_t)((since % g_frame_ns) * LINES / g_frame_ns);
    }
    default:
        break;
    }
    uint64_t v = 0;
    memcpy(&v, g_regs + off, size);
    return v;
}

#define NV_PRMDIO_WRITE_INDEX 0x6813C8u     /* the DAC palette: the entry to write */
#define NV_PRMDIO_DATA        0x6813C9u     /* ... then its R, G, B bytes, auto-incrementing */

/* Push-buffer mode: D3D's gamma ramp goes to the DAC's palette (index 0,
 * then the 768 data bytes: Riddick's LTCG 0x4c0f66, its brightness); the
 * whole table becomes the display's ramp. Not modelled, the ramp was lost
 * and Riddick's gameplay drew darker than in D3D mode. */
static struct {
    uint8_t index, comp;
    uint8_t rgb[3][256];
} g_dac;

static void dac_write(uint32_t off, uint8_t v)
{
    if (off == NV_PRMDIO_WRITE_INDEX) {
        g_dac.index = v;
        g_dac.comp = 0;
        return;
    }
    g_dac.rgb[g_dac.comp][g_dac.index] = v;
    if (++g_dac.comp == 3) {
        g_dac.comp = 0;
        if (++g_dac.index == 0) {               /* the 256th entry: the table is whole */
            hle_pb_gamma((const uint8_t (*)[256])g_dac.rgb);
        }
    }
}

static void nv2a_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    if (__builtin_expect(g_census.on, 0)) {
        census(off, true, (uint32_t)value);
    }
    if (g_pbmode && size == 1 && (off == NV_PRMDIO_WRITE_INDEX || off == NV_PRMDIO_DATA)) {
        dac_write(off, (uint8_t)value);
        return;
    }
    if (g_pbmode && size == 4 && off == NV_PFIFO_CACHE1_DMA_SUBROUTINE) {
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        hle_pb_subroutine_written((uint32_t)value);     /* D3D left a run (4928) */
        return;
    }
    if (g_pbmode && size == 4 && (off == NV_PFIFO_CACHE1_DMA_PUT || off == NV_PFIFO_CACHE1_DMA_GET)) {
        static unsigned s_logged_c;             /* debug: the first direct CACHE1 writes */
        if (s_logged_c++ < 16) {
            nc_log("hakuX-native", "nv2a: CACHE1 %s <- %08x (GET %08x PUT %08x)",
                   off == NV_PFIFO_CACHE1_DMA_PUT ? "PUT" : "GET", (uint32_t)value,
                   ld(NV_PFIFO_CACHE1_DMA_GET), ld(NV_PFIFO_CACHE1_DMA_PUT));
        }
        if (off == NV_PFIFO_CACHE1_DMA_GET) {
            hle_pb_get();                       /* a channel set-up: the scout syncs again */
        }
    }
    if (g_pbmode && size == 4 && (off == NV_USER_DMA_PUT || off == NV_USER_DMA_GET ||
                                  off == NV_USER_REF)) {
        static unsigned s_logged;               /* debug: the first channel writes */
        if (s_logged < 24 && off != NV_USER_REF) {
            s_logged++;
            nc_log("hakuX-native", "nv2a: channel %s <- %08x (GET %08x PUT %08x)",
                   off == NV_USER_DMA_PUT ? "PUT" : "GET", (uint32_t)value,
                   ld(NV_PFIFO_CACHE1_DMA_GET), ld(NV_PFIFO_CACHE1_DMA_PUT));
        }
        if (off == NV_USER_DMA_PUT) {
            hle_pb_pushed((uint32_t)value);         /* before the decoder can see it */
        }
        __atomic_store_n(reg(off - NV_USER_DMA_PUT + NV_PFIFO_CACHE1_DMA_PUT), (uint32_t)value,
                         __ATOMIC_RELEASE);
        if (off == NV_USER_DMA_PUT) {
            hle_pb_put();
        } else if (off == NV_USER_DMA_GET) {
            hle_pb_get();
        }
        return;
    }
    if (g_pbmode && size == 4 && off == NV_PGRAPH_FIFO) {
        hle_host_gpu_trace(HLE_GTR_FIFO, (uint32_t)value);
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        hle_pb_kick();                              /* access back: the decoder goes on */
        return;
    }
    if (g_pbmode && size == 4 && (off == NV_PGRAPH_INTR || off == NV_PGRAPH_INTR_EN)) {
        /* PGRAPH's interrupt: the ISR clears what it handled (1s), and a
         * decoder waiting on a trap goes on */
        if (off == NV_PGRAPH_INTR) {
            hle_host_gpu_trace(HLE_GTR_INTR_CLR, (uint32_t)value);
            __atomic_fetch_and(reg(off), ~(uint32_t)value, __ATOMIC_ACQ_REL);
        } else {
            __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        }
        update_irq();
        hle_pb_kick();
        return;
    }
    if (g_pbmode && off == NV_PGRAPH_INCREMENT && size == 4) {
        /* the vblank ISR's flip: the read counter advances (modulo), and a
         * FLIP_STALL in the push buffer may go on */
        if (value & 2) {
            uint32_t s = ld(NV_PGRAPH_SURFACE), n;
            do {
                const uint32_t mod = (s >> 28) & 7, rd = (s >> 24) & 7;
                n = (s & ~0x07000000u) | ((mod ? (rd + 1) % mod : 0) << 24);
            } while (!__atomic_compare_exchange_n(reg(NV_PGRAPH_SURFACE), &s, n, false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            static unsigned s_logged;
            if (s_logged++ < 32) {
                nc_log("hakuX-native", "nv2a: flip read -> %u (write %u, display %08x)",
                       (n >> 24) & 7, (n >> 20) & 7, ld(NV_PCRTC_START));
            }
            hle_host_gpu_trace(HLE_GTR_INCREMENT, (n >> 24) & 7);
            hle_pb_flipped(ld(NV_PCRTC_START));
            hle_pb_kick();
        }
        return;
    }
    switch (off) {
    case NV_PMC_INTR_0:
        return; /* a summary of the units' registers */
    case NV_PCRTC_INTR_0:
        hle_host_gpu_trace(HLE_GTR_PCRTC_CLR, (uint32_t)value);
        __atomic_fetch_and(reg(off), ~(uint32_t)value, __ATOMIC_ACQ_REL);
        update_irq();
        return;
    case NV_PMC_INTR_EN_0:
    case NV_PCRTC_INTR_EN_0:
        if (off == NV_PMC_INTR_EN_0) {
            hle_host_gpu_trace(HLE_GTR_PMC_EN, (uint32_t)value);
        }
        __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
        update_irq();
        return;
    case NV_PVIDEO_BUFFER:
        if (g_pbmode) {
            /* a buffer to show: in use (shown) until the overlay stops; D3D
             * stops it only when a buffer is in use (Forza 0x3bef1b) */
            __atomic_fetch_or(reg(off), (uint32_t)value, __ATOMIC_ACQ_REL);
            if (value & 1) {
                hle_pb_overlay_changed(true);
            }
            return;
        }
        break;
    case NV_PVIDEO_STOP:
        if (g_pbmode) {
            /* the overlay stops: no buffer pending, as D3D waits to see
             * (EnableOverlay(FALSE) spins on BUFFER until 0: Forza 0x3bef31) */
            __atomic_store_n(reg(off), (uint32_t)value, __ATOMIC_RELEASE);
            if (value & 1) {
                __atomic_store_n(reg(NV_PVIDEO_BUFFER), 0u, __ATOMIC_RELEASE);
                hle_pb_overlay_changed(false);
            }
            return;
        }
        break;
    case NV_PCRTC_START:
        hle_host_gpu_trace(HLE_GTR_START, (uint32_t)value);
        if (g_pbmode) {
            hle_pb_display_start((uint32_t)value);  /* the display shows that memory */
        }
        break;
    default:
        break;
    }
    memcpy(g_regs + off, &value, size);
}

void nc_dev_nv2a_display_vsync(uint64_t ns)
{
    const uint64_t last = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    if (last && ns > last) {
        const uint64_t d = ns - last;
        uint64_t p = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
        /* a callback that came late spans whole periods */
        const uint64_t n = p ? (d + p / 2) / p : 1;
        const uint64_t one = n ? d / n : d;
        if (n >= 1 && n <= 4 && one > 5000000ull && one < 50000000ull) {
            p = p ? p + ((int64_t)one - (int64_t)p) / 16 : one;
            __atomic_store_n(&g_disp_period_ns, p, __ATOMIC_RELAXED);
        }
    }
    __atomic_store_n(&g_disp_vsync_ns, ns, __ATOMIC_RELEASE);
}

void nc_dev_nv2a_display_period(uint64_t ns)
{
    __atomic_store_n(&g_disp_period_ns, ns, __ATOMIC_RELAXED);
}

uint64_t nc_dev_nv2a_next_vsync(uint64_t after)
{
    const uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    const uint64_t pd = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    if (!vs || !pd || (after > vs && after - vs > VSYNC_STALE)) {
        return 0;
    }
    return after < vs ? vs : vs + ((after - vs) / pd + 1) * pd;
}

/* x into (-pd/2, pd/2]. */
static int64_t wrap_half(int64_t x, int64_t pd)
{
    x %= pd;
    if (x <= -pd / 2) {
        x += pd;
    } else if (x > pd / 2) {
        x -= pd;
    }
    return x;
}

void nc_dev_nv2a_display_pickup(uint64_t ns)
{
    static int64_t logged = -1;
    const uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    const int64_t pd = (int64_t)__atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    if (!vs || !pd) {
        return;
    }
    int64_t off = ((int64_t)ns - (int64_t)vs) % pd;
    off = off < 0 ? off + pd : off;
    int64_t est = __atomic_load_n(&g_pick_off_ns, __ATOMIC_RELAXED);
    est = est < 0 ? off : est + wrap_half(off - est, pd) / 16;
    est = (est % pd + pd) % pd;
    __atomic_store_n(&g_pick_off_ns, est, __ATOMIC_RELAXED);
    if (logged < 0 || llabs(wrap_half(est - logged, pd)) > 2000000) {
        logged = est;
        nc_log("hakuX-native", "nv2a: the presenter picks frames up %.1f ms after the panel's "
               "vsync (period %.2f ms): vblanks %.1f ms after the vsync", est / 1e6, pd / 1e6,
               (double)((est + pd / 2) % pd) / 1e6);
    }
}

/* A frame after prev at the mode's exact rate (the vblank thread's: it
 * carries the fraction of a nanosecond). */
static uint64_t g_frame_acc;

static uint64_t frame_after(uint64_t prev, uint64_t frame)
{
    uint64_t acc = g_frame_acc;
    const uint64_t den = __atomic_load_n(&g_frame_den, __ATOMIC_RELAXED);
    acc += __atomic_load_n(&g_frame_rem, __ATOMIC_RELAXED);
    const uint64_t extra = den ? acc / den : 0;
    g_frame_acc = den ? acc % den : 0;
    return prev + frame + extra;
}

/* The next vblank after one at prev: a frame later, or locked to the panel
 * (its period times the vsyncs per frame, the phase nudged at most 0.5 ms a
 * vblank so the guest's clock never jumps). */
static uint64_t next_vblank(uint64_t prev)
{
    const uint64_t frame = __atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
    const uint64_t vs = __atomic_load_n(&g_disp_vsync_ns, __ATOMIC_ACQUIRE);
    const uint64_t pd = __atomic_load_n(&g_disp_period_ns, __ATOMIC_RELAXED);
    if (!vs || !pd || now_ns() - vs > VSYNC_STALE) {
        return frame_after(prev, frame);    /* no panel (the app in the background) */
    }
    const uint64_t k = (frame + pd / 2) / pd;
    if (k < 1 || k > 4 || llabs((int64_t)(k * pd) - (int64_t)frame) > (int64_t)(frame / 20)) {
        return frame_after(prev, frame);    /* 90 Hz and the like: free running */
    }
    const uint64_t next = prev + k * pd;
    const int64_t pick = __atomic_load_n(&g_pick_off_ns, __ATOMIC_RELAXED);
    const int64_t at = pick < 0 ? -VSYNC_LEAD : pick + (int64_t)pd / 2;  /* after a vsync */
    const int64_t ph = wrap_half((int64_t)next - (int64_t)vs - at, (int64_t)pd);
    int64_t c = -ph / 8;
    c = c > 500000 ? 500000 : c < -500000 ? -500000 : c;
    return next + (uint64_t)c;
}

/* Uncapped (the app's setting: HAKUX_HLE_UNCAP; debug.hakux.uncap 1 / 0
 * on demand), read by the vblank thread now and then. */
static bool uncapped(void)
{
    static int on = -1;
    static unsigned polls;
    if (on < 0) {
        on = getenv("HAKUX_HLE_UNCAP") != NULL;
    }
    if (polls++ % 32 == 0) {
        char v[NC_PROPERTY_VALUE_MAX] = "";
        if (hle_prop_get("debug.hakux.uncap", v) > 0) {
            on = atoi(v) != 0;
        }
    }
    return on;
}

#define EARLY_GAP_NS 1000000ull     /* uncapped: early vblanks at most this often */
#define EARLY_POLL_NS 250000ull

/* Uncapped push-buffer mode: the title's own D3D flips in its vblank
 * handler (and with a presentation interval of n, every n vblanks), so
 * while PGRAPH waits in FLIP_STALL (the buffer it would draw next is still
 * the one shown: the flip counters' read equals write) an early vblank
 * comes once the last one was handled; the frame rate then follows
 * rendering, as IMMEDIATE swaps do in D3D mode. Titles that count vblanks run fast, as frame-locked ones do
 * there. Returns when the vblank is due: next, or earlier. */
static uint64_t wait_vblank(uint64_t next, uint64_t last)
{
    if (g_pbmode && uncapped()) {
        for (uint64_t t = now_ns(); t < next; t = now_ns()) {
            if (t - last >= EARLY_GAP_NS && hle_pb_flip_waiting() &&
                !(ld(NV_PCRTC_INTR_0) & PCRTC_VBLANK)) {
                return t;
            }
            const uint64_t until = next < t + EARLY_POLL_NS ? next : t + EARLY_POLL_NS;
            const struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        return next;
    }
    const struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
    }
    return next;
}

static struct {
    pthread_t th;
    bool started;
    int quit;                   /* nc_dev_nv2a_stop */
} g_vbt;

static void *vblank_thread(void *arg)
{
    nc_pause_register("vblank");
    uint64_t next = now_ns();
    while (!__atomic_load_n(&g_vbt.quit, __ATOMIC_ACQUIRE)) {
        if (nc_paused()) {
            /* the emulation paused: no vblanks, and from now on resuming
             * (not the ones missed meanwhile, one after the other) */
            nc_pause_wait();
            next = now_ns();
        }
        const uint64_t due = next_vblank(next);
        next = wait_vblank(due, next);
        __atomic_store_n(&g_last_vblank_ns, next, __ATOMIC_RELEASE);
        __atomic_fetch_or(reg(NV_PCRTC_INTR_0), PCRTC_VBLANK, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&g_vblanks, 1, __ATOMIC_RELAXED);
        hle_host_gpu_trace(HLE_GTR_VBLANK, (uint32_t)g_vblanks | (next < due ? 0x80000000u : 0));
        update_irq();
        if (g_pbmode) {
            hle_pb_vblank();    /* the scanout shows the memory, flipped or not */
        }
        if (g_census.on && next - g_census.t_log >= 1000000000ull) {
            g_census.t_log = next;
            census_log();
        }
    }
    return NULL;
}

void nc_dev_nv2a_init(void)
{
    g_regs = nc_big_alloc(NV2A_SIZE); /* pages are touched only where the guest writes */
    nc_wr32(g_regs + NV_PMC_BOOT_0, 0x02A000A1u);
    nc_wr32(g_regs + NV_PFB_CSTATUS, 64u << 20);
    char v[NC_PROPERTY_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.nvcensus", v);
    const char *e = getenv("NC_NV2A_CENSUS");
    g_census.on = !strcmp(v, "1") || (e && !strcmp(e, "1"));
    const NcMmioDev dev = {"nv2a", NV2A_BASE, NV2A_SIZE, nv2a_read, nv2a_write, NULL};
    nc_mmio_register(&dev);
    g_vbt.quit = 0;
    g_vbt.started = pthread_create(&g_vbt.th, NULL, vblank_thread, NULL) == 0;
}

void nc_dev_nv2a_stop(void)
{
    if (g_vbt.started) {
        __atomic_store_n(&g_vbt.quit, 1, __ATOMIC_RELEASE);
        pthread_join(g_vbt.th, NULL);   /* within a frame: it sleeps until the next vblank */
        g_vbt.started = false;
    }
}


/* ---- the HLE GPU's seams (hw/xbox/hle/host.h) ---- */

/* A mod's faster vblank (mods/README.md refresh:): the video mode's rate
 * times this; a frame-locked title paced by vblanks then runs that much
 * faster unless the mod halves its steps. */
static unsigned g_mode_hz = 60, g_vblank_mul = 1;

void nc_dev_nv2a_refresh(unsigned hz)
{
    g_mode_hz = hz == 50 ? 50 : 60;
    /* the period, num / den ns: 1e9 x 1001 / 60000 (60000/1001 Hz) or 1e9 / 50 */
    const uint64_t num = hz == 50 ? 1000000000ull : 1001000000000ull;
    const uint64_t den = (hz == 50 ? 50ull : 60000ull) * g_vblank_mul;
    const uint64_t f = num / den;
    __atomic_store_n(&g_frame_rem, num % den, __ATOMIC_RELAXED);
    __atomic_store_n(&g_frame_den, den, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&g_frame_ns, f, __ATOMIC_RELAXED) != f) {
        nc_log("hakuX-native", "nv2a: vblank at %u Hz (the video mode%s)", g_mode_hz * g_vblank_mul,
               g_vblank_mul > 1 ? ", a mod's rate" : "");
    }
}

void nc_dev_nv2a_display_mode(uint32_t pitch, uint32_t xfmt, unsigned hz)
{
    hle_pb_display_mode(pitch, xfmt, hz);
}

void nc_dev_nv2a_vblank_multiply(unsigned n)
{
    g_vblank_mul = n >= 1 && n <= 4 ? n : 1;
    nc_dev_nv2a_refresh(g_mode_hz);
}

uint64_t hle_host_vblank_count(void)
{
    return __atomic_load_n(&g_vblanks, __ATOMIC_RELAXED);
}

int64_t hle_host_vblank_period_ns(void)
{
    return (int64_t)__atomic_load_n(&g_frame_ns, __ATOMIC_RELAXED);
}

uint32_t hle_host_gpu_reg(uint32_t off)
{
    return g_regs && off < NV2A_SIZE - 3 ? ld(off & ~3u) : 0;
}

void hle_host_gpu_reg_set(uint32_t off, uint32_t v)
{
    if (g_regs && off < NV2A_SIZE - 3) {
        __atomic_store_n(reg(off & ~3u), v, __ATOMIC_RELEASE);
    }
}

void hle_host_gpu_reg_mask(uint32_t off, uint32_t mask, uint32_t bits)
{
    if (!g_regs || off >= NV2A_SIZE - 3) {
        return;
    }
    uint32_t *r = reg(off & ~3u), v = __atomic_load_n(r, __ATOMIC_ACQUIRE);
    while (!__atomic_compare_exchange_n(r, &v, (v & ~mask) | (bits & mask), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
}

void hle_host_gpu_irq_update(void)
{
    if (g_regs) {
        update_irq();
    }
}

void hle_host_pb_mode(bool on)
{
    __atomic_store_n(&g_pbmode, on, __ATOMIC_RELEASE);
    nc_log("hakuX-native", "nv2a: push-buffer mode %s", on ? "on" : "off");
}

uint64_t nc_dev_nv2a_vblanks(void)
{
    return __atomic_load_n(&g_vblanks, __ATOMIC_RELAXED);
}

/* The other device windows (APU, ACI, USB, NIC, flash): register files that
 * show up in the MMIO log until each gets a device. */
static uint8_t *g_misc;
#define MISC_BASE 0xFE000000u
#define MISC_SIZE 0x02000000u

static uint64_t misc_read(void *opaque, uint32_t off, unsigned size)
{
    uint64_t v = 0;
    memcpy(&v, g_misc + off, size);
    return v;
}

static void misc_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    memcpy(g_misc + off, &value, size);
}

void nc_dev_misc_init(void)
{
    g_misc = nc_big_alloc(MISC_SIZE);
    const NcMmioDev dev = {"misc", MISC_BASE, MISC_SIZE, misc_read, misc_write, NULL};
    nc_mmio_register(&dev);
}

void nc_dev_nv2a_free(void)
{
    nc_big_free(g_regs, NV2A_SIZE);
    g_regs = NULL;
    g_vblanks = 0;
    g_frame_ns = FRAME_NS;
    g_frame_rem = 20000;
    g_frame_den = 60000;
    g_frame_acc = 0;
    g_last_vblank_ns = 0;
    g_disp_vsync_ns = g_disp_period_ns = 0;
    g_pick_off_ns = -1;
    g_pbmode = false;
    memset(&g_census, 0, sizeof(g_census));
    memset(g_gtr, 0, sizeof(g_gtr));
    g_gtr_pos = 0;
    memset(&g_dac, 0, sizeof(g_dac));
    g_mode_hz = 60;
    g_vblank_mul = 1;
    nc_big_free(g_misc, MISC_SIZE);
    g_misc = NULL;
}

/* ---------------------------------------------------------- savestates */

typedef struct NcNv2aState {
    uint64_t vblanks, frame_ns;
    uint32_t npages;
} NcNv2aState;

typedef struct NcMiscState {
    uint32_t npages;
} NcMiscState;

#define NC_DEF_SECTION NV2A
#define NC_DEF_FILE "native/state/nv2a.def"
#include "native/state/def.h"
#undef NC_DEF_SECTION
#undef NC_DEF_FILE
#define NC_DEF_SECTION MISC
#define NC_DEF_FILE "native/state/misc.def"
#include "native/state/def.h"
#include "native/state/sections.h"

static bool page_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    for (unsigned i = 0; i < NC_PAGE / 8; i++) {
        if (q[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t pages_put(NcBuf *b, const uint8_t *base, uint32_t size, bool count_only)
{
    uint32_t n = 0;
    for (uint32_t pg = 0; pg < size / NC_PAGE; pg++) {
        if (!page_zero(base + (size_t)pg * NC_PAGE)) {
            if (!count_only) {
                nc_buf_u32(b, pg);
                nc_buf_put(b, base + (size_t)pg * NC_PAGE, NC_PAGE);
            }
            n++;
        }
    }
    return n;
}

static bool pages_get(NcRd *r, uint8_t *base, uint32_t size, uint32_t n)
{
    for (uint32_t pg = 0; pg < size / NC_PAGE; pg++) {
        if (!page_zero(base + (size_t)pg * NC_PAGE)) {
            memset(base + (size_t)pg * NC_PAGE, 0, NC_PAGE);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t pg = nc_rd_u32(r);
        if (pg >= size / NC_PAGE || !nc_rd_get(r, base + (size_t)pg * NC_PAGE, NC_PAGE)) {
            return false;
        }
    }
    return !r->fail;
}

bool nc_nv2a_state_save(NcBuf *b)
{
    const NcNv2aState st = {__atomic_load_n(&g_vblanks, __ATOMIC_RELAXED), g_frame_ns,
                            pages_put(NULL, g_regs, NV2A_SIZE, true)};
    nc_rec_put(b, &nc_rec_NcNv2aState, &st);
    pages_put(b, g_regs, NV2A_SIZE, false);
    return true;
}

bool nc_nv2a_state_load(NcRd *r, unsigned v)
{
    NcNv2aState st = {0, FRAME_NS, 0};
    nc_rec_get(r, &nc_rec_NcNv2aState, &st, v);
    if (!pages_get(r, g_regs, NV2A_SIZE, st.npages)) {
        return false;
    }
    __atomic_store_n(&g_vblanks, st.vblanks, __ATOMIC_RELAXED);
    /* the state's video mode (50 Hz: a 20 ms frame), at this run's rate (the
     * mods on now: a state saved with or without them) */
    nc_dev_nv2a_refresh(st.frame_ns >= 18000000ull ? 50 : 60);
    g_last_vblank_ns = now_ns();
    g_disp_vsync_ns = 0;
    g_pick_off_ns = -1;
    update_irq();
    return true;
}

bool nc_misc_state_save(NcBuf *b)
{
    const NcMiscState st = {pages_put(NULL, g_misc, MISC_SIZE, true)};
    nc_rec_put(b, &nc_rec_NcMiscState, &st);
    pages_put(b, g_misc, MISC_SIZE, false);
    return true;
}

bool nc_misc_state_load(NcRd *r, unsigned v)
{
    NcMiscState st = {0};
    nc_rec_get(r, &nc_rec_NcMiscState, &st, v);
    return pages_get(r, g_misc, MISC_SIZE, st.npages);
}
