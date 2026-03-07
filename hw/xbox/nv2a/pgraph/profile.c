/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef XBOX
extern uint64_t tb_cache_stats_lookup_hits;
extern uint64_t tb_cache_stats_lookup_misses;
#endif

NV2AStats g_nv2a_stats;

void nv2a_profile_increment(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

static void snapshot_phase_timing(void)
{
    FramePhaseTimingWork *w = &g_nv2a_stats.phase_working;
    FramePhaseTimingStats *p = &g_nv2a_stats.phase;
    const float alpha = 0.2f;

#define SMOOTH(field) \
    p->field##_ms = p->field##_ms * (1.0f - alpha) + \
                    (float)(w->field##_ns) / 1e6f * alpha

    SMOOTH(surface_update);
    SMOOTH(texture_upload);
    SMOOTH(shader_compile);
    SMOOTH(draw_dispatch);
    SMOOTH(finish);
    SMOOTH(flip_idle);
    SMOOTH(fifo_idle);
    SMOOTH(fifo_idle_frame);
    SMOOTH(fifo_idle_starve);
    SMOOTH(draw_vtx_attr);
    SMOOTH(draw_vtx_sync);
    SMOOTH(draw_prim_rw);
    SMOOTH(draw_pipeline);
    SMOOTH(draw_desc_set);
    SMOOTH(draw_setup);
    SMOOTH(draw_vk_cmd);
    SMOOTH(pipe_bind_tex);
    SMOOTH(pipe_bind_shd);
    SMOOTH(pipe_lookup);
    SMOOTH(finish_fence);
    SMOOTH(finish_submit);
#undef SMOOTH

    p->total_ms = p->surface_update_ms + p->texture_upload_ms +
                  p->shader_compile_ms + p->draw_dispatch_ms +
                  p->finish_ms + p->flip_idle_ms + p->fifo_idle_ms;

    bool saved_post_flip = w->post_flip;
    memset(w, 0, sizeof(*w));
    w->post_flip = saved_post_flip;
}

static void snapshot_cpu_timing(void)
{
    CpuTimingWork *w = &g_nv2a_stats.cpu_working;
    CpuTimingStats *p = &g_nv2a_stats.cpu;
    const float alpha = 0.2f;

#define SMOOTH_MS(dst, src_ns) \
    (dst) = (dst) * (1.0f - alpha) + (float)(src_ns) / 1e6f * alpha

    SMOOTH_MS(p->lock_wait_ms, w->lock_wait_ns);
    SMOOTH_MS(p->pusher_run_ms, w->pusher_run_ns);
    SMOOTH_MS(p->method_exec_ms, w->method_exec_ns);
    SMOOTH_MS(p->puller_total_ms, w->puller_total_ns);
    SMOOTH_MS(p->puller_lock_ms, w->puller_lock_ns);
    SMOOTH_MS(p->puller_method_ms, w->puller_method_ns);

#undef SMOOTH_MS

#define SMOOTH_CNT(dst, src) \
    (dst) = (dst) * (1.0f - alpha) + (float)(src) * alpha

    SMOOTH_CNT(p->kick_count, w->kick_count);
    SMOOTH_CNT(p->kick_count_spun, w->kick_count_spun);
    SMOOTH_CNT(p->pusher_words, w->pusher_words);
    SMOOTH_CNT(p->method_count, w->method_count);
    SMOOTH_CNT(p->method_fast_hit, w->method_fast_hit);
    SMOOTH_CNT(p->method_noninc_words, w->method_noninc_words);

#undef SMOOTH_CNT

#ifdef XBOX
    {
        uint64_t cur_hits   = tb_cache_stats_lookup_hits;
        uint64_t cur_misses = tb_cache_stats_lookup_misses;
        uint64_t dh = cur_hits   - w->tb_hits_snap;
        uint64_t dm = cur_misses - w->tb_misses_snap;
        float frame_pct = (dh + dm) > 0
                          ? (float)dh / (float)(dh + dm) * 100.0f
                          : 100.0f;
        p->tb_hit_pct = p->tb_hit_pct * (1.0f - alpha) + frame_pct * alpha;
        w->tb_hits_snap   = cur_hits;
        w->tb_misses_snap = cur_misses;
    }
#endif

    w->lock_wait_ns      = 0;
    w->pusher_run_ns     = 0;
    w->method_exec_ns    = 0;
    w->puller_total_ns   = 0;
    w->puller_lock_ns    = 0;
    w->puller_method_ns  = 0;
    w->kick_count        = 0;
    w->kick_count_spun   = 0;
    w->pusher_words      = 0;
    w->method_count      = 0;
    w->method_fast_hit   = 0;
    w->method_noninc_words = 0;
}

static void snapshot_vsync_timing(void)
{
    VsyncTimingWork *w = &g_nv2a_stats.vsync_working;
    VsyncTimingStats *p = &g_nv2a_stats.vsync;
    const float alpha = 0.2f;

#define SMOOTH_CNT(dst, src) \
    (dst) = (dst) * (1.0f - alpha) + (float)(src) * alpha

    SMOOTH_CNT(p->calls, w->calls);
    SMOOTH_CNT(p->reqs, w->reqs);
    SMOOTH_CNT(p->merged, w->merged);
    SMOOTH_CNT(p->dirty_count, w->dirty_count);
    SMOOTH_CNT(p->bytes_kb, w->bytes_copied / 1024.0f);

#undef SMOOTH_CNT

    memset(w, 0, sizeof(*w));
}

static void snapshot_surf_timing(void)
{
    SurfTimingWork *w = &g_nv2a_stats.surf_working;
    SurfTimingStats *p = &g_nv2a_stats.surf;
    const float alpha = 0.2f;

#define SMOOTH_MS(dst, src_ns) \
    (dst) = (dst) * (1.0f - alpha) + ((float)(src_ns) / 1e6f) * alpha
#define SMOOTH_CNT(dst, src) \
    (dst) = (dst) * (1.0f - alpha) + (float)(src) * alpha

    SMOOTH_MS(p->populate_ms, w->populate_ns);
    SMOOTH_MS(p->dirty_ms, w->dirty_ns);
    SMOOTH_MS(p->enrp_ms, w->enrp_ns);
    SMOOTH_MS(p->lk_hit_ms, w->lk_hit_ns);
    SMOOTH_MS(p->lk_evict_ms, w->lk_evict_ns);
    SMOOTH_MS(p->lk_nosurf_ms, w->lk_nosurf_ns);
    SMOOTH_MS(p->create_ms, w->create_ns);
    SMOOTH_MS(p->put_ms, w->put_ns);
    SMOOTH_MS(p->bind_ms, w->bind_ns);
    SMOOTH_MS(p->upload_ms, w->upload_ns);
    SMOOTH_MS(p->download_ms, w->download_ns);
    SMOOTH_MS(p->expire_ms, w->expire_ns);
    SMOOTH_MS(p->df_flush_ms, w->df_flush_ns);
    SMOOTH_MS(p->df_read_ms, w->df_read_ns);
    SMOOTH_CNT(p->update_calls, w->update_calls);
    SMOOTH_CNT(p->create_count, w->create_count);
    SMOOTH_CNT(p->hit_count, w->hit_count);
    SMOOTH_CNT(p->evict_count, w->evict_count);
    SMOOTH_CNT(p->upload_count, w->upload_count);
    SMOOTH_CNT(p->download_count, w->download_count);
    SMOOTH_CNT(p->miss_count, w->miss_count);

#undef SMOOTH_MS
#undef SMOOTH_CNT

    memset(w, 0, sizeof(*w));
}

void nv2a_profile_flip_stall(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t render_time = (now-g_nv2a_stats.last_flip_time)/1000;

    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));

    snapshot_phase_timing();
    snapshot_cpu_timing();
    snapshot_vsync_timing();
    snapshot_surf_timing();

    g_nv2a_stats.phase_working.post_flip = true;

    /* Track game frame time (flip-to-flip interval) */
    static int64_t prev_flip_us;
    if (prev_flip_us) {
        float frame_ms = (float)(now - prev_flip_us) / 1000.0f;
        FramePacingStats *p = &g_nv2a_stats.pacing;
        p->game_frame_ms = p->game_frame_ms * 0.8f + frame_ms * 0.2f;
        if (frame_ms < p->game_frame_min_ms || p->game_frame_min_ms == 0)
            p->game_frame_min_ms = frame_ms;
        if (frame_ms > p->game_frame_max_ms)
            p->game_frame_max_ms = frame_ms;
    }
    prev_flip_us = now;

#if defined(__ANDROID__) && NV2A_PERF_LOG
    if ((g_nv2a_stats.frame_count % 60) == 0) {
        char buf[512];
        nv2a_profile_get_phase_timing_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-phase", "%s", buf);
        nv2a_profile_get_cpu_timing_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-cpu", "%s", buf);
        nv2a_profile_get_vsync_timing_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-vsync", "%s", buf);
        nv2a_profile_get_surf_timing_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-surf", "%s", buf);
        nv2a_profile_get_pacing_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-pace", "%s", buf);
        nv2a_profile_get_workload_str(buf, sizeof(buf));
        __android_log_print(ANDROID_LOG_INFO, "xemu-work", "%s", buf);
    }
#endif
}

void nv2a_profile_get_pacing_str(char *buf, int bufsize)
{
    FramePacingStats *p = &g_nv2a_stats.pacing;
    snprintf(buf, bufsize,
             "G:%.1f(%.1f-%.1f) D:%.1f(%.1f-%.1f) S:%.1f J:%.1f Df:%u Vd:%.1f Ul:%c",
             p->game_frame_ms,
             p->game_frame_min_ms,
             p->game_frame_max_ms,
             p->display_frame_ms,
             p->display_frame_min_ms,
             p->display_frame_max_ms,
             p->swap_ms,
             p->vblank_jitter_ms,
             p->defers_total,
             p->vblank_delivery_ms,
             p->unlock_mode_active ? 'Y' : 'N');
    /* Reset min/max every call so the window reflects recent behavior */
    p->game_frame_min_ms = 0;
    p->game_frame_max_ms = 0;
    p->display_frame_min_ms = 0;
    p->display_frame_max_ms = 0;
    p->defers_total = 0;
}

void nv2a_profile_get_phase_timing_str(char *buf, int bufsize)
{
    FramePhaseTimingStats *p = &g_nv2a_stats.phase;
    snprintf(buf, bufsize,
             "Surf:%.1f Tex:%.1f Shd:%.1f Draw:%.1f "
             "[Vtx:%.1f Syn:%.1f Prw:%.1f Pipe:%.1f(Tx:%.1f Sh:%.1f Lu:%.1f) "
             "Desc:%.1f Setup:%.1f Cmd:%.1f] "
             "Fin:%.1f(Sub:%.1f Fen:%.1f) Flip:%.1f Idle:%.1f(Fr:%.1f St:%.1f) | Tot:%.1f ms",
             p->surface_update_ms,
             p->texture_upload_ms,
             p->shader_compile_ms,
             p->draw_dispatch_ms,
             p->draw_vtx_attr_ms,
             p->draw_vtx_sync_ms,
             p->draw_prim_rw_ms,
             p->draw_pipeline_ms,
             p->pipe_bind_tex_ms,
             p->pipe_bind_shd_ms,
             p->pipe_lookup_ms,
             p->draw_desc_set_ms,
             p->draw_setup_ms,
             p->draw_vk_cmd_ms,
             p->finish_ms,
             p->finish_submit_ms,
             p->finish_fence_ms,
             p->flip_idle_ms,
             p->fifo_idle_ms,
             p->fifo_idle_frame_ms,
             p->fifo_idle_starve_ms,
             p->total_ms);
}

void nv2a_profile_get_cpu_timing_str(char *buf, int bufsize)
{
    CpuTimingStats *p = &g_nv2a_stats.cpu;
    float spin_pct = p->kick_count > 0
                     ? p->kick_count_spun / p->kick_count * 100.0f
                     : 0.0f;
    float parse_ms = p->pusher_run_ms - p->puller_total_ms;
    snprintf(buf, bufsize,
             "CPU: K:%.0f W:%.1fK M:%.0f(Fh:%.0f Ni:%.0f) "
             "Lock:%.1fms Push:%.1fms "
             "[Prs:%.1f Plr:%.1f(Lk:%.1f Mth:%.1f)] "
             "SpH:%.0f%% TbH:%.1f%%",
             p->kick_count,
             p->pusher_words / 1000.0f,
             p->method_count,
             p->method_fast_hit,
             p->method_noninc_words,
             p->lock_wait_ms,
             p->pusher_run_ms,
             parse_ms,
             p->puller_total_ms,
             p->puller_lock_ms,
             p->puller_method_ms,
             spin_pct,
             p->tb_hit_pct);
}

void nv2a_profile_get_vsync_timing_str(char *buf, int bufsize)
{
    VsyncTimingStats *p = &g_nv2a_stats.vsync;
    snprintf(buf, bufsize,
             "Vsyn: C:%.0f R:%.0f M:%.0f D:%.0f %.0fKB",
             p->calls, p->reqs, p->merged, p->dirty_count, p->bytes_kb);
}

void nv2a_profile_get_surf_timing_str(char *buf, int bufsize)
{
    SurfTimingStats *p = &g_nv2a_stats.surf;
    snprintf(buf, bufsize,
             "Srf: C:%.0f pop:%.1f drty:%.1f enrp:%.1f "
             "lkH:%.1f lkE:%.1f lkN:%.1f "
             "cr:%.1f put:%.1f bnd:%.1f upl:%.1f dl:%.1f exp:%.1f "
             "dfF:%.1f dfR:%.1f "
             "| #cr:%.0f #hit:%.0f #ev:%.0f #upl:%.0f #dl:%.0f #miss:%.0f",
             p->update_calls,
             p->populate_ms, p->dirty_ms, p->enrp_ms,
             p->lk_hit_ms, p->lk_evict_ms, p->lk_nosurf_ms,
             p->create_ms, p->put_ms, p->bind_ms,
             p->upload_ms, p->download_ms, p->expire_ms,
             p->df_flush_ms, p->df_read_ms,
             p->create_count, p->hit_count, p->evict_count,
             p->upload_count, p->download_count, p->miss_count);
}

void nv2a_profile_get_shader_stats_str(char *buf, int bufsize)
{
    ShaderPipelineStats *s = &g_nv2a_stats.shader_stats;
    snprintf(buf, bufsize,
             "P:%u/%u S:%u/%u SPV:%u/%u L:%s W:%u",
             s->pipeline_cache_hits,
             s->pipeline_cache_hits + s->pipeline_cache_misses,
             s->shader_cache_hits,
             s->shader_cache_hits + s->shader_cache_misses,
             s->spv_cache_hits,
             s->spv_cache_hits + s->spv_cache_misses,
             s->pipeline_cache_disk_loaded ? "Y" : "N",
             s->pipeline_cache_disk_saved);
}

void nv2a_profile_get_workload_str(char *buf, int bufsize)
{
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    int *c = g_nv2a_stats.frame_history[idx].counters;
    snprintf(buf, bufsize,
             "BE:%d DA:%d IE:%d IB:%d IA:%d Clr:%d "
             "QS:%d/%d PGen:%d PBnd:%d PNd:%d RP:%d "
             "SGen:%d SBnd:%d SNd:%d UBOd:%d UBOn:%d "
             "TexU:%d GBU:%d/%d/%d/%d/%d "
             "Fin:Vbd%d Sc%d Sd%d Bs%d Fbd%d Pr%d Fl%d Flu%d St%d",
             c[NV2A_PROF_BEGIN_ENDS],
             c[NV2A_PROF_DRAW_ARRAYS],
             c[NV2A_PROF_INLINE_ELEMENTS],
             c[NV2A_PROF_INLINE_BUFFERS],
             c[NV2A_PROF_INLINE_ARRAYS],
             c[NV2A_PROF_CLEAR],
             c[NV2A_PROF_QUEUE_SUBMIT],
             c[NV2A_PROF_QUEUE_SUBMIT_AUX],
             c[NV2A_PROF_PIPELINE_GEN],
             c[NV2A_PROF_PIPELINE_BIND],
             c[NV2A_PROF_PIPELINE_NOTDIRTY],
             c[NV2A_PROF_PIPELINE_RENDERPASSES],
             c[NV2A_PROF_SHADER_GEN],
             c[NV2A_PROF_SHADER_BIND],
             c[NV2A_PROF_SHADER_BIND_NOTDIRTY],
             c[NV2A_PROF_SHADER_UBO_DIRTY],
             c[NV2A_PROF_SHADER_UBO_NOTDIRTY],
             c[NV2A_PROF_TEX_UPLOAD],
             c[NV2A_PROF_GEOM_BUFFER_UPDATE_1],
             c[NV2A_PROF_GEOM_BUFFER_UPDATE_2],
             c[NV2A_PROF_GEOM_BUFFER_UPDATE_3],
             c[NV2A_PROF_GEOM_BUFFER_UPDATE_4],
             c[NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY],
             c[NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY],
             c[NV2A_PROF_FINISH_SURFACE_CREATE],
             c[NV2A_PROF_FINISH_SURFACE_DOWN],
             c[NV2A_PROF_FINISH_NEED_BUFFER_SPACE],
             c[NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY],
             c[NV2A_PROF_FINISH_PRESENTING],
             c[NV2A_PROF_FINISH_FLIP_STALL],
             c[NV2A_PROF_FINISH_FLUSH],
             c[NV2A_PROF_FINISH_STALLED]);
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
