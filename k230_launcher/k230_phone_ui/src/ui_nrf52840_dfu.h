#ifndef K230_PHONE_UI_NRF52840_DFU_H
#define K230_PHONE_UI_NRF52840_DFU_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_nrf52840_dfu_create(lv_obj_t *scr);
void ui_nrf52840_dfu_cleanup(void);
int ui_nrf52840_dfu_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
