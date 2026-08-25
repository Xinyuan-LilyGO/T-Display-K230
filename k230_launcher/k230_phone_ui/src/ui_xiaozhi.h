#ifndef K230_PHONE_UI_XIAOZHI_H
#define K230_PHONE_UI_XIAOZHI_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_xiaozhi_create(lv_obj_t *scr);
void ui_xiaozhi_cleanup(void);
int ui_xiaozhi_handle_back(void);
void ui_xiaozhi_handle_voice_key(int pressed);

#ifdef __cplusplus
}
#endif

#endif
