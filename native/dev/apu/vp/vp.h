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
#ifndef HW_XBOX_MCPX_VP_H
#define HW_XBOX_MCPX_VP_H

#include "native/dev/apu/apu_base.h"
#include "native/dev/apu/apu_regs.h"
#include "resample.h"
#include "svf.h"
#include "hrtf.h"

typedef struct MCPXAPUState MCPXAPUState;

typedef struct MCPXAPUVPSSLData {
    uint32_t base[MCPX_HW_SSLS_PER_VOICE];
    uint8_t count[MCPX_HW_SSLS_PER_VOICE];
    int ssl_index;
    int ssl_seg;
} MCPXAPUVPSSLData;

typedef struct MCPXAPUVoiceFilter {
    uint16_t voice;
    float resample_buf[NUM_SAMPLES_PER_FRAME * 2];
    SRC_STATE *resampler;
    sv_filter svf[2];
    HrtfFilter hrtf;
    /* The voice table for voice_resample_callback (libsamplerate's callback
     * signature cannot be extended). Set by voice_resample before
     * src_callback_read; valid only for the duration of that synchronous
     * call. */
    uint8_t *cb_voice_cache;
    /* HAKUX_AUDIO_SKIP_MUTED plumbing. Declared unconditionally to avoid ABI
     * churn (2 bytes/voice); only read/written when the flag is enabled.
     * cb_skip_decode: set by voice_resample before src_callback_read, consumed
     *   inside voice_resample_callback -> voice_get_samples to null the per-
     *   sample decode for a muted voice. Same synchronous-call lifetime
     *   contract as cb_voice_cache above (a refactor caching it across calls
     *   would break it).
     * muted_prev: this voice was muted (skipped) on the previous audible
     *   evaluation; drives the SVF/HRTF state reset on the muted->audible
     *   transition. */
    bool cb_skip_decode;
    bool muted_prev;
    /* The ADPCM block decoded last (voice_get_samples): its bytes as read
     * and its samples. A call that reads the same bytes takes the samples:
     * each GP frame's call decoded its first block again (a 64-sample block
     * spans two 32-sample frames). Not saved: a load starts it empty. */
    uint32_t adpcm_raw[36 * 2 / 4];
    int16_t adpcm_pcm[65 * 2];
    uint32_t adpcm_raw_len;     /* 0: none */
    uint8_t adpcm_channels;
} MCPXAPUVoiceFilter;

typedef struct VoiceWorkItem {
    int voice;
    int list;
} VoiceWorkItem;

typedef struct VoiceWorker {
    pthread_t thread;
    pthread_cond_t cond;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    float sample6[NUM_SAMPLES_PER_FRAME][6];   /* 5.1 (g_apu_out_channels 6): the speakers' */
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorker;

typedef struct VoiceWorkDispatch {
    pthread_mutex_t lock;
    uint8_t *table;     /* the frame's voice table (the batch's `c`) */
    int num_workers;
    VoiceWorker *workers;
    bool workers_should_exit;
    uint64_t workers_pending;
    pthread_cond_t work_finished;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    VoiceWorkItem queue[MCPX_HW_MAX_VOICES];
    int queue_len;
} VoiceWorkDispatch;

typedef struct {
    VoiceWorkDispatch voice_work_dispatch;
    MCPXAPUVoiceFilter filters[MCPX_HW_MAX_VOICES];

    // FIXME: Where are these stored?
    int ssl_base_page;
    MCPXAPUVPSSLData ssl[MCPX_HW_MAX_VOICES];
    uint8_t hrtf_headroom;
    uint8_t hrtf_submix[4];
    uint8_t submix_headroom[NUM_MIXBINS];
    float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    uint64_t voice_locked[4];
    /* the frames in a row each voice was left out, the guest holding it
     * (vp.c voice_take); not saved */
    uint16_t held_frames[MCPX_HW_MAX_VOICES];
    ApuSpin voice_spinlocks[MCPX_HW_MAX_VOICES];
    ApuSpin fe_spin;   /* fe_method: the guest's methods against the VP's traps */

    struct {
        int current_entry;
        // FIXME: Stored in RAM
        struct {
            float hrir[2][HRTF_NUM_TAPS];
            float itd;
        } entries[HRTF_ENTRY_COUNT];
    } hrtf;

    uint32_t inbuf_sge_handle; //FIXME: Where is this stored?
    uint32_t outbuf_sge_handle; //FIXME: Where is this stored?

    /* The voice and SSL tables in guest RAM (VPVADDR, VPSSLADDR), NULL when
     * the base leaves no room for the whole table (the accesses then go by
     * guest address, each checked). Set by the register write (release),
     * read by the frame thread and the workers (acquire). */
    uint8_t *voice_cache;
    uint8_t *ssl_cache;
    /* A frame's stores to the voice table: the voices written (by the
     * workers, a voice by one at a time), reported to the HLE's write
     * tracking as runs when the frame's batch ends (report_written_voices),
     * not store by store. */
    uint8_t voice_wrote[MCPX_HW_MAX_VOICES];
} MCPXAPUVPState;

/* the VP's MMIO window (0x20000 into the APU's, 0x10000 bytes) */
uint64_t mcpx_apu_vp_read(void *opaque, uint32_t addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size);

void mcpx_apu_vp_init(MCPXAPUState *d);
void mcpx_apu_vp_finalize(MCPXAPUState *d);
void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
/* The guest's VOICE_OFFs whose notification waits (vp.c voice_off): any, and
 * all of them notified now, for a frame thread stopped for the guest that
 * waited their time (true when one was). */
bool mcpx_apu_vp_off_pending(void);
bool mcpx_apu_vp_off_notify_all(MCPXAPUState *d);
void mcpx_apu_vp_reset(MCPXAPUState *d);
void mcpx_apu_vp_refresh_voice_cache(MCPXAPUState *d, uint32_t new_base);
void mcpx_apu_vp_refresh_ssl_cache(MCPXAPUState *d, uint32_t new_base);
void mcpx_apu_vp_restore_hrtf(MCPXAPUState *d);

/* PERF_LOG-gated "silent-but-active voice" probe emit. Compiles to an empty
 * function when PERF_LOG=0. Call once per EP frame from se_frame(); the
 * function self-throttles its log cadence internally. */
void vp_silent_stats_log_and_reset(void);

#endif
