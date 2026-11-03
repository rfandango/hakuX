/*
 * xbased (hakuX for PC) — scripted input and pictures (win_auto.h).
 */
#include "native/app/desk_auto.h"
#include "native/core/nc.h"
#include "native/cpu/cpu.h"
#include "hw/xbox/hle/vk/swapchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "native/app/desk.h"

#define AUTO_MAX 256

typedef struct {
    double at;
    char act[48];
} AutoEvent;

static struct {
    bool on;
    double t0;              /* the run's start, s (the system clock: relaunches share it) */
    AutoEvent ev[AUTO_MAX];
    unsigned n, next;
    char dir[MAX_PATH];
    char cmd[MAX_PATH];
    long long cmd_off;
    double cmd_seen;
    double until[32];       /* k_ctl's, held until then */
    void (*save)(int), (*load)(int);
    bool menu;              /* the in-game menu asked for */
} g_auto;

static double now_s(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (double)(((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7;
}

/* The controls by name: a digital bit, an analog button's index, or a
 * stick's direction. */
static const struct {
    const char *name;
    uint16_t bit;
    int analog;             /* -1: none */
    int stick;              /* 1 + (0 lx, 1 ly, 2 rx, 3 ry), the sign in dir */
    int dir;
} k_ctl[] = {
    {"A", 0, 0, 0, 0},      {"B", 0, 1, 0, 0},       {"X", 0, 2, 0, 0},       {"Y", 0, 3, 0, 0},
    {"BLACK", 0, 4, 0, 0},  {"WHITE", 0, 5, 0, 0},   {"LT", 0, 6, 0, 0},      {"RT", 0, 7, 0, 0},
    {"UP", NC_PAD_DPAD_UP, -1, 0, 0},       {"DOWN", NC_PAD_DPAD_DOWN, -1, 0, 0},
    {"LEFT", NC_PAD_DPAD_LEFT, -1, 0, 0},   {"RIGHT", NC_PAD_DPAD_RIGHT, -1, 0, 0},
    {"START", NC_PAD_START, -1, 0, 0},      {"BACK", NC_PAD_BACK, -1, 0, 0},
    {"LS", NC_PAD_LTHUMB, -1, 0, 0},        {"RS", NC_PAD_RTHUMB, -1, 0, 0},
    {"LUP", 0, -1, 2, 1},   {"LDOWN", 0, -1, 2, -1}, {"LLEFT", 0, -1, 1, -1}, {"LRIGHT", 0, -1, 1, 1},
    {"RUP", 0, -1, 4, 1},   {"RDOWN", 0, -1, 4, -1}, {"RLEFT", 0, -1, 3, -1}, {"RRIGHT", 0, -1, 3, 1},
};
#define NCTL (sizeof(k_ctl) / sizeof(k_ctl[0]))

static void shot(const char *name)
{
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s/shot_%s.bmp", g_auto.dir, name);
    hle_present_shot(path);
    nc_log("hakuX-auto", "%.1f s: picture %s", now_s() - g_auto.t0, path);
}

static void act(const char *a, const char *label)
{
    if (!_strnicmp(a, "shot", 4)) {
        const char *name = a[4] == ' ' ? a + 5 : label;
        shot(name);
        return;
    }
    if (!_strnicmp(a, "prof ", 5)) {
        nc_log("hakuX-auto", "%.1f s: profile for %s s", now_s() - g_auto.t0, a + 5);
#if defined(__x86_64__)
        nc_cpu_prof_start((unsigned)atoi(a + 5));
#endif
        return;
    }
    if (!_strnicmp(a, "save ", 5) || !_strnicmp(a, "load ", 5)) {
        nc_log("hakuX-auto", "%.1f s: %s", now_s() - g_auto.t0, a);
        (a[0] == 's' || a[0] == 'S' ? g_auto.save : g_auto.load)(atoi(a + 5));
        return;
    }
    if (!_stricmp(a, "menu")) {
        g_auto.menu = true;
        return;
    }
    if (!_stricmp(a, "quit")) {
        nc_log("hakuX-auto", "%.1f s: quit", now_s() - g_auto.t0);
        _exit(0);
    }
    char name[48];
    snprintf(name, sizeof(name), "%s", a);
    double hold = 0.2;
    char *star = strchr(name, '*');
    if (star) {
        *star = 0;
        hold = atof(star + 1);
    }
    for (unsigned i = 0; i < NCTL; i++) {
        if (!_stricmp(name, k_ctl[i].name)) {
            g_auto.until[i] = now_s() + hold;
            nc_log("hakuX-auto", "%.1f s: %s for %.1f s", now_s() - g_auto.t0, k_ctl[i].name, hold);
            return;
        }
    }
    nc_log("hakuX-auto", "unknown action \"%s\"", a);
}

static int by_time(const void *x, const void *y)
{
    const double a = ((const AutoEvent *)x)->at, b = ((const AutoEvent *)y)->at;
    return a < b ? -1 : a > b;
}

void nc_auto_init(const char *exe_dir, void (*save)(int slot), void (*load)(int slot))
{
    const char *script = getenv("HAKUX_AUTO");
    const char *cmd = getenv("HAKUX_AUTO_CMD");
    if (!script && !cmd) {
        return;
    }
    g_auto.on = true;
    g_auto.save = save;
    g_auto.load = load;
    const char *t0 = getenv("HAKUX_AUTO_T0");
    g_auto.t0 = t0 ? atof(t0) : now_s();
    if (!t0) {
        char v[64];
        snprintf(v, sizeof(v), "%.3f", g_auto.t0);
        SetEnvironmentVariableA("HAKUX_AUTO_T0", v); /* a relaunch's process inherits it */
        setenv("HAKUX_AUTO_T0", v, 1);
    }
    const char *dir = getenv("HAKUX_AUTO_DIR");
    if (dir) {
        snprintf(g_auto.dir, sizeof(g_auto.dir), "%s", dir);
    } else {
        snprintf(g_auto.dir, sizeof(g_auto.dir), "%sshots", exe_dir);
    }
    CreateDirectoryA(g_auto.dir, NULL);
    const double el = now_s() - g_auto.t0;
    for (const char *p = script; p && *p && g_auto.n < AUTO_MAX;) {
        const char *e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char item[64];
        snprintf(item, sizeof(item), "%.*s", (int)(len < sizeof(item) ? len : sizeof(item) - 1), p);
        char *colon = strchr(item, ':');
        if (colon) {
            *colon = 0;
            AutoEvent *ev = &g_auto.ev[g_auto.n];
            ev->at = atof(item);
            snprintf(ev->act, sizeof(ev->act), "%s", colon + 1);
            if (ev->at >= el) { /* a relaunch: the ones before it were the last process's */
                g_auto.n++;
            }
        }
        p = e ? e + 1 : NULL;
    }
    qsort(g_auto.ev, g_auto.n, sizeof(g_auto.ev[0]), by_time);
    if (cmd) {
        snprintf(g_auto.cmd, sizeof(g_auto.cmd), "%s", cmd);
        struct _stat64 st;
        g_auto.cmd_off = _stat64(cmd, &st) == 0 ? st.st_size : 0; /* lines from now on */
    }
    nc_log("hakuX-auto", "%u scripted actions%s%s, %.1f s into the run, pictures in %s", g_auto.n,
           cmd ? ", commands from " : "", cmd ? cmd : "", el, g_auto.dir);
}

static void poll_cmd(double now)
{
    if (!g_auto.cmd[0] || now - g_auto.cmd_seen < 0.1) {
        return;
    }
    g_auto.cmd_seen = now;
    FILE *f = fopen(g_auto.cmd, "rb");
    if (!f) {
        return;
    }
    if (_fseeki64(f, g_auto.cmd_off, SEEK_SET) == 0) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            const size_t len = strlen(line);
            if (!len || line[len - 1] != '\n') {
                break; /* half written: the next poll */
            }
            g_auto.cmd_off += (long long)len;
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) {
                char label[32];
                snprintf(label, sizeof(label), "%.0f", now - g_auto.t0);
                act(line, label);
            }
        }
    }
    fclose(f);
}

void nc_auto_tick(void)
{
    if (!g_auto.on) {
        return;
    }
    const double now = now_s();
    while (g_auto.next < g_auto.n && now - g_auto.t0 >= g_auto.ev[g_auto.next].at) {
        char label[32];
        snprintf(label, sizeof(label), "%03.0f", g_auto.ev[g_auto.next].at);
        act(g_auto.ev[g_auto.next].act, label);
        g_auto.next++;
    }
    poll_cmd(now);
}

void nc_auto_pad(NcPadState *s)
{
    if (!g_auto.on) {
        return;
    }
    const double now = now_s();
    for (unsigned i = 0; i < NCTL; i++) {
        if (g_auto.until[i] <= now) {
            continue;
        }
        if (k_ctl[i].analog >= 0) {
            s->analog[k_ctl[i].analog] = 0xFF;
        } else if (k_ctl[i].stick) {
            int16_t *axis[4] = {&s->lx, &s->ly, &s->rx, &s->ry};
            *axis[k_ctl[i].stick - 1] = (int16_t)(k_ctl[i].dir * 32767);
        } else {
            s->buttons |= k_ctl[i].bit;
        }
    }
}

bool nc_auto_take_menu(void)
{
    const bool m = g_auto.menu;
    g_auto.menu = false;
    return m;
}

void nc_auto_resync(void)
{
    struct _stat64 st;
    if (g_auto.cmd[0] && _stat64(g_auto.cmd, &st) == 0) {
        g_auto.cmd_off = st.st_size;    /* what was typed meanwhile was someone else's (the menu's) */
    }
}
