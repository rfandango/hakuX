/*
 * xbased on Linux: Android's system properties, for the debug.hakux.*
 * switches, as on Windows: props.ini beside the executable (name=value
 * lines, read again when it changes), values set while running, or the
 * environment (debug.hakux.perf: DEBUG_HAKUX_PERF). compat/linux/linux.c.
 */
#ifndef NC_LINUX_SYSTEM_PROPERTIES_H
#define NC_LINUX_SYSTEM_PROPERTIES_H

#define PROP_VALUE_MAX 92
#define PROP_NAME_MAX 32

#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char *name, char *value);
/* A value set while running, over props.ini's (NULL: unset). */
void nc_win_prop_set(const char *name, const char *value);
#ifdef __cplusplus
}
#endif

#endif
