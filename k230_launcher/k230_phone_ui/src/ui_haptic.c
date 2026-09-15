#include "ui_haptic.h"

#include "ui_common.h"
#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_prefs.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRV2605_ADDR 0x5A
#define DRV2605_REG_STATUS 0x00
#define DRV2605_REG_MODE 0x01
#define DRV2605_REG_RTPIN 0x02
#define DRV2605_REG_LIBRARY 0x03
#define DRV2605_REG_WAVESEQ1 0x04
#define DRV2605_REG_WAVESEQ2 0x05
#define DRV2605_REG_GO 0x0C
#define DRV2605_REG_OVERDRIVE 0x0D
#define DRV2605_REG_SUSTAINPOS 0x0E
#define DRV2605_REG_SUSTAINNEG 0x0F
#define DRV2605_REG_BREAK 0x10
#define DRV2605_REG_AUDIOMAX 0x13
#define DRV2605_REG_FEEDBACK 0x1A
#define DRV2605_REG_CONTROL3 0x1D

#define HAPTIC_PREF_EFFECT "haptic.effect"
#define HAPTIC_PREF_KEYBOARD "haptic.keyboard_enabled"
#define HAPTIC_PREF_TOUCH "haptic.touch_enabled"
#define HAPTIC_LOG "/tmp/k230_haptic.log"
#define HAPTIC_PROBE_RETRY_US 5000000ULL
#define HAPTIC_MIN_PLAY_INTERVAL_US 35000ULL

typedef struct {
    int id;
    const char *name;
    const char *hint;
} haptic_effect_def_t;

static const haptic_effect_def_t haptic_effects[] = {
    { 1, "Strong click", "Short firm tap" },
    { 2, "Light click", "Softer tap" },
    { 10, "Double click", "Two quick taps" },
    { 14, "Soft buzz", "Longer feedback" },
    { 47, "Alert buzz", "Strong alert" },
    { 83, "Smooth ramp", "Gentle transition" },
};

static int haptic_prefs_loaded;
static int haptic_effect_id = 1;
static int haptic_keyboard_on;
static int haptic_touch_on;
static int haptic_available_cache = -1;
static uint64_t haptic_next_probe_us;
static uint64_t haptic_last_play_us;
static char haptic_status_text[96] = "Not checked";
static lv_obj_t *haptic_status_label;
static lv_obj_t *haptic_effect_label;
static lv_obj_t *haptic_keyboard_switch;
static lv_obj_t *haptic_touch_switch;
static lv_obj_t *haptic_effect_btn[sizeof(haptic_effects) /
                                   sizeof(haptic_effects[0])];

static void haptic_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(HAPTIC_LOG, "a");
    if(!fp) {
        return;
    }
    fprintf(fp, "%llu ", (unsigned long long)ui_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

static int haptic_pref_bool(const char *key, int fallback)
{
    char value[16];

    if(ui_prefs_get(key, value, sizeof(value), fallback ? "1" : "0") != 0) {
        return fallback;
    }
    return atoi(value) != 0;
}

static void haptic_load_prefs(void)
{
    char value[16];
    int effect;

    if(haptic_prefs_loaded) {
        return;
    }

    if(ui_prefs_get(HAPTIC_PREF_EFFECT, value, sizeof(value), "1") == 0) {
        effect = atoi(value);
        if(effect >= 1 && effect <= 123) {
            haptic_effect_id = effect;
        }
    }
    haptic_keyboard_on = haptic_pref_bool(HAPTIC_PREF_KEYBOARD, 0);
    haptic_touch_on = haptic_pref_bool(HAPTIC_PREF_TOUCH, 0);
    haptic_prefs_loaded = 1;
}

static int haptic_reg_read(uint8_t reg, uint8_t *value)
{
    return ui_hardware_i2c4_read_reg(DRV2605_ADDR, reg, value);
}

static int haptic_reg_write(uint8_t reg, uint8_t value)
{
    return ui_hardware_i2c4_write_reg(DRV2605_ADDR, reg, value);
}

static int haptic_update_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    return ui_hardware_i2c4_update_bits(DRV2605_ADDR, reg, mask, value);
}

static int haptic_effect_known(int effect)
{
    for(size_t i = 0; i < sizeof(haptic_effects) / sizeof(haptic_effects[0]); i++) {
        if(haptic_effects[i].id == effect) {
            return 1;
        }
    }
    return effect >= 1 && effect <= 123;
}

static int haptic_init_chip(void)
{
    uint8_t status;
    int chip_id;

    if(haptic_reg_read(DRV2605_REG_STATUS, &status) != 0) {
        snprintf(haptic_status_text, sizeof(haptic_status_text),
                 "DRV2605 not detected on I2C4 0x5A");
        return -1;
    }

    chip_id = status >> 5;
    if(chip_id < 3 || chip_id > 7) {
        snprintf(haptic_status_text, sizeof(haptic_status_text),
                 "Unexpected status 0x%02X", status);
        haptic_log("probe failed: unexpected status=0x%02X chip_id=%d",
                   status, chip_id);
        return -1;
    }

    if(haptic_reg_write(DRV2605_REG_MODE, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_RTPIN, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_LIBRARY, 0x01) != 0 ||
       haptic_reg_write(DRV2605_REG_WAVESEQ1, 0x01) != 0 ||
       haptic_reg_write(DRV2605_REG_WAVESEQ2, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_OVERDRIVE, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_SUSTAINPOS, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_SUSTAINNEG, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_BREAK, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_AUDIOMAX, 0x64) != 0 ||
       haptic_update_bits(DRV2605_REG_FEEDBACK, 0x80, 0x00) != 0 ||
       haptic_update_bits(DRV2605_REG_CONTROL3, 0x20, 0x20) != 0) {
        snprintf(haptic_status_text, sizeof(haptic_status_text),
                 "DRV2605 init failed");
        haptic_log("init failed status=0x%02X chip_id=%d", status, chip_id);
        return -1;
    }

    snprintf(haptic_status_text, sizeof(haptic_status_text),
             "Ready on I2C4 0x5A");
    haptic_log("ready status=0x%02X chip_id=%d", status, chip_id);
    return 0;
}

static int haptic_probe(int force)
{
    uint64_t now = ui_monotonic_us();

    if(haptic_available_cache == 1 && !force) {
        return 1;
    }
    if(!force && haptic_available_cache == 0 && now < haptic_next_probe_us) {
        return 0;
    }

    if(haptic_init_chip() == 0) {
        haptic_available_cache = 1;
        return 1;
    }

    haptic_available_cache = 0;
    haptic_next_probe_us = now + HAPTIC_PROBE_RETRY_US;
    return 0;
}

static void haptic_play_effect(int effect, int force_probe)
{
    uint64_t now;

    if(!haptic_effect_known(effect)) {
        effect = 1;
    }
    if(!haptic_probe(force_probe)) {
        return;
    }

    now = ui_monotonic_us();
    if(!force_probe && haptic_last_play_us != 0ULL &&
       now - haptic_last_play_us < HAPTIC_MIN_PLAY_INTERVAL_US) {
        return;
    }
    haptic_last_play_us = now;

    if(haptic_reg_write(DRV2605_REG_WAVESEQ1, (uint8_t)effect) != 0 ||
       haptic_reg_write(DRV2605_REG_WAVESEQ2, 0x00) != 0 ||
       haptic_reg_write(DRV2605_REG_GO, 0x01) != 0) {
        haptic_available_cache = 0;
        snprintf(haptic_status_text, sizeof(haptic_status_text),
                 "DRV2605 play failed");
        haptic_next_probe_us = now + HAPTIC_PROBE_RETRY_US;
        haptic_log("play failed effect=%d", effect);
    }
}

void ui_haptic_init(void)
{
    haptic_load_prefs();
    snprintf(haptic_status_text, sizeof(haptic_status_text), "Not checked");
}

void ui_haptic_shutdown(void)
{
    if(haptic_available_cache == 1) {
        haptic_reg_write(DRV2605_REG_GO, 0x00);
    }
}

int ui_haptic_available(void)
{
    haptic_load_prefs();
    return haptic_probe(1);
}

const char *ui_haptic_status(void)
{
    return haptic_status_text;
}

int ui_haptic_effect(void)
{
    haptic_load_prefs();
    return haptic_effect_id;
}

const char *ui_haptic_effect_name(int effect)
{
    for(size_t i = 0; i < sizeof(haptic_effects) / sizeof(haptic_effects[0]); i++) {
        if(haptic_effects[i].id == effect) {
            return haptic_effects[i].name;
        }
    }
    return "Custom effect";
}

void ui_haptic_set_effect(int effect)
{
    char value[16];

    haptic_load_prefs();
    if(!haptic_effect_known(effect)) {
        effect = 1;
    }
    haptic_effect_id = effect;
    snprintf(value, sizeof(value), "%d", effect);
    ui_prefs_set(HAPTIC_PREF_EFFECT, value);
}

int ui_haptic_keyboard_enabled(void)
{
    haptic_load_prefs();
    return haptic_keyboard_on;
}

void ui_haptic_set_keyboard_enabled(int enabled)
{
    haptic_load_prefs();
    haptic_keyboard_on = enabled ? 1 : 0;
    ui_prefs_set(HAPTIC_PREF_KEYBOARD, haptic_keyboard_on ? "1" : "0");
}

int ui_haptic_touch_enabled(void)
{
    haptic_load_prefs();
    return haptic_touch_on;
}

void ui_haptic_set_touch_enabled(int enabled)
{
    haptic_load_prefs();
    haptic_touch_on = enabled ? 1 : 0;
    ui_prefs_set(HAPTIC_PREF_TOUCH, haptic_touch_on ? "1" : "0");
}

void ui_haptic_play_touch(void)
{
    haptic_load_prefs();
    if(haptic_touch_on) {
        haptic_play_effect(haptic_effect_id, 0);
    }
}

void ui_haptic_play_keyboard(void)
{
    haptic_load_prefs();
    if(haptic_keyboard_on) {
        haptic_play_effect(haptic_effect_id, 0);
    }
}

void ui_haptic_play_test(void)
{
    haptic_load_prefs();
    haptic_play_effect(haptic_effect_id, 1);
}

static void haptic_touch_event_cb(lv_event_t *event)
{
    (void)event;
    ui_haptic_play_touch();
}

void ui_haptic_bind_touch(lv_obj_t *obj)
{
    if(!obj) {
        return;
    }
    lv_obj_add_event_cb(obj, haptic_touch_event_cb, LV_EVENT_CLICKED, NULL);
}

static void haptic_settings_refresh(void)
{
    if(haptic_status_label && lv_obj_is_valid(haptic_status_label)) {
        lv_label_set_text(haptic_status_label, ui_tr(haptic_status_text));
    }
    if(haptic_effect_label && lv_obj_is_valid(haptic_effect_label)) {
        char text[96];

        snprintf(text, sizeof(text), "%s #%d",
                 ui_tr(ui_haptic_effect_name(haptic_effect_id)),
                 haptic_effect_id);
        lv_label_set_text(haptic_effect_label, text);
    }
    if(haptic_keyboard_switch && lv_obj_is_valid(haptic_keyboard_switch)) {
        if(haptic_keyboard_on) {
            lv_obj_add_state(haptic_keyboard_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(haptic_keyboard_switch, LV_STATE_CHECKED);
        }
    }
    if(haptic_touch_switch && lv_obj_is_valid(haptic_touch_switch)) {
        if(haptic_touch_on) {
            lv_obj_add_state(haptic_touch_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(haptic_touch_switch, LV_STATE_CHECKED);
        }
    }
    for(size_t i = 0; i < sizeof(haptic_effects) / sizeof(haptic_effects[0]); i++) {
        if(!haptic_effect_btn[i] || !lv_obj_is_valid(haptic_effect_btn[i])) {
            continue;
        }
        lv_obj_set_style_border_color(
            haptic_effect_btn[i],
            lv_color_hex(haptic_effects[i].id == haptic_effect_id ?
                         0x22D3EE : 0x2A3037), 0);
        lv_obj_set_style_border_width(
            haptic_effect_btn[i],
            haptic_effects[i].id == haptic_effect_id ? 2 : 1, 0);
    }
}

static void haptic_effect_event_cb(lv_event_t *event)
{
    int effect = (int)(intptr_t)lv_event_get_user_data(event);

    ui_haptic_set_effect(effect);
    ui_haptic_play_test();
    haptic_settings_refresh();
}

static void haptic_keyboard_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    ui_haptic_set_keyboard_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    if(haptic_keyboard_on) {
        ui_haptic_play_test();
    }
    haptic_settings_refresh();
}

static void haptic_touch_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    ui_haptic_set_touch_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
    if(haptic_touch_on) {
        ui_haptic_play_test();
    }
    haptic_settings_refresh();
}

static void haptic_test_event_cb(lv_event_t *event)
{
    (void)event;
    ui_haptic_play_test();
    haptic_settings_refresh();
}

static void haptic_create_switch_row(lv_obj_t *parent, int y, int card_w,
                                     const char *title, const char *subtitle,
                                     int checked, lv_event_cb_t cb,
                                     lv_obj_t **out_switch)
{
    int row_w = card_w > 64 ? card_w - 32 : 248;
    lv_obj_t *name;
    lv_obj_t *detail;
    lv_obj_t *sw;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 16, y);
    lv_obj_set_size(row, row_w, 76);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x121923), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x243142), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    name = ui_label(row, title, &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_set_width(name, row_w > 160 ? row_w - 128 : row_w - 96);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 16, 12);

    detail = ui_label(row, subtitle, &lv_font_montserrat_14, 0x94A3B8);
    lv_obj_set_width(detail, row_w > 160 ? row_w - 128 : row_w - 96);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 16, 44);

    sw = lv_switch_create(row);
    lv_obj_set_size(sw, 68, 36);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -14, 0);
    if(checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui_haptic_bind_touch(sw);
    if(out_switch) {
        *out_switch = sw;
    }
}

void ui_haptic_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *btn;
    int top_y = ui_is_landscape() ? 64 : 144;
    int body_x = ui_page_panel_x();
    int body_w = ui_page_panel_width();
    int card_w;
    int landscape = ui_is_landscape();
    int detected;
    int effect_cols = landscape ? 3 : 2;
    int effect_gap = 10;
    int effect_cell_w;
    int effect_cell_h = 70;

    ui_haptic_init();
    detected = haptic_probe(1);
    memset(haptic_effect_btn, 0, sizeof(haptic_effect_btn));
    haptic_status_label = NULL;
    haptic_effect_label = NULL;
    haptic_keyboard_switch = NULL;
    haptic_touch_switch = NULL;

    ui_create_header(scr, "Haptics");
    body = ui_scroll_panel(scr, body_x, ui_page_top_y(top_y), body_w,
                           ui_body_height(top_y));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B1016), 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, 14, 0);
    card_w = ui_safe_content_width(body, body_w - 32);
    if(card_w < 280) {
        card_w = 280;
    }

    card = ui_panel(body, 0, 0, card_w, landscape ? 156 : 178);
    lv_obj_set_style_pad_all(card, 0, 0);
    label = ui_label(card, "DRV2605 haptic feedback",
                     &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 16);
    label = ui_label(card, "Optional I2C4 vibration driver",
                     &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 52);

    ui_info_row_inset(card, landscape ? 84 : 90, "Status",
                      detected ? "Ready" : "Not detected",
                      detected ? 0x22C55E : 0x94A3B8, 18);
    haptic_status_label = ui_label(card, haptic_status_text,
                                   &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_width(haptic_status_label, card_w - 36);
    lv_label_set_long_mode(haptic_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(haptic_status_label, LV_ALIGN_BOTTOM_LEFT, 18, -16);

    card = ui_panel(body, 0, 0, card_w, landscape ? 232 : 316);
    lv_obj_set_style_pad_all(card, 0, 0);
    label = ui_label(card, "Effect", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 14);
    haptic_effect_label = ui_label(card, ui_haptic_effect_name(haptic_effect_id),
                                   &lv_font_montserrat_16, 0x94A3B8);
    lv_obj_set_width(haptic_effect_label, card_w - 36);
    lv_label_set_long_mode(haptic_effect_label, LV_LABEL_LONG_DOT);
    lv_obj_align(haptic_effect_label, LV_ALIGN_TOP_LEFT, 18, 46);

    effect_cell_w = (card_w - 36 - (effect_cols - 1) * effect_gap) / effect_cols;
    if(effect_cell_w < 120) {
        effect_cell_w = 120;
    }
    for(size_t i = 0; i < sizeof(haptic_effects) / sizeof(haptic_effects[0]); i++) {
        int col = (int)i % effect_cols;
        int row = (int)i / effect_cols;
        int bx = 18 + col * (effect_cell_w + effect_gap);
        int by = 82 + row * (effect_cell_h + effect_gap);

        haptic_effect_btn[i] = ui_command_button(card, bx, by,
                                                 effect_cell_w,
                                                 haptic_effects[i].name,
                                                 0x22D3EE);
        lv_obj_set_height(haptic_effect_btn[i], 58);
        lv_obj_add_event_cb(haptic_effect_btn[i], haptic_effect_event_cb,
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)haptic_effects[i].id);
    }

    card = ui_panel(body, 0, 0, card_w, landscape ? 230 : 246);
    lv_obj_set_style_pad_all(card, 0, 0);
    label = ui_label(card, "Feedback rules", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 16);
    haptic_create_switch_row(card, 58, card_w, "Keyboard press vibration",
                             "Physical TCA8418 key press feedback",
                             haptic_keyboard_on, haptic_keyboard_switch_cb,
                             &haptic_keyboard_switch);
    haptic_create_switch_row(card, 142, card_w, "Touch control vibration",
                             "Icon, button and switch click feedback",
                             haptic_touch_on, haptic_touch_switch_cb,
                             &haptic_touch_switch);

    btn = ui_command_button(body, 0, 0, card_w, "Test vibration",
                            0xF472B6);
    lv_obj_add_event_cb(btn, haptic_test_event_cb, LV_EVENT_CLICKED, NULL);

    haptic_settings_refresh();
    lv_obj_update_layout(body);
}
