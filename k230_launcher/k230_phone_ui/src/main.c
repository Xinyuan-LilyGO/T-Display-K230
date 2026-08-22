#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <lvgl/lvgl.h>
#include <lvgl/src/core/lv_refr.h>
#include <lvgl/src/debugging/sysmon/lv_sysmon.h>
#include <lvgl/src/drivers/display/drm/lv_linux_drm.h>
#include <lvgl/src/drivers/evdev/lv_evdev.h>
#include <lvgl/src/libs/lodepng/lodepng.h>
#if LV_USE_FFMPEG
#include <lvgl/src/libs/ffmpeg/lv_ffmpeg.h>
#endif
#include <png.h>
#include <v4l2-drm.h>

#include "ui_nrf9151_manager.h"
#include "ui_nrf52840_manager.h"

#ifndef LV_SYMBOL_WIFI
#define LV_SYMBOL_WIFI "WiFi"
#endif
#ifndef LV_SYMBOL_BLUETOOTH
#define LV_SYMBOL_BLUETOOTH "BT"
#endif
#ifndef LV_SYMBOL_GPS
#define LV_SYMBOL_GPS "GPS"
#endif
#ifndef LV_SYMBOL_BATTERY_FULL
#define LV_SYMBOL_BATTERY_FULL "BAT"
#endif
#ifndef LV_SYMBOL_BATTERY_3
#define LV_SYMBOL_BATTERY_3 LV_SYMBOL_BATTERY_FULL
#endif
#ifndef LV_SYMBOL_BATTERY_2
#define LV_SYMBOL_BATTERY_2 LV_SYMBOL_BATTERY_FULL
#endif
#ifndef LV_SYMBOL_BATTERY_1
#define LV_SYMBOL_BATTERY_1 LV_SYMBOL_BATTERY_FULL
#endif
#ifndef LV_SYMBOL_BATTERY_EMPTY
#define LV_SYMBOL_BATTERY_EMPTY LV_SYMBOL_BATTERY_FULL
#endif

#define TOUCH_MAX_X 1060
#define TOUCH_MAX_Y 2400
#define SCREEN_W 568
#define SCREEN_H 1232
#define TOUCH_CANVAS_X 24
#define TOUCH_CANVAS_Y 318
#define TOUCH_CANVAS_W 520
#define TOUCH_CANVAS_H 700
#define TOUCH_TRAIL_POINTS 32
#define TOUCH_MULTI_TRAIL_POINTS 8
#define TOUCH_TRACE_PATH "/tmp/k230_touch_trace.log"
#define TOUCH_TRACE_UI_WINDOW_US 3000000ULL
#define TOUCH_TRACE_SLOW_US 50000ULL
#define TOUCH_TRACE_RAW_LOG_INTERVAL_US 100000ULL
#define TOUCH_DRAW_INTERVAL_US 16000ULL
#define INPUT_BITS_PER_LONG ((int)(sizeof(unsigned long) * 8U))
#define INPUT_ABS_BITS_LEN ((ABS_MAX / INPUT_BITS_PER_LONG) + 1)
#define DISPLAY_REFR_PERIOD_MS 16
#define PORTRAIT_SCROLL_REPAIR_INTERVAL_US 12000ULL
#define PORTRAIT_SCROLL_REPAIR_DEFAULT_PERCENT 38
#define PORTRAIT_SCROLL_REPAIR_EXTRA_PX 24
#define PAGE_TRANSITION_ANIM_MS 180
#define PAGE_TRANSITION_MODE_COUNT 6
#define MOTION_BAR_COUNT 8
#define MOTION_BOX_COUNT 4
#define CAMERA_CAPTURE_BIN "/root/app/k230_phone_ui/k230_camera_capture"
#define CAMERA_PHOTO_DIR "/root/photos"
#define SCREENSHOT_DIR "/root/screenshots"
#define SCREENSHOT_LOG "/tmp/k230_screenshot.log"
#define REBOOT_DIAG_LOG "/tmp/k230_reboot_diag.log"
#define CAMERA_CAPTURE_DEVICE 1
#define CAMERA_CAPTURE_W 1920
#define CAMERA_CAPTURE_H 1080
#define CAMERA_PREVIEW_W 640
#define CAMERA_PREVIEW_H 360
#define CAMERA_STORED_THUMB_W 360
#define CAMERA_STORED_THUMB_H 640
#define CAMERA_STORED_THUMB_BYTES (CAMERA_STORED_THUMB_W * CAMERA_STORED_THUMB_H * 2)
#define CAMERA_PREVIEW_VIEW_PORTRAIT_W 520
#define CAMERA_PREVIEW_VIEW_PORTRAIT_H 890
#define CAMERA_PREVIEW_VIEW_MAX_W 928
#define CAMERA_PREVIEW_VIEW_MAX_H 890
#define CAMERA_PREVIEW_VIEW_BYTES (CAMERA_PREVIEW_VIEW_MAX_W * CAMERA_PREVIEW_VIEW_MAX_H * 2)
#define CAMERA_GALLERY_VIEW_MAX_W 1232
#define CAMERA_GALLERY_VIEW_MAX_H 1178
#define CAMERA_GALLERY_VIEW_BYTES (CAMERA_GALLERY_VIEW_MAX_W * CAMERA_GALLERY_VIEW_MAX_H * 2)
#define CAMERA_GALLERY_MAX_ITEMS 24
#define CAMERA_GALLERY_THUMB_W 160
#define CAMERA_GALLERY_THUMB_H 120
#define CAMERA_GALLERY_THUMB_BYTES (CAMERA_GALLERY_THUMB_W * CAMERA_GALLERY_THUMB_H * 2)
#define CAMERA_PREVIEW_INTERVAL_US 66000ULL
#define CAMERA_FACE_MAX_BOXES 4
#define CAMERA_FACE_GRID_MAX_W 80
#define CAMERA_FACE_GRID_MAX_H 80
#define CAMERA_FACE_GRID_MAX_CELLS (CAMERA_FACE_GRID_MAX_W * CAMERA_FACE_GRID_MAX_H)
#define CAMERA_FACE_DETECT_INTERVAL_US 700000ULL
#define NET_WIFI_IFACE "wlan0"
#define NET_ETH_IFACE "eth0"
#define STATUS_BAR_H 54
#define STATUS_BAR_SAFE_SIDE 30
#define STATUS_BAR_ITEM_W 32
#define STATUS_BAR_ITEM_H 30
#define STATUS_BAR_ITEM_GAP 6
#define STATUS_BAR_AUDIO_W 46
#define STATUS_BAR_AUDIO_ICON_PX 24
#define STATUS_BAR_LTE_W 40
#define STATUS_BAR_BATTERY_W 74
#define STATUS_BATTERY_REFRESH_US 10000000ULL
#define NET_MAX_APS 6
#define NET_SSID_MAX 64
#define NET_PASS_MAX 64
#define NET_STATUS_MAX 160
#define NET_WIFI_CONF "/tmp/k230_wifi_ui.conf"
#define NET_WIFI_CONNECT_LOG "/tmp/k230_wifi_connect.log"
#define NET_WIFI_SCAN_LOG "/tmp/k230_wifi_scan.log"
#define BACKLIGHT_PATH_MAX 160
#define BACKLIGHT_MIN_VALUE 20
#define DISPLAY_ORIENTATION_PORTRAIT "portrait"
#define DISPLAY_ORIENTATION_LANDSCAPE "landscape"
#define DISPLAY_ROTATION_ENV "K230_DISPLAY_ROTATION"
#define DISPLAY_ORIENTATION_ENV "K230_DISPLAY_ORIENTATION"
#define DISPLAY_ROTATION_RESTART_ENV "K230_DISPLAY_ROTATION_RESTART"
#define DISPLAY_ROTATION_PREF_KEY "display.rotation"
#define DISPLAY_BRIGHTNESS_PREF_KEY "display.brightness"
#define PAGE_TRANSITION_PREF_KEY "display.page_transition"
#define DISPLAY_TIMEOUT_PREF_KEY "display.timeout_s"
#define DISPLAY_TIMEOUT_DEFAULT_S 60
#define DISPLAY_TIMEOUT_MODE_COUNT 5
#define PREF_AUDIO_OUTPUT "audio.output"
#define AUDIO_OUTPUT_HEADPHONES "headphones"
#define AUDIO_OUTPUT_EXTERNAL "external"
#define STATUS_AUDIO_EARPHONE_ICON "/root/app/k230_phone_ui/icons/status/earphone.png"
#define STATUS_AUDIO_SPEAKER_ICON "/root/app/k230_phone_ui/icons/status/speaker.png"
#define PHONE_UI_BIN "/root/app/k230_phone_ui/k230_phone_ui"
#define EDGE_BACK_LOG_PATH "/tmp/k230_edge_back.log"
#define EDGE_BACK_START_PX 36
#define EDGE_BACK_TRIGGER_PX 120
#define EDGE_BACK_MAX_VERTICAL_PX 96
#define POWER_KEY_NAME "K230 PMU Power Key"
#define POWER_SHUTDOWN_PREVIEW_DELAY_US 800000ULL
#define POWER_SHUTDOWN_PROGRESS_US 3400000ULL
#define POWER_SHUTDOWN_FADE_US 700000ULL
#define POWER_SHUTDOWN_CANCEL_LIMIT_US 5000000ULL
#define POWER_SHUTDOWN_UPDATE_US 80000ULL
#ifndef K230_R58_COMPACT_SWITCH_TEST
#define K230_R58_COMPACT_SWITCH_TEST 0
#endif
#ifndef K230_R59_PERSISTENT_PANEL_UI
#define K230_R59_PERSISTENT_PANEL_UI 1
#endif
#ifndef K230_HIDE_KEYBOARD_BASE_APPS_UNTIL_DETECTED
#define K230_HIDE_KEYBOARD_BASE_APPS_UNTIL_DETECTED 0
#endif
#ifndef K230_CAMERA_FACE_KPU
#define K230_CAMERA_FACE_KPU 0
#endif
#ifndef K230_ENABLE_DEV_APPS
#define K230_ENABLE_DEV_APPS 0
#endif

#include "ui_common.h"
#include "ui_icons.h"
#if K230_CAMERA_FACE_KPU
#include "camera_face_detect.h"
#endif
#include "ui_ai_demo.h"
#include "ui_audio.h"
#include "ui_ble.h"
#include "ui_cellular.h"
#include "ui_display_test.h"
#include "ui_ethernet.h"
#include "ui_hardware.h"
#include "ui_halow.h"
#include "ui_hdmi_test.h"
#include "ui_i2c_scan.h"
#include "ui_i2s_test.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_multitouch.h"
#include "ui_language.h"
#include "ui_lora.h"
#include "ui_meshtastic.h"
#include "ui_mic_spectrum.h"
#include "ui_nes.h"
#include "ui_nrf52840_dfu.h"
#include "ui_prefs.h"
#include "ui_rtsp.h"
#include "ui_settings.h"
#include "ui_terminal.h"
#include "ui_time_settings.h"
#include "ui_usb_modem.h"
#include "ui_usb_storage.h"
#include "ui_video_player.h"
#include "ui_wifi.h"
#include "ui_wifi_iperf.h"

typedef struct {
    const char *title;
    const char *symbol;
    uint32_t color;
    page_id_t page;
} app_item_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char security[32];
    char quality[24];
    char signal[32];
    int encrypted;
} wifi_ap_info_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char password[NET_PASS_MAX];
} wifi_connect_request_t;

typedef struct {
    char photo[192];
    char thumb[192];
    time_t mtime;
} camera_gallery_item_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int score;
} camera_face_box_t;

static volatile int running = 1;
static lv_indev_t *evdev_indev;
static lv_indev_read_cb_t evdev_original_read_cb;
static char input_path[64];
static page_id_t current_page = PAGE_HOME;
static page_id_t page_stack[8];
static int page_stack_len;
static lv_obj_t *app_screen;
static lv_obj_t *ui_stage_obj;
static lv_obj_t *page_root;
static lv_obj_t *status_bar_obj;
static lv_obj_t *status_group_obj;
static lv_obj_t *status_audio_item_obj;
static lv_obj_t *status_audio_earphone_img;
static lv_obj_t *status_audio_speaker_img;
static lv_obj_t *status_location_label;
static lv_obj_t *status_lte_bars[4];
static lv_obj_t *status_lte_x_label;
static lv_obj_t *status_wifi_item_obj;
static lv_obj_t *status_wifi_label;
static lv_obj_t *status_ble_label;
static lv_obj_t *status_battery_shell_obj;
static lv_obj_t *status_battery_fill_obj;
static lv_obj_t *status_battery_tip_obj;
static lv_obj_t *status_battery_percent_label;
static lv_obj_t *transition_old_page;
static int page_transition_active;
static lv_obj_t *time_label;
static lv_obj_t *home_time_label;
static lv_obj_t *date_label;
static lv_obj_t *home_temp_value_label;
static lv_obj_t *home_power_source_value_label;
static lv_obj_t *home_power_draw_value_label;
static lv_obj_t *home_eth_title_label;
static lv_obj_t *home_eth_value_label;
static lv_obj_t *home_temp_badge;
static lv_obj_t *home_power_source_badge;
static lv_obj_t *home_power_draw_badge;
static lv_obj_t *home_eth_badge;
static lv_obj_t *home_eth_badge_text_label;
static lv_obj_t *home_apps_scroll;
static uint64_t home_telemetry_last_us;
static lv_timer_t *home_telemetry_timer;
static pthread_mutex_t home_telemetry_lock = PTHREAD_MUTEX_INITIALIZER;
static int home_telemetry_worker_active;
static int home_telemetry_cache_valid;
static unsigned int home_telemetry_generation;
static unsigned int home_telemetry_displayed_generation;
static char home_temp_cache[64] = "--";
static char home_power_source_cache[64] = "--";
static char home_power_draw_cache[64] = "--";
static char home_eth_title_cache[32] = "Network IP";
static char home_eth_symbol_cache[8] = "NET";
static char home_eth_cache[64] = "--";
static uint32_t home_temp_cache_color = 0x9AA4AF;
static uint32_t home_power_source_cache_color = 0x9AA4AF;
static uint32_t home_power_draw_cache_color = 0x9AA4AF;
static uint32_t home_eth_cache_color = 0x9AA4AF;
static int32_t home_saved_scroll_y;
static int home_saved_scroll_valid;
static int home_restore_scroll_on_create;
static lv_obj_t *touch_area;
static lv_obj_t *touch_marker;
static lv_obj_t *touch_trail[TOUCH_TRAIL_POINTS];
static lv_obj_t *touch_multi_marker[UI_MULTITOUCH_MAX_POINTS];
static lv_obj_t *touch_multi_marker_label[UI_MULTITOUCH_MAX_POINTS];
static lv_obj_t *touch_multi_trail[UI_MULTITOUCH_MAX_POINTS][TOUCH_MULTI_TRAIL_POINTS];
static lv_obj_t *touch_state_label;
static lv_obj_t *touch_xy_label;
static lv_obj_t *touch_count_label;
static lv_obj_t *touch_latency_label;
static lv_obj_t *touch_mt_status_label;
static lv_obj_t *compact_title_label;
static lv_obj_t *compact_state_label;
static lv_obj_t *compact_detail_label;
static lv_obj_t *compact_hint_label;
static lv_obj_t *compact_accent;
static lv_obj_t *r59_title_label;
static lv_obj_t *r59_state_label;
static lv_obj_t *r59_detail_label;
static lv_obj_t *r59_accent;
static lv_obj_t *r59_info_name[4];
static lv_obj_t *r59_info_value[4];
static lv_obj_t *r59_action_label[2];
static lv_obj_t *motion_rate_label;
static lv_obj_t *motion_bars[MOTION_BAR_COUNT];
static lv_obj_t *motion_boxes[MOTION_BOX_COUNT];
static lv_obj_t *camera_preview_canvas;
static lv_obj_t *camera_preview_hint_label;
static lv_obj_t *camera_status_label;
static lv_obj_t *camera_meta_label;
static lv_obj_t *camera_last_label;
static lv_obj_t *camera_gallery_canvas;
static lv_obj_t *camera_gallery_hint_label;
static lv_obj_t *camera_flip_h_btn;
static lv_obj_t *camera_flip_v_btn;
static lv_obj_t *camera_rtsp_notice_overlay;
static lv_obj_t *network_wifi_state_label;
static lv_obj_t *network_wifi_ip_label;
static lv_obj_t *network_eth_state_label;
static lv_obj_t *network_eth_ip_label;
static lv_obj_t *network_status_label;
static lv_obj_t *network_selected_label;
static lv_obj_t *network_ap_btn[NET_MAX_APS];
static lv_obj_t *network_ap_title[NET_MAX_APS];
static lv_obj_t *network_ap_meta[NET_MAX_APS];
static lv_obj_t *network_password_ta;
static lv_obj_t *display_brightness_label;
static lv_obj_t *display_backlight_label;
static lv_obj_t *display_orientation_label;
static lv_obj_t *display_rotation_btn[4];
static lv_obj_t *display_font_label;
static lv_obj_t *display_font_btn[3];
static lv_obj_t *display_transition_label;
static lv_obj_t *display_transition_btn[PAGE_TRANSITION_MODE_COUNT];
static lv_obj_t *display_timeout_label;
static lv_obj_t *display_timeout_btn[DISPLAY_TIMEOUT_MODE_COUNT];
static lv_obj_t *display_keyboard_auto_rotate_switch;
static lv_display_t *main_display;
static int32_t motion_bar_travel[MOTION_BAR_COUNT];
static int32_t motion_bar_x[MOTION_BAR_COUNT];
static int8_t motion_bar_dir[MOTION_BAR_COUNT];
static int32_t motion_box_angle[MOTION_BOX_COUNT];
static lv_timer_t *touch_timer;
static lv_timer_t *network_timer;
static uint32_t touch_sample_count;
static pthread_mutex_t touch_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t touch_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t network_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t touch_trace_thread;
static FILE *touch_trace_fp;
static volatile int touch_trace_running;
static int touch_trace_verbose;
static uint64_t app_start_us;
static uint64_t last_ui_action_us;
static uint64_t last_render_start_us;
static uint64_t last_flush_start_us;
static uint64_t last_flush_wait_start_us;
static uint64_t last_raw_syn_app_us;
static uint64_t last_raw_syn_kernel_us;
static uint64_t last_raw_lag_us;
static uint32_t last_raw_seq;
static int32_t last_raw_x = -1;
static int32_t last_raw_y = -1;
static int32_t last_raw_screen_x = -1;
static int32_t last_raw_screen_y = -1;
static int last_raw_pressed;
static uint64_t last_touch_draw_us;
static int32_t last_touch_draw_x = -1;
static int32_t last_touch_draw_y = -1;
static int last_touch_label_state = -1;
static int32_t last_touch_label_x = -1;
static int32_t last_touch_label_y = -1;
static uint32_t last_touch_label_raw_seq;
static uint32_t touch_trail_index;
static uint32_t touch_multi_trail_index[UI_MULTITOUCH_MAX_POINTS];
static int32_t touch_multi_last_x[UI_MULTITOUCH_MAX_POINTS];
static int32_t touch_multi_last_y[UI_MULTITOUCH_MAX_POINTS];
static int32_t touch_canvas_x = TOUCH_CANVAS_X;
static int32_t touch_canvas_y = TOUCH_CANVAS_Y;
static int32_t touch_canvas_w = TOUCH_CANVAS_W;
static int32_t touch_canvas_h = TOUCH_CANVAS_H;
static uint64_t motion_start_us;
static uint64_t motion_last_frame_us;
static uint64_t motion_frame_sum_us;
static uint64_t motion_frame_min_us;
static uint64_t motion_frame_max_us;
static uint64_t motion_last_stats_us;
static uint32_t motion_frame_count;
static uint32_t motion_frame_dt_count;
static uint32_t motion_update_count;
static uint8_t camera_stored_thumb_buf[CAMERA_STORED_THUMB_BYTES];
static uint8_t camera_preview_buf[CAMERA_PREVIEW_VIEW_BYTES];
static uint8_t camera_preview_frame_buf[CAMERA_PREVIEW_VIEW_BYTES];
static uint8_t camera_gallery_buf[CAMERA_GALLERY_VIEW_BYTES];
static uint8_t camera_gallery_thumb_buf[CAMERA_GALLERY_MAX_ITEMS][CAMERA_GALLERY_THUMB_BYTES];
static camera_gallery_item_t camera_gallery_items[CAMERA_GALLERY_MAX_ITEMS];
static int camera_gallery_count;
static int camera_gallery_selected;
static int camera_gallery_view_open;
static int32_t camera_gallery_press_x;
static int32_t camera_gallery_press_y;
static pthread_mutex_t camera_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t camera_preview_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t camera_face_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t camera_face_cond = PTHREAD_COND_INITIALIZER;
static lv_timer_t *camera_timer;
static pthread_t camera_preview_thread;
static pthread_t camera_face_thread;
static int camera_preview_thread_valid;
static int camera_face_thread_valid;
static int camera_face_pending;
static int camera_face_busy;
static int camera_face_stop;
static uint8_t *camera_face_request_buf;
static unsigned camera_face_request_w;
static unsigned camera_face_request_h;
static unsigned camera_face_result_w;
static unsigned camera_face_result_h;
static uint64_t camera_face_last_submit_us;
static camera_face_box_t camera_face_boxes[CAMERA_FACE_MAX_BOXES];
static volatile int camera_preview_stop;
static volatile int camera_busy;
static volatile int camera_result_ready;
static int camera_preview_frame_ready;
static int camera_preview_have_frame;
static int camera_preview_active;
static int camera_preview_last_result;
static uint32_t camera_preview_frame_count;
static int camera_face_count;
static int camera_preview_flip_h;
static int camera_preview_flip_v;
static int camera_last_result = -1;
static char camera_status_text[128] = "Ready";
static char camera_preview_status_text[128] = "Preview idle";
static char camera_last_photo_path[192];
static char camera_last_thumb_path[192];
static wifi_ap_info_t network_aps[NET_MAX_APS];
static int network_ap_count;
static int network_selected_ap = -1;
static int network_scan_busy;
static int network_connect_busy;
static int network_result_ready;
static char network_status_text[NET_STATUS_MAX] = "Tap Scan to search WiFi";
static char network_selected_ssid[NET_SSID_MAX];
static char backlight_brightness_path[BACKLIGHT_PATH_MAX];
static char backlight_max_path[BACKLIGHT_PATH_MAX];
static int backlight_max_value;
static int backlight_current_value;
static char display_orientation_value[16] = "0";
static int display_rotation_degrees;
static char page_transition_effect[16] = "off";
static int display_timeout_loaded;
static int display_timeout_s = DISPLAY_TIMEOUT_DEFAULT_S;
static pthread_mutex_t display_idle_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t display_last_activity_us;
static int display_idle_dimmed;
static int display_idle_wake_pending;
static int display_idle_saved_screen = -1;
static int display_idle_saved_keyboard = -1;
static int edge_back_pref_loaded;
static int edge_back_enabled;
static int edge_back_tracking;
static lv_point_t edge_back_start_point;
static int edge_back_direction;
static uint64_t edge_back_lvgl_suppress_until_us;
static int edge_back_raw_tracking;
static lv_point_t edge_back_raw_start_point;
static int edge_back_raw_direction;
static volatile int edge_back_raw_pending;
static volatile int edge_back_raw_pending_page;
static pthread_mutex_t edge_back_raw_state_lock = PTHREAD_MUTEX_INITIALIZER;
static int edge_back_raw_pending_direction;
static int edge_back_raw_pending_y;
static int edge_back_raw_hint_active;
static int edge_back_raw_hint_direction;
static int edge_back_raw_hint_y;
static int edge_back_raw_hint_distance;
static uint32_t edge_back_raw_hint_generation;
static lv_obj_t *edge_back_hint_obj;
static lv_obj_t *edge_back_hint_label;
static int edge_back_hint_opa;
static int settings_subpage_context;
static pthread_t power_key_thread;
static int power_key_thread_started;
static volatile int power_key_thread_stop;
static pthread_mutex_t power_key_lock = PTHREAD_MUTEX_INITIALIZER;
static int power_key_pressed;
static uint64_t power_key_press_us;
static unsigned int power_key_generation;
static char power_key_device_path[64];
static lv_obj_t *shutdown_overlay_obj;
static lv_obj_t *shutdown_progress_arc;
static lv_obj_t *shutdown_detail_label;
static lv_obj_t *shutdown_hint_label;
static lv_obj_t *screenshot_toast_obj;
static lv_obj_t *entry_block_dialog;
static lv_obj_t *entry_probe_dialog;
static lv_timer_t *entry_probe_timer;
static pthread_mutex_t entry_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static int entry_probe_active;
static int entry_probe_done;
static int entry_probe_allowed;
static page_id_t entry_probe_page;
static char entry_probe_title[128];
static char entry_probe_reason[192];
static lv_obj_t *reboot_status_label;
static lv_obj_t *reboot_confirm_btn;
static int reboot_confirm_started;
static int shutdown_visual_active;
static int shutdown_visual_committed;
static int shutdown_visual_poweroff_started;
static uint64_t shutdown_visual_start_us;
static uint64_t shutdown_visual_commit_us;
static uint64_t shutdown_visual_last_update_us;
static int shutdown_saved_screen_backlight;
static int shutdown_saved_keyboard_backlight;
static int shutdown_last_progress = -1;
static int shutdown_last_fade_log_progress = -1;
static char status_wifi_state[24] = "on";
static char status_ble_state[24] = "offline";
static int status_wifi_visible = 1;
static pthread_mutex_t status_hw_lock = PTHREAD_MUTEX_INITIALIZER;
static int status_battery_cache_valid;
static int status_battery_available_cache;
static int status_battery_soc_cache = -1;
static int status_battery_charging_cache;
static int status_battery_charge_done_cache;
static int status_battery_refresh_busy;
static uint64_t status_battery_next_refresh_us;
static uint64_t touch_block_last_log_us;

static lv_style_t style_panel;
static lv_style_t style_text_primary;
static lv_style_t style_text_secondary;
static lv_style_t style_chip;
static lv_style_t style_button;
static lv_style_t style_button_pressed;
static lv_style_t style_icon;
static int styles_ready;

static const app_item_t app_items[] = {
    {"Camera", LV_SYMBOL_VIDEO, 0x3DA5FF, PAGE_CAMERA},
    {"Music", LV_SYMBOL_AUDIO, 0x2563EB, PAGE_MUSIC},
    {"Video", LV_SYMBOL_VIDEO, 0xF43F5E, PAGE_VIDEO},
    {"Radio", LV_SYMBOL_VOLUME_MAX, 0x25C281, PAGE_NET_RADIO},
    {"I2S Test", "I2S", 0x14B8A6, PAGE_I2S_TEST},
    {"I2C Scan", "I2C", 0x22C55E, PAGE_I2C_SCAN},
    {"Battery", "BAT", 0xA3E635, PAGE_BATTERY},
    {"Keyboard", "KEY", 0xF97316, PAGE_KEYBOARD_TEST},
    {"LED Test", "LED", 0x22D3EE, PAGE_XL9555_TEST},
    {"Cellular", "LTE", 0x60A5FA, PAGE_CELLULAR},
    {"USB Modem", "5G", 0x38BDF8, PAGE_USB_MODEM},
    {"Terminal", ">_", 0xCBD5E1, PAGE_TERMINAL},
    {"Record", LV_SYMBOL_STOP, 0xEF4D5A, PAGE_RECORDER},
    {"Mic FFT", LV_SYMBOL_BARS, 0x22D3EE, PAGE_MIC_FFT},
    {"LoRa", LV_SYMBOL_UPLOAD, 0x7C3AED, PAGE_LORA},
    {"Meshtastic", "MESH", 0x10B981, PAGE_MESHTASTIC},
#if K230_ENABLE_DEV_APPS
    {"LoRa FLRC", "FLRC", 0xA855F7, PAGE_LORA_FLRC},
    {"Halow", "Ha", 0x25C281, PAGE_HALOW},
#endif
    {"LoRaWAN", "WAN", 0x14B8A6, PAGE_LORAWAN},
    {"NES", "NES", 0xF97316, PAGE_NES},
    {"AI", LV_SYMBOL_BARS, 0xFF6B6B, PAGE_AI},
    {"RTSP", LV_SYMBOL_VIDEO, 0x22C55E, PAGE_RTSP},
    {"Wi-Fi", "WiFi", 0x25C281, PAGE_WIFI},
#if K230_ENABLE_DEV_APPS
    {"WiFi Test", "iperf", 0x10B981, PAGE_WIFI_IPERF},
#endif
    {"Bluetooth", "BT", 0x3B82F6, PAGE_BLE},
    {"nRF DFU", "DFU", 0x3B82F6, PAGE_NRF52840_DFU},
    {"MTP", "MTP", 0x41C7C7, PAGE_FILES},
    {"Gallery", LV_SYMBOL_IMAGE, 0xEC4899, PAGE_GALLERY},
    {"Screenshot", LV_SYMBOL_IMAGE, 0x22C55E, PAGE_SCREENSHOT},
    {"Settings", LV_SYMBOL_SETTINGS, 0x94A3B8, PAGE_SETTINGS},
    {"System", LV_SYMBOL_LIST, 0xF5A524, PAGE_SYSTEM},
    {"Display", LV_SYMBOL_EYE_OPEN, 0x8B5CF6, PAGE_DISPLAY},
    {"Screen Test", "RGB", 0x22C55E, PAGE_DISPLAY_TEST},
    {"HDMI", "HDMI", 0x60A5FA, PAGE_HDMI_TEST},
    {"Touch", "TCH", 0x22D3EE, PAGE_TOUCH_TEST},
    {"Motion", LV_SYMBOL_PLAY, 0xF97316, PAGE_MOTION},
    {"About", LV_SYMBOL_WARNING, 0xA3E635, PAGE_ABOUT},
    {"Reboot", LV_SYMBOL_POWER, 0xEF4D5A, PAGE_REBOOT},
};

static void render_page(page_id_t page, lv_screen_load_anim_t anim_type,
                        uint32_t anim_ms);
static void cleanup_page_state(void);
static int camera_gallery_handle_back(void);
static void edge_back_load_pref(void);
static void edge_back_event_cb(lv_event_t *event);
static int display_logical_width(void);
static int display_logical_height(void);
static void load_runtime_display_orientation(void);
static void apply_display_orientation(lv_display_t *disp);
static void apply_ui_stage_transform(void);
static void style_fullscreen_root(lv_obj_t *obj);
static int display_drm_rotation_from_orientation(void);
static void restart_for_display_orientation(const char *value);
static void apply_display_rotation_change(int old_degrees, const char *reason);
static void show_rotation_startup_cover(lv_display_t *disp);
static void display_orientation_event_cb(lv_event_t *event);
static void display_update_orientation_controls(void);
static void page_transition_load_pref(void);
static int page_transition_enabled(void);
static void display_update_transition_controls(void);
static void motion_reset_stats(uint64_t now);
static void motion_step_frame_locked(uint64_t now);
static void network_set_status_locked(const char *fmt, ...);
static void *wifi_scan_thread_cb(void *arg);
static void *wifi_connect_thread_cb(void *arg);
static void update_home_telemetry_labels(int force);
static void request_fast_refresh(void);
static void start_power_key_monitor(void);
static void stop_power_key_monitor(void);
static void power_key_shutdown_visual_poll(void);
static void status_bar_relayout(void);

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t input_event_us(const struct input_event *event)
{
    return (uint64_t)event->input_event_sec * 1000000ULL +
           (uint64_t)event->input_event_usec;
}

static const char *page_name(page_id_t page)
{
    switch(page) {
    case PAGE_HOME:
        return "Home";
    case PAGE_CAMERA:
        return "Camera";
    case PAGE_NETWORK:
        return "Network";
    case PAGE_WIFI:
        return "Wi-Fi";
    case PAGE_WIFI_IPERF:
        return "WiFi iperf";
    case PAGE_ETHERNET:
        return "Ethernet";
    case PAGE_BLE:
        return "Bluetooth";
    case PAGE_NRF52840_DFU:
        return "nRF52840 DFU";
    case PAGE_MUSIC:
        return "Music";
    case PAGE_VIDEO:
        return "Video";
    case PAGE_NET_RADIO:
        return "Radio";
    case PAGE_RECORDER:
        return "Recorder";
    case PAGE_MIC_FFT:
        return "Mic FFT";
    case PAGE_LORA:
        return "LoRa";
    case PAGE_MESHTASTIC:
        return "Meshtastic";
    case PAGE_LORA_FLRC:
        return "LoRa FLRC";
    case PAGE_HALOW:
        return "Halow";
    case PAGE_LORAWAN:
        return "LoRaWAN";
    case PAGE_NES:
        return "NES";
    case PAGE_SYSTEM:
        return "System";
    case PAGE_DISPLAY:
        return "Display";
    case PAGE_DISPLAY_TEST:
        return "Screen Test";
    case PAGE_LANGUAGE:
        return "Language";
    case PAGE_TIME:
        return "Date & time";
    case PAGE_AUDIO_SETTINGS:
        return "Audio";
    case PAGE_AUDIO_OUTPUT:
        return "Audio output";
    case PAGE_NOTIFICATION_SETTINGS:
        return "Notifications";
    case PAGE_APP_STARTUP:
        return "Startup apps";
    case PAGE_I2S_TEST:
        return "I2S Test";
    case PAGE_I2C_SCAN:
        return "I2C Scan";
    case PAGE_HDMI_TEST:
        return "HDMI";
    case PAGE_FAN:
        return "Fan";
    case PAGE_SENSORS:
        return "Sensors";
    case PAGE_BQ25896:
        return "Charger";
    case PAGE_BATTERY:
        return "Battery";
    case PAGE_KEYBOARD_SETTINGS:
        return "Keyboard settings";
    case PAGE_KEYBOARD_HOTKEYS:
        return "F-key hotkeys";
    case PAGE_KEYBOARD_HOTKEY_ACTION:
        return "F-key action";
    case PAGE_KEYBOARD_TEST:
        return "Keyboard";
    case PAGE_BUTTON_TEST:
        return "Buttons";
    case PAGE_INT0_TEST:
        return "INT0 Test";
    case PAGE_XL9555_TEST:
        return "LED Test";
    case PAGE_CELLULAR:
        return "Cellular";
    case PAGE_USB_MODEM:
        return "USB Modem";
    case PAGE_FILES:
        return "MTP";
    case PAGE_AI:
        return "AI";
    case PAGE_RTSP:
        return "RTSP";
    case PAGE_TERMINAL:
        return "Terminal";
    case PAGE_ABOUT:
        return "About";
    case PAGE_GALLERY:
        return "Gallery";
    case PAGE_SCREENSHOT:
        return "Screenshot";
    case PAGE_TOUCH_TEST:
        return "Touch";
    case PAGE_MOTION:
        return "Motion";
    case PAGE_SETTINGS:
        return "Settings";
    case PAGE_LOGS:
        return "Logs";
    case PAGE_REBOOT:
        return "Reboot";
    default:
        return "Unknown";
    }
}

static int page_uses_lora_radio(page_id_t page)
{
    return page == PAGE_LORA ||
           page == PAGE_LORA_FLRC ||
           page == PAGE_LORAWAN ||
           page == PAGE_CELLULAR;
}

static void touch_trace_log(const char *fmt, ...)
{
    va_list ap;
    char msg[384];
    uint64_t now = monotonic_us();
    double since_start_ms = app_start_us ? (double)(now - app_start_us) / 1000.0 : 0.0;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&touch_log_lock);
    if(!touch_trace_fp) {
        touch_trace_fp = fopen(TOUCH_TRACE_PATH, "a");
    }
    if(touch_trace_fp) {
        fprintf(touch_trace_fp, "%10.3fms %s\n", since_start_ms, msg);
        fflush(touch_trace_fp);
    }
    if(strncmp(msg, "RAW ", 4) != 0) {
        fprintf(stderr, "[touch-trace %10.3fms] %s\n", since_start_ms, msg);
        fflush(stderr);
    }
    pthread_mutex_unlock(&touch_log_lock);
}

static void edge_back_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    char msg[384];
    uint64_t now = monotonic_us();
    double since_start_ms = app_start_us ?
                            (double)(now - app_start_us) / 1000.0 : 0.0;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&touch_log_lock);
    fp = fopen(EDGE_BACK_LOG_PATH, "a");
    if(fp) {
        fprintf(fp, "%10.3fms %s\n", since_start_ms, msg);
        fclose(fp);
    }
    pthread_mutex_unlock(&touch_log_lock);

    touch_trace_log("EDGE_BACK_%s", msg);
}

static const char *edge_back_edge_name(int direction)
{
    if(direction > 0) {
        return "left";
    }
    if(direction < 0) {
        return "right";
    }
    return "none";
}

static int edge_back_direction_for_x(int32_t x)
{
    int w = display_logical_width();

    if(x <= EDGE_BACK_START_PX) {
        return 1;
    }
    if(w > EDGE_BACK_START_PX && x >= w - EDGE_BACK_START_PX) {
        return -1;
    }
    return 0;
}

static int edge_back_is_near_edge(int32_t x)
{
    int w = display_logical_width();

    return x <= EDGE_BACK_START_PX * 2 ||
           (w > EDGE_BACK_START_PX * 2 &&
            x >= w - EDGE_BACK_START_PX * 2);
}

static void edge_back_raw_hint_publish(int active, int direction, int y,
                                       int distance)
{
    pthread_mutex_lock(&edge_back_raw_state_lock);
    edge_back_raw_hint_active = active;
    edge_back_raw_hint_direction = active ? direction : 0;
    edge_back_raw_hint_y = y;
    edge_back_raw_hint_distance = distance > 0 ? distance : 0;
    edge_back_raw_hint_generation++;
    pthread_mutex_unlock(&edge_back_raw_state_lock);
}

static void edge_back_raw_mark_pending(int page, int direction, int y)
{
    pthread_mutex_lock(&edge_back_raw_state_lock);
    edge_back_raw_pending_page = page;
    edge_back_raw_pending_direction = direction;
    edge_back_raw_pending_y = y;
    edge_back_raw_pending = 1;
    edge_back_raw_hint_active = 0;
    edge_back_raw_hint_generation++;
    pthread_mutex_unlock(&edge_back_raw_state_lock);
}

static void edge_back_raw_trace(uint32_t seq, int pressed, int32_t screen_x,
                                int32_t screen_y)
{
    static int raw_down;
    const char *reason = NULL;

    if(!edge_back_enabled) {
        reason = "disabled";
    } else if(current_page == PAGE_HOME) {
        reason = "home";
    } else if(page_transition_active) {
        reason = "transition";
    } else if(screen_x < 0 || screen_y < 0) {
        reason = "invalid_point";
    }

    if(reason) {
        if(raw_down && edge_back_raw_tracking) {
            edge_back_log("RAW_ABORT seq=%lu reason=%s page=%s edge=%s",
                          (unsigned long)seq, reason, page_name(current_page),
                          edge_back_edge_name(edge_back_raw_direction));
        }
        if(!pressed) {
            edge_back_raw_tracking = 0;
            edge_back_raw_direction = 0;
            edge_back_raw_hint_publish(0, 0, screen_y, 0);
        }
        raw_down = pressed;
        return;
    }

    if(pressed && !raw_down) {
        edge_back_raw_start_point.x = screen_x;
        edge_back_raw_start_point.y = screen_y;
        edge_back_raw_direction = edge_back_direction_for_x(screen_x);
        edge_back_raw_tracking = edge_back_raw_direction != 0;
        if(edge_back_raw_tracking) {
            edge_back_log("RAW_START seq=%lu page=%s edge=%s x=%ld y=%ld "
                          "start_px=%d width=%d",
                          (unsigned long)seq, page_name(current_page),
                          edge_back_edge_name(edge_back_raw_direction),
                          (long)screen_x, (long)screen_y,
                          EDGE_BACK_START_PX, display_logical_width());
            edge_back_raw_hint_publish(1, edge_back_raw_direction, screen_y, 0);
        } else if(edge_back_is_near_edge(screen_x)) {
            edge_back_log("RAW_NEAR_EDGE_SKIP seq=%lu page=%s x=%ld y=%ld "
                          "start_px=%d width=%d",
                          (unsigned long)seq, page_name(current_page),
                          (long)screen_x, (long)screen_y,
                          EDGE_BACK_START_PX, display_logical_width());
        }
    } else if(pressed && raw_down) {
        if(edge_back_raw_tracking) {
            int dx = screen_x - edge_back_raw_start_point.x;
            int dy = screen_y - edge_back_raw_start_point.y;
            int horizontal = edge_back_raw_direction * dx;

            if(abs(dy) > EDGE_BACK_MAX_VERTICAL_PX) {
                edge_back_log("RAW_HINT_ABORT seq=%lu page=%s edge=%s dx=%d dy=%d "
                              "max_dy=%d",
                              (unsigned long)seq, page_name(current_page),
                              edge_back_edge_name(edge_back_raw_direction),
                              dx, dy, EDGE_BACK_MAX_VERTICAL_PX);
                edge_back_raw_tracking = 0;
                edge_back_raw_direction = 0;
                edge_back_raw_hint_publish(0, 0, screen_y, 0);
            } else {
                edge_back_raw_hint_publish(1, edge_back_raw_direction,
                                           screen_y, horizontal);
            }
        }
    } else if(!pressed && raw_down) {
        if(edge_back_raw_tracking) {
            int dx = screen_x - edge_back_raw_start_point.x;
            int dy = screen_y - edge_back_raw_start_point.y;
            int horizontal = edge_back_raw_direction * dx;

            if(horizontal >= EDGE_BACK_TRIGGER_PX &&
               abs(dy) <= EDGE_BACK_MAX_VERTICAL_PX) {
                edge_back_raw_mark_pending((int)current_page,
                                           edge_back_raw_direction, screen_y);
                edge_back_log("RAW_TRIGGER seq=%lu page=%s edge=%s dx=%d dy=%d",
                              (unsigned long)seq, page_name(current_page),
                              edge_back_edge_name(edge_back_raw_direction),
                              dx, dy);
            } else {
                edge_back_log("RAW_CANCEL seq=%lu page=%s edge=%s dx=%d dy=%d "
                              "trigger=%d max_dy=%d",
                              (unsigned long)seq, page_name(current_page),
                              edge_back_edge_name(edge_back_raw_direction),
                              dx, dy, EDGE_BACK_TRIGGER_PX,
                              EDGE_BACK_MAX_VERTICAL_PX);
                edge_back_raw_hint_publish(0, 0, screen_y, 0);
            }
        }
        edge_back_raw_tracking = 0;
        edge_back_raw_direction = 0;
    }

    raw_down = pressed;
}

static int touch_trace_ui_active(uint64_t now)
{
    return last_ui_action_us && now >= last_ui_action_us &&
           now - last_ui_action_us < TOUCH_TRACE_UI_WINDOW_US;
}

static void trace_ui_action(const char *tag, page_id_t page)
{
    uint64_t now = monotonic_us();
    uint64_t raw_app_us;
    uint64_t raw_lag_us;
    uint32_t raw_seq;
    int32_t raw_x;
    int32_t raw_y;
    int32_t raw_screen_x;
    int32_t raw_screen_y;
    int raw_pressed;

    pthread_mutex_lock(&touch_state_lock);
    raw_app_us = last_raw_syn_app_us;
    raw_lag_us = last_raw_lag_us;
    raw_seq = last_raw_seq;
    raw_x = last_raw_x;
    raw_y = last_raw_y;
    raw_screen_x = last_raw_screen_x;
    raw_screen_y = last_raw_screen_y;
    raw_pressed = last_raw_pressed;
    pthread_mutex_unlock(&touch_state_lock);

    last_ui_action_us = now;

    if(raw_seq) {
        touch_trace_log("%s page=%s raw_seq=%lu raw_age=%.3fms raw_lag=%.3fms "
                        "raw=(%ld,%ld) screen=(%ld,%ld) state=%s",
                        tag, page_name(page), (unsigned long)raw_seq,
                        raw_app_us ? (double)(now - raw_app_us) / 1000.0 : -1.0,
                        (double)raw_lag_us / 1000.0, (long)raw_x, (long)raw_y,
                        (long)raw_screen_x, (long)raw_screen_y,
                        raw_pressed ? "down" : "up");
    } else {
        touch_trace_log("%s page=%s raw_seq=none", tag, page_name(page));
    }
}

static int display_orientation_is_landscape(void)
{
    return display_rotation_degrees == 90 || display_rotation_degrees == 270;
}

static int display_degrees_is_landscape(int degrees)
{
    return degrees == 90 || degrees == 270;
}

static int parse_display_rotation_degrees(const char *value)
{
    if(!value) {
        return 0;
    }
    if(strcmp(value, DISPLAY_ORIENTATION_LANDSCAPE) == 0 ||
       strcmp(value, "90") == 0 || strcmp(value, "90deg") == 0) {
        return 90;
    }
    if(strcmp(value, "180") == 0 || strcmp(value, "180deg") == 0) {
        return 180;
    }
    if(strcmp(value, "270") == 0 || strcmp(value, "270deg") == 0) {
        return 270;
    }
    return 0;
}

static void set_display_rotation_degrees(int degrees)
{
    switch(degrees) {
    case 90:
    case 180:
    case 270:
        display_rotation_degrees = degrees;
        break;
    default:
        display_rotation_degrees = 0;
        break;
    }
    snprintf(display_orientation_value, sizeof(display_orientation_value), "%d",
             display_rotation_degrees);
}

static int display_drm_rotation_from_orientation(void)
{
    return display_rotation_degrees / 90;
}

static void load_runtime_display_orientation(void)
{
    const char *value = getenv(DISPLAY_ROTATION_ENV);
    const char *source = "default";
    char pref_value[16];

    if(value) {
        source = "rotation-env";
    } else {
        value = getenv(DISPLAY_ORIENTATION_ENV);
        if(value) {
            source = "legacy-env";
        } else if(ui_prefs_get(DISPLAY_ROTATION_PREF_KEY, pref_value,
                               sizeof(pref_value), "0") == 0) {
            value = pref_value;
            source = "prefs";
        }
    }

    set_display_rotation_degrees(parse_display_rotation_degrees(value));
    touch_trace_log("DISPLAY_ROTATION_START degrees=%d source=%s persistent=on",
                    display_rotation_degrees, source);
}

static void save_runtime_display_orientation(void)
{
    int rc = ui_prefs_set(DISPLAY_ROTATION_PREF_KEY, display_orientation_value);

    touch_trace_log("DISPLAY_ROTATION_SAVE degrees=%d value=%s rc=%d",
                    display_rotation_degrees, display_orientation_value, rc);
}

int app_display_rotation_degrees(void)
{
    return display_rotation_degrees;
}

void app_set_display_rotation_degrees(int degrees)
{
    int old_degrees = display_rotation_degrees;

    set_display_rotation_degrees(degrees);
    save_runtime_display_orientation();
    touch_trace_log("DISPLAY_ROTATION_APP_SET degrees=%d persistent=on",
                    display_rotation_degrees);
    apply_display_rotation_change(old_degrees, "app");
}

static int page_transition_mode_valid(const char *mode)
{
    return mode && (strcmp(mode, "off") == 0 || strcmp(mode, "fade") == 0 ||
                    strcmp(mode, "slide_left") == 0 ||
                    strcmp(mode, "slide_right") == 0 ||
                    strcmp(mode, "slide_up") == 0 ||
                    strcmp(mode, "cover") == 0 ||
                    strcmp(mode, "rise") == 0 || strcmp(mode, "slide") == 0);
}

static const char *page_transition_normalize_mode(const char *mode)
{
    if(!mode) {
        return "off";
    }
    if(strcmp(mode, "rise") == 0) {
        return "slide_up";
    }
    if(strcmp(mode, "slide") == 0) {
        return "slide_left";
    }
    return page_transition_mode_valid(mode) ? mode : "off";
}

static void page_transition_load_pref(void)
{
    char value[16];

    ui_prefs_get(PAGE_TRANSITION_PREF_KEY, value, sizeof(value), "off");
    snprintf(page_transition_effect, sizeof(page_transition_effect), "%s",
             page_transition_normalize_mode(value));
}

static int page_transition_enabled(void)
{
    return strcmp(page_transition_effect, "off") != 0;
}

static void page_transition_save_pref(const char *mode)
{
    mode = page_transition_normalize_mode(mode);
    snprintf(page_transition_effect, sizeof(page_transition_effect), "%s",
             mode);
    ui_prefs_set(PAGE_TRANSITION_PREF_KEY, page_transition_effect);
    touch_trace_log("PAGE_TRANSITION_PREF mode=%s", page_transition_effect);
}

static int display_timeout_valid(int seconds)
{
    return seconds == 0 || seconds == 5 || seconds == 10 ||
           seconds == 30 || seconds == 60;
}

static void display_timeout_load_pref(void)
{
    char value[16];
    int seconds;

    if(display_timeout_loaded) {
        return;
    }
    ui_prefs_get(DISPLAY_TIMEOUT_PREF_KEY, value, sizeof(value), "60");
    seconds = atoi(value);
    if(!display_timeout_valid(seconds)) {
        seconds = DISPLAY_TIMEOUT_DEFAULT_S;
    }
    display_timeout_s = seconds;
    display_timeout_loaded = 1;
}

static void display_timeout_save_pref(int seconds)
{
    char value[16];

    if(!display_timeout_valid(seconds)) {
        seconds = DISPLAY_TIMEOUT_DEFAULT_S;
    }
    display_timeout_s = seconds;
    display_timeout_loaded = 1;
    snprintf(value, sizeof(value), "%d", seconds);
    ui_prefs_set(DISPLAY_TIMEOUT_PREF_KEY, value);
    touch_trace_log("DISPLAY_TIMEOUT_PREF seconds=%d", seconds);
    app_note_user_activity();
}

static const char *display_timeout_label_text(int seconds)
{
    switch(seconds) {
    case 5:
        return "5 sec";
    case 10:
        return "10 sec";
    case 30:
        return "30 sec";
    case 60:
        return "60 sec";
    case 0:
    default:
        return "Never";
    }
}

void app_note_user_activity(void)
{
    uint64_t now = monotonic_us();

    pthread_mutex_lock(&display_idle_lock);
    display_last_activity_us = now;
    if(display_idle_dimmed) {
        display_idle_wake_pending = 1;
        display_idle_dimmed = 0;
    }
    pthread_mutex_unlock(&display_idle_lock);
}

static int display_idle_is_dimmed(void)
{
    int dimmed;

    pthread_mutex_lock(&display_idle_lock);
    dimmed = display_idle_dimmed;
    pthread_mutex_unlock(&display_idle_lock);
    return dimmed;
}

static int touch_input_blocked(void)
{
    int idle_dimmed = display_idle_is_dimmed();
    int boot0_off = ui_hardware_boot0_screen_off();

    if((idle_dimmed || boot0_off) && ui_hardware_screen_backlight_get() > 0) {
        uint64_t now = monotonic_us();

        if(boot0_off) {
            ui_hardware_shutdown_backlights_apply(0, 0);
            touch_trace_log("DISPLAY_TIMEOUT_TOUCH_FORCE_OFF screen=%d",
                            ui_hardware_screen_backlight_get());
        } else if(idle_dimmed) {
            pthread_mutex_lock(&display_idle_lock);
            display_idle_dimmed = 0;
            display_idle_wake_pending = 0;
            display_last_activity_us = now;
            pthread_mutex_unlock(&display_idle_lock);
            touch_trace_log("DISPLAY_TIMEOUT_TOUCH_STATE_RECOVER screen=%d",
                            ui_hardware_screen_backlight_get());
        }
        boot0_off = ui_hardware_boot0_screen_off();
        idle_dimmed = display_idle_is_dimmed();
    }

    return idle_dimmed || boot0_off;
}

static void display_idle_poll(void)
{
    int timeout_s;
    int restore_pending;
    int saved_screen;
    int saved_keyboard;
    int should_dim = 0;
    uint64_t now = monotonic_us();
    uint64_t last_activity;

    display_timeout_load_pref();

    pthread_mutex_lock(&display_idle_lock);
    if(display_last_activity_us == 0) {
        display_last_activity_us = now;
    }
    timeout_s = display_timeout_s;
    restore_pending = display_idle_wake_pending;
    saved_screen = display_idle_saved_screen;
    saved_keyboard = display_idle_saved_keyboard;
    if(restore_pending) {
        display_idle_wake_pending = 0;
    }
    last_activity = display_last_activity_us;
    if(!restore_pending && timeout_s > 0 && !display_idle_dimmed &&
       now >= last_activity &&
       now - last_activity >= (uint64_t)timeout_s * 1000000ULL) {
        should_dim = 1;
    }
    pthread_mutex_unlock(&display_idle_lock);

    if(!restore_pending && display_idle_is_dimmed() &&
       ui_hardware_screen_backlight_get() > 0) {
        if(ui_hardware_boot0_screen_off()) {
            ui_hardware_shutdown_backlights_apply(0, 0);
            touch_trace_log("DISPLAY_TIMEOUT_POLL_FORCE_OFF screen=%d",
                            ui_hardware_screen_backlight_get());
            return;
        }
        pthread_mutex_lock(&display_idle_lock);
        display_idle_dimmed = 0;
        display_idle_wake_pending = 0;
        display_last_activity_us = now;
        pthread_mutex_unlock(&display_idle_lock);
        touch_trace_log("DISPLAY_TIMEOUT_EXTERNAL_WAKE screen=%d",
                        ui_hardware_screen_backlight_get());
        app_request_fast_refresh();
        return;
    }

    if(restore_pending) {
        int screen = saved_screen > 0 ? saved_screen : 80;
        int keyboard = saved_keyboard >= 0 ? saved_keyboard :
                       ui_hardware_keyboard_backlight_get();

        ui_hardware_set_screen_off(0);
        ui_hardware_shutdown_backlights_apply(screen, keyboard);
        touch_trace_log("DISPLAY_TIMEOUT_WAKE screen=%d keyboard=%d",
                        screen, keyboard);
        app_request_fast_refresh();
        return;
    }

    if(!should_dim) {
        return;
    }

    saved_screen = ui_hardware_screen_backlight_get();
    saved_keyboard = ui_hardware_keyboard_backlight_get();
    pthread_mutex_lock(&display_idle_lock);
    if(!display_idle_dimmed && display_timeout_s > 0 &&
       now >= display_last_activity_us &&
       now - display_last_activity_us >=
       (uint64_t)display_timeout_s * 1000000ULL) {
        display_idle_saved_screen = saved_screen > 0 ? saved_screen : 80;
        display_idle_saved_keyboard = saved_keyboard >= 0 ? saved_keyboard : 0;
        display_idle_dimmed = 1;
        should_dim = 1;
    } else {
        should_dim = 0;
    }
    pthread_mutex_unlock(&display_idle_lock);

    if(should_dim) {
        ui_hardware_set_screen_off(1);
        touch_trace_log("DISPLAY_TIMEOUT_SLEEP seconds=%d saved_screen=%d "
                        "saved_keyboard=%d",
                        timeout_s, display_idle_saved_screen,
                        display_idle_saved_keyboard);
    }
}

static void restart_for_display_orientation(const char *value)
{
    int degrees;

    if(!value) {
        return;
    }

    degrees = parse_display_rotation_degrees(value);
    set_display_rotation_degrees(degrees);

    if(main_display) {
        lv_obj_t *overlay = lv_obj_create(lv_layer_top());
        lv_obj_t *spinner;
        lv_obj_t *label;

        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_set_size(overlay, display_logical_width(),
                        display_logical_height());
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0x05070A), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_set_style_pad_all(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);

        spinner = lv_spinner_create(overlay);
        lv_obj_set_size(spinner, 72, 72);
        lv_obj_center(spinner);

        label = lv_label_create(overlay);
        lv_label_set_text_fmt(label, "Rotate %d deg", display_rotation_degrees);
        lv_obj_set_style_text_color(label, lv_color_hex(0xDDE7F0), 0);
        lv_obj_align_to(label, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);

        lv_refr_now(main_display);
        usleep(50000);
    }

    touch_trace_log("DISPLAY_ROTATION_RESTART degrees=%d persistent=on",
                    display_rotation_degrees);
    setenv(DISPLAY_ROTATION_ENV, display_orientation_value, 1);
    setenv(DISPLAY_ORIENTATION_ENV, display_orientation_value, 1);
    setenv(DISPLAY_ROTATION_RESTART_ENV, "1", 1);
    execl(PHONE_UI_BIN, "k230_phone_ui", NULL);
    touch_trace_log("DISPLAY_ORIENTATION_RESTART_FAILED errno=%d", errno);
}

static void apply_display_rotation_change(int old_degrees, const char *reason)
{
    if(old_degrees == display_rotation_degrees) {
        display_update_orientation_controls();
        request_fast_refresh();
        return;
    }

    if(!main_display ||
       display_degrees_is_landscape(old_degrees) !=
       display_degrees_is_landscape(display_rotation_degrees)) {
        touch_trace_log("DISPLAY_ROTATION_CHANGE degrees=%d old=%d "
                        "mode=restart reason=%s",
                        display_rotation_degrees, old_degrees,
                        reason ? reason : "unknown");
        restart_for_display_orientation(display_orientation_value);
        return;
    }

    touch_trace_log("DISPLAY_ROTATION_CHANGE degrees=%d old=%d "
                    "mode=runtime reason=%s",
                    display_rotation_degrees, old_degrees,
                    reason ? reason : "unknown");
    lv_linux_drm_set_rotation(main_display,
                              display_drm_rotation_from_orientation());
    apply_display_orientation(main_display);
    render_page(current_page, LV_SCREEN_LOAD_ANIM_NONE, 0);
    display_update_orientation_controls();
    lv_refr_now(main_display);
}

static void show_rotation_startup_cover(lv_display_t *disp)
{
    const char *restart_env = getenv(DISPLAY_ROTATION_RESTART_ENV);
    lv_obj_t *cover;
    lv_obj_t *spinner;

    if(!disp || !restart_env || strcmp(restart_env, "1") != 0) {
        return;
    }

    cover = lv_obj_create(NULL);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_size(cover, display_logical_width(), display_logical_height());
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_pad_all(cover, 0, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

    spinner = lv_spinner_create(cover);
    lv_obj_set_size(spinner, 72, 72);
    lv_obj_center(spinner);

    lv_screen_load(cover);
    touch_trace_log("DISPLAY_ROTATION_STARTUP_COVER degrees=%d",
                    display_rotation_degrees);
    lv_refr_now(disp);
}

static int32_t map_axis(int32_t raw, int32_t raw_max, int32_t out_size)
{
    if(raw < 0) {
        return -1;
    }
    if(raw_max <= 0 || out_size <= 1) {
        return 0;
    }
    if(raw > raw_max) {
        raw = raw_max;
    }
    return raw * (out_size - 1) / raw_max;
}

static int32_t map_axis_reverse(int32_t raw, int32_t raw_max, int32_t out_size)
{
    int32_t mapped = map_axis(raw, raw_max, out_size);

    return mapped >= 0 ? (out_size - 1) - mapped : -1;
}

static void display_map_raw_touch(int32_t raw_x, int32_t raw_y, int32_t *screen_x,
                                  int32_t *screen_y)
{
    int logical_w = display_logical_width();
    int logical_h = display_logical_height();
    int32_t x = -1;
    int32_t y = -1;

    switch(display_rotation_degrees) {
    case 90:
        x = map_axis(raw_y, TOUCH_MAX_Y, logical_w);
        y = map_axis_reverse(raw_x, TOUCH_MAX_X, logical_h);
        break;
    case 180:
        x = map_axis_reverse(raw_x, TOUCH_MAX_X, logical_w);
        y = map_axis_reverse(raw_y, TOUCH_MAX_Y, logical_h);
        break;
    case 270:
        x = map_axis_reverse(raw_y, TOUCH_MAX_Y, logical_w);
        y = map_axis(raw_x, TOUCH_MAX_X, logical_h);
        break;
    default:
        x = map_axis(raw_x, TOUCH_MAX_X, logical_w);
        y = map_axis(raw_y, TOUCH_MAX_Y, logical_h);
        break;
    }

    if(screen_x) {
        *screen_x = x;
    }
    if(screen_y) {
        *screen_y = y;
    }
}

static void evdev_guarded_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if(evdev_original_read_cb) {
        evdev_original_read_cb(indev, data);
    }
    if(touch_input_blocked()) {
        uint64_t now = monotonic_us();
        if(data) {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        if(now - touch_block_last_log_us > 1000000ULL) {
            touch_block_last_log_us = now;
            touch_trace_log("TOUCH_BLOCKED screen_off idle=%d boot0=%d",
                            display_idle_is_dimmed(),
                            ui_hardware_boot0_screen_off());
        }
    }
}

static void trace_raw_state(uint32_t seq, int pressed, int32_t raw_x, int32_t raw_y,
                            uint64_t kernel_us, uint64_t recv_us)
{
    static uint64_t last_raw_log_us;
    static int last_raw_log_pressed = -1;
    int32_t screen_x = -1;
    int32_t screen_y = -1;
    uint64_t lag_us = 0;
    int should_log;

    display_map_raw_touch(raw_x, raw_y, &screen_x, &screen_y);
    if(kernel_us && recv_us >= kernel_us) {
        lag_us = recv_us - kernel_us;
    }

    pthread_mutex_lock(&touch_state_lock);
    last_raw_seq = seq;
    last_raw_syn_app_us = recv_us;
    last_raw_syn_kernel_us = kernel_us;
    last_raw_lag_us = lag_us;
    last_raw_x = raw_x;
    last_raw_y = raw_y;
    last_raw_screen_x = screen_x;
    last_raw_screen_y = screen_y;
    last_raw_pressed = pressed;
    pthread_mutex_unlock(&touch_state_lock);

    should_log = last_raw_log_pressed != pressed ||
                 recv_us - last_raw_log_us >= TOUCH_TRACE_RAW_LOG_INTERVAL_US;

    if(should_log) {
        last_raw_log_us = recv_us;
        last_raw_log_pressed = pressed;
        touch_trace_log("RAW seq=%lu state=%s raw=(%ld,%ld) screen=(%ld,%ld) "
                        "kernel=%llu.%06llu recv_lag=%.3fms",
                        (unsigned long)seq, pressed ? "down" : "up",
                        (long)raw_x, (long)raw_y, (long)screen_x, (long)screen_y,
                        (unsigned long long)(kernel_us / 1000000ULL),
                        (unsigned long long)(kernel_us % 1000000ULL),
                        (double)lag_us / 1000.0);
    }
    if(touch_input_blocked()) {
        uint64_t now = monotonic_us();
        if(now - touch_block_last_log_us > 1000000ULL) {
            touch_block_last_log_us = now;
            touch_trace_log("RAW_IGNORED_SCREEN_OFF seq=%lu idle=%d boot0=%d",
                            (unsigned long)seq, display_idle_is_dimmed(),
                            ui_hardware_boot0_screen_off());
        }
        return;
    }
    app_note_user_activity();
    edge_back_raw_trace(seq, pressed, screen_x, screen_y);
}

static void *touch_trace_thread_main(void *arg)
{
    struct input_event event;
    uint32_t seq = 0;
    int32_t raw_x = -1;
    int32_t raw_y = -1;
    int pressed = 0;
    int changed = 0;
    int fd;
    int clock_id = CLOCK_MONOTONIC;
    const char *path = (const char *)arg;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        touch_trace_log("TRACE_OPEN_FAILED device=%s errno=%d (%s)", path, errno,
                        strerror(errno));
        return NULL;
    }

    if(ioctl(fd, EVIOCSCLOCKID, &clock_id) < 0) {
        touch_trace_log("TRACE_CLOCK_WARN EVIOCSCLOCKID failed errno=%d (%s)", errno,
                        strerror(errno));
    }

    touch_trace_log("TRACE_START device=%s log=%s", path, TOUCH_TRACE_PATH);

    while(touch_trace_running) {
        ssize_t rd = read(fd, &event, sizeof(event));

        if(rd == (ssize_t)sizeof(event)) {
            if(event.type == EV_ABS) {
                if(event.code == ABS_X || event.code == ABS_MT_POSITION_X) {
                    raw_x = event.value;
                    changed = 1;
                } else if(event.code == ABS_Y || event.code == ABS_MT_POSITION_Y) {
                    raw_y = event.value;
                    changed = 1;
                } else if(event.code == ABS_MT_TRACKING_ID) {
                    pressed = event.value >= 0;
                    changed = 1;
                }
            } else if(event.type == EV_KEY &&
                      (event.code == BTN_TOUCH || event.code == BTN_MOUSE)) {
                pressed = event.value != 0;
                changed = 1;
            } else if(event.type == EV_SYN && event.code == SYN_REPORT) {
                if(changed) {
                    seq++;
                    trace_raw_state(seq, pressed, raw_x, raw_y, input_event_us(&event),
                                    monotonic_us());
                    changed = 0;
                }
            }
            continue;
        }

        if(rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(2000);
            continue;
        }
        if(rd < 0 && errno == EINTR) {
            continue;
        }
        if(rd < 0) {
            touch_trace_log("TRACE_READ_FAILED errno=%d (%s)", errno, strerror(errno));
            break;
        }

        usleep(2000);
    }

    close(fd);
    touch_trace_log("TRACE_STOP");
    return NULL;
}

static void start_touch_trace(const char *path)
{
    touch_trace_running = 1;
    if(pthread_create(&touch_trace_thread, NULL, touch_trace_thread_main, (void *)path) != 0) {
        touch_trace_running = 0;
        touch_trace_log("TRACE_THREAD_FAILED");
        return;
    }
    pthread_detach(touch_trace_thread);
}

static void motion_note_frame(uint64_t now)
{
    if(current_page != PAGE_MOTION) {
        return;
    }

    if(motion_last_frame_us) {
        uint64_t dt = now - motion_last_frame_us;

        motion_frame_sum_us += dt;
        if(motion_frame_min_us == 0 || dt < motion_frame_min_us) {
            motion_frame_min_us = dt;
        }
        if(dt > motion_frame_max_us) {
            motion_frame_max_us = dt;
        }
        motion_frame_dt_count++;
    }

    motion_last_frame_us = now;
    motion_frame_count++;
}

static void display_trace_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    uint64_t now = monotonic_us();
    int active = touch_trace_ui_active(now);

    if(code == LV_EVENT_REFR_START) {
        motion_step_frame_locked(now);
    } else if(code == LV_EVENT_RENDER_START) {
        last_render_start_us = now;
        motion_note_frame(now);
        if(touch_trace_verbose && active) {
            touch_trace_log("RENDER_START after_ui=%.3fms",
                            (double)(now - last_ui_action_us) / 1000.0);
        }
    } else if(code == LV_EVENT_RENDER_READY) {
        uint64_t dur = last_render_start_us ? now - last_render_start_us : 0;
        if((touch_trace_verbose && active) || dur > TOUCH_TRACE_SLOW_US) {
            touch_trace_log("RENDER_READY duration=%.3fms", (double)dur / 1000.0);
        }
    } else if(code == LV_EVENT_FLUSH_START) {
        last_flush_start_us = now;
        if(touch_trace_verbose && active) {
            touch_trace_log("FLUSH_START after_ui=%.3fms",
                            (double)(now - last_ui_action_us) / 1000.0);
        }
    } else if(code == LV_EVENT_FLUSH_FINISH) {
        uint64_t dur = last_flush_start_us ? now - last_flush_start_us : 0;
        if((touch_trace_verbose && active) || dur > TOUCH_TRACE_SLOW_US) {
            touch_trace_log("FLUSH_FINISH duration=%.3fms", (double)dur / 1000.0);
        }
    } else if(code == LV_EVENT_FLUSH_WAIT_START) {
        last_flush_wait_start_us = now;
        if(touch_trace_verbose && active) {
            touch_trace_log("FLUSH_WAIT_START after_ui=%.3fms",
                            (double)(now - last_ui_action_us) / 1000.0);
        }
    } else if(code == LV_EVENT_FLUSH_WAIT_FINISH) {
        uint64_t dur = last_flush_wait_start_us ? now - last_flush_wait_start_us : 0;
        if((touch_trace_verbose && active) || dur > TOUCH_TRACE_SLOW_US) {
            touch_trace_log("FLUSH_WAIT_FINISH duration=%.3fms", (double)dur / 1000.0);
        }
    }
}

static void init_styles(void)
{
    if(styles_ready) {
        return;
    }

    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, lv_color_hex(0x171B20));
    lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
    lv_style_set_radius(&style_panel, 8);
    lv_style_set_border_width(&style_panel, 1);
    lv_style_set_border_color(&style_panel, lv_color_hex(0x2A3037));
    lv_style_set_pad_all(&style_panel, 16);

    lv_style_init(&style_text_primary);
    lv_style_set_text_color(&style_text_primary, lv_color_hex(0xF2F5F8));

    lv_style_init(&style_text_secondary);
    lv_style_set_text_color(&style_text_secondary, lv_color_hex(0x9AA4AF));

    lv_style_init(&style_chip);
    lv_style_set_bg_color(&style_chip, lv_color_hex(0x222832));
    lv_style_set_bg_opa(&style_chip, LV_OPA_COVER);
    lv_style_set_radius(&style_chip, 8);
    lv_style_set_pad_hor(&style_chip, 10);
    lv_style_set_pad_ver(&style_chip, 5);

    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, lv_color_hex(0x222832));
    lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
    lv_style_set_radius(&style_button, 8);
    lv_style_set_border_width(&style_button, 1);
    lv_style_set_border_color(&style_button, lv_color_hex(0x2A3037));
    lv_style_set_pad_all(&style_button, 8);

    lv_style_init(&style_button_pressed);
    lv_style_set_bg_color(&style_button_pressed, lv_color_hex(0x2D3744));
    lv_style_set_translate_y(&style_button_pressed, 2);

    lv_style_init(&style_icon);
    lv_style_set_radius(&style_icon, 8);
    lv_style_set_bg_opa(&style_icon, LV_OPA_COVER);
    lv_style_set_border_width(&style_icon, 0);

    styles_ready = 1;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color)
{
    return ui_label(parent, text, font, color);
}

static void set_opa_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void start_opa_anim(lv_obj_t *obj, int32_t from, int32_t to, uint32_t delay,
                           uint32_t duration)
{
    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, set_opa_anim_cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void animate_page_enter(lv_obj_t *scr, lv_screen_load_anim_t anim_type)
{
    (void)anim_type;

    if(strcmp(page_transition_effect, "fade") != 0) {
        return;
    }
    lv_obj_set_style_opa(scr, LV_OPA_TRANSP, 0);
    start_opa_anim(scr, LV_OPA_TRANSP, LV_OPA_COVER, 0,
                   PAGE_TRANSITION_ANIM_MS);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    int fit_w = ui_fit_width(parent, x, w);

    lv_obj_add_style(obj, &style_panel, 0);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, fit_w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void portrait_scroll_refresh_cb(lv_event_t *event)
{
    static uint64_t last_refresh_us;
    static int config_logged;
    uint64_t now;
    lv_event_code_t code;
    lv_obj_t *screen;
    lv_obj_t *target;
    lv_area_t repair_area;
    const char *enabled_env;
    const char *percent_env;
    int repair_percent = PORTRAIT_SCROLL_REPAIR_DEFAULT_PERCENT;
    int force_refresh;
    int logical_w;
    int logical_h;
    int repair_h;
    int y1;

    if(display_logical_width() >= display_logical_height()) {
        return;
    }

    code = lv_event_get_code(event);
    enabled_env = getenv("K230_PORTRAIT_SCROLL_REPAIR");
    if(!enabled_env || strcmp(enabled_env, "1") != 0) {
        return;
    }
    percent_env = getenv("K230_PORTRAIT_SCROLL_REPAIR_PERCENT");
    if(percent_env && percent_env[0]) {
        int value = atoi(percent_env);

        if(value >= 20 && value <= 70) {
            repair_percent = value;
        }
    }

    if(!config_logged) {
        touch_trace_log("PORTRAIT_SCROLL_REPAIR percent=%d interval_us=%llu",
                        repair_percent,
                        (unsigned long long)PORTRAIT_SCROLL_REPAIR_INTERVAL_US);
        config_logged = 1;
    }

    force_refresh = (code == LV_EVENT_SCROLL_BEGIN ||
                     code == LV_EVENT_SCROLL_END);
    now = monotonic_us();
    if(!force_refresh && last_refresh_us != 0ULL &&
       now - last_refresh_us < PORTRAIT_SCROLL_REPAIR_INTERVAL_US) {
        return;
    }
    last_refresh_us = now;

    target = lv_event_get_target(event);
    if(target) {
        lv_obj_invalidate(target);
    }

    screen = lv_scr_act();
    logical_w = display_logical_width();
    logical_h = display_logical_height();
    repair_h = logical_h * repair_percent / 100;
    if(repair_h < 120) {
        repair_h = 120;
    }
    y1 = logical_h - repair_h - PORTRAIT_SCROLL_REPAIR_EXTRA_PX;
    if(y1 < 0) {
        y1 = 0;
    }
    if(logical_w > 0 && logical_h > 0 && screen) {
        lv_area_set(&repair_area, 0, y1, logical_w - 1, logical_h - 1);
        lv_obj_invalidate_area(screen, &repair_area);
    }
    request_fast_refresh();
}

static lv_obj_t *scroll_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = panel(parent, x, y, w, h);

    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(obj, 48, 0);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_END, NULL);
    return obj;
}

static lv_obj_t *scroll_region(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_bottom(obj, 12, 0);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(obj, portrait_scroll_refresh_cb,
                        LV_EVENT_SCROLL_END, NULL);
    return obj;
}

static int page_body_width(void)
{
    int w = display_logical_width() - ui_page_side_margin() * 2;

    return w > 280 ? w : 280;
}

static int page_content_top_y(int y)
{
    if(display_orientation_is_landscape() && app_edge_back_enabled() &&
       y >= 140) {
        return 64;
    }
    return y;
}

static int page_body_height_from(int y)
{
    int h = display_logical_height() - page_content_top_y(y) - 24;

    return h > 180 ? h : 180;
}

static int path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static void trim_text(char *text)
{
    size_t len;
    char *start;

    if(!text) {
        return;
    }

    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    len = strlen(text);
    while(len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

static int read_file_first_line(const char *path, char *buf, size_t len)
{
    FILE *fp;

    if(!path || !buf || len == 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if(!fp) {
        return -1;
    }

    if(!fgets(buf, len, fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    trim_text(buf);
    return 0;
}

static int read_cmd_first_line(const char *cmd, char *buf, size_t len)
{
    FILE *fp;
    int rc;

    if(!cmd || !buf || len == 0) {
        return -1;
    }

    fp = popen(cmd, "r");
    if(!fp) {
        return -1;
    }

    if(!fgets(buf, len, fp)) {
        buf[0] = '\0';
        rc = pclose(fp);
        (void)rc;
        return -1;
    }

    rc = pclose(fp);
    (void)rc;
    buf[strcspn(buf, "\r\n")] = '\0';
    trim_text(buf);
    return buf[0] ? 0 : -1;
}

static void read_os_pretty(char *buf, size_t len)
{
    FILE *fp;
    char line[160];

    if(!buf || len == 0) {
        return;
    }

    snprintf(buf, len, "Buildroot");
    fp = fopen("/etc/os-release", "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        char *value;

        if(strncmp(line, "PRETTY_NAME=", 12) != 0) {
            continue;
        }

        value = line + 12;
        value[strcspn(value, "\r\n")] = '\0';
        if(value[0] == '"' && strlen(value) > 1U) {
            value++;
            value[strcspn(value, "\"")] = '\0';
        }
        snprintf(buf, len, "%s", value);
        break;
    }

    fclose(fp);
}

static void read_kernel_release(char *buf, size_t len)
{
    struct utsname uts;

    if(!buf || len == 0) {
        return;
    }

    if(uname(&uts) == 0) {
        snprintf(buf, len, "%s", uts.release);
    } else {
        snprintf(buf, len, "Unknown");
    }
}

static void read_cpu_summary(char *buf, size_t len)
{
    FILE *fp;
    char line[256];
    char uarch[64] = "";
    char isa[96] = "";
    char isa_short[32] = "";

    if(!buf || len == 0) {
        return;
    }

    snprintf(buf, len, "RISC-V");
    fp = fopen("/proc/cpuinfo", "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp)) {
        char *sep;

        if(strncmp(line, "uarch", 5) != 0 && strncmp(line, "isa", 3) != 0) {
            continue;
        }

        sep = strchr(line, ':');
        if(!sep) {
            continue;
        }
        sep++;
        trim_text(sep);
        sep[strcspn(sep, "\r\n")] = '\0';

        if(strncmp(line, "uarch", 5) == 0) {
            snprintf(uarch, sizeof(uarch), "%s", sep);
        } else if(strncmp(line, "isa", 3) == 0) {
            snprintf(isa, sizeof(isa), "%s", sep);
        }
    }

    fclose(fp);

    if(isa[0]) {
        size_t n = strcspn(isa, "_ \t\r\n");
        if(n >= sizeof(isa_short)) {
            n = sizeof(isa_short) - 1U;
        }
        memcpy(isa_short, isa, n);
        isa_short[n] = '\0';
    }

    if(uarch[0] && isa_short[0]) {
        snprintf(buf, len, "%s / %s", uarch, isa_short);
    } else if(uarch[0]) {
        snprintf(buf, len, "%s", uarch);
    } else if(isa_short[0]) {
        snprintf(buf, len, "%s", isa_short);
    }
}

static int read_mem_totals(unsigned long *total_kb, unsigned long *avail_kb);

static void read_mem_summary(char *buf, size_t len)
{
    unsigned long total_kb = 0;
    unsigned long avail_kb = 0;

    if(!buf || len == 0) {
        return;
    }

    snprintf(buf, len, "Unknown");
    if(read_mem_totals(&total_kb, &avail_kb) == 0 && total_kb > 0) {
        snprintf(buf, len, "%luMB / %luMB", avail_kb / 1024UL,
                 total_kb / 1024UL);
    }
}

static int read_cpu_temp_summary(char *buf, size_t len)
{
    double temp_c = 0.0;

    if(!buf || len == 0) {
        return -1;
    }
    if(ui_hardware_get_cpu_temp_c(&temp_c) == 0 &&
       temp_c > -40.0 && temp_c < 150.0) {
        snprintf(buf, len, "%.1f C", temp_c);
        return 0;
    }
    snprintf(buf, len, "--");
    return -1;
}

static int read_power_source_summary(char *buf, size_t len)
{
    int usb_present = 0;
    int vbus_mv = 0;
    int vbat_mv = 0;

    if(!buf || len == 0) {
        return -1;
    }
    if(ui_bq25896_get_power_state(&usb_present, &vbus_mv, &vbat_mv) == 0) {
        if(usb_present) {
            if(vbus_mv > 0) {
                snprintf(buf, len, "USB %dmV", vbus_mv);
            } else {
                snprintf(buf, len, "USB");
            }
        } else {
            if(vbat_mv > 0) {
                snprintf(buf, len, "Battery %.2fV", (double)vbat_mv / 1000.0);
            } else {
                snprintf(buf, len, "Battery");
            }
        }
        return 0;
    }
    snprintf(buf, len, "USB");
    return -1;
}

static int read_power_draw_summary(char *buf, size_t len)
{
    int current_ma = 0;

    if(!buf || len == 0) {
        return -1;
    }
    if(ui_bq27220_get_current_ma(&current_ma) != 0) {
        snprintf(buf, len, "--");
        return -1;
    }

    snprintf(buf, len, "%+dmA", current_ma);
    return 0;
}

static int read_home_network_summary(char *title, size_t title_len,
                                     char *symbol, size_t symbol_len,
                                     char *buf, size_t len, uint32_t *color)
{
    char path[128];
    char ip[64];
    int carrier;

    if(!buf || len == 0) {
        return -1;
    }

    if(title && title_len > 0) {
        snprintf(title, title_len, "Network IP");
    }
    if(symbol && symbol_len > 0) {
        snprintf(symbol, symbol_len, "NET");
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", NET_ETH_IFACE);
    if(path_exists(path)) {
        carrier = ui_read_iface_carrier(NET_ETH_IFACE);
        if(carrier == 1 &&
           ui_read_iface_ip(NET_ETH_IFACE, ip, sizeof(ip)) == 0) {
            char *slash = strchr(ip, '/');

            if(slash) {
                *slash = '\0';
            }
            if(title && title_len > 0) {
                snprintf(title, title_len, "Ethernet IP");
            }
            if(symbol && symbol_len > 0) {
                snprintf(symbol, symbol_len, "ETH");
            }
            snprintf(buf, len, "%s", ip[0] ? ip : "--");
            if(color) {
                *color = 0x25C281;
            }
            return 0;
        }

        if(carrier == 1) {
            if(title && title_len > 0) {
                snprintf(title, title_len, "Ethernet IP");
            }
            if(symbol && symbol_len > 0) {
                snprintf(symbol, symbol_len, "ETH");
            }
            snprintf(buf, len, "No IP");
            if(color) {
                *color = 0xF5A524;
            }
            return -1;
        }
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", NET_WIFI_IFACE);
    if(path_exists(path) &&
       ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
        char *slash = strchr(ip, '/');

        if(slash) {
            *slash = '\0';
        }
        if(title && title_len > 0) {
            snprintf(title, title_len, "WiFi IP");
        }
        if(symbol && symbol_len > 0) {
            snprintf(symbol, symbol_len, "WIFI");
        }
        snprintf(buf, len, "%s", ip[0] ? ip : "--");
        if(color) {
            *color = 0x22D3EE;
        }
        return 0;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", NET_ETH_IFACE);
    if(path_exists(path)) {
        carrier = ui_read_iface_carrier(NET_ETH_IFACE);
        if(title && title_len > 0) {
            snprintf(title, title_len, "Ethernet IP");
        }
        if(symbol && symbol_len > 0) {
            snprintf(symbol, symbol_len, "ETH");
        }
        snprintf(buf, len, carrier == 0 ? "No link" :
                 carrier == 1 ? "No IP" : "Unknown");
        if(color) {
            *color = carrier == 1 ? 0xF5A524 : 0x9AA4AF;
        }
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", NET_WIFI_IFACE);
    if(path_exists(path)) {
        if(title && title_len > 0) {
            snprintf(title, title_len, "WiFi IP");
        }
        if(symbol && symbol_len > 0) {
            snprintf(symbol, symbol_len, "WIFI");
        }
        snprintf(buf, len, "No link");
        if(color) {
            *color = 0x9AA4AF;
        }
        return -1;
    }

    snprintf(buf, len, "Missing");
    if(color) {
        *color = 0x9AA4AF;
    }
    return -1;
}

static void *home_telemetry_thread_cb(void *arg)
{
    char temp_text[64];
    char power_source_text[64];
    char power_draw_text[64];
    char eth_title[32];
    char eth_symbol[8];
    char eth_text[64];
    uint32_t temp_color;
    uint32_t source_color;
    uint32_t draw_color;
    uint32_t eth_color;

    (void)arg;

    temp_color = read_cpu_temp_summary(temp_text, sizeof(temp_text)) == 0 ?
                 0xF5A524 : 0x9AA4AF;
    source_color = read_power_source_summary(power_source_text,
                                             sizeof(power_source_text)) == 0 ?
                   0x25C281 : 0x9AA4AF;
    draw_color = read_power_draw_summary(power_draw_text,
                                         sizeof(power_draw_text)) == 0 ?
                 0x22D3EE : 0x9AA4AF;
    read_home_network_summary(eth_title, sizeof(eth_title),
                              eth_symbol, sizeof(eth_symbol),
                              eth_text, sizeof(eth_text), &eth_color);

    pthread_mutex_lock(&home_telemetry_lock);
    snprintf(home_temp_cache, sizeof(home_temp_cache), "%s", temp_text);
    snprintf(home_power_source_cache, sizeof(home_power_source_cache), "%s",
             power_source_text);
    snprintf(home_power_draw_cache, sizeof(home_power_draw_cache), "%s",
             power_draw_text);
    snprintf(home_eth_title_cache, sizeof(home_eth_title_cache), "%s",
             eth_title);
    snprintf(home_eth_symbol_cache, sizeof(home_eth_symbol_cache), "%s",
             eth_symbol);
    snprintf(home_eth_cache, sizeof(home_eth_cache), "%s", eth_text);
    home_temp_cache_color = temp_color;
    home_power_source_cache_color = source_color;
    home_power_draw_cache_color = draw_color;
    home_eth_cache_color = eth_color;
    home_telemetry_cache_valid = 1;
    home_telemetry_generation++;
    home_telemetry_worker_active = 0;
    pthread_mutex_unlock(&home_telemetry_lock);
    return NULL;
}

static void home_telemetry_start_worker(void)
{
    pthread_t thread;
    int should_start = 0;

    pthread_mutex_lock(&home_telemetry_lock);
    if(!home_telemetry_worker_active) {
        home_telemetry_worker_active = 1;
        should_start = 1;
    }
    pthread_mutex_unlock(&home_telemetry_lock);

    if(!should_start) {
        return;
    }

    if(pthread_create(&thread, NULL, home_telemetry_thread_cb, NULL) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&home_telemetry_lock);
        home_telemetry_worker_active = 0;
        pthread_mutex_unlock(&home_telemetry_lock);
    }
}

static void update_home_telemetry_labels(int force)
{
    char temp_text[64];
    char power_source_text[64];
    char power_draw_text[64];
    char eth_title[32];
    char eth_symbol[8];
    char eth_text[64];
    uint32_t temp_color;
    uint32_t source_color;
    uint32_t draw_color;
    uint32_t eth_color;
    unsigned int generation;
    int cache_valid;
    uint64_t now_us = ui_monotonic_us();

    if(!home_temp_value_label && !home_power_source_value_label &&
       !home_power_draw_value_label && !home_eth_value_label) {
        return;
    }
    if(force || !home_telemetry_last_us ||
       now_us - home_telemetry_last_us >= 3000000ULL) {
        home_telemetry_last_us = now_us;
        home_telemetry_start_worker();
    }

    pthread_mutex_lock(&home_telemetry_lock);
    cache_valid = home_telemetry_cache_valid;
    generation = home_telemetry_generation;
    snprintf(temp_text, sizeof(temp_text), "%s", home_temp_cache);
    snprintf(power_source_text, sizeof(power_source_text), "%s",
             home_power_source_cache);
    snprintf(power_draw_text, sizeof(power_draw_text), "%s",
             home_power_draw_cache);
    snprintf(eth_title, sizeof(eth_title), "%s", home_eth_title_cache);
    snprintf(eth_symbol, sizeof(eth_symbol), "%s", home_eth_symbol_cache);
    snprintf(eth_text, sizeof(eth_text), "%s", home_eth_cache);
    temp_color = home_temp_cache_color;
    source_color = home_power_source_cache_color;
    draw_color = home_power_draw_cache_color;
    eth_color = home_eth_cache_color;
    pthread_mutex_unlock(&home_telemetry_lock);

    if(!cache_valid ||
       (!force && generation == home_telemetry_displayed_generation)) {
        return;
    }
    home_telemetry_displayed_generation = generation;

    if(home_temp_value_label && lv_obj_is_valid(home_temp_value_label)) {
        lv_label_set_text(home_temp_value_label, temp_text);
        lv_obj_set_style_text_color(home_temp_value_label,
                                    lv_color_hex(temp_color), 0);
    }
    if(home_temp_badge && lv_obj_is_valid(home_temp_badge)) {
        lv_obj_set_style_bg_color(home_temp_badge, lv_color_hex(temp_color), 0);
    }
    if(home_power_source_value_label &&
       lv_obj_is_valid(home_power_source_value_label)) {
        lv_label_set_text(home_power_source_value_label, power_source_text);
        lv_obj_set_style_text_color(home_power_source_value_label,
                                    lv_color_hex(source_color), 0);
    }
    if(home_power_source_badge && lv_obj_is_valid(home_power_source_badge)) {
        lv_obj_set_style_bg_color(home_power_source_badge,
                                  lv_color_hex(source_color), 0);
    }
    if(home_power_draw_value_label &&
       lv_obj_is_valid(home_power_draw_value_label)) {
        lv_label_set_text(home_power_draw_value_label, power_draw_text);
        lv_obj_set_style_text_color(home_power_draw_value_label,
                                    lv_color_hex(draw_color), 0);
    }
    if(home_power_draw_badge && lv_obj_is_valid(home_power_draw_badge)) {
        lv_obj_set_style_bg_color(home_power_draw_badge,
                                  lv_color_hex(draw_color), 0);
    }
    if(home_eth_title_label && lv_obj_is_valid(home_eth_title_label)) {
        lv_label_set_text(home_eth_title_label, eth_title);
    }
    if(home_eth_value_label && lv_obj_is_valid(home_eth_value_label)) {
        lv_label_set_text(home_eth_value_label, eth_text);
        lv_obj_set_style_text_color(home_eth_value_label,
                                    lv_color_hex(eth_color), 0);
    }
    if(home_eth_badge && lv_obj_is_valid(home_eth_badge)) {
        lv_obj_set_style_bg_color(home_eth_badge, lv_color_hex(eth_color), 0);
    }
    if(home_eth_badge_text_label &&
       lv_obj_is_valid(home_eth_badge_text_label)) {
        lv_label_set_text(home_eth_badge_text_label, eth_symbol);
    }
}

static void home_telemetry_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if(current_page != PAGE_HOME) {
        return;
    }
    update_home_telemetry_labels(0);
}

static int read_mem_totals(unsigned long *total_kb, unsigned long *avail_kb)
{
    FILE *fp;
    char line[128];
    unsigned long total = 0;
    unsigned long avail = 0;

    if(total_kb) {
        *total_kb = 0;
    }
    if(avail_kb) {
        *avail_kb = 0;
    }

    fp = fopen("/proc/meminfo", "r");
    if(!fp) {
        return -1;
    }

    while(fgets(line, sizeof(line), fp)) {
        if(sscanf(line, "MemTotal: %lu kB", &total) == 1) {
            continue;
        }
        if(sscanf(line, "MemAvailable: %lu kB", &avail) == 1) {
            break;
        }
    }

    fclose(fp);
    if(total <= 0) {
        return -1;
    }

    if(total_kb) {
        *total_kb = total;
    }
    if(avail_kb) {
        *avail_kb = avail;
    }
    return 0;
}

static void read_storage_summary(char *buf, size_t len)
{
    struct statvfs vfs;
    unsigned long total_mb;
    unsigned long free_mb;

    if(!buf || len == 0) {
        return;
    }

    if(statvfs("/", &vfs) != 0 || vfs.f_frsize == 0) {
        snprintf(buf, len, "Unknown");
        return;
    }

    total_mb = (unsigned long)((uint64_t)vfs.f_blocks * vfs.f_frsize /
                               (1024ULL * 1024ULL));
    free_mb = (unsigned long)((uint64_t)vfs.f_bavail * vfs.f_frsize /
                              (1024ULL * 1024ULL));
    snprintf(buf, len, "%luMB free %luMB", total_mb, free_mb);
}

static int read_iface_ip(const char *iface, char *buf, size_t len)
{
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    int rc = -1;

    if(!iface || !buf || len == 0) {
        return -1;
    }

    if(getifaddrs(&ifaddr) != 0) {
        snprintf(buf, len, "--");
        return -1;
    }

    for(ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;

        if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
           strcmp(ifa->ifa_name, iface) != 0) {
            continue;
        }

        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if(inet_ntop(AF_INET, &sin->sin_addr, buf, len)) {
            rc = 0;
            break;
        }
    }
    freeifaddrs(ifaddr);

    if(rc != 0) {
        snprintf(buf, len, "--");
    }
    return rc;
}

static void read_iface_state(const char *iface, char *buf, size_t len,
                             uint32_t *color)
{
    char path[128];
    char oper[32];
    char carrier[8];
    char ip[64];

    if(!iface || !buf || len == 0) {
        return;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);
    if(!path_exists(path)) {
        snprintf(buf, len, "Missing");
        if(color) {
            *color = 0x9AA4AF;
        }
        return;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
    if(read_file_first_line(path, oper, sizeof(oper)) != 0) {
        snprintf(oper, sizeof(oper), "unknown");
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface);
    if(read_file_first_line(path, carrier, sizeof(carrier)) != 0) {
        snprintf(carrier, sizeof(carrier), "?");
    }

    if(read_iface_ip(iface, ip, sizeof(ip)) == 0) {
        snprintf(buf, len, "%s  %s", oper, ip);
        if(color) {
            *color = 0x25C281;
        }
    } else if(strcmp(carrier, "1") == 0 || strcmp(oper, "up") == 0) {
        snprintf(buf, len, "%s  no IP", oper);
        if(color) {
            *color = 0xF5A524;
        }
    } else {
        snprintf(buf, len, "%s", oper);
        if(color) {
            *color = 0x9AA4AF;
        }
    }
}

static int find_backlight_device(void)
{
    DIR *dir;
    struct dirent *ent;
    char value[32];

    if(backlight_brightness_path[0] && path_exists(backlight_brightness_path)) {
        return 0;
    }

    backlight_brightness_path[0] = '\0';
    backlight_max_path[0] = '\0';
    backlight_max_value = 0;
    backlight_current_value = 0;

    dir = opendir("/sys/class/backlight");
    if(!dir) {
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        if(ent->d_name[0] == '.') {
            continue;
        }

        snprintf(backlight_brightness_path, sizeof(backlight_brightness_path),
                 "/sys/class/backlight/%s/brightness", ent->d_name);
        snprintf(backlight_max_path, sizeof(backlight_max_path),
                 "/sys/class/backlight/%s/max_brightness", ent->d_name);

        if(path_exists(backlight_brightness_path) && path_exists(backlight_max_path)) {
            closedir(dir);
            if(read_file_first_line(backlight_max_path, value, sizeof(value)) == 0) {
                backlight_max_value = atoi(value);
            }
            if(read_file_first_line(backlight_brightness_path, value, sizeof(value)) == 0) {
                backlight_current_value = atoi(value);
            }
            if(backlight_max_value <= 0) {
                backlight_max_value = 255;
            }
            return 0;
        }
    }

    closedir(dir);
    backlight_brightness_path[0] = '\0';
    backlight_max_path[0] = '\0';
    return -1;
}

static int backlight_min_value(void)
{
    if(backlight_max_value > 0 && backlight_max_value < BACKLIGHT_MIN_VALUE) {
        return backlight_max_value;
    }

    return BACKLIGHT_MIN_VALUE;
}

static int clamp_backlight_value(int value)
{
    int min_value = backlight_min_value();

    if(backlight_max_value > 0 && value > backlight_max_value) {
        value = backlight_max_value;
    }
    if(value < min_value) {
        value = min_value;
    }

    return value;
}

static int write_backlight_value(int value)
{
    FILE *fp;

    if(find_backlight_device() != 0) {
        return -1;
    }

    value = clamp_backlight_value(value);

    fp = fopen(backlight_brightness_path, "w");
    if(!fp) {
        return -1;
    }

    fprintf(fp, "%d\n", value);
    fclose(fp);
    backlight_current_value = value;
    return 0;
}

static void apply_display_brightness_pref(void)
{
    char value[32];
    char *end;
    long parsed;

    if(ui_prefs_get(DISPLAY_BRIGHTNESS_PREF_KEY, value, sizeof(value), "") != 0) {
        value[0] = '\0';
    }

    if(value[0] != '\0') {
        errno = 0;
        parsed = strtol(value, &end, 10);
        if(errno != 0 || end == value) {
            touch_trace_log("DISPLAY_BRIGHTNESS_PREF invalid=%s", value);
            return;
        }

        if(write_backlight_value((int)parsed) == 0) {
            touch_trace_log("DISPLAY_BRIGHTNESS_PREF applied=%d max=%d",
                            backlight_current_value, backlight_max_value);
        } else {
            touch_trace_log("DISPLAY_BRIGHTNESS_PREF apply failed value=%ld",
                            parsed);
        }
        return;
    }

    if(find_backlight_device() == 0 && backlight_current_value <= 0) {
        int target = backlight_max_value > 0 ? backlight_max_value : 255;

        if(write_backlight_value(target) == 0) {
            char pref_value[24];

            snprintf(pref_value, sizeof(pref_value), "%d",
                     backlight_current_value);
            ui_prefs_set(DISPLAY_BRIGHTNESS_PREF_KEY, pref_value);
            touch_trace_log("DISPLAY_BRIGHTNESS_PREF seeded=%d max=%d",
                            backlight_current_value, backlight_max_value);
        }
    }
}

static int has_video_node(void)
{
    DIR *dir = opendir("/dev");
    struct dirent *ent;

    if(!dir) {
        return 0;
    }

    while((ent = readdir(dir)) != NULL) {
        if(strncmp(ent->d_name, "video", 5) == 0) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static const char *read_input_name(const char *event_name, char *buf, size_t len)
{
    char path[128];
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", event_name);
    fp = fopen(path, "r");
    if(!fp) {
        return NULL;
    }

    if(!fgets(buf, len, fp)) {
        fclose(fp);
        return NULL;
    }

    buf[strcspn(buf, "\r\n")] = '\0';
    fclose(fp);
    return buf;
}

static int input_bit_is_set(const unsigned long *bits, int bit)
{
    return (bits[bit / INPUT_BITS_PER_LONG] &
            (1UL << (bit % INPUT_BITS_PER_LONG))) != 0;
}

static int input_event_has_touch_abs(const char *path)
{
    unsigned long abs_bits[INPUT_ABS_BITS_LEN];
    int fd;
    int has_mt;
    int has_single;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        return 0;
    }

    memset(abs_bits, 0, sizeof(abs_bits));
    if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        close(fd);
        return 0;
    }
    close(fd);

    has_mt = input_bit_is_set(abs_bits, ABS_MT_POSITION_X) &&
             input_bit_is_set(abs_bits, ABS_MT_POSITION_Y);
    has_single = input_bit_is_set(abs_bits, ABS_X) &&
                 input_bit_is_set(abs_bits, ABS_Y);
    return has_mt || has_single;
}

static const char *find_input_event(void)
{
    DIR *dir;
    struct dirent *ent;
    char fallback[64] = "";

    if(input_path[0]) {
        return input_path;
    }

    dir = opendir("/dev/input");
    if(!dir) {
        return NULL;
    }

    while((ent = readdir(dir)) != NULL) {
        char name[64];

        if(strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }

        char full_path[64];
        int has_touch_abs;

        snprintf(full_path, sizeof(full_path), "/dev/input/%s", ent->d_name);
        has_touch_abs = input_event_has_touch_abs(full_path);

        if(read_input_name(ent->d_name, name, sizeof(name)) &&
           has_touch_abs &&
           (strstr(name, "goodix") || strstr(name, "Goodix") ||
            strstr(name, "gt9895") || strstr(name, "GT9895") ||
            strstr(name, "touch") || strstr(name, "Touch"))) {
            snprintf(input_path, sizeof(input_path), "%s", full_path);
            closedir(dir);
            return input_path;
        }

        if(has_touch_abs && !fallback[0]) {
            snprintf(fallback, sizeof(fallback), "%s", full_path);
        }
    }

    closedir(dir);

    if(fallback[0]) {
        snprintf(input_path, sizeof(input_path), "%s", fallback);
        return input_path;
    }

    return NULL;
}

static int find_power_key_event(char *path, size_t path_len)
{
    DIR *dir;
    struct dirent *ent;

    if(!path || path_len == 0) {
        return -1;
    }
    path[0] = '\0';

    dir = opendir("/dev/input");
    if(!dir) {
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        char full_path[96];
        char name[128];
        int fd;

        if(strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "/dev/input/%s", ent->d_name);
        fd = open(full_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) {
            continue;
        }
        memset(name, 0, sizeof(name));
        if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
           strstr(name, POWER_KEY_NAME)) {
            close(fd);
            snprintf(path, path_len, "%s", full_path);
            closedir(dir);
            return 0;
        }
        close(fd);
    }

    closedir(dir);
    return -1;
}

static void power_key_publish_state(int pressed)
{
    uint64_t now = monotonic_us();

    pthread_mutex_lock(&power_key_lock);
    if(pressed && !power_key_pressed) {
        power_key_press_us = now;
    } else if(!pressed) {
        power_key_press_us = 0;
    }
    power_key_pressed = pressed ? 1 : 0;
    power_key_generation++;
    pthread_mutex_unlock(&power_key_lock);

    touch_trace_log("POWER_KEY_%s", pressed ? "DOWN" : "UP");
}

static void *power_key_thread_main(void *arg)
{
    struct input_event event;
    int fd = -1;

    (void)arg;

    while(!power_key_thread_stop) {
        ssize_t rd;

        if(fd < 0) {
            char path[64];

            if(find_power_key_event(path, sizeof(path)) != 0) {
                usleep(500000);
                continue;
            }
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if(fd < 0) {
                usleep(500000);
                continue;
            }
            snprintf(power_key_device_path, sizeof(power_key_device_path), "%s",
                     path);
            touch_trace_log("POWER_KEY_DEVICE %s", power_key_device_path);
        }

        rd = read(fd, &event, sizeof(event));
        if(rd == (ssize_t)sizeof(event)) {
            if(event.type == EV_KEY && event.code == KEY_POWER) {
                if(event.value == 1) {
                    power_key_publish_state(1);
                } else if(event.value == 0) {
                    power_key_publish_state(0);
                }
            }
            continue;
        }

        if(rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(10000);
            continue;
        }
        if(rd < 0 && errno == EINTR) {
            continue;
        }
        if(rd < 0) {
            touch_trace_log("POWER_KEY_READ_FAILED errno=%d (%s)", errno,
                            strerror(errno));
        }
        close(fd);
        fd = -1;
        usleep(200000);
    }

    if(fd >= 0) {
        close(fd);
    }
    touch_trace_log("POWER_KEY_STOP");
    return NULL;
}

static void start_power_key_monitor(void)
{
    if(power_key_thread_started) {
        return;
    }

    power_key_thread_stop = 0;
    if(pthread_create(&power_key_thread, NULL, power_key_thread_main, NULL) == 0) {
        power_key_thread_started = 1;
    } else {
        touch_trace_log("POWER_KEY_THREAD_FAILED");
    }
}

static void stop_power_key_monitor(void)
{
    if(!power_key_thread_started) {
        return;
    }

    power_key_thread_stop = 1;
    pthread_join(power_key_thread, NULL);
    power_key_thread_started = 0;
}

static int shutdown_lerp_int(int from, int to, int progress)
{
    if(progress < 0) {
        progress = 0;
    }
    if(progress > 100) {
        progress = 100;
    }
    return from + ((to - from) * progress) / 100;
}

static int shutdown_visual_progress_at(uint64_t now)
{
    uint64_t elapsed;
    uint64_t progress_elapsed = 0;
    int progress;

    if(!shutdown_visual_active || !shutdown_visual_start_us) {
        return 0;
    }

    elapsed = now > shutdown_visual_start_us ?
              now - shutdown_visual_start_us : 0;
    if(elapsed > POWER_SHUTDOWN_PREVIEW_DELAY_US) {
        progress_elapsed = elapsed - POWER_SHUTDOWN_PREVIEW_DELAY_US;
    }
    progress = (int)(progress_elapsed * 100ULL / POWER_SHUTDOWN_PROGRESS_US);
    if(progress > 100) {
        progress = 100;
    }
    return progress;
}

static void shutdown_visual_create(void)
{
    lv_obj_t *arc;
    lv_obj_t *circle;
    lv_obj_t *icon;
    lv_obj_t *brand;
    lv_obj_t *title;
    int w = display_logical_width();
    int h = display_logical_height();
    int landscape = display_orientation_is_landscape();
    int center_y = landscape ? h / 2 : h / 2 - 50;
    int circle_size = landscape ? 104 : 132;
    int arc_size = circle_size + (landscape ? 24 : 30);
    int title_y = center_y + circle_size / 2 + (landscape ? 24 : 34);
    int hint_y = title_y + (landscape ? 90 : 110);

    if(shutdown_overlay_obj && lv_obj_is_valid(shutdown_overlay_obj)) {
        return;
    }

    shutdown_overlay_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(shutdown_overlay_obj, 0, 0);
    lv_obj_set_size(shutdown_overlay_obj, w, h);
    lv_obj_set_style_bg_color(shutdown_overlay_obj, lv_color_hex(0x030507), 0);
    lv_obj_set_style_bg_opa(shutdown_overlay_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shutdown_overlay_obj, 0, 0);
    lv_obj_set_style_pad_all(shutdown_overlay_obj, 0, 0);
    lv_obj_clear_flag(shutdown_overlay_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(shutdown_overlay_obj, LV_OBJ_FLAG_CLICKABLE);

    brand = label(shutdown_overlay_obj, "LILYGO", &lv_font_montserrat_24,
                  0xA7F3D0);
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, landscape ? 28 : 62);

    arc = lv_arc_create(shutdown_overlay_obj);
    lv_obj_set_size(arc, arc_size, arc_size);
    lv_obj_set_pos(arc, (w - arc_size) / 2, center_y - arc_size / 2);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(arc, landscape ? 6 : 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, landscape ? 6 : 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1D2834), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x25C281), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    shutdown_progress_arc = arc;

    circle = lv_obj_create(shutdown_overlay_obj);
    lv_obj_set_size(circle, circle_size, circle_size);
    lv_obj_set_pos(circle, (w - circle_size) / 2, center_y - circle_size / 2);
    lv_obj_set_style_radius(circle, circle_size / 2, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x0F1720), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_shadow_width(circle, 28, 0);
    lv_obj_set_style_shadow_color(circle, lv_color_hex(0x166534), 0);
    lv_obj_set_style_shadow_opa(circle, LV_OPA_40, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    icon = label(circle, LV_SYMBOL_POWER,
                 landscape ? &lv_font_montserrat_32 : &lv_font_montserrat_48,
                 0xD1FAE5);
    lv_obj_center(icon);

    title = label(shutdown_overlay_obj, "Powering off", &lv_font_montserrat_32,
                  0xF2F5F8);
    lv_obj_set_width(title, w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 0, title_y);

    shutdown_detail_label = label(shutdown_overlay_obj, "Saving display state",
                                  &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(shutdown_detail_label, w);
    lv_obj_set_style_text_align(shutdown_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(shutdown_detail_label, 0, title_y + 48);

    shutdown_hint_label = label(shutdown_overlay_obj, "Hold power key",
                                &lv_font_montserrat_16, 0x64748B);
    lv_obj_set_width(shutdown_hint_label, w);
    lv_obj_set_style_text_align(shutdown_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(shutdown_hint_label, 0, hint_y);

    lv_obj_move_foreground(shutdown_overlay_obj);
    start_opa_anim(shutdown_overlay_obj, LV_OPA_TRANSP, LV_OPA_COVER, 0, 220);
}

static void shutdown_visual_destroy(void)
{
    if(shutdown_overlay_obj && lv_obj_is_valid(shutdown_overlay_obj)) {
        lv_obj_delete_async(shutdown_overlay_obj);
    }
    shutdown_overlay_obj = NULL;
    shutdown_progress_arc = NULL;
    shutdown_detail_label = NULL;
    shutdown_hint_label = NULL;
}

static void shutdown_visual_begin(uint64_t press_us)
{
    int screen;
    int keyboard;

    if(shutdown_visual_active) {
        return;
    }

    screen = ui_hardware_screen_backlight_get();
    keyboard = ui_hardware_keyboard_backlight_get();
    shutdown_saved_screen_backlight = screen > 0 ? screen : 80;
    shutdown_saved_keyboard_backlight = keyboard >= 0 ? keyboard : 0;
    shutdown_visual_start_us = press_us;
    shutdown_visual_last_update_us = 0;
    shutdown_last_progress = -1;
    shutdown_last_fade_log_progress = -1;
    shutdown_visual_committed = 0;
    shutdown_visual_active = 1;
    shutdown_visual_create();
    touch_trace_log("SHUTDOWN_VISUAL_BEGIN screen=%d keyboard=%d rotation=%d",
                    shutdown_saved_screen_backlight,
                    shutdown_saved_keyboard_backlight,
                    display_rotation_degrees);
}

static void shutdown_visual_cancel(void)
{
    if(!shutdown_visual_active || shutdown_visual_committed) {
        return;
    }

    ui_hardware_shutdown_backlights_apply(shutdown_saved_screen_backlight,
                                          shutdown_saved_keyboard_backlight);
    shutdown_visual_destroy();
    shutdown_visual_active = 0;
    shutdown_visual_start_us = 0;
    shutdown_visual_last_update_us = 0;
    shutdown_last_progress = -1;
    shutdown_last_fade_log_progress = -1;
    shutdown_visual_committed = 0;
    shutdown_visual_poweroff_started = 0;
    shutdown_visual_commit_us = 0;
    touch_trace_log("SHUTDOWN_VISUAL_CANCEL restore_screen=%d restore_keyboard=%d",
                    shutdown_saved_screen_backlight,
                    shutdown_saved_keyboard_backlight);
    request_fast_refresh();
}

static void shutdown_visual_start_poweroff(void)
{
    int rc;

    if(shutdown_visual_poweroff_started) {
        return;
    }
    shutdown_visual_poweroff_started = 1;
    request_fast_refresh();
    rc = system("(sync; poweroff -f || poweroff) "
                ">/tmp/k230_shutdown_visual.log 2>&1 &");
    touch_trace_log("SHUTDOWN_VISUAL_POWEROFF rc=%d", rc);
}

static void shutdown_visual_commit(void)
{
    if(!shutdown_visual_active || shutdown_visual_committed) {
        return;
    }

    shutdown_visual_committed = 1;
    shutdown_visual_commit_us = monotonic_us();
    if(shutdown_progress_arc && lv_obj_is_valid(shutdown_progress_arc)) {
        lv_arc_set_value(shutdown_progress_arc, 100);
    }
    if(shutdown_detail_label && lv_obj_is_valid(shutdown_detail_label)) {
        lv_label_set_text(shutdown_detail_label, "Dimming backlights");
    }
    if(shutdown_hint_label && lv_obj_is_valid(shutdown_hint_label)) {
        lv_label_set_text(shutdown_hint_label, "Power off committed");
    }
    request_fast_refresh();
    touch_trace_log("SHUTDOWN_VISUAL_COMMIT fade_us=%u",
                    POWER_SHUTDOWN_FADE_US);
}

static void shutdown_visual_update(uint64_t now)
{
    uint64_t elapsed;
    uint64_t fade_elapsed = 0;
    uint64_t fade_start_us = POWER_SHUTDOWN_PREVIEW_DELAY_US +
                             POWER_SHUTDOWN_PROGRESS_US;
    int progress;
    int fade_progress = 0;
    int screen_value;
    int keyboard_value;

    if(!shutdown_visual_active) {
        return;
    }
    if(shutdown_visual_last_update_us &&
       now - shutdown_visual_last_update_us < POWER_SHUTDOWN_UPDATE_US) {
        return;
    }
    shutdown_visual_last_update_us = now;

    elapsed = now > shutdown_visual_start_us ?
              now - shutdown_visual_start_us : 0;

    progress = shutdown_visual_progress_at(now);

    if(shutdown_visual_committed) {
        fade_elapsed = shutdown_visual_commit_us && now > shutdown_visual_commit_us ?
                       now - shutdown_visual_commit_us : 0;
        fade_progress = (int)(fade_elapsed * 100ULL / POWER_SHUTDOWN_FADE_US);
        if(fade_progress > 100) {
            fade_progress = 100;
        }
        screen_value = shutdown_lerp_int(shutdown_saved_screen_backlight, 0,
                                         fade_progress);
        keyboard_value = shutdown_lerp_int(shutdown_saved_keyboard_backlight, 0,
                                           fade_progress);
        ui_hardware_shutdown_backlights_step_apply(screen_value,
                                                   keyboard_value);
        if(shutdown_last_fade_log_progress < 0 ||
           fade_progress >= 100 ||
           fade_progress - shutdown_last_fade_log_progress >= 10) {
            shutdown_last_fade_log_progress = fade_progress;
            touch_trace_log("SHUTDOWN_VISUAL_FADE progress=%d screen=%d keyboard=%d",
                            fade_progress, screen_value, keyboard_value);
        }
        if(shutdown_progress_arc && lv_obj_is_valid(shutdown_progress_arc)) {
            lv_arc_set_value(shutdown_progress_arc, 100);
        }
        if(shutdown_detail_label && lv_obj_is_valid(shutdown_detail_label)) {
            lv_label_set_text(shutdown_detail_label,
                              fade_progress >= 100 ? "Shutting down" :
                              "Dimming backlights");
        }
        if(shutdown_hint_label && lv_obj_is_valid(shutdown_hint_label)) {
            lv_label_set_text(shutdown_hint_label,
                              fade_progress >= 100 ? "Powering off" :
                              "Release is ignored");
        }
        request_fast_refresh();
        if(fade_progress >= 100) {
            shutdown_visual_start_poweroff();
        }
        return;
    }
    (void)fade_start_us;

    if(progress != shutdown_last_progress) {
        shutdown_last_progress = progress;
        if(shutdown_progress_arc && lv_obj_is_valid(shutdown_progress_arc)) {
            lv_arc_set_value(shutdown_progress_arc, progress);
        }
        if(shutdown_detail_label && lv_obj_is_valid(shutdown_detail_label)) {
            lv_label_set_text(shutdown_detail_label,
                              progress >= 100 ? "Shutting down" :
                              "Keep holding power key");
        }
        if(shutdown_hint_label && lv_obj_is_valid(shutdown_hint_label) &&
           progress >= 88) {
            lv_label_set_text(shutdown_hint_label,
                              progress >= 100 ? "Dimming backlights" :
                              "Almost there");
        }
        request_fast_refresh();
    } else if(fade_progress > 0) {
        request_fast_refresh();
    }

    if(progress >= 100) {
        shutdown_visual_commit();
    }
}

static void power_key_shutdown_visual_poll(void)
{
    uint64_t now = monotonic_us();
    uint64_t press_us;
    int pressed;
    unsigned int generation;

    pthread_mutex_lock(&power_key_lock);
    pressed = power_key_pressed;
    press_us = power_key_press_us;
    generation = power_key_generation;
    pthread_mutex_unlock(&power_key_lock);
    (void)generation;

    if(pressed && press_us &&
       now - press_us >= POWER_SHUTDOWN_PREVIEW_DELAY_US) {
        shutdown_visual_begin(press_us);
    }

    if(!pressed && shutdown_visual_active) {
        if(shutdown_visual_committed ||
           shutdown_visual_progress_at(now) >= 100) {
            shutdown_visual_commit();
        } else {
            shutdown_visual_cancel();
            return;
        }
    }

    shutdown_visual_update(now);
}

static void update_time_labels(lv_timer_t *timer)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char time_buf[16];
    char date_buf[32];

    (void)timer;

    localtime_r(&now, &tm_now);
    strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_now);
    strftime(date_buf, sizeof(date_buf), "%a, %b %d", &tm_now);

    if(time_label) {
        lv_label_set_text(time_label, time_buf);
    }
    if(home_time_label) {
        lv_label_set_text(home_time_label, time_buf);
    }
    if(date_label) {
        lv_label_set_text(date_label, date_buf);
    }
}

static void apply_touch_transform(void)
{
    lv_timer_t *read_timer;
    bool swap_axes = false;
    int32_t calib_x1 = 0;
    int32_t calib_y1 = 0;
    int32_t calib_x2 = TOUCH_MAX_X;
    int32_t calib_y2 = TOUCH_MAX_Y;

    ui_multitouch_set_transform(TOUCH_MAX_X, TOUCH_MAX_Y,
                                display_logical_width(),
                                display_logical_height(),
                                display_rotation_degrees);

    if(!evdev_indev) {
        return;
    }

    if(display_rotation_degrees == 90) {
        swap_axes = true;
        calib_x1 = 0;
        calib_y1 = TOUCH_MAX_X;
        calib_x2 = TOUCH_MAX_Y;
        calib_y2 = 0;
    } else if(display_rotation_degrees == 180) {
        calib_x1 = TOUCH_MAX_X;
        calib_y1 = TOUCH_MAX_Y;
        calib_x2 = 0;
        calib_y2 = 0;
    } else if(display_rotation_degrees == 270) {
        swap_axes = true;
        calib_x1 = TOUCH_MAX_Y;
        calib_y1 = 0;
        calib_x2 = 0;
        calib_y2 = TOUCH_MAX_X;
    }

    lv_evdev_set_swap_axes(evdev_indev, swap_axes);
    lv_evdev_set_calibration(evdev_indev, calib_x1, calib_y1, calib_x2,
                             calib_y2);
    lv_indev_set_scroll_limit(evdev_indev, 4);
    lv_indev_set_scroll_throw(evdev_indev, 0);
    lv_indev_set_long_press_time(evdev_indev, 250);
    lv_indev_set_long_press_repeat_time(evdev_indev, 90);

    read_timer = lv_indev_get_read_timer(evdev_indev);
    if(read_timer) {
        lv_timer_set_period(read_timer, 5);
    }

    touch_trace_log("TOUCH_TRANSFORM rotation=%d logical=%dx%d swap=%d "
                    "calib=(%ld,%ld,%ld,%ld)",
                    display_rotation_degrees, display_logical_width(),
                    display_logical_height(), swap_axes ? 1 : 0,
                    (long)calib_x1, (long)calib_y1, (long)calib_x2,
                    (long)calib_y2);
}

static void apply_display_timing(lv_display_t *disp)
{
    lv_timer_t *refr_timer;

    if(!disp) {
        return;
    }

    refr_timer = lv_display_get_refr_timer(disp);
    if(refr_timer) {
        lv_timer_set_period(refr_timer, DISPLAY_REFR_PERIOD_MS);
        touch_trace_log("DISPLAY_REFR period=%ums", DISPLAY_REFR_PERIOD_MS);
    }
}

static void apply_display_orientation(lv_display_t *disp)
{
    int drm_rotation;

    if(!disp) {
        return;
    }

    drm_rotation = display_drm_rotation_from_orientation();
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    touch_trace_log("DISPLAY_ORIENTATION value=%s drm_rotation=%d "
                    "lvgl_rotation=0 logical=%ldx%ld",
                    display_orientation_value, drm_rotation,
                    (long)lv_display_get_horizontal_resolution(disp),
                    (long)lv_display_get_vertical_resolution(disp));
    if(app_screen) {
        style_fullscreen_root(app_screen);
        apply_ui_stage_transform();
    }
    if(evdev_indev) {
        apply_touch_transform();
    }
}

static void request_fast_refresh(void)
{
    lv_timer_t *refr_timer = lv_display_get_refr_timer(NULL);

    if(refr_timer) {
        lv_timer_ready(refr_timer);
    }
}

void app_request_fast_refresh(void)
{
    request_fast_refresh();
}

void app_refresh_current_page(void)
{
    render_page(current_page, LV_SCREEN_LOAD_ANIM_NONE, 0);
}

int app_current_page_is(page_id_t page)
{
    return current_page == page;
}

static int display_logical_width(void)
{
    return main_display ? (int)lv_display_get_horizontal_resolution(main_display) :
           SCREEN_W;
}

static int display_logical_height(void)
{
    return main_display ? (int)lv_display_get_vertical_resolution(main_display) :
           SCREEN_H;
}

int app_display_logical_width(void)
{
    return display_logical_width();
}

int app_display_logical_height(void)
{
    return display_logical_height();
}

static void style_fullscreen_root(lv_obj_t *obj)
{
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, display_logical_width(), display_logical_height());
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0B0D10), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
}

static void apply_ui_stage_transform(void)
{
    if(!ui_stage_obj) {
        return;
    }

    lv_obj_set_size(ui_stage_obj, display_logical_width(),
                    display_logical_height());
    lv_obj_set_style_bg_opa(ui_stage_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_stage_obj, 0, 0);
    lv_obj_set_style_pad_all(ui_stage_obj, 0, 0);
    lv_obj_clear_flag(ui_stage_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_stage_obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_transform_pivot_x(ui_stage_obj, 0, 0);
    lv_obj_set_style_transform_pivot_y(ui_stage_obj, 0, 0);

    lv_obj_set_pos(ui_stage_obj, 0, 0);
    lv_obj_set_style_translate_x(ui_stage_obj, 0, 0);
    lv_obj_set_style_translate_y(ui_stage_obj, 0, 0);
    lv_obj_set_style_transform_rotation(ui_stage_obj, 0, 0);
    lv_obj_set_style_transform_width(ui_stage_obj, 0, 0);
    lv_obj_set_style_transform_height(ui_stage_obj, 0, 0);
    touch_trace_log("UI_STAGE rotation=%d pos=0x0 size=%dx%d transform=0 "
                    "logical=%dx%d",
                    display_rotation_degrees, display_logical_width(),
                    display_logical_height(), display_logical_width(),
                    display_logical_height());
}

static lv_obj_t *ensure_ui_stage(void)
{
    if(!ui_stage_obj) {
        ui_stage_obj = lv_obj_create(app_screen);
        lv_obj_move_background(ui_stage_obj);
    }
    apply_ui_stage_transform();
    return ui_stage_obj;
}

static lv_obj_t *create_page_root(int x)
{
    lv_obj_t *root = lv_obj_create(ensure_ui_stage());

    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, display_logical_width(), display_logical_height());
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, edge_back_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, edge_back_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, edge_back_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, edge_back_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_x(root, x);
    return root;
}

static void page_slide_completed_cb(lv_anim_t *anim)
{
    lv_obj_t *old_page = (lv_obj_t *)lv_anim_get_user_data(anim);

    if(old_page) {
        lv_obj_delete_async(old_page);
    }
    transition_old_page = NULL;
    page_transition_active = 0;
    if(app_screen) {
        lv_obj_invalidate(app_screen);
    }

    if(current_page == PAGE_MOTION) {
        motion_reset_stats(monotonic_us());
    }

    touch_trace_log("PAGE_CONTAINER_ANIM_END page=%s", page_name(current_page));
    request_fast_refresh();
}

static void page_slide_set_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);

    if(app_screen) {
        lv_obj_invalidate(app_screen);
    }
}

static void page_slide_set_y_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);

    if(app_screen) {
        lv_obj_invalidate(app_screen);
    }
}

static int page_transition_root_offset(int32_t *from_x, int32_t *from_y)
{
    if(!from_x || !from_y) {
        return 0;
    }

    *from_x = 0;
    *from_y = 0;
    if(strcmp(page_transition_effect, "slide_left") == 0) {
        *from_x = display_logical_width();
    } else if(strcmp(page_transition_effect, "slide_right") == 0) {
        *from_x = -display_logical_width();
    } else if(strcmp(page_transition_effect, "slide_up") == 0) {
        *from_y = display_logical_height();
    } else if(strcmp(page_transition_effect, "cover") == 0) {
        *from_y = display_logical_height() / 2;
    } else {
        return 0;
    }
    return 1;
}

static void start_page_move_anim(lv_obj_t *new_page, lv_obj_t *old_page,
                                 int32_t from_x, int32_t from_y,
                                 uint32_t duration)
{
    lv_anim_t anim;
    lv_anim_exec_xcb_t exec_cb;

    transition_old_page = old_page;
    page_transition_active = 1;

    exec_cb = from_y != 0 ? page_slide_set_y_cb : page_slide_set_x_cb;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, new_page);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, from_y != 0 ? from_y : from_x, 0);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_user_data(&anim, old_page);
    lv_anim_set_completed_cb(&anim, page_slide_completed_cb);
    touch_trace_log("PAGE_CONTAINER_ANIM_FULL_INVALIDATE on");
    if(strcmp(page_transition_effect, "cover") == 0) {
        lv_obj_set_style_opa(new_page, LV_OPA_TRANSP, 0);
        start_opa_anim(new_page, LV_OPA_TRANSP, LV_OPA_COVER, 0, duration);
    }
    lv_anim_start(&anim);
}

static int nav_transition_busy(page_id_t target)
{
    if(page_transition_active || lv_display_get_screen_loading(NULL)) {
        touch_trace_log("NAV_BUSY target=%s current=%s", page_name(target),
                        page_name(current_page));
        return 1;
    }

    return 0;
}

static void home_restore_scroll_async(void *user_data)
{
    (void)user_data;
    if(home_saved_scroll_valid && home_apps_scroll &&
       lv_obj_is_valid(home_apps_scroll)) {
        lv_obj_scroll_to_y(home_apps_scroll, home_saved_scroll_y, LV_ANIM_OFF);
        home_saved_scroll_valid = 0;
    }
    home_restore_scroll_on_create = 0;
    request_fast_refresh();
}

static void home_capture_scroll(void)
{
    if(home_apps_scroll && lv_obj_is_valid(home_apps_scroll)) {
        home_saved_scroll_y = lv_obj_get_scroll_y(home_apps_scroll);
        home_saved_scroll_valid = 1;
    }
}

static void home_clear_saved_scroll(void)
{
    home_saved_scroll_y = 0;
    home_saved_scroll_valid = 0;
    home_restore_scroll_on_create = 0;
}

static void home_prepare_open(int restore_saved_scroll)
{
    if(restore_saved_scroll && home_saved_scroll_valid) {
        home_restore_scroll_on_create = 1;
        return;
    }
    home_clear_saved_scroll();
}

static void home_apply_scroll_restore(lv_obj_t *apps)
{
    home_apps_scroll = apps;
    if(home_restore_scroll_on_create && home_saved_scroll_valid) {
        lv_async_call(home_restore_scroll_async, NULL);
        return;
    }
    home_restore_scroll_on_create = 0;
    if(apps) {
        lv_obj_scroll_to_y(apps, 0, LV_ANIM_OFF);
    }
}

static page_id_t nav_fallback_parent(page_id_t page)
{
    switch(page) {
    case PAGE_WIFI:
    case PAGE_ETHERNET:
    case PAGE_BLE:
    case PAGE_CELLULAR:
    case PAGE_USB_MODEM:
    case PAGE_DISPLAY:
    case PAGE_LANGUAGE:
    case PAGE_TIME:
    case PAGE_KEYBOARD_SETTINGS:
        return PAGE_SETTINGS;
    case PAGE_KEYBOARD_HOTKEYS:
        return PAGE_KEYBOARD_SETTINGS;
    case PAGE_KEYBOARD_HOTKEY_ACTION:
        return PAGE_KEYBOARD_HOTKEYS;
    case PAGE_AUDIO_SETTINGS:
    case PAGE_AUDIO_OUTPUT:
    case PAGE_NOTIFICATION_SETTINGS:
    case PAGE_FAN:
    case PAGE_SENSORS:
    case PAGE_BQ25896:
    case PAGE_BATTERY:
    case PAGE_APP_STARTUP:
    case PAGE_SYSTEM:
    case PAGE_ABOUT:
        return PAGE_SETTINGS;
    default:
        return PAGE_HOME;
    }
}

static int page_is_settings_subpage(page_id_t page)
{
    return nav_fallback_parent(page) == PAGE_SETTINGS;
}

static void transition_to_page(page_id_t page, lv_screen_load_anim_t anim_type)
{
    if(page == current_page) {
        return;
    }

    render_page(page, anim_type, PAGE_TRANSITION_ANIM_MS);
}


static void nav_to(page_id_t page)
{
    if(page == current_page) {
        return;
    }
    if(nav_transition_busy(page)) {
        return;
    }

    if(current_page == PAGE_SETTINGS) {
        ui_settings_capture_scroll();
    }
    if(current_page == PAGE_HOME) {
        home_capture_scroll();
    }
    if(page == PAGE_GALLERY) {
        camera_gallery_view_open = 0;
    }
    if(page == PAGE_SETTINGS) {
        ui_settings_prepare_open(0);
    }
    if(page == PAGE_HOME) {
        home_prepare_open(1);
    }
    if(current_page == PAGE_SETTINGS && page_is_settings_subpage(page)) {
        settings_subpage_context = 1;
    } else if(page == PAGE_SETTINGS || page == PAGE_HOME) {
        settings_subpage_context = 0;
    }

    if(page_stack_len < (int)(sizeof(page_stack) / sizeof(page_stack[0]))) {
        page_stack[page_stack_len++] = current_page;
    }

    transition_to_page(page, LV_SCREEN_LOAD_ANIM_OVER_LEFT);
}

static int camera_gallery_handle_back(void)
{
    if(current_page != PAGE_GALLERY || !camera_gallery_view_open) {
        return 0;
    }

    camera_gallery_view_open = 0;
    touch_trace_log("GALLERY_BACK_TO_GRID selected=%d count=%d",
                    camera_gallery_selected, camera_gallery_count);
    render_page(PAGE_GALLERY, LV_SCREEN_LOAD_ANIM_NONE, 0);
    return 1;
}

static void nav_back(void)
{
    if(current_page == PAGE_NRF52840_DFU && ui_nrf52840_dfu_is_running()) {
        return;
    }
    if(camera_gallery_handle_back()) {
        return;
    }
    if(current_page == PAGE_NES && ui_nes_handle_back()) {
        return;
    }
    if(current_page == PAGE_VIDEO && ui_video_player_handle_back()) {
        return;
    }
    if(current_page == PAGE_MESHTASTIC && ui_meshtastic_handle_back()) {
        return;
    }
    if(current_page == PAGE_LORAWAN && ui_lorawan_handle_back()) {
        return;
    }

    if(page_is_settings_subpage(current_page) && settings_subpage_context &&
       (page_stack_len <= 0 ||
        page_stack[page_stack_len - 1] != PAGE_SETTINGS)) {
        if(nav_transition_busy(PAGE_SETTINGS)) {
            return;
        }
        ui_settings_prepare_open(1);
        settings_subpage_context = 0;
        transition_to_page(PAGE_SETTINGS, LV_SCREEN_LOAD_ANIM_OVER_RIGHT);
        return;
    }

    if(page_stack_len > 0) {
        page_id_t target = page_stack[page_stack_len - 1];

        if(nav_transition_busy(target)) {
            return;
        }

        if(current_page == PAGE_SETTINGS) {
            ui_settings_capture_scroll();
        }
        if(current_page == PAGE_HOME) {
            home_capture_scroll();
        }
        if(target == PAGE_SETTINGS) {
            ui_settings_prepare_open(1);
            settings_subpage_context = 0;
        } else {
            ui_settings_clear_saved_scroll();
        }
        if(target == PAGE_HOME) {
            home_prepare_open(1);
        }

        page_stack_len--;
        transition_to_page(target, LV_SCREEN_LOAD_ANIM_OVER_RIGHT);
    } else if(current_page != PAGE_HOME) {
        page_id_t fallback = nav_fallback_parent(current_page);

        if(nav_transition_busy(fallback)) {
            return;
        }
        if(current_page == PAGE_SETTINGS) {
            ui_settings_clear_saved_scroll();
        }
        if(fallback == PAGE_SETTINGS) {
            ui_settings_prepare_open(1);
            settings_subpage_context = 0;
        } else if(fallback == PAGE_HOME) {
            home_prepare_open(1);
        }
        transition_to_page(fallback, LV_SCREEN_LOAD_ANIM_OVER_RIGHT);
    }
}

static void edge_back_load_pref(void)
{
    if(edge_back_pref_loaded) {
        return;
    }
    edge_back_enabled = 1;
    edge_back_pref_loaded = 1;
}

int app_edge_back_enabled(void)
{
    edge_back_load_pref();
    return edge_back_enabled;
}

void app_set_edge_back_enabled(int enabled)
{
    (void)enabled;
    edge_back_enabled = 1;
    edge_back_pref_loaded = 1;
    edge_back_log("SET ignored always_on=1");
}

static int edge_back_clamp(int value, int min_value, int max_value)
{
    if(value < min_value) {
        return min_value;
    }
    if(value > max_value) {
        return max_value;
    }
    return value;
}

static void edge_back_hint_fade_done(lv_anim_t *anim)
{
    lv_obj_t *obj = anim ? (lv_obj_t *)anim->var : NULL;

    if(obj && lv_obj_is_valid(obj)) {
        if(obj == edge_back_hint_obj) {
            edge_back_hint_obj = NULL;
            edge_back_hint_label = NULL;
            edge_back_hint_opa = 0;
        }
        lv_obj_delete_async(obj);
    }
}

static void edge_back_hint_ensure(void)
{
    if(edge_back_hint_obj && lv_obj_is_valid(edge_back_hint_obj)) {
        return;
    }

    edge_back_hint_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(edge_back_hint_obj, 44, 86);
    lv_obj_set_style_radius(edge_back_hint_obj, 24, 0);
    lv_obj_set_style_bg_color(edge_back_hint_obj, lv_color_hex(0x202A36), 0);
    lv_obj_set_style_bg_opa(edge_back_hint_obj, LV_OPA_80, 0);
    lv_obj_set_style_border_width(edge_back_hint_obj, 0, 0);
    lv_obj_set_style_shadow_width(edge_back_hint_obj, 12, 0);
    lv_obj_set_style_shadow_opa(edge_back_hint_obj, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(edge_back_hint_obj, lv_color_hex(0x3DA5FF), 0);
    lv_obj_clear_flag(edge_back_hint_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(edge_back_hint_obj, LV_OBJ_FLAG_CLICKABLE);

    edge_back_hint_label = label(edge_back_hint_obj, LV_SYMBOL_LEFT,
                                 &lv_font_montserrat_28, 0xDCEBFF);
    lv_obj_center(edge_back_hint_label);
}

static void edge_back_hint_update(int direction, int y, int distance)
{
    int progress;
    int pill_w;
    int pill_h;
    int x;
    int max_y;
    int width = display_logical_width();
    int height = display_logical_height();

    if(direction == 0) {
        return;
    }

    edge_back_hint_ensure();
    if(!edge_back_hint_obj || !lv_obj_is_valid(edge_back_hint_obj)) {
        return;
    }

    progress = edge_back_clamp(distance * 100 / EDGE_BACK_TRIGGER_PX, 0, 100);
    pill_w = 42 + progress * 18 / 100;
    pill_h = 76 + progress * 18 / 100;
    max_y = height - pill_h - 10;
    y = edge_back_clamp(y - pill_h / 2, 10, max_y > 10 ? max_y : 10);
    if(direction > 0) {
        x = -10 + progress * 12 / 100;
        lv_label_set_text(edge_back_hint_label, LV_SYMBOL_LEFT);
    } else {
        x = width - pill_w + 10 - progress * 12 / 100;
        lv_label_set_text(edge_back_hint_label, LV_SYMBOL_RIGHT);
    }

    edge_back_hint_opa = 112 + progress * 112 / 100;
    lv_anim_delete(edge_back_hint_obj, set_opa_anim_cb);
    lv_obj_set_size(edge_back_hint_obj, pill_w, pill_h);
    lv_obj_set_pos(edge_back_hint_obj, x, y);
    lv_obj_set_style_opa(edge_back_hint_obj, (lv_opa_t)edge_back_hint_opa, 0);
    lv_obj_center(edge_back_hint_label);
}

static void edge_back_hint_hide(void)
{
    lv_anim_t anim;

    if(!edge_back_hint_obj || !lv_obj_is_valid(edge_back_hint_obj) ||
       edge_back_hint_opa <= 0) {
        return;
    }

    lv_anim_delete(edge_back_hint_obj, set_opa_anim_cb);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, edge_back_hint_obj);
    lv_anim_set_exec_cb(&anim, set_opa_anim_cb);
    lv_anim_set_values(&anim, edge_back_hint_opa, 0);
    lv_anim_set_duration(&anim, 120);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, edge_back_hint_fade_done);
    lv_anim_start(&anim);
    edge_back_hint_opa = 0;
}

static void edge_back_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_point_t point;
    uint64_t now;
    int dx;
    int dy;

    edge_back_load_pref();
    now = ui_monotonic_us();
    if(edge_back_lvgl_suppress_until_us &&
       now < edge_back_lvgl_suppress_until_us) {
        if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
           code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
            edge_back_log("LVGL_SUPPRESS code=%d page=%s",
                          (int)code, page_name(current_page));
            if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
                edge_back_tracking = 0;
                edge_back_direction = 0;
                edge_back_hint_hide();
            }
        }
        return;
    }
    if(!edge_back_enabled || !evdev_indev || current_page == PAGE_HOME ||
       page_transition_active) {
        if(code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED ||
           code == LV_EVENT_PRESS_LOST || code == LV_EVENT_PRESSING) {
            edge_back_log("LVGL_SKIP code=%d enabled=%d indev=%d page=%s transition=%d",
                          (int)code, edge_back_enabled, evdev_indev ? 1 : 0,
                          page_name(current_page), page_transition_active);
        }
        edge_back_tracking = 0;
        edge_back_direction = 0;
        edge_back_hint_hide();
        return;
    }

    lv_indev_get_point(evdev_indev, &point);
    if(code == LV_EVENT_PRESSED) {
        edge_back_direction = edge_back_direction_for_x(point.x);
        edge_back_tracking = edge_back_direction != 0;
        edge_back_start_point = point;
        if(edge_back_tracking) {
            edge_back_log("LVGL_START page=%s edge=%s x=%ld y=%ld start_px=%d "
                          "width=%d",
                          page_name(current_page),
                          edge_back_edge_name(edge_back_direction),
                          (long)point.x,
                          (long)point.y, EDGE_BACK_START_PX,
                          display_logical_width());
            edge_back_hint_update(edge_back_direction, point.y, 0);
        } else if(edge_back_is_near_edge(point.x)) {
            edge_back_log("LVGL_NEAR_EDGE_SKIP page=%s x=%ld y=%ld start_px=%d "
                          "width=%d",
                          page_name(current_page), (long)point.x,
                          (long)point.y, EDGE_BACK_START_PX,
                          display_logical_width());
        }
        return;
    }

    if(code == LV_EVENT_PRESSING) {
        if(!edge_back_tracking) {
            return;
        }
        dx = point.x - edge_back_start_point.x;
        dy = point.y - edge_back_start_point.y;
        if(abs(dy) > EDGE_BACK_MAX_VERTICAL_PX) {
            edge_back_hint_hide();
            return;
        }
        edge_back_hint_update(edge_back_direction, point.y,
                              edge_back_direction * dx);
        return;
    }

    if(code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }
    if(!edge_back_tracking) {
        edge_back_hint_hide();
        return;
    }
    edge_back_tracking = 0;
    dx = point.x - edge_back_start_point.x;
    dy = point.y - edge_back_start_point.y;
    if(edge_back_direction * dx >= EDGE_BACK_TRIGGER_PX &&
       abs(dy) <= EDGE_BACK_MAX_VERTICAL_PX) {
        edge_back_raw_pending = 0;
        edge_back_hint_hide();
        edge_back_log("LVGL_TRIGGER page=%s edge=%s dx=%d dy=%d",
                      page_name(current_page),
                      edge_back_edge_name(edge_back_direction), dx, dy);
        if(current_page == PAGE_CELLULAR && ui_cellular_handle_back()) {
            trace_ui_action("LVGL_EDGE_BACK_INNER", PAGE_CELLULAR);
            edge_back_direction = 0;
            return;
        }
        if(camera_gallery_handle_back()) {
            trace_ui_action("LVGL_EDGE_BACK_INNER", PAGE_GALLERY);
            edge_back_direction = 0;
            return;
        }
        trace_ui_action("LVGL_EDGE_BACK", page_stack_len > 0 ?
                        page_stack[page_stack_len - 1] : PAGE_HOME);
        nav_back();
    } else {
        edge_back_log("LVGL_CANCEL page=%s edge=%s dx=%d dy=%d trigger=%d max_dy=%d",
                      page_name(current_page),
                      edge_back_edge_name(edge_back_direction), dx, dy,
                      EDGE_BACK_TRIGGER_PX,
                      EDGE_BACK_MAX_VERTICAL_PX);
        edge_back_hint_hide();
    }
    edge_back_direction = 0;
}

static void edge_back_update_raw_hint(void)
{
    static uint32_t last_generation;
    static int raw_hint_visible;
    uint32_t generation;
    int active;
    int direction;
    int y;
    int distance;

    pthread_mutex_lock(&edge_back_raw_state_lock);
    generation = edge_back_raw_hint_generation;
    active = edge_back_raw_hint_active;
    direction = edge_back_raw_hint_direction;
    y = edge_back_raw_hint_y;
    distance = edge_back_raw_hint_distance;
    pthread_mutex_unlock(&edge_back_raw_state_lock);

    if(generation == last_generation) {
        return;
    }
    last_generation = generation;

    if(active && edge_back_enabled && current_page != PAGE_HOME &&
       !page_transition_active) {
        edge_back_hint_update(direction, y, distance);
        raw_hint_visible = 1;
        return;
    }

    if(raw_hint_visible && !edge_back_tracking) {
        edge_back_hint_hide();
    }
    raw_hint_visible = 0;
}

static void edge_back_consume_raw_pending(void)
{
    int pending_page;
    int pending_direction;
    int pending_y;

    pthread_mutex_lock(&edge_back_raw_state_lock);
    if(!edge_back_raw_pending) {
        pthread_mutex_unlock(&edge_back_raw_state_lock);
        return;
    }

    pending_page = edge_back_raw_pending_page;
    pending_direction = edge_back_raw_pending_direction;
    pending_y = edge_back_raw_pending_y;
    edge_back_raw_pending = 0;
    pthread_mutex_unlock(&edge_back_raw_state_lock);
    if(!edge_back_enabled || current_page == PAGE_HOME || page_transition_active ||
       pending_page != (int)current_page) {
        edge_back_log("RAW_MAIN_SKIP enabled=%d current=%s pending_page=%d transition=%d",
                      edge_back_enabled, page_name(current_page), pending_page,
                      page_transition_active);
        return;
    }

    edge_back_log("RAW_MAIN_TRIGGER page=%s", page_name(current_page));
    edge_back_tracking = 0;
    edge_back_direction = 0;
    edge_back_lvgl_suppress_until_us = ui_monotonic_us() + 250000ULL;
    if(current_page == PAGE_CELLULAR && ui_cellular_handle_back()) {
        trace_ui_action("RAW_EDGE_BACK_INNER", PAGE_CELLULAR);
        edge_back_hint_update(pending_direction, pending_y, EDGE_BACK_TRIGGER_PX);
        edge_back_hint_hide();
        return;
    }
    if(camera_gallery_handle_back()) {
        trace_ui_action("RAW_EDGE_BACK_INNER", PAGE_GALLERY);
        edge_back_hint_update(pending_direction, pending_y, EDGE_BACK_TRIGGER_PX);
        edge_back_hint_hide();
        return;
    }
    trace_ui_action("RAW_EDGE_BACK", page_stack_len > 0 ?
                    page_stack[page_stack_len - 1] : PAGE_HOME);
    edge_back_hint_update(pending_direction, pending_y, EDGE_BACK_TRIGGER_PX);
    edge_back_hint_hide();
    nav_back();
}

static void entry_block_dialog_close_cb(lv_event_t *event)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(event);

    if(overlay && lv_obj_is_valid(overlay)) {
        lv_obj_delete(overlay);
    }
    entry_block_dialog = NULL;
}

static void entry_block_dialog_delete_cb(lv_event_t *event)
{
    (void)event;
    entry_block_dialog = NULL;
}

static void show_entry_block_dialog(const char *title, const char *message)
{
    int w = display_logical_width();
    int h = display_logical_height();
    int card_w = display_orientation_is_landscape() ? 520 : 456;
    int card_h = 242;
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *message_label;
    lv_obj_t *ok_btn;

    if(entry_block_dialog && lv_obj_is_valid(entry_block_dialog)) {
        lv_obj_delete(entry_block_dialog);
        entry_block_dialog = NULL;
    }

    if(card_w > w - 48) {
        card_w = w - 48;
    }
    if(card_w < 300) {
        card_w = w - 24;
    }
    if(card_h > h - 48) {
        card_h = h - 48;
    }

    entry_block_dialog = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(entry_block_dialog);
    lv_obj_set_style_bg_color(entry_block_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(entry_block_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(entry_block_dialog, 0, 0);
    lv_obj_set_style_pad_all(entry_block_dialog, 0, 0);
    lv_obj_clear_flag(entry_block_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(entry_block_dialog, entry_block_dialog_delete_cb,
                        LV_EVENT_DELETE, NULL);

    card = lv_obj_create(entry_block_dialog);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101720), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x314154), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title_label = ui_label(card, title ? title : "Hardware not detected",
                           &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(title_label, card_w - 48);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title_label, 24, 24);

    message_label = ui_label(card, message ? message : "Hardware not detected",
                             &lv_font_montserrat_18, 0xCBD5E1);
    lv_obj_set_width(message_label, card_w - 48);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(message_label, 24, 76);

    ok_btn = ui_command_button(card, (card_w - 176) / 2, card_h - 78,
                               176, "OK", 0x25C281);
    lv_obj_add_event_cb(ok_btn, entry_block_dialog_close_cb,
                        LV_EVENT_CLICKED, entry_block_dialog);
}

static void entry_probe_dialog_close(void)
{
    if(entry_probe_dialog && lv_obj_is_valid(entry_probe_dialog)) {
        lv_obj_delete(entry_probe_dialog);
    }
    entry_probe_dialog = NULL;
}

static void entry_probe_timer_cb(lv_timer_t *timer)
{
    page_id_t page;
    int done;
    int allowed;
    char title[128];
    char reason[192];

    (void)timer;

    pthread_mutex_lock(&entry_probe_lock);
    done = entry_probe_done;
    allowed = entry_probe_allowed;
    page = entry_probe_page;
    snprintf(title, sizeof(title), "%s", entry_probe_title);
    snprintf(reason, sizeof(reason), "%s", entry_probe_reason);
    if(done) {
        entry_probe_active = 0;
        entry_probe_done = 0;
    }
    pthread_mutex_unlock(&entry_probe_lock);

    if(!done) {
        return;
    }

    if(entry_probe_timer) {
        lv_timer_delete(entry_probe_timer);
        entry_probe_timer = NULL;
    }
    entry_probe_dialog_close();

    if(allowed) {
        touch_trace_log("NAV_ASYNC_PROBE_OK page=%s", page_name(page));
        nav_to(page);
    } else {
        touch_trace_log("NAV_ASYNC_PROBE_BLOCKED page=%s reason=%s",
                        page_name(page), reason);
        show_entry_block_dialog(title[0] ? title : "Hardware not detected",
                                reason[0] ? reason :
                                "Connect hardware and try again.");
    }
}

static void *entry_probe_worker(void *arg)
{
    page_id_t page = (page_id_t)(intptr_t)arg;
    char message[192] = "";
    char title[128] = "Hardware not detected";
    int allowed = 0;

    if(page == PAGE_NRF52840_DFU) {
        int rc = ui_nrf52840_dfu_preflight(message, sizeof(message));

        allowed = rc == 0;
        snprintf(title, sizeof(title), "%s",
                 rc == -2 ? "nRF52840 UART DFU unsupported" :
                 "nRF52840 not detected");
        if(!message[0]) {
            snprintf(message, sizeof(message), "%s",
                     rc == -2 ?
                     "Update the nRF52840 bootloader before using DFU." :
                     "Connect nRF52840 AT firmware and try again.");
        }
    }

    pthread_mutex_lock(&entry_probe_lock);
    entry_probe_allowed = allowed;
    snprintf(entry_probe_title, sizeof(entry_probe_title), "%s", title);
    snprintf(entry_probe_reason, sizeof(entry_probe_reason), "%s", message);
    entry_probe_done = 1;
    pthread_mutex_unlock(&entry_probe_lock);

    app_request_fast_refresh();
    return NULL;
}

static int start_entry_probe(page_id_t page)
{
    lv_obj_t *card;
    lv_obj_t *spinner;
    lv_obj_t *title;
    lv_obj_t *hint;
    pthread_t thread;
    int w = display_logical_width();
    int h = display_logical_height();
    int card_w = display_orientation_is_landscape() ? 420 : 360;
    int card_h = 210;

    if(page != PAGE_NRF52840_DFU) {
        return 0;
    }

    pthread_mutex_lock(&entry_probe_lock);
    if(entry_probe_active) {
        pthread_mutex_unlock(&entry_probe_lock);
        return 1;
    }
    entry_probe_active = 1;
    entry_probe_done = 0;
    entry_probe_allowed = 0;
    entry_probe_page = page;
    entry_probe_title[0] = '\0';
    entry_probe_reason[0] = '\0';
    pthread_mutex_unlock(&entry_probe_lock);

    if(card_w > w - 48) {
        card_w = w - 48;
    }
    if(card_w < 300) {
        card_w = w - 24;
    }

    entry_probe_dialog_close();
    entry_probe_dialog = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(entry_probe_dialog);
    lv_obj_set_style_bg_color(entry_probe_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(entry_probe_dialog, LV_OPA_70, 0);
    lv_obj_set_style_border_width(entry_probe_dialog, 0, 0);
    lv_obj_set_style_pad_all(entry_probe_dialog, 0, 0);
    lv_obj_clear_flag(entry_probe_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(entry_probe_dialog, LV_OBJ_FLAG_CLICKABLE);

    card = lv_obj_create(entry_probe_dialog);
    lv_obj_set_size(card, card_w, card_h > h - 48 ? h - 48 : card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101720), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x314154), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    spinner = lv_spinner_create(card);
    lv_obj_set_size(spinner, 64, 64);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x263342),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x3DA5FF),
                               LV_PART_INDICATOR);

    title = label(card, "Checking nRF52840...", &lv_font_montserrat_20,
                  0xF2F5F8);
    lv_obj_set_width(title, card_w - 48);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 104);

    hint = label(card, "Connect nRF52840 AT firmware and try again.",
                 &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, card_w - 48);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 144);

    if(entry_probe_timer) {
        lv_timer_delete(entry_probe_timer);
    }
    entry_probe_timer = lv_timer_create(entry_probe_timer_cb, 80, NULL);

    if(pthread_create(&thread, NULL, entry_probe_worker,
                      (void *)(intptr_t)page) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&entry_probe_lock);
        entry_probe_allowed = 0;
        snprintf(entry_probe_title, sizeof(entry_probe_title), "%s",
                 "Hardware not detected");
        snprintf(entry_probe_reason, sizeof(entry_probe_reason), "%s",
                 "Thread failed");
        entry_probe_done = 1;
        pthread_mutex_unlock(&entry_probe_lock);
    }

    touch_trace_log("NAV_ASYNC_PROBE_START page=%s", page_name(page));
    app_request_fast_refresh();
    return 1;
}

static int page_entry_allowed(page_id_t page)
{
    const char *title = "Hardware not detected";
    const char *reason = NULL;

    switch(page) {
    case PAGE_NRF52840_DFU:
        return 1;
    case PAGE_CELLULAR:
        if(!ui_hardware_keyboard_base_detected()) {
            reason = "This feature requires nRF9151 extension board.";
        }
        break;
    case PAGE_KEYBOARD_TEST:
        if(!ui_hardware_tca8418_detected()) {
            reason = "This feature requires TCA8418 keyboard controller.";
        }
        break;
    case PAGE_XL9555_TEST:
        if(!ui_hardware_xl9555_detected()) {
            reason = "This feature requires XL9555 GPIO expander.";
        }
        break;
    case PAGE_BATTERY:
        if(!ui_hardware_bq27220_detected()) {
            reason = "This feature requires BQ27220 battery gauge.";
        }
        break;
    case PAGE_BQ25896:
        if(!ui_hardware_bq25896_detected()) {
            reason = "This feature requires BQ25896 charger.";
        }
        break;
    default:
        return 1;
    }

    if(!reason) {
        return 1;
    }

    touch_trace_log("NAV_BLOCKED page=%s reason=%s", page_name(page), reason);
    show_entry_block_dialog(title, reason);
    return 0;
}

static void app_event_cb(lv_event_t *event)
{
    page_id_t page = (page_id_t)(intptr_t)lv_event_get_user_data(event);

    if(page == PAGE_SCREENSHOT) {
        trace_ui_action("LVGL_CLICKED_SCREENSHOT", current_page);
        app_take_screenshot();
        return;
    }

    trace_ui_action("LVGL_CLICKED_NAV", page);
    if(start_entry_probe(page)) {
        return;
    }
    if(!page_entry_allowed(page)) {
        return;
    }
    nav_to(page);
}

static void back_event_cb(lv_event_t *event)
{
    (void)event;
    if(current_page == PAGE_CELLULAR && ui_cellular_handle_back()) {
        trace_ui_action("LVGL_CLICKED_BACK_INNER", PAGE_CELLULAR);
        return;
    }
    if(camera_gallery_handle_back()) {
        trace_ui_action("LVGL_CLICKED_BACK_INNER", PAGE_GALLERY);
        return;
    }
    trace_ui_action("LVGL_CLICKED_BACK", page_stack_len > 0 ?
                    page_stack[page_stack_len - 1] : PAGE_HOME);
    nav_back();
}

void app_nav_to_page(page_id_t page)
{
    trace_ui_action("LVGL_CLICKED_NAV", page);
    if(start_entry_probe(page)) {
        return;
    }
    if(!page_entry_allowed(page)) {
        return;
    }
    nav_to(page);
}

void app_nav_to_settings_page(page_id_t page)
{
    settings_subpage_context = 1;
    touch_trace_log("SETTINGS_NAV page=%s current=%s stack_len=%d",
                    page_name(page), page_name(current_page), page_stack_len);
    trace_ui_action("LVGL_CLICKED_SETTINGS_NAV", page);
    if(start_entry_probe(page)) {
        settings_subpage_context = 0;
        return;
    }
    if(!page_entry_allowed(page)) {
        settings_subpage_context = 0;
        return;
    }
    nav_to(page);
}

void app_nav_back(void)
{
    if(current_page == PAGE_CELLULAR && ui_cellular_handle_back()) {
        trace_ui_action("APP_BACK_INNER", PAGE_CELLULAR);
        return;
    }
    if(camera_gallery_handle_back()) {
        trace_ui_action("APP_BACK_INNER", PAGE_GALLERY);
        return;
    }
    trace_ui_action("LVGL_CLICKED_BACK", page_stack_len > 0 ?
                    page_stack[page_stack_len - 1] : PAGE_HOME);
    nav_back();
}

static void make_click_forwarder(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &style_chip, 0);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, 30);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *txt = label(obj, text, &lv_font_montserrat_16, color);
    lv_obj_center(txt);
    return obj;
}

static lv_obj_t *status_icon_item(lv_obj_t *parent, const char *symbol,
                                  page_id_t page, uint32_t color,
                                  lv_obj_t **label_out)
{
    lv_obj_t *item = lv_obj_create(parent);

    lv_obj_set_size(item, STATUS_BAR_ITEM_W, STATUS_BAR_ITEM_H);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, app_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)page);

    lv_obj_t *icon = label(item, symbol, &lv_font_montserrat_18, color);
    lv_obj_center(icon);
    make_click_forwarder(icon);
    if(label_out) {
        *label_out = icon;
    }

    return item;
}

static void status_audio_set_obj_visible(lv_obj_t *obj, int visible)
{
    if(!obj || !lv_obj_is_valid(obj)) {
        return;
    }
    if(visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_audio_set_image_color(lv_obj_t *obj, uint32_t color)
{
    if(!obj || !lv_obj_is_valid(obj)) {
        return;
    }
    lv_obj_set_style_image_recolor(obj, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
}

static lv_obj_t *status_audio_png_image(lv_obj_t *parent, const char *path)
{
    lv_obj_t *img = lv_image_create(parent);

    lv_image_set_src(img, path);
    lv_image_set_scale(img, (STATUS_BAR_AUDIO_ICON_PX * 256) / 96);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(img);
    return img;
}

static lv_obj_t *status_audio_item(lv_obj_t *parent)
{
    lv_obj_t *item = lv_obj_create(parent);

    lv_obj_set_size(item, STATUS_BAR_AUDIO_W, STATUS_BAR_ITEM_H);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, app_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PAGE_AUDIO_SETTINGS);

    status_audio_earphone_img =
        status_audio_png_image(item, STATUS_AUDIO_EARPHONE_ICON);
    status_audio_speaker_img =
        status_audio_png_image(item, STATUS_AUDIO_SPEAKER_ICON);
    status_audio_set_obj_visible(status_audio_speaker_img, 0);

    return item;
}

static lv_obj_t *status_lte_item(lv_obj_t *parent)
{
    const int bar_w = 4;
    const int bar_gap = 3;
    const int x0 = 6;
    const int y_base = 23;
    const int heights[4] = {7, 11, 15, 19};
    lv_obj_t *item = lv_obj_create(parent);

    lv_obj_set_size(item, STATUS_BAR_LTE_W, STATUS_BAR_ITEM_H);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, app_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PAGE_CELLULAR);

    for(int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(item);
        status_lte_bars[i] = bar;
        lv_obj_set_size(bar, bar_w, heights[i]);
        lv_obj_set_pos(bar, x0 + i * (bar_w + bar_gap),
                       y_base - heights[i]);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x36404A), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        make_click_forwarder(bar);
    }
    status_lte_x_label = label(item, "x", &lv_font_montserrat_14, 0x8B949E);
    lv_obj_align(status_lte_x_label, LV_ALIGN_RIGHT_MID, -1, 1);
    lv_obj_add_flag(status_lte_x_label, LV_OBJ_FLAG_HIDDEN);
    make_click_forwarder(status_lte_x_label);

    return item;
}

static lv_obj_t *status_battery_item(lv_obj_t *parent)
{
    lv_obj_t *item = lv_obj_create(parent);

    lv_obj_set_size(item, STATUS_BAR_BATTERY_W, STATUS_BAR_ITEM_H);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x253040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(item, app_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)PAGE_BATTERY);

    status_battery_shell_obj = lv_obj_create(item);
    lv_obj_set_pos(status_battery_shell_obj, 4, 9);
    lv_obj_set_size(status_battery_shell_obj, 24, 13);
    lv_obj_set_style_bg_opa(status_battery_shell_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_battery_shell_obj, 2, 0);
    lv_obj_set_style_border_color(status_battery_shell_obj,
                                  lv_color_hex(0x8B949E), 0);
    lv_obj_set_style_radius(status_battery_shell_obj, 3, 0);
    lv_obj_set_style_pad_all(status_battery_shell_obj, 0, 0);
    lv_obj_clear_flag(status_battery_shell_obj, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(status_battery_shell_obj);

    status_battery_fill_obj = lv_obj_create(status_battery_shell_obj);
    lv_obj_set_pos(status_battery_fill_obj, 2, 2);
    lv_obj_set_size(status_battery_fill_obj, 0, 5);
    lv_obj_set_style_bg_color(status_battery_fill_obj, lv_color_hex(0x8B949E),
                              0);
    lv_obj_set_style_bg_opa(status_battery_fill_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_battery_fill_obj, 0, 0);
    lv_obj_set_style_radius(status_battery_fill_obj, 1, 0);
    lv_obj_set_style_pad_all(status_battery_fill_obj, 0, 0);
    lv_obj_clear_flag(status_battery_fill_obj, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(status_battery_fill_obj);

    status_battery_tip_obj = lv_obj_create(item);
    lv_obj_set_pos(status_battery_tip_obj, 29, 12);
    lv_obj_set_size(status_battery_tip_obj, 3, 7);
    lv_obj_set_style_bg_color(status_battery_tip_obj, lv_color_hex(0x8B949E),
                              0);
    lv_obj_set_style_bg_opa(status_battery_tip_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_battery_tip_obj, 0, 0);
    lv_obj_set_style_radius(status_battery_tip_obj, 1, 0);
    lv_obj_set_style_pad_all(status_battery_tip_obj, 0, 0);
    lv_obj_clear_flag(status_battery_tip_obj, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(status_battery_tip_obj);

    status_battery_percent_label = label(item, "--", &lv_font_montserrat_12,
                                         0x8B949E);
    lv_obj_set_pos(status_battery_percent_label, 37, 7);
    lv_obj_set_width(status_battery_percent_label, STATUS_BAR_BATTERY_W - 39);
    lv_label_set_long_mode(status_battery_percent_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_battery_percent_label,
                                LV_TEXT_ALIGN_RIGHT, 0);
    make_click_forwarder(status_battery_percent_label);
    return item;
}

static uint32_t status_gps_color(int modem_present)
{
    k230_nrf9151_status_t status;

    if(!k230_nrf9151_location_autostart_enabled()) {
        return 0x8B949E;
    }
    if(!modem_present) {
        return 0x8B949E;
    }
    if(k230_nrf9151_read_status(&status, 300) != 0 || !status.present) {
        return 0x8B949E;
    }
    if(status.gnss_has_fix) {
        return 0x25C281;
    }
    if(status.gnss_running) {
        return 0x3DA5FF;
    }
    return 0xF5A524;
}

static void status_set_label_color(lv_obj_t *obj, uint32_t color)
{
    if(obj && lv_obj_is_valid(obj)) {
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    }
}

static uint32_t status_ble_color(void)
{
    nrf52840_status_t nrf_status;

    if(ui_nrf52840_get_status(&nrf_status) == 0 && nrf_status.known) {
        if(nrf_status.mode == NRF52840_MODE_OFF) {
            return 0x8B949E;
        }
        if(nrf_status.phoneapi_connected) {
            return 0x25C281;
        }
        if(nrf_status.at_ok) {
            return 0x3DA5FF;
        }
        return 0x8B949E;
    }
    if(strcmp(status_ble_state, "connected") == 0) {
        return 0x25C281;
    }
    if(strcmp(status_ble_state, "ready") == 0) {
        return 0x3DA5FF;
    }
    return 0x8B949E;
}

static uint32_t status_wifi_color(int wifi_present, int wifi_has_ip)
{
    if(strcmp(status_wifi_state, "off") == 0) {
        return 0x8B949E;
    }
    if(wifi_has_ip || strcmp(status_wifi_state, "connected") == 0) {
        return 0x25C281;
    }
    if(strcmp(status_wifi_state, "scanning") == 0 ||
       strcmp(status_wifi_state, "connecting") == 0) {
        return 0x3DA5FF;
    }
    return wifi_present ? 0xF5A524 : 0x8B949E;
}

static void status_update_audio_route(void)
{
    char output[24];
    int external;
    uint32_t color;

    if(!status_audio_item_obj || !lv_obj_is_valid(status_audio_item_obj)) {
        return;
    }

    output[0] = '\0';
    ui_prefs_get(PREF_AUDIO_OUTPUT, output, sizeof(output), "");
    if(output[0]) {
        external = strcmp(output, AUDIO_OUTPUT_EXTERNAL) == 0;
    } else {
        external = ui_audio_output_is_external();
    }

    color = external ? 0x25C281 : 0x3DA5FF;

    status_audio_set_obj_visible(status_audio_earphone_img, !external);
    status_audio_set_obj_visible(status_audio_speaker_img, external);
    status_audio_set_image_color(status_audio_earphone_img, color);
    status_audio_set_image_color(status_audio_speaker_img, color);
}

static void *status_battery_probe_thread(void *arg)
{
    int soc = -1;
    int available = 0;
    int charging = 0;
    int done = 0;
    int current_ma = 0;

    (void)arg;
    if(ui_extension_keyboard_base_available() &&
       ui_bq27220_get_soc_pct(&soc) == 0) {
        available = 1;
    }
    if(ui_extension_keyboard_base_available()) {
        (void)ui_bq25896_get_charge_state(&charging, &done);
        if(!charging && available &&
           ui_bq27220_get_current_ma(&current_ma) == 0 &&
           current_ma > 20) {
            charging = 1;
        }
    }

    pthread_mutex_lock(&status_hw_lock);
    status_battery_cache_valid = 1;
    status_battery_available_cache = available;
    status_battery_soc_cache = available ? soc : -1;
    status_battery_charging_cache = charging;
    status_battery_charge_done_cache = done;
    status_battery_refresh_busy = 0;
    status_battery_next_refresh_us =
        monotonic_us() + STATUS_BATTERY_REFRESH_US;
    pthread_mutex_unlock(&status_hw_lock);

    return NULL;
}

static void status_schedule_battery_probe(void)
{
    pthread_t thread;
    uint64_t now = monotonic_us();
    int start = 0;

    pthread_mutex_lock(&status_hw_lock);
    if(!status_battery_refresh_busy &&
       (!status_battery_cache_valid ||
        now >= status_battery_next_refresh_us)) {
        status_battery_refresh_busy = 1;
        start = 1;
    }
    pthread_mutex_unlock(&status_hw_lock);

    if(!start) {
        return;
    }
    if(pthread_create(&thread, NULL, status_battery_probe_thread, NULL) == 0) {
        pthread_detach(thread);
        return;
    }

    pthread_mutex_lock(&status_hw_lock);
    status_battery_refresh_busy = 0;
    status_battery_next_refresh_us =
        monotonic_us() + STATUS_BATTERY_REFRESH_US;
    pthread_mutex_unlock(&status_hw_lock);
}

static void status_draw_battery_level(int available, int soc,
                                      int charging, int charge_done)
{
    uint32_t color = available ?
                     (soc <= 15 ? 0xEF4D5A :
                      (soc <= 30 ? 0xF5A524 : 0x25C281)) :
                     0x8B949E;
    int fill_w = 0;
    char percent[8];

    if(charging || charge_done) {
        color = charge_done ? 0x3DA5FF : 0x25C281;
    }

    if(available) {
        if(soc < 0) {
            soc = 0;
        } else if(soc > 100) {
            soc = 100;
        }
        fill_w = (18 * soc) / 100;
        if(soc > 0 && fill_w < 2) {
            fill_w = 2;
        }
        snprintf(percent, sizeof(percent), charging ? "+%d" : "%d%%", soc);
    } else {
        fill_w = charging ? 8 : 0;
        snprintf(percent, sizeof(percent), charging ? "+" : "--");
    }

    if(status_battery_shell_obj && lv_obj_is_valid(status_battery_shell_obj)) {
        lv_obj_set_style_border_color(status_battery_shell_obj,
                                      lv_color_hex(color), 0);
    }
    if(status_battery_fill_obj && lv_obj_is_valid(status_battery_fill_obj)) {
        lv_obj_set_size(status_battery_fill_obj, fill_w, 5);
        lv_obj_set_style_bg_color(status_battery_fill_obj,
                                  lv_color_hex(color), 0);
    }
    if(status_battery_tip_obj && lv_obj_is_valid(status_battery_tip_obj)) {
        lv_obj_set_style_bg_color(status_battery_tip_obj,
                                  lv_color_hex(color), 0);
    }
    status_set_label_color(status_battery_percent_label, color);
    if(status_battery_percent_label &&
       lv_obj_is_valid(status_battery_percent_label)) {
        lv_label_set_text(status_battery_percent_label, percent);
    }
}

static void status_update_battery(void)
{
    int cache_valid;
    int available;
    int soc;
    int charging;
    int charge_done;

    status_schedule_battery_probe();

    pthread_mutex_lock(&status_hw_lock);
    cache_valid = status_battery_cache_valid;
    available = status_battery_available_cache;
    soc = status_battery_soc_cache;
    charging = status_battery_charging_cache;
    charge_done = status_battery_charge_done_cache;
    pthread_mutex_unlock(&status_hw_lock);

    status_draw_battery_level(cache_valid && available, soc,
                              cache_valid && charging,
                              cache_valid && charge_done);
}

static void status_set_lte_bars(int level, uint32_t color)
{
    if(level < 0) {
        level = 0;
    }
    if(level > 4) {
        level = 4;
    }

    for(int i = 0; i < 4; i++) {
        uint32_t bar_color = i < level ? color : 0x36404A;

        if(status_lte_bars[i] && lv_obj_is_valid(status_lte_bars[i])) {
            lv_obj_set_style_bg_color(status_lte_bars[i],
                                      lv_color_hex(bar_color), 0);
        }
    }
    if(status_lte_x_label && lv_obj_is_valid(status_lte_x_label)) {
        if(level <= 0) {
            lv_obj_clear_flag(status_lte_x_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(status_lte_x_label,
                                        lv_color_hex(color), 0);
        } else {
            lv_obj_add_flag(status_lte_x_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static int status_group_width_current(void)
{
    int items = 4;
    int width = STATUS_BAR_ITEM_W + STATUS_BAR_LTE_W +
                STATUS_BAR_ITEM_W + STATUS_BAR_BATTERY_W;

    if(status_wifi_visible) {
        width += STATUS_BAR_ITEM_W;
        items++;
    }
    return width + STATUS_BAR_ITEM_GAP * (items - 1);
}

static void status_set_wifi_visible(int visible)
{
    visible = visible ? 1 : 0;
    if(status_wifi_visible == visible) {
        return;
    }

    status_wifi_visible = visible;
    if(status_wifi_item_obj && lv_obj_is_valid(status_wifi_item_obj)) {
        if(visible) {
            lv_obj_clear_flag(status_wifi_item_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(status_wifi_item_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    status_bar_relayout();
}

static void status_bar_update(lv_timer_t *timer)
{
    char ip[64];
    int wifi_has_ip;
    int wifi_present;
    int modem_present;
    int lte_level;

    (void)timer;

    if(!status_bar_obj || !lv_obj_is_valid(status_bar_obj)) {
        return;
    }

    ui_network_sync_default_route("status-bar");

    wifi_present = path_exists("/sys/class/net/" NET_WIFI_IFACE);
    wifi_has_ip = read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0;
    status_set_wifi_visible(wifi_has_ip);
    status_set_label_color(status_wifi_label,
                           status_wifi_color(wifi_present, wifi_has_ip));

    modem_present = k230_nrf9151_uart_present();
    lte_level = modem_present ? ui_cellular_lte_signal_level() : 0;
    status_set_lte_bars(lte_level,
                        lte_level > 0 ? 0x25C281 :
                        (modem_present ? 0xEF4D5A : 0x8B949E));

    status_set_label_color(status_location_label,
                           status_gps_color(modem_present));
    status_set_label_color(status_ble_label, status_ble_color());
    status_update_audio_route();
    status_update_battery();
}

static void status_bar_relayout(void)
{
    const int status_group_w = status_group_width_current();

    if(!status_bar_obj || !lv_obj_is_valid(status_bar_obj)) {
        return;
    }
    lv_obj_set_size(status_bar_obj, display_logical_width(), STATUS_BAR_H);
    if(time_label && lv_obj_is_valid(time_label)) {
        lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);
    }
    if(status_audio_item_obj && lv_obj_is_valid(status_audio_item_obj)) {
        lv_obj_align(status_audio_item_obj, LV_ALIGN_LEFT_MID,
                     STATUS_BAR_SAFE_SIDE, 0);
    }
    if(status_group_obj && lv_obj_is_valid(status_group_obj)) {
        lv_obj_set_size(status_group_obj, status_group_w, STATUS_BAR_ITEM_H);
        lv_obj_align(status_group_obj, LV_ALIGN_RIGHT_MID,
                     -STATUS_BAR_SAFE_SIDE, 0);
    }
}

static void create_status_bar(lv_obj_t *scr)
{
    const int status_group_w = status_group_width_current();
    lv_obj_t *bar = lv_obj_create(scr);
    status_bar_obj = bar;
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, display_logical_width(), STATUS_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B0D10), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    time_label = label(bar, "--:--", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);

    status_audio_item_obj = status_audio_item(bar);
    lv_obj_align(status_audio_item_obj, LV_ALIGN_LEFT_MID,
                 STATUS_BAR_SAFE_SIDE, 0);

    status_group_obj = lv_obj_create(bar);
    lv_obj_set_size(status_group_obj, status_group_w, STATUS_BAR_ITEM_H);
    lv_obj_set_style_bg_opa(status_group_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_group_obj, 0, 0);
    lv_obj_set_style_pad_all(status_group_obj, 0, 0);
    lv_obj_set_style_pad_column(status_group_obj, STATUS_BAR_ITEM_GAP, 0);
    lv_obj_set_flex_flow(status_group_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_group_obj, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_group_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_group_obj, LV_ALIGN_RIGHT_MID,
                 -STATUS_BAR_SAFE_SIDE, 0);

    status_icon_item(status_group_obj, LV_SYMBOL_GPS, PAGE_CELLULAR,
                     0x8B949E, &status_location_label);
    status_lte_item(status_group_obj);
    status_wifi_item_obj = status_icon_item(status_group_obj, LV_SYMBOL_WIFI,
                                            PAGE_WIFI, 0x8B949E,
                                            &status_wifi_label);
    status_icon_item(status_group_obj, LV_SYMBOL_BLUETOOTH, PAGE_BLE,
                     0x8B949E, &status_ble_label);
    status_battery_item(status_group_obj);

    status_bar_update(NULL);
    status_bar_relayout();
}

void app_set_wifi_status(const char *state)
{
    const char *value = state && state[0] ? state : "on";

    snprintf(status_wifi_state, sizeof(status_wifi_state), "%s", value);
    status_bar_update(NULL);
}

void app_set_ble_status(const char *state)
{
    const char *value = state && state[0] ? state : "offline";

    snprintf(status_ble_state, sizeof(status_ble_state), "%s", value);
    status_set_label_color(status_ble_label, status_ble_color());
}

void app_refresh_status_bar(void)
{
    status_bar_update(NULL);
}

static lv_obj_t *icon_tile(lv_obj_t *parent, const app_item_t *item, int x, int y,
                           int w, int h)
{
    lv_obj_t *tile = lv_obj_create(parent);
    int wide_tile = h >= 116;
    int icon_size = wide_tile ? 68 : 58;
    int icon_y = wide_tile ? 8 : 4;
    int name_y = icon_y + icon_size + (wide_tile ? 10 : 8);
    int name_h = h - name_y - 8;

    if(name_h < 24) {
        name_h = 24;
    }

    lv_obj_add_style(tile, &style_button, 0);
    lv_obj_add_style(tile, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tile, 6);
    lv_obj_add_event_cb(tile, app_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item->page);

    lv_obj_t *icon_box = lv_obj_create(tile);
    lv_obj_add_style(icon_box, &style_icon, 0);
    ui_style_icon_box_for_page(icon_box, item->page, item->color);
    lv_obj_set_size(icon_box, icon_size, icon_size);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, icon_y);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(icon_box);

    ui_create_page_icon(icon_box, item->page, item->symbol,
                        wide_tile ? &lv_font_montserrat_28 :
                        &lv_font_montserrat_24, 0xFFFFFF, icon_size);

    lv_obj_t *name = label(tile, item->title,
                           wide_tile ? &lv_font_montserrat_18 :
                           &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_size(name, w - 8, name_h);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, name_y);
    make_click_forwarder(name);

    return tile;
}

static void set_launcher_chrome(page_id_t page)
{
    status_bar_relayout();
    status_bar_update(NULL);

    if(status_bar_obj) {
        if(page == PAGE_DISPLAY_TEST) {
            lv_obj_add_flag(status_bar_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(status_bar_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static int app_item_requires_keyboard_base(page_id_t page)
{
#if !K230_HIDE_KEYBOARD_BASE_APPS_UNTIL_DETECTED
    (void)page;
    return 0;
#else
    switch(page) {
    case PAGE_BQ25896:
    case PAGE_BATTERY:
    case PAGE_KEYBOARD_TEST:
    case PAGE_XL9555_TEST:
    case PAGE_CELLULAR:
        return 1;
    default:
        return 0;
    }
#endif
}

static int app_item_visible(const app_item_t *item)
{
    if(!item) {
        return 0;
    }
    if(app_item_requires_keyboard_base(item->page) &&
       !ui_extension_keyboard_base_available()) {
        return 0;
    }
    return 1;
}

static void create_home_app_grid(lv_obj_t *parent, int cols, int tile_w,
                                 int tile_h, int gap_x, int gap_y)
{
    int visible_index = 0;

    if(cols < 1) {
        cols = 1;
    }

    for(size_t i = 0; i < sizeof(app_items) / sizeof(app_items[0]); i++) {
        int col;
        int row;

        if(!app_item_visible(&app_items[i])) {
            continue;
        }
        col = visible_index % cols;
        row = visible_index / cols;

        icon_tile(parent, &app_items[i], col * (tile_w + gap_x),
                  row * (tile_h + gap_y), tile_w, tile_h);
        visible_index++;
    }
}

static lv_obj_t *home_status_row(lv_obj_t *parent, int y, int w,
                                 const char *symbol, const char *title,
                                 const char *value, uint32_t value_color,
                                 lv_obj_t **title_label_out,
                                 lv_obj_t **value_label_out,
                                 lv_obj_t **badge_out,
                                 lv_obj_t **badge_text_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    int row_x = 18;
    int row_w = w - row_x;
    int badge_size = 38;
    if(row_w < 240) {
        row_x = 12;
        row_w = w - row_x;
    }
    lv_obj_add_style(row, &style_button, 0);
    lv_obj_set_pos(row, row_x, y);
    lv_obj_set_size(row, row_w, 72);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x121923), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x263442), 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, badge_size, badge_size);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(value_color), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge_text = label(badge, symbol, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_center(badge_text);

    lv_obj_t *name = label(row, title, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(name, row_w - 64);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 52, 8);

    lv_obj_t *state = label(row, value, &lv_font_montserrat_18, value_color);
    lv_obj_set_width(state, row_w - 64);
    lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 52, 36);
    if(title_label_out) {
        *title_label_out = name;
    }
    if(value_label_out) {
        *value_label_out = state;
    }
    if(badge_out) {
        *badge_out = badge;
    }
    if(badge_text_out) {
        *badge_text_out = badge_text;
    }

    return row;
}

static void create_home(lv_obj_t *scr)
{
    char temp_text[64];
    char power_source_text[64];
    char power_draw_text[64];
    char eth_title[32];
    char eth_symbol[8];
    char eth_text[64];
    uint32_t temp_color;
    uint32_t source_color;
    uint32_t draw_color;
    uint32_t eth_color;
    int logical_w = display_logical_width();
    int logical_h = display_logical_height();
    int landscape = display_orientation_is_landscape();
    int margin = landscape ? 24 : 24;
    int status_h = 54;
    int top = status_h + 16;

    home_temp_value_label = NULL;
    home_power_source_value_label = NULL;
    home_power_draw_value_label = NULL;
    home_eth_title_label = NULL;
    home_eth_value_label = NULL;
    home_temp_badge = NULL;
    home_power_source_badge = NULL;
    home_power_draw_badge = NULL;
    home_eth_badge = NULL;
    home_eth_badge_text_label = NULL;
    pthread_mutex_lock(&home_telemetry_lock);
    snprintf(temp_text, sizeof(temp_text), "%s", home_temp_cache);
    snprintf(power_source_text, sizeof(power_source_text), "%s",
             home_power_source_cache);
    snprintf(power_draw_text, sizeof(power_draw_text), "%s",
             home_power_draw_cache);
    snprintf(eth_title, sizeof(eth_title), "%s", home_eth_title_cache);
    snprintf(eth_symbol, sizeof(eth_symbol), "%s", home_eth_symbol_cache);
    snprintf(eth_text, sizeof(eth_text), "%s", home_eth_cache);
    temp_color = home_temp_cache_color;
    source_color = home_power_source_cache_color;
    draw_color = home_power_draw_cache_color;
    eth_color = home_eth_cache_color;
    pthread_mutex_unlock(&home_telemetry_lock);

    if(landscape) {
        int gap = 18;
        int left_w = logical_w / 4;
        int body_h = logical_h - top - margin;
        int grid_x;
        int grid_w;
        int grid_inner_w;
        int tile_w;
        int tile_h = 118;
        int gap_x = 14;
        int gap_y = 12;
        int app_pad = 10;
        int cols;
        int card_w;

        if(left_w < 300) {
            left_w = 300;
        }
        if(left_w > 344) {
            left_w = 344;
        }
        if(body_h < 360) {
            body_h = logical_h - top;
        }

        grid_x = margin + left_w + gap;
        grid_w = logical_w - grid_x - margin;
        if(grid_w < 520) {
            grid_w = logical_w - margin * 2;
            grid_x = margin;
            left_w = 0;
        }

        grid_inner_w = grid_w - app_pad * 2;
        if(grid_inner_w < 480) {
            grid_inner_w = grid_w;
            app_pad = 0;
        }

        cols = grid_inner_w >= 740 ? 5 : 4;
        tile_w = (grid_inner_w - (cols - 1) * gap_x) / cols;
        if(tile_w < 96) {
            tile_w = 96;
        }

        touch_trace_log("HOME_LAYOUT landscape logical=%dx%d left=%d grid=%d,%d "
                        "inner=%d cols=%d tile=%dx%d",
                        logical_w, logical_h, left_w, grid_x, grid_w,
                        grid_inner_w, cols, tile_w, tile_h);

        if(left_w > 0) {
            lv_obj_t *dash = panel(scr, margin, top, left_w, body_h);
            lv_obj_set_style_bg_color(dash, lv_color_hex(0x0E151D), 0);
            lv_obj_set_style_border_color(dash, lv_color_hex(0x24313D), 0);
            lv_obj_set_style_shadow_width(dash, 8, 0);
            lv_obj_set_style_shadow_opa(dash, LV_OPA_20, 0);
            lv_obj_set_style_shadow_color(dash, lv_color_hex(0x25C281), 0);
            lv_obj_set_style_pad_all(dash, 14, 0);

            lv_obj_t *accent = lv_obj_create(dash);
            lv_obj_set_pos(accent, 0, 0);
            lv_obj_set_size(accent, 6, body_h - 28);
            lv_obj_set_style_bg_color(accent, lv_color_hex(0x25C281), 0);
            lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(accent, 0, 0);
            lv_obj_set_style_radius(accent, 3, 0);
            lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

            home_time_label = NULL;

            lv_obj_t *name = label(dash, "T-Display-K230",
                                   &lv_font_montserrat_22,
                                   0xF2F5F8);
            lv_obj_set_width(name, left_w - 52);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_align(name, LV_ALIGN_TOP_LEFT, 20, 10);

            date_label = label(dash, "--", &lv_font_montserrat_16, 0x9AA4AF);
            lv_obj_set_width(date_label, left_w - 52);
            lv_label_set_long_mode(date_label, LV_LABEL_LONG_DOT);
            lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, 20, 44);

            card_w = left_w - 34;
            home_status_row(dash, 88, card_w, eth_symbol, eth_title,
                            eth_text, eth_color, &home_eth_title_label,
                            &home_eth_value_label, &home_eth_badge,
                            &home_eth_badge_text_label);
            home_status_row(dash, 166, card_w, "TMP", "K230 Thermal",
                            temp_text, temp_color, NULL,
                            &home_temp_value_label, &home_temp_badge, NULL);
            home_status_row(dash, 244, card_w, "PWR", "Power Source",
                            power_source_text, source_color, NULL,
                            &home_power_source_value_label,
                            &home_power_source_badge, NULL);
            home_status_row(dash, 322, card_w, "ENE", "Energy",
                            power_draw_text, draw_color, NULL,
                            &home_power_draw_value_label,
                            &home_power_draw_badge, NULL);
        } else {
            home_time_label = NULL;
            date_label = NULL;
        }

        lv_obj_t *apps = scroll_region(scr, grid_x, top, grid_w, body_h);
        lv_obj_set_style_bg_color(apps, lv_color_hex(0x0B1117), 0);
        lv_obj_set_style_bg_opa(apps, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(apps, 8, 0);
        lv_obj_set_style_pad_all(apps, app_pad, 0);
        lv_obj_set_style_pad_bottom(apps, 28, 0);
        create_home_app_grid(apps, cols, tile_w, tile_h, gap_x, gap_y);
        home_apply_scroll_restore(apps);
        update_time_labels(NULL);
        home_telemetry_timer = lv_timer_create(home_telemetry_timer_cb,
                                               3000, NULL);
        return;
    }

    lv_obj_t *hero = panel(scr, 24, 70, 520, 150);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x142033), 0);

    home_time_label = NULL;

    lv_obj_t *name = label(hero, "T-Display-K230", &lv_font_montserrat_26,
                           0xF2F5F8);
    lv_obj_set_width(name, 272);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 6, 18);

    date_label = label(hero, "--", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(date_label, 272);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_DOT);
    lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, 8, 58);

    home_eth_badge = lv_obj_create(hero);
    lv_obj_set_size(home_eth_badge, 54, 54);
    lv_obj_set_style_radius(home_eth_badge, 8, 0);
    lv_obj_set_style_bg_color(home_eth_badge, lv_color_hex(eth_color), 0);
    lv_obj_set_style_bg_opa(home_eth_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(home_eth_badge, 0, 0);
    lv_obj_clear_flag(home_eth_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(home_eth_badge, LV_ALIGN_RIGHT_MID, -168, 0);
    lv_obj_t *eth_icon = label(home_eth_badge, eth_symbol,
                               &lv_font_montserrat_14, 0xFFFFFF);
    home_eth_badge_text_label = eth_icon;
    lv_obj_center(eth_icon);

    home_eth_value_label = label(hero, eth_text, &lv_font_montserrat_22,
                                 eth_color);
    lv_obj_set_width(home_eth_value_label, 142);
    lv_label_set_long_mode(home_eth_value_label, LV_LABEL_LONG_DOT);
    lv_obj_align(home_eth_value_label, LV_ALIGN_RIGHT_MID, -8, -2);

    lv_obj_t *thermal_card = panel(scr, 24, 238, 250, 160);
    lv_obj_t *thermal_title = label(thermal_card, "K230 Thermal",
                                    &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(thermal_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *thermal_state = label(thermal_card, temp_text,
                                    &lv_font_montserrat_28, temp_color);
    home_temp_value_label = thermal_state;
    lv_obj_align(thermal_state, LV_ALIGN_BOTTOM_LEFT, 0, -16);
    lv_obj_t *thermal_note = label(thermal_card, "Die temperature",
                                   &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(thermal_note, LV_ALIGN_BOTTOM_LEFT, 0, 16);

    lv_obj_t *power_card = panel(scr, 294, 238, 250, 160);
    lv_obj_t *power_title = label(power_card, "Power", &lv_font_montserrat_20,
                                  0xF2F5F8);
    lv_obj_align(power_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *power_state = label(power_card, power_source_text,
                                  &lv_font_montserrat_24, source_color);
    home_power_source_value_label = power_state;
    lv_obj_set_width(power_state, 218);
    lv_label_set_long_mode(power_state, LV_LABEL_LONG_DOT);
    lv_obj_align(power_state, LV_ALIGN_BOTTOM_LEFT, 0, -48);
    lv_obj_t *power_note = label(power_card, power_draw_text,
                                 &lv_font_montserrat_16, draw_color);
    home_power_draw_value_label = power_note;
    lv_obj_set_width(power_note, 218);
    lv_label_set_long_mode(power_note, LV_LABEL_LONG_DOT);
    lv_obj_align(power_note, LV_ALIGN_BOTTOM_LEFT, 0, -12);

    lv_obj_t *apps = scroll_region(scr, 24, 424, 520, logical_h - 448);
    create_home_app_grid(apps, 4, 118, 106, 12, 6);
    home_apply_scroll_restore(apps);

    update_time_labels(NULL);
    home_telemetry_timer = lv_timer_create(home_telemetry_timer_cb, 3000, NULL);
}

static void create_header(lv_obj_t *scr, const char *title)
{
    int title_x = 108;
    int landscape = display_logical_width() > display_logical_height();

    if(!app_edge_back_enabled()) {
        lv_obj_t *back = lv_obj_create(scr);
        lv_obj_add_style(back, &style_button, 0);
        lv_obj_add_style(back, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_pos(back, 24, 74);
        lv_obj_set_size(back, 64, 54);
        lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(back, 8);
        lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *back_icon = label(back, LV_SYMBOL_LEFT,
                                    &lv_font_montserrat_24, 0xF2F5F8);
        lv_obj_center(back_icon);
        make_click_forwarder(back_icon);
    } else {
        title_x = 24;
    }

    if(!landscape) {
        lv_obj_t *page_title = label(scr, title, &lv_font_montserrat_28,
                                     0xF2F5F8);
        lv_obj_align(page_title, LV_ALIGN_TOP_LEFT, title_x, 82);
    }
}

static void info_row(lv_obj_t *parent, int y, const char *name, const char *value,
                     uint32_t value_color)
{
    lv_obj_t *left = label(parent, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *right = label(parent, value, &lv_font_montserrat_20, value_color);
    lv_obj_set_width(right, 260);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, 0, y - 2);
}

static void info_row_with_side_gap(lv_obj_t *parent, int y, const char *name,
                                   const char *value, uint32_t value_color,
                                   int side_gap)
{
    int parent_w = parent ? lv_obj_get_width(parent) : page_body_width();
    int value_w;
    lv_obj_t *left;
    lv_obj_t *right;

    if(parent_w <= 0) {
        parent_w = page_body_width();
    }
    if(side_gap < 0) {
        side_gap = 0;
    }
    value_w = parent_w - side_gap * 2 - 190;
    if(value_w < 260) {
        value_w = 260;
    }

    left = label(parent, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, side_gap, y);

    right = label(parent, value, &lv_font_montserrat_20, value_color);
    lv_obj_set_width(right, value_w);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -side_gap, y - 2);
}

static lv_obj_t *command_button(lv_obj_t *parent, int x, int y, int w, const char *text,
                                uint32_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    int fit_w = w == 488 ? ui_fit_width(parent, x, w) : w;

    lv_obj_add_style(btn, &style_button, 0);
    lv_obj_add_style(btn, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x202832), 0);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, fit_w, 60);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t *lbl = label(btn, text, &lv_font_montserrat_18, color);
    lv_obj_center(lbl);
    make_click_forwarder(lbl);
    return btn;
}

static lv_obj_t *settings_nav_row(lv_obj_t *parent, int y, const char *symbol,
                                  const char *title, const char *subtitle,
                                  uint32_t color, page_id_t page)
{
    lv_obj_t *row = lv_obj_create(parent);
    int row_w = ui_fit_width(parent, 0, 520);

    lv_obj_add_style(row, &style_button, 0);
    lv_obj_add_style(row, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, row_w, 94);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 6);
    lv_obj_add_event_cb(row, app_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)page);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_add_style(icon_box, &style_icon, 0);
    lv_obj_set_size(icon_box, 54, 54);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    ui_style_icon_box_for_page(icon_box, page, color);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    make_click_forwarder(icon_box);

    ui_create_page_icon(icon_box, page, symbol, &lv_font_montserrat_22,
                        0xFFFFFF, 54);

    lv_obj_t *name = label(row, title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 76, 14);
    make_click_forwarder(name);

    lv_obj_t *detail = label(row, subtitle, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(detail, row_w > 160 ? row_w - 160 : 360);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 76, 48);
    make_click_forwarder(detail);

    lv_obj_t *arrow = label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    make_click_forwarder(arrow);
    return row;
}

static void network_update_page(void)
{
    wifi_ap_info_t aps[NET_MAX_APS];
    int count;
    int selected;
    int scan_busy;
    int connect_busy;
    char status[NET_STATUS_MAX];
    char wifi_state[96];
    char wifi_ip[64];
    char eth_state[96];
    char eth_ip[64];
    uint32_t wifi_color = 0x9AA4AF;
    uint32_t eth_color = 0x9AA4AF;

    if(!network_status_label) {
        return;
    }

    read_iface_state(NET_WIFI_IFACE, wifi_state, sizeof(wifi_state), &wifi_color);
    read_iface_state(NET_ETH_IFACE, eth_state, sizeof(eth_state), &eth_color);
    read_iface_ip(NET_WIFI_IFACE, wifi_ip, sizeof(wifi_ip));
    read_iface_ip(NET_ETH_IFACE, eth_ip, sizeof(eth_ip));

    pthread_mutex_lock(&network_lock);
    memcpy(aps, network_aps, sizeof(aps));
    count = network_ap_count;
    selected = network_selected_ap;
    scan_busy = network_scan_busy;
    connect_busy = network_connect_busy;
    snprintf(status, sizeof(status), "%s", network_status_text);
    network_result_ready = 0;
    pthread_mutex_unlock(&network_lock);

    if(network_wifi_state_label) {
        lv_label_set_text(network_wifi_state_label, wifi_state);
        lv_obj_set_style_text_color(network_wifi_state_label, lv_color_hex(wifi_color), 0);
    }
    if(network_wifi_ip_label) {
        lv_label_set_text(network_wifi_ip_label, wifi_ip);
    }
    if(network_eth_state_label) {
        lv_label_set_text(network_eth_state_label, eth_state);
        lv_obj_set_style_text_color(network_eth_state_label, lv_color_hex(eth_color), 0);
    }
    if(network_eth_ip_label) {
        lv_label_set_text(network_eth_ip_label, eth_ip);
    }
    if(network_status_label) {
        if(scan_busy) {
            lv_label_set_text(network_status_label, "Scanning WiFi...");
        } else if(connect_busy) {
            lv_label_set_text(network_status_label, "Connecting WiFi...");
        } else {
            lv_label_set_text(network_status_label, status);
        }
    }
    if(network_selected_label) {
        lv_label_set_text(network_selected_label,
                          selected >= 0 && selected < count ?
                          aps[selected].ssid : "No network selected");
    }

    for(int i = 0; i < NET_MAX_APS; i++) {
        char meta[128];
        uint32_t row_color;

        if(!network_ap_btn[i] || !network_ap_title[i] || !network_ap_meta[i]) {
            continue;
        }

        if(i < count) {
            lv_obj_clear_flag(network_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(network_ap_title[i], aps[i].ssid);
            snprintf(meta, sizeof(meta), "%s  %s  %s",
                     aps[i].security[0] ? aps[i].security : "Open",
                     aps[i].quality[0] ? aps[i].quality : "--",
                     aps[i].signal[0] ? aps[i].signal : "");
            lv_label_set_text(network_ap_meta[i], meta);
            row_color = i == selected ? 0x1E3A2F : 0x151B22;
            lv_obj_set_style_bg_color(network_ap_btn[i], lv_color_hex(row_color), 0);
        } else {
            lv_label_set_text(network_ap_title[i], i == 0 ? "No scan results" : "");
            lv_label_set_text(network_ap_meta[i], i == 0 ? "Tap Scan" : "");
            lv_obj_set_style_bg_color(network_ap_btn[i], lv_color_hex(0x151B22), 0);
            if(i > 0) {
                lv_obj_add_flag(network_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(network_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void network_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    network_update_page();
}

static void wifi_scan_event_cb(lv_event_t *event)
{
    pthread_t thread;
    int start_thread = 0;

    (void)event;

    pthread_mutex_lock(&network_lock);
    if(!network_scan_busy && !network_connect_busy) {
        network_scan_busy = 1;
        network_set_status_locked("Scanning WiFi...");
        start_thread = 1;
    }
    pthread_mutex_unlock(&network_lock);

    if(start_thread) {
        if(pthread_create(&thread, NULL, wifi_scan_thread_cb, NULL) == 0) {
            pthread_detach(thread);
        } else {
            pthread_mutex_lock(&network_lock);
            network_scan_busy = 0;
            network_set_status_locked("Scan thread failed");
            pthread_mutex_unlock(&network_lock);
        }
    }

    network_update_page();
    request_fast_refresh();
}

static void wifi_ap_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    int valid = 0;

    pthread_mutex_lock(&network_lock);
    if(index >= 0 && index < network_ap_count) {
        network_selected_ap = index;
        snprintf(network_selected_ssid, sizeof(network_selected_ssid), "%s",
                 network_aps[index].ssid);
        network_set_status_locked("Selected %s", network_selected_ssid);
        valid = 1;
    }
    pthread_mutex_unlock(&network_lock);

    if(valid && network_password_ta) {
        lv_obj_send_event(network_password_ta, LV_EVENT_FOCUSED, NULL);
    }

    network_update_page();
    request_fast_refresh();
}

static void legacy_network_password_submit_cb(const char *password,
                                              void *user_data)
{
    (void)user_data;
    if(network_password_ta && lv_obj_is_valid(network_password_ta)) {
        lv_textarea_set_text(network_password_ta, password ? password : "");
    }
    network_update_page();
    request_fast_refresh();
}

static void wifi_connect_event_cb(lv_event_t *event)
{
    wifi_connect_request_t *req;
    pthread_t thread;
    const char *password = "";

    (void)event;

    req = calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&network_lock);
        network_set_status_locked("No memory for WiFi connect");
        pthread_mutex_unlock(&network_lock);
        return;
    }

    if(network_password_ta) {
        password = lv_textarea_get_text(network_password_ta);
    }

    pthread_mutex_lock(&network_lock);
    if(network_selected_ap < 0 || network_selected_ap >= network_ap_count) {
        network_set_status_locked("Select a WiFi network first");
        pthread_mutex_unlock(&network_lock);
        free(req);
        network_update_page();
        return;
    }
    if(network_scan_busy || network_connect_busy) {
        pthread_mutex_unlock(&network_lock);
        free(req);
        return;
    }

    snprintf(req->ssid, sizeof(req->ssid), "%s", network_aps[network_selected_ap].ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");
    network_connect_busy = 1;
    network_set_status_locked("Connecting to %s", req->ssid);
    pthread_mutex_unlock(&network_lock);

    if(pthread_create(&thread, NULL, wifi_connect_thread_cb, req) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&network_lock);
        network_connect_busy = 0;
        network_set_status_locked("Connect thread failed");
        pthread_mutex_unlock(&network_lock);
        free(req);
    }

    network_update_page();
    request_fast_refresh();
}

static void wifi_password_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    ui_input_dialog_config_t config = {0};
    const char *initial = "";
    int encrypted = 1;

    if(code != LV_EVENT_FOCUSED && code != LV_EVENT_CLICKED) {
        return;
    }

    if(network_password_ta) {
        initial = lv_textarea_get_text(network_password_ta);
    }
    pthread_mutex_lock(&network_lock);
    if(network_selected_ap >= 0 && network_selected_ap < network_ap_count) {
        encrypted = network_aps[network_selected_ap].encrypted;
    }
    pthread_mutex_unlock(&network_lock);

    config.title = "Wi-Fi password";
    config.placeholder = "Password, empty for open WiFi";
    config.initial_text = initial;
    config.password_mode = 1;
    config.max_length = NET_PASS_MAX - 1U;
    config.min_length = encrypted ? 8U : 0U;
    config.min_length_text = "Password must be at least 8 chars";
    config.submit_cb = legacy_network_password_submit_cb;
    config.submit_text = "OK";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void display_brightness_event_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int value = (int)lv_slider_get_value(slider);
    char text[64];

    if(write_backlight_value(value) == 0) {
        char pref_value[24];

        if(backlight_current_value != value) {
            lv_slider_set_value(slider, backlight_current_value, LV_ANIM_OFF);
        }
        snprintf(pref_value, sizeof(pref_value), "%d", backlight_current_value);
        ui_prefs_set(DISPLAY_BRIGHTNESS_PREF_KEY, pref_value);
        snprintf(text, sizeof(text), "%d / %d", backlight_current_value,
                 backlight_max_value);
    } else {
        snprintf(text, sizeof(text), "Backlight device missing");
    }

    if(display_brightness_label) {
        lv_label_set_text(display_brightness_label, text);
    }
}

static void display_style_orientation_button(lv_obj_t *btn, int selected,
                                             uint32_t accent)
{
    uint32_t child_count;

    if(!btn) {
        return;
    }

    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? accent : 0x202832), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(selected ? accent : 0x2A3037),
                                  0);

    child_count = lv_obj_get_child_count(btn);
    for(uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        lv_obj_set_style_text_color(child,
                                    lv_color_hex(selected ? 0xFFFFFF : accent),
                                    0);
    }
}

static void display_update_orientation_controls(void)
{
    static const int rotations[4] = {0, 90, 180, 270};

    if(display_orientation_label) {
        char text[32];

        snprintf(text, sizeof(text), "%d deg", display_rotation_degrees);
        lv_label_set_text(display_orientation_label, text);
        lv_obj_set_style_text_color(display_orientation_label,
                                    lv_color_hex(display_orientation_is_landscape() ?
                                                 0x3DA5FF : 0x25C281),
                                    0);
    }

    for(size_t i = 0; i < 4; i++) {
        display_style_orientation_button(display_rotation_btn[i],
                                         display_rotation_degrees == rotations[i],
                                         i == 0 ? 0x25C281 : 0x3DA5FF);
    }
}

static void display_update_font_controls(void)
{
    static const char *const modes[3] = {"small", "medium", "large"};
    const char *mode = ui_font_size_mode();

    if(display_font_label) {
        lv_label_set_text(display_font_label, ui_tr(ui_font_size_label()));
        lv_obj_set_style_text_color(display_font_label,
                                    lv_color_hex(strcmp(mode, "medium") == 0 ?
                                                 0x25C281 : 0x3DA5FF),
                                    0);
    }

    for(size_t i = 0; i < 3; i++) {
        display_style_orientation_button(display_font_btn[i],
                                         strcmp(mode, modes[i]) == 0,
                                         i == 1 ? 0x25C281 : 0x3DA5FF);
    }
}

static const char *page_transition_label_text(void)
{
    if(strcmp(page_transition_effect, "fade") == 0) {
        return "Fade";
    }
    if(strcmp(page_transition_effect, "slide_left") == 0) {
        return "Left";
    }
    if(strcmp(page_transition_effect, "slide_right") == 0) {
        return "Right";
    }
    if(strcmp(page_transition_effect, "slide_up") == 0) {
        return "Up";
    }
    if(strcmp(page_transition_effect, "cover") == 0) {
        return "Cover";
    }
    return "Off";
}

static void display_update_transition_controls(void)
{
    static const char *const modes[PAGE_TRANSITION_MODE_COUNT] = {
        "off", "fade", "slide_left", "slide_right", "slide_up", "cover",
    };

    if(display_transition_label) {
        lv_label_set_text(display_transition_label,
                          ui_tr(page_transition_label_text()));
        lv_obj_set_style_text_color(display_transition_label,
                                    lv_color_hex(page_transition_enabled() ?
                                                 0x3DA5FF : 0x9AA4AF), 0);
    }

    for(size_t i = 0; i < PAGE_TRANSITION_MODE_COUNT; i++) {
        display_style_orientation_button(display_transition_btn[i],
                                         strcmp(page_transition_effect,
                                                modes[i]) == 0,
                                         i == 0 ? 0x9AA4AF : 0x3DA5FF);
    }
}

static void display_update_timeout_controls(void)
{
    static const int timeouts[DISPLAY_TIMEOUT_MODE_COUNT] = {5, 10, 30, 60, 0};

    display_timeout_load_pref();
    if(display_timeout_label) {
        lv_label_set_text(display_timeout_label,
                          ui_tr(display_timeout_label_text(display_timeout_s)));
        lv_obj_set_style_text_color(display_timeout_label,
                                    lv_color_hex(display_timeout_s > 0 ?
                                                 0x25C281 : 0x9AA4AF), 0);
    }

    for(size_t i = 0; i < DISPLAY_TIMEOUT_MODE_COUNT; i++) {
        display_style_orientation_button(display_timeout_btn[i],
                                         display_timeout_s == timeouts[i],
                                         timeouts[i] == 0 ? 0x9AA4AF : 0x25C281);
    }
}

static void display_update_keyboard_auto_rotate_control(void)
{
    if(!display_keyboard_auto_rotate_switch ||
       !lv_obj_is_valid(display_keyboard_auto_rotate_switch)) {
        return;
    }
    if(ui_extension_keyboard_auto_rotate_enabled()) {
        lv_obj_add_state(display_keyboard_auto_rotate_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(display_keyboard_auto_rotate_switch,
                           LV_STATE_CHECKED);
    }
}

static void display_keyboard_auto_rotate_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    ui_extension_keyboard_set_auto_rotate_enabled(
        sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
    display_update_keyboard_auto_rotate_control();
    request_fast_refresh();
}

static void display_timeout_event_cb(lv_event_t *event)
{
    int seconds = (int)(intptr_t)lv_event_get_user_data(event);

    display_timeout_save_pref(seconds);
    display_update_timeout_controls();
    request_fast_refresh();
}

static void display_transition_event_cb(lv_event_t *event)
{
    const char *mode = (const char *)lv_event_get_user_data(event);

    page_transition_save_pref(mode);
    display_update_transition_controls();
    request_fast_refresh();
}

static void display_font_size_event_cb(lv_event_t *event)
{
    const char *mode = (const char *)lv_event_get_user_data(event);

    if(!mode) {
        return;
    }

    trace_ui_action("LVGL_DISPLAY_FONT_SIZE", PAGE_DISPLAY);
    ui_set_font_size_mode(mode);
    ui_fonts_apply_theme(main_display);
    display_update_font_controls();
    render_page(current_page, LV_SCREEN_LOAD_ANIM_NONE, 0);
}

static void display_orientation_event_cb(lv_event_t *event)
{
    const char *value = (const char *)lv_event_get_user_data(event);
    int old_degrees;
    int degrees;

    if(!value) {
        return;
    }

    trace_ui_action("LVGL_DISPLAY_ORIENTATION", PAGE_DISPLAY);
    old_degrees = display_rotation_degrees;
    degrees = parse_display_rotation_degrees(value);
    set_display_rotation_degrees(degrees);
    save_runtime_display_orientation();
    touch_trace_log("DISPLAY_ROTATION_RUNTIME degrees=%d persistent=on",
                    display_rotation_degrees);

    apply_display_rotation_change(old_degrees, "display-settings");
}

static uint32_t compact_page_color(page_id_t page)
{
    switch(page) {
    case PAGE_CAMERA:
        return 0x3DA5FF;
    case PAGE_SYSTEM:
        return 0x25C281;
    case PAGE_SETTINGS:
        return 0xF5A524;
    case PAGE_HOME:
        return 0x8B5CF6;
    default:
        return 0x22D3EE;
    }
}

static void create_compact_switch_shell(lv_obj_t *scr)
{
    lv_obj_t *body = panel(scr, 24, 154, 520, 300);

    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    compact_accent = lv_obj_create(body);
    lv_obj_set_pos(compact_accent, 0, 0);
    lv_obj_set_size(compact_accent, 8, 300);
    lv_obj_set_style_bg_opa(compact_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(compact_accent, 0, 0);
    lv_obj_set_style_radius(compact_accent, 4, 0);
    lv_obj_clear_flag(compact_accent, LV_OBJ_FLAG_SCROLLABLE);

    compact_title_label = label(body, "Home", &lv_font_montserrat_32, 0xF2F5F8);
    lv_obj_align(compact_title_label, LV_ALIGN_TOP_LEFT, 28, 20);

    compact_state_label = label(body, "Ready", &lv_font_montserrat_24, 0x25C281);
    lv_obj_align(compact_state_label, LV_ALIGN_TOP_LEFT, 28, 82);

    compact_detail_label = label(body, "Compact refresh diagnostic", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(compact_detail_label, 450);
    lv_label_set_long_mode(compact_detail_label, LV_LABEL_LONG_DOT);
    lv_obj_align(compact_detail_label, LV_ALIGN_TOP_LEFT, 28, 134);

    compact_hint_label = label(body, "Use the dock to switch pages", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(compact_hint_label, 450);
    lv_label_set_long_mode(compact_hint_label, LV_LABEL_LONG_DOT);
    lv_obj_align(compact_hint_label, LV_ALIGN_TOP_LEFT, 28, 190);
}

static void render_compact_switch_page(lv_obj_t *scr, page_id_t page)
{
    char detail[96];
    char hint[96];
    const char *state = "Ready";
    uint32_t state_color = 0x25C281;
    uint32_t accent = compact_page_color(page);

    if(!compact_title_label) {
        create_compact_switch_shell(scr);
    }

    switch(page) {
    case PAGE_HOME:
        snprintf(detail, sizeof(detail), "Static shell active");
        snprintf(hint, sizeof(hint), "R58 updates labels only on page switch");
        break;
    case PAGE_CAMERA:
        state = has_video_node() ? "Camera node present" : "Camera node missing";
        state_color = has_video_node() ? 0x25C281 : 0xF5A524;
        snprintf(detail, sizeof(detail), "Sensor: GC2093");
        snprintf(hint, sizeof(hint), "Preview integration remains Stage 2");
        break;
    case PAGE_SYSTEM:
        snprintf(detail, sizeof(detail), "Board: T-Display K230");
        snprintf(hint, sizeof(hint), "Panel: RM69A10 568x1232");
        break;
    case PAGE_SETTINGS:
        state = "Settings shell";
        state_color = 0xF5A524;
        snprintf(detail, sizeof(detail), "Input: %s", find_input_event() ? find_input_event() : "None");
        snprintf(hint, sizeof(hint), "Autostart enabled by init script");
        break;
    default:
        snprintf(detail, sizeof(detail), "%s page placeholder", page_name(page));
        snprintf(hint, sizeof(hint), "Compact switch diagnostic");
        break;
    }

    lv_obj_set_style_bg_color(compact_accent, lv_color_hex(accent), 0);
    lv_label_set_text(compact_title_label, page_name(page));
    lv_label_set_text(compact_state_label, state);
    lv_obj_set_style_text_color(compact_state_label, lv_color_hex(state_color), 0);
    lv_label_set_text(compact_detail_label, detail);
    lv_label_set_text(compact_hint_label, hint);
}

static void r59_set_info(int row, const char *name, const char *value, uint32_t color)
{
    if(row < 0 || row >= 4 || !r59_info_name[row] || !r59_info_value[row]) {
        return;
    }

    lv_label_set_text(r59_info_name[row], name);
    lv_label_set_text(r59_info_value[row], value);
    lv_obj_set_style_text_color(r59_info_value[row], lv_color_hex(color), 0);
}

static lv_obj_t *r59_action_button(lv_obj_t *parent, int x, const char *text, page_id_t page)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_add_style(btn, &style_button, 0);
    lv_obj_add_style(btn, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_pos(btn, x, 168);
    lv_obj_set_size(btn, 220, 62);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_add_event_cb(btn, app_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)page);

    lv_obj_t *lbl = label(btn, text, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_center(lbl);
    make_click_forwarder(lbl);
    return lbl;
}

static void create_r59_persistent_panel(lv_obj_t *scr)
{
    lv_obj_t *hero = panel(scr, 24, 72, 520, 166);
    lv_obj_t *info = panel(scr, 24, 260, 520, 304);
    lv_obj_t *actions = panel(scr, 24, 590, 520, 268);

    lv_obj_set_style_bg_color(hero, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_color(actions, lv_color_hex(0x101418), 0);

    r59_accent = lv_obj_create(hero);
    lv_obj_set_pos(r59_accent, 0, 0);
    lv_obj_set_size(r59_accent, 10, 166);
    lv_obj_set_style_bg_opa(r59_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r59_accent, 0, 0);
    lv_obj_set_style_radius(r59_accent, 4, 0);
    lv_obj_clear_flag(r59_accent, LV_OBJ_FLAG_SCROLLABLE);

    r59_title_label = label(hero, "Home", &lv_font_montserrat_32, 0xF2F5F8);
    lv_obj_align(r59_title_label, LV_ALIGN_TOP_LEFT, 30, 22);

    r59_state_label = label(hero, "Ready", &lv_font_montserrat_24, 0x25C281);
    lv_obj_align(r59_state_label, LV_ALIGN_TOP_LEFT, 30, 76);

    r59_detail_label = label(hero, "Persistent panel UI", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(r59_detail_label, 450);
    lv_label_set_long_mode(r59_detail_label, LV_LABEL_LONG_DOT);
    lv_obj_align(r59_detail_label, LV_ALIGN_TOP_LEFT, 30, 118);

    for(int i = 0; i < 4; i++) {
        int y = 18 + i * 64;

        r59_info_name[i] = label(info, "--", &lv_font_montserrat_18, 0x9AA4AF);
        lv_obj_align(r59_info_name[i], LV_ALIGN_TOP_LEFT, 0, y);

        r59_info_value[i] = label(info, "--", &lv_font_montserrat_20, 0xF2F5F8);
        lv_obj_set_width(r59_info_value[i], 280);
        lv_label_set_long_mode(r59_info_value[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(r59_info_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r59_info_value[i], LV_ALIGN_TOP_RIGHT, 0, y - 2);
    }

    lv_obj_t *action_title = label(actions, "Quick actions", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(action_title, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *action_detail = label(actions, "Fixed widgets keep page switches below the dirty-region threshold",
                                    &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(action_detail, ui_inner_width());
    lv_label_set_long_mode(action_detail, LV_LABEL_LONG_DOT);
    lv_obj_align(action_detail, LV_ALIGN_TOP_LEFT, 0, 44);

    r59_action_label[0] = r59_action_button(actions, 0, "Motion", PAGE_MOTION);
    r59_action_label[1] = r59_action_button(actions, 256, "Touch Test", PAGE_TOUCH_TEST);
}

static void render_r59_persistent_page(lv_obj_t *scr, page_id_t page)
{
    char detail[128];
    const char *state = "Ready";
    uint32_t state_color = 0x25C281;
    uint32_t accent = compact_page_color(page);

    if(!r59_title_label) {
        create_r59_persistent_panel(scr);
    }

    switch(page) {
    case PAGE_HOME:
        snprintf(detail, sizeof(detail), "Small-area navigation active");
        r59_set_info(0, "Camera", has_video_node() ? "Ready" : "No node",
                     has_video_node() ? 0x25C281 : 0xF5A524);
        r59_set_info(1, "Display", "RM69A10 52Hz", 0x3DA5FF);
        r59_set_info(2, "Touch", find_input_event() ? "Online" : "Missing",
                     find_input_event() ? 0x25C281 : 0xF5A524);
        r59_set_info(3, "Mode", "Persistent panels", 0x8B5CF6);
        break;
    case PAGE_CAMERA:
        state = has_video_node() ? "Camera ready" : "Camera missing";
        state_color = has_video_node() ? 0x25C281 : 0xF5A524;
        snprintf(detail, sizeof(detail), "GC2093 bring-up slot");
        r59_set_info(0, "Sensor", "GC2093", 0xF2F5F8);
        r59_set_info(1, "Video node", has_video_node() ? "Present" : "Missing",
                     has_video_node() ? 0x25C281 : 0xF5A524);
        r59_set_info(2, "ISP server", "Pending", 0xF5A524);
        r59_set_info(3, "Preview", "Stage 2", 0x3DA5FF);
        break;
    case PAGE_SYSTEM:
        snprintf(detail, sizeof(detail), "Runtime status");
        r59_set_info(0, "Board", "T-Display K230", 0xF2F5F8);
        r59_set_info(1, "Panel", "568x1232", 0xF2F5F8);
        r59_set_info(2, "Runtime", "Buildroot", 0x3DA5FF);
        r59_set_info(3, "Display path", "DRM direct", 0x25C281);
        break;
    case PAGE_SETTINGS:
        state = "Settings";
        state_color = 0xF5A524;
        snprintf(detail, sizeof(detail), "Device preferences");
        r59_set_info(0, "Orientation", "Portrait", 0x25C281);
        r59_set_info(1, "Autostart", "Enabled", 0x25C281);
        r59_set_info(2, "Input", find_input_event() ? find_input_event() : "None",
                     find_input_event() ? 0xF2F5F8 : 0xF5A524);
        r59_set_info(3, "UI refresh", "Small-area", 0x8B5CF6);
        break;
    case PAGE_APP_STARTUP:
        state = ui_meshtastic_autostart_enabled() ? "Autostart on" :
                                                     "Autostart off";
        state_color = ui_meshtastic_autostart_enabled() ? 0x25C281 :
                                                          0xF5A524;
        snprintf(detail, sizeof(detail), "Startup services");
        r59_set_info(0, "Meshtastic",
                     ui_meshtastic_autostart_enabled() ? "Enabled" :
                                                         "Disabled",
                     state_color);
        r59_set_info(1, "Daemon", "Background capable", 0x3DA5FF);
        r59_set_info(2, "Notifications", "Top banner", 0xA78BFA);
        r59_set_info(3, "Control", "Settings page", 0xF2F5F8);
        break;
    case PAGE_MOTION:
        snprintf(detail, sizeof(detail), "Motion page kept in safe panel mode");
        r59_set_info(0, "Animation", "Small object path", 0x25C281);
        r59_set_info(1, "Full rebuild", "Disabled", 0x25C281);
        r59_set_info(2, "Target", "No tearing", 0x3DA5FF);
        r59_set_info(3, "FPS path", "Next stage", 0xF5A524);
        break;
    case PAGE_TOUCH_TEST:
        snprintf(detail, sizeof(detail), "Touch test entry");
        r59_set_info(0, "Device", find_input_event() ? find_input_event() : "None",
                     find_input_event() ? 0xF2F5F8 : 0xF5A524);
        r59_set_info(1, "Trace", "/tmp/k230_touch_trace.log", 0x3DA5FF);
        r59_set_info(2, "Latency", "Raw logger active", 0x25C281);
        r59_set_info(3, "Canvas", "Use full page later", 0xF5A524);
        break;
    default:
        snprintf(detail, sizeof(detail), "Persistent placeholder");
        r59_set_info(0, "Navigation", "Ready", 0x25C281);
        r59_set_info(1, "Integration", "Next stage", 0xF5A524);
        r59_set_info(2, "UI refresh", "Small-area", 0x8B5CF6);
        r59_set_info(3, "Page", page_name(page), 0xF2F5F8);
        break;
    }

    lv_obj_set_style_bg_color(r59_accent, lv_color_hex(accent), 0);
    lv_label_set_text(r59_title_label, page_name(page));
    lv_label_set_text(r59_state_label, state);
    lv_obj_set_style_text_color(r59_state_label, lv_color_hex(state_color), 0);
    lv_label_set_text(r59_detail_label, detail);

    (void)r59_action_label;
}

static void clear_touch_canvas(void)
{
    touch_sample_count = 0;
    touch_trail_index = 0;
    memset(touch_multi_trail_index, 0, sizeof(touch_multi_trail_index));
    last_touch_draw_us = 0;
    last_touch_draw_x = -1;
    last_touch_draw_y = -1;
    for(size_t i = 0; i < UI_MULTITOUCH_MAX_POINTS; i++) {
        touch_multi_last_x[i] = -1;
        touch_multi_last_y[i] = -1;
    }
    last_touch_label_state = -1;
    last_touch_label_x = -1;
    last_touch_label_y = -1;
    last_touch_label_raw_seq = 0;

    if(touch_marker) {
        lv_obj_add_flag(touch_marker, LV_OBJ_FLAG_HIDDEN);
    }
    for(size_t i = 0; i < TOUCH_TRAIL_POINTS; i++) {
        if(touch_trail[i]) {
            lv_obj_add_flag(touch_trail[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for(size_t slot = 0; slot < UI_MULTITOUCH_MAX_POINTS; slot++) {
        if(touch_multi_marker[slot]) {
            lv_obj_add_flag(touch_multi_marker[slot], LV_OBJ_FLAG_HIDDEN);
        }
        for(size_t i = 0; i < TOUCH_MULTI_TRAIL_POINTS; i++) {
            if(touch_multi_trail[slot][i]) {
                lv_obj_add_flag(touch_multi_trail[slot][i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if(touch_state_label) {
        lv_label_set_text(touch_state_label, "Active: 0/10");
    }
    if(touch_xy_label) {
        lv_label_set_text(touch_xy_label, "p0: --");
    }
    if(touch_count_label) {
        lv_label_set_text(touch_count_label, "samples: 0");
    }
    if(touch_latency_label) {
        lv_label_set_text(touch_latency_label, "raw: --");
    }
    if(touch_mt_status_label) {
        lv_label_set_text(touch_mt_status_label, "mt: --");
    }
    request_fast_refresh();
}

static void touch_clear_event_cb(lv_event_t *event)
{
    (void)event;
    trace_ui_action("LVGL_PRESSED_CLEAR", current_page);
    clear_touch_canvas();
}

static uint32_t touch_slot_color(unsigned slot)
{
    static const uint32_t colors[UI_MULTITOUCH_MAX_POINTS] = {
        0x22D3EE, 0x25C281, 0xF5A524, 0xEF4D5A, 0x8B5CF6,
        0xEC4899, 0xA3E635, 0x38BDF8, 0xFB7185, 0xFACC15
    };

    return colors[slot % UI_MULTITOUCH_MAX_POINTS];
}

static int touch_screen_to_canvas(int32_t x, int32_t y, int32_t *cx, int32_t *cy)
{
    int32_t local_x = x - touch_canvas_x;
    int32_t local_y = y - touch_canvas_y;

    if(local_x < 0 || local_y < 0 ||
       local_x >= touch_canvas_w || local_y >= touch_canvas_h) {
        return 0;
    }

    if(cx) {
        *cx = local_x;
    }
    if(cy) {
        *cy = local_y;
    }
    return 1;
}

static void draw_touch_multi_point(const ui_touch_point_t *point)
{
    unsigned slot;
    int32_t cx;
    int32_t cy;
    lv_obj_t *trail;

    if(!point || !touch_area) {
        return;
    }

    slot = point->slot >= 0 ? (unsigned)point->slot : 0;
    if(slot >= UI_MULTITOUCH_MAX_POINTS ||
       !touch_screen_to_canvas(point->x, point->y, &cx, &cy)) {
        return;
    }

    if(touch_multi_marker[slot]) {
        lv_obj_set_pos(touch_multi_marker[slot], cx - 14, cy - 14);
        lv_obj_clear_flag(touch_multi_marker[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(touch_multi_marker[slot]);
    }

    if(point->x == touch_multi_last_x[slot] &&
       point->y == touch_multi_last_y[slot]) {
        return;
    }

    touch_multi_last_x[slot] = point->x;
    touch_multi_last_y[slot] = point->y;
    trail = touch_multi_trail[slot][touch_multi_trail_index[slot] %
                                   TOUCH_MULTI_TRAIL_POINTS];
    touch_multi_trail_index[slot]++;
    if(trail) {
        lv_obj_set_pos(trail, cx - 4, cy - 4);
        lv_obj_clear_flag(trail, LV_OBJ_FLAG_HIDDEN);
    }
}

static void draw_touch_dot(int32_t x, int32_t y, uint32_t color, int radius)
{
    int32_t cx = x - touch_canvas_x;
    int32_t cy = y - touch_canvas_y;
    lv_obj_t *trail;

    (void)color;
    (void)radius;

    if(!touch_area || !touch_marker) {
        return;
    }

    if(cx < 0 || cy < 0 || cx >= touch_canvas_w || cy >= touch_canvas_h) {
        return;
    }

    lv_obj_set_pos(touch_marker, cx - 8, cy - 8);
    lv_obj_clear_flag(touch_marker, LV_OBJ_FLAG_HIDDEN);

    trail = touch_trail[touch_trail_index % TOUCH_TRAIL_POINTS];
    touch_trail_index++;
    if(trail) {
        lv_obj_set_pos(trail, cx - 3, cy - 3);
        lv_obj_clear_flag(trail, LV_OBJ_FLAG_HIDDEN);
    }
    request_fast_refresh();
}

static void touch_sample_timer_cb(lv_timer_t *timer)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    int slot_seen[UI_MULTITOUCH_MAX_POINTS] = { 0 };
    lv_point_t point;
    lv_indev_state_t state;
    char buf[192];
    uint64_t now;
    uint64_t raw_app_us;
    uint64_t raw_lag_us;
    uint32_t raw_seq;
    uint32_t multi_count;
    uint32_t visible_count;
    int active_label_value;
    int state_value;
    int state_changed;

    (void)timer;

    if(!evdev_indev || !touch_area) {
        return;
    }

    lv_indev_get_point(evdev_indev, &point);
    state = lv_indev_get_state(evdev_indev);
    multi_count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    state_value = state == LV_INDEV_STATE_PRESSED ? 1 : 0;
    visible_count = multi_count > 0 ? multi_count : (uint32_t)state_value;
    active_label_value = (int)visible_count;
    state_changed = active_label_value != last_touch_label_state;

    if(touch_state_label && state_changed) {
        snprintf(buf, sizeof(buf), "Active: %lu/10", (unsigned long)visible_count);
        lv_label_set_text(touch_state_label, buf);
        lv_obj_set_style_text_color(touch_state_label,
                                    lv_color_hex(visible_count ? 0x25C281 : 0x9AA4AF),
                                    0);
    }

    if(touch_mt_status_label) {
        char mt_status[192];

        ui_multitouch_get_status(mt_status, sizeof(mt_status));
        lv_label_set_text(touch_mt_status_label, mt_status);
    }

    if(multi_count > 0) {
        ui_touch_point_t *first = &points[0];
        if(touch_xy_label) {
            size_t used = 0;
            uint32_t show_count = multi_count < 4 ? multi_count : 4;

            for(uint32_t i = 0; i < show_count; i++) {
                int written = snprintf(buf + used, sizeof(buf) - used,
                                       "%sp%d:%ld,%ld",
                                       i ? "  " : "",
                                       points[i].slot,
                                       (long)points[i].x,
                                       (long)points[i].y);

                if(written < 0) {
                    break;
                }
                if((size_t)written >= sizeof(buf) - used) {
                    used = sizeof(buf) - 1;
                    break;
                }
                used += (size_t)written;
            }
            if(multi_count > show_count && used < sizeof(buf) - 1) {
                snprintf(buf + used, sizeof(buf) - used, "  +%lu",
                         (unsigned long)(multi_count - show_count));
            }
            lv_label_set_text(touch_xy_label, buf);
            last_touch_label_x = first->x;
            last_touch_label_y = first->y;
        }
    } else if(touch_xy_label &&
              (point.x != last_touch_label_x || point.y != last_touch_label_y ||
               state_changed)) {
        snprintf(buf, sizeof(buf), "p0: %ld,%ld", (long)point.x, (long)point.y);
        lv_label_set_text(touch_xy_label, buf);
        last_touch_label_x = point.x;
        last_touch_label_y = point.y;
    }

    now = monotonic_us();

    pthread_mutex_lock(&touch_state_lock);
    raw_app_us = last_raw_syn_app_us;
    raw_lag_us = last_raw_lag_us;
    raw_seq = last_raw_seq;
    pthread_mutex_unlock(&touch_state_lock);

    if(touch_latency_label && raw_seq != last_touch_label_raw_seq) {
        if(raw_seq && raw_app_us) {
            snprintf(buf, sizeof(buf), "raw #%lu lag %.1fms",
                     (unsigned long)raw_seq, (double)raw_lag_us / 1000.0);
        } else {
            snprintf(buf, sizeof(buf), "raw: --");
        }
        lv_label_set_text(touch_latency_label, buf);
        last_touch_label_raw_seq = raw_seq;
    }

    if(multi_count > 0) {
        for(uint32_t i = 0; i < multi_count && i < UI_MULTITOUCH_MAX_POINTS; i++) {
            int slot = points[i].slot;

            if(slot >= 0 && slot < UI_MULTITOUCH_MAX_POINTS) {
                slot_seen[slot] = 1;
            }
            draw_touch_multi_point(&points[i]);
        }
        for(size_t slot = 0; slot < UI_MULTITOUCH_MAX_POINTS; slot++) {
            if(!slot_seen[slot]) {
                if(touch_multi_marker[slot]) {
                    lv_obj_add_flag(touch_multi_marker[slot], LV_OBJ_FLAG_HIDDEN);
                }
                touch_multi_last_x[slot] = -1;
                touch_multi_last_y[slot] = -1;
            }
        }
        touch_sample_count += multi_count;
        if(touch_count_label) {
            snprintf(buf, sizeof(buf), "samples: %lu  active: %lu",
                     (unsigned long)touch_sample_count,
                     (unsigned long)multi_count);
            lv_label_set_text(touch_count_label, buf);
        }
        request_fast_refresh();
    } else {
        for(size_t slot = 0; slot < UI_MULTITOUCH_MAX_POINTS; slot++) {
            if(touch_multi_marker[slot]) {
                lv_obj_add_flag(touch_multi_marker[slot], LV_OBJ_FLAG_HIDDEN);
            }
            touch_multi_last_x[slot] = -1;
            touch_multi_last_y[slot] = -1;
        }
    }

    if(multi_count == 0 && state == LV_INDEV_STATE_PRESSED &&
       (state_changed || last_touch_draw_us == 0 ||
        now - last_touch_draw_us >= TOUCH_DRAW_INTERVAL_US) &&
       (state_changed || point.x != last_touch_draw_x || point.y != last_touch_draw_y)) {
        touch_sample_count++;
        draw_touch_dot(point.x, point.y, 0x22D3EE, 3);
        last_touch_draw_us = now;
        last_touch_draw_x = point.x;
        last_touch_draw_y = point.y;

        if(touch_count_label) {
            snprintf(buf, sizeof(buf), "samples: %lu", (unsigned long)touch_sample_count);
            lv_label_set_text(touch_count_label, buf);
        }
    }

    last_touch_label_state = active_label_value;
}

static void create_touch_test_page(lv_obj_t *scr)
{
    lv_obj_t *clear_btn;
    lv_obj_t *body;
    int landscape = display_orientation_is_landscape();
    int body_y = page_content_top_y(154);
    int screen_w = display_logical_width();
    int screen_h = display_logical_height();
    int sidebar_w = landscape ? screen_w * 30 / 100 : 520;
    int sidebar_h = landscape ? page_body_height_from(154) : 136;
    int body_inner_w;
    int margin = 24;
    int gap = 20;

    create_header(scr, "Touch Test");

    if(landscape) {
        int min_canvas_w = 360;

        if(sidebar_w < 320) {
            sidebar_w = 320;
        }
        if(sidebar_w > 380) {
            sidebar_w = 380;
        }
        if(screen_w - margin - sidebar_w - gap - margin < min_canvas_w) {
            sidebar_w = screen_w - margin - gap - margin - min_canvas_w;
            if(sidebar_w < 280) {
                sidebar_w = 280;
            }
        }

        touch_canvas_x = margin + sidebar_w + gap;
        touch_canvas_y = body_y;
        touch_canvas_w = screen_w - touch_canvas_x - margin;
        touch_canvas_h = screen_h - touch_canvas_y - margin;
        if(touch_canvas_w < 260) {
            touch_canvas_w = 260;
        }
        if(touch_canvas_h < 220) {
            touch_canvas_h = 220;
        }
    } else {
        touch_canvas_x = TOUCH_CANVAS_X;
        touch_canvas_y = TOUCH_CANVAS_Y;
        touch_canvas_w = TOUCH_CANVAS_W;
        touch_canvas_h = TOUCH_CANVAS_H;
    }

    body = panel(scr, 24, body_y, sidebar_w, sidebar_h);
    body_inner_w = lv_obj_get_width(body) - 32;
    if(body_inner_w < 220) {
        body_inner_w = 220;
    }

    if(landscape) {
        lv_obj_t *device_title = label(body, "Device", &lv_font_montserrat_16,
                                       0x9AA4AF);
        lv_obj_align(device_title, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *device_value =
            label(body, find_input_event() ? find_input_event() : "None",
                  &lv_font_montserrat_18,
                  find_input_event() ? 0xF2F5F8 : 0xF5A524);
        lv_obj_set_width(device_value, body_inner_w);
        lv_label_set_long_mode(device_value, LV_LABEL_LONG_DOT);
        lv_obj_align(device_value, LV_ALIGN_TOP_LEFT, 0, 28);

        touch_state_label = label(body, "Active: 0/10", &lv_font_montserrat_20,
                                  0x9AA4AF);
        lv_obj_align(touch_state_label, LV_ALIGN_TOP_LEFT, 0, 78);

        touch_xy_label = label(body, "p0: --", &lv_font_montserrat_20,
                               0xF2F5F8);
        lv_obj_set_width(touch_xy_label, body_inner_w);
        lv_label_set_long_mode(touch_xy_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_xy_label, LV_ALIGN_TOP_LEFT, 0, 118);

        touch_count_label = label(body, "samples: 0", &lv_font_montserrat_18,
                                  0x9AA4AF);
        lv_obj_align(touch_count_label, LV_ALIGN_TOP_LEFT, 0, 166);

        touch_latency_label = label(body, "raw: --", &lv_font_montserrat_18,
                                    0x9AA4AF);
        lv_obj_set_width(touch_latency_label, body_inner_w);
        lv_label_set_long_mode(touch_latency_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_latency_label, LV_ALIGN_TOP_LEFT, 0, 204);

        touch_mt_status_label = label(body, "mt: --", &lv_font_montserrat_14,
                                      0x64748B);
        lv_obj_set_width(touch_mt_status_label, body_inner_w);
        lv_label_set_long_mode(touch_mt_status_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_mt_status_label, LV_ALIGN_TOP_LEFT, 0, 242);

        clear_btn = command_button(body, 0, sidebar_h - 76, body_inner_w,
                                   "Clear", 0x22D3EE);
        lv_obj_add_event_cb(clear_btn, touch_clear_event_cb, LV_EVENT_PRESSED,
                            NULL);
    } else {
        info_row(body, 0, "Device", find_input_event() ? find_input_event() : "None",
                 find_input_event() ? 0xF2F5F8 : 0xF5A524);

        touch_state_label = label(body, "Active: 0/10", &lv_font_montserrat_20,
                                  0x9AA4AF);
        lv_obj_align(touch_state_label, LV_ALIGN_TOP_LEFT, 0, 52);

        touch_xy_label = label(body, "p0: --", &lv_font_montserrat_20,
                               0xF2F5F8);
        lv_obj_set_width(touch_xy_label, 330);
        lv_label_set_long_mode(touch_xy_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_xy_label, LV_ALIGN_TOP_LEFT, 150, 52);

        touch_count_label = label(body, "samples: 0", &lv_font_montserrat_18,
                                  0x9AA4AF);
        lv_obj_align(touch_count_label, LV_ALIGN_TOP_LEFT, 0, 92);

        touch_latency_label = label(body, "raw: --", &lv_font_montserrat_18,
                                    0x9AA4AF);
        lv_obj_set_width(touch_latency_label, 210);
        lv_label_set_long_mode(touch_latency_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_latency_label, LV_ALIGN_TOP_LEFT, 136, 92);

        touch_mt_status_label = label(body, "mt: --", &lv_font_montserrat_14,
                                      0x64748B);
        lv_obj_set_width(touch_mt_status_label, 336);
        lv_label_set_long_mode(touch_mt_status_label, LV_LABEL_LONG_DOT);
        lv_obj_align(touch_mt_status_label, LV_ALIGN_TOP_LEFT, 0, 116);

        clear_btn = command_button(body, 350, 72, 138, "Clear", 0x22D3EE);
        lv_obj_add_event_cb(clear_btn, touch_clear_event_cb, LV_EVENT_PRESSED,
                            NULL);
    }

    touch_area = lv_obj_create(scr);
    lv_obj_set_pos(touch_area, touch_canvas_x, touch_canvas_y);
    lv_obj_set_size(touch_area, touch_canvas_w, touch_canvas_h);
    lv_obj_set_style_bg_color(touch_area, lv_color_hex(0x0F1318), 0);
    lv_obj_set_style_bg_opa(touch_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(touch_area, 1, 0);
    lv_obj_set_style_border_color(touch_area, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_radius(touch_area, 8, 0);
    lv_obj_set_style_pad_all(touch_area, 0, 0);
    lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);

    for(size_t i = 0; i < TOUCH_TRAIL_POINTS; i++) {
        touch_trail[i] = lv_obj_create(touch_area);
        lv_obj_set_size(touch_trail[i], 6, 6);
        lv_obj_set_style_bg_color(touch_trail[i], lv_color_hex(0x22D3EE), 0);
        lv_obj_set_style_bg_opa(touch_trail[i], LV_OPA_60, 0);
        lv_obj_set_style_border_width(touch_trail[i], 0, 0);
        lv_obj_set_style_radius(touch_trail[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(touch_trail[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(touch_trail[i], LV_OBJ_FLAG_HIDDEN);
    }

    touch_marker = lv_obj_create(touch_area);
    lv_obj_set_size(touch_marker, 16, 16);
    lv_obj_set_style_bg_color(touch_marker, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(touch_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(touch_marker, 3, 0);
    lv_obj_set_style_border_color(touch_marker, lv_color_hex(0x22D3EE), 0);
    lv_obj_set_style_radius(touch_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(touch_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(touch_marker, LV_OBJ_FLAG_HIDDEN);

    for(size_t slot = 0; slot < UI_MULTITOUCH_MAX_POINTS; slot++) {
        uint32_t color = touch_slot_color(slot);

        for(size_t i = 0; i < TOUCH_MULTI_TRAIL_POINTS; i++) {
            touch_multi_trail[slot][i] = lv_obj_create(touch_area);
            lv_obj_set_size(touch_multi_trail[slot][i], 8, 8);
            lv_obj_set_style_bg_color(touch_multi_trail[slot][i], lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(touch_multi_trail[slot][i], LV_OPA_50, 0);
            lv_obj_set_style_border_width(touch_multi_trail[slot][i], 0, 0);
            lv_obj_set_style_radius(touch_multi_trail[slot][i], LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(touch_multi_trail[slot][i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(touch_multi_trail[slot][i], LV_OBJ_FLAG_HIDDEN);
        }

        touch_multi_marker[slot] = lv_obj_create(touch_area);
        lv_obj_set_size(touch_multi_marker[slot], 28, 28);
        lv_obj_set_style_bg_color(touch_multi_marker[slot], lv_color_hex(0x0F1318), 0);
        lv_obj_set_style_bg_opa(touch_multi_marker[slot], LV_OPA_90, 0);
        lv_obj_set_style_border_width(touch_multi_marker[slot], 3, 0);
        lv_obj_set_style_border_color(touch_multi_marker[slot], lv_color_hex(color), 0);
        lv_obj_set_style_radius(touch_multi_marker[slot], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(touch_multi_marker[slot], 0, 0);
        lv_obj_clear_flag(touch_multi_marker[slot], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(touch_multi_marker[slot], LV_OBJ_FLAG_HIDDEN);

        touch_multi_marker_label[slot] = lv_label_create(touch_multi_marker[slot]);
        lv_label_set_text_fmt(touch_multi_marker_label[slot], "%lu",
                              (unsigned long)(slot + 1));
        lv_obj_set_style_text_font(touch_multi_marker_label[slot],
                                   &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(touch_multi_marker_label[slot],
                                    lv_color_hex(0xF2F5F8), 0);
        lv_obj_center(touch_multi_marker_label[slot]);
    }

    clear_touch_canvas();
    touch_timer = lv_timer_create(touch_sample_timer_cb, 16, NULL);
}

static void motion_reset_stats(uint64_t now)
{
    motion_start_us = now;
    motion_last_frame_us = 0;
    motion_frame_sum_us = 0;
    motion_frame_min_us = 0;
    motion_frame_max_us = 0;
    motion_last_stats_us = now;
    motion_frame_count = 0;
    motion_frame_dt_count = 0;
    motion_update_count = 0;
}

static void motion_update_stats_label(uint64_t now)
{
    uint64_t window_us = now - motion_last_stats_us;
    uint32_t fps;
    double avg_ms;
    char buf[144];

    if(!motion_rate_label || window_us < 1000000ULL) {
        return;
    }

    fps = (uint32_t)(((uint64_t)motion_frame_count * 1000000ULL +
                      window_us / 2) / window_us);
    avg_ms = motion_frame_dt_count ?
             (double)motion_frame_sum_us / (double)motion_frame_dt_count / 1000.0 : 0.0;

    snprintf(buf, sizeof(buf), "locked %lufps  avg %.1fms  min %.1f  max %.1f  step %lu/s",
             (unsigned long)fps,
             avg_ms,
             (double)motion_frame_min_us / 1000.0,
             (double)motion_frame_max_us / 1000.0,
             (unsigned long)(((uint64_t)motion_update_count * 1000000ULL +
                              window_us / 2) / window_us));
    lv_label_set_text(motion_rate_label, buf);
    touch_trace_log("MOTION_STATS mode=locked fps=%lu avg=%.3fms min=%.3fms max=%.3fms updates=%lu",
                    (unsigned long)fps,
                    avg_ms,
                    (double)motion_frame_min_us / 1000.0,
                    (double)motion_frame_max_us / 1000.0,
                    (unsigned long)(((uint64_t)motion_update_count * 1000000ULL +
                                     window_us / 2) / window_us));

    motion_frame_sum_us = 0;
    motion_frame_min_us = 0;
    motion_frame_max_us = 0;
    motion_frame_count = 0;
    motion_frame_dt_count = 0;
    motion_update_count = 0;
    motion_last_stats_us = now;
}

static void motion_step_frame_locked(uint64_t now)
{
    static const int32_t bar_speed[MOTION_BAR_COUNT] = {1, 1, 1, 2, 2, 2, 3, 3};
    static const int32_t box_speed[MOTION_BOX_COUNT] = {8, 10, 12, 14};

    if(current_page != PAGE_MOTION || !motion_start_us || page_transition_active) {
        return;
    }

    for(size_t i = 0; i < MOTION_BAR_COUNT; i++) {
        if(!motion_bars[i]) {
            continue;
        }

        motion_bar_x[i] += (int32_t)motion_bar_dir[i] * bar_speed[i];
        if(motion_bar_x[i] >= motion_bar_travel[i]) {
            motion_bar_x[i] = motion_bar_travel[i];
            motion_bar_dir[i] = -1;
        } else if(motion_bar_x[i] <= 0) {
            motion_bar_x[i] = 0;
            motion_bar_dir[i] = 1;
        }
        lv_obj_set_x(motion_bars[i], motion_bar_x[i]);
    }

    for(size_t i = 0; i < MOTION_BOX_COUNT; i++) {
        if(!motion_boxes[i]) {
            continue;
        }

        motion_box_angle[i] += box_speed[i];
        if(motion_box_angle[i] >= 3600) {
            motion_box_angle[i] -= 3600;
        }
        lv_obj_set_style_transform_rotation(motion_boxes[i], motion_box_angle[i], 0);
    }

    motion_update_count++;
    motion_update_stats_label(now);
}

static void create_motion_page(lv_obj_t *scr)
{
    static const uint32_t colors[] = {
        0x3DA5FF, 0x25C281, 0xF5A524, 0xEF4D5A, 0x8B5CF6, 0x22D3EE,
        0xEC4899, 0xA3E635
    };
    uint64_t now = monotonic_us();

    create_header(scr, "Motion");
    motion_reset_stats(now);
    memset(motion_bars, 0, sizeof(motion_bars));
    memset(motion_boxes, 0, sizeof(motion_boxes));
    memset(motion_bar_travel, 0, sizeof(motion_bar_travel));
    memset(motion_bar_x, 0, sizeof(motion_bar_x));
    memset(motion_bar_dir, 0, sizeof(motion_bar_dir));
    memset(motion_box_angle, 0, sizeof(motion_box_angle));

    lv_obj_t *body = panel(scr, 24, 154, 520, 780);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    lv_obj_t *title = label(body, "LVGL motion pacing", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    motion_rate_label = label(body, "locked --fps  avg --ms  min --  max --  step --/s",
                              &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(motion_rate_label, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_obj_set_width(motion_rate_label, ui_inner_width());
    lv_label_set_long_mode(motion_rate_label, LV_LABEL_LONG_CLIP);

    for(size_t i = 0; i < MOTION_BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_create(body);
        int32_t y = 96 + (int32_t)i * 58;
        int32_t w = 50 + (int32_t)(i % 3) * 16;

        lv_obj_set_pos(bar, 0, y);
        lv_obj_set_size(bar, w, 34);
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 8, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        motion_bars[i] = bar;
        motion_bar_travel[i] = 488 - w;
        motion_bar_x[i] = (motion_bar_travel[i] * (int32_t)(i + 1)) /
                          (int32_t)(MOTION_BAR_COUNT + 1);
        motion_bar_dir[i] = (i % 2) ? -1 : 1;
        lv_obj_set_x(bar, motion_bar_x[i]);
    }

    for(size_t i = 0; i < MOTION_BOX_COUNT; i++) {
        lv_obj_t *box = lv_obj_create(body);
        int32_t x = 38 + (int32_t)i * 116;

        lv_obj_set_pos(box, x, 608);
        lv_obj_set_size(box, 62, 62);
        lv_obj_set_style_bg_color(box, lv_color_hex(colors[(i + 2) % 8]), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_transform_pivot_x(box, 31, 0);
        lv_obj_set_style_transform_pivot_y(box, 31, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        motion_boxes[i] = box;
        motion_box_angle[i] = (int32_t)i * 450;
        lv_obj_set_style_transform_rotation(box, motion_box_angle[i], 0);
    }
}

static uint16_t rgb565_from_hex(uint32_t color)
{
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);

    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static void camera_fill_placeholder(uint8_t *dst, unsigned width, unsigned height)
{
    uint16_t bg = rgb565_from_hex(0x111820);
    uint16_t grid = rgb565_from_hex(0x1F2A36);
    uint16_t accent = rgb565_from_hex(0x3DA5FF);

    for(unsigned y = 0; y < height; y++) {
        for(unsigned x = 0; x < width; x++) {
            uint16_t color = bg;
            size_t offset = ((size_t)y * width + (size_t)x) * 2U;

            if((x % 72) == 0 || (y % 80) == 0) {
                color = grid;
            }
            if((x > width / 2 - 2 && x < width / 2 + 2) ||
               (y > height / 2 - 2 && y < height / 2 + 2)) {
                color = accent;
            }

            dst[offset + 0] = (uint8_t)(color & 0xFFU);
            dst[offset + 1] = (uint8_t)(color >> 8);
        }
    }
}

static uint8_t camera_clip8(int value)
{
    if(value < 0) {
        return 0;
    }
    if(value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static void camera_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                              uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;

    if(c < 0) {
        c = 0;
    }

    *r = camera_clip8((298 * c + 409 * e + 128) >> 8);
    *g = camera_clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
    *b = camera_clip8((298 * c + 516 * d + 128) >> 8);
}

static void camera_sample_yuv(const uint8_t *frame, unsigned width, unsigned height,
                              int nv12, unsigned x, unsigned y,
                              uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint8_t *y_plane = frame;
    const uint8_t *uv_plane = frame + width * height;
    unsigned uv_index;
    uint8_t yy = y_plane[y * width + x];
    uint8_t uu;
    uint8_t vv;

    if(nv12) {
        uv_index = (y / 2) * width + (x & ~1U);
    } else {
        uv_index = y * width + (x & ~1U);
    }

    uu = uv_plane[uv_index + 0];
    vv = uv_plane[uv_index + 1];
    camera_yuv_to_rgb(yy, uu, vv, r, g, b);
}

static void camera_cover_crop(unsigned src_w, unsigned src_h,
                              unsigned dst_w, unsigned dst_h,
                              unsigned *crop_x, unsigned *crop_y,
                              unsigned *crop_w, unsigned *crop_h)
{
    unsigned w = src_w;
    unsigned h = src_h;

    if(src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        if(crop_x) {
            *crop_x = 0;
        }
        if(crop_y) {
            *crop_y = 0;
        }
        if(crop_w) {
            *crop_w = src_w;
        }
        if(crop_h) {
            *crop_h = src_h;
        }
        return;
    }

    if((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w) {
        w = (unsigned)(((uint64_t)src_h * dst_w) / dst_h);
        if(w < 1U) {
            w = 1U;
        }
    } else {
        h = (unsigned)(((uint64_t)src_w * dst_h) / dst_w);
        if(h < 1U) {
            h = 1U;
        }
    }

    if(crop_x) {
        *crop_x = (src_w - w) / 2U;
    }
    if(crop_y) {
        *crop_y = (src_h - h) / 2U;
    }
    if(crop_w) {
        *crop_w = w;
    }
    if(crop_h) {
        *crop_h = h;
    }
}

static void camera_convert_yuv_to_rgb565_preview(const uint8_t *frame,
                                                 unsigned src_w,
                                                 unsigned src_h,
                                                 int nv12,
                                                 uint8_t *dst,
                                                 unsigned dst_w,
                                                 unsigned dst_h,
                                                 int rotate,
                                                 int flip_h,
                                                 int flip_v)
{
    unsigned base_w = rotate ? src_h : src_w;
    unsigned base_h = rotate ? src_w : src_h;
    unsigned crop_x = 0;
    unsigned crop_y = 0;
    unsigned crop_w = base_w;
    unsigned crop_h = base_h;

    camera_cover_crop(base_w, base_h, dst_w, dst_h, &crop_x, &crop_y,
                      &crop_w, &crop_h);

    for(unsigned y = 0; y < dst_h; y++) {
        unsigned sample_y = flip_v ? (dst_h - 1U - y) : y;
        unsigned base_y = crop_y +
                          (unsigned)(((uint64_t)sample_y * crop_h) / dst_h);
        if(base_y >= base_h) {
            base_y = base_h - 1U;
        }
        for(unsigned x = 0; x < dst_w; x++) {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint16_t rgb565;
            size_t offset = ((size_t)y * dst_w + (size_t)x) * 2U;
            unsigned sample_x = flip_h ? (dst_w - 1U - x) : x;
            unsigned base_x = crop_x +
                              (unsigned)(((uint64_t)sample_x * crop_w) / dst_w);
            unsigned src_x;
            unsigned src_y;

            if(base_x >= base_w) {
                base_x = base_w - 1U;
            }
            if(rotate) {
                src_x = base_y;
                src_y = src_h - 1U - base_x;
            } else {
                src_x = base_x;
                src_y = base_y;
            }
            if(src_x >= src_w) {
                src_x = src_w - 1U;
            }
            if(src_y >= src_h) {
                src_y = src_h - 1U;
            }

            camera_sample_yuv(frame, src_w, src_h, nv12, src_x, src_y,
                              &r, &g, &b);
            rgb565 = (uint16_t)(((r & 0xF8U) << 8) |
                                ((g & 0xFCU) << 3) |
                                (b >> 3));
            dst[offset + 0] = (uint8_t)(rgb565 & 0xFFU);
            dst[offset + 1] = (uint8_t)(rgb565 >> 8);
        }
    }
}

static void camera_rgb565_to_rgb(const uint8_t *buf, unsigned width,
                                 unsigned x, unsigned y,
                                 uint8_t *r, uint8_t *g, uint8_t *b)
{
    size_t off = ((size_t)y * width + x) * 2U;
    uint16_t value = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);

    *r = (uint8_t)(((value >> 11) & 0x1FU) * 255U / 31U);
    *g = (uint8_t)(((value >> 5) & 0x3FU) * 255U / 63U);
    *b = (uint8_t)((value & 0x1FU) * 255U / 31U);
}

static int camera_skin_like_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    int maxc = r > g ? r : g;
    int minc = r < g ? r : g;

    if(b > maxc) {
        maxc = b;
    }
    if(b < minc) {
        minc = b;
    }

    if(r < 72 || g < 38 || b < 22) {
        return 0;
    }
    if(r <= g + 8 || r <= b + 18) {
        return 0;
    }
    if(maxc - minc < 18) {
        return 0;
    }
    if(g > r || b > g + 44) {
        return 0;
    }
    return 1;
}

static void camera_insert_face_box(camera_face_box_t *boxes, int *count,
                                   const camera_face_box_t *candidate)
{
    int pos;

    if(!boxes || !count || !candidate || candidate->w <= 0 || candidate->h <= 0) {
        return;
    }

    pos = *count;
    if(pos < CAMERA_FACE_MAX_BOXES) {
        (*count)++;
    } else if(candidate->score <= boxes[CAMERA_FACE_MAX_BOXES - 1].score) {
        return;
    } else {
        pos = CAMERA_FACE_MAX_BOXES - 1;
    }

    while(pos > 0 && boxes[pos - 1].score < candidate->score) {
        boxes[pos] = boxes[pos - 1];
        pos--;
    }
    boxes[pos] = *candidate;
}

static int camera_detect_faces_rgb565(const uint8_t *buf, unsigned width,
                                      unsigned height,
                                      camera_face_box_t *boxes,
                                      int max_boxes)
{
    uint8_t mask[CAMERA_FACE_GRID_MAX_CELLS];
    uint8_t seen[CAMERA_FACE_GRID_MAX_CELLS];
    uint16_t queue[CAMERA_FACE_GRID_MAX_CELLS];
    unsigned step_x;
    unsigned step_y;
    unsigned grid_w;
    unsigned grid_h;
    int face_count = 0;

    if(!buf || !boxes || max_boxes <= 0 || width == 0 || height == 0) {
        return 0;
    }

    step_x = (width + CAMERA_FACE_GRID_MAX_W - 1U) / CAMERA_FACE_GRID_MAX_W;
    step_y = (height + CAMERA_FACE_GRID_MAX_H - 1U) / CAMERA_FACE_GRID_MAX_H;
    if(step_x == 0) {
        step_x = 1;
    }
    if(step_y == 0) {
        step_y = 1;
    }
    grid_w = (width + step_x - 1U) / step_x;
    grid_h = (height + step_y - 1U) / step_y;
    if(grid_w > CAMERA_FACE_GRID_MAX_W) {
        grid_w = CAMERA_FACE_GRID_MAX_W;
    }
    if(grid_h > CAMERA_FACE_GRID_MAX_H) {
        grid_h = CAMERA_FACE_GRID_MAX_H;
    }

    memset(mask, 0, sizeof(mask));
    memset(seen, 0, sizeof(seen));
    memset(boxes, 0, (size_t)max_boxes * sizeof(boxes[0]));

    for(unsigned gy = 0; gy < grid_h; gy++) {
        for(unsigned gx = 0; gx < grid_w; gx++) {
            unsigned x = gx * step_x + step_x / 2U;
            unsigned y = gy * step_y + step_y / 2U;
            uint8_t r;
            uint8_t g;
            uint8_t b;

            if(x >= width) {
                x = width - 1U;
            }
            if(y >= height) {
                y = height - 1U;
            }
            camera_rgb565_to_rgb(buf, width, x, y, &r, &g, &b);
            if(camera_skin_like_rgb(r, g, b)) {
                mask[gy * grid_w + gx] = 1;
            }
        }
    }

    for(unsigned gy = 0; gy < grid_h; gy++) {
        for(unsigned gx = 0; gx < grid_w; gx++) {
            unsigned idx = gy * grid_w + gx;
            unsigned head = 0;
            unsigned tail = 0;
            unsigned min_x = gx;
            unsigned max_x = gx;
            unsigned min_y = gy;
            unsigned max_y = gy;
            unsigned cells = 0;
            unsigned box_x;
            unsigned box_y;
            unsigned box_w;
            unsigned box_h;
            unsigned pad_x;
            unsigned pad_y;
            camera_face_box_t candidate;

            if(!mask[idx] || seen[idx]) {
                continue;
            }

            seen[idx] = 1;
            queue[tail++] = (uint16_t)idx;
            while(head < tail) {
                unsigned cur = queue[head++];
                unsigned cx = cur % grid_w;
                unsigned cy = cur / grid_w;
                static const int dx[4] = { 1, -1, 0, 0 };
                static const int dy[4] = { 0, 0, 1, -1 };

                cells++;
                if(cx < min_x) {
                    min_x = cx;
                }
                if(cx > max_x) {
                    max_x = cx;
                }
                if(cy < min_y) {
                    min_y = cy;
                }
                if(cy > max_y) {
                    max_y = cy;
                }

                for(unsigned n = 0; n < 4; n++) {
                    int nx = (int)cx + dx[n];
                    int ny = (int)cy + dy[n];
                    unsigned ni;

                    if(nx < 0 || ny < 0 || nx >= (int)grid_w ||
                       ny >= (int)grid_h) {
                        continue;
                    }
                    ni = (unsigned)ny * grid_w + (unsigned)nx;
                    if(mask[ni] && !seen[ni] && tail < CAMERA_FACE_GRID_MAX_CELLS) {
                        seen[ni] = 1;
                        queue[tail++] = (uint16_t)ni;
                    }
                }
            }

            box_x = min_x * step_x;
            box_y = min_y * step_y;
            box_w = (max_x + 1U) * step_x - box_x;
            box_h = (max_y + 1U) * step_y - box_y;
            if(box_x + box_w > width) {
                box_w = width - box_x;
            }
            if(box_y + box_h > height) {
                box_h = height - box_y;
            }

            if(cells < 10 || box_w < width / 12U || box_h < height / 14U ||
               box_w > width * 3U / 4U || box_h > height * 4U / 5U) {
                continue;
            }
            if((uint64_t)box_w * 100U < (uint64_t)box_h * 42U ||
               (uint64_t)box_w * 100U > (uint64_t)box_h * 185U) {
                continue;
            }

            pad_x = box_w / 5U;
            pad_y = box_h / 4U;
            if(pad_x > box_x) {
                pad_x = box_x;
            }
            if(pad_y > box_y) {
                pad_y = box_y;
            }
            box_x -= pad_x;
            box_y -= pad_y;
            box_w += pad_x * 2U;
            box_h += pad_y * 2U;
            if(box_x + box_w > width) {
                box_w = width - box_x;
            }
            if(box_y + box_h > height) {
                box_h = height - box_y;
            }

            candidate.x = (int)box_x;
            candidate.y = (int)box_y;
            candidate.w = (int)box_w;
            candidate.h = (int)box_h;
            candidate.score = (int)(cells * 1000U / (grid_w * grid_h));
            camera_insert_face_box(boxes, &face_count, &candidate);
        }
    }

    return face_count > max_boxes ? max_boxes : face_count;
}

static void camera_put_rgb565(uint8_t *buf, unsigned width, unsigned height,
                              int x, int y, uint16_t color)
{
    size_t off;

    if(!buf || x < 0 || y < 0 || x >= (int)width || y >= (int)height) {
        return;
    }
    off = ((size_t)y * width + (unsigned)x) * 2U;
    buf[off] = (uint8_t)(color & 0xFFU);
    buf[off + 1] = (uint8_t)(color >> 8);
}

static void camera_draw_face_rect(uint8_t *buf, unsigned width, unsigned height,
                                  const camera_face_box_t *box)
{
    uint16_t color = rgb565_from_hex(0x22D3EE);
    int x0;
    int y0;
    int x1;
    int y1;
    int thick = 4;
    int corner;

    if(!buf || !box || box->w <= 0 || box->h <= 0) {
        return;
    }

    x0 = box->x;
    y0 = box->y;
    x1 = box->x + box->w - 1;
    y1 = box->y + box->h - 1;
    if(x0 < 0) {
        x0 = 0;
    }
    if(y0 < 0) {
        y0 = 0;
    }
    if(x1 >= (int)width) {
        x1 = (int)width - 1;
    }
    if(y1 >= (int)height) {
        y1 = (int)height - 1;
    }
    if(x1 <= x0 || y1 <= y0) {
        return;
    }

    corner = box->w < box->h ? box->w / 3 : box->h / 3;
    if(corner < 18) {
        corner = 18;
    }
    if(corner > 72) {
        corner = 72;
    }

    for(int t = 0; t < thick; t++) {
        for(int x = x0; x <= x0 + corner && x <= x1; x++) {
            camera_put_rgb565(buf, width, height, x, y0 + t, color);
            camera_put_rgb565(buf, width, height, x, y1 - t, color);
        }
        for(int x = x1 - corner; x <= x1; x++) {
            camera_put_rgb565(buf, width, height, x, y0 + t, color);
            camera_put_rgb565(buf, width, height, x, y1 - t, color);
        }
        for(int y = y0; y <= y0 + corner && y <= y1; y++) {
            camera_put_rgb565(buf, width, height, x0 + t, y, color);
            camera_put_rgb565(buf, width, height, x1 - t, y, color);
        }
        for(int y = y1 - corner; y <= y1; y++) {
            camera_put_rgb565(buf, width, height, x0 + t, y, color);
            camera_put_rgb565(buf, width, height, x1 - t, y, color);
        }
    }
}

static int camera_detect_face_boxes_backend(const uint8_t *buf, unsigned width,
                                            unsigned height,
                                            camera_face_box_t *boxes,
                                            int max_boxes)
{
#if K230_CAMERA_FACE_KPU
    static int kpu_error_logged;
    int raw_boxes[CAMERA_FACE_MAX_BOXES * 5];
    int kpu_rc;

    kpu_rc = camera_face_detect_rgb565(buf, width, height, raw_boxes,
                                       max_boxes);
    if(kpu_rc >= 0) {
        if(kpu_rc > max_boxes) {
            kpu_rc = max_boxes;
        }
        memset(boxes, 0, (size_t)max_boxes * sizeof(boxes[0]));
        for(int i = 0; i < kpu_rc; i++) {
            boxes[i].x = raw_boxes[i * 5 + 0];
            boxes[i].y = raw_boxes[i * 5 + 1];
            boxes[i].w = raw_boxes[i * 5 + 2];
            boxes[i].h = raw_boxes[i * 5 + 3];
            boxes[i].score = raw_boxes[i * 5 + 4];
        }
        if(kpu_error_logged) {
            touch_trace_log("CAMERA_FACE_KPU_RECOVERED faces=%d", kpu_rc);
            kpu_error_logged = 0;
        }
        return kpu_rc;
    }
    if(!kpu_error_logged) {
        touch_trace_log("CAMERA_FACE_KPU_UNAVAILABLE rc=%d fallback=skin", kpu_rc);
        kpu_error_logged = 1;
    }
#endif

    return camera_detect_faces_rgb565(buf, width, height, boxes, max_boxes);
}

static int camera_overlay_cached_face_boxes(uint8_t *buf, unsigned width,
                                            unsigned height)
{
    camera_face_box_t boxes[CAMERA_FACE_MAX_BOXES];
    int count = 0;

    pthread_mutex_lock(&camera_face_lock);
    if(camera_face_result_w == width && camera_face_result_h == height) {
        count = camera_face_count;
        if(count > CAMERA_FACE_MAX_BOXES) {
            count = CAMERA_FACE_MAX_BOXES;
        }
        memcpy(boxes, camera_face_boxes, (size_t)count * sizeof(boxes[0]));
    }
    pthread_mutex_unlock(&camera_face_lock);

    for(int i = 0; i < count; i++) {
        camera_draw_face_rect(buf, width, height, &boxes[i]);
    }
    return count;
}

static void *camera_face_thread_cb(void *arg)
{
    uint8_t *local_buf;

    (void)arg;

    local_buf = malloc(CAMERA_PREVIEW_VIEW_BYTES);
    if(!local_buf) {
        touch_trace_log("CAMERA_FACE_WORKER_ALLOC_FAILED");
        return NULL;
    }

    while(1) {
        unsigned width;
        unsigned height;
        size_t bytes;
        camera_face_box_t boxes[CAMERA_FACE_MAX_BOXES];
        int count;

        pthread_mutex_lock(&camera_face_lock);
        while(!camera_face_stop && !camera_face_pending) {
            pthread_cond_wait(&camera_face_cond, &camera_face_lock);
        }
        if(camera_face_stop) {
            pthread_mutex_unlock(&camera_face_lock);
            break;
        }

        width = camera_face_request_w;
        height = camera_face_request_h;
        bytes = (size_t)width * height * 2U;
        if(bytes > CAMERA_PREVIEW_VIEW_BYTES) {
            camera_face_pending = 0;
            pthread_mutex_unlock(&camera_face_lock);
            continue;
        }
        memcpy(local_buf, camera_face_request_buf, bytes);
        camera_face_pending = 0;
        camera_face_busy = 1;
        pthread_mutex_unlock(&camera_face_lock);

        count = camera_detect_face_boxes_backend(local_buf, width, height,
                                                 boxes, CAMERA_FACE_MAX_BOXES);
        if(count < 0) {
            count = 0;
        }
        if(count > CAMERA_FACE_MAX_BOXES) {
            count = CAMERA_FACE_MAX_BOXES;
        }

        pthread_mutex_lock(&camera_face_lock);
        camera_face_count = count;
        camera_face_result_w = width;
        camera_face_result_h = height;
        memcpy(camera_face_boxes, boxes, (size_t)count * sizeof(boxes[0]));
        camera_face_busy = 0;
        pthread_mutex_unlock(&camera_face_lock);
    }

    free(local_buf);
    return NULL;
}

static int camera_face_worker_start(void)
{
    int rc;

    pthread_mutex_lock(&camera_face_lock);
    if(camera_face_thread_valid) {
        pthread_mutex_unlock(&camera_face_lock);
        return 0;
    }
    camera_face_stop = 0;
    camera_face_pending = 0;
    camera_face_busy = 0;
    camera_face_count = 0;
    camera_face_result_w = 0;
    camera_face_result_h = 0;
    camera_face_last_submit_us = 0;
    memset(camera_face_boxes, 0, sizeof(camera_face_boxes));
    if(!camera_face_request_buf) {
        camera_face_request_buf = malloc(CAMERA_PREVIEW_VIEW_BYTES);
    }
    if(!camera_face_request_buf) {
        pthread_mutex_unlock(&camera_face_lock);
        touch_trace_log("CAMERA_FACE_REQUEST_ALLOC_FAILED");
        return -1;
    }
    pthread_mutex_unlock(&camera_face_lock);

    rc = pthread_create(&camera_face_thread, NULL, camera_face_thread_cb, NULL);
    if(rc != 0) {
        pthread_mutex_lock(&camera_face_lock);
        free(camera_face_request_buf);
        camera_face_request_buf = NULL;
        pthread_mutex_unlock(&camera_face_lock);
        touch_trace_log("CAMERA_FACE_WORKER_START_FAILED rc=%d", rc);
        return -1;
    }

    pthread_mutex_lock(&camera_face_lock);
    camera_face_thread_valid = 1;
    pthread_mutex_unlock(&camera_face_lock);
    return 0;
}

static void camera_face_worker_stop(void)
{
    pthread_t thread;
    int join_thread = 0;

    pthread_mutex_lock(&camera_face_lock);
    if(camera_face_thread_valid) {
        thread = camera_face_thread;
        camera_face_thread_valid = 0;
        camera_face_stop = 1;
        pthread_cond_signal(&camera_face_cond);
        join_thread = 1;
    }
    pthread_mutex_unlock(&camera_face_lock);

    if(join_thread) {
        pthread_join(thread, NULL);
    }

    pthread_mutex_lock(&camera_face_lock);
    free(camera_face_request_buf);
    camera_face_request_buf = NULL;
    camera_face_pending = 0;
    camera_face_busy = 0;
    camera_face_stop = 0;
    camera_face_count = 0;
    camera_face_result_w = 0;
    camera_face_result_h = 0;
    camera_face_last_submit_us = 0;
    memset(camera_face_boxes, 0, sizeof(camera_face_boxes));
    pthread_mutex_unlock(&camera_face_lock);
}

static void camera_face_submit_frame(const uint8_t *buf, unsigned width,
                                     unsigned height)
{
    uint64_t now;
    size_t bytes;

    if(!buf || width == 0 || height == 0) {
        return;
    }
    bytes = (size_t)width * height * 2U;
    if(bytes > CAMERA_PREVIEW_VIEW_BYTES) {
        return;
    }

    now = monotonic_us();
    pthread_mutex_lock(&camera_face_lock);
    if(!camera_face_request_buf || camera_face_pending || camera_face_busy ||
       (camera_face_last_submit_us &&
        now - camera_face_last_submit_us < CAMERA_FACE_DETECT_INTERVAL_US)) {
        pthread_mutex_unlock(&camera_face_lock);
        return;
    }
    memcpy(camera_face_request_buf, buf, bytes);
    camera_face_request_w = width;
    camera_face_request_h = height;
    camera_face_pending = 1;
    camera_face_last_submit_us = now;
    pthread_cond_signal(&camera_face_cond);
    pthread_mutex_unlock(&camera_face_lock);
}

static int camera_ensure_photo_dir(void)
{
    struct stat st;

    if(stat(CAMERA_PHOTO_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if(mkdir(CAMERA_PHOTO_DIR, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int shell_exit_code(int rc)
{
    if(rc == -1) {
        return 127;
    }
    if(WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
    if(WIFSIGNALED(rc)) {
        return 128 + WTERMSIG(rc);
    }
    return 126;
}

static void network_set_status_locked(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(network_status_text, sizeof(network_status_text), fmt, ap);
    va_end(ap);
    network_result_ready = 1;
}

static void wifi_copy_field(char *dst, size_t len, const char *src)
{
    if(!dst || len == 0) {
        return;
    }

    if(!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, len, "%s", src);
    trim_text(dst);
}

static void wifi_scan_store_ap(wifi_ap_info_t *list, int *count,
                               const wifi_ap_info_t *ap)
{
    if(!list || !count || !ap || *count >= NET_MAX_APS || !ap->ssid[0]) {
        return;
    }

    list[*count] = *ap;
    if(!list[*count].security[0]) {
        snprintf(list[*count].security, sizeof(list[*count].security),
                 ap->encrypted ? "Secured" : "Open");
    }
    (*count)++;
}

static int wifi_scan_collect(wifi_ap_info_t *list, int *count, char *err,
                             size_t err_len)
{
    FILE *fp;
    char line[256];
    wifi_ap_info_t cur;
    int have_cell = 0;
    int rc;

    if(!list || !count) {
        return -1;
    }

    *count = 0;
    memset(&cur, 0, sizeof(cur));

    rc = system("(modprobe 8189fs || modprobe 8733bs) >/tmp/k230_wifi_modprobe.log 2>&1 || true");
    (void)rc;
    if(!path_exists("/sys/class/net/" NET_WIFI_IFACE)) {
        if(err && err_len > 0) {
            snprintf(err, err_len, NET_WIFI_IFACE " missing");
        }
        return -1;
    }

    fp = popen("ifconfig " NET_WIFI_IFACE " up >/dev/null 2>&1; "
               "iwlist " NET_WIFI_IFACE " scan 2>" NET_WIFI_SCAN_LOG, "r");
    if(!fp) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "scan command failed");
        }
        return -1;
    }

    while(fgets(line, sizeof(line), fp)) {
        char *p;

        if(strstr(line, "Cell ") != NULL && strstr(line, "Address:") != NULL) {
            if(have_cell) {
                wifi_scan_store_ap(list, count, &cur);
            }
            memset(&cur, 0, sizeof(cur));
            snprintf(cur.security, sizeof(cur.security), "Open");
            have_cell = 1;
            continue;
        }

        p = strstr(line, "ESSID:");
        if(p) {
            p = strchr(p, '"');
            if(p) {
                char *end;

                p++;
                end = strchr(p, '"');
                if(end) {
                    *end = '\0';
                }
                wifi_copy_field(cur.ssid, sizeof(cur.ssid), p);
            }
            continue;
        }

        p = strstr(line, "Quality=");
        if(p) {
            char *end;

            p += strlen("Quality=");
            end = p;
            while(*end && !isspace((unsigned char)*end)) {
                end++;
            }
            *end = '\0';
            wifi_copy_field(cur.quality, sizeof(cur.quality), p);
        }

        p = strstr(line, "Signal level=");
        if(p) {
            char *end;

            p += strlen("Signal level=");
            end = p;
            while(*end && *end != '\r' && *end != '\n') {
                end++;
            }
            *end = '\0';
            wifi_copy_field(cur.signal, sizeof(cur.signal), p);
        }

        p = strstr(line, "Encryption key:");
        if(p) {
            cur.encrypted = strstr(p, "on") != NULL;
            snprintf(cur.security, sizeof(cur.security), "%s",
                     cur.encrypted ? "Secured" : "Open");
        }

        if(strstr(line, "WPA3")) {
            snprintf(cur.security, sizeof(cur.security), "WPA3");
            cur.encrypted = 1;
        } else if(strstr(line, "WPA2")) {
            snprintf(cur.security, sizeof(cur.security), "WPA2");
            cur.encrypted = 1;
        } else if(strstr(line, "WPA")) {
            snprintf(cur.security, sizeof(cur.security), "WPA");
            cur.encrypted = 1;
        }
    }

    if(have_cell) {
        wifi_scan_store_ap(list, count, &cur);
    }

    rc = shell_exit_code(pclose(fp));
    if(rc != 0 && *count == 0) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "scan failed, rc=%d", rc);
        }
        return -1;
    }

    return 0;
}

static void *wifi_scan_thread_cb(void *arg)
{
    wifi_ap_info_t list[NET_MAX_APS];
    int count = 0;
    char err[96] = "";
    int rc;

    (void)arg;

    rc = wifi_scan_collect(list, &count, err, sizeof(err));

    pthread_mutex_lock(&network_lock);
    if(rc == 0) {
        memcpy(network_aps, list, sizeof(network_aps));
        network_ap_count = count;
        network_selected_ap = count > 0 ? 0 : -1;
        if(network_selected_ap >= 0) {
            snprintf(network_selected_ssid, sizeof(network_selected_ssid), "%s",
                     network_aps[network_selected_ap].ssid);
        } else {
            network_selected_ssid[0] = '\0';
        }
        network_set_status_locked(count > 0 ? "Scan complete: %d networks" :
                                  "Scan complete: no networks", count);
    } else {
        network_ap_count = 0;
        network_selected_ap = -1;
        network_selected_ssid[0] = '\0';
        network_set_status_locked("%s", err[0] ? err : "WiFi scan failed");
    }
    network_scan_busy = 0;
    pthread_mutex_unlock(&network_lock);

    return NULL;
}

static void wifi_write_quoted(FILE *fp, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    fputc('"', fp);
    while(p && *p) {
        if(*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if(*p >= 32 && *p < 127) {
            fputc(*p, fp);
        }
        p++;
    }
    fputc('"', fp);
}

static int wifi_write_config(const char *ssid, const char *password)
{
    FILE *fp;

    fp = fopen(NET_WIFI_CONF, "w");
    if(!fp) {
        return -1;
    }

    fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(fp, "ap_scan=1\n\n");
    fprintf(fp, "network={\n");
    fprintf(fp, "    ssid=");
    wifi_write_quoted(fp, ssid);
    fprintf(fp, "\n");
    fprintf(fp, "    scan_ssid=1\n");
    if(password && password[0]) {
        fprintf(fp, "    psk=");
        wifi_write_quoted(fp, password);
        fprintf(fp, "\n");
    } else {
        fprintf(fp, "    key_mgmt=NONE\n");
    }
    fprintf(fp, "}\n");
    fclose(fp);
    chmod(NET_WIFI_CONF, 0600);
    return 0;
}

static void *wifi_connect_thread_cb(void *arg)
{
    wifi_connect_request_t *req = (wifi_connect_request_t *)arg;
    char ip[64];
    int rc;

    if(!req) {
        return NULL;
    }

    if(!path_exists("/sys/class/net/" NET_WIFI_IFACE)) {
        pthread_mutex_lock(&network_lock);
        network_connect_busy = 0;
        network_set_status_locked(NET_WIFI_IFACE " missing");
        pthread_mutex_unlock(&network_lock);
        free(req);
        return NULL;
    }

    if(req->password[0] && strlen(req->password) < 8U) {
        pthread_mutex_lock(&network_lock);
        network_connect_busy = 0;
        network_set_status_locked("Password must be at least 8 chars");
        pthread_mutex_unlock(&network_lock);
        free(req);
        return NULL;
    }

    if(wifi_write_config(req->ssid, req->password) != 0) {
        pthread_mutex_lock(&network_lock);
        network_connect_busy = 0;
        network_set_status_locked("Failed to write WiFi config");
        pthread_mutex_unlock(&network_lock);
        free(req);
        return NULL;
    }

    rc = system("mkdir -p /var/run/wpa_supplicant; "
                "wpa_cli -i " NET_WIFI_IFACE " terminate >/dev/null 2>&1 || true; "
                "ifconfig " NET_WIFI_IFACE " up >" NET_WIFI_CONNECT_LOG " 2>&1 && "
                "wpa_supplicant -B -i " NET_WIFI_IFACE " -D nl80211,wext -c "
                NET_WIFI_CONF " >>" NET_WIFI_CONNECT_LOG " 2>&1 && "
                "sleep 3 && "
                "udhcpc -q -n -t 5 -p /var/run/udhcpc." NET_WIFI_IFACE ".pid -i "
                NET_WIFI_IFACE " >>" NET_WIFI_CONNECT_LOG " 2>&1");
    rc = shell_exit_code(rc);

    pthread_mutex_lock(&network_lock);
    network_connect_busy = 0;
    if(read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
        network_set_status_locked("Connected to %s  %s", req->ssid, ip);
    } else {
        network_set_status_locked("Connect failed, rc=%d", rc);
    }
    pthread_mutex_unlock(&network_lock);

    free(req);
    return NULL;
}

static int camera_read_stored_thumb(const char *path)
{
    FILE *fp;
    size_t total = 0;

    if(!path || !path[0]) {
        return -1;
    }

    fp = fopen(path, "rb");
    if(!fp) {
        return -1;
    }

    while(total < CAMERA_STORED_THUMB_BYTES) {
        size_t n = fread(camera_stored_thumb_buf + total, 1,
                         CAMERA_STORED_THUMB_BYTES - total, fp);
        if(n == 0) {
            break;
        }
        total += n;
    }

    fclose(fp);
    return total == CAMERA_STORED_THUMB_BYTES ? 0 : -1;
}

static int camera_path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if(!path || !suffix) {
        return 0;
    }

    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if(path_len < suffix_len) {
        return 0;
    }

    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static void camera_fill_rgb565(uint8_t *dst, unsigned width, unsigned height,
                               uint32_t color)
{
    uint16_t rgb565 = rgb565_from_hex(color);

    for(unsigned y = 0; y < height; y++) {
        for(unsigned x = 0; x < width; x++) {
            size_t off = ((size_t)y * width + (size_t)x) * 2U;

            dst[off + 0] = (uint8_t)(rgb565 & 0xFFU);
            dst[off + 1] = (uint8_t)(rgb565 >> 8);
        }
    }
}

static void camera_write_rgb565(uint8_t *dst, unsigned dst_w,
                                unsigned x, unsigned y,
                                unsigned r, unsigned g, unsigned b)
{
    uint16_t rgb565 = (uint16_t)(((r & 0xF8U) << 8) |
                                 ((g & 0xFCU) << 3) |
                                 (b >> 3));
    size_t off = ((size_t)y * dst_w + (size_t)x) * 2U;

    dst[off + 0] = (uint8_t)(rgb565 & 0xFFU);
    dst[off + 1] = (uint8_t)(rgb565 >> 8);
}

static void camera_scale_stored_thumb_cover(uint8_t *dst, unsigned dst_w,
                                            unsigned dst_h)
{
    for(unsigned y = 0; y < dst_h; y++) {
        unsigned src_y = (unsigned)(((uint64_t)y * CAMERA_STORED_THUMB_H) / dst_h);

        if(src_y >= CAMERA_STORED_THUMB_H) {
            src_y = CAMERA_STORED_THUMB_H - 1U;
        }

        for(unsigned x = 0; x < dst_w; x++) {
            unsigned src_x = (unsigned)(((uint64_t)x * CAMERA_STORED_THUMB_W) / dst_w);
            size_t dst_off = ((size_t)y * dst_w + (size_t)x) * 2U;
            size_t src_off;

            if(src_x >= CAMERA_STORED_THUMB_W) {
                src_x = CAMERA_STORED_THUMB_W - 1U;
            }

            src_off = ((size_t)src_y * CAMERA_STORED_THUMB_W + (size_t)src_x) * 2U;
            dst[dst_off + 0] = camera_stored_thumb_buf[src_off + 0];
            dst[dst_off + 1] = camera_stored_thumb_buf[src_off + 1];
        }
    }
}

static void camera_scale_stored_thumb_contain(uint8_t *dst, unsigned dst_w,
                                              unsigned dst_h)
{
    unsigned draw_w = dst_w;
    unsigned draw_h = dst_h;
    unsigned offset_x;
    unsigned offset_y;

    if(dst_w == 0 || dst_h == 0) {
        return;
    }

    camera_fill_rgb565(dst, dst_w, dst_h, 0x05070A);

    if((uint64_t)CAMERA_STORED_THUMB_W * dst_h >
       (uint64_t)CAMERA_STORED_THUMB_H * dst_w) {
        draw_h = (unsigned)(((uint64_t)CAMERA_STORED_THUMB_H * dst_w) /
                            CAMERA_STORED_THUMB_W);
        if(draw_h < 1U) {
            draw_h = 1U;
        }
    } else {
        draw_w = (unsigned)(((uint64_t)CAMERA_STORED_THUMB_W * dst_h) /
                            CAMERA_STORED_THUMB_H);
        if(draw_w < 1U) {
            draw_w = 1U;
        }
    }

    offset_x = (dst_w - draw_w) / 2U;
    offset_y = (dst_h - draw_h) / 2U;

    for(unsigned y = 0; y < draw_h; y++) {
        unsigned src_y = (unsigned)(((uint64_t)y * CAMERA_STORED_THUMB_H) /
                                    draw_h);

        if(src_y >= CAMERA_STORED_THUMB_H) {
            src_y = CAMERA_STORED_THUMB_H - 1U;
        }

        for(unsigned x = 0; x < draw_w; x++) {
            unsigned src_x = (unsigned)(((uint64_t)x * CAMERA_STORED_THUMB_W) /
                                        draw_w);
            size_t dst_off = ((size_t)(offset_y + y) * dst_w +
                              (size_t)(offset_x + x)) * 2U;
            size_t src_off;

            if(src_x >= CAMERA_STORED_THUMB_W) {
                src_x = CAMERA_STORED_THUMB_W - 1U;
            }

            src_off = ((size_t)src_y * CAMERA_STORED_THUMB_W +
                       (size_t)src_x) * 2U;
            dst[dst_off + 0] = camera_stored_thumb_buf[src_off + 0];
            dst[dst_off + 1] = camera_stored_thumb_buf[src_off + 1];
        }
    }
}

static int camera_decode_png_to_rgb565(const char *path, uint8_t *dst,
                                       unsigned dst_w, unsigned dst_h,
                                       int contain)
{
    FILE *fp = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep image = NULL;
    png_bytep *rows = NULL;
    png_uint_32 png_w = 0;
    png_uint_32 png_h = 0;
    png_size_t rowbytes;
    int color_type;
    int bit_depth;
    unsigned src_w;
    unsigned src_h;
    int rc = -1;

    if(!path || !dst || dst_w == 0 || dst_h == 0) {
        return -1;
    }

    fp = fopen(path, "rb");
    if(!fp) {
        return -1;
    }

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if(!png) {
        goto out;
    }

    info = png_create_info_struct(png);
    if(!info) {
        goto out;
    }

    if(setjmp(png_jmpbuf(png))) {
        goto out;
    }

    png_init_io(png, fp);
    png_read_info(png, info);
    png_get_IHDR(png, info, &png_w, &png_h, &bit_depth, &color_type,
                 NULL, NULL, NULL);

    if(png_w == 0 || png_h == 0 || png_w > 8192U || png_h > 8192U) {
        goto out;
    }

    if(bit_depth == 16) {
        png_set_strip_16(png);
    }
    if(color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if(png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if(color_type == PNG_COLOR_TYPE_GRAY ||
       color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if(!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);
    rowbytes = png_get_rowbytes(png, info);
    src_w = (unsigned)png_get_image_width(png, info);
    src_h = (unsigned)png_get_image_height(png, info);
    if(src_w == 0 || src_h == 0 || rowbytes < (png_size_t)src_w * 4U) {
        goto out;
    }

    image = malloc((size_t)rowbytes * src_h);
    rows = malloc(sizeof(*rows) * src_h);
    if(!image || !rows) {
        goto out;
    }
    for(unsigned y = 0; y < src_h; y++) {
        rows[y] = image + (size_t)y * rowbytes;
    }
    png_read_image(png, rows);

    if(contain) {
        unsigned draw_w = dst_w;
        unsigned draw_h = dst_h;
        unsigned offset_x;
        unsigned offset_y;

        camera_fill_rgb565(dst, dst_w, dst_h, 0x05070A);

        if((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w) {
            draw_h = (unsigned)(((uint64_t)src_h * dst_w) / src_w);
            if(draw_h < 1U) {
                draw_h = 1U;
            }
        } else {
            draw_w = (unsigned)(((uint64_t)src_w * dst_h) / src_h);
            if(draw_w < 1U) {
                draw_w = 1U;
            }
        }

        offset_x = (dst_w - draw_w) / 2U;
        offset_y = (dst_h - draw_h) / 2U;

        for(unsigned y = 0; y < draw_h; y++) {
            unsigned src_y = (unsigned)(((uint64_t)y * src_h) / draw_h);

            if(src_y >= src_h) {
                src_y = src_h - 1U;
            }

            for(unsigned x = 0; x < draw_w; x++) {
                unsigned src_x = (unsigned)(((uint64_t)x * src_w) / draw_w);
                const unsigned char *p;
                unsigned a;
                unsigned r;
                unsigned g;
                unsigned b;

                if(src_x >= src_w) {
                    src_x = src_w - 1U;
                }

                p = image + (size_t)src_y * rowbytes + (size_t)src_x * 4U;
                a = p[3];
                r = ((unsigned)p[0] * a) / 255U;
                g = ((unsigned)p[1] * a) / 255U;
                b = ((unsigned)p[2] * a) / 255U;
                camera_write_rgb565(dst, dst_w, offset_x + x, offset_y + y,
                                    r, g, b);
            }
        }
    } else {
        unsigned src_x0 = 0;
        unsigned src_y0 = 0;
        unsigned visible_w = src_w;
        unsigned visible_h = src_h;

        if((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w) {
            visible_w = (unsigned)(((uint64_t)src_h * dst_w) / dst_h);
            if(visible_w < 1U) {
                visible_w = 1U;
            }
            if(visible_w > src_w) {
                visible_w = src_w;
            }
            src_x0 = (src_w - visible_w) / 2U;
        } else {
            visible_h = (unsigned)(((uint64_t)src_w * dst_h) / dst_w);
            if(visible_h < 1U) {
                visible_h = 1U;
            }
            if(visible_h > src_h) {
                visible_h = src_h;
            }
            src_y0 = (src_h - visible_h) / 2U;
        }

        for(unsigned y = 0; y < dst_h; y++) {
            unsigned src_y = src_y0 +
                             (unsigned)(((uint64_t)y * visible_h) / dst_h);

            if(src_y >= src_h) {
                src_y = src_h - 1U;
            }

            for(unsigned x = 0; x < dst_w; x++) {
                unsigned src_x = src_x0 +
                                 (unsigned)(((uint64_t)x * visible_w) / dst_w);
                const unsigned char *p;
                unsigned a;
                unsigned r;
                unsigned g;
                unsigned b;

                if(src_x >= src_w) {
                    src_x = src_w - 1U;
                }

                p = image + (size_t)src_y * rowbytes + (size_t)src_x * 4U;
                a = p[3];
                r = ((unsigned)p[0] * a) / 255U;
                g = ((unsigned)p[1] * a) / 255U;
                b = ((unsigned)p[2] * a) / 255U;
                camera_write_rgb565(dst, dst_w, x, y, r, g, b);
            }
        }
    }

    rc = 0;

out:
    free(rows);
    free(image);
    if(png || info) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    }
    if(fp) {
        fclose(fp);
    }
    return rc;
}

static void camera_apply_canvas(lv_obj_t *canvas, lv_obj_t *hint,
                                const char *thumb_path, uint8_t *buf,
                                unsigned width, unsigned height,
                                int contain)
{
    int has_image;
    int is_png;

    if(!canvas) {
        return;
    }

    is_png = camera_path_has_suffix(thumb_path, ".png");
    has_image = !is_png && camera_read_stored_thumb(thumb_path) == 0;
    if(has_image && contain) {
        camera_scale_stored_thumb_contain(buf, width, height);
    } else if(has_image) {
        camera_scale_stored_thumb_cover(buf, width, height);
    } else if(is_png && camera_decode_png_to_rgb565(thumb_path, buf, width,
                                                    height, contain) == 0) {
        has_image = 1;
    } else {
        camera_fill_placeholder(buf, width, height);
    }

    lv_canvas_set_buffer(canvas, buf, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);

    if(hint) {
        if(has_image) {
            lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void camera_apply_preview_placeholder(lv_obj_t *canvas, lv_obj_t *hint,
                                             uint8_t *buf, unsigned width,
                                             unsigned height)
{
    if(!canvas || !buf) {
        return;
    }

    camera_fill_placeholder(buf, width, height);
    lv_canvas_set_buffer(canvas, buf, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
    if(hint) {
        lv_label_set_text(hint, "Starting live preview");
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static unsigned camera_preview_view_width(void)
{
    int w;

    if(!display_orientation_is_landscape()) {
        return CAMERA_PREVIEW_VIEW_PORTRAIT_W;
    }

    w = display_logical_width() - 24 - 260 - 24 - 20;
    if(w < CAMERA_PREVIEW_VIEW_PORTRAIT_W) {
        w = CAMERA_PREVIEW_VIEW_PORTRAIT_W;
    }
    if(w > CAMERA_PREVIEW_VIEW_MAX_W) {
        w = CAMERA_PREVIEW_VIEW_MAX_W;
    }
    return (unsigned)w;
}

static unsigned camera_preview_view_height(void)
{
    int h;

    if(!display_orientation_is_landscape()) {
        return CAMERA_PREVIEW_VIEW_PORTRAIT_H;
    }

    h = display_logical_height() - page_content_top_y(144);
    if(h < 260) {
        h = 260;
    }
    if(h > CAMERA_PREVIEW_VIEW_MAX_H) {
        h = CAMERA_PREVIEW_VIEW_MAX_H;
    }
    return (unsigned)h;
}

static unsigned camera_gallery_view_width(void)
{
    int w = display_logical_width();

    if(w < 1) {
        w = 1;
    }
    if(w > CAMERA_GALLERY_VIEW_MAX_W) {
        w = CAMERA_GALLERY_VIEW_MAX_W;
    }
    return (unsigned)w;
}

static unsigned camera_gallery_view_height(void)
{
    int h = display_logical_height() - 54;

    if(h < 1) {
        h = 1;
    }
    if(h > CAMERA_GALLERY_VIEW_MAX_H) {
        h = CAMERA_GALLERY_VIEW_MAX_H;
    }
    return (unsigned)h;
}

static int camera_find_latest_capture(char *photo, size_t photo_len,
                                      char *thumb, size_t thumb_len)
{
    DIR *dir = opendir(CAMERA_PHOTO_DIR);
    struct dirent *ent;
    time_t best_time = 0;
    char best_photo[192] = "";
    char best_thumb[192] = "";

    if(!dir) {
        return -1;
    }

    while((ent = readdir(dir)) != NULL) {
        char path[192];
        char thumb_path[192];
        struct stat st;
        size_t len = strlen(ent->d_name);

        if(len < 5 || strcmp(ent->d_name + len - 4, ".ppm") != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", CAMERA_PHOTO_DIR, ent->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        snprintf(thumb_path, sizeof(thumb_path), "%s/%.*s.thumb.rgb565",
                 CAMERA_PHOTO_DIR, (int)(len - 4), ent->d_name);

        if(st.st_mtime >= best_time) {
            best_time = st.st_mtime;
            snprintf(best_photo, sizeof(best_photo), "%s", path);
            snprintf(best_thumb, sizeof(best_thumb), "%s", thumb_path);
        }
    }

    closedir(dir);

    if(!best_photo[0]) {
        return -1;
    }

    snprintf(photo, photo_len, "%s", best_photo);
    snprintf(thumb, thumb_len, "%s", best_thumb);
    return 0;
}

static void camera_gallery_insert_item(const char *photo, const char *thumb,
                                       time_t mtime)
{
    int insert_at = camera_gallery_count;

    if(!photo || !photo[0] || !thumb || !thumb[0]) {
        return;
    }

    while(insert_at > 0 &&
          camera_gallery_items[insert_at - 1].mtime < mtime) {
        if(insert_at < CAMERA_GALLERY_MAX_ITEMS) {
            camera_gallery_items[insert_at] =
                camera_gallery_items[insert_at - 1];
        }
        insert_at--;
    }

    if(insert_at >= CAMERA_GALLERY_MAX_ITEMS) {
        return;
    }

    snprintf(camera_gallery_items[insert_at].photo,
             sizeof(camera_gallery_items[insert_at].photo), "%s", photo);
    snprintf(camera_gallery_items[insert_at].thumb,
             sizeof(camera_gallery_items[insert_at].thumb), "%s", thumb);
    camera_gallery_items[insert_at].mtime = mtime;
    if(camera_gallery_count < CAMERA_GALLERY_MAX_ITEMS) {
        camera_gallery_count++;
    }
}

static void camera_gallery_scan_dir(const char *dir_path, int screenshot_dir)
{
    DIR *dir = opendir(dir_path);
    struct dirent *ent;

    if(!dir) {
        return;
    }

    while((ent = readdir(dir)) != NULL) {
        char path[192];
        char thumb_path[192];
        struct stat st;
        size_t len;

        len = strlen(ent->d_name);
        if(screenshot_dir) {
            if(len < 5 || strcmp(ent->d_name + len - 4, ".png") != 0) {
                continue;
            }
        } else if(len < 5 || strcmp(ent->d_name + len - 4, ".ppm") != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if(screenshot_dir) {
            snprintf(thumb_path, sizeof(thumb_path), "%s", path);
        } else {
            snprintf(thumb_path, sizeof(thumb_path), "%s/%.*s.thumb.rgb565",
                     dir_path, (int)(len - 4), ent->d_name);
        }

        camera_gallery_insert_item(path, thumb_path, st.st_mtime);
    }

    closedir(dir);
}

static void camera_gallery_scan(void)
{
    camera_gallery_count = 0;
    camera_gallery_scan_dir(CAMERA_PHOTO_DIR, 0);
    camera_gallery_scan_dir(SCREENSHOT_DIR, 1);

    if(camera_gallery_count == 0) {
        camera_gallery_selected = 0;
    } else if(camera_gallery_selected >= camera_gallery_count) {
        camera_gallery_selected = camera_gallery_count - 1;
    } else if(camera_gallery_selected < 0) {
        camera_gallery_selected = 0;
    }
}

static void camera_gallery_select(int index)
{
    if(index < 0 || index >= camera_gallery_count) {
        return;
    }

    camera_gallery_selected = index;
    pthread_mutex_lock(&camera_lock);
    snprintf(camera_last_photo_path, sizeof(camera_last_photo_path), "%s",
             camera_gallery_items[index].photo);
    snprintf(camera_last_thumb_path, sizeof(camera_last_thumb_path), "%s",
             camera_gallery_items[index].thumb);
    pthread_mutex_unlock(&camera_lock);
}

static void camera_refresh_latest_from_disk(void)
{
    char photo[192];
    char thumb[192];

    if(camera_find_latest_capture(photo, sizeof(photo), thumb, sizeof(thumb)) == 0) {
        pthread_mutex_lock(&camera_lock);
        snprintf(camera_last_photo_path, sizeof(camera_last_photo_path), "%s", photo);
        snprintf(camera_last_thumb_path, sizeof(camera_last_thumb_path), "%s", thumb);
        pthread_mutex_unlock(&camera_lock);
    }
}

static void camera_set_status(const char *text, int result)
{
    pthread_mutex_lock(&camera_lock);
    snprintf(camera_status_text, sizeof(camera_status_text), "%s", text);
    camera_last_result = result;
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);
}

static void camera_set_preview_status(const char *text, int result, int active)
{
    pthread_mutex_lock(&camera_lock);
    snprintf(camera_preview_status_text, sizeof(camera_preview_status_text), "%s", text);
    camera_preview_last_result = result;
    camera_preview_active = active;
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);
}

static void *camera_preview_thread_cb(void *arg)
{
    struct v4l2_drm_context ctx;
    uint8_t *local_buf;
    int started = 0;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint64_t last_frame_us = 0;
    uint32_t last_layout_key = 0;

    (void)arg;

    local_buf = malloc(CAMERA_PREVIEW_VIEW_BYTES);
    if(!local_buf) {
        camera_set_preview_status("Preview buffer allocation failed", 1, 0);
        return NULL;
    }
    memset(&ctx, 0, sizeof(ctx));
    v4l2_drm_default_context(&ctx);
    ctx.device = CAMERA_CAPTURE_DEVICE;
    ctx.width = CAMERA_PREVIEW_W;
    ctx.height = CAMERA_PREVIEW_H;
    ctx.video_format = V4L2_PIX_FMT_NV16;
    ctx.display = false;
    ctx.buffer_num = 4;

    if(v4l2_drm_setup(&ctx, 1, NULL) != 0) {
        camera_set_preview_status("Preview setup failed", 1, 0);
        free(local_buf);
        return NULL;
    }

    if(v4l2_drm_start(&ctx) != 0) {
        camera_set_preview_status("Preview stream failed", 1, 0);
        v4l2_drm_stop(&ctx);
        free(local_buf);
        return NULL;
    }

    started = 1;
    camera_face_worker_start();
    camera_set_preview_status("Live preview starting", 0, 1);

    while(!camera_preview_stop) {
        int ret;
        uint64_t now;
        unsigned view_w = camera_preview_view_width();
        unsigned view_h = camera_preview_view_height();
        size_t view_bytes = (size_t)view_w * view_h * 2U;

        memset(&ctx.vbuffer, 0, sizeof(ctx.vbuffer));
        ctx.vbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ctx.vbuffer.memory = V4L2_MEMORY_MMAP;

        ret = v4l2_drm_dump(&ctx, 250);
        if(ret < 0) {
            if(errno == EAGAIN || errno == EINTR) {
                continue;
            }
            camera_set_preview_status("Preview frame error", errno, 0);
            break;
        }
        if(ctx.vbuffer.length == 0 && ctx.vbuffer.bytesused == 0) {
            continue;
        }

        if(ctx.vbuffer.index < ctx.buffer_num && ctx.buffers[ctx.vbuffer.index].mmap) {
            int landscape = display_orientation_is_landscape();
            int rotate_preview = !landscape;
            int flip_h = camera_preview_flip_h ? 1 : 0;
            int flip_v = camera_preview_flip_v ? 1 : 0;
            int detected_faces;
            uint32_t layout_key = ((uint32_t)view_w << 16) ^
                                  ((uint32_t)view_h << 1) ^
                                  (uint32_t)(rotate_preview ? 1 : 0) ^
                                  (uint32_t)(flip_h ? 0x10000000U : 0U) ^
                                  (uint32_t)(flip_v ? 0x20000000U : 0U) ^
                                  (uint32_t)(landscape ? 0x40000000U : 0U);

            if(landscape) {
                flip_v = !flip_v;
            }

            if(layout_key != last_layout_key) {
                touch_trace_log("CAMERA_PREVIEW_LAYOUT src=%ux%u view=%ux%u "
                                "mode=%s crop=cover flip_h=%d flip_v=%d",
                                CAMERA_PREVIEW_W, CAMERA_PREVIEW_H,
                                view_w, view_h,
                                rotate_preview ? "rotate90" : "native",
                                flip_h, flip_v);
                last_layout_key = layout_key;
            }
            camera_convert_yuv_to_rgb565_preview(
                (const uint8_t *)ctx.buffers[ctx.vbuffer.index].mmap,
                CAMERA_PREVIEW_W, CAMERA_PREVIEW_H, 0,
                local_buf, view_w, view_h, rotate_preview, flip_h, flip_v);
            camera_face_submit_frame(local_buf, view_w, view_h);
            detected_faces = camera_overlay_cached_face_boxes(local_buf, view_w,
                                                              view_h);

            pthread_mutex_lock(&camera_lock);
            memcpy(camera_preview_frame_buf, local_buf, view_bytes);
            camera_preview_frame_ready = 1;
            camera_preview_have_frame = 1;
            camera_preview_active = 1;
            camera_preview_last_result = 0;
            camera_preview_frame_count++;
            snprintf(camera_preview_status_text, sizeof(camera_preview_status_text),
                     "Live preview  %ux%u  Faces %d", view_w, view_h,
                     detected_faces);
            pthread_mutex_unlock(&camera_lock);
        }

        v4l2_drm_dump_release(&ctx);

        now = monotonic_us();
        if(last_frame_us && now - last_frame_us < CAMERA_PREVIEW_INTERVAL_US) {
            usleep((useconds_t)(CAMERA_PREVIEW_INTERVAL_US - (now - last_frame_us)));
        }
        last_frame_us = monotonic_us();
    }

    if(started) {
        ioctl(ctx.video_fd, VIDIOC_STREAMOFF, &type);
    }
    v4l2_drm_stop(&ctx);
    camera_face_worker_stop();
    free(local_buf);

    pthread_mutex_lock(&camera_lock);
    camera_preview_active = 0;
    if(camera_preview_stop) {
        snprintf(camera_preview_status_text, sizeof(camera_preview_status_text),
                 "Preview stopped");
    }
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);

    return NULL;
}

static void camera_stop_preview(void)
{
    pthread_t thread;
    int join_thread = 0;

    pthread_mutex_lock(&camera_preview_thread_lock);
    if(camera_preview_thread_valid) {
        camera_preview_stop = 1;
        thread = camera_preview_thread;
        camera_preview_thread_valid = 0;
        join_thread = 1;
    }
    pthread_mutex_unlock(&camera_preview_thread_lock);

    if(join_thread) {
        pthread_join(thread, NULL);
    }
}

static void camera_start_preview(void)
{
    pthread_mutex_lock(&camera_preview_thread_lock);
    if(camera_preview_thread_valid) {
        pthread_mutex_unlock(&camera_preview_thread_lock);
        return;
    }

    camera_preview_stop = 0;
    pthread_mutex_lock(&camera_lock);
    camera_preview_frame_ready = 0;
    camera_preview_have_frame = 0;
    camera_preview_active = 0;
    camera_preview_last_result = -1;
    camera_preview_frame_count = 0;
    snprintf(camera_preview_status_text, sizeof(camera_preview_status_text),
             "Starting live preview");
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);

    if(pthread_create(&camera_preview_thread, NULL, camera_preview_thread_cb, NULL) == 0) {
        camera_preview_thread_valid = 1;
        touch_trace_log("CAMERA_PREVIEW_START");
    } else {
        camera_set_preview_status("Preview thread failed", 1, 0);
    }

    pthread_mutex_unlock(&camera_preview_thread_lock);
}

static void camera_update_visible_state(void)
{
    char status[128];
    char preview_status[128];
    char photo[192];
    char thumb[192];
    int result;
    int busy;
    int preview_active;
    int preview_have_frame;
    int preview_frame_ready;
    int preview_result;
    const char *base;
    unsigned preview_w = camera_preview_view_width();
    unsigned preview_h = camera_preview_view_height();
    size_t preview_bytes = (size_t)preview_w * preview_h * 2U;

    pthread_mutex_lock(&camera_lock);
    snprintf(status, sizeof(status), "%s", camera_status_text);
    snprintf(preview_status, sizeof(preview_status), "%s", camera_preview_status_text);
    snprintf(photo, sizeof(photo), "%s", camera_last_photo_path);
    snprintf(thumb, sizeof(thumb), "%s", camera_last_thumb_path);
    result = camera_last_result;
    busy = camera_busy;
    preview_active = camera_preview_active;
    preview_have_frame = camera_preview_have_frame;
    preview_frame_ready = camera_preview_frame_ready;
    preview_result = camera_preview_last_result;
    if(preview_frame_ready) {
        memcpy(camera_preview_buf, camera_preview_frame_buf, preview_bytes);
        camera_preview_frame_ready = 0;
    }
    camera_result_ready = 0;
    pthread_mutex_unlock(&camera_lock);

    if(camera_status_label) {
        lv_label_set_text(camera_status_label,
                          busy ? "Capturing..." :
                          (preview_active || preview_have_frame ? preview_status : status));
        lv_obj_set_style_text_color(camera_status_label,
                                    lv_color_hex(busy ? 0xF5A524 :
                                                 (preview_active || preview_have_frame ?
                                                  (preview_result == 0 ? 0x25C281 : 0xEF4D5A) :
                                                  (result == 0 ? 0x25C281 :
                                                   result < 0 ? 0x9AA4AF : 0xEF4D5A))),
                                    0);
    }

    if(camera_meta_label) {
        lv_label_set_text(camera_meta_label,
                          has_video_node() ? "/dev/video1  640x360  NV16" :
                                             "camera node missing");
        lv_obj_set_style_text_color(camera_meta_label,
                                    lv_color_hex(has_video_node() ? 0xF2F5F8 : 0xF5A524),
                                    0);
    }

    if(camera_last_label) {
        base = strrchr(photo, '/');
        lv_label_set_text(camera_last_label,
                          photo[0] ? (base ? base + 1 : photo) : "No captured photo");
    }

    if(camera_preview_canvas) {
        if(preview_frame_ready || preview_have_frame) {
            lv_canvas_set_buffer(camera_preview_canvas, camera_preview_buf,
                                 preview_w, preview_h,
                                 LV_COLOR_FORMAT_RGB565);
            lv_obj_invalidate(camera_preview_canvas);
            if(camera_preview_hint_label) {
                lv_obj_add_flag(camera_preview_hint_label, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            camera_apply_preview_placeholder(camera_preview_canvas,
                                             camera_preview_hint_label,
                                             camera_preview_buf, preview_w,
                                             preview_h);
        }
    }
    if(camera_gallery_canvas) {
        unsigned gallery_w = camera_gallery_view_width();
        unsigned gallery_h = camera_gallery_view_height();

        camera_apply_canvas(camera_gallery_canvas, camera_gallery_hint_label, thumb,
                            camera_gallery_buf, gallery_w, gallery_h, 1);
    }
}

static void camera_timer_cb(lv_timer_t *timer)
{
    int refresh;

    (void)timer;

    pthread_mutex_lock(&camera_lock);
    refresh = camera_result_ready || camera_busy || camera_preview_frame_ready ||
              camera_preview_active;
    pthread_mutex_unlock(&camera_lock);

    if(refresh) {
        camera_update_visible_state();
    }
}

static void camera_make_capture_paths(char *photo, size_t photo_len,
                                      char *thumb, size_t thumb_len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char stamp[32];

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    snprintf(photo, photo_len, "%s/IMG_%s.ppm", CAMERA_PHOTO_DIR, stamp);
    snprintf(thumb, thumb_len, "%s/IMG_%s.thumb.rgb565", CAMERA_PHOTO_DIR, stamp);
}

static void *camera_capture_thread_cb(void *arg)
{
    char photo[192];
    char thumb[192];
    char cmd[640];
    int rc;
    int code;

    (void)arg;

    camera_stop_preview();

    if(camera_ensure_photo_dir() != 0) {
        camera_set_status("Photo directory error", 1);
        pthread_mutex_lock(&camera_lock);
        camera_busy = 0;
        pthread_mutex_unlock(&camera_lock);
        if(current_page == PAGE_CAMERA) {
            camera_start_preview();
        }
        return NULL;
    }

    camera_make_capture_paths(photo, sizeof(photo), thumb, sizeof(thumb));
    snprintf(cmd, sizeof(cmd),
             "%s -d %u -w %u -h %u -f NV16 -o %s -t %s "
             "--thumb-width %u --thumb-height %u "
             ">/tmp/k230_camera_capture.log 2>&1",
             CAMERA_CAPTURE_BIN, CAMERA_CAPTURE_DEVICE, CAMERA_CAPTURE_W,
             CAMERA_CAPTURE_H, photo, thumb, CAMERA_STORED_THUMB_W,
             CAMERA_STORED_THUMB_H);

    rc = system(cmd);
    code = shell_exit_code(rc);
    if(code != 0) {
        snprintf(cmd, sizeof(cmd),
                 "%s -d %u -w %u -h %u -f NV12 -o %s -t %s "
                 "--thumb-width %u --thumb-height %u "
                 ">>/tmp/k230_camera_capture.log 2>&1",
                 CAMERA_CAPTURE_BIN, CAMERA_CAPTURE_DEVICE, CAMERA_CAPTURE_W,
                 CAMERA_CAPTURE_H, photo, thumb, CAMERA_STORED_THUMB_W,
                 CAMERA_STORED_THUMB_H);
        rc = system(cmd);
        code = shell_exit_code(rc);
    }

    pthread_mutex_lock(&camera_lock);
    if(code == 0) {
        snprintf(camera_last_photo_path, sizeof(camera_last_photo_path), "%s", photo);
        snprintf(camera_last_thumb_path, sizeof(camera_last_thumb_path), "%s", thumb);
        snprintf(camera_status_text, sizeof(camera_status_text), "Saved to /root/photos");
        camera_last_result = 0;
    } else {
        snprintf(camera_status_text, sizeof(camera_status_text),
                 "Capture failed, see /tmp/k230_camera_capture.log");
        camera_last_result = code;
    }
    camera_busy = 0;
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);

    if(current_page == PAGE_CAMERA) {
        camera_start_preview();
    }

    return NULL;
}

static void camera_capture_event_cb(lv_event_t *event)
{
    pthread_t thread;
    int busy;

    (void)event;

    pthread_mutex_lock(&camera_lock);
    busy = camera_busy;
    if(!busy) {
        camera_busy = 1;
        camera_result_ready = 1;
        snprintf(camera_status_text, sizeof(camera_status_text), "Capturing...");
        camera_last_result = -1;
    }
    pthread_mutex_unlock(&camera_lock);

    if(busy) {
        camera_update_visible_state();
        return;
    }

    touch_trace_log("CAMERA_CAPTURE_START");
    if(pthread_create(&thread, NULL, camera_capture_thread_cb, NULL) != 0) {
        camera_set_status("Capture thread error", 1);
        pthread_mutex_lock(&camera_lock);
        camera_busy = 0;
        pthread_mutex_unlock(&camera_lock);
        return;
    }

    pthread_detach(thread);
    camera_update_visible_state();
}

static void camera_restart_isp_event_cb(lv_event_t *event)
{
    int rc;
    int code;

    (void)event;

    camera_stop_preview();
    rc = system("killall isp_media_server >/dev/null 2>&1; "
                "ISP_MEDIA_SENSOR_DRIVER=/usr/lib/libvvcam.so "
                "/usr/bin/isp_media_server >/tmp/isp.out.log 2>/tmp/isp.err.log &");
    code = shell_exit_code(rc);
    if(code == 0) {
        camera_set_status("ISP restart requested", 0);
    } else {
        camera_set_status("ISP restart command failed", code);
    }
    if(current_page == PAGE_CAMERA) {
        camera_start_preview();
    }
    camera_update_visible_state();
}

static void camera_status_event_cb(lv_event_t *event)
{
    (void)event;

    if(has_video_node()) {
        camera_set_status("Camera node present", 0);
    } else {
        camera_set_status("Camera node missing", 1);
    }
    camera_update_visible_state();
}

static void camera_gallery_event_cb(lv_event_t *event)
{
    (void)event;
    camera_gallery_view_open = 0;
    nav_to(PAGE_GALLERY);
}

static void camera_gallery_thumb_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    camera_gallery_select(index);
    camera_gallery_view_open = 1;
    render_page(PAGE_GALLERY, LV_SCREEN_LOAD_ANIM_NONE, 0);
}

static void camera_gallery_grid_event_cb(lv_event_t *event)
{
    (void)event;
    camera_gallery_view_open = 0;
    render_page(PAGE_GALLERY, LV_SCREEN_LOAD_ANIM_NONE, 0);
}

static void camera_gallery_swipe_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_point_t point;

    if(!evdev_indev || camera_gallery_count <= 1) {
        return;
    }

    lv_indev_get_point(evdev_indev, &point);
    if(code == LV_EVENT_PRESSED) {
        camera_gallery_press_x = point.x;
        camera_gallery_press_y = point.y;
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int32_t dx = point.x - camera_gallery_press_x;
        int32_t dy = point.y - camera_gallery_press_y;

        if(dx > 80 && llabs((long long)dy) < 80 && camera_gallery_selected > 0) {
            camera_gallery_select(camera_gallery_selected - 1);
            render_page(PAGE_GALLERY, LV_SCREEN_LOAD_ANIM_NONE, 0);
        } else if(dx < -80 && llabs((long long)dy) < 80 &&
                  camera_gallery_selected + 1 < camera_gallery_count) {
            camera_gallery_select(camera_gallery_selected + 1);
            render_page(PAGE_GALLERY, LV_SCREEN_LOAD_ANIM_NONE, 0);
        }
    }
}

static void camera_delete_photo_event_cb(lv_event_t *event)
{
    char photo[192];
    char thumb[192];

    (void)event;

    pthread_mutex_lock(&camera_lock);
    snprintf(photo, sizeof(photo), "%s", camera_last_photo_path);
    snprintf(thumb, sizeof(thumb), "%s", camera_last_thumb_path);
    pthread_mutex_unlock(&camera_lock);

    if(photo[0]) {
        unlink(photo);
    }
    if(thumb[0] && strcmp(thumb, photo) != 0) {
        unlink(thumb);
    }

    camera_gallery_scan();
    pthread_mutex_lock(&camera_lock);
    if(camera_gallery_count > 0) {
        if(camera_gallery_selected >= camera_gallery_count) {
            camera_gallery_selected = camera_gallery_count - 1;
        }
        snprintf(camera_last_photo_path, sizeof(camera_last_photo_path), "%s",
                 camera_gallery_items[camera_gallery_selected].photo);
        snprintf(camera_last_thumb_path, sizeof(camera_last_thumb_path), "%s",
                 camera_gallery_items[camera_gallery_selected].thumb);
        snprintf(camera_status_text, sizeof(camera_status_text), "Deleted photo");
        camera_last_result = 0;
    } else {
        camera_last_photo_path[0] = '\0';
        camera_last_thumb_path[0] = '\0';
        snprintf(camera_status_text, sizeof(camera_status_text), "No photo");
        camera_last_result = -1;
        camera_gallery_view_open = 0;
    }
    camera_result_ready = 1;
    pthread_mutex_unlock(&camera_lock);

    camera_update_visible_state();
}

static lv_obj_t *camera_icon_button(lv_obj_t *parent, int x, int y, int size,
                                    const char *symbol, uint32_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_add_style(btn, &style_button, 0);
    lv_obj_add_style(btn, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x161C24), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);

    lv_obj_t *icon = label(btn, symbol, &lv_font_montserrat_28, color);
    lv_obj_center(icon);
    make_click_forwarder(icon);
    return btn;
}

static lv_obj_t *camera_fixed_region(lv_obj_t *parent, int x, int y, int w,
                                     int h)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void camera_update_flip_buttons(void)
{
    if(camera_flip_h_btn && lv_obj_is_valid(camera_flip_h_btn)) {
        lv_obj_set_style_bg_color(camera_flip_h_btn,
                                  lv_color_hex(camera_preview_flip_h ?
                                               0x1E7D5B : 0x161C24), 0);
        lv_obj_set_style_border_color(camera_flip_h_btn,
                                      lv_color_hex(camera_preview_flip_h ?
                                                   0x25C281 : 0x263442), 0);
    }
    if(camera_flip_v_btn && lv_obj_is_valid(camera_flip_v_btn)) {
        lv_obj_set_style_bg_color(camera_flip_v_btn,
                                  lv_color_hex(camera_preview_flip_v ?
                                               0x1E7D5B : 0x161C24), 0);
        lv_obj_set_style_border_color(camera_flip_v_btn,
                                      lv_color_hex(camera_preview_flip_v ?
                                                   0x25C281 : 0x263442), 0);
    }
}

static void camera_flip_h_event_cb(lv_event_t *event)
{
    (void)event;

    camera_preview_flip_h = !camera_preview_flip_h;
    touch_trace_log("CAMERA_PREVIEW_FLIP_H enabled=%d", camera_preview_flip_h);
    camera_update_flip_buttons();
    request_fast_refresh();
}

static void camera_flip_v_event_cb(lv_event_t *event)
{
    (void)event;

    camera_preview_flip_v = !camera_preview_flip_v;
    touch_trace_log("CAMERA_PREVIEW_FLIP_V enabled=%d", camera_preview_flip_v);
    camera_update_flip_buttons();
    request_fast_refresh();
}

static void camera_rtsp_notice_close_cb(lv_event_t *event)
{
    (void)event;

    if(camera_rtsp_notice_overlay &&
       lv_obj_is_valid(camera_rtsp_notice_overlay)) {
        lv_obj_delete(camera_rtsp_notice_overlay);
    }
    camera_rtsp_notice_overlay = NULL;
    nav_back();
}

static void camera_show_rtsp_blocked_notice(void)
{
    int w = display_logical_width();
    int h = display_logical_height();
    int panel_w = w > 760 ? 500 : w - 72;
    int panel_h = 232;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *ok_btn;

    if(camera_rtsp_notice_overlay &&
       lv_obj_is_valid(camera_rtsp_notice_overlay)) {
        return;
    }
    if(panel_w > w - 48) {
        panel_w = w - 48;
    }
    if(panel_w < 300) {
        panel_w = w - 24;
    }
    if(panel_h > h - 48) {
        panel_h = h - 48;
    }

    camera_rtsp_notice_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(camera_rtsp_notice_overlay);
    lv_obj_set_style_bg_color(camera_rtsp_notice_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(camera_rtsp_notice_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(camera_rtsp_notice_overlay, 0, 0);
    lv_obj_set_style_pad_all(camera_rtsp_notice_overlay, 0, 0);
    lv_obj_clear_flag(camera_rtsp_notice_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = panel(camera_rtsp_notice_overlay, (w - panel_w) / 2,
                 (h - panel_h) / 2, panel_w, panel_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101720), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x314154), 0);
    lv_obj_set_style_pad_all(card, 24, 0);

    title = label(card, "RTSP is streaming", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    detail = label(card, "Stop RTSP streaming before using Camera.",
                   &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(detail, panel_w - 48);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 0, 48);

    ok_btn = command_button(card, (panel_w - 176) / 2, panel_h - 92,
                            176, "OK", 0x25C281);
    lv_obj_add_event_cb(ok_btn, camera_rtsp_notice_close_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void create_camera_page(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *viewfinder;
    lv_obj_t *controls;
    lv_obj_t *capture_btn;
    lv_obj_t *gallery_btn;
    lv_obj_t *mode;
    unsigned preview_w = camera_preview_view_width();
    unsigned preview_h = camera_preview_view_height();
    int landscape = display_orientation_is_landscape();
    int body_y = page_content_top_y(144);
    int rtsp_active;

    create_header(scr, "Camera");
    camera_refresh_latest_from_disk();

    body = camera_fixed_region(scr, 0, body_y, display_logical_width(),
                               page_body_height_from(144) + 24);

    viewfinder = panel(body, 24, 0, (int)preview_w, (int)preview_h);
    lv_obj_set_style_bg_color(viewfinder, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(viewfinder, 0, 0);

    mode = chip(viewfinder, "GC2093  1x  Auto", 0xF2F5F8);
    lv_obj_align(mode, LV_ALIGN_TOP_MID, 0, 16);

    camera_preview_canvas = lv_canvas_create(viewfinder);
    lv_obj_set_pos(camera_preview_canvas, 0, 0);
    lv_obj_set_size(camera_preview_canvas, (int)preview_w, (int)preview_h);
    lv_obj_set_style_radius(camera_preview_canvas, 8, 0);
    lv_obj_set_style_clip_corner(camera_preview_canvas, true, 0);

    camera_preview_hint_label = label(viewfinder, "Starting live preview",
                                      &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_center(camera_preview_hint_label);

    camera_status_label = label(viewfinder, "Live preview", &lv_font_montserrat_18,
                                0x25C281);
    lv_obj_set_width(camera_status_label, (int)preview_w - 32);
    lv_label_set_long_mode(camera_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(camera_status_label, LV_ALIGN_BOTTOM_LEFT, 16, -50);

    camera_meta_label = label(viewfinder, "/dev/video1  640x360  NV16",
                              &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_width(camera_meta_label, (int)preview_w - 32);
    lv_label_set_long_mode(camera_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_align(camera_meta_label, LV_ALIGN_BOTTOM_LEFT, 16, -24);

    controls = panel(body, landscape ? 24 + (int)preview_w + 20 : 24,
                     landscape ? 0 : 910,
                     landscape ? 260 : 520,
                     landscape ? (int)preview_h : 154);
    lv_obj_set_style_bg_color(controls, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(controls, 0, 0);

    gallery_btn = camera_icon_button(controls, landscape ? 96 : 22,
                                     landscape ? 36 : 43, 68, LV_SYMBOL_IMAGE,
                                     0xEC4899);
    lv_obj_add_event_cb(gallery_btn, camera_gallery_event_cb, LV_EVENT_CLICKED, NULL);

    capture_btn = camera_icon_button(controls, landscape ? 68 : 198,
                                     landscape ? ((int)preview_h - 132) / 2 : 15,
                                     landscape ? 132 : 124, LV_SYMBOL_OK, 0xFFFFFF);
    lv_obj_set_style_bg_color(capture_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(capture_btn, lv_color_hex(0x3DA5FF), 0);
    lv_obj_set_style_border_width(capture_btn, 8, 0);
    lv_obj_add_event_cb(capture_btn, camera_capture_event_cb, LV_EVENT_CLICKED, NULL);

    camera_flip_h_btn = camera_icon_button(controls, landscape ? 42 : 350,
                                           landscape ? (int)preview_h - 142 : 48,
                                           58, "H", 0x22D3EE);
    lv_obj_add_event_cb(camera_flip_h_btn, camera_flip_h_event_cb,
                        LV_EVENT_CLICKED, NULL);
    camera_flip_v_btn = camera_icon_button(controls, landscape ? 160 : 432,
                                           landscape ? (int)preview_h - 142 : 48,
                                           58, "V", 0x22D3EE);
    lv_obj_add_event_cb(camera_flip_v_btn, camera_flip_v_event_cb,
                        LV_EVENT_CLICKED, NULL);
    camera_update_flip_buttons();

    camera_timer = lv_timer_create(camera_timer_cb, 50, NULL);
    rtsp_active = ui_rtsp_is_active();
    if(rtsp_active) {
        camera_set_preview_status("RTSP stream active", 1, 0);
        camera_set_status("Stop RTSP before opening Camera", 1);
        camera_show_rtsp_blocked_notice();
    } else {
        camera_start_preview();
    }
    camera_update_visible_state();
}

static void create_gallery_page(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *delete_btn;
    lv_obj_t *grid_btn;
    unsigned gallery_w = camera_gallery_view_width();
    unsigned gallery_h = camera_gallery_view_height();

    camera_gallery_scan();
    if(camera_gallery_count > 0) {
        camera_gallery_select(camera_gallery_selected);
    } else {
        camera_gallery_view_open = 0;
    }

    body = lv_obj_create(scr);
    lv_obj_set_pos(body, 0, 54);
    lv_obj_set_size(body, (int)gallery_w, (int)gallery_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    if(!camera_gallery_view_open) {
        int cols = ((int)gallery_w - 48) / 188;

        if(cols < 2) {
            cols = 2;
        }
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(body, LV_DIR_VER);

        if(camera_gallery_count == 0) {
            camera_gallery_hint_label = label(body, "No photos",
                                              &lv_font_montserrat_24,
                                              0xF2F5F8);
            lv_obj_align(camera_gallery_hint_label, LV_ALIGN_CENTER, 0, -40);
        }

        for(int i = 0; i < camera_gallery_count; i++) {
            int col = i % cols;
            int row = i / cols;
            int x = 24 + col * 188;
            int y = 36 + row * 164;
            lv_obj_t *tile = panel(body, x, y, 168, 150);
            lv_obj_t *canvas;
            lv_obj_t *caption;
            const char *base = strrchr(camera_gallery_items[i].photo, '/');

            lv_obj_set_style_pad_all(tile, 4, 0);
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(tile, camera_gallery_thumb_event_cb,
                                LV_EVENT_CLICKED, (void *)(intptr_t)i);

            canvas = lv_canvas_create(tile);
            lv_obj_set_pos(canvas, 0, 0);
            lv_obj_set_size(canvas, CAMERA_GALLERY_THUMB_W,
                            CAMERA_GALLERY_THUMB_H);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE |
                            LV_OBJ_FLAG_EVENT_BUBBLE);
            camera_apply_canvas(canvas, NULL, camera_gallery_items[i].thumb,
                                camera_gallery_thumb_buf[i],
                                CAMERA_GALLERY_THUMB_W,
                                CAMERA_GALLERY_THUMB_H, 0);

            caption = label(tile, base ? base + 1 : camera_gallery_items[i].photo,
                            &lv_font_montserrat_14, 0xD3DAE3);
            lv_obj_set_width(caption, 156);
            lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
            lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -2);
            make_click_forwarder(caption);
        }
    } else {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(body, camera_gallery_swipe_event_cb,
                            LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(body, camera_gallery_swipe_event_cb,
                            LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(body, camera_gallery_swipe_event_cb,
                            LV_EVENT_PRESS_LOST, NULL);

        camera_gallery_canvas = lv_canvas_create(body);
        lv_obj_set_pos(camera_gallery_canvas, 0, 0);
        lv_obj_set_size(camera_gallery_canvas, (int)gallery_w, (int)gallery_h);
        lv_obj_set_style_radius(camera_gallery_canvas, 0, 0);
        lv_obj_set_style_clip_corner(camera_gallery_canvas, true, 0);
        lv_obj_add_flag(camera_gallery_canvas, LV_OBJ_FLAG_CLICKABLE |
                        LV_OBJ_FLAG_EVENT_BUBBLE);

        camera_gallery_hint_label = label(body, "No photo yet",
                                          &lv_font_montserrat_24, 0xF2F5F8);
        lv_obj_align(camera_gallery_hint_label, LV_ALIGN_CENTER, 0, -40);

        camera_last_label = label(body, "No captured photo",
                                  &lv_font_montserrat_18, 0x9AA4AF);
        lv_obj_align(camera_last_label, LV_ALIGN_BOTTOM_MID, 0, -68);
        lv_obj_set_width(camera_last_label, (int)gallery_w - 40);
        lv_label_set_long_mode(camera_last_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(camera_last_label, LV_TEXT_ALIGN_CENTER, 0);

        grid_btn = command_button(body, 24, (int)gallery_h - 76, 144, "Grid",
                                  0x3DA5FF);
        lv_obj_add_event_cb(grid_btn, camera_gallery_grid_event_cb,
                            LV_EVENT_CLICKED, NULL);
        delete_btn = command_button(body, (int)gallery_w - 188 - 24,
                                    (int)gallery_h - 76, 188, "Delete",
                                    0xEF4D5A);
        lv_obj_add_event_cb(delete_btn, camera_delete_photo_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }

    create_header(scr, "Gallery");

    camera_update_visible_state();
}

static void cleanup_page_state(void)
{
    ui_audio_cleanup();
    ui_lorawan_cleanup();
    ui_halow_cleanup();
    ui_lora_flrc_cleanup();
    ui_lora_cleanup();
    ui_meshtastic_cleanup();
    ui_nes_cleanup();
    ui_mic_spectrum_cleanup();
    ui_hardware_cleanup();
    ui_i2s_test_cleanup();
    ui_i2c_scan_cleanup();
    ui_display_test_cleanup();
    ui_hdmi_test_cleanup();
    ui_usb_storage_cleanup();
    ui_time_settings_cleanup();
    ui_ai_demo_cleanup();
    ui_rtsp_cleanup();
    ui_video_player_cleanup();
    ui_ble_cleanup();
    ui_nrf52840_dfu_cleanup();
    ui_cellular_cleanup();
    ui_usb_modem_cleanup();
    ui_terminal_cleanup();
    camera_stop_preview();
    if(camera_rtsp_notice_overlay &&
       lv_obj_is_valid(camera_rtsp_notice_overlay)) {
        lv_obj_delete(camera_rtsp_notice_overlay);
    }
    camera_rtsp_notice_overlay = NULL;
    ui_input_dialog_close_active();
    ui_wifi_cleanup();
    ui_wifi_iperf_cleanup();
    ui_ethernet_cleanup();
    if(touch_timer) {
        lv_timer_delete(touch_timer);
        touch_timer = NULL;
    }
    if(camera_timer) {
        lv_timer_delete(camera_timer);
        camera_timer = NULL;
    }
    if(network_timer) {
        lv_timer_delete(network_timer);
        network_timer = NULL;
    }
    if(home_telemetry_timer) {
        lv_timer_delete(home_telemetry_timer);
        home_telemetry_timer = NULL;
    }
    transition_old_page = NULL;
    page_transition_active = 0;
    home_time_label = NULL;
    date_label = NULL;
    home_temp_value_label = NULL;
    home_power_source_value_label = NULL;
    home_power_draw_value_label = NULL;
    home_eth_title_label = NULL;
    home_eth_value_label = NULL;
    home_temp_badge = NULL;
    home_power_source_badge = NULL;
    home_power_draw_badge = NULL;
    home_eth_badge = NULL;
    home_eth_badge_text_label = NULL;
    home_apps_scroll = NULL;
    home_telemetry_last_us = 0;
    touch_area = NULL;
    touch_marker = NULL;
    for(size_t i = 0; i < TOUCH_TRAIL_POINTS; i++) {
        touch_trail[i] = NULL;
    }
    for(size_t slot = 0; slot < UI_MULTITOUCH_MAX_POINTS; slot++) {
        touch_multi_marker[slot] = NULL;
        touch_multi_marker_label[slot] = NULL;
        for(size_t i = 0; i < TOUCH_MULTI_TRAIL_POINTS; i++) {
            touch_multi_trail[slot][i] = NULL;
        }
    }
    touch_state_label = NULL;
    touch_xy_label = NULL;
    touch_count_label = NULL;
    touch_latency_label = NULL;
    touch_mt_status_label = NULL;
    motion_rate_label = NULL;
    camera_preview_canvas = NULL;
    camera_preview_hint_label = NULL;
    camera_status_label = NULL;
    camera_meta_label = NULL;
    camera_last_label = NULL;
    camera_gallery_canvas = NULL;
    camera_gallery_hint_label = NULL;
    camera_flip_h_btn = NULL;
    camera_flip_v_btn = NULL;
    network_wifi_state_label = NULL;
    network_wifi_ip_label = NULL;
    network_eth_state_label = NULL;
    network_eth_ip_label = NULL;
    network_status_label = NULL;
    network_selected_label = NULL;
    network_password_ta = NULL;
    display_brightness_label = NULL;
    display_backlight_label = NULL;
    display_orientation_label = NULL;
    display_font_label = NULL;
    display_transition_label = NULL;
    display_timeout_label = NULL;
    display_keyboard_auto_rotate_switch = NULL;
    memset(display_rotation_btn, 0, sizeof(display_rotation_btn));
    memset(display_font_btn, 0, sizeof(display_font_btn));
    memset(display_transition_btn, 0, sizeof(display_transition_btn));
    memset(display_timeout_btn, 0, sizeof(display_timeout_btn));
    memset(network_ap_btn, 0, sizeof(network_ap_btn));
    memset(network_ap_title, 0, sizeof(network_ap_title));
    memset(network_ap_meta, 0, sizeof(network_ap_meta));
    memset(motion_bars, 0, sizeof(motion_bars));
    memset(motion_boxes, 0, sizeof(motion_boxes));
    memset(motion_bar_travel, 0, sizeof(motion_bar_travel));
    motion_start_us = 0;
    motion_last_frame_us = 0;
    motion_frame_sum_us = 0;
    motion_frame_min_us = 0;
    motion_frame_max_us = 0;
    motion_last_stats_us = 0;
    motion_frame_count = 0;
    motion_frame_dt_count = 0;
    motion_update_count = 0;
}

static void create_network_page(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *ethernet;
    lv_obj_t *wifi_list;
    lv_obj_t *connect;
    lv_obj_t *btn;
    int body_y = page_content_top_y(144);

    create_header(scr, "Network");

    body = scroll_region(scr, 0, body_y, display_logical_width(),
                         page_body_height_from(144) + 24);

    summary = panel(body, 24, 0, 520, 184);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x101820), 0);

    lv_obj_t *title = label(summary, "Wi-Fi", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    network_wifi_state_label = label(summary, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_set_width(network_wifi_state_label, 342);
    lv_label_set_long_mode(network_wifi_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(network_wifi_state_label, LV_ALIGN_TOP_LEFT, 0, 42);

    network_wifi_ip_label = label(summary, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(network_wifi_ip_label, LV_ALIGN_TOP_LEFT, 0, 76);

    network_status_label = label(summary, "Tap Scan to search WiFi",
                                 &lv_font_montserrat_16, 0xF5A524);
    lv_obj_set_width(network_status_label, ui_inner_width());
    lv_label_set_long_mode(network_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(network_status_label, LV_ALIGN_TOP_LEFT, 0, 120);

    btn = command_button(summary, 360, 22, 128, "Scan", 0x3DA5FF);
    lv_obj_add_event_cb(btn, wifi_scan_event_cb, LV_EVENT_CLICKED, NULL);

    ethernet = panel(body, 24, 204, 520, 138);
    lv_obj_set_style_bg_color(ethernet, lv_color_hex(0x101418), 0);

    lv_obj_t *eth_title = label(ethernet, "Ethernet", &lv_font_montserrat_22,
                                0xF2F5F8);
    lv_obj_align(eth_title, LV_ALIGN_TOP_LEFT, 0, 0);

    network_eth_state_label = label(ethernet, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_set_width(network_eth_state_label, ui_inner_width());
    lv_label_set_long_mode(network_eth_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(network_eth_state_label, LV_ALIGN_TOP_LEFT, 0, 42);

    network_eth_ip_label = label(ethernet, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(network_eth_ip_label, LV_ALIGN_TOP_LEFT, 0, 78);

    wifi_list = panel(body, 24, 362, 520, 394);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(0x101418), 0);

    lv_obj_t *list_title = label(wifi_list, "Available networks",
                                 &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(list_title, LV_ALIGN_TOP_LEFT, 0, 0);

    for(int i = 0; i < NET_MAX_APS; i++) {
        network_ap_btn[i] = lv_obj_create(wifi_list);
        lv_obj_add_style(network_ap_btn[i], &style_button, 0);
        lv_obj_add_style(network_ap_btn[i], &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_pos(network_ap_btn[i], 0, 42 + i * 56);
        lv_obj_set_size(network_ap_btn[i], ui_fit_width(lv_obj_get_parent(network_ap_btn[i]), 0, 488), 50);
        lv_obj_clear_flag(network_ap_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(network_ap_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(network_ap_btn[i], wifi_ap_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        network_ap_title[i] = label(network_ap_btn[i], i == 0 ? "No scan results" : "",
                                    &lv_font_montserrat_18, 0xF2F5F8);
        lv_obj_set_width(network_ap_title[i], 250);
        lv_label_set_long_mode(network_ap_title[i], LV_LABEL_LONG_DOT);
        lv_obj_align(network_ap_title[i], LV_ALIGN_LEFT_MID, 0, -9);
        make_click_forwarder(network_ap_title[i]);

        network_ap_meta[i] = label(network_ap_btn[i], i == 0 ? "Tap Scan" : "",
                                   &lv_font_montserrat_14, 0x9AA4AF);
        lv_obj_set_width(network_ap_meta[i], 250);
        lv_label_set_long_mode(network_ap_meta[i], LV_LABEL_LONG_DOT);
        lv_obj_align(network_ap_meta[i], LV_ALIGN_LEFT_MID, 0, 12);
        make_click_forwarder(network_ap_meta[i]);

        if(i > 0) {
            lv_obj_add_flag(network_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    connect = panel(body, 24, 776, 520, 282);
    lv_obj_set_style_bg_color(connect, lv_color_hex(0x101820), 0);

    lv_obj_t *connect_title = label(connect, "Connect", &lv_font_montserrat_22,
                                    0xF2F5F8);
    lv_obj_align(connect_title, LV_ALIGN_TOP_LEFT, 0, 0);

    network_selected_label = label(connect, "No network selected",
                                   &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_width(network_selected_label, ui_inner_width());
    lv_label_set_long_mode(network_selected_label, LV_LABEL_LONG_DOT);
    lv_obj_align(network_selected_label, LV_ALIGN_TOP_LEFT, 0, 38);

    network_password_ta = lv_textarea_create(connect);
    lv_obj_set_pos(network_password_ta, 0, 78);
    lv_obj_set_size(network_password_ta, ui_fit_width(lv_obj_get_parent(network_password_ta), 0, 488), 56);
    lv_textarea_set_one_line(network_password_ta, true);
    lv_textarea_set_password_mode(network_password_ta, true);
    lv_textarea_set_placeholder_text(network_password_ta, "Password, empty for open WiFi");
    lv_obj_set_style_text_font(network_password_ta,
                               ui_font_for_text("input", &lv_font_montserrat_18),
                               0);
    lv_obj_set_style_bg_color(network_password_ta, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_text_color(network_password_ta, lv_color_hex(0xF2F5F8), 0);
    lv_obj_add_event_cb(network_password_ta, wifi_password_event_cb, LV_EVENT_FOCUSED,
                        NULL);
    lv_obj_add_event_cb(network_password_ta, wifi_password_event_cb, LV_EVENT_CLICKED,
                        NULL);

    btn = command_button(connect, 150, 164, 188, "Connect", 0x25C281);
    lv_obj_add_event_cb(btn, wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);

    network_timer = lv_timer_create(network_timer_cb, 1000, NULL);
    network_update_page();
}

static void create_system_page(lv_obj_t *scr)
{
    char kernel[80];
    char cpu[160];
    char mem[80];
    char storage[80];
    char input[80];
    int body_y = page_content_top_y(154);
    int side_gap = display_orientation_is_landscape() ? 44 : 0;

    create_header(scr, "System");
    lv_obj_t *body = scroll_panel(scr, 24, body_y, page_body_width(),
                                  page_body_height_from(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    read_kernel_release(kernel, sizeof(kernel));
    read_cpu_summary(cpu, sizeof(cpu));
    read_mem_summary(mem, sizeof(mem));
    read_storage_summary(storage, sizeof(storage));
    snprintf(input, sizeof(input), "%s", find_input_event() ? find_input_event() : "None");

    info_row_with_side_gap(body, 8, "Board", "T-Display K230", 0xF2F5F8,
                           side_gap);
    info_row_with_side_gap(body, 62, "Kernel", kernel, 0xF2F5F8, side_gap);
    info_row_with_side_gap(body, 116, "CPU", cpu, 0x3DA5FF, side_gap);
    info_row_with_side_gap(body, 170, "Memory", mem, 0x25C281, side_gap);
    info_row_with_side_gap(body, 224, "Storage", storage, 0x25C281, side_gap);
    info_row_with_side_gap(body, 278, "Input", input,
                           find_input_event() ? 0xF2F5F8 : 0xF5A524,
                           side_gap);
    info_row_with_side_gap(body, 332, "Camera",
                           has_video_node() ? "Ready" : "Missing",
                           has_video_node() ? 0x25C281 : 0xF5A524,
                           side_gap);
    info_row_with_side_gap(body, 386, "Display", "/dev/dri/card0",
                           0xF2F5F8, side_gap);
    info_row_with_side_gap(body, 440, "Photos", CAMERA_PHOTO_DIR, 0xEC4899,
                           side_gap);
}

static void create_display_page(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *slider;
    char text[96];
    int has_backlight;
    int slider_min;
    int slider_value;
    int body_y = page_content_top_y(154);
    int content_w;
    int brightness_group_w;
    int brightness_group_x;
    int brightness_value_w;
    int brightness_slider_x;
    int brightness_slider_w;
    int button_gap = 14;
    int button_w;

    create_header(scr, "Display");
    body = scroll_panel(scr, 24, body_y, page_body_width(),
                        page_body_height_from(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    content_w = page_body_width() - 32;
    if(content_w < 360) {
        content_w = 360;
    }
    brightness_group_w = content_w * 85 / 100;
    if(brightness_group_w > content_w) {
        brightness_group_w = content_w;
    }
    if(brightness_group_w < 260) {
        brightness_group_w = content_w > 260 ? 260 : content_w;
    }
    brightness_group_x = display_orientation_is_landscape() ? 24 : 20;
    if(brightness_group_x + brightness_group_w > content_w) {
        brightness_group_x = content_w - brightness_group_w;
    }
    if(brightness_group_x < 0) {
        brightness_group_x = 0;
    }
    brightness_value_w = display_orientation_is_landscape() ? 180 : 144;
    if(brightness_value_w > brightness_group_w / 2) {
        brightness_value_w = brightness_group_w / 2;
    }
    brightness_slider_x = brightness_group_x;
    brightness_slider_w = brightness_group_w;
    button_w = (content_w - button_gap * 3) / 4;

    info_row(body, 8, "Panel", "RM69A10 AMOLED", 0xF2F5F8);
    info_row(body, 62, "Resolution", "568 x 1232", 0xF2F5F8);
    info_row(body, 116, "Color", "RGB565 / 16bpp", 0x25C281);
    info_row(body, 170, "Panel refresh", "RM69A10 tuned", 0x3DA5FF);

    has_backlight = find_backlight_device() == 0;
    slider_min = has_backlight ? backlight_min_value() : BACKLIGHT_MIN_VALUE;
    slider_value = has_backlight ? clamp_backlight_value(backlight_current_value) :
                   slider_min;
    display_backlight_label = label(body, "Brightness", &lv_font_montserrat_22,
                                    0xF2F5F8);
    lv_obj_set_pos(display_backlight_label, brightness_group_x, 242);
    lv_obj_set_width(display_backlight_label,
                     brightness_group_w - brightness_value_w - 12);
    lv_label_set_long_mode(display_backlight_label, LV_LABEL_LONG_DOT);

    if(has_backlight) {
        snprintf(text, sizeof(text), "%d / %d", slider_value, backlight_max_value);
    } else {
        snprintf(text, sizeof(text), "Backlight device missing");
    }
    display_brightness_label = label(body, text, &lv_font_montserrat_18,
                                     has_backlight ? 0x25C281 : 0xF5A524);
    lv_obj_set_pos(display_brightness_label,
                   brightness_group_x + brightness_group_w -
                   brightness_value_w, 244);
    lv_obj_set_width(display_brightness_label, brightness_value_w);
    lv_obj_set_style_text_align(display_brightness_label, LV_TEXT_ALIGN_RIGHT,
                                0);
    lv_label_set_long_mode(display_brightness_label, LV_LABEL_LONG_DOT);

    slider = lv_slider_create(body);
    lv_obj_set_pos(slider, brightness_slider_x, 332);
    lv_obj_set_size(slider, brightness_slider_w, 22);
    lv_slider_set_range(slider, slider_min, has_backlight ? backlight_max_value : 100);
    lv_slider_set_value(slider, slider_value, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, display_brightness_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    if(!has_backlight) {
        lv_obj_add_state(slider, LV_STATE_DISABLED);
    }

    label(body, "DRM Rotation", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 390);

    snprintf(text, sizeof(text), "%d deg", display_rotation_degrees);
    display_orientation_label = label(body, text, &lv_font_montserrat_18,
                                      display_orientation_is_landscape() ?
                                      0x3DA5FF : 0x25C281);
    lv_obj_align(display_orientation_label, LV_ALIGN_TOP_LEFT, 0, 428);

    display_rotation_btn[0] = command_button(body, 0, 488, button_w, "0",
                                             0x25C281);
    lv_obj_add_event_cb(display_rotation_btn[0], display_orientation_event_cb,
                        LV_EVENT_CLICKED, (void *)"0");
    display_rotation_btn[1] = command_button(body, button_w + button_gap, 488,
                                             button_w, "90",
                                             0x3DA5FF);
    lv_obj_add_event_cb(display_rotation_btn[1], display_orientation_event_cb,
                        LV_EVENT_CLICKED, (void *)"90");
    display_rotation_btn[2] = command_button(body,
                                             (button_w + button_gap) * 2, 488,
                                             button_w, "180",
                                             0x3DA5FF);
    lv_obj_add_event_cb(display_rotation_btn[2], display_orientation_event_cb,
                        LV_EVENT_CLICKED, (void *)"180");
    display_rotation_btn[3] = command_button(body,
                                             (button_w + button_gap) * 3, 488,
                                             button_w, "270",
                                             0x3DA5FF);
    lv_obj_add_event_cb(display_rotation_btn[3], display_orientation_event_cb,
                        LV_EVENT_CLICKED, (void *)"270");
    display_update_orientation_controls();

    label(body, "Keyboard auto landscape", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 570);

    label(body, "Rotate to 270 deg when the extension keyboard is detected",
          &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                     content_w > 120 ? content_w - 120 : content_w);
    lv_label_set_long_mode(lv_obj_get_child(body,
                           lv_obj_get_child_count(body) - 1),
                           LV_LABEL_LONG_WRAP);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 608);

    display_keyboard_auto_rotate_switch = lv_switch_create(body);
    lv_obj_set_size(display_keyboard_auto_rotate_switch, 72, 38);
    lv_obj_align(display_keyboard_auto_rotate_switch, LV_ALIGN_TOP_RIGHT, 0,
                 590);
    lv_obj_add_event_cb(display_keyboard_auto_rotate_switch,
                        display_keyboard_auto_rotate_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    display_update_keyboard_auto_rotate_control();

    label(body, "Font size", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 690);

    display_font_label = label(body, ui_font_size_label(),
                               &lv_font_montserrat_18, 0x25C281);
    lv_obj_align(display_font_label, LV_ALIGN_TOP_LEFT, 0, 728);

    button_w = (content_w - button_gap * 2) / 3;
    display_font_btn[0] = command_button(body, 0, 788, button_w, "Small",
                                         0x3DA5FF);
    lv_obj_add_event_cb(display_font_btn[0], display_font_size_event_cb,
                        LV_EVENT_CLICKED, (void *)"small");
    display_font_btn[1] = command_button(body, button_w + button_gap, 788,
                                         button_w, "Medium", 0x25C281);
    lv_obj_add_event_cb(display_font_btn[1], display_font_size_event_cb,
                        LV_EVENT_CLICKED, (void *)"medium");
    display_font_btn[2] = command_button(body,
                                         (button_w + button_gap) * 2, 788,
                                         button_w, "Large", 0x3DA5FF);
    lv_obj_add_event_cb(display_font_btn[2], display_font_size_event_cb,
                        LV_EVENT_CLICKED, (void *)"large");
    display_update_font_controls();

    label(body, "Page transition", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 870);

    display_transition_label = label(body, page_transition_label_text(),
                                     &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(display_transition_label, LV_ALIGN_TOP_LEFT, 0, 908);

    button_w = (content_w - button_gap * 2) / 3;
    display_transition_btn[0] = command_button(body, 0, 968, button_w, "Off",
                                               0x9AA4AF);
    lv_obj_add_event_cb(display_transition_btn[0],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"off");
    display_transition_btn[1] = command_button(body, button_w + button_gap,
                                               968, button_w, "Fade",
                                               0x3DA5FF);
    lv_obj_add_event_cb(display_transition_btn[1],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"fade");
    display_transition_btn[2] = command_button(body,
                                               (button_w + button_gap) * 2,
                                               968, button_w, "Left",
                                               0x3DA5FF);
    lv_obj_add_event_cb(display_transition_btn[2],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"slide_left");
    display_transition_btn[3] = command_button(body, 0, 1034, button_w, "Right",
                                               0x3DA5FF);
    lv_obj_add_event_cb(display_transition_btn[3],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"slide_right");
    display_transition_btn[4] = command_button(body, button_w + button_gap,
                                               1034, button_w, "Up",
                                               0x3DA5FF);
    lv_obj_add_event_cb(display_transition_btn[4],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"slide_up");
    display_transition_btn[5] = command_button(body,
                                               (button_w + button_gap) * 2,
                                               1034, button_w, "Cover",
                                               0x3DA5FF);
    lv_obj_add_event_cb(display_transition_btn[5],
                        display_transition_event_cb, LV_EVENT_CLICKED,
                        (void *)"cover");
    display_update_transition_controls();

    label(body, "Screen timeout", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 1120);

    display_timeout_load_pref();
    display_timeout_label = label(body, display_timeout_label_text(display_timeout_s),
                                  &lv_font_montserrat_18, 0x25C281);
    lv_obj_align(display_timeout_label, LV_ALIGN_TOP_LEFT, 0, 1158);

    button_w = (content_w - button_gap * 2) / 3;
    display_timeout_btn[0] = command_button(body, 0, 1218, button_w, "5 sec",
                                            0x25C281);
    lv_obj_add_event_cb(display_timeout_btn[0],
                        display_timeout_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)5);
    display_timeout_btn[1] = command_button(body, button_w + button_gap,
                                            1218, button_w, "10 sec",
                                            0x25C281);
    lv_obj_add_event_cb(display_timeout_btn[1],
                        display_timeout_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)10);
    display_timeout_btn[2] = command_button(body,
                                            (button_w + button_gap) * 2,
                                            1218, button_w, "30 sec",
                                            0x25C281);
    lv_obj_add_event_cb(display_timeout_btn[2],
                        display_timeout_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)30);
    display_timeout_btn[3] = command_button(body, 0, 1284, button_w, "60 sec",
                                            0x25C281);
    lv_obj_add_event_cb(display_timeout_btn[3],
                        display_timeout_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)60);
    display_timeout_btn[4] = command_button(body, button_w + button_gap,
                                            1284, button_w, "Never",
                                            0x9AA4AF);
    lv_obj_add_event_cb(display_timeout_btn[4],
                        display_timeout_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)0);
    display_update_timeout_controls();
}

static void create_settings_page(lv_obj_t *scr)
{
    int body_y = page_content_top_y(154);

    create_header(scr, "Settings");
    lv_obj_t *body = panel(scr, 24, body_y, 520, page_body_height_from(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    settings_nav_row(body, 0, "NET", "Network & internet",
                     "WiFi scan, password connect, Ethernet", 0x25C281,
                     PAGE_NETWORK);
    settings_nav_row(body, 112, LV_SYMBOL_EYE_OPEN, "Display",
                     "Backlight, orientation and panel status", 0x8B5CF6,
                     PAGE_DISPLAY);
    settings_nav_row(body, 224, "A", "Language",
                     "English and Chinese UI text", 0x22D3EE,
                     PAGE_LANGUAGE);
    settings_nav_row(body, 336, LV_SYMBOL_REFRESH, "Date & time",
                     "NTP server and time zone", 0xEC4899,
                     PAGE_TIME);
    settings_nav_row(body, 448, LV_SYMBOL_LIST, "System",
                     "Kernel, CPU, memory, storage", 0xF5A524,
                     PAGE_SYSTEM);
    settings_nav_row(body, 560, LV_SYMBOL_WARNING, "About phone",
                     "Buildroot version and device identity", 0x3DA5FF,
                     PAGE_ABOUT);
}

static void create_about_page(lv_obj_t *scr)
{
    char os[96];
    char kernel[80];
    char eth[96];
    char wifi[96];
    uint32_t color;
    int body_y = page_content_top_y(154);
    int side_gap = display_orientation_is_landscape() ? 44 : 0;

    create_header(scr, "About");
    lv_obj_t *body = scroll_panel(scr, 24, body_y, page_body_width(),
                                  page_body_height_from(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    read_os_pretty(os, sizeof(os));
    read_kernel_release(kernel, sizeof(kernel));
    read_iface_state(NET_ETH_IFACE, eth, sizeof(eth), &color);
    read_iface_state(NET_WIFI_IFACE, wifi, sizeof(wifi), &color);

    info_row_with_side_gap(body, 8, "Device", "LILYGO T-Display K230",
                           0xF2F5F8, side_gap);
    info_row_with_side_gap(body, 62, "Board", "kendryte k230 canmv v3",
                           0xF2F5F8, side_gap);
    info_row_with_side_gap(body, 116, "OS", os, 0x3DA5FF, side_gap);
    info_row_with_side_gap(body, 170, "Kernel", kernel, 0xF2F5F8, side_gap);
    info_row_with_side_gap(body, 224, "Panel", "RM69A10 568x1232",
                           0x25C281, side_gap);
    info_row_with_side_gap(body, 278, "Touch", "GT9895", 0x25C281,
                           side_gap);
    info_row_with_side_gap(body, 332, "Camera", "GC2093",
                           has_video_node() ? 0x25C281 : 0xF5A524,
                           side_gap);
    info_row_with_side_gap(body, 386, "Ethernet", eth, 0x25C281,
                           side_gap);
    info_row_with_side_gap(body, 440, "WiFi", wifi,
                           path_exists("/sys/class/net/" NET_WIFI_IFACE) ?
                           0x25C281 : 0x9AA4AF, side_gap);
}

static void create_placeholder_page(lv_obj_t *scr, const char *title, const char *state,
                                    uint32_t color)
{
    int body_y = page_content_top_y(154);

    create_header(scr, title);

    lv_obj_t *body = panel(scr, 24, body_y, 520, 330);
    lv_obj_t *big = label(body, state, &lv_font_montserrat_32, color);
    lv_obj_align(big, LV_ALIGN_TOP_LEFT, 0, 20);

    lv_obj_t *sub = label(body, "Stage 1 launcher shell", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 82);

    info_row(body, 160, "Navigation", "Ready", 0x25C281);
    info_row(body, 214, "Integration", "Next stage", 0xF5A524);
}

static void reboot_diag_shell_snapshot(const char *tag)
{
    char cmd[768];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "{ echo; echo '[reboot-diag] tag=%s'; date; "
             "printf '[reboot-diag] uptime='; cat /proc/uptime; "
             "echo '[reboot-diag] i2c-dev:'; ls -l /dev/i2c-* 2>/dev/null; "
             "if command -v i2cdetect >/dev/null 2>&1; then "
             "for d in /dev/i2c-*; do n=${d##*-}; "
             "echo \"[reboot-diag] i2cdetect bus=$n\"; i2cdetect -y \"$n\"; "
             "done; else echo '[reboot-diag] i2cdetect missing'; fi; } "
             ">> " REBOOT_DIAG_LOG " 2>&1",
             tag ? tag : "unknown");
    rc = system(cmd);
    touch_trace_log("REBOOT_SHELL_SNAPSHOT tag=%s rc=%d",
                    tag ? tag : "unknown", rc);
}

static void reboot_timer_cb(lv_timer_t *timer)
{
    int rc;

    (void)timer;

    touch_trace_log("REBOOT_EXECUTE");
    ui_hardware_reboot_diag_dump("reboot-timer-before-command");
    reboot_diag_shell_snapshot("reboot-timer-before-command");
    rc = system("(sync; reboot -f || reboot) >/tmp/k230_reboot.log 2>&1 &");
    touch_trace_log("REBOOT_COMMAND rc=%d", rc);
}

static void reboot_confirm_event_cb(lv_event_t *event)
{
    lv_timer_t *timer;

    (void)event;

    if(reboot_confirm_started) {
        return;
    }
    reboot_confirm_started = 1;
    touch_trace_log("REBOOT_CONFIRM");
    ui_hardware_reboot_diag_dump("reboot-confirm");

    if(reboot_confirm_btn && lv_obj_is_valid(reboot_confirm_btn)) {
        lv_obj_add_state(reboot_confirm_btn, LV_STATE_DISABLED);
    }
    if(reboot_status_label && lv_obj_is_valid(reboot_status_label)) {
        lv_label_set_text(reboot_status_label, ui_tr("Rebooting..."));
        lv_obj_set_style_text_color(reboot_status_label,
                                    lv_color_hex(0xF5A524), 0);
    }
    request_fast_refresh();

    timer = lv_timer_create(reboot_timer_cb, 250, NULL);
    if(timer) {
        lv_timer_set_repeat_count(timer, 1);
    }
}

static void reboot_cancel_event_cb(lv_event_t *event)
{
    (void)event;

    if(reboot_confirm_started) {
        return;
    }
    app_nav_back();
}

static void create_reboot_page(lv_obj_t *scr)
{
    int body_y = ui_page_top_y(144);
    int body_w = ui_page_panel_width();
    int body_h = ui_body_height(body_y) - 42;
    int landscape = display_orientation_is_landscape();
    int card_h = body_h;
    int icon_size = landscape ? 82 : 96;
    int content_x;
    int content_y;
    int content_w;
    int button_y;
    int button_gap = 18;
    int button_w;
    lv_obj_t *page_body;

    reboot_status_label = NULL;
    reboot_confirm_btn = NULL;
    reboot_confirm_started = 0;

    if(card_h < 330) {
        card_h = 330;
    }

    create_header(scr, "Reboot");

    page_body = ui_page_body(scr, 144);
    lv_obj_t *body = panel(page_body, ui_page_panel_x(), 18, body_w, card_h);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x111820), 0);

    lv_obj_t *icon_box = lv_obj_create(body);
    lv_obj_set_size(icon_box, icon_size, icon_size);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x2A171C), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, lv_color_hex(0x7F1D1D), 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    if(landscape) {
        lv_obj_align(icon_box, LV_ALIGN_TOP_LEFT, 24, 24);
        content_x = 24 + icon_size + 28;
        content_y = 24;
        content_w = body_w - content_x - 24;
    } else {
        lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 24);
        content_x = 20;
        content_y = 144;
        content_w = body_w - 40;
    }

    lv_obj_t *icon = label(icon_box, LV_SYMBOL_POWER, &lv_font_montserrat_32,
                           0xEF4D5A);
    lv_obj_center(icon);

    lv_obj_t *title = label(body, "Reboot device", &lv_font_montserrat_28,
                            0xF2F5F8);
    lv_obj_set_pos(title, content_x, content_y);
    lv_obj_set_width(title, content_w);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    lv_obj_t *detail = label(body, "Restart the K230 now?",
                             &lv_font_montserrat_20, 0xCBD5E1);
    lv_obj_set_pos(detail, content_x, content_y + 54);
    lv_obj_set_width(detail, content_w);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = label(body,
                           "The launcher will close and Linux will reboot.",
                           &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(hint, content_x, content_y + 98);
    lv_obj_set_width(hint, content_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    reboot_status_label = label(body, "", &lv_font_montserrat_18,
                                0x25C281);
    lv_obj_set_pos(reboot_status_label, content_x, content_y + 160);
    lv_obj_set_width(reboot_status_label, content_w);
    lv_label_set_long_mode(reboot_status_label, LV_LABEL_LONG_DOT);

    button_y = landscape ? content_y + 214 : content_y + 252;
    if(button_y > card_h - 88) {
        button_y = card_h - 88;
    }
    button_w = landscape ? 180 : 204;
    if(button_w * 2 + button_gap > body_w - 48) {
        button_w = (body_w - 48 - button_gap) / 2;
    }
    {
        int buttons_total = button_w * 2 + button_gap;
        int button_x = (body_w - buttons_total) / 2;

        if(button_x < 24) {
            button_x = 24;
        }

        lv_obj_t *cancel = command_button(body, button_x, button_y,
                                          button_w, "Cancel", 0xCBD5E1);
        lv_obj_add_event_cb(cancel, reboot_cancel_event_cb, LV_EVENT_CLICKED,
                            NULL);
        reboot_confirm_btn = command_button(body,
                                            button_x + button_w + button_gap,
                                            button_y, button_w, "Reboot",
                                            0xEF4D5A);
    }
    lv_obj_add_event_cb(reboot_confirm_btn, reboot_confirm_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void screenshot_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(SCREENSHOT_LOG, "a");
    if(!fp) {
        return;
    }

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static void screenshot_toast_delete_cb(lv_timer_t *timer)
{
    (void)timer;

    if(screenshot_toast_obj && lv_obj_is_valid(screenshot_toast_obj)) {
        lv_obj_delete_async(screenshot_toast_obj);
    }
    screenshot_toast_obj = NULL;
}

static void screenshot_show_toast(const char *title, const char *detail,
                                  uint32_t color)
{
    lv_obj_t *box;
    lv_obj_t *title_label;
    lv_obj_t *detail_label;
    lv_timer_t *timer;
    int w = display_logical_width();
    int h = display_logical_height();
    int box_w = display_orientation_is_landscape() ? 460 : 496;
    int box_h = 122;
    int x;
    int y;

    if(box_w > w - 48) {
        box_w = w - 48;
    }
    x = (w - box_w) / 2;
    y = h - box_h - (display_orientation_is_landscape() ? 28 : 64);
    if(y < 72) {
        y = 72;
    }

    if(screenshot_toast_obj && lv_obj_is_valid(screenshot_toast_obj)) {
        lv_obj_delete(screenshot_toast_obj);
    }

    box = lv_obj_create(lv_layer_top());
    screenshot_toast_obj = box;
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, box_w, box_h);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(box, 18, 0);
    lv_obj_set_style_shadow_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    title_label = label(box, title, &lv_font_montserrat_20, color);
    lv_obj_set_width(title_label, box_w - 32);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    detail_label = label(box, detail, &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_width(detail_label, box_w - 32);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label, LV_ALIGN_TOP_LEFT, 0, 42);

    lv_obj_move_foreground(box);
    start_opa_anim(box, LV_OPA_TRANSP, LV_OPA_COVER, 0, 120);

    timer = lv_timer_create(screenshot_toast_delete_cb, 2200, NULL);
    if(timer) {
        lv_timer_set_repeat_count(timer, 1);
    }
}

static int screenshot_make_path(char *path, size_t len)
{
    time_t now;
    struct tm tm_now;
    char stamp[32];
    char candidate[256];

    if(!path || len == 0) {
        return -1;
    }

    if(mkdir(SCREENSHOT_DIR, 0755) != 0 && errno != EEXIST) {
        snprintf(path, len, "%s", SCREENSHOT_DIR);
        return -1;
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
    snprintf(candidate, sizeof(candidate), SCREENSHOT_DIR "/screenshot_%s.png", stamp);
    if(access(candidate, F_OK) != 0) {
        snprintf(path, len, "%s", candidate);
        return 0;
    }

    for(int i = 1; i < 100; i++) {
        snprintf(candidate, sizeof(candidate), SCREENSHOT_DIR "/screenshot_%s_%02d.png",
                 stamp, i);
        if(access(candidate, F_OK) != 0) {
            snprintf(path, len, "%s", candidate);
            return 0;
        }
    }

    snprintf(path, len, SCREENSHOT_DIR "/screenshot_%s_last.png", stamp);
    return 0;
}

static int screenshot_buf_to_rgb24(const lv_draw_buf_t *buf, uint8_t *rgb,
                                   int w, int h, int *unsupported_cf)
{
    lv_color_format_t cf;
    int stride;

    if(!buf || !buf->data || !rgb || w <= 0 || h <= 0) {
        return -1;
    }

    cf = (lv_color_format_t)buf->header.cf;
    stride = buf->header.stride;
    if(stride <= 0) {
        int bpp = (int)LV_COLOR_FORMAT_GET_SIZE(cf);
        stride = bpp > 0 ? w * bpp : 0;
    }

    if(unsupported_cf) {
        *unsupported_cf = 0;
    }

    if(cf == LV_COLOR_FORMAT_RGB565 ||
       cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
        for(int y = 0; y < h; y++) {
            const uint8_t *src = buf->data + (size_t)y * (size_t)stride;
            uint8_t *dst = rgb + (size_t)y * (size_t)w * 3U;

            for(int x = 0; x < w; x++) {
                uint16_t px;

                if(cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
                    px = ((uint16_t)src[x * 2] << 8) |
                         (uint16_t)src[x * 2 + 1];
                } else {
                    px = (uint16_t)src[x * 2] |
                         ((uint16_t)src[x * 2 + 1] << 8);
                }

                dst[x * 3] = (uint8_t)((((px >> 11) & 0x1FU) * 255U) / 31U);
                dst[x * 3 + 1] = (uint8_t)((((px >> 5) & 0x3FU) * 255U) / 63U);
                dst[x * 3 + 2] = (uint8_t)(((px & 0x1FU) * 255U) / 31U);
            }
        }
        return 0;
    }

    if(cf == LV_COLOR_FORMAT_XRGB8888 ||
       cf == LV_COLOR_FORMAT_ARGB8888 ||
       cf == LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED) {
        for(int y = 0; y < h; y++) {
            const uint8_t *src = buf->data + (size_t)y * (size_t)stride;
            uint8_t *dst = rgb + (size_t)y * (size_t)w * 3U;

            for(int x = 0; x < w; x++) {
                const uint8_t *p = src + x * 4;

                dst[x * 3] = p[2];
                dst[x * 3 + 1] = p[1];
                dst[x * 3 + 2] = p[0];
            }
        }
        return 0;
    }

    if(cf == LV_COLOR_FORMAT_RGB888) {
        for(int y = 0; y < h; y++) {
            memcpy(rgb + (size_t)y * (size_t)w * 3U,
                   buf->data + (size_t)y * (size_t)stride,
                   (size_t)w * 3U);
        }
        return 0;
    }

    if(unsupported_cf) {
        *unsupported_cf = (int)cf;
    }
    return -2;
}

static int screenshot_save_current_frame(char *path, size_t path_len,
                                         char *error, size_t error_len)
{
    lv_draw_buf_t *buf;
    uint8_t *rgb;
    int w;
    int h;
    int unsupported_cf = 0;
    unsigned png_error;

    if(error && error_len > 0) {
        error[0] = '\0';
    }

    if(!main_display) {
        snprintf(error, error_len, "display not ready");
        return -1;
    }

    if(screenshot_make_path(path, path_len) != 0) {
        snprintf(error, error_len, "mkdir %s failed: %s",
                 SCREENSHOT_DIR, strerror(errno));
        return -1;
    }

    if(app_screen) {
        lv_obj_invalidate(app_screen);
    }
    lv_refr_now(main_display);

    buf = lv_display_get_buf_active(main_display);
    if(!buf || !buf->data) {
        snprintf(error, error_len, "active draw buffer unavailable");
        return -1;
    }

    w = buf->header.w ? (int)buf->header.w :
        (int)lv_display_get_horizontal_resolution(main_display);
    h = buf->header.h ? (int)buf->header.h :
        (int)lv_display_get_vertical_resolution(main_display);
    if(w <= 0 || h <= 0) {
        snprintf(error, error_len, "invalid frame size %dx%d", w, h);
        return -1;
    }

    rgb = malloc((size_t)w * (size_t)h * 3U);
    if(!rgb) {
        snprintf(error, error_len, "allocate %dx%d RGB failed", w, h);
        return -1;
    }

    if(screenshot_buf_to_rgb24(buf, rgb, w, h, &unsupported_cf) != 0) {
        snprintf(error, error_len, "unsupported color format %d",
                 unsupported_cf);
        free(rgb);
        return -1;
    }

    png_error = lodepng_encode24_file(path, rgb, (unsigned)w, (unsigned)h);
    free(rgb);
    if(png_error) {
        snprintf(error, error_len, "png encode failed %u: %s", png_error,
                 lodepng_error_text(png_error));
        unlink(path);
        return -1;
    }

    screenshot_log("saved path=%s size=%dx%d rotation=%d", path, w, h,
                   display_rotation_degrees);
    return 0;
}

static void screenshot_async_cb(void *user_data)
{
    char path[192];
    char error[192];
    const char *shown_path;

    (void)user_data;

    if(screenshot_save_current_frame(path, sizeof(path), error,
                                     sizeof(error)) == 0) {
        shown_path = strncmp(path, "/root/", 6) == 0 ? path + 6 : path;
        screenshot_show_toast("Screenshot saved", shown_path, 0x25C281);
        touch_trace_log("SCREENSHOT_SAVED path=%s", path);
    } else {
        screenshot_show_toast("Screenshot failed", error[0] ? error : "Unknown",
                              0xEF4D5A);
        screenshot_log("failed error=%s", error[0] ? error : "Unknown");
        touch_trace_log("SCREENSHOT_FAILED error=%s",
                        error[0] ? error : "Unknown");
    }
}

void app_take_screenshot(void)
{
    app_note_user_activity();
    lv_async_call(screenshot_async_cb, NULL);
}

static void render_page(page_id_t page, lv_screen_load_anim_t anim_type,
                        uint32_t anim_ms)
{
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *scr;
    lv_obj_t *old_page_root = NULL;
    int root_transition = 0;
    int leaving_lora_radio;
    int entering_lora_radio;
    const char *previous_page_name;
    int32_t root_from_x = 0;
    int32_t root_from_y = 0;
    uint64_t start_us = monotonic_us();

    (void)anim_ms;

    if(app_item_requires_keyboard_base(page) &&
       !ui_extension_keyboard_base_available()) {
        page = PAGE_HOME;
    }
    previous_page_name = page_name(current_page);
    leaving_lora_radio = page_uses_lora_radio(current_page);
    entering_lora_radio = page_uses_lora_radio(page);

    touch_trace_log("RENDER_BEGIN page=%s previous=%s", page_name(page),
                    previous_page_name);

    if(!app_screen) {
        app_screen = lv_obj_create(NULL);
        style_fullscreen_root(app_screen);
        lv_screen_load(app_screen);
        if(old_scr && old_scr != app_screen) {
            lv_obj_delete_async(old_scr);
        }

        page_root = create_page_root(0);
        create_status_bar(ensure_ui_stage());
    } else if(!page_root) {
        page_root = create_page_root(0);
        lv_obj_move_background(page_root);
    }

    root_transition = app_screen && page_root && page != current_page &&
                      page_transition_root_offset(&root_from_x, &root_from_y);
    if(root_transition) {
        old_page_root = page_root;
    }

    if(entering_lora_radio) {
        touch_trace_log("RADIO_OWNER_ENTER owner=%s previous=%s",
                        page_name(page), previous_page_name);
        ui_meshtastic_pause_for_radio_owner(page_name(page));
    }
    if(current_page == PAGE_NRF52840_DFU && page != PAGE_NRF52840_DFU) {
        ui_nrf52840_dfu_leave();
    }
    cleanup_page_state();
    if(root_transition) {
        page_root = create_page_root(root_from_x);
        lv_obj_set_y(page_root, root_from_y);
        lv_obj_move_foreground(page_root);
        if(status_bar_obj) {
            lv_obj_move_foreground(status_bar_obj);
        }
    }
    current_page = page;
    set_launcher_chrome(page);

    scr = page_root;

#if K230_R59_PERSISTENT_PANEL_UI
    render_r59_persistent_page(scr, page);
    touch_trace_log("PAGE_CONTENT_SWAP page=%s mode=persistent-panel-r59",
                    page_name(page));
#elif K230_R58_COMPACT_SWITCH_TEST
    render_compact_switch_page(scr, page);
    touch_trace_log("PAGE_CONTENT_SWAP page=%s mode=compact-label-only-r58",
                    page_name(page));
#else
    lv_obj_clean(scr);

    switch(page) {
    case PAGE_HOME:
        create_home(scr);
        break;
    case PAGE_CAMERA:
        create_camera_page(scr);
        break;
    case PAGE_NETWORK:
        ui_wifi_create(scr);
        break;
    case PAGE_WIFI:
        ui_wifi_create(scr);
        break;
    case PAGE_WIFI_IPERF:
        ui_wifi_iperf_create(scr);
        break;
    case PAGE_ETHERNET:
        ui_ethernet_create(scr);
        break;
    case PAGE_BLE:
        ui_ble_create(scr);
        break;
    case PAGE_NRF52840_DFU:
        ui_nrf52840_dfu_create(scr);
        break;
    case PAGE_MUSIC:
        ui_music_create(scr);
        break;
    case PAGE_VIDEO:
        ui_video_player_create(scr);
        break;
    case PAGE_NET_RADIO:
        ui_net_radio_create(scr);
        break;
    case PAGE_RECORDER:
        ui_recorder_create(scr);
        break;
    case PAGE_MIC_FFT:
        ui_mic_spectrum_create(scr);
        break;
    case PAGE_LORA:
        ui_lora_create(scr);
        break;
    case PAGE_MESHTASTIC:
        ui_meshtastic_create(scr);
        break;
    case PAGE_LORA_FLRC:
        ui_lora_flrc_create(scr);
        break;
    case PAGE_HALOW:
        ui_halow_create(scr);
        break;
    case PAGE_LORAWAN:
        ui_lorawan_create(scr);
        break;
    case PAGE_NES:
        ui_nes_create(scr);
        break;
    case PAGE_SYSTEM:
        create_system_page(scr);
        break;
    case PAGE_SETTINGS:
        ui_settings_create(scr);
        break;
    case PAGE_REBOOT:
        create_reboot_page(scr);
        break;
    case PAGE_DISPLAY:
        create_display_page(scr);
        break;
    case PAGE_DISPLAY_TEST:
        ui_display_test_create(scr);
        break;
    case PAGE_LANGUAGE:
        ui_language_create(scr);
        break;
    case PAGE_TIME:
        ui_time_settings_create(scr);
        break;
    case PAGE_AUDIO_SETTINGS:
        ui_audio_settings_create(scr);
        break;
    case PAGE_AUDIO_OUTPUT:
        ui_audio_output_create(scr);
        break;
    case PAGE_NOTIFICATION_SETTINGS:
        ui_notification_settings_create(scr);
        break;
    case PAGE_APP_STARTUP:
        ui_startup_settings_create(scr);
        break;
    case PAGE_I2S_TEST:
        ui_i2s_test_create(scr);
        break;
    case PAGE_I2C_SCAN:
        ui_i2c_scan_create(scr);
        break;
    case PAGE_HDMI_TEST:
        ui_hdmi_test_create(scr);
        break;
    case PAGE_FAN:
        ui_fan_create(scr);
        break;
    case PAGE_SENSORS:
        ui_sensors_create(scr);
        break;
    case PAGE_BQ25896:
        ui_bq25896_create(scr);
        break;
    case PAGE_BATTERY:
        ui_battery_monitor_create(scr);
        break;
    case PAGE_KEYBOARD_SETTINGS:
        ui_keyboard_settings_create(scr);
        break;
    case PAGE_KEYBOARD_HOTKEYS:
        ui_keyboard_hotkeys_create(scr);
        break;
    case PAGE_KEYBOARD_HOTKEY_ACTION:
        ui_keyboard_hotkey_action_create(scr);
        break;
    case PAGE_KEYBOARD_TEST:
        ui_keyboard_test_create(scr);
        break;
    case PAGE_BUTTON_TEST:
        ui_button_test_create(scr);
        break;
    case PAGE_INT0_TEST:
        ui_int0_test_create(scr);
        break;
    case PAGE_XL9555_TEST:
        ui_xl9555_led_create(scr);
        break;
    case PAGE_CELLULAR:
        ui_cellular_create(scr);
        break;
    case PAGE_USB_MODEM:
        ui_usb_modem_create(scr);
        break;
    case PAGE_ABOUT:
        create_about_page(scr);
        break;
    case PAGE_AI:
        ui_ai_demo_create(scr);
        break;
    case PAGE_RTSP:
        ui_rtsp_create(scr);
        break;
    case PAGE_FILES:
        ui_usb_storage_create(scr);
        break;
    case PAGE_TERMINAL:
        ui_terminal_create(scr);
        break;
    case PAGE_GALLERY:
        create_gallery_page(scr);
        break;
    case PAGE_SCREENSHOT:
        create_placeholder_page(scr, "Screenshot", "Tap the home icon or press FN+LILYGO", 0x22C55E);
        break;
    case PAGE_TOUCH_TEST:
        create_touch_test_page(scr);
        break;
    case PAGE_MOTION:
        create_motion_page(scr);
        break;
    case PAGE_LOGS:
        create_placeholder_page(scr, "Logs", "Status slot", 0x60A5FA);
        break;
    }

    update_time_labels(NULL);

    touch_trace_log("PAGE_CONTENT_SWAP page=%s mode=static-shell-no-slide-r57",
                    page_name(page));
    if(root_transition) {
        start_page_move_anim(scr, old_page_root, root_from_x, root_from_y,
                             PAGE_TRANSITION_ANIM_MS);
    } else {
        animate_page_enter(scr, anim_type);
    }
#endif

    touch_trace_log("RENDER_END page=%s duration=%.3fms", page_name(page),
                    (double)(monotonic_us() - start_us) / 1000.0);
    if(leaving_lora_radio && !entering_lora_radio) {
        if(page == PAGE_MESHTASTIC) {
            touch_trace_log("RADIO_OWNER_EXIT owner=%s resume=foreground",
                            previous_page_name);
            ui_meshtastic_release_radio_owner_foreground();
        } else {
            touch_trace_log("RADIO_OWNER_EXIT owner=%s resume=background",
                            previous_page_name);
            ui_meshtastic_resume_after_radio_owner();
        }
    }
    request_fast_refresh();
}

int main(void)
{
    lv_display_t *disp;
    char *drm_path;
    const char *input_dev;
    uint64_t last_loop_start_us = 0;

    app_start_us = monotonic_us();
    unlink(TOUCH_TRACE_PATH);
    unlink(EDGE_BACK_LOG_PATH);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    setenv("K230_LVGL_DRM_STAGING", "1", 0);
    touch_trace_verbose = getenv("K230_TOUCH_TRACE_VERBOSE") &&
                          strcmp(getenv("K230_TOUCH_TRACE_VERBOSE"), "0") != 0;
    touch_trace_log("APP_START trace_log=%s", TOUCH_TRACE_PATH);
    touch_trace_log("TRACE_VERBOSE %s", touch_trace_verbose ? "on" : "off");
    ui_hdmi_test_restore_one_shot_boot();
#if K230_R59_PERSISTENT_PANEL_UI
    touch_trace_log("PAGE_SWAP persistent-panel-r59");
#elif K230_R58_COMPACT_SWITCH_TEST
    touch_trace_log("PAGE_SWAP compact-label-only-r58");
#else
    touch_trace_log("PAGE_SWAP static-shell-no-slide-r57");
#endif

    lv_init();
#if LV_USE_FFMPEG
    lv_ffmpeg_init();
#endif
    ui_fonts_init();
    ui_i18n_init();
    ui_time_settings_apply_startup();
    ui_nrf52840_manager_startup();
    ui_hardware_startup();
    ui_hardware_reboot_diag_dump("app-start-after-hardware-startup");
    apply_display_brightness_pref();
    ui_cellular_startup();
    ui_ethernet_apply_startup();
    init_styles();
    load_runtime_display_orientation();
    page_transition_load_pref();
    edge_back_load_pref();
    edge_back_log("INIT enabled=%d log=%s", edge_back_enabled,
                  EDGE_BACK_LOG_PATH);

    disp = lv_linux_drm_create();
    if(!disp) {
        fprintf(stderr, "failed to create DRM display\n");
        return 1;
    }
#if LV_USE_SYSMON && LV_USE_PERF_MONITOR
    lv_sysmon_hide_performance(disp);
#endif
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_FLUSH_FINISH, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_add_event_cb(disp, display_trace_event_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    ui_fonts_apply_theme(disp);
    apply_display_timing(disp);

    drm_path = lv_linux_drm_find_device_path();
    if(!drm_path) {
        fprintf(stderr, "no DRM device found\n");
        return 1;
    }

    lv_linux_drm_set_rotation(disp, display_drm_rotation_from_orientation());
    lv_linux_drm_set_file(disp, drm_path, -1);
    lv_free(drm_path);
    main_display = disp;
    apply_display_orientation(disp);
    show_rotation_startup_cover(disp);

    input_dev = find_input_event();
    if(input_dev) {
        touch_trace_log("INPUT_DEVICE %s", input_dev);
        start_touch_trace(input_dev);
        ui_multitouch_start(input_dev);
        evdev_indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_dev);
        if(evdev_indev) {
            evdev_original_read_cb = lv_indev_get_read_cb(evdev_indev);
            lv_indev_set_read_cb(evdev_indev, evdev_guarded_read_cb);
        }
        apply_touch_transform();
    } else {
        fprintf(stderr, "no input event device found; touch disabled\n");
        touch_trace_log("INPUT_DEVICE none");
    }
    ui_extension_keyboard_register_indev();
    start_power_key_monitor();

    render_page(PAGE_HOME, LV_SCREEN_LOAD_ANIM_NONE, 0);
    lv_timer_create(update_time_labels, 1000, NULL);
    lv_timer_create(status_bar_update, 3000, NULL);
    ui_wifi_autoconnect_start();
    ui_meshtastic_startup();

    while(running) {
        uint64_t loop_start_us = monotonic_us();
        uint64_t handler_us;
        uint32_t wait_ms;

        if(last_loop_start_us && loop_start_us - last_loop_start_us > 100000ULL) {
            touch_trace_log("HANDLER_GAP gap=%.3fms page=%s",
                            (double)(loop_start_us - last_loop_start_us) / 1000.0,
                            page_name(current_page));
        }

        edge_back_update_raw_hint();
        edge_back_consume_raw_pending();
        power_key_shutdown_visual_poll();
        display_idle_poll();
        wait_ms = lv_timer_handler();
        handler_us = monotonic_us() - loop_start_us;
        if(handler_us > TOUCH_TRACE_SLOW_US) {
            touch_trace_log("HANDLER_SLOW duration=%.3fms page=%s",
                            (double)handler_us / 1000.0, page_name(current_page));
        }
        last_loop_start_us = loop_start_us;

        if(wait_ms == LV_NO_TIMER_READY || wait_ms > 5) {
            wait_ms = 5;
        } else if(wait_ms == 0) {
            wait_ms = 1;
        }
        usleep(wait_ms * 1000U);
    }

    stop_power_key_monitor();
    touch_trace_running = 0;
    ui_multitouch_stop();
    ui_hardware_reboot_diag_dump("app-stop-before-hardware-shutdown");
    ui_hardware_shutdown();
    ui_nrf52840_manager_shutdown();
    ui_hardware_reboot_diag_dump("app-stop-after-hardware-shutdown");
    touch_trace_log("APP_STOP");
    return 0;
}
