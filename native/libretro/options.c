/*
 * hakuX native core — the libretro core's options: what the emulator itself
 * backs, the render scale and the console's settings as its dashboard keeps
 * them. The picture's filters and its shape on screen are RetroArch's (its
 * shaders, its aspect setting).
 *
 * Each option but the overscan crop is read once, before the console boots:
 * the render scale sizes the renderer's targets, and the console's settings
 * are in the EEPROM the title reads as it starts. The core can't boot again
 * in its process (M2), so a change to one during the game takes effect when
 * the game starts next, and RetroArch shows a message saying so. The crop
 * changes at once (video.c).
 */
#include "native/libretro/libretro_int.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEXT " Takes effect when the game starts next."

enum { OPT_SCALE, OPT_CROP, OPT_FORMAT, OPT_LANGUAGE, OPT_REGION, OPT_COUNT };

/* The language values follow XC_LANGUAGE after auto: english is 1. */
static struct retro_core_option_v2_definition g_defs[OPT_COUNT + 1] = {
    [OPT_SCALE] = {
        "hakux_render_scale", "Render Scale", NULL,
        "The resolution games are drawn at: 1x is the Xbox's own, 640x480 for most. Higher is sharper and "
        "costs the GPU more." NEXT,
        NULL, NULL,
        {{"1", "1x (Xbox)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {NULL, NULL}},
        "1"},
    [OPT_CROP] = {
        "hakux_crop_overscan", "Crop Overscan", NULL,
        "Hides the border a TV hid, 1/30 of the picture on each side, so a standard-definition game fills a "
        "4:3 screen. Off shows the whole frame.",
        NULL, NULL,
        {{"enabled", "On"}, {"disabled", "Off"}, {NULL, NULL}},
        "enabled"},
    [OPT_FORMAT] = {
        "hakux_screen_format", "Screen Format", NULL,
        "The TV the console is set up for. Widescreen games then render 16:9 and are shown so, letterbox "
        "ones show 16:9 scenes inside 4:3; games without either ignore it." NEXT,
        NULL, NULL,
        {{"normal", "Normal (4:3)"}, {"widescreen", "Widescreen (16:9)"}, {"letterbox", "Letterbox"},
         {NULL, NULL}},
        "normal"},
    [OPT_LANGUAGE] = {
        "hakux_language", "Language", NULL,
        "The console's language, for games that have several. Auto is RetroArch's, or English where the "
        "Xbox lacks it." NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"english", "English"}, {"japanese", "Japanese"}, {"german", "German"},
         {"french", "French"}, {"spanish", "Spanish"}, {"italian", "Italian"}, {"korean", "Korean"},
         {"chinese", "Chinese (Traditional)"}, {"portuguese", "Portuguese"}, {NULL, NULL}},
        "auto"},
    [OPT_REGION] = {
        "hakux_video_region", "Video Region", NULL,
        "The console's TV standard. Auto follows the game: PAL for a European game, NTSC-M otherwise. PAL "
        "games run at 60 Hz where they offer it; one with only 50 Hz runs at its own speed, less smoothly."
        NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"ntsc-m", "NTSC-M (Americas)"}, {"ntsc-j", "NTSC-J (Japan)"},
         {"pal", "PAL (Europe)"}, {NULL, NULL}},
        "auto"},
    {NULL},
};

static int g_applied[OPT_COUNT];   /* the value each option booted with (its index) */

/* A frontend without the v2 options (or v1, which adds nothing used here):
 * the legacy variables, "Description; default|other|...". */
static void declare_legacy(retro_environment_t env)
{
    static char text[OPT_COUNT][512];
    static struct retro_variable vars[OPT_COUNT + 1];
    for (int i = 0; i < OPT_COUNT; i++) {
        const struct retro_core_option_v2_definition *d = &g_defs[i];
        size_t n = (size_t)snprintf(text[i], sizeof(text[i]), "%s; %s", d->desc, d->default_value);
        for (const struct retro_core_option_value *v = d->values; v->value && n < sizeof(text[i]); v++) {
            if (strcmp(v->value, d->default_value)) {
                n += (size_t)snprintf(text[i] + n, sizeof(text[i]) - n, "|%s", v->value);
            }
        }
        vars[i] = (struct retro_variable){d->key, text[i]};
    }
    env(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

void nc_lr_options_declare(retro_environment_t env)
{
    static struct retro_core_option_v2_category none[] = {{NULL, NULL, NULL}};
    static struct retro_core_options_v2 opts = {none, g_defs};
    unsigned version = 0;
    if (!env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2 ||
        !env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts)) {
        declare_legacy(env);
    }
}

/* The option's value now, as the index of one of its values (the default's
 * for one the frontend doesn't have, or doesn't know). */
static int value_of(int opt)
{
    const struct retro_core_option_v2_definition *d = &g_defs[opt];
    struct retro_variable var = {d->key, NULL};
    const char *want = nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : d->default_value;
    int def = 0;
    for (int i = 0; d->values[i].value; i++) {
        if (!strcmp(d->values[i].value, want)) {
            return i;
        }
        def = strcmp(d->values[i].value, d->default_value) ? def : i;
    }
    return def;
}

/* Auto: RetroArch's language as XC_LANGUAGE, English where the Xbox has none. */
static int auto_language(void)
{
    unsigned lang = RETRO_LANGUAGE_ENGLISH;
    nc_lr_env(RETRO_ENVIRONMENT_GET_LANGUAGE, &lang);
    switch (lang) {
    case RETRO_LANGUAGE_JAPANESE:
        return 2;
    case RETRO_LANGUAGE_GERMAN:
        return 3;
    case RETRO_LANGUAGE_FRENCH:
        return 4;
    case RETRO_LANGUAGE_SPANISH:
        return 5;
    case RETRO_LANGUAGE_ITALIAN:
        return 6;
    case RETRO_LANGUAGE_KOREAN:
        return 7;
    case RETRO_LANGUAGE_CHINESE_TRADITIONAL:
    case RETRO_LANGUAGE_CHINESE_SIMPLIFIED:
        return 8;
    case RETRO_LANGUAGE_PORTUGUESE_BRAZIL:
    case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL:
        return 9;
    default:
        return 1;
    }
}

void nc_lr_options_apply(void)
{
    for (int i = 0; i < OPT_COUNT; i++) {
        g_applied[i] = value_of(i);
    }
    const int scale = atoi(g_defs[OPT_SCALE].values[g_applied[OPT_SCALE]].value);
    nc_hle_set_render_scale(scale);
    const bool crop = strcmp(g_defs[OPT_CROP].values[g_applied[OPT_CROP]].value, "disabled") != 0;
    nc_lr_video_set_crop(crop);

    /* PAL60 always: RetroArch runs at 59.94 Hz, and an NTSC title ignores it.
     * No 480p, 720p or 1080i, as before the options. */
    const char *format = g_defs[OPT_FORMAT].values[g_applied[OPT_FORMAT]].value;
    const uint32_t flags = NC_VIDEO_PAL60 | (!strcmp(format, "widescreen") ? NC_VIDEO_WIDESCREEN
                                             : !strcmp(format, "letterbox") ? NC_VIDEO_LETTERBOX
                                                                            : 0);
    nc_kernel_set_video(flags, NC_AV_PACK_HDTV);
    nc_lr_video_set_widescreen(flags & NC_VIDEO_WIDESCREEN);

    const int language = g_applied[OPT_LANGUAGE] ? g_applied[OPT_LANGUAGE] : auto_language();
    nc_kernel_set_language(language);

    const char *region = g_defs[OPT_REGION].values[g_applied[OPT_REGION]].value;
    nc_kernel_set_av_region(!strcmp(region, "ntsc-m")   ? NC_AV_REGION_NTSC_M
                            : !strcmp(region, "ntsc-j") ? NC_AV_REGION_NTSC_J
                            : !strcmp(region, "pal")    ? NC_AV_REGION_PAL_I
                                                        : 0);
    nc_log("hakuX-libretro", "options: render scale %dx, overscan %s, screen %s, language %d (%s), video region %s",
           scale, crop ? "cropped" : "shown", format, language,
           g_defs[OPT_LANGUAGE].values[g_applied[OPT_LANGUAGE]].value, region);
}

void nc_lr_options_run(void)
{
    bool changed = false;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) || !changed) {
        return;
    }
    const int crop = value_of(OPT_CROP);
    if (crop != g_applied[OPT_CROP]) {
        g_applied[OPT_CROP] = crop;
        nc_lr_video_set_crop(strcmp(g_defs[OPT_CROP].values[crop].value, "disabled") != 0);
    }
    char names[256] = "";
    size_t n = 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (i != OPT_CROP && value_of(i) != g_applied[i] && n < sizeof(names)) {
            n += (size_t)snprintf(names + n, sizeof(names) - n, "%s%s", n ? ", " : "", g_defs[i].desc);
        }
    }
    if (!n) {
        return;
    }
    static char text[320];
    snprintf(text, sizeof(text), "%s: takes effect when the game starts next", names);
    const struct retro_message msg = {text, 240};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
    nc_log("hakuX-libretro", "options: %s", text);
}
