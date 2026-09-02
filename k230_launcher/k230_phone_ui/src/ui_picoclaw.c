#include "ui_picoclaw.h"

#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"
#include "qrcodegen.h"

#include <stdbool.h>
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
#define PICOCLAW_PRESET_KEY "picoclaw.preset"
#define PICOCLAW_OUTPUT_MAX 4096
#define PICOCLAW_CHAT_MAX 32
#define PICOCLAW_CHAT_TEXT_MAX 1024
#define PICOCLAW_QR_TEXT_MAX 1024
#define PICOCLAW_WEIXIN_QR_MAX 360
#define PICOCLAW_WEIXIN_QR_BORDER 4
#define PICOCLAW_WEIXIN_POLL_MS 2500

typedef enum {
    PICOCLAW_ACTION_STATUS = 0,
    PICOCLAW_ACTION_INSTALL,
    PICOCLAW_ACTION_SAVE_CONFIG,
    PICOCLAW_ACTION_ASK,
    PICOCLAW_ACTION_GATEWAY_START,
    PICOCLAW_ACTION_GATEWAY_STOP,
    PICOCLAW_ACTION_WEIXIN_AUTH,
    PICOCLAW_ACTION_WEIXIN_STATUS,
    PICOCLAW_ACTION_WEIXIN_CANCEL,
    PICOCLAW_ACTION_WEIXIN_UNBIND,
    PICOCLAW_ACTION_LOG,
} picoclaw_action_t;

typedef enum {
    PICOCLAW_FIELD_MODEL_NAME = 0,
    PICOCLAW_FIELD_MODEL_ID,
    PICOCLAW_FIELD_API_BASE,
    PICOCLAW_FIELD_API_KEY,
} picoclaw_field_t;

typedef enum {
    PICOCLAW_VIEW_CHAT = 0,
    PICOCLAW_VIEW_SETTINGS,
} picoclaw_view_t;

typedef struct {
    const char *display;
    const char *model_name;
    const char *model_id;
    const char *api_base;
    int custom;
} picoclaw_preset_t;

typedef struct {
    picoclaw_action_t action;
    int chat_index;
    char text[PICOCLAW_CHAT_TEXT_MAX];
} picoclaw_request_t;

typedef struct {
    char role;
    int pending;
    char text[PICOCLAW_CHAT_TEXT_MAX];
} picoclaw_chat_message_t;

static const picoclaw_preset_t picoclaw_presets[] = {
    {
        "DeepSeek Chat",
        "deepseek-chat",
        "deepseek/deepseek-chat",
        "https://api.deepseek.com/v1",
        0,
    },
    {
        "DeepSeek Reasoner",
        "deepseek-reasoner",
        "deepseek/deepseek-reasoner",
        "https://api.deepseek.com/v1",
        0,
    },
    {
        "GPT-4o mini",
        "gpt-4o-mini",
        "openai/gpt-4o-mini",
        "https://api.openai.com/v1",
        0,
    },
    {
        "OpenAI compatible",
        "custom-agent",
        "openai/gpt-4o-mini",
        "https://api.openai.com/v1",
        1,
    },
};

static pthread_mutex_t picoclaw_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *picoclaw_timer;
static lv_obj_t *picoclaw_body;
static lv_obj_t *picoclaw_status_panel;
static lv_obj_t *picoclaw_chat_scroll;
static lv_obj_t *picoclaw_input_panel;
static lv_obj_t *picoclaw_textarea;
static lv_obj_t *picoclaw_send_btn;
static lv_obj_t *picoclaw_settings_btn;
static lv_obj_t *picoclaw_status_label;
static lv_obj_t *picoclaw_model_label;
static lv_obj_t *picoclaw_detail_label;
static lv_obj_t *picoclaw_version_label;
static lv_obj_t *picoclaw_gateway_label;
static lv_obj_t *picoclaw_network_label;
static lv_obj_t *picoclaw_url_label;
static lv_obj_t *picoclaw_config_label;
static lv_obj_t *picoclaw_model_key_label;
static lv_obj_t *picoclaw_weixin_label;
static lv_obj_t *picoclaw_output_label;
static lv_obj_t *picoclaw_install_btn;
static lv_obj_t *picoclaw_save_btn;
static lv_obj_t *picoclaw_gateway_start_btn;
static lv_obj_t *picoclaw_gateway_stop_btn;
static lv_obj_t *picoclaw_weixin_auth_btn;
static lv_obj_t *picoclaw_weixin_status_btn;
static lv_obj_t *picoclaw_weixin_cancel_btn;
static lv_obj_t *picoclaw_log_btn;
static lv_obj_t *picoclaw_weixin_qr_overlay;
static lv_obj_t *picoclaw_weixin_qr_canvas;
static lv_obj_t *picoclaw_weixin_qr_status_label;
static uint16_t *picoclaw_weixin_qr_buf;
static ui_input_inline_t *picoclaw_inline_input;

static picoclaw_view_t picoclaw_view = PICOCLAW_VIEW_CHAT;
static int picoclaw_keyboard_reserved_h;
static int picoclaw_status_panel_h;
static int picoclaw_busy;
static int picoclaw_last_rc;
static int picoclaw_installed;
static int picoclaw_gateway_running;
static int picoclaw_config_ready;
static int picoclaw_network_ready;
static int picoclaw_model_key_ready;
static int picoclaw_weixin_ready;
static int picoclaw_weixin_auth_running;
static int picoclaw_weixin_qr_dirty;
static int picoclaw_chat_dirty;
static int picoclaw_chat_count;
static uint32_t picoclaw_weixin_last_poll;
static char picoclaw_status_text[160] = "Ready";
static char picoclaw_output_text[PICOCLAW_OUTPUT_MAX] = "";
static char picoclaw_version_text[256] = "not installed";
static char picoclaw_gateway_text[80] = "stopped";
static char picoclaw_network_text[80] = "unknown";
static char picoclaw_config_text[80] = "unknown";
static char picoclaw_model_key_text[80] = "missing";
static char picoclaw_weixin_text[80] = "missing";
static char picoclaw_weixin_auth_text[80] = "stopped";
static char picoclaw_weixin_qr_text[PICOCLAW_QR_TEXT_MAX] = "";
static char picoclaw_url_text[160] = "http://<board-ip>:18790";
static picoclaw_chat_message_t picoclaw_chat[PICOCLAW_CHAT_MAX];

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

static void picoclaw_trim_local(char *text)
{
    char *start;
    size_t len;

    if(!text) {
        return;
    }
    start = text;
    while(*start == ' ' || *start == '\t' || *start == '\r' ||
          *start == '\n') {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }
    len = strlen(text);
    while(len > 0 &&
          (text[len - 1U] == ' ' || text[len - 1U] == '\t' ||
           text[len - 1U] == '\r' || text[len - 1U] == '\n')) {
        text[--len] = '\0';
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
    char qr[PICOCLAW_QR_TEXT_MAX];

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
    if(picoclaw_find_value(output, "model_key", value, sizeof(value))) {
        picoclaw_model_key_ready = strcmp(value, "ready") == 0;
        snprintf(picoclaw_model_key_text, sizeof(picoclaw_model_key_text),
                 "%s", value);
    }
    if(picoclaw_find_value(output, "weixin", value, sizeof(value))) {
        picoclaw_weixin_ready = strcmp(value, "ready") == 0;
        snprintf(picoclaw_weixin_text, sizeof(picoclaw_weixin_text), "%s",
                 value);
    }
    if(picoclaw_find_value(output, "weixin_auth", value, sizeof(value))) {
        picoclaw_weixin_auth_running = strcmp(value, "running") == 0;
        snprintf(picoclaw_weixin_auth_text,
                 sizeof(picoclaw_weixin_auth_text), "%s", value);
    }
    if(picoclaw_find_value(output, "weixin_qr", qr, sizeof(qr)) && qr[0]) {
        if(strcmp(picoclaw_weixin_qr_text, qr) != 0) {
            snprintf(picoclaw_weixin_qr_text,
                     sizeof(picoclaw_weixin_qr_text), "%s", qr);
            picoclaw_weixin_qr_dirty = 1;
        }
    }
    if(picoclaw_find_value(output, "url", value, sizeof(value))) {
        snprintf(picoclaw_url_text, sizeof(picoclaw_url_text), "%s", value);
    }
}

static const picoclaw_preset_t *picoclaw_preset_at(int index)
{
    if(index < 0 ||
       index >= (int)(sizeof(picoclaw_presets) / sizeof(picoclaw_presets[0]))) {
        index = 0;
    }
    return &picoclaw_presets[index];
}

static int picoclaw_current_preset_index(void)
{
    char value[32];
    char model_id[256];
    char api_base[256];
    int i;

    if(ui_prefs_get(PICOCLAW_PRESET_KEY, value, sizeof(value), "") == 0 &&
       value[0]) {
        int index = atoi(value);

        if(index >= 0 &&
           index < (int)(sizeof(picoclaw_presets) /
                         sizeof(picoclaw_presets[0]))) {
            return index;
        }
    }

    picoclaw_load_pref(model_id, sizeof(model_id), PICOCLAW_MODEL_ID_KEY,
                       "");
    picoclaw_load_pref(api_base, sizeof(api_base), PICOCLAW_API_BASE_KEY,
                       "");
    for(i = 0;
        i < (int)(sizeof(picoclaw_presets) / sizeof(picoclaw_presets[0]));
        i++) {
        if(!picoclaw_presets[i].custom &&
           strcmp(model_id, picoclaw_presets[i].model_id) == 0 &&
           strcmp(api_base, picoclaw_presets[i].api_base) == 0) {
            return i;
        }
    }
    return (int)(sizeof(picoclaw_presets) / sizeof(picoclaw_presets[0])) - 1;
}

static const picoclaw_preset_t *picoclaw_current_preset(void)
{
    return picoclaw_preset_at(picoclaw_current_preset_index());
}

static void picoclaw_effective_model(char *model_name, size_t model_name_len,
                                     char *model_id, size_t model_id_len,
                                     char *api_base, size_t api_base_len)
{
    const picoclaw_preset_t *preset = picoclaw_current_preset();

    picoclaw_load_pref(model_name, model_name_len, PICOCLAW_MODEL_NAME_KEY,
                       preset->model_name);
    picoclaw_load_pref(model_id, model_id_len, PICOCLAW_MODEL_ID_KEY,
                       preset->model_id);
    picoclaw_load_pref(api_base, api_base_len, PICOCLAW_API_BASE_KEY,
                       preset->api_base);
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
    case PICOCLAW_ACTION_WEIXIN_AUTH:
        snprintf(cmd, cmd_len, "%s weixin-auth", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_WEIXIN_STATUS:
        snprintf(cmd, cmd_len, "%s weixin-status", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_WEIXIN_CANCEL:
        snprintf(cmd, cmd_len, "%s weixin-cancel", PICOCLAW_SCRIPT);
        return 0;
    case PICOCLAW_ACTION_WEIXIN_UNBIND:
        snprintf(cmd, cmd_len, "%s weixin-unbind", PICOCLAW_SCRIPT);
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
        picoclaw_effective_model(model_name, sizeof(model_name),
                                 model_id, sizeof(model_id),
                                 api_base, sizeof(api_base));
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
    case PICOCLAW_ACTION_WEIXIN_AUTH:
        text = "Starting Weixin login...";
        break;
    case PICOCLAW_ACTION_WEIXIN_STATUS:
        text = "Checking Weixin login...";
        break;
    case PICOCLAW_ACTION_WEIXIN_CANCEL:
        text = "Cancelling Weixin login...";
        break;
    case PICOCLAW_ACTION_WEIXIN_UNBIND:
        text = "Unbinding Weixin...";
        break;
    case PICOCLAW_ACTION_LOG:
        text = "Reading log...";
        break;
    default:
        break;
    }
    snprintf(out, len, "%s", text);
}

static int picoclaw_line_is_noise(const char *line)
{
    if(!line || !line[0]) {
        return 1;
    }
    if(strstr(line, "Time zone loaded successfully") ||
       strstr(line, "Error loading time zone") ||
       strstr(line, "TZ environment") ||
       strstr(line, "ZONEINFO environment") ||
       strstr(line, "Using config") ||
       strstr(line, "PICOCLAW_HOME") ||
       strstr(line, "PICOCLAW_CONFIG")) {
        return 1;
    }
    return 0;
}

static void picoclaw_clean_reply(const char *output, int rc, char *reply,
                                 size_t reply_len)
{
    char copy[PICOCLAW_OUTPUT_MAX];
    char *line;
    char *save = NULL;

    if(!reply || reply_len == 0) {
        return;
    }
    reply[0] = '\0';
    snprintf(copy, sizeof(copy), "%s", output ? output : "");
    line = strtok_r(copy, "\n", &save);
    while(line) {
        char item[512];

        snprintf(item, sizeof(item), "%s", line);
        picoclaw_trim_local(item);
        if(!picoclaw_line_is_noise(item)) {
            if(reply[0]) {
                picoclaw_append_output(reply, reply_len, "\n");
            }
            picoclaw_append_output(reply, reply_len, item);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    picoclaw_trim_local(reply);
    if(!reply[0]) {
        snprintf(reply, reply_len, "%s", rc == 0 ? "Done" : "Prompt failed");
    }
}

static int picoclaw_append_chat_locked(char role, const char *text,
                                       int pending)
{
    int index;

    if(picoclaw_chat_count >= PICOCLAW_CHAT_MAX) {
        memmove(&picoclaw_chat[0], &picoclaw_chat[1],
                sizeof(picoclaw_chat[0]) * (PICOCLAW_CHAT_MAX - 1));
        picoclaw_chat_count = PICOCLAW_CHAT_MAX - 1;
    }
    index = picoclaw_chat_count++;
    memset(&picoclaw_chat[index], 0, sizeof(picoclaw_chat[index]));
    picoclaw_chat[index].role = role;
    picoclaw_chat[index].pending = pending;
    snprintf(picoclaw_chat[index].text, sizeof(picoclaw_chat[index].text),
             "%s", text ? text : "");
    picoclaw_chat_dirty = 1;
    return index;
}

static void picoclaw_update_chat_locked(int index, const char *text,
                                        int pending)
{
    if(index < 0 || index >= picoclaw_chat_count) {
        return;
    }
    snprintf(picoclaw_chat[index].text, sizeof(picoclaw_chat[index].text),
             "%s", text ? text : "");
    picoclaw_chat[index].pending = pending;
    picoclaw_chat_dirty = 1;
}

static void *picoclaw_worker(void *arg)
{
    picoclaw_request_t *req = (picoclaw_request_t *)arg;
    char cmd[2400];
    char output[PICOCLAW_OUTPUT_MAX];
    char reply[PICOCLAW_CHAT_TEXT_MAX];
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
    picoclaw_last_rc = rc;
    picoclaw_apply_status_locked(output);
    if(req->action == PICOCLAW_ACTION_ASK) {
        picoclaw_clean_reply(output, rc, reply, sizeof(reply));
        picoclaw_update_chat_locked(req->chat_index, reply, 0);
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 rc == 0 ? "Reply received" : "Prompt failed");
    } else if(req->action == PICOCLAW_ACTION_LOG) {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 "Log updated");
    } else if(req->action == PICOCLAW_ACTION_WEIXIN_AUTH ||
              req->action == PICOCLAW_ACTION_WEIXIN_STATUS ||
              req->action == PICOCLAW_ACTION_WEIXIN_CANCEL ||
              req->action == PICOCLAW_ACTION_WEIXIN_UNBIND) {
        if(rc == 0 && picoclaw_weixin_ready) {
            snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                     "Weixin token ready");
        } else if(rc == 0 && picoclaw_weixin_auth_running) {
            snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                     "Weixin waiting for scan");
        } else if(rc == 0) {
            snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                     "Weixin not linked");
        } else {
            snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                     "%s rc=%d", "Weixin action failed", rc);
        }
    } else if(rc == 0) {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s",
                 "Done");
        if(req->action == PICOCLAW_ACTION_SAVE_CONFIG) {
            picoclaw_config_ready = 1;
            snprintf(picoclaw_config_text, sizeof(picoclaw_config_text), "%s",
                     "ready");
        }
    } else {
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                 "%s rc=%d", "Action failed", rc);
    }
    snprintf(picoclaw_output_text, sizeof(picoclaw_output_text), "%s",
             output[0] ? output : status);
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

    req = calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&picoclaw_lock);
        snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
                 "%s", "Out of memory");
        pthread_mutex_unlock(&picoclaw_lock);
        return;
    }

    req->action = action;
    req->chat_index = -1;
    snprintf(req->text, sizeof(req->text), "%s", text ? text : "");

    pthread_mutex_lock(&picoclaw_lock);
    if(picoclaw_busy) {
        pthread_mutex_unlock(&picoclaw_lock);
        free(req);
        return;
    }
    if(action == PICOCLAW_ACTION_ASK) {
        char prompt[PICOCLAW_CHAT_TEXT_MAX];

        snprintf(prompt, sizeof(prompt), "%s", req->text);
        picoclaw_trim_local(prompt);
        if(!prompt[0]) {
            pthread_mutex_unlock(&picoclaw_lock);
            free(req);
            return;
        }
        snprintf(req->text, sizeof(req->text), "%s", prompt);
        picoclaw_append_chat_locked('U', prompt, 0);
        req->chat_index =
            picoclaw_append_chat_locked('A', "Thinking...", 1);
    }
    picoclaw_busy = 1;
    picoclaw_status_for_action(action, status, sizeof(status));
    snprintf(picoclaw_status_text, sizeof(picoclaw_status_text), "%s", status);
    pthread_mutex_unlock(&picoclaw_lock);

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
    app_request_fast_refresh();
}

static void picoclaw_update_buttons(int busy)
{
    lv_obj_t *buttons[] = {
        picoclaw_install_btn,
        picoclaw_save_btn,
        picoclaw_send_btn,
        picoclaw_settings_btn,
        picoclaw_gateway_start_btn,
        picoclaw_gateway_stop_btn,
        picoclaw_weixin_auth_btn,
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
        if(picoclaw_send_btn) {
            lv_obj_add_state(picoclaw_send_btn, LV_STATE_DISABLED);
        }
        if(picoclaw_gateway_start_btn) {
            lv_obj_add_state(picoclaw_gateway_start_btn, LV_STATE_DISABLED);
        }
        if(picoclaw_weixin_auth_btn) {
            lv_obj_add_state(picoclaw_weixin_auth_btn, LV_STATE_DISABLED);
        }
    }
    if(picoclaw_gateway_running) {
        if(picoclaw_gateway_start_btn) {
            lv_obj_add_state(picoclaw_gateway_start_btn, LV_STATE_DISABLED);
        }
    } else if(!busy && picoclaw_gateway_stop_btn) {
        lv_obj_add_state(picoclaw_gateway_stop_btn, LV_STATE_DISABLED);
    }
    if(picoclaw_weixin_auth_btn && !busy && picoclaw_installed) {
        lv_obj_t *label = lv_obj_get_child(picoclaw_weixin_auth_btn, 0);
        const char *text = picoclaw_weixin_ready ? "Unbind Weixin" :
                           (picoclaw_weixin_auth_running ?
                            "Cancel" : "Weixin login");
        uint32_t color = picoclaw_weixin_ready ? 0xEF4D5A :
                         (picoclaw_weixin_auth_running ?
                          0xEF4D5A : 0x22C55E);

        lv_obj_set_style_border_color(picoclaw_weixin_auth_btn,
                                      lv_color_hex(color), 0);
        if(label) {
            const char *shown = ui_tr(text);

            lv_label_set_text(label, shown);
            lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            lv_obj_set_style_text_font(label,
                                       ui_font_for_text(shown,
                                                        &lv_font_montserrat_18),
                                       0);
            lv_obj_center(label);
        }
    }
}

static uint16_t picoclaw_rgb565(uint32_t rgb)
{
    uint8_t r = (uint8_t)((rgb >> 16U) & 0xffU);
    uint8_t g = (uint8_t)((rgb >> 8U) & 0xffU);
    uint8_t b = (uint8_t)(rgb & 0xffU);

    return (uint16_t)(((uint16_t)(r & 0xf8U) << 8U) |
                      ((uint16_t)(g & 0xfcU) << 3U) |
                      ((uint16_t)b >> 3U));
}

static void picoclaw_qr_render_into(lv_obj_t *canvas, uint16_t *buf,
                                    const char *url, int px)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    uint16_t black = picoclaw_rgb565(0x05070A);
    uint16_t white = picoclaw_rgb565(0xF8FAFC);
    uint16_t empty = picoclaw_rgb565(0x17212B);
    int qr_size = 0;
    int scale = 1;
    int image_px;
    int offset;
    bool ok = false;

    if(!canvas || !lv_obj_is_valid(canvas) || !buf) {
        return;
    }
    if(px < 64) {
        px = 64;
    }
    if(px > PICOCLAW_WEIXIN_QR_MAX) {
        px = PICOCLAW_WEIXIN_QR_MAX;
    }

    for(int i = 0; i < px * px; i++) {
        buf[i] = empty;
    }
    if(url && url[0]) {
        ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                  qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX,
                                  qrcodegen_Mask_AUTO, true);
    }
    if(ok) {
        qr_size = qrcodegen_getSize(qr);
        if(qr_size > 0) {
            scale = px / (qr_size + PICOCLAW_WEIXIN_QR_BORDER * 2);
            if(scale < 1) {
                scale = 1;
            }
            image_px = (qr_size + PICOCLAW_WEIXIN_QR_BORDER * 2) * scale;
            offset = (px - image_px) / 2;
            if(offset < 0) {
                offset = 0;
            }
            for(int y = 0; y < px; y++) {
                for(int x = 0; x < px; x++) {
                    int mx = (x - offset) / scale - PICOCLAW_WEIXIN_QR_BORDER;
                    int my = (y - offset) / scale - PICOCLAW_WEIXIN_QR_BORDER;
                    bool module = false;

                    if(x >= offset && y >= offset && x < offset + image_px &&
                       y < offset + image_px && mx >= 0 && my >= 0 &&
                       mx < qr_size && my < qr_size) {
                        module = qrcodegen_getModule(qr, mx, my);
                    }
                    buf[y * px + x] = module ? black : white;
                }
            }
        }
    }
    lv_canvas_set_buffer(canvas, buf, px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
}

static void picoclaw_weixin_qr_close(void)
{
    if(picoclaw_weixin_qr_overlay &&
       lv_obj_is_valid(picoclaw_weixin_qr_overlay)) {
        lv_obj_delete(picoclaw_weixin_qr_overlay);
    }
    picoclaw_weixin_qr_overlay = NULL;
    picoclaw_weixin_qr_canvas = NULL;
    picoclaw_weixin_qr_status_label = NULL;
    free(picoclaw_weixin_qr_buf);
    picoclaw_weixin_qr_buf = NULL;
}

static void picoclaw_weixin_qr_close_event_cb(lv_event_t *event)
{
    (void)event;
    picoclaw_weixin_qr_close();
}

static void picoclaw_weixin_qr_cancel_event_cb(lv_event_t *event)
{
    (void)event;
    picoclaw_weixin_qr_close();
    picoclaw_start_action(PICOCLAW_ACTION_WEIXIN_CANCEL, NULL);
}

static void picoclaw_weixin_qr_show(const char *url)
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *button;
    lv_obj_t *label;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int qr_px;
    int card_w;
    int card_h;
    int qr_btn_w;
    int qr_btn_gap;
    int qr_btn_x;

    if(!url || !url[0]) {
        return;
    }

    picoclaw_weixin_qr_close();

    qr_px = screen_w - 120;
    if(qr_px > screen_h - 260) {
        qr_px = screen_h - 260;
    }
    if(qr_px > PICOCLAW_WEIXIN_QR_MAX) {
        qr_px = PICOCLAW_WEIXIN_QR_MAX;
    }
    if(qr_px < 160) {
        qr_px = 160;
    }
    card_w = qr_px + 72;
    card_h = qr_px + 212;
    if(card_w > screen_w - 48) {
        card_w = screen_w - 48;
    }
    if(card_h > screen_h - 48) {
        card_h = screen_h - 48;
    }
    qr_btn_w = card_w < 300 ? 106 : 130;
    qr_btn_gap = card_w < 300 ? 8 : 28;
    qr_btn_x = (card_w - qr_btn_w * 2 - qr_btn_gap) / 2;
    if(qr_btn_x < 12) {
        qr_btn_x = 12;
    }

    picoclaw_weixin_qr_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(picoclaw_weixin_qr_overlay);
    lv_obj_set_style_bg_color(picoclaw_weixin_qr_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(picoclaw_weixin_qr_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(picoclaw_weixin_qr_overlay, 0, 0);
    lv_obj_set_style_pad_all(picoclaw_weixin_qr_overlay, 0, 0);
    lv_obj_add_flag(picoclaw_weixin_qr_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(picoclaw_weixin_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = ui_panel(picoclaw_weixin_qr_overlay, 0, 0, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(card, "Weixin login", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(title, card_w - 48);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    picoclaw_weixin_qr_canvas = lv_canvas_create(card);
    picoclaw_weixin_qr_buf = calloc((size_t)qr_px * (size_t)qr_px,
                                    sizeof(uint16_t));
    if(picoclaw_weixin_qr_buf) {
        picoclaw_qr_render_into(picoclaw_weixin_qr_canvas,
                                picoclaw_weixin_qr_buf, url, qr_px);
        lv_obj_align(picoclaw_weixin_qr_canvas, LV_ALIGN_TOP_MID, 0, 56);
    }

    hint = ui_label(card, "Scan with WeChat", &lv_font_montserrat_16,
                    0xCBD5E1);
    lv_obj_set_width(hint, card_w - 48);
    lv_obj_set_pos(hint, 24, qr_px + 74);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);

    picoclaw_weixin_qr_status_label =
        ui_label(card, "Weixin waiting for scan", &lv_font_montserrat_14,
                 0x94A3B8);
    lv_obj_set_width(picoclaw_weixin_qr_status_label, card_w - 48);
    lv_obj_set_pos(picoclaw_weixin_qr_status_label, 24, qr_px + 102);
    lv_obj_set_style_text_align(picoclaw_weixin_qr_status_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(picoclaw_weixin_qr_status_label,
                           LV_LABEL_LONG_DOT);

    button = ui_command_button(card, 0, 0, qr_btn_w, "Cancel", 0xEF4D5A);
    lv_obj_set_pos(button, qr_btn_x, card_h - 62);
    lv_obj_add_event_cb(button, picoclaw_weixin_qr_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);
    label = lv_obj_get_child(button, 0);
    if(label) {
        lv_obj_set_style_text_font(label,
                                   ui_font_for_text("Cancel",
                                                    &lv_font_montserrat_18),
                                   0);
    }

    button = ui_command_button(card, 0, 0, qr_btn_w, "Close", 0x475569);
    lv_obj_set_pos(button, qr_btn_x + qr_btn_w + qr_btn_gap, card_h - 62);
    lv_obj_add_event_cb(button, picoclaw_weixin_qr_close_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void picoclaw_chat_add_empty(void)
{
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *text;
    lv_obj_t *hint;
    int page_w;

    if(!picoclaw_chat_scroll || !lv_obj_is_valid(picoclaw_chat_scroll)) {
        return;
    }
    lv_obj_update_layout(picoclaw_chat_scroll);
    page_w = lv_obj_get_width(picoclaw_chat_scroll);
    if(page_w < 240) {
        page_w = ui_page_panel_width();
    }

    box = lv_obj_create(picoclaw_chat_scroll);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_top(box, 42, 0);
    lv_obj_set_style_pad_bottom(box, 28, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    icon = ui_label(box, "PC", &lv_font_montserrat_32, 0x38BDF8);
    lv_obj_set_width(icon, page_w - 48);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    text = ui_label(box, "No messages yet", &lv_font_montserrat_18,
                    0x94A3B8);
    lv_obj_set_width(text, page_w - 48);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    hint = ui_label(box, "Type a message below.",
                    &lv_font_montserrat_14, 0x64748B);
    lv_obj_set_width(hint, page_w - 48);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

static void picoclaw_chat_add_bubble(const picoclaw_chat_message_t *msg)
{
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *text;
    lv_obj_t *footer;
    int sent;
    int page_w;
    int bubble_w;

    if(!picoclaw_chat_scroll || !lv_obj_is_valid(picoclaw_chat_scroll) ||
       !msg || !msg->text[0]) {
        return;
    }
    sent = msg->role == 'U';

    lv_obj_update_layout(picoclaw_chat_scroll);
    page_w = lv_obj_get_width(picoclaw_chat_scroll);
    if(page_w < 260) {
        page_w = ui_page_panel_width();
    }
    bubble_w = (page_w * 74) / 100;
    if(bubble_w < 240) {
        bubble_w = page_w > 280 ? 240 : page_w - 28;
    }
    if(bubble_w > 660) {
        bubble_w = 660;
    }

    row = lv_obj_create(picoclaw_chat_scroll);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_top(row, 5, 0);
    lv_obj_set_style_pad_bottom(row, 5, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, sent ? LV_FLEX_ALIGN_END :
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(sent ? 0x16A34A : 0x232B35), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, sent ? 0 : 1, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 5, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    text = ui_label(bubble, msg->text, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_set_width(text, bubble_w - 20);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    footer = ui_label(bubble, sent ? "You" :
                      (msg->pending ? "Thinking..." : "PicoClaw"),
                      &lv_font_montserrat_14,
                      sent ? 0xDCFCE7 : 0x94A3B8);
    lv_obj_set_width(footer, bubble_w - 20);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);
}

static void picoclaw_chat_rebuild(int force)
{
    picoclaw_chat_message_t copy[PICOCLAW_CHAT_MAX];
    int count;
    uint32_t child_count;

    if(!picoclaw_chat_scroll || !lv_obj_is_valid(picoclaw_chat_scroll)) {
        return;
    }

    pthread_mutex_lock(&picoclaw_lock);
    if(!force && !picoclaw_chat_dirty) {
        pthread_mutex_unlock(&picoclaw_lock);
        return;
    }
    count = picoclaw_chat_count;
    if(count > PICOCLAW_CHAT_MAX) {
        count = PICOCLAW_CHAT_MAX;
    }
    memcpy(copy, picoclaw_chat, sizeof(copy));
    picoclaw_chat_dirty = 0;
    pthread_mutex_unlock(&picoclaw_lock);

    lv_obj_clean(picoclaw_chat_scroll);
    if(count <= 0) {
        picoclaw_chat_add_empty();
    } else {
        int i;

        for(i = 0; i < count; i++) {
            picoclaw_chat_add_bubble(&copy[i]);
        }
    }

    lv_obj_update_layout(picoclaw_chat_scroll);
    child_count = lv_obj_get_child_count(picoclaw_chat_scroll);
    if(child_count > 0) {
        lv_obj_t *last = lv_obj_get_child(picoclaw_chat_scroll,
                                          child_count - 1U);
        if(last) {
            lv_obj_scroll_to_view(last, LV_ANIM_OFF);
        }
    }
}

static void picoclaw_layout_main(void)
{
    int content_w = ui_page_panel_width();
    int top_y = ui_is_landscape() ? 64 : 124;
    int body_h = ui_body_height(top_y);
    int gap = ui_is_landscape() ? 8 : 10;
    int input_h = ui_is_landscape() ? 60 : 68;
    int chat_h;
    int settings_w = ui_is_landscape() ? 58 : 56;
    int send_w = ui_is_landscape() ? 86 : 78;

    picoclaw_status_panel_h = ui_is_landscape() ? 96 : 128;
    chat_h = body_h - picoclaw_status_panel_h - input_h -
             gap * 2 - picoclaw_keyboard_reserved_h;
    if(chat_h < 140) {
        chat_h = 140;
    }

    if(picoclaw_status_panel) {
        lv_obj_set_pos(picoclaw_status_panel, ui_page_panel_x(), 0);
        lv_obj_set_size(picoclaw_status_panel, content_w,
                        picoclaw_status_panel_h);
    }
    if(picoclaw_chat_scroll) {
        lv_obj_set_pos(picoclaw_chat_scroll, ui_page_panel_x(),
                       picoclaw_status_panel_h + gap);
        lv_obj_set_size(picoclaw_chat_scroll, content_w, chat_h);
    }
    if(picoclaw_input_panel) {
        lv_obj_set_pos(picoclaw_input_panel, ui_page_panel_x(),
                       picoclaw_status_panel_h + gap + chat_h + gap);
        lv_obj_set_size(picoclaw_input_panel, content_w, input_h);
    }
    if(picoclaw_settings_btn) {
        lv_obj_set_pos(picoclaw_settings_btn, content_w - settings_w, 0);
        lv_obj_set_size(picoclaw_settings_btn, settings_w,
                        ui_is_landscape() ? 48 : 52);
    }
    if(picoclaw_status_label) {
        lv_obj_set_pos(picoclaw_status_label, 0, 0);
        lv_obj_set_width(picoclaw_status_label,
                         content_w - settings_w - 12);
    }
    if(picoclaw_model_label) {
        lv_obj_set_pos(picoclaw_model_label, 0, 30);
        lv_obj_set_width(picoclaw_model_label,
                         content_w - settings_w - 12);
    }
    if(picoclaw_detail_label) {
        lv_obj_set_pos(picoclaw_detail_label, 0, 58);
        lv_obj_set_width(picoclaw_detail_label,
                         content_w - settings_w - 12);
    }
    if(picoclaw_textarea) {
        lv_obj_set_pos(picoclaw_textarea, 0, 0);
        lv_obj_set_size(picoclaw_textarea, content_w - send_w - 12,
                        input_h - 6);
    }
    if(picoclaw_send_btn) {
        lv_obj_set_pos(picoclaw_send_btn, content_w - send_w, 0);
        lv_obj_set_size(picoclaw_send_btn, send_w, input_h - 6);
    }
}

static void picoclaw_inline_layout_cb(int active, int reserved_h,
                                      void *user_data)
{
    (void)active;
    (void)user_data;
    picoclaw_keyboard_reserved_h = reserved_h;
    picoclaw_layout_main();
    picoclaw_chat_rebuild(0);
}

static void picoclaw_update_status_labels(int busy, int rc)
{
    char status[160];
    char version[256];
    char gateway[80];
    char network[80];
    char config[80];
    char model_key[80];
    char url[160];
    char model_name[128];
    char model_id[256];
    char api_base[256];
    char output[PICOCLAW_OUTPUT_MAX];
    char line[360];
    int weixin_ready;
    int weixin_auth_running;

    pthread_mutex_lock(&picoclaw_lock);
    snprintf(status, sizeof(status), "%s", picoclaw_status_text);
    snprintf(version, sizeof(version), "%s", picoclaw_version_text);
    snprintf(gateway, sizeof(gateway), "%s", picoclaw_gateway_text);
    snprintf(network, sizeof(network), "%s", picoclaw_network_text);
    snprintf(config, sizeof(config), "%s", picoclaw_config_text);
    snprintf(model_key, sizeof(model_key), "%s", picoclaw_model_key_text);
    snprintf(url, sizeof(url), "%s", picoclaw_url_text);
    snprintf(output, sizeof(output), "%s", picoclaw_output_text);
    weixin_ready = picoclaw_weixin_ready;
    weixin_auth_running = picoclaw_weixin_auth_running;
    pthread_mutex_unlock(&picoclaw_lock);

    picoclaw_effective_model(model_name, sizeof(model_name),
                             model_id, sizeof(model_id),
                             api_base, sizeof(api_base));

    if(picoclaw_status_label) {
        lv_label_set_text(picoclaw_status_label, ui_tr(status));
        lv_obj_set_style_text_color(picoclaw_status_label,
                                    lv_color_hex(busy ? 0xF5A524 :
                                                 (rc == 0 ? 0x25C281 :
                                                  0xF5A524)), 0);
    }
    if(picoclaw_model_label) {
        snprintf(line, sizeof(line), "%s  %s",
                 ui_tr("Current model"), model_name);
        lv_label_set_text(picoclaw_model_label, line);
    }
    if(picoclaw_detail_label) {
        snprintf(line, sizeof(line), "%s %s  %s %s  %s %s",
                 ui_tr("Network"), ui_tr(network),
                 ui_tr("Config"), ui_tr(config),
                 ui_tr("Gateway"), ui_tr(gateway));
        lv_label_set_text(picoclaw_detail_label, line);
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
    if(picoclaw_model_key_label) {
        lv_label_set_text(picoclaw_model_key_label, ui_tr(model_key));
        lv_obj_set_style_text_color(picoclaw_model_key_label,
                                    lv_color_hex(picoclaw_model_key_ready ?
                                                 0x25C281 : 0xF5A524), 0);
    }
    if(picoclaw_url_label) {
        lv_label_set_text(picoclaw_url_label, url);
    }
    if(picoclaw_weixin_label) {
        const char *text = weixin_ready ? "Weixin token ready" :
                           (weixin_auth_running ?
                            "Weixin waiting for scan" : "Weixin not linked");

        lv_label_set_text(picoclaw_weixin_label, ui_tr(text));
        lv_obj_set_style_text_color(picoclaw_weixin_label,
                                    lv_color_hex(weixin_ready ?
                                                 0x25C281 :
                                                 (weixin_auth_running ?
                                                  0xF5A524 : 0x9AA4AF)), 0);
    }
    if(picoclaw_weixin_qr_status_label &&
       lv_obj_is_valid(picoclaw_weixin_qr_status_label)) {
        const char *text = weixin_ready ? "Weixin token ready" :
                           (weixin_auth_running ?
                            "Weixin waiting for scan" : "Weixin not linked");

        lv_label_set_text(picoclaw_weixin_qr_status_label, ui_tr(text));
        lv_obj_set_style_text_color(picoclaw_weixin_qr_status_label,
                                    lv_color_hex(weixin_ready ?
                                                 0x25C281 :
                                                 (weixin_auth_running ?
                                                  0xF5A524 : 0x94A3B8)), 0);
    }
    if(picoclaw_output_label) {
        lv_label_set_text(picoclaw_output_label,
                          output[0] ? output : ui_tr("No output yet"));
    }

    picoclaw_update_buttons(busy);
}

static void picoclaw_update_ui(void)
{
    int busy;
    int rc;
    int qr_dirty;
    char qr_text[PICOCLAW_QR_TEXT_MAX];

    pthread_mutex_lock(&picoclaw_lock);
    busy = picoclaw_busy;
    rc = picoclaw_last_rc;
    qr_dirty = picoclaw_weixin_qr_dirty;
    if(qr_dirty) {
        snprintf(qr_text, sizeof(qr_text), "%s", picoclaw_weixin_qr_text);
        picoclaw_weixin_qr_dirty = 0;
    } else {
        qr_text[0] = '\0';
    }
    pthread_mutex_unlock(&picoclaw_lock);

    picoclaw_update_status_labels(busy, rc);
    if(qr_dirty && qr_text[0]) {
        picoclaw_weixin_qr_show(qr_text);
    }
    picoclaw_chat_rebuild(0);
}

static void picoclaw_timer_cb(lv_timer_t *timer)
{
    uint32_t now;
    int should_poll = 0;

    (void)timer;
    picoclaw_update_ui();
    now = lv_tick_get();
    pthread_mutex_lock(&picoclaw_lock);
    if(picoclaw_weixin_auth_running && !picoclaw_busy && picoclaw_installed &&
       now - picoclaw_weixin_last_poll >= PICOCLAW_WEIXIN_POLL_MS) {
        picoclaw_weixin_last_poll = now;
        should_poll = 1;
    }
    pthread_mutex_unlock(&picoclaw_lock);
    if(should_poll) {
        picoclaw_start_action(PICOCLAW_ACTION_WEIXIN_STATUS, NULL);
    }
}

static void picoclaw_send_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    picoclaw_start_action(PICOCLAW_ACTION_ASK, text ? text : "");
    if(picoclaw_textarea && lv_obj_is_valid(picoclaw_textarea)) {
        lv_textarea_set_text(picoclaw_textarea, "");
    }
}

static void picoclaw_send_event_cb(lv_event_t *event)
{
    const char *text;

    (void)event;
    if(!picoclaw_textarea || !lv_obj_is_valid(picoclaw_textarea)) {
        return;
    }
    text = lv_textarea_get_text(picoclaw_textarea);
    picoclaw_send_submit_cb(text, NULL);
}

static void picoclaw_input_focus_event_cb(lv_event_t *event)
{
    (void)event;
    if(picoclaw_inline_input) {
        ui_input_inline_focus(picoclaw_inline_input);
    }
}

static void picoclaw_focus_input_if_hardware_keyboard(void)
{
    if(picoclaw_inline_input && ui_extension_keyboard_active() &&
       !ui_input_inline_is_active(picoclaw_inline_input)) {
        ui_input_inline_focus(picoclaw_inline_input);
    }
}

static void picoclaw_action_event_cb(lv_event_t *event)
{
    picoclaw_action_t action =
        (picoclaw_action_t)(intptr_t)lv_event_get_user_data(event);

    picoclaw_start_action(action, NULL);
}

static void picoclaw_weixin_button_event_cb(lv_event_t *event)
{
    (void)event;

    if(picoclaw_busy) {
        return;
    }
    if(picoclaw_weixin_ready) {
        picoclaw_start_action(PICOCLAW_ACTION_WEIXIN_UNBIND, NULL);
    } else if(picoclaw_weixin_auth_running) {
        picoclaw_start_action(PICOCLAW_ACTION_WEIXIN_CANCEL, NULL);
    } else {
        picoclaw_start_action(PICOCLAW_ACTION_WEIXIN_AUTH, NULL);
    }
}

static void picoclaw_open_settings_event_cb(lv_event_t *event)
{
    (void)event;
    ui_input_hide_inline_active();
    picoclaw_view = PICOCLAW_VIEW_SETTINGS;
    app_refresh_current_page();
}

static void picoclaw_field_submit_cb(const char *text, void *user_data)
{
    picoclaw_field_t field = (picoclaw_field_t)(intptr_t)user_data;

    switch(field) {
    case PICOCLAW_FIELD_MODEL_NAME:
        ui_prefs_set(PICOCLAW_PRESET_KEY, "3");
        ui_prefs_set(PICOCLAW_MODEL_NAME_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_MODEL_ID:
        ui_prefs_set(PICOCLAW_PRESET_KEY, "3");
        ui_prefs_set(PICOCLAW_MODEL_ID_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_API_BASE:
        ui_prefs_set(PICOCLAW_PRESET_KEY, "3");
        ui_prefs_set(PICOCLAW_API_BASE_KEY, text ? text : "");
        break;
    case PICOCLAW_FIELD_API_KEY:
        ui_prefs_set(PICOCLAW_API_KEY_KEY, text ? text : "");
        break;
    default:
        return;
    }

    pthread_mutex_lock(&picoclaw_lock);
    snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
             "%s", "Setting saved");
    pthread_mutex_unlock(&picoclaw_lock);
    picoclaw_start_action(PICOCLAW_ACTION_SAVE_CONFIG, NULL);
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
                           "custom-agent");
        cfg.title = "Model name";
        cfg.placeholder = "custom-agent";
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
        value[0] = '\0';
        cfg.title = "API key";
        cfg.placeholder = "sk-...";
        cfg.password_mode = 1;
        break;
    default:
        return;
    }

    cfg.initial_text = value;
    cfg.max_length = 480;
    cfg.submit_cb = picoclaw_field_submit_cb;
    cfg.user_data = (void *)(intptr_t)field;
    cfg.submit_text = "Save";
    cfg.cancel_text = "Cancel";
    ui_input_dialog_open(&cfg);
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

static void picoclaw_make_info_pair(lv_obj_t *parent, int x, int y, int w,
                                    const char *name, lv_obj_t **value_out,
                                    uint32_t color)
{
    lv_obj_t *name_label = ui_label(parent, name, &lv_font_montserrat_16,
                                    0x9AA4AF);
    lv_obj_t *value_label = ui_label(parent, "--", &lv_font_montserrat_18,
                                     color);
    int name_w = w > 360 ? 140 : 112;
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

static void picoclaw_apply_preset_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    const picoclaw_preset_t *preset = picoclaw_preset_at(index);
    char value[16];

    snprintf(value, sizeof(value), "%d", index);
    ui_prefs_set(PICOCLAW_PRESET_KEY, value);
    if(!preset->custom) {
        ui_prefs_set(PICOCLAW_MODEL_NAME_KEY, preset->model_name);
        ui_prefs_set(PICOCLAW_MODEL_ID_KEY, preset->model_id);
        ui_prefs_set(PICOCLAW_API_BASE_KEY, preset->api_base);
    } else {
        char current[256];

        picoclaw_load_pref(current, sizeof(current), PICOCLAW_MODEL_NAME_KEY,
                           "");
        if(!current[0]) {
            ui_prefs_set(PICOCLAW_MODEL_NAME_KEY, preset->model_name);
        }
        picoclaw_load_pref(current, sizeof(current), PICOCLAW_MODEL_ID_KEY,
                           "");
        if(!current[0]) {
            ui_prefs_set(PICOCLAW_MODEL_ID_KEY, preset->model_id);
        }
        picoclaw_load_pref(current, sizeof(current), PICOCLAW_API_BASE_KEY,
                           "");
        if(!current[0]) {
            ui_prefs_set(PICOCLAW_API_BASE_KEY, preset->api_base);
        }
    }

    pthread_mutex_lock(&picoclaw_lock);
    snprintf(picoclaw_status_text, sizeof(picoclaw_status_text),
             "%s", "Model preset saved");
    pthread_mutex_unlock(&picoclaw_lock);
    picoclaw_start_action(PICOCLAW_ACTION_SAVE_CONFIG, NULL);
    app_refresh_current_page();
}

static lv_obj_t *picoclaw_preset_card(lv_obj_t *parent, int x, int y, int w,
                                      int index, int selected)
{
    const picoclaw_preset_t *preset = picoclaw_preset_at(index);
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *mark;
    char desc[360];

    card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, 78);
    lv_obj_set_style_bg_color(card,
                              lv_color_hex(selected ? 0x16352A : 0x141B23), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card,
                                  lv_color_hex(selected ? 0x25C281 :
                                               0x2A3441), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, 6);
    lv_obj_add_event_cb(card, picoclaw_apply_preset_event_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)index);

    title = ui_label(card, preset->display, &lv_font_montserrat_20,
                     selected ? 0xD1FAE5 : 0xF2F5F8);
    lv_obj_set_pos(title, 14, 10);
    lv_obj_set_width(title, w - 72);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    ui_make_click_forwarder(title);

    snprintf(desc, sizeof(desc), "%s  %s", preset->model_id,
             preset->api_base);
    sub = ui_label(card, desc, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_pos(sub, 14, 44);
    lv_obj_set_width(sub, w - 28);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    ui_make_click_forwarder(sub);

    if(selected) {
        mark = ui_label(card, LV_SYMBOL_OK, &lv_font_montserrat_20,
                        0x25C281);
        lv_obj_align(mark, LV_ALIGN_TOP_RIGHT, -14, 12);
        ui_make_click_forwarder(mark);
    }
    return card;
}

static void ui_picoclaw_create_chat(lv_obj_t *scr)
{
    lv_obj_t *send_label;
    int top_y = ui_is_landscape() ? 64 : 124;
    int x = ui_page_panel_x();
    int content_w = ui_page_panel_width();
    const picoclaw_preset_t *preset = picoclaw_current_preset();

    ui_create_header(scr, "PicoClaw");
    picoclaw_body = ui_page_body(scr, top_y);
    lv_obj_set_scrollbar_mode(picoclaw_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(picoclaw_body, LV_OBJ_FLAG_SCROLLABLE);
    picoclaw_keyboard_reserved_h = 0;

    picoclaw_status_panel = ui_panel(picoclaw_body, x, 0, content_w, 108);
    lv_obj_set_style_bg_color(picoclaw_status_panel, lv_color_hex(0x0F172A),
                              0);
    lv_obj_set_style_pad_all(picoclaw_status_panel, 0, 0);

    picoclaw_status_label = ui_label(picoclaw_status_panel, "Ready",
                                     &lv_font_montserrat_20, 0x25C281);
    lv_label_set_long_mode(picoclaw_status_label, LV_LABEL_LONG_DOT);

    picoclaw_model_label = ui_label(picoclaw_status_panel, preset->display,
                                    &lv_font_montserrat_16, 0xCBD5E1);
    lv_label_set_long_mode(picoclaw_model_label, LV_LABEL_LONG_DOT);

    picoclaw_detail_label = ui_label(picoclaw_status_panel, "",
                                     &lv_font_montserrat_14, 0x94A3B8);
    lv_label_set_long_mode(picoclaw_detail_label, LV_LABEL_LONG_DOT);

    picoclaw_settings_btn = ui_command_button(picoclaw_status_panel, 0, 0, 56,
                                              LV_SYMBOL_SETTINGS, 0xA78BFA);
    lv_obj_add_event_cb(picoclaw_settings_btn, picoclaw_open_settings_event_cb,
                        LV_EVENT_CLICKED, NULL);

    picoclaw_chat_scroll = ui_panel(picoclaw_body, x, 0, content_w, 300);
    lv_obj_set_style_bg_color(picoclaw_chat_scroll, lv_color_hex(0x101820),
                              0);
    lv_obj_set_style_pad_all(picoclaw_chat_scroll, 12, 0);
    lv_obj_set_style_pad_row(picoclaw_chat_scroll, 0, 0);
    ui_make_scrollable(picoclaw_chat_scroll, 20);
    lv_obj_set_flex_flow(picoclaw_chat_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(picoclaw_chat_scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    picoclaw_input_panel = ui_panel(picoclaw_body, x, 0, content_w, 66);
    lv_obj_set_style_bg_color(picoclaw_input_panel, lv_color_hex(0x0F172A),
                              0);
    lv_obj_set_style_pad_all(picoclaw_input_panel, 0, 0);

    picoclaw_textarea = lv_textarea_create(picoclaw_input_panel);
    lv_textarea_set_one_line(picoclaw_textarea, true);
    lv_textarea_set_placeholder_text(picoclaw_textarea,
                                     ui_tr("Type message"));
    lv_textarea_set_max_length(picoclaw_textarea, 900);
    lv_obj_set_style_text_font(picoclaw_textarea,
                               ui_font_for_text("input",
                                                &lv_font_montserrat_18), 0);
    lv_obj_set_style_bg_color(picoclaw_textarea, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_text_color(picoclaw_textarea, lv_color_hex(0xF2F5F8),
                                0);
    lv_obj_set_style_radius(picoclaw_textarea, 8, 0);
    lv_obj_set_style_border_width(picoclaw_textarea, 1, 0);
    lv_obj_set_style_border_color(picoclaw_textarea, lv_color_hex(0x2A3A4A),
                                  0);
    lv_obj_set_style_border_color(picoclaw_textarea, lv_color_hex(0x25C281),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(picoclaw_textarea, 14, 0);
    lv_obj_set_style_pad_right(picoclaw_textarea, 14, 0);
    lv_obj_set_style_pad_top(picoclaw_textarea, 8, 0);
    lv_obj_set_style_pad_bottom(picoclaw_textarea, 8, 0);
    lv_obj_add_event_cb(picoclaw_textarea, picoclaw_input_focus_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(picoclaw_textarea, picoclaw_input_focus_event_cb,
                        LV_EVENT_FOCUSED, NULL);

    picoclaw_send_btn = ui_command_button(picoclaw_input_panel, 0, 0, 78,
                                          "Send", 0x25C281);
    lv_obj_add_event_cb(picoclaw_send_btn, picoclaw_send_event_cb,
                        LV_EVENT_CLICKED, NULL);
    send_label = lv_obj_get_child(picoclaw_send_btn, 0);
    if(send_label) {
        lv_obj_set_style_text_font(send_label,
                                   ui_font_for_text("Send",
                                                    &lv_font_montserrat_18),
                                   0);
    }

    picoclaw_inline_input =
        ui_input_inline_create(picoclaw_textarea, scr, 900,
                               picoclaw_send_submit_cb, NULL,
                               picoclaw_inline_layout_cb, NULL);

    picoclaw_layout_main();
    picoclaw_chat_rebuild(1);
    picoclaw_focus_input_if_hardware_keyboard();
}

static void ui_picoclaw_create_settings(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *section;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *actions;
    lv_obj_t *custom;
    int top_y = ui_is_landscape() ? 64 : 124;
    int body_x = ui_page_panel_x();
    int body_w = ui_page_panel_width();
    int content_w;
    int inner_w;
    int selected = picoclaw_current_preset_index();
    int preset_count =
        (int)(sizeof(picoclaw_presets) / sizeof(picoclaw_presets[0]));
    int i;
    int card_gap = 10;
    int btn_w;
    int section_h;

    ui_create_header(scr, "PicoClaw settings");
    body = ui_scroll_panel(scr, body_x, top_y, body_w, ui_body_height(top_y));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, 16, 0);
    content_w = ui_safe_content_width(body, body_w - 32);
    inner_w = content_w - 32;
    if(inner_w < 240) {
        inner_w = content_w;
    }

    section = ui_panel(body, 0, 0, content_w, 116);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x0F172A), 0);
    title = ui_label(section, "PicoClaw settings", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(section,
                        "Choose a preset model, save the API key, or run gateway tools.",
                        &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(subtitle, 0, 42);
    lv_obj_set_width(subtitle, inner_w);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);

    section_h = 54 + preset_count * 78 + (preset_count - 1) * card_gap;
    section = ui_panel(body, 0, 0, content_w, section_h);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x111820), 0);
    title = ui_label(section, "Model preset", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    for(i = 0; i < preset_count; i++) {
        picoclaw_preset_card(section, 0, 44 + i * (78 + card_gap),
                             inner_w, i, i == selected);
    }

    custom = ui_panel(body, 0, 0, content_w, ui_is_landscape() ? 112 : 176);
    lv_obj_set_style_bg_color(custom, lv_color_hex(0x111820), 0);
    title = ui_label(custom, "Custom model", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    btn_w = ui_is_landscape() ? (inner_w - 36) / 4 :
            (inner_w - 12) / 2;
    if(btn_w < 118) {
        btn_w = inner_w;
    }
    picoclaw_small_button(custom, 0, 48, btn_w, "Model name", 0x3DA5FF,
                          picoclaw_config_event_cb,
                          (void *)(intptr_t)PICOCLAW_FIELD_MODEL_NAME);
    if(btn_w * 2 + 12 <= inner_w) {
        picoclaw_small_button(custom, btn_w + 12, 48, btn_w, "Model ID",
                              0x3DA5FF, picoclaw_config_event_cb,
                              (void *)(intptr_t)PICOCLAW_FIELD_MODEL_ID);
        if(ui_is_landscape() && btn_w * 4 + 36 <= inner_w) {
            picoclaw_small_button(custom, (btn_w + 12) * 2, 48, btn_w,
                                  "API base", 0x3DA5FF,
                                  picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_API_BASE);
            picoclaw_small_button(custom, (btn_w + 12) * 3, 48, btn_w,
                                  "API key", 0x3DA5FF,
                                  picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_API_KEY);
        } else {
            picoclaw_small_button(custom, 0, 112, btn_w, "API base",
                                  0x3DA5FF, picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_API_BASE);
            picoclaw_small_button(custom, btn_w + 12, 112, btn_w, "API key",
                                  0x3DA5FF, picoclaw_config_event_cb,
                                  (void *)(intptr_t)PICOCLAW_FIELD_API_KEY);
        }
    }

    actions = ui_panel(body, 0, 0, content_w, ui_is_landscape() ? 182 : 244);
    lv_obj_set_style_bg_color(actions, lv_color_hex(0x111820), 0);
    title = ui_label(actions, "Runtime", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    btn_w = ui_is_landscape() ? (inner_w - 48) / 4 :
            (inner_w - 12) / 2;
    if(btn_w < 116) {
        btn_w = inner_w;
    }
    picoclaw_install_btn =
        picoclaw_small_button(actions, 0, 48, btn_w, "Install", 0x38BDF8,
                              picoclaw_action_event_cb,
                              (void *)(intptr_t)PICOCLAW_ACTION_INSTALL);
    if(btn_w * 2 + 12 <= inner_w) {
        picoclaw_save_btn =
            picoclaw_small_button(actions, btn_w + 12, 48, btn_w,
                                  "Save config", 0x25C281,
                                  picoclaw_action_event_cb,
                                  (void *)(intptr_t)PICOCLAW_ACTION_SAVE_CONFIG);
        if(ui_is_landscape() && btn_w * 4 + 48 <= inner_w) {
            picoclaw_gateway_start_btn =
                picoclaw_small_button(actions, (btn_w + 16) * 2, 48, btn_w,
                                      "Start gateway", 0x60A5FA,
                                      picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_START);
            picoclaw_gateway_stop_btn =
                picoclaw_small_button(actions, (btn_w + 16) * 3, 48, btn_w,
                                      "Stop gateway", 0xEF4D5A,
                                      picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_STOP);
            picoclaw_log_btn =
                picoclaw_small_button(actions, 0, 112, btn_w, "Log",
                                      0x94A3B8, picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_LOG);
        } else {
            picoclaw_gateway_start_btn =
                picoclaw_small_button(actions, 0, 112, btn_w,
                                      "Start gateway", 0x60A5FA,
                                      picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_START);
            picoclaw_gateway_stop_btn =
                picoclaw_small_button(actions, btn_w + 12, 112, btn_w,
                                      "Stop gateway", 0xEF4D5A,
                                      picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_GATEWAY_STOP);
            picoclaw_log_btn =
                picoclaw_small_button(actions, 0, 176, btn_w, "Log",
                                      0x94A3B8, picoclaw_action_event_cb,
                                      (void *)(intptr_t)PICOCLAW_ACTION_LOG);
        }
    }

    section = ui_panel(body, 0, 0, content_w, 200);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x111820), 0);
    title = ui_label(section, "Chat apps", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(section,
                        "Scan WeChat QR code, then start the gateway.",
                        &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(subtitle, 0, 34);
    lv_obj_set_width(subtitle, inner_w);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    picoclaw_weixin_label = ui_label(section, "missing / stopped",
                                     &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(picoclaw_weixin_label, 0, 62);
    lv_obj_set_width(picoclaw_weixin_label, inner_w);
    lv_label_set_long_mode(picoclaw_weixin_label, LV_LABEL_LONG_DOT);
    btn_w = inner_w;
    if(ui_is_landscape() && btn_w > 260) {
        btn_w = 260;
    }
    picoclaw_weixin_auth_btn =
        picoclaw_small_button(section, (inner_w - btn_w) / 2, 104, btn_w,
                              "Weixin login", 0x22C55E,
                              picoclaw_weixin_button_event_cb, NULL);
    picoclaw_weixin_status_btn = NULL;
    picoclaw_weixin_cancel_btn = NULL;

    section = ui_panel(body, 0, 0, content_w, 280);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x0D1117), 0);
    title = ui_label(section, "Status", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    picoclaw_make_info_pair(section, 0, 44, inner_w, "Version",
                            &picoclaw_version_label, 0xF2F5F8);
    picoclaw_make_info_pair(section, 0, 82, inner_w, "Gateway",
                            &picoclaw_gateway_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 120, inner_w, "Network",
                            &picoclaw_network_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 158, inner_w, "Config",
                            &picoclaw_config_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 196, inner_w, "Model key",
                            &picoclaw_model_key_label, 0x9AA4AF);
    picoclaw_make_info_pair(section, 0, 234, inner_w, "URL",
                            &picoclaw_url_label, 0x38BDF8);

    section = ui_panel(body, 0, 0, content_w, ui_is_landscape() ? 190 : 260);
    lv_obj_set_style_bg_color(section, lv_color_hex(0x0D1117), 0);
    title = ui_label(section, "Output", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, inner_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    picoclaw_output_label = ui_label(section, "No output yet",
                                     &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(picoclaw_output_label, 0, 42);
    lv_obj_set_width(picoclaw_output_label, inner_w);
    lv_label_set_long_mode(picoclaw_output_label, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(body);
}

void ui_picoclaw_create(lv_obj_t *scr)
{
    if(picoclaw_view == PICOCLAW_VIEW_SETTINGS) {
        ui_picoclaw_create_settings(scr);
    } else {
        ui_picoclaw_create_chat(scr);
    }
    picoclaw_timer = lv_timer_create(picoclaw_timer_cb, 250, NULL);
    picoclaw_start_action(PICOCLAW_ACTION_STATUS, NULL);
    picoclaw_update_ui();
}

int ui_picoclaw_handle_back(void)
{
    if(picoclaw_weixin_qr_overlay &&
       lv_obj_is_valid(picoclaw_weixin_qr_overlay)) {
        picoclaw_weixin_qr_close();
        return 1;
    }
    if(picoclaw_view == PICOCLAW_VIEW_SETTINGS) {
        picoclaw_view = PICOCLAW_VIEW_CHAT;
        app_refresh_current_page();
        return 1;
    }
    return 0;
}

void ui_picoclaw_cleanup(void)
{
    if(picoclaw_timer) {
        lv_timer_delete(picoclaw_timer);
        picoclaw_timer = NULL;
    }
    if(picoclaw_inline_input) {
        ui_input_inline_destroy(picoclaw_inline_input);
        picoclaw_inline_input = NULL;
    }
    picoclaw_weixin_qr_close();
    picoclaw_body = NULL;
    picoclaw_status_panel = NULL;
    picoclaw_chat_scroll = NULL;
    picoclaw_input_panel = NULL;
    picoclaw_textarea = NULL;
    picoclaw_send_btn = NULL;
    picoclaw_settings_btn = NULL;
    picoclaw_status_label = NULL;
    picoclaw_model_label = NULL;
    picoclaw_detail_label = NULL;
    picoclaw_version_label = NULL;
    picoclaw_gateway_label = NULL;
    picoclaw_network_label = NULL;
    picoclaw_url_label = NULL;
    picoclaw_config_label = NULL;
    picoclaw_model_key_label = NULL;
    picoclaw_weixin_label = NULL;
    picoclaw_output_label = NULL;
    picoclaw_install_btn = NULL;
    picoclaw_save_btn = NULL;
    picoclaw_gateway_start_btn = NULL;
    picoclaw_gateway_stop_btn = NULL;
    picoclaw_weixin_auth_btn = NULL;
    picoclaw_weixin_status_btn = NULL;
    picoclaw_weixin_cancel_btn = NULL;
    picoclaw_log_btn = NULL;
    picoclaw_keyboard_reserved_h = 0;
}
