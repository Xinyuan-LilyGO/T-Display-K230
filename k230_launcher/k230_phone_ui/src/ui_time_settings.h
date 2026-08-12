#ifndef K230_PHONE_UI_TIME_SETTINGS_H
#define K230_PHONE_UI_TIME_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_time_settings_apply_startup(void);
void ui_time_settings_create(lv_obj_t *scr);
void ui_time_settings_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
