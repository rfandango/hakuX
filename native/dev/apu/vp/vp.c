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

#include "native/dev/apu/apu_int.h"
#include "adpcm.h"
#include "mix_inline.h"
#include "native/core/mmio.h"   /* nc_mmio_retry */

/* -------------------------------------------------------------------------
 * PERF_LOG-gated "silent-but-active voice" probe.
 *
 * Measures how much per-voice decode/resample/SVF/mix work voice_process()
 * spends on voices that are ACTIVE (fully processed) but MATHEMATICALLY
 * SILENT: envelope amplitude ea_value == 0 AND all 8 mixbin volumes fully
 * attenuated (vol & 0xFFF == 0xFFF, i.e. attenuate() == 0 exactly). The
 * per-bin mix contribution is ea_value * attenuate(vol[b]), so both-zero
 * implies the voice emits silence this frame.
 *
 * This probe changes NO control flow and skips NO work — it only reads
 * already-computed locals (ea_value, vol[]) and increments relaxed-atomic
 * counters. With PERF_LOG=0 the entire block compiles to nothing, so the
 * release path is byte-identical to upstream.
 *
 * The counters race across concurrent voice_worker_thread workers and the
 * SE/EP-thread emit; relaxed atomics make that benign for a coarse ratio.
 * ------------------------------------------------------------------------- */
#ifndef PERF_LOG
#define PERF_LOG 0
#endif

/* Skip per-voice DECODE + SVF/HRTF/mix for mathematically-silent voices.
 * Defaulted here so non-CMake / host-test builds still compile; the CMake
 * HAKUX_DEFS block sets the shipping value (default 0). When 0, every guarded
 * block below is a compile-time no-op -> byte-identical to today. */
#ifndef HAKUX_AUDIO_SKIP_MUTED
#define HAKUX_AUDIO_SKIP_MUTED 0
#endif

#if PERF_LOG

#include <stdatomic.h>
#define VP_SILENT_LOG(...) nc_log("hakuX-vp", __VA_ARGS__)

static struct {
    atomic_uint_least64_t calls;          /* active calls reaching the eval point */
    atomic_uint_least64_t silent_calls;   /* of those, ea0 AND all-vol-max */
    atomic_uint_least64_t total_samples;  /* 32 per active call */
    atomic_uint_least64_t silent_samples; /* 32 per silent call */
    atomic_uint_least64_t ea0_calls;      /* sub-counter: ea_value==0 alone */
    atomic_uint_least64_t allvol_calls;   /* sub-counter: all 8 vol==0xFFF alone */
    atomic_uint_least64_t skipped_voices; /* HAKUX_AUDIO_SKIP_MUTED: voices that
                                           * took the decode+SVF/HRTF/mix skip.
                                           * 0 when the flag is off; ~=allvolmax
                                           * when on (confirms the gate fires). */
} g_vp_silent_stats;

#define VP_ADD(field, n) \
    atomic_fetch_add_explicit(&g_vp_silent_stats.field, (n), memory_order_relaxed)

void vp_silent_stats_log_and_reset(void)
{
    static int vp_silent_frame = 0;
    /* Throttle on the SE/EP-thread emit cadence (~once / few seconds). */
    if (++vp_silent_frame % 240 == 0) {
        uint64_t calls   = atomic_load_explicit(&g_vp_silent_stats.calls,
                                                memory_order_relaxed);
        uint64_t silent  = atomic_load_explicit(&g_vp_silent_stats.silent_calls,
                                                memory_order_relaxed);
        uint64_t tot_smp = atomic_load_explicit(&g_vp_silent_stats.total_samples,
                                                memory_order_relaxed);
        uint64_t sil_smp = atomic_load_explicit(&g_vp_silent_stats.silent_samples,
                                                memory_order_relaxed);
        uint64_t ea0     = atomic_load_explicit(&g_vp_silent_stats.ea0_calls,
                                                memory_order_relaxed);
        uint64_t allvol  = atomic_load_explicit(&g_vp_silent_stats.allvol_calls,
                                                memory_order_relaxed);
        uint64_t skipped = atomic_load_explicit(&g_vp_silent_stats.skipped_voices,
                                                memory_order_relaxed);
        int frac      = calls   ? (int)((silent  * 100) / calls)   : 0;
        int smp_frac  = tot_smp ? (int)((sil_smp * 100) / tot_smp) : 0;
        VP_SILENT_LOG("calls:%llu silent:%llu tot_smp:%llu silent_smp:%llu "
                      "frac:%d%% smp_frac:%d%% ea0:%llu allvolmax:%llu "
                      "skipped:%llu",
                      (unsigned long long)calls,
                      (unsigned long long)silent,
                      (unsigned long long)tot_smp,
                      (unsigned long long)sil_smp,
                      frac, smp_frac,
                      (unsigned long long)ea0,
                      (unsigned long long)allvol,
                      (unsigned long long)skipped);
        memset(&g_vp_silent_stats, 0, sizeof(g_vp_silent_stats));
    }
}

#else /* !PERF_LOG */
void vp_silent_stats_log_and_reset(void) {}
#endif /* PERF_LOG */

static const struct {
    uint64_t top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, // 2D
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, // 3D
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, // MP
};

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    uint64_t notify_offset = d->regs[NV_PAPU_FENADDR];
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15; // Final byte is status, same for all notifiers

    // FIXME: Check notify enable
    // FIXME: Set NV1BA0_NOTIFICATION_STATUS_IN_PROGRESS when appropriate
    apu_ram_st8(notify_offset, status);

    // FIXME: Refactor this out of here
    // FIXME: Actually provied current envelope state
    apu_ram_st8(notify_offset - 1, 1);

    apu_or(&d->regs[NV_PAPU_ISTS],
              NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    apu_st(&d->set_irq, true);
}

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
}

static float clampf(float v, float min, float max)
{
    if (v < min) {
        return min;
    } else if (v > max) {
        return max;
    } else {
        return v;
    }
}

/* attenuate()'s values for the 4096 volumes, computed once (the same
 * expression: a voice asks for 16 a frame) */
static float g_attenuation[0x1000];

static void attenuation_init(void)
{
    for (int vol = 0; vol < 0x1000; vol++) {
        g_attenuation[vol] = (vol == 0xFFF) ? 0.0 : powf(10.0f, vol/(64.0 * -20.0f));
    }
}

static float attenuate(uint16_t vol)
{
    return g_attenuation[vol & 0xFFF];
}

/* A table's pointer from the guest's base: NULL with no base yet or without
 * room for all of it (the accesses then go by guest address, each checked).
 * The old pointer stays valid for a worker still using it: guest RAM is one
 * mapping for the run. */
static void refresh_table(uint8_t **table, uint32_t base, uint32_t size)
{
    apu_st_rel(table, base ? apu_ram_ptr(base, size) : NULL);
}

void mcpx_apu_vp_refresh_voice_cache(MCPXAPUState *d, uint32_t new_base)
{
    refresh_table(&d->vp.voice_cache, new_base, MCPX_HW_MAX_VOICES * NV_PAVS_SIZE);
}

void mcpx_apu_vp_refresh_ssl_cache(MCPXAPUState *d, uint32_t new_base)
{
    refresh_table(&d->vp.ssl_cache, new_base, MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE);
}

/* The table's pointer covers its MCPX_HW_MAX_VOICES voices; a handle past
 * them (the guest's: a 16-bit field) goes by guest address, checked. */
static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               uint64_t offset, uint32_t mask)
{
    uint32_t raw;
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        raw = apu_mem_ld32(c, voice_handle * NV_PAVS_SIZE + offset);
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        raw = apu_ram_ld32(voice + offset);
    }
    return (raw & mask) >> ctz32(mask);
}

static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           uint64_t offset, uint32_t mask, uint32_t val)
{
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        uint64_t coff = voice_handle * NV_PAVS_SIZE + offset;
        uint32_t v = apu_mem_ld32(c, coff) & ~mask;
        apu_mem_st32(c, coff,
                           v | ((val << ctz32(mask)) & mask));
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        uint32_t v = apu_ram_ld32(voice + offset) & ~mask;
        apu_ram_st32(voice + offset,
                    v | ((val << ctz32(mask)) & mask));
    }
}

/*
 * _nolock variants: the voice table `c` loaded once by the caller (NULL is
 * legal; the fallback path handles it), along the hot voice-frame path
 * (mcpx_apu_vp_frame / voice_worker_thread / voice_process). Cold MMIO
 * callers use voice_get_mask / voice_set_mask, which load it per call.
 */
static inline uint32_t voice_get_mask_nolock(MCPXAPUState *d, uint8_t *c,
                                             uint16_t voice_handle,
                                             uint64_t offset, uint32_t mask)
{
    uint32_t raw;
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        raw = apu_mem_ld32(c, voice_handle * NV_PAVS_SIZE + offset);
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        raw = apu_ram_ld32(voice + offset);
    }
    return (raw & mask) >> ctz32(mask);
}

#if HAKUX_AUDIO_SKIP_MUTED
/* True iff this voice contributes ZERO to every mixbin THIS frame, i.e. all 8
 * mixbin volumes are 0xFFF so attenuate(vol[i])==0 for every bin (see
 * attenuate(): returns exactly 0.0 iff vol&0xFFF==0xFFF). This is the hoisted
 * form of the vol[] reads at the bottom of voice_process (the VOLA/VOLB/VOLC
 * packing, including the split vol[6]/vol[7] nibble assembly): it MUST stay
 * bit-identical to those reads. Read here BEFORE the resample block so the
 * decode / SVF / HRTF / mix can be skipped. A divergence either drops an
 * audible voice (dangerous) or fails to skip a silent one (benign no-win). */
static inline bool voice_all_vols_maxed(MCPXAPUState *d, uint8_t *c,
                                        uint16_t v)
{
    uint16_t vol[8];
    vol[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME5);

    vol[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        if ((vol[i] & 0xFFF) != 0xFFF) { /* attenuate(vol[i]) != 0 -> audible */
            return false;
        }
    }
    return true;
}
#endif /* HAKUX_AUDIO_SKIP_MUTED */

static inline void voice_set_mask_nolock(MCPXAPUState *d, uint8_t *c,
                                         uint16_t voice_handle, uint64_t offset,
                                         uint32_t mask, uint32_t val)
{
    if (likely(c && voice_handle < MCPX_HW_MAX_VOICES)) {
        uint64_t coff = voice_handle * NV_PAVS_SIZE + offset;
        uint32_t v = apu_mem_ld32(c, coff) & ~mask;
        v |= (val << ctz32(mask)) & mask;
        apu_wr32(c + coff, v);                  /* reported with the batch */
        d->vp.voice_wrote[voice_handle] = 1;
    } else {
        uint64_t voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
        uint32_t v = apu_ram_ld32(voice + offset) & ~mask;
        apu_ram_st32(voice + offset,
                    v | ((val << ctz32(mask)) & mask));
    }
}

/* The voices the batch wrote, to the HLE's write tracking: one report per
 * run of neighbouring voices (the frame thread, after the workers). */
static void report_written_voices(MCPXAPUState *d, uint8_t *c)
{
    uint8_t *w = d->vp.voice_wrote;
    const uint32_t base = (uint32_t)(c - g_apu_ram);
    for (int v = 0; v < MCPX_HW_MAX_VOICES;) {
        uint64_t word;
        if (!(v & 7)) {
            memcpy(&word, w + v, 8);
            if (!word) {
                v += 8;
                continue;
            }
        }
        if (!w[v]) {
            v++;
            continue;
        }
        int e = v;
        while (e < MCPX_HW_MAX_VOICES && w[e]) {
            w[e++] = 0;
        }
        hle_host_mem_set_dirty(base + v * NV_PAVS_SIZE, (e - v) * NV_PAVS_SIZE);
        v = e;
    }
}

/* The guest's VOICE_OFFs whose notification waits for the voice's release
 * (voice_off): a bit per voice, its notifier and the frames it has waited.
 * Not part of a savestate (cleared by the reset before one loads). */
static uint64_t g_off_pending[MCPX_HW_MAX_VOICES / 64];
static uint8_t g_off_notifier[MCPX_HW_MAX_VOICES];
static uint8_t g_off_frames[MCPX_HW_MAX_VOICES];
#define OFF_NOTIFY_MAX_FRAMES 64        /* ~43 ms of frames: then it goes anyway */

static void off_notify_now(MCPXAPUState *d, uint16_t v)
{
    const uint64_t bit = 1ull << (v % 64);
    if (__atomic_fetch_and(&g_off_pending[v / 64], ~bit, __ATOMIC_ACQ_REL) & bit) {
        set_notify_status(d, v, g_off_notifier[v], NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
    }
}

/* The frame thread stopped for the guest (the FE trapped or halted, the SE
 * off) and the guest didn't act: the VOICE_OFFs still waiting are notified,
 * no frame coming to walk the lists or count their 64. True when one was. */
bool mcpx_apu_vp_off_notify_all(MCPXAPUState *d)
{
    bool any = false;
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        for (uint64_t p = __atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE); p; p &= p - 1) {
            off_notify_now(d, (uint16_t)(w * 64 + __builtin_ctzll(p)));
            any = true;
        }
    }
    return any;
}

/* The idle voice the SE reported last (SE2FE_IDLE_VOICE, idle_report),
 * 0xFFFF: none. Not part of a savestate (cleared by the reset before one
 * loads). */
static uint16_t g_idle_last = 0xFFFF;

bool mcpx_apu_vp_off_pending(void)
{
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        if (__atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

/* The guest turns a voice off (NV1BA0_PIO_VOICE_OFF). On the hardware the
 * SE finds it idle in its next pass and reports both together: the
 * SE2FE_IDLE_VOICE trap, whose handler releases the voice, and its done
 * notification. Posted here at once, the notification's interrupt ran
 * DirectSound's voice service DPC up to a frame (and the frame thread's
 * throttle) before the trap: a stream voice it restarted there waited for a
 * release the ISR defers while that DPC runs (Wreckless hung in menus and
 * in missions, its DPC spinning at 0x12CD7B). When the title takes idle
 * traps, the notification waits for the voice's idle trap and comes with it,
 * as on the hardware (mcpx_apu_vp_frame), or until the voice has left the
 * lists, is turned on again, or 64 frames (or their time, the frame thread
 * stopped for the guest). Waiting past the trap, until the trap's DPC had
 * released the voice, Blood Wake hung loading a mission: its DirectSound
 * turns a voice off at DISPATCH_LEVEL and spins at 0xFE047 until its ISR
 * has seen the notification, the DPC kept out, the trap stopping the frames
 * that would have counted the 64. */
static void voice_off(MCPXAPUState *d, uint16_t v)
{
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
        off_notify_now(d, v);           /* an earlier one first */
        g_off_notifier[v] = (uint8_t)notifier;
        g_off_frames[v] = 0;
        __atomic_fetch_or(&g_off_pending[v / 64], 1ull << (v % 64), __ATOMIC_ACQ_REL);
        return;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* _nolock variant of voice_off — the caller has the voice table. Used from voice_step_envelope /
 * voice_get_samples on the worker hot path. */
static void voice_off_nolock(MCPXAPUState *d, uint8_t *c, uint16_t v)
{
    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

/* vp_write: the guest CPU's register write is being applied */
static __thread bool t_guest_write;

/* A voice's spinlock. The VP's batch holds it while the voice workers
 * process the frame: 0.2 ms on average, but now and then 4-40 ms (a worker
 * preempted). The guest's DirectSound waited for it in a register write,
 * inside one MMIO access, its interrupts held off all that time: Forza's
 * vblank came 20-30 ms late and its frame a vblank late, a few times a
 * minute in a race. In the native core such a write gives up after 0.2 ms
 * and the guest's store runs again (nc_mmio_retry): it waits as long, but
 * from instruction to instruction, the interrupts taken in between. Only
 * where nothing was changed yet (may_retry), and where the access can run
 * again (an MMIO site's; a faulting access waits here as before). */
static bool voice_spin_lock(MCPXAPUState *d, uint16_t v, bool may_retry)
{
    if (may_retry && t_guest_write) {
        if (apu_spin_trylock(&d->vp.voice_spinlocks[v])) {
            return true;
        }
        const int64_t t0 = apu_host_us();
        for (unsigned i = 1;; i++) {
#if defined(__aarch64__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
            if (!__atomic_load_n(&d->vp.voice_spinlocks[v].v, __ATOMIC_RELAXED) &&
                apu_spin_trylock(&d->vp.voice_spinlocks[v])) {
                return true;
            }
            if (!(i & 63) && apu_host_us() - t0 >= 200) {
                if (nc_mmio_retry()) {
                    return false;
                }
                break;
            }
        }
    }
    apu_spin_lock(&d->vp.voice_spinlocks[v]);
    return true;
}

/* false: not taken, the guest's write runs again (voice_spin_lock); a
 * handle past the voices is no voice: the write is dropped (done) */
static bool voice_lock(MCPXAPUState *d, uint16_t v, bool lock, bool may_retry)
{
    if (!APU_CHECK(v < MCPX_HW_MAX_VOICES)) {
        return true;
    }
    if (!voice_spin_lock(d, v, may_retry)) {
        return false;
    }
    uint64_t mask = 1LL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }
    apu_spin_unlock(&d->vp.voice_spinlocks[v]);
    return true;
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    if (v >= MCPX_HW_MAX_VOICES) {
        return false;
    }
    uint64_t mask = 1LL << (v % 64);
    return (apu_ld(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;


    //assert((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == 0);

    /* The decoded method and argument are a trap's while the FE is trapped:
     * its handler reads them, and the hardware FE takes no other method
     * until the handler clears the trap. Overwritten by the guest's next
     * write (VOICE_LOCK 1 between the VP's IDLE_VOICE trap and its
     * handler), PGR2's DirectSound read the trap's method with that
     * argument, a voice handle of 1 it had no voice for, and dereferenced a
     * NULL voice (guest address 0x64: the process died). */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        d->regs[NV_PAPU_FEDECMETH] = method;
        d->regs[NV_PAPU_FEDECPARAM] = argument;
    }
    unsigned int selected_handle, list;
    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, d->regs[NV_PAPU_FECV], argument & 1, true);
        break;
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;
    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        DPRINTF("VOICE %d ON\n", selected_handle);
        if (!APU_CHECK(selected_handle < MCPX_HW_MAX_VOICES)) {
            break;
        }

        off_notify_now(d, selected_handle);    /* its last VOICE_OFF's, before it plays again */

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            /* voice is added to the top of the selected list */
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            /* voice is added after the antecedent voice */
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        // FIXME: Should set CBO here?
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0; // FIXME: verify this
        d->vp.ssl[selected_handle].ssl_index = 0; // FIXME: verify this

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENV0,
                               NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENVA,
                               NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }
        // FIXME: Will count be overwritten in other cases too?

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENV1,
                               NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_ENVF,
                               NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }
        // FIXME: Will count be overwritten in other cases too?

        voice_reset_filters(d, selected_handle);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);
        break;
    }
    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        if (!APU_CHECK(selected_handle < MCPX_HW_MAX_VOICES)) {
            break;
        }

        // FIXME: What if already in release? Restart envelope?
        // FIXME: Should release count ascend or descend?

        uint16_t rr;
        rr = voice_get_mask(d, selected_handle, NV_PAVS_VOICE_TAR_LFO_ENV,
                            NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = voice_get_mask(d, selected_handle, NV_PAVS_VOICE_CFG_MISC,
                            NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
        break;
    }
    case NV1BA0_PIO_VOICE_OFF:
        if (APU_CHECK((argument & NV1BA0_PIO_VOICE_OFF_HANDLE) < MCPX_HW_MAX_VOICES)) {
            voice_off(d, argument & NV1BA0_PIO_VOICE_OFF_HANDLE);
        }
        break;
    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE,
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;
    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        d->vp.hrtf.current_entry = handle;
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_VBIN,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_FMT,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENV0,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENVA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENV1,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_ENVF,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CFG_MISC,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        unsigned int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, current_voice, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE &&
            APU_CHECK(handle < HRTF_ENTRY_COUNT)) {
            // FIXME: Xbox software seems to reliably set voice HRTF handles
            // after updating filter parameters, however it may be possible to
            // update parameter targets for an active voice.
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        break;
    }
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLB,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_VOLC,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_LFO_ENV,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_FCA,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_FCB,
                       0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, d->regs[NV_PAPU_FECV], NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;
    case NV1BA0_PIO_SET_HRIR ... NV1BA0_PIO_SET_HRIR_X - 1: {
        if (!APU_CHECK((unsigned)d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT)) {
            break;
        }
        slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
        int8_t left0 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
        int8_t right0 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
        int8_t left1 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
        int8_t right1 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
        int coeff_idx = slot * 2;
        set_hrir_coeff_tar(d, 0, coeff_idx, left0);
        set_hrir_coeff_tar(d, 1, coeff_idx, right0);
        coeff_idx += 1;
        set_hrir_coeff_tar(d, 0, coeff_idx, left1);
        set_hrir_coeff_tar(d, 1, coeff_idx, right1);
        break;
    }
    case NV1BA0_PIO_SET_HRIR_X: {
        if (!APU_CHECK((unsigned)d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT)) {
            break;
        }
        int8_t left30 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
        int8_t right30 = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
        int16_t itd = GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
        set_hrir_coeff_tar(d, 0, 30, left30);
        set_hrir_coeff_tar(d, 1, 30, right30);
        d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        // FIXME: Is there an upper limit for the SGE table size?
        // FIXME: NV_PAPU_VPSGEADDR is probably bad, as outbuf SGE use the same
        // handle range (or that is also wrong)
        uint64_t sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        apu_ram_st32(sge_address,
                    argument &
                        NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        DPRINTF("Wrote inbuf SGE[0x%X] = 0x%08X\n", d->vp.inbuf_sge_handle,
                argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
#ifdef DEBUG_MCPX
        slot = (method - NV1BA0_PIO_SET_OUTBUF_BA) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_BA_ADDRESS = 0x007FFF00 ?
        DPRINTF("outbuf_ba[%d]: 0x%08X\n", slot, argument);
#endif
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
#ifdef DEBUG_MCPX
        slot = (method - NV1BA0_PIO_SET_OUTBUF_LEN) / 8;
        //FIXME: Use NV1BA0_PIO_SET_OUTBUF_LEN_VALUE = 0x007FFF00 ?
        DPRINTF("outbuf_len[%d]: 0x%08X\n", slot, argument);
#endif
        //assert(false); //FIXME: Enable assert! no idea what this reg does
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        // FIXME: Is there an upper limit for the SGE table size?
        // FIXME: NV_PAPU_VPSGEADDR is probably bad, as inbuf SGE use the same
        // handle range (or that is also wrong)
        // NV_PAPU_EPFADDR   EP outbufs
        // NV_PAPU_GPFADDR   GP outbufs
        // But how does it know which outbuf is being written?!
        uint64_t sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        apu_ram_st32(sge_address,
                    argument &
                        NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        DPRINTF("Wrote outbuf SGE[0x%X] = 0x%08X\n", d->vp.outbuf_sge_handle,
                argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        if (!APU_CHECK(current_voice < MCPX_HW_MAX_VOICES)) {
            break;
        }
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        // d->vp.ssl[current_voice].ssl_index = 0;
        DPRINTF("SSL%c Base = %x, Count = %d\n", 'A' + ssl,
                d->vp.ssl[current_voice].base[ssl],
                d->vp.ssl[current_voice].count[ssl]);
        break;
    }
    // FIXME: Refactor into above
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        uint32_t current_voice = d->regs[NV_PAPU_FECV];
        if (!APU_CHECK(current_voice < MCPX_HW_MAX_VOICES)) {
            break;
        }
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        // d->vp.ssl[current_voice].ssl_index = 0;
        DPRINTF("SSL%c Base = %x, Count = %d\n", 'A' + ssl,
                d->vp.ssl[current_voice].base[ssl],
                d->vp.ssl[current_voice].count[ssl]);
        break;
    }
    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS*NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }
    case NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET ...
         NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH+8*64-1: {
        // 64 offset/base pairs relative to segment base
        // FIXME: Entries are 64b, assuming they are stored
        // like this <[offset,length],...>
        assert((method & 0x3) == 0);
        uint64_t addr = d->regs[NV_PAPU_VPSSLADDR]
                      + (d->vp.ssl_base_page * 8)
                      + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
        apu_ram_st32(addr, argument);
        DPRINTF("  ssl_segment[%x + %x].%s = %x\n",
            d->vp.ssl_base_page,
            (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET)/8,
            method & 4 ? "length" : "offset",
            argument);
        break;
    }
    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (argument >>  0) & 0x1f;
        d->vp.hrtf_submix[1] = (argument >>  8) & 0x1f;
        d->vp.hrtf_submix[2] = (argument >> 16) & 0x1f;
        d->vp.hrtf_submix[3] = (argument >> 24) & 0x1f;
        break;
    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT;
        break;
    case NV1BA0_PIO_SET_SUBMIX_HEADROOM ...
         NV1BA0_PIO_SET_SUBMIX_HEADROOM+4*(NUM_MIXBINS-1):
        assert((method & 3) == 0);
        slot = (method-NV1BA0_PIO_SET_SUBMIX_HEADROOM)/4;
        d->vp.submix_headroom[slot] =
            argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT;
        break;
    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            DPRINTF("idle voice %d\n", argument);
            apu_st(&d->set_irq, true);
        } else {
            DPRINTF("ignoring idle voice %d without FETFORCE1 request\n",
                    argument);
        }
        break;
    default:
        DPRINTF("fe_method: unknown method 0x%x\n", method);
        break;
    }
}

/* The voice whose state a guest method changes that the VP's frame changes
 * too (the envelope's state and counts, the play offset), 0xFFFF for none:
 * the other methods set fields only the guest writes. */
static uint16_t method_voice(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    switch (method) {
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_RELEASE:
        return argument & NV1BA0_PIO_VOICE_ON_HANDLE;
    case NV1BA0_PIO_VOICE_OFF:
        return argument & NV1BA0_PIO_VOICE_OFF_HANDLE;
    case NV1BA0_PIO_VOICE_PAUSE:
        return argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        return (uint16_t)d->regs[NV_PAPU_FECV];
    default:
        return 0xFFFF;
    }
}

uint64_t mcpx_apu_vp_read(void *opaque, uint32_t addr, unsigned int size)
{
    DPRINTF("mcpx apu VP: read [0x%" PRIx64 "] (%s)\n", addr,
            get_method_str(addr));

    switch (addr) {
    case NV1BA0_PIO_FREE:
        /* we don't simulate the queue for now,
         * pretend to always be empty */
        return 0x80;
    default:
        break;
    }

    return 0;
}

void mcpx_apu_vp_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    DPRINTF("mcpx apu VP: [0x%" PRIx64 "] %s = 0x%lx\n", addr,
            get_method_str(addr), val);

    switch (addr) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
    case NV1BA0_PIO_VOICE_LOCK:
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_RELEASE:
    case NV1BA0_PIO_VOICE_OFF:
    case NV1BA0_PIO_VOICE_PAUSE:
    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
    case NV1BA0_PIO_SET_CURRENT_VOICE:
    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
    case NV1BA0_PIO_SET_VOICE_TAR_HRTF:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
    case NV1BA0_PIO_SET_HRIR ... NV1BA0_PIO_SET_HRIR_X - 1:
    case NV1BA0_PIO_SET_HRIR_X:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET:
    CASE_4(NV1BA0_PIO_SET_OUTBUF_BA, 8): // 8 byte pitch, 4 entries
    CASE_4(NV1BA0_PIO_SET_OUTBUF_LEN, 8): // 8 byte pitch, 4 entries
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET:
    case NV1BA0_PIO_SET_CURRENT_SSL:
    case NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET ...
         NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH+8*64-1:
    case NV1BA0_PIO_SET_VOICE_SSL_A:
    case NV1BA0_PIO_SET_VOICE_SSL_B:
    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
    case NV1BA0_PIO_SET_HRTF_HEADROOM:
    case NV1BA0_PIO_SET_SUBMIX_HEADROOM ...
         NV1BA0_PIO_SET_SUBMIX_HEADROOM+4*(NUM_MIXBINS-1):
        /* TODO: these should instead be queueing up fe commands */
        apu_spin_lock(&d->vp.fe_spin);     /* against the VP's traps */
        if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) ==
            NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
            /* the trap's method and argument stay for its handler (fe_method) */
            static unsigned s_kept;
            if (s_kept++ < 16) {
                nc_log("hakuX-apu", "method %04x (%08x) while trapped: the trap's %04x (%08x) kept",
                                    (unsigned)addr, (unsigned)val, d->regs[NV_PAPU_FEDECMETH],
                                    d->regs[NV_PAPU_FEDECPARAM]);
            }
        }
        t_guest_write = true;
        /* A voice whose state the frame changes too is the guest's while the
         * method runs, as its own VOICE_LOCK makes it: the frame leaves it
         * for the next (voice_take), and a batch processing it is waited for
         * first, the store run again if that takes long (voice_spin_lock).
         * Unguarded, the guest's VOICE_OFF and a worker's envelope update
         * read-modify-wrote the same word and one store was lost: a voice
         * turned off played on, never reported idle, its release waited for
         * for good. */
        const uint16_t v = method_voice(d, addr, val);
        const bool guard = v < MCPX_HW_MAX_VOICES && !is_voice_locked(d, v);
        if (!guard || voice_lock(d, v, true, true)) {
            if (guard) {
                /* the voice held before the method's stores (VOICE_ON's list
                 * insert) are seen: the unlock's release orders only the
                 * stores before it */
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
            }
            fe_method(d, addr, val);
            if (guard) {
                voice_lock(d, v, false, false);
            }
        }
        t_guest_write = false;
        apu_spin_unlock(&d->vp.fe_spin);
        break;

    case NV1BA0_PIO_GET_VOICE_POSITION:
    case NV1BA0_PIO_SET_CONTEXT_DMA_NOTIFY:
    case NV1BA0_PIO_SET_CURRENT_SSL_CONTEXT_DMA:
        DPRINTF("unhandled method: %" PRIx64 " = %" PRIx64 "\n", addr,
                val);
        assert(0);
    default:
        break;
    }
}

/* hakuX: VP addresses are guest controlled. With AHB guest RAM an
 * out-of-range read faults instead of reading adjacent heap (Phase 0
 * finding); clamp and count. */
uint32_t g_vp_oob_clamps;

static inline uint64_t vp_ram_addr(MCPXAPUState *d, uint64_t addr, size_t n)
{
    if (addr + n <= d->ram_size) {
        return addr;
    }
    apu_inc(&g_vp_oob_clamps);
    return 0;
}

static uint64_t get_data_ptr(MCPXAPUState *d, uint64_t sge_base,
                           unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / APU_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        apu_rd32(&d->ram_ptr[vp_ram_addr(d, sge_base + entry * 8, 4)]);
    DPRINTF("Addr: 0x%08X\n", prd_address);
    return prd_address + addr % APU_PAGE_SIZE;
}

static float voice_step_envelope(MCPXAPUState *d, uint8_t *c, uint16_t v,
                           uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, 0x00); // FIXME: Confirm this?

        if (count == 0) {
            cur++;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            count = 0;
        } else {
            count--;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            voice_get_mask_nolock(d, c, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);

        float value;
        if (attack_rate == 0) {
            // FIXME: [division by zero]
            //       Got crackling sound in hardware for amplitude env.
            value = 255.0f;
        } else {
            if (count <= (attack_rate * 16)) {
                value = (count * 0xFF) / (attack_rate * 16);
            } else {
                // FIXME: Overflow in hardware
                //       The actual value seems to overflow, but not sure how
                value = 255.0f;
            }
        }
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, value);
        // FIXME: Comparison could also be the other way around?! Test please.
        if (count == (attack_rate * 16)) {
            cur++;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16; // FIXME: Skip next phase if count is 0?
                                    // [other instances too]
        } else {
            count++;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, 0xFF);

        if (count == 0) {
            cur++;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate = voice_get_mask_nolock(
                d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);

        // FIXME: Decay should return a value no less than sustain
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            // FIXME: This formula and threshold is not accurate, but I can't
            // get it any better for now
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096 / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            // FIXME: Should we still update lvl?
            cur++;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, value);
        }
        return value / 255.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            voice_get_mask_nolock(d, c, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask_nolock(
            d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask,
            0x00); // FIXME: is this only set to 0 once or forced to zero?
        voice_set_mask_nolock(d, c, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = voice_get_mask_nolock(d, c, v, rr_reg, rr_mask);

        if (release_rate == 0) {
            count = 0;
        }

        float value = 0;
        if (count == 0) {
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            // FIXME: Appears to be an exponential but unsure about actual
            // curve; performing standard decay of current level to T60 over the
            // release interval which seems about right.
            // FIXME: Based on sustain level or just decay of current level?
            // FIXME: Update level? A very similar, alternative decay function
            // (probably what the hw actually does): y(t)=2^(-10t), which would
            // permit simpler attenuation more efficiently and update level on
            // each round.
            float pos = clampf(1 - count / (release_rate * 16.0), 0, 1);
            uint8_t lvl = voice_get_mask_nolock(d, c, v, lvl_reg, lvl_mask);
            value = powf(M_E, -6.91*pos)*lvl;
            count--; // FIXME: Should release count ascend or descend?
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }

        return value / 255.0f;
    }
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off_nolock(d, c, v);
        }
        return 0.0f;
    default:
        fprintf(stderr, "Unknown envelope state 0x%x\n", cur);
        assert(false);
        return 0.0f;
    }
}

static int voice_get_samples(MCPXAPUState *d, uint8_t *c, uint32_t v,
                       float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask_nolock(
        d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 }; /* B8, B16, ADPCM, B32 */
    unsigned int container_size_index = voice_get_mask_nolock(
        d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    bool paused = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED);
    bool loop =
        voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_LOOP);
    uint32_t ebo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST);
    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    bool linked = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_LINKED); /* FIXME? */

    assert(!multipass); // Multipass is handled before this

    int ssl_index = 0;
    int ssl_seg = 0;
    int page = 0;
    int count = 0;
    int seg_len = 0;
    int seg_cs = 0;
    int seg_spb = 0;
    int seg_s = 0;
    uint64_t segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36*2/4];
    MCPXAPUVoiceFilter *const vf = &d->vp.filters[v];
    const int16_t *adpcm_decoded = vf->adpcm_pcm;
    uint32_t pcm_page = UINT32_MAX;     /* a buffer's page last looked up, its address */
    uint64_t pcm_page_base = 0;

    // This is probably cleared when the first sample is played
    // FIXME: How will this behave if CBO > EBO on first play?
    // FIXME: How will this behave if paused?
    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) {
        return -1;
    }

    if (stream) {
        if (!persist) {
            // FIXME: Confirm. Unsure if this should wait until end of SSL or
            // terminate immediately. Definitely not before end of envelope.
            int eacur = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                DPRINTF("Voice %d envelope not in release state (%d) and "
                        "persist is not set. Ending stream now!\n",
                        v, eacur);
                voice_off_nolock(d, c, v);
                return -1;
            }
        }

        DPRINTF("**** STREAMING (%d) ****\n", v);
        assert(!loop);

        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        // Check to see if the stream has ended
        if (count == 0) {
            DPRINTF("Stream has ended\n");
            voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off_nolock(d, c, v);
            } else {
                set_notify_status(
                    d, v, MCPX_HW_NOTIFIER_SSLA_DONE + d->vp.ssl[v].ssl_index,
                    NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        uint64_t ssl_offset = page * 8;
        {
            /* the table's pointer covers its MCPX_HW_MAX_SSL_PRDS entries;
             * the guest's base can be past them (by guest address, checked) */
            uint8_t *c = apu_ld_acq(&d->vp.ssl_cache);
            if (likely(c && (uint32_t)page < MCPX_HW_MAX_SSL_PRDS)) {
                segment_offset = apu_mem_ld32(c, ssl_offset);
                segment_length = apu_mem_ld32(c, ssl_offset + 4);
            } else {
                uint64_t addr = d->regs[NV_PAPU_VPSSLADDR] + ssl_offset;
                segment_offset = apu_ram_ld32(addr);
                segment_length = apu_ram_ld32(addr + 4);
            }
        }
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        assert(seg_cs == container_size_index);
        assert((seg_spb + 1) == samples_per_block);
        assert(seg_s == stereo);
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }

        assert(seg_len > 0);
        ebo = seg_len - 1; // FIXME: Confirm seg_len-1 is last valid sample index

        DPRINTF("Segment: SSL%c[%d]\n", 'A' + ssl_index, ssl_seg);
        DPRINTF("Page: %x\n", page);
        DPRINTF("Count: %d\n", count);
        DPRINTF("Segment offset: 0x%" PRIx64 "\n", segment_offset);
        DPRINTF("Segment length: %x\n", segment_length);
        DPRINTF("...len = 0x%x\n", seg_len);
        DPRINTF("...cs  = %d (%s)\n", seg_cs, container_size_str[seg_cs]);
        DPRINTF("...spb = %d\n", seg_spb);
        DPRINTF("...s   = %d (%s)\n", seg_s, seg_s ? "stereo" : "mono");
    } else {
        DPRINTF("**** BUFFER (%d) ****\n", v);
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        /* 36 bytes a channel: a block of more (the guest's samples per
         * block above its channels) fits neither adpcm_block nor the 65
         * samples a channel adpcm_decoded holds */
        if (!APU_CHECK(samples_per_block <= channels)) {
            return -1;
        }
        block_size = 36;
        DPRINTF("ADPCM:\n");
    } else {
        assert(container_size_index < 4);
        assert(sample_size < 4);
        block_size = container_size;
        DPRINTF("PCM:\n");
        DPRINTF("  Container Size: %s\n",
                container_size_str[container_size_index]);
        DPRINTF("  Sample Size: %s\n", sample_size_str[sample_size]);
    }

    DPRINTF("CBO=%d EBO=%d\n", cbo, ebo);

    block_size *= samples_per_block;

    // FIXME: Restructure this loop
    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
#if HAKUX_AUDIO_SKIP_MUTED
        /* Muted voice: skip the per-sample int->float DECODE (adpcm_decode /
         * ldXX / host-RAM reads) and write silence instead. `continue` jumps to
         * the for-header's update step, so sample_count++ and cbo++ STILL run,
         * keeping the returned sample_count -- and therefore the resampler's
         * input-consumption pattern and the CBO/SSL bookkeeping below -- bit-
         * identical to the un-gated path. Only the sample VALUES change. */
        if (d->vp.filters[v].cb_skip_decode) {
            samples[sample_count][0] = 0.0f;
            samples[sample_count][1] = 0.0f;
            continue;
        }
#endif
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != block_index) {
                uint32_t linear_addr = block_index * block_size;
                if (stream) {
                    uint64_t addr = segment_offset + linear_addr;
                    int max_seg_byte = (seg_len >> 6) * block_size;
                    assert(linear_addr + block_size <= max_seg_byte);
                    memcpy(adpcm_block,
                           &d->ram_ptr[vp_ram_addr(d, addr, block_size)],
                           block_size); // FIXME: Use idiomatic DMA function
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        uint64_t addr = get_data_ptr(d, d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            apu_rd32(&d->ram_ptr[vp_ram_addr(d, addr, 4)]);
                        linear_addr += 4;
                    }
                }
                /* the block this voice decoded last, byte for byte: its
                 * samples are still there */
                if (vf->adpcm_raw_len != block_size || vf->adpcm_channels != channels ||
                    memcmp(vf->adpcm_raw, adpcm_block, block_size)) {
                    adpcm_decode_block(vf->adpcm_pcm, (uint8_t *)adpcm_block,
                                       block_size, channels);
                    memcpy(vf->adpcm_raw, adpcm_block, block_size);
                    vf->adpcm_raw_len = block_size;
                    vf->adpcm_channels = channels;
                }
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            uint64_t addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                /* the buffer's page looked up once per page, not per sample
                 * (a read of the guest's page table each) */
                uint32_t linear_addr = ba + cbo * block_size;
                if (linear_addr / APU_PAGE_SIZE != pcm_page) {
                    pcm_page = linear_addr / APU_PAGE_SIZE;
                    pcm_page_base = get_data_ptr(d, d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                                 pcm_page * APU_PAGE_SIZE);
                }
                addr = pcm_page_base + linear_addr % APU_PAGE_SIZE;
            }

            /* Read directly from the host-mapped RAM pointer. */
            const uint8_t *src = &d->ram_ptr[vp_ram_addr(
                d, addr, (size_t)container_size * channels)];
            for (unsigned int channel = 0; channel < channels; channel++) {
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    fval = uint8_to_float(src[0]);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    fval = int16_to_float(apu_rd16(src));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    fval = int24_to_float(apu_rd32(src));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    fval = int32_to_float(apu_rd32(src));
                    break;
                default:
                    assert(false);
                    fval = 0;
                    break;
                }
                samples[sample_count][channel] = fval;
                src += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                DPRINTF("SSL%c[%d]\n", 'A' + ssl_index, d->vp.ssl[v].ssl_seg);
            } else {
                int next_index = (ssl_index + 1) % 2;
                DPRINTF("SSL%c\n", 'A' + next_index);
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off_nolock(d, c, v);
                DPRINTF("end of buffer!\n");
            }
        }
    }

    voice_set_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

static long voice_resample_callback(void *cb_data, float **data)
{
    MCPXAPUVoiceFilter *filter = cb_data;
    uint16_t v = filter->voice;
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUState *d = container_of(filter, MCPXAPUState, vp.filters[v]);
    /* The voice table voice_resample loaded. */
    uint8_t *c = filter->cb_voice_cache;

    int sample_count = 0;
    while (sample_count < NUM_SAMPLES_PER_FRAME) {
        int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        if (!active) {
            break;
        }
        int count = voice_get_samples(
            d, c, v, (float(*)[2]) & filter->resample_buf[2 * sample_count],
            NUM_SAMPLES_PER_FRAME - sample_count);
        if (count < 0) {
            break;
        }
        sample_count += count;
    }

    if (sample_count < NUM_SAMPLES_PER_FRAME) {
        /* Starvation causes SRC hang on repeated calls. Provide silence. */
        memset(&filter->resample_buf[2*sample_count], 0,
            2*(NUM_SAMPLES_PER_FRAME-sample_count)*sizeof(float));
        sample_count = NUM_SAMPLES_PER_FRAME;
    }

    *data = filter->resample_buf;
    return sample_count;
}

static int voice_resample(MCPXAPUState *d, uint8_t *c, uint16_t v,
                          float samples[][2],
                          int requested_num, float rate)
{
    assert(v < MCPX_HW_MAX_VOICES);
    MCPXAPUVoiceFilter *filter = &d->vp.filters[v];

    if (filter->resampler == NULL) {
        filter->voice = v;
        int err;

        /* Note: Using a sinc based resampler for quality. Unsure about
         * hardware's actual interpolation method; it could just be linear, in
         * which case using this resampler is overkill, but quality is good
         * so use it for now.
         */
        // FIXME: Don't do 2ch resampling if this is a mono voice
        filter->resampler = src_callback_new(&voice_resample_callback,
                                           SRC_SINC_FASTEST, 2, &err, filter);
        if (filter->resampler == NULL) {
            fprintf(stderr, "src error: %s\n", src_strerror(err));
            assert(0);
        }
    }

    /* The voice table through to the libsamplerate callback (whose
     * signature we cannot extend), for the synchronous duration of
     * src_callback_read. */
    filter->cb_voice_cache = c;
    int count = src_callback_read(filter->resampler, rate, requested_num,
                                  (float *)samples);
    filter->cb_voice_cache = NULL;
    if (count == -1) {
        DPRINTF("resample error\n");
    }
    if (count != requested_num) {
        DPRINTF("resample returned fewer than expected: %d\n", count);

        if (count == 0)
            return -1;
    }

    return count;
}

static int peek_ahead_multipass_bin(MCPXAPUState *d, uint8_t *c,
                                    uint16_t v,
                                    uint16_t *dst_voice)
{
    bool first = true;

    /* Bounded like the list walk in mcpx_apu_vp_frame: a list the title is
     * relinking can hold a cycle without a multipass voice, and the worker
     * looped on it for ever, holding the voice spinlocks of its batch while
     * the vCPU's voice_lock spun on one (Forza, 2026-10-17: hung 160 s). */
    for (int i = 0; v != 0xFFFF; i++) {
        if (i >= MCPX_HW_MAX_VOICES) {
            DPRINTF("Voice list contains a cycle!\n");
            break;
        }
        bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
        if (multipass) {
            if (first) {
                break;
            }

            *dst_voice = v;
            int mp_bin = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
            return mp_bin;
        }

        v = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        first = false;
    }

    *dst_voice = 0xFFFF;
    return -1;
}

static void get_multipass_samples(MCPXAPUState *d, uint8_t *c,
                                  float mixbins[][NUM_SAMPLES_PER_FRAME],
                                  uint16_t v, float samples[][2])
{
    // DirectSound sets bin to 31, but hardware would allow other bins
    int mp_bin = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);

    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        samples[i][0] = mixbins[mp_bin][i];
        samples[i][1] = mixbins[mp_bin][i];
    }

    // DirectSound sets clear mix to true
    bool clear_mix = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX);
    if (clear_mix) {
        memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
    }
}

static void voice_process(MCPXAPUState *d, uint8_t *c,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO);
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED);

    if (paused) {
        return;
    }

    float ef_value = voice_step_envelope(
        d, c, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    assert(ef_value >= 0.0f);
    assert(ef_value <= 1.0f);
    int16_t p = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_ENV0,
                               NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0 / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);

    float ea_value = voice_step_envelope(
        d, c, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    assert(ea_value >= 0.0f);
    assert(ea_value <= 1.0f);

#if HAKUX_AUDIO_SKIP_MUTED
    /* Hoisted silence predicate: read the 8 mixbin vols BEFORE the resample
     * block so a muted voice can skip decode (in voice_get_samples, via
     * cb_skip_decode) and SVF/HRTF/mix (early-return below). The resampler-
     * driven pull loop still runs, so CBO/SSL/SSLA_DONE/loop/voice_off advance
     * bit-exactly (the loop trip count and num_samples_requested are value-
     * independent of the decoded samples; we only null the sample VALUES). */
    bool skip_muted = voice_all_vols_maxed(d, c, v);
#endif

    float samples[NUM_SAMPLES_PER_FRAME][2] = { 0 };

    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);

#if !defined(HAKUX_AUDIO_NO_PROCESS) || !HAKUX_AUDIO_NO_PROCESS
#if HAKUX_AUDIO_SKIP_MUTED
    /* Thread the per-voice skip decision into voice_get_samples (reached via
     * voice_resample -> src_callback_read -> voice_resample_callback). Valid
     * only for the synchronous duration of the resample loop below, same
     * lifetime contract as cb_voice_cache. Multipass voices have no decode
     * loop, so this is consumed only on the non-multipass path. */
    d->vp.filters[v].cb_skip_decode = skip_muted;
#endif
    if (multipass) {
        get_multipass_samples(d, c, mixbins, v, samples);
    } else {
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) {
                return;
            }
            int count =
                voice_resample(d, c, v, &samples[sample_count],
                               NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) {
                break;
            }
            sample_count += count;
        }
    }

    int active = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) {
        return;
    }
#else
    /* HAKUX_AUDIO_NO_PROCESS=1: skip the expensive sample-production +
     * mixing path. Envelopes above still advance guest-observable
     * PAR_STATE / CUR_ECNT / level registers, and mcpx_apu_vp_frame's
     * outer voice-list walk (the v1 deadlock site) is untouched, so the
     * guest still sees voice-list progression and fe_method side
     * effects. `samples[]` is already zeroed at the declaration above,
     * and `mixbins[]` is left untouched -> downstream DSP mixes silence.
     * NOTE: this also short-circuits voice_resample / voice_get_samples,
     * which would normally advance NV_PAVS_VOICE_PAR_OFFSET_CBO and the
     * SSL segment/end-of-buffer / SSLA_DONE notifier / voice_off() path.
     * Titles that drive playback off CBO/SSL polling may stall; this is
     * an FPS-cost probe, not a shippable audio mode. (void)samples;
     * (void)multipass; (void)rate; (void)mixbins; */
    (void)samples;
    (void)multipass;
    (void)rate;
    (void)mixbins;
    return;
#endif

    int bin[8];
    bin[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                            NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                            NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME5);

    vol[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4;
    vol[6] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLC,
                            NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLB,
                             NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4;
    vol[7] |= voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_TAR_VOLA,
                             NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

#if PERF_LOG
    /* Silent-but-active voice probe (read-only; no control-flow change).
     * This call has done the full resample+SVF work and has both predicate
     * inputs live: ea_value (the EA amplitude envelope) and vol[0..7]. */
    {
        bool ea0 = (ea_value == 0.0f);
        bool allvol_max = true;
        for (int i = 0; i < 8; i++) {
            if ((vol[i] & 0xFFF) != 0xFFF) { /* attenuate(vol[i]) != 0 */
                allvol_max = false;
                break;
            }
        }
        VP_ADD(calls, 1);
        VP_ADD(total_samples, NUM_SAMPLES_PER_FRAME);
        if (ea0) {
            VP_ADD(ea0_calls, 1);
        }
        if (allvol_max) {
            VP_ADD(allvol_calls, 1);
        }
        if (ea0 && allvol_max) {
            VP_ADD(silent_calls, 1);
            VP_ADD(silent_samples, NUM_SAMPLES_PER_FRAME);
        }
    }
#endif

    // FIXME: If phase negations means to flip the signal upside down
    //        we should modify volume of bin6 and bin7 here.

#if HAKUX_AUDIO_SKIP_MUTED
    /* Muted voice: the resample loop above already advanced CBO/SSL/SSLA_DONE/
     * loop/voice_off bit-exactly. The remaining SVF / HRTF / 8x mix all
     * accumulate scaled by attenuate(vol[b])==0, so they contribute ZERO --
     * skip them. Remember the mute so SVF/HRTF IIR state is reset-to-rest on
     * the next audible frame (filters froze mid-decay; resuming from a frozen-
     * but-stale state could click). This early-return is reached for BOTH the
     * multipass and non-multipass branches (it is after the if/else above). */
    if (skip_muted) {
#if PERF_LOG
        VP_ADD(skipped_voices, 1);
#endif
        d->vp.filters[v].muted_prev = true;
        return;
    }
#endif

#if HAKUX_AUDIO_SKIP_MUTED
    /* Audible this frame (skip_muted was false -> we did not early-return). If
     * this voice was muted last audible frame, its SVF integrators and HRTF
     * ring were frozen mid-decay across the muted frames. Resume from REST
     * instead of the stale frozen state to avoid an un-mute transient/click.
     * Un-mute in real titles is almost always a volume RAMP, so the first
     * audible frames are near-silent and the from-rest transient is masked.
     * (Bit-exact reconstruction would require running the filters over the
     * muted-frame input -- exactly the decode we skipped.) */
    if (d->vp.filters[v].muted_prev) {
        memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
        for (int ch = 0; ch < 2; ch++) {
            memset(d->vp.filters[v].hrtf.ch[ch].buf, 0,
                   sizeof(d->vp.filters[v].hrtf.ch[ch].buf));
        }
        d->vp.filters[v].muted_prev = false;
    }
#endif

    int fmode = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);

    // FIXME: Move to function
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        /* 1:DLS2+I3DL2 2:ParaEQ+I3DL2 3:I3DL2 */
        lpf = (fmode == 1);
    } else {
        /* 0:Bypass 1:DLS2 2:ParaEQ 3(Mono):DLS2+ParaEQ 3(Stereo):Bypass */
        lpf = stereo ? (fmode == 1) : (fmode & 1);
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            // FIXME: Cutoff modulation via NV_PAVS_VOICE_CFG_ENV1_EF_FCSCALE
            int16_t fc = voice_get_mask_nolock(
                d, c, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(pow(2, fc / 4096.0), 0.003906f, 1.0f);
            uint16_t q = voice_get_mask_nolock(
                d, c, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0 * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    if (v < MCPX_HW_MAX_3D_VOICES && g_apu_config.hrtf) {
        uint16_t hrtf_handle =
            voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                           NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            hrtf_filter_process(&d->vp.filters[v].hrtf, samples, samples);
        }
    }

    // FIXME: ParaEQ

    /* Deinterleave samples[32][2] into per-channel contiguous buffers once
     * (amortized across 8 subsequent mix_one_bin calls). mix_one_bin
     * (NEON-accelerated on aarch64) reads contiguous float[32]. */
    float channel_buf[2][NUM_SAMPLES_PER_FRAME];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        channel_buf[0][i] = samples[i][0];
    }
    if (channels == 2) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            channel_buf[1][i] = samples[i][1];
        }
    }

    /* The VP's own mix (the default, no DSP): the frame's mixbins are
     * cleared unread at its end, but for the multipass bin the next voices
     * of a group read (scheduled together, voice_work_schedule) */
    const bool mon_vp = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
    for (int b = 0; b < 8; b++) {
        if (mon_vp && bin[b] != MULTIPASS_BIN) {
            continue;
        }
        float g = ea_value;
        float hr;
        if ((v < MCPX_HW_MAX_3D_VOICES) && (b < 4)) {
            // FIXME: Not sure if submix/voice headroom factor in for HRTF
            hr = 1 << d->vp.hrtf_headroom;
        } else {
            hr = 1 << d->vp.submix_headroom[bin[b]];
        }
        g *= attenuate(vol[b]) / hr;
        mix_one_bin(mixbins[bin[b]], channel_buf[b % channels], g);
    }

    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        /* For VP mon, simply mix all voices together here, selecting the
         * maximal volume used for any given mixbin as the overall volume for
         * this voice.
         *
         * If the current voice belongs to a multipass sub-voice group we must
         * skip it here to avoid mixing it in twice because the sub-voices are
         * mixed into the multipass bin and that sub-mix will be mixed in here
         * later when the destination (i.e. second pass) voice is processed.
         * TODO: Are the 2D, 3D and MP voice lists merely a DirectSound
         *       convention? Perhaps hardware doesn't care if e.g. a multipass
         *       voice is in the 2D or 3D list. On the other hand, MON_VP is
         *       not how the hardware works anyway so not much point worrying
         *       about precise emulation here. DirectSound compatibility is
         *       enough.
         */
        int mp_bin = -1;
        uint16_t mp_dst_voice = 0xFFFF;
        if (voice_list == NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP - 1) {
            mp_bin = peek_ahead_multipass_bin(d, c, v, &mp_dst_voice);
        }

        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            if (bin[b] == mp_bin) {
                continue;
            }
            float hr = 1 << d->vp.submix_headroom[bin[b]];
            g = fmax(g, attenuate(vol[b]) / hr);
        }
        g *= ea_value;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g*samples[i][0];
            sample_buf[i][1] += g*samples[i][1];
        }
    }
}

static void get_voice_bin_src_dst(MCPXAPUState *d, uint8_t *c, int v,
                                  uint32_t *src, uint32_t *dst, uint32_t *clr)
{
    uint32_t src_v = 0;
    uint32_t dst_v = 0;
    uint32_t clr_v = 0;

    bool multipass = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS);
    if (multipass) {
        int mp_bin = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        bool clear_mix = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX);
        src_v |= (1 << mp_bin);
        if (clear_mix) {
            clr_v |= (1 << mp_bin);
        }
    }

    int bin[8];
    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    } else {
        bin[0] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V0BIN);
        bin[1] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V1BIN);
        bin[2] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V2BIN);
        bin[3] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                                NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    }
    bin[4] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_VBIN,
                            NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_CFG_FMT,
                            NV_PAVS_VOICE_CFG_FMT_V7BIN);

    for (int i = 0; i < 8; i++) {
        dst_v |= 1 << bin[i];
    }

    if (src) {
        *src = src_v;
    }
    if (dst) {
        *dst = dst_v;
    }
    if (clr) {
        *clr = clr_v;
    }
}

/* One share of the frame's voices: processed outside the lock, then added
 * in under it. Called and returning with vwd->lock held, by a helper
 * thread or (share 0) the frame thread. */
static void voice_work_share(MCPXAPUState *d, VoiceWorker *self)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    pthread_mutex_unlock(&vwd->lock);

    // Process queued voices
    const bool mon_vp = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
    if (mon_vp) {
        /* only the multipass bin is mixed (voice_process), and it stays in
         * the share: its group is scheduled together */
        memset(self->mixbins[MULTIPASS_BIN], 0, sizeof(self->mixbins[0]));
        memset(self->sample_buf, 0, sizeof(self->sample_buf));
    } else {
        memset(self->mixbins, 0, sizeof(self->mixbins));
    }
    /* The frame's voice table (voice_process and its callees take
     * it as `c`; their stores are reported with the batch). */
    {
        uint8_t *c = vwd->table;
        for (int i = 0; i < self->queue_len; i++) {
            voice_process(d, c, self->mixbins, self->sample_buf,
                          self->queue[i].voice, self->queue[i].list);
        }
    }

    pthread_mutex_lock(&vwd->lock);

    // Add voice contributions (the mixbins: the DSP's, which the VP's own mix has none of)
    if (!mon_vp) {
        for (int b = 0; b < NUM_MIXBINS; b++) {
            for (int s = 0; s < NUM_SAMPLES_PER_FRAME; s++) {
                vwd->mixbins[b][s] += self->mixbins[b][s];
            }
        }
    } else {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->vp.sample_buf[i][0] += self->sample_buf[i][0];
            d->vp.sample_buf[i][1] += self->sample_buf[i][1];
        }
    }

    self->queue_len = 0;
}

/* A helper: shares 1 and up (share 0 is the frame thread's own). */
static void *voice_worker_thread(void *arg)
{
    MCPXAPUState *d = arg;
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);

    int worker_id = ctz64(vwd->workers_pending);
    VoiceWorker *self = &d->vp.voice_work_dispatch.workers[worker_id];
    self->queue_len = 0;

    do {
        if (self->queue_len) {
            voice_work_share(d, self);
        }

        vwd->workers_pending &= ~(1 << worker_id);
        if (!vwd->workers_pending) {
            pthread_cond_signal(&vwd->work_finished);
        }

        pthread_cond_wait(&self->cond, &vwd->lock);
    } while (!vwd->workers_should_exit);

    pthread_mutex_unlock(&vwd->lock);   /* the next worker's wake-up takes it */
    return NULL;
}

/* A listed voice for the frame, by its state under its spinlock (the guest's
 * methods change that state holding the voice: mcpx_apu_vp_write's guard).
 * The frame never waits on the guest for long: a voice the guest holds (its
 * VOICE_LOCK, or a method running) is waited for HELD_WAIT_US, holding none
 * of its own (the guest's unlock takes it), the first frame it is held, then
 * left for a later frame. Waiting for the unlock, the frame deadlocked with a
 * guest that locks the voices of a group (a multichannel stream) one by one,
 * its next lock waiting on one the frame's batch held already; and with one
 * that waits on the APU holding a voice (a notification, the GP's mailbox:
 * the frame that produces it never came). On the hardware the VP keeps its
 * pace whatever the CPU holds. The wait lasts while the listener can't tell:
 * up to HELD_WAIT_US, while more than a callback's worth is queued for the
 * output (the frames then catch up before the next callback), slept for
 * after HELD_SPIN_US. A title's holds end within a fraction of a millisecond
 * (Forza's race: 0.22 ms at most in a minute); spun for throughout, the wait
 * kept the holding thread off a core they shared: holds then lasted until
 * the spin gave up, and about one a second was left out of its frames (a
 * gap of 0.7-2.7 ms in the voice). */
#define HELD_WAIT_US 8000
#define HELD_SPIN_US 50                 /* then slept for, HELD_NAP_US at a time */
#define HELD_NAP_US 100
#define HELD_STUCK_FRAMES 1500          /* a second of GP frames */
/* HAKUX_APU_STATS: frames a voice was left out; holds waited out, the
 * longest of those (us); the longest run of frames a voice was left out */
uint32_t g_vp_held_skips, g_vp_held_waits, g_vp_held_wait_max_us, g_vp_held_run_max;

typedef enum VoiceTake {
    VOICE_TAKEN,    /* active: its spinlock kept for the batch */
    VOICE_IDLE,     /* off: for the idle report */
    VOICE_HELD,     /* the guest's: a later frame's */
} VoiceTake;

static VoiceTake voice_take(MCPXAPUState *d, uint8_t *c, uint16_t v)
{
    ApuSpin *s = &d->vp.voice_spinlocks[v];
    apu_spin_lock(s);
    if (is_voice_locked(d, v)) {
        apu_spin_unlock(s);
        uint16_t *held = &d->vp.held_frames[v];
        bool got = false;
        if (!*held) {
            const int64_t t0 = apu_host_us();
            int64_t us = 0;
            while (us < HELD_WAIT_US && apu_out_queued() >= d->monitor.device_buffer_bytes) {
                if (!is_voice_locked(d, v)) {
                    apu_spin_lock(s);
                    if ((got = !is_voice_locked(d, v))) {
                        break;
                    }
                    apu_spin_unlock(s);
                } else if (us < HELD_SPIN_US) {
#if defined(__aarch64__)
                    __asm__ __volatile__("yield" ::: "memory");
#endif
                } else {
                    usleep(HELD_NAP_US);
                }
                us = apu_host_us() - t0;
            }
            if (got) {
                apu_inc(&g_vp_held_waits);
                if ((uint32_t)us > apu_ld(&g_vp_held_wait_max_us)) {
                    apu_st(&g_vp_held_wait_max_us, (uint32_t)us);
                }
            }
        }
        if (!got) {
            apu_inc(&g_vp_held_skips);
            if (*held < UINT16_MAX && ++*held == HELD_STUCK_FRAMES) {
                nc_log("hakuX-apu", "stuck: voice %u held by the guest (its VOICE_LOCK) for a second, "
                                    "left out of the frames since; FECTL %08x ISTS %08x",
                       v, apu_ld(&d->regs[NV_PAPU_FECTL]), apu_ld(&d->regs[NV_PAPU_ISTS]));
            }
            return VOICE_HELD;
        }
    }
    if (d->vp.held_frames[v]) {
        if (d->vp.held_frames[v] > apu_ld(&g_vp_held_run_max)) {
            apu_st(&g_vp_held_run_max, d->vp.held_frames[v]);
        }
        d->vp.held_frames[v] = 0;
    }
    if (!voice_get_mask_nolock(d, c, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
        apu_spin_unlock(s);
        return VOICE_IDLE;
    }
    return VOICE_TAKEN;
}

/* an active voice for the batch, its spinlock taken (voice_take) */
static void voice_work_enqueue(MCPXAPUState *d, int v, int list)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    assert(vwd->queue_len < ARRAY_SIZE(vwd->queue));
    vwd->queue[vwd->queue_len++] = (VoiceWorkItem){
        .voice = v,
        .list = list,
    };
}

static void voice_work_release_voice_locks(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    for (int i = 0; i < vwd->queue_len; i++) {
        apu_spin_unlock(&d->vp.voice_spinlocks[vwd->queue[i].voice]);
    }
}

/* Fewer voices than this a frame: all the frame thread's, no helper woken
 * (a wake and a merge cost more than the voices). */
#define VOICES_INLINE 8

static void voice_work_schedule(MCPXAPUState *d, uint8_t *c)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    const int shares = vwd->queue_len < VOICES_INLINE ? 1 : vwd->num_workers;
    int next_worker_to_schedule = 0;
    bool group = false;
    uint32_t dirty = 0;

    for (int i = 0; i < vwd->queue_len; i++) {
        uint32_t src, dst, clr;
        get_voice_bin_src_dst(d, c, vwd->queue[i].voice, &src, &dst, &clr);

        // TODO: To simplify submix scheduling, we make a few assumptions based
        // on Xbox software observations. However, the configurability of
        // multipass sources suggests the hardware may not be so strict. We'll
        // defer making this more robust for now.
        //
        // We currently assume that:
        //
        // - MP bin is constant
        assert(!src || (src == MULTIPASS_BIN_MASK));
        //
        // - MP voice always clears MP bin
        assert(!src || (clr == MULTIPASS_BIN_MASK));
        //
        // - MP source voices are ordered consecutively in voice lists
        assert(src || (dst & MULTIPASS_BIN_MASK) ||
               !(dirty & MULTIPASS_BIN_MASK));

        if ((dst & MULTIPASS_BIN_MASK) & ~dirty) {
            group = true;
        }

        // Assign voice to worker
        VoiceWorker *worker = &vwd->workers[next_worker_to_schedule];
        worker->queue[worker->queue_len++] = vwd->queue[i];
        vwd->workers_pending |= 1 << next_worker_to_schedule;

        dirty = (dirty & ~clr) | dst;
        if (clr & MULTIPASS_BIN_MASK) {
            group = false;
        }

        if (!group) {
            next_worker_to_schedule = (next_worker_to_schedule + 1) % shares;
        }
    }
}

static void
voice_work_dispatch(MCPXAPUState *d, uint8_t *c,
                    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);

    if (vwd->queue_len) {
        if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
            memset(vwd->mixbins, 0, sizeof(vwd->mixbins));
        }
        vwd->table = c;

        // Signal only the helpers that have been assigned work
        voice_work_schedule(d, c);
        uint64_t pending = vwd->workers_pending & ~1ull;
        while (pending) {
            int id = ctz64(pending);
            pthread_cond_signal(&vwd->workers[id].cond);
            pending &= pending - 1;
        }
        /* share 0 here, meanwhile: a wake fewer, and no wait at all when
         * it is the only one */
        if (vwd->workers_pending & 1) {
            voice_work_share(d, &vwd->workers[0]);
            vwd->workers_pending &= ~1ull;
        }
        while (vwd->workers_pending) {  /* a spurious wake-up: workers still running */
            pthread_cond_wait(&vwd->work_finished, &vwd->lock);
        }
        voice_work_release_voice_locks(d);
        vwd->queue_len = 0;

        // Add voice contributions (none to the mixbins in the VP's own mix)
        if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
            for (int b = 0; b < NUM_MIXBINS; b++) {
                for (int s = 0; s < NUM_SAMPLES_PER_FRAME; s++) {
                    mixbins[b][s] += vwd->mixbins[b][s];
                }
            }
        }
    }

    pthread_mutex_unlock(&vwd->lock);
}

static int mcpx_apu_default_vp_worker_count(void)
{
    int cpu_count = MAX(1, (int)sysconf(_SC_NPROCESSORS_ONLN));

    /*
     * Two shares: the frame thread's and one helper's. A helper costs a
     * wake, a wait and a mixbin merge per GP frame (1500 a second), and on
     * big.LITTLE SoCs the third and fourth run on the little cores beside
     * the JIT and the renderers: on the RP5 (Forza's race start, slot 3)
     * four workers took 37% of a core where two took 27% and one 25%, when
     * the frame thread only waited for them. NC_APU_WORKERS overrides
     * (g_apu_config).
     */
    return cpu_count <= 2 ? 1 : 2;
}

static void voice_work_init(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    int num_workers = g_apu_config.num_workers;
    if (num_workers <= 0) {
        num_workers = mcpx_apu_default_vp_worker_count();
    }
    vwd->num_workers = MAX(1, MIN(num_workers, MAX_VOICE_WORKERS));
    vwd->workers = calloc(vwd->num_workers, sizeof(VoiceWorker));
    vwd->workers_should_exit = false;
    vwd->workers_pending = 0;
    vwd->queue_len = 0;


    pthread_mutex_init(&vwd->lock, NULL);
    pthread_mutex_lock(&vwd->lock);
    apu_cond_init(&vwd->work_finished);
    for (int i = 0; i < vwd->num_workers; i++) {
        apu_cond_init(&vwd->workers[i].cond);
    }
    for (int i = 1; i < vwd->num_workers; i++) {    /* share 0: the frame thread */
        vwd->workers_pending |= 1 << i;
        apu_thread_start(&vwd->workers[i].thread, "apu.voice",
                           voice_worker_thread, d, true);
    }
    while (vwd->workers_pending) {
        pthread_cond_wait(&vwd->work_finished, &vwd->lock);
    }
    pthread_mutex_unlock(&vwd->lock);
}

static void voice_work_finalize(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    pthread_mutex_lock(&vwd->lock);
    vwd->workers_should_exit = true;
    for (int i = 0; i < vwd->num_workers; i++) {
        pthread_cond_signal(&vwd->workers[i].cond);
    }
    pthread_mutex_unlock(&vwd->lock);
    for (int i = 1; i < vwd->num_workers; i++) {
        pthread_join(vwd->workers[i].thread, NULL);
    }
    free(vwd->workers);
    vwd->workers = NULL;
}

/* The SE reports the idle voices on its lists to the FE, one trap at a time:
 * on the hardware a pass reports each of them, in list order, and the guest
 * releases those it wants gone. Here a frame reports one, the next after the
 * one reported last (the first when that one has left the lists): reported
 * from the head each frame, an idle voice the guest leaves on its list hid
 * those after it for good. DirectSound releases a voice group (a
 * multichannel stream's linked voices) on its last voice's report alone:
 * MechAssault stops a three-voice stream before a mission and waits for it
 * to stop playing; with its first voice reported every frame and ignored,
 * the group was never released and the game spun in GetStatus, loading,
 * where the guest turned the three off within one frame (the Odin 3). */
static void idle_report(MCPXAPUState *d, const uint16_t *idle, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (idle[i] == g_idle_last) {
            k = i + 1 < n ? i + 1 : 0;
            break;
        }
    }
    if (!n || !apu_spin_trylock(&d->vp.fe_spin)) {
        return;
    }
    /* one trap at a time, its method and argument written whole (fe_spin:
     * the guest's methods run on its CPU meanwhile); only tried: the walk
     * holds the voice locks of its batch, which a guest write may be waiting
     * on (a voice not reported now is in a later frame: it stays listed) */
    if ((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) != NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        fe_method(d, SE2FE_IDLE_VOICE, idle[k]);
        off_notify_now(d, idle[k]);     /* its VOICE_OFF's, with the trap (voice_off) */
        g_idle_last = idle[k];
    }
    apu_spin_unlock(&d->vp.fe_spin);
}

void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    /* The voice table loaded once for the frame: the list walk below and
     * voice_work_dispatch / voice_work_schedule use it. */
    uint8_t *c = apu_ld_acq(&d->vp.voice_cache);

    /* The voices queued this frame: one twice (a cycle in the guest's
     * lists) would take its own lock again and spin forever, and more than
     * MCPX_HW_MAX_VOICES would overrun the queue. */
    uint64_t queued[MCPX_HW_MAX_VOICES / 64] = {0};
    uint64_t listed[MCPX_HW_MAX_VOICES / 64] = {0};   /* the voices on a list now */
    uint16_t idle[MCPX_HW_MAX_VOICES];  /* the idle voices on them, in list order */
    unsigned nidle = 0;

    /* Process all voices, mixing each into the affected MIXBINs */
    for (int list = 0; list < 3; list++) {
        uint64_t top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];
        DPRINTF("list %d current voice %d\n", list, d->regs[current]);

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            /* Make sure not to get stuck... */
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint32_t v = d->regs[current];
            if (!APU_CHECK(v < MCPX_HW_MAX_VOICES)) {
                break;
            }
            d->regs[next] = voice_get_mask_nolock(d, c, v,
                               NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            const uint64_t bit = 1ull << (v % 64);
            listed[v / 64] |= bit;
            if (!APU_CHECK(!(queued[v / 64] & bit))) {
                break;          /* its spinlock is the batch's already */
            }
            /* its state read under its spinlock: a voice the guest is
             * turning on is on the list before it is active, and was
             * reported idle */
            switch (voice_take(d, c, (uint16_t)v)) {
            case VOICE_TAKEN:
                queued[v / 64] |= bit;
                voice_work_enqueue(d, v, list);
                break;
            case VOICE_IDLE:
                if (nidle < ARRAY_SIZE(idle)) {
                    idle[nidle++] = (uint16_t)v;    /* reported below (idle_report) */
                }
                break;
            case VOICE_HELD:
                break;          /* listed: its VOICE_OFF's notification waits */
            }
            d->regs[current] = d->regs[next];
        }
    }
    idle_report(d, idle, nidle);
    /* the guest's VOICE_OFFs (voice_off): notified once their voice has
     * left the lists, its release done */
    for (unsigned w = 0; w < MCPX_HW_MAX_VOICES / 64; w++) {
        for (uint64_t p = __atomic_load_n(&g_off_pending[w], __ATOMIC_ACQUIRE); p; p &= p - 1) {
            const uint16_t v = (uint16_t)(w * 64 + __builtin_ctzll(p));
            if (!(listed[w] & (1ull << (v % 64))) || ++g_off_frames[v] >= OFF_NOTIFY_MAX_FRAMES) {
                off_notify_now(d, v);
            }
        }
    }
    voice_work_dispatch(d, c, mixbins);
    if (c) {
        report_written_voices(d, c);
    }

    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        /* Mix all voices together to hear any audible voice */
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2*i];
            d->monitor.frame_buf[off + i][1] += isamp[2*i+1];
        }

        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float[32][32]));
    }
}

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    attenuation_init();
    for (int i = 0; i < MCPX_HW_MAX_VOICES; i++) {
        apu_spin_init(&d->vp.voice_spinlocks[i]);
    }
    apu_spin_init(&d->vp.fe_spin);

    d->vp.voice_cache = NULL;
    d->vp.ssl_cache = NULL;

    voice_work_init(d);
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    voice_work_finalize(d);
    for (int v = 0; v < ARRAY_SIZE(d->vp.filters); v++) {  /* made at a voice's first frame */
        d->vp.filters[v].resampler = src_delete(d->vp.filters[v].resampler);
    }

    d->vp.voice_cache = NULL;
    d->vp.ssl_cache = NULL;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    memset(d->vp.held_frames, 0, sizeof(d->vp.held_frames));
    memset(d->vp.voice_wrote, 0, sizeof(d->vp.voice_wrote));
    memset(g_off_pending, 0, sizeof(g_off_pending));
    g_idle_last = 0xFFFF;
    for (int v = 0; v < ARRAY_SIZE(d->vp.filters); v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
    refresh_table(&d->vp.voice_cache, 0, 0);
    refresh_table(&d->vp.ssl_cache, 0, 0);
}

/* After a savestate (the voice table's pointer set): each 3D voice's HRTF
 * filter aimed again at the entry its voice names, as SET_VOICE_TAR_HRTF
 * did (the entries come with the APU's state, the handles with RAM): a 3D
 * voice whose parameters the title doesn't commit again was silent. */
void mcpx_apu_vp_restore_hrtf(MCPXAPUState *d)
{
    if (!d->regs[NV_PAPU_VPVADDR]) {
        return;
    }
    unsigned set = 0, entries = 0;
    for (uint16_t v = 0; v < MCPX_HW_MAX_3D_VOICES; v++) {
        unsigned handle = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                         NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (handle != HRTF_NULL_HANDLE && handle < HRTF_ENTRY_COUNT) {
            hrtf_filter_set_target_params(&d->vp.filters[v].hrtf, d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
            set++;
        }
    }
    for (unsigned e = 0; e < HRTF_ENTRY_COUNT; e++) {
        bool any = d->vp.hrtf.entries[e].itd != 0.0f;
        for (unsigned k = 0; k < HRTF_NUM_TAPS && !any; k++) {
            any = d->vp.hrtf.entries[e].hrir[0][k] != 0.0f || d->vp.hrtf.entries[e].hrir[1][k] != 0.0f;
        }
        entries += any;
    }
    nc_log("hakuX-native-state", "apu: %u 3D voices' HRTF filters set again (%u entries hold coefficients)", set,
           entries);
}
