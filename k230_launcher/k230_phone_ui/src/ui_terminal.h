#ifndef K230_PHONE_UI_TERMINAL_H
#define K230_PHONE_UI_TERMINAL_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_terminal_create(lv_obj_t *scr);
void ui_terminal_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
