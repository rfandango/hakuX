#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/shader/ff.h"
#include "hle_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.frag", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* register-combiner encodings (XDK PS_* macros) */
#define IN4(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define R_ZERO 0x0
#define R_C0   0x1
#define R_FOG  0x3
#define R_V0   0x4
#define R_T0   0x8
#define R_T1   0x9
#define R_R0   0xC
#define R_R1   0xD
#define CH_A   0x10
#define INVERT 0x20
#define EXPAND 0x40

/* tex t0; mul r0, t0, v0 */
static void test_modulate(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_V0 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;                            /* t0 PROJECT2D */
    rs[HLE_PS_RGB_IN + 5] = 0xDEADBEEF;                 /* past the count: ignored */
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_EQ_U32(k.rgb_in[5], 0);
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_modulate");
    HT_EQ_INT(un, 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp(0u, nvproj(vTex0).xy);") != NULL);
    HT_CHECK(strstr(buf, "vec3 nvproj(vec4 t)") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = max(t0.rgb, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "float Aa = max(t0.a, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = A * B") != NULL);
    HT_CHECK(strstr(buf, "r0.rgb = ab;") != NULL);
    HT_CHECK(strstr(buf, "r0.a = aab;") != NULL);
    /* no final combiner: D3D's own (fog over r0, alpha r0.a) */
    HT_CHECK(strstr(buf, "outc = vec4(mix(fogr.rgb, clamp(r0.rgb, 0.0, 1.0), fogf), "
                         "clamp(r0.a, 0.0, 1.0));") != NULL);
    HT_CHECK(strstr(buf, "r0 = vec4(0.0, 0.0, 0.0, t0.a)") != NULL);
    k.specular = 1;                                     /* SPECULARENABLE: v1 + r0 */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "clamp(v1.rgb + r0.rgb, 0.0, 1.0), fogf)") != NULL);
    /* no descriptor indexing: every array sized, nothing nonuniform */
    HT_CHECK(hle_glsl_fixed_slots(buf, sizeof(buf)) == strlen(buf));
    HT_CHECK(strstr(buf, "nonuniform") == NULL && strstr(buf, "[]") == NULL);
    HT_CHECK(strstr(buf, "uniform textureCube uTexCube[4];") != NULL);
    HT_CHECK(strstr(buf, "uniform texture3D uTex3D[4];") != NULL);
    HT_CHECK(strstr(buf, "uTex2D[(ti)]") != NULL);
    dump("psh_fixed");
}

/* fog through the final combiner: xfc fog.a, r0, fog.rgb, zero, zero, zero, r0.a;
 * alpha test GREATEREQUAL; linear fog */
static void test_final_fog(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_ZERO | INVERT, R_ZERO, R_ZERO);   /* t0 * 1 */
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_FINAL_ABCD] = IN4(R_FOG | CH_A, R_R0, R_FOG, R_ZERO);
    rs[HLE_PS_FINAL_EFG] = IN4(R_ZERO, R_ZERO, R_R0 | CH_A, 0x80);
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.fog = 4;
    k.alpha_test = 1;
    k.alpha_func = 6;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_fog");
    HT_CHECK(strstr(buf, "vec3 B = max(r0.rgb, 0.0)") == NULL ||
             strstr(buf, "(1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0))") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = clamp(max(fogr.aaa, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "outc = vec4(A * B + (1.0 - A) * C + D, G);") != NULL);
    HT_CHECK(strstr(buf, "float G = clamp(max(r0.a, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "v1r0 = clamp(v1r0, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "(pc.fog.y - vFog) / (pc.fog.y - pc.fog.x)") != NULL);
    HT_CHECK(strstr(buf, "a >= r") != NULL);
    k.fog_abs = 1;                      /* an _ABS fog mode the title pushed */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "(pc.fog.y - abs(vFog)) / (pc.fog.y - pc.fog.x)") != NULL);
    k.fog_abs = 0;
    k.tex_modes = 2;                    /* PROJECT3D: (s, t, r) / q */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3(0u, nvproj(vTex0));") != NULL);
    k.tex_modes = 1;
    k.vol = 1;                          /* PROJECT2D on a volume: its first slice */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3(0u, vec3(nvproj(vTex0).xy, 0.0));") != NULL);
    k.vol = 0;
}

/* two stages: dot3 with expanded normals and MSB mux, unique C0, clip plane,
 * an unsupported (cube) mode, blue-to-alpha and output scaling */
static void test_dot_mux(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1 | (3u << 5) | (5u << 10);   /* t0 2D, t1 cube, t2 clip */
    rs[HLE_PS_COMPARE] = 0x5u << 8;                      /* t2: s >= 0, r >= 0 */
    rs[HLE_PS_RGB_IN] = IN4(R_T0 | EXPAND, R_V0 | EXPAND, R_C0, R_T1);
    rs[HLE_PS_RGB_OUT] = (R_R1 << 4) | R_R0 | (0x2000) | (0x80000) | (2u << 15);
    rs[HLE_PS_RGB_IN + 1] = IN4(R_R0, R_C0, R_R1, R_C0);
    rs[HLE_PS_RGB_OUT + 1] = (R_R0 << 8) | 0x4000;      /* r0 = mux */
    rs[HLE_PS_COUNT] = 2 | 0x100 | 0x1000;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_dot");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "vec4 t1 = sampc(1u, vTex1.xyz);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = vec3(dot(A, B))") != NULL);
    HT_CHECK(strstr(buf, "(2.0 * max(t0.rgb, 0.0) - 1.0)") != NULL);
    HT_CHECK(strstr(buf, "r1.a = ab.b;") != NULL);
    HT_CHECK(strstr(buf, "ab = clamp((ab * 2.0), -1.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    HT_CHECK(strstr(buf, "col(pc.psc[1])") != NULL);    /* stage 1's own C0 */
    HT_CHECK(strstr(buf, "if (vTex2.x >= 0.0) discard;") != NULL);
    HT_CHECK(strstr(buf, "if (vTex2.y < 0.0) discard;") != NULL);
}

/* Halo 2's scope mask: r0.a = 0.5 - t0.b (HALFBIAS_NEGATE), then a MUX on
 * its MSB picks black (CD) where t0.b is 0: the bias's 0.5 is 0x80 / 0xFF
 * in shaders that MUX on the MSB, 0.5 in the others */
static void test_mux_bias(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_ALPHA_IN + 1] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_T0 | 0xA0, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 1] = R_R0 << 8;               /* r0.a = C0.a + (0.5 - t0.b) */
    rs[HLE_PS_RGB_IN + 2] = IN4(R_C0, R_ZERO | INVERT, R_R0, R_ZERO | INVERT);
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;      /* t1 = mux(C0, r0) */
    rs[HLE_PS_COUNT] = 3 | 0x100;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_mux_bias");
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    /* a MUX in the alpha portion only; the output BIAS takes the same 0x80 */
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;
    rs[HLE_PS_ALPHA_IN + 2] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_R0 | CH_A, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_RGB_IN + 1] = IN4(R_T0 | 0x80, R_ZERO | INVERT, R_ZERO, R_ZERO);  /* HALFBIAS_NORMAL */
    rs[HLE_PS_RGB_OUT + 1] = (R_R1 << 4) | (3u << 15);  /* SHIFTLEFT_1_BIAS */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "ams = mx ? acd : aab;") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = (max(t0.rgb, 0.0) - (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "ab = clamp(((ab - (128.0 / 255.0)) * 2.0), -1.0, 1.0);") != NULL);
    rs[HLE_PS_RGB_IN + 1] = 0;
    rs[HLE_PS_RGB_OUT + 1] = 0;
    rs[HLE_PS_ALPHA_IN + 2] = 0;
    rs[HLE_PS_ALPHA_OUT + 2] = 0;
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_COUNT] = 3;                               /* MUX on the LSB */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + 0.5);") != NULL);
    rs[HLE_PS_COUNT] = 3 | 0x100;
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;                 /* no MUX: sum */
    rs[HLE_PS_ALPHA_OUT + 1] = (R_R0 << 8) | (1u << 15);   /* BIAS */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + 0.5);") != NULL);
    HT_CHECK(strstr(buf, "ams = clamp((ams - 0.5), -1.0, 1.0);") != NULL);
}

/* PGR2's car paint: t0 normal map, t1/t2 DOTPRODUCT, t3 DOT_RFLCT_SPEC */
static void test_reflect(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 0x64621;
    rs[HLE_PS_DOTMAP] = 0x111;                          /* MINUS1_TO_1_D3D */
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_reflect");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "dot1 = dot(vTex1.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot2 = dot(vTex2.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot3 = dot(vTex3.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "e = vec3(vTex1.w, vTex2.w, vTex3.w)") != NULL);
    HT_CHECK(strstr(buf, "t3 = sampc(3u,") != NULL);
}

/* NGB's shadow receivers: t0 a D24 shadow buffer (PROJECT3D, SHADOWFUNC
 * GREATEREQUAL). The coordinates go through NV2A's projection, so a
 * receiver with oT0 = 0 compares r = 0 (lit) instead of NaN weights. */
static void test_shadow_project(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_C0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 2;                            /* t0 PROJECT3D */
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.shadow = 1 | (6u << 4);
    k.shadow_fmt = 1;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_shadow");
    HT_CHECK(strstr(buf, "vec3 pq = nvproj(vTex0);") != NULL);
    HT_CHECK(strstr(buf, "float m = 16777215.0, r = clamp(pq.z, 0.0, m);") != NULL);
    HT_CHECK(strstr(buf, "greaterThanEqual(d, vec4(r))") != NULL);
    HT_CHECK(strstr(buf, "/ vTex0.w") == NULL);
}

/* Otogi's title ripple (modes 0x004c1): t0 the bump map, t1 BUMPENVMAP of
 * the copied scene, t2 2D; then t2 BUMPENVMAP_LUM reading t1 (PSInputTexture) */
static void test_bumpenv(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T1, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 0x004c1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_bumpenv");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "uvec4 bump;") != NULL);
    HT_CHECK(strstr(buf, "vec2 d = mat2(unpackHalf2x16(pc.bump.x), unpackHalf2x16(pc.bump.y)) * "
                         "sgn8(t0.bg);") != NULL);
    HT_CHECK(strstr(buf, "t1 = samp(1u, nvproj(vTex1).xy + d);") != NULL);
    HT_CHECK(strstr(buf, "vec4 t2 = samp(2u, nvproj(vTex2).xy);") != NULL);
    k.vol = 2;                          /* t1 a volume: r passes through */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_CHECK(strstr(buf, "t1 = samp3(1u, vec3(nvproj(vTex1).xy + d, vTex1.z));") != NULL);
    rs[HLE_PS_TEXMODES] = 1 | (1u << 5) | (7u << 10);
    rs[HLE_PS_INPUTTEX] = 1u << 16;                     /* t2 reads t1 */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "sgn8(t1.bg);") != NULL);
    HT_CHECK(strstr(buf, "t2.rgb *= clamp(l.x * t1.r + l.y, 0.0, 1.0);") != NULL);
}

static void test_overflow(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_COUNT] = 8;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, 1024, NULL) == 0);
}

int main(void)
{
    HT_RUN(test_modulate);
    HT_RUN(test_final_fog);
    HT_RUN(test_dot_mux);
    HT_RUN(test_mux_bias);
    HT_RUN(test_reflect);
    HT_RUN(test_shadow_project);
    HT_RUN(test_bumpenv);
    HT_RUN(test_overflow);
    HT_DONE();
}
