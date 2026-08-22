#ifndef K230_PHONE_UI_NRF52840_DFU_H
#define K230_PHONE_UI_NRF52840_DFU_H

#include <stddef.h>

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_nrf52840_dfu_create(lv_obj_t *scr);
void ui_nrf52840_dfu_cleanup(void);
void ui_nrf52840_dfu_leave(void);
int ui_nrf52840_dfu_is_running(void);
int ui_nrf52840_dfu_preflight(char *message, size_t message_len);

#ifdef __cplusplus
}
#endif

#endif
