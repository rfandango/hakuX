/*
 * Host test of native/core/xdvdfs.c: a directory whose entry table is a chain
 * rather than a balanced tree.
 *
 * XDVDFS lists a directory as a binary tree of entries, and some disc
 * repackers write it as one descending chain (every node's only child is the
 * next entry). A chain puts the last entries dozens of levels deep, so a
 * lookup bounded by depth misses them even though they are inside the table's
 * own extent. The reader must find every entry of a chain of any length that
 * fits the extent.
 *
 *   cc -O1 -I. tests/native/xc/xdvdfs_chain_test.c native/core/xdvdfs.c -o xdvdfs_chain_test
 */
#include "native/core/xdvdfs.h"
#include "native/core/nc.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

#define SECTOR 2048u
#define VD_SECTOR 32u
#define ROOT_SECTOR 33u
#define ENTRIES 76 /* the chain length that broke: 76 entries, target last */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One entry at "off" (in dwords) of the table: chain to the next, or a file. */
static size_t put_entry(uint8_t *table, size_t off, const char *name, uint32_t sector, uint32_t size,
                        uint16_t right_dword)
{
    uint8_t *e = table + off;
    size_t nlen = strlen(name);
    put16(e, 0);              /* left: none */
    put16(e + 2, right_dword); /* right: the next entry of the chain */
    put32(e + 4, sector);
    put32(e + 8, size);
    e[12] = 0x20; /* a file */
    e[13] = (uint8_t)nlen;
    memcpy(e + 14, name, nlen);
    const size_t used = 14 + nlen;
    const size_t padded = (used + 3) & ~(size_t)3;
    memset(e + used, 0, padded - used);
    return padded;
}

/* Builds a 34-sector image whose root table is a chain of ENTRIES entries,
 * the last one "target.bin" at the depth the chain gives it. */
static int build_image(const char *path)
{
    enum { SECTORS = ROOT_SECTOR + 1 };
    uint8_t *img = calloc(SECTORS, SECTOR);
    if (!img) {
        return -1;
    }
    uint8_t *vd = img + VD_SECTOR * SECTOR;
    const char magic[] = "MICROSOFT*XBOX*MEDIA";
    memcpy(vd, magic, 20);
    put32(vd + 20, ROOT_SECTOR);
    put32(vd + 24, SECTOR);
    memcpy(vd + 0x7EC, magic, 20);

    uint8_t *table = img + ROOT_SECTOR * SECTOR;
    char name[32];
    size_t off = 0;
    for (unsigned i = 0; i < ENTRIES; i++) {
        const bool last = i + 1 == ENTRIES;
        if (last) {
            snprintf(name, sizeof(name), "target.bin");
        } else {
            snprintf(name, sizeof(name), "f%02u.bin", i);
        }
        /* The next entry starts right after this one, so the right child's
         * dword offset is known only once the entry's own size is: chain
         * forward by writing the current entry with a provisional child and
         * patching it after. */
        size_t used = put_entry(table, off, name, 1000u + i, 40u * (i + 1), 0);
        if (!last) {
            put16(table + off + 2, (uint16_t)((off + used) / 4));
        }
        off += used;
    }
    printf("chain of %u entries uses %zu of %u bytes\n", (unsigned)ENTRIES, off, SECTOR);
    if (off > SECTOR) {
        free(img);
        return -1; /* the fixture must fit the extent it declares */
    }

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        free(img);
        return -1;
    }
    const ssize_t n = write(fd, img, (size_t)SECTORS * SECTOR);
    close(fd);
    free(img);
    return n == (ssize_t)(SECTORS * SECTOR) ? 0 : -1;
}

int main(void)
{
    const char *path = "xdvdfs_chain_test.img";
    if (build_image(path) != 0) {
        printf("FAIL: could not build the test image\n");
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("FAIL: could not open the test image\n");
        return 1;
    }
    NcDisc d;
    CHECK(nc_disc_open(&d, fd));

    NcDiscFile f = {0, 0, 0};

    /* The first entry of the chain, at depth 0. */
    CHECK(nc_disc_find(&d, "f00.bin", &f));
    CHECK(f.sector == 1000u);
    CHECK(f.size == 40u);

    /* Every entry of the chain is reachable, whatever its depth. */
    CHECK(nc_disc_find(&d, "f40.bin", &f));
    CHECK(f.sector == 1040u);

    /* The last entry: 75 links deep in this chain. */
    f.sector = 0;
    f.size = 0;
    CHECK(nc_disc_find(&d, "target.bin", &f));
    CHECK(f.sector == 1000u + ENTRIES - 1);
    CHECK(f.size == 40u * ENTRIES);

    /* Names are still matched case-insensitively, and only when they match. */
    CHECK(nc_disc_find(&d, "TARGET.BIN", &f));
    CHECK(!nc_disc_find(&d, "nope.bin", &f));

    nc_disc_close(&d);
    close(fd);
    unlink(path);

    printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
