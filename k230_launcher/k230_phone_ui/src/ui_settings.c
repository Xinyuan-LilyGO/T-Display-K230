#include "ui_settings.h"

#include "ui_hardware.h"
#include "ui_haptic.h"
#include "ui_i18n.h"
#include "ui_lora.h"
#include "ui_meshtastic.h"
#include "ui_nrf9151_manager.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static lv_obj_t *settings_notice_overlay;
static lv_obj_t *settings_body;
static int32_t settings_saved_scroll_y;
static int settings_saved_scroll_valid;
static int settings_restore_scroll_on_create;
static lv_obj_t *settings_cloud_overlay;
static lv_obj_t *settings_cloud_status_label;
static lv_obj_t *settings_cloud_token_label;
static lv_obj_t *settings_cloud_read_button;
static lv_obj_t *settings_cloud_provision_button;
static pthread_mutex_t settings_cloud_lock = PTHREAD_MUTEX_INITIALIZER;
static int settings_cloud_busy;
static int settings_cloud_token_ready;
static char settings_cloud_token[512];
static char settings_cloud_status[256];

#define SETTINGS_CLOUD_TOKEN_PATH "/root/nrf9151/attestation_token.txt"
#define SETTINGS_CLOUD_TOKEN_TMP_PATH "/root/nrf9151/attestation_token.txt.tmp"

enum {
    SETTINGS_CLOUD_READ_TOKEN = 1,
    SETTINGS_CLOUD_PROVISION = 2,
};

static int settings_cloud_save_token(const char *token)
{
    FILE *fp;
    size_t len;

    if(!token || !token[0]) {
        errno = EINVAL;
        return -1;
    }
    if(mkdir("/root/nrf9151", 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    fp = fopen(SETTINGS_CLOUD_TOKEN_TMP_PATH, "w");
    if(!fp) {
        return -1;
    }
    len = strlen(token);
    if(fwrite(token, 1, len, fp) != len || fputc('\n', fp) == EOF ||
       fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        int saved_errno = errno;

        fclose(fp);
        unlink(SETTINGS_CLOUD_TOKEN_TMP_PATH);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }
    if(fclose(fp) != 0 || rename(SETTINGS_CLOUD_TOKEN_TMP_PATH,
                                 SETTINGS_CLOUD_TOKEN_PATH) != 0) {
        int saved_errno = errno;

        unlink(SETTINGS_CLOUD_TOKEN_TMP_PATH);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }
    (void)chmod(SETTINGS_CLOUD_TOKEN_PATH, 0644);
    return 0;
}

static void settings_restore_scroll_async(void *user_data)
{
    (void)user_data;
    if(settings_saved_scroll_valid && settings_body &&
       lv_obj_is_valid(settings_body)) {
        lv_obj_scroll_to_y(settings_body, settings_saved_scroll_y, LV_ANIM_OFF);
        settings_saved_scroll_valid = 0;
    }
    settings_restore_scroll_on_create = 0;
    app_request_fast_refresh();
}

void ui_settings_capture_scroll(void)
{
    if(settings_body && lv_obj_is_valid(settings_body)) {
        settings_saved_scroll_y = lv_obj_get_scroll_y(settings_body);
        settings_saved_scroll_valid = 1;
    }
}

void ui_settings_clear_saved_scroll(void)
{
    settings_saved_scroll_y = 0;
    settings_saved_scroll_valid = 0;
    settings_restore_scroll_on_create = 0;
}

void ui_settings_prepare_open(int restore_saved_scroll)
{
    if(restore_saved_scroll && settings_saved_scroll_valid) {
        settings_restore_scroll_on_create = 1;
        return;
    }

    ui_settings_clear_saved_scroll();
}

static void settings_refresh_async(void *user_data)
{
    (void)user_data;
    app_refresh_current_page();
}

static void settings_notice_close_event_cb(lv_event_t *event)
{
    (void)event;
    if(settings_notice_overlay && lv_obj_is_valid(settings_notice_overlay)) {
        lv_obj_delete(settings_notice_overlay);
    }
    settings_notice_overlay = NULL;
    app_request_fast_refresh();
}

static void settings_show_notice(const char *title, const char *detail,
                                 const char *hint)
{
    int panel_w = ui_is_landscape() ? 520 : ui_fit_width(lv_layer_top(), 24, 520);
    int panel_h = 292;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *btn;

    if(settings_notice_overlay && lv_obj_is_valid(settings_notice_overlay)) {
        lv_obj_delete(settings_notice_overlay);
    }

    settings_notice_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(settings_notice_overlay);
    lv_obj_set_style_bg_color(settings_notice_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(settings_notice_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(settings_notice_overlay, 0, 0);
    lv_obj_set_style_pad_all(settings_notice_overlay, 0, 0);
    lv_obj_clear_flag(settings_notice_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_panel(settings_notice_overlay, 0, 0, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(panel, 20, 0);

    label = ui_label(panel, title, &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(label, panel_w - 40);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = ui_label(panel, detail ? detail : "--", &lv_font_montserrat_18,
                     0xF5A524);
    lv_obj_set_width(label, panel_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 56);

    label = ui_label(panel, hint, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(label, panel_w - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 116);

    btn = ui_command_button(panel, panel_w - 172, panel_h - 74, 132, "OK",
                            0x3DA5FF);
    lv_obj_add_event_cb(btn, settings_notice_close_event_cb, LV_EVENT_CLICKED,
                        NULL);
    app_request_fast_refresh();
}

static void meshtastic_autostart_switch_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    ui_meshtastic_set_autostart_enabled(lv_obj_has_state(sw,
                                                         LV_STATE_CHECKED));
}

static void nrf9151_location_switch_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    int enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    k230_nrf9151_set_location_autostart_enabled(enabled);
    if(k230_nrf9151_apply_location_autostart() != 0 && enabled) {
        settings_show_notice(ui_tr("nRF9151 location"),
                             ui_tr("nRF9151 GNSS not detected"),
                             ui_tr("Check the nRF9151 module before enabling background positioning."));
    }
}

static void settings_cloud_setup_render_async(void *user_data)
{
    char token[sizeof(settings_cloud_token)];
    char status[sizeof(settings_cloud_status)];
    int busy;
    int token_ready;

    (void)user_data;
    pthread_mutex_lock(&settings_cloud_lock);
    snprintf(token, sizeof(token), "%s", settings_cloud_token);
    snprintf(status, sizeof(status), "%s", settings_cloud_status);
    busy = settings_cloud_busy;
    token_ready = settings_cloud_token_ready;
    pthread_mutex_unlock(&settings_cloud_lock);

    if(!settings_cloud_overlay || !lv_obj_is_valid(settings_cloud_overlay)) {
        return;
    }
    if(settings_cloud_status_label && lv_obj_is_valid(settings_cloud_status_label)) {
        lv_label_set_text(settings_cloud_status_label, status);
    }
    if(settings_cloud_token_label && lv_obj_is_valid(settings_cloud_token_label)) {
        lv_label_set_text(settings_cloud_token_label,
                          token[0] ? "Token saved to /root/nrf9151/attestation_token.txt" :
                                     "Token not saved yet");
    }
    if(settings_cloud_provision_button &&
       lv_obj_is_valid(settings_cloud_provision_button)) {
        if(busy || !token_ready) {
            lv_obj_add_state(settings_cloud_provision_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(settings_cloud_provision_button, LV_STATE_DISABLED);
        }
    }
    if(settings_cloud_read_button && lv_obj_is_valid(settings_cloud_read_button)) {
        if(busy) {
            lv_obj_add_state(settings_cloud_read_button, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(settings_cloud_read_button, LV_STATE_DISABLED);
        }
    }
    app_request_fast_refresh();
}

static void *settings_cloud_setup_worker(void *arg)
{
    int mode = (int)(intptr_t)arg;
    char token[sizeof(settings_cloud_token)] = "";
    int rc;
    int saved_errno = 0;
    int restart_location = k230_nrf9151_location_autostart_enabled();
    int restart_status = k230_nrf9151_status_monitor_active();

    if(!k230_nrf9151_uart_present()) {
        pthread_mutex_lock(&settings_cloud_lock);
        settings_cloud_busy = 0;
        snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                 "%s", "nRF9151 not detected. Token export is unavailable.");
        pthread_mutex_unlock(&settings_cloud_lock);
        lv_async_call(settings_cloud_setup_render_async, NULL);
        return NULL;
    }
    k230_nrf9151_status_monitor_stop();
    usleep(300000);
    (void)k230_nrf9151_stop_gnss_monitor_wait(6000);
    if(mode == SETTINGS_CLOUD_READ_TOKEN) {
        rc = k230_nrf9151_cloud_read_attestation_token(token, sizeof(token));
        saved_errno = errno;
        if(rc == 0 && settings_cloud_save_token(token) != 0) {
            rc = -1;
            saved_errno = errno;
        }
        if(restart_location) {
            (void)k230_nrf9151_apply_location_autostart();
        }
        if(restart_status) {
            (void)k230_nrf9151_status_monitor_start();
        }
        pthread_mutex_lock(&settings_cloud_lock);
        if(rc == 0) {
            snprintf(settings_cloud_token, sizeof(settings_cloud_token), "%s",
                     token);
            settings_cloud_token_ready = 1;
            snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                     "Token saved to /root/nrf9151/attestation_token.txt. Copy it with MTP, claim this device in nRF Cloud, then start provisioning.");
        } else {
            snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                     "Token read failed (%s). Check the nRF9151 firmware and module connection.",
                     strerror(saved_errno ? saved_errno : EIO));
        }
        settings_cloud_busy = 0;
        pthread_mutex_unlock(&settings_cloud_lock);
    } else {
        rc = k230_nrf9151_cloud_provision();
        saved_errno = errno;
        if(restart_location) {
            (void)k230_nrf9151_apply_location_autostart();
        }
        if(restart_status) {
            (void)k230_nrf9151_status_monitor_start();
        }
        pthread_mutex_lock(&settings_cloud_lock);
        if(rc == 0) {
            snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                     "%s", "Provisioning complete. A-GNSS is enabled automatically when GNSS starts.");
        } else {
            snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                     "Provisioning failed (%s). Check LTE registration and the nRF Cloud claim.",
                     strerror(saved_errno ? saved_errno : EIO));
        }
        settings_cloud_busy = 0;
        pthread_mutex_unlock(&settings_cloud_lock);
    }
    lv_async_call(settings_cloud_setup_render_async, NULL);
    return NULL;
}

static void settings_cloud_setup_start(int mode)
{
    pthread_t thread;

    pthread_mutex_lock(&settings_cloud_lock);
    if(settings_cloud_busy || (mode == SETTINGS_CLOUD_PROVISION &&
                               !settings_cloud_token_ready)) {
        pthread_mutex_unlock(&settings_cloud_lock);
        return;
    }
    settings_cloud_busy = 1;
    snprintf(settings_cloud_status, sizeof(settings_cloud_status), "%s",
             mode == SETTINGS_CLOUD_READ_TOKEN ?
             "Reading the modem attestation token..." :
             "Provisioning nRF Cloud credentials; LTE may reconnect...");
    pthread_mutex_unlock(&settings_cloud_lock);
    lv_async_call(settings_cloud_setup_render_async, NULL);

    if(pthread_create(&thread, NULL, settings_cloud_setup_worker,
                      (void *)(intptr_t)mode) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&settings_cloud_lock);
        settings_cloud_busy = 0;
        snprintf(settings_cloud_status, sizeof(settings_cloud_status),
                 "%s", "Unable to start the provisioning worker.");
        pthread_mutex_unlock(&settings_cloud_lock);
        lv_async_call(settings_cloud_setup_render_async, NULL);
    }
}

static void settings_cloud_setup_event_cb(lv_event_t *event)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(event);

    settings_cloud_setup_start(mode);
}

static void settings_cloud_setup_close_event_cb(lv_event_t *event)
{
    (void)event;
    if(settings_cloud_overlay && lv_obj_is_valid(settings_cloud_overlay)) {
        lv_obj_delete(settings_cloud_overlay);
    }
    settings_cloud_overlay = NULL;
    settings_cloud_status_label = NULL;
    settings_cloud_token_label = NULL;
    settings_cloud_read_button = NULL;
    settings_cloud_provision_button = NULL;
    app_request_fast_refresh();
}

int ui_settings_nrf_cloud_handle_back(void)
{
    if(!settings_cloud_overlay || !lv_obj_is_valid(settings_cloud_overlay)) {
        return 0;
    }
    settings_cloud_setup_close_event_cb(NULL);
    return 1;
}

static void settings_cloud_setup_open_event_cb(lv_event_t *event)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int margin;
    int content_w;
    int body_y;
    int status_h;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *button;
    lv_obj_t *card;

    (void)event;
    if(settings_cloud_overlay && lv_obj_is_valid(settings_cloud_overlay)) {
        return;
    }
    margin = landscape ? 28 : 20;
    content_w = screen_w - margin * 2;
    body_y = landscape ? 78 : 72;
    status_h = landscape ? 78 : 88;
    settings_cloud_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(settings_cloud_overlay);
    lv_obj_set_style_bg_color(settings_cloud_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(settings_cloud_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_cloud_overlay, 0, 0);
    lv_obj_set_style_pad_all(settings_cloud_overlay, 0, 0);
    lv_obj_clear_flag(settings_cloud_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_panel(settings_cloud_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    button = ui_command_button(panel, margin, 12, 92, "Back", 0x94A3B8);
    lv_obj_add_event_cb(button, settings_cloud_setup_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    label = ui_label(panel, "nRF Cloud", &lv_font_montserrat_26, 0xF2F5F8);
    lv_obj_set_width(label, content_w - 108);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, margin + 108, 12);
    label = ui_label(panel, "Pair once, then A-GNSS is automatic",
                     &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(label, content_w - 108);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, margin + 108, 44);

    card = ui_panel(panel, margin, body_y, content_w, status_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x17251F), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x286A4B), 0);
    label = ui_label(card, "A-GNSS SERVICE", &lv_font_montserrat_14, 0x70D6A0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 12);
    settings_cloud_status_label = ui_label(card, "Ready",
                                           &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_width(settings_cloud_status_label, content_w - 36);
    lv_label_set_long_mode(settings_cloud_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(settings_cloud_status_label, LV_ALIGN_TOP_LEFT, 18, 36);

    if(landscape) {
        int gap = 18;
        int left_w = (content_w - gap) * 46 / 100;
        int right_w = content_w - gap - left_w;
        int right_x = margin + left_w + gap;
        int card_y = body_y + status_h + 18;
        int card_h = screen_h - card_y - 20;

        card = ui_panel(panel, margin, card_y, left_w, card_h);
        label = ui_label(card, "1  DEVICE IDENTITY", &lv_font_montserrat_18,
                         0xF2F5F8);
        lv_obj_set_width(label, left_w - 32);
        lv_obj_set_pos(label, 16, 18);
        label = ui_label(card, "Read the attestation token and copy it\nfrom the SD/MTP export path.",
                         &lv_font_montserrat_16, 0xCBD5E1);
        lv_obj_set_width(label, left_w - 32);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(label, 16, 58);
        settings_cloud_token_label = ui_label(
            card, "Token not saved yet", &lv_font_montserrat_14, 0xF5A524);
        lv_obj_set_width(settings_cloud_token_label, left_w - 32);
        lv_label_set_long_mode(settings_cloud_token_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(settings_cloud_token_label, 16, 116);

        card = ui_panel(panel, right_x, card_y, right_w, card_h);
        label = ui_label(card, "2  CLOUD PAIRING", &lv_font_montserrat_18,
                         0xF2F5F8);
        lv_obj_set_width(label, right_w - 32);
        lv_obj_set_pos(label, 16, 18);
        label = ui_label(card,
                         "Claim the device in nRF Cloud Fleet,\nthen start provisioning while LTE is available.",
                         &lv_font_montserrat_16, 0xCBD5E1);
        lv_obj_set_width(label, right_w - 32);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(label, 16, 58);

        settings_cloud_read_button = ui_command_button(
            panel, margin + 16, screen_h - 76, left_w - 32,
            "Read token", 0x3DA5FF);
        lv_obj_add_event_cb(settings_cloud_read_button,
                            settings_cloud_setup_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SETTINGS_CLOUD_READ_TOKEN);
        settings_cloud_provision_button = ui_command_button(
            panel, right_x + 16, screen_h - 76, right_w - 32,
            "Start provisioning", 0x25C281);
        lv_obj_add_event_cb(settings_cloud_provision_button,
                            settings_cloud_setup_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SETTINGS_CLOUD_PROVISION);
    } else {
        int card_y = body_y + status_h + 14;
        int card_h = 144;

        card = ui_panel(panel, margin, card_y, content_w, card_h);
        label = ui_label(card, "1  DEVICE IDENTITY", &lv_font_montserrat_18,
                         0xF2F5F8);
        lv_obj_set_pos(label, 16, 14);
        label = ui_label(card, "Read token and export it through MTP.",
                         &lv_font_montserrat_16, 0xCBD5E1);
        lv_obj_set_width(label, content_w - 32);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(label, 16, 50);
        settings_cloud_token_label = ui_label(card, "Token not saved yet",
                                               &lv_font_montserrat_14, 0xF5A524);
        lv_obj_set_width(settings_cloud_token_label, content_w - 32);
        lv_label_set_long_mode(settings_cloud_token_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(settings_cloud_token_label, 16, 82);

        settings_cloud_read_button = ui_command_button(
            panel, margin, card_y + card_h + 12, content_w,
            "Read device token", 0x3DA5FF);
        lv_obj_add_event_cb(settings_cloud_read_button,
                            settings_cloud_setup_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SETTINGS_CLOUD_READ_TOKEN);
        card_y += card_h + 72;
        card = ui_panel(panel, margin, card_y, content_w, 142);
        label = ui_label(card, "2  CLOUD PAIRING", &lv_font_montserrat_18,
                         0xF2F5F8);
        lv_obj_set_pos(label, 16, 14);
        label = ui_label(card, "Claim in Fleet, then provision over LTE.",
                         &lv_font_montserrat_16, 0xCBD5E1);
        lv_obj_set_width(label, content_w - 32);
        lv_obj_set_pos(label, 16, 50);
        settings_cloud_provision_button = ui_command_button(
            panel, margin, card_y + 78, content_w, "Start provisioning",
            0x25C281);
        lv_obj_add_event_cb(settings_cloud_provision_button,
                            settings_cloud_setup_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SETTINGS_CLOUD_PROVISION);
    }

    pthread_mutex_lock(&settings_cloud_lock);
    settings_cloud_token_ready = settings_cloud_token[0] != '\0';
    if(!settings_cloud_status[0]) {
        snprintf(settings_cloud_status, sizeof(settings_cloud_status), "%s",
                 "Read the token. It will be saved to /root/nrf9151/attestation_token.txt for MTP export.");
    }
    pthread_mutex_unlock(&settings_cloud_lock);
    settings_cloud_setup_render_async(NULL);
    app_request_fast_refresh();
}

void ui_settings_open_nrf_cloud_setup(void)
{
    settings_cloud_setup_open_event_cb(NULL);
}

static int settings_section_header(lv_obj_t *parent, int y, const char *title,
                                   const char *subtitle)
{
    int w = ui_fit_width(parent, 24, 520);
    lv_obj_t *label = ui_label(parent, title, &lv_font_montserrat_20,
                               0xF2F5F8);

    lv_obj_set_pos(label, 32, y);
    lv_obj_set_width(label, w - 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    if(subtitle && subtitle[0]) {
        label = ui_label(parent, subtitle, &lv_font_montserrat_14, 0x9AA4AF);
        lv_obj_set_pos(label, 32, y + 30);
        lv_obj_set_width(label, w - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        return y + 64;
    }

    return y + 42;
}

static int settings_next_row_y(int y)
{
    return y + 108;
}

static void settings_switch_row(lv_obj_t *parent, int y, const char *symbol,
                                const char *title, const char *subtitle,
                                uint32_t color, int checked,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    int row_w = ui_fit_width(parent, 24, 520);
    int text_w = row_w > 220 ? row_w - 190 : 220;

    lv_obj_set_pos(row, 24, y);
    lv_obj_set_size(row, row_w, 94);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 54, 54);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = ui_label(icon_box, symbol, &lv_font_montserrat_20,
                              0xFFFFFF);
    lv_obj_center(icon);

    lv_obj_t *name = ui_label(row, title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 76, 14);

    lv_obj_t *detail = ui_label(row, subtitle, &lv_font_montserrat_16,
                                0x9AA4AF);
    lv_obj_set_width(detail, text_w);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 76, 48);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 38);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -2, 0);
    if(checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui_haptic_bind_touch(sw);
}

void ui_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    int y = 8;

    ui_create_header(scr, "Settings");
    body = ui_page_body(scr, 144);
    settings_body = body;
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B1016), 0);

    y = settings_section_header(body, y, "Connections",
                                "Network, Bluetooth and modem");
    ui_settings_nav_row(body, y, "WiFi", "Wi-Fi",
                        "Scan networks and connect", 0x25C281, PAGE_WIFI);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "ETH", "Ethernet",
                        "Wired link status and IP", 0x3DA5FF, PAGE_ETHERNET);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "BT", "Bluetooth",
                        "Scan and connect BLE devices", 0x3B82F6,
                        PAGE_BLE);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "LTE", "Cellular",
                        "Cellular modem and GNSS", 0x60A5FA,
                        PAGE_CELLULAR);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "5G", "USB Modem",
                        "USB modem status and dialing", 0x38BDF8,
                        PAGE_USB_MODEM);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "Display & input",
                                "Screen, language and keyboard");
    ui_settings_nav_row(body, y, LV_SYMBOL_EYE_OPEN, "Display",
                        "Backlight, orientation and panel status", 0x8B5CF6,
                        PAGE_DISPLAY);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "A", "Language",
                        "English, Chinese and Japanese UI text", 0x22D3EE,
                        PAGE_LANGUAGE);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_REFRESH, "Date & time",
                        "NTP server and time zone", 0xEC4899,
                        PAGE_TIME);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "KEY", "Keyboard settings",
                        "Auto detect, interval and key test", 0xF97316,
                        PAGE_KEYBOARD_SETTINGS);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "Sound & hardware",
                                "Audio, sensors and power");
    ui_settings_nav_row(body, y, LV_SYMBOL_AUDIO, "Audio",
                        "Volume and output route", 0x22D3EE,
                        PAGE_AUDIO_SETTINGS);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_AUDIO, "Notifications",
                        "Incoming message sound", 0xA78BFA,
                        PAGE_NOTIFICATION_SETTINGS);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "VIB", "Haptics",
                        "DRV2605 vibration effects", 0xF472B6,
                        PAGE_HAPTIC);
    y = settings_next_row_y(y);
#if K230_FAN_ENABLED
    ui_settings_nav_row(body, y, "FAN", "Fan",
                        "Manual or automatic cooling", 0xF5A524,
                        PAGE_FAN);
    y = settings_next_row_y(y);
#endif
    ui_settings_nav_row(body, y, "AHT", "Sensors",
                        "AHT20 temperature and humidity", 0x25C281,
                        PAGE_SENSORS);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "CHG", "Charger",
                        "BQ25896 charge current and ADC", 0xF97316,
                        PAGE_BQ25896);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "BAT", "Battery",
                        "Battery gauge and runtime", 0xA3E635,
                        PAGE_BATTERY);
    y = settings_next_row_y(y) + 14;

    y = settings_section_header(body, y, "System & about",
                                "Device information and diagnostics");
    ui_settings_nav_row(body, y, LV_SYMBOL_PLAY, "Startup apps",
                        "Launch selected services at boot", 0x10B981,
                        PAGE_APP_STARTUP);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, "SD", "Expand storage",
                        "Use full SD card capacity for apps and media.",
                        0x25C281, PAGE_STORAGE_EXPAND);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_LIST, "System",
                        "Kernel, CPU, memory, storage", 0xF5A524,
                        PAGE_SYSTEM);
    y = settings_next_row_y(y);
    ui_settings_nav_row(body, y, LV_SYMBOL_WARNING, "About phone",
                        "Buildroot version and device identity", 0x94A3B8,
                        PAGE_ABOUT);

    if(settings_restore_scroll_on_create && settings_saved_scroll_valid) {
        lv_async_call(settings_restore_scroll_async, NULL);
    } else {
        settings_restore_scroll_on_create = 0;
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    }
}

void ui_startup_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *label;
    int y = 8;

    ui_create_header(scr, "Startup apps");
    body = ui_page_body(scr, ui_is_landscape() ? 64 : 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B1016), 0);

    y = settings_section_header(body, y, "Startup apps",
                                "Choose services that start automatically");
    if(ui_lora_hardware_available() == 1) {
        settings_switch_row(body, y, "MESH", "Meshtastic autostart",
                            "Start Mesh daemon when launcher opens", 0x10B981,
                            ui_meshtastic_autostart_enabled(),
                            meshtastic_autostart_switch_event_cb);
        y = settings_next_row_y(y);
    }
    settings_switch_row(body, y, "GPS", "nRF9151 location",
                        "Start background GNSS positioning at boot",
                        0x3DA5FF,
                        k230_nrf9151_location_autostart_enabled(),
                        nrf9151_location_switch_event_cb);
    y = settings_next_row_y(y);
    y += 14;

    label = ui_label(body,
                     "Autostart keeps selected services ready for maps and GNSS pages.",
                     &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(label, 32, y);
    lv_obj_set_width(label, ui_fit_width(body, 32, 504));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}
