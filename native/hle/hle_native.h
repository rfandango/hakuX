/*
 * hakuX native core — the HLE GPU layer (hw/xbox/hle) on the native core:
 * its host services (host.h), guest-write tracking and the debug probes.
 */
#ifndef NATIVE_HLE_HLE_NATIVE_H
#define NATIVE_HLE_HLE_NATIVE_H

#include "native/kernel/kernel.h"
#include "hw/xbox/hle/host.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before the guest runs: the HLE layer's init (posture from HAKUX_HLE_MODE;
 * off unless set) and, if active, its title start (push-buffer mode). */
void nc_hle_boot(NcKernel *k, NcVm *vm, NcCpu *cpu, const NcCpuContext *boot);

/* Why the title can't run on this device (the HLE GPU did not start), or
 * NULL. */
const char *nc_hle_fatal(void);
/* Whose that is: the Vulkan driver's (it lacks what the HLE GPU needs) or
 * the title's (the HLE GPU does not support it). NC_FATAL_GPU_LOST: the
 * driver lost the device while the game ran (hle_host_gpu_lost).
 * NC_FATAL_BOOT: the title didn't start (its file or XBE can't be read, the
 * console couldn't be set up), the text saying which. */
enum { NC_FATAL_NONE, NC_FATAL_DRIVER, NC_FATAL_TITLE, NC_FATAL_STATE, NC_FATAL_GPU_LOST, NC_FATAL_BOOT };
/* Why the game can't go on: the core's own reasons (a savestate that
 * couldn't load: NC_FATAL_STATE). */
void nc_hle_set_fatal(int kind, const char *why);
int nc_hle_fatal_kind(void);
/* The title renders by push-buffer decoding (pbmode.h). */
bool nc_hle_pb_active(void);
/* Before boot: the vkGetInstanceProcAddr of a custom Vulkan driver (Turnip
 * through adrenotools) for the HLE GPU, instead of the system driver's. */
void nc_hle_set_vk_loader(void *gipa);
/* Before boot: the HLE GPU's render scale (1-4x the title's resolution). */
void nc_hle_set_render_scale(int scale);
/* Before boot: the decoder's frame pace (the app's "Frame pacing" setting). */
void nc_hle_set_frame_pace(int on);
/* Before boot: the app's "Asynchronous shader compilation" and "Build cached
 * shaders before starting" settings (hw/xbox/hle/vk/pipeasync.h). */
void nc_hle_set_pipeline_options(bool async, bool prebuild);
/* The pipeline prebuild's progress, any thread: built and to build (0 / 0
 * when none runs). */
void nc_hle_pipeline_progress(uint32_t *done, uint32_t *total);

/* host_native.c (host.h on this core): the registers probes see, set up at
 * boot; the JIT flush the HLE asked for, at the end of a system op, or
 * dropped when nothing is translated yet. */
extern HleCpu nc_hle_regs;
void nc_hle_host_init(NcKernel *k, NcVm *vm, NcCpu *cpu);
void nc_hle_host_flush(void);
void nc_hle_host_flush_discard(void);

/* NcCpuConfig.hook_query: a probe at eip (core/probe.h). */
bool nc_hle_hook_query(void *opaque, uint32_t eip);

/* NC_SYSOP_HOOK: the probe at EIP logs; the title's code there runs. */
void nc_hle_hook(NcKernel *k, NcFrame *f);

/* At the end of a system op: work the HLE layer asked for (a JIT flush). */
void nc_hle_after_sysop(NcKernel *k);

/* Guest-write tracking (dirty.c), from any thread's SIGSEGV/SIGBUS: a write
 * to a page protected for tracking. Returns false if it isn't one. */
bool nc_hle_dirty_fault(const siginfo_t *info);

/* debug: NC_WATCHPA="pa:len[:seconds]" (hex, then decimal), stores to that
 * guest RAM logged (hakuX-native-watch), armed after the seconds: a guest
 * store with its exact EIP and the stack's return addresses, from the fault
 * (nc_hle_watch_store), a host one (a kernel call or the HLE writing guest
 * memory) with its length. The range is write-protected again after each
 * system op (nc_hle_watch_rearm), so while the watch is on the HLE takes
 * those pages as clean; armed from boot it can upset a title's start (PGR
 * died in a read completion), hence the delay. Device writes (APU DMA) are
 * NC_DEVWATCH's (dirty.c). */
bool nc_hle_watch_pa(uint32_t pa);
void nc_hle_watch_store(uint32_t va, uint32_t pa, uint32_t eip, uint32_t len);
void nc_hle_watch_rearm(void);

#ifdef __cplusplus
}
#endif

#endif
