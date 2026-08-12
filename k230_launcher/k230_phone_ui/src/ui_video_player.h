#ifndef K230_PHONE_UI_VIDEO_PLAYER_H
#define K230_PHONE_UI_VIDEO_PLAYER_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_video_player_create(lv_obj_t *scr);
void ui_video_player_cleanup(void);
int ui_video_player_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
