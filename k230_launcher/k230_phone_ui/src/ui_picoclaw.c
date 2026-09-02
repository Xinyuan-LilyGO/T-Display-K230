#include "ui_picoclaw.h"

#include "ui_common.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PICOCLAW_SCRIPT "/root/app/k230_phone_ui/k230_picoclaw_ctl.sh"
#define PICOCLAW_LOG "/tmp/k230_picoclaw_ui.log"
#define PICOCLAW_MODEL_NAME_KEY "picoclaw.model_name"
#define PICOCLAW_MODEL_ID_KEY "picoclaw.model_id"
#define PICOCLAW_API_BASE_KEY "picoclaw.api_base"
#define PICOCLAW_API_KEY_KEY "picoclaw.api_key"
#define PICOCLAW_OUTPUT_MAX 4096

typedef enum {
    PICOCLAW_ACTION_STATUS = 0,
    PICOCLAW_ACTION_INSTALL,
    PICOCLAW_ACTION_SAVE_CONFIG,
    PICOCLAW_ACTION_ASK,
    PICOCLAW_ACTION_GATEWAY_START,
    PICOCLAW_ACTION_GATEWAY_STOP,
    PICOCLAW_ACTION_LOG,
} picoclaw_action_t;

typedef enum {
    PICOCLAW_FIELD_MODEL_NAME = 0,
    PICOCLAW_FIELD_MODEL_ID,
    PICOCLAW_FIELD_API_BASE,
    PICOCLAW_FIELD_API_KEY,
    PICOCLAW_FIELD_PROMPT,
} picoclaw_field_t;

typedef struct {
    picoclaw_action_t action;
    char text[1024];
} picoclaw_request_t;

static pthread_mutex_t picoclaw_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *picoclaw_timer;
static lv_obj_t *picoclaw_status_label;
static lv_obj_t *picoclaw_version_label;
static lv_obj_t *picoclaw_gateway_label;
static lv_obj_t *picoclaw_network_label;
static lv_obj_t *picoclaw_url_label;
static lv_obj_t *picoclaw_config_label;
static lv_obj_t *picoclaw_output_label;
static lv_obj_t *picoclaw_install_btn;
static lv_obj_t *picoclaw_save_btn;
static lv_obj_t *picoclaw_ask_btn;
static lv_obj_t *picoclaw_gateway_start_btn;
static lv_obj_t *picoclaw_gateway_stop_btn;
static lv_obj_t *picoclaw_log_btn;

static int picoclaw_busy;
static int picoclaw_result_ready;
static int picoclaw_last_rc;
static int picoclaw_installed;
static int picoclaw_gateway_running;
static int picoclaw_config_ready;
static int picoclaw_network_ready;
static char picoclaw_status_text[160] = "Ready";
static char picoclaw_output_text[PICOCLAW_OUTPUT_MAX] = "";
static char picoclaw_version_text[256] = "not installed";
static char picoclaw_gateway_text[80] = "stopped";
static char picoclaw_network_text[80] = "unknown";
static char picoclaw_config_text[80] = "unknown";
static char picoclaw_url_text[160] = "http://<board-ip>:18790";

static void picoclaw_load_pref(char *out, size_t len, const char *key,
                               const char *fallback)
{
    if(!out || len == 0) {
        return;
    }
    if(ui_prefs_get(key, out, len, fallback) != 0 || !out[0]) {
        snprintf(out, len, "%s", fallback ? fallback : "");
    }
}

static int picoclaw_shell_quote(char *dst, size_t dst_len, const char *src)
{
    size_t used = 0;
    const char *p;

    if(!dst || dst_len == 0) {
        return -1;
    }
    dst[0] = '\0';
    if(used + 1 >= dst_len) {
        return -1;
    }
    dst[used++] = '\'';
    for(p = src ? src : ""; *p; p++) {
        if(*p == '\'') {
            if(used + 4 >= dst_len) {
                return -1;
            }
            dst[used++] = '\'';
            dst[used++] = '\\';
            dst[used++] = '\'';
            dst[used++] = '\'';
        } else {
            if(used + 1 >= dst_len) {
                return -1;
            }
            dst[used++] = *p;
        }
    }
    if(used + 1 >= dst_len) {
        return -1;
    }
    dst[used++] = '\'';
    dst[used] = '\0';
    return 0;
}

static void picoclaw_append_output(char *dst, size_t dst_len, const char *src)
{
    size_t used;
    size_t add;

    if(!dst || dst_len == 0 || !src) {
        return;
    }
    used = strlen(dst);
    add = strlen(src);
    if(used + add + 1U >= dst_len) {
        size_t keep = dst_len / 2U;

        if(used > keep) {
            memmove(dst, dst + used - keep, keep + 1U);
            used = strlen(dst);
        }
    }
    if(used < dst_len - 1U) {
        snprintf(dst + used, dst_len - used, "%s", src);
    }
}

static int picoclaw_run_capture(const char *cmd, char *out, size_t out_len)
{
    FILE *fp;
    char line[512];
    int rc;

    if(out && out_len > 0) {
        out[0] = '\0';
    }
    fp = popen(cmd, "r");
    if(!fp) {
        if(out && out_len > 0) {
            snprintf(out, out_len, "ERROR: popen failed");
        }
        return -1;
    }
    while(fgets(line, sizeof(line), fp)) {
        if(out && out_len > 0) {
            picoclaw_append_output(out, out_len, line);
        }
    }
    rc = pclose(fp);
    return ui_shell_exit_code(rc);
}

static const char *picoclaw_find_value(const char *text, const char *key,
                                       char *out, size_t out_len)
{
    const char *p;
    size_t key_len;
    size_t i = 0;

    if(!text || !key || !out || out_len == 0) {
        return NULL;
    }
    key_len = strlen(key);
    p = text;
    while(*p) {
        if(strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1U;
            while(*p && *p != '\n' && i + 1U < out_len) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return out;
        }
        while(*p && *p != '\n') {
            p++;
        }
        if(*p == '\n') {
            p++;
        }
    }
    return NULL;
}

static void picoclaw_apply_status_locked(const char *output)
{
    char value[256];

    if(!output) {
        return;
    }
    if(picoclaw_find_value(output, "installed", value, sizeof(value))) {
        picoclaw_installed = strcmp(value, "yes") == 0;
    }
    if(picoclaw_find_value(output, "version", value, sizeof(value))) {
        snprintf(picoclaw_version_text, sizeof(picoclaw_version_text), "%s",
                 value);
    }
    if(picoclaw_find_value(output, "gateway", value, sizeof(value))) {
        picoclaw_gateway_running = strcmp(value, "running") == 0;
        snprintf(picoclaw_gateway_text, sizeof(picoclaw_gateway_text), "%s",
                 value);
    }
    if(picoclaw_find_value(output, "network", value, sizeof(value))) {
        picoclaw_network_ready = strcmp(value, "ready") == 0;
        snprintf(picoclaw_network_text, sizeof(picoclaw_network_text), "%s",
                 value);
    }
    if(picoclaw_find_value(output, "config", value, sizeof(value))) {
        picoclaw_config_ready = strcmp(value, "ready") == 0;
        snprintf(picoclaw_config_text, sizeof(picoclaw_config_text), "%s",
                 value);
    }
    if(picoclaw_find_value(output, "url", value, sizeof(value))) {
        snprintf(picoclaw_url_text, sizeof(picoclaw_url_text), "%s", value);
    }
}

static int picoclaw_build_command(picoclaw_request_t *req, char *cmd,
                                  size_t cmd_len)
{
    char q0[1400];
    char q1[320];
    char q2[640];
    char q3[640];
    char q4[1200];
    char model_name[128];
    char model_id[256];
    char api_base[256];
    char api_key[512];

    if(!req || !cmd || cmd_len == 0) {
        return -1;
    }

    switch(req->action) {
    case PICOCLAW_ACTION_STATUS:
        snprintf(cmd, cmd_len, "%s status", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_INSTALL:
        snprintf(cmd, cmd_len, "%s install", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_GATEWAY_START:
        snprintf(cmd, cmd_len, "%s gateway-start", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_GATEWAY_STOP:
        snprintf(cmd, cmd_len, "%s gateway-stop", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_LOG:
        snprintf(cmd, cmd_len, "%s log", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_ASK:
        if(picoclaw_shell_quote(q0, sizeof(q0), req->text) != 0) {
            return -1;
        }
        snprintf(cmd, cmd_len, "%s ask %s", PICOCLAW_SCRIPT, q0);
        return 0;
    case PICOCLAW_ACTION_SAVE_CONFIG:
        picoclaw_load_pref(model_name, sizeof(model_name),
                           PICOCLAW_MODEL_NAME_KEY, "k230-agent");
        picoclaw_load_pref(model_id, sizeof(model_id), PICOCLAW_MODEL_ID_KEY,
                           "openai/gpt-4o-mini");
        picoclaw_load_pref(api_base, sizeof(api_base), PICOCLAW_API_BASE_KEY,
                           "https://api.openai.com/v1");
        picoclaw_load_pref(api_key, sizeof(api_key), PICOCLAW_API_KEY_KEY, "");
        if(picoclaw_shell_quote(q1, sizeof(q1), model_name) != 0 ||
           picoclaw_shell_quote(q2, sizeof(q2), model_id) != 0 ||
           picoclaw_shell_quote(q3, sizeof(q3), api_base) != 0 ||
           picoclaw_shell_quote(q4, sizeof(q4), api_key) != 0) {
            return -1;
        }
        snprintf(cmd, cmd_len, "%s save-config %s %s %s %s",
                 PICOCLAW_SCRIPT, q1, q2, q3, q4);
        return 0;
    default:
        return -1;
    }
}

static void picoclaw_status_for_action(picoclaw_action_t action, char *out,
                                       size_t len)
{
    const char *text = "Working...";

    switch(action) {
    case PICOCLAW_ACTION_STATUS:
        text = "Refreshing...";
        break;
    case PICOCLAW_ACTION_INSTALL:
        text = "Installing PicoClaw...";
        break;
    case PICOCLAW_ACTION_SAVE_CONFIG:
        text = "Saving config...";
        break;
    case PICOCLAW_ACTION_ASK:
        text = "Sending prompt...";
        break;
    case PICOCLAW_ACTION_GATEWAY_START:
        text = "Starting gateway...";
        break;
    case PICOCLAW_ACTION_GATEWAY_STOP:
        text = "Stopping gateway...";
        break;
    case PICOCLAW_ACTION_LOG:
        text = "Reading log...";
        break;
    default:
        break;
    }
    snprintf(out, len, "%s", text);
}

static void *picoclaw_worker(void *arg)
{
    picoclaw_request_t *req = (picoclaw_request_t *)arg;
    char cmd[2400];
    char output[PICOCLAW_OUTPUT_MAX];
    char status[160];
    int rc;

    if(!req) {
        return NULL;
    }
    if(picoclaw_build_command(req, cmd, sizeof(cmd)) != 0) {
        snprintf(output, sizeof(output), "ERROR: command build failed\n");
        rc = -1;
    } else {
        rc = picoclaw_run_capture(cmd, output, sizeof(output));
    }

    picoclaw_status_for_action(req->action, status, sizeof(status));

    pthread_mutex_lock(&picoclaw_lock);
    picoclaw_busy = 0;
    picoclaw_result_ready = 1;
    picoclaw_last_rc = rc;
    if(req->action == PICOCLAW_ACTION_ASK) {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 rc == 0 ? "Reply received" : "Prompt failed");
    } else if(req->action == PICOCLAW_ACTION_LOG) {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 "Log updated");
    } else if(rc == 0) {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 "Done");
    } else {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                 "%s rc=%d", "Action failed", rc);
    }
    snprintf(picoclaw_output_text, sizeof(picoclaw_output_text), "%s",
             output[0] ? output : status);
    picoclaw_apply_status_locked(output);
    pthread_mutex_unlock(&picoclaw_lock);

    free(req);
    app_request_fast_refresh();
    return NULL;
}

static void picoclaw_start_action(picoclaw_action_t action, const char *text)
{
    pthread_t thread;
    picoclaw_request_t *req;
    char status[160];

    pthread_mutex_lock(&picoclaw_lock);
    if(picoclaw_busy) {
        pthread_mutex_unlock(&picoclaw_lock);
        return;
    }
    picoclaw_busy = 1;
    picoclaw_result_ready = 1;
    picoclaw_status_for_action(action, status, sizeof(status));
    snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s", status);
    pthread_mutex_unlock(&picoclaw_lock);

    req = calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&picoclaw_lock);
        picoclaw_busy = 0;
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                 "%s", "Out of memory");
        pthread_mutex_unlock(&picoclaw_lock);
        return;
    }
    req->action = action;
    snprintf(req->text, sizeof(req->text), "%s", text ? text : "");

    if(pthread_create(&thread, NULL, picoclaw_worker, req) == 0) {
        pthread_detach(thread);
    } else {
        free(req);
        pthread_mutex_lock(&picoclaw_lock);
        picoclaw_busy = 0;
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                 "%s", "Thread start failed");
        pthread_mutex_unlock(&picoclaw_lock);
    }
}

static void picoclaw_update_buttons(int busy)
{
    lv_obj_t *buttons[] = {
        picoclaw_install_btn,
        picoclaw_save_btn,
        picoclaw_ask_btn,
        picoclaw_gateway_start_btn,
        picoclaw_gateway_stop_btn,
        picoclaw_log_btn,
    };
    size_t i;

    for(i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        if(!buttons[i]) {
            continue;
        }
        if(busy) {
            lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(buttons[i], LV_STATE_DISABLED);
        }
    }
    if(!picoclaw_installed && !busy) {
        if(picoclaw_ask_btn) {
            lv_obj_add_state(picoclaw_ask_btn, LV_STATE_DISABLED);
        }
        if(picoclaw_gateway_start_btn) {
            lv_obj_add_state(picoclaw_gateway_start_btn, LV_STATE_DISABLED);
        }
    }
    if(picoclaw_gateway_running) {
        if(picoclaw_gateway_start_btn) {
            lv_obj_add_state(picoclaw_gateway_start_btn, LV_STATE_DISABLED);
        }
    } else if(!busy && picoclaw_gateway_stop_btn) {
        lv_obj_add_state(picoclaw_gateway_stop_btn, LV_STATE_DISABLED);
    }
}

static void picoclaw_update_ui(void)
{
    int busy;
    int rc;
    char status[160];
    char version[256];
    char gateway[80];
    char network[80];
    char config[80];
    char url[160];
    char output[PICOCLAW_OUTPUT_MAX];

    pthread_mutex_lock(&picoclaw_lock);
    busy = picoclaw_busy;
    rc = picoclaw_last_rc;
    snprintf(status, sizeof(status), "%s", picoclaw_status_text);
    snprintf(version, sizeof(version), "%s", picoclaw_version_text);
    snprintf(gateway, sizeof(gateway), "%s", picoclaw_gateway_text);
    snprintf(network, sizeof(network), "%s", picoclaw_network_text);
    snprintf(config, sizeof(config), "%s", picoclaw_config_text);
    snprintf(url, sizeof(url), "%s", picoclaw_url_text);
    snprintf(output, sizeof(output), "%s", picoclaw_output_text);
    picoclaw_result_ready = 0;
    pthread_mutex_unlock(&picoclaw_lock);

    if(picoclaw_status_label) {
        lv_label_set_text(picoclaw_status_label, ui_tr(status));
        lv_obj_set_style_text_color(picoclaw_status_label,
                                    lv_color_hex(busy ? 0xF5A524 :
                                                 (rc == 0 ? 0x25C281 :
                                                  0xF5A524)), 0);
    }
    if(picoclaw_version_label) {
        lv_label_set_text(picoclaw_version_label, version);
    }
    if(picoclaw_gateway_label) {
        lv_label_set_text(picoclaw_gateway_label, ui_tr(gateway));
        lv_obj_set_style_text_color(picoclaw_gateway_label,
                                    lv_color_hex(picoclaw_gateway_running ?
                                                 0x25C281 : 0x9AA4AF), 0);
    }
    if(picoclaw_network_label) {
        lv_label_set_text(picoclaw_network_label, ui_tr(network));
        lv_obj_set_style_text_color(picoclaw_network_label,
                                    lv_color_hex(picoclaw_network_ready ?
                                                 0x25C281 : 0xF5A524), 0);
    }
    if(picoclaw_config_label) {
        lv_label_set_text(picoclaw_config_label, ui_tr(config));
        lv_obj_set_style_text_color(picoclaw_config_label,
                                    lv_color_hex(picoclaw_config_ready ?
                                                 0x25C281 : 0xF5A524), 0);
    }
    if(picoclaw_url_label) {
        lv_label_set_text(picoclaw_url_label, url);
    }
    if(picoclaw_output_label) {
        lv_label_set_text(picoclaw_output_label,
                          output[0] ? output : ui_tr("No output yet"));
    }

    picoclaw_update_buttons(busy);
}

static void picoclaw_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    picoclaw_update_ui();
}

static void picoclaw_action_event_cb(lv_event_t *event)
{
    picoclaw_action_t action =
        (picoclaw_action_t)(intptr_t)lv_event_get_user_data(event);

    picoclaw_start_action(action, NULL);
}

static void picoclaw_field_submit_cb(const char *text, void *user_data)
{
    picoclaw_field_t field = (picoclaw_field_t)(intptr_t)user_data;

    switch(field) {
    case PICOCLAW_FIELD_MODEL_NAME:
        ui_prefs_set(PICOCLAW_MODEL_NAME_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_MODEL_ID:
        ui_prefs_set(PICOCLAW_MODEL_ID_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_API_BASE:
        ui_prefs_set(PICOCLAW_API_BASE_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_API_KEY:
        ui_prefs_set(PICOCLAW_API_KEY_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_PROMPT:
        picoclaw_start_action(PICOCLAW_ACTION_ASK, text ? text : "");
        return;
    default:
        break;
    }

    pthread_mutex_lock(&picoclaw_lock);
    snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
             "%s", "Setting saved");
    pthread_mutex_unlock(&picoclaw_lock);
    app_request_fast_refresh();
}

static void picoclaw_config_event_cb(lv_event_t *event)
{
    picoclaw_field_t field =
        (picoclaw_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t cfg;
    char value[512];

    memset(&cfg, 0, sizeof(cfg));
    switch(field) {
    case PICOCLAW_FIELD_MODEL_NAME:
        picoclaw_load_pref(value, sizeof(value), PICOCLAW_MODEL_NAME_KEY,
                           "k230-agent");
        cfg.title = "Model name";
        cfg.placeholder = "k230-agent";
        break;
    case PICOCLAW_FIELD_MODEL_ID:
        picoclaw_load_pref(value, sizeof(value), PICOCLAW_MODEL_ID_KEY,
                           "openai/gpt-4o-mini");
        cfg.title = "Model ID";
        cfg.placeholder = "openai/gpt-4o-mini";
        break;
    case PICOCLAW_FIELD_API_BASE:
        picoclaw_load_pref(value, sizeof(value), PICOCLAW_API_BASE_KEY,
                           "https://api.openai.com/v1");
        cfg.title = "API base";
        cfg.placeholder = "https://api.openai.com/v1";
        break;
    case PICOCLAW_FIELD_API_KEY:
        picoclaw_load_pref(value, sizeof(value), PICOCLAW_API_KEY_KEY, "");
        cfg.title = "API key";
        cfg.placeholder = "sk-...";
        cfg.password_mode = 1;
        break;
    case PICOCLAW_FIELD_PROMPT:
        value[0] = '\0';
        cfg.title = "Ask PicoClaw";
        cfg.placeholder = "Type a prompt";
        cfg.min_length = 1;
        cfg.min_length_text = "Input is too short";
        break;
    default:
        return;
    }

    cfg.initial_text = value;
    cfg.max_length = field == PICOCLAW_FIELD_PROMPT ? 900 : 480;
    cfg.submit_cb = picoclaw_field_submit_cb;
    cfg.user_data = (void *)(intptr_t)field;
    cfg.submit_text = field == PICOCLAW_FIELD_PROMPT ? "Send" : "Save";
    cfg.cancel_text = "Cancel";
    ui_input_dialog_open(&cfg);
}

static void picoclaw_make_info_pair(lv_obj_t *parent, int x, int y, int w,
                                    const char *name, lv_obj_t **value_out,
                                    uint32_t color)
{
    lv_obj_t *name_label = ui_label(parent, name, &lv_font_montserrat_16,
                                    0x9AA4AF);
    lv_obj_t *value_label = ui_label(parent, "--", &lv_font_montserrat_18,
                                     color);
    int name_w = w > 360 ? 140 : 120;
    int value_w = w - name_w - 12;

    if(value_w < 120) {
        value_w = 120;
    }
    lv_obj_set_pos(name_label, x, y);
    lv_obj_set_width(name_label, name_w);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(value_label, x + name_w + 12, y - 2);
    lv_obj_set_width(value_label, value_w);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    if(value_out) {
        *value_out = value_label;
    }
}

static lv_obj_t *picoclaw_small_button(lv_obj_t *parent, int x, int y, int w,
                                       const char *text, uint32_t color,
                                       lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = ui_command_button(parent, x, y, w, text, color);

    lv_obj_set_height(btn, 52);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

void ui_picoclaw_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *icon_box;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *section;
    int landscape = ui_is_landscape();
    int body_x = ui_page_panel_x();
    int body_w = ui_page_panel_width();
    int body_y = ui_page_top_y(154);
    int body_h = ui_body_height(154);
    int content_w;
    int left_w;
    int right_w;
    int gap = 18;
    int action_w;
    int y;

    ui_create_header(scr, "PicoClaw");
    body = ui_scroll_panel(scr, body_x, body_y, body_w, body_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    content_w = ui_safe_content_width(body, body_w - 32);

    icon_box = lv_obj_create(body);
    lv_obj_set_size(icon_box, landscape ? 72 : 82, landscape ? 72 : 82);
    lv_obj_set_pos(icon_box, 0, 8);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    icon = ui_label(icon_box, "PC", &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_center(icon);

    title = ui_label(body, "PicoClaw", &lv_font_montserrat_28, 0xF2F5F8);
    lv_obj_set_pos(title, landscape ? 92 : 102, 12);
    subtitle = ui_label(body, "Lightweight agent runtime",
                        &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(subtitle, landscape ? 92 : 102, 52);
    lv_obj_set_width(subtitle, content_w - (landscape ? 100 : 110));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    picoclaw_status_label = ui_label(body, "Ready", &lv_font_montserrat_20,
                                     0x25C281);
    lv_obj_set_pos(picoclaw_status_label, 0, 104);
    lv_obj_set_width(picoclaw_status_label, content_w);
    lv_label_set_long_mode(picoclaw_status_label, LV_LABEL_LONG_DOT);

    if(landscape) {
        left_w = (content_w - gap) * 58 / 100;
        right_w = content_w - gap - left_w;
        y = 150;
    } else {
        left_w = content_w;
        right_w = content_w;
        y = 154;
    }

    section = ui_panel(body, 0, y, left_w, landscape ? 232 : 258);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x111820), 0);
    picoclaw_make_info_pair(section, 0, 0, left_w - 32, "Version",
                            &picoclaw_version_label, 0xF2F5F8);
    picoclaw_make_info_pair(section, 0, 42, left_w - 32, "Gateway",
                            &picoclaw_gateway_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 84, left_w - 32, "Network",
                            &picoclaw_network_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 126, left_w - 32, "Config",
                            &picoclaw_config_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 168, left_w - 32, "URL",
                            &picoclaw_url_label, 0x38BDF8);

    if(landscape) {
        section = ui_panel(body, left_w + gap, y, right_w, 232);
    } else {
        section = ui_panel(body, 0, y + 276, right_w, 318);
    }
    lv_obj_set_style_bg_color(section, lv_color_hex(0x111820), 0);
    action_w = (right_w - 32 - 12) / 2;
    if(action_w < 136) {
        action_w = right_w - 32;
    }
    picoclaw_install_btn =
        picoclaw_small_button(section, 0, 0, action_w, "Install",
                              0x38BDF8, picoclaw_action_event_cb,
                              (void *)(intptr_t)PICOCLAW_ACTION_INSTALL);
    if(action_w * 2 + 12 <= right_w - 32) {
        picoclaw_save_btn =
            picoclaw_small_button(section, action_w + 12, 0, action_w,
                                  "Save config", 0x25C281,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_SAVE_CONFIG);
        picoclaw_ask_btn =
            picoclaw_small_button(section, 0, 66, action_w, "Ask",
                                  0xF5A524, picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_PROMPT);
        picoclaw_gateway_start_btn =
            picoclaw_small_button(section, action_w + 12, 66, action_w,
                                  "Start gateway", 0x60A5FA,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_START);
        picoclaw_gateway_stop_btn =
            picoclaw_small_button(section, 0, 132, action_w,
                                  "Stop gateway", 0xEF4D5A,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_STOP);
        picoclaw_log_btn =
            picoclaw_small_button(section, action_w + 12, 132, action_w,
                                  "Log", 0x94A3B8,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_LOG);
    } else {
        picoclaw_save_btn =
            picoclaw_small_button(section, 0, 62, action_w, "Save config",
                                  0x25C281, picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_SAVE_CONFIG);
        picoclaw_ask_btn =
            picoclaw_small_button(section, 0, 124, action_w, "Ask", 0xF5A524,
                                  picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_PROMPT);
        picoclaw_gateway_start_btn =
            picoclaw_small_button(section, 0, 186, action_w, "Start gateway",
                                  0x60A5FA, picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_START);
        picoclaw_gateway_stop_btn =
            picoclaw_small_button(section, 0, 248, action_w, "Stop gateway",
                                  0xEF4D5A, picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_STOP);
        picoclaw_log_btn =
            picoclaw_small_button(section, 0, 310, action_w, "Log", 0x94A3B8,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_LOG);
    }

    y = landscape ? y + 250 : y + 614;
    section = ui_panel(body, 0, y, content_w, landscape ? 118 : 254);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x111820), 0);
    ui_label(section, "Configuration", &lv_font_montserrat_20, 0xF2F5F8);
    int config_btn_w = landscape ? (content_w - 48) / 4 : (content_w - 44) / 2;
    int config_gap = 12;
    picoclaw_small_button(section, 0, 48, config_btn_w, "Model name",
                          0x3DA5FF, picoclaw_config_event_cb,
                          (void *)(intptr_t)PICOCLAW_FIELD_MODEL_NAME);
    picoclaw_small_button(section, config_btn_w + config_gap, 48,
                          config_btn_w, "Model ID", 0x3DA5FF,
                          picoclaw_config_event_cb,
                          (void *)(intptr_t)PICOCLAW_FIELD_MODEL_ID);
    if(landscape) {
        picoclaw_small_button(section, (config_btn_w + config_gap) * 2, 48,
                              config_btn_w, "API base", 0x3DA5FF,
                              picoclaw_config_event_cb,
                              (void *)(intptr_t)PICOCLAW_FIELD_API_BASE);
        picoclaw_small_button(section, (config_btn_w + config_gap) * 3, 48,
                              config_btn_w, "API key", 0x3DA5FF,
                              picoclaw_config_event_cb,
                              (void *)(intptr_t)PICOCLAW_FIELD_API_KEY);
    } else {
        picoclaw_small_button(section, 0, 114, config_btn_w, "API base",
                              0x3DA5FF, picoclaw_config_event_cb,
                              (void *)(intptr_t)PICOCLAW_FIELD_API_BASE);
        picoclaw_small_button(section, config_btn_w + config_gap, 114,
                              config_btn_w, "API key", 0x3DA5FF,
                              picoclaw_config_event_cb,
                              (void *)(intptr_t)PICOCLAW_FIELD_API_KEY);
    }

    y += landscape ? 136 : 276;
    section = ui_panel(body, 0, y, content_w, landscape ? 240 : 330);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x0D1117), 0);
    ui_label(section, "Output", &lv_font_montserrat_20, 0xF2F5F8);
    picoclaw_output_label = ui_label(section, "No output yet",
                                     &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(picoclaw_output_label, 0, 44);
    lv_obj_set_width(picoclaw_output_label, content_w - 32);
    lv_label_set_long_mode(picoclaw_output_label, LV_LABEL_LONG_WRAP);

    picoclaw_timer = lv_timer_create(picoclaw_timer_cb, 250, NULL);
    picoclaw_start_action(PICOCLAW_ACTION_STATUS, NULL);
    picoclaw_update_ui();
}

void ui_picoclaw_cleanup(void)
{
    if(picoclaw_timer) {
        lv_timer_delete(picoclaw_timer);
        picoclaw_timer = NULL;
    }
    picoclaw_status_label = NULL;
    picoclaw_version_label = NULL;
    picoclaw_gateway_label = NULL;
    picoclaw_network_label = NULL;
    picoclaw_url_label = NULL;
    picoclaw_config_label = NULL;
    picoclaw_output_label = NULL;
    picoclaw_install_btn = NULL;
    picoclaw_save_btn = NULL;
    picoclaw_ask_btn = NULL;
    picoclaw_gateway_start_btn = NULL;
    picoclaw_gateway_stop_btn = NULL;
    picoclaw_log_btn = NULL;
}
