/*
 * hakuX native core — a run's options, the arguments a front end passes on
 * (the app's NativeCoreActivity.getArguments, xboxrun's command line):
 *
 *   uncap            every frame presented at once, without waiting for
 *                    vblank (the HLE GPU's HAKUX_HLE_UNCAP)
 *   relaunch         a title's reboot into another of its XBEs
 *   loadstate=FILE   boot into a savestate (native/state)
 *   mods=FILE        the run's mods and cheats (native/core/mods.h)
 *   version=NAME     the core's version, recorded in savestates
 *   scale=N          the render scale
 *   asyncpipe=0|1    asynchronous shader compilation (on when absent)
 *   prebuild=0|1     the cached shaders built before the game (on)
 *   pads=MASK        the players with a controller plugged in (hex, bit n
 *                    player n + 1; default player 1)
 *   video=FLAGS      the console's video settings (XC_VIDEO_FLAGS, hex)
 *   avpack=hdtv|standard|svideo|scart|vga   its AV cable
 *   pal60=0|1        PAL titles at 60 Hz (1, the default: PAL-60 on) or 50
 *   lang=N           its language (XC_LANGUAGE: 1 English ... 9 Portuguese)
 *   clock=MS         its clock's distance from the phone's time
 *   tz=MINUTES       its time zone, east of UTC
 *   net=off|nat|lan|kai  its network (native/net/net.h); with it dns=,
 *                    lanport=, lanbcast=, lanpeers=, kai=
 *
 * The front end's own (the picture's shape and filters, the input, the
 * Vulkan driver) stay with it.
 */
#ifndef NATIVE_CORE_OPTIONS_H
#define NATIVE_CORE_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NcOptions {
    bool uncap, async_pipe, prebuild;
    int64_t video;              /* -1: the core's default */
    int av_pack;                /* NC_AV_PACK_* */
} NcOptions;

/* The app's defaults. */
void nc_options_init(NcOptions *o);
/* One argument: true if it is one of the core's (applied now, or kept for
 * nc_options_apply). */
bool nc_option(NcOptions *o, const char *arg);
/* After the arguments, before boot: what needs them all (the video
 * settings, the frame pacing, the shader options). */
void nc_options_apply(const NcOptions *o);

#endif
