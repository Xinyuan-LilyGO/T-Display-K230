#ifndef K230_PHONE_UI_SETTINGS_H
#define K230_PHONE_UI_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_settings_create(lv_obj_t *scr);
void ui_startup_settings_create(lv_obj_t *scr);
void ui_settings_capture_scroll(void);
void ui_settings_clear_saved_scroll(void);
void ui_settings_prepare_open(int restore_saved_scroll);

#ifdef __cplusplus
}
#endif

#endif
