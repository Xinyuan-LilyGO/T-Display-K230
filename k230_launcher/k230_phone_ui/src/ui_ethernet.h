#ifndef K230_PHONE_UI_ETHERNET_H
#define K230_PHONE_UI_ETHERNET_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_ethernet_create(lv_obj_t *scr);
void ui_ethernet_cleanup(void);
void ui_ethernet_apply_startup(void);

#ifdef __cplusplus
}
#endif

#endif
