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
#include "qemu/mstring.h"
#include "renderer.h"
#include "ui/xemu-settings.h"

#if OPT_ASYNC_COMPILE
extern bool xemu_get_async_compile(void);
#endif

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = r->descriptor_set_count;

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 2 * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = num_sets,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + NV2A_MAX_TEXTURES];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int count = r->descriptor_set_count;

    r->descriptor_sets = g_malloc(count * sizeof(VkDescriptorSet));

    VkDescriptorSetLayout *layouts = g_malloc(count * sizeof(VkDescriptorSetLayout));
    for (int i = 0; i < count; i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = count,
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));
    g_free(layouts);
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         r->descriptor_set_count, r->descriptor_sets);
    g_free(r->descriptor_sets);
    r->descriptor_sets = NULL;
}

#if OPT_BINDLESS_TEXTURES

static void create_bindless_descriptor_resources(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->bindless_textures_supported) return;

    /* Bindless texture array layout (set 0): 3 bindings for 2D, 3D, Cube */
    VkDescriptorSetLayoutBinding bl_bindings[3];
    VkDescriptorBindingFlags bl_flags[3];
    for (int i = 0; i < 3; i++) {
        bl_bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = MAX_BINDLESS_TEXTURES,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        bl_flags[i] =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 3,
        .pBindingFlags = bl_flags,
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 3,
        .pBindings = bl_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->bindless_set_layout));

    /* UBO-only layout (set 1) */
    VkDescriptorSetLayoutBinding ubo_bindings[2] = {
        {
            .binding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        {
            .binding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo ubo_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = ubo_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &ubo_layout_info, NULL,
                                         &r->ubo_set_layout));

    /* Bindless pool: 1 set with 3 * MAX_BINDLESS_TEXTURES samplers */
    VkDescriptorPoolSize bl_pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 3 * MAX_BINDLESS_TEXTURES,
    };
    VkDescriptorPoolCreateInfo bl_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &bl_pool_size,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &bl_pool_info, NULL,
                                    &r->bindless_descriptor_pool));

    /* Allocate the single bindless descriptor set */
    VkDescriptorSetAllocateInfo bl_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->bindless_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &r->bindless_set_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &bl_alloc,
                                      &r->bindless_descriptor_set));

    /* UBO pool: cycling sets like the current descriptor pool */
    r->ubo_descriptor_set_count = NUM_GFX_DESCRIPTOR_SETS;
    VkDescriptorPoolSize ubo_pool_size = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .descriptorCount = 2 * r->ubo_descriptor_set_count,
    };
    VkDescriptorPoolCreateInfo ubo_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = r->ubo_descriptor_set_count,
        .poolSizeCount = 1,
        .pPoolSizes = &ubo_pool_size,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &ubo_pool_info, NULL,
                                    &r->ubo_descriptor_pool));

    /* Allocate all UBO descriptor sets up front */
    r->ubo_descriptor_sets =
        g_malloc(r->ubo_descriptor_set_count * sizeof(VkDescriptorSet));
    VkDescriptorSetLayout *ubo_layouts =
        g_malloc(r->ubo_descriptor_set_count * sizeof(VkDescriptorSetLayout));
    for (int i = 0; i < r->ubo_descriptor_set_count; i++) {
        ubo_layouts[i] = r->ubo_set_layout;
    }
    VkDescriptorSetAllocateInfo ubo_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->ubo_descriptor_pool,
        .descriptorSetCount = r->ubo_descriptor_set_count,
        .pSetLayouts = ubo_layouts,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &ubo_alloc,
                                      r->ubo_descriptor_sets));
    g_free(ubo_layouts);

    r->ubo_descriptor_set_index = 0;
    memset(r->bindless_slot_bitmap, 0, sizeof(r->bindless_slot_bitmap));
    r->bindless_slot_bitmap[0] = 1;
}

static void destroy_bindless_descriptor_resources(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->bindless_textures_supported) return;

    vkFreeDescriptorSets(r->device, r->ubo_descriptor_pool,
                         r->ubo_descriptor_set_count, r->ubo_descriptor_sets);
    g_free(r->ubo_descriptor_sets);
    r->ubo_descriptor_sets = NULL;
    vkDestroyDescriptorPool(r->device, r->ubo_descriptor_pool, NULL);
    vkDestroyDescriptorPool(r->device, r->bindless_descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->ubo_set_layout, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->bindless_set_layout, NULL);
}

#endif /* OPT_BINDLESS_TEXTURES */

#define NUM_PUSH_UBO_SETS 256

static void create_push_descriptor_resources(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->push_descriptors_supported) return;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) return;
#endif

    VkDescriptorSetLayoutBinding tex_bindings[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        tex_bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo tex_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = NV2A_MAX_TEXTURES,
        .pBindings = tex_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &tex_layout_info, NULL,
                                         &r->push_tex_set_layout));

    VkDescriptorSetLayoutBinding ubo_bindings[2] = {
        {
            .binding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        {
            .binding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo ubo_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = ubo_bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &ubo_layout_info, NULL,
                                         &r->push_ubo_set_layout));

    r->push_ubo_set_count = NUM_PUSH_UBO_SETS;
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .descriptorCount = 2 * r->push_ubo_set_count,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = r->push_ubo_set_count,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->push_ubo_pool));

    r->push_ubo_sets = g_malloc(r->push_ubo_set_count * sizeof(VkDescriptorSet));
    VkDescriptorSetLayout *layouts =
        g_malloc(r->push_ubo_set_count * sizeof(VkDescriptorSetLayout));
    for (int i = 0; i < r->push_ubo_set_count; i++) {
        layouts[i] = r->push_ubo_set_layout;
    }
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->push_ubo_pool,
        .descriptorSetCount = r->push_ubo_set_count,
        .pSetLayouts = layouts,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info, r->push_ubo_sets));
    g_free(layouts);
    r->push_ubo_set_index = 0;
}

static void destroy_push_descriptor_resources(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->push_descriptors_supported) return;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) return;
#endif

    vkFreeDescriptorSets(r->device, r->push_ubo_pool,
                         r->push_ubo_set_count, r->push_ubo_sets);
    g_free(r->push_ubo_sets);
    r->push_ubo_sets = NULL;
    vkDestroyDescriptorPool(r->device, r->push_ubo_pool, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->push_ubo_set_layout, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->push_tex_set_layout, NULL);
}

static bool use_push_descriptors(PGRAPHVkState *r)
{
    if (!r->push_descriptors_supported) return false;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) return false;
#endif
    return true;
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    ShaderBinding *binding = r->shader_binding;

#if OPT_ASYNC_COMPILE
    if (!qatomic_read(&binding->ready)) {
        return;
    }
#endif

    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };

    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }

    bool push_desc = use_push_descriptors(r);

#if OPT_BINDLESS_TEXTURES
    int *ds_index_ptr;
    int ds_count;
    VkDescriptorSet *ds_array;
    if (r->bindless_textures_supported) {
        ds_index_ptr = &r->ubo_descriptor_set_index;
        ds_count = r->ubo_descriptor_set_count;
        ds_array = r->ubo_descriptor_sets;
    } else if (push_desc) {
        ds_index_ptr = &r->push_ubo_set_index;
        ds_count = r->push_ubo_set_count;
        ds_array = r->push_ubo_sets;
    } else {
        ds_index_ptr = &r->descriptor_set_index;
        ds_count = r->descriptor_set_count;
        ds_array = r->descriptor_sets;
    }
#else
    int *ds_index_ptr;
    int ds_count;
    VkDescriptorSet *ds_array;
    if (push_desc) {
        ds_index_ptr = &r->push_ubo_set_index;
        ds_count = r->push_ubo_set_count;
        ds_array = r->push_ubo_sets;
    } else {
        ds_index_ptr = &r->descriptor_set_index;
        ds_count = r->descriptor_set_count;
        ds_array = r->descriptor_sets;
    }
#endif

    bool tex_triggers_new_set = !push_desc && (
#if OPT_BINDLESS_TEXTURES
        !r->bindless_textures_supported &&
#endif
        r->texture_bindings_changed);

    bool need_new_descriptor_set =
        r->shader_bindings_changed ||
        tex_triggers_new_set ||
        r->need_descriptor_rebind ||
        !(*ds_index_ptr);

    if (need_new_descriptor_set && *ds_index_ptr >= ds_count) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
#if OPT_ALWAYS_DEFERRED_FENCES
        pgraph_vk_flush_all_frames(pg);
#endif
        *ds_index_ptr = 0;
    }

    if (r->uniforms_changed) {
        if (!pgraph_vk_buffer_has_space_for(
                pg, BUFFER_UNIFORM_STAGING, ubo_buffer_total_size,
                r->device_props.limits.minUniformBufferOffsetAlignment)) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        }

        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    if (push_desc && r->texture_bindings_changed) {
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            r->push_tex_infos[i] = (VkDescriptorImageInfo){
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .imageView = r->texture_bindings[i]->image_view,
                .sampler = r->texture_bindings[i]->sampler,
            };
        }
        r->push_tex_dirty = true;
        r->texture_bindings_changed = false;
    }

    tex_triggers_new_set = !push_desc && (
#if OPT_BINDLESS_TEXTURES
        !r->bindless_textures_supported &&
#endif
        r->texture_bindings_changed);

    need_new_descriptor_set =
        r->shader_bindings_changed ||
        tex_triggers_new_set ||
        r->need_descriptor_rebind ||
        !(*ds_index_ptr);

    if (!need_new_descriptor_set) {
        return;
    }

#if OPT_DESC_REBIND_SKIP
    if (need_new_descriptor_set &&
        !r->shader_bindings_changed && !r->texture_bindings_changed &&
        *ds_index_ptr > 0) {
        OPT_STAT_INC(desc_rebind_skips);
        r->need_descriptor_rebind = false;
        return;
    }
    OPT_STAT_INC(desc_rebind_full);
#endif

    if (*ds_index_ptr >= ds_count) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
#if OPT_ALWAYS_DEFERRED_FENCES
        pgraph_vk_flush_all_frames(pg);
#endif
        *ds_index_ptr = 0;
    }

    assert(*ds_index_ptr < ds_count);

#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        VkWriteDescriptorSet descriptor_writes[2];
        VkDescriptorBufferInfo ubo_buffer_infos[2];
        for (int i = 0; i < 2; i++) {
            ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
                .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
                .offset = 0,
                .range = layouts[i]->total_size,
            };
            descriptor_writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = ds_array[*ds_index_ptr],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .descriptorCount = 1,
                .pBufferInfo = &ubo_buffer_infos[i],
            };
        }
        vkUpdateDescriptorSets(r->device, 2, descriptor_writes, 0, NULL);
    } else
#endif
    if (push_desc) {
        VkWriteDescriptorSet descriptor_writes[2];
        VkDescriptorBufferInfo ubo_buffer_infos[2];
        for (int i = 0; i < 2; i++) {
            ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
                .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
                .offset = 0,
                .range = layouts[i]->total_size,
            };
            descriptor_writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = ds_array[*ds_index_ptr],
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .descriptorCount = 1,
                .pBufferInfo = &ubo_buffer_infos[i],
            };
        }
        vkUpdateDescriptorSets(r->device, 2, descriptor_writes, 0, NULL);
    } else {
        VkWriteDescriptorSet descriptor_writes[2 + NV2A_MAX_TEXTURES];

        VkDescriptorBufferInfo ubo_buffer_infos[2];
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
                .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
                .offset = 0,
                .range = layouts[i]->total_size,
            };
            descriptor_writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = ds_array[*ds_index_ptr],
                .dstBinding = i == 0 ? VSH_UBO_BINDING : PSH_UBO_BINDING,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .descriptorCount = 1,
                .pBufferInfo = &ubo_buffer_infos[i],
            };
        }

        VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
#if OPT_ALWAYS_DEFERRED_FENCES
            if (r->texture_bindings[i]->image_view == VK_NULL_HANDLE) {
                VK_LOG_ERROR("DIAG: descriptor set %d binding tex[%d] "
                             "has NULL image_view! image=%p",
                             *ds_index_ptr, i,
                             (void *)r->texture_bindings[i]->image);
            }
#endif
            image_infos[i] = (VkDescriptorImageInfo){
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .imageView = r->texture_bindings[i]->image_view,
                .sampler = r->texture_bindings[i]->sampler,
            };
            descriptor_writes[2 + i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = ds_array[*ds_index_ptr],
                .dstBinding = PSH_TEX_BINDING + i,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .pImageInfo = &image_infos[i],
            };
        }

        vkUpdateDescriptorSets(r->device, 6, descriptor_writes, 0, NULL);
    }

    r->need_descriptor_rebind = false;
    (*ds_index_ptr)++;
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static uint64_t hash_shader_module_key(const ShaderModuleCacheKey *key)
{
    uint64_t h = fast_hash((const uint8_t *)&key->kind, sizeof(key->kind));
    switch (key->kind) {
    case VK_SHADER_STAGE_VERTEX_BIT: {
        size_t common = offsetof(VshState, fixed_function);
        h ^= fast_hash((const uint8_t *)&key->vsh.state, common);
        if (key->vsh.state.is_fixed_function) {
            h ^= fast_hash((const uint8_t *)&key->vsh.state.fixed_function,
                            sizeof(FixedFunctionVshState));
        } else {
            size_t prog_size = offsetof(ProgrammableVshState, program_data) +
                key->vsh.state.programmable.program_length *
                    VSH_TOKEN_SIZE * sizeof(uint32_t);
            h ^= fast_hash((const uint8_t *)&key->vsh.state.programmable,
                            prog_size);
        }
        h ^= fast_hash((const uint8_t *)&key->vsh.glsl_opts,
                        sizeof(key->vsh.glsl_opts));
        break;
    }
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        h ^= fast_hash((const uint8_t *)&key->geom,
                        sizeof(key->geom));
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        h ^= fast_hash((const uint8_t *)&key->psh,
                        sizeof(key->psh));
        break;
    default:
        h ^= fast_hash((const uint8_t *)key, sizeof(*key));
        break;
    }
    return h;
}

static int compare_shader_module_key(const ShaderModuleCacheKey *a,
                                     const ShaderModuleCacheKey *b)
{
    if (a->kind != b->kind) return 1;
    switch (a->kind) {
    case VK_SHADER_STAGE_VERTEX_BIT: {
        size_t common = offsetof(VshState, fixed_function);
        int r = memcmp(&a->vsh.state, &b->vsh.state, common);
        if (r) return r;
        if (a->vsh.state.is_fixed_function != b->vsh.state.is_fixed_function)
            return 1;
        if (a->vsh.state.is_fixed_function) {
            r = memcmp(&a->vsh.state.fixed_function,
                        &b->vsh.state.fixed_function,
                        sizeof(FixedFunctionVshState));
        } else {
            if (a->vsh.state.programmable.program_length !=
                b->vsh.state.programmable.program_length)
                return 1;
            size_t prog_size = offsetof(ProgrammableVshState, program_data) +
                a->vsh.state.programmable.program_length *
                    VSH_TOKEN_SIZE * sizeof(uint32_t);
            r = memcmp(&a->vsh.state.programmable,
                        &b->vsh.state.programmable, prog_size);
        }
        if (r) return r;
        return memcmp(&a->vsh.glsl_opts, &b->vsh.glsl_opts,
                      sizeof(a->vsh.glsl_opts));
    }
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return memcmp(&a->geom, &b->geom, sizeof(a->geom));
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return memcmp(&a->psh, &b->psh, sizeof(a->psh));
    default:
        return memcmp(a, b, sizeof(*a));
    }
}

static ShaderModuleCacheEntry *
get_shader_module_entry_for_key(PGRAPHVkState *r,
                                const ShaderModuleCacheKey *key)
{
    uint64_t hash = hash_shader_module_key(key);
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    return container_of(node, ShaderModuleCacheEntry, node);
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    ShaderModuleCacheEntry *entry = get_shader_module_entry_for_key(r, key);
    pgraph_vk_ref_shader_module(entry->module_info);
    return entry->module_info;
}

static void shader_binding_build_module_keys(
    PGRAPHVkState *r, ShaderBinding *binding,
    ShaderModuleCacheKey *vsh_key, ShaderModuleCacheKey *geom_key,
    ShaderModuleCacheKey *psh_key, bool *need_geom)
{
    *need_geom = pgraph_glsl_need_geom(&binding->state.geom);

    if (*need_geom) {
        memset(geom_key, 0, sizeof(*geom_key));
        geom_key->kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        geom_key->geom.state = binding->state.geom;
        geom_key->geom.glsl_opts.vulkan = true;
    }

    memset(vsh_key, 0, sizeof(*vsh_key));
    vsh_key->kind = VK_SHADER_STAGE_VERTEX_BIT;
    vsh_key->vsh.state = binding->state.vsh;
    vsh_key->vsh.glsl_opts.vulkan = true;
    vsh_key->vsh.glsl_opts.prefix_outputs = *need_geom;
    vsh_key->vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        vsh_key->vsh.glsl_opts.ubo_binding = 0;
        vsh_key->vsh.glsl_opts.ubo_set = 1;
        vsh_key->vsh.glsl_opts.vertex_push_offset =
            (r->tex_push_offset == 0)
                ? (int)(NV2A_MAX_TEXTURES * sizeof(uint32_t))
                : 0;
    } else
#endif
    if (use_push_descriptors(r)) {
        vsh_key->vsh.glsl_opts.ubo_binding = 0;
        vsh_key->vsh.glsl_opts.ubo_set = 1;
    } else {
        vsh_key->vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    }

    memset(psh_key, 0, sizeof(*psh_key));
    psh_key->kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    psh_key->psh.state = binding->state.psh;
    psh_key->psh.glsl_opts.vulkan = true;
#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        psh_key->psh.glsl_opts.ubo_binding = 1;
        psh_key->psh.glsl_opts.ubo_set = 1;
        psh_key->psh.glsl_opts.bindless = true;
        psh_key->psh.glsl_opts.tex_push_offset = r->tex_push_offset;
        psh_key->psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    } else
#endif
    if (use_push_descriptors(r)) {
        psh_key->psh.glsl_opts.ubo_binding = 1;
        psh_key->psh.glsl_opts.ubo_set = 1;
        psh_key->psh.glsl_opts.tex_binding = 0;
    } else {
        psh_key->psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
        psh_key->psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    }
}

#if OPT_ASYNC_COMPILE
static bool try_finalize_shader_binding(PGRAPHVkState *r,
                                        ShaderBinding *binding)
{
    ShaderModuleCacheEntry *vsh_entry = binding->pending_vsh_entry;
    ShaderModuleCacheEntry *geom_entry = binding->pending_geom_entry;
    ShaderModuleCacheEntry *psh_entry = binding->pending_psh_entry;

    if (!qatomic_read(&vsh_entry->ready)) return false;
    if (geom_entry && !qatomic_read(&geom_entry->ready)) return false;
    if (!qatomic_read(&psh_entry->ready)) return false;

    pgraph_vk_ref_shader_module(vsh_entry->module_info);
    binding->vsh.module_info = vsh_entry->module_info;
    if (geom_entry) {
        pgraph_vk_ref_shader_module(geom_entry->module_info);
        binding->geom.module_info = geom_entry->module_info;
    } else {
        binding->geom.module_info = NULL;
    }
    pgraph_vk_ref_shader_module(psh_entry->module_info);
    binding->psh.module_info = psh_entry->module_info;

    update_shader_uniform_locs(binding);

    binding->pending_vsh_entry = NULL;
    binding->pending_geom_entry = NULL;
    binding->pending_psh_entry = NULL;
    qatomic_set(&binding->ready, true);
    return true;
}
#endif

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
    g_nv2a_stats.shader_stats.shader_cache_misses++;

    ShaderModuleCacheKey vsh_key, geom_key, psh_key;
    bool need_geom;
    shader_binding_build_module_keys(r, binding, &vsh_key, &geom_key,
                                     &psh_key, &need_geom);

#if OPT_ASYNC_COMPILE
    if (xemu_get_async_compile()) {
        ShaderModuleCacheEntry *vsh_entry =
            get_shader_module_entry_for_key(r, &vsh_key);
        ShaderModuleCacheEntry *geom_entry = need_geom
            ? get_shader_module_entry_for_key(r, &geom_key)
            : NULL;
        ShaderModuleCacheEntry *psh_entry =
            get_shader_module_entry_for_key(r, &psh_key);

        bool all_ready = qatomic_read(&vsh_entry->ready) &&
                         (!geom_entry || qatomic_read(&geom_entry->ready)) &&
                         qatomic_read(&psh_entry->ready);

        if (all_ready) {
            pgraph_vk_ref_shader_module(vsh_entry->module_info);
            binding->vsh.module_info = vsh_entry->module_info;
            if (geom_entry) {
                pgraph_vk_ref_shader_module(geom_entry->module_info);
                binding->geom.module_info = geom_entry->module_info;
            } else {
                binding->geom.module_info = NULL;
            }
            pgraph_vk_ref_shader_module(psh_entry->module_info);
            binding->psh.module_info = psh_entry->module_info;
            update_shader_uniform_locs(binding);
            binding->ready = true;
        } else {
            binding->vsh.module_info = NULL;
            binding->geom.module_info = NULL;
            binding->psh.module_info = NULL;
            binding->pending_vsh_entry = vsh_entry;
            binding->pending_geom_entry = geom_entry;
            binding->pending_psh_entry = psh_entry;
            binding->ready = false;
        }
        return;
    }
    binding->ready = true;
#endif

    if (need_geom) {
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &geom_key);
    } else {
        binding->geom.module_info = NULL;
    }
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &vsh_key);
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &psh_key);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return pgraph_glsl_compare_shader_state(&snode->state, key);
}

static bool shader_module_warmup_in_progress;

void shader_module_key_persist(const ShaderModuleCacheKey *key)
{
    if (!g_config.perf.cache_shaders || shader_module_warmup_in_progress) {
        return;
    }

    const char *base = xemu_settings_get_base_path();
    char *path = g_strdup_printf("%sshader_module_keys.bin", base);

    FILE *f = fopen(path, "ab");
    if (f) {
        fwrite(key, sizeof(ShaderModuleCacheKey), 1, f);
        fclose(f);
    }
    g_free(path);
}

static void shader_module_compile_sync(PGRAPHVkState *r,
                                       ShaderModuleCacheEntry *module)
{
    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);

    shader_module_key_persist(&module->key);
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

#if OPT_ASYNC_COMPILE
    if (xemu_get_async_compile()) {
        module->module_info = NULL;
        module->ready = false;

        CompileJob *job = g_malloc0(sizeof(CompileJob));
        job->type = COMPILE_JOB_SHADER_MODULE;
        job->shader_module.target = module;
        memcpy(&job->shader_module.key, key, sizeof(ShaderModuleCacheKey));
        pgraph_vk_compile_worker_enqueue(r, job);
        return;
    }
    module->ready = true;
#endif

    shader_module_compile_sync(r, module);
}

#if OPT_ASYNC_COMPILE
static bool shader_module_cache_pre_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return qatomic_read(&module->ready);
}

static bool shader_cache_pre_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return qatomic_read(&binding->ready);
}
#endif

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    if (module->module_info) {
        pgraph_vk_unref_shader_module(r, module->module_info);
        module->module_info = NULL;
    }
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return compare_shader_module_key(&module->key, key);
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache, 2048);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;
#if OPT_ASYNC_COMPILE
    r->shader_cache.pre_node_evict = shader_cache_pre_evict;
#endif

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache, 1 << 16);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
#if OPT_ASYNC_COMPILE
    r->shader_module_cache.pre_node_evict = shader_module_cache_pre_evict;
#endif

    if (g_config.perf.cache_shaders) {
        const char *base = xemu_settings_get_base_path();
        char *path = g_strdup_printf("%sshader_module_keys.bin", base);
        gchar *data = NULL;
        gsize len = 0;

        if (g_file_get_contents(path, &data, &len, NULL) && len > 0) {
            size_t num_keys = len / sizeof(ShaderModuleCacheKey);
            ShaderModuleCacheKey *keys = (ShaderModuleCacheKey *)data;

            shader_module_warmup_in_progress = true;
            int warmed = 0;
            for (size_t i = 0; i < num_keys; i++) {
                uint64_t hash = hash_shader_module_key(&keys[i]);
                if (!lru_contains_hash(&r->shader_module_cache, hash)) {
                    lru_lookup(&r->shader_module_cache, hash, &keys[i]);
                    warmed++;
                }
            }
            shader_module_warmup_in_progress = false;

            VK_LOG_ERROR("Shader module warm-up: %d/%zu modules pre-compiled",
                         warmed, num_keys);
        }
        g_free(data);
        g_free(path);
    }
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    lru_destroy(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    lru_destroy(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    unsigned int misses_before = g_nv2a_stats.shader_stats.shader_cache_misses;
    uint64_t hash = pgraph_glsl_hash_shader_state(state);
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    if (g_nv2a_stats.shader_stats.shader_cache_misses == misses_before) {
        g_nv2a_stats.shader_stats.shader_cache_hits++;
    }
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

void pgraph_vk_update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    ShaderBinding *binding = r->shader_binding;

#if OPT_ASYNC_COMPILE
    if (!qatomic_read(&binding->ready)) {
        NV2A_VK_DGROUP_END();
        return;
    }
#endif

    ShaderUniformLayout *vsh_layout = &binding->vsh.module_info->uniforms;
    ShaderUniformLayout *psh_layout = &binding->psh.module_info->uniforms;

#if OPT_UNIFORM_SKIP
    uint64_t vsh_hash_before = fast_hash(vsh_layout->allocation,
                                         vsh_layout->total_size);
    uint64_t psh_hash_before = fast_hash(psh_layout->allocation,
                                         psh_layout->total_size);
#endif

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->vsh.uniform_locs, &vsh_values);
    apply_uniform_updates(vsh_layout, VshUniformInfo,
                          binding->vsh.uniform_locs, &vsh_values,
                          VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[pg->vk_renderer_state
                                             ->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values.texScale[i] = scale;
    }
    apply_uniform_updates(psh_layout, PshUniformInfo,
                          binding->psh.uniform_locs, &psh_values,
                          PshUniform__COUNT);

#if OPT_UNIFORM_SKIP
    uint64_t vsh_hash_after = fast_hash(vsh_layout->allocation,
                                        vsh_layout->total_size);
    uint64_t psh_hash_after = fast_hash(psh_layout->allocation,
                                        psh_layout->total_size);
    if (vsh_hash_before != vsh_hash_after ||
        psh_hash_before != psh_hash_after) {
        r->uniforms_changed = true;
    }
#else
    r->uniforms_changed = true;
#endif

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state;
        if (r->cached_shader_state_valid &&
            r->cached_shader_state_gen == pg->shader_state_gen &&
            !pg->program_data_dirty) {
            new_state = r->cached_shader_state;
            new_state.geom.primitive_mode =
                pgraph_prim_rewrite_get_output_mode(
                    (enum ShaderPrimitiveMode)pg->primitive_mode,
                    new_state.geom.polygon_front_mode);
            new_state.vsh.compressed_attrs = pg->compressed_attrs;
            new_state.vsh.uniform_attrs = pg->uniform_attrs;
            new_state.vsh.swizzle_attrs = pg->swizzle_attrs;
        } else {
            new_state = pgraph_glsl_get_shader_state(pg);
            r->cached_shader_state = new_state;
            r->cached_shader_state_gen = pg->shader_state_gen;
            r->cached_shader_state_valid = true;
        }
        if (!r->shader_binding ||
            pgraph_glsl_compare_shader_state(&r->shader_binding->state,
                                             &new_state)) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
            r->uniforms_changed = true;
            r->pipeline_state_dirty = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

#if OPT_ASYNC_COMPILE
    if (r->shader_binding && !qatomic_read(&r->shader_binding->ready)) {
        try_finalize_shader_binding(r, r->shader_binding);
    }
#endif

    pgraph_vk_update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->descriptor_set_count = NUM_GFX_DESCRIPTOR_SETS;
    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
#if OPT_BINDLESS_TEXTURES
    create_bindless_descriptor_resources(pg);
#endif
    create_push_descriptor_resources(pg);
#if OPT_ASYNC_COMPILE
    pgraph_vk_compile_worker_init(r);
#endif
    shader_cache_init(pg);

#if OPT_BINDLESS_TEXTURES
    if (r->bindless_textures_supported) {
        size_t vtx_budget = r->max_vertex_push_attrs * 4 * sizeof(float);
        r->use_push_constants_for_uniform_attrs =
            (r->device_props.limits.maxPushConstantsSize >= vtx_budget + 16);
    } else
#endif
    {
        r->use_push_constants_for_uniform_attrs =
            (r->device_props.limits.maxPushConstantsSize >=
             MAX_UNIFORM_ATTR_VALUES_SIZE);
    }
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

#if OPT_ASYNC_COMPILE
    pgraph_vk_compile_worker_shutdown(r);
#else
    (void)r;
#endif

    shader_cache_finalize(pg);
    destroy_push_descriptor_resources(pg);
#if OPT_BINDLESS_TEXTURES
    destroy_bindless_descriptor_resources(pg);
#endif
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
