/*
 * hakuX HLE GPU — the window's presenter (swapchain.h): the front frame
 * (display.h) drawn into the app's window through a Vulkan swapchain, once
 * per display refresh, on the window's thread (native/app/present.c).
 *
 * Each refresh: a swapchain image (waiting for one paces the thread to the
 * panel: FIFO), the front frame, and one pass that letterboxes the frame
 * into the image, the rest black, through the display's gamma ramp. The
 * anti-aliasing on the way:
 *  - FXAA over each new frame at its own size, into an image of ours, once a
 *    frame however many refreshes show it;
 *  - a frame larger than its place in the window (a render scale above the
 *    screen's) is averaged down, each window pixel the mean of the frame
 *    pixels it covers, where a bilinear sample would skip most of them and
 *    keep the edges' stairs;
 *  - a smaller one is scaled up bilinearly, or by Snapdragon Game Super
 *    Resolution 1, which rebuilds its edges.
 * FXAA is FXAA 3.11 Quality (Timothy Lottes) as PCSX2's fxaa.fx tunes it
 * (GPL-3.0-or-later); SGSR is Qualcomm's sgsr1_shader_mobile.frag
 * (BSD-3-Clause, licenses/snapdragon-gsr.license.txt).
 *
 * The queue is the render thread's too (hle_vk_queue_lock): queue order
 * puts our reads of a frame after its writes, and the render thread's next
 * write of the slot after our reads (its barriers). The device table is the
 * presenter's own: the render thread's wraps its commands for
 * strip batching (batch.c). The surface follows the window: a new one (on
 * Android, the activity's surface made again) gets a new surface and
 * swapchain, none drops them.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/compat.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/quirks.h"
#include "hw/xbox/hle/vk/swapchain.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#else
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#endif

#define T (g_ps.t)
#define PS_FRAMES 3             /* the presenter's submits in flight */
#define PS_IMAGES 8             /* swapchain images at most */
#define PS_RAMP 1024            /* a frame's share of the ramp buffer (768 used) */

enum { P_BILINEAR, P_AREA, P_SGSR, P_FXAA, P_COUNT };

typedef struct PsFrame {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkSemaphore acquired;       /* its swapchain image is ready */
    VkDescriptorSet set[2];     /* the window pass's, FXAA's */
    uint64_t value;             /* its last submit's on the timeline */
    bool ramp_stale;            /* its share of the ramp buffer is behind */
} PsFrame;

static struct {
    struct VolkDeviceTable t;
    bool inited, failed;
    VkSemaphore tl;             /* timeline: each submit's value */
    uint64_t submitted;
    VkSampler smp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dp;
    VkBuffer ubo;
    VkDeviceMemory ubo_mem;
    uint8_t *ubo_map;
    VkShaderModule vs, fs[P_COUNT];
    VkPipeline pipe[P_COUNT];
    VkFormat pipe_fmt;          /* the window pipelines' */
    PsFrame fr[PS_FRAMES];
    unsigned cur;
    /* the window */
    HleWindow *win;
    int64_t lost_ns;            /* its surface was lost then: tried again a while after */
    VkSurfaceKHR surf;
    VkSwapchainKHR sc;
    VkFormat fmt;
    VkExtent2D ext, wsize;      /* the images' (the panel's orientation), the window's */
    float rot[4];               /* the window's clip space into the images' (k_vs) */
    uint32_t nimg, idx;
    VkImage img[PS_IMAGES];
    VkImageView view[PS_IMAGES];
    VkSemaphore drawn[PS_IMAGES];   /* its pass done: what its present waits for */
    bool stale;                 /* out of date: made again at the next begin */
    bool follows;               /* its transform is the panel's: SUBOPTIMAL means it turned */
    /* the frame shown */
    HleDispFrame f;
    bool have, using;           /* f held; inside hle_disp_use_begin */
    uint64_t serial;            /* frames taken */
    /* FXAA's */
    VkImage fx_img;
    VkDeviceMemory fx_mem;
    VkImageView fx_view;
    uint32_t fx_w, fx_h;
    uint64_t fx_serial;         /* the frame it holds */
    /* the display's gamma ramp */
    uint8_t ramp[3][256];
    uint32_t ramp_gen;
    /* the last present went through, of f (have), the ramp and the caller's
     * key (hle_present_unchanged) */
    bool shown;
    uint64_t shown_key;
} g_ps;

/* ---- shaders ---- */

/* rect: x0, y0, x1, y1 in the window's clip space; rot: that space turned
 * into the image's (the panel's orientation: pre-rotated), x' = rot.x x +
 * rot.y y, y' = rot.z x + rot.w y */
static const char k_vs[] =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 rect; vec4 extra; vec4 rot; } pc;\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);\n"
    "  v_uv = c;\n"
    "  vec2 p = mix(pc.rect.xy, pc.rect.zw, c);\n"
    "  gl_Position = vec4(pc.rot.x * p.x + pc.rot.y * p.y, pc.rot.z * p.x + pc.rot.w * p.y, 0.0, 1.0);\n"
    "}\n";
/* every fragment shader's start: its inputs and the gamma ramp (768 bytes,
 * a channel's 256 after another's, as gamma.c's) */
static const char k_fs_head[] =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 rect; vec4 extra; vec4 rot; } pc;\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_tex;\n"
    "layout(set = 0, binding = 1, std140) uniform G { uvec4 w[48]; } g;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 o;\n"
    "uint lut(uint i) {\n"
    "  uint word = g.w[i >> 4][(i >> 2) & 3u];\n"
    "  return (word >> ((i & 3u) * 8u)) & 0xFFu;\n"
    "}\n"
    "vec4 ramp(vec3 c) {\n"
    "  uvec3 v = uvec3(clamp(c, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  return vec4(float(lut(v.r)), float(lut(256u + v.g)), float(lut(512u + v.b)), 255.0) / 255.0;\n"
    "}\n";
static const char k_fs_bilinear[] =
    "void main() {\n"
    "  o = ramp(texture(u_tex, v_uv).rgb);\n"
    "}\n";
/* Down: each window pixel covers extra.xy frame pixels (at most 6 a side
 * counted), each weighted by how much of it it covers: a box filter (on an
 * axis scaled up, 1 pixel wide: the same as bilinear). */
static const char k_fs_area[] =
    "void main() {\n"
    "  vec2 size = vec2(textureSize(u_tex, 0));\n"
    "  vec2 r = clamp(pc.extra.xy, vec2(1.0), vec2(6.0));\n"
    "  vec2 lo = v_uv * size - 0.5 * r, hi = lo + r;\n"
    "  ivec2 i0 = ivec2(floor(lo)), i1 = ivec2(ceil(hi)) - 1, top = ivec2(size) - 1;\n"
    "  vec3 sum = vec3(0.0);\n"
    "  float wsum = 0.0;\n"
    "  for (int y = i0.y; y <= i1.y; y++) {\n"
    "    float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));\n"
    "    for (int x = i0.x; x <= i1.x; x++) {\n"
    "      float w = (min(hi.x, float(x + 1)) - max(lo.x, float(x))) * wy;\n"
    "      sum += w * texelFetch(u_tex, clamp(ivec2(x, y), ivec2(0), top), 0).rgb;\n"
    "      wsum += w;\n"
    "    }\n"
    "  }\n"
    "  o = ramp(sum / wsum);\n"
    "}\n";
/* FXAA 3.11 Quality as PCSX2 tunes it (edges only: no subpixel blur), into
 * a frame-sized image: no ramp yet. */
static const char k_fs_fxaa[] =
    "const float EDGE_THRESHOLD = 0.063, EDGE_THRESHOLD_MIN = 0.0, SUBPIX = 0.0;\n"
    "const float STEP[12] = float[12](1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0);\n"
    "float luma(vec3 c) { return dot(c, vec3(0.2126729, 0.7151522, 0.0721750)); }\n"
    "float luma_at(vec2 p) { return luma(textureLod(u_tex, p, 0.0).rgb); }\n"
    "#define LUMA_OFF(x, y) luma(textureLodOffset(u_tex, posM, 0.0, ivec2(x, y)).rgb)\n"
    "void main() {\n"
    "  vec2 rcp = 1.0 / vec2(textureSize(u_tex, 0));\n"
    "  vec2 posM = v_uv;\n"
    "  vec3 rgbM = textureLod(u_tex, posM, 0.0).rgb;\n"
    "  float lumaM = luma(rgbM);\n"
    "  float lumaS = LUMA_OFF(0, 1), lumaE = LUMA_OFF(1, 0), lumaN = LUMA_OFF(0, -1), lumaW = LUMA_OFF(-1, 0);\n"
    "  float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));\n"
    "  float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));\n"
    "  float range = rangeMax - rangeMin;\n"
    "  if (range < max(EDGE_THRESHOLD_MIN, rangeMax * EDGE_THRESHOLD)) {\n"
    "    o = vec4(rgbM, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  float lumaNW = LUMA_OFF(-1, -1), lumaSE = LUMA_OFF(1, 1), lumaNE = LUMA_OFF(1, -1), lumaSW = LUMA_OFF(-1, 1);\n"
    "  float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;\n"
    "  float edgeHorz = abs(-2.0 * lumaW + lumaNW + lumaSW) + 2.0 * abs(-2.0 * lumaM + lumaNS) +\n"
    "                   abs(-2.0 * lumaE + lumaNE + lumaSE);\n"
    "  float edgeVert = abs(-2.0 * lumaS + lumaSW + lumaSE) + 2.0 * abs(-2.0 * lumaM + lumaWE) +\n"
    "                   abs(-2.0 * lumaN + lumaNW + lumaNE);\n"
    "  bool horzSpan = edgeHorz >= edgeVert;\n"
    "  float subpixA = 2.0 * (lumaNS + lumaWE) + (lumaNW + lumaSW + lumaNE + lumaSE);\n"
    "  float lengthSign = horzSpan ? rcp.y : rcp.x;\n"
    "  if (!horzSpan) {\n"
    "    lumaN = lumaW;\n"
    "    lumaS = lumaE;\n"
    "  }\n"
    "  float subpixB = subpixA * (1.0 / 12.0) - lumaM;\n"
    "  float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;\n"
    "  float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;\n"
    "  bool pairN = abs(gradientN) >= abs(gradientS);\n"
    "  float gradient = max(abs(gradientN), abs(gradientS));\n"
    "  if (pairN) lengthSign = -lengthSign;\n"
    "  float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);\n"
    "  vec2 offNP = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);\n"
    "  vec2 posB = posM;\n"
    "  if (horzSpan) posB.y += lengthSign * 0.5; else posB.x += lengthSign * 0.5;\n"
    "  vec2 posN = posB - offNP, posP = posB + offNP;\n"
    "  float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;\n"
    "  if (!pairN) lumaNN = lumaSS;\n"
    "  float gradientScaled = gradient * 0.25;\n"
    "  bool lumaMLTZero = lumaM - lumaNN * 0.5 < 0.0;\n"
    "  float lumaEndN = luma_at(posN) - lumaNN * 0.5, lumaEndP = luma_at(posP) - lumaNN * 0.5;\n"
    "  bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;\n"
    "  if (!doneN) posN -= offNP * STEP[0];\n"
    "  if (!doneP) posP += offNP * STEP[0];\n"
    "  for (int i = 1; i < 12 && !(doneN && doneP); i++) {\n"
    "    if (!doneN) lumaEndN = luma_at(posN) - lumaNN * 0.5;\n"
    "    if (!doneP) lumaEndP = luma_at(posP) - lumaNN * 0.5;\n"
    "    doneN = abs(lumaEndN) >= gradientScaled;\n"
    "    doneP = abs(lumaEndP) >= gradientScaled;\n"
    "    if (!doneN) posN -= offNP * STEP[i];\n"
    "    if (!doneP) posP += offNP * STEP[i];\n"
    "  }\n"
    "  float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;\n"
    "  float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;\n"
    "  bool goodSpan = dstN < dstP ? (lumaEndN < 0.0) != lumaMLTZero : (lumaEndP < 0.0) != lumaMLTZero;\n"
    "  float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;\n"
    "  float offset = max(pixelOffset, subpixF * subpixF * SUBPIX);\n"
    "  if (horzSpan) posM.y += offset * lengthSign; else posM.x += offset * lengthSign;\n"
    "  o = vec4(textureLod(u_tex, posM, 0.0).rgb, 1.0);\n"
    "}\n";
/* Snapdragon Game Super Resolution 1: Qualcomm's sgsr1_shader_mobile.frag
 * (RGBA mode: green as luma), with our input, output and ramp; ViewportInfo
 * is extra (1/w, 1/h, w, h of the frame). */
static const char k_fs_sgsr[] =
    "//====================================================================================\n"
    "//         Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.\n"
    "//                         SPDX-License-Identifier: BSD-3-Clause\n"
    "//====================================================================================\n"
    "#define OperationMode 1\n"
    "#define EdgeThreshold 8.0/255.0\n"
    "#define EdgeSharpness 2.0\n"
    "float fastLanczos2(float x) {\n"
    "  float wA = x - 4.0;\n"
    "  float wB = x * wA - wA;\n"
    "  wA *= wA;\n"
    "  return wB * wA;\n"
    "}\n"
    "vec2 weightY(float dx, float dy, float c, float std) {\n"
    "  float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
    "  float w = fastLanczos2(x);\n"
    "  return vec2(w, w * c);\n"
    "}\n"
    "void main() {\n"
    "  const int mode = OperationMode;\n"
    "  float edgeThreshold = EdgeThreshold;\n"
    "  float edgeSharpness = EdgeSharpness;\n"
    "  vec4 ViewportInfo = pc.extra;\n"
    "  vec4 color;\n"
    "  color.xyz = textureLod(u_tex, v_uv, 0.0).xyz;\n"
    "  vec2 imgCoord = ((v_uv * ViewportInfo.zw) + vec2(-0.5, 0.5));\n"
    "  vec2 imgCoordPixel = floor(imgCoord);\n"
    "  vec2 coord = (imgCoordPixel * ViewportInfo.xy);\n"
    "  vec2 pl = (imgCoord + (-imgCoordPixel));\n"
    "  vec4 left = textureGather(u_tex, coord, mode);\n"
    "  float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);\n"
    "  if (edgeVote > edgeThreshold) {\n"
    "    coord.x += ViewportInfo.x;\n"
    "    vec4 right = textureGather(u_tex, coord + vec2(ViewportInfo.x, 0.0), mode);\n"
    "    vec4 upDown;\n"
    "    upDown.xy = textureGather(u_tex, coord + vec2(0.0, -ViewportInfo.y), mode).wz;\n"
    "    upDown.zw = textureGather(u_tex, coord + vec2(0.0, ViewportInfo.y), mode).yx;\n"
    "    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
    "    left = left - vec4(mean);\n"
    "    right = right - vec4(mean);\n"
    "    upDown = upDown - vec4(mean);\n"
    "    color.w = color[mode] - mean;\n"
    "    float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +\n"
    "                  (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +\n"
    "                 (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));\n"
    "    float std = 2.181818 / sum;\n"
    "    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
    "    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
    "    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
    "    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
    "    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
    "    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
    "    aWY += weightY(pl.x, pl.y, left.z, std);\n"
    "    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
    "    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
    "    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
    "    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
    "    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
    "    float finalY = aWY.y / aWY.x;\n"
    "    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
    "    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
    "    finalY = clamp(edgeSharpness * finalY, minY, maxY);\n"
    "    float deltaY = finalY - color.w;\n"
    "    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);\n" /* smooth high contrast input */
    "    color.x = clamp((color.x + deltaY), 0.0, 1.0);\n"
    "    color.y = clamp((color.y + deltaY), 0.0, 1.0);\n"
    "    color.z = clamp((color.z + deltaY), 0.0, 1.0);\n"
    "  }\n"
    "  o = ramp(color.xyz);\n"
    "}\n";

static VkShaderModule module(HleShaderStage stage, const char *head, const char *body)
{
    g_autofree char *src = g_strdup_printf("%s%s", head, body);
    size_t n = 0;
    g_autofree uint32_t *spv = hle_glsl_compile(stage, src, &n);
    VkShaderModule m = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = n * 4, .pCode = spv };
    if (spv && T.vkCreateShaderModule(g_hvk.dev, &ci, NULL, &m) != VK_SUCCESS) {
        m = VK_NULL_HANDLE;
    }
    return m;
}

static VkPipeline pipeline(VkShaderModule fs, VkFormat fmt)
{
    if (!g_ps.vs || !fs) {
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_ps.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_ps.pl };
    VkPipeline p = VK_NULL_HANDLE;
    if (T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &p) != VK_SUCCESS) {
        p = VK_NULL_HANDLE;
    }
    return p;
}

/* ---- setup ---- */

static bool init(void)
{
    volkLoadDeviceTable(&g_ps.t, g_hvk.dev);
    hle_compat_install(&g_ps.t);        /* below 1.3: as the back end's table */
    VkSemaphoreTypeCreateInfo tci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
    VkSemaphoreCreateInfo tsi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    if (T.vkCreateSemaphore(g_hvk.dev, &tsi, NULL, &g_ps.tl) != VK_SUCCESS ||
        T.vkCreateSampler(g_hvk.dev, &sci, NULL, &g_ps.smp) != VK_SUCCESS ||
        T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_ps.dsl) != VK_SUCCESS) {
        return false;
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48 };
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &g_ps.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PS_FRAMES * 2 },
                                   { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, PS_FRAMES * 2 } };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = PS_FRAMES * 2,
        .poolSizeCount = 2, .pPoolSizes = ps };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PS_FRAMES * PS_RAMP,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
    if (T.vkCreatePipelineLayout(g_hvk.dev, &lci, NULL, &g_ps.pl) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &pci, NULL, &g_ps.dp) != VK_SUCCESS ||
        T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_ps.ubo) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_ps.ubo, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ps.ubo_mem) != VK_SUCCESS ||
        T.vkBindBufferMemory(g_hvk.dev, g_ps.ubo, g_ps.ubo_mem, 0) != VK_SUCCESS ||
        T.vkMapMemory(g_hvk.dev, g_ps.ubo_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_ps.ubo_map) != VK_SUCCESS) {
        return false;
    }
    for (unsigned i = 0; i < 256; i++) {
        g_ps.ramp[0][i] = g_ps.ramp[1][i] = g_ps.ramp[2][i] = (uint8_t)i;
    }
    for (unsigned i = 0; i < PS_FRAMES; i++) {
        PsFrame *f = &g_ps.fr[i];
        VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                        .queueFamilyIndex = g_hvk.qfam };
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkDescriptorSetLayout l[2] = { g_ps.dsl, g_ps.dsl };
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = g_ps.dp, .descriptorSetCount = 2,
                                            .pSetLayouts = l };
        if (T.vkCreateCommandPool(g_hvk.dev, &cpi, NULL, &f->pool) != VK_SUCCESS ||
            T.vkCreateSemaphore(g_hvk.dev, &si, NULL, &f->acquired) != VK_SUCCESS ||
            T.vkAllocateDescriptorSets(g_hvk.dev, &dai, f->set) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = f->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1 };
        if (T.vkAllocateCommandBuffers(g_hvk.dev, &ai, &f->cb) != VK_SUCCESS) {
            return false;
        }
        f->ramp_stale = true;
    }
    g_ps.vs = module(HLE_SHADER_VERTEX, k_vs, "");
    static const char *const body[P_COUNT] = { k_fs_bilinear, k_fs_area, k_fs_sgsr, k_fs_fxaa };
    for (unsigned i = 0; i < P_COUNT; i++) {
        g_ps.fs[i] = module(HLE_SHADER_FRAGMENT, k_fs_head, body[i]);
    }
    g_ps.pipe[P_FXAA] = pipeline(g_ps.fs[P_FXAA], VK_FORMAT_R8G8B8A8_UNORM);
    hle_log("hakuX-hle-back", "present: ready (shaders: bilinear %d, averaging %d, SGSR %d, FXAA %d)",
            g_ps.fs[P_BILINEAR] != VK_NULL_HANDLE, g_ps.fs[P_AREA] != VK_NULL_HANDLE,
            g_ps.fs[P_SGSR] != VK_NULL_HANDLE, g_ps.pipe[P_FXAA] != VK_NULL_HANDLE);
    return g_ps.fs[P_BILINEAR] != VK_NULL_HANDLE;
}

/* Every submit of the presenter's done (the timeline), and with the queue
 * idle its presents' waits too: what a swapchain or image being replaced
 * may still be used by. */
static void idle(void)
{
    if (hle_vk_lost()) {
        return;                         /* nothing runs on it any more */
    }
    VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                               .pSemaphores = &g_ps.tl, .pValues = &g_ps.submitted };
    VkResult r = T.vkWaitSemaphores(g_hvk.dev, &wi, 2000000000ull);
    if (r != VK_TIMEOUT) {
        hle_vk_check(r, "vkWaitSemaphores (present)");
    }
    hle_vk_queue_lock();
    r = T.vkQueueWaitIdle(g_hvk.queue);
    hle_vk_queue_unlock();
    hle_vk_check(r, "vkQueueWaitIdle (present)");
}

static void drop_images(void)
{
    for (uint32_t i = 0; i < g_ps.nimg; i++) {
        T.vkDestroyImageView(g_hvk.dev, g_ps.view[i], NULL);
        T.vkDestroySemaphore(g_hvk.dev, g_ps.drawn[i], NULL);
    }
    g_ps.nimg = 0;
}

/* The window gone, or another: its swapchain and surface go. */
static void drop_window(void)
{
    g_ps.shown = false;
    if (g_ps.sc || g_ps.surf) {
        idle();
    }
    drop_images();
    if (g_ps.sc) {
        T.vkDestroySwapchainKHR(g_hvk.dev, g_ps.sc, NULL);
        g_ps.sc = VK_NULL_HANDLE;
    }
    if (g_ps.surf) {
        vkDestroySurfaceKHR(g_hvk.inst, g_ps.surf, NULL);
        g_ps.surf = VK_NULL_HANDLE;
    }
    g_ps.win = NULL;
}

static bool make_swapchain(uint32_t w, uint32_t h)
{
    VkSurfaceCapabilitiesKHR caps;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_hvk.phys, g_hvk.qfam, g_ps.surf, &ok);
    if (!ok || vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_hvk.phys, g_ps.surf, &caps) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "present: the window's surface can't be presented to");
        return false;
    }
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_hvk.phys, g_ps.surf, &nf, fmts);
    /* plain 8-bit UNORM: the frame's values go out as they are (an sRGB
     * format would encode them again) */
    VkSurfaceFormatKHR fmt = nf ? fmts[0] : (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_UNORM, 0 };
    for (int pass = 0; pass < 2; pass++) {
        const VkFormat want = pass ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        for (uint32_t i = 0; i < nf; i++) {
            if (fmts[i].format == want && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                fmt = fmts[i];
                pass = 2;
                break;
            }
        }
    }
    uint32_t n = MAX(caps.minImageCount, 3u);
    if (caps.maxImageCount) {
        n = MIN(n, caps.maxImageCount);
    }
    n = MIN(n, (uint32_t)PS_IMAGES);
    /* Pre-rotated: the images in the panel's own orientation (its transform
     * now), the picture turned into them as it's drawn. An image in the
     * window's orientation the compositor turns itself, with the GPU at every
     * refresh (Odin 3 and RP5 panels are portrait): that composition took GPU
     * time from the game, and the Odin 3 fell to its 60 Hz mode with it.
     * Qualcomm's EGL had pre-rotated GL's buffers. */
    static const VkSurfaceTransformFlagBitsKHR k_turns[4] = {
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
        VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR, VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR };
    static const float k_rot[4][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 } };
    unsigned turn = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (caps.currentTransform == k_turns[i] && (caps.supportedTransforms & k_turns[i])) {
            turn = i;
        }
    }
    const VkSurfaceTransformFlagBitsKHR tf = k_turns[turn];
    g_ps.follows = tf == caps.currentTransform;
    memcpy(g_ps.rot, k_rot[turn], sizeof(g_ps.rot));
    g_ps.wsize = (VkExtent2D){ w, h };
    if (turn & 1) {
        const uint32_t t = w;   /* a quarter turn: the panel's width is the window's height */
        w = h;
        h = t;
    }
    w = MIN(MAX(w, caps.minImageExtent.width), caps.maxImageExtent.width);
    h = MIN(MAX(h, caps.minImageExtent.height), caps.maxImageExtent.height);
    if (hle_quirk(HLE_QUIRK_SWAP_W32) && (w & ~31u) >= MAX(caps.minImageExtent.width, 32u)) {
        w &= ~31u;                      /* older PowerVR drivers (quirks.h) */
    }
    VkCompositeAlphaFlagBitsKHR ca = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                         ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                         : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainKHR old = g_ps.sc;
    if (old) {
        idle();
    }
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_ps.surf,
        .minImageCount = n, .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = { w, h }, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = tf, .compositeAlpha = ca,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = T.vkCreateSwapchainKHR(g_hvk.dev, &ci, NULL, &sc);
    drop_images();
    if (old) {
        T.vkDestroySwapchainKHR(g_hvk.dev, old, NULL);
    }
    g_ps.sc = sc;
    if (r != VK_SUCCESS) {
        g_ps.sc = VK_NULL_HANDLE;
        hle_log("hakuX-hle-back", "present: vkCreateSwapchainKHR %ux%u: %d", w, h, r);
        return false;
    }
    uint32_t ni = PS_IMAGES;
    T.vkGetSwapchainImagesKHR(g_hvk.dev, sc, &ni, g_ps.img);
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ps.img[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &g_ps.view[i]);
        const VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        T.vkCreateSemaphore(g_hvk.dev, &sci, NULL, &g_ps.drawn[i]);
    }
    g_ps.nimg = ni;
    g_ps.ext = (VkExtent2D){ w, h };
    g_ps.stale = false;
    if (fmt.format != g_ps.pipe_fmt) {
        for (unsigned i = 0; i < P_FXAA; i++) {
            if (g_ps.pipe[i]) {
                T.vkDestroyPipeline(g_hvk.dev, g_ps.pipe[i], NULL);
            }
            g_ps.pipe[i] = pipeline(g_ps.fs[i], fmt.format);
        }
        g_ps.pipe_fmt = fmt.format;
    }
    g_ps.fmt = fmt.format;
    hle_log("hakuX-hle-back", "present: swapchain %ux%u (the window %ux%u, turned %u degrees), %u images, "
            "format %d", w, h, g_ps.wsize.width, g_ps.wsize.height, turn * 90, ni, fmt.format);
    return g_ps.pipe[P_BILINEAR] != VK_NULL_HANDLE;
}

/* ---- the window: an Android window, or an SDL window on Linux ---- */

static void win_size(HleWindow *win, uint32_t *w, uint32_t *h)
{
#ifdef __ANDROID__
    *w = (uint32_t)ANativeWindow_getWidth(win);
    *h = (uint32_t)ANativeWindow_getHeight(win);
#else
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(win, &iw, &ih);
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
#endif
}

static bool win_surface(HleWindow *win, VkSurfaceKHR *surf)
{
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = win };
    return vkCreateAndroidSurfaceKHR(g_hvk.inst, &sci, NULL, surf) == VK_SUCCESS;
#else
    if (!SDL_Vulkan_CreateSurface(win, g_hvk.inst, NULL, surf)) {
        hle_log("hakuX-hle-back", "present: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    return true;
#endif
}

/* ---- each refresh ---- */

static bool wait_value(uint64_t v)
{
    VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                               .pSemaphores = &g_ps.tl, .pValues = &v };
    const VkResult r = T.vkWaitSemaphores(g_hvk.dev, &wi, 1000000000ull);
    if (r == VK_ERROR_DEVICE_LOST) {
        hle_vk_lose(r, "vkWaitSemaphores (present)");
    }
    return r == VK_SUCCESS;
}

/* the presenter's calls and waits (statistics) */
static int64_t g_submit_ns, g_present_ns, g_wait_ns, g_acquire_ns;

static void lose_window(void)
{
    g_ps.lost_ns = hle_now_ns();
    drop_window();
}

bool hle_present_unchanged(HleWindow *win, uint64_t key)
{
    if (!g_ps.shown || !win || win != g_ps.win || g_ps.stale || !g_ps.sc || key != g_ps.shown_key ||
        hle_disp_gamma_seq() != g_ps.ramp_gen) {
        return false;
    }
    uint32_t ww, wh;
    win_size(win, &ww, &wh);
    if (ww != g_ps.wsize.width || wh != g_ps.wsize.height) {
        return false;
    }
    uint32_t gen, swap_id;
    const bool front = hle_disp_front(&gen, &swap_id);
    if (front != g_ps.have) {
        return false;
    }
    return !front || (gen == g_ps.f.gen && swap_id == g_ps.f.swap_id);
}

bool hle_present_begin(HleWindow *win, uint32_t *w, uint32_t *h)
{
    if (g_ps.failed || !hle_vk_ready() || hle_vk_lost()) {
        return false;
    }
    if (!g_ps.inited) {
        g_ps.inited = true;
        if (!g_hvk.caps.swapchain || !init()) {
            g_ps.failed = true;
            hle_log("hakuX-hle-back", "present: %s: nothing shown",
                    g_hvk.caps.swapchain ? "setup failed" : "no Vulkan swapchain on this device");
            return false;
        }
    }
    /* a lost surface's window may come back at the same address: a while
     * later, whatever window there is gets a new one */
    if (win != g_ps.win && (!win || hle_now_ns() - g_ps.lost_ns > 250000000ll)) {
        drop_window();
        if (win) {
            if (!win_surface(win, &g_ps.surf)) {
                g_ps.surf = VK_NULL_HANDLE;
                g_ps.lost_ns = hle_now_ns();
                hle_log("hakuX-hle-back", "present: no surface for the window");
            } else {
                g_ps.win = win;
            }
        }
    }
    if (!g_ps.surf) {
        return false;
    }
    uint32_t ww, wh;
    win_size(win, &ww, &wh);
    if ((!g_ps.sc || g_ps.stale || ww != g_ps.wsize.width || wh != g_ps.wsize.height) && !make_swapchain(ww, wh)) {
        return false;
    }
    PsFrame *f = &g_ps.fr[g_ps.cur];
    const int64_t tw0 = hle_now_ns();
    if (!wait_value(f->value)) {
        return false;
    }
    const int64_t ta0 = hle_now_ns();
    g_wait_ns += ta0 - tw0;
    VkResult r = T.vkAcquireNextImageKHR(g_hvk.dev, g_ps.sc, 100000000ull, f->acquired, VK_NULL_HANDLE,
                                         &g_ps.idx);
    g_acquire_ns += hle_now_ns() - ta0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_ps.stale = true;
        return false;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        lose_window();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (r != VK_TIMEOUT && r != VK_NOT_READY) {
            hle_vk_check(r, "vkAcquireNextImageKHR");
        }
        return false;                   /* timed out: the next refresh tries again */
    }
    if (r == VK_SUBOPTIMAL_KHR && g_ps.follows) {
        g_ps.stale = true;              /* the panel turned: this image, then another swapchain */
    }
    *w = g_ps.wsize.width;
    *h = g_ps.wsize.height;
    return true;
}

bool hle_present_frame(uint32_t *w, uint32_t *h)
{
    hle_disp_use_begin();               /* the slots' images stay until the submit */
    g_ps.using = true;
    if (g_ps.have && g_ps.f.gen != hle_disp_gen()) {
        g_ps.have = false;              /* the display was made again: its image is gone */
    }
    HleDispFrame f;
    if (hle_disp_acquire(&f) && (!g_ps.have || f.slot != g_ps.f.slot)) {
        /* a new frame: the one shown goes back (its reads are submitted, and
         * the render thread's next write of it comes after them) */
        if (g_ps.have) {
            hle_disp_release(g_ps.f.slot);
        }
        g_ps.f = f;
        g_ps.have = true;
        g_ps.serial++;
    }
    if (g_ps.have) {
        *w = g_ps.f.w;
        *h = g_ps.f.h;
    }
    return g_ps.have;
}

/* The FXAA image at the frame's size. */
static bool fxaa_target(uint32_t w, uint32_t h)
{
    if (g_ps.fx_img && g_ps.fx_w == w && g_ps.fx_h == h) {
        return true;
    }
    if (g_ps.fx_img) {
        wait_value(g_ps.submitted);
        T.vkDestroyImageView(g_hvk.dev, g_ps.fx_view, NULL);
        T.vkDestroyImage(g_hvk.dev, g_ps.fx_img, NULL);
        T.vkFreeMemory(g_hvk.dev, g_ps.fx_mem, NULL);
        g_ps.fx_img = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &g_ps.fx_img) != VK_SUCCESS) {
        g_ps.fx_img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetImageMemoryRequirements(g_hvk.dev, g_ps.fx_img, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ps.fx_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ps.fx_mem) != VK_SUCCESS ||
        T.vkBindImageMemory(g_hvk.dev, g_ps.fx_img, g_ps.fx_mem, 0) != VK_SUCCESS ||
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &g_ps.fx_view) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "present: no %ux%u FXAA image: FXAA off", w, h);
        T.vkDestroyImage(g_hvk.dev, g_ps.fx_img, NULL);
        g_ps.fx_img = VK_NULL_HANDLE;
        g_ps.pipe[P_FXAA] = VK_NULL_HANDLE;
        return false;
    }
    g_ps.fx_w = w;
    g_ps.fx_h = h;
    g_ps.fx_serial = 0;
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = src,
        .srcAccessMask = src_access, .dstStageMask = dst, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &b };
    T.vkCmdPipelineBarrier2(cb, &di);
}

/* A quad of pipeline p over rect (clip space) into the rendering open. */
static void quad(VkCommandBuffer cb, PsFrame *f, unsigned set, VkPipeline p, VkImageView src,
                 const float pc[12], uint32_t w, uint32_t h)
{
    VkDescriptorImageInfo ii = { g_ps.smp, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo bi = { g_ps.ubo, (VkDeviceSize)(f - g_ps.fr) * PS_RAMP, 768 };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set[set], .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set[set], .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi } };
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, wr, 0, NULL);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    T.vkCmdSetViewport(cb, 0, 1, &vp);
    T.vkCmdSetScissor(cb, 0, 1, &sc);
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ps.pl, 0, 1, &f->set[set], 0, NULL);
    T.vkCmdPushConstants(cb, g_ps.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    T.vkCmdDraw(cb, 4, 1, 0, 0);
}

static void rendering(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h, bool clear)
{
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { { 0, 0, 0, 1 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } },
                           .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    T.vkCmdBeginRendering(cb, &ri);
}

/* How the frame meets the window, logged when that changes. */
static void log_path(unsigned p, bool fxaa, uint32_t vw, uint32_t vh)
{
    static unsigned s_p = ~0u;
    static bool s_fxaa;
    static uint32_t s_w, s_h, s_vw, s_vh;
    if (p == s_p && fxaa == s_fxaa && g_ps.f.w == s_w && g_ps.f.h == s_h && vw == s_vw && vh == s_vh) {
        return;
    }
    s_p = p;
    s_fxaa = fxaa;
    s_w = g_ps.f.w;
    s_h = g_ps.f.h;
    s_vw = vw;
    s_vh = vh;
    hle_log("hakuX-hle-back", "present: %ux%u%s into %ux%u: %s", s_w, s_h, fxaa ? " after FXAA" : "", vw, vh,
            p == P_AREA ? "averaged down" : p == P_SGSR ? "scaled up by SGSR" : "bilinear");
}

void hle_present_end(float sx, float sy, bool fxaa, int upscale, bool average, uint64_t key)
{
    g_ps.shown = false;
    PsFrame *f = &g_ps.fr[g_ps.cur];
    const uint32_t ww = g_ps.wsize.width, wh = g_ps.wsize.height, iw = g_ps.ext.width, ih = g_ps.ext.height;
    uint8_t rgb[3][256];
    bool identity;
    if (hle_disp_gamma(rgb, &identity, &g_ps.ramp_gen)) {
        for (unsigned i = 0; i < 256; i++) {
            for (unsigned c = 0; c < 3; c++) {
                g_ps.ramp[c][i] = identity ? (uint8_t)i : rgb[c][i];
            }
        }
        for (unsigned i = 0; i < PS_FRAMES; i++) {
            g_ps.fr[i].ramp_stale = true;  /* a new ramp shows now, with no new frame */
        }
    }
    if (f->ramp_stale) {
        memcpy(g_ps.ubo_map + (size_t)g_ps.cur * PS_RAMP, g_ps.ramp, 768);
        f->ramp_stale = false;
    }
    T.vkResetCommandPool(g_hvk.dev, f->pool, 0);
    VkCommandBuffer cb = f->cb;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    T.vkBeginCommandBuffer(cb, &bi);
    VkImageView src = g_ps.have ? g_ps.f.view : VK_NULL_HANDLE;
    bool fxaa_done = false;
    /* The frame's writes need no barrier here: each writer of a display
     * image (the swap's blit, the gamma pass, the overlay) ends with one
     * into SHADER_READ_ONLY over all later commands of the queue, ours too
     * (a second queue: the CPU saw the frame complete before it was
     * presented). A barrier of our own drained the GPU at every refresh; a
     * wait on the render timeline at the frame's value sent the submit down
     * a slow path on Qualcomm's driver (Halo 2 at 30 fps, Adreno 830). */
    if (g_ps.have) {
        if (fxaa && g_ps.pipe[P_FXAA] && fxaa_target(g_ps.f.w, g_ps.f.h)) {
            if (g_ps.fx_serial != g_ps.serial) {
                barrier(cb, g_ps.fx_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                rendering(cb, g_ps.fx_view, g_ps.f.w, g_ps.f.h, false);
                static const float whole[12] = { -1.0f, -1.0f, 1.0f, 1.0f, 0, 0, 0, 0, 1.0f, 0, 0, 1.0f };
                quad(cb, f, 1, g_ps.pipe[P_FXAA], g_ps.f.view, whole, g_ps.f.w, g_ps.f.h);
                T.vkCmdEndRendering(cb);
                barrier(cb, g_ps.fx_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                g_ps.fx_serial = g_ps.serial;
            }
            src = g_ps.fx_view;
            fxaa_done = true;
        }
    }
    VkImage dst = g_ps.img[g_ps.idx];
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    rendering(cb, g_ps.view[g_ps.idx], iw, ih, true);
    if (g_ps.have) {
        /* the frame's pixels per window pixel: over 1 (either axis) averaged
         * down, under 1 scaled up as chosen */
        const float vw = sx * (float)ww, vh = sy * (float)wh;
        const float rx = (float)g_ps.f.w / vw, ry = (float)g_ps.f.h / vh;
        unsigned p = P_BILINEAR;
        float pc[12] = { -sx, -sy, sx, sy, 0.0f, 0.0f, 0.0f, 0.0f, g_ps.rot[0], g_ps.rot[1], g_ps.rot[2], g_ps.rot[3] };
        if ((rx > 1.01f || ry > 1.01f) && average && g_ps.pipe[P_AREA]) {
            p = P_AREA;
            pc[4] = rx;
            pc[5] = ry;
        } else if ((rx < 0.99f || ry < 0.99f) && upscale == HLE_UPSCALE_SGSR && g_ps.pipe[P_SGSR]) {
            p = P_SGSR;
            pc[4] = 1.0f / (float)g_ps.f.w;
            pc[5] = 1.0f / (float)g_ps.f.h;
            pc[6] = (float)g_ps.f.w;
            pc[7] = (float)g_ps.f.h;
        }
        quad(cb, f, 0, g_ps.pipe[p], src, pc, iw, ih);
        log_path(p, fxaa_done, (uint32_t)(vw + 0.5f), (uint32_t)(vh + 0.5f));
    }
    T.vkCmdEndRendering(cb);
    barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0);
    T.vkEndCommandBuffer(cb);

    /* The present waits for the pass on the GPU: Turnip's hands the
     * compositor a fence over it (Mesa's vk_android.c: a submit and a sync
     * file), and the window's thread goes on to the next refresh. Waiting
     * for the pass here, behind the game's frames on the one queue, held
     * the thread past refreshes whenever the GPU was busy, and the next
     * front frame was replaced before it was picked up ("UI missed").
     * HLE_QUIRK_PRESENT_CPU_WAIT: the image is presented once it's drawn,
     * with nothing left to wait for. Presenting with a semaphore to wait on
     * made Android's present ask Qualcomm's driver for a fence over it,
     * which waited (3-6 ms a refresh) with the queue's lock held, the render
     * thread's submits waiting on it; Halo 2 lost 2% of its frames to GL's
     * presenter. The wait is ours there, unlocked. */
    const bool cpu_wait = hle_quirk(HLE_QUIRK_PRESENT_CPU_WAIT);
    const uint64_t value = g_ps.submitted + 1;
    VkSemaphoreSubmitInfo wait = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = f->acquired,
                                   .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphoreSubmitInfo sig[2] = {
        { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_ps.tl, .value = value,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
        { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_ps.drawn[g_ps.idx],
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT } };
    VkCommandBufferSubmitInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb };
    VkSubmitInfo2 si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1,
                         .pWaitSemaphoreInfos = &wait, .commandBufferInfoCount = 1,
                         .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = cpu_wait ? 1 : 2,
                         .pSignalSemaphoreInfos = sig };
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = cpu_wait ? 0 : 1,
                            .pWaitSemaphores = &g_ps.drawn[g_ps.idx], .swapchainCount = 1,
                            .pSwapchains = &g_ps.sc, .pImageIndices = &g_ps.idx };
    hle_vk_queue_lock();
    const int64_t ts0 = hle_now_ns();
    VkResult r = T.vkQueueSubmit2(g_hvk.queue, 1, &si, VK_NULL_HANDLE);
    g_submit_ns += hle_now_ns() - ts0;
    hle_vk_queue_unlock();
    if (r == VK_SUCCESS) {
        g_ps.submitted = value;
        f->value = value;
    } else {
        hle_vk_check(r, "vkQueueSubmit2 (present)");
    }
    if (g_ps.using) {
        g_ps.using = false;
        hle_disp_use_end();
    }
    VkResult pr = r;
    if (r == VK_SUCCESS) {
        if (cpu_wait) {
            const int64_t tw0 = hle_now_ns();
            wait_value(value);          /* (timed out: presented all the same, or the image stays ours) */
            g_wait_ns += hle_now_ns() - tw0;
        }
        hle_vk_queue_lock();
        const int64_t tp0 = hle_now_ns();
        pr = T.vkQueuePresentKHR(g_hvk.queue, &pi);
        g_present_ns += hle_now_ns() - tp0;
        hle_vk_queue_unlock();
    }
    if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) {
        g_ps.shown = true;
        g_ps.shown_key = key;
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || (pr == VK_SUBOPTIMAL_KHR && g_ps.follows)) {
        g_ps.stale = true;
    } else if (pr == VK_ERROR_SURFACE_LOST_KHR) {
        lose_window();
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        hle_vk_lose(pr, "vkQueuePresentKHR");
    }
    g_ps.cur = (g_ps.cur + 1) % PS_FRAMES;
    static int64_t s_t0;
    const int64_t now = hle_now_ns();
    if (!s_t0) {
        s_t0 = now;
    } else if (now - s_t0 >= 10000000000ll) {
        const double s = (now - s_t0) / 1e9;
        hle_log("hakuX-hle-back", "present: the queue's lock waited for %.1f ms/s; the presenter's submits "
                "held it %.1f ms/s, its presents %.1f; it waited %.1f ms/s for its passes (%s), %.1f for images",
                hle_vk_queue_wait_ns() / 1e6 / s, g_submit_ns / 1e6 / s, g_present_ns / 1e6 / s,
                g_wait_ns / 1e6 / s,
                hle_quirk(HLE_QUIRK_PRESENT_CPU_WAIT) ? "each before its present" : "the present waits on the GPU",
                g_acquire_ns / 1e6 / s);
        g_submit_ns = g_present_ns = g_wait_ns = g_acquire_ns = 0;
        s_t0 = now;
    }
}
