#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/display/drm/lv_linux_drm.h>
#include <lvgl/src/drivers/evdev/lv_evdev.h>

#define DEFAULT_SECONDS 30
#define DEFAULT_REFR_MS 16

typedef struct {
    lv_obj_t *list;
    lv_obj_t *status;
    int pos;
    int dir;
    int max_scroll;
    uint32_t frames;
    uint64_t start_us;
} probe_state_t;

static volatile int running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);

    if(!value || !value[0]) {
        return fallback;
    }
    return atoi(value);
}

static int render_mode_from_env(void)
{
    const char *value = getenv("K230_SCROLL_PROBE_RENDER");

    if(!value || !value[0] || strcmp(value, "direct") == 0) {
        return LV_DISPLAY_RENDER_MODE_DIRECT;
    }
    if(strcmp(value, "partial") == 0) {
        return LV_DISPLAY_RENDER_MODE_PARTIAL;
    }
    if(strcmp(value, "full") == 0) {
        return LV_DISPLAY_RENDER_MODE_FULL;
    }
    return LV_DISPLAY_RENDER_MODE_DIRECT;
}

static const char *render_mode_name(int mode)
{
    switch(mode) {
    case LV_DISPLAY_RENDER_MODE_PARTIAL:
        return "partial";
    case LV_DISPLAY_RENDER_MODE_FULL:
        return "full";
    case LV_DISPLAY_RENDER_MODE_DIRECT:
    default:
        return "direct";
    }
}

static const char *find_input_device(void)
{
    const char *env = getenv("K230_SCROLL_PROBE_INPUT");

    if(env && env[0] && access(env, R_OK) == 0) {
        return env;
    }
    if(access("/dev/input/event1", R_OK) == 0) {
        return "/dev/input/event1";
    }
    if(access("/dev/input/event0", R_OK) == 0) {
        return "/dev/input/event0";
    }
    return NULL;
}

static void scroll_timer_cb(lv_timer_t *timer)
{
    probe_state_t *state = lv_timer_get_user_data(timer);
    uint64_t now = monotonic_us();
    char text[128];

    if(!state || !state->list) {
        return;
    }

    state->pos += state->dir * 7;
    if(state->pos >= state->max_scroll) {
        state->pos = state->max_scroll;
        state->dir = -1;
    } else if(state->pos <= 0) {
        state->pos = 0;
        state->dir = 1;
    }

    lv_obj_scroll_to_y(state->list, state->pos, LV_ANIM_OFF);
    state->frames++;
    if(state->frames % 30U == 0U && state->status) {
        double sec = (double)(now - state->start_us) / 1000000.0;
        double fps = sec > 0.0 ? (double)state->frames / sec : 0.0;

        snprintf(text, sizeof(text), "LVGL scroll probe  y=%d/%d  %.1ffps",
                 state->pos, state->max_scroll, fps);
        lv_label_set_text(state->status, text);
    }
}

static void make_row(lv_obj_t *parent, int index, int width)
{
    static const uint32_t colors[] = {
        0x173042, 0x243B2E, 0x402A33, 0x2D3148, 0x3A3327,
    };
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *title;
    lv_obj_t *sub;
    int color_index = index % (int)(sizeof(colors) / sizeof(colors[0]));

    lv_obj_set_size(row, width, 88);
    lv_obj_set_style_bg_color(row, lv_color_hex(colors[color_index]), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(row);
    lv_label_set_text_fmt(title, "Probe item %02d", index + 1);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_pos(title, 12, 10);

    sub = lv_label_create(row);
    lv_label_set_text_fmt(sub, "opaque row, stable geometry, frame marker %03d",
                          index * 17);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xA7B0BE), 0);
    lv_obj_set_pos(sub, 12, 44);
}

static void build_ui(lv_display_t *disp, probe_state_t *state, int auto_scroll)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *list;
    lv_obj_t *status;
    int w = (int)lv_display_get_horizontal_resolution(disp);
    int h = (int)lv_display_get_vertical_resolution(disp);
    int list_y = 72;
    int list_h = h - list_y - 24;
    int row_w = w - 64;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x070B10), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    status = lv_label_create(scr);
    lv_label_set_text_fmt(status, "LVGL scroll probe  %dx%d  %s",
                          w, h, auto_scroll ? "auto" : "manual");
    lv_obj_set_style_text_color(status, lv_color_hex(0xDDE7F0), 0);
    lv_obj_set_pos(status, 24, 22);

    list = lv_obj_create(scr);
    lv_obj_set_pos(list, 24, list_y);
    lv_obj_set_size(list, w - 48, list_h);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0B0D10), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 10, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 12, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_bottom(list, 48, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC |
                            LV_OBJ_FLAG_SCROLL_MOMENTUM |
                            LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for(int i = 0; i < 64; i++) {
        make_row(list, i, row_w);
    }

    lv_obj_update_layout(list);
    state->list = list;
    state->status = status;
    state->pos = 0;
    state->dir = 1;
    state->frames = 0;
    state->start_us = monotonic_us();
    state->max_scroll = lv_obj_get_scroll_bottom(list);
    if(state->max_scroll < 400) {
        state->max_scroll = 400;
    }
}

int main(int argc, char **argv)
{
    lv_display_t *disp;
    lv_timer_t *refr_timer;
    lv_timer_t *scroll_timer = NULL;
    probe_state_t state;
    char *drm_path;
    const char *input_dev;
    int seconds = DEFAULT_SECONDS;
    int auto_scroll = 1;
    int rotation;
    int refr_ms;
    int render_mode;
    uint64_t start_us;

    if(argc > 1) {
        seconds = atoi(argv[1]);
        if(seconds <= 0) {
            seconds = DEFAULT_SECONDS;
        }
    }
    if(argc > 2 && strcmp(argv[2], "manual") == 0) {
        auto_scroll = 0;
    }

    memset(&state, 0, sizeof(state));
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    rotation = env_int("K230_SCROLL_PROBE_ROTATION", 0);
    refr_ms = env_int("K230_SCROLL_PROBE_REFR_MS", DEFAULT_REFR_MS);
    render_mode = render_mode_from_env();

    lv_init();
    disp = lv_linux_drm_create();
    if(!disp) {
        fprintf(stderr, "failed to create LVGL DRM display\n");
        return 1;
    }
    drm_path = lv_linux_drm_find_device_path();
    if(!drm_path) {
        fprintf(stderr, "no DRM device found\n");
        return 1;
    }
    lv_linux_drm_set_rotation(disp, rotation);
    if(lv_linux_drm_set_file(disp, drm_path, -1) != LV_RESULT_OK) {
        fprintf(stderr, "failed to open DRM device %s\n", drm_path);
        lv_free(drm_path);
        return 1;
    }
    lv_free(drm_path);
    lv_display_set_render_mode(disp, render_mode);
    refr_timer = lv_display_get_refr_timer(disp);
    if(refr_timer) {
        lv_timer_set_period(refr_timer, refr_ms);
    }

    input_dev = find_input_device();
    if(input_dev) {
        lv_evdev_create(LV_INDEV_TYPE_POINTER, input_dev);
    }

    build_ui(disp, &state, auto_scroll);
    if(auto_scroll) {
        scroll_timer = lv_timer_create(scroll_timer_cb, 16, &state);
    }

    printf("LVGL scroll probe: %ldx%ld rotation=%d render=%s refr=%dms input=%s duration=%ds mode=%s\n",
           (long)lv_display_get_horizontal_resolution(disp),
           (long)lv_display_get_vertical_resolution(disp),
           rotation, render_mode_name(render_mode), refr_ms,
           input_dev ? input_dev : "none", seconds,
           auto_scroll ? "auto" : "manual");
    fflush(stdout);

    start_us = monotonic_us();
    while(running && monotonic_us() - start_us < (uint64_t)seconds * 1000000ULL) {
        lv_timer_handler();
        usleep(5000);
    }

    if(scroll_timer) {
        lv_timer_delete(scroll_timer);
    }
    return 0;
}
