/*
 * hakuX native core — the GLib libslirp (the NAT backend, native/net/nat.c)
 * uses beyond the HLE layer's subset (native/compat/glib): strings, a random
 * source, messages to the log, and failing stubs for the process spawning
 * only its guestfwd exec takes (never configured here). libslirp's sources
 * include <glib.h>; this directory comes first on their include path.
 */
#ifndef NATIVE_NET_SLIRP_GLIB_H
#define NATIVE_NET_SLIRP_GLIB_H

#include "native/compat/glib/glib.h"

#include <stdio.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G_OS_UNIX 1
#define G_LITTLE_ENDIAN 1234
#define G_BIG_ENDIAN 4321
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define G_BYTE_ORDER G_LITTLE_ENDIAN
#else
#define G_BYTE_ORDER G_BIG_ENDIAN
#endif
#define G_STATIC_ASSERT(e) _Static_assert(e, #e)
#define GLIB_CHECK_VERSION(major, minor, micro) 1
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define GUINT16_TO_BE(v) ((guint16)__builtin_bswap16((guint16)(v)))
#define GUINT32_TO_BE(v) ((guint32)__builtin_bswap32((guint32)(v)))
#else
#define GUINT16_TO_BE(v) ((guint16)(v))
#define GUINT32_TO_BE(v) ((guint32)(v))
#endif
#define GINT16_TO_BE(v) ((gint16)GUINT16_TO_BE(v))
#define GINT32_TO_BE(v) ((gint32)GUINT32_TO_BE(v))
#define GUINT16_FROM_BE(v) GUINT16_TO_BE(v)
#define GUINT32_FROM_BE(v) GUINT32_TO_BE(v)
#define GINT16_FROM_BE(v) GINT16_TO_BE(v)
#define GINT32_FROM_BE(v) GINT32_TO_BE(v)
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef gchar **GStrv;
typedef int GPid;
typedef void (*GSpawnChildSetupFunc)(gpointer data);
typedef enum GSpawnFlags { G_SPAWN_DEFAULT = 0, G_SPAWN_SEARCH_PATH = 1 << 2 } GSpawnFlags;
typedef struct GDebugKey {
    const gchar *key;
    guint value;
} GDebugKey;

/* Messages: to the log as hakuX-native-slirp; g_error aborts, as GLib's. */
enum { G_SLIRP_ERROR, G_SLIRP_CRITICAL, G_SLIRP_WARNING, G_SLIRP_DEBUG };
void g_slirp_log(int level, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
#define g_debug(...) g_slirp_log(G_SLIRP_DEBUG, __VA_ARGS__)
#define g_warning(...) g_slirp_log(G_SLIRP_WARNING, __VA_ARGS__)
#define g_critical(...) g_slirp_log(G_SLIRP_CRITICAL, __VA_ARGS__)
#define g_error(...) (g_slirp_log(G_SLIRP_ERROR, __VA_ARGS__), abort())
#define g_return_if_fail(e)                                                                  \
    do {                                                                                     \
        if (G_UNLIKELY(!(e))) {                                                              \
            g_slirp_log(G_SLIRP_CRITICAL, "%s: assertion '%s' failed", __func__, #e);         \
            return;                                                                          \
        }                                                                                    \
    } while (0)
#define g_return_val_if_fail(e, v)                                                           \
    do {                                                                                     \
        if (G_UNLIKELY(!(e))) {                                                              \
            g_slirp_log(G_SLIRP_CRITICAL, "%s: assertion '%s' failed", __func__, #e);         \
            return (v);                                                                      \
        }                                                                                    \
    } while (0)
#define g_warn_if_fail(e)                                                                    \
    do {                                                                                     \
        if (G_UNLIKELY(!(e))) {                                                              \
            g_slirp_log(G_SLIRP_WARNING, "%s:%d: '%s' failed", __FILE__, __LINE__, #e);       \
        }                                                                                    \
    } while (0)
#define g_warn_if_reached() g_slirp_log(G_SLIRP_WARNING, "%s:%d: code should not be reached", __FILE__, __LINE__)

/* Strings */
#define g_snprintf snprintf
#define g_vsnprintf vsnprintf
#define g_strerror strerror
#define g_getenv getenv
#define g_ascii_strcasecmp strcasecmp   /* bionic's compares ASCII only */
gsize g_strlcpy(gchar *dest, const gchar *src, gsize dest_size);
gchar *g_strstr_len(const gchar *haystack, gssize haystack_len, const gchar *needle);
gboolean g_str_has_prefix(const gchar *str, const gchar *prefix);
guint g_strv_length(gchar **str_array);
void g_strfreev(gchar **str_array);
guint g_parse_debug_string(const gchar *string, const GDebugKey *keys, guint nkeys);

typedef struct GString {
    gchar *str;
    gsize len;
    gsize allocated_len;
} GString;
GString *g_string_new(const gchar *init);
void g_string_append_printf(GString *string, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
gchar *g_string_free(GString *string, gboolean free_segment);

/* A random source: arc4random underneath. */
typedef struct GRand GRand;
GRand *g_rand_new(void);
void g_rand_free(GRand *rand);
gint32 g_rand_int_range(GRand *rand, gint32 begin, gint32 end);

/* Spawning: always fails (no guestfwd exec). */
gboolean g_shell_parse_argv(const gchar *command_line, gint *argcp, gchar ***argvp, GError **error);
gboolean g_spawn_async_with_fds(const gchar *working_directory, gchar **argv, gchar **envp, GSpawnFlags flags,
                                GSpawnChildSetupFunc child_setup, gpointer user_data, GPid *child_pid,
                                gint stdin_fd, gint stdout_fd, gint stderr_fd, GError **error);

#ifdef __cplusplus
}
#endif

#endif
