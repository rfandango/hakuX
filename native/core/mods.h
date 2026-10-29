/*
 * hakuX native core — mods and cheats for the title (mods/README.md), from
 * the manifest the app writes for the run (NativeCoreActivity, "mods=<file>"):
 *
 *   file  <TAB> <disc path> <TAB> <host file>      a disc file replaced or added
 *   patch <TAB> <XBE on the disc> <TAB> <mod> <TAB> <line of its mod.txt section>
 *   patchoff <TAB> ...    the same of a mod that is off: after a savestate
 *                        made with it on, its bytes go back to the disc's
 *   cheat <TAB> <0|1 on> <TAB> <0|1 once> <TAB> <name>
 *   code  <TAB> <a code line of the cheat above>
 *   refresh <TAB> <Hz>   the vblank's rate for a 60 Hz video mode (a 50 Hz
 *                        one scales with it)
 *
 * Files come before patches (a patch reads the file it patches).
 */
#ifndef NATIVE_CORE_MODS_H
#define NATIVE_CORE_MODS_H

#include "native/core/vm.h"
#include "native/core/xdvdfs.h"
#include "hw/xbox/hle/core/xbe.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NcCpu;
struct NcKernel;

/* The manifest's path (app_main.c), read at boot. */
void nc_mods_set_manifest(const char *path);
/* After the disc opens: its overlay (files, patches) and the cheats. */
void nc_mods_install(NcDisc *disc);
/* The core stopped: the patches remembered, the cheats and the per-boot
 * state dropped (the manifest, the front end's, stays). */
void nc_mods_reset(void);
/* Once the title's XBE is in memory and its CPU made: where the cheats
 * write (the XBE's executable sections drop translations they change). */
void nc_mods_bind(NcVm *vm, struct NcCpu *cpu, const HleXbeInfo *xbe);
/* The guest CPU's thread at each kernel exit: the cheats once a vblank. */
void nc_mods_poll(void);
/* After a savestate loaded (its memory holds the code as it was saved, with
 * the mods on then: its MODS section): their patches taken back and the
 * running XBE's of the mods on now written. Where the header page gets other
 * code, the mods' space there is cleared and the memory its code of then
 * allocated freed. */
void nc_mods_after_load(struct NcKernel *k, const char *image_path);

#ifdef __cplusplus
}
#endif

#endif
