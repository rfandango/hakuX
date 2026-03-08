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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/error-report.h"
#include "renderer.h"
#include "system/physmem.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/prim_rewrite.h"
#include <math.h>
#ifdef __ANDROID__
#include <android/log.h>
#define DBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xemu-vk-dbg", __VA_ARGS__)
#else
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static bool g_xemu_fast_fences = false;
static bool g_xemu_draw_reorder = false;
static int g_xemu_submit_frames = 3;

struct OptBisectStats g_opt_stats;

static void opt_stats_log_and_reset(void)
{
#if NV2A_PERF_LOG
    static int frame_counter = 0;
    if (++frame_counter % 60 == 0) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-rw",
                "RW:%d/%d/%d Safe:%d(L%d/LE%d) Rej:Bl%d Cw%d Dp%d Zw%d Zf%d St%d Al%d Ak%d Rt%d Fb%d Zp%d",
                g_opt_stats.reorder_windows_flushed,
                g_opt_stats.reorder_draws_reordered,
                g_opt_stats.reorder_pipeline_switches_saved,
                g_opt_stats.reorder_safe_draws,
                g_opt_stats.reorder_safe_zfunc_less,
                g_opt_stats.reorder_safe_zfunc_lequal,
                g_opt_stats.reorder_reject_blend,
                g_opt_stats.reorder_reject_no_color_write,
                g_opt_stats.reorder_reject_no_depth,
                g_opt_stats.reorder_reject_no_zwrite,
                g_opt_stats.reorder_reject_zfunc,
                g_opt_stats.reorder_reject_stencil,
                g_opt_stats.reorder_reject_alpha,
                g_opt_stats.reorder_reject_alphakill,
                g_opt_stats.reorder_reject_rtt,
                g_opt_stats.reorder_reject_fb_dirty,
                g_opt_stats.reorder_reject_zpass);
#else
        DBG_LOG("[OPT-STATS] SFP:%d/%d PEX:%d/%d VTC:%d/%d DRS:%d/%d MDI:%d/%d"
                " RW:%d/%d/%d"
                " RWSafe:%d Rej:Bl%d Cw%d Dp%d Zw%d Zf%d St%d Al%d Ak%d Rt%d Fb%d Zp%d",
                g_opt_stats.super_fast_hits,
                g_opt_stats.super_fast_misses,
                g_opt_stats.pipeline_early_hits,
                g_opt_stats.pipeline_early_misses,
                g_opt_stats.vtx_cache_hits,
                g_opt_stats.vtx_cache_misses,
                g_opt_stats.desc_rebind_skips,
                g_opt_stats.desc_rebind_full,
                g_opt_stats.multi_draw_indirect,
                g_opt_stats.multi_draw_loop,
                g_opt_stats.reorder_windows_flushed,
                g_opt_stats.reorder_draws_reordered,
                g_opt_stats.reorder_pipeline_switches_saved,
                g_opt_stats.reorder_safe_draws,
                g_opt_stats.reorder_reject_blend,
                g_opt_stats.reorder_reject_no_color_write,
                g_opt_stats.reorder_reject_no_depth,
                g_opt_stats.reorder_reject_no_zwrite,
                g_opt_stats.reorder_reject_zfunc,
                g_opt_stats.reorder_reject_stencil,
                g_opt_stats.reorder_reject_alpha,
                g_opt_stats.reorder_reject_alphakill,
                g_opt_stats.reorder_reject_rtt,
                g_opt_stats.reorder_reject_fb_dirty,
                g_opt_stats.reorder_reject_zpass);
#endif
        memset(&g_opt_stats, 0, sizeof(g_opt_stats));
    }
#else
    memset(&g_opt_stats, 0, sizeof(g_opt_stats));
#endif
}

void xemu_set_fast_fences(bool enable)
{
    g_xemu_fast_fences = enable;
}

bool xemu_get_fast_fences(void)
{
    return g_xemu_fast_fences;
}

void xemu_set_draw_reorder(bool enable)
{
    g_xemu_draw_reorder = enable;
}

bool xemu_get_draw_reorder(void)
{
    return g_xemu_draw_reorder;
}

void xemu_set_submit_frames(int count)
{
    if (count < 1) count = 1;
    if (count > NUM_SUBMIT_FRAMES) count = NUM_SUBMIT_FRAMES;
    g_xemu_submit_frames = count;
}

int xemu_get_submit_frames(void)
{
    return g_xemu_submit_frames;
}

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    void *initial_data = NULL;
    gsize initial_size = 0;

    if (g_config.perf.cache_shaders) {
        const char *base = xemu_settings_get_base_path();
        char *plc_path = g_strdup_printf("%svk_pipeline_cache.bin", base);
        if (g_file_get_contents(plc_path, (gchar **)&initial_data,
                                &initial_size, NULL)) {
            VK_LOG("Loaded pipeline cache from disk (%zu bytes)", initial_size);
            g_nv2a_stats.shader_stats.pipeline_cache_disk_loaded = 1;
        }
        g_free(plc_path);
    }

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = initial_size,
        .pInitialData = initial_data,
        .pNext = NULL,
    };
    VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                   &r->vk_pipeline_cache));
    g_free(initial_data);

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void save_pipeline_cache_to_disk(PGRAPHVkState *r)
{
    size_t size = 0;
    VkResult res = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                          &size, NULL);
    if (res == VK_SUCCESS && size > 0) {
        void *data = g_malloc(size);
        res = vkGetPipelineCacheData(r->device, r->vk_pipeline_cache,
                                     &size, data);
        if (res == VK_SUCCESS) {
            const char *base = xemu_settings_get_base_path();
            char *plc_path = g_strdup_printf("%svk_pipeline_cache.bin", base);
            g_file_set_contents(plc_path, (const gchar *)data, size, NULL);
            VK_LOG("Saved pipeline cache to disk (%zu bytes)", size);
            g_nv2a_stats.shader_stats.pipeline_cache_disk_saved++;
            g_free(plc_path);
        }
        g_free(data);
    }
}

#define PIPELINE_CACHE_SAVE_INTERVAL_US (30 * 1000000LL)

static void maybe_save_pipeline_cache(PGRAPHVkState *r)
{
    if (!g_config.perf.cache_shaders) {
        return;
    }
    static int64_t last_save_us;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (last_save_us && (now - last_save_us) < PIPELINE_CACHE_SAVE_INTERVAL_US) {
        return;
    }
    last_save_us = now;
    save_pipeline_cache_to_disk(r);
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (g_config.perf.cache_shaders) {
        save_pipeline_cache_to_disk(r);
    }

    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->frame_semaphores[i]));
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                               &r->frame_fences[i]));
    }

    VK_CHECK(
        vkCreateFence(r->device, &fence_info, NULL, &r->aux_fence));

    r->current_frame = 0;
    r->command_buffer_semaphore = r->frame_semaphores[0];
    r->command_buffer_fence = r->frame_fences[0];
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        if (r->frame_submitted[i]) {
            VK_CHECK(vkWaitForFences(r->device, 1, &r->frame_fences[i],
                                     VK_TRUE, UINT64_MAX));
            r->frame_submitted[i] = false;
        }
#if OPT_DEFERRED_FENCES && OPT_N_BUFFERED_SUBMIT
        for (int j = 0; j < r->deferred_framebuffer_count[i]; j++) {
            vkDestroyFramebuffer(r->device, r->deferred_framebuffers[i][j], NULL);
        }
        r->deferred_framebuffer_count[i] = 0;
#endif
        vkDestroyFence(r->device, r->frame_fences[i], NULL);
        vkDestroySemaphore(r->device, r->frame_semaphores[i], NULL);
    }
    vkDestroyFence(r->device, r->aux_fence, NULL);
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        g_nv2a_stats.shader_stats.pipeline_cache_hits++;
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    g_nv2a_stats.shader_stats.pipeline_cache_misses++;
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    maybe_save_pipeline_cache(r);
    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->pipeline_binding || r->shader_bindings_changed ||
        r->texture_bindings_changed || r->pipeline_state_dirty ||
        check_render_pass_dirty(pg)) {
        r->pipeline_state_dirty = false;
        return true;
    }

    if (pg->pipeline_state_gen != r->last_pipeline_state_gen) {
        return true;
    }

#if OPT_VALIDATE_GEN_COUNTERS
    {
        const unsigned int vregs[] = {
            NV_PGRAPH_BLEND,     NV_PGRAPH_CONTROL_0,
            NV_PGRAPH_CONTROL_1, NV_PGRAPH_CONTROL_2,
            NV_PGRAPH_CONTROL_3, NV_PGRAPH_SETUPRASTER,
            NV_PGRAPH_BLENDCOLOR,
            NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
        };
        for (int i = 0; i < ARRAY_SIZE(vregs); i++) {
            assert(!pgraph_is_reg_dirty(pg, vregs[i]));
        }
    }
#endif

    if (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

#if OPT_DYNAMIC_STATES
    const int regs[] = {
        NV_PGRAPH_BLEND,     NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_2, NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_CONTROL_1,
    };
#else
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_2,
        NV_PGRAPH_CONTROL_3,   NV_PGRAPH_SETUPRASTER,
        NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
        NV_PGRAPH_CONTROL_1,
    };
#endif
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
#if OPT_DYNAMIC_STATES
    bool use_dyn_ds = OPT_DYNAMIC_DEPTH_STENCIL &&
                      r->extended_dynamic_state_supported;
    if (use_dyn_ds) {
        key->regs[1] &= ~(NV_PGRAPH_CONTROL_0_ZENABLE |
                           NV_PGRAPH_CONTROL_0_ZWRITEENABLE |
                           NV_PGRAPH_CONTROL_0_ZFUNC);
        key->regs[2] &= ~(NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL |
                           NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL |
                           NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
    }
    key->regs[4] &= ~(NV_PGRAPH_SETUPRASTER_CULLENABLE |
                       NV_PGRAPH_SETUPRASTER_CULLCTRL |
                       NV_PGRAPH_SETUPRASTER_FRONTFACE);
    if (use_dyn_ds) {
        key->regs[5] &= ~(NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE |
                           NV_PGRAPH_CONTROL_1_STENCIL_FUNC |
                           NV_PGRAPH_CONTROL_1_STENCIL_REF |
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ |
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    } else {
        key->regs[5] &= ~(NV_PGRAPH_CONTROL_1_STENCIL_REF |
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ |
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    }
#endif
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_PIPELINE_EARLY_EXIT
    if (r->pipeline_binding &&
        !r->shader_bindings_changed &&
        !r->pipeline_state_dirty &&
        pg->texture_state_gen == r->last_texture_state_gen &&
        r->texture_vram_gen == r->last_texture_vram_gen &&
        !check_render_pass_dirty(pg) &&
        pg->shader_state_gen == r->last_shader_state_gen &&
        pg->pipeline_state_gen == r->last_pipeline_state_gen &&
        pg->primitive_mode == r->shader_binding->state.geom.primitive_mode) {
        OPT_STAT_INC(pipeline_early_hits);
        NV2A_VK_DGROUP_END();
        return;
    }
    OPT_STAT_INC(pipeline_early_misses);
#endif

    NV2A_PHASE_TIMER_BEGIN(pipe_bind_tex);
    if (pg->texture_state_gen != r->last_texture_state_gen ||
        r->texture_vram_gen != r->last_texture_vram_gen) {
        pgraph_vk_bind_textures(d);
        r->last_texture_state_gen = pg->texture_state_gen;
        r->last_texture_vram_gen = r->texture_vram_gen;
    }
    NV2A_PHASE_TIMER_END(pipe_bind_tex);

    NV2A_PHASE_TIMER_BEGIN(pipe_bind_shd);
#if OPT_VALIDATE_GEN_COUNTERS
    if (!pg->program_data_dirty && r->shader_binding &&
        pg->shader_state_gen == r->last_shader_state_gen &&
        pg->primitive_mode == r->shader_binding->state.geom.primitive_mode) {
        assert(!pgraph_glsl_check_shader_state_dirty(pg,
                                                     &r->shader_binding->state));
    }
#endif
    if (pg->program_data_dirty || !r->shader_binding ||
        pg->shader_state_gen != r->last_shader_state_gen ||
        pg->primitive_mode != r->shader_binding->state.geom.primitive_mode) {
        pgraph_vk_bind_shaders(pg);
        r->last_shader_state_gen = pg->shader_state_gen;
    } else {
        pgraph_vk_update_shader_uniforms(pg);
    }
    NV2A_PHASE_TIMER_END(pipe_bind_shd);

    NV2A_PHASE_TIMER_BEGIN(pipe_lookup);
    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    r->last_pipeline_state_gen = pg->pipeline_state_gen;
    r->last_shader_state_gen = pg->shader_state_gen;
    r->last_any_reg_gen = pg->any_reg_gen;
    r->last_texture_vram_gen = r->texture_vram_gen;

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        NV2A_PHASE_TIMER_END(pipe_lookup);
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);

    if (r->pipeline_binding &&
        memcmp(&key, &r->pipeline_binding->key, sizeof(key)) == 0) {
        NV2A_VK_DPRINTF("Cache hit (same binding)");
        g_nv2a_stats.shader_stats.pipeline_cache_hits++;
        NV2A_PHASE_TIMER_END(pipe_lookup);
        NV2A_VK_DGROUP_END();
        return;
    }

    uint64_t hash = fast_hash((void *)&key, sizeof(key));

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        g_nv2a_stats.shader_stats.pipeline_cache_hits++;
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_PHASE_TIMER_END(pipe_lookup);
        NV2A_VK_DGROUP_END();
        return;
    }
    NV2A_PHASE_TIMER_END(pipe_lookup);

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    g_nv2a_stats.shader_stats.pipeline_cache_misses++;
    NV2A_PHASE_TIMER_BEGIN(shader_compile);

    memcpy(&snode->key, &key, sizeof(key));

    bool use_dyn_ds = OPT_DYNAMIC_STATES && OPT_DYNAMIC_DEPTH_STENCIL &&
                      r->extended_dynamic_state_supported;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = false, depth_write = false, stencil_test = false;
    if (!use_dyn_ds) {
        depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
        depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
        stencil_test =
            pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    }

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPolygonMode polygon_mode =
        pgraph_polygon_mode_vk_map[r->shader_binding->state.geom.polygon_front_mode];
    if (polygon_mode != VK_POLYGON_MODE_FILL &&
        r->enabled_physical_device_features.fillModeNonSolid != VK_TRUE) {
        polygon_mode = VK_POLYGON_MODE_FILL;
    }

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable =
            r->enabled_physical_device_features.depthClamp == VK_TRUE ?
            VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = polygon_mode,
        .lineWidth = 1.0f,
        .frontFace =
#if OPT_DYNAMIC_STATES
            VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .cullMode = VK_CULL_MODE_NONE,
#else
            (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
             NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                 VK_FRONT_FACE_COUNTER_CLOCKWISE :
                 VK_FRONT_FACE_CLOCKWISE,
#endif
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

#if !OPT_DYNAMIC_STATES
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }
#endif

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    if (use_dyn_ds) {
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
    } else {
        depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;

        if (depth_test) {
            depth_stencil.depthTestEnable = VK_TRUE;
            uint32_t depth_func =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
            assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
            depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
        }

        if (stencil_test) {
            depth_stencil.stencilTestEnable = VK_TRUE;
            uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                             NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                            NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                          NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                        NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

            assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

            depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
            depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
            depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
            depth_stencil.front.compareOp =
                pgraph_stencil_func_vk_map[stencil_func];
            depth_stencil.front.compareMask = mask_read;
            depth_stencil.front.writeMask = mask_write;
            depth_stencil.front.reference = stencil_ref;
            depth_stencil.back = depth_stencil.front;
        }
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    float blend_constant[4] = { 0, 0, 0, 0 };

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    VkDynamicState dynamic_states[16] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
#if OPT_DYNAMIC_STATES
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
#endif
    };
    int num_dynamic_states = OPT_DYNAMIC_STATES ? 8 : 2;
    if (use_dyn_ds) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_OP;
    }

    snode->has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (snode->has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

    VkPushConstantRange push_constant_range;
    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    maybe_save_pipeline_cache(r);
    NV2A_PHASE_TIMER_END(shader_compile);
    NV2A_VK_DGROUP_END();
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);

    uint32_t dynamic_offsets[2] = {
        (uint32_t)r->uniform_buffer_offsets[0],
        (uint32_t)r->uniform_buffer_offsets[1],
    };

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->descriptor_sets[r->descriptor_set_index - 1],
                            2, dynamic_offsets);
}

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
    VkQueryControlFlags query_flags =
        r->enabled_physical_device_features.occlusionQueryPrecise == VK_TRUE ?
        VK_QUERY_CONTROL_PRECISE_BIT : 0;
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    query_flags);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = get_staging_buffer(r, index_src);
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    if (!b_src->buffer_offset) {
        return;
    }

    VkBufferCopy copy_region = { .size = b_src->buffer_offset };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .size = b_src->buffer_offset
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    b_src->buffer_offset = 0;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->vertex_ram_flush_min >= r->vertex_ram_flush_max) {
        return;
    }

    VkDeviceSize offset = r->vertex_ram_flush_min;
    VkDeviceSize size = r->vertex_ram_flush_max - r->vertex_ram_flush_min;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation,
        offset, size));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = offset,
        .size = size,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);

    r->vertex_ram_flush_min = VK_WHOLE_SIZE;
    r->vertex_ram_flush_max = 0;
}

static VkAttachmentLoadOp get_optimal_color_load_op(PGRAPHVkState *r)
{
    if (!r->color_binding) {
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    if (r->color_drawn_in_cb) {
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (!r->color_binding->initialized) {
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

static VkAttachmentLoadOp get_optimal_zeta_load_op(PGRAPHVkState *r)
{
    if (!r->zeta_binding) {
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    if (r->zeta_drawn_in_cb) {
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (!r->zeta_binding->initialized) {
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->framebuffer_index > 0);

#if OPT_LOAD_OPS
    RenderPassState begin_state;
    init_render_pass_state(pg, &begin_state);
    begin_state.color_load_op = get_optimal_color_load_op(r);
    begin_state.zeta_load_op = get_optimal_zeta_load_op(r);
    begin_state.stencil_load_op = begin_state.zeta_load_op;
    r->begin_render_pass = get_render_pass(r, &begin_state);
#else
    r->begin_render_pass = r->render_pass;
#endif

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->begin_render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;
}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

#if OPT_ALWAYS_DEFERRED_FENCES
void pgraph_vk_flush_all_frames(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    for (int i = 0; i < NUM_SUBMIT_FRAMES; i++) {
        if (r->frame_submitted[i]) {
            VK_CHECK(vkWaitForFences(r->device, 1, &r->frame_fences[i],
                                     VK_TRUE, UINT64_MAX));
            r->frame_submitted[i] = false;
            for (int j = 0; j < r->deferred_framebuffer_count[i]; j++) {
                vkDestroyFramebuffer(r->device,
                                     r->deferred_framebuffers[i][j], NULL);
            }
            r->deferred_framebuffer_count[i] = 0;
        }
        if (i != r->current_frame) {
            r->frame_staging[i].index_staging.buffer_offset = 0;
            r->frame_staging[i].vertex_inline_staging.buffer_offset = 0;
            r->frame_staging[i].uniform_staging.buffer_offset = 0;
            r->frame_staging[i].staging_src.buffer_offset = 0;
            bitmap_clear(r->frame_staging[i].uploaded_bitmap,
                         0, r->bitmap_size);
        }
    }
}
#endif

#if OPT_REORDER_SAFE_WINDOWS
static void flush_reorder_window_internal(NV2AState *d);
#endif
#if OPT_DRAW_MERGING
static void flush_draw_queue_internal(NV2AState *d);
#endif

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
#if OPT_REORDER_SAFE_WINDOWS
    {
        PGRAPHVkState *r_rw = pg->vk_renderer_state;
        if (r_rw->reorder_window.count > 0) {
            NV2AState *d = container_of(pg, NV2AState, pgraph);
            flush_reorder_window_internal(d);
        }
        r_rw->reorder_window.active = false;
    }
#endif
#if OPT_DRAW_MERGING
    PGRAPHVkState *r_pre = pg->vk_renderer_state;
    if (r_pre->draw_queue.count > 0) {
        NV2AState *d = container_of(pg, NV2AState, pgraph);
        flush_draw_queue_internal(d);
    }
    r_pre->draw_queue.active = false;
#endif

    NV2A_PHASE_TIMER_BEGIN(finish);
    PGRAPHVkState *r = pg->vk_renderer_state;

    {
        static int dbg_entry = 0;
        if (dbg_entry < 100) {
            DBG_LOG("[FIN-ENTRY] reason=%d in_cb=%d frame=%d",
                    finish_reason, r->in_command_buffer, r->current_frame);
            dbg_entry++;
        }
    }

    if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
        finish_reason == VK_FINISH_REASON_PRESENTING) {
        opt_stats_log_and_reset();
    }

    int desired_frames = g_xemu_submit_frames;
    if (desired_frames != r->num_active_frames) {
        pgraph_vk_flush_all_frames(pg);
        r->num_active_frames = desired_frames;
        r->current_frame = 0;
        r->command_buffer = r->command_buffers[0];
        r->aux_command_buffer = r->command_buffers[1];
        r->command_buffer_semaphore = r->frame_semaphores[0];
        r->command_buffer_fence = r->frame_fences[0];
    }

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        VkCommandBuffer cmd = pgraph_vk_ensure_nondraw_commands(pg);

#if OPT_ALWAYS_DEFERRED_FENCES
        /* WAR barrier: previous submissions may still be reading from shared
         * destination buffers (UNIFORM, INDEX, VERTEX_INLINE, VERTEX_RAM).
         * The staging sync below overwrites these from offset 0. Ensure all
         * previous reads complete before we write. */
        VkMemoryBarrier war_barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT,
        };
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &war_barrier, 0, NULL, 0, NULL);
#endif

        sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
        sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                BUFFER_VERTEX_INLINE);
        sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
        bitmap_clear(get_uploaded_bitmap(r), 0, r->bitmap_size);
#if OPT_SYNC_RANGE_SKIP
        r->sync_range_attr_gen = 0;
        r->sync_range_min = UINT32_MAX;
        r->sync_range_max = 0;
#endif
        flush_memory_buffer(pg, cmd);
        VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
        r->in_aux_command_buffer = false;

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT |
                                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &r->command_buffer_semaphore,
            },
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &r->command_buffer_semaphore,
                .pWaitDstStageMask = &wait_stage,
            }
        };
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        vkResetFences(r->device, 1, &r->command_buffer_fence);
        NV2A_PHASE_TIMER_BEGIN(finish_submit);
        VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                               r->command_buffer_fence));
        NV2A_PHASE_TIMER_END(finish_submit);
        r->frame_submitted[r->current_frame] = true;
        r->submit_count += 1;

        NV2A_PHASE_TIMER_BEGIN(finish_fence);

#if OPT_ALWAYS_DEFERRED_FENCES
        {
            static int dbg_finish_count = 0;
            bool deferred = (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
                             finish_reason == VK_FINISH_REASON_PRESENTING ||
                             finish_reason == VK_FINISH_REASON_NEED_BUFFER_SPACE);
            if (g_xemu_fast_fences) {
                deferred = deferred ||
                    finish_reason == VK_FINISH_REASON_VERTEX_BUFFER_DIRTY;
            }
            if (dbg_finish_count < 200) {
                DBG_LOG("[FIN] reason=%d deferred=%d frame=%d submit=%d",
                        finish_reason, deferred, r->current_frame,
                        (int)r->submit_count);
                dbg_finish_count++;
            }
            if (!deferred) {
                VK_CHECK(vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                         VK_TRUE, UINT64_MAX));
            }
        }
#endif

#if OPT_TEX_NONDRAW_CMD
        pgraph_vk_staging_reset(pg);
#endif

        bool check_budget = false;

        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

#if OPT_DEFERRED_FENCES && OPT_N_BUFFERED_SUBMIT
        if (OPT_ALWAYS_DEFERRED_FENCES ||
            finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            finish_reason == VK_FINISH_REASON_PRESENTING) {
            memcpy(r->deferred_framebuffers[r->current_frame],
                   r->framebuffers,
                   r->framebuffer_index * sizeof(VkFramebuffer));
            r->deferred_framebuffer_count[r->current_frame] = r->framebuffer_index;
            r->framebuffer_index = 0;

            int next_frame = (r->current_frame + 1) % r->num_active_frames;

            {
                static int dbg_advance_count = 0;
                if (dbg_advance_count < 200) {
                    DBG_LOG("[FIN] advance %d->%d next_submitted=%d reason=%d",
                            r->current_frame, next_frame,
                            r->frame_submitted[next_frame], finish_reason);
                    dbg_advance_count++;
                }
            }

            if (r->frame_submitted[next_frame]) {
                VK_CHECK(vkWaitForFences(r->device, 1,
                                         &r->frame_fences[next_frame],
                                         VK_TRUE, UINT64_MAX));
                r->frame_submitted[next_frame] = false;
                for (int i = 0; i < r->deferred_framebuffer_count[next_frame]; i++) {
                    vkDestroyFramebuffer(r->device,
                                         r->deferred_framebuffers[next_frame][i],
                                         NULL);
                }
                r->deferred_framebuffer_count[next_frame] = 0;
#if OPT_ALWAYS_DEFERRED_FENCES
                r->frame_staging[next_frame].index_staging.buffer_offset = 0;
                r->frame_staging[next_frame].vertex_inline_staging.buffer_offset = 0;
                r->frame_staging[next_frame].uniform_staging.buffer_offset = 0;
                r->frame_staging[next_frame].staging_src.buffer_offset = 0;
                bitmap_clear(r->frame_staging[next_frame].uploaded_bitmap,
                             0, r->bitmap_size);
#endif
            }

            r->current_frame = next_frame;
            r->command_buffer = r->command_buffers[next_frame * 2];
            r->aux_command_buffer = r->command_buffers[next_frame * 2 + 1];
            r->command_buffer_semaphore = r->frame_semaphores[next_frame];
            r->command_buffer_fence = r->frame_fences[next_frame];
        } else {
            VK_CHECK(vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                     VK_TRUE, UINT64_MAX));
            r->frame_submitted[r->current_frame] = false;

            int next_frame = (r->current_frame + 1) % r->num_active_frames;
            r->current_frame = next_frame;
            r->command_buffer = r->command_buffers[next_frame * 2];
            r->aux_command_buffer = r->command_buffers[next_frame * 2 + 1];
            r->command_buffer_semaphore = r->frame_semaphores[next_frame];
            r->command_buffer_fence = r->frame_fences[next_frame];
            destroy_framebuffers(pg);
        }
#else
        VK_CHECK(vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                 VK_TRUE, UINT64_MAX));
        r->frame_submitted[r->current_frame] = false;

        int next_frame = (r->current_frame + 1) % r->num_active_frames;
        r->current_frame = next_frame;
        r->command_buffer = r->command_buffers[next_frame * 2];
        r->aux_command_buffer = r->command_buffers[next_frame * 2 + 1];
        r->command_buffer_semaphore = r->frame_semaphores[next_frame];
        r->command_buffer_fence = r->frame_fences[next_frame];
        destroy_framebuffers(pg);
#endif

#if OPT_ALWAYS_DEFERRED_FENCES
        /* Descriptor sets are still in use by in-flight GPU frames.
         * Don't reset indices; they'll be reset when pools are exhausted
         * after flushing all frames. */
#else
        r->descriptor_set_index = 0;
#endif
        r->need_descriptor_rebind = true;
        r->in_command_buffer = false;
        r->color_drawn_in_cb = false;
        r->zeta_drawn_in_cb = false;
        r->queries_reset_in_cb = false;
#if OPT_DYNAMIC_STATES
        r->dyn_state.valid = false;
#endif

        NV2A_PHASE_TIMER_END(finish_fence);

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

#if !OPT_ALWAYS_DEFERRED_FENCES
    pgraph_vk_compute_finish_complete(r);
#endif
    NV2A_PHASE_TIMER_END(finish);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    {
        static int dbg_begin = 0;
        if (dbg_begin < 50) {
            DBG_LOG("[CB-BEGIN] frame=%d draw_time=%lu",
                    r->current_frame, (unsigned long)pg->draw_time);
            dbg_begin++;
        }
    }

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

#if OPT_SUPER_FAST_PATH
    if (!pg->clearing &&
        r->pipeline_binding &&
        r->in_command_buffer &&
        r->in_render_pass &&
        r->framebuffer_index > 0 &&
        !r->framebuffer_dirty &&
        !r->shader_bindings_changed &&
        !r->pipeline_state_dirty &&
        !r->need_descriptor_rebind &&
        !r->uniforms_changed &&
        r->descriptor_set_index > 0 &&
        pg->texture_state_gen == r->last_texture_state_gen &&
        pg->any_reg_gen == r->last_any_reg_gen &&
        !pg->program_data_dirty &&
        pg->vertex_attr_gen == r->pipeline_vertex_attr_gen) {

        bool tex_vram_clean = (r->texture_vram_gen == r->last_texture_vram_gen);
        if (!tex_vram_clean) {
            tex_vram_clean = true;
            for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                TextureBinding *b = r->texture_bindings[i];
                if (b && b != &r->dummy_texture && b->possibly_dirty) {
                    if (b->dirty_check_frame == pg->frame_time &&
                        !b->dirty_check_result) {
                        b->possibly_dirty = false;
                    } else {
                        tex_vram_clean = false;
                        break;
                    }
                }
            }
            if (tex_vram_clean) {
                r->last_texture_vram_gen = r->texture_vram_gen;
            }
        }

        if (tex_vram_clean) {
#if OPT_VALIDATE_GEN_COUNTERS
            assert(!pgraph_has_dirty_regs(pg));
#endif
            OPT_STAT_INC(super_fast_hits);
            r->pre_draw_skipped = true;
            return;
        }
    }
    OPT_STAT_INC(super_fast_misses);
#endif

#if OPT_MEDIUM_FAST_PATH
    if (!pg->clearing &&
        r->pipeline_binding &&
        r->in_command_buffer && r->in_render_pass &&
        r->framebuffer_index > 0 &&
        !r->framebuffer_dirty &&
        !r->shader_bindings_changed &&
        !r->pipeline_state_dirty &&
        !r->need_descriptor_rebind &&
        r->descriptor_set_index > 0 &&
        pg->texture_state_gen == r->last_texture_state_gen &&
        r->texture_vram_gen == r->last_texture_vram_gen &&
        pg->shader_state_gen == r->last_shader_state_gen &&
        pg->pipeline_state_gen == r->last_pipeline_state_gen &&
        pg->primitive_mode == r->shader_binding->state.geom.primitive_mode &&
        !pg->program_data_dirty &&
        pg->vertex_attr_gen == r->pipeline_vertex_attr_gen) {
        r->pre_draw_skipped = false;
        pgraph_vk_update_shader_uniforms(pg);
        pgraph_vk_update_descriptor_sets(pg);
        if (r->framebuffer_index == 0) {
            create_frame_buffer(pg);
        }
        pgraph_vk_ensure_command_buffer(pg);
        return;
    }
#endif

    r->pre_draw_skipped = false;

    NV2A_PHASE_TIMER_BEGIN(draw_pipeline);
    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
    }
    r->pipeline_vertex_attr_gen = pg->vertex_attr_gen;
    NV2A_PHASE_TIMER_END(draw_pipeline);

    {
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

        if (r->framebuffer_dirty || render_pass_dirty) {
            pgraph_vk_ensure_not_in_render_pass(pg);
        }
        if (render_pass_dirty) {
            r->render_pass = r->pipeline_binding->render_pass;
        }
        if (r->framebuffer_dirty) {
            create_frame_buffer(pg);
            r->framebuffer_dirty = false;
        }
        NV2A_PHASE_TIMER_END(draw_setup);
    }

    NV2A_PHASE_TIMER_BEGIN(draw_desc_set);
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
    }
    NV2A_PHASE_TIMER_END(draw_desc_set);

    {
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        if (r->framebuffer_index == 0) {
            create_frame_buffer(pg);
        }

        pgraph_vk_ensure_command_buffer(pg);
        NV2A_PHASE_TIMER_END(draw_setup);
    }
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding_changed = false;
        r->pipeline_binding->draw_time = pg->draw_time;
#if OPT_DYNAMIC_STATES
        r->dyn_state.valid = false;
#endif

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
    }

    if (!pg->clearing) {
#if OPT_DYNAMIC_STATES
        {
            uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
            if (!r->dyn_state.valid ||
                setupraster != r->dyn_state.setupraster) {
                VkCullModeFlags cull = VK_CULL_MODE_NONE;
                if (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
                    uint32_t cull_face = GET_MASK(setupraster,
                                                  NV_PGRAPH_SETUPRASTER_CULLCTRL);
                    assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
                    cull = pgraph_cull_face_vk_map[cull_face];
                }
                vkCmdSetCullMode(r->command_buffer, cull);
                vkCmdSetFrontFace(r->command_buffer,
                                  (setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE)
                                      ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                      : VK_FRONT_FACE_CLOCKWISE);
                r->dyn_state.setupraster = setupraster;
            }
        }

        {
            uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
            if (!r->dyn_state.valid ||
                blend_color != r->dyn_state.blendcolor) {
                float blend_constant[4] = { 0, 0, 0, 0 };
                pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
                vkCmdSetBlendConstants(r->command_buffer, blend_constant);
                r->dyn_state.blendcolor = blend_color;
            }
        }

        if (OPT_DYNAMIC_DEPTH_STENCIL &&
            r->extended_dynamic_state_supported) {
            uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
            if (!r->dyn_state.valid ||
                control_0 != r->dyn_state.control_0) {
                vkCmdSetDepthTestEnable(
                    r->command_buffer,
                    (control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) ? VK_TRUE
                                                              : VK_FALSE);
                vkCmdSetDepthWriteEnable(
                    r->command_buffer,
                    (control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE) ? VK_TRUE
                                                                   : VK_FALSE);
                uint32_t depth_func = GET_MASK(control_0,
                                               NV_PGRAPH_CONTROL_0_ZFUNC);
                assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
                vkCmdSetDepthCompareOp(r->command_buffer,
                                       pgraph_depth_func_vk_map[depth_func]);
                r->dyn_state.control_0 = control_0;
            }

            uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
            uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
            if (!r->dyn_state.valid ||
                control_1 != r->dyn_state.control_1 ||
                control_2 != r->dyn_state.control_2) {
                bool stencil_test_enable =
                    control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
                vkCmdSetStencilTestEnable(r->command_buffer,
                                          stencil_test_enable ? VK_TRUE
                                                              : VK_FALSE);
                uint32_t stencil_func = GET_MASK(control_1,
                                                 NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
                assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
                uint32_t stencil_ref = GET_MASK(control_1,
                                                NV_PGRAPH_CONTROL_1_STENCIL_REF);
                uint32_t mask_read = GET_MASK(control_1,
                                              NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
                uint32_t mask_write = GET_MASK(control_1,
                                               NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
                vkCmdSetStencilCompareMask(r->command_buffer,
                                           VK_STENCIL_FACE_FRONT_AND_BACK, mask_read);
                vkCmdSetStencilWriteMask(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, mask_write);
                vkCmdSetStencilReference(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);

                uint32_t op_fail = GET_MASK(control_2,
                                            NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
                uint32_t op_zfail = GET_MASK(control_2,
                                             NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
                uint32_t op_zpass = GET_MASK(control_2,
                                             NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
                assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
                assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
                assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));
                vkCmdSetStencilOp(r->command_buffer,
                                  VK_STENCIL_FACE_FRONT_AND_BACK,
                                  pgraph_stencil_op_vk_map[op_fail],
                                  pgraph_stencil_op_vk_map[op_zpass],
                                  pgraph_stencil_op_vk_map[op_zfail],
                                  pgraph_stencil_func_vk_map[stencil_func]);

                r->dyn_state.control_1 = control_1;
                r->dyn_state.control_2 = control_2;
            }
        } else {
            uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
            if (!r->dyn_state.valid ||
                control_1 != r->dyn_state.control_1) {
                uint32_t stencil_ref = GET_MASK(control_1,
                                                NV_PGRAPH_CONTROL_1_STENCIL_REF);
                uint32_t mask_read = GET_MASK(control_1,
                                              NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
                uint32_t mask_write = GET_MASK(control_1,
                                               NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
                vkCmdSetStencilCompareMask(r->command_buffer,
                                           VK_STENCIL_FACE_FRONT_AND_BACK, mask_read);
                vkCmdSetStencilWriteMask(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, mask_write);
                vkCmdSetStencilReference(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
                r->dyn_state.control_1 = control_1;
            }
        }

        r->dyn_state.valid = true;
#endif

        if (!r->pre_draw_skipped) {
            bind_descriptor_sets(pg);
            push_vertex_attr_values(pg);
        }
    }

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static void sync_vertex_ram_buffer(PGRAPHState *pg);
static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices);
static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size);
static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices);
static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset);

#if OPT_DRAW_MERGING || OPT_REORDER_SAFE_WINDOWS
static bool check_rt_as_texture_hazard(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *b = r->texture_bindings[i];
        if (!b || b == &r->dummy_texture) {
            continue;
        }
        hwaddr tex_start = b->key.texture_vram_offset;
        hwaddr tex_end = tex_start + b->key.texture_length;
        if (r->color_binding) {
            hwaddr rt_start = r->color_binding->vram_addr;
            hwaddr rt_end = rt_start + r->color_binding->size;
            if (tex_start < rt_end && rt_start < tex_end) {
                return true;
            }
        }
        if (r->zeta_binding) {
            hwaddr zt_start = r->zeta_binding->vram_addr;
            hwaddr zt_end = zt_start + r->zeta_binding->size;
            if (tex_start < zt_end && zt_start < tex_end) {
                return true;
            }
        }
    }
    return false;
}
#endif

#if OPT_REORDER_SAFE_WINDOWS
static bool classify_draw_safe(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);

    if (blend & NV_PGRAPH_BLEND_EN) {
        uint32_t src = GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dst = GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR);
        if (src != NV_PGRAPH_BLEND_SFACTOR_ONE ||
            dst != NV_PGRAPH_BLEND_DFACTOR_ZERO) {
            OPT_STAT_INC(reorder_reject_blend);
            return false;
        }
    }

    if ((control_0 & (NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)) !=
                     (NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                      NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)) {
        OPT_STAT_INC(reorder_reject_no_color_write);
        return false;
    }

    if (!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE)) {
        OPT_STAT_INC(reorder_reject_no_depth);
        return false;
    }
    if (!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)) {
        OPT_STAT_INC(reorder_reject_no_zwrite);
        return false;
    }

    uint32_t zfunc = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
    if (zfunc != NV_PGRAPH_CONTROL_0_ZFUNC_LESS &&
        zfunc != NV_PGRAPH_CONTROL_0_ZFUNC_LEQUAL) {
        OPT_STAT_INC(reorder_reject_zfunc);
        return false;
    }

    if (control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) {
        OPT_STAT_INC(reorder_reject_stencil);
        return false;
    }

    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) {
        uint32_t afunc = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAFUNC);
        uint32_t aref  = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ALPHAREF);
        bool safe_alpha = (afunc == ALPHA_FUNC_ALWAYS) ||
                          (afunc == ALPHA_FUNC_GREATER && aref == 0);
        if (!safe_alpha) {
            OPT_STAT_INC(reorder_reject_alpha);
            return false;
        }
    }

    /* Alphakill (texture.a == 0.0 → discard) is theoretically safe:
     * discarded fragments write nothing, survivors write color+depth,
     * depth test resolves order. Kept open for investigation. */

    if (check_rt_as_texture_hazard(pg)) {
        OPT_STAT_INC(reorder_reject_rtt);
        return false;
    }

    OPT_STAT_INC(reorder_safe_draws);
    if (zfunc == NV_PGRAPH_CONTROL_0_ZFUNC_LESS) {
        OPT_STAT_INC(reorder_safe_zfunc_less);
    } else {
        OPT_STAT_INC(reorder_safe_zfunc_lequal);
    }
    return true;
}
#endif

#if OPT_DRAW_MERGING
static bool check_draw_mergeable(PGRAPHState *pg, DrawQueue *q)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (pg->shader_state_gen != q->shader_state_gen ||
        pg->pipeline_state_gen != q->pipeline_state_gen ||
        pg->texture_state_gen != q->texture_state_gen ||
        pg->vertex_attr_gen != q->vertex_attr_gen ||
        r->texture_vram_gen != q->texture_vram_gen ||
        r->framebuffer_dirty ||
        pg->program_data_dirty ||
        pg->primitive_mode != q->primitive_mode) {
        return false;
    }

    if (check_rt_as_texture_hazard(pg)) {
        return false;
    }

    return true;
}

static bool upload_draw_uniforms(PGRAPHState *pg, size_t offsets_out[2])
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderBinding *binding = r->shader_binding;
    if (!binding) {
        return false;
    }

    pgraph_vk_update_shader_uniforms(pg);

    ShaderUniformLayout *layouts[] = {
        &binding->vsh.module_info->uniforms,
        &binding->psh.module_info->uniforms,
    };

    VkDeviceSize total = 0;
    for (int i = 0; i < 2; i++) {
        total += layouts[i]->total_size;
    }

    if (!pgraph_vk_buffer_has_space_for(
            pg, BUFFER_UNIFORM_STAGING, total,
            r->device_props.limits.minUniformBufferOffsetAlignment)) {
        return false;
    }

    for (int i = 0; i < 2; i++) {
        void *data = layouts[i]->allocation;
        VkDeviceSize size = layouts[i]->total_size;
        offsets_out[i] = pgraph_vk_append_to_buffer(
            pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
            r->device_props.limits.minUniformBufferOffsetAlignment);
    }

    return true;
}

static bool try_enqueue_draw_arrays(PGRAPHState *pg, DrawQueue *q)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int new_count = q->count + (int)pg->draw_arrays_length;
    if (new_count > DRAW_QUEUE_MAX) {
        return false;
    }

    bool uniforms_changed = (q->count > 0 &&
                             pg->any_reg_gen != q->any_reg_gen);

    size_t ubo_offsets[2];
    if (uniforms_changed || (q->count == 0 && r->shader_binding)) {
        if (!upload_draw_uniforms(pg, ubo_offsets)) {
            return false;
        }
    } else if (q->count == 0) {
        ubo_offsets[0] = r->uniform_buffer_offsets[0];
        ubo_offsets[1] = r->uniform_buffer_offsets[1];
    } else {
        ubo_offsets[0] = q->entries[q->count - 1].uniform_offsets[0];
        ubo_offsets[1] = q->entries[q->count - 1].uniform_offsets[1];
    }

    for (int i = 0; i < (int)pg->draw_arrays_length; i++) {
        DrawQueueEntry *e = &q->entries[q->count + i];
        e->first_vertex = pg->draw_arrays_start[i];
        e->vertex_count = pg->draw_arrays_count[i];
        e->uniform_offsets[0] = ubo_offsets[0];
        e->uniform_offsets[1] = ubo_offsets[1];

        uint32_t start = (uint32_t)pg->draw_arrays_start[i];
        uint32_t end = start + (uint32_t)pg->draw_arrays_count[i];
        if (q->count == 0 && i == 0) {
            q->min_start = start;
            q->max_end = end;
        } else {
            if (start < q->min_start) q->min_start = start;
            if (end > q->max_end) q->max_end = end;
        }
    }

    if (q->count == 0) {
        q->shader_state_gen = pg->shader_state_gen;
        q->pipeline_state_gen = pg->pipeline_state_gen;
        q->texture_state_gen = pg->texture_state_gen;
        q->any_reg_gen = pg->any_reg_gen;
        q->vertex_attr_gen = pg->vertex_attr_gen;
        q->texture_vram_gen = r->texture_vram_gen;
        q->primitive_mode = pg->primitive_mode;
        q->active = true;
        q->has_uniform_changes = false;
    }

    if (uniforms_changed) {
        q->has_uniform_changes = true;
        q->any_reg_gen = pg->any_reg_gen;
    }

    q->count = new_count;
    return true;
}

#if OPT_INDEXED_DRAW_MERGING
static bool try_enqueue_draw_indexed(PGRAPHState *pg, DrawQueue *q)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (q->count + 1 > DRAW_QUEUE_MAX) {
        return false;
    }

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    uint32_t *draw_indices = pg->inline_elements;
    unsigned int draw_index_count = pg->inline_elements_length;
    PrimRewrite prim_rw = pgraph_prim_rewrite_indexed(
        &r->prim_rewrite_buf, assembly,
        pg->inline_elements, pg->inline_elements_length);
    if (prim_rw.num_indices > 0) {
        draw_indices = prim_rw.indices;
        draw_index_count = prim_rw.num_indices;
    }

    unsigned int verts_per_prim;
    switch (pgraph_prim_rewrite_get_output_mode(
                assembly.primitive_mode, assembly.polygon_mode)) {
    case PRIM_TYPE_TRIANGLES: verts_per_prim = 3; break;
    case PRIM_TYPE_LINES:     verts_per_prim = 2; break;
    default:                  verts_per_prim = 1; break;
    }
    draw_index_count = (draw_index_count / verts_per_prim) * verts_per_prim;
    if (draw_index_count == 0) {
        return true;
    }

    if (q->total_indices + draw_index_count > INDEX_QUEUE_MAX) {
        return false;
    }

    uint32_t local_min = UINT32_MAX;
    uint32_t local_max = 0;
    for (unsigned int i = 0; i < draw_index_count; i++) {
        if (draw_indices[i] > local_max) local_max = draw_indices[i];
        if (draw_indices[i] < local_min) local_min = draw_indices[i];
    }

    bool uniforms_changed = (q->count > 0 &&
                             pg->any_reg_gen != q->any_reg_gen);

    size_t ubo_offsets[2];
    if (uniforms_changed || (q->count == 0 && r->shader_binding)) {
        if (!upload_draw_uniforms(pg, ubo_offsets)) {
            return false;
        }
    } else if (q->count == 0) {
        ubo_offsets[0] = r->uniform_buffer_offsets[0];
        ubo_offsets[1] = r->uniform_buffer_offsets[1];
    } else {
        ubo_offsets[0] = q->entries[q->count - 1].uniform_offsets[0];
        ubo_offsets[1] = q->entries[q->count - 1].uniform_offsets[1];
    }

    memcpy(q->index_buf + q->total_indices, draw_indices,
           draw_index_count * sizeof(uint32_t));

    DrawQueueEntry *e = &q->entries[q->count];
    e->index_offset = q->total_indices;
    e->index_count = draw_index_count;
    e->uniform_offsets[0] = ubo_offsets[0];
    e->uniform_offsets[1] = ubo_offsets[1];

    q->total_indices += draw_index_count;

    if (q->count == 0) {
        q->min_element = local_min;
        q->max_element = local_max;
        q->shader_state_gen = pg->shader_state_gen;
        q->pipeline_state_gen = pg->pipeline_state_gen;
        q->texture_state_gen = pg->texture_state_gen;
        q->any_reg_gen = pg->any_reg_gen;
        q->vertex_attr_gen = pg->vertex_attr_gen;
        q->texture_vram_gen = r->texture_vram_gen;
        q->primitive_mode = pg->primitive_mode;
        q->active = true;
        q->indexed = true;
        q->has_uniform_changes = false;
    } else {
        if (local_min < q->min_element) q->min_element = local_min;
        if (local_max > q->max_element) q->max_element = local_max;
    }

    if (uniforms_changed) {
        q->has_uniform_changes = true;
        q->any_reg_gen = pg->any_reg_gen;
    }

    q->count++;
    return true;
}
#endif

static void flush_draw_queue_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    DrawQueue *q = &r->draw_queue;

    if (q->count == 0) {
        return;
    }

    int entry_count = q->count;
    int prim_mode = q->primitive_mode;
    bool has_uniform_changes = q->has_uniform_changes;
    bool is_indexed = q->indexed;

    size_t ubo_offsets[DRAW_QUEUE_MAX][2];
    for (int i = 0; i < entry_count; i++) {
        ubo_offsets[i][0] = q->entries[i].uniform_offsets[0];
        ubo_offsets[i][1] = q->entries[i].uniform_offsets[1];
    }

    int32_t starts[DRAW_QUEUE_MAX];
    int32_t counts_arr[DRAW_QUEUE_MAX];
    uint32_t idx_offsets[DRAW_QUEUE_MAX];
    uint32_t idx_counts[DRAW_QUEUE_MAX];
    uint32_t total_indices = 0;
    uint32_t min_start = 0, max_end = 0;
    uint32_t min_element = 0, max_element = 0;

    if (is_indexed) {
        total_indices = q->total_indices;
        min_element = q->min_element;
        max_element = q->max_element;
        for (int i = 0; i < entry_count; i++) {
            idx_offsets[i] = q->entries[i].index_offset;
            idx_counts[i] = q->entries[i].index_count;
        }
    } else {
        min_start = q->min_start;
        max_end = q->max_end;
        for (int i = 0; i < entry_count; i++) {
            starts[i] = q->entries[i].first_vertex;
            counts_arr[i] = q->entries[i].vertex_count;
        }
    }

    q->count = 0;
    q->active = false;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }

    int saved_primitive_mode = pg->primitive_mode;
    uint32_t saved_shader_gen = pg->shader_state_gen;
    uint32_t saved_pipeline_gen = pg->pipeline_state_gen;
    uint32_t saved_texture_gen = pg->texture_state_gen;
    uint32_t saved_any_reg_gen = pg->any_reg_gen;
    uint32_t saved_vertex_attr_gen = pg->vertex_attr_gen;
    uint32_t saved_texture_vram_gen = r->texture_vram_gen;
    bool saved_fb_dirty = r->framebuffer_dirty;
    bool saved_prog_dirty = pg->program_data_dirty;

    pg->primitive_mode = prim_mode;
    pg->shader_state_gen = q->shader_state_gen;
    pg->pipeline_state_gen = q->pipeline_state_gen;
    pg->texture_state_gen = q->texture_state_gen;
    pg->any_reg_gen = q->any_reg_gen;
    pg->vertex_attr_gen = q->vertex_attr_gen;
    r->texture_vram_gen = q->texture_vram_gen;
    r->framebuffer_dirty = false;
    pg->program_data_dirty = false;

    r->num_vertex_ram_buffer_syncs = 0;

    if (is_indexed) {
        uint32_t provoking = (total_indices > 0)
            ? q->index_buf[total_indices - 1]
            : max_element;
        pgraph_vk_bind_vertex_attributes(d, min_element, max_element,
                                         false, 0, provoking);

        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap =
            remap_unaligned_attributes(pg, max_element + 1);

        size_t index_data_size = total_indices * sizeof(uint32_t);
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0,
                                                  max_element + 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Merged Indexed (%d)", entry_count);
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);

        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, q->index_buf, index_data_size);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);

        {
            int ds_index = r->descriptor_set_index - 1;
            assert(ds_index >= 0);
            for (int i = 0; i < entry_count; i++) {
                uint32_t dyn_off[2] = {
                    (uint32_t)ubo_offsets[i][0],
                    (uint32_t)ubo_offsets[i][1]
                };
                vkCmdBindDescriptorSets(
                    r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    r->pipeline_binding->layout, 0, 1,
                    &r->descriptor_sets[ds_index], 2, dyn_off);
                vkCmdDrawIndexed(r->command_buffer, idx_counts[i], 1,
                                 idx_offsets[i], 0, 0);
            }
        }
    } else {
        PrimAssemblyState assembly = {
            .primitive_mode = prim_mode,
            .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
                pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
            .last_provoking =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                         NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
            .flat_shading =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                         NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
        };

        pgraph_vk_bind_vertex_attributes(d, min_start, max_end - 1,
                                         false, 0, max_end - 1);

        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_end);

        PrimRewrite prim_rw = pgraph_prim_rewrite_ranges(
            &r->prim_rewrite_buf, assembly,
            starts, counts_arr, entry_count);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        } else if (entry_count > 1) {
            size_t indirect_size =
                entry_count * sizeof(VkDrawIndirectCommand);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, indirect_size);
        }

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_end);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Merged Draw Arrays (%d)", entry_count);
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 buffer_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices,
                             1, 0, 0, 0);
        } else if (!has_uniform_changes && entry_count > 1) {
            VkDrawIndirectCommand cmds[DRAW_QUEUE_MAX];
            for (int i = 0; i < entry_count; i++) {
                cmds[i] = (VkDrawIndirectCommand){
                    .vertexCount = counts_arr[i],
                    .instanceCount = 1,
                    .firstVertex = starts[i],
                    .firstInstance = 0,
                };
            }
            size_t indirect_size =
                entry_count * sizeof(VkDrawIndirectCommand);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, indirect_size);
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, cmds, indirect_size);
            vkCmdDrawIndirect(r->command_buffer,
                              r->storage_buffers[BUFFER_INDEX].buffer,
                              buffer_offset, entry_count,
                              sizeof(VkDrawIndirectCommand));
        } else if (has_uniform_changes) {
            int ds_index = r->descriptor_set_index - 1;
            assert(ds_index >= 0);

            int i = 0;
            while (i < entry_count) {
                size_t cur_off0 = ubo_offsets[i][0];
                size_t cur_off1 = ubo_offsets[i][1];

                uint32_t dyn_off[2] = {
                    (uint32_t)cur_off0, (uint32_t)cur_off1
                };
                vkCmdBindDescriptorSets(
                    r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    r->pipeline_binding->layout, 0, 1,
                    &r->descriptor_sets[ds_index], 2, dyn_off);

                int j = i;
                while (j < entry_count &&
                       ubo_offsets[j][0] == cur_off0 &&
                       ubo_offsets[j][1] == cur_off1) {
                    j++;
                }

                int group_count = j - i;
                if (group_count > 1) {
                    VkDrawIndirectCommand cmds[DRAW_QUEUE_MAX];
                    for (int k = 0; k < group_count; k++) {
                        cmds[k] = (VkDrawIndirectCommand){
                            .vertexCount = counts_arr[i + k],
                            .instanceCount = 1,
                            .firstVertex = starts[i + k],
                            .firstInstance = 0,
                        };
                    }
                    size_t indirect_size =
                        group_count * sizeof(VkDrawIndirectCommand);
                    ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                        indirect_size);
                    VkDeviceSize buf_off = pgraph_vk_update_index_buffer(
                        pg, cmds, indirect_size);
                    vkCmdDrawIndirect(r->command_buffer,
                                      r->storage_buffers[BUFFER_INDEX].buffer,
                                      buf_off, group_count,
                                      sizeof(VkDrawIndirectCommand));
                } else {
                    vkCmdDraw(r->command_buffer, counts_arr[i], 1,
                              starts[i], 0);
                }

                i = j;
            }
        } else {
            vkCmdDraw(r->command_buffer, counts_arr[0], 1, starts[0], 0);
        }
    }

    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->primitive_mode = saved_primitive_mode;
    pg->shader_state_gen = saved_shader_gen;
    pg->pipeline_state_gen = saved_pipeline_gen;
    pg->texture_state_gen = saved_texture_gen;
    pg->any_reg_gen = saved_any_reg_gen;
    pg->vertex_attr_gen = saved_vertex_attr_gen;
    r->texture_vram_gen = saved_texture_vram_gen;
    r->framebuffer_dirty = saved_fb_dirty;
    pg->program_data_dirty = saved_prog_dirty;
}
#endif

void pgraph_vk_flush_draw_queue(NV2AState *d)
{
#if OPT_DRAW_MERGING
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (r->draw_queue.count > 0) {
        flush_draw_queue_internal(d);
    }
    r->draw_queue.active = false;
#endif
}

#if OPT_REORDER_SAFE_WINDOWS

static void snapshot_vertex_buffers(PGRAPHState *pg, ReorderWindowEntry *e,
                                    uint16_t inline_map, VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    e->num_vertex_bindings = r->num_active_vertex_binding_descriptions;
    for (int i = 0; i < e->num_vertex_bindings; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        e->vertex_buffers[i] = r->storage_buffers[buffer_idx].buffer;
        e->vertex_offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }
}

static void snapshot_dynamic_state(PGRAPHState *pg, ReorderWindowEntry *e)
{
    e->dyn_setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    e->dyn_blendcolor = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
    e->dyn_control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    e->dyn_control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    e->dyn_control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    e->viewport = (VkViewport){
        .width = vp_width, .height = vp_height,
        .minDepth = 0.0, .maxDepth = 1.0,
    };

    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;
    unsigned int sw = pg->surface_shape.clip_width,
                 sh = pg->surface_shape.clip_height;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &sw, &sh);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &sw, &sh);
    e->scissor = (VkRect2D){
        .offset = { .x = xmin, .y = ymin },
        .extent = { .width = sw, .height = sh },
    };
}

static void snapshot_push_constants(PGRAPHState *pg, ReorderWindowEntry *e)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    e->use_push_constants = r->use_push_constants_for_uniform_attrs;
    e->num_push_values = 0;
    if (!e->use_push_constants || !r->shader_binding) {
        return;
    }
    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num = 0;
    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num);
    e->num_push_values = num;
    if (num > 0) {
        memcpy(e->push_values, values, num * 4 * sizeof(float));
    }
}

static bool try_snapshot_draw_arrays(NV2AState *d, ReorderWindowEntry *e)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return false;
    }

    nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
    r->num_vertex_ram_buffer_syncs = 0;

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                     pg->draw_arrays_max_count - 1, false,
                                     0, pg->draw_arrays_max_count - 1);
    uint32_t max_element = 0;
    for (int i = 0; i < pg->draw_arrays_length; i++) {
        max_element = MAX(max_element,
                         pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
    }

    sync_vertex_ram_buffer(pg);
    VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

    PrimRewrite prim_rw = pgraph_prim_rewrite_ranges(
        &r->prim_rewrite_buf, assembly,
        pg->draw_arrays_start, pg->draw_arrays_count,
        pg->draw_arrays_length);

    if (prim_rw.num_indices > 0) {
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                            prim_rw.num_indices * sizeof(uint32_t));
    } else if (pg->draw_arrays_length > 1) {
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                            pg->draw_arrays_length *
                            sizeof(VkDrawIndirectCommand));
    }

    r->need_descriptor_rebind = true;
    begin_pre_draw(pg);
    copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);

    e->pipeline_binding = r->pipeline_binding;
    e->layout = r->pipeline_binding->layout;
    e->has_dynamic_line_width = r->pipeline_binding->has_dynamic_line_width;
    if (e->has_dynamic_line_width) {
        e->line_width =
            clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
    }

    e->descriptor_set = r->descriptor_sets[r->descriptor_set_index - 1];
    e->dynamic_offsets[0] = (uint32_t)r->uniform_buffer_offsets[0];
    e->dynamic_offsets[1] = (uint32_t)r->uniform_buffer_offsets[1];
    e->pre_draw_skipped = false;

    snapshot_vertex_buffers(pg, e, remap.attributes, 0);
    snapshot_dynamic_state(pg, e);
    snapshot_push_constants(pg, e);

    if (prim_rw.num_indices > 0) {
        e->draw_mode = RW_DRAW_INDEXED;
        e->draw_count = prim_rw.num_indices;
        e->index_indirect_offset = pgraph_vk_update_index_buffer(
            pg, prim_rw.indices, prim_rw.num_indices * sizeof(uint32_t));
    } else if (pg->draw_arrays_length > 1) {
        e->draw_mode = RW_DRAW_INDIRECT;
        VkDrawIndirectCommand cmds[pg->draw_arrays_length];
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            cmds[i] = (VkDrawIndirectCommand){
                .vertexCount = pg->draw_arrays_count[i],
                .instanceCount = 1,
                .firstVertex = pg->draw_arrays_start[i],
                .firstInstance = 0,
            };
        }
        size_t indirect_size =
            pg->draw_arrays_length * sizeof(VkDrawIndirectCommand);
        e->draw_count = pg->draw_arrays_length;
        e->index_indirect_offset =
            pgraph_vk_update_index_buffer(pg, cmds, indirect_size);
    } else {
        e->draw_mode = RW_DRAW_DIRECT;
        e->vertex_count = pg->draw_arrays_count[0];
        e->first_vertex = pg->draw_arrays_start[0];
        e->draw_count = 1;
    }

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    e->color_write =
        (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    e->depth_test = !!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    e->stencil_test = !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                         NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);

    return true;
}

static bool try_snapshot_inline_elements(NV2AState *d, ReorderWindowEntry *e)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return false;
    }

    nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    uint32_t *draw_indices = pg->inline_elements;
    unsigned int draw_index_count = pg->inline_elements_length;
    PrimRewrite prim_rw = pgraph_prim_rewrite_indexed(
        &r->prim_rewrite_buf, assembly,
        pg->inline_elements, pg->inline_elements_length);
    if (prim_rw.num_indices > 0) {
        draw_indices = prim_rw.indices;
        draw_index_count = prim_rw.num_indices;
    }

    size_t index_data_size = draw_index_count * sizeof(uint32_t);
    ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

    uint32_t min_element = (uint32_t)-1;
    uint32_t max_element = 0;
    for (unsigned int i = 0; i < draw_index_count; i++) {
        max_element = MAX(draw_indices[i], max_element);
        min_element = MIN(draw_indices[i], min_element);
    }

    pgraph_vk_bind_vertex_attributes(
        d, min_element, max_element, false, 0,
        draw_indices[draw_index_count - 1]);

#if OPT_SYNC_RANGE_SKIP
    if (sync_range_covers(r, pg->vertex_attr_gen, min_element, max_element)) {
        r->num_vertex_ram_buffer_syncs = 0;
    } else {
        sync_vertex_ram_buffer(pg);
        sync_range_update(r, pg->vertex_attr_gen, min_element, max_element);
    }
#else
    sync_vertex_ram_buffer(pg);
#endif
    VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

    r->need_descriptor_rebind = true;
    begin_pre_draw(pg);
    copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);

    e->pipeline_binding = r->pipeline_binding;
    e->layout = r->pipeline_binding->layout;
    e->has_dynamic_line_width = r->pipeline_binding->has_dynamic_line_width;
    if (e->has_dynamic_line_width) {
        e->line_width =
            clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
    }

    e->descriptor_set = r->descriptor_sets[r->descriptor_set_index - 1];
    e->dynamic_offsets[0] = (uint32_t)r->uniform_buffer_offsets[0];
    e->dynamic_offsets[1] = (uint32_t)r->uniform_buffer_offsets[1];
    e->pre_draw_skipped = false;

    snapshot_vertex_buffers(pg, e, remap.attributes, 0);
    snapshot_dynamic_state(pg, e);
    snapshot_push_constants(pg, e);

    e->draw_mode = RW_DRAW_INDEXED;
    e->draw_count = draw_index_count;
    e->index_indirect_offset = pgraph_vk_update_index_buffer(
        pg, draw_indices, index_data_size);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    e->color_write =
        (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ||
        (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    e->depth_test = !!(control_0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    e->stencil_test = !!(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                         NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);

    return true;
}

static int compare_reorder_entries(const void *a, const void *b)
{
    const ReorderWindowEntry *ea = a;
    const ReorderWindowEntry *eb = b;
    if (ea->group_order < eb->group_order) return -1;
    if (ea->group_order > eb->group_order) return 1;
    return ea->sequence_number - eb->sequence_number;
}

static void emit_reorder_entry(PGRAPHState *pg, ReorderWindowEntry *e,
                                PipelineBinding *prev_pipeline)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool pipeline_changed = (e->pipeline_binding != prev_pipeline);

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        pipeline_changed = true;
    }

    if (pipeline_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          e->pipeline_binding->pipeline);
        e->pipeline_binding->draw_time = pg->draw_time;
        vkCmdSetViewport(r->command_buffer, 0, 1, &e->viewport);
        vkCmdSetScissor(r->command_buffer, 0, 1, &e->scissor);
        if (e->has_dynamic_line_width) {
            vkCmdSetLineWidth(r->command_buffer, e->line_width);
        }
    }

#if OPT_DYNAMIC_STATES
    {
        VkCullModeFlags cull = VK_CULL_MODE_NONE;
        if (e->dyn_setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
            uint32_t cull_face = GET_MASK(e->dyn_setupraster,
                                          NV_PGRAPH_SETUPRASTER_CULLCTRL);
            assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
            cull = pgraph_cull_face_vk_map[cull_face];
        }
        vkCmdSetCullMode(r->command_buffer, cull);
        vkCmdSetFrontFace(r->command_buffer,
                          (e->dyn_setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE)
                              ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                              : VK_FRONT_FACE_CLOCKWISE);

        float blend_constant[4] = { 0, 0, 0, 0 };
        pgraph_argb_pack32_to_rgba_float(e->dyn_blendcolor, blend_constant);
        vkCmdSetBlendConstants(r->command_buffer, blend_constant);

        if (OPT_DYNAMIC_DEPTH_STENCIL && r->extended_dynamic_state_supported) {
            vkCmdSetDepthTestEnable(r->command_buffer,
                (e->dyn_control_0 & NV_PGRAPH_CONTROL_0_ZENABLE) ? VK_TRUE
                                                                  : VK_FALSE);
            vkCmdSetDepthWriteEnable(r->command_buffer,
                (e->dyn_control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE)
                    ? VK_TRUE : VK_FALSE);
            uint32_t dfunc = GET_MASK(e->dyn_control_0,
                                      NV_PGRAPH_CONTROL_0_ZFUNC);
            assert(dfunc < ARRAY_SIZE(pgraph_depth_func_vk_map));
            vkCmdSetDepthCompareOp(r->command_buffer,
                                   pgraph_depth_func_vk_map[dfunc]);

            bool sten = e->dyn_control_1 &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
            vkCmdSetStencilTestEnable(r->command_buffer,
                                      sten ? VK_TRUE : VK_FALSE);
            uint32_t sfunc = GET_MASK(e->dyn_control_1,
                                      NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            assert(sfunc < ARRAY_SIZE(pgraph_stencil_func_vk_map));
            uint32_t sref = GET_MASK(e->dyn_control_1,
                                     NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t mr = GET_MASK(e->dyn_control_1,
                                   NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t mw = GET_MASK(e->dyn_control_1,
                                   NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            vkCmdSetStencilCompareMask(r->command_buffer,
                                       VK_STENCIL_FACE_FRONT_AND_BACK, mr);
            vkCmdSetStencilWriteMask(r->command_buffer,
                                     VK_STENCIL_FACE_FRONT_AND_BACK, mw);
            vkCmdSetStencilReference(r->command_buffer,
                                     VK_STENCIL_FACE_FRONT_AND_BACK, sref);
            uint32_t op_fail = GET_MASK(e->dyn_control_2,
                                        NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(e->dyn_control_2,
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(e->dyn_control_2,
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            vkCmdSetStencilOp(r->command_buffer,
                              VK_STENCIL_FACE_FRONT_AND_BACK,
                              pgraph_stencil_op_vk_map[op_fail],
                              pgraph_stencil_op_vk_map[op_zpass],
                              pgraph_stencil_op_vk_map[op_zfail],
                              pgraph_stencil_func_vk_map[sfunc]);
        } else {
            uint32_t sref = GET_MASK(e->dyn_control_1,
                                     NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t mr = GET_MASK(e->dyn_control_1,
                                   NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t mw = GET_MASK(e->dyn_control_1,
                                   NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            vkCmdSetStencilCompareMask(r->command_buffer,
                                       VK_STENCIL_FACE_FRONT_AND_BACK, mr);
            vkCmdSetStencilWriteMask(r->command_buffer,
                                     VK_STENCIL_FACE_FRONT_AND_BACK, mw);
            vkCmdSetStencilReference(r->command_buffer,
                                     VK_STENCIL_FACE_FRONT_AND_BACK, sref);
        }
    }
#endif

    {
        vkCmdBindDescriptorSets(r->command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                e->layout, 0, 1, &e->descriptor_set,
                                2, e->dynamic_offsets);
        if (e->use_push_constants && e->num_push_values > 0) {
            vkCmdPushConstants(r->command_buffer, e->layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               e->num_push_values * 4 * sizeof(float),
                               e->push_values);
        }
    }

    if (e->num_vertex_bindings > 0) {
        vkCmdBindVertexBuffers(r->command_buffer, 0,
                               e->num_vertex_bindings,
                               e->vertex_buffers, e->vertex_offsets);
    }

    switch (e->draw_mode) {
    case RW_DRAW_INDEXED:
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             e->index_indirect_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, e->draw_count, 1, 0, 0, 0);
        break;
    case RW_DRAW_INDIRECT:
        vkCmdDrawIndirect(r->command_buffer,
                          r->storage_buffers[BUFFER_INDEX].buffer,
                          e->index_indirect_offset, e->draw_count,
                          sizeof(VkDrawIndirectCommand));
        break;
    case RW_DRAW_DIRECT:
        vkCmdDraw(r->command_buffer, e->vertex_count, 1, e->first_vertex, 0);
        break;
    }
}

static void flush_reorder_window_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    ReorderWindow *w = &r->reorder_window;

    if (w->count == 0) {
        return;
    }

    int original_switches = 0;
    for (int i = 1; i < w->count; i++) {
        if (w->entries[i].pipeline_binding !=
            w->entries[i - 1].pipeline_binding) {
            original_switches++;
        }
    }

    qsort(w->entries, w->count, sizeof(ReorderWindowEntry),
          compare_reorder_entries);

    int sorted_switches = 0;
    for (int i = 1; i < w->count; i++) {
        if (w->entries[i].pipeline_binding !=
            w->entries[i - 1].pipeline_binding) {
            sorted_switches++;
        }
    }

    OPT_STAT_INC(reorder_windows_flushed);
    g_opt_stats.reorder_draws_reordered += w->count;
    g_opt_stats.reorder_pipeline_switches_saved +=
        (original_switches - sorted_switches);

    nv2a_profile_inc_counter(NV2A_PROF_REORDER_WINDOW_FLUSH);
    g_nv2a_stats.frame_working.counters[NV2A_PROF_REORDER_DRAWS] += w->count;

    pgraph_vk_ensure_command_buffer(pg);

    r->dyn_state.valid = false;

    PipelineBinding *prev_pipeline = NULL;
    for (int i = 0; i < w->count; i++) {
        ReorderWindowEntry *e = &w->entries[i];
        emit_reorder_entry(pg, e, prev_pipeline);
        prev_pipeline = e->pipeline_binding;

        pg->draw_time++;
        if (r->color_binding && e->color_write) {
            r->color_binding->draw_time = pg->draw_time;
        }
        if (r->zeta_binding && (e->depth_test || e->stencil_test)) {
            r->zeta_binding->draw_time = pg->draw_time;
        }
        pgraph_vk_set_surface_dirty(pg, e->color_write,
                                    e->depth_test || e->stencil_test);
    }

    r->pipeline_binding = prev_pipeline;
    r->pipeline_binding_changed = false;
    r->color_drawn_in_cb = r->color_drawn_in_cb || (r->color_binding != NULL);
    r->zeta_drawn_in_cb = r->zeta_drawn_in_cb || (r->zeta_binding != NULL);

    w->count = 0;
    w->active = false;
    w->num_seen_pipelines = 0;
    w->next_group = 0;
}

#endif

void pgraph_vk_flush_reorder_window(NV2AState *d)
{
#if OPT_REORDER_SAFE_WINDOWS
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (r->reorder_window.count > 0) {
        flush_reorder_window_internal(d);
    }
    r->reorder_window.active = false;
#endif
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

#if OPT_REORDER_SAFE_WINDOWS
    if (g_xemu_draw_reorder &&
        (pg->draw_arrays_length || pg->inline_elements_length) && !pg->clearing) {
        ReorderWindow *w = &r->reorder_window;
        bool is_safe = classify_draw_safe(pg);
        if (is_safe && r->framebuffer_dirty) {
            OPT_STAT_INC(reorder_reject_fb_dirty);
            is_safe = false;
        }
        if (is_safe && pg->zpass_pixel_count_enable) {
            OPT_STAT_INC(reorder_reject_zpass);
            is_safe = false;
        }

        if (is_safe && w->count < REORDER_WINDOW_MAX) {
            pgraph_vk_flush_draw_queue(d);

            ReorderWindowEntry *e = &w->entries[w->count];
            bool ok;
            if (pg->draw_arrays_length) {
                ok = try_snapshot_draw_arrays(d, e);
            } else {
                ok = try_snapshot_inline_elements(d, e);
            }
            if (ok) {
                e->sequence_number = w->count;

                int grp = -1;
                for (int i = 0; i < w->num_seen_pipelines; i++) {
                    if (w->seen_pipelines[i] == e->pipeline_binding) {
                        grp = w->seen_pipeline_group[i];
                        break;
                    }
                }
                if (grp < 0) {
                    if (w->num_seen_pipelines < REORDER_MAX_PIPELINES) {
                        w->seen_pipelines[w->num_seen_pipelines] = e->pipeline_binding;
                        w->seen_pipeline_group[w->num_seen_pipelines] = w->next_group;
                        w->num_seen_pipelines++;
                    }
                    grp = w->next_group++;
                }
                e->group_order = grp;

                w->count++;
                w->active = true;
                return;
            }
        }

        if (w->count > 0) {
            flush_reorder_window_internal(d);
        }
        w->active = false;
    } else if (r->reorder_window.count > 0) {
        flush_reorder_window_internal(d);
        r->reorder_window.active = false;
    }
#endif

#if OPT_DRAW_MERGING
    if (pg->draw_arrays_length && !pg->clearing) {
        DrawQueue *q = &r->draw_queue;

        if (q->active) {
            if (q->indexed || !check_draw_mergeable(pg, q)) {
                if (q->count > 0) {
                    flush_draw_queue_internal(d);
                }
                q->active = false;
            }
        }

        if (q->active) {
            if (q->count >= OPT_DRAW_MERGE_MAX ||
                !try_enqueue_draw_arrays(pg, q)) {
                flush_draw_queue_internal(d);
                q->active = false;
            } else {
                goto post_draw;
            }
        }

        pgraph_vk_flush_draw(d);

        q->shader_state_gen = pg->shader_state_gen;
        q->pipeline_state_gen = pg->pipeline_state_gen;
        q->texture_state_gen = pg->texture_state_gen;
        q->any_reg_gen = pg->any_reg_gen;
        q->vertex_attr_gen = pg->vertex_attr_gen;
        q->texture_vram_gen = r->texture_vram_gen;
        q->primitive_mode = pg->primitive_mode;
        q->active = true;
        q->indexed = false;
        q->has_uniform_changes = false;
        q->count = 0;

        goto post_draw;
    }

#if OPT_INDEXED_DRAW_MERGING
    if (pg->inline_elements_length && !pg->clearing) {
        DrawQueue *q = &r->draw_queue;

        if (q->active) {
            if (!q->indexed || !check_draw_mergeable(pg, q)) {
                if (q->count > 0) {
                    flush_draw_queue_internal(d);
                }
                q->active = false;
            }
        }

        if (q->active) {
            if (q->count >= OPT_DRAW_MERGE_MAX ||
                !try_enqueue_draw_indexed(pg, q)) {
                flush_draw_queue_internal(d);
                q->active = false;
            } else {
                goto post_draw;
            }
        }

        pgraph_vk_flush_draw(d);

        q->shader_state_gen = pg->shader_state_gen;
        q->pipeline_state_gen = pg->pipeline_state_gen;
        q->texture_state_gen = pg->texture_state_gen;
        q->any_reg_gen = pg->any_reg_gen;
        q->vertex_attr_gen = pg->vertex_attr_gen;
        q->texture_vram_gen = r->texture_vram_gen;
        q->primitive_mode = pg->primitive_mode;
        q->active = true;
        q->indexed = true;
        q->has_uniform_changes = false;
        q->total_indices = 0;
        q->min_element = UINT32_MAX;
        q->max_element = 0;
        q->count = 0;

        goto post_draw;
    }
#endif

    if (r->draw_queue.count > 0) {
        flush_draw_queue_internal(d);
    }
    r->draw_queue.active = false;
#endif

    pgraph_vk_flush_draw(d);

#if OPT_DRAW_MERGING
post_draw:
#endif
    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

#if OPT_SYNC_RANGE_SKIP
static inline bool sync_range_covers(PGRAPHVkState *r,
                                     uint32_t attr_gen,
                                     uint32_t min_el,
                                     uint32_t max_el)
{
    return attr_gen == r->sync_range_attr_gen &&
           min_el >= r->sync_range_min &&
           max_el <= r->sync_range_max;
}

static inline void sync_range_update(PGRAPHVkState *r,
                                     uint32_t attr_gen,
                                     uint32_t min_el,
                                     uint32_t max_el)
{
    if (attr_gen != r->sync_range_attr_gen) {
        r->sync_range_attr_gen = attr_gen;
        r->sync_range_min = min_el;
        r->sync_range_max = max_el;
    } else {
        r->sync_range_min = MIN(r->sync_range_min, min_el);
        r->sync_range_max = MAX(r->sync_range_max, max_el);
    }
}
#endif

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    VsyncTimingWork *vw = &g_nv2a_stats.vsync_working;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

#if OPT_SYNC_EARLY_EXIT
    /*
     * Fast path: if every page in the requested regions is already in the
     * uploaded_bitmap, the data was synced earlier in this command buffer
     * and the dirty bits were cleared at that time.  Skip the expensive
     * sort / merge / RCU dirty-bit walk entirely.
     */
    {
        unsigned long *bmp = get_uploaded_bitmap(r);
        bool all_uploaded = true;
        for (int i = 0; i < r->num_vertex_ram_buffer_syncs && all_uploaded; i++) {
            hwaddr addr = r->vertex_ram_buffer_syncs[i].addr;
            hwaddr size = r->vertex_ram_buffer_syncs[i].size;
            hwaddr start = addr & TARGET_PAGE_MASK;
            hwaddr end = ROUND_UP(addr + size, TARGET_PAGE_SIZE);
            size_t start_bit = start / TARGET_PAGE_SIZE;
            size_t end_bit = end / TARGET_PAGE_SIZE;
            if (find_next_zero_bit(bmp, end_bit, start_bit) < end_bit) {
                all_uploaded = false;
            }
        }
        if (all_uploaded) {
            r->num_vertex_ram_buffer_syncs = 0;
            return;
        }
    }
#endif

    vw->calls++;
    vw->reqs += r->num_vertex_ram_buffer_syncs;

    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    vw->merged += num_syncs;

    {
        ram_addr_t ram_base = r->vram_ram_addr;

        RCU_READ_LOCK_GUARD();
        DirtyMemoryBlocks *blocks =
            qatomic_rcu_read(&ram_list.dirty_memory[DIRTY_MEMORY_NV2A]);

        for (int i = 0; i < num_syncs; i++) {
            hwaddr addr = merged[i].addr;
            VkDeviceSize size = merged[i].size;

            NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

            ram_addr_t start = ram_base + addr;
            unsigned long page = start >> TARGET_PAGE_BITS;
            unsigned long end_page = (start + size) >> TARGET_PAGE_BITS;
            bool dirty = false;

            while (page < end_page) {
                unsigned long idx = page / DIRTY_MEMORY_BLOCK_SIZE;
                unsigned long ofs = page % DIRTY_MEMORY_BLOCK_SIZE;
                unsigned long num = MIN(end_page - page,
                                        DIRTY_MEMORY_BLOCK_SIZE - ofs);
                dirty |= bitmap_test_and_clear_atomic(
                    blocks->blocks[idx], ofs, num);
                page += num;
            }

            if (dirty) {
                NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
                /*
                 * Notify the TLB that dirty bits were cleared. Without this,
                 * the TLB keeps VRAM pages writable and future CPU writes
                 * bypass the notdirty handler, so dirty bits never get re-set.
                 * This causes stale vertex data (wrong UI/2D textures).
                 */
                physical_memory_dirty_bits_cleared(start, size);
                vw->dirty_count++;
                vw->bytes_copied += size;
                pgraph_vk_update_vertex_ram_buffer(pg, addr,
                                                   d->vram_ptr + addr, size);
            }
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_REORDER_SAFE_WINDOWS
    if (r->reorder_window.count > 0) {
        flush_reorder_window_internal(d);
    }
    r->reorder_window.active = false;
#endif
#if OPT_DRAW_MERGING
    if (r->draw_queue.count > 0) {
        flush_draw_queue_internal(d);
    }
    r->draw_queue.active = false;
#endif

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = get_staging_buffer(r, BUFFER_VERTEX_INLINE_STAGING);
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = get_staging_buffer(r, BUFFER_VERTEX_INLINE_STAGING);

    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    assert(buffer->mapped);

    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];

        for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            memcpy(out_ptr, in_ptr, remap.map[attr_id].new_stride);
            out_ptr += remap.map[attr_id].new_stride;
            in_ptr += remap.map[attr_id].old_stride;
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    NV2A_PHASE_TIMER_BEGIN(draw_dispatch);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        NV2A_PHASE_TIMER_END(draw_dispatch);
        return;
    }

    r->num_vertex_ram_buffer_syncs = 0;

    PrimAssemblyState assembly = {
        .primitive_mode = pg->primitive_mode,
        .polygon_mode = (enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE),
        .last_provoking = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                   NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_LAST,
        .flat_shading = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT,
    };

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_attr);
        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        NV2A_PHASE_TIMER_END(draw_vtx_attr);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_sync);
#if OPT_SYNC_RANGE_SKIP
        if (sync_range_covers(r, pg->vertex_attr_gen, min_element, max_element)) {
            r->num_vertex_ram_buffer_syncs = 0;
        } else {
            sync_vertex_ram_buffer(pg);
            sync_range_update(r, pg->vertex_attr_gen, min_element, max_element);
        }
#else
        sync_vertex_ram_buffer(pg);
#endif
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);
        NV2A_PHASE_TIMER_END(draw_vtx_sync);

        NV2A_PHASE_TIMER_BEGIN(draw_prim_rw);
        PrimRewrite prim_rw = pgraph_prim_rewrite_ranges(
            &r->prim_rewrite_buf, assembly,
            pg->draw_arrays_start, pg->draw_arrays_count,
            pg->draw_arrays_length);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size =
                prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
#if OPT_MULTI_DRAW
        } else if (pg->draw_arrays_length > 1) {
            size_t indirect_size =
                pg->draw_arrays_length * sizeof(VkDrawIndirectCommand);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, indirect_size);
#endif
        }
        NV2A_PHASE_TIMER_END(draw_prim_rw);

        begin_pre_draw(pg);
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        NV2A_PHASE_TIMER_END(draw_setup);

        NV2A_PHASE_TIMER_BEGIN(draw_vk_cmd);
        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 buffer_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
#if OPT_MULTI_DRAW
        } else if (pg->draw_arrays_length > 1) {
            OPT_STAT_INC(multi_draw_indirect);
            VkDrawIndirectCommand cmds[pg->draw_arrays_length];
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                cmds[i] = (VkDrawIndirectCommand){
                    .vertexCount = pg->draw_arrays_count[i],
                    .instanceCount = 1,
                    .firstVertex = pg->draw_arrays_start[i],
                    .firstInstance = 0,
                };
            }
            size_t indirect_size = pg->draw_arrays_length * sizeof(VkDrawIndirectCommand);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, indirect_size);
            VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
                pg, cmds, indirect_size);
            vkCmdDrawIndirect(r->command_buffer,
                              r->storage_buffers[BUFFER_INDEX].buffer,
                              buffer_offset, pg->draw_arrays_length,
                              sizeof(VkDrawIndirectCommand));
#endif
        } else {
            OPT_STAT_INC(multi_draw_loop);
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                uint32_t start = pg->draw_arrays_start[i],
                         count = pg->draw_arrays_count[i];
                NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
                vkCmdDraw(r->command_buffer, count, 1, start, 0);
            }
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_PHASE_TIMER_END(draw_vk_cmd);

        nv2a_diag_log_draw_call(d, pg, "draw_arrays", max_element);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        NV2A_PHASE_TIMER_BEGIN(draw_prim_rw);
        uint32_t *draw_indices = pg->inline_elements;
        unsigned int draw_index_count = pg->inline_elements_length;
        PrimRewrite prim_rw = pgraph_prim_rewrite_indexed(
            &r->prim_rewrite_buf, assembly, pg->inline_elements,
            pg->inline_elements_length);
        if (prim_rw.num_indices > 0) {
            draw_indices = prim_rw.indices;
            draw_index_count = prim_rw.num_indices;
        }

        size_t index_data_size = draw_index_count * sizeof(uint32_t);
        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);
        NV2A_PHASE_TIMER_END(draw_prim_rw);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_attr);
        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (unsigned int i = 0; i < draw_index_count; i++) {
            max_element = MAX(draw_indices[i], max_element);
            min_element = MIN(draw_indices[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            draw_indices[draw_index_count - 1]);
        NV2A_PHASE_TIMER_END(draw_vtx_attr);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_sync);
#if OPT_SYNC_RANGE_SKIP
        if (sync_range_covers(r, pg->vertex_attr_gen, min_element, max_element)) {
            r->num_vertex_ram_buffer_syncs = 0;
        } else {
            sync_vertex_ram_buffer(pg);
            sync_range_update(r, pg->vertex_attr_gen, min_element, max_element);
        }
#else
        sync_vertex_ram_buffer(pg);
#endif
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);
        NV2A_PHASE_TIMER_END(draw_vtx_sync);

        begin_pre_draw(pg);
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, draw_indices, index_data_size);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        NV2A_PHASE_TIMER_END(draw_setup);

        NV2A_PHASE_TIMER_BEGIN(draw_vk_cmd);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r->command_buffer, draw_index_count, 1, 0, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_PHASE_TIMER_END(draw_vk_cmd);

        nv2a_diag_log_draw_call(d, pg, "inline_elements", draw_index_count);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_attr);
        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        NV2A_PHASE_TIMER_END(draw_vtx_attr);

        NV2A_PHASE_TIMER_BEGIN(draw_prim_rw);
        PrimRewrite prim_rw = pgraph_prim_rewrite_sequential(
            &r->prim_rewrite_buf, assembly, 0, pg->inline_buffer_length);

        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);
        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        }
        NV2A_PHASE_TIMER_END(draw_prim_rw);

        begin_pre_draw(pg);
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        NV2A_PHASE_TIMER_END(draw_setup);

        NV2A_PHASE_TIMER_BEGIN(draw_vk_cmd);
        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize idx_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 idx_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
        } else {
            vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_PHASE_TIMER_END(draw_vk_cmd);

        nv2a_diag_log_draw_call(d, pg, "inline_buffer",
                                pg->inline_buffer_length);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        NV2A_PHASE_TIMER_BEGIN(draw_vtx_attr);
        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);
        NV2A_PHASE_TIMER_END(draw_vtx_attr);

        NV2A_PHASE_TIMER_BEGIN(draw_prim_rw);
        PrimRewrite prim_rw = pgraph_prim_rewrite_sequential(
            &r->prim_rewrite_buf, assembly, 0, index_count);

        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, rewrite_size);
        }
        NV2A_PHASE_TIMER_END(draw_prim_rw);

        begin_pre_draw(pg);
        NV2A_PHASE_TIMER_BEGIN(draw_setup);
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        NV2A_PHASE_TIMER_END(draw_setup);

        NV2A_PHASE_TIMER_BEGIN(draw_vk_cmd);
        if (prim_rw.num_indices > 0) {
            size_t rewrite_size = prim_rw.num_indices * sizeof(uint32_t);
            VkDeviceSize idx_offset = pgraph_vk_update_index_buffer(
                pg, prim_rw.indices, rewrite_size);
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 idx_offset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r->command_buffer, prim_rw.num_indices, 1, 0, 0,
                             0);
        } else {
            vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        }

        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_PHASE_TIMER_END(draw_vk_cmd);

        nv2a_diag_log_draw_call(d, pg, "inline_array", index_count);

        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
    NV2A_PHASE_TIMER_END(draw_dispatch);
}
