/*
 * xbased on Linux: what Android's libc gives the core and glibc doesn't
 * (system properties: sys/system_properties.h), and the thread helpers the
 * Windows build's compat layer has (nc_win_thread_name, _priority), and
 * what an older glibc lacks (arc4random: 2.36, strlcpy: 2.38) for the
 * release built on one.
 */
#define _GNU_SOURCE
#include <sys/system_properties.h>

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/random.h>

#if !__GLIBC_PREREQ(2, 36)
void arc4random_buf(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        const ssize_t got = getrandom(p, n, 0);
        if (got > 0) {
            p += got;
            n -= (size_t)got;
        }
    }
}

uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof(v));
    return v;
}

uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2) {
        return 0;
    }
    /* the values below 2^32 mod bound would come up once more often */
    const uint32_t min = -bound % bound;
    for (;;) {
        const uint32_t r = arc4random();
        if (r >= min) {
            return r % bound;
        }
    }
}
#endif

#if !__GLIBC_PREREQ(2, 38)
size_t strlcpy(char *dst, const char *src, size_t n)
{
    const size_t len = strlen(src);
    if (n) {
        const size_t k = len < n - 1 ? len : n - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return len;
}
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_props;
static size_t g_len;
static struct timespec g_mtime;
static struct timespec g_checked;

static struct {
    char name[PROP_NAME_MAX];
    char value[PROP_VALUE_MAX];
} g_set[16];

static void props_path(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) {
        snprintf(out, n, "props.ini");
        return;
    }
    exe[k] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) {
        slash[1] = 0;
    }
    snprintf(out, n, "%sprops.ini", exe);
}

/* the file again when it changed, looked at no more than every 100 ms */
static void refresh(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - g_checked.tv_sec) * 1000 + (now.tv_nsec - g_checked.tv_nsec) / 1000000 < 100 && g_checked.tv_sec) {
        return;
    }
    g_checked = now;
    char path[PATH_MAX + 16];
    props_path(path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        free(g_props);
        g_props = NULL;
        g_len = 0;
        return;
    }
    if (g_props && st.st_mtim.tv_sec == g_mtime.tv_sec && st.st_mtim.tv_nsec == g_mtime.tv_nsec) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    char *buf = malloc(64 * 1024);
    size_t n = buf ? fread(buf, 1, 64 * 1024 - 1, f) : 0;
    fclose(f);
    if (!buf) {
        return;
    }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = 0;
        }
    }
    free(g_props);
    g_props = buf;
    g_len = n;
    g_mtime = st.st_mtim;
}

void nc_win_prop_set(const char *name, const char *value)
{
    pthread_mutex_lock(&g_mu);
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(g_set[i].name, name)) {
            slot = i;
            break;
        }
        if (!g_set[i].name[0] && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        snprintf(g_set[slot].name, PROP_NAME_MAX, "%s", value ? name : "");
        snprintf(g_set[slot].value, PROP_VALUE_MAX, "%s", value ? value : "");
    }
    pthread_mutex_unlock(&g_mu);
}

int __system_property_get(const char *name, char *value)
{
    value[0] = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 16; i++) {
        if (g_set[i].name[0] && !strcmp(g_set[i].name, name)) {
            snprintf(value, PROP_VALUE_MAX, "%s", g_set[i].value);
            pthread_mutex_unlock(&g_mu);
            return (int)strlen(value);
        }
    }
    refresh();
    const size_t nl = strlen(name);
    for (size_t i = 0; g_props && i < g_len;) {
        const char *line = g_props + i;
        const size_t ll = strlen(line);
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (line[0] != '#' && !strncmp(line, name, nl)) {
            const char *p = line + nl;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                snprintf(value, PROP_VALUE_MAX, "%s", p);
                size_t vl = strlen(value);
                while (vl && (value[vl - 1] == ' ' || value[vl - 1] == '\t')) {
                    value[--vl] = 0;
                }
                pthread_mutex_unlock(&g_mu);
                return (int)vl;
            }
        }
        i += ll + 1;
    }
    pthread_mutex_unlock(&g_mu);
    char env[PROP_NAME_MAX * 2 + 8];
    size_t k = 0;
    for (; name[k] && k < sizeof(env) - 1; k++) {
        env[k] = name[k] == '.' ? '_' : (char)toupper((unsigned char)name[k]);
    }
    env[k] = 0;
    const char *v = getenv(env);
    if (!v) {
        return 0;
    }
    snprintf(value, PROP_VALUE_MAX, "%s", v);
    return (int)strlen(value);
}

void nc_win_thread_name(const char *name)
{
    char n[16];
    snprintf(n, sizeof(n), "%s", name);
    pthread_setname_np(pthread_self(), n);
}

void nc_win_thread_priority(int level)
{
    (void)level;    /* raising it needs privileges on Linux; the scheduler's default */
}
