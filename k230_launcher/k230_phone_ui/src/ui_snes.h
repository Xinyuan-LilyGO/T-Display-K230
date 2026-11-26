#ifndef K230_PHONE_UI_SNES_H
#define K230_PHONE_UI_SNES_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_snes_create(lv_obj_t *scr);
void ui_snes_cleanup(void);
int ui_snes_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
