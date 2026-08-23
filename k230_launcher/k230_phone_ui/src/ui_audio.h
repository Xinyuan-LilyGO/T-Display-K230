#ifndef K230_PHONE_UI_AUDIO_H
#define K230_PHONE_UI_AUDIO_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_music_create(lv_obj_t *scr);
void ui_net_radio_create(lv_obj_t *scr);
void ui_recorder_create(lv_obj_t *scr);
void ui_notification_settings_create(lv_obj_t *scr);
void ui_audio_cleanup(void);
void ui_audio_stop_for_exclusive_app(const char *reason);
void ui_audio_apply_startup_defaults(void);
int ui_audio_get_volume_value(void);
int ui_audio_get_volume_max(void);
void ui_audio_set_volume_value(int value, int force);
void ui_audio_play_notification(void);

#ifdef __cplusplus
}
#endif

#endif
