#include "ui_ble.h"

#include "ui_i18n.h"
#include "ui_meshtastic.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLE_MAX_DEVICES 64
#define BLE_ADDR_LEN 18
#define BLE_NAME_MAX 80
#define BLE_STATUS_MAX 160
#define BLE_GATT_TEXT_MAX 4096
#define BLE_GATT_TITLE_MAX 128
#define BLE_SCAN_DURATION_US 8000000ULL
#define BLE_HCIATTACH_LOG "/tmp/k230_ble_hciattach.log"
#define BLE_CONNECT_LOG "/tmp/k230_ble_connect.log"
#define BLE_GATT_LOG "/tmp/k230_ble_gatt.log"
#define BLE_SCAN_LOG "/tmp/k230_ble_scan.log"
#define BLE_DEBUG_LOG "/tmp/k230_ble_debug.log"
#define BLE_BLUETOOTHD_LOG "/tmp/k230_bluetoothd_ui.log"
#define BLE_NRF_UART_DEV "/dev/ttyS1"
#define BLE_NRF_BAUD B115200
#define BLE_NRF_SCAN_SECONDS 5
#define BLE_AUTO_SCAN_PERIOD_US 5000000ULL
#define BLE_PREF_ENABLED "ble.enabled"
#define BLE_PREF_MODE "ble.mode"
#define BLE_MODE_PREF_MESH "mesh"
#define BLE_MODE_PREF_CUSTOM "custom"

typedef enum {
    BLE_BACKEND_HCI = 0,
    BLE_BACKEND_NRF_AT = 1,
} ble_backend_t;

typedef enum {
    BLE_MODE_MESH_EXCLUSIVE = 0,
    BLE_MODE_CUSTOM = 1,
} ble_mode_t;

typedef struct {
    char addr[BLE_ADDR_LEN];
    char name[BLE_NAME_MAX];
    int rssi_dbm;
    int rssi_valid;
    int ble_uart;
    int scan_index;
    int scan_index_valid;
} ble_device_t;

typedef struct {
    char addr[BLE_ADDR_LEN];
    char name[BLE_NAME_MAX];
    int scan_index;
    int scan_index_valid;
} ble_connect_req_t;

static pthread_mutex_t ble_lock = PTHREAD_MUTEX_INITIALIZER;
static ble_device_t ble_devices[BLE_MAX_DEVICES];
static int ble_device_count;
static int ble_selected = -1;
static int ble_busy;
static int ble_scanning;
static int ble_scan_enabled = 1;
static int ble_prefs_loaded;
static ble_mode_t ble_mode = BLE_MODE_MESH_EXCLUSIVE;
static uint64_t ble_next_scan_us;
static int ble_adapter_present;
static int ble_adapter_ready;
static int ble_gatt_busy;
static int ble_detail_mode;
static int ble_connected;
static int ble_connect_queued;
static ble_backend_t ble_backend = BLE_BACKEND_HCI;
static char ble_status[BLE_STATUS_MAX] = "Ready";
static char ble_gatt_text[BLE_GATT_TEXT_MAX] = "No GATT data";
static char ble_connected_addr[BLE_ADDR_LEN];
static char ble_connected_name[BLE_NAME_MAX];
static char ble_queued_addr[BLE_ADDR_LEN];
static char ble_queued_name[BLE_NAME_MAX];
static int ble_queued_scan_index = -1;
static int ble_queued_scan_index_valid;
static char ble_adapter_name[16] = "hci0";

static lv_obj_t *ble_summary_panel;
static lv_obj_t *ble_state_label;
static lv_obj_t *ble_status_label;
static lv_obj_t *ble_scan_switch;
static lv_obj_t *ble_mode_mesh_btn;
static lv_obj_t *ble_mode_custom_btn;
static lv_obj_t *ble_list_title_label;
static lv_obj_t *ble_device_list;
static lv_obj_t *ble_detail_panel;
static lv_obj_t *ble_detail_title_label;
static lv_obj_t *ble_detail_addr_label;
static lv_obj_t *ble_gatt_status_label;
static lv_obj_t *ble_gatt_detail_label;
static lv_obj_t *ble_confirm_overlay;
static lv_obj_t *ble_connect_overlay;
static lv_obj_t *ble_connect_overlay_label;
static ble_device_t *ble_confirm_device;
static lv_obj_t *ble_device_btn[BLE_MAX_DEVICES];
static lv_obj_t *ble_device_type_icon[BLE_MAX_DEVICES];
static lv_obj_t *ble_device_signal_icon[BLE_MAX_DEVICES];
static lv_obj_t *ble_device_rssi_label[BLE_MAX_DEVICES];
static lv_obj_t *ble_device_name[BLE_MAX_DEVICES];
static lv_obj_t *ble_device_meta[BLE_MAX_DEVICES];
static lv_timer_t *ble_timer;

static void ble_add_device(const char *addr, const char *name, int rssi,
                           int rssi_valid, int ble_uart, int scan_index,
                           int scan_index_valid);
static int ble_prepare_hci_adapter_only(void);
static void ble_try_start_queued_connection(void);
static void ble_refresh_ui(void);

#if 0
static const lv_point_precise_t ble_type_line_center[] = {
    {17, 5}, {17, 29},
};
static const lv_point_precise_t ble_type_line_right[] = {
    {17, 5}, {25, 13}, {17, 21}, {25, 29},
};
static const lv_point_precise_t ble_type_line_left_upper[] = {
    {17, 13}, {9, 21},
};
static const lv_point_precise_t ble_type_line_left_lower[] = {
    {9, 13}, {17, 21},
};
#endif

static int ble_command_exists(const char *cmd)
{
    char shell[96];

    snprintf(shell, sizeof(shell), "command -v %s >/dev/null 2>&1", cmd);
    return ui_shell_exit_code(system(shell)) == 0;
}

static void ble_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(BLE_DEBUG_LOG, "a");
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

static ble_mode_t ble_mode_from_text(const char *value)
{
    if(value && strcmp(value, BLE_MODE_PREF_CUSTOM) == 0) {
        return BLE_MODE_CUSTOM;
    }
    return BLE_MODE_MESH_EXCLUSIVE;
}

static const char *ble_mode_pref_value(ble_mode_t mode)
{
    return mode == BLE_MODE_CUSTOM ? BLE_MODE_PREF_CUSTOM : BLE_MODE_PREF_MESH;
}

static void ble_load_prefs_locked(void)
{
    char value[16];

    if(ble_prefs_loaded) {
        return;
    }

    ui_prefs_get(BLE_PREF_ENABLED, value, sizeof(value), "1");
    ble_scan_enabled = strcmp(value, "0") != 0;
    ui_prefs_get(BLE_PREF_MODE, value, sizeof(value), BLE_MODE_PREF_MESH);
    ble_mode = ble_mode_from_text(value);
    ble_prefs_loaded = 1;
}

static void ble_save_enabled(int enabled)
{
    if(ui_prefs_set(BLE_PREF_ENABLED, enabled ? "1" : "0") != 0) {
        ble_log("save %s=%d failed", BLE_PREF_ENABLED, enabled ? 1 : 0);
    }
}

static void ble_save_mode(ble_mode_t mode)
{
    if(ui_prefs_set(BLE_PREF_MODE, ble_mode_pref_value(mode)) != 0) {
        ble_log("save %s=%s failed", BLE_PREF_MODE, ble_mode_pref_value(mode));
    }
}

int ui_ble_meshtastic_bridge_enabled(void)
{
    int enabled;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    enabled = ble_scan_enabled && ble_mode == BLE_MODE_MESH_EXCLUSIVE;
    pthread_mutex_unlock(&ble_lock);
    return enabled;
}

static int ble_custom_mode_enabled(void)
{
    int enabled;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    enabled = ble_scan_enabled && ble_mode == BLE_MODE_CUSTOM;
    pthread_mutex_unlock(&ble_lock);
    return enabled;
}

static void ble_scan_log_line(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(BLE_SCAN_LOG, "a");
    if(!fp) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
}

static void ble_text_append(char *dst, size_t dst_len, const char *fmt, ...)
{
    va_list ap;
    size_t len;

    if(!dst || dst_len == 0) {
        return;
    }

    len = strlen(dst);
    if(len >= dst_len - 1U) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(dst + len, dst_len - len, fmt, ap);
    va_end(ap);
}

static void ble_gatt_store_result(const char *text)
{
    FILE *fp;

    pthread_mutex_lock(&ble_lock);
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s",
             text && text[0] ? text : "No GATT data");
    pthread_mutex_unlock(&ble_lock);

    fp = fopen(BLE_GATT_LOG, "w");
    if(fp) {
        fputs(text && text[0] ? text : "No GATT data", fp);
        fclose(fp);
    }
}

static int wireless_signal_level_from_dbm(int dbm)
{
    if(dbm >= -55) {
        return 4;
    }
    if(dbm >= -67) {
        return 3;
    }
    if(dbm >= -80) {
        return 2;
    }
    return 1;
}

static uint32_t wireless_signal_color(int dbm, int valid)
{
    if(!valid) {
        return 0x64748B;
    }
    if(dbm >= -55) {
        return 0x25C281;
    }
    if(dbm >= -67) {
        return 0xA3E635;
    }
    if(dbm >= -80) {
        return 0xF5A524;
    }
    return 0xEF4D5A;
}

static void wireless_signal_icon_update(lv_obj_t *icon, int dbm, int valid)
{
    int level = valid ? wireless_signal_level_from_dbm(dbm) : 0;
    uint32_t color = wireless_signal_color(dbm, valid);
    uint32_t children;

    if(!icon) {
        return;
    }
    children = lv_obj_get_child_count(icon);
    for(uint32_t i = 0; i < children; i++) {
        lv_obj_t *bar = lv_obj_get_child(icon, i);
        lv_obj_set_style_bg_color(bar,
                                  lv_color_hex((int)i < level ? color : 0x26313D),
                                  0);
        lv_obj_set_style_bg_opa(bar, (int)i < level ? LV_OPA_COVER : LV_OPA_60,
                                0);
    }
}

static lv_obj_t *wireless_signal_icon_create(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);

    lv_obj_set_size(icon, 34, 28);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(icon);

    for(int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(icon);
        lv_obj_set_size(bar, 5, 7 + i * 5);
        lv_obj_set_pos(bar, 2 + i * 8, 21 - i * 5);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x26313D), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        ui_make_click_forwarder(bar);
    }
    return icon;
}

#if 0
static lv_obj_t *ble_type_line_create(lv_obj_t *parent,
                                      const lv_point_precise_t *points,
                                      uint32_t point_count)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, points, point_count);
    lv_obj_set_size(line, 34, 34);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0x64748B), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(line);
    return line;
}
#endif

static void ble_type_icon_update(lv_obj_t *icon, uint32_t color)
{
    if(!icon) {
        return;
    }
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
}

static lv_obj_t *ble_type_icon_create(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_label_create(parent);

    lv_obj_set_size(icon, 34, 34);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(icon);
    return icon;
}

static int ble_find_adapter(char *name, size_t len)
{
    DIR *dir = opendir("/sys/class/bluetooth");
    struct dirent *entry;

    if(!dir) {
        return 0;
    }

    while((entry = readdir(dir)) != NULL) {
        if(strncmp(entry->d_name, "hci", 3) != 0 ||
           !isdigit((unsigned char)entry->d_name[3])) {
            continue;
        }
        if(name && len > 0) {
            snprintf(name, len, "%s", entry->d_name);
        }
        closedir(dir);
        return 1;
    }

    closedir(dir);
    return 0;
}

static int ble_adapter_exists(void)
{
    return ble_find_adapter(NULL, 0);
}

static void ble_set_status(const char *status)
{
    pthread_mutex_lock(&ble_lock);
    snprintf(ble_status, sizeof(ble_status), "%s", status ? status : "Ready");
    pthread_mutex_unlock(&ble_lock);
}

static void ble_update_adapter_cache(void)
{
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    int present = ble_find_adapter(adapter, sizeof(adapter));

    pthread_mutex_lock(&ble_lock);
    if(ble_backend == BLE_BACKEND_NRF_AT && ble_adapter_ready) {
        pthread_mutex_unlock(&ble_lock);
        return;
    }
    ble_adapter_present = present;
    if(!present) {
        ble_adapter_ready = 0;
    }
    if(present) {
        snprintf(ble_adapter_name, sizeof(ble_adapter_name), "%s", adapter);
        ble_backend = BLE_BACKEND_HCI;
    }
    pthread_mutex_unlock(&ble_lock);
}

static int ble_shell_run(const char *cmd)
{
    int rc = system(cmd);

    return ui_shell_exit_code(rc);
}

static void ble_start_bluetoothd_once(void)
{
    int rc;

    rc = ble_shell_run("if pidof bluetoothd >/dev/null 2>&1; then exit 0; fi; "
                       "if [ -x /etc/init.d/S40bluetoothd ]; then "
                       "/etc/init.d/S40bluetoothd start-manual >" BLE_BLUETOOTHD_LOG " 2>&1; "
                       "else /usr/libexec/bluetooth/bluetoothd -n >" BLE_BLUETOOTHD_LOG " 2>&1 & "
                       "fi");
    ble_log("bluetoothd on-demand start rc=%d", rc);
}

static void ble_stop_bluetoothd_on_demand(void)
{
    int rc;

    rc = ble_shell_run("if [ -x /etc/init.d/S40bluetoothd ]; then "
                       "/etc/init.d/S40bluetoothd stop >" BLE_BLUETOOTHD_LOG " 2>&1 || true; "
                       "else killall bluetoothd >/dev/null 2>&1 || true; fi");
    ble_log("bluetoothd on-demand stop rc=%d", rc);
}

static int ble_nrf_open(void)
{
    struct termios tio;
    int fd = open(BLE_NRF_UART_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

    if(fd < 0) {
        ble_log("nrf open failed dev=%s errno=%d (%s)", BLE_NRF_UART_DEV,
                errno, strerror(errno));
        return -1;
    }

    memset(&tio, 0, sizeof(tio));
    if(tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, BLE_NRF_BAUD);
        cfsetospeed(&tio, BLE_NRF_BAUD);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~CRTSCTS;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int ble_nrf_write_cmd(int fd, const char *cmd)
{
    char line[128];
    size_t len;

    if(fd < 0 || !cmd) {
        return -1;
    }
    snprintf(line, sizeof(line), "%s\r\n", cmd);
    len = strlen(line);
    if(write(fd, line, len) != (ssize_t)len) {
        ble_log("nrf write failed cmd=%s errno=%d (%s)", cmd, errno, strerror(errno));
        return -1;
    }
    ble_log("nrf tx %s", cmd);
    return 0;
}

static int ble_nrf_read_line(int fd, char *line, size_t line_len,
                             uint64_t timeout_us)
{
    uint64_t deadline = ui_monotonic_us() + timeout_us;
    size_t len = 0;

    if(fd < 0 || !line || line_len == 0) {
        return -1;
    }

    while(ui_monotonic_us() < deadline) {
        fd_set rfds;
        struct timeval tv;
        char ch;
        ssize_t rd;
        uint64_t now = ui_monotonic_us();
        uint64_t remain = deadline > now ? deadline - now : 0;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = (time_t)(remain / 1000000ULL);
        tv.tv_usec = (suseconds_t)(remain % 1000000ULL);
        if(tv.tv_sec > 0 || tv.tv_usec > 200000) {
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
        }

        if(select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        rd = read(fd, &ch, 1);
        if(rd <= 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(ch == '\r') {
            continue;
        }
        if(ch == '\n') {
            line[len] = '\0';
            if(len == 0) {
                continue;
            }
            ble_log("nrf rx %s", line);
            return (int)len;
        }
        if(len + 1 < line_len) {
            line[len++] = ch;
        } else {
            len = 0;
        }
    }

    return -1;
}

static int ble_nrf_wait_ok(int fd, uint64_t timeout_us)
{
    char line[192];
    uint64_t deadline = ui_monotonic_us() + timeout_us;

    while(ui_monotonic_us() < deadline) {
        uint64_t remain = deadline - ui_monotonic_us();
        if(ble_nrf_read_line(fd, line, sizeof(line), remain) < 0) {
            break;
        }
        if(strcmp(line, "OK") == 0) {
            return 0;
        }
        if(strncmp(line, "ERR:", 4) == 0) {
            ble_set_status(line);
            return -1;
        }
    }
    return -1;
}

static int ble_nrf_probe(void)
{
    int fd = ble_nrf_open();
    int rc = -1;

    if(fd < 0) {
        return -1;
    }
    if(ble_nrf_write_cmd(fd, "AT") == 0) {
        rc = ble_nrf_wait_ok(fd, 900000ULL);
    }
    close(fd);
    ble_log("nrf probe rc=%d", rc);
    return rc;
}

static int ble_nrf_parse_scan_line(char *line, int *done)
{
    static const char done_prefix[] = "+SCAN:DONE,";
    char *save = NULL;
    char *index_s;
    char *addr;
    char *rssi_s;
    char *uart_s;
    char *name;
    int rssi = 0;
    int rssi_valid = 0;
    int ble_uart = 0;
    int scan_index = -1;
    int scan_index_valid = 0;

    if(done) {
        *done = 0;
    }
    if(strncmp(line, done_prefix, strlen(done_prefix)) == 0) {
        if(done) {
            *done = 1;
        }
        return 0;
    }
    if(strncmp(line, "+SCAN:", 6) != 0) {
        return 0;
    }

    index_s = strtok_r(line + 6, ",", &save);
    addr = strtok_r(NULL, ",", &save);
    rssi_s = strtok_r(NULL, ",", &save);
    uart_s = strtok_r(NULL, ",", &save);
    name = save;
    if(name && *name == ',') {
        name++;
    }
    if(index_s && index_s[0]) {
        char *end = NULL;
        long parsed = strtol(index_s, &end, 10);
        if(end != index_s && parsed >= 0 && parsed <= 255) {
            scan_index = (int)parsed;
            scan_index_valid = 1;
        }
    }
    if(rssi_s && rssi_s[0]) {
        char *end = NULL;
        long parsed = strtol(rssi_s, &end, 10);
        if(end != rssi_s && parsed >= -130 && parsed <= 20) {
            rssi = (int)parsed;
            rssi_valid = 1;
        }
    }
    if(uart_s && atoi(uart_s) != 0) {
        ble_uart = 1;
    }

    if(addr && addr[0] && name && name[0] && strcmp(name, "Unknown") != 0) {
        ble_add_device(addr, name, rssi, rssi_valid, ble_uart, scan_index,
                       scan_index_valid);
    }
    return 0;
}

static int ble_nrf_scan(void)
{
    char cmd[32];
    char line[192];
    uint64_t deadline;
    FILE *scan_log;
    int fd;
    int seen_done = 0;

    fd = ble_nrf_open();
    if(fd < 0) {
        ble_set_status("nRF UART missing");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "AT+SCAN=%d", BLE_NRF_SCAN_SECONDS);
    scan_log = fopen(BLE_SCAN_LOG, "w");
    if(scan_log) {
        fprintf(scan_log, "scan adapter=nrf52840 dev=%s\n", BLE_NRF_UART_DEV);
        fclose(scan_log);
    }

    if(ble_nrf_write_cmd(fd, cmd) != 0) {
        close(fd);
        ble_set_status("nRF scan write failed");
        return -1;
    }

    deadline = ui_monotonic_us() + (uint64_t)(BLE_NRF_SCAN_SECONDS + 3) * 1000000ULL;
    while(ui_monotonic_us() < deadline && !seen_done) {
        uint64_t remain = deadline - ui_monotonic_us();
        char parse_line[sizeof(line)];

        if(ble_nrf_read_line(fd, line, sizeof(line), remain) < 0) {
            break;
        }
        scan_log = fopen(BLE_SCAN_LOG, "a");
        if(scan_log) {
            fprintf(scan_log, "%s\n", line);
            fclose(scan_log);
        }
        if(strcmp(line, "OK") == 0) {
            continue;
        }
        if(strncmp(line, "ERR:", 4) == 0) {
            ble_set_status(line);
            break;
        }
        snprintf(parse_line, sizeof(parse_line), "%s", line);
        ble_nrf_parse_scan_line(parse_line, &seen_done);
    }

    close(fd);
    ble_log("nrf scan end devices=%d done=%d", ble_device_count, seen_done);
    return seen_done ? 0 : -1;
}

static int ble_nrf_connect_device(const char *addr, int scan_index,
                                  int scan_index_valid)
{
    char cmd[64];
    char line[192];
    uint64_t deadline;
    int fd;
    int rc = -1;

    if(!addr || !addr[0]) {
        return -1;
    }
    fd = ble_nrf_open();
    if(fd < 0) {
        ble_set_status("nRF UART missing");
        return -1;
    }

    if(scan_index_valid) {
        snprintf(cmd, sizeof(cmd), "AT+CONN=%d", scan_index);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CONN=%s", addr);
    }
    ble_log("nrf connect begin addr=%s index=%d valid=%d cmd=%s",
            addr, scan_index, scan_index_valid, cmd);
    if(ble_nrf_write_cmd(fd, cmd) != 0) {
        close(fd);
        ble_set_status("nRF connect write failed");
        return -1;
    }

    deadline = ui_monotonic_us() + 6000000ULL;
    while(ui_monotonic_us() < deadline) {
        uint64_t remain = deadline - ui_monotonic_us();

        if(ble_nrf_read_line(fd, line, sizeof(line), remain) < 0) {
            break;
        }
        if(strcmp(line, "OK") == 0) {
            rc = 0;
            continue;
        }
        if(strncmp(line, "+CONNECTED:", 11) == 0) {
            rc = 0;
            ble_set_status(line);
            break;
        }
        if(strncmp(line, "ERR:", 4) == 0) {
            ble_set_status(line);
            rc = -1;
            break;
        }
    }

    close(fd);
    ble_log("nrf connect addr=%s index=%d valid=%d rc=%d", addr, scan_index,
            scan_index_valid, rc);
    return rc;
}

static int ble_parse_first_index(const char *line, const char *prefix)
{
    const char *p;
    char *end = NULL;
    long value;

    if(!line || !prefix || strncmp(line, prefix, strlen(prefix)) != 0) {
        return -1;
    }
    p = line + strlen(prefix);
    value = strtol(p, &end, 10);
    if(end == p || value < 0 || value > 1024) {
        return -1;
    }
    return (int)value;
}

static int ble_nrf_collect_gatt_command(int fd, const char *cmd,
                                        const char *done_prefix,
                                        const char *item_prefix,
                                        char *out, size_t out_len,
                                        int *max_index)
{
    char line[224];
    uint64_t deadline;
    int accepted = 0;
    int done = 0;

    if(max_index) {
        *max_index = -1;
    }
    if(ble_nrf_write_cmd(fd, cmd) != 0) {
        ble_text_append(out, out_len, "%s -> write failed\n", cmd);
        return -1;
    }

    ble_text_append(out, out_len, "\n%s\n", cmd);
    deadline = ui_monotonic_us() + 10000000ULL;
    while(ui_monotonic_us() < deadline) {
        uint64_t remain = deadline - ui_monotonic_us();

        if(ble_nrf_read_line(fd, line, sizeof(line), remain) < 0) {
            break;
        }
        if(strcmp(line, "OK") == 0) {
            accepted = 1;
            continue;
        }
        ble_text_append(out, out_len, "%s\n", line);
        if(item_prefix && max_index &&
           strncmp(line, item_prefix, strlen(item_prefix)) == 0 &&
           !strstr(line, ":DONE,")) {
            int index = ble_parse_first_index(line, item_prefix);
            if(index > *max_index) {
                *max_index = index;
            }
        }
        if(done_prefix && strncmp(line, done_prefix, strlen(done_prefix)) == 0) {
            done = 1;
            break;
        }
        if(strncmp(line, "ERR:", 4) == 0 ||
           strcmp(line, "+GATT:TIMEOUT") == 0) {
            break;
        }
    }

    if(!accepted) {
        ble_text_append(out, out_len, "%s -> not accepted\n", cmd);
    }
    return done ? 0 : -1;
}

static int ble_nrf_collect_gatt_command_retry_busy(int fd, const char *cmd,
                                                   const char *done_prefix,
                                                   const char *item_prefix,
                                                   char *out, size_t out_len,
                                                   int *max_index)
{
    for(int attempt = 0; attempt < 4; attempt++) {
        size_t before = out ? strlen(out) : 0;
        int rc = ble_nrf_collect_gatt_command(fd, cmd, done_prefix,
                                              item_prefix, out, out_len,
                                              max_index);

        if(rc == 0) {
            return 0;
        }
        if(!out || !strstr(out + before, "ERR:GATTS,17")) {
            return rc;
        }
        ble_text_append(out, out_len,
                        "GATT busy after connect, retry %d/3\n", attempt + 1);
        ble_log("nrf gatt busy cmd=%s retry=%d", cmd, attempt + 1);
        usleep(250000);
    }
    return -1;
}

static int ble_nrf_collect_gatt(const char *addr, int scan_index,
                                int scan_index_valid, char *out,
                                size_t out_len)
{
    int fd;
    int svc_max = -1;
    int chr_max = -1;
    int local_max;
    char line[192];

    fd = ble_nrf_open();
    if(fd < 0) {
        ble_text_append(out, out_len, "nRF UART missing\n");
        return -1;
    }

    ble_text_append(out, out_len,
                    "Backend: nRF52840 AT\nDevice: %s\nScan index: %s%d\n",
                    addr, scan_index_valid ? "" : "n/a ",
                    scan_index_valid ? scan_index : -1);
    (void)ble_nrf_write_cmd(fd, "AT+STATUS?");
    while(ble_nrf_read_line(fd, line, sizeof(line), 500000ULL) > 0) {
        if(strcmp(line, "OK") == 0) {
            break;
        }
        ble_text_append(out, out_len, "%s\n", line);
    }

    close(fd);
    (void)ble_nrf_connect_device(addr, scan_index, scan_index_valid);

    fd = ble_nrf_open();
    if(fd < 0) {
        ble_text_append(out, out_len, "nRF UART missing after connect\n");
        return -1;
    }

    if(ble_nrf_collect_gatt_command_retry_busy(fd, "AT+GATTS?",
                                               "+GATTS:DONE,", "+GATTS:",
                                               out, out_len, &svc_max) != 0) {
        close(fd);
        return -1;
    }

    if(svc_max > 7) {
        svc_max = 7;
        ble_text_append(out, out_len, "service list truncated to 8 entries\n");
    }

    for(int svc = 0; svc <= svc_max; svc++) {
        char cmd[32];

        snprintf(cmd, sizeof(cmd), "AT+GATTC=%d", svc);
        local_max = -1;
        (void)ble_nrf_collect_gatt_command(fd, cmd, "+GATTC:DONE,",
                                           "+GATTC:", out, out_len,
                                           &local_max);
        if(local_max > chr_max) {
            chr_max = local_max;
        }
    }

    if(chr_max > 23) {
        chr_max = 23;
        ble_text_append(out, out_len, "characteristic list truncated to 24 entries\n");
    }

    for(int chr = 0; chr <= chr_max; chr++) {
        char cmd[32];

        snprintf(cmd, sizeof(cmd), "AT+GATTD=%d", chr);
        (void)ble_nrf_collect_gatt_command(fd, cmd, "+GATTD:DONE,",
                                           "+GATTD:", out, out_len, NULL);
    }

    close(fd);
    return 0;
}

static int ble_hci_collect_gatt(const char *adapter, const char *addr,
                                char *out, size_t out_len)
{
    char cmd[512];
    char line[256];
    FILE *fp;
    int saw_attr = 0;
    int saw_connected = 0;
    int saw_failed = 0;
    int rc;

    ble_text_append(out, out_len, "Backend: Linux HCI %s\nDevice: %s\n",
                    adapter ? adapter : "hci0", addr);
    snprintf(cmd, sizeof(cmd),
             "{ printf 'select %s\\npower on\\nagent NoInputNoOutput\\n"
             "default-agent\\nscan off\\ntrust %s\\nconnect %s\\ninfo %s\\n'; "
             "sleep 5; printf 'info %s\\nmenu gatt\\nlist-attributes\\nquit\\n'; } | "
             "bluetoothctl 2>&1",
             adapter ? adapter : "hci0", addr, addr, addr, addr);

    fp = popen(cmd, "r");
    if(!fp) {
        ble_text_append(out, out_len, "bluetoothctl popen failed\n");
        return -1;
    }

    while(fgets(line, sizeof(line), fp) != NULL) {
        ui_trim_text(line);
        if(!line[0]) {
            continue;
        }
        ble_text_append(out, out_len, "%s\n", line);
        if(strstr(line, "/org/bluez/") || strstr(line, "Service") ||
           strstr(line, "Characteristic") || strstr(line, "Descriptor")) {
            saw_attr = 1;
        }
        if(strstr(line, "Connection successful") ||
           strstr(line, "Connected: yes") ||
           strstr(line, "Connected: true")) {
            saw_connected = 1;
        }
        if(strstr(line, "Failed") || strstr(line, "failed") ||
           strstr(line, "not available") || strstr(line, "No default controller")) {
            saw_failed = 1;
        }
    }

    rc = ui_shell_exit_code(pclose(fp));
    ble_text_append(out, out_len, "bluetoothctl exit=%d\n", rc);
    if(saw_connected && !saw_attr) {
        ble_text_append(out, out_len,
                        "Connected, no GATT attributes reported\n");
    }
    if(saw_connected || saw_attr) {
        return 0;
    }
    if(saw_failed) {
        ble_text_append(out, out_len, "Connection failed, see raw output above\n");
    }
    return -1;
}

static int ble_ensure_adapter(void)
{
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    char cmd[128];
    const char *enable_uart_hci;
    int rc;

    ble_log("ensure begin");
    if(!ble_custom_mode_enabled()) {
        return ble_prepare_hci_adapter_only();
    }
    ble_shell_run("modprobe bluetooth >/tmp/k230_ble_modprobe.log 2>&1 || true; "
                  "modprobe btusb >>/tmp/k230_ble_modprobe.log 2>&1 || true");
    ble_log("modprobe bluetooth/btusb requested");

    ble_shell_run("for p in $(pidof hciattach 2>/dev/null); do "
                  "cmd=$(tr '\\0' ' ' </proc/$p/cmdline 2>/dev/null); "
                  "case \"$cmd\" in *ttyS1*) kill \"$p\" >/dev/null 2>&1 || true;; esac; "
                  "done");
    if(ble_nrf_probe() == 0) {
        pthread_mutex_lock(&ble_lock);
        ble_adapter_present = 1;
        ble_adapter_ready = 1;
        ble_backend = BLE_BACKEND_NRF_AT;
        snprintf(ble_adapter_name, sizeof(ble_adapter_name), "%s", "nrf52840");
        pthread_mutex_unlock(&ble_lock);
        ble_log("nrf backend ready dev=%s", BLE_NRF_UART_DEV);
        return 0;
    }

    if(ble_find_adapter(adapter, sizeof(adapter))) {
        ble_start_bluetoothd_once();
        snprintf(cmd, sizeof(cmd), "hciconfig %s up >/tmp/k230_ble_hciup.log 2>&1",
                 adapter);
        rc = ble_shell_run(cmd);
        pthread_mutex_lock(&ble_lock);
        ble_adapter_present = 1;
        ble_adapter_ready = rc == 0;
        ble_backend = BLE_BACKEND_HCI;
        snprintf(ble_adapter_name, sizeof(ble_adapter_name), "%s", adapter);
        pthread_mutex_unlock(&ble_lock);
        ble_log("hciconfig %s up rc=%d", adapter, rc);
        return rc;
    }

    enable_uart_hci = getenv("K230_BLE_ENABLE_UART_HCI");
    if(!enable_uart_hci || strcmp(enable_uart_hci, "1") != 0) {
        ble_update_adapter_cache();
        ble_log("UART HCI fallback disabled; set K230_BLE_ENABLE_UART_HCI=1 to use /dev/ttyS1 hciattach");
        return -1;
    }
    if(!ble_command_exists("hciattach")) {
        ble_update_adapter_cache();
        ble_log("no USB HCI and hciattach missing");
        return -1;
    }

    ble_shell_run("if ! pidof hciattach >/dev/null 2>&1; then "
                  "hciattach -s 1500000 /dev/ttyS1 any 1500000 flow nosleep "
                  ">" BLE_HCIATTACH_LOG " 2>&1 & "
                  "fi");
    usleep(2500000);

    if(!ble_find_adapter(adapter, sizeof(adapter))) {
        ble_update_adapter_cache();
        ble_log("no HCI adapter after hciattach fallback");
        return -1;
    }

    ble_start_bluetoothd_once();
    snprintf(cmd, sizeof(cmd), "hciconfig %s up >/tmp/k230_ble_hciup.log 2>&1",
             adapter);
    rc = ble_shell_run(cmd);
    pthread_mutex_lock(&ble_lock);
    ble_adapter_present = 1;
    ble_adapter_ready = rc == 0;
    ble_backend = BLE_BACKEND_HCI;
    snprintf(ble_adapter_name, sizeof(ble_adapter_name), "%s", adapter);
    pthread_mutex_unlock(&ble_lock);
    ble_log("fallback hciconfig %s up rc=%d", adapter, rc);
    return rc;
}

static int ble_prepare_hci_adapter_only(void)
{
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    char cmd[128];
    int rc;

    ble_update_adapter_cache();
    if(!ble_find_adapter(adapter, sizeof(adapter))) {
        pthread_mutex_lock(&ble_lock);
        ble_adapter_present = 0;
        ble_adapter_ready = 0;
        ble_backend = BLE_BACKEND_HCI;
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "Meshtastic is using nRF52840");
        pthread_mutex_unlock(&ble_lock);
        ble_log("scan skipped: meshtastic owns nRF52840 and no HCI adapter");
        return -1;
    }

    ble_start_bluetoothd_once();
    snprintf(cmd, sizeof(cmd), "hciconfig %s up >/tmp/k230_ble_hciup.log 2>&1",
             adapter);
    rc = ble_shell_run(cmd);
    pthread_mutex_lock(&ble_lock);
    ble_adapter_present = 1;
    ble_adapter_ready = rc == 0;
    ble_backend = BLE_BACKEND_HCI;
    snprintf(ble_adapter_name, sizeof(ble_adapter_name), "%s", adapter);
    if(rc != 0) {
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "HCI adapter up failed");
    }
    pthread_mutex_unlock(&ble_lock);
    ble_log("hci-only adapter=%s rc=%d", adapter, rc);
    return rc;
}

static int ble_addr_valid(const char *addr)
{
    size_t i;

    if(!addr || strlen(addr) != 17U) {
        return 0;
    }

    for(i = 0; i < 17U; i++) {
        if((i + 1U) % 3U == 0U) {
            if(addr[i] != ':') {
                return 0;
            }
        } else if(!isxdigit((unsigned char)addr[i])) {
            return 0;
        }
    }

    return 1;
}

static int ble_name_is_addr_like(const char *name)
{
    size_t len;

    if(!name) {
        return 0;
    }
    len = strlen(name);
    if(len != 17U) {
        return 0;
    }
    for(size_t i = 0; i < len; i++) {
        if((i + 1U) % 3U == 0U) {
            if(name[i] != ':' && name[i] != '-') {
                return 0;
            }
        } else if(!isxdigit((unsigned char)name[i])) {
            return 0;
        }
    }
    return 1;
}

static int ble_name_is_useful(const char *name)
{
    if(!name) {
        return 0;
    }
    while(*name && isspace((unsigned char)*name)) {
        name++;
    }
    if(!name[0]) {
        return 0;
    }
    if(ble_name_is_addr_like(name)) {
        return 0;
    }
    if(strncasecmp(name, "RSSI", 4) == 0 ||
       strncasecmp(name, "Pairable", 8) == 0 ||
       strncasecmp(name, "LegacyPairing", 13) == 0 ||
       strncasecmp(name, "ManufacturerData", 16) == 0 ||
       strncasecmp(name, "ServiceData", 11) == 0) {
        return 0;
    }
    if(name[0] == '(' || strcmp(name, "unknown") == 0 ||
       strcmp(name, "(unknown)") == 0) {
        return 0;
    }
    return 1;
}

static int ble_parse_rssi_value(const char *text, int *rssi)
{
    const char *p;
    char *end;
    long value;

    if(!text || !rssi) {
        return 0;
    }
    p = strchr(text, '(');
    if(p) {
        value = strtol(p + 1, &end, 10);
        if(end && *end == ')' && value <= 0 && value >= -127) {
            *rssi = (int)value;
            return 1;
        }
    }
    p = strstr(text, "RSSI:");
    if(!p) {
        return 0;
    }
    p += 5;
    while(*p && isspace((unsigned char)*p)) {
        p++;
    }
    value = strtol(p, &end, 10);
    if(end != p && value <= 0 && value >= -127) {
        *rssi = (int)value;
        return 1;
    }
    return 0;
}

static void ble_remove_device(const char *addr)
{
    int i;

    if(!ble_addr_valid(addr)) {
        return;
    }

    pthread_mutex_lock(&ble_lock);
    for(i = 0; i < ble_device_count; i++) {
        if(strcmp(ble_devices[i].addr, addr) == 0) {
            if(i + 1 < ble_device_count) {
                memmove(&ble_devices[i], &ble_devices[i + 1],
                        (size_t)(ble_device_count - i - 1) *
                        sizeof(ble_devices[0]));
            }
            ble_device_count--;
            if(ble_selected == i) {
                ble_selected = -1;
            } else if(ble_selected > i) {
                ble_selected--;
            }
            break;
        }
    }
    pthread_mutex_unlock(&ble_lock);
}

static void ble_add_device(const char *addr, const char *name, int rssi,
                           int rssi_valid, int ble_uart, int scan_index,
                           int scan_index_valid)
{
    int i;

    if(!ble_addr_valid(addr)) {
        return;
    }

    pthread_mutex_lock(&ble_lock);
    for(i = 0; i < ble_device_count; i++) {
        if(strcmp(ble_devices[i].addr, addr) == 0) {
            if(name && name[0]) {
                snprintf(ble_devices[i].name, sizeof(ble_devices[i].name),
                         "%s", name);
            }
            if(rssi_valid) {
                ble_devices[i].rssi_dbm = rssi;
                ble_devices[i].rssi_valid = 1;
            }
            if(ble_uart) {
                ble_devices[i].ble_uart = 1;
            }
            if(scan_index_valid) {
                ble_devices[i].scan_index = scan_index;
                ble_devices[i].scan_index_valid = 1;
            }
            pthread_mutex_unlock(&ble_lock);
            return;
        }
    }

    if(!ble_name_is_useful(name) && !ble_uart) {
        pthread_mutex_unlock(&ble_lock);
        return;
    }

    if(ble_device_count < BLE_MAX_DEVICES) {
        snprintf(ble_devices[ble_device_count].addr,
                 sizeof(ble_devices[ble_device_count].addr), "%s", addr);
        snprintf(ble_devices[ble_device_count].name,
                 sizeof(ble_devices[ble_device_count].name), "%s",
                 (name && name[0]) ? name : "Unknown device");
        ble_devices[ble_device_count].rssi_dbm = rssi;
        ble_devices[ble_device_count].rssi_valid = rssi_valid ? 1 : 0;
        ble_devices[ble_device_count].ble_uart = ble_uart ? 1 : 0;
        ble_devices[ble_device_count].scan_index = scan_index;
        ble_devices[ble_device_count].scan_index_valid =
            scan_index_valid ? 1 : 0;
        ble_device_count++;
    }
    pthread_mutex_unlock(&ble_lock);
}

static void ble_parse_scan_line(char *line)
{
    char addr[BLE_ADDR_LEN];
    char candidate[BLE_ADDR_LEN];
    char *name;
    char *field;
    size_t len;
    size_t i;
    int rssi;

    ui_trim_text(line);
    if(!line[0] || strncmp(line, "LE Scan", 7) == 0) {
        return;
    }
    len = strlen(line);
    if(len < 17U) {
        return;
    }

    addr[0] = '\0';
    for(i = 0; i + 17U <= len; i++) {
        memcpy(candidate, line + i, 17);
        candidate[17] = '\0';
        if(ble_addr_valid(candidate)) {
            snprintf(addr, sizeof(addr), "%s", candidate);
            break;
        }
    }
    if(!addr[0]) {
        if(strstr(line, "failed") || strstr(line, "Failed")) {
            ble_set_status(line);
        }
        return;
    }

    if(strstr(line, "[DEL] Device")) {
        ble_remove_device(addr);
        return;
    }

    if(ble_parse_rssi_value(line, &rssi)) {
        ble_add_device(addr, NULL, rssi, 1, 0, -1, 0);
        return;
    }

    field = strstr(line, " Name: ");
    if(field) {
        name = field + 7;
    } else if((field = strstr(line, " Alias: ")) != NULL) {
        name = field + 8;
    } else {
        name = strstr(line, addr);
        name = name ? name + 17 : line + len;
    }
    while(*name && isspace((unsigned char)*name)) {
        name++;
    }
    if(strncmp(name, "Device ", 7) == 0) {
        name += 7;
    }
    ui_trim_text(name);
    if(!ble_name_is_useful(name)) {
        ble_scan_log_line("[filter] ignored addr=%s name=%s", addr, name);
        return;
    }
    ble_add_device(addr, name, 0, 0, 0, -1, 0);
}

static int ble_collect_scan_output(int fd, pid_t pid, const char *method,
                                   uint64_t duration_us)
{
    int flags;
    uint64_t deadline;
    char pending[256];
    size_t pending_len = 0;
    int status = 0;
    int count;

    flags = fcntl(fd, F_GETFL, 0);
    if(flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    deadline = ui_monotonic_us() + duration_us;
    while(ui_monotonic_us() < deadline) {
        fd_set rfds;
        struct timeval tv;
        char buf[128];
        ssize_t n;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        if(select(fd + 1, &rfds, NULL, NULL, &tv) > 0 &&
           FD_ISSET(fd, &rfds)) {
            n = read(fd, buf, sizeof(buf));
            if(n > 0) {
                ssize_t i;
                for(i = 0; i < n; i++) {
                    if(buf[i] == '\n' || pending_len + 1U >= sizeof(pending)) {
                        pending[pending_len] = '\0';
                        ble_scan_log_line("[%s] %s", method, pending);
                        ble_parse_scan_line(pending);
                        pending_len = 0;
                    } else if(buf[i] != '\r') {
                        pending[pending_len++] = buf[i];
                    }
                }
            } else if(n == 0) {
                break;
            } else if(errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
    }

    if(pending_len > 0) {
        pending[pending_len] = '\0';
        ble_scan_log_line("[%s] %s", method, pending);
        ble_parse_scan_line(pending);
    }

    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    close(fd);

    pthread_mutex_lock(&ble_lock);
    count = ble_device_count;
    pthread_mutex_unlock(&ble_lock);
    ble_log("%s scan end devices=%d status=%d", method, count, status);
    return count;
}

static int ble_scan_hcitool(void)
{
    int pipefd[2];
    pid_t pid;
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    char diag_cmd[128];
    FILE *scan_log;

    if(!ble_command_exists("hcitool")) {
        ble_set_status("hcitool missing");
        return -1;
    }

    if(pipe(pipefd) != 0) {
        ble_set_status("scan pipe failed");
        return -1;
    }

    pthread_mutex_lock(&ble_lock);
    snprintf(adapter, sizeof(adapter), "%s", ble_adapter_name);
    pthread_mutex_unlock(&ble_lock);
    scan_log = fopen(BLE_SCAN_LOG, "w");
    if(scan_log) {
        fprintf(scan_log, "scan method=hcitool adapter=%s\n", adapter);
        fclose(scan_log);
    }
    snprintf(diag_cmd, sizeof(diag_cmd),
             "hciconfig %s -a >>" BLE_SCAN_LOG " 2>&1", adapter);
    ble_shell_run(diag_cmd);
    ble_log("hcitool scan begin adapter=%s", adapter);

    pid = fork();
    if(pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        ble_set_status("scan fork failed");
        return -1;
    }

    if(pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("hcitool", "hcitool", "-i", adapter, "lescan", "--duplicates",
               NULL);
        _exit(127);
    }

    close(pipefd[1]);
    return ble_collect_scan_output(pipefd[0], pid, "hcitool",
                                   BLE_SCAN_DURATION_US);
}

static int ble_scan_bluetoothctl(void)
{
    int pipefd[2];
    pid_t pid;
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    char script[256];

    if(!ble_command_exists("bluetoothctl")) {
        ble_set_status("bluetoothctl missing");
        return -1;
    }

    if(pipe(pipefd) != 0) {
        ble_set_status("scan pipe failed");
        return -1;
    }

    pthread_mutex_lock(&ble_lock);
    snprintf(adapter, sizeof(adapter), "%s", ble_adapter_name);
    pthread_mutex_unlock(&ble_lock);
    ble_scan_log_line("scan method=bluetoothctl adapter=%s", adapter);
    ble_log("bluetoothctl scan begin adapter=%s", adapter);

    pid = fork();
    if(pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        ble_set_status("scan fork failed");
        return -1;
    }

    if(pid == 0) {
        snprintf(script, sizeof(script),
                 "{ printf 'select %s\\npower on\\nscan on\\n'; sleep 8; "
                 "printf 'scan off\\nquit\\n'; } | bluetoothctl",
                 adapter);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("sh", "sh", "-c", script, NULL);
        _exit(127);
    }

    close(pipefd[1]);
    return ble_collect_scan_output(pipefd[0], pid, "bluetoothctl",
                                   BLE_SCAN_DURATION_US + 1000000ULL);
}

static void *ble_start_thread_cb(void *arg)
{
    int rc;

    (void)arg;
    pthread_mutex_lock(&ble_lock);
    ble_busy = 1;
    snprintf(ble_status, sizeof(ble_status), "%s", "Starting adapter...");
    pthread_mutex_unlock(&ble_lock);

    rc = ble_ensure_adapter();

    pthread_mutex_lock(&ble_lock);
    ble_busy = 0;
    if(rc == 0 && ble_adapter_ready) {
        snprintf(ble_status, sizeof(ble_status), "%s", "Adapter ready");
    } else if(ble_adapter_present) {
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "Adapter up failed, see /tmp/k230_ble_hciup.log");
    } else {
        snprintf(ble_status, sizeof(ble_status), "%s", "No BLE adapter");
    }
    pthread_mutex_unlock(&ble_lock);
    return NULL;
}

static void *ble_scan_thread_cb(void *arg)
{
    int rc;
    ble_backend_t backend;
    ble_mode_t mode;
    int enabled;

    (void)arg;
    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    enabled = ble_scan_enabled;
    mode = ble_mode;
    if(!enabled) {
        ble_scanning = 0;
        ble_busy = 0;
        snprintf(ble_status, sizeof(ble_status), "%s", "Bluetooth off");
        pthread_mutex_unlock(&ble_lock);
        return NULL;
    }
    if(mode == BLE_MODE_MESH_EXCLUSIVE) {
        ble_scanning = 0;
        ble_busy = 0;
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "Meshtastic BLE exclusive");
        pthread_mutex_unlock(&ble_lock);
        return NULL;
    }
    ble_busy = 1;
    ble_scanning = 1;
    snprintf(ble_status, sizeof(ble_status), "%s", "Scanning...");
    pthread_mutex_unlock(&ble_lock);

    rc = ble_ensure_adapter();
    if(rc == 0 && ble_adapter_ready) {
        pthread_mutex_lock(&ble_lock);
        backend = ble_backend;
        pthread_mutex_unlock(&ble_lock);
        if(backend == BLE_BACKEND_NRF_AT) {
            ble_nrf_scan();
        } else {
            if(ble_scan_bluetoothctl() <= 0) {
                ble_scan_hcitool();
            }
        }
    }

    pthread_mutex_lock(&ble_lock);
    ble_scanning = 0;
    ble_busy = 0;
    if(!ble_adapter_ready && ble_adapter_present) {
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "Adapter up failed, see /tmp/k230_ble_hciup.log");
    } else if(!ble_adapter_ready) {
        snprintf(ble_status, sizeof(ble_status), "%s", "No BLE adapter");
    } else if(ble_device_count == 0) {
        snprintf(ble_status, sizeof(ble_status), "%s", "No BLE devices");
    } else {
        snprintf(ble_status, sizeof(ble_status), "%d %s",
                 ble_device_count, ui_tr("devices"));
    }
    pthread_mutex_unlock(&ble_lock);
    return NULL;
}

static void *ble_gatt_thread_cb(void *arg)
{
    ble_connect_req_t *req = (ble_connect_req_t *)arg;
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    ble_backend_t backend = BLE_BACKEND_HCI;
    char result[BLE_GATT_TEXT_MAX];
    int rc = -1;

    result[0] = '\0';

    pthread_mutex_lock(&ble_lock);
    ble_busy = 1;
    ble_gatt_busy = 1;
    ble_connected = 0;
    snprintf(ble_connected_addr, sizeof(ble_connected_addr), "%s", req->addr);
    snprintf(ble_connected_name, sizeof(ble_connected_name), "%s",
             req->name[0] ? req->name : req->addr);
    snprintf(ble_status, sizeof(ble_status), "%s", "Discovering GATT...");
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "Discovering GATT...");
    pthread_mutex_unlock(&ble_lock);

    if(ble_ensure_adapter() == 0 && ble_adapter_ready) {
        pthread_mutex_lock(&ble_lock);
        snprintf(adapter, sizeof(adapter), "%s", ble_adapter_name);
        backend = ble_backend;
        pthread_mutex_unlock(&ble_lock);

        if(backend == BLE_BACKEND_NRF_AT) {
            rc = ble_nrf_collect_gatt(req->addr, req->scan_index,
                                      req->scan_index_valid, result,
                                      sizeof(result));
        } else if(ble_command_exists("bluetoothctl")) {
            rc = ble_hci_collect_gatt(adapter, req->addr, result, sizeof(result));
        } else {
            ble_text_append(result, sizeof(result),
                            "bluetoothctl missing; GATT detail unavailable\n");
            rc = -1;
        }
    } else {
        ble_text_append(result, sizeof(result), "%s\n",
                        ble_adapter_present ?
                        "Adapter up failed, see /tmp/k230_ble_hciup.log" :
                        "No BLE adapter");
    }

    ble_gatt_store_result(result);

    pthread_mutex_lock(&ble_lock);
    ble_busy = 0;
    ble_gatt_busy = 0;
    ble_connected = rc == 0;
    snprintf(ble_status, sizeof(ble_status), "%s %s",
             rc == 0 ? "GATT ready" : "GATT discovery failed", req->addr);
    pthread_mutex_unlock(&ble_lock);

    free(req);
    return NULL;
}

static void ble_start_worker(void *(*fn)(void *), void *arg)
{
    pthread_t thread;
    int busy;

    pthread_mutex_lock(&ble_lock);
    busy = ble_busy;
    pthread_mutex_unlock(&ble_lock);
    if(busy) {
        free(arg);
        return;
    }

    if(pthread_create(&thread, NULL, fn, arg) == 0) {
        pthread_detach(thread);
    } else {
        free(arg);
        ble_set_status("Thread failed");
    }
}

static void ble_scan_switch_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    int enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ble_mode_t mode;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    ble_scan_enabled = enabled;
    mode = ble_mode;
    ble_next_scan_us = 0;
    if(!enabled) {
        ble_detail_mode = 0;
        ble_connected = 0;
        ble_connect_queued = 0;
        ble_gatt_busy = 0;
        ble_connected_addr[0] = '\0';
        ble_connected_name[0] = '\0';
        ble_queued_addr[0] = '\0';
        ble_queued_name[0] = '\0';
        ble_queued_scan_index = -1;
        ble_queued_scan_index_valid = 0;
        snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "No GATT data");
    }
    snprintf(ble_status, sizeof(ble_status), "%s",
             !enabled ? "Bluetooth off" :
             (mode == BLE_MODE_MESH_EXCLUSIVE ? "Meshtastic BLE exclusive" :
              "Bluetooth on"));
    pthread_mutex_unlock(&ble_lock);
    ble_save_enabled(enabled);
    ui_meshtastic_apply_ble_setting();

    ble_log("bluetooth switch %s mode=%s", enabled ? "on" : "off",
            ble_mode_pref_value(mode));
    if(enabled && mode == BLE_MODE_CUSTOM) {
        ble_start_worker(ble_scan_thread_cb, NULL);
    } else {
        ble_stop_bluetoothd_on_demand();
    }
    ble_refresh_ui();
    app_request_fast_refresh();
}

static void ble_mode_reset_runtime_locked(void)
{
    ble_device_count = 0;
    ble_selected = -1;
    ble_detail_mode = 0;
    ble_connected = 0;
    ble_connect_queued = 0;
    ble_gatt_busy = 0;
    ble_connected_addr[0] = '\0';
    ble_connected_name[0] = '\0';
    ble_queued_addr[0] = '\0';
    ble_queued_name[0] = '\0';
    ble_queued_scan_index = -1;
    ble_queued_scan_index_valid = 0;
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "No GATT data");
}

static void ble_mode_event_cb(lv_event_t *event)
{
    ble_mode_t mode = (ble_mode_t)(intptr_t)lv_event_get_user_data(event);
    int enabled;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    if(ble_mode == mode) {
        pthread_mutex_unlock(&ble_lock);
        return;
    }
    ble_mode = mode;
    enabled = ble_scan_enabled;
    ble_next_scan_us = 0;
    ble_mode_reset_runtime_locked();
    snprintf(ble_status, sizeof(ble_status), "%s",
             mode == BLE_MODE_MESH_EXCLUSIVE ?
             "Meshtastic BLE exclusive" : "Bluetooth on");
    pthread_mutex_unlock(&ble_lock);

    ble_save_mode(mode);
    ble_log("mode changed to %s", ble_mode_pref_value(mode));
    ui_meshtastic_apply_ble_setting();
    if(enabled && mode == BLE_MODE_CUSTOM) {
        ble_start_worker(ble_scan_thread_cb, NULL);
    } else {
        ble_stop_bluetoothd_on_demand();
    }
    ble_refresh_ui();
    app_request_fast_refresh();
}

static lv_obj_t *ble_mode_button_create(lv_obj_t *parent, int x, int y, int w,
                                        const char *text, ble_mode_t mode)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 48);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ble_mode_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)mode);

    label = ui_label(btn, text, &lv_font_montserrat_16, 0xC9D3DF);
    lv_obj_center(label);
    ui_make_click_forwarder(label);
    return btn;
}

static void ble_close_confirm(void)
{
    if(ble_confirm_overlay && lv_obj_is_valid(ble_confirm_overlay)) {
        lv_obj_delete(ble_confirm_overlay);
    }
    ble_confirm_overlay = NULL;
    free(ble_confirm_device);
    ble_confirm_device = NULL;
}

static void ble_update_connect_overlay(int show, const char *name,
                                       const char *addr)
{
    if(!show) {
        if(ble_connect_overlay && lv_obj_is_valid(ble_connect_overlay)) {
            lv_obj_delete(ble_connect_overlay);
        }
        ble_connect_overlay = NULL;
        ble_connect_overlay_label = NULL;
        return;
    }

    if(!ble_connect_overlay || !lv_obj_is_valid(ble_connect_overlay)) {
        lv_obj_t *dialog;
        lv_obj_t *spinner;
        int dialog_w = ui_screen_width() - (ui_is_landscape() ? 160 : 96);
        int dialog_h = 190;

        if(dialog_w > 520) {
            dialog_w = 520;
        }
        if(dialog_w < 300) {
            dialog_w = 300;
        }

        ble_connect_overlay = lv_obj_create(lv_layer_top());
        ui_set_fullscreen(ble_connect_overlay);
        lv_obj_set_style_bg_color(ble_connect_overlay, lv_color_hex(0x000000),
                                  0);
        lv_obj_set_style_bg_opa(ble_connect_overlay, LV_OPA_50, 0);
        lv_obj_set_style_border_width(ble_connect_overlay, 0, 0);
        lv_obj_set_style_pad_all(ble_connect_overlay, 0, 0);
        lv_obj_clear_flag(ble_connect_overlay, LV_OBJ_FLAG_SCROLLABLE);

        dialog = ui_panel(ble_connect_overlay, 0, 0, dialog_w, dialog_h);
        lv_obj_set_size(dialog, dialog_w, dialog_h);
        lv_obj_center(dialog);
        lv_obj_set_style_bg_color(dialog, lv_color_hex(0x151B22), 0);

        spinner = lv_spinner_create(dialog);
        lv_obj_set_size(spinner, 58, 58);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 22);

        ble_connect_overlay_label = ui_label(dialog, "Connecting...",
                                             &lv_font_montserrat_16, 0xC9D3DF);
        lv_obj_set_width(ble_connect_overlay_label, dialog_w - 48);
        lv_obj_set_style_text_align(ble_connect_overlay_label,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(ble_connect_overlay_label, LV_LABEL_LONG_WRAP);
        lv_obj_align(ble_connect_overlay_label, LV_ALIGN_TOP_MID, 0, 96);
    }

    if(ble_connect_overlay_label) {
        char text[BLE_GATT_TITLE_MAX * 2];

        snprintf(text, sizeof(text), "%s\n%s\n%s", ui_tr("Connecting..."),
                 name && name[0] ? name : "Bluetooth device",
                 addr && addr[0] ? addr : "--");
        lv_label_set_text(ble_connect_overlay_label, text);
    }
}

static void ble_show_device_list(void)
{
    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    ble_detail_mode = 0;
    ble_connected = 0;
    ble_connect_queued = 0;
    ble_gatt_busy = 0;
    ble_connected_addr[0] = '\0';
    ble_connected_name[0] = '\0';
    ble_queued_addr[0] = '\0';
    ble_queued_name[0] = '\0';
    ble_queued_scan_index = -1;
    ble_queued_scan_index_valid = 0;
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "No GATT data");
    snprintf(ble_status, sizeof(ble_status), "%s",
             !ble_scan_enabled ? "Bluetooth off" :
             (ble_mode == BLE_MODE_MESH_EXCLUSIVE ?
              "Meshtastic BLE exclusive" : "Bluetooth on"));
    ble_next_scan_us = 0;
    pthread_mutex_unlock(&ble_lock);
}

static void ble_detail_back_event_cb(lv_event_t *event)
{
    (void)event;
    ble_show_device_list();
}

static int ble_spawn_gatt_worker(ble_connect_req_t *req)
{
    pthread_t thread;

    if(!req) {
        return 0;
    }
    if(pthread_create(&thread, NULL, ble_gatt_thread_cb, req) == 0) {
        pthread_detach(thread);
        ble_log("gatt worker started addr=%s name=%s index=%d valid=%d",
                req->addr, req->name, req->scan_index,
                req->scan_index_valid);
        return 1;
    }
    free(req);
    pthread_mutex_lock(&ble_lock);
    ble_busy = 0;
    ble_gatt_busy = 0;
    ble_connect_queued = 0;
    ble_connected = 0;
    snprintf(ble_status, sizeof(ble_status), "%s", "Thread failed");
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "Thread failed");
    pthread_mutex_unlock(&ble_lock);
    return 0;
}

static int ble_open_device_detail(const char *addr, const char *name,
                                  int scan_index, int scan_index_valid)
{
    ble_connect_req_t *req;
    int busy;
    int enabled;
    ble_mode_t mode;

    if(!addr || !addr[0]) {
        return 0;
    }

    req = (ble_connect_req_t *)calloc(1, sizeof(*req));
    if(!req) {
        ble_set_status("No memory");
        return 0;
    }
    snprintf(req->addr, sizeof(req->addr), "%s", addr);
    snprintf(req->name, sizeof(req->name), "%s",
             name && name[0] ? name : addr);
    req->scan_index = scan_index;
    req->scan_index_valid = scan_index_valid ? 1 : 0;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    busy = ble_busy;
    enabled = ble_scan_enabled;
    mode = ble_mode;
    if(!enabled || mode != BLE_MODE_CUSTOM) {
        pthread_mutex_unlock(&ble_lock);
        free(req);
        ble_set_status(!enabled ? "Bluetooth off" :
                       "Meshtastic BLE exclusive");
        return 0;
    }

    ble_detail_mode = 1;
    ble_connected = 0;
    ble_next_scan_us = 0;
    snprintf(ble_connected_addr, sizeof(ble_connected_addr), "%s", req->addr);
    snprintf(ble_connected_name, sizeof(ble_connected_name), "%s", req->name);
    if(busy) {
        ble_connect_queued = 1;
        ble_gatt_busy = 1;
        snprintf(ble_queued_addr, sizeof(ble_queued_addr), "%s", req->addr);
        snprintf(ble_queued_name, sizeof(ble_queued_name), "%s", req->name);
        ble_queued_scan_index = req->scan_index;
        ble_queued_scan_index_valid = req->scan_index_valid;
        snprintf(ble_status, sizeof(ble_status), "%s",
                 "Waiting for scan to finish...");
        snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s",
                 "Waiting for scan to finish...");
        pthread_mutex_unlock(&ble_lock);
        free(req);
        ble_log("connect queued addr=%s name=%s index=%d valid=%d", addr,
                name ? name : "", req->scan_index, req->scan_index_valid);
        return 1;
    }

    ble_busy = 1;
    ble_gatt_busy = 1;
    snprintf(ble_status, sizeof(ble_status), "%s", "Connecting...");
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "Connecting...");
    pthread_mutex_unlock(&ble_lock);

    return ble_spawn_gatt_worker(req);
}

static void ble_try_start_queued_connection(void)
{
    ble_connect_req_t *req;
    int queued;
    int busy;
    int enabled;
    ble_mode_t mode;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    queued = ble_connect_queued;
    busy = ble_busy;
    enabled = ble_scan_enabled;
    mode = ble_mode;
    if(!queued || busy || !enabled || mode != BLE_MODE_CUSTOM) {
        pthread_mutex_unlock(&ble_lock);
        return;
    }
    req = (ble_connect_req_t *)calloc(1, sizeof(*req));
    if(!req) {
        ble_connect_queued = 0;
        ble_gatt_busy = 0;
        snprintf(ble_status, sizeof(ble_status), "%s", "No memory");
        pthread_mutex_unlock(&ble_lock);
        return;
    }
    snprintf(req->addr, sizeof(req->addr), "%s", ble_queued_addr);
    snprintf(req->name, sizeof(req->name), "%s",
             ble_queued_name[0] ? ble_queued_name : ble_queued_addr);
    req->scan_index = ble_queued_scan_index;
    req->scan_index_valid = ble_queued_scan_index_valid;
    ble_connect_queued = 0;
    ble_busy = 1;
    ble_gatt_busy = 1;
    ble_connected = 0;
    snprintf(ble_status, sizeof(ble_status), "%s", "Connecting...");
    snprintf(ble_gatt_text, sizeof(ble_gatt_text), "%s", "Connecting...");
    pthread_mutex_unlock(&ble_lock);

    (void)ble_spawn_gatt_worker(req);
}

static void ble_confirm_cancel_event_cb(lv_event_t *event)
{
    (void)event;
    ble_close_confirm();
}

static void ble_confirm_connect_event_cb(lv_event_t *event)
{
    (void)event;
    if(ble_confirm_device) {
        (void)ble_open_device_detail(ble_confirm_device->addr,
                                     ble_confirm_device->name,
                                     ble_confirm_device->scan_index,
                                     ble_confirm_device->scan_index_valid);
    }
    ble_close_confirm();
}

static void ble_open_connect_confirm(const char *addr, const char *name,
                                     int scan_index, int scan_index_valid)
{
    lv_obj_t *dialog;
    lv_obj_t *label;
    lv_obj_t *btn;
    ble_device_t *device;
    char text[BLE_GATT_TITLE_MAX];
    int dialog_w = ui_screen_width() - (ui_is_landscape() ? 144 : 48);
    int dialog_h = 254;
    int pad = 28;
    int button_w = (dialog_w - pad * 2 - 18) / 2;

    if(dialog_w > 560) {
        dialog_w = 560;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    button_w = (dialog_w - pad * 2 - 18) / 2;

    if(!addr || !addr[0]) {
        return;
    }
    device = (ble_device_t *)calloc(1, sizeof(*device));
    if(!device) {
        ble_set_status("No memory");
        return;
    }
    snprintf(device->addr, sizeof(device->addr), "%s", addr);
    snprintf(device->name, sizeof(device->name), "%s",
             name && name[0] ? name : addr);
    device->scan_index = scan_index;
    device->scan_index_valid = scan_index_valid ? 1 : 0;

    ble_close_confirm();
    ble_confirm_device = device;
    ble_confirm_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(ble_confirm_overlay);
    lv_obj_set_style_bg_color(ble_confirm_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ble_confirm_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ble_confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(ble_confirm_overlay, 0, 0);
    lv_obj_clear_flag(ble_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(ble_confirm_overlay, 0, 0, dialog_w, dialog_h);
    lv_obj_set_size(dialog, dialog_w, dialog_h);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x151B22), 0);

    label = ui_label(dialog, "Connect device", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, pad, 24);

    snprintf(text, sizeof(text), "%s\n%s", device->name, device->addr);
    label = ui_label(dialog, text, &lv_font_montserrat_18, 0xC9D3DF);
    lv_obj_set_width(label, dialog_w - pad * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, pad, 72);

    btn = ui_command_button(dialog, pad, dialog_h - 76, button_w, "Cancel",
                            0x64748B);
    lv_obj_add_event_cb(btn, ble_confirm_cancel_event_cb, LV_EVENT_CLICKED,
                        NULL);
    btn = ui_command_button(dialog, pad + button_w + 18, dialog_h - 76,
                            button_w, "Connect", 0x25C281);
    lv_obj_add_event_cb(btn, ble_confirm_connect_event_cb, LV_EVENT_CLICKED,
                        NULL);
}

static void ble_device_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    int enabled;
    ble_mode_t mode;
    char addr[BLE_ADDR_LEN];
    char name[BLE_NAME_MAX];
    int scan_index = -1;
    int scan_index_valid = 0;

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    enabled = ble_scan_enabled;
    mode = ble_mode;
    if(index >= 0 && index < ble_device_count) {
        ble_selected = index;
        snprintf(addr, sizeof(addr), "%s", ble_devices[index].addr);
        snprintf(name, sizeof(name), "%s", ble_devices[index].name);
        scan_index = ble_devices[index].scan_index;
        scan_index_valid = ble_devices[index].scan_index_valid;
    } else {
        addr[0] = '\0';
        name[0] = '\0';
    }
    pthread_mutex_unlock(&ble_lock);

    if(!enabled || mode != BLE_MODE_CUSTOM) {
        ble_set_status(!enabled ? "Bluetooth off" :
                       "Meshtastic BLE exclusive");
        return;
    }
    if(addr[0]) {
        ble_open_connect_confirm(addr, name, scan_index, scan_index_valid);
    }
}

static void ble_refresh_ui(void)
{
    int i;
    int count;
    int selected;
    int present;
    int ready;
    int scanning;
    int scan_enabled;
    ble_mode_t mode;
    int gatt_busy;
    int detail_mode;
    int connected;
    ble_backend_t backend;
    char status[BLE_STATUS_MAX];
    char gatt_text[BLE_GATT_TEXT_MAX];
    char adapter[sizeof(ble_adapter_name)] = "hci0";
    char connected_addr[BLE_ADDR_LEN];
    char connected_name[BLE_NAME_MAX];

    ble_update_adapter_cache();

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    count = ble_device_count;
    selected = ble_selected;
    present = ble_adapter_present;
    ready = ble_adapter_ready;
    scanning = ble_scanning;
    scan_enabled = ble_scan_enabled;
    mode = ble_mode;
    gatt_busy = ble_gatt_busy;
    detail_mode = ble_detail_mode;
    connected = ble_connected;
    backend = ble_backend;
    snprintf(status, sizeof(status), "%s", ble_status);
    snprintf(gatt_text, sizeof(gatt_text), "%s", ble_gatt_text);
    snprintf(adapter, sizeof(adapter), "%s", ble_adapter_name);
    snprintf(connected_addr, sizeof(connected_addr), "%s", ble_connected_addr);
    snprintf(connected_name, sizeof(connected_name), "%s", ble_connected_name);
    pthread_mutex_unlock(&ble_lock);

    if(!scan_enabled || mode == BLE_MODE_CUSTOM) {
        app_set_ble_status(!scan_enabled ? "offline" :
                           (connected ? "connected" :
                            (ready ? "ready" : "offline")));
    }

    if(ble_state_label) {
        uint32_t state_color;

        if(!scan_enabled) {
            lv_label_set_text(ble_state_label, ui_tr("Bluetooth off"));
        } else if(mode == BLE_MODE_MESH_EXCLUSIVE) {
            lv_label_set_text(ble_state_label, ui_tr("Meshtastic BLE exclusive"));
        } else if(ready) {
            char text[64];
            snprintf(text, sizeof(text), "%s  %s", ui_tr("Adapter ready"),
                     backend == BLE_BACKEND_NRF_AT ? "nRF52840 AT" : adapter);
            lv_label_set_text(ble_state_label, text);
        } else if(present) {
            char text[64];
            snprintf(text, sizeof(text), "%s  %s", ui_tr("Adapter present"),
                     backend == BLE_BACKEND_NRF_AT ? "nRF52840 AT" : adapter);
            lv_label_set_text(ble_state_label, text);
        } else {
            lv_label_set_text(ble_state_label, ui_tr("No BLE adapter"));
        }
        state_color = !scan_enabled ? 0x64748B :
                      (mode == BLE_MODE_MESH_EXCLUSIVE ? 0x3DA5FF :
                       (ready ? 0x25C281 :
                        (present ? 0xF5A524 : 0xEF4D5A)));
        lv_obj_set_style_text_color(ble_state_label, lv_color_hex(state_color), 0);
    }
    if(ble_status_label) {
        const char *status_text = !scan_enabled ? "Bluetooth off" :
                                  (mode == BLE_MODE_MESH_EXCLUSIVE ?
                                   "Meshtastic owns nRF52840" :
                                   (scanning ? "Scanning..." :
                                   (gatt_busy ? "Discovering GATT..." : status)));

        lv_label_set_text(ble_status_label, ui_tr(status_text));
    }
    if(ble_scan_switch) {
        if(scan_enabled) {
            lv_obj_add_state(ble_scan_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ble_scan_switch, LV_STATE_CHECKED);
        }
    }
    if(ble_mode_mesh_btn) {
        uint32_t bg = mode == BLE_MODE_MESH_EXCLUSIVE ? 0x1E3A8A : 0x1F2937;
        uint32_t border = mode == BLE_MODE_MESH_EXCLUSIVE ? 0x3DA5FF : 0x2A3037;
        lv_obj_set_style_bg_color(ble_mode_mesh_btn, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(ble_mode_mesh_btn, lv_color_hex(border), 0);
    }
    if(ble_mode_custom_btn) {
        uint32_t bg = mode == BLE_MODE_CUSTOM ? 0x14532D : 0x1F2937;
        uint32_t border = mode == BLE_MODE_CUSTOM ? 0x25C281 : 0x2A3037;
        lv_obj_set_style_bg_color(ble_mode_custom_btn, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(ble_mode_custom_btn, lv_color_hex(border), 0);
    }
    if(ble_list_title_label) {
        char text[64];
        snprintf(text, sizeof(text), "%s  %d", ui_tr("Available BLE devices"),
                 count);
        lv_label_set_text(ble_list_title_label,
                          !scan_enabled ? ui_tr("Bluetooth is off") :
                          (mode == BLE_MODE_MESH_EXCLUSIVE ?
                           ui_tr("Meshtastic BLE exclusive") : text));
    }
    if(ble_device_list) {
        if(detail_mode) {
            lv_obj_add_flag(ble_device_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(ble_device_list, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(ble_detail_panel) {
        if(detail_mode) {
            lv_obj_clear_flag(ble_detail_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ble_detail_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if(ble_detail_title_label) {
        lv_label_set_text(ble_detail_title_label,
                          connected_name[0] ? connected_name :
                          ui_tr("Bluetooth device"));
    }
    if(ble_detail_addr_label) {
        char text[BLE_GATT_TITLE_MAX];
        snprintf(text, sizeof(text), "%s  %s",
                 connected ? ui_tr("Connected") :
                 (gatt_busy ? ui_tr("Connecting") : ui_tr("Not connected")),
                 connected_addr[0] ? connected_addr : "--");
        lv_label_set_text(ble_detail_addr_label, text);
        lv_obj_set_style_text_color(ble_detail_addr_label,
                                    lv_color_hex(connected ? 0x25C281 :
                                                 (gatt_busy ? 0x3DA5FF :
                                                  0xEF4D5A)), 0);
    }
    if(ble_gatt_status_label) {
        lv_label_set_text(ble_gatt_status_label,
                          ui_tr(gatt_busy ? "Discovering GATT..." :
                                "Services, characteristics and descriptors"));
        lv_obj_set_style_text_color(ble_gatt_status_label,
                                    lv_color_hex(gatt_busy ? 0x3DA5FF :
                                                 0x9AA4AF), 0);
    }
    if(ble_gatt_detail_label) {
        lv_label_set_text(ble_gatt_detail_label, gatt_text);
    }
    ble_update_connect_overlay(scan_enabled && detail_mode && gatt_busy,
                               connected_name, connected_addr);

    for(i = 0; i < BLE_MAX_DEVICES; i++) {
        if(!ble_device_btn[i]) {
            continue;
        }
        if(i < count) {
            char name[BLE_NAME_MAX];
            char meta[96];
            char rssi_text[32];
            int rssi;
            int rssi_valid;
            int ble_uart;
            int scan_index;
            int scan_index_valid;
            uint32_t signal_color;

            pthread_mutex_lock(&ble_lock);
            snprintf(name, sizeof(name), "%s", ble_devices[i].name);
            rssi = ble_devices[i].rssi_dbm;
            rssi_valid = ble_devices[i].rssi_valid;
            ble_uart = ble_devices[i].ble_uart;
            scan_index = ble_devices[i].scan_index;
            scan_index_valid = ble_devices[i].scan_index_valid;
            if(scan_index_valid) {
                snprintf(meta, sizeof(meta), "%s  idx=%d%s",
                         ble_devices[i].addr, scan_index,
                         ble_uart ? "  BLE UART" : "");
            } else {
                snprintf(meta, sizeof(meta), "%s%s",
                         ble_devices[i].addr, ble_uart ? "  BLE UART" : "");
            }
            pthread_mutex_unlock(&ble_lock);

            lv_obj_clear_flag(ble_device_btn[i], LV_OBJ_FLAG_HIDDEN);
            wireless_signal_icon_update(ble_device_signal_icon[i], rssi,
                                        rssi_valid);
            signal_color = wireless_signal_color(rssi, rssi_valid);
            if(ble_device_type_icon[i]) {
                ble_type_icon_update(ble_device_type_icon[i], 0x3DA5FF);
            }
            if(ble_device_rssi_label[i]) {
                if(rssi_valid) {
                    snprintf(rssi_text, sizeof(rssi_text), "%d dBm", rssi);
                } else {
                    snprintf(rssi_text, sizeof(rssi_text), "--");
                }
                lv_label_set_text(ble_device_rssi_label[i], rssi_text);
                lv_obj_set_style_text_color(ble_device_rssi_label[i],
                                            lv_color_hex(signal_color), 0);
            }
            lv_obj_set_style_border_color(ble_device_btn[i],
                                          lv_color_hex(i == selected ?
                                                       0x25C281 : 0x2A3037), 0);
            lv_obj_set_style_bg_color(ble_device_btn[i],
                                      lv_color_hex(i == selected ?
                                                   0x1E3A2E : 0x171D24), 0);
            lv_label_set_text(ble_device_name[i], name[0] ? name : ui_tr("Unknown device"));
            lv_label_set_text(ble_device_meta[i], meta);
        } else {
            wireless_signal_icon_update(ble_device_signal_icon[i], 0, 0);
            if(ble_device_type_icon[i]) {
                ble_type_icon_update(ble_device_type_icon[i], 0x64748B);
            }
            if(ble_device_rssi_label[i]) {
                lv_label_set_text(ble_device_rssi_label[i], "--");
                lv_obj_set_style_text_color(ble_device_rssi_label[i],
                                            lv_color_hex(0x64748B), 0);
            }
            lv_obj_add_flag(ble_device_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ble_timer_cb(lv_timer_t *timer)
{
    int enabled;
    int busy;
    int connected;
    int detail_mode;
    ble_mode_t mode;
    uint64_t next_scan;
    uint64_t now = ui_monotonic_us();

    (void)timer;
    ble_refresh_ui();
    ble_try_start_queued_connection();

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    enabled = ble_scan_enabled;
    mode = ble_mode;
    busy = ble_busy;
    connected = ble_connected;
    detail_mode = ble_detail_mode;
    next_scan = ble_next_scan_us;
    if(enabled && mode == BLE_MODE_CUSTOM &&
       !busy && !connected && !detail_mode &&
       (next_scan == 0 || now >= next_scan)) {
        ble_next_scan_us = now + BLE_AUTO_SCAN_PERIOD_US;
    } else {
        enabled = 0;
    }
    pthread_mutex_unlock(&ble_lock);

    if(enabled) {
        ble_start_worker(ble_scan_thread_cb, NULL);
    }
}

void ui_ble_cleanup(void)
{
    int i;

    if(ble_timer) {
        lv_timer_delete(ble_timer);
        ble_timer = NULL;
    }
    ble_close_confirm();
    ble_update_connect_overlay(0, NULL, NULL);
    ble_summary_panel = NULL;
    ble_state_label = NULL;
    ble_status_label = NULL;
    ble_scan_switch = NULL;
    ble_mode_mesh_btn = NULL;
    ble_mode_custom_btn = NULL;
    ble_list_title_label = NULL;
    ble_device_list = NULL;
    ble_detail_panel = NULL;
    ble_detail_title_label = NULL;
    ble_detail_addr_label = NULL;
    ble_gatt_status_label = NULL;
    ble_gatt_detail_label = NULL;
    for(i = 0; i < BLE_MAX_DEVICES; i++) {
        ble_device_btn[i] = NULL;
        ble_device_type_icon[i] = NULL;
        ble_device_signal_icon[i] = NULL;
        ble_device_rssi_label[i] = NULL;
        ble_device_name[i] = NULL;
        ble_device_meta[i] = NULL;
    }
}

void ui_ble_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *list;
    lv_obj_t *gatt_panel;
    lv_obj_t *btn;
    int i;
    int landscape = ui_is_landscape();
    int panel_x = ui_page_side_margin();
    int panel_w = ui_screen_width() - panel_x * 2;
    int row_x = 0;
    int row_inner_shift = landscape ? 12 : 0;
    int row_right_shift = landscape ? 12 : 0;
    int summary_h = 206;
    int list_y = summary_h + 46;
    int list_h = landscape ?
                 ui_screen_height() - ui_page_top_y(144) - list_y - 24 : 820;

    if(panel_w < 520) {
        panel_w = 520;
    }
    if(list_h < 360) {
        list_h = 360;
    }

    pthread_mutex_lock(&ble_lock);
    ble_load_prefs_locked();
    pthread_mutex_unlock(&ble_lock);

    ui_create_header(scr, "Bluetooth");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    summary = ui_panel(body, panel_x, 20, panel_w, summary_h);
    ble_summary_panel = summary;
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x151B22), 0);

    ui_label(summary, "BLE devices", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    ble_state_label = ui_label(summary, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_align(ble_state_label, LV_ALIGN_TOP_LEFT, 0, 44);

    ble_status_label = ui_label(summary, "Ready", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(ble_status_label, ui_inner_width());
    lv_label_set_long_mode(ble_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ble_status_label, LV_ALIGN_TOP_LEFT, 0, 78);

    ble_scan_switch = lv_switch_create(summary);
    lv_obj_set_size(ble_scan_switch, 72, 38);
    lv_obj_align(ble_scan_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_add_event_cb(ble_scan_switch, ble_scan_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    {
        int mode_gap = 12;
        int mode_w = (panel_w - mode_gap) / 2;

        if(mode_w > 250) {
            mode_w = 250;
        }
        ble_mode_mesh_btn = ble_mode_button_create(summary, 0, 116, mode_w,
                                                   "Meshtastic",
                                                   BLE_MODE_MESH_EXCLUSIVE);
        ble_mode_custom_btn = ble_mode_button_create(summary, mode_w + mode_gap,
                                                     116, mode_w,
                                                     "Scan BLE",
                                                     BLE_MODE_CUSTOM);
    }

    ui_label(summary, "Tap a device name to connect and inspect GATT.",
             &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                     panel_w - 32);
    lv_label_set_long_mode(lv_obj_get_child(summary,
                                            lv_obj_get_child_count(summary) - 1),
                           LV_LABEL_LONG_WRAP);
    lv_obj_align(lv_obj_get_child(summary, lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 172);

    list = ui_panel(body, panel_x, list_y, panel_w, list_h);
    ble_device_list = list;
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    ble_list_title_label = ui_label(list, "Available BLE devices",
                                    &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(ble_list_title_label, LV_ALIGN_TOP_LEFT, 16, 0);

    for(i = 0; i < BLE_MAX_DEVICES; i++) {
        int row_w;
        int text_w;

        ble_device_btn[i] = lv_obj_create(list);
        lv_obj_set_pos(ble_device_btn[i], row_x, 48 + i * 82);
        row_w = panel_w - row_x;
        if(row_w < 488) {
            row_w = 488;
        }
        lv_obj_set_size(ble_device_btn[i], row_w, 70);
        lv_obj_set_style_bg_color(ble_device_btn[i], lv_color_hex(0x171D24), 0);
        lv_obj_set_style_bg_opa(ble_device_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ble_device_btn[i], 8, 0);
        lv_obj_set_style_border_width(ble_device_btn[i], 1, 0);
        lv_obj_set_style_border_color(ble_device_btn[i], lv_color_hex(0x2A3037), 0);
        lv_obj_set_style_pad_all(ble_device_btn[i], 12, 0);
        lv_obj_clear_flag(ble_device_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ble_device_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(ble_device_btn[i], 4);
        lv_obj_add_event_cb(ble_device_btn[i], ble_device_event_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        ble_device_type_icon[i] = ble_type_icon_create(ble_device_btn[i]);
        lv_obj_align(ble_device_type_icon[i], LV_ALIGN_LEFT_MID,
                     2 + row_inner_shift, 0);

        ble_device_signal_icon[i] = wireless_signal_icon_create(ble_device_btn[i]);
        lv_obj_align(ble_device_signal_icon[i], LV_ALIGN_RIGHT_MID,
                     -58 - row_right_shift, -8);

        ble_device_rssi_label[i] = ui_label(ble_device_btn[i], "--",
                                            &lv_font_montserrat_12, 0x64748B);
        lv_obj_set_width(ble_device_rssi_label[i], 90);
        lv_obj_set_style_text_align(ble_device_rssi_label[i],
                                    LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(ble_device_rssi_label[i], LV_ALIGN_RIGHT_MID,
                     -48 - row_right_shift, 18);
        ui_make_click_forwarder(ble_device_rssi_label[i]);

        text_w = row_w - 222 - row_inner_shift - row_right_shift;
        if(text_w < 248) {
            text_w = 248;
        }

        ble_device_name[i] = ui_label(ble_device_btn[i], "Unknown device",
                                      &lv_font_montserrat_18, 0xF2F5F8);
        lv_obj_set_width(ble_device_name[i], text_w);
        lv_label_set_long_mode(ble_device_name[i], LV_LABEL_LONG_DOT);
        lv_obj_align(ble_device_name[i], LV_ALIGN_TOP_LEFT,
                     46 + row_inner_shift, 0);
        ui_make_click_forwarder(ble_device_name[i]);

        ble_device_meta[i] = ui_label(ble_device_btn[i], "--",
                                      &lv_font_montserrat_14, 0x9AA4AF);
        lv_obj_set_width(ble_device_meta[i], text_w);
        lv_label_set_long_mode(ble_device_meta[i], LV_LABEL_LONG_DOT);
        lv_obj_align(ble_device_meta[i], LV_ALIGN_TOP_LEFT,
                     46 + row_inner_shift, 32);
        ui_make_click_forwarder(ble_device_meta[i]);
        lv_obj_add_flag(ble_device_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    gatt_panel = ui_panel(body, panel_x, list_y, panel_w, list_h);
    ble_detail_panel = gatt_panel;
    lv_obj_set_style_bg_color(gatt_panel, lv_color_hex(0x101418), 0);
    lv_obj_add_flag(gatt_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gatt_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(gatt_panel, LV_SCROLLBAR_MODE_AUTO);

    ble_detail_title_label = ui_label(gatt_panel, "Bluetooth device",
                                      &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_set_width(ble_detail_title_label, panel_w - 158);
    lv_label_set_long_mode(ble_detail_title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ble_detail_title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    btn = ui_command_button(gatt_panel, panel_w - 136, -6, 104, "Devices",
                            0x64748B);
    lv_obj_add_event_cb(btn, ble_detail_back_event_cb, LV_EVENT_CLICKED, NULL);

    ble_detail_addr_label = ui_label(gatt_panel, "--", &lv_font_montserrat_14,
                                     0x9AA4AF);
    lv_obj_set_width(ble_detail_addr_label, panel_w - 32);
    lv_label_set_long_mode(ble_detail_addr_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ble_detail_addr_label, LV_ALIGN_TOP_LEFT, 0, 42);

    ble_gatt_status_label = ui_label(gatt_panel,
                                     "Services, characteristics and descriptors",
                                     &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(ble_gatt_status_label, panel_w - 32);
    lv_label_set_long_mode(ble_gatt_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ble_gatt_status_label, LV_ALIGN_TOP_LEFT, 0, 72);

    ble_gatt_detail_label = ui_label(gatt_panel, "No GATT data",
                                     &lv_font_montserrat_14, 0xC9D3DF);
    lv_obj_set_width(ble_gatt_detail_label, panel_w - 32);
    lv_label_set_long_mode(ble_gatt_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ble_gatt_detail_label, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_obj_add_flag(gatt_panel, LV_OBJ_FLAG_HIDDEN);

    ble_timer = lv_timer_create(ble_timer_cb, 500, NULL);
    ble_refresh_ui();
    if(ble_custom_mode_enabled()) {
        ble_start_worker(ble_scan_thread_cb, NULL);
    }
}
