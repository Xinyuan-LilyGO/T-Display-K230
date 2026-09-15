#ifndef K230_PHONE_UI_HAPTIC_H
#define K230_PHONE_UI_HAPTIC_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_haptic_init(void);
void ui_haptic_shutdown(void);
int ui_haptic_available(void);
const char *ui_haptic_status(void);
int ui_haptic_effect(void);
const char *ui_haptic_effect_name(int effect);
void ui_haptic_set_effect(int effect);
int ui_haptic_keyboard_enabled(void);
void ui_haptic_set_keyboard_enabled(int enabled);
int ui_haptic_touch_enabled(void);
void ui_haptic_set_touch_enabled(int enabled);
void ui_haptic_play_touch(void);
void ui_haptic_play_keyboard(void);
void ui_haptic_play_test(void);
void ui_haptic_bind_touch(lv_obj_t *obj);
void ui_haptic_settings_create(lv_obj_t *scr);

#ifdef __cplusplus
}
#endif

#endif
