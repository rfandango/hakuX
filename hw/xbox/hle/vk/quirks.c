/*
 * hakuX HLE GPU — Vulkan driver quirks (see quirks.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/vk/quirks.h"

#include "hw/xbox/hle/hle.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif

uint32_t g_hle_quirks;

typedef struct HleDriver {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceDriverProperties drv;
} HleDriver;

typedef struct HleQuirk {
    uint32_t bit;
    const char *name;
    bool (*on)(const HleDriver *d);     /* the drivers it is on for; NULL: none */
} HleQuirk;

static bool qualcomm_proprietary(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
}

static bool mesa_turnip(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_MESA_TURNIP;
}

static bool not_turnip(const HleDriver *d)
{
    return !mesa_turnip(d);
}

static bool arm_proprietary(const HleDriver *d)
{
    return d->drv.driverID == VK_DRIVER_ID_ARM_PROPRIETARY;
}

/* Mali hardware, any driver (vendor 0x13B5: Arm's, PanVK) */
static bool mali(const HleDriver *d)
{
    return d->props.vendorID == 0x13B5;
}

static bool not_adreno(const HleDriver *d)
{
    return !qualcomm_proprietary(d) && !mesa_turnip(d);
}

/* PowerVR (vendor 0x1010) before driver 0x00582558 (ARMSX2's and PPSSPP's bound) */
static bool powervr_old(const HleDriver *d)
{
    return d->props.vendorID == 0x1010 && d->props.driverVersion < 0x00582558u;
}

/* Samsung's Xclipse (AMD RDNA), Samsung's driver */
static bool samsung(const HleDriver *d)
{
    return d->props.vendorID == HLE_VENDOR_SAMSUNG ||
           (uint32_t)d->drv.driverID == HLE_DRIVER_ID_SAMSUNG;
}

static bool mali_g615(const HleDriver *d)
{
    return mali(d) && strstr(d->props.deviceName, "Mali-G615") != NULL;
}

/* Seen on (driver, version, what showed it). */
static const HleQuirk k_quirks[] = {
    /* Adreno 840 stock driver 0x8034a02c: Forza's menu text picks its glyph
     * layer with a per-vertex mask (v4); after a pipeline change the draw
     * read v4 from offset 144 of the vertex instead of 64 (wrong letters). */
    { HLE_QUIRK_VI_AFTER_PIPELINE, "vi_after_pipeline", qualcomm_proprietary },
    { HLE_QUIRK_STATE_AFTER_PIPELINE, "state_after_pipeline", NULL },
    /* Turnip: Halo's floors flashed unlit in single frames, its lighting
     * passes over the base pass split by a mid-frame kick; the stock Adreno
     * driver drew them right (A/B, 2026-10-14). */
    { HLE_QUIRK_EQ_DEPTH_STEP, "eq_depth_step", mesa_turnip },
    /* Turnip 26.3 (Adreno 650): Blood Wake's boat select and Crazy Taxi 3's
     * driver select showed the model in blocks only; both clear, draw a
     * backdrop with the depth test off, then the model. TU_DEBUG=nolrzfc
     * (LRZ cleared by a blit) drew them right (2026-10-22). */
    { HLE_QUIRK_LRZ_CLEAR, "lrz_clear", mesa_turnip },
    /* Turnip (tu_pipeline.cc: no colour attachments, a static dummy blend
     * group): Fatal Frame's afterimage (CONSTANT_ALPHA over the last frame)
     * dropped out on the frames whose post pass shared a command buffer with
     * its depth-only shadow pass, and the scene flickered (2026-10-23). */
    { HLE_QUIRK_BLEND_CONST_RESET, "blend_const_reset", mesa_turnip },
    /* ARMSX2 (GSDeviceVK.cpp): "Arm's Mali blob crashes in
     * vkCmdPushDescriptorSetKHR even where it advertises the extension" */
    { HLE_QUIRK_NO_PUSH_DESC, "no_push_desc", arm_proprietary },
    /* ARMSX2 after PPSSPP: depth vs stencil load ops that differ corrupt on Mali */
    { HLE_QUIRK_DS_SAME_LOAD, "ds_same_load", mali },
    /* ARMSX2 (tfx.glsl GPU_PROFILE_MALI) after PPSSPP's EQUAL_WZ_CORRUPTS_DEPTH */
    { HLE_QUIRK_Z_EQ_W, "z_eq_w", mali },
    { HLE_QUIRK_FRESH_CLEAR, "fresh_clear", not_adreno },
    /* ARMSX2 (VKSwapChain.cpp) after PPSSPP: older PowerVR swapchain widths */
    { HLE_QUIRK_SWAP_W32, "swap_w32", powervr_old },
    /* ARMSX2: "Mali-G615's timestamp queries never resolve" */
    { HLE_QUIRK_NO_TIMESTAMPS, "no_timestamps", mali_g615 },
    /* Qualcomm's driver: a present waiting on the pass's semaphore waited
     * 3-6 ms a refresh in the driver, the queue's lock held (Halo 2).
     * Turnip's makes a sync file and returns (Mesa vk_android.c); the other
     * drivers are untested, so they keep the wait. */
    { HLE_QUIRK_PRESENT_CPU_WAIT, "present_cpu_wait", not_turnip },
    /* Xclipse 940, SPAL 25.3.3: MK Shaolin Monks' fighters (bones by
     * c[A0 + n]) as large flat planes while the scenery (constant indices)
     * was right; the same block as a storage buffer drew them right
     * (2026-10-27). */
    { HLE_QUIRK_CONST_SSBO, "const_ssbo", samsung },
    /* Xclipse 940, SPAL 25.3.3: MK Shaolin Monks' scenery (SHORT3 positions
     * in vertex programs) as stretched polygons; fetched as R16G16B16_SSCALED
     * it drew right (2026-10-27). */
    { HLE_QUIRK_S32K_SCALED, "s32k_scaled", samsung },
};

static uint32_t g_defaults;

static void names(uint32_t mask, char *out, size_t len)
{
    size_t n = 0;
    out[0] = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (mask & k_quirks[i].bit) {
            n += (size_t)snprintf(out + n, n < len ? len - n : 0, "%s%s", n ? "," : "",
                                  k_quirks[i].name);
        }
    }
    if (!n) {
        snprintf(out, len, "-");
    }
}

/* The defaults with an override's "+name" / "-name" applied. */
static uint32_t apply(uint32_t mask, const char *spec)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        bool off = *t == '-';
        const char *name = *t == '-' || *t == '+' ? t + 1 : t;
        uint32_t bits = 0;
        if (!strcmp(name, "all")) {
            for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
                bits |= k_quirks[i].bit;
            }
        }
        for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
            if (!strcmp(name, k_quirks[i].name)) {
                bits = k_quirks[i].bit;
            }
        }
        if (!bits) {
            hle_log("hakuX-hle-back", "quirks: \"%s\" is not a quirk (ignored)", name);
        }
        mask = off ? mask & ~bits : mask | bits;
    }
    return mask;
}

static void set(uint32_t mask, const char *why)
{
    char on[256], def[256];
    g_hle_quirks = mask;
    names(mask, on, sizeof(on));
    names(g_defaults, def, sizeof(def));
    hle_log("hakuX-hle-back", "quirks: %s (driver defaults %s%s%s)", on, def, why ? "; " : "",
            why ? why : "");
}

void hle_quirks_init(VkPhysicalDevice phys)
{
    HleDriver d = { .drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES } };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                       .pNext = &d.drv };
    vkGetPhysicalDeviceProperties2(phys, &p2);
    d.props = p2.properties;
    g_defaults = 0;
    for (size_t i = 0; i < ARRAY_SIZE(k_quirks); i++) {
        if (k_quirks[i].on && k_quirks[i].on(&d)) {
            g_defaults |= k_quirks[i].bit;
        }
    }
    hle_log("hakuX-hle-back", "vk: driver \"%s\" (id %u) %s", d.drv.driverName, d.drv.driverID,
            d.drv.driverInfo);
    const char *e = getenv("HAKUX_HLE_QUIRKS");
    set(apply(g_defaults, e), e ? "HAKUX_HLE_QUIRKS" : NULL);
}

void hle_quirks_poll(void)
{
#ifdef __ANDROID__
    static char last[PROP_VALUE_MAX];
    static uint32_t polls;
    if (polls++ % 30) {
        return;
    }
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.quirks", v);
    if (strcmp(v, last)) {
        strcpy(last, v);
        set(apply(g_defaults, v), v[0] ? "debug.hakux.quirks" : NULL);
    }
#endif
}

