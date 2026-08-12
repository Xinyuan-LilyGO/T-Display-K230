#ifndef K230_PHONE_UI_WIFI_H
#define K230_PHONE_UI_WIFI_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_wifi_create(lv_obj_t *scr);
void ui_wifi_cleanup(void);
void ui_wifi_autoconnect_start(void);

#ifdef __cplusplus
}
#endif

#endif
