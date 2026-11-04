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
 *  - a smaller one is scaled up bilinearly, or by AMD FidelityFX Super
 *    Resolution 1: EASU rebuilds its edges into an image of ours at the
 *    size it takes in the window, once a frame, and RCAS sharpens that on
 *    its way into the window.
 * FXAA is FXAA 3.11 Quality (Timothy Lottes) as PCSX2's fxaa.fx tunes it
 * (GPL-3.0-or-later); FSR 1 is AMD's ffx_fsr1.h (MIT,
 * licenses/amd-fsr1.license.txt).
 *
 * The queue is the render thread's too (hle_vk_queue_lock): queue order
 * puts our reads of a frame after its writes, and the render thread's next
 * write of the slot after our reads (its barriers). The device table is the
 * presenter's own: the render thread's wraps its commands for
 * strip batching (batch.c). The surface follows the window: a new
 * ANativeWindow (the activity's surface made again) gets a new surface and
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

#include "hw/xbox/hle/vk/native_window.h"
#if defined(NC_DESKTOP) && !defined(_WIN32)
#include <SDL3/SDL_vulkan.h>
#endif

#define T (g_ps.t)
#define PS_FRAMES 3             /* the presenter's submits in flight */
#define PS_RAMP 1024            /* a frame's share of the ramp buffer (768 used) */

/* the window's pipelines, then those into images of ours (R8G8B8A8) */
enum { P_BILINEAR, P_AREA, P_RCAS, P_CRT, P_OSD, P_FXAA, P_EASU, P_COUNT };

/* An image of ours a pass draws into and the window's pass samples. */
typedef struct PsImg {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t w, h;
    uint64_t serial;            /* the frame it holds (0: none) */
    bool fxaa;                  /* FSR's: made from FXAA's */
} PsImg;

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
    struct ANativeWindow *win;
    int64_t lost_ns;            /* its surface was lost then: tried again a while after */
    VkSurfaceKHR surf;
    VkSwapchainKHR sc;
    VkFormat fmt;
    VkExtent2D ext, wsize;      /* the images' (the panel's orientation), the window's */
    float rot[4];               /* the window's clip space into the images' (k_vs) */
    uint32_t nimg, idx;
    VkImage *img;               /* nimg each */
    VkImageView *view;
    VkSemaphore *drawn;         /* its pass done: what its present waits for */
    bool stale;                 /* out of date: made again at the next begin */
    bool follows;               /* its transform is the panel's: SUBOPTIMAL means it turned */
    /* the frame shown */
    HleDispFrame f;
    bool have, using;           /* f held; inside hle_disp_use_begin */
    uint64_t serial;            /* frames taken */
    /* FXAA's image (the frame's size), FSR's (the frame's in the window) */
    PsImg fx, up;
    /* the display's gamma ramp */
    uint8_t ramp[3][256];
    uint32_t ramp_gen;
    /* the last present went through, of f (have), the ramp and the caller's
     * key (hle_present_unchanged) */
    bool shown;
    uint64_t shown_key;
    /* a picture of the next refresh (hle_present_shot): its image copied
     * into buf after the pass */
    char shot_path[512];
    bool shot_want, shot_can;   /* asked; the images can be copied from */
    VkBuffer shot_buf;
    VkDeviceMemory shot_mem;
    VkDeviceSize shot_size;
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
/* A CRT's look (hle_present_set_crt): the frame's lines as scanlines (a
 * gaussian beam across each source line, wider where brighter), a phosphor
 * mask on the window's pixels, the tube's curve and its darker corners.
 * extra.x: the kind (1 soft scanlines, 2 a TV, 3 an arcade monitor),
 * extra.y: the render scale (the console's lines are the frame's / it).
 * Lines under ~2 window pixels each fade to flat: finer, they'd beat with
 * the pixel rows into waves where the tube curves. */
static const char k_fs_crt[] =
    "void main() {\n"
    "  int kind = int(pc.extra.x + 0.5);\n"
    "  float scan = kind == 1 ? 0.30 : kind == 2 ? 0.50 : 0.65;\n"
    "  float maskk = kind == 1 ? 0.0 : kind == 2 ? 0.12 : 0.22;\n"
    "  float curve = kind == 1 ? 0.0 : kind == 2 ? 0.035 : 0.06;\n"
    "  vec2 uv = v_uv;\n"
    "  vec2 cc = uv * 2.0 - 1.0;\n"
    "  cc *= 1.0 + curve * dot(cc, cc) * vec2(1.0, 1.25);\n"
    "  uv = cc * 0.5 + 0.5;\n"
    "  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "  vec2 size = vec2(textureSize(u_tex, 0)) / vec2(1.0, max(pc.extra.y, 1.0));\n"
    "  float y = uv.y * size.y - 0.5;\n"
    "  scan *= smoothstep(1.2, 2.2, 1.0 / max(fwidth(y), 1e-4));\n"
    "  float fy = fract(y);\n"
    "  vec2 at0 = vec2(uv.x, (floor(y) + 0.5) / size.y), at1 = at0 + vec2(0.0, 1.0 / size.y);\n"
    "  vec3 c0 = texture(u_tex, at0).rgb, c1 = texture(u_tex, at1).rgb;\n"
    "  /* the beam: brighter lines are wider */\n"
    "  vec3 w0 = exp(-pow(fy / (0.32 + 0.22 * c0), vec3(2.0)) * 2.0);\n"
    "  vec3 w1 = exp(-pow((1.0 - fy) / (0.32 + 0.22 * c1), vec3(2.0)) * 2.0);\n"
    "  vec3 lit = c0 * w0 + c1 * w1;\n"
    "  vec3 flat_ = texture(u_tex, uv).rgb;\n"
    "  vec3 col = mix(flat_, lit * 1.35, scan);\n"
    "  /* the mask: an aperture grille's stripes, R G B */\n"
    "  int m = int(gl_FragCoord.x) % 3;\n"
    "  vec3 mask = vec3(m == 0 ? 1.0 : 1.0 - maskk, m == 1 ? 1.0 : 1.0 - maskk, m == 2 ? 1.0 : 1.0 - maskk);\n"
    "  col *= mask * (1.0 + maskk * 0.9);\n"
    "  /* the corners */\n"
    "  float vig = kind == 1 ? 1.0 : clamp(pow(16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y), 0.18), 0.0, 1.0);\n"
    "  o = ramp(col * vig);\n"
    "}\n";
/* The frame rate in a corner (hle_present_set_osd): four seven-segment
 * digits, green on dark, drawn by the shader alone. extra.x: the number. */
static const char k_fs_osd[] =
    "float seg(vec2 p, int m) {\n"
    "  const float t = 0.07;\n"
    "  bool h = p.x > 0.22 && p.x < 0.78, u = p.y > 0.12 && p.y < 0.5, d = p.y > 0.5 && p.y < 0.88;\n"
    "  if ((m & 1) != 0 && abs(p.y - 0.12) < t && h) return 1.0;\n"
    "  if ((m & 2) != 0 && abs(p.x - 0.78) < t && u) return 1.0;\n"
    "  if ((m & 4) != 0 && abs(p.x - 0.78) < t && d) return 1.0;\n"
    "  if ((m & 8) != 0 && abs(p.y - 0.88) < t && h) return 1.0;\n"
    "  if ((m & 16) != 0 && abs(p.x - 0.22) < t && d) return 1.0;\n"
    "  if ((m & 32) != 0 && abs(p.x - 0.22) < t && u) return 1.0;\n"
    "  if ((m & 64) != 0 && abs(p.y - 0.5) < t && h) return 1.0;\n"
    "  return 0.0;\n"
    "}\n"
    "void main() {\n"
    "  const int SEG[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);\n"
    "  int n = int(pc.extra.x + 0.5);\n"
    "  int cell = int(v_uv.x * 4.0);\n"
    "  int pw = 1;\n"
    "  for (int i = 0; i < 3 - cell; i++) pw *= 10;\n"
    "  int digit = (n / pw) % 10;\n"
    "  bool lead = n < pw && cell < 3;\n"
    "  float on = lead ? 0.0 : seg(vec2(fract(v_uv.x * 4.0), v_uv.y), SEG[digit]);\n"
    "  o = mix(vec4(0.02, 0.07, 0.01, 1.0), vec4(0.55, 0.95, 0.16, 1.0), on);\n"
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
/* AMD FidelityFX Super Resolution 1 (ffx_fsr1.h, its 32-bit FsrEasuF and
 * FsrRcasF), MIT: licenses/amd-fsr1.license.txt. EASU draws into an image
 * of the frame's size in the window: extra = the frame's width and height,
 * the image's. Its reciprocal approximations are exact ones here, kept from
 * dividing by zero. */
static const char k_fs_easu[] =
    "// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. (MIT)\n"
    "void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD,\n"
    "             float lE) {\n"
    "  float lenX = 1.0 / max(max(abs(lD - lC), abs(lC - lB)), 1e-8);\n"
    "  float dirX = lD - lB;\n"
    "  dir.x += dirX * w;\n"
    "  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);\n"
    "  len += lenX * lenX * w;\n"
    "  float lenY = 1.0 / max(max(abs(lE - lC), abs(lC - lA)), 1e-8);\n"
    "  float dirY = lE - lA;\n"
    "  dir.y += dirY * w;\n"
    "  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);\n"
    "  len += lenY * lenY * w;\n"
    "}\n"
    "void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp,\n"
    "             vec3 c) {\n"
    "  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wB = 2.0 / 5.0 * d2 - 1.0;\n"
    "  float wA = lob * d2 - 1.0;\n"
    "  wB *= wB;\n"
    "  wA *= wA;\n"
    "  wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);\n"
    "  float w = wB * wA;\n"
    "  aC += c * w;\n"
    "  aW += w;\n"
    "}\n"
    "void main() {\n"
    "  vec2 inSz = pc.extra.xy, outSz = pc.extra.zw, rin = 1.0 / inSz;\n"
    "  vec2 pp = floor(gl_FragCoord.xy) * (inSz / outSz) + (0.5 * inSz / outSz - 0.5);\n"
    "  vec2 fp = floor(pp);\n"
    "  pp -= fp;\n"
    "  vec2 p0 = fp * rin + vec2(1.0, -1.0) * rin;\n"
    "  vec2 p1 = p0 + vec2(-1.0, 2.0) * rin;\n"
    "  vec2 p2 = p0 + vec2(1.0, 2.0) * rin;\n"
    "  vec2 p3 = p0 + vec2(0.0, 4.0) * rin;\n"
    "  vec4 bczzR = textureGather(u_tex, p0, 0), bczzG = textureGather(u_tex, p0, 1),\n"
    "       bczzB = textureGather(u_tex, p0, 2);\n"
    "  vec4 ijfeR = textureGather(u_tex, p1, 0), ijfeG = textureGather(u_tex, p1, 1),\n"
    "       ijfeB = textureGather(u_tex, p1, 2);\n"
    "  vec4 klhgR = textureGather(u_tex, p2, 0), klhgG = textureGather(u_tex, p2, 1),\n"
    "       klhgB = textureGather(u_tex, p2, 2);\n"
    "  vec4 zzonR = textureGather(u_tex, p3, 0), zzonG = textureGather(u_tex, p3, 1),\n"
    "       zzonB = textureGather(u_tex, p3, 2);\n"
    "  vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);\n"
    "  vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);\n"
    "  vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);\n"
    "  vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);\n"
    "  float bL = bczzL.x, cL = bczzL.y, iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;\n"
    "  float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w, oL = zzonL.z, nL = zzonL.w;\n"
    "  vec2 dir = vec2(0.0);\n"
    "  float len = 0.0;\n"
    "  easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "  easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
    "  easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
    "  easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "  vec2 dir2 = dir * dir;\n"
    "  float dirR = dir2.x + dir2.y;\n"
    "  bool zro = dirR < 1.0 / 32768.0;\n"
    "  dirR = zro ? 1.0 : inversesqrt(dirR);\n"
    "  dir.x = zro ? 1.0 : dir.x;\n"
    "  dir *= dirR;\n"
    "  len = len * 0.5;\n"
    "  len *= len;\n"
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
    "  float clp = 1.0 / lob;\n"
    "  vec3 f = vec3(ijfeR.z, ijfeG.z, ijfeB.z), g = vec3(klhgR.w, klhgG.w, klhgB.w);\n"
    "  vec3 j = vec3(ijfeR.y, ijfeG.y, ijfeB.y), k = vec3(klhgR.x, klhgG.x, klhgB.x);\n"
    "  vec3 min4 = min(min(f, g), min(j, k)), max4 = max(max(f, g), max(j, k));\n"
    "  vec3 aC = vec3(0.0);\n"
    "  float aW = 0.0;\n"
    "  easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));\n"
    "  easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));\n"
    "  easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));\n"
    "  easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);\n"
    "  easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);\n"
    "  easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));\n"
    "  easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);\n"
    "  easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));\n"
    "  easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));\n"
    "  easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);\n"
    "  easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));\n"
    "  easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));\n"
    "  o = vec4(min(max4, max(min4, aC / aW)), 1.0);\n"
    "}\n";
/* RCAS, over EASU's image on its way into the window (its pixels the
 * window's, one for one): extra.x the sharpness, 2^-stops (FsrRcasCon). */
static const char k_fs_rcas[] =
    "// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. (MIT)\n"
    "vec3 at(ivec2 p, ivec2 top) { return texelFetch(u_tex, clamp(p, ivec2(0), top), 0).rgb; }\n"
    "void main() {\n"
    "  ivec2 top = textureSize(u_tex, 0) - 1;\n"
    "  ivec2 sp = clamp(ivec2(v_uv * vec2(top + 1)), ivec2(0), top);\n"
    "  vec3 b = at(sp + ivec2(0, -1), top), d = at(sp + ivec2(-1, 0), top), e = at(sp, top);\n"
    "  vec3 f = at(sp + ivec2(1, 0), top), h = at(sp + ivec2(0, 1), top);\n"
    "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-6));\n"
    "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-6));\n"
    "  vec3 lobeC = max(-hitMin, hitMax);\n"
    "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeC.r, max(lobeC.g, lobeC.b)), 0.0)) * pc.extra.x;\n"
    "  o = ramp((lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0));\n"
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
    static const char *const body[P_COUNT] = { k_fs_bilinear, k_fs_area, k_fs_rcas, k_fs_crt, k_fs_osd, k_fs_fxaa,
                                               k_fs_easu };
    for (unsigned i = 0; i < P_COUNT; i++) {
        g_ps.fs[i] = module(HLE_SHADER_FRAGMENT, k_fs_head, body[i]);
    }
    g_ps.pipe[P_FXAA] = pipeline(g_ps.fs[P_FXAA], VK_FORMAT_R8G8B8A8_UNORM);
    g_ps.pipe[P_EASU] = pipeline(g_ps.fs[P_EASU], VK_FORMAT_R8G8B8A8_UNORM);
    hle_log("hakuX-hle-back", "present: ready (shaders: bilinear %d, averaging %d, FSR %d, FXAA %d)",
            g_ps.fs[P_BILINEAR] != VK_NULL_HANDLE, g_ps.fs[P_AREA] != VK_NULL_HANDLE,
            g_ps.fs[P_RCAS] != VK_NULL_HANDLE && g_ps.pipe[P_EASU] != VK_NULL_HANDLE,
            g_ps.pipe[P_FXAA] != VK_NULL_HANDLE);
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
    VkResult r = hle_tl_sem_wait(g_ps.tl, g_ps.submitted, 2000000000ull);
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
    g_free(g_ps.img);
    g_free(g_ps.view);
    g_free(g_ps.drawn);
    g_ps.img = NULL;
    g_ps.view = NULL;
    g_ps.drawn = NULL;
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
    /* Android may make more images than asked for (10 for 6 on a Dimensity
     * 8300), so the arrays are sized from what the swapchain reports. */
    uint32_t n = MAX(caps.minImageCount, 3u);
    if (caps.maxImageCount) {
        n = MIN(n, caps.maxImageCount);
    }
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
    g_ps.shot_can = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_ps.surf,
        .minImageCount = n, .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = { w, h }, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (g_ps.shot_can ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
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
    uint32_t ni = 0;
    VkResult ri = T.vkGetSwapchainImagesKHR(g_hvk.dev, sc, &ni, NULL);
    VkImage *imgs = ri == VK_SUCCESS && ni ? g_new0(VkImage, ni) : NULL;
    if (imgs) {
        ri = T.vkGetSwapchainImagesKHR(g_hvk.dev, sc, &ni, imgs);
    }
    if (!imgs || ri != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "present: the swapchain's %u images: %d", ni, ri);
        g_free(imgs);
        T.vkDestroySwapchainKHR(g_hvk.dev, sc, NULL);
        g_ps.sc = VK_NULL_HANDLE;
        return false;
    }
    g_ps.img = imgs;
    g_ps.view = g_new0(VkImageView, ni);
    g_ps.drawn = g_new0(VkSemaphore, ni);
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

/* ---- each refresh ---- */

static bool wait_value(uint64_t v)
{
    const VkResult r = hle_tl_sem_wait(g_ps.tl, v, 1000000000ull);
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

static int g_crt;   /* the CRT filter's kind, 0 none */
static float g_osd; /* the frame rate shown, 0 none */

void hle_present_set_osd(float fps)
{
    g_osd = fps;
}

void hle_present_set_crt(int kind)
{
    g_crt = kind < 0 ? 0 : kind > 3 ? 3 : kind;
}

void hle_present_shot(const char *path)
{
    snprintf(g_ps.shot_path, sizeof(g_ps.shot_path), "%s", path);
    g_ps.shot_want = true;
}

/* The buffer for a picture of size bytes, host-visible. */
static bool shot_buf(VkDeviceSize size)
{
    if (g_ps.shot_buf && g_ps.shot_size >= size) {
        return true;
    }
    if (g_ps.shot_buf) {
        T.vkDestroyBuffer(g_hvk.dev, g_ps.shot_buf, NULL);
        T.vkFreeMemory(g_hvk.dev, g_ps.shot_mem, NULL);
        g_ps.shot_buf = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_ps.shot_buf) != VK_SUCCESS) {
        g_ps.shot_buf = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_ps.shot_buf, &mr);
    const int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ps.shot_mem) != VK_SUCCESS) {
        T.vkDestroyBuffer(g_hvk.dev, g_ps.shot_buf, NULL);
        g_ps.shot_buf = VK_NULL_HANDLE;
        return false;
    }
    T.vkBindBufferMemory(g_hvk.dev, g_ps.shot_buf, g_ps.shot_mem, 0);
    g_ps.shot_size = size;
    return true;
}

/* The picture copied out (the pass done) as a 32-bit top-down BMP. */
static void shot_write(uint32_t w, uint32_t h)
{
    void *map = NULL;
    if (T.vkMapMemory(g_hvk.dev, g_ps.shot_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        return;
    }
    FILE *f = fopen(g_ps.shot_path, "wb");
    if (f) {
        const uint32_t bytes = w * h * 4;
        uint8_t hd[54] = { 'B', 'M' };
        const uint32_t v[] = { 54 + bytes, 0, 54, 40, w, (uint32_t)-(int32_t)h };
        memcpy(hd + 2, &v[0], 4);
        memcpy(hd + 10, &v[2], 4);
        memcpy(hd + 14, &v[3], 4);
        memcpy(hd + 18, &v[4], 4);
        memcpy(hd + 22, &v[5], 4);
        hd[26] = 1;
        hd[28] = 32;
        fwrite(hd, 1, sizeof(hd), f);
        const bool rgba = g_ps.fmt == VK_FORMAT_R8G8B8A8_UNORM || g_ps.fmt == VK_FORMAT_R8G8B8A8_SRGB;
        const uint8_t *src = map;
        uint8_t *row = g_malloc((size_t)w * 4);
        for (uint32_t y = 0; y < h; y++) {
            memcpy(row, src + (size_t)y * w * 4, (size_t)w * 4);
            for (uint32_t x = 0; rgba && x < w; x++) {
                const uint8_t t = row[x * 4];
                row[x * 4] = row[x * 4 + 2];
                row[x * 4 + 2] = t;
            }
            fwrite(row, 1, (size_t)w * 4, f);
        }
        g_free(row);
        fclose(f);
        hle_log("hakuX-hle-back", "present: picture %ux%u in %s", w, h, g_ps.shot_path);
    }
    T.vkUnmapMemory(g_hvk.dev, g_ps.shot_mem);
}

bool hle_present_unchanged(struct ANativeWindow *win, uint64_t key)
{
    if (g_ps.shot_want || !g_ps.shown || !win || win != g_ps.win || g_ps.stale || !g_ps.sc || key != g_ps.shown_key ||
        (uint32_t)ANativeWindow_getWidth(win) != g_ps.wsize.width ||
        (uint32_t)ANativeWindow_getHeight(win) != g_ps.wsize.height || hle_disp_gamma_seq() != g_ps.ramp_gen) {
        return false;
    }
    uint32_t gen, swap_id;
    const bool front = hle_disp_front(&gen, &swap_id);
    if (front != g_ps.have) {
        return false;
    }
    return !front || (gen == g_ps.f.gen && swap_id == g_ps.f.swap_id);
}

bool hle_present_begin(struct ANativeWindow *win, uint32_t *w, uint32_t *h)
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
#if defined(NC_DESKTOP) && !defined(_WIN32)
            if (!SDL_Vulkan_CreateSurface((SDL_Window *)win->sdl_window, g_hvk.inst, NULL, &g_ps.surf)) {
#elif defined(_WIN32)
            VkWin32SurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                .hinstance = (HINSTANCE)win->hinstance, .hwnd = (HWND)win->hwnd };
            if (vkCreateWin32SurfaceKHR(g_hvk.inst, &sci, NULL, &g_ps.surf) != VK_SUCCESS) {
#else
            VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                  .window = win };
            if (vkCreateAndroidSurfaceKHR(g_hvk.inst, &sci, NULL, &g_ps.surf) != VK_SUCCESS) {
#endif
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
    const uint32_t ww = (uint32_t)ANativeWindow_getWidth(win), wh = (uint32_t)ANativeWindow_getHeight(win);
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
    if (g_ps.idx >= g_ps.nimg) {
        hle_log("hakuX-hle-back", "present: acquired image %u of %u", g_ps.idx, g_ps.nimg);
        g_ps.stale = true;
        return false;
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

/* Image t at w x h (FXAA's at the frame's size, FSR's at the frame's in the
 * window), made again when that changes; false when it can't be. */
static bool img_target(PsImg *t, uint32_t w, uint32_t h, const char *what)
{
    if (t->img && t->w == w && t->h == h) {
        return true;
    }
    if (t->img) {
        wait_value(g_ps.submitted);
        T.vkDestroyImageView(g_hvk.dev, t->view, NULL);
        T.vkDestroyImage(g_hvk.dev, t->img, NULL);
        T.vkFreeMemory(g_hvk.dev, t->mem, NULL);
        t->img = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &t->img) != VK_SUCCESS) {
        t->img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetImageMemoryRequirements(g_hvk.dev, t->img, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &t->mem) != VK_SUCCESS ||
        T.vkBindImageMemory(g_hvk.dev, t->img, t->mem, 0) != VK_SUCCESS ||
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &t->view) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "present: no %ux%u %s image: %s off", w, h, what, what);
        T.vkDestroyImage(g_hvk.dev, t->img, NULL);
        t->img = VK_NULL_HANDLE;
        return false;
    }
    t->w = w;
    t->h = h;
    t->serial = 0;
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

/* A pass of pipeline p over all of image t from src (extra: the shader's
 * pc.extra); t is sampled after it. */
static void img_pass(VkCommandBuffer cb, PsFrame *f, unsigned set, PsImg *t, VkPipeline p, VkImageView src,
                     const float extra[4])
{
    barrier(cb, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    rendering(cb, t->view, t->w, t->h, false);
    float pc[12] = { -1.0f, -1.0f, 1.0f, 1.0f, 0, 0, 0, 0, 1.0f, 0, 0, 1.0f };
    memcpy(pc + 4, extra, 4 * sizeof(float));
    quad(cb, f, set, p, src, pc, t->w, t->h);
    T.vkCmdEndRendering(cb);
    barrier(cb, t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
            p == P_AREA ? "averaged down" : p == P_RCAS ? "scaled up by FSR" : "bilinear");
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
    if (g_ps.have && fxaa && g_ps.pipe[P_FXAA]) {
        if (img_target(&g_ps.fx, g_ps.f.w, g_ps.f.h, "FXAA")) {
            if (g_ps.fx.serial != g_ps.serial) {
                static const float none[4];
                img_pass(cb, f, 1, &g_ps.fx, g_ps.pipe[P_FXAA], g_ps.f.view, none);
                g_ps.fx.serial = g_ps.serial;
            }
            src = g_ps.fx.view;
            fxaa_done = true;
        } else {
            g_ps.pipe[P_FXAA] = VK_NULL_HANDLE;     /* no image for it: FXAA off */
        }
    }
    /* the frame's pixels per window pixel: over 1 (either axis) averaged
     * down, under 1 scaled up as chosen; FSR's EASU into its image first
     * (a new frame, or a new size), its RCAS on the way into the window */
    const float vw = sx * (float)ww, vh = sy * (float)wh;
    const float rx = g_ps.have ? (float)g_ps.f.w / vw : 1.0f, ry = g_ps.have ? (float)g_ps.f.h / vh : 1.0f;
    const bool down = (rx > 1.01f || ry > 1.01f) && average && g_ps.pipe[P_AREA];
    bool fsr = g_ps.have && !down && !g_crt && (rx < 0.99f || ry < 0.99f) && upscale == HLE_UPSCALE_FSR &&
               g_ps.pipe[P_EASU] && g_ps.pipe[P_RCAS];
    if (fsr && !img_target(&g_ps.up, (uint32_t)(vw + 0.5f), (uint32_t)(vh + 0.5f), "FSR")) {
        g_ps.pipe[P_EASU] = VK_NULL_HANDLE;         /* no image for it: bilinear */
        fsr = false;
    }
    if (fsr && (g_ps.up.serial != g_ps.serial || g_ps.up.fxaa != fxaa_done)) {
        const float extra[4] = { (float)g_ps.f.w, (float)g_ps.f.h, (float)g_ps.up.w, (float)g_ps.up.h };
        img_pass(cb, f, 1, &g_ps.up, g_ps.pipe[P_EASU], src, extra);
        g_ps.up.serial = g_ps.serial;
        g_ps.up.fxaa = fxaa_done;
    }
    VkImage dst = g_ps.img[g_ps.idx];
    barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    rendering(cb, g_ps.view[g_ps.idx], iw, ih, true);
    if (g_ps.have) {
        unsigned p = P_BILINEAR;
        float pc[12] = { -sx, -sy, sx, sy, 0.0f, 0.0f, 0.0f, 0.0f, g_ps.rot[0], g_ps.rot[1], g_ps.rot[2], g_ps.rot[3] };
        VkImageView from = src;
        if (down) {
            p = P_AREA;
            pc[4] = rx;
            pc[5] = ry;
        } else if (fsr) {
            p = P_RCAS;
            pc[4] = 0.8705506f;             /* sharpness 0.2 stops: 2^-0.2 (FsrRcasCon, AMD's sample's) */
            from = g_ps.up.view;
        }
        if (g_crt && g_ps.pipe[P_CRT]) {
            p = P_CRT;
            pc[4] = (float)g_crt;
            pc[5] = (float)hle_host_render_scale();
        }
        quad(cb, f, 0, g_ps.pipe[p], p == P_CRT ? src : from, pc, iw, ih);
        if (g_osd > 0 && g_ps.pipe[P_OSD]) {
            /* top left, 4 digits each 0.022 of the window's height wide */
            const float dh = 0.045f, dw = dh * 0.62f * (float)wh / (float)ww;
            float oc[12] = { -0.98f, -0.97f, -0.98f + 4 * dw * 2, -0.97f + dh * 2, g_osd, 0, 0, 0,
                             g_ps.rot[0], g_ps.rot[1], g_ps.rot[2], g_ps.rot[3] };
            quad(cb, f, 0, g_ps.pipe[P_OSD], src, oc, iw, ih);
        }
        log_path(p, fxaa_done, (uint32_t)(vw + 0.5f), (uint32_t)(vh + 0.5f));
    }
    T.vkCmdEndRendering(cb);
    const bool shot = g_ps.shot_want && g_ps.shot_can && shot_buf((VkDeviceSize)iw * ih * 4);
    if (shot) {
        barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                .imageExtent = { iw, ih, 1 } };
        T.vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_ps.shot_buf, 1, &r);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
    }
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
        if (shot && wait_value(value)) {
            shot_write(iw, ih);
            g_ps.shot_want = false;
        }
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
