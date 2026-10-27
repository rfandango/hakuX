/*
 * hakuX HLE GPU — fixed-function shader generators (spec §5.8 FF; pure).
 * Keys are built by the back end from the draw state (render/texture-state
 * arrays + FVF); the generators emit GLSL 4.50 for glslang.
 *
 * Interface shared with vk/draw.c:
 * - vertex inputs: location 0 position, 1 normal, 2 diffuse, 3 specular,
 *   4..7 texcoord 0..3 (VIDS supplies the formats from the FVF);
 * - set 0: binding 0 sampled images[], binding 1 samplers[] (bindless);
 * - push constants: HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_FF_H
#define HW_XBOX_HLE_SHADER_FF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Xbox FVF bits (D3DFVF_*) */
#define HLE_FVF_POSITION_MASK 0x00E
#define HLE_FVF_XYZ           0x002
#define HLE_FVF_XYZRHW        0x004
#define HLE_FVF_NORMAL        0x010
#define HLE_FVF_PSIZE         0x020
#define HLE_FVF_DIFFUSE       0x040
#define HLE_FVF_SPECULAR      0x080
#define HLE_FVF_TEXCOUNT_MASK 0xF00
#define HLE_FVF_TEXCOUNT_SHIFT 8

/* Texture coordinate components for coordinate set i of an FVF (1-4). */
static inline unsigned hle_fvf_tex_size(uint32_t fvf, unsigned i)
{
    static const unsigned k_size[4] = { 2, 3, 4, 1 };
    return k_size[(fvf >> (16 + 2 * i)) & 3];
}

#define HLE_FF_STAGES 4
/* With POINTSPRITEENABLE the runtime builds the FF cascade from texture
 * stage 3 alone (4134 0x1e95e0 starts its stage loop at 3): the NV2A gives
 * unit 3 the point-sprite coordinates. The FF keys then describe that one
 * stage as their stage 0. */
#define HLE_FF_SPRITE_STAGE 3

/* Texture-coordinate generation (TEXCOORDINDEX >> 16) */
enum { HLE_TCI_PASSTHRU = 0, HLE_TCI_CAMERANORMAL, HLE_TCI_CAMERAPOSITION,
       HLE_TCI_CAMERAREFLECTION, HLE_TCI_SPHEREMAP };
/* Fog distance source (HleFfVsKey.fog_src), as D3D programs the NV2A fog
 * generator: FOGTABLEMODE NONE takes the vertex specular alpha as the fog
 * factor, RANGEFOGENABLE the eye distance, otherwise the eye depth. */
enum { HLE_FOG_SRC_OFF = 0, HLE_FOG_SRC_PLANAR, HLE_FOG_SRC_RADIAL, HLE_FOG_SRC_SPEC_ALPHA };
/* Light types (D3DLIGHTTYPE) */
enum { HLE_LIGHT_POINT = 1, HLE_LIGHT_SPOT = 2, HLE_LIGHT_DIRECTIONAL = 3 };
#define HLE_FF_LIGHTS 8

typedef struct HleFfVsKey {
    uint32_t fvf;
    uint8_t texcoord[HLE_FF_STAGES];  /* TSS TEXCOORDINDEX (low 3 bits) per stage */
    uint8_t texgen[HLE_FF_STAGES];    /* HLE_TCI_* per stage */
    uint8_t texxform[HLE_FF_STAGES];  /* texture transform: element count (0 off) |
                                         0x10 projected */
    uint8_t ntex;                     /* stages with a texture coordinate output */
    uint8_t flat;                     /* SHADEMODE FLAT */
    uint8_t lighting;                 /* LIGHTING (without a vertex normal N.L is 0) */
    uint8_t nlights;                  /* enabled lights */
    uint8_t light_type[HLE_FF_LIGHTS];
    uint8_t specular;                 /* SPECULARENABLE */
    uint8_t local_viewer;             /* LOCALVIEWER */
    uint8_t normalize;                /* NORMALIZENORMALS */
    uint8_t colorvertex;              /* COLORVERTEX */
    uint8_t src[4];                   /* diffuse, ambient, specular, emissive material
                                         source: 0 material, 1 color1, 2 color2 */
    uint8_t blend;                    /* vertex blending: matrices (0 = off, 2..4) */
    uint8_t blend_mix;                /* the last weight is 1 - the vertex's others
                                         (D3DRS_VERTEXBLEND 1 / 3 / 5); else every
                                         weight is the vertex's (2 / 4 / 6) */
    uint8_t fog_src;                  /* HLE_FOG_SRC_* (OFF: eye depth, no block) */
    uint8_t points;                   /* point lists: 1 POINTSIZE (or the vertex PSIZE),
                                         2 scaled by eye distance (POINTSCALEENABLE) */
    uint8_t wbuf;                     /* W-buffering (XYZRHW): vW = 1 / rhw */
    uint8_t cmp_normal;               /* the normal is NV2A CMP (11:11:10 signed), fetched
                                         as two 16-bit halves (a declaration's) */
    uint8_t zclamp;                   /* depth clamped, not clipped (the pipeline's: in
                                         the padding, as HleVshKey's) */
} HleFfVsKey;

/* The FF uniform block (set 1, binding 0: the dynamic window the vertex
 * programs use for their constants), vec4 slots: */
enum {
    HLE_FFU_WV = 0,                   /* world0 * view, column-major mat4 */
    HLE_FFU_PROJ = 4,                 /* projection, column-major mat4 */
    HLE_FFU_NORMAL = 8,               /* inverse transpose of WV (3 columns) */
    HLE_FFU_AMBIENT = 11,             /* AMBIENT render state */
    HLE_FFU_MATERIAL = 12,            /* diffuse, ambient, specular, emissive, (power) */
    HLE_FFU_LIGHTS = 20,              /* per light 7 vec4: diffuse, specular, ambient,
                                         position (eye), direction (eye),
                                         (range, falloff, att0, att1),
                                         (att2, cos theta/2, cos phi/2, 0) */
    HLE_FFU_TEXMAT = 76,              /* 4 texture matrices (column-major mat4) */
    HLE_FFU_BLEND = 92,               /* worldN * view for N = 1..3 (column-major) */
    HLE_FFU_POINT = 104,              /* (POINTSIZE, _MIN, _MAX, viewport height),
                                         (POINTSCALE_A, _B, _C, target pixel scale) */
    HLE_FFU_COUNT = 106,
};
#define HLE_FFU_LIGHT_VEC4 7

/* The FF VS reads the uniform block (lighting, texgen, texture transforms,
 * vertex blending or fog by eye distance); otherwise it only needs the push
 * constants. */
static inline int hle_ff_vs_needs_ubo(const HleFfVsKey *k)
{
    int n = k->lighting || k->blend || k->points || k->fog_src == HLE_FOG_SRC_PLANAR ||
            k->fog_src == HLE_FOG_SRC_RADIAL;
    for (unsigned s = 0; s < HLE_FF_STAGES; s++) {
        n |= k->texgen[s] || k->texxform[s];
    }
    return n;
}

typedef struct HleFfStage {
    uint32_t colorop, colorarg0, colorarg1, colorarg2;
    uint32_t alphaop, alphaarg0, alphaarg1, alphaarg2;
    uint32_t resultarg;
    uint8_t texture;                  /* a texture is bound to this stage */
    uint8_t tex_dim;                  /* 2 = 2D, 3 = cube, 4 = volume */
    uint8_t alpha_kill;               /* D3DTSS_ALPHAKILL: texel alpha 0 discards */
    uint8_t unset;                    /* no texture set (D3D's Textures[stage] NULL): an
                                         op reading the texture passes CURRENT through */
} HleFfStage;

typedef struct HleFfPsKey {
    HleFfStage st[HLE_FF_STAGES];
    uint8_t nstages;                  /* stages up to the first COLOROP DISABLE */
    uint8_t alpha_test;               /* ALPHATESTENABLE */
    uint8_t alpha_func;               /* D3DCMP_* - 0x200 (0..7) */
    uint8_t specular;                 /* SPECULARENABLE (add specular) */
    uint8_t flat;
    uint8_t fog;                      /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t point_sprite;             /* POINTSPRITEENABLE on a point list: stage 0
                                         (texture stage 3, see HLE_FF_SPRITE_STAGE)
                                         samples at the point's coordinates */
    uint8_t out_bgra;                 /* 8-bit (B8) target: it stores blue */
    uint8_t wbuf;                     /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t sblend;                   /* the NV2A's signed blend equations, ONE / ONE (the
                                         fixed-function blend is off): the output and
                                         the target's pixel (uFb, set 2) as two's-
                                         complement bytes, 1 dst + src (ADDSIGNED), 2
                                         dst - src (REVSUBTRACTSIGNED), saturated */
    uint8_t fbfetch;                  /* stages (bits) whose texture is the colour target:
                                         its pixel as the earlier draws left it (uFb),
                                         as the NV2A's texture unit reads the memory
                                         the draw renders */
} HleFfPsKey;

/* Push constants (std430 layout, <= 256 bytes: 256 used); HLE_PUSH_GLSL
 * declares the same block for every generator. */
typedef struct HleFfPush {
    float mvp[16];                    /* world * view * projection, column-major */
    float vp[2];                      /* D3D screen pixels -> NDC: 2 aa / target w,
                                         -2 aa / target h, then + (-1, 1) (aa: the
                                         NV2A's antialiasing scale, else 1) */
    uint32_t bump3[2];                /* stage 3's BUMPENVMAT (as bump) */
    uint32_t tex[HLE_FF_STAGES];      /* image index | sampler index << 16 */
    uint32_t tfactor;                 /* TEXTUREFACTOR (D3DCOLOR) */
    uint32_t bumpl[3];                /* stages 1..3: BUMPENVLSCALE, LOFFSET (half
                                         pair) */
    float alpha_ref;                  /* ALPHAREF / 255 */
    uint32_t flags;                   /* bit s: stage s samples a linear texture
                                         (texel coordinates); bit 8 + s: that
                                         texture is a surface, rendered at the
                                         resolution scale; bits 16-23: the
                                         target's scale (point sizes), 24-31:
                                         the resolution scale (those texels) */
    float zscale;                     /* vertex programs: 1 / depth maximum (2^-n for
                                         the integer formats: HLE_ZNORM_GLSL) */
    uint32_t fog_color;               /* FOGCOLOR (D3DCOLOR) */
    float fog[4];                     /* FOGSTART, FOGEND, FOGDENSITY, 0 */
    uint32_t psc[20];                 /* pixel shaders: C0[8], C1[8], final C0, C1
                                         (D3DCOLOR), W-buffer offset and slope */
    float scr[4];                     /* fixed function: D3D screen pixels = xy *
                                         scr.xy + scr.zw (XYZRHW: the vertex's;
                                         transformed: NDC), the screen-space offset
                                         included */
    uint32_t bump[4];                 /* pixel shaders: BUMPENVMAT 00, 01 | 10, 11
                                         (half-float pairs) of stage 1 (xy) and
                                         stage 2 (zw); each stage has its own
                                         (Halo 2's water distorts its reflection
                                         a quarter as much as the rest) */
} HleFfPush;

#define HLE_PUSH_MEMBERS_GLSL \
    "  mat4 mvp;\n  vec2 vp;\n  uvec2 bump3;\n  uvec4 tex;\n  uint tfactor;\n" \
    "  uint bumpl[3];\n" \
    "  float alpha_ref;\n  uint flags;\n  float zscale;\n  uint fog_color;\n" \
    "  vec4 fog;\n  uint psc[20];\n  vec4 scr;\n  uvec4 bump;\n"
#define HLE_PUSH_GLSL \
    "layout(push_constant, std430) uniform Push {\n" HLE_PUSH_MEMBERS_GLSL "} pc;\n"
/* The same block where the push constants are smaller (Xclipse's 128 bytes):
 * a storage buffer, std430 as well (vk/draw.c, set 3) */
#define HLE_PUSH_SSBO_GLSL \
    "layout(set = 3, binding = 0, std430) readonly buffer Push {\n" HLE_PUSH_MEMBERS_GLSL "} pc;\n"

/* The block in a storage buffer (set by the back end before any shader is made) */
extern bool g_hle_push_ssbo;

/* The constants (set 1: VshConst, FfConst) in a read-only storage buffer, not
 * a uniform one (vk/quirks.h HLE_QUIRK_CONST_SSBO: Xclipse; set by the back
 * end before any shader is made); the block's qualifier */
extern bool g_hle_const_ssbo;
static inline const char *hle_const_glsl(void)
{
    return g_hle_const_ssbo ? "readonly buffer" : "uniform";
}

/* Mali (vk/quirks.h HLE_QUIRK_Z_EQ_W, set with g_hle_push_ssbo): a clip-space
 * z equal to w corrupts the depth buffer (PPSSPP's EQUAL_WZ_CORRUPTS_DEPTH,
 * ARMSX2): the vertex shaders end by moving such a vertex a hair inside. */
extern bool g_hle_glsl_zw;
#define HLE_ZW_NUDGE_GLSL "  if (gl_Position.z == gl_Position.w) gl_Position.z *= 0.999999;\n"

/* No depthClamp (vk/vkdev.h HleCaps.depth_clamp off: Mali's Bifrost): a
 * z-clamped draw's vertices (the key's zclamp) are clamped to the depth range
 * instead, so the near and far planes don't clip them; depth across a
 * primitive that crosses them comes from its clamped corners. */
extern bool g_hle_glsl_zclamp;
#define HLE_ZCLAMP_GLSL \
    "  if (gl_Position.w > 0.0) gl_Position.z = clamp(gl_Position.z, 0.0, gl_Position.w);\n"

/* No descriptor indexing (HleCaps.bindless off: Mali's Bifrost, PowerVR's
 * Rogue): a pixel shader's bindless arrays become the draw's own set (vk/draw.c
 * fixed_bind): sized (each stage's images, then the default sampler and each
 * stage's), indexed without nonuniformEXT (the push block's indices, the same
 * for the whole draw). g_hle_glsl_fixed: hle_glsl_fixed_slots rewrites the
 * generated text; its new length, 0 if it doesn't fit in cap. */
extern bool g_hle_glsl_fixed;
size_t hle_glsl_fixed_slots(char *src, size_t cap);

static inline const char *hle_push_glsl(void)
{
    return g_hle_push_ssbo ? HLE_PUSH_SSBO_GLSL : HLE_PUSH_GLSL;
}

/* W-buffering (D3DRS_ZENABLE = D3DZB_USEW, NV2A z_perspective): the stored
 * depth is the perspective-correct w (in depth units, as the vertex program
 * left it) with the polygon offset in w: ZOFFSET + ZSLOPESCALE * max |dw|
 * (psc[18], psc[19], float bits; zero when SOLIDOFFSETENABLE is off). The
 * pixel generators declare vW (location 9, from the program vertex shader). */
#define HLE_WBUF_IN_GLSL "layout(location = 9) in float vW;\n"
/* Screen-space depth z (0 .. the buffer's maximum, 2^n - 1) to [0, 1] as
 * `out`: pc.zscale is 2^-n for the integer formats (pbdraw.c), and
 * z 2^-n + z 2^-2n is z / (2^n - 1) to float precision, both products exact
 * and summed once ("precise": not folded into one product, whose scale
 * float32 can't hold). The float nearest 1 / (2^24 - 1) is above it: a
 * vertex a step in front of the far plane (2^24 - 2) came out at 1.0, and
 * Super Monkey Ball Deluxe's movies, drawn there over a depth buffer cleared
 * to the far plane, failed their LESS test (black, the subtitles over it
 * drawn). Any other scale (1.0, a float depth buffer) is the plain product. */
#define HLE_ZNORM_GLSL(z, out) \
    "  precise float " out " = " z " * pc.zscale + " z " * (pc.zscale == uintBitsToFloat(0x33800000u) ||\n" \
    "      pc.zscale == uintBitsToFloat(0x37800000u) ? pc.zscale * pc.zscale : 0.0);\n"
/* first in main (derivatives before any discard), stored last */
#define HLE_WBUF_CALC_GLSL \
    "  float wRaw = vW + uintBitsToFloat(pc.psc[18]) + uintBitsToFloat(pc.psc[19]) *\n" \
    "      max(abs(dFdx(vW)), abs(dFdy(vW)));\n" \
    HLE_ZNORM_GLSL("wRaw", "wN") \
    "  float wDepth = clamp(wN, 0.0, 1.0);\n"
#define HLE_WBUF_STORE_GLSL "  gl_FragDepth = wDepth;\n"

/* EQUAL depth passes that do not write (vk/draw.c eq_tol) test LESSEQUAL a
 * step toward the eye, which passes everything in front of the stored depth
 * too: these shaders discard what is more than a few steps in front of it,
 * read as an input attachment (set 2, binding 1). Bits of the keys' fbfetch,
 * by the depth buffer's step: D24, D16. The declaration's input attachment
 * index (HLE_IA_DEPTH) is stripped from the SPIR-V (glsl_compile.c): one
 * without an index reads the depth attachment (local read's default map). */
#define HLE_FB_ZEQ24 0x20u
#define HLE_FB_ZEQ16 0x40u
#define HLE_FB_ZEQ (HLE_FB_ZEQ24 | HLE_FB_ZEQ16)
#define HLE_IA_DEPTH 7
#define HLE_ZEQ_DECL_GLSL \
    "layout(input_attachment_index = 7, set = 2, binding = 1) uniform subpassInput uZ;\n"
/* after the alpha test (the derivatives' neighbours are still running) */
#define HLE_ZEQ_TEST_GLSL(fb) \
    (((fb) & HLE_FB_ZEQ16) ? \
     "  if (gl_FragCoord.z < subpassLoad(uZ).r - 2.0 / 65535.0) discard;\n" : \
     "  if (gl_FragCoord.z < subpassLoad(uZ).r - 4.0 / 16777215.0) discard;\n")

/* NV2A's projective texture division: coordinates times 1/q, with 1/0 the
 * largest float and 0 times it 0, so q = 0 projects (0, 0, 0) to 0, never
 * NaN. NGB writes oT0 = 0 to turn far shadow receivers' lookups off (lit);
 * 0/0 left NaN in the shadow filter weights and those receivers black. */
#define HLE_NVPROJ_GLSL \
    "vec3 nvproj(vec4 t) {\n" \
    "  float iq = abs(t.w) > 1.0e-30 ? 1.0 / t.w : 3.0e38;\n" \
    "  return clamp(t.xyz * iq, -1.0e7, 1.0e7);\n" \
    "}\n"

/* Screen positions v (D3D screen pixels) to where the NV2A rasterizer puts
 * them. It truncates them to 1/16 pixel, and D3D's screen-space offset
 * (0.53125: half a pixel and 1/32, the truncation's rounding bias) lands the
 * positions it gives on pixel centres, which the edge rules then decide.
 * Here every position moves by that 1/32 alone: the same place for D3D's
 * positions, and every vertex by the same amount. Truncating each vertex
 * moved them apart by up to 1/16 pixel: a decal's quad and the surface under
 * it (other vertices) tilted against each other by up to 1/16 of the depth
 * slope per pixel, at a grazing angle more than the decal stands off, and it
 * flickered out as the camera moved (Burnout 3's road stains and manhole
 * covers, Dead or Alive 2's eaves shadows). debug.hakux.snap=trunc: the
 * truncation (HLE_SNAP_TRUNC_FLAG), for A/B. */
#define HLE_SNAP_TRUNC_FLAG 8192u
#define HLE_SUBPIXEL_GLSL(v) \
    "  " v " = (pc.flags & 8192u) != 0u ? trunc(" v " * 16.0) / 16.0 : " v " - 0.03125;\n"

/* Screen positions v (D3D screen pixels, after HLE_SUBPIXEL_GLSL) at a
 * resolution scale s > 1 (HleFfPush.flags bits 16-23; bit 12: the NV2A's
 * antialiasing, whose texels are its samples: not then). The NV2A covers a
 * texel an edge's line passes through the centre of when it is a left or
 * top edge and not when right or bottom; scaled, such an edge crosses the
 * middle of the texel's s x s image pixels and covers about half of them.
 * D3D's full-target quads start on the first texel's centre (its 0.53125
 * offset, to 0.5): scaled they left the image's first column and
 * row as an earlier pass left them, and Splinter Cell Chaos Theory's bloom
 * blurred the blue its EMF vision had left there into a band along the
 * frame's left and top. Positions on a texel's centre move to its first
 * image pixel's: the texel is covered, or not, whole. */
#define HLE_TEXEL_EDGE_FLAG 4096u
#define HLE_TEXEL_EDGE_GLSL(v) \
    "  { uint rsc = (pc.flags >> 16) & 255u;\n" \
    "    if (rsc > 1u && (pc.flags & 4096u) == 0u)\n" \
    "      " v " -= vec2(equal(fract(" v "), vec2(0.5))) * (0.5 - 0.5 / float(rsc)); }\n"

/* GLSL source into buf (NUL-terminated); returns its length, or 0 if it did
 * not fit. */
size_t hle_ff_vs_glsl(const HleFfVsKey *k, char *buf, size_t cap);
size_t hle_ff_ps_glsl(const HleFfPsKey *k, char *buf, size_t cap);

#endif
