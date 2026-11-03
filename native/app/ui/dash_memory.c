/*
 * hakuX front end — the dashboard's Memory: the games' own saves on the
 * console's hard disk (E:\UDATA\<title ID>\<save>, Partition1 of the hdd
 * folder), with the names and icons the games gave them (TitleMeta.xbx,
 * SaveMeta.xbx: UTF-16 Key=Value; TitleImage.xbx, SaveImage.xbx: XPR0),
 * copied out (exports/), back in, or deleted; and their save states.
 */
#include "native/app/ui/dash_int.h"
#include "native/app/ui/xbox_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TITLES 256
#define MAX_SAVES 128

typedef struct {
    char dir[1100];         /* its folder */
    char id[16];            /* the title ID / the save's folder name */
    char name[128];
    SDL_Texture *icon;
    int64_t bytes;
    int64_t mtime;
    bool exported_only;     /* in exports/, not on the disk (an import) */
} Entry;

static Entry g_titles[MAX_TITLES], g_saves[MAX_SAVES];
static int g_ntitles, g_nsaves;
static bool g_loaded;
static Entry *g_title;      /* the open one */
static int g_target;        /* the save asked about */

static void udata_dir(char *out, size_t n)
{
    const char *h = cfg_get(&D.global, "hdd");
    if (h && *h) {
        snprintf(out, n, "%s/Partition1/UDATA", h);
    } else {
        snprintf(out, n, "%shdd/Partition1/UDATA", cfg_data_dir());
    }
}

static void exports_dir(char *out, size_t n)
{
    snprintf(out, n, "%sexports", cfg_data_dir());
}

/* Key=Value lines in UTF-16LE: the value of key */
static bool meta_value(const char *path, const char *key, char *out, size_t n)
{
    size_t len = 0;
    uint8_t *b = SDL_LoadFile(path, &len);
    if (!b) {
        return false;
    }
    char text[2048];
    size_t off = len >= 2 && b[0] == 0xFF && b[1] == 0xFE ? 2 : 0;
    xd_utf16_to_utf8((const uint16_t *)(b + off), (len - off) / 2, text, sizeof(text));
    SDL_free(b);
    const size_t kl = strlen(key);
    for (char *s = text; s && *s;) {
        char *e = strpbrk(s, "\r\n");
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            snprintf(out, n, "%.*s", e ? (int)(e - s - kl - 1) : (int)strlen(s + kl + 1), s + kl + 1);
            return true;
        }
        s = e ? e + strspn(e, "\r\n") : NULL;
    }
    return false;
}

static SDL_Texture *xpr_texture(const char *path)
{
    size_t len = 0;
    uint8_t *b = SDL_LoadFile(path, &len);
    if (!b) {
        return NULL;
    }
    int w, h;
    uint8_t *px = xd_xpr_decode(b, len, &w, &h);
    SDL_free(b);
    if (!px) {
        return NULL;
    }
    SDL_Texture *t = ui_texture_rgba(px, w, h);
    free(px);
    return t;
}

static int64_t dir_bytes(const char *dir, int64_t *mtime)
{
    int64_t total = 0;
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1300];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(p, &pi)) {
            if (pi.type == SDL_PATHTYPE_DIRECTORY) {
                total += dir_bytes(p, mtime);
            } else {
                total += (int64_t)pi.size;
                if (mtime && pi.modify_time > *mtime) {
                    *mtime = pi.modify_time;
                }
            }
        }
    }
    SDL_free(names);
    return total;
}

static bool remove_tree(const char *dir)
{
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1300];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
            remove_tree(p);
        } else {
            SDL_RemovePath(p);
        }
    }
    SDL_free(names);
    return SDL_RemovePath(dir);
}

static bool copy_tree(const char *from, const char *to)
{
    SDL_CreateDirectory(to);
    int n = 0;
    bool ok = true;
    char **names = SDL_GlobDirectory(from, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char a[1300], b[1300];
        snprintf(a, sizeof(a), "%s/%s", from, names[i]);
        snprintf(b, sizeof(b), "%s/%s", to, names[i]);
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(a, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
            ok &= copy_tree(a, b);
        } else {
            ok &= SDL_CopyFile(a, b);
        }
    }
    SDL_free(names);
    return ok;
}

static void free_entries(Entry *e, int n)
{
    for (int i = 0; i < n; i++) {
        if (e[i].icon) {
            SDL_DestroyTexture(e[i].icon);
            e[i].icon = NULL;
        }
    }
}

static int cmp_name(const void *a, const void *b)
{
    return SDL_strcasecmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

static void load_titles(void)
{
    free_entries(g_titles, g_ntitles);
    g_ntitles = 0;
    char ud[1100];
    udata_dir(ud, sizeof(ud));
    int n = 0;
    char **names = SDL_GlobDirectory(ud, NULL, 0, &n);
    for (int i = 0; names && i < n && g_ntitles < MAX_TITLES; i++) {
        if (strlen(names[i]) != 8) {
            continue;
        }
        Entry *e = &g_titles[g_ntitles];
        memset(e, 0, sizeof(*e));
        snprintf(e->dir, sizeof(e->dir), "%s/%s", ud, names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(e->dir, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
            continue;
        }
        snprintf(e->id, sizeof(e->id), "%s", names[i]);
        char p[1300];
        snprintf(p, sizeof(p), "%s/TitleMeta.xbx", e->dir);
        if (!meta_value(p, "TitleName", e->name, sizeof(e->name))) {
            snprintf(e->name, sizeof(e->name), "%s", names[i]);
        }
        snprintf(p, sizeof(p), "%s/TitleImage.xbx", e->dir);
        e->icon = xpr_texture(p);
        e->bytes = dir_bytes(e->dir, &e->mtime);
        g_ntitles++;
    }
    SDL_free(names);
    qsort(g_titles, g_ntitles, sizeof(Entry), cmp_name);
    g_loaded = true;
}

static void load_saves(void)
{
    free_entries(g_saves, g_nsaves);
    g_nsaves = 0;
    int n = 0;
    char **names = SDL_GlobDirectory(g_title->dir, NULL, 0, &n);
    for (int i = 0; names && i < n && g_nsaves < MAX_SAVES; i++) {
        Entry *e = &g_saves[g_nsaves];
        memset(e, 0, sizeof(*e));
        snprintf(e->dir, sizeof(e->dir), "%s/%s", g_title->dir, names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(e->dir, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
            continue;
        }
        snprintf(e->id, sizeof(e->id), "%.15s", names[i]);
        char p[1300];
        snprintf(p, sizeof(p), "%s/SaveMeta.xbx", e->dir);
        if (!meta_value(p, "Name", e->name, sizeof(e->name))) {
            snprintf(e->name, sizeof(e->name), "%s", names[i]);
        }
        snprintf(p, sizeof(p), "%s/SaveImage.xbx", e->dir);
        e->icon = xpr_texture(p);
        if (!e->icon) {
            snprintf(p, sizeof(p), "%s/SaveImage.xbx", g_title->dir);
            e->icon = xpr_texture(p);
        }
        e->bytes = dir_bytes(e->dir, &e->mtime);
        g_nsaves++;
    }
    SDL_free(names);
    /* saves exported earlier and gone from the disk: offered back */
    char ex[1100], exd[1200];
    exports_dir(ex, sizeof(ex));
    snprintf(exd, sizeof(exd), "%s/%s", ex, g_title->id);
    names = SDL_GlobDirectory(exd, NULL, 0, &n);
    for (int i = 0; names && i < n && g_nsaves < MAX_SAVES; i++) {
        bool have = false;
        for (int k = 0; k < g_nsaves; k++) {
            have |= !strcmp(g_saves[k].id, names[i]);
        }
        char d[1300];
        snprintf(d, sizeof(d), "%s/%s", exd, names[i]);
        SDL_PathInfo pi;
        if (have || !SDL_GetPathInfo(d, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
            continue;
        }
        Entry *e = &g_saves[g_nsaves++];
        memset(e, 0, sizeof(*e));
        snprintf(e->dir, sizeof(e->dir), "%s", d);
        snprintf(e->id, sizeof(e->id), "%.15s", names[i]);
        char p[1400];
        snprintf(p, sizeof(p), "%s/SaveMeta.xbx", d);
        if (!meta_value(p, "Name", e->name, sizeof(e->name))) {
            snprintf(e->name, sizeof(e->name), "%s", names[i]);
        }
        snprintf(p, sizeof(p), "%s/SaveImage.xbx", d);
        e->icon = xpr_texture(p);
        e->bytes = dir_bytes(d, &e->mtime);
        e->exported_only = true;
    }
    SDL_free(names);
}

static void size_text(int64_t bytes, char *out, size_t n)
{
    /* the console counts 16 KiB blocks */
    snprintf(out, n, "%lld %s", (long long)((bytes + 16383) / 16384), T(S_BLOCKS));
}

static void del_title(Dash *d)
{
    (void)d;
    remove_tree(g_titles[g_target].dir);
    load_titles();
}

void scr_memory(void)
{
    const float W = ui_w(), H = ui_h();
    if (!g_loaded || D.anim == 0) {
        load_titles();
    }
    int64_t total = 0;
    for (int i = 0; i < g_ntitles; i++) {
        total += g_titles[i].bytes;
    }
    char sub[96];
    size_text(total, sub, sizeof(sub));
    draw_header(T(S_MEMORY), sub);
    if (!g_ntitles) {
        ui_text(W / 2, H * 0.45f, T(S_NO_SAVES), 26, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
    }
    List l;
    list_begin(&l, 74, 150, W - 148, H - 244, 76);
    for (int i = 0; i < g_ntitles; i++) {
        Entry *e = &g_titles[i];
        char sz[48];
        size_text(e->bytes, sz, sizeof(sz));
        if (list_row_icon(&l, e->icon, e->name, e->id, sz, NULL)) {
            g_title = e;
            D.sel[SCR_MEMORY_TITLE] = 0;
            load_saves();
            dash_push(SCR_MEMORY_TITLE);
        }
        if (list_selected(&l) && D.in.x) {
            g_target = i;
            dash_ask(T(S_CONFIRM_DELETE), del_title);
        }
    }
    list_end(&l);
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), T(S_DELETE), NULL);
}

static void del_save(Dash *d)
{
    (void)d;
    remove_tree(g_saves[g_target].dir);
    load_saves();
}

void scr_memory_title(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(g_title->name, T(S_SAVED_GAMES));
    List l;
    list_begin(&l, 74, 150, W - 148, H - 244, 76);
    for (int i = 0; i < g_nsaves; i++) {
        Entry *e = &g_saves[i];
        char sub[160] = "", sz[48];
        if (e->mtime) {
            SDL_DateTime dt;
            SDL_TimeToDateTime(e->mtime, &dt, true);
            snprintf(sub, sizeof(sub), "%04d-%02d-%02d %02d:%02d%s", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                     e->exported_only ? "  [exports]" : "");
        }
        size_text(e->bytes, sz, sizeof(sz));
        list_row_icon(&l, e->icon, e->name, sub, sz, NULL);
        if (list_selected(&l)) {
            if (D.in.x && !e->exported_only) {
                g_target = i;
                dash_ask(T(S_CONFIRM_DELETE), del_save);
            }
            if (D.in.y || D.in.ok) {
                char ex[1100], to[1400];
                exports_dir(ex, sizeof(ex));
                SDL_CreateDirectory(ex);
                if (e->exported_only) {
                    /* back to the disk */
                    snprintf(to, sizeof(to), "%s/%s", g_title->dir, e->id);
                    if (copy_tree(e->dir, to)) {
                        dash_toast(T(S_IMPORT));
                        load_saves();
                        break;
                    }
                } else {
                    snprintf(to, sizeof(to), "%s/%s", ex, g_title->id);
                    SDL_CreateDirectory(to);
                    /* the title's own files go with it: icons and names on another console */
                    char tf[1300], tt[1500];
                    static const char *const k_tf[] = {"TitleMeta.xbx", "TitleImage.xbx", "SaveImage.xbx"};
                    for (int f = 0; f < 3; f++) {
                        snprintf(tf, sizeof(tf), "%s/%s", g_title->dir, k_tf[f]);
                        snprintf(tt, sizeof(tt), "%s/%s", to, k_tf[f]);
                        SDL_CopyFile(tf, tt);
                    }
                    snprintf(tt, sizeof(tt), "%s/%s", to, e->id);
                    if (copy_tree(e->dir, tt)) {
                        char msg[1600];
                        snprintf(msg, sizeof(msg), "%s %s", T(S_EXPORTED_TO), tt);
                        dash_toast(msg);
                    }
                }
            }
        }
    }
    list_end(&l);
    if (D.in.back) {
        load_titles();
        dash_pop();
    }
    draw_hints(T(S_EXPORT), T(S_BACK), T(S_DELETE), T(S_COPY));
}
