/*
 * kantaiX auto-update (update.h).
 *
 * Checks the latest release on kiequoo/kantaiX via the GitHub API (curl).
 * On AppImages the running file is replaced in place (.new alongside it,
 * then renamed over); the new version is then relaunched and this one quits.
 * Settings, saves and states are outside the AppImage and are untouched.
 */
#include "native/app/ui/update.h"
#include "native/app/ui/config.h"
#include "native/core/nc.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REPO         "kiequoo/kantaiX"
#define API          "https://api.github.com/repos/" REPO "/releases/latest"
#define ASSET_SUFFIX "-aarch64.AppImage"

static struct {
    SDL_AtomicInt state;
    SDL_Thread   *thread;
    char current[32];
    char version[32];   /* latest release, v-stripped */
    char page[256];
    char asset[512];    /* download URL for the arm64 AppImage */
} U;

static bool do_curl(const char *url, const char *to, const char *max_time)
{
    const char *args[] = {"curl", "-sfL", "--max-time", max_time,
                          "-A", "kantaiX",
                          "-H", "Accept: application/vnd.github+json",
                          "-o", to, url, NULL};
    return cfg_run(args);
}

/* Returns true if version string a is newer than b (compared component-wise). */
static bool newer(const char *a, const char *b)
{
    while (*a || *b) {
        const long x = strtol(a, (char **)&a, 10);
        const long y = strtol(b, (char **)&b, 10);
        if (x != y) return x > y;
        a += *a == '.';
        b += *b == '.';
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9')))
            break;
    }
    return false;
}

/* Extracts the string value of key from a JSON fragment into out[n]. */
static const char *json_str(const char *s, const char *key, char *out, size_t n)
{
    char k[64];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(s, k);
    if (!p) return NULL;
    p = strchr(p + strlen(k), '"');
    if (!p) return NULL;
    p++;
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < n; p++)
        out[o++] = *p;
    out[o] = 0;
    return p;
}

static void tmp_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s%s", cfg_data_dir(), name);
}

static int check_thread(void *arg)
{
    (void)arg;
    char js[1200];
    tmp_path(js, sizeof(js), "update.json");
    size_t len = 0;
    char *body = do_curl(API, js, "15") ? SDL_LoadFile(js, &len) : NULL;
    SDL_RemovePath(js);
    char tag[64] = "";
    if (body && json_str(body, "tag_name", tag, sizeof(tag))) {
        const char *v = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
        snprintf(U.version, sizeof(U.version), "%s", v);
        snprintf(U.page, sizeof(U.page), "https://github.com/" REPO "/releases/tag/%s", tag);
        for (const char *p = body;
             (p = json_str(p, "browser_download_url", U.asset, sizeof(U.asset)));) {
            const size_t al = strlen(U.asset), sl = strlen(ASSET_SUFFIX);
            if (al > sl && !strcmp(U.asset + al - sl, ASSET_SUFFIX))
                break;
            U.asset[0] = 0;
        }
        nc_log("hakuX-app", "update: latest %s (this %s)%s",
               U.version, U.current,
               U.asset[0] ? "" : ", no arm64 AppImage in this release");
    } else {
        nc_log("hakuX-app", "update: no answer from GitHub");
    }
    SDL_free(body);
    SDL_SetAtomicInt(&U.state,
        (U.version[0] && newer(U.version, U.current)) ? UPD_AVAILABLE : UPD_IDLE);
    return 0;
}

void upd_check_start(const char *current)
{
    if (U.thread) return;
    snprintf(U.current, sizeof(U.current), "%s", current);
    SDL_SetAtomicInt(&U.state, UPD_CHECKING);
    U.thread = SDL_CreateThread(check_thread, "update-check", NULL);
    if (!U.thread)
        SDL_SetAtomicInt(&U.state, UPD_IDLE);
}

UpdState upd_state(void)      { return (UpdState)SDL_GetAtomicInt(&U.state); }
const char *upd_version(void) { return U.version; }
const char *upd_page(void)
{
    return U.page[0] ? U.page : "https://github.com/" REPO "/releases/latest";
}

static const char *appimage_path(void)
{
    const char *a = SDL_getenv("APPIMAGE");
    return (a && *a) ? a : NULL;
}

bool upd_can_install(void)
{
    return U.asset[0] && appimage_path() != NULL;
}

void upd_cleanup(void)
{
    const char *a = appimage_path();
    if (a) {
        char p[1200];
        snprintf(p, sizeof(p), "%s.new", a);
        SDL_RemovePath(p);
    }
}

static bool install(void)
{
    const char *a = appimage_path();
    char part[1200];
    snprintf(part, sizeof(part), "%s.new", a);
    if (!do_curl(U.asset, part, "600")) {
        nc_log("hakuX-app", "update: download failed");
        SDL_RemovePath(part);
        return false;
    }
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(part, &pi) || pi.size < 1024 * 1024) {
        nc_log("hakuX-app", "update: download too small for an AppImage");
        SDL_RemovePath(part);
        return false;
    }
    chmod(part, 0755);
    if (!SDL_RenamePath(part, a)) {
        nc_log("hakuX-app", "update: %s couldn't be replaced: %s", a, SDL_GetError());
        SDL_RemovePath(part);
        return false;
    }
    return true;
}

static int install_thread(void *arg)
{
    (void)arg;
    const bool ok = install();
    nc_log("hakuX-app", "update: %s %s",
           ok ? "installed" : "failed —", ok ? U.version : "open the release page");
    SDL_SetAtomicInt(&U.state, ok ? UPD_INSTALLED : UPD_FAILED);
    return 0;
}

void upd_install_start(void)
{
    if (U.thread) {
        SDL_WaitThread(U.thread, NULL);
        U.thread = NULL;
    }
    SDL_SetAtomicInt(&U.state, UPD_INSTALLING);
    U.thread = SDL_CreateThread(install_thread, "update-install", NULL);
    if (!U.thread)
        SDL_SetAtomicInt(&U.state, UPD_FAILED);
}

bool upd_relaunch(void)
{
    const char *a = appimage_path();
    if (!a) return false;
    const char *args[] = {a, NULL};
    SDL_Process *p = SDL_CreateProcess(args, false);
    if (!p) return false;
    SDL_DestroyProcess(p);  /* runs on; we exit */
    return true;
}
