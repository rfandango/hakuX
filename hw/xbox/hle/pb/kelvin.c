/*
 * hakuX HLE GPU — push-buffer mode's Kelvin (class 0x97) register shadow.
 *
 * The decoder hands every Kelvin method here, and the registers are kept as
 * the NV2A holds them, with a bit per register written. A draw takes its
 * state from them, translated into what the back end's draw path reads:
 * - texture-stage states (address, filters, LOD, kill, signs, border);
 * - the combiner block (D3DPIXELSHADERDEF is the combiner registers);
 * - the render states (canonical 5659 values: Xbox D3D's are the NV2A's GL
 *   enums for blend, depth, stencil, colour mask and shade mode).
 * Registers never written keep the state's defaults.
 */
#include "hw/xbox/hle/base.h"
#include <math.h>
#include "hw/xbox/hle/pb/kelvin.h"
#include "hw/xbox/hle/pb/pbstate.h"
#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/tex/xdk.h"

#define KV_REGS 0x800u                  /* methods 0..0x1FFC */
#define R(m)    (g_kv.r[(m) >> 2])
#define W(m)    ((g_kv.w[(m) >> 7] >> (((m) >> 2) & 31)) & 1u)

/* 5659 render-state and texture-stage-state indices (vk/draw.c) */
enum {
    RS_ZFUNC = 57, RS_ALPHAFUNC = 58, RS_ALPHABLENDENABLE = 59, RS_ALPHATESTENABLE = 60,
    RS_ALPHAREF = 61, RS_SRCBLEND = 62, RS_DESTBLEND = 63, RS_ZWRITEENABLE = 64,
    RS_SHADEMODE = 66, RS_COLORWRITEENABLE = 67, RS_STENCILZFAIL = 68,
    RS_STENCILPASS = 69, RS_STENCILFUNC = 70, RS_STENCILREF = 71, RS_STENCILMASK = 72,
    RS_STENCILWRITEMASK = 73, RS_BLENDOP = 74, RS_BLENDCOLOR = 75,
    RS_ZSLOPESCALE = 77, RS_ZOFFSET = 78, RS_POINTOFFSETENABLE = 79,
    RS_WIREFRAMEOFFSETENABLE = 80, RS_SOLIDOFFSETENABLE = 81,
    RS_FOGENABLE = 92, RS_FOGTABLEMODE = 93, RS_FOGSTART = 94, RS_FOGEND = 95,
    RS_FOGDENSITY = 96, RS_SPECULARENABLE = 103, RS_POINTSPRITEENABLE = 118,
    RS_FOGCOLOR = 138, RS_ZENABLE = 143, RS_STENCILENABLE = 144, RS_STENCILFAIL = 145,
    RS_FRONTFACE = 146, RS_CULLMODE = 147, RS_SHADOWFUNC = 156,
};
enum {
    TSS_ADDRESSU = 0, TSS_ADDRESSV, TSS_ADDRESSW, TSS_MAGFILTER, TSS_MINFILTER,
    TSS_MIPFILTER, TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL, TSS_MAXANISOTROPY,
    TSS_COLORKEYOP, TSS_COLORSIGN, TSS_ALPHAKILL,
    TSS_BUMPENVMAT00 = 22,              /* .. 11, then LSCALE, LOFFSET */
    TSS_BORDERCOLOR = 29,
};

static struct {
    uint32_t r[KV_REGS];
    uint32_t w[KV_REGS / 32];           /* written (bit per register) */
    uint32_t sgen;                      /* bumped when one hle_kelvin_state reads may change */
} g_kv;

static bool state_method(uint32_t m)
{
    return !(m >= 0x0B00 && m < 0x0C00) &&              /* program, constants */
           !(m >= 0x1500 && m < 0x1720) &&              /* immediate vertex data */
           !(m >= 0x17C8 && m <= 0x17D0) &&             /* reports */
           !(m >= 0x17FC && m < 0x1B00) &&              /* draws, inline / vertex data */
           !(m >= 0x1D6C && m <= 0x1D74) &&             /* semaphores */
           !(m >= 0x1D8C && m <= 0x1D9C) &&             /* clears */
           !(m >= 0x1E80 && m <= 0x1EA4) &&             /* transform data, loads */
           m != 0x0100 && m != 0x0110;
}

/* Of those, the ones hle_kelvin_state doesn't read: a draw's transforms,
 * lights and texture images, which change from one draw to the next. */
static bool state_unread(uint32_t m)
{
    return (m >= 0x0440 && m < 0x09C0) ||              /* matrices, texgen planes */
           (m >= 0x09D0 && m < 0x0A60) ||              /* fog plane .. eye position */
           (m >= 0x0C00 && m < 0x1500) ||              /* lights */
           (m >= 0x1720 && m < 0x17A0) ||              /* vertex arrays */
           /* a stage's OFFSET, FORMAT, CONTROL1, +0x18, IMAGE_RECT, PALETTE */
           (m >= 0x1B00 && m < 0x1C00 && ((0x1D3u >> ((m >> 2) & 15)) & 1));
}

/* ---- surface formats -------------------------------------------------------------- */

/* XDK format codes of the NV2A surface colour formats (linear, swizzled) */
static const uint8_t k_surf_color[16][2] = {
    [1] = { HLE_XFMT_LIN_X1R5G5B5, HLE_XFMT_X1R5G5B5 },
    [2] = { HLE_XFMT_LIN_X1R5G5B5, HLE_XFMT_X1R5G5B5 },
    [3] = { HLE_XFMT_LIN_R5G6B5, HLE_XFMT_R5G6B5 },
    [4] = { HLE_XFMT_LIN_X8R8G8B8, HLE_XFMT_X8R8G8B8 },
    [5] = { HLE_XFMT_LIN_X8R8G8B8, HLE_XFMT_X8R8G8B8 },
    [6] = { HLE_XFMT_LIN_A8R8G8B8, HLE_XFMT_A8R8G8B8 },
    [7] = { HLE_XFMT_LIN_A8R8G8B8, HLE_XFMT_A8R8G8B8 },
    [8] = { HLE_XFMT_LIN_A8R8G8B8, HLE_XFMT_A8R8G8B8 },
    [9] = { HLE_XFMT_LIN_L8, HLE_XFMT_L8 },
    [10] = { HLE_XFMT_LIN_G8B8, HLE_XFMT_G8B8 },
};

uint32_t hle_kelvin_color_xfmt(uint32_t cf, bool swz)
{
    return cf < 16 ? k_surf_color[cf][swz] : 0;
}

uint32_t hle_kelvin_depth_xfmt(bool z24, bool zfloat, bool swz)
{
    static const uint8_t k_z[2][2][2] = {   /* [Z24][float][swizzled] */
        { { HLE_XFMT_LIN_D16, HLE_XFMT_D16 }, { HLE_XFMT_LIN_F16, HLE_XFMT_F16 } },
        { { HLE_XFMT_LIN_D24S8, HLE_XFMT_D24S8 }, { HLE_XFMT_LIN_F24S8, HLE_XFMT_F24S8 } },
    };
    return k_z[z24][zfloat][swz];
}

/* ---- methods ---------------------------------------------------------------------- */

/* state_method && !state_unread by register (a bit each), made at the first
 * method (the decoder's thread): the two were ~15 comparisons at every
 * change of a register. */
static uint32_t g_state_read[KV_REGS / 32];
static bool g_state_read_ready;

static void state_read_init(void)
{
    for (uint32_t i = 0; i < KV_REGS; i++) {
        if (state_method(4 * i) && !state_unread(4 * i)) {
            g_state_read[i >> 5] |= 1u << (i & 31);
        }
    }
    g_state_read_ready = true;
}

void hle_kelvin_method(uint32_t m, uint32_t v)
{
    if (m >= 4 * KV_REGS) {
        return;
    }
    if (__builtin_expect(!g_state_read_ready, 0)) {
        state_read_init();
    }
    uint32_t i = m >> 2;
    if ((g_kv.r[i] != v || !W(m)) && (g_state_read[i >> 5] >> (i & 31) & 1)) {
        g_kv.sgen++;                    /* a first write counts: hle_kelvin_state reads W() */
    }
    g_kv.r[i] = v;
    g_kv.w[i >> 5] |= 1u << (i & 31);
}

void hle_kelvin_reset(void)
{
    memset(&g_kv, 0, sizeof(g_kv));     /* g_state_read stays: it is the methods' */
}

uint32_t hle_kelvin_state_gen(void)
{
    return g_kv.sgen;
}

/* ---- render state ------------------------------------------------------------------ */

static void tss(uint32_t *t, unsigned s)
{
    uint32_t b = 0x1B00 + 64 * s;
    uint32_t addr = R(b + 0x08), ctl0 = R(b + 0x0C), filt = R(b + 0x14);
    t[TSS_ADDRESSU] = addr & 0xF;
    t[TSS_ADDRESSV] = (addr >> 8) & 0xF;
    t[TSS_ADDRESSW] = (addr >> 16) & 0xF;
    /* MIN: 1 nearest, 2 linear, 3-6 {nearest, linear} x mip {nearest, linear},
     * 7 convolution; MAG: 1 nearest, 2 linear, 4 convolution */
    static const uint8_t k_min[8][2] = { { 1, 0 }, { 1, 0 }, { 2, 0 }, { 1, 1 },
                                         { 2, 1 }, { 1, 2 }, { 2, 2 }, { 2, 0 } };
    unsigned min = (filt >> 16) & 7, aniso = (ctl0 >> 4) & 3;
    t[TSS_MAGFILTER] = (filt >> 24) & 0xF;
    t[TSS_MINFILTER] = aniso && k_min[min][0] == 2 ? 3 : k_min[min][0];
    t[TSS_MIPFILTER] = k_min[min][1];
    t[TSS_MAXANISOTROPY] = 1u << aniso;
    int32_t lod = (int32_t)(filt << 19) >> 19;          /* signed 5.8 */
    float bias = lod / 256.0f;
    memcpy(&t[TSS_MIPMAPLODBIAS], &bias, 4);
    t[TSS_MAXMIPLEVEL] = (ctl0 >> 26) & 0xF;            /* min LOD clamp, 4.8 */
    t[TSS_COLORKEYOP] = ctl0 & 3;
    t[TSS_ALPHAKILL] = (ctl0 >> 2) & 1;
    t[TSS_COLORSIGN] = filt & 0xF0000000u;
    t[TSS_BORDERCOLOR] = R(b + 0x24);
    /* SET_TEXTURE_SET_BUMP_ENV_MAT 00 01 10 11, SCALE, OFFSET, where written */
    for (unsigned i = 0; i < 6; i++) {
        if (W(b + 0x28 + 4 * i)) {
            t[TSS_BUMPENVMAT00 + i] = R(b + 0x28 + 4 * i);
        }
    }
}

/* The render states taken from one method each, the value as the NV2A
 * takes it. */
static const struct { uint16_t m, rs; } k_rs_map[] = {
    { 0x0300, RS_ALPHATESTENABLE }, { 0x0304, RS_ALPHABLENDENABLE }, { 0x033C, RS_ALPHAFUNC },
    { 0x0340, RS_ALPHAREF }, { 0x0344, RS_SRCBLEND }, { 0x0348, RS_DESTBLEND },
    { 0x034C, RS_BLENDCOLOR }, { 0x0350, RS_BLENDOP }, { 0x0354, RS_ZFUNC },
    { 0x0358, RS_COLORWRITEENABLE }, { 0x035C, RS_ZWRITEENABLE }, { 0x037C, RS_SHADEMODE },
    { 0x032C, RS_STENCILENABLE }, { 0x0360, RS_STENCILWRITEMASK }, { 0x0364, RS_STENCILFUNC },
    { 0x0368, RS_STENCILREF }, { 0x036C, RS_STENCILMASK }, { 0x0370, RS_STENCILFAIL },
    { 0x0374, RS_STENCILZFAIL }, { 0x0378, RS_STENCILPASS }, { 0x0330, RS_POINTOFFSETENABLE },
    { 0x0334, RS_WIREFRAMEOFFSETENABLE }, { 0x0338, RS_SOLIDOFFSETENABLE },
    { 0x0384, RS_ZSLOPESCALE }, { 0x0388, RS_ZOFFSET }, { 0x02A4, RS_FOGENABLE },
    { 0x03B8, RS_SPECULARENABLE },
    /* SET_SHADOW_COMPARE_FUNC (D3DCMP_*: NEVER put Fable's whole world in
     * shadow), SET_POINT_SMOOTH_ENABLE (point sprites) */
    { 0x1E6C, RS_SHADOWFUNC }, { 0x031C, RS_POINTSPRITEENABLE },
};

void hle_kelvin_state(HleCmdState *st)
{
    uint32_t *rs = st->rs;
    for (unsigned i = 0; i < ARRAY_SIZE(k_rs_map); i++) {
        if (W(k_rs_map[i].m)) {
            rs[k_rs_map[i].rs] = R(k_rs_map[i].m);
        }
    }
    if (W(0x030C)) {                    /* W-buffering: Z_PERSPECTIVE_ENABLE */
        rs[RS_ZENABLE] = R(0x030C) ? ((R(0x0290) & 0x10000u) ? 2 : 1) : 0;
    }
    if (W(0x0308) || W(0x039C) || W(0x03A0)) {
        /* CULLMODE is the winding culled: the front one for GL_FRONT */
        uint32_t ff = W(0x03A0) && R(0x03A0) ? R(0x03A0) : 0x900;
        rs[RS_FRONTFACE] = ff;
        rs[RS_CULLMODE] = !R(0x0308) ? 0 : R(0x039C) == 0x404 ? ff : (ff ^ 1);
    }
    if (W(0x029C)) {                    /* 0x100: an _ABS mode (0x802 .. 0x804) */
        uint32_t m = R(0x029C);
        rs[RS_FOGTABLEMODE] = (m == 0x2601 || m == 0x804 ? 3 : m == 0x801 || m == 0x803 ? 2 : 1) |
                              (m >= 0x802 && m <= 0x804 ? 0x100 : 0);
    }
    if (W(0x09C0) || W(0x09C4)) {
        /* SET_FOG_PARAMS as pushed (Conker's meshes: their own fog): the
         * NV2A's linear factor is p0 - 1 + p1 d, exp / exp2 take the density
         * from p1 (D3D's encoding, as LLE decodes it). The back end's fog
         * takes start / end / density: the parameters go back to those. */
        float p0, p1, st, en, dens;
        uint32_t r0 = R(0x09C0), r1 = R(0x09C4);
        memcpy(&p0, &r0, 4);
        memcpy(&p1, &r1, 4);
        if ((rs[RS_FOGTABLEMODE] & 3) == 3) {
            if (fabsf(p1) > 1.0e-30f) {
                en = (1.0f - p0) / p1;
                st = en + 1.0f / p1;
            } else {                    /* the constant p0 - 1 over a vast range */
                en = (p0 - 1.0f) * 1.0e30f;
                st = en - 1.0e30f;
            }
            memcpy(&rs[RS_FOGSTART], &st, 4);
            memcpy(&rs[RS_FOGEND], &en, 4);
        } else {
            dens = -p1 * 2.0f *
                   ((rs[RS_FOGTABLEMODE] & 3) == 2 ? sqrtf(logf(256.0f)) : logf(256.0f));
            memcpy(&rs[RS_FOGDENSITY], &dens, 4);
        }
    }
    if (W(0x02A8)) {                    /* ABGR -> D3DCOLOR */
        uint32_t c = R(0x02A8);
        rs[RS_FOGCOLOR] = (c & 0xFF00FF00u) | (c & 0xFF) << 16 | ((c >> 16) & 0xFF);
    }
    if (W(0x1E60)) {                    /* the combiners: D3DPIXELSHADERDEF */
        for (unsigned i = 0; i < 8; i++) {
            rs[HLE_PS_ALPHA_IN + i] = R(0x0260 + 4 * i);
            rs[HLE_PS_C0 + i] = R(0x0A60 + 4 * i);
            rs[HLE_PS_C1 + i] = R(0x0A80 + 4 * i);
            rs[HLE_PS_ALPHA_OUT + i] = R(0x0AA0 + 4 * i);
            rs[HLE_PS_RGB_IN + i] = R(0x0AC0 + 4 * i);
            rs[HLE_PS_RGB_OUT + i] = R(0x1E40 + 4 * i);
        }
        rs[HLE_PS_FINAL_ABCD] = R(0x0288);
        rs[HLE_PS_FINAL_EFG] = R(0x028C);
        rs[HLE_PS_COMPARE] = R(0x17F8);
        rs[HLE_PS_FINAL_C0] = R(0x1E20);
        rs[HLE_PS_FINAL_C1] = R(0x1E24);
        rs[HLE_PS_COUNT] = R(0x1E60);
        if (W(0x1E70)) {
            rs[HLE_PS_TEXMODES] = R(0x1E70);
        }
        rs[HLE_PS_DOTMAP] = R(0x1E74);
        rs[HLE_PS_INPUTTEX] = R(0x1E78);
        st->psh = 1;
    }
    for (unsigned s = 0; s < HLE_TSS_STAGES; s++) {
        if (W(0x1B0C + 64 * s)) {
            tss(st->tss[s], s);
        }
    }
    if (W(0x03B8)) {
        st->spec = R(0x03B8) != 0;
    }
    if (W(0x1D78)) {                    /* SET_ZMIN_MAX_CONTROL: ZCLAMP_EN (bits 4..7) CLAMP */
        st->zclamp = ((R(0x1D78) >> 4) & 0xF) == 1;
    }
}

/* ---- savestates (pb/pbstate.h) ------------------------------------------------------ */

_Static_assert(sizeof(((HleKvState *)0)->r) == sizeof(g_kv.r), "r");
_Static_assert(sizeof(((HleKvState *)0)->w) == sizeof(g_kv.w), "w");

void hle_kelvin_state_get(HleKvState *s)
{
    memcpy(s->r, g_kv.r, sizeof(s->r));
    memcpy(s->w, g_kv.w, sizeof(s->w));
}

void hle_kelvin_state_set(const HleKvState *s)
{
    memcpy(g_kv.r, s->r, sizeof(g_kv.r));
    memcpy(g_kv.w, s->w, sizeof(g_kv.w));
    g_kv.sgen++;
}
