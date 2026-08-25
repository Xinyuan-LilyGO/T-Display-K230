#ifndef K230_PHONE_UI_COMMON_H
#define K230_PHONE_UI_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCREEN_W
#define SCREEN_W 568
#endif

#ifndef SCREEN_H
#define SCREEN_H 1232
#endif

#ifndef NET_WIFI_IFACE
#define NET_WIFI_IFACE "wlan0"
#endif

#ifndef NET_ETH_IFACE
#define NET_ETH_IFACE "eth0"
#endif

#ifndef NET_MAX_APS
#define NET_MAX_APS 10
#endif

#ifndef NET_SSID_MAX
#define NET_SSID_MAX 64
#endif

#ifndef NET_PASS_MAX
#define NET_PASS_MAX 64
#endif

#ifndef NET_STATUS_MAX
#define NET_STATUS_MAX 160
#endif

#ifndef UI_CJK_FONT_PATH
#define UI_CJK_FONT_PATH "/root/app/k230_phone_ui/fonts/ALIBABA-PUHUITI-MEDIUM.TTF"
#endif
#ifndef UI_JA_FONT_PATH
#define UI_JA_FONT_PATH "/root/app/k230_phone_ui/fonts/NotoSansJP-Regular.otf"
#endif
#ifndef UI_EMOJI_FONT_PATH
#define UI_EMOJI_FONT_PATH "/root/app/k230_phone_ui/fonts/NotoColorEmoji.ttf"
#endif

typedef enum {
    PAGE_HOME = 0,
    PAGE_CAMERA,
    PAGE_NETWORK,
    PAGE_WIFI,
    PAGE_WIFI_IPERF,
    PAGE_ETHERNET,
    PAGE_BLE,
    PAGE_MUSIC,
    PAGE_VIDEO,
    PAGE_NET_RADIO,
    PAGE_RECORDER,
    PAGE_MIC_FFT,
    PAGE_LORA,
    PAGE_MESHTASTIC,
    PAGE_LORA_FLRC,
    PAGE_HALOW,
    PAGE_LORAWAN,
    PAGE_NES,
    PAGE_SYSTEM,
    PAGE_DISPLAY,
    PAGE_DISPLAY_TEST,
    PAGE_LANGUAGE,
    PAGE_TIME,
    PAGE_AUDIO_SETTINGS,
    PAGE_AUDIO_OUTPUT,
    PAGE_NOTIFICATION_SETTINGS,
    PAGE_APP_STARTUP,
    PAGE_I2S_TEST,
    PAGE_I2C_SCAN,
    PAGE_HDMI_TEST,
    PAGE_FAN,
    PAGE_SENSORS,
    PAGE_BQ25896,
    PAGE_BATTERY,
    PAGE_KEYBOARD_SETTINGS,
    PAGE_KEYBOARD_HOTKEYS,
    PAGE_KEYBOARD_HOTKEY_ACTION,
    PAGE_KEYBOARD_TEST,
    PAGE_BUTTON_TEST,
    PAGE_INT0_TEST,
    PAGE_XL9555_TEST,
    PAGE_CELLULAR,
    PAGE_USB_MODEM,
    PAGE_FILES,
    PAGE_AI,
    PAGE_XIAOZHI,
    PAGE_RTSP,
    PAGE_TERMINAL,
    PAGE_ABOUT,
    PAGE_GALLERY,
    PAGE_SCREENSHOT,
    PAGE_TOUCH_TEST,
    PAGE_MOTION,
    PAGE_SETTINGS,
    PAGE_LOGS,
    PAGE_REBOOT,
    PAGE_NRF52840_DFU,
} page_id_t;

void app_nav_to_page(page_id_t page);
void app_nav_to_settings_page(page_id_t page);
void app_nav_back(void);
void app_take_screenshot(void);
void app_request_fast_refresh(void);
void app_refresh_current_page(void);
int app_current_page_is(page_id_t page);
void app_set_wifi_status(const char *state);
void app_set_ble_status(const char *state);
void app_refresh_status_bar(void);
void app_note_user_activity(void);
int app_edge_back_enabled(void);
void app_set_edge_back_enabled(int enabled);
void app_edge_back_cancel_gesture(uint32_t suppress_ms);
int app_display_rotation_degrees(void);
void app_set_display_rotation_degrees(int degrees);
int app_display_logical_width(void);
int app_display_logical_height(void);
int ui_screen_width(void);
int ui_screen_height(void);
int ui_is_landscape(void);
int ui_page_side_margin(void);
int ui_page_panel_x(void);
int ui_page_panel_width(void);
int ui_content_width(void);
int ui_inner_width(void);
int ui_page_top_y(int default_top_y);
int ui_body_height(int top_y);
int ui_fit_width(lv_obj_t *parent, int x, int design_w);
int ui_safe_content_width(lv_obj_t *parent, int design_w);

int ui_fonts_init(void);
void ui_fonts_apply_theme(lv_display_t *disp);
const lv_font_t *ui_font_for_text(const char *text, const lv_font_t *fallback);
int ui_fonts_cjk_ready(void);
const char *ui_font_size_mode(void);
const char *ui_font_size_label(void);
int ui_set_font_size_mode(const char *mode);

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color);
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *ui_scroll_panel(lv_obj_t *parent, int x, int y, int w, int h);
lv_obj_t *ui_page_body(lv_obj_t *scr, int top_y);
lv_obj_t *ui_command_button(lv_obj_t *parent, int x, int y, int w,
                            const char *text, uint32_t color);
lv_obj_t *ui_settings_nav_row(lv_obj_t *parent, int y, const char *symbol,
                              const char *title, const char *subtitle,
                              uint32_t color, page_id_t page);
void ui_create_header(lv_obj_t *scr, const char *title);
void ui_info_row(lv_obj_t *parent, int y, const char *name, const char *value,
                 uint32_t value_color);
void ui_info_row_inset(lv_obj_t *parent, int y, const char *name,
                       const char *value, uint32_t value_color,
                       int side_gap);
void ui_make_click_forwarder(lv_obj_t *obj);
void ui_make_scrollable(lv_obj_t *obj, int bottom_pad);
void ui_set_fullscreen(lv_obj_t *obj);

int ui_path_exists(const char *path);
void ui_trim_text(char *text);
int ui_read_file_first_line(const char *path, char *buf, size_t len);
int ui_read_cmd_first_line(const char *cmd, char *buf, size_t len);
int ui_read_iface_carrier(const char *iface);
int ui_read_iface_ip(const char *iface, char *buf, size_t len);
void ui_read_iface_state(const char *iface, char *buf, size_t len,
                         uint32_t *color);
void ui_network_sync_default_route(const char *reason);
void ui_network_force_default_route(const char *reason);
int ui_shell_exit_code(int rc);
uint64_t ui_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif
