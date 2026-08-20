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
#define K230_NRF9151_HTTP_TIMEOUT_US 30000000ULL
#define K230_NRF9151_MQTT_TIMEOUT_US 30000000ULL
#define K230_NRF9151_GNSS_RESTART_US (120ULL * 1000000ULL)

typedef struct {
    int fd;
    int lock_fd;
    int stop;
    int active;
    int configured;
    int first_fix_reported;
    uint64_t session_start_us;
    uint64_t last_fix_us;
    uint64_t last_nmea_us;
    uint64_t last_restart_us;
    char line[256];
    size_t line_used;
} k230_nrf9151_gnss_monitor_t;

static pthread_mutex_t nrf9151_manager_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t nrf9151_gpio_lock = PTHREAD_MUTEX_INITIALIZER;
static k230_nrf9151_status_t nrf9151_status_cache;
static int nrf9151_status_cache_loaded;
static k230_nrf9151_gnss_monitor_t nrf9151_gnss_monitor = {
    .fd = -1,
    .lock_fd = -1,
};
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

static int nrf9151_manager_exchange(int fd, const char *cmd, char *resp,
                                    size_t resp_len, uint64_t timeout_us,
                                    k230_nrf9151_cancel_cb_t cancel_cb,
                                    void *cancel_user)
{
    uint64_t start;
    size_t used = 0;
    size_t cmd_len;

    if(resp && resp_len > 0U) {
        resp[0] = '\0';
    }
    if(fd < 0 || !cmd) {
        errno = EINVAL;
        return -1;
    }
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
        if(resp && resp_len > 0U && used + 1U < resp_len) {
            size_t copy = (size_t)rd;

            if(copy > resp_len - used - 1U) {
                copy = resp_len - used - 1U;
            }
            memcpy(resp + used, buf, copy);
            used += copy;
            resp[used] = '\0';
        }
        if(resp &&
           (nrf9151_manager_response_has_token(resp, "OK") ||
            nrf9151_manager_response_has_token(resp, "ERROR"))) {
            return nrf9151_manager_response_has_token(resp, "OK") ? 0 : 1;
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
        if(resp && (strstr(resp, "#XHTTPCSTAT:") ||
                    strstr(resp, "#XMQTTEVT: 0,0") ||
                    strstr(resp, "#XMQTTEVT: 1,0") ||
                    strstr(resp, "#XMQTTEVT: 7,0") ||
                    strstr(resp, "#XMQTTEVT: 3,0") ||
                    strstr(resp, "#XMQTTEVT: 2,0") ||
                    strstr(resp, "#XDATAMODE: 0"))) {
            start = nrf9151_manager_monotonic_us();
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

int k230_nrf9151_run_lte_check_ex(k230_nrf9151_status_t *status,
                                  char *log, size_t log_len,
                                  k230_nrf9151_cancel_cb_t cancel_cb,
                                  void *cancel_user)
{
    static const char *const cmds[] = {
        "AT",
        "AT+CMEE=1",
        "ATI",
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
        "AT#XPING=\"223.5.5.5\",32,5000,3,1000",
    };
    k230_nrf9151_status_t local;
    int lock_fd = -1;
    int fd = -1;
    int failures = 0;

    if(log && log_len > 0U) {
        log[0] = '\0';
    }
    if(k230_nrf9151_read_status(&local, 0) != 0) {
        k230_nrf9151_status_init(&local);
    }
    local.present = k230_nrf9151_uart_present();
    local.epoch = time(NULL);
    snprintf(local.lte_status, sizeof(local.lte_status), "%s", "Checking");
    snprintf(local.modem_state, sizeof(local.modem_state), "%s",
             local.present ? "present" : "missing");
    k230_nrf9151_write_status(&local);

    if(nrf9151_manager_open_session("nrf9151-lte", 8000, &lock_fd, &fd,
                                    log, log_len) != 0) {
        failures++;
        snprintf(local.modem_state, sizeof(local.modem_state), "%s",
                 "missing");
        snprintf(local.lte_status, sizeof(local.lte_status), "%s",
                 "UART unavailable");
        snprintf(local.last_error, sizeof(local.last_error), "%s",
                 strerror(errno));
        k230_nrf9151_write_status(&local);
        if(status) {
            *status = local;
        }
        return -1;
    }

    for(size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        uint64_t timeout = nrf9151_manager_starts_with(cmds[i], "AT#XPING") ?
                           K230_NRF9151_LONG_TIMEOUT_US :
                           K230_NRF9151_CMD_TIMEOUT_US;
        int rc = nrf9151_manager_run_cmd_logged(fd, cmds[i], timeout,
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
        if(nrf9151_manager_starts_with(cmds[i], "AT#XPING")) {
            char more[2048] = "";
            int urc_rc;

            urc_rc = nrf9151_manager_read_urc(fd, more, sizeof(more),
                                              8000000ULL, cancel_cb,
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
                nrf9151_manager_update_lte_from_response(&local, cmds[i],
                                                         more, 0);
            }
        }
        local.epoch = time(NULL);
        k230_nrf9151_write_status(&local);
        usleep(100000);
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
    local.epoch = time(NULL);
    k230_nrf9151_write_status(&local);
    nrf9151_manager_close_session(lock_fd, fd);
    if(status) {
        *status = local;
    }
    return failures == 0 ? 0 : 1;
}

int k230_nrf9151_run_lte_check(k230_nrf9151_status_t *status,
                               char *log, size_t log_len)
{
    return k230_nrf9151_run_lte_check_ex(status, log, log_len, NULL, NULL);
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
    type = fields[0] + strlen(fields[0]) - 3U;
    if(strcmp(type, "GGA") == 0) {
        double lat = 0.0;
        double lon = 0.0;
        double alt = 0.0;
        int fix = count > 6 ? atoi(fields[6]) : 0;
        int sats = count > 7 ? atoi(fields[7]) : 0;

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
        }
    } else if(strcmp(type, "GSV") == 0) {
        int sats = count > 3 ? atoi(fields[3]) : 0;

        if(sats >= 0) {
            status->satellites = (unsigned int)sats;
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

static int nrf9151_manager_start_gnss_locked(int fd)
{
    char resp[1024];
    int rc;
    k230_nrf9151_status_t status;

    if(k230_nrf9151_read_status(&status, 0) != 0) {
        k230_nrf9151_status_init(&status);
    }
    rc = nrf9151_manager_exchange(fd, "AT#XGNSS?", resp, sizeof(resp),
                                  K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL);
    if(rc == 0 && nrf9151_manager_response_gnss_active(resp) > 0) {
        (void)nrf9151_manager_exchange(fd, "AT#XNMEA=1", resp, sizeof(resp),
                                       K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                       NULL);
    } else {
        (void)nrf9151_manager_exchange(fd, "AT+CFUN=31", resp, sizeof(resp),
                                       K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                       NULL, NULL);
        (void)nrf9151_manager_exchange(fd, "AT#XNMEA=1", resp, sizeof(resp),
                                       K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                       NULL);
        rc = nrf9151_manager_exchange(fd, "AT#XGNSS=1,0,0,0", resp,
                                      sizeof(resp),
                                      K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                      NULL);
        if(rc != 0) {
            char status_resp[1024];

            if(nrf9151_manager_exchange(fd, "AT#XGNSS?", status_resp,
                                        sizeof(status_resp),
                                        K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                        NULL) != 0 ||
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
    }
    nrf9151_gnss_monitor.configured = 1;
    nrf9151_gnss_monitor.session_start_us = nrf9151_manager_monotonic_us();
    nrf9151_gnss_monitor.first_fix_reported = 0;
    status.present = 1;
    status.link_ok = 1;
    status.gnss_running = 1;
    status.gnss_has_fix = 0;
    status.nmea_rx_count = 0;
    status.nmea_valid_count = 0;
    status.nmea_nofix_count = 0;
    status.satellites = 0;
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

    if(nrf9151_manager_open_session("nrf9151-gnss", 8000, &lock_fd, &fd,
                                    NULL, 0) != 0) {
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_manager_status_set_error_locked("GNSS UART unavailable");
        nrf9151_gnss_monitor.active = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return NULL;
    }
    nrf9151_gnss_monitor.fd = fd;
    nrf9151_gnss_monitor.lock_fd = lock_fd;
    if(nrf9151_manager_exchange(fd, "AT", NULL, 0,
                                K230_NRF9151_CMD_TIMEOUT_US, NULL, NULL) != 0 ||
       nrf9151_manager_start_gnss_locked(fd) != 0) {
        nrf9151_manager_close_session(lock_fd, fd);
        pthread_mutex_lock(&nrf9151_manager_lock);
        nrf9151_gnss_monitor.fd = -1;
        nrf9151_gnss_monitor.lock_fd = -1;
        nrf9151_gnss_monitor.active = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return NULL;
    }

    while(!nrf9151_gnss_monitor.stop) {
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
           nrf9151_gnss_monitor.last_fix_us == 0ULL &&
           nrf9151_gnss_monitor.session_start_us > 0ULL &&
           now_us - nrf9151_gnss_monitor.session_start_us >
           K230_NRF9151_GNSS_RESTART_US &&
           (nrf9151_gnss_monitor.last_restart_us == 0ULL ||
            now_us - nrf9151_gnss_monitor.last_restart_us >
            K230_NRF9151_GNSS_RESTART_US)) {
            char resp[512];

            nrf9151_gnss_monitor.last_restart_us = now_us;
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
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return NULL;
}

int k230_nrf9151_start_gnss_monitor(void)
{
    pthread_t thread;

    pthread_mutex_lock(&nrf9151_manager_lock);
    if(nrf9151_gnss_monitor.active) {
        nrf9151_gnss_monitor.stop = 0;
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return 0;
    }
    nrf9151_gnss_monitor.stop = 0;
    pthread_mutex_unlock(&nrf9151_manager_lock);

    if(pthread_create(&thread, NULL, nrf9151_manager_gnss_thread, NULL) != 0) {
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

int k230_nrf9151_stop_gnss_monitor(void)
{
    pthread_mutex_lock(&nrf9151_manager_lock);
    if(!nrf9151_gnss_monitor.active) {
        pthread_mutex_unlock(&nrf9151_manager_lock);
        return 0;
    }
    nrf9151_gnss_monitor.stop = 1;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return 0;
}

int k230_nrf9151_gnss_monitor_active(void)
{
    int active;

    pthread_mutex_lock(&nrf9151_manager_lock);
    active = nrf9151_gnss_monitor.active;
    pthread_mutex_unlock(&nrf9151_manager_lock);
    return active;
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
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
    }
    snprintf(cmd, sizeof(cmd), "AT#XCLOSE=%d", handle);
    (void)nrf9151_manager_run_cmd_logged(fd, cmd,
                                         K230_NRF9151_CMD_TIMEOUT_US, NULL,
                                         log, log_len, NULL, NULL);
    nrf9151_manager_close_session(lock_fd, fd);
    return rc == 0 ? 0 : (rc == -2 ? -2 : -1);
}

int k230_nrf9151_http_request(const k230_nrf9151_http_request_t *request,
                              char *log, size_t log_len)
{
    return k230_nrf9151_http_request_ex(request, log, log_len, NULL, NULL);
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
    nrf9151_manager_escape_at_string(request->payload ? request->payload : "",
                                     payload, sizeof(payload));

    if(nrf9151_manager_open_session("nrf9151-mqtt", 8000, &lock_fd, &fd,
                                    log, log_len) != 0) {
        return -1;
    }
    (void)nrf9151_manager_run_cmd_logged(fd, "AT", K230_NRF9151_CMD_TIMEOUT_US,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    (void)nrf9151_manager_run_cmd_logged(fd, "AT+CFUN=1",
                                         K230_NRF9151_CMD_TIMEOUT_US * 3ULL,
                                         NULL, log, log_len, cancel_cb,
                                         cancel_user);
    snprintf(cmd, sizeof(cmd), "AT#XMQTTCFG=\"%s\",60,1", client_id);
    rc = nrf9151_manager_run_cmd_logged(fd, cmd, K230_NRF9151_CMD_TIMEOUT_US,
                                        NULL, log, log_len, cancel_cb,
                                        cancel_user);
    if(rc == 0) {
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
    }
    if(rc == 0) {
        int urc_rc = nrf9151_manager_read_urc(fd, more, sizeof(more),
                                              K230_NRF9151_MQTT_TIMEOUT_US,
                                              cancel_cb, cancel_user);

        if(urc_rc == -2) {
            rc = -2;
        }
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
        }
        if(strstr(more, "#XMQTTEVT: 0,0") == NULL) {
            nrf9151_manager_log_append(log, log_len,
                                       "MQTT CONNACK not observed");
        }
    }
    if(rc == 0 && topic[0]) {
        snprintf(cmd, sizeof(cmd), "AT#XMQTTSUB=\"%s\",%d", topic,
                 request->qos >= 0 && request->qos <= 2 ? request->qos : 0);
        (void)nrf9151_manager_run_cmd_logged(fd, cmd,
                                             K230_NRF9151_CMD_TIMEOUT_US,
                                             NULL, log, log_len, cancel_cb,
                                             cancel_user);
    }
    if(rc == 0 && payload[0]) {
        snprintf(cmd, sizeof(cmd), "AT#XMQTTPUB=\"%s\",\"%s\",%d,%d", topic,
                 payload,
                 request->qos >= 0 && request->qos <= 2 ? request->qos : 0,
                 request->retain ? 1 : 0);
        (void)nrf9151_manager_run_cmd_logged(fd, cmd,
                                             K230_NRF9151_CMD_TIMEOUT_US,
                                             NULL, log, log_len, cancel_cb,
                                             cancel_user);
        more[0] = '\0';
        (void)nrf9151_manager_read_urc(fd, more, sizeof(more), 5000000ULL,
                                       cancel_cb, cancel_user);
        if(more[0]) {
            nrf9151_manager_trim_text(more);
            nrf9151_manager_log_append(log, log_len, "%s", more);
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
