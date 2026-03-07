/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/compiler.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

const int num_invalid_surfaces_to_keep = 10;  // FIXME: Make automatic
const int max_surface_frame_time_delta = 5;

static void destroy_surface_image(PGRAPHVkState *r, SurfaceBinding *surface);

void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    // FIXME: It's just flush
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor; // FIXME: Move internal to renderer
}

void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    int new_factor = MAX(factor, 1);
    if (pg->surface_scale_factor != new_factor) {
        pg->shader_state_gen++;
    }
    pg->surface_scale_factor = new_factor;
}

// FIXME: Move to common
static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static void memcpy_image(void *dst, void const *src, int dst_stride,
                         int src_stride, int height)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, dst_stride * height);
        return;
    }

    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t const *src_ptr = (uint8_t *)src;

    size_t copy_stride = MIN(src_stride, dst_stride);

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, copy_stride);
        dst_ptr += dst_stride;
        src_ptr += src_stride;
    }
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

bool pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                   hwaddr start, hwaddr size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;
    bool found_overlap = false;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (check_surface_overlaps_range(surface, start, size)) {
            found_overlap = true;
            pgraph_vk_surface_download_if_dirty(
                container_of(pg, NV2AState, pgraph), surface);
        }
    }

    return found_overlap;
}


static bool download_surface_record_deferred(NV2AState *d,
                                             SurfaceBinding *surface,
                                             uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!surface->width || !surface->height) {
        return true;
    }

    bool is_ds =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool downscale = (pg->surface_scale_factor != 1);

    if (r->num_deferred_downloads >= MAX_DEFERRED_DOWNLOADS) {
        return false;
    }

    size_t staging_size = is_ds
        ? (4 * surface->width * surface->height)
        : (surface->host_fmt.host_bytes_per_pixel *
           surface->width * surface->height);

    VkDeviceSize aligned_offset = ROUND_UP(r->staging_dst_offset, 16);
    if (aligned_offset + staging_size >
        r->storage_buffers[BUFFER_STAGING_DST].buffer_size) {
        return false;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED,
                                 "download_surface_deferred");

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBuffer staging_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

    if (is_ds) {
        VkBuffer compute_dst = r->storage_buffers[BUFFER_COMPUTE_DST].buffer;
        VkBuffer compute_src = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
        size_t compute_dst_size =
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size;

        int num_copy_regions = 1;
        VkBufferImageCopy copy_regions[2];
        copy_regions[0] = (VkBufferImageCopy){
            .bufferOffset = 0,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };

        if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
            size_t depth_size = scaled_width * scaled_height * 4;
            copy_regions[num_copy_regions++] = (VkBufferImageCopy){
                .bufferOffset = ROUND_UP(depth_size,
                    r->device_props.limits.minStorageBufferOffsetAlignment),
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                .imageSubresource.layerCount = 1,
                .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
            };
        }

        VkBufferMemoryBarrier pre_img_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = compute_dst,
            .size = compute_dst_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_img_barrier, 0, NULL);

        vkCmdCopyImageToBuffer(cmd, surface->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               compute_dst, num_copy_regions, copy_regions);

        VkBufferMemoryBarrier post_img_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = compute_dst,
            .size = compute_dst_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &post_img_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = compute_src,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(pg, surface, cmd,
                                     compute_dst, compute_src, downscale);

        VkBufferMemoryBarrier post_pack_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = compute_src,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_staging_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging_buffer,
            .offset = aligned_offset,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_staging_barrier, 0, NULL);

        VkBufferCopy buf_copy = {
            .srcOffset = 0,
            .dstOffset = aligned_offset,
            .size = staging_size,
        };
        vkCmdCopyBuffer(cmd, compute_src, staging_buffer, 1, &buf_copy);

        VkBufferMemoryBarrier post_staging_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging_buffer,
            .offset = aligned_offset,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                             &post_staging_barrier, 0, NULL);
    } else {
        int num_copy_regions = 1;
        VkBufferImageCopy copy_regions[2];
        copy_regions[0] = (VkBufferImageCopy){
            .bufferOffset = aligned_offset,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.layerCount = 1,
        };

        VkImage surface_image_loc;
        if (downscale) {
            copy_regions[0].imageExtent =
                (VkExtent3D){ surface->width, surface->height, 1 };

            if (surface->image_scratch_current_layout !=
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                pgraph_vk_transition_image_layout(
                    pg, cmd, surface->image_scratch,
                    surface->host_fmt.vk_format,
                    surface->image_scratch_current_layout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                surface->image_scratch_current_layout =
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            }

            VkImageBlit blit_region = {
                .srcSubresource.aspectMask = surface->host_fmt.aspect,
                .srcSubresource.mipLevel = 0,
                .srcSubresource.baseArrayLayer = 0,
                .srcSubresource.layerCount = 1,
                .srcOffsets[0] = (VkOffset3D){0, 0, 0},
                .srcOffsets[1] =
                    (VkOffset3D){scaled_width, scaled_height, 1},
                .dstSubresource.aspectMask = surface->host_fmt.aspect,
                .dstSubresource.mipLevel = 0,
                .dstSubresource.baseArrayLayer = 0,
                .dstSubresource.layerCount = 1,
                .dstOffsets[0] = (VkOffset3D){0, 0, 0},
                .dstOffsets[1] =
                    (VkOffset3D){surface->width, surface->height, 1},
            };

            vkCmdBlitImage(
                cmd, surface->image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                surface->image_scratch,
                surface->image_scratch_current_layout, 1, &blit_region,
                VK_FILTER_LINEAR);

            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch,
                surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            surface_image_loc = surface->image_scratch;
        } else {
            copy_regions[0].imageExtent =
                (VkExtent3D){ scaled_width, scaled_height, 1 };
            surface_image_loc = surface->image;
        }

        VkBufferMemoryBarrier pre_copy_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging_buffer,
            .offset = aligned_offset,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_barrier, 0, NULL);

        vkCmdCopyImageToBuffer(cmd, surface_image_loc,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging_buffer,
                               num_copy_regions, copy_regions);

        VkBufferMemoryBarrier post_copy_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging_buffer,
            .offset = aligned_offset,
            .size = staging_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                             &post_copy_barrier, 0, NULL);
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    DeferredSurfaceDownload *dl =
        &r->deferred_downloads[r->num_deferred_downloads++];
    dl->staging_offset = aligned_offset;
    dl->download_size = staging_size;
    dl->dest_ptr = pixels;
    dl->width = surface->width;
    dl->height = surface->height;
    dl->pitch = surface->pitch;
    dl->bytes_per_pixel = surface->fmt.bytes_per_pixel;
    dl->swizzle = surface->swizzle;
    dl->color = surface->color;
    dl->host_fmt = surface->host_fmt;
    dl->fmt = surface->fmt;
    dl->use_compute_to_swizzle = false;

    r->staging_dst_offset = aligned_offset + staging_size;
    return true;
}

static void download_surface_complete_deferred(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_deferred_downloads == 0) {
        return;
    }

    int64_t _t0 = nv2a_clock_ns();
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    int64_t _t1 = nv2a_clock_ns();

    StorageBuffer *staging = &r->storage_buffers[BUFFER_STAGING_DST];

    for (int i = 0; i < r->num_deferred_downloads; i++) {
        DeferredSurfaceDownload *dl = &r->deferred_downloads[i];

        vmaInvalidateAllocation(r->allocator, staging->allocation,
                                dl->staging_offset, dl->download_size);

        void *src = staging->mapped + dl->staging_offset;

        if (dl->swizzle) {
            g_autofree uint8_t *swizzle_buf =
                (uint8_t *)g_malloc(dl->pitch * dl->height);
            memcpy_image(swizzle_buf, src, dl->pitch,
                         dl->width * dl->bytes_per_pixel, dl->height);
            swizzle_rect(swizzle_buf, dl->width, dl->height, dl->dest_ptr,
                         dl->pitch, dl->bytes_per_pixel);
            nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        } else {
            memcpy_image(dl->dest_ptr, src, dl->pitch,
                         dl->width * dl->bytes_per_pixel, dl->height);
        }
    }

    g_nv2a_stats.surf_working.df_flush_ns += _t1 - _t0;
    g_nv2a_stats.surf_working.df_read_ns += nv2a_clock_ns() - _t1;

    r->num_deferred_downloads = 0;
    r->staging_dst_offset = 0;
}

static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    download_surface_complete_deferred(d);

    VK_LOG("download_surface: %s addr=0x%x %ux%u pitch=%d bpp=%d swizzle=%d",
           surface->color ? "COLOR" : "ZETA", surface->vram_addr,
           surface->width, surface->height, surface->pitch,
           surface->fmt.bytes_per_pixel, surface->swizzle);

    if (!surface->width || !surface->height) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool use_compute_to_swizzle = OPT_COMPUTE_SWIZZLE &&
                                   surface->swizzle &&
                                   surface->fmt.bytes_per_pixel == 4 &&
                                   !use_compute_to_convert_depth_stencil_format;

    bool no_conversion_necessary =
        surface->color || use_compute_to_convert_depth_stencil_format ||
        surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM;

    assert(no_conversion_necessary);

    bool compute_needs_finish =
        ((use_compute_to_convert_depth_stencil_format || use_compute_to_swizzle)
         && pgraph_vk_compute_needs_finish(r));

#if OPT_SURF_TO_TEX_INLINE
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
#if OPT_ALWAYS_DEFERRED_FENCES
        pgraph_vk_flush_all_frames(pg);
        r->compute.descriptor_set_index = 0;
#endif
    }
#else
    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    } else if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
#if OPT_ALWAYS_DEFERRED_FENCES
        pgraph_vk_flush_all_frames(pg);
        r->compute.descriptor_set_index = 0;
#endif
    }
#endif

    bool downscale = (pg->surface_scale_factor != 1);

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    // Read surface into memory
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (surface->swizzle && !use_compute_to_swizzle) {
        assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

#if OPT_SURF_TO_TEX_INLINE
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int num_copy_regions = 1;
    VkBufferImageCopy copy_regions[2];
    copy_regions[0] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
    };

    VkImage surface_image_loc;
    if (downscale && !use_compute_to_convert_depth_stencil_format) {
        copy_regions[0].imageExtent =
            (VkExtent3D){ surface->width, surface->height, 1 };

        if (surface->image_scratch_current_layout !=
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        VkImageBlit blit_region = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},
        };

        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       surface->image_scratch,
                       surface->image_scratch_current_layout, 1, &blit_region,
                       surface->color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        surface_image_loc = surface->image_scratch;
    } else {
        copy_regions[0].imageExtent =
            (VkExtent3D){ scaled_width, scaled_height, 1 };
        surface_image_loc = surface->image;
    }

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        size_t depth_size = scaled_width * scaled_height * 4;
        copy_regions[num_copy_regions++] = (VkBufferImageCopy){
            .bufferOffset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };
    }

    //
    // Copy image to staging buffer, or to compute_dst if we need to pack it
    //

    size_t downloaded_image_size = surface->host_fmt.host_bytes_per_pixel *
                                   surface->width * surface->height;
    assert((downloaded_image_size) <=
           r->storage_buffers[BUFFER_STAGING_DST].buffer_size);

    bool use_compute_buffer = use_compute_to_convert_depth_stencil_format ||
                              use_compute_to_swizzle;
    int copy_buffer_idx = use_compute_buffer ? BUFFER_COMPUTE_DST :
                                               BUFFER_STAGING_DST;
    VkBuffer copy_buffer = r->storage_buffers[copy_buffer_idx].buffer;

    size_t copy_buffer_size = r->storage_buffers[copy_buffer_idx].buffer_size;
    {
        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = copy_buffer_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);
    }
    vkCmdCopyImageToBuffer(cmd, surface_image_loc,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                           num_copy_regions, copy_regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // FIXME: Verify output of depth stencil conversion
    // FIXME: Track current layout and only transition when required

    if (use_compute_to_convert_depth_stencil_format) {
        size_t bytes_per_pixel = 4;
        size_t packed_size =
            downscale ? (surface->width * surface->height * bytes_per_pixel) :
                        (scaled_width * scaled_height * bytes_per_pixel);

        //
        // Pack the depth-stencil image into compute_src buffer
        //

        VkBufferMemoryBarrier pre_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = copy_buffer_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_src_barrier, 0, NULL);

        VkBuffer pack_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(pg, surface, cmd, copy_buffer, pack_buffer,
                                     downscale);

        VkBufferMemoryBarrier post_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = copy_buffer_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_dst_barrier, 0, NULL);

        //
        // Copy packed image over to staging buffer for host download
        //

        copy_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);

        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, pack_buffer, copy_buffer, 1, &buffer_copy_region);

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);
    }

    if (use_compute_to_swizzle) {
        size_t swizzle_size = surface->width * surface->height *
                              surface->fmt.bytes_per_pixel;
        VkBuffer swizzle_src = r->storage_buffers[BUFFER_COMPUTE_DST].buffer;
        VkBuffer swizzle_dst = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_swizzle_barriers[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = swizzle_src,
                .size = swizzle_size,
            },
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = swizzle_dst,
                .size = swizzle_size,
            },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             2, pre_swizzle_barriers, 0, NULL);

        pgraph_vk_compute_swizzle(pg, cmd, swizzle_src, swizzle_size,
                                   swizzle_dst, swizzle_size,
                                   surface->width, surface->height, false);

        VkBufferMemoryBarrier post_swizzle_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = swizzle_dst,
            .size = swizzle_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_swizzle_barrier, 0, NULL);

        copy_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

        VkBufferMemoryBarrier pre_copy_staging_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = swizzle_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_staging_barrier, 0, NULL);

        VkBufferCopy buffer_copy_region = { .size = swizzle_size };
        vkCmdCopyBuffer(cmd, swizzle_dst, copy_buffer, 1,
                        &buffer_copy_region);

        downloaded_image_size = swizzle_size;
    }

    //
    // Download image data to host
    //

    VkBufferMemoryBarrier post_copy_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer,
        .size = downloaded_image_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &post_copy_dst_barrier, 0, NULL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_TO_TEX_INLINE
    pgraph_vk_end_nondraw_commands(pg, cmd);
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif

    void *mapped_memory_ptr = r->storage_buffers[BUFFER_STAGING_DST].mapped;

    vmaInvalidateAllocation(r->allocator,
                            r->storage_buffers[BUFFER_STAGING_DST].allocation,
                            0, downloaded_image_size);

    if (use_compute_to_swizzle) {
        memcpy(pixels, mapped_memory_ptr, downloaded_image_size);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else {
        memcpy_image(gl_read_buf, mapped_memory_ptr, surface->pitch,
                     surface->width * surface->fmt.bytes_per_pixel,
                     surface->height);

        if (surface->swizzle) {
            swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                         surface->pitch, surface->fmt.bytes_per_pixel);
            nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
            g_free(swizzle_buf);
        }
    }
}

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }

    // FIXME: Respect write enable at last TOU?

    download_surface_to_buffer(d, surface, d->vram_ptr + surface->vram_addr);

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);

    surface->download_pending = false;
    surface->draw_dirty = false;
}

void pgraph_vk_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;
    bool require_download = qatomic_read(&surface->draw_dirty);

#ifdef __ANDROID__
    /*
     * Android Vulkan currently presents through the CPU/VGA fallback path.
     * Force framebuffer surface download so fallback upload sees fresh pixels.
     */
    if (!d->pgraph.vk_renderer_state->display.use_external_memory) {
        require_download = true;
    }
#endif

    if (require_download) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&d->pgraph.vk_renderer_state->downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&d->pgraph.vk_renderer_state->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.vk_renderer_state->downloads_complete);
    }
}

void pgraph_vk_process_pending_downloads(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        download_surface(d, surface, false);
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_vk_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        pgraph_vk_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
        }
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static void bind_surface(PGRAPHVkState *r, SurfaceBinding *surface)
{
    VK_LOG("bind_surface: %s addr=0x%x %ux%u fmt=%d",
           surface->color ? "COLOR" : "ZETA", surface->vram_addr,
           surface->width, surface->height, surface->host_fmt.vk_format);

    if (surface->color) {
        r->color_binding = surface;
        r->color_drawn_in_cb = false;
    } else {
        r->zeta_binding = surface;
        r->zeta_drawn_in_cb = false;
    }

    r->framebuffer_dirty = true;
    r->pipeline_state_dirty = true;
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
        }
    }
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        surface->invalidation_frame = r->current_frame;
    } else {
        surface->invalidation_frame = -1;
    }

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    g_hash_table_remove(r->surface_addr_map,
                        (gpointer)(uintptr_t)surface->vram_addr);
    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
    r->surface_list_gen++;
}

/*
 * Move a surface from the active list to the shelf.
 *
 * Shelving preserves the VkImage and its GPU-side allocation so that if
 * the same VRAM address is later re-bound with an identical format,
 * the image can be re-used without recreation or re-upload (see
 * get_shelved_surface).  The surface is unbound, its CPU access
 * callback is unregistered, and it is removed from the address map.
 */
static void shelve_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (surface == r->color_binding) {
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    g_hash_table_remove(r->surface_addr_map,
                        (gpointer)(uintptr_t)surface->vram_addr);
    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->shelved_surfaces, surface, entry);
    r->surface_list_gen++;
}

/*
 * Look up and reclaim a shelved surface whose format, dimensions, and
 * pitch exactly match the target.  Returns NULL on miss.  On hit the
 * surface is removed from the shelf and its VkImage can be reused
 * directly, avoiding the cost of image creation and VRAM upload.
 */
static SurfaceBinding *get_shelved_surface(PGRAPHVkState *r,
                                           hwaddr vram_addr,
                                           SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->shelved_surfaces, entry, next) {
        if (surface->vram_addr == vram_addr &&
            surface->host_fmt.vk_format == target->host_fmt.vk_format &&
            surface->color == target->color &&
            surface->width == target->width &&
            surface->height == target->height &&
            surface->pitch == target->pitch) {
            QTAILQ_REMOVE(&r->shelved_surfaces, surface, entry);
            return surface;
        }
    }
    return NULL;
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE (other_surface, &r->surfaces, entry, next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width,
                other_surface->height, other_surface->pitch);
            pgraph_vk_surface_download_if_dirty(d, other_surface);
            invalidate_surface(d, other_surface);
        }
    }

    QTAILQ_FOREACH_SAFE (other_surface, &r->shelved_surfaces, entry,
                          next_surface) {
        if (other_surface->vram_addr == surface->vram_addr) {
            continue;
        }
        if (check_surfaces_overlap(surface, other_surface)) {
            pgraph_vk_surface_download_if_dirty(d, other_surface);
            QTAILQ_REMOVE(&r->shelved_surfaces, other_surface, entry);
            destroy_surface_image(r, other_surface);
            g_free(other_surface);
        }
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    assert(pgraph_vk_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    g_hash_table_insert(r->surface_addr_map,
                        (gpointer)(uintptr_t)surface->vram_addr, surface);
    QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
    r->surface_list_gen++;
}

SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    return g_hash_table_lookup(r->surface_addr_map, (gpointer)(uintptr_t)addr);
}

SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

static void set_surface_label(PGRAPHState *pg, SurfaceBinding const *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d aa:%d",
        surface->vram_addr, surface->color ? "Color" : "Zeta",
        surface->color ? surface->shape.color_format :
                         surface->shape.zeta_format,
        surface->width, surface->height, pg->surface_shape.anti_aliasing);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)surface->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, surface->allocation, label);

    if (surface->image_scratch) {
        g_autofree gchar *label_scratch =
            g_strdup_printf("%s (scratch)", label);
        name_info.objectHandle = (uint64_t)surface->image_scratch;
        name_info.pObjectName = label_scratch;
        if (r->debug_utils_extension_enabled) {
            vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
        }
        vmaSetAllocationName(r->allocator, surface->allocation_scratch,
                             label_scratch);
    }
}

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    assert(!surface->image);
    assert(!surface->image_scratch);

    NV2A_VK_DPRINTF(
        "Creating new surface image width=%d height=%d @ %08" HWADDR_PRIx,
        width, height, surface->vram_addr);

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = surface->host_fmt.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | surface->host_fmt.usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image,
                            &surface->allocation, NULL));

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image_scratch,
                            &surface->allocation_scratch, NULL));
    surface->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = surface->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface->host_fmt.vk_format,
        .subresourceRange.aspectMask = surface->host_fmt.aspect,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &surface->image_view));

#if OPT_SURF_BATCH_CREATE
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_UNDEFINED,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_3);
    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_BATCH_CREATE
    pgraph_vk_end_nondraw_commands(pg, cmd);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif
    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_image(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->image = src->image;
    dst->image_view = src->image_view;
    dst->allocation = src->allocation;
    dst->image_scratch = src->image_scratch;
    dst->image_scratch_current_layout = src->image_scratch_current_layout;
    dst->allocation_scratch = src->allocation_scratch;

    src->image = VK_NULL_HANDLE;
    src->image_view = VK_NULL_HANDLE;
    src->allocation = VK_NULL_HANDLE;
    src->image_scratch = VK_NULL_HANDLE;
    src->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    src->allocation_scratch = VK_NULL_HANDLE;
}

static void destroy_surface_image(PGRAPHVkState *r, SurfaceBinding *surface)
{
    vkDestroyImageView(r->device, surface->image_view, NULL);
    surface->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image, surface->allocation);
    surface->image = VK_NULL_HANDLE;
    surface->allocation = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image_scratch,
                    surface->allocation_scratch);
    surface->image_scratch = VK_NULL_HANDLE;
    surface->allocation_scratch = VK_NULL_HANDLE;
}

static bool check_invalid_surface_is_compatibile(SurfaceBinding *surface,
                                                 SurfaceBinding *target)
{
    return surface->host_fmt.vk_format == target->host_fmt.vk_format &&
           surface->width == target->width &&
           surface->height == target->height &&
           surface->host_fmt.usage == target->host_fmt.usage;
}

static bool surface_in_flight(PGRAPHVkState *r, SurfaceBinding *surface)
{
    int f = surface->invalidation_frame;
    return f >= 0 && (f == r->current_frame || r->frame_submitted[f]);
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHVkState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        if (surface_in_flight(r, surface)) {
            continue;
        }
        if (check_invalid_surface_is_compatibile(surface, target)) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHVkState *r, int keep)
{
    int num_surfaces = 0;

    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        num_surfaces += 1;
        if (num_surfaces > keep) {
            if (surface_in_flight(r, surface)) {
                continue;
            }
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            destroy_surface_image(r, surface);
            g_free(surface);
        }
    }
}

static const int max_shelved_surfaces = 20;

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            pgraph_vk_surface_download_if_dirty(d, s);
            invalidate_surface(d, s);
        }
    }

    int shelved_count = 0;
    QTAILQ_FOREACH_SAFE(s, &r->shelved_surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta ||
            shelved_count >= max_shelved_surfaces) {
            pgraph_vk_surface_download_if_dirty(d, s);
            QTAILQ_REMOVE(&r->shelved_surfaces, s, entry);
            destroy_surface_image(r, s);
            g_free(s);
        } else {
            shelved_count++;
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.vk_format == s2->host_fmt.vk_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }

    VK_LOG("upload_surface: %s addr=0x%x %ux%u pitch=%d bpp=%d swizzle=%d",
           surface->color ? "COLOR" : "ZETA", surface->vram_addr,
           surface->width, surface->height, surface->pitch,
           surface->fmt.bytes_per_pixel, surface->swizzle);

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

#if OPT_SURF_BATCH_UPLOAD
    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
    }
#else
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
#endif

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        surface->initialized = true;
        return;
    }

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool use_compute_to_unswizzle = OPT_COMPUTE_SWIZZLE &&
                                     surface->swizzle &&
                                     surface->fmt.bytes_per_pixel == 4 &&
                                     !use_compute_to_convert_depth_stencil_format;

    bool no_conversion_necessary =
        surface->color || surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM ||
        use_compute_to_convert_depth_stencil_format;
    assert(no_conversion_necessary);

    g_autofree uint8_t *swizzle_buf = NULL;
    uint8_t *gl_read_buf = NULL;

    if (surface->swizzle && !use_compute_to_unswizzle) {
        swizzle_buf = (uint8_t*)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       swizzle_buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else {
        gl_read_buf = buf;
    }

    //
    // Upload image data from host to staging buffer
    //

    size_t uploaded_image_size = surface->height * surface->width *
                                 surface->fmt.bytes_per_pixel;

#if OPT_SURF_BATCH_UPLOAD
    VkDeviceSize staging_base = pgraph_vk_staging_alloc(pg, uploaded_image_size);
    if (staging_base == VK_WHOLE_SIZE) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
#if OPT_ALWAYS_DEFERRED_FENCES
        staging_base = pgraph_vk_staging_alloc(pg, uploaded_image_size);
        if (staging_base == VK_WHOLE_SIZE) {
            pgraph_vk_flush_all_frames(pg);
            pgraph_vk_staging_reset(pg);
            staging_base = pgraph_vk_staging_alloc(pg, uploaded_image_size);
            assert(staging_base != VK_WHOLE_SIZE);
        }
#else
        pgraph_vk_staging_reset(pg);
        staging_base = pgraph_vk_staging_alloc(pg, uploaded_image_size);
        assert(staging_base != VK_WHOLE_SIZE);
#endif
    }
    StorageBuffer *copy_buffer = get_staging_buffer(r, BUFFER_STAGING_SRC);
    void *mapped_memory_ptr = copy_buffer->mapped + staging_base;
#else
    StorageBuffer *copy_buffer = get_staging_buffer(r, BUFFER_STAGING_SRC);
    assert(uploaded_image_size <= copy_buffer->buffer_size);
    VkDeviceSize staging_base = 0;
    void *mapped_memory_ptr = copy_buffer->mapped;
#endif

    if (use_compute_to_unswizzle) {
        memcpy(mapped_memory_ptr, buf, uploaded_image_size);
    } else {
        memcpy_image(mapped_memory_ptr, gl_read_buf,
                     surface->width * surface->fmt.bytes_per_pixel,
                     surface->pitch, surface->height);
    }

    vmaFlushAllocation(r->allocator, copy_buffer->allocation, staging_base,
                       uploaded_image_size);

#if OPT_SURF_BATCH_UPLOAD
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
#else
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .offset = staging_base,
        .size = uploaded_image_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    // Set up image copy regions (which may be modified by compute unpack)

    VkBufferImageCopy regions[2];
    int num_regions = 0;

    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = staging_base,
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = staging_base,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
        };
    }


    unsigned int scaled_width = surface->width, scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    if (use_compute_to_convert_depth_stencil_format) {

        //
        // Copy packed image buffer to compute_dst for unpacking
        //

        size_t packed_size = uploaded_image_size;
        VkBufferCopy buffer_copy_region = {
            .srcOffset = staging_base,
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &buffer_copy_region);

        size_t num_pixels = scaled_width * scaled_height;
        size_t unpacked_depth_image_size = num_pixels * 4;
        size_t unpacked_stencil_image_size = num_pixels;
        size_t unpacked_size =
            unpacked_depth_image_size + unpacked_stencil_image_size;

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer->buffer,
            .offset = staging_base,
            .size = uploaded_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);

        //
        // Unpack depth-stencil image into compute_src
        //

        VkBufferMemoryBarrier pre_unpack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_unpack_src_barrier, 0, NULL);

        StorageBuffer *unpack_buffer = &r->storage_buffers[BUFFER_COMPUTE_SRC];

        VkBufferMemoryBarrier pre_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                             &pre_unpack_dst_barrier, 0, NULL);

        pgraph_vk_unpack_depth_stencil(
            pg, surface, cmd, r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            unpack_buffer->buffer);

        VkBufferMemoryBarrier post_unpack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_dst_barrier, 0, NULL);

        // Already scaled during compute. Adjust copy regions.
        regions[0].imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 };
        regions[1].imageExtent = regions[0].imageExtent;
        regions[1].bufferOffset =
            ROUND_UP(unpacked_depth_image_size,
                     r->device_props.limits.minStorageBufferOffsetAlignment);

        copy_buffer = unpack_buffer;
    }

    if (use_compute_to_unswizzle) {
        size_t swizzle_size = uploaded_image_size;

        VkBufferCopy buf_copy = { .srcOffset = staging_base, .size = swizzle_size };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &buf_copy);

        VkBuffer unsw_src = r->storage_buffers[BUFFER_COMPUTE_DST].buffer;
        VkBuffer unsw_dst = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_unsw_barriers[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = unsw_src,
                .size = swizzle_size,
            },
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = unsw_dst,
                .size = swizzle_size,
            },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             2, pre_unsw_barriers, 0, NULL);

        pgraph_vk_compute_swizzle(pg, cmd, unsw_src, swizzle_size,
                                   unsw_dst, swizzle_size,
                                   surface->width, surface->height, true);

        VkBufferMemoryBarrier post_unsw_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unsw_dst,
            .size = swizzle_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unsw_barrier, 0, NULL);

        copy_buffer = &r->storage_buffers[BUFFER_COMPUTE_SRC];
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    }

    //
    // Copy image data from buffer to staging image
    //

    if (surface->image_scratch_current_layout !=
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    vkCmdCopyBufferToImage(cmd, copy_buffer->buffer, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_regions,
                           regions);

    VkBufferMemoryBarrier post_copy_src_buffer_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = copy_buffer->buffer_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_src_buffer_barrier, 0, NULL);

    //
    // Copy staging image to final image
    //

    pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                      surface->host_fmt.vk_format,
                                      surface->image_scratch_current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    surface->image_scratch_current_layout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    bool upscale = pg->surface_scale_factor > 1 &&
                   !use_compute_to_convert_depth_stencil_format;

    if (upscale) {
        VkImageBlit blitRegion = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},
        };

        vkCmdBlitImage(cmd, surface->image_scratch,
                       surface->image_scratch_current_layout, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion,
                       surface->color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        // Note: We should be able to vkCmdCopyBufferToImage directly into
        // surface->image, but there is an apparent AMD Windows driver
        // synchronization bug we'll hit when doing this. For this reason,
        // always use a staging image.

        for (int i = 0; i < num_regions; i++) {
            VkImageAspectFlags aspect = regions[i].imageSubresource.aspectMask;
            VkImageCopy copy_region = {
                .srcSubresource.aspectMask = aspect,
                .srcSubresource.layerCount = 1,
                .dstSubresource.aspectMask = aspect,
                .dstSubresource.layerCount = 1,
                .extent = regions[i].imageExtent,
            };
            vkCmdCopyImage(cmd, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy_region);
        }
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_2);
    pgraph_vk_end_debug_marker(r, cmd);
#if OPT_SURF_BATCH_UPLOAD
    pgraph_vk_end_nondraw_commands(pg, cmd);
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif

    surface->initialized = true;
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
    #define DO_CMP(fld) \
        if (a->fld != b->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)a->fld, (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.vk_format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_vk_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt = kelvin_surface_color_format_vk_map[pg->surface_shape.color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(r->kelvin_surface_zeta_vk_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt = r->kelvin_surface_zeta_vk_map[pg->surface_shape.zeta_format];
        // FIXME: Support float 16,24b float format surface
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    // There's a bunch of bugs that could cause us to hit this function
    // at the wrong time and get a invalid dma object.
    // Check that it's sane.
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        // Since we determine surface dimensions based on the clipping
        // rectangle, make sure to include the surface offset as well.
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_nv2a_stats.surf_working.update_calls++;

    int64_t _st0 = nv2a_clock_ns();
    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    target.invalidation_frame = -1;
    populate_surface_binding_target(d, color, &target);
    g_nv2a_stats.surf_working.populate_ns += nv2a_clock_ns() - _st0;

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    int64_t _st1 = nv2a_clock_ns();
    bool mem_dirty = false;
    if (!tcg_enabled()) {
        ram_addr_t start = r->vram_ram_addr + target.vram_addr;
        unsigned long page = start >> TARGET_PAGE_BITS;
        unsigned long end_page =
            TARGET_PAGE_ALIGN(start + target.size) >> TARGET_PAGE_BITS;

        RCU_READ_LOCK_GUARD();
        DirtyMemoryBlocks *blocks =
            qatomic_rcu_read(&ram_list.dirty_memory[DIRTY_MEMORY_NV2A]);

        while (page < end_page) {
            unsigned long idx = page / DIRTY_MEMORY_BLOCK_SIZE;
            unsigned long ofs = page % DIRTY_MEMORY_BLOCK_SIZE;
            unsigned long num = MIN(end_page - page,
                                    DIRTY_MEMORY_BLOCK_SIZE - ofs);
            mem_dirty |= bitmap_test_and_clear_atomic(
                blocks->blocks[idx], ofs, num);
            page += num;
        }
    }
    g_nv2a_stats.surf_working.dirty_ns += nv2a_clock_ns() - _st1;

    SurfaceBinding *current_binding = color ? r->color_binding
                                            : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        int64_t _gt0 = nv2a_clock_ns();
        // FIXME: We don't need to be so aggressive flushing the command list
        // pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
        pgraph_vk_ensure_not_in_render_pass(pg);

        unbind_surface(d, color);
        g_nv2a_stats.surf_working.enrp_ns += nv2a_clock_ns() - _gt0;

        int64_t _gt1 = nv2a_clock_ns();
        SurfaceBinding *surface = pgraph_vk_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            // FIXME: Support same color/zeta surface target? In the mean time,
            // if the surface we just found is currently bound, just unbind it.
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                               trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                     surface->shape.clip_x, surface->shape.clip_width,
                     surface->shape.clip_y, surface->shape.clip_height,
                     surface->pitch);

            assert(!(target.swizzle && pg->clearing));

#if 0
            if (surface->swizzle != target.swizzle) {
                // Clears should only be done on linear surfaces. Avoid
                // synchronization by allowing (1) a surface marked swizzled to
                // be cleared under the assumption the entire surface is
                // destined to be cleared and (2) a fully cleared linear surface
                // to be marked swizzled. Strictly match size to avoid
                // pathological cases.
                is_compatible &= (pg->clearing || surface->cleared) &&
                    check_surface_compatibility(surface, &target, true);
                if (is_compatible) {
                    trace_nv2a_pgraph_surface_migrate_type(
                        target.swizzle ? "swizzled" : "linear");
                }
            }
#endif

            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
            }

            if (is_compatible) {
                // FIXME: Refactor
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height = surface->shape.clip_height;
                surface->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
                g_nv2a_stats.surf_working.lk_hit_ns += nv2a_clock_ns() - _gt1;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", surface->vram_addr);
                compare_surfaces(surface, &target);
                if (surface->draw_dirty) {
                    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);

                    /*
                     * When a draw-dirty surface is shelved without a
                     * full download, the VRAM at its address still
                     * contains stale data (typically zeros).  If a new
                     * surface (or texture) is later bound at the same
                     * address, the upload path reads from VRAM and the
                     * stale zeros are interpreted as e.g. depth=0.0,
                     * causing everything to fail the depth test (all-
                     * white rendering).
                     *
                     * To avoid this, fill the VRAM region with 0xFF
                     * which encodes as depth ≈ 1.0 (D24S8) or opaque
                     * white (color), ensuring that any subsequent
                     * texture or surface upload from this region gets
                     * safe initial values.  The dirty bits are set so
                     * the texture cache knows to re-hash and re-upload.
                     */
                    size_t region =
                        (size_t)surface->pitch * surface->height;
                    memset(d->vram_ptr + surface->vram_addr, 0xFF,
                           region);
                    memory_region_set_client_dirty(
                        d->vram, surface->vram_addr, region,
                        DIRTY_MEMORY_NV2A_TEX);
                    memory_region_set_client_dirty(
                        d->vram, surface->vram_addr, region,
                        DIRTY_MEMORY_VGA);
                }
                shelve_surface(d, surface);
                g_nv2a_stats.surf_working.lk_evict_ns += nv2a_clock_ns() - _gt1;
                g_nv2a_stats.surf_working.evict_count++;
            }
        } else {
            g_nv2a_stats.surf_working.lk_nosurf_ns += nv2a_clock_ns() - _gt1;
        }

        if (should_create) {
            int64_t _gt2 = nv2a_clock_ns();
            bool unshelved = false;
            surface = get_shelved_surface(r, target.vram_addr, &target);
            if (surface) {
                migrate_surface_image(&target, surface);
                unshelved = true;
            } else {
                surface = get_any_compatible_invalid_surface(r, &target);
                if (surface) {
                    migrate_surface_image(&target, surface);
                } else {
                    surface = g_malloc(sizeof(SurfaceBinding));
                    create_surface_image(pg, &target);
                }
            }
            g_nv2a_stats.surf_working.create_ns += nv2a_clock_ns() - _gt2;

            *surface = target;
            set_surface_label(pg, surface);

            if (unshelved) {
                /*
                 * The VkImage already contains valid data from the
                 * previous binding, so skip the VRAM upload and mark
                 * the surface as initialized.
                 */
                surface->upload_pending = false;
                surface->initialized = true;
            }

            int64_t _gt3 = nv2a_clock_ns();
            surface_put(d, surface);
            g_nv2a_stats.surf_working.put_ns += nv2a_clock_ns() - _gt3;

            // FIXME: Refactor
            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height, surface->pitch);

        int64_t _gt4 = nv2a_clock_ns();
        bind_surface(r, surface);
        g_nv2a_stats.surf_working.bind_ns += nv2a_clock_ns() - _gt4;

        pg_surface->buffer_dirty = false;
        if (should_create) {
            g_nv2a_stats.surf_working.create_count++;
        } else {
            g_nv2a_stats.surf_working.hit_count++;
        }
    } else {
        g_nv2a_stats.surf_working.miss_count++;
    }

    if (!upload && pg_surface->draw_dirty) {
        int64_t _st3 = nv2a_clock_ns();
        if (!tcg_enabled()) {
            // FIXME: Cannot monitor for reads/writes; flush now
            download_surface(d, color ? r->color_binding : r->zeta_binding,
                             true);
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
        g_nv2a_stats.surf_working.download_ns += nv2a_clock_ns() - _st3;
        g_nv2a_stats.surf_working.download_count++;
    }
}

// FIXME: Move to common?
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write)
{
    NV2A_PHASE_TIMER_BEGIN(surface_update);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_LOG("surface_update: upload=%d color_w=%d zeta_w=%d clearing=%d",
           upload, color_write, zeta_write, pg->clearing);

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    download_surface_complete_deferred(d);

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    {
        int64_t _su0 = nv2a_clock_ns();
        if (r->color_binding) {
            r->color_binding->frame_time = pg->frame_time;
            if (upload) {
                pgraph_vk_upload_surface_data(d, r->color_binding, false);
                r->color_binding->draw_time = pg->draw_time;
                r->color_binding->swizzle = swizzle;
                g_nv2a_stats.surf_working.upload_count++;
            }
        }

        if (r->zeta_binding) {
            r->zeta_binding->frame_time = pg->frame_time;
            if (upload) {
                pgraph_vk_upload_surface_data(d, r->zeta_binding, false);
                r->zeta_binding->draw_time = pg->draw_time;
                r->zeta_binding->swizzle = swizzle;
                g_nv2a_stats.surf_working.upload_count++;
            }
        }
        g_nv2a_stats.surf_working.upload_ns += nv2a_clock_ns() - _su0;
    }

    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    {
        int64_t _se0 = nv2a_clock_ns();
        expire_old_surfaces(d);
        prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
        g_nv2a_stats.surf_working.expire_ns += nv2a_clock_ns() - _se0;
    }

    NV2A_PHASE_TIMER_END(surface_update);
}

static bool check_format_and_usage_supported(PGRAPHVkState *r, VkFormat format,
                                             VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 pdif2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        r->physical_device, &pdif2, &props);
    return result == VK_SUCCESS;
}

static bool check_surface_internal_formats_supported(
    PGRAPHVkState *r, const SurfaceFormatInfo *fmts, size_t count)
{
    bool all_supported = true;
    for (int i = 0; i < count; i++) {
        const SurfaceFormatInfo *f = &fmts[i];
        if (f->host_bytes_per_pixel) {
            all_supported &=
                check_format_and_usage_supported(r, f->vk_format, f->usage);
        }
    }
    return all_supported;
}

void pgraph_vk_init_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Make sure all surface format types are supported. We don't expect issue
    // with these, and therefore have no fallback mechanism.
    bool color_formats_supported = check_surface_internal_formats_supported(
        r, kelvin_surface_color_format_vk_map,
        ARRAY_SIZE(kelvin_surface_color_format_vk_map));
    if (!color_formats_supported) {
        fprintf(stderr,
                "Warning: Some Vulkan surface color formats are unsupported; "
                "continuing with best-effort mapping.\n");
    }

    // Check if the device supports preferred VK_FORMAT_D24_UNORM_S8_UINT
    // format, fall back to D32_SFLOAT_S8_UINT otherwise.
    r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z16] = zeta_d16;
    if (check_surface_internal_formats_supported(r, &zeta_d24_unorm_s8_uint,
                                                 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d24_unorm_s8_uint;
    } else if (check_surface_internal_formats_supported(
                   r, &zeta_d32_sfloat_s8_uint, 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d32_sfloat_s8_uint;
    } else {
        fprintf(stderr,
                "Warning: No suitable Vulkan Z24S8 depth-stencil format; "
                "falling back to Z16 mapping.\n");
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d16;
    }

    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->invalid_surfaces);
    QTAILQ_INIT(&r->shelved_surfaces);
    r->surface_addr_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    r->surface_list_gen = 0;

    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;

    pgraph_vk_reload_surface_scale_factor(pg); // FIXME: Move internal
}

void pgraph_vk_finalize_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_surface_flush(container_of(pg, NV2AState, pgraph));
    if (r->surface_addr_map) {
        g_hash_table_destroy(r->surface_addr_map);
        r->surface_addr_map = NULL;
    }
}

void pgraph_vk_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Clear last surface shape to force recreation of buffers at next draw
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        pgraph_vk_surface_download_if_dirty(d, s);
        invalidate_surface(d, s);
    }

    QTAILQ_FOREACH_SAFE(s, &r->shelved_surfaces, entry, next) {
        pgraph_vk_surface_download_if_dirty(d, s);
        QTAILQ_REMOVE(&r->shelved_surfaces, s, entry);
        destroy_surface_image(r, s);
        g_free(s);
    }

    prune_invalid_surfaces(r, 0);

    pgraph_vk_reload_surface_scale_factor(pg);
}
