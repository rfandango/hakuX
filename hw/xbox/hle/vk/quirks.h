/*
 * hakuX HLE GPU — Vulkan driver quirks: what a driver gets wrong that the
 * back end works around. Each quirk is a bit with a name and the drivers it
 * is on for by default, picked from the driver's identity at device
 * creation (quirks.c). A new one is a row in that table and a test of
 * hle_quirk() where the back end works around it.
 *
 * A/B on any driver: HAKUX_HLE_QUIRKS at start, or the live property
 * debug.hakux.quirks: names separated by commas or spaces, "+name" (or the
 * bare name) on and "-name" off on top of the driver's defaults, "+all" /
 * "-all" every quirk. Empty: the driver's defaults.
 */
#ifndef HW_XBOX_HLE_VK_QUIRKS_H
#define HW_XBOX_HLE_VK_QUIRKS_H

#include "hw/xbox/hle/vk/vkdev.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    /* Binding a pipeline drops the dynamic vertex input: a draw whose input
     * equals the last one's (so the back end does not record it again)
     * fetches another layout's attributes. */
    HLE_QUIRK_VI_AFTER_PIPELINE = 1u << 0,
    /* Every dynamic state recorded again after a pipeline bind: the broad
     * form of the one above, for telling which state a driver loses. */
    HLE_QUIRK_STATE_AFTER_PIPELINE = 1u << 1,
    /* Two programs with the same invariant position math come out a depth
     * step apart when they run in different command buffers: an EQUAL pass
     * that does not write misses whole triangles. Such passes test a step
     * toward the eye and discard in the shader what is further in front
     * (vk/draw.c eq_tol, hle_draw_zeq). */
    HLE_QUIRK_EQ_DEPTH_STEP = 1u << 2,
    /* A depth clear by the pass's loadOp (an LRZ fast clear the driver keeps
     * on chip) is lost when the pass's first draws run without LRZ (the
     * depth test off: a backdrop): the depth-tested draws after them read
     * the LRZ state stored at the last pass's end and are rejected in blocks
     * of 128 x 32 pixels against the previous frame's depth. Such a pass is
     * preceded by an empty one of one pixel that clears depth, whose end
     * stores the cleared state (back.c open_pass). */
    HLE_QUIRK_LRZ_CLEAR = 1u << 3,
    /* A pipeline without a colour attachment sets the blend constants to
     * zero but keeps the ones set last as current: set again unchanged they
     * record nothing, and draws blending by them use zero. The first colour
     * draw after such a pipeline sets other constants before its own
     * (vk/draw.c). */
    HLE_QUIRK_BLEND_CONST_RESET = 1u << 4,
    /* From ARMSX2 (pcsx2/GS/Renderers/Vulkan GSDeviceVK, VKSwapChain), most of
     * them PPSSPP's first, for the drivers other than Adreno's. */
    /* vkCmdPushDescriptorSetKHR crashes even where the extension is listed
     * (Arm's driver): fbfetch's set 2 comes from a pool (vk/draw.c). Read
     * when the back end starts. */
    HLE_QUIRK_NO_PUSH_DESC = 1u << 5,
    /* A packed depth/stencil attachment whose depth and stencil load ops
     * differ corrupts (Mali): such a pass loads both and clears the other
     * aspect in it (back.c open_pass). */
    HLE_QUIRK_DS_SAME_LOAD = 1u << 6,
    /* A clip-space z equal to w corrupts the depth buffer (Mali): the vertex
     * shaders move such a vertex a hair inside (shader/ff.h). Read when the
     * back end starts: it changes the shaders. */
    HLE_QUIRK_Z_EQ_W = 1u << 7,
    /* A colour target's first pass loads its undefined contents (no clear
     * pending): zeros on Adreno, anything on GPUs that compress their
     * attachments (Mali's AFBC, Xclipse's DCC): it clears to zero there. */
    HLE_QUIRK_FRESH_CLEAR = 1u << 8,
    /* Older PowerVR drivers corrupt the output unless the swapchain is a
     * multiple of 32 pixels wide: its width is rounded down to one. */
    HLE_QUIRK_SWAP_W32 = 1u << 9,
    /* Timestamp queries never resolve (Mali-G615): the GPU time profile
     * (debug) is off. */
    HLE_QUIRK_NO_TIMESTAMPS = 1u << 10,
    /* A present with a semaphore to wait on waits for it on the CPU, the
     * queue held (Qualcomm's: Android's present asks the driver for a fence
     * over it): the presenter waits for its pass itself, unlocked, and
     * presents with nothing to wait for (vk/swapchain.c). On for every
     * driver but Turnip, whose present hands the compositor a fence. */
    HLE_QUIRK_PRESENT_CPU_WAIT = 1u << 11,
    /* A uniform buffer indexed per vertex (the vertex programs' c[A0 + n],
     * skinning's bone matrices) reads wrong values (Samsung's Xclipse): the
     * constants (set 1, VshConst / FfConst) are a read-only storage buffer
     * instead, same std140 layout and dynamic offsets (vk/draw.c,
     * shader/ff.h g_hle_const_ssbo). Read when the back end starts: it
     * changes the shaders. */
    HLE_QUIRK_CONST_SSBO = 1u << 12,
    /* Programs' S32K attributes are fetched as integers (R16..._SINT) and
     * converted in the shader; with this quirk as SSCALED floats, the same
     * values (Xclipse, whose missing R16G16B16_SINT fell back to the
     * four-component form, w read from the next 2 bytes). Where a driver
     * lacks an n-component SINT format but has the SSCALED one, that is used
     * regardless (vk/draw.c vat_format). Read when the back end starts. */
    HLE_QUIRK_S32K_SCALED = 1u << 13,
};

/* Samsung's Xclipse GPUs (AMD RDNA in Exynos SoCs): their PCI vendor ID, and
 * VK_DRIVER_ID_SAMSUNG_PROPRIETARY (21) by number, for headers older than it */
#define HLE_VENDOR_SAMSUNG    0x144Du
#define HLE_DRIVER_ID_SAMSUNG 21u

extern uint32_t g_hle_quirks;

static inline bool hle_quirk(uint32_t q)
{
    return (g_hle_quirks & q) != 0;
}

/* The driver's quirks (the device is created), and HAKUX_HLE_QUIRKS. */
void hle_quirks_init(VkPhysicalDevice phys);

/* Once a frame: debug.hakux.quirks changed. */
void hle_quirks_poll(void);

#endif
