/*
 * hakuX front end — discs and XPR images (xbox_data.h), as the Android
 * app's XboxDisc.kt and XprImage.kt read them.
 */
#include "native/app/ui/xbox_data.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void xd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        char b[4];
        int k;
        if (c < 0x80) {
            b[0] = (char)c;
            k = 1;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | c >> 6);
            b[1] = (char)(0x80 | (c & 63));
            k = 2;
        } else if (c < 0x10000) {
            b[0] = (char)(0xE0 | c >> 12);
            b[1] = (char)(0x80 | (c >> 6 & 63));
            b[2] = (char)(0x80 | (c & 63));
            k = 3;
        } else {
            b[0] = (char)(0xF0 | c >> 18);
            b[1] = (char)(0x80 | (c >> 12 & 63));
            b[2] = (char)(0x80 | (c >> 6 & 63));
            b[3] = (char)(0x80 | (c & 63));
            k = 4;
        }
        if (o + k >= cap) {
            break;
        }
        memcpy(out + o, b, k);
        o += k;
    }
    out[o < cap ? o : cap - 1] = 0;
}

void xd_clean_name(const char *path, char *out, size_t n)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", b);
    /* the extensions: .iso, .xiso.iso, .xiso, .cso, .cci */
    for (int pass = 0; pass < 2; pass++) {
        char *dot = strrchr(tmp, '.');
        if (dot && (!SDL_strcasecmp(dot, ".iso") || !SDL_strcasecmp(dot, ".xiso") || !SDL_strcasecmp(dot, ".cso") ||
                    !SDL_strcasecmp(dot, ".cci"))) {
            *dot = 0;
        }
    }
    size_t o = 0;
    int depth = 0;
    for (const char *p = tmp; *p && o + 1 < n; p++) {
        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
        } else if ((*p == ')' || *p == ']' || *p == '}') && depth) {
            depth--;
        } else if (!depth) {
            out[o++] = *p;
        }
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ the disc */

typedef struct {
    FILE *f;
    int64_t base;   /* the game partition */
} Disc;

static bool rd_at(Disc *d, int64_t off, void *buf, size_t n)
{
#ifdef _WIN32
    if (_fseeki64(d->f, d->base + off, SEEK_SET) != 0) {
#else
    if (fseeko(d->f, d->base + off, SEEK_SET) != 0) {
#endif
        return false;
    }
    return fread(buf, 1, n, d->f) == n;
}

/* A name in a directory's tree (case-insensitive): its sector and size. */
static bool dir_find(Disc *d, uint32_t sec, uint32_t size, const char *name, uint32_t *fsec, uint32_t *fsize)
{
    if (!size || size > 4u << 20) {
        return false;
    }
    uint8_t *t = malloc(size);
    if (!t || !rd_at(d, (int64_t)sec * 2048, t, size)) {
        free(t);
        return false;
    }
    bool hit = false;
    uint32_t stack[64], sp = 0, depth = 0;
    stack[sp++] = 0;
    while (sp && depth++ < size / 14) {
        const uint32_t o = stack[--sp] * 4;
        if (o + 14 > size) {
            continue;
        }
        const uint16_t l = rd16(t + o), r = rd16(t + o + 2);
        if (l == 0xFFFF) {
            continue;
        }
        const uint8_t nl = t[o + 13];
        if (o + 14 + nl <= size && strlen(name) == nl && !SDL_strncasecmp((const char *)t + o + 14, name, nl)) {
            *fsec = rd32(t + o + 4);
            *fsize = rd32(t + o + 8);
            hit = true;
            break;
        }
        if (l && sp < 64) {
            stack[sp++] = l;
        }
        if (r && sp < 64) {
            stack[sp++] = r;
        }
    }
    free(t);
    return hit;
}

static uint8_t *xbe_section(const uint8_t *x, size_t n, const char *want, uint32_t *len)
{
    const uint32_t base = rd32(x + 0x104), nsec = rd32(x + 0x11C), sva = rd32(x + 0x120);
    for (uint32_t i = 0; i < nsec && i < 256; i++) {
        const size_t so = (size_t)(sva - base) + i * 0x38;
        if (so + 0x38 > n) {
            break;
        }
        const uint32_t name_va = rd32(x + so + 0x14), raw = rd32(x + so + 0x0C), rs = rd32(x + so + 0x10);
        const size_t no = name_va - base;
        if (no + strlen(want) + 1 <= n && !memcmp(x + no, want, strlen(want) + 1) && raw + (size_t)rs <= n) {
            *len = rs;
            return (uint8_t *)x + raw;
        }
    }
    return NULL;
}

bool xd_read_game(const char *path, XdGame *g)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->path, sizeof(g->path), "%s", path);
    xd_clean_name(path, g->title, sizeof(g->title));
    Disc d = {fopen(path, "rb"), 0};
    if (!d.f) {
        return false;
    }
#ifdef _WIN32
    _fseeki64(d.f, 0, SEEK_END);
    g->size = _ftelli64(d.f);
#else
    fseeko(d.f, 0, SEEK_END);
    g->size = ftello(d.f);
#endif
    static const int64_t k_bases[] = {0, 0x18300000, 0xFD90000, 0x2080000, 0x30600000};
    uint8_t vd[0x20];
    bool found = false;
    for (size_t i = 0; i < sizeof(k_bases) / sizeof(k_bases[0]) && !found; i++) {
        d.base = k_bases[i];
        found = rd_at(&d, 0x10000, vd, sizeof(vd)) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20);
    }
    uint32_t sec, size;
    uint8_t *x = NULL;
    bool ok = false;
    if (found && dir_find(&d, rd32(vd + 20), rd32(vd + 24), "default.xbe", &sec, &size) && size >= 0x200 &&
        size < 256u << 20) {
        /* the headers and certificate, then the title image section's bytes */
        x = malloc(size);
        if (x && rd_at(&d, (int64_t)sec * 2048, x, size) && !memcmp(x, "XBEH", 4)) {
            const uint32_t base = rd32(x + 0x104), cert = rd32(x + 0x118) - base;
            if (cert + 0xB0 <= size) {
                g->title_id = rd32(x + cert + 8);
                g->regions = rd32(x + cert + 0xA0);
                g->version = rd32(x + cert + 0xAC);
                char name[128];
                uint16_t u16[40];
                memcpy(u16, x + cert + 0x0C, sizeof(u16));
                xd_utf16_to_utf8(u16, 40, name, sizeof(name));
                if (name[0]) {
                    snprintf(g->title, sizeof(g->title), "%s", name);
                }
                ok = true;
            }
            uint32_t il = 0;
            uint8_t *img = xbe_section(x, size, "$$XTIMAGE", &il);
            if (!img) {
                img = xbe_section(x, size, "$$XSIMAGE", &il);
            }
            if (img) {
                g->icon = xd_xpr_decode(img, il, &g->icon_w, &g->icon_h);
            }
        }
    }
    free(x);
    fclose(d.f);
    snprintf(g->key, sizeof(g->key), "%08X.%u", g->title_id, g->version);
    return ok;
}

void xd_free_game(XdGame *g)
{
    free(g->icon);
    g->icon = NULL;
}

/* ------------------------------------------------------------ XPR0 */

static uint32_t c565(uint32_t v)
{
    const uint32_t r = v >> 11 & 31, gg = v >> 5 & 63, b = v & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((gg << 2 | gg >> 4) << 8) | (b << 3 | b >> 2);
}

static uint32_t c1555(uint32_t v)
{
    const uint32_t r = v >> 10 & 31, gg = v >> 5 & 31, b = v & 31;
    return (v & 0x8000 ? 0xFF000000u : 0) | ((r << 3 | r >> 2) << 16) | ((gg << 3 | gg >> 2) << 8) | (b << 3 | b >> 2);
}

static uint32_t c4444(uint32_t v)
{
    return ((v >> 12 & 15) * 17u) << 24 | ((v >> 8 & 15) * 17u) << 16 | ((v >> 4 & 15) * 17u) << 8 | (v & 15) * 17u;
}

static uint32_t cmix(uint32_t a, uint32_t b, int wa, int wb, int div)
{
    uint32_t o = 0xFF000000u;
    for (int s = 0; s <= 16; s += 8) {
        o |= (uint32_t)((wa * (int)(a >> s & 255) + wb * (int)(b >> s & 255)) / div) << s;
    }
    return o;
}

static uint32_t deposit(uint32_t v, uint32_t m)
{
    uint32_t r = 0;
    while (m) {
        const uint32_t low = m & -m;
        if (v & 1) {
            r |= low;
        }
        v >>= 1;
        m &= m - 1;
    }
    return r;
}

/* ARGB (0xAARRGGBB) to RGBA bytes */
static void put(uint8_t *o, uint32_t argb)
{
    o[0] = (uint8_t)(argb >> 16);
    o[1] = (uint8_t)(argb >> 8);
    o[2] = (uint8_t)argb;
    o[3] = (uint8_t)(argb >> 24);
}

uint8_t *xd_xpr_decode(const uint8_t *b, size_t n, int *pw, int *ph)
{
    if (n < 0x20 || memcmp(b, "XPR0", 4)) {
        return NULL;
    }
    const size_t at = (size_t)rd32(b + 8) + rd32(b + 0x10);
    const uint32_t format = rd32(b + 0x18), size = rd32(b + 0x1C);
    int w, h, pitch = 0;
    if (size) {
        w = (int)(size & 0xFFF) + 1;
        h = (int)(size >> 12 & 0xFFF) + 1;
        pitch = (int)((size >> 24 & 0xFF) + 1) * 64;
    } else {
        w = 1 << (format >> 20 & 0xF);
        h = 1 << (format >> 24 & 0xF);
    }
    if (w < 1 || w > 1024 || h < 1 || h > 1024 || at >= n) {
        return NULL;
    }
    const uint32_t fmt = format >> 8 & 0xFF;
    uint8_t *out = calloc((size_t)w * h, 4);
    if (!out) {
        return NULL;
    }
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        const int kind = fmt == 0x0C ? 1 : fmt == 0x0E ? 3 : 5, bb = kind == 1 ? 8 : 16;
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (at + (size_t)bw * bh * bb > n) {
            free(out);
            return NULL;
        }
        const uint8_t *p = b + at;
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int alpha[16];
                if (kind == 3) {
                    for (int i = 0; i < 16; i++) {
                        alpha[i] = (p[i / 2] >> (4 * (i & 1)) & 15) * 17;
                    }
                    p += 8;
                } else if (kind == 5) {
                    const int a0 = p[0], a1 = p[1];
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) {
                        bits |= (uint64_t)p[2 + i] << (8 * i);
                    }
                    for (int i = 0; i < 16; i++) {
                        const int c = (int)(bits >> (3 * i) & 7);
                        alpha[i] = c == 0   ? a0
                                   : c == 1 ? a1
                                   : a0 > a1 ? ((8 - c) * a0 + (c - 1) * a1) / 7
                                   : c == 6  ? 0
                                   : c == 7  ? 255
                                             : ((6 - c) * a0 + (c - 1) * a1) / 5;
                    }
                    p += 8;
                } else {
                    for (int i = 0; i < 16; i++) {
                        alpha[i] = 255;
                    }
                }
                const uint32_t c0 = rd16(p), c1 = rd16(p + 2), idx = rd32(p + 4);
                p += 8;
                uint32_t rgb[4] = {c565(c0), c565(c1), 0, 0};
                const bool four = kind != 1 || c0 > c1;
                rgb[2] = four ? cmix(rgb[0], rgb[1], 2, 1, 3) : cmix(rgb[0], rgb[1], 1, 1, 2);
                rgb[3] = four ? cmix(rgb[0], rgb[1], 1, 2, 3) : 0;
                for (int i = 0; i < 16; i++) {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h) {
                        continue;
                    }
                    const uint32_t code = idx >> (2 * i) & 3;
                    const uint32_t a = (!four && code == 3) ? 0 : (uint32_t)alpha[i];
                    put(out + ((size_t)y * w + x) * 4, a << 24 | (rgb[code] & 0xFFFFFF));
                }
            }
        }
    } else {
        int bpp, swz;
        uint32_t (*conv)(uint32_t) = NULL;
        uint32_t orv = 0;
        switch (fmt) {
        case 0x02: bpp = 2, swz = 1, conv = c1555; break;
        case 0x03: bpp = 2, swz = 1, conv = c1555, orv = 0x8000; break;
        case 0x04: bpp = 2, swz = 1, conv = c4444; break;
        case 0x05: bpp = 2, swz = 1, conv = c565; break;
        case 0x06: bpp = 4, swz = 1; break;
        case 0x07: bpp = 4, swz = 1, orv = 0xFF000000u; break;
        case 0x10: bpp = 2, swz = 0, conv = c1555; break;
        case 0x11: bpp = 2, swz = 0, conv = c565; break;
        case 0x12: bpp = 4, swz = 0; break;
        case 0x1C: bpp = 2, swz = 0, conv = c1555, orv = 0x8000; break;
        case 0x1D: bpp = 2, swz = 0, conv = c4444; break;
        case 0x1E: bpp = 4, swz = 0, orv = 0xFF000000u; break;
        default: free(out); return NULL;
        }
        const int stride = !swz && pitch > 0 ? pitch : w * bpp;
        if (at + (size_t)stride * (h - 1) + (size_t)w * bpp > n) {
            free(out);
            return NULL;
        }
        uint32_t xm = 0, ym = 0, bit = 1;
        for (int i = 1; i < w || i < h; i <<= 1) {
            if (i < w) {
                xm |= bit, bit <<= 1;
            }
            if (i < h) {
                ym |= bit, bit <<= 1;
            }
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const size_t off = swz ? at + (size_t)(deposit((uint32_t)y, ym) | deposit((uint32_t)x, xm)) * bpp
                                       : at + (size_t)y * stride + (size_t)x * bpp;
                uint32_t v = bpp == 4 ? rd32(b + off) : rd16(b + off);
                v = conv ? conv(v | orv) : v | orv;
                put(out + ((size_t)y * w + x) * 4, v);
            }
        }
    }
    *pw = w;
    *ph = h;
    return out;
}
