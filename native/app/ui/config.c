/*
 * hakuX front end — settings files (config.h).
 */
#include "native/app/ui/config.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') {
        i++;
    }
    memmove(s, s + i, strlen(s + i) + 1);
}

void cfg_load(Config *c, const char *path)
{
    /* path may be c's own (a reload: cfg_load(c, c->path)): kept before
     * c is cleared. Cleared first, it read as "", the dashboard showed the
     * defaults after each game, and saved its settings nowhere. */
    char p[sizeof(c->path)];
    snprintf(p, sizeof(p), "%s", path);
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", p);
    path = c->path;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    char line[600];
    bool head = true;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') {
            if (head && line[0] == '#' && strlen(c->comments) + strlen(line) + 2 < sizeof(c->comments)) {
                strcat(c->comments, line);
                strcat(c->comments, "\n");
            }
            continue;
        }
        head = false;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            trim(line);
            char *v = eq + 1;
            trim(v);
            cfg_add(c, line, v);
        } else {
            cfg_add(c, line, "");
            c->flag[c->n - 1] = true;
        }
    }
    fclose(f);
}

bool cfg_save(const Config *c)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", c->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    /* the header (hakuX's earlier one becomes xbased's) */
    const char *head = "# xbased options, one a line (the command line's come after and win)\n";
    fputs(c->comments[0] && strncmp(c->comments, "# hakuX options", 15) ? c->comments : head, f);
    for (int i = 0; i < c->n; i++) {
        if (c->flag[i]) {
            fprintf(f, "%s\n", c->key[i]);
        } else {
            fprintf(f, "%s=%s\n", c->key[i], c->val[i]);
        }
    }
    fclose(f);
    remove(c->path);
    return rename(tmp, c->path) == 0;
}

static int find(const Config *c, const char *key)
{
    for (int i = c->n - 1; i >= 0; i--) {
        if (!strcmp(c->key[i], key)) {
            return i;
        }
    }
    return -1;
}

const char *cfg_get(const Config *c, const char *key)
{
    const int i = find(c, key);
    return i < 0 ? NULL : c->flag[i] ? "1" : c->val[i];
}

int cfg_int(const Config *c, const char *key, int def)
{
    const char *v = cfg_get(c, key);
    return v && *v ? atoi(v) : def;
}

bool cfg_bool(const Config *c, const char *key, bool def)
{
    const char *v = cfg_get(c, key);
    return v ? strcmp(v, "0") && strcmp(v, "off") && strcmp(v, "false") : def;
}

void cfg_set(Config *c, const char *key, const char *val)
{
    const int i = find(c, key);
    if (i >= 0) {
        snprintf(c->val[i], sizeof(c->val[i]), "%s", val);
        c->flag[i] = false;
        return;
    }
    cfg_add(c, key, val);
}

void cfg_set_int(Config *c, const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    cfg_set(c, key, b);
}

void cfg_set_bool(Config *c, const char *key, bool v)
{
    cfg_set(c, key, v ? "1" : "0");
}

void cfg_unset(Config *c, const char *key)
{
    for (int i = c->n - 1; i >= 0; i--) {
        if (!strcmp(c->key[i], key)) {
            memmove(&c->key[i], &c->key[i + 1], sizeof(c->key[0]) * (size_t)(c->n - i - 1));
            memmove(&c->val[i], &c->val[i + 1], sizeof(c->val[0]) * (size_t)(c->n - i - 1));
            memmove(&c->flag[i], &c->flag[i + 1], sizeof(c->flag[0]) * (size_t)(c->n - i - 1));
            c->n--;
        }
    }
}

int cfg_all(const Config *c, const char *key, const char **out, int max)
{
    int k = 0;
    for (int i = 0; i < c->n && k < max; i++) {
        if (!strcmp(c->key[i], key)) {
            out[k++] = c->val[i];
        }
    }
    return k;
}

void cfg_add(Config *c, const char *key, const char *val)
{
    if (c->n >= CFG_MAX) {
        return;
    }
    snprintf(c->key[c->n], sizeof(c->key[0]), "%s", key);
    snprintf(c->val[c->n], sizeof(c->val[0]), "%s", val);
    c->flag[c->n] = false;
    c->n++;
}

void cfg_remove_value(Config *c, const char *key, const char *val)
{
    for (int i = c->n - 1; i >= 0; i--) {
        if (!strcmp(c->key[i], key) && !strcmp(c->val[i], val)) {
            memmove(&c->key[i], &c->key[i + 1], sizeof(c->key[0]) * (size_t)(c->n - i - 1));
            memmove(&c->val[i], &c->val[i + 1], sizeof(c->val[0]) * (size_t)(c->n - i - 1));
            memmove(&c->flag[i], &c->flag[i + 1], sizeof(c->flag[0]) * (size_t)(c->n - i - 1));
            c->n--;
        }
    }
}

const char *cfg_exe_dir(void)
{
    const char *b = SDL_GetBasePath();
    return b ? b : "./";
}

const char *cfg_data_dir(void)
{
    static char dir[1024];
    if (dir[0]) {
        return dir;
    }
    const char *exe = cfg_exe_dir();
    char probe[1100];
    snprintf(probe, sizeof(probe), "%sportable.txt", exe);
#ifdef _WIN32
    /* Portable: the exe's folder, if it can be written. Program Files, a
     * read-only copy or Windows' controlled folder access (Desktop,
     * Documents) let nothing be created there: the hdd, a game's cache and
     * saves failed, and games showed their disc error. Then the user's own
     * application data folder. */
    snprintf(probe, sizeof(probe), "%s.xbased-write-test", exe);
    SDL_IOStream *t = SDL_IOFromFile(probe, "wb");
    const bool writable = t && SDL_WriteIO(t, "x", 1) == 1;
    if (t) {
        SDL_CloseIO(t);
        SDL_RemovePath(probe);
    }
    if (writable) {
        snprintf(dir, sizeof(dir), "%s", exe);
    } else {
        char *p = SDL_GetPrefPath("", "xbased");
        snprintf(dir, sizeof(dir), "%s", p ? p : exe);
        SDL_free(p);
    }
#else
    if (SDL_GetPathInfo(probe, NULL)) {
        snprintf(dir, sizeof(dir), "%s", exe);
    } else {
        char *p = SDL_GetPrefPath("", "xbased");
        snprintf(dir, sizeof(dir), "%s", p ? p : exe);
        SDL_free(p);
    }
#endif
    return dir;
}

bool cfg_run(const char *const *args)
{
#ifdef _WIN32
    /* SDL's processes open a console window for a console program (curl
     * and tar flashed one at each start), and its windowless "background"
     * ones give no exit code: CreateProcess with CREATE_NO_WINDOW. The
     * command line quoted as the C runtime reads it back. */
    char cmd[8192];
    size_t n = 0;
    for (int i = 0; args[i] && n < sizeof(cmd) - 4; i++) {
        if (i) {
            cmd[n++] = ' ';
        }
        cmd[n++] = '"';
        for (const char *a = args[i]; *a && n < sizeof(cmd) - 4; a++) {
            size_t bs = 0;
            while (a[bs] == '\\') {
                bs++;
            }
            if (a[bs] == '"' || a[bs] == 0) {
                /* backslashes before a quote, or the closing one, doubled */
                for (size_t k = 0; k < bs * 2 && n < sizeof(cmd) - 4; k++) {
                    cmd[n++] = '\\';
                }
                a += bs;
                if (!*a) {
                    break;
                }
                cmd[n++] = '\\';
                cmd[n++] = '"';
            } else {
                for (size_t k = 0; k < bs && n < sizeof(cmd) - 4; k++) {
                    cmd[n++] = '\\';
                }
                a += bs;
                cmd[n++] = *a;
            }
        }
        cmd[n++] = '"';
    }
    cmd[n] = 0;
    wchar_t wcmd[8192];
    if (!MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 8192)) {
        return false;
    }
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
#else
    SDL_Process *p = SDL_CreateProcess(args, false);
    if (!p) {
        return false;
    }
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    return code == 0;
#endif
}
