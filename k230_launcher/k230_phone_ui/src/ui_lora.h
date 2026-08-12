#ifndef K230_PHONE_UI_LORA_H
#define K230_PHONE_UI_LORA_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_lora_create(lv_obj_t *scr);
void ui_lora_cleanup(void);
void ui_lorawan_create(lv_obj_t *scr);
void ui_lorawan_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
