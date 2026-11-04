/*
 * hakuX HLE GPU — front-end -> back-end commands (spec §5.3).
 * Every record is { HleCmdHdr, payload } padded to 8 bytes.
 */
#ifndef HW_XBOX_HLE_FRONT_CMD_H
#define HW_XBOX_HLE_FRONT_CMD_H

#include "hw/xbox/hle/hle_pure.h"

enum HleOp {
    HLE_OP_PAD = 0,             /* ring wrap filler, never returned by peek */
    HLE_OP_DEVICE_CREATE = 1,   /* HleCmdDevice */
    HLE_OP_DEVICE_RESET = 2,    /* HleCmdDevice */
    HLE_OP_OBJ_CREATE = 4,      /* HleCmdObj (render-target capable surfaces) */
    HLE_OP_OBJ_DESTROY = 5,     /* HleCmdObjDestroy */
    HLE_OP_SET_RT = 6,          /* HleCmdSetRt */
    HLE_OP_VIEWPORT = 7,        /* HleCmdViewport */
    HLE_OP_CLEAR = 8,           /* HleCmdClear */
    HLE_OP_SWAP = 9,            /* HleCmdSwap   (submit boundary) */
    HLE_OP_KICK = 11,           /* HleCmdFence  (submit boundary) */
    HLE_OP_READBACK = 12,       /* HleCmdReadback (submit boundary) */
    HLE_OP_STATE = 13,          /* HleCmdState: draw state, sent when it changed */
    HLE_OP_TEX = 14,            /* HleCmdTex: texture definition + texels in the arena */
    HLE_OP_DRAW = 15,           /* HleCmdDraw */
    HLE_OP_VSH = 16,            /* HleCmdVsh: a vertex program, before its first draw */
    HLE_OP_VIS_BEGIN = 17,      /* no payload: a visibility test starts */
    HLE_OP_VIS_END = 18,        /* HleCmdVis: it ends, its pixel count goes to index */
    HLE_OP_STATE_PART = 19,     /* HleCmdStatePart: bytes of the draw state that changed */
    HLE_OP_GAMMA = 20,          /* HleCmdGamma: the display's gamma ramp */
    HLE_OP_OVERLAY = 21,        /* HleCmdOverlay: the video overlay, from the next swap */
    HLE_OP_COPY = 22,           /* HleCmdCopy: CopyRects between two surfaces */
    HLE_OP_ZREAD = 23,          /* HleCmdZRead: a depth-stencil surface read as colour */
    HLE_OP_UPLOAD = 25,         /* HleCmdUpload: guest-memory texels into a surface */
    HLE_OP_ZWRITE = 26,         /* HleCmdZWrite: a colour surface's words as depth */
};

/* Colour surface dst gets depth-stencil surface src (same size) as the
 * NV2A's memory holds it, one 32-bit word per pixel (D24S8: depth << 8 |
 * stencil): channel c (r, g, b, a) of dst is byte map[c] of the word, or 1
 * for map[c] = 4. A texture over a depth buffer's memory samples dst.
 * map[0] = HLE_ZREAD_HALVES: dst is twice src's width in 16-bit texels, one
 * a half word (the even texel the low half), as 16-bit luminance. */
#define HLE_ZREAD_HALVES 5
typedef struct HleCmdZRead {
    uint32_t dst, src;          /* object ids */
    uint8_t map[4];
} HleCmdZRead;

/* The reverse: depth-stencil surface dst is bound over the memory of 32-bit
 * colour surface src (same size), which a pass wrote after dst's last
 * write. Each word a << 24 | r << 16 | g << 8 | b there is D24S8 depth << 8 |
 * stencil to the NV2A: dst's depth becomes the word's top 24 bits (the back
 * end skips it when dst is newer). */
typedef struct HleCmdZWrite {
    uint32_t dst, src;          /* object ids */
} HleCmdZWrite;

/* The video overlay (UpdateOverlay / EnableOverlay): on = 0 turns it off;
 * else w x h B8G8R8A8 texels at arena_off become the overlay image, its src
 * rectangle drawn over each presented frame at dst, in back-buffer pixels
 * of a fb_w x fb_h frame. */
typedef struct HleCmdOverlay {
    uint32_t on, arena_off, w, h;
    int32_t src[4], dst[4];     /* left, top, right, bottom */
    uint32_t fb_w, fb_h;
} HleCmdOverlay;

/* SetGammaRamp: D3DGAMMARAMP (red, green, blue [256] bytes); applied from
 * the next swap (the DAC loads it at vblank) or at once (D3DSGR_IMMEDIATE). */
typedef struct HleCmdGamma {
    uint32_t immediate;
    uint8_t rgb[3][256];
} HleCmdGamma;

/* A changed run of HleCmdState: len bytes at byte offset off follow. */
typedef struct HleCmdStatePart {
    uint32_t off, len;
} HleCmdStatePart;

/* D3D visibility test indices: native's report table is 64 pages of 256
 * (5659 0x1c4280; the teardown at 0x1ca01f frees 0x40 pages). */
#define HLE_VIS_INDICES 16384
/* A test known only by its report (push-buffer mode's GET_REPORT): its
 * count goes to the report, not to an index. */
#define HLE_VIS_NO_INDEX 0xFFFFFFFFu

/* The time stamp HLE reports for a finished test (the P2 constant: the
 * native call returned it under xemu). */
#define HLE_VIS_TIMESTAMP 0x0011223344556677ull

typedef struct HleCmdVis {
    uint32_t index;             /* D3D visibility test index */
    uint32_t div;               /* render-target samples per guest pixel (supersampling) */
    uint32_t report;            /* guest PA of the device's report for it, 0 none */
    uint32_t pad;
    uint64_t expect;            /* the value GetVisibilityTestResult waits for */
} HleCmdVis;

typedef struct HleCmdHdr {
    uint16_t op;
    uint16_t rsvd;
    uint32_t len;               /* payload bytes */
} HleCmdHdr;

static inline const void *hle_cmd_payload(const HleCmdHdr *h)
{
    return (const uint8_t *)h + sizeof(HleCmdHdr);
}

#define HLE_CMD_MAX_SURF  6
#define HLE_CMD_MAX_RECTS 8

typedef struct HleSurfDesc {
    uint32_t id;                /* front-end object id */
    uint32_t xfmt;              /* X_D3DFORMAT */
    uint32_t w, h;              /* pixels (multisample-scaled) */
    uint32_t pa;                /* guest data PA */
    uint32_t pitch;             /* bytes (0 = swizzled) */
    uint32_t role;              /* HleSurfRole, 0 for application surfaces */
    /* A texture with more than one level or face that is rendered to owns one
     * image for all of them (levels, faces: 0 = 1); its level and face
     * surfaces are views into it (parent: the owner's id). */
    uint32_t parent;
    uint16_t levels, faces;
    uint16_t level, face;
} HleSurfDesc;

typedef struct HleCmdDevice {
    uint32_t disp_w, disp_h;    /* display (front buffer) size */
    uint32_t present_id;        /* surface SWAP presents */
    uint32_t nsurf;
    HleSurfDesc surf[HLE_CMD_MAX_SURF];
} HleCmdDevice;

typedef struct HleCmdObj {
    HleSurfDesc s;
} HleCmdObj;

typedef struct HleCmdObjDestroy {
    uint64_t after;             /* destroy once this timeline value completes */
    uint32_t id, pad;
} HleCmdObjDestroy;

typedef struct HleCmdSetRt {
    uint32_t rt, ds;            /* object ids, 0 = none */
} HleCmdSetRt;

typedef struct HleCmdViewport {
    uint32_t x, y, w, h;        /* render-target pixels */
    float minz, maxz;
    float offx, offy;           /* screen-space offset (SetScreenSpaceOffset + 0.53125),
                                   render-target pixels: the NV2A viewport offset fixed
                                   function adds (programs: in c[59]) */
    /* SetScissors' rectangle, render-target pixels: rendering is clipped to it
     * instead of the viewport (the NV2A surface clip); clip 0 = the viewport */
    uint32_t clip;
    int32_t cx1, cy1, cx2, cy2;
} HleCmdViewport;

typedef struct HleRect {
    int32_t x1, y1, x2, y2;
} HleRect;

typedef struct HleCmdClear {
    uint32_t flags;             /* D3DCLEAR_* */
    uint32_t color;             /* D3DCOLOR 0xAARRGGBB */
    float z;
    uint32_t stencil;
    uint32_t nrects;            /* 0 = the whole render target */
    HleRect rect[HLE_CMD_MAX_RECTS];
} HleCmdClear;

/* CopyRects: rectangle i of surface src (pixels, clipped to both surfaces)
 * goes to dst with its top-left at (x[i], y[i]). */
typedef struct HleCmdCopy {
    uint32_t src, dst;          /* object ids */
    uint32_t resolve;           /* 1: the whole source filtered onto the whole
                                   destination (a copy swap's downsample), no rects */
    uint32_t nrects;
    HleRect rect[HLE_CMD_MAX_RECTS];
    int32_t x[HLE_CMD_MAX_RECTS], y[HLE_CMD_MAX_RECTS];
} HleCmdCopy;

/* Texels the CPU wrote (linear rows of pitch bytes at guest PA pa, the
 * destination's format): rectangle i goes to object dst's image with its
 * top-left at (x[i], y[i]), in guest pixels. With guest RAM the GPU can't
 * read in place (not imported: the native core's), the rectangles' rows
 * from guest PA lo on are copied into the arena at arena - 1. */
typedef struct HleCmdUpload {
    uint32_t dst;               /* object id */
    uint32_t pa, pitch;
    uint32_t lo, arena;         /* arena: 0 = the rows are read in guest RAM */
    uint32_t nrects;
    HleRect rect[HLE_CMD_MAX_RECTS];
    int32_t x[HLE_CMD_MAX_RECTS], y[HLE_CMD_MAX_RECTS];
} HleCmdUpload;

typedef struct HleCmdSwap {
    uint64_t value;
    uint32_t swap_id;
    uint32_t surf;              /* object id presented */
    uint32_t dump_off;          /* observe shadow: arena space for a copy of surf
                                   (32-bit texels), HLE_SWAP_NO_DUMP = none */
    uint32_t src_w, src_h;      /* the part of surf shown, from its top left (guest
                                   pixels, the antialiasing's included); 0: all */
    uint32_t pad;
} HleCmdSwap;

#define HLE_SWAP_NO_DUMP 0xFFFFFFFFu

typedef struct HleCmdFence {
    uint64_t value;
} HleCmdFence;

typedef struct HleCmdReadback {
    uint64_t value;                 /* 0: no submission of its own */
    uint32_t surf, pa, pitch;
    /* arena_off != HLE_RB_DIRECT: the subresource (level, face of a texture
     * image; a level / face surface names its own) is copied tightly packed
     * into the arena at arena_off, w x h texels; the front end swizzles it
     * into guest memory. Else linear rows go straight to guest RAM at pa,
     * the image filtered down to w x h first when those are set; with guest
     * RAM the GPU can't write (not imported: the native core's), to the
     * arena at fb - 1 instead (fb_len bytes), landed at pa when the GPU
     * reaches the value (hle_tl_land). */
    uint32_t arena_off, level, face, w, h;
    uint32_t fb, fb_len;            /* fb 0: none */
} HleCmdReadback;
#define HLE_RB_DIRECT 0xFFFFFFFFu
/* arena_off: the subresource packed as above into the landing buffer (the
 * arena without one) at fb - 1, swizzled into guest memory at the value
 * (hle_tl_land_swz) */
#define HLE_RB_PACKED 0xFFFFFFFEu

/* ---- P3 draws ------------------------------------------------------------- */

#define HLE_RS_COUNT   166      /* 5659 D3DRS_MAX */
#define HLE_TSS_STAGES 4
#define HLE_TSS_COUNT  32       /* per stage */

typedef struct HleCmdState {
    uint32_t rs[HLE_RS_COUNT];
    uint32_t tss[HLE_TSS_STAGES][HLE_TSS_COUNT];
    float mvp[16];              /* world * view * projection, column-major */
    uint32_t fvf;               /* vertex shader handle or FVF */
    uint32_t xs, ys;            /* render target multisample scale (XYZRHW mapping) */
    uint32_t psh;               /* a pixel shader is set: rs[0..56] is its program */
    uint32_t spec;              /* vertex programs: oD1 / oB1 reach the pixel stage
                                   (NV2A SPECULAR_ENABLE: SPECULARENABLE or device
                                   flag 0x40, 5659 0x1c7390), else (0, 0, 0, 1) */
    uint32_t aa;                /* 1: the NV2A antialiases the target (device flags
                                   rt_ms_bit, the multisample types): it scales screen
                                   positions by xs/ys and D3D's are logical pixels;
                                   0: D3D scales by xs/ys itself (supersampling) */
    uint32_t zclamp;            /* NV2A ZMIN_MAX_CONTROL ZCLAMP_EN: depth past the near
                                   and far planes is clamped to them, not clipped */
    uint32_t pad;               /* 8-byte words (hle_fe_state) */
} HleCmdState;

/* Texels are tightly packed in the arena: for each face (6 for cubes), each
 * level, rows of width * bytes per texel (DXT: block rows). */
typedef struct HleCmdTex {
    uint32_t id;                /* front-end object id */
    uint32_t xfmt;              /* X_D3DFORMAT code */
    uint32_t w, h, d, levels, faces;
    uint32_t arena_off, arena_len;
    uint32_t pad;
} HleCmdTex;

#define HLE_DRAW_MAX_BIND 16
#define HLE_DRAW_ATTRS    16

typedef struct HleCmdDraw {
    uint32_t prim;              /* Vulkan topology after conversion (VkPrimitiveTopology) */
    uint32_t count;             /* indices when ib_off != ~0, else vertices */
    uint32_t vb_off, vb_len;    /* arena */
    uint32_t stride;
    uint32_t ib_off;            /* arena, 16-bit indices; ~0 = none */
    uint32_t fvf;
    uint32_t imm;               /* 1 = immediate-mode layout: 8 float4 per vertex at
                                   locations 0..7 (position, normal, diffuse, specular,
                                   texcoord 0..3) */
    uint32_t tex[HLE_TSS_STAGES];   /* object ids, 0 = none */
    uint32_t tex_unset;         /* bit s: no texture set (D3D's Textures[s] NULL), as
                                   against one HLE could not prepare */
    uint32_t tex_lin;           /* bit s: the stage's format is linear, sampled by texel
                                   coordinates (whatever the layout of a target there) */
    uint32_t tex_ord;           /* bits 2s..2s+1: the stage's 32-bit byte order where it
                                   samples an A8R8G8B8 target of another (HLE_DRAW_ORD_*) */
    uint32_t tex_rect[HLE_TSS_STAGES];  /* a linear stage's own size, its IMAGE_RECT
                                           (width 28:16, height 12:0); 0 for the others */
    /* vertex programs and declarations (P4, pulled forward) */
    uint64_t vsh;               /* program hash (HLE_OP_VSH); 0 = fixed function */
    uint32_t consts_off;        /* program draws: arena, 192 vec4 constants */
    float zscale;               /* 1 / depth-buffer maximum (programs, W-buffering);
                                   2^-n for the integer formats (HLE_ZNORM_GLSL) */
    uint32_t guest_vb;          /* read in place from guest RAM (offset = PA): bindings
                                   (bit i), the FVF stream (HLE_DRAW_GUEST_FVF) */
    uint32_t nbind;             /* > 0: the layout below replaces fvf/vb_off/stride */
    uint32_t bind_off[HLE_DRAW_MAX_BIND];       /* arena, or PA (guest_vb) */
    uint32_t bind_stride[HLE_DRAW_MAX_BIND];
    uint32_t attr[HLE_DRAW_ATTRS];  /* by location: NV2A format byte | binding << 8 |
                                       offset << 16; format count 0 = unbound */
    /* FF draws: the FF uniform block (shader/ff.h HLE_FFU_*) in the arena */
    uint32_t ff_ubo;            /* HLE_DRAW_NO_UBO = none */
    uint32_t ff_lights;         /* enabled lights: count | D3DLIGHTTYPE of light i <<
                                   (4 + 2 i); HLE_DRAW_LIT_DATA: the block holds the
                                   material and lights */
    uint32_t dbg;               /* debug: 1 = from a push-buffer run (HAKUX_HLE_PBTINT) */
    uint32_t diffuse_cur;       /* FVF draws without diffuse: arena offset + 1 of the
                                   diffuse register's current value (float4), 0 = none */
    float wscale;               /* untransformed FF draws: 1 / the projection's W-buffer
                                   far w (W-buffering stores w / wfar); 0 = keep z */
} HleCmdDraw;

#define HLE_DRAW_GUEST_FVF (1u << 31)
#define HLE_DRAW_NO_UBO   0xFFFFFFFFu
#define HLE_DRAW_LIT_DATA (1u << 31)
/* tex_ord: 32-bit texel byte orders other than A8R8G8B8's (names MSB first) */
#define HLE_DRAW_ORD_ABGR 1u        /* A8B8G8R8: bytes R G B A */
#define HLE_DRAW_ORD_BGRA 2u        /* B8G8R8A8: bytes A R G B */
#define HLE_DRAW_ORD_RGBA 3u        /* R8G8B8A8: bytes A B G R */

/* Vertex program microcode: nslots 4-dword instructions follow. */
typedef struct HleCmdVsh {
    uint64_t hash;
    uint32_t nslots;
    uint32_t pad;
} HleCmdVsh;

#endif
