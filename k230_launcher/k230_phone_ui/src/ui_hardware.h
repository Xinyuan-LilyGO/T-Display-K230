#ifndef K230_PHONE_UI_HARDWARE_H
#define K230_PHONE_UI_HARDWARE_H

#include "ui_common.h"

#ifndef K230_FAN_ENABLED
#define K230_FAN_ENABLED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ui_hardware_startup(void);
void ui_hardware_shutdown(void);
void ui_hardware_reboot_diag_dump(const char *tag);
typedef void (*ui_hardware_screen_toggle_cb_t)(void *user_data);
void ui_hardware_set_screen_toggle_cb(ui_hardware_screen_toggle_cb_t cb,
                                      void *user_data);
void ui_extension_keyboard_register_indev(void);
typedef void (*ui_extension_keyboard_key_cb_t)(int code, uint32_t key,
                                               int pressed, void *user_data);
int ui_extension_keyboard_enabled(void);
int ui_extension_keyboard_active(void);
int ui_extension_keyboard_set_enabled(int enabled);
int ui_extension_keyboard_base_available(void);
int ui_extension_keyboard_auto_detect_enabled(void);
void ui_extension_keyboard_set_auto_detect_enabled(int enabled);
int ui_extension_keyboard_auto_detect_interval_s(void);
void ui_extension_keyboard_set_auto_detect_interval_s(int seconds);
int ui_extension_keyboard_auto_rotate_enabled(void);
void ui_extension_keyboard_set_auto_rotate_enabled(int enabled);
int ui_extension_keyboard_probe_now(void);
int ui_hardware_keyboard_base_detected(void);
int ui_hardware_tca8418_detected(void);
int ui_hardware_xl9555_detected(void);
int ui_hardware_bq25896_detected(void);
int ui_hardware_bq27220_detected(void);
int ui_hardware_i2c4_scan(uint8_t found[128], char *status,
                          unsigned int status_len);
const char *ui_extension_keyboard_status(void);
void ui_extension_keyboard_focus_obj(lv_obj_t *obj);
void ui_extension_keyboard_set_key_cb(ui_extension_keyboard_key_cb_t cb,
                                      void *user_data);
int ui_extension_keyboard_pinyin_enabled(void);
void ui_extension_keyboard_set_pinyin_enabled(int enabled);
void ui_extension_keyboard_toggle_pinyin(void);
int ui_audio_output_set_external(int external);
int ui_audio_output_is_external(void);
int ui_audio_input_route_enter(const char *owner);
void ui_audio_input_route_leave(const char *owner);
int ui_amp_set_enabled(int enabled);
int ui_amp_is_enabled(void);
int ui_hardware_get_cpu_temp_c(double *temp_c);
int ui_hardware_get_aht20(double *temp_c, double *humidity_pct);
int ui_bq25896_get_usb_present(int *present, int *vbus_mv);
int ui_bq25896_get_power_state(int *usb_present, int *vbus_mv, int *vbat_mv);
int ui_bq25896_get_charge_state(int *charging, int *done);
int ui_bq27220_get_current_ma(int *current_ma);
int ui_bq27220_get_voltage_mv(int *voltage_mv);
int ui_bq27220_get_soc_pct(int *soc_pct);
int ui_hardware_consume_low_battery_shutdown(int *voltage_mv,
                                             int *threshold_mv,
                                             int *soc_pct);
int ui_hardware_screen_backlight_get(void);
int ui_hardware_keyboard_backlight_get(void);
int ui_hardware_boot0_screen_off(void);
void ui_hardware_set_screen_off(int off);
void ui_hardware_shutdown_backlights_apply(int screen_value,
                                           int keyboard_percent);
void ui_hardware_shutdown_backlights_step_apply(int screen_value,
                                                int keyboard_percent);
void ui_audio_settings_create(lv_obj_t *scr);
void ui_audio_output_create(lv_obj_t *scr);
void ui_fan_create(lv_obj_t *scr);
void ui_sensors_create(lv_obj_t *scr);
void ui_bq25896_create(lv_obj_t *scr);
void ui_battery_monitor_create(lv_obj_t *scr);
void ui_keyboard_settings_create(lv_obj_t *scr);
void ui_keyboard_hotkeys_create(lv_obj_t *scr);
void ui_keyboard_hotkey_action_create(lv_obj_t *scr);
void ui_keyboard_test_create(lv_obj_t *scr);
void ui_button_test_create(lv_obj_t *scr);
void ui_int0_test_create(lv_obj_t *scr);
void ui_xl9555_led_create(lv_obj_t *scr);
void ui_hardware_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
