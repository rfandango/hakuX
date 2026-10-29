/*
 * hakuX native core — the libretro core's Vulkan:
 * the renderer on RetroArch's instance and device, and each frame handed to
 * RetroArch as an image it samples.
 *
 * The device: create_device2 builds the renderer's device on RetroArch's
 * instance and GPU (hle_vk_init_shared), through RetroArch's wrapper, which
 * adds what RetroArch needs itself. The queue is shared (the Adreno 740 has
 * one): every submission of the renderer takes RetroArch's lock_queue inside
 * its own (hle_vk_queue_lock). The guest boots once RetroArch has taken the
 * device (context_reset).
 *
 * Each retro_run, after wait_sync_index: the front display slot is copied,
 * without its overscan, into the sync index's own image, the copy handed over
 * with set_command_buffers (RetroArch submits it ahead of its own work, in
 * video_cb) and the image with set_image. Every frame is copied, new or not,
 * so no image is shown across sync indices. The slot stays held from its
 * acquisition until video_cb has submitted the copy, as the window's
 * presenter holds it; the renderer's next write to it waits on its barrier.
 *
 * RetroArch letting the context go stops the core (nc_lr_end, M2): the
 * renderer's objects destroyed, the device forgotten (RetroArch destroys
 * it) and the next create_device2 makes another.
 */
#include "native/libretro/libretro_int.h"
#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/display.h"
#include "native/libretro/libretro_vulkan.h"

#include <string.h>

#define MAX_SYNC 16

static const struct retro_hw_render_interface_vulkan *g_vk;   /* NULL: no handoff */
static bool g_hw;                   /* SET_HW_RENDER taken: the frames go through Vulkan */
static uint32_t g_mask;             /* the sync indices the images were made for */
static VkCommandPool g_pool;
static struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkCommandBuffer cb;
    uint32_t w, h;
    struct retro_vulkan_image ri;   /* set_image's: valid until video_cb returns */
} g_ex[MAX_SYNC];

#define T (g_hvk.t)

static const VkApplicationInfo *application_info(void)
{
    static const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hakuX",
        .pEngineName = "hakuX",
        .apiVersion = VK_API_VERSION_1_3,       /* the renderer's floor */
    };
    return &app;
}

struct maker {
    retro_vulkan_create_device_wrapper_t wrap;
    void *opaque;
};

static VkDevice make_device(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *ci)
{
    const struct maker *m = opaque;
    return m->wrap(gpu, m->opaque, ci);
}

static bool create_device2(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
                           VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr gipa,
                           retro_vulkan_create_device_wrapper_t wrap, void *opaque)
{
    if (g_hvk.dev && g_hvk.inst != instance) {
        nc_log("hakuX-libretro", "vulkan: a second device for RetroArch: not before the core can stop");
        return false;
    }
    struct maker m = {wrap, opaque};
    const int r = hle_vk_init_shared(gipa, instance, gpu, surface, make_device, &m);
    if (r != 0) {
        nc_log("hakuX-libretro", "vulkan: no device for the renderer (%d)%s%s", r, r == -2 ? ": missing " : "",
               r == -2 ? g_hvk.caps.missing : "");
        return false;
    }
    context->gpu = g_hvk.phys;
    context->device = g_hvk.dev;
    context->queue = g_hvk.queue;
    context->queue_family_index = g_hvk.qfam;
    context->presentation_queue = g_hvk.queue;
    context->presentation_queue_family_index = g_hvk.qfam;
    nc_log("hakuX-libretro", "vulkan: the renderer's device on RetroArch's instance, queue family %u", g_hvk.qfam);
    return true;
}

/* v1, which can't express the renderer's features: declined. RetroArch only
 * negotiates when it is set, and then calls create_device2. */
static bool create_device(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
                          VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr gipa, const char **exts, unsigned nexts,
                          const char **layers, unsigned nlayers, const VkPhysicalDeviceFeatures *features)
{
    nc_log("hakuX-libretro", "vulkan: the frontend asked for a v1 device: not enough for the renderer");
    return false;
}

static void destroy_device(void)
{
}

static void destroy_images(void)
{
    for (unsigned i = 0; i < MAX_SYNC; i++) {
        if (g_ex[i].view) {
            T.vkDestroyImageView(g_hvk.dev, g_ex[i].view, NULL);
        }
        if (g_ex[i].img) {
            T.vkDestroyImage(g_hvk.dev, g_ex[i].img, NULL);
        }
        if (g_ex[i].mem) {
            T.vkFreeMemory(g_hvk.dev, g_ex[i].mem, NULL);
        }
        g_ex[i].view = VK_NULL_HANDLE;
        g_ex[i].img = VK_NULL_HANDLE;
        g_ex[i].mem = VK_NULL_HANDLE;
        g_ex[i].w = g_ex[i].h = 0;
    }
}

/* The frames' images, command buffers and pool: the device idle. */
static void release(void)
{
    destroy_images();
    for (unsigned i = 0; i < MAX_SYNC; i++) {
        g_ex[i].cb = VK_NULL_HANDLE;
    }
    T.vkDestroyCommandPool(g_hvk.dev, g_pool, NULL);
    g_pool = VK_NULL_HANDLE;
    g_mask = 0;
}

static void context_reset(void)
{
    const struct retro_hw_render_interface_vulkan *vk = NULL;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void *)&vk) || !vk ||
        vk->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        vk->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
        nc_lr_fail("RetroArch gave no Vulkan render interface");
        return;
    }
    if (vk->device != g_hvk.dev || !g_hvk.dev) {
        nc_lr_fail("RetroArch did not take the renderer's Vulkan device");
        return;
    }
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_hvk.qfam};
    if (T.vkCreateCommandPool(g_hvk.dev, &pci, NULL, &g_pool) != VK_SUCCESS) {
        nc_lr_fail("no command pool for the frames");
        return;
    }
    hle_vk_set_frontend_queue_lock(vk->lock_queue, vk->unlock_queue, vk->handle);
    g_vk = vk;
    nc_lr_start();          /* the guest boots on the device RetroArch has now */
}

static void context_destroy(void)
{
    if (!g_vk) {
        return;
    }
    /* The core stops off the device RetroArch is letting go (a video
     * driver rebuild, or the unload, which comes after this); or, with
     * HAKUX_LR_STOP=0, RetroArch's process ends. */
    nc_lr_end("vulkan: RetroArch destroyed the context");
    if (!g_vk) {
        return;
    }
    hle_vk_queue_lock();
    T.vkDeviceWaitIdle(g_hvk.dev);
    hle_vk_queue_unlock();
    release();
    hle_vk_set_frontend_queue_lock(NULL, NULL, NULL);
    hle_vk_shutdown();      /* RetroArch's to destroy */
    g_vk = NULL;
}

void nc_lr_vk_reset(void)
{
    g_hw = false;           /* the next load negotiates again (a frontend without Vulkan reads back) */
    hle_vk_shutdown();      /* the renderer's own device (frames read back) */
}

bool nc_lr_vk_negotiate(void)
{
    static struct retro_hw_render_callback hw = {
        .context_type = RETRO_HW_CONTEXT_VULKAN,
        .version_major = VK_API_VERSION_1_3,
        .context_reset = context_reset,
        .context_destroy = context_destroy,
        /* A reinit with the context cached keeps the device but rebuilds
         * RetroArch's driver, and with it the render interface and the
         * queue lock's handle the core holds: uncached, RetroArch lets the
         * context go (the core stops) and makes another (it boots again). */
        .cache_context = false,
    };
    static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
        .interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
        .interface_version = 2,
        .get_application_info = application_info,
        .create_device = create_device,
        .destroy_device = destroy_device,
        .create_device2 = create_device2,
    };
    struct retro_hw_render_context_negotiation_interface support = {
        .interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN};
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support) ||
        support.interface_version < 2) {
        nc_log("hakuX-libretro", "vulkan: the frontend has no device negotiation (v2): frames read back");
        return false;
    }
    if (!nc_lr_env(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw)) {
        nc_log("hakuX-libretro", "vulkan: the frontend has no Vulkan render: frames read back");
        return false;
    }
    if (!nc_lr_env(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void *)&iface)) {
        nc_lr_fail("RetroArch refused the Vulkan device negotiation");
        return false;
    }
    g_hw = true;
    return true;
}

/* idx's image, w x h: made again when its size changed (wait_sync_index has
 * returned for idx: RetroArch is done with the old one). */
static bool export_image(unsigned idx, uint32_t w, uint32_t h)
{
    if (g_ex[idx].img && g_ex[idx].w == w && g_ex[idx].h == h) {
        return true;
    }
    if (g_ex[idx].view) {
        T.vkDestroyImageView(g_hvk.dev, g_ex[idx].view, NULL);
        T.vkDestroyImage(g_hvk.dev, g_ex[idx].img, NULL);
        T.vkFreeMemory(g_hvk.dev, g_ex[idx].mem, NULL);
        g_ex[idx].view = VK_NULL_HANDLE;
        g_ex[idx].img = VK_NULL_HANDLE;
        g_ex[idx].mem = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &g_ex[idx].img) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr;
    T.vkGetImageMemoryRequirements(g_hvk.dev, g_ex[idx].img, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    mt = mt < 0 ? hle_vk_memtype(mr.memoryTypeBits, 0, 0) : mt;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)mt};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_ex[idx].img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ex[idx].mem) != VK_SUCCESS ||
        T.vkBindImageMemory(g_hvk.dev, g_ex[idx].img, g_ex[idx].mem, 0) != VK_SUCCESS ||
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &g_ex[idx].view) != VK_SUCCESS) {
        return false;
    }
    g_ex[idx].ri = (struct retro_vulkan_image){g_ex[idx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, vci};
    g_ex[idx].w = w;
    g_ex[idx].h = h;
    if (!g_ex[idx].cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        if (T.vkAllocateCommandBuffers(g_hvk.dev, &ai, &g_ex[idx].cb) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool nc_lr_vk_run(retro_video_refresh_t video)
{
    if (!g_hw) {
        return false;
    }
    if (!g_vk) {
        video(NULL, 640, 480, 0);   /* the device isn't up: RetroArch shows what it has */
        return true;
    }
    const unsigned mask = g_vk->get_sync_index_mask(g_vk->handle);
    if (mask != g_mask) {           /* RetroArch's images changed: it is idle (the contract) */
        destroy_images();
        g_mask = mask;
    }
    const unsigned idx = g_vk->get_sync_index(g_vk->handle);
    g_vk->wait_sync_index(g_vk->handle);

    hle_disp_use_begin();
    HleDispFrame f;
    nc_lr_video_pickup();
    if (idx >= MAX_SYNC || !hle_disp_acquire(&f)) {
        hle_disp_use_end();
        video(NULL, 640, 480, 0);   /* no frame yet (the boot) */
        return true;
    }
    uint32_t cx, cy;
    nc_lr_overscan(f.w, f.h, &cx, &cy);
    const uint32_t ow = f.w - 2 * cx, oh = f.h - 2 * cy;
    if (!export_image(idx, ow, oh)) {
        hle_disp_release(f.slot);
        hle_disp_use_end();
        nc_lr_fail("no image for RetroArch");
        video(NULL, ow, oh, 0);
        return true;
    }
    VkCommandBuffer cb = g_ex[idx].cb;
    T.vkResetCommandBuffer(cb, 0);
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    T.vkBeginCommandBuffer(cb, &bi);
    hle_img_barrier(cb, f.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    hle_img_barrier(cb, g_ex[idx].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .srcOffset = {(int32_t)cx, (int32_t)cy, 0},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .extent = {ow, oh, 1}};
    T.vkCmdCopyImage(cb, f.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_ex[idx].img,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    hle_img_barrier(cb, f.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    hle_img_barrier(cb, g_ex[idx].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    T.vkEndCommandBuffer(cb);

    nc_lr_video_size(f.w, f.h, ow, oh);
    g_vk->set_command_buffers(g_vk->handle, 1, &cb);
    g_vk->set_image(g_vk->handle, &g_ex[idx].ri, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
    video(RETRO_HW_FRAME_BUFFER_VALID, ow, oh, 0);   /* RetroArch submits the copy here */
    hle_disp_release(f.slot);
    hle_disp_use_end();
    return true;
}
