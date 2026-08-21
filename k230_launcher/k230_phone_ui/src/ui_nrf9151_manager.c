#include "ui_nrf9151_manager.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define K230_NRF9151_UART_BAUD B115200
#define K230_NRF9151_EN_GPIO 2U
#define K230_NRF9151_IOMUX_BASE 0x91105000UL
#define K230_NRF9151_IOMUX_SIZE 0x1000UL
#define K230_NRF9151_IOMUX_IO2_OFFSET (2U * 4U)
#define K230_NRF9151_IOMUX_IO28_OFFSET (28U * 4U)
#define K230_NRF9151_IOMUX_IO29_OFFSET (29U * 4U)
#define K230_NRF9151_IOMUX_IO50_OFFSET (50U * 4U)
#define K230_NRF9151_IOMUX_IO51_OFFSET (51U * 4U)
#define K230_NRF9151_IOMUX_FUNC_ALT0 (0U << 11)
#define K230_NRF9151_IOMUX_FUNC_ALT2 (2U << 11)
#define K230_NRF9151_IOMUX_IE_BIT (1U << 8)
#define K230_NRF9151_IOMUX_OE_BIT (1U << 7)
#define K230_NRF9151_IOMUX_ST_BIT (1U << 0)
#define K230_NRF9151_IOMUX_DS_8MA (8U << 1)
#define K230_NRF9151_CMD_TIMEOUT_US 1800000ULL
#define K230_NRF9151_LONG_TIMEOUT_US 12000000ULL
#define K230_NRF9151_HTTP_TIMEOUT_US 60000000ULL
#define K230_NRF9151_MQTT_TIMEOUT_US 30000000ULL
#define K230_NRF9151_GNSS_NMEA_RESTART_US (15ULL * 1000000ULL)
#define K230_NRF9151_STATUS_REFRESH_US (30ULL * 1000000ULL)
#define K230_NRF9151_STATUS_RETRY_US (8ULL * 1000000ULL)
#define K230_NRF9151_MANAGER_LOG "/tmp/k230_nrf9151_test.log"
#define K230_NRF9151_PREF_DIR "/root/.config/k230_phone_ui"
#define K230_NRF9151_PREF_FILE K230_NRF9151_PREF_DIR "/settings.conf"
#define K230_NRF9151_PREF_LOCATION_AUTOSTART "nrf9151.location.autostart"
#define K230_NRF9151_GNSS_OWNER_LEGACY (1U << 0)
#define K230_NRF9151_GNSS_OWNER_CELLULAR (1U << 1)
#define K230_NRF9151_GNSS_OWNER_MESHTASTIC (1U << 2)
#define K230_NRF9151_GNSS_OWNER_LOCATION (1U << 3)

typedef struct {
    int fd;
    int lock_fd;
    int stop;
    int active;
    int configured;
    int first_fix_reported;
    unsigned int owners;
    uint64_t session_start_us;
    uint64_t last_fix_us;
    uint64_t last_nmea_us;
    uint64_t last_restart_us;
    char line[256];
    size_t line_used;
} k230_nrf9151_gnss_monitor_t;

typedef struct {
    int fd;
    int lock_fd;
    int connected;
    int qos;
    char broker[160];
    char topic[160];
} k230_nrf9151_mqtt_session_t;

typedef struct {
    pthread_t thread;
    int started;
    int stop;
    int request;
    int refreshing;
    uint64_t last_run_us;
    uint64_t last_success_us;
} k230_nrf9151_status_monitor_t;

static pthread_mutex_t nrf9151_manager_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t nrf9151_gpio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t nrf9151_mqtt_lock = PTHREAD_MUTEX_INITIALIZER;
static k230_nrf9151_status_t nrf9151_status_cache;
static int nrf9151_status_cache_loaded;
static k230_nrf9151_gnss_monitor_t nrf9151_gnss_monitor = {
    .fd = -1,
    .lock_fd = -1,
};
static k230_nrf9151_mqtt_session_t nrf9151_mqtt_session = {
    .fd = -1,
    .lock_fd = -1,
};
static k230_nrf9151_status_monitor_t nrf9151_status_monitor;
static struct gpiod_chip *nrf9151_gpio_chip;
static struct gpiod_line_request *nrf9151_en_gpio_request;
static unsigned int nrf9151_en_gpio_offset;

static uint64_t nrf9151_manager_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void nrf9151_manager_debug_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    if(!fmt) {
        return;
    }
    fp = fopen(K230_NRF9151_MANAGER_LOG, "a");
    if(!fp) {
        return;
    }
    fprintf(fp, "%llu ",
            (unsigned long long)nrf9151_manager_monotonic_us());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static unsigned int nrf9151_manager_gnss_owner_bit(const char *owner)
{
    if(!owner || !owner[0]) {
        return K230_NRF9151_GNSS_OWNER_LEGACY;
    }
    if(strcmp(owner, "cellular") == 0 || strcmp(owner, "lte") == 0) {
        return K230_NRF9151_GNSS_OWNER_CELLULAR;
    }
    if(strcmp(owner, "meshtastic") == 0 || strcmp(owner, "mesh") == 0) {
        return K230_NRF9151_GNSS_OWNER_MESHTASTIC;
    }
    if(strcmp(owner, K230_NRF9151_LOCATION_OWNER) == 0 ||
       strcmp(owner, "location") == 0 || strcmp(owner, "system") == 0) {
        return K230_NRF9151_GNSS_OWNER_LOCATION;
    }
    return K230_NRF9151_GNSS_OWNER_LEGACY;
}

static const char *nrf9151_manager_gnss_owner_name(unsigned int bit)
{
    switch(bit) {
    case K230_NRF9151_GNSS_OWNER_CELLULAR:
        return "cellular";
    case K230_NRF9151_GNSS_OWNER_MESHTASTIC:
        return "meshtastic";
    case K230_NRF9151_GNSS_OWNER_LOCATION:
        return "location";
    default:
        return "legacy";
    }
}

static void nrf9151_manager_trim_text(char *text)
{
    char *start;
    size_t len;

    if(!text) {
        return;
    }
    start = text;
    while(*start && isspace((unsigned char)*start)) {
        start++;
    }
    if(start != text) {
        memmove(text, start, strlen(start) + 1U);
    }

    len = strlen(text);
    while(len > 0 && isspace((unsigned char)text[len - 1U])) {
        text[--len] = '\0';
    }
}

static int nrf9151_manager_pref_get(const char *key, char *value,
                                    size_t value_len, const char *fallback)
{
    FILE *fp;
    char line[256];

    if(!key || !value || value_len == 0U) {
        return -1;
    }
    snprintf(value, value_len, "%s", fallback ? fallback : "");
    fp = fopen(K230_NRF9151_PREF_FILE, "r");
    if(!fp) {
        return -1;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *sep;

        line[strcspn(line, "\r\n")] = '\0';
        nrf9151_manager_trim_text(line);
        if(!line[0] || line[0] == '#') {
            continue;
        }
        sep = strchr(line, '=');
        if(!sep) {
            continue;
        }
        *sep++ = '\0';
        nrf9151_manager_trim_text(line);
        nrf9151_manager_trim_text(sep);
        if(strcmp(line, key) == 0) {
            snprintf(value, value_len, "%s", sep);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int nrf9151_manager_pref_set(const char *key, const char *value)
{
    char lines[96][256];
    char tmp_path[sizeof(K230_NRF9151_PREF_FILE) + 8];
    FILE *fp;
    int count = 0;
    int replaced = 0;

    if(!key || !value || !key[0]) {
        errno = EINVAL;
        return -1;
    }

    fp = fopen(K230_NRF9151_PREF_FILE, "r");
    if(fp) {
        while(count < (int)(sizeof(lines) / sizeof(lines[0])) &&
              fgets(lines[count], sizeof(lines[count]), fp)) {
            char probe[256];
            char *sep;

            lines[count][strcspn(lines[count], "\r\n")] = '\0';
            snprintf(probe, sizeof(probe), "%s", lines[count]);
            nrf9151_manager_trim_text(probe);
            sep = strchr(probe, '=');
            if(sep) {
                *sep = '\0';
                nrf9151_manager_trim_text(probe);
                if(strcmp(probe, key) == 0) {
                    snprintf(lines[count], sizeof(lines[count]), "%s=%s",
                             key, value);
                    replaced = 1;
                }
            }
            count++;
        }
        fclose(fp);
    }
    if(!replaced && count < (int)(sizeof(lines) / sizeof(lines[0]))) {
        snprintf(lines[count++], sizeof(lines[0]), "%s=%s", key, value);
    }

    if(mkdir("/root/.config", 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if(mkdir(K230_NRF9151_PREF_DIR, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", K230_NRF9151_PREF_FILE);
    fp = fopen(tmp_path, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "# k230_phone_ui persistent settings\n");
    for(int i = 0; i < count; i++) {
        if(lines[i][0] &&
           strcmp(lines[i], "# k230_phone_ui persistent settings") != 0) {
            fprintf(fp, "%s\n", lines[i]);
        }
    }
    if(fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if(rename(tmp_path, K230_NRF9151_PREF_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int nrf9151_manager_starts_with(const char *text, const char *prefix)
{
    size_t len;

    if(!text || !prefix) {
        return 0;
    }
    len = strlen(prefix);
    return strncmp(text, prefix, len) == 0;
}

static void nrf9151_manager_log_append(char *log, size_t log_len,
                                       const char *fmt, ...)
{
    size_t used;
    va_list ap;

    if(!log || log_len == 0U || !fmt) {
        return;
    }
    used = strlen(log);
    if(used + 2U >= log_len) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(log + used, log_len - used, fmt, ap);
    va_end(ap);
    used = strlen(log);
    if(used + 1U < log_len) {
        log[used++] = '\n';
        log[used] = '\0';
    }
}

static int nrf9151_manager_parse_int_field(const char *field)
{
    char *end = NULL;
    long value;

    if(!field || !field[0]) {
        return -1;
    }
    errno = 0;
    value = strtol(field, &end, 10);
    if(end == field || errno == ERANGE) {
        return -1;
    }
    return (int)value;
}

static void nrf9151_manager_clear_satellite_details(
    k230_nrf9151_status_t *status)
{
    if(!status) {
        return;
    }
    memset(status->satellite_detail, 0, sizeof(status->satellite_detail));
    status->satellite_detail_count = 0;
}

static void nrf9151_manager_recount_satellite_details(
    k230_nrf9151_status_t *status)
{
    unsigned int count = 0;

    if(!status) {
        return;
    }
    for(size_t i = 0; i < K230_NRF9151_MAX_GNSS_SATS; i++) {
        if(status->satellite_detail[i].valid) {
            count++;
        }
    }
    status->satellite_detail_count = count;
}

static void nrf9151_manager_clear_satellite_talker(
    k230_nrf9151_status_t *status, const char *talker)
{
    if(!status || !talker || !talker[0]) {
        return;
    }
    for(size_t i = 0; i < K230_NRF9151_MAX_GNSS_SATS; i++) {
        if(status->satellite_detail[i].valid &&
           strncmp(status->satellite_detail[i].talker, talker, 2) == 0) {
            memset(&status->satellite_detail[i], 0,
                   sizeof(status->satellite_detail[i]));
        }
    }
    nrf9151_manager_recount_satellite_details(status);
}

static void nrf9151_manager_upsert_satellite_detail(
    k230_nrf9151_status_t *status, const char *talker, int prn, int elevation,
    int azimuth, int cn0)
{
    k230_nrf9151_satellite_t *slot = NULL;

    if(!status || !talker || prn <= 0) {
        return;
    }
    for(size_t i = 0; i < K230_NRF9151_MAX_GNSS_SATS; i++) {
        if(status->satellite_detail[i].valid) {
            if(status->satellite_detail[i].prn == prn &&
               strncmp(status->satellite_detail[i].talker, talker, 2) == 0) {
                slot = &status->satellite_detail[i];
                break;
            }
        } else if(!slot) {
            slot = &status->satellite_detail[i];
        }
    }
    if(!slot) {
        return;
    }
    snprintf(slot->talker, sizeof(slot->talker), "%.2s", talker);
    slot->prn = prn;
    slot->elevation = elevation >= 0 ? elevation : 0;
    slot->azimuth = azimuth >= 0 ? azimuth : 0;
    slot->cn0 = cn0 >= 0 ? cn0 : 0;
    slot->valid = 1;
    nrf9151_manager_recount_satellite_details(status);
}

void k230_nrf9151_status_init(k230_nrf9151_status_t *status)
{
    if(!status) {
        return;
    }
    memset(status, 0, sizeof(*status));
    snprintf(status->modem_state, sizeof(status->modem_state), "%s", "missing");
    snprintf(status->gps_state, sizeof(status->gps_state), "%s", "off");
    snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s", "off");
    snprintf(status->imei, sizeof(status->imei), "%s", "--");
    snprintf(status->firmware, sizeof(status->firmware), "%s", "--");
    snprintf(status->sim_status, sizeof(status->sim_status), "%s", "--");
    snprintf(status->operator_name, sizeof(status->operator_name), "%s", "--");
    snprintf(status->ip, sizeof(status->ip), "%s", "--");
    snprintf(status->apn, sizeof(status->apn), "%s", "--");
    snprintf(status->signal_text, sizeof(status->signal_text), "%s", "--");
    snprintf(status->lte_status, sizeof(status->lte_status), "%s", "Not tested");
    snprintf(status->gnss_status, sizeof(status->gnss_status), "%s", "Off");
    snprintf(status->last_error, sizeof(status->last_error), "%s", "-");
}

static void nrf9151_manager_status_merge_locked(
    const k230_nrf9151_status_t *status)
{
    if(!status) {
        return;
    }
    nrf9151_status_cache = *status;
    nrf9151_status_cache_loaded = 1;
}

static void nrf9151_manager_status_snapshot(k230_nrf9151_status_t *status)
{
    if(!status) {
        return;
    }
    pthread_mutex_lock(&nrf9151_manager_lock);
    if(!nrf9151_status_cache_loaded) {
        k230_nrf9151_status_init(&nrf9151_status_cache);
        nrf9151_status_cache_loaded = 1;
    }
    *status = nrf9151_status_cache;
    pthread_mutex_unlock(&nrf9151_manager_lock);
}

static void nrf9151_manager_status_set_error_locked(const char *error)
{
    if(!nrf9151_status_cache_loaded) {
        k230_nrf9151_status_init(&nrf9151_status_cache);
        nrf9151_status_cache_loaded = 1;
    }
    snprintf(nrf9151_status_cache.last_error,
             sizeof(nrf9151_status_cache.last_error), "%s",
             error && error[0] ? error : "-");
    nrf9151_status_cache.epoch = time(NULL);
}

static void nrf9151_manager_status_write_key(FILE *fp, const char *key,
                                             const char *value)
{
    fprintf(fp, "%s=%s\n", key, value && value[0] ? value : "");
}

int k230_nrf9151_write_status(const k230_nrf9151_status_t *status)
{
    FILE *fp;
    k230_nrf9151_status_t copy;

    if(!status) {
        errno = EINVAL;
        return -1;
    }
    copy = *status;
    if(copy.epoch <= 0) {
        copy.epoch = time(NULL);
    }
    fp = fopen(K230_NRF9151_STATUS_CACHE_TMP, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "version=1\n");
    fprintf(fp, "epoch=%ld\n", (long)copy.epoch);
    fprintf(fp, "present=%d\n", copy.present);
    fprintf(fp, "link_ok=%d\n", copy.link_ok);
    fprintf(fp, "sim_ready=%d\n", copy.sim_ready);
    fprintf(fp, "lte_registered=%d\n", copy.lte_registered);
    fprintf(fp, "packet_attached=%d\n", copy.packet_attached);
    fprintf(fp, "pdp_active=%d\n", copy.pdp_active);
    fprintf(fp, "gnss_running=%d\n", copy.gnss_running);
    fprintf(fp, "gnss_has_fix=%d\n", copy.gnss_has_fix);
    fprintf(fp, "lte_signal_level=%d\n", copy.lte_signal_level);
    fprintf(fp, "nmea_rx_count=%u\n", copy.nmea_rx_count);
    fprintf(fp, "nmea_valid_count=%u\n", copy.nmea_valid_count);
    fprintf(fp, "nmea_nofix_count=%u\n", copy.nmea_nofix_count);
    fprintf(fp, "satellites=%u\n", copy.satellites);
    fprintf(fp, "sat_detail_count=%u\n", copy.satellite_detail_count);
    for(size_t i = 0, out = 0; i < K230_NRF9151_MAX_GNSS_SATS; i++) {
        const k230_nrf9151_satellite_t *sat = &copy.satellite_detail[i];

        if(!sat->valid) {
            continue;
        }
        fprintf(fp, "sat_detail%zu=%s,%d,%d,%d,%d\n", out++,
                sat->talker[0] ? sat->talker : "--", sat->prn,
                sat->elevation, sat->azimuth, sat->cn0);
    }
    fprintf(fp, "ttff_ms=%lu\n", copy.ttff_ms);
    fprintf(fp, "last_nmea_ms=%lu\n", copy.last_nmea_ms);
    fprintf(fp, "lat=%.7f\n", copy.latitude);
    fprintf(fp, "lon=%.7f\n", copy.longitude);
    fprintf(fp, "alt=%.2f\n", copy.altitude_m);
    fprintf(fp, "has_alt=%d\n", copy.has_altitude);
    nrf9151_manager_status_write_key(fp, "modem_state", copy.modem_state);
    nrf9151_manager_status_write_key(fp, "gps_state", copy.gps_state);
    nrf9151_manager_status_write_key(fp, "gnss_phase", copy.gnss_phase);
    nrf9151_manager_status_write_key(fp, "imei", copy.imei);
    nrf9151_manager_status_write_key(fp, "firmware", copy.firmware);
    nrf9151_manager_status_write_key(fp, "sim_status", copy.sim_status);
    nrf9151_manager_status_write_key(fp, "operator", copy.operator_name);
    nrf9151_manager_status_write_key(fp, "ip", copy.ip);
    nrf9151_manager_status_write_key(fp, "apn", copy.apn);
    nrf9151_manager_status_write_key(fp, "signal_text", copy.signal_text);
    nrf9151_manager_status_write_key(fp, "lte_status", copy.lte_status);
    nrf9151_manager_status_write_key(fp, "gnss_status", copy.gnss_status);
    nrf9151_manager_status_write_key(fp, "last_error", copy.last_error);
    if(fclose(fp) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_STATUS_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }
    if(rename(K230_NRF9151_STATUS_CACHE_TMP,
              K230_NRF9151_STATUS_CACHE) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_STATUS_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }

    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_manager_status_merge_locked(&copy);
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return 0;
}

static void nrf9151_manager_status_set_string(char *dst, size_t dst_len,
                                              const char *value)
{
    if(dst && dst_len > 0U) {
        snprintf(dst, dst_len, "%s", value ? value : "");
    }
}

int k230_nrf9151_read_status(k230_nrf9151_status_t *status,
                             int max_age_seconds)
{
    struct stat st;
    FILE *fp;
    char line[384];
    time_t now;

    if(!status) {
        errno = EINVAL;
        return -1;
    }
    k230_nrf9151_status_init(status);
    if(stat(K230_NRF9151_STATUS_CACHE, &st) != 0) {
        nrf9151_manager_status_snapshot(status);
        if(status->epoch > 0) {
            return 0;
        }
        return -1;
    }
    fp = fopen(K230_NRF9151_STATUS_CACHE, "r");
    if(!fp) {
        return -1;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *eq;

        nrf9151_manager_trim_text(line);
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq++ = '\0';
        if(strcmp(line, "epoch") == 0) {
            status->epoch = (time_t)strtol(eq, NULL, 10);
        } else if(strcmp(line, "present") == 0) {
            status->present = atoi(eq);
        } else if(strcmp(line, "link_ok") == 0) {
            status->link_ok = atoi(eq);
        } else if(strcmp(line, "sim_ready") == 0) {
            status->sim_ready = atoi(eq);
        } else if(strcmp(line, "lte_registered") == 0) {
            status->lte_registered = atoi(eq);
        } else if(strcmp(line, "packet_attached") == 0) {
            status->packet_attached = atoi(eq);
        } else if(strcmp(line, "pdp_active") == 0) {
            status->pdp_active = atoi(eq);
        } else if(strcmp(line, "gnss_running") == 0) {
            status->gnss_running = atoi(eq);
        } else if(strcmp(line, "gnss_has_fix") == 0) {
            status->gnss_has_fix = atoi(eq);
        } else if(strcmp(line, "lte_signal_level") == 0) {
            status->lte_signal_level = atoi(eq);
        } else if(strcmp(line, "nmea_rx_count") == 0) {
            status->nmea_rx_count = (unsigned int)strtoul(eq, NULL, 10);
        } else if(strcmp(line, "nmea_valid_count") == 0) {
            status->nmea_valid_count = (unsigned int)strtoul(eq, NULL, 10);
        } else if(strcmp(line, "nmea_nofix_count") == 0) {
            status->nmea_nofix_count = (unsigned int)strtoul(eq, NULL, 10);
        } else if(strcmp(line, "satellites") == 0) {
            status->satellites = (unsigned int)strtoul(eq, NULL, 10);
        } else if(strcmp(line, "sat_detail_count") == 0) {
            status->satellite_detail_count =
                (unsigned int)strtoul(eq, NULL, 10);
        } else if(strncmp(line, "sat_detail", 10) == 0 &&
                  isdigit((unsigned char)line[10])) {
            char talker[8] = "";
            int prn = 0;
            int elevation = 0;
            int azimuth = 0;
            int cn0 = 0;

            if(sscanf(eq, "%7[^,],%d,%d,%d,%d", talker, &prn, &elevation,
                      &azimuth, &cn0) == 5) {
                nrf9151_manager_upsert_satellite_detail(status, talker, prn,
                                                        elevation, azimuth,
                                                        cn0);
            }
        } else if(strcmp(line, "ttff_ms") == 0) {
            status->ttff_ms = strtoul(eq, NULL, 10);
        } else if(strcmp(line, "last_nmea_ms") == 0) {
            status->last_nmea_ms = strtoul(eq, NULL, 10);
        } else if(strcmp(line, "lat") == 0) {
            status->latitude = strtod(eq, NULL);
        } else if(strcmp(line, "lon") == 0) {
            status->longitude = strtod(eq, NULL);
        } else if(strcmp(line, "alt") == 0) {
            status->altitude_m = strtod(eq, NULL);
        } else if(strcmp(line, "has_alt") == 0) {
            status->has_altitude = atoi(eq);
        } else if(strcmp(line, "modem_state") == 0) {
            nrf9151_manager_status_set_string(status->modem_state,
                                              sizeof(status->modem_state), eq);
        } else if(strcmp(line, "gps_state") == 0) {
            nrf9151_manager_status_set_string(status->gps_state,
                                              sizeof(status->gps_state), eq);
        } else if(strcmp(line, "gnss_phase") == 0) {
            nrf9151_manager_status_set_string(status->gnss_phase,
                                              sizeof(status->gnss_phase), eq);
        } else if(strcmp(line, "imei") == 0) {
            nrf9151_manager_status_set_string(status->imei,
                                              sizeof(status->imei), eq);
        } else if(strcmp(line, "firmware") == 0) {
            nrf9151_manager_status_set_string(status->firmware,
                                              sizeof(status->firmware), eq);
        } else if(strcmp(line, "sim_status") == 0) {
            nrf9151_manager_status_set_string(status->sim_status,
                                              sizeof(status->sim_status), eq);
        } else if(strcmp(line, "operator") == 0) {
            nrf9151_manager_status_set_string(status->operator_name,
                                              sizeof(status->operator_name), eq);
        } else if(strcmp(line, "ip") == 0) {
            nrf9151_manager_status_set_string(status->ip,
                                              sizeof(status->ip), eq);
        } else if(strcmp(line, "apn") == 0) {
            nrf9151_manager_status_set_string(status->apn,
                                              sizeof(status->apn), eq);
        } else if(strcmp(line, "signal_text") == 0) {
            nrf9151_manager_status_set_string(status->signal_text,
                                              sizeof(status->signal_text), eq);
        } else if(strcmp(line, "lte_status") == 0) {
            nrf9151_manager_status_set_string(status->lte_status,
                                              sizeof(status->lte_status), eq);
        } else if(strcmp(line, "gnss_status") == 0) {
            nrf9151_manager_status_set_string(status->gnss_status,
                                              sizeof(status->gnss_status), eq);
        } else if(strcmp(line, "last_error") == 0) {
            nrf9151_manager_status_set_string(status->last_error,
                                              sizeof(status->last_error), eq);
        }
    }
    fclose(fp);
    if(status->epoch <= 0) {
        status->epoch = st.st_mtime;
    }
    now = time(NULL);
    if(now > 0 && status->epoch > 0 && now >= status->epoch) {
        status->age_seconds = (long)(now - status->epoch);
    }
    if(max_age_seconds > 0 && status->age_seconds > max_age_seconds) {
        return -1;
    }
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_manager_status_merge_locked(status);
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return 0;
}

static int nrf9151_manager_configure_uart3_iomux(void)
{
    int fd;
    void *map;
    volatile uint32_t *regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if(fd < 0) {
        return -1;
    }
    map = mmap(NULL, K230_NRF9151_IOMUX_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, K230_NRF9151_IOMUX_BASE);
    if(map == MAP_FAILED) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    regs = (volatile uint32_t *)map;
    regs[K230_NRF9151_IOMUX_IO2_OFFSET / 4U] =
        K230_NRF9151_IOMUX_FUNC_ALT0 | K230_NRF9151_IOMUX_OE_BIT |
        K230_NRF9151_IOMUX_DS_8MA;
    regs[K230_NRF9151_IOMUX_IO28_OFFSET / 4U] =
        K230_NRF9151_IOMUX_FUNC_ALT2 | K230_NRF9151_IOMUX_IE_BIT |
        K230_NRF9151_IOMUX_OE_BIT | K230_NRF9151_IOMUX_ST_BIT |
        K230_NRF9151_IOMUX_DS_8MA;
    regs[K230_NRF9151_IOMUX_IO29_OFFSET / 4U] =
        K230_NRF9151_IOMUX_FUNC_ALT2 | K230_NRF9151_IOMUX_IE_BIT |
        K230_NRF9151_IOMUX_ST_BIT | K230_NRF9151_IOMUX_DS_8MA;
    regs[K230_NRF9151_IOMUX_IO50_OFFSET / 4U] =
        K230_NRF9151_IOMUX_FUNC_ALT0 | K230_NRF9151_IOMUX_DS_8MA;
    regs[K230_NRF9151_IOMUX_IO51_OFFSET / 4U] =
        K230_NRF9151_IOMUX_FUNC_ALT0 | K230_NRF9151_IOMUX_DS_8MA;
    munmap(map, K230_NRF9151_IOMUX_SIZE);
    close(fd);
    return 0;
}

static int nrf9151_manager_set_en_gpio(int enabled)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    int value = enabled ? 1 : 0;
    int rc = -1;

    pthread_mutex_lock(&nrf9151_gpio_lock);
    if(!nrf9151_gpio_chip) {
        nrf9151_gpio_chip = gpiod_chip_open("/dev/gpiochip0");
        if(!nrf9151_gpio_chip) {
            goto out;
        }
    }
    if(!nrf9151_en_gpio_request) {
        settings = gpiod_line_settings_new();
        line_config = gpiod_line_config_new();
        request_config = gpiod_request_config_new();
        if(!settings || !line_config || !request_config) {
            goto out;
        }
        gpiod_line_settings_set_direction(settings,
                                          GPIOD_LINE_DIRECTION_OUTPUT);
        gpiod_line_settings_set_output_value(settings,
                                             enabled ?
                                             GPIOD_LINE_VALUE_ACTIVE :
                                             GPIOD_LINE_VALUE_INACTIVE);
        nrf9151_en_gpio_offset = K230_NRF9151_EN_GPIO;
        if(gpiod_line_config_add_line_settings(line_config,
                                               &nrf9151_en_gpio_offset, 1,
                                               settings) != 0) {
            goto out;
        }
        gpiod_request_config_set_consumer(request_config,
                                         "k230-nrf9151-manager");
        nrf9151_en_gpio_request =
            gpiod_chip_request_lines(nrf9151_gpio_chip, request_config,
                                     line_config);
        if(!nrf9151_en_gpio_request) {
            goto out;
        }
    }
    rc = gpiod_line_request_set_value(nrf9151_en_gpio_request,
                                      nrf9151_en_gpio_offset,
                                      enabled ? GPIOD_LINE_VALUE_ACTIVE :
                                                GPIOD_LINE_VALUE_INACTIVE);
out:
    if(settings) {
        gpiod_line_settings_free(settings);
    }
    if(line_config) {
        gpiod_line_config_free(line_config);
    }
    if(request_config) {
        gpiod_request_config_free(request_config);
    }
    pthread_mutex_unlock(&nrf9151_gpio_lock);
    return rc;
}

static int nrf9151_manager_prepare_hw(void)
{
    int rc = 0;

    if(nrf9151_manager_configure_uart3_iomux() != 0) {
        rc = -1;
    }
    if(nrf9151_manager_set_en_gpio(1) != 0) {
        rc = -1;
    }
    usleep(600000);
    return rc;
}

static int nrf9151_manager_open_uart(void)
{
    int fd = open(K230_NRF9151_UART_DEV,
                  O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    struct termios tio;

    if(fd < 0) {
        return -1;
    }
    if(tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, K230_NRF9151_UART_BAUD);
    cfsetospeed(&tio, K230_NRF9151_UART_BAUD);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if(tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int nrf9151_manager_response_has_token(const char *resp,
                                              const char *token)
{
    const char *p = resp;
    size_t token_len;

    if(!resp || !token) {
        return 0;
    }
    token_len = strlen(token);
    while(*p) {
        while(*p == '\r' || *p == '\n' || isspace((unsigned char)*p)) {
            p++;
        }
        if(strncmp(p, token, token_len) == 0 &&
           (p[token_len] == '\0' || p[token_len] == '\r' ||
            p[token_len] == '\n' || isspace((unsigned char)p[token_len]))) {
            return 1;
        }
        while(*p && *p != '\r' && *p != '\n') {
            p++;
        }
    }
    return 0;
}

static void nrf9151_manager_drain_uart(int fd, uint64_t quiet_us,
                                       uint64_t max_us)
{
    uint64_t start;
    uint64_t last_data;

    if(fd < 0) {
        return;
    }
    start = nrf9151_manager_monotonic_us();
    last_data = start;
    for(;;) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        uint64_t now = nrf9151_manager_monotonic_us();
        int rc;

        if(now - start >= max_us || now - last_data >= quiet_us) {
            break;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rc <= 0 || !FD_ISSET(fd, &rfds)) {
            continue;
        }
        if(read(fd, buf, sizeof(buf)) > 0) {
            last_data = nrf9151_manager_monotonic_us();
        }
    }
}

static int nrf9151_manager_http_stat_complete(const char *text)
{
    const char *p;
    const char *first_comma;
    const char *second_comma;
    char *endp;

    if(!text) {
        return 0;
    }
    p = strstr(text, "#XHTTPCSTAT:");
    if(!p) {
        return 0;
    }
    first_comma = strchr(p, ',');
    if(!first_comma) {
        return 0;
    }
    second_comma = strchr(first_comma + 1, ',');
    if(!second_comma) {
        return 0;
    }
    (void)strtol(first_comma + 1, &endp, 10);
    return endp && endp > first_comma + 1 && endp <= second_comma;
}

static int nrf9151_manager_urc_done_seen(const char *text)
{
    static const char *const tokens[] = {
        "#XMQTTEVT: 0,",
        "#XMQTTEVT: 1,",
        "#XMQTTEVT: 2,",
        "#XMQTTEVT: 3,",
        "#XMQTTEVT: 7,",
        "#XMQTTMSG:",
        "+CME ERROR",
        "+CMS ERROR",
        "ERROR",
    };

    if(!text) {
        return 0;
    }
    if(nrf9151_manager_http_stat_complete(text)) {
        return 1;
    }
    for(size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        if(strstr(text, tokens[i])) {
            return 1;
        }
    }
    return 0;
}

static int nrf9151_manager_exchange(int fd, const char *cmd, char *resp,
                                    size_t resp_len, uint64_t timeout_us,
                                    k230_nrf9151_cancel_cb_t cancel_cb,
                                    void *cancel_user)
{
    uint64_t start;
    size_t used = 0;
    size_t cmd_len;
    char scratch[512];
    char *scan_resp = resp && resp_len > 0U ? resp : scratch;
    size_t scan_len = resp && resp_len > 0U ? resp_len : sizeof(scratch);

    if(scan_len > 0U) {
        scan_resp[0] = '\0';
    }
    if(fd < 0 || !cmd) {
        errno = EINVAL;
        return -1;
    }
    nrf9151_manager_drain_uart(fd, 25000ULL, 120000ULL);
    cmd_len = strlen(cmd);
    if(write(fd, cmd, cmd_len) < 0 || write(fd, "\r\n", 2) < 0) {
        return -1;
    }
    start = nrf9151_manager_monotonic_us();
    while(nrf9151_manager_monotonic_us() - start < timeout_us) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;
        int rc;

        if(cancel_cb && cancel_cb(cancel_user)) {
            errno = ECANCELED;
            return -2;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 80000;
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(rc == 0 || !FD_ISSET(fd, &rfds)) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf));
        if(rd < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(rd == 0) {
            continue;
        }
        if(scan_len > 0U && used + 1U < scan_len) {
            size_t copy = (size_t)rd;

            if(copy > scan_len - used - 1U) {
                copy = scan_len - used - 1U;
            }
            memcpy(scan_resp + used, buf, copy);
            used += copy;
            scan_resp[used] = '\0';
        }
        if(nrf9151_manager_response_has_token(scan_resp, "OK") ||
           nrf9151_manager_response_has_token(scan_resp, "ERROR")) {
            return nrf9151_manager_response_has_token(scan_resp, "OK") ? 0 : 1;
        }
    }
    return -1;
}

static int nrf9151_manager_read_urc(int fd, char *resp, size_t resp_len,
                                    uint64_t timeout_us,
                                    k230_nrf9151_cancel_cb_t cancel_cb,
                                    void *cancel_user)
{
    uint64_t start = nrf9151_manager_monotonic_us();
    size_t used = resp && resp_len > 0U ? strlen(resp) : 0U;
    char tail[192] = "";

    while(nrf9151_manager_monotonic_us() - start < timeout_us) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;
        int rc;

        if(cancel_cb && cancel_cb(cancel_user)) {
            errno = ECANCELED;
            return -2;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 150000;
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rc <= 0 || !FD_ISSET(fd, &rfds)) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf));
        if(rd <= 0) {
            continue;
        }
        {
            char scan[sizeof(tail) + sizeof(buf) + 1U];
            char chunk[sizeof(buf) + 1U];
            size_t chunk_len = (size_t)rd;
            size_t scan_len;

            if(chunk_len >= sizeof(chunk)) {
                chunk_len = sizeof(chunk) - 1U;
            }
            memcpy(chunk, buf, chunk_len);
            chunk[chunk_len] = '\0';
            snprintf(scan, sizeof(scan), "%s%s", tail, chunk);
            scan_len = strlen(scan);
            if(scan_len >= sizeof(tail)) {
                memcpy(tail, scan + scan_len - (sizeof(tail) - 1U),
                       sizeof(tail) - 1U);
                tail[sizeof(tail) - 1U] = '\0';
            } else {
                snprintf(tail, sizeof(tail), "%s", scan);
            }
            if(nrf9151_manager_urc_done_seen(scan)) {
                if(resp && resp_len > 0U && used + 1U < resp_len) {
                    size_t copy = (size_t)rd;

                    if(copy > resp_len - used - 1U) {
                        copy = resp_len - used - 1U;
                    }
                    memcpy(resp + used, buf, copy);
                    used += copy;
                    resp[used] = '\0';
                }
                return 1;
            }
        }
        if(resp && resp_len > 0U && used + 1U < resp_len) {
            size_t copy = (size_t)rd;

            if(copy > resp_len - used - 1U) {
                copy = resp_len - used - 1U;
            }
            memcpy(resp + used, buf, copy);
            used += copy;
            resp[used] = '\0';
        }
        if(nrf9151_manager_urc_done_seen(resp)) {
            return 1;
        }
    }
    return 0;
}

static int nrf9151_manager_read_ping_result(int fd, char *resp,
                                            size_t resp_len,
                                            uint64_t timeout_us,
                                            k230_nrf9151_cancel_cb_t cancel_cb,
                                            void *cancel_user)
{
    uint64_t start = nrf9151_manager_monotonic_us();
    size_t used = 0U;

    if(resp && resp_len > 0U) {
        resp[0] = '\0';
    }
    while(nrf9151_manager_monotonic_us() - start < timeout_us) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;
        int rc;

        if(cancel_cb && cancel_cb(cancel_user)) {
            errno = ECANCELED;
            return -2;
        }
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 150000;
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rc <= 0 || !FD_ISSET(fd, &rfds)) {
            continue;
        }
        rd = read(fd, buf, sizeof(buf));
        if(rd <= 0) {
            continue;
        }
        if(resp && resp_len > 0U && used + 1U < resp_len) {
            size_t copy = (size_t)rd;

            if(copy > resp_len - used - 1U) {
                copy = resp_len - used - 1U;
            }
            memcpy(resp + used, buf, copy);
            used += copy;
            resp[used] = '\0';
        }
        if(resp && (strstr(resp, "#XPING: average") ||
                    strstr(resp, "ERROR"))) {
            return 1;
        }
    }
    return 0;
}

static int nrf9151_manager_open_session(const char *owner, int wait_ms,
                                        int *lock_fd_out, int *fd_out,
                                        char *log, size_t log_len)
{
    int lock_fd;
    int fd;

    if(lock_fd_out) {
        *lock_fd_out = -1;
    }
    if(fd_out) {
        *fd_out = -1;
    }
    if(!lock_fd_out || !fd_out) {
        errno = EINVAL;
        return -1;
    }

    lock_fd = k230_nrf9151_acquire_uart(owner, wait_ms);
    if(lock_fd < 0) {
        nrf9151_manager_log_append(log, log_len, "UART busy: %s",
                                   strerror(errno));
        return -1;
    }
    if(nrf9151_manager_prepare_hw() != 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "hardware prepare warning: %s",
                                   strerror(errno));
    }
    fd = nrf9151_manager_open_uart();
    if(fd < 0) {
        nrf9151_manager_log_append(log, log_len, "open %s failed: %s",
                                   K230_NRF9151_UART_DEV, strerror(errno));
        k230_nrf9151_release_uart(lock_fd);
        return -1;
    }
    *lock_fd_out = lock_fd;
    *fd_out = fd;
    return 0;
}

static void nrf9151_manager_close_session(int lock_fd, int fd)
{
    if(fd >= 0) {
        close(fd);
    }
    k230_nrf9151_release_uart(lock_fd);
}

const char *k230_nrf9151_uart_dev(void)
{
    return K230_NRF9151_UART_DEV;
}

const char *k230_nrf9151_gnss_cache_path(void)
{
    return K230_NRF9151_GNSS_FIX_CACHE;
}

const char *k230_nrf9151_gnss_cache_tmp_path(void)
{
    return K230_NRF9151_GNSS_FIX_CACHE_TMP;
}

const char *k230_nrf9151_status_cache_path(void)
{
    return K230_NRF9151_STATUS_CACHE;
}

int k230_nrf9151_uart_present(void)
{
    return access(K230_NRF9151_UART_DEV, R_OK | W_OK) == 0;
}

int k230_nrf9151_write_gnss_fix(double latitude, double longitude,
                                int has_altitude, double altitude_m,
                                int satellites, const char *source)
{
    FILE *fp;

    if(!isfinite(latitude) || !isfinite(longitude) ||
       latitude < -90.0 || latitude > 90.0 ||
       longitude < -180.0 || longitude > 180.0 ||
       (fabs(latitude) < 0.000001 && fabs(longitude) < 0.000001)) {
        errno = EINVAL;
        return -1;
    }

    fp = fopen(K230_NRF9151_GNSS_FIX_CACHE_TMP, "w");
    if(!fp) {
        return -1;
    }
    fprintf(fp, "version=1\n");
    fprintf(fp, "epoch=%ld\n", (long)time(NULL));
    fprintf(fp, "lat=%.7f\n", latitude);
    fprintf(fp, "lon=%.7f\n", longitude);
    fprintf(fp, "has_alt=%d\n", has_altitude ? 1 : 0);
    fprintf(fp, "alt=%.2f\n", has_altitude ? altitude_m : 0.0);
    fprintf(fp, "sats=%d\n", satellites > 0 ? satellites : 0);
    fprintf(fp, "source=%s\n", source && source[0] ? source : "nrf9151");
    if(fclose(fp) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_GNSS_FIX_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }
    if(rename(K230_NRF9151_GNSS_FIX_CACHE_TMP,
              K230_NRF9151_GNSS_FIX_CACHE) != 0) {
        int saved_errno = errno;

        unlink(K230_NRF9151_GNSS_FIX_CACHE_TMP);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int k230_nrf9151_read_gnss_fix(k230_nrf9151_gnss_fix_t *fix,
                               int max_age_seconds)
{
    struct stat st;
    FILE *fp;
    char line[192];
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    int has_alt = 0;
    int sats = 0;
    time_t epoch = 0;
    time_t now;
    char source[48] = "cache";

    if(!fix) {
        return -1;
    }
    memset(fix, 0, sizeof(*fix));
    if(stat(K230_NRF9151_GNSS_FIX_CACHE, &st) != 0) {
        return -1;
    }

    fp = fopen(K230_NRF9151_GNSS_FIX_CACHE, "r");
    if(!fp) {
        return -1;
    }
    while(fgets(line, sizeof(line), fp)) {
        char *eq;

        nrf9151_manager_trim_text(line);
        eq = strchr(line, '=');
        if(!eq) {
            continue;
        }
        *eq++ = '\0';
        if(strcmp(line, "epoch") == 0) {
            epoch = (time_t)strtol(eq, NULL, 10);
        } else if(strcmp(line, "lat") == 0) {
            lat = strtod(eq, NULL);
        } else if(strcmp(line, "lon") == 0) {
            lon = strtod(eq, NULL);
        } else if(strcmp(line, "has_alt") == 0) {
            has_alt = atoi(eq) != 0;
        } else if(strcmp(line, "alt") == 0) {
            alt = strtod(eq, NULL);
        } else if(strcmp(line, "sats") == 0) {
            sats = atoi(eq);
        } else if(strcmp(line, "source") == 0) {
            snprintf(source, sizeof(source), "%s", eq);
        }
    }
    fclose(fp);

    if(epoch <= 0) {
        epoch = st.st_mtime;
    }
    now = time(NULL);
    if(now > 0 && epoch > 0 && now >= epoch) {
        fix->age_seconds = (long)(now - epoch);
    } else {
        fix->age_seconds = 0;
    }
    if(max_age_seconds > 0 && fix->age_seconds > max_age_seconds) {
        return -1;
    }
    if(!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 ||
       lon < -180.0 || lon > 180.0 ||
       (fabs(lat) < 0.000001 && fabs(lon) < 0.000001)) {
        return -1;
    }

    fix->valid = 1;
    fix->latitude = lat;
    fix->longitude = lon;
    fix->altitude_m = alt;
    fix->has_altitude = has_alt;
    fix->satellites = sats;
    fix->epoch = epoch;
    snprintf(fix->source, sizeof(fix->source), "%s", source);
    return 0;
}

int k230_nrf9151_acquire_uart(const char *owner, int wait_ms)
{
    int fd;
    uint64_t start_us;
    int waited_ms = 0;

    fd = open(K230_NRF9151_UART_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if(fd < 0) {
        return -1;
    }
    start_us = nrf9151_manager_monotonic_us();
    for(;;) {
        if(flock(fd, LOCK_EX | LOCK_NB) == 0) {
            if(ftruncate(fd, 0) != 0) {
                flock(fd, LOCK_UN);
                close(fd);
                return -1;
            }
            dprintf(fd, "%s\n", owner && owner[0] ? owner : "unknown");
            return fd;
        }
        if(errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd);
            return -1;
        }
        if(wait_ms <= 0 || waited_ms >= wait_ms) {
            close(fd);
            errno = EBUSY;
            return -1;
        }
        usleep(20000);
        waited_ms = (int)((nrf9151_manager_monotonic_us() - start_us) /
                          1000ULL);
    }
}

void k230_nrf9151_release_uart(int lock_fd)
{
    if(lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

static int nrf9151_manager_line_containing(const char *resp,
                                           const char *needle,
                                           char *out, size_t out_len)
{
    const char *p = resp;
    size_t used = 0;

    if(!resp || !needle || !out || out_len == 0U) {
        return -1;
    }
    while(*p) {
        char line[256];

        used = 0;
        while(*p && *p != '\n' && used + 1U < sizeof(line)) {
            if(*p != '\r') {
                line[used++] = *p;
            }
            p++;
        }
        while(*p == '\r' || *p == '\n') {
            p++;
        }
        line[used] = '\0';
        nrf9151_manager_trim_text(line);
        if(strstr(line, needle)) {
            snprintf(out, out_len, "%s", line);
            return 0;
        }
    }
    return -1;
}

static int nrf9151_manager_numeric_response(const char *resp, char *out,
                                            size_t out_len)
{
    const char *p = resp;

    if(!resp || !out || out_len == 0U) {
        return -1;
    }
    while(*p) {
        char line[128];
        size_t used = 0;
        int digits = 0;

        while(*p && *p != '\n' && used + 1U < sizeof(line)) {
            if(*p != '\r') {
                line[used++] = *p;
            }
            p++;
        }
        while(*p == '\r' || *p == '\n') {
            p++;
        }
        line[used] = '\0';
        nrf9151_manager_trim_text(line);
        if(!line[0] || nrf9151_manager_starts_with(line, "AT") ||
           strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0) {
            continue;
        }
        for(size_t i = 0; line[i]; i++) {
            if(!isdigit((unsigned char)line[i])) {
                digits = -1000;
                break;
            }
            digits++;
        }
        if(digits >= 6) {
            snprintf(out, out_len, "%s", line);
            return 0;
        }
    }
    return -1;
}

static void nrf9151_manager_extract_quoted(const char *line, char *out,
                                           size_t out_len)
{
    const char *first;
    const char *second;
    size_t copy;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(!line) {
        return;
    }
    first = strchr(line, '"');
    if(!first) {
        return;
    }
    second = strchr(first + 1, '"');
    if(!second) {
        return;
    }
    copy = (size_t)(second - first - 1);
    if(copy >= out_len) {
        copy = out_len - 1U;
    }
    memcpy(out, first + 1, copy);
    out[copy] = '\0';
}

static void nrf9151_manager_extract_quoted_index(const char *line, int index,
                                                 char *out, size_t out_len)
{
    const char *p = line;
    const char *start;
    const char *end;
    int current = 0;
    size_t copy;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(!line || index < 0) {
        return;
    }
    while((start = strchr(p, '"')) != NULL) {
        start++;
        end = strchr(start, '"');
        if(!end) {
            return;
        }
        if(current == index) {
            copy = (size_t)(end - start);
            if(copy >= out_len) {
                copy = out_len - 1U;
            }
            memcpy(out, start, copy);
            out[copy] = '\0';
            return;
        }
        current++;
        p = end + 1;
    }
}

static int nrf9151_manager_parse_cereg_stat(const char *line)
{
    const char *p;
    int values[4] = { -1, -1, -1, -1 };
    int count = 0;

    if(!line) {
        return -1;
    }
    p = strchr(line, ':');
    if(!p) {
        return -1;
    }
    p++;
    while(*p && count < 4) {
        while(*p && !isdigit((unsigned char)*p) && *p != '-') {
            p++;
        }
        if(!*p) {
            break;
        }
        values[count++] = (int)strtol(p, (char **)&p, 10);
    }
    if(count >= 2) {
        return values[1];
    }
    if(count == 1) {
        return values[0];
    }
    return -1;
}

static int nrf9151_manager_run_cmd_logged(int fd, const char *cmd,
                                          uint64_t timeout_us,
                                          k230_nrf9151_status_t *status,
                                          char *log, size_t log_len,
                                          k230_nrf9151_cancel_cb_t cancel_cb,
                                          void *cancel_user);

static void nrf9151_manager_quiesce_gnss_output(int fd, char *log,
                                                size_t log_len)
{
    if(fd < 0) {
        return;
    }
    nrf9151_manager_drain_uart(fd, 80000ULL, 600000ULL);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT#XNMEA=0",
                                         K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                         log, log_len, NULL, NULL);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT#XGNSS=0",
                                         K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                         log, log_len, NULL, NULL);
    nrf9151_manager_drain_uart(fd, 100000ULL, 900000ULL);
}

static int nrf9151_manager_signal_level_from_cesq(const char *line)
{
    const char *p;
    int values[6] = { 255, 255, 255, 255, 255, 255 };
    int count = 0;
    int rsrp;

    if(!line) {
        return 0;
    }
    p = strchr(line, ':');
    if(!p) {
        return 0;
    }
    p++;
    while(*p && count < 6) {
        while(*p && !isdigit((unsigned char)*p) && *p != '-') {
            p++;
        }
        if(!*p) {
            break;
        }
        values[count++] = (int)strtol(p, (char **)&p, 10);
    }
    if(count < 6) {
        return 0;
    }
    rsrp = values[5];
    if(rsrp == 255 || rsrp <= 0) {
        return 0;
    }
    if(rsrp < 35) {
        return 1;
    }
    if(rsrp < 50) {
        return 2;
    }
    if(rsrp < 65) {
        return 3;
    }
    return 4;
}

static void nrf9151_manager_update_lte_from_response(
    k230_nrf9151_status_t *status, const char *cmd, const char *resp, int rc)
{
    char line[256];

    if(!status || !cmd) {
        return;
    }
    status->present = 1;
    if(strcmp(cmd, "AT") == 0 && rc == 0) {
        status->link_ok = 1;
        snprintf(status->modem_state, sizeof(status->modem_state), "%s",
                 "present");
    }
    if(strcmp(cmd, "AT+CGSN") == 0 && rc == 0 &&
       nrf9151_manager_numeric_response(resp, line, sizeof(line)) == 0) {
        snprintf(status->imei, sizeof(status->imei), "%s", line);
    }
    if(strcmp(cmd, "AT+CGMR") == 0 && rc == 0 &&
       nrf9151_manager_line_containing(resp, "mfw_", line, sizeof(line)) == 0) {
        snprintf(status->firmware, sizeof(status->firmware), "%s", line);
    }
    if(nrf9151_manager_line_containing(resp, "%XSIM:", line,
                                       sizeof(line)) == 0) {
        if(strstr(line, "%XSIM: 1")) {
            status->sim_ready = 1;
            snprintf(status->sim_status, sizeof(status->sim_status), "%s",
                     "OK");
        } else if(!status->sim_ready) {
            snprintf(status->sim_status, sizeof(status->sim_status), "%s",
                     "No SIM");
        }
    }
    if(nrf9151_manager_line_containing(resp, "+CPIN:", line,
                                       sizeof(line)) == 0) {
        if(strstr(line, "READY")) {
            status->sim_ready = 1;
            snprintf(status->sim_status, sizeof(status->sim_status), "%s",
                     "READY");
        } else if(!status->sim_ready) {
            snprintf(status->sim_status, sizeof(status->sim_status), "%s",
                     line);
        }
    }
    if(nrf9151_manager_line_containing(resp, "+CEREG:", line,
                                       sizeof(line)) == 0) {
        int stat = nrf9151_manager_parse_cereg_stat(line);

        status->lte_registered = (stat == 1 || stat == 5) ? 1 : 0;
        snprintf(status->lte_status, sizeof(status->lte_status), "%s",
                 status->lte_registered ? "Registered" : line);
    }
    if(nrf9151_manager_line_containing(resp, "+CESQ:", line,
                                       sizeof(line)) == 0) {
        int level = nrf9151_manager_signal_level_from_cesq(line);

        snprintf(status->signal_text, sizeof(status->signal_text),
                 "%s", line);
        if(level > 0) {
            status->lte_signal_level = level;
        }
    }
    if(nrf9151_manager_line_containing(resp, "+COPS:", line,
                                       sizeof(line)) == 0) {
        char oper[96];

        nrf9151_manager_extract_quoted(line, oper, sizeof(oper));
        if(oper[0]) {
            snprintf(status->operator_name, sizeof(status->operator_name),
                     "%s", oper);
        }
    }
    if(nrf9151_manager_line_containing(resp, "+CGATT:", line,
                                       sizeof(line)) == 0) {
        status->packet_attached = strstr(line, ": 1") || strstr(line, ":1");
    }
    if(nrf9151_manager_line_containing(resp, "+CGACT:", line,
                                       sizeof(line)) == 0) {
        if(strstr(line, ",1")) {
            status->pdp_active = 1;
        }
    }
    if(nrf9151_manager_line_containing(resp, "+CGPADDR:", line,
                                       sizeof(line)) == 0) {
        const char *comma = strchr(line, ',');

        if(comma && strchr(comma, '.')) {
            snprintf(status->ip, sizeof(status->ip), "%s", comma + 1);
            nrf9151_manager_trim_text(status->ip);
            status->pdp_active = 1;
        }
    }
    if(nrf9151_manager_line_containing(resp, "+CGDCONT:", line,
                                       sizeof(line)) == 0) {
        char apn[96];

        nrf9151_manager_extract_quoted_index(line, 1, apn, sizeof(apn));
        if(apn[0]) {
            snprintf(status->apn, sizeof(status->apn), "%s", apn);
        }
    }
    if(nrf9151_manager_line_containing(resp, "#XPING:", line,
                                       sizeof(line)) == 0) {
        if(strstr(line, "average")) {
            snprintf(status->lte_status, sizeof(status->lte_status), "%s",
                     "Ping OK");
        }
    }
    if(rc != 0) {
        snprintf(status->last_error, sizeof(status->last_error), "%s failed",
                 cmd);
    }
}

static int nrf9151_manager_run_cmd_logged(int fd, const char *cmd,
                                          uint64_t timeout_us,
                                          k230_nrf9151_status_t *status,
                                          char *log, size_t log_len,
                                          k230_nrf9151_cancel_cb_t cancel_cb,
                                          void *cancel_user)
{
    char resp[2048];
    int rc;

    nrf9151_manager_log_append(log, log_len, "> %s", cmd);
    rc = nrf9151_manager_exchange(fd, cmd, resp, sizeof(resp), timeout_us,
                                  cancel_cb, cancel_user);
    nrf9151_manager_trim_text(resp);
    if(resp[0]) {
        nrf9151_manager_log_append(log, log_len, "%s", resp);
    } else if(rc == -2) {
        nrf9151_manager_log_append(log, log_len, "< canceled");
    } else {
        nrf9151_manager_log_append(log, log_len, "< no response");
    }
    if(status) {
        nrf9151_manager_update_lte_from_response(status, cmd, resp, rc);
    }
    return rc;
}

static int nrf9151_manager_run_lte_check_common(
    k230_nrf9151_status_t *status, char *log, size_t log_len,
    k230_nrf9151_cancel_cb_t cancel_cb, void *cancel_user,
    int include_ping, int wait_ms, int write_open_error,
    int quiesce_gnss, const char *owner)
{
    static const char *const cmds[] = {
        "AT",
        "AT+CMEE=1",
        "AT+CGSN",
        "AT+CGMR",
        "AT+CFUN=1",
        "AT%XSIM=1",
        "AT%XSIM?",
        "AT+CPIN?",
        "AT%XICCID",
        "AT+CIMI",
        "AT+CEREG=5",
        "AT+CEREG?",
        "AT+CESQ",
        "AT%XMONITOR",
        "AT+COPS?",
        "AT+CGATT?",
        "AT+CGACT?",
        "AT+CGPADDR",
        "AT+CGDCONT?",
    };
    static const char *const ping_cmd =
        "AT#XPING=\"223.5.5.5\",32,5000,3,1000";
    k230_nrf9151_status_t local;
    int lock_fd = -1;
    int fd = -1;
    int failures = 0;
    int saved_errno;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(k230_nrf9151_read_status(&local, 0) != 0) {
        k230_nrf9151_status_init(&local);
    }
    local.present = k230_nrf9151_uart_present();
    local.epoch = time(NULL);
    snprintf(local.modem_state, sizeof(local.modem_state), "%s",
             local.present ? "present" : "missing");
    if(!local.present) {
        local.link_ok = 0;
        local.sim_ready = 0;
        local.lte_registered = 0;
        local.packet_attached = 0;
        local.pdp_active = 0;
        local.lte_signal_level = 0;
        snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                 "UART missing");
        snprintf(local.last_error, sizeof(local.last_error), "%s",
                 "UART missing");
        k230_nrf9151_write_status(&local);
        if(status) {
            *status = local;
        }
        errno = ENODEV;
        return -1;
    }

    if(nrf9151_manager_open_session(owner ? owner : "nrf9151-lte",
                                    wait_ms, &lock_fd, &fd,
                                    log, log_len) != 0) {
        saved_errno = errno;
        failures++;
        if(write_open_error) {
            snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                     "UART unavailable");
            snprintf(local.last_error, sizeof(local.last_error), "%s",
                     strerror(saved_errno));
            local.epoch = time(NULL);
            k230_nrf9151_write_status(&local);
        }
        if(status) {
            *status = local;
        }
        errno = saved_errno;
        return -1;
    }

    local.link_ok = 0;
    local.sim_ready = 0;
    local.lte_registered = 0;
    local.packet_attached = 0;
    local.pdp_active = 0;
    local.lte_signal_level = 0;
    snprintf(local.sim_status, sizeof(local.sim_status), "%s", "--");
    snprintf(local.operator_name, sizeof(local.operator_name), "%s", "--");
    snprintf(local.ip, sizeof(local.ip), "%s", "--");
    snprintf(local.apn, sizeof(local.apn), "%s", "--");
    snprintf(local.signal_text, sizeof(local.signal_text), "%s", "--");
    snprintf(local.lte_status, sizeof(local.lte_status), "%s",
             include_ping ? "Checking" : "Refreshing");
    snprintf(local.last_error, sizeof(local.last_error), "%s", "-");
    local.epoch = time(NULL);
    k230_nrf9151_write_status(&local);

    if(quiesce_gnss) {
        nrf9151_manager_quiesce_gnss_output(fd, log, log_len);
    }

    for(size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        int rc = nrf9151_manager_run_cmd_logged(fd, cmds[i],
                                                K230_NRF9151_CMD_TIMEOUT_US,
                                                &local, log, log_len,
                                                cancel_cb, cancel_user);

        if(rc == -2) {
            snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                     "Canceled");
            snprintf(local.last_error, sizeof(local.last_error), "%s",
                     "Canceled");
            local.epoch = time(NULL);
            k230_nrf9151_write_status(&local);
            nrf9151_manager_close_session(lock_fd, fd);
            if(status) {
                *status = local;
            }
            return -2;
        }
        if(rc != 0) {
            failures++;
        }
        local.epoch = time(NULL);
        k230_nrf9151_write_status(&local);
        usleep(100000);
    }
    if(include_ping) {
        char more[2048] = "";
        int rc;
        int urc_rc;

        rc = nrf9151_manager_run_cmd_logged(fd, ping_cmd,
                                            K230_NRF9151_LONG_TIMEOUT_US,
                                            &local, log, log_len,
                                            cancel_cb, cancel_user);
        if(rc == -2) {
            snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                     "Canceled");
            snprintf(local.last_error, sizeof(local.last_error), "%s",
                     "Canceled");
            local.epoch = time(NULL);
            k230_nrf9151_write_status(&local);
            nrf9151_manager_close_session(lock_fd, fd);
            if(status) {
                *status = local;
            }
            return -2;
        }
        if(rc != 0) {
            failures++;
        }
        urc_rc = nrf9151_manager_read_ping_result(fd, more, sizeof(more),
                                                  20000000ULL,
                                                  cancel_cb,
                                                  cancel_user);
        if(urc_rc == -2) {
            snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                     "Canceled");
            snprintf(local.last_error, sizeof(local.last_error), "%s",
                     "Canceled");
            local.epoch = time(NULL);
            k230_nrf9151_write_status(&local);
            nrf9151_manager_close_session(lock_fd, fd);
            if(status) {
                *status = local;
            }
            return -2;
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
            nrf9151_manager_update_lte_from_response(&local, ping_cmd,
                                                     more, 0);
        }
        nrf9151_manager_drain_uart(fd, 150000ULL, 1000000ULL);
        local.epoch = time(NULL);
        k230_nrf9151_write_status(&local);
    }

    if(!local.link_ok) {
        snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                 "No AT response");
    } else if(!local.sim_ready) {
        snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                 "SIM not ready");
    } else if(local.lte_registered || local.packet_attached ||
              local.pdp_active) {
        snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                 local.ip[0] && strcmp(local.ip, "--") != 0 ?
                 "Online" : "Registered");
    }
    if(failures == 0) {
        snprintf(local.last_error, sizeof(local.last_error), "%s", "-");
    }
    local.epoch = time(NULL);
    k230_nrf9151_write_status(&local);
    nrf9151_manager_close_session(lock_fd, fd);
    if(status) {
        *status = local;
    }
    return (!include_ping && local.link_ok) ? 0 : (failures == 0 ? 0 : 1);
}

int k230_nrf9151_run_lte_check_ex(k230_nrf9151_status_t *status,
                                  char *log, size_t log_len,
                                  k230_nrf9151_cancel_cb_t cancel_cb,
                                  void *cancel_user)
{
    return nrf9151_manager_run_lte_check_common(status, log, log_len,
                                                cancel_cb, cancel_user,
                                                1, 8000, 1, 1,
                                                "nrf9151-lte");
}

int k230_nrf9151_run_lte_check(k230_nrf9151_status_t *status,
                               char *log, size_t log_len)
{
    return k230_nrf9151_run_lte_check_ex(status, log, log_len, NULL, NULL);
}

static void nrf9151_manager_seed_status_monitor_cache(void)
{
    k230_nrf9151_status_t status;
    int present;

    present = k230_nrf9151_uart_present();
    if(k230_nrf9151_read_status(&status, 20) == 0) {
        return;
    }
    k230_nrf9151_status_init(&status);
    status.present = present;
    status.epoch = time(NULL);
    snprintf(status.modem_state, sizeof(status.modem_state), "%s",
             present ? "probing" : "missing");
    snprintf(status.lte_status, sizeof(status.lte_status), "%s",
             present ? "Probing" : "UART missing");
    snprintf(status.last_error, sizeof(status.last_error), "%s",
             present ? "-" : "UART missing");
    k230_nrf9151_write_status(&status);
}

static void *nrf9151_manager_status_monitor_main(void *arg)
{
    (void)arg;

    for(;;) {
        uint64_t now = nrf9151_manager_monotonic_us();
        int should_run = 0;
        int stop = 0;

        pthread_mutex_lock(&nrf9151_manager_lock);
        stop = nrf9151_status_monitor.stop;
        if(stop) {
            nrf9151_status_monitor.started = 0;
            nrf9151_status_monitor.refreshing = 0;
            pthread_mutex_unlock(&nrf9151_manager_lock);
            break;
        }
        if(nrf9151_status_monitor.request ||
           nrf9151_status_monitor.last_run_us == 0ULL ||
           now - nrf9151_status_monitor.last_run_us >=
           K230_NRF9151_STATUS_REFRESH_US) {
            nrf9151_status_monitor.request = 0;
            nrf9151_status_monitor.refreshing = 1;
            nrf9151_status_monitor.last_run_us = now;
            should_run = 1;
        }
        pthread_mutex_unlock(&nrf9151_manager_lock);

        if(should_run) {
            int rc = -1;

            if(!k230_nrf9151_gnss_monitor_active() &&
               !k230_nrf9151_mqtt_session_connected()) {
                char log[1024];
                k230_nrf9151_status_t status;

                log[0] = '\0';
                rc = nrf9151_manager_run_lte_check_common(
                    &status, log, sizeof(log), NULL, NULL,
                    0, 120, 0, 0, "nrf9151-status");
            }
            pthread_mutex_lock(&nrf9151_manager_lock);
            nrf9151_status_monitor.refreshing = 0;
            if(rc == 0) {
                nrf9151_status_monitor.last_success_us =
                    nrf9151_manager_monotonic_us();
            } else {
                nrf9151_status_monitor.last_run_us =
                    nrf9151_manager_monotonic_us() -
                    (K230_NRF9151_STATUS_REFRESH_US -
                     K230_NRF9151_STATUS_RETRY_US);
            }
            pthread_mutex_unlock(&nrf9151_manager_lock);
        } else {
            usleep(250000);
        }
    }
    return NULL;
}

int k230_nrf9151_status_monitor_start(void)
{
    pthread_t thread;

    nrf9151_manager_seed_status_monitor_cache();
    pthread_mutex_lock(&nrf9151_manager_lock);
    if(nrf9151_status_monitor.started) {
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return 0;
    }
    memset(&nrf9151_status_monitor, 0, sizeof(nrf9151_status_monitor));
    nrf9151_status_monitor.started = 1;
    nrf9151_status_monitor.request = 1;
    pthread_mutex_unlock(&nrf9151_manager_lock);

    if(pthread_create(&thread, NULL,
                      nrf9151_manager_status_monitor_main, NULL) != 0) {
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_status_monitor.started = 0;
        nrf9151_status_monitor.request = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return -1;
    }
    pthread_detach(thread);
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_status_monitor.thread = thread;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return 0;
}

void k230_nrf9151_status_monitor_stop(void)
{
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_status_monitor.stop = 1;
    nrf9151_status_monitor.request = 0;
    pthread_mutex_unlock(&nrf9151_manager_lock);
}

void k230_nrf9151_status_monitor_request_refresh(void)
{
    if(k230_nrf9151_status_monitor_start() != 0) {
        return;
    }
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_status_monitor.request = 1;
    pthread_mutex_unlock(&nrf9151_manager_lock);
}

int k230_nrf9151_status_monitor_active(void)
{
    int active;

    pthread_mutex_lock(&nrf9151_manager_lock);
    active = nrf9151_status_monitor.started;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return active;
}

static int nrf9151_manager_nmea_prefix(const char *line)
{
    return line && (strncmp(line, "$GP", 3) == 0 ||
                    strncmp(line, "$GN", 3) == 0 ||
                    strncmp(line, "$GA", 3) == 0 ||
                    strncmp(line, "$GB", 3) == 0 ||
                    strncmp(line, "$BD", 3) == 0);
}

static int nrf9151_manager_nmea_checksum_ok(const char *line)
{
    const char *star;
    unsigned int calc = 0;
    unsigned int expect;

    if(!line || line[0] != '$') {
        return 0;
    }
    star = strchr(line, '*');
    if(!star) {
        return 1;
    }
    if(!isxdigit((unsigned char)star[1]) ||
       !isxdigit((unsigned char)star[2])) {
        return 0;
    }
    for(const char *p = line + 1; p < star; p++) {
        calc ^= (unsigned char)*p;
    }
    expect = (unsigned int)strtoul(star + 1, NULL, 16);
    return (calc & 0xffU) == (expect & 0xffU);
}

static int nrf9151_manager_split_csv(char *body, char **fields,
                                     int max_fields)
{
    int count = 0;
    char *p = body;

    while(count < max_fields) {
        fields[count++] = p;
        while(*p && *p != ',') {
            p++;
        }
        if(!*p) {
            break;
        }
        *p++ = '\0';
    }
    return count;
}

static int nrf9151_manager_coord_to_double(const char *value, const char *dir,
                                           double *out)
{
    int deg_width;
    int degrees;
    double raw;
    char deg_str[4] = { 0 };

    if(!value || !value[0] || !dir || !dir[0] || !out) {
        return 0;
    }
    deg_width = (dir[0] == 'N' || dir[0] == 'S') ? 2 : 3;
    if((int)strlen(value) <= deg_width ||
       (size_t)deg_width >= sizeof(deg_str)) {
        return 0;
    }
    memcpy(deg_str, value, (size_t)deg_width);
    degrees = atoi(deg_str);
    raw = strtod(value + deg_width, NULL);
    *out = (double)degrees + raw / 60.0;
    if(dir[0] == 'S' || dir[0] == 'W') {
        *out = -*out;
    }
    return isfinite(*out);
}

static void nrf9151_manager_status_set_gnss_phase(
    k230_nrf9151_status_t *status)
{
    if(!status) {
        return;
    }
    if(!status->gnss_running) {
        snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s", "off");
    } else if(status->gnss_has_fix) {
        snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s", "fix");
    } else if(status->nmea_rx_count == 0U) {
        snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s",
                 "first");
    } else if(status->satellites == 0U) {
        snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s",
                 "no_sat");
    } else {
        snprintf(status->gnss_phase, sizeof(status->gnss_phase), "%s",
                 "sat_no_fix");
    }
}

static void nrf9151_manager_apply_gnss_fix(k230_nrf9151_status_t *status,
                                           double lat, double lon,
                                           int has_alt, double alt_m,
                                           unsigned int sats)
{
    uint64_t now_us = nrf9151_manager_monotonic_us();

    if(!status || !isfinite(lat) || !isfinite(lon) ||
       lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 ||
       (fabs(lat) < 0.000001 && fabs(lon) < 0.000001)) {
        return;
    }
    status->gnss_has_fix = 1;
    status->latitude = lat;
    status->longitude = lon;
    status->altitude_m = alt_m;
    status->has_altitude = has_alt;
    status->satellites = sats;
    snprintf(status->gps_state, sizeof(status->gps_state), "%s", "fix");
    snprintf(status->gnss_status, sizeof(status->gnss_status),
             "Fix %.6f %.6f S%u", lat, lon, sats);
    if(!nrf9151_gnss_monitor.first_fix_reported) {
        if(nrf9151_gnss_monitor.session_start_us > 0ULL &&
           now_us >= nrf9151_gnss_monitor.session_start_us) {
            status->ttff_ms =
                (unsigned long)((now_us -
                                 nrf9151_gnss_monitor.session_start_us) /
                                1000ULL);
        }
        nrf9151_gnss_monitor.first_fix_reported = 1;
    }
    nrf9151_gnss_monitor.last_fix_us = now_us;
    nrf9151_manager_status_set_gnss_phase(status);
    k230_nrf9151_write_gnss_fix(lat, lon, has_alt, alt_m, (int)sats,
                                "nrf9151-manager");
}

static void nrf9151_manager_process_nmea_sentence(
    k230_nrf9151_status_t *status, const char *line)
{
    char body[256];
    char talker[3] = "";
    char *fields[32];
    const char *star;
    const char *type;
    size_t len;
    int count;

    if(!status || !line || !nrf9151_manager_nmea_checksum_ok(line)) {
        return;
    }
    status->nmea_valid_count++;
    star = strchr(line, '*');
    len = star ? (size_t)(star - line - 1) : strlen(line + 1);
    if(len >= sizeof(body)) {
        len = sizeof(body) - 1U;
    }
    memcpy(body, line + 1, len);
    body[len] = '\0';
    count = nrf9151_manager_split_csv(body, fields, 32);
    if(count <= 0 || strlen(fields[0]) < 5U) {
        return;
    }
    snprintf(talker, sizeof(talker), "%.2s", fields[0]);
    type = fields[0] + strlen(fields[0]) - 3U;
    if(strcmp(type, "GGA") == 0) {
        double lat = 0.0;
        double lon = 0.0;
        double alt = 0.0;
        int fix = count > 6 ? nrf9151_manager_parse_int_field(fields[6]) : 0;
        int sats = count > 7 ? nrf9151_manager_parse_int_field(fields[7]) : 0;

        if(sats > 0) {
            status->satellites = (unsigned int)sats;
        }
        if(fix > 0 && count > 9 &&
           nrf9151_manager_coord_to_double(fields[2], fields[3], &lat) &&
           nrf9151_manager_coord_to_double(fields[4], fields[5], &lon)) {
            alt = fields[9] && fields[9][0] ? strtod(fields[9], NULL) : 0.0;
            nrf9151_manager_apply_gnss_fix(status, lat, lon,
                                           fields[9] && fields[9][0], alt,
                                           status->satellites);
        } else {
            status->gnss_has_fix = 0;
            status->nmea_nofix_count++;
        }
    } else if(strcmp(type, "RMC") == 0) {
        double lat = 0.0;
        double lon = 0.0;

        if(count > 6 && fields[2][0] == 'A' &&
           nrf9151_manager_coord_to_double(fields[3], fields[4], &lat) &&
           nrf9151_manager_coord_to_double(fields[5], fields[6], &lon)) {
            nrf9151_manager_apply_gnss_fix(status, lat, lon, 0, 0.0,
                                           status->satellites);
        } else if(count > 2 && fields[2][0] == 'V') {
            status->gnss_has_fix = 0;
        }
    } else if(strcmp(type, "GSV") == 0) {
        int total_msgs = count > 1 ? nrf9151_manager_parse_int_field(fields[1]) : 0;
        int msg_num = count > 2 ? nrf9151_manager_parse_int_field(fields[2]) : 0;
        int sats = count > 3 ? nrf9151_manager_parse_int_field(fields[3]) : 0;
        unsigned int before_count = status->satellite_detail_count;

        if(sats >= 0) {
            status->satellites = (unsigned int)sats;
        }
        if(msg_num == 1) {
            nrf9151_manager_clear_satellite_talker(status, talker);
        }
        for(int i = 4; i + 3 < count; i += 4) {
            int prn = nrf9151_manager_parse_int_field(fields[i]);
            int elevation = nrf9151_manager_parse_int_field(fields[i + 1]);
            int azimuth = nrf9151_manager_parse_int_field(fields[i + 2]);
            int cn0 = nrf9151_manager_parse_int_field(fields[i + 3]);

            if(prn > 0) {
                nrf9151_manager_upsert_satellite_detail(status, talker, prn,
                                                        elevation, azimuth,
                                                        cn0);
            }
        }
        if(status->satellite_detail_count != before_count ||
           status->nmea_rx_count <= 10U || status->nmea_rx_count % 100U == 0U) {
            nrf9151_manager_debug_log(
                "GNSS GSV %s msg=%d/%d total=%d detail=%u",
                talker, msg_num, total_msgs, sats, status->satellite_detail_count);
        }
    }
}

static void nrf9151_manager_process_gnss_line(char *line)
{
    const char *nmea = NULL;
    k230_nrf9151_status_t status;
    uint64_t now_us;

    if(!line) {
        return;
    }
    nrf9151_manager_trim_text(line);
    if(!line[0]) {
        return;
    }
    now_us = nrf9151_manager_monotonic_us();
    if(k230_nrf9151_read_status(&status, 0) != 0) {
        k230_nrf9151_status_init(&status);
    }
    status.present = 1;
    status.link_ok = 1;
    status.gnss_running = 1;
    snprintf(status.modem_state, sizeof(status.modem_state), "%s", "present");
    if(strncmp(line, "#XGNSSNMEA:", 11) == 0) {
        nmea = line + 11;
        while(*nmea && isspace((unsigned char)*nmea)) {
            nmea++;
        }
    } else if(nrf9151_manager_nmea_prefix(line)) {
        nmea = line;
    }
    if(nmea && nrf9151_manager_nmea_prefix(nmea)) {
        status.nmea_rx_count++;
        nrf9151_gnss_monitor.last_nmea_us = now_us;
        status.last_nmea_ms = 0;
        nrf9151_manager_process_nmea_sentence(&status, nmea);
        if(status.nmea_rx_count <= 5U ||
           status.nmea_rx_count % 20U == 0U) {
            nrf9151_manager_debug_log(
                "GNSS NMEA rx=%u valid=%u nofix=%u sats=%u fix=%d",
                status.nmea_rx_count, status.nmea_valid_count,
                status.nmea_nofix_count, status.satellites,
                status.gnss_has_fix);
        }
        if(!status.gnss_has_fix) {
            snprintf(status.gps_state, sizeof(status.gps_state), "%s",
                     "searching");
            snprintf(status.gnss_status, sizeof(status.gnss_status),
                     status.satellites > 0U ?
                     "Satellites visible, no fix" :
                     "GNSS running, no satellites");
        }
    } else if(strncmp(line, "#XGNSS:", 7) == 0 ||
              strncmp(line, "#XNMEA:", 7) == 0) {
        snprintf(status.gnss_status, sizeof(status.gnss_status), "%s", line);
    }
    if(nrf9151_gnss_monitor.last_nmea_us > 0ULL &&
       now_us >= nrf9151_gnss_monitor.last_nmea_us) {
        status.last_nmea_ms =
            (unsigned long)((now_us - nrf9151_gnss_monitor.last_nmea_us) /
                            1000ULL);
    }
    nrf9151_manager_status_set_gnss_phase(&status);
    status.epoch = time(NULL);
    k230_nrf9151_write_status(&status);
}

static void nrf9151_manager_mark_gnss_off(const char *reason)
{
    k230_nrf9151_status_t status;

    if(k230_nrf9151_read_status(&status, 0) != 0) {
        k230_nrf9151_status_init(&status);
    }
    status.gnss_running = 0;
    status.gnss_has_fix = 0;
    status.last_nmea_ms = 0;
    snprintf(status.gps_state, sizeof(status.gps_state), "%s", "off");
    snprintf(status.gnss_phase, sizeof(status.gnss_phase), "%s", "off");
    snprintf(status.gnss_status, sizeof(status.gnss_status), "%s",
             reason && reason[0] ? reason : "Off");
    status.epoch = time(NULL);
    k230_nrf9151_write_status(&status);
}

static void nrf9151_manager_feed_gnss_bytes(const char *buf, size_t len)
{
    for(size_t i = 0; i < len; i++) {
        char c = buf[i];

        if(c == '\r') {
            continue;
        }
        if(c == '\n') {
            nrf9151_gnss_monitor.line[nrf9151_gnss_monitor.line_used] = '\0';
            nrf9151_manager_process_gnss_line(nrf9151_gnss_monitor.line);
            nrf9151_gnss_monitor.line_used = 0;
            continue;
        }
        if(nrf9151_gnss_monitor.line_used + 1U >=
           sizeof(nrf9151_gnss_monitor.line)) {
            nrf9151_gnss_monitor.line[nrf9151_gnss_monitor.line_used] = '\0';
            nrf9151_manager_process_gnss_line(nrf9151_gnss_monitor.line);
            nrf9151_gnss_monitor.line_used = 0;
        }
        nrf9151_gnss_monitor.line[nrf9151_gnss_monitor.line_used++] = c;
    }
}

static int nrf9151_manager_response_gnss_active(const char *resp)
{
    const char *p;

    if(!resp) {
        return -1;
    }
    p = strstr(resp, "#XGNSS:");
    if(!p) {
        return -1;
    }
    p = strchr(p, ':');
    if(!p) {
        return -1;
    }
    p++;
    while(*p && isspace((unsigned char)*p)) {
        p++;
    }
    return atoi(p) > 0 ? 1 : 0;
}

static int nrf9151_manager_gnss_should_stop(void)
{
    int stop;

    pthread_mutex_lock(&nrf9151_manager_lock);
    stop = nrf9151_gnss_monitor.stop;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return stop;
}

static int nrf9151_manager_start_gnss_locked(int fd)
{
    char resp[1024];
    int rc;
    int active;
    k230_nrf9151_status_t status;

    if(k230_nrf9151_read_status(&status, 0) != 0) {
        k230_nrf9151_status_init(&status);
    }
    rc = nrf9151_manager_exchange(fd, "AT#XGNSS?", resp, sizeof(resp),
                                  K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL);
    nrf9151_manager_debug_log("GNSS cmd AT#XGNSS? rc=%d resp=%s", rc, resp);
    active = rc == 0 ? nrf9151_manager_response_gnss_active(resp) : -1;
    if(active > 0) {
        rc = nrf9151_manager_exchange(fd, "AT#XGNSS=0", resp, sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                      NULL);
        nrf9151_manager_debug_log("GNSS cmd AT#XGNSS=0 rc=%d resp=%s",
                                  rc, resp);
        rc = nrf9151_manager_exchange(fd, "AT#XNMEA=0", resp, sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                      NULL);
        nrf9151_manager_debug_log("GNSS cmd AT#XNMEA=0 rc=%d resp=%s",
                                  rc, resp);
        usleep(120000);
    }

    rc = nrf9151_manager_exchange(fd, "AT%XSYSTEMMODE=1,0,1,0", resp,
                                  sizeof(resp), K230_NRF9151_CMD_TIMEOUT_US,
                                  NULL, NULL);
    nrf9151_manager_debug_log(
        "GNSS cmd AT%%XSYSTEMMODE=1,0,1,0 rc=%d resp=%s", rc, resp);
    rc = nrf9151_manager_exchange(fd, "AT+CFUN=1", resp, sizeof(resp),
                                  K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                  NULL, NULL);
    nrf9151_manager_debug_log("GNSS cmd AT+CFUN=1 rc=%d resp=%s", rc, resp);
    rc = nrf9151_manager_exchange(fd, "AT#XNMEA=1", resp, sizeof(resp),
                                  K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                  NULL);
    nrf9151_manager_debug_log("GNSS cmd AT#XNMEA=1 rc=%d resp=%s", rc, resp);
    rc = nrf9151_manager_exchange(fd, "AT#XGNSS=1,0,1", resp, sizeof(resp),
                                  K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL);
    nrf9151_manager_debug_log("GNSS cmd AT#XGNSS=1,0,1 rc=%d resp=%s",
                              rc, resp);
    if(rc != 0) {
        rc = nrf9151_manager_exchange(fd, "AT#XGNSS=1,0,0,0", resp,
                                      sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                      NULL);
        nrf9151_manager_debug_log(
            "GNSS cmd AT#XGNSS=1,0,0,0 rc=%d resp=%s", rc, resp);
    }
    if(rc != 0) {
        char status_resp[1024];
        int status_rc;

        status_rc = nrf9151_manager_exchange(fd, "AT#XGNSS?", status_resp,
                                             sizeof(status_resp),
                                             K230_NRF9151_CMD_TIMEOUT_US,
                                             NULL, NULL);
        nrf9151_manager_debug_log("GNSS cmd AT#XGNSS? rc=%d resp=%s",
                                  status_rc, status_resp);
        if(status_rc != 0 ||
           nrf9151_manager_response_gnss_active(status_resp) <= 0) {
            snprintf(status.gnss_status, sizeof(status.gnss_status),
                     "%s", "GNSS start failed");
            snprintf(status.gps_state, sizeof(status.gps_state), "%s",
                     "error");
            status.gnss_running = 0;
            k230_nrf9151_write_status(&status);
            return -1;
        }
    }
    nrf9151_gnss_monitor.configured = 1;
    nrf9151_gnss_monitor.session_start_us = nrf9151_manager_monotonic_us();
    nrf9151_gnss_monitor.first_fix_reported = 0;
    nrf9151_gnss_monitor.last_fix_us = 0;
    nrf9151_gnss_monitor.last_nmea_us = 0;
    status.present = 1;
    status.link_ok = 1;
    status.gnss_running = 1;
    status.gnss_has_fix = 0;
    status.nmea_rx_count = 0;
    status.nmea_valid_count = 0;
    status.nmea_nofix_count = 0;
    status.satellites = 0;
    nrf9151_manager_clear_satellite_details(&status);
    status.ttff_ms = 0;
    status.last_nmea_ms = 0;
    snprintf(status.modem_state, sizeof(status.modem_state), "%s", "present");
    snprintf(status.gps_state, sizeof(status.gps_state), "%s", "searching");
    snprintf(status.gnss_status, sizeof(status.gnss_status), "%s",
             "GNSS running");
    nrf9151_manager_status_set_gnss_phase(&status);
    status.epoch = time(NULL);
    k230_nrf9151_write_status(&status);
    return 0;
}

static void *nrf9151_manager_gnss_thread(void *arg)
{
    int lock_fd = -1;
    int fd = -1;

    (void)arg;
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_gnss_monitor.active = 1;
    nrf9151_gnss_monitor.stop = 0;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    nrf9151_manager_debug_log("GNSS thread enter owners=0x%x",
                              nrf9151_gnss_monitor.owners);

    if(nrf9151_manager_open_session("nrf9151-gnss", 8000, &lock_fd, &fd,
                                    NULL, 0) != 0) {
        nrf9151_manager_debug_log("GNSS session open failed: %s",
                                  strerror(errno));
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_manager_status_set_error_locked("GNSS UART unavailable");
        nrf9151_gnss_monitor.active = 0;
        nrf9151_gnss_monitor.owners = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        nrf9151_manager_mark_gnss_off("GNSS UART unavailable");
        return NULL;
    }
    nrf9151_gnss_monitor.fd = fd;
    nrf9151_gnss_monitor.lock_fd = lock_fd;
    if(nrf9151_manager_exchange(fd, "AT", NULL, 0,
                                K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL) != 0 ||
       nrf9151_manager_start_gnss_locked(fd) != 0) {
        nrf9151_manager_debug_log("GNSS start failed after session open");
        nrf9151_manager_close_session(lock_fd, fd);
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_gnss_monitor.fd = -1;
        nrf9151_gnss_monitor.lock_fd = -1;
        nrf9151_gnss_monitor.active = 0;
        nrf9151_gnss_monitor.owners = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        nrf9151_manager_mark_gnss_off("GNSS start failed");
        return NULL;
    }
    nrf9151_manager_debug_log("GNSS configured owners=0x%x",
                              nrf9151_gnss_monitor.owners);

    while(!nrf9151_manager_gnss_should_stop()) {
        fd_set rfds;
        struct timeval tv;
        char buf[256];
        ssize_t rd;
        uint64_t now_us;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        if(select(fd + 1, &rfds, NULL, NULL, &tv) > 0 &&
           FD_ISSET(fd, &rfds)) {
            rd = read(fd, buf, sizeof(buf));
            if(rd > 0) {
                nrf9151_manager_feed_gnss_bytes(buf, (size_t)rd);
            }
        }
        now_us = nrf9151_manager_monotonic_us();
        if(nrf9151_gnss_monitor.configured &&
           nrf9151_gnss_monitor.session_start_us > 0ULL &&
           nrf9151_gnss_monitor.last_nmea_us == 0ULL &&
           now_us - nrf9151_gnss_monitor.session_start_us >
           K230_NRF9151_GNSS_NMEA_RESTART_US &&
           (nrf9151_gnss_monitor.last_restart_us == 0ULL ||
            now_us - nrf9151_gnss_monitor.last_restart_us >
            K230_NRF9151_GNSS_NMEA_RESTART_US)) {
            char resp[512];

            nrf9151_gnss_monitor.last_restart_us = now_us;
            nrf9151_manager_debug_log("GNSS restart: no NMEA after %llums",
                                      (unsigned long long)(
                                          (now_us -
                                           nrf9151_gnss_monitor.session_start_us) /
                                          1000ULL));
            (void)nrf9151_manager_exchange(fd, "AT#XGNSS=0", resp,
                                           sizeof(resp),
                                           K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                           NULL);
            (void)nrf9151_manager_exchange(fd, "AT#XNMEA=0", resp,
                                           sizeof(resp),
                                           K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                           NULL);
            usleep(120000);
            (void)nrf9151_manager_start_gnss_locked(fd);
        }
    }
    nrf9151_manager_debug_log("GNSS thread stop requested owners=0x%x",
                              nrf9151_gnss_monitor.owners);
    (void)nrf9151_manager_exchange(fd, "AT#XGNSS=0", NULL, 0,
                                   K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL);
    (void)nrf9151_manager_exchange(fd, "AT#XNMEA=0", NULL, 0,
                                   K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL);
    nrf9151_manager_close_session(lock_fd, fd);
    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_gnss_monitor.fd = -1;
    nrf9151_gnss_monitor.lock_fd = -1;
    nrf9151_gnss_monitor.active = 0;
    nrf9151_gnss_monitor.configured = 0;
    nrf9151_gnss_monitor.owners = 0;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    nrf9151_manager_mark_gnss_off("Off");
    nrf9151_manager_debug_log("GNSS thread exit");
    return NULL;
}

int k230_nrf9151_start_gnss_monitor(void)
{
    return k230_nrf9151_start_gnss_monitor_for("legacy");
}

int k230_nrf9151_start_gnss_monitor_for(const char *owner)
{
    pthread_t thread;
    unsigned int owner_bit = nrf9151_manager_gnss_owner_bit(owner);

    pthread_mutex_lock(&nrf9151_manager_lock);
    nrf9151_gnss_monitor.owners |= owner_bit;
    if(nrf9151_gnss_monitor.active) {
        nrf9151_gnss_monitor.stop = 0;
        nrf9151_manager_debug_log("GNSS start owner=%s already active owners=0x%x",
                                  nrf9151_manager_gnss_owner_name(owner_bit),
                                  nrf9151_gnss_monitor.owners);
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return 0;
    }
    nrf9151_gnss_monitor.active = 1;
    nrf9151_gnss_monitor.configured = 0;
    nrf9151_gnss_monitor.fd = -1;
    nrf9151_gnss_monitor.lock_fd = -1;
    nrf9151_gnss_monitor.stop = 0;
    nrf9151_manager_debug_log("GNSS start owner=%s owners=0x%x",
                              nrf9151_manager_gnss_owner_name(owner_bit),
                              nrf9151_gnss_monitor.owners);
    pthread_mutex_unlock(&nrf9151_manager_lock);

    if(pthread_create(&thread, NULL, nrf9151_manager_gnss_thread, NULL) != 0) {
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_gnss_monitor.owners &= ~owner_bit;
        nrf9151_gnss_monitor.active = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        nrf9151_manager_debug_log("GNSS pthread_create failed owner=%s",
                                  nrf9151_manager_gnss_owner_name(owner_bit));
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

int k230_nrf9151_stop_gnss_monitor(void)
{
    pthread_mutex_lock(&nrf9151_manager_lock);
    if(!nrf9151_gnss_monitor.active) {
        nrf9151_gnss_monitor.owners = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        nrf9151_manager_debug_log("GNSS force stop ignored: inactive");
        return 0;
    }
    nrf9151_gnss_monitor.owners = 0;
    nrf9151_gnss_monitor.stop = 1;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    nrf9151_manager_debug_log("GNSS force stop requested");
    return 0;
}

int k230_nrf9151_stop_gnss_monitor_for(const char *owner)
{
    unsigned int owner_bit = nrf9151_manager_gnss_owner_bit(owner);
    int should_stop = 0;
    int active;

    pthread_mutex_lock(&nrf9151_manager_lock);
    active = nrf9151_gnss_monitor.active;
    nrf9151_gnss_monitor.owners &= ~owner_bit;
    if(active && nrf9151_gnss_monitor.owners == 0U) {
        nrf9151_gnss_monitor.stop = 1;
        should_stop = 1;
    }
    nrf9151_manager_debug_log(
        "GNSS stop owner=%s active=%d owners=0x%x stop=%d",
        nrf9151_manager_gnss_owner_name(owner_bit), active,
        nrf9151_gnss_monitor.owners, should_stop);
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return 0;
}

int k230_nrf9151_stop_gnss_monitor_wait(int wait_ms)
{
    uint64_t start_us;
    int active;

    pthread_mutex_lock(&nrf9151_manager_lock);
    active = nrf9151_gnss_monitor.active;
    if(active) {
        nrf9151_gnss_monitor.owners = 0;
        nrf9151_gnss_monitor.stop = 1;
    }
    pthread_mutex_unlock(&nrf9151_manager_lock);
    if(!active) {
        nrf9151_manager_debug_log("GNSS stop wait ignored: inactive");
        return 0;
    }
    nrf9151_manager_debug_log("GNSS force stop wait requested wait=%dms",
                              wait_ms);
    if(wait_ms <= 0) {
        return 0;
    }

    start_us = nrf9151_manager_monotonic_us();
    do {
        usleep(20000);
        pthread_mutex_lock(&nrf9151_manager_lock);
        active = nrf9151_gnss_monitor.active;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        if(!active) {
            return 0;
        }
    } while((int)((nrf9151_manager_monotonic_us() - start_us) / 1000ULL) <
            wait_ms);

    errno = ETIMEDOUT;
    return -1;
}

int k230_nrf9151_gnss_monitor_active(void)
{
    int active;

    pthread_mutex_lock(&nrf9151_manager_lock);
    active = nrf9151_gnss_monitor.active;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return active;
}

int k230_nrf9151_location_autostart_enabled(void)
{
    char value[16];

    if(nrf9151_manager_pref_get(K230_NRF9151_PREF_LOCATION_AUTOSTART, value,
                                sizeof(value), "1") != 0) {
        return 1;
    }
    return atoi(value) != 0;
}

int k230_nrf9151_set_location_autostart_enabled(int enabled)
{
    return nrf9151_manager_pref_set(K230_NRF9151_PREF_LOCATION_AUTOSTART,
                                    enabled ? "1" : "0");
}

int k230_nrf9151_apply_location_autostart(void)
{
    int enabled = k230_nrf9151_location_autostart_enabled();

    (void)k230_nrf9151_status_monitor_start();
    if(!enabled) {
        (void)k230_nrf9151_stop_gnss_monitor_for(K230_NRF9151_LOCATION_OWNER);
        (void)k230_nrf9151_stop_gnss_monitor_for("meshtastic");
        nrf9151_manager_debug_log("location autostart disabled");
        return 0;
    }
    if(!k230_nrf9151_uart_present()) {
        nrf9151_manager_debug_log("location autostart skipped: uart missing");
        errno = ENODEV;
        return -1;
    }
    nrf9151_manager_debug_log("location autostart enabled");
    return k230_nrf9151_start_gnss_monitor_for(K230_NRF9151_LOCATION_OWNER);
}

static void nrf9151_manager_escape_at_string(const char *in, char *out,
                                             size_t out_len)
{
    size_t used = 0;

    if(!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if(!in) {
        return;
    }
    for(size_t i = 0; in[i] && used + 1U < out_len; i++) {
        if((in[i] == '"' || in[i] == '\\') && used + 2U < out_len) {
            out[used++] = '\\';
            out[used++] = in[i];
        } else if((unsigned char)in[i] >= 0x20U) {
            out[used++] = in[i];
        }
    }
    out[used] = '\0';
}

static int nrf9151_manager_parse_url(const char *url, int *https,
                                     char *host, size_t host_len,
                                     int *port)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    char port_text[12];
    size_t copy;

    if(!url || !url[0] || !https || !host || host_len == 0U || !port) {
        return -1;
    }
    if(strncmp(url, "https://", 8) == 0) {
        *https = 1;
        p = url + 8;
        *port = 443;
    } else if(strncmp(url, "http://", 7) == 0) {
        *https = 0;
        p = url + 7;
        *port = 80;
    } else {
        return -1;
    }
    host_start = p;
    while(*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        p++;
    }
    host_end = p;
    if(*p == ':') {
        const char *port_start = ++p;
        size_t port_len;

        while(*p && isdigit((unsigned char)*p)) {
            p++;
        }
        port_len = (size_t)(p - port_start);
        if(port_len == 0U || port_len >= sizeof(port_text)) {
            return -1;
        }
        memcpy(port_text, port_start, port_len);
        port_text[port_len] = '\0';
        *port = atoi(port_text);
    }
    copy = (size_t)(host_end - host_start);
    if(copy == 0U || copy >= host_len || *port <= 0 || *port > 65535) {
        return -1;
    }
    memcpy(host, host_start, copy);
    host[copy] = '\0';
    return 0;
}

static int nrf9151_manager_parse_socket_handle(const char *resp,
                                               const char *token)
{
    const char *p;

    if(!resp || !token) {
        return -1;
    }
    p = strstr(resp, token);
    if(!p) {
        return -1;
    }
    p = strchr(p, ':');
    if(!p) {
        return -1;
    }
    p++;
    while(*p && isspace((unsigned char)*p)) {
        p++;
    }
    return atoi(p);
}

static int nrf9151_manager_hex_value(int c)
{
    if(c >= '0' && c <= '9') {
        return c - '0';
    }
    if(c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if(c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int nrf9151_manager_http_status_code(const char *resp)
{
    const char *p;
    char *endp;
    long status;

    if(!resp) {
        return -1;
    }
    p = strstr(resp, "#XHTTPCSTAT");
    if(!p) {
        return -1;
    }
    p = strchr(p, ',');
    if(!p) {
        return -1;
    }
    status = strtol(p + 1, &endp, 10);
    if(endp == p + 1 || status < 0 || status > 999) {
        return -1;
    }
    return (int)status;
}

static int nrf9151_manager_http_status_ok(const char *resp)
{
    int status = nrf9151_manager_http_status_code(resp);

    return status >= 200 && status < 300;
}

static size_t nrf9151_manager_decode_hex_line(const char *line,
                                              const char *line_end,
                                              char *out, size_t out_len,
                                              size_t used)
{
    const char *p = line;

    while(p < line_end) {
        int hi;
        int lo;
        unsigned char byte;

        while(p < line_end && isspace((unsigned char)*p)) {
            p++;
        }
        if(p >= line_end) {
            break;
        }
        hi = nrf9151_manager_hex_value((unsigned char)p[0]);
        lo = p + 1 < line_end ?
             nrf9151_manager_hex_value((unsigned char)p[1]) : -1;
        if(hi < 0 || lo < 0) {
            break;
        }
        byte = (unsigned char)((hi << 4) | lo);
        if(out && out_len > 0U && used + 1U < out_len) {
            if(byte == '\r') {
                out[used++] = '\n';
            } else if(byte == 0U) {
                /* Drop NUL bytes from modem payload previews. */
            } else if(byte < 0x20U && byte != '\n' && byte != '\t') {
                out[used++] = ' ';
            } else {
                out[used++] = (char)byte;
            }
            out[used] = '\0';
        }
        p += 2;
    }
    return used;
}

static void nrf9151_manager_http_decode_body(const char *resp,
                                             char *out, size_t out_len)
{
    const char *p;
    size_t used = 0U;

    if(out && out_len > 0U) {
        out[0] = '\0';
    }
    if(!resp || !out || out_len == 0U) {
        return;
    }
    p = resp;
    while((p = strstr(p, "#XHTTPCDATA:")) != NULL) {
        const char *line = strchr(p, '\n');

        if(!line) {
            break;
        }
        line++;
        for(;;) {
            const char *line_end;
            const char *scan;
            int hex_pairs = 0;
            int non_space = 0;

            while(*line == '\r' || *line == '\n') {
                line++;
            }
            if(!*line || *line == '#' || *line == '>') {
                break;
            }
            line_end = line;
            while(*line_end && *line_end != '\r' && *line_end != '\n') {
                line_end++;
            }
            scan = line;
            while(scan < line_end) {
                if(isspace((unsigned char)*scan)) {
                    scan++;
                    continue;
                }
                non_space = 1;
                if(scan + 1 < line_end &&
                   nrf9151_manager_hex_value((unsigned char)scan[0]) >= 0 &&
                   nrf9151_manager_hex_value((unsigned char)scan[1]) >= 0) {
                    hex_pairs++;
                    scan += 2;
                    continue;
                }
                hex_pairs = 0;
                break;
            }
            if(!non_space || hex_pairs <= 0) {
                break;
            }
            used = nrf9151_manager_decode_hex_line(line, line_end, out,
                                                   out_len, used);
            if(used + 1U >= out_len) {
                out[out_len - 1U] = '\0';
                return;
            }
            line = line_end;
        }
        p = line;
    }
}

int k230_nrf9151_http_request_ex(const k230_nrf9151_http_request_t *request,
                                 char *log, size_t log_len,
                                 k230_nrf9151_cancel_cb_t cancel_cb,
                                 void *cancel_user)
{
    char host[160];
    char url[260];
    char cmd[768];
    char resp[4096];
    int https;
    int port;
    int lock_fd = -1;
    int fd = -1;
    int handle;
    int rc;
    int method;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(request && request->response_body && request->response_body_len > 0U) {
        request->response_body[0] = '\0';
    }
    if(request && request->status_code) {
        *request->status_code = -1;
    }
    if(!request || !request->url ||
       nrf9151_manager_parse_url(request->url, &https, host, sizeof(host),
                                 &port) != 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "Invalid URL. Use http:// or https://");
        return -1;
    }
    if(https && request->sec_tag < 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "HTTPS needs a modem TLS credential sec_tag");
        return -2;
    }
    nrf9151_manager_escape_at_string(request->url, url, sizeof(url));
    method = request->method == K230_NRF9151_HTTP_POST ? 1 : 0;
    if(nrf9151_manager_open_session("nrf9151-http", 8000, &lock_fd, &fd,
                                    log, log_len) != 0) {
        return -1;
    }
    (void)nrf9151_manager_run_cmd_logged(fd, "AT", K230_NRF9151_CMD_TIMEOUT_US,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    nrf9151_manager_quiesce_gnss_output(fd, log, log_len);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT+CFUN=1",
                                         K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    if(https) {
        snprintf(cmd, sizeof(cmd), "AT#XSSOCKET=1,1,0,%d,2",
                 request->sec_tag);
        rc = nrf9151_manager_exchange(fd, cmd, resp, sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, cancel_cb,
                                      cancel_user);
        nrf9151_manager_log_append(log, log_len, "> %s", cmd);
        nrf9151_manager_log_append(log, log_len, "%s", resp);
        handle = nrf9151_manager_parse_socket_handle(resp, "#XSSOCKET:");
    } else {
        snprintf(cmd, sizeof(cmd), "%s", "AT#XSOCKET=1,1,0");
        rc = nrf9151_manager_exchange(fd, cmd, resp, sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, cancel_cb,
                                      cancel_user);
        nrf9151_manager_log_append(log, log_len, "> %s", cmd);
        nrf9151_manager_log_append(log, log_len, "%s", resp);
        handle = nrf9151_manager_parse_socket_handle(resp, "#XSOCKET:");
    }
    if(rc != 0 || handle < 0) {
        nrf9151_manager_close_session(lock_fd, fd);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "AT#XCONNECT=%d,\"%s\",%d", handle, host, port);
    rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                        K230_NRF9151_LONG_TIMEOUT_US, NULL,
                                        log, log_len, cancel_cb, cancel_user);
    if(rc != 0) {
        snprintf(cmd, sizeof(cmd), "AT#XCLOSE=%d", handle);
        (void)nrf9151_manager_run_cmd_logged(fd, cmd,
                                             K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                             log, log_len, NULL, NULL);
        nrf9151_manager_drain_uart(fd, 120000ULL, 1800000ULL);
        nrf9151_manager_close_session(lock_fd, fd);
        return -1;
    }

    if(method == 1) {
        size_t body_len = strlen(request->body ? request->body : "");

        snprintf(cmd, sizeof(cmd),
                 "AT#XHTTPCREQ=%d,\"%s\",1,1,1,%u,\"Content-Type: text/plain\",\"Connection: close\"",
                 handle, url, (unsigned)body_len);
        rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                            K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                            log, log_len, cancel_cb,
                                            cancel_user);
        if(rc == 0 && body_len > 0U) {
            ssize_t wr = write(fd, request->body, body_len);

            nrf9151_manager_log_append(log, log_len, "> body %u/%u bytes",
                                       wr > 0 ? (unsigned)wr : 0U,
                                       (unsigned)body_len);
            if(wr < 0 || (size_t)wr != body_len) {
                rc = -1;
            }
        }
    } else {
        snprintf(cmd, sizeof(cmd),
                 "AT#XHTTPCREQ=%d,\"%s\",0,1,1,0,\"Connection: close\"",
                 handle, url);
        rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                            K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                            log, log_len, cancel_cb,
                                            cancel_user);
    }
    if(rc == 0) {
        char more[4096] = "";
        int urc_rc;

        urc_rc = nrf9151_manager_read_urc(fd, more, sizeof(more),
                                          K230_NRF9151_HTTP_TIMEOUT_US,
                                          cancel_cb, cancel_user);
        if(urc_rc == -2) {
            rc = -2;
        } else if(urc_rc <= 0) {
            nrf9151_manager_log_append(log, log_len,
                                       "HTTP response timeout");
            rc = -1;
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
            if(request->status_code) {
                *request->status_code =
                    nrf9151_manager_http_status_code(more);
            }
            nrf9151_manager_http_decode_body(more, request->response_body,
                                             request->response_body_len);
        }
        if(rc == 0 && !nrf9151_manager_http_status_ok(more)) {
            nrf9151_manager_log_append(log, log_len,
                                       "HTTP status is not 2xx");
            rc = -1;
        }
    }
    snprintf(cmd, sizeof(cmd), "AT#XCLOSE=%d", handle);
    (void)nrf9151_manager_run_cmd_logged(fd, cmd,
                                         K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                         log, log_len, NULL, NULL);
    nrf9151_manager_drain_uart(fd, 80000ULL, 600000ULL);
    nrf9151_manager_close_session(lock_fd, fd);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_http_request(const k230_nrf9151_http_request_t *request,
                              char *log, size_t log_len)
{
    return k230_nrf9151_http_request_ex(request, log, log_len, NULL, NULL);
}

static void nrf9151_manager_mqtt_session_close_locked(char *log,
                                                      size_t log_len)
{
    if(nrf9151_mqtt_session.fd >= 0) {
        (void)nrf9151_manager_run_cmd_logged(
            nrf9151_mqtt_session.fd, "AT#XMQTTCON=0",
            K230_NRF9151_CMD_TIMEOUT_US, NULL, log, log_len, NULL, NULL);
        nrf9151_manager_drain_uart(nrf9151_mqtt_session.fd, 120000ULL,
                                   900000ULL);
        nrf9151_manager_close_session(nrf9151_mqtt_session.lock_fd,
                                      nrf9151_mqtt_session.fd);
    }
    nrf9151_mqtt_session.fd = -1;
    nrf9151_mqtt_session.lock_fd = -1;
    nrf9151_mqtt_session.connected = 0;
    nrf9151_mqtt_session.qos = 0;
    nrf9151_mqtt_session.broker[0] = '\0';
    nrf9151_mqtt_session.topic[0] = '\0';
}

int k230_nrf9151_mqtt_session_connected(void)
{
    int connected;

    pthread_mutex_lock(&nrf9151_mqtt_lock);
    connected = nrf9151_mqtt_session.connected &&
                nrf9151_mqtt_session.fd >= 0;
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return connected;
}

int k230_nrf9151_mqtt_session_disconnect(char *log, size_t log_len)
{
    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(!nrf9151_mqtt_session.connected || nrf9151_mqtt_session.fd < 0) {
        nrf9151_manager_log_append(log, log_len, "MQTT already disconnected");
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return 0;
    }
    nrf9151_manager_log_append(log, log_len, "MQTT disconnect");
    nrf9151_manager_mqtt_session_close_locked(log, log_len);
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return 0;
}

int k230_nrf9151_mqtt_session_connect_ex(
    const k230_nrf9151_mqtt_request_t *request,
    char *log, size_t log_len,
    k230_nrf9151_cancel_cb_t cancel_cb,
    void *cancel_user)
{
    char broker[160];
    char client_id[96];
    char username[128];
    char password[128];
    char topic[160];
    char cmd[768];
    char more[2048] = "";
    int lock_fd = -1;
    int fd = -1;
    int rc;
    int port;
    int op = 1;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(!request || !request->broker || !request->broker[0]) {
        nrf9151_manager_log_append(log, log_len, "Broker is empty");
        return -1;
    }
    if(request->auth == K230_NRF9151_MQTT_AUTH_MTLS) {
        nrf9151_manager_log_append(log, log_len,
                                   "mTLS requires credentials preloaded in modem sec_tag");
    }
    if((request->auth == K230_NRF9151_MQTT_AUTH_TLS_USER_PASS ||
        request->auth == K230_NRF9151_MQTT_AUTH_MTLS) &&
       request->sec_tag < 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "TLS MQTT needs sec_tag with modem credentials");
        return -2;
    }

    port = request->port > 0 ? request->port : 1883;
    nrf9151_manager_escape_at_string(request->broker, broker, sizeof(broker));
    nrf9151_manager_escape_at_string(request->client_id &&
                                     request->client_id[0] ?
                                     request->client_id : "k230-nrf9151",
                                     client_id, sizeof(client_id));
    nrf9151_manager_escape_at_string(request->username ? request->username : "",
                                     username, sizeof(username));
    nrf9151_manager_escape_at_string(request->password ? request->password : "",
                                     password, sizeof(password));
    nrf9151_manager_escape_at_string(request->topic && request->topic[0] ?
                                     request->topic : "k230/test",
                                     topic, sizeof(topic));

    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(nrf9151_mqtt_session.connected && nrf9151_mqtt_session.fd >= 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "MQTT reconnect: close old session");
        nrf9151_manager_mqtt_session_close_locked(log, log_len);
    }

    if(nrf9151_manager_open_session("nrf9151-mqtt-session", 8000, &lock_fd,
                                    &fd, log, log_len) != 0) {
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return -1;
    }
    (void)nrf9151_manager_run_cmd_logged(fd, "AT", K230_NRF9151_CMD_TIMEOUT_US,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    nrf9151_manager_quiesce_gnss_output(fd, log, log_len);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT+CFUN=1",
                                         K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    snprintf(cmd, sizeof(cmd), "AT#XMQTTCFG=\"%s\",60,1", client_id);
    rc = nrf9151_manager_run_cmd_logged(fd, cmd, K230_NRF9151_CMD_TIMEOUT_US,
                                        NULL, log, log_len, cancel_cb,
                                        cancel_user);
    if(rc != 0) {
        goto fail;
    }

    nrf9151_manager_log_append(log, log_len, "MQTT connect: %s:%d",
                               request->broker, port);
    if(request->auth == K230_NRF9151_MQTT_AUTH_TLS_USER_PASS ||
       request->auth == K230_NRF9151_MQTT_AUTH_MTLS) {
        snprintf(cmd, sizeof(cmd),
                 "AT#XMQTTCON=%d,\"%s\",\"%s\",\"%s\",%d,%d",
                 op, username, password, broker, port, request->sec_tag);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "AT#XMQTTCON=%d,\"%s\",\"%s\",\"%s\",%d",
                 op,
                 request->auth == K230_NRF9151_MQTT_AUTH_USER_PASS ?
                 username : "",
                 request->auth == K230_NRF9151_MQTT_AUTH_USER_PASS ?
                 password : "",
                 broker, port);
    }
    rc = nrf9151_manager_run_cmd_logged(fd, cmd, K230_NRF9151_LONG_TIMEOUT_US,
                                        NULL, log, log_len, cancel_cb,
                                        cancel_user);
    if(rc != 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "MQTT connect command failed");
        goto fail;
    }

    rc = nrf9151_manager_read_urc(fd, more, sizeof(more),
                                  K230_NRF9151_MQTT_TIMEOUT_US,
                                  cancel_cb, cancel_user);
    if(rc == -2) {
        goto fail;
    }
    if(more[0]) {
        nrf9151_manager_trim_text(more);
        nrf9151_manager_log_append(log, log_len, "%s", more);
    }
    if(strstr(more, "#XMQTTEVT: 0,0") == NULL) {
        nrf9151_manager_log_append(log, log_len,
                                   "MQTT CONNACK not observed");
        rc = -1;
        goto fail;
    }

    nrf9151_mqtt_session.fd = fd;
    nrf9151_mqtt_session.lock_fd = lock_fd;
    nrf9151_mqtt_session.connected = 1;
    nrf9151_mqtt_session.qos =
        request->qos >= 0 && request->qos <= 2 ? request->qos : 0;
    snprintf(nrf9151_mqtt_session.broker,
             sizeof(nrf9151_mqtt_session.broker), "%s:%d",
             request->broker, port);
    snprintf(nrf9151_mqtt_session.topic,
             sizeof(nrf9151_mqtt_session.topic), "%s",
             request->topic && request->topic[0] ? request->topic :
             "k230/test");
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return 0;

fail:
    if(fd >= 0) {
        (void)nrf9151_manager_run_cmd_logged(fd, "AT#XMQTTCON=0",
                                             K230_NRF9151_CMD_TIMEOUT_US,
                                             NULL, log, log_len, NULL, NULL);
        nrf9151_manager_close_session(lock_fd, fd);
    } else if(lock_fd >= 0) {
        k230_nrf9151_release_uart(lock_fd);
    }
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return rc == -2 ? -2 : -1;
}

int k230_nrf9151_mqtt_session_subscribe(const char *topic, int qos,
                                         char *log, size_t log_len,
                                         k230_nrf9151_cancel_cb_t cancel_cb,
                                         void *cancel_user)
{
    char topic_at[180];
    char cmd[256];
    char more[2048] = "";
    int rc;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(!topic || !topic[0]) {
        nrf9151_manager_log_append(log, log_len, "Topic is empty");
        return -1;
    }
    nrf9151_manager_escape_at_string(topic, topic_at, sizeof(topic_at));
    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(!nrf9151_mqtt_session.connected || nrf9151_mqtt_session.fd < 0) {
        nrf9151_manager_log_append(log, log_len, "MQTT is not connected");
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "AT#XMQTTSUB=\"%s\",%d", topic_at,
             qos >= 0 && qos <= 2 ? qos : 0);
    rc = nrf9151_manager_run_cmd_logged(
        nrf9151_mqtt_session.fd, cmd, K230_NRF9151_CMD_TIMEOUT_US, NULL,
        log, log_len, cancel_cb, cancel_user);
    if(rc == 0) {
        int urc_rc = nrf9151_manager_read_urc(
            nrf9151_mqtt_session.fd, more, sizeof(more), 10000000ULL,
            cancel_cb, cancel_user);

        if(urc_rc == -2) {
            rc = -2;
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
        if(rc == 0 && strstr(more, "#XMQTTEVT: 7,0") == NULL) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT SUBACK not observed");
            rc = -1;
        }
    }
    if(rc == 0) {
        snprintf(nrf9151_mqtt_session.topic,
                 sizeof(nrf9151_mqtt_session.topic), "%s", topic);
        nrf9151_mqtt_session.qos = qos >= 0 && qos <= 2 ? qos : 0;
    }
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_mqtt_session_unsubscribe(const char *topic,
                                          char *log, size_t log_len,
                                          k230_nrf9151_cancel_cb_t cancel_cb,
                                          void *cancel_user)
{
    char topic_at[180];
    char cmd[256];
    char more[2048] = "";
    int rc;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(!topic || !topic[0]) {
        nrf9151_manager_log_append(log, log_len, "Topic is empty");
        return -1;
    }
    nrf9151_manager_escape_at_string(topic, topic_at, sizeof(topic_at));
    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(!nrf9151_mqtt_session.connected || nrf9151_mqtt_session.fd < 0) {
        nrf9151_manager_log_append(log, log_len, "MQTT is not connected");
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "AT#XMQTTUNSUB=\"%s\"", topic_at);
    rc = nrf9151_manager_run_cmd_logged(
        nrf9151_mqtt_session.fd, cmd, K230_NRF9151_CMD_TIMEOUT_US, NULL,
        log, log_len, cancel_cb, cancel_user);
    if(rc == 0) {
        int urc_rc = nrf9151_manager_read_urc(
            nrf9151_mqtt_session.fd, more, sizeof(more), 10000000ULL,
            cancel_cb, cancel_user);

        if(urc_rc == -2) {
            rc = -2;
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
        if(rc == 0 && strstr(more, "#XMQTTEVT:") != NULL &&
           strstr(more, ",0") == NULL) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT UNSUBACK reports an error");
            rc = -1;
        }
    }
    if(rc == 0 && strcmp(nrf9151_mqtt_session.topic, topic) == 0) {
        nrf9151_mqtt_session.topic[0] = '\0';
    }
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_mqtt_session_publish(const char *topic, const char *payload,
                                       int qos, int retain,
                                       char *log, size_t log_len,
                                       k230_nrf9151_cancel_cb_t cancel_cb,
                                       void *cancel_user)
{
    char topic_at[180];
    char payload_at[384];
    char cmd[768];
    char more[2048] = "";
    int rc;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(!topic || !topic[0]) {
        nrf9151_manager_log_append(log, log_len, "Topic is empty");
        return -1;
    }
    nrf9151_manager_escape_at_string(topic, topic_at, sizeof(topic_at));
    nrf9151_manager_escape_at_string(payload ? payload : "", payload_at,
                                     sizeof(payload_at));
    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(!nrf9151_mqtt_session.connected || nrf9151_mqtt_session.fd < 0) {
        nrf9151_manager_log_append(log, log_len, "MQTT is not connected");
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "AT#XMQTTPUB=\"%s\",\"%s\",%d,%d",
             topic_at, payload_at, qos >= 0 && qos <= 2 ? qos : 0,
             retain ? 1 : 0);
    rc = nrf9151_manager_run_cmd_logged(
        nrf9151_mqtt_session.fd, cmd, K230_NRF9151_CMD_TIMEOUT_US, NULL,
        log, log_len, cancel_cb, cancel_user);
    if(rc == 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "MQTT publish command accepted");
        (void)nrf9151_manager_read_urc(nrf9151_mqtt_session.fd, more,
                                       sizeof(more), 2500000ULL, cancel_cb,
                                       cancel_user);
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
    }
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_mqtt_session_poll(char *log, size_t log_len,
                                    unsigned int timeout_ms,
                                    k230_nrf9151_cancel_cb_t cancel_cb,
                                    void *cancel_user)
{
    char more[2048] = "";
    int rc;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    pthread_mutex_lock(&nrf9151_mqtt_lock);
    if(!nrf9151_mqtt_session.connected || nrf9151_mqtt_session.fd < 0) {
        pthread_mutex_unlock(&nrf9151_mqtt_lock);
        return -1;
    }
    rc = nrf9151_manager_read_urc(
        nrf9151_mqtt_session.fd, more, sizeof(more),
        (uint64_t)(timeout_ms ? timeout_ms : 250U) * 1000ULL,
        cancel_cb, cancel_user);
    if(more[0]) {
        nrf9151_manager_trim_text(more);
        nrf9151_manager_log_append(log, log_len, "%s", more);
    }
    pthread_mutex_unlock(&nrf9151_mqtt_lock);
    return rc == -2 ? -2 : (more[0] ? 1 : 0);
}

int k230_nrf9151_mqtt_test_ex(const k230_nrf9151_mqtt_request_t *request,
                              char *log, size_t log_len,
                              k230_nrf9151_cancel_cb_t cancel_cb,
                              void *cancel_user)
{
    char broker[160];
    char client_id[96];
    char username[128];
    char password[128];
    char topic[160];
    char payload[256];
    char cmd[768];
    char more[4096] = "";
    const char *broker_candidates[5];
    int broker_count = 0;
    int broker_index;
    int connected = 0;
    int suback_seen = 0;
    int loopback_seen = 0;
    int lock_fd = -1;
    int fd = -1;
    int rc;
    int port;
    int op = 1;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(!request || !request->broker || !request->broker[0]) {
        nrf9151_manager_log_append(log, log_len, "Broker is empty");
        return -1;
    }
    if(request->auth == K230_NRF9151_MQTT_AUTH_MTLS) {
        nrf9151_manager_log_append(log, log_len,
                                   "mTLS requires credentials preloaded in modem sec_tag");
    }
    if((request->auth == K230_NRF9151_MQTT_AUTH_TLS_USER_PASS ||
        request->auth == K230_NRF9151_MQTT_AUTH_MTLS) &&
       request->sec_tag < 0) {
        nrf9151_manager_log_append(log, log_len,
                                   "TLS MQTT needs sec_tag with modem credentials");
        return -2;
    }
    port = request->port > 0 ? request->port : 1883;
    nrf9151_manager_escape_at_string(request->client_id &&
                                     request->client_id[0] ?
                                     request->client_id : "k230-nrf9151",
                                     client_id, sizeof(client_id));
    nrf9151_manager_escape_at_string(request->username ? request->username : "",
                                     username, sizeof(username));
    nrf9151_manager_escape_at_string(request->password ? request->password : "",
                                     password, sizeof(password));
    nrf9151_manager_escape_at_string(request->topic && request->topic[0] ?
                                     request->topic : "k230/test",
                                     topic, sizeof(topic));
    nrf9151_manager_escape_at_string(request->payload ? request->payload : "",
                                     payload, sizeof(payload));

    broker_candidates[broker_count++] = request->broker;
    if(request->auth == K230_NRF9151_MQTT_AUTH_NONE && port == 1883 &&
       (strcmp(request->broker, "test.mosquitto.org") == 0 ||
        strcmp(request->broker, "broker.hivemq.com") == 0 ||
        strcmp(request->broker, "broker.emqx.io") == 0 ||
        strcmp(request->broker, "mqtt.eclipseprojects.io") == 0)) {
        static const char *const public_brokers[] = {
            "broker.hivemq.com",
            "broker.emqx.io",
            "mqtt.eclipseprojects.io",
            "test.mosquitto.org",
        };

        for(size_t i = 0; i < sizeof(public_brokers) / sizeof(public_brokers[0]);
            i++) {
            int exists = 0;

            for(int j = 0; j < broker_count; j++) {
                if(strcmp(broker_candidates[j], public_brokers[i]) == 0) {
                    exists = 1;
                    break;
                }
            }
            if(!exists && broker_count < (int)(sizeof(broker_candidates) /
                                               sizeof(broker_candidates[0]))) {
                broker_candidates[broker_count++] = public_brokers[i];
            }
        }
    }

    if(nrf9151_manager_open_session("nrf9151-mqtt", 8000, &lock_fd, &fd,
                                    log, log_len) != 0) {
        return -1;
    }
    (void)nrf9151_manager_run_cmd_logged(fd, "AT", K230_NRF9151_CMD_TIMEOUT_US,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    nrf9151_manager_quiesce_gnss_output(fd, log, log_len);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT+CFUN=1",
                                         K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    snprintf(cmd, sizeof(cmd), "AT#XMQTTCFG=\"%s\",60,1", client_id);
    rc = nrf9151_manager_run_cmd_logged(fd, cmd, K230_NRF9151_CMD_TIMEOUT_US,
                                        NULL, log, log_len, cancel_cb,
                                        cancel_user);
    for(broker_index = 0; rc == 0 && broker_index < broker_count;
        broker_index++) {
        nrf9151_manager_escape_at_string(broker_candidates[broker_index],
                                         broker, sizeof(broker));
        nrf9151_manager_log_append(log, log_len, "MQTT broker try: %s:%d",
                                   broker_candidates[broker_index], port);
        if(request->auth == K230_NRF9151_MQTT_AUTH_TLS_USER_PASS ||
           request->auth == K230_NRF9151_MQTT_AUTH_MTLS) {
            snprintf(cmd, sizeof(cmd),
                     "AT#XMQTTCON=%d,\"%s\",\"%s\",\"%s\",%d,%d",
                     op, username, password, broker, port, request->sec_tag);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "AT#XMQTTCON=%d,\"%s\",\"%s\",\"%s\",%d",
                     op,
                     request->auth == K230_NRF9151_MQTT_AUTH_USER_PASS ?
                     username : "",
                     request->auth == K230_NRF9151_MQTT_AUTH_USER_PASS ?
                     password : "",
                     broker, port);
        }
        rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                            K230_NRF9151_LONG_TIMEOUT_US, NULL,
                                            log, log_len, cancel_cb,
                                            cancel_user);
        if(rc != 0) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT connect command failed");
            rc = 0;
            (void)nrf9151_manager_run_cmd_logged(
                fd, "AT#XMQTTCON=0", K230_NRF9151_CMD_TIMEOUT_US, NULL,
                log, log_len, NULL, NULL);
            nrf9151_manager_drain_uart(fd, 120000ULL, 1000000ULL);
            continue;
        }
        more[0] = '\0';
        {
            int urc_rc = nrf9151_manager_read_urc(
                fd, more, sizeof(more), K230_NRF9151_MQTT_TIMEOUT_US,
                cancel_cb, cancel_user);

            if(urc_rc == -2) {
                rc = -2;
                break;
            }
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
        if(strstr(more, "#XMQTTEVT: 0,0") != NULL) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT active broker: %s:%d",
                                       broker_candidates[broker_index], port);
            connected = 1;
            break;
        }
        nrf9151_manager_log_append(log, log_len,
                                   "MQTT CONNACK not observed");
        (void)nrf9151_manager_run_cmd_logged(
            fd, "AT#XMQTTCON=0", K230_NRF9151_CMD_TIMEOUT_US, NULL,
            log, log_len, NULL, NULL);
        nrf9151_manager_drain_uart(fd, 120000ULL, 1000000ULL);
    }
    if(rc == 0 && !connected) {
        rc = -1;
    }
    if(rc == 0 && topic[0]) {
        snprintf(cmd, sizeof(cmd), "AT#XMQTTSUB=\"%s\",%d", topic,
                 request->qos >= 0 && request->qos <= 2 ? request->qos : 0);
        rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                            K230_NRF9151_CMD_TIMEOUT_US,
                                            NULL, log, log_len, cancel_cb,
                                            cancel_user);
        if(rc != 0) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT subscribe failed");
        }
    }
    if(rc == 0 && topic[0]) {
        more[0] = '\0';
        {
            int urc_rc = nrf9151_manager_read_urc(
                fd, more, sizeof(more), 10000000ULL, cancel_cb, cancel_user);

            if(urc_rc == -2) {
                rc = -2;
            }
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
        suback_seen = strstr(more, "#XMQTTEVT: 7,0") != NULL;
        if(rc == 0 && !suback_seen) {
            char extra[2048] = "";

            (void)nrf9151_manager_read_urc(fd, extra, sizeof(extra),
                                           6000000ULL, cancel_cb, cancel_user);
            if(extra[0]) {
                nrf9151_manager_trim_text(extra);
                nrf9151_manager_log_append(log, log_len, "%s", extra);
                suback_seen = strstr(extra, "#XMQTTEVT: 7,0") != NULL;
            }
        }
        if(rc == 0 && !suback_seen) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT SUBACK not observed");
            rc = -1;
        }
    }
    if(rc == 0 && payload[0]) {
        snprintf(cmd, sizeof(cmd), "AT#XMQTTPUB=\"%s\",\"%s\",%d,%d", topic,
                 payload,
                 request->qos >= 0 && request->qos <= 2 ? request->qos : 0,
                 request->retain ? 1 : 0);
        rc = nrf9151_manager_run_cmd_logged(fd, cmd,
                                            K230_NRF9151_CMD_TIMEOUT_US,
                                            NULL, log, log_len, cancel_cb,
                                            cancel_user);
        if(rc != 0) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT publish command failed");
        } else {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT publish command accepted");
        }
        more[0] = '\0';
        if(rc == 0) {
            int wait_rc;

            wait_rc = nrf9151_manager_read_urc(fd, more, sizeof(more),
                                               12000000ULL, cancel_cb,
                                               cancel_user);
            if(wait_rc == -2) {
                rc = -2;
            }
            if(more[0]) {
                nrf9151_manager_trim_text(more);
                nrf9151_manager_log_append(log, log_len, "%s", more);
            }
            loopback_seen = strstr(more, "#XMQTTMSG:") != NULL ||
                            strstr(more, "#XMQTTEVT: 2,0") != NULL;
        }
        if(rc == 0 && !loopback_seen) {
            char extra[2048] = "";

            (void)nrf9151_manager_read_urc(fd, extra, sizeof(extra),
                                           12000000ULL, cancel_cb,
                                           cancel_user);
            if(extra[0]) {
                nrf9151_manager_trim_text(extra);
                nrf9151_manager_log_append(log, log_len, "%s", extra);
                loopback_seen = strstr(extra, "#XMQTTMSG:") != NULL ||
                                strstr(extra, "#XMQTTEVT: 2,0") != NULL;
            }
        }
        if(rc == 0 && !loopback_seen) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT loopback message not observed");
            rc = -1;
        }
    }
    (void)nrf9151_manager_run_cmd_logged(fd, "AT#XMQTTCON=0",
                                         K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                         log, log_len, NULL, NULL);
    nrf9151_manager_close_session(lock_fd, fd);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_mqtt_test(const k230_nrf9151_mqtt_request_t *request,
                           char *log, size_t log_len)
{
    return k230_nrf9151_mqtt_test_ex(request, log, log_len, NULL, NULL);
}
