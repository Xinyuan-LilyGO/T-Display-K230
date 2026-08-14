#include "ui_meshtastic.h"

#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define MESHTASTIC_PROBE_PATH "/root/app/k230_phone_ui/k230_meshtastic_probe"
#define MESHTASTIC_SOCKET_PATH "/tmp/k230_meshtastic.sock"
#define MESHTASTIC_DAEMON_LOG "/tmp/k230_meshtastic_daemon_ui.log"
#define MESHTASTIC_UI_LOG_MAX 4096
#define MESHTASTIC_PREF_REGION "meshtastic.region"
#define MESHTASTIC_PREF_PRESET "meshtastic.preset"
#define MESHTASTIC_PREF_CHANNEL "meshtastic.channel"
#define MESHTASTIC_PREF_PSK "meshtastic.psk"
#define MESHTASTIC_PREF_POWER "meshtastic.power"
#define MESHTASTIC_PREF_NODE "meshtastic.node"
#define MESHTASTIC_PREF_FROM "meshtastic.from"
#define MESHTASTIC_PREF_TO "meshtastic.to"
#define MESHTASTIC_PREF_HOP "meshtastic.hop"
#define MESHTASTIC_PREF_ACK "meshtastic.ack"

static lv_obj_t *mesh_status_label;
static lv_obj_t *mesh_detail_label;
static lv_obj_t *mesh_profile_label;
static lv_obj_t *mesh_chat_label;
static lv_obj_t *mesh_log_label;
static lv_obj_t *mesh_send_button;
static lv_timer_t *mesh_timer;
static char mesh_status_text[512] = "Not running";
static char mesh_log_text[MESHTASTIC_UI_LOG_MAX];
static char mesh_region[24] = "US";
static char mesh_preset[32] = "LONG_FAST";
static char mesh_channel_name[64] = "";
static char mesh_psk[80] = "default";
static char mesh_tx_power[8] = "auto";
static char mesh_node_name[48] = "k230-t-display";
static char mesh_from_node[24] = "0";
static char mesh_to_node[24] = "0xffffffff";
static char mesh_hop_limit[8] = "3";
static int mesh_ack_enabled = 1;
static lv_obj_t *mesh_settings_overlay;
static lv_obj_t *mesh_settings_value_labels[10];

typedef enum {
    MESH_FIELD_REGION = 0,
    MESH_FIELD_PRESET,
    MESH_FIELD_CHANNEL,
    MESH_FIELD_PSK,
    MESH_FIELD_POWER,
    MESH_FIELD_NODE,
    MESH_FIELD_FROM,
    MESH_FIELD_TO,
    MESH_FIELD_HOP,
    MESH_FIELD_ACK,
} mesh_setting_field_t;

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
           strcasecmp(text, "0xffffffff") == 0;
}

static void mesh_update_profile_label(void)
{
    char text[420];

    snprintf(text, sizeof(text),
             "Profile %s / %s / channel=%s / psk=%s / power=%s\nNode %s / from=%s to=%s hop=%s ack=%s",
             mesh_region, mesh_preset,
             mesh_channel_name[0] ? mesh_channel_name : "<preset>",
             mesh_psk, mesh_tx_power, mesh_node_name,
             mesh_from_text_is_auto(mesh_from_node) ? "auto" : mesh_from_node,
             mesh_to_text_is_broadcast(mesh_to_node) ? "broadcast" : mesh_to_node,
             mesh_hop_limit, mesh_ack_enabled ? "on" : "off");
    if(mesh_profile_label && lv_obj_is_valid(mesh_profile_label)) {
        lv_label_set_text(mesh_profile_label, text);
    }
}

static void mesh_load_profile_prefs(void)
{
    ui_prefs_get(MESHTASTIC_PREF_REGION, mesh_region, sizeof(mesh_region),
                 "US");
    ui_prefs_get(MESHTASTIC_PREF_PRESET, mesh_preset, sizeof(mesh_preset),
                 "LONG_FAST");
    ui_prefs_get(MESHTASTIC_PREF_CHANNEL, mesh_channel_name,
                 sizeof(mesh_channel_name), "");
    ui_prefs_get(MESHTASTIC_PREF_PSK, mesh_psk, sizeof(mesh_psk), "default");
    ui_prefs_get(MESHTASTIC_PREF_POWER, mesh_tx_power,
                 sizeof(mesh_tx_power), "auto");
    ui_prefs_get(MESHTASTIC_PREF_NODE, mesh_node_name,
                 sizeof(mesh_node_name), "k230-t-display");
    ui_prefs_get(MESHTASTIC_PREF_FROM, mesh_from_node,
                 sizeof(mesh_from_node), "0");
    ui_prefs_get(MESHTASTIC_PREF_TO, mesh_to_node,
                 sizeof(mesh_to_node), "0xffffffff");
    ui_prefs_get(MESHTASTIC_PREF_HOP, mesh_hop_limit,
                 sizeof(mesh_hop_limit), "3");
    {
        char ack[8];
        ui_prefs_get(MESHTASTIC_PREF_ACK, ack, sizeof(ack), "1");
        mesh_ack_enabled = strcmp(ack, "0") != 0;
    }
    mesh_normalize_power();
    mesh_normalize_hop();
}

static void mesh_save_profile_prefs(void)
{
    ui_prefs_set(MESHTASTIC_PREF_REGION, mesh_region);
    ui_prefs_set(MESHTASTIC_PREF_PRESET, mesh_preset);
    ui_prefs_set(MESHTASTIC_PREF_CHANNEL, mesh_channel_name);
    ui_prefs_set(MESHTASTIC_PREF_PSK, mesh_psk);
    ui_prefs_set(MESHTASTIC_PREF_POWER, mesh_tx_power);
    ui_prefs_set(MESHTASTIC_PREF_NODE, mesh_node_name);
    ui_prefs_set(MESHTASTIC_PREF_FROM, mesh_from_node);
    ui_prefs_set(MESHTASTIC_PREF_TO, mesh_to_node);
    ui_prefs_set(MESHTASTIC_PREF_HOP, mesh_hop_limit);
    ui_prefs_set(MESHTASTIC_PREF_ACK, mesh_ack_enabled ? "1" : "0");
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

static void mesh_refresh_chat(void)
{
    char response[3072];
    const char *shown;

    if(!mesh_chat_label || !lv_obj_is_valid(mesh_chat_label)) {
        return;
    }
    if(mesh_ipc_command("CHAT\n", response, sizeof(response)) != 0) {
        return;
    }
    shown = response;
    if(strncmp(response, "OK chat\n", 8) == 0) {
        shown = response + 8;
    }
    lv_label_set_text(mesh_chat_label, shown);
}

static void mesh_refresh_status(void)
{
    char response[512];
    int online;

    if(mesh_ipc_command("STATUS\n", response, sizeof(response)) == 0) {
        snprintf(mesh_status_text, sizeof(mesh_status_text), "%s", response);
    } else {
        snprintf(mesh_status_text, sizeof(mesh_status_text), "%s", response);
    }
    ui_trim_text(mesh_status_text);
    online = mesh_status_is_online(mesh_status_text);

    if(mesh_status_label && lv_obj_is_valid(mesh_status_label)) {
        lv_label_set_text(mesh_status_label,
                          online ? ui_tr("Daemon online") :
                          ui_tr("Daemon offline"));
        lv_obj_set_style_text_color(mesh_status_label,
                                    lv_color_hex(online ? 0x25C281 : 0xF5A524),
                                    0);
    }
    if(mesh_detail_label && lv_obj_is_valid(mesh_detail_label)) {
        lv_label_set_text(mesh_detail_label, mesh_status_text);
    }
    if(mesh_send_button && lv_obj_is_valid(mesh_send_button)) {
        if(online) {
            lv_obj_clear_state(mesh_send_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(mesh_send_button, LV_STATE_DISABLED);
        }
    }
    if(online) {
        mesh_refresh_chat();
        mesh_refresh_daemon_log();
    }
}

static void mesh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    mesh_refresh_status();
}

static void mesh_start_event_cb(lv_event_t *event)
{
    char region_arg[32];
    char preset_arg[40];
    char channel_arg[80];
    char psk_arg[96];
    char power_arg[16];
    char power_option[32];
    char node_arg[64];
    char from_arg[32];
    char to_arg[32];
    char hop_arg[16];
    char command[1040];
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
        mesh_refresh_status();
        return;
    }

    mesh_safe_arg(region_arg, sizeof(region_arg), mesh_region);
    mesh_safe_arg(preset_arg, sizeof(preset_arg), mesh_preset);
    mesh_safe_arg(channel_arg, sizeof(channel_arg), mesh_channel_name);
    mesh_safe_arg(psk_arg, sizeof(psk_arg), mesh_psk);
    mesh_normalize_power();
    power_option[0] = '\0';
    if(!mesh_power_is_auto()) {
        mesh_safe_or_default(power_arg, sizeof(power_arg), mesh_tx_power,
                             "17");
        snprintf(power_option, sizeof(power_option), "--power %s ",
                 power_arg);
    }
    mesh_safe_or_default(node_arg, sizeof(node_arg), mesh_node_name,
                         "k230-t-display");
    mesh_safe_or_default(from_arg, sizeof(from_arg), mesh_from_node, "0");
    mesh_safe_or_default(to_arg, sizeof(to_arg), mesh_to_node, "0xffffffff");
    mesh_normalize_hop();
    mesh_safe_or_default(hop_arg, sizeof(hop_arg), mesh_hop_limit, "3");
    if(mesh_channel_name[0]) {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--channel-name %s --psk %s %s--node %s --from %s --to %s --hop-limit %s %s "
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, channel_arg, psk_arg,
                 power_option, node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack");
    } else {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--psk %s %s--node %s --from %s --to %s --hop-limit %s %s "
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, psk_arg, power_option,
                 node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack");
    }
    rc = system(command);
    mesh_append_log("start daemon rc=%d log=%s", ui_shell_exit_code(rc),
                    MESHTASTIC_DAEMON_LOG);
    usleep(250000);
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

static const char *mesh_setting_name(mesh_setting_field_t field)
{
    switch(field) {
    case MESH_FIELD_REGION:
        return "Region";
    case MESH_FIELD_PRESET:
        return "Preset";
    case MESH_FIELD_CHANNEL:
        return "Channel";
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
    default:
        return "Setting";
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
    default:
        return "";
    }
}

static void mesh_settings_refresh(void)
{
    for(int i = 0; i < (int)MESH_FIELD_ACK + 1; i++) {
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
        mesh_safe_or_default(mesh_region, sizeof(mesh_region), text, "US");
        break;
    case MESH_FIELD_PRESET:
        mesh_safe_or_default(mesh_preset, sizeof(mesh_preset), text,
                             "LONG_FAST");
        break;
    case MESH_FIELD_CHANNEL:
        if(!text[0] || strcmp(text, "-") == 0 ||
           strcmp(text, "default") == 0) {
            mesh_channel_name[0] = '\0';
        } else {
            mesh_safe_arg(mesh_channel_name, sizeof(mesh_channel_name), text);
        }
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
    default:
        return;
    }

    mesh_save_profile_prefs();
    mesh_settings_refresh();
    mesh_append_log("settings saved: %s=%s", mesh_setting_name(field),
                    mesh_setting_value(field, tmp, sizeof(tmp)));
}

static void mesh_setting_edit_event_cb(lv_event_t *event)
{
    mesh_setting_field_t field =
        (mesh_setting_field_t)(intptr_t)lv_event_get_user_data(event);
    ui_input_dialog_config_t config;
    char placeholder[96];
    char value[32];

    if(field == MESH_FIELD_ACK) {
        mesh_ack_enabled = !mesh_ack_enabled;
        mesh_save_profile_prefs();
        mesh_settings_refresh();
        mesh_append_log("settings saved: ACK=%s",
                        mesh_ack_enabled ? "on" : "off");
        return;
    }

    snprintf(placeholder, sizeof(placeholder), "%s",
             mesh_setting_value(field, value, sizeof(value)));
    if(field == MESH_FIELD_CHANNEL && strcmp(placeholder, "<preset>") == 0) {
        snprintf(placeholder, sizeof(placeholder), "-");
    }

    memset(&config, 0, sizeof(config));
    config.title = mesh_setting_name(field);
    config.placeholder = placeholder;
    config.password_mode = field == MESH_FIELD_PSK;
    config.max_length = field == MESH_FIELD_PSK ? 80 : 64;
    config.submit_cb = mesh_setting_submit_cb;
    config.user_data = (void *)(intptr_t)field;
    config.submit_text = "Save";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void mesh_settings_close_event_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
}

static void mesh_profile_event_cb(lv_event_t *event)
{
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int panel_w = landscape ? screen_w - 80 : screen_w - 40;
    int panel_h = landscape ? screen_h - 48 : screen_h - 80;
    int row_h = 56;
    int y = 58;

    (void)event;
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));

    mesh_settings_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mesh_settings_overlay, screen_w, screen_h);
    lv_obj_set_style_bg_color(mesh_settings_overlay, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(mesh_settings_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(mesh_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);

    panel = ui_scroll_panel(mesh_settings_overlay,
                            (screen_w - panel_w) / 2,
                            (screen_h - panel_h) / 2,
                            panel_w, panel_h);
    title = ui_label(panel, "Meshtastic settings", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    btn = ui_command_button(panel, panel_w - 124, 0, 96, "Close", 0x374151);
    lv_obj_add_event_cb(btn, mesh_settings_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    for(int i = 0; i <= (int)MESH_FIELD_ACK; i++) {
        char value[32];
        lv_obj_t *name = ui_label(panel, mesh_setting_name((mesh_setting_field_t)i),
                                  &lv_font_montserrat_16, 0x9AA4AF);
        lv_obj_t *edit;
        lv_obj_set_pos(name, 0, y + 4);
        mesh_settings_value_labels[i] =
            ui_label(panel,
                     mesh_setting_value((mesh_setting_field_t)i, value,
                                        sizeof(value)),
                     &lv_font_montserrat_18, 0xF2F5F8);
        lv_obj_set_pos(mesh_settings_value_labels[i], 150, y + 2);
        lv_obj_set_width(mesh_settings_value_labels[i], panel_w - 300);
        lv_label_set_long_mode(mesh_settings_value_labels[i],
                               LV_LABEL_LONG_DOT);
        edit = ui_command_button(panel, panel_w - 124, y - 4, 96,
                                 i == (int)MESH_FIELD_ACK ? "Toggle" : "Edit",
                                 i == (int)MESH_FIELD_ACK ? 0x25C281 :
                                 0x3DA5FF);
        lv_obj_add_event_cb(edit, mesh_setting_edit_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        y += row_h;
    }

    mesh_settings_refresh();
}

static void mesh_nodes_event_cb(lv_event_t *event)
{
    char response[2048];
    const char *shown;

    (void)event;
    if(mesh_ipc_command("NODES\n", response, sizeof(response)) != 0) {
        ui_trim_text(response);
        mesh_append_log("nodes failed: %s", response);
        return;
    }
    shown = response;
    if(strncmp(response, "OK nodes\n", 9) == 0) {
        shown = response + 9;
    }
    mesh_append_log("nodes:\n%s", shown);
}

static void mesh_send_submit_cb(const char *text, void *user_data)
{
    char command[320];
    char response[256];

    (void)user_data;
    if(!text || !text[0]) {
        mesh_append_log("send skipped: empty message");
        return;
    }
    snprintf(command, sizeof(command), "SEND %.220s\n", text);
    if(mesh_ipc_command(command, response, sizeof(response)) == 0) {
        ui_trim_text(response);
        mesh_append_log("send: %s", response);
    } else {
        ui_trim_text(response);
        mesh_append_log("send failed: %s", response);
    }
    mesh_refresh_status();
}

static void mesh_send_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;

    (void)event;
    memset(&config, 0, sizeof(config));
    config.title = "Meshtastic message";
    config.placeholder = "Type a text message";
    config.password_mode = 0;
    config.max_length = 220;
    config.submit_cb = mesh_send_submit_cb;
    config.submit_text = "Send";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
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

void ui_meshtastic_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *status_panel;
    lv_obj_t *chat_panel;
    lv_obj_t *log_panel;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int w = ui_screen_width();
    int content_w = ui_content_width();
    int col_gap = 16;
    int left_w = landscape ? (w - 64 - col_gap) / 2 : 520;
    int right_w = landscape ? left_w : 520;
    int x_right = landscape ? 24 + left_w + col_gap : 24;
    int chat_y = landscape ? 0 : 250;
    int log_y = landscape ? 250 : 500;

    mesh_load_profile_prefs();
    ui_create_header(scr, "Meshtastic");
    body = ui_page_body(scr, 150);

    if(!landscape) {
        content_w = 568;
    }
    (void)content_w;

    status_panel = ui_panel(body, 24, 0, left_w, 226);
    mesh_panel_title(status_panel, "Mesh daemon",
                     "US LongFast socket bridge for SX1262/LR2021");
    mesh_status_label = ui_label(status_panel, "Daemon offline",
                                 &lv_font_montserrat_24, 0xF5A524);
    lv_obj_align(mesh_status_label, LV_ALIGN_TOP_LEFT, 0, 82);

    mesh_profile_label = ui_label(status_panel, "",
                                  &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(mesh_profile_label, left_w - 32);
    lv_label_set_long_mode(mesh_profile_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(mesh_profile_label, LV_ALIGN_TOP_LEFT, 0, 66);
    mesh_update_profile_label();

    mesh_detail_label = ui_label(status_panel, "Not running",
                                 &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_width(mesh_detail_label, left_w - 32);
    lv_label_set_long_mode(mesh_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(mesh_detail_label, LV_ALIGN_TOP_LEFT, 0, 120);

    btn = ui_command_button(status_panel, 0, 154, 112, "Start", 0x25C281);
    lv_obj_add_event_cb(btn, mesh_start_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(status_panel, 124, 154, 112, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(btn, mesh_stop_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(status_panel, 248, 154, 112, "Refresh", 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_refresh_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(status_panel, 372, 154, 112, "Profile", 0x7C3AED);
    lv_obj_add_event_cb(btn, mesh_profile_event_cb, LV_EVENT_CLICKED, NULL);

    chat_panel = ui_panel(body, x_right, chat_y, right_w, 226);
    mesh_panel_title(chat_panel, "Chat", "Decoded TX/RX text cache");
    mesh_send_button = ui_command_button(chat_panel, 0, 92, 220,
                                         "Send Message", 0x7C3AED);
    lv_obj_add_event_cb(mesh_send_button, mesh_send_event_cb,
                        LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(chat_panel, 240, 92, 104, "Status", 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_refresh_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(chat_panel, 356, 92, 104, "Nodes", 0x25C281);
    lv_obj_add_event_cb(btn, mesh_nodes_event_cb, LV_EVENT_CLICKED, NULL);

    mesh_chat_label = ui_label(chat_panel, "No mesh messages yet",
                               &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(mesh_chat_label, 0, 144);
    lv_obj_set_width(mesh_chat_label, right_w - 32);
    lv_label_set_long_mode(mesh_chat_label, LV_LABEL_LONG_WRAP);

    log_panel = ui_scroll_panel(body, landscape ? 24 : 24, log_y,
                                landscape ? w - 48 : 520,
                                landscape ? 250 : 420);
    mesh_panel_title(log_panel, "Event log", NULL);
    mesh_log_label = ui_label(log_panel, "Ready", &lv_font_montserrat_16,
                              0xCBD5E1);
    lv_obj_set_pos(mesh_log_label, 0, 42);
    lv_obj_set_width(mesh_log_label, landscape ? w - 96 : 488);
    lv_label_set_long_mode(mesh_log_label, LV_LABEL_LONG_WRAP);

    if(mesh_log_text[0]) {
        lv_label_set_text(mesh_log_label, mesh_log_text);
    } else {
        snprintf(mesh_log_text, sizeof(mesh_log_text), "%s\n",
                 "Meshtastic UI ready");
        lv_label_set_text(mesh_log_label, mesh_log_text);
    }

    mesh_refresh_status();
    mesh_timer = lv_timer_create(mesh_timer_cb, 2000, NULL);
}

void ui_meshtastic_cleanup(void)
{
    if(mesh_timer) {
        lv_timer_delete(mesh_timer);
        mesh_timer = NULL;
    }
    mesh_status_label = NULL;
    mesh_detail_label = NULL;
    mesh_profile_label = NULL;
    mesh_chat_label = NULL;
    mesh_log_label = NULL;
    mesh_send_button = NULL;
}
