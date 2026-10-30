/*
 * hakuX native core — a custom Vulkan driver for the HLE GPU (Turnip),
 * through adrenotools: the app's GPU driver setting ("vkdriver=" from
 * NativeCoreActivity), and xboxrun's.
 */
#include "native/app/app.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/prop.h"

#ifdef NC_ADRENOTOOLS
#include <adrenotools/driver.h>
#include <dlfcn.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int g_kgsl_prio;                 /* nc_vk_driver_kgsl_priority */

int nc_vk_driver_kgsl_priority(void)
{
    return g_kgsl_prio;
}

#ifdef NC_ADRENOTOOLS
/* The driver package's meta.json names its vendor: Turnip's is "Mesa". */
static bool driver_is_mesa(const char *dir)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s%smeta.json", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char json[4096];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = 0;
    const char *v = strstr(json, "\"vendor\"");
    v = v ? strchr(v + 8, ':') : NULL;
    v = v ? strchr(v, '"') : NULL;
    return v && !strncasecmp(v + 1, "mesa\"", 5);
}

/* Turnip leaves its kgsl draw contexts at the kernel's default priority (8:
 * the third of four ringbuffers, priority / 4), below SurfaceFlinger's
 * GPU composition (the second on the RP5): while the screen is composited
 * on the GPU (Developer options' "Disable HW overlays", or a layer the
 * display hardware can't take), its work preempts ours some 100 times a
 * second, and a Turnip sysmem pass on Adreno 6xx preempted with its draws
 * in flight hangs the GPU (PGR2's race: 4-9 hangs in 6 minutes,
 * VK_ERROR_DEVICE_LOST). On the compositor's ringbuffer nothing of it
 * preempts them (0 in 12 minutes); at its lowest priority there, 7, the
 * dispatcher still runs the compositor's work first (2 late frames in 6
 * minutes; priority 1, the first ringbuffer, preempts the compositor: 433).
 * debug.hakux.kgslprio: another priority (1..15), 0 for the driver's own. */
static int kgsl_priority(const char *dir)
{
    /* KGSL only: the pack's vendor says "Mesa" for Turnip, which is also
     * built for Mali devices (malisx2 is not "Mesa", and a Mali pack has
     * nothing to set here anyway). */
    if (!driver_is_mesa(dir) || access("/dev/kgsl-3d0", F_OK) != 0) {
        return -1;
    }
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.kgslprio", v);
    int prio = v[0] ? atoi(v) : 7;
    return prio >= 1 && prio <= 15 ? prio : -1;
}
#endif

void nc_vk_driver_load(const char *spec)
{
#ifdef NC_ADRENOTOOLS
    /* Turnip's debug flags for A/B runs (debug.hakux.tu_debug, e.g.
     * "sysmem"): read when the driver creates its device. */
    char tu[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.tu_debug", tu);
    if (tu[0]) {
        setenv("TU_DEBUG", tu, 1);
        nc_log("hakuX-app", "TU_DEBUG=%s", tu);
    }
    /* debug.hakux.vkdriver: "system", the system driver for this run; a
     * directory, the driver library of the setting's name from there (another
     * build unpacked); the setting kept (A/B: a driver's bug or the HLE's,
     * one driver build against another) */
    char sel[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.vkdriver", sel);
    if (!strcmp(sel, "system")) {
        nc_log("hakuX-app", "Vulkan driver: the system's (debug.hakux.vkdriver=system)");
        return;
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", spec);
    char *lib = strchr(dir, '|');
    char *hooks = lib ? strchr(lib + 1, '|') : NULL;
    if (lib && hooks) {
        *lib++ = 0;
        *hooks++ = 0;
        const char *from = sel[0] == '/' ? sel : dir;
        const int prio = kgsl_priority(from);
        adrenotools_set_kgsl_context_priority(prio);
        void *h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM |
                                             (prio > 0 ? ADRENOTOOLS_DRIVER_KGSL_CONTEXT_PRIORITY : 0),
                                             NULL, hooks, from, lib, NULL, NULL);
        void *gipa = h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
        if (gipa) {
            nc_hle_set_vk_loader(nc_vk_layers(gipa));
        }
        nc_log("hakuX-app", "Vulkan driver %s from %s%s: %s", lib, from, from == sel ? " (debug.hakux.vkdriver)" : "",
               gipa ? "loaded" : "failed, the system driver it is");
        if (gipa && prio > 0) {
            g_kgsl_prio = prio;
            nc_log("hakuX-app", "its kgsl draw contexts at priority %d (ringbuffer %d of 0-3)", prio, prio / 4);
        }
    }
#else
    nc_log("hakuX-app", "Vulkan driver requested, but this build has no adrenotools");
#endif
}
