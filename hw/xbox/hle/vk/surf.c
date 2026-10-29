/*
 * hakuX HLE GPU — render-target / depth surfaces (spec §5.5: HLE-owned
 * images; identity by front-end object id).
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/tex/xdk.h"
#include "hw/xbox/hle/vk/back_int.h"

#define T (g_hvk.t)
#define HLE_MAX_VK_SURF 256

#define HLE_MAX_VIEW_RETIRE 128

static HleVkSurf g_surf[HLE_MAX_VK_SURF];

/* Lookup by id (several per draw): a per-id hint, set only while that
 * surface is live (unhint takes it away when it is parked or destroyed, so
 * a hit needs no check), and misses remembered until a surface is created
 * (g_surf_gen). */
#define SURF_IDS 8193
static uint16_t g_surf_hint[SURF_IDS];
static uint32_t g_surf_miss[SURF_IDS], g_surf_gen = 1;

static void unhint(const HleVkSurf *s)
{
    if (s->id < SURF_IDS && g_surf_hint[s->id] == s - g_surf + 1) {
        g_surf_hint[s->id] = 0;
    }
}

static uint32_t g_rec_gen;          /* render thread only */
static bool g_rec_open;
static uint64_t g_surf_bugs;
static uint64_t g_view_gen;         /* HleVkSurf.view_gen */
static uint64_t g_wgen;             /* HleVkSurf.wgen */
static unsigned g_log_general, g_log_unscaled, g_log_full;     /* hle_surf_define's logs */
/* Views replaced while recordings may still use them: destroyed once the
 * submission that closes the open recording completes. */
static struct {
    VkImageView v;
    uint64_t after;
} g_vretire[HLE_MAX_VIEW_RETIRE];

static void retire_view(VkImageView v)
{
    if (!v) {
        return;
    }
    for (unsigned i = 0; i < HLE_MAX_VIEW_RETIRE; i++) {
        if (!g_vretire[i].v) {
            g_vretire[i].v = v;
            g_vretire[i].after = HLE_SURF_PARKED;
            return;
        }
    }
    hle_vk_queue_lock();
    const VkResult r = T.vkDeviceWaitIdle(g_hvk.dev);   /* full: rare, and safe */
    hle_vk_queue_unlock();
    hle_vk_check(r, "vkDeviceWaitIdle (views)");
    T.vkDestroyImageView(g_hvk.dev, v, NULL);
}

static bool owner_kind(const HleSurfDesc *d)
{
    return !d->parent && (d->levels > 1 || d->faces > 1);
}

/* Small render-to-texture targets render at guest resolution. Morrowind's
 * water ripples are a simulation in 128x128 targets drawn with quads whose
 * edges lie on pixel centres (0..128 plus D3D's 0.53125 offset, truncated to
 * 0.5): the NV2A's top-left rule covers those whole pixels, a scaled image
 * leaves its first host row and column unwritten, and the wrapped normal
 * map then seamed every water tile. Its taps also rely on the 1x sample
 * positions (texel corners, half-texel offsets). */
#define SURF_UNSCALED_MAX 128

static uint32_t surf_scale(const HleSurfDesc *d, VkImageAspectFlags aspect)
{
    bool small = !d->role && !d->pitch && !d->parent && d->levels <= 1 && d->faces <= 1 &&
                 (aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
                 MAX(d->w, d->h) <= SURF_UNSCALED_MAX;
    return small ? 1 : g_hle_rscale;
}

static uint32_t sub_bit(const HleVkSurf *o, uint32_t level, uint32_t face)
{
    uint32_t i = face * o->nlevels + level;
    return i < 63 ? i : 63;
}

void hle_surf_rec_begin(void)
{
    g_rec_gen++;
    g_rec_open = true;
}

void hle_surf_rec_end(void)
{
    g_rec_open = false;
}

static bool attach_ok(VkFormat f, bool depth)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_hvk.phys, f, &fp);
    VkFormatFeatureFlags need = depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                      : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

static VkFormat surf_format(uint32_t xfmt, VkImageAspectFlags *aspect, uint32_t *bytes)
{
    *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (xfmt) {
    case HLE_XFMT_A8R8G8B8: case HLE_XFMT_LIN_A8R8G8B8:
    case HLE_XFMT_X8R8G8B8: case HLE_XFMT_LIN_X8R8G8B8:
        *bytes = 4;
        return VK_FORMAT_B8G8R8A8_UNORM;
    case HLE_XFMT_R5G6B5: case HLE_XFMT_LIN_R5G6B5:
        *bytes = 2;
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case HLE_XFMT_A1R5G5B5: case HLE_XFMT_LIN_A1R5G5B5:
    case HLE_XFMT_X1R5G5B5: case HLE_XFMT_LIN_X1R5G5B5:
        *bytes = 2;
        return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case HLE_XFMT_L8: case HLE_XFMT_LIN_L8:     /* NV2A B8: the blue channel */
        *bytes = 1;
        return VK_FORMAT_R8_UNORM;
    case HLE_XFMT_G8B8: case HLE_XFMT_LIN_G8B8: /* B in the low byte (R), G the high */
        *bytes = 2;
        return VK_FORMAT_R8G8_UNORM;
    case HLE_XFMT_L16: case HLE_XFMT_LIN_L16:   /* a depth buffer's halves (zread) */
        *bytes = 2;
        return attach_ok(VK_FORMAT_R16_UNORM, false) ? VK_FORMAT_R16_UNORM : VK_FORMAT_UNDEFINED;
    case HLE_XFMT_D24S8: case HLE_XFMT_LIN_D24S8:
        *aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        *bytes = 4;
        /* D32S8 where D24S8 is no attachment (Xclipse) or can't be filtered (vkdev.c) */
        return g_hvk.caps.d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    case HLE_XFMT_F24S8: case HLE_XFMT_LIN_F24S8:
        *aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        *bytes = 4;
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case HLE_XFMT_D16: case HLE_XFMT_LIN_D16:
        *aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        *bytes = 2;
        return VK_FORMAT_D16_UNORM;
    case HLE_XFMT_F16: case HLE_XFMT_LIN_F16:
        *aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        *bytes = 2;
        return VK_FORMAT_D32_SFLOAT;
    default:
        *bytes = 0;
        return VK_FORMAT_UNDEFINED;
    }
}

static void destroy_now(HleVkSurf *s)
{
    unhint(s);
    if (s->img && g_rec_open && s->rec_gen == g_rec_gen) {
        /* Must never happen: the open recording references this image. */
        if (g_surf_bugs++ < 8) {
            hle_log("hakuX-hle-back", "BUG: destroying surface %u used by the open "
                    "recording", s->id);
        }
    }
    if (s->view) {
        T.vkDestroyImageView(g_hvk.dev, s->view, NULL);
    }
    if (s->d.parent) {                          /* a view: the owner holds the image */
        memset(s, 0, sizeof(*s));
        return;
    }
    if (s->sview) {
        T.vkDestroyImageView(g_hvk.dev, s->sview, NULL);
    }
    if (s->oview) {
        T.vkDestroyImageView(g_hvk.dev, s->oview, NULL);
    }
    if (s->zr_depth) {
        T.vkDestroyImageView(g_hvk.dev, s->zr_depth, NULL);
    }
    if (s->zr_stencil) {
        T.vkDestroyImageView(g_hvk.dev, s->zr_stencil, NULL);
    }
    if (s->cube_view) {
        T.vkDestroyImageView(g_hvk.dev, s->cube_view, NULL);
    }
    for (unsigned i = 0; s->img && s->general && i < HLE_MAX_VK_SURF; i++) {
        HleVkSurf *v = &g_surf[i];
        if (v->alive && v->d.parent == s->id && v->owner_img == s->img) {
            if (v->view) {                      /* last used before the owner was parked */
                T.vkDestroyImageView(g_hvk.dev, v->view, NULL);
            }
            v->view = VK_NULL_HANDLE;
            v->img = VK_NULL_HANDLE;
            v->owner_img = VK_NULL_HANDLE;
        }
    }
    if (s->img) {
        T.vkDestroyImage(g_hvk.dev, s->img, NULL);
    }
    if (s->mem) {
        T.vkFreeMemory(g_hvk.dev, s->mem, NULL);
    }
    memset(s, 0, sizeof(*s));
}

static bool surf_live(const HleVkSurf *s, uint32_t id)
{
    return s->alive && s->id == id && !s->destroy_after;
}

HleVkSurf *hle_surf_get(uint32_t id)
{
    if (!id) {
        return NULL;
    }
    if (id < SURF_IDS) {
        unsigned h = g_surf_hint[id];
        if (h) {
            return &g_surf[h - 1];
        }
        if (g_surf_miss[id] == g_surf_gen) {
            return NULL;
        }
    }
    for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
        if (surf_live(&g_surf[i], id)) {
            if (id < SURF_IDS) {
                g_surf_hint[id] = i + 1;
            }
            return &g_surf[i];
        }
    }
    if (id < SURF_IDS) {
        g_surf_miss[id] = g_surf_gen;
    }
    return NULL;
}

HleVkSurf *hle_surf_define(const HleSurfDesc *d)
{
    HleVkSurf *s = hle_surf_get(d->id);
    if (s && s->d.xfmt == d->xfmt && s->d.w == d->w && s->d.h == d->h &&
        s->d.parent == d->parent && s->d.levels == d->levels && s->d.faces == d->faces &&
        s->d.level == d->level && s->d.face == d->face) {
        /* Same image identity. pa/pitch are READBACK metadata (Swap's Data
         * rotation and header re-resolution change them every frame), and the
         * role set by DEVICE_CREATE survives an OBJ_CREATE with role 0. */
        uint32_t role = s->d.role;
        s->d = *d;
        if (!d->role) {
            s->d.role = role;
        }
        return s;
    }
    if (s) {
        /* Real redefinition (format or size): the old image may be in the
         * open recording, so park it instead of waiting the device idle. */
        s->destroy_after = HLE_SURF_PARKED;
        unhint(s);
    }
    for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
        if (!g_surf[i].alive) {
            s = &g_surf[i];
            memset(s, 0, sizeof(*s));
            s->alive = true;
            s->id = d->id;
            g_surf_gen++;                       /* remembered misses may now hit */
            s->d = *d;
            s->fmt = surf_format(d->xfmt, &s->aspect, &s->texel_bytes);
            s->scale = surf_scale(d, s->aspect);
            s->w = d->w * s->scale;
            s->h = d->h * s->scale;
            s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            s->general = d->parent || owner_kind(d);
            s->nlevels = s->nfaces = 1;
            if (owner_kind(d)) {
                /* levels the image can hold; a cube needs square faces */
                uint32_t mx = 1;
                while ((MAX(d->w, d->h) >> mx) && mx < 16) {
                    mx++;
                }
                s->nlevels = MIN(MAX((uint32_t)d->levels, 1u), mx);
                s->nfaces = d->faces == 6 && d->w == d->h ? 6 : 1;
            }
            if (s->fmt == VK_FORMAT_UNDEFINED) {
                hle_log("hakuX-hle-back", "surface %u: format 0x%x not renderable",
                        d->id, d->xfmt);
            }
            if (s->general && g_log_general < 48) {
                g_log_general++;
                hle_log("hakuX-hle-back", "surface %u: %s %ux%u fmt 0x%x levels %u faces %u "
                        "(parent %u level %u face %u)", d->id, d->parent ? "view" : "texture",
                        d->w, d->h, d->xfmt, s->nlevels, s->nfaces, d->parent, d->level,
                        d->face);
            }
            if (s->scale != g_hle_rscale && g_log_unscaled < 16) {
                g_log_unscaled++;
                hle_log("hakuX-hle-back", "surface %u: %ux%u fmt 0x%x at guest resolution",
                        d->id, d->w, d->h, d->xfmt);
            }
            return s;
        }
    }
    if (g_log_full++ < 4) {
        /* what holds the table: live surfaces by size / format / kind */
        struct { uint32_t w, h, f, k, n; } hist[24];
        unsigned nh = 0, parked = 0;
        for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
            const HleVkSurf *q = &g_surf[i];
            parked += q->destroy_after != 0;
            uint32_t k = q->d.parent ? 2 : q->general ? 1 : 0;
            unsigned j = 0;
            while (j < nh && !(hist[j].w == q->d.w && hist[j].h == q->d.h &&
                               hist[j].f == q->d.xfmt && hist[j].k == k)) {
                j++;
            }
            if (j == nh && nh < ARRAY_SIZE(hist)) {
                hist[nh++] = (typeof(hist[0])){ q->d.w, q->d.h, q->d.xfmt, k, 0 };
            }
            if (j < nh) {
                hist[j].n++;
            }
        }
        char line[512];
        int m = 0;
        for (unsigned j = 0; j < nh && m < (int)sizeof(line) - 32; j++) {
            m += snprintf(line + m, sizeof(line) - m, " %ux%u/%x/%c:%u", hist[j].w, hist[j].h,
                          hist[j].f, "tgv"[hist[j].k], hist[j].n);
        }
        hle_log("hakuX-hle-back", "surface table full (%u; %u to destroy):%s", HLE_MAX_VK_SURF,
                parked, line);
    }
    return NULL;
}

/* A level / face surface: a 2D view of its owner's subresource, remade when
 * the owner's image changed. */
static bool ensure_view(HleVkSurf *s)
{
    HleVkSurf *o = hle_surf_get(s->d.parent);
    if (!o || !o->general || !hle_surf_ensure(o) || s->d.level >= o->nlevels ||
        s->d.face >= o->nfaces) {
        return false;
    }
    if (s->view && s->owner_img == o->img) {
        return true;
    }
    retire_view(s->view);
    s->view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = o->fmt,
        .subresourceRange = { o->aspect, s->d.level, 1, s->d.face, 1 } };
    if (T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->view) != VK_SUCCESS) {
        s->view = VK_NULL_HANDLE;
        return false;
    }
    s->view_gen = ++g_view_gen;
    s->img = o->img;
    s->owner_img = o->img;
    s->scale = o->scale;
    s->w = MAX(o->w >> s->d.level, 1u);
    s->h = MAX(o->h >> s->d.level, 1u);
    s->fmt = o->fmt;
    s->aspect = o->aspect;
    s->texel_bytes = o->texel_bytes;
    s->layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

bool hle_surf_ensure(HleVkSurf *s)
{
    if (s->d.parent) {
        return ensure_view(s);
    }
    if (s->img) {
        return true;
    }
    if (s->fmt == VK_FORMAT_UNDEFINED || !s->d.w || !s->d.h) {
        return false;
    }
    bool depth = !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .flags = s->nfaces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .format = s->fmt, .extent = { s->w, s->h, 1 }, .mipLevels = s->nlevels,
        .arrayLayers = s->nfaces, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* input attachments: colour for fbfetch, depth for EQUAL passes
         * (vk/draw.c hle_draw_zeq) */
        .usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                 (g_hvk.caps.fbfetch ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0) |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &s->img) != VK_SUCCESS) {
        s->img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    T.vkGetImageMemoryRequirements(g_hvk.dev, s->img, &req);
    int mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &s->mem) != VK_SUCCESS) {
        T.vkDestroyImage(g_hvk.dev, s->img, NULL);
        s->img = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->fmt,
        .subresourceRange = { s->aspect, 0, 1, 0, 1 } };
    if (!hle_vk_check(T.vkBindImageMemory(g_hvk.dev, s->img, s->mem, 0), "vkBindImageMemory (surface)") ||
        !hle_vk_check(T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->view),   /* level 0, face 0 */
                      "vkCreateImageView (surface)")) {
        T.vkDestroyImage(g_hvk.dev, s->img, NULL);
        T.vkFreeMemory(g_hvk.dev, s->mem, NULL);
        s->img = VK_NULL_HANDLE;
        s->mem = VK_NULL_HANDLE;
        s->view = VK_NULL_HANDLE;
        return false;
    }
    s->view_gen = ++g_view_gen;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->dirty = 0;
    s->rendered_levels = 0;
    s->sview_levels = 0;
    s->oview_levels = 0;
    return true;
}

void hle_surf_destroy_after(uint32_t id, uint64_t value)
{
    HleVkSurf *s = hle_surf_get(id);
    if (s) {
        s->destroy_after = value ? value : 1;
        unhint(s);
    }
}

void hle_surf_stamp_parked(uint64_t value)
{
    for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
        if (g_surf[i].alive && g_surf[i].destroy_after == HLE_SURF_PARKED) {
            g_surf[i].destroy_after = value;
        }
    }
    for (unsigned i = 0; i < HLE_MAX_VIEW_RETIRE; i++) {
        if (g_vretire[i].v && g_vretire[i].after == HLE_SURF_PARKED) {
            g_vretire[i].after = value;
        }
    }
}

void hle_surf_reap(uint64_t completed)
{
    for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
        HleVkSurf *s = &g_surf[i];
        if (s->alive && s->destroy_after && s->destroy_after != HLE_SURF_PARKED &&
            s->destroy_after <= completed) {
            destroy_now(s);
        }
    }
    for (unsigned i = 0; i < HLE_MAX_VIEW_RETIRE; i++) {
        if (g_vretire[i].v && g_vretire[i].after != HLE_SURF_PARKED &&
            g_vretire[i].after <= completed) {
            T.vkDestroyImageView(g_hvk.dev, g_vretire[i].v, NULL);
            g_vretire[i].v = VK_NULL_HANDLE;
        }
    }
}

/* Guest-size scratch images that readbacks of scaled surfaces are blitted
 * down into: one per format and size, kept until the surfaces go. */
#define HLE_MAX_DOWN 8
static struct {
    VkImage img;
    VkDeviceMemory mem;
    VkFormat fmt;
    uint32_t w, h;
} g_down[HLE_MAX_DOWN];

static VkImage down_image(VkFormat fmt, uint32_t w, uint32_t h)
{
    unsigned i = 0;
    for (; i < HLE_MAX_DOWN && g_down[i].img; i++) {
        if (g_down[i].fmt == fmt && g_down[i].w == w && g_down[i].h == h) {
            return g_down[i].img;
        }
    }
    if (i == HLE_MAX_DOWN) {
        return VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &img) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements req;
    T.vkGetImageMemoryRequirements(g_hvk.dev, img, &req);
    int mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory mem;
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &mem) != VK_SUCCESS) {
        T.vkDestroyImage(g_hvk.dev, img, NULL);
        return VK_NULL_HANDLE;
    }
    T.vkBindImageMemory(g_hvk.dev, img, mem, 0);
    g_down[i].img = img;
    g_down[i].mem = mem;
    g_down[i].fmt = fmt;
    g_down[i].w = w;
    g_down[i].h = h;
    return img;
}

VkImage hle_surf_scratch(VkFormat fmt, uint32_t w, uint32_t h)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_hvk.phys, fmt, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need ? down_image(fmt, w, h) : VK_NULL_HANDLE;
}

VkImage hle_surf_down(VkCommandBuffer cb, HleVkSurf *o, VkImageLayout layout,
                      uint32_t level, uint32_t layer, uint32_t w, uint32_t h)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_hvk.phys, o->fmt, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkImage d = (fp.optimalTilingFeatures & need) == need ? down_image(o->fmt, w, h)
                                                          : VK_NULL_HANDLE;
    if (!d) {
        return VK_NULL_HANDLE;
    }
    bool lin = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    hle_img_barrier(cb, d, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit2 r = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)MAX(o->w >> level, 1u),
                                       (int32_t)MAX(o->h >> level, 1u), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
    VkBlitImageInfo2 bi = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = o->img, .srcImageLayout = layout,
        .dstImage = d, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1, .pRegions = &r, .filter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST };
    T.vkCmdBlitImage2(cb, &bi);
    hle_img_barrier(cb, d, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return d;
}

void hle_surf_destroy_all(void)
{
    hle_vk_queue_lock();
    const VkResult r = T.vkDeviceWaitIdle(g_hvk.dev);   /* caller already flushed (flush_idle) */
    hle_vk_queue_unlock();
    hle_vk_check(r, "vkDeviceWaitIdle (surfaces)");
    for (unsigned i = 0; i < HLE_MAX_DOWN; i++) {
        if (g_down[i].img) {
            T.vkDestroyImage(g_hvk.dev, g_down[i].img, NULL);
            T.vkFreeMemory(g_hvk.dev, g_down[i].mem, NULL);
        }
    }
    memset(g_down, 0, sizeof(g_down));
    for (unsigned i = 0; i < HLE_MAX_VK_SURF; i++) {
        if (g_surf[i].alive) {
            destroy_now(&g_surf[i]);
        }
    }
    for (unsigned i = 0; i < HLE_MAX_VIEW_RETIRE; i++) {
        if (g_vretire[i].v) {
            T.vkDestroyImageView(g_hvk.dev, g_vretire[i].v, NULL);
            g_vretire[i].v = VK_NULL_HANDLE;
        }
    }
}

/* The core stops (back_int.h): every surface, parked or live, its views,
 * the retired views and the down / scratch images destroyed (destroy_all),
 * then the table, its hints and misses, the counters and the logs' limits
 * as before the first surface. A recording left open is never submitted
 * (back.c frees its pool), so destroying what it used is no bug. The view
 * and write generations start over with the surfaces: what keeps them
 * (vk/draw.c's surface slots, back.c's zeq_wgen) is reset in the same stop. */
void hle_surf_shutdown(void)
{
    g_rec_open = false;
    hle_surf_destroy_all();
    memset(g_surf, 0, sizeof(g_surf));
    memset(g_vretire, 0, sizeof(g_vretire));
    memset(g_surf_hint, 0, sizeof(g_surf_hint));
    memset(g_surf_miss, 0, sizeof(g_surf_miss));
    g_surf_gen = 1;
    g_rec_gen = 0;
    g_surf_bugs = 0;
    g_view_gen = 0;
    g_wgen = 0;
    g_log_general = g_log_unscaled = g_log_full = 0;
}

static void barrier_range(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                          VkImageLayout from, VkImageLayout to, uint32_t level,
                          uint32_t nlevels, uint32_t layer, uint32_t nlayers)
{
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { aspect, level, nlevels, layer, nlayers } };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b };
    T.vkCmdPipelineBarrier2(cb, &di);
}

void hle_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to)
{
    barrier_range(cb, img, aspect, from, to, 0, 1, 0, 1);
}

static HleVkSurf *owner_of(HleVkSurf *s)
{
    return s->d.parent ? hle_surf_get(s->d.parent) : s;
}

/* Owners and views stay in GENERAL: a pass renders one subresource while
 * draws sample others (mip chains built level from level). Writes are
 * ordered after earlier uses of their subresource; reads wait for the
 * subresources rendered since the last read barrier. */
static void general_use(VkCommandBuffer cb, HleVkSurf *s, VkImageLayout to)
{
    HleVkSurf *o = owner_of(s);
    if (!o || !o->img) {
        return;
    }
    o->rec_gen = g_rec_gen;
    s->layout = VK_IMAGE_LAYOUT_GENERAL;
    if (o->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        /* levels and faces never rendered read as zero, not stale memory */
        barrier_range(cb, o->img, o->aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL, 0, o->nlevels, 0, o->nfaces);
        VkImageSubresourceRange all = { o->aspect, 0, o->nlevels, 0, o->nfaces };
        if (o->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue z = { { 0 } };
            T.vkCmdClearColorImage(cb, o->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
        } else {
            VkClearDepthStencilValue z = { 1.0f, 0 };
            T.vkCmdClearDepthStencilImage(cb, o->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
        }
        o->layout = VK_IMAGE_LAYOUT_GENERAL;
        o->dirty = ~0ull;
    }
    uint32_t level = s->d.parent ? s->d.level : 0, face = s->d.parent ? s->d.face : 0;
    if (to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
        to == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier_range(cb, o->img, o->aspect, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL, level, 1, face, 1);
        o->dirty |= 1ull << sub_bit(o, level, face);
        o->rendered_levels = MAX(o->rendered_levels, level + 1);
    } else if (o->dirty) {
        barrier_range(cb, o->img, o->aspect, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL, 0, o->nlevels, 0, o->nfaces);
        o->dirty = 0;
    }
}

void hle_surf_written(HleVkSurf *s)
{
    s->wgen = ++g_wgen;                         /* never repeats, across surfaces too */
}

void hle_surf_transition(VkCommandBuffer cb, HleVkSurf *s, VkImageLayout to)
{
    s->rec_gen = g_rec_gen;                     /* every recorded use goes through here */
    if (s->general) {
        general_use(cb, s, to);
        return;
    }
    if (s->layout != to) {
        hle_img_barrier(cb, s->img, s->aspect, s->layout, to);
        s->layout = to;
    }
}

/* Sampling owner s needs a read barrier first: subresources rendered since
 * the last one, other than the one the current pass renders (rt_id). */
bool hle_surf_read_pending(HleVkSurf *s, uint32_t rt_id)
{
    uint64_t d = s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? ~0ull : s->dirty;
    HleVkSurf *rt = d ? hle_surf_get(rt_id) : NULL;
    if (rt && rt->general && owner_of(rt) == s) {
        d &= ~(1ull << sub_bit(s, rt->d.parent ? rt->d.level : 0,
                               rt->d.parent ? rt->d.face : 0));
    }
    return d != 0;
}

bool hle_surf_x_alpha(const HleVkSurf *s)
{
    switch (s->d.xfmt) {
    case HLE_XFMT_X8R8G8B8: case HLE_XFMT_LIN_X8R8G8B8:
    case HLE_XFMT_X1R5G5B5: case HLE_XFMT_LIN_X1R5G5B5:
        return (s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    default:
        return false;
    }
}

/* Sampled views of owner s: the levels rendered so far (a level never
 * rendered is left out, as a single-level texture's top surface would be). */
void hle_surf_sample_views(HleVkSurf *s, VkImageView *view, VkImageView *cube)
{
    uint32_t n = MIN(MAX(s->rendered_levels, 1u), s->nlevels);
    if (s->sview_levels != n || !s->sview) {
        retire_view(s->sview);
        retire_view(s->cube_view);
        s->sview = s->cube_view = VK_NULL_HANDLE;
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->fmt,
            .subresourceRange = { s->aspect & VK_IMAGE_ASPECT_COLOR_BIT ?
                                  VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
                                  0, n, 0, 1 } };
        if (s->fmt == VK_FORMAT_R8_UNORM || s->fmt == VK_FORMAT_R16_UNORM) {    /* L8, L16: (l, l, l, 1) */
            vci.components = (VkComponentMapping){
                VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                VK_COMPONENT_SWIZZLE_ONE };
        } else if (s->fmt == VK_FORMAT_R8G8_UNORM) {    /* G8B8: (b, g, b, g), its texture map */
            vci.components = (VkComponentMapping){
                VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
                VK_COMPONENT_SWIZZLE_G };
        } else if (hle_surf_x_alpha(s)) {
            /* GTA San Andreas's shadow camera renders its silhouette into an
             * X8R8G8B8 target (alpha 0 where the ped is) and resamples it
             * alpha-tested: the ped's texels must pass */
            vci.components.a = VK_COMPONENT_SWIZZLE_ONE;
        }
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->sview);
        if (s->nfaces == 6) {
            vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            vci.subresourceRange.layerCount = 6;
            T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->cube_view);
        }
        s->sview_levels = n;
        s->view_gen = ++g_view_gen;
    }
    *view = s->sview;
    *cube = s->cube_view;
}

/* The NV2A renders colour as A8R8G8B8 (X8R8G8B8): B8G8R8A8 here, its bytes
 * B G R A. A texture of another 8888 order over the target's memory reads
 * those bytes as its own channels (pbres.c k_zread: channel -> byte):
 * Mercenaries copies its scene depth into a colour target and looks its sky
 * dome's fade up by that copy's bytes, sampled as A8B8G8R8 (DPNDNT_GB); read
 * in the target's own order, the lookup took the stencil byte for the
 * depth's and the sky stayed black. */
VkImageView hle_surf_order_view(HleVkSurf *s, uint32_t ord)
{
    static const uint8_t k_byte[4][4] = {       /* ord: the texture's channel -> byte */
        { 2, 1, 0, 3 }, { 0, 1, 2, 3 }, { 1, 2, 3, 0 }, { 3, 2, 1, 0 } };
    static const VkComponentSwizzle k_chan[4] = {   /* byte -> the image's channel */
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_A };
    if (!ord || ord > HLE_DRAW_ORD_RGBA || s->d.parent || !s->img || s->fmt != VK_FORMAT_B8G8R8A8_UNORM ||
        !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        return VK_NULL_HANDLE;
    }
    const uint32_t n = MAX(MIN(MAX(s->rendered_levels, 1u), s->nlevels), 1u);
    if (s->oview && s->oview_ord == ord && s->oview_levels == n) {
        return s->oview;
    }
    retire_view(s->oview);
    s->oview = VK_NULL_HANDLE;
    VkComponentSwizzle c[4];
    for (unsigned i = 0; i < 4; i++) {
        c[i] = k_chan[k_byte[ord][i]];
        if (c[i] == VK_COMPONENT_SWIZZLE_A && hle_surf_x_alpha(s)) {
            c[i] = VK_COMPONENT_SWIZZLE_ONE;    /* X8R8G8B8: alpha 1, as its own view */
        }
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->fmt,
        .components = { c[0], c[1], c[2], c[3] },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, n, 0, 1 } };
    if (T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->oview) != VK_SUCCESS) {
        s->oview = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    s->oview_ord = ord;
    s->oview_levels = n;
    s->oview_gen = ++g_view_gen;
    return s->oview;
}

