#include "ui_meshtastic.h"

#include "ui_audio.h"
#include "ui_ble.h"
#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_multitouch.h"
#include "ui_nrf9151_manager.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#include "qrcodegen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MESHTASTIC_PROBE_PATH "/root/app/k230_phone_ui/k230_meshtastic_probe"
#define MESHTASTIC_SOCKET_PATH "/tmp/k230_meshtastic.sock"
#define MESHTASTIC_DAEMON_LOG "/tmp/k230_meshtastic_daemon_ui.log"
#define MESHTASTIC_UI_TRACE_LOG "/tmp/k230_meshtastic_ui.log"
#define MESHTASTIC_CHANNEL_DIR "/root/meshtastic"
#define MESHTASTIC_CHANNEL_URL_FILE MESHTASTIC_CHANNEL_DIR "/channel_url.txt"
#define MESHTASTIC_CHANNEL_PROFILE_DIR MESHTASTIC_CHANNEL_DIR "/channels"
#define MESHTASTIC_CANNED_FILE MESHTASTIC_CHANNEL_DIR "/canned_messages.txt"
#define MESHTASTIC_QR_SCAN_PATH "/root/app/k230_phone_ui/k230_qr_scan"
#define MESHTASTIC_UI_LOG_MAX 4096
#define MESHTASTIC_UI_CHAT_MAX 8192
#define MESHTASTIC_UI_NODE_SELECT_MAX 24
#define MESHTASTIC_UI_NODE_LINE_MAX 768
#define MESHTASTIC_UI_WAYPOINT_SELECT_MAX 16
#define MESHTASTIC_UI_WAYPOINT_LINE_MAX 512
#define MESHTASTIC_UI_CHANNEL_SLOT_MAX 8
#define MESHTASTIC_CHANNEL_PROFILE_MAX 24
#define MESHTASTIC_CANNED_MAX 16
#define MESHTASTIC_CHANNEL_QR_MAX 640
#define MESHTASTIC_CHANNEL_QR_BORDER 4
#define MESHTASTIC_VOICE_RAW_PATH "/tmp/k230_mesh_voice_tx.raw"
#define MESHTASTIC_VOICE_RECORD_LOG "/tmp/k230_mesh_voice_record.log"
#define MESHTASTIC_VOICE_SAMPLE_RATE 8000U
#define MESHTASTIC_VOICE_SAMPLE_BYTES 2U
#define MESHTASTIC_VOICE_MIN_MS 500U
#define MESHTASTIC_VOICE_MIN_BYTES \
    ((MESHTASTIC_VOICE_SAMPLE_RATE * MESHTASTIC_VOICE_SAMPLE_BYTES * \
      MESHTASTIC_VOICE_MIN_MS) / 1000U)
#define MESHTASTIC_VOICE_MAX_SECONDS "10"
#define MESHTASTIC_PHOTO_DIR "/root/photos"
#define MESHTASTIC_PHOTO_STORE_DIR "/root/meshtastic/photos"
#define MESHTASTIC_PHOTO_MAX_ITEMS 24
#define MESHTASTIC_PHOTO_THUMB_W 160
#define MESHTASTIC_PHOTO_THUMB_H 120
#define MESHTASTIC_PHOTO_THUMB_BYTES \
    (MESHTASTIC_PHOTO_THUMB_W * MESHTASTIC_PHOTO_THUMB_H * 2)
#define MESHTASTIC_PHOTO_STORED_THUMB_W 360
#define MESHTASTIC_PHOTO_STORED_THUMB_H 640
#define MESHTASTIC_PHOTO_STORED_THUMB_BYTES \
    (MESHTASTIC_PHOTO_STORED_THUMB_W * MESHTASTIC_PHOTO_STORED_THUMB_H * 2)
#define MESHTASTIC_PREF_REGION "meshtastic.region"
#define MESHTASTIC_PREF_PRESET "meshtastic.preset"
#define MESHTASTIC_PREF_CHANNEL "meshtastic.channel"
#define MESHTASTIC_PREF_TX_CHANNEL "meshtastic.tx_channel"
#define MESHTASTIC_PREF_SLOT "meshtastic.slot"
#define MESHTASTIC_PREF_PSK "meshtastic.psk"
#define MESHTASTIC_PREF_POWER "meshtastic.power"
#define MESHTASTIC_PREF_NODE "meshtastic.node"
#define MESHTASTIC_PREF_FROM "meshtastic.from"
#define MESHTASTIC_PREF_TO "meshtastic.to"
#define MESHTASTIC_PREF_HOP "meshtastic.hop"
#define MESHTASTIC_PREF_ACK "meshtastic.ack"
#define MESHTASTIC_PREF_REBROADCAST "meshtastic.rebroadcast"
#define MESHTASTIC_PREF_AUTOSTART "meshtastic.autostart"
#define MESHTASTIC_PREF_POSITION "meshtastic.position"
#define MESHTASTIC_PREF_POSITION_INTERVAL "meshtastic.position_interval"
#define MESHTASTIC_PREF_FIXED_POSITION "meshtastic.fixed_position"
#define MESHTASTIC_PREF_FIXED_LATITUDE "meshtastic.fixed_latitude_i"
#define MESHTASTIC_PREF_FIXED_LONGITUDE "meshtastic.fixed_longitude_i"
#define MESHTASTIC_PREF_FIXED_ALTITUDE "meshtastic.fixed_altitude_m"
#define MESHTASTIC_PREF_TELEMETRY "meshtastic.telemetry"
#define MESHTASTIC_PREF_TELEMETRY_ENV "meshtastic.telemetry_env"
#define MESHTASTIC_PREF_TELEMETRY_DEVICE_INTERVAL "meshtastic.telemetry_device_interval"
#define MESHTASTIC_PREF_TELEMETRY_ENV_INTERVAL "meshtastic.telemetry_env_interval"
#define MESHTASTIC_PREF_PHOTO_REPEAT "meshtastic.photo_repeat"
#define MESHTASTIC_PREF_PHOTO_REPAIR_ROUNDS "meshtastic.photo_repair_rounds"
#define MESHTASTIC_PREF_PHOTO_REPAIR_REPEAT "meshtastic.photo_repair_repeat"
#define MESHTASTIC_PREF_PHOTO_REPAIR_WINDOW_MS "meshtastic.photo_repair_window_ms"
#define MESHTASTIC_PREF_PHOTO_CACHE_TTL_SEC "meshtastic.photo_cache_ttl_sec"
#define MESHTASTIC_DEFAULT_UI_REGION "EU_868"
#define MESHTASTIC_DEFAULT_UI_PRESET "LONG_FAST"
#define MESHTASTIC_QR_PREVIEW_FILE "/tmp/k230_mesh_qr_preview.rgb565"
#define MESHTASTIC_QR_PREVIEW_TMP MESHTASTIC_QR_PREVIEW_FILE ".tmp"
#define MESHTASTIC_QR_CAPTURE_W 1280
#define MESHTASTIC_QR_CAPTURE_H 720
#define MESHTASTIC_QR_PREVIEW_W 512
#define MESHTASTIC_QR_PREVIEW_H 288
#define MESHTASTIC_QR_PREVIEW_BYTES (MESHTASTIC_QR_PREVIEW_W * MESHTASTIC_QR_PREVIEW_H * 2)
#define MESHTASTIC_MAP_ROOT "/root/maps"
#define MESHTASTIC_MAP_STYLE "openstreetmap"
#define MESHTASTIC_MAP_TILE_SIZE 256
#define MESHTASTIC_MAP_MIN_ZOOM 5
#define MESHTASTIC_MAP_MAX_ZOOM 14
#define MESHTASTIC_MAP_DEFAULT_ZOOM 12
#define MESHTASTIC_MAP_PREFETCH_TILES 1
#define MESHTASTIC_MAP_PINCH_IN_RATIO 1.32
#define MESHTASTIC_MAP_PINCH_OUT_RATIO 0.76
#define MESHTASTIC_MAP_FAKE_LAT 23.1291
#define MESHTASTIC_MAP_FAKE_LON 113.2644
#define MESHTASTIC_MAP_RESPONSE_MAX 32768
#define MESHTASTIC_PREF_MAP_FAKE_GPS "meshtastic.map.fake_gps"
#define MESHTASTIC_PREF_MAP_ZOOM "meshtastic.map.zoom"
#define MESHTASTIC_NODE_RECENT_WINDOW_S 900
#define MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS 6
#define MESHTASTIC_NODE_DETAIL_REFRESH_TICKS 1
#define MESHTASTIC_NODE_DETAIL_REFRESH_ATTEMPTS 5

typedef enum {
    MESH_NODES_OVERLAY_NONE = 0,
    MESH_NODES_OVERLAY_LIST,
    MESH_NODES_OVERLAY_DETAIL,
} mesh_nodes_overlay_kind_t;

static void mesh_nodes_event_cb(lv_event_t *event);
static void mesh_channels_event_cb(lv_event_t *event);
static void mesh_detector_event_cb(lv_event_t *event);
static void mesh_overlay_auto_refresh_tick(void);
static void mesh_node_detail_refresh_current(void);
static void mesh_node_detail_open(const char *line);
static void mesh_layout_main(void);
static void mesh_update_target_button(void);
static void mesh_save_profile_prefs(void);
static void mesh_channel_import_confirm_show(const char *url,
                                             const char *preview_response);
static int mesh_parse_u32_text(const char *text, unsigned long *value);
static uint16_t mesh_rgb565(uint32_t rgb);
static int mesh_node_line_value(const char *line, const char *key,
                                char *out, size_t out_len);

static lv_obj_t *mesh_status_label;
static lv_obj_t *mesh_detail_label;
static lv_obj_t *mesh_profile_label;
static lv_obj_t *mesh_airtime_label;
static lv_obj_t *mesh_chutil_bar;
static lv_obj_t *mesh_chat_scroll;
static lv_obj_t *mesh_log_label;
static lv_obj_t *mesh_send_button;
static lv_obj_t *mesh_canned_button;
static lv_obj_t *mesh_voice_button;
static lv_obj_t *mesh_photo_button;
static lv_obj_t *mesh_target_button;
static lv_obj_t *mesh_target_label;
static lv_obj_t *mesh_channel_button;
static lv_obj_t *mesh_tx_channel_button;
static lv_obj_t *mesh_map_button;
static lv_obj_t *mesh_nodes_button;
static lv_obj_t *mesh_settings_button;
static lv_obj_t *mesh_body;
static lv_obj_t *mesh_status_panel;
static lv_obj_t *mesh_input_panel;
static lv_obj_t *mesh_textarea;
static ui_input_inline_t *mesh_inline_input;
static lv_timer_t *mesh_timer;
static lv_timer_t *mesh_background_timer;
static lv_timer_t *mesh_map_timer;
static char mesh_status_text[4096] = "Not running";
static char mesh_log_text[MESHTASTIC_UI_LOG_MAX];
static char mesh_last_chat_text[MESHTASTIC_UI_CHAT_MAX];
static char mesh_last_ble_state[32] = "offline";
static char mesh_node_select_ids[MESHTASTIC_UI_NODE_SELECT_MAX][24];
static char mesh_node_select_lines[MESHTASTIC_UI_NODE_SELECT_MAX][MESHTASTIC_UI_NODE_LINE_MAX];
static char mesh_waypoint_select_lines[MESHTASTIC_UI_WAYPOINT_SELECT_MAX][MESHTASTIC_UI_WAYPOINT_LINE_MAX];
static char mesh_node_detail_target_id[24];
static char mesh_node_detail_status_text[160];
static char mesh_radio_pause_owner[32];
static int mesh_keyboard_reserved_h;
static int mesh_status_panel_h;
static int mesh_chat_gap;
static int mesh_radio_pause_active;
static char mesh_region[24] = MESHTASTIC_DEFAULT_UI_REGION;
static char mesh_preset[32] = MESHTASTIC_DEFAULT_UI_PRESET;
static char mesh_channel_name[64] = "";
static char mesh_tx_channel_name[64] = "default";
static char mesh_frequency_slot[8] = "auto";
static char mesh_psk[80] = "default";
static char mesh_tx_power[8] = "auto";
static char mesh_node_name[48] = "k230-t-display";
static char mesh_from_node[24] = "0";
static char mesh_to_node[24] = "0xffffffff";
static char mesh_hop_limit[8] = "3";
static char mesh_position_interval[8] = "900";
static char mesh_fixed_latitude_i[16] = "0";
static char mesh_fixed_longitude_i[16] = "0";
static char mesh_fixed_altitude_m[12] = "0";
static char mesh_telemetry_device_interval[8] = "300";
static char mesh_telemetry_environment_interval[8] = "300";
static char mesh_photo_repeat[8] = "2";
static char mesh_photo_repair_rounds[8] = "2";
static char mesh_photo_repair_repeat[8] = "2";
static char mesh_photo_repair_window_ms[8] = "7000";
static char mesh_photo_cache_ttl_sec[8] = "300";
static int mesh_ack_enabled = 0;
static int mesh_rebroadcast_enabled = 0;
static int mesh_position_enabled = 1;
static int mesh_fixed_position_enabled = 0;
static int mesh_telemetry_enabled = 1;
static int mesh_environment_telemetry_enabled = 1;
static int mesh_tx_channel_index = 0;
static int mesh_voice_available = 0;
static int mesh_photo_available = 0;
static int mesh_map_fake_gps_enabled = 1;
static int mesh_map_zoom = MESHTASTIC_MAP_DEFAULT_ZOOM;
static int mesh_map_center_valid = 0;
static double mesh_map_center_lat;
static double mesh_map_center_lon;
static int mesh_map_drag_active = 0;
static int mesh_map_drag_dirty = 0;
static lv_point_t mesh_map_drag_last_point;
static int mesh_map_drag_total_dx = 0;
static int mesh_map_drag_total_dy = 0;
static int mesh_map_pinch_active = 0;
static double mesh_map_pinch_start_distance = 0.0;
static int mesh_map_pinch_start_zoom = MESHTASTIC_MAP_DEFAULT_ZOOM;
static int mesh_map_has_position = 0;
static char mesh_map_notice_text[128];
static uint32_t mesh_map_notice_color = 0x25C281;
static lv_obj_t *mesh_map_view_obj;
static lv_obj_t *mesh_map_layer_obj;
static lv_obj_t *mesh_settings_overlay;
static lv_obj_t *mesh_settings_status_card;
static lv_obj_t *mesh_settings_status_labels[3];
static lv_obj_t *mesh_log_overlay;
static lv_obj_t *mesh_nodes_overlay;
static lv_obj_t *mesh_nodes_panel;
static lv_obj_t *mesh_map_overlay;
static lv_obj_t *mesh_waypoints_overlay;
static lv_obj_t *mesh_detector_overlay;
static lv_obj_t *mesh_detector_panel;
static lv_obj_t *mesh_choice_overlay;
static lv_obj_t *mesh_channel_overlay;
static lv_obj_t *mesh_channels_overlay;
static lv_obj_t *mesh_channel_edit_overlay;
static lv_obj_t *mesh_channel_profiles_overlay;
static lv_obj_t *mesh_channel_profile_delete_overlay;
static lv_obj_t *mesh_channel_import_overlay;
static lv_obj_t *mesh_channel_import_status_label;
static lv_obj_t *mesh_canned_overlay;
static lv_obj_t *mesh_canned_delete_overlay;
static lv_obj_t *mesh_voice_preview_overlay;
static lv_obj_t *mesh_voice_preview_status_label;
static lv_obj_t *mesh_photo_picker_overlay;
static lv_obj_t *mesh_photo_preview_overlay;
static lv_obj_t *mesh_photo_status_label;
static lv_timer_t *mesh_photo_picker_close_timer;
static uint8_t *mesh_photo_preview_pixels;
static lv_image_dsc_t mesh_photo_preview_dsc;
static lv_obj_t *mesh_voice_record_overlay;
static lv_obj_t *mesh_voice_record_time_label;
static lv_obj_t *mesh_voice_record_level_label;
static lv_timer_t *mesh_voice_record_timer;
static lv_obj_t *mesh_channel_url_label;

typedef struct {
    char paths[512];
    char idle_text[96];
    unsigned duration_ms;
    lv_obj_t *bubble;
    lv_obj_t *label;
} mesh_voice_bubble_ctx_t;

typedef struct {
    char photo[192];
    char thumb[192];
    time_t mtime;
} mesh_photo_item_t;

typedef struct {
    char path[192];
    char title[96];
    unsigned width;
    unsigned height;
    uint8_t *thumb_pixels;
    lv_image_dsc_t thumb_dsc;
} mesh_photo_bubble_ctx_t;

typedef enum {
    MESH_MAP_POS_READY = 0,
    MESH_MAP_POS_DAEMON_OFFLINE,
    MESH_MAP_POS_NRF9151_MISSING,
    MESH_MAP_POS_USING_CACHE,
    MESH_MAP_POS_FIRST_FIX,
    MESH_MAP_POS_NO_SATELLITES,
    MESH_MAP_POS_SATELLITES_NO_FIX,
    MESH_MAP_POS_GNSS_SEARCHING,
    MESH_MAP_POS_GNSS_ERROR,
    MESH_MAP_POS_COORD_UNAVAILABLE,
} mesh_map_position_state_t;
static lv_obj_t *mesh_channel_status_label;
static lv_obj_t *mesh_channel_qr_canvas;
static lv_obj_t *mesh_channel_qr_fullscreen_overlay;
static lv_obj_t *mesh_channel_qr_fullscreen_canvas;
static lv_obj_t *mesh_publish_status_label;
static lv_obj_t *mesh_node_request_status_label;
static lv_obj_t *mesh_pairing_overlay;
static lv_obj_t *mesh_settings_value_labels[24];
static char mesh_last_pairing_code[16];
static char mesh_channel_url_text[1024];
static char mesh_channel_profile_edit_path[160];
static char mesh_channel_profile_delete_path[160];
static char mesh_channel_import_pending_url[1024];
static char mesh_channel_scan_pending_url[1024];
static int mesh_channel_edit_index = -1;
static char mesh_channel_edit_role[16] = "secondary";
static char mesh_channel_edit_name[64];
static char mesh_channel_edit_psk[96] = "default";
static lv_obj_t *mesh_channel_edit_title_label;
static lv_obj_t *mesh_channel_edit_role_label;
static lv_obj_t *mesh_channel_edit_name_label;
static lv_obj_t *mesh_channel_edit_psk_label;
static lv_obj_t *mesh_channel_edit_status_label;
static char mesh_canned_messages[MESHTASTIC_CANNED_MAX][160];
static int mesh_canned_manage_mode;
static int mesh_canned_delete_index = -1;
static pid_t mesh_voice_record_pid = -1;
static uint64_t mesh_voice_record_start_us;
static size_t mesh_voice_record_bytes;
static unsigned mesh_voice_record_duration_ms;
static pid_t mesh_voice_play_pid = -1;
static uint64_t mesh_voice_play_start_us;
static mesh_voice_bubble_ctx_t *mesh_voice_playing_ctx;
static lv_timer_t *mesh_voice_playing_timer;
static unsigned mesh_voice_playing_phase;
static unsigned mesh_voice_record_phase;
static int mesh_photo_send_inflight;
static mesh_photo_item_t mesh_photo_items[MESHTASTIC_PHOTO_MAX_ITEMS];
static int mesh_photo_item_count;
static uint8_t mesh_photo_thumb_buf[MESHTASTIC_PHOTO_MAX_ITEMS]
                                [MESHTASTIC_PHOTO_THUMB_BYTES];
static uint8_t mesh_photo_stored_thumb_buf[MESHTASTIC_PHOTO_STORED_THUMB_BYTES];
static uint16_t mesh_channel_qr_buf[MESHTASTIC_CHANNEL_QR_MAX *
                                    MESHTASTIC_CHANNEL_QR_MAX];
static uint16_t *mesh_channel_qr_fullscreen_buf;
static lv_timer_t *mesh_channel_scan_timer;
static lv_obj_t *mesh_channel_scan_overlay;
static lv_obj_t *mesh_channel_scan_preview_image;
static lv_obj_t *mesh_channel_scan_preview_placeholder;
static lv_obj_t *mesh_channel_scan_preview_status_label;
static lv_obj_t *mesh_channel_scan_guide_box;
static int mesh_channel_scan_preview_panel_w;
static int mesh_channel_scan_preview_panel_h;
static uint8_t *mesh_channel_scan_preview_pixels;
static lv_image_dsc_t mesh_channel_scan_preview_dsc;
static lv_obj_t *mesh_notification_toast;
static lv_timer_t *mesh_notification_timer;
static pthread_mutex_t mesh_channel_scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mesh_channel_scan_thread;
static int mesh_channel_scan_running;
static int mesh_channel_scan_ready;
static int mesh_channel_scan_ok;
static char mesh_channel_scan_status[512];
static char mesh_channel_profile_paths[MESHTASTIC_CHANNEL_PROFILE_MAX][160];
static mesh_nodes_overlay_kind_t mesh_nodes_overlay_kind =
    MESH_NODES_OVERLAY_NONE;
static int mesh_nodes_auto_refresh_ticks =
    MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
static int mesh_detector_auto_refresh_ticks =
    MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
static int mesh_nodes_preserve_scroll;
static int mesh_detector_preserve_scroll;
static int mesh_node_detail_refresh_ticks;
static int mesh_node_detail_refresh_attempts;
static uint32_t mesh_node_detail_status_color = 0x94A3B8;

typedef enum {
    MESH_FIELD_REGION = 0,
    MESH_FIELD_PRESET,
    MESH_FIELD_CHANNEL,
    MESH_FIELD_SLOT,
    MESH_FIELD_PSK,
    MESH_FIELD_POWER,
    MESH_FIELD_NODE,
    MESH_FIELD_FROM,
    MESH_FIELD_TO,
    MESH_FIELD_HOP,
    MESH_FIELD_ACK,
    MESH_FIELD_REBROADCAST,
    MESH_FIELD_POSITION,
    MESH_FIELD_POSITION_INTERVAL,
    MESH_FIELD_TELEMETRY,
    MESH_FIELD_TELEMETRY_INTERVAL,
    MESH_FIELD_ENV_TELEMETRY,
    MESH_FIELD_ENV_TELEMETRY_INTERVAL,
    MESH_FIELD_PHOTO_REPEAT,
    MESH_FIELD_PHOTO_REPAIR_ROUNDS,
    MESH_FIELD_PHOTO_REPAIR_REPEAT,
    MESH_FIELD_PHOTO_REPAIR_WINDOW,
    MESH_FIELD_PHOTO_CACHE_TTL,
    MESH_FIELD_COUNT,
} mesh_setting_field_t;

typedef enum {
    MESH_PROFILE_STD = 0,
    MESH_PROFILE_EU868,
    MESH_PROFILE_LITE,
    MESH_PROFILE_NARROW,
    MESH_PROFILE_HAM_20KHZ,
    MESH_PROFILE_HAM_100KHZ,
} mesh_choice_profile_t;

typedef struct {
    const char *value;
    const char *label;
    mesh_choice_profile_t profile;
    const char *default_preset;
} mesh_region_choice_t;

typedef struct {
    const char *value;
    const char *label;
    unsigned profiles;
} mesh_preset_choice_t;

typedef struct {
    const char *value;
    const char *label;
} mesh_choice_t;

#define MESH_PROFILE_MASK(profile) (1U << (unsigned)(profile))

static const mesh_region_choice_t mesh_region_choices[] = {
    {"US", "US 902-928", MESH_PROFILE_STD, "LONG_FAST"},
    {"EU_433", "EU 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"EU_868", "EU 868", MESH_PROFILE_EU868, "LONG_FAST"},
    {"EU_866", "EU 866", MESH_PROFILE_LITE, "LITE_FAST"},
    {"EU_N_868", "EU 868 Narrow", MESH_PROFILE_NARROW, "NARROW_SLOW"},
    {"CN", "China", MESH_PROFILE_STD, "LONG_FAST"},
    {"JP", "Japan", MESH_PROFILE_STD, "LONG_FAST"},
    {"ANZ", "ANZ 915", MESH_PROFILE_STD, "LONG_FAST"},
    {"ANZ_433", "ANZ 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"RU", "Russia", MESH_PROFILE_STD, "LONG_FAST"},
    {"KR", "Korea", MESH_PROFILE_STD, "LONG_FAST"},
    {"TW", "Taiwan", MESH_PROFILE_STD, "LONG_FAST"},
    {"IN", "India", MESH_PROFILE_STD, "LONG_FAST"},
    {"NZ_865", "NZ 865", MESH_PROFILE_STD, "LONG_FAST"},
    {"TH", "Thailand", MESH_PROFILE_STD, "LONG_FAST"},
    {"UA_433", "Ukraine 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"UA_868", "Ukraine 868", MESH_PROFILE_STD, "LONG_FAST"},
    {"MY_433", "Malaysia 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"MY_919", "Malaysia 919", MESH_PROFILE_STD, "LONG_FAST"},
    {"SG_923", "Singapore 923", MESH_PROFILE_STD, "LONG_FAST"},
    {"PH_433", "Philippines 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"PH_868", "Philippines 868", MESH_PROFILE_STD, "LONG_FAST"},
    {"PH_915", "Philippines 915", MESH_PROFILE_STD, "LONG_FAST"},
    {"KZ_433", "Kazakhstan 433", MESH_PROFILE_STD, "LONG_FAST"},
    {"KZ_863", "Kazakhstan 863", MESH_PROFILE_STD, "LONG_FAST"},
    {"NP_865", "Nepal 865", MESH_PROFILE_STD, "LONG_FAST"},
    {"BR_902", "Brazil 902", MESH_PROFILE_STD, "LONG_FAST"},
    {"ITU1_2M", "ITU1 2m", MESH_PROFILE_HAM_20KHZ, "TINY_FAST"},
    {"ITU2_2M", "ITU2 2m", MESH_PROFILE_HAM_20KHZ, "TINY_FAST"},
    {"ITU3_2M", "ITU3 2m", MESH_PROFILE_HAM_20KHZ, "TINY_FAST"},
    {"ITU2_125CM", "ITU2 1.25m", MESH_PROFILE_HAM_100KHZ, "NARROW_SLOW"},
    {"ITU1_70CM", "ITU1 70cm", MESH_PROFILE_HAM_100KHZ, "NARROW_SLOW"},
    {"ITU2_70CM", "ITU2 70cm", MESH_PROFILE_HAM_100KHZ, "NARROW_SLOW"},
    {"ITU3_70CM", "ITU3 70cm", MESH_PROFILE_HAM_100KHZ, "NARROW_SLOW"},
    {"LORA_24", "LoRa 2.4GHz", MESH_PROFILE_STD, "LONG_FAST"},
};

static const mesh_preset_choice_t mesh_preset_choices[] = {
    {"LONG_FAST", "Long Fast",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"LONG_SLOW", "Long Slow",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"MEDIUM_SLOW", "Medium Slow",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"MEDIUM_FAST", "Medium Fast",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"SHORT_SLOW", "Short Slow",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"SHORT_FAST", "Short Fast",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"LONG_MODERATE", "Long Moderate",
     MESH_PROFILE_MASK(MESH_PROFILE_STD) |
         MESH_PROFILE_MASK(MESH_PROFILE_EU868)},
    {"SHORT_TURBO", "Short Turbo", MESH_PROFILE_MASK(MESH_PROFILE_STD)},
    {"LONG_TURBO", "Long Turbo", MESH_PROFILE_MASK(MESH_PROFILE_STD)},
    {"MEDIUM_TURBO", "Medium Turbo", MESH_PROFILE_MASK(MESH_PROFILE_STD)},
    {"LITE_FAST", "Lite Fast", MESH_PROFILE_MASK(MESH_PROFILE_LITE)},
    {"LITE_SLOW", "Lite Slow", MESH_PROFILE_MASK(MESH_PROFILE_LITE)},
    {"NARROW_FAST", "Narrow Fast",
     MESH_PROFILE_MASK(MESH_PROFILE_NARROW) |
         MESH_PROFILE_MASK(MESH_PROFILE_HAM_100KHZ)},
    {"NARROW_SLOW", "Narrow Slow",
     MESH_PROFILE_MASK(MESH_PROFILE_NARROW) |
         MESH_PROFILE_MASK(MESH_PROFILE_HAM_100KHZ)},
    {"TINY_FAST", "Tiny Fast", MESH_PROFILE_MASK(MESH_PROFILE_HAM_20KHZ)},
    {"TINY_SLOW", "Tiny Slow", MESH_PROFILE_MASK(MESH_PROFILE_HAM_20KHZ)},
};

static const mesh_choice_t mesh_power_choices[] = {
    {"auto", "Auto"},
    {"-9", "-9 dBm"},
    {"0", "0 dBm"},
    {"5", "5 dBm"},
    {"10", "10 dBm"},
    {"14", "14 dBm"},
    {"17", "17 dBm"},
    {"20", "20 dBm"},
    {"22", "22 dBm"},
};

static const mesh_choice_t mesh_slot_choices[] = {
    {"auto", "Auto (name hash)"},
    {"1", "Slot 1"},
    {"2", "Slot 2"},
    {"3", "Slot 3"},
    {"4", "Slot 4"},
    {"5", "Slot 5"},
    {"6", "Slot 6"},
    {"7", "Slot 7"},
    {"8", "Slot 8"},
    {"9", "Slot 9"},
    {"10", "Slot 10"},
    {"11", "Slot 11"},
    {"12", "Slot 12"},
    {"13", "Slot 13"},
    {"14", "Slot 14"},
    {"15", "Slot 15"},
    {"16", "Slot 16"},
    {"17", "Slot 17"},
    {"18", "Slot 18"},
    {"19", "Slot 19"},
    {"20", "Slot 20"},
};

static const mesh_choice_t mesh_hop_choices[] = {
    {"0", "0 hop"},
    {"1", "1 hop"},
    {"2", "2 hops"},
    {"3", "3 hops"},
    {"4", "4 hops"},
    {"5", "5 hops"},
    {"6", "6 hops"},
    {"7", "7 hops"},
};

static const mesh_choice_t mesh_bool_choices[] = {
    {"1", "On"},
    {"0", "Off"},
};

static const mesh_choice_t mesh_position_interval_choices[] = {
    {"300", "5 min"},
    {"900", "15 min"},
    {"1800", "30 min"},
    {"3600", "60 min"},
};

static const mesh_choice_t mesh_photo_repeat_choices[] = {
    {"1", "Fast (1x)"},
    {"2", "Balanced (2x)"},
    {"3", "Robust (3x)"},
};

static const mesh_choice_t mesh_photo_repair_round_choices[] = {
    {"0", "Off"},
    {"1", "1 round"},
    {"2", "Balanced (2 rounds)"},
    {"3", "Robust (3 rounds)"},
    {"4", "Max (4 rounds)"},
};

static const mesh_choice_t mesh_photo_repair_repeat_choices[] = {
    {"1", "Fast (1x)"},
    {"2", "Balanced (2x)"},
    {"3", "Robust (3x)"},
};

static const mesh_choice_t mesh_photo_repair_window_choices[] = {
    {"3000", "3 sec"},
    {"7000", "7 sec"},
    {"12000", "12 sec"},
    {"20000", "20 sec"},
};

static const mesh_choice_t mesh_photo_cache_ttl_choices[] = {
    {"60", "1 min"},
    {"300", "5 min"},
    {"900", "15 min"},
};

static void mesh_settings_refresh(void);
static void mesh_settings_status_summary(const char *status, char *line1,
                                         size_t line1_len, char *line2,
                                         size_t line2_len, char *line3,
                                         size_t line3_len);
static void mesh_log_close(void);
static mesh_choice_profile_t mesh_current_profile(void);
static const char *mesh_default_preset_for_region(const char *region_value);
static int mesh_profile_supports_ui_preset(mesh_choice_profile_t profile,
                                           const char *preset);

static int mesh_write_all(int fd, const char *data, size_t len)
{
    while(len > 0U) {
        ssize_t rc = send(fd, data, len, MSG_NOSIGNAL);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(rc == 0) {
            return -1;
        }
        data += rc;
        len -= (size_t)rc;
    }
    return 0;
}

static int mesh_read_first_line(const char *path, char *buf, size_t len)
{
    FILE *fp;
    size_t n;

    if(!path || !buf || len == 0U) {
        return -1;
    }
    buf[0] = '\0';
    fp = fopen(path, "r");
    if(!fp) {
        return -1;
    }
    if(!fgets(buf, (int)len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    n = strlen(buf);
    while(n > 0U && (buf[n - 1U] == '\n' || buf[n - 1U] == '\r' ||
                     buf[n - 1U] == ' ' || buf[n - 1U] == '\t')) {
        buf[--n] = '\0';
    }
    return buf[0] ? 0 : -1;
}

static void mesh_auto_node_name(char *buf, size_t len)
{
    char mac[64];
    char compact[13];
    size_t out = 0;

    if(!buf || len == 0U) {
        return;
    }
    if(mesh_read_first_line("/sys/class/net/eth0/address", mac,
                            sizeof(mac)) != 0 &&
       mesh_read_first_line("/sys/class/net/wlan0/address", mac,
                            sizeof(mac)) != 0) {
        snprintf(buf, len, "k230-t-display");
        return;
    }
    for(size_t i = 0; mac[i] && out < sizeof(compact) - 1U; i++) {
        if(isxdigit((unsigned char)mac[i])) {
            compact[out++] = (char)tolower((unsigned char)mac[i]);
        }
    }
    compact[out] = '\0';
    if(out >= 4U) {
        snprintf(buf, len, "k230-%s", compact + out - 4U);
    } else {
        snprintf(buf, len, "k230-t-display");
    }
}

static int mesh_node_name_is_default(const char *name)
{
    if(!name || !name[0] || strcmp(name, "k230-t-display") == 0) {
        return 1;
    }
    if(strlen(name) == 9U && strncmp(name, "k230-", 5) == 0) {
        for(size_t i = 5; i < 9; i++) {
            if(!isxdigit((unsigned char)name[i])) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int mesh_ipc_command(const char *command, char *response,
                            size_t response_len)
{
    struct sockaddr_un addr;
    struct timeval tv;
    int fd;
    size_t used = 0;

    if(response && response_len > 0U) {
        response[0] = '\0';
    }
    if(!command || !response || response_len == 0U) {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        snprintf(response, response_len, "socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", MESHTASTIC_SOCKET_PATH);
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(response, response_len, "daemon offline: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if(mesh_write_all(fd, command, strlen(command)) != 0) {
        snprintf(response, response_len, "command send failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);

    while(used + 1U < response_len) {
        ssize_t n = recv(fd, response + used, response_len - used - 1U, 0);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            break;
        }
        if(n == 0) {
            break;
        }
        used += (size_t)n;
    }
    response[used] = '\0';
    close(fd);
    return used > 0U ? 0 : -1;
}

static void mesh_ui_trace(const char *fmt, ...)
{
    struct timeval tv;
    FILE *fp;
    va_list ap;

    fp = fopen(MESHTASTIC_UI_TRACE_LOG, "a");
    if(!fp) {
        return;
    }
    gettimeofday(&tv, NULL);
    fprintf(fp, "[%ld.%03ld] ", (long)tv.tv_sec,
            (long)(tv.tv_usec / 1000));
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static void mesh_append_log(const char *fmt, ...)
{
    char line[384];
    size_t current;
    size_t add_len;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    current = strlen(mesh_log_text);
    add_len = strlen(line) + 1U;
    if(current + add_len + 1U >= sizeof(mesh_log_text)) {
        size_t keep = sizeof(mesh_log_text) / 2U;
        if(current > keep) {
            memmove(mesh_log_text, mesh_log_text + current - keep, keep + 1U);
            current = strlen(mesh_log_text);
        } else {
            mesh_log_text[0] = '\0';
            current = 0;
        }
    }

    snprintf(mesh_log_text + current, sizeof(mesh_log_text) - current,
             "%s\n", line);
    if(mesh_log_label && lv_obj_is_valid(mesh_log_label)) {
        lv_label_set_text(mesh_log_label, mesh_log_text);
    }
}

static int mesh_status_is_online(const char *status)
{
    return status && strncmp(status, "OK ", 3) == 0;
}

int ui_meshtastic_is_running(void)
{
    char response[512];

    if(mesh_ipc_command("STATUS\n", response, sizeof(response)) != 0) {
        return 0;
    }
    return mesh_status_is_online(response);
}

static void mesh_status_field(const char *status, const char *key,
                              char *out, size_t out_len,
                              const char *fallback)
{
    const char *p;
    size_t key_len;

    if(!out || out_len == 0U) {
        return;
    }
    snprintf(out, out_len, "%s", fallback ? fallback : "");
    if(!status || !key || !key[0]) {
        return;
    }
    key_len = strlen(key);
    p = status;
    while((p = strstr(p, key)) != NULL) {
        if((p == status || isspace((unsigned char)p[-1])) &&
           p[key_len] == '=') {
            size_t n = 0;
            p += key_len + 1U;
            while(p[n] && !isspace((unsigned char)p[n]) &&
                  n + 1U < out_len) {
                out[n] = p[n];
                n++;
            }
            out[n] = '\0';
            return;
        }
        p += key_len;
    }
}

static float mesh_status_float_field(const char *status, const char *key,
                                     float fallback)
{
    char value[24];
    char *end = NULL;
    float parsed;

    mesh_status_field(status, key, value, sizeof(value), "");
    if(!value[0]) {
        return fallback;
    }
    parsed = strtof(value, &end);
    if(end == value) {
        return fallback;
    }
    return parsed;
}

static void mesh_update_voice_capability(const char *status, int online)
{
    char voice[24];
    int available;

    mesh_status_field(status, "voice", voice, sizeof(voice), "disabled");
    available = online && strcmp(voice, "flrc") == 0;
    if(mesh_voice_available != available) {
        mesh_voice_available = available;
        mesh_layout_main();
    } else {
        mesh_voice_available = available;
    }
    if(mesh_voice_button && lv_obj_is_valid(mesh_voice_button)) {
        if(mesh_voice_available) {
            lv_obj_clear_flag(mesh_voice_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(mesh_voice_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(mesh_voice_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(mesh_voice_button, LV_STATE_DISABLED);
        }
    }
}

static void mesh_update_photo_capability(const char *status, int online)
{
    char photo[24];
    int available;

    mesh_status_field(status, "photo", photo, sizeof(photo), "disabled");
    available = online && strcmp(photo, "flrc") == 0;
    if(mesh_photo_available != available) {
        mesh_photo_available = available;
        mesh_layout_main();
    } else {
        mesh_photo_available = available;
    }
    if(mesh_photo_button && lv_obj_is_valid(mesh_photo_button)) {
        if(mesh_photo_available) {
            lv_obj_clear_flag(mesh_photo_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(mesh_photo_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(mesh_photo_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(mesh_photo_button, LV_STATE_DISABLED);
        }
    }
}

static void mesh_refresh_tx_channel_from_daemon(int online)
{
    char response[2048];
    char *saveptr = NULL;
    char *line;
    int selected_found = 0;
    int primary_index = 0;
    char primary_name[64] = "";
    char selected_name[64] = "";

    if(mesh_tx_channel_index < 0 ||
       mesh_tx_channel_index >= MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
        mesh_tx_channel_index = 0;
    }
    if(!online || mesh_ipc_command("CHANNELS\n", response,
                                   sizeof(response)) != 0 ||
       strncmp(response, "OK channels", 11) != 0) {
        snprintf(mesh_tx_channel_name, sizeof(mesh_tx_channel_name), "%s",
                 mesh_channel_name[0] ? mesh_channel_name : "default");
        mesh_update_target_button();
        return;
    }

    line = strtok_r(response, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "OK channels", 11) == 0) {
            char primary[8];
            unsigned long value;

            mesh_status_field(line, "primary", primary, sizeof(primary), "0");
            if(mesh_parse_u32_text(primary, &value) == 0 &&
               value < MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
                primary_index = (int)value;
            }
        } else if(strncmp(line, "CH ", 3) == 0) {
            char index_text[8];
            char role[16];
            char name[64];
            unsigned long value;

            mesh_status_field(line, "index", index_text, sizeof(index_text), "");
            mesh_status_field(line, "role", role, sizeof(role), "disabled");
            mesh_status_field(line, "name", name, sizeof(name), "default");
            if(mesh_parse_u32_text(index_text, &value) == 0 &&
               value < MESHTASTIC_UI_CHANNEL_SLOT_MAX &&
               strcmp(role, "disabled") != 0) {
                if((int)value == primary_index) {
                    snprintf(primary_name, sizeof(primary_name), "%s", name);
                }
                if((int)value == mesh_tx_channel_index) {
                    selected_found = 1;
                    snprintf(selected_name, sizeof(selected_name), "%s", name);
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    if(selected_found) {
        snprintf(mesh_tx_channel_name, sizeof(mesh_tx_channel_name), "%s",
                 selected_name[0] ? selected_name : "default");
    } else {
        mesh_tx_channel_index = primary_index;
        snprintf(mesh_tx_channel_name, sizeof(mesh_tx_channel_name), "%s",
                 primary_name[0] ? primary_name :
                 (mesh_channel_name[0] ? mesh_channel_name : "default"));
        mesh_save_profile_prefs();
    }
    mesh_update_target_button();
}

static void mesh_apply_ble_status(const char *status, int online)
{
    char ble_state[32];

    if(!ui_ble_meshtastic_bridge_enabled()) {
        snprintf(ble_state, sizeof(ble_state), "%s", "offline");
    } else {
        mesh_status_field(status, "ble", ble_state, sizeof(ble_state),
                          "offline");
    }
    if(!online) {
        snprintf(ble_state, sizeof(ble_state), "%s", "offline");
    }
    app_set_ble_status(ble_state);
    if(strcmp(mesh_last_ble_state, ble_state) != 0) {
        mesh_append_log("BLE bridge: %s", ble_state);
        snprintf(mesh_last_ble_state, sizeof(mesh_last_ble_state), "%s",
                 ble_state);
    }
}

static void mesh_pairing_notice_close_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_pairing_overlay && lv_obj_is_valid(mesh_pairing_overlay)) {
        lv_obj_delete(mesh_pairing_overlay);
    }
    mesh_pairing_overlay = NULL;
    app_request_fast_refresh();
}

static void mesh_close_pairing_notice(void)
{
    if(mesh_pairing_overlay && lv_obj_is_valid(mesh_pairing_overlay)) {
        lv_obj_delete(mesh_pairing_overlay);
    }
    mesh_pairing_overlay = NULL;
}

static int mesh_pairing_code_is_valid(const char *code)
{
    size_t len;

    if(!code || !code[0] || strcmp(code, "-") == 0) {
        return 0;
    }
    len = strlen(code);
    if(len != 6U) {
        return 0;
    }
    for(size_t i = 0; i < len; i++) {
        if(!isdigit((unsigned char)code[i])) {
            return 0;
        }
    }
    return 1;
}

static void mesh_show_pairing_notice(const char *code)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *passkey;
    lv_obj_t *hint;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int panel_w = ui_is_landscape() ? 500 : ui_fit_width(lv_layer_top(), 24, 420);
    int panel_h = ui_is_landscape() ? 268 : 304;

    if(!mesh_pairing_code_is_valid(code)) {
        return;
    }
    if(strcmp(mesh_last_pairing_code, code) == 0) {
        return;
    }
    snprintf(mesh_last_pairing_code, sizeof(mesh_last_pairing_code), "%s", code);
    mesh_close_pairing_notice();

    if(panel_w > screen_w - 48) {
        panel_w = screen_w - 48;
    }
    if(panel_w < 300) {
        panel_w = screen_w - 24;
    }
    if(panel_h > screen_h - 48) {
        panel_h = screen_h - 48;
    }

    mesh_pairing_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_pairing_overlay);
    lv_obj_set_style_bg_color(mesh_pairing_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_pairing_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(mesh_pairing_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_pairing_overlay, 0, 0);
    lv_obj_clear_flag(mesh_pairing_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_panel(mesh_pairing_overlay, 0, 0, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263342), 0);
    lv_obj_set_style_pad_all(panel, 22, 0);

    title = ui_label(panel, ui_tr("BLE Pairing"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_width(title, panel_w - 44);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    detail = ui_label(panel, ui_tr("Enter this code in the Meshtastic app"),
                      &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(detail, panel_w - 44);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 0, 48);

    passkey = ui_label(panel, code, &lv_font_montserrat_32, 0x25C281);
    lv_obj_set_width(passkey, panel_w - 44);
    lv_obj_set_style_text_align(passkey, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(passkey, LV_ALIGN_TOP_LEFT, 0, 100);

    hint = ui_label(panel, ui_tr("If the app does not prompt again, forget the old Bluetooth device and reconnect."),
                    &lv_font_montserrat_14, 0x64748B);
    lv_obj_set_width(hint, panel_w - 44);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 154);

    btn = ui_command_button(panel, (panel_w - 156) / 2, panel_h - 74, 156,
                            ui_tr("OK"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_pairing_notice_close_cb, LV_EVENT_CLICKED,
                        NULL);
    app_request_fast_refresh();
}

static void mesh_check_pairing_code(const char *status, int online)
{
    char code[16];
    char ble_state[32];

    mesh_status_field(status, "ble_pair", code, sizeof(code), "-");
    if(online && mesh_pairing_code_is_valid(code)) {
        mesh_show_pairing_notice(code);
        return;
    }
    mesh_status_field(status, "ble", ble_state, sizeof(ble_state), "offline");
    if(!online || strcmp(ble_state, "offline") == 0) {
        mesh_last_pairing_code[0] = '\0';
        mesh_close_pairing_notice();
    }
}

static void mesh_safe_arg(char *dst, size_t dst_len, const char *src)
{
    size_t out = 0;

    if(!dst || dst_len == 0U) {
        return;
    }
    if(!src || !src[0]) {
        snprintf(dst, dst_len, "-");
        return;
    }
    for(size_t i = 0; src[i] && out + 1U < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if(isalnum(c) || c == '_' || c == '-' || c == '.') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '_';
        }
    }
    dst[out] = '\0';
    if(out == 0U) {
        snprintf(dst, dst_len, "-");
    }
}

static void mesh_safe_or_default(char *dst, size_t dst_len, const char *src,
                                 const char *fallback)
{
    mesh_safe_arg(dst, dst_len, src);
    if(strcmp(dst, "-") == 0) {
        snprintf(dst, dst_len, "%s", fallback ? fallback : "");
    }
}

static int mesh_parse_u32_text(const char *text, unsigned long *value)
{
    char *endp = NULL;
    unsigned long v;

    if(!text || !text[0] || !value) {
        return -1;
    }
    errno = 0;
    v = strtoul(text, &endp, 0);
    if(errno != 0 || endp == text || *endp != '\0' ||
       v > 0xffffffffUL) {
        return -1;
    }
    *value = v;
    return 0;
}

static int mesh_parse_i32_text(const char *text, long *value)
{
    char *endp = NULL;
    long v;

    if(!text || !text[0] || !value) {
        return -1;
    }
    errno = 0;
    v = strtol(text, &endp, 0);
    if(errno != 0 || endp == text || *endp != '\0') {
        return -1;
    }
    *value = v;
    return 0;
}

static void mesh_normalize_hop(void)
{
    unsigned long hop;

    if(mesh_parse_u32_text(mesh_hop_limit, &hop) != 0) {
        hop = 3;
    }
    if(hop > 7) {
        hop = 7;
    }
    snprintf(mesh_hop_limit, sizeof(mesh_hop_limit), "%lu", hop);
}

static int mesh_slot_text_is_auto(const char *text)
{
    return !text || !text[0] || strcasecmp(text, "auto") == 0 ||
           strcasecmp(text, "default") == 0 || strcmp(text, "-") == 0 ||
           strcmp(text, "0") == 0;
}

static int mesh_slot_is_auto(void)
{
    return mesh_slot_text_is_auto(mesh_frequency_slot);
}

static void mesh_normalize_slot(void)
{
    unsigned long slot;

    if(mesh_slot_is_auto()) {
        snprintf(mesh_frequency_slot, sizeof(mesh_frequency_slot), "auto");
        return;
    }
    if(mesh_parse_u32_text(mesh_frequency_slot, &slot) != 0 ||
       slot < 1UL || slot > 255UL) {
        snprintf(mesh_frequency_slot, sizeof(mesh_frequency_slot), "auto");
        return;
    }
    snprintf(mesh_frequency_slot, sizeof(mesh_frequency_slot), "%lu", slot);
}

static int mesh_power_text_is_auto(const char *text)
{
    return !text || !text[0] || strcasecmp(text, "auto") == 0 ||
           strcasecmp(text, "default") == 0 || strcmp(text, "-") == 0;
}

static int mesh_power_is_auto(void)
{
    return mesh_power_text_is_auto(mesh_tx_power);
}

static void mesh_normalize_power(void)
{
    long power;

    if(mesh_power_is_auto()) {
        snprintf(mesh_tx_power, sizeof(mesh_tx_power), "auto");
        return;
    }
    if(mesh_parse_i32_text(mesh_tx_power, &power) != 0 ||
       power < -9L || power > 22L) {
        snprintf(mesh_tx_power, sizeof(mesh_tx_power), "auto");
        return;
    }
    snprintf(mesh_tx_power, sizeof(mesh_tx_power), "%ld", power);
}

static void mesh_normalize_u32_text(char *value_text, size_t len,
                                    unsigned long min_value,
                                    unsigned long max_value,
                                    const char *fallback)
{
    unsigned long value;

    if(!value_text || len == 0U ||
       mesh_parse_u32_text(value_text, &value) != 0 ||
       value < min_value || value > max_value) {
        snprintf(value_text, len, "%s", fallback ? fallback : "");
        return;
    }
    snprintf(value_text, len, "%lu", value);
}

static void mesh_normalize_media_config(void)
{
    mesh_normalize_u32_text(mesh_photo_repeat, sizeof(mesh_photo_repeat),
                            1, 3, "2");
    mesh_normalize_u32_text(mesh_photo_repair_rounds,
                            sizeof(mesh_photo_repair_rounds),
                            0, 4, "2");
    mesh_normalize_u32_text(mesh_photo_repair_repeat,
                            sizeof(mesh_photo_repair_repeat),
                            1, 3, "2");
    mesh_normalize_u32_text(mesh_photo_repair_window_ms,
                            sizeof(mesh_photo_repair_window_ms),
                            3000, 20000, "7000");
    mesh_normalize_u32_text(mesh_photo_cache_ttl_sec,
                            sizeof(mesh_photo_cache_ttl_sec),
                            60, 900, "300");
}

static int mesh_from_text_is_auto(const char *text)
{
    return !text || !text[0] || strcasecmp(text, "auto") == 0 ||
           strcasecmp(text, "default") == 0 || strcmp(text, "-") == 0 ||
           strcmp(text, "0") == 0 || strcasecmp(text, "0x0") == 0;
}

static int mesh_to_text_is_broadcast(const char *text)
{
    return !text || !text[0] || strcasecmp(text, "broadcast") == 0 ||
           strcasecmp(text, "default") == 0 || strcmp(text, "-") == 0 ||
           strcasecmp(text, "0xffffffff") == 0;
}

static int mesh_to_text_is_legacy_broadcast(const char *text)
{
    return text && (strcasecmp(text, "0x00ffffff") == 0 ||
                    strcasecmp(text, "0xffffff") == 0);
}

static int mesh_normalize_from_node_text(const char *text, char *dst,
                                         size_t dst_len)
{
    char tmp[96];
    unsigned long value;

    if(!dst || dst_len == 0U) {
        return -1;
    }
    mesh_safe_or_default(tmp, sizeof(tmp), text, "auto");
    if(mesh_from_text_is_auto(tmp)) {
        snprintf(dst, dst_len, "0");
        return 0;
    }
    if(mesh_parse_u32_text(tmp, &value) != 0 || value == 0UL ||
       value == 0xffffffffUL) {
        return -1;
    }
    snprintf(dst, dst_len, "0x%08lx", value & 0xffffffffUL);
    return 0;
}

static int mesh_normalize_to_node_text(const char *text, char *dst,
                                       size_t dst_len)
{
    char tmp[96];
    unsigned long value;

    if(!dst || dst_len == 0U) {
        return -1;
    }
    mesh_safe_or_default(tmp, sizeof(tmp), text, "broadcast");
    if(mesh_to_text_is_broadcast(tmp) ||
       mesh_to_text_is_legacy_broadcast(tmp)) {
        snprintf(dst, dst_len, "0xffffffff");
        return 0;
    }
    if(mesh_parse_u32_text(tmp, &value) != 0 || value == 0UL) {
        return -1;
    }
    snprintf(dst, dst_len, "0x%08lx", value & 0xffffffffUL);
    return 0;
}

static void mesh_normalize_from_node(void)
{
    char normalized[24];

    if(mesh_normalize_from_node_text(mesh_from_node, normalized,
                                     sizeof(normalized)) != 0) {
        snprintf(mesh_from_node, sizeof(mesh_from_node), "0");
        return;
    }
    snprintf(mesh_from_node, sizeof(mesh_from_node), "%s", normalized);
}

static void mesh_normalize_to_node(void)
{
    char normalized[24];

    if(mesh_normalize_to_node_text(mesh_to_node, normalized,
                                   sizeof(normalized)) != 0) {
        snprintf(mesh_to_node, sizeof(mesh_to_node), "0xffffffff");
        mesh_ack_enabled = 0;
        return;
    }
    snprintf(mesh_to_node, sizeof(mesh_to_node), "%s", normalized);
}

static void mesh_normalize_target_ack(void)
{
    mesh_normalize_to_node();
    if(mesh_to_text_is_broadcast(mesh_to_node)) {
        snprintf(mesh_to_node, sizeof(mesh_to_node), "0xffffffff");
        mesh_ack_enabled = 0;
    }
}

static void mesh_update_profile_label(void)
{
    char text[180];

    if(ui_is_landscape()) {
        snprintf(text, sizeof(text),
                 "%s  %s  %s  slot:%s",
                 mesh_region, mesh_preset,
                 mesh_channel_name[0] ? mesh_channel_name : "default",
                 mesh_slot_is_auto() ? "auto" : mesh_frequency_slot);
    } else {
        snprintf(text, sizeof(text),
                 "%s  %s  CH:%s",
                 mesh_region, mesh_preset,
                 mesh_channel_name[0] ? mesh_channel_name : "default");
    }
    if(mesh_profile_label && lv_obj_is_valid(mesh_profile_label)) {
        lv_label_set_text(mesh_profile_label, text);
    }
}

static void mesh_tx_channel_label_text(char *out, size_t len)
{
    const char *name = mesh_tx_channel_name[0] ? mesh_tx_channel_name :
                       (mesh_channel_name[0] ? mesh_channel_name : "default");

    if(!out || len == 0U) {
        return;
    }
    snprintf(out, len, "CH%d %s", mesh_tx_channel_index, name);
}

static void mesh_target_button_text(char *out, size_t len)
{
    char channel[88];

    if(!out || len == 0U) {
        return;
    }
    mesh_tx_channel_label_text(channel, sizeof(channel));
    if(mesh_to_text_is_broadcast(mesh_to_node)) {
        snprintf(out, len, "%s  %s", channel, ui_tr("Channel broadcast"));
    } else {
        snprintf(out, len, "%s  %s %s", channel, ui_tr("Direct message"),
                 mesh_to_node);
    }
}

static void mesh_update_target_button(void)
{
    char text[96];
    uint32_t color;

    if(!app_current_page_is(PAGE_MESHTASTIC)) {
        return;
    }
    if(!mesh_target_label || !lv_obj_is_valid(mesh_target_label)) {
        return;
    }
    mesh_target_button_text(text, sizeof(text));
    color = mesh_to_text_is_broadcast(mesh_to_node) ? 0x25C281 : 0xF5A524;
    lv_label_set_text(mesh_target_label, text);
    lv_obj_set_style_text_color(mesh_target_label, lv_color_hex(color), 0);
    if(mesh_target_button && lv_obj_is_valid(mesh_target_button)) {
        lv_obj_set_style_border_color(mesh_target_button, lv_color_hex(color), 0);
    }
}

static void mesh_load_profile_prefs(void)
{
    ui_prefs_get(MESHTASTIC_PREF_REGION, mesh_region, sizeof(mesh_region),
                 MESHTASTIC_DEFAULT_UI_REGION);
    ui_prefs_get(MESHTASTIC_PREF_PRESET, mesh_preset, sizeof(mesh_preset),
                 MESHTASTIC_DEFAULT_UI_PRESET);
    ui_prefs_get(MESHTASTIC_PREF_CHANNEL, mesh_channel_name,
                 sizeof(mesh_channel_name), "");
    {
        char tx_channel[8];
        unsigned long value;

        ui_prefs_get(MESHTASTIC_PREF_TX_CHANNEL, tx_channel,
                     sizeof(tx_channel), "0");
        if(mesh_parse_u32_text(tx_channel, &value) == 0 &&
           value < MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
            mesh_tx_channel_index = (int)value;
        } else {
            mesh_tx_channel_index = 0;
        }
    }
    ui_prefs_get(MESHTASTIC_PREF_SLOT, mesh_frequency_slot,
                 sizeof(mesh_frequency_slot), "auto");
    ui_prefs_get(MESHTASTIC_PREF_PSK, mesh_psk, sizeof(mesh_psk), "default");
    ui_prefs_get(MESHTASTIC_PREF_POWER, mesh_tx_power,
                 sizeof(mesh_tx_power), "auto");
    ui_prefs_get(MESHTASTIC_PREF_NODE, mesh_node_name,
                 sizeof(mesh_node_name), "k230-t-display");
    if(mesh_node_name_is_default(mesh_node_name)) {
        mesh_auto_node_name(mesh_node_name, sizeof(mesh_node_name));
    }
    ui_prefs_get(MESHTASTIC_PREF_FROM, mesh_from_node,
                 sizeof(mesh_from_node), "0");
    ui_prefs_get(MESHTASTIC_PREF_TO, mesh_to_node,
                 sizeof(mesh_to_node), "0xffffffff");
    ui_prefs_get(MESHTASTIC_PREF_HOP, mesh_hop_limit,
                 sizeof(mesh_hop_limit), "3");
    {
        char ack[8];
        ui_prefs_get(MESHTASTIC_PREF_ACK, ack, sizeof(ack), "0");
        mesh_ack_enabled = strcmp(ack, "0") != 0;
    }
    {
        char rebroadcast[8];
        ui_prefs_get(MESHTASTIC_PREF_REBROADCAST, rebroadcast,
                     sizeof(rebroadcast), "0");
        mesh_rebroadcast_enabled = strcmp(rebroadcast, "0") != 0;
    }
    {
        char position[8];
        ui_prefs_get(MESHTASTIC_PREF_POSITION, position, sizeof(position),
                     "1");
        mesh_position_enabled = strcmp(position, "0") != 0;
    }
    ui_prefs_get(MESHTASTIC_PREF_POSITION_INTERVAL, mesh_position_interval,
                 sizeof(mesh_position_interval), "900");
    {
        char fixed[8];
        ui_prefs_get(MESHTASTIC_PREF_FIXED_POSITION, fixed, sizeof(fixed),
                     "0");
        mesh_fixed_position_enabled = strcmp(fixed, "0") != 0;
    }
    ui_prefs_get(MESHTASTIC_PREF_FIXED_LATITUDE, mesh_fixed_latitude_i,
                 sizeof(mesh_fixed_latitude_i), "0");
    ui_prefs_get(MESHTASTIC_PREF_FIXED_LONGITUDE, mesh_fixed_longitude_i,
                 sizeof(mesh_fixed_longitude_i), "0");
    ui_prefs_get(MESHTASTIC_PREF_FIXED_ALTITUDE, mesh_fixed_altitude_m,
                 sizeof(mesh_fixed_altitude_m), "0");
    {
        char telemetry[8];
        ui_prefs_get(MESHTASTIC_PREF_TELEMETRY, telemetry, sizeof(telemetry),
                     "1");
        mesh_telemetry_enabled = strcmp(telemetry, "0") != 0;
    }
    {
        char telemetry_env[8];
        ui_prefs_get(MESHTASTIC_PREF_TELEMETRY_ENV, telemetry_env,
                     sizeof(telemetry_env), "1");
        mesh_environment_telemetry_enabled =
            strcmp(telemetry_env, "0") != 0;
    }
    ui_prefs_get(MESHTASTIC_PREF_TELEMETRY_DEVICE_INTERVAL,
                 mesh_telemetry_device_interval,
                 sizeof(mesh_telemetry_device_interval), "300");
    ui_prefs_get(MESHTASTIC_PREF_TELEMETRY_ENV_INTERVAL,
                 mesh_telemetry_environment_interval,
                 sizeof(mesh_telemetry_environment_interval), "300");
    ui_prefs_get(MESHTASTIC_PREF_PHOTO_REPEAT, mesh_photo_repeat,
                 sizeof(mesh_photo_repeat), "2");
    ui_prefs_get(MESHTASTIC_PREF_PHOTO_REPAIR_ROUNDS,
                 mesh_photo_repair_rounds,
                 sizeof(mesh_photo_repair_rounds), "2");
    ui_prefs_get(MESHTASTIC_PREF_PHOTO_REPAIR_REPEAT,
                 mesh_photo_repair_repeat,
                 sizeof(mesh_photo_repair_repeat), "2");
    ui_prefs_get(MESHTASTIC_PREF_PHOTO_REPAIR_WINDOW_MS,
                 mesh_photo_repair_window_ms,
                 sizeof(mesh_photo_repair_window_ms), "7000");
    ui_prefs_get(MESHTASTIC_PREF_PHOTO_CACHE_TTL_SEC,
                 mesh_photo_cache_ttl_sec,
                 sizeof(mesh_photo_cache_ttl_sec), "300");
    mesh_normalize_power();
    mesh_normalize_from_node();
    mesh_normalize_hop();
    mesh_normalize_slot();
    mesh_normalize_target_ack();
    mesh_normalize_media_config();
}

static void mesh_save_profile_prefs(void)
{
    mesh_normalize_from_node();
    mesh_normalize_target_ack();

    ui_prefs_set(MESHTASTIC_PREF_REGION, mesh_region);
    ui_prefs_set(MESHTASTIC_PREF_PRESET, mesh_preset);
    ui_prefs_set(MESHTASTIC_PREF_CHANNEL, mesh_channel_name);
    {
        char tx_channel[8];

        if(mesh_tx_channel_index < 0 ||
           mesh_tx_channel_index >= MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
            mesh_tx_channel_index = 0;
        }
        snprintf(tx_channel, sizeof(tx_channel), "%d", mesh_tx_channel_index);
        ui_prefs_set(MESHTASTIC_PREF_TX_CHANNEL, tx_channel);
    }
    ui_prefs_set(MESHTASTIC_PREF_SLOT, mesh_frequency_slot);
    ui_prefs_set(MESHTASTIC_PREF_PSK, mesh_psk);
    ui_prefs_set(MESHTASTIC_PREF_POWER, mesh_tx_power);
    ui_prefs_set(MESHTASTIC_PREF_NODE, mesh_node_name);
    ui_prefs_set(MESHTASTIC_PREF_FROM, mesh_from_node);
    ui_prefs_set(MESHTASTIC_PREF_TO, mesh_to_node);
    ui_prefs_set(MESHTASTIC_PREF_HOP, mesh_hop_limit);
    ui_prefs_set(MESHTASTIC_PREF_ACK, mesh_ack_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_REBROADCAST,
                 mesh_rebroadcast_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_POSITION,
                 mesh_position_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_POSITION_INTERVAL, mesh_position_interval);
    ui_prefs_set(MESHTASTIC_PREF_FIXED_POSITION,
                 mesh_fixed_position_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_FIXED_LATITUDE, mesh_fixed_latitude_i);
    ui_prefs_set(MESHTASTIC_PREF_FIXED_LONGITUDE, mesh_fixed_longitude_i);
    ui_prefs_set(MESHTASTIC_PREF_FIXED_ALTITUDE, mesh_fixed_altitude_m);
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY,
                 mesh_telemetry_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_ENV,
                 mesh_environment_telemetry_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_DEVICE_INTERVAL,
                 mesh_telemetry_device_interval);
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_ENV_INTERVAL,
                 mesh_telemetry_environment_interval);
    mesh_normalize_media_config();
    ui_prefs_set(MESHTASTIC_PREF_PHOTO_REPEAT, mesh_photo_repeat);
    ui_prefs_set(MESHTASTIC_PREF_PHOTO_REPAIR_ROUNDS,
                 mesh_photo_repair_rounds);
    ui_prefs_set(MESHTASTIC_PREF_PHOTO_REPAIR_REPEAT,
                 mesh_photo_repair_repeat);
    ui_prefs_set(MESHTASTIC_PREF_PHOTO_REPAIR_WINDOW_MS,
                 mesh_photo_repair_window_ms);
    ui_prefs_set(MESHTASTIC_PREF_PHOTO_CACHE_TTL_SEC,
                 mesh_photo_cache_ttl_sec);
}

static int mesh_apply_media_runtime_config(void)
{
    char command[256];
    char response[256];
    int ret;

    mesh_normalize_media_config();
    snprintf(command, sizeof(command),
             "MEDIA_CONFIG photo_repeat=%s repair_rounds=%s "
             "repair_repeat=%s repair_window_ms=%s cache_ttl_sec=%s\n",
             mesh_photo_repeat, mesh_photo_repair_rounds,
             mesh_photo_repair_repeat, mesh_photo_repair_window_ms,
             mesh_photo_cache_ttl_sec);
    ret = mesh_ipc_command(command, response, sizeof(response));
    ui_trim_text(response);
    if(ret == 0) {
        mesh_append_log("media config applied: %s", response);
    } else {
        mesh_append_log("media config pending: %s",
                        response[0] ? response : "daemon offline");
    }
    return ret;
}

int ui_meshtastic_autostart_enabled(void)
{
    char value[8];

    ui_prefs_get(MESHTASTIC_PREF_AUTOSTART, value, sizeof(value), "1");
    return strcmp(value, "0") != 0;
}

void ui_meshtastic_set_autostart_enabled(int enabled)
{
    ui_prefs_set(MESHTASTIC_PREF_AUTOSTART, enabled ? "1" : "0");
}

static int mesh_status_value_truthy(const char *value)
{
    return value && (strcmp(value, "1") == 0 ||
                     strcasecmp(value, "on") == 0 ||
                     strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "yes") == 0);
}

static int mesh_status_copy_if_changed(char *dst, size_t dst_len,
                                       const char *value)
{
    char clean[96];

    if(!dst || dst_len == 0U || !value || !value[0] ||
       strcmp(value, "-") == 0) {
        return 0;
    }
    mesh_safe_arg(clean, sizeof(clean), value);
    if(strcmp(clean, "-") == 0 || strcmp(dst, clean) == 0) {
        return 0;
    }
    snprintf(dst, dst_len, "%s", clean);
    return 1;
}

static int mesh_status_sync_channel(const char *value)
{
    char clean[96];

    if(!value || !value[0]) {
        return 0;
    }
    if(strcmp(value, "-") == 0 || strcasecmp(value, "default") == 0 ||
       strcasecmp(value, "<preset>") == 0) {
        if(mesh_channel_name[0]) {
            mesh_channel_name[0] = '\0';
            return 1;
        }
        return 0;
    }
    mesh_safe_arg(clean, sizeof(clean), value);
    if(strcmp(clean, "-") == 0 || strcmp(mesh_channel_name, clean) == 0) {
        return 0;
    }
    snprintf(mesh_channel_name, sizeof(mesh_channel_name), "%s", clean);
    return 1;
}

static int mesh_status_sync_from_node(const char *value)
{
    char normalized[24];

    if(!value || !value[0] || strcmp(value, "-") == 0) {
        return 0;
    }
    if(mesh_normalize_from_node_text(value, normalized,
                                     sizeof(normalized)) != 0) {
        return 0;
    }
    if(strcmp(mesh_from_node, normalized) == 0) {
        return 0;
    }
    snprintf(mesh_from_node, sizeof(mesh_from_node), "%s", normalized);
    return 1;
}

static int mesh_status_sync_to_node(const char *value)
{
    char normalized[24];

    if(!value || !value[0] || strcmp(value, "-") == 0) {
        return 0;
    }
    if(mesh_normalize_to_node_text(value, normalized, sizeof(normalized)) !=
       0) {
        return 0;
    }
    if(strcmp(mesh_to_node, normalized) == 0) {
        return 0;
    }
    snprintf(mesh_to_node, sizeof(mesh_to_node), "%s", normalized);
    if(mesh_to_text_is_broadcast(mesh_to_node)) {
        mesh_ack_enabled = 0;
    }
    return 1;
}

static void mesh_sync_profile_from_status(const char *status, int online)
{
    char value[96];
    int changed = 0;

    if(!online || !status) {
        return;
    }
    if((mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) ||
       (mesh_choice_overlay && lv_obj_is_valid(mesh_choice_overlay))) {
        return;
    }

    mesh_status_field(status, "region", value, sizeof(value), "");
    changed |= mesh_status_copy_if_changed(mesh_region, sizeof(mesh_region),
                                           value);
    mesh_status_field(status, "preset", value, sizeof(value), "");
    changed |= mesh_status_copy_if_changed(mesh_preset, sizeof(mesh_preset),
                                           value);
    mesh_status_field(status, "channel", value, sizeof(value), "");
    changed |= mesh_status_sync_channel(value);
    mesh_status_field(status, "slot", value, sizeof(value), "");
    if(value[0] && strcmp(value, "-") != 0 &&
       strcmp(mesh_frequency_slot, value) != 0) {
        snprintf(mesh_frequency_slot, sizeof(mesh_frequency_slot), "%s", value);
        mesh_normalize_slot();
        changed = 1;
    }
    mesh_status_field(status, "node", value, sizeof(value), "");
    changed |= mesh_status_copy_if_changed(mesh_node_name,
                                           sizeof(mesh_node_name), value);
    mesh_status_field(status, "from", value, sizeof(value), "");
    changed |= mesh_status_sync_from_node(value);
    mesh_status_field(status, "to", value, sizeof(value), "");
    changed |= mesh_status_sync_to_node(value);

    mesh_status_field(status, "want_ack", value, sizeof(value), "");
    if(value[0]) {
        int ack = mesh_status_value_truthy(value);
        if(mesh_ack_enabled != ack) {
            mesh_ack_enabled = ack;
            changed = 1;
        }
    }
    mesh_status_field(status, "relay", value, sizeof(value), "");
    if(value[0]) {
        int relay = mesh_status_value_truthy(value);
        if(mesh_rebroadcast_enabled != relay) {
            mesh_rebroadcast_enabled = relay;
            changed = 1;
        }
    }

    mesh_status_field(status, "manual_power", value, sizeof(value), "");
    if(value[0]) {
        int manual_power = mesh_status_value_truthy(value);
        char power_text[16];

        if(!manual_power) {
            if(strcmp(mesh_tx_power, "auto") != 0) {
                snprintf(mesh_tx_power, sizeof(mesh_tx_power), "%s", "auto");
                changed = 1;
            }
        } else {
            long power;

            mesh_status_field(status, "power", power_text,
                              sizeof(power_text), "");
            if(mesh_parse_i32_text(power_text, &power) == 0 &&
               power >= -9L && power <= 22L) {
                char normalized[8];

                snprintf(normalized, sizeof(normalized), "%ld", power);
                if(strcmp(mesh_tx_power, normalized) != 0) {
                    snprintf(mesh_tx_power, sizeof(mesh_tx_power), "%s",
                             normalized);
                    changed = 1;
                }
            }
        }
    }
    mesh_status_field(status, "photo_repeat", value, sizeof(value), "");
    changed |= mesh_status_copy_if_changed(mesh_photo_repeat,
                                           sizeof(mesh_photo_repeat), value);
    mesh_status_field(status, "photo_repair_rounds", value, sizeof(value),
                      "");
    changed |= mesh_status_copy_if_changed(mesh_photo_repair_rounds,
                                           sizeof(mesh_photo_repair_rounds),
                                           value);
    mesh_status_field(status, "photo_repair_repeat", value, sizeof(value),
                      "");
    changed |= mesh_status_copy_if_changed(mesh_photo_repair_repeat,
                                           sizeof(mesh_photo_repair_repeat),
                                           value);
    mesh_status_field(status, "photo_repair_window_ms", value, sizeof(value),
                      "");
    changed |= mesh_status_copy_if_changed(mesh_photo_repair_window_ms,
                                           sizeof(mesh_photo_repair_window_ms),
                                           value);
    mesh_status_field(status, "photo_cache_ttl_sec", value, sizeof(value),
                      "");
    changed |= mesh_status_copy_if_changed(mesh_photo_cache_ttl_sec,
                                           sizeof(mesh_photo_cache_ttl_sec),
                                           value);
    mesh_normalize_media_config();

    if(changed) {
        mesh_save_profile_prefs();
        mesh_settings_refresh();
        mesh_append_log("profile synced from daemon status");
    }
}

static void mesh_refresh_daemon_log(void)
{
    char response[3072];
    const char *shown;

    if(!mesh_log_label || !lv_obj_is_valid(mesh_log_label)) {
        return;
    }
    if(mesh_ipc_command("LOG\n", response, sizeof(response)) != 0) {
        return;
    }
    shown = response;
    if(strncmp(response, "OK log\n", 7) == 0) {
        shown = response + 7;
    }
    lv_label_set_text(mesh_log_label, shown);
}

static int mesh_chat_extract_meta_field(const char *meta, const char *key,
                                        char *out, size_t out_len)
{
    const char *p;
    size_t key_len;
    size_t i = 0;

    if(out && out_len > 0U) {
        out[0] = '\0';
    }
    if(!meta || !key || !key[0] || !out || out_len == 0U) {
        return 0;
    }
    key_len = strlen(key);
    p = strstr(meta, key);
    if(!p) {
        return 0;
    }
    p += key_len;
    while(*p && !isspace((unsigned char)*p) && *p != ':' &&
          i + 1U < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return out[0] != '\0';
}

static const char *mesh_chat_status_text(const char *status)
{
    if(!status || !status[0]) {
        return "";
    }
    if(strcmp(status, "air") == 0) {
        return ui_tr("Sending");
    }
    if(strcmp(status, "sent") == 0) {
        return ui_tr("Sent");
    }
    if(strcmp(status, "queued") == 0) {
        return ui_tr("Queued");
    }
    if(strcmp(status, "pending") == 0) {
        return ui_tr("Waiting ACK");
    }
    if(strncmp(status, "retry", 5) == 0) {
        return ui_tr("Retrying");
    }
    if(strcmp(status, "ack") == 0) {
        return ui_tr("ACK");
    }
    if(strcmp(status, "relayed") == 0) {
        return ui_tr("Relayed");
    }
    if(strcmp(status, "timeout") == 0) {
        return ui_tr("Timeout");
    }
    if(strcmp(status, "nak") == 0) {
        return ui_tr("NAK");
    }
    if(strcmp(status, "dropped") == 0) {
        return ui_tr("Dropped");
    }
    if(strcmp(status, "tx-failed") == 0) {
        return ui_tr("TX failed");
    }
    if(strcmp(status, "failed") == 0 ||
       strcmp(status, "init-failed") == 0) {
        return ui_tr("Failed");
    }
    return status;
}

static uint32_t mesh_chat_status_color(const char *status, int sent)
{
    if(!sent) {
        return 0x94A3B8;
    }
    if(!status || !status[0]) {
        return 0xDDFCE8;
    }
    if(strcmp(status, "ack") == 0 ||
       strcmp(status, "relayed") == 0 ||
       strcmp(status, "sent") == 0) {
        return 0xDDFCE8;
    }
    if(strcmp(status, "queued") == 0 ||
       strcmp(status, "air") == 0 ||
       strcmp(status, "pending") == 0 ||
       strncmp(status, "retry", 5) == 0) {
        return 0xFDE68A;
    }
    if(strcmp(status, "timeout") == 0 ||
       strcmp(status, "nak") == 0 ||
       strcmp(status, "dropped") == 0 ||
       strcmp(status, "tx-failed") == 0 ||
       strcmp(status, "failed") == 0 ||
       strcmp(status, "init-failed") == 0) {
        return 0xFCA5A5;
    }
    return 0xFDE68A;
}

static const char *mesh_chat_mode_label(const char *mode)
{
    if(mode && strcmp(mode, "direct") == 0) {
        return ui_tr("Direct");
    }
    if(mode && strcmp(mode, "channel") == 0) {
        return ui_tr("Channel");
    }
    if(mode && strcmp(mode, "broadcast") == 0) {
        return ui_tr("Broadcast");
    }
    return "";
}

static int mesh_chat_parse_voice_line(const char *line, char *path,
                                      size_t path_len, char *duration,
                                      size_t duration_len)
{
    const char *voice;
    const char *file;
    const char *p;
    size_t used = 0;

    if(path && path_len > 0U) {
        path[0] = '\0';
    }
    if(duration && duration_len > 0U) {
        duration[0] = '\0';
    }
    if(!line || (strncmp(line, "RX ", 3) != 0 &&
                 strncmp(line, "TX ", 3) != 0)) {
        return 0;
    }
    if(strstr(line, ": ")) {
        return 0;
    }
    voice = strstr(line, " voice ");
    if(!voice) {
        return 0;
    }
    p = voice + 7;
    while(*p && !isspace((unsigned char)*p) && used + 1U < duration_len) {
        duration[used++] = *p++;
    }
    if(duration && duration_len > 0U) {
        duration[used] = '\0';
    }
    file = strstr(line, " file=");
    if(file && path && path_len > 0U) {
        size_t i = 0;

        file += 6;
        while(file[i] && !isspace((unsigned char)file[i]) &&
              i + 1U < path_len) {
            path[i] = file[i];
            i++;
        }
        path[i] = '\0';
        if(strncmp(path, "/tmp/k230_mesh_voice_", 20) != 0) {
            path[0] = '\0';
        }
    }
    return 1;
}

static int mesh_photo_chat_path_allowed(const char *path)
{
    if(!path || !path[0]) {
        return 0;
    }
    if(strncmp(path, "/tmp/k230_mesh_photo_", 20) != 0 &&
       strncmp(path, MESHTASTIC_PHOTO_STORE_DIR "/",
               strlen(MESHTASTIC_PHOTO_STORE_DIR) + 1U) != 0) {
        return 0;
    }
    for(size_t i = 0; path[i]; i++) {
        unsigned char c = (unsigned char)path[i];
        if(isspace(c) || c == '\'' || c == '"' || c == '`' ||
           c == '$' || c == ';' || c == '|') {
            return 0;
        }
    }
    return 1;
}

static const char *mesh_photo_linux_path(const char *path)
{
    if(!path) {
        return "";
    }
    if(strncmp(path, "A:", 2) == 0) {
        return path + 2;
    }
    return path;
}

static int mesh_photo_file_readable(const char *path)
{
    const char *linux_path = mesh_photo_linux_path(path);

    return linux_path[0] && access(linux_path, R_OK) == 0;
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
} mesh_photo_jpeg_error_mgr_t;

static void mesh_photo_jpeg_error_exit(j_common_ptr cinfo)
{
    mesh_photo_jpeg_error_mgr_t *err =
        (mesh_photo_jpeg_error_mgr_t *)cinfo->err;

    longjmp(err->setjmp_buffer, 1);
}

static void mesh_photo_scale_rgb_to_rgb565(const uint8_t *src,
                                           unsigned src_w, unsigned src_h,
                                           uint8_t *dst,
                                           unsigned dst_w, unsigned dst_h)
{
    unsigned draw_w = dst_w;
    unsigned draw_h = dst_h;
    unsigned off_x;
    unsigned off_y;
    uint16_t bg = mesh_rgb565(0x05070A);

    if(!dst || dst_w == 0U || dst_h == 0U) {
        return;
    }
    for(unsigned y = 0U; y < dst_h; y++) {
        for(unsigned x = 0U; x < dst_w; x++) {
            size_t off = ((size_t)y * dst_w + x) * 2U;
            dst[off + 0U] = (uint8_t)(bg & 0xffU);
            dst[off + 1U] = (uint8_t)(bg >> 8U);
        }
    }
    if(!src || src_w == 0U || src_h == 0U) {
        return;
    }
    if((uint64_t)src_w * dst_h > (uint64_t)src_h * dst_w) {
        draw_h = (unsigned)(((uint64_t)src_h * dst_w) / src_w);
        if(draw_h == 0U) {
            draw_h = 1U;
        }
    } else {
        draw_w = (unsigned)(((uint64_t)src_w * dst_h) / src_h);
        if(draw_w == 0U) {
            draw_w = 1U;
        }
    }
    off_x = (dst_w - draw_w) / 2U;
    off_y = (dst_h - draw_h) / 2U;
    for(unsigned y = 0U; y < draw_h; y++) {
        unsigned sy = (unsigned)(((uint64_t)y * src_h) / draw_h);

        if(sy >= src_h) {
            sy = src_h - 1U;
        }
        for(unsigned x = 0U; x < draw_w; x++) {
            unsigned sx = (unsigned)(((uint64_t)x * src_w) / draw_w);
            size_t src_off;
            size_t dst_off;
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint16_t color;

            if(sx >= src_w) {
                sx = src_w - 1U;
            }
            src_off = ((size_t)sy * src_w + sx) * 3U;
            dst_off = ((size_t)(off_y + y) * dst_w + (off_x + x)) * 2U;
            r = src[src_off + 0U];
            g = src[src_off + 1U];
            b = src[src_off + 2U];
            color = (uint16_t)(((uint16_t)(r & 0xf8U) << 8U) |
                               ((uint16_t)(g & 0xfcU) << 3U) |
                               ((uint16_t)b >> 3U));
            dst[dst_off + 0U] = (uint8_t)(color & 0xffU);
            dst[dst_off + 1U] = (uint8_t)(color >> 8U);
        }
    }
}

static int mesh_photo_load_jpeg_rgb565(const char *path, unsigned view_w,
                                       unsigned view_h, uint8_t **pixels,
                                       lv_image_dsc_t *dsc,
                                       unsigned *src_w_out,
                                       unsigned *src_h_out)
{
    FILE *fp = NULL;
    struct jpeg_decompress_struct cinfo;
    mesh_photo_jpeg_error_mgr_t jerr;
    uint8_t *src_rgb = NULL;
    uint8_t *dst = NULL;
    size_t src_stride;
    size_t src_size;
    size_t dst_size;
    int ok = -1;

    if(pixels) {
        *pixels = NULL;
    }
    if(src_w_out) {
        *src_w_out = 0U;
    }
    if(src_h_out) {
        *src_h_out = 0U;
    }
    if(!path || !path[0] || !pixels || !dsc ||
       view_w == 0U || view_h == 0U) {
        return -1;
    }

    fp = fopen(mesh_photo_linux_path(path), "rb");
    if(!fp) {
        return -1;
    }

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = mesh_photo_jpeg_error_exit;
    if(setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        free(src_rgb);
        free(dst);
        fclose(fp);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if(cinfo.output_width == 0U || cinfo.output_height == 0U ||
       cinfo.output_components != 3U) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return -1;
    }

    src_stride = (size_t)cinfo.output_width * 3U;
    src_size = src_stride * cinfo.output_height;
    dst_size = (size_t)view_w * view_h * 2U;
    src_rgb = (uint8_t *)malloc(src_size);
    dst = (uint8_t *)malloc(dst_size);
    if(!src_rgb || !dst) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        free(src_rgb);
        free(dst);
        fclose(fp);
        return -1;
    }

    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row[1];
        row[0] = src_rgb + (size_t)cinfo.output_scanline * src_stride;
        jpeg_read_scanlines(&cinfo, row, 1);
    }
    mesh_photo_scale_rgb_to_rgb565(src_rgb, cinfo.output_width,
                                   cinfo.output_height, dst, view_w,
                                   view_h);
    if(src_w_out) {
        *src_w_out = cinfo.output_width;
    }
    if(src_h_out) {
        *src_h_out = cinfo.output_height;
    }

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = view_w;
    dsc->header.h = view_h;
    dsc->header.stride = view_w * 2U;
    dsc->data_size = dst_size;
    dsc->data = dst;
    *pixels = dst;
    dst = NULL;
    ok = 0;

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(src_rgb);
    free(dst);
    fclose(fp);
    return ok;
}

static int mesh_chat_parse_photo_line(const char *line, int *sent,
                                      char *node, size_t node_len,
                                      char *path, size_t path_len,
                                      char *dims, size_t dims_len)
{
    const char *photo;
    const char *file;
    const char *p;
    size_t used = 0;

    if(sent) {
        *sent = 0;
    }
    if(node && node_len > 0U) {
        node[0] = '\0';
    }
    if(path && path_len > 0U) {
        path[0] = '\0';
    }
    if(dims && dims_len > 0U) {
        dims[0] = '\0';
    }
    if(!line || (strncmp(line, "RX ", 3) != 0 &&
                 strncmp(line, "TX ", 3) != 0)) {
        return 0;
    }
    if(strstr(line, ": ")) {
        return 0;
    }
    if(sent && strncmp(line, "TX ", 3) == 0) {
        *sent = 1;
    }
    p = line + 3;
    while(*p && !isspace((unsigned char)*p) && used + 1U < node_len) {
        if(node) {
            node[used] = *p;
        }
        used++;
        p++;
    }
    if(node && node_len > 0U) {
        node[used < node_len ? used : node_len - 1U] = '\0';
    }
    photo = strstr(line, " photo ");
    if(!photo) {
        return 0;
    }
    p = photo + 7;
    used = 0;
    while(*p && !isspace((unsigned char)*p) && used + 1U < dims_len) {
        if(dims) {
            dims[used] = *p;
        }
        used++;
        p++;
    }
    if(dims && dims_len > 0U) {
        dims[used < dims_len ? used : dims_len - 1U] = '\0';
    }
    file = strstr(line, " file=");
    if(file && path && path_len > 0U) {
        size_t i = 0;

        file += 6;
        while(file[i] && !isspace((unsigned char)file[i]) &&
              i + 1U < path_len) {
            path[i] = file[i];
            i++;
        }
        path[i] = '\0';
        if(!mesh_photo_chat_path_allowed(path)) {
            path[0] = '\0';
        }
    }
    return 1;
}

static void mesh_photo_format_footer(const char *line, int sent,
                                     const char *node, char *footer,
                                     size_t footer_len,
                                     uint32_t *footer_color)
{
    char state[24] = "";
    char packets[16] = "";
    char elapsed[24] = "";
    char sha[72] = "";
    char hash_text[20] = "";
    char packet_text[32] = "";
    char elapsed_text[40] = "";
    const char *state_text = NULL;
    uint32_t color;

    if(!footer || footer_len == 0U) {
        return;
    }
    mesh_chat_extract_meta_field(line, "state=", state, sizeof(state));
    mesh_chat_extract_meta_field(line, "packets=", packets, sizeof(packets));
    mesh_chat_extract_meta_field(line, "elapsed=", elapsed, sizeof(elapsed));
    mesh_chat_extract_meta_field(line, "sha256=", sha, sizeof(sha));
    if(sha[0]) {
        snprintf(hash_text, sizeof(hash_text), " hash %.8s", sha);
    }
    if(packets[0]) {
        snprintf(packet_text, sizeof(packet_text), " %spkt", packets);
    }
    if(elapsed[0]) {
        snprintf(elapsed_text, sizeof(elapsed_text), " %s", elapsed);
    }
    state_text = state[0] ? mesh_chat_status_text(state) :
                 (sent ? ui_tr("Sent") : ui_tr("Received"));
    snprintf(footer, footer_len, "%s %s - %s%s%s%s",
             sent ? "TX" : "RX",
             node && node[0] ? node : "photo",
             state_text, packet_text, elapsed_text, hash_text);
    color = state[0] ? mesh_chat_status_color(state, sent) : 0xDDFCE8;
    if(footer_color) {
        *footer_color = color;
    }
}

static const char *mesh_capability_short(const char *value)
{
    return value && strcmp(value, "flrc") == 0 ? "FLRC" : "-";
}

static int mesh_chat_parse_voice_group_item(const char *line, int *sent,
                                            char *node, size_t node_len,
                                            char *path, size_t path_len,
                                            double *duration_s)
{
    const char *node_start;
    const char *node_end;
    const char *voice;
    const char *file;

    if(sent) {
        *sent = 0;
    }
    if(node && node_len > 0U) {
        node[0] = '\0';
    }
    if(path && path_len > 0U) {
        path[0] = '\0';
    }
    if(duration_s) {
        *duration_s = 0.0;
    }
    if(!line || (strncmp(line, "RX ", 3) != 0 &&
                 strncmp(line, "TX ", 3) != 0)) {
        return 0;
    }
    if(strstr(line, ": ")) {
        return 0;
    }
    if(sent) {
        *sent = strncmp(line, "TX ", 3) == 0;
    }
    node_start = line + 3;
    node_end = node_start;
    while(*node_end && !isspace((unsigned char)*node_end)) {
        node_end++;
    }
    if(node && node_len > 0U) {
        size_t n = (size_t)(node_end - node_start);
        if(n >= node_len) {
            n = node_len - 1U;
        }
        snprintf(node, node_len, "%.*s", (int)n, node_start);
    }
    voice = strstr(line, " voice ");
    if(!voice) {
        return 0;
    }
    if(duration_s) {
        *duration_s = atof(voice + 7);
    }
    file = strstr(line, " file=");
    if(file && path && path_len > 0U) {
        size_t i = 0;

        file += 6;
        while(file[i] && !isspace((unsigned char)file[i]) &&
              i + 1U < path_len) {
            path[i] = file[i];
            i++;
        }
        path[i] = '\0';
        if(strncmp(path, "/tmp/k230_mesh_voice_", 20) != 0) {
            path[0] = '\0';
        }
    }
    return 1;
}

static int mesh_voice_path_is_safe(const char *path)
{
    return path && strncmp(path, "/tmp/k230_mesh_voice_", 20) == 0 &&
           !strchr(path, '\'') && !strchr(path, ';') &&
           !strchr(path, '\n') && !strchr(path, '\r');
}

static int mesh_voice_paths_have_safe_item(const char *paths)
{
    char copy[512];
    char *item;
    char *save = NULL;

    if(!paths || !paths[0]) {
        return 0;
    }
    snprintf(copy, sizeof(copy), "%s", paths);
    item = strtok_r(copy, ";", &save);
    while(item) {
        ui_trim_text(item);
        if(mesh_voice_path_is_safe(item)) {
            return 1;
        }
        item = strtok_r(NULL, ";", &save);
    }
    return 0;
}

static unsigned mesh_voice_duration_text_ms(const char *duration)
{
    double seconds;

    if(!duration || !duration[0]) {
        return 0;
    }
    seconds = atof(duration);
    if(seconds <= 0.0) {
        return 0;
    }
    if(seconds > 60.0) {
        seconds = 60.0;
    }
    return (unsigned)(seconds * 1000.0 + 0.5);
}

static int mesh_voice_playback_poll(void)
{
    int status = 0;
    pid_t rc;

    if(mesh_voice_play_pid <= 0) {
        return 0;
    }
    rc = waitpid(mesh_voice_play_pid, &status, WNOHANG);
    if(rc == 0) {
        return 1;
    }
    if(rc == mesh_voice_play_pid || (rc < 0 && errno == ECHILD)) {
        mesh_voice_play_pid = -1;
        return 0;
    }
    return 1;
}

static void mesh_voice_playing_timer_stop(void)
{
    if(mesh_voice_playing_timer) {
        lv_timer_delete(mesh_voice_playing_timer);
        mesh_voice_playing_timer = NULL;
    }
}

static void mesh_voice_playing_visual_stop(void)
{
    mesh_voice_bubble_ctx_t *ctx = mesh_voice_playing_ctx;

    mesh_voice_playing_timer_stop();
    if(ctx && ctx->label && lv_obj_is_valid(ctx->label)) {
        lv_label_set_text(ctx->label, ctx->idle_text);
    }
    if(ctx && ctx->bubble && lv_obj_is_valid(ctx->bubble)) {
        lv_obj_set_style_border_width(ctx->bubble, 0, 0);
    }
    mesh_voice_playing_ctx = NULL;
    mesh_voice_playing_phase = 0;
    app_request_fast_refresh();
}

static void mesh_voice_stop_playback(int clear_visual)
{
    if(mesh_voice_play_pid > 0) {
        int status = 0;

        if(waitpid(mesh_voice_play_pid, &status, WNOHANG) == 0) {
            kill(-mesh_voice_play_pid, SIGTERM);
            usleep(30000);
            if(waitpid(mesh_voice_play_pid, &status, WNOHANG) == 0) {
                kill(-mesh_voice_play_pid, SIGKILL);
                waitpid(mesh_voice_play_pid, &status, 0);
            }
        }
        mesh_voice_play_pid = -1;
    }
    if(clear_visual) {
        mesh_voice_playing_visual_stop();
    }
}

static void mesh_voice_playback_child(const char *paths)
{
    char copy[512];
    char rate[16];
    char *item;
    char *save = NULL;

    snprintf(copy, sizeof(copy), "%s", paths ? paths : "");
    snprintf(rate, sizeof(rate), "%u", MESHTASTIC_VOICE_SAMPLE_RATE);
    item = strtok_r(copy, ";", &save);
    while(item) {
        pid_t pid;
        int status = 0;

        ui_trim_text(item);
        if(!mesh_voice_path_is_safe(item)) {
            item = strtok_r(NULL, ";", &save);
            continue;
        }
        pid = fork();
        if(pid == 0) {
            execlp("aplay", "aplay", "-q", "-D", "default", "-t", "raw",
                   "-f", "S16_LE", "-c", "1", "-r", rate, item,
                   (char *)NULL);
            _exit(127);
        }
        if(pid > 0) {
            waitpid(pid, &status, 0);
        }
        item = strtok_r(NULL, ";", &save);
    }
    _exit(0);
}

static int mesh_voice_play_pcm_file(const char *path)
{
    pid_t pid;

    if(!mesh_voice_paths_have_safe_item(path)) {
        mesh_append_log("voice playback rejected path=%s",
                        path ? path : "(null)");
        return -1;
    }
    mesh_voice_stop_playback(1);
    pid = fork();
    if(pid < 0) {
        mesh_append_log("voice playback fork failed: %s", strerror(errno));
        return -1;
    }
    if(pid == 0) {
        setpgid(0, 0);
        mesh_voice_playback_child(path);
    }
    setpgid(pid, pid);
    mesh_voice_play_pid = pid;
    mesh_voice_play_start_us = ui_monotonic_us();
    return 0;
}

static void mesh_voice_playing_timer_cb(lv_timer_t *timer)
{
    mesh_voice_bubble_ctx_t *ctx = mesh_voice_playing_ctx;
    const char *waves[] = { ">    ", ">>   ", ">>>  ", ">>>> " };
    char text[128];
    uint64_t elapsed_ms;
    int running;

    (void)timer;
    if(!ctx || !ctx->label || !lv_obj_is_valid(ctx->label)) {
        mesh_voice_playing_visual_stop();
        return;
    }
    running = mesh_voice_playback_poll();
    elapsed_ms = (ui_monotonic_us() - mesh_voice_play_start_us) / 1000ULL;
    if(!running ||
       (ctx->duration_ms > 0U && elapsed_ms > ctx->duration_ms + 2000U)) {
        mesh_voice_stop_playback(0);
        mesh_voice_playing_visual_stop();
        return;
    }
    snprintf(text, sizeof(text), "%s %s",
             waves[mesh_voice_playing_phase %
                   (sizeof(waves) / sizeof(waves[0]))],
             ctx->idle_text);
    mesh_voice_playing_phase++;
    lv_label_set_text(ctx->label, text);
    app_request_fast_refresh();
}

static void mesh_voice_bubble_play(mesh_voice_bubble_ctx_t *ctx)
{
    if(!ctx || !ctx->paths[0]) {
        return;
    }
    if(mesh_voice_play_pcm_file(ctx->paths) != 0) {
        return;
    }
    mesh_voice_playing_ctx = ctx;
    mesh_voice_playing_phase = 0;
    if(ctx->bubble && lv_obj_is_valid(ctx->bubble)) {
        lv_obj_set_style_border_color(ctx->bubble, lv_color_hex(0xBBF7D0), 0);
        lv_obj_set_style_border_width(ctx->bubble, 2, 0);
    }
    mesh_voice_playing_timer =
        lv_timer_create(mesh_voice_playing_timer_cb, 160, ctx);
    mesh_voice_playing_timer_cb(mesh_voice_playing_timer);
}

static void mesh_voice_bubble_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    mesh_voice_bubble_ctx_t *ctx =
        (mesh_voice_bubble_ctx_t *)lv_event_get_user_data(event);

    if(code == LV_EVENT_CLICKED) {
        if(ctx && ctx->paths[0]) {
            mesh_voice_bubble_play(ctx);
            mesh_append_log("voice playback: %s", ctx->paths);
        }
        return;
    }
    if(code == LV_EVENT_DELETE) {
        if(ctx == mesh_voice_playing_ctx) {
            mesh_voice_stop_playback(0);
            mesh_voice_playing_ctx = NULL;
            mesh_voice_playing_timer_stop();
        }
        free(ctx);
    }
}

static void mesh_photo_preview_close(void)
{
    if(mesh_photo_preview_overlay &&
       lv_obj_is_valid(mesh_photo_preview_overlay)) {
        lv_obj_delete(mesh_photo_preview_overlay);
    }
    mesh_photo_preview_overlay = NULL;
    free(mesh_photo_preview_pixels);
    mesh_photo_preview_pixels = NULL;
    memset(&mesh_photo_preview_dsc, 0, sizeof(mesh_photo_preview_dsc));
}

static void mesh_photo_preview_close_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_photo_preview_close();
}

static void mesh_photo_preview_open(const mesh_photo_bubble_ctx_t *ctx)
{
    lv_obj_t *panel;
    lv_obj_t *img;
    lv_obj_t *title;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_is_landscape() ? 22 : 18;
    int panel_w = screen_w - margin * 2;
    int panel_h = screen_h - margin * 2;
    int img_w;
    int img_h;
    int avail_w;
    int avail_h;

    if(!ctx || !ctx->path[0]) {
        return;
    }
    mesh_photo_preview_close();
    mesh_photo_preview_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_photo_preview_overlay);
    lv_obj_set_style_bg_color(mesh_photo_preview_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_photo_preview_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(mesh_photo_preview_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_photo_preview_overlay, 0, 0);
    lv_obj_add_flag(mesh_photo_preview_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mesh_photo_preview_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_panel(mesh_photo_preview_overlay, margin, margin,
                     panel_w, panel_h);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    title = ui_label(panel, ctx->title[0] ? ctx->title : ui_tr("Photo"),
                     &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_width(title, panel_w - 88);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, panel_w - 76, 0, 76,
                            ui_tr("Close"), 0x374151);
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, mesh_photo_preview_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    avail_w = panel_w;
    avail_h = panel_h - 58;
    img_w = avail_w;
    img_h = avail_h;
    if(mesh_photo_load_jpeg_rgb565(ctx->path, (unsigned)avail_w,
                                   (unsigned)avail_h,
                                   &mesh_photo_preview_pixels,
                                   &mesh_photo_preview_dsc, NULL,
                                   NULL) == 0) {
        img = lv_image_create(panel);
        lv_image_set_src(img, &mesh_photo_preview_dsc);
        lv_obj_set_size(img, img_w, img_h);
        lv_obj_set_pos(img, 0, 54);
    } else {
        lv_obj_t *error = ui_label(panel, ui_tr("Photo unavailable"),
                                   &lv_font_montserrat_20, 0xF5A524);
        lv_obj_set_width(error, panel_w);
        lv_obj_set_style_text_align(error, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(error, 0, 54 + (avail_h - 28) / 2);
    }
    app_request_fast_refresh();
}

static void mesh_photo_bubble_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    mesh_photo_bubble_ctx_t *ctx =
        (mesh_photo_bubble_ctx_t *)lv_event_get_user_data(event);

    if(code == LV_EVENT_CLICKED) {
        if(ctx && ctx->path[0]) {
            mesh_photo_preview_open(ctx);
            mesh_append_log("photo preview: %s", ctx->path);
        }
        return;
    }
    if(code == LV_EVENT_DELETE) {
        if(ctx) {
            free(ctx->thumb_pixels);
        }
        free(ctx);
    }
}

static void mesh_chat_format_tx_meta(char *meta, size_t meta_len,
                                     char *status, size_t status_len,
                                     uint32_t *footer_color)
{
    char raw[192];
    char id[32];
    char ack[32];
    char mode[24];
    const char *status_text;
    const char *mode_text;

    if(status && status_len > 0U) {
        status[0] = '\0';
    }
    if(!meta || meta_len == 0U) {
        return;
    }

    snprintf(raw, sizeof(raw), "%s", meta);
    if(!mesh_chat_extract_meta_field(raw, "id=", id, sizeof(id)) ||
       !mesh_chat_extract_meta_field(raw, "ack=", ack, sizeof(ack))) {
        return;
    }
    mode[0] = '\0';
    mesh_chat_extract_meta_field(raw, "mode=", mode, sizeof(mode));
    mode_text = mesh_chat_mode_label(mode);
    status_text = mesh_chat_status_text(ack);
    if(mode_text[0]) {
        snprintf(meta, meta_len, "%s - %s - %s", mode_text, id,
                 status_text);
    } else {
        snprintf(meta, meta_len, "%s - %s", id, status_text);
    }
    if(status && status_len > 0U) {
        snprintf(status, status_len, "%s", ack);
    }
    if(footer_color) {
        *footer_color = mesh_chat_status_color(ack, 1);
    }
}

static void mesh_chat_format_rx_meta(char *meta, size_t meta_len)
{
    char raw[192];
    char from[32] = "";
    char mode[24];
    char rssi[24];
    const char *mode_text;
    const char *p;
    size_t n = 0;

    if(!meta || meta_len == 0U || !meta[0]) {
        return;
    }
    snprintf(raw, sizeof(raw), "%s", meta);
    if(strncmp(raw, "RX ", 3) != 0) {
        return;
    }
    p = raw + 3;
    while(p[n] && !isspace((unsigned char)p[n]) &&
          n + 1U < sizeof(from)) {
        from[n] = p[n];
        n++;
    }
    from[n] = '\0';
    mode[0] = '\0';
    rssi[0] = '\0';
    mesh_chat_extract_meta_field(raw, "mode=", mode, sizeof(mode));
    mesh_chat_extract_meta_field(raw, "rssi=", rssi, sizeof(rssi));
    mode_text = mesh_chat_mode_label(mode);
    if(mode_text[0] && rssi[0]) {
        snprintf(meta, meta_len, "%s - %s - %s", from, mode_text, rssi);
    } else if(mode_text[0]) {
        snprintf(meta, meta_len, "%s - %s", from, mode_text);
    }
}

static void mesh_chat_parse_line(const char *line, int *sent,
                                 char *meta, size_t meta_len,
                                 char *body, size_t body_len,
                                 char *status, size_t status_len,
                                 uint32_t *footer_color)
{
    const char *colon;
    size_t prefix_len;

    if(sent) {
        *sent = 0;
    }
    if(meta && meta_len > 0U) {
        meta[0] = '\0';
    }
    if(body && body_len > 0U) {
        body[0] = '\0';
    }
    if(status && status_len > 0U) {
        status[0] = '\0';
    }
    if(footer_color) {
        *footer_color = 0x94A3B8;
    }
    if(!line || !line[0] || !body || body_len == 0U) {
        return;
    }

    if(strncmp(line, "TX ", 3) == 0) {
        if(sent) {
            *sent = 1;
        }
        colon = strstr(line, ": ");
        if(colon) {
            prefix_len = (size_t)(colon - line);
            if(meta && meta_len > 0U) {
                snprintf(meta, meta_len, "%.*s", (int)prefix_len, line);
                mesh_chat_format_tx_meta(meta, meta_len, status,
                                         status_len, footer_color);
            }
            snprintf(body, body_len, "%s", colon + 2);
            return;
        }
    } else if(strncmp(line, "RX ", 3) == 0) {
        colon = strstr(line, ": ");
        if(colon) {
            prefix_len = (size_t)(colon - line);
            if(meta && meta_len > 0U) {
                snprintf(meta, meta_len, "%.*s", (int)prefix_len, line);
                mesh_chat_format_rx_meta(meta, meta_len);
            }
            snprintf(body, body_len, "%s", colon + 2);
            return;
        }
    }

    if(meta && meta_len > 0U) {
        snprintf(meta, meta_len, "system");
    }
    snprintf(body, body_len, "%s", line);
}

static void mesh_chat_add_empty(void)
{
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *text;
    int page_w;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll)) {
        return;
    }
    lv_obj_update_layout(mesh_chat_scroll);
    page_w = lv_obj_get_width(mesh_chat_scroll);
    if(page_w < 240) {
        page_w = ui_page_panel_width();
    }

    box = lv_obj_create(mesh_chat_scroll);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_top(box, 36, 0);
    lv_obj_set_style_pad_bottom(box, 20, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    icon = ui_label(box, LV_SYMBOL_LIST, &lv_font_montserrat_32, 0x64748B);
    lv_obj_set_width(icon, page_w - 48);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    text = ui_label(box, "No mesh messages yet", &lv_font_montserrat_18,
                    0x94A3B8);
    lv_obj_set_width(text, page_w - 48);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
}

static void mesh_chat_add_bubble(const char *line)
{
    int sent = 0;
    char meta[96];
    char body[256];
    char voice_path[512] = "";
    char voice_duration[24] = "";
    char photo_path[192] = "";
    char photo_lvgl_path[200] = "";
    char photo_dims[32] = "";
    char photo_node[32] = "";
    char photo_footer[128] = "";
    char status[24];
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *text;
    lv_obj_t *footer;
    int page_w;
    int bubble_w;
    int is_voice = 0;
    int is_photo = 0;
    int photo_failed = 0;
    unsigned photo_w = 680U;
    unsigned photo_h = 480U;
    uint32_t footer_color = 0x94A3B8;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll) ||
       !line || !line[0]) {
        return;
    }
    mesh_chat_parse_line(line, &sent, meta, sizeof(meta), body, sizeof(body),
                         status, sizeof(status), &footer_color);
    if(mesh_chat_parse_voice_line(line, voice_path, sizeof(voice_path),
                                  voice_duration, sizeof(voice_duration))) {
        char voice_node[32] = "";
        int voice_sent = 0;

        is_voice = 1;
        snprintf(body, sizeof(body), "%s%s%s",
                 ui_tr("Voice message"),
                 voice_duration[0] ? " " : "",
                 voice_duration);
        if(mesh_chat_parse_voice_group_item(line, &voice_sent,
                                            voice_node, sizeof(voice_node),
                                            NULL, 0, NULL) &&
           voice_node[0]) {
            snprintf(meta, sizeof(meta), "%s %s",
                     voice_sent ? "TX" : "RX", voice_node);
            sent = voice_sent;
        } else {
            snprintf(meta, sizeof(meta), "%s", sent ? "TX voice" : "RX voice");
        }
    } else if(mesh_chat_parse_photo_line(line, &sent, photo_node,
                                         sizeof(photo_node), photo_path,
                                         sizeof(photo_path), photo_dims,
                                         sizeof(photo_dims))) {
        is_photo = 1;
        if(strstr(line, "incomplete")) {
            photo_failed = 1;
            photo_path[0] = '\0';
            snprintf(body, sizeof(body), "%s",
                     ui_tr("Photo receive incomplete"));
        } else if(strstr(line, "failed") || strstr(line, "unavailable") ||
                  strstr(line, "crc")) {
            photo_failed = 1;
            photo_path[0] = '\0';
            snprintf(body, sizeof(body), "%s", ui_tr("Photo transfer failed"));
        } else if(photo_path[0] && !mesh_photo_file_readable(photo_path)) {
            photo_failed = 1;
            photo_path[0] = '\0';
            snprintf(body, sizeof(body), "%s", ui_tr("Photo unavailable"));
        } else if(sscanf(photo_dims, "%ux%u", &photo_w, &photo_h) != 2 ||
           photo_w == 0U || photo_h == 0U) {
            photo_w = 680U;
            photo_h = 480U;
        }
        if(!photo_failed) {
            snprintf(body, sizeof(body), "%s%s%s",
                     ui_tr("Photo message"),
                     photo_dims[0] ? " " : "",
                     photo_dims);
            snprintf(photo_lvgl_path, sizeof(photo_lvgl_path), "%s",
                     photo_path);
        }
        mesh_photo_format_footer(line, sent, photo_node, photo_footer,
                                 sizeof(photo_footer), &footer_color);
        snprintf(meta, sizeof(meta), "%s",
                 photo_footer[0] ? photo_footer :
                 (sent ? "TX photo" : "RX photo"));
    }
    ui_trim_text(body);
    ui_trim_text(meta);
    if(!body[0]) {
        return;
    }

    lv_obj_update_layout(mesh_chat_scroll);
    page_w = lv_obj_get_width(mesh_chat_scroll);
    if(page_w < 260) {
        page_w = ui_page_panel_width();
    }
    bubble_w = (page_w * 72) / 100;
    if(bubble_w < 260) {
        bubble_w = page_w > 300 ? 260 : page_w - 28;
    }
    if(bubble_w > 620) {
        bubble_w = 620;
    }

    row = lv_obj_create(mesh_chat_scroll);
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
                              lv_color_hex((is_voice || is_photo) ? 0x16A34A :
                                           (sent ? 0x16A34A : 0x232B35)), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 5, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    if(photo_lvgl_path[0]) {
        mesh_photo_bubble_ctx_t *ctx =
            (mesh_photo_bubble_ctx_t *)calloc(1, sizeof(*ctx));
        int max_img_w = bubble_w - 20;
        int max_img_h = ui_is_landscape() ? 160 : 220;
        int img_w = max_img_w;
        int img_h = photo_w > 0U ?
            (int)(((uint64_t)photo_h * (uint64_t)img_w) / photo_w) :
            max_img_h;

        if(img_h < 1) {
            img_h = 1;
        }
        if(img_h > max_img_h) {
            img_h = max_img_h;
            img_w = photo_h > 0U ?
                (int)(((uint64_t)photo_w * (uint64_t)img_h) / photo_h) :
                max_img_w;
            if(img_w < 1) {
                img_w = 1;
            }
        }
        if(ctx &&
           mesh_photo_load_jpeg_rgb565(photo_lvgl_path, (unsigned)img_w,
                                       (unsigned)img_h, &ctx->thumb_pixels,
                                       &ctx->thumb_dsc, &ctx->width,
                                       &ctx->height) == 0) {
            lv_obj_t *img = lv_image_create(bubble);

            lv_image_set_src(img, &ctx->thumb_dsc);
            lv_obj_set_size(img, img_w, img_h);
            lv_obj_set_style_radius(img, 6, 0);
            lv_obj_set_style_clip_corner(img, true, 0);
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE |
                            LV_OBJ_FLAG_EVENT_BUBBLE);
            snprintf(ctx->path, sizeof(ctx->path), "%s", photo_lvgl_path);
            snprintf(ctx->title, sizeof(ctx->title), "%s", body);
            lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bubble, mesh_photo_bubble_event_cb,
                                LV_EVENT_ALL, ctx);
        } else {
            free(ctx);
            snprintf(body, sizeof(body), "%s", ui_tr("Photo unavailable"));
            mesh_append_log("photo decode failed: %s", photo_lvgl_path);
        }
    }

    text = ui_label(bubble, body, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_set_width(text, bubble_w - 20);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    if(voice_path[0]) {
        mesh_voice_bubble_ctx_t *ctx =
            (mesh_voice_bubble_ctx_t *)calloc(1, sizeof(*ctx));

        if(ctx) {
            snprintf(ctx->paths, sizeof(ctx->paths), "%s", voice_path);
            snprintf(ctx->idle_text, sizeof(ctx->idle_text), "%s", body);
            ctx->duration_ms = mesh_voice_duration_text_ms(voice_duration);
            ctx->bubble = bubble;
            ctx->label = text;
            lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(bubble, mesh_voice_bubble_event_cb,
                                LV_EVENT_ALL, ctx);
        }
    }

    footer = ui_label(bubble, meta[0] ? meta : (sent ? "TX" : "RX"),
                      &lv_font_montserrat_14,
                      footer_color);
    lv_obj_set_width(footer, bubble_w - 20);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);
}

static void mesh_chat_add_voice_group(const char *node, double duration_s,
                                      const char *paths)
{
    char line[768];

    if(!node || !node[0] || !paths || !paths[0]) {
        return;
    }
    snprintf(line, sizeof(line),
             "RX %s voice %.1fs codec=codec2 file=%s",
             node, duration_s, paths);
    mesh_chat_add_bubble(line);
}

static void mesh_chat_rebuild(const char *shown)
{
    char copy[3072];
    char *line;
    char *save = NULL;
    int count = 0;
    int voice_group_active = 0;
    char voice_group_node[32] = "";
    char voice_group_paths[512] = "";
    double voice_group_duration = 0.0;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll)) {
        return;
    }
    lv_obj_clean(mesh_chat_scroll);
    if(!shown || !shown[0]) {
        mesh_chat_add_empty();
        return;
    }

    snprintf(copy, sizeof(copy), "%s", shown);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        ui_trim_text(line);
        if(line[0] && strcmp(line, "No mesh messages yet") != 0) {
            int voice_sent = 0;
            char voice_node[32] = "";
            char voice_path[160] = "";
            double voice_duration = 0.0;

            if(mesh_chat_parse_voice_group_item(line, &voice_sent,
                                                voice_node,
                                                sizeof(voice_node),
                                                voice_path,
                                                sizeof(voice_path),
                                                &voice_duration) &&
               !voice_sent && voice_path[0]) {
                if(voice_group_active &&
                   strcmp(voice_group_node, voice_node) == 0 &&
                   strlen(voice_group_paths) + strlen(voice_path) + 2U <
                       sizeof(voice_group_paths)) {
                    strncat(voice_group_paths, ";",
                            sizeof(voice_group_paths) -
                            strlen(voice_group_paths) - 1U);
                    strncat(voice_group_paths, voice_path,
                            sizeof(voice_group_paths) -
                            strlen(voice_group_paths) - 1U);
                    voice_group_duration += voice_duration;
                } else {
                    if(voice_group_active) {
                        mesh_chat_add_voice_group(voice_group_node,
                                                  voice_group_duration,
                                                  voice_group_paths);
                        count++;
                    }
                    voice_group_active = 1;
                    snprintf(voice_group_node, sizeof(voice_group_node),
                             "%s", voice_node);
                    snprintf(voice_group_paths, sizeof(voice_group_paths),
                             "%s", voice_path);
                    voice_group_duration = voice_duration;
                }
                line = strtok_r(NULL, "\n", &save);
                continue;
            }
            if(voice_group_active) {
                mesh_chat_add_voice_group(voice_group_node,
                                          voice_group_duration,
                                          voice_group_paths);
                voice_group_active = 0;
                voice_group_node[0] = '\0';
                voice_group_paths[0] = '\0';
                voice_group_duration = 0.0;
                count++;
            }
            mesh_chat_add_bubble(line);
            count++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if(voice_group_active) {
        mesh_chat_add_voice_group(voice_group_node,
                                  voice_group_duration,
                                  voice_group_paths);
        count++;
    }
    if(count == 0) {
        mesh_chat_add_empty();
    } else {
        lv_obj_t *last = lv_obj_get_child(mesh_chat_scroll, count - 1);
        if(last) {
            lv_obj_scroll_to_view(last, LV_ANIM_OFF);
        }
    }
}

static int mesh_chat_near_bottom(void)
{
    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll)) {
        return 1;
    }

    lv_obj_update_layout(mesh_chat_scroll);
    return lv_obj_get_scroll_bottom(mesh_chat_scroll) <= 36;
}

static int mesh_chat_append_tail(const char *old_text, const char *new_text,
                                 const char **tail)
{
    size_t old_len;

    if(tail) {
        *tail = NULL;
    }
    if(!old_text || !old_text[0] || !new_text || !tail) {
        return 0;
    }

    old_len = strlen(old_text);
    if(strncmp(old_text, new_text, old_len) != 0 ||
       new_text[old_len] == '\0') {
        return 0;
    }

    if(new_text[old_len] == '\n') {
        *tail = new_text + old_len + 1;
    } else if(old_len > 0 && old_text[old_len - 1] == '\n') {
        *tail = new_text + old_len;
    } else {
        return 0;
    }

    return (*tail && (*tail)[0]) ? 1 : 0;
}

static int mesh_chat_append_lines(const char *lines, int auto_scroll)
{
    char copy[3072];
    char *line;
    char *save = NULL;
    int appended = 0;
    int voice_group_active = 0;
    char voice_group_node[32] = "";
    char voice_group_paths[512] = "";
    double voice_group_duration = 0.0;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll) ||
       !lines || !lines[0]) {
        return 0;
    }

    snprintf(copy, sizeof(copy), "%s", lines);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        ui_trim_text(line);
        if(line[0] && strcmp(line, "No mesh messages yet") != 0) {
            int voice_sent = 0;
            char voice_node[32] = "";
            char voice_path[160] = "";
            double voice_duration = 0.0;

            if(mesh_chat_parse_voice_group_item(line, &voice_sent,
                                                voice_node,
                                                sizeof(voice_node),
                                                voice_path,
                                                sizeof(voice_path),
                                                &voice_duration) &&
               !voice_sent && voice_path[0]) {
                if(voice_group_active &&
                   strcmp(voice_group_node, voice_node) == 0 &&
                   strlen(voice_group_paths) + strlen(voice_path) + 2U <
                       sizeof(voice_group_paths)) {
                    strncat(voice_group_paths, ";",
                            sizeof(voice_group_paths) -
                            strlen(voice_group_paths) - 1U);
                    strncat(voice_group_paths, voice_path,
                            sizeof(voice_group_paths) -
                            strlen(voice_group_paths) - 1U);
                    voice_group_duration += voice_duration;
                } else {
                    if(voice_group_active) {
                        mesh_chat_add_voice_group(voice_group_node,
                                                  voice_group_duration,
                                                  voice_group_paths);
                        appended++;
                    }
                    voice_group_active = 1;
                    snprintf(voice_group_node, sizeof(voice_group_node),
                             "%s", voice_node);
                    snprintf(voice_group_paths, sizeof(voice_group_paths),
                             "%s", voice_path);
                    voice_group_duration = voice_duration;
                }
                line = strtok_r(NULL, "\n", &save);
                continue;
            }
            if(voice_group_active) {
                mesh_chat_add_voice_group(voice_group_node,
                                          voice_group_duration,
                                          voice_group_paths);
                voice_group_active = 0;
                voice_group_node[0] = '\0';
                voice_group_paths[0] = '\0';
                voice_group_duration = 0.0;
                appended++;
            }
            mesh_chat_add_bubble(line);
            appended++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if(voice_group_active) {
        mesh_chat_add_voice_group(voice_group_node, voice_group_duration,
                                  voice_group_paths);
        appended++;
    }

    if(appended > 0 && auto_scroll) {
        int child_count;
        lv_obj_t *last;

        lv_obj_update_layout(mesh_chat_scroll);
        child_count = lv_obj_get_child_count(mesh_chat_scroll);
        last = child_count > 0 ? lv_obj_get_child(mesh_chat_scroll,
                                                  child_count - 1) : NULL;
        if(last) {
            lv_obj_scroll_to_view(last, LV_ANIM_ON);
        }
    }
    return appended;
}

static int mesh_chat_line_exists(const char *text, const char *line)
{
    const char *pos;
    size_t len;

    if(!text || !line || !line[0]) {
        return 0;
    }

    len = strlen(line);
    pos = text;
    while((pos = strstr(pos, line)) != NULL) {
        int before_ok = pos == text || pos[-1] == '\n';
        int after_ok = pos[len] == '\0' || pos[len] == '\n';

        if(before_ok && after_ok) {
            return 1;
        }
        pos++;
    }
    return 0;
}

static int mesh_chat_has_new_rx(const char *old_text, const char *new_text)
{
    char copy[3072];
    char *save = NULL;
    char *line;

    if(!old_text || !old_text[0] || !new_text || !new_text[0]) {
        return 0;
    }

    snprintf(copy, sizeof(copy), "%s", new_text);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        ui_trim_text(line);
        if(strncmp(line, "RX ", 3) == 0 &&
           !mesh_chat_line_exists(old_text, line)) {
            return 1;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return 0;
}

static void mesh_chat_latest_new_rx(const char *old_text, const char *new_text,
                                    char *out, size_t out_len)
{
    char copy[3072];
    char *save = NULL;
    char *line;

    if(out && out_len > 0U) {
        out[0] = '\0';
    }
    if(!old_text || !old_text[0] || !new_text || !new_text[0] ||
       !out || out_len == 0U) {
        return;
    }

    snprintf(copy, sizeof(copy), "%s", new_text);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        ui_trim_text(line);
        if(strncmp(line, "RX ", 3) == 0 &&
           !mesh_chat_line_exists(old_text, line)) {
            snprintf(out, out_len, "%s", line);
        }
        line = strtok_r(NULL, "\n", &save);
    }
}

static void mesh_notification_close(void)
{
    if(mesh_notification_timer) {
        lv_timer_delete(mesh_notification_timer);
        mesh_notification_timer = NULL;
    }
    if(mesh_notification_toast && lv_obj_is_valid(mesh_notification_toast)) {
        lv_obj_delete(mesh_notification_toast);
    }
    mesh_notification_toast = NULL;
}

static void mesh_notification_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    mesh_notification_close();
}

static void mesh_notification_open_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_notification_close();
    app_nav_to_page(PAGE_MESHTASTIC);
}

static void mesh_show_incoming_notification(const char *line)
{
    lv_obj_t *toast;
    lv_obj_t *accent;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_anim_t anim;
    int screen_w = ui_screen_width();
    int w = screen_w - 32;
    int h = ui_is_landscape() ? 72 : 84;
    int y = ui_is_landscape() ? 44 : 54;

    if(app_current_page_is(PAGE_MESHTASTIC)) {
        return;
    }
    if(w < 280) {
        w = screen_w - 16;
    }
    mesh_notification_close();

    toast = ui_panel(lv_layer_top(), 16, -h - 8, w, h);
    mesh_notification_toast = toast;
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast, lv_color_hex(0x1F3B2E), 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_shadow_width(toast, 18, 0);
    lv_obj_set_style_shadow_color(toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(toast, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toast, mesh_notification_open_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(toast);

    accent = lv_obj_create(toast);
    lv_obj_set_size(accent, 4, h - 24);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accent, 2, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(accent);

    title = ui_label(toast, "Meshtastic", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_pos(title, 18, 4);
    lv_obj_set_width(title, w - 44);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    ui_make_click_forwarder(title);

    body = ui_label(toast,
                    line && line[0] ? line : ui_tr("Incoming Meshtastic message"),
                    &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(body, 18, 34);
    lv_obj_set_width(body, w - 44);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    ui_make_click_forwarder(body);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, toast);
    lv_anim_set_values(&anim, -h - 8, y);
    lv_anim_set_time(&anim, 220);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&anim);

    mesh_notification_timer = lv_timer_create(mesh_notification_timer_cb,
                                              4200, NULL);
    lv_timer_set_repeat_count(mesh_notification_timer, 1);
    app_request_fast_refresh();
}

static void mesh_refresh_chat_common(int update_ui, int notify_background)
{
    char response[MESHTASTIC_UI_CHAT_MAX];
    const char *shown;
    const char *append_lines = NULL;
    char latest_rx[256];
    int has_new_rx;
    int can_append;
    int was_near_bottom;

    if(mesh_ipc_command("CHAT\n", response, sizeof(response)) != 0) {
        return;
    }
    shown = response;
    if(strncmp(response, "OK chat\n", 8) == 0) {
        shown = response + 8;
    }
    if(strcmp(mesh_last_chat_text, shown) == 0) {
        if(update_ui && mesh_chat_scroll && lv_obj_is_valid(mesh_chat_scroll) &&
           lv_obj_get_child_count(mesh_chat_scroll) <= 1) {
            mesh_chat_rebuild(shown);
        }
        return;
    }
    has_new_rx = mesh_chat_has_new_rx(mesh_last_chat_text, shown);
    mesh_chat_latest_new_rx(mesh_last_chat_text, shown, latest_rx,
                            sizeof(latest_rx));
    was_near_bottom = update_ui ? mesh_chat_near_bottom() : 0;
    can_append = mesh_chat_append_tail(mesh_last_chat_text, shown,
                                       &append_lines);
    snprintf(mesh_last_chat_text, sizeof(mesh_last_chat_text), "%s", shown);
    if(update_ui && mesh_chat_scroll && lv_obj_is_valid(mesh_chat_scroll)) {
        if(can_append &&
           mesh_chat_append_lines(append_lines, was_near_bottom) > 0) {
            app_request_fast_refresh();
        } else {
            mesh_chat_rebuild(shown);
        }
    }
    if(has_new_rx) {
        ui_audio_play_notification();
        if(notify_background) {
            mesh_show_incoming_notification(latest_rx);
        }
    }
}

static void mesh_layout_main(void)
{
    int body_h;
    int x;
    int content_w;
    int landscape = ui_is_landscape();
    int input_h = ui_is_landscape() ? 58 : 66;
    int bottom_pad = ui_is_landscape() ? 8 : 12;
    int available_h;
    int input_y;
    int target_y;
    int target_h = ui_is_landscape() ? 36 : 34;
    int chat_y;
    int chat_h;
    int send_w = ui_is_landscape() ? 90 : 82;
    int canned_w = ui_is_landscape() ? 52 : 56;
    int photo_w = ui_is_landscape() ? 52 : 56;
    int voice_w = ui_is_landscape() ? 56 : 58;
    int photo_enabled = mesh_photo_available;
    int voice_enabled = mesh_voice_available;
    int input_gap = 8;
    int textarea_w;

    if(!mesh_body || !lv_obj_is_valid(mesh_body)) {
        return;
    }

    mesh_status_panel_h = landscape ? 116 : 184;
    mesh_chat_gap = landscape ? 8 : 10;

    body_h = lv_obj_get_height(mesh_body);
    if(body_h <= 0) {
        body_h = ui_body_height(ui_is_landscape() ? 64 : 124);
    }
    x = ui_page_panel_x();
    content_w = ui_page_panel_width();
    available_h = body_h - mesh_keyboard_reserved_h;
    if(available_h < mesh_status_panel_h + input_h + 180) {
        available_h = mesh_status_panel_h + input_h + 180;
    }
    input_y = available_h - input_h - bottom_pad;
    target_y = input_y - target_h - 6;
    chat_y = mesh_status_panel_h + mesh_chat_gap;
    chat_h = target_y - chat_y - mesh_chat_gap;
    if(chat_h < 140) {
        chat_h = 140;
    }

    if(mesh_status_panel && lv_obj_is_valid(mesh_status_panel)) {
        int panel_pad = 12;
        int inner_w = content_w - panel_pad * 2;
        int action_gap = landscape ? 8 : 8;
        int action_count = 5;
        int action_h = landscape ? 44 : 40;
        int action_w = landscape ? 64 :
            (inner_w - action_gap * (action_count - 1)) / action_count;
        int action_y = landscape ? 4 : 132;
        int action_x0 = landscape ?
            panel_pad + inner_w - action_count * action_w -
            action_gap * (action_count - 1) : panel_pad;
        int text_w = landscape ? action_x0 - panel_pad - 14 : inner_w;

        if(text_w < 160) {
            text_w = 160;
        }
        lv_obj_set_pos(mesh_status_panel, x, 0);
        lv_obj_set_size(mesh_status_panel, content_w, mesh_status_panel_h);
        if(mesh_status_label && lv_obj_is_valid(mesh_status_label)) {
            lv_obj_set_pos(mesh_status_label, panel_pad, landscape ? 0 : 6);
            lv_obj_set_width(mesh_status_label, text_w);
        }
        if(mesh_profile_label && lv_obj_is_valid(mesh_profile_label)) {
            lv_obj_set_pos(mesh_profile_label, panel_pad, landscape ? 28 : 34);
            lv_obj_set_width(mesh_profile_label, text_w);
        }
        if(mesh_detail_label && lv_obj_is_valid(mesh_detail_label)) {
            lv_obj_set_pos(mesh_detail_label, panel_pad, landscape ? 52 : 60);
            lv_obj_set_width(mesh_detail_label, text_w);
        }
        if(mesh_airtime_label && lv_obj_is_valid(mesh_airtime_label)) {
            lv_obj_set_pos(mesh_airtime_label, panel_pad, landscape ? 76 : 88);
            lv_obj_set_width(mesh_airtime_label, text_w);
        }
        if(mesh_chutil_bar && lv_obj_is_valid(mesh_chutil_bar)) {
            lv_obj_set_pos(mesh_chutil_bar, panel_pad, landscape ? 100 : 112);
            lv_obj_set_size(mesh_chutil_bar, text_w, 6);
        }
        if(action_w < 54) {
            action_w = 54;
        }
        if(mesh_channel_button && lv_obj_is_valid(mesh_channel_button)) {
            lv_obj_set_pos(mesh_channel_button, action_x0, action_y);
            lv_obj_set_size(mesh_channel_button, action_w, action_h);
        }
        if(mesh_tx_channel_button && lv_obj_is_valid(mesh_tx_channel_button)) {
            lv_obj_set_pos(mesh_tx_channel_button,
                           action_x0 + (action_w + action_gap), action_y);
            lv_obj_set_size(mesh_tx_channel_button, action_w, action_h);
        }
        if(mesh_map_button && lv_obj_is_valid(mesh_map_button)) {
            lv_obj_set_pos(mesh_map_button,
                           action_x0 + (action_w + action_gap) * 2, action_y);
            lv_obj_set_size(mesh_map_button, action_w, action_h);
        }
        if(mesh_nodes_button && lv_obj_is_valid(mesh_nodes_button)) {
            lv_obj_set_pos(mesh_nodes_button,
                           action_x0 + (action_w + action_gap) * 3, action_y);
            lv_obj_set_size(mesh_nodes_button, action_w, action_h);
        }
        if(mesh_settings_button && lv_obj_is_valid(mesh_settings_button)) {
            lv_obj_set_pos(mesh_settings_button,
                           action_x0 + (action_w + action_gap) * 4, action_y);
            lv_obj_set_size(mesh_settings_button, action_w, action_h);
        }
    }
    if(mesh_chat_scroll && lv_obj_is_valid(mesh_chat_scroll)) {
        lv_obj_set_pos(mesh_chat_scroll, x, chat_y);
        lv_obj_set_size(mesh_chat_scroll, content_w, chat_h);
    }
    if(mesh_target_button && lv_obj_is_valid(mesh_target_button)) {
        lv_obj_set_pos(mesh_target_button, x, target_y);
        lv_obj_set_size(mesh_target_button, content_w, target_h);
        if(mesh_target_label && lv_obj_is_valid(mesh_target_label)) {
            lv_obj_set_width(mesh_target_label, content_w - 24);
        }
    }
    if(mesh_input_panel && lv_obj_is_valid(mesh_input_panel)) {
        lv_obj_set_pos(mesh_input_panel, x, input_y);
        lv_obj_set_size(mesh_input_panel, content_w, input_h);
    }
    if(mesh_canned_button && lv_obj_is_valid(mesh_canned_button)) {
        lv_obj_set_pos(mesh_canned_button, 0, 0);
        lv_obj_set_size(mesh_canned_button, canned_w, input_h - 2);
    }
    if(mesh_photo_button && lv_obj_is_valid(mesh_photo_button)) {
        if(photo_enabled) {
            lv_obj_clear_flag(mesh_photo_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(mesh_photo_button, canned_w + input_gap, 0);
            lv_obj_set_size(mesh_photo_button, photo_w, input_h - 2);
        } else {
            lv_obj_add_flag(mesh_photo_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(mesh_voice_button && lv_obj_is_valid(mesh_voice_button)) {
        if(voice_enabled) {
            lv_obj_clear_flag(mesh_voice_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(mesh_voice_button,
                           canned_w + input_gap +
                           (photo_enabled ? photo_w + input_gap : 0), 0);
            lv_obj_set_size(mesh_voice_button, voice_w, input_h - 2);
        } else {
            lv_obj_add_flag(mesh_voice_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(mesh_textarea && lv_obj_is_valid(mesh_textarea)) {
        int text_x = canned_w + input_gap;
        int gaps = 2;
        if(photo_enabled) {
            text_x += photo_w + input_gap;
            gaps++;
        }
        if(voice_enabled) {
            text_x += voice_w + input_gap;
            gaps++;
        }
        textarea_w = content_w - canned_w - send_w - input_gap * gaps -
                     (voice_enabled ? voice_w : 0) -
                     (photo_enabled ? photo_w : 0);
        if(textarea_w < 180) {
            textarea_w = 180;
        }
        lv_obj_set_pos(mesh_textarea, text_x, 0);
        lv_obj_set_size(mesh_textarea, textarea_w, input_h - 2);
    }
    if(mesh_send_button && lv_obj_is_valid(mesh_send_button)) {
        lv_obj_set_pos(mesh_send_button, content_w - send_w, 0);
        lv_obj_set_size(mesh_send_button, send_w, input_h - 2);
    }
}

static void mesh_inline_layout_cb(int active, int reserved_h, void *user_data)
{
    (void)active;
    (void)user_data;
    mesh_keyboard_reserved_h = reserved_h;
    mesh_layout_main();
}

static void mesh_refresh_status(void)
{
    char response[4096];
    int online;

    if(mesh_ipc_command("STATUS\n", response, sizeof(response)) == 0) {
        snprintf(mesh_status_text, sizeof(mesh_status_text), "%s", response);
    } else {
        snprintf(mesh_status_text, sizeof(mesh_status_text), "%s", response);
    }
    ui_trim_text(mesh_status_text);
    online = mesh_status_is_online(mesh_status_text);
    mesh_apply_ble_status(mesh_status_text, online);
    mesh_check_pairing_code(mesh_status_text, online);
    mesh_sync_profile_from_status(mesh_status_text, online);
    mesh_update_voice_capability(mesh_status_text, online);
    mesh_update_photo_capability(mesh_status_text, online);
    mesh_refresh_tx_channel_from_daemon(online);

    if(mesh_status_label && lv_obj_is_valid(mesh_status_label)) {
        lv_label_set_text(mesh_status_label,
                          ui_is_landscape() ?
                          (online ? ui_tr("Daemon online") :
                           ui_tr("Daemon offline")) :
                          (online ? ui_tr("Online") : ui_tr("Offline")));
        lv_obj_set_style_text_color(mesh_status_label,
                                    lv_color_hex(online ? 0x25C281 : 0xF5A524),
                                    0);
    }
    if(mesh_detail_label && lv_obj_is_valid(mesh_detail_label)) {
        char detail[360];
        char ack_pending[16];
        char ack_rx[16];
        char nak_rx[16];
        char ack_retry[16];
        char ack_timeout[16];
        char ack_drop[16];
        char queued_count[16];
        char nrf9151[24];
        char gps[24];
        char sats[16];
        char position_tx[16];
        char telemetry_tx[16];
        float ch_value;
        float air_value;
        float duty_value;
        uint32_t airtime_color = 0x25C281;
        const char *airtime_state = "Voice OK";

        mesh_status_field(mesh_status_text, "queued_count", queued_count,
                          sizeof(queued_count), "0");
        mesh_status_field(mesh_status_text, "ack_pending", ack_pending,
                          sizeof(ack_pending), "0");
        mesh_status_field(mesh_status_text, "ack_rx", ack_rx, sizeof(ack_rx),
                          "0");
        mesh_status_field(mesh_status_text, "nak_rx", nak_rx, sizeof(nak_rx),
                          "0");
        mesh_status_field(mesh_status_text, "ack_retry", ack_retry,
                          sizeof(ack_retry), "0");
        mesh_status_field(mesh_status_text, "ack_timeout", ack_timeout,
                          sizeof(ack_timeout), "0");
        mesh_status_field(mesh_status_text, "ack_drop", ack_drop,
                          sizeof(ack_drop), "0");
        mesh_status_field(mesh_status_text, "nrf9151", nrf9151,
                          sizeof(nrf9151), "-");
        mesh_status_field(mesh_status_text, "gps", gps, sizeof(gps), "-");
        mesh_status_field(mesh_status_text, "sats", sats, sizeof(sats), "0");
        mesh_status_field(mesh_status_text, "position_tx", position_tx,
                          sizeof(position_tx), "0");
        mesh_status_field(mesh_status_text, "telemetry_tx", telemetry_tx,
                          sizeof(telemetry_tx), "0");
        ch_value = mesh_status_float_field(mesh_status_text, "ch_util", 0.0f);
        air_value = mesh_status_float_field(mesh_status_text, "air_tx", 0.0f);
        duty_value = mesh_status_float_field(mesh_status_text, "duty", 100.0f);
        if(ch_value >= 40.0f) {
            airtime_color = 0xEF4D5A;
            airtime_state = "Channel busy";
        } else if(ch_value >= 25.0f) {
            airtime_color = 0xF5A524;
            airtime_state = "Voice limited";
        }
        lv_obj_set_style_text_color(mesh_detail_label,
                                    lv_color_hex(0x94A3B8), 0);
        mesh_update_target_button();
        if(ui_is_landscape()) {
            snprintf(detail, sizeof(detail),
                     "GPS %s/%s S%s TX%s  TEL%s  Q%s ACK %s P%s/R%s/N%s/RT%s/TO%s/D%s",
                     nrf9151, gps, sats, position_tx,
                     telemetry_tx, queued_count,
                     mesh_ack_enabled ? "on" : "off",
                     ack_pending, ack_rx, nak_rx, ack_retry, ack_timeout,
                     ack_drop);
        } else {
            snprintf(detail, sizeof(detail),
                     "GPS %s/%s S%s  Q%s  ACK %s",
                     nrf9151, gps, sats, queued_count,
                     mesh_ack_enabled ? "on" : "off");
        }
        lv_label_set_text(mesh_detail_label, detail);
        if(mesh_airtime_label && lv_obj_is_valid(mesh_airtime_label)) {
            char airtime[192];
            char repair_req_tx[16];
            char repair_complete[16];
            char repair_fail[16];
            char photo_drop_hits[16];
            char voice_mode[24];
            char photo_mode[24];
            char drop_suffix[32] = "";
            unsigned long repair_req_count;
            unsigned long repair_ok_count;
            unsigned long repair_fail_count;
            unsigned long photo_drop_count;

            mesh_status_field(mesh_status_text, "photo_repair_req_tx",
                              repair_req_tx, sizeof(repair_req_tx), "0");
            mesh_status_field(mesh_status_text, "photo_repair_complete",
                              repair_complete, sizeof(repair_complete), "0");
            mesh_status_field(mesh_status_text, "photo_repair_fail",
                              repair_fail, sizeof(repair_fail), "0");
            mesh_status_field(mesh_status_text, "photo_drop_hits",
                              photo_drop_hits, sizeof(photo_drop_hits), "0");
            mesh_status_field(mesh_status_text, "voice", voice_mode,
                              sizeof(voice_mode), "disabled");
            mesh_status_field(mesh_status_text, "photo", photo_mode,
                              sizeof(photo_mode), "disabled");
            repair_req_count = strtoul(repair_req_tx, NULL, 10);
            repair_ok_count = strtoul(repair_complete, NULL, 10);
            repair_fail_count = strtoul(repair_fail, NULL, 10);
            photo_drop_count = strtoul(photo_drop_hits, NULL, 10);
            if(photo_drop_count > 0UL) {
                snprintf(drop_suffix, sizeof(drop_suffix), "  %s %lu",
                         ui_tr("Drop"), photo_drop_count);
            }
            if(!ui_is_landscape()) {
                if(repair_req_count > 0UL || repair_ok_count > 0UL ||
                   repair_fail_count > 0UL || photo_drop_count > 0UL) {
                    snprintf(airtime, sizeof(airtime),
                             "Ch %.1f%%  TX %.1f%%  R %lu/%lu/%lu%s",
                             ch_value, air_value,
                             repair_req_count, repair_ok_count,
                             repair_fail_count, drop_suffix);
                } else {
                    snprintf(airtime, sizeof(airtime),
                             "Ch %.1f%%  TX %.1f%%  %s",
                             ch_value, air_value, airtime_state);
                }
            } else if(repair_req_count > 0UL || repair_ok_count > 0UL ||
               repair_fail_count > 0UL || photo_drop_count > 0UL) {
                snprintf(airtime, sizeof(airtime),
                         "Ch %.1f%%  TX %.2f/%.1f%%  %s  %s V:%s P:%s  %s %lu/%lu/%lu%s",
                         ch_value, air_value, duty_value, airtime_state,
                         ui_tr("Media"),
                         mesh_capability_short(voice_mode),
                         mesh_capability_short(photo_mode),
                         ui_tr("Repair"),
                         repair_req_count, repair_ok_count,
                         repair_fail_count, drop_suffix);
            } else {
                snprintf(airtime, sizeof(airtime),
                         "Ch %.1f%%  TX %.2f/%.1f%%  %s  %s V:%s P:%s",
                         ch_value, air_value, duty_value, airtime_state,
                         ui_tr("Media"),
                         mesh_capability_short(voice_mode),
                         mesh_capability_short(photo_mode));
            }
            lv_label_set_text(mesh_airtime_label, airtime);
            lv_obj_set_style_text_color(mesh_airtime_label,
                                        lv_color_hex(airtime_color), 0);
        }
        if(mesh_chutil_bar && lv_obj_is_valid(mesh_chutil_bar)) {
            int bar_value = (int)(ch_value + 0.5f);

            if(bar_value < 0) {
                bar_value = 0;
            } else if(bar_value > 40) {
                bar_value = 40;
            }
            lv_bar_set_value(mesh_chutil_bar, bar_value, LV_ANIM_ON);
            lv_obj_set_style_bg_color(mesh_chutil_bar,
                                      lv_color_hex(airtime_color),
                                      LV_PART_INDICATOR);
        }
    }
    if(mesh_send_button && lv_obj_is_valid(mesh_send_button)) {
        if(online) {
            lv_obj_clear_state(mesh_send_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(mesh_send_button, LV_STATE_DISABLED);
        }
    }
    if(online) {
        int in_mesh_page = app_current_page_is(PAGE_MESHTASTIC);

        mesh_refresh_chat_common(in_mesh_page, !in_mesh_page);
        mesh_refresh_daemon_log();
    }
}

static int32_t mesh_overlay_scroll_y(lv_obj_t *panel)
{
    if(panel && lv_obj_is_valid(panel)) {
        return lv_obj_get_scroll_y(panel);
    }
    return 0;
}

static void mesh_overlay_restore_scroll(lv_obj_t *panel, int32_t scroll_y)
{
    if(panel && lv_obj_is_valid(panel) && scroll_y > 0) {
        lv_obj_scroll_to_y(panel, scroll_y, LV_ANIM_OFF);
    }
}

static int mesh_overlay_is_valid(lv_obj_t *overlay)
{
    return overlay && lv_obj_is_valid(overlay);
}

static void mesh_overlay_auto_refresh_tick(void)
{
    if(!app_current_page_is(PAGE_MESHTASTIC)) {
        mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        mesh_detector_auto_refresh_ticks =
            MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        return;
    }

    if(mesh_overlay_is_valid(mesh_nodes_overlay) &&
       mesh_nodes_overlay_kind == MESH_NODES_OVERLAY_LIST) {
        if(--mesh_nodes_auto_refresh_ticks <= 0) {
            mesh_nodes_auto_refresh_ticks =
                MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
            mesh_nodes_preserve_scroll = 1;
            mesh_nodes_event_cb(NULL);
            mesh_nodes_preserve_scroll = 0;
        }
        mesh_detector_auto_refresh_ticks =
            MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        return;
    }
    if(mesh_overlay_is_valid(mesh_nodes_overlay) &&
       mesh_nodes_overlay_kind == MESH_NODES_OVERLAY_DETAIL) {
        if(mesh_node_detail_refresh_attempts > 0) {
            if(--mesh_node_detail_refresh_ticks <= 0) {
                mesh_node_detail_refresh_ticks =
                    MESHTASTIC_NODE_DETAIL_REFRESH_TICKS;
                mesh_node_detail_refresh_attempts--;
                mesh_node_detail_refresh_current();
            }
        }
        mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        mesh_detector_auto_refresh_ticks =
            MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        return;
    }
    mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_node_detail_refresh_attempts = 0;

    if(mesh_overlay_is_valid(mesh_detector_overlay) &&
       !mesh_overlay_is_valid(mesh_nodes_overlay)) {
        if(--mesh_detector_auto_refresh_ticks <= 0) {
            mesh_detector_auto_refresh_ticks =
                MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
            mesh_detector_preserve_scroll = 1;
            mesh_detector_event_cb(NULL);
            mesh_detector_preserve_scroll = 0;
        }
        return;
    }
    mesh_detector_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
}

static void mesh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    (void)mesh_voice_playback_poll();
    mesh_refresh_status();
    mesh_overlay_auto_refresh_tick();
}

static void mesh_background_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(app_current_page_is(PAGE_MESHTASTIC)) {
        return;
    }
    (void)mesh_voice_playback_poll();
    mesh_refresh_status();
}

static void mesh_background_monitor_start(void)
{
    if(!mesh_background_timer) {
        mesh_background_timer = lv_timer_create(mesh_background_timer_cb,
                                                3000, NULL);
    }
}

static void mesh_background_monitor_stop(void)
{
    if(mesh_background_timer) {
        lv_timer_delete(mesh_background_timer);
        mesh_background_timer = NULL;
    }
}

static void mesh_start_event_cb(lv_event_t *event)
{
    char region_arg[32];
    char preset_arg[40];
    char channel_arg[80];
    char slot_arg[16];
    char slot_option[32];
    char psk_arg[96];
    char power_arg[16];
    char power_option[32];
    char node_arg[64];
    char from_arg[32];
    char to_arg[32];
    char hop_arg[16];
    char relay_option[24];
    char position_option[80];
    char position_interval_arg[16];
    char fixed_position_option[96];
    char telemetry_option[160];
    char telemetry_device_interval_arg[16];
    char telemetry_environment_interval_arg[16];
    char media_option[192];
    char phoneapi_option[32];
    char command[1792];
    int rc;

    (void)event;
    if(access(MESHTASTIC_PROBE_PATH, X_OK) != 0) {
        mesh_append_log("probe missing: %s", MESHTASTIC_PROBE_PATH);
        mesh_refresh_status();
        return;
    }
    if(mesh_ipc_command("STATUS\n", mesh_status_text,
                        sizeof(mesh_status_text)) == 0) {
        mesh_append_log("daemon already running");
        if(mesh_position_enabled) {
            (void)k230_nrf9151_start_gnss_monitor();
        }
        mesh_background_monitor_start();
        mesh_refresh_status();
        return;
    }

    mesh_safe_arg(region_arg, sizeof(region_arg), mesh_region);
    mesh_safe_arg(preset_arg, sizeof(preset_arg), mesh_preset);
    mesh_safe_arg(channel_arg, sizeof(channel_arg), mesh_channel_name);
    mesh_normalize_slot();
    slot_option[0] = '\0';
    if(!mesh_slot_is_auto()) {
        mesh_safe_arg(slot_arg, sizeof(slot_arg), mesh_frequency_slot);
        snprintf(slot_option, sizeof(slot_option), "--slot %s ", slot_arg);
    }
    mesh_safe_arg(psk_arg, sizeof(psk_arg), mesh_psk);
    mesh_normalize_power();
    power_option[0] = '\0';
    if(!mesh_power_is_auto()) {
        mesh_safe_or_default(power_arg, sizeof(power_arg), mesh_tx_power,
                             "17");
        snprintf(power_option, sizeof(power_option), "--power %s ",
                 power_arg);
    }
    if(mesh_node_name_is_default(mesh_node_name)) {
        mesh_auto_node_name(mesh_node_name, sizeof(mesh_node_name));
    }
    mesh_safe_or_default(node_arg, sizeof(node_arg), mesh_node_name,
                         "k230-t-display");
    mesh_normalize_from_node();
    mesh_normalize_target_ack();
    mesh_save_profile_prefs();
    mesh_safe_or_default(from_arg, sizeof(from_arg), mesh_from_node, "0");
    mesh_safe_or_default(to_arg, sizeof(to_arg), mesh_to_node, "0xffffffff");
    mesh_normalize_hop();
    mesh_safe_or_default(hop_arg, sizeof(hop_arg), mesh_hop_limit, "3");
    relay_option[0] = '\0';
    if(!mesh_rebroadcast_enabled) {
        snprintf(relay_option, sizeof(relay_option), "--no-rebroadcast ");
    }
    mesh_safe_or_default(position_interval_arg,
                         sizeof(position_interval_arg),
                         mesh_position_interval, "900");
    snprintf(position_option, sizeof(position_option), "%s --position-interval %s ",
             mesh_position_enabled ? "--position" : "--no-position",
             position_interval_arg);
    fixed_position_option[0] = '\0';
    if(mesh_fixed_position_enabled) {
        long lat_i;
        long lon_i;
        long alt_m;

        if(mesh_parse_i32_text(mesh_fixed_latitude_i, &lat_i) == 0 &&
           mesh_parse_i32_text(mesh_fixed_longitude_i, &lon_i) == 0 &&
           mesh_parse_i32_text(mesh_fixed_altitude_m, &alt_m) == 0 &&
           lat_i != 0 && lon_i != 0) {
            snprintf(fixed_position_option, sizeof(fixed_position_option),
                     "--fixed-position-i %ld,%ld,%ld ",
                     lat_i, lon_i, alt_m);
        }
    }
    mesh_safe_or_default(telemetry_device_interval_arg,
                         sizeof(telemetry_device_interval_arg),
                         mesh_telemetry_device_interval, "300");
    mesh_safe_or_default(telemetry_environment_interval_arg,
                         sizeof(telemetry_environment_interval_arg),
                         mesh_telemetry_environment_interval, "300");
    snprintf(telemetry_option, sizeof(telemetry_option),
             "%s --telemetry-interval %s %s --env-telemetry-interval %s ",
             mesh_telemetry_enabled ? "--telemetry" : "--no-telemetry",
             telemetry_device_interval_arg,
             mesh_environment_telemetry_enabled ? "--env-telemetry" :
                                                  "--no-env-telemetry",
             telemetry_environment_interval_arg);
    mesh_normalize_media_config();
    snprintf(media_option, sizeof(media_option),
             "--photo-repeat %s --photo-repair-rounds %s "
             "--photo-repair-repeat %s --photo-repair-window-ms %s "
             "--photo-cache-ttl-sec %s ",
             mesh_photo_repeat, mesh_photo_repair_rounds,
             mesh_photo_repair_repeat, mesh_photo_repair_window_ms,
             mesh_photo_cache_ttl_sec);
    snprintf(phoneapi_option, sizeof(phoneapi_option), "%s",
             ui_ble_meshtastic_bridge_enabled() ? "" : "--no-phoneapi ");
    if(mesh_channel_name[0]) {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--channel-name %s %s--psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s%s%s%s%s%s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, channel_arg, slot_option, psk_arg,
                 power_option, node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option,
                 position_option, fixed_position_option, telemetry_option,
                 media_option, phoneapi_option);
    } else {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "%s--psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s%s%s%s%s%s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, slot_option, psk_arg, power_option,
                 node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option,
                 position_option, fixed_position_option, telemetry_option,
                 media_option, phoneapi_option);
    }
    rc = system(command);
    mesh_append_log("start daemon rc=%d log=%s", ui_shell_exit_code(rc),
                    MESHTASTIC_DAEMON_LOG);
    if(mesh_position_enabled) {
        (void)k230_nrf9151_start_gnss_monitor();
    }
    usleep(250000);
    mesh_background_monitor_start();
    mesh_refresh_status();
}

static void mesh_stop_event_cb(lv_event_t *event)
{
    char response[256];

    (void)event;
    if(mesh_ipc_command("QUIT\n", response, sizeof(response)) == 0) {
        ui_trim_text(response);
        mesh_append_log("%s", response);
    } else {
        ui_trim_text(response);
        mesh_append_log("stop failed: %s", response);
    }
    (void)k230_nrf9151_stop_gnss_monitor();
    usleep(120000);
    mesh_refresh_status();
}

void ui_meshtastic_pause_for_radio_owner(const char *owner)
{
    char response[512];
    char quit_response[256];

    if(mesh_radio_pause_active) {
        return;
    }

    mesh_radio_pause_active = 1;
    snprintf(mesh_radio_pause_owner, sizeof(mesh_radio_pause_owner), "%s",
             owner && owner[0] ? owner : "LoRa");
    mesh_background_monitor_stop();
    (void)k230_nrf9151_stop_gnss_monitor();

    if(mesh_ipc_command("STATUS\n", response, sizeof(response)) == 0 &&
       mesh_status_is_online(response)) {
        if(mesh_ipc_command("QUIT\n", quit_response,
                            sizeof(quit_response)) == 0) {
            ui_trim_text(quit_response);
            mesh_append_log("paused for %s: %s", mesh_radio_pause_owner,
                            quit_response);
        } else {
            ui_trim_text(quit_response);
            mesh_append_log("pause for %s failed: %s", mesh_radio_pause_owner,
                            quit_response);
        }
        usleep(160000);
    } else {
        ui_trim_text(response);
        mesh_append_log("pause for %s: daemon already offline (%s)",
                        mesh_radio_pause_owner,
                        response[0] ? response : "no status");
    }

    snprintf(mesh_status_text, sizeof(mesh_status_text), "Paused by %s",
             mesh_radio_pause_owner);
    mesh_apply_ble_status(mesh_status_text, 0);
}

void ui_meshtastic_resume_after_radio_owner(void)
{
    char owner[sizeof(mesh_radio_pause_owner)];

    if(!mesh_radio_pause_active) {
        return;
    }

    snprintf(owner, sizeof(owner), "%s",
             mesh_radio_pause_owner[0] ? mesh_radio_pause_owner : "LoRa");
    mesh_radio_pause_active = 0;
    mesh_radio_pause_owner[0] = '\0';

    if(!ui_meshtastic_autostart_enabled()) {
        mesh_append_log("resume after %s skipped: autostart disabled", owner);
        return;
    }

    mesh_append_log("resume after %s", owner);
    mesh_load_profile_prefs();
    mesh_start_event_cb(NULL);
    if(mesh_position_enabled) {
        (void)k230_nrf9151_start_gnss_monitor();
    }
    mesh_background_monitor_start();
}

void ui_meshtastic_release_radio_owner_foreground(void)
{
    char owner[sizeof(mesh_radio_pause_owner)];

    if(!mesh_radio_pause_active) {
        return;
    }

    snprintf(owner, sizeof(owner), "%s",
             mesh_radio_pause_owner[0] ? mesh_radio_pause_owner : "LoRa");
    mesh_radio_pause_active = 0;
    mesh_radio_pause_owner[0] = '\0';
    mesh_append_log("resume after %s handled by Meshtastic foreground",
                    owner);
}

static void mesh_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_refresh_status();
    mesh_append_log("refresh: %s", mesh_status_text);
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        mesh_settings_refresh();
    }
}

static void mesh_restart_daemon_if_online(void)
{
    char response[256];

    if(mesh_ipc_command("STATUS\n", response, sizeof(response)) != 0) {
        mesh_refresh_status();
        return;
    }
    if(mesh_ipc_command("QUIT\n", response, sizeof(response)) == 0) {
        ui_trim_text(response);
        mesh_append_log("restart: %s", response);
    } else {
        ui_trim_text(response);
        mesh_append_log("restart stop failed: %s", response);
    }
    usleep(220000);
    mesh_start_event_cb(NULL);
}

void ui_meshtastic_apply_ble_setting(void)
{
    if(!ui_ble_meshtastic_bridge_enabled()) {
        app_set_ble_status("offline");
    }
    mesh_restart_daemon_if_online();
}

static void mesh_publish_event_cb(lv_event_t *event)
{
    const char *command = (const char *)lv_event_get_user_data(event);
    char response[256];
    int ok;

    if(!command || !command[0]) {
        return;
    }
    ok = mesh_ipc_command(command, response, sizeof(response)) == 0;
    if(ok) {
        ui_trim_text(response);
        mesh_append_log("publish: %s", response);
    } else {
        ui_trim_text(response);
        mesh_append_log("publish failed: %s", response);
    }
    if(mesh_publish_status_label &&
       lv_obj_is_valid(mesh_publish_status_label)) {
        lv_label_set_text(mesh_publish_status_label,
                          response[0] ? response :
                          (ok ? ui_tr("Queued") : ui_tr("Failed")));
        lv_obj_set_style_text_color(
            mesh_publish_status_label,
            lv_color_hex(ok ? 0x25C281 : 0xEF4D5A), 0);
    }
    mesh_refresh_status();
}

static int mesh_channel_profile_ensure_dir(void)
{
    if(mkdir(MESHTASTIC_CHANNEL_DIR, 0755) != 0 && errno != EEXIST) {
        mesh_append_log("channel profile mkdir failed: %s", strerror(errno));
        return -1;
    }
    if(mkdir(MESHTASTIC_CHANNEL_PROFILE_DIR, 0755) != 0 &&
       errno != EEXIST) {
        mesh_append_log("channel profile mkdir failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void mesh_channel_profiles_close(void)
{
    if(mesh_channel_profile_delete_overlay &&
       lv_obj_is_valid(mesh_channel_profile_delete_overlay)) {
        lv_obj_delete(mesh_channel_profile_delete_overlay);
    }
    mesh_channel_profile_delete_overlay = NULL;
    if(mesh_channel_profiles_overlay &&
       lv_obj_is_valid(mesh_channel_profiles_overlay)) {
        lv_obj_delete(mesh_channel_profiles_overlay);
    }
    mesh_channel_profiles_overlay = NULL;
}

static void mesh_channel_profiles_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_channel_profiles_close();
}

static void mesh_channel_profile_default_name(char *out, size_t out_len)
{
    if(!out || out_len == 0U) {
        return;
    }
    if(mesh_channel_name[0]) {
        snprintf(out, out_len, "%s", mesh_channel_name);
    } else {
        snprintf(out, out_len, "%s %s",
                 mesh_region[0] ? mesh_region : MESHTASTIC_DEFAULT_UI_REGION,
                 mesh_preset[0] ? mesh_preset : MESHTASTIC_DEFAULT_UI_PRESET);
    }
}

static int mesh_channel_profile_read_value(const char *path,
                                           const char *key,
                                           char *out, size_t out_len,
                                           const char *fallback)
{
    FILE *fp;
    char line[256];
    size_t key_len;

    if(!out || out_len == 0U) {
        return -1;
    }
    snprintf(out, out_len, "%s", fallback ? fallback : "");
    if(!path || !key || !key[0]) {
        return -1;
    }
    fp = fopen(path, "r");
    if(!fp) {
        return -1;
    }
    key_len = strlen(key);
    while(fgets(line, sizeof(line), fp)) {
        char *eq;

        ui_trim_text(line);
        if(line[0] == '#' || line[0] == '\0') {
            continue;
        }
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq++ = '\0';
        ui_trim_text(line);
        ui_trim_text(eq);
        if(strlen(line) == key_len && strcmp(line, key) == 0) {
            snprintf(out, out_len, "%s", eq);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int mesh_channel_profile_write_current(void)
{
    char path[160];
    char tmp_path[176];
    char name[64];
    FILE *fp = NULL;
    int slot = -1;
    int overwrite = 0;

    if(mesh_channel_profile_ensure_dir() != 0) {
        return -1;
    }

    if(mesh_channel_profile_edit_path[0] &&
       access(mesh_channel_profile_edit_path, F_OK) == 0) {
        snprintf(path, sizeof(path), "%s", mesh_channel_profile_edit_path);
        overwrite = 1;
    }

    for(int i = 0; !overwrite && i < 100; i++) {
        snprintf(path, sizeof(path), "%s/channel_%02d.conf",
                 MESHTASTIC_CHANNEL_PROFILE_DIR, i);
        if(access(path, F_OK) != 0) {
            slot = i;
            break;
        }
    }
    if(!overwrite && slot < 0) {
        mesh_append_log("channel profile save failed: no free slot");
        return -1;
    }

    if(!overwrite) {
        snprintf(path, sizeof(path), "%s/channel_%02d.conf",
                 MESHTASTIC_CHANNEL_PROFILE_DIR, slot);
    }
    if(overwrite) {
        mesh_channel_profile_read_value(path, "name", name, sizeof(name),
                                        "");
    } else {
        mesh_channel_profile_default_name(name, sizeof(name));
    }
    if(!name[0]) {
        mesh_channel_profile_default_name(name, sizeof(name));
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        mesh_append_log("channel profile save failed: %s", strerror(errno));
        return -1;
    }

    fprintf(fp, "# K230 Meshtastic channel profile\n");
    fprintf(fp, "name=%s\n", name);
    fprintf(fp, "region=%s\n", mesh_region);
    fprintf(fp, "preset=%s\n", mesh_preset);
    fprintf(fp, "channel=%s\n", mesh_channel_name);
    fprintf(fp, "slot=%s\n", mesh_frequency_slot);
    fprintf(fp, "psk=%s\n", mesh_psk);
    fprintf(fp, "power=%s\n", mesh_tx_power);
    fprintf(fp, "node=%s\n", mesh_node_name);
    mesh_normalize_from_node();
    mesh_normalize_target_ack();
    fprintf(fp, "from=%s\n", mesh_from_node);
    fprintf(fp, "to=%s\n", mesh_to_node);
    fprintf(fp, "hop=%s\n", mesh_hop_limit);
    fprintf(fp, "ack=%d\n", mesh_ack_enabled ? 1 : 0);
    fprintf(fp, "rebroadcast=%d\n", mesh_rebroadcast_enabled ? 1 : 0);
    fprintf(fp, "position=%d\n", mesh_position_enabled ? 1 : 0);
    fprintf(fp, "position_interval=%s\n", mesh_position_interval);
    fprintf(fp, "telemetry=%d\n", mesh_telemetry_enabled ? 1 : 0);
    fprintf(fp, "telemetry_interval=%s\n", mesh_telemetry_device_interval);
    fprintf(fp, "environment_telemetry=%d\n",
            mesh_environment_telemetry_enabled ? 1 : 0);
    fprintf(fp, "environment_telemetry_interval=%s\n",
            mesh_telemetry_environment_interval);
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        mesh_append_log("channel profile save failed: %s", strerror(errno));
        return -1;
    }
    if(rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        mesh_append_log("channel profile save failed: %s", strerror(errno));
        return -1;
    }
    snprintf(mesh_channel_profile_edit_path,
             sizeof(mesh_channel_profile_edit_path), "%s", path);
    mesh_append_log(overwrite ? "channel profile updated: %s" :
                    "channel profile saved: %s", path);
    return 0;
}

static int mesh_channel_profile_apply_file_ex(const char *path,
                                              int close_profiles,
                                              int restart_daemon,
                                              const char *action)
{
    char value[96];

    if(!path || !path[0]) {
        return -1;
    }
    if(mesh_channel_profile_read_value(path, "region", value,
                                       sizeof(value),
                                       MESHTASTIC_DEFAULT_UI_REGION) == 0) {
        mesh_safe_or_default(mesh_region, sizeof(mesh_region), value,
                             MESHTASTIC_DEFAULT_UI_REGION);
    }
    if(mesh_channel_profile_read_value(path, "preset", value,
                                       sizeof(value),
                                       MESHTASTIC_DEFAULT_UI_PRESET) == 0) {
        mesh_safe_or_default(mesh_preset, sizeof(mesh_preset), value,
                             MESHTASTIC_DEFAULT_UI_PRESET);
    }
    if(!mesh_profile_supports_ui_preset(mesh_current_profile(), mesh_preset)) {
        snprintf(mesh_preset, sizeof(mesh_preset), "%s",
                 mesh_default_preset_for_region(mesh_region));
    }
    if(mesh_channel_profile_read_value(path, "channel", value,
                                       sizeof(value), "") == 0) {
        if(!value[0] || strcmp(value, "-") == 0 ||
           strcasecmp(value, "default") == 0) {
            mesh_channel_name[0] = '\0';
        } else {
            mesh_safe_arg(mesh_channel_name, sizeof(mesh_channel_name), value);
        }
    }
    if(mesh_channel_profile_read_value(path, "slot", value, sizeof(value),
                                       "auto") == 0) {
        mesh_safe_or_default(mesh_frequency_slot, sizeof(mesh_frequency_slot),
                             value, "auto");
        mesh_normalize_slot();
    }
    if(mesh_channel_profile_read_value(path, "psk", value, sizeof(value),
                                       "default") == 0) {
        mesh_safe_or_default(mesh_psk, sizeof(mesh_psk), value, "default");
    }
    if(mesh_channel_profile_read_value(path, "power", value, sizeof(value),
                                       "auto") == 0) {
        mesh_safe_or_default(mesh_tx_power, sizeof(mesh_tx_power), value,
                             "auto");
        mesh_normalize_power();
    }
    if(mesh_channel_profile_read_value(path, "node", value, sizeof(value),
                                       mesh_node_name) == 0) {
        mesh_safe_or_default(mesh_node_name, sizeof(mesh_node_name), value,
                             "k230-t-display");
    }
    if(mesh_channel_profile_read_value(path, "from", value, sizeof(value),
                                       "0") == 0) {
        if(mesh_normalize_from_node_text(value, mesh_from_node,
                                         sizeof(mesh_from_node)) != 0) {
            snprintf(mesh_from_node, sizeof(mesh_from_node), "0");
        }
    }
    if(mesh_channel_profile_read_value(path, "to", value, sizeof(value),
                                       "0xffffffff") == 0) {
        if(mesh_normalize_to_node_text(value, mesh_to_node,
                                       sizeof(mesh_to_node)) != 0) {
            snprintf(mesh_to_node, sizeof(mesh_to_node), "0xffffffff");
        }
    }
    if(mesh_channel_profile_read_value(path, "hop", value, sizeof(value),
                                       "3") == 0) {
        mesh_safe_or_default(mesh_hop_limit, sizeof(mesh_hop_limit), value,
                             "3");
        mesh_normalize_hop();
    }
    if(mesh_channel_profile_read_value(path, "ack", value, sizeof(value),
                                       "0") == 0) {
        mesh_ack_enabled = strcmp(value, "0") != 0;
    }
    if(mesh_channel_profile_read_value(path, "rebroadcast", value,
                                       sizeof(value), "0") == 0) {
        mesh_rebroadcast_enabled = strcmp(value, "0") != 0;
    }
    if(mesh_channel_profile_read_value(path, "position", value,
                                       sizeof(value), "1") == 0) {
        mesh_position_enabled = strcmp(value, "0") != 0;
    }
    if(mesh_channel_profile_read_value(path, "position_interval", value,
                                       sizeof(value), "900") == 0) {
        mesh_safe_or_default(mesh_position_interval,
                             sizeof(mesh_position_interval), value, "900");
    }
    if(mesh_channel_profile_read_value(path, "telemetry", value,
                                       sizeof(value), "1") == 0) {
        mesh_telemetry_enabled = strcmp(value, "0") != 0;
    }
    if(mesh_channel_profile_read_value(path, "telemetry_interval", value,
                                       sizeof(value), "300") == 0) {
        mesh_safe_or_default(mesh_telemetry_device_interval,
                             sizeof(mesh_telemetry_device_interval), value,
                             "300");
    }
    if(mesh_channel_profile_read_value(path, "environment_telemetry", value,
                                       sizeof(value), "1") == 0) {
        mesh_environment_telemetry_enabled = strcmp(value, "0") != 0;
    }
    if(mesh_channel_profile_read_value(path, "environment_telemetry_interval",
                                       value, sizeof(value), "300") == 0) {
        mesh_safe_or_default(mesh_telemetry_environment_interval,
                             sizeof(mesh_telemetry_environment_interval),
                             value, "300");
    }
    mesh_normalize_target_ack();
    mesh_save_profile_prefs();
    mesh_settings_refresh();
    mesh_update_profile_label();
    mesh_update_target_button();
    if(close_profiles) {
        mesh_channel_profiles_close();
    }
    mesh_append_log("channel profile %s: %s",
                    action && action[0] ? action : "loaded", path);
    if(restart_daemon) {
        mesh_restart_daemon_if_online();
    }
    return 0;
}

static void mesh_channel_profile_apply_file(const char *path)
{
    if(mesh_channel_profile_apply_file_ex(path, 1, 1, "loaded") == 0) {
        mesh_channel_profile_edit_path[0] = '\0';
    }
}

static void mesh_channel_profile_load_event_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_channel_profile_apply_file(path);
}

static void mesh_channel_profiles_event_cb(lv_event_t *event);
static void mesh_profile_event_cb(lv_event_t *event);

static void mesh_channel_profile_save_event_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_channel_profile_write_current() == 0) {
        mesh_channel_profiles_event_cb(NULL);
    }
}

static void mesh_channel_profile_edit_event_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);
    char edit_path[160];

    if(event) {
        lv_event_stop_processing(event);
    }
    if(!path || !path[0]) {
        return;
    }
    snprintf(edit_path, sizeof(edit_path), "%s", path);
    if(mesh_channel_profile_apply_file_ex(edit_path, 1, 0, "editing") == 0) {
        snprintf(mesh_channel_profile_edit_path,
                 sizeof(mesh_channel_profile_edit_path), "%s", edit_path);
        mesh_profile_event_cb(NULL);
    }
}

static void mesh_channel_profile_delete_confirm_close(void)
{
    if(mesh_channel_profile_delete_overlay &&
       lv_obj_is_valid(mesh_channel_profile_delete_overlay)) {
        lv_obj_delete(mesh_channel_profile_delete_overlay);
    }
    mesh_channel_profile_delete_overlay = NULL;
}

static void mesh_channel_profile_delete_cancel_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_channel_profile_delete_confirm_close();
}

static void mesh_channel_profile_delete_accept_event_cb(lv_event_t *event)
{
    char deleted_path[160];

    if(event) {
        lv_event_stop_processing(event);
    }
    snprintf(deleted_path, sizeof(deleted_path), "%s",
             mesh_channel_profile_delete_path);
    if(deleted_path[0] && unlink(deleted_path) == 0) {
        mesh_append_log("channel profile deleted: %s", deleted_path);
        if(strcmp(mesh_channel_profile_edit_path, deleted_path) == 0) {
            mesh_channel_profile_edit_path[0] = '\0';
        }
    } else {
        mesh_append_log("channel profile delete failed: %s",
                        strerror(errno));
    }
    mesh_channel_profile_delete_path[0] = '\0';
    mesh_channel_profile_delete_confirm_close();
    mesh_channel_profiles_event_cb(NULL);
}

static void mesh_channel_profile_delete_event_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);
    lv_obj_t *dialog;
    lv_obj_t *title;
    lv_obj_t *name_label;
    lv_obj_t *note;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int dialog_w = ui_is_landscape() ? 560 : 500;
    int dialog_h = ui_is_landscape() ? 236 : 260;
    int pad = 24;
    int gap = 18;
    int button_w;
    int button_y;
    char name[64];

    if(event) {
        lv_event_stop_processing(event);
    }
    if(!path || !path[0]) {
        return;
    }

    mesh_channel_profile_delete_confirm_close();
    snprintf(mesh_channel_profile_delete_path,
             sizeof(mesh_channel_profile_delete_path), "%s", path);
    mesh_channel_profile_read_value(path, "name", name, sizeof(name),
                                    "Channel");

    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 48) {
        dialog_h = screen_h - 48;
    }
    if(dialog_h < 216) {
        dialog_h = 216;
    }
    button_w = (dialog_w - pad * 2 - gap) / 2;
    button_y = dialog_h - pad - 58;

    mesh_channel_profile_delete_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_channel_profile_delete_overlay);
    lv_obj_set_style_bg_color(mesh_channel_profile_delete_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_channel_profile_delete_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mesh_channel_profile_delete_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_profile_delete_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_profile_delete_overlay,
                      LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(mesh_channel_profile_delete_overlay, 0, 0,
                      dialog_w, dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3A2630), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    title = ui_label(dialog, ui_tr("Delete profile?"),
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, pad, pad);
    lv_obj_set_width(title, dialog_w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    name_label = ui_label(dialog, name, &lv_font_montserrat_18, 0xF5A524);
    lv_obj_set_pos(name_label, pad, pad + 52);
    lv_obj_set_width(name_label, dialog_w - pad * 2);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    note = ui_label(dialog, ui_tr("This cannot be undone."),
                    &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(note, pad, pad + 88);
    lv_obj_set_width(note, dialog_w - pad * 2);
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, button_y, button_w,
                            ui_tr("Cancel"), 0x9AA4AF);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3644), 0);
    lv_obj_add_event_cb(btn, mesh_channel_profile_delete_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + button_w + gap, button_y,
                            button_w, ui_tr("Delete"), 0xEF4D5A);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1D24), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xEF4D5A), 0);
    lv_obj_add_event_cb(btn, mesh_channel_profile_delete_accept_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static int mesh_channel_profile_collect(char paths[][160], int max_paths)
{
    int count = 0;

    if(mesh_channel_profile_ensure_dir() != 0) {
        return 0;
    }
    for(int i = 0; i < 100 && count < max_paths; i++) {
        char path[160];

        snprintf(path, sizeof(path), "%s/channel_%02d.conf",
                 MESHTASTIC_CHANNEL_PROFILE_DIR, i);
        if(access(path, R_OK) == 0) {
            snprintf(paths[count], 160, "%s", path);
            count++;
        }
    }
    return count;
}

static void mesh_channel_profile_add_card(lv_obj_t *panel, const char *path,
                                          int x, int y, int w, int h,
                                          int index)
{
    lv_obj_t *card;
    lv_obj_t *name_label;
    lv_obj_t *summary_label;
    lv_obj_t *btn;
    int button_gap = 8;
    int button_w = (w - 28 - button_gap * 2) / 3;
    int button_total_w;
    int button_x;
    int button_y = h - 46;
    char name[64];
    char region[24];
    char preset[32];
    char channel[64];
    char slot[16];
    char psk[32];
    char summary[220];

    if(index < 0 || index >= MESHTASTIC_CHANNEL_PROFILE_MAX ||
       !path || !path[0]) {
        return;
    }
    mesh_channel_profile_read_value(path, "name", name, sizeof(name),
                                    "Channel");
    mesh_channel_profile_read_value(path, "region", region, sizeof(region),
                                    MESHTASTIC_DEFAULT_UI_REGION);
    mesh_channel_profile_read_value(path, "preset", preset, sizeof(preset),
                                    MESHTASTIC_DEFAULT_UI_PRESET);
    mesh_channel_profile_read_value(path, "channel", channel,
                                    sizeof(channel), "");
    mesh_channel_profile_read_value(path, "slot", slot, sizeof(slot),
                                    "auto");
    mesh_channel_profile_read_value(path, "psk", psk, sizeof(psk),
                                    "default");
    if(!channel[0]) {
        snprintf(channel, sizeof(channel), "default");
    }
    snprintf(summary, sizeof(summary), "%s  %s\nchannel %s  slot %s\npsk %s",
             region, preset, channel, slot, psk);

    card = ui_panel(panel, x, y, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x243044), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    name_label = ui_label(card, name, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(name_label, 14, 12);
    lv_obj_set_width(name_label, w - 28);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    summary_label = ui_label(card, summary, &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(summary_label, 14, 48);
    lv_obj_set_width(summary_label, w - 28);
    lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);

    if(button_w < 64) {
        button_w = 64;
    }
    button_total_w = button_w * 3 + button_gap * 2;
    if(button_total_w > w - 28) {
        button_w = (w - 28 - button_gap * 2) / 3;
        if(button_w < 56) {
            button_w = 56;
        }
        button_total_w = button_w * 3 + button_gap * 2;
    }
    button_x = 14 + ((w - 28) - button_total_w) / 2;
    btn = ui_command_button(card, button_x, button_y, button_w,
                            ui_tr("Load"), 0x25C281);
    lv_obj_set_height(btn, 38);
    lv_obj_add_event_cb(btn, mesh_channel_profile_load_event_cb,
                        LV_EVENT_CLICKED, mesh_channel_profile_paths[index]);
    btn = ui_command_button(card, button_x + button_w + button_gap, button_y,
                            button_w, ui_tr("Edit"), 0x3DA5FF);
    lv_obj_set_height(btn, 38);
    lv_obj_add_event_cb(btn, mesh_channel_profile_edit_event_cb,
                        LV_EVENT_CLICKED, mesh_channel_profile_paths[index]);
    btn = ui_command_button(card, button_x + (button_w + button_gap) * 2,
                            button_y, button_w, ui_tr("Delete"), 0xEF4D5A);
    lv_obj_set_height(btn, 38);
    lv_obj_add_event_cb(btn, mesh_channel_profile_delete_event_cb,
                        LV_EVENT_CLICKED, mesh_channel_profile_paths[index]);
}

static void mesh_channel_profiles_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    lv_obj_t *empty;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int columns = ui_is_landscape() ? 2 : 1;
    int gap = 12;
    int card_w = columns == 2 ? (content_w - gap) / 2 : content_w;
    int card_h = 184;
    int y = 112;
    int count;

    (void)event;
    ui_input_hide_inline_active();
    mesh_channel_profiles_close();
    memset(mesh_channel_profile_paths, 0, sizeof(mesh_channel_profile_paths));
    count = mesh_channel_profile_collect(mesh_channel_profile_paths,
                                         MESHTASTIC_CHANNEL_PROFILE_MAX);

    mesh_channel_profiles_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_channel_profiles_overlay);
    lv_obj_set_style_bg_color(mesh_channel_profiles_overlay,
                              lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_channel_profiles_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_channel_profiles_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_channel_profiles_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_profiles_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_profiles_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_channel_profiles_overlay);

    panel = ui_scroll_panel(mesh_channel_profiles_overlay, 0, 0,
                            screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Channel profiles"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 240);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel,
                        ui_tr("Save current channel or load a saved profile"),
                        &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 240);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 216, 18, 110,
                            ui_tr("Save current"), 0x25C281);
    lv_obj_add_event_cb(btn, mesh_channel_profile_save_event_cb,
                        LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_channel_profiles_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    if(count == 0) {
        empty = ui_label(panel,
                         ui_tr("No saved channel profiles yet"),
                         &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(empty, margin, y + 12);
        lv_obj_set_width(empty, content_w);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        return;
    }

    for(int i = 0; i < count; i++) {
        int col = i % columns;
        int row = i / columns;
        int x = margin + col * (card_w + gap);
        int card_y = y + row * (card_h + gap);

        mesh_channel_profile_add_card(panel, mesh_channel_profile_paths[i],
                                      x, card_y, card_w, card_h, i);
    }
}

static const mesh_region_choice_t *mesh_find_region_choice(const char *value)
{
    if(!value || !value[0]) {
        return NULL;
    }
    for(size_t i = 0; i < sizeof(mesh_region_choices) /
           sizeof(mesh_region_choices[0]); i++) {
        if(strcmp(mesh_region_choices[i].value, value) == 0) {
            return &mesh_region_choices[i];
        }
    }
    return NULL;
}

static mesh_choice_profile_t mesh_current_profile(void)
{
    const mesh_region_choice_t *region = mesh_find_region_choice(mesh_region);

    return region ? region->profile : MESH_PROFILE_EU868;
}

static const char *mesh_default_preset_for_region(const char *region_value)
{
    const mesh_region_choice_t *region = mesh_find_region_choice(region_value);

    return region ? region->default_preset : MESHTASTIC_DEFAULT_UI_PRESET;
}

static int mesh_profile_supports_ui_preset(mesh_choice_profile_t profile,
                                           const char *preset)
{
    unsigned mask = MESH_PROFILE_MASK(profile);

    if(!preset || !preset[0]) {
        return 0;
    }
    for(size_t i = 0; i < sizeof(mesh_preset_choices) /
           sizeof(mesh_preset_choices[0]); i++) {
        if(strcmp(mesh_preset_choices[i].value, preset) == 0) {
            return (mesh_preset_choices[i].profiles & mask) != 0U;
        }
    }
    return 0;
}

static const char *mesh_setting_name(mesh_setting_field_t field)
{
    switch(field) {
    case MESH_FIELD_REGION:
        return "Region";
    case MESH_FIELD_PRESET:
        return "Preset";
    case MESH_FIELD_CHANNEL:
        return "Channel name";
    case MESH_FIELD_SLOT:
        return "Frequency slot";
    case MESH_FIELD_PSK:
        return "PSK";
    case MESH_FIELD_POWER:
        return "TX power";
    case MESH_FIELD_NODE:
        return "Node name";
    case MESH_FIELD_FROM:
        return "From node";
    case MESH_FIELD_TO:
        return "To node";
    case MESH_FIELD_HOP:
        return "Hop limit";
    case MESH_FIELD_ACK:
        return "ACK";
    case MESH_FIELD_REBROADCAST:
        return "Rebroadcast";
    case MESH_FIELD_POSITION:
        return "Position";
    case MESH_FIELD_POSITION_INTERVAL:
        return "Position interval";
    case MESH_FIELD_TELEMETRY:
        return "Device telemetry";
    case MESH_FIELD_TELEMETRY_INTERVAL:
        return "Device telemetry interval";
    case MESH_FIELD_ENV_TELEMETRY:
        return "Environment telemetry";
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        return "Environment telemetry interval";
    case MESH_FIELD_PHOTO_REPEAT:
        return "Photo repeat";
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        return "Photo repair rounds";
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        return "Photo repair repeat";
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        return "Photo repair window";
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return "Photo cache TTL";
    default:
        return "Setting";
    }
}

static int mesh_setting_uses_choice(mesh_setting_field_t field)
{
    switch(field) {
    case MESH_FIELD_REGION:
    case MESH_FIELD_PRESET:
    case MESH_FIELD_POWER:
    case MESH_FIELD_SLOT:
    case MESH_FIELD_HOP:
    case MESH_FIELD_ACK:
    case MESH_FIELD_REBROADCAST:
    case MESH_FIELD_POSITION:
    case MESH_FIELD_POSITION_INTERVAL:
    case MESH_FIELD_TELEMETRY:
    case MESH_FIELD_TELEMETRY_INTERVAL:
    case MESH_FIELD_ENV_TELEMETRY:
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
    case MESH_FIELD_PHOTO_REPEAT:
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return 1;
    default:
        return 0;
    }
}

static const char *mesh_choice_label_for_value(const mesh_choice_t *choices,
                                               size_t count,
                                               const char *value,
                                               char *buf, size_t len)
{
    if(choices && value) {
        for(size_t i = 0; i < count; i++) {
            if(strcmp(choices[i].value, value) == 0) {
                return choices[i].label;
            }
        }
    }
    if(buf && len > 0U) {
        snprintf(buf, len, "%s", value && value[0] ? value : "-");
        return buf;
    }
    return "-";
}

static const char *mesh_setting_value(mesh_setting_field_t field,
                                      char *buf, size_t len)
{
    switch(field) {
    case MESH_FIELD_REGION:
        return mesh_region;
    case MESH_FIELD_PRESET:
        return mesh_preset;
    case MESH_FIELD_CHANNEL:
        return mesh_channel_name[0] ? mesh_channel_name : "<preset>";
    case MESH_FIELD_SLOT:
        if(mesh_slot_is_auto()) {
            snprintf(buf, len, "Auto");
            return buf;
        }
        snprintf(buf, len, "Slot %s", mesh_frequency_slot);
        return buf;
    case MESH_FIELD_PSK:
        return mesh_psk;
    case MESH_FIELD_POWER:
        if(mesh_power_is_auto()) {
            snprintf(buf, len, "Auto");
            return buf;
        }
        return mesh_tx_power;
    case MESH_FIELD_NODE:
        return mesh_node_name;
    case MESH_FIELD_FROM:
        if(mesh_from_text_is_auto(mesh_from_node)) {
            snprintf(buf, len, "Auto");
            return buf;
        }
        return mesh_from_node;
    case MESH_FIELD_TO:
        if(mesh_to_text_is_broadcast(mesh_to_node)) {
            snprintf(buf, len, "Broadcast");
            return buf;
        }
        return mesh_to_node;
    case MESH_FIELD_HOP:
        return mesh_hop_limit;
    case MESH_FIELD_ACK:
        snprintf(buf, len, "%s", mesh_ack_enabled ? "On" : "Off");
        return buf;
    case MESH_FIELD_REBROADCAST:
        snprintf(buf, len, "%s", mesh_rebroadcast_enabled ? "On" : "Off");
        return buf;
    case MESH_FIELD_POSITION:
        snprintf(buf, len, "%s", mesh_position_enabled ? "On" : "Off");
        return buf;
    case MESH_FIELD_POSITION_INTERVAL:
        if(strcmp(mesh_position_interval, "300") == 0) {
            snprintf(buf, len, "5 min");
        } else if(strcmp(mesh_position_interval, "1800") == 0) {
            snprintf(buf, len, "30 min");
        } else if(strcmp(mesh_position_interval, "3600") == 0) {
            snprintf(buf, len, "60 min");
        } else {
            snprintf(buf, len, "15 min");
        }
        return buf;
    case MESH_FIELD_TELEMETRY:
        snprintf(buf, len, "%s", mesh_telemetry_enabled ? "On" : "Off");
        return buf;
    case MESH_FIELD_TELEMETRY_INTERVAL:
        if(strcmp(mesh_telemetry_device_interval, "900") == 0) {
            snprintf(buf, len, "15 min");
        } else if(strcmp(mesh_telemetry_device_interval, "1800") == 0) {
            snprintf(buf, len, "30 min");
        } else if(strcmp(mesh_telemetry_device_interval, "3600") == 0) {
            snprintf(buf, len, "60 min");
        } else {
            snprintf(buf, len, "5 min");
        }
        return buf;
    case MESH_FIELD_ENV_TELEMETRY:
        snprintf(buf, len, "%s",
                 mesh_environment_telemetry_enabled ? "On" : "Off");
        return buf;
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        if(strcmp(mesh_telemetry_environment_interval, "900") == 0) {
            snprintf(buf, len, "15 min");
        } else if(strcmp(mesh_telemetry_environment_interval, "1800") == 0) {
            snprintf(buf, len, "30 min");
        } else if(strcmp(mesh_telemetry_environment_interval, "3600") == 0) {
            snprintf(buf, len, "60 min");
        } else {
            snprintf(buf, len, "5 min");
        }
        return buf;
    case MESH_FIELD_PHOTO_REPEAT:
        return mesh_choice_label_for_value(
            mesh_photo_repeat_choices,
            sizeof(mesh_photo_repeat_choices) /
                sizeof(mesh_photo_repeat_choices[0]),
            mesh_photo_repeat, buf, len);
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        return mesh_choice_label_for_value(
            mesh_photo_repair_round_choices,
            sizeof(mesh_photo_repair_round_choices) /
                sizeof(mesh_photo_repair_round_choices[0]),
            mesh_photo_repair_rounds, buf, len);
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        return mesh_choice_label_for_value(
            mesh_photo_repair_repeat_choices,
            sizeof(mesh_photo_repair_repeat_choices) /
                sizeof(mesh_photo_repair_repeat_choices[0]),
            mesh_photo_repair_repeat, buf, len);
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        return mesh_choice_label_for_value(
            mesh_photo_repair_window_choices,
            sizeof(mesh_photo_repair_window_choices) /
                sizeof(mesh_photo_repair_window_choices[0]),
            mesh_photo_repair_window_ms, buf, len);
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return mesh_choice_label_for_value(
            mesh_photo_cache_ttl_choices,
            sizeof(mesh_photo_cache_ttl_choices) /
                sizeof(mesh_photo_cache_ttl_choices[0]),
            mesh_photo_cache_ttl_sec, buf, len);
    default:
        return "";
    }
}

static void mesh_settings_refresh(void)
{
    if(mesh_settings_status_card && lv_obj_is_valid(mesh_settings_status_card)) {
        char line1[160];
        char line2[160];
        char line3[240];
        uint32_t accent = mesh_status_is_online(mesh_status_text) ? 0x25C281 :
                          0xF5A524;
        mesh_settings_status_summary(mesh_status_text, line1, sizeof(line1),
                                     line2, sizeof(line2), line3,
                                     sizeof(line3));
        lv_obj_set_style_border_color(mesh_settings_status_card,
                                      lv_color_hex(accent), 0);
        if(mesh_settings_status_labels[0] &&
           lv_obj_is_valid(mesh_settings_status_labels[0])) {
            lv_label_set_text(mesh_settings_status_labels[0], line1);
            lv_obj_set_style_text_color(mesh_settings_status_labels[0],
                                        lv_color_hex(accent), 0);
        }
        if(mesh_settings_status_labels[1] &&
           lv_obj_is_valid(mesh_settings_status_labels[1])) {
            lv_label_set_text(mesh_settings_status_labels[1], line2);
        }
        if(mesh_settings_status_labels[2] &&
           lv_obj_is_valid(mesh_settings_status_labels[2])) {
            lv_label_set_text(mesh_settings_status_labels[2], line3);
        }
    }
    for(int i = 0; i < (int)MESH_FIELD_COUNT; i++) {
        char buf[32];
        if(mesh_settings_value_labels[i] &&
           lv_obj_is_valid(mesh_settings_value_labels[i])) {
            lv_label_set_text(mesh_settings_value_labels[i],
                              mesh_setting_value((mesh_setting_field_t)i,
                                                 buf, sizeof(buf)));
        }
    }
    mesh_update_profile_label();
}

static void mesh_setting_submit_cb(const char *text, void *user_data)
{
    mesh_setting_field_t field =
        (mesh_setting_field_t)(intptr_t)user_data;
    char tmp[96];
    unsigned long value;
    long signed_value;

    if(!text) {
        return;
    }
    switch(field) {
    case MESH_FIELD_REGION:
        mesh_safe_or_default(mesh_region, sizeof(mesh_region), text,
                             MESHTASTIC_DEFAULT_UI_REGION);
        break;
    case MESH_FIELD_PRESET:
        mesh_safe_or_default(mesh_preset, sizeof(mesh_preset), text,
                             MESHTASTIC_DEFAULT_UI_PRESET);
        break;
    case MESH_FIELD_CHANNEL:
        if(!text[0] || strcmp(text, "-") == 0 ||
           strcmp(text, "default") == 0) {
            mesh_channel_name[0] = '\0';
        } else {
            mesh_safe_arg(mesh_channel_name, sizeof(mesh_channel_name), text);
        }
        break;
    case MESH_FIELD_SLOT:
        mesh_safe_or_default(mesh_frequency_slot, sizeof(mesh_frequency_slot),
                             text, "auto");
        mesh_normalize_slot();
        break;
    case MESH_FIELD_PSK:
        mesh_safe_or_default(mesh_psk, sizeof(mesh_psk), text, "default");
        break;
    case MESH_FIELD_POWER:
        mesh_safe_or_default(tmp, sizeof(tmp), text, "auto");
        if(mesh_power_text_is_auto(tmp)) {
            snprintf(mesh_tx_power, sizeof(mesh_tx_power), "auto");
            break;
        }
        if(mesh_parse_i32_text(tmp, &signed_value) != 0 ||
           signed_value < -9L || signed_value > 22L) {
            mesh_append_log("invalid TX power: %s", text);
            return;
        }
        snprintf(mesh_tx_power, sizeof(mesh_tx_power), "%ld", signed_value);
        break;
    case MESH_FIELD_NODE:
        mesh_safe_or_default(mesh_node_name, sizeof(mesh_node_name), text,
                             "k230-t-display");
        break;
    case MESH_FIELD_FROM:
        if(mesh_normalize_from_node_text(text, mesh_from_node,
                                         sizeof(mesh_from_node)) != 0) {
            mesh_append_log("invalid from node: %s", text);
            return;
        }
        break;
    case MESH_FIELD_TO:
        if(mesh_normalize_to_node_text(text, mesh_to_node,
                                       sizeof(mesh_to_node)) != 0) {
            mesh_append_log("invalid to node: %s", text);
            return;
        }
        break;
    case MESH_FIELD_HOP:
        mesh_safe_or_default(tmp, sizeof(tmp), text, "3");
        if(mesh_parse_u32_text(tmp, &value) != 0 || value > 7UL) {
            mesh_append_log("invalid hop limit: %s", text);
            return;
        }
        snprintf(mesh_hop_limit, sizeof(mesh_hop_limit), "%lu", value);
        break;
    case MESH_FIELD_ACK:
    case MESH_FIELD_REBROADCAST:
    case MESH_FIELD_POSITION:
    case MESH_FIELD_POSITION_INTERVAL:
    default:
        return;
    }

    mesh_normalize_target_ack();
    mesh_save_profile_prefs();
    mesh_settings_refresh();
    mesh_update_target_button();
    mesh_append_log("settings saved: %s=%s", mesh_setting_name(field),
                    mesh_setting_value(field, tmp, sizeof(tmp)));
}

static void mesh_choice_close(void)
{
    if(mesh_choice_overlay && lv_obj_is_valid(mesh_choice_overlay)) {
        lv_obj_delete(mesh_choice_overlay);
    }
    mesh_choice_overlay = NULL;
}

static void mesh_choice_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_choice_close();
}

static int mesh_choice_is_selected(mesh_setting_field_t field,
                                   const char *value)
{
    if(!value) {
        return 0;
    }
    switch(field) {
    case MESH_FIELD_REGION:
        return strcmp(mesh_region, value) == 0;
    case MESH_FIELD_PRESET:
        return strcmp(mesh_preset, value) == 0;
    case MESH_FIELD_POWER:
        if(mesh_power_text_is_auto(value)) {
            return mesh_power_is_auto();
        }
        return strcmp(mesh_tx_power, value) == 0;
    case MESH_FIELD_SLOT:
        if(mesh_slot_text_is_auto(value)) {
            return mesh_slot_is_auto();
        }
        return strcmp(mesh_frequency_slot, value) == 0;
    case MESH_FIELD_HOP:
        return strcmp(mesh_hop_limit, value) == 0;
    case MESH_FIELD_ACK:
        return mesh_ack_enabled == (strcmp(value, "0") != 0);
    case MESH_FIELD_REBROADCAST:
        return mesh_rebroadcast_enabled == (strcmp(value, "0") != 0);
    case MESH_FIELD_POSITION:
        return mesh_position_enabled == (strcmp(value, "0") != 0);
    case MESH_FIELD_POSITION_INTERVAL:
        return strcmp(mesh_position_interval, value) == 0;
    case MESH_FIELD_TELEMETRY:
        return mesh_telemetry_enabled == (strcmp(value, "0") != 0);
    case MESH_FIELD_TELEMETRY_INTERVAL:
        return strcmp(mesh_telemetry_device_interval, value) == 0;
    case MESH_FIELD_ENV_TELEMETRY:
        return mesh_environment_telemetry_enabled ==
               (strcmp(value, "0") != 0);
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        return strcmp(mesh_telemetry_environment_interval, value) == 0;
    case MESH_FIELD_PHOTO_REPEAT:
        return strcmp(mesh_photo_repeat, value) == 0;
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        return strcmp(mesh_photo_repair_rounds, value) == 0;
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        return strcmp(mesh_photo_repair_repeat, value) == 0;
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        return strcmp(mesh_photo_repair_window_ms, value) == 0;
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return strcmp(mesh_photo_cache_ttl_sec, value) == 0;
    default:
        return 0;
    }
}

static const char *mesh_choice_value_at(mesh_setting_field_t field, int index)
{
    switch(field) {
    case MESH_FIELD_REGION:
        if(index >= 0 && index < (int)(sizeof(mesh_region_choices) /
           sizeof(mesh_region_choices[0]))) {
            return mesh_region_choices[index].value;
        }
        break;
    case MESH_FIELD_PRESET:
        if(index >= 0 && index < (int)(sizeof(mesh_preset_choices) /
           sizeof(mesh_preset_choices[0]))) {
            return mesh_preset_choices[index].value;
        }
        break;
    case MESH_FIELD_POWER:
        if(index >= 0 && index < (int)(sizeof(mesh_power_choices) /
           sizeof(mesh_power_choices[0]))) {
            return mesh_power_choices[index].value;
        }
        break;
    case MESH_FIELD_SLOT:
        if(index >= 0 && index < (int)(sizeof(mesh_slot_choices) /
           sizeof(mesh_slot_choices[0]))) {
            return mesh_slot_choices[index].value;
        }
        break;
    case MESH_FIELD_HOP:
        if(index >= 0 && index < (int)(sizeof(mesh_hop_choices) /
           sizeof(mesh_hop_choices[0]))) {
            return mesh_hop_choices[index].value;
        }
        break;
    case MESH_FIELD_ACK:
    case MESH_FIELD_REBROADCAST:
    case MESH_FIELD_POSITION:
    case MESH_FIELD_TELEMETRY:
    case MESH_FIELD_ENV_TELEMETRY:
        if(index >= 0 && index < (int)(sizeof(mesh_bool_choices) /
           sizeof(mesh_bool_choices[0]))) {
            return mesh_bool_choices[index].value;
        }
        break;
    case MESH_FIELD_POSITION_INTERVAL:
    case MESH_FIELD_TELEMETRY_INTERVAL:
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        if(index >= 0 && index < (int)(sizeof(mesh_position_interval_choices) /
           sizeof(mesh_position_interval_choices[0]))) {
            return mesh_position_interval_choices[index].value;
        }
        break;
    case MESH_FIELD_PHOTO_REPEAT:
        if(index >= 0 && index < (int)(sizeof(mesh_photo_repeat_choices) /
           sizeof(mesh_photo_repeat_choices[0]))) {
            return mesh_photo_repeat_choices[index].value;
        }
        break;
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        if(index >= 0 &&
           index < (int)(sizeof(mesh_photo_repair_round_choices) /
           sizeof(mesh_photo_repair_round_choices[0]))) {
            return mesh_photo_repair_round_choices[index].value;
        }
        break;
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        if(index >= 0 &&
           index < (int)(sizeof(mesh_photo_repair_repeat_choices) /
           sizeof(mesh_photo_repair_repeat_choices[0]))) {
            return mesh_photo_repair_repeat_choices[index].value;
        }
        break;
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        if(index >= 0 &&
           index < (int)(sizeof(mesh_photo_repair_window_choices) /
           sizeof(mesh_photo_repair_window_choices[0]))) {
            return mesh_photo_repair_window_choices[index].value;
        }
        break;
    case MESH_FIELD_PHOTO_CACHE_TTL:
        if(index >= 0 && index < (int)(sizeof(mesh_photo_cache_ttl_choices) /
           sizeof(mesh_photo_cache_ttl_choices[0]))) {
            return mesh_photo_cache_ttl_choices[index].value;
        }
        break;
    default:
        break;
    }
    return NULL;
}

static void mesh_choice_apply(mesh_setting_field_t field, const char *value)
{
    char log_value[32];

    if(!value) {
        return;
    }
    switch(field) {
    case MESH_FIELD_REGION:
        mesh_safe_or_default(mesh_region, sizeof(mesh_region), value,
                             MESHTASTIC_DEFAULT_UI_REGION);
        if(!mesh_profile_supports_ui_preset(mesh_current_profile(),
                                            mesh_preset)) {
            snprintf(mesh_preset, sizeof(mesh_preset), "%s",
                     mesh_default_preset_for_region(mesh_region));
            mesh_append_log("preset adjusted for region: %s", mesh_preset);
        }
        break;
    case MESH_FIELD_PRESET:
        if(!mesh_profile_supports_ui_preset(mesh_current_profile(), value)) {
            mesh_append_log("invalid preset for %s: %s", mesh_region, value);
            return;
        }
        mesh_safe_or_default(mesh_preset, sizeof(mesh_preset), value,
                             mesh_default_preset_for_region(mesh_region));
        break;
    case MESH_FIELD_POWER:
        mesh_safe_or_default(mesh_tx_power, sizeof(mesh_tx_power), value,
                             "auto");
        mesh_normalize_power();
        break;
    case MESH_FIELD_SLOT:
        mesh_safe_or_default(mesh_frequency_slot, sizeof(mesh_frequency_slot),
                             value, "auto");
        mesh_normalize_slot();
        break;
    case MESH_FIELD_HOP:
        mesh_safe_or_default(mesh_hop_limit, sizeof(mesh_hop_limit), value,
                             "3");
        mesh_normalize_hop();
        break;
    case MESH_FIELD_ACK:
        mesh_ack_enabled = strcmp(value, "0") != 0;
        mesh_normalize_target_ack();
        break;
    case MESH_FIELD_REBROADCAST:
        mesh_rebroadcast_enabled = strcmp(value, "0") != 0;
        break;
    case MESH_FIELD_POSITION:
        mesh_position_enabled = strcmp(value, "0") != 0;
        break;
    case MESH_FIELD_POSITION_INTERVAL:
        mesh_safe_or_default(mesh_position_interval,
                             sizeof(mesh_position_interval), value, "900");
        break;
    case MESH_FIELD_TELEMETRY:
        mesh_telemetry_enabled = strcmp(value, "0") != 0;
        break;
    case MESH_FIELD_TELEMETRY_INTERVAL:
        mesh_safe_or_default(mesh_telemetry_device_interval,
                             sizeof(mesh_telemetry_device_interval), value,
                             "300");
        break;
    case MESH_FIELD_ENV_TELEMETRY:
        mesh_environment_telemetry_enabled = strcmp(value, "0") != 0;
        break;
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        mesh_safe_or_default(mesh_telemetry_environment_interval,
                             sizeof(mesh_telemetry_environment_interval),
                             value, "300");
        break;
    case MESH_FIELD_PHOTO_REPEAT:
        mesh_safe_or_default(mesh_photo_repeat, sizeof(mesh_photo_repeat),
                             value, "2");
        mesh_normalize_media_config();
        break;
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        mesh_safe_or_default(mesh_photo_repair_rounds,
                             sizeof(mesh_photo_repair_rounds), value, "2");
        mesh_normalize_media_config();
        break;
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        mesh_safe_or_default(mesh_photo_repair_repeat,
                             sizeof(mesh_photo_repair_repeat), value, "2");
        mesh_normalize_media_config();
        break;
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        mesh_safe_or_default(mesh_photo_repair_window_ms,
                             sizeof(mesh_photo_repair_window_ms), value,
                             "7000");
        mesh_normalize_media_config();
        break;
    case MESH_FIELD_PHOTO_CACHE_TTL:
        mesh_safe_or_default(mesh_photo_cache_ttl_sec,
                             sizeof(mesh_photo_cache_ttl_sec), value, "300");
        mesh_normalize_media_config();
        break;
    default:
        return;
    }
    mesh_save_profile_prefs();
    mesh_update_target_button();
    if(field == MESH_FIELD_PHOTO_REPEAT ||
       field == MESH_FIELD_PHOTO_REPAIR_ROUNDS ||
       field == MESH_FIELD_PHOTO_REPAIR_REPEAT ||
       field == MESH_FIELD_PHOTO_REPAIR_WINDOW ||
       field == MESH_FIELD_PHOTO_CACHE_TTL) {
        (void)mesh_apply_media_runtime_config();
    }
    mesh_settings_refresh();
    mesh_append_log("settings saved: %s=%s", mesh_setting_name(field),
                    mesh_setting_value(field, log_value, sizeof(log_value)));
}

static void mesh_choice_event_cb(lv_event_t *event)
{
    intptr_t code = (intptr_t)lv_event_get_user_data(event);
    mesh_setting_field_t field =
        (mesh_setting_field_t)((code >> 16) & 0xffff);
    int index = (int)(code & 0xffff);
    const char *value = mesh_choice_value_at(field, index);

    mesh_choice_apply(field, value);
    mesh_choice_close();
}

static void mesh_style_choice_button(lv_obj_t *btn, int selected)
{
    uint32_t count;

    if(!btn || !lv_obj_is_valid(btn)) {
        return;
    }
    lv_obj_set_style_bg_color(btn,
                              lv_color_hex(selected ? 0x173B2A : 0x151B22),
                              0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(selected ? 0x25C281 :
                                                       0x2A3037),
                                  0);
    count = lv_obj_get_child_count(btn);
    for(uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        lv_obj_set_style_text_color(child,
                                    lv_color_hex(selected ? 0xFFFFFF :
                                                          0xD7DEE8),
                                    0);
    }
}

static int mesh_choice_should_show(mesh_setting_field_t field, int index)
{
    if(field == MESH_FIELD_PRESET &&
       index >= 0 && index < (int)(sizeof(mesh_preset_choices) /
       sizeof(mesh_preset_choices[0]))) {
        unsigned mask = MESH_PROFILE_MASK(mesh_current_profile());
        return (mesh_preset_choices[index].profiles & mask) != 0U;
    }
    return 1;
}

static const char *mesh_choice_label_at(mesh_setting_field_t field, int index)
{
    switch(field) {
    case MESH_FIELD_REGION:
        return mesh_region_choices[index].label;
    case MESH_FIELD_PRESET:
        return mesh_preset_choices[index].label;
    case MESH_FIELD_POWER:
        return mesh_power_choices[index].label;
    case MESH_FIELD_SLOT:
        return mesh_slot_choices[index].label;
    case MESH_FIELD_HOP:
        return mesh_hop_choices[index].label;
    case MESH_FIELD_ACK:
    case MESH_FIELD_REBROADCAST:
    case MESH_FIELD_POSITION:
    case MESH_FIELD_TELEMETRY:
    case MESH_FIELD_ENV_TELEMETRY:
        return mesh_bool_choices[index].label;
    case MESH_FIELD_POSITION_INTERVAL:
    case MESH_FIELD_TELEMETRY_INTERVAL:
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        return mesh_position_interval_choices[index].label;
    case MESH_FIELD_PHOTO_REPEAT:
        return mesh_photo_repeat_choices[index].label;
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        return mesh_photo_repair_round_choices[index].label;
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        return mesh_photo_repair_repeat_choices[index].label;
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        return mesh_photo_repair_window_choices[index].label;
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return mesh_photo_cache_ttl_choices[index].label;
    default:
        return "";
    }
}

static int mesh_choice_count(mesh_setting_field_t field)
{
    switch(field) {
    case MESH_FIELD_REGION:
        return (int)(sizeof(mesh_region_choices) / sizeof(mesh_region_choices[0]));
    case MESH_FIELD_PRESET:
        return (int)(sizeof(mesh_preset_choices) / sizeof(mesh_preset_choices[0]));
    case MESH_FIELD_POWER:
        return (int)(sizeof(mesh_power_choices) / sizeof(mesh_power_choices[0]));
    case MESH_FIELD_SLOT:
        return (int)(sizeof(mesh_slot_choices) / sizeof(mesh_slot_choices[0]));
    case MESH_FIELD_HOP:
        return (int)(sizeof(mesh_hop_choices) / sizeof(mesh_hop_choices[0]));
    case MESH_FIELD_ACK:
    case MESH_FIELD_REBROADCAST:
    case MESH_FIELD_POSITION:
    case MESH_FIELD_TELEMETRY:
    case MESH_FIELD_ENV_TELEMETRY:
        return (int)(sizeof(mesh_bool_choices) / sizeof(mesh_bool_choices[0]));
    case MESH_FIELD_POSITION_INTERVAL:
    case MESH_FIELD_TELEMETRY_INTERVAL:
    case MESH_FIELD_ENV_TELEMETRY_INTERVAL:
        return (int)(sizeof(mesh_position_interval_choices) /
                     sizeof(mesh_position_interval_choices[0]));
    case MESH_FIELD_PHOTO_REPEAT:
        return (int)(sizeof(mesh_photo_repeat_choices) /
                     sizeof(mesh_photo_repeat_choices[0]));
    case MESH_FIELD_PHOTO_REPAIR_ROUNDS:
        return (int)(sizeof(mesh_photo_repair_round_choices) /
                     sizeof(mesh_photo_repair_round_choices[0]));
    case MESH_FIELD_PHOTO_REPAIR_REPEAT:
        return (int)(sizeof(mesh_photo_repair_repeat_choices) /
                     sizeof(mesh_photo_repair_repeat_choices[0]));
    case MESH_FIELD_PHOTO_REPAIR_WINDOW:
        return (int)(sizeof(mesh_photo_repair_window_choices) /
                     sizeof(mesh_photo_repair_window_choices[0]));
    case MESH_FIELD_PHOTO_CACHE_TTL:
        return (int)(sizeof(mesh_photo_cache_ttl_choices) /
                     sizeof(mesh_photo_cache_ttl_choices[0]));
    default:
        return 0;
    }
}

static void mesh_choice_open(mesh_setting_field_t field)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int cols = ui_is_landscape() ? 3 : 2;
    int gap = 12;
    int col_w;
    int row_h = 72;
    int x;
    int y = 104;
    int visible = 0;
    int total = mesh_choice_count(field);

    if(!mesh_setting_uses_choice(field)) {
        return;
    }
    if(cols < 1) {
        cols = 1;
    }
    col_w = (content_w - gap * (cols - 1)) / cols;
    if(col_w < 128) {
        cols = 1;
        col_w = content_w;
    }

    mesh_choice_close();
    mesh_choice_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_choice_overlay);
    lv_obj_set_style_bg_color(mesh_choice_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_choice_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_choice_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_choice_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_choice_overlay, 0, 0);
    lv_obj_clear_flag(mesh_choice_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_choice_overlay);

    panel = ui_scroll_panel(mesh_choice_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr(mesh_setting_name(field)), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    subtitle = ui_label(panel,
                        ui_tr(field == MESH_FIELD_PRESET ?
                        "Preset list is filtered by current region" :
                        field == MESH_FIELD_SLOT ?
                        "Auto derives frequency from channel name" :
                        "Select one option"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 112);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"),
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_choice_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    for(int i = 0; i < total; i++) {
        const char *value;
        const char *label;
        int selected;

        if(!mesh_choice_should_show(field, i)) {
            continue;
        }
        value = mesh_choice_value_at(field, i);
        label = mesh_choice_label_at(field, i);
        selected = mesh_choice_is_selected(field, value);
        x = margin + (visible % cols) * (col_w + gap);
        y = 104 + (visible / cols) * row_h;
        btn = ui_command_button(panel, x, y, col_w, ui_tr(label), 0xD7DEE8);
        mesh_style_choice_button(btn, selected);
        lv_obj_add_event_cb(btn, mesh_choice_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(((int)field << 16) | i));
        visible++;
    }
}

static void mesh_setting_edit_event_cb(lv_event_t *event)
{
    mesh_setting_field_t field =
        (mesh_setting_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;
    char placeholder[96];
    char value[32];

    if(mesh_setting_uses_choice(field)) {
        mesh_choice_open(field);
        return;
    }

    snprintf(placeholder, sizeof(placeholder), "%s",
             mesh_setting_value(field, value, sizeof(value)));
    if(field == MESH_FIELD_CHANNEL && strcmp(placeholder, "<preset>") == 0) {
        snprintf(placeholder, sizeof(placeholder), "-");
    }

    memset(&config, 0, sizeof(config));
    config.title = ui_tr(mesh_setting_name(field));
    config.placeholder = placeholder;
    config.password_mode = field == MESH_FIELD_PSK;
    config.max_length = field == MESH_FIELD_PSK ? 80 :
                        field == MESH_FIELD_CHANNEL ? 12 : 64;
    config.submit_cb = mesh_setting_submit_cb;
    config.user_data = (void *)(intptr_t)field;
    config.submit_text = ui_tr("Save");
    config.cancel_text = ui_tr("Cancel");
    ui_input_dialog_open(&config);
}

static void mesh_close_settings_page(void)
{
    mesh_choice_close();
    mesh_channel_profiles_close();
    mesh_close_pairing_notice();
    mesh_log_close();
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    mesh_settings_status_card = NULL;
    memset(mesh_settings_status_labels, 0, sizeof(mesh_settings_status_labels));
    mesh_log_label = NULL;
    mesh_publish_status_label = NULL;
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
}

static void mesh_close_nodes_page(void)
{
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_overlay = NULL;
    mesh_nodes_panel = NULL;
    mesh_nodes_overlay_kind = MESH_NODES_OVERLAY_NONE;
    mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_nodes_preserve_scroll = 0;
    mesh_node_detail_refresh_ticks = 0;
    mesh_node_detail_refresh_attempts = 0;
    mesh_node_detail_target_id[0] = '\0';
    mesh_node_detail_status_text[0] = '\0';
    mesh_node_detail_status_color = 0x94A3B8;
    mesh_node_request_status_label = NULL;
}

static void mesh_channel_scan_overlay_close(void)
{
    if(mesh_channel_scan_overlay &&
       lv_obj_is_valid(mesh_channel_scan_overlay)) {
        lv_obj_delete(mesh_channel_scan_overlay);
    }
    mesh_channel_scan_overlay = NULL;
    mesh_channel_scan_preview_image = NULL;
    mesh_channel_scan_preview_placeholder = NULL;
    mesh_channel_scan_preview_status_label = NULL;
    mesh_channel_scan_guide_box = NULL;
    mesh_channel_scan_preview_panel_w = 0;
    mesh_channel_scan_preview_panel_h = 0;
}

static void mesh_channel_import_confirm_close(void)
{
    if(mesh_channel_import_overlay &&
       lv_obj_is_valid(mesh_channel_import_overlay)) {
        lv_obj_delete(mesh_channel_import_overlay);
    }
    mesh_channel_import_overlay = NULL;
    mesh_channel_import_status_label = NULL;
}

static void mesh_channel_qr_fullscreen_close(void)
{
    if(mesh_channel_qr_fullscreen_overlay &&
       lv_obj_is_valid(mesh_channel_qr_fullscreen_overlay)) {
        lv_obj_delete(mesh_channel_qr_fullscreen_overlay);
    }
    mesh_channel_qr_fullscreen_overlay = NULL;
    mesh_channel_qr_fullscreen_canvas = NULL;
    if(mesh_channel_qr_fullscreen_buf) {
        free(mesh_channel_qr_fullscreen_buf);
        mesh_channel_qr_fullscreen_buf = NULL;
    }
}

static void mesh_channel_qr_fullscreen_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_channel_qr_fullscreen_close();
}

static void mesh_close_channel_page(void)
{
    if(mesh_channel_scan_timer) {
        lv_timer_delete(mesh_channel_scan_timer);
        mesh_channel_scan_timer = NULL;
    }
    mesh_channel_scan_overlay_close();
    mesh_channel_import_confirm_close();
    mesh_channel_qr_fullscreen_close();
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        lv_obj_delete(mesh_channel_overlay);
    }
    mesh_channel_overlay = NULL;
    mesh_channel_url_label = NULL;
    mesh_channel_status_label = NULL;
    mesh_channel_qr_canvas = NULL;
}

static void mesh_channel_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_close_channel_page();
}

static void mesh_channel_scan_overlay_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_channel_scan_overlay_close();
}

static void mesh_channel_scan_preview_update(void)
{
    FILE *fp;
    size_t n;
    int scale_w;
    int scale_h;
    int scale;

    if(!mesh_channel_scan_preview_image ||
       !lv_obj_is_valid(mesh_channel_scan_preview_image)) {
        return;
    }
    if(!mesh_channel_scan_preview_pixels) {
        mesh_channel_scan_preview_pixels = malloc(MESHTASTIC_QR_PREVIEW_BYTES);
        if(!mesh_channel_scan_preview_pixels) {
            return;
        }
    }

    fp = fopen(MESHTASTIC_QR_PREVIEW_FILE, "rb");
    if(!fp) {
        return;
    }
    n = fread(mesh_channel_scan_preview_pixels, 1,
              MESHTASTIC_QR_PREVIEW_BYTES, fp);
    fclose(fp);
    if(n != MESHTASTIC_QR_PREVIEW_BYTES) {
        return;
    }

    memset(&mesh_channel_scan_preview_dsc, 0,
           sizeof(mesh_channel_scan_preview_dsc));
    mesh_channel_scan_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    mesh_channel_scan_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    mesh_channel_scan_preview_dsc.header.w = MESHTASTIC_QR_PREVIEW_W;
    mesh_channel_scan_preview_dsc.header.h = MESHTASTIC_QR_PREVIEW_H;
    mesh_channel_scan_preview_dsc.header.stride = MESHTASTIC_QR_PREVIEW_W * 2;
    mesh_channel_scan_preview_dsc.data_size = MESHTASTIC_QR_PREVIEW_BYTES;
    mesh_channel_scan_preview_dsc.data = mesh_channel_scan_preview_pixels;

    lv_image_cache_drop(&mesh_channel_scan_preview_dsc);
    lv_image_set_src(mesh_channel_scan_preview_image,
                     &mesh_channel_scan_preview_dsc);
    scale_w = mesh_channel_scan_preview_panel_w > 0 ?
              mesh_channel_scan_preview_panel_w * 256 /
              MESHTASTIC_QR_PREVIEW_W : 256;
    scale_h = mesh_channel_scan_preview_panel_h > 0 ?
              mesh_channel_scan_preview_panel_h * 256 /
              MESHTASTIC_QR_PREVIEW_H : 256;
    scale = scale_w < scale_h ? scale_w : scale_h;
    if(scale < 128) {
        scale = 128;
    }
    if(scale > 512) {
        scale = 512;
    }
    lv_image_set_scale(mesh_channel_scan_preview_image, scale);
    lv_obj_center(mesh_channel_scan_preview_image);
    lv_obj_clear_flag(mesh_channel_scan_preview_image, LV_OBJ_FLAG_HIDDEN);
    if(mesh_channel_scan_preview_placeholder &&
       lv_obj_is_valid(mesh_channel_scan_preview_placeholder)) {
        lv_obj_add_flag(mesh_channel_scan_preview_placeholder,
                        LV_OBJ_FLAG_HIDDEN);
    }
}

static void mesh_channel_scan_overlay_show(void)
{
    lv_obj_t *panel;
    lv_obj_t *preview;
    lv_obj_t *title;
    lv_obj_t *btn;
    lv_obj_t *guide_hint;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int title_y = ui_is_landscape() ? 18 : 28;
    int preview_y = ui_is_landscape() ? 72 : 92;
    int status_y;
    int preview_h = screen_h - preview_y - 112;
    int guide_size;

    if(preview_h < 220) {
        preview_h = 220;
    }
    if(preview_h > screen_h - preview_y - 84) {
        preview_h = screen_h - preview_y - 84;
    }

    mesh_channel_scan_overlay_close();
    mesh_channel_scan_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_channel_scan_overlay);
    lv_obj_set_style_bg_color(mesh_channel_scan_overlay,
                              lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_channel_scan_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mesh_channel_scan_overlay, 0, 0);
    lv_obj_set_style_radius(mesh_channel_scan_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_scan_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_scan_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_channel_scan_overlay);

    title = ui_label(mesh_channel_scan_overlay, ui_tr("Scan channel QR"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, margin, title_y);
    lv_obj_set_width(title, content_w - 116);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    btn = ui_command_button(mesh_channel_scan_overlay,
                            screen_w - margin - 96, title_y - 4,
                            96, ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_channel_scan_overlay_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    preview = ui_panel(mesh_channel_scan_overlay, margin, preview_y,
                       content_w, preview_h);
    lv_obj_set_style_bg_color(preview, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x243044), 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);

    mesh_channel_scan_preview_panel_w = content_w - 24;
    mesh_channel_scan_preview_panel_h = preview_h - 24;
    mesh_channel_scan_preview_image = lv_image_create(preview);
    lv_obj_add_flag(mesh_channel_scan_preview_image, LV_OBJ_FLAG_HIDDEN);

    mesh_channel_scan_preview_placeholder =
        ui_label(preview, ui_tr("Point camera at Meshtastic QR"),
                 &lv_font_montserrat_20, 0x94A3B8);
    lv_obj_set_width(mesh_channel_scan_preview_placeholder,
                     content_w - 48);
    lv_label_set_long_mode(mesh_channel_scan_preview_placeholder,
                           LV_LABEL_LONG_WRAP);
    lv_obj_center(mesh_channel_scan_preview_placeholder);

    guide_size = mesh_channel_scan_preview_panel_w <
                 mesh_channel_scan_preview_panel_h ?
                 mesh_channel_scan_preview_panel_w :
                 mesh_channel_scan_preview_panel_h;
    guide_size = (guide_size * 70) / 100;
    if(guide_size < 160) {
        guide_size = 160;
    }
    mesh_channel_scan_guide_box = lv_obj_create(preview);
    lv_obj_set_size(mesh_channel_scan_guide_box, guide_size, guide_size);
    lv_obj_center(mesh_channel_scan_guide_box);
    lv_obj_set_style_bg_opa(mesh_channel_scan_guide_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mesh_channel_scan_guide_box, 3, 0);
    lv_obj_set_style_border_color(mesh_channel_scan_guide_box,
                                  lv_color_hex(0x25C281), 0);
    lv_obj_set_style_radius(mesh_channel_scan_guide_box, 16, 0);
    lv_obj_clear_flag(mesh_channel_scan_guide_box, LV_OBJ_FLAG_SCROLLABLE);

    guide_hint = ui_label(preview, ui_tr("Keep QR inside the frame"),
                          &lv_font_montserrat_14, 0xD1FAE5);
    lv_obj_set_width(guide_hint, content_w - 48);
    lv_obj_align(guide_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_text_align(guide_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(guide_hint, LV_LABEL_LONG_DOT);

    status_y = preview_y + preview_h + 16;
    if(status_y > screen_h - 40) {
        status_y = screen_h - 40;
    }
    panel = mesh_channel_scan_overlay;
    mesh_channel_scan_preview_status_label =
        ui_label(panel, ui_tr("Scanning channel QR..."),
                 &lv_font_montserrat_16, 0xF5A524);
    lv_obj_set_pos(mesh_channel_scan_preview_status_label, margin, status_y);
    lv_obj_set_width(mesh_channel_scan_preview_status_label, content_w);
    lv_label_set_long_mode(mesh_channel_scan_preview_status_label,
                           LV_LABEL_LONG_DOT);
}

static int mesh_channel_url_fetch(char *out, size_t out_len,
                                  char *status, size_t status_len)
{
    char response[1280];
    const char *prefix = "OK channel_url=";
    const char *url;

    if(out && out_len > 0U) {
        out[0] = '\0';
    }
    if(status && status_len > 0U) {
        status[0] = '\0';
    }
    if(mesh_ipc_command("CHANNEL_URL\n", response, sizeof(response)) != 0) {
        ui_trim_text(response);
        if(status && status_len > 0U) {
            snprintf(status, status_len, "%s",
                     response[0] ? response : "Channel URL unavailable");
        }
        return -1;
    }
    ui_trim_text(response);
    if(strncmp(response, prefix, strlen(prefix)) != 0) {
        if(status && status_len > 0U) {
            snprintf(status, status_len, "%s", response);
        }
        return -1;
    }
    url = response + strlen(prefix);
    if(!url[0]) {
        if(status && status_len > 0U) {
            snprintf(status, status_len, "%s", "Empty channel URL");
        }
        return -1;
    }
    snprintf(out, out_len, "%s", url);
    if(status && status_len > 0U) {
        snprintf(status, status_len, "%s", "Channel URL ready");
    }
    return 0;
}

static uint16_t mesh_rgb565(uint32_t rgb)
{
    uint8_t r = (uint8_t)((rgb >> 16U) & 0xffU);
    uint8_t g = (uint8_t)((rgb >> 8U) & 0xffU);
    uint8_t b = (uint8_t)(rgb & 0xffU);

    return (uint16_t)(((uint16_t)(r & 0xf8U) << 8U) |
                      ((uint16_t)(g & 0xfcU) << 3U) |
                      ((uint16_t)b >> 3U));
}

static void mesh_channel_qr_render_into(lv_obj_t *canvas, uint16_t *buf,
                                        const char *url, int px)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    uint16_t black = mesh_rgb565(0x05070A);
    uint16_t white = mesh_rgb565(0xF8FAFC);
    uint16_t empty = mesh_rgb565(0x17212B);
    int qr_size = 0;
    int scale = 1;
    int image_px;
    int offset;
    bool ok = false;

    if(!canvas || !lv_obj_is_valid(canvas) || !buf) {
        return;
    }
    if(px < 64) {
        px = 64;
    }
    if(px > MESHTASTIC_CHANNEL_QR_MAX) {
        px = MESHTASTIC_CHANNEL_QR_MAX;
    }

    for(int i = 0; i < px * px; i++) {
        buf[i] = empty;
    }
    if(url && url[0]) {
        ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                  qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX,
                                  qrcodegen_Mask_AUTO, true);
    }
    if(ok) {
        qr_size = qrcodegen_getSize(qr);
        if(qr_size > 0) {
            scale = px / (qr_size + MESHTASTIC_CHANNEL_QR_BORDER * 2);
            if(scale < 1) {
                scale = 1;
            }
            image_px = (qr_size + MESHTASTIC_CHANNEL_QR_BORDER * 2) * scale;
            offset = (px - image_px) / 2;
            if(offset < 0) {
                offset = 0;
            }
            for(int y = 0; y < px; y++) {
                for(int x = 0; x < px; x++) {
                    int mx = (x - offset) / scale - MESHTASTIC_CHANNEL_QR_BORDER;
                    int my = (y - offset) / scale - MESHTASTIC_CHANNEL_QR_BORDER;
                    bool module = false;

                    if(x >= offset && y >= offset && x < offset + image_px &&
                       y < offset + image_px && mx >= 0 && my >= 0 &&
                       mx < qr_size && my < qr_size) {
                        module = qrcodegen_getModule(qr, mx, my);
                    }
                    buf[y * px + x] = module ? black : white;
                }
            }
        }
    }
    lv_canvas_set_buffer(canvas, buf, px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
}

static void mesh_channel_qr_render(const char *url, int px)
{
    mesh_channel_qr_render_into(mesh_channel_qr_canvas, mesh_channel_qr_buf,
                                url, px);
}

static void mesh_channel_set_status(const char *text, uint32_t color)
{
    if(mesh_channel_status_label &&
       lv_obj_is_valid(mesh_channel_status_label)) {
        lv_label_set_text(mesh_channel_status_label,
                          text && text[0] ? ui_tr(text) : ui_tr("Ready"));
        lv_obj_set_style_text_color(mesh_channel_status_label,
                                    lv_color_hex(color), 0);
    }
}

static void mesh_channel_refresh_view(int ok, const char *status, int qr_px)
{
    if(mesh_channel_url_label && lv_obj_is_valid(mesh_channel_url_label)) {
        lv_label_set_text(mesh_channel_url_label,
                          mesh_channel_url_text[0] ? mesh_channel_url_text :
                          ui_tr("Channel URL unavailable"));
        lv_obj_set_style_text_color(mesh_channel_url_label,
                                    lv_color_hex(ok ? 0xD7DEE8 : 0xF5A524),
                                    0);
    }
    mesh_channel_set_status(status && status[0] ? status : "Ready",
                            ok ? 0x25C281 : 0xF5A524);
    mesh_channel_qr_render(mesh_channel_url_text, qr_px);
}

static void mesh_channel_qr_fullscreen_show(void)
{
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int safe = ui_is_landscape() ? 42 : 32;
    int qr_px = screen_w < screen_h ? screen_w : screen_h;
    int qr_x;
    int qr_y;

    if(mesh_channel_qr_fullscreen_overlay &&
       lv_obj_is_valid(mesh_channel_qr_fullscreen_overlay)) {
        mesh_channel_qr_fullscreen_close();
    }
    qr_px -= safe * 2;
    if(qr_px > MESHTASTIC_CHANNEL_QR_MAX) {
        qr_px = MESHTASTIC_CHANNEL_QR_MAX;
    }
    if(qr_px < 180) {
        qr_px = 180;
    }
    mesh_channel_qr_fullscreen_buf =
        malloc((size_t)MESHTASTIC_CHANNEL_QR_MAX *
               MESHTASTIC_CHANNEL_QR_MAX * sizeof(uint16_t));
    if(!mesh_channel_qr_fullscreen_buf) {
        mesh_channel_set_status("QR scan failed", 0xEF4D5A);
        return;
    }

    mesh_channel_qr_fullscreen_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_channel_qr_fullscreen_overlay);
    lv_obj_set_style_bg_color(mesh_channel_qr_fullscreen_overlay,
                              lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_channel_qr_fullscreen_overlay,
                            LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_channel_qr_fullscreen_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_channel_qr_fullscreen_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_qr_fullscreen_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_qr_fullscreen_overlay,
                      LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mesh_channel_qr_fullscreen_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mesh_channel_qr_fullscreen_overlay,
                        mesh_channel_qr_fullscreen_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_channel_qr_fullscreen_canvas =
        lv_canvas_create(mesh_channel_qr_fullscreen_overlay);
    qr_x = (screen_w - qr_px) / 2;
    qr_y = (screen_h - qr_px) / 2;
    if(qr_x < safe) {
        qr_x = safe;
    }
    if(qr_y < safe) {
        qr_y = safe;
    }
    lv_obj_set_pos(mesh_channel_qr_fullscreen_canvas, qr_x, qr_y);
    mesh_channel_qr_render_into(mesh_channel_qr_fullscreen_canvas,
                                mesh_channel_qr_fullscreen_buf,
                                mesh_channel_url_text, qr_px);
    lv_obj_move_foreground(mesh_channel_qr_fullscreen_overlay);
}

static void mesh_channel_qr_fullscreen_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_channel_qr_fullscreen_show();
}

static void mesh_channel_preview_value(const char *text, const char *key,
                                       char *out, size_t out_len,
                                       const char *fallback)
{
    const char *p;
    size_t i = 0;

    if(!out || out_len == 0U) {
        return;
    }
    snprintf(out, out_len, "%s", fallback ? fallback : "-");
    if(!text || !key || !key[0]) {
        return;
    }
    p = strstr(text, key);
    if(!p) {
        return;
    }
    p += strlen(key);
    while(*p && !isspace((unsigned char)*p) && i + 1U < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    if(!out[0]) {
        snprintf(out, out_len, "%s", fallback ? fallback : "-");
    }
}

static void mesh_channel_preview_format(const char *response,
                                        char *out, size_t out_len)
{
    char region[32];
    char preset[48];
    char channel[80];
    char psk[80];
    char hop[16];
    char slot[16];
    char freq[32];
    char bw[24];
    char sf[16];
    char power[16];

    if(!out || out_len == 0U) {
        return;
    }
    mesh_channel_preview_value(response, "region=", region, sizeof(region),
                               "-");
    mesh_channel_preview_value(response, "preset=", preset, sizeof(preset),
                               "-");
    mesh_channel_preview_value(response, "channel=", channel, sizeof(channel),
                               "-");
    mesh_channel_preview_value(response, "psk=", psk, sizeof(psk), "-");
    mesh_channel_preview_value(response, "hop=", hop, sizeof(hop), "-");
    mesh_channel_preview_value(response, "slot=", slot, sizeof(slot), "-");
    mesh_channel_preview_value(response, "freq=", freq, sizeof(freq), "-");
    mesh_channel_preview_value(response, "bw=", bw, sizeof(bw), "-");
    mesh_channel_preview_value(response, "sf=", sf, sizeof(sf), "-");
    mesh_channel_preview_value(response, "power=", power, sizeof(power),
                               "-");
    snprintf(out, out_len,
             "Region  %s\nPreset  %s\nChannel %s\nPSK      %s\nHop %s  Slot %s\nFreq %s MHz  BW %s kHz\nSF %s  Power %s dBm",
             region, preset, channel, psk, hop, slot, freq, bw, sf, power);
}

static void mesh_channel_import_apply_event_cb(lv_event_t *event)
{
    char command[1200];
    char response[1280];
    char fetch_status[160];
    int ok = 0;
    int qr_px;

    if(event) {
        lv_event_stop_processing(event);
    }
    if(!mesh_channel_import_pending_url[0]) {
        return;
    }
    snprintf(command, sizeof(command), "IMPORT_CHANNEL_URL %s\n",
             mesh_channel_import_pending_url);
    response[0] = '\0';
    if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
        ui_trim_text(response);
        ok = strncmp(response, "OK imported", 11) == 0;
    } else {
        ui_trim_text(response);
    }

    if(!ok) {
        const char *status = response[0] ? response : "Invalid Meshtastic QR";

        if(mesh_channel_import_status_label &&
           lv_obj_is_valid(mesh_channel_import_status_label)) {
            lv_label_set_text(mesh_channel_import_status_label, status);
            lv_obj_set_style_text_color(mesh_channel_import_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
        mesh_channel_set_status(status, 0xEF4D5A);
        mesh_append_log("channel QR import failed: %s", status);
        return;
    }

    qr_px = (mesh_channel_qr_canvas &&
             lv_obj_is_valid(mesh_channel_qr_canvas)) ?
            lv_obj_get_width(mesh_channel_qr_canvas) : 0;
    if(qr_px <= 0) {
        qr_px = ui_is_landscape() ? 220 : 240;
    }
    mesh_load_profile_prefs();
    mesh_settings_refresh();
    mesh_update_profile_label();
    mesh_channel_url_text[0] = '\0';
    (void)mesh_channel_url_fetch(mesh_channel_url_text,
                                 sizeof(mesh_channel_url_text),
                                 fetch_status, sizeof(fetch_status));
    mesh_channel_refresh_view(1, ui_tr("Channel imported"), qr_px);
    mesh_append_log("channel QR import: %s", response);
    mesh_channel_import_confirm_close();
}

static void mesh_channel_import_cancel_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_channel_import_confirm_close();
}

static void mesh_channel_import_url_submit_cb(const char *text,
                                              void *user_data)
{
    char url[1024];
    char command[1200];
    char response[1280];
    int ok = 0;

    (void)user_data;
    snprintf(url, sizeof(url), "%s", text ? text : "");
    ui_trim_text(url);
    if(!url[0]) {
        mesh_channel_set_status("No channel URL to import", 0xF5A524);
        return;
    }

    snprintf(command, sizeof(command), "PREVIEW_CHANNEL_URL %s\n", url);
    response[0] = '\0';
    if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
        ui_trim_text(response);
        ok = strncmp(response, "OK preview", 10) == 0;
    } else {
        ui_trim_text(response);
    }

    if(!ok) {
        mesh_channel_set_status(response[0] ? response : "Invalid Meshtastic URL",
                                0xEF4D5A);
        mesh_append_log("channel URL preview failed: %s",
                        response[0] ? response : url);
        return;
    }

    mesh_channel_import_confirm_show(url, response);
    mesh_channel_set_status("Channel URL ready", 0x25C281);
    mesh_append_log("channel URL preview: %s", response);
}

static void mesh_channel_import_url_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    if(event) {
        lv_event_stop_processing(event);
    }
    memset(&config, 0, sizeof(config));
    config.title = ui_tr("Import URL");
    config.placeholder = "https://meshtastic.org/e/#...";
    config.initial_text = "";
    config.max_length = sizeof(mesh_channel_import_pending_url) - 1;
    config.submit_cb = mesh_channel_import_url_submit_cb;
    config.submit_text = ui_tr("Preview");
    config.cancel_text = ui_tr("Cancel");
    ui_input_dialog_open(&config);
}

static void mesh_channel_save_profile_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    if(mesh_channel_profile_write_current() == 0) {
        mesh_channel_set_status("Channel profile saved", 0x25C281);
    } else {
        mesh_channel_set_status("Save failed", 0xEF4D5A);
    }
}

static void mesh_channel_import_confirm_show(const char *url,
                                             const char *preview_response)
{
    lv_obj_t *dialog;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *summary_label;
    lv_obj_t *url_label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int dialog_w = ui_is_landscape() ? 620 : 520;
    int dialog_h = ui_is_landscape() ? 360 : 430;
    int pad = 24;
    int gap = 18;
    int button_w;
    int button_y;
    char summary[512];

    if(!url || !url[0]) {
        return;
    }
    mesh_channel_import_confirm_close();
    snprintf(mesh_channel_import_pending_url,
             sizeof(mesh_channel_import_pending_url), "%s", url);
    mesh_channel_preview_format(preview_response, summary, sizeof(summary));

    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 48) {
        dialog_h = screen_h - 48;
    }
    if(dialog_h < 300) {
        dialog_h = 300;
    }
    button_w = (dialog_w - pad * 2 - gap) / 2;
    button_y = dialog_h - pad - 58;

    mesh_channel_import_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_channel_import_overlay);
    lv_obj_set_style_bg_color(mesh_channel_import_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_channel_import_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(mesh_channel_import_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_import_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_import_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(mesh_channel_import_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x24402F), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    title = ui_label(dialog, ui_tr("Import channel?"),
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, pad, pad);
    lv_obj_set_width(title, dialog_w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    subtitle = ui_label(dialog, ui_tr("Review scanned channel before applying"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, pad, pad + 36);
    lv_obj_set_width(subtitle, dialog_w - pad * 2);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    summary_label = ui_label(dialog, summary, &lv_font_montserrat_16,
                             0xD7DEE8);
    lv_obj_set_pos(summary_label, pad, pad + 70);
    lv_obj_set_width(summary_label, dialog_w - pad * 2);
    lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);

    url_label = ui_label(dialog, url, &lv_font_montserrat_12, 0x64748B);
    lv_obj_set_pos(url_label, pad, button_y - 50);
    lv_obj_set_width(url_label, dialog_w - pad * 2);
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_DOT);

    mesh_channel_import_status_label =
        ui_label(dialog, "", &lv_font_montserrat_14, 0xEF4D5A);
    lv_obj_set_pos(mesh_channel_import_status_label, pad, button_y - 24);
    lv_obj_set_width(mesh_channel_import_status_label, dialog_w - pad * 2);
    lv_label_set_long_mode(mesh_channel_import_status_label,
                           LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, button_y, button_w,
                            ui_tr("Cancel"), 0x374151);
    lv_obj_set_height(btn, 58);
    lv_obj_add_event_cb(btn, mesh_channel_import_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + button_w + gap, button_y,
                            button_w, ui_tr("Apply channel"), 0x25C281);
    lv_obj_set_height(btn, 58);
    lv_obj_add_event_cb(btn, mesh_channel_import_apply_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void *mesh_channel_scan_worker(void *arg)
{
    char url[1024];
    char command[1200];
    char response[1280];
    FILE *fp;
    int ok = 0;

    (void)arg;
    url[0] = '\0';
    response[0] = '\0';

    snprintf(command, sizeof(command),
             MESHTASTIC_QR_SCAN_PATH
             " -w %d -h %d --skip 2"
             " --timeout-sec 18 --preview-file " MESHTASTIC_QR_PREVIEW_FILE
             " --preview-width %d --preview-height %d "
             "--preview-interval-ms 100 --verbose 2>/tmp/k230_qr_scan.log",
             MESHTASTIC_QR_CAPTURE_W, MESHTASTIC_QR_CAPTURE_H,
             MESHTASTIC_QR_PREVIEW_W, MESHTASTIC_QR_PREVIEW_H);
    fp = popen(command, "r");
    if(fp) {
        if(fgets(url, sizeof(url), fp)) {
            ui_trim_text(url);
        }
        if(pclose(fp) == 0 && url[0]) {
            snprintf(command, sizeof(command), "PREVIEW_CHANNEL_URL %s\n",
                     url);
            if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
                ui_trim_text(response);
                ok = strncmp(response, "OK preview", 10) == 0;
            }
        } else {
            snprintf(response, sizeof(response), "%s", "QR scan failed");
        }
    } else {
        snprintf(response, sizeof(response), "%s", "QR scan failed");
    }
    pthread_mutex_lock(&mesh_channel_scan_mutex);
    mesh_channel_scan_ok = ok;
    mesh_channel_scan_ready = 1;
    mesh_channel_scan_running = 0;
    snprintf(mesh_channel_scan_pending_url,
             sizeof(mesh_channel_scan_pending_url), "%s", ok ? url : "");
    snprintf(mesh_channel_scan_status, sizeof(mesh_channel_scan_status), "%s",
             response[0] ? response : (ok ? "Scanned channel" :
             "QR scan failed"));
    pthread_mutex_unlock(&mesh_channel_scan_mutex);
    return NULL;
}

static void mesh_channel_scan_timer_cb(lv_timer_t *timer)
{
    char status[512];
    char scanned_url[1024];
    int ready;
    int ok;

    (void)timer;
    mesh_channel_scan_preview_update();
    pthread_mutex_lock(&mesh_channel_scan_mutex);
    ready = mesh_channel_scan_ready;
    ok = mesh_channel_scan_ok;
    snprintf(status, sizeof(status), "%s", mesh_channel_scan_status);
    snprintf(scanned_url, sizeof(scanned_url), "%s",
             mesh_channel_scan_pending_url);
    if(ready) {
        mesh_channel_scan_ready = 0;
    }
    pthread_mutex_unlock(&mesh_channel_scan_mutex);

    if(!ready) {
        return;
    }
    if(ok) {
        mesh_channel_scan_overlay_close();
        mesh_channel_import_confirm_show(scanned_url, status);
        mesh_channel_set_status(ui_tr("Scanned channel"), 0x25C281);
        mesh_append_log("channel QR preview: %s", status);
    } else {
        mesh_channel_set_status(status[0] ? status : ui_tr("QR scan failed"),
                                0xEF4D5A);
        if(mesh_channel_scan_preview_status_label &&
           lv_obj_is_valid(mesh_channel_scan_preview_status_label)) {
            lv_label_set_text(mesh_channel_scan_preview_status_label,
                              status[0] ? status : ui_tr("QR scan failed"));
            lv_obj_set_style_text_color(mesh_channel_scan_preview_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
        mesh_append_log("channel QR import failed: %s", status);
    }
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        lv_obj_invalidate(mesh_channel_overlay);
    }
    if(mesh_channel_scan_timer) {
        lv_timer_delete(mesh_channel_scan_timer);
        mesh_channel_scan_timer = NULL;
    }
}

static void mesh_channel_scan_event_cb(lv_event_t *event)
{
    int running;

    (void)event;
    pthread_mutex_lock(&mesh_channel_scan_mutex);
    running = mesh_channel_scan_running;
    if(!running) {
        mesh_channel_scan_running = 1;
        mesh_channel_scan_ready = 0;
        mesh_channel_scan_ok = 0;
        mesh_channel_scan_status[0] = '\0';
        mesh_channel_scan_pending_url[0] = '\0';
    }
    pthread_mutex_unlock(&mesh_channel_scan_mutex);

    if(running) {
        if(!mesh_channel_scan_overlay ||
           !lv_obj_is_valid(mesh_channel_scan_overlay)) {
            mesh_channel_scan_overlay_show();
        }
        mesh_channel_set_status(ui_tr("Scanning channel QR..."), 0xF5A524);
        return;
    }
    unlink(MESHTASTIC_QR_PREVIEW_FILE);
    unlink(MESHTASTIC_QR_PREVIEW_TMP);
    mesh_channel_scan_overlay_show();
    mesh_channel_set_status(ui_tr("Scanning channel QR..."), 0xF5A524);
    if(pthread_create(&mesh_channel_scan_thread, NULL,
                      mesh_channel_scan_worker, NULL) != 0) {
        pthread_mutex_lock(&mesh_channel_scan_mutex);
        mesh_channel_scan_running = 0;
        pthread_mutex_unlock(&mesh_channel_scan_mutex);
        mesh_channel_set_status("QR scan failed", 0xEF4D5A);
        return;
    }
    pthread_detach(mesh_channel_scan_thread);
    if(!mesh_channel_scan_timer) {
        mesh_channel_scan_timer = lv_timer_create(mesh_channel_scan_timer_cb,
                                                  200, NULL);
    }
}

static void mesh_channel_save_event_cb(lv_event_t *event)
{
    FILE *fp;

    (void)event;
    if(!mesh_channel_url_text[0]) {
        if(mesh_channel_status_label &&
           lv_obj_is_valid(mesh_channel_status_label)) {
            lv_label_set_text(mesh_channel_status_label,
                              ui_tr("No channel URL to save"));
            lv_obj_set_style_text_color(mesh_channel_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        return;
    }
    if(mkdir(MESHTASTIC_CHANNEL_DIR, 0755) != 0 && errno != EEXIST) {
        if(mesh_channel_status_label &&
           lv_obj_is_valid(mesh_channel_status_label)) {
            lv_label_set_text(mesh_channel_status_label,
                              ui_tr("Save failed"));
            lv_obj_set_style_text_color(mesh_channel_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
        mesh_append_log("channel URL mkdir failed: %s", strerror(errno));
        return;
    }
    fp = fopen(MESHTASTIC_CHANNEL_URL_FILE, "w");
    if(!fp) {
        if(mesh_channel_status_label &&
           lv_obj_is_valid(mesh_channel_status_label)) {
            lv_label_set_text(mesh_channel_status_label,
                              ui_tr("Save failed"));
            lv_obj_set_style_text_color(mesh_channel_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
        mesh_append_log("channel URL save failed: %s", strerror(errno));
        return;
    }
    fprintf(fp, "%s\n", mesh_channel_url_text);
    fclose(fp);
    if(mesh_channel_status_label && lv_obj_is_valid(mesh_channel_status_label)) {
        lv_label_set_text(mesh_channel_status_label,
                          ui_tr("Saved to /root/meshtastic/channel_url.txt"));
        lv_obj_set_style_text_color(mesh_channel_status_label,
                                    lv_color_hex(0x25C281), 0);
    }
    mesh_append_log("channel URL saved: %s", MESHTASTIC_CHANNEL_URL_FILE);
}

static void mesh_channel_refresh_event_cb(lv_event_t *event)
{
    char status[160];
    int ok;
    int qr_px;

    (void)event;
    mesh_channel_url_text[0] = '\0';
    ok = mesh_channel_url_fetch(mesh_channel_url_text,
                                sizeof(mesh_channel_url_text),
                                status, sizeof(status)) == 0;
    qr_px = (mesh_channel_qr_canvas &&
             lv_obj_is_valid(mesh_channel_qr_canvas)) ?
            lv_obj_get_width(mesh_channel_qr_canvas) : 0;
    if(qr_px <= 0) {
        qr_px = ui_is_landscape() ? 220 : 240;
    }
    mesh_channel_refresh_view(ok, status, qr_px);
}

static void mesh_channel_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *btn;
    char status[160];
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int card_y = landscape ? 76 : 104;
    int card_h;
    int card_max_h = screen_h - card_y - (landscape ? 96 : 150);
    int qr_px;
    int qr_x;
    int qr_y;
    int button_w;
    int y;

    (void)event;
    ui_input_hide_inline_active();
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        mesh_close_channel_page();
    }

    mesh_channel_url_text[0] = '\0';
    (void)mesh_channel_url_fetch(mesh_channel_url_text,
                                 sizeof(mesh_channel_url_text),
                                 status, sizeof(status));

    if(card_max_h < 220) {
        card_max_h = 220;
    }
    qr_px = content_w - 32;
    if(qr_px > card_max_h - 32) {
        qr_px = card_max_h - 32;
    }
    if(qr_px > MESHTASTIC_CHANNEL_QR_MAX) {
        qr_px = MESHTASTIC_CHANNEL_QR_MAX;
    }
    if(qr_px < 180) {
        qr_px = 180;
    }
    card_h = qr_px + 32;
    if(landscape && card_h < card_max_h) {
        card_h = card_max_h;
    }

    mesh_channel_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_channel_overlay);
    lv_obj_set_style_bg_color(mesh_channel_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_channel_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_channel_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_channel_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_channel_overlay);

    panel = ui_scroll_panel(mesh_channel_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Channel share"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 236);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    subtitle = ui_label(panel, ui_tr("Meshtastic official channel URL"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 236);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 210, 18, 104,
                            ui_tr("Full QR"),
                            0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_channel_qr_fullscreen_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"),
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_channel_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    card = ui_panel(panel, margin, card_y, content_w, card_h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x243044), 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    mesh_channel_qr_canvas = lv_canvas_create(card);
    qr_x = (content_w - qr_px) / 2;
    qr_y = (card_h - qr_px) / 2;
    if(qr_x < 16) {
        qr_x = 16;
    }
    if(qr_y < 16) {
        qr_y = 16;
    }
    lv_obj_set_pos(mesh_channel_qr_canvas, qr_x, qr_y);
    mesh_channel_qr_render(mesh_channel_url_text, qr_px);

    label = ui_label(card, ui_tr("Show this QR to Meshtastic app"),
                     &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

    mesh_channel_url_label =
        ui_label(card,
                 mesh_channel_url_text[0] ? mesh_channel_url_text :
                 ui_tr("Channel URL unavailable"),
                 &lv_font_montserrat_16,
                 mesh_channel_url_text[0] ? 0xD7DEE8 : 0xF5A524);
    lv_obj_add_flag(mesh_channel_url_label, LV_OBJ_FLAG_HIDDEN);

    y = card_y + card_h + 16;
    mesh_channel_status_label = ui_label(panel, status[0] ? status : "Ready",
                                         &lv_font_montserrat_16,
                                         mesh_channel_url_text[0] ?
                                         0x25C281 : 0xF5A524);
    lv_obj_set_pos(mesh_channel_status_label, margin, y);
    lv_obj_set_width(mesh_channel_status_label, content_w);
    lv_label_set_long_mode(mesh_channel_status_label, LV_LABEL_LONG_DOT);

    y += 42;
    {
        int cols = landscape ? 6 : 2;
        int gap = 10;
        int button_h = 46;
        int button_x;
        int button_y;

        button_w = (content_w - gap * (cols - 1)) / cols;
        if(button_w < 104) {
            button_w = 104;
        }

        button_x = margin;
        button_y = y;
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Save profile"), 0x25C281);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_save_profile_event_cb,
                            LV_EVENT_CLICKED, NULL);

        button_x = margin + (button_w + gap) * (1 % cols);
        button_y = y + (1 / cols) * (button_h + gap);
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Channel profiles"), 0xF59E0B);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_profiles_event_cb,
                            LV_EVENT_CLICKED, NULL);

        button_x = margin + (button_w + gap) * (2 % cols);
        button_y = y + (2 / cols) * (button_h + gap);
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Import URL"), 0x8B5CF6);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_import_url_event_cb,
                            LV_EVENT_CLICKED, NULL);

        button_x = margin + (button_w + gap) * (3 % cols);
        button_y = y + (3 / cols) * (button_h + gap);
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Save URL"), 0x3DA5FF);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_save_event_cb, LV_EVENT_CLICKED,
                            NULL);

        button_x = margin + (button_w + gap) * (4 % cols);
        button_y = y + (4 / cols) * (button_h + gap);
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Refresh"), 0x64748B);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_refresh_event_cb,
                            LV_EVENT_CLICKED, NULL);

        button_x = margin + (button_w + gap) * (5 % cols);
        button_y = y + (5 / cols) * (button_h + gap);
        btn = ui_command_button(panel, button_x, button_y, button_w,
                                ui_tr("Scan QR"), 0xEC4899);
        lv_obj_set_height(btn, button_h);
        lv_obj_add_event_cb(btn, mesh_channel_scan_event_cb, LV_EVENT_CLICKED,
                            NULL);
    }
}

static void mesh_settings_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_close_settings_page();
}

static void mesh_nodes_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_close_nodes_page();
}

static void mesh_channels_close(void)
{
    if(mesh_channels_overlay && lv_obj_is_valid(mesh_channels_overlay)) {
        lv_obj_delete(mesh_channels_overlay);
    }
    mesh_channels_overlay = NULL;
}

static void mesh_channels_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_channels_close();
}

static void mesh_channel_edit_close(void)
{
    if(mesh_channel_edit_overlay && lv_obj_is_valid(mesh_channel_edit_overlay)) {
        lv_obj_delete(mesh_channel_edit_overlay);
    }
    mesh_channel_edit_overlay = NULL;
    mesh_channel_edit_title_label = NULL;
    mesh_channel_edit_role_label = NULL;
    mesh_channel_edit_name_label = NULL;
    mesh_channel_edit_psk_label = NULL;
    mesh_channel_edit_status_label = NULL;
}

static void mesh_channel_edit_close_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_channel_edit_close();
}

static const char *mesh_channel_role_label(const char *role)
{
    if(role && strcmp(role, "primary") == 0) {
        return ui_tr("Primary");
    }
    if(role && strcmp(role, "secondary") == 0) {
        return ui_tr("Secondary");
    }
    return ui_tr("Disabled");
}

static void mesh_channel_edit_refresh(void)
{
    char text[160];
    const char *role = mesh_channel_edit_role[0] ?
                       mesh_channel_edit_role : "secondary";
    const char *name = mesh_channel_edit_name[0] ?
                       mesh_channel_edit_name : "default";
    const char *psk = mesh_channel_edit_psk[0] ?
                      mesh_channel_edit_psk : "default";

    if(mesh_channel_edit_title_label &&
       lv_obj_is_valid(mesh_channel_edit_title_label)) {
        snprintf(text, sizeof(text), "%s CH%d",
                 ui_tr("Channel slot"), mesh_channel_edit_index);
        lv_label_set_text(mesh_channel_edit_title_label, text);
    }
    if(mesh_channel_edit_role_label &&
       lv_obj_is_valid(mesh_channel_edit_role_label)) {
        snprintf(text, sizeof(text), "%s: %s", ui_tr("Role"),
                 mesh_channel_role_label(role));
        lv_label_set_text(mesh_channel_edit_role_label, text);
    }
    if(mesh_channel_edit_name_label &&
       lv_obj_is_valid(mesh_channel_edit_name_label)) {
        snprintf(text, sizeof(text), "%s: %s", ui_tr("Channel name"), name);
        lv_label_set_text(mesh_channel_edit_name_label, text);
    }
    if(mesh_channel_edit_psk_label &&
       lv_obj_is_valid(mesh_channel_edit_psk_label)) {
        snprintf(text, sizeof(text), "%s: %s", ui_tr("PSK"),
                 strcmp(psk, "default") == 0 ? "default" : "custom");
        lv_label_set_text(mesh_channel_edit_psk_label, text);
    }
}

static void mesh_channel_edit_set_role_event_cb(lv_event_t *event)
{
    const char *role = (const char *)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    if(!role || !role[0]) {
        return;
    }
    snprintf(mesh_channel_edit_role, sizeof(mesh_channel_edit_role), "%s",
             role);
    mesh_channel_edit_refresh();
}

static void mesh_channel_edit_name_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    if(!text || !text[0] || strcmp(text, "-") == 0 ||
       strcasecmp(text, "default") == 0) {
        mesh_channel_edit_name[0] = '\0';
    } else {
        mesh_safe_arg(mesh_channel_edit_name, sizeof(mesh_channel_edit_name),
                      text);
    }
    mesh_channel_edit_refresh();
}

static void mesh_channel_edit_psk_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    if(!text || !text[0]) {
        snprintf(mesh_channel_edit_psk, sizeof(mesh_channel_edit_psk),
                 "default");
    } else {
        mesh_safe_arg(mesh_channel_edit_psk, sizeof(mesh_channel_edit_psk),
                      text);
    }
    mesh_channel_edit_refresh();
}

static void mesh_channel_edit_name_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    if(event) {
        lv_event_stop_processing(event);
    }
    memset(&config, 0, sizeof(config));
    config.title = ui_tr("Channel name");
    config.placeholder = mesh_channel_edit_name[0] ?
                         mesh_channel_edit_name : "-";
    config.max_length = 12;
    config.submit_cb = mesh_channel_edit_name_submit_cb;
    config.submit_text = ui_tr("Save");
    config.cancel_text = ui_tr("Cancel");
    ui_input_dialog_open(&config);
}

static void mesh_channel_edit_psk_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    if(event) {
        lv_event_stop_processing(event);
    }
    memset(&config, 0, sizeof(config));
    config.title = ui_tr("PSK");
    config.placeholder = mesh_channel_edit_psk[0] ?
                         mesh_channel_edit_psk : "default";
    config.password_mode = 1;
    config.max_length = 80;
    config.submit_cb = mesh_channel_edit_psk_submit_cb;
    config.submit_text = ui_tr("Save");
    config.cancel_text = ui_tr("Cancel");
    ui_input_dialog_open(&config);
}

static const char *mesh_channel_edit_role_token(void)
{
    if(strcmp(mesh_channel_edit_role, "primary") == 0) {
        return "primary";
    }
    if(strcmp(mesh_channel_edit_role, "disabled") == 0) {
        return "disabled";
    }
    return "secondary";
}

static void mesh_channel_edit_save_event_cb(lv_event_t *event)
{
    char command[256];
    char response[2048];
    const char *role = mesh_channel_edit_role_token();
    const char *name = mesh_channel_edit_name[0] ?
                       mesh_channel_edit_name : "-";
    const char *psk = mesh_channel_edit_psk[0] ?
                      mesh_channel_edit_psk : "default";

    if(event) {
        lv_event_stop_processing(event);
    }
    if(mesh_channel_edit_index < 0 ||
       mesh_channel_edit_index >= MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
        return;
    }
    snprintf(command, sizeof(command), "SET_CHANNEL_SLOT %d %s %s %s\n",
             mesh_channel_edit_index, role, name, psk);
    if(mesh_ipc_command(command, response, sizeof(response)) != 0 ||
       strncmp(response, "OK channels", 11) != 0) {
        ui_trim_text(response);
        if(mesh_channel_edit_status_label &&
           lv_obj_is_valid(mesh_channel_edit_status_label)) {
            lv_label_set_text(mesh_channel_edit_status_label,
                              response[0] ? response : ui_tr("Save failed"));
            lv_obj_set_style_text_color(mesh_channel_edit_status_label,
                                        lv_color_hex(0xFCA5A5), 0);
        }
        mesh_append_log("channel slot save failed: %s", response);
        return;
    }
    mesh_append_log("channel slot saved: CH%d %s",
                    mesh_channel_edit_index, role);
    mesh_refresh_tx_channel_from_daemon(1);
    mesh_channel_edit_close();
    mesh_channels_event_cb(NULL);
}

static void mesh_channel_edit_open(int index, const char *role,
                                   const char *name, const char *psk)
{
    lv_obj_t *dialog;
    lv_obj_t *label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int dialog_w = ui_is_landscape() ? 620 : 500;
    int dialog_h = ui_is_landscape() ? 468 : 520;
    int pad = 24;
    int button_gap = 10;
    int role_w;
    int y;

    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 32) {
        dialog_h = screen_h - 32;
    }
    if(dialog_h < 360) {
        dialog_h = 360;
    }

    mesh_channel_edit_close();
    mesh_channel_edit_index = index;
    snprintf(mesh_channel_edit_role, sizeof(mesh_channel_edit_role), "%s",
             role && strcmp(role, "disabled") != 0 ? role : "secondary");
    snprintf(mesh_channel_edit_name, sizeof(mesh_channel_edit_name), "%s",
             name && strcmp(name, "default") != 0 ? name : "");
    snprintf(mesh_channel_edit_psk, sizeof(mesh_channel_edit_psk), "%s",
             psk && psk[0] && strcmp(psk, "invalid") != 0 ? psk :
             "default");

    mesh_channel_edit_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_channel_edit_overlay);
    lv_obj_set_style_bg_color(mesh_channel_edit_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_channel_edit_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mesh_channel_edit_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channel_edit_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channel_edit_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(mesh_channel_edit_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x243044), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    mesh_channel_edit_title_label =
        ui_label(dialog, "", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(mesh_channel_edit_title_label, pad, pad);
    lv_obj_set_width(mesh_channel_edit_title_label, dialog_w - pad * 2);
    lv_label_set_long_mode(mesh_channel_edit_title_label, LV_LABEL_LONG_DOT);

    y = pad + 48;
    mesh_channel_edit_role_label =
        ui_label(dialog, "", &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(mesh_channel_edit_role_label, pad, y);
    lv_obj_set_width(mesh_channel_edit_role_label, dialog_w - pad * 2);
    lv_label_set_long_mode(mesh_channel_edit_role_label, LV_LABEL_LONG_DOT);

    y += 34;
    role_w = (dialog_w - pad * 2 - button_gap * 2) / 3;
    btn = ui_command_button(dialog, pad, y, role_w, ui_tr("Primary"),
                            0x25C281);
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, mesh_channel_edit_set_role_event_cb,
                        LV_EVENT_CLICKED, "primary");
    btn = ui_command_button(dialog, pad + role_w + button_gap, y, role_w,
                            ui_tr("Secondary"), 0x3DA5FF);
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, mesh_channel_edit_set_role_event_cb,
                        LV_EVENT_CLICKED, "secondary");
    btn = ui_command_button(dialog, pad + (role_w + button_gap) * 2, y,
                            role_w, ui_tr("Disabled"), 0x64748B);
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, mesh_channel_edit_set_role_event_cb,
                        LV_EVENT_CLICKED, "disabled");

    y += 62;
    mesh_channel_edit_name_label =
        ui_label(dialog, "", &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_pos(mesh_channel_edit_name_label, pad, y);
    lv_obj_set_width(mesh_channel_edit_name_label, dialog_w - pad * 2 - 110);
    lv_label_set_long_mode(mesh_channel_edit_name_label, LV_LABEL_LONG_DOT);
    btn = ui_command_button(dialog, dialog_w - pad - 92, y - 8, 92,
                            ui_tr("Edit"), 0x3DA5FF);
    lv_obj_set_height(btn, 42);
    lv_obj_add_event_cb(btn, mesh_channel_edit_name_event_cb,
                        LV_EVENT_CLICKED, NULL);

    y += 54;
    mesh_channel_edit_psk_label =
        ui_label(dialog, "", &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_pos(mesh_channel_edit_psk_label, pad, y);
    lv_obj_set_width(mesh_channel_edit_psk_label, dialog_w - pad * 2 - 110);
    lv_label_set_long_mode(mesh_channel_edit_psk_label, LV_LABEL_LONG_DOT);
    btn = ui_command_button(dialog, dialog_w - pad - 92, y - 8, 92,
                            ui_tr("Edit"), 0x3DA5FF);
    lv_obj_set_height(btn, 42);
    lv_obj_add_event_cb(btn, mesh_channel_edit_psk_event_cb,
                        LV_EVENT_CLICKED, NULL);

    y += 54;
    label = ui_label(dialog,
                     ui_tr("Use primary for the radio channel; secondary can receive matching channel hashes"),
                     &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(label, pad, y);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    mesh_channel_edit_status_label =
        ui_label(dialog, "", &lv_font_montserrat_14, 0xFCA5A5);
    lv_obj_set_pos(mesh_channel_edit_status_label, pad, dialog_h - pad - 106);
    lv_obj_set_width(mesh_channel_edit_status_label, dialog_w - pad * 2);
    lv_label_set_long_mode(mesh_channel_edit_status_label, LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, dialog_h - pad - 54,
                            (dialog_w - pad * 2 - button_gap) / 2,
                            ui_tr("Cancel"), 0x64748B);
    lv_obj_set_height(btn, 54);
    lv_obj_add_event_cb(btn, mesh_channel_edit_close_event_cb,
                        LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(dialog,
                            pad + (dialog_w - pad * 2 + button_gap) / 2,
                            dialog_h - pad - 54,
                            (dialog_w - pad * 2 - button_gap) / 2,
                            ui_tr("Save"), 0x25C281);
    lv_obj_set_height(btn, 54);
    lv_obj_add_event_cb(btn, mesh_channel_edit_save_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_channel_edit_refresh();
}

static void mesh_channel_slot_edit_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    char response[2048];
    char *saveptr = NULL;
    char *line;
    char role[16] = "disabled";
    char name[64] = "default";
    char psk[16] = "default";

    if(event) {
        lv_event_stop_processing(event);
    }
    if(index < 0 || index >= MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
        return;
    }
    if(mesh_ipc_command("CHANNELS\n", response, sizeof(response)) == 0 &&
       strncmp(response, "OK channels", 11) == 0) {
        line = strtok_r(response, "\n", &saveptr);
        while(line) {
            char index_text[8];
            unsigned long value;

            if(strncmp(line, "CH ", 3) == 0) {
                mesh_status_field(line, "index", index_text,
                                  sizeof(index_text), "");
                if(mesh_parse_u32_text(index_text, &value) == 0 &&
                   (int)value == index) {
                    mesh_status_field(line, "role", role, sizeof(role),
                                      "disabled");
                    mesh_status_field(line, "name", name, sizeof(name),
                                      "default");
                    mesh_status_field(line, "psk", psk, sizeof(psk),
                                      "default");
                    break;
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    mesh_channel_edit_open(index, role, name, psk);
}

static void mesh_select_tx_channel_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    if(index < 0 || index >= MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
        return;
    }
    mesh_tx_channel_index = index;
    mesh_save_profile_prefs();
    mesh_refresh_tx_channel_from_daemon(1);
    mesh_channels_close();
    mesh_append_log("tx channel selected: CH%d", mesh_tx_channel_index);
}

static void mesh_channels_event_cb(lv_event_t *event)
{
    char response[2048];
    char *saveptr = NULL;
    char *line;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int columns = landscape ? 2 : 1;
    int gap = 12;
    int card_w = columns == 2 ? (content_w - gap) / 2 : content_w;
    int card_h = landscape ? 142 : 136;
    int y = 96;
    int count = 0;

    if(event) {
        lv_event_stop_processing(event);
    }
    ui_input_hide_inline_active();
    if(mesh_channels_overlay && lv_obj_is_valid(mesh_channels_overlay)) {
        lv_obj_delete(mesh_channels_overlay);
    }
    mesh_channels_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_channels_overlay);
    lv_obj_set_style_bg_color(mesh_channels_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_channels_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_channels_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_channels_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_channels_overlay, 0, 0);
    lv_obj_clear_flag(mesh_channels_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_channels_overlay);

    panel = ui_scroll_panel(mesh_channels_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Send channel"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 110);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel,
                        ui_tr("Broadcast messages use the selected channel slot"),
                        &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 110);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_channels_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    if(mesh_ipc_command("CHANNELS\n", response, sizeof(response)) != 0 ||
       strncmp(response, "OK channels", 11) != 0) {
        lv_obj_t *label = ui_label(panel, response[0] ? response :
                                   ui_tr("Daemon offline"),
                                   &lv_font_montserrat_18, 0xF5A524);
        lv_obj_set_pos(label, margin, y);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        return;
    }

    line = strtok_r(response, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "CH ", 3) == 0) {
            char index_text[8];
            char role[16];
            char name[64];
            char hash[16];
            char psk[16];
            unsigned long index_value;

            mesh_status_field(line, "index", index_text, sizeof(index_text), "");
            mesh_status_field(line, "role", role, sizeof(role), "disabled");
            mesh_status_field(line, "name", name, sizeof(name), "default");
            mesh_status_field(line, "hash", hash, sizeof(hash), "0x00");
            mesh_status_field(line, "psk", psk, sizeof(psk), "-");
            if(mesh_parse_u32_text(index_text, &index_value) == 0 &&
               index_value < MESHTASTIC_UI_CHANNEL_SLOT_MAX) {
                int disabled = strcmp(role, "disabled") == 0;
                int selected = (int)index_value == mesh_tx_channel_index;
                int col = count % columns;
                int row = count / columns;
                int x = margin + col * (card_w + gap);
                int card_y = y + row * (card_h + gap);
                uint32_t accent = disabled ? 0x475569 :
                                  (selected ? 0x25C281 : 0x3DA5FF);
                lv_obj_t *card = ui_panel(panel, x, card_y, card_w, card_h);
                lv_obj_t *label;
                char text[160];
                char meta[160];

                lv_obj_set_style_border_width(card, 1, 0);
                lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
                lv_obj_set_style_bg_color(card,
                                          lv_color_hex(selected ? 0x123328 :
                                                       0x101820), 0);
                snprintf(text, sizeof(text), "CH%lu  %s",
                         index_value, name[0] ? name : "default");
                label = ui_label(card, text, &lv_font_montserrat_18,
                                 disabled ? 0x94A3B8 : 0xF2F5F8);
                lv_obj_set_pos(label, 14, 12);
                lv_obj_set_width(label, card_w - 28);
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
                snprintf(meta, sizeof(meta), "%s  hash %s  key %s%s",
                         mesh_channel_role_label(role), hash, psk,
                         selected ? "  *" : "");
                label = ui_label(card, meta, &lv_font_montserrat_14, accent);
                lv_obj_set_pos(label, 14, 44);
                lv_obj_set_width(label, card_w - 28);
                lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
                if(!disabled) {
                    lv_obj_t *use_btn =
                        ui_command_button(card, 14, card_h - 44,
                                          (card_w - 38) / 2,
                                          ui_tr("Use"), 0x25C281);
                    lv_obj_set_height(use_btn, 34);
                    lv_obj_add_event_cb(use_btn,
                                        mesh_select_tx_channel_event_cb,
                                        LV_EVENT_CLICKED,
                                        (void *)(intptr_t)index_value);
                }
                {
                    int edit_w = disabled ? 116 : (card_w - 38) / 2;
                    int edit_x = disabled ? card_w - 14 - edit_w :
                                 24 + (card_w - 38) / 2;
                    if(edit_w > card_w - 28) {
                        edit_w = card_w - 28;
                        edit_x = 14;
                    }
                    lv_obj_t *edit_btn =
                        ui_command_button(card, edit_x, card_h - 44, edit_w,
                                          ui_tr("Edit"), 0x3DA5FF);
                    lv_obj_set_height(edit_btn, 34);
                    lv_obj_add_event_cb(edit_btn,
                                        mesh_channel_slot_edit_event_cb,
                                        LV_EVENT_CLICKED,
                                        (void *)(intptr_t)index_value);
                }
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static int mesh_node_line_value(const char *line, const char *key,
                                char *out, size_t out_len)
{
    const char *start;
    size_t n = 0;

    if(!out || out_len == 0U) {
        return 0;
    }
    snprintf(out, out_len, "-");
    if(!line || !key || !key[0]) {
        return 0;
    }
    start = line;
    while((start = strstr(start, key)) != NULL) {
        if(start == line || isspace((unsigned char)start[-1])) {
            break;
        }
        start++;
    }
    if(!start) {
        return 0;
    }
    start += strlen(key);
    while(start[n] && !isspace((unsigned char)start[n]) &&
          n + 1U < out_len) {
        out[n] = start[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static int mesh_node_line_segment(const char *line, const char *start_key,
                                  const char *end_key, char *out,
                                  size_t out_len)
{
    const char *start;
    const char *end;
    size_t n;

    if(!out || out_len == 0U) {
        return 0;
    }
    snprintf(out, out_len, "-");
    if(!line || !start_key || !start_key[0]) {
        return 0;
    }
    start = strstr(line, start_key);
    if(!start) {
        return 0;
    }
    start += strlen(start_key);
    end = end_key && end_key[0] ? strstr(start, end_key) : NULL;
    if(!end) {
        end = line + strlen(line);
    }
    while(end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    n = (size_t)(end - start);
    if(n >= out_len) {
        n = out_len - 1U;
    }
    memcpy(out, start, n);
    out[n] = '\0';
    return n > 0;
}

static int mesh_node_text_missing(const char *text)
{
    return !text || !text[0] || strcmp(text, "-") == 0;
}

static long mesh_line_long_value(const char *line, const char *key,
                                 long fallback)
{
    char text[32];
    char *endptr;
    long value;

    if(!mesh_node_line_value(line, key, text, sizeof(text)) ||
       mesh_node_text_missing(text)) {
        return fallback;
    }
    value = strtol(text, &endptr, 10);
    if(endptr == text) {
        return fallback;
    }
    return value;
}

static double mesh_line_double_value(const char *line, const char *key,
                                     double fallback)
{
    char text[40];
    char *endptr;
    double value;

    if(!mesh_node_line_value(line, key, text, sizeof(text)) ||
       mesh_node_text_missing(text)) {
        return fallback;
    }
    value = strtod(text, &endptr);
    if(endptr == text || !isfinite(value)) {
        return fallback;
    }
    return value;
}

static int mesh_line_bool_value(const char *line, const char *key)
{
    char text[16];

    if(!mesh_node_line_value(line, key, text, sizeof(text))) {
        return 0;
    }
    return strcmp(text, "1") == 0 || strcmp(text, "yes") == 0 ||
           strcmp(text, "true") == 0;
}

static int mesh_append_text(char *out, size_t out_len, const char *fmt, ...)
{
    va_list ap;
    size_t used;
    int rc;

    if(!out || out_len == 0U || !fmt) {
        return -1;
    }
    used = strlen(out);
    if(used >= out_len) {
        return -1;
    }
    va_start(ap, fmt);
    rc = vsnprintf(out + used, out_len - used, fmt, ap);
    va_end(ap);
    if(rc < 0 || (size_t)rc >= out_len - used) {
        out[out_len - 1U] = '\0';
        return -1;
    }
    return 0;
}

static int mesh_map_extract_first_line(const char *map_text, char *out,
                                       size_t out_len)
{
    const char *end;
    size_t n;

    if(!map_text || !out || out_len == 0U) {
        return -1;
    }
    end = strchr(map_text, '\n');
    n = end ? (size_t)(end - map_text) : strlen(map_text);
    if(n >= out_len) {
        n = out_len - 1U;
    }
    memcpy(out, map_text, n);
    out[n] = '\0';
    return out[0] ? 0 : -1;
}

static int mesh_map_node_has_visible_identity(const char *line)
{
    char name[64];
    char short_name[24];
    long hw;
    long rx;
    long age_s;
    int has_name;
    int has_short;
    int key_ok;
    int has_pos;

    mesh_node_line_value(line, "name=", name, sizeof(name));
    mesh_node_line_value(line, "short=", short_name, sizeof(short_name));
    hw = mesh_line_long_value(line, "hw=", -1);
    rx = mesh_line_long_value(line, "rx=", 0);
    age_s = mesh_line_long_value(line, "age_s=", 999999);
    has_name = !mesh_node_text_missing(name);
    has_short = !mesh_node_text_missing(short_name);
    key_ok = mesh_line_bool_value(line, "key=");
    has_pos = mesh_line_bool_value(line, "has_pos=");

    if(!has_name && !has_short && !has_pos && !key_ok && hw < 0 && rx <= 1) {
        return 0;
    }
    if(age_s > 24L * 3600L && !has_pos) {
        return 0;
    }
    return 1;
}

static int mesh_map_node_to_legacy_line(const char *line, char *out,
                                        size_t out_len)
{
    char id[24];
    char name[64];
    char short_name[24];
    char key[8];
    char tel[160];
    char pos[160];
    long hw;
    long age_s;
    long rx;
    long rssi;
    double snr;
    double lat;
    double lon;
    long alt;
    long sats;
    long precision;
    long ts;
    long battery;
    double voltage;
    double ch_util;
    double air_tx;
    int has_pos;
    int has_tel = 0;

    if(!line || strncmp(line, "NODE ", 5) != 0 || !out || out_len == 0U) {
        return -1;
    }
    if(!mesh_node_line_value(line, "id=", id, sizeof(id)) ||
       mesh_node_text_missing(id)) {
        return -1;
    }
    mesh_node_line_value(line, "name=", name, sizeof(name));
    mesh_node_line_value(line, "short=", short_name, sizeof(short_name));
    if(mesh_node_text_missing(name) && !mesh_node_text_missing(short_name)) {
        snprintf(name, sizeof(name), "%s", short_name);
    }
    if(mesh_node_text_missing(name)) {
        snprintf(name, sizeof(name), "-");
    }
    if(mesh_node_text_missing(short_name)) {
        snprintf(short_name, sizeof(short_name), "-");
    }
    hw = mesh_line_long_value(line, "hw=", -1);
    rx = mesh_line_long_value(line, "rx=", 0);
    age_s = mesh_line_long_value(line, "age_s=", 999999);
    rssi = mesh_line_long_value(line, "rssi=", -999);
    snr = mesh_line_double_value(line, "snr=", 0.0);
    has_pos = mesh_line_bool_value(line, "has_pos=");
    lat = mesh_line_double_value(line, "lat=", 0.0);
    lon = mesh_line_double_value(line, "lon=", 0.0);
    alt = mesh_line_long_value(line, "alt=", 0);
    sats = mesh_line_long_value(line, "sats=", 0);
    precision = mesh_line_long_value(line, "precision=", 0);
    ts = mesh_line_long_value(line, "ts=", 0);
    battery = mesh_line_long_value(line, "battery=", -1);
    voltage = mesh_line_double_value(line, "voltage=", 0.0);
    ch_util = mesh_line_double_value(line, "ch_util=", -1.0);
    air_tx = mesh_line_double_value(line, "air_tx=", -1.0);
    snprintf(key, sizeof(key), "%s",
             mesh_line_bool_value(line, "key=") ? "yes" : "no");

    tel[0] = '\0';
    if(battery >= 0) {
        mesh_append_text(tel, sizeof(tel), "bat=%ld", battery);
        has_tel = 1;
    }
    if(voltage > 0.0) {
        mesh_append_text(tel, sizeof(tel), "%sv=%.2f",
                         has_tel ? " " : "", voltage);
        has_tel = 1;
    }
    if(ch_util >= 0.0) {
        mesh_append_text(tel, sizeof(tel), "%sch=%.1f",
                         has_tel ? " " : "", ch_util);
        has_tel = 1;
    }
    if(air_tx >= 0.0) {
        mesh_append_text(tel, sizeof(tel), "%sair=%.2f",
                         has_tel ? " " : "", air_tx);
        has_tel = 1;
    }
    if(!has_tel) {
        snprintf(tel, sizeof(tel), "-");
    }

    if(has_pos && (fabs(lat) >= 0.000001 || fabs(lon) >= 0.000001)) {
        snprintf(pos, sizeof(pos),
                 "%.7f,%.7f alt=%ldm speed=- track=- sats=%ld "
                 "precision=%ld time=%ld",
                 lat, lon, alt, sats, precision, ts);
    } else {
        snprintf(pos, sizeof(pos), "-");
    }

    snprintf(out, out_len,
             "%s name=%s short=%s hw=%ld key=%s rx=%ld age=%lds "
             "rssi=%lddBm snr=%.1f pos=%s tel=%s trace=- nbr=-",
             id, name, short_name, hw, key, rx, age_s, rssi, snr, pos, tel);
    return 0;
}

static int mesh_map_waypoint_to_legacy_line(const char *line, char *out,
                                            size_t out_len)
{
    char id[24];
    char from[24];
    char age[24];
    char lat[32];
    char lon[32];
    char expire[24];
    char locked[24];
    char icon[24];
    char name[48];
    char desc[96];
    double lat_value;
    double lon_value;

    if(!line || strncmp(line, "WAYPOINT ", 9) != 0 || !out ||
       out_len == 0U) {
        return -1;
    }
    if(!mesh_node_line_value(line, "lat=", lat, sizeof(lat)) ||
       !mesh_node_line_value(line, "lon=", lon, sizeof(lon))) {
        return -1;
    }
    lat_value = mesh_line_double_value(line, "lat=", 0.0);
    lon_value = mesh_line_double_value(line, "lon=", 0.0);
    if(fabs(lat_value) < 0.000001 && fabs(lon_value) < 0.000001) {
        return -1;
    }
    mesh_node_line_value(line, "id=", id, sizeof(id));
    mesh_node_line_value(line, "from=", from, sizeof(from));
    mesh_node_line_value(line, "age_s=", age, sizeof(age));
    mesh_node_line_value(line, "expire=", expire, sizeof(expire));
    mesh_node_line_value(line, "locked=", locked, sizeof(locked));
    mesh_node_line_value(line, "icon=", icon, sizeof(icon));
    mesh_node_line_value(line, "name=", name, sizeof(name));
    mesh_node_line_value(line, "desc=", desc, sizeof(desc));
    if(mesh_node_text_missing(id)) {
        snprintf(id, sizeof(id), "0x00000000");
    }
    if(mesh_node_text_missing(from)) {
        snprintf(from, sizeof(from), "0x00000000");
    }
    if(mesh_node_text_missing(age)) {
        snprintf(age, sizeof(age), "0");
    }
    if(mesh_node_text_missing(expire)) {
        snprintf(expire, sizeof(expire), "0");
    }
    if(mesh_node_text_missing(locked)) {
        snprintf(locked, sizeof(locked), "0x00000000");
    }
    if(mesh_node_text_missing(icon)) {
        snprintf(icon, sizeof(icon), "0x00000000");
    }
    if(mesh_node_text_missing(name)) {
        snprintf(name, sizeof(name), "%s", ui_tr("Waypoint"));
    }
    if(mesh_node_text_missing(desc)) {
        snprintf(desc, sizeof(desc), "-");
    }
    snprintf(out, out_len,
             "wp id=%s from=%s age=%ss lat=%s lon=%s expire=%s "
             "locked=%s icon=%s name=%s desc=%s",
             id, from, age, lat, lon, expire, locked, icon, name, desc);
    return 0;
}

static int mesh_map_build_legacy_nodes(const char *map_text, char *out,
                                       size_t out_len)
{
    char copy[MESHTASTIC_MAP_RESPONSE_MAX];
    char legacy[MESHTASTIC_UI_NODE_LINE_MAX];
    char *saveptr = NULL;
    char *line;
    int count = 0;

    if(!map_text || strncmp(map_text, "OK map", 6) != 0 || !out ||
       out_len == 0U) {
        return -1;
    }
    out[0] = '\0';
    snprintf(copy, sizeof(copy), "%s", map_text);
    line = strtok_r(copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "NODE ", 5) == 0 &&
           mesh_map_node_has_visible_identity(line) &&
           mesh_map_node_to_legacy_line(line, legacy, sizeof(legacy)) == 0) {
            if(mesh_append_text(out, out_len, "%s\n", legacy) != 0) {
                return count > 0 ? count : -1;
            }
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static int mesh_map_build_legacy_waypoints(const char *map_text, char *out,
                                           size_t out_len)
{
    char copy[MESHTASTIC_MAP_RESPONSE_MAX];
    char legacy[512];
    char *saveptr = NULL;
    char *line;
    int count = 0;

    if(!map_text || strncmp(map_text, "OK map", 6) != 0 || !out ||
       out_len == 0U) {
        return -1;
    }
    out[0] = '\0';
    snprintf(copy, sizeof(copy), "%s", map_text);
    line = strtok_r(copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "WAYPOINT ", 9) == 0 &&
           mesh_map_waypoint_to_legacy_line(line, legacy,
                                            sizeof(legacy)) == 0) {
            if(mesh_append_text(out, out_len, "%s\n", legacy) != 0) {
                return count > 0 ? count : -1;
            }
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static int mesh_fetch_legacy_nodes(char *nodes_text, size_t nodes_len,
                                   char *error_text, size_t error_len,
                                   char *map_status, size_t map_status_len)
{
    char map_response[MESHTASTIC_MAP_RESPONSE_MAX];
    char response[8192];

    if(nodes_text && nodes_len > 0U) {
        nodes_text[0] = '\0';
    }
    if(error_text && error_len > 0U) {
        error_text[0] = '\0';
    }
    if(map_status && map_status_len > 0U) {
        map_status[0] = '\0';
    }
    if(!nodes_text || nodes_len == 0U) {
        return -1;
    }
    if(mesh_ipc_command("MAP\n", map_response, sizeof(map_response)) == 0 &&
       strncmp(map_response, "OK map", 6) == 0 &&
       mesh_map_build_legacy_nodes(map_response, nodes_text, nodes_len) >= 0) {
        if(map_status && map_status_len > 0U) {
            (void)mesh_map_extract_first_line(map_response, map_status,
                                             map_status_len);
        }
        return 0;
    }
    if(mesh_ipc_command("NODES\n", response, sizeof(response)) != 0) {
        if(error_text && error_len > 0U) {
            snprintf(error_text, error_len, "%s", response);
        }
        return -1;
    }
    snprintf(nodes_text, nodes_len, "%s", response);
    if(strncmp(nodes_text, "OK nodes\n", 9) == 0) {
        memmove(nodes_text, nodes_text + 9, strlen(nodes_text + 9) + 1U);
    }
    return 0;
}

static int mesh_find_legacy_node_line(const char *target_id, char *out,
                                      size_t out_len)
{
    char response[512];
    char nodes_text[8192];
    char *saveptr = NULL;
    char *line;

    if(!target_id || !target_id[0] || !out || out_len == 0U) {
        return -1;
    }
    out[0] = '\0';
    if(mesh_fetch_legacy_nodes(nodes_text, sizeof(nodes_text), response,
                               sizeof(response), NULL, 0) != 0) {
        ui_trim_text(response);
        mesh_append_log("node refresh failed: %s", response);
        return -1;
    }
    line = strtok_r(nodes_text, "\n", &saveptr);
    while(line) {
        char node_id[24];

        if(strncmp(line, "0x", 2) == 0 &&
           sscanf(line, "%23s", node_id) == 1 &&
           strcmp(node_id, target_id) == 0) {
            snprintf(out, out_len, "%s", line);
            return 0;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return -1;
}

static void mesh_node_detail_set_status(const char *text, uint32_t color)
{
    snprintf(mesh_node_detail_status_text,
             sizeof(mesh_node_detail_status_text), "%s", text ? text : "");
    mesh_node_detail_status_color = color;
    if(mesh_node_request_status_label &&
       lv_obj_is_valid(mesh_node_request_status_label)) {
        lv_label_set_text(mesh_node_request_status_label,
                          mesh_node_detail_status_text[0] ?
                          ui_tr(mesh_node_detail_status_text) :
                          ui_tr("Tap a request button to update this node"));
        lv_obj_set_style_text_color(mesh_node_request_status_label,
                                    lv_color_hex(mesh_node_detail_status_color),
                                    0);
    }
}

static const char *mesh_node_request_state_label(const char *state,
                                                 uint32_t *color)
{
    if(color) {
        *color = 0x94A3B8;
    }
    if(!state || !state[0]) {
        return "Unknown";
    }
    if(strcmp(state, "queued") == 0) {
        if(color) {
            *color = 0x25C281;
        }
        return "Queued";
    }
    if(strcmp(state, "held") == 0) {
        if(color) {
            *color = 0xF59E0B;
        }
        return "Radio busy";
    }
    if(strcmp(state, "sending") == 0) {
        if(color) {
            *color = 0x3DA5FF;
        }
        return "Sending";
    }
    if(strcmp(state, "sent") == 0) {
        if(color) {
            *color = 0x25C281;
        }
        return "Sent";
    }
    if(strcmp(state, "replied") == 0) {
        if(color) {
            *color = 0x25C281;
        }
        return "Replied";
    }
    if(strcmp(state, "tx-failed") == 0 ||
       strcmp(state, "build-failed") == 0) {
        if(color) {
            *color = 0xEF4D5A;
        }
        return "Failed";
    }
    return state;
}

static int mesh_node_detail_fetch_request_status(const char *target_id,
                                                 char *out, size_t out_len,
                                                 uint32_t *color)
{
    char response[4096];
    char copy[4096];
    char target_key[40];
    char *saveptr = NULL;
    char *line;
    char last_line[256] = { 0 };
    char type[40];
    char state[40];
    char age_ms[24];
    char latency_ms[24];

    if(!target_id || !target_id[0] || !out || out_len == 0U) {
        return -1;
    }
    out[0] = '\0';
    if(color) {
        *color = 0x94A3B8;
    }
    if(mesh_ipc_command("REQUEST_STATUS\n", response, sizeof(response)) != 0 ||
       strncmp(response, "OK request_status", 17) != 0) {
        return -1;
    }
    snprintf(copy, sizeof(copy), "%s", response);
    snprintf(target_key, sizeof(target_key), "target=%s", target_id);
    line = strtok_r(copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "REQ ", 4) == 0 && strstr(line, target_key)) {
            snprintf(last_line, sizeof(last_line), "%s", line);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    if(!last_line[0]) {
        return -1;
    }
    mesh_node_line_value(last_line, "type=", type, sizeof(type));
    mesh_node_line_value(last_line, "state=", state, sizeof(state));
    mesh_node_line_value(last_line, "age_ms=", age_ms, sizeof(age_ms));
    mesh_node_line_value(last_line, "latency_ms=", latency_ms,
                         sizeof(latency_ms));
    if(!type[0]) {
        snprintf(type, sizeof(type), "%s", "request");
    }
    if(!age_ms[0]) {
        snprintf(age_ms, sizeof(age_ms), "%s", "-");
    }
    if(strcmp(state, "replied") == 0 && latency_ms[0] &&
       strcmp(latency_ms, "0") != 0) {
        snprintf(out, out_len, "%s %s: %s, %sms",
                 ui_tr("Request"), type,
                 ui_tr(mesh_node_request_state_label(state, color)),
                 latency_ms);
    } else {
        snprintf(out, out_len, "%s %s: %s, %sms",
                 ui_tr("Request"), type,
                 ui_tr(mesh_node_request_state_label(state, color)), age_ms);
    }
    return 0;
}

static void mesh_node_detail_refresh_current(void)
{
    char target_id[24];
    char line[MESHTASTIC_UI_NODE_LINE_MAX];
    char request_status[160];
    uint32_t request_color = 0x94A3B8;

    if(!mesh_node_detail_target_id[0]) {
        return;
    }
    snprintf(target_id, sizeof(target_id), "%s", mesh_node_detail_target_id);
    if(mesh_find_legacy_node_line(target_id, line, sizeof(line)) == 0) {
        if(mesh_node_detail_fetch_request_status(target_id, request_status,
                                                 sizeof(request_status),
                                                 &request_color) == 0) {
            mesh_node_detail_set_status(request_status, request_color);
        } else {
            mesh_node_detail_set_status(
                "Refreshed. Reply may still be in flight.", 0x25C281);
        }
        mesh_node_detail_open(line);
        return;
    }
    mesh_node_detail_set_status("Waiting for node reply...", 0xF59E0B);
}

static void mesh_node_append_line(char *out, size_t out_len,
                                  const char *name, const char *value)
{
    size_t used;

    if(!out || out_len == 0U || !name || mesh_node_text_missing(value)) {
        return;
    }
    used = strlen(out);
    if(used + 1U >= out_len) {
        return;
    }
    snprintf(out + used, out_len - used, "%s%s: %s",
             used > 0U ? "\n" : "", ui_tr(name), value);
}

static void mesh_node_append_value_from_key(char *out, size_t out_len,
                                            const char *text,
                                            const char *key,
                                            const char *name)
{
    char value[64];

    if(mesh_node_line_value(text, key, value, sizeof(value))) {
        mesh_node_append_line(out, out_len, name, value);
    }
}

static void mesh_node_format_position(const char *raw, char *out,
                                      size_t out_len)
{
    const char *space;
    size_t n;
    char coords[96];

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(mesh_node_text_missing(raw)) {
        snprintf(out, out_len, "No position data");
        return;
    }

    space = strchr(raw, ' ');
    n = space ? (size_t)(space - raw) : strlen(raw);
    if(n >= sizeof(coords)) {
        n = sizeof(coords) - 1U;
    }
    memcpy(coords, raw, n);
    coords[n] = '\0';
    mesh_node_append_line(out, out_len, "Coordinates", coords);
    mesh_node_append_value_from_key(out, out_len, raw, "alt=", "Altitude");
    mesh_node_append_value_from_key(out, out_len, raw, "speed=", "Speed");
    mesh_node_append_value_from_key(out, out_len, raw, "track=", "Track");
    mesh_node_append_value_from_key(out, out_len, raw, "sats=", "Satellites");
    mesh_node_append_value_from_key(out, out_len, raw, "precision=",
                                    "Precision");
    mesh_node_append_value_from_key(out, out_len, raw, "time=", "Updated");
    if(out[0] == '\0') {
        snprintf(out, out_len, "%s", raw);
    }
}

static void mesh_node_format_telemetry(const char *raw, char *out,
                                       size_t out_len)
{
    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(mesh_node_text_missing(raw)) {
        snprintf(out, out_len, "No telemetry data");
        return;
    }

    mesh_node_append_value_from_key(out, out_len, raw, "bat=", "Battery");
    mesh_node_append_value_from_key(out, out_len, raw, "v=", "Voltage");
    mesh_node_append_value_from_key(out, out_len, raw, "ch=",
                                    "Channel util");
    mesh_node_append_value_from_key(out, out_len, raw, "air=", "Air util");
    mesh_node_append_value_from_key(out, out_len, raw, "temp=",
                                    "Temperature");
    mesh_node_append_value_from_key(out, out_len, raw, "hum=", "Humidity");
    mesh_node_append_value_from_key(out, out_len, raw, "press=", "Pressure");
    mesh_node_append_value_from_key(out, out_len, raw, "env_v=",
                                    "Sensor voltage");
    mesh_node_append_value_from_key(out, out_len, raw, "iaq=", "IAQ");
    mesh_node_append_value_from_key(out, out_len, raw, "up=", "Uptime");
    mesh_node_append_value_from_key(out, out_len, raw, "time=", "Updated");
    if(out[0] == '\0') {
        snprintf(out, out_len, "%s", raw);
    }
}

static void mesh_node_format_optional(const char *raw, const char *empty_text,
                                      char *out, size_t out_len)
{
    if(!out || out_len == 0U) {
        return;
    }
    if(mesh_node_text_missing(raw)) {
        snprintf(out, out_len, "%s", empty_text ? empty_text : "-");
    } else {
        snprintf(out, out_len, "%s", raw);
    }
}

static int mesh_node_text_line_count(const char *text, int width)
{
    int lines = 1;
    int chars_per_line = (width - 36) / 9;
    int current = 0;

    if(!text || !text[0]) {
        return 1;
    }
    if(chars_per_line < 18) {
        chars_per_line = 18;
    }
    for(const char *p = text; *p; p++) {
        if(*p == '\n') {
            lines++;
            current = 0;
            continue;
        }
        current++;
        if(current >= chars_per_line) {
            lines++;
            current = 0;
        }
    }
    return lines;
}

static void mesh_node_detail_move_after_columns(int *y_left, int *y_right)
{
    int y;

    if(!y_left || !y_right) {
        return;
    }
    y = *y_left > *y_right ? *y_left : *y_right;
    *y_left = y;
    *y_right = y;
}

static void mesh_node_detail_append_spacer(lv_obj_t *parent, int y)
{
    lv_obj_t *spacer;

    if(!parent) {
        return;
    }
    spacer = lv_obj_create(parent);
    lv_obj_set_pos(spacer, 0, y);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
}

static int mesh_node_detail_card(lv_obj_t *parent, int x, int y, int w,
                                 const char *title, const char *body,
                                 uint32_t accent)
{
    lv_obj_t *card;
    lv_obj_t *label;
    int lines = mesh_node_text_line_count(body, w);
    int body_h = lines * 25;
    int h = 72 + body_h;

    if(h < 124) {
        h = 124;
    }
    card = ui_panel(parent, x, y, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101822), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263244), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    label = ui_label(card, ui_tr(title), &lv_font_montserrat_18, accent);
    lv_obj_set_pos(label, 14, 12);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, body && body[0] ? body : "-", &lv_font_montserrat_16,
                     0xD7DEE8);
    lv_obj_set_pos(label, 14, 44);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return h;
}

static int mesh_node_detail_add_card(lv_obj_t *parent, int x, int *y,
                                     int w, const char *title,
                                     const char *body, uint32_t accent)
{
    int h;

    if(!y) {
        return 0;
    }
    h = mesh_node_detail_card(parent, x, *y, w, title, body, accent);
    *y += h + 14;
    return h;
}

static void mesh_select_node_target_event_cb(lv_event_t *event)
{
    const char *node_id = (const char *)lv_event_get_user_data(event);

    if(!node_id || !node_id[0]) {
        return;
    }
    if(mesh_normalize_to_node_text(node_id, mesh_to_node,
                                   sizeof(mesh_to_node)) != 0) {
        mesh_append_log("invalid target node: %s", node_id);
        return;
    }
    mesh_save_profile_prefs();
    mesh_update_profile_label();
    mesh_update_target_button();
    mesh_close_nodes_page();
    mesh_append_log("target selected: %s",
                    mesh_to_text_is_broadcast(mesh_to_node) ?
                    "broadcast" : mesh_to_node);
    mesh_restart_daemon_if_online();
}

static void mesh_node_detail_back_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_nodes_event_cb(NULL);
}

static void mesh_node_remote_request_event_cb(lv_event_t *event)
{
    const char *command_prefix =
        (const char *)lv_event_get_user_data(event);
    char command[96];
    char response[256];

    if(!command_prefix || !command_prefix[0] ||
       !mesh_node_detail_target_id[0]) {
        return;
    }
    snprintf(command, sizeof(command), "%s %s\n", command_prefix,
             mesh_node_detail_target_id);
    if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
        ui_trim_text(response);
        if(strncmp(response, "OK", 2) == 0) {
            mesh_append_log("request %s: %s",
                            mesh_node_detail_target_id, response);
            mesh_node_detail_set_status("Request queued. Refreshing node data...",
                                        0x25C281);
            mesh_node_detail_refresh_ticks =
                MESHTASTIC_NODE_DETAIL_REFRESH_TICKS;
            mesh_node_detail_refresh_attempts =
                MESHTASTIC_NODE_DETAIL_REFRESH_ATTEMPTS;
        } else {
            mesh_append_log("request rejected %s: %s",
                            mesh_node_detail_target_id, response);
            mesh_node_detail_set_status(response[0] ? response :
                                        "Request failed", 0xEF4D5A);
        }
    } else {
        ui_trim_text(response);
        mesh_append_log("request failed %s: %s",
                        mesh_node_detail_target_id, response);
        mesh_node_detail_set_status(response[0] ? response : "Request failed",
                                    0xEF4D5A);
    }
    mesh_refresh_status();
}

static void mesh_map_close(void)
{
    if(mesh_map_timer) {
        lv_timer_delete(mesh_map_timer);
        mesh_map_timer = NULL;
    }
    if(mesh_map_overlay && lv_obj_is_valid(mesh_map_overlay)) {
        lv_obj_delete(mesh_map_overlay);
    }
    mesh_map_overlay = NULL;
    mesh_map_has_position = 0;
}

static void mesh_map_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_map_close();
}

static void mesh_map_load_prefs(void)
{
    char value[16];
    char fallback[8];
    int zoom;

    ui_prefs_get(MESHTASTIC_PREF_MAP_FAKE_GPS, value, sizeof(value), "1");
    mesh_map_fake_gps_enabled = atoi(value) != 0;
    snprintf(fallback, sizeof(fallback), "%d", MESHTASTIC_MAP_DEFAULT_ZOOM);
    ui_prefs_get(MESHTASTIC_PREF_MAP_ZOOM, value, sizeof(value), fallback);
    zoom = atoi(value);
    if(zoom < MESHTASTIC_MAP_MIN_ZOOM) {
        zoom = MESHTASTIC_MAP_MIN_ZOOM;
    } else if(zoom > MESHTASTIC_MAP_MAX_ZOOM) {
        zoom = MESHTASTIC_MAP_MAX_ZOOM;
    }
    mesh_map_zoom = zoom;
}

static void mesh_map_save_prefs(void)
{
    char value[16];

    snprintf(value, sizeof(value), "%d", mesh_map_fake_gps_enabled ? 1 : 0);
    ui_prefs_set(MESHTASTIC_PREF_MAP_FAKE_GPS, value);
    snprintf(value, sizeof(value), "%d", mesh_map_zoom);
    ui_prefs_set(MESHTASTIC_PREF_MAP_ZOOM, value);
}

static double mesh_map_clip_latitude(double lat)
{
    if(lat > 85.05112878) {
        return 85.05112878;
    }
    if(lat < -85.05112878) {
        return -85.05112878;
    }
    return lat;
}

static void mesh_map_lonlat_to_pixel(double lat, double lon, int zoom,
                                     double *px, double *py)
{
    double n = (double)(1U << (unsigned)zoom);
    double lat_rad = mesh_map_clip_latitude(lat) * M_PI / 180.0;
    double x = (lon + 180.0) / 360.0 * n;
    double y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) /
               2.0 * n;

    if(px) {
        *px = x * MESHTASTIC_MAP_TILE_SIZE;
    }
    if(py) {
        *py = y * MESHTASTIC_MAP_TILE_SIZE;
    }
}

static void mesh_map_pixel_to_lonlat(double px, double py, int zoom,
                                     double *lat, double *lon)
{
    double world = (double)MESHTASTIC_MAP_TILE_SIZE *
                   (double)(1U << (unsigned)zoom);
    double x = px;
    double y = py;
    double lat_rad;

    if(world <= 0.0) {
        if(lat) {
            *lat = MESHTASTIC_MAP_FAKE_LAT;
        }
        if(lon) {
            *lon = MESHTASTIC_MAP_FAKE_LON;
        }
        return;
    }
    while(x < 0.0) {
        x += world;
    }
    while(x >= world) {
        x -= world;
    }
    if(y < 0.0) {
        y = 0.0;
    } else if(y > world) {
        y = world;
    }
    if(lon) {
        *lon = x / world * 360.0 - 180.0;
    }
    if(lat) {
        lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * y / world)));
        *lat = lat_rad * 180.0 / M_PI;
    }
}

static void mesh_map_status_stats_line(const char *status, char *out,
                                       size_t out_len)
{
    char rx[24];
    char sats[16];
    char ttff[24];
    char ttff_valid[8];
    char last_nmea[24];
    unsigned long ttff_ms;
    unsigned long last_ms;
    char ttff_text[24];
    char last_text[24];

    if(!out || out_len == 0U) {
        return;
    }
    mesh_status_field(status, "nmea_rx", rx, sizeof(rx), "0");
    mesh_status_field(status, "gnss_sats_seen", sats, sizeof(sats), "0");
    if(strcmp(sats, "0") == 0) {
        mesh_status_field(status, "sats", sats, sizeof(sats), "0");
    }
    mesh_status_field(status, "ttff_ms", ttff, sizeof(ttff), "0");
    mesh_status_field(status, "ttff_valid", ttff_valid, sizeof(ttff_valid),
                      "0");
    mesh_status_field(status, "last_nmea_ms", last_nmea, sizeof(last_nmea),
                      "0");
    ttff_ms = strtoul(ttff, NULL, 10);
    last_ms = strtoul(last_nmea, NULL, 10);
    if(strcmp(ttff_valid, "1") == 0 && ttff_ms > 0UL) {
        snprintf(ttff_text, sizeof(ttff_text), "%.1fs", ttff_ms / 1000.0);
    } else {
        snprintf(ttff_text, sizeof(ttff_text), "-");
    }
    if(last_ms > 0UL) {
        snprintf(last_text, sizeof(last_text), "%.1fs", last_ms / 1000.0);
    } else {
        snprintf(last_text, sizeof(last_text), "-");
    }
    snprintf(out, out_len, "RX %s  Sats %s  TTFT %s  Last %s",
             rx, sats, ttff_text, last_text);
}

static int mesh_map_status_position(const char *status, double *lat,
                                    double *lon, char *reason,
                                    size_t reason_len,
                                    mesh_map_position_state_t *state)
{
    char nrf9151[24];
    char gps[24];
    char phase[24];
    char lat_text[32];
    char lon_text[32];
    char stats[128];
    char *endptr;
    double parsed_lat;
    double parsed_lon;

    if(reason && reason_len > 0U) {
        snprintf(reason, reason_len, "%s", ui_tr("Waiting for position"));
    }
    if(state) {
        *state = MESH_MAP_POS_GNSS_SEARCHING;
    }
    if(!status || !mesh_status_is_online(status)) {
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len, "%s",
                     ui_tr("Meshtastic service is starting"));
        }
        if(state) {
            *state = MESH_MAP_POS_DAEMON_OFFLINE;
        }
        return 0;
    }

    mesh_status_field(status, "nrf9151", nrf9151, sizeof(nrf9151), "missing");
    mesh_status_field(status, "gnss_phase", phase, sizeof(phase), "-");
    if(strcmp(phase, "-") == 0) {
        mesh_status_field(status, "phase", phase, sizeof(phase), "-");
    }
    mesh_map_status_stats_line(status, stats, sizeof(stats));
    if(strcmp(nrf9151, "present") != 0) {
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len, "%s",
                     ui_tr("nRF9151 GNSS not detected"));
        }
        if(state) {
            *state = MESH_MAP_POS_NRF9151_MISSING;
        }
        return 0;
    }

    mesh_status_field(status, "gps", gps, sizeof(gps), "-");
    mesh_status_field(status, "lat", lat_text, sizeof(lat_text), "-");
    mesh_status_field(status, "lon", lon_text, sizeof(lon_text), "-");
    if(strcmp(gps, "fix") != 0 && strcmp(gps, "fixed") != 0 &&
       strcmp(gps, "debug") != 0) {
        if(reason && reason_len > 0U) {
            if(strcmp(gps, "error") == 0 || strcmp(gps, "failed") == 0) {
                snprintf(reason, reason_len, "%s\n%s",
                         ui_tr("GNSS needs attention"), stats);
            } else if(strcmp(phase, "first") == 0) {
                snprintf(reason, reason_len, "%s\n%s",
                         ui_tr("Waiting first GNSS fix"), stats);
            } else if(strcmp(phase, "no_sat") == 0) {
                snprintf(reason, reason_len, "%s\n%s",
                         ui_tr("GNSS running, no satellites"), stats);
            } else if(strcmp(phase, "sat_no_fix") == 0) {
                snprintf(reason, reason_len, "%s\n%s",
                         ui_tr("GNSS satellites visible, no fix"), stats);
            } else {
                snprintf(reason, reason_len, "%s\n%s",
                         ui_tr("nRF9151 GNSS locating..."), stats);
            }
        }
        if(state) {
            if(strcmp(gps, "error") == 0 || strcmp(gps, "failed") == 0) {
                *state = MESH_MAP_POS_GNSS_ERROR;
            } else if(strcmp(phase, "first") == 0) {
                *state = MESH_MAP_POS_FIRST_FIX;
            } else if(strcmp(phase, "no_sat") == 0) {
                *state = MESH_MAP_POS_NO_SATELLITES;
            } else if(strcmp(phase, "sat_no_fix") == 0) {
                *state = MESH_MAP_POS_SATELLITES_NO_FIX;
            } else {
                *state = MESH_MAP_POS_GNSS_SEARCHING;
            }
        }
        return 0;
    }

    parsed_lat = strtod(lat_text, &endptr);
    if(endptr == lat_text || !isfinite(parsed_lat)) {
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len, "%s",
                     ui_tr("Waiting for GNSS coordinates"));
        }
        if(state) {
            *state = MESH_MAP_POS_COORD_UNAVAILABLE;
        }
        return 0;
    }
    parsed_lon = strtod(lon_text, &endptr);
    if(endptr == lon_text || !isfinite(parsed_lon)) {
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len, "%s",
                     ui_tr("Waiting for GNSS coordinates"));
        }
        if(state) {
            *state = MESH_MAP_POS_COORD_UNAVAILABLE;
        }
        return 0;
    }
    if(fabs(parsed_lat) < 0.000001 && fabs(parsed_lon) < 0.000001) {
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len, "%s",
                     ui_tr("Waiting for GNSS coordinates"));
        }
        if(state) {
            *state = MESH_MAP_POS_COORD_UNAVAILABLE;
        }
        return 0;
    }

    if(lat) {
        *lat = parsed_lat;
    }
    if(lon) {
        *lon = parsed_lon;
    }
    if(reason && reason_len > 0U) {
        if(strcmp(phase, "cache") == 0) {
            snprintf(reason, reason_len, "%s\n%s",
                     ui_tr("Using last GNSS fix"), stats);
        } else {
            snprintf(reason, reason_len, "%s\n%s",
                     ui_tr("GNSS fixed"), stats);
        }
    }
    if(state) {
        *state = strcmp(phase, "cache") == 0 ? MESH_MAP_POS_USING_CACHE :
                 MESH_MAP_POS_READY;
    }
    return 1;
}

static int mesh_map_current_position(const char *status, double *lat,
                                     double *lon, char *reason,
                                     size_t reason_len,
                                     mesh_map_position_state_t *state)
{
    if(mesh_map_fake_gps_enabled) {
        if(lat) {
            *lat = MESHTASTIC_MAP_FAKE_LAT;
        }
        if(lon) {
            *lon = MESHTASTIC_MAP_FAKE_LON;
        }
        if(reason && reason_len > 0U) {
            snprintf(reason, reason_len,
                     "Debug GPS %.5f, %.5f", MESHTASTIC_MAP_FAKE_LAT,
                     MESHTASTIC_MAP_FAKE_LON);
        }
        if(state) {
            *state = MESH_MAP_POS_READY;
        }
        return 1;
    }
    return mesh_map_status_position(status, lat, lon, reason, reason_len,
                                    state);
}

static void mesh_map_tile_path(int z, int x, int y, char *out,
                               size_t out_len)
{
    snprintf(out, out_len, "%s/%s/%d/%d/%d.png", MESHTASTIC_MAP_ROOT,
             MESHTASTIC_MAP_STYLE, z, x, y);
}

static void mesh_map_add_placeholder_tile(lv_obj_t *map, int x, int y,
                                          const char *text)
{
    lv_obj_t *tile = lv_obj_create(map);
    lv_obj_t *label;

    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, MESHTASTIC_MAP_TILE_SIZE,
                    MESHTASTIC_MAP_TILE_SIZE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x233044), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_radius(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    label = ui_label(tile, text, &lv_font_montserrat_14, 0x64748B);
    lv_obj_set_width(label, MESHTASTIC_MAP_TILE_SIZE - 24);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
}

static void mesh_map_add_marker(lv_obj_t *map, int x, int y, uint32_t color,
                                const char *text)
{
    lv_obj_t *dot;
    lv_obj_t *label;

    if(x < -24 || y < -24 || x > lv_obj_get_width(map) + 24 ||
       y > lv_obj_get_height(map) + 24) {
        return;
    }

    dot = lv_obj_create(map);
    lv_obj_set_pos(dot, x - 7, y - 7);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, 7, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    if(text && text[0]) {
        label = ui_label(map, text, &lv_font_montserrat_14, 0xF8FAFC);
        lv_obj_set_style_bg_color(label, lv_color_hex(0x0B1220), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
        lv_obj_set_style_pad_left(label, 6, 0);
        lv_obj_set_style_pad_right(label, 6, 0);
        lv_obj_set_style_pad_top(label, 2, 0);
        lv_obj_set_style_pad_bottom(label, 2, 0);
        lv_obj_set_style_radius(label, 5, 0);
        lv_obj_set_pos(label, x + 10, y - 12);
        lv_obj_set_width(label, 96);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
}

static void mesh_map_add_self_marker(lv_obj_t *map, int x, int y,
                                     const char *text)
{
    lv_obj_t *halo;
    lv_obj_t *dot;
    lv_obj_t *ring;
    lv_obj_t *label;
    lv_obj_t *line_h;
    lv_obj_t *line_v;
    int map_w = lv_obj_get_width(map);
    int map_h = lv_obj_get_height(map);
    int label_w = 210;
    int label_h = 54;
    int label_x = x - label_w / 2;
    int label_y = y + 38;

    if(x < -36 || y < -36 || x > map_w + 36 || y > map_h + 36) {
        mesh_ui_trace("map self marker skipped x=%d y=%d map=%dx%d",
                      x, y, map_w, map_h);
        return;
    }
    if(label_x + label_w > map_w - 6) {
        label_x = map_w - label_w - 6;
    }
    if(label_x < 6) {
        label_x = 6;
    }
    if(label_y + label_h > map_h - 6) {
        label_y = y - label_h - 38;
    }
    if(label_y < 6) {
        label_y = 6;
    }

    line_h = lv_obj_create(map);
    lv_obj_set_pos(line_h, x - 44, y - 2);
    lv_obj_set_size(line_h, 88, 5);
    lv_obj_set_style_bg_color(line_h, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(line_h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_h, 0, 0);
    lv_obj_set_style_radius(line_h, 2, 0);
    lv_obj_clear_flag(line_h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line_h, LV_OBJ_FLAG_CLICKABLE);

    line_v = lv_obj_create(map);
    lv_obj_set_pos(line_v, x - 2, y - 44);
    lv_obj_set_size(line_v, 5, 88);
    lv_obj_set_style_bg_color(line_v, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(line_v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_v, 0, 0);
    lv_obj_set_style_radius(line_v, 2, 0);
    lv_obj_clear_flag(line_v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line_v, LV_OBJ_FLAG_CLICKABLE);

    halo = lv_obj_create(map);
    lv_obj_set_pos(halo, x - 34, y - 34);
    lv_obj_set_size(halo, 68, 68);
    lv_obj_set_style_radius(halo, 34, 0);
    lv_obj_set_style_bg_color(halo, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_60, 0);
    lv_obj_set_style_border_width(halo, 0, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);

    ring = lv_obj_create(map);
    lv_obj_set_pos(ring, x - 23, y - 23);
    lv_obj_set_size(ring, 46, 46);
    lv_obj_set_style_radius(ring, 23, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 5, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    dot = lv_obj_create(map);
    lv_obj_set_pos(dot, x - 13, y - 13);
    lv_obj_set_size(dot, 26, 26);
    lv_obj_set_style_radius(dot, 13, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 3, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0x062A1B), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    if(text && text[0]) {
        label = ui_label(map, text, &lv_font_montserrat_14, 0xF8FAFC);
        lv_obj_set_style_bg_color(label, lv_color_hex(0x052E1A), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_90, 0);
        lv_obj_set_style_pad_left(label, 10, 0);
        lv_obj_set_style_pad_right(label, 10, 0);
        lv_obj_set_style_pad_top(label, 4, 0);
        lv_obj_set_style_pad_bottom(label, 4, 0);
        lv_obj_set_style_radius(label, 7, 0);
        lv_obj_set_pos(label, label_x, label_y);
        lv_obj_set_width(label, label_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(label);
    }
    lv_obj_move_foreground(line_h);
    lv_obj_move_foreground(line_v);
    lv_obj_move_foreground(halo);
    lv_obj_move_foreground(ring);
    lv_obj_move_foreground(dot);
    mesh_ui_trace("map self marker x=%d y=%d label=%d,%d map=%dx%d",
                  x, y, label_x, label_y, map_w, map_h);
}

static void mesh_map_draw_nodes(lv_obj_t *map, const char *nodes_text,
                                double top_left_x, double top_left_y,
                                int zoom)
{
    char nodes_copy[8192];
    char *saveptr = NULL;
    char *line;

    if(!nodes_text || !nodes_text[0]) {
        return;
    }
    snprintf(nodes_copy, sizeof(nodes_copy), "%s", nodes_text);
    line = strtok_r(nodes_copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "0x", 2) == 0) {
            char pos[96];
            double lat;
            double lon;
            if(mesh_node_line_value(line, "pos=", pos, sizeof(pos)) &&
               sscanf(pos, "%lf,%lf", &lat, &lon) == 2) {
                double px;
                double py;
                char short_name[24];
                int local_x;
                int local_y;
                mesh_map_lonlat_to_pixel(lat, lon, zoom, &px, &py);
                local_x = (int)(px - top_left_x);
                local_y = (int)(py - top_left_y);
                if(!mesh_node_line_value(line, "short=", short_name,
                                         sizeof(short_name)) ||
                   mesh_node_text_missing(short_name)) {
                    snprintf(short_name, sizeof(short_name), "%.10s", line);
                }
                mesh_map_add_marker(map, local_x, local_y, 0xF59E0B,
                                    short_name);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static void mesh_map_draw_waypoints(lv_obj_t *map, const char *waypoints_text,
                                    double top_left_x, double top_left_y,
                                    int zoom)
{
    char waypoints_copy[4096];
    char *saveptr = NULL;
    char *line;

    if(!waypoints_text || !waypoints_text[0]) {
        return;
    }
    snprintf(waypoints_copy, sizeof(waypoints_copy), "%s", waypoints_text);
    line = strtok_r(waypoints_copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "wp ", 3) == 0) {
            char lat_text[32];
            char lon_text[32];
            char name[48];
            char id_text[24];
            char *endptr;
            double lat;
            double lon;

            if(mesh_node_line_value(line, "lat=", lat_text,
                                    sizeof(lat_text)) &&
               mesh_node_line_value(line, "lon=", lon_text,
                                    sizeof(lon_text))) {
                double px;
                double py;
                int local_x;
                int local_y;

                lat = strtod(lat_text, &endptr);
                if(endptr == lat_text || !isfinite(lat)) {
                    line = strtok_r(NULL, "\n", &saveptr);
                    continue;
                }
                lon = strtod(lon_text, &endptr);
                if(endptr == lon_text || !isfinite(lon)) {
                    line = strtok_r(NULL, "\n", &saveptr);
                    continue;
                }
                if(!mesh_node_line_value(line, "name=", name,
                                         sizeof(name)) ||
                   mesh_node_text_missing(name)) {
                    if(mesh_node_line_value(line, "id=", id_text,
                                            sizeof(id_text))) {
                        snprintf(name, sizeof(name), "%s", id_text);
                    } else {
                        snprintf(name, sizeof(name), "%s",
                                 ui_tr("Waypoint"));
                    }
                }
                mesh_map_lonlat_to_pixel(lat, lon, zoom, &px, &py);
                local_x = (int)(px - top_left_x);
                local_y = (int)(py - top_left_y);
                mesh_map_add_marker(map, local_x, local_y, 0x38BDF8,
                                    name);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static void mesh_map_draw_tiles(lv_obj_t *map, double center_lat,
                                double center_lon, int zoom,
                                const char *nodes_text,
                                const char *waypoints_text, int view_w,
                                int view_h, int *missing_out)
{
    lv_obj_t *layer;
    double center_x;
    double center_y;
    double top_left_x;
    double top_left_y;
    int map_w = view_w > 0 ? view_w : lv_obj_get_width(map);
    int map_h = view_h > 0 ? view_h : lv_obj_get_height(map);
    int n = 1 << zoom;
    int tx0;
    int tx1;
    int ty0;
    int ty1;
    int missing = 0;

    mesh_map_lonlat_to_pixel(center_lat, center_lon, zoom, &center_x,
                             &center_y);
    top_left_x = center_x - (double)map_w / 2.0;
    top_left_y = center_y - (double)map_h / 2.0;
    tx0 = (int)floor(top_left_x / MESHTASTIC_MAP_TILE_SIZE);
    tx1 = (int)floor((top_left_x + map_w) / MESHTASTIC_MAP_TILE_SIZE);
    ty0 = (int)floor(top_left_y / MESHTASTIC_MAP_TILE_SIZE);
    ty1 = (int)floor((top_left_y + map_h) / MESHTASTIC_MAP_TILE_SIZE);
    tx0 -= MESHTASTIC_MAP_PREFETCH_TILES;
    tx1 += MESHTASTIC_MAP_PREFETCH_TILES;
    ty0 -= MESHTASTIC_MAP_PREFETCH_TILES;
    ty1 += MESHTASTIC_MAP_PREFETCH_TILES;

    layer = lv_obj_create(map);
    mesh_map_layer_obj = layer;
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_size(layer, map_w, map_h);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_radius(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    for(int ty = ty0; ty <= ty1; ty++) {
        if(ty < 0 || ty >= n) {
            continue;
        }
        for(int tx = tx0; tx <= tx1; tx++) {
            char path[256];
            char fallback[64];
            int tile_x = tx;
            int local_x;
            int local_y;

            while(tile_x < 0) {
                tile_x += n;
            }
            while(tile_x >= n) {
                tile_x -= n;
            }
            local_x = (int)((double)tx * MESHTASTIC_MAP_TILE_SIZE -
                            top_left_x);
            local_y = (int)((double)ty * MESHTASTIC_MAP_TILE_SIZE -
                            top_left_y);
            mesh_map_tile_path(zoom, tile_x, ty, path, sizeof(path));
            if(ui_path_exists(path)) {
                lv_obj_t *img = lv_image_create(layer);
                lv_image_set_src(img, path);
                lv_obj_set_pos(img, local_x, local_y);
                lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
            } else {
                snprintf(fallback, sizeof(fallback), "z%d/%d/%d", zoom,
                         tile_x, ty);
                mesh_map_add_placeholder_tile(layer, local_x, local_y,
                                              fallback);
                missing++;
            }
        }
    }

    mesh_map_draw_nodes(layer, nodes_text, top_left_x, top_left_y, zoom);
    mesh_map_draw_waypoints(layer, waypoints_text, top_left_x, top_left_y,
                            zoom);
    if(missing_out) {
        *missing_out = missing;
    }
}

static void mesh_map_add_current_position_overlay(lv_obj_t *map,
                                                  double center_lat,
                                                  double center_lon,
                                                  double self_lat,
                                                  double self_lon,
                                                  int zoom, int view_w,
                                                  int view_h)
{
    double center_x;
    double center_y;
    double self_x;
    double self_y;
    double top_left_x;
    double top_left_y;
    int local_x;
    int local_y;
    int map_w = view_w > 0 ? view_w : lv_obj_get_width(map);
    int map_h = view_h > 0 ? view_h : lv_obj_get_height(map);
    char marker_label[80];

    mesh_map_lonlat_to_pixel(center_lat, center_lon, zoom, &center_x,
                             &center_y);
    mesh_map_lonlat_to_pixel(self_lat, self_lon, zoom, &self_x, &self_y);
    top_left_x = center_x - (double)map_w / 2.0;
    top_left_y = center_y - (double)map_h / 2.0;
    local_x = (int)(self_x - top_left_x);
    local_y = (int)(self_y - top_left_y);
    mesh_ui_trace("map current overlay center=%.5f,%.5f self=%.5f,%.5f local=%d,%d zoom=%d",
                  center_lat, center_lon, self_lat, self_lon, local_x,
                  local_y, zoom);
    snprintf(marker_label, sizeof(marker_label), "%s\n%.5f, %.5f",
             mesh_map_fake_gps_enabled ? "DEBUG" : "ME", self_lat,
             self_lon);
    mesh_map_add_self_marker(map, local_x, local_y, marker_label);
}

static void mesh_map_rebuild(void);

static void mesh_map_rebuild_async(void *user_data)
{
    (void)user_data;
    mesh_map_rebuild();
}

static void mesh_map_recenter(void)
{
    mesh_map_center_valid = 0;
    mesh_map_drag_active = 0;
    mesh_map_drag_dirty = 0;
    mesh_map_drag_total_dx = 0;
    mesh_map_drag_total_dy = 0;
    mesh_map_pinch_active = 0;
}

static void mesh_map_pan_by_pixels(int dx, int dy)
{
    double px;
    double py;

    if(!mesh_map_center_valid) {
        return;
    }
    mesh_map_lonlat_to_pixel(mesh_map_center_lat, mesh_map_center_lon,
                             mesh_map_zoom, &px, &py);
    px -= (double)dx;
    py -= (double)dy;
    mesh_map_pixel_to_lonlat(px, py, mesh_map_zoom, &mesh_map_center_lat,
                             &mesh_map_center_lon);
}

static void mesh_map_shift_layer(int dx, int dy)
{
    if(!mesh_map_layer_obj || !lv_obj_is_valid(mesh_map_layer_obj)) {
        return;
    }
    lv_obj_set_pos(mesh_map_layer_obj, lv_obj_get_x(mesh_map_layer_obj) + dx,
                   lv_obj_get_y(mesh_map_layer_obj) + dy);
}

static int mesh_map_touch_point_in_obj(lv_obj_t *obj, int x, int y)
{
    lv_area_t coords;

    if(!obj || !lv_obj_is_valid(obj)) {
        return 0;
    }
    lv_obj_get_coords(obj, &coords);
    return x >= coords.x1 && x <= coords.x2 && y >= coords.y1 &&
           y <= coords.y2;
}

static int mesh_map_get_touch_pair(lv_obj_t *map, ui_touch_point_t *a,
                                   ui_touch_point_t *b)
{
    ui_touch_point_t points[UI_MULTITOUCH_MAX_POINTS];
    uint32_t count = ui_multitouch_get_points(points, UI_MULTITOUCH_MAX_POINTS);
    int found = 0;

    for(uint32_t i = 0; i < count; i++) {
        if(!points[i].active ||
           !mesh_map_touch_point_in_obj(map, points[i].x, points[i].y)) {
            continue;
        }
        if(found == 0 && a) {
            *a = points[i];
        } else if(found == 1 && b) {
            *b = points[i];
        }
        found++;
        if(found >= 2) {
            return 1;
        }
    }
    return 0;
}

static double mesh_map_touch_distance(const ui_touch_point_t *a,
                                      const ui_touch_point_t *b)
{
    double dx = (double)a->x - (double)b->x;
    double dy = (double)a->y - (double)b->y;

    return sqrt(dx * dx + dy * dy);
}

static int mesh_map_pinch_target_zoom(double distance)
{
    int target = mesh_map_pinch_start_zoom;
    double ratio;

    if(mesh_map_pinch_start_distance < 24.0 || distance < 24.0) {
        return mesh_map_zoom;
    }
    ratio = distance / mesh_map_pinch_start_distance;
    while(ratio >= MESHTASTIC_MAP_PINCH_IN_RATIO &&
          target < MESHTASTIC_MAP_MAX_ZOOM) {
        target++;
        ratio /= MESHTASTIC_MAP_PINCH_IN_RATIO;
    }
    while(ratio <= MESHTASTIC_MAP_PINCH_OUT_RATIO &&
          target > MESHTASTIC_MAP_MIN_ZOOM) {
        target--;
        ratio /= MESHTASTIC_MAP_PINCH_OUT_RATIO;
    }
    return target;
}

static int mesh_map_handle_pinch(lv_obj_t *map)
{
    ui_touch_point_t a;
    ui_touch_point_t b;
    double distance;
    int target_zoom;

    if(!mesh_map_get_touch_pair(map, &a, &b)) {
        return 0;
    }
    distance = mesh_map_touch_distance(&a, &b);
    if(distance < 24.0) {
        return 1;
    }
    if(!mesh_map_pinch_active) {
        if(mesh_map_drag_dirty) {
            mesh_map_pan_by_pixels(mesh_map_drag_total_dx,
                                   mesh_map_drag_total_dy);
            mesh_map_drag_total_dx = 0;
            mesh_map_drag_total_dy = 0;
            mesh_map_drag_dirty = 0;
        }
        mesh_map_pinch_active = 1;
        mesh_map_drag_active = 0;
        mesh_map_pinch_start_distance = distance;
        mesh_map_pinch_start_zoom = mesh_map_zoom;
        return 1;
    }

    target_zoom = mesh_map_pinch_target_zoom(distance);
    if(target_zoom != mesh_map_zoom) {
        mesh_map_zoom = target_zoom;
        mesh_map_drag_dirty = 1;
    }
    app_request_fast_refresh();
    return 1;
}

static void mesh_map_drag_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *map = lv_event_get_target(event);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    int dx;
    int dy;

    if(!indev || !map || !lv_obj_is_valid(map)) {
        return;
    }
    lv_indev_get_point(indev, &point);
    if((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) &&
       mesh_map_handle_pinch(map)) {
        lv_event_stop_processing(event);
        return;
    }
    if(code == LV_EVENT_PRESSED) {
        mesh_map_drag_active = 1;
        mesh_map_drag_dirty = 0;
        mesh_map_drag_total_dx = 0;
        mesh_map_drag_total_dy = 0;
        mesh_map_drag_last_point = point;
        lv_event_stop_processing(event);
        return;
    }
    if(code == LV_EVENT_PRESSING) {
        if(!mesh_map_drag_active || !mesh_map_center_valid) {
            return;
        }
        dx = point.x - mesh_map_drag_last_point.x;
        dy = point.y - mesh_map_drag_last_point.y;
        if(abs(dx) < 1 && abs(dy) < 1) {
            return;
        }
        mesh_map_shift_layer(dx, dy);
        mesh_map_drag_total_dx += dx;
        mesh_map_drag_total_dy += dy;
        mesh_map_drag_last_point = point;
        mesh_map_drag_dirty = 1;
        app_request_fast_refresh();
        lv_event_stop_processing(event);
        return;
    }
    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(mesh_map_pinch_active) {
            if(mesh_map_drag_dirty) {
                mesh_map_save_prefs();
                lv_async_call(mesh_map_rebuild_async, NULL);
            }
        } else if(mesh_map_drag_active && mesh_map_drag_dirty) {
            mesh_map_pan_by_pixels(mesh_map_drag_total_dx,
                                   mesh_map_drag_total_dy);
            lv_async_call(mesh_map_rebuild_async, NULL);
        }
        mesh_map_drag_active = 0;
        mesh_map_drag_dirty = 0;
        mesh_map_drag_total_dx = 0;
        mesh_map_drag_total_dy = 0;
        mesh_map_pinch_active = 0;
        lv_event_stop_processing(event);
    }
}

static void mesh_map_fake_button_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_map_fake_gps_enabled = mesh_map_fake_gps_enabled ? 0 : 1;
    mesh_map_recenter();
    mesh_map_save_prefs();
    mesh_map_rebuild();
}

static void mesh_map_zoom_event_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);

    mesh_map_zoom += delta;
    if(mesh_map_zoom < MESHTASTIC_MAP_MIN_ZOOM) {
        mesh_map_zoom = MESHTASTIC_MAP_MIN_ZOOM;
    } else if(mesh_map_zoom > MESHTASTIC_MAP_MAX_ZOOM) {
        mesh_map_zoom = MESHTASTIC_MAP_MAX_ZOOM;
    }
    mesh_map_save_prefs();
    mesh_map_rebuild();
}

static lv_obj_t *mesh_map_zoom_button(lv_obj_t *parent, int x, int y,
                                      const char *text, intptr_t delta)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, mesh_map_zoom_event_cb, LV_EVENT_CLICKED,
                        (void *)delta);

    label = ui_label(btn, text, &lv_font_montserrat_28, 0xF8FAFC);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void mesh_map_add_zoom_controls(lv_obj_t *map, int map_w, int map_h)
{
    lv_obj_t *panel;
    int panel_w = 66;
    int panel_h = 122;
    int panel_x = map_w - panel_w - 14;
    int panel_y = (map_h - panel_h) / 2;

    if(panel_x < 8) {
        panel_x = 8;
    }
    if(panel_y + panel_h > map_h - 8) {
        panel_y = map_h > panel_h + 16 ? map_h - panel_h - 8 : 8;
    }

    panel = lv_obj_create(map);
    lv_obj_set_pos(panel, panel_x, panel_y);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    mesh_map_zoom_button(panel, (panel_w - 50) / 2, 8, "+", (intptr_t)1);
    mesh_map_zoom_button(panel, (panel_w - 50) / 2, 64, "-", (intptr_t)-1);
    lv_obj_move_foreground(panel);
}

static void mesh_map_add_zoom_badge(lv_obj_t *map, int map_w)
{
    lv_obj_t *badge;
    char text[32];
    int badge_w = 96;
    int x = map_w - badge_w - 90;

    if(x < 12) {
        x = 12;
    }
    snprintf(text, sizeof(text), "%s %d", ui_tr("Zoom"), mesh_map_zoom);
    badge = ui_label(map, text, &lv_font_montserrat_16, 0xF8FAFC);
    lv_obj_set_pos(badge, x, 12);
    lv_obj_set_width(badge, badge_w);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_80, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_pad_left(badge, 10, 0);
    lv_obj_set_style_pad_right(badge, 10, 0);
    lv_obj_set_style_pad_top(badge, 5, 0);
    lv_obj_set_style_pad_bottom(badge, 5, 0);
    lv_label_set_long_mode(badge, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(badge);
}

static void mesh_map_add_position_badge(lv_obj_t *map, int map_w, int map_h,
                                        const char *reason,
                                        mesh_map_position_state_t state)
{
    lv_obj_t *badge;
    char text[160];
    const char *newline;
    int badge_w = map_w - 24;
    uint32_t color = 0x25C281;

    if(!reason || !reason[0]) {
        return;
    }
    if(badge_w > 430) {
        badge_w = 430;
    } else if(badge_w < 220) {
        badge_w = map_w - 16;
    }
    newline = strchr(reason, '\n');
    if(newline) {
        size_t len = (size_t)(newline - reason);
        if(len >= sizeof(text)) {
            len = sizeof(text) - 1U;
        }
        memcpy(text, reason, len);
        text[len] = '\0';
    } else {
        snprintf(text, sizeof(text), "%s", reason);
    }
    if(state == MESH_MAP_POS_USING_CACHE) {
        color = 0xF5A524;
    } else if(state != MESH_MAP_POS_READY) {
        color = 0xD7DEE8;
    }
    badge = ui_label(map, text, &lv_font_montserrat_16, color);
    lv_obj_set_pos(badge, 12, map_h > 54 ? map_h - 44 : 10);
    lv_obj_set_width(badge, badge_w);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_80, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_pad_left(badge, 10, 0);
    lv_obj_set_style_pad_right(badge, 10, 0);
    lv_obj_set_style_pad_top(badge, 5, 0);
    lv_obj_set_style_pad_bottom(badge, 5, 0);
    lv_label_set_long_mode(badge, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(badge);
}

static void mesh_map_set_notice(const char *text, uint32_t color)
{
    snprintf(mesh_map_notice_text, sizeof(mesh_map_notice_text), "%s",
             text && text[0] ? text : "");
    mesh_map_notice_color = color;
}

static void mesh_map_add_notice(lv_obj_t *map, int map_w)
{
    lv_obj_t *badge;
    int badge_w;

    if(!mesh_map_notice_text[0]) {
        return;
    }
    badge_w = map_w - 24;
    if(badge_w > 360) {
        badge_w = 360;
    }
    if(badge_w < 180) {
        badge_w = map_w > 24 ? map_w - 24 : map_w;
    }
    badge = ui_label(map, mesh_map_notice_text, &lv_font_montserrat_16,
                     mesh_map_notice_color);
    lv_obj_set_pos(badge, (map_w - badge_w) / 2, 52);
    lv_obj_set_width(badge, badge_w);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_80, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_pad_left(badge, 10, 0);
    lv_obj_set_style_pad_right(badge, 10, 0);
    lv_obj_set_style_pad_top(badge, 5, 0);
    lv_obj_set_style_pad_bottom(badge, 5, 0);
    lv_label_set_long_mode(badge, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(badge);
}

static void mesh_map_sanitize_waypoint_name(char *name, size_t name_len)
{
    if(!name || name_len == 0U) {
        return;
    }
    for(size_t i = 0; name[i]; i++) {
        if(name[i] == ',' || name[i] == '\r' || name[i] == '\n' ||
           name[i] == '\t') {
            name[i] = ' ';
        }
    }
    ui_trim_text(name);
    if(!name[0]) {
        snprintf(name, name_len, "K230 waypoint");
    }
}

static void mesh_map_share_event_cb(lv_event_t *event)
{
    char status[4096];
    char reason[128];
    char response[512];
    char command[256];
    char name[48];
    double lat = 0.0;
    double lon = 0.0;
    mesh_map_position_state_t state = MESH_MAP_POS_GNSS_SEARCHING;

    (void)event;
    if(mesh_ipc_command("STATUS\n", status, sizeof(status)) != 0) {
        snprintf(status, sizeof(status), "%s", mesh_status_text);
    }
    if(!mesh_map_current_position(status, &lat, &lon, reason,
                                  sizeof(reason), &state)) {
        mesh_map_set_notice(ui_tr("No position to share"), 0xF5A524);
        mesh_append_log("waypoint share skipped: no position state=%d reason=%s",
                        (int)state, reason);
        mesh_map_rebuild();
        return;
    }
    snprintf(name, sizeof(name), "%s", mesh_node_name[0] ? mesh_node_name :
             "K230 waypoint");
    mesh_map_sanitize_waypoint_name(name, sizeof(name));
    snprintf(command, sizeof(command), "SEND_WAYPOINT %.7f,%.7f,%s\n",
             lat, lon, name);
    if(mesh_ipc_command(command, response, sizeof(response)) == 0 &&
       strncmp(response, "OK waypoint", 11) == 0) {
        mesh_map_set_notice(ui_tr("Waypoint shared"), 0x25C281);
        mesh_append_log("waypoint shared %.7f,%.7f %s", lat, lon, name);
    } else {
        ui_trim_text(response);
        mesh_map_set_notice(ui_tr("Share failed"), 0xEF4D5A);
        mesh_append_log("waypoint share failed: %s", response);
    }
    mesh_map_rebuild();
}

static void mesh_map_add_share_button(lv_obj_t *map, int map_w, int map_h)
{
    lv_obj_t *btn;
    int btn_w = 96;
    int btn_h = 44;
    int x = map_w - btn_w - 14;
    int y = map_h - btn_h - 14;

    if(x < 12) {
        x = 12;
    }
    if(y < 76) {
        y = 76;
    }
    btn = ui_command_button(map, x, y, btn_w, ui_tr("Share"), 0x25C281);
    lv_obj_set_height(btn, btn_h);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, mesh_map_share_event_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_move_foreground(btn);
}

static void mesh_waypoints_close(void)
{
    if(mesh_waypoints_overlay && lv_obj_is_valid(mesh_waypoints_overlay)) {
        lv_obj_delete(mesh_waypoints_overlay);
    }
    mesh_waypoints_overlay = NULL;
    memset(mesh_waypoint_select_lines, 0, sizeof(mesh_waypoint_select_lines));
}

static void mesh_waypoints_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_waypoints_close();
}

static int mesh_waypoints_fetch(char *out, size_t out_len,
                                char *error_text, size_t error_len)
{
    char map_response[MESHTASTIC_MAP_RESPONSE_MAX];
    char response[4096];
    char copy[4096];
    char *saveptr = NULL;
    char *line;
    int count = 0;

    if(out && out_len > 0U) {
        out[0] = '\0';
    }
    if(error_text && error_len > 0U) {
        error_text[0] = '\0';
    }
    if(!out || out_len == 0U) {
        return -1;
    }

    if(mesh_ipc_command("MAP\n", map_response, sizeof(map_response)) == 0 &&
       strncmp(map_response, "OK map", 6) == 0) {
        count = mesh_map_build_legacy_waypoints(map_response, out, out_len);
        if(count > 0) {
            return count;
        }
        if(count < 0) {
            out[0] = '\0';
        }
    }

    if(mesh_ipc_command("WAYPOINTS\n", response, sizeof(response)) != 0) {
        if(error_text && error_len > 0U) {
            snprintf(error_text, error_len, "%s", response);
            ui_trim_text(error_text);
        }
        return -1;
    }

    snprintf(copy, sizeof(copy), "%s", response);
    line = strtok_r(copy, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "wp ", 3) == 0) {
            if(mesh_append_text(out, out_len, "%s\n", line) != 0) {
                return count > 0 ? count : -1;
            }
            count++;
        } else if(strncmp(line, "WAYPOINT ", 9) == 0) {
            char legacy[MESHTASTIC_UI_WAYPOINT_LINE_MAX];
            if(mesh_map_waypoint_to_legacy_line(line, legacy,
                                                sizeof(legacy)) == 0) {
                if(mesh_append_text(out, out_len, "%s\n", legacy) != 0) {
                    return count > 0 ? count : -1;
                }
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

static int mesh_waypoint_parse_position(const char *line, double *lat,
                                        double *lon)
{
    char lat_text[32];
    char lon_text[32];
    char *endptr;
    double parsed_lat;
    double parsed_lon;

    if(!line ||
       !mesh_node_line_value(line, "lat=", lat_text, sizeof(lat_text)) ||
       !mesh_node_line_value(line, "lon=", lon_text, sizeof(lon_text))) {
        return 0;
    }
    parsed_lat = strtod(lat_text, &endptr);
    if(endptr == lat_text || !isfinite(parsed_lat)) {
        return 0;
    }
    parsed_lon = strtod(lon_text, &endptr);
    if(endptr == lon_text || !isfinite(parsed_lon)) {
        return 0;
    }
    if(fabs(parsed_lat) < 0.000001 && fabs(parsed_lon) < 0.000001) {
        return 0;
    }
    if(lat) {
        *lat = parsed_lat;
    }
    if(lon) {
        *lon = parsed_lon;
    }
    return 1;
}

static void mesh_waypoint_center_event_cb(lv_event_t *event)
{
    const char *line = (const char *)lv_event_get_user_data(event);
    double lat = 0.0;
    double lon = 0.0;

    if(!mesh_waypoint_parse_position(line, &lat, &lon)) {
        mesh_map_set_notice(ui_tr("Waypoint has no coordinates"), 0xF5A524);
        mesh_waypoints_close();
        mesh_map_rebuild();
        return;
    }

    mesh_map_center_lat = lat;
    mesh_map_center_lon = lon;
    mesh_map_center_valid = 1;
    mesh_map_drag_active = 0;
    mesh_map_drag_dirty = 0;
    mesh_map_drag_total_dx = 0;
    mesh_map_drag_total_dy = 0;
    mesh_map_pinch_active = 0;
    mesh_map_set_notice(ui_tr("Waypoint centered"), 0x38BDF8);
    mesh_waypoints_close();
    mesh_map_rebuild();
}

static void mesh_waypoints_add_card(lv_obj_t *panel, const char *line,
                                    int x, int y, int w, int h,
                                    size_t select_index)
{
    lv_obj_t *card;
    lv_obj_t *label;
    char id[24];
    char from[24];
    char age[24];
    char name[48];
    char desc[96];
    char meta[192];
    double lat = 0.0;
    double lon = 0.0;

    if(select_index >= MESHTASTIC_UI_WAYPOINT_SELECT_MAX || !line) {
        return;
    }
    snprintf(mesh_waypoint_select_lines[select_index],
             sizeof(mesh_waypoint_select_lines[select_index]), "%s", line);
    mesh_node_line_value(line, "id=", id, sizeof(id));
    mesh_node_line_value(line, "from=", from, sizeof(from));
    mesh_node_line_value(line, "age=", age, sizeof(age));
    mesh_node_line_value(line, "name=", name, sizeof(name));
    mesh_node_line_value(line, "desc=", desc, sizeof(desc));
    if(mesh_node_text_missing(id)) {
        snprintf(id, sizeof(id), "-");
    }
    if(mesh_node_text_missing(from)) {
        snprintf(from, sizeof(from), "-");
    }
    if(mesh_node_text_missing(age)) {
        snprintf(age, sizeof(age), "-");
    }
    if(mesh_node_text_missing(name)) {
        snprintf(name, sizeof(name), "%s", ui_tr("Waypoint"));
    }
    if(mesh_node_text_missing(desc)) {
        snprintf(desc, sizeof(desc), "-");
    }
    (void)mesh_waypoint_parse_position(line, &lat, &lon);

    card = ui_panel(panel, x, y, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, mesh_waypoint_center_event_cb,
                        LV_EVENT_CLICKED,
                        mesh_waypoint_select_lines[select_index]);

    label = ui_label(card, name, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(label, 14, 10);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    snprintf(meta, sizeof(meta), "%.5f, %.5f", lat, lon);
    label = ui_label(card, meta, &lv_font_montserrat_16, 0x38BDF8);
    lv_obj_set_pos(label, 14, 42);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    snprintf(meta, sizeof(meta), "%s %s  %s %s  %s %s",
             ui_tr("From"), from, ui_tr("Age"), age, ui_tr("ID"), id);
    label = ui_label(card, meta, &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(label, 14, 70);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card,
                     strcmp(desc, "-") == 0 ? ui_tr("Tap to center") : desc,
                     &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(label, 14, 96);
    lv_obj_set_width(label, w - 28);
    lv_obj_set_height(label, h - 106);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void mesh_waypoints_event_cb(lv_event_t *event)
{
    char waypoints[4096];
    char error_text[256];
    char copy[4096];
    char *saveptr = NULL;
    char *line;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    lv_obj_t *label;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int cols = landscape ? 2 : 1;
    int gap = 12;
    int card_w = cols == 2 ? (content_w - gap) / 2 : content_w;
    int card_h = landscape ? 128 : 136;
    int y = 94;
    int count;
    int shown = 0;

    (void)event;
    ui_input_hide_inline_active();
    count = mesh_waypoints_fetch(waypoints, sizeof(waypoints),
                                 error_text, sizeof(error_text));
    mesh_waypoints_close();

    mesh_waypoints_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_waypoints_overlay);
    lv_obj_set_style_bg_color(mesh_waypoints_overlay, lv_color_hex(0x05070A),
                              0);
    lv_obj_set_style_bg_opa(mesh_waypoints_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_waypoints_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_waypoints_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_waypoints_overlay, 0, 0);
    lv_obj_clear_flag(mesh_waypoints_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_waypoints_overlay);

    panel = ui_scroll_panel(mesh_waypoints_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Waypoints"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 118);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel, ui_tr("Tap a waypoint to center map"),
                        &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 118);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_waypoints_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    if(count <= 0 || !waypoints[0]) {
        label = ui_label(panel,
                         count < 0 && error_text[0] ? error_text :
                         ui_tr("No waypoints"),
                         &lv_font_montserrat_20, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        mesh_node_detail_append_spacer(panel, y + 92);
        return;
    }

    snprintf(copy, sizeof(copy), "%s", waypoints);
    line = strtok_r(copy, "\n", &saveptr);
    while(line && shown < MESHTASTIC_UI_WAYPOINT_SELECT_MAX) {
        if(strncmp(line, "wp ", 3) == 0) {
            int col = shown % cols;
            int row = shown / cols;
            int card_x = margin + col * (card_w + gap);
            int card_y = y + row * (card_h + gap);
            mesh_waypoints_add_card(panel, line, card_x, card_y, card_w,
                                    card_h, (size_t)shown);
            shown++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    mesh_node_detail_append_spacer(panel,
                                   y + ((shown + cols - 1) / cols) *
                                   (card_h + gap) + 48);
}

static void mesh_map_add_waypoints_button(lv_obj_t *map)
{
    lv_obj_t *btn;
    int btn_w = ui_is_landscape() ? 132 : 116;
    int btn_h = 44;

    btn = ui_command_button(map, 12, 12, btn_w, ui_tr("Waypoints"),
                            0x38BDF8);
    lv_obj_set_height(btn, btn_h);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, mesh_waypoints_event_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_move_foreground(btn);
}

static void mesh_map_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_map_recenter();
    mesh_map_rebuild();
}

static const char *mesh_map_position_hint(mesh_map_position_state_t state)
{
    switch(state) {
    case MESH_MAP_POS_DAEMON_OFFLINE:
        return ui_tr("Waiting for Meshtastic service.");
    case MESH_MAP_POS_NRF9151_MISSING:
        return ui_tr("Install nRF9151 or enable Debug GPS.");
    case MESH_MAP_POS_USING_CACHE:
        return ui_tr("Move outdoors to refresh GNSS fix.");
    case MESH_MAP_POS_FIRST_FIX:
        return ui_tr("Waiting for the first valid NMEA fix.");
    case MESH_MAP_POS_NO_SATELLITES:
        return ui_tr("Check GNSS antenna and sky view.");
    case MESH_MAP_POS_SATELLITES_NO_FIX:
        return ui_tr("Satellites are visible; keep antenna still.");
    case MESH_MAP_POS_GNSS_ERROR:
        return ui_tr("Open Cellular app to check GNSS.");
    case MESH_MAP_POS_COORD_UNAVAILABLE:
    case MESH_MAP_POS_GNSS_SEARCHING:
        return ui_tr("Open sky improves GNSS fix.");
    case MESH_MAP_POS_READY:
    default:
        return "";
    }
}

static void mesh_map_draw_position_state(lv_obj_t *map, int map_w, int map_h,
                                         const char *reason,
                                         mesh_map_position_state_t state)
{
    lv_obj_t *card;
    lv_obj_t *spinner = NULL;
    lv_obj_t *label;
    lv_obj_t *hint;
    const char *hint_text = mesh_map_position_hint(state);
    int show_spinner = state == MESH_MAP_POS_GNSS_SEARCHING ||
                       state == MESH_MAP_POS_FIRST_FIX ||
                       state == MESH_MAP_POS_NO_SATELLITES ||
                       state == MESH_MAP_POS_SATELLITES_NO_FIX ||
                       state == MESH_MAP_POS_COORD_UNAVAILABLE ||
                       state == MESH_MAP_POS_DAEMON_OFFLINE;
    int card_w = map_w - 48;
    int card_h = show_spinner ? 192 : 148;
    int text_y = show_spinner ? 74 : 24;

    if(card_w > 420) {
        card_w = 420;
    }
    if(card_w < 220) {
        card_w = map_w - 24;
    }
    if(card_h > map_h - 24) {
        card_h = map_h - 24;
    }

    card = lv_obj_create(map);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x233044), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if(show_spinner) {
        spinner = lv_spinner_create(card);
        lv_obj_set_size(spinner, 42, 42);
        lv_obj_set_pos(spinner, (card_w - 42) / 2, 20);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(0x25C281),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(0x233044),
                                   LV_PART_MAIN);
    }

    label = ui_label(card, reason && reason[0] ? reason :
                     ui_tr("nRF9151 GNSS locating..."),
                     &lv_font_montserrat_20,
                     state == MESH_MAP_POS_NRF9151_MISSING ? 0xF5A524 :
                     state == MESH_MAP_POS_USING_CACHE ? 0xF5A524 :
                     state == MESH_MAP_POS_GNSS_ERROR ? 0xEF4D5A :
                     0xD7DEE8);
    lv_obj_set_pos(label, 16, text_y);
    lv_obj_set_width(label, card_w - 32);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    if(hint_text && hint_text[0]) {
        hint = ui_label(card, hint_text, &lv_font_montserrat_16, 0x94A3B8);
        lv_obj_set_pos(hint, 18, text_y + 46);
        lv_obj_set_width(hint, card_w - 36);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void mesh_map_rebuild(void)
{
    char status[4096];
    char nodes[8192];
    char waypoints[4096];
    char map_response[MESHTASTIC_MAP_RESPONSE_MAX];
    char reason[128];
    char info[256];
    double lat = 0.0;
    double lon = 0.0;
    double self_lat = 0.0;
    double self_lon = 0.0;
    int has_position;
    int has_self_position = 0;
    int missing_tiles = 0;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    mesh_map_position_state_t position_state = MESH_MAP_POS_GNSS_SEARCHING;
    int margin = landscape ? 14 : ui_page_side_margin();
    int bottom_margin = landscape ? 12 : margin;
    int content_w = screen_w - margin * 2;
    int title_y = landscape ? 16 : 22;
    int top_y = landscape ? 14 : 18;
    int map_x = margin;
    int map_y = landscape ? 64 : 72;
    int map_w = content_w;
    int map_h = screen_h - map_y - bottom_margin;
    int close_w = 82;
    int refresh_w = 92;
    int debug_w = 132;
    int gap = 8;
    int close_x = screen_w - margin - close_w;
    int refresh_x = close_x - gap - refresh_w;
    int debug_x = refresh_x - gap - debug_w;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *btn;
    lv_obj_t *label;
    lv_obj_t *map;

    if(!mesh_map_overlay || !lv_obj_is_valid(mesh_map_overlay)) {
        return;
    }
    if(map_w < 240) {
        map_w = 240;
    }
    if(map_h < 180) {
        map_h = 180;
    }
    mesh_map_load_prefs();
    mesh_map_view_obj = NULL;
    mesh_map_layer_obj = NULL;
    mesh_map_drag_active = 0;
    mesh_map_drag_dirty = 0;
    mesh_map_drag_total_dx = 0;
    mesh_map_drag_total_dy = 0;
    mesh_map_pinch_active = 0;
    lv_obj_clean(mesh_map_overlay);

    nodes[0] = '\0';
    waypoints[0] = '\0';
    map_response[0] = '\0';
    if(mesh_ipc_command("MAP\n", map_response, sizeof(map_response)) == 0 &&
       strncmp(map_response, "OK map", 6) == 0) {
        if(mesh_map_extract_first_line(map_response, status,
                                       sizeof(status)) != 0) {
            snprintf(status, sizeof(status), "%s", mesh_status_text);
        }
        (void)mesh_map_build_legacy_nodes(map_response, nodes,
                                          sizeof(nodes));
        (void)mesh_map_build_legacy_waypoints(map_response, waypoints,
                                              sizeof(waypoints));
    } else {
        if(mesh_ipc_command("STATUS\n", status, sizeof(status)) != 0) {
            snprintf(status, sizeof(status), "%s", mesh_status_text);
        }
        if(mesh_ipc_command("NODES\n", nodes, sizeof(nodes)) != 0) {
            nodes[0] = '\0';
        } else if(strncmp(nodes, "OK nodes\n", 9) == 0) {
            memmove(nodes, nodes + 9, strlen(nodes + 9) + 1U);
        }
        if(mesh_ipc_command("WAYPOINTS\n", waypoints,
                            sizeof(waypoints)) != 0) {
            waypoints[0] = '\0';
        } else if(strncmp(waypoints, "OK waypoints\n", 13) == 0) {
            memmove(waypoints, waypoints + 13,
                    strlen(waypoints + 13) + 1U);
        }
    }
    has_position = mesh_map_current_position(status, &lat, &lon, reason,
                                             sizeof(reason), &position_state);
    if(has_position) {
        self_lat = lat;
        self_lon = lon;
        has_self_position = 1;
        if(!mesh_map_center_valid) {
            mesh_map_center_lat = lat;
            mesh_map_center_lon = lon;
            mesh_map_center_valid = 1;
        } else {
            lat = mesh_map_center_lat;
            lon = mesh_map_center_lon;
        }
    } else if(mesh_map_center_valid) {
        lat = mesh_map_center_lat;
        lon = mesh_map_center_lon;
        has_position = 1;
    }
    mesh_map_has_position = has_position;
    mesh_ui_trace("map rebuild has_position=%d state=%d reason=%s",
                  has_position, (int)position_state, reason);

    panel = lv_obj_create(mesh_map_overlay);
    ui_set_fullscreen(panel);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if(debug_x < margin + 96) {
        debug_x = margin + 96;
        debug_w = refresh_x - gap - debug_x;
        if(debug_w < 96) {
            debug_w = 96;
        }
    }
    title = ui_label(panel, ui_tr("Mesh Map"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, title_y);
    lv_obj_set_width(title, debug_x - margin - 10);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, debug_x, top_y, debug_w,
                            mesh_map_fake_gps_enabled ?
                            ui_tr("Debug GPS On") :
                            ui_tr("Debug GPS Off"),
                            mesh_map_fake_gps_enabled ? 0x25C281 : 0x374151);
    lv_obj_add_event_cb(btn, mesh_map_fake_button_event_cb, LV_EVENT_CLICKED,
                        NULL);

    btn = ui_command_button(panel, refresh_x, top_y, refresh_w,
                            ui_tr("Refresh"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_map_refresh_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(panel, close_x, top_y, close_w,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_map_close_event_cb, LV_EVENT_CLICKED, NULL);

    map = lv_obj_create(panel);
    mesh_map_view_obj = map;
    lv_obj_set_pos(map, map_x, map_y);
    lv_obj_set_size(map, map_w, map_h);
    lv_obj_set_style_bg_color(map, lv_color_hex(0x08111C), 0);
    lv_obj_set_style_border_color(map, lv_color_hex(0x233044), 0);
    lv_obj_set_style_border_width(map, 1, 0);
    lv_obj_set_style_radius(map, 8, 0);
    lv_obj_set_style_clip_corner(map, 1, 0);
    lv_obj_set_style_pad_all(map, 0, 0);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map, mesh_map_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(map, mesh_map_drag_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(map, mesh_map_drag_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(map, mesh_map_drag_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_update_layout(map);

    if(has_position) {
        mesh_map_draw_tiles(map, lat, lon, mesh_map_zoom, nodes, waypoints,
                            map_w, map_h, &missing_tiles);
        if(missing_tiles > 0) {
            snprintf(info, sizeof(info), "Missing %d offline tiles",
                     missing_tiles);
            label = ui_label(map, info, &lv_font_montserrat_14, 0xF5A524);
            lv_obj_set_style_bg_color(label, lv_color_hex(0x0B1220), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
            lv_obj_set_style_pad_all(label, 6, 0);
            lv_obj_set_pos(label, 12, 62);
        }
        mesh_map_add_waypoints_button(map);
        if(has_self_position) {
            mesh_map_add_current_position_overlay(map, lat, lon, self_lat,
                                                  self_lon, mesh_map_zoom,
                                                  map_w, map_h);
            mesh_map_add_share_button(map, map_w, map_h);
        }
        mesh_map_add_zoom_controls(map, map_w, map_h);
        mesh_map_add_zoom_badge(map, map_w);
        mesh_map_add_position_badge(map, map_w, map_h, reason,
                                    position_state);
        mesh_map_add_notice(map, map_w);
    } else {
        mesh_map_draw_position_state(map, map_w, map_h, reason,
                                     position_state);
        mesh_map_add_waypoints_button(map);
        mesh_map_add_zoom_badge(map, map_w);
        mesh_map_add_notice(map, map_w);
    }

    app_request_fast_refresh();
}

static void mesh_map_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(!mesh_map_overlay || !lv_obj_is_valid(mesh_map_overlay)) {
        if(mesh_map_timer) {
            lv_timer_delete(mesh_map_timer);
            mesh_map_timer = NULL;
        }
        return;
    }
    if(mesh_map_has_position) {
        lv_timer_delete(mesh_map_timer);
        mesh_map_timer = NULL;
        return;
    }
    mesh_map_rebuild();
    if(mesh_map_has_position && mesh_map_timer) {
        lv_timer_delete(mesh_map_timer);
        mesh_map_timer = NULL;
    }
}

static void mesh_map_event_cb(lv_event_t *event)
{
    (void)event;
    ui_input_hide_inline_active();
    if(!mesh_map_overlay || !lv_obj_is_valid(mesh_map_overlay)) {
        mesh_map_overlay = lv_obj_create(lv_screen_active());
        ui_set_fullscreen(mesh_map_overlay);
        lv_obj_set_style_bg_color(mesh_map_overlay, lv_color_hex(0x05070A), 0);
        lv_obj_set_style_bg_opa(mesh_map_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mesh_map_overlay, 0, 0);
        lv_obj_set_style_border_width(mesh_map_overlay, 0, 0);
        lv_obj_set_style_pad_all(mesh_map_overlay, 0, 0);
        lv_obj_clear_flag(mesh_map_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(mesh_map_overlay);
    }
    mesh_map_rebuild();
    if(!mesh_map_has_position && !mesh_map_timer) {
        mesh_map_timer = lv_timer_create(mesh_map_timer_cb, 2000, NULL);
    }
}

static void mesh_node_detail_open(const char *line)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int left_w = landscape ? (content_w - 18) / 2 : content_w;
    int right_x = landscape ? margin + left_w + 18 : margin;
    int right_w = landscape ? content_w - left_w - 18 : content_w;
    int request_gap = 8;
    int request_cols = landscape ? 5 : 3;
    int request_rows = landscape ? 1 : 2;
    int request_area_w = content_w > (landscape ? 760 : 560) ?
                         (landscape ? 760 : 560) : content_w;
    int request_w = (request_area_w - request_gap * (request_cols - 1)) /
                    request_cols;
    int y;
    int y_left;
    int y_right;
    int bottom_y;
    char node_id[24];
    char name[64];
    char short_name[24];
    char hw[16];
    char key[16];
    char rx[16];
    char age[24];
    char rssi[24];
    char snr[24];
    char pos[128];
    char tel[160];
    char trace[192];
    char nbr[192];
    char identity_detail[320];
    char link_detail[320];
    char position_detail[320];
    char telemetry_detail[320];
    char trace_detail[224];
    char neighbor_detail[224];

    if(!line || strncmp(line, "0x", 2) != 0 ||
       sscanf(line, "%23s", node_id) != 1) {
        return;
    }

    mesh_node_line_segment(line, "name=", " short=", name, sizeof(name));
    mesh_node_line_value(line, "short=", short_name, sizeof(short_name));
    mesh_node_line_value(line, "hw=", hw, sizeof(hw));
    mesh_node_line_value(line, "key=", key, sizeof(key));
    mesh_node_line_value(line, "rx=", rx, sizeof(rx));
    mesh_node_line_value(line, "age=", age, sizeof(age));
    mesh_node_line_value(line, "rssi=", rssi, sizeof(rssi));
    mesh_node_line_value(line, "snr=", snr, sizeof(snr));
    mesh_node_line_segment(line, "pos=", " tel=", pos, sizeof(pos));
    mesh_node_line_segment(line, "tel=", " trace=", tel, sizeof(tel));
    mesh_node_line_segment(line, "trace=", " nbr=", trace, sizeof(trace));
    mesh_node_line_segment(line, "nbr=", NULL, nbr, sizeof(nbr));
    mesh_node_format_position(pos, position_detail, sizeof(position_detail));
    mesh_node_format_telemetry(tel, telemetry_detail,
                               sizeof(telemetry_detail));
    mesh_node_format_optional(trace, "No traceroute data", trace_detail,
                              sizeof(trace_detail));
    mesh_node_format_optional(nbr, "No neighbor data", neighbor_detail,
                              sizeof(neighbor_detail));
    if(strcmp(name, "-") == 0 && strcmp(short_name, "-") != 0) {
        snprintf(name, sizeof(name), "%s", short_name);
    }
    identity_detail[0] = '\0';
    mesh_node_append_line(identity_detail, sizeof(identity_detail),
                          "Node ID", node_id);
    mesh_node_append_line(identity_detail, sizeof(identity_detail),
                          "Short name", short_name);
    mesh_node_append_line(identity_detail, sizeof(identity_detail),
                          "Hardware", hw);
    mesh_node_append_line(identity_detail, sizeof(identity_detail),
                          "Public key",
                          (strcmp(key, "yes") == 0 ||
                           strcmp(key, "1") == 0) ?
                          ui_tr("Available") : ui_tr("Missing"));
    link_detail[0] = '\0';
    mesh_node_append_line(link_detail, sizeof(link_detail), "RSSI", rssi);
    mesh_node_append_line(link_detail, sizeof(link_detail), "SNR", snr);
    mesh_node_append_line(link_detail, sizeof(link_detail), "Packets", rx);
    mesh_node_append_line(link_detail, sizeof(link_detail), "Last seen", age);
    snprintf(mesh_node_detail_target_id, sizeof(mesh_node_detail_target_id),
             "%s", node_id);

    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_panel = NULL;
    mesh_nodes_overlay_kind = MESH_NODES_OVERLAY_DETAIL;
    mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_nodes_preserve_scroll = 0;
    mesh_nodes_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_nodes_overlay);
    lv_obj_set_style_bg_color(mesh_nodes_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_nodes_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_nodes_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_nodes_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_nodes_overlay, 0, 0);
    lv_obj_clear_flag(mesh_nodes_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_nodes_overlay);

    panel = ui_scroll_panel(mesh_nodes_overlay, 0, 0, screen_w, screen_h);
    mesh_nodes_panel = panel;
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, name, &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 210);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel, node_id, &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 210);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                            "Message", 0x25C281);
    lv_obj_add_event_cb(btn, mesh_select_node_target_event_cb,
                        LV_EVENT_CLICKED, mesh_node_detail_target_id);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96, "Back",
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_node_detail_back_event_cb,
                        LV_EVENT_CLICKED, NULL);

#define MESH_NODE_REQ_BUTTON(index, label, command, color) \
    do { \
        int bx = margin + ((index) % request_cols) * \
                 (request_w + request_gap); \
        int by = 88 + ((index) / request_cols) * 46; \
        btn = ui_command_button(panel, bx, by, request_w, ui_tr(label), \
                                color); \
        lv_obj_add_event_cb(btn, mesh_node_remote_request_event_cb, \
                            LV_EVENT_CLICKED, command); \
    } while(0)

    MESH_NODE_REQ_BUTTON(0, "Node info", "REQUEST_NODEINFO", 0x25C281);
    MESH_NODE_REQ_BUTTON(1, "Position", "REQUEST_POSITION", 0x3DA5FF);
    MESH_NODE_REQ_BUTTON(2, "Telemetry", "REQUEST_TELEMETRY", 0xA78BFA);
    MESH_NODE_REQ_BUTTON(3, "Trace", "REQUEST_TRACEROUTE", 0xF59E0B);
    MESH_NODE_REQ_BUTTON(4, "Neighbors", "REQUEST_NEIGHBORINFO", 0x14B8A6);
#undef MESH_NODE_REQ_BUTTON

    y = 88 + request_rows * 46 + 6;
    mesh_node_request_status_label =
        ui_label(panel,
                 mesh_node_detail_status_text[0] ?
                 ui_tr(mesh_node_detail_status_text) :
                 ui_tr("Tap a request button to update this node"),
                 &lv_font_montserrat_14,
                 mesh_node_detail_status_color);
    lv_obj_set_pos(mesh_node_request_status_label, margin, y);
    lv_obj_set_width(mesh_node_request_status_label, content_w);
    lv_label_set_long_mode(mesh_node_request_status_label,
                           LV_LABEL_LONG_DOT);

    y += 34;
    y_left = y;
    y_right = y;
    if(landscape) {
        mesh_node_detail_add_card(panel, margin, &y_left, left_w, "Identity",
                                  identity_detail, 0x25C281);
        mesh_node_detail_add_card(panel, right_x, &y_right, right_w, "Signal",
                                  link_detail, 0x3DA5FF);
        mesh_node_detail_add_card(panel, margin, &y_left, left_w, "Telemetry",
                                  telemetry_detail, 0xA78BFA);
        mesh_node_detail_add_card(panel, right_x, &y_right, right_w,
                                  "Position", position_detail, 0xF59E0B);
        mesh_node_detail_add_card(panel, margin, &y_left, left_w, "Trace",
                                  trace_detail, 0xF97316);
        mesh_node_detail_add_card(panel, right_x, &y_right, right_w,
                                  "Neighbors", neighbor_detail, 0x14B8A6);
        mesh_node_detail_move_after_columns(&y_left, &y_right);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w, "Raw",
                                  line, 0x64748B);
        bottom_y = y_left;
    } else {
        mesh_node_detail_add_card(panel, margin, &y_left, content_w,
                                  "Identity", identity_detail, 0x25C281);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w,
                                  "Signal", link_detail, 0x3DA5FF);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w,
                                  "Position", position_detail, 0xF59E0B);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w,
                                  "Telemetry", telemetry_detail, 0xA78BFA);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w, "Trace",
                                  trace_detail, 0xF97316);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w,
                                  "Neighbors", neighbor_detail, 0x14B8A6);
        mesh_node_detail_add_card(panel, margin, &y_left, content_w, "Raw",
                                  line, 0x64748B);
        bottom_y = y_left;
    }
    mesh_node_detail_append_spacer(panel, bottom_y + 48);
}

static void mesh_node_detail_event_cb(lv_event_t *event)
{
    const char *line = (const char *)lv_event_get_user_data(event);

    mesh_node_detail_status_text[0] = '\0';
    mesh_node_detail_status_color = 0x94A3B8;
    mesh_node_detail_refresh_ticks = 0;
    mesh_node_detail_refresh_attempts = 0;
    mesh_node_detail_open(line);
}

static void mesh_add_node_card(lv_obj_t *panel, const char *line,
                               int x, int y, int w, int h,
                               size_t select_index)
{
    lv_obj_t *card;
    lv_obj_t *name_label;
    lv_obj_t *id_label;
    lv_obj_t *meta_label;
    lv_obj_t *detail_label;
    lv_obj_t *hint_label;
    char node_id[24];
    char name[64];
    char short_name[24];
    char hw[16];
    char rx[16];
    char age[24];
    char rssi[24];
    char snr[24];
    char pos[96];
    char detail[260];
    char meta[160];

    if(select_index >= MESHTASTIC_UI_NODE_SELECT_MAX ||
       !line || strncmp(line, "0x", 2) != 0) {
        return;
    }
    if(sscanf(line, "%23s", node_id) != 1) {
        return;
    }
    mesh_node_line_segment(line, "name=", " short=", name, sizeof(name));
    mesh_node_line_value(line, "short=", short_name, sizeof(short_name));
    mesh_node_line_value(line, "hw=", hw, sizeof(hw));
    mesh_node_line_value(line, "rx=", rx, sizeof(rx));
    mesh_node_line_value(line, "age=", age, sizeof(age));
    mesh_node_line_value(line, "rssi=", rssi, sizeof(rssi));
    mesh_node_line_value(line, "snr=", snr, sizeof(snr));
    mesh_node_line_segment(line, "pos=", " tel=", pos, sizeof(pos));

    if(strcmp(name, "-") == 0 && strcmp(short_name, "-") != 0) {
        snprintf(name, sizeof(name), "%s", short_name);
    }
    snprintf(mesh_node_select_ids[select_index],
             sizeof(mesh_node_select_ids[select_index]), "%s", node_id);
    snprintf(mesh_node_select_lines[select_index],
             sizeof(mesh_node_select_lines[select_index]), "%s", line);

    card = ui_panel(panel, x, y, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x243044), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, mesh_node_detail_event_cb,
                        LV_EVENT_CLICKED, mesh_node_select_lines[select_index]);

    name_label = ui_label(card, name, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(name_label, 14, 12);
    lv_obj_set_width(name_label, w - 28);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    id_label = ui_label(card, node_id, &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(id_label, 14, 42);
    lv_obj_set_width(id_label, w - 28);
    lv_label_set_long_mode(id_label, LV_LABEL_LONG_DOT);

    snprintf(meta, sizeof(meta), "RSSI %s  SNR %s  RX %s  Age %s",
             rssi, snr, rx, age);
    meta_label = ui_label(card, meta, &lv_font_montserrat_14, 0x25C281);
    lv_obj_set_pos(meta_label, 14, 68);
    lv_obj_set_width(meta_label, w - 28);
    lv_label_set_long_mode(meta_label, LV_LABEL_LONG_DOT);

    snprintf(detail, sizeof(detail), "HW %s  Pos %s",
             hw, (pos[0] && strcmp(pos, "-") != 0) ? pos : "-");
    detail_label = ui_label(card, detail, &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(detail_label, 14, 94);
    lv_obj_set_width(detail_label, w - 28);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);

    hint_label = ui_label(card, ui_tr("Tap for details"),
                          &lv_font_montserrat_14, 0x3DA5FF);
    lv_obj_set_pos(hint_label, 14, h - 36);
    lv_obj_set_width(hint_label, w - 28);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_DOT);
}

typedef struct {
    char line[MESHTASTIC_UI_NODE_LINE_MAX];
    char node_id[24];
    char name[64];
    char short_name[24];
    char rx[16];
    char age[24];
    char rssi[24];
    char snr[24];
    char pos[96];
    long rx_count;
    long age_s;
    long rssi_dbm;
    double snr_db;
    int has_rssi;
    int has_snr;
    int has_pos;
    int score;
} mesh_detector_node_t;

static long mesh_detector_parse_long(const char *text, long fallback)
{
    char *endptr;
    long value;

    if(!text || !text[0] || strcmp(text, "-") == 0) {
        return fallback;
    }
    value = strtol(text, &endptr, 10);
    if(endptr == text) {
        return fallback;
    }
    return value;
}

static double mesh_detector_parse_double(const char *text, double fallback)
{
    char *endptr;
    double value;

    if(!text || !text[0] || strcmp(text, "-") == 0) {
        return fallback;
    }
    value = strtod(text, &endptr);
    if(endptr == text) {
        return fallback;
    }
    return value;
}

static void mesh_detector_node_prepare(mesh_detector_node_t *node,
                                       const char *line)
{
    long rx_count;
    long age_s;
    long rssi_dbm;
    double snr_db;

    memset(node, 0, sizeof(*node));
    snprintf(node->line, sizeof(node->line), "%s", line ? line : "");
    if(sscanf(node->line, "%23s", node->node_id) != 1) {
        snprintf(node->node_id, sizeof(node->node_id), "-");
    }
    mesh_node_line_segment(node->line, "name=", " short=", node->name,
                           sizeof(node->name));
    mesh_node_line_value(node->line, "short=", node->short_name,
                         sizeof(node->short_name));
    mesh_node_line_value(node->line, "rx=", node->rx, sizeof(node->rx));
    mesh_node_line_value(node->line, "age=", node->age, sizeof(node->age));
    mesh_node_line_value(node->line, "rssi=", node->rssi, sizeof(node->rssi));
    mesh_node_line_value(node->line, "snr=", node->snr, sizeof(node->snr));
    mesh_node_line_segment(node->line, "pos=", " tel=", node->pos,
                           sizeof(node->pos));
    if((!node->name[0] || strcmp(node->name, "-") == 0) &&
       node->short_name[0] && strcmp(node->short_name, "-") != 0) {
        snprintf(node->name, sizeof(node->name), "%s", node->short_name);
    }
    if(!node->name[0] || strcmp(node->name, "-") == 0) {
        snprintf(node->name, sizeof(node->name), "%s", node->node_id);
    }

    rx_count = mesh_detector_parse_long(node->rx, 0);
    age_s = mesh_detector_parse_long(node->age, 999999);
    rssi_dbm = mesh_detector_parse_long(node->rssi, -999);
    snr_db = mesh_detector_parse_double(node->snr, -999.0);
    node->rx_count = rx_count;
    node->age_s = age_s;
    node->rssi_dbm = rssi_dbm;
    node->snr_db = snr_db;
    node->has_rssi = strcmp(node->rssi, "-") != 0;
    node->has_snr = strcmp(node->snr, "-") != 0;
    node->has_pos = node->pos[0] && strcmp(node->pos, "-") != 0;
    node->score = (int)(rx_count > 80 ? 80 : rx_count);
    if(age_s <= 300) {
        node->score += 45;
    } else if(age_s <= 3600) {
        node->score += 22;
    }
    if(node->has_rssi) {
        int rssi_score = (int)(rssi_dbm + 130);
        if(rssi_score < 0) {
            rssi_score = 0;
        }
        if(rssi_score > 55) {
            rssi_score = 55;
        }
        node->score += rssi_score;
    }
    if(node->has_snr) {
        int snr_score = (int)((snr_db + 10.0) * 2.0);
        if(snr_score < 0) {
            snr_score = 0;
        }
        if(snr_score > 40) {
            snr_score = 40;
        }
        node->score += snr_score;
    }
    if(node->has_pos) {
        node->score += 12;
    }
}

static void mesh_detector_sort_nodes(mesh_detector_node_t *nodes, int count)
{
    int i;

    for(i = 0; i < count; i++) {
        int j;
        for(j = i + 1; j < count; j++) {
            if(nodes[j].score > nodes[i].score) {
                mesh_detector_node_t tmp = nodes[i];
                nodes[i] = nodes[j];
                nodes[j] = tmp;
            }
        }
    }
}

static void mesh_detector_close(void)
{
    if(mesh_detector_overlay && lv_obj_is_valid(mesh_detector_overlay)) {
        lv_obj_delete(mesh_detector_overlay);
    }
    mesh_detector_overlay = NULL;
    mesh_detector_panel = NULL;
    mesh_detector_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_detector_preserve_scroll = 0;
}

static void mesh_detector_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_detector_close();
}

static void mesh_detector_summary_card(lv_obj_t *panel, int x, int y, int w,
                                       int h, const char *title,
                                       const char *value, const char *detail,
                                       uint32_t color)
{
    lv_obj_t *card;
    lv_obj_t *label;

    card = ui_panel(panel, x, y, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);

    label = ui_label(card, title, &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(label, 12, 10);
    lv_obj_set_width(label, w - 24);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, value, &lv_font_montserrat_24, color);
    lv_obj_set_pos(label, 12, 34);
    lv_obj_set_width(label, w - 24);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, detail, &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(label, 12, h - 42);
    lv_obj_set_height(label, 30);
    lv_obj_set_width(label, w - 24);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static void mesh_detector_node_row(lv_obj_t *panel,
                                   const mesh_detector_node_t *node,
                                   int x, int y, int w, int h,
                                   size_t select_index)
{
    lv_obj_t *card;
    lv_obj_t *label;
    char meta[192];
    char score[48];

    if(select_index >= MESHTASTIC_UI_NODE_SELECT_MAX || !node) {
        return;
    }
    snprintf(mesh_node_select_lines[select_index],
             sizeof(mesh_node_select_lines[select_index]), "%s", node->line);

    card = ui_panel(panel, x, y, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(card,
                                  lv_color_hex(node->score > 120 ?
                                               0x25C281 : 0x334155), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, mesh_node_detail_event_cb, LV_EVENT_CLICKED,
                        mesh_node_select_lines[select_index]);

    label = ui_label(card, node->name, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_pos(label, 14, 10);
    lv_obj_set_width(label, w - 88);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    snprintf(score, sizeof(score), "%d", node->score);
    label = ui_label(card, score, &lv_font_montserrat_20, 0xF59E0B);
    lv_obj_set_pos(label, w - 66, 10);
    lv_obj_set_width(label, 52);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, node->node_id, &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(label, 14, 38);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    snprintf(meta, sizeof(meta), "RSSI %s  SNR %s  RX %s  Age %s  Pos %s",
             node->rssi, node->snr, node->rx, node->age,
             node->has_pos ? "yes" : "no");
    label = ui_label(card, meta, &lv_font_montserrat_14, 0x25C281);
    lv_obj_set_pos(label, 14, 64);
    lv_obj_set_height(label, h - 78);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void mesh_detector_event_cb(lv_event_t *event)
{
    char status[4096];
    char nodes_response[8192];
    char nodes_text[8192];
    char region[24];
    char preset[32];
    char channel[64];
    char freq[24];
    char rx[24];
    char tx[24];
    char dup[24];
    char nodedb[24];
    char hist[24];
    char profile[192];
    char value[64];
    char detail[160];
    char *saveptr = NULL;
    char *line;
    mesh_detector_node_t nodes[MESHTASTIC_UI_NODE_SELECT_MAX];
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    lv_obj_t *label;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int summary_cols = landscape ? 4 : 2;
    int summary_gap = 10;
    int summary_w = (content_w - summary_gap * (summary_cols - 1)) /
                    summary_cols;
    int summary_h = landscape ? 116 : 118;
    int summary_rows = landscape ? 1 : 2;
    int node_cols = landscape ? 2 : 1;
    int node_gap = 12;
    int node_w = node_cols == 2 ? (content_w - node_gap) / 2 : content_w;
    int node_h = 122;
    int y = 0;
    int node_count = 0;
    int db_count = 0;
    int detected_count = 0;
    int active_5m = 0;
    int active_15m = 0;
    int positioned = 0;
    int weak = 0;
    int best_rssi_valid = 0;
    int best_snr_valid = 0;
    long packets_total = 0;
    long best_rssi = 0;
    double best_snr = 0.0;
    int32_t old_scroll_y =
        mesh_detector_preserve_scroll ?
        mesh_overlay_scroll_y(mesh_detector_panel) : 0;

    (void)event;
    ui_input_hide_inline_active();
    if(mesh_ipc_command("STATUS\n", status, sizeof(status)) != 0) {
        ui_trim_text(status);
        mesh_append_log("detector status failed: %s", status);
        snprintf(status, sizeof(status), "%s", "ERR offline");
    }
    if(mesh_fetch_legacy_nodes(nodes_text, sizeof(nodes_text),
                               nodes_response, sizeof(nodes_response),
                               NULL, 0) != 0) {
        ui_trim_text(nodes_response);
        mesh_append_log("detector nodes failed: %s", nodes_response);
        nodes_text[0] = '\0';
    }

    mesh_status_field(status, "region", region, sizeof(region), "-");
    mesh_status_field(status, "preset", preset, sizeof(preset), "-");
    mesh_status_field(status, "channel", channel, sizeof(channel), "-");
    mesh_status_field(status, "freq", freq, sizeof(freq), "-");
    mesh_status_field(status, "rx", rx, sizeof(rx), "0");
    mesh_status_field(status, "tx", tx, sizeof(tx), "0");
    mesh_status_field(status, "dup", dup, sizeof(dup), "0");
    mesh_status_field(status, "nodedb", nodedb, sizeof(nodedb), "0");
    mesh_status_field(status, "hist", hist, sizeof(hist), "0");
    snprintf(profile, sizeof(profile), "%s / %s / %s MHz / %s",
             region, preset, freq, channel);

    line = strtok_r(nodes_text, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "0x", 2) == 0) {
            mesh_detector_node_t parsed;
            int recent;
            mesh_detector_node_prepare(&parsed, line);
            db_count++;
            recent = parsed.age_s <= MESHTASTIC_NODE_RECENT_WINDOW_S;
            if(recent) {
                detected_count++;
                packets_total += parsed.rx_count;
                if(parsed.age_s <= 300) {
                    active_5m++;
                }
                active_15m++;
                if(parsed.has_pos) {
                    positioned++;
                }
                if((parsed.has_rssi && parsed.rssi_dbm <= -105) ||
                   (parsed.has_snr && parsed.snr_db < 0.0)) {
                    weak++;
                }
                if(parsed.has_rssi &&
                   (!best_rssi_valid || parsed.rssi_dbm > best_rssi)) {
                    best_rssi = parsed.rssi_dbm;
                    best_rssi_valid = 1;
                }
                if(parsed.has_snr &&
                   (!best_snr_valid || parsed.snr_db > best_snr)) {
                    best_snr = parsed.snr_db;
                    best_snr_valid = 1;
                }
                if(node_count < MESHTASTIC_UI_NODE_SELECT_MAX - 1) {
                    nodes[node_count++] = parsed;
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    mesh_detector_sort_nodes(nodes, node_count);
    if(strcmp(nodedb, "0") == 0 && db_count > 0) {
        snprintf(nodedb, sizeof(nodedb), "%d", db_count);
    }

    mesh_detector_close();
    mesh_detector_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_detector_overlay);
    lv_obj_set_style_bg_color(mesh_detector_overlay, lv_color_hex(0x05070A),
                              0);
    lv_obj_set_style_bg_opa(mesh_detector_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_detector_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_detector_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_detector_overlay, 0, 0);
    lv_obj_clear_flag(mesh_detector_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_detector_overlay);

    panel = ui_scroll_panel(mesh_detector_overlay, 0, 0, screen_w, screen_h);
    mesh_detector_panel = panel;
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Mesh Detector"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 220);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel, ui_tr("Current-channel mesh activity"),
                        &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 220);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                            ui_tr("Refresh"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_detector_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_detector_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    y = 98;
#define MESH_DETECTOR_SUMMARY(index, title, card_value, card_detail, color) \
    mesh_detector_summary_card(panel, \
                               margin + ((index) % summary_cols) * \
                               (summary_w + summary_gap), \
                               y + ((index) / summary_cols) * \
                               (summary_h + summary_gap), \
                               summary_w, summary_h, title, card_value, \
                               card_detail, color)

    snprintf(value, sizeof(value), "%d", detected_count);
    snprintf(detail, sizeof(detail), "15m / db %s", nodedb);
    MESH_DETECTOR_SUMMARY(0, ui_tr("Mesh nodes"), value, detail, 0x25C281);

    snprintf(value, sizeof(value), "%d / %d", active_5m, active_15m);
    MESH_DETECTOR_SUMMARY(1, ui_tr("Active nodes"), value, "5m / 15m",
                          0x3DA5FF);

    if(best_rssi_valid) {
        snprintf(value, sizeof(value), "%ld dBm", best_rssi);
    } else {
        snprintf(value, sizeof(value), "%s", "-");
    }
    if(best_snr_valid) {
        snprintf(detail, sizeof(detail), "SNR %.1f", best_snr);
    } else {
        snprintf(detail, sizeof(detail), "SNR -");
    }
    MESH_DETECTOR_SUMMARY(2, ui_tr("Best signal"), value, detail, 0xF59E0B);

    snprintf(value, sizeof(value), "%ld", packets_total);
    snprintf(detail, sizeof(detail), "rx %s / tx %s / dup %s", rx, tx, dup);
    MESH_DETECTOR_SUMMARY(3, ui_tr("Packets"), value, detail, 0xA78BFA);
#undef MESH_DETECTOR_SUMMARY

    y += summary_rows * summary_h + (summary_rows - 1) * summary_gap;
    if(!landscape) {
        y += summary_gap;
        snprintf(value, sizeof(value), "%d", positioned);
        mesh_detector_summary_card(panel, margin, y, summary_w, summary_h,
                                   ui_tr("Positioned"), value, profile,
                                   0x14B8A6);
        snprintf(value, sizeof(value), "%d", weak);
        snprintf(detail, sizeof(detail), "history %s", hist);
        mesh_detector_summary_card(panel, margin + summary_w + summary_gap, y,
                                   summary_w, summary_h, ui_tr("Weak links"),
                                   value, detail, 0xEF4D5A);
        y += summary_h;
    }

    y += 24;
    label = ui_label(panel, ui_tr("Current profile"),
                     &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_pos(label, margin, y);
    lv_obj_set_width(label, content_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    y += 30;
    label = ui_label(panel, profile, &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(label, margin, y);
    lv_obj_set_width(label, content_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    y += 38;

    label = ui_label(panel, ui_tr("Detector ranking"),
                     &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_pos(label, margin, y);
    lv_obj_set_width(label, content_w);
    y += 36;

    if(node_count == 0) {
        label = ui_label(panel, ui_tr("No mesh activity detected yet"),
                         &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        mesh_detector_auto_refresh_ticks =
            MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
        mesh_overlay_restore_scroll(mesh_detector_panel, old_scroll_y);
        return;
    }

    for(int i = 0; i < node_count; i++) {
        int col = i % node_cols;
        int row = i / node_cols;
        int x = margin + col * (node_w + node_gap);
        int row_y = y + row * (node_h + node_gap);
        mesh_detector_node_row(panel, &nodes[i], x, row_y, node_w, node_h,
                               (size_t)(i + 1));
    }
    mesh_detector_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_overlay_restore_scroll(mesh_detector_panel, old_scroll_y);
}

static void mesh_settings_status_summary(const char *status, char *line1,
                                         size_t line1_len, char *line2,
                                         size_t line2_len, char *line3,
                                         size_t line3_len)
{
    char chip[24];
    char op[24];
    char region[24];
    char preset[32];
    char freq[24];
    char channel[64];
    char tx[16];
    char rx[16];
    char queued[16];
    char nodes[16];
    char ble[24];
    char gnss[24];
    char sats[16];
    float ch_value;
    int online = mesh_status_is_online(status);

    if(line1 && line1_len > 0U) {
        line1[0] = '\0';
    }
    if(line2 && line2_len > 0U) {
        line2[0] = '\0';
    }
    if(line3 && line3_len > 0U) {
        line3[0] = '\0';
    }
    if(!online) {
        if(line1 && line1_len > 0U) {
            snprintf(line1, line1_len, "%s", ui_tr("Daemon offline"));
        }
        if(line2 && line2_len > 0U) {
            snprintf(line2, line2_len, "%s", ui_tr("Tap Start to run Mesh"));
        }
        if(line3 && line3_len > 0U) {
            snprintf(line3, line3_len, "%s", status && status[0] ? status : "-");
        }
        return;
    }

    mesh_status_field(status, "chip", chip, sizeof(chip), "-");
    mesh_status_field(status, "op", op, sizeof(op), "-");
    mesh_status_field(status, "region", region, sizeof(region), "-");
    mesh_status_field(status, "preset", preset, sizeof(preset), "-");
    mesh_status_field(status, "freq", freq, sizeof(freq), "-");
    mesh_status_field(status, "channel", channel, sizeof(channel), "-");
    mesh_status_field(status, "tx", tx, sizeof(tx), "0");
    mesh_status_field(status, "rx", rx, sizeof(rx), "0");
    mesh_status_field(status, "queued_count", queued, sizeof(queued), "0");
    mesh_status_field(status, "nodedb", nodes, sizeof(nodes), "0");
    mesh_status_field(status, "ble", ble, sizeof(ble), "-");
    mesh_status_field(status, "gps", gnss, sizeof(gnss), "-");
    mesh_status_field(status, "sats", sats, sizeof(sats), "0");
    ch_value = mesh_status_float_field(status, "ch_util", 0.0f);

    if(line1 && line1_len > 0U) {
        snprintf(line1, line1_len, "%s  %s / %s / %s MHz",
                 ui_tr("Daemon online"), chip, op, freq);
    }
    if(line2 && line2_len > 0U) {
        snprintf(line2, line2_len, "%s / %s  %s",
                 region, preset, channel);
    }
    if(line3 && line3_len > 0U) {
        snprintf(line3, line3_len,
                 "TX %s  RX %s  Q %s  Nodes %s  BLE %s  GNSS %s S%s  Ch %.1f%%",
                 tx, rx, queued, nodes, ble, gnss, sats, ch_value);
    }
}

static void mesh_settings_add_status_card(lv_obj_t *parent, int x, int y,
                                          int w, int h)
{
    lv_obj_t *card;
    lv_obj_t *label;
    char line1[160];
    char line2[160];
    char line3[240];
    uint32_t accent = mesh_status_is_online(mesh_status_text) ? 0x25C281 :
                      0xF5A524;

    mesh_settings_status_summary(mesh_status_text, line1, sizeof(line1),
                                 line2, sizeof(line2), line3,
                                 sizeof(line3));
    card = ui_panel(parent, x, y, w, h);
    mesh_settings_status_card = card;
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label = ui_label(card, line1, &lv_font_montserrat_18, accent);
    mesh_settings_status_labels[0] = label;
    lv_obj_set_pos(label, 14, 12);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, line2, &lv_font_montserrat_14, 0xCBD5E1);
    mesh_settings_status_labels[1] = label;
    lv_obj_set_pos(label, 14, 40);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    label = ui_label(card, line3, &lv_font_montserrat_14, 0x94A3B8);
    mesh_settings_status_labels[2] = label;
    lv_obj_set_pos(label, 14, 64);
    lv_obj_set_width(label, w - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static void mesh_log_close(void)
{
    if(mesh_log_overlay && lv_obj_is_valid(mesh_log_overlay)) {
        lv_obj_delete(mesh_log_overlay);
    }
    mesh_log_overlay = NULL;
    mesh_log_label = NULL;
}

static void mesh_log_close_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_log_close();
}

static void mesh_log_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_refresh_daemon_log();
}

static void mesh_log_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;

    (void)event;
    mesh_log_close();
    mesh_log_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_log_overlay);
    lv_obj_set_style_bg_color(mesh_log_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_log_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_log_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_log_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_log_overlay, 0, 0);
    lv_obj_clear_flag(mesh_log_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_log_overlay);

    panel = ui_scroll_panel(mesh_log_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Event log"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 216);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                            ui_tr("Refresh"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_log_refresh_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_log_close_event_cb, LV_EVENT_CLICKED, NULL);

    mesh_log_label = ui_label(panel,
                              mesh_log_text[0] ? mesh_log_text : ui_tr("Ready"),
                              &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(mesh_log_label, margin, 92);
    lv_obj_set_width(mesh_log_label, content_w);
    lv_label_set_long_mode(mesh_log_label, LV_LABEL_LONG_WRAP);
    mesh_refresh_daemon_log();
}

static void mesh_profile_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *section;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int row_h = 62;
    int value_x = ui_is_landscape() ? 230 : 176;
    int edit_w = ui_is_landscape() ? 92 : 86;
    int value_w = content_w - value_x - edit_w - 18;
    int button_w;
    int button_gap = 10;
    int button_cols = ui_is_landscape() ? 4 : 2;
    int button_row_h = 50;
    int button_rows = 4;
    int status_h = ui_is_landscape() ? 96 : 104;
    int publish_w;
    int y = 0;

    (void)event;
    ui_input_hide_inline_active();
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
    mesh_settings_status_card = NULL;
    memset(mesh_settings_status_labels, 0, sizeof(mesh_settings_status_labels));

    mesh_settings_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_settings_overlay);
    lv_obj_set_style_bg_color(mesh_settings_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_settings_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_settings_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_settings_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_settings_overlay, 0, 0);
    lv_obj_clear_flag(mesh_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_settings_overlay);

    panel = ui_scroll_panel(mesh_settings_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, "Meshtastic", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    subtitle = ui_label(panel,
                        ui_tr("Mesh radio, channel, ACK and node settings"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 120);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_settings_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    y = 98;
    mesh_settings_add_status_card(panel, margin, y, content_w, status_h);
    y += status_h + 16;

    section = ui_label(panel, ui_tr("Mesh control"), &lv_font_montserrat_18,
                       0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    y += 32;
    button_w = (content_w - button_gap * (button_cols - 1)) / button_cols;
    if(button_w < 86) {
        button_w = 86;
    }
    button_rows = (8 + button_cols - 1) / button_cols;
#define MESH_CONN_BUTTON(index, label, color, cb) \
    do { \
        int bx = margin + ((index) % button_cols) * (button_w + button_gap); \
        int by = y + ((index) / button_cols) * button_row_h; \
        btn = ui_command_button(panel, bx, by, button_w, label, color); \
        lv_obj_set_height(btn, button_row_h - 6); \
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL); \
    } while(0)

    MESH_CONN_BUTTON(0, ui_tr("Start"), 0x25C281, mesh_start_event_cb);
    MESH_CONN_BUTTON(1, ui_tr("Stop"), 0xEF4D5A, mesh_stop_event_cb);
    MESH_CONN_BUTTON(2, ui_tr("Refresh"), 0x3DA5FF, mesh_refresh_event_cb);
    MESH_CONN_BUTTON(3, ui_tr("Log"), 0x64748B, mesh_log_event_cb);
    MESH_CONN_BUTTON(4, ui_tr("Detector"), 0xF59E0B, mesh_detector_event_cb);
    MESH_CONN_BUTTON(5, ui_tr("Channel"), 0xA78BFA, mesh_channel_event_cb);
    MESH_CONN_BUTTON(6, ui_tr("Profiles"), 0xF59E0B, mesh_channel_profiles_event_cb);
    MESH_CONN_BUTTON(7, ui_tr("Slots"), 0xEC4899, mesh_channels_event_cb);
#undef MESH_CONN_BUTTON

    y += button_rows * button_row_h + 18;
    section = ui_label(panel, ui_tr("Publish now"), &lv_font_montserrat_18,
                       0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    y += 34;
    publish_w = (content_w - button_gap * 2) / 3;
    if(publish_w < 112) {
        publish_w = 112;
    }
    btn = ui_command_button(panel, margin, y, publish_w,
                            ui_tr("Node info"), 0x25C281);
    lv_obj_add_event_cb(btn, mesh_publish_event_cb, LV_EVENT_CLICKED,
                        "PUBLISH_NODEINFO\n");
    btn = ui_command_button(panel, margin + publish_w + button_gap, y,
                            publish_w, ui_tr("Position"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_publish_event_cb, LV_EVENT_CLICKED,
                        "PUBLISH_POSITION\n");
    btn = ui_command_button(panel, margin + (publish_w + button_gap) * 2, y,
                            publish_w, ui_tr("Telemetry"), 0xA78BFA);
    lv_obj_add_event_cb(btn, mesh_publish_event_cb, LV_EVENT_CLICKED,
                        "PUBLISH_TELEMETRY\n");

    y += button_row_h + 10;
    mesh_publish_status_label = ui_label(panel, ui_tr("Ready"),
                                         &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(mesh_publish_status_label, margin, y);
    lv_obj_set_width(mesh_publish_status_label, content_w);
    lv_label_set_long_mode(mesh_publish_status_label, LV_LABEL_LONG_DOT);

    y += 38;
    section = ui_label(panel, ui_tr("Radio profile"), &lv_font_montserrat_18,
                       0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    y += 42;

    for(int i = 0; i < (int)MESH_FIELD_COUNT; i++) {
        char value[32];
        lv_obj_t *name = ui_label(panel,
                                  ui_tr(mesh_setting_name((mesh_setting_field_t)i)),
                                  &lv_font_montserrat_16, 0x9AA4AF);
        lv_obj_t *edit;
        lv_obj_set_pos(name, margin, y + 8);
        lv_obj_set_width(name, value_x - 10);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        mesh_settings_value_labels[i] =
            ui_label(panel,
                     mesh_setting_value((mesh_setting_field_t)i, value,
                                        sizeof(value)),
                     &lv_font_montserrat_18, 0xF2F5F8);
        lv_obj_set_pos(mesh_settings_value_labels[i], margin + value_x, y + 6);
        lv_obj_set_width(mesh_settings_value_labels[i],
                         value_w > 120 ? value_w : 120);
        lv_label_set_long_mode(mesh_settings_value_labels[i],
                               LV_LABEL_LONG_DOT);
        edit = ui_command_button(panel, screen_w - margin - edit_w, y - 2,
                                 edit_w,
                                 mesh_setting_uses_choice(
                                     (mesh_setting_field_t)i) ?
                                 ui_tr("Select") : ui_tr("Edit"),
                                 mesh_setting_uses_choice(
                                     (mesh_setting_field_t)i) ?
                                 0x25C281 : 0x3DA5FF);
        lv_obj_add_event_cb(edit, mesh_setting_edit_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        y += row_h;
    }

    mesh_node_detail_append_spacer(panel, y + 48);

    mesh_settings_refresh();
}

static void mesh_nodes_event_cb(lv_event_t *event)
{
    char response[8192];
    char nodes_text[8192];
    char map_status[512];
    mesh_detector_node_t nodes[MESHTASTIC_UI_NODE_SELECT_MAX];
    char *saveptr = NULL;
    char *line;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int columns = landscape ? 2 : 1;
    int gap = 12;
    int card_w = columns == 2 ? (content_w - gap) / 2 : content_w;
    int card_h = landscape ? 178 : 190;
    int y = 98;
    int node_index = 1;
    int shown_count = 0;
    int db_count = 0;
    int recent_count = 0;
    int stale_count = 0;
    char subtitle_text[192];
    char empty_text[192];
    int32_t old_scroll_y =
        mesh_nodes_preserve_scroll ? mesh_overlay_scroll_y(mesh_nodes_panel) :
        0;

    (void)event;
    ui_input_hide_inline_active();
    if(mesh_fetch_legacy_nodes(nodes_text, sizeof(nodes_text),
                               response, sizeof(response),
                               map_status, sizeof(map_status)) != 0) {
        ui_trim_text(response);
        mesh_append_log("nodes failed: %s", response);
        return;
    }

    line = strtok_r(nodes_text, "\n", &saveptr);
    while(line) {
        if(strncmp(line, "0x", 2) == 0) {
            mesh_detector_node_t parsed;
            mesh_detector_node_prepare(&parsed, line);
            db_count++;
            if(parsed.age_s <= MESHTASTIC_NODE_RECENT_WINDOW_S) {
                recent_count++;
                if(shown_count < MESHTASTIC_UI_NODE_SELECT_MAX - 1) {
                    nodes[shown_count++] = parsed;
                }
            } else {
                stale_count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    mesh_detector_sort_nodes(nodes, shown_count);
    if(map_status[0]) {
        char active_5m_text[16];
        char active_15m_text[16];
        char stale_text[16];
        mesh_status_field(map_status, "active_5m", active_5m_text,
                          sizeof(active_5m_text), "-");
        mesh_status_field(map_status, "active_15m", active_15m_text,
                          sizeof(active_15m_text), "-");
        mesh_status_field(map_status, "stale", stale_text,
                          sizeof(stale_text), "-");
        snprintf(subtitle_text, sizeof(subtitle_text),
                 "%s  5m %s / 15m %s / db %d / stale %s",
                 ui_tr("Recently active mesh nodes; tap one for direct messages"),
                 active_5m_text, active_15m_text, db_count, stale_text);
    } else {
        snprintf(subtitle_text, sizeof(subtitle_text), "%s  15m %d / db %d",
                 ui_tr("Recently active mesh nodes; tap one for direct messages"),
                 recent_count, db_count);
    }

    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_panel = NULL;
    mesh_nodes_overlay_kind = MESH_NODES_OVERLAY_LIST;
    mesh_nodes_overlay = lv_obj_create(lv_screen_active());
    ui_set_fullscreen(mesh_nodes_overlay);
    lv_obj_set_style_bg_color(mesh_nodes_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_nodes_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_nodes_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_nodes_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_nodes_overlay, 0, 0);
    lv_obj_clear_flag(mesh_nodes_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_nodes_overlay);

    panel = ui_scroll_panel(mesh_nodes_overlay, 0, 0, screen_w, screen_h);
    mesh_nodes_panel = panel;
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    title = ui_label(panel, ui_tr("Nearby nodes"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 230);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel, subtitle_text,
                        &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    snprintf(mesh_node_select_ids[0], sizeof(mesh_node_select_ids[0]),
             "0xffffffff");
    btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                            ui_tr("Broadcast"), 0x25C281);
    lv_obj_add_event_cb(btn, mesh_select_node_target_event_cb,
                        LV_EVENT_CLICKED, mesh_node_select_ids[0]);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"),
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_nodes_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    for(int i = 0; i < shown_count && node_index < MESHTASTIC_UI_NODE_SELECT_MAX;
        i++) {
        int col = i % columns;
        int row = i / columns;
        int x = margin + col * (card_w + gap);
        int card_y = y + row * (card_h + gap);
        mesh_add_node_card(panel, nodes[i].line, x, card_y, card_w, card_h,
                           (size_t)node_index);
        node_index++;
    }

    if(shown_count == 0) {
        snprintf(empty_text, sizeof(empty_text), "%s\n15m 0 / db %d / stale %d",
                 ui_tr("No recently active mesh nodes"), db_count,
                 stale_count);
        label = ui_label(panel, empty_text, &lv_font_montserrat_18,
                         0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 12);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    mesh_nodes_auto_refresh_ticks = MESHTASTIC_OVERLAY_AUTO_REFRESH_TICKS;
    mesh_overlay_restore_scroll(mesh_nodes_panel, old_scroll_y);
}

static void mesh_send_text_now(const char *text, const char *source,
                               int clear_textarea)
{
    char clean[256];
    char command[320];
    char response[256];
    int ret;

    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    ui_trim_text(clean);
    mesh_ui_trace("SEND_%s len=%u active_inline=%d text=%.80s",
                  source ? source : "unknown", (unsigned)strlen(clean),
                  ui_input_inline_is_active(mesh_inline_input), clean);

    if(!clean[0]) {
        mesh_append_log("send skipped: empty message");
        mesh_ui_trace("SEND_SKIP source=%s reason=empty",
                      source ? source : "unknown");
        return;
    }

    if(mesh_to_text_is_broadcast(mesh_to_node)) {
        snprintf(command, sizeof(command), "SEND_CHANNEL %d %.220s\n",
                 mesh_tx_channel_index, clean);
    } else {
        snprintf(command, sizeof(command), "%s %s %.220s\n",
                 mesh_ack_enabled ? "SEND_TO_ACK" : "SEND_TO",
                 mesh_to_node, clean);
    }
    ret = mesh_ipc_command(command, response, sizeof(response));
    ui_trim_text(response);
    mesh_ui_trace("SEND_RESPONSE source=%s ret=%d response=%s",
                  source ? source : "unknown", ret, response);
    if(ret == 0) {
        mesh_append_log("send: %s", response);
    } else {
        mesh_append_log("send failed: %s", response);
    }
    if(clear_textarea && mesh_textarea && lv_obj_is_valid(mesh_textarea)) {
        lv_textarea_set_text(mesh_textarea, "");
        lv_obj_add_state(mesh_textarea, LV_STATE_FOCUSED);
    }
    mesh_refresh_status();
    app_request_fast_refresh();
}

static void mesh_voice_set_button_text(const char *text)
{
    lv_obj_t *label;

    if(!mesh_voice_button || !lv_obj_is_valid(mesh_voice_button)) {
        return;
    }
    label = lv_obj_get_child(mesh_voice_button, 0);
    if(label && lv_obj_is_valid(label)) {
        lv_label_set_text(label, ui_tr(text));
    }
}

static void mesh_voice_record_overlay_close(void)
{
    if(mesh_voice_record_timer) {
        lv_timer_delete(mesh_voice_record_timer);
        mesh_voice_record_timer = NULL;
    }
    if(mesh_voice_record_overlay && lv_obj_is_valid(mesh_voice_record_overlay)) {
        lv_obj_delete(mesh_voice_record_overlay);
    }
    mesh_voice_record_overlay = NULL;
    mesh_voice_record_time_label = NULL;
    mesh_voice_record_level_label = NULL;
    mesh_voice_record_phase = 0;
}

static void mesh_voice_record_overlay_timer_cb(lv_timer_t *timer)
{
    const char *levels[] = { "|    ", "|||  ", "|||||", " ||| ", "  |  " };
    char text[80];
    uint64_t elapsed_ms;

    (void)timer;
    if(!mesh_voice_record_overlay ||
       !lv_obj_is_valid(mesh_voice_record_overlay)) {
        mesh_voice_record_overlay_close();
        return;
    }
    elapsed_ms = (ui_monotonic_us() - mesh_voice_record_start_us) / 1000ULL;
    if(mesh_voice_record_time_label &&
       lv_obj_is_valid(mesh_voice_record_time_label)) {
        snprintf(text, sizeof(text), "%s  %llu.%01llus",
                 ui_tr("Recording"),
                 (unsigned long long)(elapsed_ms / 1000ULL),
                 (unsigned long long)((elapsed_ms / 100ULL) % 10ULL));
        lv_label_set_text(mesh_voice_record_time_label, text);
    }
    if(mesh_voice_record_level_label &&
       lv_obj_is_valid(mesh_voice_record_level_label)) {
        lv_label_set_text(mesh_voice_record_level_label,
                          levels[mesh_voice_record_phase %
                                 (sizeof(levels) / sizeof(levels[0]))]);
    }
    mesh_voice_record_phase++;
    app_request_fast_refresh();
}

static void mesh_voice_record_overlay_open(const char *source)
{
    lv_obj_t *card;
    lv_obj_t *spinner;
    lv_obj_t *mic;
    lv_obj_t *hint;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int card_w = ui_is_landscape() ? 310 : 286;
    int card_h = ui_is_landscape() ? 224 : 252;
    int spinner_size = ui_is_landscape() ? 102 : 112;

    (void)source;
    mesh_voice_record_overlay_close();
    if(card_w > screen_w - 40) {
        card_w = screen_w - 40;
    }
    if(card_h > screen_h - 40) {
        card_h = screen_h - 40;
    }

    mesh_voice_record_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_voice_record_overlay);
    lv_obj_set_style_bg_color(mesh_voice_record_overlay, lv_color_hex(0x000000),
                              0);
    lv_obj_set_style_bg_opa(mesh_voice_record_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mesh_voice_record_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_voice_record_overlay, 0, 0);
    lv_obj_add_flag(mesh_voice_record_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mesh_voice_record_overlay, LV_OBJ_FLAG_SCROLLABLE);

    card = ui_panel(mesh_voice_record_overlay, 0, 0, card_w, card_h);
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

    mesh_voice_record_time_label =
        ui_label(card, ui_tr("Recording"), &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_width(mesh_voice_record_time_label, card_w - 36);
    lv_obj_set_style_text_align(mesh_voice_record_time_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(mesh_voice_record_time_label, LV_LABEL_LONG_DOT);
    lv_obj_align(mesh_voice_record_time_label, LV_ALIGN_TOP_MID, 0,
                 34 + spinner_size);

    mesh_voice_record_level_label =
        ui_label(card, "|||||", &lv_font_montserrat_24, 0x25C281);
    lv_obj_set_width(mesh_voice_record_level_label, card_w - 36);
    lv_obj_set_style_text_align(mesh_voice_record_level_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mesh_voice_record_level_label, LV_ALIGN_TOP_MID, 0,
                 64 + spinner_size);

    hint = ui_label(card, ui_tr("Release to preview"),
                    &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_width(hint, card_w - 36);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    mesh_voice_record_timer =
        lv_timer_create(mesh_voice_record_overlay_timer_cb, 120, NULL);
    mesh_voice_record_overlay_timer_cb(mesh_voice_record_timer);
    app_request_fast_refresh();
}

static void mesh_voice_preview_close(void)
{
    if(mesh_voice_preview_overlay && lv_obj_is_valid(mesh_voice_preview_overlay)) {
        lv_obj_delete(mesh_voice_preview_overlay);
    }
    mesh_voice_preview_overlay = NULL;
    mesh_voice_preview_status_label = NULL;
}

static int mesh_voice_send_recorded(const char *source, char *response,
                                    size_t response_len)
{
    char command[320];
    int ret;

    if(response && response_len > 0U) {
        response[0] = '\0';
    }
    snprintf(command, sizeof(command), "SEND_VOICE_FILE %s\n",
             MESHTASTIC_VOICE_RAW_PATH);
    ret = mesh_ipc_command(command, response, response_len);
    if(response && response_len > 0U) {
        ui_trim_text(response);
    }
    mesh_ui_trace("VOICE_RESPONSE source=%s ret=%d response=%s",
                  source ? source : "unknown", ret,
                  response ? response : "");
    if(ret == 0) {
        mesh_append_log("voice send: %s", response ? response : "OK");
    } else {
        mesh_append_log("voice send failed: %s",
                        response && response[0] ? response : "no response");
    }
    mesh_refresh_status();
    mesh_refresh_chat_common(1, 0);
    app_request_fast_refresh();
    return ret;
}

static void mesh_voice_preview_play_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_voice_play_pcm_file(MESHTASTIC_VOICE_RAW_PATH);
}

static void mesh_voice_preview_cancel_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    unlink(MESHTASTIC_VOICE_RAW_PATH);
    mesh_voice_preview_close();
}

static void mesh_voice_preview_send_event_cb(lv_event_t *event)
{
    char response[256];
    const char *message;
    int ret;

    if(event) {
        lv_event_stop_processing(event);
    }
    if(mesh_voice_preview_status_label && lv_obj_is_valid(mesh_voice_preview_status_label)) {
        lv_label_set_text(mesh_voice_preview_status_label, ui_tr("Sending voice"));
    }
    ret = mesh_voice_send_recorded("PREVIEW", response, sizeof(response));
    if(ret == 0) {
        mesh_voice_preview_close();
    } else if(mesh_voice_preview_status_label &&
              lv_obj_is_valid(mesh_voice_preview_status_label)) {
        message = response[0] ? response : ui_tr("Voice send failed");
        if(strstr(message, "voice-airtime") || strstr(message, "channel-busy") ||
           strstr(message, "channel-budget")) {
            message = ui_tr("Channel is busy, try later");
        } else if(strstr(message, "duty-cycle") ||
                  strstr(message, "duty-budget")) {
            message = ui_tr("Radio duty limit, try later");
        } else if(strstr(message, "chunk count") || strstr(message, "queue")) {
            message = ui_tr("Voice is too long or radio is busy");
        }
        lv_label_set_text(mesh_voice_preview_status_label,
                          message);
    }
}

static void mesh_voice_preview_open(size_t bytes, unsigned duration_ms)
{
    lv_obj_t *dialog;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *hint;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int dialog_w = ui_is_landscape() ? 560 : 500;
    int dialog_h = ui_is_landscape() ? 260 : 300;
    int pad = 22;
    int gap = 14;
    int button_w;
    int button_y;
    char text[160];

    mesh_voice_preview_close();
    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 36) {
        dialog_h = screen_h - 36;
    }
    if(dialog_h < 228) {
        dialog_h = 228;
    }
    button_w = (dialog_w - pad * 2 - gap * 2) / 3;
    button_y = dialog_h - pad - 58;

    mesh_voice_preview_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_voice_preview_overlay);
    lv_obj_set_style_bg_color(mesh_voice_preview_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_voice_preview_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mesh_voice_preview_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_voice_preview_overlay, 0, 0);
    lv_obj_clear_flag(mesh_voice_preview_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(mesh_voice_preview_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x253B31), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    title = ui_label(dialog, ui_tr("Voice preview"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, pad, pad);
    lv_obj_set_width(title, dialog_w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    snprintf(text, sizeof(text), "%s %.1fs  %u bytes",
             ui_tr("Voice message"), (double)duration_ms / 1000.0,
             (unsigned)bytes);
    detail = ui_label(dialog, text, &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_pos(detail, pad, pad + 48);
    lv_obj_set_width(detail, dialog_w - pad * 2);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);

    hint = ui_label(dialog, ui_tr("Play it before sending, or cancel."),
                    &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(hint, pad, pad + 86);
    lv_obj_set_width(hint, dialog_w - pad * 2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    mesh_voice_preview_status_label =
        ui_label(dialog, ui_tr("Ready"), &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(mesh_voice_preview_status_label, pad, button_y - 34);
    lv_obj_set_width(mesh_voice_preview_status_label, dialog_w - pad * 2);
    lv_label_set_long_mode(mesh_voice_preview_status_label, LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, button_y, button_w,
                            ui_tr("Play"), 0x3DA5FF);
    lv_obj_set_height(btn, 58);
    lv_obj_add_event_cb(btn, mesh_voice_preview_play_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + button_w + gap, button_y, button_w,
                            ui_tr("Cancel"), 0x9AA4AF);
    lv_obj_set_height(btn, 58);
    lv_obj_add_event_cb(btn, mesh_voice_preview_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + (button_w + gap) * 2, button_y,
                            button_w, ui_tr("Send"), 0x25C281);
    lv_obj_set_height(btn, 58);
    lv_obj_add_event_cb(btn, mesh_voice_preview_send_event_cb,
                        LV_EVENT_CLICKED, NULL);
    app_request_fast_refresh();
}

static int mesh_photo_read_stored_thumb(const char *path)
{
    FILE *fp;
    size_t total = 0U;

    if(!path || !path[0]) {
        return -1;
    }
    fp = fopen(path, "rb");
    if(!fp) {
        return -1;
    }
    while(total < MESHTASTIC_PHOTO_STORED_THUMB_BYTES) {
        size_t n = fread(mesh_photo_stored_thumb_buf + total, 1U,
                         MESHTASTIC_PHOTO_STORED_THUMB_BYTES - total, fp);
        if(n == 0U) {
            break;
        }
        total += n;
    }
    fclose(fp);
    return total == MESHTASTIC_PHOTO_STORED_THUMB_BYTES ? 0 : -1;
}

static void mesh_photo_fill_rgb565(uint8_t *dst, unsigned width,
                                   unsigned height, uint32_t color)
{
    uint16_t rgb565 = mesh_rgb565(color);

    for(unsigned y = 0U; y < height; y++) {
        for(unsigned x = 0U; x < width; x++) {
            size_t off = ((size_t)y * width + x) * 2U;

            dst[off + 0U] = (uint8_t)(rgb565 & 0xffU);
            dst[off + 1U] = (uint8_t)(rgb565 >> 8U);
        }
    }
}

static void mesh_photo_fill_placeholder(uint8_t *dst, unsigned width,
                                        unsigned height)
{
    uint16_t bg = mesh_rgb565(0x0F172A);
    uint16_t line = mesh_rgb565(0x334155);

    for(unsigned y = 0U; y < height; y++) {
        for(unsigned x = 0U; x < width; x++) {
            size_t off = ((size_t)y * width + x) * 2U;
            uint16_t color = ((x / 12U + y / 12U) % 2U) ? bg : line;

            dst[off + 0U] = (uint8_t)(color & 0xffU);
            dst[off + 1U] = (uint8_t)(color >> 8U);
        }
    }
}

static void mesh_photo_scale_thumb_cover(uint8_t *dst, unsigned dst_w,
                                         unsigned dst_h)
{
    for(unsigned y = 0U; y < dst_h; y++) {
        unsigned src_y =
            (unsigned)(((uint64_t)y * MESHTASTIC_PHOTO_STORED_THUMB_H) /
                       dst_h);

        if(src_y >= MESHTASTIC_PHOTO_STORED_THUMB_H) {
            src_y = MESHTASTIC_PHOTO_STORED_THUMB_H - 1U;
        }
        for(unsigned x = 0U; x < dst_w; x++) {
            unsigned src_x =
                (unsigned)(((uint64_t)x * MESHTASTIC_PHOTO_STORED_THUMB_W) /
                           dst_w);
            size_t dst_off = ((size_t)y * dst_w + x) * 2U;
            size_t src_off;

            if(src_x >= MESHTASTIC_PHOTO_STORED_THUMB_W) {
                src_x = MESHTASTIC_PHOTO_STORED_THUMB_W - 1U;
            }
            src_off = ((size_t)src_y * MESHTASTIC_PHOTO_STORED_THUMB_W +
                       src_x) * 2U;
            dst[dst_off + 0U] = mesh_photo_stored_thumb_buf[src_off + 0U];
            dst[dst_off + 1U] = mesh_photo_stored_thumb_buf[src_off + 1U];
        }
    }
}

static void mesh_photo_apply_thumb(lv_obj_t *canvas, const char *thumb_path,
                                   uint8_t *buf, unsigned width,
                                   unsigned height)
{
    if(!canvas || !buf) {
        return;
    }
    if(mesh_photo_read_stored_thumb(thumb_path) == 0) {
        mesh_photo_scale_thumb_cover(buf, width, height);
    } else {
        mesh_photo_fill_placeholder(buf, width, height);
    }
    lv_canvas_set_buffer(canvas, buf, width, height,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
}

static int mesh_photo_picker_scan(void)
{
    DIR *dir = opendir(MESHTASTIC_PHOTO_DIR);

    mesh_photo_item_count = 0;
    if(!dir) {
        return 0;
    }
    while(mesh_photo_item_count < MESHTASTIC_PHOTO_MAX_ITEMS) {
        struct dirent *ent = readdir(dir);
        char path[192];
        char thumb_path[192];
        struct stat st;
        size_t len;
        int insert_at;

        if(!ent) {
            break;
        }
        len = strlen(ent->d_name);
        if(len < 5U || strcmp(ent->d_name + len - 4U, ".ppm") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", MESHTASTIC_PHOTO_DIR,
                 ent->d_name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        snprintf(thumb_path, sizeof(thumb_path), "%s/%.*s.thumb.rgb565",
                 MESHTASTIC_PHOTO_DIR, (int)(len - 4U), ent->d_name);
        insert_at = mesh_photo_item_count;
        while(insert_at > 0 &&
              mesh_photo_items[insert_at - 1].mtime < st.st_mtime) {
            mesh_photo_items[insert_at] = mesh_photo_items[insert_at - 1];
            insert_at--;
        }
        snprintf(mesh_photo_items[insert_at].photo,
                 sizeof(mesh_photo_items[insert_at].photo), "%s", path);
        snprintf(mesh_photo_items[insert_at].thumb,
                 sizeof(mesh_photo_items[insert_at].thumb), "%s",
                 thumb_path);
        mesh_photo_items[insert_at].mtime = st.st_mtime;
        mesh_photo_item_count++;
    }
    closedir(dir);
    return mesh_photo_item_count;
}

static void mesh_photo_picker_close(void)
{
    if(mesh_photo_picker_close_timer) {
        lv_timer_delete(mesh_photo_picker_close_timer);
        mesh_photo_picker_close_timer = NULL;
    }
    if(mesh_photo_picker_overlay &&
       lv_obj_is_valid(mesh_photo_picker_overlay)) {
        lv_obj_delete(mesh_photo_picker_overlay);
    }
    mesh_photo_picker_overlay = NULL;
    mesh_photo_status_label = NULL;
    mesh_photo_send_inflight = 0;
}

static void mesh_photo_picker_close_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    mesh_photo_picker_close_timer = NULL;
    mesh_photo_picker_close();
    mesh_refresh_status();
    mesh_refresh_chat_common(1, 0);
    app_request_fast_refresh();
}

static void mesh_photo_picker_close_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_photo_picker_close();
}

static void mesh_photo_send_path(const char *path)
{
    char command[320];
    char response[256];
    int ret;

    if(!path || !path[0]) {
        return;
    }
    if(mesh_photo_send_inflight) {
        if(mesh_photo_status_label && lv_obj_is_valid(mesh_photo_status_label)) {
            lv_label_set_text(mesh_photo_status_label,
                              ui_tr("Photo already queued"));
            lv_obj_set_style_text_color(mesh_photo_status_label,
                                        lv_color_hex(0xF5A524), 0);
        }
        app_request_fast_refresh();
        return;
    }
    mesh_photo_send_inflight = 1;
    snprintf(command, sizeof(command), "SEND_PHOTO_FILE %s\n", path);
    if(mesh_photo_status_label && lv_obj_is_valid(mesh_photo_status_label)) {
        lv_label_set_text(mesh_photo_status_label, ui_tr("Sending photo"));
        lv_obj_set_style_text_color(mesh_photo_status_label,
                                    lv_color_hex(0x25C281), 0);
    }
    ret = mesh_ipc_command(command, response, sizeof(response));
    ui_trim_text(response);
    if(ret == 0) {
        mesh_append_log("photo send: %s", response);
        if(mesh_photo_status_label &&
           lv_obj_is_valid(mesh_photo_status_label)) {
            lv_label_set_text(mesh_photo_status_label, ui_tr("Photo queued"));
            lv_obj_set_style_text_color(mesh_photo_status_label,
                                        lv_color_hex(0x25C281), 0);
        }
        if(mesh_photo_picker_close_timer) {
            lv_timer_delete(mesh_photo_picker_close_timer);
        }
        mesh_photo_picker_close_timer =
            lv_timer_create(mesh_photo_picker_close_timer_cb, 700, NULL);
        lv_timer_set_repeat_count(mesh_photo_picker_close_timer, 1);
        mesh_refresh_status();
        mesh_refresh_chat_common(1, 0);
    } else {
        const char *message = response[0] ? response :
                              ui_tr("Photo send failed");

        if(strstr(message, "photo-too-large") ||
           strstr(message, "photo-encode")) {
            message = ui_tr("Photo is too large or cannot be encoded");
        } else if(strstr(message, "requires-lr2021")) {
            message = ui_tr("LR2021 FLRC is required for photo messages");
        } else if(strstr(message, "queue")) {
            message = ui_tr("Radio is busy, try later");
        }
        mesh_append_log("photo send failed: %s", response);
        if(mesh_photo_status_label &&
           lv_obj_is_valid(mesh_photo_status_label)) {
            lv_label_set_text(mesh_photo_status_label, message);
            lv_obj_set_style_text_color(mesh_photo_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
        mesh_photo_send_inflight = 0;
    }
    app_request_fast_refresh();
}

static void mesh_photo_tile_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    if(index < 0 || index >= mesh_photo_item_count) {
        return;
    }
    mesh_photo_send_path(mesh_photo_items[index].photo);
}

static void mesh_photo_picker_open(void)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int columns = ui_is_landscape() ? 4 : 2;
    int gap = 12;
    int tile_w = (content_w - gap * (columns - 1)) / columns;
    int tile_h = 166;
    int y = 84;
    int count;

    if(!mesh_photo_available) {
        mesh_append_log("photo unavailable: LR2021 FLRC required");
        return;
    }
    mesh_photo_picker_close();
    count = mesh_photo_picker_scan();
    mesh_photo_picker_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_photo_picker_overlay);
    lv_obj_set_style_bg_color(mesh_photo_picker_overlay,
                              lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_photo_picker_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mesh_photo_picker_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_photo_picker_overlay, 0, 0);
    lv_obj_clear_flag(mesh_photo_picker_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_scroll_panel(mesh_photo_picker_overlay, 0, 0,
                            screen_w, screen_h);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Select photo"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 96);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 82, 16, 82,
                            ui_tr("Close"), 0x374151);
    lv_obj_set_height(btn, 48);
    lv_obj_add_event_cb(btn, mesh_photo_picker_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_photo_status_label =
        ui_label(panel, count > 0 ? ui_tr("Tap a photo to send") :
                 ui_tr("No photos"),
                 &lv_font_montserrat_14, count > 0 ? 0x94A3B8 : 0xF5A524);
    lv_obj_set_pos(mesh_photo_status_label, margin, 56);
    lv_obj_set_width(mesh_photo_status_label, content_w);
    lv_label_set_long_mode(mesh_photo_status_label, LV_LABEL_LONG_DOT);

    for(int i = 0; i < count; i++) {
        int col = i % columns;
        int row = i / columns;
        int x = margin + col * (tile_w + gap);
        int ty = y + row * (tile_h + gap);
        lv_obj_t *tile = ui_panel(panel, x, ty, tile_w, tile_h);
        lv_obj_t *canvas;
        lv_obj_t *caption;
        const char *base = strrchr(mesh_photo_items[i].photo, '/');

        lv_obj_set_style_bg_color(tile, lv_color_hex(0x101820), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x223244), 0);
        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, mesh_photo_tile_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        canvas = lv_canvas_create(tile);
        lv_obj_set_pos(canvas, (tile_w - MESHTASTIC_PHOTO_THUMB_W) / 2, 4);
        lv_obj_set_size(canvas, MESHTASTIC_PHOTO_THUMB_W,
                        MESHTASTIC_PHOTO_THUMB_H);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE |
                        LV_OBJ_FLAG_EVENT_BUBBLE);
        mesh_photo_apply_thumb(canvas, mesh_photo_items[i].thumb,
                               mesh_photo_thumb_buf[i],
                               MESHTASTIC_PHOTO_THUMB_W,
                               MESHTASTIC_PHOTO_THUMB_H);

        caption = ui_label(tile,
                           base ? base + 1 : mesh_photo_items[i].photo,
                           &lv_font_montserrat_14, 0xD3DAE3);
        lv_obj_set_width(caption, tile_w - 16);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -6);
        ui_make_click_forwarder(caption);
    }
    app_request_fast_refresh();
}

static void mesh_photo_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_photo_picker_open();
}

static int mesh_voice_record_start(const char *source)
{
    pid_t pid;
    int logfd;

    if(!mesh_voice_available) {
        mesh_append_log("voice unavailable: LR2021 FLRC required");
        return -1;
    }
    if(mesh_voice_record_pid > 0) {
        return 0;
    }
    mesh_voice_preview_close();
    unlink(MESHTASTIC_VOICE_RAW_PATH);
    unlink(MESHTASTIC_VOICE_RECORD_LOG);
    mesh_voice_record_bytes = 0;
    mesh_voice_record_duration_ms = 0;
    mesh_voice_record_start_us = ui_monotonic_us();
    mesh_ui_trace("VOICE_%s record_start", source ? source : "unknown");
    mesh_append_log("voice: hold to record...");
    ui_audio_input_route_enter("meshtastic_voice");

    pid = fork();
    if(pid < 0) {
        ui_audio_input_route_leave("meshtastic_voice");
        mesh_append_log("voice record fork failed: %s", strerror(errno));
        mesh_ui_trace("VOICE_RECORD_FORK_FAILED err=%s", strerror(errno));
        return -1;
    }
    if(pid == 0) {
        setpgid(0, 0);
        logfd = open(MESHTASTIC_VOICE_RECORD_LOG,
                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }
        execlp("arecord", "arecord", "-q", "-D", "default", "-f", "S16_LE",
               "-c", "1", "-r", "8000", "-d", MESHTASTIC_VOICE_MAX_SECONDS,
               "-t", "raw", MESHTASTIC_VOICE_RAW_PATH, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    mesh_voice_record_pid = pid;
    mesh_voice_set_button_text("Recording");
    mesh_voice_record_overlay_open(source);
    app_request_fast_refresh();
    return 0;
}

static int mesh_voice_record_stop(int open_preview, const char *source)
{
    pid_t pid = mesh_voice_record_pid;
    int status = 0;
    struct stat st;

    if(pid <= 0) {
        return -1;
    }
    mesh_voice_record_pid = -1;
    if(waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGTERM);
        for(int i = 0; i < 12; i++) {
            usleep(25000);
            if(waitpid(pid, &status, WNOHANG) == pid) {
                break;
            }
        }
        if(waitpid(pid, &status, WNOHANG) == 0) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    ui_audio_input_route_leave("meshtastic_voice");
    mesh_voice_record_overlay_close();
    mesh_voice_set_button_text("Mic");
    mesh_ui_trace("VOICE_%s record_stop status=%d",
                  source ? source : "unknown", status);

    if(stat(MESHTASTIC_VOICE_RAW_PATH, &st) != 0 || st.st_size <= 0) {
        mesh_append_log("voice record failed or empty log=%s",
                        MESHTASTIC_VOICE_RECORD_LOG);
        return -1;
    }
    mesh_voice_record_bytes = (size_t)st.st_size;
    mesh_voice_record_duration_ms =
        (unsigned)((mesh_voice_record_bytes * 1000ULL) /
                   (MESHTASTIC_VOICE_SAMPLE_RATE *
                    MESHTASTIC_VOICE_SAMPLE_BYTES));
    if(mesh_voice_record_bytes < MESHTASTIC_VOICE_MIN_BYTES) {
        mesh_append_log("voice too short: %.1fs",
                        (double)mesh_voice_record_duration_ms / 1000.0);
        unlink(MESHTASTIC_VOICE_RAW_PATH);
        return -1;
    }
    mesh_append_log("voice ready: %.1fs",
                    (double)mesh_voice_record_duration_ms / 1000.0);
    if(open_preview) {
        mesh_voice_preview_open(mesh_voice_record_bytes,
                                mesh_voice_record_duration_ms);
    }
    return 0;
}

static void mesh_canned_close(void)
{
    if(mesh_canned_overlay && lv_obj_is_valid(mesh_canned_overlay)) {
        lv_obj_delete(mesh_canned_overlay);
    }
    mesh_canned_overlay = NULL;
}

static void mesh_canned_delete_confirm_close(void)
{
    if(mesh_canned_delete_overlay &&
       lv_obj_is_valid(mesh_canned_delete_overlay)) {
        lv_obj_delete(mesh_canned_delete_overlay);
    }
    mesh_canned_delete_overlay = NULL;
    mesh_canned_delete_index = -1;
}

static int mesh_canned_load(void)
{
    static const char *defaults[] = {
        "OK",
        "On my way",
        "Need help",
        "At location",
        "Battery low",
        "Signal check",
        "Please repeat",
        "Stand by",
    };
    FILE *fp;
    char line[192];
    int count = 0;

    memset(mesh_canned_messages, 0, sizeof(mesh_canned_messages));
    fp = fopen(MESHTASTIC_CANNED_FILE, "r");
    if(fp) {
        while(count < MESHTASTIC_CANNED_MAX &&
              fgets(line, sizeof(line), fp)) {
            ui_trim_text(line);
            if(!line[0] || line[0] == '#') {
                continue;
            }
            snprintf(mesh_canned_messages[count],
                     sizeof(mesh_canned_messages[count]), "%s", line);
            count++;
        }
        fclose(fp);
    }
    if(count == 0) {
        int n = (int)(sizeof(defaults) / sizeof(defaults[0]));

        if(n > MESHTASTIC_CANNED_MAX) {
            n = MESHTASTIC_CANNED_MAX;
        }
        for(int i = 0; i < n; i++) {
            snprintf(mesh_canned_messages[count],
                     sizeof(mesh_canned_messages[count]), "%s", defaults[i]);
            count++;
        }
    }
    return count;
}

static int mesh_canned_save(int count)
{
    char tmp_path[192];
    FILE *fp;

    if(count < 0) {
        count = 0;
    }
    if(count > MESHTASTIC_CANNED_MAX) {
        count = MESHTASTIC_CANNED_MAX;
    }
    if(mkdir(MESHTASTIC_CHANNEL_DIR, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MESHTASTIC_CANNED_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "# K230 Meshtastic canned messages\n");
    fprintf(fp, "# One message per line. Empty lines are ignored.\n");
    for(int i = 0; i < count && i < MESHTASTIC_CANNED_MAX; i++) {
        if(mesh_canned_messages[i][0]) {
            fprintf(fp, "%s\n", mesh_canned_messages[i]);
        }
    }
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if(rename(tmp_path, MESHTASTIC_CANNED_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static void mesh_canned_open(int manage_mode);

static void mesh_canned_send_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    if(index < 0 || index >= MESHTASTIC_CANNED_MAX ||
       !mesh_canned_messages[index][0]) {
        return;
    }
    mesh_canned_close();
    mesh_send_text_now(mesh_canned_messages[index], "CANNED", 0);
}

static void mesh_canned_edit_submit_cb(const char *text, void *user_data)
{
    char clean[160];
    int edit_index = (int)(intptr_t)user_data;
    int count;

    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    ui_trim_text(clean);
    if(!clean[0]) {
        mesh_append_log("canned message skipped: empty");
        return;
    }
    count = mesh_canned_load();
    if(edit_index >= 0 && edit_index < count) {
        snprintf(mesh_canned_messages[edit_index],
                 sizeof(mesh_canned_messages[edit_index]), "%s", clean);
    } else {
        if(count >= MESHTASTIC_CANNED_MAX) {
            mesh_append_log("canned message list full");
            return;
        }
        snprintf(mesh_canned_messages[count],
                 sizeof(mesh_canned_messages[count]), "%s", clean);
        count++;
    }
    if(mesh_canned_save(count) == 0) {
        mesh_append_log("canned messages saved");
    } else {
        mesh_append_log("canned messages save failed");
    }
    mesh_canned_open(1);
}

static void mesh_canned_edit_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;

    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_canned_load();
    memset(&config, 0, sizeof(config));
    config.title = ui_tr(index >= 0 ? "Edit message" : "Add message");
    config.placeholder = ui_tr("Message");
    config.initial_text = (index >= 0 && index < MESHTASTIC_CANNED_MAX) ?
                          mesh_canned_messages[index] : "";
    config.max_length = 140;
    config.min_length = 1;
    config.min_length_text = ui_tr("Message is empty");
    config.submit_cb = mesh_canned_edit_submit_cb;
    config.user_data = (void *)(intptr_t)index;
    config.submit_text = ui_tr("Save");
    config.cancel_text = ui_tr("Cancel");
    ui_input_dialog_open(&config);
}

static void mesh_canned_delete_accept_event_cb(lv_event_t *event)
{
    int index = mesh_canned_delete_index;
    int count;

    if(event) {
        lv_event_stop_processing(event);
    }
    count = mesh_canned_load();
    if(index < 0 || index >= count) {
        return;
    }
    for(int i = index; i + 1 < count; i++) {
        snprintf(mesh_canned_messages[i], sizeof(mesh_canned_messages[i]),
                 "%s", mesh_canned_messages[i + 1]);
    }
    mesh_canned_messages[count - 1][0] = '\0';
    if(mesh_canned_save(count - 1) == 0) {
        mesh_append_log("canned message deleted");
    } else {
        mesh_append_log("canned message delete failed");
    }
    mesh_canned_delete_confirm_close();
    mesh_canned_open(1);
}

static void mesh_canned_delete_cancel_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_canned_delete_confirm_close();
}

static void mesh_canned_delete_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *dialog;
    lv_obj_t *title;
    lv_obj_t *message;
    lv_obj_t *note;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int dialog_w = ui_is_landscape() ? 560 : 500;
    int dialog_h = ui_is_landscape() ? 236 : 260;
    int pad = 24;
    int gap = 18;
    int button_w;
    int button_y;
    int count;

    if(event) {
        lv_event_stop_processing(event);
    }
    count = mesh_canned_load();
    if(index < 0 || index >= count) {
        return;
    }
    mesh_canned_delete_confirm_close();
    mesh_canned_delete_index = index;
    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 48) {
        dialog_h = screen_h - 48;
    }
    if(dialog_h < 216) {
        dialog_h = 216;
    }
    button_w = (dialog_w - pad * 2 - gap) / 2;
    button_y = dialog_h - pad - 58;

    mesh_canned_delete_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_canned_delete_overlay);
    lv_obj_set_style_bg_color(mesh_canned_delete_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mesh_canned_delete_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mesh_canned_delete_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_canned_delete_overlay, 0, 0);
    lv_obj_clear_flag(mesh_canned_delete_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(mesh_canned_delete_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3A2630), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    title = ui_label(dialog, ui_tr("Delete message?"),
                     &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, pad, pad);
    lv_obj_set_width(title, dialog_w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    message = ui_label(dialog, mesh_canned_messages[index],
                       &lv_font_montserrat_18, 0xF5A524);
    lv_obj_set_pos(message, pad, pad + 52);
    lv_obj_set_width(message, dialog_w - pad * 2);
    lv_label_set_long_mode(message, LV_LABEL_LONG_DOT);

    note = ui_label(dialog, ui_tr("This cannot be undone."),
                    &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(note, pad, pad + 88);
    lv_obj_set_width(note, dialog_w - pad * 2);
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, button_y, button_w,
                            ui_tr("Cancel"), 0x9AA4AF);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3644), 0);
    lv_obj_add_event_cb(btn, mesh_canned_delete_cancel_event_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + button_w + gap, button_y,
                            button_w, ui_tr("Delete"), 0xEF4D5A);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1D24), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xEF4D5A), 0);
    lv_obj_add_event_cb(btn, mesh_canned_delete_accept_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void mesh_canned_manage_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_canned_open(1);
}

static void mesh_canned_close_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_canned_close();
}

static void mesh_canned_open(int manage_mode)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    lv_obj_t *empty;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int columns = ui_is_landscape() ? 3 : 2;
    int gap = 12;
    int row_h = 66;
    int y = 112;
    int count;
    int col_w;

    mesh_canned_manage_mode = manage_mode ? 1 : 0;
    ui_input_hide_inline_active();
    count = mesh_canned_load();
    if(columns < 1) {
        columns = 1;
    }
    col_w = (content_w - gap * (columns - 1)) / columns;
    if(col_w < 132) {
        columns = 1;
        col_w = content_w;
    }
    if(mesh_canned_manage_mode && col_w < 240) {
        columns = 1;
        col_w = content_w;
    }

    mesh_canned_close();
    mesh_canned_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(mesh_canned_overlay);
    lv_obj_set_style_bg_color(mesh_canned_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_canned_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mesh_canned_overlay, 0, 0);
    lv_obj_set_style_border_width(mesh_canned_overlay, 0, 0);
    lv_obj_set_style_pad_all(mesh_canned_overlay, 0, 0);
    lv_obj_clear_flag(mesh_canned_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(mesh_canned_overlay);

    panel = ui_scroll_panel(mesh_canned_overlay, 0, 0, screen_w, screen_h);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    title = ui_label(panel, ui_tr("Canned messages"),
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 120);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    subtitle = ui_label(panel, ui_tr("Tap to send a canned message"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 230);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    if(!mesh_canned_manage_mode) {
        btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                                ui_tr("Manage"), 0x3DA5FF);
        lv_obj_add_event_cb(btn, mesh_canned_manage_event_cb,
                            LV_EVENT_CLICKED, NULL);
    } else {
        btn = ui_command_button(panel, screen_w - margin - 206, 18, 100,
                                ui_tr("Add"), 0x25C281);
        lv_obj_add_event_cb(btn, mesh_canned_edit_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    }
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96,
                            ui_tr("Close"), 0x374151);
    lv_obj_add_event_cb(btn, mesh_canned_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    if(count <= 0) {
        empty = ui_label(panel, ui_tr("No canned messages"),
                         &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(empty, margin, y);
        lv_obj_set_width(empty, content_w);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        return;
    }

    for(int i = 0; i < count; i++) {
        int col = i % columns;
        int row = i / columns;
        int x = margin + col * (col_w + gap);
        int by = y + row * row_h;
        int action_w = mesh_canned_manage_mode ? 72 : 0;
        int label_w = mesh_canned_manage_mode ? col_w - action_w - gap : col_w;

        btn = ui_command_button(panel, x, by, label_w,
                                mesh_canned_messages[i], 0x25C281);
        lv_obj_set_height(btn, 54);
        lv_obj_add_event_cb(btn,
                            mesh_canned_manage_mode ?
                            mesh_canned_edit_event_cb :
                            mesh_canned_send_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if(mesh_canned_manage_mode) {
            btn = ui_command_button(panel, x + label_w + gap, by,
                                    action_w, ui_tr("Delete"), 0xEF4444);
            lv_obj_set_height(btn, 54);
            lv_obj_add_event_cb(btn, mesh_canned_delete_event_cb,
                                LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

static void mesh_canned_event_cb(lv_event_t *event)
{
    if(event) {
        lv_event_stop_processing(event);
    }
    mesh_canned_open(0);
}

static void mesh_voice_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if(code == LV_EVENT_PRESSED) {
        if(event) {
            lv_event_stop_processing(event);
        }
        (void)mesh_voice_record_start("BUTTON");
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(event) {
            lv_event_stop_processing(event);
        }
        (void)mesh_voice_record_stop(1, "BUTTON");
    }
}

void ui_meshtastic_trigger_voice_key(void)
{
    ui_meshtastic_handle_voice_key(1);
    ui_meshtastic_handle_voice_key(0);
}

void ui_meshtastic_handle_voice_key(int pressed)
{
    if(!app_current_page_is(PAGE_MESHTASTIC)) {
        return;
    }
    if(!mesh_voice_available) {
        return;
    }
    if(pressed) {
        (void)mesh_voice_record_start("MIC_KEY");
    } else {
        (void)mesh_voice_record_stop(1, "MIC_KEY");
    }
}

static void mesh_send_submit_cb(const char *text, void *user_data)
{
    (void)user_data;
    mesh_send_text_now(text, "SUBMIT", 0);
}

static void mesh_send_event_cb(lv_event_t *event)
{
    const char *text = "";

    if(mesh_textarea && lv_obj_is_valid(mesh_textarea)) {
        text = lv_textarea_get_text(mesh_textarea);
    }
    mesh_ui_trace("SEND_CLICK code=%d active_inline=%d text_len=%u",
                  (int)lv_event_get_code(event),
                  ui_input_inline_is_active(mesh_inline_input),
                  (unsigned)strlen(text ? text : ""));

    if(mesh_inline_input && ui_input_inline_is_active(mesh_inline_input)) {
        ui_input_inline_submit(mesh_inline_input);
    } else {
        mesh_send_text_now(text, "BUTTON", 1);
    }
}

static void mesh_input_focus_event_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_inline_input) {
        ui_input_inline_focus(mesh_inline_input);
    }
}

static void mesh_focus_input_if_hardware_keyboard(void)
{
    if(mesh_inline_input && ui_extension_keyboard_active() &&
       !ui_input_inline_is_active(mesh_inline_input)) {
        ui_input_inline_focus(mesh_inline_input);
    }
}

static lv_obj_t *mesh_panel_title(lv_obj_t *parent, const char *title,
                                  const char *subtitle)
{
    lv_obj_t *label = ui_label(parent, title, &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    if(subtitle) {
        lv_obj_t *sub = ui_label(parent, subtitle, &lv_font_montserrat_16,
                                 0x9AA4AF);
        lv_obj_set_width(sub, lv_obj_get_width(parent) - 24);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 34);
    }
    return label;
}

void ui_meshtastic_startup(void)
{
    if(!ui_meshtastic_autostart_enabled()) {
        mesh_append_log("autostart disabled");
        return;
    }

    mesh_load_profile_prefs();
    mesh_start_event_cb(NULL);
    mesh_background_monitor_start();
}

void ui_meshtastic_create(lv_obj_t *scr)
{
    lv_obj_t *btn;
    int top_y = ui_is_landscape() ? 64 : 124;
    int x = ui_page_panel_x();
    int content_w = ui_page_panel_width();
    int landscape = ui_is_landscape();
    int send_w = landscape ? 90 : 82;

    mesh_load_profile_prefs();
    ui_create_header(scr, "Meshtastic");
    mesh_body = ui_page_body(scr, top_y);
    lv_obj_set_scrollbar_mode(mesh_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(mesh_body, LV_OBJ_FLAG_SCROLLABLE);

    mesh_status_panel_h = landscape ? 116 : 166;
    mesh_chat_gap = landscape ? 8 : 10;
    mesh_keyboard_reserved_h = 0;

    mesh_status_panel = ui_panel(mesh_body, x, 0, content_w,
                                 mesh_status_panel_h);
    lv_obj_set_style_bg_color(mesh_status_panel, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(mesh_status_panel, 0, 0);

    mesh_status_label = ui_label(mesh_status_panel, "Daemon offline",
                                 &lv_font_montserrat_20, 0xF5A524);
    lv_obj_set_pos(mesh_status_label, 0, 0);
    lv_obj_set_width(mesh_status_label, content_w);
    lv_label_set_long_mode(mesh_status_label, LV_LABEL_LONG_DOT);

    mesh_profile_label = ui_label(mesh_status_panel, "", &lv_font_montserrat_14,
                                  0xCBD5E1);
    lv_obj_set_pos(mesh_profile_label, 0, 28);
    lv_obj_set_width(mesh_profile_label, content_w);
    lv_label_set_long_mode(mesh_profile_label, LV_LABEL_LONG_DOT);

    mesh_detail_label = ui_label(mesh_status_panel, "",
                                 &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(mesh_detail_label, 0, 52);
    lv_obj_set_width(mesh_detail_label, content_w);
    lv_label_set_long_mode(mesh_detail_label, LV_LABEL_LONG_DOT);

    mesh_airtime_label = ui_label(mesh_status_panel, "ChUtil 0.0%",
                                  &lv_font_montserrat_12, 0x25C281);
    lv_obj_set_pos(mesh_airtime_label, 0, 76);
    lv_obj_set_width(mesh_airtime_label, content_w);
    lv_label_set_long_mode(mesh_airtime_label, LV_LABEL_LONG_DOT);

    mesh_chutil_bar = lv_bar_create(mesh_status_panel);
    lv_obj_set_pos(mesh_chutil_bar, 0, 96);
    lv_obj_set_size(mesh_chutil_bar, content_w, 6);
    lv_bar_set_range(mesh_chutil_bar, 0, 40);
    lv_bar_set_value(mesh_chutil_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mesh_chutil_bar, lv_color_hex(0x263241),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(mesh_chutil_bar, lv_color_hex(0x25C281),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(mesh_chutil_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(mesh_chutil_bar, 3, LV_PART_INDICATOR);

    mesh_channel_button = ui_command_button(mesh_status_panel, 0, 0, 60,
                                            ui_tr("Channel"), 0xA78BFA);
    lv_obj_add_event_cb(mesh_channel_button, mesh_channel_event_cb,
                        LV_EVENT_CLICKED, NULL);
    mesh_tx_channel_button = ui_command_button(mesh_status_panel, 0, 0, 60,
                                               ui_tr("Slots"), 0xEC4899);
    lv_obj_add_event_cb(mesh_tx_channel_button, mesh_channels_event_cb,
                        LV_EVENT_CLICKED, NULL);
    mesh_map_button = ui_command_button(mesh_status_panel, 0, 0, 60,
                                        ui_tr("Map"), 0x3DA5FF);
    lv_obj_add_event_cb(mesh_map_button, mesh_map_event_cb, LV_EVENT_CLICKED,
                        NULL);
    mesh_nodes_button = ui_command_button(mesh_status_panel, 0, 0, 60,
                                          LV_SYMBOL_LIST, 0x25C281);
    lv_obj_add_event_cb(mesh_nodes_button, mesh_nodes_event_cb,
                        LV_EVENT_CLICKED, NULL);
    mesh_settings_button = ui_command_button(mesh_status_panel, 0, 0, 60,
                                             LV_SYMBOL_SETTINGS, 0xA78BFA);
    lv_obj_add_event_cb(mesh_settings_button, mesh_profile_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_chat_scroll = ui_panel(mesh_body, x, 0, content_w, 300);
    lv_obj_set_style_bg_color(mesh_chat_scroll, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(mesh_chat_scroll, 12, 0);
    lv_obj_set_style_pad_row(mesh_chat_scroll, 0, 0);
    ui_make_scrollable(mesh_chat_scroll, 20);
    lv_obj_set_flex_flow(mesh_chat_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mesh_chat_scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    mesh_chat_add_empty();

    mesh_target_button = ui_command_button(mesh_body, x, 0, content_w,
                                           "", 0x25C281);
    lv_obj_set_style_bg_color(mesh_target_button, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(mesh_target_button, 1, 0);
    lv_obj_set_style_border_color(mesh_target_button, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_radius(mesh_target_button, 8, 0);
    lv_obj_add_event_cb(mesh_target_button, mesh_nodes_event_cb,
                        LV_EVENT_CLICKED, NULL);
    mesh_target_label = lv_obj_get_child(mesh_target_button, 0);
    if(mesh_target_label && lv_obj_is_valid(mesh_target_label)) {
        lv_obj_set_style_text_font(mesh_target_label,
                                   ui_font_for_text("mesh_target",
                                                    &lv_font_montserrat_16), 0);
        lv_label_set_long_mode(mesh_target_label, LV_LABEL_LONG_DOT);
    }

    mesh_input_panel = ui_panel(mesh_body, x, 0, content_w, 66);
    lv_obj_set_style_bg_color(mesh_input_panel, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(mesh_input_panel, 0, 0);
    mesh_textarea = lv_textarea_create(mesh_input_panel);
    lv_textarea_set_one_line(mesh_textarea, true);
    lv_textarea_set_placeholder_text(mesh_textarea, ui_tr("Type message"));
    lv_textarea_set_max_length(mesh_textarea, 220);
    lv_obj_set_style_text_font(mesh_textarea,
                               ui_font_for_text("input",
                                                &lv_font_montserrat_18), 0);
    lv_obj_set_style_bg_color(mesh_textarea, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_text_color(mesh_textarea, lv_color_hex(0xF2F5F8), 0);
    lv_obj_set_style_radius(mesh_textarea, 8, 0);
    lv_obj_set_style_border_width(mesh_textarea, 1, 0);
    lv_obj_set_style_border_color(mesh_textarea, lv_color_hex(0x2A3A4A), 0);
    lv_obj_set_style_border_color(mesh_textarea, lv_color_hex(0x25C281),
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(mesh_textarea, 14, 0);
    lv_obj_set_style_pad_right(mesh_textarea, 14, 0);
    lv_obj_set_style_pad_top(mesh_textarea, 8, 0);
    lv_obj_set_style_pad_bottom(mesh_textarea, 8, 0);
    lv_obj_add_event_cb(mesh_textarea, mesh_input_focus_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(mesh_textarea, mesh_input_focus_event_cb,
                        LV_EVENT_FOCUSED, NULL);

    mesh_canned_button = ui_command_button(mesh_input_panel, 0, 0, 56,
                                           LV_SYMBOL_LIST, 0x3DA5FF);
    lv_obj_add_event_cb(mesh_canned_button, mesh_canned_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_photo_button = ui_command_button(mesh_input_panel, 0, 0, 56,
                                          LV_SYMBOL_IMAGE, 0xEC4899);
    if(!mesh_photo_available) {
        lv_obj_add_flag(mesh_photo_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(mesh_photo_button, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(mesh_photo_button, mesh_photo_event_cb,
                        LV_EVENT_CLICKED, NULL);

    mesh_voice_button = ui_command_button(mesh_input_panel, 0, 0, 58,
                                          ui_tr("Mic"), 0xF59E0B);
    if(!mesh_voice_available) {
        lv_obj_add_flag(mesh_voice_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(mesh_voice_button, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(mesh_voice_button, mesh_voice_event_cb,
                        LV_EVENT_ALL, NULL);

    mesh_send_button = ui_command_button(mesh_input_panel,
                                         content_w - send_w, 0,
                                         send_w, "Send", 0x25C281);
    lv_obj_add_event_cb(mesh_send_button, mesh_send_event_cb,
                        LV_EVENT_CLICKED, NULL);
    mesh_inline_input = ui_input_inline_create(mesh_textarea, scr, 220,
                                               mesh_send_submit_cb, NULL,
                                               mesh_inline_layout_cb, NULL);

    if(!mesh_log_text[0]) {
        snprintf(mesh_log_text, sizeof(mesh_log_text), "%s\n",
                 "Meshtastic UI ready");
    }
    mesh_update_profile_label();
    mesh_update_target_button();
    if(mesh_last_chat_text[0]) {
        mesh_chat_rebuild(mesh_last_chat_text);
    }

    mesh_layout_main();
    mesh_focus_input_if_hardware_keyboard();
    mesh_start_event_cb(NULL);
    mesh_timer = lv_timer_create(mesh_timer_cb, 2000, NULL);
}

void ui_meshtastic_cleanup(void)
{
    if(mesh_voice_record_pid > 0) {
        (void)mesh_voice_record_stop(0, "CLEANUP");
    }
    if(mesh_body && lv_obj_is_valid(mesh_body)) {
        lv_obj_add_flag(mesh_body, LV_OBJ_FLAG_HIDDEN);
    }
    mesh_voice_stop_playback(1);
    mesh_voice_record_overlay_close();
    mesh_voice_preview_close();
    mesh_photo_picker_close();
    mesh_photo_preview_close();
    if(mesh_timer) {
        lv_timer_delete(mesh_timer);
        mesh_timer = NULL;
    }
    if(mesh_inline_input) {
        ui_input_inline_destroy(mesh_inline_input);
        mesh_inline_input = NULL;
    }
    mesh_body = NULL;
    mesh_status_panel = NULL;
    mesh_input_panel = NULL;
    mesh_textarea = NULL;
    mesh_status_label = NULL;
    mesh_detail_label = NULL;
    mesh_profile_label = NULL;
    mesh_airtime_label = NULL;
    mesh_chutil_bar = NULL;
    mesh_chat_scroll = NULL;
    mesh_log_label = NULL;
    mesh_target_button = NULL;
    mesh_target_label = NULL;
    mesh_send_button = NULL;
    mesh_canned_button = NULL;
    mesh_voice_button = NULL;
    mesh_photo_button = NULL;
    mesh_channel_button = NULL;
    mesh_tx_channel_button = NULL;
    mesh_map_button = NULL;
    mesh_nodes_button = NULL;
    mesh_settings_button = NULL;
    mesh_keyboard_reserved_h = 0;
    mesh_choice_close();
    mesh_canned_delete_confirm_close();
    mesh_canned_close();
    mesh_channel_edit_close();
    mesh_channel_profiles_close();
    mesh_channels_close();
    mesh_log_close();
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    mesh_settings_status_card = NULL;
    memset(mesh_settings_status_labels, 0, sizeof(mesh_settings_status_labels));
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
    mesh_close_nodes_page();
    mesh_detector_close();
    mesh_waypoints_close();
    mesh_map_close();
    mesh_close_channel_page();
}

int ui_meshtastic_handle_back(void)
{
    if(mesh_photo_preview_overlay && lv_obj_is_valid(mesh_photo_preview_overlay)) {
        mesh_photo_preview_close();
        return 1;
    }
    if(mesh_photo_picker_overlay && lv_obj_is_valid(mesh_photo_picker_overlay)) {
        mesh_photo_picker_close();
        return 1;
    }
    if(mesh_voice_preview_overlay && lv_obj_is_valid(mesh_voice_preview_overlay)) {
        mesh_voice_preview_close();
        return 1;
    }
    if(mesh_pairing_overlay && lv_obj_is_valid(mesh_pairing_overlay)) {
        mesh_pairing_notice_close_cb(NULL);
        return 1;
    }
    if(mesh_log_overlay && lv_obj_is_valid(mesh_log_overlay)) {
        mesh_log_close();
        return 1;
    }
    if(mesh_choice_overlay && lv_obj_is_valid(mesh_choice_overlay)) {
        mesh_choice_close();
        return 1;
    }
    if(mesh_channel_profile_delete_overlay &&
       lv_obj_is_valid(mesh_channel_profile_delete_overlay)) {
        mesh_channel_profile_delete_confirm_close();
        return 1;
    }
    if(mesh_channel_profiles_overlay &&
       lv_obj_is_valid(mesh_channel_profiles_overlay)) {
        mesh_channel_profiles_close();
        return 1;
    }
    if(mesh_channel_edit_overlay && lv_obj_is_valid(mesh_channel_edit_overlay)) {
        mesh_channel_edit_close();
        return 1;
    }
    if(mesh_channel_scan_overlay &&
       lv_obj_is_valid(mesh_channel_scan_overlay)) {
        mesh_channel_scan_overlay_close();
        return 1;
    }
    if(mesh_channel_import_overlay &&
       lv_obj_is_valid(mesh_channel_import_overlay)) {
        mesh_channel_import_confirm_close();
        return 1;
    }
    if(mesh_canned_delete_overlay && lv_obj_is_valid(mesh_canned_delete_overlay)) {
        mesh_canned_delete_confirm_close();
        return 1;
    }
    if(mesh_canned_overlay && lv_obj_is_valid(mesh_canned_overlay)) {
        mesh_canned_close();
        return 1;
    }
    if(mesh_channels_overlay && lv_obj_is_valid(mesh_channels_overlay)) {
        mesh_channels_close();
        return 1;
    }
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        mesh_close_channel_page();
        return 1;
    }
    if(mesh_waypoints_overlay && lv_obj_is_valid(mesh_waypoints_overlay)) {
        mesh_waypoints_close();
        return 1;
    }
    if(mesh_map_overlay && lv_obj_is_valid(mesh_map_overlay)) {
        mesh_map_close();
        return 1;
    }
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        mesh_close_settings_page();
        return 1;
    }
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        mesh_close_nodes_page();
        return 1;
    }
    if(mesh_detector_overlay && lv_obj_is_valid(mesh_detector_overlay)) {
        mesh_detector_close();
        return 1;
    }
    if(mesh_inline_input && ui_input_inline_is_active(mesh_inline_input)) {
        ui_input_inline_hide(mesh_inline_input);
        return 1;
    }
    return 0;
}
