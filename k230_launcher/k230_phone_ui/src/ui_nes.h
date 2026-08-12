#ifndef K230_PHONE_UI_NES_H
#define K230_PHONE_UI_NES_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_nes_create(lv_obj_t *scr);
void ui_nes_cleanup(void);
int ui_nes_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
