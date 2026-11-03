/*
 * hakuX front end — what the dashboard reads from discs and saves
 * (xbox_data.c): an image's game (its XBE's certificate and title image),
 * XPR0 images (title and save icons) as RGBA.
 */
#ifndef NATIVE_APP_UI_XBOX_DATA_H
#define NATIVE_APP_UI_XBOX_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XdGame {
    char path[1024];        /* the image */
    char title[128];        /* the certificate's name (UTF-8), else the file's */
    uint32_t title_id, version, regions;
    char key[24];           /* "%08X.%u": its settings', mods', states' folder */
    uint8_t *icon;          /* RGBA, icon_w x icon_h, or NULL */
    int icon_w, icon_h;
    int64_t size;
} XdGame;

/* The game on an Xbox disc image (XISO, or a full dump's game partition).
 * False when it isn't one; the title is the file's name then. */
bool xd_read_game(const char *path, XdGame *g);
void xd_free_game(XdGame *g);
/* A file's name without its folders, extension and (...) [...] groups. */
void xd_clean_name(const char *path, char *out, size_t n);

/* An XPR0 texture (a title or save image) as RGBA (malloc'ed). */
uint8_t *xd_xpr_decode(const uint8_t *b, size_t n, int *w, int *h);

/* UTF-16LE (n code units) to UTF-8. */
void xd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap);

#endif
