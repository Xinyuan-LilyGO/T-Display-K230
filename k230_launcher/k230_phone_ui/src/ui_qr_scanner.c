#include "ui_qr_scanner.h"

#include "ui_common.h"
#include "ui_i18n.h"
#include "ui_rtsp.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QR_SCANNER_BIN "/root/app/k230_phone_ui/k230_qr_scan"
#define QR_SCANNER_PREVIEW_FILE "/tmp/k230_qr_scanner_preview.rgb565"
#define QR_SCANNER_PREVIEW_TMP QR_SCANNER_PREVIEW_FILE ".tmp"
#define QR_SCANNER_LOG "/tmp/k230_qr_scanner.log"
#define QR_SCANNER_RESULT_DIR "/root/qrcode"
#define QR_SCANNER_RESULT_FILE QR_SCANNER_RESULT_DIR "/last.txt"
#define QR_SCANNER_CAPTURE_W 1280
#define QR_SCANNER_CAPTURE_H 720
#define QR_SCANNER_PREVIEW_W 512
#define QR_SCANNER_PREVIEW_H 288
#define QR_SCANNER_PREVIEW_BYTES (QR_SCANNER_PREVIEW_W * QR_SCANNER_PREVIEW_H * 2)

static pthread_mutex_t qr_scanner_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t qr_scanner_thread;
static int qr_scanner_thread_active;
static int qr_scanner_running;
static int qr_scanner_ready;
static int qr_scanner_ok;
static char qr_scanner_status[192] = "Ready";
static char qr_scanner_result[1024];

static lv_timer_t *qr_scanner_timer;
static lv_obj_t *qr_scanner_preview_image;
static lv_obj_t *qr_scanner_preview_placeholder;
static lv_obj_t *qr_scanner_status_label;
static lv_obj_t *qr_scanner_result_label;
static lv_obj_t *qr_scanner_scan_btn;
static lv_obj_t *qr_scanner_stop_btn;
static lv_obj_t *qr_scanner_save_btn;
static lv_obj_t *qr_scanner_guide_box;
static lv_obj_t *qr_scanner_toast;
static lv_obj_t *qr_scanner_toast_label;
static uint16_t *qr_scanner_preview_pixels;
static lv_image_dsc_t qr_scanner_preview_dsc;
static int qr_scanner_preview_panel_w;
static int qr_scanner_preview_panel_h;
static uint32_t qr_scanner_toast_until;

static void qr_scanner_set_status_locked(const char *status, int ok)
{
    snprintf(qr_scanner_status, sizeof(qr_scanner_status), "%s",
             status ? status : "");
    qr_scanner_ok = ok;
}

static void qr_scanner_kill_process(void)
{
    int rc = system("pkill -f 'k230_qr_scan.*k230_qr_scanner_preview' "
                    ">/dev/null 2>&1 || true");
    (void)rc;
}

static void qr_scanner_show_toast(const char *text, uint32_t color,
                                  uint32_t duration_ms)
{
    if(!qr_scanner_toast || !lv_obj_is_valid(qr_scanner_toast) ||
       !qr_scanner_toast_label ||
       !lv_obj_is_valid(qr_scanner_toast_label)) {
        return;
    }
    lv_label_set_text(qr_scanner_toast_label, text ? text : "");
    lv_obj_set_style_text_color(qr_scanner_toast_label,
                                lv_color_hex(color), 0);
    lv_obj_clear_flag(qr_scanner_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qr_scanner_toast);
    lv_obj_center(qr_scanner_toast_label);
    qr_scanner_toast_until = lv_tick_get() + duration_ms;
}

static void qr_scanner_preview_update(void)
{
    FILE *fp;
    size_t n;
    int scale_w;
    int scale_h;
    int scale;

    if(!qr_scanner_preview_image ||
       !lv_obj_is_valid(qr_scanner_preview_image)) {
        return;
    }
    if(!qr_scanner_preview_pixels) {
        qr_scanner_preview_pixels = malloc(QR_SCANNER_PREVIEW_BYTES);
        if(!qr_scanner_preview_pixels) {
            return;
        }
    }

    fp = fopen(QR_SCANNER_PREVIEW_FILE, "rb");
    if(!fp) {
        return;
    }
    n = fread(qr_scanner_preview_pixels, 1, QR_SCANNER_PREVIEW_BYTES, fp);
    fclose(fp);
    if(n != QR_SCANNER_PREVIEW_BYTES) {
        return;
    }

    memset(&qr_scanner_preview_dsc, 0, sizeof(qr_scanner_preview_dsc));
    qr_scanner_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    qr_scanner_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    qr_scanner_preview_dsc.header.w = QR_SCANNER_PREVIEW_W;
    qr_scanner_preview_dsc.header.h = QR_SCANNER_PREVIEW_H;
    qr_scanner_preview_dsc.header.stride = QR_SCANNER_PREVIEW_W * 2;
    qr_scanner_preview_dsc.data_size = QR_SCANNER_PREVIEW_BYTES;
    qr_scanner_preview_dsc.data = (const uint8_t *)qr_scanner_preview_pixels;

    lv_image_cache_drop(&qr_scanner_preview_dsc);
    lv_image_set_src(qr_scanner_preview_image, &qr_scanner_preview_dsc);

    scale_w = qr_scanner_preview_panel_w > 0 ?
        qr_scanner_preview_panel_w * 256 / QR_SCANNER_PREVIEW_W : 256;
    scale_h = qr_scanner_preview_panel_h > 0 ?
        qr_scanner_preview_panel_h * 256 / QR_SCANNER_PREVIEW_H : 256;
    scale = scale_w < scale_h ? scale_w : scale_h;
    if(scale < 128) {
        scale = 128;
    }
    if(scale > 768) {
        scale = 768;
    }
    lv_image_set_scale(qr_scanner_preview_image, scale);
    lv_obj_center(qr_scanner_preview_image);
    lv_obj_clear_flag(qr_scanner_preview_image, LV_OBJ_FLAG_HIDDEN);
    if(qr_scanner_preview_placeholder &&
       lv_obj_is_valid(qr_scanner_preview_placeholder)) {
        lv_obj_add_flag(qr_scanner_preview_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void qr_scanner_update_buttons(int running, int has_result)
{
    if(qr_scanner_scan_btn) {
        if(running) {
            lv_obj_add_state(qr_scanner_scan_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(qr_scanner_scan_btn, LV_STATE_DISABLED);
        }
    }
    if(qr_scanner_stop_btn) {
        if(running) {
            lv_obj_clear_state(qr_scanner_stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(qr_scanner_stop_btn, LV_STATE_DISABLED);
        }
    }
    if(qr_scanner_save_btn) {
        if(has_result) {
            lv_obj_clear_state(qr_scanner_save_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(qr_scanner_save_btn, LV_STATE_DISABLED);
        }
    }
}

static void qr_scanner_update(void)
{
    int running;
    int ready;
    int ok;
    char status[192];
    char result[1024];

    qr_scanner_preview_update();

    pthread_mutex_lock(&qr_scanner_lock);
    running = qr_scanner_running;
    ready = qr_scanner_ready;
    ok = qr_scanner_ok;
    snprintf(status, sizeof(status), "%s", qr_scanner_status);
    snprintf(result, sizeof(result), "%s", qr_scanner_result);
    if(ready) {
        qr_scanner_ready = 0;
    }
    pthread_mutex_unlock(&qr_scanner_lock);

    if(qr_scanner_status_label) {
        uint32_t color = running ? 0xF5A524 :
            (ok ? 0x25C281 : 0x9AA4AF);
        lv_label_set_text(qr_scanner_status_label,
                          ui_tr(status[0] ? status : "Ready"));
        lv_obj_set_style_text_color(qr_scanner_status_label,
                                    lv_color_hex(color), 0);
    }
    if(qr_scanner_result_label) {
        lv_label_set_text(qr_scanner_result_label,
                          result[0] ? result : ui_tr("No QR result yet"));
    }
    qr_scanner_update_buttons(running, result[0] != '\0');

    if(ready) {
        qr_scanner_show_toast(ok ? ui_tr("QR code scanned") :
                              ui_tr("QR scan timed out"),
                              ok ? 0x25C281 : 0xF5A524, 1000);
    }
    if(qr_scanner_toast && qr_scanner_toast_until &&
       (int32_t)(lv_tick_get() - qr_scanner_toast_until) >= 0) {
        lv_obj_add_flag(qr_scanner_toast, LV_OBJ_FLAG_HIDDEN);
        qr_scanner_toast_until = 0;
    }
}

static void *qr_scanner_worker(void *arg)
{
    char command[1024];
    char line[1024];
    FILE *fp;
    int rc = 1;

    (void)arg;

    snprintf(command, sizeof(command),
             QR_SCANNER_BIN
             " -w %d -h %d --skip 2 --focus-sweep"
             " --timeout-sec 45 --preview-file " QR_SCANNER_PREVIEW_FILE
             " --preview-width %d --preview-height %d"
             " --preview-interval-ms 100 --verbose 2>" QR_SCANNER_LOG,
             QR_SCANNER_CAPTURE_W, QR_SCANNER_CAPTURE_H,
             QR_SCANNER_PREVIEW_W, QR_SCANNER_PREVIEW_H);

    fp = popen(command, "r");
    line[0] = '\0';
    if(fp) {
        if(fgets(line, sizeof(line), fp)) {
            ui_trim_text(line);
        }
        rc = pclose(fp);
    } else {
        rc = -1;
    }

    pthread_mutex_lock(&qr_scanner_lock);
    qr_scanner_running = 0;
    qr_scanner_thread_active = 0;
    qr_scanner_ready = 1;
    if(rc == 0 && line[0]) {
        qr_scanner_set_status_locked("QR code scanned", 1);
        snprintf(qr_scanner_result, sizeof(qr_scanner_result), "%s", line);
    } else {
        qr_scanner_set_status_locked("QR scan timed out", 0);
    }
    pthread_mutex_unlock(&qr_scanner_lock);
    app_request_fast_refresh();
    return NULL;
}

static void qr_scanner_start(void)
{
    pthread_t thread;

    if(access(QR_SCANNER_BIN, X_OK) != 0) {
        pthread_mutex_lock(&qr_scanner_lock);
        qr_scanner_set_status_locked("QR scanner binary missing", 0);
        qr_scanner_ready = 1;
        pthread_mutex_unlock(&qr_scanner_lock);
        return;
    }
    if(ui_rtsp_is_active()) {
        pthread_mutex_lock(&qr_scanner_lock);
        qr_scanner_set_status_locked("Stop RTSP before scanning QR", 0);
        qr_scanner_ready = 1;
        pthread_mutex_unlock(&qr_scanner_lock);
        return;
    }

    pthread_mutex_lock(&qr_scanner_lock);
    if(qr_scanner_running) {
        pthread_mutex_unlock(&qr_scanner_lock);
        return;
    }
    qr_scanner_running = 1;
    qr_scanner_thread_active = 1;
    qr_scanner_ready = 0;
    qr_scanner_result[0] = '\0';
    qr_scanner_set_status_locked("Scanning QR...", 0);
    pthread_mutex_unlock(&qr_scanner_lock);

    unlink(QR_SCANNER_PREVIEW_FILE);
    unlink(QR_SCANNER_PREVIEW_TMP);
    unlink(QR_SCANNER_LOG);

    if(pthread_create(&thread, NULL, qr_scanner_worker, NULL) == 0) {
        pthread_detach(thread);
        qr_scanner_thread = thread;
    } else {
        pthread_mutex_lock(&qr_scanner_lock);
        qr_scanner_running = 0;
        qr_scanner_thread_active = 0;
        qr_scanner_set_status_locked("QR scanner thread failed", 0);
        qr_scanner_ready = 1;
        pthread_mutex_unlock(&qr_scanner_lock);
    }
}

static void qr_scanner_stop(void)
{
    pthread_mutex_lock(&qr_scanner_lock);
    if(qr_scanner_running || qr_scanner_thread_active) {
        qr_scanner_set_status_locked("Stopping scanner...", 0);
    }
    pthread_mutex_unlock(&qr_scanner_lock);

    qr_scanner_kill_process();
}

static void qr_scanner_scan_event(lv_event_t *event)
{
    (void)event;
    qr_scanner_start();
    qr_scanner_update();
}

static void qr_scanner_stop_event(lv_event_t *event)
{
    (void)event;
    qr_scanner_stop();
    qr_scanner_update();
}

static void qr_scanner_save_event(lv_event_t *event)
{
    FILE *fp;
    char result[1024];

    (void)event;

    pthread_mutex_lock(&qr_scanner_lock);
    snprintf(result, sizeof(result), "%s", qr_scanner_result);
    pthread_mutex_unlock(&qr_scanner_lock);

    if(!result[0]) {
        qr_scanner_show_toast(ui_tr("No QR result yet"), 0xF5A524, 1000);
        return;
    }
    if(mkdir(QR_SCANNER_RESULT_DIR, 0755) != 0 && errno != EEXIST) {
        qr_scanner_show_toast(ui_tr("Save failed"), 0xEF4D5A, 1200);
        return;
    }
    fp = fopen(QR_SCANNER_RESULT_FILE, "w");
    if(!fp) {
        qr_scanner_show_toast(ui_tr("Save failed"), 0xEF4D5A, 1200);
        return;
    }
    fprintf(fp, "%s\n", result);
    if(fclose(fp) != 0) {
        qr_scanner_show_toast(ui_tr("Save failed"), 0xEF4D5A, 1200);
        return;
    }
    qr_scanner_show_toast(ui_tr("Saved to /root/qrcode/last.txt"),
                          0x25C281, 1200);
}

static void qr_scanner_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    qr_scanner_update();
}

static void qr_scanner_add_toast(lv_obj_t *parent, int w, int h)
{
    (void)h;

    qr_scanner_toast = lv_obj_create(parent);
    lv_obj_set_size(qr_scanner_toast, w > 440 ? 360 : w - 64, 64);
    lv_obj_align(qr_scanner_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(qr_scanner_toast, 18, 0);
    lv_obj_set_style_bg_color(qr_scanner_toast, lv_color_hex(0x0B1118), 0);
    lv_obj_set_style_bg_opa(qr_scanner_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_color(qr_scanner_toast, lv_color_hex(0x263241), 0);
    lv_obj_set_style_border_width(qr_scanner_toast, 1, 0);
    lv_obj_set_style_pad_all(qr_scanner_toast, 0, 0);
    lv_obj_add_flag(qr_scanner_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(qr_scanner_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(qr_scanner_toast, LV_OBJ_FLAG_CLICKABLE);

    qr_scanner_toast_label =
        ui_label(qr_scanner_toast, "", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_width(qr_scanner_toast_label,
                     lv_obj_get_width(qr_scanner_toast) - 32);
    lv_label_set_long_mode(qr_scanner_toast_label, LV_LABEL_LONG_DOT);
    lv_obj_center(qr_scanner_toast_label);
}

static void qr_scanner_create_preview(lv_obj_t *parent, int x, int y,
                                      int w, int h)
{
    lv_obj_t *preview = ui_panel(parent, x, y, w, h);
    int guide_size;

    lv_obj_set_style_bg_color(preview, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x263442), 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    qr_scanner_preview_panel_w = w - 24;
    qr_scanner_preview_panel_h = h - 24;
    qr_scanner_preview_image = lv_image_create(preview);
    lv_obj_add_flag(qr_scanner_preview_image, LV_OBJ_FLAG_HIDDEN);

    qr_scanner_preview_placeholder =
        ui_label(preview, ui_tr("Point camera at a QR code"),
                 &lv_font_montserrat_22, 0x94A3B8);
    lv_obj_set_width(qr_scanner_preview_placeholder, w - 48);
    lv_label_set_long_mode(qr_scanner_preview_placeholder,
                           LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(qr_scanner_preview_placeholder,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(qr_scanner_preview_placeholder);

    guide_size = qr_scanner_preview_panel_w < qr_scanner_preview_panel_h ?
        qr_scanner_preview_panel_w : qr_scanner_preview_panel_h;
    guide_size = (guide_size * 68) / 100;
    if(guide_size < 180) {
        guide_size = 180;
    }
    qr_scanner_guide_box = lv_obj_create(preview);
    lv_obj_set_size(qr_scanner_guide_box, guide_size, guide_size);
    lv_obj_center(qr_scanner_guide_box);
    lv_obj_set_style_bg_opa(qr_scanner_guide_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(qr_scanner_guide_box, 3, 0);
    lv_obj_set_style_border_color(qr_scanner_guide_box,
                                  lv_color_hex(0x25C281), 0);
    lv_obj_set_style_radius(qr_scanner_guide_box, 16, 0);
    lv_obj_clear_flag(qr_scanner_guide_box, LV_OBJ_FLAG_SCROLLABLE);
}

static void qr_scanner_create_info(lv_obj_t *parent, int x, int y,
                                   int w, int h, int landscape)
{
    lv_obj_t *card = ui_panel(parent, x, y, w, h);
    lv_obj_t *title;
    lv_obj_t *hint;
    int pad = 18;
    int btn_y = h - (landscape ? 70 : 76);
    int gap = 10;
    int btn_w = (w - pad * 2 - gap * 2) / 3;

    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x223044), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(card, ui_tr("QR Scanner"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, pad, 18);
    lv_obj_set_width(title, w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    qr_scanner_status_label =
        ui_label(card, ui_tr("Scanning QR..."), &lv_font_montserrat_18,
                 0xF5A524);
    lv_obj_set_pos(qr_scanner_status_label, pad, 58);
    lv_obj_set_width(qr_scanner_status_label, w - pad * 2);
    lv_label_set_long_mode(qr_scanner_status_label, LV_LABEL_LONG_DOT);

    hint = ui_label(card, ui_tr("Decoded result"), &lv_font_montserrat_16,
                    0x94A3B8);
    lv_obj_set_pos(hint, pad, 98);
    lv_obj_set_width(hint, w - pad * 2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);

    qr_scanner_result_label =
        ui_label(card, ui_tr("No QR result yet"), &lv_font_montserrat_18,
                 0xF2F5F8);
    lv_obj_set_pos(qr_scanner_result_label, pad, 130);
    lv_obj_set_width(qr_scanner_result_label, w - pad * 2);
    lv_label_set_long_mode(qr_scanner_result_label, LV_LABEL_LONG_WRAP);

    qr_scanner_scan_btn =
        ui_command_button(card, pad, btn_y, btn_w, ui_tr("Scan"), 0x25C281);
    lv_obj_add_event_cb(qr_scanner_scan_btn, qr_scanner_scan_event,
                        LV_EVENT_CLICKED, NULL);

    qr_scanner_stop_btn =
        ui_command_button(card, pad + btn_w + gap, btn_y, btn_w,
                          ui_tr("Stop"), 0xEF4D5A);
    lv_obj_add_event_cb(qr_scanner_stop_btn, qr_scanner_stop_event,
                        LV_EVENT_CLICKED, NULL);

    qr_scanner_save_btn =
        ui_command_button(card, pad + (btn_w + gap) * 2, btn_y, btn_w,
                          ui_tr("Save"), 0x60A5FA);
    lv_obj_add_event_cb(qr_scanner_save_btn, qr_scanner_save_event,
                        LV_EVENT_CLICKED, NULL);
}

void ui_qr_scanner_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int landscape = ui_is_landscape();
    int top_y = ui_page_top_y(72);
    int body_h = ui_body_height(top_y);
    int margin = ui_page_side_margin();
    int w = ui_screen_width();
    int content_w = w - margin * 2;

    ui_create_header(scr, "QR Scanner");

    body = lv_obj_create(scr);
    lv_obj_set_pos(body, 0, top_y);
    lv_obj_set_size(body, ui_screen_width(), body_h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    if(landscape) {
        int preview_w = (content_w * 62) / 100;
        int info_w = content_w - preview_w - 18;

        qr_scanner_create_preview(body, margin, 18, preview_w, body_h - 36);
        qr_scanner_create_info(body, margin + preview_w + 18, 18, info_w,
                               body_h - 36, landscape);
    } else {
        int preview_h = (body_h * 54) / 100;
        int info_h = body_h - preview_h - 30;

        if(preview_h < 360) {
            preview_h = 360;
        }
        qr_scanner_create_preview(body, margin, 18, content_w, preview_h);
        qr_scanner_create_info(body, margin, preview_h + 34, content_w,
                               info_h, landscape);
    }

    qr_scanner_add_toast(scr, ui_screen_width(), ui_screen_height());
    qr_scanner_timer = lv_timer_create(qr_scanner_timer_cb, 150, NULL);
    qr_scanner_start();
    qr_scanner_update();
}

void ui_qr_scanner_cleanup(void)
{
    if(qr_scanner_timer) {
        lv_timer_delete(qr_scanner_timer);
        qr_scanner_timer = NULL;
    }
    qr_scanner_stop();
    if(qr_scanner_preview_pixels) {
        free(qr_scanner_preview_pixels);
        qr_scanner_preview_pixels = NULL;
    }
    qr_scanner_preview_image = NULL;
    qr_scanner_preview_placeholder = NULL;
    qr_scanner_status_label = NULL;
    qr_scanner_result_label = NULL;
    qr_scanner_scan_btn = NULL;
    qr_scanner_stop_btn = NULL;
    qr_scanner_save_btn = NULL;
    qr_scanner_guide_box = NULL;
    qr_scanner_toast = NULL;
    qr_scanner_toast_label = NULL;
    qr_scanner_toast_until = 0;
}
