#ifndef K230_PHONE_UI_MEGADRIVE_H
#define K230_PHONE_UI_MEGADRIVE_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_megadrive_create(lv_obj_t *scr);
void ui_megadrive_cleanup(void);
int ui_megadrive_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
