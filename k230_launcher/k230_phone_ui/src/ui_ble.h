#ifndef K230_PHONE_UI_BLE_H
#define K230_PHONE_UI_BLE_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_ble_create(lv_obj_t *scr);
void ui_ble_cleanup(void);
int ui_ble_meshtastic_bridge_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
