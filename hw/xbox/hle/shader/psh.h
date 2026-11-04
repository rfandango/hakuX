/*
 * hakuX HLE GPU — Xbox pixel shaders (NV2A register combiners and texture
 * modes) -> GLSL 4.50 (spec §5.8; pure). The key is the pixel-shader
 * render-state block (5659 render states 0..56, which SetPixelShader loads
 * from the D3DPIXELSHADERDEF); the combiner constants are push constants.
 *
 * Interface: inputs location 0 vDiffuse, 1 vSpecular, 2..5 vTex0..3, 6 vFog
 * (as vsh.h / ff.h); set 0 bindless images and samplers; HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_PSH_H
#define HW_XBOX_HLE_SHADER_PSH_H

#include <stddef.h>
#include <stdint.h>

/* D3DPIXELSHADERDEF / render-state indices (5659: D3DRS_PSALPHAINPUTS0 = 0) */
enum {
    HLE_PS_ALPHA_IN = 0,                /* [8] */
    HLE_PS_FINAL_ABCD = 8,
    HLE_PS_FINAL_EFG = 9,
    HLE_PS_C0 = 10,                     /* [8] */
    HLE_PS_C1 = 18,                     /* [8] */
    HLE_PS_ALPHA_OUT = 26,              /* [8] */
    HLE_PS_RGB_IN = 34,                 /* [8] */
    HLE_PS_COMPARE = 42,
    HLE_PS_FINAL_C0 = 43,
    HLE_PS_FINAL_C1 = 44,
    HLE_PS_RGB_OUT = 45,                /* [8] */
    HLE_PS_COUNT = 53,
    HLE_PS_TEXMODES = 54,
    HLE_PS_DOTMAP = 55,
    HLE_PS_INPUTTEX = 56,
    HLE_PS_REGS = 57,
};

/* PSTextureModes values */
enum {
    HLE_PST_NONE = 0, HLE_PST_PROJECT2D, HLE_PST_PROJECT3D, HLE_PST_CUBEMAP,
    HLE_PST_PASSTHRU, HLE_PST_CLIPPLANE, HLE_PST_BUMPENVMAP, HLE_PST_BUMPENVMAP_LUM,
    HLE_PST_BRDF, HLE_PST_DOT_ST, HLE_PST_DOT_ZW, HLE_PST_DOT_RFLCT_DIFF,
    HLE_PST_DOT_RFLCT_SPEC, HLE_PST_DOT_STR_3D, HLE_PST_DOT_STR_CUBE, HLE_PST_DPNDNT_AR,
    HLE_PST_DPNDNT_GB, HLE_PST_DOTPRODUCT, HLE_PST_DOT_RFLCT_SPEC_CONST,
};

typedef struct HlePshKey {
    uint32_t rgb_in[8], rgb_out[8], alpha_in[8], alpha_out[8];
    uint32_t final_abcd, final_efg;
    uint32_t count;                     /* PSCombinerCount */
    uint32_t tex_modes;                 /* PSTextureModes */
    uint32_t dot_map;                   /* PSDotMapping */
    uint32_t input_tex;                 /* PSInputTexture */
    uint32_t compare;                   /* PSCompareMode */
    uint8_t alpha_test;                 /* ALPHATESTENABLE */
    uint8_t alpha_func;                 /* D3DCMP_* - 0x200 (0..7) */
    uint8_t flat;
    uint8_t fog;                        /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t fog_abs;                    /* the factor of |distance|: an NV2A _ABS fog mode
                                           the title pushed (D3D's are never _ABS) */
    uint8_t debug;                      /* output instead: 1 t0, 2 v0, 3 r0, 4 fog, 5 v1,
                                           6 t3, 7 r0.a, 8 t2, 9 shadow bands, 10 v1.a,
                                           11 t0.a, 12 t3.a, 13 fract(T3 s/q, t/q),
                                           14 |T3 str| / 4, 15 t3 and 16 t0 as sampled,
                                           17 shadow-buffer draws: (stored depth, r,
                                           result) / range, unblended; 18 magenta,
                                           19 t1, 20 r1, 21 |T2 str| / 4, 22 T2 str
                                           direction, 23 T2 NaN / Inf, 24 stage 2's
                                           cube along (0.3, 0.5, 0.8) */
    uint8_t point_sprite;               /* POINTSPRITEENABLE on a point list: texture
                                           unit 3's coordinates are the point's */
    /* Shadow buffers: stages whose texture is a depth surface (bits 0-3),
     * D3DRS_SHADOWFUNC - 0x200 (bits 4-6); per stage (2 bits) the depth
     * range the r coordinate is in: 1 D24 (2^24 - 1), 2 D16, 3 float. */
    uint8_t shadow;
    uint8_t shadow_fmt;
    uint8_t out_bgra;                   /* 8-bit (B8) target: it stores blue */
    uint8_t alpha_kill;                 /* stages with D3DTSS_ALPHAKILL (bits 0-3) */
    uint8_t vol;                        /* stages whose texture is a volume (bits 0-3):
                                           2D-coordinate modes fetch it in 3D, as the NV2A
                                           (LLE): PROJECT2D at r = 0, bump-env at the
                                           coordinates' r; HLE_PSH_RECT: the stage the
                                           push block's fog.w names has its linear
                                           texture kept to its own size, part of a
                                           larger surface (ff.h HLE_NVRECT_GLSL); bits
                                           5-7 free */
    uint8_t wbuf;                       /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t fbfetch;                    /* stages (bits 0-3) whose texture is the colour
                                           target drawn into: its pixel, read as the
                                           input attachment (set 2) */
    uint8_t specular;                   /* SPECULARENABLE: D3D's own final combiner adds
                                           v1 (a shader without one) */
    uint8_t sblend;                     /* the NV2A's signed blend equations (ff.h
                                           HleFfPsKey.sblend) */
} HlePshKey;
#define HLE_PSH_RECT 0x10u              /* HlePshKey.vol: a linear texture kept to its size */

/* Key from the render-state array (rs[0..56] = the pixel-shader block). */
void hle_psh_key(const uint32_t *rs, HlePshKey *k);

/* GLSL into buf; returns its length, 0 when it does not fit. Unsupported
 * texture modes sample as PROJECT2D and set *unsupported (may be NULL). */
size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported);

#endif
