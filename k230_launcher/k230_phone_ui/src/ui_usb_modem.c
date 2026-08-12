#include "ui_usb_modem.h"

#include "ui_i18n.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define USB_MODEM_LOG "/tmp/k230_usb_modem.log"
#define USB_MODEM_DIAL_LOG "/tmp/k230_usb_modem_dial.log"
#define USB_MODEM_UDHCPC_PID "/tmp/k230_usb_modem_udhcpc.pid"
#define USB_MODEM_MAX_USB 5
#define USB_MODEM_MAX_IFACES 5
#define USB_MODEM_MAX_PORTS 8
#define USB_MODEM_LOG_MEM 3072
#define USB_MODEM_LOG_VIEW_MAX 4096

typedef struct {
    char name[32];
    char driver[48];
    char state[24];
    char ip[48];
} usb_modem_iface_t;

static lv_obj_t *usb_modem_status_label;
static lv_obj_t *usb_modem_usb_label;
static lv_obj_t *usb_modem_iface_label;
static lv_obj_t *usb_modem_ports_label;
static lv_obj_t *usb_modem_tools_label;
static lv_obj_t *usb_modem_log_label;
static lv_timer_t *usb_modem_timer;
static pthread_t usb_modem_worker;
static int usb_modem_worker_active;
static pthread_mutex_t usb_modem_lock = PTHREAD_MUTEX_INITIALIZER;
static char usb_modem_log_mem[USB_MODEM_LOG_MEM];
static char usb_modem_status[160] = "Ready";
static usb_modem_iface_t usb_modem_ifaces[USB_MODEM_MAX_IFACES];
static int usb_modem_iface_count;

static void usb_modem_log_refresh(void)
{
    if(usb_modem_log_label) {
        char text[USB_MODEM_LOG_VIEW_MAX];
        char line[256];
        size_t used = 0;
        FILE *fp;

        pthread_mutex_lock(&usb_modem_lock);
        if(usb_modem_log_mem[0]) {
            used = snprintf(text, sizeof(text), "App log:\n%s",
                            usb_modem_log_mem);
            if(used >= sizeof(text)) {
                used = sizeof(text) - 1U;
            }
        } else {
            used = snprintf(text, sizeof(text), "%s\n", ui_tr("No log yet"));
        }
        pthread_mutex_unlock(&usb_modem_lock);

        fp = popen("tail -60 " USB_MODEM_DIAL_LOG " 2>/dev/null", "r");
        if(fp && used < sizeof(text) - 1U) {
            used += snprintf(text + used, sizeof(text) - used,
                             "\nDial log:\n");
            while(fgets(line, sizeof(line), fp)) {
                if(used + strlen(line) + 1U >= sizeof(text)) {
                    break;
                }
                used += snprintf(text + used, sizeof(text) - used, "%s", line);
            }
            pclose(fp);
        }

        fp = popen("dmesg | grep -Ei 'usb|rndis|cdc|qmi|mbim|wwan|ttyUSB|ttyACM|modem|asr|qualcomm' | tail -20",
                   "r");
        if(fp && used < sizeof(text) - 1U) {
            used += snprintf(text + used, sizeof(text) - used,
                             "\nKernel USB tail:\n");
            while(fgets(line, sizeof(line), fp)) {
                if(used + strlen(line) + 1U >= sizeof(text)) {
                    break;
                }
                used += snprintf(text + used, sizeof(text) - used, "%s", line);
            }
            pclose(fp);
        }

        text[sizeof(text) - 1U] = '\0';
        lv_label_set_text(usb_modem_log_label, text);
    }
}

static void usb_modem_log_append(const char *fmt, ...)
{
    char line[320];
    size_t used;
    size_t add;
    FILE *fp;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&usb_modem_lock);
    used = strlen(usb_modem_log_mem);
    add = strlen(line);
    if(used + add + 2U >= sizeof(usb_modem_log_mem)) {
        size_t keep = sizeof(usb_modem_log_mem) / 2U;
        memmove(usb_modem_log_mem, usb_modem_log_mem + used - keep, keep + 1U);
        used = strlen(usb_modem_log_mem);
    }
    snprintf(usb_modem_log_mem + used, sizeof(usb_modem_log_mem) - used,
             "%s\n", line);
    pthread_mutex_unlock(&usb_modem_lock);

    fp = fopen(USB_MODEM_LOG, "a");
    if(fp) {
        fprintf(fp, "%s\n", line);
        fclose(fp);
    }
}

static void usb_modem_set_status(const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&usb_modem_lock);
    va_start(ap, fmt);
    vsnprintf(usb_modem_status, sizeof(usb_modem_status), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&usb_modem_lock);
}

static int usb_modem_read_text(const char *path, char *buf, size_t len)
{
    if(ui_read_file_first_line(path, buf, len) != 0) {
        if(len > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    ui_trim_text(buf);
    return buf[0] ? 0 : -1;
}

static int usb_modem_basename_from_link(const char *path, char *buf, size_t len)
{
    char link_buf[256];
    ssize_t rd;
    char *slash;

    if(!buf || len == 0) {
        return -1;
    }
    buf[0] = '\0';
    rd = readlink(path, link_buf, sizeof(link_buf) - 1U);
    if(rd < 0) {
        return -1;
    }
    link_buf[rd] = '\0';
    slash = strrchr(link_buf, '/');
    snprintf(buf, len, "%s", slash ? slash + 1 : link_buf);
    return buf[0] ? 0 : -1;
}

static int usb_modem_text_contains(const char *text, const char *needle)
{
    size_t nlen;

    if(!text || !needle) {
        return 0;
    }
    nlen = strlen(needle);
    if(nlen == 0) {
        return 1;
    }
    for(const char *p = text; *p; p++) {
        size_t i;
        for(i = 0; i < nlen; i++) {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)needle[i];
            if(!a || tolower(a) != tolower(b)) {
                break;
            }
        }
        if(i == nlen) {
            return 1;
        }
    }
    return 0;
}

static int usb_modem_driver_is_candidate(const char *driver)
{
    static const char *drivers[] = {
        "rndis_host",
        "cdc_ether",
        "cdc_ncm",
        "cdc_mbim",
        "qmi_wwan",
        "option",
        "usbserial",
    };

    if(!driver || !driver[0]) {
        return 0;
    }
    for(size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        if(strcmp(driver, drivers[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int usb_modem_iface_is_candidate(const char *name, const char *driver)
{
    char path[160];
    char resolved[256];
    ssize_t rd;

    if(!name || !name[0] || strcmp(name, "lo") == 0 ||
       strcmp(name, NET_WIFI_IFACE) == 0 ||
       strcmp(name, NET_ETH_IFACE) == 0) {
        return 0;
    }
    if(usb_modem_driver_is_candidate(driver)) {
        return 1;
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/device", name);
    rd = readlink(path, resolved, sizeof(resolved) - 1U);
    if(rd < 0) {
        return 0;
    }
    resolved[rd] = '\0';
    return usb_modem_text_contains(resolved, "usb");
}

static int usb_modem_tool_exists(const char *tool)
{
    char cmd[96];
    char out[96];

    snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", tool);
    return ui_read_cmd_first_line(cmd, out, sizeof(out)) == 0 && out[0];
}

static void usb_modem_scan_ifaces(void)
{
    DIR *dir;
    struct dirent *ent;

    usb_modem_iface_count = 0;
    dir = opendir("/sys/class/net");
    if(!dir) {
        return;
    }
    while((ent = readdir(dir)) != NULL &&
          usb_modem_iface_count < USB_MODEM_MAX_IFACES) {
        char driver_path[160];
        char state_path[160];
        usb_modem_iface_t item;

        if(ent->d_name[0] == '.') {
            continue;
        }
        memset(&item, 0, sizeof(item));
        snprintf(item.name, sizeof(item.name), "%s", ent->d_name);
        snprintf(driver_path, sizeof(driver_path),
                 "/sys/class/net/%s/device/driver", item.name);
        usb_modem_basename_from_link(driver_path, item.driver,
                                     sizeof(item.driver));
        if(!usb_modem_iface_is_candidate(item.name, item.driver)) {
            continue;
        }
        snprintf(state_path, sizeof(state_path), "/sys/class/net/%s/operstate",
                 item.name);
        if(usb_modem_read_text(state_path, item.state,
                               sizeof(item.state)) != 0) {
            snprintf(item.state, sizeof(item.state), "--");
        }
        if(ui_read_iface_ip(item.name, item.ip, sizeof(item.ip)) != 0) {
            snprintf(item.ip, sizeof(item.ip), "no ip");
        }
        usb_modem_ifaces[usb_modem_iface_count++] = item;
    }
    closedir(dir);
}

static void usb_modem_format_usb_devices(char *buf, size_t len)
{
    DIR *dir;
    struct dirent *ent;
    size_t used = 0;
    int count = 0;

    if(len > 0) {
        buf[0] = '\0';
    }
    dir = opendir("/sys/bus/usb/devices");
    if(!dir) {
        snprintf(buf, len, "USB sysfs unavailable");
        return;
    }

    while((ent = readdir(dir)) != NULL && count < USB_MODEM_MAX_USB) {
        char base[160];
        char path[192];
        char vid[16] = "";
        char pid[16] = "";
        char manufacturer[80] = "";
        char product[96] = "";
        char driver[48] = "";
        char klass[16] = "";
        int modem_like = 0;
        int add;

        if(ent->d_name[0] == '.') {
            continue;
        }
        snprintf(base, sizeof(base), "/sys/bus/usb/devices/%s", ent->d_name);
        snprintf(path, sizeof(path), "%s/idVendor", base);
        if(usb_modem_read_text(path, vid, sizeof(vid)) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/idProduct", base);
        usb_modem_read_text(path, pid, sizeof(pid));
        snprintf(path, sizeof(path), "%s/manufacturer", base);
        usb_modem_read_text(path, manufacturer, sizeof(manufacturer));
        snprintf(path, sizeof(path), "%s/product", base);
        usb_modem_read_text(path, product, sizeof(product));
        snprintf(path, sizeof(path), "%s/bDeviceClass", base);
        usb_modem_read_text(path, klass, sizeof(klass));
        snprintf(path, sizeof(path), "%s/driver", base);
        usb_modem_basename_from_link(path, driver, sizeof(driver));

        modem_like = usb_modem_text_contains(manufacturer, "qualcomm") ||
                     usb_modem_text_contains(manufacturer, "asr") ||
                     usb_modem_text_contains(product, "modem") ||
                     usb_modem_text_contains(product, "lte") ||
                     usb_modem_text_contains(product, "5g") ||
                     usb_modem_text_contains(product, "wwan") ||
                     strcmp(klass, "02") == 0 ||
                     strcmp(klass, "0a") == 0 ||
                     strcmp(klass, "e0") == 0 ||
                     strcmp(klass, "ff") == 0;

        if(!modem_like && usb_modem_iface_count == 0) {
            continue;
        }
        add = snprintf(buf + used, len - used, "%s%s:%s %s %s drv=%s\n",
                       used ? "" : "", vid, pid,
                       manufacturer[0] ? manufacturer : "USB",
                       product[0] ? product : "device",
                       driver[0] ? driver : "--");
        if(add < 0 || (size_t)add >= len - used) {
            break;
        }
        used += (size_t)add;
        count++;
    }
    closedir(dir);
    if(count == 0) {
        snprintf(buf, len, "No USB modem-like device");
    }
}

static void usb_modem_format_ports(char *buf, size_t len)
{
    static const char *prefixes[] = { "ttyUSB", "ttyACM", "cdc-wdm", "wwan" };
    DIR *dir;
    struct dirent *ent;
    size_t used = 0;
    int count = 0;

    if(len > 0) {
        buf[0] = '\0';
    }
    dir = opendir("/dev");
    if(!dir) {
        snprintf(buf, len, "/dev unavailable");
        return;
    }
    while((ent = readdir(dir)) != NULL && count < USB_MODEM_MAX_PORTS) {
        int match = 0;
        for(size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            if(strncmp(ent->d_name, prefixes[i], strlen(prefixes[i])) == 0) {
                match = 1;
                break;
            }
        }
        if(!match) {
            continue;
        }
        int add = snprintf(buf + used, len - used, "%s%s", used ? "  " : "",
                           ent->d_name);
        if(add < 0 || (size_t)add >= len - used) {
            break;
        }
        used += (size_t)add;
        count++;
    }
    closedir(dir);
    if(count == 0) {
        snprintf(buf, len, "No AT/QMI/MBIM ports");
    }
}

static void usb_modem_format_ifaces(char *buf, size_t len)
{
    size_t used = 0;

    if(len > 0) {
        buf[0] = '\0';
    }
    if(usb_modem_iface_count == 0) {
        snprintf(buf, len, "No modem net interface");
        return;
    }
    for(int i = 0; i < usb_modem_iface_count; i++) {
        int add = snprintf(buf + used, len - used, "%s %s %s %s\n",
                           usb_modem_ifaces[i].name,
                           usb_modem_ifaces[i].driver[0] ?
                           usb_modem_ifaces[i].driver : "usb",
                           usb_modem_ifaces[i].state,
                           usb_modem_ifaces[i].ip);
        if(add < 0 || (size_t)add >= len - used) {
            break;
        }
        used += (size_t)add;
    }
}

static void usb_modem_update_ui(void)
{
    char status[160];
    char text[640];

    usb_modem_scan_ifaces();

    pthread_mutex_lock(&usb_modem_lock);
    snprintf(status, sizeof(status), "%s", usb_modem_status);
    pthread_mutex_unlock(&usb_modem_lock);

    if(usb_modem_status_label) {
        lv_label_set_text(usb_modem_status_label, status);
    }
    if(usb_modem_usb_label) {
        usb_modem_format_usb_devices(text, sizeof(text));
        lv_label_set_text(usb_modem_usb_label, text);
    }
    if(usb_modem_iface_label) {
        usb_modem_format_ifaces(text, sizeof(text));
        lv_label_set_text(usb_modem_iface_label, text);
    }
    if(usb_modem_ports_label) {
        usb_modem_format_ports(text, sizeof(text));
        lv_label_set_text(usb_modem_ports_label, text);
    }
    if(usb_modem_tools_label) {
        snprintf(text, sizeof(text), "udhcpc:%s  pppd:%s  uqmi:%s  mbimcli:%s",
                 usb_modem_tool_exists("udhcpc") ? "ok" : "missing",
                 usb_modem_tool_exists("pppd") ? "ok" : "missing",
                 usb_modem_tool_exists("uqmi") ? "ok" : "missing",
                 usb_modem_tool_exists("mbimcli") ? "ok" : "missing");
        lv_label_set_text(usb_modem_tools_label, text);
    }
    usb_modem_log_refresh();
}

static int usb_modem_first_iface(char *iface, size_t len)
{
    usb_modem_scan_ifaces();
    if(usb_modem_iface_count <= 0 || !iface || len == 0) {
        return -1;
    }
    snprintf(iface, len, "%s", usb_modem_ifaces[0].name);
    return 0;
}

static int usb_modem_safe_iface(const char *iface)
{
    if(!iface || !iface[0]) {
        return 0;
    }
    for(const char *p = iface; *p; p++) {
        if(!isalnum((unsigned char)*p) && *p != '_' && *p != '-' &&
           *p != '.') {
            return 0;
        }
    }
    return 1;
}

static void *usb_modem_dial_thread(void *arg)
{
    char iface[32] = "";
    char cmd[512];
    int rc;

    (void)arg;
    if(usb_modem_first_iface(iface, sizeof(iface)) != 0 ||
       !usb_modem_safe_iface(iface)) {
        usb_modem_set_status("No DHCP-capable modem interface");
        usb_modem_log_append("Dial skipped: no RNDIS/ECM/NCM/QMI net interface");
        usb_modem_log_append("If only /dev/ttyUSB* exists, PPP APN setup is next");
        goto out;
    }

    if(!usb_modem_tool_exists("udhcpc")) {
        usb_modem_set_status("udhcpc missing");
        usb_modem_log_append("Dial failed: udhcpc missing");
        goto out;
    }

    usb_modem_set_status("Dialing %s by DHCP", iface);
    usb_modem_log_append("Dial start iface=%s", iface);
    snprintf(cmd, sizeof(cmd),
             "ip link set %s up >" USB_MODEM_DIAL_LOG " 2>&1; "
             "udhcpc -i %s -p " USB_MODEM_UDHCPC_PID
             " -q -n -t 5 -T 3 >>" USB_MODEM_DIAL_LOG " 2>&1",
             iface, iface);
    rc = system(cmd);
    if(ui_shell_exit_code(rc) == 0) {
        char ip[48] = "";
        ui_read_iface_ip(iface, ip, sizeof(ip));
        usb_modem_set_status("%s connected %s", iface, ip[0] ? ip : "no ip");
        usb_modem_log_append("Dial OK iface=%s ip=%s", iface,
                             ip[0] ? ip : "no ip");
    } else {
        usb_modem_set_status("Dial failed on %s", iface);
        usb_modem_log_append("Dial failed iface=%s rc=%d log=%s", iface,
                             ui_shell_exit_code(rc), USB_MODEM_DIAL_LOG);
    }

out:
    pthread_mutex_lock(&usb_modem_lock);
    usb_modem_worker_active = 0;
    pthread_mutex_unlock(&usb_modem_lock);
    return NULL;
}

static void usb_modem_scan_event_cb(lv_event_t *event)
{
    (void)event;
    usb_modem_set_status("Scan refreshed");
    usb_modem_log_append("Scan requested");
    usb_modem_update_ui();
    app_request_fast_refresh();
}

static void usb_modem_dial_event_cb(lv_event_t *event)
{
    int busy;

    (void)event;
    pthread_mutex_lock(&usb_modem_lock);
    busy = usb_modem_worker_active;
    if(!busy) {
        usb_modem_worker_active = 1;
    }
    pthread_mutex_unlock(&usb_modem_lock);

    if(busy) {
        usb_modem_log_append("Dial ignored: worker busy");
        return;
    }
    if(pthread_create(&usb_modem_worker, NULL, usb_modem_dial_thread,
                      NULL) != 0) {
        pthread_mutex_lock(&usb_modem_lock);
        usb_modem_worker_active = 0;
        pthread_mutex_unlock(&usb_modem_lock);
        usb_modem_set_status("Dial thread failed");
        usb_modem_log_append("pthread_create failed");
    } else {
        pthread_detach(usb_modem_worker);
    }
    app_request_fast_refresh();
}

static void usb_modem_disconnect_event_cb(lv_event_t *event)
{
    char iface[32] = "";
    char cmd[256];
    int rc;

    (void)event;
    usb_modem_first_iface(iface, sizeof(iface));
    if(!usb_modem_safe_iface(iface)) {
        iface[0] = '\0';
    }
    snprintf(cmd, sizeof(cmd),
             "if [ -s " USB_MODEM_UDHCPC_PID " ]; then "
             "kill $(cat " USB_MODEM_UDHCPC_PID ") >/dev/null 2>&1 || true; "
             "rm " USB_MODEM_UDHCPC_PID " >/dev/null 2>&1 || true; fi; "
             "%s%s%s",
             iface[0] ? "ip link set " : ":",
             iface[0] ? iface : "",
             iface[0] ? " down >/dev/null 2>&1" : "");
    rc = system(cmd);
    (void)rc;
    usb_modem_set_status("Disconnected");
    usb_modem_log_append("Disconnect requested iface=%s",
                         iface[0] ? iface : "--");
    usb_modem_update_ui();
    app_request_fast_refresh();
}

static void usb_modem_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    usb_modem_update_ui();
}

void ui_usb_modem_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *panel;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(144);
    int body_w = ui_screen_width();
    int summary_w = landscape ? 350 : 520;
    int right_x = landscape ? summary_w + 48 : 24;
    int right_w = landscape ? body_w - right_x - 24 : 520;
    int log_h = landscape ? body_h - 40 : 150;

    if(right_w < 300) {
        right_w = 300;
    }
    if(log_h < 260) {
        log_h = 260;
    }

    ui_create_header(scr, "USB Modem");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    summary = ui_panel(body, 24, 20, summary_w, landscape ? 224 : 250);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x151B22), 0);

    ui_label(summary, "ASR / Qualcomm USB modem", &lv_font_montserrat_22,
             0xF2F5F8);
    lv_obj_align(lv_obj_get_child(summary,
                                  lv_obj_get_child_count(summary) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    usb_modem_status_label = ui_label(summary, "Ready",
                                      &lv_font_montserrat_18, 0x25C281);
    lv_obj_set_width(usb_modem_status_label, summary_w - 32);
    lv_label_set_long_mode(usb_modem_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(usb_modem_status_label, LV_ALIGN_TOP_LEFT, 0, 44);

    btn = ui_command_button(summary, 0, 102, landscape ? 98 : 154, "Scan",
                            0x60A5FA);
    lv_obj_add_event_cb(btn, usb_modem_scan_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(summary, landscape ? 106 : 166, 102,
                            landscape ? 98 : 154, "Dial", 0x25C281);
    lv_obj_add_event_cb(btn, usb_modem_dial_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(summary, landscape ? 212 : 332, 102,
                            landscape ? 106 : 154, "Disconnect", 0xEF4D5A);
    lv_obj_add_event_cb(btn, usb_modem_disconnect_event_cb, LV_EVENT_CLICKED,
                        NULL);

    ui_info_row(summary, 182, "Mode", "RNDIS/ECM/NCM DHCP", 0xF2F5F8);

    panel = ui_panel(body, 24, landscape ? 268 : 294, summary_w,
                     landscape ? 142 : 232);
    ui_label(panel, "USB device", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    usb_modem_usb_label = ui_label(panel, "--", &lv_font_montserrat_16,
                                   0x9AA4AF);
    lv_obj_set_width(usb_modem_usb_label, summary_w - 32);
    lv_label_set_long_mode(usb_modem_usb_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(usb_modem_usb_label, LV_ALIGN_TOP_LEFT, 0, 40);

    panel = ui_panel(body, 24, landscape ? 430 : 550, summary_w,
                     landscape ? 120 : 204);
    ui_label(panel, "Network interface", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    usb_modem_iface_label = ui_label(panel, "--", &lv_font_montserrat_16,
                                     0x9AA4AF);
    lv_obj_set_width(usb_modem_iface_label, summary_w - 32);
    lv_label_set_long_mode(usb_modem_iface_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(usb_modem_iface_label, LV_ALIGN_TOP_LEFT, 0, 40);

    panel = ui_panel(body, 24, landscape ? 570 : 778, summary_w,
                     landscape ? 96 : 130);
    ui_label(panel, "Control ports", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    usb_modem_ports_label = ui_label(panel, "--", &lv_font_montserrat_16,
                                     0x9AA4AF);
    lv_obj_set_width(usb_modem_ports_label, summary_w - 32);
    lv_label_set_long_mode(usb_modem_ports_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(usb_modem_ports_label, LV_ALIGN_TOP_LEFT, 0, 40);

    panel = ui_panel(body, 24, landscape ? 686 : 932, summary_w,
                     landscape ? 96 : 102);
    ui_label(panel, "Tools", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    usb_modem_tools_label = ui_label(panel, "--", &lv_font_montserrat_16,
                                     0x9AA4AF);
    lv_obj_set_width(usb_modem_tools_label, summary_w - 32);
    lv_label_set_long_mode(usb_modem_tools_label, LV_LABEL_LONG_DOT);
    lv_obj_align(usb_modem_tools_label, LV_ALIGN_TOP_LEFT, 0, 40);

    panel = ui_panel(body, right_x, landscape ? 20 : 1058, right_w, log_h);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    ui_label(panel, "Log", &lv_font_montserrat_20, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    usb_modem_log_label = ui_label(panel, "No log yet",
                                   &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(usb_modem_log_label, right_w - 32);
    lv_label_set_long_mode(usb_modem_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(usb_modem_log_label, LV_ALIGN_TOP_LEFT, 0, 34);

    usb_modem_update_ui();
    usb_modem_timer = lv_timer_create(usb_modem_timer_cb, 2000, NULL);
}

void ui_usb_modem_cleanup(void)
{
    if(usb_modem_timer) {
        lv_timer_delete(usb_modem_timer);
        usb_modem_timer = NULL;
    }
    usb_modem_status_label = NULL;
    usb_modem_usb_label = NULL;
    usb_modem_iface_label = NULL;
    usb_modem_ports_label = NULL;
    usb_modem_tools_label = NULL;
    usb_modem_log_label = NULL;
}
