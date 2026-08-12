#include "ui_display_test.h"

#include "ui_i18n.h"

#include <stdint.h>
#include <stdio.h>

typedef enum {
    DISPLAY_TEST_SOLID = 0,
    DISPLAY_TEST_BARS,
    DISPLAY_TEST_GRAY,
    DISPLAY_TEST_GRID,
    DISPLAY_TEST_CHECKER,
} display_test_kind_t;

typedef struct {
    const char *title;
    const char *hint;
    display_test_kind_t kind;
    uint32_t color;
} display_test_pattern_t;

static const display_test_pattern_t display_test_patterns[] = {
    {"Black", "Check for bright stuck pixels", DISPLAY_TEST_SOLID, 0x000000},
    {"White", "Check for dark dead pixels", DISPLAY_TEST_SOLID, 0xFFFFFF},
    {"Red", "Check red sub-pixels", DISPLAY_TEST_SOLID, 0xFF0000},
    {"Green", "Check green sub-pixels", DISPLAY_TEST_SOLID, 0x00FF00},
    {"Blue", "Check blue sub-pixels", DISPLAY_TEST_SOLID, 0x0000FF},
    {"Cyan", "Check mixed color uniformity", DISPLAY_TEST_SOLID, 0x00FFFF},
    {"Magenta", "Check mixed color uniformity", DISPLAY_TEST_SOLID, 0xFF00FF},
    {"Yellow", "Check mixed color uniformity", DISPLAY_TEST_SOLID, 0xFFFF00},
    {"Gray 50%", "Check mura and banding", DISPLAY_TEST_SOLID, 0x808080},
    {"RGB Bars", "Check channel order and color blocks", DISPLAY_TEST_BARS, 0},
    {"Gray Steps", "Check brightness steps", DISPLAY_TEST_GRAY, 0},
    {"Grid", "Check alignment and line defects", DISPLAY_TEST_GRID, 0},
    {"Checker", "Check pixel alternation and retention", DISPLAY_TEST_CHECKER, 0},
};

static lv_obj_t *display_test_root;
static lv_obj_t *display_test_layer;
static lv_obj_t *display_test_overlay;
static lv_obj_t *display_test_title;
static lv_obj_t *display_test_hint;
static lv_obj_t *display_test_hide_label;
static int display_test_index;
static int display_test_controls_visible = 1;

static void display_test_render(void);

static void display_test_style_full(lv_obj_t *obj)
{
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, ui_screen_width(), ui_screen_height());
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void display_test_add_rect(lv_obj_t *parent, int x, int y, int w, int h,
                                  uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(obj);
}

static void display_test_update_overlay(void)
{
    char title[96];
    const display_test_pattern_t *pattern;
    int count = (int)(sizeof(display_test_patterns) /
                      sizeof(display_test_patterns[0]));

    if(!display_test_title || !display_test_hint || !display_test_hide_label) {
        return;
    }

    pattern = &display_test_patterns[display_test_index];
    snprintf(title, sizeof(title), "%02d/%02d  %s", display_test_index + 1,
             count, ui_tr(pattern->title));
    lv_label_set_text(display_test_title, title);
    lv_label_set_text(display_test_hint, ui_tr(pattern->hint));
    lv_label_set_text(display_test_hide_label,
                      display_test_controls_visible ? ui_tr("Hide") :
                                                       ui_tr("Show"));
}

static void display_test_set_controls_visible(int visible)
{
    display_test_controls_visible = visible ? 1 : 0;
    if(display_test_overlay) {
        if(display_test_controls_visible) {
            lv_obj_clear_flag(display_test_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(display_test_overlay);
        } else {
            lv_obj_add_flag(display_test_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
    display_test_update_overlay();
    app_request_fast_refresh();
}

static void display_test_step(int delta)
{
    int count = (int)(sizeof(display_test_patterns) /
                      sizeof(display_test_patterns[0]));

    display_test_index = (display_test_index + delta + count) % count;
    display_test_render();
    app_request_fast_refresh();
}

static void display_test_layer_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_LONG_PRESSED) {
        display_test_set_controls_visible(!display_test_controls_visible);
    } else if(code == LV_EVENT_CLICKED) {
        if(!display_test_controls_visible) {
            display_test_set_controls_visible(1);
        } else {
            display_test_step(1);
        }
    }
}

static void display_test_prev_event(lv_event_t *event)
{
    (void)event;
    display_test_step(-1);
}

static void display_test_next_event(lv_event_t *event)
{
    (void)event;
    display_test_step(1);
}

static void display_test_hide_event(lv_event_t *event)
{
    (void)event;
    display_test_set_controls_visible(!display_test_controls_visible);
}

static void display_test_exit_event(lv_event_t *event)
{
    (void)event;
    app_nav_back();
}

static lv_obj_t *display_test_button(lv_obj_t *parent, int x, int y, int w,
                                     const char *text, uint32_t color,
                                     lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *lbl;

    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 46);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lbl = ui_label(btn, text, &lv_font_montserrat_18, color);
    lv_obj_center(lbl);
    ui_make_click_forwarder(lbl);
    if(!display_test_hide_label && cb == display_test_hide_event) {
        display_test_hide_label = lbl;
    }
    return btn;
}

static void display_test_create_overlay(void)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int overlay_w = screen_w - (landscape ? 72 : 32);
    int overlay_h = landscape ? 82 : 132;
    int button_y = landscape ? 18 : 76;
    int gap = 10;
    int button_w = landscape ? 112 : (overlay_w - 32 - gap * 3) / 4;
    int x = 16;

    display_test_overlay = lv_obj_create(display_test_root);
    lv_obj_set_size(display_test_overlay, overlay_w, overlay_h);
    lv_obj_align(display_test_overlay, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(display_test_overlay, 8, 0);
    lv_obj_set_style_border_width(display_test_overlay, 1, 0);
    lv_obj_set_style_border_color(display_test_overlay, lv_color_hex(0x374151),
                                  0);
    lv_obj_set_style_bg_color(display_test_overlay, lv_color_hex(0x020617), 0);
    lv_obj_set_style_bg_opa(display_test_overlay, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(display_test_overlay, 0, 0);
    lv_obj_clear_flag(display_test_overlay, LV_OBJ_FLAG_SCROLLABLE);

    display_test_title = ui_label(display_test_overlay, "", &lv_font_montserrat_20,
                                  0xF8FAFC);
    lv_obj_set_width(display_test_title, overlay_w - 32);
    lv_label_set_long_mode(display_test_title, LV_LABEL_LONG_DOT);
    lv_obj_align(display_test_title, LV_ALIGN_TOP_LEFT, 16, 12);

    display_test_hint = ui_label(display_test_overlay, "", &lv_font_montserrat_14,
                                 0xCBD5E1);
    lv_obj_set_width(display_test_hint, overlay_w - 32);
    lv_label_set_long_mode(display_test_hint, LV_LABEL_LONG_DOT);
    lv_obj_align(display_test_hint, LV_ALIGN_TOP_LEFT, 16,
                 landscape ? 44 : 42);

    if(!landscape) {
        display_test_button(display_test_overlay, x, button_y, button_w, "Prev",
                            0x93C5FD, display_test_prev_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Next",
                            0x86EFAC, display_test_next_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Hide",
                            0xFDE68A, display_test_hide_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Exit",
                            0xFDA4AF, display_test_exit_event);
    } else {
        x = overlay_w - 16 - button_w * 4 - gap * 3;
        display_test_button(display_test_overlay, x, button_y, button_w, "Prev",
                            0x93C5FD, display_test_prev_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Next",
                            0x86EFAC, display_test_next_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Hide",
                            0xFDE68A, display_test_hide_event);
        x += button_w + gap;
        display_test_button(display_test_overlay, x, button_y, button_w, "Exit",
                            0xFDA4AF, display_test_exit_event);
        lv_obj_set_width(display_test_title, x - 32);
        lv_obj_set_width(display_test_hint, x - 32);
    }

    (void)screen_h;
    display_test_update_overlay();
}

static void display_test_draw_bars(void)
{
    static const uint32_t colors[] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    };
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int count = (int)(sizeof(colors) / sizeof(colors[0]));
    int x = 0;

    for(int i = 0; i < count; ++i) {
        int next_x = (screen_w * (i + 1)) / count;
        display_test_add_rect(display_test_layer, x, 0, next_x - x, screen_h,
                              colors[i]);
        x = next_x;
    }
}

static void display_test_draw_gray(void)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int steps = 16;
    int x = 0;

    for(int i = 0; i < steps; ++i) {
        int next_x = (screen_w * (i + 1)) / steps;
        int v = (255 * i) / (steps - 1);
        uint32_t color = ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;

        display_test_add_rect(display_test_layer, x, 0, next_x - x, screen_h,
                              color);
        x = next_x;
    }
}

static void display_test_draw_grid(void)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int major = ui_is_landscape() ? 64 : 48;
    int minor = major / 2;

    lv_obj_set_style_bg_color(display_test_layer, lv_color_hex(0x000000), 0);
    for(int x = 0; x < screen_w; x += minor) {
        uint32_t color = (x % major) == 0 ? 0xFFFFFF : 0x334155;
        display_test_add_rect(display_test_layer, x, 0, 1, screen_h, color);
    }
    for(int y = 0; y < screen_h; y += minor) {
        uint32_t color = (y % major) == 0 ? 0xFFFFFF : 0x334155;
        display_test_add_rect(display_test_layer, 0, y, screen_w, 1, color);
    }
}

static void display_test_draw_checker(void)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int cell = ui_is_landscape() ? 44 : 36;

    lv_obj_set_style_bg_color(display_test_layer, lv_color_hex(0x000000), 0);
    for(int y = 0; y < screen_h; y += cell) {
        for(int x = 0; x < screen_w; x += cell) {
            int w = x + cell > screen_w ? screen_w - x : cell;
            int h = y + cell > screen_h ? screen_h - y : cell;
            uint32_t color = ((x / cell) + (y / cell)) & 1 ? 0xFFFFFF :
                                                                   0x000000;

            display_test_add_rect(display_test_layer, x, y, w, h, color);
        }
    }
}

static void display_test_render(void)
{
    const display_test_pattern_t *pattern =
        &display_test_patterns[display_test_index];

    if(!display_test_layer) {
        return;
    }

    lv_obj_clean(display_test_layer);
    lv_obj_set_style_bg_color(display_test_layer, lv_color_hex(pattern->color),
                              0);
    lv_obj_set_style_bg_opa(display_test_layer, LV_OPA_COVER, 0);

    switch(pattern->kind) {
    case DISPLAY_TEST_SOLID:
        break;
    case DISPLAY_TEST_BARS:
        display_test_draw_bars();
        break;
    case DISPLAY_TEST_GRAY:
        display_test_draw_gray();
        break;
    case DISPLAY_TEST_GRID:
        display_test_draw_grid();
        break;
    case DISPLAY_TEST_CHECKER:
        display_test_draw_checker();
        break;
    }

    display_test_update_overlay();
}

void ui_display_test_cleanup(void)
{
    display_test_root = NULL;
    display_test_layer = NULL;
    display_test_overlay = NULL;
    display_test_title = NULL;
    display_test_hint = NULL;
    display_test_hide_label = NULL;
    display_test_controls_visible = 1;
}

void ui_display_test_create(lv_obj_t *scr)
{
    display_test_root = lv_obj_create(scr);
    display_test_style_full(display_test_root);
    lv_obj_set_style_bg_color(display_test_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(display_test_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(display_test_root, LV_OBJ_FLAG_CLICKABLE);

    display_test_layer = lv_obj_create(display_test_root);
    display_test_style_full(display_test_layer);
    lv_obj_add_flag(display_test_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(display_test_layer, 0);
    lv_obj_add_event_cb(display_test_layer, display_test_layer_event,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(display_test_layer, display_test_layer_event,
                        LV_EVENT_LONG_PRESSED, NULL);

    display_test_create_overlay();
    display_test_render();
}
