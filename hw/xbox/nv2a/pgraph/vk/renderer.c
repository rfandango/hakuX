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
#include "qemu/error-report.h"
#include "ui/xemu-settings.h"
#ifdef __ANDROID__
#include <android/log.h>
#define DBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xemu-vk-dbg", __VA_ARGS__)
#else
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

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

        char *smk_path = g_strdup_printf("%sshader_module_keys.bin", base);
        remove_directory_recursive(spv_dir);
        unlink(plc_path);
        unlink(smk_path);
        g_free(spv_dir);
        g_free(plc_path);
        g_free(smk_path);

        g_file_set_contents(id_path, (const gchar *)&current,
                            sizeof(GpuDriverIdentity), NULL);
    }

    g_free(id_path);
}

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#endif

void pgraph_vk_gl_make_context_current(void)
{
#if HAVE_EXTERNAL_MEMORY
    if (!g_gl_context) {
        g_gl_context = glo_context_create();
    }
    if (g_gl_context) {
        glo_set_current(g_gl_context);
    }
#endif
}

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
    pg->vk_renderer_state->need_descriptor_rebind = true;

#if HAVE_EXTERNAL_MEMORY
    bool use_external_memory = pgraph_vk_gl_external_memory_available();
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

    VK_LOG_ERROR("init: command_buffers");
    pgraph_vk_init_command_buffers(pg);
    VK_LOG_ERROR("init: buffers");
    if (!pgraph_vk_init_buffers(d, errp)) {
        VK_LOG_ERROR("init: buffers FAILED");
        return;
    }
    VK_LOG_ERROR("init: surfaces");
    pgraph_vk_init_surfaces(pg);
    VK_LOG_ERROR("init: shaders");
    pgraph_vk_init_shaders(pg);
    VK_LOG_ERROR("init: pipelines");
    pgraph_vk_init_pipelines(pg);
    VK_LOG_ERROR("init: textures");
    pgraph_vk_init_textures(pg);
    VK_LOG_ERROR("init: reports");
    pgraph_vk_init_reports(pg);
    VK_LOG_ERROR("init: compute");
    pgraph_vk_init_compute(pg);
    VK_LOG_ERROR("init: display");
    pgraph_vk_init_display(pg);

    pg->vk_renderer_state->vram_ram_addr = memory_region_get_ram_addr(d->vram);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    VK_LOG_ERROR("init: renderer_ready");

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

static char rt_dump_dir[512] = "";
static volatile int rt_dump_pending = 0;

void nv2a_dbg_set_rt_dump_path(const char *dir)
{
    snprintf(rt_dump_dir, sizeof(rt_dump_dir), "%s", dir);
}

void nv2a_dbg_trigger_rt_dump(void)
{
    qatomic_set(&rt_dump_pending, 1);
}

static void dump_surface_ppm(NV2AState *d, SurfaceBinding *surface,
                             const char *path)
{
    unsigned int w = surface->width;
    unsigned int h = surface->height;
    unsigned int bpp = surface->fmt.bytes_per_pixel;
    unsigned int pitch = surface->pitch;

    if (!w || !h || !bpp) {
        return;
    }

    const uint8_t *vram = d->vram_ptr + surface->vram_addr;

    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }

    fprintf(f, "P6\n%u %u\n255\n", w, h);

    for (unsigned int y = 0; y < h; y++) {
        const uint8_t *row = vram + y * pitch;
        for (unsigned int x = 0; x < w; x++) {
            uint8_t rgb[3];
            if (bpp == 4) {
                rgb[0] = row[x * 4 + 2];
                rgb[1] = row[x * 4 + 1];
                rgb[2] = row[x * 4 + 0];
            } else if (bpp == 2) {
                uint16_t px = *(const uint16_t *)(row + x * 2);
                rgb[0] = (px >> 8) & 0xF8;
                rgb[1] = (px >> 3) & 0xFC;
                rgb[2] = (px << 3) & 0xF8;
            } else {
                rgb[0] = rgb[1] = rgb[2] = row[x * bpp];
            }
            fwrite(rgb, 1, 3, f);
        }
    }

    fclose(f);
}

static void maybe_dump_render_target(NV2AState *d)
{
    if (!qatomic_read(&rt_dump_pending)) {
        return;
    }
    qatomic_set(&rt_dump_pending, 0);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface = r->color_binding;

    if (!surface || !surface->color) {
        fprintf(stderr, "rt_dump: no color surface bound\n");
        return;
    }

    if (!surface->width || !surface->height || !surface->fmt.bytes_per_pixel) {
        fprintf(stderr, "rt_dump: invalid surface dimensions\n");
        return;
    }

    char path[600];
    if (rt_dump_dir[0]) {
        mkdir(rt_dump_dir, 0755);
        snprintf(path, sizeof(path), "%s/rt_dump_%u.ppm", rt_dump_dir,
                 g_nv2a_stats.frame_count);
    } else {
        snprintf(path, sizeof(path), "/tmp/rt_dump_%u.ppm",
                 g_nv2a_stats.frame_count);
    }

    dump_surface_ppm(d, surface, path);
    fprintf(stderr, "rt_dump: saved %ux%u to %s\n",
            surface->width, surface->height, path);
}

/*
 * Per-draw-call diagnostic frame capture
 */

#define DIAG_MAX_DRAWS 4096
#define DIAG_JSON_INITIAL_CAP (256 * 1024)

static volatile int diag_frame_pending = 0;
static volatile int diag_frame_active = 0;
static int diag_draw_index = 0;
static unsigned int diag_frame_num = 0;

static char *diag_json_buf = NULL;
static size_t diag_json_len = 0;
static size_t diag_json_cap = 0;

void nv2a_dbg_trigger_diag_frame(void)
{
    qatomic_set(&diag_frame_pending, 1);
}

bool nv2a_dbg_diag_frame_active(void)
{
    return qatomic_read(&diag_frame_active) != 0;
}

static void diag_json_ensure(size_t needed)
{
    if (diag_json_len + needed <= diag_json_cap) {
        return;
    }
    size_t new_cap = diag_json_cap ? diag_json_cap * 2 : DIAG_JSON_INITIAL_CAP;
    while (new_cap < diag_json_len + needed) {
        new_cap *= 2;
    }
    diag_json_buf = g_realloc(diag_json_buf, new_cap);
    diag_json_cap = new_cap;
}

static void diag_json_append(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        diag_json_ensure((size_t)n + 1);
        vsnprintf(diag_json_buf + diag_json_len, (size_t)n + 1, fmt, ap2);
        diag_json_len += (size_t)n;
    }
    va_end(ap2);
}

static void diag_write_json(void)
{
    char dir[600];
    if (rt_dump_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", rt_dump_dir);
    } else {
        snprintf(dir, sizeof(dir), "/tmp");
    }
    mkdir(dir, 0755);

    char path[700];
    snprintf(path, sizeof(path), "%s/diag_frame_%u.json", dir, diag_frame_num);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "diag: cannot write %s\n", path);
        return;
    }
    if (diag_json_buf && diag_json_len > 0) {
        fwrite(diag_json_buf, 1, diag_json_len, f);
    }
    fclose(f);
    fprintf(stderr, "diag: wrote %zu bytes to %s\n", diag_json_len, path);
}

static const char *diag_stencil_op_name(uint32_t op)
{
    switch (op) {
    case 0x1: return "KEEP";
    case 0x2: return "ZERO";
    case 0x3: return "REPLACE";
    case 0x4: return "INCRSAT";
    case 0x5: return "DECRSAT";
    case 0x6: return "INVERT";
    case 0x7: return "INCR";
    case 0x8: return "DECR";
    default:  return "?";
    }
}

static const char *diag_compare_func_name(uint32_t f)
{
    switch (f) {
    case 0: return "NEVER";
    case 1: return "LESS";
    case 2: return "EQUAL";
    case 3: return "LEQUAL";
    case 4: return "GREATER";
    case 5: return "NOTEQUAL";
    case 6: return "GEQUAL";
    case 7: return "ALWAYS";
    default: return "?";
    }
}

static const char *diag_blend_factor_name(uint32_t f)
{
    switch (f) {
    case 0:  return "ZERO";
    case 1:  return "ONE";
    case 2:  return "SRC_COLOR";
    case 3:  return "INV_SRC_COLOR";
    case 4:  return "SRC_ALPHA";
    case 5:  return "INV_SRC_ALPHA";
    case 6:  return "DST_ALPHA";
    case 7:  return "INV_DST_ALPHA";
    case 8:  return "DST_COLOR";
    case 9:  return "INV_DST_COLOR";
    case 10: return "SRC_ALPHA_SAT";
    case 12: return "CONST_COLOR";
    case 13: return "INV_CONST_COLOR";
    case 14: return "CONST_ALPHA";
    case 15: return "INV_CONST_ALPHA";
    default: return "?";
    }
}

static const char *diag_blend_eq_name(uint32_t eq)
{
    switch (eq) {
    case 0: return "SUB";
    case 1: return "REV_SUB";
    case 2: return "ADD";
    case 3: return "MIN";
    case 4: return "MAX";
    case 5: return "REV_SUB";
    case 6: return "ADD";
    default: return "?";
    }
}

static const char *diag_prim_name(int mode)
{
    switch (mode) {
    case 1: return "POINTS";
    case 2: return "LINES";
    case 3: return "LINE_LOOP";
    case 4: return "LINE_STRIP";
    case 5: return "TRIANGLES";
    case 6: return "TRIANGLE_STRIP";
    case 7: return "TRIANGLE_FAN";
    case 8: return "QUADS";
    case 9: return "QUAD_STRIP";
    case 10: return "POLYGON";
    default: return "?";
    }
}

void nv2a_diag_log_draw_call(NV2AState *d, PGRAPHState *pg,
                             const char *type, int count)
{
    if (!qatomic_read(&diag_frame_active)) {
        return;
    }

    if (diag_draw_index >= DIAG_MAX_DRAWS) {
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    int idx = diag_draw_index++;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    uint32_t blend_reg = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);

    bool blend_en    = blend_reg & NV_PGRAPH_BLEND_EN;
    uint32_t sfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
    uint32_t dfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
    uint32_t blend_eq = GET_MASK(blend_reg, NV_PGRAPH_BLEND_EQN);

    bool depth_test  = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    uint32_t zfunc   = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);

    bool stencil_test = control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    uint32_t stencil_func = GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
    uint32_t stencil_ref  = GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
    uint32_t stencil_mask_read  = GET_MASK(control_1,
                                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
    uint32_t stencil_mask_write = GET_MASK(control_1,
                                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    uint32_t op_fail  = GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
    uint32_t op_zfail = GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
    uint32_t op_zpass = GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

    bool mask_r = !!(control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE);
    bool mask_g = !!(control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE);
    bool mask_b = !!(control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool mask_a = !!(control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE);

    bool cull_en = !!(setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE);
    uint32_t cull_face = GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
    bool front_ccw = !!(setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE);

    const char *cull_str = "NONE";
    if (cull_en) {
        switch (cull_face) {
        case 1: cull_str = "FRONT"; break;
        case 2: cull_str = "BACK";  break;
        case 3: cull_str = "BOTH";  break;
        }
    }

    diag_json_append(
        "%s  {\n"
        "    \"draw_index\": %d,\n"
        "    \"type\": \"%s\",\n"
        "    \"count\": %d,\n"
        "    \"primitive_mode\": \"%s\",\n"
        "    \"blend\": {"
            "\"enabled\": %s, \"src\": \"%s\", \"dst\": \"%s\", "
            "\"eq\": \"%s\"},\n"
        "    \"depth\": {"
            "\"test\": %s, \"write\": %s, \"func\": \"%s\"},\n"
        "    \"stencil\": {"
            "\"test\": %s, \"func\": \"%s\", \"ref\": %u, "
            "\"mask_read\": %u, \"mask_write\": %u, "
            "\"op_fail\": \"%s\", \"op_zfail\": \"%s\", "
            "\"op_zpass\": \"%s\"},\n"
        "    \"color_write_mask\": {"
            "\"r\": %s, \"g\": %s, \"b\": %s, \"a\": %s},\n"
        "    \"cull\": {"
            "\"enabled\": %s, \"face\": \"%s\", \"front_ccw\": %s},\n",
        idx > 0 ? ",\n" : "",
        idx,
        type,
        count,
        diag_prim_name(pg->primitive_mode),
        blend_en ? "true" : "false",
        diag_blend_factor_name(sfactor),
        diag_blend_factor_name(dfactor),
        diag_blend_eq_name(blend_eq),
        depth_test ? "true" : "false",
        depth_write ? "true" : "false",
        diag_compare_func_name(zfunc),
        stencil_test ? "true" : "false",
        diag_compare_func_name(stencil_func),
        stencil_ref, stencil_mask_read, stencil_mask_write,
        diag_stencil_op_name(op_fail),
        diag_stencil_op_name(op_zfail),
        diag_stencil_op_name(op_zpass),
        mask_r ? "true" : "false",
        mask_g ? "true" : "false",
        mask_b ? "true" : "false",
        mask_a ? "true" : "false",
        cull_en ? "true" : "false",
        cull_str,
        front_ccw ? "true" : "false"
    );

    if (r->color_binding) {
        diag_json_append(
            "    \"color_surface\": {"
                "\"format\": %u, \"width\": %u, \"height\": %u, "
                "\"pitch\": %u, \"bpp\": %u},\n",
            r->color_binding->shape.color_format,
            r->color_binding->width, r->color_binding->height,
            r->color_binding->pitch, r->color_binding->fmt.bytes_per_pixel
        );
    } else {
        diag_json_append("    \"color_surface\": null,\n");
    }

    if (r->zeta_binding) {
        diag_json_append(
            "    \"zeta_surface\": {"
                "\"format\": %u, \"width\": %u, \"height\": %u, "
                "\"pitch\": %u, \"bpp\": %u},\n",
            r->zeta_binding->shape.zeta_format,
            r->zeta_binding->width, r->zeta_binding->height,
            r->zeta_binding->pitch, r->zeta_binding->fmt.bytes_per_pixel
        );
    } else {
        diag_json_append("    \"zeta_surface\": null,\n");
    }

    diag_json_append("    \"textures\": [");
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bool tex_en = pgraph_is_texture_enabled(pg, i);
        uint32_t tex_fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        unsigned int color_format = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_COLOR);
        unsigned int dimensionality = GET_MASK(tex_fmt,
                                               NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
        unsigned int log_w = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
        unsigned int log_h = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);

        diag_json_append(
            "%s{\"stage\": %d, \"enabled\": %s, \"color_format\": %u, "
            "\"dim\": %u, \"width\": %u, \"height\": %u}",
            i > 0 ? ", " : "",
            i,
            tex_en ? "true" : "false",
            color_format,
            dimensionality,
            1u << log_w, 1u << log_h
        );
    }
    diag_json_append("]\n  }");

    char dir[600];
    if (rt_dump_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", rt_dump_dir);
    } else {
        snprintf(dir, sizeof(dir), "/tmp");
    }
    mkdir(dir, 0755);

    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);

    if (r->color_binding && r->color_binding->draw_dirty) {
        pgraph_vk_surface_download_if_dirty(d, r->color_binding);
        char path[700];
        snprintf(path, sizeof(path), "%s/diag_%u_draw%d_color.ppm",
                 dir, diag_frame_num, idx);
        dump_surface_ppm(d, r->color_binding, path);
    }

    if (r->zeta_binding && r->zeta_binding->draw_dirty) {
        pgraph_vk_surface_download_if_dirty(d, r->zeta_binding);
        char path[700];
        snprintf(path, sizeof(path), "%s/diag_%u_draw%d_depth.ppm",
                 dir, diag_frame_num, idx);
        dump_surface_ppm(d, r->zeta_binding, path);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
#ifdef XBOX
    {
        extern volatile int32_t *xbox_ram_fp_active_ptr;
        if (xbox_ram_fp_active_ptr &&
            !qatomic_read(xbox_ram_fp_active_ptr)) {
            qatomic_set(xbox_ram_fp_active_ptr, 1);
            error_report("[TLB-FP] activated after first flip");
        }
    }
#endif
    {
        static int dbg_flip = 0;
        if (dbg_flip < 30) {
            PGRAPHVkState *r = d->pgraph.vk_renderer_state;
            DBG_LOG("[FLIP] flip_stall: in_cb=%d frame=%d submit=%d",
                    r->in_command_buffer, r->current_frame,
                    (int)r->submit_count);
            dbg_flip++;
        }
    }
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);

    if (qatomic_read(&diag_frame_active)) {
        diag_json_append("\n]\n");
        diag_write_json();
        qatomic_set(&diag_frame_active, 0);
        fprintf(stderr, "diag: frame %u capture complete (%d draw calls)\n",
                diag_frame_num, diag_draw_index);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-diag",
                            "frame %u capture complete (%d draw calls)",
                            diag_frame_num, diag_draw_index);
#endif
    }

    if (qatomic_read(&diag_frame_pending)) {
        qatomic_set(&diag_frame_pending, 0);
        diag_frame_num = g_nv2a_stats.frame_count;
        diag_draw_index = 0;
        diag_json_len = 0;
        diag_json_append("[\n");
        qatomic_set(&diag_frame_active, 1);
        fprintf(stderr, "diag: starting capture for frame %u\n",
                diag_frame_num);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-diag",
                            "starting capture for frame %u", diag_frame_num);
#endif
    }

    maybe_dump_render_target(d);
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
#if OPT_DISPLAY_DOUBLE_BUFFER
        DisplayImage *ready = &r->display.images[r->display.display_idx];
        if (ready->valid) {
            if (ready->fence_submitted) {
                VK_CHECK(vkWaitForFences(r->device, 1, &ready->fence,
                                         VK_TRUE, UINT64_MAX));
                ready->fence_submitted = false;
            }
            int tex = ready->gl_texture_id;

            qemu_event_reset(&d->pgraph.sync_complete);
            qatomic_set(&pg->sync_pending, true);
            pfifo_kick(d);
            qemu_mutex_unlock(&d->pfifo.lock);
            return tex;
        }
#endif
        qemu_event_reset(&d->pgraph.sync_complete);
        qatomic_set(&pg->sync_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.sync_complete);
#if OPT_DISPLAY_DOUBLE_BUFFER
        ready = &r->display.images[r->display.display_idx];
        if (ready->valid && ready->fence_submitted) {
            VK_CHECK(vkWaitForFences(r->device, 1, &ready->fence,
                                     VK_TRUE, UINT64_MAX));
            ready->fence_submitted = false;
        }
        return ready->valid ? ready->gl_texture_id : 0;
#else
        return r->display.images[0].gl_texture_id;
#endif
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
