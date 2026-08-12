#ifndef K230_PHONE_UI_RTSP_H
#define K230_PHONE_UI_RTSP_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_rtsp_create(lv_obj_t *scr);
void ui_rtsp_cleanup(void);
int ui_rtsp_is_active(void);

#ifdef __cplusplus
}
#endif

#endif
