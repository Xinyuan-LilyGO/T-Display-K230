#ifndef K230_PHONE_UI_I2C_SCAN_H
#define K230_PHONE_UI_I2C_SCAN_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_i2c_scan_create(lv_obj_t *scr);
void ui_i2c_scan_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
