/*
 * hakuX native core — a run's options (options.h), shared by the app's front
 * end (native/app/app_main.c) and xboxrun (native/run/xboxrun.c).
 */
#include "native/core/options.h"
#include "native/core/core.h"
#include "native/core/mods.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"
#include "native/net/net.h"
#include "native/state/save.h"

#include <stdlib.h>
#include <string.h>

void nc_options_init(NcOptions *o)
{
    *o = (NcOptions){ .async_pipe = true, .prebuild = true, .video = -1, .av_pack = NC_AV_PACK_HDTV };
}

static const char *value(const char *arg, const char *name)
{
    const size_t n = strlen(name);
    return !strncmp(arg, name, n) ? arg + n : NULL;
}

bool nc_option(NcOptions *o, const char *arg)
{
    const char *v;
    if (nc_net_option(arg)) {
        /* net=, dns=, lanport=, lanbcast=, lanpeers=, kai= (native/net/net.h) */
    } else if (!strcmp(arg, "uncap")) {
        o->uncap = true;
    } else if (!strcmp(arg, "relaunch")) {
        nc_core_set_relaunch(true);
    } else if ((v = value(arg, "loadstate="))) {
        nc_core_set_loadstate(v);
        nc_log("hakuX-app", "savestate %s", v);
    } else if ((v = value(arg, "mods="))) {
        nc_mods_set_manifest(v);                /* read as the disc opens */
    } else if ((v = value(arg, "version="))) {
        nc_state_set_core_version(v);
    } else if ((v = value(arg, "pace="))) {
        nc_hle_set_frame_pace(atoi(v));
    } else if ((v = value(arg, "scale="))) {
        nc_hle_set_render_scale(atoi(v));
        nc_log("hakuX-app", "render scale %sx", v);
    } else if ((v = value(arg, "asyncpipe="))) {
        o->async_pipe = atoi(v) != 0;
    } else if ((v = value(arg, "prebuild="))) {
        o->prebuild = atoi(v) != 0;
    } else if ((v = value(arg, "pads="))) {
        setenv("NC_PAD_MASK", v, 1);            /* read by the USB at boot (ohci.c) */
    } else if ((v = value(arg, "video="))) {
        o->video = (int64_t)strtoul(v, NULL, 16);
    } else if ((v = value(arg, "avpack="))) {
        o->av_pack = !strcmp(v, "standard") ? NC_AV_PACK_STANDARD
                     : !strcmp(v, "svideo") ? NC_AV_PACK_SVIDEO
                     : !strcmp(v, "scart")  ? NC_AV_PACK_SCART
                     : !strcmp(v, "vga")    ? NC_AV_PACK_VGA
                                            : NC_AV_PACK_HDTV;
    } else if ((v = value(arg, "lang="))) {
        nc_kernel_set_language(atoi(v));
        nc_log("hakuX-app", "console language %s", v);
    } else if ((v = value(arg, "clock="))) {
        nc_kernel_set_clock(strtoll(v, NULL, 10));  /* none: the phone's time */
    } else if ((v = value(arg, "tz="))) {
        nc_kernel_set_time_zone(atoi(v));           /* none: the phone's zone */
    } else {
        return false;
    }
    return true;
}

void nc_options_apply(const NcOptions *o)
{
    /* The console's video settings: what the title may show (widescreen,
     * 480p/720p/1080i) and the cable it finds (HD modes need component). */
    nc_kernel_set_video(o->video, o->av_pack);
    if (o->video >= 0) {
        nc_log("hakuX-app", "video flags %08llx, AV pack %d", (unsigned long long)o->video, o->av_pack);
    }
    if (o->uncap) {
        setenv("HAKUX_HLE_UNCAP", "1", 1);
    } else {
        unsetenv("HAKUX_HLE_UNCAP");
    }
    nc_hle_set_pipeline_options(o->async_pipe, o->prebuild);
    nc_log("hakuX-app", "shaders: asynchronous compilation %s, cached ones built before the game %s",
           o->async_pipe ? "on" : "off", o->prebuild ? "on" : "off");
}
