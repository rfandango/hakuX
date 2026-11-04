/*
 * xbased's PC front end (win_main.c, win_mods.c, win_auto.c) on Linux: the
 * few Windows calls it makes, over POSIX. On Windows: windows.h.
 */
#ifndef NATIVE_APP_DESK_H
#define NATIVE_APP_DESK_H

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH 4096
#define O_BINARY 0
#define _strnicmp strncasecmp
#define _stricmp strcasecmp
#define _fseeki64 fseeko
#define _stat64 stat
typedef uint32_t DWORD;
typedef void *HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10

void nc_win_thread_name(const char *name);     /* compat/linux/linux.c */
void nc_win_thread_priority(int level);

static inline DWORD GetModuleFileNameA(void *m, char *out, DWORD n)
{
    (void)m;
    const ssize_t k = readlink("/proc/self/exe", out, n - 1);
    out[k > 0 ? k : 0] = 0;
    return k > 0 ? (DWORD)k : 0;
}

static inline DWORD GetFileAttributesA(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0x80;
}

static inline bool MoveFileA(const char *from, const char *to)
{
    return rename(from, to) == 0;
}

static inline bool CreateDirectoryA(const char *p, void *sa)
{
    (void)sa;
    return mkdir(p, 0775) == 0;
}

static inline bool SetEnvironmentVariableA(const char *n, const char *v)
{
    return setenv(n, v, 1) == 0;
}

typedef struct {
    DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;

/* 100 ns intervals (since the Unix epoch here: only differences are used) */
static inline void GetSystemTimeAsFileTime(FILETIME *ft)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    const uint64_t v = (uint64_t)t.tv_sec * 10000000u + (uint64_t)t.tv_nsec / 100u;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

typedef struct {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

static inline void GetLocalTime(SYSTEMTIME *s)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    s->wYear = (uint16_t)(t.tm_year + 1900);
    s->wMonth = (uint16_t)(t.tm_mon + 1);
    s->wDayOfWeek = (uint16_t)t.tm_wday;
    s->wDay = (uint16_t)t.tm_mday;
    s->wHour = (uint16_t)t.tm_hour;
    s->wMinute = (uint16_t)t.tm_min;
    s->wSecond = (uint16_t)t.tm_sec;
    s->wMilliseconds = 0;
}

/* FindFirstFile over a "<dir>/<glob>" pattern ('\' taken as '/') */
typedef struct {
    DWORD dwFileAttributes;
    char cFileName[256];
} WIN32_FIND_DATAA;

typedef struct {
    DIR *d;
    char dir[MAX_PATH];
    char glob[256];
} DeskFind;

static inline bool desk_find_next(DeskFind *f, WIN32_FIND_DATAA *fd)
{
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->glob, e->d_name, 0) == 0) {
            char p[MAX_PATH + 260];
            snprintf(p, sizeof(p), "%s/%s", f->dir, e->d_name);
            snprintf(fd->cFileName, sizeof(fd->cFileName), "%s", e->d_name);
            fd->dwFileAttributes = GetFileAttributesA(p) == FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : 0x80;
            return true;
        }
    }
    return false;
}

static inline HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd)
{
    DeskFind *f = (DeskFind *)calloc(1, sizeof(*f));
    snprintf(f->dir, sizeof(f->dir), "%s", pattern);
    for (char *c = f->dir; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    char *slash = strrchr(f->dir, '/');
    if (slash) {
        snprintf(f->glob, sizeof(f->glob), "%s", slash + 1);
        *slash = 0;
    } else {
        snprintf(f->glob, sizeof(f->glob), "%s", f->dir);
        snprintf(f->dir, sizeof(f->dir), ".");
    }
    f->d = opendir(f->dir);
    if (!f->d || !desk_find_next(f, fd)) {
        if (f->d) {
            closedir(f->d);
        }
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

static inline bool FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    return desk_find_next((DeskFind *)h, fd);
}

static inline bool FindClose(HANDLE h)
{
    DeskFind *f = (DeskFind *)h;
    closedir(f->d);
    free(f);
    return true;
}
#endif

#endif
