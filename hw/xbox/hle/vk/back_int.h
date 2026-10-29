/*
 * hakuX HLE GPU — back-end internals shared by the vk/ sources (CONFIG_VULKAN
 * only).
 */
#ifndef HW_XBOX_HLE_VK_BACK_INT_H
#define HW_XBOX_HLE_VK_BACK_INT_H

#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/front/cmd.h"

/* timeline.c */
int hle_tl_init(void (*on_complete)(uint64_t completed));
/* The core stopping (hle_back_shutdown): the watcher stopped and joined;
 * then, the device idle, the semaphore destroyed and the timeline back at 0. */
void hle_tl_stop(void);
void hle_tl_destroy(void);
VkSemaphore hle_tl_sem(void);
void hle_tl_signaled(uint64_t value);
/* Before each submit: the GPU left idle a moment when the app asked
 * (hle_tl_gap, back.h). */
void hle_tl_gap_serve(void);

/* Resolution scale (the surface_scale setting, 1-4, read at DEVICE_CREATE):
 * every render target and depth surface is rendered at d.w * scale by
 * d.h * scale. Guest-facing values stay in guest pixels (D3D viewports,
 * clear rects, readbacks, visibility counts) and are converted where the
 * image is used. */
extern uint32_t g_hle_rscale;

/* surf.c */
typedef struct HleVkSurf {
    bool alive;
    uint32_t id;
    HleSurfDesc d;
    uint32_t w, h;              /* image extent (views: their level's), scaled */
    uint32_t scale;             /* host pixels per guest pixel (hle_surf_define) */
    VkFormat fmt;
    VkImageAspectFlags aspect;
    uint32_t texel_bytes;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    uint64_t view_gen;          /* new value whenever its views are made: a remade view
                                   can reuse a destroyed one's handle, so the sampling
                                   slots (vk/draw.c surf_slot) compare this */
    VkImageLayout layout;
    uint64_t destroy_after;     /* 0 = live; HLE_SURF_PARKED = next submit */
    uint32_t rec_gen;           /* recording generation that last used it */
    /* Textures with several levels / faces (d.levels, d.faces) and their
     * level / face views (d.parent): one image in GENERAL layout. */
    bool general;
    /* a draw read it as an input attachment (vk/draw.c hle_draw_fb_stages):
     * passes on it render in RENDERING_LOCAL_READ from then on */
    bool local_read;
    uint32_t nlevels, nfaces;   /* owners: the image's levels and layers */
    uint64_t dirty;            /* owners: subresources rendered since the last read barrier */
    uint32_t rendered_levels;   /* owners: levels 0..n-1 sampled (the highest rendered + 1) */
    uint32_t sview_levels;
    VkImageView sview, cube_view;   /* owners: sampled views */
    VkImageView oview;          /* owners: the texels in another byte order
                                   (hle_surf_order_view: oview_ord) */
    uint32_t oview_ord, oview_levels;
    uint64_t oview_gen;         /* view_gen's, for oview */
    VkImage owner_img;          /* views: the owner image the view was made from */
    VkImageView zr_depth, zr_stencil;   /* depth buffers read as colour (zread.c) */
    uint64_t wgen;              /* new value whenever a pass or copy may write it */
    uint64_t zr_gen;            /* zread targets: the source's wgen converted */
    uint32_t zr_src;
    uint8_t zr_map[4];
    /* asynchronous pipeline builds (vk/draw.c target_note): the last swap a
     * draw targeted it and the one before (swap count + 1, 0: never) */
    uint64_t drawn_cur, drawn_prev;
} HleVkSurf;

/* A pass or copy may write s: its wgen changes (zread.c converts again). */
void hle_surf_written(HleVkSurf *s);

/* A surface the open (unsubmitted) recording may reference is never
 * destroyed directly: it is parked and stamped with the value of the
 * submission that closes the recording (Vulkan: destroying a resource used
 * by a recording command buffer invalidates it). */
#define HLE_SURF_PARKED UINT64_MAX

HleVkSurf *hle_surf_get(uint32_t id);
HleVkSurf *hle_surf_define(const HleSurfDesc *d);
bool hle_surf_ensure(HleVkSurf *s);
void hle_surf_destroy_after(uint32_t id, uint64_t value);
void hle_surf_stamp_parked(uint64_t value);
void hle_surf_reap(uint64_t completed);
/* Caller guarantees no open recording (back.c flush_idle). */
void hle_surf_destroy_all(void);
/* Recording generation tracking (debug check in destroy_now). */
void hle_surf_rec_begin(void);
void hle_surf_rec_end(void);
void hle_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to);
void hle_surf_transition(VkCommandBuffer cb, HleVkSurf *s, VkImageLayout to);
/* gamma.c: the swap's copy through the gamma ramp of src's top left sw x sh
 * pixels; false = not done (blit) */
bool hle_gamma_pass(VkCommandBuffer cb, unsigned slot, const uint8_t ramp[3][256],
                    HleVkSurf *src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw, uint32_t dh);
/* zread.c: depth-stencil surface src into colour surface dst as its 32-bit
 * words (HleCmdZRead), outside a pass; slot: the frame's command buffer.
 * NULL = done, else why not. */
/* HLE_OP_ZWRITE: colour surface src's words as depth surface dst's depth
 * (outside a render pass); NULL, or why it was skipped. */
const char *hle_zwrite_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst);
const char *hle_zread_pass(VkCommandBuffer cb, unsigned slot, HleVkSurf *src, HleVkSurf *dst,
                           const uint8_t map[4]);
/* Depth buffer s's depth-aspect view (VK_NULL_HANDLE: none). */
VkImageView hle_zread_depth_view(HleVkSurf *s);
bool hle_surf_read_pending(HleVkSurf *s, uint32_t rt_id);
/* Readback of a scaled surface: subresource (level, layer) of owner o, in
 * layout, blitted down to w x h (guest size) into a scratch image left in
 * TRANSFER_SRC_OPTIMAL. VK_NULL_HANDLE = the format cannot be blitted. */
VkImage hle_surf_down(VkCommandBuffer cb, HleVkSurf *o, VkImageLayout layout,
                      uint32_t level, uint32_t layer, uint32_t w, uint32_t h);
/* A w x h scratch image of fmt (transfer source and destination) for texels
 * on their way up; VK_NULL_HANDLE = none (the format cannot be blitted). */
VkImage hle_surf_scratch(VkFormat fmt, uint32_t w, uint32_t h);
void hle_surf_sample_views(HleVkSurf *s, VkImageView *view, VkImageView *cube);

/* An A8R8G8B8 / X8R8G8B8 target's texels as a 32-bit texture of another
 * byte order over its memory reads them (ord: HLE_DRAW_ORD_*): a view with
 * its channels reordered; VK_NULL_HANDLE when s is no such target. */
VkImageView hle_surf_order_view(HleVkSurf *s, uint32_t ord);
/* s's format has no alpha (X8R8G8B8, X1R5G5B5): sampled, its alpha is 1
 * whatever passes on it wrote there (its sampled views say so). */
bool hle_surf_x_alpha(const HleVkSurf *s);

/* An 8-bit target (NV2A B8, R8 here) stores the colour's blue channel, a
 * G8B8 one (R8G8) blue and green: the fragment shaders write .bgra and clears
 * swap red and blue. */
static inline bool hle_surf_out_bgra(VkFormat f)
{
    return f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}

/* draw.c (P3) */
int hle_draw_init(void);
/* The core stops (hle_back_shutdown: the threads joined, the device idle):
 * each module's Vulkan objects destroyed and its state as before its init,
 * so that a later init on another device starts over. */
void hle_draw_shutdown(void);
void hle_surf_shutdown(void);
void hle_gamma_shutdown(void);
void hle_zread_shutdown(void);
/* Before the render thread starts (hle_back_start): the title's recorded
 * pipelines built (the "Build cached shaders before starting" setting). */
void hle_draw_prebuild(void);
void hle_draw_cmd_state(const HleCmdState *s);
void hle_draw_cmd_state_part(const HleCmdStatePart *p);
void hle_draw_cmd_tex(const HleCmdTex *c, VkCommandBuffer cb);
void hle_draw_cmd_draw(const HleCmdDraw *c, VkCommandBuffer cb, uint32_t rt_id,
                       uint32_t ds_id, const HleCmdViewport *vp);
/* A recording starts (back.c cb_get, outside any pass). */
void hle_draw_rec_begin(VkCommandBuffer cb);
/* It is frame slot's (its last submission has completed): the per-slot push
 * blocks and fbfetch sets (caps.push / caps.push_desc off) start over. */
void hle_draw_slot(unsigned slot);
/* The command buffer's pipeline / dynamic state is no longer what the draws
 * recorded: a new command buffer, or another pipeline bound into it. */
void hle_draw_state_lost(void);
void hle_draw_ff_diag(void);
/* Retired textures: destroyed once the timeline reaches their last use;
 * parked ones get the value of the submit that closes the recording. */
void hle_draw_tex_reap(uint64_t completed);
void hle_draw_tex_stamp(uint64_t value);
bool hle_draw_tex_needs_prep(const HleCmdDraw *c, uint32_t rt_id);
/* Combiner stages of draw c that sample the colour target rt_id it renders
 * into (the NV2A reads the framebuffer): read as an input attachment. */
unsigned hle_draw_fb_stages(const HleCmdDraw *c, uint32_t rt_id);
/* debug (pixel history): the draw state's alpha test enable, func, ref, blend
 * enable, src << 16 | dst, z enable << 16 | z write, z func, colour write mask */
void hle_draw_rs_brief(uint32_t out[9]);
/* Combiner stages of draw c read the colour target rt_id at coordinates of
 * their own (not its pixel, which fbfetch reads): outside the pass,
 * hle_draw_snap copies it and the draw samples the copy; hle_draw_snap_end
 * after the draw. */
bool hle_draw_snap_needed(const HleCmdDraw *c, uint32_t rt_id);
void hle_draw_snap(VkCommandBuffer cb, HleVkSurf *rt);
void hle_draw_snap_end(void);
/* Draw c tests depth EQUAL without writing it: HLE_FB_ZEQ24 / ZEQ16
 * (shader/ff.h) by depth buffer ds_id's step, its fragments in front of the
 * stored depth discarded by reading it (set 2, binding 1); 0 = not. */
unsigned hle_draw_zeq(const HleCmdDraw *c, uint32_t ds_id);
/* The next draw runs without the driver's LRZ (vk/quirks.h HLE_QUIRK_LRZ_CLEAR). */
bool hle_draw_lrz_off(void);
/* What the next hle_draw_cmd_draw has bound of that (back.c): the bits, or 0. */
extern unsigned g_hle_zeq;
/* In the open pass: rt's view as input attachment 0 and depth view zv as the
 * depth one (set 2; either may be absent), after the earlier draws' writes
 * (the depth's: zbar). */
void hle_draw_fb_bind(VkCommandBuffer cb, HleVkSurf *rt, HleVkSurf *ds, VkImageView zv,
                      bool zbar);
extern bool g_hle_fbbar_off;            /* debug (debug.hakux.fbbar=0): no barrier */
/* debug (debug.hakux.gpucut): 1 (frag) draws scissored to one pixel, 2 (draw)
 * the state recorded and no draw, 3 (pass) neither: the passes alone, 4 (tri)
 * draws of 3 vertices scissored to one pixel */
extern int g_hle_gpucut;
/* The vertices drawn and pipelines bound so far (the pass profile, back.c). */
void hle_draw_counts(uint64_t *verts, uint64_t *pipes);
void hle_draw_forget_tex(uint32_t id);
void hle_draw_forget_surf(uint32_t id);
/* A debug capture is armed (pixel history): pipelines are built in
 * place, no draw of the captured frames skipped (vk/draw.c). */
bool hle_back_capture_armed(void);
/* debug probe (draw.c): an occlusion query of the visibility tests is open,
 * and the submissions so far */
bool hle_back_vis_query_open(void);
uint64_t hle_back_submits(void);
void hle_draw_psframe_poll(uint64_t swap);
void hle_draw_cmd_vsh(const HleCmdVsh *c);
void hle_draw_tex_prep(const HleCmdDraw *c, uint32_t rt_id, VkCommandBuffer cb);

/* display.c (here, not display.h, so that header has no Vulkan types) */
VkImage hle_disp_image(int slot);
/* The front frame for the window's presenter (vk/swapchain.c): its slot,
 * held until hle_disp_release(_fenced); the image in SHADER_READ_ONLY_OPTIMAL,
 * complete. Between taking it and submitting the work that reads it, the
 * caller is inside hle_disp_use_begin/end: a display resize (hle_disp_init)
 * waits for that before it destroys the images. */
typedef struct HleDispFrame {
    int slot;
    uint32_t gen, swap_id, w, h;
    uint64_t value;             /* the render timeline's (hle_tl_sem) when it was done */
    VkImage img;
    VkImageView view;
} HleDispFrame;
bool hle_disp_acquire(HleDispFrame *f);
/* The slots' generation (made again at a resize): inside use_begin/end. */
uint32_t hle_disp_gen(void);
void hle_disp_use_begin(void);
void hle_disp_use_end(void);

#endif
