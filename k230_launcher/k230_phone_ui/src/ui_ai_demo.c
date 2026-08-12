#include "ui_ai_demo.h"

#include "ui_i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#define AI_DEMO_LOG "/tmp/k230_ai_demo.log"
#define AI_DEMO_LOG_TEXT_MAX 1536
#define AI_DEMO_PREVIEW_RAW "/tmp/k230_ai_demo_preview.rgb565"
#define AI_DEMO_PREVIEW_W 488
#define AI_DEMO_PREVIEW_H 244
#define AI_DEMO_PREVIEW_BYTES (AI_DEMO_PREVIEW_W * AI_DEMO_PREVIEW_H * 2)

typedef struct {
    const char *title;
    const char *subtitle;
    const char *workdir;
    const char *command;
    const char *script_path;
    const char *required_path;
    const char *result_path;
    int long_running;
    uint32_t color;
} ai_demo_t;

static const ai_demo_t ai_demos[] = {
    {
        "AI2D + KPU",
        "Offline resize and KPU self-test",
        "/root/app/ai2d_kpu",
        "sh ./run.sh",
        "/root/app/ai2d_kpu/run.sh",
        "/root/app/ai2d_kpu/ai2d_kpu.elf",
        "/root/app/ai2d_kpu/result.bin",
        0,
        0x3DA5FF,
    },
    {
        "Face image",
        "Run face detection on bundled test.jpg",
        "/root/app/face_detect",
        "sh ./face_detect_image.sh",
        "/root/app/face_detect/face_detect_image.sh",
        "/root/app/face_detect/test.jpg",
        "/root/app/face_detect/face_detection_result.jpg",
        0,
        0x25C281,
    },
    {
        "Face camera",
        "Live ISP demo, may take display ownership",
        "/root/app/face_detect",
        "sh ./face_detect_video.sh",
        "/root/app/face_detect/face_detect_video.sh",
        "/root/app/face_detect/face_detect.elf",
        NULL,
        1,
        0xEC4899,
    },
};

static pthread_mutex_t ai_demo_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_obj_t *ai_demo_status_label;
static lv_obj_t *ai_demo_active_label;
static lv_obj_t *ai_demo_result_label;
static lv_obj_t *ai_demo_preview_image;
static lv_obj_t *ai_demo_preview_placeholder;
static lv_obj_t *ai_demo_log_label;
static lv_obj_t *ai_demo_buttons[sizeof(ai_demos) / sizeof(ai_demos[0])];
static lv_obj_t *ai_demo_stop_btn;
static lv_timer_t *ai_demo_timer;
static pid_t ai_demo_pid = -1;
static int ai_demo_active_index = -1;
static int ai_demo_stop_requested;
static uint64_t ai_demo_stop_request_us;
static char ai_demo_status_text[192] = "Ready";
static char ai_demo_result_text[192] = "No result yet";
static char ai_demo_preview_source[256];
static time_t ai_demo_preview_mtime;
static uint8_t *ai_demo_preview_pixels;
static lv_image_dsc_t ai_demo_preview_dsc;

static size_t ai_demo_count(void)
{
    return sizeof(ai_demos) / sizeof(ai_demos[0]);
}

static int ai_demo_available(const ai_demo_t *demo)
{
    return ui_path_exists(demo->script_path) &&
           (!demo->required_path || ui_path_exists(demo->required_path));
}

static long ai_demo_file_size(const char *path)
{
    struct stat st;

    if(!path || stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static time_t ai_demo_file_mtime(const char *path)
{
    struct stat st;

    if(!path || stat(path, &st) != 0) {
        return 0;
    }
    return st.st_mtime;
}

static int ai_demo_shell_quote(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;

    if(!in || !out || out_len < 3) {
        return -1;
    }

    out[pos++] = '\'';
    for(size_t i = 0; in[i] != '\0'; i++) {
        if(in[i] == '\'') {
            if(pos + 4 >= out_len) {
                break;
            }
            out[pos++] = '\'';
            out[pos++] = '\\';
            out[pos++] = '\'';
            out[pos++] = '\'';
        } else {
            if(pos + 2 >= out_len) {
                break;
            }
            out[pos++] = in[i];
        }
    }
    out[pos++] = '\'';
    out[pos] = '\0';
    return 0;
}

static void ai_demo_append_log(const char *text)
{
    FILE *fp = fopen(AI_DEMO_LOG, "a");

    if(!fp) {
        return;
    }
    fprintf(fp, "%s\n", text ? text : "");
    fclose(fp);
}

static void ai_demo_reset_log(const ai_demo_t *demo)
{
    FILE *fp = fopen(AI_DEMO_LOG, "w");
    time_t now = time(NULL);
    struct tm tm_now;
    char now_text[48];

    if(!fp) {
        return;
    }

    localtime_r(&now, &tm_now);
    strftime(now_text, sizeof(now_text), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(fp, "=== %s ===\n", now_text);
    fprintf(fp, "demo: %s\n", demo->title);
    fprintf(fp, "workdir: %s\n", demo->workdir);
    fprintf(fp, "command: %s\n\n", demo->command);
    fclose(fp);
}

static int ai_demo_convert_preview(const char *path)
{
    char quoted[512];
    char cmd[1024];
    int rc;

    unlink(AI_DEMO_PREVIEW_RAW);

    if(!path || !ui_path_exists(path)) {
        return 0;
    }
    if(access("/usr/bin/ffmpeg", X_OK) != 0) {
        ai_demo_append_log("preview: ffmpeg missing");
        return 0;
    }
    if(ai_demo_shell_quote(path, quoted, sizeof(quoted)) != 0) {
        ai_demo_append_log("preview: quote failed");
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -nostdin -hide_banner -loglevel error -i %s "
             "-vf 'scale=%d:%d:force_original_aspect_ratio=decrease,"
             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x0B1016' "
             "-pix_fmt rgb565le -f rawvideo " AI_DEMO_PREVIEW_RAW
             " >>" AI_DEMO_LOG " 2>&1",
             quoted, AI_DEMO_PREVIEW_W, AI_DEMO_PREVIEW_H,
             AI_DEMO_PREVIEW_W, AI_DEMO_PREVIEW_H);
    rc = system(cmd);
    if(ui_shell_exit_code(rc) != 0 ||
       ai_demo_file_size(AI_DEMO_PREVIEW_RAW) != AI_DEMO_PREVIEW_BYTES) {
        ai_demo_append_log("preview: convert failed");
        return 0;
    }

    ai_demo_append_log("preview: ready");
    return 1;
}

static int ai_demo_load_preview_pixels(void)
{
    FILE *fp;
    size_t bytes_read;

    if(ai_demo_file_size(AI_DEMO_PREVIEW_RAW) != AI_DEMO_PREVIEW_BYTES) {
        return 0;
    }

    if(!ai_demo_preview_pixels) {
        ai_demo_preview_pixels = malloc(AI_DEMO_PREVIEW_BYTES);
        if(!ai_demo_preview_pixels) {
            ai_demo_append_log("preview: alloc failed");
            return 0;
        }
    }

    fp = fopen(AI_DEMO_PREVIEW_RAW, "rb");
    if(!fp) {
        return 0;
    }
    bytes_read = fread(ai_demo_preview_pixels, 1, AI_DEMO_PREVIEW_BYTES, fp);
    fclose(fp);
    if(bytes_read != AI_DEMO_PREVIEW_BYTES) {
        return 0;
    }

    memset(&ai_demo_preview_dsc, 0, sizeof(ai_demo_preview_dsc));
    ai_demo_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ai_demo_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ai_demo_preview_dsc.header.w = AI_DEMO_PREVIEW_W;
    ai_demo_preview_dsc.header.h = AI_DEMO_PREVIEW_H;
    ai_demo_preview_dsc.header.stride = AI_DEMO_PREVIEW_W * 2;
    ai_demo_preview_dsc.data_size = AI_DEMO_PREVIEW_BYTES;
    ai_demo_preview_dsc.data = ai_demo_preview_pixels;
    return 1;
}

static void ai_demo_update_preview(void)
{
    const char *path = NULL;
    time_t mtime = 0;

    if(ui_path_exists("/root/app/face_detect/face_detection_result.jpg")) {
        path = "/root/app/face_detect/face_detection_result.jpg";
        mtime = ai_demo_file_mtime(path);
    }

    if(!ai_demo_preview_image || !ai_demo_preview_placeholder) {
        return;
    }

    if(!path || !mtime) {
        lv_image_set_src(ai_demo_preview_image, NULL);
        lv_obj_add_flag(ai_demo_preview_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ai_demo_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
        ai_demo_preview_source[0] = '\0';
        ai_demo_preview_mtime = 0;
        return;
    }

    if(strcmp(ai_demo_preview_source, path) != 0 ||
       ai_demo_preview_mtime != mtime) {
        snprintf(ai_demo_preview_source, sizeof(ai_demo_preview_source), "%s",
                 path);
        ai_demo_preview_mtime = mtime;
        if(!ai_demo_convert_preview(path)) {
            lv_image_set_src(ai_demo_preview_image, NULL);
            lv_obj_add_flag(ai_demo_preview_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ai_demo_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    if(ai_demo_load_preview_pixels()) {
        lv_image_cache_drop(&ai_demo_preview_dsc);
        lv_image_set_src(ai_demo_preview_image, &ai_demo_preview_dsc);
        lv_obj_clear_flag(ai_demo_preview_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ai_demo_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static int ai_demo_spawn(const ai_demo_t *demo, pid_t *out_pid)
{
    pid_t pid;

    ai_demo_reset_log(demo);
    pid = fork();
    if(pid < 0) {
        return -1;
    }

    if(pid == 0) {
        int null_fd;
        int log_fd;

        setsid();

        null_fd = open("/dev/null", O_RDWR);
        if(null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if(null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }

        log_fd = open(AI_DEMO_LOG, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if(log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            if(log_fd > STDERR_FILENO) {
                close(log_fd);
            }
        }

        if(chdir(demo->workdir) != 0) {
            perror("chdir");
            _exit(126);
        }

        execl("/bin/sh", "sh", "-c", demo->command, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    *out_pid = pid;
    return 0;
}

static void ai_demo_finish_locked(int status)
{
    const ai_demo_t *demo = NULL;
    int code = -1;

    if(ai_demo_active_index >= 0 &&
       ai_demo_active_index < (int)ai_demo_count()) {
        demo = &ai_demos[ai_demo_active_index];
    }

    if(WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if(WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    }

    if(ai_demo_stop_requested) {
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
                 "Stopped");
    } else if(code == 0) {
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
                 "Completed");
    } else {
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text),
                 "Failed rc=%d", code);
    }

    if(demo && demo->result_path) {
        if(ui_path_exists(demo->result_path)) {
            snprintf(ai_demo_result_text, sizeof(ai_demo_result_text),
                     "Result: %s", demo->result_path);
        } else {
            snprintf(ai_demo_result_text, sizeof(ai_demo_result_text),
                     "Result missing: %s", demo->result_path);
        }
    } else if(demo && demo->long_running && ai_demo_stop_requested) {
        snprintf(ai_demo_result_text, sizeof(ai_demo_result_text), "%s",
                 "Camera demo stopped");
    }

    ai_demo_pid = -1;
    ai_demo_active_index = -1;
    ai_demo_stop_requested = 0;
    ai_demo_stop_request_us = 0;
}

static void ai_demo_poll_process(void)
{
    pid_t pid;
    int status = 0;

    pthread_mutex_lock(&ai_demo_lock);
    pid = ai_demo_pid;
    if(pid <= 0) {
        pthread_mutex_unlock(&ai_demo_lock);
        return;
    }

    if(ai_demo_stop_requested &&
       ai_demo_stop_request_us &&
       ui_monotonic_us() - ai_demo_stop_request_us > 2000000ULL) {
        kill(-pid, SIGKILL);
        ai_demo_stop_request_us = 0;
        ai_demo_append_log("Forced SIGKILL after stop timeout");
    }
    pthread_mutex_unlock(&ai_demo_lock);

    pid = waitpid(pid, &status, WNOHANG);
    if(pid > 0) {
        pthread_mutex_lock(&ai_demo_lock);
        ai_demo_finish_locked(status);
        pthread_mutex_unlock(&ai_demo_lock);
        app_request_fast_refresh();
    }
}

static void ai_demo_read_log_tail(char *buf, size_t len)
{
    FILE *fp;
    long size;
    long start;
    size_t got;

    if(!buf || len == 0) {
        return;
    }
    buf[0] = '\0';

    fp = fopen(AI_DEMO_LOG, "r");
    if(!fp) {
        snprintf(buf, len, "%s", ui_tr("No log yet"));
        return;
    }

    if(fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        snprintf(buf, len, "%s", ui_tr("Log unavailable"));
        return;
    }

    size = ftell(fp);
    if(size < 0) {
        fclose(fp);
        snprintf(buf, len, "%s", ui_tr("Log unavailable"));
        return;
    }

    start = size > (long)(len - 1U) ? size - (long)(len - 1U) : 0;
    if(fseek(fp, start, SEEK_SET) != 0) {
        fclose(fp);
        snprintf(buf, len, "%s", ui_tr("Log unavailable"));
        return;
    }

    got = fread(buf, 1, len - 1U, fp);
    buf[got] = '\0';
    fclose(fp);

    if(start > 0) {
        char *newline = strchr(buf, '\n');
        if(newline && newline[1]) {
            memmove(buf, newline + 1, strlen(newline + 1) + 1U);
        }
    }

    if(!buf[0]) {
        snprintf(buf, len, "%s", ui_tr("No log yet"));
    }
}

static void ai_demo_update_ui(void)
{
    char status[sizeof(ai_demo_status_text)];
    char result[sizeof(ai_demo_result_text)];
    char active[128];
    char log_text[AI_DEMO_LOG_TEXT_MAX];
    pid_t pid;
    int active_index;

    pthread_mutex_lock(&ai_demo_lock);
    snprintf(status, sizeof(status), "%s", ai_demo_status_text);
    snprintf(result, sizeof(result), "%s", ai_demo_result_text);
    pid = ai_demo_pid;
    active_index = ai_demo_active_index;
    if(active_index >= 0 && active_index < (int)ai_demo_count()) {
        snprintf(active, sizeof(active), "%s pid=%ld",
                 ai_demos[active_index].title, (long)pid);
    } else {
        snprintf(active, sizeof(active), "%s", "Idle");
    }
    pthread_mutex_unlock(&ai_demo_lock);

    if(ai_demo_status_label) {
        lv_label_set_text(ai_demo_status_label, ui_tr(status));
        lv_obj_set_style_text_color(ai_demo_status_label,
                                    lv_color_hex(pid > 0 ? 0xF5A524 :
                                                 (strncmp(status, "Failed", 6) == 0 ?
                                                  0xEF4D5A : 0x9AA4AF)),
                                    0);
    }

    if(ai_demo_active_label) {
        lv_label_set_text(ai_demo_active_label, active);
    }

    if(ai_demo_result_label) {
        lv_label_set_text(ai_demo_result_label, result);
    }

    ai_demo_update_preview();

    for(size_t i = 0; i < ai_demo_count(); i++) {
        if(!ai_demo_buttons[i]) {
            continue;
        }
        if(pid > 0 || !ai_demo_available(&ai_demos[i])) {
            lv_obj_add_state(ai_demo_buttons[i], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ai_demo_buttons[i], LV_STATE_DISABLED);
        }
    }

    if(ai_demo_stop_btn) {
        if(pid > 0) {
            lv_obj_clear_state(ai_demo_stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ai_demo_stop_btn, LV_STATE_DISABLED);
        }
    }

    if(ai_demo_log_label) {
        ai_demo_read_log_tail(log_text, sizeof(log_text));
        lv_label_set_text(ai_demo_log_label, log_text);
    }
}

static void ai_demo_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ai_demo_poll_process();
    ai_demo_update_ui();

    if(!ai_demo_status_label && ai_demo_pid <= 0 && ai_demo_timer) {
        lv_timer_delete(ai_demo_timer);
        ai_demo_timer = NULL;
    }
}

static void ai_demo_start_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    const ai_demo_t *demo;
    pid_t pid;

    if(index < 0 || index >= (int)ai_demo_count()) {
        return;
    }

    demo = &ai_demos[index];
    if(!ai_demo_available(demo)) {
        pthread_mutex_lock(&ai_demo_lock);
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
                 "Demo files missing");
        pthread_mutex_unlock(&ai_demo_lock);
        ai_demo_update_ui();
        return;
    }

    pthread_mutex_lock(&ai_demo_lock);
    if(ai_demo_pid > 0) {
        pthread_mutex_unlock(&ai_demo_lock);
        return;
    }
    pthread_mutex_unlock(&ai_demo_lock);

    if(ai_demo_spawn(demo, &pid) != 0) {
        pthread_mutex_lock(&ai_demo_lock);
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text),
                 "Start failed: %s", strerror(errno));
        pthread_mutex_unlock(&ai_demo_lock);
        ai_demo_update_ui();
        return;
    }

    pthread_mutex_lock(&ai_demo_lock);
    ai_demo_pid = pid;
    ai_demo_active_index = index;
    ai_demo_stop_requested = 0;
    ai_demo_stop_request_us = 0;
    snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
             demo->long_running ? "Running, tap Stop to exit" : "Running");
    snprintf(ai_demo_result_text, sizeof(ai_demo_result_text), "%s",
             demo->result_path ? "Waiting for result" : "Live demo active");
    pthread_mutex_unlock(&ai_demo_lock);

    if(!ai_demo_timer) {
        ai_demo_timer = lv_timer_create(ai_demo_timer_cb, 600, NULL);
    }
    ai_demo_update_ui();
    app_request_fast_refresh();
}

static void ai_demo_stop_event_cb(lv_event_t *event)
{
    pid_t pid;

    (void)event;

    pthread_mutex_lock(&ai_demo_lock);
    pid = ai_demo_pid;
    if(pid > 0 && !ai_demo_stop_requested) {
        ai_demo_stop_requested = 1;
        ai_demo_stop_request_us = ui_monotonic_us();
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
                 "Stopping");
        kill(-pid, SIGTERM);
        ai_demo_append_log("Sent SIGTERM");
    }
    pthread_mutex_unlock(&ai_demo_lock);

    ai_demo_update_ui();
    app_request_fast_refresh();
}

static void ai_demo_refresh_log_event_cb(lv_event_t *event)
{
    (void)event;
    ai_demo_update_ui();
    app_request_fast_refresh();
}

static lv_obj_t *ai_demo_card(lv_obj_t *parent, int y, int index)
{
    const ai_demo_t *demo = &ai_demos[index];
    int available = ai_demo_available(demo);
    lv_obj_t *card;
    lv_obj_t *icon_box;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *state;
    lv_obj_t *btn;

    card = ui_panel(parent, 24, y, 520, 136);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121923), 0);

    icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, 56, 56);
    lv_obj_align(icon_box, LV_ALIGN_TOP_LEFT, 0, 2);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(demo->color), 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    icon = ui_label(icon_box, demo->long_running ? LV_SYMBOL_VIDEO : LV_SYMBOL_BARS,
                    &lv_font_montserrat_22, 0xFFFFFF);
    lv_obj_center(icon);
    ui_make_click_forwarder(icon);

    title = ui_label(card, demo->title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 0);
    subtitle = ui_label(card, demo->subtitle, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(subtitle, 300);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 72, 34);

    state = ui_label(card, available ? "Installed" : "Missing",
                     &lv_font_montserrat_16, available ? 0x25C281 : 0xEF4D5A);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 72, 72);

    btn = ui_command_button(card, 370, 28, 118,
                            demo->long_running ? "Start" : "Run",
                            demo->color);
    lv_obj_add_event_cb(btn, ai_demo_start_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    ai_demo_buttons[index] = btn;

    return card;
}

void ui_ai_demo_cleanup(void)
{
    pthread_mutex_lock(&ai_demo_lock);
    if(ai_demo_pid > 0 && ai_demo_active_index >= 0 &&
       ai_demos[ai_demo_active_index].long_running && !ai_demo_stop_requested) {
        ai_demo_stop_requested = 1;
        ai_demo_stop_request_us = ui_monotonic_us();
        snprintf(ai_demo_status_text, sizeof(ai_demo_status_text), "%s",
                 "Stopping");
        kill(-ai_demo_pid, SIGTERM);
        ai_demo_append_log("Sent SIGTERM on page cleanup");
    }
    pthread_mutex_unlock(&ai_demo_lock);

    ai_demo_status_label = NULL;
    ai_demo_active_label = NULL;
    ai_demo_result_label = NULL;
    ai_demo_log_label = NULL;
    ai_demo_preview_image = NULL;
    ai_demo_preview_placeholder = NULL;
    ai_demo_stop_btn = NULL;
    memset(ai_demo_buttons, 0, sizeof(ai_demo_buttons));

    if(ai_demo_timer && ai_demo_pid <= 0) {
        lv_timer_delete(ai_demo_timer);
        ai_demo_timer = NULL;
    }
}

void ui_ai_demo_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *btn;

    ui_create_header(scr, "AI Demo");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    panel = ui_panel(body, 24, 24, 520, 150);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x141F2E), 0);
    title = ui_label(panel, "K230 AI Hub", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    ai_demo_status_label = ui_label(panel, "Ready", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_align(ai_demo_status_label, LV_ALIGN_TOP_LEFT, 0, 42);
    ai_demo_active_label = ui_label(panel, "Idle", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(ai_demo_active_label, ui_inner_width());
    lv_label_set_long_mode(ai_demo_active_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ai_demo_active_label, LV_ALIGN_TOP_LEFT, 0, 76);
    ai_demo_result_label = ui_label(panel, "No result yet", &lv_font_montserrat_16,
                                    0x25C281);
    lv_obj_set_width(ai_demo_result_label, ui_inner_width());
    lv_label_set_long_mode(ai_demo_result_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ai_demo_result_label, LV_ALIGN_TOP_LEFT, 0, 108);

    ai_demo_card(body, 198, 0);
    ai_demo_card(body, 350, 1);
    ai_demo_card(body, 502, 2);

    btn = ui_command_button(body, 24, 662, 248, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(btn, ai_demo_stop_event_cb, LV_EVENT_CLICKED, NULL);
    ai_demo_stop_btn = btn;
    btn = ui_command_button(body, 296, 662, 248, "Refresh log", 0x3DA5FF);
    lv_obj_add_event_cb(btn, ai_demo_refresh_log_event_cb, LV_EVENT_CLICKED, NULL);

    panel = ui_panel(body, 24, 746, 520, 318);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1016), 0);
    title = ui_label(panel, "Result preview", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    ai_demo_preview_image = lv_image_create(panel);
    lv_obj_set_size(ai_demo_preview_image, AI_DEMO_PREVIEW_W, AI_DEMO_PREVIEW_H);
    lv_obj_align(ai_demo_preview_image, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_obj_add_flag(ai_demo_preview_image, LV_OBJ_FLAG_HIDDEN);

    ai_demo_preview_placeholder = ui_panel(panel, 0, 38, AI_DEMO_PREVIEW_W,
                                           AI_DEMO_PREVIEW_H);
    lv_obj_set_style_bg_color(ai_demo_preview_placeholder,
                              lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(ai_demo_preview_placeholder, 0, 0);
    lv_obj_t *preview_hint = ui_label(ai_demo_preview_placeholder,
                                      "Run Face image to preview result",
                                      &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_center(preview_hint);

    panel = ui_panel(body, 24, 1090, 520, 306);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1016), 0);
    title = ui_label(panel, "Log", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    ai_demo_log_label = ui_label(panel, "No log yet", &lv_font_montserrat_14,
                                 0x9AA4AF);
    lv_obj_set_width(ai_demo_log_label, ui_inner_width());
    lv_label_set_long_mode(ai_demo_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ai_demo_log_label, LV_ALIGN_TOP_LEFT, 0, 38);

    if(!ai_demo_timer) {
        ai_demo_timer = lv_timer_create(ai_demo_timer_cb, 600, NULL);
    }
    ai_demo_update_ui();
}
