#include "ui_xiaozhi.h"

#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define XIAOZHI_BIN "/root/app/k230_phone_ui/k230_xiaozhi_probe"
#define XIAOZHI_LOG "/tmp/k230_xiaozhi_ui.log"
#define XIAOZHI_CTL "/tmp/k230_xiaozhi_session.ctl"
#define XIAOZHI_LOG_TEXT_MAX 4096
#define XIAOZHI_CHAT_TEXT_MAX 8192
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
static lv_timer_t *xiaozhi_record_overlay_timer;
static lv_obj_t *xiaozhi_status_panel;
static lv_obj_t *xiaozhi_chat_scroll;
static lv_obj_t *xiaozhi_action_panel;
static lv_obj_t *xiaozhi_record_overlay;
static lv_obj_t *xiaozhi_record_time_label;
static lv_obj_t *xiaozhi_record_level_label;
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
static pid_t xiaozhi_session_pid = -1;
static int xiaozhi_ptt_recording;
static int xiaozhi_session_ready;
static int xiaozhi_record_phase;
static uint64_t xiaozhi_record_start_us;
static char xiaozhi_status_text[192] = "Ready";
static char xiaozhi_detail_text[256] =
    "Probe cloud handshake, test audio loopback, or send one short PTT turn.";
static char xiaozhi_log_text[XIAOZHI_LOG_TEXT_MAX] = "No log yet";
static char xiaozhi_chat_text[XIAOZHI_CHAT_TEXT_MAX];
static char xiaozhi_rendered_chat_text[XIAOZHI_CHAT_TEXT_MAX];
static size_t xiaozhi_log_scan_len;

static void xiaozhi_set_status_locked(const char *status, const char *detail,
                                      int rc);
static void xiaozhi_append_log_locked(const char *line);
static void xiaozhi_record_overlay_close(void);

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

static void xiaozhi_apply_audio_output_route(void)
{
    int external = ui_audio_output_is_external() ? 1 : 0;

    ui_audio_output_set_external(external);
}

static int xiaozhi_process_alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int xiaozhi_write_control(const char *cmd)
{
    int fd;
    char line[96];
    ssize_t written;

    if(!cmd || !cmd[0]) {
        return -1;
    }

    fd = open(XIAOZHI_CTL, O_WRONLY | O_NONBLOCK);
    if(fd < 0) {
        return -1;
    }
    snprintf(line, sizeof(line), "%s\n", cmd);
    written = write(fd, line, strlen(line));
    close(fd);
    return written > 0 ? 0 : -1;
}

static void xiaozhi_start_session(void)
{
    FILE *fp;
    char cmd[2400];
    char env_prefix[1800];
    char ctl_q[128];
    char log_q[128];
    char line[64];
    long pid;

    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_process_alive(xiaozhi_session_pid)) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    xiaozhi_session_pid = -1;
    xiaozhi_session_ready = 0;
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

    unlink(XIAOZHI_CTL);
    if(mkfifo(XIAOZHI_CTL, 0600) != 0 && errno != EEXIST) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Start failed", "Control FIFO failed", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    fp = fopen(XIAOZHI_LOG, "w");
    if(fp) {
        fclose(fp);
    }

    xiaozhi_apply_audio_output_route();
    xiaozhi_env_prefix(env_prefix, sizeof(env_prefix));
    xiaozhi_shell_quote(ctl_q, sizeof(ctl_q), XIAOZHI_CTL);
    xiaozhi_shell_quote(log_q, sizeof(log_q), XIAOZHI_LOG);
    snprintf(cmd, sizeof(cmd),
             "%s %s session --control %s --seconds 30 --wait 15 "
             "--timeout-ms 10000 >> %s 2>&1 & echo $!",
             env_prefix, XIAOZHI_BIN, ctl_q, log_q);

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
        xiaozhi_set_status_locked("Start failed", "Invalid session pid", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    pthread_mutex_lock(&xiaozhi_lock);
    xiaozhi_session_pid = (pid_t)pid;
    xiaozhi_session_ready = 0;
    xiaozhi_set_status_locked("Connecting", "Session handshake", 0);
    xiaozhi_log_text[0] = '\0';
    xiaozhi_log_scan_len = 0;
    xiaozhi_append_log_locked("== Xiaozhi session starting ==");
    pthread_mutex_unlock(&xiaozhi_lock);
    app_request_fast_refresh();
}

static void xiaozhi_stop_session(void)
{
    pid_t pid;

    xiaozhi_record_overlay_close();
    pthread_mutex_lock(&xiaozhi_lock);
    pid = xiaozhi_session_pid;
    xiaozhi_session_pid = -1;
    xiaozhi_session_ready = 0;
    xiaozhi_ptt_recording = 0;
    xiaozhi_running = 0;
    pthread_mutex_unlock(&xiaozhi_lock);

    xiaozhi_write_control("QUIT");
    if(pid > 0) {
        kill(pid, SIGTERM);
    }
    unlink(XIAOZHI_CTL);
}

static void xiaozhi_restart_session(void)
{
    xiaozhi_stop_session();
    xiaozhi_start_session();
}

static void xiaozhi_truncate_log(void)
{
    FILE *fp = fopen(XIAOZHI_LOG, "w");
    if(fp) {
        fclose(fp);
    }
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

static const char *xiaozhi_chat_payload_from_log_line(const char *line,
                                                      char *role)
{
    const char *p;

    if(!line || !role) {
        return NULL;
    }

    p = strstr(line, "] ");
    p = p ? p + 2 : line;
    while(*p == ' ') {
        p++;
    }
    if(strncmp(p, "CHAT", 4) != 0) {
        return NULL;
    }
    p += 4;
    while(*p == ' ') {
        p++;
    }

    if(strncmp(p, "user:", 5) == 0) {
        *role = 'U';
        p += 5;
    } else if(strncmp(p, "assistant:", 10) == 0) {
        *role = 'A';
        p += 10;
    } else {
        return NULL;
    }

    while(*p == ' ') {
        p++;
    }
    return p;
}

static void xiaozhi_chat_append_locked(char role, const char *text)
{
    char clean[512];
    char entry[560];
    const char *last;
    size_t old_len;
    size_t entry_len;

    if(!text || !text[0]) {
        return;
    }

    snprintf(clean, sizeof(clean), "%s", text);
    ui_trim_text(clean);
    if(!clean[0]) {
        return;
    }

    snprintf(entry, sizeof(entry), "%c|%s", role == 'U' ? 'U' : 'A', clean);
    last = strrchr(xiaozhi_chat_text, '\n');
    last = last ? last + 1 : xiaozhi_chat_text;
    if(last[0] && strcmp(last, entry) == 0) {
        return;
    }

    old_len = strlen(xiaozhi_chat_text);
    entry_len = strlen(entry);
    while(old_len + entry_len + 2U >= sizeof(xiaozhi_chat_text) &&
          xiaozhi_chat_text[0]) {
        char *next = strchr(xiaozhi_chat_text, '\n');

        if(!next) {
            xiaozhi_chat_text[0] = '\0';
            old_len = 0;
            break;
        }
        memmove(xiaozhi_chat_text, next + 1, strlen(next + 1) + 1U);
        old_len = strlen(xiaozhi_chat_text);
    }

    snprintf(xiaozhi_chat_text + old_len,
             sizeof(xiaozhi_chat_text) - old_len, "%s%s",
             old_len ? "\n" : "", entry);
}

static void xiaozhi_chat_sync_from_log_locked(void)
{
    size_t log_len = strlen(xiaozhi_log_text);
    const char *scan;

    if(log_len < xiaozhi_log_scan_len) {
        xiaozhi_log_scan_len = 0;
    }

    scan = xiaozhi_log_text + xiaozhi_log_scan_len;
    while(*scan) {
        char line[640];
        char role = 0;
        const char *payload;
        size_t line_len = strcspn(scan, "\n");
        size_t copy_len = line_len < sizeof(line) - 1U ?
                          line_len : sizeof(line) - 1U;

        memcpy(line, scan, copy_len);
        line[copy_len] = '\0';
        payload = xiaozhi_chat_payload_from_log_line(line, &role);
        if(payload && payload[0]) {
            xiaozhi_chat_append_locked(role, payload);
        }

        scan += line_len;
        if(*scan == '\n') {
            scan++;
        }
    }

    xiaozhi_log_scan_len = log_len;
}

static void xiaozhi_last_log_line(const char *log_text, char *out,
                                  size_t out_len)
{
    const char *line;

    if(!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if(!log_text || !log_text[0]) {
        snprintf(out, out_len, "%s", ui_tr("No log yet"));
        return;
    }

    line = strrchr(log_text, '\n');
    line = line ? line + 1 : log_text;
    snprintf(out, out_len, "%s", line[0] ? line : ui_tr("No log yet"));
}

static void xiaozhi_chat_add_empty(void)
{
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *text;
    lv_obj_t *hint;
    int page_w;

    if(!xiaozhi_chat_scroll || !lv_obj_is_valid(xiaozhi_chat_scroll)) {
        return;
    }

    lv_obj_update_layout(xiaozhi_chat_scroll);
    page_w = lv_obj_get_width(xiaozhi_chat_scroll);
    if(page_w < 240) {
        page_w = ui_page_panel_width();
    }

    box = lv_obj_create(xiaozhi_chat_scroll);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_top(box, ui_is_landscape() ? 16 : 42, 0);
    lv_obj_set_style_pad_bottom(box, 18, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    icon = ui_label(box, LV_SYMBOL_AUDIO, &lv_font_montserrat_32, 0x64748B);
    lv_obj_set_width(icon, page_w - 48);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    text = ui_label(box, "No voice messages yet", &lv_font_montserrat_18,
                    0x94A3B8);
    lv_obj_set_width(text, page_w - 48);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    hint = ui_label(box, "Hold PTT to start a conversation.",
                    &lv_font_montserrat_14, 0x64748B);
    lv_obj_set_width(hint, page_w - 48);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

static void xiaozhi_chat_add_bubble(char role, const char *body)
{
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *text;
    lv_obj_t *footer;
    int sent = role == 'U';
    int page_w;
    int bubble_w;

    if(!xiaozhi_chat_scroll || !lv_obj_is_valid(xiaozhi_chat_scroll) ||
       !body || !body[0]) {
        return;
    }

    lv_obj_update_layout(xiaozhi_chat_scroll);
    page_w = lv_obj_get_width(xiaozhi_chat_scroll);
    if(page_w < 260) {
        page_w = ui_page_panel_width();
    }
    bubble_w = (page_w * 74) / 100;
    if(bubble_w < 240) {
        bubble_w = page_w > 280 ? 240 : page_w - 28;
    }
    if(bubble_w > 640) {
        bubble_w = 640;
    }

    row = lv_obj_create(xiaozhi_chat_scroll);
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

    text = ui_label(bubble, body, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_set_width(text, bubble_w - 20);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    footer = ui_label(bubble, sent ? "You" : "Assistant",
                      &lv_font_montserrat_14,
                      sent ? 0xDCFCE7 : 0x94A3B8);
    lv_obj_set_width(footer, bubble_w - 20);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);
}

static void xiaozhi_chat_rebuild(const char *shown)
{
    char copy[XIAOZHI_CHAT_TEXT_MAX];
    char *line;
    char *save = NULL;
    int count = 0;

    if(!xiaozhi_chat_scroll || !lv_obj_is_valid(xiaozhi_chat_scroll)) {
        return;
    }

    lv_obj_clean(xiaozhi_chat_scroll);
    if(!shown || !shown[0]) {
        xiaozhi_chat_add_empty();
        return;
    }

    snprintf(copy, sizeof(copy), "%s", shown);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        char role = line[0];
        char *body = strchr(line, '|');

        if(body && body[1]) {
            body++;
            xiaozhi_chat_add_bubble(role, body);
            count++;
        }
        line = strtok_r(NULL, "\n", &save);
    }

    if(count == 0) {
        xiaozhi_chat_add_empty();
    } else {
        lv_obj_t *last = lv_obj_get_child(xiaozhi_chat_scroll, count - 1);

        if(last) {
            lv_obj_scroll_to_view(last, LV_ANIM_ON);
        }
    }
}

static void xiaozhi_record_overlay_close(void)
{
    if(xiaozhi_record_overlay_timer) {
        lv_timer_delete(xiaozhi_record_overlay_timer);
        xiaozhi_record_overlay_timer = NULL;
    }
    if(xiaozhi_record_overlay && lv_obj_is_valid(xiaozhi_record_overlay)) {
        lv_obj_delete(xiaozhi_record_overlay);
    }
    xiaozhi_record_overlay = NULL;
    xiaozhi_record_time_label = NULL;
    xiaozhi_record_level_label = NULL;
    xiaozhi_record_phase = 0;
}

static void xiaozhi_record_overlay_timer_cb(lv_timer_t *timer)
{
    const char *levels[] = { "|    ", "|||  ", "|||||", " ||| ", "  |  " };
    uint64_t elapsed_ms;
    char text[96];

    (void)timer;
    if(!xiaozhi_record_overlay ||
       !lv_obj_is_valid(xiaozhi_record_overlay)) {
        xiaozhi_record_overlay_close();
        return;
    }

    elapsed_ms = (ui_monotonic_us() - xiaozhi_record_start_us) / 1000ULL;
    if(xiaozhi_record_time_label &&
       lv_obj_is_valid(xiaozhi_record_time_label)) {
        snprintf(text, sizeof(text), "%s  %llu.%01llus",
                 ui_tr("Recording"),
                 (unsigned long long)(elapsed_ms / 1000ULL),
                 (unsigned long long)((elapsed_ms / 100ULL) % 10ULL));
        lv_label_set_text(xiaozhi_record_time_label, text);
    }
    if(xiaozhi_record_level_label &&
       lv_obj_is_valid(xiaozhi_record_level_label)) {
        lv_label_set_text(xiaozhi_record_level_label,
                          levels[xiaozhi_record_phase %
                                 (sizeof(levels) / sizeof(levels[0]))]);
    }
    xiaozhi_record_phase++;
    app_request_fast_refresh();
}

static void xiaozhi_record_overlay_open(const char *source)
{
    lv_obj_t *card;
    lv_obj_t *spinner;
    lv_obj_t *mic;
    lv_obj_t *hint;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int card_w = ui_is_landscape() ? 312 : 286;
    int card_h = ui_is_landscape() ? 224 : 252;
    int spinner_size = ui_is_landscape() ? 102 : 112;

    (void)source;
    xiaozhi_record_overlay_close();
    if(card_w > screen_w - 40) {
        card_w = screen_w - 40;
    }
    if(card_h > screen_h - 40) {
        card_h = screen_h - 40;
    }

    xiaozhi_record_start_us = ui_monotonic_us();
    xiaozhi_record_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(xiaozhi_record_overlay);
    lv_obj_set_style_bg_color(xiaozhi_record_overlay, lv_color_hex(0x000000),
                              0);
    lv_obj_set_style_bg_opa(xiaozhi_record_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(xiaozhi_record_overlay, 0, 0);
    lv_obj_set_style_pad_all(xiaozhi_record_overlay, 0, 0);
    lv_obj_add_flag(xiaozhi_record_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(xiaozhi_record_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = ui_panel(xiaozhi_record_overlay, 0, 0, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F3B2E), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    spinner = lv_spinner_create(card);
    lv_obj_set_size(spinner, spinner_size, spinner_size);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x1F3B2E),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x25C281),
                               LV_PART_INDICATOR);

    mic = ui_label(card, LV_SYMBOL_AUDIO, &lv_font_montserrat_32, 0x25C281);
    lv_obj_set_width(mic, spinner_size);
    lv_obj_set_style_text_align(mic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mic, LV_ALIGN_TOP_MID, 0, 20 + (spinner_size - 34) / 2);

    xiaozhi_record_time_label =
        ui_label(card, ui_tr("Recording"), &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_width(xiaozhi_record_time_label, card_w - 36);
    lv_obj_set_style_text_align(xiaozhi_record_time_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(xiaozhi_record_time_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_record_time_label, LV_ALIGN_TOP_MID, 0,
                 34 + spinner_size);

    xiaozhi_record_level_label =
        ui_label(card, "|||||", &lv_font_montserrat_24, 0x25C281);
    lv_obj_set_width(xiaozhi_record_level_label, card_w - 36);
    lv_obj_set_style_text_align(xiaozhi_record_level_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(xiaozhi_record_level_label, LV_ALIGN_TOP_MID, 0,
                 64 + spinner_size);

    hint = ui_label(card, ui_tr("Release to send"), &lv_font_montserrat_14,
                    0x94A3B8);
    lv_obj_set_width(hint, card_w - 36);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    xiaozhi_record_overlay_timer =
        lv_timer_create(xiaozhi_record_overlay_timer_cb, 120, NULL);
    xiaozhi_record_overlay_timer_cb(xiaozhi_record_overlay_timer);
    app_request_fast_refresh();
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

    xiaozhi_apply_audio_output_route();
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
    xiaozhi_stop_session();
    xiaozhi_start_session();
}

static void xiaozhi_audio_event_cb(lv_event_t *event)
{
    (void)event;
    xiaozhi_start_action(XIAOZHI_ACTION_AUDIO);
}

static void xiaozhi_start_ptt_hold(void)
{
    char header[128];
    int session_ready;
    pid_t session_pid;

    pthread_mutex_lock(&xiaozhi_lock);
    if(xiaozhi_running) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    session_pid = xiaozhi_session_pid;
    session_ready = xiaozhi_session_ready;
    pthread_mutex_unlock(&xiaozhi_lock);

    if(!xiaozhi_process_alive(session_pid)) {
        xiaozhi_start_session();
    }
    if(!xiaozhi_process_alive(session_pid) || !session_ready) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Session not ready",
                                  "Wait for session ready", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    xiaozhi_truncate_log();
    if(xiaozhi_write_control("PTT_BEGIN") != 0) {
        pthread_mutex_lock(&xiaozhi_lock);
        xiaozhi_set_status_locked("Session not ready",
                                  "Tap Reconnect and try again", -1);
        pthread_mutex_unlock(&xiaozhi_lock);
        app_request_fast_refresh();
        return;
    }

    pthread_mutex_lock(&xiaozhi_lock);
    xiaozhi_running = 1;
    xiaozhi_ptt_recording = 1;
    xiaozhi_log_text[0] = '\0';
    xiaozhi_log_scan_len = 0;
    xiaozhi_set_status_locked("Recording", "Release to send", 0);
    snprintf(header, sizeof(header), "== Hold PTT session ==");
    xiaozhi_append_log_locked(header);
    pthread_mutex_unlock(&xiaozhi_lock);
    xiaozhi_record_overlay_open("PTT");
    app_request_fast_refresh();
}

static void xiaozhi_stop_ptt_hold(void)
{
    pthread_mutex_lock(&xiaozhi_lock);
    if(!xiaozhi_ptt_recording) {
        pthread_mutex_unlock(&xiaozhi_lock);
        return;
    }
    xiaozhi_ptt_recording = 0;
    xiaozhi_set_status_locked("Sending", "Waiting for reply", 0);
    pthread_mutex_unlock(&xiaozhi_lock);

    xiaozhi_write_control("PTT_END");
    xiaozhi_record_overlay_close();
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

void ui_xiaozhi_handle_voice_key(int pressed)
{
    if(!app_current_page_is(PAGE_XIAOZHI)) {
        return;
    }

    if(pressed) {
        xiaozhi_start_ptt_hold();
    } else {
        xiaozhi_stop_ptt_hold();
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
        xiaozhi_restart_session();
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
        xiaozhi_restart_session();
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
        xiaozhi_restart_session();
    } else {
        xiaozhi_save_status("Save failed", "Token is too long");
    }
}

static void xiaozhi_update(void)
{
    int running;
    int rc;
    int recording;
    int session_ready;
    pid_t ptt_pid;
    pid_t session_pid;
    char status[192];
    char detail[256];
    char log_text[XIAOZHI_LOG_TEXT_MAX];
    char log_preview[512];
    char chat_text[XIAOZHI_CHAT_TEXT_MAX];

    pthread_mutex_lock(&xiaozhi_lock);
    session_pid = xiaozhi_session_pid;
    if(session_pid > 0) {
        xiaozhi_reload_log_tail_locked();
        if(!xiaozhi_session_ready &&
           strstr(xiaozhi_log_text, "session ready")) {
            xiaozhi_session_ready = 1;
            if(!xiaozhi_running) {
                xiaozhi_set_status_locked("Ready", "Session ready", 0);
            }
        }
        if(xiaozhi_running && !xiaozhi_ptt_recording &&
           strstr(xiaozhi_log_text, "ptt turn done rc=0")) {
            xiaozhi_running = 0;
            xiaozhi_set_status_locked("Completed", "PTT", 0);
            xiaozhi_record_overlay_close();
        } else if(xiaozhi_running && !xiaozhi_ptt_recording &&
                  strstr(xiaozhi_log_text, "ptt turn done rc=")) {
            xiaozhi_running = 0;
            xiaozhi_set_status_locked("Failed, check log", "PTT", -1);
            xiaozhi_record_overlay_close();
        }
        if(kill(session_pid, 0) != 0 && errno == ESRCH) {
            xiaozhi_session_pid = -1;
            xiaozhi_session_ready = 0;
            xiaozhi_ptt_recording = 0;
            xiaozhi_running = 0;
            xiaozhi_set_status_locked("Session stopped", "Tap Reconnect",
                                      -1);
            xiaozhi_record_overlay_close();
        }
    }
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
            xiaozhi_record_overlay_close();
        }
    }
    running = xiaozhi_running;
    rc = xiaozhi_last_rc;
    recording = xiaozhi_ptt_recording;
    session_ready = xiaozhi_session_ready;
    ptt_pid = xiaozhi_ptt_pid;
    session_pid = xiaozhi_session_pid;
    xiaozhi_chat_sync_from_log_locked();
    snprintf(status, sizeof(status), "%s", xiaozhi_status_text);
    snprintf(detail, sizeof(detail), "%s", xiaozhi_detail_text);
    snprintf(log_text, sizeof(log_text), "%s", xiaozhi_log_text);
    snprintf(chat_text, sizeof(chat_text), "%s", xiaozhi_chat_text);
    pthread_mutex_unlock(&xiaozhi_lock);

    xiaozhi_last_log_line(log_text, log_preview, sizeof(log_preview));

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
        lv_label_set_text(xiaozhi_log_label, log_preview);
    }
    if(xiaozhi_chat_scroll &&
       strcmp(chat_text, xiaozhi_rendered_chat_text) != 0) {
        snprintf(xiaozhi_rendered_chat_text,
                 sizeof(xiaozhi_rendered_chat_text), "%s", chat_text);
        xiaozhi_chat_rebuild(chat_text);
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
        if((running && ptt_pid <= 0 && !recording) ||
           !session_ready ||
           !xiaozhi_process_alive(session_pid)) {
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
    lv_obj_t *title;
    lv_obj_t *btn;
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int body_y = ui_page_top_y(144);
    int body_h = ui_body_height(body_y);
    int landscape = ui_is_landscape();
    int content_w = w - 32;
    int top_pad = landscape ? 8 : 14;
    int gap = landscape ? 8 : 10;
    int status_h = landscape ? 144 : 276;
    int action_h = landscape ? 82 : 152;
    int chat_y = top_pad + status_h + gap;
    int chat_h = body_h - top_pad - status_h - action_h - gap * 2 - 14;
    int action_y;
    int cfg_y = landscape ? 0 : 202;
    int cfg_w = landscape ? 112 : (content_w - 20) / 3;
    int title_w = landscape ? content_w - 386 : content_w;

    if(chat_h < 160) {
        chat_h = 160;
    }
    action_y = chat_y + chat_h + gap;

    ui_create_header(scr, "Xiaozhi");

    body = ui_page_body(scr, 144);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    xiaozhi_status_panel = ui_panel(body, x, top_pad, w, status_h);
    lv_obj_set_style_bg_color(xiaozhi_status_panel, lv_color_hex(0x0F172A),
                              0);
    lv_obj_set_style_border_color(xiaozhi_status_panel,
                                  lv_color_hex(0x1F2937), 0);

    title = ui_label(xiaozhi_status_panel, "Xiaozhi voice assistant",
                     &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_width(title, title_w > 120 ? title_w : content_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    xiaozhi_status_label = ui_label(xiaozhi_status_panel, "Ready",
                                    &lv_font_montserrat_22,
                                    0x25C281);
    lv_obj_set_width(xiaozhi_status_label,
                     title_w > 120 ? title_w : content_w);
    lv_label_set_long_mode(xiaozhi_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_status_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 34 : 42);

    xiaozhi_detail_label = ui_label(xiaozhi_status_panel,
                                    "Probe cloud handshake, test audio loopback, or send one short PTT turn.",
                                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(xiaozhi_detail_label,
                     title_w > 120 ? title_w : content_w);
    lv_label_set_long_mode(xiaozhi_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(xiaozhi_detail_label, LV_ALIGN_TOP_LEFT, 0,
                 landscape ? 66 : 80);

    xiaozhi_url_label = ui_label(xiaozhi_status_panel, "",
                                 &lv_font_montserrat_14,
                                 0x9AA4AF);
    lv_obj_set_width(xiaozhi_url_label, landscape ? 360 : content_w);
    lv_label_set_long_mode(xiaozhi_url_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_url_label,
                 landscape ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                 0, landscape ? 70 : 108);

    xiaozhi_token_label = ui_label(xiaozhi_status_panel, "",
                                   &lv_font_montserrat_14,
                                   0x9AA4AF);
    lv_obj_set_width(xiaozhi_token_label, landscape ? 360 : content_w);
    lv_label_set_long_mode(xiaozhi_token_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_token_label,
                 landscape ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                 0, landscape ? 92 : 132);

    if(landscape) {
        int cfg_x = content_w - cfg_w * 3 - 20;

        btn = ui_command_button(xiaozhi_status_panel, cfg_x, cfg_y, cfg_w,
                                "Server",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, xiaozhi_open_url_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(xiaozhi_status_panel, cfg_x + cfg_w + 10,
                                cfg_y, cfg_w, "Token",
                                0xA78BFA);
        lv_obj_add_event_cb(btn, xiaozhi_open_token_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(xiaozhi_status_panel, cfg_x + (cfg_w + 10) * 2,
                                cfg_y, cfg_w,
                                "Clear token", 0xF97316);
        lv_obj_add_event_cb(btn, xiaozhi_clear_token_event_cb,
                            LV_EVENT_CLICKED, NULL);
    } else {
        btn = ui_command_button(xiaozhi_status_panel, 0, cfg_y, cfg_w,
                                "Server",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, xiaozhi_open_url_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(xiaozhi_status_panel, cfg_w + 10, cfg_y,
                                cfg_w, "Token",
                                0xA78BFA);
        lv_obj_add_event_cb(btn, xiaozhi_open_token_event_cb, LV_EVENT_CLICKED,
                            NULL);
        btn = ui_command_button(xiaozhi_status_panel, (cfg_w + 10) * 2, cfg_y,
                                cfg_w,
                                "Clear token", 0xF97316);
        lv_obj_add_event_cb(btn, xiaozhi_clear_token_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    xiaozhi_log_label = ui_label(xiaozhi_status_panel, "No log yet",
                                 &lv_font_montserrat_12, 0x94A3B8);
    lv_obj_set_width(xiaozhi_log_label,
                     landscape ? (title_w > 120 ? title_w : content_w) :
                                 content_w);
    lv_label_set_long_mode(xiaozhi_log_label, LV_LABEL_LONG_DOT);
    lv_obj_align(xiaozhi_log_label, LV_ALIGN_TOP_LEFT, 0, status_h - 28);

    xiaozhi_chat_scroll = ui_panel(body, x, chat_y, w, chat_h);
    lv_obj_set_style_bg_color(xiaozhi_chat_scroll, lv_color_hex(0x101820),
                              0);
    lv_obj_set_style_border_color(xiaozhi_chat_scroll,
                                  lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_pad_all(xiaozhi_chat_scroll, 12, 0);
    lv_obj_set_style_pad_row(xiaozhi_chat_scroll, 2, 0);
    ui_make_scrollable(xiaozhi_chat_scroll, 18);
    lv_obj_set_flex_flow(xiaozhi_chat_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(xiaozhi_chat_scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    xiaozhi_action_panel = ui_panel(body, x, action_y, w, action_h);
    lv_obj_set_style_bg_color(xiaozhi_action_panel, lv_color_hex(0x0F172A),
                              0);
    lv_obj_set_style_border_color(xiaozhi_action_panel,
                                  lv_color_hex(0x1F2937), 0);

    if(landscape) {
        int button_w = (content_w - 24) / 3;

        xiaozhi_probe_btn = ui_command_button(xiaozhi_action_panel, 0, 10,
                                              button_w, "Reconnect",
                                              0x3DA5FF);
        xiaozhi_audio_btn = ui_command_button(xiaozhi_action_panel,
                                              button_w + 12, 10, button_w,
                                              "Audio", 0x22C55E);
        xiaozhi_ptt_btn = ui_command_button(xiaozhi_action_panel,
                                            (button_w + 12) * 2, 10, button_w,
                                            "Hold to talk", 0xF97316);
    } else {
        int half_w = (content_w - 12) / 2;

        xiaozhi_probe_btn = ui_command_button(xiaozhi_action_panel, 0, 8,
                                              half_w, "Reconnect", 0x3DA5FF);
        xiaozhi_audio_btn = ui_command_button(xiaozhi_action_panel,
                                              half_w + 12, 8, half_w,
                                              "Audio", 0x22C55E);
        xiaozhi_ptt_btn = ui_command_button(xiaozhi_action_panel, 0, 82,
                                            content_w, "Hold to talk",
                                            0xF97316);
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

    xiaozhi_rendered_chat_text[0] = '\0';
    xiaozhi_chat_rebuild(xiaozhi_chat_text);
    snprintf(xiaozhi_rendered_chat_text, sizeof(xiaozhi_rendered_chat_text),
             "%s", xiaozhi_chat_text);
    xiaozhi_start_session();
    xiaozhi_timer = lv_timer_create(xiaozhi_timer_cb, 500, NULL);
    xiaozhi_update_settings_labels();
    xiaozhi_update();
}

void ui_xiaozhi_cleanup(void)
{
    xiaozhi_stop_session();
    xiaozhi_record_overlay_close();

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
    xiaozhi_status_panel = NULL;
    xiaozhi_chat_scroll = NULL;
    xiaozhi_action_panel = NULL;
    xiaozhi_status_label = NULL;
    xiaozhi_detail_label = NULL;
    xiaozhi_log_label = NULL;
    xiaozhi_probe_btn = NULL;
    xiaozhi_audio_btn = NULL;
    xiaozhi_ptt_btn = NULL;
    xiaozhi_url_label = NULL;
    xiaozhi_token_label = NULL;
    xiaozhi_rendered_chat_text[0] = '\0';
}
