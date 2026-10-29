#ifndef NATIVE_PLATFORM_PROPERTIES_H
#define NATIVE_PLATFORM_PROPERTIES_H

#ifdef __ANDROID__
#include <sys/system_properties.h>
#define NC_PROPERTY_VALUE_MAX PROP_VALUE_MAX
static inline int nc_property_get(const char *name, char *value)
{
    return __system_property_get(name, value);
}
#else
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC_PROPERTY_VALUE_MAX 92
/* Linux startup configuration, e.g. HAKUX_PROP_DEBUG_HAKUX_APUSPIN=0.
 * Changing the parent's environment cannot update a running process. */
static inline int nc_property_get(const char *name, char *value)
{
    char key[128] = "HAKUX_PROP_";
    size_t n = strlen(key);
    while (*name && n + 1 < sizeof(key)) {
        unsigned char c = (unsigned char)*name++;
        key[n++] = isalnum(c) ? (char)toupper(c) : '_';
    }
    key[n] = 0;
    const char *v = *name ? NULL : getenv(key);
    snprintf(value, NC_PROPERTY_VALUE_MAX, "%s", v ? v : "");
    return (int)strlen(value);
}
#endif
#endif
