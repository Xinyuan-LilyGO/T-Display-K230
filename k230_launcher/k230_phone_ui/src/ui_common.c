#include "ui_common.h"

#include "ui_i18n.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UI_FONT_SIZE_PREF_KEY "display.font_size"

typedef struct {
    const lv_font_t *fallback;
    uint32_t size;
    lv_font_t *font;
    lv_font_t *ja_font;
} ui_font_slot_t;

static int ui_fonts_started;
static int ui_fonts_ready;
static int ui_fonts_ja_ready;
static int ui_font_size_loaded;
static int ui_font_size_delta;
static char ui_font_size_mode_value[8] = "medium";

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
static int ui_fonts_create_from_path(const char *path, int japanese)
{
    size_t i;
    int ready = 0;

    if(access(path, R_OK) != 0) {
        fprintf(stderr, "[font] %s missing path=%s\n",
                japanese ? "ja" : "cjk", path);
        return -1;
    }

    for(i = 0; i < sizeof(ui_font_slots) / sizeof(ui_font_slots[0]); i++) {
        lv_font_t *font =
            lv_freetype_font_create(path,
                                    LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                    ui_font_slots[i].size,
                                    LV_FREETYPE_FONT_STYLE_NORMAL);
        if(japanese) {
            ui_font_slots[i].ja_font = font;
        } else {
            ui_font_slots[i].font = font;
        }
        if(!font) {
            fprintf(stderr, "[font] %s create failed size=%u\n",
                    japanese ? "ja" : "cjk",
                    (unsigned int)ui_font_slots[i].size);
        }
    }

    ready = japanese ? (ui_font_slots[2].ja_font != NULL) :
            (ui_font_slots[2].font != NULL);
    fprintf(stderr, "[font] %s %s path=%s\n",
            japanese ? "ja" : "cjk", ready ? "ready" : "unavailable", path);
    return ready ? 0 : -1;
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

void ui_make_scrollable(lv_obj_t *obj, int bottom_pad)
{
    if(!obj) {
        return;
    }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(obj, bottom_pad, 0);
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
    lv_obj_add_event_cb(row, ui_nav_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)page);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 54, 54);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(icon_box);

    lv_obj_t *icon = ui_label(icon_box, symbol, &lv_font_montserrat_22, 0xFFFFFF);
    lv_obj_center(icon);
    ui_make_click_forwarder(icon);

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
    char cmd[160];

    if(!iface || !buf || len == 0) {
        return -1;
    }

    snprintf(cmd, sizeof(cmd),
             "ip -4 addr show dev %s 2>/dev/null | awk '/inet / {print $2; exit}'",
             iface);
    if(ui_read_cmd_first_line(cmd, buf, len) == 0) {
        return 0;
    }

    snprintf(buf, len, "--");
    return -1;
}

void ui_read_iface_state(const char *iface, char *buf, size_t len,
                         uint32_t *color)
{
    char path[128];
    char oper[32];
    char carrier[8];
    char ip[64];

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

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface);
    if(ui_read_file_first_line(path, carrier, sizeof(carrier)) != 0) {
        snprintf(carrier, sizeof(carrier), "?");
    }

    if(ui_read_iface_ip(iface, ip, sizeof(ip)) == 0) {
        snprintf(buf, len, "%s  %s", oper, ip);
        if(color) {
            *color = 0x25C281;
        }
    } else if(strcmp(carrier, "1") == 0 || strcmp(oper, "up") == 0) {
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
