#ifndef K230_PHONE_UI_POWER_MANAGER_H
#define K230_PHONE_UI_POWER_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_POWER_STATE_ACTIVE = 0,
    UI_POWER_STATE_IDLE_DIM,
    UI_POWER_STATE_SCREEN_OFF,
    UI_POWER_STATE_APP_HEAVY,
    UI_POWER_STATE_LOW_POWER_BACKGROUND,
    UI_POWER_STATE_SHUTDOWN_FADE,
} ui_power_state_t;

typedef void (*ui_power_refresh_cb_t)(void *user_data);
typedef void (*ui_power_trace_cb_t)(const char *message, void *user_data);

void ui_power_manager_init(ui_power_refresh_cb_t refresh_cb,
                           ui_power_trace_cb_t trace_cb,
                           void *user_data);
void ui_power_manager_shutdown(void);

void ui_power_manager_note_activity(void);
void ui_power_manager_poll(void);

int ui_power_manager_touch_blocked(void);
int ui_power_manager_screen_off(void);
ui_power_state_t ui_power_manager_state(void);
const char *ui_power_manager_state_name(ui_power_state_t state);

int ui_power_manager_timeout_valid(int seconds);
int ui_power_manager_timeout_s(void);
void ui_power_manager_set_timeout_s(int seconds);
const char *ui_power_manager_timeout_label(int seconds);

void ui_power_manager_set_shutdown_fade(int active);
void ui_power_manager_set_app_heavy(int active, const char *owner);
void ui_power_manager_set_low_power_background(int active, const char *owner);

void ui_power_manager_toggle_screen_from_key(void);

#ifdef __cplusplus
}
#endif

#endif
