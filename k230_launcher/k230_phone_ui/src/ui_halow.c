#include "ui_halow.h"

#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#define HALOW_SCRIPT "/root/app/k230_phone_ui/k230_halow_camera_stream.sh"
#define HALOW_UDP_BIN "/root/app/k230_phone_ui/k230_halow_udp_stream"
#define HALOW_LOG "/tmp/k230_halow_stream.log"
#define HALOW_INNER_LOG "/tmp/k230_halow_udp_inner.log"
#define HALOW_PREVIEW_FILE "/tmp/k230_halow_preview.rgb565"
#define HALOW_PREVIEW_META "/tmp/k230_halow_preview.meta"
#define HALOW_STOP_FILE "/tmp/k230_halow_camera_stream.stop"
#define HALOW_PREF_LOCAL_IP "halow.local_ip"
#define HALOW_PREF_PEER_IP "halow.peer_ip"
#define HALOW_PREF_NETMASK "halow.netmask"
#define HALOW_PREF_PRESET "halow.preset"
#define HALOW_PREF_PRESET_SCHEMA "halow.preset.schema"
#define HALOW_PREF_FPS "halow.fps"
#define HALOW_PREF_QUALITY "halow.quality"
#define HALOW_PREF_FORMAT "halow.format"
#define HALOW_PREF_TRANSPORT "halow.transport"
#define HALOW_IP_MAX 48
#define HALOW_STATUS_MAX 192
#define HALOW_LOG_TEXT_MAX 1024
#define HALOW_DEFAULT_LOCAL_IP "192.168.100.210"
#define HALOW_DEFAULT_PEER_IP "192.168.100.237"
#define HALOW_DEFAULT_NETMASK "255.255.255.0"
#define HALOW_DEFAULT_PRESET "320x240"
#define HALOW_PRESET_SCHEMA "2"
#define HALOW_DEFAULT_FORMAT "jpeg"
#define HALOW_DEFAULT_TRANSPORT "udp"
#define HALOW_DEFAULT_FPS 12
#define HALOW_DEFAULT_QUALITY 45
#define HALOW_PREVIEW_W 640
#define HALOW_PREVIEW_H 360
#define HALOW_PREVIEW_BYTES (HALOW_PREVIEW_W * HALOW_PREVIEW_H * 2)

typedef enum {
    HALOW_FIELD_LOCAL_IP = 0,
    HALOW_FIELD_PEER_IP,
} halow_input_field_t;

static pthread_mutex_t halow_lock = PTHREAD_MUTEX_INITIALIZER;
static char halow_local_ip[HALOW_IP_MAX] = HALOW_DEFAULT_LOCAL_IP;
static char halow_peer_ip[HALOW_IP_MAX] = HALOW_DEFAULT_PEER_IP;
static char halow_netmask[HALOW_IP_MAX] = HALOW_DEFAULT_NETMASK;
static char halow_preset[16] = HALOW_DEFAULT_PRESET;
static char halow_frame_format[16] = HALOW_DEFAULT_FORMAT;
static char halow_transport[16] = HALOW_DEFAULT_TRANSPORT;
static unsigned halow_fps = HALOW_DEFAULT_FPS;
static unsigned halow_quality = HALOW_DEFAULT_QUALITY;
static char halow_status[HALOW_STATUS_MAX] = "Ready";
static char halow_log_text[HALOW_LOG_TEXT_MAX] = "No log yet";
static char halow_role[16] = "Idle";
static double halow_mbps;
static double halow_stream_fps;
static uint64_t halow_frames;
static uint64_t halow_dropped;
static uint64_t halow_errors;
static unsigned halow_last_frame_bytes;
static unsigned halow_stream_w = 320;
static unsigned halow_stream_h = 240;
static pid_t halow_pid = -1;

static lv_timer_t *halow_timer;
static lv_obj_t *halow_status_label;
static lv_obj_t *halow_config_label;
static lv_obj_t *halow_stats_label;
static lv_obj_t *halow_log_label;
static lv_obj_t *halow_preview_image;
static lv_obj_t *halow_preview_placeholder;
static lv_obj_t *halow_preset_btn_320;
static lv_obj_t *halow_preset_btn_640;
static lv_obj_t *halow_preset_btn_720;
static lv_obj_t *halow_quality_btn_low;
static lv_obj_t *halow_quality_btn_mid;
static lv_obj_t *halow_quality_btn_high;
static lv_obj_t *halow_format_btn_jpeg;
static lv_obj_t *halow_format_btn_raw;
static lv_obj_t *halow_transport_btn_udp;
static lv_obj_t *halow_transport_btn_tcp;
static lv_obj_t *halow_settings_overlay;
static uint8_t *halow_preview_pixels;
static lv_image_dsc_t halow_preview_dsc;
static uint64_t halow_preview_sig;
static int halow_preview_panel_w;
static int halow_preview_panel_h;

static void halow_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&halow_lock);
    va_start(ap, fmt);
    vsnprintf(halow_status, sizeof(halow_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&halow_lock);
}

static int halow_ipv4_valid(const char *text)
{
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    char tail;

    if(!text || !text[0]) {
        return 0;
    }
    if(sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return 0;
    }
    return a <= 255U && b <= 255U && c <= 255U && d <= 255U;
}

static int halow_frame_format_valid(const char *text)
{
    return text && (strcmp(text, "jpeg") == 0 || strcmp(text, "raw") == 0);
}

static int halow_transport_valid(const char *text)
{
    return text && (strcmp(text, "udp") == 0 || strcmp(text, "tcp") == 0);
}

static unsigned halow_pref_uint(const char *key, unsigned fallback,
                                unsigned min_value, unsigned max_value)
{
    char text[32];
    char *end = NULL;
    unsigned long value;

    ui_prefs_get(key, text, sizeof(text), "");
    if(!text[0]) {
        return fallback;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if(errno || !end || *end != '\0' || value < min_value ||
       value > max_value) {
        return fallback;
    }
    return (unsigned)value;
}

static void halow_save_uint(const char *key, unsigned value)
{
    char text[32];

    snprintf(text, sizeof(text), "%u", value);
    ui_prefs_set(key, text);
}

static void halow_apply_preset_dimensions_locked(void)
{
    if(strcmp(halow_preset, "720p") == 0) {
        halow_stream_w = 1280;
        halow_stream_h = 720;
    } else if(strcmp(halow_preset, "640x480") == 0) {
        halow_stream_w = 640;
        halow_stream_h = 480;
    } else {
        snprintf(halow_preset, sizeof(halow_preset), "320x240");
        halow_stream_w = 320;
        halow_stream_h = 240;
    }
}

static void halow_load_prefs(void)
{
    char schema[16];

    pthread_mutex_lock(&halow_lock);
    ui_prefs_get(HALOW_PREF_LOCAL_IP, halow_local_ip, sizeof(halow_local_ip),
                 HALOW_DEFAULT_LOCAL_IP);
    ui_prefs_get(HALOW_PREF_PEER_IP, halow_peer_ip, sizeof(halow_peer_ip),
                 HALOW_DEFAULT_PEER_IP);
    ui_prefs_get(HALOW_PREF_NETMASK, halow_netmask, sizeof(halow_netmask),
                 HALOW_DEFAULT_NETMASK);
    ui_prefs_get(HALOW_PREF_PRESET, halow_preset, sizeof(halow_preset),
                 HALOW_DEFAULT_PRESET);
    ui_prefs_get(HALOW_PREF_FORMAT, halow_frame_format,
                 sizeof(halow_frame_format), HALOW_DEFAULT_FORMAT);
    ui_prefs_get(HALOW_PREF_TRANSPORT, halow_transport,
                 sizeof(halow_transport), HALOW_DEFAULT_TRANSPORT);
    ui_prefs_get(HALOW_PREF_PRESET_SCHEMA, schema, sizeof(schema), "");
    if(strcmp(schema, HALOW_PRESET_SCHEMA) != 0) {
        snprintf(halow_preset, sizeof(halow_preset), "%s",
                 HALOW_DEFAULT_PRESET);
        ui_prefs_set(HALOW_PREF_PRESET, halow_preset);
        ui_prefs_set(HALOW_PREF_PRESET_SCHEMA, HALOW_PRESET_SCHEMA);
    }
    halow_fps = halow_pref_uint(HALOW_PREF_FPS, HALOW_DEFAULT_FPS, 1, 30);
    halow_quality = halow_pref_uint(HALOW_PREF_QUALITY,
                                    HALOW_DEFAULT_QUALITY, 5, 95);
    if(!halow_frame_format_valid(halow_frame_format)) {
        snprintf(halow_frame_format, sizeof(halow_frame_format), "%s",
                 HALOW_DEFAULT_FORMAT);
        ui_prefs_set(HALOW_PREF_FORMAT, halow_frame_format);
    }
    if(!halow_transport_valid(halow_transport)) {
        snprintf(halow_transport, sizeof(halow_transport), "%s",
                 HALOW_DEFAULT_TRANSPORT);
        ui_prefs_set(HALOW_PREF_TRANSPORT, halow_transport);
    }
    if(!halow_ipv4_valid(halow_local_ip)) {
        snprintf(halow_local_ip, sizeof(halow_local_ip), "%s",
                 HALOW_DEFAULT_LOCAL_IP);
    }
    if(!halow_ipv4_valid(halow_peer_ip)) {
        snprintf(halow_peer_ip, sizeof(halow_peer_ip), "%s",
                 HALOW_DEFAULT_PEER_IP);
    }
    if(!halow_ipv4_valid(halow_netmask)) {
        snprintf(halow_netmask, sizeof(halow_netmask), "%s",
                 HALOW_DEFAULT_NETMASK);
    }
    halow_apply_preset_dimensions_locked();
    pthread_mutex_unlock(&halow_lock);
}

static void halow_shell_quote(char *dst, size_t len, const char *src)
{
    size_t pos = 0;

    if(!dst || len == 0) {
        return;
    }
    dst[pos++] = '\'';
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

static int halow_process_running(void)
{
    int status;
    pid_t ret;

    if(halow_pid <= 0) {
        return 0;
    }
    ret = waitpid(halow_pid, &status, WNOHANG);
    if(ret == halow_pid) {
        halow_pid = -1;
        return 0;
    }
    if(kill(halow_pid, 0) == 0) {
        return 1;
    }
    if(errno == ESRCH) {
        waitpid(halow_pid, &status, WNOHANG);
        halow_pid = -1;
    }
    return 0;
}

static void halow_stop_process(void)
{
    FILE *fp;
    int rc;

    fp = fopen(HALOW_STOP_FILE, "w");
    if(fp) {
        fputs("stop\n", fp);
        fclose(fp);
    }
    if(halow_pid > 0) {
        kill(-halow_pid, SIGTERM);
        usleep(150000);
        kill(-halow_pid, SIGKILL);
        waitpid(halow_pid, NULL, 0);
        halow_pid = -1;
    }
    rc = system("killall k230_halow_udp_stream k230_camera_capture >/dev/null 2>&1 || true");
    (void)rc;
    halow_set_status("Stopped");
}

static void halow_start_role(const char *role)
{
    char local_ip[HALOW_IP_MAX];
    char peer_ip[HALOW_IP_MAX];
    char netmask[HALOW_IP_MAX];
    char preset[16];
    char frame_format[16];
    char transport[16];
    char local_q[HALOW_IP_MAX * 6];
    char peer_q[HALOW_IP_MAX * 6];
    char netmask_q[HALOW_IP_MAX * 6];
    char preset_q[64];
    char frame_format_q[64];
    char transport_q[64];
    char camera_transform[96];
    char cmd[1280];
    unsigned fps;
    unsigned quality;
    int landscape;
    int camera_rotate;
    int camera_flip_y;
    pid_t pid;

    if(!role || (strcmp(role, "tx") != 0 && strcmp(role, "rx") != 0)) {
        return;
    }
    if(halow_process_running()) {
        halow_set_status("Halow stream already running");
        return;
    }
    if(access(HALOW_SCRIPT, X_OK) != 0 || access(HALOW_UDP_BIN, X_OK) != 0) {
        halow_set_status("Halow helper missing");
        return;
    }

    pthread_mutex_lock(&halow_lock);
    snprintf(local_ip, sizeof(local_ip), "%s", halow_local_ip);
    snprintf(peer_ip, sizeof(peer_ip), "%s", halow_peer_ip);
    snprintf(netmask, sizeof(netmask), "%s", halow_netmask);
    snprintf(preset, sizeof(preset), "%s", halow_preset);
    snprintf(frame_format, sizeof(frame_format), "%s", halow_frame_format);
    snprintf(transport, sizeof(transport), "%s", halow_transport);
    fps = halow_fps;
    quality = halow_quality;
    halow_mbps = 0.0;
    halow_stream_fps = 0.0;
    halow_frames = 0;
    halow_dropped = 0;
    halow_errors = 0;
    halow_last_frame_bytes = 0;
    snprintf(halow_role, sizeof(halow_role), "%s",
             strcmp(role, "tx") == 0 ? "TX" : "RX");
    pthread_mutex_unlock(&halow_lock);

    if(!halow_ipv4_valid(local_ip)) {
        halow_set_status("Set local IP first");
        return;
    }
    if(strcmp(role, "tx") == 0 && !halow_ipv4_valid(peer_ip)) {
        halow_set_status("Set peer IP first");
        return;
    }
    if(!halow_ipv4_valid(netmask)) {
        snprintf(netmask, sizeof(netmask), "%s", HALOW_DEFAULT_NETMASK);
    }

    halow_shell_quote(local_q, sizeof(local_q), local_ip);
    halow_shell_quote(peer_q, sizeof(peer_q), peer_ip);
    halow_shell_quote(netmask_q, sizeof(netmask_q), netmask);
    halow_shell_quote(preset_q, sizeof(preset_q), preset);
    halow_shell_quote(frame_format_q, sizeof(frame_format_q), frame_format);
    halow_shell_quote(transport_q, sizeof(transport_q), transport);
    landscape = ui_is_landscape();
    camera_rotate = landscape ? 0 : 90;
    camera_flip_y = landscape ? 1 : 0;
    snprintf(camera_transform, sizeof(camera_transform), "--camera-rotate %d%s",
             camera_rotate, camera_flip_y ? " --camera-flip-y" : "");
    unlink(HALOW_LOG);
    unlink(HALOW_INNER_LOG);
    unlink(HALOW_STOP_FILE);
    unlink(HALOW_PREVIEW_FILE);
    unlink(HALOW_PREVIEW_META);

    if(strcmp(role, "tx") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "exec %s --role tx --peer %s --local-ip %s --netmask %s "
                 "--preset %s --fps %u --frame-format %s "
                 "--transport %s --jpeg-quality %u %s > %s 2>&1",
                 HALOW_SCRIPT, peer_q, local_q, netmask_q, preset_q, fps,
                 frame_format_q, transport_q, quality, camera_transform,
                 HALOW_LOG);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "exec %s --role rx --local-ip %s --netmask %s --preset %s "
                 "--fps %u --frame-format %s --transport %s "
                 "--jpeg-quality %u > %s 2>&1",
                 HALOW_SCRIPT, local_q, netmask_q, preset_q, fps,
                 frame_format_q, transport_q, quality, HALOW_LOG);
    }

    pid = fork();
    if(pid < 0) {
        halow_set_status("Start failed");
        return;
    }
    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    halow_pid = pid;
    halow_set_status("%s started", strcmp(role, "tx") == 0 ? "TX" : "RX");
}

static int halow_file_size(const char *path)
{
    struct stat st;

    if(!path || stat(path, &st) != 0 || st.st_size < 0 ||
       st.st_size > 0x7FFFFFFF) {
        return -1;
    }
    return (int)st.st_size;
}

static uint64_t halow_file_sig(const char *path)
{
    struct stat st;

    if(!path || stat(path, &st) != 0) {
        return 0;
    }
#if defined(__linux__)
    return ((uint64_t)st.st_mtim.tv_sec * 1000000000ULL) ^
           (uint64_t)st.st_mtim.tv_nsec ^ (uint64_t)st.st_size;
#else
    return ((uint64_t)st.st_mtime << 32) ^ (uint64_t)st.st_size;
#endif
}

static int halow_load_preview(void)
{
    FILE *fp;
    size_t bytes_read;
    uint64_t sig;

    if(halow_file_size(HALOW_PREVIEW_FILE) != HALOW_PREVIEW_BYTES) {
        return 0;
    }
    sig = halow_file_sig(HALOW_PREVIEW_FILE);
    if(sig == halow_preview_sig && halow_preview_dsc.data) {
        return 1;
    }
    if(!halow_preview_pixels) {
        halow_preview_pixels = (uint8_t *)malloc(HALOW_PREVIEW_BYTES);
        if(!halow_preview_pixels) {
            return 0;
        }
    }
    fp = fopen(HALOW_PREVIEW_FILE, "rb");
    if(!fp) {
        return 0;
    }
    bytes_read = fread(halow_preview_pixels, 1, HALOW_PREVIEW_BYTES, fp);
    fclose(fp);
    if(bytes_read != HALOW_PREVIEW_BYTES) {
        return 0;
    }
    memset(&halow_preview_dsc, 0, sizeof(halow_preview_dsc));
    halow_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    halow_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    halow_preview_dsc.header.w = HALOW_PREVIEW_W;
    halow_preview_dsc.header.h = HALOW_PREVIEW_H;
    halow_preview_dsc.header.stride = HALOW_PREVIEW_W * 2;
    halow_preview_dsc.data_size = HALOW_PREVIEW_BYTES;
    halow_preview_dsc.data = halow_preview_pixels;
    halow_preview_sig = sig;
    return 1;
}

static void halow_read_log_tail(char *out, size_t len)
{
    FILE *fp;
    char line[192];
    char last[HALOW_LOG_TEXT_MAX] = "";

    if(!out || len == 0) {
        return;
    }
    out[0] = '\0';
    fp = fopen(HALOW_LOG, "r");
    if(!fp) {
        snprintf(out, len, "%s", "No log yet");
        return;
    }
    while(fgets(line, sizeof(line), fp)) {
        size_t last_len;
        size_t line_len;

        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }
        last_len = strlen(last);
        line_len = strlen(line);
        if(last_len + line_len + 2U >= sizeof(last)) {
            memmove(last, last + sizeof(last) / 3U,
                    strlen(last + sizeof(last) / 3U) + 1U);
            last_len = strlen(last);
        }
        snprintf(last + last_len, sizeof(last) - last_len, "%s%s",
                 last_len ? "\n" : "", line);
    }
    fclose(fp);
    snprintf(out, len, "%s", last[0] ? last : "No log yet");
}

static void halow_parse_key_value_line(const char *line)
{
    const char *p = line;

    while(p && *p) {
        char key[32];
        char value[64];
        int used = 0;

        while(*p == ' ') {
            p++;
        }
        if(sscanf(p, "%31[^=]=%63s%n", key, value, &used) != 2 ||
           used <= 0) {
            break;
        }
        if(strcmp(key, "role") == 0) {
            snprintf(halow_role, sizeof(halow_role), "%s", value);
        } else if(strcmp(key, "mbps") == 0) {
            halow_mbps = strtod(value, NULL);
        } else if(strcmp(key, "fps") == 0) {
            halow_stream_fps = strtod(value, NULL);
        } else if(strcmp(key, "frames") == 0) {
            halow_frames = strtoull(value, NULL, 10);
        } else if(strcmp(key, "dropped") == 0) {
            halow_dropped = strtoull(value, NULL, 10);
        } else if(strcmp(key, "errors") == 0) {
            halow_errors = strtoull(value, NULL, 10);
        } else if(strcmp(key, "last") == 0 ||
                  strcmp(key, "last_frame_bytes") == 0) {
            halow_last_frame_bytes = (unsigned)strtoul(value, NULL, 10);
        }
        p += used;
    }
}

static void halow_parse_meta(void)
{
    FILE *fp;
    char line[192];

    fp = fopen(HALOW_PREVIEW_META, "r");
    if(!fp) {
        return;
    }
    pthread_mutex_lock(&halow_lock);
    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        halow_parse_key_value_line(line);
    }
    pthread_mutex_unlock(&halow_lock);
    fclose(fp);
}

static void halow_parse_log_stats(const char *text)
{
    const char *p = text;
    const char *last = NULL;

    while((p = strstr(p, "STAT ")) != NULL) {
        last = p + 5;
        p += 5;
    }
    if(!last) {
        return;
    }
    pthread_mutex_lock(&halow_lock);
    halow_parse_key_value_line(last);
    pthread_mutex_unlock(&halow_lock);
}

static void halow_update_preview(void)
{
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t scale;
    int avail_w;
    int avail_h;

    if(!halow_preview_image || !lv_obj_is_valid(halow_preview_image)) {
        return;
    }
    if(!halow_load_preview()) {
        if(halow_preview_placeholder &&
           lv_obj_is_valid(halow_preview_placeholder)) {
            lv_obj_clear_flag(halow_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(halow_preview_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    avail_w = halow_preview_panel_w - 24;
    avail_h = halow_preview_panel_h - 24;
    if(avail_w < 160) {
        avail_w = 160;
    }
    if(avail_h < 90) {
        avail_h = 90;
    }
    scale_x = (uint32_t)((uint64_t)avail_w * LV_SCALE_NONE /
                         HALOW_PREVIEW_W);
    scale_y = (uint32_t)((uint64_t)avail_h * LV_SCALE_NONE /
                         HALOW_PREVIEW_H);
    scale = scale_x < scale_y ? scale_x : scale_y;
    if(scale < 32U) {
        scale = 32U;
    }
    lv_image_cache_drop(&halow_preview_dsc);
    lv_image_set_src(halow_preview_image, &halow_preview_dsc);
    lv_image_set_scale(halow_preview_image, scale);
    lv_obj_align(halow_preview_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(halow_preview_image, LV_OBJ_FLAG_HIDDEN);
    if(halow_preview_placeholder &&
       lv_obj_is_valid(halow_preview_placeholder)) {
        lv_obj_add_flag(halow_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(halow_preview_image);
    app_request_fast_refresh();
}

static void halow_timer_cb(lv_timer_t *timer)
{
    char local_ip[HALOW_IP_MAX];
    char peer_ip[HALOW_IP_MAX];
    char preset[16];
    char frame_format[16];
    char transport[16];
    char status[HALOW_STATUS_MAX];
    char role[16];
    char log_text[HALOW_LOG_TEXT_MAX];
    char eth_ip[64];
    unsigned fps;
    unsigned quality;
    unsigned stream_w;
    unsigned stream_h;
    double mbps;
    double stream_fps;
    uint64_t frames;
    uint64_t dropped;
    uint64_t errors;
    unsigned last_frame;
    int running;
    int carrier;

    (void)timer;
    running = halow_process_running();
    halow_read_log_tail(log_text, sizeof(log_text));
    halow_parse_log_stats(log_text);
    halow_parse_meta();

    pthread_mutex_lock(&halow_lock);
    snprintf(local_ip, sizeof(local_ip), "%s", halow_local_ip);
    snprintf(peer_ip, sizeof(peer_ip), "%s", halow_peer_ip);
    snprintf(preset, sizeof(preset), "%s", halow_preset);
    snprintf(frame_format, sizeof(frame_format), "%s", halow_frame_format);
    snprintf(transport, sizeof(transport), "%s", halow_transport);
    snprintf(status, sizeof(status), "%s", halow_status);
    snprintf(role, sizeof(role), "%s", running ? halow_role : "Idle");
    fps = halow_fps;
    quality = halow_quality;
    stream_w = halow_stream_w;
    stream_h = halow_stream_h;
    mbps = halow_mbps;
    stream_fps = halow_stream_fps;
    frames = halow_frames;
    dropped = halow_dropped;
    errors = halow_errors;
    last_frame = halow_last_frame_bytes;
    pthread_mutex_unlock(&halow_lock);

    if(halow_status_label && lv_obj_is_valid(halow_status_label)) {
        lv_label_set_text_fmt(halow_status_label, "%s  %s",
                              ui_tr(status), role);
    }
    if(halow_config_label && lv_obj_is_valid(halow_config_label)) {
        carrier = ui_read_iface_carrier(NET_ETH_IFACE);
        if(carrier == 0) {
            snprintf(eth_ip, sizeof(eth_ip), "No link");
        } else if(ui_read_iface_ip(NET_ETH_IFACE, eth_ip, sizeof(eth_ip)) != 0) {
            snprintf(eth_ip, sizeof(eth_ip), "No eth0 IP");
        }
        lv_label_set_text_fmt(halow_config_label,
                              "eth0 %s\nLocal %s  Peer %s\n%s  %ux%u  %ufps  %s/%s  Q%u",
                              eth_ip, local_ip, peer_ip, preset, stream_w,
                              stream_h, fps,
                              strcmp(transport, "tcp") == 0 ? "TCP" : "UDP",
                              strcmp(frame_format, "raw") == 0 ? "RAW" :
                              "JPEG", quality);
    }
    if(halow_stats_label && lv_obj_is_valid(halow_stats_label)) {
        lv_label_set_text_fmt(halow_stats_label,
                              "Rate %.2f Mbps  %.1f fps\nFrames %llu  Drop %llu  Err %llu\nLast %.1f KB",
                              mbps, stream_fps,
                              (unsigned long long)frames,
                              (unsigned long long)dropped,
                              (unsigned long long)errors,
                              (double)last_frame / 1024.0);
    }
    if(halow_log_label && lv_obj_is_valid(halow_log_label)) {
        lv_label_set_text(halow_log_label, log_text);
    }
    halow_update_preview();
}

static void halow_start_tx_event_cb(lv_event_t *event)
{
    (void)event;
    halow_start_role("tx");
}

static void halow_start_rx_event_cb(lv_event_t *event)
{
    (void)event;
    halow_start_role("rx");
}

static void halow_stop_event_cb(lv_event_t *event)
{
    (void)event;
    halow_stop_process();
}

static void halow_ip_submit_cb(const char *text, void *user_data)
{
    halow_input_field_t field =
        (halow_input_field_t)(intptr_t)user_data;
    char value[HALOW_IP_MAX];

    snprintf(value, sizeof(value), "%s", text ? text : "");
    ui_trim_text(value);
    if(!halow_ipv4_valid(value)) {
        halow_set_status("Invalid IPv4 address");
        return;
    }

    pthread_mutex_lock(&halow_lock);
    if(field == HALOW_FIELD_LOCAL_IP) {
        snprintf(halow_local_ip, sizeof(halow_local_ip), "%s", value);
        ui_prefs_set(HALOW_PREF_LOCAL_IP, value);
    } else {
        snprintf(halow_peer_ip, sizeof(halow_peer_ip), "%s", value);
        ui_prefs_set(HALOW_PREF_PEER_IP, value);
    }
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("IP saved");
}

static void halow_edit_ip_event_cb(lv_event_t *event)
{
    halow_input_field_t field =
        (halow_input_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;
    char initial[HALOW_IP_MAX];

    pthread_mutex_lock(&halow_lock);
    snprintf(initial, sizeof(initial), "%s",
             field == HALOW_FIELD_LOCAL_IP ? halow_local_ip : halow_peer_ip);
    pthread_mutex_unlock(&halow_lock);

    memset(&config, 0, sizeof(config));
    config.title = field == HALOW_FIELD_LOCAL_IP ? "Local IP" : "Peer IP";
    config.placeholder = "IPv4 address";
    config.initial_text = initial;
    config.max_length = HALOW_IP_MAX - 1U;
    config.min_length = 7U;
    config.min_length_text = "Enter IPv4 address";
    config.submit_cb = halow_ip_submit_cb;
    config.user_data = (void *)(intptr_t)field;
    config.submit_text = "Save";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void halow_apply_local_ip_event_cb(lv_event_t *event)
{
    char local_ip[HALOW_IP_MAX];
    char netmask[HALOW_IP_MAX];
    char cmd[256];

    (void)event;
    pthread_mutex_lock(&halow_lock);
    snprintf(local_ip, sizeof(local_ip), "%s", halow_local_ip);
    snprintf(netmask, sizeof(netmask), "%s", halow_netmask);
    pthread_mutex_unlock(&halow_lock);
    if(!halow_ipv4_valid(local_ip)) {
        halow_set_status("Set local IP first");
        return;
    }
    snprintf(cmd, sizeof(cmd), "ifconfig " NET_ETH_IFACE " up && "
             "ifconfig " NET_ETH_IFACE " %s netmask %s",
             local_ip, netmask);
    if(ui_shell_exit_code(system(cmd)) == 0) {
        halow_set_status("Local IP applied");
    } else {
        halow_set_status("Local IP apply failed");
    }
}

static void halow_style_choice(lv_obj_t *btn, int active, uint32_t color)
{
    if(!btn || !lv_obj_is_valid(btn)) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? color : 0x222832), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(active ? color : 0x2A3037),
                                  0);
}

static void halow_update_settings_buttons(void)
{
    char preset[16];
    char frame_format[16];
    char transport[16];
    unsigned quality;

    pthread_mutex_lock(&halow_lock);
    snprintf(preset, sizeof(preset), "%s", halow_preset);
    snprintf(frame_format, sizeof(frame_format), "%s", halow_frame_format);
    snprintf(transport, sizeof(transport), "%s", halow_transport);
    quality = halow_quality;
    pthread_mutex_unlock(&halow_lock);
    halow_style_choice(halow_preset_btn_320, strcmp(preset, "320x240") == 0,
                       0x25C281);
    halow_style_choice(halow_preset_btn_640, strcmp(preset, "640x480") == 0,
                       0x25C281);
    halow_style_choice(halow_preset_btn_720, strcmp(preset, "720p") == 0,
                       0x25C281);
    halow_style_choice(halow_quality_btn_low, quality <= 35U, 0x3DA5FF);
    halow_style_choice(halow_quality_btn_mid, quality > 35U && quality < 60U,
                       0x3DA5FF);
    halow_style_choice(halow_quality_btn_high, quality >= 60U, 0x3DA5FF);
    halow_style_choice(halow_format_btn_jpeg,
                       strcmp(frame_format, "jpeg") == 0, 0xF5B84B);
    halow_style_choice(halow_format_btn_raw,
                       strcmp(frame_format, "raw") == 0, 0xF5B84B);
    halow_style_choice(halow_transport_btn_udp,
                       strcmp(transport, "udp") == 0, 0x8B5CF6);
    halow_style_choice(halow_transport_btn_tcp,
                       strcmp(transport, "tcp") == 0, 0x8B5CF6);
}

static void halow_preset_event_cb(lv_event_t *event)
{
    const char *preset = (const char *)lv_event_get_user_data(event);

    if(!preset) {
        return;
    }
    pthread_mutex_lock(&halow_lock);
    snprintf(halow_preset, sizeof(halow_preset), "%s", preset);
    halow_apply_preset_dimensions_locked();
    ui_prefs_set(HALOW_PREF_PRESET, halow_preset);
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("Preset saved");
    halow_update_settings_buttons();
}

static void halow_quality_event_cb(lv_event_t *event)
{
    unsigned quality = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    pthread_mutex_lock(&halow_lock);
    halow_quality = quality;
    halow_save_uint(HALOW_PREF_QUALITY, halow_quality);
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("Quality saved");
    halow_update_settings_buttons();
}

static void halow_format_event_cb(lv_event_t *event)
{
    const char *frame_format = (const char *)lv_event_get_user_data(event);

    if(!halow_frame_format_valid(frame_format)) {
        return;
    }
    pthread_mutex_lock(&halow_lock);
    snprintf(halow_frame_format, sizeof(halow_frame_format), "%s",
             frame_format);
    ui_prefs_set(HALOW_PREF_FORMAT, halow_frame_format);
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("Format saved");
    halow_update_settings_buttons();
}

static void halow_transport_event_cb(lv_event_t *event)
{
    const char *transport = (const char *)lv_event_get_user_data(event);

    if(!halow_transport_valid(transport)) {
        return;
    }
    pthread_mutex_lock(&halow_lock);
    snprintf(halow_transport, sizeof(halow_transport), "%s", transport);
    ui_prefs_set(HALOW_PREF_TRANSPORT, halow_transport);
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("Transport saved");
    halow_update_settings_buttons();
}

static void halow_fps_event_cb(lv_event_t *event)
{
    unsigned fps = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    pthread_mutex_lock(&halow_lock);
    halow_fps = fps;
    halow_save_uint(HALOW_PREF_FPS, halow_fps);
    pthread_mutex_unlock(&halow_lock);
    halow_set_status("FPS saved");
}

static void halow_settings_close(void)
{
    if(halow_settings_overlay && lv_obj_is_valid(halow_settings_overlay)) {
        lv_obj_delete(halow_settings_overlay);
    }
    halow_settings_overlay = NULL;
    halow_preset_btn_320 = NULL;
    halow_preset_btn_640 = NULL;
    halow_preset_btn_720 = NULL;
    halow_quality_btn_low = NULL;
    halow_quality_btn_mid = NULL;
    halow_quality_btn_high = NULL;
    halow_format_btn_jpeg = NULL;
    halow_format_btn_raw = NULL;
    halow_transport_btn_udp = NULL;
    halow_transport_btn_tcp = NULL;
}

static void halow_settings_close_event_cb(lv_event_t *event)
{
    (void)event;
    halow_settings_close();
}

static void halow_settings_event_cb(lv_event_t *event)
{
    lv_obj_t *card;
    lv_obj_t *section;
    lv_obj_t *btn;
    int sw = ui_screen_width();
    int sh = ui_screen_height();
    int card_w = ui_is_landscape() ? 700 : 520;
    int card_h = ui_is_landscape() ? 420 : 610;
    int pad = 22;
    int gap = 14;
    int col_w;
    int y;

    (void)event;
    if(card_w > sw - 80) {
        card_w = sw - 80;
    }
    if(card_h > sh - 80) {
        card_h = sh - 80;
    }
    if(card_w < 360) {
        card_w = 360;
    }
    if(card_h < 360) {
        card_h = 360;
    }

    halow_settings_close();
    halow_settings_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(halow_settings_overlay);
    lv_obj_set_style_bg_color(halow_settings_overlay, lv_color_hex(0x05080C),
                              0);
    lv_obj_set_style_bg_opa(halow_settings_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(halow_settings_overlay, 0, 0);
    lv_obj_clear_flag(halow_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = ui_panel(halow_settings_overlay, (sw - card_w) / 2,
                    (sh - card_h) / 2, card_w, card_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111821), 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);

    ui_label(card, "Halow settings", &lv_font_montserrat_26, 0xF2F5F8);
    btn = ui_command_button(card, card_w - pad * 2 - 92, 0, 92, "Close",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, halow_settings_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    section = ui_label(card, "Resolution", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, 62);
    y = 96;
    col_w = (card_w - pad * 2 - gap * 2) / 3;
    halow_preset_btn_320 =
        ui_command_button(card, 0, y, col_w, "320x240", 0xF2F5F8);
    lv_obj_add_event_cb(halow_preset_btn_320, halow_preset_event_cb,
                        LV_EVENT_CLICKED, "320x240");
    halow_preset_btn_640 =
        ui_command_button(card, col_w + gap, y, col_w, "640x480",
                          0xF2F5F8);
    lv_obj_add_event_cb(halow_preset_btn_640, halow_preset_event_cb,
                        LV_EVENT_CLICKED, "640x480");
    halow_preset_btn_720 =
        ui_command_button(card, (col_w + gap) * 2, y, col_w, "720p",
                          0xF2F5F8);
    lv_obj_add_event_cb(halow_preset_btn_720, halow_preset_event_cb,
                        LV_EVENT_CLICKED, "720p");

    section = ui_label(card, "Transport format", &lv_font_montserrat_18,
                       0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap) / 2;
    halow_format_btn_jpeg =
        ui_command_button(card, 0, y, col_w, "JPEG", 0xF2F5F8);
    lv_obj_add_event_cb(halow_format_btn_jpeg, halow_format_event_cb,
                        LV_EVENT_CLICKED, "jpeg");
    halow_format_btn_raw =
        ui_command_button(card, col_w + gap, y, col_w, "RAW LAN",
                          0xF2F5F8);
    lv_obj_add_event_cb(halow_format_btn_raw, halow_format_event_cb,
                        LV_EVENT_CLICKED, "raw");

    section = ui_label(card, "Transport protocol", &lv_font_montserrat_18,
                       0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap) / 2;
    halow_transport_btn_udp =
        ui_command_button(card, 0, y, col_w, "UDP", 0xF2F5F8);
    lv_obj_add_event_cb(halow_transport_btn_udp, halow_transport_event_cb,
                        LV_EVENT_CLICKED, "udp");
    halow_transport_btn_tcp =
        ui_command_button(card, col_w + gap, y, col_w, "TCP LAN",
                          0xF2F5F8);
    lv_obj_add_event_cb(halow_transport_btn_tcp, halow_transport_event_cb,
                        LV_EVENT_CLICKED, "tcp");

    section = ui_label(card, "JPEG quality", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap * 2) / 3;
    halow_quality_btn_low =
        ui_command_button(card, 0, y, col_w, "Q35", 0xF2F5F8);
    lv_obj_add_event_cb(halow_quality_btn_low, halow_quality_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)35U);
    halow_quality_btn_mid =
        ui_command_button(card, col_w + gap, y, col_w, "Q45", 0xF2F5F8);
    lv_obj_add_event_cb(halow_quality_btn_mid, halow_quality_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)45U);
    halow_quality_btn_high =
        ui_command_button(card, (col_w + gap) * 2, y, col_w, "Q60",
                          0xF2F5F8);
    lv_obj_add_event_cb(halow_quality_btn_high, halow_quality_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)60U);

    section = ui_label(card, "Frame rate", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 82);
    y += 116;
    col_w = (card_w - pad * 2 - gap * 2) / 3;
    btn = ui_command_button(card, 0, y, col_w, "8 fps", 0xF2F5F8);
    lv_obj_add_event_cb(btn, halow_fps_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)8U);
    btn = ui_command_button(card, col_w + gap, y, col_w, "12 fps",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, halow_fps_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)12U);
    btn = ui_command_button(card, (col_w + gap) * 2, y, col_w, "15 fps",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, halow_fps_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)15U);

    section = ui_label(card,
                       "JPEG/UDP is safer for low bandwidth HaLow links. RAW/TCP LAN removes JPEG encode/decode and UDP chunk overhead for wired or high throughput tests.",
                       &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(section, 0, y + 86);
    lv_obj_set_width(section, card_w - pad * 2);
    lv_label_set_long_mode(section, LV_LABEL_LONG_WRAP);
    halow_update_settings_buttons();
}

void ui_halow_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *preview;
    lv_obj_t *side;
    lv_obj_t *btn;
    lv_obj_t *placeholder_icon;
    lv_obj_t *placeholder_text;
    int landscape = ui_is_landscape();
    int screen_w = ui_screen_width();
    int body_h = ui_body_height(144);
    int margin = landscape ? 24 : 24;
    int gap = landscape ? 22 : 18;
    int body_w = screen_w - margin * 2;
    int preview_x = margin;
    int preview_y = 20;
    int preview_w;
    int preview_h;
    int side_x;
    int side_y;
    int side_w;
    int side_h;
    int inner_w;
    int row_w;
    int button_gap;
    int y;

    halow_load_prefs();
    ui_create_header(scr, "Halow");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    if(landscape) {
        preview_h = body_h - 40;
        if(preview_h < 320) {
            preview_h = 320;
        }
        preview_w = body_w * 60 / 100;
        if(preview_w < 420) {
            preview_w = 420;
        }
        if(preview_w > body_w - 360 - gap) {
            preview_w = body_w - 360 - gap;
        }
        side_x = preview_x + preview_w + gap;
        side_y = preview_y;
        side_w = screen_w - side_x - margin;
        side_h = preview_h;
    } else {
        preview_w = body_w;
        preview_h = 390;
        side_x = margin;
        side_y = preview_y + preview_h + gap;
        side_w = preview_w;
        side_h = body_h - side_y - 32;
        if(side_h < 520) {
            side_h = 520;
        }
    }

    preview = ui_panel(body, preview_x, preview_y, preview_w, preview_h);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x081018), 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x174C3A), 0);

    halow_preview_panel_w = preview_w - 32;
    halow_preview_panel_h = preview_h - 32;
    halow_preview_image = lv_image_create(preview);
    lv_obj_add_flag(halow_preview_image, LV_OBJ_FLAG_HIDDEN);

    halow_preview_placeholder = lv_obj_create(preview);
    lv_obj_set_pos(halow_preview_placeholder, 16, 16);
    lv_obj_set_size(halow_preview_placeholder, preview_w - 32,
                    preview_h - 32);
    lv_obj_set_style_bg_color(halow_preview_placeholder,
                              lv_color_hex(0x10201B), 0);
    lv_obj_set_style_bg_opa(halow_preview_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(halow_preview_placeholder, 0, 0);
    lv_obj_set_style_radius(halow_preview_placeholder, 8, 0);
    lv_obj_clear_flag(halow_preview_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    placeholder_icon =
        ui_label(halow_preview_placeholder, LV_SYMBOL_VIDEO,
                 &lv_font_montserrat_32, 0x25C281);
    lv_obj_align(placeholder_icon, LV_ALIGN_CENTER, 0, -36);
    placeholder_text =
        ui_label(halow_preview_placeholder,
                 "Set local IP, enter peer IP, then start TX or RX",
                 &lv_font_montserrat_18, 0xC9D3DF);
    lv_obj_set_width(placeholder_text, preview_w - 72);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(placeholder_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(placeholder_text, LV_ALIGN_CENTER, 0, 28);

    side = ui_panel(body, side_x, side_y, side_w, side_h);
    lv_obj_set_style_bg_color(side, lv_color_hex(0x151B22), 0);
    ui_make_scrollable(side, 48);
    inner_w = side_w - 32;
    if(inner_w < 260) {
        inner_w = 260;
    }

    ui_label(side, "Halow video", &lv_font_montserrat_24, 0xF2F5F8);
    halow_status_label =
        ui_label(side, "Ready", &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_pos(halow_status_label, 0, 42);
    lv_obj_set_width(halow_status_label, inner_w);
    lv_label_set_long_mode(halow_status_label, LV_LABEL_LONG_DOT);

    halow_config_label = ui_label(side, "--", &lv_font_montserrat_16,
                                  0xC9D3DF);
    lv_obj_set_pos(halow_config_label, 0, 74);
    lv_obj_set_width(halow_config_label, inner_w);
    lv_label_set_long_mode(halow_config_label, LV_LABEL_LONG_WRAP);

    row_w = (inner_w - gap) / 2;
    if(row_w < 118) {
        row_w = 118;
    }
    button_gap = landscape ? 10 : 18;
    y = landscape ? 148 : 190;
    btn = ui_command_button(side, 0, y, row_w, "Local IP", 0x60A5FA);
    lv_obj_add_event_cb(btn, halow_edit_ip_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)HALOW_FIELD_LOCAL_IP);
    btn = ui_command_button(side, row_w + gap, y, row_w, "Peer IP",
                            0x60A5FA);
    lv_obj_add_event_cb(btn, halow_edit_ip_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)HALOW_FIELD_PEER_IP);
    y += 60 + button_gap;
    btn = ui_command_button(side, 0, y, row_w, "Apply IP", 0x25C281);
    lv_obj_add_event_cb(btn, halow_apply_local_ip_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(side, row_w + gap, y, row_w, "Settings",
                            0xF2F5F8);
    lv_obj_add_event_cb(btn, halow_settings_event_cb, LV_EVENT_CLICKED, NULL);
    y += 60 + button_gap;
    btn = ui_command_button(side, 0, y, row_w, "Start RX", 0x3DA5FF);
    lv_obj_add_event_cb(btn, halow_start_rx_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(side, row_w + gap, y, row_w, "Start TX",
                            0x25C281);
    lv_obj_add_event_cb(btn, halow_start_tx_event_cb, LV_EVENT_CLICKED, NULL);
    y += 60 + button_gap;
    btn = ui_command_button(side, 0, y, row_w, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(btn, halow_stop_event_cb, LV_EVENT_CLICKED, NULL);

    y += landscape ? 46 : 76;
    halow_stats_label = ui_label(side, "--", &lv_font_montserrat_16,
                                 0x9AA4AF);
    lv_obj_set_pos(halow_stats_label, 0, y);
    lv_obj_set_width(halow_stats_label, inner_w);
    lv_label_set_long_mode(halow_stats_label, LV_LABEL_LONG_WRAP);

    y += landscape ? 88 : 110;
    halow_log_label =
        ui_label(side, "No log yet", &lv_font_montserrat_14, 0xC9D3DF);
    lv_obj_set_pos(halow_log_label, 0, y);
    lv_obj_set_width(halow_log_label, inner_w);
    lv_label_set_long_mode(halow_log_label, LV_LABEL_LONG_WRAP);

    if(!halow_timer) {
        halow_timer = lv_timer_create(halow_timer_cb, 250, NULL);
    }
    halow_timer_cb(NULL);
}

void ui_halow_cleanup(void)
{
    if(halow_timer) {
        lv_timer_delete(halow_timer);
        halow_timer = NULL;
    }
    halow_stop_process();
    halow_settings_close();
    halow_status_label = NULL;
    halow_config_label = NULL;
    halow_stats_label = NULL;
    halow_log_label = NULL;
    halow_preview_image = NULL;
    halow_preview_placeholder = NULL;
    free(halow_preview_pixels);
    halow_preview_pixels = NULL;
    memset(&halow_preview_dsc, 0, sizeof(halow_preview_dsc));
    halow_preview_sig = 0;
}
