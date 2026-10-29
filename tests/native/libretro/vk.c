/*
 * hakuX libretro core — the harness's Vulkan frontend (harness.c,
 * HARNESS_VULKAN=1), as RetroArch's vulkan driver is to the core, without a
 * window: an instance per game, the device the core makes through
 * create_device2 (the frontend's wrapper adding nothing), the render
 * interface with two sync indices and a queue lock, and each frame the core's
 * command buffers submitted with a fence per index. Every HV_PROBE frames the
 * image handed over is read back: its size and mean brightness are printed,
 * so a black or missing picture shows.
 */
#include "tests/native/libretro/vk.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define HV_SYNC 2
#define HV_PROBE 300

#define FN(X)                                                                                                \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetDeviceProcAddr)           \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue)         \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit)                 \
    X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)          \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)          \
    X(vkCmdCopyImageToBuffer) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)         \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory)

#define DECLARE(name) static PFN_##name name;
FN(DECLARE)
static PFN_vkGetInstanceProcAddr g_gipa;

static struct {
    VkInstance inst;
    VkPhysicalDevice gpu;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    pthread_mutex_t qlock;
    struct retro_hw_render_interface_vulkan iface;
    unsigned idx;
    VkFence fence[HV_SYNC];
    const VkCommandBuffer *cmds;   /* set_command_buffers', for the next submit */
    uint32_t ncmd;
    struct retro_vulkan_image image;
    bool have_image;
    unsigned waits;                /* semaphores set_image asked to wait on */
    /* the probe */
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
    unsigned long frames;
} g;

static void lock_queue(void *h)
{
    pthread_mutex_lock(&g.qlock);
}

static void unlock_queue(void *h)
{
    pthread_mutex_unlock(&g.qlock);
}

static void set_image(void *h, const struct retro_vulkan_image *image, uint32_t n, const VkSemaphore *sems,
                      uint32_t src_queue_family)
{
    g.image = *image;
    g.have_image = true;
    g.waits += n;
}

static uint32_t get_sync_index(void *h)
{
    return g.idx;
}

static uint32_t get_sync_index_mask(void *h)
{
    return (1u << HV_SYNC) - 1;
}

static void set_command_buffers(void *h, uint32_t n, const VkCommandBuffer *cmd)
{
    g.cmds = cmd;
    g.ncmd = n;
}

static void wait_sync_index(void *h)
{
    vkWaitForFences(g.dev, 1, &g.fence[g.idx], VK_TRUE, UINT64_MAX);
}

static void set_signal_semaphore(void *h, VkSemaphore s)
{
}

bool hv_load(void)
{
    if (g_gipa) {
        return true;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    g_gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!g_gipa) {
        fprintf(stderr, "vulkan: no libvulkan.so.1\n");
        return false;
    }
    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    pthread_mutex_init(&g.qlock, NULL);
    return vkCreateInstance != NULL;
}

PFN_vkGetInstanceProcAddr hv_gipa(void)
{
    return g_gipa;
}

static VkDevice make_device(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *ci)
{
    VkDevice d = VK_NULL_HANDLE;
    return vkCreateDevice(gpu, ci, NULL, &d) == VK_SUCCESS ? d : VK_NULL_HANDLE;
}

static bool probe_init(void)
{
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g.qfam};
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    g.size = 3840 * 2160 * 4;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = g.size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateCommandPool(g.dev, &pci, NULL, &g.pool) != VK_SUCCESS) {
        return false;
    }
    ai.commandPool = g.pool;
    if (vkAllocateCommandBuffers(g.dev, &ai, &g.cb) != VK_SUCCESS ||
        vkCreateBuffer(g.dev, &bci, NULL, &g.buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, g.buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t t = 0;
    while (t < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << t)) || (mp.memoryTypes[t].propertyFlags & want) != want)) {
        t++;
    }
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = t};
    return t < mp.memoryTypeCount && vkAllocateMemory(g.dev, &mai, NULL, &g.mem) == VK_SUCCESS &&
           vkBindBufferMemory(g.dev, g.buf, g.mem, 0) == VK_SUCCESS &&
           vkMapMemory(g.dev, g.mem, 0, VK_WHOLE_SIZE, 0, &g.map) == VK_SUCCESS;
}

bool hv_create(const struct retro_hw_render_context_negotiation_interface_vulkan *neg)
{
    const VkApplicationInfo *app = neg && neg->get_application_info ? neg->get_application_info() : NULL;
    VkApplicationInfo def = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = app ? app : &def};
    /* a global command: the last create's FN(LOAD) asked the instance for it (NULL) */
    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    if (vkCreateInstance(&ici, NULL, &g.inst) != VK_SUCCESS) {
        fprintf(stderr, "vulkan: no instance\n");
        return false;
    }
#define LOAD(name) name = (PFN_##name)g_gipa(g.inst, #name);
    FN(LOAD)
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(g.inst, &n, &g.gpu) < 0 || !n) {
        fprintf(stderr, "vulkan: no GPU\n");
        return false;
    }
    struct retro_vulkan_context ctx = {0};
    if (!neg || neg->interface_version < 2 || !neg->create_device2 ||
        !neg->create_device2(&ctx, g.inst, g.gpu, VK_NULL_HANDLE, g_gipa, make_device, NULL)) {
        fprintf(stderr, "vulkan: the core made no device\n");
        return false;
    }
    g.dev = ctx.device;
    g.queue = ctx.queue;
    g.qfam = ctx.queue_family_index;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (unsigned i = 0; i < HV_SYNC; i++) {
        vkCreateFence(g.dev, &fci, NULL, &g.fence[i]);
    }
    if (!probe_init()) {
        fprintf(stderr, "vulkan: no read-back buffer\n");
        return false;
    }
    g.iface = (struct retro_hw_render_interface_vulkan){
        .interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN,
        .interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION,
        .handle = &g, .instance = g.inst, .gpu = g.gpu, .device = g.dev,
        .get_device_proc_addr = vkGetDeviceProcAddr, .get_instance_proc_addr = g_gipa,
        .queue = g.queue, .queue_index = g.qfam,
        .set_image = set_image, .get_sync_index = get_sync_index, .get_sync_index_mask = get_sync_index_mask,
        .set_command_buffers = set_command_buffers, .wait_sync_index = wait_sync_index,
        .lock_queue = lock_queue, .unlock_queue = unlock_queue, .set_signal_semaphore = set_signal_semaphore};
    g.idx = 0;
    g.frames = 0;
    printf("vulkan: device made by the core, queue family %u\n", g.qfam);
    return true;
}

const struct retro_hw_render_interface_vulkan *hv_interface(void)
{
    return g.dev ? &g.iface : NULL;
}

void hv_frame(unsigned w, unsigned h)
{
    const VkFence f = g.fence[g.idx];
    vkResetFences(g.dev, 1, &f);
    const bool look = g.have_image && ++g.frames % HV_PROBE == 0 && (size_t)w * h * 4 <= g.size;
    VkCommandBuffer cmds[9];
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.ncmd && n < 8; i++) {
        cmds[n++] = g.cmds[i];
    }
    if (look) {
        const VkImage img = g.image.create_info.image;
        vkResetCommandBuffer(g.cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(g.cb, &bi);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = g.image.image_layout,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(g.cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.buf, 1, &r);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = g.image.image_layout;
        vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        vkEndCommandBuffer(g.cb);
        cmds[n++] = g.cb;
    }
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = n, .pCommandBuffers = cmds};
    lock_queue(NULL);
    const VkResult r = vkQueueSubmit(g.queue, 1, &si, f);
    unlock_queue(NULL);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vulkan: submit failed (%d)\n", r);
    }
    g.ncmd = 0;
    if (look) {
        vkWaitForFences(g.dev, 1, &f, VK_TRUE, UINT64_MAX);
        const uint8_t *p = g.map;
        uint64_t sum = 0, lit = 0;
        for (size_t i = 0; i < (size_t)w * h; i++, p += 4) {
            const unsigned y = (p[0] * 2 + p[1] * 5 + p[2]) / 8;
            sum += y;
            lit += y > 16;
        }
        printf("vulkan: frame %lu: %ux%u, mean brightness %.1f, %.1f%% lit\n", g.frames, w, h,
               (double)sum / ((double)w * h), 100.0 * (double)lit / ((double)w * h));
    }
    g.idx = (g.idx + 1) % HV_SYNC;
}

void hv_destroy(void)
{
    if (!g.dev) {
        return;
    }
    lock_queue(NULL);
    vkDeviceWaitIdle(g.dev);
    unlock_queue(NULL);
    for (unsigned i = 0; i < HV_SYNC; i++) {
        vkDestroyFence(g.dev, g.fence[i], NULL);
    }
    vkDestroyBuffer(g.dev, g.buf, NULL);
    vkFreeMemory(g.dev, g.mem, NULL);
    vkDestroyCommandPool(g.dev, g.pool, NULL);
    vkDestroyDevice(g.dev, NULL);
    vkDestroyInstance(g.inst, NULL);
    printf("vulkan: device and instance destroyed (%lu frames, %u semaphore waits asked)\n", g.frames, g.waits);
    const pthread_mutex_t m = g.qlock;
    memset(&g, 0, sizeof(g));
    g.qlock = m;
}
