/*
 * xemu SDL display driver
 *
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Based on sdl2.c, sdl2-gl.c
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu-version.h"
#include "qemu-main.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qobject/qdict.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/xemu-display.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "xui/xemu-hud.h"
#include "xemu-input.h"
#include "xemu-settings.h"
// #include "xemu-shaders.h"
#include "xemu-snapshots.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"

#include "data/xemu_64x64.png.h"

#include "hw/xbox/smbus.h" // For eject, drive tray
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/debug.h"
#include "ui/xemu-notifications.h"

#include <stb_image.h>
#include <locale.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifdef _WIN32
#include "nvapi.h"
// Provide hint to prefer high-performance graphics for hybrid systems
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

void tcg_register_init_ctx(void); // tcg.c

// #define DEBUG_XEMU_C

#ifdef DEBUG_XEMU_C
#define DPRINTF(...) fprintf(stderr, __VA_ARGS__)
#else
#define DPRINTF(...)
#endif

static bool xb_console_gl_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

void xb_surface_gl_create_texture(DisplaySurface *surface);
void xb_surface_gl_update_texture(DisplaySurface *surface, int x, int y, int w, int h);
void xb_surface_gl_destroy_texture(DisplaySurface *surface);

static void sleep_ns(int64_t ns);

static int sdl2_num_outputs;
static struct sdl2_console *sdl2_console;
static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;
static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = KMOD_LALT | KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;
static SDL_Window *m_window;
static SDL_GLContext m_context;
static SDL_threadID sdl_render_thread_id;
// struct decal_shader *blit;

static QemuSemaphore display_init_sem;

static void toggle_full_screen(struct sdl2_console *scon);

#ifdef __ANDROID__
int bdrv_flush_all(void);
static bool g_android_gl_bgra_supported = true;
static bool g_android_use_hud = false;
static bool g_android_paused = false;
static bool g_android_should_quit = false;
static uint64_t g_android_frame_counter = 0;
static GLuint g_android_blit_prog;
static GLuint g_android_blit_vao;
static GLuint g_android_blit_vbo;
static GLint g_android_blit_tex_loc = -1;
static GLint g_android_blit_flip_loc = -1;
static GLuint g_android_cpu_tex = 0;
static int g_android_cpu_tex_w = 0;
static int g_android_cpu_tex_h = 0;
static uint8_t *g_android_cpu_buf = NULL;
static size_t g_android_cpu_buf_size = 0;

static bool sdl2_is_render_thread(void)
{
    return sdl_render_thread_id != 0 && SDL_ThreadID() == sdl_render_thread_id;
}

static bool sdl2_gl_has_extension(const char *ext_list, const char *ext)
{
    if (!ext_list || !ext || !ext[0]) {
        return false;
    }
    const size_t len = strlen(ext);
    const char *pos = ext_list;
    while ((pos = strstr(pos, ext)) != NULL) {
        const char *end = pos + len;
        if ((pos == ext_list || pos[-1] == ' ') &&
            (*end == '\0' || *end == ' ')) {
            return true;
        }
        pos = end;
    }
    return false;
}

static void android_log_gl_error(const char *stage)
{
    GLenum err;
    bool logged = false;
    while ((err = glGetError()) != GL_NO_ERROR) {
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "GL error at %s: 0x%x", stage, err);
        logged = true;
    }
    (void)logged;
}

static GLuint android_gl_compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    if (!shader) {
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "android_gl_compile_shader: glCreateShader failed");
        return 0;
    }
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024] = {0};
        GLsizei len = 0;
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log) - 1, &len, log);
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool android_blit_init(void)
{
    if (g_android_blit_prog) {
        return true;
    }
    const char *vs = "#version 300 es\n"
                     "precision highp float;\n"
                     "layout(location=0) in vec2 a_pos;\n"
                     "layout(location=1) in vec2 a_uv;\n"
                     "uniform bool u_flip;\n"
                     "out vec2 v_uv;\n"
                     "void main() {\n"
                     "  v_uv = a_uv;\n"
                     "  if (u_flip) { v_uv.y = 1.0 - v_uv.y; }\n"
                     "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                     "}\n";
    const char *fs = "#version 300 es\n"
                     "precision highp float;\n"
                     "in vec2 v_uv;\n"
                     "uniform sampler2D u_tex;\n"
                     "out vec4 out_Color;\n"
                     "void main() {\n"
                     "  vec4 c = texture(u_tex, v_uv);\n"
                     "  out_Color = vec4(c.rgb, 1.0);\n"
                     "}\n";

    GLuint vshader = android_gl_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint fshader = android_gl_compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vshader || !fshader) {
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vshader);
    glAttachShader(prog, fshader);
    glLinkProgram(prog);
    glDeleteShader(vshader);
    glDeleteShader(fshader);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {0};
        GLsizei len = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log) - 1, &len, log);
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "shader link failed: %s", log);
        glDeleteProgram(prog);
        return false;
    }

    g_android_blit_prog = prog;
    g_android_blit_tex_loc = glGetUniformLocation(prog, "u_tex");
    g_android_blit_flip_loc = glGetUniformLocation(prog, "u_flip");

    const float verts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenVertexArrays(1, &g_android_blit_vao);
    glBindVertexArray(g_android_blit_vao);
    glGenBuffers(1, &g_android_blit_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_android_blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    return true;
}

static void android_blit_frame(GLuint tex, bool flip)
{
    if (!android_blit_init()) {
        return;
    }
    android_log_gl_error("blit-start");
    int w = 0;
    int h = 0;
    SDL_GL_GetDrawableSize(m_window, &w, &h);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_android_blit_prog);
    glUniform1i(g_android_blit_tex_loc, 0);
    glUniform1i(g_android_blit_flip_loc, flip ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(g_android_blit_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    android_log_gl_error("blit-draw");
}
#endif

int xemu_is_fullscreen(void)
{
    return gui_fullscreen;
}

void xemu_toggle_fullscreen(void)
{
    toggle_full_screen(&sdl2_console[0]);
}

#define SDL2_REFRESH_INTERVAL_BUSY 16
#define SDL2_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT \
                             / SDL2_REFRESH_INTERVAL_BUSY + 1)

static struct sdl2_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < sdl2_num_outputs; i++) {
        if (sdl2_console[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &sdl2_console[i];
        }
    }
    return NULL;
}

void sdl2_window_resize(struct sdl2_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void sdl2_redraw(struct sdl2_console *scon)
{
    if (scon->opengl) {
        sdl2_gl_redraw(scon);
    }
}

static void sdl_update_caption(struct sdl2_console *scon)
{
}

static void sdl_hide_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    }
}

static void sdl_show_cursor(struct sdl2_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor(SDL_ENABLE);
}

static void sdl_grab_start(struct sdl2_console *scon)
{
}

static void sdl_grab_end(struct sdl2_console *scon)
{
    SDL_SetWindowGrab(scon->real_window, SDL_FALSE);
    gui_grab = 0;
    sdl_show_cursor(scon);
    sdl_update_caption(scon);
}

static void absolute_mouse_grab(struct sdl2_console *scon)
{
    int mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        sdl_grab_start(scon);
    }
}

static void sdl_mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(sdl2_console[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            absolute_mouse_grab(&sdl2_console[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            sdl_grab_end(&sdl2_console[0]);
        }
        absolute_enabled = 0;
    }
}

static void sdl_send_mouse_event(struct sdl2_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON(SDL_BUTTON_RIGHT),
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void set_full_screen(struct sdl2_console *scon, bool set)
{
    gui_fullscreen = set;

    if (gui_fullscreen) {
        SDL_SetWindowFullscreen(scon->real_window,
                                (g_config.display.window.fullscreen_exclusive ?
                                SDL_WINDOW_FULLSCREEN :
                                SDL_WINDOW_FULLSCREEN_DESKTOP));
        gui_saved_grab = gui_grab;
        sdl_grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            sdl_grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, 0);
    }
}

static void toggle_full_screen(struct sdl2_console *scon)
{
    set_full_screen(scon, !gui_fullscreen);
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | KMOD_LSHIFT)) ==
            (gui_grab_code | KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & KMOD_RCTRL) == KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);
    if (scon == NULL) return; 
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.keysym.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                sdl_grab_end(scon);
            }

            win = ev->key.keysym.scancode - SDL_SCANCODE_1;
            if (win < sdl2_num_outputs) {
                sdl2_console[win].hidden = !sdl2_console[win].hidden;
                if (sdl2_console[win].real_window) {
                    if (sdl2_console[win].hidden) {
                        SDL_HideWindow(sdl2_console[win].real_window);
                    } else {
                        SDL_ShowWindow(sdl2_console[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                sdl_grab_start(scon);
            } else if (!gui_fullscreen) {
                sdl_grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            sdl2_window_resize(scon);
            gui_keysym = 1;
            break;
        default:
            break;
        }
    }
    if (!gui_keysym) {
        sdl2_process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->key.windowID);
    if (!scon) return;

    scon->ignore_hotkeys = false;
    sdl2_process_key(scon, &ev->key);
}

static void handle_textinput(SDL_Event *ev)
{
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct sdl2_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            sdl_grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            sdl_grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        sdl_send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct sdl2_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_MOUSEBUTTONUP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            sdl_grab_start(scon);
        }
    } else {
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            buttonstate |= SDL_BUTTON(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON(bev->button);
        }
        sdl_send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct sdl2_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->window.event) {
    case SDL_WINDOWEVENT_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);

            if (!gui_fullscreen) {
                g_config.display.window.last_width = ev->window.data1;
                g_config.display.window.last_height = ev->window.data2;
            }
        }
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_EXPOSED:
        sdl2_redraw(scon);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
    case SDL_WINDOWEVENT_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen) {
            sdl_grab_end(scon);
        }
        break;
    case SDL_WINDOWEVENT_RESTORED:
#ifdef __ANDROID__
        g_android_paused = false;
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "android: window restored");
#endif
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
#ifdef __ANDROID__
        g_android_paused = true;
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "android: window minimized");
#endif
        break;
    case SDL_WINDOWEVENT_CLOSE:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
#ifdef __ANDROID__
                g_android_should_quit = true;
                __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                    "android: window close requested");
#endif
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_WINDOWEVENT_SHOWN:
        scon->hidden = false;
#ifdef __ANDROID__
        g_android_paused = false;
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "android: window shown");
#endif
        break;
    case SDL_WINDOWEVENT_HIDDEN:
        scon->hidden = true;
#ifdef __ANDROID__
        g_android_paused = true;
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "android: window hidden");
#endif
        break;
    }
}

void sdl2_poll_events(struct sdl2_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;

    if (scon->last_vm_running != runstate_is_running()) {
        scon->last_vm_running = runstate_is_running();
        sdl_update_caption(scon);
    }

    int kbd = 0, mouse = 0;
#ifdef __ANDROID__
    if (g_android_use_hud) {
        xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);
    }
#else
    xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);
#endif

    while (SDL_PollEvent(ev)) {
        // HUD must process events first so that if a controller is detached,
        // a latent rebind request can cancel before the state is freed
#ifdef __ANDROID__
        if (g_android_use_hud) {
            xemu_hud_process_sdl_events(ev);
        }
#else
        xemu_hud_process_sdl_events(ev);
#endif
        xemu_input_process_sdl_events(ev);

        switch (ev->type) {
        case SDL_KEYDOWN:
            if (kbd) break;
            handle_keydown(ev);
            break;
        case SDL_KEYUP:
            if (kbd) break;
            handle_keyup(ev);
            break;
        case SDL_TEXTINPUT:
            if (kbd) break;
            handle_textinput(ev);
            break;
        case SDL_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
#ifdef __ANDROID__
                g_android_should_quit = true;
                __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                    "android: SDL_QUIT");
#endif
            }
            break;
#ifdef __ANDROID__
        case SDL_APP_TERMINATING:
            g_android_should_quit = true;
            bdrv_flush_all();
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "android: app terminating, flushed block devices");
            break;
        case SDL_APP_WILLENTERBACKGROUND:
            g_android_paused = true;
            bdrv_flush_all();
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "android: app entering background, flushed block devices");
            break;
        case SDL_APP_DIDENTERBACKGROUND:
            break;
        case SDL_APP_WILLENTERFOREGROUND:
        case SDL_APP_DIDENTERFOREGROUND:
            g_android_paused = false;
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "android: app foreground");
            break;
#endif
        case SDL_MOUSEMOTION:
            if (mouse) break;
            handle_mousemotion(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (mouse) break;
            handle_mousebutton(ev);
            break;
        case SDL_MOUSEWHEEL:
            if (mouse) break;
            handle_mousewheel(ev);
            break;
        case SDL_WINDOWEVENT:
            handle_windowevent(ev);
            break;
        default:
            break;
        }
    }

    xemu_input_update_controllers();

    scon->idle_counter = 0;
    scon->dcl.update_interval = 16; // Ignored
}

static void sdl_mouse_warp(DisplayChangeListener *dcl,
                           int x, int y, bool on)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            sdl_show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        sdl_hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void sdl_mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_FreeCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_FreeSurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateRGBSurfaceFrom(c->data, c->width, c->height, 32, c->width * 4,
                                 0xff0000, 0x00ff00, 0xff, 0xff000000);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "sdl2-gl",
    .dpy_gfx_update          = sdl2_gl_update,
    .dpy_gfx_switch          = sdl2_gl_switch,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = sdl_mouse_warp,
    .dpy_cursor_define       = sdl_mouse_define,
    .dpy_gl_update           = sdl2_gl_scanout_flush,
};

static void sdl2_display_very_early_init(DisplayOptions *o)
{
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_very_early_init: start");
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    /* on Linux, SDL may use fbcon|directfb|svgalib when run without
     * accessible $DISPLAY to open X11 window.  This is often the case
     * when qemu is run using sudo.  But in this case, and when actually
     * run in X11 environment, SDL fights with X11 for the video card,
     * making current display unavailable, often until reboot.
     * So make x11 the default SDL video driver if this variable is unset.
     * This is a bit hackish but saves us from bigger problem.
     * Maybe it's a good idea to fix this in SDL instead.
     */
    setenv("SDL_VIDEODRIVER", "x11", 0);
#endif

#ifdef __ANDROID__
    SDL_setenv("SDL_AUDIODRIVER", "aaudio", 0);
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "SDL_AUDIODRIVER=%s", SDL_getenv("SDL_AUDIODRIVER"));
#endif

    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Failed to initialize SDL video subsystem: %s\n",
                SDL_GetError());
        exit(1);
    }

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // Initialize rendering context
    // Note: On Android, we always need GL context even for Vulkan because
    // Vulkan uses GL external memory for display presentation
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    char *title = g_strdup_printf("xemu | v%s"
#ifdef XEMU_DEBUG_BUILD
                                  " Debug"
#endif
                                  , xemu_version);

    // Decide window size
    int min_window_width = 640;
    int min_window_height = 480;
    int window_width = min_window_width;
    int window_height = min_window_height;

    const int res_table[][2] = {
        {640,  480},
        {720,  480},
        {1280, 720},
        {1280, 800},
        {1280, 960},
        {1920, 1080},
        {2560, 1440},
        {2560, 1600},
        {2560, 1920},
        {3840, 2160}
    };

    if (g_config.display.window.startup_size == CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED) {
        window_width  = g_config.display.window.last_width;
        window_height = g_config.display.window.last_height;
    } else {
        window_width  = res_table[g_config.display.window.startup_size-1][0];
        window_height = res_table[g_config.display.window.startup_size-1][1];
    }

    if (window_width < min_window_width) {
        window_width = min_window_width;
    }
    if (window_height < min_window_height) {
        window_height = min_window_height;
    }

    // On Android, always use OpenGL window even for Vulkan because Vulkan
    // needs GL context for external memory display presentation
#ifdef __ANDROID__
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#else
    bool use_vulkan = (g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(
        (use_vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | 
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#endif

    // Create main window
    m_window = SDL_CreateWindow(
        title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width, window_height,
        window_flags);
    if (m_window == NULL) {
        fprintf(stderr, "Failed to create main window\n");
        SDL_Quit();
        exit(1);
    }
    g_free(title);
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    SDL_DisplayMode disp_mode;
    SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(m_window), &disp_mode);
    if (disp_mode.w < window_width || disp_mode.h < window_height) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    m_context = SDL_GL_CreateContext(m_window);

#ifndef __ANDROID__
    if (m_context != NULL && epoxy_gl_version() < 40) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DeleteContext(m_context);
        m_context = NULL;
    }
#endif

    if (m_context == NULL) {
#ifdef __ANDROID__
        const char *msg =
            "Unable to create OpenGL ES context. This usually means the\r\n"
            "graphics device on this system does not support OpenGL ES 3.0.\r\n"
            "\r\n"
            "xemu cannot continue and will now exit.";
#else
        const char *msg =
            "Unable to create OpenGL context. This usually means the\r\n"
            "graphics device on this system does not support OpenGL 4.0.\r\n"
            "\r\n"
            "xemu cannot continue and will now exit.";
#endif
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Unable to create OpenGL context",
            msg,
            m_window);
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        exit(1);
    }

    if (SDL_GL_MakeCurrent(m_window, m_context) != 0) {
        fprintf(stderr, "Failed to make GL context current: %s\n", SDL_GetError());
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        exit(1);
    }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_very_early_init: GL context current");
#endif

    int width, height, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *icon_data = stbi_load_from_memory(xemu_64x64_data, xemu_64x64_size, &width, &height, &channels, 4);
    if (icon_data) {
        SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(icon_data, width, height, 32, width*4,
            0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
        if (icon) {
            SDL_SetWindowIcon(m_window, icon);
        }
        // Note: Retaining the memory allocated by stbi_load. It's used in place
        // by the SDL surface.
    }

    fprintf(stderr, "CPU: %s\n", xemu_get_cpu_info());
    fprintf(stderr, "OS_Version: %s\n", xemu_get_os_info());
    fprintf(stderr, "GL_VENDOR: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
#ifdef __ANDROID__
    {
        const char *vendor = (const char *)glGetString(GL_VENDOR);
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        const char *version = (const char *)glGetString(GL_VERSION);
        const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
        const char *exts = (const char *)glGetString(GL_EXTENSIONS);
        g_android_gl_bgra_supported = sdl2_gl_has_extension(exts, "GL_EXT_texture_format_BGRA8888") ||
                                      sdl2_gl_has_extension(exts, "GL_EXT_texture_format_BGRA8888_OES");
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "GL_VENDOR=%s", vendor ? vendor : "(null)");
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "GL_RENDERER=%s", renderer ? renderer : "(null)");
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "GL_VERSION=%s", version ? version : "(null)");
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "GLSL_VERSION=%s", sl ? sl : "(null)");
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "GL_EXT_texture_format_BGRA8888=%s",
                            g_android_gl_bgra_supported ? "yes" : "no");
    }
#endif

    // Initialize offscreen rendering context now
#ifdef __ANDROID__
    // Android uses a separate preinit path on the SDL thread.
#else
    nv2a_context_init();
#endif
#ifndef __ANDROID__
    SDL_GL_MakeCurrent(NULL, NULL);
#endif

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_very_early_init: done");
#endif
    // FIXME: atexit(sdl_cleanup);
}

static void sdl2_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);
    display_opengl = 1;

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_early_init");
#endif
#ifdef __ANDROID__
    // Avoid binding the display context on the QEMU thread.
    return;
#else
    SDL_GL_MakeCurrent(m_window, m_context);
    SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
    xemu_hud_init(m_window, m_context);
#endif
    // blit = create_decal_shader(SHADER_TYPE_BLIT_GAMMA);
}

static void sdl2_display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;
    SDL_SysWMinfo info;

    assert(o->type == DISPLAY_TYPE_XEMU);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_init: begin");
#else
    SDL_GL_MakeCurrent(m_window, m_context);
#endif

    memset(&info, 0, sizeof(info));
    SDL_VERSION(&info.version);

    gui_fullscreen = o->has_full_screen && o->full_screen;

    gui_fullscreen |= g_config.display.window.fullscreen_on_startup;

#if 1
    // Explicitly set number of outputs to 1 for a single screen. We don't need
    // multiple for now, but maybe in the future debug stuff can go on a second
    // screen.
    sdl2_num_outputs = 1;
#else
    for (i = 0;; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        if (!con) {
            break;
        }
    }
    sdl2_num_outputs = i;
    if (sdl2_num_outputs == 0) {
        return;
    }
#endif

    sdl2_console = g_new0(struct sdl2_console, sdl2_num_outputs);
    for (i = 0; i < sdl2_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            sdl2_console[i].hidden = true;
        }
        sdl2_console[i].idx = i;
        sdl2_console[i].opts = o;
        sdl2_console[i].opengl = 1;
        sdl2_console[i].dcl.ops = &dcl_gl_ops;
        sdl2_console[i].dcl.con = con;
        sdl2_console[i].kbd = qkbd_state_init(con);
        register_displaychangelistener(&sdl2_console[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS) || defined(SDL_VIDEO_DRIVER_X11)
        if (SDL_GetWindowWMInfo(sdl2_console[i].real_window, &info)) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
            qemu_console_set_window_id(con, (uintptr_t)info.info.win.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
            qemu_console_set_window_id(con, info.info.x11.window);
#endif
        }
#endif
    }

    sdl2_console[0].real_window = m_window;
    sdl2_console[0].winctx = m_context;

    mouse_mode_notifier.notify = sdl_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    /* Tell main thread to go ahead and create the app and enter the run loop */
#ifndef __ANDROID__
    SDL_GL_MakeCurrent(NULL, NULL);
#endif
    qemu_sem_post(&display_init_sem);

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "sdl2_display_init: posted display_init_sem");
#endif
}

static QemuDisplay qemu_display_sdl2 = {
    .type       = DISPLAY_TYPE_XEMU,
    .early_init = sdl2_display_early_init,
    .init       = sdl2_display_init,
};

static void register_sdl1(void)
{
    qemu_display_register(&qemu_display_sdl2);
}

type_init(register_sdl1);

#ifdef __ANDROID__
void xemu_android_force_xemu_display_link(void)
{
}

void xemu_android_display_preinit(void)
{
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;
    sdl_render_thread_id = SDL_ThreadID();
    sdl2_display_very_early_init(NULL);
    if (SDL_GL_MakeCurrent(m_window, m_context) != 0) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "xemu_android_display_preinit: make current failed: %s",
                            SDL_GetError());
#endif
    }
#ifdef __ANDROID__
    // Cache EGL state now while GL context is current on this thread
    extern void glo_android_cache_current_egl_state(void);
    glo_android_cache_current_egl_state();
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "xemu_android_display_preinit: cached EGL state");
#endif
    nv2a_android_early_context_init();
    qemu_sem_init(&display_init_sem, 0);
}

void xemu_android_display_wait_ready(void)
{
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "xemu_android_display_wait_ready: waiting");
#endif
    qemu_sem_wait(&display_init_sem);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "xemu_android_display_wait_ready: ready");
#endif
}

void xemu_android_display_loop(void)
{
    // Mirror the desktop main-loop rendering path.
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "xemu_android_display_loop: start");
#endif
    if (sdl_render_thread_id == 0) {
        sdl_render_thread_id = SDL_ThreadID();
    }
    if (SDL_GL_GetCurrentContext() != m_context) {
        if (SDL_GL_MakeCurrent(m_window, m_context) != 0) {
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                                "xemu_android_display_loop: make current failed: %s",
                                SDL_GetError());
#endif
            return;
        }
    }
#ifdef __ANDROID__
    SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
    if (g_android_use_hud) {
        xemu_hud_init(m_window, m_context);
    }
#endif
    tcg_register_init_ctx();
    qemu_set_current_aio_context(qemu_get_aio_context());
    qemu_mutex_lock_main_loop();
    bql_lock();
    xemu_input_init();
    bql_unlock();
    qemu_mutex_unlock_main_loop();

    while (1) {
        if (g_android_should_quit || qemu_shutdown_requested_get() != SHUTDOWN_CAUSE_NONE) {
            break;
        }
        if (g_android_paused || sdl2_console[0].hidden) {
            qemu_mutex_lock_main_loop();
            bql_lock();
            sdl2_poll_events(&sdl2_console[0]);
            bql_unlock();
            qemu_mutex_unlock_main_loop();
            SDL_Delay(16);
            continue;
        }
        sdl2_gl_refresh(&sdl2_console[0].dcl);
#ifdef __ANDROID__
        if (!g_android_paused && SDL_GL_GetCurrentContext() != NULL) {
            android_log_gl_error("loop-end");
        }
#else
        assert(glGetError() == GL_NO_ERROR);
#endif
    }
}
#endif

void xb_surface_gl_create_texture(DisplaySurface *surface)
{
    assert(QEMU_IS_ALIGNED(surface_stride(surface), surface_bytes_per_pixel(surface)));

    GLenum internal_format = GL_RGBA;
    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->glformat = GL_BGRA_EXT;
        surface->gltype = GL_UNSIGNED_BYTE;
        internal_format = GL_RGBA;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->glformat = GL_RGBA;
        surface->gltype = GL_UNSIGNED_BYTE;
        internal_format = GL_RGBA;
        break;
    case PIXMAN_r5g6b5:
        surface->glformat = GL_RGB;
        surface->gltype = GL_UNSIGNED_SHORT_5_6_5;
        internal_format = GL_RGB;
        break;
    default:
        g_assert_not_reached();
    }

    if (!surface->texture) {
        glGenTextures(1, &surface->texture);
    }
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    const void *pixels = surface_data(surface);
    GLenum upload_format = surface->glformat;
    GLenum upload_type = surface->gltype;
#ifdef __ANDROID__
    uint8_t *converted = NULL;
    bool use_row_length = true;
    if (upload_format == GL_BGRA_EXT) {
        const int width = surface_width(surface);
        const int height = surface_height(surface);
        const int stride = surface_stride(surface);
        const uint8_t *src = (const uint8_t *)surface_data(surface);
        converted = g_malloc((size_t)width * height * 4);
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = src + (size_t)y * stride;
            uint8_t *dst = converted + (size_t)y * width * 4;
            for (int x = 0; x < width; ++x) {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t g = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];
                const uint8_t a = row[x * 4 + 3];
                dst[x * 4 + 0] = r;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = b;
                dst[x * 4 + 3] = a;
            }
        }
        upload_format = GL_RGBA;
        pixels = converted;
        use_row_length = false;
    }
    if (use_row_length) {
#endif
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
                      surface_stride(surface) / surface_bytes_per_pixel(surface));
#ifdef __ANDROID__
    }
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
                 surface_width(surface),
                 surface_height(surface),
                 0, upload_format, upload_type,
                 pixels);
#ifdef __ANDROID__
    if (use_row_length) {
#endif
        glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
#ifdef __ANDROID__
    }
    if (converted) {
        g_free(converted);
    }
#endif

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void xb_surface_gl_destroy_texture(DisplaySurface *surface)
{
    if (!surface || !surface->texture) {
        return;
    }
    glDeleteTextures(1, &surface->texture);
    surface->texture = 0;
}

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    assert(scon->opengl);

#ifdef __ANDROID__
    if (!sdl2_is_render_thread()) {
        return;
    }
#endif
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
}

void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    assert(scon->opengl);
    DisplaySurface *old_surface = scon->surface;
    scon->surface = new_surface;
#ifdef __ANDROID__
    if (!sdl2_is_render_thread()) {
        return;
    }
#endif
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    xb_surface_gl_destroy_texture(old_surface);
    if (!new_surface) {
        return;
    }

    if (!scon->real_window) {
        scon->real_window = m_window;
        scon->winctx = m_context;
        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    }
}

float fps = 1.0;

static void update_fps(void)
{
    static int64_t last_update = 0;
    const float r = 0.5;//0.1;
    static float avg = 1.0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    float ms = ((float)(now-last_update)/1000000.0);
    last_update = now;
    if (fabs(avg-ms) > 0.25*avg) avg = ms;
    else avg = avg*(1.0-r)+ms*r;
    fps = 1000.0/avg;
}

void sdl2_gl_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    assert(scon->opengl);
    bool flip_required = false;

#ifdef __ANDROID__
    if (!sdl2_is_render_thread()) {
        return;
    }
    if (g_android_paused || scon->hidden) {
        if ((g_android_frame_counter++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh paused: hidden=%d paused=%d runstate=%d",
                                scon->hidden ? 1 : 0,
                                g_android_paused ? 1 : 0,
                                (int)runstate_get());
        }
        qemu_mutex_lock_main_loop();
        bql_lock();
        sdl2_poll_events(scon);
        bql_unlock();
        qemu_mutex_unlock_main_loop();
        SDL_Delay(16);
        return;
    }
#endif

    /* Frame limiter: cap the render/swap path at ~60 Hz. */
    {
        static int64_t next_render_ns;
        const int64_t min_frame_ns = 16666667; /* 60 Hz */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

        if (next_render_ns && now < next_render_ns) {
            qemu_mutex_lock_main_loop();
            bql_lock();
            sdl2_poll_events(scon);
            bql_unlock();
            qemu_mutex_unlock_main_loop();
            SDL_Delay(1);
            return;
        }
        now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (!next_render_ns || now > next_render_ns + min_frame_ns) {
            next_render_ns = now + min_frame_ns;
        } else {
            next_render_ns += min_frame_ns;
        }
    }

    if (SDL_GL_MakeCurrent(scon->real_window, scon->winctx) != 0 ||
        SDL_GL_GetCurrentContext() == NULL) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "xemu-android",
                            "sdl2_gl_refresh: make current failed: %s",
                            SDL_GetError());
        g_android_paused = true;
#endif
        qemu_mutex_lock_main_loop();
        bql_lock();
        sdl2_poll_events(scon);
        bql_unlock();
        qemu_mutex_unlock_main_loop();
        SDL_Delay(16);
        return;
    }
#ifdef __ANDROID__
    android_log_gl_error("refresh-makecurrent");
#endif
    update_fps();
    g_android_frame_counter++;

#ifdef __ANDROID__
    static int force_cpu_blit_mode = -2; /* -2=uninit, -1=auto, 0=off, 1=on */
    if (force_cpu_blit_mode == -2) {
        const char *env = SDL_getenv("XEMU_ANDROID_FORCE_CPU_BLIT");
        if (!env) {
            force_cpu_blit_mode = -1;
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh: CPU blit mode auto");
        } else if (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 ||
                   strcmp(env, "TRUE") == 0) {
            force_cpu_blit_mode = 1;
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh: forcing CPU blit path");
        } else {
            force_cpu_blit_mode = 0;
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh: CPU blit disabled via env");
        }
    }
    bool force_cpu_blit = false;
    if (force_cpu_blit_mode == -1) {
        /* Vulkan path should prefer direct texture presentation on Android. */
        force_cpu_blit = (g_config.display.renderer != CONFIG_DISPLAY_RENDERER_VULKAN);
    } else {
        force_cpu_blit = (force_cpu_blit_mode == 1);
    }
#endif

    /* XXX: Note that this bypasses the usual VGA path in order to quickly
     * get the surface. This is simple and fast, at the cost of accuracy.
     * Ideally, this should go through the VGA code and opportunistically pull
     * the surface like this, but handle the VGA logic as well. For now, just
     * use this fast path to handle the common case.
     *
     * In the event the surface is not found in the surface cache, e.g. when
     * the guest code isn't using HW accelerated rendering, but just blitting
     * to the framebuffer, fall back to the VGA path.
     */
    GLuint tex = 0;
#ifdef __ANDROID__
    if (force_cpu_blit) {
        /* Trigger a render/sync so the readback buffer gets updated. */
        (void)nv2a_get_framebuffer_surface();
        nv2a_release_framebuffer_surface();

        int rb_w = 0;
        int rb_h = 0;
        if (nv2a_android_copy_readback(&g_android_cpu_buf,
                                       &g_android_cpu_buf_size,
                                       &rb_w, &rb_h)) {
            if ((g_android_frame_counter % 120) == 0) {
                __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                    "refresh: using readback %dx%d", rb_w, rb_h);
                if (g_android_cpu_buf && rb_w > 0 && rb_h > 0) {
                    uint32_t tl = 0, mid = 0, br = 0;
                    size_t tl_off = 0;
                    size_t mid_off = ((size_t)(rb_h / 2) * (size_t)rb_w +
                                      (size_t)(rb_w / 2)) * 4;
                    size_t br_off = ((size_t)(rb_h - 1) * (size_t)rb_w +
                                     (size_t)(rb_w - 1)) * 4;
                    if (mid_off + 4 <= g_android_cpu_buf_size) {
                        memcpy(&mid, g_android_cpu_buf + mid_off, sizeof(mid));
                    }
                    if (br_off + 4 <= g_android_cpu_buf_size) {
                        memcpy(&br, g_android_cpu_buf + br_off, sizeof(br));
                    }
                    if (tl_off + 4 <= g_android_cpu_buf_size) {
                        memcpy(&tl, g_android_cpu_buf + tl_off, sizeof(tl));
                    }
                    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                        "readback sample tl=%08x mid=%08x br=%08x",
                                        tl, mid, br);
                }
            }
            if (!g_android_cpu_tex) {
                glGenTextures(1, &g_android_cpu_tex);
            }
            glBindTexture(GL_TEXTURE_2D, g_android_cpu_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            if (rb_w != g_android_cpu_tex_w || rb_h != g_android_cpu_tex_h) {
                g_android_cpu_tex_w = rb_w;
                g_android_cpu_tex_h = rb_h;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rb_w, rb_h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, g_android_cpu_buf);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rb_w, rb_h,
                                GL_RGBA, GL_UNSIGNED_BYTE, g_android_cpu_buf);
            }
            tex = g_android_cpu_tex;
            flip_required = true;
        } else if ((g_android_frame_counter % 120) == 0) {
            __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                                "refresh: no readback available yet");
        }
        if ((g_android_frame_counter % 120) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh: force CPU blit path");
        }
    } else {
        tex = nv2a_get_framebuffer_surface();
        if (tex != 0 && glIsTexture(tex) == GL_FALSE) {
            if ((g_android_frame_counter % 120) == 0) {
                __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                                    "refresh: nv2a tex %u not valid in display context",
                                    (unsigned)tex);
            }
            tex = 0;
        }
    }
#else
    tex = nv2a_get_framebuffer_surface();
#endif
#ifdef __ANDROID__
    android_log_gl_error("refresh-get-fb");
    if ((g_android_frame_counter % 120) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "refresh frame=%llu tex=%u flip=%d surface=%p size=%dx%d runstate=%d",
                            (unsigned long long)g_android_frame_counter,
                            (unsigned)tex,
                            flip_required ? 1 : 0,
                            (void *)scon->surface,
                            scon->surface ? surface_width(scon->surface) : 0,
                            scon->surface ? surface_height(scon->surface) : 0,
                            (int)runstate_get());
    }
#endif
    if (tex == 0) {
#ifdef __ANDROID__
        qemu_mutex_lock_main_loop();
        bql_lock();
        graphic_hw_update(scon->dcl.con);
        bql_unlock();
        qemu_mutex_unlock_main_loop();
#endif
        // FIXME: Don't upload if notdirty
        xb_surface_gl_create_texture(scon->surface);
        scon->updates++;
        tex = scon->surface->texture;
        flip_required = true;
#ifdef __ANDROID__
        if ((g_android_frame_counter % 120) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "refresh no nv2a fb, using surface texture=%u",
                                (unsigned)tex);
        }
#endif
    }
#ifdef __ANDROID__
    android_log_gl_error("refresh-create-texture");
#endif

    /* FIXME: Finer locking. Event handlers in segments of the code expect
     * to be running on the main thread with the BQL. For now, acquire the
     * lock and perform rendering, but release before swap to avoid
     * possible lengthy blocking (for vsync).
     */
    qemu_mutex_lock_main_loop();
    bql_lock();
    sdl2_poll_events(scon);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef __ANDROID__
    android_blit_frame(tex, flip_required);
#else
#endif
#ifdef __ANDROID__
    android_log_gl_error("refresh-blit");
#else
    xemu_snapshots_set_framebuffer_texture(tex, flip_required);
    xemu_hud_set_framebuffer_texture(tex, flip_required);
    xemu_hud_render();
#endif

    // Release BQL before swapping (which may sleep if swap interval is not immediate)
    bql_unlock();
    qemu_mutex_unlock_main_loop();

#ifndef __ANDROID__
    glFinish();
#endif
    nv2a_release_framebuffer_surface();
#ifdef __ANDROID__
    android_log_gl_error("refresh-finish");
#endif
    SDL_GL_SwapWindow(scon->real_window);
#ifdef __ANDROID__
    android_log_gl_error("refresh-swap");
#endif

    /* VGA update for dirty-region tracking */
    qemu_mutex_lock_main_loop();
    bql_lock();
    graphic_hw_update(scon->dcl.con);
    if (scon->updates && scon->surface) {
        scon->updates = 0;
    }
    bql_unlock();
    qemu_mutex_unlock_main_loop();

}

void sdl2_gl_redraw(struct sdl2_console *scon)
{
    assert(scon->opengl);

    if (scon->scanout_mode) {
        assert(0);
        /* sdl2_gl_scanout_flush actually only care about
         * the first argument. */
        // return sdl2_gl_scanout_flush(&scon->dcl, 0, 0, 0, 0);
    }
    if (scon->surface) {
        // xemu_sdl2_gl_render_surface(scon);
    }
}

QEMUGLContext sdl2_gl_create_context(DisplayChangeListener *dcl,
                                     QEMUGLParams *params)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    SDL_GLContext ctx;

    assert(0);

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (scon->opts->gl == DISPLAY_GL_MODE_ON ||
        scon->opts->gl == DISPLAY_GL_MODE_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
    } else if (scon->opts->gl == DISPLAY_GL_MODE_ES) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, params->minor_ver);

    ctx = SDL_GL_CreateContext(scon->real_window);

    /* If SDL fail to create a GL context and we use the "on" flag,
     * then try to fallback to GLES.
     */
    if (!ctx && scon->opts->gl == DISPLAY_GL_MODE_ON) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        ctx = SDL_GL_CreateContext(scon->real_window);
    }
    return (QEMUGLContext)ctx;
}

void sdl2_gl_destroy_context(DisplayChangeListener *dcl, QEMUGLContext ctx)
{
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    SDL_GL_DeleteContext(sdlctx);
}

int sdl2_gl_make_context_current(DisplayChangeListener *dcl,
                                 QEMUGLContext ctx)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    assert(scon->opengl);

#ifdef __ANDROID__
    if (!sdl2_is_render_thread()) {
        return 0;
    }
#endif
    return SDL_GL_MakeCurrent(scon->real_window, sdlctx);
}

QEMUGLContext sdl2_gl_get_current_context(DisplayChangeListener *dcl)
{
    SDL_GLContext sdlctx;

    sdlctx = SDL_GL_GetCurrentContext();
    return (QEMUGLContext)sdlctx;
}

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl)
{
    assert(0);
}

void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h)
{
    assert(0);
}

void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    assert(0);
}

// sdl2-input.c
void sdl2_process_key(struct sdl2_console *scon,
                      SDL_KeyboardEvent *ev)
{
    int qcode;

    if (ev->keysym.scancode >= qemu_input_map_usb_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->keysym.scancode];
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_KEYDOWN);
}

int gArgc;
char **gArgv;

// vl.c

static void *call_qemu_main(void *opaque)
{
    int status;

    DPRINTF("Second thread: calling qemu_main()\n");
    qemu_init(gArgc, gArgv);
    status = qemu_main();
    DPRINTF("Second thread: qemu_main() returned, exiting\n");
    exit(status);
}

/* Note: only supports millisecond resolution on Windows */
static void sleep_ns(int64_t ns)
{
#ifndef _WIN32
        struct timespec sleep_delay, rem_delay;
        sleep_delay.tv_sec = ns / 1000000000LL;
        sleep_delay.tv_nsec = ns % 1000000000LL;
        nanosleep(&sleep_delay, &rem_delay);
#else
        Sleep(ns / SCALE_MS);
#endif
}

#ifdef _WIN32
static const wchar_t *get_executable_name(void)
{
    static wchar_t exe_name[MAX_PATH] = { 0 };
    static bool initialized = false;

    if (!initialized) {
        wchar_t full_path[MAX_PATH];
        DWORD length = GetModuleFileNameW(NULL, full_path, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return NULL;
        }

        wchar_t *last_slash = wcsrchr(full_path, L'\\');
        if (last_slash) {
            wcsncpy_s(exe_name, MAX_PATH, last_slash + 1, _TRUNCATE);
        } else {
            wcsncpy_s(exe_name, MAX_PATH, full_path, _TRUNCATE);
        }

        initialized = true;
    }

    return exe_name;
}

static void setup_nvidia_profile(void)
{
    const wchar_t *exe_name = get_executable_name();
    if (exe_name == NULL) {
        fprintf(stderr, "Failed to get current executable name\n");
        return;
    }

    if (nvapi_init()) {
        nvapi_setup_profile((NvApiProfileOpts){
            .profile_name = L"xemu",
            .executable_name = exe_name,
            .threaded_optimization = false,
        });
        nvapi_finalize();
    }
}
#endif

#ifndef __ANDROID__
int main(int argc, char **argv)
{
    QemuThread thread;

    setlocale(LC_NUMERIC, "C");

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Launched with a console. If stdout and stderr are not associated with
        // an output stream, redirect to parent console.
        if (_fileno(stdout) == -2) {
            freopen("CONOUT$", "w+", stdout);
        }
        if (_fileno(stderr) == -2) {
            freopen("CONOUT$", "w+", stderr);
        }
    } else {
        // Launched without a console. Redirect stdout and stderr to a log file.
        HANDLE logfile = CreateFileA("xemu.log",
            GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logfile != INVALID_HANDLE_VALUE) {
            freopen("xemu.log", "a", stdout);
            freopen("xemu.log", "a", stderr);
        }
    }

    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);

    DPRINTF("Entered main()\n");
    gArgc = argc;
    gArgv = argv;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i+1]) {
                xemu_settings_set_path(argv[i+1]);
                argv[i+1] = NULL;
            }
            break;
        }
    }

    if (!xemu_settings_load()) {
        const char *err_msg = xemu_settings_get_error_message();
        fprintf(stderr, "%s", err_msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
            "Failed to load xemu config file", err_msg,
            m_window);
        SDL_Quit();
        exit(1);
    }
    atexit(xemu_settings_save);

#ifdef _WIN32
    if (g_config.display.setup_nvidia_profile) {
        setup_nvidia_profile();
    }
#endif

    sdl2_display_very_early_init(NULL);

    qemu_sem_init(&display_init_sem, 0);
    qemu_thread_create(&thread, "qemu_main", call_qemu_main,
                       NULL, QEMU_THREAD_DETACHED);

    DPRINTF("Main thread: waiting for display_init_sem\n");
    qemu_sem_wait(&display_init_sem);

    gui_grab = 0;
    if (gui_fullscreen) {
        sdl_grab_start(0);
        set_full_screen(&sdl2_console[0], gui_fullscreen);
    }

    /*
     * FIXME: May want to create a callback mechanism for main QEMU thread
     * to just run functions to avoid TLS bugs and locking issues.
     */
    tcg_register_init_ctx();
    // rcu_register_thread();
    qemu_set_current_aio_context(qemu_get_aio_context());

    DPRINTF("Main thread: initializing app\n");

    qemu_mutex_lock_main_loop();
    bql_lock();
    xemu_input_init();
    bql_unlock();
    qemu_mutex_unlock_main_loop();

    while (1) {
        sdl2_gl_refresh(&sdl2_console[0].dcl);
        assert(glGetError() == GL_NO_ERROR);
    }

    // rcu_unregister_thread();
}
#endif

void xemu_eject_disc(Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    // Xbox software may request that the drive open, but do it now anyway
    qmp_eject("ide0-cd1", NULL, true, false, &error);
    if (error) {
        error_propagate(errp, error);
    }

    xbox_smc_update_tray_state();
}

void xemu_load_disc(const char *path, Error **errp)
{
    Error *error = NULL;

    // Ensure an eject sequence is always triggered so Xbox software reloads
    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    qmp_blockdev_change_medium("ide0-cd1", NULL, path, "raw", false, false,
                               false, 0, &error);
    if (error) {
        error_propagate(errp, error);
    } else {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, path);
    }

    xbox_smc_update_tray_state();
}
