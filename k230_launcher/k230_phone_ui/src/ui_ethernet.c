#include "ui_ethernet.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *eth_state_label;
static lv_obj_t *eth_ip_label;
static lv_obj_t *eth_carrier_label;
static lv_obj_t *eth_mac_label;
static lv_timer_t *eth_timer;

static void ethernet_update_page(void)
{
    char state[96];
    char ip[64];
    char path[128];
    char carrier[32];
    char mac[64];
    uint32_t color = 0x9AA4AF;

    if(!eth_state_label) {
        return;
    }

    ui_read_iface_state(NET_ETH_IFACE, state, sizeof(state), &color);
    ui_read_iface_ip(NET_ETH_IFACE, ip, sizeof(ip));

    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", NET_ETH_IFACE);
    if(ui_read_file_first_line(path, carrier, sizeof(carrier)) != 0) {
        snprintf(carrier, sizeof(carrier), "--");
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", NET_ETH_IFACE);
    if(ui_read_file_first_line(path, mac, sizeof(mac)) != 0) {
        snprintf(mac, sizeof(mac), "--");
    }

    lv_label_set_text(eth_state_label, state);
    lv_obj_set_style_text_color(eth_state_label, lv_color_hex(color), 0);
    if(eth_ip_label) {
        lv_label_set_text(eth_ip_label, ip);
    }
    if(eth_carrier_label) {
        lv_label_set_text(eth_carrier_label, strcmp(carrier, "1") == 0 ?
                          "Link detected" : "No link");
        lv_obj_set_style_text_color(eth_carrier_label,
                                    lv_color_hex(strcmp(carrier, "1") == 0 ?
                                                 0x25C281 : 0x9AA4AF), 0);
    }
    if(eth_mac_label) {
        lv_label_set_text(eth_mac_label, mac);
    }
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
}

void ui_ethernet_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;

    ui_create_header(scr, "Ethernet");

    body = ui_scroll_panel(scr, 24, ui_page_top_y(154), 520,
                           ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    title = ui_label(body, "Wired connection", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    eth_state_label = ui_label(body, "--", &lv_font_montserrat_22, 0x9AA4AF);
    lv_obj_set_width(eth_state_label, ui_inner_width());
    lv_label_set_long_mode(eth_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(eth_state_label, LV_ALIGN_TOP_LEFT, 0, 56);

    ui_info_row(body, 130, "Interface", NET_ETH_IFACE, 0xF2F5F8);

    eth_ip_label = ui_label(body, "--", &lv_font_montserrat_20, 0x25C281);
    lv_obj_set_width(eth_ip_label, 260);
    lv_label_set_long_mode(eth_ip_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_ip_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_ip_label, LV_ALIGN_TOP_RIGHT, 0, 184);
    ui_label(body, "IPv4", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 186);

    eth_carrier_label = ui_label(body, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_set_width(eth_carrier_label, 260);
    lv_label_set_long_mode(eth_carrier_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_carrier_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_carrier_label, LV_ALIGN_TOP_RIGHT, 0, 238);
    ui_label(body, "Link", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 240);

    eth_mac_label = ui_label(body, "--", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(eth_mac_label, 300);
    lv_label_set_long_mode(eth_mac_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(eth_mac_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(eth_mac_label, LV_ALIGN_TOP_RIGHT, 0, 294);
    ui_label(body, "MAC", &lv_font_montserrat_18, 0x9AA4AF);
    lv_obj_align(lv_obj_get_child(body, lv_obj_get_child_count(body) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 296);

    eth_timer = lv_timer_create(ethernet_timer_cb, 1000, NULL);
    ethernet_update_page();
}
