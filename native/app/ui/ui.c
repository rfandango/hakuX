/*
 * hakuX front end — the dashboard's drawing (ui.h).
 *
 * Text: glyphs rasterized by stb_truetype at the size they are drawn at
 * (pixels: crisp at any window size) into one atlas texture, shelf-packed,
 * looked up in an open-addressed table. A codepoint the bundled font lacks
 * (CJK) comes from the first system font that has it.
 */
#include "native/app/ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define ATLAS 2048
#define GLYPHS 8192         /* table slots (a power of two) */
#define MAX_FACES 8

typedef struct {
    stbtt_fontinfo info;
    unsigned char *data;
    bool ok;
} Face;

typedef struct {
    uint32_t key;           /* codepoint | style << 21 | px << 23 (0: empty) */
    uint16_t x, y, w, h;
    int16_t xoff, yoff;
    float adv;
} Glyph;

static struct {
    SDL_Window *win;
    SDL_Renderer *r;
    SDL_Texture *atlas, *glow;
    Face face[MAX_FACES];   /* 0 regular, 1 bold, 2.. fallbacks */
    int nfaces;
    Glyph g[GLYPHS];
    int shelf_x, shelf_y, shelf_h;
    float scale;            /* pixels per 720p unit */
    int pw, ph;
    uint64_t t0;
} U;

UiColor ui_mix(UiColor a, UiColor b, float t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return (UiColor){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                     (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t)};
}

static SDL_FColor fc(UiColor c)
{
    return (SDL_FColor){c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}

static unsigned char *read_file(const char *path)
{
    size_t n = 0;
    return SDL_LoadFile(path, &n);
}

static bool face_load(Face *f, const char *path, int index)
{
    f->data = read_file(path);
    if (!f->data) {
        return false;
    }
    const int off = stbtt_GetFontOffsetForIndex(f->data, index);
    f->ok = off >= 0 && stbtt_InitFont(&f->info, f->data, off);
    if (!f->ok) {
        SDL_free(f->data);
        f->data = NULL;
    }
    return f->ok;
}

static void fallback_fonts(void)
{
    static const char *const k_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\malgun.ttf",
        "C:\\Windows\\Fonts\\msgothic.ttc", "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]) && U.nfaces < MAX_FACES; i++) {
        if (face_load(&U.face[U.nfaces], k_paths[i], 0)) {
            U.nfaces++;
        }
    }
}

static SDL_Texture *make_glow(void)
{
    enum { N = 128 };
    uint8_t *px = SDL_malloc(N * N * 4);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
            float d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1 ? 0 : (1 - d) * (1 - d);
            a = a * a * 0.6f + a * 0.4f;
            uint8_t *p = px + (y * N + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255);
        }
    }
    SDL_Texture *t = ui_texture_rgba(px, N, N);
    SDL_free(px);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
    return t;
}

bool ui_init(SDL_Window *win, const char *font_dir)
{
    memset(&U, 0, sizeof(U));
    U.win = win;
    U.r = SDL_CreateRenderer(win, NULL);
    if (!U.r) {
        U.r = SDL_CreateRenderer(win, "vulkan");   /* a Vulkan window (the game's, on Linux) */
    }
    if (!U.r) {
        SDL_Log("ui: no renderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(U.r, 1);
    /* shapes (untextured geometry) blend with what is under them */
    SDL_SetRenderDrawBlendMode(U.r, SDL_BLENDMODE_BLEND);
    char p[1024];
    snprintf(p, sizeof(p), "%sPlay-Regular.ttf", font_dir);
    if (!face_load(&U.face[0], p, 0)) {
        SDL_Log("ui: no font at %s", p);
        return false;
    }
    snprintf(p, sizeof(p), "%sPlay-Bold.ttf", font_dir);
    if (!face_load(&U.face[1], p, 0)) {
        U.face[1] = U.face[0];
    }
    U.nfaces = 2;
    fallback_fonts();
    U.atlas = SDL_CreateTexture(U.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    SDL_SetTextureBlendMode(U.atlas, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(U.atlas, SDL_SCALEMODE_LINEAR);
    U.glow = make_glow();
    U.t0 = SDL_GetTicksNS();
    return true;
}

void ui_shutdown(void)
{
    if (U.atlas) {
        SDL_DestroyTexture(U.atlas);
    }
    if (U.glow) {
        SDL_DestroyTexture(U.glow);
    }
    if (U.r) {
        SDL_DestroyRenderer(U.r);
    }
    for (int i = 0; i < U.nfaces; i++) {
        if (U.face[i].data && !(i == 1 && U.face[1].data == U.face[0].data)) {
            SDL_free(U.face[i].data);
        }
    }
    memset(&U, 0, sizeof(U));
}

SDL_Renderer *ui_renderer(void)
{
    return U.r;
}

void ui_begin(void)
{
    SDL_GetCurrentRenderOutputSize(U.r, &U.pw, &U.ph);
    U.scale = U.ph / 720.0f;
    if (U.pw / U.scale < 960) {
        U.scale = U.pw / 960.0f;   /* narrow windows: at least 960 units wide */
    }
    SDL_SetRenderDrawColor(U.r, 0, 0, 0, 255);
    SDL_RenderClear(U.r);
}

void ui_end(void)
{
    SDL_RenderPresent(U.r);
}

float ui_w(void)
{
    return U.pw / U.scale;
}

float ui_h(void)
{
    return U.ph / U.scale;
}

float ui_scale(void)
{
    return U.scale;
}

double ui_time(void)
{
    return (SDL_GetTicksNS() - U.t0) / 1e9;
}

/* ------------------------------------------------------------ text */

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static void atlas_reset(void)
{
    memset(U.g, 0, sizeof(U.g));
    U.shelf_x = U.shelf_y = U.shelf_h = 0;
}

static const Glyph *glyph(uint32_t cp, int style, int px)
{
    if (px > 255) {
        px = 255;
    }
    const uint32_t key = (cp & 0x1FFFFF) | (uint32_t)(style & 3) << 21 | (uint32_t)px << 23;
    uint32_t h = key * 2654435761u;
    for (int probe = 0; probe < GLYPHS; probe++) {
        Glyph *g = &U.g[(h + probe) & (GLYPHS - 1)];
        if (g->key == key) {
            return g;
        }
        if (g->key) {
            continue;
        }
        /* not cached: the first face with the codepoint */
        Face *f = &U.face[style == UI_FONT_BOLD ? 1 : 0];
        int gi = stbtt_FindGlyphIndex(&f->info, cp);
        for (int i = 2; !gi && i < U.nfaces; i++) {
            if ((gi = stbtt_FindGlyphIndex(&U.face[i].info, cp))) {
                f = &U.face[i];
            }
        }
        const float sc = stbtt_ScaleForPixelHeight(&f->info, (float)px);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&f->info, gi, sc, sc, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, hh = y1 - y0;
        if (U.shelf_x + w + 1 > ATLAS) {
            U.shelf_x = 0;
            U.shelf_y += U.shelf_h + 1;
            U.shelf_h = 0;
        }
        if (U.shelf_y + hh + 1 > ATLAS) {
            atlas_reset();      /* full: start over (glyphs redraw as needed) */
            return glyph(cp, style, px);
        }
        g->key = key;
        g->x = (uint16_t)U.shelf_x;
        g->y = (uint16_t)U.shelf_y;
        g->w = (uint16_t)w;
        g->h = (uint16_t)hh;
        g->xoff = (int16_t)x0;
        g->yoff = (int16_t)y0;
        g->adv = adv * sc;
        /* the face's ascent: baselines line up across faces */
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
        g->yoff = (int16_t)(y0 + (int)lrintf(asc * sc));
        if (w > 0 && hh > 0) {
            unsigned char *a = SDL_malloc((size_t)w * hh);
            stbtt_MakeGlyphBitmap(&f->info, a, w, hh, w, sc, sc, gi);
            uint8_t *rgba = SDL_malloc((size_t)w * hh * 4);
            for (int i = 0; i < w * hh; i++) {
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = a[i];
            }
            const SDL_Rect rc = {U.shelf_x, U.shelf_y, w, hh};
            SDL_UpdateTexture(U.atlas, &rc, rgba, w * 4);
            SDL_free(a);
            SDL_free(rgba);
        }
        U.shelf_x += w + 1;
        if (hh > U.shelf_h) {
            U.shelf_h = hh;
        }
        return g;
    }
    atlas_reset();
    return glyph(cp, style, px);
}

static float text_run(float x, float y, const char *s, float size, int font, UiColor c, bool draw, float max_w,
                      const char **stop)
{
    const int px = (int)lrintf(size * U.scale);
    float pen = x * U.scale;
    const float start = pen;
    SDL_SetTextureColorMod(U.atlas, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(U.atlas, c.a);
    while (*s) {
        const char *at = s;
        const uint32_t cp = utf8_next(&s);
        if (cp == '\n') {
            break;
        }
        const Glyph *g = glyph(cp, font, px);
        if (max_w > 0 && (pen + g->adv - start) / U.scale > max_w) {
            if (stop) {
                *stop = at;
            }
            return (pen - start) / U.scale;
        }
        if (draw && g->w) {
            const SDL_FRect src = {g->x, g->y, g->w, g->h};
            const SDL_FRect dst = {floorf(pen) + g->xoff, floorf(y * U.scale) + g->yoff, g->w, g->h};
            SDL_RenderTexture(U.r, U.atlas, &src, &dst);
        }
        pen += g->adv;
    }
    if (stop) {
        *stop = NULL;
    }
    return (pen - start) / U.scale;
}

float ui_text_width(const char *s, float size, int font)
{
    return text_run(0, 0, s, size, font, UI_WHITE, false, 0, NULL);
}

float ui_text(float x, float y, const char *s, float size, int font, UiColor c, int align)
{
    if (align != UI_ALIGN_LEFT) {
        const float w = ui_text_width(s, size, font);
        x -= align == UI_ALIGN_CENTER ? w / 2 : w;
    }
    return text_run(x, y, s, size, font, c, true, 0, NULL);
}

float ui_text_fit(float x, float y, const char *s, float size, int font, UiColor c, int align, float max_w)
{
    const float w = ui_text_width(s, size, font);
    if (w <= max_w) {
        return ui_text(x, y, s, size, font, c, align);
    }
    const float ell = ui_text_width("...", size, font);
    const char *stop = NULL;
    char buf[512];
    text_run(0, 0, s, size, font, c, false, max_w - ell, &stop);
    const size_t n = stop ? (size_t)(stop - s) : strlen(s);
    snprintf(buf, sizeof(buf), "%.*s...", (int)(n < sizeof(buf) - 4 ? n : sizeof(buf) - 4), s);
    return ui_text(x, y, buf, size, font, c, align == UI_ALIGN_CENTER ? UI_ALIGN_CENTER : align);
}

/* ------------------------------------------------------------ shapes */

static void quad(float x, float y, float w, float h, UiColor c0, UiColor c1, UiColor c2, UiColor c3)
{
    const float s = U.scale;
    SDL_Vertex v[4] = {
        {{x * s, y * s}, fc(c0), {0, 0}},
        {{(x + w) * s, y * s}, fc(c1), {0, 0}},
        {{(x + w) * s, (y + h) * s}, fc(c2), {0, 0}},
        {{x * s, (y + h) * s}, fc(c3), {0, 0}},
    };
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(U.r, NULL, v, 4, idx, 6);
}

void ui_rect(float x, float y, float w, float h, UiColor c)
{
    quad(x, y, w, h, c, c, c, c);
}

void ui_rect_grad(float x, float y, float w, float h, UiColor top, UiColor bottom)
{
    quad(x, y, w, h, top, top, bottom, bottom);
}

void ui_rect_hgrad(float x, float y, float w, float h, UiColor left, UiColor right)
{
    quad(x, y, w, h, left, right, right, left);
}

#define ARC 8   /* segments a corner */

static int round_path(float x, float y, float w, float h, float r, SDL_FPoint *pt)
{
    r = fminf(r, fminf(w, h) / 2);
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    int n = 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= ARC; i++) {
            const float a = (float)(-M_PI / 2 + (k + (float)i / ARC) * M_PI / 2);
            pt[n++] = (SDL_FPoint){cx[k] + cosf(a) * r, cy[k] + sinf(a) * r};
        }
    }
    return n;
}

void ui_round(float x, float y, float w, float h, float r, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    const int n = round_path(x, y, w, h, r, pt);
    SDL_Vertex v[4 * (ARC + 1) + 1];
    int idx[3 * 4 * (ARC + 1)];
    const SDL_FColor col = fc(c);
    v[0] = (SDL_Vertex){{(x + w / 2) * U.scale, (y + h / 2) * U.scale}, col, {0, 0}};
    for (int i = 0; i < n; i++) {
        v[i + 1] = (SDL_Vertex){{pt[i].x * U.scale, pt[i].y * U.scale}, col, {0, 0}};
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(U.r, NULL, v, n + 1, idx, n * 3);
}

static void strip(const SDL_FPoint *pt, int n, float t, UiColor c, bool closed)
{
    SDL_Vertex v[2 * 4 * (ARC + 1) + 4];
    int idx[6 * 4 * (ARC + 1) + 6];
    if (n > 4 * (ARC + 1)) {
        n = 4 * (ARC + 1);
    }
    const SDL_FColor col = fc(c);
    for (int i = 0; i < n; i++) {
        const SDL_FPoint a = pt[(i + n - 1) % n], b = pt[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        const float l = sqrtf(dx * dx + dy * dy);
        dx = l > 0 ? dx / l : 1;
        dy = l > 0 ? dy / l : 0;
        const float nx = -dy * t / 2, ny = dx * t / 2;
        v[i * 2] = (SDL_Vertex){{(pt[i].x + nx) * U.scale, (pt[i].y + ny) * U.scale}, col, {0, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{(pt[i].x - nx) * U.scale, (pt[i].y - ny) * U.scale}, col, {0, 0}};
    }
    int k = 0;
    for (int i = 0; i < (closed ? n : n - 1); i++) {
        const int a = i * 2, b = ((i + 1) % n) * 2;
        idx[k++] = a;
        idx[k++] = a + 1;
        idx[k++] = b;
        idx[k++] = b;
        idx[k++] = a + 1;
        idx[k++] = b + 1;
    }
    SDL_RenderGeometry(U.r, NULL, v, n * 2, idx, k);
}

void ui_round_outline(float x, float y, float w, float h, float r, float t, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    strip(pt, round_path(x, y, w, h, r, pt), t, c, true);
}

void ui_line(float x0, float y0, float x1, float y1, float t, UiColor c)
{
    float dx = x1 - x0, dy = y1 - y0;
    const float l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) {
        return;
    }
    const float nx = -dy / l * t / 2, ny = dx / l * t / 2, s = U.scale;
    const SDL_FColor col = fc(c);
    SDL_Vertex v[4] = {{{(x0 + nx) * s, (y0 + ny) * s}, col, {0, 0}},
                       {{(x1 + nx) * s, (y1 + ny) * s}, col, {0, 0}},
                       {{(x1 - nx) * s, (y1 - ny) * s}, col, {0, 0}},
                       {{(x0 - nx) * s, (y0 - ny) * s}, col, {0, 0}}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(U.r, NULL, v, 4, idx, 6);
}

void ui_glow(float cx, float cy, float r, UiColor c)
{
    SDL_SetTextureColorMod(U.glow, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(U.glow, c.a);
    const SDL_FRect d = {(cx - r) * U.scale, (cy - r) * U.scale, 2 * r * U.scale, 2 * r * U.scale};
    SDL_RenderTexture(U.r, U.glow, NULL, &d);
}

void ui_circle(float cx, float cy, float r, UiColor c)
{
    ui_round(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void ui_ring(float cx, float cy, float rx, float ry, float rot, float t, UiColor c)
{
    SDL_FPoint pt[4 * (ARC + 1)];
    const int n = 4 * (ARC + 1);
    const float cr = cosf(rot), sr = sinf(rot);
    for (int i = 0; i < n; i++) {
        const float a = (float)(2 * M_PI * i / n), ex = cosf(a) * rx, ey = sinf(a) * ry;
        pt[i] = (SDL_FPoint){cx + ex * cr - ey * sr, cy + ex * sr + ey * cr};
    }
    strip(pt, n, t, c, true);
}

SDL_Texture *ui_texture_rgba(const uint8_t *rgba, int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(U.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, NULL, rgba, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

void ui_image(SDL_Texture *t, float x, float y, float w, float h, float alpha)
{
    if (!t) {
        return;
    }
    float tw, th;
    SDL_GetTextureSize(t, &tw, &th);
    const float k = fminf(w / tw, h / th), dw = tw * k, dh = th * k;
    const SDL_FRect d = {(x + (w - dw) / 2) * U.scale, (y + (h - dh) / 2) * U.scale, dw * U.scale, dh * U.scale};
    SDL_SetTextureAlphaModFloat(t, alpha);
    SDL_RenderTexture(U.r, t, NULL, &d);
}

void ui_clip(float x, float y, float w, float h)
{
    const SDL_Rect r = {(int)(x * U.scale), (int)(y * U.scale), (int)(w * U.scale), (int)(h * U.scale)};
    SDL_SetRenderClipRect(U.r, &r);
}

void ui_unclip(void)
{
    SDL_SetRenderClipRect(U.r, NULL);
}

/* ------------------------------------------------------------ layers */

SDL_Texture *ui_layer_begin(SDL_Texture *layer)
{
    float tw = 0, th = 0;
    if (layer) {
        SDL_GetTextureSize(layer, &tw, &th);
    }
    if (!layer || (int)tw != U.pw || (int)th != U.ph) {
        if (layer) {
            SDL_DestroyTexture(layer);
        }
        layer = SDL_CreateTexture(U.r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, U.pw, U.ph);
        /* drawn over a transparent target with plain blending: its colour is
         * premultiplied (and additive glows keep theirs where alpha is 0) */
        SDL_SetTextureBlendMode(layer, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(layer, SDL_SCALEMODE_LINEAR);
    }
    SDL_SetRenderTarget(U.r, layer);
    SDL_SetRenderDrawColor(U.r, 0, 0, 0, 0);
    SDL_RenderClear(U.r);
    return layer;
}

void ui_layer_end(void)
{
    SDL_SetRenderTarget(U.r, NULL);
}

void ui_layer_draw3d(SDL_Texture *layer, float angle, float depth, float alpha)
{
    if (!layer || alpha <= 0.003f) {
        return;
    }
    enum { COLS = 24 };
    const float W = (float)U.pw, H = (float)U.ph, f = W * 1.25f, cx = W / 2, cy = H / 2;
    const float ca = cosf(angle), sa = sinf(angle);
    SDL_Vertex v[(COLS + 1) * 2];
    int idx[COLS * 6];
    const SDL_FColor col = {alpha, alpha, alpha, alpha};   /* premultiplied */
    for (int i = 0; i <= COLS; i++) {
        const float u = (float)i / COLS, X = (u - 0.5f) * W;
        const float z = X * sa + depth * W, x = X * ca;
        const float k = f / (f + z);
        v[i * 2] = (SDL_Vertex){{cx + x * k, cy - H / 2 * k}, col, {u, 0}};
        v[i * 2 + 1] = (SDL_Vertex){{cx + x * k, cy + H / 2 * k}, col, {u, 1}};
    }
    for (int i = 0; i < COLS; i++) {
        const int a = i * 2;
        idx[i * 6] = a;
        idx[i * 6 + 1] = a + 2;
        idx[i * 6 + 2] = a + 1;
        idx[i * 6 + 3] = a + 1;
        idx[i * 6 + 4] = a + 2;
        idx[i * 6 + 5] = a + 3;
    }
    SDL_RenderGeometry(U.r, layer, v, (COLS + 1) * 2, idx, COLS * 6);
}

/* ------------------------------------------------------------ the console's look */

void ui_backdrop(float k)
{
    const float W = ui_w(), H = ui_h();
    const double t = ui_time();
    ui_rect_grad(0, 0, W, H, (UiColor){0x02, 0x0A, 0x02, 0xFF}, (UiColor){0x04, 0x16, 0x03, 0xFF});
    /* the fog: slow green lights drifting */
    for (int i = 0; i < 6; i++) {
        const float ph = (float)(t * (0.05 + i * 0.013) + i * 1.7);
        const float x = W * (0.5f + 0.42f * sinf(ph * 1.3f + i)), y = H * (0.5f + 0.38f * cosf(ph + i * 2.1f));
        ui_glow(x, y, H * (0.55f + 0.15f * sinf(ph * 2)), ui_alpha((UiColor){0x3C, 0xA0, 0x10, 0xFF}, 0.22f * k));
    }
    /* the linked hexagons, drifting and pulsing (the original dashboard's backdrop) */
    {
        const float hs = 46, hx = hs * 1.5f, hy = hs * 0.866f * 2;
        const float ox = fmodf((float)(t * 6), hx * 2), oy = fmodf((float)(t * 3), hy);
        for (int gx = -1; gx * hx < W + hx; gx++) {
            for (int gy = -1; gy * hy < H * 0.7f + hy; gy++) {
                const float x = gx * hx - ox, y = gy * hy + ((gx & 1) ? hy / 2 : 0) - oy;
                const float wave = 0.5f + 0.5f * sinf((float)t * 1.3f + gx * 0.7f + gy * 1.1f);
                const float a = 0.13f * k * wave * (1 - y / (H * 0.75f));
                if (a <= 0.004f) {
                    continue;
                }
                for (int s = 0; s < 6; s++) {
                    const float a0 = (float)(s * M_PI / 3), a1 = (float)((s + 1) * M_PI / 3);
                    ui_line(x + cosf(a0) * hs * 0.5f, y + sinf(a0) * hs * 0.5f, x + cosf(a1) * hs * 0.5f,
                            y + sinf(a1) * hs * 0.5f, 1.2f, ui_alpha(UI_GREEN, a));
                }
            }
        }
    }
    /* the floor's grid, receding */
    const UiColor gl = ui_alpha(UI_GREEN, 0.10f * k);
    const float horizon = H * 0.62f;
    for (int i = 0; i <= 12; i++) {
        const float z = fmodf((float)(i + fmod(t * 0.35, 1.0)), 12.f) / 12.f;
        const float y = horizon + (H - horizon) * z * z;
        ui_line(0, y, W, y, 1.2f, ui_alpha(gl, z));
    }
    for (int i = -12; i <= 12; i++) {
        ui_line(W / 2 + i * W * 0.02f, horizon, W / 2 + i * W * 0.16f, H, 1.0f, gl);
    }
    /* specks rising */
    for (int i = 0; i < 40; i++) {
        const float sx = fmodf(i * 97.31f, 1.0f) * W;
        const float sp = 0.02f + fmodf(i * 0.618f, 1.0f) * 0.05f;
        const float sy = H - fmodf((float)(t * sp * H + i * 53.7), H + 20);
        ui_glow(sx + 6 * sinf((float)t + i), sy, 3 + (i % 3), ui_alpha(UI_GREEN_HI, 0.5f * k));
    }
    /* a vignette */
    ui_rect_grad(0, 0, W, H * 0.25f, (UiColor){0, 0, 0, 160}, (UiColor){0, 0, 0, 0});
    ui_rect_grad(0, H * 0.75f, W, H * 0.25f, (UiColor){0, 0, 0, 0}, (UiColor){0, 0, 0, 200});
}

void ui_orb(float cx, float cy, float r, float pulse)
{
    const double t = ui_time();
    const float p = 0.85f + 0.15f * sinf((float)t * 2.2f) + pulse * 0.3f;
    ui_glow(cx, cy, r * 3.2f, ui_alpha((UiColor){0x50, 0xD0, 0x10, 0xFF}, 0.35f * p));
    ui_glow(cx, cy, r * 1.9f, ui_alpha((UiColor){0x80, 0xFF, 0x30, 0xFF}, 0.45f * p));
    ui_circle(cx, cy, r, (UiColor){0x1A, 0x50, 0x06, 0xFF});
    ui_glow(cx - r * 0.25f, cy - r * 0.3f, r * 0.9f, ui_alpha(UI_GREEN_HI, 0.55f));
    ui_glow(cx, cy, r * 1.05f, ui_alpha(UI_GREEN, 0.5f * p));
    for (int i = 0; i < 3; i++) {
        const float rot = (float)(t * (0.4 + i * 0.25) + i * 1.1);
        ui_ring(cx, cy, r * (1.35f + i * 0.22f), r * (0.42f + i * 0.08f), rot, 2.0f,
                ui_alpha(UI_GREEN_HI, 0.55f - i * 0.12f));
    }
}
