#ifndef K230_PHONE_UI_MESHTASTIC_H
#define K230_PHONE_UI_MESHTASTIC_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_meshtastic_create(lv_obj_t *scr);
void ui_meshtastic_cleanup(void);
int ui_meshtastic_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
