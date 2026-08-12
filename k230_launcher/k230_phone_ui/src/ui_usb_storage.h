#ifndef K230_PHONE_UI_USB_STORAGE_H
#define K230_PHONE_UI_USB_STORAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_usb_storage_create(lv_obj_t *scr);
void ui_usb_storage_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
