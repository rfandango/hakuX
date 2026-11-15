/*
 * hakuX native core — bionic's system properties off Android (Linux): the
 * debug properties (debug.hakux.*) are read from the environment under the
 * same names, e.g. `env debug.hakux.caps=-bda hakux game.iso`. Only those
 * read at startup: the environment does not change while the core runs.
 */
#ifndef NATIVE_COMPAT_LINUX_SYS_SYSTEM_PROPERTIES_H
#define NATIVE_COMPAT_LINUX_SYS_SYSTEM_PROPERTIES_H

#include <stdio.h>
#include <stdlib.h>

#define PROP_VALUE_MAX 92

static inline int __system_property_get(const char *name, char *value)
{
    const char *e = getenv(name);
    const int n = snprintf(value, PROP_VALUE_MAX, "%s", e ? e : "");
    return n < PROP_VALUE_MAX ? n : PROP_VALUE_MAX - 1;
}

#endif
