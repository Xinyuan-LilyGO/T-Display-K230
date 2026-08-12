#include "ui_time_settings.h"

#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define TIME_NTP_KEY "time.ntp_server"
#define TIME_TZ_KEY "time.timezone"
#define TIME_NTP_DEFAULT "pool.ntp.org"
#define TIME_TZ_DEFAULT "UTC0"
#define TIME_NTP_LOG "/tmp/k230_time_sync.log"

typedef struct {
    const char *name;
    const char *posix_tz;
    uint32_t color;
} time_zone_option_t;

static const time_zone_option_t time_zone_options[] = {
    { "UTC", "UTC0", 0x3DA5FF },
    { "Pacific/Honolulu", "HST10", 0x22D3EE },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", 0x38BDF8 },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0", 0xEC4899 },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0", 0xA3E635 },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0", 0xF97316 },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0", 0xF5A524 },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0", 0x60A5FA },
    { "America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0", 0x8B5CF6 },
    { "America/Sao_Paulo", "BRT3", 0x25C281 },
    { "America/Buenos_Aires", "ART3", 0x14B8A6 },
    { "Atlantic/Azores", "AZOT1AZOST,M3.5.0/0,M10.5.0/1", 0x0EA5E9 },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0", 0x8B5CF6 },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3", 0x3DA5FF },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", 0x25C281 },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4", 0xF5A524 },
    { "Europe/Moscow", "MSK-3", 0xEF4D5A },
    { "Asia/Dubai", "GST-4", 0x60A5FA },
    { "Asia/Karachi", "PKT-5", 0x38BDF8 },
    { "Asia/Kolkata", "IST-5:30", 0xA3E635 },
    { "Asia/Dhaka", "BDT-6", 0xF97316 },
    { "Asia/Bangkok", "ICT-7", 0x22D3EE },
    { "Asia/Shanghai", "CST-8", 0x25C281 },
    { "Asia/Hong_Kong", "HKT-8", 0x3DA5FF },
    { "Asia/Taipei", "CST-8", 0x14B8A6 },
    { "Asia/Singapore", "SGT-8", 0x06B6D4 },
    { "Asia/Tokyo", "JST-9", 0xEC4899 },
    { "Asia/Seoul", "KST-9", 0x8B5CF6 },
    { "Australia/Perth", "AWST-8", 0x60A5FA },
    { "Australia/Darwin", "ACST-9:30", 0xF5A524 },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", 0xEF4D5A },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3", 0x25C281 },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", 0x3DA5FF },
    { "Pacific/Chatham", "CHAST-12:45CHADT,M9.5.0,M4.1.0/3:45", 0xA3E635 },
    { "Pacific/Kiritimati", "LINT-14", 0xF97316 },
};

static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_obj_t *time_now_label;
static lv_obj_t *time_zone_label;
static lv_obj_t *time_ntp_label;
static lv_obj_t *time_status_label;
static lv_timer_t *time_timer;
static int time_sync_busy;
static char time_ntp_server[128] = TIME_NTP_DEFAULT;
static char time_timezone[64] = TIME_TZ_DEFAULT;
static char time_status_text[160] = "Ready";

static int time_zone_index_by_posix(const char *posix_tz)
{
    if(!posix_tz || !posix_tz[0]) {
        return 0;
    }

    for(size_t i = 0; i < sizeof(time_zone_options) / sizeof(time_zone_options[0]); i++) {
        if(strcmp(time_zone_options[i].posix_tz, posix_tz) == 0) {
            return (int)i;
        }
    }

    return 0;
}

static const time_zone_option_t *time_current_zone(void)
{
    return &time_zone_options[time_zone_index_by_posix(time_timezone)];
}

static void time_set_status_locked(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(time_status_text, sizeof(time_status_text), fmt, ap);
    va_end(ap);
}

static int time_ntp_server_valid(const char *server)
{
    size_t len;

    if(!server) {
        return 0;
    }

    len = strlen(server);
    if(len == 0 || len >= sizeof(time_ntp_server) || server[0] == '-') {
        return 0;
    }

    for(size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)server[i];

        if(!(isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ':')) {
            return 0;
        }
    }

    return 1;
}

static void time_load_prefs(void)
{
    char server[sizeof(time_ntp_server)];
    char timezone[sizeof(time_timezone)];

    ui_prefs_get(TIME_NTP_KEY, server, sizeof(server), TIME_NTP_DEFAULT);
    ui_prefs_get(TIME_TZ_KEY, timezone, sizeof(timezone), TIME_TZ_DEFAULT);

    if(!time_ntp_server_valid(server)) {
        snprintf(server, sizeof(server), "%s", TIME_NTP_DEFAULT);
    }
    if(time_zone_index_by_posix(timezone) == 0 &&
       strcmp(timezone, time_zone_options[0].posix_tz) != 0) {
        snprintf(timezone, sizeof(timezone), "%s", TIME_TZ_DEFAULT);
    }

    pthread_mutex_lock(&time_lock);
    snprintf(time_ntp_server, sizeof(time_ntp_server), "%s", server);
    snprintf(time_timezone, sizeof(time_timezone), "%s", timezone);
    pthread_mutex_unlock(&time_lock);
}

static int write_text_file(const char *path, const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(path, "w");
    if(!fp) {
        return -1;
    }

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    return fclose(fp) == 0 ? 0 : -1;
}

static void time_write_timezone_files(const char *posix_tz)
{
    if(!posix_tz || !posix_tz[0]) {
        posix_tz = TIME_TZ_DEFAULT;
    }

    write_text_file("/etc/TZ", "%s\n", posix_tz);
    write_text_file("/etc/profile.d/k230_timezone.sh",
                    "export TZ='%s'\n", posix_tz);
}

static void time_apply_timezone(const char *posix_tz)
{
    if(!posix_tz || !posix_tz[0]) {
        posix_tz = TIME_TZ_DEFAULT;
    }

    setenv("TZ", posix_tz, 1);
    tzset();
    time_write_timezone_files(posix_tz);
}

static void time_write_ntp_files(const char *server)
{
    if(!time_ntp_server_valid(server)) {
        server = TIME_NTP_DEFAULT;
    }

    write_text_file("/etc/default/sntp",
                    "SNTP_SERVERS=\"%s\"\n"
                    "SNTP_ARGS=\"-Ss -M 128\"\n",
                    server);
    write_text_file("/etc/ntp.conf",
                    "server %s iburst\n\n"
                    "restrict default nomodify nopeer noquery limited kod\n"
                    "restrict 127.0.0.1\n"
                    "restrict [::1]\n",
                    server);
}

void ui_time_settings_apply_startup(void)
{
    time_load_prefs();

    pthread_mutex_lock(&time_lock);
    time_apply_timezone(time_timezone);
    time_write_ntp_files(time_ntp_server);
    pthread_mutex_unlock(&time_lock);
}

static void time_refresh_labels(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char now_text[48];
    char server[sizeof(time_ntp_server)];
    char status[sizeof(time_status_text)];
    const time_zone_option_t *zone;
    int busy;

    localtime_r(&now, &tm_now);
    strftime(now_text, sizeof(now_text), "%Y-%m-%d %H:%M:%S", &tm_now);

    pthread_mutex_lock(&time_lock);
    zone = time_current_zone();
    snprintf(server, sizeof(server), "%s", time_ntp_server);
    snprintf(status, sizeof(status), "%s", time_status_text);
    busy = time_sync_busy;
    pthread_mutex_unlock(&time_lock);

    if(time_now_label) {
        lv_label_set_text(time_now_label, now_text);
    }
    if(time_zone_label) {
        lv_label_set_text(time_zone_label, ui_tr(zone->name));
    }
    if(time_ntp_label) {
        lv_label_set_text(time_ntp_label, server);
    }
    if(time_status_label) {
        lv_label_set_text(time_status_label, busy ? ui_tr("Syncing") :
                          ui_tr(status));
        lv_obj_set_style_text_color(time_status_label,
                                    lv_color_hex(busy ? 0xF5A524 : 0x9AA4AF),
                                    0);
    }
}

static void time_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    time_refresh_labels();
}

static void time_refresh_page_async(void *user_data)
{
    (void)user_data;
    app_refresh_current_page();
}

static void time_server_submit_cb(const char *text, void *user_data)
{
    char server[sizeof(time_ntp_server)];

    (void)user_data;
    snprintf(server, sizeof(server), "%s", text ? text : "");
    ui_trim_text(server);

    pthread_mutex_lock(&time_lock);
    if(!time_ntp_server_valid(server)) {
        time_set_status_locked("Invalid server");
        pthread_mutex_unlock(&time_lock);
        time_refresh_labels();
        app_request_fast_refresh();
        return;
    }

    snprintf(time_ntp_server, sizeof(time_ntp_server), "%s", server);
    ui_prefs_set(TIME_NTP_KEY, time_ntp_server);
    time_write_ntp_files(time_ntp_server);
    time_set_status_locked("Saved");
    pthread_mutex_unlock(&time_lock);

    time_refresh_labels();
    app_request_fast_refresh();
}

static void time_edit_server_event_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config;
    char server[sizeof(time_ntp_server)];

    (void)event;
    pthread_mutex_lock(&time_lock);
    snprintf(server, sizeof(server), "%s", time_ntp_server);
    pthread_mutex_unlock(&time_lock);

    memset(&config, 0, sizeof(config));
    config.title = "NTP server";
    config.placeholder = TIME_NTP_DEFAULT;
    config.initial_text = server;
    config.password_mode = 0;
    config.max_length = sizeof(time_ntp_server) - 1U;
    config.submit_cb = time_server_submit_cb;
    config.submit_text = "Save";
    config.cancel_text = "Cancel";
    ui_input_dialog_open(&config);
}

static void *time_sync_thread_cb(void *arg)
{
    char server[sizeof(time_ntp_server)];
    char cmd[384];
    int rc;

    (void)arg;

    pthread_mutex_lock(&time_lock);
    snprintf(server, sizeof(server), "%s", time_ntp_server);
    time_set_status_locked("Syncing");
    pthread_mutex_unlock(&time_lock);

    time_write_ntp_files(server);
    snprintf(cmd, sizeof(cmd),
             "(/etc/init.d/S49ntp stop; /usr/bin/ntpdate -u %s; rc=$?; "
             "hwclock -w 2>/dev/null || true; /etc/init.d/S49ntp start; "
             "exit $rc) >%s 2>&1",
             server, TIME_NTP_LOG);
    rc = system(cmd);

    pthread_mutex_lock(&time_lock);
    time_sync_busy = 0;
    time_set_status_locked(ui_shell_exit_code(rc) == 0 ? "Sync OK" :
                           "Sync failed");
    pthread_mutex_unlock(&time_lock);

    return NULL;
}

static void time_sync_now_event_cb(lv_event_t *event)
{
    pthread_t thread;
    int start_thread = 0;

    (void)event;
    pthread_mutex_lock(&time_lock);
    if(!time_sync_busy) {
        time_sync_busy = 1;
        time_set_status_locked("Syncing");
        start_thread = 1;
    }
    pthread_mutex_unlock(&time_lock);

    if(start_thread) {
        if(pthread_create(&thread, NULL, time_sync_thread_cb, NULL) == 0) {
            pthread_detach(thread);
        } else {
            pthread_mutex_lock(&time_lock);
            time_sync_busy = 0;
            time_set_status_locked("Sync failed");
            pthread_mutex_unlock(&time_lock);
        }
    }

    time_refresh_labels();
    app_request_fast_refresh();
}

static void time_zone_event_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index < 0 ||
       index >= (int)(sizeof(time_zone_options) / sizeof(time_zone_options[0]))) {
        return;
    }

    pthread_mutex_lock(&time_lock);
    snprintf(time_timezone, sizeof(time_timezone), "%s",
             time_zone_options[index].posix_tz);
    ui_prefs_set(TIME_TZ_KEY, time_timezone);
    time_apply_timezone(time_timezone);
    time_set_status_locked("Timezone saved");
    pthread_mutex_unlock(&time_lock);

    lv_async_call(time_refresh_page_async, NULL);
}

static lv_obj_t *time_zone_row(lv_obj_t *parent, int y, int index)
{
    const time_zone_option_t *zone = &time_zone_options[index];
    int selected = strcmp(time_timezone, zone->posix_tz) == 0;
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, ui_fit_width(lv_obj_get_parent(row), 0, 488), 62);
    lv_obj_set_style_bg_color(row,
                              lv_color_hex(selected ? zone->color : 0x202832),
                              0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2D3744), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(row, 2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, selected ? 0 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 6);
    lv_obj_add_event_cb(row, time_zone_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);

    lv_obj_t *name = ui_label(row, zone->name, &lv_font_montserrat_18,
                              selected ? 0xFFFFFF : 0xF2F5F8);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
    ui_make_click_forwarder(name);

    if(selected) {
        lv_obj_t *mark = ui_label(row, LV_SYMBOL_OK, &lv_font_montserrat_20,
                                  0xFFFFFF);
        lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
        ui_make_click_forwarder(mark);
    }

    return row;
}

void ui_time_settings_cleanup(void)
{
    if(time_timer) {
        lv_timer_delete(time_timer);
        time_timer = NULL;
    }
    time_now_label = NULL;
    time_zone_label = NULL;
    time_ntp_label = NULL;
    time_status_label = NULL;
}

void ui_time_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *btn;
    int zone_count =
        (int)(sizeof(time_zone_options) / sizeof(time_zone_options[0]));
    int zone_panel_h = 76 + zone_count * 72 + 20;

    time_load_prefs();
    ui_time_settings_apply_startup();

    ui_create_header(scr, "Date & time");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    panel = ui_panel(body, 24, 24, 520, 222);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101820), 0);
    ui_label(panel, "Local time", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    time_now_label = ui_label(panel, "--", &lv_font_montserrat_28, 0x25C281);
    lv_obj_align(time_now_label, LV_ALIGN_TOP_LEFT, 0, 44);
    ui_info_row(panel, 106, "Time zone", time_current_zone()->name, 0x3DA5FF);
    time_zone_label = lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1);
    time_status_label = ui_label(panel, "Ready", &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(time_status_label, ui_inner_width());
    lv_label_set_long_mode(time_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(time_status_label, LV_ALIGN_TOP_LEFT, 0, 166);

    panel = ui_panel(body, 24, 270, 520, 238);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x121923), 0);
    ui_label(panel, "NTP server", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    time_ntp_label = ui_label(panel, time_ntp_server, &lv_font_montserrat_18,
                              0x25C281);
    lv_obj_set_width(time_ntp_label, ui_inner_width());
    lv_label_set_long_mode(time_ntp_label, LV_LABEL_LONG_DOT);
    lv_obj_align(time_ntp_label, LV_ALIGN_TOP_LEFT, 0, 44);
    btn = ui_command_button(panel, 0, 112, 216, "Edit server", 0x3DA5FF);
    lv_obj_add_event_cb(btn, time_edit_server_event_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(panel, 272, 112, 216, "Sync now", 0x25C281);
    lv_obj_add_event_cb(btn, time_sync_now_event_cb, LV_EVENT_CLICKED, NULL);

    panel = ui_panel(body, 24, 532, 520, zone_panel_h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), 0);
    ui_label(panel, "Time zone", &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_align(lv_obj_get_child(panel, lv_obj_get_child_count(panel) - 1),
                 LV_ALIGN_TOP_LEFT, 0, 0);

    for(int i = 0; i < zone_count; i++) {
        time_zone_row(panel, 52 + i * 72, i);
    }

    time_timer = lv_timer_create(time_timer_cb, 1000, NULL);
    time_refresh_labels();
}
