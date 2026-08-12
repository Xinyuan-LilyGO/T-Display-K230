#include "ui_hdmi_test.h"

#include "ui_i18n.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDMI_ACTIVE_DTB "/boot/k.dtb"
#define HDMI_PANEL_DTB "/boot/k230-canmv-rm69a10.dtb"
#define HDMI_OUTPUT_DTB "/boot/k230-canmv-rm69a10-hdmi.dtb"
#define HDMI_ONE_SHOT_MARKER "/boot/k230_hdmi_next_boot_only"
#define HDMI_CONFIRM_US 8000000ULL

typedef enum {
    HDMI_BOOT_NONE = 0,
    HDMI_BOOT_OUTPUT,
} hdmi_boot_target_t;

static pthread_mutex_t hdmi_action_lock = PTHREAD_MUTEX_INITIALIZER;

static lv_timer_t *hdmi_timer;
static lv_obj_t *hdmi_runtime_label;
static lv_obj_t *hdmi_next_boot_label;
static lv_obj_t *hdmi_connector_label;
static lv_obj_t *hdmi_dsi_label;
static lv_obj_t *hdmi_mode_label;
static lv_obj_t *hdmi_model_label;
static lv_obj_t *hdmi_action_label;
static lv_obj_t *hdmi_output_btn;

static uint64_t hdmi_status_last_us;
static hdmi_boot_target_t hdmi_confirm_target;
static uint64_t hdmi_confirm_deadline_us;
static int hdmi_action_busy;
static int hdmi_action_rc;
static char hdmi_action_text[192] = "Ready";

static void hdmi_set_action_locked(const char *text, int rc)
{
    snprintf(hdmi_action_text, sizeof(hdmi_action_text), "%s",
             text ? text : "Ready");
    hdmi_action_rc = rc;
}

int ui_hdmi_test_restore_one_shot_boot(void)
{
    char cmd[640];
    int rc;

    if(!ui_path_exists(HDMI_ONE_SHOT_MARKER)) {
        return 0;
    }

    if(!ui_path_exists(HDMI_PANEL_DTB)) {
        fprintf(stderr, "[hdmi] one-shot restore marker present but panel DTB missing\n");
        return -1;
    }

    snprintf(cmd, sizeof(cmd),
             "mount -o remount,rw /boot 2>/dev/null || true; "
             "cp -f %s %s && rm -f %s && sync",
             HDMI_PANEL_DTB, HDMI_ACTIVE_DTB, HDMI_ONE_SHOT_MARKER);
    rc = ui_shell_exit_code(system(cmd));
    if(rc == 0) {
        fprintf(stderr, "[hdmi] restored next boot to AMOLED after one-shot HDMI boot\n");
    } else {
        fprintf(stderr, "[hdmi] failed to restore AMOLED DTB after one-shot HDMI boot rc=%d\n",
                rc);
    }
    return rc;
}

static void hdmi_update_action_label(void)
{
    char text[sizeof(hdmi_action_text)];
    int busy;
    int rc;

    pthread_mutex_lock(&hdmi_action_lock);
    snprintf(text, sizeof(text), "%s", hdmi_action_text);
    busy = hdmi_action_busy;
    rc = hdmi_action_rc;
    pthread_mutex_unlock(&hdmi_action_lock);

    if(hdmi_action_label) {
        lv_label_set_text(hdmi_action_label, ui_tr(text));
        lv_obj_set_style_text_color(hdmi_action_label,
                                    lv_color_hex(busy ? 0xF5A524 :
                                                 (rc == 0 ? 0x9AA4AF : 0xEF4D5A)),
                                    0);
    }

    if(hdmi_output_btn) {
        if(busy) {
            lv_obj_add_state(hdmi_output_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(hdmi_output_btn, LV_STATE_DISABLED);
        }
    }
}

static int hdmi_files_equal(const char *a_path, const char *b_path)
{
    FILE *a;
    FILE *b;
    int result = 0;

    if(!a_path || !b_path) {
        return 0;
    }

    a = fopen(a_path, "rb");
    if(!a) {
        return 0;
    }
    b = fopen(b_path, "rb");
    if(!b) {
        fclose(a);
        return 0;
    }

    for(;;) {
        unsigned char abuf[4096];
        unsigned char bbuf[4096];
        size_t ar = fread(abuf, 1, sizeof(abuf), a);
        size_t br = fread(bbuf, 1, sizeof(bbuf), b);

        if(ar != br || memcmp(abuf, bbuf, ar) != 0) {
            result = 0;
            break;
        }
        if(ar < sizeof(abuf)) {
            result = feof(a) && feof(b);
            break;
        }
    }

    fclose(a);
    fclose(b);
    return result;
}

static int hdmi_find_connector(const char *needle, char *name, size_t name_len,
                               char *status, size_t status_len,
                               char *mode, size_t mode_len)
{
    DIR *dir;
    struct dirent *entry;

    if(name && name_len) {
        snprintf(name, name_len, "--");
    }
    if(status && status_len) {
        snprintf(status, status_len, "missing");
    }
    if(mode && mode_len) {
        snprintf(mode, mode_len, "--");
    }

    dir = opendir("/sys/class/drm");
    if(!dir) {
        return -1;
    }

    while((entry = readdir(dir)) != NULL) {
        char path[192];

        if(!strstr(entry->d_name, needle)) {
            continue;
        }

        if(name && name_len) {
            snprintf(name, name_len, "%s", entry->d_name);
        }

        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", entry->d_name);
        if(status && status_len &&
           ui_read_file_first_line(path, status, status_len) != 0) {
            snprintf(status, status_len, "unknown");
        }

        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", entry->d_name);
        if(mode && mode_len && ui_read_file_first_line(path, mode, mode_len) != 0) {
            snprintf(mode, mode_len, "No modes");
        }

        closedir(dir);
        return 0;
    }

    closedir(dir);
    return -1;
}

static void hdmi_label_set(lv_obj_t *label, const char *text, uint32_t color)
{
    if(!label) {
        return;
    }

    lv_label_set_text(label, ui_tr(text ? text : "--"));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static lv_obj_t *hdmi_value_row(lv_obj_t *parent, int y, const char *name)
{
    lv_obj_t *left = ui_label(parent, name, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_t *right;

    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, y);

    right = ui_label(parent, "--", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(right, 306);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, y - 2);
    return right;
}

static void hdmi_update_status(void)
{
    char model[128];
    char hdmi_name[64];
    char hdmi_status[32];
    char hdmi_mode[96];
    char dsi_name[64];
    char dsi_status[32];
    char dsi_mode[64];
    char text[192];
    const char *next_boot = "Unknown";
    uint32_t next_color = 0xF5A524;
    int hdmi_found;
    int dsi_found;
    int hdmi_connected;
    int dsi_connected;

    hdmi_found = hdmi_find_connector("HDMI", hdmi_name, sizeof(hdmi_name),
                                     hdmi_status, sizeof(hdmi_status),
                                     hdmi_mode, sizeof(hdmi_mode)) == 0;
    dsi_found = hdmi_find_connector("DSI", dsi_name, sizeof(dsi_name),
                                    dsi_status, sizeof(dsi_status),
                                    dsi_mode, sizeof(dsi_mode)) == 0;
    hdmi_connected = hdmi_found && strcmp(hdmi_status, "connected") == 0;
    dsi_connected = dsi_found && strcmp(dsi_status, "connected") == 0;

    if(ui_read_file_first_line("/proc/device-tree/model", model, sizeof(model)) != 0) {
        snprintf(model, sizeof(model), "--");
    }

    if(ui_path_exists(HDMI_ONE_SHOT_MARKER)) {
        next_boot = "AMOLED restore pending";
        next_color = 0xF5A524;
    } else if(hdmi_files_equal(HDMI_ACTIVE_DTB, HDMI_OUTPUT_DTB)) {
        next_boot = "HDMI boot";
        next_color = 0x60A5FA;
    } else if(hdmi_files_equal(HDMI_ACTIVE_DTB, HDMI_PANEL_DTB)) {
        next_boot = "AMOLED boot";
        next_color = 0x25C281;
    } else if(!ui_path_exists(HDMI_ACTIVE_DTB)) {
        next_boot = "Missing /boot/k.dtb";
        next_color = 0xEF4D5A;
    }

    if(hdmi_runtime_label) {
        if(hdmi_connected) {
            hdmi_label_set(hdmi_runtime_label, "HDMI active", 0x25C281);
        } else if(dsi_connected || dsi_found) {
            hdmi_label_set(hdmi_runtime_label, "AMOLED active", 0x25C281);
        } else {
            hdmi_label_set(hdmi_runtime_label, "Unknown", 0xF5A524);
        }
    }

    hdmi_label_set(hdmi_next_boot_label, next_boot, next_color);

    if(hdmi_connector_label) {
        snprintf(text, sizeof(text), "%s  %s",
                 hdmi_found ? hdmi_name : "HDMI", hdmi_found ? hdmi_status : "missing");
        lv_label_set_text(hdmi_connector_label, text);
        lv_obj_set_style_text_color(hdmi_connector_label,
                                    lv_color_hex(hdmi_connected ? 0x25C281 :
                                                 (hdmi_found ? 0xF5A524 : 0x9AA4AF)),
                                    0);
    }

    if(hdmi_dsi_label) {
        snprintf(text, sizeof(text), "%s  %s",
                 dsi_found ? dsi_name : "DSI", dsi_found ? dsi_status : "missing");
        lv_label_set_text(hdmi_dsi_label, text);
        lv_obj_set_style_text_color(hdmi_dsi_label,
                                    lv_color_hex(dsi_connected ? 0x25C281 :
                                                 (dsi_found ? 0xF5A524 : 0x9AA4AF)),
                                    0);
    }

    if(hdmi_mode_label) {
        lv_label_set_text(hdmi_mode_label, hdmi_found ? hdmi_mode : "No HDMI modes");
        lv_obj_set_style_text_color(hdmi_mode_label,
                                    lv_color_hex(hdmi_connected ? 0x60A5FA : 0x9AA4AF),
                                    0);
    }

    if(hdmi_model_label) {
        lv_label_set_text(hdmi_model_label, model);
    }
}

static void *hdmi_boot_switch_thread(void *arg)
{
    const char *src = HDMI_OUTPUT_DTB;
    char cmd[640];
    int rc;

    (void)arg;

    if(!ui_path_exists(src)) {
        pthread_mutex_lock(&hdmi_action_lock);
        hdmi_action_busy = 0;
        hdmi_set_action_locked("Target DTB missing", -1);
        pthread_mutex_unlock(&hdmi_action_lock);
        app_request_fast_refresh();
        return NULL;
    }

    snprintf(cmd, sizeof(cmd),
             "mount -o remount,rw /boot 2>/dev/null || true; "
             "printf 'restore=%s\\n' > %s && "
             "cp -f %s %s && sync && reboot",
             HDMI_PANEL_DTB, HDMI_ONE_SHOT_MARKER, src, HDMI_ACTIVE_DTB);
    rc = ui_shell_exit_code(system(cmd));

    pthread_mutex_lock(&hdmi_action_lock);
    hdmi_action_busy = 0;
    hdmi_set_action_locked(rc == 0 ? "HDMI next boot only, rebooting..." :
                           "Switch command failed", rc);
    pthread_mutex_unlock(&hdmi_action_lock);
    app_request_fast_refresh();
    return NULL;
}

static void hdmi_start_boot_switch(void)
{
    pthread_t thread;

    pthread_mutex_lock(&hdmi_action_lock);
    if(hdmi_action_busy) {
        pthread_mutex_unlock(&hdmi_action_lock);
        return;
    }
    hdmi_action_busy = 1;
    hdmi_confirm_target = HDMI_BOOT_NONE;
    hdmi_confirm_deadline_us = 0;
    hdmi_set_action_locked("HDMI next boot only, rebooting...", 0);
    pthread_mutex_unlock(&hdmi_action_lock);

    if(pthread_create(&thread, NULL, hdmi_boot_switch_thread, NULL) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&hdmi_action_lock);
        hdmi_action_busy = 0;
        hdmi_set_action_locked("Switch thread failed", -1);
        pthread_mutex_unlock(&hdmi_action_lock);
    }
    hdmi_update_action_label();
}

static void hdmi_boot_event_cb(lv_event_t *event)
{
    uint64_t now = ui_monotonic_us();

    (void)event;
    pthread_mutex_lock(&hdmi_action_lock);
    if(hdmi_action_busy) {
        pthread_mutex_unlock(&hdmi_action_lock);
        return;
    }
    if(hdmi_confirm_target == HDMI_BOOT_OUTPUT &&
       now < hdmi_confirm_deadline_us) {
        pthread_mutex_unlock(&hdmi_action_lock);
        hdmi_start_boot_switch();
        return;
    }
    hdmi_confirm_target = HDMI_BOOT_OUTPUT;
    hdmi_confirm_deadline_us = now + HDMI_CONFIRM_US;
    hdmi_set_action_locked("Tap again to confirm HDMI reboot", 0);
    pthread_mutex_unlock(&hdmi_action_lock);
    hdmi_update_action_label();
}

static void hdmi_timer_cb(lv_timer_t *timer)
{
    uint64_t now;

    (void)timer;

    now = ui_monotonic_us();
    if(hdmi_status_last_us == 0 || now - hdmi_status_last_us > 1000000ULL) {
        hdmi_status_last_us = now;
        hdmi_update_status();
    }

    pthread_mutex_lock(&hdmi_action_lock);
    if(hdmi_confirm_target != HDMI_BOOT_NONE && now > hdmi_confirm_deadline_us) {
        hdmi_confirm_target = HDMI_BOOT_NONE;
        hdmi_confirm_deadline_us = 0;
        if(!hdmi_action_busy) {
            hdmi_set_action_locked("Ready", 0);
        }
    }
    pthread_mutex_unlock(&hdmi_action_lock);
    hdmi_update_action_label();
}

void ui_hdmi_test_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *status;
    lv_obj_t *controls;
    lv_obj_t *label;

    ui_create_header(scr, "HDMI");

    body = ui_page_body(scr, 144);

    status = ui_panel(body, 24, 10, 520, 330);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x101418), 0);
    label = ui_label(status, "HDMI status", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    hdmi_runtime_label = hdmi_value_row(status, 54, "Runtime");
    hdmi_next_boot_label = hdmi_value_row(status, 94, "Next boot");
    hdmi_connector_label = hdmi_value_row(status, 134, "HDMI");
    hdmi_dsi_label = hdmi_value_row(status, 174, "DSI");
    hdmi_mode_label = hdmi_value_row(status, 214, "Mode");
    hdmi_model_label = hdmi_value_row(status, 254, "Board");

    controls = ui_panel(body, 24, 366, 520, 260);
    lv_obj_set_style_bg_color(controls, lv_color_hex(0x101418), 0);
    label = ui_label(controls, "HDMI boot is one-shot",
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = ui_label(controls, "HDMI output returns to AMOLED after one boot",
                     &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(label, ui_inner_width());
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 42);

    hdmi_output_btn = ui_command_button(controls, 0, 96, 488,
                                        "Reboot to HDMI once", 0x60A5FA);
    lv_obj_add_event_cb(hdmi_output_btn, hdmi_boot_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)HDMI_BOOT_OUTPUT);

    hdmi_action_label = ui_label(controls, "Ready", &lv_font_montserrat_18,
                                 0x9AA4AF);
    lv_obj_set_width(hdmi_action_label, ui_inner_width());
    lv_label_set_long_mode(hdmi_action_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(hdmi_action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hdmi_action_label, LV_ALIGN_TOP_MID, 0, 188);

    hdmi_update_status();
    hdmi_update_action_label();

    hdmi_status_last_us = ui_monotonic_us();
    hdmi_timer = lv_timer_create(hdmi_timer_cb, 500, NULL);
}

void ui_hdmi_test_cleanup(void)
{
    if(hdmi_timer) {
        lv_timer_delete(hdmi_timer);
        hdmi_timer = NULL;
    }

    hdmi_runtime_label = NULL;
    hdmi_next_boot_label = NULL;
    hdmi_connector_label = NULL;
    hdmi_dsi_label = NULL;
    hdmi_mode_label = NULL;
    hdmi_model_label = NULL;
    hdmi_action_label = NULL;
    hdmi_output_btn = NULL;
    hdmi_status_last_us = 0;
    hdmi_confirm_target = HDMI_BOOT_NONE;
    hdmi_confirm_deadline_us = 0;
}
