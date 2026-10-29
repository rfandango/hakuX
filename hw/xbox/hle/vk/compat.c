/*
 * hakuX HLE GPU — Vulkan 1.3's entry points on drivers below it: Mali's
 * Valhall parts on the builds vendors cap at 1.1 or never updated (Samsung's
 * Exynos phones, older Dimensity firmware), PowerVR's BXM, Xclipse's first
 * firmware.
 *
 * The back end records Vulkan 1.3: dynamic rendering, synchronization2, the
 * copy commands 2, 1.2's timeline semaphores. Below 1.3 (HleCaps.vk13 off,
 * vkdev.c) the device table's entries for them are their extensions' where the
 * driver has them, else this file's, which record Vulkan 1.0's commands:
 * - vkCmdBeginRendering / vkCmdEndRendering: a render pass instance. The pass
 *   is cached by its attachments' formats, load and store operations and
 *   layouts, the framebuffer by the pass, the views and the render area's
 *   extent. A pass's initial and final layouts are the attachments' own: the
 *   back end moves images between layouts with its barriers, as it does for
 *   dynamic rendering, and an instance adds no dependency of its own (no
 *   layout transition, so no implicit external dependency either).
 * - vkCreateGraphicsPipelines: the VkPipelineRenderingCreateInfo heading the
 *   chain becomes a pass of the same formats, compatible with every instance
 *   the draws open on them (compatibility is formats and samples).
 * - vkCmdPipelineBarrier2 / vkQueueSubmit2: 1.0's barrier and submit, the
 *   stages and accesses mapped to 1.0's (compat_map.h), one stage mask per
 *   barrier call (the union of its barriers'), the timeline values through
 *   VkTimelineSemaphoreSubmitInfo.
 * - vkCmdBlitImage2 / vkCmdCopyImage2: 1.0's commands.
 * Extended dynamic state is not emulated here: without it the draws build the
 * state into their pipelines (vk/draw.c PipeKey.ds).
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/util/thread.h"
#include "hw/xbox/hle/vk/compat.h"
#include "hw/xbox/hle/vk/compat_map.h"

#define MAX_COLORS 4
#define MAX_ATT    (MAX_COLORS + 1)

static struct VolkDeviceTable R;    /* the device's entry points, as loaded */

/* An attachment as a pass describes it; fmt UNDEFINED: unused */
typedef struct RpAtt {
    uint32_t fmt, layout;
    uint8_t load, store, sload, sstore;
} RpAtt;

typedef struct RpKey {
    uint32_t ncol;
    RpAtt c[MAX_COLORS], d;
} RpKey;

typedef struct FbKey {
    VkRenderPass rp;
    VkImageView v[MAX_ATT];
    uint32_t n, w, h, layers;
} FbKey;

static struct {
    bool init;
    HleMutex lock;
    GHashTable *views;              /* VkImageView -> its format */
    GHashTable *passes;             /* RpKey -> VkRenderPass */
    GHashTable *fbs;                /* FbKey -> VkFramebuffer */
    GHashTable *att;                /* VkImageView -> framebuffers using it */
    uint64_t npass, nfb;
    bool said;                      /* the rendering not emulated logged */
} g_c;

/* the caches' keys: zeroed, then filled, so their bytes say all */
static guint key_hash(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static guint rp_hash(gconstpointer k)
{
    return key_hash(k, sizeof(RpKey));
}

static gboolean rp_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(RpKey)) == 0;
}

static guint fb_hash(gconstpointer k)
{
    return key_hash(k, sizeof(FbKey));
}

static gboolean fb_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(FbKey)) == 0;
}

static bool has_stencil(uint32_t f)
{
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT;
}

static bool has_depth(uint32_t f)
{
    return f != VK_FORMAT_S8_UINT;
}

/* k's pass, made the first time (under g_c.lock) */
static VkRenderPass pass_get(const RpKey *k)
{
    VkRenderPass rp = (VkRenderPass)g_hash_table_lookup(g_c.passes, k);
    if (rp) {
        return rp;
    }
    VkAttachmentDescription a[MAX_ATT];
    VkAttachmentReference cr[MAX_COLORS], dr;
    uint32_t na = 0;
    for (uint32_t i = 0; i < k->ncol; i++) {
        const RpAtt *c = &k->c[i];
        if (!c->fmt) {
            cr[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
            continue;
        }
        a[na] = (VkAttachmentDescription){
            .format = (VkFormat)c->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = (VkAttachmentLoadOp)c->load, .storeOp = (VkAttachmentStoreOp)c->store,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = (VkImageLayout)c->layout, .finalLayout = (VkImageLayout)c->layout };
        cr[i] = (VkAttachmentReference){ na++, (VkImageLayout)c->layout };
    }
    if (k->d.fmt) {
        a[na] = (VkAttachmentDescription){
            .format = (VkFormat)k->d.fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = (VkAttachmentLoadOp)k->d.load, .storeOp = (VkAttachmentStoreOp)k->d.store,
            .stencilLoadOp = (VkAttachmentLoadOp)k->d.sload,
            .stencilStoreOp = (VkAttachmentStoreOp)k->d.sstore,
            .initialLayout = (VkImageLayout)k->d.layout, .finalLayout = (VkImageLayout)k->d.layout };
        dr = (VkAttachmentReference){ na++, (VkImageLayout)k->d.layout };
    }
    VkSubpassDescription sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = k->ncol, .pColorAttachments = k->ncol ? cr : NULL,
        .pDepthStencilAttachment = k->d.fmt ? &dr : NULL };
    VkRenderPassCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = na, .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp };
    VkResult r = R.vkCreateRenderPass(g_hvk.dev, &ci, NULL, &rp);
    if (r != VK_SUCCESS) {
        hle_vk_check(r, "vkCreateRenderPass (compat)");
        return VK_NULL_HANDLE;
    }
    g_hash_table_insert(g_c.passes, g_memdup2(k, sizeof(*k)), rp);
    g_c.npass++;
    return rp;
}

/* (under g_c.lock) */
static VkFramebuffer fb_get(const FbKey *k)
{
    VkFramebuffer fb = (VkFramebuffer)g_hash_table_lookup(g_c.fbs, k);
    if (fb) {
        return fb;
    }
    VkFramebufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = k->rp,
        .attachmentCount = k->n, .pAttachments = k->v,
        .width = k->w, .height = k->h, .layers = k->layers };
    VkResult r = R.vkCreateFramebuffer(g_hvk.dev, &ci, NULL, &fb);
    if (r != VK_SUCCESS) {
        hle_vk_check(r, "vkCreateFramebuffer (compat)");
        return VK_NULL_HANDLE;
    }
    g_hash_table_insert(g_c.fbs, g_memdup2(k, sizeof(*k)), fb);
    for (uint32_t i = 0; i < k->n; i++) {
        uintptr_t n = (uintptr_t)g_hash_table_lookup(g_c.att, k->v[i]);
        g_hash_table_insert(g_c.att, k->v[i], (gpointer)(n + 1));
    }
    g_c.nfb++;
    return fb;
}

static uint32_t view_format(VkImageView v)
{
    return (uint32_t)(uintptr_t)g_hash_table_lookup(g_c.views, v);
}

static void VKAPI_CALL c_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    RpKey k;
    FbKey f;
    VkClearValue cv[MAX_ATT];
    memset(&k, 0, sizeof(k));
    memset(&f, 0, sizeof(f));
    if (ri->flags || ri->viewMask || ri->colorAttachmentCount > MAX_COLORS) {
        if (!g_c.said) {
            g_c.said = true;
            hle_log("hakuX-hle-back", "compat: rendering flags %x, view mask %x, %u colours "
                    "not emulated", ri->flags, ri->viewMask, ri->colorAttachmentCount);
        }
    }
    hle_mutex_lock(&g_c.lock);
    k.ncol = MIN(ri->colorAttachmentCount, (uint32_t)MAX_COLORS);
    for (uint32_t i = 0; i < k.ncol; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        if (!a->imageView) {
            continue;
        }
        k.c[i] = (RpAtt){ .fmt = view_format(a->imageView), .layout = a->imageLayout,
                          .load = (uint8_t)a->loadOp, .store = (uint8_t)a->storeOp };
        cv[f.n] = a->clearValue;
        f.v[f.n++] = a->imageView;
    }
    /* one depth/stencil attachment for both: an aspect the rendering leaves out
     * is loaded and stored (dynamic rendering doesn't touch it) */
    const VkRenderingAttachmentInfo *da = ri->pDepthAttachment, *sa = ri->pStencilAttachment;
    da = da && da->imageView ? da : NULL;
    sa = sa && sa->imageView ? sa : NULL;
    if (da || sa) {
        const VkRenderingAttachmentInfo *a = da ? da : sa;
        k.d.fmt = view_format(a->imageView);
        k.d.layout = a->imageLayout;
        k.d.load = (uint8_t)(da ? da->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD);
        k.d.store = (uint8_t)(da ? da->storeOp : VK_ATTACHMENT_STORE_OP_STORE);
        k.d.sload = (uint8_t)(sa ? sa->loadOp : VK_ATTACHMENT_LOAD_OP_LOAD);
        k.d.sstore = (uint8_t)(sa ? sa->storeOp : VK_ATTACHMENT_STORE_OP_STORE);
        if (!has_depth(k.d.fmt)) {
            k.d.load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.d.store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        if (!has_stencil(k.d.fmt)) {
            k.d.sload = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.d.sstore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        cv[f.n].depthStencil.depth = da ? da->clearValue.depthStencil.depth : 0.0f;
        cv[f.n].depthStencil.stencil = sa ? sa->clearValue.depthStencil.stencil : 0;
        f.v[f.n++] = a->imageView;
    }
    f.rp = pass_get(&k);
    /* the render area lies within every attachment */
    f.w = MAX((uint32_t)ri->renderArea.offset.x + ri->renderArea.extent.width, 1u);
    f.h = MAX((uint32_t)ri->renderArea.offset.y + ri->renderArea.extent.height, 1u);
    f.layers = MAX(ri->layerCount, 1u);
    VkFramebuffer fb = f.rp ? fb_get(&f) : VK_NULL_HANDLE;
    hle_mutex_unlock(&g_c.lock);
    if (!fb) {
        return;
    }
    VkRenderPassBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = f.rp,
        .framebuffer = fb, .renderArea = ri->renderArea, .clearValueCount = f.n,
        .pClearValues = cv };
    R.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

static void VKAPI_CALL c_end_rendering(VkCommandBuffer cb)
{
    R.vkCmdEndRenderPass(cb);
}

static VkResult VKAPI_CALL c_create_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                              const VkGraphicsPipelineCreateInfo *ci,
                                              const VkAllocationCallbacks *al, VkPipeline *out)
{
    enum { N = 8 };
    VkGraphicsPipelineCreateInfo c[N];
    if (n > N) {
        return R.vkCreateGraphicsPipelines(dev, cache, n, ci, al, out);
    }
    for (uint32_t i = 0; i < n; i++) {
        c[i] = ci[i];
        const VkPipelineRenderingCreateInfo *ri = ci[i].pNext;
        if (ci[i].renderPass || !ri || ri->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            continue;
        }
        /* the formats matter, not the operations or layouts */
        RpKey k;
        memset(&k, 0, sizeof(k));
        k.ncol = MIN(ri->colorAttachmentCount, (uint32_t)MAX_COLORS);
        for (uint32_t j = 0; j < k.ncol; j++) {
            k.c[j] = (RpAtt){ .fmt = ri->pColorAttachmentFormats[j],
                              .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .load = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                              .store = VK_ATTACHMENT_STORE_OP_DONT_CARE };
        }
        uint32_t df = ri->depthAttachmentFormat ? ri->depthAttachmentFormat
                                                : ri->stencilAttachmentFormat;
        if (df) {
            k.d = (RpAtt){ .fmt = df, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           .load = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           .store = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           .sload = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           .sstore = VK_ATTACHMENT_STORE_OP_DONT_CARE };
        }
        hle_mutex_lock(&g_c.lock);
        c[i].renderPass = pass_get(&k);
        hle_mutex_unlock(&g_c.lock);
        c[i].subpass = 0;
        c[i].pNext = ri->pNext;
    }
    return R.vkCreateGraphicsPipelines(dev, cache, n, c, al, out);
}

static VkResult VKAPI_CALL c_create_view(VkDevice dev, const VkImageViewCreateInfo *ci,
                                         const VkAllocationCallbacks *al, VkImageView *v)
{
    VkResult r = R.vkCreateImageView(dev, ci, al, v);
    if (r == VK_SUCCESS) {
        hle_mutex_lock(&g_c.lock);
        g_hash_table_insert(g_c.views, *v, (gpointer)(uintptr_t)ci->format);
        hle_mutex_unlock(&g_c.lock);
    }
    return r;
}

typedef struct FbDrop {
    VkImageView v;
    FbKey **k;
    uint32_t n, cap;
} FbDrop;

static void fb_find(gpointer key, gpointer value, gpointer opaque)
{
    FbKey *k = key;
    FbDrop *d = opaque;
    for (uint32_t i = 0; i < k->n; i++) {
        if (k->v[i] == d->v) {
            if (d->n == d->cap) {
                d->cap = d->cap ? 2 * d->cap : 8;
                d->k = g_renew(FbKey *, d->k, d->cap);
            }
            d->k[d->n++] = k;
            return;
        }
    }
}

/* A view goes with the framebuffers that use it: it is destroyed once nothing
 * recorded uses it, so they aren't either. */
static void VKAPI_CALL c_destroy_view(VkDevice dev, VkImageView v, const VkAllocationCallbacks *al)
{
    if (v) {
        hle_mutex_lock(&g_c.lock);
        g_hash_table_remove(g_c.views, v);
        if (g_hash_table_lookup(g_c.att, v)) {
            FbDrop d = { .v = v };
            g_hash_table_foreach(g_c.fbs, fb_find, &d);
            for (uint32_t i = 0; i < d.n; i++) {
                FbKey *k = d.k[i];
                R.vkDestroyFramebuffer(dev, (VkFramebuffer)g_hash_table_lookup(g_c.fbs, k), NULL);
                for (uint32_t j = 0; j < k->n; j++) {
                    uintptr_t n = (uintptr_t)g_hash_table_lookup(g_c.att, k->v[j]);
                    if (n > 1) {
                        g_hash_table_insert(g_c.att, k->v[j], (gpointer)(n - 1));
                    } else {
                        g_hash_table_remove(g_c.att, k->v[j]);
                    }
                }
                g_hash_table_remove(g_c.fbs, k);    /* frees k */
            }
            g_free(d.k);
        }
        hle_mutex_unlock(&g_c.lock);
    }
    R.vkDestroyImageView(dev, v, al);
}

static void VKAPI_CALL c_barrier2(VkCommandBuffer cb, const VkDependencyInfo *d)
{
    enum { N = 16 };
    uint32_t im = 0, ib = 0, ii = 0;
    while (im < d->memoryBarrierCount || ib < d->bufferMemoryBarrierCount ||
           ii < d->imageMemoryBarrierCount) {
        VkMemoryBarrier mb[N];
        VkBufferMemoryBarrier bb[N];
        VkImageMemoryBarrier xb[N];
        uint32_t nm = 0, nb = 0, ni = 0;
        VkPipelineStageFlags src = 0, dst = 0;
        for (; im < d->memoryBarrierCount && nm < N; im++, nm++) {
            const VkMemoryBarrier2 *b = &d->pMemoryBarriers[im];
            src |= hle_compat_stages(b->srcStageMask);
            dst |= hle_compat_stages(b->dstStageMask);
            mb[nm] = (VkMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = hle_compat_access(b->srcAccessMask),
                .dstAccessMask = hle_compat_access(b->dstAccessMask) };
        }
        for (; ib < d->bufferMemoryBarrierCount && nb < N; ib++, nb++) {
            const VkBufferMemoryBarrier2 *b = &d->pBufferMemoryBarriers[ib];
            src |= hle_compat_stages(b->srcStageMask);
            dst |= hle_compat_stages(b->dstStageMask);
            bb[nb] = (VkBufferMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = hle_compat_access(b->srcAccessMask),
                .dstAccessMask = hle_compat_access(b->dstAccessMask),
                .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
                .dstQueueFamilyIndex = b->dstQueueFamilyIndex,
                .buffer = b->buffer, .offset = b->offset, .size = b->size };
        }
        for (; ii < d->imageMemoryBarrierCount && ni < N; ii++, ni++) {
            const VkImageMemoryBarrier2 *b = &d->pImageMemoryBarriers[ii];
            src |= hle_compat_stages(b->srcStageMask);
            dst |= hle_compat_stages(b->dstStageMask);
            xb[ni] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = hle_compat_access(b->srcAccessMask),
                .dstAccessMask = hle_compat_access(b->dstAccessMask),
                .oldLayout = b->oldLayout, .newLayout = b->newLayout,
                .srcQueueFamilyIndex = b->srcQueueFamilyIndex,
                .dstQueueFamilyIndex = b->dstQueueFamilyIndex,
                .image = b->image, .subresourceRange = b->subresourceRange };
        }
        R.vkCmdPipelineBarrier(cb, hle_compat_src(src), hle_compat_dst(dst), d->dependencyFlags,
                               nm, mb, nb, bb, ni, xb);
    }
}

static VkResult VKAPI_CALL c_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence fence)
{
    enum { NS = 4, NE = 8 };
    VkSubmitInfo si[NS];
    VkTimelineSemaphoreSubmitInfo ti[NS];
    VkSemaphore ws[NS][NE], ss[NS][NE];
    uint64_t wv[NS][NE], sv[NS][NE];
    VkPipelineStageFlags wst[NS][NE];
    VkCommandBuffer cbs[NS][NE];
    if (n > NS) {
        hle_log("hakuX-hle-back", "compat: %u submits in one call (%u emulated)", n, NS);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < n; i++) {
        const VkSubmitInfo2 *x = &s[i];
        if (x->waitSemaphoreInfoCount > NE || x->commandBufferInfoCount > NE ||
            x->signalSemaphoreInfoCount > NE) {
            hle_log("hakuX-hle-back", "compat: a submit of %u waits, %u command buffers, %u "
                    "signals (%u each emulated)", x->waitSemaphoreInfoCount,
                    x->commandBufferInfoCount, x->signalSemaphoreInfoCount, NE);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        for (uint32_t j = 0; j < x->waitSemaphoreInfoCount; j++) {
            const VkSemaphoreSubmitInfo *w = &x->pWaitSemaphoreInfos[j];
            VkPipelineStageFlags st = hle_compat_stages(w->stageMask);
            ws[i][j] = w->semaphore;
            wv[i][j] = w->value;
            wst[i][j] = st ? st : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        for (uint32_t j = 0; j < x->commandBufferInfoCount; j++) {
            cbs[i][j] = x->pCommandBufferInfos[j].commandBuffer;
        }
        for (uint32_t j = 0; j < x->signalSemaphoreInfoCount; j++) {
            ss[i][j] = x->pSignalSemaphoreInfos[j].semaphore;
            sv[i][j] = x->pSignalSemaphoreInfos[j].value;
        }
        ti[i] = (VkTimelineSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = x->waitSemaphoreInfoCount, .pWaitSemaphoreValues = wv[i],
            .signalSemaphoreValueCount = x->signalSemaphoreInfoCount,
            .pSignalSemaphoreValues = sv[i] };
        si[i] = (VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti[i],
            .waitSemaphoreCount = x->waitSemaphoreInfoCount, .pWaitSemaphores = ws[i],
            .pWaitDstStageMask = wst[i],
            .commandBufferCount = x->commandBufferInfoCount, .pCommandBuffers = cbs[i],
            .signalSemaphoreCount = x->signalSemaphoreInfoCount, .pSignalSemaphores = ss[i] };
    }
    return R.vkQueueSubmit(q, n, si, fence);
}

static void VKAPI_CALL c_blit2(VkCommandBuffer cb, const VkBlitImageInfo2 *b)
{
    enum { N = 16 };
    VkImageBlit r[N];
    for (uint32_t i0 = 0; i0 < b->regionCount; i0 += N) {
        uint32_t n = MIN((uint32_t)N, b->regionCount - i0);
        for (uint32_t i = 0; i < n; i++) {
            const VkImageBlit2 *x = &b->pRegions[i0 + i];
            r[i].srcSubresource = x->srcSubresource;
            r[i].dstSubresource = x->dstSubresource;
            memcpy(r[i].srcOffsets, x->srcOffsets, sizeof(r[i].srcOffsets));
            memcpy(r[i].dstOffsets, x->dstOffsets, sizeof(r[i].dstOffsets));
        }
        R.vkCmdBlitImage(cb, b->srcImage, b->srcImageLayout, b->dstImage, b->dstImageLayout, n, r,
                         b->filter);
    }
}

static void VKAPI_CALL c_copy2(VkCommandBuffer cb, const VkCopyImageInfo2 *c)
{
    enum { N = 16 };
    VkImageCopy r[N];
    for (uint32_t i0 = 0; i0 < c->regionCount; i0 += N) {
        uint32_t n = MIN((uint32_t)N, c->regionCount - i0);
        for (uint32_t i = 0; i < n; i++) {
            const VkImageCopy2 *x = &c->pRegions[i0 + i];
            r[i] = (VkImageCopy){ x->srcSubresource, x->srcOffset, x->dstSubresource,
                                  x->dstOffset, x->extent };
        }
        R.vkCmdCopyImage(cb, c->srcImage, c->srcImageLayout, c->dstImage, c->dstImageLayout, n, r);
    }
}

void hle_compat_install(struct VolkDeviceTable *t)
{
    const HleCaps *c = &g_hvk.caps;
    if (!g_c.init) {
        R = *t;
        hle_mutex_init(&g_c.lock);
        g_c.views = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_c.att = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_c.passes = g_hash_table_new_full(rp_hash, rp_equal, g_free, NULL);
        g_c.fbs = g_hash_table_new_full(fb_hash, fb_equal, g_free, NULL);
        g_c.init = true;
        if (!c->vk13 || !c->dyn_render || !c->sync2) {
            hle_log("hakuX-hle-back", "compat: Vulkan %s, %s, %s", c->vk13 ? "1.3" : "1.1",
                    c->dyn_render ? "dynamic rendering" : "render passes",
                    c->sync2 ? "synchronization2" : "1.0 barriers and submits");
        }
    }
    if (!c->vk13) {
        /* 1.2's from their extensions (the floor below 1.3), else core 1.2's */
#define PICK(core, ext) t->core = R.ext ? R.ext : R.core
        PICK(vkWaitSemaphores, vkWaitSemaphoresKHR);
        PICK(vkSignalSemaphore, vkSignalSemaphoreKHR);
        PICK(vkGetSemaphoreCounterValue, vkGetSemaphoreCounterValueKHR);
        PICK(vkGetBufferDeviceAddress, vkGetBufferDeviceAddressKHR);
        PICK(vkResetQueryPool, vkResetQueryPoolEXT);
#undef PICK
        t->vkCmdBlitImage2 = c_blit2;
        t->vkCmdCopyImage2 = c_copy2;
        /* the draws record these only with the caps on (vk/draw.c) */
        t->vkCmdSetCullMode = c->eds1 ? R.vkCmdSetCullModeEXT : NULL;
        t->vkCmdSetFrontFace = c->eds1 ? R.vkCmdSetFrontFaceEXT : NULL;
        t->vkCmdSetPrimitiveTopology = c->eds1 ? R.vkCmdSetPrimitiveTopologyEXT : NULL;
        t->vkCmdSetDepthTestEnable = c->eds1 ? R.vkCmdSetDepthTestEnableEXT : NULL;
        t->vkCmdSetDepthWriteEnable = c->eds1 ? R.vkCmdSetDepthWriteEnableEXT : NULL;
        t->vkCmdSetDepthCompareOp = c->eds1 ? R.vkCmdSetDepthCompareOpEXT : NULL;
        t->vkCmdSetStencilTestEnable = c->eds1 ? R.vkCmdSetStencilTestEnableEXT : NULL;
        t->vkCmdSetStencilOp = c->eds1 ? R.vkCmdSetStencilOpEXT : NULL;
        t->vkCmdBindVertexBuffers2 = c->eds1 ? R.vkCmdBindVertexBuffers2EXT : NULL;
        t->vkCmdSetDepthBiasEnable = c->eds2 ? R.vkCmdSetDepthBiasEnableEXT : NULL;
        t->vkCmdSetPrimitiveRestartEnable = c->eds2 ? R.vkCmdSetPrimitiveRestartEnableEXT : NULL;
        if (c->dyn_render) {
            t->vkCmdBeginRendering = R.vkCmdBeginRenderingKHR;
            t->vkCmdEndRendering = R.vkCmdEndRenderingKHR;
        }
        if (c->sync2) {
            t->vkCmdPipelineBarrier2 = R.vkCmdPipelineBarrier2KHR;
            t->vkQueueSubmit2 = R.vkQueueSubmit2KHR;
        }
    }
    if (!c->dyn_render) {
        t->vkCmdBeginRendering = c_begin_rendering;
        t->vkCmdEndRendering = c_end_rendering;
        t->vkCreateGraphicsPipelines = c_create_pipelines;
        t->vkCreateImageView = c_create_view;
        t->vkDestroyImageView = c_destroy_view;
    }
    if (!c->sync2) {
        t->vkCmdPipelineBarrier2 = c_barrier2;
        t->vkQueueSubmit2 = c_submit2;
    }
}

static void fb_destroy(gpointer key, gpointer value, gpointer opaque)
{
    R.vkDestroyFramebuffer(g_hvk.dev, (VkFramebuffer)value, NULL);
}

static void pass_destroy(gpointer key, gpointer value, gpointer opaque)
{
    R.vkDestroyRenderPass(g_hvk.dev, (VkRenderPass)value, NULL);
}

void hle_compat_shutdown(void)
{
    if (!g_c.init) {
        return;
    }
    /* a view still known is its owner's: its framebuffers go here, but its
     * destroy after this would find no tables (the owners' shutdowns first) */
    const guint live = g_hash_table_size(g_c.views);
    if (live) {
        hle_log("hakuX-hle-back", "compat: %u image views not destroyed before the stop", live);
    }
    g_hash_table_foreach(g_c.fbs, fb_destroy, NULL);
    g_hash_table_foreach(g_c.passes, pass_destroy, NULL);
    g_hash_table_destroy(g_c.fbs);      /* frees the keys */
    g_hash_table_destroy(g_c.passes);
    g_hash_table_destroy(g_c.att);
    g_hash_table_destroy(g_c.views);
    hle_mutex_destroy(&g_c.lock);
    memset(&g_c, 0, sizeof(g_c));
    memset(&R, 0, sizeof(R));           /* the next install loads the next device's */
}
