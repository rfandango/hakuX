/*
 * hakuX HLE GPU — Xbox pixel shaders -> GLSL (pure). See psh.h.
 *
 * Register combiners (D3DPIXELSHADERDEF, NV_register_combiners on NV2A):
 * - input byte: bits 0-3 register, bit 4 alpha channel (RGB portion: .aaa;
 *   alpha portion: .a, else .b), bits 5-7 input mapping;
 * - output word: bits 0-3 CD, 4-7 AB, 8-11 SUM/MUX register, bit 12 CD dot,
 *   13 AB dot, 14 MUX, 15-17 output mapping, 18 CD blue-to-alpha, 19 AB;
 * - every input of a stage is read before any output is written; stage
 *   results are clamped to [-1, 1]; r0.a starts as t0.a;
 * - PSCombinerCount: bits 0-7 stages, bit 8 MUX on r0.a's MSB (else LSB),
 *   bit 12 a C0 per stage (else stage 0's), bit 16 the same for C1;
 * - final combiner: rgb = A*B + (1-A)*C + D, alpha = G, inputs clamped to
 *   [0, 1]; EF_PROD = E*F, V1R0_SUM = v1 + r0 (EFG flags: 0x80 clamp,
 *   0x40 complement v1, 0x20 complement r0). Without one, the output is r0.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/psh.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
} Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

void hle_psh_key(const uint32_t *rs, HlePshKey *k)
{
    memset(k, 0, sizeof(*k));
    unsigned n = rs[HLE_PS_COUNT] & 0xFF;
    n = n > 8 ? 8 : n;
    for (unsigned s = 0; s < n; s++) {      /* stages past the count do not split keys */
        k->rgb_in[s] = rs[HLE_PS_RGB_IN + s];
        k->rgb_out[s] = rs[HLE_PS_RGB_OUT + s];
        k->alpha_in[s] = rs[HLE_PS_ALPHA_IN + s];
        k->alpha_out[s] = rs[HLE_PS_ALPHA_OUT + s];
    }
    k->final_abcd = rs[HLE_PS_FINAL_ABCD];
    k->final_efg = rs[HLE_PS_FINAL_EFG];
    k->count = rs[HLE_PS_COUNT] & 0x111FF;
    k->tex_modes = rs[HLE_PS_TEXMODES] & 0xFFFFF;
    k->dot_map = rs[HLE_PS_DOTMAP] & 0xFFF;
    k->input_tex = rs[HLE_PS_INPUTTEX] & 0x310000;
    k->compare = rs[HLE_PS_COMPARE] & 0xFFFF;
}

/* Register as a vec4 expression; final = the final combiner. */
static const char *reg(unsigned r, bool final)
{
    static const char *const k_reg[16] = {
        "vec4(0.0)", "C0", "C1", "fogr", "v0", "v1", "vec4(0.0)", "vec4(0.0)",
        "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efp",
    };
    if (!final && r >= 14) {
        return "vec4(0.0)";
    }
    return k_reg[r & 15];
}

/* Input of a combiner: alpha = the alpha portion (float), else vec3. */
static void in_expr(Buf *b, uint8_t in, bool alpha, bool final)
{
    char x[48];
    const char *r = reg(in & 0xF, final);
    bool a = (in & 0x10) != 0;
    if (alpha) {
        snprintf(x, sizeof(x), "%s.%c", r, a ? 'a' : 'b');
    } else {
        snprintf(x, sizeof(x), a ? "%s.aaa" : "%s.rgb", r);
    }
    switch ((in >> 5) & 7) {
    case 0: put(b, "max(%s, 0.0)", x); break;                    /* UNSIGNED_IDENTITY */
    case 1: put(b, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;     /* UNSIGNED_INVERT */
    case 2: put(b, "(2.0 * max(%s, 0.0) - 1.0)", x); break;      /* EXPAND_NORMAL */
    case 3: put(b, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;     /* EXPAND_NEGATE */
    case 4: put(b, "(max(%s, 0.0) - 0.5)", x); break;            /* HALFBIAS_NORMAL */
    case 5: put(b, "(-max(%s, 0.0) + 0.5)", x); break;           /* HALFBIAS_NEGATE */
    case 6: put(b, "(%s)", x); break;                            /* SIGNED_IDENTITY */
    default: put(b, "(-(%s))", x); break;                        /* SIGNED_NEGATE */
    }
}

static const char *out_map(uint32_t out)
{
    switch ((out >> 15) & 7) {
    case 1: return "(%s - 0.5)";                        /* BIAS */
    case 2: return "(%s * 2.0)";                        /* SHIFTLEFT_1 */
    case 3: return "((%s - 0.5) * 2.0)";                /* SHIFTLEFT_1_BIAS */
    case 4: return "(%s * 4.0)";                        /* SHIFTLEFT_2 */
    case 6: return "(%s * 0.5)";                        /* SHIFTRIGHT_1 */
    default: return "(%s)";
    }
}

/* Writable combiner outputs. */
static const char *dst(unsigned r)
{
    switch (r & 15) {
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: return NULL;                               /* DISCARD and read-only */
    }
}

static void stage(Buf *b, const HlePshKey *k, unsigned s)
{
    uint32_t ri = k->rgb_in[s], ro = k->rgb_out[s], ai = k->alpha_in[s], ao = k->alpha_out[s];
    unsigned c0 = (k->count & 0x1000) ? s : 0, c1 = 8 + ((k->count & 0x10000) ? s : 0);
    put(b, "  { /* combiner stage %u */\n"
           "    vec4 C0 = col(pc.psc[%u]), C1 = col(pc.psc[%u]);\n", s, c0, c1);
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        put(b, "    vec3 %c = ", k_v[i]);
        in_expr(b, (ri >> (24 - 8 * i)) & 0xFF, false, false);
        put(b, ";\n    float %ca = ", k_v[i]);
        in_expr(b, (ai >> (24 - 8 * i)) & 0xFF, true, false);
        put(b, ";\n");
    }
    /* MSB: bit 7 of the 8-bit value (D3D cnd: r0.a > 0.5); 1.0 * 0.5 is
     * 0xFF >> 1 = 0x7F, clear */
    put(b, "    bool mx = %s;\n", (k->count & 0x100) ? "r0.a > 0.5"
                                                   : "(uint(r0.a * 255.0) & 1u) == 1u");
    put(b, "    vec3 ab = %s, cd = %s;\n", (ro & 0x2000) ? "vec3(dot(A, B))" : "A * B",
        (ro & 0x1000) ? "vec3(dot(C, D))" : "C * D");
    put(b, "    vec3 ms = %s;\n", (ro & 0x4000) ? "mx ? cd : ab" : "ab + cd");
    put(b, "    float aab = Aa * Ba, acd = Ca * Da, ams = %s;\n",
        (ao & 0x4000) ? "mx ? acd : aab" : "aab + acd");
    char e[64];
    static const char *const k_rv[3] = { "ab", "cd", "ms" };
    static const char *const k_av[3] = { "aab", "acd", "ams" };
    for (unsigned i = 0; i < 3; i++) {
        snprintf(e, sizeof(e), out_map(ro), k_rv[i]);
        put(b, "    %s = clamp(%s, -1.0, 1.0);\n", k_rv[i], e);
        snprintf(e, sizeof(e), out_map(ao), k_av[i]);
        put(b, "    %s = clamp(%s, -1.0, 1.0);\n", k_av[i], e);
    }
    /* outputs: AB bits 4-7, CD bits 0-3, SUM bits 8-11 */
    static const unsigned k_shift[3] = { 4, 0, 8 };
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ro >> k_shift[i]);
        if (d) {
            put(b, "    %s.rgb = %s;\n", d, k_rv[i]);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ao >> k_shift[i]);
        if (d) {
            put(b, "    %s.a = %s;\n", d, k_av[i]);
        }
    }
    if (ro & 0x80000) {                                 /* AB_BLUE_TO_ALPHA */
        const char *d = dst(ro >> 4);
        if (d) {
            put(b, "    %s.a = ab.b;\n", d);
        }
    }
    if (ro & 0x40000) {                                 /* CD_BLUE_TO_ALPHA */
        const char *d = dst(ro);
        if (d) {
            put(b, "    %s.a = cd.b;\n", d);
        }
    }
    put(b, "  }\n");
}

static void final_combiner(Buf *b, const HlePshKey *k)
{
    uint32_t abcd = k->final_abcd, efg = k->final_efg;
    put(b, "  vec4 outc;\n");
    if (!abcd && !efg) {
        /* No final combiner in the shader: the one D3D's fog setters push
         * (Otogi 0xfc362 / 0xfc3ce): A = fog.a, B = r0 or, with
         * SPECULARENABLE, v1 + r0 (clamped), C = fog.rgb, G = r0.a; without
         * fog, D = B (fogf is 1 then) */
        put(b, "  outc = vec4(mix(fogr.rgb, clamp(%s, 0.0, 1.0), fogf), clamp(r0.a, 0.0, 1.0));\n",
            k->specular ? "v1.rgb + r0.rgb" : "r0.rgb");
        return;
    }
    put(b, "  {\n    vec4 C0 = col(pc.psc[16]), C1 = col(pc.psc[17]);\n");
    put(b, "    vec3 E = clamp(");
    in_expr(b, (efg >> 24) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec3 F = clamp(");
    in_expr(b, (efg >> 16) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec4 efp = vec4(E * F, 0.0);\n");
    put(b, "    vec4 v1r0 = vec4(%s + %s, 0.0);\n",
        (efg & 0x40) ? "(1.0 - v1.rgb)" : "v1.rgb", (efg & 0x20) ? "(1.0 - r0.rgb)" : "r0.rgb");
    if (efg & 0x80) {
        put(b, "    v1r0 = clamp(v1r0, 0.0, 1.0);\n");
    }
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        put(b, "    vec3 %c = clamp(", k_v[i]);
        in_expr(b, (abcd >> (24 - 8 * i)) & 0xFF, false, true);
        put(b, ", 0.0, 1.0);\n");
    }
    put(b, "    float G = clamp(");
    in_expr(b, (efg >> 8) & 0xFF, true, true);
    put(b, ", 0.0, 1.0);\n"
           "    outc = vec4(A * B + (1.0 - A) * C + D, G);\n  }\n");
}

static const char *k_cmp[8] = {
    "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", "true",
};

/* Shadow buffer at stage s: the stored depth compared with the r coordinate
 * (depth FUNC r, both in the format's range; PROJECT2D compares with 0),
 * the four results bilinearly weighted (percentage-closer filtering). */
static void shadow_sample(Buf *b, const HlePshKey *k, unsigned s, bool project3d)
{
    static const char *const k_max[4] = { "1.0", "16777215.0", "65535.0", "511.9375" };
    static const char *const k_op[8] = { "", "lessThan", "equal", "lessThanEqual", "greaterThan",
                                         "notEqual", "greaterThanEqual", "" };
    unsigned func = (k->shadow >> 4) & 7, fmt = (k->shadow_fmt >> (2 * s)) & 3;
    if (func == 0 || func == 7) {
        put(b, "  vec4 t%u = vec4(%s);\n", s, func ? "1.0" : "0.0");
        return;
    }
    const char *r = project3d ? "clamp(pq.z, 0.0, m)" : "0.0";
    put(b, "  vec4 t%u;\n  { uint ti = pc.tex[%u] & 0xFFFFu, si = pc.tex[%u] >> 16;\n"
           "    vec3 pq = nvproj(vTex%u);\n"
           "    vec2 uv = pq.xy;\n"
           "    vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
           "    if ((pc.flags & (1u << %uu)) != 0u)\n"
           /* a scaled surface's texel coordinates: sz / the resolution scale */
           "      uv /= sz / ((pc.flags & (256u << %uu)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
           "    float m = %s, r = %s;\n"
           "    vec4 d = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], "
           "uSmp[nonuniformEXT(si)]), uv) * m;\n"
           "    vec4 p = vec4(%s(d, vec4(r)));\n"
           "    vec2 f = fract(uv * sz - 0.5);\n"
           "    t%u = vec4(mix(mix(p.w, p.z, f.x), mix(p.x, p.y, f.x), f.y));\n",
        s, s, s, s, s, s, k_max[fmt], r, k_op[func], s);
    if (k->debug == 17) {               /* debug view 17: stored depth, r, result */
        put(b, "    shdbg = vec4(d.x / m, r / m, t%u.x, 1.0);\n", s);
    }
    put(b, "  }\n");
}

/* D3DTSS_ALPHAKILL: a fetched texel with alpha 0 discards the fragment. A
 * shadow-buffer stage's result is its alpha too: projected shadow receivers
 * (PGR2's car shadow) draw only where the compare passed. */
static void alpha_kill(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    if (((k->alpha_kill >> s) & 1) && m != HLE_PST_NONE && m != HLE_PST_PASSTHRU &&
        m != HLE_PST_CLIPPLANE && m != HLE_PST_DOTPRODUCT) {
        put(b, "  if (t%u.a == 0.0) discard;\n", s);
    }
}

size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported)
{
    Buf b = { buf, cap, 0, false };
    const char *interp = k->flat ? "flat " : "";
    unsigned unsup = 0;
    put(&b, "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n%s",
        hle_push_glsl());
    put(&b, "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
            "layout(set = 0, binding = 1) uniform sampler uSmp[];\n"
            "layout(set = 0, binding = 2) uniform textureCube uTexCube[];\n"
            "layout(set = 0, binding = 3) uniform texture3D uTex3D[];\n"
            "layout(location = 0) %sin vec4 vDiffuse;\n"
            "layout(location = 1) %sin vec4 vSpecular;\n"
            "layout(location = 2) in vec4 vTex0;\n"
            "layout(location = 3) in vec4 vTex1;\n"
            "layout(location = 4) in vec4 vTex2;\n"
            "layout(location = 5) in vec4 vTex3i;\n"
            "layout(location = 6) in float vFog;\n"
            "%s%s%s"
            "layout(location = 0) out vec4 oColor;\n"
            "vec4 col(uint c) { return unpackUnorm4x8(c).zyxw; }\n"
            HLE_NVPROJ_GLSL
            /* Xbox linear (non-swizzled) textures use texel coordinates */
            "vec4 samp(uint s, vec2 uv) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
            "  if ((pc.flags & (1u << s)) != 0u)\n"
            "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
            "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n"
            "}\n"
            "vec4 samp3(uint s, vec3 uvw) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
            "  return texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
            "uvw);\n"
            "}\n"
            "vec4 sampc(uint s, vec3 dir) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = pc.tex[s] >> 16;\n"
            "  return texture(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
            "dir);\n"
            "}\n"
            /* PSDotMapping of a texture color to a vector */
            "vec3 dmap(uint m, vec4 c) {\n"
            "  if (m == 0u) return c.rgb;\n"
            "  if (m == 3u) { vec3 v = floor(c.rgb * 255.0 + 0.5);\n"
            "    return mix(v, v - 256.0, greaterThanEqual(v, vec3(128.0))) / 127.0; }\n"
            "  if (m >= 4u) { vec2 h = c.rg * 2.0 - 1.0;\n"
            "    return vec3(h, m == 4u ? 1.0 : sqrt(max(1.0 - dot(h, h), 0.0))); }\n"
            "  return c.rgb * 2.0 - 1.0;\n"
            "}\n"
            /* bytes as two's complement over 127 (bump offsets) */
            "vec2 sgn8(vec2 c) {\n"
            "  vec2 v = floor(c * 255.0 + 0.5);\n"
            "  return mix(v, v - 256.0, greaterThanEqual(v, vec2(128.0))) / 127.0;\n"
            "}\n"
            "void main() {\n"
            "  vec4 v0 = vDiffuse, v1 = vSpecular;\n"
            "  vec4 vTex3 = %s;\n", interp, interp, k->wbuf ? HLE_WBUF_IN_GLSL : "",
        (k->fbfetch & ~HLE_FB_ZEQ) || k->sblend ?
            "layout(input_attachment_index = 0, set = 2, binding = 0) "
            "uniform subpassInput uFb;\n" : "",
        k->fbfetch & HLE_FB_ZEQ ? HLE_ZEQ_DECL_GLSL : "",
        k->point_sprite ? "vec4(gl_PointCoord, 1.0, 1.0)" : "vTex3i");
    if (k->wbuf) {
        put(&b, HLE_WBUF_CALC_GLSL);
    }
    /* the table modes' distance: signed, as D3D's fog modes (LTCG 5849
     * 0x5421d0: 0x2601 linear, 0x800 / 0x801 exp, never the _ABS ones);
     * Conker's menu room fogs by a program's oFog that is negative */
    const char *fd = k->fog_abs ? "abs(vFog)" : "vFog";
    switch (k->fog) {
    case 0:  put(&b, "  float fogf = 1.0;\n"); break;
    /* vertex fog: a NaN factor (D3D's linear scale / bias are -inf, NaN when
     * FOGSTART == FOGEND: Otogi's characters) is 1, unfogged, as the NV2A
     * shows them (its min / max keep the number) */
    case 1:  put(&b, "  float fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"); break;
    /* the table modes too: the NV2A's linear mode is the same hardware
     * (push-buffer mode decodes D3D's vertex fog as linear, p0 1 / p1 1), so
     * Otogi 2's NaN fog coordinates turned every surface the fog colour */
    case 2:  put(&b, "  float fogf = exp(-%s * pc.fog.z);\n", fd); break;
    case 3:  put(&b, "  float fd = %s * pc.fog.z;\n"
                     "  float fogf = exp(-fd * fd);\n", fd); break;
    default: put(&b, "  float fogf = (pc.fog.y - %s) / (pc.fog.y - pc.fog.x);\n", fd); break;
    }
    if (k->fog >= 2) {
        put(&b, "  fogf = isnan(fogf) ? 1.0 : clamp(fogf, 0.0, 1.0);\n");
    }
    put(&b, "  vec4 fogr = vec4(col(pc.fog_color).rgb, fogf);\n"
            "  float dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n%s",
        k->debug == 17 ? "  vec4 shdbg = vec4(0.0, 0.0, 0.0, 1.0);\n" : "");
    for (unsigned s = 0; s < 4; s++) {
        unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        /* dot-product stages: the texture coordinates dotted with the
         * stage's input texture (PSInputTexture) through PSDotMapping */
        unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
        unsigned dm = s ? (k->dot_map >> (4 * (s - 1))) & 7 : 0;
        bool dotmode = s && (m == HLE_PST_DOTPRODUCT || (m >= HLE_PST_DOT_ST &&
                                                         m <= HLE_PST_DOT_STR_CUBE) ||
                             m == HLE_PST_DOT_RFLCT_SPEC_CONST);
        if (dotmode) {
            put(&b, "  dot%u = dot(vTex%u.xyz, dmap(%uu, t%u));\n", s, s, dm, in);
        }
        if (s == 2 && m == HLE_PST_DOT_RFLCT_DIFF) {
            /* the diffuse lookup needs stage 3's dot, computed here first */
            unsigned in3 = (k->input_tex >> 20) & 3, dm3 = (k->dot_map >> 8) & 7;
            put(&b, "  dot3 = dot(vTex3.xyz, dmap(%uu, t%u));\n", dm3, in3);
        }
        if ((k->fbfetch >> s) & 1) {
            /* the target drawn into: this pixel as the earlier draws left it
             * (an 8-bit target holds blue: back to its colour) */
            put(&b, "  vec4 t%u = subpassLoad(uFb)%s;\n", s, k->out_bgra ? ".bgra" : "");
            alpha_kill(&b, k, s, m);
            continue;
        }
        if (((k->shadow >> s) & 1) && (m == HLE_PST_PROJECT2D || m == HLE_PST_PROJECT3D)) {
            shadow_sample(&b, k, s, m == HLE_PST_PROJECT3D);
            alpha_kill(&b, k, s, m);
            continue;
        }
        switch (m) {
        case HLE_PST_NONE:
            /* a stage with no texture reads opaque black, as an unbound D3D
             * texture samples: Voodoo Vince draws its cutscene letterbox
             * as t0 * v0 (black diffuse, SRCALPHA blending) with nothing on
             * stage 0; with t0.a 0 the bars blended to nothing */
            put(&b, "  vec4 t%u = vec4(0.0, 0.0, 0.0, 1.0);\n", s);
            break;
        case HLE_PST_PASSTHRU:
            put(&b, "  vec4 t%u = clamp(vTex%u, 0.0, 1.0);\n", s, s);
            break;
        case HLE_PST_CLIPPLANE:
            put(&b, "  vec4 t%u = vec4(0.0);\n", s);
            for (unsigned c = 0; c < 4; c++) {
                put(&b, "  if (vTex%u.%c %s 0.0) discard;\n", s, "xyzw"[c],
                    ((k->compare >> (4 * s + c)) & 1) ? ">=" : "<");
            }
            break;
        case HLE_PST_PROJECT2D:
            put(&b, (k->vol >> s) & 1 ? "  vec4 t%u = samp3(%uu, vec3(nvproj(vTex%u).xy, 0.0));\n"
                                      : "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy);\n", s, s, s);
            break;
        case HLE_PST_PROJECT3D:
            put(&b, "  vec4 t%u = samp3(%uu, nvproj(vTex%u));\n", s, s, s);
            break;
        case HLE_PST_CUBEMAP:
            put(&b, "  vec4 t%u = sampc(%uu, vTex%u.xyz);\n", s, s, s);
            break;
        case HLE_PST_BUMPENVMAP:
        case HLE_PST_BUMPENVMAP_LUM: {
            /* offset texture: the input stage's (b, g) as signed bytes (du,
             * dv) through the bump matrix (column-major, as the NV2A takes
             * BUMPENVMAT 00, 01, 10, 11) move this stage's coordinates; LUM
             * scales the RGB by LSCALE * the input's r + LOFFSET */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            static const char *const mat[4][2] = {
                { "", "" }, { "pc.bump.x", "pc.bump.y" }, { "pc.bump.z", "pc.bump.w" },
                { "pc.bump3.x", "pc.bump3.y" } };
            put(&b, "  vec4 t%u;\n"
                    "  { vec2 d = mat2(unpackHalf2x16(%s), unpackHalf2x16(%s)) * "
                    "sgn8(t%u.bg);\n", s, mat[s][0], mat[s][1], in);
            if ((k->vol >> s) & 1) {            /* a volume: r passes through (Conker's
                                                 * electricity, animated along r) */
                put(&b, "    t%u = samp3(%uu, vec3(nvproj(vTex%u).xy + d, vTex%u.z));\n",
                    s, s, s, s);
            } else {
                put(&b, "    t%u = samp(%uu, nvproj(vTex%u).xy + d);\n", s, s, s);
            }
            if (m == HLE_PST_BUMPENVMAP_LUM) {
                put(&b, "    vec2 l = unpackHalf2x16(pc.bumpl[%u]);\n"
                        "    t%u.rgb *= clamp(l.x * t%u.r + l.y, 0.0, 1.0);\n", s - 1, s, in);
            }
            put(&b, "  }\n");
            break;
        }
        case HLE_PST_DPNDNT_AR:
        case HLE_PST_DPNDNT_GB: {
            /* dependent read: the input stage's (a, r) or (g, b), colours in
             * 0..1, are this stage's 2D coordinates, normalised whatever the
             * texture's layout. Mercenaries' sky dome looks its fade up by
             * the scene depth's top bytes (the depth read as A8R8G8B8): at
             * its own coordinates (none written) the fade was 0 and the sky
             * black. */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            put(&b, "  vec4 t%u;\n"
                    "  { uint ti = pc.tex[%u] & 0xFFFFu, si = pc.tex[%u] >> 16;\n"
                    "    t%u = texture(sampler2D(uTex2D[nonuniformEXT(ti)], "
                    "uSmp[nonuniformEXT(si)]), t%u.%s); }\n",
                s, s, s, s, in, m == HLE_PST_DPNDNT_AR ? "ar" : "gb");
            break;
        }
        case HLE_PST_DOT_STR_3D:                    /* stage 3: (dot1, dot2, dot3) */
            put(&b, "  vec4 t%u = samp3(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOTPRODUCT:
            put(&b, "  vec4 t%u = vec4(0.0);\n", s);
            break;
        case HLE_PST_DOT_ST:
            /* the previous stage's dot and this one's: (dot1, dot2) on stage
             * 2, (dot2, dot3) on stage 3 (texm3x2pad / texm3x2tex on stages 2
             * and 3). Read at (dot1, dot2) on stage 3, Splinter Cell Chaos
             * Theory's thermal vision (stage 2 DOTPRODUCT, stage 3 DOT_ST over
             * the heat image, a shimmer by a noise map) took one column of
             * the image at a height set by one dot: vertical stripes */
            put(&b, "  vec4 t%u = samp(%uu, vec2(dot%u, dot%u));\n", s, s, s - 1, s);
            break;
        case HLE_PST_DOT_STR_CUBE:                  /* stage 3: N */
            put(&b, "  vec4 t%u = sampc(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOT_RFLCT_DIFF:                /* stage 2: N */
            put(&b, "  vec4 t%u = sampc(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOT_RFLCT_SPEC:                /* stage 3: N reflected about E */
        case HLE_PST_DOT_RFLCT_SPEC_CONST:          /* (E = the eye vector: q coordinates) */
            put(&b, "  vec4 t%u;\n  { vec3 n = vec3(dot1, dot2, dot3), "
                    "e = vec3(vTex1.w, vTex2.w, vTex3.w);\n"
                    "    t%u = sampc(%uu, 2.0 * n * dot(n, e) / max(dot(n, n), 1e-8) - e); }\n",
                s, s, s);
            if (m == HLE_PST_DOT_RFLCT_SPEC_CONST) {
                unsup |= 1u << m;
            }
            break;
        default:                        /* volume, BRDF, DOT_ZW, dependent AR/GB */
            unsup |= 1u << m;
            put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy); /* mode %u */\n",
                s, s, s, m);
            break;
        }
        alpha_kill(&b, k, s, m);
    }
    put(&b, "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n"
            "  vec4 t0s = t0, t3s = t3;\n");      /* as sampled (debug views 15, 16) */
    unsigned n = k->count & 0xFF;
    for (unsigned s = 0; s < n && s < 8; s++) {
        stage(&b, k, s);
    }
    final_combiner(&b, k);
    put(&b, "  outc = clamp(outc, 0.0, 1.0);\n");
    if (k->alpha_test && k->alpha_func < 8) {
        put(&b, "  { float a = outc.a, r = pc.alpha_ref; if (!(%s)) discard; }\n",
            k_cmp[k->alpha_func]);
    }
    if (k->fbfetch & HLE_FB_ZEQ) {
        put(&b, "%s", HLE_ZEQ_TEST_GLSL(k->fbfetch));
    }
    static const char *const k_dbg[17] = {
        "outc", "t0", "v0", "r0", "vec4(vec3(fogf), 1.0)", "v1", "t3", "vec4(vec3(r0.a), 1.0)",
        "t2",
        /* 16-pixel bands: stage 1 (a shadow compare), its r / 65535 */
        ("((int(gl_FragCoord.y) & 16) == 0 ? t1 : "
         "vec4(vec3(clamp(vTex1.z / vTex1.w / 65535.0, 0.0, 1.0)), 1.0))"),
        "vec4(vec3(vSpecular.a), 1.0)", "vec4(vec3(t0.a), 1.0)", "vec4(vec3(t3.a), 1.0)",
        "vec4(fract(vTex3.xy / vTex3.w), 0.0, 1.0)", "vec4(clamp(abs(vTex3.xyz) * 0.25, 0.0, 1.0), 1.0)",
        "t3s", "t0s" };
    /* 17: shadow-buffer draws show (stored depth, r, result) / range, others as is */
    const char *dbg = k->debug < 17 ? k_dbg[k->debug] :
                      k->debug == 17 && (k->shadow & 15) ? "shdbg" :
                      k->debug == 18 ? "vec4(1.0, 0.0, 1.0, 1.0)" :
                      k->debug == 19 ? "t1" : k->debug == 20 ? "r1" :
                      /* stage 2's coordinates: |str| / 4, direction, NaN / Inf, and
                       * its cube sampled along a fixed direction */
                      k->debug == 21 ? "vec4(clamp(abs(vTex2.xyz) * 0.25, 0.0, 1.0), 1.0)" :
                      k->debug == 22 ? "vec4(normalize(vTex2.xyz) * 0.5 + 0.5, 1.0)" :
                      k->debug == 23 ? "vec4(any(isnan(vTex2)) ? 1.0 : 0.0, "
                                       "any(isinf(vTex2)) ? 1.0 : 0.0, 0.0, 1.0)" :
                      k->debug == 24 ? "sampc(2u, vec3(0.3, 0.5, 0.8))" : "outc";
    put(&b, "  oColor = %s;\n%s", dbg, k->out_bgra ? "  oColor = oColor.bgra;\n" : "");
    if (k->sblend) {
        put(&b, "  { ivec4 sv = ivec4(floor(clamp(%s, 0.0, 1.0) * 255.0 + 0.5));\n"
        "    ivec4 dv = ivec4(floor(clamp(subpassLoad(uFb), 0.0, 1.0) * 255.0 + 0.5));\n"
        "    sv -= 256 * (sv >> 7); dv -= 256 * (dv >> 7);\n"
        "    ivec4 r = clamp(dv %s sv, ivec4(-128), ivec4(127));\n"
        "    oColor = vec4(r & 255) / 255.0; }\n", "oColor", k->sblend == 1 ? "+" : "-");
    }
    put(&b, "%s}\n", k->wbuf ? HLE_WBUF_STORE_GLSL : "");
    if (unsupported) {
        *unsupported = unsup;
    }
    return b.overflow ? 0 : b.len;
}
