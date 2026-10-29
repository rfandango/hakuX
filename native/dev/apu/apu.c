/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
/* hakuX native core: the device on the core's MMIO window (0xFE800000) and
 * interrupt line 5, its frame thread, and its settings. */

#include "native/dev/apu/apu_int.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"
#include "native/dev/dev.h"

/* HAKUX_APU_STATS=1 -> 1 Hz hakuX-apu line (audio evidence) */
static bool g_apu_stats;
static int g_apu_fifo_min = INT_MAX, g_apu_fifo_max;
extern uint32_t g_vp_oob_clamps;
extern uint32_t g_vp_held_skips, g_vp_held_waits, g_vp_held_wait_max_us, g_vp_held_run_max; /* vp.c voice_take */
/* NC_APU_TRAP_PARK=1: the frames stop while the FE is trapped (frames_stopped) */
static bool g_trap_park;

MCPXAPUState *g_state; /* the one APU: the savestate section's */
static bool g_apu_trace;                /* NC_TRACE_APU: its thread runs */
static pthread_t g_apu_trace_thread;
uint8_t *g_apu_ram;
uint32_t g_apu_ram_size;
ApuConfig g_apu_config = {
    .use_dsp = false,
    .hrtf = true,
    .volume_limit = 1.0f,
    .num_workers = 0,
};

/* ------------------------------------------------------------ platform */

void apu_cond_init(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

bool apu_cond_timedwait_us(pthread_cond_t *c, pthread_mutex_t *m, int64_t us)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += us / 1000000;
    ts.tv_nsec += (long)(us % 1000000) * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    return pthread_cond_timedwait(c, m, &ts) == 0;
}

bool apu_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, int ms)
{
    return apu_cond_timedwait_us(c, m, (int64_t)ms * 1000);
}

/* The device code was written for desktop stacks (8 MiB); bionic's default
 * is 1 MiB. */
void apu_thread_start(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, bool joinable)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    char n[16];
    snprintf(n, sizeof(n), "%s", name);
    pthread_setname_np(*t, n);
    if (!joinable) {
        pthread_detach(*t);
    }
}

/* ----------------------------------------------------------- registers */

static void update_irq(MCPXAPUState *d)
{
    pthread_mutex_lock(&d->irq_lock);
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        apu_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        apu_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        nc_irq_set(MCPX_APU_IRQ, true);
    } else {
        apu_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        nc_irq_set(MCPX_APU_IRQ, false);
    }
    pthread_mutex_unlock(&d->irq_lock);
}

/* XGSCNT, the global sample counter: 48 kHz (DirectSoundGetSampleTime,
 * DirectMusic's synth clock), from the guest's time (paused with it,
 * restored with its savestates), counting from the value the guest last
 * wrote (DirectSound writes 0 at its start); the register holds the offset.
 * It returned the guest's time in 100 ns units (xemu's guess): 208 times
 * too fast, wrapping every 7 minutes. Nightcaster's DirectMusic then never
 * let go of a wave it had played nor of its audio path: its 6 MiB heap
 * filled in ~3 minutes and the game crashed on a null allocation. */
static uint32_t gscnt_now(void)
{
    return (uint32_t)(nc_ke_now_ns() * 6 / 125000);        /* 48000 / 1e9 */
}

static uint64_t mcpx_apu_read(MCPXAPUState *d, uint32_t addr, unsigned int size)
{
    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = gscnt_now() - apu_ld(&d->regs[NV_PAPU_XGSCNT]);
        break;
    default:
        if (addr < 0x20000) {
            r = apu_ld(&d->regs[addr]);
        }
        break;
    }
    return r;
}

/* A guest CPU thread parked waiting on memory the APU writes. Without the
 * DSP preference (use_dsp off, the default) a GP frame runs 1000 DSP cycles,
 * so the DirectSound program reaches its command check only every ~60 frames
 * (~40 ms) while the game spins on the command word (PGR2: ~40% of its CPU
 * time). For the next few milliseconds GP frames run to completion
 * instead. */
#define GP_BOOST_US 3000

static void mcpx_apu_spin_wait(void)
{
    MCPXAPUState *d = g_state;
    if (d) {
        apu_st(&d->gp_boost_until_us, apu_host_us() + GP_BOOST_US);
    }
}

static void mcpx_apu_write(MCPXAPUState *d, uint32_t addr, uint64_t val,
                           unsigned int size)
{
    switch (addr) {
    case NV_PAPU_ISTS:
        /* the bits of the interrupts to clear are written */
        apu_and(&d->regs[NV_PAPU_ISTS], ~val);
        apu_inc(&d->ists_writes);
        update_irq(d);
        pthread_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        apu_st(&d->regs[addr], val);
        pthread_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write'
         * This value is expected to be written to FEMEMADDR on completion of
         * something to do with notifies. Just do it now :/ */
        apu_ram_st32(d->regs[NV_PAPU_FEMEMADDR], val);
        apu_st(&d->regs[addr], val);
        break;
    case NV_PAPU_XGSCNT:
        apu_st(&d->regs[NV_PAPU_XGSCNT], gscnt_now() - (uint32_t)val);
        break;
    case NV_PAPU_VPVADDR:
        apu_st(&d->regs[NV_PAPU_VPVADDR], val);
        mcpx_apu_vp_refresh_voice_cache(d, val);
        break;
    case NV_PAPU_VPSSLADDR:
        apu_st(&d->regs[NV_PAPU_VPSSLADDR], val);
        mcpx_apu_vp_refresh_ssl_cache(d, val);
        break;
    default:
        if (addr < 0x20000) {
            apu_st(&d->regs[addr], val);
        }
        break;
    }
}

/* The window: the registers below 0x20000, the VP's methods at 0x20000, the
 * GP's at 0x30000 and the EP's at 0x50000 (0x10000 bytes each). Wider
 * accesses than 4 bytes go to the handlers in 4-byte pieces, narrower ones
 * as they are (the handlers' contract in xemu). */
typedef uint64_t (*ApuRead)(void *opaque, uint32_t addr, unsigned int size);
typedef void (*ApuWrite)(void *opaque, uint32_t addr, uint64_t val, unsigned int size);

static uint64_t regs_read(void *opaque, uint32_t addr, unsigned int size)
{
    return mcpx_apu_read(opaque, addr, size);
}

static void regs_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    mcpx_apu_write(opaque, addr, val, size);
}

static void route(uint32_t *off, ApuRead *rd, ApuWrite *wr)
{
    if (*off - 0x20000 < 0x10000) {
        *off -= 0x20000;
        *rd = mcpx_apu_vp_read;
        *wr = mcpx_apu_vp_write;
    } else if (*off - 0x30000 < 0x10000) {
        *off -= 0x30000;
        *rd = mcpx_apu_gp_read;
        *wr = mcpx_apu_gp_write;
    } else if (*off - 0x50000 < 0x10000) {
        *off -= 0x50000;
        *rd = mcpx_apu_ep_read;
        *wr = mcpx_apu_ep_write;
    } else {
        *rd = regs_read;
        *wr = regs_write;
    }
}

static uint64_t piece_mask(unsigned step)
{
    return step == 8 ? ~0ull : (1ull << (8 * step)) - 1;
}

static uint64_t apu_mmio_read(void *opaque, uint32_t off, unsigned size)
{
    ApuRead rd;
    ApuWrite wr;
    route(&off, &rd, &wr);
    const unsigned step = MIN(size, 4u);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i += step) {
        v |= (rd(opaque, off + i, step) & piece_mask(step)) << (8 * i);
    }
    return v;
}

static void apu_mmio_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    ApuRead rd;
    ApuWrite wr;
    route(&off, &rd, &wr);
    const unsigned step = MIN(size, 4u);
    for (unsigned i = 0; i < size; i += step) {
        wr(opaque, off + i, (value >> (8 * i)) & piece_mask(step), step);
    }
}

/* ---------------------------------------------------------- the frames */

/* One GP frame (32 samples) of real time; the EP boundary takes the rest of
 * EP_FRAME_US so eight frames stay exactly one EP frame. */
#define GP_FRAME_US (EP_FRAME_US / 8)
/* A VOICE_OFF's notification waits at most 64 frames (vp.c voice_off). */
#define OFF_WAIT_US (64 * GP_FRAME_US)

/* Microsecond sleeps (millisecond waits are too coarse on Android), the lock
 * released meanwhile like a timed wait would. */
static void sleep_until(MCPXAPUState *d, int64_t deadline_us)
{
    while (!apu_ld(&d->exiting)) {
        int64_t remaining_us = deadline_us - apu_host_us();
        if (remaining_us <= 50) {
            break;
        }
        pthread_mutex_unlock(&d->lock);
        usleep((useconds_t)MIN(remaining_us, EP_FRAME_US));
        pthread_mutex_lock(&d->lock);
    }
}

static void throttle(MCPXAPUState *d)
{
    /* GP frames one at a time, as the hardware runs them. The GP DSP takes
     * DirectSound's commands once per frame while the CPU spins waiting;
     * rendered in bursts (a whole output callback, ~43 ms, then a sleep) or
     * eight at a time, a command waited milliseconds (PGR2 spent ~37% of its
     * CPU time in that spin). */
    if (d->ep_frame_div % 8) {
        if (d->pace_gp && d->monitor.fifo_capacity_bytes > 0) {
            int64_t t0 = apu_host_us();
            sleep_until(d, d->next_frame_time_us);
            d->next_frame_time_us += GP_FRAME_US;
            d->sleep_acc_us += apu_host_us() - t0;
        }
        return;
    }
    d->pace_gp = false;

    if (d->monitor.fifo_capacity_bytes <= 0) {
        return;
    }

    int64_t start_us = apu_host_us();
    int queued_bytes = apu_out_queued();

    while (!apu_ld(&d->exiting) && !nc_paused() &&
           queued_bytes >= d->monitor.queued_bytes_high) {
        apu_cond_timedwait_ms(&d->cond, &d->lock, EP_FRAME_US / 1000);
        if (apu_ld(&d->exiting)) {
            break;
        }
        queued_bytes = apu_out_queued();
    }

    /* Above the low watermark, frames are paced in real time (one GP frame
     * per GP_FRAME_US). */
    if (queued_bytes > d->monitor.queued_bytes_low) {
        int64_t now_us = apu_host_us();
        if (d->next_frame_time_us == 0 ||
            now_us - d->next_frame_time_us > EP_FRAME_US) {
            d->next_frame_time_us = now_us;
        }
        sleep_until(d, d->next_frame_time_us);
        d->next_frame_time_us += EP_FRAME_US - 7 * GP_FRAME_US;
        d->pace_gp = true;

        /* Nudge frame timing based on queue level to avoid drifting
         * toward one of the watermarks.
         */
        int mid = (d->monitor.queued_bytes_low + d->monitor.queued_bytes_high) / 2;
        d->next_frame_time_us += (queued_bytes > mid) - (queued_bytes < mid);
    } else {
        d->next_frame_time_us = start_us;
    }

    d->sleep_acc_us += apu_host_us() - start_us;
}

/* Once a second, from the frame thread: states of the guest's that last,
 * the APU left waiting on it: a hang's evidence for the session log, each
 * logged when it has lasted from one look to the next, and again only after
 * it ended. The FE trapped (the trap's handler never ran: the guest's
 * interrupts or its DPCs held off), the interrupt up and not acknowledged.
 * A voice the guest holds is vp.c voice_take's. */
enum { STUCK_TRAP = 1, STUCK_IRQ = 2 };

static void stuck_check(MCPXAPUState *d, int64_t now_ms)
{
    if (now_ms - d->stuck.at_ms < 1000) {
        return;
    }
    d->stuck.at_ms = now_ms;
    const uint32_t fectl = apu_ld(&d->regs[NV_PAPU_FECTL]), ists = apu_ld(&d->regs[NV_PAPU_ISTS]);
    const uint32_t acks = apu_ld(&d->ists_writes);
    unsigned now = 0;
    if ((fectl & NV_PAPU_FECTL_FEMETHMODE) == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        now |= STUCK_TRAP;
    }
    if ((ists & NV_PAPU_ISTS_GINTSTS) && acks == d->stuck.ists_writes) {
        now |= STUCK_IRQ;
    }
    d->stuck.ists_writes = acks;
    const unsigned fresh = now & d->stuck.seen & ~d->stuck.logged;
    d->stuck.seen = now;
    d->stuck.logged &= now;
    if (fresh) {
        d->stuck.logged |= fresh;
        nc_log("hakuX-apu", "stuck: %s%s%s for a second | FECTL %08x (method %04x %08x) ISTS %08x IEN %08x "
                            "SECTL %08x, VOICE_OFFs waiting: %s",
               fresh & STUCK_TRAP ? "the FE trapped" : "", fresh == (STUCK_TRAP | STUCK_IRQ) ? ", " : "",
               fresh & STUCK_IRQ ? "the interrupt up, not acknowledged" : "", fectl,
               apu_ld(&d->regs[NV_PAPU_FEDECMETH]), apu_ld(&d->regs[NV_PAPU_FEDECPARAM]), ists,
               apu_ld(&d->regs[NV_PAPU_IEN]), apu_ld(&d->regs[NV_PAPU_SECTL]),
               mcpx_apu_vp_off_pending() ? "yes" : "no");
    }
}

/* The frames stop for the guest's SECTL (XCNTMODE off) and FECTL (halted),
 * not for a trap: the FE's trap holds its methods, not the SE nor the GP
 * and EP DSPs, separate engines on the hardware. Stopped for one, a guest
 * that waits on the APU at DISPATCH_LEVEL (DirectSound's lock), the trap's
 * DPC kept out, waited for good: for the GP to take a mailbox command, a
 * voice's notification (Blood Wake, its VOICE_OFF's: the 64-frame wait
 * below). A trap's idle reports wait for its handler (vp.c idle_report). */
static bool frames_stopped(MCPXAPUState *d)
{
    /* a mode, not bits: TRAPPED (0xE0) has HALTED's (0x80) */
    const uint32_t mode = apu_ld(&d->regs[NV_PAPU_FECTL]) & NV_PAPU_FECTL_FEMETHMODE;
    return GET_MASK(apu_ld(&d->regs[NV_PAPU_SECTL]), NV_PAPU_SECTL_XCNTMODE) == NV_PAPU_SECTL_XCNTMODE_OFF ||
           (mode != NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING &&
            (mode != NV_PAPU_FECTL_FEMETHMODE_TRAPPED || g_trap_park));
}

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);

    /* A rudimentary calculation to determine approximately how taxed the APU
     * thread is, by measuring how much time we spend waiting for FIFO to drain
     * versus working on building frames.
     * =1: thread is not sleeping and likely falling behind realtime
     * <1: thread is able to complete work on time
     */
    int64_t now = apu_host_ms();
    if (now - d->frame_count_time_ms >= 1000) {
        float t = 1.0f - ((double)d->sleep_acc_us /
                          (double)((now - d->frame_count_time_ms) * 1000));

        stuck_check(d, now);
        if (g_apu_stats) {
            uint32_t cb, short_cb;
            apu_out_stats(&cb, &short_cb);
            nc_log("hakuX-apu", "frames=%d util=%.2f fifo=%d-%d short=%u cb=%u vp_oob=%u held: skipped=%u "
                                "run=%u waited=%u (max %u us)",
                   d->frame_count, t, g_apu_fifo_min == INT_MAX ? 0 : g_apu_fifo_min,
                   g_apu_fifo_max, short_cb, cb, apu_ld(&g_vp_oob_clamps), apu_xchg(&g_vp_held_skips, 0),
                   apu_xchg(&g_vp_held_run_max, 0), apu_xchg(&g_vp_held_waits, 0),
                   apu_xchg(&g_vp_held_wait_max_us, 0));
            g_apu_fifo_min = INT_MAX;
            g_apu_fifo_max = 0;
        }
        d->frame_count_time_ms = now;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;
    if (g_apu_stats) {
        int used = apu_out_queued();
        g_apu_fifo_min = MIN(g_apu_fifo_min, used);
        g_apu_fifo_max = MAX(g_apu_fifo_max, used);
    }

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = { 0 };

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);

    if ((d->ep_frame_div + 1) % 8 == 0) {
        if (0 <= g_apu_config.volume_limit && g_apu_config.volume_limit < 1) {
            float f = pow(g_apu_config.volume_limit, M_E);
            for (int i = 0; i < 256; i++) {
                d->monitor.frame_buf[i][0] *= f;
                d->monitor.frame_buf[i][1] *= f;
            }
        }

        if (d->monitor.fifo_capacity_bytes > 0) {
            apu_out_push(d->monitor.frame_buf, sizeof(d->monitor.frame_buf));
        }
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    }

    d->ep_frame_div++;

    /* PERF_LOG-gated silent-but-active voice probe emit (no-op in release).
     * Deliberately NOT inside the `% 8` FIFO block above (that is gated on
     * frame phase); the probe self-throttles its own log cadence. */
    vp_silent_stats_log_and_reset();
}

static int getenv_int_clamped(const char *name, int min_value, int max_value,
                              int fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return (int)parsed;
}

/* The output: a ring of NC_APU_FIFO_FRAMES EP frames (48), drained in
 * callbacks of NC_APU_SAMPLES frames (512, 10.7 ms). The frame thread fills
 * it to three callbacks and paces itself in real time above one: ~32 ms
 * queued. Callbacks of 2048 queued ~128 ms (~130 ms from the guest to the
 * speaker); with the callback non-blocking (fill), 512 and 256 ran a minute
 * of Halo 2 on the Odin 3 without a short callback. */
static void monitor_init(MCPXAPUState *d)
{
    const char *st = getenv("HAKUX_APU_STATS");
    g_apu_stats = st && atoi(st) != 0;
    d->monitor.queued_bytes_low = 0;
    d->monitor.queued_bytes_high = 0;

    int fifo_frames = getenv_int_clamped("NC_APU_FIFO_FRAMES", 3, 128, 48);
    int audio_samples = getenv_int_clamped("NC_APU_SAMPLES", 256, 4096, 512);
    int fifo_frame_bytes = sizeof(d->monitor.frame_buf);
    int fifo_capacity_bytes = fifo_frames * fifo_frame_bytes;

    int drain_bytes = apu_out_open(fifo_capacity_bytes, audio_samples, &d->cond);
    if (drain_bytes <= 0) {
        drain_bytes = audio_samples * 2 * sizeof(int16_t);
    }
    drain_bytes = MAX(drain_bytes, fifo_frame_bytes);
    d->monitor.fifo_capacity_bytes = fifo_capacity_bytes;
    int max_high = MAX(d->monitor.fifo_capacity_bytes - fifo_frame_bytes,
                       fifo_frame_bytes);
    d->monitor.device_buffer_bytes = drain_bytes;
    d->monitor.queued_bytes_high = MIN(3 * drain_bytes, max_high);
    d->monitor.queued_bytes_low = MIN(drain_bytes, d->monitor.queued_bytes_high);
}

void mcpx_apu_reset(MCPXAPUState *d)
{
    pthread_mutex_lock(&d->lock); // FIXME: Can fail if thread is pegged, add flag
    memset(d->regs, 0, sizeof(d->regs));

    mcpx_apu_vp_reset(d);

    // FIXME: Reset DSP state
    memset(d->gp.dsp->core.pram_opcache, 0,
           sizeof(d->gp.dsp->core.pram_opcache));
    memset(d->ep.dsp->core.pram_opcache, 0,
           sizeof(d->ep.dsp->core.pram_opcache));
    apu_st(&d->set_irq, false);
    d->snapshot_just_loaded = false;
    d->next_frame_time_us = 0;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

/* After a savestate's fields are in (d->lock held since the reset before
 * them). */
void mcpx_apu_post_load(MCPXAPUState *d)
{
    /* The tables' pointers are not part of the state; set them from the
     * loaded base registers. */
    mcpx_apu_vp_refresh_voice_cache(d, d->regs[NV_PAPU_VPVADDR]);
    mcpx_apu_vp_refresh_ssl_cache(d, d->regs[NV_PAPU_VPSSLADDR]);
    mcpx_apu_vp_restore_hrtf(d);

    /* Snapshot restore re-notify guard.
     *
     * If the save was taken with a voice AT completion (CBO>=EBO / SSL end /
     * voice-off), the frame thread re-detects that completion on its very
     * first se_frame and re-emits the voice's end/loop notification + FE
     * interrupt within microseconds of the resume — before the guest has
     * resumed far enough to free + re-register that voice's callback. The
     * guest then dispatches the already-serviced-and-freed callback object
     * -> wild indirect CALL -> Xbox BugCheck 0xA.
     *
     * Clear the stale notify-interrupt bits, deassert the line, and arm a
     * one-shot guard so the frame thread drops exactly the FIRST post-restore
     * IRQ assert. This only DELAYS the notification by one frame: the next
     * se_frame re-detects the still-completed voice and delivers it after the
     * guest has settled. */
    apu_and(&d->regs[NV_PAPU_ISTS],
            ~(NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS));
    apu_st(&d->set_irq, false);
    d->snapshot_just_loaded = true;
    update_irq(d);
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = arg;
    int64_t off_since_us = 0;   /* the frames stopped, VOICE_OFFs waiting: since */

    /* Paused (a savestate, the activity): parked between frames, the lock
     * free for the state to be taken or replaced. Running on, it wrote guest
     * RAM after a save had taken RAM and before it took the APU. */
    nc_pause_register("apu");
    pthread_mutex_lock(&d->lock);
    while (!apu_ld(&d->exiting)) {
        if (nc_paused()) {
            pthread_mutex_unlock(&d->lock);
            nc_pause_wait();
            pthread_mutex_lock(&d->lock);
            continue;
        }
        int xcntmode = GET_MASK(apu_ld(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = apu_ld(&d->regs[NV_PAPU_FECTL]);
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            apu_st(&d->set_irq, true);
        }

        /* taken and cleared at once: one the guest thread raises meanwhile
         * stays for the next frame (a plain clear after update_irq lost it) */
        if (apu_xchg(&d->set_irq, false)) {
            if (d->snapshot_just_loaded) {
                /* Swallow exactly the FIRST post-restore IRQ assert: a voice
                 * that was at completion when the snapshot was taken re-emits
                 * its notify on the first frame, before the guest has
                 * freed/re-registered its callback. Drop just this one assert
                 * (clear the notify bits); the next se_frame re-detects the
                 * completion and delivers it once the guest has settled, so
                 * the notification is delayed — never lost. */
                apu_and(&d->regs[NV_PAPU_ISTS],
                        ~(NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS));
                d->snapshot_just_loaded = false;
            } else {
                update_irq(d);
            }
        }

        if (frames_stopped(d)) {
            nc_pause_idle(true);                /* stopped until the guest acts */
            /* timed: the guest's register writes signal without d->lock (its
             * thread never takes it), so one can come between the check and
             * the wait */
            apu_cond_timedwait_us(&d->cond, &d->lock, EP_FRAME_US);
            nc_pause_idle(false);
            /* VOICE_OFFs waiting for frames that don't come: notified after
             * their 64 frames' time, their interrupt raised at the top (the
             * guest may wait for them before it acts) */
            const int64_t now_us = apu_host_us();
            if (!mcpx_apu_vp_off_pending()) {
                off_since_us = 0;
            } else if (!off_since_us) {
                off_since_us = now_us;
            } else if (now_us - off_since_us >= OFF_WAIT_US) {
                mcpx_apu_vp_off_notify_all(d);
                off_since_us = 0;
            }
            stuck_check(d, now_us / 1000);
            continue;
        }
        off_since_us = 0;
        throttle(d);
        if (nc_paused()) {
            continue;                           /* the throttle let go for the pause: no frame */
        }
        se_frame(d);
    }
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

/* The app's audio settings: the defaults above; NC_APU_DSP=1 runs the GP and
 * EP frames to completion, NC_APU_HRTF=0 turns the 3D voices' filters off,
 * NC_APU_WORKERS=n sets the voice workers (0: by the CPU count),
 * NC_APU_TRAP_PARK=1 stops the frames while the FE is trapped, as they did
 * before (an A/B switch). */
static void config_init(void)
{
    g_apu_config.use_dsp = getenv_int_clamped("NC_APU_DSP", 0, 1, g_apu_config.use_dsp);
    g_apu_config.hrtf = getenv_int_clamped("NC_APU_HRTF", 0, 1, g_apu_config.hrtf);
    g_apu_config.num_workers = getenv_int_clamped("NC_APU_WORKERS", 0, 64, g_apu_config.num_workers);
    g_trap_park = getenv_int_clamped("NC_APU_TRAP_PARK", 0, 1, 0);
}

/* NC_TRACE_APU: every 2 s, the registers that decide whether the APU
 * interrupts and which voice lists it walks. */
static void *apu_trace_thread(void *arg)
{
    MCPXAPUState *d = arg;
    while (!apu_ld(&d->exiting)) {
        sleep(2);
        nc_log("hakuX-native-apu",
               "ien %08x ists %08x fectl %08x sectl %08x fenaddr %08x | 2d %04x/%04x 3d %04x/%04x mp %04x/%04x",
               d->regs[NV_PAPU_IEN], d->regs[NV_PAPU_ISTS], d->regs[NV_PAPU_FECTL], d->regs[NV_PAPU_SECTL],
               d->regs[NV_PAPU_FENADDR], d->regs[NV_PAPU_TVL2D], d->regs[NV_PAPU_CVL2D], d->regs[NV_PAPU_TVL3D],
               d->regs[NV_PAPU_TVL3D + 4], d->regs[NV_PAPU_TVLMP], d->regs[NV_PAPU_TVLMP + 4]);
        /* the voices on each list (2D, 3D, MP) and how many of them are
         * active: voices that never end pile up here */
        {
            static const uint32_t k_top[3] = { NV_PAPU_TVL2D, NV_PAPU_TVL3D, NV_PAPU_TVLMP };
            unsigned n[3] = { 0 }, act[3] = { 0 };
            for (unsigned l = 0; l < 3; l++) {
                uint32_t h = d->regs[k_top[l]] & 0xFFFF;
                while (h != 0xFFFF && n[l] < MCPX_HW_MAX_VOICES) {
                    const uint8_t *vp = apu_ram_ptr(d->regs[NV_PAPU_VPVADDR] + h * NV_PAVS_SIZE, NV_PAVS_SIZE);
                    if (!vp) {
                        break;
                    }
                    n[l]++;
                    act[l] += (apu_rd32(vp + NV_PAVS_VOICE_PAR_STATE) & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0;
                    h = apu_rd32(vp + NV_PAVS_VOICE_TAR_PITCH_LINK) & NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE;
                }
            }
            nc_log("hakuX-native-apu", "voices on the lists: 2d %u (%u active) 3d %u (%u active) mp %u (%u active)",
                   n[0], act[0], n[1], act[1], n[2], act[2]);
        }
        /* The 2D list's head voice, whole (NV_PAVS_*: 0x80 bytes). */
        const uint32_t v = d->regs[NV_PAPU_TVL2D];
        const uint32_t pa = d->regs[NV_PAPU_VPVADDR] + v * NV_PAVS_SIZE;
        const uint8_t *p = apu_ram_ptr(pa, NV_PAVS_SIZE);
        if (v != 0xFFFF && p) {
            char line[400];
            int n = snprintf(line, sizeof(line), "voice %02x:", v);
            for (unsigned i = 0; i < NV_PAVS_SIZE; i += 4) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", apu_rd32(p + i));
            }
            nc_log("hakuX-native-apu", "%s", line);
        }
    }
    return NULL;
}

void nc_dev_apu_init(NcVm *vm)
{
    MCPXAPUState *d = nc_big_alloc(sizeof(*d)); /* 1.6 MiB */
    g_state = d;
    g_apu_ram = vm->phys;
    g_apu_ram_size = (uint32_t)vm->ram_size;
    d->ram_ptr = g_apu_ram;
    d->ram_size = g_apu_ram_size;
    config_init();

    mcpx_apu_dsp_init(d);

    apu_st(&d->set_irq, false);
    d->exiting = false;

    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->irq_lock, NULL);
    apu_cond_init(&d->cond);

    mcpx_apu_vp_init(d);
    monitor_init(d);

    const NcMmioDev dev = {"mcpx-apu", 0xFE800000u, 0x80000, apu_mmio_read, apu_mmio_write, d};
    nc_mmio_register(&dev);
    mcpx_apu_reset(d);

    apu_thread_start(&d->apu_thread, "apu.frame", mcpx_apu_frame_thread, d, true);
    g_apu_trace = getenv("NC_TRACE_APU") != NULL;
    if (g_apu_trace) {
        apu_thread_start(&g_apu_trace_thread, "apu.trace", apu_trace_thread, d, true);
    }
}

void nc_dev_apu_stop(void)
{
    MCPXAPUState *d = g_state;
    if (!d) {
        return;
    }
    pthread_mutex_lock(&d->lock);
    apu_st(&d->exiting, true);
    pthread_cond_broadcast(&d->cond);   /* a frame thread stopped until the guest acts */
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->apu_thread, NULL);
    if (g_apu_trace) {
        pthread_join(g_apu_trace_thread, NULL);     /* within its 2 s */
        g_apu_trace = false;
    }
    mcpx_apu_vp_finalize(d);            /* the voice workers */
    apu_out_close();
}

void nc_dev_apu_free(void)
{
    MCPXAPUState *d = g_state;
    if (!d) {
        return;
    }
    dsp_destroy(d->gp.dsp);
    dsp_destroy(d->ep.dsp);
    pthread_mutex_destroy(&d->lock);
    pthread_mutex_destroy(&d->irq_lock);
    pthread_cond_destroy(&d->cond);
    nc_big_free(d, sizeof(*d));
    g_state = NULL;
    g_apu_ram = NULL;
    g_apu_ram_size = 0;
    g_apu_fifo_min = INT_MAX;
    g_apu_fifo_max = 0;
}

void nc_dev_apu_spin_wait(const uint32_t *word)
{
    mcpx_apu_spin_wait(); /* GP frames run to completion meanwhile */
#if defined(__aarch64__)
    /* WFE on the word: the APU thread's store clears the exclusive monitor
     * (the event stream bounds each wait otherwise). */
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
    for (;;) {
        uint32_t v;
        __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(word) : "memory");
        if (!v) {
            break;
        }
        __asm__ volatile("wfe" ::: "memory");
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
        if (t - t0 > f / 1000) {
            break;
        }
    }
#else
    for (int i = 0; i < 1000 && __atomic_load_n(word, __ATOMIC_ACQUIRE); i++) {
        usleep(1);
    }
#endif
}
