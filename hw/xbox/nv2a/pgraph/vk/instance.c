/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "ui/xemu-settings.h"
#include "renderer.h"
#include "xemu-version.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <volk.h>

#define VkExtensionPropertiesArray GArray
#define StringArray GArray

static bool enable_validation = false;

/* Filled during select_physical_device for display in the Android overlay. */
char g_vulkan_driver_info[256] = "Vulkan: initializing...";

static char const *const validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static char const *const required_device_extensions[] = {
#ifdef WIN32
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
#ifdef __ANDROID__
    int prio = ANDROID_LOG_VERBOSE;
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        prio = ANDROID_LOG_ERROR;
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        prio = ANDROID_LOG_WARN;
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        prio = ANDROID_LOG_INFO;
    __android_log_print(prio, "xemu-vk-validation", "%s",
                        pCallbackData->pMessage);
#else
    fprintf(stderr, "[vk] %s\n", pCallbackData->pMessage);
#endif

    if ((messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) {
        assert(!g_config.display.vulkan.assert_on_validation_msg);
    }
    return VK_FALSE;
}

static bool check_validation_layer_support(void)
{
    uint32_t num_available_layers;
    vkEnumerateInstanceLayerProperties(&num_available_layers, NULL);

    g_autofree VkLayerProperties *available_layers =
        g_malloc_n(num_available_layers, sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&num_available_layers, available_layers);

    for (int i = 0; i < ARRAY_SIZE(validation_layers); i++) {
        bool found = false;
        for (int j = 0; j < num_available_layers; j++) {
            if (!strcmp(validation_layers[i], available_layers[j].layerName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "desired validation layer not found: %s\n",
                    validation_layers[i]);
            return false;
        }
    }

    return true;
}

static VkExtensionPropertiesArray *
get_available_instance_extensions(PGRAPHState *pg)
{
    uint32_t num_extensions = 0;

    VK_CHECK(
        vkEnumerateInstanceExtensionProperties(NULL, &num_extensions, NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(
        NULL, &num_extensions, (VkExtensionProperties *)extensions->data));

    return extensions;
}

static bool
is_extension_available(VkExtensionPropertiesArray *available_extensions,
                       const char *extension_name)
{
    for (int i = 0; i < available_extensions->len; i++) {
        VkExtensionProperties *e =
            &g_array_index(available_extensions, VkExtensionProperties, i);
        if (!strcmp(e->extensionName, extension_name)) {
            return true;
        }
    }

    return false;
}

static bool
add_extension_if_available(VkExtensionPropertiesArray *available_extensions,
                           StringArray *enabled_extension_names,
                           const char *desired_extension_name)
{
    if (is_extension_available(available_extensions, desired_extension_name)) {
        g_array_append_val(enabled_extension_names, desired_extension_name);
        return true;
    }

    fprintf(stderr, "Warning: extension not available: %s\n",
            desired_extension_name);
    return false;
}

static void
add_optional_instance_extension_names(PGRAPHState *pg,
                                      VkExtensionPropertiesArray *available_extensions,
                                      StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->debug_utils_extension_enabled =
        g_config.display.vulkan.validation_layers &&
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}

static bool create_instance(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

#ifdef __ANDROID__
    /* Try to use a custom Vulkan driver loaded via adrenotools */
    extern PFN_vkGetInstanceProcAddr xemu_android_get_vk_proc_addr(void);
    PFN_vkGetInstanceProcAddr custom_proc = xemu_android_get_vk_proc_addr();
    if (custom_proc) {
        volkInitializeCustom(custom_proc);
        result = VK_SUCCESS;
    } else {
        result = volkInitialize();
    }
#else
    result = volkInitialize();
#endif
    if (result != VK_SUCCESS) {
        error_setg(errp, "volkInitialize failed");
        return false;
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xemu",
        .applicationVersion = VK_MAKE_VERSION(
            xemu_version_major, xemu_version_minor, xemu_version_patch),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_instance_extensions(pg);

    g_autoptr(StringArray) enabled_extension_names =
        g_array_new(FALSE, FALSE, sizeof(char *));

    add_optional_instance_extension_names(pg, available_extensions,
                                          enabled_extension_names);

    const char *const *enabled_instance_extension_names = NULL;
    if (enabled_extension_names->len > 0) {
        enabled_instance_extension_names =
            &g_array_index(enabled_extension_names, const char *, 0);
    }

    fprintf(stderr, "Enabled instance extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames = enabled_instance_extension_names,
    };

    enable_validation = g_config.display.vulkan.validation_layers;

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-vk-validation",
                        "validation_layers config = %d", enable_validation ? 1 : 0);
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk-validation",
                            "Available instance layers: %u", n);
        if (n > 0) {
            VkLayerProperties *lp = g_malloc_n(n, sizeof(VkLayerProperties));
            vkEnumerateInstanceLayerProperties(&n, lp);
            for (uint32_t i = 0; i < n; i++) {
                __android_log_print(ANDROID_LOG_INFO, "xemu-vk-validation",
                                    "  layer[%u]: %s", i, lp[i].layerName);
            }
            g_free(lp);
        }
    }
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        __android_log_print(ANDROID_LOG_INFO, "xemu-vk-validation",
                            "Available instance extensions: %u", n);
    }
#endif

    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        // VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
    };

    VkValidationFeaturesEXT validationFeatures = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = ARRAY_SIZE(enables),
        .pEnabledValidationFeatures = enables,
    };

    if (enable_validation) {
        if (check_validation_layer_support()) {
            fprintf(stderr, "Warning: Validation layers enabled. Expect "
                            "performance impact.\n");
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_WARN, "xemu-vk-validation",
                                "Validation layers ENABLED — expect performance impact");
#endif
            create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
            create_info.ppEnabledLayerNames = validation_layers;
            create_info.pNext = &validationFeatures;
        } else {
            fprintf(stderr, "Warning: validation layers not available\n");
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "xemu-vk-validation",
                                "Validation layers requested but NOT AVAILABLE — "
                                "push the layer .so via adb");
#endif
            enable_validation = false;
        }
    }

    result = vkCreateInstance(&create_info, NULL, &r->instance);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create instance (%d)", result);
        return false;
    }

    volkLoadInstance(r->instance);

    if (r->debug_utils_extension_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(r->instance, &messenger_info,
                                                NULL, &r->debug_messenger));
    }

    return true;
}

static bool is_queue_family_indicies_complete(QueueFamilyIndices indices)
{
    return indices.queue_family >= 0;
}

QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = {
        .queue_family = -1,
    };

    uint32_t num_queue_families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families, NULL);

    g_autofree VkQueueFamilyProperties *queue_families =
        g_malloc_n(num_queue_families, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families,
                                             queue_families);

    for (int i = 0; i < num_queue_families; i++) {
        VkQueueFamilyProperties queueFamily = queue_families[i];
        // FIXME: Support independent graphics, compute queues
        int required_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((queueFamily.queueFlags & required_flags) == required_flags) {
            indices.queue_family = i;
        }
        if (is_queue_family_indicies_complete(indices)) {
            break;
        }
    }

    return indices;
}

static VkExtensionPropertiesArray *
get_available_device_extensions(VkPhysicalDevice device)
{
    uint32_t num_extensions = 0;

    VK_CHECK(vkEnumerateDeviceExtensionProperties(device, NULL, &num_extensions,
                                                  NULL));

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(
        device, NULL, &num_extensions,
        (VkExtensionProperties *)extensions->data));

    return extensions;
}

static StringArray *get_required_device_extension_names(void)
{
    StringArray *extensions =
        g_array_sized_new(FALSE, FALSE, sizeof(char *),
                          ARRAY_SIZE(required_device_extensions));

    g_array_append_vals(extensions, required_device_extensions,
                        ARRAY_SIZE(required_device_extensions));

    return extensions;
}

static void add_optional_device_extension_names(
    PGRAPHState *pg, VkExtensionPropertiesArray *available_extensions,
    StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->custom_border_color_extension_enabled =
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    r->memory_budget_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
}

static bool check_device_support_required_extensions(VkPhysicalDevice device)
{
    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(device);

    for (int i = 0; i < ARRAY_SIZE(required_device_extensions); i++) {
        if (!is_extension_available(available_extensions,
                                    required_device_extensions[i])) {
            fprintf(stderr, "required device extension not found: %s\n",
                    required_device_extensions[i]);
            return false;
        }
    }

    return true;
}

static bool is_device_compatible(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        return false;
    }

    QueueFamilyIndices indices = pgraph_vk_find_queue_families(device);

    return is_queue_family_indicies_complete(indices) &&
           check_device_support_required_extensions(device);
    // FIXME: Check formats
    // FIXME: Check vram
}

static bool select_physical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    uint32_t num_physical_devices = 0;

    result =
        vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, NULL);
    if (result != VK_SUCCESS || num_physical_devices == 0) {
        error_setg(errp, "Failed to find GPUs with Vulkan support");
        return false;
    }

    g_autofree VkPhysicalDevice *devices =
        g_malloc_n(num_physical_devices, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, devices);

    const char *preferred_device = g_config.display.vulkan.preferred_physical_device;
    int preferred_device_index = -1;

    fprintf(stderr, "Available physical devices:\n");
    for (int i = 0; i < num_physical_devices; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &r->device_props);
        bool is_preferred =
            preferred_device &&
            !strcmp(r->device_props.deviceName, preferred_device);
        if (is_preferred) {
            preferred_device_index = i;
        }
        fprintf(stderr, "- %s%s\n", r->device_props.deviceName,
                is_preferred ? " *" : "");
    }

    r->physical_device = VK_NULL_HANDLE;

    if (preferred_device_index >= 0 &&
        is_device_compatible(devices[preferred_device_index])) {
        r->physical_device = devices[preferred_device_index];
    } else {
        for (int i = 0; i < num_physical_devices; i++) {
            if (is_device_compatible(devices[i])) {
                r->physical_device = devices[i];
                break;
            }
        }
    }
    if (r->physical_device == VK_NULL_HANDLE) {
        error_setg(errp, "Failed to find a suitable GPU");
        return false;
    }

    vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
    xemu_settings_set_string(&g_config.display.vulkan.preferred_physical_device,
                             r->device_props.deviceName);

    /*
     * Query extended driver properties (Vulkan 1.2+) to get the actual
     * driver name and conformance info.  This is especially useful on
     * Android where custom GPU drivers (e.g. Turnip) can be injected
     * at runtime via adrenotools.
     */
    if (r->device_props.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof(drv));
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 props2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &drv,
        };
        vkGetPhysicalDeviceProperties2(r->physical_device, &props2);

        fprintf(stderr,
                "Selected physical device: %s\n"
                "- Vendor: %04x, Device: %04x\n"
                "- API Version: %d.%d.%d\n"
                "- Driver: %s (%s)\n"
                "- Driver Version: %d.%d.%d\n",
                r->device_props.deviceName,
                r->device_props.vendorID,
                r->device_props.deviceID,
                VK_VERSION_MAJOR(r->device_props.apiVersion),
                VK_VERSION_MINOR(r->device_props.apiVersion),
                VK_VERSION_PATCH(r->device_props.apiVersion),
                drv.driverName, drv.driverInfo,
                VK_VERSION_MAJOR(r->device_props.driverVersion),
                VK_VERSION_MINOR(r->device_props.driverVersion),
                VK_VERSION_PATCH(r->device_props.driverVersion));

#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-vulkan",
                            "GPU: %s | Driver: %s (%s) | "
                            "Vendor: %04x Device: %04x | "
                            "API: %d.%d.%d | DriverVer: %d.%d.%d",
                            r->device_props.deviceName,
                            drv.driverName, drv.driverInfo,
                            r->device_props.vendorID,
                            r->device_props.deviceID,
                            VK_VERSION_MAJOR(r->device_props.apiVersion),
                            VK_VERSION_MINOR(r->device_props.apiVersion),
                            VK_VERSION_PATCH(r->device_props.apiVersion),
                            VK_VERSION_MAJOR(r->device_props.driverVersion),
                            VK_VERSION_MINOR(r->device_props.driverVersion),
                            VK_VERSION_PATCH(r->device_props.driverVersion));
#endif
        snprintf(g_vulkan_driver_info, sizeof(g_vulkan_driver_info),
                 "%s (%s)", drv.driverName, drv.driverInfo);
    } else {
        fprintf(stderr,
                "Selected physical device: %s\n"
                "- Vendor: %04x, Device: %04x\n"
                "- Driver Version: %d.%d.%d\n",
                r->device_props.deviceName,
                r->device_props.vendorID,
                r->device_props.deviceID,
                VK_VERSION_MAJOR(r->device_props.driverVersion),
                VK_VERSION_MINOR(r->device_props.driverVersion),
                VK_VERSION_PATCH(r->device_props.driverVersion));

#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-vulkan",
                            "GPU: %s | Vendor: %04x Device: %04x | "
                            "DriverVer: %d.%d.%d (Vulkan <1.2, no driver name)",
                            r->device_props.deviceName,
                            r->device_props.vendorID,
                            r->device_props.deviceID,
                            VK_VERSION_MAJOR(r->device_props.driverVersion),
                            VK_VERSION_MINOR(r->device_props.driverVersion),
                            VK_VERSION_PATCH(r->device_props.driverVersion));
#endif
        snprintf(g_vulkan_driver_info, sizeof(g_vulkan_driver_info),
                 "%s (v%d.%d.%d)",
                 r->device_props.deviceName,
                 VK_VERSION_MAJOR(r->device_props.driverVersion),
                 VK_VERSION_MINOR(r->device_props.driverVersion),
                 VK_VERSION_PATCH(r->device_props.driverVersion));
    }

    return true;
}

static bool create_logical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(r->physical_device);

    g_autoptr(StringArray) enabled_extension_names =
        get_required_device_extension_names();

    add_optional_device_extension_names(pg, available_extensions,
                                        enabled_extension_names);

    const char *const *enabled_device_extension_names = NULL;
    if (enabled_extension_names->len > 0) {
        enabled_device_extension_names =
            &g_array_index(enabled_extension_names, const char *, 0);
    }

    fprintf(stderr, "Enabled device extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = indices.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Check device features
    VkPhysicalDeviceFeatures physical_device_features;
    vkGetPhysicalDeviceFeatures(r->physical_device, &physical_device_features);
    memset(&r->enabled_physical_device_features, 0,
           sizeof(r->enabled_physical_device_features));

    struct {
        const char *name;
        VkBool32 available, *enabled;
        bool required;
    } desired_features[] = {
        // clang-format off
        #define F(n, req) { \
            .name = #n, \
            .available = physical_device_features.n, \
            .enabled = &r->enabled_physical_device_features.n, \
            .required = req, \
        }
        F(depthClamp, false),
        F(fillModeNonSolid, false),
        F(geometryShader, true),
        F(occlusionQueryPrecise, false),
        F(samplerAnisotropy, false),
        F(shaderClipDistance, false),
        F(shaderTessellationAndGeometryPointSize, false),
        F(wideLines, false),
        #undef F
        // clang-format on
    };

    bool all_required_features_available = true;
    char missing_required_features[256] = { 0 };
    size_t missing_required_len = 0;
    for (int i = 0; i < ARRAY_SIZE(desired_features); i++) {
        fprintf(stderr, "Vulkan feature %-36s : %s%s\n",
                desired_features[i].name,
                desired_features[i].available == VK_TRUE ? "available" : "missing",
                desired_features[i].required ? " (required)" : "");
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "vk feature %s: %s%s",
                            desired_features[i].name,
                            desired_features[i].available == VK_TRUE ? "available" : "missing",
                            desired_features[i].required ? " (required)" : "");
#endif
        if (desired_features[i].required &&
            desired_features[i].available != VK_TRUE) {
            fprintf(stderr,
                    "Error: Device does not support required feature %s\n",
                    desired_features[i].name);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "vk required feature missing: %s",
                                desired_features[i].name);
#endif
            int n = snprintf(missing_required_features + missing_required_len,
                             sizeof(missing_required_features) - missing_required_len,
                             "%s%s",
                             missing_required_len ? ", " : "",
                             desired_features[i].name);
            if (n > 0) {
                size_t remaining = sizeof(missing_required_features) -
                                   missing_required_len - 1;
                size_t consumed = (size_t)n > remaining ? remaining : (size_t)n;
                missing_required_len += consumed;
            }
            all_required_features_available = false;
        }
        *desired_features[i].enabled = desired_features[i].available;
    }

    if (!all_required_features_available) {
        error_setg(errp, "Device does not support required features: %s",
                   missing_required_features[0] ?
                   missing_required_features : "(unknown)");
        return false;
    }

    void *next_struct = NULL;

    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border_features;
    if (r->custom_border_color_extension_enabled) {
        custom_border_features = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .customBorderColors = VK_TRUE,
            .pNext = next_struct,
        };
        next_struct = &custom_border_features;
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .pEnabledFeatures = &r->enabled_physical_device_features,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames = enabled_device_extension_names,
        .pNext = next_struct,
    };

    if (enable_validation) {
        device_create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
        device_create_info.ppEnabledLayerNames = validation_layers;
    }

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vkCreateDevice");
#endif
    result = vkCreateDevice(r->physical_device, &device_create_info, NULL,
                            &r->device);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create logical device (%d)", result);
        return false;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vkCreateDevice done");
#endif

    vkGetDeviceQueue(r->device, indices.queue_family, 0, &r->queue);
    return true;
}

uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++) {
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties &&
            type_bits & (1 << i)) {
            return i;
        }
    }
    return 0xFFFFFFFF; // Unable to find memoryType
}

static bool init_allocator(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    VmaVulkanFunctions vulkanFunctions = {
        /// Required when using VMA_DYNAMIC_VULKAN_FUNCTIONS.
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        /// Required when using VMA_DYNAMIC_VULKAN_FUNCTIONS.
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR = vkBindBufferMemory2,
        .vkBindImageMemory2KHR = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
    };

    uint32_t device_api_version = r->device_props.apiVersion;

    /* Clamp to VK_API_VERSION_1_3 -- VMA may be compiled without 1.4
     * header support and will assert if given a higher version. xemu only
     * requires Vulkan 1.1 features anyway. */
    if (device_api_version > VK_API_VERSION_1_3) {
        device_api_version = VK_API_VERSION_1_3;
    }

    if (device_api_version >= VK_API_VERSION_1_3) {
        vulkanFunctions.vkGetDeviceBufferMemoryRequirements =
            vkGetDeviceBufferMemoryRequirements;
        vulkanFunctions.vkGetDeviceImageMemoryRequirements =
            vkGetDeviceImageMemoryRequirements;
    }

    VmaAllocatorCreateInfo create_info = {
        .flags = (r->memory_budget_extension_enabled ?
                      VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT :
                      0),
        .vulkanApiVersion = device_api_version,
        .instance = r->instance,
        .physicalDevice = r->physical_device,
        .device = r->device,
        .pVulkanFunctions = &vulkanFunctions,
    };

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vmaCreateAllocator");
#endif
    result = vmaCreateAllocator(&create_info, &r->allocator);
    if (result != VK_SUCCESS) {
        error_setg(errp, "vmaCreateAllocator failed");
        return false;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: vmaCreateAllocator done");
#endif

    return true;
}

void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp)
{
    bool ok = false;

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: create_instance");
#endif
    if (!create_instance(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: select_physical_device");
#endif
    if (!select_physical_device(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: create_logical_device");
#endif
    if (!create_logical_device(pg, errp)) {
        goto done;
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "vk init stage: init_allocator");
#endif
    if (!init_allocator(pg, errp)) {
        goto done;
    }

    ok = true;

done:
    if (ok) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "vk init stage: complete");
#endif
        return;
    }

    pgraph_vk_finalize_instance(pg);

    const char *msg = "Failed to initialize Vulkan renderer";
    if (*errp) {
        error_prepend(errp, "%s: ", msg);
    } else {
        error_setg(errp, "%s", msg);
    }
}

void pgraph_vk_finalize_instance(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(r->allocator);
        r->allocator = VK_NULL_HANDLE;
    }

    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
    }

    if (r->debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
        r->debug_messenger = VK_NULL_HANDLE;
    }

    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }

    volkFinalize();
}
