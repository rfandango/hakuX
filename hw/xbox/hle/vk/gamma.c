/*
 * hakuX HLE GPU — the display's gamma ramp (SetGammaRamp) at swap: the
 * swap's blit becomes a fullscreen pass that samples the presented surface
 * and maps each channel through the ramp (the NV2A DAC does this at
 * scanout). Identity ramps keep the blit (back.c).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back_int.h"

#define T (g_hvk.t)
#define GM_SLOTS  HLE_FRAMES            /* one per command buffer in flight */
#define GM_STRIDE 1024                  /* per slot in the ramp buffer (768 used) */
#define GM_VIEWS  8

static const char k_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char k_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(set = 0, binding = 1, std140) uniform G { uvec4 w[48]; } g;\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "uint lut(uint i) {\n"
    "  uint word = g.w[i >> 4][(i >> 2) & 3u];\n"
    "  return (word >> ((i & 3u) * 8u)) & 0xFFu;\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = texture(src, uv);\n"
    "  uvec3 v = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  o = vec4(float(lut(v.r)), float(lut(256u + v.g)), float(lut(512u + v.b)), 255.0)"
    " / 255.0;\n"
    "  o.a = c.a;\n"
    "}\n";

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkSampler smp;
    VkDescriptorPool pool;
    VkDescriptorSet set[GM_SLOTS];
    VkBuffer ubo;
    VkDeviceMemory mem;
    uint8_t *map;
    struct {
        VkImage img;
        VkImageView view;
    } dst[GM_VIEWS];
    unsigned ndst;
    uint32_t dst_gen;                   /* the display images' generation they are views of */
} g_gm;

static VkShaderModule module(HleShaderStage st, const char *src)
{
    size_t n = 0;
    uint32_t *spv = hle_glsl_compile(st, src, &n);
    if (!spv) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = n * 4, .pCode = spv };
    VkShaderModule m = VK_NULL_HANDLE;
    T.vkCreateShaderModule(g_hvk.dev, &ci, NULL, &m);
    g_free(spv);
    return m;
}

static bool init(void)
{
    if (g_gm.ready || g_gm.failed) {
        return g_gm.ready;
    }
    g_gm.failed = true;                 /* until everything below succeeds */
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_gm.dsl };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GM_SLOTS },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, GM_SLOTS },
    };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = GM_SLOTS, .poolSizeCount = 2,
                                       .pPoolSizes = ps };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = GM_SLOTS * GM_STRIDE,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_gm.dsl) != VK_SUCCESS ||
        T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_gm.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_gm.smp) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_gm.pool) != VK_SUCCESS ||
        T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_gm.ubo) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "gamma: setup failed (ramps not applied)");
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_gm.ubo, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_gm.mem) != VK_SUCCESS ||
        T.vkBindBufferMemory(g_hvk.dev, g_gm.ubo, g_gm.mem, 0) != VK_SUCCESS ||
        T.vkMapMemory(g_hvk.dev, g_gm.mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_gm.map) !=
            VK_SUCCESS) {
        hle_log("hakuX-hle-back", "gamma: buffer failed (ramps not applied)");
        return false;
    }
    VkDescriptorSetLayout ls[GM_SLOTS] = { g_gm.dsl, g_gm.dsl, g_gm.dsl };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_gm.pool,
                                        .descriptorSetCount = GM_SLOTS, .pSetLayouts = ls };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, g_gm.set) != VK_SUCCESS) {
        return false;
    }
    VkShaderModule vm = module(HLE_SHADER_VERTEX, k_vs), fm = module(HLE_SHADER_FRAGMENT, k_fs);
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_gm.pl };
        T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &g_gm.pipe);
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    if (!g_gm.pipe) {
        hle_log("hakuX-hle-back", "gamma: pipeline failed (ramps not applied)");
        return false;
    }
    g_gm.failed = false;
    g_gm.ready = true;
    hle_log("hakuX-hle-back", "gamma: display pass ready");
    return true;
}

/* A view of display image img, kept while the display's images are the same
 * ones: made again (a new size), a handle can repeat a destroyed image's,
 * and the views of the old ones go (their GPU work done: the display waited
 * for the device before it destroyed them). */
static VkImageView dst_view(VkImage img)
{
    if (g_gm.dst_gen != hle_disp_gen()) {
        for (unsigned i = 0; i < g_gm.ndst; i++) {
            T.vkDestroyImageView(g_hvk.dev, g_gm.dst[i].view, NULL);
            g_gm.dst[i].view = VK_NULL_HANDLE;  /* not destroyed again when i is reused */
        }
        g_gm.ndst = 0;
        g_gm.dst_gen = hle_disp_gen();
    }
    for (unsigned i = 0; i < g_gm.ndst; i++) {
        if (g_gm.dst[i].img == img) {
            return g_gm.dst[i].view;
        }
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v = VK_NULL_HANDLE;
    if (T.vkCreateImageView(g_hvk.dev, &vci, NULL, &v) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    unsigned i = g_gm.ndst < GM_VIEWS ? g_gm.ndst++ : 0;  /* display slots: a few */
    if (g_gm.dst[i].view) {
        T.vkDestroyImageView(g_hvk.dev, g_gm.dst[i].view, NULL);
    }
    g_gm.dst[i].img = img;
    g_gm.dst[i].view = v;
    return v;
}

bool hle_gamma_pass(VkCommandBuffer cb, unsigned slot, const uint8_t ramp[3][256],
                    HleVkSurf *src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw, uint32_t dh)
{
    if (!init()) {
        return false;
    }
    VkImageView dv = dst_view(dst);
    if (!dv) {
        return false;
    }
    slot %= GM_SLOTS;                   /* this frame slot's earlier use has completed */
    memcpy(g_gm.map + slot * GM_STRIDE, ramp, 768);
    hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = { g_gm.smp, src->view, src->layout };   /* or GENERAL */
    VkDescriptorBufferInfo bi = { g_gm.ubo, slot * GM_STRIDE, 768 };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_gm.set[slot],
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_gm.set[slot],
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
    };
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, w, 0, NULL);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { dw, dh } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    /* the whole of src over a viewport its shown part (sw x sh) fills dw x dh of */
    VkViewport vp = { 0, 0, (float)dw * src->w / MAX(sw, 1u), (float)dh * src->h / MAX(sh, 1u), 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { dw, dh } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gm.pipe);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_gm.pl, 0, 1,
                              &g_gm.set[slot], 0, NULL);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    T.vkCmdEndRendering(cb);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

/* The core stops (back_int.h): the pass's objects and the display images'
 * views destroyed, and init and dst_view as before the first ramp (a setup
 * that failed is tried again, on the next device). */
void hle_gamma_shutdown(void)
{
    VkDevice d = g_hvk.dev;
    for (unsigned i = 0; i < g_gm.ndst; i++) {
        T.vkDestroyImageView(d, g_gm.dst[i].view, NULL);
    }
    T.vkDestroyPipeline(d, g_gm.pipe, NULL);
    T.vkDestroyDescriptorPool(d, g_gm.pool, NULL);      /* its sets with it */
    if (g_gm.map) {
        T.vkUnmapMemory(d, g_gm.mem);
    }
    T.vkDestroyBuffer(d, g_gm.ubo, NULL);
    T.vkFreeMemory(d, g_gm.mem, NULL);
    T.vkDestroySampler(d, g_gm.smp, NULL);
    T.vkDestroyPipelineLayout(d, g_gm.pl, NULL);
    T.vkDestroyDescriptorSetLayout(d, g_gm.dsl, NULL);
    memset(&g_gm, 0, sizeof(g_gm));
}
