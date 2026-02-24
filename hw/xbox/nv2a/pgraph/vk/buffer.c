/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

static const char *const buffer_names[BUFFER_COUNT] = {
    "BUFFER_STAGING_DST",
    "BUFFER_STAGING_SRC",
    "BUFFER_COMPUTE_DST",
    "BUFFER_COMPUTE_SRC",
    "BUFFER_INDEX",
    "BUFFER_INDEX_STAGING",
    "BUFFER_VERTEX_RAM",
    "BUFFER_VERTEX_INLINE",
    "BUFFER_VERTEX_INLINE_STAGING",
    "BUFFER_UNIFORM",
    "BUFFER_UNIFORM_STAGING",
};

static bool create_buffer(PGRAPHState *pg, StorageBuffer *buffer,
                          const char *name, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vmaCreateBuffer(r->allocator, &buffer_create_info,
                                      &buffer->alloc_info, &buffer->buffer,
                                      &buffer->allocation, NULL);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create Vulkan buffer %s (%zu bytes): %d",
                   name, buffer->buffer_size, result);
        return false;
    }
    return true;
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (buffer->buffer == VK_NULL_HANDLE && buffer->allocation == VK_NULL_HANDLE) {
        return;
    }
    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

bool pgraph_vk_init_buffers(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    const size_t mib = 1024 * 1024;
    size_t vram_size = memory_region_size(d->vram);
    size_t staging_size = vram_size;
    if (staging_size < (16 * mib)) {
        staging_size = 16 * mib;
    }
    size_t compute_size = vram_size * 2;
    if (compute_size < (64 * mib)) {
        compute_size = 64 * mib;
    }
#ifdef __ANDROID__
    if (compute_size > (64 * mib)) {
        compute_size = 64 * mib;
    }
#else
    if (compute_size > (256 * mib)) {
        compute_size = 256 * mib;
    }
#endif

    VK_LOG("buffer_init: vram=%zu staging=%zu compute=%zu",
           vram_size, staging_size, compute_size);

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = 0,
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = staging_size,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = compute_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = sizeof(pg->inline_elements) * 100,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    if (!r->uploaded_bitmap) {
        error_setg(errp, "Failed to allocate uploaded surface bitmap");
        return false;
    }
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = NV2A_VERTEXSHADER_ATTRIBUTES * NV2A_MAX_BATCH_LENGTH *
                       4 * sizeof(float) * 10,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 8 * 1024 * 1024,
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        VK_LOG("buffer_init: create %s size=%zu",
               buffer_names[i], r->storage_buffers[i].buffer_size);
        if (!create_buffer(pg, &r->storage_buffers[i], buffer_names[i], errp)) {
            VK_LOG_ERROR("buffer_init: create %s FAILED", buffer_names[i]);
            goto fail;
        }
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING,
                             BUFFER_STAGING_SRC,
                             BUFFER_STAGING_DST };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        int idx = buffers_to_map[i];
        VK_LOG("buffer_init: map %s", buffer_names[idx]);
        VkResult result = vmaMapMemory(
            r->allocator, r->storage_buffers[idx].allocation,
            (void **)&r->storage_buffers[idx].mapped);
        if (result != VK_SUCCESS) {
            VK_LOG_ERROR("buffer_init: map %s FAILED: %d",
                         buffer_names[idx], result);
            error_setg(errp, "Failed to map Vulkan buffer %s (%zu bytes): %d",
                       buffer_names[idx], r->storage_buffers[idx].buffer_size,
                       result);
            goto fail;
        }
    }

    pgraph_prim_rewrite_init(&r->prim_rewrite_buf);
    return true;

fail:
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
            r->storage_buffers[i].mapped = NULL;
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }
    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
    r->bitmap_size = 0;
    return false;
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }

    pgraph_prim_rewrite_finalize(&r->prim_rewrite_buf);

    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    return (ROUND_UP(b->buffer_offset, alignment) + size) <= b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}
