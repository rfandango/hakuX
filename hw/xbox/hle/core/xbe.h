/*
 * hakuX HLE GPU — pure XBE header parser.
 * Layout reference: xemu-xbe.h (struct xbe_header, packed).
 */
#ifndef HW_XBOX_HLE_CORE_XBE_H
#define HW_XBOX_HLE_CORE_XBE_H

#include "hw/xbox/hle/hle_pure.h"

#define HLE_XBE_MAGIC            0x48454258u  /* "XBEH" */
#define HLE_XBE_ENTRY_KEY_RETAIL 0xA8FC57ABu
#define HLE_XBE_ENTRY_KEY_DEBUG  0x94859D4Bu
#define HLE_XBE_THUNK_KEY_RETAIL 0x5B6D40B6u
#define HLE_XBE_THUNK_KEY_DEBUG  0xEFB1F152u

#define HLE_XBE_SEC_WRITABLE   0x01u
#define HLE_XBE_SEC_PRELOAD    0x02u
#define HLE_XBE_SEC_EXECUTABLE 0x04u

#define HLE_XBE_MAX_SECTIONS 1024  /* Fable has 107, Spikeout: Battle Street 398 */
#define HLE_XBE_MAX_LIBS     48

typedef struct HleXbeSection {
    char     name[9];
    uint32_t flags;
    uint32_t va;
    uint32_t vsize;
    uint32_t raw_off;
    uint32_t raw_size;
} HleXbeSection;

typedef struct HleXbeLib {
    char     name[9];
    uint16_t major, minor, build;
    uint16_t qfe;
    bool     debug_build;
} HleXbeLib;

typedef struct HleXbeInfo {
    uint32_t base;
    uint32_t sizeof_headers;
    uint32_t sizeof_image;
    uint32_t timedate;
    uint32_t entry;          /* decoded */
    uint32_t kernel_thunk;   /* decoded */
    bool     debug_keys;
    bool     plain_entry;    /* stored unencoded (guest-memory header) */
    uint32_t title_id;
    unsigned nsections;
    HleXbeSection sections[HLE_XBE_MAX_SECTIONS];
    unsigned nlibs;
    HleXbeLib libs[HLE_XBE_MAX_LIBS];
} HleXbeInfo;

enum {
    HLE_XBE_OK = 0,
    HLE_XBE_ERR_SHORT = -1,
    HLE_XBE_ERR_MAGIC = -2,
    HLE_XBE_ERR_RANGE = -3,
    HLE_XBE_ERR_ENTRY = -4,
};

/* Parses XBE headers held in hdr[0..hdr_len) (the first sizeof_headers
 * bytes of the image, i.e. what is mapped at the base VA). */
int hle_xbe_parse(const uint8_t *hdr, uint32_t hdr_len, HleXbeInfo *out);
const HleXbeSection *hle_xbe_find_section(const HleXbeInfo *x,
                                          const char *name);
const HleXbeLib *hle_xbe_find_lib(const HleXbeInfo *x, const char *name);
/* The title's D3D library: D3D8, or D3D8LTCG for link-time-compiled titles
 * (the same device structure per build). */
const HleXbeLib *hle_xbe_d3d_lib(const HleXbeInfo *x);
const HleXbeSection *hle_xbe_section_at(const HleXbeInfo *x, uint32_t va);

#endif
