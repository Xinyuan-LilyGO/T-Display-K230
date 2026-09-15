#ifndef K230_PHONE_UI_QR_SCANNER_H
#define K230_PHONE_UI_QR_SCANNER_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_qr_scanner_create(lv_obj_t *scr);
void ui_qr_scanner_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
