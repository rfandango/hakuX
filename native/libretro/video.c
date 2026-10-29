/*
 * hakuX native core — the frames to RetroArch, in the libretro core.
 *
 * Through Vulkan when RetroArch took the renderer's device (vulkan.c: the
 * front image copied on the GPU). Otherwise, for a frontend without Vulkan,
 * the front frame is read back, as xboxrun's captures are
 * (hle_disp_read_front), and handed over as XRGB8888; a retro_run with no new
 * frame then shows the last one again. Either way the gamma ramp is in the
 * frame (the render thread bakes it), and RetroArch letterboxes it at the
 * aspect reported here.
 *
 * A standard-definition frame loses its overscan, as a TV hid it, unless the
 * core option says otherwise: 1/30 of its height at the top and bottom and
 * of its width at each side, so the picture keeps its shape and fills a 4:3
 * screen. Titles left that band
 * black (Burnout 3: 16 lines above and below its 448) or kept their HUD
 * clear of it (the title-safe area).
 */
#include "native/libretro/libretro_int.h"
#include "native/dev/dev.h"
#include "hw/xbox/hle/vk/display.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_W 3840
#define MAX_H 2160

static struct {
    uint32_t w, h;           /* the frame shown, without its overscan */
    bool have;
    uint32_t *buf;           /* the read-back frame */
    size_t cap;              /* pixels */
    uint32_t gen, swap;      /* the read-back frame's display generation and swap */
    bool dupe_checked, can_dupe;
    bool widescreen;         /* the console is set to widescreen */
    bool full;               /* the overscan shown, not cropped */
} g_v;

/* As present.c's auto: the frame's own shape, but a standard-definition
 * frame (640x480, 720x480: under 16:10) is shown 4:3, or 16:9 when the
 * console is set to widescreen (the title renders anamorphic into it). */
static float aspect_of(uint32_t w, uint32_t h)
{
    const float fa = (float)w / (float)h;
    return fa >= 1.6f ? fa : g_v.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
}

void nc_lr_video_reset(void)
{
    free(g_v.buf);
    memset(&g_v, 0, sizeof(g_v));   /* the options set widescreen and the crop again at load */
}

void nc_lr_video_set_widescreen(bool on)
{
    g_v.widescreen = on;
}

void nc_lr_video_geometry(struct retro_game_geometry *g)
{
    g->base_width = g_v.have ? g_v.w : 640;
    g->base_height = g_v.have ? g_v.h : 480;
    g->max_width = MAX_W;
    g->max_height = MAX_H;
    g->aspect_ratio = aspect_of(g->base_width, g->base_height);
}

void nc_lr_video_set_crop(bool on)
{
    g_v.full = !on;
}

void nc_lr_overscan(uint32_t w, uint32_t h, uint32_t *cx, uint32_t *cy)
{
    const bool sd = !g_v.full && (float)w / (float)h < 1.6f;
    *cx = sd ? w / 30 : 0;
    *cy = sd ? h / 30 : 0;
}

void nc_lr_video_size(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh)
{
    if (g_v.have && ow == g_v.w && oh == g_v.h) {
        return;
    }
    g_v.w = ow;
    g_v.h = oh;
    g_v.have = true;
    struct retro_game_geometry geo;
    nc_lr_video_geometry(&geo);
    nc_lr_env(RETRO_ENVIRONMENT_SET_GEOMETRY, &geo);
    nc_log("hakuX-libretro", "video: %ux%u, %ux%u without its overscan, shown at %.3f", w, h, ow, oh,
           geo.aspect_ratio);
}

/* The front frame is looked at now: the NV2A's vblank keeps half a period
 * from here (nc_dev_nv2a_display_pickup). Not retro_run's start, the vsync:
 * what comes between them - the audio batch, which RetroArch may block in
 * with audio sync, and wait_sync_index - moves the look by up to a period,
 * and a vblank beside it then showed a frame twice and skipped the next
 * (47.5 new frames a second of the guest's 59.9 under ROCKNIX's RetroArch). */
void nc_lr_video_pickup(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    nc_dev_nv2a_display_pickup((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

/* Without Vulkan: the front frame, if it is newer than the last one shown,
 * read back into g_v.buf (without its overscan). */
static bool take_new(void)
{
    uint32_t gen, swap;
    nc_lr_video_pickup();
    if (!hle_disp_front(&gen, &swap) || (g_v.buf && gen == g_v.gen && swap == g_v.swap)) {
        return false;
    }
    uint32_t w = 0, h = 0;
    uint8_t *rgba = hle_disp_read_front(&w, &h);
    if (!rgba || !w || !h || w > MAX_W || h > MAX_H) {
        g_free(rgba);
        return false;
    }
    uint32_t cx, cy;
    nc_lr_overscan(w, h, &cx, &cy);
    const uint32_t ow = w - 2 * cx, oh = h - 2 * cy;
    const size_t n = (size_t)ow * oh;
    if (n > g_v.cap) {
        uint32_t *b = realloc(g_v.buf, n * sizeof(*b));
        if (!b) {
            g_free(rgba);
            return false;
        }
        g_v.buf = b;
        g_v.cap = n;
    }
    for (uint32_t y = 0; y < oh; y++) {
        const uint8_t *p = rgba + 4 * ((size_t)(y + cy) * w + cx);
        uint32_t *d = g_v.buf + (size_t)y * ow;
        for (uint32_t x = 0; x < ow; x++, p += 4) {
            d[x] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }
    g_free(rgba);
    g_v.gen = gen;
    g_v.swap = swap;
    nc_lr_video_size(w, h, ow, oh);
    return true;
}

void nc_lr_video_run(retro_video_refresh_t video)
{
    static uint32_t black[640 * 480];
    if (nc_lr_vk_run(video)) {
        return;
    }
    if (!g_v.dupe_checked) {
        g_v.dupe_checked = true;
        g_v.can_dupe = false;
        nc_lr_env(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_v.can_dupe);
    }
    if (take_new()) {
        video(g_v.buf, g_v.w, g_v.h, g_v.w * sizeof(uint32_t));
    } else if (g_v.buf) {
        video(g_v.can_dupe ? NULL : g_v.buf, g_v.w, g_v.h, g_v.w * sizeof(uint32_t));
    } else {
        video(black, 640, 480, 640 * sizeof(uint32_t));
    }
}
