/*
 * Geforce NV2A PGRAPH Vulkan Renderer - Render Command Thread
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
#include "renderer.h"

void pgraph_vk_snapshot_state(PGRAPHState *pg, RenderCommandSnapshot *snap)
{
    memcpy(snap->regs, pg->regs_, sizeof(snap->regs));

    snap->shader_state_gen = pg->shader_state_gen;
    snap->pipeline_state_gen = pg->pipeline_state_gen;
    snap->texture_state_gen = pg->texture_state_gen;
    snap->vertex_attr_gen = pg->vertex_attr_gen;
    snap->non_dynamic_reg_gen = pg->non_dynamic_reg_gen;
    snap->any_reg_gen = pg->any_reg_gen;

    snap->program_data_dirty = pg->program_data_dirty;
    memcpy(snap->program_data, pg->program_data, sizeof(snap->program_data));

    memcpy(snap->vertex_attributes, pg->vertex_attributes,
           sizeof(snap->vertex_attributes));
    snap->compressed_attrs = pg->compressed_attrs;
    snap->uniform_attrs = pg->uniform_attrs;
    snap->swizzle_attrs = pg->swizzle_attrs;

    snap->surface_shape = pg->surface_shape;
    snap->surface_binding_dim.clip_x = pg->surface_binding_dim.clip_x;
    snap->surface_binding_dim.clip_width = pg->surface_binding_dim.clip_width;
    snap->surface_binding_dim.clip_y = pg->surface_binding_dim.clip_y;
    snap->surface_binding_dim.clip_height = pg->surface_binding_dim.clip_height;
    snap->surface_binding_dim.width = pg->surface_binding_dim.width;
    snap->surface_binding_dim.height = pg->surface_binding_dim.height;
    snap->surface_scale_factor = pg->surface_scale_factor;

    snap->primitive_mode = pg->primitive_mode;
    snap->clearing = pg->clearing;
    memcpy(snap->texture_matrix_enable, pg->texture_matrix_enable,
           sizeof(snap->texture_matrix_enable));
    memcpy(snap->texture_dirty, pg->texture_dirty,
           sizeof(snap->texture_dirty));

    snap->dma_a = pg->dma_a;
    snap->dma_b = pg->dma_b;
    snap->dma_vertex_a = pg->dma_vertex_a;
    snap->dma_vertex_b = pg->dma_vertex_b;
    snap->dma_color = pg->dma_color;
    snap->dma_zeta = pg->dma_zeta;

    snap->frame_time = pg->frame_time;
    snap->draw_time = pg->draw_time;

    memcpy(snap->vsh_constants, pg->vsh_constants,
           sizeof(snap->vsh_constants));
    memcpy(snap->vsh_constants_dirty, pg->vsh_constants_dirty,
           sizeof(snap->vsh_constants_dirty));
    snap->vsh_constants_any_dirty = pg->vsh_constants_any_dirty;

    memcpy(snap->ltctxa, pg->ltctxa, sizeof(snap->ltctxa));
    memcpy(snap->ltctxa_dirty, pg->ltctxa_dirty, sizeof(snap->ltctxa_dirty));
    snap->ltctxa_any_dirty = pg->ltctxa_any_dirty;
    memcpy(snap->ltctxb, pg->ltctxb, sizeof(snap->ltctxb));
    memcpy(snap->ltctxb_dirty, pg->ltctxb_dirty, sizeof(snap->ltctxb_dirty));
    snap->ltctxb_any_dirty = pg->ltctxb_any_dirty;
    memcpy(snap->ltc1, pg->ltc1, sizeof(snap->ltc1));
    memcpy(snap->ltc1_dirty, pg->ltc1_dirty, sizeof(snap->ltc1_dirty));
    snap->ltc1_any_dirty = pg->ltc1_any_dirty;

    snap->material_alpha = pg->material_alpha;
    snap->specular_power = pg->specular_power;
    snap->specular_power_back = pg->specular_power_back;
    memcpy(snap->specular_params, pg->specular_params,
           sizeof(snap->specular_params));
    memcpy(snap->specular_params_back, pg->specular_params_back,
           sizeof(snap->specular_params_back));
    memcpy(snap->point_params, pg->point_params, sizeof(snap->point_params));

    memcpy(snap->light_infinite_half_vector, pg->light_infinite_half_vector,
           sizeof(snap->light_infinite_half_vector));
    memcpy(snap->light_infinite_direction, pg->light_infinite_direction,
           sizeof(snap->light_infinite_direction));
    memcpy(snap->light_local_position, pg->light_local_position,
           sizeof(snap->light_local_position));
    memcpy(snap->light_local_attenuation, pg->light_local_attenuation,
           sizeof(snap->light_local_attenuation));

    snap->inline_array_length = pg->inline_array_length;
    snap->inline_elements_length = pg->inline_elements_length;
    snap->inline_buffer_length = pg->inline_buffer_length;
    snap->draw_arrays_length = pg->draw_arrays_length;
    snap->draw_arrays_min_start = pg->draw_arrays_min_start;
    snap->draw_arrays_max_count = pg->draw_arrays_max_count;

    snap->zpass_pixel_count_enable = pg->zpass_pixel_count_enable;
}

static void process_finish(PGRAPHVkState *r, RenderCommand *cmd)
{
    VkSubmitInfo submit_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd->finish.aux_command_buffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &cmd->finish.semaphore,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd->finish.command_buffer,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &cmd->finish.semaphore,
            .pWaitDstStageMask = &cmd->finish.wait_stage,
        }
    };

    vkResetFences(r->device, 1, &cmd->finish.fence);
    VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos),
                           submit_infos, cmd->finish.fence));
    qatomic_set(&r->frame_submitted[cmd->finish.frame_index], true);
    qatomic_inc(&r->submit_count);

    if (!cmd->finish.deferred) {
        VK_CHECK(vkWaitForFences(r->device, 1, &cmd->finish.fence,
                                 VK_TRUE, UINT64_MAX));
        if (cmd->finish.post_fence_cb) {
            cmd->finish.post_fence_cb(r, NULL, cmd->finish.post_fence_opaque);
        }
    } else if (cmd->finish.post_fence_cb) {
        VK_CHECK(vkWaitForFences(r->device, 1, &cmd->finish.fence,
                                 VK_TRUE, UINT64_MAX));
        cmd->finish.post_fence_cb(r, NULL, cmd->finish.post_fence_opaque);
    }

    if (cmd->finish.completion) {
        qemu_event_set(cmd->finish.completion);
    }
}

static void process_vertex_ram_update(PGRAPHVkState *r, RenderCommand *cmd)
{
    hwaddr offset = cmd->vertex_ram.offset;
    VkDeviceSize size = cmd->vertex_ram.size;
    void *data = cmd->vertex_ram.data;

    StorageBuffer *vram = get_staging_buffer(r, BUFFER_VERTEX_RAM);
    memcpy(vram->mapped + offset, data, size);

#if OPT_ALWAYS_DEFERRED_FENCES
    FrameStagingState *fs = &r->frame_staging[r->current_frame];
    if (offset < fs->vertex_ram_flush_min) {
        fs->vertex_ram_flush_min = offset;
    }
    VkDeviceSize end = offset + size;
    if (end > fs->vertex_ram_flush_max) {
        fs->vertex_ram_flush_max = end;
    }
#else
    if (offset < r->vertex_ram_flush_min) {
        r->vertex_ram_flush_min = offset;
    }
    VkDeviceSize end = offset + size;
    if (end > r->vertex_ram_flush_max) {
        r->vertex_ram_flush_max = end;
    }
#endif

    size_t start_bit = offset / TARGET_PAGE_SIZE;
    size_t end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
    bitmap_set(get_uploaded_bitmap(r), start_bit, end_bit - start_bit);

    g_free(data);
}

static void *render_thread_func(void *opaque)
{
    PGRAPHVkState *r = opaque;
    RenderThread *rt = &r->render_thread;

    while (true) {
        qemu_mutex_lock(&rt->lock);

        while (QSIMPLEQ_EMPTY(&rt->queue) && !rt->shutdown) {
            qemu_event_set(&rt->idle_event);
            qemu_cond_wait(&rt->cond, &rt->lock);
        }

        if (rt->shutdown && QSIMPLEQ_EMPTY(&rt->queue)) {
            qemu_mutex_unlock(&rt->lock);
            break;
        }

        RenderCommand *cmd = QSIMPLEQ_FIRST(&rt->queue);
        QSIMPLEQ_REMOVE_HEAD(&rt->queue, entry);
        rt->queue_depth--;

        qemu_mutex_unlock(&rt->lock);

        switch (cmd->type) {
        case RCMD_FINISH:
            process_finish(r, cmd);
            break;

        case RCMD_VERTEX_RAM_UPDATE:
            process_vertex_ram_update(r, cmd);
            break;

        case RCMD_PROCESS_DOWNLOADS:
            r->is_render_thread_context = true;
            if (cmd->download.dirty_surfaces) {
                pgraph_vk_download_dirty_surfaces(r->nv2a);
            } else {
                pgraph_vk_process_pending_downloads(r->nv2a);
            }
            r->is_render_thread_context = false;
            if (cmd->download.completion) {
                qemu_event_set(cmd->download.completion);
            }
            break;

        case RCMD_SYNC_DISPLAY: {
            NV2AState *d = r->nv2a;
            PGRAPHState *pg = &d->pgraph;
            r->is_render_thread_context = true;
#if HAVE_EXTERNAL_MEMORY
            if (r->display.use_external_memory) {
                pgraph_vk_render_display(pg);
            }
#else
            pgraph_vk_render_display(pg);
#endif
            r->is_render_thread_context = false;
            qatomic_set(&pg->sync_pending, false);
            qemu_event_set(&pg->sync_complete);
            if (cmd->sync.completion) {
                qemu_event_set(cmd->sync.completion);
            }
            break;
        }

        case RCMD_FLUSH: {
            NV2AState *d = r->nv2a;
            PGRAPHState *pg = &d->pgraph;
            r->is_render_thread_context = true;
            pgraph_vk_flush_reorder_window(d);
            pgraph_vk_flush_draw_queue(d);
            pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
            pgraph_vk_surface_flush(d);
            pgraph_vk_mark_textures_possibly_dirty(
                d, 0, memory_region_size(d->vram));
            pgraph_vk_update_vertex_ram_buffer(
                pg, 0, d->vram_ptr, memory_region_size(d->vram));
            r->texture_vram_gen++;
            for (int i = 0; i < 4; i++) {
                pg->texture_dirty[i] = true;
            }
            r->is_render_thread_context = false;
            qatomic_set(&pg->flush_pending, false);
            qemu_event_set(&pg->flush_complete);
            if (cmd->flush_op.completion) {
                qemu_event_set(cmd->flush_op.completion);
            }
            break;
        }

        case RCMD_SHUTDOWN:
            g_free(cmd);
            goto out;

        default:
            break;
        }

        g_free(cmd);
        qemu_event_set(&rt->idle_event);
    }

out:
    return NULL;
}

void pgraph_vk_render_thread_enqueue(PGRAPHVkState *r, RenderCommand *cmd)
{
    RenderThread *rt = &r->render_thread;

    qemu_event_reset(&rt->idle_event);

    qemu_mutex_lock(&rt->lock);
    QSIMPLEQ_INSERT_TAIL(&rt->queue, cmd, entry);
    rt->queue_depth++;
    qemu_cond_signal(&rt->cond);
    qemu_mutex_unlock(&rt->lock);
}

void pgraph_vk_render_thread_wait_idle(PGRAPHVkState *r)
{
    if (r->is_render_thread_context) {
        return;
    }

    RenderThread *rt = &r->render_thread;

    while (true) {
        qemu_mutex_lock(&rt->lock);
        bool idle = QSIMPLEQ_EMPTY(&rt->queue);
        qemu_mutex_unlock(&rt->lock);

        if (idle) {
            return;
        }

        qemu_event_wait(&rt->idle_event);
        qemu_event_reset(&rt->idle_event);
    }
}

void pgraph_vk_render_thread_init(PGRAPHVkState *r)
{
    RenderThread *rt = &r->render_thread;
    qemu_mutex_init(&rt->lock);
    qemu_cond_init(&rt->cond);
    qemu_event_init(&rt->idle_event, false);
    QSIMPLEQ_INIT(&rt->queue);
    rt->shutdown = false;
    rt->queue_depth = 0;
    qemu_thread_create(&rt->thread, "pgraph.vk.render",
                       render_thread_func, r, QEMU_THREAD_JOINABLE);
}

void pgraph_vk_render_thread_shutdown(PGRAPHVkState *r)
{
    RenderThread *rt = &r->render_thread;

    pgraph_vk_render_thread_wait_idle(r);

    qemu_mutex_lock(&rt->lock);
    rt->shutdown = true;
    qemu_cond_signal(&rt->cond);
    qemu_mutex_unlock(&rt->lock);

    qemu_thread_join(&rt->thread);

    RenderCommand *cmd;
    while ((cmd = QSIMPLEQ_FIRST(&rt->queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&rt->queue, entry);
        g_free(cmd);
    }

    qemu_mutex_destroy(&rt->lock);
    qemu_cond_destroy(&rt->cond);
    qemu_event_destroy(&rt->idle_event);
}
