#include "ui_usb_storage.h"

#include "ui_common.h"
#include "ui_i18n.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_MTP_SCRIPT "/etc/init.d/S41adb_mtp"
#define USB_MTP_STORAGE_STOP_SCRIPT "/usr/sbin/k230-usb-storage-gadget"
#define USB_MTP_LOG "/tmp/k230_mtp_ui.log"
#define USB_MTP_GADGET_UDC "/sys/kernel/config/usb_gadget/g1/UDC"
#define USB_MTP_EP0 "/dev/usb-ffs/mtp/ep0"
#define USB_MTP_STORE "/root/"
#define USB_MTP_STORE_LABEL "/root/ on SD card"
#define USB_MTP_SCREENSHOT_DIR "/root/screenshots"

typedef enum {
    USB_ACTION_START = 0,
    USB_ACTION_STOP,
} usb_storage_action_t;

static pthread_mutex_t usb_storage_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_timer_t *usb_storage_timer;
static lv_obj_t *usb_storage_status_label;
static lv_obj_t *usb_storage_state_label;
static lv_obj_t *usb_storage_image_label;
static lv_obj_t *usb_storage_mount_label;
static lv_obj_t *usb_storage_screenshot_label;
static lv_obj_t *usb_storage_start_btn;
static lv_obj_t *usb_storage_stop_btn;
static int usb_storage_busy;
static int usb_storage_result_ready;
static int usb_storage_last_rc;
static char usb_storage_status_text[192] = "Ready";

static int usb_storage_is_active(void)
{
    char udc[64];

    if(ui_read_file_first_line(USB_MTP_GADGET_UDC, udc, sizeof(udc)) == 0) {
        return udc[0] != '\0';
    }

    return access(USB_MTP_EP0, F_OK) == 0;
}

static void usb_storage_set_status_locked(const char *text, int rc)
{
    snprintf(usb_storage_status_text, sizeof(usb_storage_status_text), "%s",
             text ? text : "");
    usb_storage_last_rc = rc;
    usb_storage_result_ready = 1;
}

static void *usb_storage_action_thread(void *arg)
{
    usb_storage_action_t action = (usb_storage_action_t)(intptr_t)arg;
    char cmd[384];
    int rc;

    if(action == USB_ACTION_START) {
        snprintf(cmd, sizeof(cmd),
                 "if [ -x %s ]; then %s stop >/dev/null 2>&1; fi; "
                 "%s start-manual > %s 2>&1",
                 USB_MTP_STORAGE_STOP_SCRIPT, USB_MTP_STORAGE_STOP_SCRIPT,
                 USB_MTP_SCRIPT, USB_MTP_LOG);
    } else {
        snprintf(cmd, sizeof(cmd), "%s stop > %s 2>&1",
                 USB_MTP_SCRIPT, USB_MTP_LOG);
    }
    rc = system(cmd);
    rc = ui_shell_exit_code(rc);

    pthread_mutex_lock(&usb_storage_lock);
    usb_storage_busy = 0;
    if(rc == 0) {
        usb_storage_set_status_locked(action == USB_ACTION_START ?
                                      "MTP mode active" :
                                      "MTP mode stopped", rc);
    } else {
        usb_storage_set_status_locked(action == USB_ACTION_START ?
                                      "Start failed, check /tmp log" :
                                      "Stop failed, check /tmp log", rc);
    }
    pthread_mutex_unlock(&usb_storage_lock);
    app_request_fast_refresh();
    return NULL;
}

static void usb_storage_start_action(usb_storage_action_t action)
{
    pthread_t thread;

    pthread_mutex_lock(&usb_storage_lock);
    if(usb_storage_busy) {
        pthread_mutex_unlock(&usb_storage_lock);
        return;
    }
    usb_storage_busy = 1;
    usb_storage_set_status_locked(action == USB_ACTION_START ?
                                  "Starting MTP..." :
                                  "Stopping MTP...", 0);
    pthread_mutex_unlock(&usb_storage_lock);

    if(pthread_create(&thread, NULL, usb_storage_action_thread,
                      (void *)(intptr_t)action) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&usb_storage_lock);
        usb_storage_busy = 0;
        usb_storage_set_status_locked("MTP action thread failed", -1);
        pthread_mutex_unlock(&usb_storage_lock);
    }
}

static void usb_storage_start_event_cb(lv_event_t *event)
{
    (void)event;
    usb_storage_start_action(USB_ACTION_START);
}

static void usb_storage_stop_event_cb(lv_event_t *event)
{
    (void)event;
    usb_storage_start_action(USB_ACTION_STOP);
}

static void usb_storage_update(void)
{
    int active = usb_storage_is_active();
    int busy;
    int rc;
    char status[192];

    pthread_mutex_lock(&usb_storage_lock);
    busy = usb_storage_busy;
    rc = usb_storage_last_rc;
    snprintf(status, sizeof(status), "%s", usb_storage_status_text);
    usb_storage_result_ready = 0;
    pthread_mutex_unlock(&usb_storage_lock);

    if(usb_storage_status_label) {
        uint32_t color = active ? 0x25C281 : (rc == 0 ? 0x9AA4AF : 0xF5A524);
        lv_label_set_text(usb_storage_status_label,
                          ui_tr(busy ? status :
                                (active ? "Shared with host computer" : status)));
        lv_obj_set_style_text_color(usb_storage_status_label, lv_color_hex(color), 0);
    }

    if(usb_storage_state_label) {
        lv_label_set_text(usb_storage_state_label,
                          ui_tr(active ? "MTP active" : "Local mode"));
        lv_obj_set_style_text_color(usb_storage_state_label,
                                    lv_color_hex(active ? 0x25C281 : 0x9AA4AF), 0);
    }

    if(usb_storage_image_label) {
        lv_label_set_text(usb_storage_image_label, ui_tr("MTP + ADB"));
    }

    if(usb_storage_mount_label) {
        lv_label_set_text(usb_storage_mount_label, ui_tr(USB_MTP_STORE_LABEL));
    }

    if(usb_storage_screenshot_label) {
        lv_label_set_text(usb_storage_screenshot_label, USB_MTP_SCREENSHOT_DIR);
    }

    if(usb_storage_start_btn) {
        if(active || busy) {
            lv_obj_add_state(usb_storage_start_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(usb_storage_start_btn, LV_STATE_DISABLED);
        }
    }

    if(usb_storage_stop_btn) {
        if(!active || busy) {
            lv_obj_add_state(usb_storage_stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(usb_storage_stop_btn, LV_STATE_DISABLED);
        }
    }
}

static void usb_storage_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    usb_storage_update();
}

void ui_usb_storage_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *icon_box;
    lv_obj_t *icon;
    lv_obj_t *title;
    lv_obj_t *label_obj;
    int landscape = ui_is_landscape();
    int body_x = ui_page_panel_x();
    int body_w = ui_page_panel_width();
    int left_w = landscape ? body_w * 34 / 100 : 520;
    int left_x = landscape ? 0 : 0;
    int right_x;
    int right_w;
    int button_w;
    int value_w;

    if(left_w < 260) {
        left_w = 260;
    }
    if(left_w > 360) {
        left_w = 360;
    }
    right_x = landscape ? left_w + 36 : 0;
    right_w = landscape ? body_w - right_x : ui_inner_width();
    button_w = landscape ? (right_w - 20) / 2 : 226;
    if(right_w < 260) {
        right_w = 260;
    }
    if(button_w < 118) {
        button_w = 118;
    }
    value_w = landscape ? right_w / 2 : 300;
    if(value_w < 180) {
        value_w = 180;
    }

    ui_create_header(scr, "MTP");

    body = ui_scroll_panel(scr, body_x, ui_page_top_y(154), body_w,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    icon_box = lv_obj_create(body);
    lv_obj_set_size(icon_box, 104, 104);
    if(landscape) {
        lv_obj_set_pos(icon_box, left_x + (left_w - 104) / 2, 38);
    } else {
        lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 8);
    }
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x41C7C7), 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    icon = ui_label(icon_box, "MTP", &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_center(icon);

    title = ui_label(body, "MTP file access", &lv_font_montserrat_24, 0xF2F5F8);
    if(landscape) {
        lv_obj_set_width(title, left_w);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(title, left_x, 164);
    } else {
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 132);
    }

    usb_storage_status_label = ui_label(body, "Ready", &lv_font_montserrat_20,
                                        0x9AA4AF);
    lv_obj_set_width(usb_storage_status_label,
                     landscape ? right_w : ui_inner_width());
    lv_label_set_long_mode(usb_storage_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(usb_storage_status_label,
                                landscape ? LV_TEXT_ALIGN_LEFT :
                                LV_TEXT_ALIGN_CENTER, 0);
    if(landscape) {
        lv_obj_set_pos(usb_storage_status_label, right_x, 34);
    } else {
        lv_obj_align(usb_storage_status_label, LV_ALIGN_TOP_MID, 0, 172);
    }

    label_obj = ui_label(body, "State", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(label_obj, landscape ? right_x : 0, landscape ? 104 : 244);
    usb_storage_state_label = ui_label(body, "--", &lv_font_montserrat_20,
                                       0xF2F5F8);
    lv_obj_set_width(usb_storage_state_label, landscape ? value_w : 260);
    lv_obj_set_style_text_align(usb_storage_state_label, LV_TEXT_ALIGN_RIGHT, 0);
    if(landscape) {
        lv_obj_set_pos(usb_storage_state_label, right_x + right_w - value_w, 100);
    } else {
        lv_obj_align(usb_storage_state_label, LV_ALIGN_TOP_RIGHT, 0, 240);
    }

    label_obj = ui_label(body, "Protocol", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(label_obj, landscape ? right_x : 0, landscape ? 166 : 306);
    usb_storage_image_label = ui_label(body, "MTP + ADB",
                                       &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(usb_storage_image_label, landscape ? value_w : 330);
    lv_label_set_long_mode(usb_storage_image_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(usb_storage_image_label, LV_TEXT_ALIGN_RIGHT, 0);
    if(landscape) {
        lv_obj_set_pos(usb_storage_image_label, right_x + right_w - value_w, 164);
    } else {
        lv_obj_align(usb_storage_image_label, LV_ALIGN_TOP_RIGHT, 0, 304);
    }

    label_obj = ui_label(body, "Storage", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(label_obj, landscape ? right_x : 0, landscape ? 228 : 368);
    usb_storage_mount_label = ui_label(body, USB_MTP_STORE,
                                       &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(usb_storage_mount_label, value_w);
    lv_label_set_long_mode(usb_storage_mount_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(usb_storage_mount_label, LV_TEXT_ALIGN_RIGHT, 0);
    if(landscape) {
        lv_obj_set_pos(usb_storage_mount_label, right_x + right_w - value_w, 226);
    } else {
        lv_obj_align(usb_storage_mount_label, LV_ALIGN_TOP_RIGHT, 0, 366);
    }

    label_obj = ui_label(body, "Screenshots", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(label_obj, landscape ? right_x : 0, landscape ? 286 : 430);
    usb_storage_screenshot_label = ui_label(body, USB_MTP_SCREENSHOT_DIR,
                                            &lv_font_montserrat_18, 0x22C55E);
    lv_obj_set_width(usb_storage_screenshot_label, value_w);
    lv_label_set_long_mode(usb_storage_screenshot_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(usb_storage_screenshot_label,
                                LV_TEXT_ALIGN_RIGHT, 0);
    if(landscape) {
        lv_obj_set_pos(usb_storage_screenshot_label,
                       right_x + right_w - value_w, 284);
    } else {
        lv_obj_align(usb_storage_screenshot_label, LV_ALIGN_TOP_RIGHT, 0, 428);
    }

    usb_storage_start_btn = ui_command_button(body,
                                              landscape ? right_x : 0,
                                              landscape ? 352 : 528,
                                              button_w, "Start MTP",
                                              0x25C281);
    lv_obj_add_event_cb(usb_storage_start_btn, usb_storage_start_event_cb,
                        LV_EVENT_CLICKED, NULL);

    usb_storage_stop_btn = ui_command_button(body,
                                             landscape ? right_x + button_w + 20 : 262,
                                             landscape ? 352 : 528,
                                             button_w, "Stop MTP",
                                             0xEF4D5A);
    lv_obj_add_event_cb(usb_storage_stop_btn, usb_storage_stop_event_cb,
                        LV_EVENT_CLICKED, NULL);

    usb_storage_timer = lv_timer_create(usb_storage_timer_cb, 500, NULL);
    usb_storage_update();
}

void ui_usb_storage_cleanup(void)
{
    if(usb_storage_timer) {
        lv_timer_delete(usb_storage_timer);
        usb_storage_timer = NULL;
    }
    usb_storage_status_label = NULL;
    usb_storage_state_label = NULL;
    usb_storage_image_label = NULL;
    usb_storage_mount_label = NULL;
    usb_storage_screenshot_label = NULL;
    usb_storage_start_btn = NULL;
    usb_storage_stop_btn = NULL;
}
