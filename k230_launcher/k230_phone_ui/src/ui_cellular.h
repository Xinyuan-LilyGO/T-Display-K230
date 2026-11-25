#ifndef K230_PHONE_UI_CELLULAR_H
#define K230_PHONE_UI_CELLULAR_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_cellular_startup(void);
int ui_cellular_lte_signal_level(void);
int ui_cellular_handle_back(void);
void ui_cellular_create(lv_obj_t *scr);
void ui_cellular_cleanup(void);

#if K230_PAGE_SMOKE_TEST
void ui_cellular_flow_test_start(void);
int ui_cellular_flow_test_poll(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
