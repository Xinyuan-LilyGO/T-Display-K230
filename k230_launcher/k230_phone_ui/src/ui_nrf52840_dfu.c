#include "ui_nrf52840_dfu.h"

#include "ui_common.h"
#include "ui_i18n.h"
#include "ui_meshtastic.h"
#include "ui_nrf52840_manager.h"

#include <dirent.h>
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

#define NRF_DFU_TOOL "/root/app/k230_phone_ui/k230_nrf52840_dfu"
#define NRF_DFU_DIR "/root/nrf52840/firmware"
#define NRF_DFU_PACKAGE NRF_DFU_DIR "/firmware.zip"
#define NRF_DFU_PORT "/dev/ttyS1"
#define NRF_DFU_LOG "/tmp/k230_nrf52840_dfu_ui.log"
#define NRF_DFU_MAX_FILES 16
#define NRF_DFU_LOG_TEXT_MAX 2048

typedef struct {
    char name[160];
    char path[256];
    off_t size;
} nrf_dfu_file_t;

typedef struct {
    char package_path[256];
} nrf_dfu_request_t;

static pthread_mutex_t nrf_dfu_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *nrf_dfu_timer;
static lv_obj_t *nrf_dfu_status_label;
static lv_obj_t *nrf_dfu_mode_label;
static lv_obj_t *nrf_dfu_file_label;
static lv_obj_t *nrf_dfu_current_label;
static lv_obj_t *nrf_dfu_target_label;
static lv_obj_t *nrf_dfu_list_panel;
static lv_obj_t *nrf_dfu_log_label;
static lv_obj_t *nrf_dfu_bar;
static lv_obj_t *nrf_dfu_bar_label;
static lv_obj_t *nrf_dfu_update_btn;
static lv_obj_t *nrf_dfu_select_btn;
static lv_obj_t *nrf_dfu_log_btn;
static lv_obj_t *nrf_dfu_mtp_btn;
static lv_obj_t *nrf_dfu_select_dialog;
static lv_obj_t *nrf_dfu_log_dialog;
static lv_obj_t *nrf_dfu_confirm_dialog;
static lv_obj_t *nrf_dfu_overlay;
static lv_obj_t *nrf_dfu_overlay_spinner;
static lv_obj_t *nrf_dfu_overlay_status;
static lv_obj_t *nrf_dfu_overlay_bar;
static lv_obj_t *nrf_dfu_overlay_percent;
static lv_obj_t *nrf_dfu_overlay_close_btn;

static nrf_dfu_file_t nrf_dfu_files[NRF_DFU_MAX_FILES];
static int nrf_dfu_file_count;
static int nrf_dfu_selected_index = -1;
static int nrf_dfu_running;
static int nrf_dfu_overlay_hold;
static int nrf_dfu_progress;
static int nrf_dfu_last_rc;
static char nrf_dfu_status_text[192] = "Ready";
static char nrf_dfu_mode_text[96] = "Idle";
static char nrf_dfu_current_version[96] = "--";
static char nrf_dfu_target_version[96] = "--";
static char nrf_dfu_log_text[NRF_DFU_LOG_TEXT_MAX] = "No log yet";
static int nrf_dfu_preflight_cache_valid;
static int nrf_dfu_preflight_cache_rc;
static char nrf_dfu_preflight_cache_message[192];
static char nrf_dfu_preflight_cache_version[96];
static int nrf_dfu_restore_meshtastic_on_leave;

static void nrf_dfu_update_ui(void);

static void nrf_dfu_screen_metrics(int *out_w, int *out_h)
{
    lv_obj_t *top = lv_layer_top();
    int w;
    int h;

    if(top) {
        lv_obj_update_layout(top);
        w = lv_obj_get_width(top);
        h = lv_obj_get_height(top);
    } else {
        w = 0;
        h = 0;
    }
    if(w <= 0) {
        w = ui_screen_width();
    }
    if(h <= 0) {
        h = ui_screen_height();
    }
    if(out_w) {
        *out_w = w;
    }
    if(out_h) {
        *out_h = h;
    }
}

static void nrf_dfu_set_screen_overlay(lv_obj_t *overlay, int w, int h)
{
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, w, h);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
}

static int nrf_dfu_ends_with(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if(!text || !suffix) {
        return 0;
    }
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int nrf_dfu_file_compare(const void *a, const void *b)
{
    const nrf_dfu_file_t *fa = (const nrf_dfu_file_t *)a;
    const nrf_dfu_file_t *fb = (const nrf_dfu_file_t *)b;
    return strcmp(fa->name, fb->name);
}

static void nrf_dfu_set_status_locked(const char *status, const char *mode,
                                      int progress, int rc)
{
    if(status) {
        snprintf(nrf_dfu_status_text, sizeof(nrf_dfu_status_text), "%s",
                 status);
    }
    if(mode) {
        snprintf(nrf_dfu_mode_text, sizeof(nrf_dfu_mode_text), "%s", mode);
    }
    if(progress >= 0) {
        if(progress > 100) {
            progress = 100;
        }
        nrf_dfu_progress = progress;
    }
    nrf_dfu_last_rc = rc;
}

static void nrf_dfu_append_log_locked(const char *line)
{
    FILE *fp;
    size_t old_len;
    size_t add_len;
    size_t keep_from;

    if(!line || !line[0]) {
        return;
    }

    fp = fopen(NRF_DFU_LOG, "a");
    if(fp) {
        fprintf(fp, "%s\n", line);
        fclose(fp);
    }

    if(strcmp(nrf_dfu_log_text, "No log yet") == 0) {
        nrf_dfu_log_text[0] = '\0';
    }

    old_len = strlen(nrf_dfu_log_text);
    add_len = strlen(line);
    if(old_len + add_len + 2 >= sizeof(nrf_dfu_log_text)) {
        keep_from = old_len > sizeof(nrf_dfu_log_text) / 2 ?
                    old_len - sizeof(nrf_dfu_log_text) / 2 : 0;
        memmove(nrf_dfu_log_text, nrf_dfu_log_text + keep_from,
                old_len - keep_from + 1U);
        old_len = strlen(nrf_dfu_log_text);
    }

    snprintf(nrf_dfu_log_text + old_len,
             sizeof(nrf_dfu_log_text) - old_len, "%s%s",
             old_len > 0 ? "\n" : "", line);
}

static void nrf_dfu_append_log(const char *line)
{
    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_append_log_locked(line);
    pthread_mutex_unlock(&nrf_dfu_lock);
}

static int nrf_dfu_version_char(int ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '+';
}

static int nrf_dfu_version_looks_valid(const char *text)
{
    size_t len;

    if(!text || !text[0]) {
        return 0;
    }
    len = strlen(text);
    if(len < 8U || len >= 96U) {
        return 0;
    }
    if(strncmp(text, "20", 2) == 0 && len >= 10U &&
       text[4] == '-' && text[7] == '-') {
        return 1;
    }
    return strstr(text, "uart") || strstr(text, "dfu") ||
           strstr(text, "k230") || strstr(text, "K230");
}

static int nrf_dfu_extract_at_version(const char *response,
                                      char *out, size_t out_len)
{
    const char *marker = "+VER:K230_NRF52840_AT,";
    const char *pos;
    size_t i = 0;

    if(!response || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    pos = strstr(response, marker);
    if(!pos) {
        return -1;
    }
    pos += strlen(marker);
    while(pos[i] && nrf_dfu_version_char((unsigned char)pos[i]) &&
          i + 1U < out_len) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return nrf_dfu_version_looks_valid(out) ? 0 : -1;
}

static int nrf_dfu_extract_plain_version(const char *response,
                                         char *out, size_t out_len)
{
    const char *p = response;

    if(!response || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    while(*p) {
        char line[128];
        size_t i = 0;

        while(*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            p++;
        }
        while(p[i] && p[i] != '\r' && p[i] != '\n' &&
              i + 1U < sizeof(line)) {
            line[i] = p[i];
            i++;
        }
        line[i] = '\0';
        ui_trim_text(line);
        if(nrf_dfu_version_looks_valid(line)) {
            snprintf(out, out_len, "%s", line);
            return 0;
        }
        p += i;
        while(*p && *p != '\r' && *p != '\n') {
            p++;
        }
    }
    return -1;
}

static void nrf_dfu_parse_line(const char *line)
{
    int current;
    int total;
    int percent;

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_append_log_locked(line);
    if(sscanf(line, "DFU data: %d/%d chunks %d%%",
              &current, &total, &percent) == 3) {
        nrf_dfu_set_status_locked("Writing firmware...", "Transferring",
                                  percent, 0);
    } else if(strstr(line, "DFU transfer complete")) {
        nrf_dfu_set_status_locked("Activating firmware...", "Activating",
                                  98, 0);
    } else if(strcmp(line, "Done") == 0) {
        nrf_dfu_set_status_locked("Update complete", "Done", 100, 0);
    } else if(strstr(line, "dfu error") || strstr(line, "failed") ||
              strstr(line, "missing")) {
        nrf_dfu_set_status_locked(line, "Failed", -1, -1);
    }
    pthread_mutex_unlock(&nrf_dfu_lock);
}

static int nrf_dfu_run_argv(char *const argv[], int parse_progress,
                            char *response, size_t response_len)
{
    int pipe_fd[2];
    pid_t pid;
    int status;
    char buf[256];
    char line[512];
    size_t line_len = 0;
    ssize_t n;

    (void)parse_progress;

    if(response && response_len > 0) {
        response[0] = '\0';
    }

    if(pipe(pipe_fd) != 0) {
        nrf_dfu_append_log("pipe failed");
        return -1;
    }

    pid = fork();
    if(pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        nrf_dfu_append_log("fork failed");
        return -1;
    }

    if(pid == 0) {
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execv(argv[0], argv);
        _exit(127);
    }

    close(pipe_fd[1]);
    while((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        ssize_t i;
        if(response && response_len > 1) {
            size_t used = strlen(response);
            size_t copy = (size_t)n;
            if(copy > response_len - used - 1U) {
                copy = response_len - used - 1U;
            }
            if(copy > 0) {
                memcpy(response + used, buf, copy);
                response[used + copy] = '\0';
            }
        }
        for(i = 0; i < n; i++) {
            char ch = buf[i];
            if(ch == '\r') {
                continue;
            }
            if(ch == '\n') {
                line[line_len] = '\0';
                ui_trim_text(line);
                if(line[0]) {
                    nrf_dfu_parse_line(line);
                }
                line_len = 0;
            } else if(line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }
    close(pipe_fd[0]);

    if(line_len > 0) {
        line[line_len] = '\0';
        ui_trim_text(line);
        if(line[0]) {
            nrf_dfu_parse_line(line);
        }
    }

    if(waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if(WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if(WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

static int nrf_dfu_preflight_cache_get(char *message, size_t message_len)
{
    int rc = 999;

    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_preflight_cache_valid) {
        rc = nrf_dfu_preflight_cache_rc;
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s",
                     nrf_dfu_preflight_cache_message);
        }
    }
    pthread_mutex_unlock(&nrf_dfu_lock);
    return rc;
}

static void nrf_dfu_preflight_cache_set(int rc, const char *message,
                                        const char *version)
{
    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_preflight_cache_valid = 1;
    nrf_dfu_preflight_cache_rc = rc;
    snprintf(nrf_dfu_preflight_cache_message,
             sizeof(nrf_dfu_preflight_cache_message), "%s",
             message ? message : "");
    snprintf(nrf_dfu_preflight_cache_version,
             sizeof(nrf_dfu_preflight_cache_version), "%s",
             version ? version : "");
    if(version && version[0]) {
        snprintf(nrf_dfu_current_version, sizeof(nrf_dfu_current_version),
                 "%s", version);
    }
    pthread_mutex_unlock(&nrf_dfu_lock);
}

int ui_nrf52840_dfu_preflight(char *message, size_t message_len)
{
    nrf52840_status_t manager_status;
    int cached_rc;
    int rc;

    if(message && message_len > 0U) {
        message[0] = '\0';
    }

    cached_rc = nrf_dfu_preflight_cache_get(message, message_len);
    if(cached_rc != 999) {
        return cached_rc;
    }

    if(access(NRF_DFU_TOOL, X_OK) != 0) {
        if(message && message_len > 0U) {
            snprintf(message, message_len, "%s", "DFU tool missing");
        }
        return -1;
    }

    rc = ui_nrf52840_prepare_dfu(message, message_len);
    if(ui_nrf52840_get_status(&manager_status) == 0) {
        if(manager_status.version[0]) {
            pthread_mutex_lock(&nrf_dfu_lock);
            snprintf(nrf_dfu_current_version, sizeof(nrf_dfu_current_version),
                     "%s", manager_status.version);
            pthread_mutex_unlock(&nrf_dfu_lock);
        }
        if(rc == 0 || rc == -2) {
            nrf_dfu_preflight_cache_set(rc, message ? message : "",
                                        manager_status.version);
        }
    }

    ui_meshtastic_startup();
    return rc;
}

static int nrf_dfu_query_package_version(const char *path,
                                         char *out, size_t out_len)
{
    char *argv[] = {
        (char *)NRF_DFU_TOOL,
        (char *)"--package-version",
        (char *)path,
        NULL
    };
    char response[512];
    int rc;

    rc = nrf_dfu_run_argv(argv, 0, response, sizeof(response));
    if(rc != 0) {
        return -1;
    }
    return nrf_dfu_extract_plain_version(response, out, out_len);
}

static int nrf_dfu_scan_files(void)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    mkdir("/root/nrf52840", 0755);
    mkdir(NRF_DFU_DIR, 0755);

    dir = opendir(NRF_DFU_DIR);
    if(!dir) {
        return -1;
    }

    while((entry = readdir(dir)) != NULL && count < NRF_DFU_MAX_FILES) {
        char path[256];
        struct stat st;

        if(entry->d_name[0] == '.') {
            continue;
        }
        if(!nrf_dfu_ends_with(entry->d_name, ".zip")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", NRF_DFU_DIR, entry->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        snprintf(nrf_dfu_files[count].name, sizeof(nrf_dfu_files[count].name),
                 "%s", entry->d_name);
        snprintf(nrf_dfu_files[count].path, sizeof(nrf_dfu_files[count].path),
                 "%s", path);
        nrf_dfu_files[count].size = st.st_size;
        count++;
    }
    closedir(dir);

    qsort(nrf_dfu_files, (size_t)count, sizeof(nrf_dfu_files[0]),
          nrf_dfu_file_compare);
    nrf_dfu_file_count = count;
    if(nrf_dfu_selected_index >= count) {
        nrf_dfu_selected_index = count > 0 ? 0 : -1;
    }
    if(nrf_dfu_selected_index < 0 && count > 0) {
        nrf_dfu_selected_index = 0;
    }
    return count;
}

static void nrf_dfu_refresh_file_label(void)
{
    char text[256];
    struct stat st;

    if(!nrf_dfu_file_label) {
        return;
    }
    if(stat(NRF_DFU_PACKAGE, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(text, sizeof(text), "firmware.zip  %ld KB",
                 (long)((st.st_size + 1023) / 1024));
    } else {
        snprintf(text, sizeof(text), "firmware.zip missing");
    }
    lv_label_set_text(nrf_dfu_file_label, text);
}

static void nrf_dfu_select_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    pthread_mutex_lock(&nrf_dfu_lock);
    if(!nrf_dfu_running && index >= 0 && index < nrf_dfu_file_count) {
        nrf_dfu_selected_index = index;
        nrf_dfu_set_status_locked("Firmware selected", "Ready", -1, 0);
    }
    pthread_mutex_unlock(&nrf_dfu_lock);
    nrf_dfu_refresh_file_label();
    nrf_dfu_update_ui();
    if(nrf_dfu_select_dialog && lv_obj_is_valid(nrf_dfu_select_dialog)) {
        lv_obj_del(nrf_dfu_select_dialog);
    }
}

static void nrf_dfu_rebuild_file_list(void)
{
    int i;

    if(!nrf_dfu_list_panel) {
        return;
    }

    lv_obj_clean(nrf_dfu_list_panel);
    if(nrf_dfu_file_count <= 0) {
        lv_obj_t *empty = ui_label(nrf_dfu_list_panel,
                                   "No .zip packages in firmware folder",
                                   &lv_font_montserrat_18, 0x9AA4AF);
        lv_obj_set_width(empty, lv_obj_get_width(nrf_dfu_list_panel) - 24);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(empty, 12, 18);
        return;
    }

    for(i = 0; i < nrf_dfu_file_count; i++) {
        char label[224];
        lv_obj_t *btn;
        lv_obj_t *name;
        lv_obj_t *size;
        int selected = i == nrf_dfu_selected_index;
        int btn_w = lv_obj_get_width(nrf_dfu_list_panel) - 24;

        btn = lv_obj_create(nrf_dfu_list_panel);
        lv_obj_set_pos(btn, 12, 12 + i * 72);
        lv_obj_set_size(btn, btn_w, 60);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn,
                                      lv_color_hex(selected ? 0x3DA5FF : 0x27313C),
                                      0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(btn, 6);
        lv_obj_set_style_bg_color(btn,
                                  lv_color_hex(selected ? 0x1E3A5F : 0x161C23),
                                  0);
        lv_obj_add_event_cb(btn, nrf_dfu_select_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        snprintf(label, sizeof(label), "%s", nrf_dfu_files[i].name);
        name = ui_label(btn, label, &lv_font_montserrat_18,
                        selected ? 0x7DD3FC : 0xF2F5F8);
        lv_obj_set_width(name, btn_w - 106);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 14, 10);
        ui_make_click_forwarder(name);

        snprintf(label, sizeof(label), "%ld KB",
                 (long)((nrf_dfu_files[i].size + 1023) / 1024));
        size = ui_label(btn, label, &lv_font_montserrat_14, 0x9AA4AF);
        lv_obj_align(size, LV_ALIGN_BOTTOM_LEFT, 14, -8);
        ui_make_click_forwarder(size);
    }
}

static void nrf_dfu_refresh_files(void)
{
    struct stat st;
    char version[96];

    mkdir("/root/nrf52840", 0755);
    mkdir(NRF_DFU_DIR, 0755);
    pthread_mutex_lock(&nrf_dfu_lock);
    if(stat(NRF_DFU_PACKAGE, &st) != 0 || !S_ISREG(st.st_mode)) {
        nrf_dfu_selected_index = -1;
        snprintf(nrf_dfu_target_version, sizeof(nrf_dfu_target_version), "--");
        nrf_dfu_set_status_locked("firmware.zip missing", "Waiting", 0, -1);
    } else {
        nrf_dfu_selected_index = 0;
        nrf_dfu_set_status_locked("Ready to update", "Ready", 0, 0);
    }
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(nrf_dfu_selected_index == 0 && access(NRF_DFU_TOOL, X_OK) == 0) {
        if(nrf_dfu_query_package_version(NRF_DFU_PACKAGE, version,
                                         sizeof(version)) == 0) {
            pthread_mutex_lock(&nrf_dfu_lock);
            snprintf(nrf_dfu_target_version, sizeof(nrf_dfu_target_version),
                     "%s", version);
            pthread_mutex_unlock(&nrf_dfu_lock);
        } else {
            pthread_mutex_lock(&nrf_dfu_lock);
            snprintf(nrf_dfu_target_version, sizeof(nrf_dfu_target_version),
                     "--");
            nrf_dfu_set_status_locked("Package version unavailable", "Error",
                                      -1, -1);
            pthread_mutex_unlock(&nrf_dfu_lock);
        }
    }
    nrf_dfu_refresh_file_label();
}

static void nrf_dfu_dialog_close_event_cb(lv_event_t *event)
{
    lv_obj_t *dialog = (lv_obj_t *)lv_event_get_user_data(event);

    if(dialog && lv_obj_is_valid(dialog)) {
        lv_obj_del(dialog);
    }
}

static void nrf_dfu_overlay_close_event_cb(lv_event_t *event)
{
    (void)event;

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_overlay_hold = 0;
    pthread_mutex_unlock(&nrf_dfu_lock);
    nrf_dfu_update_ui();
}

static void nrf_dfu_select_dialog_delete_cb(lv_event_t *event)
{
    (void)event;
    nrf_dfu_select_dialog = NULL;
    nrf_dfu_list_panel = NULL;
}

static void nrf_dfu_log_dialog_delete_cb(lv_event_t *event)
{
    (void)event;
    nrf_dfu_log_dialog = NULL;
    nrf_dfu_log_label = NULL;
}

static lv_obj_t *nrf_dfu_dialog_card(lv_obj_t **dialog_out,
                                     const char *title_text,
                                     int preferred_w,
                                     int preferred_h,
                                     lv_event_cb_t delete_cb)
{
    lv_obj_t *dialog;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *close_btn;
    lv_obj_t *close_label;
    int w;
    int h;
    int card_w = preferred_w;
    int card_h = preferred_h;

    nrf_dfu_screen_metrics(&w, &h);
    if(card_w > w - 48) {
        card_w = w - 48;
    }
    if(card_h > h - 48) {
        card_h = h - 48;
    }

    dialog = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dialog, delete_cb, LV_EVENT_DELETE, NULL);

    card = ui_panel(dialog, (w - card_w) / 2, (h - card_h) / 2,
                    card_w, card_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111820), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(card, title_text, &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_width(title, card_w - 112);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 20, 22);

    close_btn = lv_obj_create(card);
    lv_obj_set_size(close_btn, 42, 42);
    lv_obj_set_pos(close_btn, card_w - 58, 14);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x27313C), 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x18202A), 0);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close_btn, 8);
    lv_obj_add_event_cb(close_btn, nrf_dfu_dialog_close_event_cb,
                        LV_EVENT_CLICKED, dialog);
    close_label = ui_label(close_btn, "X", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_center(close_label);
    ui_make_click_forwarder(close_label);

    if(dialog_out) {
        *dialog_out = dialog;
    }
    return card;
}

static void nrf_dfu_select_open_event_cb(lv_event_t *event)
{
    lv_obj_t *card;
    lv_obj_t *hint;
    int card_w = ui_is_landscape() ? 660 : 500;
    int card_h = ui_is_landscape() ? 430 : 620;

    (void)event;
    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_running) {
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(nrf_dfu_select_dialog && lv_obj_is_valid(nrf_dfu_select_dialog)) {
        lv_obj_del(nrf_dfu_select_dialog);
    }

    card = nrf_dfu_dialog_card(&nrf_dfu_select_dialog, "Select firmware",
                               card_w, card_h,
                               nrf_dfu_select_dialog_delete_cb);
    hint = ui_label(card, NRF_DFU_DIR, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(hint, lv_obj_get_width(card) - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint, 20, 62);

    nrf_dfu_list_panel = ui_scroll_panel(card, 0, 92,
                                         lv_obj_get_width(card),
                                         lv_obj_get_height(card) - 104);
    lv_obj_set_style_bg_opa(nrf_dfu_list_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nrf_dfu_list_panel, 0, 0);
    nrf_dfu_refresh_files();
}

static void nrf_dfu_log_open_event_cb(lv_event_t *event)
{
    lv_obj_t *card;
    lv_obj_t *path;
    int card_w = ui_is_landscape() ? 720 : 500;
    int card_h = ui_is_landscape() ? 430 : 620;

    (void)event;
    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_running) {
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(nrf_dfu_log_dialog && lv_obj_is_valid(nrf_dfu_log_dialog)) {
        lv_obj_del(nrf_dfu_log_dialog);
    }

    card = nrf_dfu_dialog_card(&nrf_dfu_log_dialog, "Update log",
                               card_w, card_h, nrf_dfu_log_dialog_delete_cb);
    path = ui_label(card, NRF_DFU_LOG, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(path, lv_obj_get_width(card) - 40);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(path, 20, 62);

    nrf_dfu_log_label = ui_label(card, "No log yet",
                                 &lv_font_montserrat_14, 0xD7DEE8);
    lv_obj_set_width(nrf_dfu_log_label, lv_obj_get_width(card) - 40);
    lv_label_set_long_mode(nrf_dfu_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(nrf_dfu_log_label, 20, 96);
    nrf_dfu_update_ui();
}

static void nrf_dfu_open_mtp_event_cb(lv_event_t *event)
{
    (void)event;
    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_running) {
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    pthread_mutex_unlock(&nrf_dfu_lock);
    app_nav_to_page(PAGE_FILES);
}

static void *nrf_dfu_worker(void *arg)
{
    nrf_dfu_request_t *req = (nrf_dfu_request_t *)arg;
    char *probe_argv[] = {
        (char *)NRF_DFU_TOOL,
        (char *)"--at", (char *)"AT+VER?",
        (char *)"--at-read-ms", (char *)"1200",
        (char *)"-p", (char *)NRF_DFU_PORT,
        NULL
    };
    char *dfu_argv_trigger[] = {
        (char *)NRF_DFU_TOOL,
        (char *)"--baud", (char *)"115200",
        (char *)"--trigger-baud", (char *)"115200",
        (char *)"--trigger-delay-ms", (char *)"50",
        (char *)"--post-start-delay-ms", (char *)"0",
        (char *)"--post-init-delay-ms", (char *)"0",
        (char *)"--ack-timeout-ms", (char *)"4000",
        (char *)"--retries", (char *)"6",
        (char *)"--chunk-size", (char *)"512",
        (char *)"--throttle-every", (char *)"0",
        (char *)"--throttle-ms", (char *)"0",
        (char *)"-p", (char *)NRF_DFU_PORT,
        req->package_path,
        NULL
    };
    char *verify_argv[] = {
        (char *)NRF_DFU_TOOL,
        (char *)"--at", (char *)"AT+VER?",
        (char *)"--at-read-ms", (char *)"1800",
        (char *)"-p", (char *)NRF_DFU_PORT,
        NULL
    };
    char response[512];
    char current_version[96];
    char target_version[96];
    char msg[224];
    int probe_rc;
    int rc;
    int probe_attempt;
    int probed_version;

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_set_status_locked("Preparing update...", "Preparing", 0, 0);
    snprintf(nrf_dfu_log_text, sizeof(nrf_dfu_log_text),
             "Package: %s", req->package_path);
    pthread_mutex_unlock(&nrf_dfu_lock);
    unlink(NRF_DFU_LOG);
    nrf_dfu_append_log("nRF52840 DFU started");

    if(access(NRF_DFU_TOOL, X_OK) != 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked("DFU tool missing", "Failed", 0, -1);
        nrf_dfu_append_log_locked(NRF_DFU_TOOL);
        nrf_dfu_running = 0;
        pthread_mutex_unlock(&nrf_dfu_lock);
        free(req);
        app_request_fast_refresh();
        return NULL;
    }

    if(access(req->package_path, R_OK) != 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked("firmware.zip missing", "Failed", 0, -1);
        nrf_dfu_running = 0;
        pthread_mutex_unlock(&nrf_dfu_lock);
        free(req);
        app_request_fast_refresh();
        return NULL;
    }

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_set_status_locked("Checking package version...", "Preparing", 1, 0);
    pthread_mutex_unlock(&nrf_dfu_lock);
    if(nrf_dfu_query_package_version(req->package_path, target_version,
                                     sizeof(target_version)) != 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        snprintf(nrf_dfu_target_version, sizeof(nrf_dfu_target_version), "--");
        nrf_dfu_set_status_locked("Package version unavailable", "Failed", 0,
                                  -1);
        nrf_dfu_running = 0;
        pthread_mutex_unlock(&nrf_dfu_lock);
        free(req);
        app_request_fast_refresh();
        return NULL;
    }
    pthread_mutex_lock(&nrf_dfu_lock);
    snprintf(nrf_dfu_target_version, sizeof(nrf_dfu_target_version), "%s",
             target_version);
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(ui_nrf52840_begin_dfu(3000, msg, sizeof(msg)) != 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked(msg[0] ? msg : "nRF52840 is busy",
                                  "Failed", 0, -1);
        nrf_dfu_overlay_hold = 1;
        nrf_dfu_running = 0;
        pthread_mutex_unlock(&nrf_dfu_lock);
        free(req);
        app_request_fast_refresh();
        return NULL;
    }
    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_restore_meshtastic_on_leave = 1;
    pthread_mutex_unlock(&nrf_dfu_lock);
    usleep(300000);

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_set_status_locked("Checking nRF52840...", "Probing", 2, 0);
    pthread_mutex_unlock(&nrf_dfu_lock);
    probe_rc = -1;
    current_version[0] = '\0';
    probed_version = 0;
    for(probe_attempt = 1; probe_attempt <= 4; ++probe_attempt) {
        response[0] = '\0';
        probe_rc = nrf_dfu_run_argv(probe_argv, 0, response, sizeof(response));
        if(probe_rc == 0 && strstr(response, "OK") != NULL &&
           nrf_dfu_extract_at_version(response, current_version,
                                      sizeof(current_version)) == 0) {
            probed_version = 1;
            break;
        }
        snprintf(msg, sizeof(msg), "AT version probe retry %d/4: %s",
                 probe_attempt, response[0] ? response : "no response");
        nrf_dfu_append_log(msg);
        usleep(300000);
    }
    if(probed_version) {
        pthread_mutex_lock(&nrf_dfu_lock);
        snprintf(nrf_dfu_current_version, sizeof(nrf_dfu_current_version),
                 "%s", current_version);
        pthread_mutex_unlock(&nrf_dfu_lock);
        if(strcmp(current_version, target_version) == 0) {
            snprintf(msg, sizeof(msg), "Firmware already up to date: %s",
                     current_version);
            pthread_mutex_lock(&nrf_dfu_lock);
            nrf_dfu_set_status_locked("Firmware already up to date", "Ready",
                                      100, 0);
            nrf_dfu_append_log_locked(msg);
            nrf_dfu_overlay_hold = 1;
            nrf_dfu_running = 0;
            pthread_mutex_unlock(&nrf_dfu_lock);
            ui_nrf52840_note_dfu_finished(0);
            free(req);
            app_request_fast_refresh();
            return NULL;
        }
    } else {
        pthread_mutex_lock(&nrf_dfu_lock);
        snprintf(nrf_dfu_current_version, sizeof(nrf_dfu_current_version), "--");
        nrf_dfu_set_status_locked("nRF52840 AT response unavailable", "Failed",
                                  0, -1);
        nrf_dfu_overlay_hold = 1;
        nrf_dfu_running = 0;
        pthread_mutex_unlock(&nrf_dfu_lock);
        ui_nrf52840_note_dfu_finished(-1);
        free(req);
        app_request_fast_refresh();
        return NULL;
    }

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_set_status_locked("AT app detected, entering DFU",
                              "AT+DFU", 4, 0);
    pthread_mutex_unlock(&nrf_dfu_lock);

    rc = nrf_dfu_run_argv(dfu_argv_trigger, 1, NULL, 0);

    if(rc == 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked("Verifying new firmware...", "Verify", 100, 0);
        pthread_mutex_unlock(&nrf_dfu_lock);
        sleep(2);
        if(nrf_dfu_run_argv(verify_argv, 0, response, sizeof(response)) == 0 &&
           strstr(response, "OK") &&
           nrf_dfu_extract_at_version(response, current_version,
                                      sizeof(current_version)) == 0) {
            pthread_mutex_lock(&nrf_dfu_lock);
            snprintf(nrf_dfu_current_version, sizeof(nrf_dfu_current_version),
                     "%s", current_version);
            nrf_dfu_set_status_locked("Update successful", "Ready", 100, 0);
            nrf_dfu_overlay_hold = 1;
            pthread_mutex_unlock(&nrf_dfu_lock);
        } else {
            pthread_mutex_lock(&nrf_dfu_lock);
            nrf_dfu_set_status_locked("Update written, verify later", "Ready",
                                      100, 0);
            nrf_dfu_overlay_hold = 1;
            pthread_mutex_unlock(&nrf_dfu_lock);
        }
    } else {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked("Update failed, check log", "Failed", -1, rc);
        nrf_dfu_overlay_hold = 1;
        pthread_mutex_unlock(&nrf_dfu_lock);
    }

    pthread_mutex_lock(&nrf_dfu_lock);
    nrf_dfu_running = 0;
    nrf_dfu_last_rc = rc;
    pthread_mutex_unlock(&nrf_dfu_lock);
    ui_nrf52840_note_dfu_finished(rc);

    free(req);
    app_request_fast_refresh();
    return NULL;
}

static void nrf_dfu_start_confirmed(void)
{
    pthread_t thread;
    nrf_dfu_request_t *req;

    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_running) {
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    if(access(NRF_DFU_PACKAGE, R_OK) != 0) {
        nrf_dfu_set_status_locked("firmware.zip missing", "Waiting", -1, -1);
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    nrf_dfu_running = 1;
    nrf_dfu_overlay_hold = 0;
    nrf_dfu_progress = 0;
    nrf_dfu_last_rc = 0;
    nrf_dfu_set_status_locked("Checking nRF52840...", "Probing", 0, 0);
    pthread_mutex_unlock(&nrf_dfu_lock);
    nrf_dfu_update_ui();
    app_request_fast_refresh();

    req = (nrf_dfu_request_t *)calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_running = 0;
        nrf_dfu_set_status_locked("Out of memory", "Failed", -1, -1);
        pthread_mutex_unlock(&nrf_dfu_lock);
        nrf_dfu_update_ui();
        return;
    }
    snprintf(req->package_path, sizeof(req->package_path), "%s",
             NRF_DFU_PACKAGE);

    if(pthread_create(&thread, NULL, nrf_dfu_worker, req) == 0) {
        pthread_detach(thread);
    } else {
        free(req);
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_running = 0;
        nrf_dfu_set_status_locked("Thread failed", "Failed", -1, -1);
        pthread_mutex_unlock(&nrf_dfu_lock);
    }
}

static void nrf_dfu_confirm_cancel_event_cb(lv_event_t *event)
{
    (void)event;
    if(nrf_dfu_confirm_dialog && lv_obj_is_valid(nrf_dfu_confirm_dialog)) {
        lv_obj_delete(nrf_dfu_confirm_dialog);
    }
}

static void nrf_dfu_confirm_update_event_cb(lv_event_t *event)
{
    (void)event;
    if(nrf_dfu_confirm_dialog && lv_obj_is_valid(nrf_dfu_confirm_dialog)) {
        lv_obj_delete(nrf_dfu_confirm_dialog);
    }
    nrf_dfu_start_confirmed();
}

static void nrf_dfu_confirm_delete_cb(lv_event_t *event)
{
    (void)event;
    nrf_dfu_confirm_dialog = NULL;
}

static void nrf_dfu_start_event_cb(lv_event_t *event)
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *text;
    lv_obj_t *btn;
    int w;
    int h;
    int card_w = ui_is_landscape() ? 520 : 456;
    int card_h = 250;
    int button_w = 180;

    (void)event;
    pthread_mutex_lock(&nrf_dfu_lock);
    if(nrf_dfu_running) {
        pthread_mutex_unlock(&nrf_dfu_lock);
        return;
    }
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(access(NRF_DFU_PACKAGE, R_OK) != 0) {
        pthread_mutex_lock(&nrf_dfu_lock);
        nrf_dfu_set_status_locked("firmware.zip missing", "Waiting", -1, -1);
        pthread_mutex_unlock(&nrf_dfu_lock);
        nrf_dfu_update_ui();
        return;
    }

    nrf_dfu_screen_metrics(&w, &h);
    if(card_w > w - 48) {
        card_w = w - 48;
    }
    if(nrf_dfu_confirm_dialog && lv_obj_is_valid(nrf_dfu_confirm_dialog)) {
        lv_obj_delete(nrf_dfu_confirm_dialog);
    }
    nrf_dfu_confirm_dialog = lv_obj_create(lv_layer_top());
    nrf_dfu_set_screen_overlay(nrf_dfu_confirm_dialog, w, h);
    lv_obj_add_event_cb(nrf_dfu_confirm_dialog, nrf_dfu_confirm_delete_cb,
                        LV_EVENT_DELETE, NULL);

    card = lv_obj_create(nrf_dfu_confirm_dialog);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121820), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x27313C), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(card, "Confirm update", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_width(title, card_w - 48);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 24, 24);

    text = ui_label(card, "Update nRF52840 firmware now?",
                    &lv_font_montserrat_18, 0xCBD5E1);
    lv_obj_set_width(text, card_w - 48);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(text, 24, 78);

    if(button_w * 2 + 18 > card_w - 48) {
        button_w = (card_w - 66) / 2;
    }
    btn = ui_command_button(card, card_w - 24 - button_w * 2 - 18,
                            card_h - 76, button_w, "Cancel", 0x374151);
    lv_obj_add_event_cb(btn, nrf_dfu_confirm_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(card, card_w - 24 - button_w, card_h - 76,
                            button_w, "Update", 0x25C281);
    lv_obj_add_event_cb(btn, nrf_dfu_confirm_update_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void nrf_dfu_update_ui(void)
{
    int running;
    int hold;
    int progress;
    int rc;
    char status[192];
    char mode[96];
    char current_version[96];
    char target_version[96];
    char log_text[NRF_DFU_LOG_TEXT_MAX];

    pthread_mutex_lock(&nrf_dfu_lock);
    running = nrf_dfu_running;
    hold = nrf_dfu_overlay_hold;
    progress = nrf_dfu_progress;
    rc = nrf_dfu_last_rc;
    snprintf(status, sizeof(status), "%s", nrf_dfu_status_text);
    snprintf(mode, sizeof(mode), "%s", nrf_dfu_mode_text);
    snprintf(current_version, sizeof(current_version), "%s",
             nrf_dfu_current_version);
    snprintf(target_version, sizeof(target_version), "%s",
             nrf_dfu_target_version);
    snprintf(log_text, sizeof(log_text), "%s", nrf_dfu_log_text);
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(nrf_dfu_status_label) {
        lv_label_set_text(nrf_dfu_status_label, ui_tr(status));
        lv_obj_set_style_text_color(nrf_dfu_status_label,
                                    lv_color_hex(rc == 0 ? 0x25C281 : 0xF5A524),
                                    0);
    }
    if(nrf_dfu_mode_label) {
        lv_label_set_text(nrf_dfu_mode_label, ui_tr(mode));
    }
    if(nrf_dfu_current_label) {
        lv_label_set_text(nrf_dfu_current_label, current_version);
    }
    if(nrf_dfu_target_label) {
        lv_label_set_text(nrf_dfu_target_label, target_version);
    }
    if(nrf_dfu_bar) {
        if(running) {
            lv_obj_clear_flag(nrf_dfu_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(nrf_dfu_bar, progress, LV_ANIM_ON);
        } else {
            lv_obj_add_flag(nrf_dfu_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(nrf_dfu_bar_label) {
        if(running) {
            char percent[32];
            lv_obj_clear_flag(nrf_dfu_bar_label, LV_OBJ_FLAG_HIDDEN);
            snprintf(percent, sizeof(percent), "%d%%", progress);
            lv_label_set_text(nrf_dfu_bar_label, percent);
        } else {
            lv_obj_add_flag(nrf_dfu_bar_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(nrf_dfu_log_label) {
        lv_label_set_text(nrf_dfu_log_label, log_text);
    }
    if(nrf_dfu_update_btn) {
        if(running || access(NRF_DFU_PACKAGE, R_OK) != 0) {
            lv_obj_add_state(nrf_dfu_update_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(nrf_dfu_update_btn, LV_STATE_DISABLED);
        }
    }
    if(nrf_dfu_select_btn) {
        if(running) {
            lv_obj_add_state(nrf_dfu_select_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(nrf_dfu_select_btn, LV_STATE_DISABLED);
        }
    }
    if(nrf_dfu_log_btn) {
        if(running) {
            lv_obj_add_state(nrf_dfu_log_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(nrf_dfu_log_btn, LV_STATE_DISABLED);
        }
    }
    if(nrf_dfu_mtp_btn) {
        if(running) {
            lv_obj_add_state(nrf_dfu_mtp_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(nrf_dfu_mtp_btn, LV_STATE_DISABLED);
        }
    }

    if(nrf_dfu_overlay) {
        if(running || hold) {
            lv_obj_clear_flag(nrf_dfu_overlay, LV_OBJ_FLAG_HIDDEN);
            if(nrf_dfu_overlay_status) {
                lv_label_set_text(nrf_dfu_overlay_status, ui_tr(status));
            }
            if(nrf_dfu_overlay_spinner) {
                if(running && progress < 100) {
                    lv_obj_clear_flag(nrf_dfu_overlay_spinner,
                                      LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(nrf_dfu_overlay_spinner,
                                    LV_OBJ_FLAG_HIDDEN);
                }
            }
            if(nrf_dfu_overlay_bar) {
                lv_bar_set_value(nrf_dfu_overlay_bar, progress, LV_ANIM_ON);
            }
            if(nrf_dfu_overlay_percent) {
                char percent[48];
                snprintf(percent, sizeof(percent), "%d%%", progress);
                lv_label_set_text(nrf_dfu_overlay_percent, percent);
            }
            if(nrf_dfu_overlay_close_btn) {
                if(running) {
                    lv_obj_add_flag(nrf_dfu_overlay_close_btn,
                                    LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(nrf_dfu_overlay_close_btn,
                                      LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            lv_obj_add_flag(nrf_dfu_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void nrf_dfu_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    nrf_dfu_update_ui();
}

static lv_obj_t *nrf_dfu_info_pair(lv_obj_t *parent, int x, int y, int w,
                                   int label_w, const char *name,
                                   lv_obj_t **value_out)
{
    lv_obj_t *name_label = ui_label(parent, name, &lv_font_montserrat_16,
                                    0x9AA4AF);
    lv_obj_t *value = ui_label(parent, "--", &lv_font_montserrat_18,
                               0xF2F5F8);
    int gap = 16;
    int value_w;

    if(label_w < 112) {
        label_w = 112;
    }
    value_w = w - label_w - gap;
    if(value_w < 160) {
        value_w = 160;
    }
    lv_obj_set_pos(name_label, x, y);
    lv_obj_set_width(name_label, label_w);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, value_w);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(value, x + label_w + gap, y - 2);
    if(value_out) {
        *value_out = value;
    }
    return value;
}

static void nrf_dfu_create_overlay(lv_obj_t *scr)
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *label;
    int w;
    int h;
    int card_w = ui_is_landscape() ? 560 : 472;
    int card_h = 360;

    (void)scr;
    nrf_dfu_screen_metrics(&w, &h);
    if(card_w > w - 48) {
        card_w = w - 48;
    }

    if(nrf_dfu_overlay && lv_obj_is_valid(nrf_dfu_overlay)) {
        lv_obj_delete(nrf_dfu_overlay);
    }
    nrf_dfu_overlay = lv_obj_create(lv_layer_top());
    nrf_dfu_set_screen_overlay(nrf_dfu_overlay, w, h);
    lv_obj_add_flag(nrf_dfu_overlay, LV_OBJ_FLAG_HIDDEN);

    card = lv_obj_create(nrf_dfu_overlay);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121820), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x27313C), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(card, "Updating nRF52840", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    nrf_dfu_overlay_spinner = lv_spinner_create(card);
    lv_obj_set_size(nrf_dfu_overlay_spinner, 58, 58);
    lv_obj_align(nrf_dfu_overlay_spinner, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_arc_color(nrf_dfu_overlay_spinner,
                               lv_color_hex(0x263342), LV_PART_MAIN);
    lv_obj_set_style_arc_color(nrf_dfu_overlay_spinner,
                               lv_color_hex(0x3DA5FF), LV_PART_INDICATOR);

    nrf_dfu_overlay_status = ui_label(card, "Preparing update...",
                                      &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(nrf_dfu_overlay_status, card_w - 48);
    lv_label_set_long_mode(nrf_dfu_overlay_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(nrf_dfu_overlay_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nrf_dfu_overlay_status, LV_ALIGN_TOP_MID, 0, 154);

    nrf_dfu_overlay_bar = lv_bar_create(card);
    lv_obj_set_size(nrf_dfu_overlay_bar, card_w - 80, 22);
    lv_obj_align(nrf_dfu_overlay_bar, LV_ALIGN_TOP_MID, 0, 204);
    lv_bar_set_range(nrf_dfu_overlay_bar, 0, 100);
    lv_bar_set_value(nrf_dfu_overlay_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(nrf_dfu_overlay_bar, lv_color_hex(0x27313C),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(nrf_dfu_overlay_bar, lv_color_hex(0x3DA5FF),
                              LV_PART_INDICATOR);

    nrf_dfu_overlay_percent = ui_label(card, "0%", &lv_font_montserrat_20,
                                       0xF2F5F8);
    lv_obj_align(nrf_dfu_overlay_percent, LV_ALIGN_TOP_MID, 0, 238);

    hint = ui_label(card, "Do not touch the screen or power off the board.",
                    &lv_font_montserrat_16, 0xF5A524);
    lv_obj_set_width(hint, card_w - 48);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 280);

    nrf_dfu_overlay_close_btn = lv_obj_create(card);
    lv_obj_set_size(nrf_dfu_overlay_close_btn, 160, 46);
    lv_obj_align(nrf_dfu_overlay_close_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(nrf_dfu_overlay_close_btn, 8, 0);
    lv_obj_set_style_border_width(nrf_dfu_overlay_close_btn, 0, 0);
    lv_obj_set_style_bg_color(nrf_dfu_overlay_close_btn,
                              lv_color_hex(0x25C281), 0);
    lv_obj_set_style_bg_opa(nrf_dfu_overlay_close_btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(nrf_dfu_overlay_close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nrf_dfu_overlay_close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(nrf_dfu_overlay_close_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(nrf_dfu_overlay_close_btn,
                        nrf_dfu_overlay_close_event_cb, LV_EVENT_CLICKED, NULL);
    label = ui_label(nrf_dfu_overlay_close_btn, ui_tr("Close"),
                     &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_center(label);
    ui_make_click_forwarder(label);
}

void ui_nrf52840_dfu_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *icon_box;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *hint;
    int landscape = ui_is_landscape();
    int body_x = ui_page_panel_x();
    int body_w = ui_page_panel_width();
    int body_h = ui_body_height(154);
    int top_y = ui_page_top_y(154);
    int card_h = body_h - 24;
    int margin;
    int icon_size;
    int action_x;
    int action_w;
    int button_y;
    int content_x;
    int content_w;
    int info_x;
    int info_w;
    int info_label_w;
    int row_step;
    int y;

    ui_create_header(scr, "nRF52840 DFU");
    body = ui_scroll_panel(scr, body_x, top_y, body_w, body_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x121820), 0);

    if(card_h < (landscape ? 330 : 540)) {
        card_h = landscape ? 330 : 540;
    }

    margin = landscape ? 56 : 34;
    if(body_w - margin * 2 < 320) {
        margin = (body_w - 320) / 2;
        if(margin < 24) {
            margin = 24;
        }
    }
    icon_size = landscape ? 68 : 84;
    content_x = margin;
    content_w = body_w - margin * 2;
    info_x = content_x;
    info_w = content_w;
    info_label_w = landscape ? (content_w >= 680 ? 210 : 176) :
                   (content_w >= 430 ? 166 : 136);
    row_step = landscape ? 38 : 54;
    action_w = landscape ? 220 : content_w;
    if(action_w > content_w) {
        action_w = content_w;
    }
    action_x = (body_w - action_w) / 2;
    button_y = card_h - 76;

    icon_box = lv_obj_create(body);
    lv_obj_set_size(icon_box, icon_size, icon_size);
    lv_obj_set_pos(icon_box, content_x, 24);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x3B82F6), 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    icon = ui_label(icon_box, "DFU",
                    landscape ? &lv_font_montserrat_22 :
                    &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_center(icon);

    title = ui_label(body, "nRF52840 firmware update",
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_width(title, content_w - icon_size - 16);
    lv_label_set_long_mode(title, landscape ? LV_LABEL_LONG_DOT :
                           LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(title, content_x + icon_size + 16, 28);

    hint = ui_label(body,
                    "Copy firmware.zip to /root/nrf52840/firmware with MTP, then tap Update.",
                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, content_w);
    lv_label_set_long_mode(hint, landscape ? LV_LABEL_LONG_DOT :
                           LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, content_x, landscape ? 104 : 128);

    y = landscape ? 144 : 212;
    nrf_dfu_info_pair(body, info_x, y, info_w, info_label_w,
                      "Package", &nrf_dfu_file_label);
    y += row_step;
    nrf_dfu_info_pair(body, info_x, y, info_w, info_label_w,
                      "Current version", &nrf_dfu_current_label);
    y += row_step;
    nrf_dfu_info_pair(body, info_x, y, info_w, info_label_w,
                      "Target version", &nrf_dfu_target_label);
    y += row_step;
    nrf_dfu_info_pair(body, info_x, y, info_w, info_label_w,
                      "Status", &nrf_dfu_status_label);
    y += row_step;
    nrf_dfu_info_pair(body, info_x, y, info_w, info_label_w,
                      "Mode", &nrf_dfu_mode_label);

    nrf_dfu_bar = lv_bar_create(body);
    lv_obj_set_pos(nrf_dfu_bar, action_x, button_y - 42);
    lv_obj_set_size(nrf_dfu_bar, action_w, landscape ? 16 : 18);
    lv_bar_set_range(nrf_dfu_bar, 0, 100);
    lv_bar_set_value(nrf_dfu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(nrf_dfu_bar, lv_color_hex(0x27313C),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(nrf_dfu_bar, lv_color_hex(0x25C281),
                              LV_PART_INDICATOR);
    lv_obj_add_flag(nrf_dfu_bar, LV_OBJ_FLAG_HIDDEN);
    nrf_dfu_bar_label = ui_label(body, "0%", &lv_font_montserrat_16,
                                 0xF2F5F8);
    lv_obj_align_to(nrf_dfu_bar_label, nrf_dfu_bar, LV_ALIGN_OUT_BOTTOM_RIGHT,
                    0, 6);
    lv_obj_add_flag(nrf_dfu_bar_label, LV_OBJ_FLAG_HIDDEN);

    nrf_dfu_update_btn = ui_command_button(body,
                                           action_x, button_y, action_w,
                                           "Update", 0x25C281);
    lv_obj_add_event_cb(nrf_dfu_update_btn, nrf_dfu_start_event_cb,
                        LV_EVENT_CLICKED, NULL);

    nrf_dfu_create_overlay(scr);
    nrf_dfu_refresh_files();
    nrf_dfu_timer = lv_timer_create(nrf_dfu_timer_cb, 200, NULL);
    nrf_dfu_update_ui();
}

void ui_nrf52840_dfu_cleanup(void)
{
    if(nrf_dfu_timer) {
        lv_timer_delete(nrf_dfu_timer);
        nrf_dfu_timer = NULL;
    }
    nrf_dfu_status_label = NULL;
    nrf_dfu_mode_label = NULL;
    nrf_dfu_file_label = NULL;
    nrf_dfu_current_label = NULL;
    nrf_dfu_target_label = NULL;
    nrf_dfu_list_panel = NULL;
    nrf_dfu_log_label = NULL;
    nrf_dfu_bar = NULL;
    nrf_dfu_bar_label = NULL;
    nrf_dfu_update_btn = NULL;
    nrf_dfu_select_btn = NULL;
    nrf_dfu_log_btn = NULL;
    nrf_dfu_mtp_btn = NULL;
    nrf_dfu_select_dialog = NULL;
    nrf_dfu_log_dialog = NULL;
    if(nrf_dfu_confirm_dialog && lv_obj_is_valid(nrf_dfu_confirm_dialog)) {
        lv_obj_delete(nrf_dfu_confirm_dialog);
    }
    nrf_dfu_confirm_dialog = NULL;
    if(nrf_dfu_overlay && lv_obj_is_valid(nrf_dfu_overlay)) {
        lv_obj_delete(nrf_dfu_overlay);
    }
    nrf_dfu_overlay = NULL;
    nrf_dfu_overlay_spinner = NULL;
    nrf_dfu_overlay_status = NULL;
    nrf_dfu_overlay_bar = NULL;
    nrf_dfu_overlay_percent = NULL;
    nrf_dfu_overlay_close_btn = NULL;
    nrf_dfu_overlay_hold = 0;
}

void ui_nrf52840_dfu_leave(void)
{
    int restore = 0;

    pthread_mutex_lock(&nrf_dfu_lock);
    if(!nrf_dfu_running && nrf_dfu_restore_meshtastic_on_leave) {
        restore = 1;
        nrf_dfu_restore_meshtastic_on_leave = 0;
    }
    pthread_mutex_unlock(&nrf_dfu_lock);

    if(restore) {
        ui_meshtastic_startup();
    }
}

int ui_nrf52840_dfu_is_running(void)
{
    int running;

    pthread_mutex_lock(&nrf_dfu_lock);
    running = nrf_dfu_running;
    pthread_mutex_unlock(&nrf_dfu_lock);
    return running;
}
