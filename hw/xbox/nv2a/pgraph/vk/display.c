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

#include "renderer.h"
#include "qemu/error-report.h"
#include <EGL/egl.h>
#include <math.h>
#ifdef __ANDROID__
#include <android/log.h>
#define DBG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xemu-vk-dbg", __VA_ARGS__)
#else
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

extern bool xemu_get_frame_skip(void);

#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <vulkan/vulkan_android.h>

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID p_vkGetAndroidHardwareBufferPropertiesANDROID;

static bool ahb_interop_loaded;
static bool ahb_interop_available;

static bool load_ahb_interop_symbols(VkDevice device)
{
    if (ahb_interop_loaded) {
        return ahb_interop_available;
    }
    ahb_interop_loaded = true;

    p_eglGetNativeClientBufferANDROID =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");
    p_eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    p_vkGetAndroidHardwareBufferPropertiesANDROID =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            device, "vkGetAndroidHardwareBufferPropertiesANDROID");

    ahb_interop_available = p_eglGetNativeClientBufferANDROID &&
                            p_eglCreateImageKHR &&
                            p_eglDestroyImageKHR &&
                            p_glEGLImageTargetTexture2DOES &&
                            p_vkGetAndroidHardwareBufferPropertiesANDROID;

    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "AHB interop: %s", ahb_interop_available ? "available" : "NOT available");
    return ahb_interop_available;
}

bool pgraph_vk_gl_external_memory_available(void)
{
    return true;
}

#else /* !__ANDROID__ */

static PFNGLDELETEMEMORYOBJECTSEXTPROC p_glDeleteMemoryObjectsEXT;
static PFNGLISMEMORYOBJECTEXTPROC p_glIsMemoryObjectEXT;
static PFNGLCREATEMEMORYOBJECTSEXTPROC p_glCreateMemoryObjectsEXT;
static PFNGLIMPORTMEMORYFDEXTPROC p_glImportMemoryFdEXT;
static PFNGLTEXSTORAGEMEM2DEXTPROC p_glTexStorageMem2DEXT;

static bool gl_external_memory_loaded;
static bool gl_external_memory_available;

static bool load_gl_external_memory_symbols(void)
{
    if (gl_external_memory_loaded) {
        return gl_external_memory_available;
    }
    gl_external_memory_loaded = true;

    p_glDeleteMemoryObjectsEXT =
        (PFNGLDELETEMEMORYOBJECTSEXTPROC)eglGetProcAddress(
            "glDeleteMemoryObjectsEXT");
    p_glIsMemoryObjectEXT =
        (PFNGLISMEMORYOBJECTEXTPROC)eglGetProcAddress(
            "glIsMemoryObjectEXT");
    p_glCreateMemoryObjectsEXT =
        (PFNGLCREATEMEMORYOBJECTSEXTPROC)eglGetProcAddress(
            "glCreateMemoryObjectsEXT");
    p_glImportMemoryFdEXT =
        (PFNGLIMPORTMEMORYFDEXTPROC)eglGetProcAddress(
            "glImportMemoryFdEXT");
    p_glTexStorageMem2DEXT =
        (PFNGLTEXSTORAGEMEM2DEXTPROC)eglGetProcAddress(
            "glTexStorageMem2DEXT");

    gl_external_memory_available = p_glDeleteMemoryObjectsEXT &&
                                   p_glIsMemoryObjectEXT &&
                                   p_glCreateMemoryObjectsEXT &&
                                   p_glImportMemoryFdEXT &&
                                   p_glTexStorageMem2DEXT;
    return gl_external_memory_available;
}

bool pgraph_vk_gl_external_memory_available(void)
{
    return load_gl_external_memory_symbols();
}
#endif /* __ANDROID__ */
#endif /* HAVE_EXTERNAL_MEMORY */

static uint8_t *convert_texture_data__CR8YB8CB8YA8(uint8_t *data_out,
                                                   const uint8_t *data_in,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   unsigned int pitch)
{
    int x, y;
    for (y = 0; y < height; y++) {
        const uint8_t *line = &data_in[y * pitch];
        const uint32_t row_offset = y * width;
        for (x = 0; x < width; x++) {
            uint8_t *pixel = &data_out[(row_offset + x) * 4];
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    return data_out;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void destroy_pvideo_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(r->device, d->pvideo.sampler, NULL);
        d->pvideo.sampler = VK_NULL_HANDLE;
    }

    if (d->pvideo.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(r->device, d->pvideo.image_view, NULL);
        d->pvideo.image_view = VK_NULL_HANDLE;
    }

    if (d->pvideo.image != VK_NULL_HANDLE) {
        vmaDestroyImage(r->allocator, d->pvideo.image, d->pvideo.allocation);
        d->pvideo.image = VK_NULL_HANDLE;
        d->pvideo.allocation = VK_NULL_HANDLE;
    }
}

static void create_pvideo_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->pvideo.image == VK_NULL_HANDLE || d->pvideo.width != width ||
        d->pvideo.height != height) {
        destroy_pvideo_image(pg);
    }

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };
    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &d->pvideo.image,
                            &d->pvideo.allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = d->pvideo.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &d->pvideo.image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &d->pvideo.sampler));
}

static void upload_pvideo_image(PGRAPHState *pg, PvideoState state)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;

    create_pvideo_image(pg, state.in_width, state.in_height);

    // FIXME: Dirty tracking. We don't necessarily need to upload so much.

    size_t display_data_size = state.in_width * state.in_height * 4;

#if OPT_ALWAYS_DEFERRED_FENCES
    VkDeviceSize staging_base = pgraph_vk_staging_alloc(pg, display_data_size);
    if (staging_base == VK_WHOLE_SIZE) {
        pgraph_vk_flush_all_frames(pg);
        pgraph_vk_staging_reset(pg);
        staging_base = pgraph_vk_staging_alloc(pg, display_data_size);
        assert(staging_base != VK_WHOLE_SIZE);
    }
#else
    VkDeviceSize staging_base = 0;
#endif

    StorageBuffer *disp_staging = get_staging_buffer(r, BUFFER_STAGING_SRC);
    uint8_t *mapped_memory_ptr =
        (uint8_t *)disp_staging->mapped + staging_base;

    convert_texture_data__CR8YB8CB8YA8(
        mapped_memory_ptr, d->vram_ptr + state.base + state.offset,
        state.in_width, state.in_height, state.pitch);

    vmaFlushAllocation(r->allocator, disp_staging->allocation,
                       staging_base, display_data_size);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = disp_staging->buffer,
        .offset = staging_base,
        .size = display_data_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(
        pg, cmd, disp->pvideo.image, VK_FORMAT_R8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = staging_base,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ state.in_width, state.in_height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, disp_staging->buffer,
                           disp->pvideo.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, disp->pvideo.image,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_end_single_time_commands(pg, cmd);
}

static const char *display_frag_glsl =
    "#version 450\n"
    "layout(binding = 0) uniform sampler2D tex;\n"
    "layout(binding = 1) uniform sampler2D pvideo_tex;\n"
    "layout(binding = 2) uniform sampler2D prev_tex;\n"
    "layout(push_constant, std430) uniform PushConstants {\n"
    "    float line_offset;\n"
    "    vec2 display_size;\n"
    "    bool pvideo_enable;\n"
    "    vec2 pvideo_in_pos;\n"
    "    vec4 pvideo_pos;\n"
    "    vec4 pvideo_scale;\n"
    "    bool pvideo_color_key_enable;\n"
    "    vec3 pvideo_color_key;\n"
    "    float blend_factor;\n"
    "};\n"
    "layout(location = 0) out vec4 out_Color;\n"
    "void main()\n"
    "{\n"
    "    vec2 tex_coord = gl_FragCoord.xy/display_size;\n"
    "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
    "    tex_coord.y = 1 + rel*(tex_coord.y - 1);\n"
    "    tex_coord.y = 1 - tex_coord.y;\n"
    "    out_Color.rgba = texture(tex, tex_coord);\n"
    "    if (pvideo_enable) {\n"
    "        vec2 screen_coord = vec2(gl_FragCoord.x, display_size.y - gl_FragCoord.y) * pvideo_scale.z;\n"
    "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
    "        bvec4 clip = bvec4(lessThan(screen_coord, output_region.xy),\n"
    "                           greaterThan(screen_coord, output_region.zw));\n"
    "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
    "            vec2 out_xy = screen_coord - pvideo_pos.xy;\n"
    "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
    "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
    "        }\n"
    "    }\n"
    "    if (blend_factor > 0.0) {\n"
    "        vec2 prev_coord = gl_FragCoord.xy / display_size;\n"
    "        vec4 prev = texture(prev_tex, prev_coord);\n"
    "        out_Color.rgba = mix(out_Color.rgba, prev, blend_factor);\n"
    "    }\n"
    "}\n";

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorPoolSize pool_sizes = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 3 * NUM_DISPLAY_IMAGES,
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_sizes,
        .maxSets = NUM_DISPLAY_IMAGES,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->display.descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->display.descriptor_pool, NULL);
    r->display.descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[3];

    for (int i = 0; i < ARRAY_SIZE(bindings); i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
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
                                         &r->display.descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->display.descriptor_set_layout,
                                 NULL);
    r->display.descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[NUM_DISPLAY_IMAGES];
    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        layouts[i] = r->display.descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->display.descriptor_pool,
        .descriptorSetCount = NUM_DISPLAY_IMAGES,
        .pSetLayouts = layouts,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      r->display.descriptor_sets));
}

static void create_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkAttachmentDescription attachment;

    VkAttachmentReference color_reference;
    attachment = (VkAttachmentDescription){
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    color_reference = (VkAttachmentReference){
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    dependency.srcStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask |=
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &r->display.render_pass));
}

static void destroy_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyRenderPass(r->device, r->display.render_pass, NULL);
    r->display.render_pass = VK_NULL_HANDLE;
}

static void create_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->display.display_frag =
        pgraph_vk_create_shader_module_from_glsl(
            r, VK_SHADER_STAGE_FRAGMENT_BIT, display_frag_glsl);

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        },
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->display.display_frag->module,
            .pName = "main",
        },
     };

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
        .depthTestEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPushConstantRange push_constant_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = r->display.display_frag->push_constants.total_size,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->display.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &r->display.pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ARRAY_SIZE(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = r->display.pipeline_layout,
        .renderPass = r->display.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &r->display.pipeline));
}

static void destroy_display_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyPipeline(r->device, r->display.pipeline, NULL);
    r->display.pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, r->display.pipeline_layout, NULL);
    r->display.pipeline_layout = VK_NULL_HANDLE;

    pgraph_vk_destroy_shader_module(r, r->display.display_frag);
    r->display.display_frag = NULL;
}

static void create_frame_buffer(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->display.render_pass,
        .attachmentCount = 1,
        .pAttachments = &img->image_view,
        .width = r->display.width,
        .height = r->display.height,
        .layers = 1,
    };
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &img->framebuffer));
}

static void destroy_frame_buffer(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vkDestroyFramebuffer(r->device, img->framebuffer, NULL);
    img->framebuffer = VK_NULL_HANDLE;
}

static void destroy_single_display_image(PGRAPHState *pg, DisplayImage *img)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (img->image == VK_NULL_HANDLE) {
        return;
    }

    destroy_frame_buffer(pg, img);

    if (img->fence != VK_NULL_HANDLE) {
        if (img->fence_submitted) {
            vkWaitForFences(r->device, 1, &img->fence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(r->device, img->fence, NULL);
        img->fence = VK_NULL_HANDLE;
    }
    img->fence_submitted = false;
    img->valid = false;

    if (img->cmd_buffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(r->device, r->command_pool, 1, &img->cmd_buffer);
        img->cmd_buffer = VK_NULL_HANDLE;
    }

#if HAVE_EXTERNAL_MEMORY
    if (img->gl_texture_id) {
        glDeleteTextures(1, &img->gl_texture_id);
    }
    img->gl_texture_id = 0;

#ifdef __ANDROID__
    if (img->egl_image != EGL_NO_IMAGE_KHR && p_eglDestroyImageKHR) {
        p_eglDestroyImageKHR(eglGetCurrentDisplay(), img->egl_image);
    }
    img->egl_image = EGL_NO_IMAGE_KHR;

    if (img->ahb) {
        AHardwareBuffer_release(img->ahb);
        img->ahb = NULL;
    }
#else
    if (img->gl_memory_obj && p_glDeleteMemoryObjectsEXT) {
        p_glDeleteMemoryObjectsEXT(1, &img->gl_memory_obj);
    }
    img->gl_memory_obj = 0;
#ifdef WIN32
    CloseHandle(img->handle);
    img->handle = 0;
#endif
#endif
#endif

    vkDestroyImageView(r->device, img->image_view, NULL);
    img->image_view = VK_NULL_HANDLE;

    vkDestroyImage(r->device, img->image, NULL);
    img->image = VK_NULL_HANDLE;

    vkFreeMemory(r->device, img->memory, NULL);
    img->memory = VK_NULL_HANDLE;
}

static void destroy_current_display_image(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

#if HAVE_EXTERNAL_MEMORY
    if (d->use_external_memory) {
        pgraph_vk_gl_make_context_current();
    }
#endif

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        destroy_single_display_image(pg, &d->images[i]);
    }

    if (d->blend_prev_view) {
        vkDestroyImageView(r->device, d->blend_prev_view, NULL);
        d->blend_prev_view = VK_NULL_HANDLE;
    }
    if (d->blend_prev_image) {
        vmaDestroyImage(r->allocator, d->blend_prev_image,
                        d->blend_prev_alloc);
        d->blend_prev_image = VK_NULL_HANDLE;
        d->blend_prev_alloc = VK_NULL_HANDLE;
    }
    d->blend_prev_valid = false;

    d->render_idx = 0;
    d->display_idx = 0;
    d->draw_time = 0;
}

static bool create_single_display_image_resources(PGRAPHState *pg,
                                                   DisplayImage *img,
                                                   int width, int height,
                                                   bool use_optimal_tiling,
                                                   bool use_external_memory)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(img, 0, sizeof(*img));
#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
    img->egl_image = EGL_NO_IMAGE_KHR;
#elif !defined(WIN32)
    img->fd = -1;
#endif
#endif

#if HAVE_EXTERNAL_MEMORY && defined(__ANDROID__)
    if (use_external_memory) {
        AHardwareBuffer_Desc ahb_desc = {
            .width = width,
            .height = height,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        };
        int ret = AHardwareBuffer_allocate(&ahb_desc, &img->ahb);
        if (ret != 0) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: AHardwareBuffer_allocate failed (%d)", ret);
            return false;
        }

        VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        };
        VkResult result = p_vkGetAndroidHardwareBufferPropertiesANDROID(
            r->device, img->ahb, &ahb_props);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: vkGetAndroidHardwareBufferProperties failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkExternalMemoryImageCreateInfo ext_mem_info = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
        };
        VkImageCreateInfo image_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_mem_info,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        result = vkCreateImage(r->device, &image_create_info, NULL, &img->image);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: vkCreateImage (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        uint32_t memory_type_index = pgraph_vk_get_memory_type(
            pg, ahb_props.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type_index == 0xFFFFFFFF) {
            memory_type_index = pgraph_vk_get_memory_type(
                pg, ahb_props.memoryTypeBits, 0);
        }
        if (memory_type_index == 0xFFFFFFFF) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: no compatible memory type for AHB");
            destroy_single_display_image(pg, img);
            return false;
        }

        VkImportAndroidHardwareBufferInfoANDROID import_ahb = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .buffer = img->ahb,
        };
        VkMemoryDedicatedAllocateInfo dedicated_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import_ahb,
            .image = img->image,
        };
        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated_info,
            .allocationSize = ahb_props.allocationSize,
            .memoryTypeIndex = memory_type_index,
        };
        result = vkAllocateMemory(r->device, &alloc_info, NULL, &img->memory);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: vkAllocateMemory (AHB import) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }
        result = vkBindImageMemory(r->device, img->image, img->memory, 0);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: vkBindImageMemory (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        result = vkCreateImageView(r->device, &view_info, NULL, &img->image_view);
        if (result != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: vkCreateImageView (AHB) failed (%d)", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &img->fence));

        VkCommandBufferAllocateInfo cmd_alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cmd_alloc, &img->cmd_buffer));

        pgraph_vk_gl_make_context_current();

        EGLClientBuffer client_buf = p_eglGetNativeClientBufferANDROID(img->ahb);
        if (!client_buf) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: eglGetNativeClientBufferANDROID failed");
            destroy_single_display_image(pg, img);
            return false;
        }

        EGLint img_attrs[] = { EGL_NONE };
        img->egl_image = (void *)p_eglCreateImageKHR(
            eglGetCurrentDisplay(), EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID, client_buf, img_attrs);
        if (img->egl_image == EGL_NO_IMAGE_KHR) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: eglCreateImageKHR failed (0x%x)", eglGetError());
            destroy_single_display_image(pg, img);
            return false;
        }

        glGenTextures(1, &img->gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, img->gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img->egl_image);
        GLenum gl_err = glGetError();
        if (gl_err != GL_NO_ERROR) {
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "display: glEGLImageTargetTexture2DOES failed (0x%x)", gl_err);
            destroy_single_display_image(pg, img);
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "display: AHB image created %dx%d tex=%u",
                            width, height, img->gl_texture_id);
        return true;
    }
#endif /* HAVE_EXTERNAL_MEMORY && __ANDROID__ */

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = use_optimal_tiling ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

#if HAVE_EXTERNAL_MEMORY
    VkExternalMemoryImageCreateInfo external_memory_image_create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
#ifdef WIN32
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
    };
    if (use_external_memory) {
        image_create_info.pNext = &external_memory_image_create_info;
    }
#endif

    VkResult result = vkCreateImage(r->device, &image_create_info, NULL, &img->image);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkCreateImage failed (%d)\n", result);
        return false;
    }

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 memory_requirements2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 image_memory_requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = img->image,
    };
    vkGetImageMemoryRequirements2(r->device, &image_memory_requirements_info,
                                  &memory_requirements2);
    VkMemoryRequirements memory_requirements = memory_requirements2.memoryRequirements;

    uint32_t memory_type_index =
        pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type_index == 0xFFFFFFFF) {
        memory_type_index =
            pgraph_vk_get_memory_type(pg, memory_requirements.memoryTypeBits, 0);
    }
    if (memory_type_index == 0xFFFFFFFF) {
        fprintf(stderr, "create_display_image: no compatible memory type\n");
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };

#if HAVE_EXTERNAL_MEMORY
    VkExportMemoryAllocateInfo export_memory_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes =
#ifdef WIN32
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
#else
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
            ,
    };
#endif
    void *alloc_p_next = NULL;
    VkMemoryDedicatedAllocateInfo dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img->image,
    };
    if (dedicated_requirements.requiresDedicatedAllocation == VK_TRUE) {
        alloc_p_next = &dedicated_alloc_info;
    }
#if HAVE_EXTERNAL_MEMORY
    if (use_external_memory) {
        export_memory_alloc_info.pNext = alloc_p_next;
        alloc_p_next = &export_memory_alloc_info;
    }
#endif
    alloc_info.pNext = alloc_p_next;

    result = vkAllocateMemory(r->device, &alloc_info, NULL, &img->memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkAllocateMemory failed (%d)\n", result);
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }
    result = vkBindImageMemory(r->device, img->image, img->memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkBindImageMemory failed (%d)\n", result);
        vkFreeMemory(r->device, img->memory, NULL);
        img->memory = VK_NULL_HANDLE;
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    result = vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &img->image_view);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "create_display_image: vkCreateImageView failed (%d)\n", result);
        vkFreeMemory(r->device, img->memory, NULL);
        img->memory = VK_NULL_HANDLE;
        vkDestroyImage(r->device, img->image, NULL);
        img->image = VK_NULL_HANDLE;
        return false;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &img->fence));

#if OPT_DISPLAY_DOUBLE_BUFFER
    {
        VkCommandBufferAllocateInfo cmd_alloc = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = r->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cmd_alloc, &img->cmd_buffer));
    }
#endif

#if HAVE_EXTERNAL_MEMORY && !defined(__ANDROID__)
    if (use_external_memory) {
#ifdef WIN32
        VkMemoryGetWin32HandleInfoKHR handle_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .memory = img->memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
        };
        VK_CHECK(vkGetMemoryWin32HandleKHR(r->device, &handle_info, &img->handle));

        p_glCreateMemoryObjectsEXT(1, &img->gl_memory_obj);
        glImportMemoryWin32HandleEXT(img->gl_memory_obj, memory_requirements.size,
                                     GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, img->handle);
        assert(glGetError() == GL_NO_ERROR);
#else
        VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = img->memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        result = vkGetMemoryFdKHR(r->device, &fd_info, &img->fd);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "create_display_image: vkGetMemoryFdKHR failed (%d)\n", result);
            destroy_single_display_image(pg, img);
            return false;
        }

        p_glCreateMemoryObjectsEXT(1, &img->gl_memory_obj);
        p_glImportMemoryFdEXT(img->gl_memory_obj, memory_requirements.size,
                              GL_HANDLE_TYPE_OPAQUE_FD_EXT, img->fd);
        if (!p_glIsMemoryObjectEXT(img->gl_memory_obj) || glGetError() != GL_NO_ERROR) {
            fprintf(stderr, "create_display_image: GL memory object import failed\n");
            destroy_single_display_image(pg, img);
            return false;
        }
#endif

        const GLint gl_internal_format = GL_RGBA8;
        glGenTextures(1, &img->gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, img->gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                        use_optimal_tiling ? GL_OPTIMAL_TILING_EXT :
                                             GL_LINEAR_TILING_EXT);
        p_glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, gl_internal_format,
                               width, height, img->gl_memory_obj, 0);
        if (glGetError() != GL_NO_ERROR) {
            fprintf(stderr, "create_display_image: glTexStorageMem2DEXT failed\n");
            destroy_single_display_image(pg, img);
            return false;
        }
    }
#endif

    return true;
}

static bool create_display_image(PGRAPHState *pg, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *d = &r->display;

    if (d->images[0].image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    bool use_optimal_tiling = true;
#if HAVE_EXTERNAL_MEMORY
    bool use_external_memory = d->use_external_memory;

#ifdef __ANDROID__
    if (use_external_memory && !load_ahb_interop_symbols(r->device)) {
        __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                            "display: AHB interop not available, using download fallback");
        d->use_external_memory = false;
        use_external_memory = false;
    }
#else
    if (use_external_memory) {
        pgraph_vk_gl_make_context_current();
    }

    if (use_external_memory && !load_gl_external_memory_symbols()) {
        fprintf(stderr, "Vulkan display: GL_EXT_memory_object not available\n");
        d->use_external_memory = false;
        use_external_memory = false;
    }

    if (use_external_memory) {
        const GLint gl_internal_format = GL_RGBA8;
        GLint num_tiling_types;
        glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                              GL_NUM_TILING_TYPES_EXT, 1, &num_tiling_types);
        GLint tiling_types[num_tiling_types];
        glGetInternalformativ(GL_TEXTURE_2D, gl_internal_format,
                              GL_TILING_TYPES_EXT, num_tiling_types, tiling_types);
        for (int i = 0; i < num_tiling_types; i++) {
            if (tiling_types[i] == GL_LINEAR_TILING_EXT) {
                use_optimal_tiling = false;
                break;
            }
        }
    }
#endif
#else
    bool use_external_memory = false;
#endif

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        if (!create_single_display_image_resources(pg, &d->images[i], width, height,
                                                   use_optimal_tiling, use_external_memory)) {
            destroy_current_display_image(pg);
            return false;
        }
    }

    d->width = width;
    d->height = height;
    d->render_idx = 0;
    d->display_idx = 0;

    for (int i = 0; i < NUM_DISPLAY_IMAGES; i++) {
        create_frame_buffer(pg, &d->images[i]);
    }

    {
        VkImageCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo ai = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VK_CHECK(vmaCreateImage(r->allocator, &ci, &ai,
                                &d->blend_prev_image,
                                &d->blend_prev_alloc, NULL));

        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = d->blend_prev_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        VK_CHECK(vkCreateImageView(r->device, &vi, NULL,
                                   &d->blend_prev_view));
        d->blend_prev_valid = false;
    }

    return true;
}

static void update_descriptor_set(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorImageInfo image_infos[3];
    VkWriteDescriptorSet descriptor_writes[3];

    image_infos[0] = (VkDescriptorImageInfo){
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = surface->image_view,
        .sampler = r->display.sampler,
    };
    VkDescriptorSet current_set = r->display.descriptor_sets[r->display.render_idx];

    descriptor_writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[0],
    };

    if (r->display.pvideo.state.enabled) {
        assert(r->display.pvideo.image_view != VK_NULL_HANDLE);
        assert(r->display.pvideo.sampler != VK_NULL_HANDLE);
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->display.pvideo.image_view,
            .sampler = r->display.pvideo.sampler,
        };
    } else {
        image_infos[1] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_texture.sampler,
        };
    }

    if (r->display.blend_prev_valid && r->display.blend_prev_view) {
        image_infos[2] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->display.blend_prev_view,
            .sampler = r->display.sampler,
        };
    } else {
        image_infos[2] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->dummy_texture.image_view,
            .sampler = r->dummy_texture.sampler,
        };
    }
    descriptor_writes[1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[1],
    };

    descriptor_writes[2] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = current_set,
        .dstBinding = 2,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_infos[2],
    };

    vkUpdateDescriptorSets(r->device, ARRAY_SIZE(descriptor_writes),
                           descriptor_writes, 0, NULL);
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state;

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    state.enabled = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE)
        && d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];

    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* TODO: support other color formats */
    assert(state.format == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);

    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);

    state.in_s = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_S);
    state.in_t = GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN],
                        NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height = floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);

    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);

    // Note: PVIDEO color keying ignores alpha.
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    assert(state.offset + state.pitch * state.in_height <= state.limit);
    hwaddr end = state.base + state.offset + state.pitch * state.in_height;
    assert(end <= memory_region_size(d->vram));

    return state;
}

static void update_uniforms(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderUniformLayout *l = &r->display.display_frag->push_constants;

    int display_size_loc = uniform_index(l, "display_size");  // FIXME: Cache
    uniform2f(l, display_size_loc, r->display.width, r->display.height);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    int line_offset_loc = uniform_index(l, "line_offset");
    uniform1f(l, line_offset_loc, line_offset);

    PvideoState *pvideo = &r->display.pvideo.state;
    uniform1i(l, uniform_index(l, "pvideo_enable"), pvideo->enabled);
    if (pvideo->enabled) {
        uniform1i(l, uniform_index(l, "pvideo_color_key_enable"),
                  pvideo->color_key_enabled);
        uniform3f(
            l, uniform_index(l, "pvideo_color_key"),
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
            GET_MASK(pvideo->color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);
        uniform2f(l, uniform_index(l, "pvideo_in_pos"), pvideo->in_s / 16.f,
                  pvideo->in_t / 8.f);
        uniform4f(l, uniform_index(l, "pvideo_pos"), pvideo->out_x,
                  pvideo->out_y, pvideo->out_width, pvideo->out_height);
        uniform4f(l, uniform_index(l, "pvideo_scale"), pvideo->scale_x,
                  pvideo->scale_y, 1.0f / pg->surface_scale_factor, 1.0);
    }

    uniform1f(l, uniform_index(l, "blend_factor"),
              r->display.blend_active ? 0.5f : 0.0f);
}

static void render_display(PGRAPHState *pg, SurfaceBinding *surface)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    PGRAPHVkDisplayState *disp = &r->display;
    DisplayImage *img = &disp->images[disp->render_idx];

    if (img->image == VK_NULL_HANDLE || img->framebuffer == VK_NULL_HANDLE) {
        return;
    }

    {
        static int dbg_render = 0;
        if (dbg_render < 30) {
            DBG_LOG("[DISP] render_display: in_cb=%d draw_time=%lu cb_start=%lu",
                    r->in_command_buffer,
                    (unsigned long)surface->draw_time,
                    (unsigned long)r->command_buffer_start_time);
            dbg_render++;
        }
    }

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_PRESENTING);
    }

    pgraph_vk_upload_surface_data(d, surface, !tcg_enabled());

    disp->pvideo.state = get_pvideo_state(pg);
    if (disp->pvideo.state.enabled) {
        upload_pvideo_image(pg, disp->pvideo.state);
    }

    update_uniforms(pg, surface);
    update_descriptor_set(pg, surface);

#if OPT_DISPLAY_DOUBLE_BUFFER
    if (img->fence_submitted) {
        VK_CHECK(vkWaitForFences(r->device, 1, &img->fence, VK_TRUE, UINT64_MAX));
        img->fence_submitted = false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(img->cmd_buffer, &begin_info));
    VkCommandBuffer cmd = img->cmd_buffer;
#else
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
#endif

    pgraph_vk_begin_debug_marker(r, cmd, RGBA_YELLOW,
        "Display Surface %08"HWADDR_PRIx, surface->vram_addr);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_transition_image_layout(
        pg, cmd, img->image, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = disp->render_pass,
        .framebuffer = img->framebuffer,
        .renderArea.extent.width = disp->width,
        .renderArea.extent.height = disp->height,
    };
    vkCmdBeginRenderPass(cmd, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      disp->pipeline);

    VkDescriptorSet current_ds = disp->descriptor_sets[disp->render_idx];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            disp->pipeline_layout, 0, 1, &current_ds,
                            0, NULL);

    VkViewport viewport = {
        .width = disp->width,
        .height = disp->height,
        .minDepth = 0.0,
        .maxDepth = 1.0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .extent.width = disp->width,
        .extent.height = disp->height,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, disp->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, disp->display_frag->push_constants.total_size,
                       disp->display_frag->push_constants.allocation);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    pgraph_vk_transition_image_layout(pg, cmd, surface->image,
                                      surface->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    if (!disp->blend_active && disp->blend_prev_image) {
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        pgraph_vk_transition_image_layout(pg, cmd, disp->blend_prev_image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageCopy region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { disp->width, disp->height, 1 },
        };
        vkCmdCopyImage(cmd,
                       img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       disp->blend_prev_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);

        pgraph_vk_transition_image_layout(pg, cmd, disp->blend_prev_image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        disp->blend_prev_valid = true;
    } else {
        pgraph_vk_transition_image_layout(pg, cmd, img->image,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    pgraph_vk_end_debug_marker(r, cmd);

#if OPT_DISPLAY_DOUBLE_BUFFER
    VK_CHECK(vkEndCommandBuffer(cmd));

    vkResetFences(r->device, 1, &img->fence);
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, img->fence));
    img->fence_submitted = true;
    img->valid = true;

    disp->display_idx = disp->render_idx;
    disp->render_idx = (disp->render_idx + 1) % NUM_DISPLAY_IMAGES;
#ifdef __ANDROID__
    {
        static int render_count = 0;
        if (render_count < 5) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                "display: render_display done #%d disp_idx=%d render_idx=%d tex=%u",
                render_count, disp->display_idx, disp->render_idx,
                disp->images[disp->display_idx].gl_texture_id);
            render_count++;
        }
    }
#endif
#else
    pgraph_vk_end_single_time_commands(pg, cmd);
#endif
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);

    disp->draw_time = surface->draw_time;
}

static void create_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &r->display.sampler));
}

static void destroy_surface_sampler(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroySampler(r->device, r->display.sampler, NULL);
    r->display.sampler = VK_NULL_HANDLE;
}

void pgraph_vk_init_display(PGRAPHState *pg)
{
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    create_render_pass(pg);
    create_display_pipeline(pg);
    create_surface_sampler(pg);
}

void pgraph_vk_finalize_display(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    destroy_pvideo_image(pg);

    if (r->display.images[0].image != VK_NULL_HANDLE) {
        destroy_current_display_image(pg);
    }

    destroy_surface_sampler(pg);
    destroy_display_pipeline(pg);
    destroy_render_pass(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
}

void pgraph_vk_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    hwaddr display_addr = d->pcrtc.start + vga_display_params.line_offset;

    if (r->frame_was_skipped && xemu_get_frame_skip() &&
        r->frame_skip_last_good_addr) {
        display_addr = r->frame_skip_last_good_addr;
    } else {
        r->frame_skip_last_good_addr = display_addr;
    }

    SurfaceBinding *surface = pgraph_vk_surface_get_within(d, display_addr);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height) {
        static int dbg_no_surf = 0;
        if (dbg_no_surf < 30) {
            DBG_LOG("[DISP] no valid surface (surface=%p)", surface);
            dbg_no_surf++;
        }
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] != NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }

    pgraph_apply_scaling_factor(pg, &width, &height);

    PGRAPHVkDisplayState *disp = &r->display;
    if (!disp->images[0].image || disp->width != width || disp->height != height) {
        if (!create_display_image(pg, width, height)) {
            return;
        }
    }

    disp->blend_active = !r->frame_was_skipped &&
                         r->blend_after_skip &&
                         xemu_get_frame_skip() &&
                         disp->blend_prev_valid;
    if (disp->blend_active) {
        r->blend_after_skip = false;
    }

    render_display(pg, surface);
}
