#ifndef K230_PHONE_UI_PICOCLAW_H
#define K230_PHONE_UI_PICOCLAW_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_picoclaw_create(lv_obj_t *scr);
int ui_picoclaw_handle_back(void);
void ui_picoclaw_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
