#include "ui_wifi_iperf.h"

#include "ui_i18n.h"
#include "ui_input.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIFI_IPERF_LOG "/tmp/k230_wifi_iperf.log"
#define WIFI_IPERF_HOST_MAX 96
#define WIFI_IPERF_STATUS_MAX 192
#define WIFI_IPERF_RESULT_MAX 1536
#define WIFI_IPERF_DEFAULT_HOST "192.168.36.1"
#define WIFI_IPERF_DEFAULT_PORT 5201
#define WIFI_IPERF_DEFAULT_DURATION 10

typedef enum {
    WIFI_IPERF_MODE_UPLOAD = 0,
    WIFI_IPERF_MODE_REVERSE,
    WIFI_IPERF_MODE_SERVER,
} wifi_iperf_mode_t;

typedef struct {
    wifi_iperf_mode_t mode;
    char host[WIFI_IPERF_HOST_MAX];
    int port;
    int duration_s;
} wifi_iperf_request_t;

static pthread_mutex_t wifi_iperf_lock = PTHREAD_MUTEX_INITIALIZER;
static int wifi_iperf_busy;
static char wifi_iperf_host[WIFI_IPERF_HOST_MAX] = WIFI_IPERF_DEFAULT_HOST;
static int wifi_iperf_port = WIFI_IPERF_DEFAULT_PORT;
static int wifi_iperf_duration_s = WIFI_IPERF_DEFAULT_DURATION;
static char wifi_iperf_status[WIFI_IPERF_STATUS_MAX] = "Ready";
static char wifi_iperf_result[WIFI_IPERF_RESULT_MAX] = "No test yet";

static lv_obj_t *wifi_iperf_status_label;
static lv_obj_t *wifi_iperf_target_label;
static lv_obj_t *wifi_iperf_ip_label;
static lv_obj_t *wifi_iperf_result_label;
static lv_timer_t *wifi_iperf_timer;

static void wifi_iperf_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&wifi_iperf_lock);
    va_start(ap, fmt);
    vsnprintf(wifi_iperf_status, sizeof(wifi_iperf_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&wifi_iperf_lock);
}

static void wifi_iperf_set_result(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&wifi_iperf_lock);
    va_start(ap, fmt);
    vsnprintf(wifi_iperf_result, sizeof(wifi_iperf_result), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&wifi_iperf_lock);
}

static void wifi_iperf_log(const char *fmt, ...)
{
    FILE *fp = fopen(WIFI_IPERF_LOG, "a");
    va_list ap;

    if(!fp) {
        return;
    }
    fprintf(fp, "[%llu] ", (unsigned long long)ui_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static int wifi_iperf_command_exists(void)
{
    return ui_shell_exit_code(system("command -v iperf3 >/dev/null 2>&1")) == 0;
}

static void wifi_iperf_shell_quote(char *dst, size_t len, const char *src)
{
    size_t pos = 0;

    if(!dst || len == 0U) {
        return;
    }
    if(pos + 1U < len) {
        dst[pos++] = '\'';
    }
    if(src) {
        for(size_t i = 0; src[i] && pos + 5U < len; i++) {
            if(src[i] == '\'') {
                memcpy(dst + pos, "'\\''", 4U);
                pos += 4U;
            } else {
                dst[pos++] = src[i];
            }
        }
    }
    if(pos + 1U < len) {
        dst[pos++] = '\'';
    }
    dst[pos < len ? pos : len - 1U] = '\0';
}

static int wifi_iperf_capture_command(const char *cmd, char *out, size_t out_len)
{
    FILE *pipe;
    char line[256];
    size_t used = 0;
    int rc;

    if(out && out_len) {
        out[0] = '\0';
    }
    pipe = popen(cmd, "r");
    if(!pipe) {
        if(out && out_len) {
            snprintf(out, out_len, "popen failed: %s", strerror(errno));
        }
        return -1;
    }

    while(fgets(line, sizeof(line), pipe)) {
        if(out && used + 1U < out_len) {
            size_t line_len = strlen(line);
            size_t copy_len = line_len;
            if(copy_len > out_len - used - 1U) {
                copy_len = out_len - used - 1U;
            }
            memcpy(out + used, line, copy_len);
            used += copy_len;
            out[used] = '\0';
        }
    }

    rc = pclose(pipe);
    if(WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    return -1;
}

static double wifi_iperf_parse_last_bps(const char *text)
{
    const char *p = text;
    double last = 0.0;

    while(p && (p = strstr(p, "\"bits_per_second\"")) != NULL) {
        char *endp = NULL;
        double value;

        p = strchr(p, ':');
        if(!p) {
            break;
        }
        p++;
        value = strtod(p, &endp);
        if(endp != p && value > 0.0) {
            last = value;
        }
        p = endp;
    }
    return last;
}

static void wifi_iperf_first_log_lines(const char *text, char *out, size_t out_len)
{
    const char *p = text;
    size_t used = 0;
    int lines = 0;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    while(p && *p && lines < 10 && used + 1U < out_len) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if(n > out_len - used - 2U) {
            n = out_len - used - 2U;
        }
        memcpy(out + used, p, n);
        used += n;
        out[used++] = '\n';
        out[used] = '\0';
        lines++;
        if(!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void *wifi_iperf_thread_cb(void *arg)
{
    wifi_iperf_request_t *req = (wifi_iperf_request_t *)arg;
    char host_q[WIFI_IPERF_HOST_MAX * 6];
    char cmd[384];
    char output[WIFI_IPERF_RESULT_MAX];
    char excerpt[640];
    double bps;
    int rc;

    if(!req) {
        return NULL;
    }

    if(!wifi_iperf_command_exists()) {
        wifi_iperf_set_status("iperf3 missing");
        wifi_iperf_set_result("Install or include iperf3 in the image before running this test.");
        pthread_mutex_lock(&wifi_iperf_lock);
        wifi_iperf_busy = 0;
        pthread_mutex_unlock(&wifi_iperf_lock);
        free(req);
        return NULL;
    }

    wifi_iperf_shell_quote(host_q, sizeof(host_q), req->host);
    if(req->mode == WIFI_IPERF_MODE_SERVER) {
        snprintf(cmd, sizeof(cmd), "iperf3 -s -1 -p %d 2>&1", req->port);
        wifi_iperf_set_status("Server waiting on port %d", req->port);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "iperf3 -c %s -p %d -t %d %s -J 2>&1",
                 host_q, req->port, req->duration_s,
                 req->mode == WIFI_IPERF_MODE_REVERSE ? "-R" : "");
        wifi_iperf_set_status("%s test running",
                              req->mode == WIFI_IPERF_MODE_REVERSE ?
                              "Download" : "Upload");
    }

    wifi_iperf_log("run: %s", cmd);
    rc = wifi_iperf_capture_command(cmd, output, sizeof(output));
    bps = wifi_iperf_parse_last_bps(output);
    wifi_iperf_first_log_lines(output, excerpt, sizeof(excerpt));
    wifi_iperf_log("rc=%d bps=%.2f output=%s", rc, bps, output);

    if(req->mode == WIFI_IPERF_MODE_SERVER) {
        wifi_iperf_set_status(rc == 0 ? "Server test finished" :
                              "Server failed rc=%d", rc);
    } else if(rc == 0 && bps > 0.0) {
        wifi_iperf_set_status("%s %.2f Mbps",
                              req->mode == WIFI_IPERF_MODE_REVERSE ?
                              "Download" : "Upload",
                              bps / 1000000.0);
    } else {
        wifi_iperf_set_status("iperf failed rc=%d", rc);
    }

    if(bps > 0.0) {
        wifi_iperf_set_result("%s\nRate: %.2f Mbps\n\n%s",
                              req->mode == WIFI_IPERF_MODE_SERVER ?
                              "Server mode" :
                              (req->mode == WIFI_IPERF_MODE_REVERSE ?
                               "Reverse download" : "Upload"),
                              bps / 1000000.0, excerpt);
    } else {
        wifi_iperf_set_result("%s", excerpt[0] ? excerpt : "No iperf output");
    }

    pthread_mutex_lock(&wifi_iperf_lock);
    wifi_iperf_busy = 0;
    pthread_mutex_unlock(&wifi_iperf_lock);
    free(req);
    return NULL;
}

static void wifi_iperf_start(wifi_iperf_mode_t mode)
{
    wifi_iperf_request_t *req;
    pthread_t thread;

    pthread_mutex_lock(&wifi_iperf_lock);
    if(wifi_iperf_busy) {
        pthread_mutex_unlock(&wifi_iperf_lock);
        wifi_iperf_set_status("iperf busy");
        return;
    }
    wifi_iperf_busy = 1;
    pthread_mutex_unlock(&wifi_iperf_lock);

    req = (wifi_iperf_request_t *)calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&wifi_iperf_lock);
        wifi_iperf_busy = 0;
        pthread_mutex_unlock(&wifi_iperf_lock);
        wifi_iperf_set_status("No memory");
        return;
    }

    req->mode = mode;
    pthread_mutex_lock(&wifi_iperf_lock);
    snprintf(req->host, sizeof(req->host), "%s", wifi_iperf_host);
    req->port = wifi_iperf_port;
    req->duration_s = wifi_iperf_duration_s;
    pthread_mutex_unlock(&wifi_iperf_lock);

    if(pthread_create(&thread, NULL, wifi_iperf_thread_cb, req) != 0) {
        free(req);
        pthread_mutex_lock(&wifi_iperf_lock);
        wifi_iperf_busy = 0;
        pthread_mutex_unlock(&wifi_iperf_lock);
        wifi_iperf_set_status("Thread start failed");
        return;
    }
    pthread_detach(thread);
}

static void wifi_iperf_stop_event_cb(lv_event_t *event)
{
    int rc;

    (void)event;
    rc = system("killall iperf3 >/dev/null 2>&1");
    (void)rc;
    wifi_iperf_set_status("Stop requested");
}

static void wifi_iperf_upload_event_cb(lv_event_t *event)
{
    (void)event;
    wifi_iperf_start(WIFI_IPERF_MODE_UPLOAD);
}

static void wifi_iperf_download_event_cb(lv_event_t *event)
{
    (void)event;
    wifi_iperf_start(WIFI_IPERF_MODE_REVERSE);
}

static void wifi_iperf_server_event_cb(lv_event_t *event)
{
    (void)event;
    wifi_iperf_start(WIFI_IPERF_MODE_SERVER);
}

static void wifi_iperf_host_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    if(!text || !text[0]) {
        return;
    }

    pthread_mutex_lock(&wifi_iperf_lock);
    snprintf(wifi_iperf_host, sizeof(wifi_iperf_host), "%s", text);
    ui_trim_text(wifi_iperf_host);
    pthread_mutex_unlock(&wifi_iperf_lock);
    wifi_iperf_set_status("Target updated");
}

static void wifi_iperf_set_host_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;
    char host[WIFI_IPERF_HOST_MAX];

    (void)event;
    pthread_mutex_lock(&wifi_iperf_lock);
    snprintf(host, sizeof(host), "%s", wifi_iperf_host);
    pthread_mutex_unlock(&wifi_iperf_lock);

    memset(&config, 0, sizeof(config));
    config.title = "iperf server";
    config.placeholder = "Server IP or host";
    config.initial_text = host;
    config.max_length = WIFI_IPERF_HOST_MAX - 1U;
    config.min_length = 1U;
    config.min_length_text = "Enter server address";
    config.submit_cb = wifi_iperf_host_submit_cb;
    config.submit_text = "Save";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void wifi_iperf_timer_cb(lv_timer_t *timer)
{
    char status[WIFI_IPERF_STATUS_MAX];
    char result[WIFI_IPERF_RESULT_MAX];
    char host[WIFI_IPERF_HOST_MAX];
    char ip[64];
    int busy;

    (void)timer;
    pthread_mutex_lock(&wifi_iperf_lock);
    snprintf(status, sizeof(status), "%s", wifi_iperf_status);
    snprintf(result, sizeof(result), "%s", wifi_iperf_result);
    snprintf(host, sizeof(host), "%s", wifi_iperf_host);
    busy = wifi_iperf_busy;
    pthread_mutex_unlock(&wifi_iperf_lock);

    if(wifi_iperf_status_label && lv_obj_is_valid(wifi_iperf_status_label)) {
        lv_label_set_text(wifi_iperf_status_label, ui_tr(status));
    }
    if(wifi_iperf_target_label && lv_obj_is_valid(wifi_iperf_target_label)) {
        lv_label_set_text_fmt(wifi_iperf_target_label, "Target %s:%d  %ds",
                              host, wifi_iperf_port, wifi_iperf_duration_s);
    }
    if(wifi_iperf_ip_label && lv_obj_is_valid(wifi_iperf_ip_label)) {
        if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) != 0) {
            snprintf(ip, sizeof(ip), "No %s IP", NET_WIFI_IFACE);
        }
        lv_label_set_text_fmt(wifi_iperf_ip_label, "%s  %s%s",
                              NET_WIFI_IFACE, ip, busy ? "  running" : "");
    }
    if(wifi_iperf_result_label && lv_obj_is_valid(wifi_iperf_result_label)) {
        lv_label_set_text(wifi_iperf_result_label, result);
    }
}

void ui_wifi_iperf_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *result;
    lv_obj_t *btn;
    int top = ui_page_top_y(144);
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int landscape = ui_is_landscape();
    int panel_h = landscape ? 160 : 188;
    int button_y = panel_h + 18;
    int button_w = landscape ? (w - 24) / 4 : (w - 18) / 2;
    int result_y = landscape ? panel_h + 96 : panel_h + 168;
    int result_h = ui_screen_height() - top - result_y - 32;

    if(result_h < 220) {
        result_h = 220;
    }

    ui_create_header(scr, "WiFi iperf");
    body = ui_page_body(scr, top);

    panel = ui_panel(body, x, 0, w, panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    ui_label(panel, "WiFi throughput", &lv_font_montserrat_24, 0xF2F5F8);

    wifi_iperf_status_label =
        ui_label(panel, "Ready", &lv_font_montserrat_18, 0x25C281);
    lv_obj_align(wifi_iperf_status_label, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_set_width(wifi_iperf_status_label, w - 32);
    lv_label_set_long_mode(wifi_iperf_status_label, LV_LABEL_LONG_DOT);

    wifi_iperf_ip_label =
        ui_label(panel, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(wifi_iperf_ip_label, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_width(wifi_iperf_ip_label, w - 32);

    wifi_iperf_target_label =
        ui_label(panel, "--", &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_align(wifi_iperf_target_label, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_obj_set_width(wifi_iperf_target_label, w - 32);
    lv_label_set_long_mode(wifi_iperf_target_label, LV_LABEL_LONG_DOT);

    if(landscape) {
        btn = ui_command_button(body, x, button_y, button_w, "Target",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, wifi_iperf_set_host_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x + button_w + 8, button_y, button_w,
                                "Upload", 0x25C281);
        lv_obj_add_event_cb(btn, wifi_iperf_upload_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x + (button_w + 8) * 2, button_y,
                                button_w, "Download", 0x3DA5FF);
        lv_obj_add_event_cb(btn, wifi_iperf_download_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x + (button_w + 8) * 3, button_y,
                                button_w, "Server", 0xF5A524);
        lv_obj_add_event_cb(btn, wifi_iperf_server_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x, button_y + 68, button_w, "Stop",
                                0xEF4D5A);
        lv_obj_add_event_cb(btn, wifi_iperf_stop_event_cb,
                            LV_EVENT_CLICKED, NULL);
    } else {
        btn = ui_command_button(body, x, button_y, button_w, "Target",
                                0x60A5FA);
        lv_obj_add_event_cb(btn, wifi_iperf_set_host_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x + button_w + 18, button_y, button_w,
                                "Upload", 0x25C281);
        lv_obj_add_event_cb(btn, wifi_iperf_upload_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x, button_y + 76, button_w, "Download",
                                0x3DA5FF);
        lv_obj_add_event_cb(btn, wifi_iperf_download_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x + button_w + 18, button_y + 76,
                                button_w, "Server", 0xF5A524);
        lv_obj_add_event_cb(btn, wifi_iperf_server_event_cb,
                            LV_EVENT_CLICKED, NULL);
        btn = ui_command_button(body, x, button_y + 152, button_w, "Stop",
                                0xEF4D5A);
        lv_obj_add_event_cb(btn, wifi_iperf_stop_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    result = ui_panel(body, x, result_y, w, result_h);
    lv_obj_set_style_bg_color(result, lv_color_hex(0x0B1117), 0);
    wifi_iperf_result_label =
        ui_label(result, "No test yet", &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_set_width(wifi_iperf_result_label, w - 32);
    lv_label_set_long_mode(wifi_iperf_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(wifi_iperf_result_label, LV_ALIGN_TOP_LEFT, 0, 0);

    if(!wifi_iperf_timer) {
        wifi_iperf_timer = lv_timer_create(wifi_iperf_timer_cb, 500, NULL);
    }
    wifi_iperf_timer_cb(NULL);
}

void ui_wifi_iperf_cleanup(void)
{
    if(wifi_iperf_timer) {
        lv_timer_delete(wifi_iperf_timer);
        wifi_iperf_timer = NULL;
    }
    wifi_iperf_status_label = NULL;
    wifi_iperf_target_label = NULL;
    wifi_iperf_ip_label = NULL;
    wifi_iperf_result_label = NULL;
}
