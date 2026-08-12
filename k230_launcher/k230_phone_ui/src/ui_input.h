#ifndef K230_PHONE_UI_INPUT_H
#define K230_PHONE_UI_INPUT_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_input_submit_cb_t)(const char *text, void *user_data);

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

#ifdef __cplusplus
}
#endif

#endif
