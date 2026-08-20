#include "ui_icons.h"

#define UI_ICON_BASE "/root/app/k230_phone_ui/icons/app/"
#define UI_ICON_FILE(name) UI_ICON_BASE name ".png"

const char *ui_icon_path_for_page(page_id_t page)
{
    switch(page) {
    case PAGE_CAMERA:
        return UI_ICON_FILE("camera");
    case PAGE_NETWORK:
        return UI_ICON_FILE("network");
    case PAGE_WIFI:
        return UI_ICON_FILE("wifi");
    case PAGE_WIFI_IPERF:
        return UI_ICON_FILE("wifi_test");
    case PAGE_ETHERNET:
        return UI_ICON_FILE("ethernet");
    case PAGE_BLE:
        return UI_ICON_FILE("bluetooth");
    case PAGE_MUSIC:
        return UI_ICON_FILE("music");
    case PAGE_VIDEO:
        return UI_ICON_FILE("video");
    case PAGE_NET_RADIO:
        return UI_ICON_FILE("radio");
    case PAGE_RECORDER:
        return UI_ICON_FILE("record");
    case PAGE_MIC_FFT:
        return UI_ICON_FILE("mic_fft");
    case PAGE_LORA:
        return UI_ICON_FILE("lora");
    case PAGE_MESHTASTIC:
        return UI_ICON_FILE("meshtastic");
    case PAGE_LORA_FLRC:
        return UI_ICON_FILE("lora_flrc");
    case PAGE_HALOW:
        return UI_ICON_FILE("halow");
    case PAGE_LORAWAN:
        return UI_ICON_FILE("lorawan");
    case PAGE_NES:
        return UI_ICON_FILE("nes");
    case PAGE_SYSTEM:
        return UI_ICON_FILE("system");
    case PAGE_DISPLAY:
        return UI_ICON_FILE("display");
    case PAGE_DISPLAY_TEST:
        return UI_ICON_FILE("screen_test");
    case PAGE_LANGUAGE:
        return UI_ICON_FILE("language");
    case PAGE_TIME:
        return UI_ICON_FILE("time");
    case PAGE_AUDIO_SETTINGS:
    case PAGE_AUDIO_OUTPUT:
        return UI_ICON_FILE("audio");
    case PAGE_NOTIFICATION_SETTINGS:
        return UI_ICON_FILE("notifications");
    case PAGE_APP_STARTUP:
        return UI_ICON_FILE("startup");
    case PAGE_I2S_TEST:
        return UI_ICON_FILE("i2s");
    case PAGE_I2C_SCAN:
        return UI_ICON_FILE("i2c");
    case PAGE_HDMI_TEST:
        return UI_ICON_FILE("hdmi");
    case PAGE_FAN:
        return UI_ICON_FILE("fan");
    case PAGE_SENSORS:
        return UI_ICON_FILE("sensors");
    case PAGE_BQ25896:
        return UI_ICON_FILE("charger");
    case PAGE_BATTERY:
        return UI_ICON_FILE("battery");
    case PAGE_KEYBOARD_SETTINGS:
    case PAGE_KEYBOARD_TEST:
        return UI_ICON_FILE("keyboard");
    case PAGE_BUTTON_TEST:
    case PAGE_INT0_TEST:
    case PAGE_TOUCH_TEST:
        return UI_ICON_FILE("touch");
    case PAGE_XL9555_TEST:
        return UI_ICON_FILE("led");
    case PAGE_CELLULAR:
        return UI_ICON_FILE("cellular");
    case PAGE_USB_MODEM:
        return UI_ICON_FILE("usb_modem");
    case PAGE_FILES:
        return UI_ICON_FILE("mtp");
    case PAGE_AI:
        return UI_ICON_FILE("ai");
    case PAGE_RTSP:
        return UI_ICON_FILE("rtsp");
    case PAGE_TERMINAL:
        return UI_ICON_FILE("terminal");
    case PAGE_ABOUT:
        return UI_ICON_FILE("about");
    case PAGE_GALLERY:
        return UI_ICON_FILE("gallery");
    case PAGE_SCREENSHOT:
        return UI_ICON_FILE("screenshot");
    case PAGE_MOTION:
        return UI_ICON_FILE("motion");
    case PAGE_SETTINGS:
        return UI_ICON_FILE("settings");
    case PAGE_LOGS:
        return UI_ICON_FILE("logs");
    case PAGE_REBOOT:
        return UI_ICON_FILE("reboot");
    case PAGE_NRF52840_DFU:
        return UI_ICON_FILE("dfu");
    case PAGE_HOME:
    default:
        return NULL;
    }
}

int ui_page_has_asset_icon(page_id_t page)
{
    const char *path = ui_icon_path_for_page(page);

    return path && ui_path_exists(path);
}

void ui_style_icon_box_for_page(lv_obj_t *box, page_id_t page,
                                uint32_t fallback_color)
{
    if(ui_page_has_asset_icon(page)) {
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_shadow_width(box, 0, 0);
        return;
    }

    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(fallback_color), 0);
    lv_obj_set_style_border_width(box, 0, 0);
}

lv_obj_t *ui_create_page_icon(lv_obj_t *parent, page_id_t page,
                              const char *fallback_symbol,
                              const lv_font_t *fallback_font,
                              uint32_t fallback_color, int icon_px)
{
    const char *path = ui_icon_path_for_page(page);

    if(path && ui_path_exists(path)) {
        lv_obj_t *img = lv_image_create(parent);
        int scale = icon_px > 0 ? (icon_px * 256) / 96 : 256;

        if(scale < 1) {
            scale = 1;
        }

        lv_image_set_src(img, path);
        lv_image_set_scale(img, scale);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        ui_make_click_forwarder(img);
        return img;
    }

    lv_obj_t *icon = ui_label(parent, fallback_symbol ? fallback_symbol : "",
                              fallback_font, fallback_color);
    lv_obj_center(icon);
    ui_make_click_forwarder(icon);
    return icon;
}
