/*
 * hakuX HLE GPU — fixed-function GLSL generators (pure). See ff.h.
 * - VS: XYZRHW (screen space) and transformed positions; D3D8 lighting
 *   (directional, point, spot; material colour sources, specular, local
 *   viewer), vertex blending (up to 3 weights), texgen (camera-space
 *   normal / position / reflection, sphere map) and texture transforms
 *   (count, projected) from the FF uniform block (ff.h HLE_FFU_*).
 * - PS: the texture-stage cascade (D3DTOP_*), alpha test, specular add.
 * NV2A semantics reference: xemu hw/xbox/nv2a/pgraph/glsl/vsh-ff.c, psh.c.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"

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

bool g_hle_push_ssbo;                   /* ff.h */
bool g_hle_const_ssbo;                  /* ff.h */
bool g_hle_glsl_zw;                     /* ff.h */
bool g_hle_glsl_zclamp;                 /* ff.h */
bool g_hle_glsl_fixed;                  /* ff.h */

size_t hle_glsl_fixed_slots(char *src, size_t cap)
{
    _Static_assert(HLE_FF_STAGES == 4, "the sizes below");
    static const struct { const char *from, *to; } k_rw[] = {
        { "#extension GL_EXT_nonuniform_qualifier : require\n", "" },
        { "nonuniformEXT(", "(" },
        { "uTex2D[];", "uTex2D[4];" }, { "uTexCube[];", "uTexCube[4];" },
        { "uTex3D[];", "uTex3D[4];" }, { "uSmp[];", "uSmp[5];" },
    };
    const size_t nrw = sizeof(k_rw) / sizeof(k_rw[0]);
    size_t n = strlen(src), o = 0;
    char *out = malloc(cap);
    if (!out) {
        return 0;
    }
    for (size_t i = 0; i < n;) {
        size_t j = 0;
        while (j < nrw && strncmp(src + i, k_rw[j].from, strlen(k_rw[j].from))) {
            j++;
        }
        const char *rep = j < nrw ? k_rw[j].to : src + i;
        size_t len = j < nrw ? strlen(k_rw[j].to) : 1;
        if (o + len >= cap) {
            free(out);
            return 0;
        }
        memcpy(out + o, rep, len);
        o += len;
        i += j < nrw ? strlen(k_rw[j].from) : 1;
    }
    out[o] = 0;
    memcpy(src, out, o + 1);
    free(out);
    return o;
}

/* Material colour source s (0 material, 1 vertex diffuse, 2 vertex specular)
 * for material slot m. Without COLORVERTEX or the vertex colour, the
 * material. */
static const char *mat_src(const HleFfVsKey *k, unsigned slot, unsigned m, char *tmp, size_t n)
{
    unsigned s = k->colorvertex ? k->src[slot] : 0;
    if (s == 1 && (k->fvf & HLE_FVF_DIFFUSE)) {
        return "iDiffuse";
    }
    if (s == 2 && (k->fvf & HLE_FVF_SPECULAR)) {
        return "iSpecular";
    }
    snprintf(tmp, n, "f[%u]", HLE_FFU_MATERIAL + m);
    return tmp;
}

size_t hle_ff_vs_glsl(const HleFfVsKey *k, char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    uint32_t pos = k->fvf & HLE_FVF_POSITION_MASK;
    unsigned ntc = (k->fvf & HLE_FVF_TEXCOUNT_MASK) >> HLE_FVF_TEXCOUNT_SHIFT;
    const char *interp = k->flat ? "flat " : "";
    /* pre-transformed vertices read the block only for the point size */
    bool ubo = hle_ff_vs_needs_ubo(k) && (pos != HLE_FVF_XYZRHW || k->points);
    bool xf = ubo && pos != HLE_FVF_XYZRHW;
    bool normal = (k->fvf & HLE_FVF_NORMAL) != 0;
    put(&b, "#version 450\n%s", hle_push_glsl());
    if (ubo) {
        put(&b, "layout(set = 1, binding = 0, std140) %s FfConst { vec4 f[%u]; };\n",
            hle_const_glsl(), HLE_FFU_COUNT);
    }
    put(&b, "layout(location = 0) in vec4 iPos;\n");
    if (normal) {
        put(&b, "layout(location = 1) in %s;\n", k->cmp_normal ? "uvec2 iNormalP" : "vec3 iNormal");
    }
    if (k->fvf & HLE_FVF_DIFFUSE) {
        put(&b, "layout(location = 2) in vec4 iDiffuse;\n");
    }
    if (k->fvf & HLE_FVF_SPECULAR) {
        put(&b, "layout(location = 3) in vec4 iSpecular;\n");
    }
    for (unsigned i = 0; i < ntc && i < HLE_FF_STAGES; i++) {
        put(&b, "layout(location = %u) in vec4 iTex%u;\n", 4 + i, i);
    }
    if (xf && k->blend) {
        put(&b, "layout(location = 8) in vec4 iWeight;\n");
    }
    if (k->points && (k->fvf & HLE_FVF_PSIZE)) {
        put(&b, "layout(location = 9) in float iPSize;\n");
    }
    put(&b, "layout(location = 0) %sout vec4 vDiffuse;\n", interp);
    put(&b, "layout(location = 1) %sout vec4 vSpecular;\n", interp);
    for (unsigned s = 0; s < k->ntex && s < HLE_FF_STAGES; s++) {
        put(&b, "layout(location = %u) out vec4 vTex%u;\n", 2 + s, s);
    }
    put(&b, "layout(location = 6) out float vFog;\n");
    if (k->wbuf) {      /* W-buffering: the clip w for the pixel stage's depth */
        put(&b, "layout(location = 9) out float vW;\n");
    }
    if (xf) {
        put(&b, "mat4 m4(int i) { return mat4(f[i], f[i + 1], f[i + 2], f[i + 3]); }\n");
    }
    put(&b, "void main() {\n");
    if (normal && k->cmp_normal) {
        /* CMP: x, y 11 bits, z 10 bits, signed normalized (the dword is
         * often only 2-byte aligned: two 16-bit halves) */
        put(&b, "  int nP = int(iNormalP.x | (iNormalP.y << 16u));\n"
                "  vec3 iNormal = vec3(bitfieldExtract(nP, 0, 11), bitfieldExtract(nP, 11, 11),\n"
                "                      bitfieldExtract(nP, 22, 10)) / vec3(1023.0, 1023.0, 511.0);\n");
    }
    if (pos == HLE_FVF_XYZRHW) {
        /* screen space: x, y in target pixels, z in [0, 1], w = 1 / rhw; to
         * D3D screen pixels (supersample scale, screen-space offset), placed
         * as the NV2A rasterizer does (ff.h) */
        put(&b, "  float w = (iPos.w != 0.0) ? 1.0 / iPos.w : 1.0;\n"
                "  vec2 scr = iPos.xy * pc.scr.xy + pc.scr.zw;\n"
                HLE_SUBPIXEL_GLSL("scr")
                HLE_TEXEL_EDGE_GLSL("scr")
                "  vec2 ndc = scr * pc.vp + vec2(-1.0, 1.0);\n"
                "  gl_Position = vec4(ndc * w, iPos.z * w, w);\n"
                "  vec3 ePos = vec3(0.0), eN = vec3(0.0, 0.0, -1.0);\n");
    } else if (!xf) {
        put(&b, "  gl_Position = pc.mvp * vec4(iPos.xyz, 1.0);\n"
                "  vec3 ePos = vec3(0.0, 0.0, gl_Position.w), eN = vec3(0.0, 0.0, -1.0);\n");
    } else {
        /* eye space: WV (and the blend matrices) */
        put(&b, "  vec4 p = vec4(iPos.xyz, 1.0);\n");
        put(&b, "  vec3 n = %s;\n", normal ? "iNormal" : "vec3(0.0, 0.0, 1.0)");
        if (k->blend) {
            /* NV2A skinning: matrix i (world i * view) weighted by the vertex's
             * weight i; in the mix modes the last weight is 1 - the others */
            put(&b, "  float wl = 1.0;\n  vec4 eP4 = vec4(0.0);\n  vec3 en = vec3(0.0);\n");
            for (unsigned i = 0; i < k->blend && i < 4; i++) {
                char mbuf[24];
                snprintf(mbuf, sizeof(mbuf), "m4(%u)",
                         i ? HLE_FFU_BLEND + 4 * (i - 1) : HLE_FFU_WV);
                if (k->blend_mix && i == k->blend - 1u) {
                    put(&b, "  eP4 += wl * (%s * p); en += wl * mat3(%s) * n;\n", mbuf, mbuf);
                } else {
                    put(&b, "  { float wi = iWeight[%u]; wl -= wi;\n"
                            "    eP4 += wi * (%s * p); en += wi * mat3(%s) * n; }\n",
                        i, mbuf, mbuf);
                }
            }
            put(&b, "  vec3 ePos = eP4.xyz;\n  vec3 eN = en;\n");
        } else {
            put(&b, "  vec3 ePos = (m4(%u) * p).xyz;\n"
                    "  vec3 eN = mat3(f[%u].xyz, f[%u].xyz, f[%u].xyz) * n;\n",
                HLE_FFU_WV, HLE_FFU_NORMAL, HLE_FFU_NORMAL + 1, HLE_FFU_NORMAL + 2);
        }
        if (k->normalize) {
            put(&b, "  eN = normalize(eN);\n");
        }
        put(&b, "  gl_Position = m4(%u) * vec4(ePos, 1.0);\n", HLE_FFU_PROJ);
    }
    /* point size: POINTSIZE or the vertex PSIZE, scaled by the eye distance
     * (D3D8: viewport height * sqrt(1 / (A + B d + C d^2))), clamped, in
     * target pixels */
    if (k->points) {
        unsigned P = HLE_FFU_POINT;
        put(&b, "  { float ps = %s;\n", (k->fvf & HLE_FVF_PSIZE) ? "iPSize" : "f[104].x");
        if (k->points == 2 && xf) {
            put(&b, "    float d = length(ePos);\n"
                    "    ps *= f[%u].w * inversesqrt(max(f[%u].x + f[%u].y * d + "
                    "f[%u].z * d * d, 1e-12));\n", P, P + 1, P + 1, P + 1);
        }
        put(&b, "    gl_PointSize = clamp(ps, f[%u].y, f[%u].z) * f[%u].w *\n"
                "                   float(max((pc.flags >> 16) & 255u, 1u)); }\n", P, P, P + 1);
    }
    /* lighting (D3D8 fixed function) */
    if (xf && k->lighting) {
        char t0[24], t1[24], t2[24], t3[24];
        const char *cd = mat_src(k, 0, 0, t0, sizeof(t0));      /* diffuse */
        const char *ca = mat_src(k, 1, 1, t1, sizeof(t1));      /* ambient */
        const char *cs = mat_src(k, 2, 2, t2, sizeof(t2));      /* specular */
        const char *ce = mat_src(k, 3, 3, t3, sizeof(t3));      /* emissive */
        put(&b, "  vec3 E = %s;\n", k->local_viewer ? "normalize(-ePos)" : "vec3(0.0, 0.0, -1.0)");
        put(&b, "  vec3 dsum = vec3(0.0), ssum = vec3(0.0), asum = vec3(0.0);\n");
        for (unsigned i = 0; i < k->nlights && i < HLE_FF_LIGHTS; i++) {
            unsigned L = HLE_FFU_LIGHTS + HLE_FFU_LIGHT_VEC4 * i;
            put(&b, "  { /* light %u */\n", i);
            if (k->light_type[i] == HLE_LIGHT_DIRECTIONAL) {
                put(&b, "    vec3 l = normalize(-f[%u].xyz);\n    float att = 1.0;\n", L + 4);
            } else {
                put(&b, "    vec3 d = f[%u].xyz - ePos;\n    float dist = length(d);\n"
                        "    vec3 l = d / max(dist, 1e-6);\n"
                        "    float att = dist > f[%u].x ? 0.0 :\n"
                        "      1.0 / max(f[%u].z + f[%u].w * dist + f[%u].x * dist * dist, 1e-6);\n",
                    L + 3, L + 5, L + 5, L + 5, L + 6);
                if (k->light_type[i] == HLE_LIGHT_SPOT) {
                    put(&b, "    float rho = dot(-l, normalize(f[%u].xyz));\n"
                            "    float ct = f[%u].y, cp = f[%u].z;\n"
                            "    att *= rho > ct ? 1.0 : rho <= cp ? 0.0 :\n"
                            "      pow(clamp((rho - cp) / max(ct - cp, 1e-6), 0.0, 1.0), f[%u].y);\n",
                        L + 4, L + 6, L + 6, L + 5);
                }
            }
            /* D3D8: vertices without a normal light with a dot product of 0
             * (ambient and emissive only; Morrowind's shadow volumes take
             * their alpha 0 from the material) */
            put(&b, "    float nl = %s;\n"
                    "    asum += f[%u].rgb * att;\n"
                    "    dsum += f[%u].rgb * nl * att;\n",
                normal ? "max(dot(eN, l), 0.0)" : "0.0", L + 2, L);
            if (k->specular) {
                put(&b, "    if (nl > 0.0) {\n"
                        "      float nh = max(dot(eN, normalize(l + E)), 0.0);\n"
                        "      ssum += f[%u].rgb * pow(nh, f[%u].x) * att;\n    }\n",
                    L + 1, HLE_FFU_MATERIAL + 4);
            }
            put(&b, "  }\n");
        }
        put(&b, "  vDiffuse = clamp(vec4(%s.rgb + f[%u].rgb * %s.rgb + asum * %s.rgb + "
                "dsum * %s.rgb, %s.a), 0.0, 1.0);\n",
            ce, HLE_FFU_AMBIENT, ca, ca, cd, cd);
        if (k->specular) {
            put(&b, "  vSpecular = clamp(vec4(ssum * %s.rgb, %s.a), 0.0, 1.0);\n", cs, cs);
        } else {
            put(&b, "  vSpecular = vec4(0.0);\n");
        }
    } else {
        put(&b, "  vDiffuse = %s;\n",
            (k->fvf & HLE_FVF_DIFFUSE) ? "iDiffuse" : "vec4(1.0)");
        put(&b, "  vSpecular = %s;\n",
            (k->fvf & HLE_FVF_SPECULAR) ? "iSpecular" : "vec4(0.0)");
    }
    /* fog coordinate: the fog factor (specular alpha) or the distance the
     * pixel side turns into one; pre-transformed vertices: their w */
    const char *spec_a = (k->fvf & HLE_FVF_SPECULAR) ? "iSpecular.a" : "1.0";
    const char *fog;
    if (k->fog_src == HLE_FOG_SRC_SPEC_ALPHA) {
        fog = spec_a;
    } else if (pos == HLE_FVF_XYZRHW) {
        fog = k->fog_src ? "w" : spec_a;
    } else {
        fog = k->fog_src == HLE_FOG_SRC_RADIAL && xf ? "length(ePos)" : "abs(ePos.z)";
    }
    put(&b, "  vFog = %s;\n", fog);
    for (unsigned s = 0; s < k->ntex && s < HLE_FF_STAGES; s++) {
        unsigned src = k->texcoord[s] & 7;
        unsigned g = xf ? k->texgen[s] : 0;
        put(&b, "  { vec4 t = ");
        if (g == HLE_TCI_CAMERANORMAL) {
            put(&b, "vec4(eN, 1.0)");
        } else if (g == HLE_TCI_CAMERAPOSITION) {
            put(&b, "vec4(ePos, 1.0)");
        } else if (g == HLE_TCI_CAMERAREFLECTION) {
            put(&b, "vec4(reflect(normalize(ePos), normalize(eN)), 1.0)");
        } else if (g == HLE_TCI_SPHEREMAP) {
            put(&b, "vec4(0.0, 0.0, 0.0, 1.0); { vec3 r = reflect(normalize(ePos), normalize(eN));\n"
                    "    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n"
                    "    t.xy = r.xy / max(m, 1e-6) + 0.5; }");
        } else if (src < ntc) {
            unsigned n = hle_fvf_tex_size(k->fvf, src);
            const char *sw = n == 1 ? "vec4(iTex%u.x, 0.0, 0.0, 1.0)" :
                             n == 2 ? "vec4(iTex%u.xy, 0.0, 1.0)" :
                             n == 3 ? "vec4(iTex%u.xyz, 1.0)" : "iTex%u";
            put(&b, sw, src);
        } else {
            put(&b, "vec4(0.0, 0.0, 0.0, 1.0)");
        }
        put(&b, ";\n");
        unsigned tx = xf ? k->texxform[s] : 0;
        if (tx & 0xF) {
            /* D3DTTFF_COUNTn: n coordinates out; PROJECTED: divided by the last */
            put(&b, "    t = m4(%u) * t;\n", HLE_FFU_TEXMAT + 4 * s);
            unsigned cnt = tx & 0xF;
            if (tx & 0x10) {
                static const char k_last[5] = { 'w', 'x', 'y', 'z', 'w' };
                put(&b, "    t = vec4(t.xyz, t.%c);\n", k_last[cnt < 5 ? cnt : 4]);
            } else if (cnt < 4) {
                put(&b, "    t.w = 1.0;\n");
            }
        }
        put(&b, "    vTex%u = t; }\n", s);
    }
    if (pos != HLE_FVF_XYZRHW) {
        /* the viewport transform here (the Vulkan viewport is the whole
         * target): NDC -> D3D screen pixels with the screen-space offset, the
         * rasterizer's subpixel placement (ff.h), -> the target's NDC (in clip
         * space) */
        put(&b, "  { vec4 c = gl_Position;\n"
                "    vec2 s = c.xy * pc.scr.xy + pc.scr.zw * c.w;\n"
                "    if (abs(c.w) > 1e-20) { vec2 q = s / c.w;\n"
                HLE_SUBPIXEL_GLSL("q")
                HLE_TEXEL_EDGE_GLSL("q")
                "      s = q * c.w; }\n"
                "    gl_Position = vec4(s * pc.vp + vec2(-1.0, 1.0) * c.w, c.z, c.w); }\n");
    }
    if (k->zclamp && g_hle_glsl_zclamp) {
        put(&b, HLE_ZCLAMP_GLSL);
    }
    if (g_hle_glsl_zw) {
        put(&b, HLE_ZW_NUDGE_GLSL);
    }
    put(&b, "%s}\n", k->wbuf ? "  vW = gl_Position.w;\n" : "");
    return b.overflow ? 0 : b.len;
}

/* ---- pixel side -------------------------------------------------------------- */

/* D3DTOP (Xbox) */
enum {
    TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X,
    TOP_MODULATE4X, TOP_ADD, TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH,
    TOP_BLENDDIFFUSEALPHA, TOP_BLENDCURRENTALPHA, TOP_BLENDTEXTUREALPHA,
    TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_PREMODULATE,
    TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA,
    TOP_MODULATEINVALPHA_ADDCOLOR, TOP_MODULATEINVCOLOR_ADDALPHA, TOP_DOTPRODUCT3,
    TOP_MULTIPLYADD, TOP_LERP, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE,
};

/* D3DTA (Xbox, same as D3D8) */
#define TA_SELECTMASK     0x0F
#define TA_DIFFUSE        0
#define TA_CURRENT        1
#define TA_TEXTURE        2
#define TA_TFACTOR        3
#define TA_SPECULAR       4
#define TA_TEMP           5
#define TA_COMPLEMENT     0x10
#define TA_ALPHAREPLICATE 0x20

static void arg(Buf *b, unsigned s, uint32_t a, bool alpha)
{
    char base[48];
    switch (a & TA_SELECTMASK) {
    case TA_DIFFUSE:  snprintf(base, sizeof(base), "vDiffuse"); break;
    case TA_CURRENT:  snprintf(base, sizeof(base), "%s", s ? "cur" : "vDiffuse"); break;
    case TA_TEXTURE:  snprintf(base, sizeof(base), "t%u", s); break;
    case TA_TFACTOR:  snprintf(base, sizeof(base), "unpackUnorm4x8(pc.tfactor).zyxw"); break;
    case TA_SPECULAR: snprintf(base, sizeof(base), "vSpecular"); break;
    case TA_TEMP:     snprintf(base, sizeof(base), "tmp"); break;
    default:          snprintf(base, sizeof(base), "vec4(0.0)"); break;
    }
    char v[96];
    if (a & TA_ALPHAREPLICATE) {
        snprintf(v, sizeof(v), "vec4(%s.a)", base);
    } else {
        snprintf(v, sizeof(v), "%s", base);
    }
    if (a & TA_COMPLEMENT) {
        put(b, "(vec4(1.0) - %s)", v);
    } else {
        put(b, "%s", v);
    }
    (void)alpha;
}

/* Expression for one D3DTOP over args a0/a1/a2 of stage s. */
static void op(Buf *b, unsigned s, uint32_t o, uint32_t a0, uint32_t a1, uint32_t a2,
               bool alpha)
{
#define A0 arg(b, s, a0, alpha)
#define A1 arg(b, s, a1, alpha)
#define A2 arg(b, s, a2, alpha)
    switch (o) {
    case TOP_SELECTARG1: A1; break;
    case TOP_SELECTARG2: A2; break;
    case TOP_MODULATE:   put(b, "("); A1; put(b, " * "); A2; put(b, ")"); break;
    case TOP_MODULATE2X: put(b, "(2.0 * "); A1; put(b, " * "); A2; put(b, ")"); break;
    case TOP_MODULATE4X: put(b, "(4.0 * "); A1; put(b, " * "); A2; put(b, ")"); break;
    case TOP_ADD:        put(b, "("); A1; put(b, " + "); A2; put(b, ")"); break;
    case TOP_ADDSIGNED:  put(b, "("); A1; put(b, " + "); A2; put(b, " - 0.5)"); break;
    case TOP_ADDSIGNED2X:
        put(b, "(2.0 * ("); A1; put(b, " + "); A2; put(b, " - 0.5))"); break;
    case TOP_SUBTRACT:   put(b, "("); A1; put(b, " - "); A2; put(b, ")"); break;
    case TOP_ADDSMOOTH:
        put(b, "("); A1; put(b, " + "); A2; put(b, " * (vec4(1.0) - "); A1; put(b, "))");
        break;
    case TOP_BLENDDIFFUSEALPHA:
        put(b, "mix("); A2; put(b, ", "); A1; put(b, ", vDiffuse.a)"); break;
    case TOP_BLENDCURRENTALPHA:
        put(b, "mix("); A2; put(b, ", "); A1; put(b, ", %s.a)", s ? "cur" : "vDiffuse");
        break;
    case TOP_BLENDTEXTUREALPHA:
        put(b, "mix("); A2; put(b, ", "); A1; put(b, ", t%u.a)", s); break;
    case TOP_BLENDFACTORALPHA:
        put(b, "mix("); A2; put(b, ", "); A1; put(b, ", unpackUnorm4x8(pc.tfactor).w)"); break;
    case TOP_BLENDTEXTUREALPHAPM:
        put(b, "("); A1; put(b, " + "); A2; put(b, " * (1.0 - t%u.a))", s); break;
    case TOP_PREMODULATE:
        A1; break;                     /* modulation applies at the next stage */
    case TOP_MODULATEALPHA_ADDCOLOR:
        put(b, "("); A1; put(b, " + "); A1; put(b, ".a * "); A2; put(b, ")"); break;
    case TOP_MODULATECOLOR_ADDALPHA:
        put(b, "("); A1; put(b, " * "); A2; put(b, " + "); A1; put(b, ".a)"); break;
    case TOP_MODULATEINVALPHA_ADDCOLOR:
        put(b, "("); A1; put(b, " + (1.0 - "); A1; put(b, ".a) * "); A2; put(b, ")"); break;
    case TOP_MODULATEINVCOLOR_ADDALPHA:
        put(b, "((vec4(1.0) - "); A1; put(b, ") * "); A2; put(b, " + "); A1; put(b, ".a)");
        break;
    case TOP_DOTPRODUCT3:
        put(b, "vec4(clamp(4.0 * dot(("); A1; put(b, ").rgb - 0.5, ("); A2;
        put(b, ").rgb - 0.5), 0.0, 1.0))");
        break;
    case TOP_MULTIPLYADD:
        put(b, "("); A0; put(b, " + "); A1; put(b, " * "); A2; put(b, ")"); break;
    case TOP_LERP:
        put(b, "mix("); A2; put(b, ", "); A1; put(b, ", "); A0; put(b, ")"); break;
    default:                            /* BUMPENVMAP*: P4 (texture modes) */
        put(b, "%s", s ? "cur" : "vDiffuse");
        break;
    }
#undef A0
#undef A1
#undef A2
}

/* Whether op o reads the stage's texture: an argument, or the texture alpha
 * BLENDTEXTUREALPHA(PM) blend by (PREMODULATE takes it in the first stage). */
static bool reads_tex(uint32_t o, uint32_t a0, uint32_t a1, uint32_t a2, bool first)
{
    bool t0 = (a0 & TA_SELECTMASK) == TA_TEXTURE, t1 = (a1 & TA_SELECTMASK) == TA_TEXTURE;
    bool t2 = (a2 & TA_SELECTMASK) == TA_TEXTURE;
    switch (o) {
    case TOP_DISABLE:
    case TOP_BUMPENVMAP:
    case TOP_BUMPENVMAPLUMINANCE:
        return false;
    case TOP_SELECTARG1:            return t1;
    case TOP_SELECTARG2:            return t2;
    case TOP_BLENDTEXTUREALPHA:
    case TOP_BLENDTEXTUREALPHAPM:   return true;
    case TOP_PREMODULATE:           return t1 || first;
    case TOP_MULTIPLYADD:
    case TOP_LERP:                  return t0 || t1 || t2;
    default:                        return t1 || t2;
    }
}

/* A stage without a texture whose op reads it: D3D's combiner compiler maps
 * D3DTA_TEXTURE to -1 there (Blinx 0x13f8e0: Textures[stage] NULL) and turns
 * a -1 input into CURRENT (diffuse in the first stage) times one, keeping
 * the op's output scale and bias (0x13fd66). */
static void passthrough(Buf *b, unsigned s, uint32_t o)
{
    const char *c = s ? "cur" : "vDiffuse";
    switch (o) {
    case TOP_MODULATE2X:  put(b, "(2.0 * %s)", c); break;
    case TOP_MODULATE4X:  put(b, "(4.0 * %s)", c); break;
    case TOP_ADDSIGNED:   put(b, "(%s - 0.5)", c); break;
    case TOP_ADDSIGNED2X: put(b, "(2.0 * (%s - 0.5))", c); break;
    default:              put(b, "%s", c); break;
    }
}

static const char *k_cmp[8] = {
    "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", "true",
};

size_t hle_ff_ps_glsl(const HleFfPsKey *k, char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    const char *interp = k->flat ? "flat " : "";
    put(&b, "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n%s", hle_push_glsl());
    put(&b, "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
            "layout(set = 0, binding = 1) uniform sampler uSmp[];\n");
    put(&b, "layout(location = 0) %sin vec4 vDiffuse;\n", interp);
    put(&b, "layout(location = 1) %sin vec4 vSpecular;\n", interp);
    for (unsigned s = 0; s < k->nstages; s++) {
        if (k->st[s].texture) {
            put(&b, "layout(location = %u) in vec4 vTex%u;\n", 2 + s, s);
        }
    }
    if (k->fog) {
        put(&b, "layout(location = 6) in float vFog;\n");
    }
    if (k->wbuf) {
        put(&b, HLE_WBUF_IN_GLSL);
    }
    put(&b, "layout(location = 0) out vec4 oColor;\n" HLE_NVPROJ_GLSL);
    if (k->sblend || (k->fbfetch & ~HLE_FB_ZEQ)) {
        put(&b, "layout(input_attachment_index = 0, set = 2, binding = 0) "
                "uniform subpassInput uFb;\n");
    }
    if (k->fbfetch & HLE_FB_ZEQ) {
        put(&b, HLE_ZEQ_DECL_GLSL);
    }
    put(&b, "void main() {\n  vec4 cur = vDiffuse, tmp = vec4(0.0), r;\n");
    if (k->wbuf) {
        put(&b, HLE_WBUF_CALC_GLSL);
    }
    for (unsigned s = 0; s < k->nstages; s++) {
        const HleFfStage *st = &k->st[s];
        if (st->texture && ((k->fbfetch >> s) & 1)) {
            /* the target drawn into: this pixel as the earlier draws left it
             * (an 8-bit target holds blue: back to its colour) */
            put(&b, "  vec4 t%u = subpassLoad(uFb)%s;\n", s, k->out_bgra ? ".bgra" : "");
            if (st->alpha_kill) {
                put(&b, "  if (t%u.a == 0.0) discard;\n", s);
            }
        } else if (st->texture) {
            /* Xbox linear (non-swizzled) textures use texel coordinates */
            if (s == 0 && k->point_sprite) {
                put(&b, "  vec2 uv0 = gl_PointCoord;\n");
            } else {
                put(&b, "  vec2 uv%u = nvproj(vTex%u).xy;\n"
                        "  if ((pc.flags & %uu) != 0u) uv%u /= vec2(textureSize(sampler2D("
                        "uTex2D[nonuniformEXT(pc.tex[%u] & 0xFFFFu)], uSmp[0]), 0)) /\n"
                        "      ((pc.flags & %uu) != 0u ? float(pc.flags >> 24) : 1.0);\n",
                    s, s, 1u << s, s, s, 256u << s);
            }
            put(&b, "  vec4 t%u = texture(sampler2D(uTex2D[nonuniformEXT(pc.tex[%u] & 0xFFFFu)], "
                    "uSmp[nonuniformEXT(pc.tex[%u] >> 16)]), uv%u);\n",
                s, s, s, s);
            if (st->alpha_kill) {               /* D3DTSS_ALPHAKILL */
                put(&b, "  if (t%u.a == 0.0) discard;\n", s);
            }
        } else {
            put(&b, "  vec4 t%u = vec4(1.0);\n", s);
        }
        put(&b, "  r.rgb = (");
        if (st->unset &&
            reads_tex(st->colorop, st->colorarg0, st->colorarg1, st->colorarg2, !s)) {
            passthrough(&b, s, st->colorop);
        } else {
            op(&b, s, st->colorop, st->colorarg0, st->colorarg1, st->colorarg2, false);
        }
        put(&b, ").rgb;\n  r.a = (");
        if (st->alphaop == TOP_DISABLE) {
            put(&b, "%s", s ? "cur" : "vDiffuse");
        } else if (st->unset &&
                   reads_tex(st->alphaop, st->alphaarg0, st->alphaarg1, st->alphaarg2, !s)) {
            passthrough(&b, s, st->alphaop);
        } else {
            op(&b, s, st->alphaop, st->alphaarg0, st->alphaarg1, st->alphaarg2, true);
        }
        put(&b, ").a;\n  r = clamp(r, 0.0, 1.0);\n");
        put(&b, "  %s = r;\n", (st->resultarg & TA_SELECTMASK) == TA_TEMP ? "tmp" : "cur");
    }
    if (k->specular) {
        put(&b, "  cur.rgb = clamp(cur.rgb + vSpecular.rgb, 0.0, 1.0);\n");
    }
    /* fog: the final combiner's mix towards FOGCOLOR by the fog factor */
    switch (k->fog) {
    case 0:  break;
    /* a NaN factor is 1, unfogged; the distance signed, as D3D's fog modes
     * (psh.c) */
    case 1:  put(&b, "  float fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"); break;
    case 2:  put(&b, "  float fogf = clamp(exp(-vFog * pc.fog.z), 0.0, 1.0);\n"); break;
    case 3:  put(&b, "  float fd = vFog * pc.fog.z;\n"
                     "  float fogf = clamp(exp(-fd * fd), 0.0, 1.0);\n"); break;
    default: put(&b, "  float fogf = clamp((pc.fog.y - vFog) / "
                     "(pc.fog.y - pc.fog.x), 0.0, 1.0);\n"); break;
    }
    if (k->fog) {
        put(&b, "  cur.rgb = mix(unpackUnorm4x8(pc.fog_color).zyx, cur.rgb, fogf);\n");
    }
    if (k->alpha_test && k->alpha_func < 8) {
        put(&b, "  { float a = cur.a, r = pc.alpha_ref; if (!(%s)) discard; }\n",
            k_cmp[k->alpha_func]);
    }
    if (k->fbfetch & HLE_FB_ZEQ) {
        put(&b, "%s", HLE_ZEQ_TEST_GLSL(k->fbfetch));
    }
    put(&b, "  oColor = %s;\n", k->out_bgra ? "cur.bgra" : "cur");
    if (k->sblend) {
        put(&b, "  { ivec4 sv = ivec4(floor(clamp(%s, 0.0, 1.0) * 255.0 + 0.5));\n"
        "    ivec4 dv = ivec4(floor(clamp(subpassLoad(uFb), 0.0, 1.0) * 255.0 + 0.5));\n"
        "    sv -= 256 * (sv >> 7); dv -= 256 * (dv >> 7);\n"
        "    ivec4 r = clamp(dv %s sv, ivec4(-128), ivec4(127));\n"
        "    oColor = vec4(r & 255) / 255.0; }\n", "oColor", k->sblend == 1 ? "+" : "-");
    }
    put(&b, "%s}\n", k->wbuf ? HLE_WBUF_STORE_GLSL : "");
    return b.overflow ? 0 : b.len;
}
