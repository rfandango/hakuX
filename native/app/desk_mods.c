/*
 * xbased (hakuX for PC) — the run's mods and cheats (mods/README.md): the
 * manifest the core reads (native/core/mods.h), written from the mods
 * folder beside the executable as the Android app's ModRun writes it from
 * its assets.
 *
 * A disc's folders: mods/<TITLE ID>.<version> (its default.xbe's
 * certificate) and mods/<TITLE ID> (any release), beside the executable
 * (the ones that ship) and in the data folder (the user's own). A mod is a
 * folder with a mod.txt, or an .ips file: the usual patch of default.xbe
 * (file offsets, as Lunar IPS applies it), its name the file's. A mod is on
 * when an option names it, "mod=<id>" ("mod=*": all of the game's), a
 * cheat with "cheat=<name>". Every mod's patches are listed, an off one's
 * as patchoff (a savestate made with it on gets the disc's bytes back).
 */
#include "native/app/desk_mods.h"
#include "native/core/mods.h"
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "native/app/desk.h"
#include "native/app/ui/config.h"

#include <SDL3/SDL.h>

#define MAX_MODS 64

typedef struct {
    char id[128];
    char name[256];
    char dir[MAX_PATH * 2];     /* the folder it's in */
    int refresh;
    bool on, ips;               /* ips: dir/<id>.ips */
} Mod;

static bool opt_has(char **opts, int n, const char *kind, const char *value)
{
    const size_t k = strlen(kind);
    for (int i = 0; i < n; i++) {
        if (!strncmp(opts[i], kind, k) && opts[i][k] == '=' &&
            (!strcasecmp(opts[i] + k + 1, value) || !strcmp(opts[i] + k + 1, "*"))) {
            return true;
        }
    }
    return false;
}

/* The disc's game key: title ID and certificate version (54430006.6). */
static bool game_key(int disc_fd, char *key, size_t n)
{
    const int fd = dup(disc_fd);
    if (fd < 0) {
        return false;
    }
    NcDisc d;
    memset(&d, 0, sizeof(d));
    if (!nc_disc_open(&d, fd)) {
        close(fd);
        return false;
    }
    NcDiscFile f;
    uint8_t h[0x1000];
    bool ok = false;
    if (nc_disc_find(&d, "default.xbe", &f) && f.size >= sizeof(h) && nc_disc_read(&d, &f, 0, h, sizeof(h)) &&
        !memcmp(h, "XBEH", 4)) {
        uint32_t base, cert;
        memcpy(&base, h + 0x104, 4);
        memcpy(&cert, h + 0x118, 4);
        const uint32_t c = cert - base;
        if (c + 0xB0 <= sizeof(h)) {
            uint32_t tid, ver;
            memcpy(&tid, h + c + 8, 4);
            memcpy(&ver, h + c + 0xAC, 4);
            snprintf(key, n, "%08X.%u", tid, ver);
            ok = true;
        }
    }
    nc_disc_close(&d);
    return ok;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    return s;
}

/* A "key: value" header line's key (letters only), lower-cased into k. */
static const char *header(char *line, char *k, size_t kn)
{
    char *colon = strchr(line, ':');
    if (!colon || colon == line || (size_t)(colon - line) >= kn) {
        return NULL;
    }
    for (char *p = line; p < colon; p++) {
        if (!isalpha((unsigned char)*p)) {
            return NULL;
        }
        k[p - line] = (char)tolower((unsigned char)*p);
    }
    k[colon - line] = 0;
    return trim(colon + 1);
}

/* A mod's files/ folder: each file as a disc path over the disc. */
static void list_files(FILE *out, const char *root, const char *rel)
{
    char pattern[MAX_PATH * 2];
    snprintf(pattern, sizeof(pattern), "%s/%s%s*", root, rel, rel[0] ? "/" : "");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) {
            continue;
        }
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_files(out, root, sub);
        } else {
            /* the disc's path with its backslashes; the host's with '/' (Windows takes both) */
            char host[MAX_PATH * 2];
            snprintf(host, sizeof(host), "%s/%s", root, sub);
            for (char *c = host; *c; c++) {
                if (*c == '\\') {
                    *c = '/';
                }
            }
            fprintf(out, "file\t%s\t%s\n", sub, host);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int mod_cmp(const void *a, const void *b)
{
    return strcasecmp(((const Mod *)a)->id, ((const Mod *)b)->id);
}

/* A folder's mods: subfolders with a mod.txt, .ips files (an id met
 * before, in an earlier folder, is that one) */
static void scan_mods(const char *dir, Mod *mods, int *n)
{
    char pattern[MAX_PATH * 2 + 4];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE && *n < MAX_MODS) {
        const size_t len = strlen(fd.cFileName);
        const bool sub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.';
        const bool ips = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && len > 4 &&
                         !strcasecmp(fd.cFileName + len - 4, ".ips");
        if (sub || ips) {
            Mod *m = &mods[*n];
            memset(m, 0, sizeof(*m));
            snprintf(m->id, sizeof(m->id), "%.*s", (int)(ips ? len - 4 : len), fd.cFileName);
            snprintf(m->dir, sizeof(m->dir), "%s", dir);
            m->ips = ips;
            bool dup = false;
            for (int i = 0; i < *n; i++) {
                dup |= !strcasecmp(mods[i].id, m->id);
            }
            if (!dup) {
                (*n)++;
            }
        }
        if (!FindNextFileA(h, &fd)) {
            break;
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
    }
}

/* An IPS patch of default.xbe as the manifest's patch lines: each record's
 * file offset at the address it loads at (the headers at the base, a
 * section's raw bytes at its address). Returns the records, -1 if it isn't
 * one. */
static int ips_patches(FILE *out, int disc_fd, const char *path, const char *id, bool on)
{
    size_t len = 0;
    uint8_t *p = SDL_LoadFile(path, &len);
    if (!p || len < 8 || memcmp(p, "PATCH", 5)) {
        SDL_free(p);
        nc_log("kantaiX-mods", "%s: not an IPS patch", path);
        return -1;
    }
    /* default.xbe's headers: the base, the sections' raw ranges */
    const int fd = dup(disc_fd);
    NcDisc d;
    memset(&d, 0, sizeof(d));
    uint8_t *hdr = NULL;
    uint32_t hsize = 0, base = 0;
    if (fd >= 0 && nc_disc_open(&d, fd)) {
        NcDiscFile f;
        uint8_t h0[0x180];
        if (nc_disc_find(&d, "default.xbe", &f) && nc_disc_read(&d, &f, 0, h0, sizeof(h0)) && !memcmp(h0, "XBEH", 4)) {
            memcpy(&base, h0 + 0x104, 4);
            memcpy(&hsize, h0 + 0x108, 4);
            if (hsize >= sizeof(h0) && hsize < (8u << 20)) {
                hdr = malloc(hsize);
                if (hdr && !nc_disc_read(&d, &f, 0, hdr, hsize)) {
                    free(hdr);
                    hdr = NULL;
                }
            }
        }
        nc_disc_close(&d);
    } else if (fd >= 0) {
        close(fd);
    }
    if (!hdr) {
        SDL_free(p);
        return -1;
    }
    uint32_t nsec = 0, secs = 0;
    memcpy(&nsec, hdr + 0x11C, 4);
    memcpy(&secs, hdr + 0x120, 4);
    secs -= base;
    static uint8_t rle[0x10000];
    int recs = 0;
    size_t at = 5;
    while (at + 5 <= len && memcmp(p + at, "EOF", 3)) {
        const uint32_t off = (uint32_t)p[at] << 16 | (uint32_t)p[at + 1] << 8 | p[at + 2];
        uint32_t size = (uint32_t)p[at + 3] << 8 | p[at + 4];
        at += 5;
        const uint8_t *bytes = p + at;
        if (size == 0) {    /* a run: count, the byte */
            if (at + 3 > len) {
                break;
            }
            size = (uint32_t)p[at] << 8 | p[at + 1];
            memset(rle, p[at + 2], size);
            bytes = rle;
            at += 3;
        } else {
            if (at + size > len) {
                break;
            }
            at += size;
        }
        recs++;
        /* byte by byte to its address, in lines of up to 32 bytes in a row */
        uint32_t line_va = 0, line_n = 0;
        char line[32 * 3 + 8] = "";
        for (uint32_t i = 0; i <= size; i++) {
            uint32_t va = 0;
            bool mapped = false;
            if (i < size) {
                const uint32_t o = off + i;
                if (o < hsize) {
                    va = base + o;
                    mapped = true;
                }
                for (uint32_t k = 0; !mapped && k < nsec && secs + (k + 1) * 0x38 <= hsize; k++) {
                    const uint8_t *sh = hdr + secs + k * 0x38;
                    uint32_t sva, raw, rsize;
                    memcpy(&sva, sh + 4, 4);
                    memcpy(&raw, sh + 0xC, 4);
                    memcpy(&rsize, sh + 0x10, 4);
                    if (o >= raw && o < raw + rsize) {
                        va = sva + (o - raw);
                        mapped = true;
                    }
                }
            }
            if (line_n && (i == size || !mapped || va != line_va + line_n || line_n == 32)) {
                fprintf(out, "%s\tdefault.xbe\t%s\t0x%08X:%s\n", on ? "patch" : "patchoff", id, line_va, line);
                line_n = 0;
            }
            if (i < size && mapped) {
                if (!line_n) {
                    line_va = va;
                    line[0] = 0;
                }
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " %02X", bytes[i]);
                line_n++;
            }
        }
    }
    free(hdr);
    SDL_free(p);
    return recs;
}

bool nc_desk_mods_prepare(const char *exe_dir, int disc_fd, char **opts, int nopts)
{
    char key[32];
    if (disc_fd < 0 || !game_key(disc_fd, key, sizeof(key))) {
        return false;
    }
    /* the shipped mods, then the user's; a release's folder, then the title's */
    char dirs[4][MAX_PATH * 2];
    snprintf(dirs[0], sizeof(dirs[0]), "%smods/%s", exe_dir, key);
    snprintf(dirs[1], sizeof(dirs[1]), "%smods/%.8s", exe_dir, key);
    snprintf(dirs[2], sizeof(dirs[2]), "%smods/%s", cfg_data_dir(), key);
    snprintf(dirs[3], sizeof(dirs[3]), "%smods/%.8s", cfg_data_dir(), key);
    static Mod mods[MAX_MODS];
    int n = 0;
    for (int d = 0; d < 4; d++) {
        if (d == 2 && !strcmp(cfg_data_dir(), exe_dir)) {
            break;  /* portable: the one folder */
        }
        scan_mods(dirs[d], mods, &n);
    }
    if (!n && GetFileAttributesA(dirs[0]) == INVALID_FILE_ATTRIBUTES) {
        nc_log("kantaiX-mods", "%s: no mods or cheats", key);
        return false;
    }
    const char *dir = dirs[0];   /* cheats.txt's */
    qsort(mods, (size_t)n, sizeof(Mod), mod_cmp);

    char manifest[MAX_PATH + 32];
    snprintf(manifest, sizeof(manifest), "%smods_run.txt", cfg_data_dir()); /* writable (an AppImage's folder isn't) */
    FILE *out = fopen(manifest, "w");
    if (!out) {
        return false;
    }
    int on = 0, refresh = 0;
    /* the files of the mods on first (patches read the files they patch) */
    for (int i = 0; i < n; i++) {
        mods[i].on = opt_has(opts, nopts, "mod", mods[i].id);
        snprintf(mods[i].name, sizeof(mods[i].name), "%s", mods[i].id);
        mods[i].refresh = 0;
        on += mods[i].on;
        if (mods[i].on && !mods[i].ips) {
            char files[MAX_PATH * 2 + 16];
            snprintf(files, sizeof(files), "%s/%s/files", mods[i].dir, mods[i].id);
            list_files(out, files, "");
        }
    }
    for (int i = 0; i < n; i++) {
        char path[MAX_PATH * 2 + 16];
        if (mods[i].ips) {
            snprintf(path, sizeof(path), "%s/%s.ips", mods[i].dir, mods[i].id);
            const int recs = ips_patches(out, disc_fd, path, mods[i].id, mods[i].on);
            nc_log("kantaiX-mods", "%s patch %s.ips (%d records): %s", key, mods[i].id, recs,
                   mods[i].on ? "on" : "off (mod=<its name> turns it on)");
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/mod.txt", mods[i].dir, mods[i].id);
        FILE *fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        char raw[1024], xbe[256] = "";
        while (fgets(raw, sizeof(raw), fp)) {
            char *line = trim(raw);
            if (!line[0] || line[0] == '#') {
                continue;
            }
            const size_t len = strlen(line);
            if (line[0] == '[' && line[len - 1] == ']') {
                line[len - 1] = 0;
                snprintf(xbe, sizeof(xbe), "%s", trim(line + 1));
                continue;
            }
            if (xbe[0]) {
                fprintf(out, "%s\t%s\t%s\t%s\n", mods[i].on ? "patch" : "patchoff", xbe, mods[i].id, line);
                continue;
            }
            char k[32];
            const char *v = header(line, k, sizeof(k));
            if (v && !strcmp(k, "name") && v[0]) {
                snprintf(mods[i].name, sizeof(mods[i].name), "%s", v);
            } else if (v && !strcmp(k, "refresh")) {
                mods[i].refresh = atoi(v);
            }
        }
        fclose(fp);
        if (mods[i].on && mods[i].refresh > refresh) {
            refresh = mods[i].refresh;
        }
        nc_log("kantaiX-mods", "%s mod %s (\"%s\"): %s", key, mods[i].id, mods[i].name,
               mods[i].on ? "on" : "off (mod=<its folder> turns it on)");
    }
    if (refresh > 0) {
        fprintf(out, "refresh\t%d\n", refresh);
    }

    /* cheats.txt: [name], note:, once, code lines */
    char cpath[MAX_PATH * 2 + 16];
    snprintf(cpath, sizeof(cpath), "%s/cheats.txt", dir);
    FILE *cf = fopen(cpath, "r");
    int cheats = 0;
    if (cf) {
        char raw[1024], name[256] = "", code[64][256];
        int ncode = 0;
        bool once = false;
        for (bool more = true; more;) {
            char *line = NULL;
            more = fgets(raw, sizeof(raw), cf) != NULL;
            if (more) {
                line = trim(raw);
                if (!line[0] || line[0] == '#') {
                    continue;
                }
            }
            const size_t len = line ? strlen(line) : 0;
            const bool head = line && line[0] == '[' && line[len - 1] == ']';
            if ((head || !more) && name[0]) {
                fprintf(out, "cheat\t%d\t%d\t%s\n", opt_has(opts, nopts, "cheat", name) ? 1 : 0, once ? 1 : 0, name);
                for (int i = 0; i < ncode; i++) {
                    fprintf(out, "code\t%s\n", code[i]);
                }
                cheats++;
            }
            if (!more) {
                break;
            }
            if (head) {
                line[len - 1] = 0;
                snprintf(name, sizeof(name), "%s", trim(line + 1));
                ncode = 0;
                once = false;
                continue;
            }
            if (!name[0]) {
                continue;
            }
            char k[32];
            if (header(line, k, sizeof(k)) && !strcmp(k, "note")) {
                continue;
            }
            if (!strcasecmp(line, "once")) {
                once = true;
            } else if (ncode < 64) {
                snprintf(code[ncode++], sizeof(code[0]), "%s", line);
            }
        }
        fclose(cf);
    }
    fclose(out);
    nc_mods_set_manifest(manifest);
    nc_log("kantaiX-mods", "%s: %d of %d mods on, %d cheats (manifest %s)", key, on, n, cheats, manifest);
    return true;
}
