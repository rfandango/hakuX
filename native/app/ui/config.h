/*
 * hakuX front end — settings files (config.c). One option a line, as the
 * command line takes them ("scale=2", "uncap", "mod=60fps"); '#' comments.
 * xbased.ini holds the global ones, games/<TITLE ID>.<version>.ini a game's
 * (the game's override the global, the command line both).
 */
#ifndef NATIVE_APP_UI_CONFIG_H
#define NATIVE_APP_UI_CONFIG_H

#include <stdbool.h>

#define CFG_MAX 256

typedef struct Config {
    char path[1024];
    int n;
    char key[CFG_MAX][48];
    char val[CFG_MAX][464];     /* "" for a flag ("uncap"); "1" is written as the flag */
    bool flag[CFG_MAX];
    char comments[2048];        /* the file's leading comment lines, kept */
} Config;

void cfg_load(Config *c, const char *path);
bool cfg_save(const Config *c);
const char *cfg_get(const Config *c, const char *key);    /* NULL: unset */
int cfg_int(const Config *c, const char *key, int def);
bool cfg_bool(const Config *c, const char *key, bool def);
void cfg_set(Config *c, const char *key, const char *val);
void cfg_set_int(Config *c, const char *key, int v);
void cfg_set_bool(Config *c, const char *key, bool v);
void cfg_unset(Config *c, const char *key);
/* Keys that may repeat (mod=, cheat=): all their values. */
int cfg_all(const Config *c, const char *key, const char **out, int max);
void cfg_add(Config *c, const char *key, const char *val);
void cfg_remove_value(Config *c, const char *key, const char *val);

/* Where the emulator keeps its data (xbased.ini, hdd, states, games/,
 * covers/): beside the executable (Windows, or a portable.txt there), else
 * the user's data folder (Linux: ~/.local/share/hakuX). Ends in a separator. */
const char *cfg_data_dir(void);
/* Beside the executable (fonts, mods). */
const char *cfg_exe_dir(void);

#endif
