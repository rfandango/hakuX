#include "hw/xbox/hle/core/xbe.h"
#include "hle_test.h"
#include "xbe_fixture.h"

static uint8_t buf[0x1000];

static const FxSection k_secs[] = {
    { ".text", 0x06, 0x00011000, 0x00100000, 0x001000, 0x100000 },
    { "D3D",   0x06, 0x00111000, 0x00040000, 0x101000, 0x040000 },
    { "XGRPH", 0x06, 0x00151000, 0x00008000, 0x141000, 0x008000 },
    { ".data", 0x03, 0x00159000, 0x00020000, 0x149000, 0x010000 },
};

static const FxLib k_libs[] = {
    { "XAPILIB", 1, 0, 5849, 0x4001 },
    { "D3D8",    1, 0, 5849, 0x4003 },
    { "XGRAPHC", 1, 0, 5849, 0x8002 },
};

static void test_retail_header(void)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false,
                                k_secs, 4, k_libs, 3, 0x4D530064);
    HleXbeInfo x;
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    HT_EQ_U32(x.base, 0x10000);
    HT_EQ_U32(x.timedate, 0x3C000000);
    HT_EQ_U32(x.entry, 0x00012345);
    HT_EQ_U32(x.kernel_thunk, 0x00012000);
    HT_CHECK(!x.debug_keys);
    HT_EQ_U32(x.title_id, 0x4D530064);
    HT_EQ_U32(x.nsections, 4);
    HT_EQ_STR(x.sections[1].name, "D3D");
    HT_EQ_U32(x.sections[1].va, 0x00111000);
    HT_EQ_U32(x.sections[1].vsize, 0x40000);
    HT_EQ_U32(x.sections[1].flags, 0x06);
    HT_EQ_U32(x.nlibs, 3);
}

static void test_library_versions(void)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false,
                                k_secs, 4, k_libs, 3, 0);
    HleXbeInfo x;
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    const HleXbeLib *d3d = hle_xbe_find_lib(&x, "D3D8");
    HT_CHECK(d3d != NULL);
    if (d3d) {
        HT_EQ_U32(d3d->build, 5849);
        HT_EQ_U32(d3d->qfe, 3);
        HT_CHECK(!d3d->debug_build);
    }
    const HleXbeLib *xg = hle_xbe_find_lib(&x, "XGRAPHC");
    HT_CHECK(xg != NULL);
    if (xg) {
        HT_EQ_U32(xg->qfe, 2);
        HT_CHECK(xg->debug_build);
    }
    HT_CHECK(hle_xbe_find_lib(&x, "D3D8LTCG") == NULL);
}

static void test_debug_keys(void)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00020000, true,
                                k_secs, 4, k_libs, 3, 0);
    HleXbeInfo x;
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    HT_CHECK(x.debug_keys);
    HT_EQ_U32(x.entry, 0x00020000);
    HT_EQ_U32(x.kernel_thunk, 0x00012000);
}

/* Guest-memory headers (0x10000 at runtime) carry the entry point and kernel
 * thunk already decoded, unlike XBE files. */
static void test_plain_entry(void)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false,
                                k_secs, 4, k_libs, 3, 0x4D530064);
    fx_w32(buf, 0x128, 0x00012345);
    fx_w32(buf, 0x158, 0x00012000);
    HleXbeInfo x;
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    HT_CHECK(x.plain_entry);
    HT_CHECK(!x.debug_keys);
    HT_EQ_U32(x.entry, 0x00012345);
    HT_EQ_U32(x.kernel_thunk, 0x00012000);
    HT_EQ_U32(x.title_id, 0x4D530064);
}

static void test_errors(void)
{
    HleXbeInfo x;
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false,
                                k_secs, 4, k_libs, 3, 0);
    HT_EQ_INT(hle_xbe_parse(buf, 0x100, &x), HLE_XBE_ERR_SHORT);
    buf[0] = 'Y';
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_ERR_MAGIC);
    buf[0] = 'X';
    fx_w32(buf, 0x11C, 200);                      /* headers run past len */
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_ERR_RANGE);
    fx_w32(buf, 0x11C, 4);
    fx_w32(buf, 0x128, 0x12345678u);              /* entry decodes nowhere */
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_ERR_ENTRY);
}

static void test_lookups(void)
{
    uint32_t len = fx_build_xbe(buf, sizeof(buf), 0x00012345, false,
                                k_secs, 4, k_libs, 3, 0);
    HleXbeInfo x;
    HT_EQ_INT(hle_xbe_parse(buf, len, &x), HLE_XBE_OK);
    const HleXbeSection *s = hle_xbe_find_section(&x, "XGRPH");
    HT_CHECK(s && s->va == 0x00151000);
    s = hle_xbe_section_at(&x, 0x00111010);
    HT_CHECK(s && strcmp(s->name, "D3D") == 0);
    HT_CHECK(hle_xbe_section_at(&x, 0x00500000) == NULL);
    HT_CHECK(hle_xbe_find_section(&x, "NOPE") == NULL);
}

/* fx_build_xbe() has room for 18 section headers; this moves them to
 * 0x1000 (names after them) so nsec up to HLE_XBE_MAX_SECTIONS + 1 fit.
 * Section i is "S<i>" at 0x11000 + i * 0x1000, the last one "XGRPH". */
#define BIG_SIZE ((0x1000 + (HLE_XBE_MAX_SECTIONS + 1) * (56 + 9) + 0xFFF) & ~0xFFFu)
static uint8_t big[BIG_SIZE < 0x6000 ? 0x6000 : BIG_SIZE];

static uint32_t build_many_sections(unsigned nsec)
{
    fx_build_xbe(big, sizeof(big), 0x00012345, false, NULL, 0, k_libs, 3,
                 0x4D530064);
    fx_w32(big, 0x108, sizeof(big));              /* sizeof_headers */
    fx_w32(big, 0x11C, nsec);
    fx_w32(big, 0x120, 0x10000 + 0x1000);
    uint32_t name_off = 0x1000 + nsec * 56;
    for (unsigned i = 0; i < nsec; i++) {
        uint32_t s = 0x1000 + i * 56;
        fx_w32(big, s + 0x00, 0x06);
        fx_w32(big, s + 0x04, 0x11000 + i * 0x1000);
        fx_w32(big, s + 0x08, 0x1000);
        fx_w32(big, s + 0x0C, 0x1000 + i * 0x1000);
        fx_w32(big, s + 0x10, 0x1000);
        fx_w32(big, s + 0x14, 0x10000 + name_off);
        int n = i + 1 == nsec ? snprintf((char *)big + name_off, 9, "XGRPH")
                              : snprintf((char *)big + name_off, 9, "S%u", i);
        name_off += (uint32_t)n + 1;
    }
    HT_CHECK(name_off <= sizeof(big));
    return sizeof(big);
}

/* Fable has 107 sections; the old cap of 96 rejected it. */
static void test_many_sections(void)
{
    HleXbeInfo x;
    uint32_t len = build_many_sections(107);
    int rc = hle_xbe_parse(big, len, &x);
    HT_EQ_INT(rc, HLE_XBE_OK);
    if (rc != HLE_XBE_OK) {
        return;                     /* sections[] may be shorter than 107 */
    }
    HT_EQ_U32(x.nsections, 107);
    HT_EQ_STR(x.sections[96].name, "S96");
    HT_EQ_U32(x.sections[96].va, 0x11000 + 96 * 0x1000);
    HT_EQ_STR(x.sections[106].name, "XGRPH");
    HT_EQ_U32(x.sections[106].va, 0x11000 + 106 * 0x1000);
    HT_EQ_U32(x.sections[106].raw_off, 0x1000 + 106 * 0x1000);
    const HleXbeSection *s = hle_xbe_find_section(&x, "XGRPH");
    HT_CHECK(s == &x.sections[106]);
    s = hle_xbe_section_at(&x, 0x11000 + 100 * 0x1000 + 0x10);
    HT_CHECK(s && strcmp(s->name, "S100") == 0);
    HT_EQ_U32(x.nlibs, 3);
    HT_EQ_U32(x.title_id, 0x4D530064);

    len = build_many_sections(HLE_XBE_MAX_SECTIONS);
    HT_EQ_INT(hle_xbe_parse(big, len, &x), HLE_XBE_OK);
    HT_EQ_U32(x.nsections, HLE_XBE_MAX_SECTIONS);
    HT_EQ_STR(x.sections[HLE_XBE_MAX_SECTIONS - 1].name, "XGRPH");

    /* Spikeout: Battle Street has 398 (one per Sega module); 256 rejected it. */
    len = build_many_sections(398);
    rc = hle_xbe_parse(big, len, &x);
    HT_EQ_INT(rc, HLE_XBE_OK);
    if (rc == HLE_XBE_OK) {
        HT_EQ_U32(x.nsections, 398);
        HT_EQ_STR(x.sections[300].name, "S300");
        HT_EQ_STR(x.sections[397].name, "XGRPH");
        HT_EQ_U32(x.sections[397].va, 0x11000 + 397 * 0x1000);
        HT_EQ_U32(x.sections[397].raw_off, 0x1000 + 397 * 0x1000);
    }

    /* One more than the cap: the headers fit in len, the cap rejects. */
    len = build_many_sections(HLE_XBE_MAX_SECTIONS + 1);
    HT_EQ_INT(hle_xbe_parse(big, len, &x), HLE_XBE_ERR_RANGE);
}

int main(void)
{
    HT_RUN(test_retail_header);
    HT_RUN(test_library_versions);
    HT_RUN(test_debug_keys);
    HT_RUN(test_plain_entry);
    HT_RUN(test_errors);
    HT_RUN(test_lookups);
    HT_RUN(test_many_sections);
    HT_DONE();
}
