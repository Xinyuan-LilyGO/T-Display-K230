#ifndef K230_PHONE_UI_ICON_ASSETS_H
#define K230_PHONE_UI_ICON_ASSETS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

const lv_image_dsc_t *ui_icon_asset_for_name(const char *name);
const lv_image_dsc_t *ui_icon_asset_for_name_size(const char *name,
                                                  int size_px);

#ifdef __cplusplus
}
#endif

#endif
