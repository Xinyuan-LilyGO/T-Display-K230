#ifndef K230_PHONE_UI_ICONS_H
#define K230_PHONE_UI_ICONS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *ui_icon_path_for_page(page_id_t page);
int ui_page_has_asset_icon(page_id_t page);
void ui_style_icon_box_for_page(lv_obj_t *box, page_id_t page,
                                uint32_t fallback_color);
lv_obj_t *ui_create_page_icon(lv_obj_t *parent, page_id_t page,
                              const char *fallback_symbol,
                              const lv_font_t *fallback_font,
                              uint32_t fallback_color, int icon_px);

#ifdef __cplusplus
}
#endif

#endif
