/*
 * hakuX HLE GPU — push-buffer mode's resources (see pbres.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/front/fe.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/pb/pbres.h"
#include "hw/xbox/hle/tex/xdk_res.h"
#include "hw/xbox/hle/tex/xg_swizzle.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/guest_ram.h"

/* The log lines' limits, the first few of each a boot, and the texture dump
 * asked for (HAKUX_HLE_TEXDUMP: -2 not read yet, -1 none or done):
 * hle_res_reset */
static struct {
    unsigned yuv, headers;
    uint32_t kinds;                     /* the resource kinds logged, a bit each */
    uint32_t fmts[8];                   /* the formats logged, + 1 */
} g_lg;
static int64_t g_texdump = -2;

/* ---- arena ------------------------------------------------------------------- */

/* Room in arena a for len bytes (HLE_ARENA_FULL: none even with the GPU
 * idle). Full: while its oldest block is a submitted one, wait for just
 * that submission (the blocks behind it stay in flight, the GPU busy);
 * once only the open one holds it, kick that and wait for it. Waiting for
 * all that was submitted left the GPU idle and the decoder stopped for
 * over 50 ms at a time. */
static uint32_t ring_alloc(HleArena *a, uint32_t len, uint32_t align)
{
    for (unsigned kicks = 0; kicks < 2;) {
        hle_arena_retire(a, hle_tl_completed());
        uint32_t off = hle_arena_alloc(a, len, align, hle_fe_last_value() + 1);
        if (off != HLE_ARENA_FULL) {
            return off;
        }
        uint32_t pa = hle_hitch_enter(HLE_ACT_ARENA);
        uint64_t v = hle_arena_oldest(a);
        if (!v || v > hle_fe_last_value()) {
            /* everything pending belongs to the next submit: kick it */
            v = hle_fe_next_value();
            HleCmdFence *f = hle_fe_begin(HLE_OP_KICK, sizeof(*f));
            f->value = v;
            hle_fe_commit();
            kicks++;
        }
        hle_tl_wait(v);
        hle_hitch_leave(pa);
    }
    return HLE_ARENA_FULL;
}

static uint32_t arena_alloc(uint32_t len, uint32_t align, uint8_t **ptr)
{
    uint32_t off = ring_alloc(&g_harena.alloc, len, align);
    if (off != HLE_ARENA_FULL) {
        *ptr = g_harena.map + off;
    }
    return off;
}

uint32_t hle_draw_arena_alloc(uint32_t len, uint8_t **ptr)
{
    return arena_alloc(len, 256, ptr);
}

uint32_t hle_draw_arena_alloc16(uint32_t len, uint8_t **ptr)
{
    return arena_alloc(len, 16, ptr);
}

/* ---- textures ------------------------------------------------------------------ */

/* Packed byte size of one level of a w x h texture in format fi. */
static uint32_t level_bytes(const HleFmtInfo *fi, uint32_t w, uint32_t h, uint32_t *row)
{
    if (fi->flags & HLE_FI_DXT) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fi->bpp * 2;   /* 8 or 16 */
        *row = bw * bb;
        return *row * bh;
    }
    *row = w * fi->bpp / 8;
    return *row * h;
}

/* DXT1/3/5 (BC1/2/3) volume -> A8R8G8B8 (bytes B, G, R, A). A DXT volume
 * level is not its slices one after another: the NV2A takes the depth four
 * slices at a time and, inside such a group, keeps each 4x4 block's slices
 * together (group, block row, block column, slice). Read slice by slice, the
 * blocks of other slices land all over the image (the dots on Forza's car
 * paint, whose reflection is a DXT5 volume). */
static uint32_t rgb565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static uint32_t mix565(uint32_t a, uint32_t b, unsigned wa, unsigned wb, unsigned div)
{
    uint32_t o = 0;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        o |= (((a >> sh & 0xFF) * wa + (b >> sh & 0xFF) * wb) / div) << sh;
    }
    return o;
}

/* one block at block (bx, by) of a w x h slice */
static void dxt_block(uint32_t xfmt, const uint8_t *s, uint32_t bx, uint32_t by, uint32_t w,
                      uint32_t h, uint32_t *out)
{
    const uint8_t *cb = s + (xfmt == HLE_XFMT_DXT1 ? 0 : 8);
    uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
    uint32_t pal[4] = { rgb565(c0), rgb565(c1) }, idx = cb[4] | cb[5] << 8 |
                      cb[6] << 16 | (uint32_t)cb[7] << 24;
    bool four = xfmt != HLE_XFMT_DXT1 || c0 > c1;
    pal[2] = four ? mix565(pal[0], pal[1], 2, 1, 3) : mix565(pal[0], pal[1], 1, 1, 2);
    pal[3] = four ? mix565(pal[0], pal[1], 1, 2, 3) : 0;
    uint8_t a[16];
    if (xfmt == HLE_XFMT_DXT3) {
        for (unsigned i = 0; i < 16; i++) {
            unsigned v = (s[i / 2] >> (4 * (i & 1))) & 15;
            a[i] = v << 4 | v;
        }
    } else if (xfmt == HLE_XFMT_DXT5) {
        unsigned a0 = s[0], a1 = s[1], ap[8] = { a0, a1 };
        for (unsigned i = 2; i < 8; i++) {
            ap[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 :
                    i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : i == 6 ? 0 : 255;
        }
        uint64_t bits = 0;
        for (unsigned i = 0; i < 6; i++) {
            bits |= (uint64_t)s[2 + i] << (8 * i);
        }
        for (unsigned i = 0; i < 16; i++) {
            a[i] = ap[(bits >> (3 * i)) & 7];
        }
    } else {
        for (unsigned i = 0; i < 16; i++) {
            a[i] = !four && ((idx >> (2 * i)) & 3) == 3 ? 0 : 255;
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
        if (x < w && y < h) {
            out[y * w + x] = pal[(idx >> (2 * i)) & 3] | (uint32_t)a[i] << 24;
        }
    }
}

static void dxt_volume(uint32_t xfmt, const uint8_t *s, uint32_t w, uint32_t h, uint32_t d,
                       uint32_t *out)
{
    unsigned bb = xfmt == HLE_XFMT_DXT1 ? 8 : 16;
    for (uint32_t z0 = 0; z0 < d; z0 += 4) {
        uint32_t z1 = MIN(d, z0 + 4);
        for (uint32_t by = 0; by < (h + 3) / 4; by++) {
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                for (uint32_t z = z0; z < z1; z++, s += bb) {
                    dxt_block(xfmt, s, bx, by, w, h, out + (size_t)z * w * h);
                }
            }
        }
    }
}

/* A4R4G4B4 -> A8R8G8B8 (bytes B, G, R, A), each 4-bit value n as n * 17, for a
 * GPU without the 4444 formats (Mali's r26: HleCaps.x4444 off) */
static void expand_4444(const uint16_t *s, size_t n, uint32_t *out)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        uint32_t x = (c & 0xF) | (c & 0xF0) << 4 | (c & 0xF00) << 8 | (c & 0xF000) << 12;
        out[i] = x | x << 4;
    }
}

static uint8_t clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* YUY2 / UYVY (BT.601 video range) -> B8G8R8A8, w x h texels: integer form
 * as xemu's convert_yuy2_to_rgb. */
static void yuv_convert(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, bool uyvy,
                        uint8_t *dst)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *l = src + y * pitch;
        uint8_t *q = dst + y * w * 4;
        for (uint32_t x = 0; x + 1 < w; x += 2, l += 4, q += 8) {
            int y0 = (uyvy ? l[1] : l[0]) - 16, y1 = (uyvy ? l[3] : l[2]) - 16;
            int u = (uyvy ? l[0] : l[1]) - 128, v = (uyvy ? l[2] : l[3]) - 128;
            int yy[2] = { y0, y1 };
            for (unsigned k = 0; k < 2; k++) {
                int c = 298 * yy[k];
                q[4 * k + 0] = clip8((c + 516 * u + 128) >> 8);
                q[4 * k + 1] = clip8((c - 100 * u - 208 * v + 128) >> 8);
                q[4 * k + 2] = clip8((c + 409 * v + 128) >> 8);
                q[4 * k + 3] = 255;
            }
        }
    }
}

/* A YUY2 / UYVY texture's texels converted to A8R8G8B8 into the arena as
 * object id's image (HLE_OP_TEX), sent as a linear A8R8G8B8 texture (texel
 * coordinates like the source); false if they can't be. */
static bool yuv_define(uint32_t id, const uint32_t h5[5], const HleResDesc *d)
{
    uint32_t pitch;
    uint32_t off0 = hle_res_subresource(d, 0, 0, &pitch);
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]) + off0, pitch * d->h);
    if (!src || d->w < 2) {
        return false;
    }
    uint8_t *dst;
    uint32_t total = d->w * d->h * 4;
    uint32_t off = arena_alloc(total, 16, &dst);
    if (off == HLE_ARENA_FULL) {
        return false;
    }
    yuv_convert(src, pitch, d->w, d->h, d->fmt == HLE_XFMT_UYVY, dst);
    if (g_lg.yuv++ < 4) {               /* movies: when their frames start converting */
        hle_log("hakuX-hle-front", "YUV texture %u: %ux%u %s converted (#%u of 4 logged)", id,
                d->w, d->h, d->fmt == HLE_XFMT_UYVY ? "UYVY" : "YUY2", g_lg.yuv);
    }
    HleCmdTex *c = hle_fe_begin(HLE_OP_TEX, sizeof(*c));
    c->id = id;
    c->xfmt = HLE_XFMT_LIN_A8R8G8B8;
    c->w = d->w;
    c->h = d->h;
    c->d = 1;
    c->levels = 1;
    c->faces = 1;
    c->arena_off = off;
    c->arena_len = total;
    return true;
}

/* What header words h5 describe, when it is a kind of texture the back end
 * defines (else logged, the first few). */
static bool tex_desc(const uint32_t h5[5], HleResDesc *dp, HleResInfo *rip)
{
    HleResDesc d;
    HleResInfo ri;
    if (!hle_res_from_header(h5[0], h5[3], h5[4], &d) || !hle_res_info(&d, &ri)) {
        if (g_lg.headers++ < 8) {
            hle_log("hakuX-hle-front", "texture header not understood: %08x %08x %08x %08x "
                    "%08x (not bound; first 8 logged)", h5[0], h5[1], h5[2], h5[3], h5[4]);
        }
        return false;
    }
    if (d.kind != HLE_RK_TEXTURE && d.kind != HLE_RK_CUBE && d.kind != HLE_RK_SURFACE &&
        d.kind != HLE_RK_VOLUME) {
        if (!(g_lg.kinds & (1u << (d.kind & 31)))) {
            g_lg.kinds |= 1u << (d.kind & 31);
            hle_log("hakuX-hle-front", "texture kind %u not bound (%08x %08x %08x %08x %08x)",
                    d.kind, h5[0], h5[1], h5[2], h5[3], h5[4]);
        }
        return false;
    }
    *dp = d;
    *rip = ri;
    return true;
}

/* The texels of texture d (header words h5; P8: palette pal, npal entries)
 * unswizzled or converted into the arena as object id's image (HLE_OP_TEX);
 * false if they can't be (format, memory, arena). */
static bool tex_define(uint32_t id, const uint32_t h5[5], const HleResDesc *dp,
                       const HleResInfo *rip, const uint32_t *pal, uint32_t npal)
{
    const HleResDesc d = *dp;
    const HleResInfo ri = *rip;
    const HleFmtInfo *fi = hle_fmt_info(d.fmt);
    bool p8 = d.fmt == HLE_XFMT_P8;
    if (!fi || (fi->flags & (HLE_FI_YUV | HLE_FI_DEPTH)) || (p8 && !npal)) {
        /* YUV / depth textures, P8 without a palette */
        for (unsigned i = 0; i < 8; i++) {
            if (g_lg.fmts[i] == d.fmt + 1) {
                break;
            }
            if (!g_lg.fmts[i]) {
                g_lg.fmts[i] = d.fmt + 1;
                hle_log("hakuX-hle-front", "texture format 0x%x not uploaded yet (%ux%u kind %u)",
                        d.fmt, d.w, d.h, d.kind);
                break;
            }
        }
        return false;
    }
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]), ri.bytes);
    if (!src) {
        return false;
    }
    /* P8 expands to A8R8G8B8 on the way into the arena, and so does DXT in a
     * volume (dxt_volume) or on a GPU without BC (Mali, PowerVR's B-Series:
     * one slice); a volume's levels are depth slices (3D swizzle, or DXT
     * blocks in groups of four slices), depth halving per level */
    bool vol = d.kind == HLE_RK_VOLUME;
    bool dxtd = (fi->flags & HLE_FI_DXT) && (vol || !g_hvk.caps.bc);
    /* A4R4G4B4 on a GPU without the 4444 formats: A8R8G8B8 as well (expand_4444),
     * a linear texture a linear one */
    bool x4444 = (d.fmt == HLE_XFMT_A4R4G4B4 || d.fmt == HLE_XFMT_LIN_A4R4G4B4) &&
                 !g_hvk.caps.x4444;
    uint32_t oxfmt = p8 || dxtd ? HLE_XFMT_A8R8G8B8 :
                     x4444 ? (d.fmt == HLE_XFMT_LIN_A4R4G4B4 ? HLE_XFMT_LIN_A8R8G8B8
                                                            : HLE_XFMT_A8R8G8B8) : d.fmt;
    const HleFmtInfo *ofi = hle_fmt_info(oxfmt);
    uint32_t faces = d.kind == HLE_RK_CUBE ? 6 : 1, levels = MAX(ri.levels, 1u), total = 0;
    uint32_t depth = vol ? MAX(d.d, 1u) : 1;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t row;
        total += level_bytes(ofi, MAX(d.w >> l, 1u), MAX(d.h >> l, 1u), &row) *
                 MAX(depth >> l, 1u);
    }
    total *= faces;
    uint8_t *dst;
    uint32_t off = arena_alloc(total, 16, &dst);
    if (off == HLE_ARENA_FULL) {
        return false;
    }
    uint32_t pos = 0;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t w = MAX(d.w >> l, 1u), h = MAX(d.h >> l, 1u), row, pitch;
            uint32_t dd = MAX(depth >> l, 1u), n = level_bytes(ofi, w, h, &row) * dd;
            const uint8_t *s = src + hle_res_subresource(&d, f, l, &pitch);
            /* a bordered level (hle_res_level): the image inside the border;
             * a cube's border holds its neighbours' texels, which the host's
             * seamless cube filtering takes from them itself */
            uint32_t sw, sh, bo;
            hle_res_level(&d, l, &sw, &sh, &bo);
            HleXgRect img = { bo, bo, w, h };
            /* x4444: the 16-bit texels to a scratch copy first, expanded below */
            uint8_t *out = dst + pos;
            uint32_t orow = row;
            g_autofree uint8_t *t16 = NULL;
            if (x4444) {
                orow = w * 2;
                out = t16 = g_malloc((size_t)orow * h * dd);
            }
            if (dxtd && !vol) {
                dxt_volume(d.fmt, s, w, h, 1, (uint32_t *)(dst + pos));
            } else if (vol) {
                if (dxtd) {
                    dxt_volume(d.fmt, s, w, h, dd, (uint32_t *)(dst + pos));
                } else if (p8) {
                    g_autofree uint8_t *idx = g_malloc((size_t)w * h * dd);
                    hle_xg_unswizzle_box(s, w, h, dd, NULL, idx, w, w * h, 0, 0, 0, 1);
                    uint32_t *o32 = (uint32_t *)(dst + pos);
                    for (uint32_t i = 0; i < w * h * dd; i++) {
                        o32[i] = pal[idx[i]];
                    }
                } else {
                    hle_xg_unswizzle_box(s, w, h, dd, NULL, out, orow, orow * h, 0, 0, 0,
                                         fi->bpp / 8);
                }
            } else if (p8) {
                hle_xg_unswizzle_rect_p8(s, sw, sh, &img, pal, dst + pos, row, 0, 0);
            } else if (fi->flags & HLE_FI_SWZ) {
                hle_xg_unswizzle_rect(s, sw, sh, &img, out, orow, 0, 0, fi->bpp / 8);
            } else if (fi->flags & HLE_FI_DXT) {
                memcpy(dst + pos, s, n);
            } else {
                uint32_t sp = pitch ? pitch : orow;
                for (uint32_t y = 0; y < h; y++) {
                    memcpy(out + y * orow, s + y * sp, orow);
                }
            }
            if (x4444) {
                expand_4444((const uint16_t *)t16, (size_t)w * h * dd, (uint32_t *)(dst + pos));
            }
            pos += n;
        }
    }
    {   /* debug: HAKUX_HLE_TEXDUMP=<id>, that texture's linear texels once */
        if (g_texdump == -2) {
            const char *e = getenv("HAKUX_HLE_TEXDUMP");
            g_texdump = e ? atoll(e) : -1;
        }
#ifdef __ANDROID__
        {   /* on demand: a new value of debug.hakux.texdump=<id>, its next upload */
            static HleProp prop;
            char v[PROP_VALUE_MAX];
            if (hle_prop_new(&prop, "debug.hakux.texdump", v) && atoll(v) > 0) {
                g_texdump = atoll(v);
            }
        }
#endif
        if (g_texdump == id) {
            g_texdump = -1;
            g_autofree char *p = g_strdup_printf("%shle_shadow/tex_%u_f%x_%ux%u_l%u.bin",
                                                 hle_host_base_path(), id, oxfmt, d.w, d.h,
                                                 levels);
            g_file_set_contents(p, (const char *)dst, total, NULL);
            hle_log("hakuX-hle-front", "texdump %s (%u bytes)", p, total);
        }
    }
    HleCmdTex *c = hle_fe_begin(HLE_OP_TEX, sizeof(*c));
    c->id = id;
    c->xfmt = oxfmt;
    c->w = d.w;
    c->h = d.h;
    c->d = depth;
    c->levels = levels;
    c->faces = faces;
    c->arena_off = off;
    c->arena_len = total;
    return true;
}

uint32_t hle_draw_tex_define(uint32_t id, const uint32_t h5[5], const uint32_t *pal, uint32_t npal,
                             uint32_t *bytes)
{
    HleResDesc d;
    HleResInfo ri;
    if (!tex_desc(h5, &d, &ri)) {
        return 0;
    }
    *bytes = ri.bytes;
    if (d.fmt == HLE_XFMT_YUY2 || d.fmt == HLE_XFMT_UYVY) {
        return yuv_define(id, h5, &d) ? id : 0;        /* movies: converted to A8R8G8B8 */
    }
    return tex_define(id, h5, &d, &ri, pal, npal) ? id : 0;
}

/* The overlay is not a texture object: the video decoder rewrites it every
 * frame and the back end keeps its own image (HleCmdOverlay). */
uint32_t hle_draw_overlay_upload(const uint32_t h5[5], uint32_t *w, uint32_t *h)
{
    HleResDesc d;
    HleResInfo ri;
    uint32_t pitch;
    if (!hle_res_from_header(h5[0], h5[3], h5[4], &d) || !hle_res_info(&d, &ri) ||
        (d.fmt != HLE_XFMT_YUY2 && d.fmt != HLE_XFMT_UYVY) || d.w < 2 || !d.h) {
        return HLE_ARENA_FULL;
    }
    uint32_t off0 = hle_res_subresource(&d, 0, 0, &pitch);
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]) + off0, pitch * d.h);
    uint8_t *dst;
    uint32_t off = src ? arena_alloc(d.w * d.h * 4, 16, &dst) : HLE_ARENA_FULL;
    if (off != HLE_ARENA_FULL) {
        yuv_convert(src, pitch, d.w, d.h, d.fmt == HLE_XFMT_UYVY, dst);
        *w = d.w;
        *h = d.h;
    }
    return off;
}

/* ---- primitives ---------------------------------------------------------------- */

enum {
    PT_POINTLIST = 1, PT_LINELIST, PT_LINELOOP, PT_LINESTRIP, PT_TRIANGLELIST,
    PT_TRIANGLESTRIP, PT_TRIANGLEFAN, PT_QUADLIST, PT_QUADSTRIP, PT_POLYGON,
};
/* VkPrimitiveTopology (no Vulkan headers here) */
enum { VT_POINTS = 0, VT_LINES = 1, VT_LINE_STRIP = 2, VT_TRIS = 3, VT_TRI_STRIP = 4,
       VT_TRI_FAN = 5 };

uint32_t hle_draw_prim_topology(uint32_t pt)
{
    switch (pt) {
    case PT_POINTLIST:     return VT_POINTS;
    case PT_LINELIST:      return VT_LINES;
    case PT_LINELOOP:
    case PT_LINESTRIP:     return VT_LINE_STRIP;
    case PT_TRIANGLESTRIP:
    case PT_QUADSTRIP:     return VT_TRI_STRIP;    /* same vertex order */
    case PT_TRIANGLEFAN:
    case PT_POLYGON:       return VT_TRI_FAN;
    default:               return VT_TRIS;         /* lists; quads become triangles */
    }
}

uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n)
{
    if (pt == PT_QUADLIST) {
        return n / 4 * 6;
    }
    if (pt == PT_LINELOOP) {
        return n ? n + 1 : 0;
    }
    return n;
}

void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out)
{
#define IN(i) ((uint16_t)((in ? in[i] : (i)) - base))
    if (pt == PT_QUADLIST) {
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            *out++ = IN(q); *out++ = IN(q + 1); *out++ = IN(q + 2);
            *out++ = IN(q); *out++ = IN(q + 2); *out++ = IN(q + 3);
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        *out++ = IN(i);
    }
    if (pt == PT_LINELOOP && n) {
        *out = IN(0);
    }
#undef IN
}

/* ---- render targets ---------------------------------------------------------------- */

void hle_res_upload(uint32_t id, uint32_t pa, uint32_t pitch, uint32_t n,
                    const HleRect *rects, const int32_t (*pts)[2])
{
    uint32_t lo = UINT32_MAX, hi = 0, i = 0;
    while (i < n) {
        const uint32_t i0 = i, i1 = MIN(n, i0 + HLE_CMD_MAX_RECTS);
        uint32_t clo = UINT32_MAX, chi = 0;
        for (i = i0; i < i1; i++) {
            clo = MIN(clo, pa + (uint32_t)MAX(rects[i].y1, 0) * pitch);
            chi = MAX(chi, pa + (uint32_t)MAX(rects[i].y2, 0) * pitch);
        }
        /* guest RAM the GPU can't read (not imported), or rows the CPU
         * rewrote while the GPU read them in place (a first write waited:
         * a CPU-drawn target uploaded every frame made every frame's first
         * write wait for the GPU, Midnight Club 3 ~40 waits a second): the
         * rows, as they are now, through the arena (allocated before the
         * command: a full arena kicks one) */
        uint32_t arena = 0;
        if ((!g_hram.zero_copy || hle_barrier_contended(clo, chi - clo)) && chi > clo) {
            uint8_t *dst;
            const uint8_t *src = hle_pa_ptr(clo, chi - clo);
            uint32_t off = src ? hle_draw_arena_alloc(chi - clo, &dst) : HLE_ARENA_FULL;
            if (off == HLE_ARENA_FULL) {
                continue;                       /* cmd_upload would skip it too */
            }
            memcpy(dst, src, chi - clo);
            arena = off + 1;
        }
        HleCmdUpload *c = hle_fe_begin(HLE_OP_UPLOAD, sizeof(*c));
        c->dst = id;
        c->pa = pa;
        c->pitch = pitch;
        c->lo = clo;
        c->arena = arena;
        for (i = i0; i < i1; i++) {
            c->rect[c->nrects] = rects[i];
            c->x[c->nrects] = pts[i][0];
            c->y[c->nrects++] = pts[i][1];
        }
        hle_fe_commit();
        if (!arena) {
            lo = MIN(lo, clo);
            hi = MAX(hi, chi);
        }
    }
    if (hi > lo) {          /* the GPU reads it at the submission: CPU writes wait */
        hle_barrier_arm(lo, hi - lo, hle_fe_last_value() + 1);
    }
}

uint32_t hle_res_rb_fb(uint32_t len)
{
    uint8_t *p;
    if (g_hram.zero_copy || !len) {
        return 0;
    }
    if (!g_hland.buf) {
        uint32_t off = hle_draw_arena_alloc(len, &p);
        return off == HLE_ARENA_FULL ? 0 : off + 1;
    }
    /* the landing buffer (host-cached): its space is free again once the
     * GPU passes the value, after the landing read it */
    uint32_t off = ring_alloc(&g_hland.alloc, len, 256);
    return off == HLE_ARENA_FULL ? 0 : off + 1;
}

void hle_res_rb_land(HleCmdReadback *rb, uint32_t fb, uint32_t len, uint64_t value)
{
    if (fb && hle_tl_land(value, fb - 1, rb->pa, len)) {
        rb->fb = fb;
        rb->fb_len = len;
    }
}

/* A 32-bit linear colour texture over a D24S8 target's memory reads the
 * depth buffer's bits, as the NV2A's memory holds them (Conker's glow mask:
 * its depth buffer as R8G8B8A8, the stencil in alpha). */
static const struct { uint8_t xfmt, map[4]; } k_zread[] = {     /* word bytes per channel */
    { HLE_XFMT_LIN_A8R8G8B8, { 2, 1, 0, 3 } }, { HLE_XFMT_LIN_X8R8G8B8, { 2, 1, 0, 4 } },
    { HLE_XFMT_LIN_A8B8G8R8, { 0, 1, 2, 3 } }, { HLE_XFMT_LIN_B8G8R8A8, { 1, 2, 3, 0 } },
    { HLE_XFMT_LIN_R8G8B8A8, { 3, 2, 1, 0 } },
};

bool hle_res_zread_map(uint32_t xfmt, uint8_t map[4])
{
    for (unsigned k = 0; k < ARRAY_SIZE(k_zread); k++) {
        if (k_zread[k].xfmt == xfmt) {
            memcpy(map, k_zread[k].map, 4);
            return true;
        }
    }
    return false;
}

void hle_res_reset(void)
{
    memset(&g_lg, 0, sizeof(g_lg));
    g_texdump = -2;
}
