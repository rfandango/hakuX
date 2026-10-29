/*
 * hakuX HLE GPU — runtime state and the host-facing API.
 */
#ifndef HW_XBOX_HLE_HLE_H
#define HW_XBOX_HLE_HLE_H

#include "hw/xbox/hle/core/xbe.h"

typedef enum HlePosture {
    HLE_POSTURE_OFF = 0,
    HLE_POSTURE_REPLACE = 2,
} HlePosture;

#define HLE_MAX_LIB_RANGES 8

typedef struct HleTitle {
    uint64_t fp;
    HleXbeInfo xbe;
    /* the D3D library's sections (diagnostics) */
    uint32_t lib_lo[HLE_MAX_LIB_RANGES], lib_hi[HLE_MAX_LIB_RANGES];
    unsigned nlib;
} HleTitle;

typedef struct HleRuntime {
    HlePosture posture;
    HleTitle title;
} HleRuntime;

extern HleRuntime g_hle;

/* Why the HLE GPU could not start (the Vulkan driver's shortfall), for the
 * user; empty while it can. */
extern char g_hle_unavailable[256];

void hle_init(void);
/* The core stops (M2; the guest's CPU has left): the HLE GPU's threads
 * joined, the renderer torn down (the device stays: hle_vk_shutdown) and
 * its state as before hle_init. */
void hle_shutdown(void);
const char *hle_posture_name(HlePosture p);
void hle_log(const char *tag, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* The HLE GPU renders the titles (HAKUX_HLE_MODE=replace). */
static inline bool hle_active(void)
{
    return g_hle.posture != HLE_POSTURE_OFF;
}

/* Title lifecycle (core/title.c): the XBE mapped now, in push-buffer mode
 * (vCPU thread, before its code runs). */
void hle_title_start(void);
void hle_title_stop(void);
bool hle_title_in_lib(uint32_t va);

#endif
