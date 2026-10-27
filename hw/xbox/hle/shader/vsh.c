/*
 * hakuX HLE GPU — NV2A vertex program -> GLSL (pure). See vsh.h.
 *
 * Semantics follow NV_vertex_program1_1 plus the NV2A rules games rely on:
 * - a MAC and an ILU operation in one slot read their operands before
 *   either writes (both results are computed into temporaries first);
 * - MUL/MAD: a zero factor gives zero even against inf/NaN;
 * - RCP(0) = +inf, RCC clamps the magnitude to [2^-64, 2^64];
 * - R12 is oPos;
 * - the program leaves oPos in screen space (the XDK appends the viewport
 *   transform through c[-38]/c[-37]): the epilogue maps x/y back to NDC
 *   with the same mapping as pre-transformed vertices, divides z by the
 *   depth maximum and multiplies by w to undo the perspective divide.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/vsh.h"
#include "subprojects/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.h"

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

/* NV2A output register file: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0,
 * 8 oB1, 9..12 oT0..oT3 (1, 2 unused). */
static const char *k_out[16] = {
    "oPos", "oUnused1", "oUnused2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3", "oUnused13", "oUnused14", "oUnused15",
};

static const char k_sw[4] = { 'x', 'y', 'z', 'w' };

static void mask_str(Nv2aVshWritemask m, char *s)
{
    unsigned v = (unsigned)m, n = 0;
    if (v & 8) s[n++] = 'x';
    if (v & 4) s[n++] = 'y';
    if (v & 2) s[n++] = 'z';
    if (v & 1) s[n++] = 'w';
    s[n] = 0;
}

static void in_str(Buf *b, const Nv2aVshInput *in)
{
    char reg[48];
    switch (in->type) {
    case NV2ART_TEMPORARY:
        snprintf(reg, sizeof(reg), in->index == 12 ? "oPos" : "R%u", in->index);
        break;
    case NV2ART_INPUT:
        snprintf(reg, sizeof(reg), "v%u", in->index & 15);
        break;
    case NV2ART_CONTEXT:
        if (in->is_relative) {
            snprintf(reg, sizeof(reg), "c[clamp(A0 + %u, 0, 191)]", in->index);
        } else {
            snprintf(reg, sizeof(reg), "c[%u]", in->index % 192);
        }
        break;
    default:
        snprintf(reg, sizeof(reg), "vec4(0.0)");
        break;
    }
    put(b, "%s%s.%c%c%c%c", in->is_negated ? "-" : "", reg, k_sw[in->swizzle[0] & 3],
        k_sw[in->swizzle[1] & 3], k_sw[in->swizzle[2] & 3], k_sw[in->swizzle[3] & 3]);
}

/* Result expression of one operation (vec4), operands already swizzled. */
static void op_expr(Buf *b, const Nv2aVshOperation *o)
{
#define I(n) in_str(b, &o->inputs[n])
    switch (o->opcode) {
    case NV2AOP_MOV: I(0); break;
    case NV2AOP_MUL: put(b, "mulz("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_ADD: put(b, "("); I(0); put(b, " + "); I(1); put(b, ")"); break;
    case NV2AOP_MAD:
        put(b, "(mulz("); I(0); put(b, ", "); I(1); put(b, ") + "); I(2); put(b, ")");
        break;
    case NV2AOP_DP3: put(b, "vec4(dot(("); I(0); put(b, ").xyz, ("); I(1); put(b, ").xyz))");
        break;
    case NV2AOP_DP4: put(b, "vec4(dot("); I(0); put(b, ", "); I(1); put(b, "))"); break;
    case NV2AOP_DPH:
        put(b, "vec4(dot(("); I(0); put(b, ").xyz, ("); I(1); put(b, ").xyz) + ("); I(1);
        put(b, ").w)");
        break;
    case NV2AOP_DST:
        put(b, "dst4("); I(0); put(b, ", "); I(1); put(b, ")");
        break;
    case NV2AOP_MIN: put(b, "min("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_MAX: put(b, "max("); I(0); put(b, ", "); I(1); put(b, ")"); break;
    case NV2AOP_SLT: put(b, "vec4(lessThan("); I(0); put(b, ", "); I(1); put(b, "))"); break;
    case NV2AOP_SGE:
        put(b, "vec4(greaterThanEqual("); I(0); put(b, ", "); I(1); put(b, "))");
        break;
    case NV2AOP_ARL: I(0); break;
    case NV2AOP_RCP: put(b, "rcp4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_RCC: put(b, "rcc4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_RSQ: put(b, "rsq4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_EXP: put(b, "exp4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_LOG: put(b, "log4(("); I(0); put(b, ").x)"); break;
    case NV2AOP_LIT: put(b, "lit4("); I(0); put(b, ")"); break;
    default:         put(b, "vec4(0.0)"); break;
    }
#undef I
}

/* fogw: set when the program writes oFog */
static void out_write(Buf *b, const Nv2aVshOutput *o, const char *val, bool *fogw)
{
    char m[5];
    mask_str(o->writemask, m);
    switch (o->type) {
    case NV2ART_TEMPORARY:
        if (m[0]) {
            if (o->index == 12) {
                put(b, "    oPos.%s = %s.%s;\n", m, val, m);
            } else {
                put(b, "    R%u.%s = %s.%s;\n", o->index % 12, m, val, m);
            }
        }
        break;
    case NV2ART_OUTPUT:
        if (m[0] && (o->index & 15) == 5) {
            /* oFog: the fog coordinate is x, which takes the most
             * significant masked component (a ".w" write lands in x) */
            *fogw = true;
            put(b, "    oFog.x = %s.%c;\n", val, m[strlen(m) - 1]);
        } else if (m[0]) {
            put(b, "    %s.%s = %s.%s;\n", k_out[o->index & 15], m, val, m);
        }
        break;
    case NV2ART_ADDRESS:
        /* ARL floors, but an index the program computes as a whole number
         * (a bone's, from a byte attribute times 765: i / 255 * 765) can land
         * a rounding error below it here where the NV2A lands on it: floored,
         * the vertex took the bone's previous matrix row, and Azurik's model
         * threw a few of its vertices into the ground. A bias far above fp32's
         * error at these magnitudes, far below an intended fraction's. */
        put(b, "    A0 = int(floor(%s.x + 1.0 / 1024.0));\n", val);
        break;
    default:                            /* CONTEXT writes: state programs only */
        break;
    }
}

static const char k_lib[] =
    "vec4 mulz(vec4 a, vec4 b) {\n"
    "  vec4 r = a * b;\n"
    "  vec4 z = vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0)));\n"
    "  return mix(r, vec4(0.0), greaterThan(z, vec4(0.0)));\n"
    "}\n"
    "vec4 dst4(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n"
    "vec4 rcp4(float x) { return vec4(x == 0.0 ? uintBitsToFloat(0x7F800000u) : 1.0 / x); }\n"
    "vec4 rcc4(float x) {\n"
    "  float r = 1.0 / x;\n"
    "  float m = clamp(abs(r), uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u));\n"
    "  return vec4((r < 0.0 || (r == 0.0 && floatBitsToUint(r) != 0u)) ? -m : m);\n"
    "}\n"
    "vec4 rsq4(float x) {\n"
    "  float a = abs(x);\n"
    "  return vec4(a == 0.0 ? uintBitsToFloat(0x7F800000u) : inversesqrt(a));\n"
    "}\n"
    "vec4 exp4(float x) {\n"
    "  float f = floor(x);\n"
    "  return vec4(exp2(f), x - f, exp2(x), 1.0);\n"
    "}\n"
    "vec4 log4(float x) {\n"
    "  float a = abs(x);\n"
    "  if (a == 0.0) return vec4(-uintBitsToFloat(0x7F800000u), 1.0,\n"
    "                            -uintBitsToFloat(0x7F800000u), 1.0);\n"
    "  float e = floor(log2(a));\n"
    "  return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 lit4(vec4 s) {\n"
    "  float x = max(s.x, 0.0), y = max(s.y, 0.0);\n"
    "  float w = clamp(s.w, -127.9961, 127.9961);\n"
    "  return vec4(1.0, x, x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0);\n"
    "}\n";

/* The declarations and main's start: the inputs (v0..v15, current values
 * for registers no array feeds), the outputs, the registers. */
static void head(Buf *bp, const HleVshKey *k)
{
    Buf b = *bp;
    const char *interp = k->flat ? "flat " : "";
    put(&b, "#version 450\n%s", hle_push_glsl());
    put(&b, "layout(set = 1, binding = 0, std140) %s VshConst { vec4 c[192]; "
            "vec4 vcur[16]; };\n"
            "invariant gl_Position;\n", hle_const_glsl());
    for (unsigned i = 0; i < 16; i++) {
        if (k->in_mask & (1u << i)) {
            put(&b, "layout(location = %u) in %s v%u_in;\n", i,
                (k->cmp_mask >> i) & 1 ? "uvec2" : (k->int_mask >> i) & 1 ? "ivec4" : "vec4", i);
        }
    }
    put(&b, "layout(location = 0) %sout vec4 vDiffuse;\n"
            "layout(location = 1) %sout vec4 vSpecular;\n"
            "layout(location = 2) out vec4 vTex0;\n"
            "layout(location = 3) out vec4 vTex1;\n"
            "layout(location = 4) out vec4 vTex2;\n"
            "layout(location = 5) out vec4 vTex3;\n"
            "layout(location = 6) out float vFog;\n"
            "layout(location = 7) %sout vec4 vBack0;\n"
            "layout(location = 8) %sout vec4 vBack1;\n", interp, interp, interp, interp);
    if (k->wbuf) {      /* W-buffering: perspective-correct w for the depth */
        put(&b, "layout(location = 9) out float vW;\n");
    }
    put(&b, "%s", k_lib);
    put(&b, "void main() {\n");
    for (unsigned i = 0; i < 16; i++) {
        if (k->in_mask & (1u << i) && (k->cmp_mask >> i) & 1) {
            /* fetched as two 16-bit halves: the dword is often only 2-byte aligned */
            put(&b, "  int v%up = int(v%u_in.x | (v%u_in.y << 16u));\n"
                    "  vec4 v%u = vec4(vec3(bitfieldExtract(v%up, 0, 11), "
                    "bitfieldExtract(v%up, 11, 11), bitfieldExtract(v%up, 22, 10)) / "
                    "vec3(1023.0, 1023.0, 511.0), 1.0);\n", i, i, i, i, i, i, i);
        } else if (k->in_mask & (1u << i)) {
            put(&b, "  vec4 v%u = vec4(v%u_in);\n", i, i);
        } else {
            /* not in the arrays: the NV2A reads the register's current value
             * (SET_VERTEX_DATA*: Halo's decals take their tint and fade from
             * SetVertexDataColor(9)) */
            put(&b, "  vec4 v%u = vcur[%u];\n", i, i);
        }
    }
    put(&b, "  int A0 = 0;\n"
            "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0);\n"
            "  vec4 R4 = vec4(0.0), R5 = vec4(0.0), R6 = vec4(0.0), R7 = vec4(0.0);\n"
            "  vec4 R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
            "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oD1 = vec4(0.0, 0.0, 0.0, 1.0), oFog = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oPts = vec4(0.0, 0.0, 0.0, 1.0), oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oB1 = vec4(0.0, 0.0, 0.0, 1.0), oT0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oT1 = vec4(0.0, 0.0, 0.0, 1.0), oT2 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  vec4 oUnused1, oUnused2, oUnused13, oUnused14, oUnused15;\n");
    *bp = b;
}

static void tail(Buf *b, const HleVshKey *k, bool fogw);

size_t hle_vsh_glsl(const uint32_t *tokens, unsigned nslots, const HleVshKey *k,
                    char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    head(&b, k);
    bool final = false, fogw = false;
    for (unsigned s = 0; s < nslots && s < HLE_VSH_MAX_SLOTS && !final; s++) {
        Nv2aVshStep st;
        memset(&st, 0, sizeof(st));
        if (nv2a_vsh_parse_step(&st, tokens + 4 * s) != NV2AVPR_SUCCESS) {
            return 0;
        }
        final = st.is_final;
        bool mac = st.mac.opcode != NV2AOP_NOP, ilu = st.ilu.opcode != NV2AOP_NOP;
        if (!mac && !ilu) {
            continue;
        }
        put(&b, "  { /* slot %u */\n", s);
        if (mac) {
            put(&b, "    vec4 m = ");
            op_expr(&b, &st.mac);
            put(&b, ";\n");
        }
        if (ilu) {
            put(&b, "    vec4 i = ");
            op_expr(&b, &st.ilu);
            put(&b, ";\n");
        }
        for (unsigned o = 0; mac && o < 2; o++) {
            out_write(&b, &st.mac.outputs[o], "m", &fogw);
        }
        for (unsigned o = 0; ilu && o < 2; o++) {
            out_write(&b, &st.ilu.outputs[o], "i", &fogw);
        }
        put(&b, "  }\n");
    }
    if (!final) {
        return 0;
    }
    tail(&b, k, fogw);
    return b.overflow ? 0 : b.len;
}

size_t hle_vsh_glsl_wrap(const HleVshKey *k, const char *body, bool fogw, char *buf, size_t cap)
{
    Buf b = { buf, cap, 0, false };
    head(&b, k);
    put(&b, "%s", body);
    tail(&b, k, fogw);
    return b.overflow ? 0 : b.len;
}

/* main's end: the outputs from the registers, oPos (screen space) to clip
 * space. fogw: oFog was written. */
static void tail(Buf *bp, const HleVshKey *k, bool fogw)
{
    Buf b = *bp;
    /* specular off (NV2A SPECULAR_ENABLE clear): the pixel stage sees
     * (0, 0, 0, 1) whatever the program wrote */
    const char *d1 = k->spec_off ? "vec4(0.0, 0.0, 0.0, 1.0)" : "clamp(oD1, 0.0, 1.0)";
    const char *b1 = k->spec_off ? "vec4(0.0, 0.0, 0.0, 1.0)" : "clamp(oB1, 0.0, 1.0)";
    put(&b, "  vDiffuse = clamp(oD0, 0.0, 1.0);\n"
            "  vSpecular = %s;\n"
            "  vBack0 = clamp(oB0, 0.0, 1.0);\n"
            "  vBack1 = %s;\n"
            "  vTex0 = oT0; vTex1 = oT1; vTex2 = oT2; vTex3 = oT3;\n", d1, b1);
    /* A program that leaves oFog alone is not fogged: Halo draws its Bink
     * movies with fog enabled through a program writing no oFog, and they
     * show on the console. Unfogged is factor 1 under FOGTABLEMODE NONE
     * (vertex fog), distance 0 under the table modes. */
    if (fogw) {
        put(&b, "  vFog = oFog.x;\n");
    } else {
        put(&b, "  vFog = %s;\n", k->fog_table ? "0.0" : "1.0");
    }
    put(&b, "  gl_PointSize = max(oPts.x, 1.0) * float(max((pc.flags >> 16) & 255u, 1u));\n"
            /* screen space -> clip space (the NV2A rasterizer's subpixel
             * placement, ff.h) */
            "  float w = oPos.w;\n"
            "  if (w == 0.0) w = 1.0;\n"
            /* the NV2A keeps w finite (magnitude at most 2^64): D3D's XYZRHW
             * pass-through takes RCP of rhw, and rhw 0 (Otogi's title art)
             * would make w +inf and drop the primitive */
            "  if (isinf(w)) w = w > 0.0 ? uintBitsToFloat(0x5F800000u) : uintBitsToFloat(0xDF800000u);\n"
            "  vec2 scr = oPos.xy;\n"
            HLE_SUBPIXEL_GLSL("scr")
            HLE_TEXEL_EDGE_GLSL("scr")
            "  vec2 ndc = scr * pc.vp + vec2(-1.0, 1.0);\n");
    /* The NV2A clips on the depth it tests: under W-buffering that is w (the
     * pixel stage's), and without a depth test nothing. The program's z then
     * must not clip the primitive (Forza's sky dome past the far plane, its
     * minimap at z far below 0), so the clip z only keeps w >= 0. */
    if (!k->wbuf && !k->zfree) {
        put(&b, HLE_ZNORM_GLSL("oPos.z", "zN"));
    }
    put(&b, "  gl_Position = vec4(ndc * w, %s, w);\n", k->wbuf || k->zfree ? "0.0" : "zN * w");
    if (k->zclamp && g_hle_glsl_zclamp) {
        put(&b, HLE_ZCLAMP_GLSL);
    }
    if (g_hle_glsl_zw) {
        put(&b, HLE_ZW_NUDGE_GLSL);
    }
    put(&b, "%s}\n", k->wbuf ? "  vW = w;\n" : "");
    *bp = b;
}
