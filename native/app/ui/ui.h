/*
 * hakuX front end — the dashboard's drawing (ui.c): text in the bundled
 * font with the system's CJK fonts behind it, shapes, the console's green
 * theme, over an SDL_Renderer (Direct3D on Windows, OpenGL/Vulkan on Linux).
 */
#ifndef NATIVE_APP_UI_UI_H
#define NATIVE_APP_UI_UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint8_t r, g, b, a; } UiColor;

/* The theme: the original Xbox dashboard's greens on black. */
#define UI_GREEN      ((UiColor){0x8C, 0xF0, 0x2A, 0xFF})
#define UI_GREEN_HI   ((UiColor){0xD8, 0xFF, 0x7A, 0xFF})
#define UI_GREEN_DIM  ((UiColor){0x3E, 0x7A, 0x12, 0xFF})
#define UI_GREEN_DARK ((UiColor){0x0C, 0x24, 0x06, 0xFF})
#define UI_PANEL      ((UiColor){0x08, 0x16, 0x05, 0xD8})
#define UI_WHITE      ((UiColor){0xF2, 0xFF, 0xEA, 0xFF})
#define UI_GREY       ((UiColor){0x9A, 0xB0, 0x90, 0xFF})
#define UI_RED        ((UiColor){0xFF, 0x60, 0x50, 0xFF})
#define UI_BLACK      ((UiColor){0, 0, 0, 0xFF})

static inline UiColor ui_alpha(UiColor c, float a)
{
    c.a = (uint8_t)(c.a * (a < 0 ? 0 : a > 1 ? 1 : a));
    return c;
}
UiColor ui_mix(UiColor a, UiColor b, float t);

enum { UI_FONT_REGULAR, UI_FONT_BOLD, UI_FONTS };
enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_RIGHT = 2 };

/* Up and down: the renderer for win, the fonts from font_dir (Play) and the
 * system's CJK fonts. */
bool ui_init(SDL_Window *win, const char *font_dir);
void ui_shutdown(void);
SDL_Renderer *ui_renderer(void);
/* The frame: its size (pixels) and scale (1 at 720 lines: sizes are given
 * in 720p units and scaled), then drawing, then ui_end presents. */
void ui_begin(void);
void ui_end(void);
float ui_w(void);   /* in 720p units */
float ui_h(void);
float ui_scale(void);
double ui_time(void);  /* seconds since ui_init */

/* Text in 720p units: size is the font's pixel height. */
float ui_text(float x, float y, const char *utf8, float size, int font, UiColor c, int align);
float ui_text_width(const char *utf8, float size, int font);
/* Text cut to max_w with an ellipsis. */
float ui_text_fit(float x, float y, const char *utf8, float size, int font, UiColor c, int align, float max_w);

void ui_rect(float x, float y, float w, float h, UiColor c);
void ui_rect_grad(float x, float y, float w, float h, UiColor top, UiColor bottom);
void ui_rect_hgrad(float x, float y, float w, float h, UiColor left, UiColor right);
void ui_round(float x, float y, float w, float h, float r, UiColor c);
void ui_round_outline(float x, float y, float w, float h, float r, float t, UiColor c);
void ui_line(float x0, float y0, float x1, float y1, float t, UiColor c);
/* A soft radial glow (additive), radius r. */
void ui_glow(float cx, float cy, float r, UiColor c);
void ui_circle(float cx, float cy, float r, UiColor c);
void ui_ring(float cx, float cy, float rx, float ry, float rot, float t, UiColor c);
/* An image (texture) fitted into the box, keeping its shape. */
void ui_image(SDL_Texture *t, float x, float y, float w, float h, float alpha);
SDL_Texture *ui_texture_rgba(const uint8_t *rgba, int w, int h);
void ui_clip(float x, float y, float w, float h);
void ui_unclip(void);

/* Layers: drawing into a transparent texture the window's size (a screen's
 * foreground), then that texture shown turned about the vertical axis
 * (angle, radians: 0 flat), pushed back (depth, in widths) and faded. */
SDL_Texture *ui_layer_begin(SDL_Texture *layer);
void ui_layer_end(void);
void ui_layer_draw3d(SDL_Texture *layer, float angle, float depth, float alpha);

/* The console's animated backdrop: a green fog and drifting light. */
void ui_backdrop(float intensity);
/* The dashboard's orb (its centre and size). */
void ui_orb(float cx, float cy, float r, float pulse);

#endif
