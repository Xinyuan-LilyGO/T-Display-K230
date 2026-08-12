#include "ui_wifi.h"

#include "ui_i18n.h"
#include "ui_input.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIFI_AUTO_SCAN_PERIOD_US 5000000ULL
#define WIFI_CONF_PATH "/tmp/k230_wifi_ui.conf"
#define WIFI_SCAN_LOG "/tmp/k230_wifi_scan.log"
#define WIFI_SCAN_ERR_LOG "/tmp/k230_wifi_scan.err"
#define WIFI_CONNECT_LOG "/tmp/k230_wifi_connect.log"
#define WIFI_PROFILE_PATH "/root/app/k230_phone_ui/wifi_profiles.conf"
#define WIFI_PROFILE_TMP_PATH "/root/app/k230_phone_ui/wifi_profiles.conf.tmp"
#define WIFI_MAX_PROFILES 12
#define WIFI_SCAN_WAIT_STEP_US 100000U
#define WIFI_SCAN_WAIT_MAX_STEPS 100U
#define WIFI_MODULE_LOG "/tmp/k230_wifi_modprobe.log"

typedef struct {
    char ssid[NET_SSID_MAX];
    char security[32];
    char quality[24];
    char signal[32];
    int signal_dbm;
    int signal_valid;
    int encrypted;
} wifi_ap_info_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char password[NET_PASS_MAX];
    int encrypted;
    int auto_connect;
} wifi_connect_request_t;

typedef struct {
    char ssid[NET_SSID_MAX];
    char password[NET_PASS_MAX];
    int encrypted;
} wifi_profile_t;

static pthread_mutex_t wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static wifi_ap_info_t wifi_aps[NET_MAX_APS];
static wifi_profile_t wifi_profiles[WIFI_MAX_PROFILES];
static int wifi_ap_count;
static int wifi_profile_count;
static int wifi_profiles_loaded;
static int wifi_selected_ap = -1;
static int wifi_enabled = 1;
static int wifi_scan_busy;
static int wifi_connect_busy;
static int wifi_selected_encrypted;
static int wifi_autoconnect_started;
static int wifi_suppress_click_index = -1;
static uint64_t wifi_suppress_click_until_us;
static char wifi_status_text[NET_STATUS_MAX] = "Wi-Fi on";
static char wifi_selected_ssid[NET_SSID_MAX];
static char wifi_dialog_ssid[NET_SSID_MAX];
static int wifi_dialog_encrypted;
static uint64_t wifi_last_scan_us;

static lv_obj_t *wifi_switch_obj;
static lv_obj_t *wifi_state_label;
static lv_obj_t *wifi_ip_label;
static lv_obj_t *wifi_status_label;
static lv_obj_t *wifi_connect_spinner;
static lv_obj_t *wifi_ap_btn[NET_MAX_APS];
static lv_obj_t *wifi_ap_type_icon[NET_MAX_APS];
static lv_obj_t *wifi_ap_signal_icon[NET_MAX_APS];
static lv_obj_t *wifi_ap_rssi_label[NET_MAX_APS];
static lv_obj_t *wifi_ap_title[NET_MAX_APS];
static lv_obj_t *wifi_ap_meta[NET_MAX_APS];
static lv_timer_t *wifi_timer;

static void wifi_password_submit_cb(const char *password, void *user_data);

#if 0
static const lv_point_precise_t wifi_type_line_outer[] = {
    {3, 14}, {17, 4}, {31, 14},
};
static const lv_point_precise_t wifi_type_line_middle[] = {
    {8, 20}, {17, 13}, {26, 20},
};
static const lv_point_precise_t wifi_type_line_inner[] = {
    {13, 25}, {17, 22}, {21, 25},
};
#endif

static void wifi_set_status_locked(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(wifi_status_text, sizeof(wifi_status_text), fmt, ap);
    va_end(ap);
}

static void wifi_log(const char *fmt, ...)
{
    FILE *fp = fopen(WIFI_CONNECT_LOG, "a");
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

static void wifi_load_modules(void)
{
    static int attempted;
    const char *modules_env = getenv("K230_WIFI_MODULES");
    const char *modules = modules_env && modules_env[0] ? modules_env : "8189fs 8723ds";
    char cmd[256];
    int rc;

    if(attempted) {
        return;
    }
    attempted = 1;

    snprintf(cmd, sizeof(cmd),
             "for m in %s; do modprobe \"$m\" >>" WIFI_MODULE_LOG " 2>&1 || true; done",
             modules);
    wifi_log("Loading Wi-Fi modules: %s", modules);
    rc = system("mkdir -p /tmp; : >" WIFI_MODULE_LOG);
    (void)rc;
    rc = system(cmd);
    (void)rc;
}

static void wifi_copy_field(char *dst, size_t len, const char *src)
{
    if(!dst || len == 0) {
        return;
    }
    if(!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, len, "%s", src);
    ui_trim_text(dst);
}

static int wifi_parse_signal_dbm(const char *text, int *dbm)
{
    const char *p;
    char *end = NULL;
    long value;

    if(!text || !dbm) {
        return 0;
    }
    p = text;
    while(*p && *p != '-' && !isdigit((unsigned char)*p)) {
        p++;
    }
    if(!*p) {
        return 0;
    }
    value = strtol(p, &end, 10);
    if(end == p || value < -130 || value > 20) {
        return 0;
    }
    *dbm = (int)value;
    return 1;
}

static void wifi_scan_log_results(const wifi_ap_info_t *list, int count,
                                  int rc)
{
    FILE *fp = fopen(WIFI_SCAN_LOG, "w");

    if(!fp) {
        return;
    }
    fprintf(fp, "scan rc=%d count=%d\n", rc, count);
    for(int i = 0; list && i < count && i < NET_MAX_APS; i++) {
        fprintf(fp,
                "%02d ssid=\"%s\" signal_valid=%d signal_dbm=%d raw_signal=\"%s\" quality=\"%s\" security=\"%s\"\n",
                i, list[i].ssid, list[i].signal_valid, list[i].signal_dbm,
                list[i].signal, list[i].quality, list[i].security);
    }
    fclose(fp);
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
static lv_obj_t *wifi_type_line_create(lv_obj_t *parent,
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

static void wifi_type_icon_update(lv_obj_t *icon, uint32_t color)
{
    if(!icon) {
        return;
    }
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
}

static lv_obj_t *wifi_type_icon_create(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_label_create(parent);

    lv_obj_set_size(icon, 34, 34);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_click_forwarder(icon);
    return icon;
}

static int wifi_hex_value(char ch)
{
    if(ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if(ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if(ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static void wifi_escape_write(FILE *fp, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while(fp && p && *p) {
        switch(*p) {
        case '\\':
            fputs("\\\\", fp);
            break;
        case '\t':
            fputs("\\t", fp);
            break;
        case '\n':
            fputs("\\n", fp);
            break;
        case '\r':
            fputs("\\r", fp);
            break;
        default:
            if(*p >= 32 && *p != 127) {
                fputc(*p, fp);
            } else {
                fprintf(fp, "\\x%02X", *p);
            }
            break;
        }
        p++;
    }
}

static void wifi_unescape_copy(char *dst, size_t len, const char *src)
{
    size_t out = 0;

    if(!dst || len == 0) {
        return;
    }
    dst[0] = '\0';
    if(!src) {
        return;
    }

    while(*src && out + 1U < len) {
        if(*src == '\\' && src[1]) {
            src++;
            switch(*src) {
            case 'x':
            case 'X':
                if(isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
                    int hi = wifi_hex_value(src[1]);
                    int lo = wifi_hex_value(src[2]);
                    if(hi >= 0 && lo >= 0) {
                        dst[out++] = (char)((hi << 4) | lo);
                        src += 2;
                    }
                } else {
                    dst[out++] = *src;
                }
                break;
            case 't':
                dst[out++] = '\t';
                break;
            case 'n':
                dst[out++] = '\n';
                break;
            case 'r':
                dst[out++] = '\r';
                break;
            default:
                dst[out++] = *src;
                break;
            }
        } else {
            dst[out++] = *src;
        }
        src++;
    }
    dst[out] = '\0';
}

static int wifi_profile_find_locked(const char *ssid)
{
    if(!ssid || !ssid[0]) {
        return -1;
    }

    for(int i = 0; i < wifi_profile_count; i++) {
        if(strcmp(wifi_profiles[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void wifi_profiles_load_locked(void)
{
    FILE *fp;
    char line[256];

    if(wifi_profiles_loaded) {
        return;
    }

    wifi_profiles_loaded = 1;
    wifi_profile_count = 0;
    fp = fopen(WIFI_PROFILE_PATH, "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp) &&
          wifi_profile_count < WIFI_MAX_PROFILES) {
        char *p1;
        char *p2;
        char *p3;
        wifi_profile_t profile;

        line[strcspn(line, "\r\n")] = '\0';
        p1 = strchr(line, '\t');
        if(!p1 || strncmp(line, "v1", 2) != 0) {
            continue;
        }
        *p1++ = '\0';
        p2 = strchr(p1, '\t');
        if(!p2) {
            continue;
        }
        *p2++ = '\0';
        p3 = strchr(p2, '\t');
        if(!p3) {
            continue;
        }
        *p3++ = '\0';

        memset(&profile, 0, sizeof(profile));
        profile.encrypted = atoi(p1) ? 1 : 0;
        wifi_unescape_copy(profile.ssid, sizeof(profile.ssid), p2);
        wifi_unescape_copy(profile.password, sizeof(profile.password), p3);
        if(profile.ssid[0] && wifi_profile_find_locked(profile.ssid) < 0) {
            wifi_profiles[wifi_profile_count++] = profile;
        }
    }
    fclose(fp);
}

static int wifi_profiles_save_locked(void)
{
    FILE *fp = fopen(WIFI_PROFILE_TMP_PATH, "w");

    if(!fp) {
        return -1;
    }

    for(int i = 0; i < wifi_profile_count; i++) {
        fputs("v1\t", fp);
        fprintf(fp, "%d\t", wifi_profiles[i].encrypted ? 1 : 0);
        wifi_escape_write(fp, wifi_profiles[i].ssid);
        fputc('\t', fp);
        wifi_escape_write(fp, wifi_profiles[i].password);
        fputc('\n', fp);
    }
    fclose(fp);
    chmod(WIFI_PROFILE_TMP_PATH, 0600);
    if(rename(WIFI_PROFILE_TMP_PATH, WIFI_PROFILE_PATH) != 0) {
        unlink(WIFI_PROFILE_TMP_PATH);
        return -1;
    }
    chmod(WIFI_PROFILE_PATH, 0600);
    return 0;
}

static void wifi_profile_remember(const char *ssid, const char *password,
                                  int encrypted)
{
    wifi_profile_t profile;
    int index;
    int limit;

    if(!ssid || !ssid[0]) {
        return;
    }

    memset(&profile, 0, sizeof(profile));
    snprintf(profile.ssid, sizeof(profile.ssid), "%s", ssid);
    snprintf(profile.password, sizeof(profile.password), "%s", password ? password : "");
    profile.encrypted = encrypted ? 1 : 0;

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    index = wifi_profile_find_locked(profile.ssid);
    if(index < 0) {
        index = wifi_profile_count;
        if(wifi_profile_count < WIFI_MAX_PROFILES) {
            wifi_profile_count++;
        }
    }
    limit = index;
    if(limit >= WIFI_MAX_PROFILES) {
        limit = WIFI_MAX_PROFILES - 1;
    }
    for(int i = limit; i > 0; i--) {
        wifi_profiles[i] = wifi_profiles[i - 1];
    }
    wifi_profiles[0] = profile;
    if(wifi_profiles_save_locked() == 0) {
        wifi_log("Saved Wi-Fi profile ssid=%s encrypted=%d password_len=%u",
                 profile.ssid, profile.encrypted,
                 (unsigned)strlen(profile.password));
    } else {
        wifi_log("Failed to save Wi-Fi profile ssid=%s", profile.ssid);
    }
    pthread_mutex_unlock(&wifi_lock);
}

static int wifi_parse_quoted_value(const char *line, char *dst, size_t len)
{
    const char *p;
    size_t out = 0;

    if(!line || !dst || len == 0) {
        return -1;
    }
    dst[0] = '\0';
    p = strchr(line, '"');
    if(!p) {
        return -1;
    }
    p++;
    while(*p && *p != '"' && out + 1U < len) {
        if(*p == '\\' && p[1]) {
            p++;
            if((*p == 'x' || *p == 'X') &&
               isxdigit((unsigned char)p[1]) &&
               isxdigit((unsigned char)p[2])) {
                int hi = wifi_hex_value(p[1]);
                int lo = wifi_hex_value(p[2]);
                if(hi >= 0 && lo >= 0) {
                    dst[out++] = (char)((hi << 4) | lo);
                    p += 3;
                    continue;
                }
            }
        }
        dst[out++] = *p++;
    }
    dst[out] = '\0';
    return dst[0] ? 0 : -1;
}

static int wifi_read_runtime_config(char *ssid, size_t ssid_len,
                                    char *password, size_t password_len,
                                    int *encrypted)
{
    FILE *fp;
    char line[256];
    char local_ssid[NET_SSID_MAX] = "";
    char local_password[NET_PASS_MAX] = "";
    int local_encrypted = 0;

    fp = fopen(WIFI_CONF_PATH, "r");
    if(!fp) {
        return -1;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(strncmp(line, "ssid=", 5) == 0 ||
           strncmp(line, "ssid =", 6) == 0) {
            (void)wifi_parse_quoted_value(line, local_ssid,
                                          sizeof(local_ssid));
        } else if(strncmp(line, "psk=", 4) == 0 ||
                  strncmp(line, "psk =", 5) == 0) {
            if(wifi_parse_quoted_value(line, local_password,
                                       sizeof(local_password)) == 0) {
                local_encrypted = 1;
            }
        } else if(strstr(line, "key_mgmt=NONE") ||
                  strstr(line, "key_mgmt = NONE")) {
            local_encrypted = 0;
        }
    }
    fclose(fp);

    if(!local_ssid[0]) {
        return -1;
    }
    if(ssid && ssid_len > 0) {
        snprintf(ssid, ssid_len, "%s", local_ssid);
    }
    if(password && password_len > 0) {
        snprintf(password, password_len, "%s", local_password);
    }
    if(encrypted) {
        *encrypted = local_encrypted;
    }
    return 0;
}

static int wifi_wpa_completed(void)
{
    char cmd[160];
    char state[32];

    snprintf(cmd, sizeof(cmd),
             "wpa_cli -i %s status 2>/dev/null | sed -n 's/^wpa_state=//p'",
             NET_WIFI_IFACE);
    return ui_read_cmd_first_line(cmd, state, sizeof(state)) == 0 &&
           strcmp(state, "COMPLETED") == 0;
}

static void wifi_scan_store_ap(wifi_ap_info_t *list, int *count,
                               const wifi_ap_info_t *ap)
{
    if(!list || !count || !ap || *count >= NET_MAX_APS || !ap->ssid[0]) {
        return;
    }

    list[*count] = *ap;
    if(!list[*count].security[0]) {
        snprintf(list[*count].security, sizeof(list[*count].security),
                 ap->encrypted ? "Secured" : "Open");
    }
    (*count)++;
}

static int wifi_scan_find_ap(const wifi_ap_info_t *list, int count,
                             const char *ssid)
{
    if(!list || !ssid || !ssid[0]) {
        return -1;
    }

    for(int i = 0; i < count && i < NET_MAX_APS; i++) {
        if(strcmp(list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static int wifi_scan_collect(wifi_ap_info_t *list, int *count, char *err,
                             size_t err_len)
{
    FILE *fp;
    char line[256];
    wifi_ap_info_t cur;
    int have_cell = 0;
    int rc;

    if(!list || !count) {
        return -1;
    }

    *count = 0;
    memset(&cur, 0, sizeof(cur));

    wifi_load_modules();
    if(!ui_path_exists("/sys/class/net/" NET_WIFI_IFACE)) {
        if(err && err_len > 0) {
            snprintf(err, err_len, NET_WIFI_IFACE " missing");
        }
        return -1;
    }

    fp = popen("ifconfig " NET_WIFI_IFACE " up >/dev/null 2>&1; "
               "iwlist " NET_WIFI_IFACE " scan 2>" WIFI_SCAN_ERR_LOG, "r");
    if(!fp) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "scan command failed");
        }
        return -1;
    }

    while(fgets(line, sizeof(line), fp)) {
        char *p;

        if(strstr(line, "Cell ") != NULL && strstr(line, "Address:") != NULL) {
            if(have_cell) {
                wifi_scan_store_ap(list, count, &cur);
            }
            memset(&cur, 0, sizeof(cur));
            snprintf(cur.security, sizeof(cur.security), "Open");
            have_cell = 1;
            continue;
        }

        p = strstr(line, "ESSID:");
        if(p) {
            p = strchr(p, '"');
            if(p) {
                char *end;

                p++;
                end = strchr(p, '"');
                if(end) {
                    *end = '\0';
                }
                wifi_unescape_copy(cur.ssid, sizeof(cur.ssid), p);
                ui_trim_text(cur.ssid);
            }
            continue;
        }

        p = strstr(line, "Quality=");
        if(p) {
            char *end;
            char saved;

            p += strlen("Quality=");
            end = p;
            while(*end && !isspace((unsigned char)*end)) {
                end++;
            }
            saved = *end;
            *end = '\0';
            wifi_copy_field(cur.quality, sizeof(cur.quality), p);
            *end = saved;
        }

        p = strstr(line, "Signal level");
        if(p) {
            char *end;

            p += strlen("Signal level");
            while(*p == '=' || *p == ':' || isspace((unsigned char)*p)) {
                p++;
            }
            end = p;
            while(*end && *end != '\r' && *end != '\n') {
                end++;
            }
            *end = '\0';
            wifi_copy_field(cur.signal, sizeof(cur.signal), p);
            cur.signal_valid = wifi_parse_signal_dbm(cur.signal,
                                                     &cur.signal_dbm);
        }

        p = strstr(line, "Encryption key:");
        if(p) {
            cur.encrypted = strstr(p, "on") != NULL;
            snprintf(cur.security, sizeof(cur.security), "%s",
                     cur.encrypted ? "Secured" : "Open");
        }

        if(strstr(line, "WPA3")) {
            snprintf(cur.security, sizeof(cur.security), "WPA3");
            cur.encrypted = 1;
        } else if(strstr(line, "WPA2")) {
            snprintf(cur.security, sizeof(cur.security), "WPA2");
            cur.encrypted = 1;
        } else if(strstr(line, "WPA")) {
            snprintf(cur.security, sizeof(cur.security), "WPA");
            cur.encrypted = 1;
        }
    }

    if(have_cell) {
        wifi_scan_store_ap(list, count, &cur);
    }

    rc = ui_shell_exit_code(pclose(fp));
    wifi_scan_log_results(list, *count, rc);
    if(rc != 0 && *count == 0) {
        if(err && err_len > 0) {
            snprintf(err, err_len, "scan failed, rc=%d", rc);
        }
        return -1;
    }

    return 0;
}

static void wifi_write_quoted(FILE *fp, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    fputc('"', fp);
    while(p && *p) {
        if(*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc(*p, fp);
        } else if(*p >= 32 && *p != 127) {
            fputc(*p, fp);
        } else {
            fprintf(fp, "\\x%02X", *p);
        }
        p++;
    }
    fputc('"', fp);
}

static int wifi_write_config(const char *ssid, const char *password)
{
    FILE *fp = fopen(WIFI_CONF_PATH, "w");

    if(!fp) {
        return -1;
    }

    fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(fp, "ap_scan=1\n\n");
    fprintf(fp, "network={\n");
    fprintf(fp, "    ssid=");
    wifi_write_quoted(fp, ssid);
    fprintf(fp, "\n");
    fprintf(fp, "    scan_ssid=1\n");
    if(password && password[0]) {
        fprintf(fp, "    psk=");
        wifi_write_quoted(fp, password);
        fprintf(fp, "\n");
    } else {
        fprintf(fp, "    key_mgmt=NONE\n");
    }
    fprintf(fp, "}\n");
    fclose(fp);
    chmod(WIFI_CONF_PATH, 0600);
    return 0;
}

static int wifi_write_connect_script(void)
{
    FILE *fp = fopen("/tmp/k230_wifi_connect.sh", "w");

    if(!fp) {
        return -1;
    }

    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "LOG='%s'\n", WIFI_CONNECT_LOG);
    fprintf(fp, "IF='%s'\n", NET_WIFI_IFACE);
    fprintf(fp, "CONF='%s'\n", WIFI_CONF_PATH);
    fprintf(fp, "echo '[shell] connect start' >>$LOG\n");
    fprintf(fp, "mkdir -p /var/run/wpa_supplicant\n");
    fprintf(fp, "wpa_cli -i $IF terminate >>$LOG 2>&1 || true\n");
    fprintf(fp, "killall -q wpa_supplicant || true\n");
    fprintf(fp, "if [ -f /var/run/udhcpc.$IF.pid ]; then kill $(cat /var/run/udhcpc.$IF.pid) >>$LOG 2>&1 || true; rm -f /var/run/udhcpc.$IF.pid; fi\n");
    fprintf(fp, "rm -f /var/run/wpa_supplicant/$IF\n");
    fprintf(fp, "ifconfig $IF up >>$LOG 2>&1 || exit 10\n");
    fprintf(fp, "wpa_supplicant -B -i $IF -D nl80211,wext -c $CONF >>$LOG 2>&1 || exit 11\n");
    fprintf(fp, "i=0\n");
    fprintf(fp, "state=''\n");
    fprintf(fp, "while [ $i -lt 20 ]; do\n");
    fprintf(fp, "  state=$(wpa_cli -i $IF status 2>>$LOG | sed -n 's/^wpa_state=//p')\n");
    fprintf(fp, "  echo \"[shell] wait_wpa[$i]=$state\" >>$LOG\n");
    fprintf(fp, "  [ \"$state\" = 'COMPLETED' ] && break\n");
    fprintf(fp, "  sleep 1\n");
    fprintf(fp, "  i=$((i + 1))\n");
    fprintf(fp, "done\n");
    fprintf(fp, "[ \"$state\" = 'COMPLETED' ] || exit 12\n");
    fprintf(fp, "udhcpc -q -n -t 8 -p /var/run/udhcpc.$IF.pid -i $IF >>$LOG 2>&1 || exit 13\n");
    fprintf(fp, "ifconfig $IF >>$LOG 2>&1 || true\n");
    fprintf(fp, "exit 0\n");
    fclose(fp);
    chmod("/tmp/k230_wifi_connect.sh", 0700);
    return 0;
}

static int wifi_wait_scan_idle(void)
{
    for(int i = 0; i < WIFI_SCAN_WAIT_MAX_STEPS; i++) {
        int busy;

        pthread_mutex_lock(&wifi_lock);
        busy = wifi_scan_busy;
        pthread_mutex_unlock(&wifi_lock);

        if(!busy) {
            return 0;
        }
        if(i == 0) {
            wifi_log("Connect waits for active scan");
        }
        usleep(WIFI_SCAN_WAIT_STEP_US);
    }

    wifi_log("Connect timed out waiting for active scan");
    return -1;
}

static void wifi_update_page(void)
{
    wifi_ap_info_t aps[NET_MAX_APS];
    int saved[NET_MAX_APS] = {0};
    int count;
    int selected;
    int enabled;
    int scan_busy;
    int connect_busy;
    char status[NET_STATUS_MAX];
    char state[96];
    char ip[64];
    uint32_t color = 0x9AA4AF;

    if(!wifi_status_label) {
        return;
    }

    ui_read_iface_state(NET_WIFI_IFACE, state, sizeof(state), &color);
    ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip));

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    memcpy(aps, wifi_aps, sizeof(aps));
    count = wifi_ap_count;
    for(int i = 0; i < count && i < NET_MAX_APS; i++) {
        saved[i] = wifi_profile_find_locked(aps[i].ssid) >= 0;
    }
    selected = wifi_selected_ap;
    enabled = wifi_enabled;
    scan_busy = wifi_scan_busy;
    connect_busy = wifi_connect_busy;
    snprintf(status, sizeof(status), "%s", wifi_status_text);
    pthread_mutex_unlock(&wifi_lock);

    if(wifi_switch_obj) {
        if(enabled) {
            lv_obj_add_state(wifi_switch_obj, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(wifi_switch_obj, LV_STATE_CHECKED);
        }
    }

    if(wifi_state_label) {
        lv_label_set_text(wifi_state_label, enabled ? ui_tr(state) : ui_tr("Off"));
        lv_obj_set_style_text_color(wifi_state_label,
                                    lv_color_hex(enabled ? color : 0x9AA4AF), 0);
    }
    if(wifi_ip_label) {
        lv_label_set_text(wifi_ip_label, enabled ? ip : "--");
    }
    if(wifi_status_label) {
        if(!enabled) {
            lv_label_set_text(wifi_status_label, ui_tr("Wi-Fi off"));
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x9AA4AF),
                                        0);
        } else if(scan_busy) {
            lv_label_set_text(wifi_status_label, ui_tr("Scanning..."));
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x3DA5FF),
                                        0);
        } else if(connect_busy) {
            lv_label_set_text(wifi_status_label, ui_tr("Connecting..."));
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x3DA5FF),
                                        0);
        } else {
            lv_label_set_text(wifi_status_label, ui_tr(status));
            lv_obj_set_style_text_color(wifi_status_label,
                                        lv_color_hex(strstr(status, "fail") ||
                                                     strstr(status, "missing") ||
                                                     strstr(status, "Failed") ?
                                                     0xEF4D5A : 0xF5A524),
                                        0);
        }
    }
    if(wifi_connect_spinner) {
        if(enabled && connect_busy) {
            lv_obj_clear_flag(wifi_connect_spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wifi_connect_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for(int i = 0; i < NET_MAX_APS; i++) {
        char meta[128];
        char rssi_text[32];
        uint32_t row_color;
        uint32_t signal_color;

        if(!wifi_ap_btn[i] || !wifi_ap_title[i] || !wifi_ap_meta[i]) {
            continue;
        }

        if(enabled && i < count) {
            lv_obj_clear_flag(wifi_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            wireless_signal_icon_update(wifi_ap_signal_icon[i],
                                        aps[i].signal_dbm,
                                        aps[i].signal_valid);
            signal_color = wireless_signal_color(aps[i].signal_dbm,
                                                 aps[i].signal_valid);
            if(wifi_ap_type_icon[i]) {
                wifi_type_icon_update(wifi_ap_type_icon[i], signal_color);
            }
            if(wifi_ap_rssi_label[i]) {
                snprintf(rssi_text, sizeof(rssi_text), "%s",
                         aps[i].signal_valid ? "" : "--");
                if(aps[i].signal_valid) {
                    snprintf(rssi_text, sizeof(rssi_text), "%d dBm",
                             aps[i].signal_dbm);
                }
                lv_label_set_text(wifi_ap_rssi_label[i], rssi_text);
                lv_obj_set_style_text_color(wifi_ap_rssi_label[i],
                                            lv_color_hex(signal_color), 0);
            }
            lv_label_set_text(wifi_ap_title[i], aps[i].ssid);
            if(aps[i].quality[0] && !aps[i].signal_valid) {
                snprintf(meta, sizeof(meta), "%s  %s%s",
                         aps[i].security[0] ? aps[i].security : ui_tr("Open"),
                         aps[i].quality,
                         saved[i] ? ui_tr("  Saved") : "");
            } else {
                snprintf(meta, sizeof(meta), "%s%s",
                         aps[i].security[0] ? aps[i].security : ui_tr("Open"),
                         saved[i] ? ui_tr("  Saved") : "");
            }
            lv_label_set_text(wifi_ap_meta[i], meta);
            row_color = i == selected ? 0x1E3A2F : 0x151B22;
            lv_obj_set_style_bg_color(wifi_ap_btn[i], lv_color_hex(row_color), 0);
        } else {
            wireless_signal_icon_update(wifi_ap_signal_icon[i], 0, 0);
            if(wifi_ap_type_icon[i]) {
                wifi_type_icon_update(wifi_ap_type_icon[i], 0x64748B);
            }
            if(wifi_ap_rssi_label[i]) {
                lv_label_set_text(wifi_ap_rssi_label[i], "--");
                lv_obj_set_style_text_color(wifi_ap_rssi_label[i],
                                            lv_color_hex(0x64748B), 0);
            }
            lv_label_set_text(wifi_ap_title[i], i == 0 ?
                              (enabled ? ui_tr("No networks") :
                               ui_tr("Wi-Fi off")) : "");
            lv_label_set_text(wifi_ap_meta[i], i == 0 ?
                              (enabled ? ui_tr("Scanning every 5 seconds") :
                               "") : "");
            lv_obj_set_style_bg_color(wifi_ap_btn[i], lv_color_hex(0x151B22), 0);
            if(i > 0) {
                lv_obj_add_flag(wifi_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(wifi_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void *wifi_scan_thread_cb(void *arg)
{
    wifi_ap_info_t list[NET_MAX_APS];
    int count = 0;
    char err[96] = "";
    char selected_ssid[NET_SSID_MAX] = "";
    int rc;

    (void)arg;
    rc = wifi_scan_collect(list, &count, err, sizeof(err));

    pthread_mutex_lock(&wifi_lock);
    if(wifi_selected_ssid[0]) {
        snprintf(selected_ssid, sizeof(selected_ssid), "%s", wifi_selected_ssid);
    } else if(wifi_selected_ap >= 0 && wifi_selected_ap < wifi_ap_count) {
        snprintf(selected_ssid, sizeof(selected_ssid), "%s",
                 wifi_aps[wifi_selected_ap].ssid);
    }

    if(rc == 0 && count > 0) {
        int selected;

        memcpy(wifi_aps, list, sizeof(wifi_aps));
        wifi_ap_count = count;

        selected = wifi_scan_find_ap(wifi_aps, wifi_ap_count, selected_ssid);
        if(selected >= 0) {
            wifi_selected_ap = selected;
            snprintf(wifi_selected_ssid, sizeof(wifi_selected_ssid), "%s",
                     wifi_aps[selected].ssid);
            wifi_selected_encrypted = wifi_aps[selected].encrypted;
        } else {
            wifi_selected_ap = -1;
            wifi_selected_encrypted = 0;
            wifi_selected_ssid[0] = '\0';
        }
        if(!wifi_connect_busy) {
            wifi_set_status_locked(count > 0 ? "%d networks found" : "No networks found",
                                   count);
        }
    } else if(rc == 0) {
        if(wifi_ap_count == 0) {
            wifi_selected_ap = -1;
            wifi_selected_encrypted = 0;
            wifi_selected_ssid[0] = '\0';
            if(!wifi_connect_busy) {
                wifi_set_status_locked("No networks found");
            }
        } else if(!wifi_connect_busy) {
            wifi_set_status_locked("No new scan results, keeping list");
        }
    } else {
        if(!wifi_connect_busy) {
            if(wifi_ap_count > 0) {
                wifi_set_status_locked("Scan failed, keeping previous list");
            } else {
                wifi_set_status_locked("%s", err[0] ? err : "Scan failed");
            }
        }
    }
    wifi_scan_busy = 0;
    pthread_mutex_unlock(&wifi_lock);

    return NULL;
}

static void wifi_start_scan(int force)
{
    pthread_t thread;
    int start_thread = 0;
    uint64_t now = ui_monotonic_us();

    pthread_mutex_lock(&wifi_lock);
    if(wifi_enabled && !wifi_scan_busy && !wifi_connect_busy &&
       (force || wifi_last_scan_us == 0 ||
        now - wifi_last_scan_us >= WIFI_AUTO_SCAN_PERIOD_US)) {
        wifi_scan_busy = 1;
        wifi_last_scan_us = now;
        wifi_set_status_locked("Scanning...");
        start_thread = 1;
    }
    pthread_mutex_unlock(&wifi_lock);

    if(start_thread) {
        if(pthread_create(&thread, NULL, wifi_scan_thread_cb, NULL) == 0) {
            pthread_detach(thread);
        } else {
            pthread_mutex_lock(&wifi_lock);
            wifi_scan_busy = 0;
            wifi_set_status_locked("Scan thread failed");
            pthread_mutex_unlock(&wifi_lock);
        }
    }
}

static void *wifi_connect_thread_cb(void *arg)
{
    wifi_connect_request_t *req = (wifi_connect_request_t *)arg;
    char ip[64];
    int rc;

    if(!req) {
        return NULL;
    }

    wifi_load_modules();

    if(wifi_wait_scan_idle() != 0) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Connect failed: scan busy");
        pthread_mutex_unlock(&wifi_lock);
        free(req);
        return NULL;
    }

    if(!ui_path_exists("/sys/class/net/" NET_WIFI_IFACE)) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked(NET_WIFI_IFACE " missing");
        pthread_mutex_unlock(&wifi_lock);
        free(req);
        return NULL;
    }

    if(req->encrypted && strlen(req->password) < 8U) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Password must be at least 8 chars");
        pthread_mutex_unlock(&wifi_lock);
        wifi_log("Reject connect ssid=%s encrypted=%d password_len=%u: password too short",
                 req->ssid, req->encrypted, (unsigned)strlen(req->password));
        free(req);
        return NULL;
    }

    if(req->encrypted && !req->password[0]) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Password required for %s", req->ssid);
        pthread_mutex_unlock(&wifi_lock);
        wifi_log("Reject connect ssid=%s encrypted=%d password_len=0",
                 req->ssid, req->encrypted);
        free(req);
        return NULL;
    }

    if(wifi_write_config(req->ssid, req->password) != 0) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Failed to write Wi-Fi config");
        pthread_mutex_unlock(&wifi_lock);
        wifi_log("Failed to write config ssid=%s encrypted=%d password_len=%u",
                 req->ssid, req->encrypted, (unsigned)strlen(req->password));
        free(req);
        return NULL;
    }

    if(wifi_write_connect_script() != 0) {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Failed to write connect script");
        pthread_mutex_unlock(&wifi_lock);
        wifi_log("Failed to write connect script");
        free(req);
        return NULL;
    }

    (void)unlink(WIFI_CONNECT_LOG);
    wifi_log("%s connect request ssid=%s encrypted=%d password_len=%u",
             req->auto_connect ? "Auto" : "Manual", req->ssid, req->encrypted,
             (unsigned)strlen(req->password));
    rc = system("sh /tmp/k230_wifi_connect.sh");
    rc = ui_shell_exit_code(rc);
    wifi_log("Connect script exit rc=%d", rc);

    pthread_mutex_lock(&wifi_lock);
    wifi_connect_busy = 0;
    if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
        wifi_set_status_locked("Connected to %s  %s", req->ssid, ip);
    } else {
        wifi_set_status_locked("Connect failed, rc=%d", rc);
    }
    pthread_mutex_unlock(&wifi_lock);

    if(rc == 0 && ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
        wifi_profile_remember(req->ssid, req->password, req->encrypted);
    }

    free(req);
    return NULL;
}

static int wifi_start_connect_request(const char *ssid, const char *password,
                                      int encrypted, int auto_connect)
{
    wifi_connect_request_t *req;
    pthread_t thread;

    if(!ssid || !ssid[0]) {
        return -1;
    }

    req = calloc(1, sizeof(*req));
    if(!req) {
        pthread_mutex_lock(&wifi_lock);
        wifi_set_status_locked("No memory for Wi-Fi connect");
        pthread_mutex_unlock(&wifi_lock);
        return -1;
    }

    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");
    req->encrypted = encrypted ? 1 : 0;
    req->auto_connect = auto_connect ? 1 : 0;

    pthread_mutex_lock(&wifi_lock);
    if(!wifi_enabled || wifi_connect_busy) {
        int enabled = wifi_enabled;
        int connecting = wifi_connect_busy;

        pthread_mutex_unlock(&wifi_lock);
        wifi_log("Drop %s connect ssid=%s enabled=%d connect_busy=%d",
                 req->auto_connect ? "auto" : "manual", req->ssid, enabled,
                 connecting);
        free(req);
        return -1;
    }
    wifi_connect_busy = 1;
    wifi_set_status_locked("%s to %s",
                           req->auto_connect ? "Auto connecting" : "Connecting",
                           req->ssid);
    pthread_mutex_unlock(&wifi_lock);

    if(pthread_create(&thread, NULL, wifi_connect_thread_cb, req) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&wifi_lock);
        wifi_connect_busy = 0;
        wifi_set_status_locked("Connect thread failed");
        pthread_mutex_unlock(&wifi_lock);
        free(req);
        return -1;
    }

    app_request_fast_refresh();
    return 0;
}

static void wifi_open_password_dialog(const char *ssid, int encrypted,
                                      const char *initial_password)
{
    ui_input_dialog_config_t config;

    if(!ssid || !ssid[0]) {
        return;
    }

    snprintf(wifi_dialog_ssid, sizeof(wifi_dialog_ssid), "%s", ssid);
    wifi_dialog_encrypted = encrypted ? 1 : 0;

    memset(&config, 0, sizeof(config));
    config.title = wifi_dialog_ssid;
    config.placeholder = encrypted ? "Password" : "Empty for open Wi-Fi";
    config.initial_text = initial_password && initial_password[0] ?
                          initial_password : NULL;
    config.password_mode = encrypted ? 1 : 0;
    config.max_length = NET_PASS_MAX - 1U;
    config.min_length = encrypted ? 8U : 0U;
    config.min_length_text = "Password must be at least 8 chars";
    config.submit_cb = wifi_password_submit_cb;
    config.user_data = wifi_dialog_ssid;
    ui_input_dialog_open(&config);
}

static void wifi_password_submit_cb(const char *password, void *user_data)
{
    const char *ssid = (const char *)user_data;
    int encrypted;

    if(!ssid || !ssid[0]) {
        return;
    }

    pthread_mutex_lock(&wifi_lock);
    encrypted = wifi_dialog_encrypted;
    pthread_mutex_unlock(&wifi_lock);
    wifi_log("Password submit ssid=%s encrypted=%d password_len=%u",
             ssid, encrypted, (unsigned)strlen(password ? password : ""));
    (void)wifi_start_connect_request(ssid, password, encrypted, 0);
}

void ui_wifi_autoconnect_start(void)
{
    char ip[64];
    char ssid[NET_SSID_MAX] = "";
    char password[NET_PASS_MAX] = "";
    int encrypted = 0;
    int should_start = 0;

    if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0 &&
       wifi_wpa_completed()) {
        if(wifi_read_runtime_config(ssid, sizeof(ssid), password,
                                    sizeof(password), &encrypted) == 0) {
            wifi_profile_remember(ssid, password, encrypted);
            wifi_log("Imported active Wi-Fi profile ssid=%s encrypted=%d password_len=%u",
                     ssid, encrypted, (unsigned)strlen(password));
        }
        return;
    }

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    if(!wifi_autoconnect_started && wifi_enabled && !wifi_connect_busy &&
       wifi_profile_count > 0) {
        snprintf(ssid, sizeof(ssid), "%s", wifi_profiles[0].ssid);
        snprintf(password, sizeof(password), "%s", wifi_profiles[0].password);
        encrypted = wifi_profiles[0].encrypted;
        wifi_autoconnect_started = 1;
        should_start = 1;
    }
    pthread_mutex_unlock(&wifi_lock);

    if(should_start) {
        wifi_log("Auto reconnect selected ssid=%s encrypted=%d password_len=%u",
                 ssid, encrypted, (unsigned)strlen(password));
        if(wifi_start_connect_request(ssid, password, encrypted, 1) != 0) {
            pthread_mutex_lock(&wifi_lock);
            wifi_autoconnect_started = 0;
            pthread_mutex_unlock(&wifi_lock);
        }
    }
}

static void wifi_switch_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    int enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

    pthread_mutex_lock(&wifi_lock);
    wifi_enabled = enabled;
    wifi_last_scan_us = 0;
    wifi_ap_count = 0;
    wifi_selected_ap = -1;
    wifi_selected_encrypted = 0;
    wifi_autoconnect_started = 0;
    wifi_selected_ssid[0] = '\0';
    wifi_set_status_locked(enabled ? "Wi-Fi on" : "Wi-Fi off");
    pthread_mutex_unlock(&wifi_lock);

    if(enabled) {
        ui_wifi_autoconnect_start();
        wifi_start_scan(1);
    } else {
        int down_rc = system("wpa_cli -i " NET_WIFI_IFACE " terminate >/dev/null 2>&1 || true; "
                             "ifconfig " NET_WIFI_IFACE " down >/dev/null 2>&1 || true");
        if(down_rc == -1) {
            pthread_mutex_lock(&wifi_lock);
            wifi_set_status_locked("Wi-Fi down command failed");
            pthread_mutex_unlock(&wifi_lock);
        }
    }

    wifi_update_page();
    app_request_fast_refresh();
}

static void wifi_ap_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    char ssid[NET_SSID_MAX] = "";
    char saved_password[NET_PASS_MAX] = "";
    int valid = 0;
    int encrypted = 0;
    int saved_profile = 0;
    uint64_t now = ui_monotonic_us();

    if(index == wifi_suppress_click_index && now < wifi_suppress_click_until_us) {
        return;
    }
    if(now >= wifi_suppress_click_until_us) {
        wifi_suppress_click_index = -1;
        wifi_suppress_click_until_us = 0;
    }

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    if(wifi_enabled && index >= 0 && index < wifi_ap_count) {
        int profile_index;

        wifi_selected_ap = index;
        wifi_selected_encrypted = wifi_aps[index].encrypted;
        encrypted = wifi_selected_encrypted;
        snprintf(ssid, sizeof(ssid), "%s", wifi_aps[index].ssid);
        profile_index = wifi_profile_find_locked(ssid);
        if(profile_index >= 0) {
            saved_profile = 1;
            snprintf(saved_password, sizeof(saved_password), "%s",
                     wifi_profiles[profile_index].password);
            encrypted = wifi_profiles[profile_index].encrypted || encrypted;
        }
        snprintf(wifi_selected_ssid, sizeof(wifi_selected_ssid), "%s", ssid);
        wifi_set_status_locked(saved_profile ? "Connecting saved %s" :
                               "Selected %s", wifi_selected_ssid);
        valid = 1;
    }
    pthread_mutex_unlock(&wifi_lock);

    wifi_update_page();
    app_request_fast_refresh();

    if(valid) {
        if(saved_profile) {
            (void)wifi_start_connect_request(ssid, saved_password, encrypted, 0);
        } else {
            wifi_open_password_dialog(ssid, encrypted, NULL);
        }
    }
}

static void wifi_ap_long_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    char ssid[NET_SSID_MAX] = "";
    char saved_password[NET_PASS_MAX] = "";
    int valid = 0;
    int encrypted = 0;

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    if(wifi_enabled && index >= 0 && index < wifi_ap_count) {
        int profile_index;

        encrypted = wifi_aps[index].encrypted;
        snprintf(ssid, sizeof(ssid), "%s", wifi_aps[index].ssid);
        profile_index = wifi_profile_find_locked(ssid);
        if(profile_index >= 0) {
            snprintf(saved_password, sizeof(saved_password), "%s",
                     wifi_profiles[profile_index].password);
            encrypted = wifi_profiles[profile_index].encrypted || encrypted;
        }
        wifi_suppress_click_index = index;
        wifi_suppress_click_until_us = ui_monotonic_us() + 900000ULL;
        wifi_set_status_locked("Edit password for %s", ssid);
        valid = 1;
    }
    pthread_mutex_unlock(&wifi_lock);

    wifi_update_page();
    app_request_fast_refresh();

    if(valid) {
        wifi_open_password_dialog(ssid, encrypted, saved_password);
    }
}

static void wifi_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    wifi_start_scan(0);
    wifi_update_page();
}

void ui_wifi_cleanup(void)
{
    if(wifi_timer) {
        lv_timer_delete(wifi_timer);
        wifi_timer = NULL;
    }
    wifi_switch_obj = NULL;
    wifi_state_label = NULL;
    wifi_ip_label = NULL;
    wifi_status_label = NULL;
    wifi_connect_spinner = NULL;
    memset(wifi_ap_btn, 0, sizeof(wifi_ap_btn));
    memset(wifi_ap_type_icon, 0, sizeof(wifi_ap_type_icon));
    memset(wifi_ap_signal_icon, 0, sizeof(wifi_ap_signal_icon));
    memset(wifi_ap_rssi_label, 0, sizeof(wifi_ap_rssi_label));
    memset(wifi_ap_title, 0, sizeof(wifi_ap_title));
    memset(wifi_ap_meta, 0, sizeof(wifi_ap_meta));
}

void ui_wifi_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *summary;
    lv_obj_t *list;
    lv_obj_t *title;
    char ip[64];
    int has_ip;
    int landscape = ui_is_landscape();
    int panel_x = ui_page_side_margin();
    int panel_w = ui_screen_width() - panel_x * 2;
    int row_x = landscape ? 12 : 0;
    int list_y = 228;
    int list_h = landscape ?
                 ui_screen_height() - ui_page_top_y(144) - list_y - 24 : 810;

    if(panel_w < 520) {
        panel_w = 520;
    }
    if(list_h < 240) {
        list_h = 240;
    }

    ui_create_header(scr, "Wi-Fi");

    body = ui_page_body(scr, 144);

    summary = ui_panel(body, panel_x, 0, panel_w, 204);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x101820), 0);

    title = ui_label(summary, "Wi-Fi", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    wifi_switch_obj = lv_switch_create(summary);
    lv_obj_set_size(wifi_switch_obj, 84, 44);
    lv_obj_align(wifi_switch_obj, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_state(wifi_switch_obj, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(wifi_switch_obj, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_bg_color(wifi_switch_obj, lv_color_hex(0x25C281),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(wifi_switch_obj, wifi_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    wifi_state_label = ui_label(summary, "--", &lv_font_montserrat_20, 0x9AA4AF);
    lv_obj_set_width(wifi_state_label, panel_w - 150);
    lv_label_set_long_mode(wifi_state_label, LV_LABEL_LONG_DOT);
    lv_obj_align(wifi_state_label, LV_ALIGN_TOP_LEFT, 0, 52);

    wifi_ip_label = ui_label(summary, "--", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(wifi_ip_label, LV_ALIGN_TOP_LEFT, 0, 86);

    wifi_status_label = ui_label(summary, "Wi-Fi on", &lv_font_montserrat_16,
                                 0xF5A524);
    lv_obj_set_width(wifi_status_label, panel_w - 32);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 132);

    wifi_connect_spinner = lv_spinner_create(summary);
    lv_obj_set_size(wifi_connect_spinner, 34, 34);
    lv_obj_align(wifi_connect_spinner, LV_ALIGN_TOP_RIGHT, -104, 58);
    lv_obj_add_flag(wifi_connect_spinner, LV_OBJ_FLAG_HIDDEN);

    list = ui_panel(body, panel_x, list_y, panel_w, list_h);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    title = ui_label(list, "Available networks", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 0);

    for(int i = 0; i < NET_MAX_APS; i++) {
        int row_w;
        int text_w;

        wifi_ap_btn[i] = lv_obj_create(list);
        lv_obj_set_pos(wifi_ap_btn[i], row_x, 44 + i * 72);
        row_w = panel_w - row_x * 2;
        lv_obj_set_size(wifi_ap_btn[i], row_w, 64);
        lv_obj_set_style_bg_color(wifi_ap_btn[i], lv_color_hex(0x151B22), 0);
        lv_obj_set_style_bg_opa(wifi_ap_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(wifi_ap_btn[i], lv_color_hex(0x202832),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_radius(wifi_ap_btn[i], 8, 0);
        lv_obj_set_style_border_width(wifi_ap_btn[i], 1, 0);
        lv_obj_set_style_border_color(wifi_ap_btn[i], lv_color_hex(0x25303A), 0);
        lv_obj_clear_flag(wifi_ap_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(wifi_ap_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(wifi_ap_btn[i], 4);
        lv_obj_add_event_cb(wifi_ap_btn[i], wifi_ap_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_add_event_cb(wifi_ap_btn[i], wifi_ap_long_event_cb,
                            LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        wifi_ap_type_icon[i] = wifi_type_icon_create(wifi_ap_btn[i]);
        lv_obj_align(wifi_ap_type_icon[i], LV_ALIGN_LEFT_MID, 12, 0);

        wifi_ap_signal_icon[i] = wireless_signal_icon_create(wifi_ap_btn[i]);
        lv_obj_align(wifi_ap_signal_icon[i], LV_ALIGN_RIGHT_MID, -58, -8);

        wifi_ap_rssi_label[i] = ui_label(wifi_ap_btn[i], "--",
                                         &lv_font_montserrat_12, 0x64748B);
        lv_obj_set_width(wifi_ap_rssi_label[i], 90);
        lv_obj_set_style_text_align(wifi_ap_rssi_label[i], LV_TEXT_ALIGN_RIGHT,
                                    0);
        lv_obj_align(wifi_ap_rssi_label[i], LV_ALIGN_RIGHT_MID, -48, 18);
        ui_make_click_forwarder(wifi_ap_rssi_label[i]);

        text_w = row_w - 220;
        if(text_w < 248) {
            text_w = 248;
        }

        wifi_ap_title[i] = ui_label(wifi_ap_btn[i],
                                    i == 0 ? "Scanning every 5 seconds" : "",
                                    &lv_font_montserrat_18, 0xF2F5F8);
        lv_obj_set_width(wifi_ap_title[i], text_w);
        lv_label_set_long_mode(wifi_ap_title[i], LV_LABEL_LONG_DOT);
        lv_obj_align(wifi_ap_title[i], LV_ALIGN_LEFT_MID, 58, -10);
        ui_make_click_forwarder(wifi_ap_title[i]);

        wifi_ap_meta[i] = ui_label(wifi_ap_btn[i], "", &lv_font_montserrat_14,
                                   0x9AA4AF);
        lv_obj_set_width(wifi_ap_meta[i], text_w);
        lv_label_set_long_mode(wifi_ap_meta[i], LV_LABEL_LONG_DOT);
        lv_obj_align(wifi_ap_meta[i], LV_ALIGN_LEFT_MID, 58, 14);
        ui_make_click_forwarder(wifi_ap_meta[i]);

        if(i > 0) {
            lv_obj_add_flag(wifi_ap_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    has_ip = ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0 &&
             wifi_wpa_completed();

    pthread_mutex_lock(&wifi_lock);
    wifi_profiles_load_locked();
    wifi_enabled = 1;
    wifi_last_scan_us = 0;
    if(has_ip) {
        wifi_set_status_locked("Wi-Fi connected");
    } else {
        wifi_set_status_locked("Wi-Fi on");
    }
    pthread_mutex_unlock(&wifi_lock);

    wifi_timer = lv_timer_create(wifi_timer_cb, 1000, NULL);
    wifi_start_scan(1);
    wifi_update_page();
}
