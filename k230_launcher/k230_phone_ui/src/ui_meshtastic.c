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
#define MESHTASTIC_PREF_REBROADCAST "meshtastic.rebroadcast"
#define MESHTASTIC_DEFAULT_UI_REGION "EU_868"
#define MESHTASTIC_DEFAULT_UI_PRESET "LONG_FAST"

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
static char mesh_status_text[512] = "Not running";
static char mesh_log_text[MESHTASTIC_UI_LOG_MAX];
static char mesh_last_chat_text[3072];
static int mesh_keyboard_reserved_h;
static int mesh_status_panel_h;
static int mesh_chat_gap;
static char mesh_region[24] = MESHTASTIC_DEFAULT_UI_REGION;
static char mesh_preset[32] = MESHTASTIC_DEFAULT_UI_PRESET;
static char mesh_channel_name[64] = "";
static char mesh_psk[80] = "default";
static char mesh_tx_power[8] = "auto";
static char mesh_node_name[48] = "k230-t-display";
static char mesh_from_node[24] = "0";
static char mesh_to_node[24] = "0xffffffff";
static char mesh_hop_limit[8] = "3";
static int mesh_ack_enabled = 1;
static int mesh_rebroadcast_enabled = 0;
static lv_obj_t *mesh_settings_overlay;
static lv_obj_t *mesh_nodes_overlay;
static lv_obj_t *mesh_settings_value_labels[11];

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
    MESH_FIELD_REBROADCAST,
    MESH_FIELD_COUNT,
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
    char text[180];

    snprintf(text, sizeof(text),
             "%s  %s  %s",
             mesh_region, mesh_preset,
             mesh_channel_name[0] ? mesh_channel_name : "default");
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
    {
        char rebroadcast[8];
        ui_prefs_get(MESHTASTIC_PREF_REBROADCAST, rebroadcast,
                     sizeof(rebroadcast), "0");
        mesh_rebroadcast_enabled = strcmp(rebroadcast, "0") != 0;
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
    ui_prefs_set(MESHTASTIC_PREF_REBROADCAST,
                 mesh_rebroadcast_enabled ? "1" : "0");
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
            lv_obj_scroll_to_view(last, LV_ANIM_ON);
        }
    }
}

static void mesh_refresh_chat(void)
{
    char response[3072];
    const char *shown;

    if(!mesh_chat_scroll || !lv_obj_is_valid(mesh_chat_scroll)) {
        return;
    }
    if(mesh_ipc_command("CHAT\n", response, sizeof(response)) != 0) {
        return;
    }
    shown = response;
    if(strncmp(response, "OK chat\n", 8) == 0) {
        shown = response + 8;
    }
    if(strcmp(mesh_last_chat_text, shown) == 0) {
        return;
    }
    snprintf(mesh_last_chat_text, sizeof(mesh_last_chat_text), "%s", shown);
    mesh_chat_rebuild(shown);
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
        char detail[180];
        snprintf(detail, sizeof(detail), "%s -> %s  ACK %s",
                 mesh_node_name,
                 mesh_to_text_is_broadcast(mesh_to_node) ? "broadcast" :
                 mesh_to_node,
                 mesh_ack_enabled ? "on" : "off");
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
    char relay_option[24];
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
    relay_option[0] = '\0';
    if(!mesh_rebroadcast_enabled) {
        snprintf(relay_option, sizeof(relay_option), "--no-rebroadcast ");
    }
    if(mesh_channel_name[0]) {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--channel-name %s --psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, channel_arg, psk_arg,
                 power_option, node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option);
    } else {
        snprintf(command, sizeof(command),
                 "rm -f " MESHTASTIC_SOCKET_PATH "; "
                 "(" MESHTASTIC_PROBE_PATH " --daemon --region %s --preset %s "
                 "--psk %s %s--node %s --from %s --to %s --hop-limit %s %s %s"
                 "> " MESHTASTIC_DAEMON_LOG " 2>&1) &",
                 region_arg, preset_arg, psk_arg, power_option,
                 node_arg, from_arg, to_arg, hop_arg,
                 mesh_ack_enabled ? "--ack" : "--no-ack", relay_option);
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
    case MESH_FIELD_REBROADCAST:
        return "Rebroadcast";
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
    case MESH_FIELD_REBROADCAST:
        snprintf(buf, len, "%s", mesh_rebroadcast_enabled ? "On" : "Off");
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
    case MESH_FIELD_REBROADCAST:
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

    if(field == MESH_FIELD_ACK || field == MESH_FIELD_REBROADCAST) {
        if(field == MESH_FIELD_ACK) {
            mesh_ack_enabled = !mesh_ack_enabled;
        } else {
            mesh_rebroadcast_enabled = !mesh_rebroadcast_enabled;
        }
        mesh_save_profile_prefs();
        mesh_settings_refresh();
        mesh_append_log("settings saved: %s=%s", mesh_setting_name(field),
                        field == MESH_FIELD_ACK ?
                        (mesh_ack_enabled ? "on" : "off") :
                        (mesh_rebroadcast_enabled ? "on" : "off"));
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

static void mesh_close_settings_page(void)
{
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
    button_w = (content_w - button_gap * 3) / 4;
    if(button_w < 86) {
        button_w = 86;
    }
    btn = ui_command_button(panel, margin, y, button_w, "Start", 0x25C281);
    lv_obj_add_event_cb(btn, mesh_start_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, margin + (button_w + button_gap), y,
                            button_w, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(btn, mesh_stop_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, margin + (button_w + button_gap) * 2, y,
                            button_w, "Refresh", 0x3DA5FF);
    lv_obj_add_event_cb(btn, mesh_refresh_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, margin + (button_w + button_gap) * 3, y,
                            button_w, "Nodes", 0x25C281);
    lv_obj_add_event_cb(btn, mesh_nodes_event_cb, LV_EVENT_CLICKED, NULL);

    y += 78;
    status = ui_label(panel, mesh_status_text, &lv_font_montserrat_14,
                      0xCBD5E1);
    lv_obj_set_pos(status, margin, y);
    lv_obj_set_width(status, content_w);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);

    y += ui_is_landscape() ? 70 : 104;
    section = ui_label(panel, "Radio profile", &lv_font_montserrat_18,
                       0xF2F5F8);
    lv_obj_set_pos(section, margin, y);
    y += 42;

    for(int i = 0; i < (int)MESH_FIELD_COUNT; i++) {
        char value[32];
        lv_obj_t *name = ui_label(panel, mesh_setting_name((mesh_setting_field_t)i),
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
                                 (i == (int)MESH_FIELD_ACK ||
                                  i == (int)MESH_FIELD_REBROADCAST) ?
                                 "Toggle" : "Edit",
                                 (i == (int)MESH_FIELD_ACK ||
                                  i == (int)MESH_FIELD_REBROADCAST) ?
                                 0x25C281 :
                                 0x3DA5FF);
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
    const char *shown;
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *label;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int margin = ui_page_side_margin();
    int content_w = screen_w - margin * 2;

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
    title = ui_label(panel, "Meshtastic nodes", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_pos(title, margin, 22);
    btn = ui_command_button(panel, screen_w - margin - 96, 18, 96, "Close",
                            0x374151);
    lv_obj_add_event_cb(btn, mesh_nodes_close_event_cb, LV_EVENT_CLICKED,
                        NULL);

    label = ui_label(panel, shown, &lv_font_montserrat_16, 0xCBD5E1);
    lv_obj_set_pos(label, margin, 76);
    lv_obj_set_width(label, content_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
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
    (void)event;
    if(mesh_inline_input) {
        ui_input_inline_submit(mesh_inline_input);
    }
}

static void mesh_input_focus_event_cb(lv_event_t *event)
{
    (void)event;
    if(mesh_inline_input) {
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
    mesh_last_chat_text[0] = '\0';
    mesh_update_profile_label();

    mesh_layout_main();
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
    if(mesh_settings_overlay && lv_obj_is_valid(mesh_settings_overlay)) {
        lv_obj_delete(mesh_settings_overlay);
    }
    mesh_settings_overlay = NULL;
    memset(mesh_settings_value_labels, 0, sizeof(mesh_settings_value_labels));
    if(mesh_nodes_overlay && lv_obj_is_valid(mesh_nodes_overlay)) {
        lv_obj_delete(mesh_nodes_overlay);
    }
    mesh_nodes_overlay = NULL;
}

int ui_meshtastic_handle_back(void)
{
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
