/*
 * hakuX HLE GPU — depth buffers read as colour (HLE_OP_ZREAD): a texture over
 * a D24S8 target's memory samples the NV2A's 32-bit words there (depth << 8
 * | stencil). A fullscreen pass fetches each texel's depth and stencil and
 * writes the word's bytes as the texture format's channels into the colour
 * image the texture samples, or for a 16-bit texture twice as wide the
 * words' halves (HLE_ZREAD_HALVES).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back_int.h"

#define T (g_hvk.t)
#define ZR_SLOTS    HLE_FRAMES          /* one per command buffer in flight */
#define ZR_PER_SLOT 16                  /* conversions a recording */

static const char k_vs[] =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char k_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D zt;\n"
    "layout(set = 0, binding = 1) uniform usampler2D st;\n"
    "layout(push_constant) uniform P { uvec4 map; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "float ch(uint w, uint m) {\n"
    "  return m > 3u ? 1.0 : float((w >> (8u * m)) & 0xFFu) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    /* halves (HLE_ZREAD_HALVES): two texels a word, the pixel's guest texel
     * (map.y: the image's scale) even for the low half. Scaled, both get the
     * upper one: a title maps the double-width texture over its frame, so a
     * pixel's centre samples the odd texel at 1x (the depth's top 16 bits),
     * but half of a scaled pixel's samples fell on the even one (the low
     * depth byte and the stencil): Midtown Madness 3's sky mask became a
     * grid over the whole frame */
    "  bool halves = pc.map.x == 5u;\n"
    "  ivec2 q = halves ? ivec2(p.x >> 1, p.y) : p;\n"
    /* clamped: 1.0 * 16777215 + 0.5 rounds to 2^24 in fp32, and d << 8 would
     * wrap the far plane to 0 (Mercenaries' sky dome, depth-tested against
     * its copied scene depth, then failed wherever the scene was empty) */
    "  uint d = min(uint(clamp(texelFetch(zt, q, 0).r, 0.0, 1.0) * 16777215.0 + 0.5),\n"
    "               16777215u);\n"
    "  uint w = d << 8 | (texelFetch(st, q, 0).r & 0xFFu);\n"
    "  if (halves) {\n"
    "    uint h = pc.map.y > 1u || ((uint(p.x) / max(pc.map.y, 1u)) & 1u) != 0u ? w >> 16\n"
    "                                                                          : w & 0xFFFFu;\n"
    "    o = vec4(float(h) / 65535.0);\n"
    "    return;\n"
    "  }\n"
    "  o = vec4(ch(w, pc.map.x), ch(w, pc.map.y), ch(w, pc.map.z), ch(w, pc.map.w));\n"
    "}\n";

/* HLE_OP_ZWRITE: a colour surface's 32-bit words a.r.g.b as D24S8 depth << 8 |
 * stencil; the depth is the top 24 bits (k_fs_zw), the stencil the low byte
 * (k_fs_zws: one pass per bit, each writing that bit where it is set, after
 * the stencil is cleared: no shader stencil export needed) */
static const char k_fs_zw[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D ct;\n"
    "void main() {\n"
    "  uvec4 b = uvec4(clamp(texelFetch(ct, ivec2(gl_FragCoord.xy), 0), 0.0, 1.0) * 255.0 + 0.5);\n"
    "  uint d = b.a << 16 | b.r << 8 | b.g;\n"
    "  gl_FragDepth = float(d) / 16777215.0;\n"
    "}\n";

static const char k_fs_zws[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D ct;\n"
    "layout(push_constant) uniform P { uvec4 map; } pc;\n"
    "void main() {\n"
    "  uint s = uint(clamp(texelFetch(ct, ivec2(gl_FragCoord.xy), 0).b, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  if (((s >> pc.map.x) & 1u) == 0u) discard;\n"
    "}\n";

static struct {
    bool ready, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkSampler smp;
    VkDescriptorPool pool;
    VkDescriptorSet set[ZR_SLOTS][ZR_PER_SLOT];
    VkShaderModule vs, fs, fs_zw, fs_zws;
    struct { VkFormat f; bool bits; VkPipeline p; } zw[4];
    struct { VkFormat f; VkPipeline p; } pipe[4];
    unsigned npipe, nzw;
    unsigned slot, used;
    uint64_t rec;                       /* submit count when slot's recording began */
} g_zr;

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
    if (g_zr.ready || g_zr.failed) {
        return g_zr.ready;
    }
    g_zr.failed = true;                 /* until everything below succeeds */
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = b };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &g_zr.dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                2 * ZR_SLOTS * ZR_PER_SLOT };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = ZR_SLOTS * ZR_PER_SLOT, .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_zr.dsl) != VK_SUCCESS ||
        T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_zr.pl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_zr.smp) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &dpi, NULL, &g_zr.pool) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "zread: setup failed (depth buffers not read as colour)");
        return false;
    }
    VkDescriptorSetLayout ls[ZR_SLOTS * ZR_PER_SLOT];
    for (unsigned i = 0; i < ARRAY_SIZE(ls); i++) {
        ls[i] = g_zr.dsl;
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = g_zr.pool,
                                        .descriptorSetCount = ARRAY_SIZE(ls), .pSetLayouts = ls };
    g_zr.vs = module(HLE_SHADER_VERTEX, k_vs);
    g_zr.fs = module(HLE_SHADER_FRAGMENT, k_fs);
    g_zr.fs_zw = module(HLE_SHADER_FRAGMENT, k_fs_zw);
    g_zr.fs_zws = module(HLE_SHADER_FRAGMENT, k_fs_zws);
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &dai, &g_zr.set[0][0]) != VK_SUCCESS ||
        !g_zr.vs || !g_zr.fs || !g_zr.fs_zw || !g_zr.fs_zws) {
        hle_log("hakuX-hle-back", "zread: shaders failed (depth buffers not read as colour)");
        return false;
    }
    g_zr.failed = false;
    g_zr.ready = true;
    hle_log("hakuX-hle-back", "zread: depth-as-colour pass ready");
    return true;
}

/* The pipeline writing colour format f, made on first use. */
static VkPipeline pipe_for(VkFormat f)
{
    for (unsigned i = 0; i < g_zr.npipe; i++) {
        if (g_zr.pipe[i].f == f) {
            return g_zr.pipe[i].p;
        }
    }
    if (g_zr.npipe == ARRAY_SIZE(g_zr.pipe)) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_zr.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_zr.fs, .pName = "main" },
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
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
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
    VkPipelineRenderingCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g_zr.pl };
    VkPipeline p = VK_NULL_HANDLE;
    if (T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &p) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_zr.pipe[g_zr.npipe].f = f;
    g_zr.pipe[g_zr.npipe++].p = p;
    return p;
}

/* The pipeline writing depth-stencil format f's depth, or (bits) one stencil
 * bit a pass (the write mask) (HLE_OP_ZWRITE). */
static VkPipeline zw_pipe_for(VkFormat f, bool stencil, bool bits)
{
    for (unsigned i = 0; i < g_zr.nzw; i++) {
        if (g_zr.zw[i].f == f && g_zr.zw[i].bits == bits) {
            return g_zr.zw[i].p;
        }
    }
    if (g_zr.nzw == ARRAY_SIZE(g_zr.zw)) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_zr.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = bits ? g_zr.fs_zws : g_zr.fs_zw,
          .pName = "main" },
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
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* bits: the stencil bit the write mask lets through, set where the
     * fragment survives (reference 0xFF, REPLACE); the depth untouched */
    const VkStencilOpState sop = { .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
                                   .depthFailOp = VK_STENCIL_OP_KEEP,
                                   .compareOp = VK_COMPARE_OP_ALWAYS, .compareMask = 0xFF,
                                   .writeMask = 0xFF, .reference = 0xFF };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = !bits, .depthWriteEnable = !bits,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = bits,
        .front = sop, .back = sop };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = bits ? 3 : 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .depthAttachmentFormat = f,
        .stencilAttachmentFormat = stencil ? f : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = g_zr.pl };
    VkPipeline p = VK_NULL_HANDLE;
    if (T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &p) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_zr.zw[g_zr.nzw].f = f;
    g_zr.zw[g_zr.nzw].bits = bits;
    g_zr.zw[g_zr.nzw++].p = p;
    return p;
}

/* Depth (i 0) or stencil (1) view of depth buffer s (destroyed with it,
 * surf.c). */
static bool view(HleVkSurf *s, unsigned i)
{
    VkImageView *v = i ? &s->zr_stencil : &s->zr_depth;
    if (*v) {
        return true;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->fmt,
        .subresourceRange = { i ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
                              0, 1, 0, 1 } };
    if (T.vkCreateImageView(g_hvk.dev, &vci, NULL, v) != VK_SUCCESS) {
        *v = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool views(HleVkSurf *s)
{
    return view(s, 0) && view(s, 1);
}

VkImageView hle_zread_depth_view(HleVkSurf *s)
{
    return s->img && view(s, 0) ? s->zr_depth : VK_NULL_HANDLE;
}

/* A descriptor set of this recording's slot (NULL: none left). */
static VkDescriptorSet slot_set(unsigned slot)
{
    /* a new recording: its slot's sets are free again (cb_get waited for the
     * slot's last submission) */
    slot %= ZR_SLOTS;
    uint64_t rec = hle_back_submits();
    if (slot != g_zr.slot || rec != g_zr.rec) {
        g_zr.slot = slot;
        g_zr.rec = rec;
        g_zr.used = 0;
    }
    return g_zr.used < ZR_PER_SLOT ? g_zr.set[slot][g_zr.used++] : VK_NULL_HANDLE;
}

const char *hle_zwrite_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst)
{
    if (!init()) {
        return "setup failed";
    }
    bool stencil = (dst->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    VkPipeline p = zw_pipe_for(dst->fmt, stencil, false);
    VkPipeline pb = stencil ? zw_pipe_for(dst->fmt, true, true) : VK_NULL_HANDLE;
    if (!p || (stencil && !pb)) {
        return "no pipeline";
    }
    VkDescriptorSet set = slot_set(slot);
    if (!set) {
        return "too many this recording";
    }
    VkImageView sv = src->view, cube = VK_NULL_HANDLE;
    if (src->general) {
        hle_surf_sample_views(src, &sv, &cube);
    } else {
        hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkDescriptorImageInfo ii[2] = { { g_zr.smp, sv, src->layout },
                                    { g_zr.smp, sv, src->layout } };
    VkWriteDescriptorSet w[2];
    for (unsigned i = 0; i < 2; i++) {
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ii[i] };
    }
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, w, 0, NULL);
    hle_surf_transition(cb, dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    uint32_t fw = MIN(src->w, dst->w), fh = MIN(src->h, dst->h);
    VkRenderingAttachmentInfo za = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst->view,
        .imageLayout = dst->layout, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo sa = za;
    sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;    /* the stencil comes from the words' low byte */
    sa.clearValue.depthStencil.stencil = 0;
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { fw, fh } }, .layerCount = 1,
                           .pDepthAttachment = &za, .pStencilAttachment = stencil ? &sa : NULL };
    T.vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)fw, (float)fh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { fw, fh } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_zr.pl, 0, 1, &set, 0, NULL);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    if (stencil) {
        T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
        for (uint32_t bit = 0; bit < 8; bit++) {
            const uint32_t pcv[4] = { bit, 0, 0, 0 };
            T.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
            T.vkCmdPushConstants(cb, g_zr.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcv), pcv);
            T.vkCmdDraw(cb, 3, 1, 0, 0);
        }
    }
    T.vkCmdEndRendering(cb);
    return NULL;
}

const char *hle_zread_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst,
                           const uint8_t map[4])
{
    if (!init()) {
        return "setup failed";
    }
    VkPipeline p = pipe_for(dst->fmt);
    if (!p) {
        return "no pipeline";
    }
    if (!views(src)) {
        return "no depth views";
    }
    VkDescriptorSet set = slot_set(slot);
    if (!set) {
        return "too many this recording";
    }
    hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii[2] = { { g_zr.smp, src->zr_depth, src->layout },
                                    { g_zr.smp, src->zr_stencil, src->layout } };
    VkWriteDescriptorSet w[2];
    for (unsigned i = 0; i < 2; i++) {
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ii[i] };
    }
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, w, 0, NULL);
    hle_surf_transition(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const bool halves = map[0] == HLE_ZREAD_HALVES;
    uint32_t fw = MIN(src->w * (halves ? 2 : 1), dst->w), fh = MIN(src->h, dst->h);
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst->view,
        .imageLayout = dst->layout, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = { { 0, 0 }, { fw, fh } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)fw, (float)fh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { fw, fh } };
    uint32_t pc[4] = { map[0], halves ? dst->scale : map[1], map[2], map[3] };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    hle_draw_state_lost();
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_zr.pl, 0, 1, &set, 0, NULL);
    T.vkCmdPushConstants(cb, g_zr.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    T.vkCmdDraw(cb, 3, 1, 0, 0);
    T.vkCmdEndRendering(cb);
    hle_surf_transition(cb, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return NULL;
}

/* The core stops (back_int.h): the passes' objects destroyed (the depth
 * buffers' views go with their surfaces, surf.c), and init and the
 * pipelines made on first use as before the first conversion (a setup that
 * failed is tried again, on the next device). */
void hle_zread_shutdown(void)
{
    VkDevice d = g_hvk.dev;
    for (unsigned i = 0; i < g_zr.npipe; i++) {
        T.vkDestroyPipeline(d, g_zr.pipe[i].p, NULL);
    }
    for (unsigned i = 0; i < g_zr.nzw; i++) {
        T.vkDestroyPipeline(d, g_zr.zw[i].p, NULL);
    }
    T.vkDestroyShaderModule(d, g_zr.vs, NULL);
    T.vkDestroyShaderModule(d, g_zr.fs, NULL);
    T.vkDestroyShaderModule(d, g_zr.fs_zw, NULL);
    T.vkDestroyShaderModule(d, g_zr.fs_zws, NULL);
    T.vkDestroyDescriptorPool(d, g_zr.pool, NULL);      /* its sets with it */
    T.vkDestroySampler(d, g_zr.smp, NULL);
    T.vkDestroyPipelineLayout(d, g_zr.pl, NULL);
    T.vkDestroyDescriptorSetLayout(d, g_zr.dsl, NULL);
    memset(&g_zr, 0, sizeof(g_zr));
}
