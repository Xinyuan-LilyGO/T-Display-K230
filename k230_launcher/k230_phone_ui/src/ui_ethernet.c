#include "ui_ethernet.h"

#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ETH_PREF_MODE "ethernet.mode"
#define ETH_PREF_IP "ethernet.static_ip"
#define ETH_PREF_NETMASK "ethernet.netmask"
#define ETH_PREF_GATEWAY "ethernet.gateway"
#define ETH_PREF_DNS "ethernet.dns"

#define ETH_MODE_DHCP "dhcp"
#define ETH_MODE_STATIC "static"
#define ETH_LOG_PATH "/tmp/k230_ethernet_config.log"
#define ETH_STATUS_MAX 160
#define ETH_VALUE_MAX 48
#define ETH_UDHCPC_PID "/var/run/udhcpc." NET_ETH_IFACE ".pid"

typedef enum {
    ETH_FIELD_IP = 0,
    ETH_FIELD_NETMASK,
    ETH_FIELD_GATEWAY,
    ETH_FIELD_DNS,
    ETH_FIELD_COUNT
} ethernet_field_t;

typedef struct {
    char mode[12];
    char ip[ETH_VALUE_MAX];
    char netmask[ETH_VALUE_MAX];
    char gateway[ETH_VALUE_MAX];
    char dns[ETH_VALUE_MAX];
} ethernet_config_t;

static pthread_mutex_t eth_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_obj_t *eth_state_label;
static lv_obj_t *eth_ip_label;
static lv_obj_t *eth_carrier_label;
static lv_obj_t *eth_mac_label;
static lv_obj_t *eth_status_label;
static lv_obj_t *eth_mode_label;
static lv_obj_t *eth_mode_dhcp_btn;
static lv_obj_t *eth_mode_static_btn;
static lv_obj_t *eth_field_value[ETH_FIELD_COUNT];
static lv_obj_t *eth_apply_btn;
static lv_timer_t *eth_timer;
static char eth_status_text[ETH_STATUS_MAX] = "Ready";
static int eth_apply_busy;

static void ethernet_log(const char *fmt, ...)
{
    FILE *fp = fopen(ETH_LOG_PATH, "a");
    va_list ap;

    if(!fp) {
        return;
    }

    fprintf(fp, "[%llu] ", (unsigned long long)ui_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static void ethernet_copy(char *dst, size_t len, const char *src)
{
    if(!dst || len == 0) {
        return;
    }
    snprintf(dst, len, "%s", src ? src : "");
    ui_trim_text(dst);
}

static void ethernet_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&eth_lock);
    va_start(ap, fmt);
    vsnprintf(eth_status_text, sizeof(eth_status_text), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&eth_lock);
}

static int ethernet_pref_get(const char *key, char *buf, size_t len,
                             const char *fallback)
{
    int rc = ui_prefs_get(key, buf, len, fallback);

    (void)len;
    ui_trim_text(buf);
    return rc;
}

static void ethernet_load_config(ethernet_config_t *config)
{
    if(!config) {
        return;
    }

    ethernet_pref_get(ETH_PREF_MODE, config->mode, sizeof(config->mode),
                      ETH_MODE_DHCP);
    if(strcmp(config->mode, ETH_MODE_STATIC) != 0) {
        snprintf(config->mode, sizeof(config->mode), "%s", ETH_MODE_DHCP);
    }
    ethernet_pref_get(ETH_PREF_IP, config->ip, sizeof(config->ip),
                      "192.168.1.120");
    ethernet_pref_get(ETH_PREF_NETMASK, config->netmask,
                      sizeof(config->netmask), "255.255.255.0");
    ethernet_pref_get(ETH_PREF_GATEWAY, config->gateway,
                      sizeof(config->gateway), "192.168.1.1");
    ethernet_pref_get(ETH_PREF_DNS, config->dns, sizeof(config->dns),
                      "8.8.8.8");
}

static void ethernet_save_config(const ethernet_config_t *config)
{
    if(!config) {
        return;
    }

    ui_prefs_set(ETH_PREF_MODE, config->mode);
    ui_prefs_set(ETH_PREF_IP, config->ip);
    ui_prefs_set(ETH_PREF_NETMASK, config->netmask);
    ui_prefs_set(ETH_PREF_GATEWAY, config->gateway);
    ui_prefs_set(ETH_PREF_DNS, config->dns);
}

static int ethernet_ipv4_valid(const char *text, int allow_empty)
{
    int dots = 0;
    int value = -1;
    int digits = 0;
    const char *p = text;

    if(!text || !text[0]) {
        return allow_empty;
    }

    while(*p) {
        unsigned char ch = (unsigned char)*p++;

        if(isdigit(ch)) {
            if(value < 0) {
                value = 0;
            }
            value = value * 10 + (int)(ch - '0');
            digits++;
            if(value > 255 || digits > 3) {
                return 0;
            }
            continue;
        }

        if(ch == '.') {
            if(value < 0 || digits == 0) {
                return 0;
            }
            dots++;
            value = -1;
            digits = 0;
            if(dots > 3) {
                return 0;
            }
            continue;
        }

        return 0;
    }

    return dots == 3 && value >= 0 && digits > 0;
}

static int ethernet_static_config_valid(const ethernet_config_t *config,
                                        char *reason, size_t reason_len)
{
    if(!config) {
        return 0;
    }
    if(!ethernet_ipv4_valid(config->ip, 0)) {
        snprintf(reason, reason_len, "%s", "Invalid IP address");
        return 0;
    }
    if(!ethernet_ipv4_valid(config->netmask, 0)) {
        snprintf(reason, reason_len, "%s", "Invalid netmask");
        return 0;
    }
    if(!ethernet_ipv4_valid(config->gateway, 1)) {
        snprintf(reason, reason_len, "%s", "Invalid gateway");
        return 0;
    }
    if(!ethernet_ipv4_valid(config->dns, 1)) {
        snprintf(reason, reason_len, "%s", "Invalid DNS");
        return 0;
    }
    if(reason && reason_len > 0) {
        reason[0] = '\0';
    }
    return 1;
}

static int ethernet_run(const char *cmd)
{
    int rc;

    ethernet_log("run: %s", cmd);
    rc = system(cmd);
    ethernet_log("rc=%d exit=%d", rc, ui_shell_exit_code(rc));
    return ui_shell_exit_code(rc);
}

static void ethernet_config_to_shell(const ethernet_config_t *config,
                                     char *cmd, size_t cmd_len)
{
    if(strcmp(config->mode, ETH_MODE_STATIC) == 0) {
        snprintf(cmd, cmd_len,
                 "mkdir -p /var/run; "
                 "if [ -f " ETH_UDHCPC_PID " ]; then "
                 "kill $(cat " ETH_UDHCPC_PID ") 2>/dev/null || true; "
                 "rm -f " ETH_UDHCPC_PID "; fi; "
                 "ifconfig " NET_ETH_IFACE " up || exit $?; "
                 "ifconfig " NET_ETH_IFACE " %s netmask %s || exit $?; "
                 "route del default dev " NET_ETH_IFACE " 2>/dev/null || true; "
                 "%s%s%s"
                 "%s%s%s",
                 config->ip, config->netmask,
                 config->gateway[0] ? "route add default gw " : "",
                 config->gateway[0] ? config->gateway : "",
                 config->gateway[0] ? " dev " NET_ETH_IFACE " || exit $?; " : "",
                 config->dns[0] ?
                 "printf 'nameserver " : "",
                 config->dns[0] ? config->dns : "",
                 config->dns[0] ? "\\n' >/etc/resolv.conf" : "true");
    } else {
        snprintf(cmd, cmd_len,
                 "mkdir -p /var/run; "
                 "if [ -f " ETH_UDHCPC_PID " ]; then "
                 "kill $(cat " ETH_UDHCPC_PID ") 2>/dev/null || true; "
                 "rm -f " ETH_UDHCPC_PID "; fi; "
                 "ifconfig " NET_ETH_IFACE " up && "
                 "udhcpc -i " NET_ETH_IFACE " -p " ETH_UDHCPC_PID
                 " -n -q -t 5 -T 2");
    }
}

static int ethernet_apply_config_sync(const ethernet_config_t *config)
{
    char reason[64];
    char cmd[512];

    if(!config) {
        return -1;
    }

    if(strcmp(config->mode, ETH_MODE_STATIC) == 0 &&
       !ethernet_static_config_valid(config, reason, sizeof(reason))) {
        ethernet_set_status("%s", reason);
        ethernet_log("invalid static config: %s", reason);
        return -1;
    }

    ethernet_config_to_shell(config, cmd, sizeof(cmd));
    return ethernet_run(cmd);
}

static void *ethernet_apply_thread_cb(void *arg)
{
    ethernet_config_t *config = (ethernet_config_t *)arg;
    int rc;

    if(!config) {
        return NULL;
    }

    ethernet_set_status(strcmp(config->mode, ETH_MODE_STATIC) == 0 ?
                        "Applying static IP..." : "Requesting DHCP...");
    rc = ethernet_apply_config_sync(config);

    pthread_mutex_lock(&eth_lock);
    eth_apply_busy = 0;
    snprintf(eth_status_text, sizeof(eth_status_text), "%s",
             rc == 0 ? "Ethernet settings applied" :
             "Ethernet apply failed");
    pthread_mutex_unlock(&eth_lock);

    free(config);
    return NULL;
}

static void ethernet_start_apply(int save)
{
    pthread_t thread;
    ethernet_config_t *config;
    char reason[64];

    pthread_mutex_lock(&eth_lock);
    if(eth_apply_busy) {
        pthread_mutex_unlock(&eth_lock);
        ethernet_set_status("Ethernet apply busy");
        return;
    }
    eth_apply_busy = 1;
    pthread_mutex_unlock(&eth_lock);

    config = (ethernet_config_t *)calloc(1, sizeof(*config));
    if(!config) {
        pthread_mutex_lock(&eth_lock);
        eth_apply_busy = 0;
        pthread_mutex_unlock(&eth_lock);
        ethernet_set_status("No memory");
        return;
    }

    ethernet_load_config(config);
    if(strcmp(config->mode, ETH_MODE_STATIC) == 0 &&
       !ethernet_static_config_valid(config, reason, sizeof(reason))) {
        pthread_mutex_lock(&eth_lock);
        eth_apply_busy = 0;
        pthread_mutex_unlock(&eth_lock);
        ethernet_set_status("%s", reason);
        free(config);
        return;
    }

    if(save) {
        ethernet_save_config(config);
    }

    if(pthread_create(&thread, NULL, ethernet_apply_thread_cb, config) != 0) {
        pthread_mutex_lock(&eth_lock);
        eth_apply_busy = 0;
        pthread_mutex_unlock(&eth_lock);
        ethernet_set_status("Ethernet thread failed");
        free(config);
        return;
    }
    pthread_detach(thread);
}

static void ethernet_style_choice_button(lv_obj_t *btn, int selected,
                                         uint32_t accent)
{
    uint32_t children;

    if(!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? accent : 0x202832),
                              0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(selected ? accent : 0x2A3037),
                                  0);
    lv_obj_set_style_border_width(btn, selected ? 0 : 1, 0);

    children = lv_obj_get_child_count(btn);
    for(uint32_t i = 0; i < children; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, i);
        lv_obj_set_style_text_color(child,
                                    lv_color_hex(selected ? 0xFFFFFF : accent),
                                    0);
    }
}

static void ethernet_update_config_ui(void)
{
    ethernet_config_t config;
    char status[ETH_STATUS_MAX];
    int busy;

    ethernet_load_config(&config);

    pthread_mutex_lock(&eth_lock);
    snprintf(status, sizeof(status), "%s", eth_status_text);
    busy = eth_apply_busy;
    pthread_mutex_unlock(&eth_lock);

    if(eth_mode_label) {
        lv_label_set_text(eth_mode_label, ui_tr(
                          strcmp(config.mode, ETH_MODE_STATIC) == 0 ?
                          "Static IP" : "DHCP"));
    }
    ethernet_style_choice_button(eth_mode_dhcp_btn,
                                 strcmp(config.mode, ETH_MODE_DHCP) == 0,
                                 0x3DA5FF);
    ethernet_style_choice_button(eth_mode_static_btn,
                                 strcmp(config.mode, ETH_MODE_STATIC) == 0,
                                 0x25C281);
    if(eth_field_value[ETH_FIELD_IP]) {
        lv_label_set_text(eth_field_value[ETH_FIELD_IP], config.ip);
    }
    if(eth_field_value[ETH_FIELD_NETMASK]) {
        lv_label_set_text(eth_field_value[ETH_FIELD_NETMASK], config.netmask);
    }
    if(eth_field_value[ETH_FIELD_GATEWAY]) {
        lv_label_set_text(eth_field_value[ETH_FIELD_GATEWAY],
                          config.gateway[0] ? config.gateway : "--");
    }
    if(eth_field_value[ETH_FIELD_DNS]) {
        lv_label_set_text(eth_field_value[ETH_FIELD_DNS],
                          config.dns[0] ? config.dns : "--");
    }
    if(eth_status_label) {
        lv_label_set_text(eth_status_label, ui_tr(busy ? "Applying..." : status));
        lv_obj_set_style_text_color(eth_status_label,
                                    lv_color_hex(busy ? 0xF5A524 : 0x9AA4AF),
                                    0);
    }
    if(eth_apply_btn) {
        lv_obj_clear_state(eth_apply_btn, LV_STATE_DISABLED);
        if(busy) {
            lv_obj_add_state(eth_apply_btn, LV_STATE_DISABLED);
        }
    }
}

static void ethernet_update_page(void)
{
    char state[96];
    char ip[64];
    char path[128];
    char mac[64];
    uint32_t color = 0x9AA4AF;
    uint32_t ip_color = 0x9AA4AF;
    int carrier;

    if(!eth_state_label) {
        return;
    }

    ui_read_iface_state(NET_ETH_IFACE, state, sizeof(state), &color);
    carrier = ui_read_iface_carrier(NET_ETH_IFACE);
    if(carrier == 0) {
        snprintf(ip, sizeof(ip), "No link");
        ip_color = 0x9AA4AF;
    } else if(ui_read_iface_ip(NET_ETH_IFACE, ip, sizeof(ip)) == 0) {
        ip_color = 0x25C281;
    } else if(carrier == 1) {
        snprintf(ip, sizeof(ip), "No IP");
        ip_color = 0xF5A524;
    } else {
        snprintf(ip, sizeof(ip), "--");
        ip_color = 0x9AA4AF;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", NET_ETH_IFACE);
    if(ui_read_file_first_line(path, mac, sizeof(mac)) != 0) {
        snprintf(mac, sizeof(mac), "--");
    }

    lv_label_set_text(eth_state_label, state);
    lv_obj_set_style_text_color(eth_state_label, lv_color_hex(color), 0);
    if(eth_ip_label) {
        lv_label_set_text(eth_ip_label, ip);
        lv_obj_set_style_text_color(eth_ip_label, lv_color_hex(ip_color), 0);
    }
    if(eth_carrier_label) {
        lv_label_set_text(eth_carrier_label, carrier == 1 ? "Link detected" :
                          carrier == 0 ? "No link" : "Unknown");
        lv_obj_set_style_text_color(eth_carrier_label,
                                    lv_color_hex(carrier == 1 ? 0x25C281 :
                                                 carrier == 0 ? 0x9AA4AF :
                                                 0xF5A524), 0);
    }
    if(eth_mac_label) {
        lv_label_set_text(eth_mac_label, mac);
    }
    ethernet_update_config_ui();
}

static void ethernet_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ethernet_update_page();
}

void ui_ethernet_cleanup(void)
{
    if(eth_timer) {
        lv_timer_delete(eth_timer);
        eth_timer = NULL;
    }
    eth_state_label = NULL;
    eth_ip_label = NULL;
    eth_carrier_label = NULL;
    eth_mac_label = NULL;
    eth_status_label = NULL;
    eth_mode_label = NULL;
    eth_mode_dhcp_btn = NULL;
    eth_mode_static_btn = NULL;
    eth_apply_btn = NULL;
    memset(eth_field_value, 0, sizeof(eth_field_value));
}

void ui_ethernet_apply_startup(void)
{
    char mode[12];

    if(ui_prefs_get(ETH_PREF_MODE, mode, sizeof(mode), "") != 0 ||
       !mode[0]) {
        return;
    }
    if(strcmp(mode, ETH_MODE_DHCP) != 0 &&
       strcmp(mode, ETH_MODE_STATIC) != 0) {
        return;
    }

    ethernet_log("startup apply mode=%s", mode);
    ethernet_start_apply(0);
}

static void ethernet_mode_event_cb(lv_event_t *event)
{
    const char *mode = (const char *)lv_event_get_user_data(event);
    ethernet_config_t config;

    ethernet_load_config(&config);
    snprintf(config.mode, sizeof(config.mode), "%s",
             mode ? mode : ETH_MODE_DHCP);
    ethernet_save_config(&config);
    ethernet_set_status(strcmp(config.mode, ETH_MODE_STATIC) == 0 ?
                        "Static IP selected" : "DHCP selected");
    ethernet_update_config_ui();
    app_request_fast_refresh();
}

static void ethernet_field_submit_cb(const char *text, void *user_data)
{
    ethernet_field_t field = (ethernet_field_t)(intptr_t)user_data;
    ethernet_config_t config;
    char value[ETH_VALUE_MAX];
    char reason[64];

    ethernet_load_config(&config);
    ethernet_copy(value, sizeof(value), text);

    if((field == ETH_FIELD_IP || field == ETH_FIELD_NETMASK) &&
       !ethernet_ipv4_valid(value, 0)) {
        ethernet_set_status(field == ETH_FIELD_IP ?
                            "Invalid IP address" : "Invalid netmask");
        ethernet_update_config_ui();
        app_request_fast_refresh();
        return;
    }
    if((field == ETH_FIELD_GATEWAY || field == ETH_FIELD_DNS) &&
       !ethernet_ipv4_valid(value, 1)) {
        ethernet_set_status(field == ETH_FIELD_GATEWAY ?
                            "Invalid gateway" : "Invalid DNS");
        ethernet_update_config_ui();
        app_request_fast_refresh();
        return;
    }

    switch(field) {
    case ETH_FIELD_IP:
        snprintf(config.ip, sizeof(config.ip), "%s", value);
        break;
    case ETH_FIELD_NETMASK:
        snprintf(config.netmask, sizeof(config.netmask), "%s", value);
        break;
    case ETH_FIELD_GATEWAY:
        snprintf(config.gateway, sizeof(config.gateway), "%s", value);
        break;
    case ETH_FIELD_DNS:
        snprintf(config.dns, sizeof(config.dns), "%s", value);
        break;
    default:
        return;
    }

    if(strcmp(config.mode, ETH_MODE_STATIC) == 0 &&
       !ethernet_static_config_valid(&config, reason, sizeof(reason))) {
        ethernet_set_status("%s", reason);
    } else {
        ethernet_save_config(&config);
        ethernet_set_status("Saved");
    }

    ethernet_update_config_ui();
    app_request_fast_refresh();
}

static const char *ethernet_field_title(ethernet_field_t field)
{
    switch(field) {
    case ETH_FIELD_IP:
        return "IP address";
    case ETH_FIELD_NETMASK:
        return "Netmask";
    case ETH_FIELD_GATEWAY:
        return "Gateway";
    case ETH_FIELD_DNS:
        return "DNS server";
    default:
        return "Input";
    }
}

static const char *ethernet_field_placeholder(ethernet_field_t field)
{
    switch(field) {
    case ETH_FIELD_IP:
        return "192.168.1.120";
    case ETH_FIELD_NETMASK:
        return "255.255.255.0";
    case ETH_FIELD_GATEWAY:
        return "192.168.1.1";
    case ETH_FIELD_DNS:
        return "8.8.8.8";
    default:
        return "";
    }
}

static void ethernet_edit_field_event_cb(lv_event_t *event)
{
    ethernet_field_t field = (ethernet_field_t)(intptr_t)lv_event_get_user_data(event);
    ethernet_config_t config;
    ui_input_dialog_config_t input_config;
    const char *initial = "";

    ethernet_load_config(&config);
    switch(field) {
    case ETH_FIELD_IP:
        initial = config.ip;
        break;
    case ETH_FIELD_NETMASK:
        initial = config.netmask;
        break;
    case ETH_FIELD_GATEWAY:
        initial = config.gateway;
        break;
    case ETH_FIELD_DNS:
        initial = config.dns;
        break;
    default:
        break;
    }

    memset(&input_config, 0, sizeof(input_config));
    input_config.title = ethernet_field_title(field);
    input_config.placeholder = ethernet_field_placeholder(field);
    input_config.initial_text = initial;
    input_config.max_length = ETH_VALUE_MAX - 1U;
    input_config.submit_cb = ethernet_field_submit_cb;
    input_config.user_data = (void *)(intptr_t)field;
    input_config.submit_text = "Save";
    input_config.cancel_text = "Cancel";
    ui_input_dialog_open(&input_config);
}

static void ethernet_apply_event_cb(lv_event_t *event)
{
    (void)event;
    ethernet_start_apply(1);
    ethernet_update_config_ui();
    app_request_fast_refresh();
}

static lv_obj_t *ethernet_field_row(lv_obj_t *parent, int y,
                                    ethernet_field_t field, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *label_obj;
    lv_obj_t *value;
    lv_obj_t *edit;
    int row_w = ui_fit_width(parent, 0, ui_inner_width());
    int value_w = row_w - 210;

    if(value_w < 150) {
        value_w = 150;
    }

    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, row_w, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151D25), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    label_obj = ui_label(row, name, &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_set_width(label_obj, 118);
    lv_label_set_long_mode(label_obj, LV_LABEL_LONG_DOT);
    lv_obj_align(label_obj, LV_ALIGN_LEFT_MID, 14, 0);

    value = ui_label(row, "--", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(value, value_w);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -78, 0);
    eth_field_value[field] = value;

    edit = ui_command_button(row, row_w - 64, 8, 54, "Edit", 0x3DA5FF);
    lv_obj_set_height(edit, 40);
    lv_obj_set_style_radius(edit, 10, 0);
    lv_obj_add_event_cb(edit, ethernet_edit_field_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)field);

    return row;
}

void ui_ethernet_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *network_title;
    lv_obj_t *config_title;
    lv_obj_t *label_obj;
    int body_x = ui_page_panel_x();
    int body_y = ui_page_top_y(154);
    int body_w = ui_page_panel_width();
    int inner_w = body_w - 32;
    int btn_w = (inner_w - 16) / 2;

    ui_create_header(scr, "Ethernet");

    body = ui_scroll_panel(scr, body_x, body_y, body_w,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    title = ui_label(body, "Wired connection", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    subtitle = ui_label(body, "DHCP or manual IPv4 settings",
                        &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(subtitle, inner_w);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 0, 34);

    eth_state_label = ui_label(body, "--", &lv_font_montserrat_22, 0x9AA4AF);
    lv_obj_set_width(eth_state_label, inner_w);
    lv_label_set_long_mode(eth_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(eth_state_label, LV_ALIGN_TOP_LEFT, 0, 72);

    network_title = ui_label(body, "Current link", &lv_font_montserrat_20,
                             0xF2F5F8);
    lv_obj_align(network_title, LV_ALIGN_TOP_LEFT, 0, 126);

    ui_info_row(body, 164, "Interface", NET_ETH_IFACE, 0xF2F5F8);

    eth_ip_label = ui_label(body, "--", &lv_font_montserrat_20, 0x25C281);
    lv_obj_set_width(eth_ip_label, inner_w - 170);
    lv_label_set_long_mode(eth_ip_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_ip_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_ip_label, LV_ALIGN_TOP_RIGHT, 0, 218);
    label_obj = ui_label(body, "IPv4", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(label_obj, LV_ALIGN_TOP_LEFT, 0, 220);

    eth_carrier_label = ui_label(body, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_set_width(eth_carrier_label, inner_w - 170);
    lv_label_set_long_mode(eth_carrier_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_carrier_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_carrier_label, LV_ALIGN_TOP_RIGHT, 0, 272);
    label_obj = ui_label(body, "Link", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(label_obj, LV_ALIGN_TOP_LEFT, 0, 274);

    eth_mac_label = ui_label(body, "--", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(eth_mac_label, inner_w - 170);
    lv_label_set_long_mode(eth_mac_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_mac_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_mac_label, LV_ALIGN_TOP_RIGHT, 0, 328);
    label_obj = ui_label(body, "MAC", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(label_obj, LV_ALIGN_TOP_LEFT, 0, 330);

    config_title = ui_label(body, "IP assignment", &lv_font_montserrat_20,
                            0xF2F5F8);
    lv_obj_align(config_title, LV_ALIGN_TOP_LEFT, 0, 398);

    eth_mode_label = ui_label(body, "--", &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_width(eth_mode_label, inner_w - 180);
    lv_label_set_long_mode(eth_mode_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_mode_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_mode_label, LV_ALIGN_TOP_RIGHT, 0, 400);

    eth_mode_dhcp_btn = ui_command_button(body, 0, 442, btn_w, "DHCP",
                                          0x3DA5FF);
    lv_obj_set_height(eth_mode_dhcp_btn, 52);
    lv_obj_add_event_cb(eth_mode_dhcp_btn, ethernet_mode_event_cb,
                        LV_EVENT_CLICKED, (void *)ETH_MODE_DHCP);

    eth_mode_static_btn = ui_command_button(body, btn_w + 16, 442, btn_w,
                                            "Static IP", 0x25C281);
    lv_obj_set_height(eth_mode_static_btn, 52);
    lv_obj_add_event_cb(eth_mode_static_btn, ethernet_mode_event_cb,
                        LV_EVENT_CLICKED, (void *)ETH_MODE_STATIC);

    ethernet_field_row(body, 520, ETH_FIELD_IP, "IP address");
    ethernet_field_row(body, 584, ETH_FIELD_NETMASK, "Netmask");
    ethernet_field_row(body, 648, ETH_FIELD_GATEWAY, "Gateway");
    ethernet_field_row(body, 712, ETH_FIELD_DNS, "DNS server");

    eth_apply_btn = ui_command_button(body, 0, 796, inner_w,
                                      "Apply settings", 0x25C281);
    lv_obj_set_height(eth_apply_btn, 56);
    lv_obj_set_style_radius(eth_apply_btn, 12, 0);
    lv_obj_add_event_cb(eth_apply_btn, ethernet_apply_event_cb,
                        LV_EVENT_CLICKED, NULL);

    eth_status_label = ui_label(body, "Ready", &lv_font_montserrat_16,
                                0x9AA4AF);
    lv_obj_set_width(eth_status_label, inner_w);
    lv_label_set_long_mode(eth_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(eth_status_label, LV_ALIGN_TOP_LEFT, 0, 868);

    eth_timer = lv_timer_create(ethernet_timer_cb, 1000, NULL);
    ethernet_update_page();
}
