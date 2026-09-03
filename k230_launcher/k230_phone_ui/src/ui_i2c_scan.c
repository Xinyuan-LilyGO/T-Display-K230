#include "ui_i2c_scan.h"

#include "ui_hardware.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define I2C_SCAN_BUS_MAX 8
#define I2C_SCAN_EXT_BUS 4
#define I2C_SCAN_ADDR_MAX 0x80
#define I2C_SCAN_STATUS_MAX 160
#define I2C_SCAN_SUMMARY_MAX 192

static pthread_mutex_t i2c_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static int i2c_scan_busy;
static int i2c_scan_result_ready;
static char i2c_scan_status[I2C_SCAN_STATUS_MAX] = "Ready";
static char i2c_scan_summary[I2C_SCAN_BUS_MAX][I2C_SCAN_SUMMARY_MAX];
static uint8_t i2c_scan_bus_present[I2C_SCAN_BUS_MAX];
static uint8_t i2c_scan_found[I2C_SCAN_BUS_MAX][I2C_SCAN_ADDR_MAX];
static lv_timer_t *i2c_scan_timer;
static lv_obj_t *i2c_scan_status_label;
static lv_obj_t *i2c_scan_bus_summary_label[I2C_SCAN_BUS_MAX];
static lv_obj_t *i2c_scan_addr_obj[I2C_SCAN_BUS_MAX][I2C_SCAN_ADDR_MAX];

static const char *i2c_scan_bus_title(int bus)
{
    switch(bus) {
    case 0:
        return "I2C0 / physical I2C4";
    case 1:
        return "I2C1 / physical I2C3";
    case I2C_SCAN_EXT_BUS:
        return "I2C4 GPIO SDA47/SCL46";
    default:
        break;
    }
    return NULL;
}

static void i2c_scan_format_bus_name(int bus, char *buf, size_t len)
{
    const char *title = i2c_scan_bus_title(bus);

    if(!buf || len == 0) {
        return;
    }
    if(title) {
        snprintf(buf, len, "%s", title);
    } else {
        snprintf(buf, len, "I2C%d", bus);
    }
}

static int i2c_bus_exists(int bus)
{
    char dev[32];

    if(bus == I2C_SCAN_EXT_BUS) {
        return 1;
    }

    snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
    return access(dev, F_OK) == 0;
}

static int i2c_probe_addr(int fd, int addr)
{
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args;

    if(ioctl(fd, I2C_SLAVE, addr) < 0) {
        return 0;
    }

    memset(&data, 0, sizeof(data));
    memset(&args, 0, sizeof(args));
    args.read_write = I2C_SMBUS_WRITE;
    args.command = 0;
    args.size = I2C_SMBUS_QUICK;
    args.data = &data;
    if(ioctl(fd, I2C_SMBUS, &args) == 0) {
        return 1;
    }

    args.read_write = I2C_SMBUS_READ;
    args.size = I2C_SMBUS_BYTE;
    return ioctl(fd, I2C_SMBUS, &args) == 0;
}

static void i2c_scan_append_known(char *buf, size_t len, const char *name)
{
    size_t used;

    if(!buf || !name || len == 0) {
        return;
    }
    used = strlen(buf);
    if(used + 4 >= len) {
        return;
    }
    snprintf(buf + used, len - used, "%s%s", used ? " " : "  ", name);
}

static void i2c_scan_known_devices(const uint8_t found[I2C_SCAN_ADDR_MAX],
                                   char *buf, size_t len)
{
    int xl_addr = -1;

    if(!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if(!found) {
        return;
    }

    if(found[0x34]) {
        i2c_scan_append_known(buf, len, "TCA8418");
    }
    if(found[0x37]) {
        i2c_scan_append_known(buf, len, "GC2093");
    }
    if(found[0x38]) {
        i2c_scan_append_known(buf, len, "AHT20");
    }
    if(found[0x3B]) {
        i2c_scan_append_known(buf, len, "LT9611");
    }
    if(found[0x55]) {
        i2c_scan_append_known(buf, len, "BQ27220");
    }
    if(found[0x5D]) {
        i2c_scan_append_known(buf, len, "GT9895");
    }
    if(found[0x6B]) {
        i2c_scan_append_known(buf, len, "BQ25896");
    }
    for(int addr = 0x20; addr <= 0x27; addr++) {
        if(found[addr]) {
            xl_addr = addr;
            break;
        }
    }
    if(xl_addr >= 0) {
        char name[20];

        snprintf(name, sizeof(name), "XL9555@0x%02X", xl_addr);
        i2c_scan_append_known(buf, len, name);
    }
}

static void i2c_scan_format_summary(int bus, int found_bus,
                                    const uint8_t found[I2C_SCAN_ADDR_MAX],
                                    char *summary, size_t len)
{
    char name[48];
    char known[112];

    if(!summary || len == 0) {
        return;
    }

    i2c_scan_format_bus_name(bus, name, sizeof(name));
    i2c_scan_known_devices(found, known, sizeof(known));
    snprintf(summary, len, "%s  %d device%s%s", name, found_bus,
             found_bus == 1 ? "" : "s", known);
}

static void *i2c_scan_thread(void *arg)
{
    char local_summary[I2C_SCAN_BUS_MAX][I2C_SCAN_SUMMARY_MAX];
    uint8_t local_present[I2C_SCAN_BUS_MAX];
    uint8_t local_found[I2C_SCAN_BUS_MAX][I2C_SCAN_ADDR_MAX];
    int found_total = 0;
    int bus_total = 0;

    (void)arg;
    memset(local_summary, 0, sizeof(local_summary));
    memset(local_present, 0, sizeof(local_present));
    memset(local_found, 0, sizeof(local_found));

    for(int bus = 0; bus < I2C_SCAN_BUS_MAX; bus++) {
        char dev[32];
        char bus_name[48];
        int fd;
        int found_bus = 0;

        if(bus == I2C_SCAN_EXT_BUS) {
            char hw_status[96];
            int rc;

            local_present[bus] = 1;
            bus_total++;
            rc = ui_hardware_i2c4_scan(local_found[bus], hw_status,
                                       sizeof(hw_status));
            if(rc < 0) {
                snprintf(local_summary[bus], sizeof(local_summary[bus]),
                         "%s", hw_status);
                continue;
            }
            found_bus = rc;
            found_total += found_bus;
            i2c_scan_format_summary(bus, found_bus, local_found[bus],
                                    local_summary[bus],
                                    sizeof(local_summary[bus]));
            continue;
        }

        snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
        if(access(dev, F_OK) != 0) {
            snprintf(local_summary[bus], sizeof(local_summary[bus]),
                     "%s missing", dev);
            continue;
        }

        local_present[bus] = 1;
        bus_total++;
        fd = open(dev, O_RDWR);
        if(fd < 0) {
            snprintf(local_summary[bus], sizeof(local_summary[bus]),
                     "%s open failed: %s", dev, strerror(errno));
            continue;
        }

        for(int addr = 0x03; addr <= 0x77; addr++) {
            if(i2c_probe_addr(fd, addr)) {
                local_found[bus][addr] = 1;
                found_bus++;
                found_total++;
            }
        }
        close(fd);

        i2c_scan_format_bus_name(bus, bus_name, sizeof(bus_name));
        snprintf(local_summary[bus], sizeof(local_summary[bus]),
                 "%s %s  %d device%s", bus_name, dev, found_bus,
                 found_bus == 1 ? "" : "s");
        i2c_scan_known_devices(local_found[bus],
                               local_summary[bus] +
                                   strlen(local_summary[bus]),
                               sizeof(local_summary[bus]) -
                                   strlen(local_summary[bus]));
    }

    pthread_mutex_lock(&i2c_scan_lock);
    memcpy(i2c_scan_summary, local_summary, sizeof(i2c_scan_summary));
    memcpy(i2c_scan_bus_present, local_present, sizeof(i2c_scan_bus_present));
    memcpy(i2c_scan_found, local_found, sizeof(i2c_scan_found));
    snprintf(i2c_scan_status, sizeof(i2c_scan_status),
             "%d device%s found on %d I2C bus%s", found_total,
             found_total == 1 ? "" : "s", bus_total,
             bus_total == 1 ? "" : "es");
    i2c_scan_busy = 0;
    i2c_scan_result_ready = 1;
    pthread_mutex_unlock(&i2c_scan_lock);
    return NULL;
}

static void i2c_scan_style_addr(lv_obj_t *obj, int valid, int found)
{
    lv_obj_t *label;
    uint32_t bg = valid ? 0x18212B : 0x0E141B;
    uint32_t fg = valid ? 0x64748B : 0x263241;

    if(!obj) {
        return;
    }
    if(found) {
        bg = 0x25C281;
        fg = 0xFFFFFF;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(found ? 0x7CF4B8 :
                                                    0x263544), 0);
    if(lv_obj_get_child_count(obj) > 0) {
        label = lv_obj_get_child(obj, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    }
}

static void i2c_scan_update_page(void)
{
    char status[I2C_SCAN_STATUS_MAX];
    char summary[I2C_SCAN_BUS_MAX][I2C_SCAN_SUMMARY_MAX];
    uint8_t present[I2C_SCAN_BUS_MAX];
    uint8_t found[I2C_SCAN_BUS_MAX][I2C_SCAN_ADDR_MAX];
    int busy;

    pthread_mutex_lock(&i2c_scan_lock);
    busy = i2c_scan_busy;
    snprintf(status, sizeof(status), "%s", i2c_scan_status);
    memcpy(summary, i2c_scan_summary, sizeof(summary));
    memcpy(present, i2c_scan_bus_present, sizeof(present));
    memcpy(found, i2c_scan_found, sizeof(found));
    i2c_scan_result_ready = 0;
    pthread_mutex_unlock(&i2c_scan_lock);

    if(i2c_scan_status_label) {
        lv_label_set_text(i2c_scan_status_label, busy ? "Scanning..." :
                          status);
        lv_obj_set_style_text_color(i2c_scan_status_label,
                                    lv_color_hex(busy ? 0xF5A524 :
                                                 0x25C281), 0);
    }

    for(int bus = 0; bus < I2C_SCAN_BUS_MAX; bus++) {
        if(i2c_scan_bus_summary_label[bus]) {
            lv_label_set_text(i2c_scan_bus_summary_label[bus],
                              summary[bus][0] ? summary[bus] : "--");
        }
        for(int addr = 0; addr < I2C_SCAN_ADDR_MAX; addr++) {
            i2c_scan_style_addr(i2c_scan_addr_obj[bus][addr],
                                present[bus] && addr >= 0x03 && addr <= 0x77,
                                present[bus] && found[bus][addr]);
        }
    }
}

static void i2c_scan_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    i2c_scan_update_page();
}

static void i2c_scan_start(void)
{
    pthread_t thread;
    int start_thread = 0;

    pthread_mutex_lock(&i2c_scan_lock);
    if(!i2c_scan_busy) {
        i2c_scan_busy = 1;
        i2c_scan_result_ready = 0;
        snprintf(i2c_scan_status, sizeof(i2c_scan_status), "Scanning...");
        start_thread = 1;
    }
    pthread_mutex_unlock(&i2c_scan_lock);

    if(start_thread) {
        if(pthread_create(&thread, NULL, i2c_scan_thread, NULL) == 0) {
            pthread_detach(thread);
        } else {
            pthread_mutex_lock(&i2c_scan_lock);
            i2c_scan_busy = 0;
            snprintf(i2c_scan_status, sizeof(i2c_scan_status),
                     "Scan thread failed");
            i2c_scan_result_ready = 1;
            pthread_mutex_unlock(&i2c_scan_lock);
        }
    }

    i2c_scan_update_page();
    app_request_fast_refresh();
}

static void i2c_scan_event_cb(lv_event_t *event)
{
    (void)event;
    i2c_scan_start();
}

static lv_obj_t *i2c_addr_cell(lv_obj_t *parent, int x, int y, int w, int h,
                               int addr)
{
    char text[8];
    lv_obj_t *cell = lv_obj_create(parent);

    snprintf(text, sizeof(text), "%02X", addr);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, 6, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_label(cell, text, &lv_font_montserrat_14, 0x64748B);
    lv_obj_center(label);
    i2c_scan_style_addr(cell, addr >= 0x03 && addr <= 0x77, 0);
    return cell;
}

static int i2c_create_bus_matrix(lv_obj_t *body, int bus, int y)
{
    int landscape = ui_is_landscape();
    int body_w = ui_screen_width() - 48;
    int cols = landscape ? 16 : 8;
    int gap = 6;
    int cell_w = (body_w - gap * (cols - 1)) / cols;
    int cell_h = landscape ? 32 : 36;
    int rows = (I2C_SCAN_ADDR_MAX + cols - 1) / cols;
    int panel_h = 72 + rows * (cell_h + gap);
    lv_obj_t *panel;
    lv_obj_t *title;
    char text[48];

    if(body_w < 320) {
        body_w = 320;
    }
    if(cell_w < 30) {
        cell_w = 30;
    }

    panel = ui_panel(body, 24, y, body_w, panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x121820), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x263544), 0);
    lv_obj_set_style_pad_all(panel, 12, 0);

    i2c_scan_format_bus_name(bus, text, sizeof(text));
    title = ui_label(panel, text, &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_pos(title, 0, 0);

    i2c_scan_bus_summary_label[bus] = ui_label(panel, "--",
                                                &lv_font_montserrat_16,
                                                0x9AA4AF);
    lv_obj_set_width(i2c_scan_bus_summary_label[bus], body_w - 24);
    lv_label_set_long_mode(i2c_scan_bus_summary_label[bus],
                           LV_LABEL_LONG_DOT);
    lv_obj_set_pos(i2c_scan_bus_summary_label[bus], 0, 34);

    for(int addr = 0; addr < I2C_SCAN_ADDR_MAX; addr++) {
        int col = addr % cols;
        int row = addr / cols;
        i2c_scan_addr_obj[bus][addr] =
            i2c_addr_cell(panel, col * (cell_w + gap),
                          72 + row * (cell_h + gap),
                          cell_w, cell_h, addr);
    }

    return y + panel_h + 18;
}

void ui_i2c_scan_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *scan_btn;
    int y = 24;
    int body_w = ui_screen_width() - 48;
    int button_w = ui_is_landscape() ? 160 : 488;
    int bus_count = 0;

    ui_create_header(scr, "I2C Scan");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);

    i2c_scan_status_label = ui_label(body, "Ready", &lv_font_montserrat_18,
                                     0x9AA4AF);
    lv_obj_set_width(i2c_scan_status_label, body_w - button_w - 36);
    if(!ui_is_landscape()) {
        lv_obj_set_width(i2c_scan_status_label, body_w);
    }
    lv_label_set_long_mode(i2c_scan_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(i2c_scan_status_label, 24, y + 18);

    scan_btn = ui_command_button(body,
                                 ui_is_landscape() ? body_w - button_w + 24 : 24,
                                 ui_is_landscape() ? y : y + 58,
                                 button_w, "Scan", 0x22D3EE);
    lv_obj_add_event_cb(scan_btn, i2c_scan_event_cb, LV_EVENT_CLICKED, NULL);
    y += ui_is_landscape() ? 84 : 142;

    for(int bus = 0; bus < I2C_SCAN_BUS_MAX; bus++) {
        if(!i2c_bus_exists(bus)) {
            continue;
        }
        y = i2c_create_bus_matrix(body, bus, y);
        bus_count++;
    }

    if(bus_count == 0) {
        lv_obj_t *label = ui_label(body, "No I2C bus found",
                                   &lv_font_montserrat_22, 0xF5A524);
        lv_obj_set_pos(label, 24, y + 24);
    }

    if(!i2c_scan_timer) {
        i2c_scan_timer = lv_timer_create(i2c_scan_timer_cb, 250, NULL);
    }
    i2c_scan_start();
}

void ui_i2c_scan_cleanup(void)
{
    if(i2c_scan_timer) {
        lv_timer_delete(i2c_scan_timer);
        i2c_scan_timer = NULL;
    }
    i2c_scan_status_label = NULL;
    memset(i2c_scan_bus_summary_label, 0, sizeof(i2c_scan_bus_summary_label));
    memset(i2c_scan_addr_obj, 0, sizeof(i2c_scan_addr_obj));
}
