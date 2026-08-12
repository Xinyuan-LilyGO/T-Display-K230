#include "ui_rtsp.h"

#include "ui_common.h"
#include "ui_i18n.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RTSP_DEMO_BIN "/root/app/camera_rtsp_demo"
#define RTSP_LOG_PATH "/tmp/k230_rtsp_stream.log"
#define RTSP_URL_PATH "/test"

typedef enum {
    RTSP_ACTION_START = 0,
    RTSP_ACTION_STOP,
} rtsp_action_t;

static pthread_mutex_t rtsp_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *rtsp_timer;
static lv_obj_t *rtsp_status_label;
static lv_obj_t *rtsp_url_label;
static lv_obj_t *rtsp_log_label;
static lv_obj_t *rtsp_start_btn;
static lv_obj_t *rtsp_stop_btn;
static int rtsp_busy;
static int rtsp_last_rc;
static char rtsp_status_text[192] = "Ready";

static void rtsp_set_status_locked(const char *text, int rc)
{
    snprintf(rtsp_status_text, sizeof(rtsp_status_text), "%s",
             text ? text : "");
    rtsp_last_rc = rc;
}

int ui_rtsp_is_active(void)
{
    char pid[64];

    return ui_read_cmd_first_line("pidof camera_rtsp_demo 2>/dev/null",
                                  pid, sizeof(pid)) == 0 && pid[0] != '\0';
}

static void rtsp_make_url(char *out, size_t len)
{
    char ip[64];
    char *slash;

    if(!out || len == 0) {
        return;
    }
    if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) != 0 &&
       ui_read_iface_ip(NET_ETH_IFACE, ip, sizeof(ip)) != 0) {
        snprintf(out, len, "rtsp://<board-ip>:8554%s", RTSP_URL_PATH);
        return;
    }
    slash = strchr(ip, '/');
    if(slash) {
        *slash = '\0';
    }
    snprintf(out, len, "rtsp://%s:8554%s", ip, RTSP_URL_PATH);
}

static void rtsp_read_log_tail(char *out, size_t len)
{
    FILE *fp;
    char line[192];
    char last[768] = "";

    if(!out || len == 0) {
        return;
    }
    out[0] = '\0';
    fp = fopen(RTSP_LOG_PATH, "r");
    if(!fp) {
        snprintf(out, len, "%s", "No log yet");
        return;
    }
    while(fgets(line, sizeof(line), fp)) {
        size_t last_len = strlen(last);
        size_t line_len;

        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }
        line_len = strlen(line);
        if(last_len + line_len + 2 >= sizeof(last)) {
            memmove(last, last + sizeof(last) / 3,
                    strlen(last + sizeof(last) / 3) + 1);
            last_len = strlen(last);
        }
        snprintf(last + last_len, sizeof(last) - last_len, "%s%s",
                 last_len ? "\n" : "", line);
    }
    fclose(fp);
    snprintf(out, len, "%s", last[0] ? last : "No log yet");
}

static void *rtsp_action_thread(void *arg)
{
    rtsp_action_t action = (rtsp_action_t)(intptr_t)arg;
    char cmd[512];
    int rc;

    if(action == RTSP_ACTION_START) {
        if(access(RTSP_DEMO_BIN, X_OK) != 0) {
            pthread_mutex_lock(&rtsp_lock);
            rtsp_busy = 0;
            rtsp_set_status_locked("Camera RTSP demo missing", 127);
            pthread_mutex_unlock(&rtsp_lock);
            app_request_fast_refresh();
            return NULL;
        }
        snprintf(cmd, sizeof(cmd),
                 "killall camera_rtsp_demo >/dev/null 2>&1 || true; "
                 "%s -t h264 -w 1280 -h 720 -b 2000 > %s 2>&1 &",
                 RTSP_DEMO_BIN, RTSP_LOG_PATH);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "killall camera_rtsp_demo > %s 2>&1 || true",
                 RTSP_LOG_PATH);
    }

    rc = ui_shell_exit_code(system(cmd));
    pthread_mutex_lock(&rtsp_lock);
    rtsp_busy = 0;
    if(rc == 0) {
        rtsp_set_status_locked(action == RTSP_ACTION_START ?
                               "Streaming" : "Stopped", rc);
    } else {
        rtsp_set_status_locked(action == RTSP_ACTION_START ?
                               "Start failed, check RTSP log" :
                               "Stop failed, check RTSP log", rc);
    }
    pthread_mutex_unlock(&rtsp_lock);
    app_request_fast_refresh();
    return NULL;
}

static void rtsp_start_action(rtsp_action_t action)
{
    pthread_t thread;

    pthread_mutex_lock(&rtsp_lock);
    if(rtsp_busy) {
        pthread_mutex_unlock(&rtsp_lock);
        return;
    }
    rtsp_busy = 1;
    rtsp_set_status_locked(action == RTSP_ACTION_START ?
                           "Starting RTSP..." : "Stopping RTSP...", 0);
    pthread_mutex_unlock(&rtsp_lock);

    if(pthread_create(&thread, NULL, rtsp_action_thread,
                      (void *)(intptr_t)action) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&rtsp_lock);
        rtsp_busy = 0;
        rtsp_set_status_locked("Thread failed", -1);
        pthread_mutex_unlock(&rtsp_lock);
    }
}

static void rtsp_start_event_cb(lv_event_t *event)
{
    (void)event;
    rtsp_start_action(RTSP_ACTION_START);
}

static void rtsp_stop_event_cb(lv_event_t *event)
{
    (void)event;
    rtsp_start_action(RTSP_ACTION_STOP);
}

static void rtsp_update(void)
{
    int active = ui_rtsp_is_active();
    int busy;
    int rc;
    char status[192];
    char url[128];
    char log_text[1024];

    pthread_mutex_lock(&rtsp_lock);
    busy = rtsp_busy;
    rc = rtsp_last_rc;
    snprintf(status, sizeof(status), "%s", rtsp_status_text);
    pthread_mutex_unlock(&rtsp_lock);

    rtsp_make_url(url, sizeof(url));
    rtsp_read_log_tail(log_text, sizeof(log_text));

    if(rtsp_status_label) {
        uint32_t color = active ? 0x25C281 : (rc == 0 ? 0x9AA4AF : 0xF5A524);
        lv_label_set_text(rtsp_status_label,
                          ui_tr(busy ? status : (active ? "Streaming" : status)));
        lv_obj_set_style_text_color(rtsp_status_label, lv_color_hex(color), 0);
    }
    if(rtsp_url_label) {
        lv_label_set_text(rtsp_url_label, url);
    }
    if(rtsp_log_label) {
        lv_label_set_text(rtsp_log_label, log_text);
    }
    if(rtsp_start_btn) {
        if(active || busy) {
            lv_obj_add_state(rtsp_start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(rtsp_start_btn, LV_STATE_DISABLED);
        }
    }
    if(rtsp_stop_btn) {
        if(!active || busy) {
            lv_obj_add_state(rtsp_stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(rtsp_stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void rtsp_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    rtsp_update();
}

void ui_rtsp_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *hint;
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int body_y = ui_page_top_y(144);
    int landscape = ui_is_landscape();
    int actions_y = landscape ? 174 : 244;
    int log_y = landscape ? 254 : 364;
    int button_w = landscape ? 180 : 232;

    ui_create_header(scr, "RTSP");

    body = ui_page_body(scr, 144);
    panel = ui_panel(body, x, 18, w, ui_body_height(body_y) - 42);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), 0);
    ui_make_scrollable(panel, 48);

    title = ui_label(panel, "Camera RTSP stream", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    rtsp_status_label = ui_label(panel, "Ready", &lv_font_montserrat_20,
                                 0x9AA4AF);
    lv_obj_set_width(rtsp_status_label, w - 32);
    lv_label_set_long_mode(rtsp_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(rtsp_status_label, LV_ALIGN_TOP_LEFT, 0, 46);

    hint = ui_label(panel, "Open this URL on a PC player.",
                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, w - 32);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 86);

    rtsp_url_label = ui_label(panel, "--", &lv_font_montserrat_20, 0x3DA5FF);
    lv_obj_set_width(rtsp_url_label, w - 32);
    lv_label_set_long_mode(rtsp_url_label, LV_LABEL_LONG_DOT);
    lv_obj_align(rtsp_url_label, LV_ALIGN_TOP_LEFT, 0, 124);

    rtsp_start_btn = ui_command_button(panel, 0, actions_y, button_w,
                                       "Start stream", 0x25C281);
    lv_obj_add_event_cb(rtsp_start_btn, rtsp_start_event_cb,
                        LV_EVENT_CLICKED, NULL);

    rtsp_stop_btn = ui_command_button(panel, button_w + 24, actions_y,
                                      button_w, "Stop stream", 0xEF4D5A);
    lv_obj_add_event_cb(rtsp_stop_btn, rtsp_stop_event_cb,
                        LV_EVENT_CLICKED, NULL);

    hint = ui_label(panel, "RTSP log", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, log_y);

    rtsp_log_label = ui_label(panel, "No log yet", &lv_font_montserrat_14,
                              0x9AA4AF);
    lv_obj_set_width(rtsp_log_label, w - 32);
    lv_label_set_long_mode(rtsp_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(rtsp_log_label, LV_ALIGN_TOP_LEFT, 0, log_y + 34);

    rtsp_timer = lv_timer_create(rtsp_timer_cb, 1000, NULL);
    rtsp_update();
}

void ui_rtsp_cleanup(void)
{
    if(rtsp_timer) {
        lv_timer_delete(rtsp_timer);
        rtsp_timer = NULL;
    }
    rtsp_status_label = NULL;
    rtsp_url_label = NULL;
    rtsp_log_label = NULL;
    rtsp_start_btn = NULL;
    rtsp_stop_btn = NULL;
}
