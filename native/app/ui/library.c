/*
 * hakuX front end — the game library (library.h).
 *
 * The cache (<data>/covers/library.txt): a line a disc image, its path,
 * size, modification time and certificate fields, so a rescan reads only
 * new or changed images; their title icons (<key>.icon.png) beside. Box
 * art (<key>.png) comes from libretro-thumbnails' list of names, matched
 * to the game's title by its words, preferring the disc's region.
 */
#include "native/app/ui/library.h"
#include "native/app/ui/config.h"
#include "native/app/ui/ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#define MAX_GAMES 4096
#define THUMBS "https://raw.githubusercontent.com/libretro-thumbnails/Microsoft_-_Xbox/master/Named_Boxarts/"
#define TREE "https://api.github.com/repos/libretro-thumbnails/Microsoft_-_Xbox/git/trees/master?recursive=1"

static struct {
    SDL_Mutex *mu;
    LibGame *games[MAX_GAMES];
    int n;
    SDL_Thread *scan, *fetch;
    SDL_AtomicInt scanning, fetching, stop;
    char status[160];
    /* fetch queue: indices */
    int queue[MAX_GAMES], nq;
} L;

/* strsep's: the text up to the next delimiter, *s past it (NULL at the end) */
static char *tok(char **s, const char *delim)
{
    char *t = *s;
    if (!t) {
        return NULL;
    }
    char *e = t + strcspn(t, delim);
    *s = *e ? e + 1 : NULL;
    *e = 0;
    return t;
}

static void covers_dir(char *out, size_t n)
{
    snprintf(out, n, "%scovers", cfg_data_dir());
    SDL_CreateDirectory(out);
}

static bool is_image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!SDL_strcasecmp(dot, ".iso") || !SDL_strcasecmp(dot, ".xiso") || !SDL_strcasecmp(dot, ".cci"));
}

/* ------------------------------------------------------------ the cache */

typedef struct {
    char path[1024];
    int64_t size, mtime;
    XdGame g;
} CacheRow;

static CacheRow *g_cache;
static int g_ncache;

static void cache_load(void)
{
    char p[1100];
    covers_dir(p, sizeof(p));
    strcat(p, "/library.txt");
    FILE *f = fopen(p, "rb");
    if (!f) {
        return;
    }
    g_cache = calloc(MAX_GAMES, sizeof(CacheRow));
    char line[1600];
    while (g_ncache < MAX_GAMES && fgets(line, sizeof(line), f)) {
        CacheRow *r = &g_cache[g_ncache];
        char *fld[7];
        int k = 0;
        for (char *s = line, *t; k < 7 && (t = tok(&s, "\t")); k++) {
            fld[k] = t;
        }
        if (k < 7) {
            continue;
        }
        fld[6][strcspn(fld[6], "\r\n")] = 0;
        snprintf(r->path, sizeof(r->path), "%s", fld[0]);
        r->size = strtoll(fld[1], NULL, 10);
        r->mtime = strtoll(fld[2], NULL, 10);
        r->g.title_id = (uint32_t)strtoul(fld[3], NULL, 16);
        r->g.version = (uint32_t)strtoul(fld[4], NULL, 10);
        r->g.regions = (uint32_t)strtoul(fld[5], NULL, 10);
        snprintf(r->g.title, sizeof(r->g.title), "%s", fld[6]);
        snprintf(r->g.path, sizeof(r->g.path), "%s", r->path);
        snprintf(r->g.key, sizeof(r->g.key), "%08X.%u", r->g.title_id, r->g.version);
        r->g.size = r->size;
        g_ncache++;
    }
    fclose(f);
}

static void cache_save(void)
{
    char p[1100];
    covers_dir(p, sizeof(p));
    strcat(p, "/library.txt");
    FILE *f = fopen(p, "wb");
    if (!f) {
        return;
    }
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        const LibGame *g = L.games[i];
        fprintf(f, "%s\t%lld\t%lld\t%08X\t%u\t%u\t%s\n", g->g.path, (long long)g->g.size, (long long)g->mtime,
                g->g.title_id, g->g.version, g->g.regions, g->g.title);
    }
    SDL_UnlockMutex(L.mu);
    fclose(f);
}

static void icon_path(const XdGame *g, char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/%s.icon.png", d, g->key);
}

static void cover_path(const XdGame *g, char *out, size_t n)
{
    char d[1100];
    covers_dir(d, sizeof(d));
    snprintf(out, n, "%s/%s.png", d, g->key);
}

/* ------------------------------------------------------------ scanning */

static int cmp_title(const void *a, const void *b)
{
    return SDL_strcasecmp((*(LibGame *const *)a)->g.title, (*(LibGame *const *)b)->g.title);
}

static void add_game(const char *path, const SDL_PathInfo *pi)
{
    for (int i = 0; i < L.n; i++) {
        if (!strcmp(L.games[i]->g.path, path)) {
            return;
        }
    }
    LibGame *lg = calloc(1, sizeof(*lg));
    lg->mtime = pi->modify_time;
    bool cached = false;
    for (int i = 0; i < g_ncache; i++) {
        if (!strcmp(g_cache[i].path, path) && g_cache[i].size == (int64_t)pi->size && g_cache[i].mtime == pi->modify_time) {
            lg->g = g_cache[i].g;
            cached = true;
            break;
        }
    }
    if (!cached) {
        xd_read_game(path, &lg->g);
        if (lg->g.icon) {
            char ip[1200];
            icon_path(&lg->g, ip, sizeof(ip));
            stbi_write_png(ip, lg->g.icon_w, lg->g.icon_h, 4, lg->g.icon, lg->g.icon_w * 4);
            xd_free_game(&lg->g);
        }
        lg->g.size = (int64_t)pi->size;
    }
    SDL_LockMutex(L.mu);
    /* one entry per game and version: an XISO before a full dump of it */
    for (int i = 0; i < L.n; i++) {
        LibGame *o = L.games[i];
        if (o->g.title_id && o->g.title_id == lg->g.title_id && o->g.version == lg->g.version) {
            if (o->g.size > lg->g.size) {
                L.games[i] = lg;
                lg = o;
            }
            SDL_UnlockMutex(L.mu);
            free(lg);
            return;
        }
    }
    if (L.n < MAX_GAMES) {
        L.games[L.n++] = lg;
        qsort(L.games, L.n, sizeof(L.games[0]), cmp_title);
    } else {
        free(lg);
    }
    SDL_UnlockMutex(L.mu);
}

static void scan_dir(const char *dir, int depth)
{
    if (depth > 6 || SDL_GetAtomicInt(&L.stop)) {
        return;
    }
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\' ? "" : "/",
                 names[i]);
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(p, &pi)) {
            continue;
        }
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            scan_dir(p, depth + 1);
        } else if (pi.type == SDL_PATHTYPE_FILE && is_image_name(names[i]) && pi.size > 1 << 20) {
            /* foo.iso beside foo.xiso.iso: the XISO */
            add_game(p, &pi);
        }
    }
    SDL_free(names);
}

typedef struct {
    char dirs[16][1024];
    int n;
} ScanArgs;

static int scan_thread(void *arg)
{
    ScanArgs *a = arg;
    if (!g_cache) {
        cache_load();
    }
    for (int i = 0; i < a->n; i++) {
        scan_dir(a->dirs[i], 0);
    }
    /* games whose files went away */
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        if (!SDL_GetPathInfo(L.games[i]->g.path, NULL)) {
            L.games[i]->g.path[0] = 0;  /* hidden; freed at shutdown */
        }
    }
    SDL_UnlockMutex(L.mu);
    cache_save();
    free(a);
    SDL_SetAtomicInt(&L.scanning, 0);
    return 0;
}

void lib_scan(const char *const *folders, int n)
{
    if (!L.mu) {
        L.mu = SDL_CreateMutex();
    }
    if (SDL_GetAtomicInt(&L.scanning)) {
        return;
    }
    if (L.scan) {
        SDL_WaitThread(L.scan, NULL);
    }
    ScanArgs *a = calloc(1, sizeof(*a));
    for (int i = 0; i < n && i < 16; i++) {
        snprintf(a->dirs[a->n++], sizeof(a->dirs[0]), "%s", folders[i]);
    }
    SDL_SetAtomicInt(&L.scanning, 1);
    L.scan = SDL_CreateThread(scan_thread, "lib-scan", a);
}

bool lib_scanning(void)
{
    return SDL_GetAtomicInt(&L.scanning) != 0;
}

int lib_count(void)
{
    if (!L.mu) {
        return 0;
    }
    SDL_LockMutex(L.mu);
    int k = 0;
    for (int i = 0; i < L.n; i++) {
        k += L.games[i]->g.path[0] != 0;
    }
    SDL_UnlockMutex(L.mu);
    return k;
}

LibGame *lib_get(int idx)
{
    LibGame *r = NULL;
    SDL_LockMutex(L.mu);
    for (int i = 0, k = 0; i < L.n; i++) {
        if (L.games[i]->g.path[0] && k++ == idx) {
            r = L.games[i];
            break;
        }
    }
    SDL_UnlockMutex(L.mu);
    return r;
}

SDL_Texture *lib_load_png(const char *path)
{
    int w, h, c;
    uint8_t *px = stbi_load(path, &w, &h, &c, 4);
    if (!px) {
        return NULL;
    }
    SDL_Texture *t = ui_texture_rgba(px, w, h);
    stbi_image_free(px);
    return t;
}

SDL_Texture *lib_icon(LibGame *g)
{
    if (!g->icon_tex) {
        char p[1200];
        icon_path(&g->g, p, sizeof(p));
        g->icon_tex = lib_load_png(p);
        if (!g->icon_tex) {
            g->icon_tex = (SDL_Texture *)1;    /* none: not looked for again */
        }
    }
    return g->icon_tex == (SDL_Texture *)1 ? NULL : g->icon_tex;
}

SDL_Texture *lib_cover(LibGame *g)
{
    if (!g->cover_tex && !g->cover_tried) {
        char p[1200];
        cover_path(&g->g, p, sizeof(p));
        g->cover_tex = lib_load_png(p);
        g->cover_tried = true;
    }
    return g->cover_tex;
}

/* ------------------------------------------------------------ box art */

static bool run(const char *const *args)
{
    return cfg_run(args);
}

static bool download(const char *url, const char *to)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.part", to);
    const char *args[] = {"curl", "-sfL", "--max-time", "60", "-A", "xbased", "-o", tmp, url, NULL};
    if (!run(args)) {
        remove(tmp);
        return false;
    }
    remove(to);
    return rename(tmp, to) == 0;
}

/* lowercase words, "&" as "and", groups dropped */
static void norm(const char *s, char *out, size_t n)
{
    size_t o = 0;
    int depth = 0;
    for (; *s && o + 5 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '(' || c == '[') {
            depth++;
        } else if ((c == ')' || c == ']') && depth) {
            depth--;
        } else if (depth) {
            continue;
        } else if (c == '&') {
            memcpy(out + o, " and ", 5);
            o += 5;
        } else if (isalnum(c) || c >= 0x80) {
            out[o++] = (char)tolower(c);
        } else if (o && out[o - 1] != ' ') {
            out[o++] = ' ';
        }
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = 0;
}

/* words shared, over the words of both (articles and "the" ignored) */
static float score(const char *a, const char *b)
{
    char wa[32][32], wb[32][32];
    int na = 0, nb = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", a);
    for (char *s = tmp, *t; na < 32 && (t = tok(&s, " "));) {
        if (*t && strcmp(t, "the") && strcmp(t, "and") && strcmp(t, "of")) {
            snprintf(wa[na++], 32, "%s", t);
        }
    }
    snprintf(tmp, sizeof(tmp), "%s", b);
    for (char *s = tmp, *t; nb < 32 && (t = tok(&s, " "));) {
        if (*t && strcmp(t, "the") && strcmp(t, "and") && strcmp(t, "of")) {
            snprintf(wb[nb++], 32, "%s", t);
        }
    }
    int common = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (!strcmp(wa[i], wb[j])) {
                common++;
                break;
            }
        }
    }
    return na + nb ? 2.0f * common / (na + nb) : 0;
}

static char **g_names;
static int g_nnames;

static bool names_load(void)
{
    if (g_names) {
        return true;
    }
    char d[1100], p[1200];
    covers_dir(d, sizeof(d));
    snprintf(p, sizeof(p), "%s/boxart_names.txt", d);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(p, &pi) || pi.size < 1000) {
        char js[1200];
        snprintf(js, sizeof(js), "%s/tree.json", d);
        snprintf(L.status, sizeof(L.status), "libretro-thumbnails...");
        if (!download(TREE, js)) {
            return false;
        }
        /* the "path" values under Named_Boxarts/ */
        size_t n = 0;
        char *t = SDL_LoadFile(js, &n);
        FILE *f = t ? fopen(p, "wb") : NULL;
        for (char *s = t; f && (s = strstr(s, "\"path\":\"Named_Boxarts/"));) {
            s += 22;    /* past "path":"Named_Boxarts/ */
            char *e = strchr(s, '"');
            if (!e) {
                break;
            }
            if (e - s > 4 && !strncmp(e - 4, ".png", 4)) {
                /* JSON escapes: & for '&' and the like */
                for (char *c = s; c < e; c++) {
                    if (c[0] == '\\' && c[1] == 'u' && e - c >= 6) {
                        fputc((int)strtol((char[5]){c[2], c[3], c[4], c[5], 0}, NULL, 16), f);
                        c += 5;
                    } else if (c[0] == '\\' && c + 1 < e) {
                        fputc(*++c, f);
                    } else {
                        fputc(*c, f);
                    }
                }
                fputc('\n', f);
            }
            s = e;
        }
        if (f) {
            fclose(f);
        }
        SDL_free(t);
        remove(js);
    }
    size_t n = 0;
    char *t = SDL_LoadFile(p, &n);
    if (!t) {
        return false;
    }
    g_names = calloc(4096, sizeof(char *));
    for (char *s = t, *l; g_nnames < 4096 && (l = tok(&s, "\n"));) {
        if (*l) {
            g_names[g_nnames++] = l;
        }
    }
    return g_nnames > 0;
}

static const char *best_name(const XdGame *g)
{
    char a[256], b[256], f[256];
    norm(g->title, a, sizeof(a));
    char fn[256];
    xd_clean_name(g->path, fn, sizeof(fn));
    norm(fn, f, sizeof(f));
    const char *best = NULL;
    float bs = 0;
    for (int i = 0; i < g_nnames; i++) {
        norm(g_names[i], b, sizeof(b));
        float s = score(a, b);
        const float s2 = score(f, b);
        if (s2 > s) {
            s = s2;
        }
        if (s < 0.6f) {
            continue;
        }
        /* the disc's region first (1 North America, 2 Japan, 4 the rest), no demos */
        const char *nm = g_names[i];
        if (strstr(nm, "(Demo)") || strstr(nm, "(Beta)") || strstr(nm, "(Proto)")) {
            s -= 0.3f;
        }
        if (((g->regions & 4) && strstr(nm, "(Europe")) || ((g->regions & 1) && strstr(nm, "(USA")) ||
            ((g->regions & 2) && strstr(nm, "(Japan"))) {
            s += 0.05f;
        }
        if (s > bs) {
            bs = s;
            best = nm;
        }
    }
    return best;
}

static void url_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = 0;
}

static int fetch_thread(void *arg)
{
    for (;;) {
        SDL_LockMutex(L.mu);
        if (!L.nq || SDL_GetAtomicInt(&L.stop)) {
            SDL_UnlockMutex(L.mu);
            break;
        }
        LibGame *g = L.games[L.queue[--L.nq]];
        const int left = L.nq;
        SDL_UnlockMutex(L.mu);
        snprintf(L.status, sizeof(L.status), "%s (%d)", g->g.title, left + 1);
        if (!names_load()) {
            break;
        }
        const char *nm = best_name(&g->g);
        if (!nm) {
            continue;
        }
        char esc[600], url[800], to[1200];
        url_escape(nm, esc, sizeof(esc));
        snprintf(url, sizeof(url), THUMBS "%s", esc);
        cover_path(&g->g, to, sizeof(to));
        if (download(url, to)) {
            g->cover_tried = false;     /* the UI's thread loads it */
        }
    }
    L.status[0] = 0;
    SDL_SetAtomicInt(&L.fetching, 0);
    return 0;
}

static void fetch_start(void)
{
    if (SDL_GetAtomicInt(&L.fetching)) {
        return;
    }
    if (L.fetch) {
        SDL_WaitThread(L.fetch, NULL);
    }
    SDL_SetAtomicInt(&L.fetching, 1);
    L.fetch = SDL_CreateThread(fetch_thread, "lib-covers", NULL);
}

void lib_fetch_cover(LibGame *g)
{
    SDL_LockMutex(L.mu);
    for (int i = 0; i < L.n; i++) {
        if (L.games[i] == g && L.nq < MAX_GAMES) {
            L.queue[L.nq++] = i;
        }
    }
    SDL_UnlockMutex(L.mu);
    fetch_start();
}

void lib_fetch_all_covers(void)
{
    SDL_LockMutex(L.mu);
    for (int i = L.n - 1; i >= 0; i--) {
        char p[1200];
        cover_path(&L.games[i]->g, p, sizeof(p));
        if (L.games[i]->g.path[0] && !SDL_GetPathInfo(p, NULL) && L.nq < MAX_GAMES) {
            L.queue[L.nq++] = i;
        }
    }
    SDL_UnlockMutex(L.mu);
    fetch_start();
}

const char *lib_fetch_status(void)
{
    return SDL_GetAtomicInt(&L.fetching) && L.status[0] ? L.status : NULL;
}

void lib_poll(void)
{
    /* covers fetched: their textures reload (cover_tried cleared) */
    for (int i = 0; i < L.n; i++) {
        LibGame *g = L.games[i];
        if (!g->cover_tried && g->cover_tex) {
            SDL_DestroyTexture(g->cover_tex);
            g->cover_tex = NULL;
        }
    }
}

void lib_shutdown(void)
{
    SDL_SetAtomicInt(&L.stop, 1);
    if (L.scan) {
        SDL_WaitThread(L.scan, NULL);
    }
    if (L.fetch) {
        SDL_WaitThread(L.fetch, NULL);
    }
    for (int i = 0; i < L.n; i++) {
        LibGame *g = L.games[i];
        if (g->icon_tex && g->icon_tex != (SDL_Texture *)1) {
            SDL_DestroyTexture(g->icon_tex);
        }
        if (g->cover_tex) {
            SDL_DestroyTexture(g->cover_tex);
        }
        free(g);
    }
    L.n = 0;
}
