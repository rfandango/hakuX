/*
 * QEMU Geforce NV2A profiling and debug helpers
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_DEBUG_H
#define HW_XBOX_NV2A_DEBUG_H

#include <stdint.h>

#define NV2A_XPRINTF(x, ...) do { \
    if (x) { \
        fprintf(stderr, "nv2a: " __VA_ARGS__); \
    } \
} while (0)

#ifndef DEBUG_NV2A
# define DEBUG_NV2A 0
#endif

#if DEBUG_NV2A
# define NV2A_DPRINTF(format, ...)       printf("nv2a: " format, ## __VA_ARGS__)
#else
# define NV2A_DPRINTF(format, ...)       do { } while (0)
#endif

/* Debug prints to identify when unimplemented or unconfirmed features
 * are being exercised. These cases likely result in graphical problems of
 * varying degree, but should otherwise not crash the system. Enable this
 * macro for debugging.
 */
#ifndef DEBUG_NV2A_FEATURES
# define DEBUG_NV2A_FEATURES 0
#endif

#if DEBUG_NV2A_FEATURES

/* Feature which has not yet been confirmed */
#define NV2A_UNCONFIRMED(format, ...) do { \
    fprintf(stderr, "nv2a: Warning unconfirmed feature: " format "\n", ## __VA_ARGS__); \
} while (0)

/* Feature which is not implemented */
#define NV2A_UNIMPLEMENTED(format, ...) do { \
    fprintf(stderr, "nv2a: Warning unimplemented feature: " format "\n", ## __VA_ARGS__); \
} while (0)

#else

#define NV2A_UNCONFIRMED(...) do {} while (0)
#define NV2A_UNIMPLEMENTED(...) do {} while (0)

#endif

#define NV2A_PROF_COUNTERS_XMAC \
    _X(NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY) \
    _X(NV2A_PROF_FINISH_SURFACE_CREATE) \
    _X(NV2A_PROF_FINISH_SURFACE_DOWN) \
    _X(NV2A_PROF_FINISH_NEED_BUFFER_SPACE) \
    _X(NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY) \
    _X(NV2A_PROF_FINISH_PRESENTING) \
    _X(NV2A_PROF_FINISH_FLIP_STALL) \
    _X(NV2A_PROF_FINISH_FLUSH) \
    _X(NV2A_PROF_FINISH_STALLED) \
    _X(NV2A_PROF_CLEAR) \
    _X(NV2A_PROF_QUEUE_SUBMIT) \
    _X(NV2A_PROF_QUEUE_SUBMIT_AUX) \
    _X(NV2A_PROF_PIPELINE_NOTDIRTY) \
    _X(NV2A_PROF_PIPELINE_GEN) \
    _X(NV2A_PROF_PIPELINE_BIND) \
    _X(NV2A_PROF_PIPELINE_RENDERPASSES) \
    _X(NV2A_PROF_BEGIN_ENDS) \
    _X(NV2A_PROF_DRAW_ARRAYS) \
    _X(NV2A_PROF_INLINE_BUFFERS) \
    _X(NV2A_PROF_INLINE_ARRAYS) \
    _X(NV2A_PROF_INLINE_ELEMENTS) \
    _X(NV2A_PROF_QUERY) \
    _X(NV2A_PROF_SHADER_GEN) \
    _X(NV2A_PROF_SHADER_BIND) \
    _X(NV2A_PROF_SHADER_BIND_NOTDIRTY) \
    _X(NV2A_PROF_SHADER_UBO_DIRTY) \
    _X(NV2A_PROF_SHADER_UBO_NOTDIRTY) \
    _X(NV2A_PROF_ATTR_BIND) \
    _X(NV2A_PROF_TEX_UPLOAD) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_1) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_2) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_3) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_4) \
    _X(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY) \
    _X(NV2A_PROF_SURF_SWIZZLE) \
    _X(NV2A_PROF_SURF_CREATE) \
    _X(NV2A_PROF_SURF_DOWNLOAD) \
    _X(NV2A_PROF_SURF_UPLOAD) \
    _X(NV2A_PROF_SURF_TO_TEX) \
    _X(NV2A_PROF_SURF_TO_TEX_FALLBACK) \
    _X(NV2A_PROF_QUEUE_SUBMIT_1) \
    _X(NV2A_PROF_QUEUE_SUBMIT_2) \
    _X(NV2A_PROF_QUEUE_SUBMIT_3) \
    _X(NV2A_PROF_QUEUE_SUBMIT_4) \
    _X(NV2A_PROF_QUEUE_SUBMIT_5) \
    _X(NV2A_PROF_REORDER_WINDOW_FLUSH) \
    _X(NV2A_PROF_REORDER_DRAWS) \

enum NV2A_PROF_COUNTERS_ENUM {
    #define _X(x) x,
    NV2A_PROF_COUNTERS_XMAC
    #undef _X
    NV2A_PROF__COUNT
};

#define NV2A_PROF_NUM_FRAMES 300

typedef struct FramePacingStats {
    float game_frame_ms;
    float game_frame_min_ms;
    float game_frame_max_ms;
    float display_frame_ms;
    float display_frame_min_ms;
    float display_frame_max_ms;
    float swap_ms;
    unsigned int defers_total;
    unsigned int defers_window;
    unsigned int vblank_fired;
    float vblank_jitter_ms;
    float vblank_delivery_ms;
    bool unlock_mode_active;
} FramePacingStats;

typedef struct ShaderPipelineStats {
    unsigned int pipeline_cache_hits;
    unsigned int pipeline_cache_misses;
    unsigned int shader_cache_hits;
    unsigned int shader_cache_misses;
    unsigned int spv_cache_hits;
    unsigned int spv_cache_misses;
    unsigned int pipeline_cache_disk_loaded;
    unsigned int pipeline_cache_disk_saved;
} ShaderPipelineStats;

typedef struct FramePhaseTimingWork {
    int64_t surface_update_ns;
    int64_t texture_upload_ns;
    int64_t shader_compile_ns;
    int64_t draw_dispatch_ns;
    int64_t finish_ns;
    int64_t flip_idle_ns;
    int64_t fifo_idle_ns;
    int64_t fifo_idle_frame_ns;
    int64_t fifo_idle_starve_ns;
    bool post_flip;
    /* Sub-phases within draw_dispatch */
    int64_t draw_vtx_attr_ns;
    int64_t draw_vtx_sync_ns;
    int64_t draw_prim_rw_ns;
    int64_t draw_pipeline_ns;
    int64_t draw_desc_set_ns;
    int64_t draw_setup_ns;
    int64_t draw_vk_cmd_ns;
    /* Sub-phases within draw_pipeline (create_pipeline) */
    int64_t pipe_bind_tex_ns;
    int64_t pipe_bind_shd_ns;
    int64_t pipe_lookup_ns;
    /* Sub-phases within finish */
    int64_t finish_fence_ns;
    int64_t finish_submit_ns;
} FramePhaseTimingWork;

typedef struct FramePhaseTimingStats {
    float surface_update_ms;
    float texture_upload_ms;
    float shader_compile_ms;
    float draw_dispatch_ms;
    float finish_ms;
    float flip_idle_ms;
    float fifo_idle_ms;
    float fifo_idle_frame_ms;
    float fifo_idle_starve_ms;
    float total_ms;
    /* Sub-phases within draw_dispatch */
    float draw_vtx_attr_ms;
    float draw_vtx_sync_ms;
    float draw_prim_rw_ms;
    float draw_pipeline_ms;
    float draw_desc_set_ms;
    float draw_setup_ms;
    float draw_vk_cmd_ms;
    /* Sub-phases within draw_pipeline (create_pipeline) */
    float pipe_bind_tex_ms;
    float pipe_bind_shd_ms;
    float pipe_lookup_ms;
    /* Sub-phases within finish */
    float finish_fence_ms;
    float finish_submit_ms;
} FramePhaseTimingStats;

typedef struct CpuTimingWork {
    int64_t lock_wait_ns;
    int64_t pusher_run_ns;
    int64_t method_exec_ns;
    int64_t puller_total_ns;
    int64_t puller_lock_ns;
    int64_t puller_method_ns;
    uint32_t kick_count;
    uint32_t kick_count_spun;
    uint32_t pusher_words;
    uint32_t method_count;
    uint32_t method_fast_hit;
    uint32_t method_noninc_words;
    uint64_t tb_hits_snap;
    uint64_t tb_misses_snap;
} CpuTimingWork;

typedef struct CpuTimingStats {
    float lock_wait_ms;
    float pusher_run_ms;
    float method_exec_ms;
    float puller_total_ms;
    float puller_lock_ms;
    float puller_method_ms;
    float kick_count;
    float kick_count_spun;
    float pusher_words;
    float method_count;
    float method_fast_hit;
    float method_noninc_words;
    float tb_hit_pct;
} CpuTimingStats;

typedef struct VsyncTimingWork {
    uint32_t calls;
    uint32_t reqs;
    uint32_t merged;
    uint32_t dirty_count;
    uint64_t bytes_copied;
} VsyncTimingWork;

typedef struct VsyncTimingStats {
    float calls;
    float reqs;
    float merged;
    float dirty_count;
    float bytes_kb;
} VsyncTimingStats;

typedef struct SurfTimingWork {
    int64_t populate_ns;
    int64_t dirty_ns;
    int64_t enrp_ns;
    int64_t lk_hit_ns;
    int64_t lk_evict_ns;
    int64_t lk_nosurf_ns;
    int64_t create_ns;
    int64_t put_ns;
    int64_t bind_ns;
    int64_t upload_ns;
    int64_t download_ns;
    int64_t expire_ns;
    int64_t df_flush_ns;
    int64_t df_read_ns;
    uint32_t update_calls;
    uint32_t create_count;
    uint32_t hit_count;
    uint32_t evict_count;
    uint32_t upload_count;
    uint32_t download_count;
    uint32_t miss_count;
} SurfTimingWork;

typedef struct SurfTimingStats {
    float populate_ms;
    float dirty_ms;
    float enrp_ms;
    float lk_hit_ms;
    float lk_evict_ms;
    float lk_nosurf_ms;
    float create_ms;
    float put_ms;
    float bind_ms;
    float upload_ms;
    float download_ms;
    float expire_ms;
    float df_flush_ms;
    float df_read_ms;
    float update_calls;
    float create_count;
    float hit_count;
    float evict_count;
    float upload_count;
    float download_count;
    float miss_count;
} SurfTimingStats;

typedef struct NV2AStats {
    int64_t last_flip_time;
    unsigned int frame_count;
    unsigned int increment_fps;
    struct {
        int mspf;
        int counters[NV2A_PROF__COUNT];
    } frame_working, frame_history[NV2A_PROF_NUM_FRAMES];
    unsigned int frame_ptr;
    FramePacingStats pacing;
    ShaderPipelineStats shader_stats;
    FramePhaseTimingWork phase_working;
    FramePhaseTimingStats phase;
    CpuTimingWork cpu_working;
    CpuTimingStats cpu;
    VsyncTimingWork vsync_working;
    VsyncTimingStats vsync;
    SurfTimingWork surf_working;
    SurfTimingStats surf;
} NV2AStats;

#ifdef __cplusplus
extern "C" {
#endif

extern NV2AStats g_nv2a_stats;

const char *nv2a_profile_get_counter_name(unsigned int cnt);
int nv2a_profile_get_counter_value(unsigned int cnt);
void nv2a_profile_increment(void);
void nv2a_profile_flip_stall(void);
void nv2a_profile_get_pacing_str(char *buf, int bufsize);
void nv2a_profile_get_shader_stats_str(char *buf, int bufsize);
void nv2a_profile_get_phase_timing_str(char *buf, int bufsize);
void nv2a_profile_get_cpu_timing_str(char *buf, int bufsize);
void nv2a_profile_get_vsync_timing_str(char *buf, int bufsize);
void nv2a_profile_get_surf_timing_str(char *buf, int bufsize);
void nv2a_profile_get_workload_str(char *buf, int bufsize);

#ifndef NV2A_PERF_LOG
#define NV2A_PERF_LOG 0
#endif

/*
 * Fast nanosecond clock for per-frame phase timing.
 * On ARM64, reads the generic timer directly via cntvct_el0.
 * A fixed-point multiplier (precomputed from cntfrq_el0) converts ticks
 * to nanoseconds with a single multiply + shift, avoiding a costly
 * 64-bit udiv on every call.
 */
#if defined(__aarch64__)
static inline int64_t nv2a_clock_ns(void)
{
    static uint64_t ns_mult;
    static unsigned int ns_shift;
    if (__builtin_expect(!ns_mult, 0)) {
        uint64_t freq;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        ns_shift = 32;
        ns_mult = ((uint64_t)1000000000ULL << ns_shift) / freq;
    }
    uint64_t cnt;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    return (int64_t)(((__uint128_t)cnt * ns_mult) >> ns_shift);
}
#else
static inline int64_t nv2a_clock_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}
#endif

#if NV2A_PERF_LOG

static inline void nv2a_profile_inc_counter(enum NV2A_PROF_COUNTERS_ENUM cnt)
{
    g_nv2a_stats.frame_working.counters[cnt] += 1;
}

#define NV2A_PHASE_TIMER_BEGIN(phase) \
    int64_t _phase_t0_##phase = nv2a_clock_ns()

#define NV2A_PHASE_TIMER_END(phase) do { \
    g_nv2a_stats.phase_working.phase##_ns += \
        nv2a_clock_ns() - _phase_t0_##phase; \
} while (0)

#else /* !NV2A_PERF_LOG */

static inline void nv2a_profile_inc_counter(enum NV2A_PROF_COUNTERS_ENUM cnt)
{
    (void)cnt;
}

#define NV2A_PHASE_TIMER_BEGIN(phase) do { } while (0)
#define NV2A_PHASE_TIMER_END(phase)  do { } while (0)

#endif /* NV2A_PERF_LOG */

void nv2a_dbg_renderdoc_init(void);
void *nv2a_dbg_renderdoc_get_api(void);
bool nv2a_dbg_renderdoc_available(void);
void nv2a_dbg_renderdoc_capture_frames(int num_frames, bool trace);
extern int renderdoc_capture_frames;
extern bool renderdoc_trace_frames;

void nv2a_dbg_set_rt_dump_path(const char *dir);
void nv2a_dbg_trigger_rt_dump(void);

void nv2a_dbg_trigger_diag_frame(void);
bool nv2a_dbg_diag_frame_active(void);

typedef struct NV2AState NV2AState;
typedef struct PGRAPHState PGRAPHState;

void nv2a_diag_log_draw_call(NV2AState *d, PGRAPHState *pg,
                             const char *type, int count);

#ifdef __cplusplus
}
#endif

#endif
