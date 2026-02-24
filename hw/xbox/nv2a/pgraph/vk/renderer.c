/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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
#include "renderer.h"
#include "ui/xemu-settings.h"

#include "gloffscreen.h"

#include <sys/stat.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

typedef struct {
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint8_t  pipeline_cache_uuid[VK_UUID_SIZE];
} GpuDriverIdentity;

static void remove_directory_recursive(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        return;
    }
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            remove_directory_recursive(child);
        } else {
            unlink(child);
        }
        g_free(child);
    }
    g_dir_close(dir);
    rmdir(path);
}

static void check_driver_identity_and_wipe_caches(PGRAPHVkState *r)
{
    if (!g_config.perf.cache_shaders) {
        return;
    }

    const char *base = xemu_settings_get_base_path();
    char *id_path = g_strdup_printf("%sgpu_driver_id.bin", base);

    GpuDriverIdentity current;
    current.vendor_id = r->device_props.vendorID;
    current.device_id = r->device_props.deviceID;
    current.driver_version = r->device_props.driverVersion;
    memcpy(current.pipeline_cache_uuid, r->device_props.pipelineCacheUUID,
           VK_UUID_SIZE);

    bool match = false;
    gchar *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(id_path, &data, &len, NULL) &&
        len == sizeof(GpuDriverIdentity)) {
        match = memcmp(data, &current, sizeof(GpuDriverIdentity)) == 0;
    }
    g_free(data);

    if (!match) {
        char *spv_dir = g_strdup_printf("%sspv_cache", base);
        char *plc_path = g_strdup_printf("%svk_pipeline_cache.bin", base);

        VK_LOG("Driver changed -- wiping shader and pipeline caches");
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk",
            "Driver identity mismatch: wiping spv_cache and pipeline cache "
            "(vendor=%04x device=%04x driverVer=%08x)",
            current.vendor_id, current.device_id, current.driver_version);
#else
        fprintf(stderr, "xemu-vk: Driver identity mismatch: wiping caches "
                "(vendor=%04x device=%04x driverVer=%08x)\n",
                current.vendor_id, current.device_id, current.driver_version);
#endif

        remove_directory_recursive(spv_dir);
        unlink(plc_path);
        g_free(spv_dir);
        g_free(plc_path);

        g_file_set_contents(id_path, (const gchar *)&current,
                            sizeof(GpuDriverIdentity), NULL);
    }

    g_free(id_path);
}

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
    /*
     * On Android, only cache EGL share/config on the SDL thread here.
     * Create/bind the offscreen context later on the renderer thread.
     */
    glo_android_cache_current_egl_state();
#else
    g_gl_context = glo_context_create();
#endif
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));

#if HAVE_EXTERNAL_MEMORY
    bool use_external_memory = false;
#ifdef __ANDROID__
    if (!g_gl_context) {
        // On Android, we need to ensure the main GL context is accessible
        // for context sharing. The EGL state should already be cached.
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "pgraph_vk_init: creating GL offscreen context");
        g_gl_context = glo_context_create();
        if (!g_gl_context) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "pgraph_vk_init: glo_context_create failed");
        }
    }
#endif
    if (g_gl_context) {
        glo_set_current(g_gl_context);
        use_external_memory = pgraph_vk_gl_external_memory_available();
    }
    if (!use_external_memory) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                            "pgraph_vk_init: external memory interop unavailable, using download fallback");
#endif
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "pgraph_vk_init: external memory interop=%s",
                        use_external_memory ? "enabled" : "disabled");
#endif
    pg->vk_renderer_state->display.use_external_memory = use_external_memory;
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    check_driver_identity_and_wipe_caches(pg->vk_renderer_state);

    VK_LOG("init: command_buffers");
    pgraph_vk_init_command_buffers(pg);
    VK_LOG("init: buffers");
    if (!pgraph_vk_init_buffers(d, errp)) {
        VK_LOG_ERROR("init: buffers FAILED");
        return;
    }
    VK_LOG("init: surfaces");
    pgraph_vk_init_surfaces(pg);
    VK_LOG("init: shaders");
    pgraph_vk_init_shaders(pg);
    VK_LOG("init: pipelines");
    pgraph_vk_init_pipelines(pg);
    VK_LOG("init: textures");
    pgraph_vk_init_textures(pg);
    VK_LOG("init: reports");
    pgraph_vk_init_reports(pg);
    VK_LOG("init: compute");
    pgraph_vk_init_compute(pg);
    VK_LOG("init: display");
    pgraph_vk_init_display(pg);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    VK_LOG("init: renderer_ready");

}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;

#if HAVE_EXTERNAL_MEMORY
    if (g_gl_context) {
        glo_context_destroy(g_gl_context);
        g_gl_context = NULL;
    }
#endif
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
#if HAVE_EXTERNAL_MEMORY
    if (pg->vk_renderer_state->display.use_external_memory) {
        pgraph_vk_render_display(pg);
    }
#else
    pgraph_vk_render_display(pg);
#endif

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
    pgraph_vk_debug_frame_terminator();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    // qatomic_set(&d->pgraph.vk_renderer_state->shader_cache_writeback_pending, true);
    // qemu_event_reset(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    // qemu_event_wait(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);   
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

#if HAVE_EXTERNAL_MEMORY
    if (r->display.use_external_memory) {
        qemu_event_reset(&d->pgraph.sync_complete);
        qatomic_set(&pg->sync_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.sync_complete);
        return r->display.gl_texture_id;
    }
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#else
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_vk_wait_for_surface_download(surface);
    return 0;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_force_register(void)
{
    static bool registered = false;
    if (registered) {
        return;
    }
    pgraph_renderer_register(&pgraph_vk_renderer);
    registered = true;
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}
