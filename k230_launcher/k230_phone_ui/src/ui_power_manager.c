#include "ui_power_manager.h"

#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_prefs.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define POWER_MANAGER_LOG "/tmp/k230_power_manager.log"
#define DISPLAY_TIMEOUT_PREF_KEY "display.timeout_s"
#define DISPLAY_TIMEOUT_DEFAULT_S 0

typedef struct {
    pthread_mutex_t lock;
    ui_power_refresh_cb_t refresh_cb;
    ui_power_trace_cb_t trace_cb;
    void *user_data;
    ui_power_state_t state;
    uint64_t last_activity_us;
    int timeout_loaded;
    int timeout_s;
    int screen_off;
    int wake_pending;
    int saved_screen;
    int saved_keyboard;
    int shutdown_fade;
    int app_heavy_count;
    int low_power_background_count;
    char heavy_owner[48];
    char low_power_owner[48];
} ui_power_manager_t;

static ui_power_manager_t g_pm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .state = UI_POWER_STATE_ACTIVE,
    .timeout_s = DISPLAY_TIMEOUT_DEFAULT_S,
    .saved_screen = -1,
    .saved_keyboard = -1,
};

static void pm_log(const char *fmt, ...)
{
    FILE *fp;
    char message[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    fp = fopen(POWER_MANAGER_LOG, "a");
    if(fp) {
        fprintf(fp, "%llu %s\n",
                (unsigned long long)ui_monotonic_us(), message);
        fclose(fp);
    }

    if(g_pm.trace_cb) {
        g_pm.trace_cb(message, g_pm.user_data);
    }
}

const char *ui_power_manager_state_name(ui_power_state_t state)
{
    switch(state) {
    case UI_POWER_STATE_ACTIVE:
        return "ACTIVE";
    case UI_POWER_STATE_IDLE_DIM:
        return "IDLE_DIM";
    case UI_POWER_STATE_SCREEN_OFF:
        return "SCREEN_OFF";
    case UI_POWER_STATE_APP_HEAVY:
        return "APP_HEAVY";
    case UI_POWER_STATE_LOW_POWER_BACKGROUND:
        return "LOW_POWER_BACKGROUND";
    case UI_POWER_STATE_SHUTDOWN_FADE:
        return "SHUTDOWN_FADE";
    default:
        return "UNKNOWN";
    }
}

static ui_power_state_t pm_derive_state_locked(void)
{
    if(g_pm.shutdown_fade) {
        return UI_POWER_STATE_SHUTDOWN_FADE;
    }
    if(g_pm.screen_off) {
        return UI_POWER_STATE_SCREEN_OFF;
    }
    if(g_pm.app_heavy_count > 0) {
        return UI_POWER_STATE_APP_HEAVY;
    }
    if(g_pm.low_power_background_count > 0) {
        return UI_POWER_STATE_LOW_POWER_BACKGROUND;
    }
    return UI_POWER_STATE_ACTIVE;
}

static void pm_set_state_locked(ui_power_state_t state, const char *reason)
{
    ui_power_state_t old_state = g_pm.state;

    if(old_state == state) {
        return;
    }
    g_pm.state = state;
    pm_log("STATE %s -> %s reason=%s",
           ui_power_manager_state_name(old_state),
           ui_power_manager_state_name(state),
           reason ? reason : "-");
}

static void pm_refresh(void)
{
    if(g_pm.refresh_cb) {
        g_pm.refresh_cb(g_pm.user_data);
    }
}

int ui_power_manager_timeout_valid(int seconds)
{
    return seconds == 0 || seconds == 5 || seconds == 10 ||
           seconds == 30 || seconds == 60;
}

static void pm_load_timeout_locked(void)
{
    char value[16];
    int seconds;

    if(g_pm.timeout_loaded) {
        return;
    }
    ui_prefs_get(DISPLAY_TIMEOUT_PREF_KEY, value, sizeof(value), "0");
    seconds = atoi(value);
    if(!ui_power_manager_timeout_valid(seconds)) {
        seconds = DISPLAY_TIMEOUT_DEFAULT_S;
    }
    g_pm.timeout_s = seconds;
    g_pm.timeout_loaded = 1;
}

int ui_power_manager_timeout_s(void)
{
    int seconds;

    pthread_mutex_lock(&g_pm.lock);
    pm_load_timeout_locked();
    seconds = g_pm.timeout_s;
    pthread_mutex_unlock(&g_pm.lock);
    return seconds;
}

void ui_power_manager_set_timeout_s(int seconds)
{
    char value[16];

    if(!ui_power_manager_timeout_valid(seconds)) {
        seconds = DISPLAY_TIMEOUT_DEFAULT_S;
    }

    pthread_mutex_lock(&g_pm.lock);
    g_pm.timeout_s = seconds;
    g_pm.timeout_loaded = 1;
    g_pm.last_activity_us = ui_monotonic_us();
    pthread_mutex_unlock(&g_pm.lock);

    snprintf(value, sizeof(value), "%d", seconds);
    ui_prefs_set(DISPLAY_TIMEOUT_PREF_KEY, value);
    pm_log("TIMEOUT seconds=%d", seconds);
}

const char *ui_power_manager_timeout_label(int seconds)
{
    switch(seconds) {
    case 5:
        return "5 sec";
    case 10:
        return "10 sec";
    case 30:
        return "30 sec";
    case 60:
        return "60 sec";
    case 0:
    default:
        return "Never";
    }
}

void ui_power_manager_init(ui_power_refresh_cb_t refresh_cb,
                           ui_power_trace_cb_t trace_cb,
                           void *user_data)
{
    pthread_mutex_lock(&g_pm.lock);
    g_pm.refresh_cb = refresh_cb;
    g_pm.trace_cb = trace_cb;
    g_pm.user_data = user_data;
    g_pm.last_activity_us = ui_monotonic_us();
    g_pm.screen_off = ui_hardware_boot0_screen_off();
    g_pm.wake_pending = 0;
    g_pm.saved_screen = -1;
    g_pm.saved_keyboard = -1;
    g_pm.shutdown_fade = 0;
    g_pm.app_heavy_count = 0;
    g_pm.low_power_background_count = 0;
    pm_load_timeout_locked();
    pm_set_state_locked(pm_derive_state_locked(), "init");
    pthread_mutex_unlock(&g_pm.lock);
    pm_log("INIT timeout=%d screen_off=%d", ui_power_manager_timeout_s(),
           g_pm.screen_off);
}

void ui_power_manager_shutdown(void)
{
    pthread_mutex_lock(&g_pm.lock);
    g_pm.refresh_cb = NULL;
    g_pm.trace_cb = NULL;
    g_pm.user_data = NULL;
    pthread_mutex_unlock(&g_pm.lock);
    pm_log("SHUTDOWN");
}

void ui_power_manager_note_activity(void)
{
    pthread_mutex_lock(&g_pm.lock);
    g_pm.last_activity_us = ui_monotonic_us();
    if(g_pm.screen_off && !g_pm.shutdown_fade) {
        g_pm.wake_pending = 1;
    }
    pthread_mutex_unlock(&g_pm.lock);
}

static int pm_restore_from_screen_off(void)
{
    int screen;
    int keyboard;

    pthread_mutex_lock(&g_pm.lock);
    screen = g_pm.saved_screen > 0 ? g_pm.saved_screen : 80;
    keyboard = g_pm.saved_keyboard >= 0 ? g_pm.saved_keyboard :
               ui_hardware_keyboard_backlight_get();
    g_pm.wake_pending = 0;
    g_pm.screen_off = 0;
    g_pm.last_activity_us = ui_monotonic_us();
    pm_set_state_locked(pm_derive_state_locked(), "wake");
    pthread_mutex_unlock(&g_pm.lock);

    ui_hardware_set_screen_off(0);
    ui_hardware_shutdown_backlights_apply(screen, keyboard);
    pm_log("WAKE screen=%d keyboard=%d", screen, keyboard);
    pm_refresh();
    return 0;
}

static int pm_enter_screen_off(const char *reason)
{
    int saved_screen = ui_hardware_screen_backlight_get();
    int saved_keyboard = ui_hardware_keyboard_backlight_get();
    int log_screen;
    int log_keyboard;

    pthread_mutex_lock(&g_pm.lock);
    if(g_pm.screen_off || g_pm.shutdown_fade) {
        pthread_mutex_unlock(&g_pm.lock);
        return 0;
    }
    g_pm.saved_screen = saved_screen > 0 ? saved_screen : 80;
    g_pm.saved_keyboard = saved_keyboard >= 0 ? saved_keyboard : 0;
    log_screen = g_pm.saved_screen;
    log_keyboard = g_pm.saved_keyboard;
    g_pm.screen_off = 1;
    g_pm.wake_pending = 0;
    pm_set_state_locked(pm_derive_state_locked(), reason);
    pthread_mutex_unlock(&g_pm.lock);

    ui_hardware_set_screen_off(1);
    pm_log("SCREEN_OFF reason=%s saved_screen=%d saved_keyboard=%d",
           reason ? reason : "-", log_screen, log_keyboard);
    pm_refresh();
    return 0;
}

void ui_power_manager_toggle_screen_from_key(void)
{
    int screen_off;

    pthread_mutex_lock(&g_pm.lock);
    screen_off = g_pm.screen_off;
    pthread_mutex_unlock(&g_pm.lock);

    if(screen_off) {
        (void)pm_restore_from_screen_off();
    } else {
        (void)pm_enter_screen_off("boot0");
    }
}

void ui_power_manager_poll(void)
{
    int restore_pending;
    int timeout_s;
    int screen_off;
    int shutdown_fade;
    uint64_t now = ui_monotonic_us();
    uint64_t last_activity;
    int should_sleep = 0;

    pthread_mutex_lock(&g_pm.lock);
    pm_load_timeout_locked();
    if(g_pm.last_activity_us == 0) {
        g_pm.last_activity_us = now;
    }
    if(g_pm.shutdown_fade) {
        g_pm.last_activity_us = now;
        pthread_mutex_unlock(&g_pm.lock);
        return;
    }
    restore_pending = g_pm.wake_pending;
    if(restore_pending) {
        g_pm.wake_pending = 0;
    }
    timeout_s = g_pm.timeout_s;
    last_activity = g_pm.last_activity_us;
    screen_off = g_pm.screen_off;
    shutdown_fade = g_pm.shutdown_fade;
    if(!restore_pending && !screen_off && !shutdown_fade && timeout_s > 0 &&
       now >= last_activity &&
       now - last_activity >= (uint64_t)timeout_s * 1000000ULL) {
        should_sleep = 1;
    }
    pthread_mutex_unlock(&g_pm.lock);

    if(restore_pending) {
        (void)pm_restore_from_screen_off();
        return;
    }

    if(should_sleep) {
        (void)pm_enter_screen_off("timeout");
    }
}

int ui_power_manager_touch_blocked(void)
{
    int blocked;

    pthread_mutex_lock(&g_pm.lock);
    blocked = g_pm.screen_off || g_pm.shutdown_fade;
    pthread_mutex_unlock(&g_pm.lock);
    return blocked;
}

int ui_power_manager_screen_off(void)
{
    int screen_off;

    pthread_mutex_lock(&g_pm.lock);
    screen_off = g_pm.screen_off;
    pthread_mutex_unlock(&g_pm.lock);
    return screen_off;
}

ui_power_state_t ui_power_manager_state(void)
{
    ui_power_state_t state;

    pthread_mutex_lock(&g_pm.lock);
    state = g_pm.state;
    pthread_mutex_unlock(&g_pm.lock);
    return state;
}

void ui_power_manager_set_shutdown_fade(int active)
{
    pthread_mutex_lock(&g_pm.lock);
    g_pm.shutdown_fade = active ? 1 : 0;
    g_pm.last_activity_us = ui_monotonic_us();
    pm_set_state_locked(pm_derive_state_locked(),
                        active ? "shutdown-fade" : "shutdown-clear");
    pthread_mutex_unlock(&g_pm.lock);
    pm_log("SHUTDOWN_FADE active=%d", active ? 1 : 0);
}

void ui_power_manager_set_app_heavy(int active, const char *owner)
{
    pthread_mutex_lock(&g_pm.lock);
    if(active) {
        g_pm.app_heavy_count++;
        snprintf(g_pm.heavy_owner, sizeof(g_pm.heavy_owner), "%s",
                 owner ? owner : "-");
    } else if(g_pm.app_heavy_count > 0) {
        g_pm.app_heavy_count--;
        if(g_pm.app_heavy_count == 0) {
            g_pm.heavy_owner[0] = '\0';
        }
    }
    pm_set_state_locked(pm_derive_state_locked(),
                        active ? "heavy-enter" : "heavy-leave");
    pthread_mutex_unlock(&g_pm.lock);
    pm_log("APP_HEAVY active=%d count=%d owner=%s",
           active ? 1 : 0, g_pm.app_heavy_count, owner ? owner : "-");
}

void ui_power_manager_set_low_power_background(int active, const char *owner)
{
    pthread_mutex_lock(&g_pm.lock);
    if(active) {
        g_pm.low_power_background_count++;
        snprintf(g_pm.low_power_owner, sizeof(g_pm.low_power_owner), "%s",
                 owner ? owner : "-");
    } else if(g_pm.low_power_background_count > 0) {
        g_pm.low_power_background_count--;
        if(g_pm.low_power_background_count == 0) {
            g_pm.low_power_owner[0] = '\0';
        }
    }
    pm_set_state_locked(pm_derive_state_locked(),
                        active ? "background-enter" : "background-leave");
    pthread_mutex_unlock(&g_pm.lock);
    pm_log("LOW_POWER_BACKGROUND active=%d count=%d owner=%s",
           active ? 1 : 0, g_pm.low_power_background_count,
           owner ? owner : "-");
}
