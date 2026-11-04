/*
 * hakuX HLE GPU — Vulkan instance/device, floor + plus capability tiers.
 */
#include "hw/xbox/hle/base.h"

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/vk/vkdev.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/batch.h"
#include "hw/xbox/hle/vk/compat.h"
#include "hw/xbox/hle/vk/quirks.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif

HleVk g_hvk;
static HleMutex g_queue_lock;
static bool g_ready;
static bool g_lost;                     /* hle_vk_lose */
static int64_t g_lock_wait_ns;          /* hle_vk_queue_wait_ns */

const char *hle_vk_result_text(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: {
        static __thread char s[24];
        snprintf(s, sizeof(s), "VkResult %d", (int)r);
        return s;
    }
    }
}

void hle_vk_lose(VkResult r, const char *what)
{
    if (__atomic_exchange_n(&g_lost, true, __ATOMIC_ACQ_REL)) {
        return;
    }
    char why[160];
    snprintf(why, sizeof(why), "stopped responding (%s in %s)", hle_vk_result_text(r), what);
    hle_log("hakuX-hle-back", "FATAL: the GPU %s: nothing more is drawn", why);
    hle_tl_lose();
    hle_host_gpu_lost(why);
}

bool hle_vk_lost(void)
{
    return __atomic_load_n(&g_lost, __ATOMIC_ACQUIRE);
}

bool hle_vk_check(VkResult r, const char *what)
{
    if (r == VK_SUCCESS) {
        return true;
    }
    if (r == VK_ERROR_DEVICE_LOST) {
        hle_vk_lose(r, what);
        return false;
    }
    static unsigned s_said;
    if (__atomic_fetch_add(&s_said, 1, __ATOMIC_RELAXED) < 16) {
        hle_log("hakuX-hle-back", "vk: %s: %s", what, hle_vk_result_text(r));
    }
    return false;
}

void hle_vk_queue_lock(void)
{
    if (pthread_mutex_trylock(&g_queue_lock.m) == 0) {
        return;
    }
    const int64_t t0 = hle_now_ns();
    hle_mutex_lock(&g_queue_lock);
    __atomic_fetch_add(&g_lock_wait_ns, hle_now_ns() - t0, __ATOMIC_RELAXED);
}

void hle_vk_queue_unlock(void)
{
    hle_mutex_unlock(&g_queue_lock);
}

int64_t hle_vk_queue_wait_ns(void)
{
    return __atomic_exchange_n(&g_lock_wait_ns, 0, __ATOMIC_RELAXED);
}

bool hle_vk_ready(void)
{
    return hle_atomic_load_acquire(&g_ready);
}

void hle_vk_set_ready(void)
{
    hle_smp_wmb();
    hle_atomic_set(&g_ready, true);
}

static bool has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(e[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static void need(HleCaps *c, bool ok, const char *what)
{
    if (!ok) {
        size_t l = strlen(c->missing);
        c->floor_ok = false;
        snprintf(c->missing + l, sizeof(c->missing) - l, "%s%s", l ? "," : "", what);
    }
}

static void chain(VkBaseOutStructure **tail, void *s)
{
    (*tail)->pNext = s;
    *tail = s;
}

/* debug.hakux.caps (else HAKUX_HLE_CAPS), read at device creation: "-name"
 * turns a capability off, so its substitute runs (what other vendors'
 * drivers lack), e.g. "-bc,-vids,-push"; "-vk13" makes a 1.3 device a 1.1 one
 * (an instance of 1.1) */
static char g_caps_v[96];

static void caps_read(void)
{
    g_caps_v[0] = 0;
#ifdef __ANDROID__
    hle_prop_get("debug.hakux.caps", g_caps_v);
#endif
    if (!g_caps_v[0] && getenv("HAKUX_HLE_CAPS")) {
        snprintf(g_caps_v, sizeof(g_caps_v), "%s", getenv("HAKUX_HLE_CAPS"));
    }
}

/* "-name" is in debug.hakux.caps */
static bool caps_off(const char *name)
{
    size_t n = strlen(name);
    for (const char *p = strchr(g_caps_v, '-'); p; p = strchr(p + 1, '-')) {
        if (strncmp(p + 1, name, n) == 0 && (p[1 + n] == 0 || p[1 + n] == ',' || p[1 + n] == ' ')) {
            return true;
        }
    }
    return false;
}

static void caps_override(HleCaps *c)
{
    char v[sizeof(g_caps_v)];
    snprintf(v, sizeof(v), "%s", g_caps_v);
    if (!v[0]) {
        return;
    }
    static const struct { const char *name; size_t off; } k_caps[] = {
        { "bc", offsetof(HleCaps, bc) },             { "vids", offsetof(HleCaps, vids) },
        { "push", offsetof(HleCaps, push) },         { "pushdesc", offsetof(HleCaps, push_desc) },
        { "cbc", offsetof(HleCaps, cbc) },           { "d24s8", offsetof(HleCaps, d24s8) },
        { "4444", offsetof(HleCaps, x4444) },        { "bindless", offsetof(HleCaps, bindless) },
        { "dclamp", offsetof(HleCaps, depth_clamp) },
        { "bda", offsetof(HleCaps, bda) },           { "ahb", offsetof(HleCaps, ahb) },
        { "fbfetch", offsetof(HleCaps, fbfetch) },   { "vk13", offsetof(HleCaps, vk13) },
        { "dynrender", offsetof(HleCaps, dyn_render) }, { "sync2", offsetof(HleCaps, sync2) },
        { "eds1", offsetof(HleCaps, eds1) },         { "eds2", offsetof(HleCaps, eds2) },
    };
    for (char *save = NULL, *t = strtok_r(v, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        const char *name = *t == '-' ? t + 1 : t;
        size_t i = 0;
        while (i < ARRAY_SIZE(k_caps) && strcmp(name, k_caps[i].name)) {
            i++;
        }
        if (*t != '-' || i == ARRAY_SIZE(k_caps)) {
            hle_log("hakuX-hle-back", "caps: \"%s\" ignored (\"-name\" turns one off)", t);
            continue;
        }
        *(bool *)((char *)c + k_caps[i].off) = false;
        hle_log("hakuX-hle-back", "caps: %s off (debug)", name);
    }
}

int hle_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want,
                   VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < g_hvk.mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = g_hvk.mem.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) {
            return (int)i;
        }
    }
    return -1;
}

int hle_vk_init(void)
{
    HleVk *v = &g_hvk;
    HleCaps *c = &v->caps;
    if (v->dev) {
        return 0;
    }
    if (volkGetInstanceVersion() == 0) {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)hle_host_vk_loader();
        if (gipa) {
            volkInitializeCustom(gipa);
        } else if (volkInitialize() != VK_SUCCESS) {
            hle_log("hakuX-hle-back", "vk: volkInitialize failed");
            return -1;
        }
    }
    hle_mutex_init(&g_queue_lock);
    caps_read();
    /* debug: a 1.3 device used as a 1.1 one (every 1.3 part from its extension
     * or vk/compat.c, as on the drivers below 1.3) */
    const bool force11 = caps_off("vk13");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hakuX-hle",
        .apiVersion = force11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_3,
    };
    /* the window's surface (vk/swapchain.c): where the loader has it */
    uint32_t nie = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    g_autofree VkExtensionProperties *iext = g_new(VkExtensionProperties, nie ? nie : 1);
    vkEnumerateInstanceExtensionProperties(NULL, &nie, iext);
    const char *iexts[2];
    uint32_t nix = 0;
    if (has_ext(iext, nie, VK_KHR_SURFACE_EXTENSION_NAME) &&
        has_ext(iext, nie, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        iexts[nix++] = VK_KHR_SURFACE_EXTENSION_NAME;
        iexts[nix++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = nix, .ppEnabledExtensionNames = iexts,
    };
    if (vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "vk: vkCreateInstance failed");
        return -1;
    }
    volkLoadInstanceOnly(v->inst);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(v->inst, &n, &v->phys);
    if (!n) {
        hle_log("hakuX-hle-back", "vk: no physical device");
        return -1;
    }
    vkGetPhysicalDeviceProperties(v->phys, &v->props);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mem);

    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
    /* graphics, and compute too where a family has both (nothing dispatches) */
    v->qfam = UINT32_MAX;
    for (int pass = 0; pass < 2 && v->qfam == UINT32_MAX; pass++) {
        VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | (pass ? 0 : VK_QUEUE_COMPUTE_BIT);
        for (uint32_t i = 0; i < nq && v->qfam == UINT32_MAX; i++) {
            if ((qf[i].queueFlags & want) == want) {
                v->qfam = i;
            }
        }
    }
    for (uint32_t i = 0; i < nq; i++) {
        hle_log("hakuX-hle-back", "vk: queue family %u: flags %x, %u queues%s", i, qf[i].queueFlags,
                qf[i].queueCount, i == v->qfam ? " (used)" : "");
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(v->phys, NULL, &ne, NULL);
    g_autofree VkExtensionProperties *ext = g_new(VkExtensionProperties, ne);
    vkEnumerateDeviceExtensionProperties(v->phys, NULL, &ne, ext);
    bool e_vids = has_ext(ext, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    bool e_eds2 = has_ext(ext, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    bool e_cbc = has_ext(ext, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    bool e_pv = has_ext(ext, ne, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    bool e_dce = has_ext(ext, ne, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    bool e_4444 = has_ext(ext, ne, VK_EXT_4444_FORMATS_EXTENSION_NAME);
    bool e_iu8x = has_ext(ext, ne, "VK_EXT_index_type_uint8");
    bool e_iu8k = has_ext(ext, ne, "VK_KHR_index_type_uint8");
    bool e_ahb = has_ext(ext, ne,
                         VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) &&
                 has_ext(ext, ne, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    bool e_eds3 = has_ext(ext, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    bool e_gpl = has_ext(ext, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                 has_ext(ext, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    bool e_lr = has_ext(ext, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    bool e_pd = has_ext(ext, ne, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    /* Below 1.3 (vk/compat.c): what 1.2 and 1.3 made core, from extensions;
     * a 1.2 device has 1.2's in its core (the structs are core 1.2's then) */
    const uint32_t api = force11 ? VK_API_VERSION_1_1 : v->props.apiVersion;
    const bool v13 = api >= VK_API_VERSION_1_3, v12 = api >= VK_API_VERSION_1_2;
    bool e_tl = v12 || has_ext(ext, ne, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    bool e_di = v12 || has_ext(ext, ne, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    bool e_bda = v12 || has_ext(ext, ne, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    bool e_hqr = v12 || has_ext(ext, ne, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
    bool e_s2 = has_ext(ext, ne, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    /* KHR_dynamic_rendering needs depth_stencil_resolve (and create_renderpass2) below 1.2 */
    bool e_dr = has_ext(ext, ne, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
                (v12 || (has_ext(ext, ne, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
                         has_ext(ext, ne, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)));
    bool e_eds1 = has_ext(ext, ne, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    bool e_pcc = has_ext(ext, ne, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    if (api < VK_API_VERSION_1_1) {     /* no vkGetPhysicalDeviceFeatures2 */
        hle_log("hakuX-hle-back", "vk: \"%s\" is a Vulkan 1.0 device", v->props.deviceName);
        snprintf(g_hle_unavailable, sizeof(g_hle_unavailable), "%s lacks vulkan1.1",
                 v->props.deviceName);
        vkDestroyInstance(v->inst, NULL);
        v->inst = VK_NULL_HANDLE;
        return -2;
    }
    {   /* debug: how guest RAM could be shared with the GPU */
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(v->phys, &mp);
        char mt[160];
        int o = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount && o < (int)sizeof(mt) - 12; i++) {
            o += snprintf(mt + o, sizeof(mt) - (size_t)o, " %u:%x", i, mp.memoryTypes[i].propertyFlags);
        }
        hle_log("hakuX-hle-back", "vk: memory: host_ptr=%d dma_buf=%d fd=%d ahb=%d, types%s",
                has_ext(ext, ne, "VK_EXT_external_memory_host"),
                has_ext(ext, ne, "VK_EXT_external_memory_dma_buf"),
                has_ext(ext, ne, "VK_KHR_external_memory_fd"), e_ahb, mt);
    }

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT q_vids = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT q_eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT q_cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    VkPhysicalDeviceProvokingVertexFeaturesEXT q_pv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT q_dce = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
    VkPhysicalDevice4444FormatsFeaturesEXT q_4444 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT };
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT q_iu8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT q_eds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT q_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR q_lr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features q13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features q12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &q13 };
    VkPhysicalDeviceFeatures2 q = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = v13 ? &q12 : NULL };
    /* below 1.3: each part's own struct (Vulkan12/13Features need 1.2/1.3) */
    VkPhysicalDeviceTimelineSemaphoreFeatures q_tl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceDescriptorIndexingFeatures q_di = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures q_bda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    VkPhysicalDeviceHostQueryResetFeatures q_hqr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES };
    VkPhysicalDeviceSynchronization2Features q_s2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeatures q_dr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT q_eds1 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDevicePipelineCreationCacheControlFeatures q_pcc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES };
    VkBaseOutStructure *tail = v13 ? (VkBaseOutStructure *)&q13 : (VkBaseOutStructure *)&q;
    if (!v13) {
        if (e_tl) chain(&tail, &q_tl);
        if (e_di) chain(&tail, &q_di);
        if (e_bda) chain(&tail, &q_bda);
        if (e_hqr) chain(&tail, &q_hqr);
        if (e_s2) chain(&tail, &q_s2);
        if (e_dr) chain(&tail, &q_dr);
        if (e_eds1) chain(&tail, &q_eds1);
        if (e_pcc) chain(&tail, &q_pcc);
    }
    if (e_vids) chain(&tail, &q_vids);
    if (e_eds2) chain(&tail, &q_eds2);
    if (e_cbc) chain(&tail, &q_cbc);
    if (e_pv) chain(&tail, &q_pv);
    if (e_dce) chain(&tail, &q_dce);
    if (e_4444) chain(&tail, &q_4444);
    if (e_iu8x || e_iu8k) chain(&tail, &q_iu8);
    if (e_eds3) chain(&tail, &q_eds3);
    if (e_gpl) chain(&tail, &q_gpl);
    if (e_lr) chain(&tail, &q_lr);
    vkGetPhysicalDeviceFeatures2(v->phys, &q);
    /* the limits the bindless set and the samplers need (Vulkan 1.2's: a 1.1
     * device fails the floor without them) */
    VkPhysicalDeviceCustomBorderColorPropertiesEXT p_cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT };
    VkPhysicalDeviceVulkan12Properties p12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
        .pNext = e_cbc ? &p_cbc : NULL };
    VkPhysicalDeviceDescriptorIndexingProperties p_di = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,
        .pNext = e_cbc ? &p_cbc : NULL };
    VkPhysicalDeviceProperties2 p2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = v13 ? (void *)&p12 : e_di ? (void *)&p_di : e_cbc ? (void *)&p_cbc : NULL };
    vkGetPhysicalDeviceProperties2(v->phys, &p2);
    /* the bindless limits, from whichever struct has them */
    const uint32_t l_img = v13 ? p12.maxPerStageDescriptorUpdateAfterBindSampledImages
                               : p_di.maxPerStageDescriptorUpdateAfterBindSampledImages;
    const uint32_t l_set_img = v13 ? p12.maxDescriptorSetUpdateAfterBindSampledImages
                                   : p_di.maxDescriptorSetUpdateAfterBindSampledImages;
    const uint32_t l_smp = v13 ? p12.maxPerStageDescriptorUpdateAfterBindSamplers
                               : p_di.maxPerStageDescriptorUpdateAfterBindSamplers;
    const uint32_t l_set_smp = v13 ? p12.maxDescriptorSetUpdateAfterBindSamplers
                                   : p_di.maxDescriptorSetUpdateAfterBindSamplers;
    /* the 1.2 features, from whichever struct has them */
    const bool ok_tl = v13 ? q12.timelineSemaphore : e_tl && q_tl.timelineSemaphore;
    const bool ok_rda = v13 ? q12.runtimeDescriptorArray : e_di && q_di.runtimeDescriptorArray;
    const bool ok_pb = v13 ? q12.descriptorBindingPartiallyBound
                          : e_di && q_di.descriptorBindingPartiallyBound;
    const bool ok_uab = v13 ? q12.descriptorBindingSampledImageUpdateAfterBind
                           : e_di && q_di.descriptorBindingSampledImageUpdateAfterBind;
    const bool ok_nui = v13 ? q12.shaderSampledImageArrayNonUniformIndexing
                           : e_di && q_di.shaderSampledImageArrayNonUniformIndexing;

    /* The floor: what every path needs. What some drivers lack and the back
     * end works without (Mali, Xclipse, PowerVR) is enabled where present,
     * the device Adreno gets unchanged: logicOp, dualSrcBlend, BC, EDS2,
     * provoking vertex, depth clip, uint8 indices, BDA, AHB, vertex input. */
    memset(c, 0, sizeof(*c));
    c->floor_ok = true;
    need(c, v->qfam != UINT32_MAX, "graphics queue");
    c->wide_lines = q.features.wideLines;
    need(c, q.features.largePoints, "largePoints");
    c->depth_clamp = q.features.depthClamp;
    need(c, ok_tl, "timelineSemaphore");
    /* the bindless set: its arrays indexed nonuniformEXT, written while bound */
    c->bindless = ok_rda && ok_pb && ok_uab && ok_nui &&
                  l_img >= 3 * HLE_MAX_IMAGES && l_set_img >= 3 * HLE_MAX_IMAGES &&
                  l_smp >= HLE_MAX_SAMPLERS && l_set_smp >= HLE_MAX_SAMPLERS;
    need(c, v->props.limits.maxBoundDescriptorSets >= 4, "4 descriptor sets");
    if (v13) {                          /* both required of a 1.3 driver */
        need(c, q13.synchronization2, "synchronization2");
        need(c, q13.dynamicRendering, "dynamicRendering");
    }
    c->vk13 = v13;
    c->dyn_render = v13 || (e_dr && q_dr.dynamicRendering);
    c->sync2 = v13 || (e_s2 && q_s2.synchronization2);
    c->eds1 = v13 || (e_eds1 && q_eds1.extendedDynamicState);
    c->eds2 = v13 || (e_eds2 && q_eds2.extendedDynamicState2);
    /* custom border colours and A4R4G4B4: substituted where missing (Mali's r26) */
    c->cbc = e_cbc && q_cbc.customBorderColors;
    c->x4444 = e_4444 && q_4444.formatA4R4G4B4;
    if (!c->x4444 && v13) {             /* core in 1.3, its extension listed or not */
        VkFormatProperties f4;
        const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_A4R4G4B4_UNORM_PACK16, &f4);
        c->x4444 = (f4.optimalTilingFeatures & want) == want;
    }
    c->bc = q.features.textureCompressionBC;
    if (!c->bc) {
        /* the formats without the feature (Xclipse; PowerVR's D-Series) */
        static const VkFormat k_bc[3] = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                                          VK_FORMAT_BC3_UNORM_BLOCK };
        const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        c->bc = true;
        for (unsigned i = 0; i < 3; i++) {
            VkFormatProperties bp;
            vkGetPhysicalDeviceFormatProperties(v->phys, k_bc[i], &bp);
            c->bc &= (bp.optimalTilingFeatures & want) == want;
        }
    }
    c->vids = e_vids && q_vids.vertexInputDynamicState;
    c->push = v->props.limits.maxPushConstantsSize >= HLE_PUSH_BYTES;
    c->push_desc = e_pd;
    c->cbc_any = q_cbc.customBorderColorWithoutFormat &&
                 p_cbc.maxCustomBorderColorSamplers >= HLE_MAX_SAMPLERS;
    {   /* D24S8, unless it can't be filtered and D32S8 can (PowerVR's shadow
         * maps are gathered linearly, psh.c); Xclipse has no D24S8 at all */
        VkFormatProperties dp, fp;
        const VkFormatFeatureFlags ds = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        const VkFormatFeatureFlags lin = ds | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_D24_UNORM_S8_UINT, &dp);
        vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
        c->d24s8 = (dp.optimalTilingFeatures & ds) == ds &&
                   ((dp.optimalTilingFeatures & lin) == lin || (fp.optimalTilingFeatures & lin) != lin);
    }
    c->bda = v13 ? q12.bufferDeviceAddress : e_bda && q_bda.bufferDeviceAddress;
    c->ahb = e_ahb;
    c->max_lod_bias = v->props.limits.maxSamplerLodBias;
    /* (unused, enabled where present: on 1.3 only) */
    c->gpl = v13 && e_gpl && q_gpl.graphicsPipelineLibrary;
    c->eds3 = v13 && e_eds3 && q_eds3.extendedDynamicState3ColorBlendEnable &&
              q_eds3.extendedDynamicState3ColorBlendEquation &&
              q_eds3.extendedDynamicState3ColorWriteMask &&
              q_eds3.extendedDynamicState3LogicOpEnable;
    c->descriptor_buffer = has_ext(ext, ne, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    c->maint5 = has_ext(ext, ne, "VK_KHR_maintenance5");
    /* local read is dynamic rendering's: 1.3 devices only */
    c->local_read = v13 && e_lr && q_lr.dynamicRenderingLocalRead;
    c->fbfetch = c->local_read && !getenv("HAKUX_HLE_NO_FBFETCH");
    c->exec_props = has_ext(ext, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    c->aniso = q.features.samplerAnisotropy;
    c->host_query_reset = v13 ? q12.hostQueryReset : e_hqr && q_hqr.hostQueryReset;
    c->cache_control = v13 ? q13.pipelineCreationCacheControl
                           : e_pcc && q_pcc.pipelineCreationCacheControl;
    c->occl_precise = q.features.occlusionQueryPrecise;
    c->swapchain = nix && has_ext(ext, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(v->phys, VK_FORMAT_R16G16_SSCALED, &fp);
    c->sscaled = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    /* Samsung Xclipse (Xclipse 940, Samsung's driver SPAL 25.3.3, Vulkan
     * 1.3.304): skinned meshes (MK Armageddon's fighters)
     * came out as huge spiky triangles while the scenery and HUD were right,
     * with the vertex strides and the state set dynamically (vids: the
     * strides by vkCmdSetVertexInputEXT; without it, eds1: by
     * vkCmdBindVertexBuffers2; eds1/eds2: the rest). All of it
     * baked into the pipelines (as debug.hakux.caps=-vids,-eds1,-eds2) drew
     * them right. */
    if (v->props.vendorID == HLE_VENDOR_SAMSUNG ||
        (v13 && (uint32_t)p12.driverID == HLE_DRIVER_ID_SAMSUNG)) {
        c->vids = c->eds1 = c->eds2 = false;
        hle_log("hakuX-hle-back", "caps: vids eds1 eds2 off (Samsung Xclipse: vertex input "
                "and state in the pipelines)");
    }
    caps_override(c);
    c->fbfetch = c->fbfetch && c->dyn_render;  /* local read is dynamic rendering's */
    c->cbc_any = c->cbc_any && c->cbc;
    if (!c->bindless) {                 /* the draw sets' arrays, by the push block's index */
        need(c, q.features.shaderSampledImageArrayDynamicIndexing, "sampledImageDynamicIndexing");
    }
    if (!c->floor_ok) {
        hle_log("hakuX-hle-back", "vk: floor not met on \"%s\": missing %s",
                v->props.deviceName, c->missing);
        snprintf(g_hle_unavailable, sizeof(g_hle_unavailable), "%s lacks %s", v->props.deviceName,
                 c->missing);
        vkDestroyInstance(v->inst, NULL);
        v->inst = VK_NULL_HANDLE;
        return -2;
    }

    /* Enable exactly the features used (never "everything supported"), and
     * those the floor used to need where present (Adreno's device as before). */
    const bool u_vids = e_vids && q_vids.vertexInputDynamicState;
    const bool u_eds2 = v13 ? e_eds2 && q_eds2.extendedDynamicState2 : c->eds2;
    const bool u_pv = e_pv && q_pv.provokingVertexLast;
    const bool u_dce = e_dce && q_dce.depthClipEnable;
    const bool u_iu8 = (e_iu8x || e_iu8k) && q_iu8.indexTypeUint8;
    const bool u_4444 = c->x4444 && e_4444 && q_4444.formatA4R4G4B4;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT f_vids = q_vids;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT f_eds2 = {
        .sType = q_eds2.sType, .extendedDynamicState2 = VK_TRUE,
        .extendedDynamicState2LogicOp = q_eds2.extendedDynamicState2LogicOp };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT f_cbc = {
        .sType = q_cbc.sType, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = q_cbc.customBorderColorWithoutFormat };
    VkPhysicalDeviceProvokingVertexFeaturesEXT f_pv = {
        .sType = q_pv.sType, .provokingVertexLast = VK_TRUE };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT f_dce = {
        .sType = q_dce.sType, .depthClipEnable = VK_TRUE };
    VkPhysicalDevice4444FormatsFeaturesEXT f_4444 = {
        .sType = q_4444.sType, .formatA4R4G4B4 = VK_TRUE };
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_iu8 = {
        .sType = q_iu8.sType, .indexTypeUint8 = VK_TRUE };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f_eds3 = {
        .sType = q_eds3.sType,
        .extendedDynamicState3ColorBlendEnable = VK_TRUE,
        .extendedDynamicState3ColorBlendEquation = VK_TRUE,
        .extendedDynamicState3ColorWriteMask = VK_TRUE,
        .extendedDynamicState3LogicOpEnable = VK_TRUE };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f_gpl = {
        .sType = q_gpl.sType, .graphicsPipelineLibrary = VK_TRUE };
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR f_lr = {
        .sType = q_lr.sType, .dynamicRenderingLocalRead = VK_TRUE };
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE,
        .pipelineCreationCacheControl = c->cache_control,
        /* SPIR-V 1.6: glslang makes a discard a demote */
        .shaderDemoteToHelperInvocation = q13.shaderDemoteToHelperInvocation };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13,
        .timelineSemaphore = VK_TRUE, .bufferDeviceAddress = c->bda,
        .runtimeDescriptorArray = VK_TRUE, .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .hostQueryReset = c->host_query_reset };
    VkPhysicalDeviceFeatures2 f = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = v13 ? &f12 : NULL };
    /* below 1.3: the parts' own structs (no demote: SPIR-V 1.3 discards) */
    VkPhysicalDeviceTimelineSemaphoreFeatures f_tl = {
        .sType = q_tl.sType, .timelineSemaphore = VK_TRUE };
    VkPhysicalDeviceDescriptorIndexingFeatures f_di = {
        .sType = q_di.sType, .runtimeDescriptorArray = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE };
    VkPhysicalDeviceBufferDeviceAddressFeatures f_bda = {
        .sType = q_bda.sType, .bufferDeviceAddress = VK_TRUE };
    VkPhysicalDeviceHostQueryResetFeatures f_hqr = {
        .sType = q_hqr.sType, .hostQueryReset = VK_TRUE };
    VkPhysicalDeviceSynchronization2Features f_s2 = {
        .sType = q_s2.sType, .synchronization2 = VK_TRUE };
    VkPhysicalDeviceDynamicRenderingFeatures f_dr = {
        .sType = q_dr.sType, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT f_eds1 = {
        .sType = q_eds1.sType, .extendedDynamicState = VK_TRUE };
    VkPhysicalDevicePipelineCreationCacheControlFeatures f_pcc = {
        .sType = q_pcc.sType, .pipelineCreationCacheControl = VK_TRUE };
    f.features.wideLines = c->wide_lines;
    f.features.largePoints = VK_TRUE;
    f.features.depthClamp = c->depth_clamp;
    f.features.shaderSampledImageArrayDynamicIndexing = !c->bindless;
    f.features.logicOp = q.features.logicOp;
    f.features.dualSrcBlend = q.features.dualSrcBlend;
    f.features.textureCompressionBC = q.features.textureCompressionBC;
    f.features.samplerAnisotropy = c->aniso;
    f.features.occlusionQueryPrecise = c->occl_precise;
    f_vids.pNext = NULL;
    f_vids.vertexInputDynamicState = VK_TRUE;
    tail = v13 ? (VkBaseOutStructure *)&f13 : (VkBaseOutStructure *)&f;
    if (!v13) {
        chain(&tail, &f_tl);
        if (c->bindless) chain(&tail, &f_di);
        if (c->bda) chain(&tail, &f_bda);
        if (c->host_query_reset) chain(&tail, &f_hqr);
        if (c->sync2) chain(&tail, &f_s2);
        if (c->dyn_render) chain(&tail, &f_dr);
        if (c->eds1) chain(&tail, &f_eds1);
        if (c->cache_control) chain(&tail, &f_pcc);
    }
    if (u_vids) chain(&tail, &f_vids);
    if (u_eds2) chain(&tail, &f_eds2);
    if (c->cbc) chain(&tail, &f_cbc);
    if (u_pv) chain(&tail, &f_pv);
    if (u_dce) chain(&tail, &f_dce);
    if (u_4444) chain(&tail, &f_4444);
    if (u_iu8) chain(&tail, &f_iu8);
    if (c->eds3) chain(&tail, &f_eds3);
    if (c->gpl) chain(&tail, &f_gpl);
    if (c->fbfetch) chain(&tail, &f_lr);

    const char *exts[32];
    uint32_t nx = 0;
    if (!v13) {
        if (!v12) {                     /* core in 1.2 */
            exts[nx++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
            if (c->bindless) exts[nx++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
            if (c->bda) exts[nx++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
            if (c->host_query_reset) exts[nx++] = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME;
        }
        if (c->sync2) exts[nx++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
        if (c->dyn_render) {
            exts[nx++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
            if (!v12) {
                exts[nx++] = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME;
                exts[nx++] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
            }
        }
        if (c->eds1) exts[nx++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
        if (c->cache_control) exts[nx++] = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME;
    }
    if (u_vids) exts[nx++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (u_eds2) exts[nx++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME;
    if (c->cbc) exts[nx++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    if (u_pv) exts[nx++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
    if (u_dce) exts[nx++] = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME;
    if (u_4444) exts[nx++] = VK_EXT_4444_FORMATS_EXTENSION_NAME;
    if (u_iu8) exts[nx++] = e_iu8k ? "VK_KHR_index_type_uint8" : "VK_EXT_index_type_uint8";
    if (c->ahb) {
        exts[nx++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
        exts[nx++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    }
    if (c->eds3) exts[nx++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
    if (c->gpl) {
        exts[nx++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        exts[nx++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    if (c->fbfetch) {
        exts[nx++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        if (e_pd) {
            exts[nx++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        }
    }
    if (c->swapchain) {
        exts[nx++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = v->qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nx, .ppEnabledExtensionNames = exts };
    VkResult r = vkCreateDevice(v->phys, &dci, NULL, &v->dev);
    if (r != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "vk: vkCreateDevice failed (%d)", r);
        vkDestroyInstance(v->inst, NULL);
        v->inst = VK_NULL_HANDLE;
        v->dev = VK_NULL_HANDLE;
        return -1;
    }
    volkLoadDeviceTable(&v->t, v->dev);
    hle_compat_install(&v->t);          /* below 1.3: its parts from extensions or emulated */
    hle_batch_install(&v->t);           /* strip batching sees every recorded command */
    g_hle_glsl_vk11 = !c->vk13;
    v->t.vkGetDeviceQueue(v->dev, v->qfam, 0, &v->queue);
    v->get_ahb_props = c->ahb ? (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(v->dev, "vkGetAndroidHardwareBufferPropertiesANDROID") : NULL;
    hle_log("hakuX-hle-back", "vk: device \"%s\" api %u.%u.%u driver 0x%x floor=ok "
            "plus: gpl=%d eds3=%d dbuf=%d m5=%d sscaled=%d local_read=%d fbfetch=%d cache_control=%d "
            "swapchain=%d",
            v->props.deviceName, VK_API_VERSION_MAJOR(v->props.apiVersion),
            VK_API_VERSION_MINOR(v->props.apiVersion),
            VK_API_VERSION_PATCH(v->props.apiVersion), v->props.driverVersion,
            c->gpl, c->eds3, c->descriptor_buffer, c->maint5, c->sscaled,
            c->local_read, c->fbfetch, c->cache_control, c->swapchain);
    hle_log("hakuX-hle-back", "vk: bc=%d vids=%d push=%d push_desc=%d cbc=%d cbc_any=%d "
            "x4444=%d d24s8=%d bda=%d ahb=%d vk13=%d dyn_render=%d sync2=%d eds1=%d eds2=%d "
            "bindless=%d depth_clamp=%d (0: the substitute runs)", c->bc, c->vids, c->push,
            c->push_desc, c->cbc, c->cbc_any, c->x4444, c->d24s8, c->bda, c->ahb, c->vk13,
            c->dyn_render, c->sync2, c->eds1, c->eds2, c->bindless, c->depth_clamp);
    hle_quirks_init(v->phys);           /* what this driver needs worked around */
    return 0;
}

