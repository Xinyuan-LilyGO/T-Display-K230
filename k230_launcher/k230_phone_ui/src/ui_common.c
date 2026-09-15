#include "ui_common.h"

#include "ui_haptic.h"
#include "ui_icons.h"
#include "ui_i18n.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UI_FONT_SIZE_PREF_KEY "display.font_size"
#define UI_PORTRAIT_SCROLL_REPAIR_INTERVAL_US 12000ULL
#define UI_PORTRAIT_SCROLL_REPAIR_DEFAULT_PERCENT 38
#define UI_PORTRAIT_SCROLL_REPAIR_EXTRA_PX 24
#define UI_NETWORK_ROUTE_LOG "/tmp/k230_route_manager.log"
#define UI_NETWORK_ROUTE_SYNC_MIN_US 6000000ULL
#define UI_NETWORK_ETH_DHCP_MIN_US 10000000ULL

typedef struct {
    const lv_font_t *fallback;
    uint32_t size;
    lv_font_t *font;
    lv_font_t *ja_font;
    lv_font_t *emoji_font;
} ui_font_slot_t;

static int ui_fonts_started;
static int ui_fonts_ready;
static int ui_fonts_ja_ready;
static int ui_fonts_emoji_ready;
static int ui_font_size_loaded;
static int ui_font_size_delta;
static char ui_font_size_mode_value[8] = "medium";
static uint64_t ui_network_route_last_us;
static char ui_network_route_last_preferred[16] = "";
static int ui_network_route_last_eth_carrier = -2;
static int ui_network_route_last_eth_ip;
static int ui_network_route_last_wifi_ip;
static uint64_t ui_network_eth_dhcp_last_us;

static ui_font_slot_t ui_font_slots[] = {
    { &lv_font_montserrat_12, 12, NULL, NULL },
    { &lv_font_montserrat_14, 14, NULL, NULL },
    { &lv_font_montserrat_16, 16, NULL, NULL },
    { &lv_font_montserrat_18, 18, NULL, NULL },
    { &lv_font_montserrat_20, 20, NULL, NULL },
    { &lv_font_montserrat_22, 22, NULL, NULL },
    { &lv_font_montserrat_24, 24, NULL, NULL },
    { &lv_font_montserrat_26, 26, NULL, NULL },
    { &lv_font_montserrat_28, 28, NULL, NULL },
    { &lv_font_montserrat_32, 32, NULL, NULL },
    { &lv_font_montserrat_48, 48, NULL, NULL },
};

static int ui_text_is_symbol(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t len;

    if(!s || !s[0]) {
        return 0;
    }

    len = strlen(text);
    if(len >= 3U && s[0] == 0xEF && s[1] >= 0x80 && s[1] <= 0x83) {
        return 1;
    }

    if(len >= 3U && s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0xA2) {
        return 1;
    }

    return 0;
}

static int ui_font_slot_index_for_fallback(const lv_font_t *fallback)
{
    size_t i;

    for(i = 0; i < sizeof(ui_font_slots) / sizeof(ui_font_slots[0]); i++) {
        if(ui_font_slots[i].fallback == fallback) {
            return (int)i;
        }
    }

    return 2;
}

static void ui_font_size_apply_mode(const char *mode)
{
    if(mode && strcmp(mode, "small") == 0) {
        snprintf(ui_font_size_mode_value, sizeof(ui_font_size_mode_value), "small");
        ui_font_size_delta = -1;
    } else if(mode && strcmp(mode, "large") == 0) {
        snprintf(ui_font_size_mode_value, sizeof(ui_font_size_mode_value), "large");
        ui_font_size_delta = 1;
    } else {
        snprintf(ui_font_size_mode_value, sizeof(ui_font_size_mode_value), "medium");
        ui_font_size_delta = 0;
    }
}

static void ui_font_size_load(void)
{
    char value[16];

    if(ui_font_size_loaded) {
        return;
    }
    ui_prefs_get(UI_FONT_SIZE_PREF_KEY, value, sizeof(value), "medium");
    ui_font_size_apply_mode(value);
    ui_font_size_loaded = 1;
}

static ui_font_slot_t *ui_font_slot_scaled_for_fallback(const lv_font_t *fallback)
{
    int index;
    int max_index = (int)(sizeof(ui_font_slots) / sizeof(ui_font_slots[0])) - 1;

    ui_font_size_load();
    index = ui_font_slot_index_for_fallback(fallback) + ui_font_size_delta;
    if(index < 0) {
        index = 0;
    }
    if(index > max_index) {
        index = max_index;
    }
    return &ui_font_slots[index];
}

#if LV_USE_FREETYPE
static int ui_fonts_create_from_path(const char *path, int font_kind)
{
    size_t i;
    int ready = 0;
    const char *name = "cjk";

    if(access(path, R_OK) != 0) {
        if(font_kind == 1) {
            name = "ja";
        } else if(font_kind == 2) {
            name = "emoji";
        }
        fprintf(stderr, "[font] %s missing path=%s\n", name, path);
        return -1;
    }

    for(i = 0; i < sizeof(ui_font_slots) / sizeof(ui_font_slots[0]); i++) {
        lv_font_t *font =
            lv_freetype_font_create(path,
                                    LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                    ui_font_slots[i].size,
                                    LV_FREETYPE_FONT_STYLE_NORMAL);
        if(font_kind == 1) {
            ui_font_slots[i].ja_font = font;
        } else if(font_kind == 2) {
            ui_font_slots[i].emoji_font = font;
        } else {
            ui_font_slots[i].font = font;
        }
        if(!font) {
            fprintf(stderr, "[font] %s create failed size=%u\n",
                    font_kind == 1 ? "ja" : font_kind == 2 ? "emoji" : "cjk",
                    (unsigned int)ui_font_slots[i].size);
        }
    }

    if(font_kind == 1) {
        ready = ui_font_slots[2].ja_font != NULL;
    } else if(font_kind == 2) {
        ready = ui_font_slots[2].emoji_font != NULL;
    } else {
        ready = ui_font_slots[2].font != NULL;
    }
    fprintf(stderr, "[font] %s %s path=%s\n",
            font_kind == 1 ? "ja" : font_kind == 2 ? "emoji" : "cjk",
            ready ? "ready" : "unavailable", path);
    return ready ? 0 : -1;
}

static void ui_fonts_link_fallbacks(void)
{
    size_t i;

    if(!ui_fonts_emoji_ready) {
        return;
    }

    for(i = 0; i < sizeof(ui_font_slots) / sizeof(ui_font_slots[0]); i++) {
        if(ui_font_slots[i].font && !ui_font_slots[i].font->fallback) {
            ui_font_slots[i].font->fallback = ui_font_slots[i].emoji_font;
        }
        if(ui_font_slots[i].ja_font && !ui_font_slots[i].ja_font->fallback) {
            ui_font_slots[i].ja_font->fallback = ui_font_slots[i].emoji_font;
        }
        if(ui_font_slots[i].emoji_font &&
           !ui_font_slots[i].emoji_font->fallback) {
            ui_font_slots[i].emoji_font->fallback = ui_font_slots[i].fallback;
        }
    }
}
#endif

int ui_fonts_init(void)
{
    if(ui_fonts_started) {
        return ui_fonts_ready ? 0 : -1;
    }
    ui_fonts_started = 1;

#if LV_USE_FREETYPE
    lv_result_t ft_init_result;

    ft_init_result = lv_freetype_init(512);
    if(ft_init_result != LV_RESULT_OK) {
        fprintf(stderr, "[font] freetype init returned %d, trying existing context\n",
                (int)ft_init_result);
    }

    ui_fonts_ready = ui_fonts_create_from_path(UI_CJK_FONT_PATH, 0) == 0;
    ui_fonts_ja_ready = ui_fonts_create_from_path(UI_JA_FONT_PATH, 1) == 0;
    ui_fonts_emoji_ready =
        ui_fonts_create_from_path(UI_EMOJI_FONT_PATH, 2) == 0;
    ui_fonts_link_fallbacks();
    return ui_fonts_ready ? 0 : -1;
#else
    fprintf(stderr, "[font] freetype disabled\n");
    return -1;
#endif
}

void ui_fonts_apply_theme(lv_display_t *disp)
{
#if LV_USE_FREETYPE && LV_USE_THEME_DEFAULT
    lv_theme_t *theme;

    if(!disp || !ui_fonts_ready) {
        return;
    }

    theme = lv_theme_default_init(disp, lv_color_hex(0x3DA5FF),
                                  lv_color_hex(0xEF4D5A), true,
                                  ui_font_for_text("CJK", &lv_font_montserrat_16));
    if(theme) {
        lv_display_set_theme(disp, theme);
    }
#else
    (void)disp;
#endif
}

const lv_font_t *ui_font_for_text(const char *text, const lv_font_t *fallback)
{
    ui_font_slot_t *slot;

    if(!fallback) {
        fallback = &lv_font_montserrat_16;
    }

    if(ui_text_is_symbol(text)) {
        return fallback;
    }

    slot = ui_font_slot_scaled_for_fallback(fallback);
    if(!ui_fonts_ready) {
        return slot->fallback ? slot->fallback : fallback;
    }
    if(ui_i18n_is_japanese() && ui_fonts_ja_ready && slot->ja_font) {
        return slot->ja_font;
    }
    return slot->font ? slot->font : fallback;
}

const char *ui_font_size_mode(void)
{
    ui_font_size_load();
    return ui_font_size_mode_value;
}

const char *ui_font_size_label(void)
{
    const char *mode = ui_font_size_mode();

    if(strcmp(mode, "small") == 0) {
        return "Small";
    }
    if(strcmp(mode, "large") == 0) {
        return "Large";
    }
    return "Medium";
}

int ui_set_font_size_mode(const char *mode)
{
    ui_font_size_apply_mode(mode);
    ui_font_size_loaded = 1;
    return ui_prefs_set(UI_FONT_SIZE_PREF_KEY, ui_font_size_mode_value);
}

int ui_fonts_cjk_ready(void)
{
    return ui_fonts_ready;
}

static void ui_style_button(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x222832), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x2D3744), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color)
{
    const char *shown = ui_tr(text);
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, shown);
    lv_obj_set_style_text_font(obj, ui_font_for_text(shown, font), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    return obj;
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    int fit_w = ui_fit_width(parent, x, w);

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, fit_w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(obj, 16, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

int ui_screen_width(void)
{
    int w = app_display_logical_width();

    return w > 0 ? w : SCREEN_W;
}

int ui_screen_height(void)
{
    int h = app_display_logical_height();

    return h > 0 ? h : SCREEN_H;
}

int ui_is_landscape(void)
{
    return ui_screen_width() > ui_screen_height();
}

int ui_page_side_margin(void)
{
    return ui_is_landscape() ? 44 : 24;
}

int ui_page_panel_x(void)
{
    return ui_page_side_margin();
}

int ui_page_panel_width(void)
{
    int w = ui_screen_width() - ui_page_side_margin() * 2;

    return w > 280 ? w : 280;
}

int ui_page_top_y(int default_top_y)
{
    if(ui_is_landscape() && app_edge_back_enabled() && default_top_y >= 140) {
        return 64;
    }
    return default_top_y;
}

int ui_body_height(int top_y)
{
    int h = ui_screen_height() - ui_page_top_y(top_y);

    return h > 180 ? h : 180;
}

int ui_content_width(void)
{
    int w = ui_screen_width() - ui_page_side_margin() * 2;

    return w > 520 ? w : 520;
}

int ui_inner_width(void)
{
    int w = ui_content_width() - 32;

    return w > 248 ? w : 248;
}

int ui_fit_width(lv_obj_t *parent, int x, int design_w)
{
    int parent_w;
    int right_margin;
    int fit_w;

    if(design_w <= 0 || ui_screen_width() <= SCREEN_W) {
        return design_w;
    }
    if(design_w != 520 && design_w != 488) {
        return design_w;
    }

    parent_w = parent ? lv_obj_get_width(parent) : 0;
    if(parent_w <= 0) {
        parent_w = ui_screen_width();
    }

    if(parent_w == ui_screen_width()) {
        right_margin = ui_page_side_margin();
    } else {
        right_margin = x >= 24 ? 24 : 32;
    }
    fit_w = parent_w - x - right_margin;
    return fit_w > design_w ? fit_w : design_w;
}

int ui_safe_content_width(lv_obj_t *parent, int design_w)
{
    int w = 0;

    if(parent) {
        int obj_w = lv_obj_get_width(parent);

        if(obj_w > 0) {
            int pad_l = lv_obj_get_style_pad_left(parent, 0);
            int pad_r = lv_obj_get_style_pad_right(parent, 0);

            w = obj_w - pad_l - pad_r;
        }
        if(w <= 0) {
            w = lv_obj_get_content_width(parent);
        }
    }
    if(w <= 0) {
        w = ui_fit_width(parent, 0, design_w > 0 ? design_w : 488);
    }
    if(w <= 0) {
        w = design_w > 0 ? design_w : 248;
    }
    return w;
}

static void ui_portrait_scroll_refresh_cb(lv_event_t *event)
{
    static uint64_t last_refresh_us;
    uint64_t now;
    lv_event_code_t code;
    lv_obj_t *screen;
    lv_obj_t *target;
    lv_area_t repair_area;
    const char *enabled_env;
    const char *percent_env;
    int repair_percent = UI_PORTRAIT_SCROLL_REPAIR_DEFAULT_PERCENT;
    int force_refresh;
    int screen_w;
    int screen_h;
    int repair_h;
    int y1;

    if(ui_is_landscape()) {
        return;
    }

    enabled_env = getenv("K230_PORTRAIT_SCROLL_REPAIR");
    if(!enabled_env || strcmp(enabled_env, "1") != 0) {
        return;
    }

    percent_env = getenv("K230_PORTRAIT_SCROLL_REPAIR_PERCENT");
    if(percent_env && percent_env[0]) {
        int value = atoi(percent_env);

        if(value >= 20 && value <= 70) {
            repair_percent = value;
        }
    }

    code = lv_event_get_code(event);
    force_refresh = (code == LV_EVENT_SCROLL_BEGIN ||
                     code == LV_EVENT_SCROLL_END);
    now = ui_monotonic_us();
    if(!force_refresh && last_refresh_us != 0ULL &&
       now - last_refresh_us < UI_PORTRAIT_SCROLL_REPAIR_INTERVAL_US) {
        return;
    }
    last_refresh_us = now;

    screen = lv_scr_act();
    target = lv_event_get_target(event);
    if(target) {
        lv_obj_invalidate(target);
    }
    screen_w = ui_screen_width();
    screen_h = ui_screen_height();
    repair_h = screen_h * repair_percent / 100;
    if(repair_h < 120) {
        repair_h = 120;
    }
    y1 = screen_h - repair_h - UI_PORTRAIT_SCROLL_REPAIR_EXTRA_PX;
    if(y1 < 0) {
        y1 = 0;
    }
    if(screen && screen_w > 0 && screen_h > 0) {
        lv_area_set(&repair_area, 0, y1, screen_w - 1, screen_h - 1);
        lv_obj_invalidate_area(screen, &repair_area);
    }
    app_request_fast_refresh();
}

void ui_make_scrollable(lv_obj_t *obj, int bottom_pad)
{
    if(!obj) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(obj, bottom_pad, 0);
    lv_obj_add_event_cb(obj, ui_portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(obj, ui_portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(obj, ui_portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_END, NULL);
}

lv_obj_t *ui_scroll_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    int parent_w = parent ? lv_obj_get_width(parent) : 0;
    int parent_h = parent ? lv_obj_get_height(parent) : 0;

    if(y >= 140 && parent_w == ui_screen_width() &&
       parent_h == ui_screen_height()) {
        y = ui_page_top_y(y);
    }

    lv_obj_t *obj = ui_panel(parent, x, y, w, h);

    ui_make_scrollable(obj, 48);
    return obj;
}

lv_obj_t *ui_page_body(lv_obj_t *scr, int top_y)
{
    top_y = ui_page_top_y(top_y);

    lv_obj_t *body = ui_scroll_panel(scr, 0, top_y, ui_screen_width(),
                                     ui_body_height(top_y));

    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, 48, 0);
    return body;
}

void ui_set_fullscreen(lv_obj_t *obj)
{
    if(!obj) {
        return;
    }

    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, ui_screen_width(), ui_screen_height());
}

void ui_make_click_forwarder(lv_obj_t *obj)
{
    if(obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

static void ui_nav_event_cb(lv_event_t *event)
{
    page_id_t page = (page_id_t)(intptr_t)lv_event_get_user_data(event);
    app_nav_to_page(page);
}

static void ui_settings_nav_event_cb(lv_event_t *event)
{
    page_id_t page = (page_id_t)(intptr_t)lv_event_get_user_data(event);
    app_nav_to_settings_page(page);
}

static void ui_back_event_cb(lv_event_t *event)
{
    (void)event;
    app_nav_back();
}

lv_obj_t *ui_command_button(lv_obj_t *parent, int x, int y, int w,
                            const char *text, uint32_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    int fit_w = w == 488 ? ui_fit_width(parent, x, w) : w;

    ui_style_button(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, fit_w, 60);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 6);
    ui_haptic_bind_touch(btn);

    lv_obj_t *lbl = ui_label(btn, text, &lv_font_montserrat_18, color);
    lv_obj_center(lbl);
    ui_make_click_forwarder(lbl);
    return btn;
}

lv_obj_t *ui_settings_nav_row(lv_obj_t *parent, int y, const char *symbol,
                              const char *title, const char *subtitle,
                              uint32_t color, page_id_t page)
{
    lv_obj_t *row = lv_obj_create(parent);
    int row_w = ui_fit_width(parent, 24, 520);

    ui_style_button(row);
    lv_obj_set_pos(row, 24, y);
    lv_obj_set_size(row, row_w, 94);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 6);
    ui_haptic_bind_touch(row);
    lv_obj_add_event_cb(row, ui_settings_nav_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)page);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 54, 54);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    ui_style_icon_box_for_page(icon_box, page, color);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(icon_box);

    ui_create_page_icon(icon_box, page, symbol, &lv_font_montserrat_22,
                        0xFFFFFF, 54);

    lv_obj_t *name = ui_label(row, title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 76, 14);
    ui_make_click_forwarder(name);

    lv_obj_t *detail = ui_label(row, subtitle, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(detail, row_w > 160 ? row_w - 160 : 360);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 76, 48);
    ui_make_click_forwarder(detail);

    lv_obj_t *arrow = ui_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    ui_make_click_forwarder(arrow);
    return row;
}

void ui_create_header(lv_obj_t *scr, const char *title)
{
    int title_x = 108;
    int landscape = ui_is_landscape();

    if(!app_edge_back_enabled()) {
        lv_obj_t *back = lv_obj_create(scr);
        ui_style_button(back);
        lv_obj_set_pos(back, 24, 74);
        lv_obj_set_size(back, 64, 54);
        lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(back, 8);
        lv_obj_add_event_cb(back, ui_back_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *back_icon = ui_label(back, LV_SYMBOL_LEFT,
                                       &lv_font_montserrat_24, 0xF2F5F8);
        lv_obj_center(back_icon);
        ui_make_click_forwarder(back_icon);
    } else {
        title_x = 24;
    }

    if(!landscape) {
        lv_obj_t *page_title = ui_label(scr, title, &lv_font_montserrat_28,
                                        0xF2F5F8);
        lv_obj_align(page_title, LV_ALIGN_TOP_LEFT, title_x, 82);
    }
}

void ui_info_row(lv_obj_t *parent, int y, const char *name, const char *value,
                 uint32_t value_color)
{
    lv_obj_t *left = ui_label(parent, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *right = ui_label(parent, value, &lv_font_montserrat_20, value_color);
    lv_obj_set_width(right, 260);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, y - 2);
}

void ui_info_row_inset(lv_obj_t *parent, int y, const char *name,
                       const char *value, uint32_t value_color,
                       int side_gap)
{
    int row_w = ui_safe_content_width(parent, 488);
    int value_w;
    int label_w;
    lv_obj_t *left;
    lv_obj_t *right;

    if(side_gap < 0) {
        side_gap = 0;
    }
    if(side_gap * 2 >= row_w) {
        side_gap = row_w > 120 ? 24 : 0;
    }

    value_w = row_w / 2;
    if(value_w > 320) {
        value_w = 320;
    }
    if(value_w < 150) {
        value_w = 150;
    }
    if(value_w > row_w - side_gap * 2 - 96) {
        value_w = row_w - side_gap * 2 - 96;
    }
    if(value_w < 96) {
        value_w = 96;
    }

    label_w = row_w - value_w - side_gap * 2 - 16;
    if(label_w < 80) {
        label_w = 80;
    }

    left = ui_label(parent, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(left, label_w);
    lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, side_gap, y);

    right = ui_label(parent, value, &lv_font_montserrat_20, value_color);
    lv_obj_set_width(right, value_w);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -side_gap, y - 2);
}

int ui_path_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

void ui_trim_text(char *text)
{
    size_t len;
    char *start;

    if(!text) {
        return;
    }

    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    len = strlen(text);
    while(len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

int ui_read_file_first_line(const char *path, char *buf, size_t len)
{
    FILE *fp;

    if(!path || !buf || len == 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if(!fp) {
        return -1;
    }

    if(!fgets(buf, len, fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    ui_trim_text(buf);
    return buf[0] ? 0 : -1;
}

int ui_read_cmd_first_line(const char *cmd, char *buf, size_t len)
{
    FILE *fp;

    if(!cmd || !buf || len == 0) {
        return -1;
    }

    fp = popen(cmd, "r");
    if(!fp) {
        return -1;
    }

    if(!fgets(buf, len, fp)) {
        pclose(fp);
        return -1;
    }

    pclose(fp);
    ui_trim_text(buf);
    return buf[0] ? 0 : -1;
}

int ui_read_iface_ip(const char *iface, char *buf, size_t len)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    int rc = -1;

    if(!iface || !buf || len == 0) {
        return -1;
    }

    if(getifaddrs(&ifaddr) != 0) {
        snprintf(buf, len, "--");
        return -1;
    }

    for(ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;

        if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
           strcmp(ifa->ifa_name, iface) != 0) {
            continue;
        }
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if(inet_ntop(AF_INET, &sin->sin_addr, buf, len)) {
            rc = 0;
            break;
        }
    }
    freeifaddrs(ifaddr);

    if(rc != 0) {
        snprintf(buf, len, "--");
    }
    return rc;
}

int ui_read_iface_carrier(const char *iface)
{
    char path[128];
    char carrier[8];

    if(!iface || !iface[0]) {
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface);
    if(ui_read_file_first_line(path, carrier, sizeof(carrier)) != 0) {
        return -1;
    }
    if(strcmp(carrier, "1") == 0) {
        return 1;
    }
    if(strcmp(carrier, "0") == 0) {
        return 0;
    }
    return -1;
}

void ui_read_iface_state(const char *iface, char *buf, size_t len,
                         uint32_t *color)
{
    char path[128];
    char oper[32];
    char ip[64];
    int carrier;

    if(!iface || !buf || len == 0) {
        return;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    if(!ui_path_exists(path)) {
        snprintf(buf, len, "Missing");
        if(color) {
            *color = 0x9AA4AF;
        }
        return;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
    if(ui_read_file_first_line(path, oper, sizeof(oper)) != 0) {
        snprintf(oper, sizeof(oper), "unknown");
    }

    carrier = ui_read_iface_carrier(iface);
    if(carrier == 0) {
        snprintf(buf, len, "No link");
        if(color) {
            *color = 0x9AA4AF;
        }
        return;
    }

    if(ui_read_iface_ip(iface, ip, sizeof(ip)) == 0) {
        snprintf(buf, len, "%s  %s", oper, ip);
        if(color) {
            *color = 0x25C281;
        }
    } else if(carrier == 1 || strcmp(oper, "up") == 0) {
        snprintf(buf, len, "%s  no IP", oper);
        if(color) {
            *color = 0xF5A524;
        }
    } else {
        snprintf(buf, len, "%s", oper);
        if(color) {
            *color = 0x9AA4AF;
        }
    }
}

static void ui_network_route_log(const char *reason, const char *preferred,
                                 int eth_carrier, int eth_has_ip,
                                 const char *eth_ip, int wifi_has_ip,
                                 const char *wifi_ip, int rc)
{
    FILE *fp = fopen(UI_NETWORK_ROUTE_LOG, "a");

    if(!fp) {
        return;
    }

    fprintf(fp,
            "[%llu] reason=%s preferred=%s eth_carrier=%d eth_ip=%s wifi_ip=%s rc=%d\n",
            (unsigned long long)ui_monotonic_us(),
            reason && reason[0] ? reason : "periodic",
            preferred && preferred[0] ? preferred : "none",
            eth_carrier,
            eth_has_ip && eth_ip && eth_ip[0] ? eth_ip : "--",
            wifi_has_ip && wifi_ip && wifi_ip[0] ? wifi_ip : "--",
            rc);
    fclose(fp);
}

static int ui_network_run_route_sync(const char *preferred,
                                     const char *other,
                                     int flush_other_ipv4)
{
    char cmd[768];
    int rc;

    if(!preferred || !preferred[0]) {
        return -1;
    }

    snprintf(cmd, sizeof(cmd),
             "PREF='%s'; OTHER='%s'; FLUSH_OTHER=%d; "
             "if command -v ip >/dev/null 2>&1; then "
             "GW=$(ip route show default dev \"$PREF\" 2>/dev/null | "
             "awk '/ via / {print $3; exit}'); "
             "[ -z \"$GW\" ] && "
             "GW=$(ip -4 addr show dev \"$PREF\" 2>/dev/null | "
             "awk '/ inet / {split($2,a,\"/\"); split(a[1],o,\".\"); "
             "if(o[1] != \"\") print o[1]\".\"o[2]\".\"o[3]\".1\"; exit}'); "
             "while ip route del default >/dev/null 2>&1; do :; done; "
             "[ \"$FLUSH_OTHER\" = \"1\" ] && "
             "ip addr flush dev \"$OTHER\" >/dev/null 2>&1 || true; "
             "[ -n \"$GW\" ] || exit 2; "
             "ip route replace default via \"$GW\" dev \"$PREF\"; "
             "else "
             "route del default dev \"$OTHER\" >/dev/null 2>&1 || true; "
             "fi",
             preferred, other && other[0] ? other : "",
             flush_other_ipv4 ? 1 : 0);

    rc = system(cmd);
    return ui_shell_exit_code(rc);
}

static void ui_network_start_eth_dhcp(const char *reason, uint64_t now)
{
    FILE *fp;
    char cmd[1024];
    int rc;

    if(now - ui_network_eth_dhcp_last_us < UI_NETWORK_ETH_DHCP_MIN_US) {
        return;
    }
    ui_network_eth_dhcp_last_us = now;

    snprintf(cmd, sizeof(cmd),
             "if [ -d /sys/class/net/%s ]; then "
             "rm -f /var/run/udhcpc.%s.pid; "
             "ifconfig %s up >/dev/null 2>&1 || true; "
             "(echo '[dhcp] start %s'; "
             "udhcpc -q -n -t 5 -p /var/run/udhcpc.%s.pid -i %s; "
             "echo '[dhcp] done rc='$?) >>/tmp/k230_eth_dhcp.log 2>&1 & "
             "fi",
             NET_ETH_IFACE, NET_ETH_IFACE, NET_ETH_IFACE, NET_ETH_IFACE,
             NET_ETH_IFACE, NET_ETH_IFACE);

    rc = ui_shell_exit_code(system(cmd));
    fp = fopen(UI_NETWORK_ROUTE_LOG, "a");
    if(fp) {
        fprintf(fp, "[%llu] reason=%s eth_dhcp_start rc=%d\n",
                (unsigned long long)now,
                reason && reason[0] ? reason : "periodic", rc);
        fclose(fp);
    }
}

void ui_network_sync_default_route(const char *reason)
{
    char eth_ip[64] = "";
    char wifi_ip[64] = "";
    char eth_path[128];
    char wifi_path[128];
    const char *preferred = NULL;
    const char *other = NULL;
    int flush_other_ipv4 = 0;
    int eth_present;
    int wifi_present;
    int eth_carrier = -1;
    int eth_has_ip;
    int wifi_has_ip;
    uint64_t now = ui_monotonic_us();
    int should_run = 0;
    int state_changed = 0;
    int rc = 0;

    snprintf(eth_path, sizeof(eth_path), "/sys/class/net/%s", NET_ETH_IFACE);
    snprintf(wifi_path, sizeof(wifi_path), "/sys/class/net/%s", NET_WIFI_IFACE);
    eth_present = ui_path_exists(eth_path);
    wifi_present = ui_path_exists(wifi_path);
    eth_carrier = eth_present ? ui_read_iface_carrier(NET_ETH_IFACE) : -1;
    eth_has_ip = eth_present &&
                 ui_read_iface_ip(NET_ETH_IFACE, eth_ip, sizeof(eth_ip)) == 0;
    wifi_has_ip = wifi_present &&
                  ui_read_iface_ip(NET_WIFI_IFACE, wifi_ip,
                                   sizeof(wifi_ip)) == 0;

    if(eth_present && eth_carrier == 1 && !eth_has_ip) {
        ui_network_start_eth_dhcp(reason, now);
    }

    if(eth_carrier == 1 && eth_has_ip) {
        preferred = NET_ETH_IFACE;
        other = NET_WIFI_IFACE;
    } else if(wifi_has_ip) {
        preferred = NET_WIFI_IFACE;
        other = NET_ETH_IFACE;
        flush_other_ipv4 = eth_present && eth_carrier != 1 && eth_has_ip;
    }

    if(!preferred) {
        return;
    }

    state_changed =
        strcmp(ui_network_route_last_preferred, preferred) != 0 ||
        ui_network_route_last_eth_carrier != eth_carrier ||
        ui_network_route_last_eth_ip != eth_has_ip ||
        ui_network_route_last_wifi_ip != wifi_has_ip;
    if(state_changed ||
       now - ui_network_route_last_us >= UI_NETWORK_ROUTE_SYNC_MIN_US) {
        should_run = 1;
    }
    if(!should_run) {
        return;
    }

    rc = ui_network_run_route_sync(preferred, other, flush_other_ipv4);
    snprintf(ui_network_route_last_preferred,
             sizeof(ui_network_route_last_preferred), "%s", preferred);
    ui_network_route_last_eth_carrier = eth_carrier;
    ui_network_route_last_eth_ip = eth_has_ip;
    ui_network_route_last_wifi_ip = wifi_has_ip;
    ui_network_route_last_us = now;
    if(state_changed || rc != 0) {
        ui_network_route_log(reason, preferred, eth_carrier, eth_has_ip,
                             eth_ip, wifi_has_ip, wifi_ip, rc);
    }
}

void ui_network_force_default_route(const char *reason)
{
    ui_network_route_last_us = 0;
    ui_network_route_last_preferred[0] = '\0';
    ui_network_route_last_eth_carrier = -2;
    ui_network_route_last_eth_ip = 0;
    ui_network_route_last_wifi_ip = 0;
    ui_network_sync_default_route(reason);
}

int ui_shell_exit_code(int rc)
{
    if(rc == -1) {
        return -1;
    }
    if(WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    return rc;
}

uint64_t ui_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
