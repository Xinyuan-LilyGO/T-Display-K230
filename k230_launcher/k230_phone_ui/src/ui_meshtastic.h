#ifndef K230_PHONE_UI_MESHTASTIC_H
#define K230_PHONE_UI_MESHTASTIC_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_meshtastic_create(lv_obj_t *scr);
void ui_meshtastic_cleanup(void);
int ui_meshtastic_handle_back(void);
void ui_meshtastic_startup(void);
int ui_meshtastic_autostart_enabled(void);
void ui_meshtastic_set_autostart_enabled(int enabled);
void ui_meshtastic_pause_for_radio_owner(const char *owner);
void ui_meshtastic_resume_after_radio_owner(void);
void ui_meshtastic_trigger_voice_key(void);

#ifdef __cplusplus
}
#endif

#endif
