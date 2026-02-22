#ifndef XEMU_CONFIG_H
#define XEMU_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL = 0,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT,
} CONFIG_DISPLAY_RENDERER;

typedef enum CONFIG_DISPLAY_FILTERING {
    CONFIG_DISPLAY_FILTERING_LINEAR = 0,
    CONFIG_DISPLAY_FILTERING_NEAREST,
    CONFIG_DISPLAY_FILTERING__COUNT,
} CONFIG_DISPLAY_FILTERING;

typedef enum CONFIG_DISPLAY_WINDOW_STARTUP_SIZE {
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED = 0,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_640X480,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_720X480,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_1280X720,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_1280X800,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_1280X960,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_1920X1080,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_2560X1440,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_2560X1600,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_2560X1920,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_3840X2160,
    CONFIG_DISPLAY_WINDOW_STARTUP_SIZE__COUNT,
} CONFIG_DISPLAY_WINDOW_STARTUP_SIZE;

typedef enum CONFIG_DISPLAY_UI_FIT {
    CONFIG_DISPLAY_UI_FIT_CENTER = 0,
    CONFIG_DISPLAY_UI_FIT_SCALE,
    CONFIG_DISPLAY_UI_FIT_STRETCH,
    CONFIG_DISPLAY_UI_FIT__COUNT,
} CONFIG_DISPLAY_UI_FIT;

typedef enum CONFIG_DISPLAY_UI_ASPECT_RATIO {
    CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE = 0,
    CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO,
    CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3,
    CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9,
    CONFIG_DISPLAY_UI_ASPECT_RATIO__COUNT,
} CONFIG_DISPLAY_UI_ASPECT_RATIO;

typedef enum CONFIG_NET_BACKEND {
    CONFIG_NET_BACKEND_NAT = 0,
    CONFIG_NET_BACKEND_UDP,
    CONFIG_NET_BACKEND_PCAP,
    CONFIG_NET_BACKEND__COUNT,
} CONFIG_NET_BACKEND;

typedef enum CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL {
    CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_TCP = 0,
    CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP,
    CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL__COUNT,
} CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL;

typedef enum CONFIG_SYS_MEM_LIMIT {
    CONFIG_SYS_MEM_LIMIT_64 = 0,
    CONFIG_SYS_MEM_LIMIT_128,
    CONFIG_SYS_MEM_LIMIT__COUNT,
} CONFIG_SYS_MEM_LIMIT;

typedef enum CONFIG_SYS_AVPACK {
    CONFIG_SYS_AVPACK_SCART = 0,
    CONFIG_SYS_AVPACK_HDTV,
    CONFIG_SYS_AVPACK_VGA,
    CONFIG_SYS_AVPACK_RFU,
    CONFIG_SYS_AVPACK_SVIDEO,
    CONFIG_SYS_AVPACK_COMPOSITE,
    CONFIG_SYS_AVPACK_NONE,
    CONFIG_SYS_AVPACK__COUNT,
} CONFIG_SYS_AVPACK;

struct config {
    struct general {
        bool show_welcome;
        struct {
            bool check;
        } updates;
        const char *screenshot_dir;
        const char *games_dir;
        bool skip_boot_anim;
        int last_viewed_menu_index;
        const char *user_token;
        struct {
            struct {
                const char *f5;
                const char *f6;
                const char *f7;
                const char *f8;
            } shortcuts;
            bool filter_current_game;
        } snapshots;
    } general;

    struct input {
        struct bindings {
            const char *port1_driver;
            const char *port1;
            const char *port2_driver;
            const char *port2;
            const char *port3_driver;
            const char *port3;
            const char *port4_driver;
            const char *port4;
        } bindings;
        struct peripherals {
            struct {
                int peripheral_type_0;
                const char *peripheral_param_0;
                int peripheral_type_1;
                const char *peripheral_param_1;
            } port1;
            struct {
                int peripheral_type_0;
                const char *peripheral_param_0;
                int peripheral_type_1;
                const char *peripheral_param_1;
            } port2;
            struct {
                int peripheral_type_0;
                const char *peripheral_param_0;
                int peripheral_type_1;
                const char *peripheral_param_1;
            } port3;
            struct {
                int peripheral_type_0;
                const char *peripheral_param_0;
                int peripheral_type_1;
                const char *peripheral_param_1;
            } port4;
        } peripherals;
        const char *gamecontrollerdb_path;
        bool auto_bind;
        bool allow_vibration;
        bool background_input_capture;
        struct keyboard_controller_scancode_map {
            int a;
            int b;
            int x;
            int y;
            int dpad_left;
            int dpad_up;
            int dpad_right;
            int dpad_down;
            int back;
            int start;
            int white;
            int black;
            int lstick_btn;
            int rstick_btn;
            int guide;
            int lstick_up;
            int lstick_left;
            int lstick_right;
            int lstick_down;
            int ltrigger;
            int rstick_up;
            int rstick_left;
            int rstick_right;
            int rstick_down;
            int rtrigger;
        } keyboard_controller_scancode_map;
        struct gamepad_mappings {
            const char *gamepad_id;
            bool enable_rumble;
            struct controller_mapping {
                int a;
                int b;
                int x;
                int y;
                int back;
                int guide;
                int start;
                int lstick_btn;
                int rstick_btn;
                int lshoulder;
                int rshoulder;
                int dpad_up;
                int dpad_down;
                int dpad_left;
                int dpad_right;
                int axis_left_x;
                int axis_left_y;
                int axis_right_x;
                int axis_right_y;
                int axis_trigger_left;
                int axis_trigger_right;
                bool invert_axis_left_x;
                bool invert_axis_left_y;
                bool invert_axis_right_x;
                bool invert_axis_right_y;
            } controller_mapping;
        } *gamepad_mappings;
        unsigned int gamepad_mappings_count;
    } input;

    struct display {
        CONFIG_DISPLAY_RENDERER renderer;
        struct {
            bool validation_layers;
            bool debug_shaders;
            bool assert_on_validation_msg;
            const char *preferred_physical_device;
        } vulkan;
        struct {
            int surface_scale;
        } quality;
        CONFIG_DISPLAY_FILTERING filtering;
        struct window {
            bool fullscreen_on_startup;
            bool fullscreen_exclusive;
            CONFIG_DISPLAY_WINDOW_STARTUP_SIZE startup_size;
            int last_width;
            int last_height;
            bool vsync;
        } window;
        struct ui {
            bool show_menubar;
            bool show_notifications;
            bool hide_cursor;
            bool use_animations;
            CONFIG_DISPLAY_UI_FIT fit;
            CONFIG_DISPLAY_UI_ASPECT_RATIO aspect_ratio;
            int scale;
            bool auto_scale;
        } ui;
        struct debug {
            struct video {
                bool transparency;
                double x_pos;
                double y_pos;
                double x_winsize;
                double y_winsize;
                bool advanced_tree_state;
            } video;
        } debug;
        bool setup_nvidia_profile;
    } display;

    struct audio {
        struct {
            int num_workers;
        } vp;
        bool use_dsp;
        bool hrtf;
        double volume_limit;
    } audio;

    struct net {
        bool enable;
        CONFIG_NET_BACKEND backend;
        struct {
            const char *netif;
        } pcap;
        struct {
            const char *bind_addr;
            const char *remote_addr;
        } udp;
        struct {
            struct forward_port {
                int host;
                int guest;
                CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol;
            } *forward_ports;
            unsigned int forward_ports_count;
        } nat;
    } net;

    struct sys {
        CONFIG_SYS_MEM_LIMIT mem_limit;
        CONFIG_SYS_AVPACK avpack;
        struct files {
            const char *bootrom_path;
            const char *flashrom_path;
            const char *eeprom_path;
            const char *hdd_path;
            const char *dvd_path;
        } files;
    } sys;

    struct perf {
        bool hard_fpu;
        bool cache_shaders;
    } perf;
};

#endif /* XEMU_CONFIG_H */
