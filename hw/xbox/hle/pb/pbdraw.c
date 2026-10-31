/*
 * hakuX HLE GPU — push-buffer mode's drawing (pbdraw.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/pb/kelvin.h"
#include "hw/xbox/hle/front/cmd.h"
#include "hw/xbox/hle/front/fe.h"
#include "hw/xbox/hle/pb/pbdraw.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/pb/pbmode.h"
#include "hw/xbox/hle/pb/pbstate.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/guest_ram.h"
#include "hw/xbox/hle/vk/quirks.h"
#include "hw/xbox/hle/pb/pbres.h"
#include "hw/xbox/hle/tex/xdk.h"
#include "hw/xbox/hle/tex/xdk_res.h"
#include "hw/xbox/hle/tex/xg_swizzle.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/front/arena.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/shader/ffhw.h"
#include "hw/xbox/hle/shader/vsh.h"

#include "subprojects/nv2a_vsh_cpu/src/nv2a_vsh_emulator.h"

#define XXH_INLINE_ALL
#include "xxhash.h"                     /* span_hash */

/* Kelvin methods read here */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200u   /* x 15:0, width 31:16 */
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204u
#define NV097_SET_SURFACE_FORMAT          0x0208u   /* colour 3:0, zeta 7:4, type 11:8, log2 w/h */
#define NV097_SET_SURFACE_PITCH           0x020Cu   /* colour 15:0, zeta 31:16 */
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210u
#define NV097_SET_SURFACE_ZETA_OFFSET     0x0214u
#define NV097_SET_CONTROL0                0x0290u   /* bit 12: float depth */
#define NV097_SET_WINDOW_CLIP_TYPE        0x02B4u   /* 0 inclusive, 1 exclusive */
#define NV097_SET_WINDOW_CLIP_HORIZONTAL  0x02C0u   /* 8 of them: x min 11:0, max 27:16 */
#define NV097_SET_WINDOW_CLIP_VERTICAL    0x02E0u   /* 8 of them: y min 11:0, max 27:16 */
#define NV097_FLIP_INCREMENT_WRITE        0x012Cu
#define NV097_SET_ZPASS_PIXEL_COUNT_ENABLE 0x17CCu  /* 1: a visibility test counts */
#define NV097_SET_ZSTENCIL_CLEAR_VALUE    0x1D8Cu
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90u
#define NV097_CLEAR_SURFACE               0x1D94u   /* D3DCLEAR flags */
#define NV097_SET_CLEAR_RECT_HORIZONTAL   0x1D98u   /* x1 15:0, x2 31:16 (inclusive) */
#define NV097_SET_CLEAR_RECT_VERTICAL     0x1D9Cu
#define NV097_SET_VIEWPORT_OFFSET         0x0A20u   /* .. 0x0A2C: constant 59 */
#define NV097_SET_VIEWPORT_SCALE          0x0AF0u   /* .. 0x0AFC: constant 58 */
#define NV097_SET_TRANSFORM_PROGRAM       0x0B00u   /* .. 0x0B7C */
#define NV097_SET_TRANSFORM_CONSTANT      0x0B80u   /* .. 0x0BFC */
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720u  /* 16 of them */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760u  /* type 3:0, size 7:4, stride 31:8 */
#define NV097_SET_BEGIN_END               0x17FCu
#define NV097_ARRAY_ELEMENT16             0x1800u
#define NV097_ARRAY_ELEMENT32             0x1808u
#define NV097_DRAW_ARRAYS                 0x1810u   /* start 23:0, count - 1 31:24 */
#define NV097_INLINE_ARRAY                0x1818u
#define NV097_SET_TRANSFORM_DATA          0x1E80u   /* .. 0x1E8C: a state program's v0 */
#define NV097_LAUNCH_TRANSFORM_PROGRAM    0x1E90u   /* runs the state program at the slot */
#define NV097_SET_TRANSFORM_EXECUTION_MODE 0x1E94u  /* mode 1:0: 0 fixed, 2 program */
#define NV097_SET_TRANSFORM_PROGRAM_LOAD  0x1E9Cu
#define NV097_SET_TRANSFORM_PROGRAM_START 0x1EA0u
#define NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4u
#define NV097_SET_TEXTURE_OFFSET          0x1B00u   /* stage s at + 64 s */
#define NV097_SET_TEXTURE_FORMAT          0x1B04u   /* a D3D header's Format */
#define NV097_SET_TEXTURE_CONTROL0        0x1B0Cu   /* bit 30: enable */
#define NV097_SET_TEXTURE_CONTROL1        0x1B10u   /* linear pitch 31:16 */
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1Cu   /* linear width 31:16, height 15:0 */
#define NV097_SET_TEXTURE_PALETTE         0x1B20u   /* offset 31:6 | length 3:2 (256 >> n) */

#define NV097_SET_VERTEX3F                0x1500u   /* x y z: the third word emits */
#define NV097_SET_VERTEX4F                0x1518u   /* x y z w: the fourth word emits */
#define NV097_SET_VERTEX_DATA2F_M         0x1880u   /* attribute a at + 8 a */
#define NV097_SET_VERTEX_DATA2S           0x1900u   /* ... + 4 a: two shorts */
#define NV097_SET_VERTEX_DATA4UB          0x1940u   /* ... + 4 a: four bytes, unit range */
#define NV097_SET_VERTEX_DATA4S_M         0x1980u   /* ... + 8 a: four shorts */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00u   /* ... + 16 a */

/* SET_BEGIN_END's primitives Vulkan has no topology for (converted to
 * indices: hle_draw_prim_indices) */
#define NV2A_PRIM_LINE_LOOP 3u
#define NV2A_PRIM_QUADS     8u

#define VPSCL 58                            /* the viewport's constant slots */
#define VPOFF 59
#define MAX_INDICES (1u << 20)
#define ATTRS 16
#define MAX_IMM 65535u                      /* immediate vertices a draw (16-bit indices) */
#define MAX_INLINE (1u << 20)               /* inline-array words a draw */
#define MAX_SENT 4096
#define SENT_SLOTS (2 * MAX_SENT)

#define MAX_OBJS 128
#define MAX_ZREADS 8
#define SCAN_IDLE_NS 100000000              /* nothing presented this long: the title isn't flipping */
#define TEX_SLOTS 2048u                     /* the texture table (open addressing) */
#define TEX_MAX 1536u                       /* ... this full: the least recently used go (tex_evict) */

/* A target's memory as it was when its image and it last matched: the
 * writes reported to its pages (mem/barrier.c hle_mem_wseq_sum), its first
 * and last page's, and its own bytes on those two pages when it shares them
 * (a neighbour's writes there don't make its memory newer) */
typedef struct CohSnap {
    uint64_t sum, h0, hl;
    uint64_t bsum;                      /* by blocks (hle_mem_wseq_blocks): a coarse first look */
    uint32_t w0, wl;
    bool valid;
} CohSnap;

typedef struct PbObj {
    uint32_t id;
    HleSurfDesc d;
    uint64_t bound;                     /* when last bound as a target (g_pd.binds) */
    bool cube;                          /* a cube texture's image, its faces' targets views */
    uint64_t written;                   /* g_pd.wseq at its last draw, clear or copy */
    uint64_t edge_hash[2];              /* its bytes on its first and last page (rt_cpu_sync) */
    bool cpu_used;                      /* the CPU wrote its memory (writeback) */
    uint64_t mem_seq;                   /* g_pd.wseq when a texture took its memory as the
                                           title's data: superseded while written <= it */
    /* its memory against its image (coh_*, colour targets) */
    uint64_t synced;                    /* written when the two last matched */
    CohSnap snap;                       /* its memory's write counts then */
    uint64_t land_v, land_w;            /* its image's landing asked for: value, written then */
    uint64_t land_last;                 /* its latest landing's value (in flight till completed) */
    uint64_t shown_seq;                 /* g_coh.shows when it or a frame over it was last shown */
    uint64_t absorb_swap;               /* when a neighbour's writes on its edge pages were last
                                           taken in (coh_mem_newer: once a frame) */
    uint32_t vw0, vwl;                  /* ... its edge pages' counts its own bytes there were
                                           last found unchanged at */
    uint64_t mem_pend;                  /* a copy queued into its memory lands at this value */
    uint32_t armed_drops;               /* hle_barrier_guard_drops() when guarded (0: not) */
    uint32_t mn_epoch;                  /* coh_mem_newer's answer (mn) as of this write epoch */
    bool mn;
    bool learned;                       /* the guest reads it: landed at fences (coh_fence) */
    bool want;                          /* ... drawn into since its last landing */
    uint8_t unread;                     /* its landings in a row the guest didn't take */
    bool display;                       /* shown: the front-buffer path keeps its memory */
} PbObj;

/* A texture the stage registers described: the back end's image of it,
 * defined again when its memory was written (checked once a frame) or its
 * palette changed. */
typedef struct PbTex {
    uint32_t key[4];                    /* offset, format, size, palette; id 0: free */
    uint32_t id;
    uint32_t pa, bytes;                 /* the memory its texels came from */
    uint32_t pal_hash;
    uint64_t checked;                   /* the epoch it was last checked at */
    uint64_t edge_hash[2];              /* its bytes on its first and last pages, at definition */
    bool ok;                            /* defined (else it can't be: not again) */
} PbTex;

static struct {
    bool on, device;
    uint32_t disp_w, disp_h;            /* the device's display (guest pixels) */
    uint32_t r[0x2000 / 4];             /* the methods read here, as last written */
    bool rt_dirty;
    uint32_t rt, ds;                    /* the bound targets' object ids */
    uint32_t rt_w, rt_h;                /* the targets' physical size */
    uint32_t aa_x, aa_y;                /* their antialiasing scale (1, 2) */
    uint32_t frame_rt, frame_aa_x, frame_aa_y;  /* the last colour target bound that
                                                   covers the display, its antialiasing
                                                   (swap) */
    uint32_t shown_ax, shown_ay;        /* the antialiasing of the frame shown (shown_id) */
    PbObj objs[MAX_OBJS];
    unsigned nobjs;
    uint32_t made;                      /* the object last made (evict_slot) */
    uint64_t binds;                     /* targets bound (PbObj.bound's clock) */
    /* colour textures over a depth buffer's memory: their images, converted
     * from the depth image (HLE_OP_ZREAD) at each use, or the CPU's bytes
     * when it wrote the memory since (zread) */
    struct {
        uint32_t id, src, pa, w, h, xfmt;
        uint64_t edge_hash[2];          /* the memory's bytes on its first and last page */
    } zr[MAX_ZREADS];
    uint64_t zr_uploads;                /* CPU-written zread memory uploaded (debug) */
    unsigned nzr;
    uint32_t next_id;
    uint64_t clears, swaps;             /* swaps: FLIP_INCREMENT_WRITE's */
    uint32_t swap_ids;                  /* every swap's id (those and PCRTC_START's) */
    uint64_t swaps_seen;                /* swaps at the last PCRTC_START */
    uint64_t pcrtc_presents;
    uint64_t epoch;                     /* a frame or 4 ms of draws: textures are checked
                                           for CPU writes once an epoch */
    uint64_t rt_uploads;                /* render targets the CPU wrote (rt_cpu_sync) */
    uint64_t writebacks;                /* small targets copied back to memory */
    uint32_t shown_id;                  /* the object last presented */
    int64_t shown_ns;                   /* ... when (atomic: hle_pbd_scan_due) */
    uint64_t scan_presents;             /* frames on screen the CPU changed, shown again */
    uint64_t shown_written;             /* ... its written then */
    uint64_t wseq;                      /* draws, clears and copies into objects */
    PbObj *rt_obj;                      /* the bound colour target's */
    bool rt_fresh;                      /* no clear or draw into it since its bind, the last
                                           swap or the last pass (rt_first_write) */
    PbObj *ds_obj;                      /* the bound depth target's (ds_written) */
    uint64_t display_repeats;           /* PCRTC_START writes of an unchanged shown object */
    bool overlay_on;
    uint64_t overlays;
    int64_t kick_ns;                    /* the last submit boundary */
    unsigned kick_draws;
    bool vis_open;                      /* a visibility test counts (ZPASS enabled) */
    bool vis_unsent;                    /* reports ended since the last submit boundary */
    uint64_t vis_tests, vis_kicks;
    /* frames swapped, waiting for their flip: decode thread in, vblank out
     * (pend_flipped: one whose flip came before it, hle_pb_flipped) */
    uint32_t pend[8];
    bool pend_flipped[8];
    uint32_t pend_head, pend_tail;
    /* D3D's immediate flip (its trap's handler, before the frame's
     * FLIP_INCREMENT_WRITE): ahead_pa, the bound target's memory while it
     * has a frame drawn since the last swap (set at each trap, atomic);
     * flipped_pa, a flip there: the swap that comes is one flipped */
    uint32_t ahead_pa, flipped_pa;
    uint64_t swap_wseq;                 /* wseq at the last swap */
    uint64_t flipped_presents;
    uint64_t shown;
    int64_t flipped_ns;                 /* the last flip (hle_now_ns) */
    /* the transform unit: program memory and constants, their load points */
    uint32_t pmem[HLE_VSH_MAX_SLOTS][4];
    uint32_t pload, cload;
    uint32_t consts[HLE_VSH_CONSTANTS][4];
    uint32_t xdata[4];                  /* SET_TRANSFORM_DATA: the next state program's v0 */
    /* the last program draw's constant block (constants and current
     * attributes), reused while neither changed and the submission it was
     * tagged for is still the one being gathered */
    uint64_t cgen;                      /* bumped as consts or cur change */
    uint64_t ffgen;                     /* bumped as a register ff_consts reads changes */
    uint64_t blk_gen, blk_ffgen, blk_value;
    uint32_t blk_off;
    bool blk_ok, blk_ff;
    uint64_t blk_reused;
    uint64_t vb_place, vb_copied;       /* vertex ranges read in place, copied */
    uint64_t seq_draws;                 /* DRAW_ARRAYS of one run in order, drawn without indices */
    uint64_t bnd_kicks;                 /* submissions closed for a barrier's waiter */
    uint64_t sent[SENT_SLOTS];              /* program hashes the back end has (a set; 0 free) */
    unsigned nsent;
    uint32_t pgen;                          /* bumped by program memory writes */
    /* the draw being gathered: indices, immediate vertices (each the 16
     * attributes' current values as its position's last word came), or an
     * inline array's words */
    bool in_draw;
    uint32_t prim;
    uint32_t dm[12];                    /* debug: the draw's first methods (method << 16 | value) */
    unsigned ndm;
    float cur[ATTRS][4];
    float *imm;
    uint32_t nimm;
    uint32_t *inl;
    uint32_t ninl;
    uint32_t *idx;
    uint32_t nidx, cap;
    uint16_t *idx16;
    HleCmdState st;
    bool st_sent;
    uint32_t st_gen;                    /* hle_kelvin_state_gen of st */
    uint32_t vp_key[2];
    uint32_t wc_key[17];                /* the window clip the viewport was sent with */
    uint64_t draws, ff, skipped;
    PbTex *tex;                         /* TEX_SLOTS */
    unsigned ntex;
    uint64_t tex_defs, tex_flushes;
} g_pd;

#define R(m) (g_pd.r[(m) >> 2])
static bool coh_managed(const PbObj *o, uint32_t *len);
static void coh_synced(PbObj *o, uint32_t len);
static void coh_forget(const PbObj *o);
static void coh_reset(void);
static uint64_t coh_fill(uint32_t pa, uint32_t len, bool read);
static void coh_adopt(void);
static bool coh_mem_newer(PbObj *o);
static void coh_pend_wait(uint32_t pa, uint32_t len);
static void coh_pend_wait_inplace(uint32_t pa, uint32_t len);
static void coh_wait(uint64_t v);
static uint64_t coh_land(PbObj *o);
static void coh_serve(void);
static void coh_shown(PbObj *o);
static bool coh_display_over(uint32_t pa, uint32_t len);
static bool coh_len(const PbObj *o, uint32_t *len);
/* Presents (coh_shown: a swap, a display start), and how many ago a frame
 * buffer still counts as one (a flip chain's take turns) */
static uint64_t g_coh_shows;
#define COH_SHOWN_RECENT 32
/* The linear target a blit's rows go into as they are copied, image and
 * memory both (hle_pbd_copy_begin notes it, hle_pbd_blit uploads them), and
 * which of the two was the newer then */
static struct {
    uint32_t id;
    bool mem_newer, img_newer;
} g_coh_rows;

/* debug.hakux.pbdraws: draws still to log (drawlog), clears with them */
static int g_dlog_left;

/* The decode thread's waits, for the log line (hle_pbd_phase) */
static volatile int g_phase;            /* 0 none, 1 arena space, 2 ring space */

static uint32_t arena(uint32_t len, uint8_t **ptr)
{
    g_phase = 1;
    uint32_t off = hle_draw_arena_alloc(len, ptr);
    g_phase = 0;
    return off;
}

/* A draw's 16-bit indices: the index buffer is bound at the arena's start
 * (firstIndex), so 16 bytes apart will do where 256 wasted ~120 bytes a
 * draw, and back-to-back draws' indices end up contiguous more often (the
 * back end's strip batching). Vertex copies stay 256 apart: their binding
 * offsets then match from draw to draw, which that batching needs. */
static uint32_t arena_idx(uint32_t len, uint8_t **ptr)
{
    g_phase = 1;
    uint32_t off = hle_draw_arena_alloc16(len, ptr);
    g_phase = 0;
    return off;
}

static void *begin(uint16_t op, uint32_t len)
{
    g_phase = 2;
    void *p = hle_fe_begin(op, len);
    g_phase = 0;
    return p;
}

const char *hle_pbd_phase(void)
{
    return g_phase == 1 ? "arena" : g_phase == 2 ? "ring" : NULL;
}

static void vsh_forget(void);

/* The method words ff_consts reads, a bit each (method >> 2): a change to
 * one bumps ffgen, and a fixed-function draw reuses the last block while
 * none moved (emit). */
static uint64_t g_ff_regs[0x2000 / 4 / 64];

static void ff_regs_init(void)
{
    static const uint32_t ranges[][2] = {   /* [first, end) */
        { 0x03A8, 0x03B8 },     /* emission, scene ambient's alpha */
        { 0x043C, 0x0440 },     /* point size */
        { 0x0480, 0x07C0 },     /* modelview, inverse, composite, texture matrices */
        { 0x0840, 0x0940 },     /* texgen planes */
        { 0x09D0, 0x0A60 },     /* fog plane, specular, ambient, viewport offset, point, eye */
        { 0x1000, 0x1400 },     /* lights */
        { 0x1E28, 0x1E40 },     /* back specular */
    };
    memset(g_ff_regs, 0, sizeof(g_ff_regs));
    for (unsigned i = 0; i < ARRAY_SIZE(ranges); i++) {
        for (uint32_t m = ranges[i][0]; m < ranges[i][1]; m += 4) {
            g_ff_regs[m >> 8] |= 1ull << ((m >> 2) & 63);
        }
    }
}

bool hle_pbd_start(void)
{
    memset(&g_pd, 0, sizeof(g_pd));
    ff_regs_init();
    vsh_forget();                           /* program memory starts empty */
    g_pd.next_id = 1;
    g_pd.idx = g_new(uint32_t, MAX_INDICES);
    g_pd.idx16 = g_new(uint16_t, MAX_INDICES * 3 / 2 + 8);
    g_pd.cap = MAX_INDICES;
    g_pd.tex = g_new0(PbTex, TEX_SLOTS);
    g_pd.imm = g_new(float, (size_t)MAX_IMM * ATTRS * 4);
    g_pd.inl = g_new(uint32_t, MAX_INLINE);
    for (unsigned a = 0; a < ATTRS; a++) {
        g_pd.cur[a][3] = 1.0f;
    }
    if (!hle_back_running() && hle_back_start() != 0) {
        hle_log("hakuX-hle-pb", "no back end: push-buffer mode draws nothing");
        return false;
    }
    hle_disp_present_late(true);
    coh_reset();
    hle_barrier_set_guard_fill(coh_fill);
    g_pd.on = true;
    return true;
}

void hle_pbd_stop(void)
{
    hle_disp_present_late(false);
    hle_barrier_set_guard_fill(NULL);
    hle_barrier_set_stale(0, 0, NULL);
    g_pd.on = false;
}

uint64_t hle_pbd_overlays(void)
{
    return g_pd.overlays;
}

uint64_t hle_pbd_display_repeats(void)
{
    return g_pd.display_repeats;
}

uint64_t hle_pbd_const_reuses(void)
{
    return g_pd.blk_reused;
}

uint64_t hle_pbd_writebacks(void)
{
    return g_pd.writebacks;
}

void hle_pbd_vb_counts(uint64_t *in_place, uint64_t *copied, uint64_t *bnd_kicks, uint64_t *unindexed)
{
    *in_place = g_pd.vb_place;
    *copied = g_pd.vb_copied;
    *bnd_kicks = g_pd.bnd_kicks;
    *unindexed = g_pd.seq_draws;
}

void hle_pbd_counts(uint64_t *clears, uint64_t *swaps, unsigned *objects,
                    uint64_t *rt_uploads)
{
    *rt_uploads = g_pd.rt_uploads;
    *clears = g_pd.clears;
    *swaps = g_pd.swaps + g_pd.pcrtc_presents;
    *objects = g_pd.nobjs;
}

void hle_pbd_draw_counts(uint64_t *draws, uint64_t *ff, uint64_t *skipped, uint64_t *tex_defs)
{
    *draws = g_pd.draws;
    *ff = g_pd.ff;
    *skipped = g_pd.skipped;
    *tex_defs = g_pd.tex_defs;
}

static uint32_t tile_pitch(uint32_t bytes);

/* The display AvSetDisplayMode set (hle_pb_display_mode): as wide as its
 * frame buffer's rows (exactly, or in their tile pitch), and as tall as the
 * modes of that width are (640 / 720: 480 lines, 576 at 50 Hz; 1280: 720;
 * 1920: 1080). False when none was set or the pitch fits no such width;
 * *seq: the modes set so far. */
/* An SD mode's frame buffer is 480 or 576 lines, which its mode word doesn't
 * tell: bit 30 (50 Hz) marks PAL modes whose buffer is 480 lines as well,
 * the encoder scaling it (Dead or Alive 3's 0x58040415, 720 x 480). The
 * height the title draws at (cur), when it is one of them; half of it when
 * that is (a back buffer supersampled 2x down, Hunter's 640 x 960); by the
 * refresh only else. Taken as 576, DOA3's 480-line targets were made 576
 * lines (linear_size), their write-backs ran 96 lines past them into its
 * push buffer, and the FMV hung on its faults. */
static uint32_t sd_height(uint32_t cur, unsigned hz)
{
    if (cur == 480 || cur == 576) {
        return cur;
    }
    if (cur == 960 || cur == 1152) {
        return cur / 2;
    }
    return hz == 50 ? 576 : 480;
}

/* The display the video mode shows: its width by the frame buffer's pitch,
 * its height the HD modes' or (SD) sd_height of cur, the height the title
 * draws at now. */
static bool mode_size(uint32_t cur, uint32_t *w, uint32_t *h, uint32_t *seq)
{
    static const uint32_t k_w[] = { 640, 720, 1280, 1920 };
    uint32_t pitch, xfmt;
    unsigned hz;
    *seq = hle_pb_display_mode_get(&pitch, &xfmt, &hz);
    const HleFmtInfo *fi = pitch ? hle_fmt_info(xfmt) : NULL;
    const uint32_t bytes = fi && !(fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT)) ? fi->bpp / 8 : 0;
    for (unsigned i = 0; bytes && i < ARRAY_SIZE(k_w); i++) {
        if (k_w[i] * bytes == pitch || tile_pitch(k_w[i] * bytes) == pitch) {
            *w = k_w[i];
            *h = k_w[i] == 1920 ? 1080 : k_w[i] == 1280 ? 720 : sd_height(cur, hz);
            return true;
        }
    }
    return false;
}

static uint32_t g_mode_seen;            /* the video modes set the display followed */
static bool g_disp_moded;               /* the display is the video mode's size (not a guess) */
/* A savestate's display (hle_pbd_state_set), for the device of the run that
 * loads it: a load starts the core again, and the title doesn't set its
 * video mode again. */
static struct {
    uint32_t w, h;
    bool moded;
} g_saved_disp;
static bool g_restored_display;         /* a savestate loaded: the display set at its first swap */

/* The device, at the first render target: the display is that target's
 * size, or the video mode's (mode_sync). */
static void device(uint32_t w, uint32_t h)
{
    if (g_pd.device) {
        return;
    }
    uint32_t mw, mh;
    /* a mode set since a load: its lines as the state's display had them,
     * not guessed from a target bound midway (sd_height) */
    g_disp_moded = mode_size(g_saved_disp.h ? g_saved_disp.h : h, &mw, &mh, &g_mode_seen);
    if (g_disp_moded) {
        w = mw;
        h = mh;
    } else if (g_saved_disp.w && g_saved_disp.h) {
        w = g_saved_disp.w;
        h = g_saved_disp.h;
        g_disp_moded = g_saved_disp.moded;
    }
    if (g_saved_disp.w && g_saved_disp.h) {
        g_restored_display = false;         /* sized by the mode or the state, not a guess */
    }
    g_saved_disp.w = g_saved_disp.h = 0;
    HleCmdDevice *c = begin(HLE_OP_DEVICE_CREATE, sizeof(*c));
    c->disp_w = w;
    c->disp_h = h;
    c->present_id = 0;
    c->nsurf = 0;
    hle_fe_commit();
    g_pd.device = true;
    g_pd.disp_w = w;
    g_pd.disp_h = h;
    hle_log("hakuX-hle-pb", "device %ux%u", w, h);
}

/* A linear target's size (w x h in, physical): the surface clip bounds only
 * what the passes draw, and titles move it pass by pass (Riddick scissors
 * each light with it). The surface is as wide as its pitch, and a surface
 * as wide as the display (scaled by the antialiasing) as tall as it. Sized
 * by the clip, each clip made another object over the same memory: every
 * one began empty, and Riddick's gameplay drew black, at a few frames a
 * second, one draw per pass. */
/* The NV2A's tile pitches: D3D gives a linear target the smallest that
 * holds its rows (a 720-wide A8R8G8B8 frame buffer, 2880 bytes a row, gets
 * 3072) */
static uint32_t tile_pitch(uint32_t bytes)
{
    static const uint16_t k[] = {
        0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00, 0x0C00, 0x0E00,
        0x1000, 0x1400, 0x1800, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3800, 0x4000,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(k); i++) {
        if (k[i] >= bytes) {
            return k[i];
        }
    }
    return 0;
}

static void linear_size(uint32_t pitch, uint32_t xfmt, uint32_t sx, uint32_t sy, uint32_t *w,
                        uint32_t *h)
{
    const HleFmtInfo *fi = hle_fmt_info(xfmt);
    const uint32_t bytes = fi ? fi->bpp / 8 : 0;
    if (!bytes || !pitch) {
        return;
    }
    const uint32_t pw = MIN(pitch / bytes, 4096u);
    if (pw < *w) {
        return;
    }
    /* a display-wide surface in a padded tile pitch: as wide as the
     * display, not the pitch. Sized by its 768-pixel pitch, Midnight Club
     * 3's 720 x 480 frame buffer showed 48 never drawn columns, a black
     * strip at the right (DOA3, NGB too), and its minimap pass (the clip a
     * 128 x 128 window low in it) made a 768 x 451 object of its own, drawn
     * and never shown */
    static int tilew = -1;                  /* debug: HAKUX_HLE_PB_TILEW=0, the pitch's width (A/B) */
    if (tilew < 0) {
        const char *e = getenv("HAKUX_HLE_PB_TILEW");
        tilew = !e || strcmp(e, "0");
    }
    const uint32_t dw = g_pd.disp_w * sx;
    if (tilew && dw && sy == 1 && *w <= dw && pw > dw && pitch == tile_pitch(dw * bytes)) {
        *w = dw;
        *h = MAX(*h, g_pd.disp_h);
        return;
    }
    /* an antialiased view whose samples are the display's pixels: the same
     * rows, as wide as the display. Dead or Alive 2 draws its glow sources
     * at 360 x 240 2x2 over the 720 x 480 frame it has just drawn, in the
     * same memory and depth, then blurs that. Sized by the pitch, the view
     * was an object of its own that nothing cleared or filled, and the glow
     * of earlier frames stayed behind the fighters. */
    if (tilew && g_pd.disp_w && sx > 1 && *w <= g_pd.disp_w && pw > g_pd.disp_w &&
        pitch == tile_pitch(g_pd.disp_w * bytes)) {
        *w = g_pd.disp_w;
        *h = MAX(*h, g_pd.disp_h);
        return;
    }
    /* not with vertical antialiasing: a 2x2 view of a display-wide surface
     * covers its rows in half the lines (Forza draws 640 x 240 2x2 into its
     * 1280 x 480 scene target, which a texture then samples) */
    if (g_pd.disp_w && pw == g_pd.disp_w * sx && sy == 1) {
        *h = MAX(*h, g_pd.disp_h);
    }
    *w = pw;
}

static void obj_edges_set(PbObj *o);

/* After a savestate's load (hle_pbd_restored): a linear colour target made
 * before this many swaps takes its pixels from guest memory, where the save
 * wrote what the GPU had drawn (hle_pbd_park); made later, it is new. */
static uint64_t g_restored_until;

static void restored_pixels(PbObj *o)
{
    const HleFmtInfo *fi = hle_fmt_info(o->d.xfmt);
    if (g_pd.swaps >= g_restored_until || !fi || (fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT))) {
        return;
    }
    const HleRect r = { 0, 0, (int32_t)o->d.w, (int32_t)o->d.h };
    const int32_t pt[1][2] = { { 0, 0 } };
    hle_res_upload(o->id, o->d.pa, o->d.pitch, 1, &r, pt);
    o->written = ++g_pd.wseq;
    uint32_t len;
    if (coh_managed(o, &len)) {
        coh_synced(o, len);
    }
}

/* The slot a new object takes once all are used: the one bound longest ago
 * (never bound first), and never one still in use: the bound targets, the
 * one shown, a cube or its faces (views into it), a z-read's source, the
 * one just made (a bind makes its colour target, then its depth one).
 * NULL if every one is. */
static PbObj *evict_slot(void)
{
    PbObj *best = NULL;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        bool busy = o->id == g_pd.rt || o->id == g_pd.ds || o->id == g_pd.shown_id ||
                    o->id == g_pd.made || o->cube || o->d.parent;
        for (unsigned j = 0; j < g_pd.nzr && !busy; j++) {
            busy = g_pd.zr[j].src == o->id;
        }
        if (!busy && (!best || o->bound < best->bound)) {
            best = o;
        }
    }
    return best;
}

/* The object for a surface (memory, format, pitch, size), made the first
 * time; 0 if none can be. A linear surface's height is only what its clip
 * reaches (the NV2A has none): a view of the top rows of one already made,
 * same memory, format, pitch and width, is that one, the tallest (its
 * pixels are the same bytes). DOA2 Ultimate draws its character select's
 * model into a 606 x 232 window of its back buffer: an object 311 rows high
 * of its own, drawn and never shown, the model missing. The width stays
 * part of the identity: it comes from the pitch or the display (linear_size),
 * and DOA2's supersampled 1440-wide fight buffer in the same 1536-pixel
 * pitch, shown as the wider one, had 96 never drawn columns. */
static uint32_t object(uint32_t pa, uint32_t xfmt, uint32_t pitch, uint32_t w, uint32_t h)
{
    uint32_t best = 0;
    uint64_t best_area = 0;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const HleSurfDesc *d = &g_pd.objs[i].d;
        if (d->pa != pa || d->xfmt != xfmt || d->pitch != pitch || g_pd.objs[i].cube) {
            continue;
        }
        if (d->w == w && d->h == h && !pitch) {
            return g_pd.objs[i].id;
        }
        const uint64_t area = (uint64_t)d->w * d->h;
        if (pitch && d->w == w && d->h >= h && area > best_area) {
            best = g_pd.objs[i].id;
            best_area = area;
        }
    }
    if (best) {
        return best;
    }
    PbObj *o;
    if (g_pd.nobjs < MAX_OBJS) {
        o = &g_pd.objs[g_pd.nobjs++];
    } else {
        o = evict_slot();
        if (!o) {
            static unsigned s_full;
            if (s_full++ < 8) {
                hle_log("hakuX-hle-pb", "surface %08x: all %u objects in use (first 8 logged)", pa, MAX_OBJS);
            }
            return 0;
        }
        coh_forget(o);
        HleCmdObjDestroy *x = begin(HLE_OP_OBJ_DESTROY, sizeof(*x));
        x->after = 0;
        x->id = o->id;
        hle_fe_commit();
    }
    memset(o, 0, sizeof(*o));
    o->id = g_pd.next_id++;
    g_pd.made = o->id;
    o->d.id = o->id;
    o->d.xfmt = xfmt;
    o->d.w = w;
    o->d.h = h;
    o->d.pa = pa;
    o->d.pitch = pitch;
    o->d.levels = 1;
    o->d.faces = 1;
    uint32_t olen;
    o->display = coh_len(o, &olen) && coh_display_over(pa, olen);  /* a frame buffer's memory */
    o->shown_seq = o->display ? g_coh_shows : 0;
    HleCmdObj *c = begin(HLE_OP_OBJ_CREATE, sizeof(*c));
    c->s = o->d;
    hle_fe_commit();
    if (pitch) {
        hle_mem_written(pa, pitch * h);     /* CPU writes from now on (rt_cpu_sync) */
        obj_edges_set(o);
        restored_pixels(o);
    }
    static unsigned s_logged;
    if (s_logged++ < 32) {
        hle_log("hakuX-hle-pb", "surface %u: %08x format %02x %ux%u pitch %u", o->id, pa, xfmt, w, h,
                pitch);
    }
    return o->id;
}

/* object() for a linear target whose swizzled twin (alt: that format) may
 * already have an object over the same memory and size: that one. D3D
 * clears a swizzled target through a linear view of its memory (a fill is
 * the same in either order): Conker's 256 x 256 bloom target was never
 * cleared, and the pixels its bright pass alpha-tests away kept every
 * earlier frame's glow. Only that way round. A texture over the object
 * addresses it as its own format says, linear by texel and swizzled
 * normalized, not as the object is laid out (vk/draw.c stage_slot): Fable
 * blurs a silhouette drawn through the linear view with a linear texture,
 * and Conker's character shadows are swizzled textures (by texel, they
 * read one white corner). */
static uint32_t object_view(uint32_t pa, uint32_t xfmt, uint32_t alt, uint32_t pitch, uint32_t w,
                            uint32_t h)
{
    for (unsigned i = 0; alt && i < g_pd.nobjs; i++) {
        const HleSurfDesc *d = &g_pd.objs[i].d;
        if (d->pa == pa && d->xfmt == alt && d->w == w && d->h == h) {
            return g_pd.objs[i].id;
        }
    }
    return object(pa, xfmt, pitch, w, h);
}

static const HleSurfDesc *object_desc(uint32_t id);

static PbObj *obj_of(uint32_t id)
{
    for (unsigned i = 0; id && i < g_pd.nobjs; i++) {
        if (g_pd.objs[i].id == id) {
            return &g_pd.objs[i];
        }
    }
    return NULL;
}

static uint64_t span_hash(uint32_t pa, uint32_t len);

/* The bound depth target was (or may have been) written: its written, as a
 * colour target's at its draws (zread: the memory's last writer). */
static void ds_written(void)
{
    if (!g_pd.ds) {
        return;
    }
    if (!g_pd.ds_obj || g_pd.ds_obj->id != g_pd.ds) {
        g_pd.ds_obj = obj_of(g_pd.ds);  /* its slot was reused */
    }
    if (g_pd.ds_obj) {
        g_pd.ds_obj->written = ++g_pd.wseq;
    }
}
static bool edge_dirty(uint32_t page);

/* A linear target's memory rarely starts and ends on a page: its first and
 * last page hold other data too. Judged by those pages' dirty bits alone, a
 * CPU write to that data counted as a write to the target, and its rows
 * were uploaded from memory, which holds no pixels the GPU drew (the NV2A's
 * targets live in images here): Midnight Club 3's 384 x 240 glow target
 * shares its pages with data the CPU writes every frame, and when such a
 * write fell between the glow passes and the pass that adds the glow over
 * the scene, the scene was replaced by the memory's stale white for a frame
 * (sporadic white flashes, the HUD drawn over them). As textures do
 * (tex_written): the pages wholly inside the target count by their dirty
 * bits, the first and last by the target's own bytes on them against their
 * hashes at the last sync. */
static void span_edges_set(uint32_t lo, uint32_t len, uint64_t h[2])
{
    const uint32_t hi = lo + len, p0 = lo & ~4095u, pl = (hi - 1) & ~4095u;
    h[0] = span_hash(lo, MIN(hi, p0 + 4096) - lo);
    h[1] = pl != p0 ? span_hash(pl, hi - pl) : 0;
}

static bool span_written(uint32_t lo, uint32_t len, const uint64_t h[2])
{
    const uint32_t hi = lo + len, p0 = lo & ~4095u, pl = (hi - 1) & ~4095u;
    static int whole = -1;                  /* debug: HAKUX_HLE_PB_RTEDGE=0, every page by its bit (A/B) */
    if (whole < 0) {
        const char *e = getenv("HAKUX_HLE_PB_RTEDGE");
        whole = e && !strcmp(e, "0");
    }
    if (whole) {
        return hle_mem_written(lo, len);
    }
    const bool mid = pl > p0 + 4096 && hle_mem_written(p0 + 4096, pl - p0 - 4096);
    const bool first = edge_dirty(p0) && span_hash(lo, MIN(hi, p0 + 4096) - lo) != h[0];
    const bool last = pl != p0 && edge_dirty(pl) && span_hash(pl, hi - pl) != h[1];
    return mid || first || last;
}

static void obj_edges_set(PbObj *o)
{
    span_edges_set(o->d.pa, o->d.pitch * o->d.h, o->edge_hash);
}

static bool obj_written(const PbObj *o)
{
    return span_written(o->d.pa, o->d.pitch * o->d.h, o->edge_hash);
}

/* A texture was defined from [pa, pa + len): the memory holds the title's
 * data there, newer than the images of the colour targets over it drawn
 * before (memory D3D reused). Those images are superseded until drawn
 * again: no writeback (savestate, kept memory, front buffer) puts them over
 * the data. The memory's dirty bits can't tell: the definition consumes
 * them. Conker
 * draws a 640 x 480 target at boot and later loads its menu's textures into
 * that memory; a savestate wrote the old image back over them, and after a
 * load the save screen's frame and pages sampled it as DXT1 (rainbow). */
static void mem_supersedes(uint32_t pa, uint32_t len)
{
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        const HleFmtInfo *fi = o->id && !o->cube ? hle_fmt_info(o->d.xfmt) : NULL;
        if (!fi || (fi->flags & HLE_FI_DEPTH) || o->written <= o->mem_seq) {
            continue;
        }
        const uint32_t olen = o->d.pitch ? o->d.pitch * o->d.h : o->d.w * o->d.h * (fi->bpp / 8);
        if (o->d.pa < pa + len && o->d.pa + olen > pa) {
            o->mem_seq = g_pd.wseq;
        }
    }
}

static bool obj_superseded(const PbObj *o)
{
    return o->mem_seq && o->written <= o->mem_seq;
}

/* debug: HAKUX_HLE_PB_RTLOG=1 logs the CPU's writes into targets, with the
 * swaps and display starts around them */
static bool rtlog_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("HAKUX_HLE_PB_RTLOG") != NULL;
    }
    return on || g_dlog_left > 0;       /* and among the draw log's draws (debug.hakux.pbdraws) */
}

/* Target id's memory into its image, whatever was written; used: as
 * rt_cpu_upload's. */
static void rt_upload(PbObj *o, uint32_t id, bool used)
{
    obj_edges_set(o);
    const HleRect r = { 0, 0, (int32_t)o->d.w, (int32_t)o->d.h };
    const int32_t pt[1][2] = { { 0, 0 } };
    hle_res_upload(id, o->d.pa, o->d.pitch, 1, &r, pt);
    o->written = ++g_pd.wseq;
    o->cpu_used |= used;
    g_pd.rt_uploads++;
    if (rtlog_on()) {
        hle_log("hakuX-hle-pb", "rt upload %u: %08x %ux%u pitch %u fmt %02x swap %llu", id,
                o->d.pa, o->d.w, o->d.h, o->d.pitch, o->d.xfmt, (unsigned long long)g_pd.swaps);
    }
}

/* The CPU wrote render target id's memory since it was last looked at (a
 * movie frame copied into the frame buffer: Riddick's Bink): its rows go
 * into the image before the GPU draws on it or shows it. True if they did.
 * used: the target counts as one the CPU uses (writeback at each unbind).
 * Not a target the coherence keeps (coh_managed: its memory newer, by its
 * pages' write counts, is uploaded at its next draw, coh_first_write); a
 * copy queued into its memory lands first. */
static bool rt_cpu_upload(uint32_t id, bool used)
{
    PbObj *o = obj_of(id);
    uint32_t len;
    if (!o || !o->d.pitch || coh_managed(o, &len)) {
        return false;
    }
    coh_pend_wait(o->d.pa, o->d.pitch * o->d.h);
    if (!obj_written(o)) {
        return false;
    }
    rt_upload(o, id, used);
    return true;
}

static bool rt_cpu_sync(uint32_t id)
{
    return rt_cpu_upload(id, true);
}

#define WRITEBACK_MAX 16384u                /* bytes: a small target goes back to memory */

/* A writeback of the old paths' (writeback, writeback_range) counts as a
 * write to its memory when it lands (the GPU writes guest RAM in place, no
 * host write reports it): a target the coherence keeps over the same memory
 * (a twin) has its memory newer then */
#define WB_NOTES 256                        /* (the timeline holds no more) */
static struct {
    uint32_t pa, len;
    uint64_t v;
} g_wbn[WB_NOTES];
static uint32_t g_wbn_next;

static void wb_landed(uint32_t token, bool landed)
{
    if (landed) {
        hle_mem_host_wrote(g_wbn[token % WB_NOTES].pa, g_wbn[token % WB_NOTES].len);
    }
}

static void wb_note(uint32_t pa, uint32_t len, uint64_t v)
{
    const uint32_t k = g_wbn_next++ % WB_NOTES;
    g_wbn[k].pa = pa;
    g_wbn[k].len = len;
    g_wbn[k].v = v;
    hle_tl_land_note(v, wb_landed, k);
}

/* A small linear colour target the frame moves off goes back into guest
 * memory with the next submission (the NV2A renders into memory, and a
 * title may read a target's texels without a lock once its fence passes):
 * Forza averages its scene into an 80 x 30 L8 target and reads it on the
 * CPU for its exposure; reading what was there, it drew its races far too
 * bright. A CPU access to the memory waits for the copy (the barrier).
 *
 * A target the CPU wrote goes back too, whatever its size: a title that
 * uses a target's memory from the CPU may read it as well. Splinter Cell
 * Chaos Theory reads its loading picture's TGA pixels into a frame buffer
 * and, leaving the loading screen, copies its front buffer into the texture
 * it fades the menu in from; the memory still held the TGA's bytes, and four
 * small upside-down copies of the picture faded in across the top of the
 * screen. HAKUX_HLE_PB_CPUWB=0: small targets only (A/B). */
static void writeback(uint32_t id)
{
    static int cpu_wb = -1;
    if (cpu_wb < 0) {
        const char *e = getenv("HAKUX_HLE_PB_CPUWB");
        cpu_wb = !(e && !strcmp(e, "0"));
    }
    const HleSurfDesc *d = id ? object_desc(id) : NULL;
    const HleFmtInfo *fi = d ? hle_fmt_info(d->xfmt) : NULL;
    const PbObj *o = d ? obj_of(id) : NULL;
    if (!fi || !d->pitch || (fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT)) ||
        ((uint64_t)d->pitch * d->h > WRITEBACK_MAX && !(cpu_wb && o && o->cpu_used))) {
        return;
    }
    const uint32_t len = d->pitch * d->h;
    const uint32_t fb = hle_res_rb_fb(len);
    if (!fb) {
        coh_pend_wait_inplace(d->pa, len);
    }
    HleCmdReadback *rb = begin(HLE_OP_READBACK, sizeof(*rb));
    memset(rb, 0, sizeof(*rb));
    rb->value = 0;
    rb->surf = id;
    rb->pa = d->pa;
    rb->pitch = d->pitch;
    rb->arena_off = HLE_RB_DIRECT;
    hle_res_rb_land(rb, fb, len, hle_fe_last_value() + 1);     /* the next submission */
    hle_barrier_arm_landing(d->pa, len, hle_fe_last_value() + 1);
    hle_fe_commit();
    wb_note(d->pa, len, hle_fe_last_value() + 1);
    g_pd.writebacks++;
}

/* A D24S8 depth target bound over the memory of a 32-bit colour target of
 * its size: the colour pass's words are the depth buffer's to the NV2A.
 * Mercenaries copies its scene depth out as colour into a target and then
 * depth-tests its sky dome against that memory as a depth buffer (and
 * fades the dome by it through a texture); with the two images apart the
 * dome failed against an empty depth buffer, the sky stayed black and
 * geometry smeared over it. The back end converts the colour image into
 * the depth image (HLE_OP_ZWRITE) when the colour one was written later,
 * as D3D mode's depth views do. */
static void zwrite_from_colour(uint32_t ds)
{
    const HleSurfDesc *d = object_desc(ds);
    if (!d || !d->pitch || (d->xfmt != HLE_XFMT_LIN_D24S8 && d->xfmt != HLE_XFMT_D24S8)) {
        return;
    }
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *o = &g_pd.objs[i];
        const HleFmtInfo *oi = o->d.pa == d->pa && o->id != ds && !o->cube ? hle_fmt_info(o->d.xfmt) : NULL;
        if (oi && !(oi->flags & HLE_FI_DEPTH) && oi->bpp == 32 && o->d.w == d->w && o->d.h == d->h) {
            HleCmdZWrite *z = begin(HLE_OP_ZWRITE, sizeof(*z));
            z->dst = ds;
            z->src = o->id;
            hle_fe_commit();
            return;
        }
    }
}

/* The window clip (Dead or Alive 3's SetScissors: its character select
 * draws the fighter clipped to the portrait's frame): 8 rectangles, corners
 * in logical pixels, both included; inclusive (draw inside them) or
 * exclusive (outside). Unused ones: D3D sets the first over the whole
 * surface (w x h) when it clips nothing, and leaves the rest at 0, one pixel
 * at the origin. The used ones' box, [x1, y1, x2, y2), clips an inclusive
 * set (their union when they're apart); an exclusive one isn't done (a
 * scissor can't). False: no clip. */
static bool window_clip(uint32_t w, uint32_t h, int32_t r[4])
{
    int32_t b[4] = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t x = R(NV097_SET_WINDOW_CLIP_HORIZONTAL + 4 * i), y = R(NV097_SET_WINDOW_CLIP_VERTICAL + 4 * i);
        const int32_t x1 = x & 0xFFF, x2 = ((x >> 16) & 0xFFF) + 1, y1 = y & 0xFFF, y2 = ((y >> 16) & 0xFFF) + 1;
        if ((!x && !y) || (x1 == 0 && y1 == 0 && x2 >= (int32_t)w && y2 >= (int32_t)h)) {
            continue;
        }
        b[0] = MIN(b[0], x1);
        b[1] = MIN(b[1], y1);
        b[2] = MAX(b[2], x2);
        b[3] = MAX(b[3], y2);
    }
    if (b[0] == INT32_MAX || (R(NV097_SET_WINDOW_CLIP_TYPE) & 1)) {
        return false;
    }
    memcpy(r, b, sizeof(b));
    return true;
}

/* A video mode set since the display last followed one: the display is its
 * size. CreateDevice binds its back buffer before it sets the mode, and the
 * back buffer needn't be the frame shown: Hunter: The Reckoning - Redeemer
 * supersamples, a 640 x 960 back buffer D3D's Present filters into the
 * 640 x 480 frame it shows. Sized by the back buffer, the display (and every
 * surface as wide as it, the frame's own) was 640 x 960, the frames in its
 * top half. Before the targets that follow are sized (linear_size). */
static void mode_sync(void)
{
    uint32_t w, h, seq;
    const bool ok = mode_size(g_pd.disp_h, &w, &h, &seq);
    if (seq == g_mode_seen || !g_pd.device) {
        return;
    }
    g_mode_seen = seq;
    g_disp_moded = ok;
    if (!ok || (w == g_pd.disp_w && h == g_pd.disp_h)) {
        return;
    }
    HleCmdDevice *c = begin(HLE_OP_DEVICE_RESET, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->disp_w = w;
    c->disp_h = h;
    hle_fe_commit();
    hle_log("hakuX-hle-pb", "display %ux%u: the video mode's (was %ux%u, the first target bound)", w, h,
            g_pd.disp_w, g_pd.disp_h);
    g_pd.disp_w = w;
    g_pd.disp_h = h;
}

static void coh_drawn(PbObj *o);
static void coh_first_write(PbObj *o, bool full);
static bool coh_managed(const PbObj *o, uint32_t *len);
static void coh_land_over(const PbObj *o, uint32_t len);

/* A colour target that covers the display (its video mode known): one that
 * can be D3D's back buffer, a frame the display shows (swap). Linear, as
 * back buffers are: a swizzled reflection or glow target as large isn't. */
static bool covers_display(const PbObj *o)
{
    return o && o->d.pitch && g_disp_moded && g_pd.disp_w && o->d.w >= g_pd.disp_w &&
           o->d.h >= g_pd.disp_h;
}

/* The targets the surface methods describe, bound when they changed. */
static void sync_rt(void)
{
    mode_sync();
    if (!g_pd.rt_dirty) {
        return;
    }
    g_pd.rt_dirty = false;
    uint32_t fmt = R(NV097_SET_SURFACE_FORMAT), cf = fmt & 0xF, zf = (fmt >> 4) & 0xF;
    bool swz = ((fmt >> 8) & 0xF) == 2, zfloat = R(NV097_SET_CONTROL0) & 0x1000u;
    uint32_t cw = R(NV097_SET_SURFACE_CLIP_HORIZONTAL) >> 16, cx = R(NV097_SET_SURFACE_CLIP_HORIZONTAL) & 0xFFFF;
    uint32_t ch = R(NV097_SET_SURFACE_CLIP_VERTICAL) >> 16, cy = R(NV097_SET_SURFACE_CLIP_VERTICAL) & 0xFFFF;
    uint32_t w = cx + cw, h = cy + ch;
    /* antialiasing (SET_SURFACE_FORMAT 15:12: 1 center-corner 2x horizontal, 2
     * square-offset 2x2): the clip is in logical pixels, the surface (pitch,
     * image) twice as wide / high; positions scale by it on the NV2A */
    const uint32_t aa = (fmt >> 12) & 0xF, sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;
    if (swz) {
        w = 1u << ((fmt >> 16) & 0xF);
        h = 1u << ((fmt >> 24) & 0xF);
    }
    if (!w || !h || w > 4096 || h > 4096) {
        return;
    }
    device(w, h);
    if (!swz) {
        w *= sx;
        h *= sy;
    }
    g_pd.aa_x = swz ? 1 : sx;
    g_pd.aa_y = swz ? 1 : sy;
    uint32_t cpitch = R(NV097_SET_SURFACE_PITCH) & 0xFFFF, zpitch = R(NV097_SET_SURFACE_PITCH) >> 16;
    uint32_t cpa = R(NV097_SET_SURFACE_COLOR_OFFSET) & 0x0FFFFFFFu;
    uint32_t zpa = R(NV097_SET_SURFACE_ZETA_OFFSET) & 0x0FFFFFFFu;
    uint32_t cx_fmt = hle_kelvin_color_xfmt(cf, swz);
    uint32_t zx_fmt = (zf == 1 || zf == 2) ? hle_kelvin_depth_xfmt(zf == 2, zfloat, swz) : 0;
    /* no surface where D3D left none: offset 0, or (linear) a pitch short of
     * the width (its "no depth buffer" is offset 0, pitch 64: Riddick's
     * image of that lost the device) */
    const uint32_t cbpp = cf == 9 ? 1 : cf <= 3 || cf == 10 ? 2 : 4, zbpp = zf == 1 ? 2 : 4;
    if (!cpa || (!swz && cpitch < w * cbpp)) {
        cx_fmt = 0;
    }
    if (!zpa || (!swz && zpitch < w * zbpp)) {
        zx_fmt = 0;
    }
    if (!swz) {                         /* the surfaces' size, not the clip's */
        if (cx_fmt) {
            linear_size(cpitch, cx_fmt, sx, sy, &w, &h);
        } else if (zx_fmt) {
            linear_size(zpitch, zx_fmt, sx, sy, &w, &h);
        }
    }
    uint32_t rt = cx_fmt ? object_view(cpa, cx_fmt, swz ? 0 : hle_kelvin_color_xfmt(cf, true),
                                       swz ? 0 : cpitch, w, h) : 0;
    uint32_t ds = zx_fmt ? object_view(zpa, zx_fmt,
                                       swz ? 0 : hle_kelvin_depth_xfmt(zf == 2, zfloat, true),
                                       swz ? 0 : zpitch, w, h) : 0;
    g_pd.binds++;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        if (g_pd.objs[i].id == rt || g_pd.objs[i].id == ds) {
            g_pd.objs[i].bound = g_pd.binds;
        }
    }
    if (ds && ds != g_pd.ds) {
        zwrite_from_colour(ds);
    }
    if (rt != g_pd.rt || ds != g_pd.ds) {
        if (rt != g_pd.rt) {
            writeback(g_pd.rt);
        }
        g_pd.rt = rt;
        g_pd.rt_obj = obj_of(rt);
        g_pd.ds_obj = obj_of(ds);
        g_pd.ds = ds;
        /* the objects' size: a view of a part of one is the whole (object()) */
        const PbObj *so = g_pd.rt_obj ? g_pd.rt_obj : g_pd.ds_obj;
        g_pd.rt_w = so ? so->d.w : w;
        g_pd.rt_h = so ? so->d.h : h;
        HleCmdSetRt *s = begin(HLE_OP_SET_RT, sizeof(*s));
        s->rt = rt;
        s->ds = ds;
        hle_fe_commit();
        rt_cpu_sync(rt);
        g_pd.rt_fresh = true;
        g_pd.vp_key[0] = g_pd.vp_key[1] = ~0u;
    }
    if (rt && covers_display(g_pd.rt_obj)) {
        g_pd.frame_rt = rt;
        g_pd.frame_aa_x = g_pd.aa_x;
        g_pd.frame_aa_y = g_pd.aa_y;
    }
    /* the surface clip: the draws' viewport rect and scissor; the window
     * clip narrows the scissor */
    uint32_t wc[17];
    wc[0] = R(NV097_SET_WINDOW_CLIP_TYPE);
    for (unsigned i = 0; i < 16; i++) {
        wc[1 + i] = R(NV097_SET_WINDOW_CLIP_HORIZONTAL + 4 * i);
    }
    if (g_pd.vp_key[0] != R(NV097_SET_SURFACE_CLIP_HORIZONTAL) ||
        g_pd.vp_key[1] != R(NV097_SET_SURFACE_CLIP_VERTICAL) || memcmp(g_pd.wc_key, wc, sizeof(wc))) {
        g_pd.vp_key[0] = R(NV097_SET_SURFACE_CLIP_HORIZONTAL);
        g_pd.vp_key[1] = R(NV097_SET_SURFACE_CLIP_VERTICAL);
        memcpy(g_pd.wc_key, wc, sizeof(wc));
        HleCmdViewport *v = begin(HLE_OP_VIEWPORT, sizeof(*v));
        v->x = swz ? 0 : cx * sx;           /* target (physical) pixels */
        v->y = swz ? 0 : cy * sy;
        v->w = swz ? w : cw * sx;
        v->h = swz ? h : ch * sy;
        v->minz = 0.0f;
        v->maxz = 1.0f;
        int32_t r[4];
        if (window_clip(swz ? w : cx + cw, swz ? h : cy + ch, r)) {
            v->clip = 1;
            v->cx1 = MAX(r[0] * (int32_t)g_pd.aa_x, (int32_t)v->x);
            v->cy1 = MAX(r[1] * (int32_t)g_pd.aa_y, (int32_t)v->y);
            v->cx2 = MIN(r[2] * (int32_t)g_pd.aa_x, (int32_t)(v->x + v->w));
            v->cy2 = MIN(r[3] * (int32_t)g_pd.aa_y, (int32_t)(v->y + v->h));
        }
        hle_fe_commit();
    }
}

/* Before the first clear or draw into the target since its bind, a swap or
 * the end of a pass (the decoder caught up with PUT; what the guest does
 * before its next kick comes before the commands it kicks): what the CPU
 * wrote there goes in first, so that the GPU's writes land over it, as the
 * NV2A's do in memory. Checked at the swap and the display start only, the
 * writes went over the draws before them: Splinter Cell Chaos Theory reads
 * its logo's 8-bit TGA pixels into the back buffer between two kicks of a
 * frame (the second one draws over them, and its flip is a trap whose
 * handler starts the display there), and the bytes covered the top quarter
 * of the frame as four small upside-down copies. Once a pass that draws,
 * not a draw (rt_cpu_sync hashes the target's dirty edge pages). A target
 * whose memory is newer than its image another way (a copy into it, a twin's
 * landing, a device: coh_mem_newer) gets that memory too; full: the write is
 * a clear of all of it, which needs none of it. */
static void rt_first_write(bool full)
{
    if (g_pd.rt_fresh) {
        g_pd.rt_fresh = false;
        PbObj *o = obj_of(g_pd.rt);
        if (o) {
            coh_first_write(o, full);
        }
    }
}

/* Before the first write ever into the bound target, a draw or a clear of
 * part of it (its memory about to be uploaded into its image: never drawn,
 * the memory is all there is): what newer the other images over that
 * memory hold lands there first (coh_land_over). At the draw's start and the
 * clear's, not in rt_first_write: a draw's comes after its arena blocks,
 * and a landing kicks and waits; with the GPU idle the arena starts again
 * at 0, over them. Once an object: never drawn into, its memory newer. */
static void rt_first_land(bool full)
{
    PbObj *o = g_pd.rt_fresh && !full ? obj_of(g_pd.rt) : NULL;
    uint32_t len;
    if (o && !o->written && coh_managed(o, &len)) {
        coh_land_over(o, len);
    }
}

/* A float depth buffer (CONTROL0 bit 12) holds the depth itself: D3D's range
 * is 0 .. 1e30 for F24S8 and 0 .. 511.9375 (the format's maximum) for F16,
 * its far plane what it sets as CLIP_MAX. The image (D32_SFLOAT) holds it
 * over that maximum, as the shadow-buffer compares read it (psh.c k_max):
 * the largest scale whose product with the maximum stays within 1.0, so a
 * vertex on the far plane does not clip. Unscaled, Touge R's scene (F24S8,
 * the far plane at 1e30) clipped or clamped to 1.0 against its clear at 0.0
 * and failed its LESSEQUAL test: the screen black under the HUD. */
static float zfloat_scale(bool z24)
{
    static float s[2];
    if (!s[z24]) {
        const float m = z24 ? 1.0e30f : 511.9375f;
        float v = 1.0f / m;
        while (m * v > 1.0f) {
            v = nextafterf(v, 0.0f);
        }
        s[z24] = v;
    }
    return s[z24];
}

/* A float depth buffer's clear value (F24: 8-bit exponent, 16-bit mantissa;
 * F16: 4-bit exponent biased 7, 12-bit mantissa) as the top of the depths
 * it stands for: the NV2A tests at the format's precision, and a far-plane
 * vertex lands on D3D's far clear, short of 1e30 by the mantissa it drops. */
static float zfloat_clear(uint32_t v, bool z24)
{
    if (!v) {
        return 0.0f;
    }
    uint32_t b = z24 ? (v << 7) | 0x7Fu : ((v << 11) + 0x3C000000u) | 0x7FFu;
    float f;
    memcpy(&f, &b, 4);
    return isfinite(f) ? MIN(f * zfloat_scale(z24), 1.0f) : 1.0f;
}

static void clear(uint32_t flags)
{
    sync_rt();
    uint32_t hr = R(NV097_SET_CLEAR_RECT_HORIZONTAL), vr = R(NV097_SET_CLEAR_RECT_VERTICAL);
    /* the rect in logical pixels, the surface's scaled by its antialiasing */
    const int32_t ax = (int32_t)MAX(g_pd.aa_x, 1u), ay = (int32_t)MAX(g_pd.aa_y, 1u);
    HleRect r = { (int32_t)(hr & 0xFFFF) * ax, (int32_t)(vr & 0xFFFF) * ay,
                  (int32_t)((hr >> 16) + 1) * ax, (int32_t)((vr >> 16) + 1) * ay };
    const bool full = (flags & 0xF0) == 0xF0 && R(0x0358) == 0x01010101u && r.x1 <= 0 && r.y1 <= 0 &&
                      (uint32_t)r.x2 >= g_pd.rt_w && (uint32_t)r.y2 >= g_pd.rt_h;
    rt_first_land(full);
    rt_first_write(full);
    if (!g_pd.rt && !g_pd.ds) {
        return;
    }
    uint32_t zs = R(NV097_SET_ZSTENCIL_CLEAR_VALUE), zf = (R(NV097_SET_SURFACE_FORMAT) >> 4) & 0xF;
    bool zfloat = R(NV097_SET_CONTROL0) & 0x1000u;
    HleCmdClear *c = begin(HLE_OP_CLEAR, sizeof(*c));
    c->flags = flags & 0xF3;
    c->color = R(NV097_SET_COLOR_CLEAR_VALUE);
    if ((R(NV097_SET_SURFACE_FORMAT) & 0xF) == 3) {     /* R5G6B5: the value is in its format */
        uint32_t v = c->color;
        c->color = 0xFF000000u | ((v >> 11) & 0x1F) * 255 / 31 << 16 |
                   ((v >> 5) & 0x3F) * 255 / 63 << 8 | (v & 0x1F) * 255 / 31;
    }
    if (zf == 2) {
        c->z = zfloat ? zfloat_clear(zs >> 8, true) : (zs >> 8) / 16777215.0f;
        c->stencil = zs & 0xFF;
    } else {
        c->z = zfloat ? zfloat_clear(zs & 0xFFFF, false) : (zs & 0xFFFF) / 65535.0f;
    }
    if (r.x1 > 0 || r.y1 > 0 || (uint32_t)r.x2 < g_pd.rt_w || (uint32_t)r.y2 < g_pd.rt_h) {
        c->nrects = 1;
        c->rect[0] = r;
    }
    if (g_dlog_left > 0) {
        hle_log("hakuX-hle-pb", "clear %02x rt %u ds %u colour %08x z %g s %u rect %d,%d-%d,%d of %ux%u",
                c->flags, g_pd.rt, g_pd.ds, c->color, c->z, c->stencil, r.x1, r.y1, r.x2, r.y2,
                g_pd.rt_w, g_pd.rt_h);
    }
    hle_fe_commit();
    if (g_pd.rt_obj && (c->flags & HLE_CLEAR_TARGET)) {
        g_pd.rt_obj->written = ++g_pd.wseq;
        coh_drawn(g_pd.rt_obj);
    }
    if (c->flags & (HLE_CLEAR_ZBUFFER | HLE_CLEAR_STENCIL)) {
        ds_written();
    }
    g_pd.clears++;
}

static void front_fill(uint32_t pa, uint32_t len);

/* After a savestate's load the device's display is the first target bound
 * (device()), which a frame resumed midway names: Burnout 3's was a 64 x 32
 * one, and every frame went to a 64 x 32 display stretched to the screen. A
 * boot binds the back buffer first (CreateDevice). The first frame swapped
 * after the load is the display's: its logical size (the antialiasing scale
 * off) resizes the display, once. Only for a state without its display
 * (v1), and not a display the video mode sized: the target bound at a swap
 * needn't be the frame (swap), and Double-S.T.E.A.L 2's, 32 x 16, made a
 * 32 x 16 display after a load, every frame black again. */
static void restored_display(void)
{
    g_restored_display = false;
    if (g_disp_moded) {
        return;
    }
    const PbObj *o = g_pd.rt_obj;
    const uint32_t w = o ? o->d.w / MAX(g_pd.aa_x, 1u) : 0, h = o ? o->d.h / MAX(g_pd.aa_y, 1u) : 0;
    if (!g_pd.device || !w || !h || (w == g_pd.disp_w && h == g_pd.disp_h)) {
        return;
    }
    HleCmdDevice *c = begin(HLE_OP_DEVICE_RESET, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->disp_w = w;
    c->disp_h = h;
    hle_fe_commit();
    hle_log("hakuX-hle-pb", "savestate: display %ux%u from the first frame swapped (was %ux%u, the first "
            "target bound)", w, h, g_pd.disp_w, g_pd.disp_h);
    g_pd.disp_w = w;
    g_pd.disp_h = h;
}

/* After a swap the frame on screen is the GPU's image, and its memory is
 * older. The NV2A draws into memory and scans it out: a title that draws
 * into its front buffer on the CPU (Midtown Madness 3's loading prompt,
 * GetBackBuffer(-1) and LockRect, in a corner of the faded menu) writes over
 * the frame there, and one may read it (Forza copies the background behind
 * its loading spinner out of it, after BlockUntilIdle, and blends the turbo
 * over the copy: from memory the GPU never wrote, a black square). The
 * memory has no guest access meanwhile: the first read or write has the
 * frame copied into it first (front_fill). Titles that leave their frame
 * buffers alone pay nothing once those are protected (mem/barrier.c keeps
 * them so until they are touched). */
static void front_stale(const PbObj *o)
{
    const HleFmtInfo *fi = o && o->d.pitch && !o->cube ? hle_fmt_info(o->d.xfmt) : NULL;
    const bool colour = fi && !(fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT));
    hle_barrier_set_stale(colour ? o->d.pa : 0, colour ? o->d.pitch * o->d.h : 0, front_fill);
}

static void shown_part(const PbObj *o, uint32_t ax, uint32_t ay, uint32_t *w, uint32_t *h);

/* The frame pace (pbmode.c's decode loop stalls on it): without it the
 * guest's push ring holds however far the renderer is behind (Sega GT 2002
 * on a Mali-G52: ~25 frames, the flip reaching the display 1030-1130 ms
 * later) and the pad is read by a simulation the screen shows a second
 * later. After a swap the decoder stops, as at a FLIP_STALL, until the GPU
 * has finished the swap `depth` before it; the ring fills, and the guest,
 * stalling on ring space in its own frame loop, throttles itself to the
 * render pace. Two back by default: one frame queued behind the one the
 * GPU runs, so it never runs dry at the swap. The wait is a timeline note
 * that kicks the decoder, which meanwhile serves the guest's stores and
 * render-target reads (it is stalled, not blocked). The app's "Frame
 * pacing" setting, on by default; debug.hakux.pace (0 off, 1-3 swaps back)
 * overrides it, read at each swap so an A/B needs no restart. */
static struct {
    uint64_t swap[4];                   /* the last swaps' values, by g_pd.swaps */
    uint64_t want;                      /* the value the decoder stalls for */
    uint32_t seq, reached;              /* stalls, and the last the GPU reached (atomic) */
} g_pace;

static void pace_landed(uint32_t seq, bool landed)
{
    __atomic_store_n(&g_pace.reached, seq, __ATOMIC_RELEASE);
    hle_pb_kick();
}

bool hle_pbd_pace_reached(void)
{
    return __atomic_load_n(&g_pace.reached, __ATOMIC_ACQUIRE) == g_pace.seq ||
           hle_tl_completed() >= g_pace.want;
}

bool hle_pbd_pace(void)
{
    char v[PROP_VALUE_MAX];
    hle_prop_get("debug.hakux.pace", v);
    int depth = v[0] ? atoi(v) : hle_host_frame_pace() ? 2 : 0;
    depth = MIN(depth, 3);
    if (depth <= 0 || g_pd.swaps <= (uint64_t)depth) {
        return false;
    }
    g_pace.want = g_pace.swap[(g_pd.swaps - depth) % ARRAY_SIZE(g_pace.swap)];
    if (hle_tl_completed() >= g_pace.want) {
        return false;
    }
    /* no room for the note: no kick would come, so no stall */
    if (!hle_tl_land_note(g_pace.want, pace_landed, ++g_pace.seq)) {
        return false;
    }
    return !hle_pbd_pace_reached();
}

/* FLIP_INCREMENT_WRITE: the frame in the colour target is done; it goes to
 * the display (D3D's vblank handler shows it there too). The display scans
 * out D3D's back buffer, whatever is bound: a target smaller than the
 * display both ways can't be the frame, and the last one bound that covers
 * it, drawn since the swap before, is shown instead. Double-S.T.E.A.L 2
 * binds a 32 x 16 target after its frame, before its Present, and that went
 * on screen, black, for every frame. */
static void swap(void)
{
    sync_rt();
    if (!g_pd.rt) {
        return;
    }
    if (g_restored_display) {
        restored_display();
    }
    uint32_t id = g_pd.rt, ax = g_pd.aa_x, ay = g_pd.aa_y;
    PbObj *fo = g_pd.rt_obj;
    if (fo && g_disp_moded && g_pd.disp_w && g_pd.disp_h && fo->d.w < g_pd.disp_w &&
        fo->d.h < g_pd.disp_h) {
        PbObj *f = obj_of(g_pd.frame_rt);
        if (f && f->written > g_pd.swap_wseq) {
            id = g_pd.frame_rt;
            fo = f;
            ax = g_pd.frame_aa_x;
            ay = g_pd.frame_aa_y;
        }
    }
    coh_shown(fo);                          /* the front buffer's path keeps its memory */
    /* memory the CPU wrote is the image now: nothing older to fill */
    front_stale(rt_cpu_sync(id) ? NULL : fo);
    g_pd.rt_fresh = true;
    if (rtlog_on()) {
        hle_log("hakuX-hle-pb", "swap %llu: shows %u, %u bound (clears %llu draws %llu)",
                (unsigned long long)g_pd.swaps + 1, id, g_pd.rt, (unsigned long long)g_pd.clears,
                (unsigned long long)g_pd.draws);
    }
    g_pd.shown_id = id;
    g_pd.shown_ax = ax;
    g_pd.shown_ay = ay;
    __atomic_store_n(&g_pd.shown_ns, hle_now_ns(), __ATOMIC_RELAXED);
    g_pd.shown_written = fo ? fo->written : 0;
    HleCmdSwap *c = begin(HLE_OP_SWAP, sizeof(*c));
    const uint64_t value = c->value = hle_fe_next_value();
    c->swap_id = ++g_pd.swap_ids;
    shown_part(fo, ax, ay, &c->src_w, &c->src_h);
    g_pd.swaps++;
    g_pace.swap[g_pd.swaps % ARRAY_SIZE(g_pace.swap)] = value;      /* (hle_pbd_pace) */
    g_pd.epoch++;
    g_pd.kick_ns = hle_now_ns();
    g_pd.vis_unsent = false;                /* a swap submits */
    c->surf = id;
    c->dump_off = HLE_SWAP_NO_DUMP;
    hle_hitch_pb_swap(c->swap_id);          /* before the back end stamps its stages */
    hle_fe_commit();
    const uint32_t pa = fo ? fo->d.pa : 0;
    const uint32_t flipped = __atomic_exchange_n(&g_pd.flipped_pa, 0, __ATOMIC_ACQ_REL);
    uint32_t head = g_pd.pend_head;
    if (head - __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE) < ARRAY_SIZE(g_pd.pend)) {
        g_pd.pend[head % ARRAY_SIZE(g_pd.pend)] = c->swap_id;
        g_pd.pend_flipped[head % ARRAY_SIZE(g_pd.pend)] = pa && flipped == pa;
        __atomic_store_n(&g_pd.pend_head, head + 1, __ATOMIC_RELEASE);
    }
    g_pd.swap_wseq = g_pd.wseq;
    __atomic_store_n(&g_pd.ahead_pa, 0, __ATOMIC_RELEASE);
}

void hle_pbd_trap(void)
{
    const PbObj *o = g_pd.on ? g_pd.rt_obj : NULL;
    __atomic_store_n(&g_pd.ahead_pa, o && o->written > g_pd.swap_wseq ? o->d.pa : 0, __ATOMIC_RELEASE);
}

static uint32_t object_at(uint32_t pa);

/* A frame presented: the frame rate in the log every 5 s (perf A/B from
 * logcat: hakuX-hle-fps). */
static void fps_log(void)
{
    static int64_t s_t0;
    static unsigned s_n, s_n0;
    const int64_t now = g_get_monotonic_time();
    s_n++;
    if (!s_t0) {
        s_t0 = now;
        s_n0 = s_n;
    } else if (now - s_t0 >= 5000000) {
        hle_log("hakuX-hle-fps", "%.1f fps (%u frames in %.2f s)",
                (s_n - s_n0) * 1e6 / (double)(now - s_t0), s_n - s_n0, (now - s_t0) / 1e6);
        s_t0 = now;
        s_n0 = s_n;
    }
}

/* The part of object o the display shows: as many of its pixels from its top
 * left as the display has (ax x ay samples each: an antialiased target's),
 * as the NV2A's scanout reads them; it scales nothing. A frame buffer made
 * taller than the frame (a surface as wide as the display is as tall as it,
 * and Hunter: The Reckoning - Redeemer's display was its 640 x 960 back
 * buffer until it set its video mode) shows its frame, not all of itself
 * squeezed into the display. Whole when it is no bigger, or while the
 * display's size is a guess (no video mode known: the first target bound). */
static void shown_part(const PbObj *o, uint32_t ax, uint32_t ay, uint32_t *w, uint32_t *h)
{
    const bool crop = o && g_disp_moded && g_pd.disp_w && g_pd.disp_h;
    *w = crop ? MIN(g_pd.disp_w * MAX(ax, 1u), o->d.w) : 0;
    *h = crop ? MIN(g_pd.disp_h * MAX(ay, 1u), o->d.h) : 0;
}

/* id goes on screen as it is now, with no flip: the display scans out the
 * memory it points at */
static void present_now(uint32_t id)
{
    const PbObj *o = obj_of(id);
    /* the frame shown again keeps its antialiasing (a swap's frame needn't be
     * the target bound) */
    const uint32_t ax = id == g_pd.shown_id ? g_pd.shown_ax : id == g_pd.rt ? g_pd.aa_x : 1;
    const uint32_t ay = id == g_pd.shown_id ? g_pd.shown_ay : id == g_pd.rt ? g_pd.aa_y : 1;
    g_pd.shown_id = id;
    g_pd.shown_ax = ax;
    g_pd.shown_ay = ay;
    __atomic_store_n(&g_pd.shown_ns, hle_now_ns(), __ATOMIC_RELAXED);
    g_pd.shown_written = o ? o->written : 0;
    HleCmdSwap *c = begin(HLE_OP_SWAP, sizeof(*c));
    c->value = hle_fe_next_value();
    c->swap_id = ++g_pd.swap_ids;
    c->surf = id;
    c->dump_off = HLE_SWAP_NO_DUMP;
    shown_part(o, ax, ay, &c->src_w, &c->src_h);
    hle_hitch_pb_swap(c->swap_id);
    hle_hitch_frame(c->swap_id, HLE_FR_FLIP);   /* shown as it is set */
    hle_fe_commit();
    g_pd.kick_ns = hle_now_ns();
    g_pd.vis_unsent = false;
    hle_disp_present(c->swap_id);           /* applies once the back end writes it */
    fps_log();
    g_pd.epoch++;
}

bool hle_pbd_flip_due(void)
{
    if (!g_pd.on || __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE) ==
                    __atomic_load_n(&g_pd.pend_head, __ATOMIC_ACQUIRE)) {
        return false;
    }
    const int64_t t = __atomic_load_n(&g_pd.flipped_ns, __ATOMIC_RELAXED);
    return t && hle_now_ns() - t < 1000000000;
}

void hle_pbd_display(uint32_t pa, bool flip)
{
    if (!g_pd.on) {
        return;
    }
    if (flip || g_pd.swaps != g_pd.swaps_seen) {
        /* D3D's flip: its vblank handler presents. A swap waited for it when
         * the start was written (flip), or one came since the last start.
         * The decoder a frame ahead (the next swap in before this start)
         * made the second test miss the start of the frame after: that
         * presented the render target again, half drawn, as a frame of its
         * own (PGR2's menus every few seconds: a front the UI never shows). */
        g_pd.swaps_seen = g_pd.swaps;
        return;
    }
    uint32_t id = object_at(pa & 0x0FFFFFFFu);
    if (!id) {
        static unsigned s_logged;
        if (s_logged++ < 4) {
            hle_log("hakuX-hle-pb", "display start %08x: no render target there (not shown)", pa);
        }
        return;
    }
    /* the object shown, nothing new in it: the display already shows that
     * memory (a write of PCRTC_START's value again changes nothing). D3D
     * 3925's vblank handler writes it after a flip, and PGR1 once more:
     * each frame was presented twice. */
    PbObj *o = obj_of(id);
    coh_shown(o);
    if (!rt_cpu_sync(id) && id == g_pd.shown_id && o && o->written == g_pd.shown_written) {
        g_pd.display_repeats++;
        return;
    }
    if (rtlog_on()) {
        hle_log("hakuX-hle-pb", "display %08x: shows %u after swap %llu (was %u)", pa, id,
                (unsigned long long)g_pd.swaps, g_pd.shown_id);
    }
    present_now(id);
    g_pd.pcrtc_presents++;
}

/* A swap whose flip came before it (hle_pb_flipped) and no flip since, the
 * title idle: the frame the console shows, presented (the newest such; the
 * ones before it go). Heroes draws its tutorial prompts once over the scene
 * and waits on a button: no flip came to present them. */
static bool present_flipped(void)
{
    const size_t n = ARRAY_SIZE(g_pd.pend);
    uint32_t tail = __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE), take, id;
    do {
        const uint32_t head = __atomic_load_n(&g_pd.pend_head, __ATOMIC_ACQUIRE);
        take = head;
        for (uint32_t i = tail; i != head; i++) {
            if (g_pd.pend_flipped[i % n]) {
                take = i;
            }
        }
        if (take == head) {
            return false;
        }
        id = g_pd.pend[take % n];
    } while (!__atomic_compare_exchange_n(&g_pd.pend_tail, &tail, take + 1, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    __atomic_store_n(&g_pd.shown_ns, hle_now_ns(), __ATOMIC_RELAXED);
    hle_hitch_frame(id, HLE_FR_FLIP);
    hle_disp_present(id);
    fps_log();
    if (g_pd.flipped_presents++ < 4) {
        hle_log("hakuX-hle-pb", "scanout: swap %u, flipped before it came, had no flip since: shown "
                "(first 4 logged)", id);
    }
    return true;
}

bool hle_pbd_scan_due(void)
{
    const int64_t t = __atomic_load_n(&g_pd.shown_ns, __ATOMIC_RELAXED);
    return g_pd.on && t && hle_now_ns() - t > SCAN_IDLE_NS;
}

void hle_pbd_scanout(void)
{
    if (rtlog_on() && g_pd.on) {
        static int64_t s_logged_for;        /* once an idle period */
        const PbObj *o = obj_of(g_pd.shown_id);
        if (s_logged_for != g_pd.shown_ns) {
            s_logged_for = g_pd.shown_ns;
            hle_log("hakuX-hle-pb", "scanout check: shown %u (%08x %ux%u) idle %lld ms, flips pending %u",
                    g_pd.shown_id, o ? o->d.pa : 0, o ? o->d.w : 0, o ? o->d.h : 0,
                    (long long)((hle_now_ns() - g_pd.shown_ns) / 1000000),
                    __atomic_load_n(&g_pd.pend_head, __ATOMIC_ACQUIRE) -
                        __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE));
        }
    }
    if (!g_pd.on || !g_pd.shown_id || !hle_pbd_scan_due()) {
        return;
    }
    if (present_flipped()) {
        return;
    }
    /* What the CPU wrote there (not a target the CPU uses from now on: its
     * unbinds aren't written back), or what the GPU drew there since it was
     * shown: front-buffer rendering, Conker's "Loading..." screen while its
     * level loads, drawn into the frame on screen with no flip). */
    const bool cpu = rt_cpu_upload(g_pd.shown_id, false);
    const PbObj *so = obj_of(g_pd.shown_id);
    const bool gpu = !cpu && so && so->written != g_pd.shown_written;
    if (!cpu && !gpu) {
        return;
    }
    /* swaps still waiting for their flip this long get none: D3D shows the
     * frame of a display mode change itself (AvSetDisplayMode with it, no
     * vblank flip), as Midtown Madness 3's last one before its loading
     * prompt. This present supersedes them; a late flip of one would bring
     * the older frame back. */
    uint32_t tail = __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE);
    const uint32_t head = __atomic_load_n(&g_pd.pend_head, __ATOMIC_ACQUIRE);
    while (tail != head &&
           !__atomic_compare_exchange_n(&g_pd.pend_tail, &tail, head, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (g_pd.scan_presents++ < 4) {
        hle_log("hakuX-hle-pb", "scanout: the %s wrote the frame on screen (%u) and the title doesn't "
                "flip: shown again (first 4 logged)", cpu ? "CPU" : "GPU", g_pd.shown_id);
    }
    present_now(g_pd.shown_id);
}

void hle_pbd_cpu_display(uint32_t pa)
{
    uint32_t pitch, xfmt, w, h, seq;
    unsigned hz;
    if (!g_pd.on || g_pd.device) {
        return;                             /* D3D's: its own frames, flipped */
    }
    hle_pb_display_mode_get(&pitch, &xfmt, &hz);
    if (!mode_size(0, &w, &h, &seq)) {
        hle_log("hakuX-hle-pb", "frame buffer %08x: no video mode it fits (pitch %u format %02x): not shown", pa,
                pitch, xfmt);
        return;
    }
    device(w, h);
    const uint32_t id = object(pa & 0x0FFFFFFFu, xfmt, pitch, w, h);
    PbObj *o = obj_of(id);
    if (!o) {
        return;
    }
    /* the front buffer's path, as a swap's (coh_shown): kept by the
     * coherence instead, the frame buffer's memory was never newer than its
     * image to the scanout (rt_cpu_upload), and Insignia's Setup Assistant
     * showed its first frame for good */
    coh_shown(o);
    rt_upload(o, id, false);
    present_now(id);
    hle_log("hakuX-hle-pb", "frame buffer %08x %ux%u format %02x: the title draws it with the CPU (no GPU device), "
            "shown as written", pa, w, h, xfmt);
}

void hle_pbd_gamma(const uint8_t rgb[3][256])
{
    if (!g_pd.on) {
        return;
    }
    HleCmdGamma *c = begin(HLE_OP_GAMMA, sizeof(*c));
    c->immediate = 1;
    memcpy(c->rgb, rgb, sizeof(c->rgb));
    hle_fe_commit();
    static unsigned s_logged;
    if (s_logged++ < 4) {
        hle_log("hakuX-hle-pb", "gamma ramp r=%u,%u,%u,%u g=%u,%u b=%u,%u", rgb[0][0], rgb[0][64],
                rgb[0][128], rgb[0][255], rgb[1][128], rgb[1][255], rgb[2][128], rgb[2][255]);
    }
}

void hle_pbd_overlay(bool show)
{
    if (!g_pd.on || !g_pd.device) {
        return;
    }
    const uint32_t size_in = hle_host_gpu_reg(0x008928);
    const bool on = show && size_in != 0xFFFFFFFFu;
    uint32_t w = 0, h = 0, off = HLE_ARENA_FULL;
    int32_t src[4], dst[4];
    if (on) {
        const uint32_t base = hle_host_gpu_reg(0x008900), offset = hle_host_gpu_reg(0x008920);
        const uint32_t fmt = hle_host_gpu_reg(0x008958), pitch = fmt & 0x1FFF;
        const uint32_t pout = hle_host_gpu_reg(0x008948), sout = hle_host_gpu_reg(0x008950);
        const uint32_t ow = sout & 0xFFF, oh = (sout >> 16) & 0xFFF;
        uint32_t iw = size_in & 0x7FF, ih = (size_in >> 16) & 0x7FF;
        iw = MIN(iw, ow ? ow : iw);         /* larger than the output: capped, not scaled */
        ih = MIN(ih, oh ? oh : ih);
        /* a linear YUY2 (CR8YB8CB8YA8) or UYVY surface's header words */
        const uint32_t code = ((fmt >> 16) & 3) == 1 ? HLE_XFMT_YUY2 : HLE_XFMT_UYVY;
        const uint32_t h5[5] = { HLE_COMMON_TYPE_SURFACE | 1, (base + offset) & 0x0FFFFFFFu, 0,
                                 0x00010029u | code << 8,
                                 (MAX(iw, 1u) - 1) | (MAX(ih, 1u) - 1) << HLE_SIZE_HEIGHT_SHIFT |
                                     (MAX(pitch, 64u) / 64 - 1) << HLE_SIZE_PITCH_SHIFT };
        coh_pend_wait((base + offset) & 0x0FFFFFFFu, MAX(pitch, 64u) * ih);   /* a copy queued into it lands first */
        off = iw && ih ? hle_draw_overlay_upload(h5, &w, &h) : HLE_ARENA_FULL;
        src[0] = 0; src[1] = 0; src[2] = (int32_t)w; src[3] = (int32_t)h;
        dst[0] = (int32_t)(pout & 0xFFF);
        dst[1] = (int32_t)((pout >> 16) & 0xFFF);
        dst[2] = dst[0] + (int32_t)ow;
        dst[3] = dst[1] + (int32_t)oh;
    }
    if (!on && !g_pd.overlay_on) {
        return;
    }
    if (on && off == HLE_ARENA_FULL) {
        return;                             /* not shown this time (logged by the upload) */
    }
    HleCmdOverlay *c = begin(HLE_OP_OVERLAY, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->on = on;
    if (on) {
        c->arena_off = off;
        c->w = w;
        c->h = h;
        memcpy(c->src, src, sizeof(src));
        memcpy(c->dst, dst, sizeof(dst));
    }
    const HleSurfDesc *sd = g_pd.shown_id ? object_desc(g_pd.shown_id) : NULL;
    c->fb_w = sd ? sd->w : 640;
    c->fb_h = sd ? sd->h : 480;
    hle_fe_commit();
    g_pd.overlay_on = on;
    g_pd.overlays++;
    /* the overlay is scanned out with the frame on screen: a title that
     * plays a movie through it without flipping still shows it (as D3D
     * mode's UpdateOverlay does) */
    if (on && g_pd.shown_id && hle_now_ns() - g_pd.shown_ns > 50000000) {
        HleCmdSwap *s = begin(HLE_OP_SWAP, sizeof(*s));
        s->value = hle_fe_next_value();
        s->swap_id = ++g_pd.swap_ids;
        s->surf = g_pd.shown_id;
        s->dump_off = HLE_SWAP_NO_DUMP;
        shown_part(obj_of(g_pd.shown_id), g_pd.shown_ax, g_pd.shown_ay, &s->src_w, &s->src_h);
        hle_hitch_pb_swap(s->swap_id);      /* a frame to the monitor, not a gap */
        hle_hitch_frame(s->swap_id, HLE_FR_FLIP);
        hle_fe_commit();
        hle_disp_present(s->swap_id);
        fps_log();
        g_pd.epoch++;
    }
    static unsigned s_logged;
    if (s_logged++ < 4 || g_dlog_left > 0) {
        hle_log("hakuX-hle-pb", "overlay %s: %ux%u -> %d,%d-%d,%d", on ? "on" : "off", w, h,
                on ? dst[0] : 0, on ? dst[1] : 0, on ? dst[2] : 0, on ? dst[3] : 0);
    }
}

void hle_pb_flipped(uint32_t start)
{
    if (!g_pd.on) {
        return;
    }
    /* The display at the target being drawn: D3D's immediate flip, done by
     * its trap's handler before the frame's FLIP_INCREMENT_WRITE (Dungeons
     * & Dragons Heroes, Project Gotham Racing 2, Blinx). It still presents
     * the frame before, as below: the GPU here finishes a frame well after
     * the guest flips it, and that frame's lead keeps the cadence (shown at
     * their swaps, Blinx's 30 fps frames went on screen as each was written,
     * 9-14 ms on, and juddered). The swap that comes is marked flipped, for
     * when no flip follows it (hle_pbd_scanout). */
    const uint32_t pa = start & 0x0FFFFFFFu;
    if (pa && __atomic_load_n(&g_pd.ahead_pa, __ATOMIC_ACQUIRE) == pa) {
        __atomic_store_n(&g_pd.flipped_pa, pa, __ATOMIC_RELEASE);
    }
    /* the oldest swap waiting; hle_pbd_scanout may drop them meanwhile */
    uint32_t tail = __atomic_load_n(&g_pd.pend_tail, __ATOMIC_ACQUIRE), id;
    do {
        if (tail == __atomic_load_n(&g_pd.pend_head, __ATOMIC_ACQUIRE)) {
            return;                                 /* a flip of nothing we drew */
        }
        id = g_pd.pend[tail % ARRAY_SIZE(g_pd.pend)];
    } while (!__atomic_compare_exchange_n(&g_pd.pend_tail, &tail, tail + 1, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    __atomic_store_n(&g_pd.flipped_ns, hle_now_ns(), __ATOMIC_RELAXED);     /* once a frame */
    hle_hitch_frame(id, HLE_FR_FLIP);
    hle_disp_present(id);
    fps_log();
    g_pd.shown++;
}

static uint64_t fnv64(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

/* The last slots hashed, per source (a program, the fixed-function key):
 * a draw's program is the same as the last one's almost always, and a
 * memcmp of its slots is cheaper than hashing them (Crimson Sea hashed
 * ~2300 programs a frame, ~4% of the decoder, with fnv64). */
typedef struct VshHash {
    uint32_t n;
    bool ok;
    uint64_t h;
    uint32_t pgen, start;               /* a program's: the memory and start it was read at */
    uint8_t slots[16 * HLE_VSH_MAX_SLOTS];
} VshHash;
static VshHash g_vh_prog, g_vh_ff;

/* Whether the back end has program h; if not, it is counted as sent. A
 * set: the list searched every draw was ~4000 long after a while. Full,
 * it starts over (the back end keeps them all: a few are sent again). */
static bool vsh_sent(uint64_t h)
{
    if (!h) {
        return false;                   /* the free mark: sent each time */
    }
    unsigned i = (unsigned)(h ^ h >> 29) & (SENT_SLOTS - 1);
    for (; g_pd.sent[i]; i = (i + 1) & (SENT_SLOTS - 1)) {
        if (g_pd.sent[i] == h) {
            return true;
        }
    }
    if (g_pd.nsent >= MAX_SENT) {
        memset(g_pd.sent, 0, sizeof(g_pd.sent));
        g_pd.nsent = 0;
        i = (unsigned)(h ^ h >> 29) & (SENT_SLOTS - 1);
    }
    g_pd.sent[i] = h;
    g_pd.nsent++;
    return false;
}

/* Program h (n slots) to the back end, once. */
static uint64_t vsh_put(uint64_t h, const void *slots, uint32_t n)
{
    if (vsh_sent(h)) {
        return h;
    }
    HleCmdVsh *c = begin(HLE_OP_VSH, sizeof(HleCmdVsh) + 16 * n);
    c->hash = h;
    c->nslots = n;
    memcpy(c + 1, slots, 16 * n);
    hle_fe_commit();
    return h;
}

/* n program slots (a program, or a fixed-function key) to the back end,
 * once: their hash (vc: the source's last, reused when the slots match). */
static uint64_t send_vsh(const void *slots, uint32_t n, VshHash *vc)
{
    uint64_t h;
    if (vc->ok && vc->n == n && !memcmp(vc->slots, slots, 16 * n)) {
        h = vc->h;
    } else {
        h = XXH3_64bits(slots, 16 * n);     /* fnv64's byte chain: ~2% of the decoder in Halo 2 */
        vc->ok = n <= HLE_VSH_MAX_SLOTS;
        if (vc->ok) {
            vc->n = n;
            vc->h = h;
            memcpy(vc->slots, slots, 16 * n);
        }
    }
    return vsh_put(h, slots, n);
}

static void vsh_forget(void)
{
    g_vh_prog.ok = g_vh_ff.ok = false;
}

/* The program from PROGRAM_START to its final instruction. */
static uint64_t program(void)
{
    uint32_t start = R(NV097_SET_TRANSFORM_PROGRAM_START);
    if (start >= HLE_VSH_MAX_SLOTS) {
        return 0;
    }
    /* no program memory written and the same start as the last draw's:
     * its program, its slots not compared again (up to 2 KB a draw) */
    VshHash *vc = &g_vh_prog;
    if (vc->ok && vc->pgen == g_pd.pgen && vc->start == start) {
        return vsh_put(vc->h, vc->slots, vc->n);
    }
    uint32_t n = 0;
    while (start + n < HLE_VSH_MAX_SLOTS) {
        n++;
        if (g_pd.pmem[start + n - 1][3] & 1) {      /* the final flag */
            break;
        }
    }
    const uint64_t h = send_vsh(g_pd.pmem[start], n, vc);
    vc->pgen = g_pd.pgen;
    vc->start = start;
    return h;
}

/* ---- the fixed-function vertex stage (shader/ffhw.h) ---- */

/* n method words from m on, as floats */
static void regs_f(float *out, uint32_t m, unsigned n)
{
    memcpy(out, &g_pd.r[m >> 2], 4 * n);
}

static uint8_t texgen_mode(uint32_t v)
{
    switch (v) {
    case 0x2400: return HLE_FFHW_TG_EYE;
    case 0x2401: return HLE_FFHW_TG_OBJECT;
    case 0x2402: return HLE_FFHW_TG_SPHERE;
    case 0x8512: return HLE_FFHW_TG_REFLECTION;
    case 0x8511: return HLE_FFHW_TG_NORMAL;
    default: return HLE_FFHW_TG_OFF;
    }
}

/* The fixed-function state as the methods left it: its key, sent as a
 * program's slots. */
static uint64_t ff_program(void)
{
    HleFfhwKey k;
    memset(&k, 0, sizeof(k));
    k.magic = HLE_FFHW_MAGIC;
    k.skin = R(0x0328) & 7;
    k.lighting = R(0x0314) & 1;
    k.normalize = R(0x03A4) & 1;
    uint32_t lc = R(0x0294);
    k.separate_spec = lc & 1;
    k.local_eye = (lc >> 16) & 1;
    k.alpha_mat_spec = (lc >> 17) & 1;
    k.specular = R(0x03B8) & 1;
    k.fog = R(0x02A4) ? (uint8_t)((R(0x02A0) & 7) + 1) : 0;
    uint32_t cm = R(0x0298);
    for (unsigned i = 0; i < 4; i++) {
        k.src[i] = (cm >> (2 * i)) & 3;
    }
    uint32_t le = R(0x03BC);
    for (unsigned i = 0; i < 8; i++) {
        k.light[i] = (le >> (2 * i)) & 3;
    }
    for (unsigned s = 0; s < 4; s++) {
        for (unsigned j = 0; j < 4; j++) {
            k.texgen[s][j] = texgen_mode(R(0x03C0 + 16 * s + 4 * j));
        }
        k.texmat |= (uint8_t)((R(0x0420 + 4 * s) & 1) << s);
    }
    k.points = R(0x0318) & 1;
    return send_vsh(&k, HLE_FFHW_SLOTS, &g_vh_ff);
}

/* Its constants (ffhw.h's layout) from the transform and lighting methods. */
static void ff_consts(float (*c)[4])
{
    memset(c, 0, HLE_VSH_CONSTANTS * 16);
    regs_f(c[HLE_FFHW_C_COMPOSITE], 0x0680, 16);
    regs_f(c[HLE_FFHW_C_MODELVIEW], 0x0480, 64);
    regs_f(c[HLE_FFHW_C_INVMODELVIEW], 0x0580, 64);
    regs_f(c[HLE_FFHW_C_TEXMAT], 0x06C0, 64);
    regs_f(c[HLE_FFHW_C_TEXGEN], 0x0840, 64);
    regs_f(c[HLE_FFHW_C_VPOFF], NV097_SET_VIEWPORT_OFFSET, 4);
    regs_f(c[HLE_FFHW_C_FOGPLANE], 0x09D0, 4);
    regs_f(c[HLE_FFHW_C_EYE], 0x0A50, 4);
    regs_f(c[HLE_FFHW_C_AMBIENT], 0x0A10, 3);
    regs_f(&c[HLE_FFHW_C_AMBIENT][3], 0x03B4, 1);
    regs_f(c[HLE_FFHW_C_EMISSION], 0x03A8, 3);
    float sp[6], spb[6];
    regs_f(sp, 0x09E0, 6);
    regs_f(spb, 0x1E28, 6);
    c[HLE_FFHW_C_PARAMS][0] = hle_ffhw_specular_power(sp);
    c[HLE_FFHW_C_PARAMS][1] = hle_ffhw_specular_power(spb);
    c[HLE_FFHW_C_PARAMS][2] = (R(0x043C) & 0x1FF) / 8.0f;
    regs_f(c[HLE_FFHW_C_POINT], 0x0A30, 8);
    for (unsigned i = 0; i < 8; i++) {
        float (*l)[4] = &c[HLE_FFHW_C_LIGHTS + HLE_FFHW_LIGHT_VEC4 * i];
        uint32_t b = 0x1000 + 128 * i;
        regs_f(l[HLE_FFHW_L_AMBIENT], b + 0x00, 3);
        regs_f(l[HLE_FFHW_L_DIFFUSE], b + 0x0C, 3);
        regs_f(l[HLE_FFHW_L_SPECULAR], b + 0x18, 3);
        regs_f(l[HLE_FFHW_L_RANGE], b + 0x24, 1);
        regs_f(l[HLE_FFHW_L_HALF], b + 0x28, 3);
        regs_f(l[HLE_FFHW_L_DIRECTION], b + 0x34, 3);
        regs_f(l[HLE_FFHW_L_FALLOFF], b + 0x40, 3);
        regs_f(l[HLE_FFHW_L_SPOT], b + 0x4C, 4);
        regs_f(l[HLE_FFHW_L_POSITION], b + 0x5C, 3);
        regs_f(l[HLE_FFHW_L_ATTENUATION], b + 0x68, 3);
    }
}

/* Bytes of one element of an NV2A vertex array format (type 3:0, size 7:4) */
static uint32_t attr_bytes(uint32_t fmt)
{
    uint32_t type = fmt & 0xF, size = (fmt >> 4) & 0xF;
    switch (type) {
    case 0: case 4: return size;            /* UB_D3D, UB_OGL */
    case 1: case 5: return 2 * size;        /* S1, S32K */
    case 2: return 4 * size;                /* F */
    case 6: return 4 * size;                /* CMP: three in a word */
    default: return 4 * size;
    }
}

/* ---- 2D blits ---- */

/* The colour object whose memory starts at pa (the one bound last), else 0 */
static uint32_t object_at(uint32_t pa)
{
    const PbObj *best = NULL;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *o = &g_pd.objs[i];
        const HleFmtInfo *oi = o->d.pa == pa && !o->cube ? hle_fmt_info(o->d.xfmt) : NULL;
        if (oi && !(oi->flags & HLE_FI_DEPTH) && (!best || o->bound > best->bound)) {
            best = o;
        }
    }
    return best ? best->id : 0;
}

/* A colour target whose memory is exactly face f of a cube (w x w texels
 * of bytes each at pa + f * stride): swizzled, or linear rows of w. */
static bool face_target(const PbObj *o, uint32_t pa, uint32_t w, uint32_t bytes)
{
    const HleFmtInfo *oi = !o->cube && !o->d.parent && o->d.pa == pa && o->d.w == w && o->d.h == w ?
                           hle_fmt_info(o->d.xfmt) : NULL;
    return oi && !(oi->flags & (HLE_FI_DEPTH | HLE_FI_DXT)) && oi->bpp / 8 == bytes &&
           (!o->d.pitch || o->d.pitch == w * bytes);
}

/* A cube texture (format code, w x w) over the memory of six face targets
 * (pa + face * stride): an image of its own whose faces those targets
 * become, views into it (as D3D mode's cube render targets), so what the
 * faces render is what the cube samples. Every target over a face's memory
 * becomes its view, whatever its layout, as a 2D texture over a target's
 * memory samples its image (texture). Made once; 0 when a face has no
 * target. Forza renders its cars' reflection cube this way; sampled as
 * face 0's 2D image, the cars' glass and roofs came out white. Sega GT
 * 2002's sun cube for its cars' highlights is cleared black each frame
 * through linear targets (five faces) and its sun drawn into the sixth
 * swizzled: with swizzled faces only, no cube, and the cars were white. */
static uint32_t cube_of_targets(uint32_t pa, uint32_t code, uint32_t w, uint32_t stride)
{
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        if (g_pd.objs[i].cube && g_pd.objs[i].d.pa == pa && g_pd.objs[i].d.w == w) {
            return g_pd.objs[i].id;
        }
    }
    const HleFmtInfo *fi = hle_fmt_info(code);
    const uint32_t bytes = fi ? fi->bpp / 8 : 0;
    uint32_t xfmt = 0;  /* a target's: one the GPU renders (the texture's may not be, A4R4G4B4) */
    for (unsigned f = 0; f < 6; f++) {
        bool found = false;
        for (unsigned i = 0; i < g_pd.nobjs && !found; i++) {
            found = face_target(&g_pd.objs[i], pa + f * stride, w, bytes);
            if (found && !f) {
                xfmt = g_pd.objs[i].d.xfmt;
            }
        }
        if (!found || g_pd.nobjs >= MAX_OBJS) {
            return 0;
        }
    }
    PbObj *c = &g_pd.objs[g_pd.nobjs++];
    memset(c, 0, sizeof(*c));
    c->id = g_pd.next_id++;
    c->cube = true;
    c->d = (HleSurfDesc){ .id = c->id, .xfmt = xfmt, .w = w, .h = w, .pa = pa, .levels = 1, .faces = 6 };
    HleCmdObj *oc = begin(HLE_OP_OBJ_CREATE, sizeof(*oc));
    oc->s = c->d;
    hle_fe_commit();
    for (unsigned f = 0; f < 6; f++) {
        for (unsigned i = 0; i < g_pd.nobjs; i++) {
            PbObj *o = &g_pd.objs[i];
            if (!face_target(o, pa + f * stride, w, bytes)) {
                continue;
            }
            /* the face's own image goes; the target is a view from now on */
            HleCmdObjDestroy *x = begin(HLE_OP_OBJ_DESTROY, sizeof(*x));
            x->after = 0;
            x->id = o->id;
            hle_fe_commit();
            if (g_pd.rt == o->id) {
                g_pd.rt = 0;                /* bound again at the next draw */
                g_pd.rt_dirty = true;
            }
            o->id = g_pd.next_id++;
            o->d.id = o->id;
            o->d.parent = c->id;
            o->d.face = (uint16_t)f;
            o->d.levels = 1;
            o->d.faces = 1;
            HleCmdObj *v = begin(HLE_OP_OBJ_CREATE, sizeof(*v));
            v->s = o->d;
            hle_fe_commit();
        }
    }
    hle_log("hakuX-hle-pb", "cube %u: %ux%u at %08x (face stride %x) from its face targets", c->id,
            w, w, pa, stride);
    return c->id;
}

static const HleSurfDesc *object_desc(uint32_t id)
{
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        if (g_pd.objs[i].id == id) {
            return &g_pd.objs[i].d;
        }
    }
    return NULL;
}

/* A blit from a linear render target into another, or into memory with none
 * (one made there with the source's format, which a texture over that
 * memory then samples): the GPU's copy of the images, at their scale, the
 * memory left as it is. Its rows there would be the source's memory's, older
 * than its image: Otogi 2 blits its blur target back over the blur's source
 * between passes, and copies its frame for the colour passes; from the
 * memory's rows, its glow drew black. A destination whose image is newer
 * than its memory is guarded (coh_drawn). Each image takes its memory's
 * newer bytes first (the CPU's, a copy's: coh_first_write; the
 * destination's, unless the copy covers all of it).
 *
 * False: the copy is memory's (hle_pbd_copy_begin), the CPU's writes into
 * the targets there gone into their images first (rt_cpu_sync). A swizzled
 * target's memory is a block of bytes to the 2D engine, its pixels not where
 * the blit's rows say, read or written. D3D's CopyRects on swizzled surfaces
 * does that: Morrowind copies its inventory's character out of its target
 * (cleared red) and back, keying the red out on the CPU in between; as image
 * copies, the character came back on red, a black band over its head (the
 * copy's 4096 x 64 rows into the 256 x 256 target). */
bool hle_pbd_blit_images(uint32_t sbase, uint32_t sp, uint32_t dbase, uint32_t dp, uint32_t bpp,
                         uint32_t ix, uint32_t iy, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h)
{
    if (!g_pd.on || !g_pd.device) {
        return false;
    }
    uint32_t src = object_at(sbase), dst = object_at(dbase);
    PbObj *so = obj_of(src), *dd = obj_of(dst);
    if (g_dlog_left > 0) {
        hle_log("hakuX-hle-pb", "blit %08x/%u -> %08x/%u bpp %u %ux%u from %u,%u to %u,%u objects %u -> %u",
                sbase, sp, dbase, dp, bpp, w, h, ix, iy, ox, oy, src, dst);
    }
    if (so && so->d.pitch && !dd && dp) {
        dst = object(dbase, so->d.xfmt, dp, MAX(dp / bpp, ox + w), oy + h);
        so = obj_of(src);                   /* (a slot may have gone to it) */
        dd = obj_of(dst);
    }
    if (!so || !so->d.pitch || !dd || !dd->d.pitch || dst == src) {
        rt_cpu_sync(src);
        rt_cpu_sync(dst);
        return false;
    }
    coh_first_write(so, false);
    coh_first_write(dd, !ox && !oy && ox + w >= dd->d.w && oy + h >= dd->d.h);
    HleCmdCopy *c = begin(HLE_OP_COPY, sizeof(*c));
    c->src = src;
    c->dst = dst;
    c->nrects = 1;
    c->rect[0] = (HleRect){ (int32_t)ix, (int32_t)iy, (int32_t)(ix + w), (int32_t)(iy + h) };
    c->x[0] = (int32_t)ox;
    c->y[0] = (int32_t)oy;
    hle_fe_commit();
    dd->written = ++g_pd.wseq;
    coh_drawn(dd);
    return true;
}

/* After a blit's copy in memory (hle_pbd_copy_begin, _done; queued: dep):
 * the linear render target at dbase takes its rows into its image as well.
 * One the coherence keeps, only when the copy was made at once over memory
 * no newer than its image (its memory and image both have them then, its
 * image still the newer elsewhere if it was, and guarded again); else its
 * memory is the newer, uploaded at its next draw. One the old paths keep
 * waits for a queued copy first. */
void hle_pbd_blit(uint32_t dbase, uint32_t dp, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h, uint64_t dep)
{
    const uint32_t rows_id = g_coh_rows.id;
    g_coh_rows.id = 0;
    if (!g_pd.on || !g_pd.device) {
        return;
    }
    const uint32_t dst = object_at(dbase);
    PbObj *dd = obj_of(dst);
    if (!dd || !dd->d.pitch) {
        return;                             /* a swizzled target: its memory is the newer */
    }
    uint32_t len;
    const bool kept = coh_managed(dd, &len);
    if (kept && (dep || rows_id != dst || g_coh_rows.mem_newer)) {
        return;
    }
    if (!kept && dep) {
        coh_wait(dep);
    }
    const HleRect r = { (int32_t)ox, (int32_t)oy, (int32_t)(ox + w), (int32_t)(oy + h) };
    const int32_t pt[1][2] = { { (int32_t)ox, (int32_t)oy } };
    hle_res_upload(dst, dbase, dp, 1, &r, pt);
    dd->written = ++g_pd.wseq;
    if (kept) {
        const uint64_t was = dd->synced;
        coh_synced(dd, len);                /* the copy's bytes are the image's too */
        if (g_coh_rows.img_newer) {
            dd->synced = was;               /* the rest of its image is still the newer */
            coh_drawn(dd);
        }
    }
}

/* ---- textures ---- */

static uint32_t hash32(const void *p, size_t n)
{
    return (uint32_t)XXH3_64bits(p, n);
}

/* skipped draws by reason (debug: the log line) */
enum {
    SK_EMIT_ARENA,
    SK_DRAW_IMM_ARENA,
    SK_DRAW_INLINE_ARENA,
    SK_DRAW_ARRAYS_SPAN,
    SK_DRAW_ARRAYS_ARENA,
    SK_DRAW_ARRAYS_ARENA2,
    SK_DRAW_EMPTY,
    SK_DRAW_NO_TARGET,
    SK_N
};
static const char *const k_skip_name[SK_N] = {
    "emit_arena",
    "draw_imm_arena",
    "draw_inline_arena",
    "draw_arrays_span",
    "draw_arrays_arena",
    "draw_arrays_arena2",
    "draw_empty",
    "draw_no_target",
};
static uint64_t g_skips[SK_N];

static void skip(unsigned why)
{
    g_pd.skipped++;
    g_skips[why]++;
}

void hle_pbd_skip_reasons(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < SK_N && o < len; i++) {
        if (g_skips[i]) {
            o += (size_t)snprintf(buf + o, len - o, " %s=%llu", k_skip_name[i],
                                  (unsigned long long)g_skips[i]);
        }
    }
}

/* Every texture forgotten (the table full): their images go. */
static void tex_flush(void)
{
    for (unsigned i = 0; i < TEX_SLOTS; i++) {
        if (g_pd.tex[i].id) {
            HleCmdObjDestroy *x = begin(HLE_OP_OBJ_DESTROY, sizeof(*x));
            x->after = 0;
            x->id = g_pd.tex[i].id;
            hle_fe_commit();
        }
    }
    memset(g_pd.tex, 0, TEX_SLOTS * sizeof(PbTex));
    g_pd.ntex = 0;
    g_pd.tex_flushes++;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The table full: the quarter of its textures checked longest ago go, their
 * images with them, and the rest are placed again (open addressing has no
 * deletion). Keys of memory a title has reused piled up until every texture
 * went at once, and the working set was defined again over the next frames.
 * None checked this epoch goes: the draw being built may hold its id. */
static void tex_evict(void)
{
    static uint64_t when[TEX_SLOTS];
    static PbTex keep[TEX_SLOTS];
    unsigned n = 0, nkeep = 0;
    for (unsigned i = 0; i < TEX_SLOTS; i++) {
        if (g_pd.tex[i].id) {
            when[n++] = g_pd.tex[i].checked;
        }
    }
    qsort(when, n, sizeof(when[0]), cmp_u64);
    /* a quarter: all checked before the cut, and as many checked at it as
     * that leaves room for (a scene's textures all checked at one epoch) */
    const uint64_t cut = when[n / 4];
    unsigned ties = n / 4;
    for (unsigned i = 0; when[i] < cut; i++) {
        ties--;
    }
    for (unsigned i = 0; i < TEX_SLOTS; i++) {
        PbTex *t = &g_pd.tex[i];
        if (!t->id) {
            continue;
        }
        bool go = t->checked != g_pd.epoch && (t->checked < cut || (t->checked == cut && ties));
        ties -= go && t->checked == cut;
        if (!go) {
            keep[nkeep++] = *t;
            continue;
        }
        HleCmdObjDestroy *x = begin(HLE_OP_OBJ_DESTROY, sizeof(*x));
        x->after = 0;
        x->id = t->id;
        hle_fe_commit();
    }
    if (nkeep == n) {
        tex_flush();                    /* all of them checked this epoch */
        return;
    }
    memset(g_pd.tex, 0, TEX_SLOTS * sizeof(PbTex));
    for (unsigned i = 0; i < nkeep; i++) {
        uint32_t h = hash32(keep[i].key, sizeof(keep[i].key)) % TEX_SLOTS;
        while (g_pd.tex[h].id) {
            h = (h + 1) % TEX_SLOTS;
        }
        g_pd.tex[h] = keep[i];
    }
    g_pd.ntex = nkeep;
    hle_log("hakuX-hle-pb", "textures: table full, the %u checked longest ago dropped (%u kept)", n - nkeep,
            nkeep);
}

/* A 32-bit linear colour texture (w x h at pa, channels from the depth
 * word's bytes as map says) over a D24S8 target's memory reads the target's
 * depth words, as the NV2A's memory holds them (Conker's glow mask: its
 * depth buffer as R8G8B8A8, stencil in alpha). Its own image, converted from
 * the depth image before each draw that samples it (the back end skips an
 * unchanged one); 0 when no such target is there. xfmt: the texture's
 * format, 32-bit, or LIN_L16 twice the target's width (map[0] =
 * HLE_ZREAD_HALVES): Midtown Madness 3 reads its depth buffer as 1280 x 480
 * L16, the words' upper halves its depth's top 16 bits, into the alpha its
 * sky dome blends in by (DST_ALPHA); read from memory the GPU never wrote,
 * the alpha stayed 0 and the sky was the clear colour. */
static uint32_t zread(uint32_t pa, uint32_t w, uint32_t h, const uint8_t map[4], uint32_t xfmt)
{
    const bool halves = map[0] == HLE_ZREAD_HALVES;
    const uint32_t bpp = halves ? 2 : 4;
    const PbObj *src = NULL;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *o = &g_pd.objs[i];
        if (o->d.pa == pa && o->d.w * (halves ? 2 : 1) == w && o->d.h == h &&
            (o->d.xfmt == HLE_XFMT_D24S8 || o->d.xfmt == HLE_XFMT_LIN_D24S8) &&
            (!src || o->bound > src->bound)) {
            src = o;
        }
    }
    if (!src) {
        return 0;
    }
    /* the memory's last writer: a colour target bound there after the depth
     * buffer holds the texels the texture reads. Oddworld copies its scene
     * into a 640 x 480 colour target and binds the same memory as a depth
     * buffer for a later pass; its composite then sampled that pass's depth
     * (cleared: 0xFFFFFF00, yellow as A8R8G8B8) instead of the scene, and the
     * frame went yellow behind the level's loading hint. Or one written
     * there since the depth buffer last was (a draw, clear or blit): Splinter
     * Cell Chaos Theory's thermal vision blits its frame over its depth
     * buffer and shimmers it from there; the depth's bytes came out as
     * green / yellow / red contour bands. That target's texels are read in
     * the texture's byte order (tex_order: Mercenaries' sky) */
    for (unsigned k = 0; k < g_pd.nobjs; k++) {
        const PbObj *o = &g_pd.objs[k];
        const HleFmtInfo *oi = o->d.pa == pa && !o->cube &&
                               (o->bound > src->bound || o->written > src->written) ?
                               hle_fmt_info(o->d.xfmt) : NULL;
        if (oi && !(oi->flags & HLE_FI_DEPTH)) {
            return 0;
        }
    }
    unsigned i = 0;
    while (i < g_pd.nzr && !(g_pd.zr[i].pa == pa && g_pd.zr[i].w == w && g_pd.zr[i].h == h &&
                             g_pd.zr[i].xfmt == xfmt)) {
        i++;
    }
    const bool fresh = i == g_pd.nzr;
    if (fresh) {
        if (g_pd.nzr == MAX_ZREADS) {
            i = (unsigned)(g_pd.binds % MAX_ZREADS);  /* one replaced */
            HleCmdObjDestroy *x = begin(HLE_OP_OBJ_DESTROY, sizeof(*x));
            x->after = 0;
            x->id = g_pd.zr[i].id;
            hle_fe_commit();
        } else {
            g_pd.nzr++;
        }
        g_pd.zr[i].id = g_pd.next_id++;
        g_pd.zr[i].pa = pa;
        g_pd.zr[i].w = w;
        g_pd.zr[i].h = h;
        g_pd.zr[i].xfmt = xfmt;
        HleCmdObj *c = begin(HLE_OP_OBJ_CREATE, sizeof(*c));
        memset(c, 0, sizeof(*c));
        c->s = (HleSurfDesc){ .id = g_pd.zr[i].id, .xfmt = halves ? xfmt : HLE_XFMT_LIN_A8R8G8B8,
                              .w = w, .h = h, .pa = pa, .pitch = w * bpp, .levels = 1, .faces = 1 };
        hle_fe_commit();
        hle_log("hakuX-hle-pb", "texture %08x (%ux%u) reads depth target %u", pa, w, h, src->id);
    }
    /* What the CPU wrote into the memory since is the memory's now: its rows
     * go into the texture's image, and the back end converts the depth image
     * again only once the depth buffer is written after them (cmd_zread).
     * Splinter Cell Chaos Theory draws its boot and loading pictures from its
     * depth buffer's memory, written on the CPU: converted from the cleared
     * depth image instead, they were 0xFFFFFF00, a yellow screen) */
    {
        const uint32_t len = w * bpp * h;
        if (fresh ? hle_mem_written(pa, len) : span_written(pa, len, g_pd.zr[i].edge_hash)) {
            const HleRect r = { 0, 0, (int32_t)w, (int32_t)h };
            const int32_t pt[1][2] = { { 0, 0 } };
            hle_res_upload(g_pd.zr[i].id, pa, w * bpp, 1, &r, pt);
            if (g_pd.zr_uploads++ < 8) {
                hle_log("hakuX-hle-pb", "texture %08x over depth target %u: the CPU's writes (%ux%u, "
                        "first 8 logged)", pa, src->id, w, h);
            }
            span_edges_set(pa, len, g_pd.zr[i].edge_hash);
        } else if (fresh) {
            span_edges_set(pa, len, g_pd.zr[i].edge_hash);
        }
    }
    g_pd.zr[i].src = src->id;
    HleCmdZRead *z = begin(HLE_OP_ZREAD, sizeof(*z));
    z->dst = g_pd.zr[i].id;
    z->src = src->id;
    memcpy(z->map, map, 4);
    hle_fe_commit();
    return g_pd.zr[i].id;
}

/* A texture's span starts and ends inside pages it may share with other
 * data the CPU rewrites (dynamic vertices, other resources). Checked by the
 * pages' dirty bits alone, at every use, such a page was write-protected
 * again by each draw and written by the CPU before the next: a host fault a
 * page a draw and the texture defined (uploaded) again each time; Burnout 3
 * redefined ~90 textures a frame and took ~14k faults a second. Instead the
 * pages wholly inside the span count by their dirty bits; the first and last
 * by the span's bytes on them against their hashes at definition. A dirty
 * edge page is left writable (no fault per write), and its bytes compared at
 * each use: XXH3 (NEON), as a serial multiply chain they were ~22% of the
 * decoder in a Burnout 3 race. */
static uint64_t span_hash(uint32_t pa, uint32_t len)
{
    const uint8_t *m = hle_pa_ptr(pa, len);
    return m ? XXH3_64bits(m, len) : 0;
}

static void tex_edges_set(PbTex *t)
{
    const uint32_t lo = t->pa, hi = lo + t->bytes, p0 = lo & ~4095u, pl = (hi - 1) & ~4095u;
    t->edge_hash[0] = span_hash(lo, MIN(hi, p0 + 4096) - lo);
    t->edge_hash[1] = pl != p0 ? span_hash(pl, hi - pl) : 0;
}

static bool edge_dirty(uint32_t page)
{
    return hle_host_mem_dirty((uint32_t)(hle_ram_addr_base() + page));
}

static bool tex_written(const PbTex *t)
{
    if (!t->bytes) {
        return false;
    }
    const uint32_t lo = t->pa, hi = lo + t->bytes, p0 = lo & ~4095u, pl = (hi - 1) & ~4095u;
    const bool mid = pl > p0 + 4096 && hle_mem_written(p0 + 4096, pl - p0 - 4096);
    const bool first = edge_dirty(p0) && span_hash(lo, MIN(hi, p0 + 4096) - lo) != t->edge_hash[0];
    const bool last = pl != p0 && edge_dirty(pl) && span_hash(pl, hi - pl) != t->edge_hash[1];
    return mid || first || last;
}

/* Palettes hashed, by address (direct-mapped), their bytes kept for the
 * compare: equal bytes, equal hash. A palette was hashed for each draw of
 * each paletted stage (128 rounds of multiplies; Crimson Sea ~4% of the
 * decoder); one entry per stage missed, its stages switching palettes
 * from draw to draw. */
#define PAL_HASHES 64u
typedef struct PalHash {
    bool ok;
    uint32_t pa, n, hash;
    uint8_t bytes[4 * 256];
} PalHash;
static PalHash g_pal_hash[PAL_HASHES];

/* A linear texture over memory a colour target with its pitch and texel size
 * covers from an earlier address (a part of it, past its first row or texel):
 * that part of the target, copied by the GPU into the texture's image (the
 * object there, dst, else a new one) when the target was drawn after the
 * image was last written, before the draw that samples it (the NV2A reads
 * the memory before the draw writes it). Breakdown draws its 640 x 448 scene
 * at row 16 of a 640 x 480 frame buffer, into a target of its own there or
 * the frame buffer clipped to it, and samples it from row 16 for its post
 * pass: from memory, which holds nothing the GPU drew, or the target there
 * not drawn this frame, the pass painted the scene black under its HUD. 0:
 * no such target. */
static uint32_t target_part(uint32_t pa, uint32_t code, const HleFmtInfo *fi, uint32_t pitch,
                            uint32_t w, uint32_t h, PbObj *dst)
{
    PbObj *src = NULL;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        const HleFmtInfo *oi = !o->cube && o->d.pitch == pitch && !o->d.parent ? hle_fmt_info(o->d.xfmt) : NULL;
        if (oi && !(oi->flags & HLE_FI_DEPTH) && oi->bpp == fi->bpp && o->d.pa < pa &&
            pa - o->d.pa < pitch * o->d.h && (!src || o->written > src->written)) {
            src = o;
        }
    }
    const uint32_t bytes = fi->bpp / 8, rel = src ? pa - src->d.pa : 0;
    const uint32_t x = bytes ? rel % pitch / bytes : 0, y = src ? rel / pitch : 0;
    if (!src || !bytes || rel % pitch % bytes || x + w > src->d.w || y + h > src->d.h || !src->written ||
        (dst && src->written <= dst->written)) {
        return dst ? dst->id : 0;           /* the image there is the newer */
    }
    if (!dst) {
        dst = obj_of(object(pa, code, pitch, w, h));
        if (!dst || dst == src) {
            return 0;
        }
    }
    /* each image with its memory's newer bytes first (coh_first_write: the
     * CPU's, a copy's; the part's, unless the copy covers all of it) */
    coh_first_write(src, false);
    coh_first_write(dst, w >= dst->d.w && h >= dst->d.h);
    HleCmdCopy *c = begin(HLE_OP_COPY, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->src = src->id;
    c->dst = dst->id;
    c->nrects = 1;
    c->rect[0] = (HleRect){ (int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h) };
    hle_fe_commit();
    hle_mem_written(pa, pitch * h);         /* the image holds the copy, not the memory */
    dst->written = ++g_pd.wseq;
    coh_drawn(dst);
    return dst->id;
}

/* The texture stage s samples: the object id, 0 if it can't be drawn;
 * *unset when the stage has none (CONTROL0 disabled), *lin when its format
 * is linear (sampled by texel coordinates). */
static uint32_t texture(unsigned s, bool *unset, bool *lin)
{
    uint32_t b = 64 * s;
    *unset = false;
    *lin = false;
    if (!(R(NV097_SET_TEXTURE_CONTROL0 + b) & 0x40000000u)) {
        *unset = true;
        return 0;
    }
    uint32_t off = R(NV097_SET_TEXTURE_OFFSET + b), fmt = R(NV097_SET_TEXTURE_FORMAT + b);
    uint32_t code = HLE_FMT_CODE(fmt), size = 0, pa = off & 0x0FFFFFFFu;
    const HleFmtInfo *fi = hle_fmt_info(code);
    *lin = fi && (fi->flags & HLE_FI_LIN);
    if (*lin) {
        uint32_t rect = R(NV097_SET_TEXTURE_IMAGE_RECT + b), pitch = R(NV097_SET_TEXTURE_CONTROL1 + b) >> 16;
        size = (MAX(rect >> 16, 1u) - 1) | (MAX(rect & 0xFFFF, 1u) - 1) << HLE_SIZE_HEIGHT_SHIFT |
               (MAX(pitch, 64u) / 64 - 1) << HLE_SIZE_PITCH_SHIFT;
        static const uint8_t k_halves[4] = { HLE_ZREAD_HALVES, 0, 0, 0 };
        uint8_t map[4];
        uint32_t zid = hle_res_zread_map(code, map) ? zread(pa, rect >> 16, rect & 0xFFFF, map, code) :
                       code == HLE_XFMT_LIN_L16 ? zread(pa, rect >> 16, rect & 0xFFFF, k_halves, code) : 0;
        if (zid) {
            return zid;
        }
    }
    /* a render target's memory: its image (with what the CPU wrote there).
     * Only a target of the texture's kind, colour or depth, and of those
     * the one bound last: memory D3D reuses holds several (Fable's glow
     * textures sampled a stale depth target there, compared as a shadow
     * map, and washed the frame out). */
    const bool want_depth = fi && (fi->flags & HLE_FI_DEPTH);
    if ((fmt & 4) && fi && (fi->flags & HLE_FI_SWZ) && fi->bpp >= 8) {
        /* a cube: its faces' targets, if that is what its memory is */
        const uint32_t cw = 1u << ((fmt >> 20) & 0xF), levels = MAX((fmt >> 16) & 0xF, 1u);
        uint32_t stride = 0;
        for (uint32_t l = 0; l < levels; l++) {
            const uint32_t lw = MAX(cw >> l, 1u);
            stride += lw * lw * (fi->bpp / 8);
        }
        stride = (stride + 127) & ~127u;    /* faces 128-byte aligned (XGSetCubeTextureHeader) */
        const uint32_t cube = cube_of_targets(pa, code, cw, stride);
        if (cube) {
            return cube;
        }
    }
    /* A compressed texture is never a target's image (the NV2A renders into
     * no DXT format): over a target's memory it is the title's data, loaded
     * there since. Forza's loading screen before each track is a 1024 x 512
     * DXT5 picture in the memory its menus' depth buffer and a 1280 x 240
     * colour pass used; sampled as that target, the CPU's DXT bytes uploaded
     * into it as colour, the picture was the clear colour (teal). */
    const bool dxt = fi && (fi->flags & HLE_FI_DXT);
    PbObj *best = NULL;
    /* nor a colour target whose memory is newer than its image
     * (coh_mem_newer: a copy into it, its twin's image landed there, the
     * title's writes): the memory is what the NV2A samples */
    coh_adopt();
    for (unsigned i = 0; !dxt && i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        const HleFmtInfo *oi = o->d.pa == pa && !o->cube ? hle_fmt_info(o->d.xfmt) : NULL;
        if (oi && !!(oi->flags & HLE_FI_DEPTH) == want_depth && (!best || o->bound > best->bound) &&
            (want_depth || !coh_mem_newer(o))) {
            best = o;
        }
    }
    if (best) {
        rt_cpu_sync(best->id);
    }
    if (fi && (fi->flags & HLE_FI_LIN) && !want_depth && !dxt && (!best || best->d.pitch)) {
        const uint32_t rect = R(NV097_SET_TEXTURE_IMAGE_RECT + b);
        const uint32_t part = target_part(pa, code, fi, R(NV097_SET_TEXTURE_CONTROL1 + b) >> 16,
                                          rect >> 16, rect & 0xFFFF, best);
        if (part) {
            return part;
        }
    }
    if (best) {
        return best->id;
    }
    uint32_t pal_reg = code == HLE_XFMT_P8 ? R(NV097_SET_TEXTURE_PALETTE + b) : 0;
    uint32_t key[4] = { off, fmt, size, pal_reg };
    uint32_t h = hash32(key, sizeof(key)) % TEX_SLOTS;
    PbTex *t = &g_pd.tex[h];
    while (t->id && memcmp(t->key, key, sizeof(key))) {
        h = (h + 1) % TEX_SLOTS;
        t = &g_pd.tex[h];
    }
    bool redefine = false;
    if (!t->id) {
        if (g_pd.ntex >= TEX_MAX) {
            tex_evict();
            return texture(s, unset, lin);
        }
        memcpy(t->key, key, sizeof(key));
        t->id = g_pd.next_id++;
        t->checked = g_pd.epoch;
        g_pd.ntex++;
        redefine = true;
    } else if (t->ok) {
        /* texels written since, checked at every use: the decoder copies a
         * texture's texels when it decodes the draw, before any later fence
         * lets the CPU write them again (Crimson Skies writes each label's
         * string into one texture between its draws) */
        t->checked = g_pd.epoch;
        coh_pend_wait(t->pa, t->bytes);             /* a copy queued into it lands first */
        redefine = tex_written(t);
    }
    static uint32_t pal[256];
    uint32_t npal = 0, pal_hash = 0;
    const uint8_t *pal_mem = NULL;
    if (pal_reg) {
        /* hashed by words where it is (byte by byte, 4 stages a draw, it was
         * a sixth of the decoder's time); copied only to define */
        npal = 256u >> ((pal_reg >> 2) & 3);
        coh_pend_wait(pal_reg & 0x0FFFFFC0u, 4 * npal);    /* a copy queued into it lands first */
        pal_mem = hle_pa_ptr(pal_reg & 0x0FFFFFC0u, 4 * npal);
        if (pal_mem) {
            const uint32_t ppa = pal_reg & 0x0FFFFFC0u;
            PalHash *ph = &g_pal_hash[(ppa >> 6) % PAL_HASHES];
            if (ph->ok && ph->pa == ppa && ph->n == npal && !memcmp(ph->bytes, pal_mem, 4 * npal)) {
                pal_hash = ph->hash;
            } else {
                uint64_t h = 0x9E3779B97F4A7C15ull;
                for (uint32_t i = 0; i < 4 * npal; i += 8) {
                    uint64_t w;
                    memcpy(&w, pal_mem + i, 8);
                    h = (h ^ w) * 0x100000001B3ull;
                    h ^= h >> 29;
                }
                pal_hash = (uint32_t)(h ^ (h >> 32));
                ph->ok = true;
                ph->pa = ppa;
                ph->n = npal;
                ph->hash = pal_hash;
                memcpy(ph->bytes, pal_mem, 4 * npal);
            }
        } else {
            npal = 0;
        }
        redefine |= t->ok && pal_hash != t->pal_hash;
    }
    if (redefine && pal_mem) {
        memcpy(pal, pal_mem, 4 * npal);
    }
    if (redefine) {
        const uint32_t h5[5] = { HLE_COMMON_TYPE_TEXTURE | 1, off, 0, fmt, size };
        uint32_t bytes = 0;
        coh_pend_wait(pa, hle_draw_tex_bytes(h5));  /* a copy queued into its texels lands first */
        t->ok = hle_draw_tex_define(t->id, h5, pal, npal, &bytes) != 0;
        t->pa = pa;
        t->bytes = bytes;
        t->pal_hash = pal_hash;
        if (t->ok) {
            hle_mem_written(pa, bytes);             /* clean from this definition on */
            tex_edges_set(t);
            mem_supersedes(pa, bytes);
            g_pd.tex_defs++;
        }
    }
    return t->ok ? t->id : 0;
}

/* Stage s's 32-bit byte order where the object it samples (id) is a colour
 * target, A8R8G8B8 or X8R8G8B8 as the NV2A renders, and the texture's format
 * another 8888 order: HLE_DRAW_ORD_*, the back end reading the target's
 * texels in the texture's order (vk/surf.c hle_surf_order_view). 0: the
 * target's own order, or no target (a texture, a z-read: their own bytes). */
static uint32_t tex_order(unsigned s, uint32_t id)
{
    uint32_t ord;
    switch (HLE_FMT_CODE(R(NV097_SET_TEXTURE_FORMAT + 64 * s))) {
    case HLE_XFMT_A8B8G8R8: case HLE_XFMT_LIN_A8B8G8R8:
        ord = HLE_DRAW_ORD_ABGR;
        break;
    case HLE_XFMT_B8G8R8A8: case HLE_XFMT_LIN_B8G8R8A8:
        ord = HLE_DRAW_ORD_BGRA;
        break;
    case HLE_XFMT_R8G8B8A8: case HLE_XFMT_LIN_R8G8B8A8:
        ord = HLE_DRAW_ORD_RGBA;
        break;
    default:
        return 0;
    }
    const PbObj *o = obj_of(id);
    if (!o || o->cube) {
        return 0;
    }
    switch (o->d.xfmt) {
    case HLE_XFMT_A8R8G8B8: case HLE_XFMT_X8R8G8B8:
    case HLE_XFMT_LIN_A8R8G8B8: case HLE_XFMT_LIN_X8R8G8B8:
        return ord;
    default:
        return 0;
    }
}

static void state(void)
{
    /* nothing it is built from changed since the last draw's (most change
     * only their textures, arrays, transforms and constants): that state.
     * hle_kelvin_state alone was ~7% of Blinx's decoder, every draw. */
    const uint32_t gen = hle_kelvin_state_gen();
    if (g_pd.st_sent && gen == g_pd.st_gen && g_pd.st.xs == MAX(g_pd.aa_x, 1u) &&
        g_pd.st.ys == MAX(g_pd.aa_y, 1u)) {
        return;
    }
    HleCmdState st;
    memset(&st, 0, sizeof(st));
    st.xs = MAX(g_pd.aa_x, 1u);
    st.ys = MAX(g_pd.aa_y, 1u);
    st.aa = st.xs > 1 || st.ys > 1;         /* the NV2A scales screen positions */
    hle_kelvin_state(&st);
    g_pd.st_gen = gen;
    /* what changed only: Blinx flips its cull face ~3500 times a frame,
     * and the whole state went each time (1.3 KB, ~5 MB a frame) */
    g_phase = 2;
    hle_fe_state(&st, &g_pd.st, &g_pd.st_sent);
    g_phase = 0;
}

/* debug: a new value of debug.hakux.pbdraws=<n> logs the next n draws: the
 * target, each stage's texture registers and object, the vertex source, the
 * skinning mode and the first two attributes' formats (position, weights) */
static void drawlog(const HleCmdDraw *d, bool ff)
{
#ifdef __ANDROID__
    static HleProp prop;
    static unsigned polls;
    char v[PROP_VALUE_MAX];
    if ((polls++ & 255) == 0 && hle_prop_new(&prop, "debug.hakux.pbdraws", v)) {
        g_dlog_left = atoi(v);
    }
#endif
    if (g_dlog_left <= 0) {
        return;
    }
    g_dlog_left--;
    char tx[768];
    int o = 0;
    for (unsigned s = 0; s < 4; s++) {
        uint32_t b = 64 * s;
        if (!R(NV097_SET_TEXTURE_OFFSET + b) && !R(NV097_SET_TEXTURE_FORMAT + b)) {
            continue;
        }
        o += snprintf(tx + o, sizeof(tx) - (size_t)o, " t%u=%u%s[%08x f%08x c0 %08x c1 %08x r %08x"
                      " a %08x b %08x]", s,
                      d->tex[s], (d->tex_unset >> s) & 1 ? "(off)" : "", R(NV097_SET_TEXTURE_OFFSET + b),
                      R(NV097_SET_TEXTURE_FORMAT + b), R(NV097_SET_TEXTURE_CONTROL0 + b),
                      R(NV097_SET_TEXTURE_CONTROL1 + b), R(NV097_SET_TEXTURE_IMAGE_RECT + b),
                      R(0x1B08 + b), R(0x1B24 + b));
    }
    hle_log("hakuX-hle-pb", "draw %llu: %s prim %u count %u rt %u ds %u nbind %u stride %u surf "
            "[f%08x c%08x z%08x p%08x clip %08x %08x mask %08x] f0 %08x %08x %08x %08x n %08x at %x/%x/%x bl %x/%08x "
            "z %x/%x/%x po %x %08x %08x skin %x va01 %08x %08x zclip %08x %08x zmm %x%s",
            (unsigned long long)g_pd.draws, ff ? "ff" : "prog", d->prim, d->count, g_pd.rt, g_pd.ds,
            d->nbind, d->bind_stride[0], R(NV097_SET_SURFACE_FORMAT), R(NV097_SET_SURFACE_COLOR_OFFSET),
            R(NV097_SET_SURFACE_ZETA_OFFSET), R(NV097_SET_SURFACE_PITCH),
            R(NV097_SET_SURFACE_CLIP_HORIZONTAL), R(NV097_SET_SURFACE_CLIP_VERTICAL), R(0x0358),
            R(0x0A60), R(0x0A64), R(0x0A68), R(0x0A6C), R(0x1E60), R(0x0300), R(0x033C), R(0x0340),
            R(0x0304), R(0x034C), R(0x030C), R(0x035C), R(0x0354), R(0x0338), R(0x0384), R(0x0388), R(0x0328),
            R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT), R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4),
            R(0x0394), R(0x0398), R(0x1D78), tx);
}

/* A submit boundary: what was recorded goes to the GPU. */
static void kick(void)
{
    g_pd.epoch++;
    g_pd.vis_unsent = false;
    HleCmdFence *f = begin(HLE_OP_KICK, sizeof(*f));
    f->value = hle_fe_next_value();
    hle_fe_commit();
}

/* ---- visibility tests ----
 * D3D's Begin pushes SET_ZPASS_PIXEL_COUNT_ENABLE 1, its End 0 and then
 * GET_REPORT: the back end counts the draws between in occlusion queries
 * and writes the report (time stamp, pixels, status 0) once the GPU has
 * run them. A report written at once with no pixels hid every object
 * Fable tests for occlusion. */
/* debug (A/B of the GPU's cost): HAKUX_HLE_PB_NOVIS=1, no occlusion queries:
 * every visibility test reports all its pixels passed (pbmode.c) */
bool hle_pbd_novis(void)
{
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("HAKUX_HLE_PB_NOVIS");
        off = e && !strcmp(e, "1");
    }
    return off;
}

/* debug: HAKUX_HLE_PB_KICK_MS, the periodic submit boundary's interval
 * (default 4; 0: none, a frame's swap submits) */
static int64_t kick_interval_ns(void)
{
    static int64_t ns = -1;
    if (ns < 0) {
        const char *e = getenv("HAKUX_HLE_PB_KICK_MS");
        ns = e ? (int64_t)atoi(e) * 1000000 : 4000000;
    }
    return ns;
}

static void vis_begin(void)
{
    if (!g_pd.device || g_pd.vis_open || hle_pbd_novis()) {
        return;
    }
    sync_rt();
    g_pd.vis_open = true;
    begin(HLE_OP_VIS_BEGIN, 0);
    hle_fe_commit();
}

bool hle_pbd_vis_report(uint32_t pa)
{
    if (!g_pd.on || !g_pd.device || hle_pbd_novis()) {
        return false;
    }
    g_pd.vis_open = false;
    HleCmdVis *c = begin(HLE_OP_VIS_END, sizeof(*c));
    c->index = HLE_VIS_NO_INDEX;
    c->div = MAX(g_pd.aa_x, 1u) * MAX(g_pd.aa_y, 1u);
    c->report = pa;
    c->pad = 0;
    c->expect = hle_fe_last_value() + 1;
    hle_fe_commit();
    g_pd.vis_unsent = true;
    g_pd.vis_tests++;
    return true;
}

/* ---- memory kept across a quick reboot ----
 * D3D's PersistDisplay copies the frame on screen into a contiguous surface
 * (CopyRects: a blit, into an image of that memory here) and persists it for
 * the next title, which the core boots afresh with the persisted memory
 * restored (native/kernel/mm.c). Before that the images over the memory go
 * back into it, the last writer last: Splinter Cell Chaos Theory draws its
 * mission's loading screen over the menu's last frame; restored from memory
 * the GPU never wrote, it was black around the loading video. Requests come
 * from the guest's thread, the decode thread serves them when it is idle. */
#define KEEP_MAX 8
static struct {
    uint32_t pa[KEEP_MAX], len[KEEP_MAX];
    uint32_t req, done;                 /* requests made, served (atomics) */
    uint64_t value;                     /* the submission the served readbacks complete with */
} g_keep;

void hle_pbd_keep(uint32_t pa, uint32_t len)
{
    const uint32_t r = __atomic_load_n(&g_keep.req, __ATOMIC_RELAXED);
    g_keep.pa[r % KEEP_MAX] = pa;
    g_keep.len[r % KEEP_MAX] = len;
    __atomic_store_n(&g_keep.req, r + 1, __ATOMIC_RELEASE);
}

/* The colour images over [pa, end) a draw, clear or copy wrote, back into
 * guest memory, in the order they were written: a target the coherence
 * keeps by its landing (coh_land, none when its memory is the newer), the
 * other linear ones' rows with the submission that goes next (*lv: the
 * latest value, a kick sends them). gpu_newer (a savestate's, over all of RAM): only
 * those whose memory the CPU hasn't written since the GPU did; the decoder
 * keeps objects after a title stops drawing into their memory, and one
 * written back over the data the title keeps there now corrupted it (PGR2
 * read a NULL pointer right after a save). Returns how many. */
static unsigned writeback_range(uint32_t pa, uint32_t end, bool gpu_newer, const char *what, uint64_t *lv)
{
    unsigned n = 0, kept = 0;
    uint64_t after = 0;
    for (;;) {
        PbObj *next = NULL;
        uint32_t len = 0;
        for (unsigned i = 0; i < g_pd.nobjs; i++) {
            PbObj *o = &g_pd.objs[i];
            const HleFmtInfo *fi = hle_fmt_info(o->d.xfmt);
            uint32_t ol;
            if (!coh_managed(o, &ol)) {
                ol = !o->cube && fi && !(fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT)) ? o->d.pitch * o->d.h : 0;
            }
            if (ol && o->written > after && (!next || o->written < next->written) && o->d.pa < end &&
                o->d.pa + ol > pa) {
                next = o;
                len = ol;
            }
        }
        if (!next) {
            break;
        }
        after = next->written;
        if (obj_superseded(next)) {
            /* the title's data is there now (a texture defined from it), over
             * a target the coherence keeps too: its image would be landed */
            if (gpu_newer) {
                hle_log("hakuX-hle-pb", "%s: surface %u (%08x %ux%u) kept: textures took its memory since "
                        "its last draw", what, next->id, next->d.pa, next->d.w, next->d.h);
            }
            kept++;
            continue;
        }
        if (coh_managed(next, &len)) {
            const uint64_t l = coh_land(next);
            *lv = MAX(*lv, l);
            n += l != 0;
            continue;
        }
        if (gpu_newer && obj_written(next)) {
            /* memory is newer: it stays, and the check's clear is undone
             * (rt_cpu_sync uploads it before the GPU next uses the image) */
            hle_mem_mark_written(next->d.pa, len);
            kept++;
            continue;
        }
        const uint32_t fb = hle_res_rb_fb(len);
        if (!fb) {
            coh_pend_wait_inplace(next->d.pa, len);
        }
        HleCmdReadback *rb = begin(HLE_OP_READBACK, sizeof(*rb));
        memset(rb, 0, sizeof(*rb));
        const uint64_t v = hle_fe_last_value() + 1;     /* the submission it goes with */
        rb->surf = next->id;
        rb->pa = next->d.pa;
        rb->pitch = next->d.pitch;
        rb->arena_off = HLE_RB_DIRECT;
        hle_res_rb_land(rb, fb, len, v);
        hle_barrier_arm_landing(next->d.pa, len, v);
        hle_fe_commit();
        wb_note(next->d.pa, len, v);
        *lv = MAX(*lv, v);
        if (!gpu_newer) {
            hle_log("hakuX-hle-pb", "%s %08x+%u: surface %u (%08x %ux%u) written back", what, pa, end - pa,
                    next->id, next->d.pa, next->d.w, next->d.h);
        }
        n++;
    }
    if (gpu_newer) {
        hle_log("hakuX-hle-pb", "%s: %u surfaces written back, %u whose memory is newer kept", what, n, kept);
    }
    return n;
}

static void keep_serve(void)
{
    const uint32_t r = __atomic_load_n(&g_keep.req, __ATOMIC_ACQUIRE);
    if (r == g_keep.done) {
        return;
    }
    unsigned n = 0;
    uint64_t lv = 0;                                /* what they land with (the kick below sends it) */
    for (uint32_t q = g_keep.done; g_pd.on && g_pd.device && q != r; q++) {
        const uint32_t pa = g_keep.pa[q % KEEP_MAX], end = pa + g_keep.len[q % KEEP_MAX];
        n += writeback_range(pa, end, false, "kept memory", &lv);
    }
    if (n) {
        kick();
    }
    g_keep.value = lv;
    __atomic_store_n(&g_keep.done, r, __ATOMIC_RELEASE);
}

bool hle_pbd_keep_wait(uint64_t timeout_ns)
{
    const uint32_t r = __atomic_load_n(&g_keep.req, __ATOMIC_ACQUIRE);
    const int64_t t0 = hle_now_ns();
    while (__atomic_load_n(&g_keep.done, __ATOMIC_ACQUIRE) != r) {
        if ((uint64_t)(hle_now_ns() - t0) > timeout_ns) {
            hle_log("hakuX-hle-pb", "kept memory: the decoder did not get to it in %llu ms",
                    (unsigned long long)(timeout_ns / 1000000));
            return false;
        }
        hle_pb_kick();                  /* the decoder serves it when it is idle */
        g_usleep(1000);
    }
    if (g_keep.value) {
        hle_tl_wait(g_keep.value);
    }
    return true;
}

/* front_stale's copy: the guest's thread asks at its first write, the
 * decode thread copies the images over the range into it at its next pass,
 * wherever it is in the push buffer (stalled on a flip, it waits for the
 * guest); the write waits for the copy (hle_mem_first_write) */
static struct {
    uint32_t pa, len;
    uint32_t req, done;                 /* requests made, served (atomics) */
} g_front;
static __thread bool t_decoder;         /* the decode thread (hle_pbd_front_serve) */

static void front_fill(uint32_t pa, uint32_t len)
{
    if (t_decoder) {
        return;                         /* it can't wait for itself */
    }
    g_front.pa = pa;
    g_front.len = len;
    const uint32_t r = __atomic_add_fetch(&g_front.req, 1, __ATOMIC_ACQ_REL);
    const int64_t t0 = hle_now_ns();
    while (__atomic_load_n(&g_front.done, __ATOMIC_ACQUIRE) != r) {
        if (hle_now_ns() - t0 > 100000000) {
            hle_log("hakuX-hle-pb", "front buffer %08x: the decoder did not copy the frame in 100 ms "
                    "(the CPU writes over older memory)", pa);
            return;
        }
        hle_pb_kick();
        g_usleep(200);
    }
}



void hle_pbd_front_serve(void)
{
    t_decoder = true;
    coh_serve();
    const uint32_t r = __atomic_load_n(&g_front.req, __ATOMIC_ACQUIRE);
    if (r == g_front.done) {
        return;
    }
    if (rtlog_on()) {
        hle_log("hakuX-hle-pb", "front buffer %08x+%u: the CPU's first write, shown %u", g_front.pa,
                g_front.len, g_pd.shown_id);
    }
    if (g_pd.on && g_pd.device) {
        /* not over memory the CPU wrote since (the range reused for data) */
        uint64_t lv = 0;                                /* (the guest's write waits for these) */
        if (writeback_range(g_front.pa, g_front.pa + g_front.len, true, "front buffer", &lv)) {
            kick();
        }
    }
    __atomic_store_n(&g_front.done, r, __ATOMIC_RELEASE);
}

void hle_pbd_idle(void)
{
    /* reports ended with nothing after them would wait for the next frame's
     * submission: a title that waits on one (its fence passed) waits as long */
    if (g_pd.on && g_pd.vis_unsent) {
        g_pd.vis_kicks++;
        kick();
    }
    g_pd.rt_fresh = true;                   /* the CPU may write the target now (rt_first_write) */
    hle_pbd_fence();                        /* the guest may read the targets now */
    keep_serve();
}

void hle_pbd_vis_counts(uint64_t *tests, uint64_t *kicks)
{
    *tests = g_pd.vis_tests;
    *kicks = g_pd.vis_kicks;
}

/* The rest of a draw whose vertices and indices d holds: the stages'
 * textures, the constants (a program's, or the fixed-function stage's from
 * its methods) and the attributes' current values after them, the state
 * and the vertex shader; emitted. */
static void emit(HleCmdDraw *d)
{
    const bool ff = (R(NV097_SET_TRANSFORM_EXECUTION_MODE) & 3) != 2;
    uint8_t *cb;
    uint32_t c_off;
    /* a draw whose constants and current attributes are the last one's, in
     * the same submission (its block lives until that completes): that
     * block. Crimson Sea's 2300 draws a frame copied 3.3 KB each; a
     * fixed-function draw's block (ff_consts) holds while none of the
     * registers it is made of changed (ffgen). */
    if (g_pd.blk_ok && g_pd.blk_ff == ff && g_pd.blk_gen == g_pd.cgen &&
        (!ff || g_pd.blk_ffgen == g_pd.ffgen) && g_pd.blk_value == hle_fe_last_value()) {
        c_off = g_pd.blk_off;
        g_pd.blk_reused++;
    } else {
        c_off = arena(sizeof(g_pd.consts) + sizeof(g_pd.cur), &cb);
        if (c_off == HLE_ARENA_FULL) {
            skip(SK_EMIT_ARENA);
            return;
        }
        if (ff) {
            ff_consts((float (*)[4])cb);
        } else {
            memcpy(cb, g_pd.consts, sizeof(g_pd.consts));
        }
        memcpy(cb + sizeof(g_pd.consts), g_pd.cur, sizeof(g_pd.cur));
        g_pd.blk_ok = true;
        g_pd.blk_ff = ff;
        g_pd.blk_ffgen = g_pd.ffgen;
        g_pd.blk_off = c_off;
        g_pd.blk_gen = g_pd.cgen;
        g_pd.blk_value = hle_fe_last_value();   /* after arena(): it may have kicked */
    }
    rt_first_write(false);
    if (g_pd.rt_obj) {
        g_pd.rt_obj->written = ++g_pd.wseq;
        coh_drawn(g_pd.rt_obj);
    }
    if (g_pd.ds && ((R(0x030C) && R(0x035C)) || (R(0x032C) && (R(0x0360) & 0xFF)))) {
        ds_written();                   /* depth test and writes, or stencil writes */
    }
    for (unsigned s = 0; s < 4; s++) {
        bool unset, lin;
        d->tex[s] = texture(s, &unset, &lin);
        d->tex_unset |= (uint32_t)unset << s;
        d->tex_lin |= (uint32_t)lin << s;
        /* per draw: the state cache isn't rebuilt when only a texture changes */
        d->tex_rect[s] = lin ? R(NV097_SET_TEXTURE_IMAGE_RECT + 64 * s) : 0;
        d->tex_ord |= tex_order(s, d->tex[s]) << 2 * s;
    }
    state();
    uint32_t zf = (R(NV097_SET_SURFACE_FORMAT) >> 4) & 0xF;
    d->vsh = ff ? ff_program() : program();
    d->consts_off = c_off;
    /* the integer formats' 2^-n, the shaders adding the rest (HLE_ZNORM_GLSL);
     * a float format's own (zfloat_scale) */
    d->zscale = (R(NV097_SET_CONTROL0) & 0x1000u) ? zfloat_scale(zf == 2) : zf == 1 ? 1.0f / 65536.0f
                                                                           : 1.0f / 16777216.0f;
    d->ff_ubo = HLE_DRAW_NO_UBO;
    drawlog(d, ff);
    HleCmdDraw *c = begin(HLE_OP_DRAW, sizeof(*c));
    *c = *d;
    hle_fe_commit();
    g_pd.draws++;
    g_pd.ff += ff;
    /* a submit boundary every 4 ms of draws (checked every 256): a title
     * that flips rarely, or not through FLIP_INCREMENT_WRITE, would grow one
     * submission without end (Riddick's lost the device). A visibility test
     * ended since the last one goes at the next check: its report sooner,
     * the title that reads it a frame later waits less for it (the report
     * gate, mem/barrier.h hle_barrier_report_pushed; Blinx) */
    if (++g_pd.kick_draws >= 256) {
        g_pd.kick_draws = 0;
        int64_t now = hle_now_ns();
        const int64_t every = kick_interval_ns();
        if (every && (now - g_pd.kick_ns >= every || g_pd.vis_unsent)) {
            g_pd.kick_ns = now;
            kick();
        }
    }
}

/* Indices 0 .. n - 1 converted for the primitive into the arena (d's
 * count and index buffer); false without room. */
static bool sequential(HleCmdDraw *d, uint32_t n)
{
    uint32_t cnt = hle_draw_prim_count(g_pd.prim, n);
    uint8_t *ib;
    uint32_t ib_off = cnt ? arena_idx(2 * cnt + 4, &ib) : HLE_ARENA_FULL;
    if (ib_off == HLE_ARENA_FULL) {
        return false;
    }
    hle_draw_prim_indices(g_pd.prim, NULL, n, 0, (uint16_t *)ib);
    d->prim = hle_draw_prim_topology(g_pd.prim);
    d->count = cnt;
    d->ib_off = ib_off;
    return true;
}

/* Immediate vertices: one binding, the 16 attributes as float4s. */
static void draw_imm(void)
{
    HleCmdDraw d;
    memset(&d, 0, sizeof(d));
    uint32_t n = g_pd.nimm, len = n * ATTRS * 16;
    uint8_t *dst;
    uint32_t off = arena(len, &dst);
    if (off == HLE_ARENA_FULL || !sequential(&d, n)) {
        skip(SK_DRAW_IMM_ARENA);
        return;
    }
    memcpy(dst, g_pd.imm, len);
    d.nbind = 1;
    d.bind_off[0] = off;
    d.bind_stride[0] = ATTRS * 16;
    for (unsigned a = 0; a < ATTRS; a++) {
        d.attr[a] = (4u << 4 | 2u) | (16u * a) << 16;       /* F, 4 components */
    }
    emit(&d);
}

/* An inline array: vertices of the enabled attributes' formats, packed in
 * attribute order (the arrays' offsets and strides aside). */
static void draw_inline(void)
{
    HleCmdDraw d;
    memset(&d, 0, sizeof(d));
    uint32_t vsize = 0;
    for (unsigned a = 0; a < ATTRS; a++) {
        uint32_t fmt = R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a);
        if ((fmt >> 4) & 0xF) {
            d.attr[a] = (fmt & 0xFF) | vsize << 16;
            vsize += (attr_bytes(fmt) + 3) & ~3u;
        }
    }
    uint32_t len = g_pd.ninl * 4, n = vsize ? len / vsize : 0;
    uint8_t *dst;
    uint32_t off = n && n <= MAX_IMM ? arena(n * vsize, &dst) : HLE_ARENA_FULL;
    if (off == HLE_ARENA_FULL || !sequential(&d, n)) {
        skip(SK_DRAW_INLINE_ARENA);
        return;
    }
    memcpy(dst, g_pd.inl, n * vsize);
    d.nbind = 1;
    d.bind_off[0] = off;
    d.bind_stride[0] = vsize;
    emit(&d);
}

/* Vertex arrays read in place: the GPU fetches them from guest RAM (the
 * imported RAM buffer, offset = PA) as the NV2A does, and the first CPU
 * write to their pages waits for the draws (the barrier); a range whose
 * pages made a write wait (the CPU rewrites them while the GPU reads) is
 * copied to the arena instead. Crimson Sea copied the same meshes about 2000
 * times a frame, a fifth of its decoder. HAKUX_HLE_VB_COPY=1 keeps the
 * copies (A/B), as in D3D mode. */
static bool vb_in_place(void)
{
    static int on = -1;
    if (on < 0) {
        on = g_hram.zero_copy && !getenv("HAKUX_HLE_VB_COPY");
    }
    return on;
}

/* The decode thread, between methods: a guest store waits for `value` (the
 * barrier): the submission still open is closed and goes out at once. */
void hle_pbd_boundary(uint64_t value)
{
    t_decoder = true;
    coh_serve();                            /* a guest thread waits for a landing */
    if (g_pd.on && hle_fe_last_value() < value) {
        g_pd.bnd_kicks++;
        kick();
    }
    hle_back_urgent();
}

/* debug: HAKUX_HLE_PB_VBRACE=1 (via debug.hakux.env), a vertex range copied
 * again in the same frame with other bytes than its last copy is logged
 * (hakuX-hle-pb vbrace): the CPU rewrote vertices between two draws. posoff
 * is the position's offset in a vertex (-1: none), hashed apart: a changed
 * position moves the draw's depth. */
static void vbrace(uint32_t from, uint32_t len, uint32_t stride, int posoff, const uint8_t *copy)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("HAKUX_HLE_PB_VBRACE");
        on = e && atoi(e) > 0;
    }
    if (!on) {
        return;
    }
    static struct {
        uint32_t from, len;
        uint64_t swap, draw, h, hp;
        int64_t ns;
        uint32_t get, put;
    } t[256];
    static unsigned logged;
    const int64_t now = hle_now_ns();
    uint32_t get, put;
    hle_pb_debug_pos(&get, &put);
    uint64_t h = fnv64(copy, len), hp = 0;
    for (uint32_t o = (uint32_t)posoff; posoff >= 0 && stride && o + 12 <= len; o += stride) {
        hp = (hp ^ fnv64(copy + o, 12)) * 0x100000001b3ull;
    }
    unsigned i = (from ^ (from >> 12) ^ len * 31u) & 255u;
    if (t[i].from == from && t[i].len == len && t[i].swap == g_pd.swaps && t[i].h != h &&
        (++logged <= 16 || (logged & 255) == 0)) {
        hle_log("hakuX-hle-pb", "vbrace: %08x+%u stride %u copied for draw %llu with other %s "
                "than for draw %llu, %.3f ms before (swap %llu; GET/PUT then %08x/%08x, now %08x/%08x)",
                from, len, stride,
                (unsigned long long)g_pd.draws, t[i].hp != hp ? "positions" : "bytes (positions same)",
                (unsigned long long)t[i].draw, (double)(now - t[i].ns) / 1e6,
                (unsigned long long)g_pd.swaps, t[i].get, t[i].put, get, put);
    }
    t[i].ns = now;
    t[i].get = get;
    t[i].put = put;
    t[i].from = from;
    t[i].len = len;
    t[i].swap = g_pd.swaps;
    t[i].draw = g_pd.draws;
    t[i].h = h;
    t[i].hp = hp;
}

/* Vertex ranges copied again in one frame: a second pass over the same
 * vertices, which the guest may rewrite for its next frame before the
 * decoder reaches the second. From then on each copy of them arms the
 * catch-up first (barrier.h hle_barrier_arm_copied): a store after it waits
 * until the decoder has read the draws pushed before the store. */
static bool recopied(uint32_t from, uint32_t len)
{
    static struct {
        uint32_t from, len;
        uint64_t swap;
        bool again;
    } t[1024];
    const unsigned i = (from ^ (from >> 12) ^ len * 31u) & 1023u;
    if (t[i].from != from || t[i].len != len) {
        t[i].from = from;
        t[i].len = len;
        t[i].again = false;
    } else if (t[i].swap == g_pd.swaps) {
        t[i].again = true;
    }
    t[i].swap = g_pd.swaps;
    return t[i].again;
}

/* The bytes of an attribute's component as the back end fetches it, which
 * its address must be a multiple of (vk/draw.c vat_format: CMP as 16-bit
 * halves). */
static uint32_t attr_comp(uint32_t fmt)
{
    switch (fmt & 0xF) {
    case 0: case 4: return 1;               /* UB_D3D, UB_OGL */
    case 1: case 5: case 6: return 2;       /* S1, S32K, CMP */
    default: return 4;                      /* F */
    }
}

/* HLE_QUIRK_VB_ALIGN: an attribute of binding b (its vertex 0 at base)
 * whose address its components don't divide, at some vertex. */
static bool vb_misaligned(const HleCmdDraw *d, unsigned b, uint32_t base)
{
    for (unsigned a = 0; a < HLE_DRAW_ATTRS; a++) {
        const uint32_t x = d->attr[a], c = attr_comp(x & 0xFF);
        if (((x >> 4) & 0xF) && ((x >> 8) & 0xFF) == b &&
            ((base + (x >> 16)) % c || d->bind_stride[b] % c)) {
            return true;
        }
    }
    return false;
}

/* HLE_QUIRK_VB_ALIGN: binding b's n vertices (src, len bytes from from)
 * copied to the arena with each attribute at an offset its components
 * divide, in the order they had, and a stride 4 divides; the draw's
 * attributes and binding say where. False: no room. */
static bool vb_realign(HleCmdDraw *d, unsigned b, const uint8_t *src, uint32_t from, uint32_t len,
                       uint32_t n)
{
    struct { unsigned a; uint32_t off, size, to; } at[HLE_DRAW_ATTRS];
    unsigned na = 0;
    for (unsigned a = 0; a < HLE_DRAW_ATTRS; a++) {
        const uint32_t x = d->attr[a];
        if (((x >> 4) & 0xF) && ((x >> 8) & 0xFF) == b) {
            unsigned k = na++;
            while (k && at[k - 1].off > (x >> 16)) {
                at[k] = at[k - 1];
                k--;
            }
            at[k].a = a;
            at[k].off = x >> 16;
            at[k].size = attr_bytes(x & 0xFF);
        }
    }
    uint32_t end = 0;
    for (unsigned i = 0; i < na; i++) {
        at[i].to = ROUND_UP(end, attr_comp(d->attr[at[i].a] & 0xFF));
        end = at[i].to + at[i].size;
    }
    const uint32_t st = d->bind_stride[b], ns = st ? ROUND_UP(end, 4u) : 0;
    uint8_t *dst;
    const uint32_t off = arena(st ? n * ns : end, &dst);
    if (off == HLE_ARENA_FULL) {
        return false;
    }
    if (recopied(from, len)) {
        hle_barrier_arm_copied(from, len);
    }
    for (uint32_t v = 0; v < (st ? n : 1); v++) {
        for (unsigned i = 0; i < na; i++) {
            memcpy(dst + v * ns + at[i].to, src + v * st + at[i].off, at[i].size);
        }
    }
    static bool said;
    if (!said) {
        said = true;
        hle_log("hakuX-hle-pb", "vb_align: a binding of stride %u at %08x copied with its attributes aligned "
                "(stride %u)", st, from, ns);
    }
    for (unsigned i = 0; i < na; i++) {
        d->attr[at[i].a] = (d->attr[at[i].a] & 0xFFFFu) | at[i].to << 16;
    }
    d->bind_stride[b] = ns;
    d->bind_off[b] = off;
    g_pd.vb_copied++;
    return true;
}

/* The vertex arrays the indices reach, read in place or copied to the
 * arena, the indices converted for the primitive and rebased. */
static void draw_arrays(void)
{
    uint32_t n = g_pd.nidx;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        lo = MIN(lo, g_pd.idx[i]);
        hi = MAX(hi, g_pd.idx[i]);
    }
    if (hi - lo >= 0xFFFF) {
        skip(SK_DRAW_ARRAYS_SPAN);
        return;
    }
    HleCmdDraw d;
    memset(&d, 0, sizeof(d));
    /* bindings: attributes of one stride whose data lie within one stride
     * of a binding's start share it (interleaved vertices) */
    uint32_t base[HLE_DRAW_MAX_BIND], end[HLE_DRAW_MAX_BIND];
    uint32_t order[HLE_DRAW_ATTRS], nord = 0;
    for (unsigned a = 0; a < HLE_DRAW_ATTRS; a++) {
        if ((R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a) >> 4) & 0xF) {
            uint32_t pa = R(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a) & 0x7FFFFFFFu;
            unsigned k = nord++;
            while (k && (R(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * order[k - 1]) & 0x7FFFFFFFu) > pa) {
                order[k] = order[k - 1];
                k--;
            }
            order[k] = a;
        }
    }
    for (unsigned j = 0; j < nord; j++) {
        unsigned a = order[j];
        uint32_t fmt = R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * a), stride = fmt >> 8;
        uint32_t pa = R(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * a) & 0x7FFFFFFFu;
        unsigned b = 0;
        while (b < d.nbind && !(stride && d.bind_stride[b] == stride && pa - base[b] < stride)) {
            b++;
        }
        if (b == d.nbind) {
            if (d.nbind == HLE_DRAW_MAX_BIND) {
                continue;
            }
            d.nbind++;
            base[b] = pa;
            end[b] = 0;
            d.bind_stride[b] = stride;
        }
        end[b] = MAX(end[b], pa - base[b] + attr_bytes(fmt));
        d.attr[a] = (fmt & 0xFF) | b << 8 | (pa - base[b]) << 16;
    }
    for (unsigned b = 0; b < d.nbind; b++) {
        uint32_t st = d.bind_stride[b];
        uint32_t from = base[b] + (st ? lo * st : 0);
        uint32_t len = st ? (hi - lo) * st + MAX(end[b], 1u) : end[b];
        coh_pend_wait(from, len);           /* a copy queued into it lands first */
        const uint8_t *src = hle_pa_ptr(from, len);
        if (src && hle_quirk(HLE_QUIRK_VB_ALIGN) && vb_misaligned(&d, b, base[b])) {
            if (!vb_realign(&d, b, src, from, len, st ? hi - lo + 1 : 1)) {
                skip(SK_DRAW_ARRAYS_ARENA);
                return;
            }
            continue;
        }
        if (src && vb_in_place() && !hle_barrier_contended(from, len)) {
            hle_barrier_arm(from, len, hle_fe_last_value() + 1);
            d.guest_vb |= 1u << b;
            d.bind_off[b] = from;
            g_pd.vb_place++;
            continue;
        }
        uint8_t *dst;
        uint32_t off = src ? arena(len, &dst) : HLE_ARENA_FULL;
        if (off == HLE_ARENA_FULL) {
            skip(SK_DRAW_ARRAYS_ARENA);
            return;
        }
        if (recopied(from, len)) {
            hle_barrier_arm_copied(from, len);
        }
        memcpy(dst, src, len);
        d.bind_off[b] = off;
        g_pd.vb_copied++;
        bool pos = ((R(NV097_SET_VERTEX_DATA_ARRAY_FORMAT) >> 4) & 0xF) && ((d.attr[0] >> 8) & 0xFF) == b;
        vbrace(from, len, st, pos ? (int)(d.attr[0] >> 16) : -1, dst);
    }
    uint32_t pt = g_pd.prim;
    /* One run of vertices in order, of a primitive Vulkan draws as it is
     * (quads and line loops are converted): a draw without indices. The
     * back end joins such strips into one draw (vk/batch.c): Blinx draws its
     * snowy town as ~17k short strips a frame, which as indexed draws went to
     * the GPU one by one at ~1.7 us each, and the frame took the GPU 30 ms
     * at 2x (26 fps; 30 at 3x in D3D mode, whose strips were joined). */
    if (hi - lo + 1 == n && pt != NV2A_PRIM_QUADS && pt != NV2A_PRIM_LINE_LOOP) {
        bool seq = true;
        for (uint32_t i = 0; i < n && seq; i++) {
            seq = g_pd.idx[i] == lo + i;
        }
        if (seq) {
            d.prim = hle_draw_prim_topology(pt);
            d.count = n;
            d.ib_off = UINT32_MAX;
            g_pd.seq_draws++;
            emit(&d);
            return;
        }
    }
    /* indices, rebased (and quads and line loops converted) */
    uint32_t cnt = hle_draw_prim_count(pt, n);
    uint8_t *ib;
    uint32_t ib_off = cnt ? arena_idx(2 * cnt + 4, &ib) : HLE_ARENA_FULL;
    if (ib_off == HLE_ARENA_FULL) {
        skip(SK_DRAW_ARRAYS_ARENA2);
        return;
    }
    if (pt == NV2A_PRIM_QUADS || pt == NV2A_PRIM_LINE_LOOP) {
        for (uint32_t i = 0; i < n; i++) {
            g_pd.idx16[i] = (uint16_t)(g_pd.idx[i] - lo);
        }
        hle_draw_prim_indices(pt, g_pd.idx16, n, 0, (uint16_t *)ib);
    } else {
        uint16_t *o = (uint16_t *)ib;
        for (uint32_t i = 0; i < n; i++) {
            o[i] = (uint16_t)(g_pd.idx[i] - lo);
        }
    }
    d.prim = hle_draw_prim_topology(pt);
    d.count = cnt;
    d.ib_off = ib_off;
    emit(&d);
}

/* The draw gathered since SET_BEGIN_END. */
static void draw(void)
{
    sync_rt();
    if (!g_pd.rt && !g_pd.ds) {
        skip(SK_DRAW_NO_TARGET);
        if (g_dlog_left > 0) {
            hle_log("hakuX-hle-pb", "skipped draw: no target (surface format %08x colour %08x zeta "
                    "%08x pitch %08x clip %08x %08x)", R(NV097_SET_SURFACE_FORMAT),
                    R(NV097_SET_SURFACE_COLOR_OFFSET), R(NV097_SET_SURFACE_ZETA_OFFSET),
                    R(NV097_SET_SURFACE_PITCH), R(NV097_SET_SURFACE_CLIP_HORIZONTAL),
                    R(NV097_SET_SURFACE_CLIP_VERTICAL));
        }
        return;
    }
    if (!g_pd.nidx && !g_pd.nimm && !g_pd.ninl) {
        skip(SK_DRAW_EMPTY);
        if (g_dlog_left > 0) {
            char t[160];
            int o = 0;
            for (unsigned i = 0; i < g_pd.ndm && o < (int)sizeof(t) - 12; i++) {
                o += snprintf(t + o, sizeof(t) - (size_t)o, " %04x:%04x", g_pd.dm[i] >> 16,
                              g_pd.dm[i] & 0xFFFF);
            }
            hle_log("hakuX-hle-pb", "skipped draw: no vertices (prim %u, methods%s)", g_pd.prim, t);
        }
        return;
    }
    rt_first_land(false);
    if (g_pd.nimm) {
        draw_imm();
    } else if (g_pd.ninl) {
        draw_inline();
    } else {
        draw_arrays();
    }
}

/* An attribute's current value (immediate data); the position's last word
 * emits a vertex in a draw. */
static void attr_set(unsigned a, const float x[4], bool emit_vertex)
{
    memcpy(g_pd.cur[a], x, 16);
    g_pd.cgen++;
    if (emit_vertex && g_pd.in_draw && g_pd.nimm < MAX_IMM) {
        memcpy(g_pd.imm + (size_t)g_pd.nimm * ATTRS * 4, g_pd.cur, sizeof(g_pd.cur));
        g_pd.nimm++;
    }
}

/* SET_VERTEX3F / 4F and SET_VERTEX_DATA*: true if m was one. */
static bool vertex_data(uint32_t m, uint32_t v)
{
    float x[4];
    unsigned a, comp, last;
    if (m >= NV097_SET_VERTEX3F && m < NV097_SET_VERTEX4F + 16) {
        bool four = m >= NV097_SET_VERTEX4F;
        a = 0;
        comp = (m - (four ? NV097_SET_VERTEX4F : NV097_SET_VERTEX3F)) / 4;
        last = four ? 3 : 2;
        memcpy(x, g_pd.cur[0], 16);
        memcpy(&x[comp], &v, 4);
        if (!four) {
            x[3] = 1.0f;
        }
    } else if (m >= NV097_SET_VERTEX_DATA2F_M && m < NV097_SET_VERTEX_DATA2S) {
        a = (m - NV097_SET_VERTEX_DATA2F_M) / 8;
        comp = ((m - NV097_SET_VERTEX_DATA2F_M) / 4) & 1;
        last = 1;
        memcpy(x, g_pd.cur[a], 16);
        memcpy(&x[comp], &v, 4);
        x[2] = 0.0f;
        x[3] = 1.0f;
    } else if (m >= NV097_SET_VERTEX_DATA2S && m < NV097_SET_VERTEX_DATA4UB) {
        a = (m - NV097_SET_VERTEX_DATA2S) / 4;
        comp = last = 0;
        x[0] = (float)(int16_t)(v & 0xFFFF);
        x[1] = (float)(int16_t)(v >> 16);
        x[2] = 0.0f;
        x[3] = 1.0f;
    } else if (m >= NV097_SET_VERTEX_DATA4UB && m < NV097_SET_VERTEX_DATA4S_M) {
        a = (m - NV097_SET_VERTEX_DATA4UB) / 4;
        comp = last = 0;
        for (unsigned c = 0; c < 4; c++) {
            x[c] = ((v >> (8 * c)) & 0xFF) / 255.0f;
        }
    } else if (m >= NV097_SET_VERTEX_DATA4S_M && m < NV097_SET_VERTEX_DATA4F_M) {
        a = (m - NV097_SET_VERTEX_DATA4S_M) / 8;
        comp = ((m - NV097_SET_VERTEX_DATA4S_M) / 4) & 1;
        last = 1;
        memcpy(x, g_pd.cur[a], 16);
        x[2 * comp] = (float)(int16_t)(v & 0xFFFF);
        x[2 * comp + 1] = (float)(int16_t)(v >> 16);
    } else if (m >= NV097_SET_VERTEX_DATA4F_M && m < NV097_SET_VERTEX_DATA4F_M + 16 * ATTRS) {
        a = (m - NV097_SET_VERTEX_DATA4F_M) / 16;
        comp = ((m - NV097_SET_VERTEX_DATA4F_M) / 4) & 3;
        last = 3;
        memcpy(x, g_pd.cur[a], 16);
        memcpy(&x[comp], &v, 4);
    } else {
        return false;
    }
    attr_set(a, x, a == 0 && comp == last);
    return true;
}

static void push_index(uint32_t i)
{
    if (g_pd.in_draw && g_pd.nidx < g_pd.cap) {
        g_pd.idx[g_pd.nidx++] = i;
    }
}

void hle_pbd_run(uint32_t m, bool noninc, const uint32_t *w, uint32_t n)
{
    if (!g_pd.on) {
        return;
    }
    if (noninc && m == NV097_ARRAY_ELEMENT16) {
        /* as push_index word by word, the bounds checked once for the run
         * (one call and two tests an index were ~4% of the decoder) */
        if (g_pd.in_draw) {
            const uint32_t k = MIN(n, (g_pd.cap - g_pd.nidx) / 2);
            uint32_t *d = g_pd.idx + g_pd.nidx;
            for (uint32_t i = 0; i < k; i++) {
                d[2 * i] = w[i] & 0xFFFF;
                d[2 * i + 1] = w[i] >> 16;
            }
            g_pd.nidx += 2 * k;
            if (k < n && g_pd.nidx < g_pd.cap) {
                g_pd.idx[g_pd.nidx++] = w[k] & 0xFFFF;  /* the one that still fits */
            }
        }
        g_pd.r[m >> 2] = w[n - 1];
        return;
    }
    if (noninc && m == NV097_INLINE_ARRAY) {
        if (g_pd.in_draw) {
            uint32_t k = MIN(n, MAX_INLINE - g_pd.ninl);
            memcpy(g_pd.inl + g_pd.ninl, w, 4 * k);
            g_pd.ninl += k;
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        hle_pbd_method(noninc ? m : m + 4 * i, w[i]);
    }
}

/* The fixed-function transform state the NV2A keeps in the programs' constant
 * memory (xemu's NV_IGRAPH_XF_XFCTX rows): composite matrix 0-3, projection
 * 4-7, model-view n 8 + 8n, its inverse 12 + 8n, texgen planes 0x40 + 8t,
 * texture matrix 0x44 + 8t, eye position 0x38, fog plane 0x39. A program
 * reads them there: Wreckless's world draws take their transform from the
 * model-view slots its SetTransform loads, and with them zero (only the
 * fixed-function state had them) the world was not drawn. The constant slot
 * of method m, or -1. */
static int xf_slot(uint32_t m, unsigned *comp)
{
    uint32_t base, row0;
    if (m >= 0x0440 && m < 0x0480) {            /* SET_PROJECTION_MATRIX */
        base = 0x0440, row0 = 0x04;
    } else if (m >= 0x0480 && m < 0x0680) {     /* SET_(INVERSE_)MODEL_VIEW_MATRIX */
        const uint32_t i = (m - 0x0480) >> 2, mat = (i >> 4) & 3, inv = i >> 6;
        *comp = i & 3;
        return (int)(0x08 + 4 * inv + 8 * mat + ((i >> 2) & 3));
    } else if (m >= 0x0680 && m < 0x06C0) {     /* SET_COMPOSITE_MATRIX */
        base = 0x0680, row0 = 0x00;
    } else if (m >= 0x06C0 && m < 0x07C0) {     /* SET_TEXTURE_MATRIX */
        const uint32_t i = (m - 0x06C0) >> 2;
        *comp = i & 3;
        return (int)(0x44 + 8 * (i >> 4) + ((i >> 2) & 3));
    } else if (m >= 0x0840 && m < 0x0940) {     /* SET_TEXGEN_PLANE_S/T/R/Q */
        const uint32_t i = (m - 0x0840) >> 2;
        *comp = i & 3;
        return (int)(0x40 + 8 * (i >> 4) + ((i >> 2) & 3));
    } else if (m >= 0x09D0 && m < 0x09E0) {     /* SET_FOG_PLANE */
        base = 0x09D0, row0 = 0x39;
    } else if (m >= 0x0A50 && m < 0x0A60) {     /* SET_EYE_POSITION */
        base = 0x0A50, row0 = 0x38;
    } else {
        return -1;
    }
    const uint32_t i = (m - base) >> 2;
    *comp = i & 3;
    return (int)(row0 + (i >> 2));
}

/* LAUNCH_TRANSFORM_PROGRAM: one run of the program at slot start, v0 the
 * last SET_TRANSFORM_DATA, its writes going to the constants (D3D's vertex
 * state shaders, RunVertexStateShader), on the CPU as xemu does. Wreckless
 * builds each object's transform so; ignored, its world drew nothing. */
static void launch_state_program(uint32_t start)
{
    static struct {
        Nv2aVshProgram prog;
        uint64_t hash;                  /* its slots' (the program memory changes as the title
                                           switches programs, the state programs stay) */
        bool parsed, ok;
    } cache[HLE_VSH_MAX_SLOTS];
    if (start >= HLE_VSH_MAX_SLOTS) {
        return;
    }
    uint32_t n = 0;
    while (start + n < HLE_VSH_MAX_SLOTS && !(g_pd.pmem[start + n++][3] & 1)) {
    }
    const uint64_t h = XXH3_64bits(g_pd.pmem[start], 16 * n);
    typeof(cache[0]) *c = &cache[start];
    if (!c->parsed || c->hash != h) {
        if (c->ok) {
            nv2a_vsh_program_destroy(&c->prog);
        }
        c->ok = nv2a_vsh_parse_program(&c->prog, g_pd.pmem[start], n) == NV2AVPR_SUCCESS;
        c->parsed = true;
        c->hash = h;
        if (!c->ok) {
            static unsigned logged;
            if (logged++ < 4) {
                hle_log("hakuX-hle-pb", "state program at %u: not a program it can run", start);
            }
            return;
        }
    }
    static float k[HLE_VSH_CONSTANTS * 4];
    memcpy(k, g_pd.consts, sizeof(k));
    Nv2aVshCPUXVSSExecutionState xs;
    Nv2aVshExecutionState st = nv2a_vsh_emu_initialize_xss_execution_state(&xs, k);
    memcpy(xs.input_regs, g_pd.xdata, sizeof(xs.input_regs));
    bool wrote[HLE_VSH_CONSTANTS] = { false };
    nv2a_vsh_emu_execute_track_context_writes(&st, &c->prog, wrote);
    for (unsigned i = 0; i < HLE_VSH_CONSTANTS; i++) {
        if (wrote[i] && memcmp(g_pd.consts[i], &k[4 * i], 16)) {
            memcpy(g_pd.consts[i], &k[4 * i], 16);
            g_pd.cgen++;
        }
    }
}

/* The methods hle_pbd_method does more for than keep (by method / 4): vertex
 * data, program and constants and their load points, the viewport, the
 * fixed-function transform state (xf_slot), state programs, the draws, the
 * targets and clears, the swap. The rest (most of a frame's state) is stored and done
 * with, past none of the ranges and switches below. */
static const bool k_pbd_acts[2048] = {
    [0x0440 / 4 ... 0x07BC / 4] = true,         /* xf_slot's matrices */
    [0x0840 / 4 ... 0x093C / 4] = true,
    [0x09D0 / 4 ... 0x09DC / 4] = true,
    [0x0A50 / 4 ... 0x0A5C / 4] = true,
    [NV097_SET_VERTEX3F / 4 ... (NV097_SET_VERTEX4F + 12) / 4] = true,
    [NV097_SET_VERTEX_DATA2F_M / 4 ... (NV097_SET_VERTEX_DATA4F_M + 16 * ATTRS - 4) / 4] = true,
    [NV097_SET_TRANSFORM_PROGRAM / 4 ... (NV097_SET_TRANSFORM_PROGRAM + 0x7C) / 4] = true,
    [NV097_SET_TRANSFORM_CONSTANT / 4 ... (NV097_SET_TRANSFORM_CONSTANT + 0x7C) / 4] = true,
    [NV097_SET_VIEWPORT_OFFSET / 4 ... (NV097_SET_VIEWPORT_OFFSET + 12) / 4] = true,
    [NV097_SET_VIEWPORT_SCALE / 4 ... (NV097_SET_VIEWPORT_SCALE + 12) / 4] = true,
    [NV097_SET_TRANSFORM_PROGRAM_LOAD / 4] = true,
    [NV097_SET_TRANSFORM_CONSTANT_LOAD / 4] = true,
    [NV097_SET_TRANSFORM_DATA / 4 ... NV097_LAUNCH_TRANSFORM_PROGRAM / 4] = true,
    [NV097_SET_ZPASS_PIXEL_COUNT_ENABLE / 4] = true,
    [NV097_SET_BEGIN_END / 4] = true,
    [NV097_ARRAY_ELEMENT16 / 4] = true,
    [NV097_ARRAY_ELEMENT32 / 4] = true,
    [NV097_DRAW_ARRAYS / 4] = true,
    [NV097_INLINE_ARRAY / 4] = true,
    [NV097_SET_WINDOW_CLIP_HORIZONTAL / 4 ... (NV097_SET_WINDOW_CLIP_HORIZONTAL + 0x3C) / 4] = true,
    [NV097_SET_SURFACE_CLIP_HORIZONTAL / 4 ... NV097_SET_SURFACE_ZETA_OFFSET / 4] = true,
    [NV097_SET_CONTROL0 / 4] = true,
    [NV097_SET_WINDOW_CLIP_TYPE / 4] = true,
    [NV097_CLEAR_SURFACE / 4] = true,
    [NV097_FLIP_INCREMENT_WRITE / 4] = true,
};

void hle_pbd_method(uint32_t m, uint32_t v)
{
    if (!g_pd.on || m >= 0x2000) {
        return;
    }
    if (g_pd.in_draw && g_pd.ndm < 12) {     /* debug: what an empty draw held */
        g_pd.dm[g_pd.ndm++] = m << 16 | (v & 0xFFFF);
    }
    if (g_pd.r[m >> 2] != v && (g_ff_regs[m >> 8] >> ((m >> 2) & 63) & 1)) {
        g_pd.ffgen++;                       /* (D3D sends matrices again unchanged) */
    }
    g_pd.r[m >> 2] = v;
    if (!k_pbd_acts[m >> 2]) {
        return;
    }
    if (vertex_data(m, v)) {
        return;
    }
    if (m >= NV097_SET_TRANSFORM_PROGRAM && m < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        unsigned comp = (m >> 2) & 3;
        if (g_pd.pload < HLE_VSH_MAX_SLOTS) {
            g_pd.pmem[g_pd.pload][comp] = v;
            g_pd.pgen++;
        }
        g_pd.pload += comp == 3;
        return;
    }
    if (m >= NV097_SET_TRANSFORM_CONSTANT && m < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        unsigned comp = (m >> 2) & 3;
        if (g_pd.cload < HLE_VSH_CONSTANTS) {
            g_pd.consts[g_pd.cload][comp] = v;
            g_pd.cgen++;
        }
        g_pd.cload += comp == 3;
        return;
    }
    if (m >= NV097_SET_VIEWPORT_OFFSET && m < NV097_SET_VIEWPORT_OFFSET + 16) {
        g_pd.consts[VPOFF][(m >> 2) & 3] = v;
        g_pd.cgen++;
        return;
    }
    if (m < 0x0A60) {
        unsigned comp;
        const int slot = xf_slot(m, &comp);
        if (slot >= 0) {
            if (g_pd.consts[slot][comp] != v) {
                g_pd.consts[slot][comp] = v;
                g_pd.cgen++;
            }
            return;
        }
    }
    if (m >= NV097_SET_VIEWPORT_SCALE && m < NV097_SET_VIEWPORT_SCALE + 16) {
        g_pd.consts[VPSCL][(m >> 2) & 3] = v;
        g_pd.cgen++;
        return;
    }
    switch (m) {
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        g_pd.pload = v;
        break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        g_pd.cload = v;
        break;
    case NV097_SET_TRANSFORM_DATA:
    case NV097_SET_TRANSFORM_DATA + 4:
    case NV097_SET_TRANSFORM_DATA + 8:
    case NV097_SET_TRANSFORM_DATA + 12:
        g_pd.xdata[(m >> 2) & 3] = v;
        break;
    case NV097_LAUNCH_TRANSFORM_PROGRAM:
        launch_state_program(v);
        break;
    case NV097_SET_ZPASS_PIXEL_COUNT_ENABLE:
        if (v & 1) {
            vis_begin();
        }
        break;
    case NV097_SET_BEGIN_END:
        if (v) {
            g_pd.in_draw = true;
            g_pd.prim = v;
            g_pd.nidx = g_pd.nimm = g_pd.ninl = 0;
            g_pd.ndm = 0;
        } else if (g_pd.in_draw) {
            g_pd.in_draw = false;
            draw();
        }
        break;
    case NV097_ARRAY_ELEMENT16:
        push_index(v & 0xFFFF);
        push_index(v >> 16);
        break;
    case NV097_ARRAY_ELEMENT32:
        push_index(v);
        break;
    case NV097_DRAW_ARRAYS: {
        uint32_t first = v & 0x00FFFFFFu, count = (v >> 24) + 1;
        for (uint32_t i = 0; i < count; i++) {
            push_index(first + i);
        }
        break;
    }
    case NV097_INLINE_ARRAY:
        if (g_pd.in_draw && g_pd.ninl < MAX_INLINE) {
            g_pd.inl[g_pd.ninl++] = v;
        }
        break;
    default:
        break;
    }
    if (m - NV097_SET_WINDOW_CLIP_HORIZONTAL < 0x40) {
        g_pd.rt_dirty = true;               /* the window clip's rectangles */
    }
    switch (m) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
    case NV097_SET_SURFACE_CLIP_VERTICAL:
    case NV097_SET_SURFACE_FORMAT:
    case NV097_SET_SURFACE_PITCH:
    case NV097_SET_SURFACE_COLOR_OFFSET:
    case NV097_SET_SURFACE_ZETA_OFFSET:
    case NV097_SET_CONTROL0:
        g_pd.rt_dirty = true;
        break;
    case NV097_SET_WINDOW_CLIP_TYPE:
        g_pd.rt_dirty = true;
        break;
    case NV097_CLEAR_SURFACE:
        clear(v);
        break;
    case NV097_FLIP_INCREMENT_WRITE:
        swap();
        break;
    default:
        break;
    }
}

/* ---- render targets' memory against their images (coh_*) ----
 * The NV2A draws into memory; here a colour target's draws land in its
 * image, and its memory keeps what it held. A target knows when the two
 * last matched (synced, a written value) and how its memory was then
 * (CohSnap: the writes reported to its pages, mem/barrier.c). Every writer
 * of guest memory reports its writes (the guest's first-write faults, the
 * host's and the devices', the decoder's copies, the landings below), so
 * its memory is newer than its image once those counts moved (on a page it
 * shares, its own bytes there changed: coh_mem_newer), and its image newer
 * once it was drawn into since (written > synced).
 *
 * Image newer: the target's memory is guarded (mem/barrier.c
 * hle_barrier_guard): a swizzled target's, a linear one's that the front
 * buffer's path and the writeback at its unbind (small, or the CPU's) don't
 * keep. The guest's first access to its bytes has its image land there
 * first: asked for on the guest's thread (coh_fill), at the decoder's next
 * method or pass (coh_serve), a readback whose texels the timeline swizzles
 * (a linear target's rows: copies) into memory when the GPU reaches its
 * value (coh_land, vk/timeline.c), the guest waiting for that value alone
 * and the decoder for nothing. The guest's own bytes on a page a target
 * shares are reached for it through the host's view (native/hle/dirty.c),
 * the page kept closed. A target the guest reads (learned) lands at each
 * fence once drawn into (hle_pbd_fence: the data is there before the guest
 * looks), till COH_UNREAD of its landings in a row go untaken. Fable draws
 * its hero's head and skin into a 256 x 256 R5G6B5 target as the hero
 * loads, a dozen layered composites in a row without leaving it, and
 * compresses that memory into the DXT1 textures the model wears after
 * each: from memory the GPU never wrote, the face and arms were black after
 * a boot, or the noise of an older image there.
 *
 * Memory newer: textures over it read the memory (texture()), a draw into
 * the target has it uploaded first (coh_first_write). The 2D engine's copies
 * are memory's (hle_pbd_copy_begin): one that reads a target whose image is
 * newer waits on the timeline behind that target's landing, its destination
 * closed to the guest till it lands; the decoder goes on. Morrowind copies
 * its inventory's character out of its target (cleared red) as bytes, keys
 * the red out on the CPU and copies it back: from the stale memory, the
 * character was on red with a black band over its head. */
#define COH_SLOTS 64
#define COH_PENDS 32
#define COH_UNREAD 4

typedef struct CohLand {
    int state;                          /* 0 free, 1 asked for, 2 landed or dropped (atomic) */
    uint32_t gen;                       /* g_coh.gen when asked for: a restore's leftovers ignored */
    uint32_t id, pa, len;
    uint64_t v;                         /* its value */
    uint64_t written;
    bool linear, landed;
    CohSnap snap;                       /* the memory as it landed */
} CohLand;
static struct {
    CohLand l[COH_SLOTS];
    uint32_t gen;
    unsigned done;                      /* slots landed, not taken in (atomic) */
    struct { uint32_t lo, hi, slo, shi; uint64_t v; } pend[COH_PENDS]; /* copies queued: into, from */
    unsigned npend;
    unsigned wants;                     /* learned targets drawn into since their landing */
    uint64_t lands, copies, waits;      /* landings asked for, copies queued, decoder waits */
} g_coh;

/* A colour target the coherence keeps: its bytes in memory */
static bool coh_len(const PbObj *o, uint32_t *len)
{
    const HleFmtInfo *fi = o->id && !o->cube && !o->d.parent ? hle_fmt_info(o->d.xfmt) : NULL;
    if (!fi || (fi->flags & (HLE_FI_DEPTH | HLE_FI_DXT)) || fi->bpp < 8) {
        return false;
    }
    *len = o->d.pitch ? o->d.pitch * o->d.h : o->d.w * o->d.h * (fi->bpp / 8);
    return *len != 0;
}

/* ... and one it keeps, guarded while its image is newer: a swizzled one, a
 * linear one but a frame buffer (the front buffer's path), a small one or
 * one the CPU used before it was kept (written back at each unbind, the CPU's
 * writes uploaded by rt_cpu_sync) */
static bool coh_display(const PbObj *o)
{
    return o->display && g_coh_shows - o->shown_seq <= COH_SHOWN_RECENT;
}

static bool coh_managed(const PbObj *o, uint32_t *len)
{
    return coh_len(o, len) && !coh_display(o) && (!o->d.pitch || (*len > WRITEBACK_MAX && !o->cpu_used));
}

/* Any thread: [pa, pa + len) as it is now (CohSnap) */
static void coh_snap_take(uint32_t pa, uint32_t len, CohSnap *s)
{
    const uint32_t end = pa + len, p0 = pa & ~4095u, pl = (end - 1) & ~4095u;
    s->sum = hle_mem_wseq_sum(pa, len, &s->w0, &s->wl);
    s->bsum = hle_mem_wseq_blocks(pa, len);
    s->h0 = (pa & 4095) || (p0 == pl && (end & 4095)) ? span_hash(pa, MIN(end, p0 + 4096) - pa) : 0;
    s->hl = pl != p0 && (end & 4095) ? span_hash(pl, end - pl) : 0;
    s->valid = true;
}

/* The landings done (coh_landed), taken in: their targets' memory matched
 * their images as of the written they landed */
static void coh_adopt(void)
{
    if (!__atomic_load_n(&g_coh.done, __ATOMIC_ACQUIRE)) {
        return;
    }
    for (unsigned i = 0; i < COH_SLOTS; i++) {
        CohLand *l = &g_coh.l[i];
        if (__atomic_load_n(&l->state, __ATOMIC_ACQUIRE) != 2) {
            continue;
        }
        PbObj *o = obj_of(l->id);
        if (o && l->landed && o->d.pa == l->pa && l->written > o->synced) {
            o->synced = l->written;
            o->snap = l->snap;
            o->mn_epoch = 0;
        }
        __atomic_sub_fetch(&g_coh.done, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    }
}

/* Whether o's memory is newer than its image (the memory is what the NV2A
 * holds), as of the write counts' epoch it was last judged in. Only a target
 * the coherence keeps; never drawn into, its memory is all there is; its
 * landing in flight, the memory is its image's (that landing's counts are
 * taken in with it, coh_adopt). Its blocks' counts unmoved, nothing of it
 * was written. Its edge pages' counts move with a neighbour's writes too:
 * then its own bytes there tell, against their hashes when it synced (once
 * a count); the neighbour's are taken in once a frame, those pages tracked
 * again (a written page tracks no stores till then, its own bytes' among
 * them). */
static bool coh_mem_newer(PbObj *o)
{
    uint32_t len;
    if (!coh_managed(o, &len)) {
        return false;                       /* the old paths' (rt_cpu_sync, writeback) */
    }
    /* (completed first: a landing done after it is in flight below, one
     * done before it is taken in here) */
    const uint64_t done = hle_tl_completed();
    coh_adopt();
    if (o->mem_pend) {
        if (o->mem_pend > done) {
            return true;                    /* a copy into it lands later */
        }
        o->mem_pend = 0;                    /* landed: counted below */
    }
    if (!o->written) {
        return true;
    }
    if (!o->snap.valid || o->land_last > done) {
        return false;
    }
    const uint32_t ep = hle_mem_write_epoch() + 1;      /* (0: not judged) */
    if (o->mn_epoch == ep) {
        return o->mn;
    }
    o->mn_epoch = ep;
    const uint64_t bsum = hle_mem_wseq_blocks(o->d.pa, len);
    if (bsum == o->snap.bsum) {
        o->mn = false;
        return false;
    }
    uint32_t w0, wl;
    const uint64_t sum = hle_mem_wseq_sum(o->d.pa, len, &w0, &wl);
    const uint32_t end = o->d.pa + len, p0 = o->d.pa & ~4095u, pl = (end - 1) & ~4095u;
    const bool e0 = (o->d.pa & 4095) || (p0 == pl && (end & 4095)), el = pl != p0 && (end & 4095);
    const bool m0 = e0 && w0 != o->snap.w0, ml = el && wl != o->snap.wl;
    /* its own bytes hashed at a new count, while the page is written open
     * (its stores there not counted), and before the counts are taken in */
    const bool due = o->absorb_swap != g_pd.swaps;
    const bool h0 = m0 && (w0 != o->vw0 || due || hle_host_mem_open(p0));
    const bool hl = ml && (wl != o->vwl || due || hle_host_mem_open(pl));
    const uint64_t in = sum - (e0 ? w0 : 0) - (el ? wl : 0);
    const uint64_t was = o->snap.sum - (e0 ? o->snap.w0 : 0) - (el ? o->snap.wl : 0);
    const bool newer = in != was ||
                       (h0 && span_hash(o->d.pa, MIN(end, p0 + 4096) - o->d.pa) != o->snap.h0) ||
                       (hl && span_hash(pl, end - pl) != o->snap.hl);
    if (!newer) {
        o->vw0 = h0 ? w0 : o->vw0;
        o->vwl = hl ? wl : o->vwl;
        const bool absorb = (m0 || ml) && due;
        if (absorb) {
            o->absorb_swap = g_pd.swaps;
            o->snap.sum = sum;
            o->snap.w0 = w0;
            o->snap.wl = wl;
            if (m0) {
                hle_host_mem_rearm(p0, 4096);
            }
            if (ml) {
                hle_host_mem_rearm(pl, 4096);
            }
        }
        if (!(m0 || ml) || absorb) {
            o->snap.bsum = bsum;            /* (the next look, coarse again) */
        }
    }
    o->mn = newer;
    return newer;
}

/* Timeline (the thread that lands it, vk/timeline.c): landing token done */
static void coh_landed(uint32_t token, bool landed)
{
    CohLand *l = &g_coh.l[token % COH_SLOTS];
    if (l->gen != token / COH_SLOTS || __atomic_load_n(&l->state, __ATOMIC_ACQUIRE) != 1) {
        return;
    }
    if (landed && l->linear) {
        hle_mem_cache_inval(l->pa, l->len);     /* the GPU wrote its rows in place: no stale line */
        hle_mem_mark_landed(l->pa, l->len);     /* ... counted */
    }
    if (landed) {
        coh_snap_take(l->pa, l->len, &l->snap);
        hle_host_mem_rearm(l->pa, l->len);      /* the guest's stores from now on are seen */
    }
    l->landed = landed;
    __atomic_add_fetch(&g_coh.done, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&l->state, 2, __ATOMIC_RELEASE);
}

/* The decoder waits for v (a landing, a queued copy, a writeback): the
 * open submission sent first if v is its */
static void coh_wait(uint64_t v)
{
    if (!v) {
        return;
    }
    if (v > hle_fe_last_value()) {
        kick();
    }
    g_coh.waits++;
    hle_tl_wait(v);
}

/* A free landing slot, the oldest landing in flight waited for if none
 * (-1: none came free) */
static int coh_slot(void)
{
    for (int tries = 0; tries < 2; tries++) {
        coh_adopt();
        uint64_t oldest = UINT64_MAX;
        for (unsigned i = 0; i < COH_SLOTS; i++) {
            const int s = __atomic_load_n(&g_coh.l[i].state, __ATOMIC_ACQUIRE);
            if (!s) {
                return (int)i;
            }
            if (s == 1) {
                oldest = MIN(oldest, g_coh.l[i].v);
            }
        }
        if (oldest == UINT64_MAX) {
            break;
        }
        g_coh.waits++;
        hle_tl_wait(oldest);
    }
    return -1;
}

/* o's image into its memory when the GPU reaches the value returned (0:
 * nothing newer to land, or it can't be): its readback, the landing queued
 * on the timeline, its guard and the first write to its pages (an
 * overlapped read into them too) waiting for it. With the landings' room
 * full (slots, the timeline's queue, the landing buffer), the older ones
 * land first. */
static uint64_t coh_land(PbObj *o)
{
    uint32_t len;
    if (!g_pd.device || !coh_managed(o, &len) || o->written <= o->synced) {
        return 0;
    }
    if (o->land_v && o->land_w == o->written) {
        return o->land_v;                   /* asked for already */
    }
    if (coh_mem_newer(o)) {
        return 0;                           /* its memory is the newer */
    }
    for (int tries = 0; !hle_tl_land_room(2) && tries < 4; tries++) {
        const uint64_t ov = tries < 3 ? hle_tl_land_oldest() : 0;
        coh_wait(ov ? ov : hle_fe_last_value() + 1);
    }
    const int k = coh_slot();
    const HleFmtInfo *fi = hle_fmt_info(o->d.xfmt);
    uint32_t fb = k < 0 ? 0 : o->d.pitch ? hle_res_rb_fb(len) : hle_res_land_alloc(len);
    for (int tries = 0; k >= 0 && !fb && (!o->d.pitch || !hle_res_ram_direct()) && tries < 2; tries++) {
        /* the landing buffer full: the oldest landing's space first, else all */
        const uint64_t ov = tries ? 0 : hle_tl_land_oldest();
        coh_wait(ov ? ov : hle_fe_last_value() + 1);
        fb = o->d.pitch ? hle_res_rb_fb(len) : hle_res_land_alloc(len);
    }
    if (k < 0 || (!fb && (!o->d.pitch || !hle_res_ram_direct()))) {
        static unsigned s_logged;
        if (s_logged++ < 8) {
            hle_log("hakuX-hle-pb", "target %u (%08x): no room to land its image (%s)", o->id, o->d.pa,
                    k < 0 ? "landings" : "landing buffer");
        }
        return 0;
    }
    if (o->d.pitch && !fb) {
        coh_pend_wait_inplace(o->d.pa, len);    /* (before its readback is begun) */
    }
    CohLand *l = &g_coh.l[k];
    const uint32_t token = g_coh.gen * COH_SLOTS + (uint32_t)k;
    l->gen = g_coh.gen;
    l->id = o->id;
    l->pa = o->d.pa;
    l->len = len;
    l->written = o->written;
    l->linear = o->d.pitch != 0;
    l->landed = false;
    __atomic_store_n(&l->state, 1, __ATOMIC_RELEASE);
    HleCmdReadback *rb = begin(HLE_OP_READBACK, sizeof(*rb));
    memset(rb, 0, sizeof(*rb));
    const uint64_t v = rb->value = hle_fe_next_value();     /* a submission of its own */
    l->v = v;
    rb->surf = o->id;
    bool queued;
    if (!o->d.pitch) {
        rb->arena_off = HLE_RB_PACKED;
        rb->fb = fb;
        rb->w = o->d.w;
        rb->h = o->d.h;
        hle_barrier_arm_keep(o->d.pa, len, v, false);
        hle_fe_commit();
        queued = hle_tl_land_swz(v, fb - 1, o->d.pa, o->d.w, o->d.h, fi->bpp / 8, coh_landed, token);
    } else {
        rb->pa = o->d.pa;
        rb->pitch = o->d.pitch;
        rb->arena_off = HLE_RB_DIRECT;      /* rows: guest RAM, or landed from fb */
        hle_res_rb_land(rb, fb, len, v);
        hle_barrier_arm_keep(o->d.pa, len, v, true);
        hle_fe_commit();
        queued = hle_tl_land_note(v, coh_landed, token);
    }
    o->land_last = v;                       /* (the readback runs anyway) */
    if (!queued) {
        __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
        return 0;
    }
    o->land_v = v;
    o->land_w = o->written;
    o->want = false;
    hle_barrier_guard_landing(o->d.pa, len, v);
    g_coh.lands++;
    return v;
}

/* The latest copy queued into [lo, hi) not landed yet (src: or from it; 0:
 * none); the landed ones go */
static uint64_t coh_pend_any(uint32_t lo, uint32_t hi, bool src)
{
    const uint64_t done = hle_tl_completed();
    uint64_t v = 0;
    unsigned k = 0;
    for (unsigned i = 0; i < g_coh.npend; i++) {
        if (g_coh.pend[i].v <= done) {
            continue;
        }
        if ((g_coh.pend[i].lo < hi && lo < g_coh.pend[i].hi) ||
            (src && g_coh.pend[i].slo < hi && lo < g_coh.pend[i].shi)) {
            v = MAX(v, g_coh.pend[i].v);
        }
        g_coh.pend[k++] = g_coh.pend[i];
    }
    g_coh.npend = k;
    return v;
}

static uint64_t coh_pend_over(uint32_t lo, uint32_t hi)
{
    return coh_pend_any(lo, hi, false);
}

/* The decoder about to read [pa, pa + len) on the CPU: copies queued into it
 * land first (the GPU's own order: the copy came before) */
static void coh_pend_wait(uint32_t pa, uint32_t len)
{
    if (!g_coh.npend || !len) {
        return;
    }
    coh_wait(coh_pend_over(pa, pa + len));
}

/* A readback the GPU writes into [pa, pa + len) in place about to be
 * recorded: the copies queued into it or from it land first (the GPU runs
 * on past their value before the timeline lands them) */
static void coh_pend_wait_inplace(uint32_t pa, uint32_t len)
{
    if (!g_coh.npend || !len) {
        return;
    }
    coh_wait(coh_pend_any(pa, pa + len, true));
}

/* Whether a copy's rows (row bytes from dlo, dp apart; over [d0, d1)) hold
 * every byte of target o's (len): dense, or a linear target of their pitch
 * each of whose rows lies in one of theirs */
static bool copy_covers(const PbObj *o, uint32_t len, uint32_t dlo, uint32_t dp, uint32_t row, uint32_t rows,
                        uint32_t d0, uint32_t d1)
{
    if (dp == row) {
        return d0 <= o->d.pa && d1 >= o->d.pa + len;
    }
    if (!o->d.pitch || dp != o->d.pitch || dlo > o->d.pa) {
        return false;
    }
    const HleFmtInfo *fi = hle_fmt_info(o->d.xfmt);
    const uint32_t q = (o->d.pa - dlo) / dp, c = (o->d.pa - dlo) % dp;
    return c + o->d.w * (fi->bpp / 8) <= row && q + o->d.h <= rows;
}

/* The targets the coherence keeps whose images are newer than their memory
 * land first: over the source; over the destination, where the copy leaves
 * some of their bytes. A blit's linear destination target (at dbase) doesn't
 * when the copy is made at once: its image takes the rows too (hle_pbd_blit,
 * as noted in g_coh_rows), at its scale. The copy comes after the landings
 * in flight over either range, the copies queued into either, and those
 * queued from its destination (they read it as they land); queued, the
 * guest's first write to its source waits for it too. */
uint64_t hle_pbd_copy_begin(uint32_t slo, uint32_t sp, uint32_t dlo, uint32_t dp, uint32_t row,
                            uint32_t rows, uint32_t dbase)
{
    g_coh_rows.id = 0;
    if (!g_pd.on || !rows || !row) {
        return 0;
    }
    /* the spans (an M2MF's pitches may be negative: its rows run backwards) */
    const int64_t sl = (int64_t)(int32_t)sp * (rows - 1), dl = (int64_t)(int32_t)dp * (rows - 1);
    const uint32_t s0 = (uint32_t)MIN((int64_t)slo, (int64_t)slo + sl), s1 = (uint32_t)MAX((int64_t)slo, (int64_t)slo + sl) + row;
    const uint32_t d0 = (uint32_t)MIN((int64_t)dlo, (int64_t)dlo + dl), d1 = (uint32_t)MAX((int64_t)dlo, (int64_t)dlo + dl) + row;
    coh_adopt();
    uint32_t len;
    PbObj *rd = dbase && g_pd.device ? obj_of(object_at(dbase)) : NULL;
    if (rd && (!rd->d.pitch || !coh_managed(rd, &len))) {
        rd = NULL;
    }
    const uint64_t done = hle_tl_completed();
    uint64_t v = MAX(coh_pend_any(s0, s1, false), coh_pend_any(d0, d1, true));
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *o = &g_pd.objs[i];
        if (o->land_last > done && coh_len(o, &len) &&
            ((o->d.pa < s1 && o->d.pa + len > s0) || (o->d.pa < d1 && o->d.pa + len > d0))) {
            v = MAX(v, o->land_last);
        }
    }
    for (unsigned i = 0; i < WB_NOTES; i++) {           /* the old paths' writebacks in flight */
        const uint32_t wa = g_wbn[i].pa, we = wa + g_wbn[i].len;
        if (g_wbn[i].v > done && ((wa < s1 && s0 < we) || (wa < d1 && d0 < we))) {
            v = MAX(v, g_wbn[i].v);
        }
    }
    /* (again for rd's own, when the copy is queued after all) */
    for (int pass = 0; pass < 2 && (!pass || (v && rd)); pass++) {
        for (unsigned i = 0; i < g_pd.nobjs; i++) {
            PbObj *o = &g_pd.objs[i];
            if (!coh_managed(o, &len) || o->written <= o->synced || (pass && o != rd)) {
                continue;
            }
            const bool src = o->d.pa < s1 && o->d.pa + len > s0;
            const bool dst = o->d.pa < d1 && o->d.pa + len > d0 && !copy_covers(o, len, dlo, dp, row, rows, d0, d1);
            if (src || (dst && (pass || o != rd))) {
                v = MAX(v, coh_land(o));
            }
        }
    }
    if (v && g_coh.npend == COH_PENDS) {
        coh_pend_over(0, 0);                /* (the landed ones go) */
        if (g_coh.npend == COH_PENDS) {
            coh_wait(g_coh.pend[0].v);      /* the oldest */
            coh_pend_over(0, 0);
        }
    }
    if (v > hle_fe_last_value()) {
        kick();                             /* (a writeback in the open submission: sent, so a
                                               wait for the copy never waits on the decoder) */
    }
    if (v && !hle_tl_land_copy(v, slo, sp, dlo, dp, row, rows, NULL, 0)) {
        coh_wait(v);                        /* no room in the queue: made now, after its landings */
        coh_adopt();
        v = 0;
    }
    if (v) {
        g_coh.pend[g_coh.npend].lo = d0;
        g_coh.pend[g_coh.npend].hi = d1;
        g_coh.pend[g_coh.npend].slo = s0;
        g_coh.pend[g_coh.npend].shi = s1;
        g_coh.pend[g_coh.npend++].v = v;
        hle_barrier_arm_src(s0, s1 - s0, v);
    }
    if (!v && rd) {
        g_coh_rows.id = rd->id;
        g_coh_rows.mem_newer = coh_mem_newer(rd);
        g_coh_rows.img_newer = rd->written > rd->synced;
    }
    g_coh.copies += v != 0;
    return v;
}

void hle_pbd_copy_done(uint32_t dlo, uint32_t dp, uint32_t row, uint32_t rows, uint64_t dep)
{
    if (!g_pd.on || !rows || !row) {
        return;
    }
    const int64_t dl = (int64_t)(int32_t)dp * (rows - 1);
    const uint32_t d0 = (uint32_t)MIN((int64_t)dlo, (int64_t)dlo + dl), d1 = (uint32_t)MAX((int64_t)dlo, (int64_t)dlo + dl) + row;
    if (dep) {
        /* the guest's access waits for it (its landing drops the guard) */
        hle_barrier_guard(d0, d1 - d0);
        hle_barrier_guard_landing(d0, d1 - d0, dep);
    }
    /* the targets kept over it: their memory the newer (but rd's, which
     * takes the rows into its image too) */
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        uint32_t len;
        if (coh_managed(o, &len) && o->d.pa < d1 && o->d.pa + len > d0 && (dep || o->id != g_coh_rows.id)) {
            o->mem_pend = MAX(o->mem_pend, dep);
            o->mn_epoch = 0;
        }
    }
}

/* Guest thread, at its first access to a guarded target's bytes
 * (mem/barrier.c): the landing asked for of the newest image over [pa, pa +
 * len), at the decoder's next method or pass (coh_serve); its value, 0 if
 * none. Withdrawn if the decoder hasn't taken it in 250 ms (it waits on this
 * thread: the access goes ahead on older memory); taken, waited for. */
enum { FILL_IDLE, FILL_ASKED, FILL_TAKEN, FILL_DONE, FILL_GONE };
static struct {
    uint32_t pa, len;
    bool read;
    uint64_t v;
    int state;                              /* FILL_* (atomic) */
    int busy;                               /* a guest thread's request in flight */
} g_fill;

static uint64_t coh_fill(uint32_t pa, uint32_t len, bool read)
{
    if (t_decoder) {
        return 0;                           /* it can't wait for itself */
    }
    while (__atomic_exchange_n(&g_fill.busy, 1, __ATOMIC_ACQUIRE)) {
        g_usleep(100);
    }
    g_fill.pa = pa;
    g_fill.len = len;
    g_fill.read = read;
    g_fill.v = 0;
    __atomic_store_n(&g_fill.state, FILL_ASKED, __ATOMIC_RELEASE);
    hle_pb_boundary_request(1);
    const int64_t t0 = hle_now_ns();
    uint64_t v = 0;
    for (;;) {
        int s = __atomic_load_n(&g_fill.state, __ATOMIC_ACQUIRE);
        if (s == FILL_DONE) {
            v = g_fill.v;
            break;
        }
        if (s == FILL_ASKED && hle_now_ns() - t0 > 250000000 &&
            __atomic_compare_exchange_n(&g_fill.state, &s, FILL_GONE, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            hle_log("hakuX-hle-pb", "target %08x: the decoder did not take its landing in 250 ms "
                    "(the guest reads older memory)", pa);
            break;
        }
        hle_pb_kick();
        g_usleep(100);
    }
    __atomic_store_n(&g_fill.state, FILL_IDLE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fill.busy, 0, __ATOMIC_RELEASE);
    return v;
}

static void coh_serve(void)
{
    int s = FILL_ASKED;
    if (__atomic_load_n(&g_fill.state, __ATOMIC_ACQUIRE) != FILL_ASKED ||
        !__atomic_compare_exchange_n(&g_fill.state, &s, FILL_TAKEN, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return;
    }
    const uint32_t pa = g_fill.pa, end = pa + g_fill.len;
    coh_adopt();
    PbObj *best = NULL;                     /* the memory's last writer */
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        uint32_t len;
        if (coh_managed(o, &len) && o->d.pa < end && o->d.pa + len > pa && o->written > o->synced &&
            (!best || o->written > best->written)) {
            best = o;
        }
    }
    uint64_t v = 0;
    if (best) {
        uint32_t len;
        if (coh_len(best, &len) && best->d.pa == pa && len == g_fill.len) {
            /* the guest uses it (reads it, or writes it, which needs the
             * rest of it there): landed at fences from now on */
            best->learned = true;
        }
        v = coh_land(best);
    }
    g_fill.v = v;
    __atomic_store_n(&g_fill.state, FILL_DONE, __ATOMIC_RELEASE);
}

static void coh_drawn(PbObj *o)
{
    uint32_t len;
    if (!coh_managed(o, &len)) {
        return;
    }
    if (o->land_v) {                        /* its last landing: did the guest take it? */
        uint64_t v;
        const bool untaken = hle_barrier_guard_state(o->d.pa, len, &v) == HLE_GUARD_LANDING &&
                             v == o->land_v;
        o->unread = untaken ? o->unread + 1 : 0;
        if (o->unread >= COH_UNREAD) {
            o->learned = false;
            o->unread = 0;
        }
        o->land_v = 0;
        o->armed_drops = 0;                 /* armed again: its guard held the landing */
    }
    const uint32_t drops = hle_barrier_guard_drops();
    if (o->armed_drops != drops) {
        hle_barrier_guard(o->d.pa, len);
        o->armed_drops = drops;
    }
    if (o->learned && !o->want) {
        o->want = true;
        g_coh.wants++;
    }
}

void hle_pbd_fence(void)
{
    if (!g_coh.wants || !g_pd.on || !g_pd.device) {
        return;
    }
    g_coh.wants = 0;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *o = &g_pd.objs[i];
        if (o->want) {
            o->want = false;
            if (o->learned) {
                coh_land(o);
            }
        }
    }
}

/* o's memory, a swizzled target's, unswizzled into the arena and uploaded
 * into its image */
static void coh_upload_swz(PbObj *o, uint32_t len)
{
    const HleFmtInfo *fi = hle_fmt_info(o->d.xfmt);
    const uint8_t *src = hle_pa_ptr(o->d.pa, len);
    uint8_t *dst = NULL;
    const uint32_t off = src ? hle_draw_arena_alloc(len, &dst) : HLE_ARENA_FULL;
    if (off == HLE_ARENA_FULL) {
        return;
    }
    const uint32_t bpp = fi->bpp / 8;
    const HleXgRect r = { 0, 0, o->d.w, o->d.h };
    hle_xg_unswizzle_rect(src, o->d.w, o->d.h, &r, dst, o->d.w * bpp, 0, 0, bpp);
    HleCmdUpload *c = begin(HLE_OP_UPLOAD, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->dst = o->id;
    c->pitch = o->d.w * bpp;
    c->arena = off + 1;
    c->nrects = 1;
    c->rect[0] = (HleRect){ 0, 0, (int32_t)o->d.w, (int32_t)o->d.h };
    hle_fe_commit();
    o->written = ++g_pd.wseq;
    g_pd.rt_uploads++;
}

/* What newer the others over o's memory (len) hold lands there first,
 * waited for: o's image is about to take that memory (rt_first_land). D3D
 * clears a swizzled target through a linear view of its memory
 * (object_view), and before the swizzled one has an object, the clear goes
 * to the view's: Morrowind's inventory character, drawn for the first time
 * after a load, went over its target's older memory instead of red, and the
 * keying left the edges it had bled into that red opaque around it. As
 * coh_serve: the image of the newest writer among the targets the coherence
 * keeps; the old paths' writebacks in flight there (a small view's, one the
 * CPU used) land before it. */
static void coh_land_over(const PbObj *o, uint32_t len)
{
    PbObj *best = NULL;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *x = &g_pd.objs[i];
        uint32_t xl;
        if (x != o && coh_managed(x, &xl) && x->d.pa < o->d.pa + len && x->d.pa + xl > o->d.pa &&
            x->written > x->synced && (!best || x->written > best->written)) {
            best = x;
        }
    }
    uint64_t v = best ? coh_land(best) : 0;
    const uint64_t done = hle_tl_completed();
    for (unsigned i = 0; i < WB_NOTES; i++) {
        if (g_wbn[i].v > done && g_wbn[i].pa < o->d.pa + len && o->d.pa < g_wbn[i].pa + g_wbn[i].len) {
            v = MAX(v, g_wbn[i].v);
        }
    }
    if (v) {
        coh_wait(v);
        coh_adopt();
    }
}

static void coh_first_write(PbObj *o, bool full)
{
    uint32_t len;
    if (!coh_mem_newer(o)) {
        rt_cpu_sync(o->id);                 /* the CPU's rows (a target the old paths keep) */
        return;
    }
    /* the image gets the memory before the GPU draws over it (a clear of all
     * of it needs none of it) */
    coh_managed(o, &len);
    coh_pend_wait(o->d.pa, len);
    if (!full) {
        if (o->d.pitch) {
            rt_upload(o, o->id, false);
        } else {
            coh_upload_swz(o, len);
        }
    }
    coh_synced(o, len);
}

/* o's image has its memory now (uploaded into it, or about to be drawn over
 * by what covers it all): synced, the memory's counts noted, its pages
 * tracked again */
static void coh_synced(PbObj *o, uint32_t len)
{
    o->synced = o->written;
    if (o->mem_pend && o->mem_pend <= hle_tl_completed()) {
        o->mem_pend = 0;
    }
    coh_snap_take(o->d.pa, len, &o->snap);
    o->mn_epoch = 0;
    hle_host_mem_rearm(o->d.pa, len);       /* the guest's stores from now on are seen */
}

/* x's memory is a frame buffer's: the front buffer's path keeps it,
 * rt_cpu_sync the CPU's writes there, unguarded (guarded again from its
 * next draw once no frame over it was shown lately, coh_display); clear:
 * its pages' dirty bits go (its landings', which would have its memory
 * uploaded over the image) */
static void coh_to_display(PbObj *x, bool clear)
{
    uint32_t len;
    x->display = true;
    x->shown_seq = g_coh_shows;
    x->learned = x->want = false;
    x->armed_drops = 0;
    if (coh_len(x, &len)) {
        hle_barrier_unguard(x->d.pa, len);
        if (clear && x->d.pitch) {
            hle_mem_written(x->d.pa, len);
            obj_edges_set(x);
        }
    }
}

/* o is shown (a frame buffer): it and the targets over its memory (a view
 * of part of it: Breakdown's scene rows, target_part) to the front buffer's
 * path, one made over it while it is shown too (coh_display_over), each
 * present again counting them shown. The landings in flight over it come
 * in first (they dirty its pages); its dirty bits go unless its memory was
 * the newer before them. */
static void coh_shown(PbObj *o)
{
    uint32_t len, xl;
    if (!o) {
        return;
    }
    /* a frame buffer no longer shown lately leaves the front buffer's path:
     * kept again from its memory as it is now (its image the newer for what
     * the GPU drew since it synced: guarded from its next draw). The CPU's
     * writes there go into its image first, while it is still shown lately
     * (rt_cpu_upload passes over the targets the coherence keeps): taken
     * into the snapshot without, they were the image's, and a landing (a
     * savestate's) put the old frame over them, textures loaded into a
     * frame buffer's memory a Reset freed. Not as a target the CPU uses: a
     * writeback at each unbind, were it drawn into again. Uploaded, its
     * image is its memory; a texture defined from that memory since its
     * last draw (whose check took the CPU's writes there: none to upload),
     * the title's data is there: synced either way, no old frame lands. */
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *x = &g_pd.objs[i];
        if (x->display && g_coh_shows + 1 - x->shown_seq > COH_SHOWN_RECENT) {
            const bool uploaded = rt_cpu_upload(x->id, false);
            x->display = false;
            x->armed_drops = 0;
            if (coh_len(x, &xl) && (uploaded || obj_superseded(x))) {
                coh_synced(x, xl);
            } else if (coh_len(x, &xl)) {
                coh_snap_take(x->d.pa, xl, &x->snap);
                x->mn_epoch = 0;
                hle_host_mem_rearm(x->d.pa, xl);
            }
        }
    }
    g_coh_shows++;
    if (!coh_len(o, &len)) {
        coh_to_display(o, false);
        return;
    }
    const bool was = coh_display(o);
    if (!was) {
        const bool newer = coh_mem_newer(o);
        uint64_t lv = 0;
        for (unsigned i = 0; i < g_pd.nobjs; i++) {
            const PbObj *x = &g_pd.objs[i];
            if (coh_len(x, &xl) && x->d.pa < o->d.pa + len && o->d.pa < x->d.pa + xl) {
                lv = MAX(lv, x->land_last);
            }
        }
        if (lv > hle_tl_completed()) {
            coh_wait(lv);
            coh_adopt();
        }
        coh_to_display(o, !newer);
    }
    o->shown_seq = g_coh_shows;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        PbObj *x = &g_pd.objs[i];
        if (x != o && coh_len(x, &xl) && x->d.pa < o->d.pa + len && o->d.pa < x->d.pa + xl) {
            if (coh_display(x)) {
                x->shown_seq = g_coh_shows;
            } else {
                coh_to_display(x, false);
            }
        }
    }
}

/* Whether the memory of a frame buffer shown lately overlaps [pa, pa + len)
 * (one long not shown, its memory reused since, doesn't) */
static bool coh_display_over(uint32_t pa, uint32_t len)
{
    uint32_t xl;
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *x = &g_pd.objs[i];
        if (coh_display(x) && coh_len(x, &xl) && x->d.pa < pa + len && pa < x->d.pa + xl) {
            return true;
        }
    }
    return false;
}

/* o's slot goes to another target: its guard too, unless one that stays
 * shares it */
static void coh_forget(const PbObj *o)
{
    uint32_t len, xl;
    if (!coh_len(o, &len)) {
        return;
    }
    for (unsigned i = 0; i < g_pd.nobjs; i++) {
        const PbObj *x = &g_pd.objs[i];
        if (x != o && x->d.pa == o->d.pa && coh_len(x, &xl) && xl == len) {
            return;
        }
    }
    hle_barrier_unguard(o->d.pa, len);
}

static void coh_reset(void)
{
    g_coh.gen++;
    for (unsigned i = 0; i < COH_SLOTS; i++) {
        __atomic_store_n(&g_coh.l[i].state, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_coh.done, 0, __ATOMIC_RELEASE);
    g_coh.npend = 0;
    g_coh.wants = 0;
}

void hle_pbd_coh_counts(uint64_t *lands, uint64_t *copies, uint64_t *waits)
{
    *lands = g_coh.lands;
    *copies = g_coh.copies;
    *waits = g_coh.waits;
}

/* ---- savestates (pbstate.h) ---- */

void hle_pbd_park(void)
{
    if (!g_pd.on) {
        return;
    }
    uint64_t lv = 0;
    if (g_pd.device) {
        /* the colour targets the GPU drew since the CPU last wrote their
         * memory: linear rows, swizzled texels (coh_land: Conker renders its
         * save screen's 32 x 32 lighting map once, as the screen opens, and
         * after a load the frame and pages sampled what the memory held
         * before, rainbow) */
        writeback_range(0, (uint32_t)hle_ram_size(), true, "savestate", &lv);
    }
    kick();                                 /* with everything else gathered */
    hle_tl_wait(MAX(hle_fe_last_value(), lv));
    coh_adopt();
}

void hle_pbd_restored(void)
{
    g_restored_until = g_pd.swaps + 120;
    g_restored_display = true;
    /* the loaded memory is newer than any image: no landing over it (the
     * frame on screen is guarded again at its swap, a target at its next
     * draw) */
    hle_barrier_set_stale(0, 0, NULL);
    coh_reset();
}

_Static_assert(sizeof(((HlePbdState *)0)->pmem) == sizeof(g_pd.pmem), "pmem");
_Static_assert(sizeof(((HlePbdState *)0)->consts) == sizeof(g_pd.consts), "consts");
_Static_assert(sizeof(((HlePbdState *)0)->cur) == sizeof(g_pd.cur), "cur");
_Static_assert(sizeof(((HlePbdState *)0)->r) == sizeof(g_pd.r), "r");

void hle_pbd_state_get(HlePbdState *s, const float **imm, const uint32_t **inl, const uint32_t **idx)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->r, g_pd.r, sizeof(s->r));
    memcpy(s->pmem, g_pd.pmem, sizeof(s->pmem));
    s->pload = g_pd.pload;
    s->cload = g_pd.cload;
    memcpy(s->consts, g_pd.consts, sizeof(s->consts));
    memcpy(s->cur, g_pd.cur, sizeof(s->cur));
    s->in_draw = g_pd.in_draw;
    s->prim = g_pd.prim;
    s->nimm = g_pd.nimm;
    s->ninl = g_pd.ninl;
    s->nidx = g_pd.nidx;
    s->vis_open = g_pd.vis_open;
    s->clears = g_pd.clears;
    s->swaps = g_pd.swaps;
    s->draws = g_pd.draws;
    /* the display once settled: not while a load's first swap is still to
     * size it (0, as a v1 state), and a loaded one no device took yet */
    const bool settled = g_pd.device && !g_restored_display;
    s->disp_w = settled ? g_pd.disp_w : g_pd.device ? 0 : g_saved_disp.w;
    s->disp_h = settled ? g_pd.disp_h : g_pd.device ? 0 : g_saved_disp.h;
    s->disp_moded = settled ? g_disp_moded : !g_pd.device && g_saved_disp.moded;
    *imm = g_pd.imm;
    *inl = g_pd.inl;
    *idx = g_pd.idx;
}

bool hle_pbd_state_set(const HlePbdState *s, const float *imm, const uint32_t *inl, const uint32_t *idx)
{
    if (s->nimm > MAX_IMM || s->ninl > MAX_INLINE || s->nidx > g_pd.cap) {
        return false;
    }
    memcpy(g_pd.r, s->r, sizeof(g_pd.r));
    memcpy(g_pd.pmem, s->pmem, sizeof(g_pd.pmem));
    g_pd.pgen++;
    g_pd.pload = s->pload;
    g_pd.cload = s->cload;
    memcpy(g_pd.consts, s->consts, sizeof(g_pd.consts));
    memcpy(g_pd.cur, s->cur, sizeof(g_pd.cur));
    g_pd.in_draw = s->in_draw;
    g_pd.prim = s->prim;
    g_pd.nimm = s->nimm;
    g_pd.ninl = s->ninl;
    g_pd.nidx = s->nidx;
    memcpy(g_pd.imm, imm, (size_t)s->nimm * ATTRS * 4 * sizeof(float));
    memcpy(g_pd.inl, inl, (size_t)s->ninl * sizeof(uint32_t));
    memcpy(g_pd.idx, idx, (size_t)s->nidx * sizeof(uint32_t));
    g_pd.clears = s->clears;
    g_pd.swaps = s->swaps;
    g_pd.draws = s->draws;
    if (s->vis_open) {
        /* the test's draws so far were counted by the run that saved */
        hle_log("hakuX-hle-pb", "savestate: a visibility test was open; it counts from here");
        vis_begin();
    }
    g_pd.rt_dirty = true;                   /* the targets bound again from the methods */
    g_pd.cgen++;
    g_pd.ffgen++;
    g_pd.st_sent = false;
    hle_pbd_restored();
    /* the display the state's device had (v2): the device made at the next
     * bind takes it, or the first swap (restored_display), not the target
     * bound then */
    const bool ok = s->disp_w && s->disp_h && s->disp_w <= 4096 && s->disp_h <= 4096;  /* as sync_rt */
    g_saved_disp.w = ok ? s->disp_w : 0;
    g_saved_disp.h = ok ? s->disp_h : 0;
    g_saved_disp.moded = ok && s->disp_moded;
    return true;
}
