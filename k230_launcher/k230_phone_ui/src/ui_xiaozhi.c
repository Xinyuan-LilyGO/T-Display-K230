#include "ui_xiaozhi.h"

#include "ui_common.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define XIAOZHI_BIN "/root/app/k230_phone_ui/k230_xiaozhi_probe"
#define XIAOZHI_LOG "/tmp/k230_xiaozhi_ui.log"
#define XIAOZHI_LOG_TEXT_MAX 4096
#define XIAOZHI_DEFAULT_URL "wss://api.tenclass.net:443/xiaozhi/v1/"
#define XIAOZHI_PREF_URL "xiaozhi.url"
#define XIAOZHI_PREF_TOKEN "xiaozhi.token"
#define XIAOZHI_PREF_VALUE_MAX 160

typedef enum {
    XIAOZHI_ACTION_PROBE = 0,
    XIAOZHI_ACTION_AUDIO,
    XIAOZHI_ACTION_PTT,
} xiaozhi_action_t;

static pthread_mutex_t xiaozhi_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *xiaozhi_timer;
static lv_obj_t *xiaozhi_status_label;
static lv_obj_t *xiaozhi_detail_label;
static lv_obj_t *xiaozhi_log_label;
static lv_obj_t *xiaozhi_probe_btn;
static lv_obj_t *xiaozhi_audio_btn;
static lv_obj_t *xiaozhi_ptt_btn;
static lv_obj_t *xiaozhi_url_label;
static lv_obj_t *xiaozhi_token_label;
static int xiaozhi_running;
static int xiaozhi_last_rc;
static pid_t xiaozhi_ptt_pid = -1;
static int xiaozhi_ptt_recording;
static char xiaozhi_status_text[192] = "Ready";
static char xiaozhi_detail_text[256] =
    "Probe cloud handshake, test audio loopback, or send one short PTT turn.";
static char xiaozhi_log_text[XIAOZHI_LOG_TEXT_MAX] = "No log yet";

static void xiaozhi_get_url(char *buf, size_t len)
{
    if(!buf || len == 0) {
        return;
    }

    ui_prefs_get(XIAOZHI_PREF_URL, buf, len, XIAOZHI_DEFAULT_URL);
    if(!buf[0]) {
        snprintf(buf, len, "%s", XIAOZHI_DEFAULT_URL);
    }
}

static void xiaozhi_get_token(char *buf, size_t len)
{
    if(!buf || len == 0) {
        return;
    }

    ui_prefs_get(XIAOZHI_PREF_TOKEN, buf, len, "");
}

static void xiaozhi_shell_quote(char *out, size_t out_len, const char *in)
{
    size_t used = 0;

    if(!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if(out_len < 3) {
        return;
    }

    out[used++] = '\'';
    out[used] = '\0';
    for(const char *p = in ? in : ""; *p && used + 5U < out_len; p++) {
        if(*p == '\'') {
            memcpy(out + used, "'\\''", 4);
            used += 4;
        } else {
            out[used++] = *p;
        }
        out[used] = '\0';
    }
    if(used + 2U <= out_len) {
        out[used++] = '\'';
        out[used] = '\0';
    }
}

static void xiaozhi_env_prefix(char *out, size_t len)
{
    char url[XIAOZHI_PREF_VALUE_MAX];
    char token[XIAOZHI_PREF_VALUE_MAX];
    char url_q[XIAOZHI_PREF_VALUE_MAX * 5];
    char token_q[XIAOZHI_PREF_VALUE_MAX * 5];

    xiaozhi_get_url(url, sizeof(url));
    xiaozhi_get_token(token, sizeof(token));
    xiaozhi_shell_quote(url_q, sizeof(url_q), url);
    xiaozhi_shell_quote(token_q, sizeof(token_q), token);
    snprintf(out, len, "env XIAOZHI_URL=%s XIAOZHI_TOKEN=%s", url_q,
             token_q);
}

static void xiaozhi_update_settings_labels(void)
{
    char url[XIAOZHI_PREF_VALUE_MAX];
    char token[XIAOZHI_PREF_VALUE_MAX];
    char text[240];

    xiaozhi_get_url(url, sizeof(url));
    xiaozhi_get_token(token, sizeof(token));

    if(xiaozhi_url_label) {
        snprintf(text, sizeof(text), "%s: %s", ui_tr("Server URL"), url);
        lv_label_set_text(xiaozhi_url_label, text);
    }
    if(xiaozhi_token_label) {
        snprintf(text, sizeof(text), "%s: %s", ui_tr("Token"),
                 token[0] ? "********" : ui_tr("Not set"));
        lv_label_set_text(xiaozhi_token_label, text);
    }
}

static void xiaozhi_set_status_locked(const char *status, const char *detail,
                                      int rc)
{
    if(status) {
        snprintf(xiaozhi_status_text, sizeof(xiaozhi_status_text), "%s",
                 status);
    }
    if(detail) {
        snprintf(xiaozhi_detail_text, sizeof(xiaozhi_detail_text), "%s",
                 detail);
    }
    xiaozhi_last_rc = rc;
}

static void xiaozhi_append_log_locked(const char *line)
{
    FILE *fp;
    size_t old_len;
    size_t add_len;

    if(!line || !line[0]) {
        return;
    }

    fp = fopen(XIAOZHI_LOG, "a");
    if(fp) {
        fprintf(fp, "%s\n", line);
        fclose(fp);
    }

    if(strcmp(xiaozhi_log_text, "No log yet") == 0) {
        xiaozhi_log_text[0] = '\0';
    }

    old_len = strlen(xiaozhi_log_text);
    add_len = strlen(line);
    if(old_len + add_len + 2U >= sizeof(xiaozhi_log_text)) {
        size_t keep_from = old_len > sizeof(xiaozhi_log_text) / 2U ?
                           old_len - sizeof(xiaozhi_log_text) / 2U : 0;
        memmove(xiaozhi_log_text, xiaozhi_log_text + keep_from,
                old_len - keep_from + 1U);
        old_len = strlen(xiaozhi_log_text);
    }

    snprintf(xiaozhi_log_text + old_len,
             sizeof(xiaozhi_log_text) - old_len, "%s%s",
             old_len ? "\n" : "", line);
}

static void xiaozhi_reload_log_tail_locked(void)
{
    FILE *fp;
    char line[320];
    char tail[XIAOZHI_LOG_TEXT_MAX] = "";
    size_t used = 0;

    fp = fopen(XIAOZHI_LOG, "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        size_t line_len;

        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }

        line_len = strlen(line);
        if(used + line_len + 2U >= sizeof(tail)) {
            size_t drop = sizeof(tail) / 3U;
            if(drop < used) {
                memmove(tail, tail + drop, strlen(tail + drop) + 1U);
                used = strlen(tail);
            } else {
                tail[0] = '\0';
                used = 0;
            }
        }
        used += snprintf(tail + used, sizeof(tail) - used, "%s%s",
                         used ? "\n" : "", line);
        if(used >= sizeof(tail)) {
            used = sizeof(tail) - 1U;
        }
    }
    fclose(fp);

    if(tail[0]) {
        snprintf(xiaozhi_log_text, sizeof(xiaozhi_log_text), "%s", tail);
    }
}

static const char *xiaozhi_action_name(xiaozhi_action_t action)
{
    switch(action) {
    case XIAOZHI_ACTION_PROBE:
        return "Probe";
    case XIAOZHI_ACTION_AUDIO:
        return "Audio loopback";
    case XIAOZHI_ACTION_PTT:
        return "PTT";
    default:
        return "Unknown";
    }
}

static void xiaozhi_build_command(xiaozhi_action_t action, char *cmd,
                                  size_t len)
{
    char env_prefix[1800];

    xiaozhi_env_prefix(env_prefix, sizeof(env_prefix));
    switch(action) {
    case XIAOZHI_ACTION_PROBE:
        snprintf(cmd, len, "%s %s probe --timeout-ms 8000 2>&1",
                 env_prefix, XIAOZHI_BIN);
        break;
    case XIAOZHI_ACTION_AUDIO:
        snprintf(cmd, len, "%s %s audio-loopback --seconds 1 2>&1",
                 env_prefix, XIAOZHI_BIN);
        break;
    case XIAOZHI_ACTION_PTT:
        snprintf(cmd, len,
                 "%s %s ptt --seconds 3 --wait 15 --timeout-ms 10000 2>&1",
                 env_prefix, XIAOZHI_BIN);
        break;
    default:
        snprintf(cmd, len, "%s %s probe --timeout-ms 8000 2>&1",
                 env_prefix, XIAOZHI_BIN);
        break;
    }
}

static void *xiaozhi_worker(void *arg)
{
    xiaozhi_action_t action = (xiaozhi_action_t)(intptr_t)arg;
    char cmd[2300];
    char line[512];
    FILE *fp;
    int rc = -1;

    if(access(XIAOZHI_BIN, X_OK) != 0) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Helper missing",
                                  "k230_xiaozhi_probe is not installed.",
                                  127);
        xiaozhi_running = 0;
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return NULL;
    }

    xiaozhi_build_command(action, cmd, sizeof(cmd));
    fp = popen(cmd, "r");
    if(!fp) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Start failed", "popen failed", -1);
        xiaozhi_running = 0;
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return NULL;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_append_log_locked(line);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
    }

    rc = ui_shell_exit_code(pclose(fp));
    pthread_mutex_lock(&xiaozhi_lock);
    xiaozhi_running = 0;
    if(rc == 0) {
        xiaozhi_set_status_locked("Completed", xiaozhi_action_name(action),
                                  rc);
    } else {
        xiaozhi_set_status_locked("Failed, check log",
                                  xiaozhi_action_name(action), rc);
    }
    pthread_mutex_unlock(&xiaozhi_lock);
    app_request_fast_refresh();
    return NULL;
}

static void xiaozhi_start_action(xiaozhi_action_t action)
{
    pthread_t thread;
    char header[128];

    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_running) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    xiaozhi_running = 1;
    xiaozhi_set_status_locked("Running", xiaozhi_action_name(action), 0);
    snprintf(header, sizeof(header), "== %s ==", xiaozhi_action_name(action));
    xiaozhi_append_log_locked(header);
    pthread_mutex_unlock(&xiaozhi_lock);

    if(pthread_create(&thread, NULL, xiaozhi_worker,
                      (void *)(intptr_t)action) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_running = 0;
        xiaozhi_set_status_locked("Thread failed", xiaozhi_action_name(action),
                                  -1);
        pthread_mutex_unlock(&xiaozhi_lock);
    }
}

static void xiaozhi_probe_event_cb(lv_event_t *event)
{
    (void)event;
    xiaozhi_start_action(XIAOZHI_ACTION_PROBE);
}

static void xiaozhi_audio_event_cb(lv_event_t *event)
{
    (void)event;
    xiaozhi_start_action(XIAOZHI_ACTION_AUDIO);
}

static void xiaozhi_start_ptt_hold(void)
{
    FILE *fp;
    char cmd[2300];
    char env_prefix[1800];
    char line[64];
    char header[128];
    long pid;

    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_running || xiaozhi_ptt_pid > 0) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    pthread_mutex_unlock(&xiaozhi_lock);

    if(access(XIAOZHI_BIN, X_OK) != 0) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Helper missing",
                                  "k230_xiaozhi_probe is not installed.",
                                  127);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    fp = fopen(XIAOZHI_LOG, "w");
    if(fp) {
        fclose(fp);
    }

    xiaozhi_env_prefix(env_prefix, sizeof(env_prefix));
    snprintf(cmd, sizeof(cmd),
             "%s %s ptt --seconds 30 --wait 15 --timeout-ms 10000 "
             ">> %s 2>&1 & echo $!",
             env_prefix, XIAOZHI_BIN, XIAOZHI_LOG);

    fp = popen(cmd, "r");
    if(!fp || !fgets(line, sizeof(line), fp)) {
        if(fp) {
            pclose(fp);
        }
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Start failed", "popen failed", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }
    pclose(fp);

    pid = strtol(line, NULL, 10);
    if(pid <= 0) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Start failed", "Invalid PTT pid", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    pthread_mutex_lock(&xiaozhi_lock);
    xiaozhi_running = 1;
    xiaozhi_ptt_pid = (pid_t)pid;
    xiaozhi_ptt_recording = 1;
    xiaozhi_log_text[0] = '\0';
    xiaozhi_set_status_locked("Recording", "Release to send", 0);
    snprintf(header, sizeof(header), "== Hold PTT pid=%ld ==", pid);
    xiaozhi_append_log_locked(header);
    pthread_mutex_unlock(&xiaozhi_lock);
    app_request_fast_refresh();
}

static void xiaozhi_stop_ptt_hold(void)
{
    pid_t pid;

    pthread_mutex_lock(&xiaozhi_lock);
    pid = xiaozhi_ptt_pid;
    if(pid <= 0 || !xiaozhi_ptt_recording) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    xiaozhi_ptt_recording = 0;
    xiaozhi_set_status_locked("Sending", "Waiting for reply", 0);
    pthread_mutex_unlock(&xiaozhi_lock);

    kill(pid, SIGUSR1);
    app_request_fast_refresh();
}

static void xiaozhi_ptt_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_PRESSED) {
        xiaozhi_start_ptt_hold();
        lv_event_stop_processing(event);
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        xiaozhi_stop_ptt_hold();
        lv_event_stop_processing(event);
    }
}

static void xiaozhi_save_status(const char *status, const char *detail)
{
    pthread_mutex_lock(&xiaozhi_lock);
    xiaozhi_set_status_locked(status, detail, 0);
    xiaozhi_append_log_locked(detail);
    pthread_mutex_unlock(&xiaozhi_lock);
    xiaozhi_update_settings_labels();
    app_request_fast_refresh();
}

static void xiaozhi_url_submit_cb(const char *text, void *user_data)
{
    char value[XIAOZHI_PREF_VALUE_MAX];

    (void)user_data;
    snprintf(value, sizeof(value), "%s", text ? text : "");
    ui_trim_text(value);
    if(!value[0]) {
        snprintf(value, sizeof(value), "%s", XIAOZHI_DEFAULT_URL);
    }
    if(strncmp(value, "wss://", 6) != 0 && strncmp(value, "ws://", 5) != 0) {
        xiaozhi_save_status("Invalid URL",
                            "URL must start with ws:// or wss://");
        return;
    }
    if(ui_prefs_set(XIAOZHI_PREF_URL, value) == 0) {
        xiaozhi_save_status("Saved", "Server URL saved");
    } else {
        xiaozhi_save_status("Save failed", "Server URL is too long");
    }
}

static void xiaozhi_token_submit_cb(const char *text, void *user_data)
{
    char value[XIAOZHI_PREF_VALUE_MAX];

    (void)user_data;
    snprintf(value, sizeof(value), "%s", text ? text : "");
    ui_trim_text(value);
    if(ui_prefs_set(XIAOZHI_PREF_TOKEN, value) == 0) {
        xiaozhi_save_status("Saved",
                            value[0] ? "Token saved" : "Token cleared");
    } else {
        xiaozhi_save_status("Save failed", "Token is too long");
    }
}

static void xiaozhi_open_url_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;
    char url[XIAOZHI_PREF_VALUE_MAX];

    (void)event;
    xiaozhi_get_url(url, sizeof(url));
    memset(&config, 0, sizeof(config));
    config.title = "Server URL";
    config.placeholder = XIAOZHI_DEFAULT_URL;
    config.initial_text = url;
    config.max_length = XIAOZHI_PREF_VALUE_MAX - 1U;
    config.submit_cb = xiaozhi_url_submit_cb;
    config.submit_text = "Save";
    ui_input_dialog_open(&config);
}

static void xiaozhi_open_token_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;
    char token[XIAOZHI_PREF_VALUE_MAX];

    (void)event;
    xiaozhi_get_token(token, sizeof(token));
    memset(&config, 0, sizeof(config));
    config.title = "Token";
    config.placeholder = "Bearer token";
    config.initial_text = token;
    config.password_mode = 1;
    config.max_length = XIAOZHI_PREF_VALUE_MAX - 1U;
    config.submit_cb = xiaozhi_token_submit_cb;
    config.submit_text = "Save";
    ui_input_dialog_open(&config);
}

static void xiaozhi_clear_token_event_cb(lv_event_t *event)
{
    (void)event;
    if(ui_prefs_set(XIAOZHI_PREF_TOKEN, "") == 0) {
        xiaozhi_save_status("Saved", "Token cleared");
    } else {
        xiaozhi_save_status("Save failed", "Token is too long");
    }
}

static void xiaozhi_update(void)
{
    int running;
    int rc;
    pid_t ptt_pid;
    char status[192];
    char detail[256];
    char log_text[XIAOZHI_LOG_TEXT_MAX];

    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_ptt_pid > 0) {
        xiaozhi_reload_log_tail_locked();
        if(kill(xiaozhi_ptt_pid, 0) != 0 && errno == ESRCH) {
            if(strstr(xiaozhi_log_text, "ptt done")) {
                xiaozhi_set_status_locked("Completed", "PTT", 0);
            } else {
                xiaozhi_set_status_locked("Failed, check log", "PTT", -1);
            }
            xiaozhi_ptt_pid = -1;
            xiaozhi_ptt_recording = 0;
            xiaozhi_running = 0;
        }
    }
    running = xiaozhi_running;
    rc = xiaozhi_last_rc;
    ptt_pid = xiaozhi_ptt_pid;
    snprintf(status, sizeof(status), "%s", xiaozhi_status_text);
    snprintf(detail, sizeof(detail), "%s", xiaozhi_detail_text);
    snprintf(log_text, sizeof(log_text), "%s", xiaozhi_log_text);
    pthread_mutex_unlock(&xiaozhi_lock);

    if(xiaozhi_status_label) {
        lv_label_set_text(xiaozhi_status_label, ui_tr(status));
        lv_obj_set_style_text_color(xiaozhi_status_label,
                                    lv_color_hex(running ? 0x3DA5FF :
                                                 (rc == 0 ? 0x25C281 :
                                                  0xF5A524)),
                                    0);
    }
    if(xiaozhi_detail_label) {
        lv_label_set_text(xiaozhi_detail_label, ui_tr(detail));
    }
    if(xiaozhi_log_label) {
        lv_label_set_text(xiaozhi_log_label, log_text);
    }
    if(xiaozhi_probe_btn) {
        running ? lv_obj_add_state(xiaozhi_probe_btn, LV_STATE_DISABLED) :
                  lv_obj_clear_state(xiaozhi_probe_btn, LV_STATE_DISABLED);
    }
    if(xiaozhi_audio_btn) {
        running ? lv_obj_add_state(xiaozhi_audio_btn, LV_STATE_DISABLED) :
                  lv_obj_clear_state(xiaozhi_audio_btn, LV_STATE_DISABLED);
    }
    if(xiaozhi_ptt_btn) {
        if(running && ptt_pid <= 0) {
            lv_obj_add_state(xiaozhi_ptt_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(xiaozhi_ptt_btn, LV_STATE_DISABLED);
        }
    }
}

static void xiaozhi_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    xiaozhi_update();
}

void ui_xiaozhi_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *log_title;
    lv_obj_t *btn;
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int body_y = ui_page_top_y(144);
    int landscape = ui_is_landscape();
    int panel_h = ui_body_height(body_y) - 42;
    int content_w = w - 32;
    int button_w = landscape ? (content_w - 32) / 3 : content_w;
    int settings_y = landscape ? 172 : 198;
    int button_y = landscape ? 246 : 420;
    int log_y = landscape ? 326 : 650;

    ui_create_header(scr, "Xiaozhi");

    body = ui_page_body(scr, 144);
    panel = ui_panel(body, x, 18, w, panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), 0);
    ui_make_scrollable(panel, 54);

    title = ui_label(panel, "Xiaozhi voice probe", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_width(title, content_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    xiaozhi_status_label = ui_label(panel, "Ready", &lv_font_montserrat_22,
                                    0x25C281);
    lv_obj_set_width(xiaozhi_status_label, content_w);
    lv_label_set_long_mode(xiaozhi_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_status_label, LV_ALIGN_TOP_LEFT, 0, 46);

    xiaozhi_detail_label = ui_label(panel,
                                    "Probe cloud handshake, test audio loopback, or send one short PTT turn.",
                                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(xiaozhi_detail_label, content_w);
    lv_label_set_long_mode(xiaozhi_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(xiaozhi_detail_label, LV_ALIGN_TOP_LEFT, 0, 84);

    xiaozhi_url_label = ui_label(panel, "", &lv_font_montserrat_14,
                                 0x9AA4AF);
    lv_obj_set_width(xiaozhi_url_label, content_w);
    lv_label_set_long_mode(xiaozhi_url_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_url_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 120 : 140);

    xiaozhi_token_label = ui_label(panel, "", &lv_font_montserrat_14,
                                   0x9AA4AF);
    lv_obj_set_width(xiaozhi_token_label, content_w);
    lv_label_set_long_mode(xiaozhi_token_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_token_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 144 : 164);

    if(landscape) {
        int cfg_w = (content_w - 32) / 3;

        btn = ui_command_button(panel, 0, settings_y, cfg_w, "Server",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, xiaozhi_open_url_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(panel, cfg_w + 16, settings_y, cfg_w, "Token",
                                0xA78BFA);
        lv_obj_add_event_cb(btn, xiaozhi_open_token_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(panel, (cfg_w + 16) * 2, settings_y, cfg_w,
                                "Clear token", 0xF97316);
        lv_obj_add_event_cb(btn, xiaozhi_clear_token_event_cb,
                            LV_EVENT_CLICKED, NULL);
    } else {
        btn = ui_command_button(panel, 0, settings_y, content_w, "Server",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, xiaozhi_open_url_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(panel, 0, settings_y + 74, content_w, "Token",
                                0xA78BFA);
        lv_obj_add_event_cb(btn, xiaozhi_open_token_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(panel, 0, settings_y + 148, content_w,
                                "Clear token", 0xF97316);
        lv_obj_add_event_cb(btn, xiaozhi_clear_token_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    if(landscape) {
        xiaozhi_probe_btn = ui_command_button(panel, 0, button_y, button_w,
                                              "Probe", 0x3DA5FF);
        xiaozhi_audio_btn = ui_command_button(panel, button_w + 16, button_y,
                                              button_w, "Audio", 0x22C55E);
        xiaozhi_ptt_btn = ui_command_button(panel, (button_w + 16) * 2,
                                            button_y, button_w, "PTT",
                                            0xF97316);
    } else {
        xiaozhi_probe_btn = ui_command_button(panel, 0, button_y, button_w,
                                              "Probe", 0x3DA5FF);
        xiaozhi_audio_btn = ui_command_button(panel, 0, button_y + 74,
                                              button_w, "Audio", 0x22C55E);
        xiaozhi_ptt_btn = ui_command_button(panel, 0, button_y + 148,
                                            button_w, "PTT", 0xF97316);
    }
    lv_obj_add_event_cb(xiaozhi_probe_btn, xiaozhi_probe_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(xiaozhi_audio_btn, xiaozhi_audio_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(xiaozhi_ptt_btn, xiaozhi_ptt_event_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(xiaozhi_ptt_btn, xiaozhi_ptt_event_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(xiaozhi_ptt_btn, xiaozhi_ptt_event_cb,
                        LV_EVENT_PRESS_LOST, NULL);

    log_title = ui_label(panel, "Log", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_align(log_title, LV_ALIGN_TOP_LEFT, 0, log_y);

    hint = ui_label(panel,
                    "Hold PTT to record, release to send Opus and play the reply.",
                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(hint, content_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, log_y + 32);

    xiaozhi_log_label = ui_label(panel, "No log yet", &lv_font_montserrat_14,
                                 0x9AA4AF);
    lv_obj_set_width(xiaozhi_log_label, content_w);
    lv_label_set_long_mode(xiaozhi_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(xiaozhi_log_label, LV_ALIGN_TOP_LEFT, 0, log_y + 68);

    xiaozhi_timer = lv_timer_create(xiaozhi_timer_cb, 500, NULL);
    xiaozhi_update_settings_labels();
    xiaozhi_update();
}

void ui_xiaozhi_cleanup(void)
{
    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_ptt_pid > 0) {
        kill(xiaozhi_ptt_pid, SIGTERM);
        xiaozhi_ptt_pid = -1;
        xiaozhi_ptt_recording = 0;
        xiaozhi_running = 0;
    }
    pthread_mutex_unlock(&xiaozhi_lock);

    if(xiaozhi_timer) {
        lv_timer_delete(xiaozhi_timer);
        xiaozhi_timer = NULL;
    }
    xiaozhi_status_label = NULL;
    xiaozhi_detail_label = NULL;
    xiaozhi_log_label = NULL;
    xiaozhi_probe_btn = NULL;
    xiaozhi_audio_btn = NULL;
    xiaozhi_ptt_btn = NULL;
    xiaozhi_url_label = NULL;
    xiaozhi_token_label = NULL;
}
