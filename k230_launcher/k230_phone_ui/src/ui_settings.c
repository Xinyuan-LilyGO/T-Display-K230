#include "ui_settings.h"

#include "ui_hardware.h"

static lv_obj_t *settings_notice_overlay;
static lv_obj_t *settings_body;
static int32_t settings_saved_scroll_y;
static int settings_saved_scroll_valid;
static int settings_restore_scroll_on_create;

static void settings_restore_scroll_async(void *user_data)
{
    (void)user_data;
    if(settings_saved_scroll_valid && settings_body &&
       lv_obj_is_valid(settings_body)) {
        lv_obj_scroll_to_y(settings_body, settings_saved_scroll_y, LV_ANIM_OFF);
        settings_saved_scroll_valid = 0;
    }
    settings_restore_scroll_on_create = 0;
    app_request_fast_refresh();
}

void ui_settings_capture_scroll(void)
{
    if(settings_body && lv_obj_is_valid(settings_body)) {
        settings_saved_scroll_y = lv_obj_get_scroll_y(settings_body);
        settings_saved_scroll_valid = 1;
    }
}

void ui_settings_clear_saved_scroll(void)
{
    settings_saved_scroll_y = 0;
    settings_saved_scroll_valid = 0;
    settings_restore_scroll_on_create = 0;
}

void ui_settings_prepare_open(int restore_saved_scroll)
{
    if(restore_saved_scroll && settings_saved_scroll_valid) {
        settings_restore_scroll_on_create = 1;
        return;
    }

    ui_settings_clear_saved_scroll();
}

static void settings_refresh_async(void *user_data)
{
    (void)user_data;
    app_refresh_current_page();
}

static void settings_notice_close_event_cb(lv_event_t *event)
{
    (void)event;
    if(settings_notice_overlay && lv_obj_is_valid(settings_notice_overlay)) {
        lv_obj_delete(settings_notice_overlay);
    }
    settings_notice_overlay = NULL;
    app_request_fast_refresh();
}

static void settings_show_notice(const char *title, const char *detail,
                                 const char *hint)
{
    int panel_w = ui_is_landscape() ? 520 : ui_fit_width(lv_layer_top(), 24, 520);
    int panel_h = 292;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *btn;

    if(settings_notice_overlay && lv_obj_is_valid(settings_notice_overlay)) {
        lv_obj_delete(settings_notice_overlay);
    }

    settings_notice_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(settings_notice_overlay);
    lv_obj_set_style_bg_color(settings_notice_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(settings_notice_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(settings_notice_overlay, 0, 0);
    lv_obj_set_style_pad_all(settings_notice_overlay, 0, 0);
    lv_obj_clear_flag(settings_notice_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_panel(settings_notice_overlay, 0, 0, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(panel, 20, 0);

    label = ui_label(panel, title, &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(label, panel_w - 40);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = ui_label(panel, detail ? detail : "--", &lv_font_montserrat_18,
                     0xF5A524);
    lv_obj_set_width(label, panel_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 56);

    label = ui_label(panel, hint, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(label, panel_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 116);

    btn = ui_command_button(panel, panel_w - 172, panel_h - 74, 132, "OK",
                            0x3DA5FF);
    lv_obj_add_event_cb(btn, settings_notice_close_event_cb, LV_EVENT_CLICKED,
                        NULL);
    app_request_fast_refresh();
}

static void edge_back_switch_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    app_set_edge_back_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static int settings_section_header(lv_obj_t *parent, int y, const char *title,
                                   const char *subtitle)
{
    int w = ui_fit_width(parent, 24, 520);
    lv_obj_t *label = ui_label(parent, title, &lv_font_montserrat_20,
                               0xF2F5F8);

    lv_obj_set_pos(label, 32, y);
    lv_obj_set_width(label, w - 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    if(subtitle && subtitle[0]) {
        label = ui_label(parent, subtitle, &lv_font_montserrat_14, 0x9AA4AF);
        lv_obj_set_pos(label, 32, y + 30);
        lv_obj_set_width(label, w - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        return y + 64;
    }

    return y + 42;
}

static int settings_next_row_y(int y)
{
    return y + 108;
}

static void settings_switch_row(lv_obj_t *parent, int y, const char *symbol,
                                const char *title, const char *subtitle,
                                uint32_t color, int checked,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    int row_w = ui_fit_width(parent, 24, 520);
    int text_w = row_w > 220 ? row_w - 190 : 220;

    lv_obj_set_pos(row, 24, y);
    lv_obj_set_size(row, row_w, 94);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 54, 54);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = ui_label(icon_box, symbol, &lv_font_montserrat_20,
                              0xFFFFFF);
    lv_obj_center(icon);

    lv_obj_t *name = ui_label(row, title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 76, 14);

    lv_obj_t *detail = ui_label(row, subtitle, &lv_font_montserrat_16,
                                0x9AA4AF);
    lv_obj_set_width(detail, text_w);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 76, 48);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -2, 0);
    if(checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void ui_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int y = 8;

    ui_create_header(scr, "Settings");
    body = ui_page_body(scr, 144);
    settings_body = body;
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B1016), 0);

    y = settings_section_header(body, y, "Connections",
                                "Network, Bluetooth and modem");
    ui_settings_nav_row(body, y, "WiFi", "Wi-Fi",
                        "Scan networks and connect", 0x25C281, PAGE_WIFI);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "ETH", "Ethernet",
                        "Wired link status and IP", 0x3DA5FF, PAGE_ETHERNET);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "BT", "Bluetooth",
                        "Scan and connect BLE devices", 0x3B82F6,
                        PAGE_BLE);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "LTE", "Cellular",
                        "Cellular modem and GNSS", 0x60A5FA,
                        PAGE_CELLULAR);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "5G", "USB Modem",
                        "USB modem status and dialing", 0x38BDF8,
                        PAGE_USB_MODEM);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "Display & input",
                                "Screen, language and keyboard");
    ui_settings_nav_row(body, y, LV_SYMBOL_EYE_OPEN, "Display",
                        "Backlight, orientation and panel status", 0x8B5CF6,
                        PAGE_DISPLAY);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "A", "Language",
                        "English, Chinese and Japanese UI text", 0x22D3EE,
                        PAGE_LANGUAGE);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_REFRESH, "Date & time",
                        "NTP server and time zone", 0xEC4899,
                        PAGE_TIME);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "KEY", "Keyboard settings",
                        "Auto detect, interval and key test", 0xF97316,
                        PAGE_KEYBOARD_SETTINGS);
    y = settings_next_row_y(y);
    settings_switch_row(body, y, LV_SYMBOL_LEFT, "Edge back",
                        "Swipe from either edge to go back", 0x60A5FA,
                        app_edge_back_enabled(), edge_back_switch_event_cb);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "Sound & hardware",
                                "Audio, sensors and power");
    ui_settings_nav_row(body, y, LV_SYMBOL_AUDIO, "Audio",
                        "Volume and output route", 0x22D3EE,
                        PAGE_AUDIO_SETTINGS);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_AUDIO, "Notifications",
                        "Incoming message sound", 0xA78BFA,
                        PAGE_NOTIFICATION_SETTINGS);
    y = settings_next_row_y(y);
#if K230_FAN_ENABLED
    ui_settings_nav_row(body, y, "FAN", "Fan",
                        "Manual or automatic cooling", 0xF5A524,
                        PAGE_FAN);
    y = settings_next_row_y(y);
#endif
    ui_settings_nav_row(body, y, "AHT", "Sensors",
                        "AHT20 temperature and humidity", 0x25C281,
                        PAGE_SENSORS);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "CHG", "Charger",
                        "BQ25896 charge current and ADC", 0xF97316,
                        PAGE_BQ25896);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "BAT", "Battery",
                        "Battery gauge and runtime", 0xA3E635,
                        PAGE_BATTERY);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "System & about",
                                "Device information and diagnostics");
    ui_settings_nav_row(body, y, LV_SYMBOL_LIST, "System",
                        "Kernel, CPU, memory, storage", 0xF5A524,
                        PAGE_SYSTEM);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_WARNING, "About phone",
                        "Buildroot version and device identity", 0x94A3B8,
                        PAGE_ABOUT);

    if(settings_restore_scroll_on_create && settings_saved_scroll_valid) {
        lv_async_call(settings_restore_scroll_async, NULL);
    } else {
        settings_restore_scroll_on_create = 0;
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    }
}
