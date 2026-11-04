/*
 * hakuX native core — libslirp's GLib beyond the HLE subset (glib.h here).
 */
#include "glib.h"

#include "native/core/nc.h"

#include <stdarg.h>

void g_slirp_log(int level, const gchar *fmt, ...)
{
    static const char *const what[] = { "error", "critical", "warning", "debug" };
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    nc_log("hakuX-native-slirp", "%s: %s", what[level], line);
}

gsize g_strlcpy(gchar *dest, const gchar *src, gsize dest_size)
{
    return strlcpy(dest, src, dest_size);
}

gchar *g_strstr_len(const gchar *haystack, gssize haystack_len, const gchar *needle)
{
    if (haystack_len < 0) {
        return strstr(haystack, needle);
    }
    const size_t n = strlen(needle);
    if (!n) {
        return (gchar *)haystack;
    }
    /* up to haystack_len bytes or the haystack's terminator */
    for (gssize i = 0; i + (gssize)n <= haystack_len && haystack[i]; i++) {
        if (!memcmp(haystack + i, needle, n)) {
            return (gchar *)haystack + i;
        }
    }
    return NULL;
}

gboolean g_str_has_prefix(const gchar *str, const gchar *prefix)
{
    return !strncmp(str, prefix, strlen(prefix));
}

guint g_strv_length(gchar **str_array)
{
    guint n = 0;
    while (str_array && str_array[n]) {
        n++;
    }
    return n;
}

void g_strfreev(gchar **str_array)
{
    for (guint i = 0; str_array && str_array[i]; i++) {
        g_free(str_array[i]);
    }
    g_free(str_array);
}

guint g_parse_debug_string(const gchar *string, const GDebugKey *keys, guint nkeys)
{
    if (!string) {
        return 0;
    }
    guint flags = 0;
    if (!strcasecmp(string, "all")) {
        for (guint i = 0; i < nkeys; i++) {
            flags |= keys[i].value;
        }
        return flags;
    }
    /* keys separated by ':', ';', ',' or spaces, case-insensitive */
    for (const gchar *p = string; *p;) {
        const size_t len = strcspn(p, ":;, \t");
        for (guint i = 0; i < nkeys; i++) {
            if (strlen(keys[i].key) == len && !strncasecmp(keys[i].key, p, len)) {
                flags |= keys[i].value;
            }
        }
        p += len;
        p += *p != 0;
    }
    return flags;
}

GString *g_string_new(const gchar *init)
{
    GString *s = g_new0(GString, 1);
    s->len = init ? strlen(init) : 0;
    s->allocated_len = s->len + 64;
    s->str = g_malloc(s->allocated_len);
    if (init) {
        memcpy(s->str, init, s->len);
    }
    s->str[s->len] = 0;
    return s;
}

void g_string_append_printf(GString *string, const gchar *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (string->len + (gsize)n + 1 > string->allocated_len) {
            string->allocated_len = (string->len + (gsize)n + 1) * 2;
            string->str = g_realloc(string->str, string->allocated_len);
        }
        vsnprintf(string->str + string->len, (size_t)n + 1, fmt, ap2);
        string->len += (gsize)n;
    }
    va_end(ap2);
}

gchar *g_string_free(GString *string, gboolean free_segment)
{
    gchar *str = string->str;
    if (free_segment) {
        g_free(str);
        str = NULL;
    }
    g_free(string);
    return str;
}

struct GRand {
    int unused;
};

GRand *g_rand_new(void)
{
    return g_new0(GRand, 1);
}

void g_rand_free(GRand *rand)
{
    g_free(rand);
}

gint32 g_rand_int_range(GRand *rand, gint32 begin, gint32 end)
{
    return begin + (gint32)arc4random_uniform((uint32_t)(end - begin));
}

static void spawn_error(GError **error)
{
    if (error) {
        *error = g_new0(GError, 1);
        (*error)->message = g_strdup("processes can't be spawned here");
    }
}

gboolean g_shell_parse_argv(const gchar *command_line, gint *argcp, gchar ***argvp, GError **error)
{
    spawn_error(error);
    return FALSE;
}

gboolean g_spawn_async_with_fds(const gchar *working_directory, gchar **argv, gchar **envp, GSpawnFlags flags,
                                GSpawnChildSetupFunc child_setup, gpointer user_data, GPid *child_pid,
                                gint stdin_fd, gint stdout_fd, gint stderr_fd, GError **error)
{
    spawn_error(error);
    return FALSE;
}
