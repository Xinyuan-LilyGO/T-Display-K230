#ifndef K230_PHONE_UI_CELLULAR_H
#define K230_PHONE_UI_CELLULAR_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_cellular_startup(void);
void ui_cellular_create(lv_obj_t *scr);
void ui_cellular_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
