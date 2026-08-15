#include "ui_meshtastic.h"

#include "ui_audio.h"
#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>

#include "qrcodegen.h"

#define MESHTASTIC_PROBE_PATH "/root/app/k230_phone_ui/k230_meshtastic_probe"
#define MESHTASTIC_SOCKET_PATH "/tmp/k230_meshtastic.sock"
#define MESHTASTIC_DAEMON_LOG "/tmp/k230_meshtastic_daemon_ui.log"
#define MESHTASTIC_UI_TRACE_LOG "/tmp/k230_meshtastic_ui.log"
#define MESHTASTIC_CHANNEL_DIR "/root/meshtastic"
#define MESHTASTIC_CHANNEL_URL_FILE MESHTASTIC_CHANNEL_DIR "/channel_url.txt"
#define MESHTASTIC_CHANNEL_PROFILE_DIR MESHTASTIC_CHANNEL_DIR "/channels"
#define MESHTASTIC_QR_SCAN_PATH "/root/app/k230_phone_ui/k230_qr_scan"
#define MESHTASTIC_UI_LOG_MAX 4096
#define MESHTASTIC_UI_NODE_SELECT_MAX 24
#define MESHTASTIC_UI_NODE_LINE_MAX 768
#define MESHTASTIC_CHANNEL_PROFILE_MAX 24
#define MESHTASTIC_CHANNEL_QR_MAX 280
#define MESHTASTIC_CHANNEL_QR_BORDER 4
#define MESHTASTIC_PREF_REGION "meshtastic.region"
#define MESHTASTIC_PREF_PRESET "meshtastic.preset"
#define MESHTASTIC_PREF_CHANNEL "meshtastic.channel"
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
#define MESHTASTIC_PREF_TELEMETRY "meshtastic.telemetry"
#define MESHTASTIC_PREF_TELEMETRY_ENV "meshtastic.telemetry_env"
#define MESHTASTIC_PREF_TELEMETRY_DEVICE_INTERVAL "meshtastic.telemetry_device_interval"
#define MESHTASTIC_PREF_TELEMETRY_ENV_INTERVAL "meshtastic.telemetry_env_interval"
#define MESHTASTIC_DEFAULT_UI_REGION "EU_868"
#define MESHTASTIC_DEFAULT_UI_PRESET "LONG_FAST"
#define MESHTASTIC_QR_PREVIEW_FILE "/tmp/k230_mesh_qr_preview.rgb565"
#define MESHTASTIC_QR_PREVIEW_TMP MESHTASTIC_QR_PREVIEW_FILE ".tmp"
#define MESHTASTIC_QR_PREVIEW_W 384
#define MESHTASTIC_QR_PREVIEW_H 216
#define MESHTASTIC_QR_PREVIEW_BYTES (MESHTASTIC_QR_PREVIEW_W * MESHTASTIC_QR_PREVIEW_H * 2)

static lv_obj_t *mesh_status_label;
static lv_obj_t *mesh_detail_label;
static lv_obj_t *mesh_profile_label;
static lv_obj_t *mesh_chat_scroll;
static lv_obj_t *mesh_log_label;
static lv_obj_t *mesh_send_button;
static lv_obj_t *mesh_body;
static lv_obj_t *mesh_status_panel;
static lv_obj_t *mesh_input_panel;
static lv_obj_t *mesh_textarea;
static ui_input_inline_t *mesh_inline_input;
static lv_timer_t *mesh_timer;
static lv_timer_t *mesh_background_timer;
static char mesh_status_text[4096] = "Not running";
static char mesh_log_text[MESHTASTIC_UI_LOG_MAX];
static char mesh_last_chat_text[3072];
static char mesh_last_ble_state[32] = "offline";
static char mesh_node_select_ids[MESHTASTIC_UI_NODE_SELECT_MAX][24];
static char mesh_node_select_lines[MESHTASTIC_UI_NODE_SELECT_MAX][MESHTASTIC_UI_NODE_LINE_MAX];
static char mesh_node_detail_target_id[24];
static int mesh_keyboard_reserved_h;
static int mesh_status_panel_h;
static int mesh_chat_gap;
static char mesh_region[24] = MESHTASTIC_DEFAULT_UI_REGION;
static char mesh_preset[32] = MESHTASTIC_DEFAULT_UI_PRESET;
static char mesh_channel_name[64] = "";
static char mesh_frequency_slot[8] = "auto";
static char mesh_psk[80] = "default";
static char mesh_tx_power[8] = "auto";
static char mesh_node_name[48] = "k230-t-display";
static char mesh_from_node[24] = "0";
static char mesh_to_node[24] = "0xffffffff";
static char mesh_hop_limit[8] = "3";
static char mesh_position_interval[8] = "900";
static char mesh_telemetry_device_interval[8] = "300";
static char mesh_telemetry_environment_interval[8] = "300";
static int mesh_ack_enabled = 0;
static int mesh_rebroadcast_enabled = 0;
static int mesh_position_enabled = 1;
static int mesh_telemetry_enabled = 1;
static int mesh_environment_telemetry_enabled = 1;
static lv_obj_t *mesh_settings_overlay;
static lv_obj_t *mesh_nodes_overlay;
static lv_obj_t *mesh_choice_overlay;
static lv_obj_t *mesh_channel_overlay;
static lv_obj_t *mesh_channel_profiles_overlay;
static lv_obj_t *mesh_channel_url_label;
static lv_obj_t *mesh_channel_status_label;
static lv_obj_t *mesh_channel_qr_canvas;
static lv_obj_t *mesh_pairing_overlay;
static lv_obj_t *mesh_settings_value_labels[24];
static char mesh_last_pairing_code[16];
static char mesh_channel_url_text[1024];
static uint16_t mesh_channel_qr_buf[MESHTASTIC_CHANNEL_QR_MAX *
                                    MESHTASTIC_CHANNEL_QR_MAX];
static lv_timer_t *mesh_channel_scan_timer;
static lv_obj_t *mesh_channel_scan_overlay;
static lv_obj_t *mesh_channel_scan_preview_image;
static lv_obj_t *mesh_channel_scan_preview_placeholder;
static lv_obj_t *mesh_channel_scan_preview_status_label;
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
static char mesh_channel_scan_status[256];
static char mesh_channel_profile_paths[MESHTASTIC_CHANNEL_PROFILE_MAX][160];

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

static void mesh_settings_refresh(void);
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

static void mesh_apply_ble_status(const char *status, int online)
{
    char ble_state[32];

    mesh_status_field(status, "ble", ble_state, sizeof(ble_state), "offline");
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
           strcasecmp(text, "0xffffffff") == 0 ||
           strcasecmp(text, "0x000000ff") == 0 ||
           strcasecmp(text, "0xff") == 0;
}

static void mesh_normalize_target_ack(void)
{
    if(mesh_to_text_is_broadcast(mesh_to_node)) {
        snprintf(mesh_to_node, sizeof(mesh_to_node), "0xffffffff");
        mesh_ack_enabled = 0;
    }
}

static void mesh_update_profile_label(void)
{
    char text[180];

    snprintf(text, sizeof(text),
             "%s  %s  %s  slot:%s",
             mesh_region, mesh_preset,
             mesh_channel_name[0] ? mesh_channel_name : "default",
             mesh_slot_is_auto() ? "auto" : mesh_frequency_slot);
    if(mesh_profile_label && lv_obj_is_valid(mesh_profile_label)) {
        lv_label_set_text(mesh_profile_label, text);
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
    mesh_normalize_power();
    mesh_normalize_hop();
    mesh_normalize_slot();
    mesh_normalize_target_ack();
}

static void mesh_save_profile_prefs(void)
{
    ui_prefs_set(MESHTASTIC_PREF_REGION, mesh_region);
    ui_prefs_set(MESHTASTIC_PREF_PRESET, mesh_preset);
    ui_prefs_set(MESHTASTIC_PREF_CHANNEL, mesh_channel_name);
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
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY,
                 mesh_telemetry_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_ENV,
                 mesh_environment_telemetry_enabled ? "1" : "0");
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_DEVICE_INTERVAL,
                 mesh_telemetry_device_interval);
    ui_prefs_set(MESHTASTIC_PREF_TELEMETRY_ENV_INTERVAL,
                 mesh_telemetry_environment_interval);
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
    changed |= mesh_status_copy_if_changed(mesh_from_node,
                                           sizeof(mesh_from_node), value);
    mesh_status_field(status, "to", value, sizeof(value), "");
    changed |= mesh_status_copy_if_changed(mesh_to_node,
                                           sizeof(mesh_to_node), value);

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

static void mesh_chat_parse_line(const char *line, int *sent,
                                 char *meta, size_t meta_len,
                                 char *body, size_t body_len)
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
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *text;
    lv_obj_t *footer;
    int page_w;
    int bubble_w;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll) ||
       !line || !line[0]) {
        return;
    }
    mesh_chat_parse_line(line, &sent, meta, sizeof(meta), body, sizeof(body));
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
                              lv_color_hex(sent ? 0x16A34A : 0x232B35), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 5, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    text = ui_label(bubble, body, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_set_width(text, bubble_w - 20);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    footer = ui_label(bubble, meta[0] ? meta : (sent ? "TX" : "RX"),
                      &lv_font_montserrat_14,
                      sent ? 0xDDFCE8 : 0x94A3B8);
    lv_obj_set_width(footer, bubble_w - 20);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_DOT);
}

static void mesh_chat_rebuild(const char *shown)
{
    char copy[3072];
    char *line;
    char *save = NULL;
    int count = 0;

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
            mesh_chat_add_bubble(line);
            count++;
        }
        line = strtok_r(NULL, "\n", &save);
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

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll) ||
       !lines || !lines[0]) {
        return 0;
    }

    snprintf(copy, sizeof(copy), "%s", lines);
    line = strtok_r(copy, "\n", &save);
    while(line) {
        ui_trim_text(line);
        if(line[0] && strcmp(line, "No mesh messages yet") != 0) {
            mesh_chat_add_bubble(line);
            appended++;
        }
        line = strtok_r(NULL, "\n", &save);
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
    char response[3072];
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
    int input_h = ui_is_landscape() ? 58 : 66;
    int bottom_pad = ui_is_landscape() ? 8 : 12;
    int available_h;
    int input_y;
    int chat_y;
    int chat_h;
    int send_w = ui_is_landscape() ? 90 : 82;
    int textarea_w;

    if(!mesh_body || !lv_obj_is_valid(mesh_body)) {
        return;
    }

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
    chat_y = mesh_status_panel_h + mesh_chat_gap;
    chat_h = input_y - chat_y - mesh_chat_gap;
    if(chat_h < 140) {
        chat_h = 140;
    }

    if(mesh_status_panel && lv_obj_is_valid(mesh_status_panel)) {
        lv_obj_set_pos(mesh_status_panel, x, 0);
        lv_obj_set_size(mesh_status_panel, content_w, mesh_status_panel_h);
    }
    if(mesh_chat_scroll && lv_obj_is_valid(mesh_chat_scroll)) {
        lv_obj_set_pos(mesh_chat_scroll, x, chat_y);
        lv_obj_set_size(mesh_chat_scroll, content_w, chat_h);
    }
    if(mesh_input_panel && lv_obj_is_valid(mesh_input_panel)) {
        lv_obj_set_pos(mesh_input_panel, x, input_y);
        lv_obj_set_size(mesh_input_panel, content_w, input_h);
    }
    if(mesh_textarea && lv_obj_is_valid(mesh_textarea)) {
        textarea_w = content_w - send_w - 16;
        if(textarea_w < 180) {
            textarea_w = 180;
        }
        lv_obj_set_pos(mesh_textarea, 0, 0);
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

    if(mesh_status_label && lv_obj_is_valid(mesh_status_label)) {
        lv_label_set_text(mesh_status_label,
                          online ? ui_tr("Daemon online") :
                          ui_tr("Daemon offline"));
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
        snprintf(detail, sizeof(detail),
                 "%s -> %s  GPS %s/%s S%s TX%s  TEL%s  Q%s ACK %s P%s/R%s/N%s/RT%s/TO%s/D%s",
                 mesh_node_name,
                 mesh_to_text_is_broadcast(mesh_to_node) ? "broadcast" :
                 mesh_to_node,
                 nrf9151, gps, sats, position_tx,
                 telemetry_tx, queued_count, mesh_ack_enabled ? "on" : "off",
                 ack_pending, ack_rx, nak_rx, ack_retry, ack_timeout,
                 ack_drop);
        lv_label_set_text(mesh_detail_label, detail);
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

static void mesh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    mesh_refresh_status();
}

static void mesh_background_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(app_current_page_is(PAGE_MESHTASTIC)) {
        return;
    }
    mesh_refresh_status();
}

static void mesh_background_monitor_start(void)
{
    if(!mesh_background_timer) {
        mesh_background_timer = lv_timer_create(mesh_background_timer_cb,
                                                3000, NULL);
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
    char telemetry_option[160];
    char telemetry_device_interval_arg[16];
    char telemetry_environment_interval_arg[16];
    char command[1536];
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
    mesh_safe_or_default(from_arg, sizeof(from_arg), mesh_from_node, "0");
    mesh_normalize_target_ack();
    mesh_save_profile_prefs();
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
    if(mesh_channel_name[0]) {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--channel-name %s %s--psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s%s%s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, channel_arg, slot_option, psk_arg,
                 power_option, node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option,
                 position_option, telemetry_option);
    } else {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "%s--psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s%s%s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, slot_option, psk_arg, power_option,
                 node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option,
                 position_option, telemetry_option);
    }
    rc = system(command);
    mesh_append_log("start daemon rc=%d log=%s", ui_shell_exit_code(rc),
                    MESHTASTIC_DAEMON_LOG);
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
    usleep(120000);
    mesh_refresh_status();
}

static void mesh_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    mesh_refresh_status();
    mesh_append_log("refresh: %s", mesh_status_text);
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

static void mesh_publish_event_cb(lv_event_t *event)
{
    const char *command = (const char *)lv_event_get_user_data(event);
    char response[256];

    if(!command || !command[0]) {
        return;
    }
    if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
        ui_trim_text(response);
        mesh_append_log("publish: %s", response);
    } else {
        ui_trim_text(response);
        mesh_append_log("publish failed: %s", response);
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

    if(mesh_channel_profile_ensure_dir() != 0) {
        return -1;
    }
    for(int i = 0; i < 100; i++) {
        snprintf(path, sizeof(path), "%s/channel_%02d.conf",
                 MESHTASTIC_CHANNEL_PROFILE_DIR, i);
        if(access(path, F_OK) != 0) {
            slot = i;
            break;
        }
    }
    if(slot < 0) {
        mesh_append_log("channel profile save failed: no free slot");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/channel_%02d.conf",
             MESHTASTIC_CHANNEL_PROFILE_DIR, slot);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        mesh_append_log("channel profile save failed: %s", strerror(errno));
        return -1;
    }

    mesh_channel_profile_default_name(name, sizeof(name));
    fprintf(fp, "# K230 Meshtastic channel profile\n");
    fprintf(fp, "name=%s\n", name);
    fprintf(fp, "region=%s\n", mesh_region);
    fprintf(fp, "preset=%s\n", mesh_preset);
    fprintf(fp, "channel=%s\n", mesh_channel_name);
    fprintf(fp, "slot=%s\n", mesh_frequency_slot);
    fprintf(fp, "psk=%s\n", mesh_psk);
    fprintf(fp, "power=%s\n", mesh_tx_power);
    fprintf(fp, "node=%s\n", mesh_node_name);
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
    mesh_append_log("channel profile saved: %s", path);
    return 0;
}

static void mesh_channel_profile_apply_file(const char *path)
{
    char value[96];

    if(!path || !path[0]) {
        return;
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
        mesh_safe_or_default(mesh_from_node, sizeof(mesh_from_node), value,
                             "0");
    }
    if(mesh_channel_profile_read_value(path, "to", value, sizeof(value),
                                       "0xffffffff") == 0) {
        mesh_safe_or_default(mesh_to_node, sizeof(mesh_to_node), value,
                             "0xffffffff");
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
    mesh_channel_profiles_close();
    mesh_append_log("channel profile loaded: %s", path);
    mesh_restart_daemon_if_online();
}

static void mesh_channel_profile_load_event_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);

    mesh_channel_profile_apply_file(path);
}

static void mesh_channel_profiles_event_cb(lv_event_t *event);

static void mesh_channel_profile_save_event_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_channel_profile_write_current() == 0) {
        mesh_channel_profiles_event_cb(NULL);
    }
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
    lv_obj_t *hint_label;
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
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, mesh_channel_profile_load_event_cb,
                        LV_EVENT_CLICKED, mesh_channel_profile_paths[index]);

    name_label = ui_label(card, name, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(name_label, 14, 12);
    lv_obj_set_width(name_label, w - 28);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    summary_label = ui_label(card, summary, &lv_font_montserrat_14, 0xCBD5E1);
    lv_obj_set_pos(summary_label, 14, 48);
    lv_obj_set_width(summary_label, w - 28);
    lv_label_set_long_mode(summary_label, LV_LABEL_LONG_WRAP);

    hint_label = ui_label(card, ui_tr("Tap to load and restart"),
                          &lv_font_montserrat_14, 0x25C281);
    lv_obj_set_pos(hint_label, 14, h - 34);
    lv_obj_set_width(hint_label, w - 28);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_DOT);
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
    int card_h = 156;
    int y = 112;
    int count;

    (void)event;
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
        return 1;
    default:
        return 0;
    }
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
    default:
        return "";
    }
}

static void mesh_settings_refresh(void)
{
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
        mesh_safe_or_default(tmp, sizeof(tmp), text, "auto");
        if(mesh_from_text_is_auto(tmp)) {
            snprintf(mesh_from_node, sizeof(mesh_from_node), "0");
            break;
        }
        if(mesh_parse_u32_text(tmp, &value) != 0) {
            mesh_append_log("invalid from node: %s", text);
            return;
        }
        snprintf(mesh_from_node, sizeof(mesh_from_node), "%s", tmp);
        break;
    case MESH_FIELD_TO:
        mesh_safe_or_default(tmp, sizeof(tmp), text, "broadcast");
        if(mesh_to_text_is_broadcast(tmp)) {
            snprintf(mesh_to_node, sizeof(mesh_to_node), "0xffffffff");
            mesh_ack_enabled = 0;
            break;
        }
        if(mesh_parse_u32_text(tmp, &value) != 0) {
            mesh_append_log("invalid to node: %s", text);
            return;
        }
        snprintf(mesh_to_node, sizeof(mesh_to_node), "%s", tmp);
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
    default:
        return;
    }
    mesh_save_profile_prefs();
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
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    mesh_log_label = NULL;
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
}

static void mesh_close_nodes_page(void)
{
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_overlay = NULL;
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
    mesh_channel_scan_preview_panel_w = 0;
    mesh_channel_scan_preview_panel_h = 0;
}

static void mesh_close_channel_page(void)
{
    if(mesh_channel_scan_timer) {
        lv_timer_delete(mesh_channel_scan_timer);
        mesh_channel_scan_timer = NULL;
    }
    mesh_channel_scan_overlay_close();
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
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int title_y = ui_is_landscape() ? 18 : 28;
    int preview_y = ui_is_landscape() ? 72 : 92;
    int status_y;
    int preview_h = screen_h - preview_y - 112;

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

static void mesh_channel_qr_render(const char *url, int px)
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

    if(!mesh_channel_qr_canvas || !lv_obj_is_valid(mesh_channel_qr_canvas)) {
        return;
    }
    if(px < 64) {
        px = 64;
    }
    if(px > MESHTASTIC_CHANNEL_QR_MAX) {
        px = MESHTASTIC_CHANNEL_QR_MAX;
    }

    for(int i = 0; i < px * px; i++) {
        mesh_channel_qr_buf[i] = empty;
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
                    mesh_channel_qr_buf[y * px + x] = module ? black : white;
                }
            }
        }
    }
    lv_canvas_set_buffer(mesh_channel_qr_canvas, mesh_channel_qr_buf,
                         px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(mesh_channel_qr_canvas);
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
             " --timeout-sec 18 --preview-file " MESHTASTIC_QR_PREVIEW_FILE
             " --preview-width %d --preview-height %d "
             "--preview-interval-ms 100 2>/tmp/k230_qr_scan.log",
             MESHTASTIC_QR_PREVIEW_W, MESHTASTIC_QR_PREVIEW_H);
    fp = popen(command, "r");
    if(fp) {
        if(fgets(url, sizeof(url), fp)) {
            ui_trim_text(url);
        }
        if(pclose(fp) == 0 && url[0]) {
            snprintf(command, sizeof(command), "IMPORT_CHANNEL_URL %s\n", url);
            if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
                ui_trim_text(response);
                ok = strncmp(response, "OK imported", 11) == 0;
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
    snprintf(mesh_channel_scan_status, sizeof(mesh_channel_scan_status), "%s",
             response[0] ? response : (ok ? "Channel imported" :
             "QR scan failed"));
    pthread_mutex_unlock(&mesh_channel_scan_mutex);
    return NULL;
}

static void mesh_channel_scan_timer_cb(lv_timer_t *timer)
{
    char status[256];
    int ready;
    int ok;
    int qr_px;

    (void)timer;
    mesh_channel_scan_preview_update();
    pthread_mutex_lock(&mesh_channel_scan_mutex);
    ready = mesh_channel_scan_ready;
    ok = mesh_channel_scan_ok;
    snprintf(status, sizeof(status), "%s", mesh_channel_scan_status);
    if(ready) {
        mesh_channel_scan_ready = 0;
    }
    pthread_mutex_unlock(&mesh_channel_scan_mutex);

    if(!ready) {
        return;
    }
    qr_px = (mesh_channel_qr_canvas &&
             lv_obj_is_valid(mesh_channel_qr_canvas)) ?
            lv_obj_get_width(mesh_channel_qr_canvas) : 0;
    if(qr_px <= 0) {
        qr_px = ui_is_landscape() ? 220 : 240;
    }
    if(ok) {
        char fetch_status[160];

        mesh_load_profile_prefs();
        mesh_settings_refresh();
        mesh_update_profile_label();
        mesh_channel_url_text[0] = '\0';
        (void)mesh_channel_url_fetch(mesh_channel_url_text,
                                     sizeof(mesh_channel_url_text),
                                     fetch_status, sizeof(fetch_status));
        mesh_channel_refresh_view(1, ui_tr("Channel imported"), qr_px);
        mesh_append_log("channel QR import: %s", status);
        mesh_channel_scan_overlay_close();
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
    int card_y = landscape ? 88 : 104;
    int card_h = landscape ? screen_h - card_y - 104 : 392;
    int qr_px;
    int text_x;
    int text_y;
    int text_w;
    int button_w;
    int y;

    (void)event;
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        mesh_close_channel_page();
    }

    mesh_channel_url_text[0] = '\0';
    (void)mesh_channel_url_fetch(mesh_channel_url_text,
                                 sizeof(mesh_channel_url_text),
                                 status, sizeof(status));

    if(card_h < 220) {
        card_h = 220;
    }
    if(!landscape && card_h > screen_h - card_y - 126) {
        card_h = screen_h - card_y - 126;
    }
    if(card_h < 220) {
        card_h = 220;
    }
    qr_px = landscape ? card_h - 32 : content_w - 96;
    if(qr_px > MESHTASTIC_CHANNEL_QR_MAX) {
        qr_px = MESHTASTIC_CHANNEL_QR_MAX;
    }
    if(qr_px < 180) {
        qr_px = 180;
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
    lv_obj_set_width(title, content_w - 120);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    subtitle = ui_label(panel, ui_tr("Meshtastic official channel URL"),
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 120);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

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
    lv_obj_set_pos(mesh_channel_qr_canvas,
                   landscape ? 16 : (content_w - qr_px) / 2, 16);
    mesh_channel_qr_render(mesh_channel_url_text, qr_px);

    text_x = landscape ? qr_px + 32 : 16;
    text_y = landscape ? 18 : qr_px + 34;
    text_w = landscape ? content_w - text_x - 32 : content_w - 32;
    if(text_w < 160) {
        text_w = content_w - 32;
        text_x = 16;
        text_y = qr_px + 34;
    }

    label = ui_label(card, ui_tr("Show this QR to Meshtastic app"),
                     &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(label, text_x, text_y);
    lv_obj_set_width(label, text_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    mesh_channel_url_label =
        ui_label(card,
                 mesh_channel_url_text[0] ? mesh_channel_url_text :
                 ui_tr("Channel URL unavailable"),
                 &lv_font_montserrat_16,
                 mesh_channel_url_text[0] ? 0xD7DEE8 : 0xF5A524);
    lv_obj_set_pos(mesh_channel_url_label, text_x, text_y + 34);
    lv_obj_set_width(mesh_channel_url_label, text_w);
    lv_label_set_long_mode(mesh_channel_url_label, LV_LABEL_LONG_WRAP);

    y = card_y + card_h + 16;
    mesh_channel_status_label = ui_label(panel, status[0] ? status : "Ready",
                                         &lv_font_montserrat_16,
                                         mesh_channel_url_text[0] ?
                                         0x25C281 : 0xF5A524);
    lv_obj_set_pos(mesh_channel_status_label, margin, y);
    lv_obj_set_width(mesh_channel_status_label, content_w);
    lv_label_set_long_mode(mesh_channel_status_label, LV_LABEL_LONG_DOT);

    y += 42;
    button_w = (content_w - 24) / 3;
    if(button_w < 112) {
        button_w = 112;
    }
    btn = ui_command_button(panel, margin, y, button_w, ui_tr("Save URL"),
                            0x25C281);
    lv_obj_add_event_cb(btn, mesh_channel_save_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(panel, margin + button_w + 12, y, button_w,
                            ui_tr("Refresh"), 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_channel_refresh_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(panel, margin + (button_w + 12) * 2, y,
                            button_w, ui_tr("Scan QR"), 0x8B5CF6);
    lv_obj_add_event_cb(btn, mesh_channel_scan_event_cb, LV_EVENT_CLICKED,
                        NULL);
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

static void mesh_nodes_event_cb(lv_event_t *event);

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
    start = strstr(line, key);
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

static void mesh_select_node_target_event_cb(lv_event_t *event)
{
    const char *node_id = (const char *)lv_event_get_user_data(event);

    if(!node_id || !node_id[0]) {
        return;
    }
    snprintf(mesh_to_node, sizeof(mesh_to_node), "%s", node_id);
    mesh_save_profile_prefs();
    mesh_update_profile_label();
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

static void mesh_node_detail_event_cb(lv_event_t *event)
{
    const char *line = (const char *)lv_event_get_user_data(event);
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *section;
    lv_obj_t *label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int landscape = ui_is_landscape();
    int left_w = landscape ? (content_w - 18) / 2 : content_w;
    int right_x = landscape ? margin + left_w + 18 : margin;
    int right_w = landscape ? content_w - left_w - 18 : content_w;
    int y;
    char node_id[24];
    char name[64];
    char short_name[24];
    char hw[16];
    char rx[16];
    char age[24];
    char rssi[24];
    char snr[24];
    char pos[128];
    char tel[160];
    char nbr[192];
    char summary[320];

    if(!line || strncmp(line, "0x", 2) != 0 ||
       sscanf(line, "%23s", node_id) != 1) {
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
    mesh_node_line_segment(line, "tel=", " nbr=", tel, sizeof(tel));
    mesh_node_line_segment(line, "nbr=", NULL, nbr, sizeof(nbr));
    if(strcmp(name, "-") == 0 && strcmp(short_name, "-") != 0) {
        snprintf(name, sizeof(name), "%s", short_name);
    }
    snprintf(mesh_node_detail_target_id, sizeof(mesh_node_detail_target_id),
             "%s", node_id);

    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
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

    y = 104;
    snprintf(summary, sizeof(summary),
             "RSSI %s\nSNR %s\nPackets %s\nLast seen %s\nHardware %s",
             rssi, snr, rx, age, hw);

    section = ui_label(panel, "Signal", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    label = ui_label(panel, summary, &lv_font_montserrat_18, 0xCBD5E1);
    lv_obj_set_pos(label, margin, y + 36);
    lv_obj_set_width(label, left_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    if(landscape) {
        section = ui_label(panel, "Position", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, right_x, y);
        label = ui_label(panel, pos, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, right_x, y + 36);
        lv_obj_set_width(label, right_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        y += 176;
        section = ui_label(panel, "Telemetry", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, margin, y);
        label = ui_label(panel, tel, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 36);
        lv_obj_set_width(label, left_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        section = ui_label(panel, "Neighbors", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, right_x, y);
        label = ui_label(panel, nbr, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, right_x, y + 36);
        lv_obj_set_width(label, right_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    } else {
        y += 172;
        section = ui_label(panel, "Position", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, margin, y);
        label = ui_label(panel, pos, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 36);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        y += 156;
        section = ui_label(panel, "Telemetry", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, margin, y);
        label = ui_label(panel, tel, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 36);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        y += 156;
        section = ui_label(panel, "Neighbors", &lv_font_montserrat_20,
                           0xF2F5F8);
        lv_obj_set_pos(section, margin, y);
        label = ui_label(panel, nbr, &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 36);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

        y += 156;
        section = ui_label(panel, "Raw", &lv_font_montserrat_20, 0xF2F5F8);
        lv_obj_set_pos(section, margin, y);
        label = ui_label(panel, line, &lv_font_montserrat_14, 0x94A3B8);
        lv_obj_set_pos(label, margin, y + 36);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
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

static void mesh_profile_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *section;
    lv_obj_t *status;
    lv_obj_t *log_title;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;
    int row_h = 62;
    int value_x = ui_is_landscape() ? 190 : 138;
    int edit_w = 92;
    int value_w = content_w - value_x - edit_w - 18;
    int button_w;
    int button_gap = 10;
    int button_cols = ui_is_landscape() ? 6 : 3;
    int button_row_h = 56;
    int publish_w;
    int y = 0;

    (void)event;
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));

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
                        "Mesh radio, channel, ACK and node settings",
                        &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_pos(subtitle, margin, 56);
    lv_obj_set_width(subtitle, content_w - 120);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96, "Close",
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_settings_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    y = 98;
    section = ui_label(panel, "Connection", &lv_font_montserrat_18,
                       0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    y += 34;
    button_w = (content_w - button_gap * (button_cols - 1)) / button_cols;
    if(button_w < 86) {
        button_w = 86;
    }
#define MESH_CONN_BUTTON(index, label, color, cb) \
    do { \
        int bx = margin + ((index) % button_cols) * (button_w + button_gap); \
        int by = y + ((index) / button_cols) * button_row_h; \
        btn = ui_command_button(panel, bx, by, button_w, label, color); \
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL); \
    } while(0)

    MESH_CONN_BUTTON(0, "Start", 0x25C281, mesh_start_event_cb);
    MESH_CONN_BUTTON(1, "Stop", 0xEF4D5A, mesh_stop_event_cb);
    MESH_CONN_BUTTON(2, "Refresh", 0x3DA5FF, mesh_refresh_event_cb);
    MESH_CONN_BUTTON(3, "Nodes", 0x25C281, mesh_nodes_event_cb);
    MESH_CONN_BUTTON(4, "Share", 0xA78BFA, mesh_channel_event_cb);
    MESH_CONN_BUTTON(5, "Profiles", 0xF59E0B, mesh_channel_profiles_event_cb);
#undef MESH_CONN_BUTTON

    y += (button_cols >= 6 ? button_row_h : button_row_h * 2) + 22;
    status = ui_label(panel, mesh_status_text, &lv_font_montserrat_14,
                      0xCBD5E1);
    lv_obj_set_pos(status, margin, y);
    lv_obj_set_width(status, content_w);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);

    y += ui_is_landscape() ? 70 : 104;
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

    y += button_row_h + 22;
    section = ui_label(panel, "Radio profile", &lv_font_montserrat_18,
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

    y += 12;
    log_title = ui_label(panel, "Event log", &lv_font_montserrat_18,
                         0xF2F5F8);
    lv_obj_set_pos(log_title, margin, y);
    y += 36;
    mesh_log_label = ui_label(panel, mesh_log_text[0] ? mesh_log_text : "Ready",
                              &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(mesh_log_label, margin, y);
    lv_obj_set_width(mesh_log_label, content_w);
    lv_label_set_long_mode(mesh_log_label, LV_LABEL_LONG_WRAP);

    mesh_settings_refresh();
    mesh_refresh_daemon_log();
}

static void mesh_nodes_event_cb(lv_event_t *event)
{
    char response[2048];
    char nodes_text[2048];
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

    (void)event;
    if(mesh_ipc_command("NODES\n", response, sizeof(response)) != 0) {
        ui_trim_text(response);
        mesh_append_log("nodes failed: %s", response);
        return;
    }
    snprintf(nodes_text, sizeof(nodes_text), "%s", response);
    if(strncmp(response, "OK nodes\n", 9) == 0) {
        snprintf(nodes_text, sizeof(nodes_text), "%s", response + 9);
    }
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
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
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    title = ui_label(panel, ui_tr("Nearby nodes"), &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    lv_obj_set_width(title, content_w - 230);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    subtitle = ui_label(panel,
                        ui_tr("Recently heard mesh nodes; tap one for direct messages"),
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

    line = strtok_r(nodes_text, "\n", &saveptr);
    while(line && node_index < MESHTASTIC_UI_NODE_SELECT_MAX) {
        if(strncmp(line, "0x", 2) == 0) {
            int col = shown_count % columns;
            int row = shown_count / columns;
            int x = margin + col * (card_w + gap);
            int card_y = y + row * (card_h + gap);
            mesh_add_node_card(panel, line, x, card_y, card_w, card_h,
                               (size_t)node_index);
            node_index++;
            shown_count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if(shown_count == 0) {
        label = ui_label(panel,
                         nodes_text[0] ? nodes_text : "No nodes seen yet",
                         &lv_font_montserrat_18, 0xCBD5E1);
        lv_obj_set_pos(label, margin, y + 12);
        lv_obj_set_width(label, content_w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
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

    snprintf(command, sizeof(command), "SEND %.220s\n", clean);
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

    mesh_status_panel_h = landscape ? 78 : 88;
    mesh_chat_gap = landscape ? 8 : 10;
    mesh_keyboard_reserved_h = 0;

    mesh_status_panel = ui_panel(mesh_body, x, 0, content_w,
                                 mesh_status_panel_h);
    lv_obj_set_style_bg_color(mesh_status_panel, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_pad_all(mesh_status_panel, 12, 0);

    mesh_status_label = ui_label(mesh_status_panel, "Daemon offline",
                                 &lv_font_montserrat_20, 0xF5A524);
    lv_obj_set_pos(mesh_status_label, 0, 0);
    lv_obj_set_width(mesh_status_label, content_w - 156);
    lv_label_set_long_mode(mesh_status_label, LV_LABEL_LONG_DOT);

    mesh_profile_label = ui_label(mesh_status_panel, "", &lv_font_montserrat_14,
                                  0xCBD5E1);
    lv_obj_set_pos(mesh_profile_label, 0, 28);
    lv_obj_set_width(mesh_profile_label, content_w - 156);
    lv_label_set_long_mode(mesh_profile_label, LV_LABEL_LONG_DOT);

    mesh_detail_label = ui_label(mesh_status_panel, "",
                                 &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_pos(mesh_detail_label, 0, 50);
    lv_obj_set_width(mesh_detail_label, content_w - 156);
    lv_label_set_long_mode(mesh_detail_label, LV_LABEL_LONG_DOT);

    btn = ui_command_button(mesh_status_panel, content_w - 144, 0, 60,
                            LV_SYMBOL_LIST, 0x25C281);
    lv_obj_add_event_cb(btn, mesh_nodes_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_height(btn, 54);
    btn = ui_command_button(mesh_status_panel, content_w - 72, 0, 60,
                            LV_SYMBOL_SETTINGS, 0xA78BFA);
    lv_obj_add_event_cb(btn, mesh_profile_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_height(btn, 54);

    mesh_chat_scroll = ui_panel(mesh_body, x, 0, content_w, 300);
    lv_obj_set_style_bg_color(mesh_chat_scroll, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(mesh_chat_scroll, 12, 0);
    lv_obj_set_style_pad_row(mesh_chat_scroll, 0, 0);
    ui_make_scrollable(mesh_chat_scroll, 20);
    lv_obj_set_flex_flow(mesh_chat_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mesh_chat_scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    mesh_chat_add_empty();

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
    mesh_chat_scroll = NULL;
    mesh_log_label = NULL;
    mesh_send_button = NULL;
    mesh_choice_close();
    mesh_channel_profiles_close();
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_overlay = NULL;
    mesh_close_channel_page();
}

int ui_meshtastic_handle_back(void)
{
    if(mesh_pairing_overlay && lv_obj_is_valid(mesh_pairing_overlay)) {
        mesh_pairing_notice_close_cb(NULL);
        return 1;
    }
    if(mesh_choice_overlay && lv_obj_is_valid(mesh_choice_overlay)) {
        mesh_choice_close();
        return 1;
    }
    if(mesh_channel_profiles_overlay &&
       lv_obj_is_valid(mesh_channel_profiles_overlay)) {
        mesh_channel_profiles_close();
        return 1;
    }
    if(mesh_channel_scan_overlay &&
       lv_obj_is_valid(mesh_channel_scan_overlay)) {
        mesh_channel_scan_overlay_close();
        return 1;
    }
    if(mesh_channel_overlay && lv_obj_is_valid(mesh_channel_overlay)) {
        mesh_close_channel_page();
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
    if(mesh_inline_input && ui_input_inline_is_active(mesh_inline_input)) {
        ui_input_inline_hide(mesh_inline_input);
        return 1;
    }
    return 0;
}
