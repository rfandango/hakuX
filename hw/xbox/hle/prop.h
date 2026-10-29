/*
 * hakuX HLE — the debug properties (debug.hakux.*): every read goes through
 * hle_prop_get, and the on-demand ones that act on a new value through
 * hle_prop_new. For the latter, the value seen at the first poll is the
 * baseline, an empty one included (a property reset to '' before a run);
 * afterwards each change to a non-empty value triggers once.
 */
#ifndef HW_XBOX_HLE_PROP_H
#define HW_XBOX_HLE_PROP_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "native/platform/properties.h"

/* A debug property's value in v (empty when unset) and its length. With
 * HAKUX_NO_PROPS=1 none is read, each as if unset: a run that mustn't depend
 * on what was left set on the device (they persist across runs). */
static inline int hle_prop_get(const char *name, char v[NC_PROPERTY_VALUE_MAX])
{
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("HAKUX_NO_PROPS");
        off = e && e[0] == '1';
    }
    v[0] = 0;
    return off ? 0 : nc_property_get(name, v);
}

typedef struct HleProp {
    char last[NC_PROPERTY_VALUE_MAX];
    bool polled;
} HleProp;

/* True when name holds a new, non-empty value (returned in v). */
static inline bool hle_prop_new(HleProp *p, const char *name, char v[NC_PROPERTY_VALUE_MAX])
{
    hle_prop_get(name, v);
    bool first = !p->polled;
    p->polled = true;
    if (!strcmp(v, p->last)) {
        return false;
    }
    strcpy(p->last, v);
    return !first && v[0];
}
#endif
