#ifndef K230_PHONE_UI_HDMI_TEST_H
#define K230_PHONE_UI_HDMI_TEST_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_hdmi_test_create(lv_obj_t *scr);
void ui_hdmi_test_cleanup(void);
int ui_hdmi_test_restore_one_shot_boot(void);

#ifdef __cplusplus
}
#endif

#endif
