#ifndef K230_PHONE_UI_INPUT_H
#define K230_PHONE_UI_INPUT_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_input_submit_cb_t)(const char *text, void *user_data);
typedef struct ui_input_inline ui_input_inline_t;
typedef void (*ui_input_inline_layout_cb_t)(int active, int reserved_h,
                                            void *user_data);

typedef struct {
    const char *title;
    const char *placeholder;
    const char *initial_text;
    int password_mode;
    size_t max_length;
    size_t min_length;
    const char *min_length_text;
    ui_input_submit_cb_t submit_cb;
    void *user_data;
    const char *submit_text;
    const char *cancel_text;
} ui_input_dialog_config_t;

void ui_input_dialog_open(const ui_input_dialog_config_t *config);
void ui_input_dialog_close_active(void);
void ui_input_set_soft_keyboard_enabled(int enabled);
int ui_input_soft_keyboard_enabled(void);
ui_input_inline_t *ui_input_inline_create(lv_obj_t *textarea,
                                          lv_obj_t *keyboard_parent,
                                          size_t max_length,
                                          ui_input_submit_cb_t submit_cb,
                                          void *submit_user_data,
                                          ui_input_inline_layout_cb_t layout_cb,
                                          void *layout_user_data);
void ui_input_inline_destroy(ui_input_inline_t *state);
void ui_input_inline_focus(ui_input_inline_t *state);
void ui_input_inline_submit(ui_input_inline_t *state);
void ui_input_inline_hide(ui_input_inline_t *state);
int ui_input_inline_is_active(ui_input_inline_t *state);
void ui_input_hide_inline_active(void);

#ifdef __cplusplus
}
#endif

#endif
