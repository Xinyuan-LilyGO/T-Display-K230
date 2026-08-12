#include "ui_language.h"

#include "ui_i18n.h"

#include <stdint.h>
#include <string.h>

static void language_refresh_async(void *user_data)
{
    (void)user_data;
    app_refresh_current_page();
}

static void language_event_cb(lv_event_t *event)
{
    const char *language = (const char *)lv_event_get_user_data(event);

    if(!language) {
        return;
    }

    ui_i18n_set_language(language);
    lv_async_call(language_refresh_async, NULL);
}

static lv_obj_t *language_row(lv_obj_t *parent, int y, const char *title,
                              const char *subtitle, const char *language,
                              uint32_t color)
{
    int selected = strcmp(ui_i18n_language(), language) == 0;
    lv_obj_t *row = lv_obj_create(parent);
    int row_w = ui_fit_width(parent, 24, 520);

    lv_obj_set_pos(row, 24, y);
    lv_obj_set_size(row, row_w, 116);
    lv_obj_set_style_bg_color(row,
                              lv_color_hex(selected ? color : 0x171D24), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x242C36), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(row, 2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, selected ? 0 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_pad_all(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 6);
    lv_obj_add_event_cb(row, language_event_cb, LV_EVENT_CLICKED,
                        (void *)language);

    lv_obj_t *name = ui_label(row, title, &lv_font_montserrat_24,
                              selected ? 0xFFFFFF : 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 6);
    ui_make_click_forwarder(name);

    lv_obj_t *detail = ui_label(row, subtitle, &lv_font_montserrat_16,
                                selected ? 0xEAF8FF : 0x9AA4AF);
    lv_obj_set_width(detail, row_w > 130 ? row_w - 130 : 390);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 0, 48);
    ui_make_click_forwarder(detail);

    if(selected) {
        lv_obj_t *mark = ui_label(row, LV_SYMBOL_OK, &lv_font_montserrat_24,
                                  0xFFFFFF);
        lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
        ui_make_click_forwarder(mark);
    }

    return row;
}

void ui_language_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;

    ui_create_header(scr, "Language");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    title = ui_label(body, "Preferred language", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);

    language_row(body, 86, "English", "Use English UI text", UI_LANG_EN,
                 0x3DA5FF);
    language_row(body, 222, "Chinese", "Use Chinese UI text", UI_LANG_ZH,
                 0x25C281);
    language_row(body, 358, "Japanese", "Use Japanese UI text", UI_LANG_JA,
                 0xF5A524);
}
