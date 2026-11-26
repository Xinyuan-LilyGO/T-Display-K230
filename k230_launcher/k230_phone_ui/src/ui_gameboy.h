#ifndef K230_PHONE_UI_GAMEBOY_H
#define K230_PHONE_UI_GAMEBOY_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_gameboy_create(lv_obj_t *scr);
void ui_gameboy_cleanup(void);
int ui_gameboy_handle_back(void);

#ifdef __cplusplus
}
#endif

#endif
