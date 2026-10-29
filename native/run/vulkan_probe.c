#include "native/core/nc.h"
#include "hw/xbox/hle/vk/vkdev.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    puts("hakuX Vulkan capability probe: no surface, rendering or RAM import is tested");
    int result = hle_vk_init();
    if (result) {
        fprintf(stderr, "FAIL: renderer initialization (%d); missing: %s\n", result, g_hvk.caps.missing);
        return result == -2 ? 2 : 1;
    }
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(g_hvk.phys, &props);
    printf("GPU: %s\nDriver: %s / %s\nPASS: all portable renderer requirements in vkdev.c\n",
           props.properties.deviceName, driver.driverName, driver.driverInfo);
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) {
        return 1;
    }
    vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
    puts("Instance extensions (availability only):");
    for (uint32_t i = 0; i < count; i++) {
        printf("  %s\n", extensions[i].extensionName);
    }
    free(extensions);
    g_hvk.t.vkDestroyDevice(g_hvk.dev, NULL);
    vkDestroyInstance(g_hvk.inst, NULL);
    return 0;
}
